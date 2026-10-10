/*
 * race.c - the race itself: the track, the grid, the racers and the
 * camera that watches them.
 *
 * The subsystems keep their own files - level.c the manifest, trk.c
 * the geometry, grid.c the starting slots, racer.c the racers,
 * horizon.c the sky. What lives here is the ORCHESTRATION, plus the
 * chase camera and the race-level gameplay CLAUDE.md rule 13 assigns
 * to this file.
 *
 * ONE RACE AT A TIME, so the state is file-scope rather than a struct
 * passed around. The original does the same.
 */

#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <math.h>

#include "race.h"
#include "hud.h"
#include "rand.h"

/* Downstream of the menu's selections - g_selectedTrack,
 * g_selectedBike, g_soloMode, g_selectedMode, g_reverseEnabled. */
#include "menu.h"
#include "options.h"
#include "level.h"
#include "trk.h"
#include "trig.h"
#include "detail.h"
#include "grid.h"
#include "autopilot.h"
#include "racer.h"
#include "cull.h"
#include "horizon.h"
#include "objsnd.h"
#include "bikes.h"
#include "physics.h"
#include "tracks.h"
#include "bank.h"
#include "mesh.h"
#include "skl.h"
#include "model.h"
#include "prim.h"
#include "queue.h"
#include "render.h"
#include "drawscene.h"
#include "transform.h"
#include "racerdraw.h"
#include "fx.h"
#include "framebuffer.h"
#include "assets.h"
#include "netplay.h"
#include "config.h"
#include "log.h"
#include "agent.h"
#include "audio.h"      /* the post-race cue */
#include "timing.h"

/* Set once the track and grid are up; every draw path checks it. */
static int s_race_ok;
static int s_race_viewLogged;

/* 0 = draw everything; see the [RENDER] drawdistance note below. */
static int   s_race_drawAll;
static float s_race_backPlane;

/*
 * THE AUTOPILOT'S DEFAULT SPEED, AND NOT A CONSTANT OF THE GAME: one
 * instant of MOTO.EXE accelerating on one straight, kept only so the
 * scaffold moves at a realistic pace. Do not cite it as the game's
 * speed - speed is not constant and this is not its maximum.
 */


/* A segment index plus the fraction travelled through it. */
static float        s_race_segPos;

/*
 * NO-WALLS - ride through the guard rails. OFF at load, and it matches
 * what tools/mr_cheats.py pokes into MOTO.EXE so the two can be put in
 * the same state for a comparison.
 *
 * IT SURVIVES A RESTART AND A TRACK CHANGE. race_walls_toggle is the
 * only writer and race_load does not clear it - deliberate, because
 * MOTO's side is a code patch that survives the same way, but it means
 * a session can carry it into a track it was not meant for. The log
 * line on every toggle is the only warning.
 */
static int          s_race_noWalls;


static float        s_race_lateral;

/* THE DRIVETRAIN DRIVES THE PLAYER, and it is the only path. */
static MR_Drivetrain s_race_drive;
static MR_Gearbox    s_race_gearbox;

/*
 * WORLD UNITS PER METRE - the drivetrain works in m/s and the track in
 * world units. READ, NOT FITTED: 1.28 is the DOUBLE at 0x004d1010 and
 * the original's integrator at 0x0046bc8a scales it to 128 units a
 * second. Derivation and the two travel checks in
 * port-divergences.txt 2.5; racer.c carries the same constant as
 * AI_STEP_SCALE.
 *
 * The ini knob is an experiment override, not a calibration.
 */
#define RACE_UNITS_PER_METRE_DEFAULT 128.0f
static float s_race_unitsPerMetre = RACE_UNITS_PER_METRE_DEFAULT;
static double       s_race_lastTime;   /* tm_now() last tick */
static float        s_race_yawOrient;
static float        s_race_yawFrame;

/* camera+0xc0 - see camera_view_yaw. Written by the chase camera
 * each frame, read by the particle billboard. */
static float        s_race_camYaw;
/*
 * THE CAMERA, PUBLISHED FOR THE HUD. The original keeps a camera
 * RECORD and Hud_DrawRacerCallouts reads g_abCameraRecords[i]'s
 * pViewMatrix and pEye out of it; ours builds both as locals in
 * race_draw_3d, so the HUD - which runs later - needs them kept.
 * Same reason s_race_camYaw above exists.
 */
static float        s_race_camView[12];
static float        s_race_camEyePos[3];
static float        s_race_leanDelta;
/*
 * THE ENGINE'S ANGLE UNIT, 0x4000 to the turn - trig-table.txt. A
 * heading is an INDEX into g_cosTable, not radians. The conversion
 * pair is TRIG_IDX_PER_RAD / TRIG_RAD_PER_IDX in trig.h, which is
 * where the binary's own constant lives.
 */

/*
 * Radians to the engine's 14-bit index, 0x0045cbb4.
 *
 * __ftol TRUNCATES TOWARD ZERO and does not round. Reproducing the
 * quantisation is the whole job here; a lroundf would quietly disagree
 * with the original by one index on half the angles.
 */
static int race_ang_from_rad(float rad)
{
	return (int)((double)rad * TRIG_IDX_PER_RAD);   /* __ftol */
}


static float        s_race_yawWorld;   /* the bike's heading */
/*
 * The same heading as a 14-bit index, the only form the original
 * stores. THIS is what directions are built from; the float above is
 * kept for readability and is not authoritative.
 */
static int          s_race_yawIndex;
static float        s_race_prevLongSpeed;

/*
 * NON-ZERO WHILE race_autopilot_probe IS SIMULATING. The only thing
 * in the tick path that reads it is the wheel-FX call, which must
 * not spray particles for a future that is about to be thrown away.
 * See race_autopilot_probe.
 */
static int          s_race_dryRun;

/* Defined below, used by race_build_grid. */
static void race_load_bike(void);
static void race_ghost_reset(void);
static void race_ghost_load(void);
static void race_ghost_load_race(void);
static void race_load_wheels(void);
static int  race_camera(int advance, float eye[3], float viewEye[3],
                        float target[3], float *lens);
/*
 * raceState, racer +0x2de5. 0 AND 1 BOTH MEAN STILL RACING: 0 is "has
 * not crossed the line yet", and the first crossing turns it into 1
 * rather than counting as a lap.
 */
#define RACESTATE_RACING_0  0
#define RACESTATE_RACING_1  1
#define RACESTATE_FINISHED  2
#define RACESTATE_TIMED_OUT 3

/* The sub-millisecond remainder of the frame step - see the
 * quantisation in race_update. */
static float s_race_msCarry = 0.0f;

/* The trackside director's six shots, and the replay's two modes. */
#define CAM_SHOT_FIXED       0   /* a placed trackside camera        */
#define CAM_SHOT_CHASE       1   /* the ordinary chase, mode 7       */
#define CAM_SHOT_ORBIT       2   /* orbit, yaw running               */
#define CAM_SHOT_ORBIT_PI    3   /* orbit, yaw pinned                */
#define CAM_SHOT_FIXED_ZOOM  4   /* placed, framed by zoom           */
#define CAM_SHOT_LOOKSLOT    5   /* a bike-local placement           */

#define REPLAY_MODE_LIVE  0
#define REPLAY_MODE_PLAY  1

/* The twenty track pages start here; the slots below belong to the
 * menu and the glass. The bike takes the two after them - see
 * RACE_BIKE_BANK - and the sky the one after those. */
#define RACE_TRK_BANK_BASE 8
#define RACE_HORIZON_BANK (RACE_TRK_BANK_BASE + LEVEL_BANKS + 2)

/* The track manifest - the land name, the audio track, LenBPlane. */
static MR_Level s_race_level;

/* g_nRaceLaps 0x004dc0c8 - see race_load. */
static int      s_race_laps;
/*
 * g_nRaceOverNet in the original, raised by a peer sending 0x6c.
 * A solo port had no way to reach it, so it never existed here.
 */
static int      s_race_overNet;
static int s_race_inputFocus = 1;   /* the window has the keyboard */
static int s_race_inputUnfocused;   /* [DEBUG] inputunfocused      */


static void race_net_drain_slot(int slot);
static void race_net_reset(void);
static void race_net_apply_state(const MR_NetMsgState *st);
static void race_net_check_over(void);
/*
 * WHICH SLOT THIS MACHINE DRIVES. One in a solo game; in a net
 * game the peer id the relay assigned. g_localRacerIndex
 * (0x004ea4c0) in the original, which exists precisely because a
 * network player is NOT always slot 1 - every indexing of the
 * array by a literal 1 here used to assume it was.
 */
static int      s_race_localIndex = 1;
/* Segments with a non-negative linkA - the original's [0x004e9844]. */
static int      s_race_cpSegCount;

/* Defined with the replay state it belongs to. */
static void     race_seed_sim(void);
static MR_Trk   s_race_trk;

/*
 * The visible segment list - 0x005dabd8 and 0x005dabcc. Rebuilt every
 * frame in race_draw_3d.
 *
 * THE ORIGINAL HAS A STALE-COUNT HAZARD AND WE DO NOT REPRODUCE IT: it
 * writes the count only when the zone call runs but draws racers
 * unconditionally, so a frame that skips the call filters against last
 * frame's count over an unwritten buffer. Ours is built and consumed
 * in one block. [open - whether any frame takes that path]
 */
static int      s_race_segList[TRK_SEGMENT_LIST_MAX];
static int      s_race_segCount;
static MR_GridSlot s_race_grid[GRID_MAX_SLOTS];
static int         s_race_standingsLogged;

/* The chase camera's state, carried across frames for the lerps. */
static MR_Camera s_race_cam;
static MR_Racer    s_race_racers[RACER_MAX];

/* Defined below, next to the clock they belong to. */
static void race_apply_checkpoint_times(void);
static void race_clock_reset(void);
static void race_update_progress(void);

/*
 * WHAT THE HARNESS NEEDS TO PLACE A BIKE.
 *
 * ORIGIN  NONE - ours.
 *
 * tools/mr_place.py needs MR_Racer's stride and a few offsets to reach
 * racer 1. They are EXPORTED rather than transcribed because the
 * struct gains and loses members constantly, and a tool computing them
 * by hand says nothing when a placement lands in the middle of a
 * matrix.
 *
 * NOTHING IN THE PROGRAM READS IT, so /OPT:REF strips it and the
 * /INCLUDE below is the only thing keeping the symbol in the image and
 * the .map. Remove the pragma and mr_place.py reports the port as
 * unplaceable.
 */
/* 'MRPL' - see the sentinel at the end of the array. */
#define MR_RACE_LAYOUT_MAGIC 0x4d52504c

#if defined(_MSC_VER) && defined(_M_IX86)
#pragma comment(linker, "/INCLUDE:_g_race_layout")
#endif
int g_race_layout[7] = {
	(int)sizeof(MR_Racer),
	(int)offsetof(MR_Racer, position),
	(int)offsetof(MR_Racer, moveDir),
	(int)offsetof(MR_Racer, segmentPrev),
	/*
	 * A PLACEMENT THAT SETS NEITHER IS NOT A CONTROLLED START, and
	 * two bikes left in different gears reads as a physics defect -
	 * racers-convergence.txt 9.18.13.
	 *
	 * speed is the first of the three-float run speed, speedPrev,
	 * gearboxRoadSpeed, so a tool writes twelve bytes from here.
	 *
	 * THE GEAR IS NOT IN MR_Racer. It lives in s_race_gearbox and the
	 * tool addresses that separately. racer+0x7c, where the original
	 * keeps it, EXISTS IN OUR STRUCT AND IS DEAD - writing it looks
	 * like it worked and does nothing.
	 */
	(int)offsetof(MR_Racer, speed),
	(int)offsetof(MR_Gearbox, gear),
	/*
	 * A SENTINEL, AND NOT DECORATION. A tool reading 28 bytes from an
	 * OLDER port gets 16 of layout and 12 of whatever follows in
	 * .data; the read succeeds, a length check cannot tell, and
	 * gearOff is legitimately 0 so no single value looks wrong. The
	 * result is a write over racer+0 - humanPlayer among them - that
	 * then VERIFIES AS MATCHED, because the check reads back through
	 * the same wrong offsets.
	 */
	MR_RACE_LAYOUT_MAGIC,
};
static unsigned int s_race_ticks;

/*
 * THE PER-GEAR HANDOVER SPEEDS, m/s, indexed by gear; [0] unused.
 * racer+0x90 is per racer in the original - one table is enough here
 * while only the local racer runs the real gearbox.
 */
static float s_race_shiftTable[GB_SHIFT_TABLE_MAX];

/*
 * THE SIDE CAMERA: 0 off, 1 left, 2 right. Ours - the original has the
 * placements in look slots 2 and 3 and no key that reaches them.
 *
 * DO NOT ROUTE IT THROUGH camera_update. The chase lerps its eye and
 * then re-normalises with the boom, and that lag points BACKWARD along
 * the direction of travel - a sideways offset gets dragged behind the
 * bike as speed rises until the view is looking at the back wheel.
 *
 * SUPERSEDED, AND NOT YET REMOVED. CAM_SIDE_X/Y are slot 2 and 3's
 * offsets, and camera_look_slot now places any slot from the real
 * tables with the measured view matrix. This should become
 * camera_look_slot(c, pos, m, 2 or 3). [open]
 */
static int s_race_camSide;

/* F5's chase slot. Outside MR_Camera because camera_reset memsets
 * the record and the player's choice has to survive a restart. */
static int s_race_camViewSlot;

/* F6's state - DAT_004dc00c. A REQUEST, not the mode. */
static int s_race_camFirstPerson;

/*
 * g_bFirstPersonActive, 0x004dc008 - the request AND the crash test,
 * resolved once per tick by Camera_SelectView and read by BOTH the
 * camera and the draw loop. One flag rather than two copies of the
 * test is what stops those two disagreeing about whether the bike or
 * the cockpit is on screen this frame.
 */
static int s_race_fpActive;

/*
 * nLookBackKey's state, camera+0x21c - HELD, not toggled. Non-zero
 * pre-empts both cameras and suppresses the cockpit meshes.
 */
static int s_race_camLookSlot;

static int         s_race_racerCount;
static int      s_race_collideInPractice;

#define RACE_BIKE_BANK (RACE_TRK_BANK_BASE + LEVEL_BANKS)


static MR_Skeleton   s_race_bikeSkl;
static MR_SklPose    s_race_bikePose;
static MR_Primitive *s_race_bikePrims;
static int           s_race_bikePrimCount;
static int           s_race_bikeOk;

/*
 * THE HAND SKELETON - manifest record 3, data\<bike>\mHand\corps.skl,
 * and the reason racer+0x319d is called handSkeleton.
 *
 * IT IS A SECOND TOPOLOGY, NOT A LEVEL OF DETAIL. The base corps.skl
 * welds the rider's hands into a BIKE member; in mHand they belong to
 * the rider's own bones.
 *
 * WHICH IS WHY A CRASH NEEDS IT - drawn from the base skeleton the
 * hands stay welded to the bars and the arm stretches across the track
 * to the body on the ground. It must be swapped TOGETHER WITH ITS
 * PRIMS, which are built from its own mesh.
 *
 * Optional: a manifest naming no record 3 falls back to the base.
 * mHand is present in all four shipped bike manifests.
 */
static MR_Skeleton   s_race_handSkl;
static MR_Primitive *s_race_handPrims;
static int           s_race_handPrimCount;
static int           s_race_handOk;

/*
 * ORIGIN  NONE - ours, standing in for the original's two articulated
 *         slots.
 *
 * Racer_Create builds normals for both and maybe_Racer_Draw picks with
 * racer_select_skeleton_slot, so the choice is PER RACER PER FRAME and
 * not per level.
 */
static const MR_Skeleton *race_skl_for_slot(int slot)
{
	if (slot == 1 && s_race_handOk)
		return &s_race_handSkl;
	return &s_race_bikeSkl;
}

/* The rider's whole pose set for this bike. racers.txt 9.52. */
static MR_PilotRots  s_race_pilotRots;

#define RACE_WHEELS      2


/*
 * A SECOND scale, applied after Load3dfAndMap's own 96.0 - net 768.
 * maybe_Game_LoadBikeMeshes 0x00413450; bike-assets.txt 2.
 */
#define RACE_COCKPIT_SCALE   8.0f

static MESH          s_race_wheelMesh[RACE_WHEELS];
static MR_Primitive *s_race_wheelPrims[RACE_WHEELS];
static int           s_race_wheelPrimCount[RACE_WHEELS];
static int           s_race_wheelOk[RACE_WHEELS];

static void race_load_wheels(void)
{
	static const int kMesh[RACE_WHEELS] = {
		BIKE_PATH_WHEEL_R, BIKE_PATH_WHEEL_F
	};
	static const int kMap[RACE_WHEELS] = {
		BIKE_PATH_WHEEL_R_MAP, BIKE_PATH_WHEEL_F_MAP
	};
	const MR_BikeCarac *b = bike_at(g_selectedBike);
	int i;

	for (i = 0; i < RACE_WHEELS; i++) {
		const char *mesh = bikes_asset(kMesh[i]);
		const char *map  = bikes_asset(kMap[i]);
		int         n    = 0;

		s_race_wheelOk[i] = 0;
		if (!mesh || !map || !b)
			continue;
		if (!mesh_load_with_map(&s_race_wheelMesh[i], mesh, map,
		                        MESH_SCALE_3DF_MAP)) {
			mrlog_err("bike: wheel %s did not load\n", mesh);
			continue;
		}
		if (!prim_build_from_mesh_bank(&s_race_wheelMesh[i],
		                               &s_race_wheelPrims[i], &n, 0,
		                               RACE_BIKE_BANK) || n <= 0) {
			mesh_free(&s_race_wheelMesh[i]);
			continue;
		}
		s_race_wheelPrimCount[i] = n;

		/*
		 * THE ONLY MESHES IN THE GAME THAT GET VERTEX NORMALS.
		 * maybe_Game_LoadBikeMeshes calls Mesh_ComputeVertexNormals
		 * on the four wheels and nothing else, the loop at
		 * 0x00413646 - and without them mesh_shade_vertices returns
		 * at once, the wheels keep the flat 0x1f their transform
		 * gave them, and they GLOW against a bike that darkens in
		 * a tunnel. See mesh.h.
		 */
		mesh_compute_vertex_normals(&s_race_wheelMesh[i]);

		/* No static offset: racerdraw_racer solves both wheels off the
		 * contacts, the fork and the arm every frame. */
		s_race_wheelOk[i] = 1;
		mrlog("bike: wheel %d %s, %d prims\n", i, mesh, n);
	}
}

/*
 * THE FIRST-PERSON COCKPIT. NEITHER MESH GETS VERTEX NORMALS - only
 * the wheels do - so the bars and the dash are flat-shaded, which is
 * why transform_verts_flat is the right transform for them.
 */
static MESH          s_race_cockpitMesh[COCKPIT_PARTS];
static MR_Primitive *s_race_cockpitPrims[COCKPIT_PARTS];
static int           s_race_cockpitPrimCount[COCKPIT_PARTS];
static MR_CockpitSet s_race_cockpitSet;
static int           s_race_cockpitOk;

/* ORIGIN  maybe_Game_LoadBikeMeshes @ 0x00413450, its
 *         FileGuidon/FileCockpit pair */
static void race_load_cockpit(void)
{
	static const int kMesh[COCKPIT_PARTS] = {
		BIKE_PATH_GUIDON, BIKE_PATH_COCKPIT
	};
	static const int kMap[COCKPIT_PARTS] = {
		BIKE_PATH_GUIDON_MAP, BIKE_PATH_COCKPIT_MAP
	};
	int i, ok = 0;

	memset(&s_race_cockpitSet, 0, sizeof(s_race_cockpitSet));

	for (i = 0; i < COCKPIT_PARTS; i++) {
		const char *mesh = bikes_asset(kMesh[i]);
		const char *map  = bikes_asset(kMap[i]);
		int         n    = 0;

		if (!mesh || !map)
			continue;
		if (!mesh_load_with_map(&s_race_cockpitMesh[i], mesh, map,
		                        MESH_SCALE_3DF_MAP)) {
			mrlog_err("bike: cockpit %s did not load\n", mesh);
			continue;
		}

		/*
		 * AND THEN THE SECOND SCALE, IN THIS ORDER. Passing 8.0 as the
		 * LOADER scale instead of applying it here loads the mesh
		 * twelve times too small, and it still transforms, culls and
		 * queues perfectly - it collapses to a point. Exactly the
		 * silent failure mesh.h warns about.
		 */
		mesh_scale(&s_race_cockpitMesh[i], RACE_COCKPIT_SCALE,
		           RACE_COCKPIT_SCALE, RACE_COCKPIT_SCALE);
		if (!prim_build_from_mesh_bank(&s_race_cockpitMesh[i],
		                               &s_race_cockpitPrims[i], &n, 0,
		                               RACE_TRK_BANK_BASE) || n <= 0) {
			mesh_free(&s_race_cockpitMesh[i]);
			continue;
		}
		/*
		 * THE TRACK BASE, NOT THE BIKE'S. The cockpit is textured from
		 * the TRACK's pages - its .maps name bank 7 where every other
		 * bike mesh names 0 or 1 - and the original keeps one flat
		 * table where the bike's two pages are just entries 0 and 1.
		 * The port splits that table, so rebasing this by the BIKE
		 * base lands on a slot nothing loads.
		 * racer-draw-pipeline.txt 5.4.3.
		 */
		/* The build applied it - prim_build_from_mesh_bank. */
		s_race_cockpitPrimCount[i] = n;

		s_race_cockpitSet.part[i].mesh      = &s_race_cockpitMesh[i];
		s_race_cockpitSet.part[i].prims     = s_race_cockpitPrims[i];
		s_race_cockpitSet.part[i].primCount = n;
		ok++;
		mrlog("bike: cockpit %d %s, %d verts, %d prims\n", i, mesh,
		      s_race_cockpitMesh[i].vertexCount, n);
	}

	/*
	 * BOTH OR NEITHER, as maybe_Game_LoadBikeMeshes does - it returns 0
	 * on the first failure and abandons the set. Half a cockpit reads
	 * as a hole in the bike.
	 */
	s_race_cockpitOk = (ok == COCKPIT_PARTS);
	if (!s_race_cockpitOk)
		mrlog("bike: cockpit unavailable - first person draws nothing\n");
	racerdraw_cockpit_reset();
}


/*
 * THE THREE LOWER BODIES. maybe_Racer_SelectLod indexes g_meshSkl[5]:
 * slot 0 is the articulated corps.skl and slots 2, 3 and 4 are single
 * static meshes.
 *
 * SLOT 1 IS NOT ONE OF THEM and is not loaded here - it is the hand
 * skeleton, which race_load_bike loads and crashLatch selects rather
 * than distance.
 */
static MESH          s_race_lodMesh[RACER_LOD_SLOTS];
static MR_Primitive *s_race_lodPrims[RACER_LOD_SLOTS];
static int           s_race_lodPrimCount[RACER_LOD_SLOTS];
static int           s_race_lodOk[RACER_LOD_SLOTS];
static int           s_race_lodAll;   /* all three, so LOD may run */

static void race_load_lod_bodies(void)
{
	static const int kSlot[3] = { 2, 3, 4 };
	static const int kMesh[3] = {
		BIKE_PATH_MED, BIKE_PATH_LOW, BIKE_PATH_VLOW
	};
	static const int kMap[3] = {
		BIKE_PATH_MED_MAP, BIKE_PATH_LOW_MAP, BIKE_PATH_VLOW_MAP
	};
	int i;

	s_race_lodAll = 0;
	for (i = 0; i < RACER_LOD_SLOTS; i++)
		s_race_lodOk[i] = 0;

	for (i = 0; i < 3; i++) {
		int         slot = kSlot[i];
		const char *mesh = bikes_asset(kMesh[i]);
		const char *map  = bikes_asset(kMap[i]);
		int         n    = 0;

		if (!mesh || !map)
			continue;
		if (!mesh_load_with_map(&s_race_lodMesh[slot], mesh, map,
		                        MESH_SCALE_3DF_MAP)) {
			mrlog_err("bike: lod %s did not load", mesh);
			continue;
		}
		if (!prim_build_from_mesh_bank(&s_race_lodMesh[slot],
		                               &s_race_lodPrims[slot], &n, 0,
		                               RACE_BIKE_BANK) || n <= 0) {
			mesh_free(&s_race_lodMesh[slot]);
			continue;
		}
		s_race_lodPrimCount[slot] = n;
		s_race_lodOk[slot] = 1;
		mrlog("bike: lod slot %d %s, %d prims", slot, mesh, n);
	}

	/*
	 * ALL THREE OR NONE. racerdraw_select_lod has no fallback: it picks
	 * a slot from a distance and the draw expects that mesh. One
	 * missing body makes bikes vanish in ONE distance band and nowhere
	 * else.
	 */
	s_race_lodAll = s_race_lodOk[2] && s_race_lodOk[3] && s_race_lodOk[4];
	if (!s_race_lodAll)
		mrlog("bike: lod bodies incomplete, every racer draws slot 0");
}

/*
 * THE BIKE ON THE GRID - manifest, bank, skeleton, staticPied pose,
 * primitives. staticPied is the manifest's standing pose, not a choice
 * made here; bikes.h.
 *
 * ONE MODEL, EVERY RACER. They share the skeleton and its primitives
 * and only the matrix differs, which is what the original does with
 * its single g_meshSkl.
 */

static void race_load_bike(void)
{
	const char *bank0, *bank1, *body, *pose;
	int         n = 0;

	s_race_bikeOk = 0;
	s_race_bikePrims = NULL;
	s_race_bikePrimCount = 0;

	bikes_load_for_track(track_at(g_selectedTrack),
	                 g_pocketEnabled);

	/*
	 * THE RIDER'S POSE SET, FROM THE MANIFEST AND NOT bikes_asset():
	 * the PilotRot list is variable length and runs past BIKE_PATHS, so
	 * the crash, glance and podium frames are only reachable this way.
	 * Non-fatal - a bike with no poses rides, it just does not emote.
	 * racers.txt 9.52.
	 */
	memset(&s_race_pilotRots, 0, sizeof s_race_pilotRots);
	if (bikes_manifest())
		pilot_rots_load(&s_race_pilotRots, bikes_manifest(),
	                    BIKE_PATH_ROT_STATIC);

	bank0 = bikes_asset(BIKE_PATH_BANK0);
	bank1 = bikes_asset(BIKE_PATH_BANK1);
	body  = bikes_asset(BIKE_PATH_SKL);
	pose  = bikes_asset(BIKE_PATH_ROT_STATICPIED);
	if (!body) {
		mrlog_err("bike: the manifest names no skeleton\n");
		return;
	}

	/* Paletted 8-bit with a colour key, exactly as page 10 loads it -
	 * the liveries are palettes of one indexed page. */
	if (bank0 && bank_load(RACE_BIKE_BANK, bank0, 1))
		bank_alloc_texture_slot(RACE_BIKE_BANK, 1, 1, 8, 16);
	if (bank1 && bank_load(RACE_BIKE_BANK + 1, bank1, 1))
		bank_alloc_texture_slot(RACE_BIKE_BANK + 1, 1, 1, 8, 16);

	if (!skl_load(&s_race_bikeSkl, body)) {
		mrlog_err("bike: %s did not load\n", body);
		return;
	}
	/*
	 * THE GRID POSE IS staticPied. Page 10 bakes the same one -
	 * Game_LoadBikeAndBanks calls Rot_ApplyToRacer(&g_menuRacer, 0)
	 * with g_pilotRot[4] - and a race at rest shows that stance: one
	 * foot down, hands on the bars, the foot lifting to the peg once
	 * the bike moves [observed].
	 *
	 * DO NOT TRY TO CONFIRM A POSE FROM A VERTICAL EXTENT. Two poses
	 * as different as hands-on-the-bars and arms-out-front have
	 * near-identical y ranges, and a capture of the BIND POOL cannot
	 * disagree with itself at all because maybe_Racer_Draw never poses
	 * the pool. Compare a member matrix.
	 */
	if (pose && rot_load(&s_race_bikePose, pose))
		skl_apply_pose(&s_race_bikeSkl, &s_race_bikePose);

	if (!prim_build_from_mesh_bank(&s_race_bikeSkl.mesh,
	                               &s_race_bikePrims, &n, 0,
	                               RACE_BIKE_BANK) || n <= 0) {
		mrlog_err("bike: %s produced no primitives\n", body);
		skl_free(&s_race_bikeSkl);
		return;
	}

	/*
	 * THERE IS NO REST LIFT, AND ADDING ONE IS THE WRONG FIX. The
	 * original never offsets the racer position to place the bike - it
	 * builds the placement from the WHEEL CONTACTS, which already sit
	 * on the tarmac, so ride height is whatever the suspension made it
	 * that frame. racerdraw_racer owns that now.
	 */
	s_race_bikePrimCount = n;
	s_race_bikeOk = 1;

	/* And the hand skeleton beside it, posed and primmed the same way -
	 * see the note by s_race_handSkl. */
	{
		const char *hand = bikes_asset(BIKE_PATH_SKL_HAND);
		int         hn   = 0;

		s_race_handOk = 0;
		if (hand && skl_load(&s_race_handSkl, hand)) {
			if (s_race_bikePose.loaded)
				skl_apply_pose(&s_race_handSkl,
			                   &s_race_bikePose);
			if (prim_build_from_mesh_bank(&s_race_handSkl.mesh,
			                              &s_race_handPrims, &hn,
			                              0, RACE_BIKE_BANK)
			    && hn > 0) {
				s_race_handPrimCount = hn;
				s_race_handOk = 1;
				mrlog("bike: hand skeleton %s, %d prims\n",
				      hand, hn);
			} else {
				skl_free(&s_race_handSkl);
			}
		}
		if (!s_race_handOk)
			mrlog_err("bike: no hand skeleton - a crash will "
		              "weld the rider's hands to the bars\n");
	}

	bank_set_palette(RACE_BIKE_BANK, g_selectedBike & 7);
	mrlog("bike: %s, %d prims, bank slot %d\n", body, n, RACE_BIKE_BANK);
	race_load_wheels();
	race_load_lod_bodies();
	race_load_cockpit();
}

/*
 * The roster size, as maybe_Game_LoadRace decides it at 0x0047b5a0
 * [verified]:
 *
 *     network        -> the players in state 3
 *     solo, mode 0   -> 1 opponent
 *     solo, otherwise-> 23, or 11 with NbComputerCut
 *
 * maybe_Game_CreateRacers then builds opponentCount + 2 records: INDEX
 * 0 IS UNUSED and index 1 is the local player, so a default solo race
 * is racerCount 25 for 24 bikes on track.
 *
 * The network arm is not modelled - there is no session. [open]
 */
#define OPP_KMH_TO_MS      0.277778  /* 0x004d1938, 1/3.6            */
#define OPP_DIFF_EASY           0.9   /* 0x004d1928, difficulty 0     */
#define OPP_DIFF_HARD_SOLO     1.05   /* 0x004d1930, difficulty 2     */
/*                             1.1      0x004d1940, difficulty 2 on a
 *                                      NETWORK race - not modelled  */
#define OPP_TABLE_TOP            22   /* the last ComputerSpeed entry */

/*
 * ORIGIN  maybe_Race_SetOpponentSpeeds @ 0x0047bc60, the SOLO arm
 *
 * THE ONLY WRITER OF racer+0xb4 FOR AN OPPONENT, so a port that
 * substitutes c->realSpeedMax everywhere is wrong the moment the AI is
 * ticked: the player's copy is the carac's RealSpeedMax (Racer_Create,
 * 0x0045c572) and an opponent's is this table.
 *
 *     racers[2 + k].realSpeedMax =
 *         ComputerSpeed[sel] * (1/3.6) * pocket * difficulty
 *
 *     sel = (opponents == 23) ? 22 - k : 22 - 2 * k
 *
 * Two strides for the two roster sizes the game ships, so 23 and 11
 * both spread across the same 23 entries. ComputerSpeed is km/h out of
 * the .spt - level-spt.txt 2.6.
 *
 * THE POCKET FACTOR IS A DIVIDE, 1.0 / FactorPocket, so the pocket
 * bike makes every opponent FASTER in these units - the AI is
 * rebalanced against the smaller bike rather than the bike being
 * handled differently.
 *
 * AND THEN IT SHUFFLES, opponents/2 random swaps at 0x0047bdb1, so
 * WHICH bike gets which speed does not match MOTO - only the multiset
 * does. Do not A/B this field per racer. The original gates the
 * shuffle on 0x004dbfd4 == 1 and 0x004e9830 == 0 and neither is
 * modelled, so ours always runs. [open]
 *
 * The network arm walks the table backwards from 22 and picks racers
 * out of the player table at 0x00508048; not transcribed. [open]
 */
static void race_set_opponent_speeds(MR_Racer *racers, int count,
                                     int opponents)
{
	const MR_Level *lvl = &s_race_level;
	double          base;
	int             rounds;
	int             k;

	if (!racers || opponents <= 0 || lvl->nbComputer <= 0)
		return;

	base = 1.0;
	if (g_pocketEnabled && lvl->factorPocket != 0.0f)
		base = 1.0 / (double)lvl->factorPocket;

	if (g_options.difficulty == 0)
		base *= OPP_DIFF_EASY;
	else if (g_options.difficulty == 2)
		base *= OPP_DIFF_HARD_SOLO;

	for (k = 0; k < opponents; k++) {
		int sel = (opponents == 23) ? (OPP_TABLE_TOP - k)
		                            : (OPP_TABLE_TOP - 2 * k);
		int slot = 2 + k;

		if (slot >= count)
			break;
		if (sel < 0)
			sel = 0;
		if (sel >= lvl->nbComputer)
			sel = lvl->nbComputer - 1;

		racers[slot].realSpeedMax =
		    (float)((double)lvl->computerSpeed[sel]
		            * OPP_KMH_TO_MS * base);
	}

	/*
	 * THE SHUFFLE, 0x0047bda6. Two indices in 2 .. opponents+1, and
	 * the swap is skipped when they land on the same racer - which
	 * is the original's own test, not a tidy-up.
	 */
	rounds = opponents / 2;
	for (k = 0; k < rounds; k++) {
		int a = 2 + (rand() % opponents);
		int b = 2 + (rand() % opponents);
		float t;

		if (a == b || a >= count || b >= count)
			continue;
		t = racers[a].realSpeedMax;
		racers[a].realSpeedMax = racers[b].realSpeedMax;
		racers[b].realSpeedMax = t;
	}

	{
		float lo = racers[2].realSpeedMax;
		float hi = lo;

		for (k = 1; k < opponents && 2 + k < count; k++) {
			float v = racers[2 + k].realSpeedMax;

			if (v < lo) lo = v;
			if (v > hi) hi = v;
		}
		mrlog("grid: %d opponent speeds set, %.2f .. %.2f m/s\n",
		      opponents, lo, hi);
	}
}


/* 0x004d0e88, added to the racer's realSpeedMax in manual. */
#define K_MANUAL_SPEED_BONUS 2.0f

static void race_build_grid(void)
{
	int opponents;
	int racerCount;
	int placed;

	if (!g_soloMode) {
		/*
		 * THE ROSTER IS THE LOBBY'S, so slot N is the same bike on
		 * every machine - which is what lets a STATE name a slot.
		 */
		const MR_NetLobby *lobby = netplay_lobby();
		int                top   = 0;
		int                i;

		if (!lobby) {
			mrlog_err("grid: link game with no lobby\n");
			return;
		}
		for (i = 1; i < NET_PEER_MAX; i++)
			if (lobby->players[i].dpId != 0)
				top = i;
		if (top < 1) {
			mrlog_err("grid: link game with an empty roster\n");
			return;
		}
		opponents  = 0;
		racerCount = top + 1;
	} else if (g_selectedMode == 0) {
		opponents  = 1;
		racerCount = opponents + 2;
	} else {
		opponents  = g_options.opponents ? 11 : 23;
		racerCount = opponents + 2;
	}
	placed = grid_build(s_race_grid, GRID_MAX_SLOTS, &s_race_trk,
	                    racerCount, g_reverseEnabled, g_selectedMode);
	if (!placed) {
		mrlog_err("grid: could not place %d racers\n", racerCount);
		return;
	}
	grid_log(s_race_grid, racerCount, s_race_level.land);

	s_race_racerCount = racerCount;

	/*
	 * AND WHICH ONE IS OURS. Always 1 in solo; in a net game the peer id
	 * the relay gave us, clamped so a bad one cannot index past the
	 * array.
	 */
	s_race_localIndex = 1;
	if (!g_soloMode) {
		int self = netplay_self();

		if (self >= 1 && self < racerCount)
			s_race_localIndex = self;
		else
			mrlog_err("grid: peer %d is not a slot in %d racer(s)\n",
		              self, racerCount);
	}
	mrlog("grid: local racer is slot %d of %d\n", s_race_localIndex,
	      racerCount);

	racer_place_from_grid(s_race_racers, racerCount, s_race_grid,
	                      s_race_trk.segmentCount, g_reverseEnabled,
	                      s_race_localIndex);

	/*
	 * THE LOBBY NAMES AND BIKES, AND THIS MUST FOLLOW PLACEMENT.
	 * racer_place_from_grid WRITES playerName itself - "COMPUTER"
	 * on every slot but ours - so copying the lobby in before it
	 * is silently undone and every peer wears an AI's name.
	 *
	 * Placement can only know OUR name, so the lobby is the only
	 * source for the rest; all slots including ours, so one place
	 * decides. Still ahead of the bike_at loop below, which reads
	 * bikeIndex.
	 */
	if (!g_soloMode) {
		const MR_NetLobby *lobby = netplay_lobby();
		int                i;

		for (i = 1; lobby && i < racerCount && i < NET_PEER_MAX; i++) {
			if (lobby->players[i].name[0]) {
				strncpy(s_race_racers[i].playerName,
				        lobby->players[i].name,
				        sizeof s_race_racers[i].playerName - 1);
				s_race_racers[i].playerName[
				    sizeof s_race_racers[i].playerName - 1] = 0;
			}
			if (lobby->bikes[i] >= 0)
				s_race_racers[i].bikeIndex = lobby->bikes[i];
		}
	}


	/*
	 * ORIGIN  maybe_Game_InitRace @ 0x0047bf70, at 0x0047c02a
	 *
	 * SLOT 2 IS THE GHOST IN PRACTICE, NOT AN OPPONENT. Practice asks
	 * for one opponent so the record is built like any other, but
	 * Race_TickAllRacers drives slot 2 off a recorded stream with
	 * maybe_Racer_TickRemote and runs no AI (racers.txt 9.5). LEFT
	 * ACTIVE WITH NO STREAM, our AI tick picks it up and rides it -
	 * that is the phantom second rider on a practice lap.
	 *
	 * It starts on the player's own spot by design: grid.c gates both
	 * the grid step and the lateral offset on selectedMode != 0.
	 *
	 * AND IT IS WHY PRACTICE DRAWS NO POSITION - both HUD arms count
	 * ACTIVE racers, so the count is 1, hud_draw_pos_only wants more
	 * and time attack reads "1/1".
	 *
	 * Ours clears it on both arms; retail does the same until a lap has
	 * been recorded. [open - the ghost recorder, maybe_Ghost_RecordFrame
	 * 0x004191c0, and its playback]
	 */
	/*
	 * SOLO ONLY, AND THE TEST IS NOT JUST THE MODE. In a NETWORK game
	 * mode 0 means SINGLE RACE, not practice - "0 is practice in a
	 * solo game and single race in a network one" [verified,
	 * 0x004478a6, see page 5]. Without the g_soloMode term this
	 * deactivates a real player's bike in slot 2.
	 */
	if (g_soloMode && g_selectedMode == 0 && racerCount > 2) {
		s_race_racers[2].active = race_ghost_has_lap() ? 1 : 0;
		mrlog("grid: practice - slot 2 is the ghost, %s "
		      "(time attack %s)\n",
		      s_race_racers[2].active ? "racing the taped lap"
		                              : "nothing taped yet",
		      g_timeAttack ? "on" : "off");
	}


	/*
	 * racer+0xb4, AND IT HAS TO BE IN THIS ORDER. Racer_Create copies
	 * the carac's RealSpeedMax into every racer at 0x0045c572 and
	 * maybe_Race_SetOpponentSpeeds then overwrites the opponents' from
	 * the ComputerSpeed table.
	 *
	 * BOTH MUST RUN AFTER racer_place_from_grid, which memsets the
	 * array - a write before it is silently lost.
	 */
	{
		int i;

		for (i = 0; i < racerCount; i++) {
			const MR_BikeCarac *bc =
			    bike_at(s_race_racers[i].bikeIndex);

			if (bc)
				s_race_racers[i].realSpeedMax = bc->realSpeedMax;
		}
	}
	/*
	 * THE AI SPEED TABLE, SO SOLO ONLY. ComputerSpeed exists to pace AI
	 * opponents; a remote racer is paced by its own machine, and
	 * overwriting its realSpeedMax here would cap a real player at an
	 * AI's speed.
	 */
	if (g_soloMode)
		race_set_opponent_speeds(s_race_racers, racerCount, opponents);

	racer_log(s_race_racers, racerCount, s_race_level.land);
	race_load_bike();

	/* BEFORE THE BANK CHANGES. A voice outlives its sample
	   otherwise, and audio_load_bank frees the PCM under it. */
	race_audio_stop();

	/*
	 * ORIGIN  FUN_00410d70, which calls Snd_LoadRaceBank with
	 *         g_bikeCarac[g_nSelectedBike].PersoAnim
	 *
	 * THE BIKE PICKS THE BANK, NOT THE TRACK. PersoAnim is 1 on every
	 * Motocross carac and 0 on every Speed one, and the same field
	 * chooses the bike's start sample on page 10 and its animation set
	 * in maybe_Racer_UpdateSkeleton. One flag, three subsystems - and
	 * MotoType is NOT it, see menu.c.
	 *
	 * It sits behind a loading caption in the original, so this is
	 * the right seam: the menu bank goes out and the race bank
	 * comes in while the loading screen is up.
	 *
	 * THE WAY BACK RELOADS IT, and the seam is menu_set_page -
	 * the original reloads the menu bank from Menu_LoadResources,
	 * which Menu_SetPage calls on the way into the menus, so
	 * entering any menu page restores it.  Do not add a second
	 * reload on a new race-to-menu route; there is one home for
	 * it and page 9 already lost its local copy to it.
	 */
	{
		const MR_BikeCarac *bc = bike_at(g_selectedBike);

		audio_load_bank((bc && bc->persoAnim != 0)
		                    ? AUDIO_BANK_RACE_KTM
		                    : AUDIO_BANK_RACE_NINJA);
	}

	/*
	 * ORIGIN  maybe_Game_InitRace @ 0x0047bf95, AudioTrack out of the
	 *         level manifest at 0x004db67c
	 *
	 * MODE 0, SO RACE MUSIC DOES NOT LOOP - it plays once and nothing
	 * restarts it, so a race longer than its track finishes in silence.
	 * That is the original's behaviour and not something to improve on.
	 *
	 * THE MENU MUSIC IS REPLACED, NOT STOPPED. race_audio_stop above is
	 * Snd_StopAll and that call is the SAMPLES; the menu track is still
	 * playing here and this request displaces it.
	 *
	 * audio-cues.txt 11.3.
	 */
	audio_music_cue(s_race_level.audioTrack, CD_MODE_ONCE);

	/*
	 * THIS MUST STAY AFTER race_load_bike, which is what loads the
	 * page whose palette count decides every livery. Ahead of it the
	 * count is zero, every racer takes -1 and the whole field draws in
	 * one colour - AND ONLY ON THE FIRST RACE OF A SESSION, because a
	 * bank outlives its race and the second one finds the previous
	 * page still resident.
	 */
	/*
	 * THE LIVERIES, AND THEY ARE NOT RANDOM.
	 *
	 * ORIGIN  maybe_Game_CreateRacers @ 0x00410b8c, the opponent loop
	 *
	 * Every racer rides the SAME MESH, so a retail grid's variety is
	 * entirely palette, picked from the racer's own index:
	 *
	 *     player      livery = g_nSelectedBike & 7
	 *     opponent i  livery = (i - 1) & 7
	 *                 except 0 when (i - 1) == g_nSelectedBike
	 *
	 * It reads as random on screen only because grid order is not
	 * finishing order.
	 *
	 * THE SUBSTITUTION IS IMPERFECT AND IS TRANSCRIBED AS FOUND: the
	 * original compares BEFORE the mask, so an opponent that collides
	 * with the player's livery by wrapping - i - 1 == 8 against a
	 * player on 0 - is not caught.
	 *
	 * -1 IF THE PAGE HAS NO PALETTES, which means "not paletted" all
	 * the way down to the queue.
	 *
	 * ALL OPPONENTS RIDE BIKE 0 IN SOLO, as the original does, and
	 * racer_place_from_grid's memset already leaves it there - which
	 * is why no code here writes it.
	 * A NET RACE KEYS ON bikeIndex - netplay-relay.txt 3.7.
	 */
	{
		int       i;
		int       wheel;
		const MR_BikeCarac *bc;
		MR_Bank  *bb = bank_get(RACE_BIKE_BANK);
		const int paletted = (bb && bb->paletteCount > 1);
		const int mine = g_selectedBike;

		for (i = 0; i < racerCount; i++) {
			int liv;

			if (!paletted)
				liv = -1;
			else if (!g_soloMode)
				liv = s_race_racers[i].bikeIndex & 7;
			else if (i == 0 || i == s_race_localIndex)
				liv = mine & 7;
			else if (i - 1 == mine)
				liv = 0;
			else
				liv = (i - 1) & 7;
			/*
			 * ALL FOUR INSTANCES, as Racer_Create does - the body
			 * and the rider unconditionally, the two wheels only
			 * when the bike is not a dirt bike. Racer_Create forces
			 * its param_4 to -1 when PersoAnim is 1 and every
			 * caller passes 0, so that is the whole rule: road
			 * bikes have coloured wheels, dirt bikes do not.
			 */
			bc = bike_at(s_race_racers[i].bikeIndex);
			wheel = (liv >= 0 && bc && bc->persoAnim == 1) ? -1 : liv;

			s_race_racers[i].livery           = liv;
			s_race_racers[i].liveryRider      = liv;
			s_race_racers[i].liveryFrontWheel = wheel;
			s_race_racers[i].liveryRearWheel  = wheel;
		}
		mrlog("race: liveries %s, player %d\n",
		      paletted ? "on" : "off (page has no palettes)",
		      s_race_racers[s_race_localIndex].livery);
	}

	/*
	 * THE SKID RIBBONS, RESET AND HANDED OUT. FUN_00457300 clears all
	 * sixteen; a racer owns two ACROSS FRAMES, which is why they are
	 * indexed rather than pooled.
	 *
	 * SIXTEEN SLOTS DO NOT COVER A FULL FIELD. Two each runs out after
	 * eight racers and the rest take -1, so a 24-bike grid leaves most
	 * of it laying no rubber. The original has the same sixteen. [open]
	 */
	fx_ribbon_reset_all();
	fx_particle_reset_all();
	{
		int i;

		for (i = 0; i < racerCount; i++) {
			int slot = i * 2;

			s_race_racers[i].fxSlot0 = (slot < FX_RIBBON_COUNT)
			        ? slot : -1;
			s_race_racers[i].fxSlot1 = (slot + 1 < FX_RIBBON_COUNT)
			        ? slot + 1 : -1;
		}
	}

	/*
	 * THE SHADOW'S BANK IS BIKE-RELATIVE AND MUST BE REBASED.
	 * racerdraw_init_shadow writes the original's literal 1, and in
	 * MOTO that IS the page - its banks run bike 0, bike 1, then the
	 * track from 2. The port puts the track at 8 and the bike at
	 * RACE_BIKE_BANK, so the same file is a different slot here.
	 *
	 * WITHOUT THIS the shadow points at a slot nothing loads, both
	 * quads fall through to flatColor, and the silhouette becomes a
	 * SOLID BLACK RECTANGLE. racer-draw-audit.txt 60.
	 */
	{
		int i;

		for (i = 0; i < racerCount; i++)
			prim_rebase_banks(&s_race_racers[i].bikeShadow.prim[0], 2,
		                      RACE_BIKE_BANK);
	}

	/*
	 * VERTEX NORMALS, ONCE PER RACER PER SLOT - Racer_Create builds
	 * them at 0x0045c35d and 0x0045c4dc. They depend only on the bind
	 * pose, which is why this is not per frame.
	 */
	{
		int i, slot;

		for (i = 0; i < racerCount; i++)
			for (slot = 0; slot < 2; slot++)
				racerdraw_build_normals(&s_race_racers[i],
		                          race_skl_for_slot(slot),
		                          slot);
	}

	/*
	 * AND THE CLOCK. The checkpoint times have to be applied
	 * first: they come from LEVEL.BPT and overwrite what the
	 * .trk carries, and race_time_limit_ms reads the result.
	 */
	race_apply_checkpoint_times();
	race_clock_reset();
}

/*
 * ORIGIN  NONE - ours. The globals maybe_Racer_Draw reaches for.
 */
static MR_RacerDrawSet s_race_drawSet;

static void race_build_draw_set(void)
{
	int w;

	memset(&s_race_drawSet, 0, sizeof s_race_drawSet);
	s_race_drawSet.skl       = &s_race_bikeSkl;
	s_race_drawSet.prims     = s_race_bikePrims;
	s_race_drawSet.primCount = s_race_bikePrimCount;
	/* race_draw_racers re-points these per racer, from skeletonSlot. */

	for (w = 0; w < RACE_WHEELS; w++) {
		s_race_drawSet.wheelOk[w]        = s_race_wheelOk[w];
		s_race_drawSet.wheelMesh[w]      = &s_race_wheelMesh[w];
		s_race_drawSet.wheelPrims[w]     = s_race_wheelPrims[w];
		s_race_drawSet.wheelPrimCount[w] = s_race_wheelPrimCount[w];
	}

	for (w = 2; w < RACER_LOD_SLOTS; w++) {
		if (!s_race_lodOk[w])
			continue;
		s_race_drawSet.lodMesh[w]      = &s_race_lodMesh[w];
		s_race_drawSet.lodPrims[w]     = s_race_lodPrims[w];
		s_race_drawSet.lodPrimCount[w] = s_race_lodPrimCount[w];
	}
}


/*
 * ORIGIN  NONE - ours. THE POSE THE ORIGINAL FILLS IN THE TICK.
 *
 * MOTO keeps its bones in the racer as MR_RacerPart[5][20] and writes
 * their angles during the update, so by the time maybe_Racer_Draw runs
 * they are simply there. The port keeps its skeleton in an MR_Skeleton
 * loaded from the .skl and builds an MR_AnimFrame per racer per frame,
 * so something has to carry one into the other. That is this.
 *
 * IT MUST RUN BEFORE racerdraw_racer, every racer, every frame -
 * racerdraw_bind_skeleton memsets the pose array before refilling it,
 * so calling it afterwards would erase the steering angle the draw
 * writes into part 1.
 */
static void race_pose_racer(MR_Racer *r, float distSq, int isPlayer)
{
	const MR_BikeCarac *bc = bike_at(r->bikeIndex);
	MR_AnimFrame        frame;
	MR_PoseEnv          env;
	float               front[3];
	float               rear[3];

	/*
	 * DELIBERATELY UNUSED - the pose flag is PersoAnim, not this; see
	 * the note at racer_update_pose below. Kept because the caller
	 * already computes it and the AI and remote ticks will want it.
	 */
	(void)isPlayer;

	if (!bc)
		return;

	memset(&frame, 0, sizeof frame);

	if (s_race_pilotRots.frameCount > 0) {
		/*
		 * THE GROUND PAIR, because what this feeds is a TILT:
		 * racer_update_pose takes (front.y - rear.y) over the
		 * wheelbase. The chassis pair differs in y by a constant
		 * 5.76 on the street bike even on dead flat ground, which
		 * would lean the rider permanently.
		 */
		racer_wheel_ground(r, bc, rear, front);
		racer_wheelie_swing_contact(front, rear, r->wheelieAngle);

		/*
		 * THE GLOBALS THE ORIGINAL READS, handed in - see racer.h
		 * MR_PoseEnv. podiumMode and podiumTimer have no port
		 * equivalent, so those passes take their false edge. [open]
		 */
		memset(&env, 0, sizeof env);
		env.rots       = &s_race_pilotRots;
		env.racers     = s_race_racers;
		env.racerCount = s_race_racerCount;
		env.fieldSize  = s_race_racerCount;
		/*
		 * A FREE-RUNNING STAMP, NOT THE RACE CLOCK, and that is what
		 * the original uses: UpdatePose picks the next stunt off
		 * g_framePrevMs. tm_now also STOPS WHILE PAUSED, which a pose
		 * timer should - see racer_pose_pitch_tick.
		 *
		 * A SOURCE THAT DOES NOT ADVANCE BREAKS TWO THINGS AT ONCE
		 * here: elapsed is always 0 so a stunt never reaches its
		 * duration and the rider sticks in it through the landing and
		 * the crash, and stuntNext is nowMs %% stuntCount so only the
		 * first stunt of the set ever plays.
		 */
		env.nowMs      = (int)(tm_now() * 1000.0);
		env.stunts     = s_race_pilotRots.stunt;
		env.stuntCount = s_race_pilotRots.stuntCount;

		/*
		 * PersoAnim, NOT isPlayer - maybe_Racer_UpdateSkeleton pushes
		 * g_bikeCarac[bikeIndex].PersoAnim at 0x00467bbe. Passing the
		 * local-player flag instead turns on four branches the
		 * original leaves off for every shipped bike. See
		 * racer_pose_lean_weight.
		 */
		racer_update_pose(r, bc->persoAnim, &env, bc, front, rear,
		                  (float)sqrt((double)distSq), &frame);
	}

	/*
	 * THE SLOT CHOOSES THE SKELETON. r->skeletonSlot is already set
	 * by racer_select_skeleton_slot, and slot 1 - a crash, or
	 * handSkeleton - wants the mHand topology.
	 */
	racerdraw_bind_skeleton(r, race_skl_for_slot(r->skeletonSlot),
	                        &frame);
}


/*
 * ORIGIN  maybe_Race_DrawRacers @ 0x00458b80
 *
 * THE RACER DRAW LOOP, and the owner of racer+0x0. The original:
 *
 *     this = &g_menuRacer;              racer 1; index 0 is unused
 *     do {
 *         this->drawnThisFrame = 0;     for EVERY racer
 *         if (active && FUN_0046e8b0(position, 512.0f)) {
 *             if (!DAT_004dc008 || viewedRacer != i) {
 *                 ...segment list filter...
 *                 maybe_Racer_Draw(this, ..., -1);
 *                 maybe_Racer_DrawShadow(this, ...);
 *                 this->drawnThisFrame = 1;   ONLY if drawn
 *             } else {
 *                 FUN_00462110(...);    the first-person path
 *             }
 *         }
 *     } while (++i < g_racerCount);
 *
 * THREE DIVERGENCES, all deliberate:
 *
 * THE NEAREST-FIRST SORT IS OURS, and only because the primitive
 * budget is: the original walks 1..count in index order and has no
 * budget to run out of. Without the sort the far half of the field eats
 * the queue before the near half is drawn.
 *
 * The first-person fork at 0x00458bdf is reproduced - see it below.
 */
static int race_draw_racers(const float *view34, const float *camPos,
                          int budgetLeft, const int *segmentList,
                          int segmentCount)
{
	int   order[RACER_MAX];
	float dist[RACER_MAX];
	int   i, a, b, n = 0;
	int   drawn = 0;

	if (!s_race_bikeOk || s_race_racerCount < 2)
		return 0;

	race_build_draw_set();

	/* 0 FOR EVERY RACER, active or not - the original clears it at
	 * the top of the loop body, before the active test. */
	for (i = 1; i < s_race_racerCount; i++)
		s_race_racers[i].drawnThisFrame = 0;

	for (i = 1; i < s_race_racerCount; i++) {
		const MR_Racer *r = &s_race_racers[i];
		float dx, dy, dz;

		if (!r->active)
			continue;
		dx = r->position[0] - camPos[0];
		dy = r->position[1] - camPos[1];
		dz = r->position[2] - camPos[2];
		order[n] = i;
		dist[n] = dx * dx + dy * dy + dz * dz;
		n++;
	}
	for (a = 1; a < n; a++) {
		int   oi = order[a];
		float di = dist[a];
		for (b = a - 1; b >= 0 && dist[b] > di; b--) {
			order[b + 1] = order[b];
			dist[b + 1] = dist[b];
		}
		order[b + 1] = oi;
		dist[b + 1] = di;
	}

	for (a = 0; a < n; a++) {
		MR_Racer *r = &s_race_racers[order[a]];

		if (budgetLeft <= 0)
			break;

		/*
		 * SELECTED FROM THE DRAW, NOT THE TICK, which is a divergence:
		 * the original calls maybe_Racer_SelectLod from gameplay
		 * (FUN_00467b30) and maybe_Racer_Draw then takes -1 and uses
		 * whatever it left. race_tick_all has no camera and the eye
		 * only exists here.
		 *
		 * DO NOT STASH THE EYE IN A STATIC for the tick to read: the
		 * first frame would select from a zero eye and put every racer
		 * on slot 4, the very-low body. [open]
		 */
		if (s_race_lodAll)
			racerdraw_select_lod(r, camPos);

		if (!racerdraw_visible(r->position, RACERDRAW_RACER_RADIUS,
		                       camPos))
		    continue;

		/*
		 * THE SEGMENT LIST, AND THIS IS WHAT REMOVES A RACER WHO HAS
		 * PULLED AHEAD - not the cull above and not the frustum.
		 * racer-draw-pipeline.txt 8 has the list and its globals.
		 *
		 * A NULL list means no filtering, as the original does. That
		 * branch is dead in the shipped binary - its one caller always
		 * passes a list - and here it is a valve for a frame with no
		 * track.
		 */
		if (segmentList && segmentCount > 0) {
			int k, inList = 0;

			for (k = 0; k < segmentCount; k++) {
				if (segmentList[k] == r->segmentStart) {
					inList = 1;
					break;
				}
			}
			if (!inList)
				continue;
		}

		race_pose_racer(r, dist[a], order[a] == 1);

		/*
		 * AND THE SKELETON THIS RACER IS DRAWN FROM. The set is
		 * built once for the frame but the slot is PER RACER - one
		 * rider can be down while the rest ride - so the skeleton
		 * and its prims are re-pointed here, together, immediately
		 * before the draw. Swapping one without the other queues
		 * the base mesh's faces against the hand mesh's vertices.
		 */
		{
			const MR_Skeleton *sk =
			        race_skl_for_slot(r->skeletonSlot);

			s_race_drawSet.skl = sk;
			if (sk == &s_race_handSkl) {
				s_race_drawSet.prims     = s_race_handPrims;
				s_race_drawSet.primCount = s_race_handPrimCount;
			} else {
				s_race_drawSet.prims     = s_race_bikePrims;
				s_race_drawSet.primCount = s_race_bikePrimCount;
			}
		}

		/*
		 * ORIGIN  maybe_Race_DrawRacers @ 0x00458b80, at 0x00458bdf
		 *
		 * THE VIEWED RACER'S BIKE IS NOT DRAWN IN FIRST PERSON - mode
		 * 6 sits the eye on the bike, so drawing it would fill the
		 * screen with the inside of the fairing.
		 *
		 * AND NO SHADOW, AND NO drawnThisFrame: the original's else arm
		 * reaches neither, so a rider in first person casts no shadow
		 * and the HUD map does not mark a bike nothing drew.
		 * Continuing before both is what reproduces that.
		 *
		 * The cockpit itself goes up in its own depth-off pass - see
		 * race_draw_3d.
		 */
		if (s_race_fpActive && r == race_viewed_racer())
			continue;

		if (racerdraw_racer(r, &s_race_drawSet, view34, camPos, -1,
		                    &budgetLeft, &s_race_trk) <= 0)
		    continue;

		/*
		 * THE SHADOW GOES WITH THE RACER, in the original order:
		 * maybe_Racer_Draw, maybe_Racer_DrawShadow, drawnThisFrame.
		 * Not charged to the budget - the original has none.
		 */
		racerdraw_shadow(r, view34, camPos);

		r->drawnThisFrame = 1;
		drawn++;
	}
	return drawn;
}

/*
 * THE CULL FAR DISTANCE - 0x0051d1bc's first float at the live detail
 * level, which the frustum builder is handed at 0x00480147. The
 * distance test reads the same record; see racerdraw_visible.
 */
#define RACE_CULL_FAR    18000.0f

/* g_projScaleX, 0x006d20c4, read live out of a race - base 394 with
 * zoom 1.0, against the menu's 1911. */
#define RACE_VIEW_LENS   394.0f

/*
 * The race near plane, 0x42800000 at 0x00410e19. The menu uses
 * 256 and the two genuinely differ - see race_draw_3d.
 */
#define RACE_NEAR_PLANE   64.0f

/*
 * One camera for both halves of the frame: the sky is a backdrop and
 * has to be laid down before the 3D bracket, but it needs the same eye
 * and target the 3D pass will use.
 *
 * ADVANCE EXACTLY ONCE A FRAME. Both callers need the camera and if
 * both STEP it the lerps run at double rate, which reads as a camera
 * that tracks the bike far too rigidly. The original has the same
 * split and no problem: Camera_UpdateChase runs in the tick and
 * maybe_Game_RenderFrame only READS the record. race_update advances;
 * race_draw_3d and race_draw_2d read.
 */
static int race_camera(int advance, float eye[3], float viewEye[3],
                     float target[3], float *lens)
{
	const MR_Racer *p;
	int             i;

	if (!s_race_ok || !s_race_trk.segments || s_race_racerCount < 2)
		return 0;

	/*
	 * THE RACER, not the grid slot it started on. This followed
	 * s_race_grid[1] while nothing moved, which was fine until
	 * something did: the player drove away and the camera stayed
	 * on the grid watching the racer parked next to it, which
	 * reads as the bike having been duplicated.
	 *
	 * The original follows g_viewedRacerIndex's record, taking
	 * its position from +0x0c and its matrix from +0x98 - so a
	 * racer is what belongs here, and index 1 is the local
	 * player.
	 */
	p = &s_race_racers[s_race_localIndex];
	if (advance) {
		/*
		 * ORIGIN  Camera_SelectView @ 0x004583e0, its raceState arm
		 *
		 * THE FINISHED CAMERA IS THE RACING CAMERA PLUS ONE FLAG.
		 * Both arms of the original select mode 7 and call the same
		 * Camera_UpdateChase; all that changes is nSelectFlag, which
		 * gates the three terms in camera_update.
		 *
		 * IT IS THE VIEWED RACER'S STATE, not the local one's and
		 * not race_over_hud() - watching an AI come home gets the
		 * pan too. That is also why it is set here rather than
		 * inside camera_update: the selection is the caller's job
		 * in the original as well.
		 */
		{
			const MR_Racer *vr = race_viewed_racer();

			s_race_cam.selectFlag =
			    (vr && vr->raceState == RACESTATE_FINISHED);
		}

		/*
		 * THE CRASH ARM BELOW NEVER TOUCHES THE EYE - only the aim,
		 * which tracks the midpoint of the bike and the rider as the
		 * two separate. The camera stops at the impact and watches it.
		 * camera.txt 1.9.9.
		 *
		 * The 0.5f there is EXACT and not an approximation: the
		 * original divides by 2.0 and 1.0/2.0 is exact in binary.
		 *
		 * THE DIRECTOR OVERRIDES ALL OF IT. Camera_SelectView picks a
		 * mode and then, if any of its flags is up AND the track placed
		 * cameras, throws the choice away for the trackside shots. One
		 * of those flags is a replay, which is why retail replays look
		 * like the attract demo; the rest are the attract demo. A track
		 * with no cameras gets no shots and the chase carries on.
		 */
		/*
		 * ORIGIN  Camera_SelectView @ 0x004583e0, at 0x0045841e
		 *
		 * THE ZOOM IS RESET EVERY FRAME, before any mode is chosen, so
		 * a zoom cannot outlive the shot that wanted it. Leaving each
		 * shot to restore it is how it leaked - the director can hand
		 * the frame back to the chase, and the chase is not part of the
		 * director.
		 */
		screen3d_set_proj_zoom(1.0f);

		/* F5's chase slot - race_camera_cycle_view. */
		s_race_cam.boomSlot = s_race_camViewSlot;

		/* Ours: the original has no field to clear. */
		s_race_cam.roll      = 0;
		s_race_cam.viewValid = 0;

		/*
		 * ORIGIN  Camera_SelectView @ 0x004583e0, at 0x004583e1
		 *
		 * g_bFirstPersonActive, resolved HERE because the original
		 * resolves it in the same function that picks the mode.
		 * race_draw_racers reads it.
		 */
		s_race_fpActive = (s_race_camFirstPerson && !p->crashLatch
		                   && s_race_camLookSlot == 0);

		if ((race_auto_ride()
			 || race_replay_mode() == REPLAY_MODE_PLAY)
		    && s_race_trk.cameraCount > 0) {
			camera_update_trackside(&s_race_cam, race_frame_delta(),
			                        p, &s_race_trk);
			/*
			 * SHOT 1 IS THE CHASE, and the director says so by
			 * leaving the eye alone - the original reselects mode
			 * 7 for it. Fall through to camera_update so that one
			 * shot is the real chase rather than a copy of it.
			 */
			if (!s_race_cam.directorEye)
				camera_update(&s_race_cam, p->position,
			                  p->matOrientation, race_frame_delta());
		} else if (s_race_camLookSlot) {
			/*
			 * THE LOOK BRANCH PRE-EMPTS BOTH CAMERAS - mode 6 falls
			 * through to the chase on a non-zero slot at 0x004701b5,
			 * so first person reaches the same placement.
			 */
			camera_look_slot(&s_race_cam, p->position,
			                 p->matOrientation, s_race_camLookSlot);
		} else if (s_race_fpActive) {
			/*
			 * ORIGIN  Camera_SelectView @ 0x004583e0, at 0x004583e1
			 *
			 * MODE 6 NEVER SEES A CRASH: the crash shot belongs to
			 * Camera_UpdateChase, and Camera_SelectView's own
			 * crashLatch test is what keeps a crashed rider out of
			 * mode 6 - which is why this arm sits above it.
			 */
			camera_first_person(&s_race_cam, p);
		} else if (p->crashLatch) {
			for (i = 0; i < 3; i++)
				s_race_cam.target[i] =
			            (p->position[i] + p->riderPosition[i])
			            * 0.5f;
		} else if (s_race_camSide) {
			camera_side(&s_race_cam, p->position, p->matOrientation,
			            s_race_camSide == 2);
		} else {
			camera_update(&s_race_cam, p->position,
			              p->matOrientation, race_frame_delta());
		}
	}

	for (i = 0; i < 3; i++) {
		eye[i]     = s_race_cam.eye[i];
		viewEye[i] = s_race_cam.viewEye[i];
		target[i]  = s_race_cam.target[i];
	}

	*lens = RACE_VIEW_LENS;
	return 1;
}

/* ================================================================== */
/* THE END-OF-RACE PLAQUE - the giant 1ST/2ND/3RD past the line.      */
/*                                                                    */
/* 3D GEOMETRY, not HUD text, and it lives here rather than in hud.c  */
/* because the original draws it from maybe_Game_RenderFrame and      */
/* resets it in maybe_Game_InitRace - the two places race.c owns.     */
/*                                                                    */
/* hud_draw_finish_place is the OTHER display of the same fact, the    */
/* small "%d%c" readout drawn only where this one declines. THEY MUST  */
/* NEVER BOTH APPEAR - see race_final_place. hud-race.txt 1.3.        */
/* ================================================================== */

#define RACE_PATH_SEP        '\\'     /* asset names use it   */
#define RACE_FINAL_MESHES       4
#define RACE_FINAL_WREATH       3         /* "Laureat", drawn behind */

#define RACE_FINAL_Z_START  16000.0f      /* 0x0047d5f8, at init     */
#define RACE_FINAL_Z_REST    2500.0f      /* 0x004d18f4              */
#define RACE_FINAL_Z_CLAMP   2504.0f      /* 0x004d18e8, a DOUBLE    */
#define RACE_FINAL_Z_RATE     (-80.0f)    /* 0x004d18f0, the fly-in  */
#define RACE_FINAL_Z_EXIT     (-30.0f)    /* 0x004d18fc, the fly-out */
#define RACE_FINAL_Z_MIN      768.0f      /* 0x004d1904, draw cutoff */
#define RACE_FINAL_BOB_RATE     4.0f      /* 0x004d18f8              */
#define RACE_FINAL_BOB_AMP    128.0f      /* 0x004f0c44, at init     */
#define RACE_FINAL_SPIN  0.015707963267948966 /* 0x004d18e0, DOUBLE  */
#define RACE_FINAL_SHADE       0x1f       /* flat, brightest of 32   */
#define RACE_FINAL_DEPTH_WREATH 16.0f     /* 0x41800000              */
#define RACE_FINAL_DEPTH_PLACE  13.0f     /* 0x41500000              */
#define RACE_FINAL_DETAIL         3       /* forced, then restored   */
/*
 * hwFlags 2 and bucketMode 0, as the original hands
 * Screen3D_QueuePrimitive. Bit 1 is "extends outside the viewport",
 * which a mesh this large at zoom 1.0 needs.
 *
 * IT DOES NOT CULL, and routing it through cull_and_queue_faces is the
 * natural-looking mistake: every face goes in at a FIXED depth so the
 * plaque sorts as one object and keeps its back faces. The cull would
 * derive a per-face depth and drop every back-facing triangle.
 */
#define RACE_FINAL_HWFLAGS        2

/*
 * THE ANGLE IS IN RADIANS AND THE TABLE IS NOT - see TRIG_IDX_PER_RAD.
 * Reading a radian phase straight as an index looks right, compiles,
 * and turns a four-second revolution into one lasting about three
 * hours; the plaque simply sits there.
 */

static MESH  s_race_finalMesh[RACE_FINAL_MESHES];
static int   s_race_finalOk;
static float s_race_finalZ     = RACE_FINAL_Z_START;
static float s_race_finalPhase;
static MR_Primitive *s_race_finalPrims[RACE_FINAL_MESHES];
static int   s_race_finalPrimCount[RACE_FINAL_MESHES];
static int   s_race_overHud;            /* g_nRaceOverHud  */
/*
 * DAT_00504eac, AND IT IS NOT THE PLAQUE'S OWN FLAG. It means THE
 * RESULT HAS BEEN ACKNOWLEDGED - by a key at 0x00431533, or by the
 * sequence reaching its transition at 0x00412b0c - and the plaque
 * flying out is one of the things that follows. Hud_DrawMap reads it
 * too.
 *
 * ONLY THE AUTO-RIDE PATH RAISES IT HERE, so a player who finishes a
 * race never acknowledges and the plaque stays up. [open]
 */
static int   s_race_creditsShown;

/*
 * ORIGIN  Race_LoadFinalMeshes @ 0x0047b600, whole
 *
 * Four .3df/.map pairs out of the track's own "final" directory. THE
 * FIRST IS SPELT "1rst", not "1st", on every shipped disc - correcting
 * the spelling just fails to load.
 *
 * ALL FOUR ARE CENTRED, [0] included: the decompiler hoists the loop's
 * pointer increment above the call and reads as though it skipped the
 * first, but 0x0047b774 sets ECX before the ADD.
 */
static void race_load_final_meshes(const MR_Track *t)
{
	static const char *const kNames[RACE_FINAL_MESHES] = {
		"1RST", "2ND", "3RD", "LAUREAT"
	};
	char leaf[64];
	char n3df[MAX_PATH];
	char nmap[MAX_PATH];
	int  i;

	s_race_finalOk = 0;
	for (i = 0; i < RACE_FINAL_MESHES; i++) {
		sprintf(leaf, "FINAL%c%s.3DF", RACE_PATH_SEP, kNames[i]);
		if (!track_asset_name(t, leaf, n3df, (int)sizeof n3df))
			return;
		sprintf(leaf, "FINAL%c%s.MAP", RACE_PATH_SEP, kNames[i]);
		if (!track_asset_name(t, leaf, nmap, (int)sizeof nmap))
			return;

		/*
		 * MESH_SCALE_3DF_MAP, NOT 0 - see mesh.h. Zero loads it
		 * 96 times too small, which draws, transforms and culls
		 * perfectly and is invisible on screen.
		 */
		if (!mesh_load_with_map(&s_race_finalMesh[i], n3df, nmap,
		                        MESH_SCALE_3DF_MAP)) {
			mrlog_err("race: final mesh %s did not load\n", n3df);
			return;
		}
	}
	for (i = 0; i < RACE_FINAL_MESHES; i++)
		mesh_center_on_bounds(&s_race_finalMesh[i], NULL);

	/*
	 * MESH_FACE IS NOT MR_Primitive, however well the cast compiles.
	 * The original needs no converter because Load3dfAndMap builds
	 * MR_Primitive directly; ours builds them once, here.
	 */
	for (i = 0; i < RACE_FINAL_MESHES; i++) {
		/* faceKindSelect 1 - the one call site that does. */
		if (!prim_build_from_mesh_bank(&s_race_finalMesh[i],
		                               &s_race_finalPrims[i],
		                               &s_race_finalPrimCount[i], 1,
		                               RACE_TRK_BANK_BASE)) {
			mrlog_err("race: final prims %d did not build\n", i);
			return;
		}

		/*
		 * REBASE, exactly as the track's own meshes are.
		 *
		 * The plaque .maps name bank 19, a real track page. The
		 * original can take that as an absolute index because its
		 * table IS the manifest - entry N sits in slot N - while this
		 * port starts the manifest at RACE_TRK_BANK_BASE to leave the
		 * menu's pages alone, so every index out of a track .map has
		 * to shift with it.
		 *
		 * WITHOUT IT THE PLAQUE DRAWS A REAL TEXTURE, JUST THE WRONG
		 * ONE - 19 unshifted lands on track page 11 - so it does not
		 * read as a missing step. trk_rebase_banks is the same
		 * correction for the track itself.
		 * [deliberate divergence - our bank slots are offset]
		 */
		/* Applied by the build - prim_build_from_mesh_bank. */
	}

	s_race_finalOk    = 1;
	s_race_finalZ     = RACE_FINAL_Z_START;
	s_race_finalPhase = 0.0f;
	s_race_creditsShown  = 0;
}

static void race_free_final_meshes(void)
{
	int i;

	for (i = 0; i < RACE_FINAL_MESHES; i++) {
		mesh_free(&s_race_finalMesh[i]);
		free(s_race_finalPrims[i]);
		s_race_finalPrims[i]     = NULL;
		s_race_finalPrimCount[i] = 0;
	}
	s_race_finalOk = 0;
}

/*
 * ORIGIN  NONE - ours. Mirrors g_nRaceOverHud @ 0x00509ba0
 *
 * Set the frame the LOCAL racer's lapCounter reaches the lap count, and
 * the only thing the original's HUD dispatcher tests before taking its
 * post-race branch.
 *
 * NOT raceState, WHICH IS PER RACER. The two go true on the same frame
 * for the local racer so the substitution looks right, but every other
 * racer reaches raceState 2 as it finishes - so a viewed AI, or a state
 * carried out of the last race, puts the finishing-position readout on
 * screen mid-race.
 */
int race_over_hud(void)
{
	return s_race_overHud;
}

/*
 * ORIGIN  Race_DrawFinalPlaque @ 0x0047b7a0, the place test
 *
 * -1 when no plaque is due. PRACTICE NEVER GETS ONE - the same
 * g_nSelectedMode 0 that suppresses the countdown - and neither does
 * fourth or worse. Those are exactly the cases the original hands to
 * FUN_00481970 instead, which is why hud.c asks through this and the
 * two displays cannot both appear.
 */
int race_final_place(void)
{
	const MR_Racer *r = race_viewed_racer();

	if (!s_race_finalOk || s_race_finalMesh[0].faceCount == 0)
		return -1;
	if (g_selectedMode == 0 || !r)
		return -1;
	if (r->raceState != RACESTATE_FINISHED)
		return -1;
	if (r->finishPosition < 0 || r->finishPosition >= 3)
		return -1;
	return r->finishPosition;
}

/*
 * ORIGIN  Race_DrawFinalPlaque @ 0x0047b7a0, whole
 *
 * Called from race_draw_3d where the original calls it from
 * maybe_Game_RenderFrame (0x004802bd, unconditional).
 *
 * THE TWO MATRICES ARE CONSTANT-FOLDED IN THE ORIGINAL - it builds
 * both from g_cosTable at fixed indices, where index 0 is 1 and
 * 0x3000 is 0, so most of the terms it spells out multiply away. They
 * are written here as what they are:
 *
 *     wreath   identity, translated to (0, 0, z)
 *     number   a Y rotation by the spin phase, same translation
 *
 * IT FORCES PROJECTION ZOOM 1.0 and g_cfgDetailLevel 3, restoring
 * both. The zoom is what makes the plaque read as screen-filling
 * rather than as an object out in the world - it is why a wreath 2500
 * units away is not a speck.
 *
 * THE WREATH BOBS AND THE NUMBER DOES NOT, because the bobbed value is
 * a local and only the steady one is stored back. Transcribed as
 * written; it reads like an oversight and is not ours to correct.
 *
 * NOT REPRODUCED: the g_zBufferEnabled arm, which re-baselines each
 * mesh against its own minimum z and pins invZ to 1.0. invZ is unused
 * in our back end (drawscene.c), so the arm would be dead code.
 * [deliberate divergence]
 */
/*
 * THE FACING TEST, BECAUSE OUR BACK END DOES NOT CULL. The port's
 * winding test lives in cull_and_queue_faces and queueing a primitive
 * directly goes round it - without this a rotating number draws its
 * own back faces and reads as an inside-out shell.
 * hud_counter_draw_face needs the same step for the same reason.
 *
 * It is cull.c's cross product against the same 1.0f, so backface and
 * degenerate-area rejection stay one comparison.
 *
 * THE ORIGINAL NEEDS NONE OF THIS - its rasterizer drops a back-facing
 * triangle at fill time, which is why it can queue these meshes whole.
 * AND cull_and_queue_faces IS NOT THE ANSWER: it would cull correctly
 * but take the depth from camZ and lose the fixed 16.0 and 13.0.
 */
static int race_final_facing(const MR_Primitive *pr,
                            const MR_Vertex *verts, int n)
{
	const MR_Vertex *a, *b, *c;
	float            facing;

	if (pr->vertexIndex[0] >= (unsigned short)n ||
	    pr->vertexIndex[1] >= (unsigned short)n ||
	    pr->vertexIndex[2] >= (unsigned short)n)
	    return 0;

	a = &verts[pr->vertexIndex[0]];
	b = &verts[pr->vertexIndex[1]];
	c = &verts[pr->vertexIndex[2]];

	facing = (float)((a->screenY - b->screenY) *
	                 (c->screenX - b->screenX) +
	                 (c->screenY - b->screenY) *
	                 (b->screenX - a->screenX));
	return (facing > 1.0f);
}

static void race_draw_final_plaque(float dt)
{
	float      m[12];
	float      zBob, c, sn;
	int        place, saveDetail, i;
	float      saveZoom;
	MR_Vertex *v;
	int        idx;

	place = race_final_place();
	if (place < 0)
		return;

	s_race_finalPhase += dt * (float)RACE_FINAL_SPIN;

	if (!s_race_creditsShown) {
		/*
		 * THE FLY-IN. z eases down from 16000 and is CLAMPED to the
		 * rest height once it passes 2504 - the clamp sits a hair
		 * above the target so a long step cannot straddle it.
		 */
		if (s_race_finalZ > RACE_FINAL_Z_REST) {
			s_race_finalZ += dt * RACE_FINAL_Z_RATE;
			if (s_race_finalZ < RACE_FINAL_Z_CLAMP)
				s_race_finalZ = RACE_FINAL_Z_REST;
		}
		zBob = s_race_finalZ;
		if (s_race_finalZ == RACE_FINAL_Z_REST) {
			zBob = RACE_FINAL_Z_REST +
			       (float)sin((double)s_race_finalPhase *
			                  RACE_FINAL_BOB_RATE) * RACE_FINAL_BOB_AMP;
		}
	} else {
		zBob = s_race_finalZ + dt * RACE_FINAL_Z_EXIT;
		s_race_finalZ = zBob;
		if (s_race_finalZ < 0.0f)
			s_race_finalZ = 0.0f;
	}

	if (s_race_finalZ <= RACE_FINAL_Z_MIN)
		return;

	saveZoom   = screen3d_get_proj_zoom();
	saveDetail = g_cfgDetailLevel;
	screen3d_set_proj_zoom(1.0f);
	g_cfgDetailLevel = RACE_FINAL_DETAIL;

	/* The wreath: identity, at the bobbed distance. */
	memset(m, 0, sizeof m);
	m[0] = m[4] = m[8] = 1.0f;
	m[11] = zBob;
	v = transform_verts_flat(m, (const float *)
	                         s_race_finalMesh[RACE_FINAL_WREATH].vertices,
	                         s_race_finalMesh[RACE_FINAL_WREATH].vertexCount,
	                         RACE_FINAL_SHADE);
	if (v) {
		for (i = 0; i < s_race_finalPrimCount[RACE_FINAL_WREATH]; i++) {
			const MR_Primitive *pr = &s_race_finalPrims[RACE_FINAL_WREATH][i];

			if (!race_final_facing(pr, v,
			        s_race_finalMesh[RACE_FINAL_WREATH].vertexCount))
			    continue;
			queue_primitive((MR_Primitive *)pr, RACE_FINAL_DEPTH_WREATH, v,
			                RACE_FINAL_HWFLAGS, 0);
		}
	}

	/* The number, spun about Y - the original takes cos and sin from
	 * the one table, a quarter turn apart. trig.h. */
	idx = (int)((double)s_race_finalPhase * TRIG_IDX_PER_RAD);
	c   = trig_cos(idx);
	sn  = trig_sin(idx);

	memset(m, 0, sizeof m);
	m[0] = c;    m[2] = -sn;
	m[4] = 1.0f;
	m[6] = sn;   m[8] = c;
	m[11] = s_race_finalZ;
	v = transform_verts_flat(m, (const float *)
	                         s_race_finalMesh[place].vertices,
	                         s_race_finalMesh[place].vertexCount,
	                         RACE_FINAL_SHADE);
	if (v) {
		for (i = 0; i < s_race_finalPrimCount[place]; i++) {
			const MR_Primitive *pr = &s_race_finalPrims[place][i];

			if (!race_final_facing(pr, v,
			        s_race_finalMesh[place].vertexCount))
			    continue;
			queue_primitive((MR_Primitive *)pr, RACE_FINAL_DEPTH_PLACE, v,
			                RACE_FINAL_HWFLAGS, 0);
		}
	}

	screen3d_set_proj_zoom(saveZoom);
	g_cfgDetailLevel = saveDetail;
}

void race_draw_3d(void)
{
	float              lens;
	float              viewEye[3];
	float              pos[3];
	float              target[3];
	float              view[12];
	int                drawn;
	int                props;
	int                meshes;
	int                bikes;
	float              camFwd[3];
	int                camSeg = -1;

	if (!s_race_ok || !s_race_trk.segments)
		return;

	if (!race_camera(0, pos, viewEye, target, &lens))
		return;

	/*
	 * camera_look_at BELOW USES THE PRE-BOOM EYE while the rest of the
	 * frame uses the post-boom one: 0x0046fece runs before 0x0046fed3,
	 * so the original really does build its view matrix at a point it
	 * then moves away from. Reproduced rather than tidied - race.h.
	 */
	if (s_race_cam.viewValid) {
		/*
		 * THE LOOK BRANCH BUILT ITS OWN VIEW and deliberately wrote no
		 * target, so there is nothing here to aim at - camera_look_slot.
		 */
		int vi;

		for (vi = 0; vi < 12; vi++)
			view[vi] = s_race_cam.view[vi];
	} else if (!camera_look_at(view, viewEye, target)) {
		return;
	}

	/*
	 * MODE 6'S ROLL, between Camera_LookAt (0x00470510) and
	 * Matrix34_YawOf - the original's order. It matters only in
	 * principle: the roll mixes x and y and leaves every z alone, so
	 * the yaw below reads the same either way.
	 */
	camera_apply_roll(view, s_race_cam.roll);

	/*
	 * THE YAW THE BILLBOARDS TURN BY, and this is where the original
	 * stores it: Camera_LookAt's last act is to write camera+0xc0
	 * [verified, 0x0046e304]. Our camera is a bare matrix rather than
	 * a record, so it lands in a static instead.
	 */
	s_race_camYaw = camera_view_yaw(view);
	memcpy(s_race_camView, view, sizeof s_race_camView);
	/*
	 * THE POST-BOOM EYE, which is what the rest of the frame uses -
	 * fx_draw_all_particles below takes the same one. The pre-boom
	 * viewEye is camera_look_at's alone; see the note above it.
	 */
	memcpy(s_race_camEyePos, pos, sizeof s_race_camEyePos);

	/*
	 * THE CULL FRUSTUM, once a frame, where the original builds it
	 * into the camera record from maybe_Game_RenderFrame at
	 * 0x00480166. Without it racerdraw_visible has only the distance
	 * test, which almost never fires - so a racer who pulled ahead
	 * was drawn for ever, and the ghost's caption with it.
	 */
	racerdraw_set_frustum(view, pos, RACE_CULL_FAR);

	screen3d_set_proj_base_x(lens);
	screen3d_set_proj_base_y(lens);

	/*
	 * AND THE NEAR PLANE IS THE RACE'S, NOT THE MENU'S.
	 *
	 * ORIGIN  FUN_00410e10 @ 0x00410e10, called once by
	 *         maybe_Game_LoadRace - it writes 64.0 into
	 *         maybe_g_nearPlaneZ (0x00625774) along with the far
	 *         distances and the span-buffer flag.
	 *
	 * Menu_SetPage writes 256.0 into the same global, so without this
	 * the race inherits the MENU'S near plane - four times too far.
	 *
	 * THE CHASE CAMERA HIDES THAT. At 484 units out only the road
	 * immediately under and behind the bike is clipped early, which
	 * reads as nothing much; any camera nearer than 256 - a look slot
	 * is 250 - puts the bike itself inside the plane.
	 *
	 * SET PER FRAME, like the projection base above and the back plane
	 * below: these are globals and whichever of the race and the menu
	 * ran last owns them. The original sets it once at load and never
	 * has a menu page change underneath it. [deliberate divergence]
	 */
	g_nearPlaneZ = RACE_NEAR_PLANE;

	queue_reset();
	vertexpool_reset();

	/*
	 * THE BACK PLANE IS A GLOBAL TOO, set for this frame and cleared at
	 * the end. Left set it follows the frame out into whatever menu
	 * page draws next, whose objects sit near 1500 - so nothing looks
	 * wrong, which is how that leak stays hidden until it does not.
	 */
	/*
	 * DRAW DISTANCE IS A RING WINDOW, NOT A CLIP. trk_draw selects
	 * world meshes the way 0x0040ce60 does - the camera's mesh plus a
	 * span each way along the track - so the normal path has no
	 * distance test, and neither does the original.
	 *
	 *     [RENDER] drawdistance   0 or absent = the ring window
	 *                                           alone, ie retail
	 *                            -1           = NO window - every
	 *                                           mesh, nearest first,
	 *                                           no clip
	 *                             N > 0       = the window AND a
	 *                                           clip at N
	 *
	 * -1 IS A DEBUG VIEW for a flyover or a screenshot, and is not
	 * what retail does.
	 */
	{
		int dd = 0;             /* absent = the manifest's value */

		get_ini_value("RENDER", "drawdistance", &dd);
		s_race_backPlane = (dd > 0) ? (float)dd : 0.0f;
		s_race_drawAll   = (dd < 0);
	}
	/*
	 * AND THE QUEUE HAS TO REACH THAT FAR. Its buckets are one per
	 * unit out to 7998 and a single clamp bucket beyond, so drawing
	 * past 7998 without widening them piles unordered geometry into
	 * one bucket - which is what painted the whole frame over.
	 */
	{
		float reach = (s_race_backPlane > 0.0f)
		              ? s_race_backPlane : g_drawSceneZFar;
		float upb   = reach / (float)QUEUE_BUCKET_MAX;

		queue_set_units_per_bucket((upb > 1.0f) ? upb : 1.0f);
	}
	g_backPlaneZ = s_race_backPlane;
	/*
	 * BIKES FIRST, WITH HALF THE BUDGET. A full field of articulated
	 * skeletons at 340 primitives each would eat the whole queue on
	 * its own; this caps them and lets trk_draw spend the rest.
	 */
	/*
	 * THE VISIBLE SEGMENT LIST GOES FIRST, as in
	 * maybe_Game_RenderFrame: the zone call at 0x0048022c, then
	 * maybe_Race_DrawRacers at 0x00480263 with its results.
	 *
	 * THE CAMERA SEGMENT hint is the player's segment where the
	 * original passes a per-camera one. The hint only BOUNDS the
	 * search; it does not change which segment is found.
	 *
	 * THE CAMERA FORWARD IS THE VIEW MATRIX'S THIRD COLUMN - the
	 * original transforms (0,0,1) by the view's inverse, which for a
	 * rigid matrix is that column. camera.txt 1.5 confirms it
	 * independently: the near-plane normal equals it bit for bit.
	 */
	{
		camFwd[0] = view[2];
		camFwd[1] = view[5];
		camFwd[2] = view[8];

		camSeg = trk_find_segment_for_point(&s_race_trk,
		             s_race_racers[s_race_localIndex].segmentStart, pos, NULL);

		s_race_segCount = trk_list_visible_segments(&s_race_trk, camSeg,
		                      camFwd, s_race_segList,
		                      TRK_SEGMENT_LIST_MAX);
	}

	bikes = race_draw_racers(view, pos, TRK_DRAW_PRIM_BUDGET / 2,
	                         s_race_segList, s_race_segCount);
	/*
	 * THE propClip COMPUTED HERE IS ALMOST ALWAYS DISCARDED.
	 * trk_draw zeroes it whenever it is given a segment list,
	 * because the ring window already bounds decor along the
	 * TRACK the way the original does. What is passed matters
	 * only on the drawAll path and if the instance count ever
	 * exceeds TRK_DRAW_MAX_MESHES, where the segment branch is
	 * skipped. drawdistance still overrides.
	 */
	drawn = trk_draw(&s_race_trk, view, pos,
	                 s_race_backPlane,
	                 s_race_drawAll ? 0.0f
	                     : (s_race_backPlane > 0.0f
	                        ? s_race_backPlane
	                        : s_race_level.lenBPlane[0]),
	                 s_race_drawAll ? -1 : camSeg, camFwd,
	                 s_race_drawAll ? NULL : s_race_segList,
	                 s_race_drawAll ? 0 : s_race_segCount,
	                 &props, &meshes);
	/*
	 * PARTICLES GO BETWEEN THE RACERS AND THE QUEUE WALK, where
	 * maybe_Game_RenderFrame puts maybe_Fx_DrawAllParticles.
	 *
	 * g_cfgDetailLevel IS 0x004dc098, the same global the original
	 * reads at 0x004b0c03 - detail.c holds that one, not a parallel
	 * copy of it.
	 */
	fx_draw_all_particles(view, pos, s_race_camYaw, g_cfgDetailLevel);

	/*
	 * AND THE SKID RIBBONS beside them - FUN_004573e0 at 0x0048027e.
	 * It takes the camera rather than a prepared view because it folds
	 * the eye in per ribbon.
	 */
	fx_ribbon_draw_all(view, pos);

	/*
	 * AND THE INSTRUMENT NEEDLES LAST, so they land in front of
	 * everything - screen-space primitives in the same queue, as the
	 * original does. hud-counter.txt 8.
	 */
	hud_counter_draw_face();
	hud_counter_draw_needles();

	queue_walk(drawscene_dispatch);

	/*
	 * THE FIRST-PERSON COCKPIT, IN ITS OWN PASS WITH DEPTH OFF.
	 *
	 * Screen3D_CullAndQueueTranslucent keeps it in front by compressing
	 * its SORT KEY, which is an ordering trick a z-buffer cannot
	 * express - depth-tested on the true camZ the road interleaves with
	 * it and the cockpit is buried in the road surface. It must be a
	 * PASS, as the plaque and the map below are.
	 *
	 * THE KEY COMPRESSION STAYS: monotonic within the pass, so the two
	 * meshes still sort against each other, and it drops the same near
	 * faces the original drops. racer-draw-pipeline.txt 5.4.5.
	 */
	if (s_race_fpActive && s_race_cockpitOk) {
		const MR_Racer *vr = race_viewed_racer();

		if (vr) {
			queue_reset();
			drawscene_set_depth_test(0);
			racerdraw_cockpit(vr, vr->wheelieAngle,
			                  &s_race_cockpitSet);
			queue_walk(drawscene_dispatch);
			drawscene_set_depth_test(1);
		}
	}

	/*
	 * THE END-OF-RACE PLAQUE, IN ITS OWN PASS WITH DEPTH OFF.
	 *
	 * The original queues it with the scene (0x004802bd) and pins every
	 * vertex's invZ to 1.0 when the z-buffer is on - the same trick as
	 * the map below - which leaves the QUEUE'S BUCKETS to order the two
	 * meshes, so the wreath at 16.0 always paints behind the number at
	 * 13.0.
	 *
	 * ON REAL DEPTH THEY INTERSECT EVERY CYCLE: the wreath's bob
	 * carries it from 2372 to 2628 through the number's fixed 2500.
	 * invZ is unused here, so the pass is what replaces the pin.
	 * [deliberate divergence]
	 */
	queue_reset();
	drawscene_set_depth_test(0);
	race_draw_final_plaque(race_frame_delta());
	queue_walk(drawscene_dispatch);
	drawscene_set_depth_test(1);

	/*
	 * THE MAP, ON TOP OF THE SCENE AND WITH DEPTH OFF.
	 *
	 * The original queues it with the rest and pins every map vertex's
	 * invZ to 1.0 - the nearest value there is - whenever the z-buffer
	 * is on (0x0040f6a0). invZ does nothing here, so the pass with the
	 * depth test off is what buys the same thing.
	 *
	 * DEPTH-TESTED ON REAL z THE MAP LOSES TO THE ROAD: it is placed at
	 * z 4000 and the road is a few hundred units away and fills the
	 * bottom of the screen, so only whatever clears the horizon shows.
	 *
	 * AND camZ CANNOT JUST BE MOVED NEARER - it is also the projection
	 * divisor, so the map would shrink and slide as it came forward.
	 *
	 * queue_reset CLEARS BUCKETS ONLY - the vertex pool is reset once a
	 * frame, above the world walk, and has room for the extra passes.
	 * [deliberate divergence - our renderer ignores invZ]
	 */
	queue_reset();
	drawscene_set_depth_test(0);
	hud_draw_map();
	queue_walk(drawscene_dispatch);
	drawscene_set_depth_test(1);
	g_backPlaneZ = 0.0f;

	if (!s_race_viewLogged) {
		s_race_viewLogged = 1;
		mrlog("race: track view - eye (%.0f %.0f %.0f) at "
		      "(%.0f %.0f %.0f)\n",
		      pos[0], pos[1], pos[2],
		      target[0], target[1], target[2]);
		/*
		 * COUNTED SEPARATELY ON PURPOSE - trk_draw returns props and
		 * world meshes, and one combined number against meshCount
		 * alone reads far healthier than it is.
		 */
		mrlog("race: drew %d/%d world meshes, %d/%d props, "
		      "%d bikes\n",
		      meshes, s_race_trk.meshCount,
		      props, s_race_trk.instanceCount, bikes);
	}
}

/* ------------------------------------------------------------------ */

/* ================================================================== */
/* THE TRACK RECORD - Data\bestscor.dat                               */
/*                                                                    */
/* ORIGIN  FUN_004656c0 @ 0x004656c0, the read half                   */
/* ================================================================== */

/*
 * THE FILE, verified against decomp/DATA/BESTSCOR.DAT - whose LAYOUT
 * is evidence and whose TIMES are not, because the game writes this
 * one and 9 of its 131 entries have been played against
 * (tools/bestscor.py):
 *
 *     char magic[4]                    "00.1"
 *     repeated to EOF:
 *         char key[0x20]               the track key, NUL padded
 *         Record best                  0xf9 bytes
 *         Record table[8]              0xf9 bytes each
 *
 *     Record:                          0xf9 bytes
 *         char holder[9]     at +0x00   the name the game ships
 *         char initials[]    at +0x09   the three letters the player
 *                                       types, and what a row draws
 *         u32  raceMs        at +0xf1
 *         u32  lapMs         at +0xf5
 *
 * so one entry is 0x20 + 9 * 0xf9 = 0x8e1 bytes and the search is a
 * linear walk comparing key against the one built below.
 * bestscor.txt 1.
 *
 * o THE KEY IS NOT JUST THE TRACK NAME. The original appends "POC"
 *   for the pocket bike, "REV" for a reversed track, and in mode 0
 *   "TRA" plus g_nRaceLaps - in that order, so a key is
 *   <track>[POC][REV][TRA<laps>] and a record is per track AND bike
 *   class AND direction AND lap count. Ours builds the plain name,
 *   which matches only a forward superbike race and finds nothing
 *   for the other three. [open - the port, not the format;
 *   bestscor.txt 2]
 *
 * o IT IS A TOP-NINE TABLE. Record 0 is the best lap and 1 onward are
 *   the placings; race_load_track_record takes 0 and as many of the
 *   rest as RACE_RESULT_ROWS asks for.
 *
 * o THE SENTINEL IS 0x7f7f7f7f, the same one every other reader
 *   tests. A miss leaves the name empty and the time at it, which is
 *   what makes the BEST LAP block draw no record row rather than a
 *   zero one.
 */
#define RACE_REC_MAGIC     "00.1"
#define RACE_REC_KEY_LEN   0x20
#define RACE_REC_SIZE      0xf9
#define RACE_REC_TIME_OFF  0xf5
#define RACE_REC_PER_KEY   9
#define RACE_REC_NO_TIME   0x7f7f7f7f

static MR_TrackRecord s_race_record;
static MR_RaceResultRow s_race_rows[RACE_RESULT_ROWS];
static MR_RaceResultRow s_race_bestLap;

#define RACE_REC_INIT_OFF   9
#define RACE_REC_RACE_OFF   0xf1

static int race_rec_i32(const unsigned char *rec, int off)
{
	return (int)((unsigned int)rec[off]
	             | ((unsigned int)rec[off + 1] << 8)
	             | ((unsigned int)rec[off + 2] << 16)
	             | ((unsigned int)rec[off + 3] << 24));
}

static void race_rec_to_row(const unsigned char *rec,
                            MR_RaceResultRow *row)
{
	unsigned int n;

	memset(row, 0, sizeof *row);
	for (n = 0; n + 1 < sizeof(row->name) &&
	            rec[RACE_REC_INIT_OFF + n]; n++) {
		row->name[n] = (char)rec[RACE_REC_INIT_OFF + n];
	}
	row->name[n] = 0;
	row->raceMs  = race_rec_i32(rec, RACE_REC_RACE_OFF);
	row->lapMs   = race_rec_i32(rec, RACE_REC_TIME_OFF);
	row->valid   = ((unsigned int)row->lapMs != RACE_REC_NO_TIME)
	               && ((unsigned int)row->raceMs
	                   != RACE_REC_NO_TIME);
}

/* ORIGIN  FUN_004656c0 @ 0x004656c0 */
static void race_load_track_record(const MR_Track *t)
{
	unsigned char *blob = NULL;
	unsigned int   size = 0;
	unsigned int   off;
	unsigned int   stride;

	memset(&s_race_record, 0, sizeof(s_race_record));
	s_race_record.timeMs = RACE_REC_NO_TIME;
	memset(s_race_rows, 0, sizeof s_race_rows);
	memset(&s_race_bestLap, 0, sizeof s_race_bestLap);

	if (!t)
		return;

	blob = (unsigned char *)asset_load("BESTSCOR.DAT", &size);
	if (!blob) {
		mrlog("race:   no BESTSCOR.DAT - no track record\n");
		return;
	}

	stride = RACE_REC_KEY_LEN + RACE_REC_PER_KEY * RACE_REC_SIZE;

	if (size < 4 || memcmp(blob, RACE_REC_MAGIC, 4) != 0) {
		mrlog_err("race:   BESTSCOR.DAT magic is wrong\n");
		free(blob);
		return;
	}

	for (off = 4; off + stride <= size; off += stride) {
		const unsigned char *key = blob + off;
		const unsigned char *rec = key + RACE_REC_KEY_LEN;
		unsigned int         n;

		if (strncmp((const char *)key, t->name,
		            RACE_REC_KEY_LEN) != 0) {
			continue;
		}

		for (n = 0; n + 1 < sizeof(s_race_record.name) &&
		            rec[n]; n++) {
			s_race_record.name[n] = (char)rec[n];
		}
		s_race_record.name[n] = 0;
		s_race_record.timeMs  =
		    (int)((unsigned int)rec[RACE_REC_TIME_OFF]
		          | ((unsigned int)rec[RACE_REC_TIME_OFF + 1] << 8)
		          | ((unsigned int)rec[RACE_REC_TIME_OFF + 2] << 16)
		          | ((unsigned int)rec[RACE_REC_TIME_OFF + 3] << 24));
		s_race_record.valid   = 1;

		/*
		 * RECORD 0 HAS NO RACE TIME - its raceMs is the sentinel in
		 * every DSI row in the shipped file - so its valid is retaken
		 * on the lap time alone where a table row needs both.
		 */
		race_rec_to_row(rec, &s_race_bestLap);
		s_race_bestLap.valid =
		    ((unsigned int)s_race_bestLap.lapMs
		     != RACE_REC_NO_TIME);
		for (n = 0; n < RACE_RESULT_ROWS &&
		            (int)(n + 1) < RACE_REC_PER_KEY; n++) {
			race_rec_to_row(rec + (n + 1) * RACE_REC_SIZE,
			                &s_race_rows[n]);
		}
		break;
	}

	free(blob);

	if (s_race_record.valid) {
		mrlog("race:   track record %s %d ms (%s)\n",
		      s_race_record.name, s_race_record.timeMs, t->name);
	} else {
		mrlog("race:   no record for %s\n", t->name);
	}
}

/* ORIGIN  NONE - ours. Row i of the top five, NULL when empty. */
const MR_RaceResultRow *race_result_row(int i)
{
	if (i < 0 || i >= RACE_RESULT_ROWS || !s_race_rows[i].valid)
		return NULL;
	return &s_race_rows[i];
}

/* ORIGIN  NONE - ours. Record 0, the track's best lap. */
const MR_RaceResultRow *race_best_lap_record(void)
{
	return s_race_bestLap.valid ? &s_race_bestLap : NULL;
}

/* ORIGIN  NONE - ours, the seam hud.c reads. NULL when there is none. */
const MR_TrackRecord *race_track_record(void)
{
	if (!s_race_record.valid ||
	    (unsigned int)s_race_record.timeMs == RACE_REC_NO_TIME) {
		return NULL;
	}
	return &s_race_record;
}

int race_load(int trackIndex)
{
	const MR_Track *t = track_at(trackIndex);
	char            name[MAX_PATH];

	race_free();

	/*
	 * "Level.Bpt", NOT "Level.Spt". The original asks for the .Spt
	 * and Script_Open swaps the extension; bpt_open here deliberately
	 * does not, so callers name the file they actually want.
	 */
	if (!t || !track_asset_name(t, "Level.Bpt", name, (int)sizeof(name)) ||
	    !level_load(&s_race_level, name)) {
		mrlog_err("race: level manifest did not load\n");
		return 0;
	}
	level_log(&s_race_level, t->name);

	/*
	 * g_raceLaps, NOT t->laps - THE MENU OWNS THE DISTANCE. Practice
	 * lets page 6 set it; single race and championship force it to the
	 * track default on every tick of that page, so the global is the
	 * track value there and the chosen one in practice. t->laps here
	 * would ignore the selection and run every practice at the
	 * default. g_nRaceLaps in the original.
	 */
	s_race_laps = (g_raceLaps > 0) ? g_raceLaps : t->laps;
	race_load_track_record(t);

	/*
	 * AND THE TRACK ITSELF. Its name comes out of the manifest and is
	 * NOT ileNN.trk on three of the eight islands - see
	 * level-spt.txt section 4.
	 *
	 * THE trk_log BELOW IS A KNOWN-ANSWER TEST: trk.txt 7 tabulates
	 * segments, meshes, vertices, faces, named, instances and anim for
	 * all ten shipped tracks, so its line reads straight against that.
	 */
	if (!track_asset_name(t, s_race_level.land, name, (int)sizeof(name)) ||
	    !trk_load(&s_race_trk, name)) {
		mrlog_err("race: track %s did not load\n", s_race_level.land);
		return 0;
	}
	trk_log(&s_race_trk, s_race_level.land);

	/*
	 * THE MAP, built from the segments as Trk_Load does. The reset must
	 * come first - hud.c caches the map's primitives, and they belong
	 * to the track being replaced.
	 */
	hud_map_reset();
	trk_build_map(&s_race_trk);

	/*
	 * The textures, then move every face's bankIndex into our slot
	 * pool. Slots 0..4 belong to the menu pages and the glass, so the
	 * track's twenty start at RACE_TRK_BANK_BASE.
	 */
	level_load_banks(&s_race_level, t, RACE_TRK_BANK_BASE);
	trk_rebase_banks(&s_race_trk, RACE_TRK_BANK_BASE);
	horizon_load(t, RACE_HORIZON_BANK);

	s_race_ok = 1;
	s_race_viewLogged = 0;

	/*
	 * SEED BEFORE ANYTHING DRAWS. race_build_grid shuffles the
	 * opponent speeds with rand(), so seeding later - in
	 * race_replay_reset, where the rest of the replay state is
	 * cleared - leaves that shuffle on whatever the stream held and
	 * a playback gets a different field. See race_seed_sim.
	 */
	race_seed_sim();

	/*
	 * BEFORE race_build_grid, WHICH READS race_ghost_has_lap to
	 * decide whether slot 2 is active. The original splits this
	 * across two functions and keeps the same order:
	 * maybe_Game_LoadRace empties the playback stream and then
	 * loads the saved lap off disk at 0x0047b42f, and only
	 * maybe_Game_InitRace - later - rewinds it and sets
	 * racer[2].active from the count it found.
	 */
	race_ghost_load_race();
	race_ghost_reset();

	race_build_grid();

	/*
	 * THE TRACK'S OWN SOUNDS. Sound.Spt in the original, SOUND.BPT on
	 * disk - the loader rewrites the extension. A track without one is
	 * silent and that is not an error, so neither call may fail the
	 * load.
	 *
	 * IT MUST COME AFTER race_build_grid, WHICH IS NOT VISIBLE FROM
	 * HERE. The grid build changes the sample bank, and on the way it
	 * calls race_audio_stop - which frees every loaded sample AND
	 * calls objsnd_reset. Loading first loses both the samples and the
	 * bindings a few lines later.
	 *
	 * AND THE LOG WILL NOT SHOW IT: both objsnd lines print before the
	 * teardown, so a silent run still reports its descriptors and its
	 * bound objects. Silence is the only symptom.
	 */
	if (track_asset_name(t, "SOUND.BPT", name, (int)sizeof(name))) {
		objsnd_load(name);
		objsnd_bind(&s_race_trk);
	}
	camera_reset(&s_race_cam);

	/* Slot 1 is the local player's - see race_build_grid. */
	s_race_segPos  = (float)s_race_grid[1].segment;
	s_race_lateral = (float)s_race_grid[1].lateral;
	s_race_lastTime = 0.0;

	/*
	 * SEED THE HEADING AND THE TRAVEL DIRECTION FROM THE GRID POSE.
	 * The yaw comes off the grid matrix so yawRel is 0 and the bike
	 * sets off down the track rather than across it; moveDir is a
	 * VELOCITY - r->speed is its magnitude - so left zero the first
	 * frame has neither a speed nor a direction to lean against.
	 */
	{
		const MR_Racer *me = &s_race_racers[s_race_localIndex];

		s_race_yawWorld = racer_matrix_yaw(me->matOrientation);
	}

	memcpy(s_race_racers[s_race_localIndex].moveDir,
	       &s_race_racers[s_race_localIndex].matOrientation[6],
	       sizeof s_race_racers[s_race_localIndex].moveDir);

	/*
	 * NO-WALLS IS DELIBERATELY NOT RESET HERE. race_load runs on every
	 * restart, so clearing it would put the rails back each time and a
	 * comparison run would silently get them halfway through. The
	 * static is 0 at launch, so walls are ON for anyone who never asks
	 * otherwise. [deliberate divergence]
	 */

	get_ini_value("DEBUG", "inputunfocused", &s_race_inputUnfocused);
	s_race_collideInPractice = 0;
	get_ini_value("DEBUG", "collideinpractice",
	              &s_race_collideInPractice);
	s_race_unitsPerMetre = RACE_UNITS_PER_METRE_DEFAULT;
	{
		int u = (int)RACE_UNITS_PER_METRE_DEFAULT;

		if (get_ini_value("DEBUG", "unitspermetre", &u) && u > 0)
			s_race_unitsPerMetre = (float)u;
	}

	memset(&s_race_drive, 0, sizeof s_race_drive);
	memset(&s_race_gearbox, 0, sizeof s_race_gearbox);
	s_race_prevLongSpeed = 0.0f;
	s_race_gearbox.gear = 1;
	/*
	 * THE GEARBOX MODE IS THE BIKE PAGE'S AUTO/MANUAL BUTTON.
	 *
	 * ORIGIN  FUN_00411010 @ 0x00411010, the racer loop at
	 *         0x004110c4
	 *
	 * racer+0x88 is written once per racer as the race starts and is
	 * NOT the same value for everyone: 1 for every racer but the local
	 * one, g_autoGearbox for the local one solo, the player record on a
	 * network game.  So AN AI NEVER RIDES A MANUAL GEARBOX - which is
	 * why one gearbox costs nothing here, and why this global is the
	 * wrong thing to reach for if AI gearboxes ever arrive.
	 * racers.txt 9.33.2.
	 *
	 * IT IS STORED TWICE BECAUSE THE PORT SPLIT THE STRUCT: MR_Racer is
	 * byte-exact, so racer+0x88 is f_0088 and the manual latch reads it
	 * there, while MR_Gearbox is ours and carries the same field as
	 * mode, which the clutch timers and the torque multiplier read.
	 * ONE ORIGINAL FIELD, TWO HOMES - assign them together.
	 */
	s_race_gearbox.mode        = g_autoGearbox;
	s_race_racers[s_race_localIndex].f_0088    = g_autoGearbox;

	/* The same bike the rest of race.c draws - page 10 picked it. */
	s_race_racers[s_race_localIndex].bikeIndex = g_selectedBike;

	/*
	 * AND THE SPEED CEILING, PER RACER, BECAUSE IT IS THE RIDER'S AND
	 * NOT THE BIKE'S.
	 *
	 * ORIGIN  Racer_Create @ 0x0045c569 for the copy,
	 *         0x0045c594 for the bonus
	 *
	 * Each racer copies its OWN carac field, and a manual rider then
	 * gets K_MANUAL_SPEED_BONUS more of it, gated on humanPlayer, the
	 * solo flag and g_autoGearbox == 0.  Only the bonus is local.
	 *
	 * EVERY RACER, NOT JUST THE LOCAL ONE.  race_tick_all dispatches
	 * all of them through racer_apply_drive, so an unseeded AI meets
	 * the clamps with a ceiling of ZERO and stands still.
	 *
	 * THE INSTANCE COPY AND THE CARAC ARE BOTH LIVE AND DIFFER BY THE
	 * BONUS.  Every clamp on the instance reads this copy;
	 * Gearbox_BuildShiftTable reads the carac.  Shift points belong to
	 * the bike, the ceiling to whoever is riding it - physics.c splits
	 * the same way, and racers.txt 9.33.2.1 lists all five consumers
	 * with their addresses.
	 *
	 * THE PROTECTION ZEROES THIS FIELD to pin a pirated bike at a
	 * standstill (copy-protection.txt 5.1).  The payload is not
	 * reproduced - a rebuilt executable takes the pass branch.
	 */
	{
		int i;

		for (i = 0; i < RACER_MAX; i++) {
			MR_Racer           *rr = &s_race_racers[i];
			const MR_BikeCarac *bc = bike_at(rr->bikeIndex);

			if (i < 2 || i >= s_race_racerCount)
				rr->realSpeedMax = bc ? bc->realSpeedMax
			                          : 0.0f;
			if (rr->humanPlayer && rr->soloMode &&
			    g_autoGearbox == 0)
			    rr->realSpeedMax += K_MANUAL_SPEED_BONUS;
		}
	}

	/* Once, here: it depends only on the bike. The original builds it
	 * per racer, the same way. */
	{
		const MR_BikeCarac *gc =
		    bike_at(s_race_racers[s_race_localIndex].bikeIndex);

		gearbox_build_shift_table(gc, s_race_shiftTable);
		if (gc)
			mrlog("race: shift table %.1f %.1f %.1f %.1f %.1f "
		          "(nbSpeed %.0f)\n",
		          s_race_shiftTable[1], s_race_shiftTable[2],
		          s_race_shiftTable[3], s_race_shiftTable[4],
		          s_race_shiftTable[5], gc->nbSpeed);
	}
	mrlog("race: drivetrain drive, %.0f units/metre\n",
	      s_race_unitsPerMetre);
	s_race_overHud = 0;
	race_net_reset();
	race_post_race_reset();
	race_replay_reset();
	/* Per track, and a track without them just gets no plaque. */
	race_load_final_meshes(t);

	mrlog("race: player starts at segment %.0f lane %.0f\n",
	      s_race_segPos, s_race_lateral);
	return 1;
}

void race_free(void)
{
	/*
	 * THE IN-RACE MENU GOES WITH THE RACE, and leaving it open is worse
	 * than it looks.  Quitting keeps the race ticking for the frames a
	 * page transition takes, and race_post_race_tick re-raises the
	 * prompt every one of them, so its open flag outlives the race.
	 *
	 * NOTHING DRAWS IT THEN - hud_draw needs race_ready - BUT
	 * hud_menu_overlay_rect STILL REPORTS A RECT, and main.cpp's
	 * compositor lifts the last frame's overlay pixels onto whatever
	 * page drew next.  A ghost of the prompt, every frame, until
	 * something else opened the menu.
	 */
	hud_menu_close();

	/*
	 * THE ENGINE NOTE FIRST: it is a LOOPING voice, so nothing ends it
	 * on its own and a race torn down while it sounds keeps rumbling
	 * over the menus until something takes its slot.
	 */
	race_audio_stop();
	trk_free(&s_race_trk);
	race_free_final_meshes();
	horizon_free();
	camera_reset(&s_race_cam);
	s_race_ok = 0;
	s_race_viewLogged = 0;
	s_race_racerCount = 0;
}

int race_ready(void)
{
	return s_race_ok && s_race_trk.segments != 0;
}

/*
 * ORIGIN  NONE - ours. The original reads g_racers[g_viewedRacerIndex]
 *         inline wherever it wants it.
 *
 * THE LOCAL RACER, which on a guest is NOT slot 1. Nothing here views
 * another one - the replay plays the local racer back rather than
 * following the field.
 * The indirection keeps that out of the HUD and leaves one place to
 * change if a camera ever views somebody else.
 */
/* ORIGIN  g_viewedRacerIndex, 0x00508834. NOT 1 - a literal 1 shows
 * a guest the HOST'S position and lap times. hud-race.txt 7.2. */
int race_viewed_index(void)
{
	return s_race_localIndex;
}


const MR_Racer *race_viewed_racer(void)
{
	if (!race_ready() || s_race_racerCount < 2)
		return NULL;
	return &s_race_racers[s_race_localIndex];
}

/* ================================================================== */
/* THE RACE CLOCK                                                     */
/* ================================================================== */

/*
 * THE GRID COUNTDOWN, 0x0041129d. Six seconds, a literal, written by
 * the race-start function along with g_bPreRace.
 */
#define RACE_GRID_COUNTDOWN_MS 6000

/*
 * THE LONGEST PHYSICS STEP race_update will take, in seconds, and the
 * most slices it will cut one frame into.
 *
 * 0.02 is a shade over a 60 fps frame, so at normal speed nothing
 * substeps and the behaviour is unchanged.  32 slices gives 0.64 s a
 * frame - about 38x at 60 fps - and costs 32 physics ticks only on the
 * frames that use them all.
 *
 * THE FRAME CLAMP IS DERIVED FROM THESE, NEVER GIVEN ITS OWN NUMBER.
 * The invariant is that no step exceeds RACE_MAX_SUBSTEP, so a frame
 * may carry exactly what the slices cover - and an independent clamp
 * SILENTLY CAPS FAST-FORWARD instead: dt is the multiplier over the
 * frame rate, so 0.25 s means nothing above 15x at 60 fps, with 20x
 * and 50x both arriving as 15x and no warning either way.
 *
 * A clamp is needed at all because one huge step teleports the bike.
 * Substepping is what prevents that, so the clamp follows the substep
 * budget rather than second-guessing it.
 */
#define RACE_MAX_SUBSTEP   0.02f
#define RACE_MAX_SUBSTEPS  32
#define RACE_MAX_FRAME_DT  (RACE_MAX_SUBSTEP * RACE_MAX_SUBSTEPS)

/*
 * The per-difficulty bonus added to the time limit in solo, the table
 * of three dwords at 0x00515fe0 [verified, read live]. Network adds a
 * flat 120000 instead, at 0x0047dda6.
 */
static const int kRaceDifficultyBonusMs[3] = { 25000, 20000, 15000 };

int                 g_extendedTimeMs;
static MR_RaceClock s_race_clock;
static int          s_race_lapTimes[RACER_MAX * RACE_LAP_SLOTS];

/*
 * ORIGIN  Trk_Load @ 0x0040d650, at 0x0040dfb3
 *
 * THE CHECKPOINT TIMES IN THE .trk ARE OVERWRITTEN, and this is the
 * single most surprising thing about the race clock.
 *
 * Every segment carries two fields the port already parses as linkA
 * (+0xd8, u16) and linkB (+0xda): linkA is a CHECKPOINT INDEX and is
 * 0xffff on an ordinary segment, and linkB is a time. ile01 has three
 * such segments - 0, 319 and 539, indices 0, 1, 2.
 *
 * BUT THE FILE'S linkB IS NOT USED. Trk_Load walks the segments in
 * order and, for each one whose linkA is not 0xffff, overwrites linkB
 * from a global table at 0x00509228 which the LEVEL.BPT reader filled
 * from TimeCheckPoint[]. ile01's file says 30, 30, 30; LEVEL.BPT says
 * 15000, 15000, 13000, and 15000 is what a running MOTO holds.
 *
 * o SO THE .trk's OWN NUMBERS ARE DEAD DATA - probably seconds from
 *   an older authoring pass. A reconstruction that trusts them gives
 *   a thirty MILLISECOND race.
 *
 * The original does this inside the track loader against a global the
 * level loader already filled. Ours does it here, where both are in
 * scope, which is the same thing without the global.
 */
static void race_apply_checkpoint_times(void)
{
	int i;
	int n = 0;

	s_race_cpSegCount = 0;

	if (!s_race_trk.segments || s_race_level.nbCheckPoint <= 0)
		return;

	for (i = 0; i < s_race_trk.segmentCount; i++) {
		if ((short)s_race_trk.segments[i].linkA < 0)
			continue;
		s_race_cpSegCount++;
		if (n >= s_race_level.nbCheckPoint)
			continue;
		s_race_trk.segments[i].linkB =
		    (unsigned int)s_race_level.timeCheckPoint[n++];
	}

	mrlog("race:   %d checkpoint times applied over %d timing "
	      "segment(s), first %u ms\n", n, s_race_cpSegCount,
	      (unsigned int)(s_race_trk.segments[0].linkB));
}

/*
 * ORIGIN  maybe_Race_TickClock @ 0x0047dae0, at 0x0047dd67
 *
 * The limit the grid transition loads:
 *
 *     if ((short)startSeg->linkA < 0) leave the timer negative
 *     timer = startSeg->linkB + bonus
 *
 * A NEGATIVE linkA SKIPS THE RELOAD, and that is the whole of it: the
 * timer keeps the small negative value it crossed zero with and goes
 * on down. -1 here means "do not reload", not "untimed".
 *
 * WHAT MAKES A RACE UNTIMED IS THE MODE GATE in race_clock_tick's
 * expiry arm, not this. A negative linkA in a TIMED mode still times
 * the player out, about 500 ms after the grid clears, because the arm
 * only asks whether the timer is negative. Reading this branch as the
 * untimed case is the mistake it invites - and the -1 this returns
 * reaches race_clock_start as hasTimeLimit 0, which skips the reload
 * and NOTHING else, exactly as the original's JL does.
 *
 * Every shipped track has 0 on its start segment, so the gate never
 * fires on real data. The test is a SIGNED 16-bit compare against a u16
 * field (0x0047dd6c), which is what the cast below is for.
 */
static int race_time_limit_ms(void)
{
	const MR_TrkSegment *start;
	int                  bonus;
	int                  diff;

	if (!s_race_trk.segments || s_race_trk.segmentCount <= 0)
		return -1;

	start = &s_race_trk.segments[0];
	if ((short)start->linkA < 0)
		return -1;

	diff = g_options.difficulty;
	if (diff < 0 || diff > 2)
		diff = 1;
	bonus = kRaceDifficultyBonusMs[diff];

	return (int)start->linkB + bonus;
}

/*
 * ORIGIN  NONE - ours. g_bPreRace, 0x004dc0b4, as the rest of the
 * port has to see it.
 *
 * THE GRID HOLD IS THIS ONE BIT: racer_apply_drive forces the brake on
 * while it is set, and racer_tick_ai suppresses the wheelie, the
 * steering and the drive. A caller that passes a literal 0 instead
 * launches the whole field on the first frame of the countdown.
 */
int race_pre_race(void)
{
	return (race_ready() && s_race_clock.preRace) ? 1 : 0;
}

/* ORIGIN  NONE - ours, the seam hud.c reads. */
const MR_RaceClock *race_clock(void)
{
	if (!race_ready())
		return NULL;
	return &s_race_clock;
}

/*
 * ORIGIN  FUN_00411010 @ 0x00411010, at 0x0041127b
 *
 * The race-start arm: preRace on, the six-second countdown loaded,
 * every lap time cleared.
 *
 * DAT_004dc0ac IS (g_nSoloMode != 0) - written at 0x00410206 from the
 * same test that sets g_bPreRace, and again unconditionally for a
 * network race at 0x004111d7. Always 1 in this solo-only port, so the
 * whole clock runs.
 */
static void race_clock_reset(void)
{
	memset(&s_race_clock, 0, sizeof(s_race_clock));
	memset(s_race_lapTimes, 0, sizeof(s_race_lapTimes));

	s_race_clock.enabled = 1;
	s_race_clock.preRace = 1;
	s_race_clock.timerMs = RACE_GRID_COUNTDOWN_MS;
	g_extendedTimeMs     = 0;

	/*
	 * [OPTIONS_CUSTOM] SkipCountdown - AN OVERRIDE, NOT A BYPASS, and
	 * that is why it is written this way. Zeroing the timer makes the
	 * NEXT tick take it negative, so the real transition at
	 * race_clock_tick step 3 fires: race_clock_start runs, raceState is
	 * assigned to every racer, the per-difficulty limit is loaded, and
	 * preRace clears through the one path that clears it. Only the six
	 * seconds are gone.
	 */
	if (g_options.skipCountdown)
		s_race_clock.timerMs = 0;
	s_race_standingsLogged = 0;
	hud_countdown_reset();
	autopilot_reset();
}

/* ================================================================== */
/* PROGRESS AND STANDINGS.                                            */
/* ================================================================== */

/*
 * ORIGIN  maybe_Race_UpdateProgress @ 0x0047e3d0 - A SKELETON
 *
 * The original is 2156 bytes and does five jobs: segment tracking, the
 * wrong-way warning, the checkpoint sweep, lap closing and finishing
 * positions. It carries site 2 of the seven patches.
 *
 * ALL FIVE HAVE LIVE CODE in the functions below. Three are complete -
 * segment tracking, the checkpoint sweep, the finishing positions - and
 * TWO HAVE NAMED GAPS, listed here with the rest so that an untagged
 * gap reads as an oversight rather than as a decision:
 *
 *   0x0047e727  the WRONG WAY CAPTION - FUN_00481db0 to raise it,
 *               Widget_DestroyById to clear it. The plane test and
 *               the voice are in race_wrong_way_tick; only the text
 *               is missing.
 *   0x0047eb0d  FUN_00458310, the track record WRITE. The best-lap
 *               scan finds the time and nothing stores it, so this
 *               port reads records and never sets one.
 *   0x0047e8e1  the ILLEGAL VERSION overlay. Deliberately absent -
 *               see docs/subsystems/copy-protection.txt.
 *   throughout  the network and ghost arms.
 *
 * o THE HALF-TRACK WRAP IS THE WHOLE OF THE DELTA LOGIC. A racer
 *   crossing the start line steps from segmentCount-1 to 0, which
 *   is a delta of -(segmentCount-1); wrapping it into the half-open
 *   range around zero turns that into +1. Without it every lap
 *   would subtract a lap's worth of progress.
 *
 * o AND THE NEGATION IS ONLY IN REVERSE, which is what settles that an
 *   INCREASING segment index is forward in a normal race.
 *   game-object.txt 6.5 works it through.
 */

/*
 * DAT_004ea4c4, read live as 1. It is what lets a racer that is
 * neither local nor human close out laps at all - without it the
 * AI never counts a lap and never holds the best one, which is
 * visibly wrong: retail shows COMPUTER against the best lap
 * within a lap or two of the start. [open - what sets it]
 */
#define RACE_AI_LAPS_COUNT  1

/*
 * ORIGIN  maybe_Race_UpdateProgress @ 0x0047e3d0, the repeated
 *         visibility test
 *
 *     local == i || (soloMode && (humanPlayer || DAT_004ea4c4))
 *
 * It guards the lap close-out, the best-lap scan and the finish.
 * Every racer this port builds has soloMode 1, so with
 * RACE_AI_LAPS_COUNT the whole field passes - which is what
 * retail does.
 */
static int race_lap_visible(const MR_Racer *r, int index)
{
	if (index == s_race_localIndex)
		return 1;
	return r->soloMode != 0 &&
	       (r->humanPlayer != 0 || RACE_AI_LAPS_COUNT != 0);
}

/*
 * ORIGIN  maybe_Race_UpdateProgress @ 0x0047e3d0, at
 *         0x0047e67d..0x0047e73e
 *
 * THE WRONG-WAY WARNING.  Runs only when the segment step came
 * out below 1 - so it is asked the moment progress stops going
 * forward, not on a timer - and only for the racer on screen.
 *
 * THE TEST IS A PLANE SIDE, not an angle:
 *
 *     dot(segment.plane0.normal,
 *         raceDir * forward + position)  <  0
 *
 * forward is matOrientation row 2 and position is its
 * translation row, so the point tested is one unit in front of
 * the bike - ahead in a normal race, BEHIND in a reversed one,
 * which is the whole job raceDir does here.  The threshold at
 * 0x004d1900 is 0.0f.
 *
 * THE LATCH IS ON THE VOICE ONLY.  The original calls the
 * on-screen warning BEFORE testing the latch - a comma
 * expression at 0x0047e727 - so the text is put up every frame
 * the bike faces the wrong way while the line is spoken once.
 * Getting that round the wrong way gives a warning that
 * vanishes after one frame, or an announcer that will not stop
 * talking.
 *
 * IT RE-ARMS ON THE WAY BACK: the forward arm clears the latch
 * and destroys the widget at 0x0047e660, so turning round and
 * going wrong again is announced afresh.
 *
 * [open] The caption - the voice is here and the text is not.
 * Its calls are named in the skeleton header above.
 */
static int s_race_wrongWay;          /* DAT_004f0c58 */

static void race_wrong_way_tick(const MR_Racer *r)
{
	const MR_TrkSegment *sg;
	float                dir = g_reverseEnabled ? -1.0f : 1.0f;
	float                px, py, pz, dot;

	if (r->segmentStart < 0 ||
	    r->segmentStart >= s_race_trk.segmentCount)
	    return;

	sg = &s_race_trk.segments[r->segmentStart];

	px = dir * r->matOrientation[6] + r->matOrientation[9];
	py = dir * r->matOrientation[7] + r->matOrientation[10];
	pz = dir * r->matOrientation[8] + r->matOrientation[11];

	dot = sg->planes[0][0] * px
	      + sg->planes[0][1] * py
	      + sg->planes[0][2] * pz;

	if (dot >= 0.0f)
		return;

	/* [open] the WRONG WAY caption goes up here, every frame. */

	if (s_race_wrongWay)
		return;
	audio_cue_delayed(SND_VOICE_WRONGWAY, SND_VOICE_DELAY_MS);
	s_race_wrongWay = 1;
}


/*
 * STREAM 1, and it is not the one the rest of the race audio
 * uses.  maybe_Rand (0x00458cd0) is a one-line wrapper on
 * Rand_Next(1), and the finish line is its only caller here.
 */
#define RACE_FINISH_RAND_STREAM  1

static void race_update_progress(void)
{
	int half;
	int i;
	int cpCrossed = 0;

	if (!s_race_trk.segments || s_race_trk.segmentCount <= 0)
		return;

	half = s_race_trk.segmentCount / 2;

	for (i = 1; i < s_race_racerCount; i++) {
		MR_Racer *r = &s_race_racers[i];
		int       seg;
		int       delta;
		int       lapCrossed = 0;
		int       cpThis     = 0;

		if (!r->active)
			continue;

		/*
		 * ITS OWN LOOKUP, hinted by the racer's current segment.
		 * The racer tick does one too and they agree; the original
		 * also calls it twice a frame.
		 */
		seg = trk_find_segment_for_point(&s_race_trk,
		                                 r->segmentStart,
		                                 r->position,
		                                 &r->subSegment);
		r->segmentStart = seg;

		if (seg == r->segmentPrev)
			continue;

		delta = seg - r->segmentPrev;

		/*
		 * WRAPPED INTO (-half, +half], then negated for a reversed
		 * race. 0x0047e4a0 for the forward arm and 0x0047e4f0 for
		 * the reverse one; the two are not mirror images and are
		 * transcribed separately.
		 */
		if (!g_reverseEnabled) {
			if (delta > half)
				delta -= s_race_trk.segmentCount;
			if (delta <= -half && -delta != half) {
				delta += s_race_trk.segmentCount;
				/*
				 * THE LINE CROSSING, 0x0047e4c8. It is the WRAP
				 * itself that closes a lap - a delta that had to
				 * be pulled back up by a whole lap of segments is
				 * a racer stepping from the last segment to the
				 * first.
				 *
				 * AND IT IS TESTED BEFORE THE ACCUMULATOR MOVES.
				 * progress == progressHigh means "at your furthest
				 * ever", so backing over the line and crossing it
				 * again cannot buy a second lap.
				 */
				if (race_lap_visible(r, i) &&
				    r->trackTotalProgress == r->progressHigh) {
					lapCrossed = 1;
				}
			}
		} else {
			/*
			 * THE REVERSE ARM TESTS THE SEGMENTS, NOT THE WRAP,
			 * 0x0047e50e: the previous segment in the first half
			 * and the new one past the midpoint.
			 */
			if (r->segmentPrev >= 0 && r->segmentPrev < half &&
			    seg > half && race_lap_visible(r, i) &&
			    r->trackTotalProgress == r->progressHigh) {
				lapCrossed = 1;
			}
			delta = -delta;
			if (delta >= half && -half != seg - r->segmentPrev)
				delta -= s_race_trk.segmentCount;
			if (delta <= -half && -delta != half)
				delta += s_race_trk.segmentCount;
		}

		r->trackTotalProgress += delta;

		/*
		 * THE WRONG-WAY ARMS, 0x0047e64d.  Below 1 asks the
		 * question; 1 or more answers it by clearing the latch.
		 * Only for the racer on screen - race_viewed_racer.
		 */
		if (delta < 1) {
			if (i == s_race_localIndex)
				race_wrong_way_tick(r);
		} else if (i == s_race_localIndex) {
			s_race_wrongWay = 0;
			/* [open] and the caption comes down here. */
		}

		if (delta >= 1 && r->progressHigh < r->trackTotalProgress) {
			int n = delta;
			int at = r->segmentPrev + (g_reverseEnabled ? -1 : 1);

			r->progressHigh = r->trackTotalProgress;

			/*
			 * THE CHECKPOINT SWEEP, 0x0047e740. Every segment
			 * BETWEEN the old one and the new is visited, not
			 * just the one landed on - a fast bike can cross
			 * several in a frame and must not skip a line.
			 *
			 * A checkpoint is a segment with a non-negative linkA,
			 * and its linkB is added to the clock. Only the local
			 * racer's crossings move the timer.
			 */
			while (n-- > 0) {
				const MR_TrkSegment *sg;

				at = trk_wrap_segment_index(&s_race_trk, at);
				sg = &s_race_trk.segments[at];
				/*
				 * SIGNED, NOT A COMPARE AGAINST 0xffff - 0x0047e5f0
				 * reads linkA with MOVSX and tests -1 < it. The two
				 * agree on every shipped track, but 0xffff alone
				 * would take 0x8000..0xfffe for checkpoints where
				 * the original does not.
				 */
				if ((short)sg->linkA >= 0 && i == s_race_localIndex) {
					s_race_clock.timerMs +=
					    (int)sg->linkB;
					/*
					 * FIVE SECONDS OF "EXTENDED TIME",
					 * 0x0047e62f. hud.c reads it for the
					 * yellow arm of the countdown's
					 * colour ladder.
					 */
					g_extendedTimeMs = 5000;

					/*
					 * NOTED HERE, SPOKEN AFTER THE LOOP.
					 *
					 * The original does not queue the checkpoint
					 * cues from the crossing at all - the crossing
					 * only opens the window above and bumps a
					 * counter at racer+0x2f79, and Hud_DrawInRace
					 * queues them LATER IN THE FRAME, once the lap
					 * bookkeeping below has already run.
					 *
					 * THAT ORDER IS THE WHOLE POINT.  Speaking from
					 * inside the sweep reads lapCounter BEFORE the
					 * crossing that closes the lap increments it, so
					 * the final-lap suppression misses by exactly one
					 * lap and the checkpoint talks over FINAL LAP.
					 */
					cpCrossed = 1;
					cpThis    = 1;
				}
				at += (g_reverseEnabled ? -1 : 1);
			}
		}

		r->segmentPrev = seg;

		/*
		 * THE CROSSING COUNTER, 0x0047e82e - ONCE FOR THE FRAME,
		 * wrapped at the TIMING SEGMENT count and not at
		 * nbCheckPoint. audio-cues.txt 8.2.
		 */
		if (cpThis && r->raceState == RACESTATE_RACING_1) {
			r->checkpointIndex = (short)(r->checkpointIndex + 1);
			if (s_race_cpSegCount > 0 &&
			    r->checkpointIndex >= (short)s_race_cpSegCount)
			    r->checkpointIndex = 0;
		}

		/*
		 * THE LAP CLOSE-OUT, 0x0047e852. lapCounter moves here and
		 * nowhere else, so removing it pins the lap list at 1/4,
		 * accumulates every lap time into slot 0, and records no
		 * best lap at all.
		 */
		if (!lapCrossed)
			continue;

		if (race_lap_visible(r, i)) {
			if (r->raceState == RACESTATE_RACING_0) {
				/*
				 * THE FIRST CROSSING IS NOT A LAP. Leaving the
				 * grid takes raceState 0 -> 1 and counts nothing;
				 * only crossings in state 1 increment.
				 */
				r->raceState = RACESTATE_RACING_1;
			} else if (r->raceState == RACESTATE_RACING_1) {
				r->lapCounter = (short)(r->lapCounter + 1);
				/*
				 * THE FINAL LAP CALL, 0x0047e8b7.
				 *
				 * ONE LAP SHORT, NOT ON THE LAST ONE: the test
				 * is lapCounter - raceLaps == -1, so it fires
				 * as the bike STARTS its final lap, which is
				 * the crossing that leaves one to go.
				 *
				 * [open] The banner - five Widget_DestroyById
				 * calls clearing the upper stack, then
				 * FUN_00481c50(0,0,0,0,0).  The original tears
				 * the other messages down FIRST so the banner
				 * lands on a clear screen.
				 */
				if (i == 1 && s_race_laps > 0 &&
				    r->lapCounter == (short)(s_race_laps - 1)) {
					audio_cue_delayed(SND_VOICE_FINLAP,
					                  SND_VOICE_DELAY_MS);
				}
			}
		}

		if (r->raceState != RACESTATE_RACING_1)
			continue;

		/*
		 * THE BEST LAP, 0x0047eaf0. It RESCANS every completed lap
		 * rather than comparing the one just finished, which is
		 * how a bestLapMs that starts at the 0x7f7f7f7f sentinel
		 * gets filled without a special case for the first lap.
		 */
		if (race_lap_visible(r, i)) {
			int k;

			for (k = 0; k < r->lapCounter && k < RACE_LAP_SLOTS; k++) {
				int t = s_race_lapTimes[i * RACE_LAP_SLOTS + k];

				if (t < r->bestLapMs) {
					r->bestLapMs = t;
					/*
					 * 0x0047eb0d FUN_00458310 - the track record
					 * write, solo only. Not transcribed; it owns
					 * the name and time at 0x005d25b0.
					 */
				}
			}
		}

		/*
		 * THE FINISH, 0x0047eb90. raceState 2 is what moves a racer
		 * into the standings' FINISHED bucket, and until this landed
		 * that bucket never filled - see race_compute_standings.
		 */
		if (s_race_laps > 0 && r->lapCounter == (short)s_race_laps &&
		    race_lap_visible(r, i)) {
			/*
			 * THE POST-RACE HUD GATE, 0x0047eb04 - and it is the
			 * LOCAL racer's lap count that arms it, nobody else's.
			 * OUR SLOT, NOT SLOT 1, and not solo-only: the network
			 * arm waits for a peer to raise g_nRaceOverNet and
			 * nothing sends it, so a net race showed no results at
			 * all. Crossing the line now arms it either way and the
			 * host converges the stragglers. hud-race.txt 1.1.
			 */
			if (i == s_race_localIndex)
				s_race_overHud = 1;

			r->raceState   = RACESTATE_FINISHED;
			race_compute_standings();
			r->finishPosition = race_finishing_position(i);

			/*
			 * WHAT THE COMMENTATOR MAKES OF IT.
			 *
			 * ORIGIN  maybe_Race_UpdateProgress @ 0x0047e3d0,
			 *         at 0x0047ebf8
			 *
			 * SOLO ONLY, and only for the racer on screen.
			 *
			 *     won        a coin toss between WellDone
			 *                and yeaah, Rand_Next(1) under
			 *                0x4000
			 *     last       yuralosr
			 *     2nd, 3rd   NOTHING AT ALL
			 *     4th+       youlost
			 *
			 * THE SILENT ARM IS DELIBERATE and is easy to
			 * lose: `if (pos < 3) goto done` sits INSIDE the
			 * else, so a podium that is not a win draws no
			 * remark.  Only an outright win or a finish off
			 * the podium is worth saying something about.
			 *
			 * THE LAST-PLACE TEST COMES FIRST, so coming
			 * last in a field of three gets yuralosr rather
			 * than the silence third place would otherwise
			 * have earned.
			 *
			 * finishPosition is ZERO-BASED - 0 is the win.
			 */
			if (i == 1 && g_soloMode != 0) {
				int pos = r->finishPosition;
				int cue = -1;

				if (pos == 0) {
					cue = (rand_next(RACE_FINISH_RAND_STREAM)
					       < 0x4000)
					          ? SND_VOICE_WELLDONE
					          : SND_VOICE_YEAAH;
				} else if (pos == s_race_racerCount - 1) {
					cue = SND_VOICE_YURALOSR;
				} else if (pos >= 3) {
					cue = SND_VOICE_YOULOST;
				}

				if (cue >= 0)
					audio_cue_delayed(cue,
				                      SND_VOICE_DELAY_MS);
			}
		}
	}

	/*
	 * THE CHECKPOINT CALL - the blip, then the word.
	 *
	 * ORIGIN  Hud_DrawInRace @ 0x00413180, at 0x0041321a and
	 *         0x00413299 - which runs after this whole
	 *         function, so lapCounter is already current.
	 *
	 * THE MUTE IS ONE CROSSING, AND FORWARD IT IS THE START
	 * LINE, not a checkpoint - audio-cues.txt 8.2.
	 *
	 * AND SILENT ONCE THE RACE IS WON.  Hud_DrawInRace gates its
	 * whole checkpoint block on raceState == 1, at 0x004131d0
	 * and again at 0x004131e6, so a racer who has gone to state
	 * 2 gets nothing.
	 *
	 * THE FINISHING CROSSING IS ALSO A CHECKPOINT CROSSING,
	 * which is the whole reason that gate matters: without
	 * it the line is announced as a checkpoint at the same
	 * moment the result and RACE COMPLETED are queued, and
	 * three voices arrive together.
	 */
	if (cpCrossed && s_race_racerCount > 1) {
		const MR_Racer *lr = &s_race_racers[s_race_localIndex];

		/* 0x004131d0 forward, 0x004131e6 reversed - 8.2. */
		int mute = lr->checkpointIndex == 1 &&
		           (g_reverseEnabled
		            ? (s_race_laps > 0 &&
		               lr->lapCounter == (short)(s_race_laps - 1))
		            : lr->lapCounter == 0);

		if (lr->raceState == RACESTATE_RACING_1 && !mute) {
			audio_cue_delayed(SND_CHECKPOINT_BLIP,
			                  SND_VOICE_DELAY_MS);
			/* The word only in a mode that wants it,
			   0x0041323f - audio-cues.txt 8.6. */
			if (g_selectedMode != 0 || g_timeAttack != 0)
				audio_cue_delayed(SND_VOICE_CKCKPNT,
			                      SND_VOICE_DELAY_MS);
		}
	}
}

/*
 * THE STANDINGS KEY, packed by Race_ComputeStandings.
 *
 * THREE BUCKETS, ONE SORT. Every key carries the racer's index in
 * its top five bits, so sorting the packed keys sorts the racers and
 * a final shift recovers the indices. No parallel array and no
 * comparator that has to dereference a racer.
 *
 *     finished   key = ~raceClockMs & 0x7ffffff - INVERTED, so a
 *                descending sort gives ascending finish time
 *     racing     key = (trackTotalProgress << 12) | fraction
 *     out        key = raceClockMs plain
 *
 * then FINISHED, RACING, OUT concatenated.
 *
 * o IT CAPS THE DESIGN AT 27 BITS OF CLOCK AND 31 RACERS. Neither
 *   is reachable; a port that widens either has to abandon the
 *   packing.
 */
#define RACE_STANDINGS_MAX RACER_MAX

static int  s_race_standings[RACE_STANDINGS_MAX];
static int  s_race_standingsCount;

/*
 * ORIGIN  Race_SortStandingsKey @ 0x00457730, whole
 *
 * THE MASK IS THE WHOLE TRICK AND IT IS EASY TO MISS. The racer index
 * lives in the key's TOP FIVE BITS, ABOVE the payload, so a comparator
 * that looks at the whole word sorts by INDEX and nothing else -
 * putting the local racer, index 1, last in a field of twelve on every
 * frame, which is a plausible number that never changes. Masking to 27
 * bits compares the payload alone and carries the index for free.
 *
 * Descending: a > b sorts first, which is why the finished bucket
 * inverts its clock - descending on ~clock is ascending on clock.
 */
static int race_standings_cmp(const void *a, const void *b)
{
	unsigned int ka = *(const unsigned int *)a & 0x07ffffffu;
	unsigned int kb = *(const unsigned int *)b & 0x07ffffffu;

	if (ka < kb) return 1;
	return (ka <= kb) - 1;
}

/* ORIGIN  Race_ComputeStandings @ 0x0047e1e0, whole */
void race_compute_standings(void)
{
	unsigned int fin[RACE_STANDINGS_MAX];
	unsigned int run[RACE_STANDINGS_MAX];
	unsigned int out[RACE_STANDINGS_MAX];
	int nFin = 0, nRun = 0, nOut = 0;
	int i, k;

	s_race_standingsCount = 0;
	if (!race_ready())
		return;

	for (i = 0; i < s_race_racerCount && i < RACE_STANDINGS_MAX; i++) {
		const MR_Racer *r = &s_race_racers[i];
		unsigned int    tag = (unsigned int)i << 27;

		if (!r->active)
			continue;

		if (r->raceState == 2) {
			fin[nFin++] = (~(unsigned int)r->raceClockMs
			               & 0x7ffffffu) | tag;
		} else if (r->raceState == 3 || r->raceState == 0) {
			out[nOut++] = ((unsigned int)r->raceClockMs
			               & 0x7ffffffu) | tag;
		} else {
			/*
			 * THE +1024 BIAS, 0x0047e28d, from the double at
			 * 0x004d1978 - added forward, negated in reverse.
			 *
			 * subSegment is a SIGNED distance - read live it is
			 * -360 on one racer and -162 on another - so without
			 * the bias every racer clamps to 0 and the low twelve
			 * bits of the key carry nothing. The fraction is what
			 * separates two racers on the SAME segment, which on a
			 * starting grid is most of them.
			 */
			int f = (int)(g_reverseEnabled
			              ? -r->subSegment
			              : r->subSegment + 1024.0f);

			if (f < 0)      f = 0;
			if (f > 0xfff)  f = 0xfff;
			run[nRun++] = ((unsigned int)f & 0xfffu)
			              | ((unsigned int)r->trackTotalProgress
			                 << 12) | tag;
		}
	}

	qsort(fin, (size_t)nFin, sizeof(fin[0]), race_standings_cmp);
	qsort(run, (size_t)nRun, sizeof(run[0]), race_standings_cmp);
	qsort(out, (size_t)nOut, sizeof(out[0]), race_standings_cmp);

	k = 0;
	for (i = 0; i < nFin; i++) s_race_standings[k++] = (int)(fin[i] >> 27);
	for (i = 0; i < nRun; i++) s_race_standings[k++] = (int)(run[i] >> 27);
	for (i = 0; i < nOut; i++) s_race_standings[k++] = (int)(out[i] >> 27);
	s_race_standingsCount = k;

	/*
	 * ONCE PER RACE, the order as it comes off the grid. A position
	 * that is wrong only on the first frames is a SEED question, and
	 * the seeds are gone by the time anyone can read them.
	 */
	if (!s_race_standingsLogged && k > 0) {
		char line[160];
		int  n = 0;

		s_race_standingsLogged = 1;
		for (i = 0; i < k && n < (int)sizeof(line) - 8; i++)
			n += sprintf(line + n, "%d ", s_race_standings[i]);
		mrlog("race:   first standings: %s(local is racer 1, "
		      "position %d)\n", line,
		      race_finishing_position(race_viewed_index()) + 1);
	}
}

/* ORIGIN  maybe_Race_FinishingPosition @ 0x00459f50, whole */
int race_finishing_position(int racerIndex)
{
	int i;

	for (i = 0; i < s_race_standingsCount; i++)
		if (s_race_standings[i] == racerIndex)
			return i;
	return -1;
}

/* ORIGIN  NONE - ours, the seam hud.c reads. */
int race_standings_count(void)
{
	return s_race_standingsCount;
}

/* ORIGIN  NONE - ours. hud.c needs the map mesh and the scale. */
const MR_Trk *race_trk(void)
{
	return race_ready() ? &s_race_trk : NULL;
}

/* ORIGIN  NONE - ours, the seam hud.c reads for the LAPS banner. */
int race_lap_count(void)
{
	return s_race_laps;
}

/*
 * ORIGIN  NONE - ours. g_lapTimes @ 0x005db290 in the original,
 * indexed [racerIndex * 8 + lap]; RACE_LAP_SLOTS is that 8.
 *
 * The VIEWED racer, because every consumer so far is a HUD block
 * drawn from the viewed camera rather than from the local rider.
 */
int race_viewed_lap_time(int lap)
{
	if (!race_ready() || lap < 0 || lap >= RACE_LAP_SLOTS)
		return 0;
	/*
	 * BY g_viewedRacerIndex (0x00508834), which is the local racer
	 * and is not slot 1 on a guest.
	 */
	return s_race_lapTimes[race_viewed_index() * RACE_LAP_SLOTS
	                       + lap];
}

/* ORIGIN  NONE - ours, a read-only window on the roster. */
const MR_Racer *race_racer_at(int index)
{
	if (!race_ready() || index < 0 || index >= s_race_racerCount)
		return NULL;
	return &s_race_racers[index];
}

/* ORIGIN  NONE - ours, the seam hud.c reads. */
int race_racer_count(void)
{
	return race_ready() ? s_race_racerCount : 0;
}

/* ORIGIN  maybe_Hud_BindCornerSigns @ 0x00473370, the match loop */
const MR_TrkMesh *race_find_named_mesh(const char *prefix, int n)
{
	int i;
	int m;

	if (!race_ready() || !prefix || !s_race_trk.named)
		return NULL;

	for (i = 0; i < s_race_trk.namedCount; i++) {
		if (strncmp(s_race_trk.named[i].name, prefix, (size_t)n) != 0)
			continue;
		/*
		 * THE FIRST NON-EMPTY LOD. The original indexes a flat
		 * global mesh array and takes whatever slot the name
		 * matched; ours has the three per template and nothing
		 * has confirmed what they are. For the counter it makes
		 * no difference - the plate is one quad at every level.
		 */
		for (m = 0; m < 3; m++)
			if (s_race_trk.named[i].mesh[m].primCount > 0)
				return &s_race_trk.named[i].mesh[m];
		return NULL;
	}
	return NULL;
}

/* ================================================================== */
/* THE REPLAY.                                                        */
/*                                                                    */
/* ORIGIN  maybe_Replay_WriteInput @ 0x004577f0                       */
/*         maybe_Replay_ReadInput  @ 0x004578a0                       */
/*                                                                    */
/* IT RECORDS INPUTS, NOT POSITIONS, and plays them back through the  */
/* real physics - which is why the playback lands on the same lap     */
/* times to the millisecond. See docs/subsystems/replay.txt.          */
/*                                                                    */
/* THE FRAME DELTA IS RECORDED ALONGSIDE THE INPUT, and that is what  */
/* keeps it in sync: a replay does not re-time the race, it re-runs   */
/* it with the SAME dt sequence, so an integration done in 33 ms      */
/* steps is redone in 33 ms steps whatever this machine is doing now. */
/*                                                                    */
/* o WHICH MEANS A RETAIL .Sav WILL NOT PLAY BACK HERE, and never     */
/*   will unless the physics is bit-exact. Recording inputs is        */
/*   unforgiving in a way recording positions would not have been.    */
/*   Our own races replay fine: same binary, same integrator.         */
/* ================================================================== */

/* 0x4fb0, tested in maybe_Replay_WriteInput and nowhere else. A
 * longer race records only its first 20,400 frames - no wrap, no
 * error, the rest of the replay is simply missing. */
#define REPLAY_MAX        20400


/*
 * EIGHT BYTES, THE ORIGINAL'S LAYOUT EXACTLY - which is deliberate
 * and not just tidiness: tools/mr_replay.py decodes MOTO's buffer
 * and reads ours unchanged because of it.
 *
 *     +0x00  3   the input word, low 24 bits
 *     +0x03  1   the frame delta in MILLISECONDS
 *     +0x04  2   joystick axis 0, 0x8000 centred
 *     +0x06  2   axis 1
 */
typedef struct {
	unsigned int   packed;      /* ms << 24 | buttons & 0xffffff */
	unsigned short axis0;
	unsigned short axis1;
} MR_ReplayRec;

static MR_ReplayRec  s_replay[REPLAY_MAX];
static int           s_replayCount;   /* records written, = write index */

/*
 * THE INPUT AS POLLED, for the recorder. The tick CLEARS stuntHeld -
 * racer_apply_drive's brake arm - so reading the racer back after the
 * tick records what is left rather than what the tick consumed, and
 * the playback then applies that one substep early.
 */
static unsigned int   s_pollWord;
static int            s_pollStuntHeld;
static int            s_pollStuntPressed;
static unsigned short s_pollAxis0 = 0x8000;
static unsigned short s_pollAxis1 = 0x8000;

/* ORIGIN  NONE - ours. */
static void race_poll_capture(const MR_Racer *r)
{
	if (r != &s_race_racers[s_race_localIndex])
		return;
	s_pollWord         = r->inputWord & 0xffffffu;
	s_pollStuntHeld    = r->stuntHeld;
	s_pollStuntPressed = r->stuntPressed;
	s_pollAxis0        = (unsigned short)r->axis0;
	s_pollAxis1        = (unsigned short)r->axis1;
}

static int           s_replayFrame;   /* the PLAYBACK position          */
static int           s_replayMode;
static unsigned int  s_replayTotalMs;
/*
 * ARMED ACROSS THE RESTART. Playing a race back means running it
 * again from the grid with the recorded inputs, and the restart
 * goes through race_load - which resets the replay. This survives
 * that one reset, so the recording is not wiped by the very thing
 * about to play it.
 */
static int           s_replayArmed;
static int           s_replayLongWarned;
static int           s_replayBudgetMs;

/*
 * THE SIMULATION SEED, AND THE ORIGINAL SEEDS TOO.
 *
 * srand is FUN_004b35f0 in the DB - unnamed, which is why a search
 * for the name finds nothing - and it has two callers [verified]:
 *
 *   maybe_Game_InitRace @ 0x0047bf70, which is game vtable slot
 *   +0x8, the reset-and-grid slot, seeds at 0x0047bfcf. It reads
 *   g_dwReplayMode first: on PLAYBACK it re-uses the seed stashed
 *   at 0x004f0c48, and on a live race it calls timeGetTime and
 *   stores that there. So the replay is deliberately given the
 *   recording's own seed.
 *
 *   FUN_00411010 @ 0x00411097 seeds with a FIXED 0, and the
 *   in-race menus reach it through Menu_DispatchPendingRequest
 *   bit 0x2 - so a restart and a replay-load both land on seed 0.
 *
 * SO RETAIL'S REPLAY DETERMINISM IS DESIGNED, NOT INCIDENTAL, and
 * it has to be: a wall hit crashes on rand() & 3 == 0 at
 * 0x00469166, one chance in four, so an unseeded playback would
 * crash where the recording did not.
 *
 * Ours uses a fixed constant, which is FUN_00411010's approach
 * rather than the stash-and-reuse one. The property is the same -
 * the live run and its playback start from one stream position -
 * and a constant needs nothing persisted. The seed is NOT in the
 * .Sav header either way: 0x004f0c48 has exactly two xrefs, the
 * read and the write above, so retail's own saved replays only
 * reproduce within the session that recorded them.
 *
 * WITHOUT IT A REPLAY CANNOT MATCH ITS RECORDING. race_load resets
 * the racers and not the generator, so playing a recording back
 * starts from a different stream position and the AI ground sink,
 * the stunt test and the crash fall side all diverge. A wall crash
 * shows it first because that is where the most conditional draws
 * land at once.
 *
 * ONLY THE SIMULATION'S STREAM NEEDS THIS. The FX draw from
 * rand_next(1) and the trackside director from rand_next(0), so
 * neither can move this one however many particles a frame emits -
 * which is the whole reason the original splits them. Their own
 * streams are deliberately NOT reset: the spray may differ between
 * a recording and its playback, and being visual it cannot desync
 * anything. See rand.h.
 */
#define RACE_SIM_SEED 1
static unsigned int  s_replaySeed = RACE_SIM_SEED;

/*
 * Called from race_load BEFORE the first draw. A fresh race takes
 * the fixed seed; an ARMED one keeps the seed its recording was
 * made at, so the same inputs meet the same stream.
 */
static void race_seed_sim(void)
{
	if (!s_replayArmed)
		s_replaySeed = RACE_SIM_SEED;
	srand(s_replaySeed);
}

/* Latched once a frame on playback, applied to every substep. */
static unsigned int   s_replayWord;
static unsigned short s_replayAxis0 = 0x8000;
static unsigned short s_replayAxis1 = 0x8000;

/* ORIGIN  FUN_00410120 @ 0x00410120, its replay half */
void race_replay_reset(void)
{
	if (s_replayArmed) {
		/* Keep the recording - rewind it and play. */
		s_replayArmed = 0;
		s_replayFrame = 0;
		s_replayMode  = REPLAY_MODE_PLAY;
		/*
		 * AND CUT AT ONCE. camera_reset leaves the shot timer at
		 * zero, which is not yet expired, so the director would
		 * hold a stale eye for one frame before its first
		 * selection.
		 */
		s_race_cam.shot      = CAM_SHOT_CHASE;
		s_race_cam.shotTimer = -1;
		mrlog("race: replay starting, %d frames, %u ms\n",
		      s_replayCount, s_replayTotalMs);
		return;
	}
	s_replayCount   = 0;
	s_replayFrame   = 0;
	s_replayMode    = REPLAY_MODE_LIVE;
	s_replayLongWarned = 0;
	s_replayBudgetMs   = 0;
	s_replayTotalMs = 0;
	s_replayWord    = 0;
	s_replayAxis0   = 0x8000;
	s_replayAxis1   = 0x8000;
}

int race_replay_mode(void)  { return s_replayMode; }
int race_replay_count(void) { return s_replayCount; }

/*
 * ORIGIN  NONE - ours. The original has no restart to survive.
 *
 * Arm the playback. The caller then RESTARTS the race; the arm
 * survives race_load's reset and the playback begins at the grid.
 */
int race_replay_start(void)
{
	if (s_replayCount <= 0)
		return 0;
	s_replayArmed = 1;
	return 1;
}

/* ORIGIN  NONE - ours, the other half of race_replay_start. */
void race_replay_stop(void)
{
	if (s_replayMode != REPLAY_MODE_LIVE)
		mrlog("race: replay ended at frame %d of %d\n",
	          s_replayFrame, s_replayCount);
	s_replayMode = REPLAY_MODE_LIVE;
}

/*
 * ORIGIN  maybe_Replay_WriteInput @ 0x004577f0, whole
 *
 * Nine lines in the original: clamp the delta, fold in the two stunt
 * bits, pack, bump the count.
 *
 * THE STUNT IS SYNTHESISED AND HAS TO BE. Turbo/Stunts is the one
 * bindable action that sets NO input bit - its key handler writes
 * stuntHeld and stuntPressed directly and never touches the word -
 * so without folding them in here a recording could not reproduce a
 * wheelie. Bit 7 is a LEVEL and bit 6 an EDGE; they are not two more
 * keys. replay.txt 2.1.
 *
 * o g_dwReplayTotalMs ACCUMULATES THE UNCLAMPED DELTA, so a frame
 *   longer than 255 ms makes that total and the sum of the recorded
 *   deltas disagree - and the replay plays that frame back short.
 *   Transcribed as written.
 */
static void race_replay_write(const MR_Racer *r, int frameMs)
{
	MR_ReplayRec *rec;
	unsigned int  w;
	unsigned int  ms;

	if (!r || s_replayCount >= REPLAY_MAX)
		return;

	s_replayTotalMs += (unsigned int)frameMs;   /* UNCLAMPED */

	ms = (unsigned int)frameMs;
	if (ms > 0xffu) {
		/*
		 * ONE BYTE CANNOT HOLD A STEP THIS LONG, so this record
		 * drifts from here - the bullet above has the rest of it.
		 * FAST-FORWARD IS HOW IT HAPPENS: the virtual clock scales
		 * the step, so 40x turns a 17 ms frame into 640.
		 *
		 * Warned once, not per frame - a fast-forwarded lap would
		 * otherwise fill the log. The clamp itself is the
		 * original's. [open - recording under fast-forward]
		 */
		if (!s_replayLongWarned) {
			s_replayLongWarned = 1;
			mrlog_err("race: replay frame %d ms exceeds the "
			          "record's 255 - this playback will drift\n",
			          frameMs);
		}
		ms = 0xffu;
	}

	/* The polled input, NOT the racer's fields - race_poll_capture. */
	(void)r;
	w = s_pollWord;
	if (s_pollStuntHeld)    w |=  0x80u;
	else                    w &= ~0x80u;
	if (s_pollStuntPressed) w |=  0x40u;
	else                    w &= ~0x40u;

	rec = &s_replay[s_replayCount++];
	rec->packed = (ms << 24) | (w & 0xffffffu);
	rec->axis0  = s_pollAxis0;
	rec->axis1  = s_pollAxis1;
}

/*
 * ORIGIN  NONE - ours. The original never replays faster than it
 *         recorded, so it has no pacing to do.
 *
 * HOW MANY RECORDED STEPS FIT IN THE TIME THAT HAS ACTUALLY
 * PASSED - the playback's pacing, and what makes a race recorded
 * under fast-forward play back at NORMAL speed.
 *
 * A record is one SUBSTEP, so it carries a fixed amount of
 * simulated time whatever the frame rate or clock scale it was
 * recorded at. Replaying is therefore just a question of how many
 * of them this frame's wall clock is worth.
 *
 * Peeks only - race_replay_begin_frame does the consuming, one
 * step at a time, because each carries its own input word.
 */
static int race_replay_peek_steps(int budgetMs, int maxSteps,
                                  int *usedMs)
{
	int n = 0;
	int used = 0;
	int at = s_replayFrame;

	while (n < maxSteps && at < s_replayCount) {
		int ms = (int)((s_replay[at].packed >> 24) & 0xffu);

		/* A zero-length record would spin here for ever. */
		if (ms <= 0)
			ms = 1;
		if (used + ms > budgetMs)
			break;
		used += ms;
		at++;
		n++;
	}
	if (usedMs)
		*usedMs = used;
	return n;
}

/*
 * ORIGIN  maybe_Replay_ReadInput @ 0x004578a0
 *
 * Pulls one frame's record and hands back the dt it was recorded
 * with, so the caller integrates the same step. Returns 0 when the
 * recording runs out, which ends the playback.
 */
int race_replay_begin_frame(float *dt)
{
	const MR_ReplayRec *rec;

	if (s_replayMode != REPLAY_MODE_PLAY)
		return 0;
	if (s_replayFrame >= s_replayCount) {
		race_replay_stop();
		return 0;
	}

	rec           = &s_replay[s_replayFrame++];
	s_replayWord  = rec->packed & 0xffffffu;
	s_replayAxis0 = rec->axis0;
	s_replayAxis1 = rec->axis1;

	if (dt)
		*dt = (float)((rec->packed >> 24) & 0xffu) / 1000.0f;
	return 1;
}

/*
 * ORIGIN  NONE - ours, the playback half of race_poll_input.
 *
 * The recorded word instead
 * of the keyboard. THE GAME IS NOT MERGING THE TWO INPUT SOURCES, it
 * is switching between them (replay.txt 3).
 */
static void race_replay_apply(MR_Racer *r)
{
	r->inputWord  = s_replayWord & 0x3fu;   /* the six real keys */
	r->inputWord2 = 0;
	r->axis0      = s_replayAxis0;
	r->axis1      = s_replayAxis1;

	/*
	 * BOTH ASSIGNED. Bit 6 is a one-shot and leaving it to
	 * maybe_Racer_UpdatePose to clear does NOT match the live rider:
	 * that clear runs once per frame off the draw while these run
	 * once per substep, so the flag outlives the live one and can
	 * start a stunt the recording never started.
	 */
	r->stuntHeld    = (s_replayWord & 0x80u) ? 1 : 0;
	r->stuntPressed = (s_replayWord & 0x40u) ? 1 : 0;
}

/*
 * ORIGIN  maybe_Input_ReadDevice @ 0x00411a90, its tail at 0x00411e6b
 *
 * THE STUNT BUTTON IS AN EDGE, AND IT WRITES TWO FIELDS. stuntHeld
 * (+0x31f9) is the LEVEL; stuntPressed (+0x3201) is a ONE-SHOT that
 * maybe_Racer_UpdatePose consumes through its own prev/now latch at
 * 0x0045e36c. SETTING THE LEVEL ALONE STARTS NO STUNT AT ALL, because
 * racer_pose_stunt's rising-edge test never passes.
 *
 * THE EDGE IS DETECTED AGAINST action6Down, which is where the original
 * detects it. Nothing else in the port writes that field.
 */
static void race_stunt_edge(MR_Racer *r, int down)
{
	if (down == r->action6Down)
		return;

	r->action6Down = down;
	if (down) {
		r->stuntHeld    = 1;
		r->stuntPressed = 1;
	} else {
		r->stuntHeld    = 0;
	}
}


/* Defined below; the probe is the only forward user. */
static void race_tick_physics(MR_Racer *r, float dtScaled);

/* ================================================================== */
/* THE LOOK-AHEAD SIMULATOR.                                          */
/*                                                                    */
/* ORIGIN  NONE - ours. A HELPER FOR THE AUTOPILOT and nothing else.  */
/* ================================================================== */

/*
 * Run the REAL physics forward from the CURRENT state with a given
 * button word AND STEERING ANGLE held, report where the bike ended
 * up, and put every byte back.
 *
 * IT LIVES HERE BECAUSE OF WHAT IT NEEDS, not because the autopilot
 * belongs in race.c. race_tick_physics is static, and so is every
 * piece of cross-frame state a tick touches; reaching them from
 * outside would mean exporting six statics and the tick itself. One
 * function that borrows them in place is a far smaller hole, and the
 * autopilot sees only the result.
 *
 * EVERY PIECE OF CROSS-FRAME STATE IS SAVED AND PUT BACK - the save
 * block below IS that list, and a new static has to join it or the
 * probe leaks into the race.
 *
 * o THE WHEEL FX EMITTER CANNOT BE RESTORED, since it appends
 *   particles and burns RNG, so it is suppressed for the duration by
 *   s_race_dryRun.
 *
 * o THE RNG IS NOT RESTORED, and it does not need to be. The only
 *   rand() left inside the gate is the crash fall side; a probe
 *   that crashes has already told the caller what it needed to
 *   know, and one drawn number does not change a race.
 *
 * o NOTHING ELSE IN THE FRAME RUNS. No progress, no standings, no
 *   clock, no AI, no draw - this is race_tick_physics alone.
 */
int race_autopilot_probe(unsigned int buttons, int axis1, int ticks,
                         float dtScaled, MR_AutoProbe *out)
{
	MR_Racer      saveRacer;
	MR_Drivetrain saveDrive;
	MR_Gearbox    saveGearbox;
	float         savePrevLong;
	float         saveSegPos;
	float         saveLeanDelta;
	float         saveYawOrient;
	float         saveYawFrame;
	float         saveYawWorld;
	int           saveYawIndex;
	int           saveLatch;
	MR_Racer     *r;
	int           i;

	if (!out || ticks <= 0 || !race_ready() || s_race_racerCount < 2)
		return 0;
	if (s_race_dryRun)              /* no probing from inside a probe */
	    return 0;

	/* The step the REAL tick is taking - see race_frame_delta. A
	 * probe that integrates at a different rate is answering a
	 * different question. */
	if (dtScaled <= 0.0f)
		dtScaled = race_frame_delta();
	if (dtScaled <= 0.0f)
		return 0;

	r = &s_race_racers[s_race_localIndex];

	memcpy(&saveRacer, r, sizeof(saveRacer));
	saveDrive     = s_race_drive;
	saveGearbox   = s_race_gearbox;
	savePrevLong  = s_race_prevLongSpeed;
	saveSegPos    = s_race_segPos;
	saveLeanDelta = s_race_leanDelta;
	saveYawOrient = s_race_yawOrient;
	saveYawFrame  = s_race_yawFrame;
	saveYawWorld  = s_race_yawWorld;
	saveYawIndex  = s_race_yawIndex;
	saveLatch     = racer_shift_latch_get();

	s_race_dryRun = 1;

	for (i = 0; i < ticks; i++) {
		r->inputWord  = buttons;
		r->inputWord2 = 0;
		/*
		 * THE STEERING ANGLE IS SET EVERY TICK TOO, and it has to
		 * be: left alone, a rollout simulates whatever angle the
		 * live frame happened to leave on the racer, so "what if I
		 * steered the other way" answers with the CURRENT steering
		 * and a different throttle. axis1 goes back with the racer.
		 */
		r->axis1      = axis1;
		race_tick_physics(r, dtScaled);
	}

	out->position[0] = r->position[0];
	out->position[1] = r->position[1];
	out->position[2] = r->position[2];
	out->speed       = r->speed;
	out->segment     = r->segmentStart;
	out->crashed     = (r->flCrashTimerMs > 0.0f) ? 1 : 0;

	s_race_dryRun = 0;

	memcpy(r, &saveRacer, sizeof(saveRacer));
	s_race_drive         = saveDrive;
	s_race_gearbox       = saveGearbox;
	s_race_prevLongSpeed = savePrevLong;
	s_race_segPos        = saveSegPos;
	s_race_leanDelta     = saveLeanDelta;
	s_race_yawOrient     = saveYawOrient;
	s_race_yawFrame      = saveYawFrame;
	s_race_yawWorld      = saveYawWorld;
	s_race_yawIndex      = saveYawIndex;
	racer_shift_latch_set(saveLatch);

	return 1;
}


/*
 * ORIGIN  maybe_Input_ReadDevice @ 0x00411a90 - the keyboard, into the
 *         racer's input word.
 *
 * THE BIT LAYOUT IS THE GAME'S, from racer+0x31e9: 1 accelerate,
 * 2 brake, 4 left, 8 right. Everything downstream - racer_apply_drive,
 * racer_apply_steering, physics_brake_gated - already reads those
 * bits, so this is the only place a key code appears.
 *
 * GetAsyncKeyState rather than the message queue, because this wants
 * the CURRENT held state once a frame and not an edge - the game's own
 * input is a level-triggered word too (keyboard via GetKeyState, no
 * DirectInput).
 */
/* ORIGIN  NONE - ours. main.cpp calls it from WM_SETFOCUS. */
void race_set_input_focus(int active)
{
	s_race_inputFocus = active ? 1 : 0;
}


static void race_poll_input(MR_Racer *r)
{
	unsigned int w = 0;
	int          stunt = 0;
	int          ax1 = 0x8000;

	/*
	 * THE REPLAY WINS OVER EVERY OTHER SOURCE, and it is a SWITCH
	 * rather than a merge - the player is locked out for the whole
	 * playback, which is also what stops mrhook's SEND_INPUT
	 * driving through it.
	 */
	if (race_replay_mode() == REPLAY_MODE_PLAY) {
		race_replay_apply(r);
		return;
	}

	/*
	 * THE AGENT DRIVES IF IT IS DRIVING. While SEND_INPUT holds a
	 * key the keyboard is ignored, so a scripted run is not fought
	 * by whatever the user happens to be leaning on. Off by
	 * default and gone the moment the run ends - agent.h.
	 */
	if (agent_input_get(&w, &stunt)) {
		r->inputWord  = w;
		r->inputWord2 = 0;
		r->axis0 = 0x8000;
		r->axis1 = 0x8000;
		race_stunt_edge(r, stunt);
		return;
	}

	/*
	 * AND THEN THE AUTOPILOT, SECOND - see autopilot.h. Same
	 * contract as the agent above, on purpose: two overrides in
	 * a row should not differ in shape.
	 *
	 * THE ORDER IS NOT ARBITRARY. A scripted mr_place.py run has
	 * to win, or a debug flag somebody left on would fight the
	 * comparison it was set up to make - and the trace would
	 * diverge from MOTO's for a reason nothing on screen shows.
	 */
	if (autopilot_input(r, &s_race_trk, s_race_racers,
	                    s_race_racerCount, &w, &stunt, &ax1)) {
		r->inputWord  = w;
		r->inputWord2 = 0;
		/*
		 * THROTTLE DIGITAL, STEERING ANALOG. axis0 stays
		 * centred so the brake keeps working off the button;
		 * axis1 carries a real angle - autopilot.h.
		 */
		r->axis0 = 0x8000;
		r->axis1 = ax1;
		race_stunt_edge(r, stunt);
		return;
	}

	/*
	 * NOT WHILE ANOTHER WINDOW HAS THE KEYBOARD. GetAsyncKeyState
	 * reads the physical key whatever has focus. The three
	 * overrides above are deliberately NOT gated - a scripted run
	 * should work with the window in the background. Neutral
	 * rather than skipped, or whatever was held would latch.
	 */
	if (!s_race_inputFocus && !s_race_inputUnfocused) {
		r->inputWord  = 0;
		r->inputWord2 = 0;
		r->axis0 = 0x8000;
		r->axis1 = 0x8000;
		race_stunt_edge(r, 0);
		return;
	}

	if (GetAsyncKeyState('Z') & 0x8000)      w |= 1u;
	if (GetAsyncKeyState('X') & 0x8000)      w |= 2u;
	if (GetAsyncKeyState(VK_LEFT) & 0x8000)  w |= 4u;
	if (GetAsyncKeyState(VK_RIGHT) & 0x8000) w |= 8u;

	/*
	 * THE GEAR BUTTONS, bits 0x10 up and 0x20 down - read by the
	 * manual latch in racer_update_airborne, ORIGIN 0x00466735.
	 * They do nothing in automatic: the latch tests f_0088 first.
	 *
	 * B AND V ARE OURS.  The original binds action 4 Higher Gear
	 * to Z and action 5 Lower Gear to A - see
	 * input-bindings.txt 2 - but this port already took Z for the
	 * throttle, its own divergence from the original's UP arrow.
	 * Rebinding is a menu page nobody has built, so the layout
	 * stays literal here.
	 *
	 * BOTH HELD IS NOT A SHIFT.  The latch has an explicit arm
	 * for it at 0x0046674b that swallows the frame, so rolling a
	 * finger across the two keys cannot double-shift.
	 */
	if (GetAsyncKeyState('B') & 0x8000)      w |= 0x10u;
	if (GetAsyncKeyState('V') & 0x8000)      w |= 0x20u;

	r->inputWord  = w;
	r->inputWord2 = 0;

	/*
	 * Centred - there is no analog stick here, so the axis paths in
	 * racer_apply_drive and racer_apply_steering stay unused.
	 *
	 * AND 0x8000 IS WHAT THE ORIGINAL WRITES, not just a convenient
	 * placeholder: maybe_Game_ReadInput centres both axes for any
	 * racer whose deviceType is not 2 or 4, which is every keyboard
	 * player. Confirmed live - deviceType 1, both axes 0x8000.
	 * racers.txt 9.45.
	 */
	r->axis0 = 0x8000;
	r->axis1 = 0x8000;

	race_stunt_edge(r, (GetAsyncKeyState('C') & 0x8000) ? 1 : 0);
}


/*
 * ORIGIN  NONE - ours. The original has no such split: instance+0x24
 *         IS the racer, so its drivetrain fields ARE the racer's.
 *         This pair is the seam the split makes necessary, and it
 *         disappears if MR_Drivetrain is ever retired.
 *
 * THE RACER IS THE STATE; s_race_drive IS THE MIRROR, never the other
 * way round. Loading at the top of the tick is what lets an EXTERNAL
 * write to a racer field take effect, which is what mr_place and mr_ab
 * depend on.
 *
 * MIRROR-FIRST DISCARDS THOSE WRITES SILENTLY - the next tick simply
 * overwrites the racer from the static - and it reads as the bike
 * refusing to change speed rather than as a lost write. The byte-exact
 * struct exists to stop that: one offset, one live field, on both
 * sides, for reads AND writes.
 */
static void drive_load(MR_Drivetrain *d, const MR_Racer *r)
{
	d->engineSpeed      = r->engineSpeed;
	d->airTime          = r->flAirTime;
	d->brakeHoldMs      = r->brakeHoldMs;
	d->speed            = r->speed;
	d->gearboxRoadSpeed = r->gearboxRoadSpeed;
}

static void drive_store(const MR_Drivetrain *d, MR_Racer *r)
{
	r->engineSpeed      = d->engineSpeed;
	r->flAirTime        = d->airTime;
	r->brakeHoldMs      = d->brakeHoldMs;
	r->speed            = d->speed;
	r->gearboxRoadSpeed = d->gearboxRoadSpeed;
}


/*
 * ORIGIN  maybe_Racer_Tick @ 0x0045d580 - one frame of the drivetrain,
 *         on the rail. The body below follows its call order.
 *
 * The three drive paths are dispatched on the demand's SIGN, which is
 * what maybe_Physics_TickDrivetrain branches on (racers.txt 9.21.4).
 */
static void race_tick_physics(MR_Racer *r, float dtScaled)
{
	const MR_BikeCarac *c = bike_at(r->bikeIndex);
	MR_DriveDemand      dm;
	/*
	 * dtMs IS A DOUBLE, AND THAT IS NOT COSMETIC.
	 *
	 * dtScaled is 3.2999999523 and the product with ten lands a
	 * whisker under 33.  In a FLOAT that whisker is below half an
	 * ULP at that magnitude, so it rounds to exactly 33.0 and every
	 * integer consumer reads 33; the original computes on the x87 at
	 * 80-bit and truncates the same quantity to 32.  One count, once
	 * a frame, on the gearbox clutch timer - worth a whole frame per
	 * 300 ms shift.  See racers-convergence.txt 9.18.28.
	 *
	 * The float consumers are unaffected: 32.9999995 converts back to
	 * 33.0f at the call, which is what they were already getting.
	 */
	double              dtMs;
	float               segLen;
	/*
	 * EBX at 0x0045d748. ONE FLAG, TWO CALLS - the lift gate in
	 * Integrate and the airborne threshold in UpdateGroundContact.
	 * See the comment at the integrate call below.
	 */
	int                 bLiftAllowed;

	if (!c || !c->loaded || dtScaled <= 0.0f)
		return;

	/*
	 * dtScaled ARRIVES ALREADY SCALED, as Racer_TickDispatch passes
	 * it - a 0.1-scaled millisecond unit, 3.3 at 33 ms. The
	 * drivetrain scales it by ten again internally. See physics.h.
	 */
	dtMs = (double)dtScaled * 10.0;

	/*
	 * ORIGIN  Racer_SavePrevFrame @ 0x00465fa0
	 *
	 * FIRST, because maybe_Racer_Tick calls it first at 0x0045d5bf.
	 * It is the only writer of matOrientationPrev, speedTail,
	 * positionPrev2 and fxGatePrev, none of which the port had.
	 */
	racer_save_prev_frame(r, race_frame_delta());

	/* The racer is the state - see drive_load. */
	drive_load(&s_race_drive, r);

	/*
	 * NOT WHILE PROBING, and this is the whole reason a probe
	 * works at all. race_autopilot_probe re-enters this
	 * function to simulate one button word held; if the poll
	 * ran inside that, it would overwrite the injected word on
	 * every nested tick and the probe would be simulating the
	 * autopilot instead of the candidate. Every candidate then
	 * scores the same and the veto is noise.
	 */
	if (!s_race_dryRun) {
		race_poll_input(r);
		race_poll_capture(r);
	}

	/*
	 * ORIGIN  maybe_Racer_Tick @ 0x0045d580, its inline preamble
	 *
	 * springCompression IS A ONE-FRAME KICK, not a stored value. The
	 * original zeroes it at the top of every tick and the writers
	 * later in the frame - the wheelie touchdown at 0x004661b8 and
	 * the landing block at 0x00469460 - fill it in again, so anything
	 * reading it must do so in the same frame.
	 *
	 * WITHOUT THE CLEAR IT LATCHES into a permanent bias on the
	 * forks. MOTO reads 0 every frame through a wheelie and -14.893
	 * on the single touchdown frame.
	 */
	r->springCompression = 0.0f;

	/*
	 * AND THE RIDER STATE WORD, 0x0045d59d..0x0045d5ab. The player
	 * tick zeroes THREE fields together - handSkeleton (+0x319d),
	 * riderStateBits (+0x2dac) and +0x2e95 - and racer_tick_ai does
	 * the same for the AI at its own 0x0046bfb2.
	 *
	 * THE BITS ARE A ONE-FRAME REPORT, NOT A STATE. Everything that
	 * writes them ORs in, so without the clear they only accumulate -
	 * one shove sets a bit that is still set ten frames later, where
	 * MOTO reads 0 on every frame after the impact.
	 */
	r->handSkeleton   = 0;
	r->riderStateBits = 0;

	/*
	 * AND THE BRAKE LEAN UNWINDS, at the top of the tick where
	 * maybe_Racer_Tick has it - the racer+0x2ea5 block at
	 * 0x0045d5cb, before its own SlipSpeed call at 0x0045d679.
	 *
	 * IT IS THE OTHER HALF OF racer_update_airborne, which pushes
	 * headingRef down by frameDelta * -pi/1600 while the brake is
	 * held, floored at -pi/16. This pulls it back to zero and pins it
	 * there once the brake is released.
	 */
	racer_update_heading_ref(r, race_frame_delta());

	/*
	 * AND THE FX GATE, 0x0045d676 - right after that SlipSpeed call
	 * and before the drive chain:
	 *
	 *     slip   = maybe_Racer_SlipSpeed(instance)
	 *     fxGate = (onGround && |slip| >= 0.04) ? 1 : 0   0x004d0f40
	 *
	 * IT IS WHAT TURNS A REAR-WHEEL SLIDE INTO ANYTHING. Three things
	 * test racer+0x2da0:
	 *
	 *   the skid ribbons   (fxGate && |slip| > 0.5) - the tyre streak
	 *   the wheel spray    the same test - the smoke off a sliding
	 *                      rear wheel
	 *   racer_steer_torque grip = |slip| * K + 1, so a sliding bike
	 *                      turns HARDER for the same bar angle
	 *
	 * BRAKING SETS ITS OWN BIT, which is why braking effects can work
	 * while every sliding one is dead - the tell if this reads 0. On
	 * the dirt bike, where holding a turn breaks the back end loose,
	 * this gate is the whole effect.
	 *
	 * Known answer: gas and right held, MOTO raises fxGate by step 24
	 * and runs skidWidth 1.00 -> 6.00.
	 */
	r->fxGate = racer_fx_gate(r->onGround,
	                          physics_slip_speed(r->matOrientation,
	                                             r->velDir, r->speed));

	/*
	 * NOTHING DRIVES WHILE HE IS DOWN. maybe_Racer_Tick gates the
	 * whole input chain on crashLatch, not just the bars: on MOTO a
	 * crashed bike's speed decays a clean 0.9 a frame - 4.37, 3.56,
	 * 2.90, 2.37, 1.93 - with no re-acceleration at all, which only
	 * holds if the drivetrain is skipped. Run it anyway and the bike
	 * drives off from under its own rider.
	 *
	 * dm IS A PURE OUT-PARAMETER and nothing here reads it. The
	 * original has no such struct - the demand is three locals inside
	 * ApplyDrive, passed straight to TickDrivetrain.
	 */
	if (!r->crashLatch)
		racer_apply_drive(r, c, r->wheelieAngle, r->inputWord,
	                      r->axis0, race_pre_race(), &dm,
	                      &s_race_drive, &s_race_gearbox,
	                      dtScaled);

	/*
	 * THE SHIFT REQUEST GOES AFTER THE WHOLE DRIVETRAIN, and reads
	 * THIS frame's road speed.
	 *
	 * maybe_Physics_TickDrivetrain has no gearbox call at all.
	 * Gearbox_RequestAutoShift (0x00466730) and maybe_Gearbox_TickShift
	 * (0x004667d0) both sit inside maybe_Racer_UpdateAirborne, which
	 * maybe_Racer_Tick runs at 0x0045d7a6 - long past ApplyDrive at
	 * 0x0045d6ee. So the request sees the road speed THE TAIL left.
	 *
	 * ASKING EARLIER COSTS A FRAME PER SHIFT, in either direction.
	 * The original reads racer+0x12c part way through the frame, after
	 * the increment and before the tail lerps and drag pull it back
	 * down, so it sees a higher number than anything visible at a
	 * frame boundary - its shift timer already reads 136 of 200 on a
	 * frame whose end-of-frame speed is 17.32, below the 17.8 that
	 * requested it. racers-convergence.txt 9.46.18.
	 */
	/*
	 * AND NONE OF IT IN MANUAL.  Gearbox_RequestAutoShift opens on
	 * racer+0x88 and takes a different arm entirely when it is 0:
	 * build the shift table if it has never been built, then RETURN.
	 * It never requests.  Without this the game shifts for the player
	 * and fights the buttons.
	 *
	 * THE driveDisengaged TERM IS THE ORIGINAL'S OTHER HALF - the full
	 * condition at 0x0048e550 is
	 *
	 *     racer+0x13c == 0 && state == 0
	 *
	 * and the port only had the second.  Nothing writes
	 * driveDisengaged yet, so it is a no-op today; it is here because
	 * it is half of a condition, not because it does anything.
	 */
	if (s_race_gearbox.mode != 0 && !r->driveDisengaged &&
	    s_race_gearbox.state == 0) {
		/*
		 * SHIFT ON ROAD SPEED, AGAINST THE SWEPT TABLE.
		 *
		 * Gearbox_RequestAutoShift @ 0x0048e550:
		 *
		 *     up    roadSpeed >  T[gear]
		 *     down  roadSpeed <  T[gear-1] - GB_DOWN_HYST
		 *
		 * T is built once by gearbox_build_shift_table from a power
		 * crossover - see its comment.
		 *
		 * T[gear] IS BELOW THE GEAR'S ASYMPTOTIC TOP, which defeats
		 * the obvious objection: road speed can never cross the gear
		 * top (true), so the test looks like it must be engine speed
		 * against moteurVitesseMax * zoneRouge (false). Measured,
		 * gear 1 reaches 17.887 against a T[1] of 17.8.
		 *
		 * THE HYSTERESIS IS A FLAT 2 m/s below the down threshold,
		 * not a fraction of the up one.
		 */
		float up   = s_race_shiftTable[s_race_gearbox.gear];
		float down = (s_race_gearbox.gear > 1)
		             ? s_race_shiftTable[s_race_gearbox.gear - 1]
		             : 0.0f;

		/*
		 * GROUNDED ONLY. Gearbox_RequestAutoShift is called from
		 * INSIDE maybe_Racer_UpdateAirborne's grounded arm - the
		 * airborne arm jumps over it at 0x00466688 and lands on
		 * 0x00466735, the manual latch - so a bike in the air
		 * never auto-shifts. The TICK below is outside that arm
		 * and does run in the air, which is how a shift already
		 * in flight still completes over a jump.
		 */
		if (r->onGround) {
			if (up > 0.0f && s_race_drive.gearboxRoadSpeed > up)
				gearbox_request_upshift(&s_race_gearbox, c);
			else if (down > 0.0f
			         && s_race_drive.gearboxRoadSpeed
			            < down - GB_DOWN_HYST)
			    gearbox_request_downshift(&s_race_gearbox);
		}
	}

	/*
	 * AND ONLY NOW THE TIMER, AFTER THE REQUEST.
	 * maybe_Racer_UpdateAirborne calls Gearbox_RequestAutoShift at
	 * 0x00466730 and Gearbox_TickShift at 0x004667d0 - that order, one
	 * function, one frame.
	 *
	 * TICKING FIRST COSTS A FRAME ON EVERY SHIFT, because the frame a
	 * shift is ASKED FOR then never counts against its own clutch
	 * delay.  Known answer, coasting from 20 m/s in gear 3: both sides
	 * request the downshift at once and count 300 ms down, and MOTO
	 * reads 236 after two frames.  A tick behind reads 267 and lands
	 * the gear two frames late.  racers-convergence.txt 9.18.28.
	 */
	if (gearbox_tick_shift(&s_race_gearbox, c, (int)dtMs, 0,
	                       s_race_shiftTable,
	                       s_race_drive.gearboxRoadSpeed)) {
		/*
		 * THE GEAR MOVED; RESYNC BOTH SIDES THROUGH IT, and it is
		 * a ROUND TRIP, not one conversion. Both of
		 * maybe_Gearbox_TickShift's arms end the same way:
		 *
		 *     engine = SpeedToEngine(&roadSpeed, gear, engine)
		 *     roadSpeed = EngineToSpeed(&engine, gear, roadSpeed)
		 *
		 * so the two come out of a change AGREEING through the new
		 * ratio. Doing only the first half leaves the road speed on
		 * the OLD gear's terms while the engine has moved to the
		 * new one - the same defect, in the same shape, as the
		 * drivetrain seam in racers-convergence.txt 9.46.18.2.
		 *
		 * IT BELONGS INSIDE gearbox_tick_shift, where the original
		 * has it; it is here because that function has no
		 * drivetrain. [open - pass the drivetrain in]
		 */
		s_race_drive.engineSpeed =
		        physics_speed_to_engine(c, s_race_gearbox.gear,
		                                &s_race_drive.gearboxRoadSpeed,
		                        s_race_drive.engineSpeed,
		                        s_race_gearbox.state != 0);
		s_race_drive.gearboxRoadSpeed =
		        physics_engine_to_speed(c,
		                        s_race_racers[s_race_localIndex].realSpeedMax,
		                                s_race_gearbox.gear,
		                                &s_race_drive.engineSpeed,
		                                s_race_drive.gearboxRoadSpeed);
	}

	/*
	 * AND STRAIGHT BACK INTO THE RACER, because the drive step is
	 * finished and the rest of the tick reads r->speed, not the
	 * mirror.
	 *
	 * THE MIRROR IS OURS, NOT THE ORIGINAL'S. MOTO has no
	 * s_race_drive: maybe_Physics_TickDrivetrain writes racer+0x124
	 * directly, so every later reader in the frame sees this frame's
	 * speed. Carry it in the mirror until the later drive_store
	 * instead, and racer_apply_steering reads LAST frame's r->speed -
	 * its digital branch is gated on speed >= 0.5556, so turning in
	 * from rest it sees zero and winds nothing on for a frame.
	 * Ordering the steering after the drive step does not fix that on
	 * its own; this writeback is the half that carries the value.
	 * racers-convergence.txt 9.18.26.
	 *
	 * The later drive_store stays: anything after this that touches
	 * the mirror still needs it, and storing twice is idempotent.
	 */
	drive_store(&s_race_drive, r);

	/*
	 * THE BARS AND THE QUANTISE RUN AFTER THE DRIVE STEP, and the
	 * reason is a split rather than an ordering difference.
	 * THE ORIGINAL'S ORDER IS THE SAME; ITS ApplyDrive IS NOT.
	 * maybe_Racer_Tick calls ApplyDrive at 0x0045d713 and
	 * ApplySteering at 0x0045d743, but maybe_Racer_ApplyDrive's ONLY
	 * callee is maybe_Physics_TickDrivetrain - so by the time the
	 * original steers, the speed is already this frame's. The port
	 * splits those two, so the steering moves instead. The writeback
	 * above carries the other half; racers-convergence.txt 9.18.26.
	 *
	 * racer_apply_steering maintains r->steering, which is
	 * what a heading has to be built from - the arrows moving the
	 * lane below is a placeholder that predates it.
	 *
	 * dtScaled IS THE dt THE ORIGINAL USES HERE, calibrated against
	 * MOTO.EXE rather than assumed: roueAccAngulaire is 10.0 and
	 * steerRate read exactly 165.0 after five frames of held left,
	 * so 5 * 10 * dt = 165 and dt = 3.3 - which is dtScaled at
	 * 33 ms. The lock stop agrees too, both sides saturating at
	 * roueLimiteAngulaire = 600.0. racers-convergence.txt 9.61.
	 *
	 * The original gates this on crashLatch and on the grid having
	 * cleared; steering is dead on the grid. We have crashLatch.
	 */
	if (!r->crashLatch)
		racer_apply_steering(r, c, r->inputWord, r->axis1,
	                         physics_slip_speed(r->matOrientation,
	                                            r->velDir, r->speed),
	                         dtScaled);

	/*
	 * THE QUANTISE BELONGS HERE, NOT AT THE END OF THE TICK.
	 * maybe_Racer_ApplySteering calls Racer_ComposeOrientation at
	 * 0x0046664a and ComposeOrientation's tail is the quantise, so
	 * the original quantises BEFORE UpdateGroundContact re-fits the
	 * matrix to the terrain - and the ground fit has the last word.
	 *
	 * MEASURED with a watchpoint on matOrientation[6], three writes
	 * per frame in this order:
	 *
	 *     0045cb58   the multiply copy   -1.19e-12
	 *     0045cca6   the rebuild store   -1.19e-12
	 *     00469738   the ground fit       0.0
	 *
	 * so MOTO's rebuild produces the SAME table residue ours does
	 * and then throws it away. Running the quantise last instead
	 * kept the residue, and that single 1e-12 is what eventually
	 * flipped a pose pass from COPY to BLEND. See
	 * docs/racer-draw-audit.txt 50.
	 */
	racer_orient_quantise(r->matOrientation);

	/*
	 * RE-APPLY THE LIMITER AFTER THE INTEGRATION, because it has to
	 * be the last word on the frame.
	 *
	 * The original calls maybe_Gearbox_SpeedToEngine from SIX sites
	 * INSIDE maybe_Physics_TickDrivetrain, so the clamp is woven
	 * through the integration. That function is 4284 bytes and is
	 * not transcribed, so the clamp runs here instead.
	 *
	 * APPLYING IT ONLY BEFORE THE TICK IS USELESS - the integration
	 * overwrites the clamp a few lines later, and first gear runs to
	 * 18.447 m/s against a top of 0.30 * 61 = 18.30. THE ENGINE READS
	 * A SATURATED 7.000 THROUGHOUT either way, which is what makes
	 * that easy to mistake for a working limiter.
	 *
	 * The two speeds are ONE value in the original - racer+0x124
	 * and +0x12c read identically through a whole dwell - so the
	 * road side follows the gearbox side down.
	 * [open: the six in-tick call sites]
	 */
	/*
	 * NOT WHILE THE CLUTCH IS OUT. The original guards every tail
	 * resync with !bVar3, so during a shift the engine keeps the
	 * value it free-revved to instead of being recomputed from a
	 * road speed it is no longer coupled to.
	 */
	if (s_race_gearbox.state == 0)
		s_race_drive.engineSpeed =
	            physics_speed_to_engine(c, s_race_gearbox.gear,
	                                    &s_race_drive.gearboxRoadSpeed,
	                            s_race_drive.engineSpeed,
	                            s_race_gearbox.state != 0);
	/*
	 * DO NOT COUPLE speed TO gearboxRoadSpeed HERE. THE ORIGINAL RUNS
	 * WITH speed ABOVE IT - two frames after a rail glance MOTO reads
	 * 7.9154 against 7.8916, because velDir carries a 0.0774 lateral
	 * and the tail lerp takes the longitudinal, 0.997001 of the speed.
	 * Any clamp of one to the other throws that split away every
	 * frame. racers.txt 9.18.23.3.
	 */

	/*
	 * THE WHEELIE, on the racer itself. racer_update_wheelie is
	 * maybe_Racer_UpdateWheelie brought over whole, so the angle,
	 * the timer, the torque scale and the landing thump all land in
	 * the racer fields the other seven consumers read.
	 *
	 * KEEP THEM ON THE RACER. Mirroring the angle into a side struct
	 * matches MOTO to four decimals while the timer sits at 0, so the
	 * 200 cap never fires, the wheelie never ends, and the 2.0 torque
	 * scale never reaches the drivetrain.
	 *
	 * THE LATCH GOES FIRST because it produces this function's
	 * input. Zero for the countdown: MR_RaceClock is written but
	 * nothing instantiates it yet, so there is no countdown to be
	 * in. [open]
	 */
	racer_latch_stunt_request(r, 0);
	racer_update_wheelie(r, dtScaled);

	/*
	 * THE SPRING, tenth in the tick order and the CONSUMER of
	 * springCompression - see racer.h. It has to run after the
	 * wheelie, which is what kicks that field on touchdown, and
	 * it reads matBikeRoot, which the draw rebuilds from the
	 * wheel contacts.
	 *
	 * dtScaled IS g_frameDelta HERE. The original reads the
	 * global at 0x00581014 and clamps it to [3, 5]; ours is 3.3
	 * at 33 ms, the same unit and inside the same window.
	 */
	racer_update_suspension(r, c, dtScaled);

	/* And back out - see drive_store. */
	drive_store(&s_race_drive, r);


	/*
	 * AND THE ENGINE SPEED INTO racer+0x00ec. Left in the mirror it
	 * reads 0 to every A/B and to every imported function expecting it
	 * there, which looks like a dead engine beside MOTO's 0.7 idle -
	 * see drive_store's header. racer-draw-audit.txt 8.
	 */
	r->engineSpeed = s_race_drive.engineSpeed;

	/* The heading below advances segPos; this is its scale. */
	segLen = racer_segment_length(&s_race_trk, (int)s_race_segPos);

	/*
	 * FREE BODY, IN TRACK COORDINATES. The bike now goes where it
	 * POINTS rather than where the rail points: the steering torque
	 * turns a world heading, and the heading is resolved against the
	 * current segment each frame so a curving track carries the bike
	 * round without the heading having to be re-based.
	 *
	 * Keeping segPos/lateral rather than a free world position is
	 * deliberate - the segment lookup, the ground snap and the wall
	 * clip all work in these coordinates already, and locally the two
	 * are the same thing.
	 *
	 * +lateral IS TO THE RIGHT: row0 of a segment matrix is its
	 * lateral axis, and a left turn RAISES the yaw that
	 * racer_matrix_yaw reports while moving the bike to -lateral.
	 * Verified against MOTO.EXE - racers-convergence.txt 9.61.
	 */
	{
		/*
		 * THE SEGMENT'S OWN YAW, read from the track and NOT from
		 * r->matOrientation, which at this point still carries LAST
		 * frame's rotation. Using it feeds the heading back into
		 * its own reference: the body ends up at yawWorld minus the
		 * previous yawRel, the travel frame is skewed the other way,
		 * and the slip comes out with the sign reversed. Measured
		 * -0.300 where MOTO.EXE reads +0.269.
		 */
		float segYaw;

		{
			int   n  = s_race_trk.segmentCount;
			float sp = (float)fmod((double)s_race_segPos, (double)n);
			int   si;

			if (sp < 0.0f)
				sp += (float)n;
			si = (int)sp;
			if (si < 0)  si = 0;
			if (si >= n) si = n - 1;
			segYaw = racer_matrix_yaw(s_race_trk.segments[si].matrix);
		}

		/*
		 * QUANTISE THE HEADING TO THE ENGINE'S 14-BIT ANGLE.
		 *
		 * The original never carries a full-precision orientation. Every
		 * frame FUN_0045cb00 multiplies matOrientation, decomposes it to
		 * Euler angles, converts each with __ftol and REBUILDS the
		 * matrix out of g_cosTable[i & 0x3fff] - so the stored
		 * orientation can only ever hold multiples of 2*pi/0x4000, and
		 * __ftol TRUNCATES toward zero rather than rounding.
		 *
		 * That truncation is a systematic lag, and it is measurable:
		 * carrying the heading in full precision ran the port a flat
		 * 1.0077 times the original through turn entry, constant from
		 * frame 2 and with no trend as speed rose. One table step
		 * against the per-frame yaw increment there is
		 * 0.000383 / 0.0504 = 0.76 per cent.
		 *
		 * See trig-table.txt: 0x4000 to the turn, 0x1000 to the
		 * quarter, wrapping by AND rather than fmod.
		 */
		s_race_yawIndex = race_ang_from_rad(s_race_yawWorld);
		s_race_yawWorld = (float)((double)s_race_yawIndex
		                          * TRIG_RAD_PER_IDX);
		/*
		 * THE SLIP IS THE INTEGRATOR'S. racer_integrate blends moveDir
		 * towards the matrix forward at a weight clamped to
		 * [0.85, 0.9] - measured identical to MOTO's, 0.85 flat at
		 * racing speeds, closing 15 per cent of the gap a frame.
		 * racers.txt 9.29.12.5.
		 *
		 * DO NOT REPUBLISH moveDir AFTER IT from a yaw scalar. The
		 * wall clip's bounce assigns moveDir the reflected direction,
		 * so a later republish discards it on the next tick and the
		 * bike can never leave the rail - it shows as moveDir flipping
		 * to +65.9 on the bounce and back to -62.9 a frame later.
		 */

		/*
		 * FREE POSITION: MOVE, THEN REACT, which is the original's
		 * arrangement. The bike integrates along its own travel
		 * direction and the track is asked afterwards where it ended
		 * up - ModelInstance_Integrate advances the position in all
		 * three axes, gravity included, and
		 * maybe_Racer_UpdateGroundContact then does everything else:
		 * the two-pass wall clip, the crash decision, the ground snap,
		 * the frame rebuild, the wheel contacts and the pending-yaw
		 * drain.
		 *
		 * positionPrev IS THE MOVE'S `from`, which the import
		 * subtracts from position to get the frame's displacement.
		 * racer_save_prev_frame shifts it at the top of the tick,
		 * where maybe_Racer_Tick does (0x0045d5bf), and NOTHING
		 * BETWEEN THERE AND HERE MOVES position - the mover is
		 * racer_integrate just below - so it still holds the `from`.
		 */
		{
			/*
			 * THE MOVER IS racer_integrate - ModelInstance_Integrate
			 * whole. It owns the heading rebuild, the slip blend, the
			 * low-speed damping, the gravity on both branches, the
			 * wall slide push and the Euler add.
			 *
			 * THE GROUNDED BRANCH DOES NOT INTEGRATE GRAVITY AT ALL:
			 * it adds a CONSTANT dt * -1255.68 straight to the
			 * displacement, where -1255.68 is -128 * 9.81, gravity in
			 * units per metre. 0.033 * -1255.68 = -41.437. The
			 * AIRBORNE formula on this path instead - 2.5 g into the
			 * polar velocity - gives about -3.4 a frame against
			 * MOTO's measured -41.4374. port-divergences 2.13.
			 */
			/*
			 * THE LIFT GATE IS PersoAnim AND A STRAIGHT, and it
			 * is read straight off 0x0045d748..0x0045d762:
			 *
			 *     EBX = 0
			 *     if (PersoAnim) {
			 *         if (!Racer_AiIsCorner(this, segmentStart, 8))
			 *             EBX = 1
			 *     }
			 *     ModelInstance_Integrate(..., EBX)
			 *
			 * A ROAD BIKE IS NEVER ALLOWED TO LIFT, and neither is
			 * a dirt bike in a corner - only a dirt bike on a
			 * straight. That is why the jumps exist on the dirt
			 * arena and nowhere else.
			 *
			 * PASS A LITERAL 0 AND THE DOWNWARD PUSH APPLIES on every
			 * frame of every bike, so the player can never leave the
			 * ground however steep the crest.
			 *
			 * AND IT IS THE SAME CORNER TEST THE AI USES.
			 * Racer_AiIsCorner was imported for maybe_Racer_TickAI;
			 * maybe_Racer_Tick calls it for the PLAYER at
			 * 0x0045d759 with the same eight-segment look-ahead.
			 *
			 * *** AND EBX IS PUSHED TWICE. *** It is computed once at
			 * 0x0045d748 and handed to BOTH calls - Integrate at
			 * 0x0045d76b and UpdateGroundContact at 0x0045d799 - with
			 * nothing in between touching it. So the lift gate and the
			 * AIRBORNE THRESHOLD are one flag, not two:
			 *
			 *     0046942c  EDI = the pushed flag
			 *     00469427  EAX = 32
			 *     00469437  EAX = 8      when the flag is set
			 *
			 * WITHOUT THE SECOND USE THE FIRST CANNOT SHOW. The lift
			 * raises the bike 26 units over a crest and the contact then
			 * needs 32 to call it airborne, so it snaps the bike back
			 * down and the jump is invisible. Measured on the dirt
			 * arena's big ramp from an identical frame on both sides:
			 * MOTO went airborne with y 26 above the ground and ours
			 * stayed pinned. racers-convergence.txt 9.46.12.
			 */
			bLiftAllowed = c->persoAnim
			               && !racer_ai_is_corner(&s_race_trk,
			                                      r->segmentStart, 8,
			                                      g_reverseEnabled
			                                          ? -1 : 1);
			racer_integrate(r, c, dtScaled, bLiftAllowed);
		}

		r->frameDelta = dtScaled;

		/*
		 * THE CORNERING GOES HERE, between the integrate and the
		 * ground contact, because that is where maybe_Racer_Tick
		 * has it - Integrate 0x0045d770, ApplyCornering 0x0045d792,
		 * UpdateGroundContact 0x0045d79c.
		 *
		 * THE ORDER IS THE POINT. racer_integrate has just seeded
		 * velDir from the heading; this is what then gives velDir a
		 * LATERAL component, through physics_set_slip_speed. Run it
		 * anywhere else and velDir is only the heading plus
		 * quantisation residue. fx-particles.txt 10.4.1.
		 */
		racer_apply_cornering(r, &s_race_trk, c, dtScaled);

		(void)racer_update_ground_contact(r, &s_race_trk, c,
		                                  &s_race_gearbox, 0,
		                                  bLiftAllowed,
		                                  s_race_noWalls);

		/*
		 * AND THE AIR TIMER AND BRAKE DIVE, at MOTO's own place in
		 * the order: maybe_Racer_Tick runs UpdateGroundContact at
		 * 0x0045d79c and UpdateAirborne at 0x0045d7a6, so this reads
		 * the onGround the contact just wrote.
		 *
		 * IT IS THE ONLY THING THAT PITCHES matOrientation ON A
		 * GROUNDED BIKE. Without it the matrix is a pure yaw with row1
		 * exactly (0,1,0), which makes physics_slip_speed a STRUCTURAL
		 * zero - row0 . row2 is zero by construction - so the skid
		 * ribbon never widens and the grip curve never leaves 1.0.
		 * See racer_update_airborne.
		 */
		racer_update_airborne(r, r->inputWord | r->inputWord2,
		                      &s_race_gearbox, c, race_frame_delta());

		/*
		 * THE FORK DIVE, and with it steerReturnScale - the
		 * longitudinal ACCELERATION, which racer_apply_steering
		 * multiplies the self-centring rate by. Left at zero the bars
		 * centre at the plain roueVitesseRetour and take twice as long
		 * to come back: MOTO clears the 600 lock stop in 3 frames with
		 * steerReturnScale 14.27, and 6 with it at 0.
		 *
		 * speedTail is the PREVIOUS frame's longitudinal speed. The
		 * original lerps racer+0x108 toward it by FUN_0048eb40,
		 * which for a MotoType other than 1 returns max(1.0,
		 * MinAdherenceWheel) - so for this bike the tail tracks
		 * exactly and the difference is one frame of speed change.
		 *
		 * IT RUNS HERE, AFTER THE GROUND CONTACT AND THE AIRBORNE
		 * STEP, because that is where maybe_Racer_Tick puts it -
		 * s_tickOrder in racer.c, entry 8, against UpdateGroundContact
		 * at 6 and UpdateAirborne at 7. THE WALL IMPACT SCRUBS THE
		 * SPEED INSIDE THE GROUND CONTACT, so running the fork before
		 * it samples the speed the impact has not touched yet and the
		 * dive turns up a whole tick late.
		 *
		 * The tell on a wall ride is steerReturnScale with the WRONG
		 * SIGN on a contact frame - +20.12 where MOTO reads -81.98,
		 * a deceleration not yet sampled. racers-convergence.txt 9.18.38.
		 *
		 * THE DECAY FIRST, then the peak hold. forkDive is a peak
		 * hold RACING a fixed decay - maybe_Racer_Tick shrinks it by
		 * 0.01 a frame at 0x0045d63e - and without the decay it just
		 * latches at the +-PI/16 clamp and stays there. Measured
		 * pinned at +0.19635 with MOTO.EXE reading -0.0439.
		 */
		{
			/*
			 * r->speed, NOT THE MIRROR. The original reads the
			 * speed live off the body instance - CALL 0x0048ca10
			 * on racer+0x24 at 0x004667ec, then FSUB racer+0x2da8
			 * for the tail - so it sees whatever the wall impact
			 * just did. drive_store pushed the mirror out BEFORE
			 * the ground contact ran, so it holds the PRE-IMPACT
			 * speed and the dive misses the hit even with the call
			 * in the right place.
			 */
			racer_decay_fork_dive(r);
			racer_update_fork_dive(r, r->speed,
			                       s_race_prevLongSpeed,
			                       &r->prevLongAccel, dtScaled);
			s_race_prevLongSpeed = r->speed;
		}

		/*
		 * THE WHEELS TURN HERE, after the contacts move. Each tick
		 * variant in the original calls Racer_SpinWheels once -
		 * maybe_Racer_Tick at 0x0045d7b4, TickRemote at 0x0045f0f6,
		 * TickAI at 0x0046c74d - and it reads
		 * rearWheelContact against rearWheelContactPrev, so it has
		 * to run after racer_update_ground_contact has set both.
		 *
		 * racerdraw_build_matrices feeds rearWheelSpin and
		 * frontWheelSpin into the two wheel chains, and this is
		 * the only writer of either - without it the wheels stand
		 * still.
		 */
		racer_spin_wheels(r, c, race_frame_delta());

		/*
		 * AND THE SHADOW IS BUILT HERE, BEFORE THE LEAN. The order in
		 * maybe_Racer_Tick is UpdateGroundContact (0x0045d79c),
		 * SpinWheels (0x0045d7b4), BuildShadow (0x0045d7c2), then
		 * UpdateLean (0x0045d7c9) - and UpdateLean is what swings the
		 * front contact up for a wheelie.
		 *
		 * SO THE ORIGINAL'S SHADOW NEVER SEES A RAISED FRONT WHEEL.
		 * UpdateGroundContact puts the contact back on the road every
		 * frame, the shadow is built from that, and only afterwards
		 * does the lean lift it again. Measured through a stunt:
		 * MOTO's front contact reaches y 118.79 with the wheels 118.8
		 * apart horizontally and its shadow stays 248.0 units long,
		 * unchanged from standing still.
		 *
		 * BUILDING IT AFTER THE TICK hands the raised contact to the
		 * shadow and shortens it to 198.8 - a change the original does
		 * not make. racer-draw-audit.txt 62.
		 */
		racerdraw_build_shadow(r, &s_race_trk);

		/*
		 * AND THE SPEED COMES BACK. r->speed IS racer+0x124, the one
		 * speed the original has: its drivetrain reads and writes that
		 * field directly, so a wall that scrubs it is felt by the next
		 * tick's drive.
		 *
		 * THE MIRROR MUST BE RELOADED, NOT LEFT. Everything the ground
		 * contact wrote to r->speed - the wall's 0.75 damping, the 0.3
		 * scrape, the crash floor - is lost if the mirror carries the
		 * value from the top of the tick into the next drive, and the
		 * bike keeps its throttle straight through the rail. The crash
		 * update writes it again later and reloads again there.
		 */
		drive_load(&s_race_drive, r);
		/*
		 * AND THE GEAR. The original keeps it at racer+0x7c and reads
		 * it there - BeginCrash puts it back to first, ResetCrash
		 * resyncs the engine from it - while ours lives in MR_Gearbox,
		 * so an unmirrored field reads 0 to every A/B through a whole
		 * upshift.
		 *
		 * Retiring MR_Gearbox the way MR_Crash went is the real fix;
		 * this keeps the field truthful until then. [open]
		 */
		r->gear = s_race_gearbox.gear;

		/*
		 * THE ATTITUDE IS THE TERRAIN'S, not the segment's:
		 * racer_update_ground_contact is import 1, whole, and fits
		 * matOrientation to two ground probes. The fit is verified bit
		 * for bit against MOTO - racers-convergence.txt 9.18.32. This line only
		 * carries the segment index across.
		 *
		 * segmentStart (+0x31c1) IS THE SEGMENT THE BIKE IS ON, and is
		 * what every reader here wants. Its neighbour +0x31bd is the
		 * PREVIOUS segment, kept a frame behind by race_update_progress
		 * so the delta between the two is the step - COPY ONE OVER THE
		 * OTHER AND THAT DELTA IS PERMANENTLY ZERO, which stops
		 * progress, the high-water mark and the checkpoint sweep
		 * together, silently.
		 */
		s_race_segPos = (float)r->segmentStart;

		/*
		 * THE MATRIX BELONGS TO THE IMPORTED CHAIN. The original's
		 * division of labour is ApplySteering yaws the matrix,
		 * UpdateGroundContact re-fits it to the terrain, and the yaw
		 * survives that fit because the probes are laid out along the
		 * previous forward axis.
		 *
		 * SO NOTHING HERE MAY WRITE matOrientation. Rebuilding it from
		 * the segment matrix and the heading throws the terrain fit
		 * away every frame; even re-quantising it here does, in a
		 * subtler form, by carrying the angle table's 1e-12 at the
		 * quarter turns. The quantise belongs next to
		 * racer_apply_steering, where the original has it.
		 *
		 * s_race_yawWorld IS A CACHE, NOT THE STATE. It is read BACK
		 * from the matrix so it reflects the ground fit, and
		 * ComposeOrientation's tail has already quantised through the
		 * angle table, so it can only ever be a table entry.
		 */
		s_race_yawWorld = racer_matrix_yaw(r->matOrientation);
		s_race_yawIndex = race_ang_from_rad(s_race_yawWorld);
	}

	/*
	 * THE LEAN: yaw(matOrientation) against yaw(the frame the wheel
	 * contacts imply) - a heading discrepancy, which on a turning bike
	 * is the turn rate expressed geometrically. racers.txt 9.60. dt is
	 * milliseconds, floored at 20 for a digital rider by
	 * racer_lean_response.
	 *
	 * IT RUNS AFTER THE MOVE, which is the original's order -
	 * UpdateGroundContact then UpdateLean. Run before it, the frame it
	 * publishes as matBikeRoot is built from the PREVIOUS frame's
	 * travel direction and the drawn bike trails the real one by a
	 * whole frame: 0.047 rad behind yawMove, against about 0.05 for
	 * one frame of heading.
	 *
	 * The three yaws it publishes are diagnostics that race_log and
	 * mr_place read, so they are copied back into the statics.
	 */
	{
		MR_LeanDbg dbg;

		racer_update_lean(r, c, &s_race_trk, s_race_drive.speed,
		                  (float)dtMs, race_replay_mode(), &dbg);
		s_race_yawOrient = dbg.yawOrient;
		s_race_yawFrame  = dbg.yawFrame;
		s_race_leanDelta = dbg.leanDelta;
	}

	/*
	 * AND THE CRASH UPDATE, HERE AND NOT EARLIER. maybe_Racer_Tick
	 * calls maybe_Racer_UpdateCrash at 0x0045d7d0 - AFTER
	 * maybe_Racer_UpdateGroundContact at 0x0045d79c and after
	 * UpdateLean at 0x0045d7c9, and immediately before the skeleton
	 * and the wheel FX.
	 *
	 * WHY THE POSITION MATTERS: the crash BEGINS inside
	 * racer_update_ground_contact. Running the update before it
	 * means that on the frame the bike goes down crashLatch is
	 * still 0, so the first 0.9 is never applied - and the bike
	 * carries 1/0.9 too much speed for the whole slide afterwards.
	 *
	 * MEASURED, shallow-angle wheelie crash at speed 70. The frame
	 * of the impact, both entering it at 72.22:
	 *
	 *     MOTO   72.23 -> 34.815     shed 37.41
	 *     ours   72.23 -> 38.568     shed 33.66,  a ratio of 0.8996
	 *
	 * one whole CRASH_DECAY short, and every later value carried
	 * the same 1.108 offset while decaying at the correct 0.81.
	 * racers.txt 9.65.
	 */
	if (r->crashLatch) {
		r->frameDelta = dtScaled;
		if (racer_update_crash(r, &s_race_trk) == RACER_CRASH_DONE)
			/*
		     * THE VIEWED RACER IS 1 because that is the only one
		     * this port simulates or watches. The original reads
		     * g_viewedRacerIndex, which also drives the gate that
		     * keeps the field down together; with one racer the
		     * gate is trivially open. [open - when the AI runs]
		     */
		    (void)race_reset_crashes(r->slotIndex, 1);
		/*
		 * racer_update_crash writes r->speed directly, so the
		 * mirror is reloaded rather than left holding the pre-crash
		 * value. The drive step and its drive_store have both
		 * already run by this point, so this is what keeps anything
		 * reading the mirror later in the frame in step.
		 */
		drive_load(&s_race_drive, r);
	}

	/*
	 * AND THE WHEEL FX, AFTER THE LEAN. maybe_Racer_Tick calls
	 * maybe_Racer_EmitWheelFx at 0x0045d7e4, after UpdateLean
	 * (0x0045d7c9) and UpdateSkeleton (0x0045d7d8) - so it sees the
	 * SWUNG contacts, which is the opposite of the shadow above and
	 * is why the two cannot share a call site.
	 *
	 * The original reads the viewed racer and g_frameDelta as globals;
	 * the port has index 1 as the local player and race_frame_delta().
	 */
	/* Not while probing - see s_race_dryRun. */
	if (!s_race_dryRun) {
		racer_emit_wheel_fx(r, r->inputWord | r->inputWord2,
		                    &s_race_trk,
		                    s_race_racers[s_race_localIndex].segmentStart, 1,
		                    race_frame_delta());
	}

	/*
	 * THE DASH, at 0x0045d7eb - the call immediately before
	 * maybe_Trk_LaneOfRacer in maybe_Racer_Tick. It owns speedKmhRaw,
	 * speedKmhNeedle and tachoRpm.
	 *
	 * THE PLAYER TAKES ITS EARLY RETURN - humanPlayer and soloMode
	 * both set - so the speedo comes off the REAR CONTACT's travel and
	 * the tacho off the engine, and the gearbox_auto_select arm below
	 * it is the AI's.
	 */
	racer_update_dash(r, c, r->frameDelta);

	/*
	 * THE PLAYER'S LANE IS MEASURED, NOT CHOSEN - maybe_Trk_LaneOfRacer
	 * at 0x0045d7f2. It exists so the AI's blocking and overtaking can
	 * see the human as traffic; without it the player reads as lane 0
	 * forever and every AI treats it as being on the inside line. Can
	 * be -1, which racer_ai_lane_step's compares tolerate as the
	 * original's do.
	 */
	r->lane = racer_lane_of_position(r, &s_race_trk);

	/*
	 * SNAP TO THE ROAD SURFACE - AND THIS SEAM IS OURS.
	 * maybe_Racer_Tick has no snap here: the original snaps in exactly
	 * two places, the grounded branch of UpdateGroundContact
	 * (0x0046947d) and, for a ROAD bike only, the wheel contacts
	 * inside UpdateLean. Nothing after UpdateLean touches racer+0x34.
	 *
	 * MEASURED A NO-OP over 40 frames of the arena straight through a
	 * jump: segment equalled segmentStart on every one, and nothing
	 * between the contact and here moves position, so this recomputes
	 * the height the contact already wrote. Kept only because the two
	 * fields are maintained by different code and a corner or a wall
	 * clip has not been checked. [open - delete it once segment and
	 * segmentStart are shown to agree everywhere]
	 *
	 * THE onGround GUARD IS NOT OPTIONAL. A grounded bike has no
	 * independent height - the grounded branch and the AI tick both
	 * assign position[1] from Trk_GroundHeightAt outright rather than
	 * integrating a vertical velocity - but the AIRBORNE branch does,
	 * and re-pinning y without the guard means the player can never
	 * leave the ground however big the jump.
	 *
	 * The surface test covers a track whose planes were never built,
	 * which would otherwise drop the rider to y = 0.
	 */
	if (r->onGround
	    && trk_segment_has_surface(&s_race_trk, r->segmentStart)) {
		r->position[1] = trk_ground_height_at(&s_race_trk, r->segmentStart,
		                                 r->position);
	}

	/*
	 * THE SPEED COPY IS NOT PART OF THE SNAP, and must not be put
	 * inside the onGround guard with it.
	 *
	 * physics_drivetrain_tail applies the drag to the DRIVETRAIN
	 * mirror, and the airborne drag is 1.6 against 0.3 on the ground.
	 * racer_integrate's airborne arm then overwrites r->speed with
	 * |moveDir| - the projectile's velocity magnitude, which GROWS as
	 * the bike falls. Skip this copy in the air and the drag never
	 * reaches the racer, so the bike ACCELERATES through the whole
	 * flight: off the first ramp MOTO runs 32.28 -> 30.00 over six
	 * frames where that reads 33.48 -> 40.43.
	 */
	if (trk_segment_has_surface(&s_race_trk, r->segmentStart)) {

		/*
		 * DO NOT APPLY THE SLOPE HERE - physics_drivetrain_tail
		 * has it, and a second application doubles it. The
		 * original adds it to the longitudinal scalar at
		 * 0x0048fdf0, before the roll and before
		 * Racer_SetLongSpeed. racers.txt 9.18.23.1.
		 */
		if (s_race_drive.speed < 0.0f)
			s_race_drive.speed = 0.0f;
		/*
		 * AND STILL NO COUPLING - see the limiter block above. The
		 * lerp at 0x00490140 converges the two by the GRIP FACTOR,
		 * which is 1.0 only while the bike is going straight, so
		 * forcing them equal here erases the one frame they come
		 * apart: entering a left-hander under brakes MOTO reads
		 * speed 19.66401 against gearboxRoadSpeed 19.65801, then
		 * re-converges. racers.txt 9.18.23.
		 */
		r->speed = s_race_drive.speed;
	}

	/*
	 * AND THE TICK LATCHES ITS BUTTONS LAST. maybe_Racer_Tick stores
	 * them into racer+0x2dc8 at 0x0045d7fd, the final instruction of
	 * its call list - AFTER maybe_Racer_EmitWheelFx has read the old
	 * value at 0x004671fa. That is what makes tickInputWord the
	 * PREVIOUS frame's word and the pair a rising-edge detector.
	 *
	 * LEAVE IT UNWRITTEN AND (tickInputWord & 2) == 0 IS PERMANENTLY
	 * TRUE, so the brake edge fires on EVERY frame of a brake rather
	 * than the first. The emitter then allocates a fresh ribbon pair
	 * per frame, each gets a single append, and all sixteen sit at two
	 * vertices and zero quads - a skid mark needs two appends before
	 * it has a quad, so nothing draws at all.
	 *
	 * THE PORT HAS NO buttons ARGUMENT HERE - race_poll_input leaves
	 * the word on the racer instead - so this latches the same
	 * expression the dispatch and the emitter both use.
	 */
	r->tickInputWord = r->inputWord | r->inputWord2;
}


/*
 * ORIGIN  NONE - ours, bound in main.cpp.
 *
 * BOTH WALL SITES, and routing it through one flag is the point:
 * racer_update_ground_contact skips the clip AND the containment on
 * it. Gate only the clip and the bike still hangs on the rail, which
 * reads as the toggle not working.
 */
void race_walls_toggle(void)
{
	s_race_noWalls = !s_race_noWalls;
	mrlog("race: guard rails %s\n",
	      s_race_noWalls ? "OFF - riding through" : "on");
}


/*
 * ORIGIN  Input_OnKeyDown @ 0x00431250, at 0x00432084 - case 0x74,
 *         VK_F5, which HELP.GEN advertises as "CHANGE VIEW"
 *
 * The original is three instructions on the camera record:
 *
 *     MOV ECX, [EAX + 0x5163e0]    camera[active].nViewSlot
 *     INC ECX
 *     AND ECX, 0x3
 *
 * then maybe_Camera_SignalCut. Camera_UpdateChase adds
 * pViewBoom[nViewSlot] - seeded { 0, 128, 256, 512 } by
 * Camera_InitRecord - to flOrbitRadius, so with the -484 a live race
 * reads the four booms are -484, -356, -228 and +28. THE LAST ONE IS
 * POSITIVE: the pull-back crosses the bike origin and the eye ends
 * up just in FRONT of the rider, which is why retail's fourth view
 * shows neither the rider nor the bike.
 *
 * NOT F6. That toggles DAT_004dc00c and switches the MODE 7 -> 6 -
 * race_camera_toggle_first_person. See camera.txt 1.13.
 */
void race_camera_cycle_view(void)
{
	s_race_camViewSlot = (s_race_camViewSlot + 1) & 3;
	mrlog("race: chase view slot %d\n", s_race_camViewSlot);
}


/*
 * ORIGIN  Input_OnKeyDown @ 0x00431250, at 0x004320b8 - case 0x75,
 *         VK_F6, which HELP.GEN advertises as "1RST PERSON VIEW"
 *
 * DAT_004dc00c = 1 - DAT_004dc00c, then maybe_Camera_SignalCut.
 * Camera_SelectView reads it and asks for MODE 6 instead of 7.
 *
 * INDEPENDENT OF F5. Mode 6 has no boom, so the chase slot is simply
 * not read while first person is up and comes back untouched.
 */
void race_camera_toggle_first_person(void)
{
	s_race_camFirstPerson = !s_race_camFirstPerson;
	mrlog("race: camera %s\n",
	      s_race_camFirstPerson ? "FIRST PERSON" : "chase");
}


/*
 * ORIGIN  Input_OnKeyDown @ 0x00431250, case 9 (VK_TAB), and
 *         maybe_Input_OnKeyUp @ 0x00432700
 *
 * The original writes BOTH fields on the way down and clears both on
 * the way up:
 *
 *     0043149f   camera[idx].flLookYaw    = pi
 *     004314a9   camera[idx].nLookBackKey = 1
 *     004328e5   camera[idx].flLookYaw    = 0
 *     004328eb   camera[idx].nLookBackKey = 0
 *
 * flLookYaw IS NOT THE VIEW ANGLE and writing it changes nothing on
 * this path - it is read only on the slot-0 chase, where it shortens
 * the boom under the finish flag. Set anyway, because leaving pi in it
 * would shorten the boom the moment the key came up. camera.txt 1.17.
 */
void race_camera_look_back(int down)
{
	s_race_camLookSlot   = down ? CAM_LOOK_BACK : CAM_LOOK_CHASE;
	s_race_cam.lookYaw   = down ? 3.1415927f : 0.0f;
}


/* ORIGIN  NONE - ours. Retail has no side camera. */
void race_camera_toggle_side(void)
{
	/*
	 * OFF, LEFT, RIGHT. Which side is useful depends on which way
	 * the corner goes, and cycling costs nothing over a plain
	 * toggle.
	 */
	s_race_camSide = (s_race_camSide + 1) % 3;
	mrlog("race: camera %s\n",
	      s_race_camSide == 0 ? "chase"
	      : s_race_camSide == 1 ? "SIDE left" : "SIDE right");
}


/* ORIGIN  NONE - ours */
unsigned int race_tick_count(void)
{
	return s_race_ticks;
}


void race_update(void)
{
	float        eye[3], viewEye[3], target[3], lens;
	double       now;
	float        dt, sdt;

	if (!race_ready())
		return;

	/*
	 * THE NETWORK, ONCE A FRAME. Nothing else pumps it during a race
	 * - the lobby page did it while the lobby was up - so without
	 * this the link goes quiet the moment the race starts and every
	 * remote racer freezes where it stood.
	 */
	if (g_soloMode == 0) {
		netplay_poll();
		netplay_pump();
		race_net_send_state();
		race_net_check_over();
	}

	/*
	 * A PEER DECIDED THE RACE IS OVER, 0x6c. Checked every frame and
	 * not where the local racer closes its last lap, because the
	 * message can arrive long after that - the solo arm at 0x0047eb04
	 * fires on the lap itself and the network arm cannot.
	 */
	if (g_soloMode == 0 && s_race_overNet)
		s_race_overHud = 1;

	/*
	 * THE VIRTUAL CLOCK, NOT timeGetTime.
	 *
	 * tm_now stops while paused and runs at tm_speed otherwise, so the
	 * race integrates the same time the frame pacer does. The wall
	 * clock instead keeps the bike moving through a pause and ignores
	 * fast-forward - the two disagree exactly when the clock is being
	 * driven by hand, which is when anyone is watching.
	 *
	 * The first frame after a load has no previous stamp, so it takes
	 * a zero step rather than integrating whatever the clock held.
	 */
	/*
	 * THE IN-RACE MENU FREEZES THE RACE, and it does it here
	 * rather than through the pause. The original has no flag
	 * for this: the menu object carries +0x10, and
	 * InGameMenu_ServiceAll (0x004126a0) returns 0 to the
	 * frame loop as soon as it finds a menu with that set,
	 * which the caller reads as "do not tick the game".
	 *
	 * THE CLOCK IS RE-BASED ON THE WAY OUT, not left running.
	 * Returning without touching s_race_lastTime would hand
	 * the first frame after the menu closes the whole time
	 * the menu was up as one dt - a teleport. RACE_MAX_FRAME_DT
	 * below would cap it, which is worse: a capped jump looks
	 * like a physics bug rather than like a missing reset.
	 */
	/*
	 * THE AUDIO PAUSES WITH THE SIMULATION, and THERE ARE TWO WAYS
	 * TO STOP IT. hud_menu_active is the escape menu, which is the
	 * original's own pause; tm_paused is ours, stopping the virtual
	 * clock so the race does not integrate. Both mean the bike is not
	 * moving, so both must silence the engine - hook only the first
	 * and the virtual-clock pause leaves the note looping.
	 *
	 * SET EVERY FRAME, not on an edge: audio_set_paused ignores a
	 * call that does not change the state, so there is no edge to
	 * miss and no flag of our own to keep in step.
	 *
	 * ONLY LOOPS ARE HELD, which is the original's rule and not a
	 * shortcut - and it is why retail keeps its gulls and its surf
	 * going over a pause while the engine stops. Ambience is fired
	 * as one-shots, so nothing holds it. See audio.h.
	 */
	audio_set_paused(hud_menu_active() || tm_paused());

	/* AN OVERLAY IN A NET RACE, not a freeze - the others do not
	   stop, so neither may we. netplay-relay.txt 3.4. */
	if (hud_menu_active() && g_soloMode) {
		s_race_lastTime = tm_now();
		return;
	}
	now = tm_now();
	dt  = (s_race_lastTime > 0.0) ? (float)(now - s_race_lastTime) : 0.0f;
	s_race_lastTime = now;
	if (dt < 0.0f)
		dt = 0.0f;
	/* A stall must not teleport it - and see RACE_MAX_FRAME_DT
	 * for why this is the substep budget rather than a number. */
	if (dt > RACE_MAX_FRAME_DT)
		dt = RACE_MAX_FRAME_DT;

	/*
	 * QUANTISE THE STEP TO WHOLE MILLISECONDS, because that is all
	 * the replay record can hold - and a recording whose dt differs
	 * from the dt the SIMULATION used drifts apart from it.
	 *
	 * At 60fps the frame is 16.667 ms. Recording (int)(dt * 1000)
	 * stores 16 while the physics integrates 16.667, so a playback
	 * runs about four per cent short - which looks like nothing for
	 * the first seconds and has the bike turning early by the end
	 * of a lap. The fix is not to record more precisely, it is to
	 * STEP in whole milliseconds so the record is exact.
	 *
	 * THE REMAINDER IS CARRIED, so the sequence runs 17, 17, 16,
	 * 17 ... and averages the true frame time. Truncating without
	 * the carry would make the race itself run four per cent slow -
	 * self-consistent, and wrong against the clock.
	 *
	 * The original never faces this: its frame delta IS an integer
	 * millisecond count (maybe_Game_Tick's __ftol), and the replay
	 * records that very value.
	 */
	if (race_replay_mode() != REPLAY_MODE_PLAY) {
		int ms;

		s_race_msCarry += dt * 1000.0f;
		ms = (int)s_race_msCarry;
		if (ms < 0)
			ms = 0;
		s_race_msCarry -= (float)ms;
		dt = (float)ms / 1000.0f;
	}

	/*
	 * A PLAYBACK KEEPS THE REAL FRAME TIME and banks it as a budget.
	 * Taking the recorded delta HERE instead replays a fast-forwarded
	 * race AT fast-forward, because the recording holds the steps the
	 * sim took and those are enormous. The pacing belongs a level
	 * down, in the substep loop: this frame is worth however many
	 * recorded steps fit in it, and each one carries its own dt.
	 */
	if (race_replay_mode() == REPLAY_MODE_PLAY)
		s_replayBudgetMs += (int)(dt * 1000.0f + 0.5f);

	/*
	 * COUNT THE TICKS THAT ACTUALLY INTEGRATED, so a scripted run
	 * can report what it OBSERVED rather than what it asked for.
	 *
	 * A rendered frame is not a simulation step: while paused
	 * tm_now is frozen, dt comes out zero, and this function does
	 * nothing but return. Counting frames would count those.
	 *
	 * The equivalent on the original is the counter at 0x004e981c,
	 * which SEND_INPUT reads either side of a run. Trusting the
	 * REQUESTED count there hid a doubled frame rate for several
	 * rounds of measurement - every single-frame burst advanced
	 * two - so the port reports an observed count for the same
	 * reason. See frame-loop.txt 1.4.
	 */
	if (dt > 0.0f)
		s_race_ticks++;

	sdt = dt;
	/*
	 * SUBSTEPPED, AND THAT IS NOT A REFINEMENT.
	 *
	 * ONE physics step per RENDERED frame, taking the whole frame
	 * delta, is harmless at 1x: a 16 ms step moves the bike 53 units
	 * and everything it touches is far bigger than that.
	 *
	 * AT 10x FAST-FORWARD THAT STEP IS 166 ms AND THE BIKE TUNNELS.
	 * At 90 km/h it covers 533 world units in one step - MORE THAN
	 * A WHOLE 512-UNIT SEGMENT, and 1.4 times the road half width
	 * through the bridge. Wall clipping, ground contact and the
	 * segment walk are each handed a displacement larger than the
	 * geometry they test against, so the bike goes through walls
	 * that a smaller step would have caught.
	 *
	 * dt-SCALING IS NECESSARY AND NOT SUFFICIENT. Every routine here
	 * scales by its delta correctly and it still breaks, because one
	 * large Euler step is not several small ones once collision is
	 * involved. timing.h names this exact fix - "several
	 * normal-sized ticks per frame rather than one big one".
	 *
	 * WHAT IS INSIDE THE LOOP AND WHAT IS NOT:
	 *
	 *   in    the racer tick, progress, standings, the clock, the
	 *         decor animation and the bike-to-bike push - everything
	 *         that INTEGRATES or that tests geometry
	 *   out   the particles, the camera, the HUD and the draw prep -
	 *         all per-FRAME, and the camera is a first-order filter,
	 *         so running it N times would pull it in N times as fast
	 *
	 * THE STEP COUNT IS BOUNDED. A slow machine falls behind in real
	 * time rather than spiralling, which is the same reasoning
	 * FRAME_MAX_STEPS carries in timing.h.
	 */
	{
		int steps;
		int step;
		int stepMs = 0, baseMs = 0, extraMs = 0;
		int play = (race_replay_mode() == REPLAY_MODE_PLAY);

		if (play) {
			/*
			 * ONE RECORD IS ONE SUBSTEP, so the recording is
			 * paced in SIMULATED time and this frame simply
			 * spends as much of it as really elapsed. Each
			 * step's own dt and input come out of its record
			 * at the top of the loop.
			 */
			int used = 0;

			steps = race_replay_peek_steps(s_replayBudgetMs,
			                               RACE_MAX_SUBSTEPS, &used);
			s_replayBudgetMs -= used;
			if (steps == 0 && s_replayFrame >= s_replayCount)
				race_replay_stop();
		} else {
			steps = (int)(sdt / RACE_MAX_SUBSTEP) + 1;
			if (steps > RACE_MAX_SUBSTEPS)
				steps = RACE_MAX_SUBSTEPS;

			/*
			 * THE FRAME'S MILLISECONDS ARE SHARED OUT AS WHOLE
			 * NUMBERS, the remainder one each to the first few.
			 * A substep of dt/steps is not generally a whole
			 * millisecond, and a record that cannot express the
			 * step the sim took is a playback that drifts - the
			 * same mistake as the frame-level one above, one
			 * level down.
			 */
			stepMs  = (int)(sdt * 1000.0f + 0.5f);
			baseMs  = stepMs / steps;
			extraMs = stepMs - baseMs * steps;
		}

		for (step = 0; step < steps; step++) {
		if (play) {
			if (!race_replay_begin_frame(&sdt))
				break;
		} else {
			sdt = (float)(baseMs + (step < extraMs ? 1 : 0))
				  / 1000.0f;
		}
		/*
		 * THE ANIMATED DECOR, on a real step only - the helicopter,
		 * its rotor and the flashing lights. maybe_Game_Tick runs it
		 * at 0x0047d877, in the tick, so a rendered frame must not
		 * advance it: the periods are in real time and a paused game
		 * would spin the rotor.
		 *
		 * sdt * 100 IS THE UNIT, NOT sdt. sdt here is SECONDS; the
		 * scaled tick delta is sdt * 100 - 3.3 at 33 ms - which is
		 * what race_tick_all below is passed and what the loader's
		 * 0.1 scaling (0x004d1bd8) puts the periods in.
		 *
		 * PASSING sdt RAN THE ANIMATOR AT 1/100 SPEED. Measured, the
		 * rotor turned 0.0042 rad a frame where its record asks for
		 * 0.207 - a linear ramp of -2pi over a period of 100, which
		 * is about a revolution a second. It looked like nothing
		 * moving at all.
		 */
		if (sdt > 0.0f)
			trk_anim_decor_tick(&s_race_trk, sdt * 100.0f);

		/*
		 * A ZERO-dt FRAME IS NOT A SIMULATION STEP, and the guard is
		 * not a tidiness measure. WHILE PAUSED THE APP STILL RENDERS,
		 * so this runs every frame with sdt zero. Most of the physics
		 * scales by dt and no-ops, which is what makes the exception
		 * hard to find: racer_ai_lean_step takes NO dt at all and
		 * multiplies the lean by 1.2 a call, so a paused game walks an
		 * opponent's lean to the pi/4 clamp within a few hundred idle
		 * frames and holds it there. The original calls
		 * Race_TickAllRacers once per step, never per rendered frame.
		 *
		 * ONE TICK PATH. race_tick_all is the transcription of
		 * Race_TickAllRacers and it owns the loop, the dispatch and the
		 * shadow; calling race_tick_physics from here bypasses all
		 * three.
		 */
		if (sdt > 0.0f && s_race_racerCount > 1) {
			race_tick_all(s_race_racers, s_race_racerCount, sdt * 100.0f);

			/* The engine note follows the tick, so it reads this
			 * frame's gear and rpm rather than last frame's. */
			race_audio_tick();

			/*
			 * PROGRESS FIRST, THEN THE CLOCK. The checkpoint sweep
			 * adds to the timer, so it has to run before the tick
			 * decrements it - which is the original's order too:
			 * maybe_Game_Tick runs UpdateProgress and then
			 * maybe_Race_TickClock.
			 */
			race_update_progress();
			race_compute_standings();

			/*
			 * THE EXTENDED-TIME BANNER TIMER, counted down by the
			 * frame delta - maybe_Game_Tick at 0x0047d7b6, which
			 * guards it with > 0 rather than letting it go negative.
			 */
			if (g_extendedTimeMs > 0) {
				g_extendedTimeMs -= (int)(sdt * 1000.0f);
				if (g_extendedTimeMs < 0)
					g_extendedTimeMs = 0;
			}

			/*
			 * THE CLOCK, ONE STEP, HERE. maybe_Game_Tick runs
			 * maybe_Race_TickClock IMMEDIATELY BEFORE
			 * Race_TickAllRacers (racers.txt 9.8.3); ours runs it just
			 * after, because race_tick_all owns the dispatch and the
			 * order only matters for the frame the grid clears.
			 *
			 * The tick returns 1 on that frame and the caller supplies
			 * the limit, because the terms come from the track record
			 * and the difficulty table and the clock module owns
			 * neither.
			 */
			if (race_clock_tick(&s_race_clock, s_race_racers,
				                s_race_racerCount, s_race_localIndex,
				                (int)(sdt * 1000.0f), s_race_lapTimes)) {
				int limit = race_time_limit_ms();

				race_clock_start(&s_race_clock, s_race_racers,
					             s_race_racerCount, s_race_localIndex,
					             s_race_trk.segmentCount,
					             (limit >= 0), limit, 0,
					             g_reverseEnabled,
					             s_race_lapTimes);
				mrlog("race:   grid clear, limit %d ms%s\n",
					  s_race_clock.timerMs,
					  (limit < 0) ? " (limit not reloaded)" : "");
			}

			/*
			 * AND THEN THE BIKES PUSH EACH OTHER. Vtable slot +0x18
			 * of the game object, once a frame, between the tick and
			 * the draw - the positions it corrects are the ones the
			 * tick just wrote and the ones the draw is about to use.
			 *
			 * THE CALL SITE IS NOT FOUND. Tick and RenderFrame both
			 * go through the same vtable from a loop that has not
			 * been read; this order is the only one that works.
			 * [inferred]
			 */
			race_collide_racers(s_race_racers, s_race_racerCount,
				                sdt * 100.0f);
		}

		/*
		 * THE RECORDER, ONE RECORD PER SUBSTEP - not per frame.
		 *
		 * A substep is a FIXED slice of simulated time whatever
		 * the frame rate or the clock scale, so the recording is
		 * paced in game time rather than in frames. That is what
		 * lets a race recorded under fast-forward play back at
		 * normal speed: the steps are the same, only how many of
		 * them a frame is worth changes.
		 *
		 * It also keeps the step inside the record's one byte -
		 * 20 ms against the 640 a 40x frame was asking it to
		 * hold - and it is AFTER the tick because that is when
		 * the racer's input word holds what was polled.
		 */
		if (!play && sdt > 0.0f)
			race_replay_write(&s_race_racers[s_race_localIndex],
			                  (int)(sdt * 1000.0f + 0.5f));
		}
	}


	/*
	 * THE PARTICLES TICK AT GAME LEVEL, ONCE A FRAME, AND NOT PER
	 * RACER. maybe_Fx_TickAllParticles has exactly one caller -
	 * maybe_Game_Tick at 0x0047d887 - and it sits outside
	 * Race_TickAllRacers entirely. Only the SPAWN is inside the racer
	 * tick, through maybe_Racer_EmitWheelFx.
	 *
	 * Putting it inside the racer loop would age every particle once
	 * per racer, so a full field would evaporate the spray eight
	 * times as fast as the player's own frame rate suggests.
	 *
	 * dt IS THE SAME dt * 100 THE RACERS GET - about 3.3 at 33 ms,
	 * which is what makes the size growth 3.3 * 0.6 = 1.98 and the
	 * life fall 3.3 a tick. See fx.c.
	 */
	if (dt > 0.0f)
		fx_tick_all_particles(dt * 100.0f);

	/*
	 * AND THE POST-RACE SEQUENCE, once a FRAME rather than per
	 * substep - maybe_Game_Tick calls it at 0x0047d9f9, just
	 * after the particle tick at 0x0047d887.
	 */
	race_post_race_tick((int)(dt * 1000.0f));

	/*
	 * ONLY WHEN TIME ACTUALLY PASSED.
	 *
	 * The chase camera is a first-order filter chasing a point rigidly
	 * attached to the bike, so stepping it on a frame the bike did not
	 * move pulls it IN toward that point for free. race_update runs on
	 * paused frames too - it just takes a zero step - so an unguarded
	 * camera converges whenever the game sits paused, which shows as
	 * the view creeping in after a pause.
	 *
	 * AND IT CORRUPTS EVERY STEPPED MEASUREMENT, which is the worse
	 * half: driving in bursts pauses between them, so the filter
	 * closes up during each pause while the original's stays frozen.
	 * It settles 569 units from the bike against 1050 and tracks a
	 * turn almost rigidly, 0.98 of the bike's rotation where the
	 * original lags 0.48 rising to 0.83 - with the raw eye and the
	 * alpha IDENTICAL throughout, 488.21 and 0.2. That reads as a
	 * camera-model difference and is a pause artefact. camera.txt.
	 */
	if (dt > 0.0f)
		race_camera(1, eye, viewEye, target, &lens);
}

/*
 * The 2D backdrop. Drawn BEFORE the 3D bracket, because the horizon
 * writes every pixel it covers and the track then draws over it.
 */
void race_draw_2d(void)
{
	float eye[3], viewEye[3], target[3], lens;

	if (!race_ready())
		return;
	if (race_camera(0, eye, viewEye, target, &lens))
		horizon_draw(eye, viewEye, target, lens,
	                 s_race_trk.horizonYBias,
	                 s_race_trk.horizonDamp);
}

/* ================================================================== */
/* THE CHASE CAMERA.                                                  */
/* Transcribed from Camera_UpdateChase (0x0046f840); every constant   */
/* carries the address it was read from. See docs/subsystems/         */
/* camera.txt.                                                        */
/* ================================================================== */

/* ------------------------------------------------------------------ */
/*
 * The constants, all read out of the binary or the live process:
 *
 *   camera +0x1dc  64.0     the eye's local height, from 0x004d1354
 *   camera +0x58  -484.0    the boom base
 *   camera +0x54   191.0    added to eye.y after the boom
 *   camera +0x11c  (0, -160, 1680)   the target offset, from
 *                                    0x004d134c and 0x004d1350
 *   camera +0x224  {0, 128, 256, 512}   the boom table
 *
 *   0x004d1404     0.2      the eye lerp
 *   0x004d1408     0.5      the target lerp on x and z
 *   0x004d140c     0.333333 the target lerp on y
 */
#define CAM_LOCAL_UP      64.0f
#define CAM_BOOM_BASE   -484.0f
#define CAM_LIFT         191.0f

#define CAM_TARGET_X       0.0f
#define CAM_TARGET_Y    -160.0f
#define CAM_TARGET_Z    1680.0f

/* The look slot, camera.txt 1.2 slot 2 - ours only. */
#define CAM_SIDE_X       250.0f
#define CAM_SIDE_Y       150.0f

#define CAM_LERP_EYE       0.2f
#define CAM_LERP_TGT_XZ    0.5f
#define CAM_LERP_TGT_Y     0.333333f

/*
 * THE FINISH CAMERA'S FIVE, and every one is a closed form:
 *
 *   0x004d13e8  -3pi/4     the ramp's floor      (double)
 *   0x004d13f0  -pi/300    the ramp's step       (double)
 *   0x004d13f8  -4/(3pi)   the shared scale      (double)
 *   0x004d1400  -0.5       the boom's share
 *   0x004d1410   2/3       the eye drop's share
 *
 * THE FIRST THREE ARE ONE NUMBER. -3pi/4 * -4/(3pi) is EXACTLY
 * 1, so at the end of the ramp both terms below reduce to a
 * plain multiple - the boom by -0.5 and the eye by 2/3. That is
 * the check that says these were read correctly rather than
 * fitted.
 *
 * The ramp runs 0 -> -3pi/4 at -pi/300 per dt unit, which at the
 * 1.667 a 60fps frame delivers is about 135 frames - 2.2
 * seconds, the length of the pan in a retail recording.
 */
#define CAM_FINISH_RAMP_END   (-2.356194490192345f)
#define CAM_FINISH_RAMP_STEP  (-0.010471975511966f)
#define CAM_FINISH_SCALE      (-0.4244131815783876f)
#define CAM_FINISH_BOOM_K     (-0.5f)
#define CAM_FINISH_EYE_K      (0.6666667f)

/*
 * MODE 6, THE FIRST-PERSON CAMERA. camera.txt 3.2 has the model, and
 * says why the original's two code blocks fold into one path here.
 *
 *   0x004d1418   170.0      the eye height on the bike's up axis
 *   0x004d141c    40.0      how far ahead the aim point sits
 *   0x004d1424     5.0      wheelie term, target, PersoAnim 0
 *   0x004d1428    -5.0      wheelie term, eye,    PersoAnim 0
 *   0x004d1414    10.0      wheelie term, target, PersoAnim != 0
 *   0x004d1420   -10.0      wheelie term, eye,    PersoAnim != 0
 *   0x004d1430     10/13    the y lerp, PersoAnim 0      (double)
 *   0x004d1408     0.5      the y lerp, PersoAnim != 0
 *   0x004d13e0     0.25     the share of the lean the view rolls by
 *   0x004d13a0     0x4000 / 2pi, radians to trig-table units (double)
 */
#define CAM_FP_UP           170.0f
#define CAM_FP_AHEAD         40.0f
#define CAM_FP_WHEELIE_K      5.0f
#define CAM_FP_WHEELIE_K_PA  10.0f
#define CAM_FP_LERP_Y        0.76923076923076927f
#define CAM_FP_LERP_Y_PA     0.5f
#define CAM_FP_ROLL_SHARE    0.25f


static const float CAM_BOOM_TABLE[CAM_BOOM_SLOTS] = {
	0.0f, 128.0f, 256.0f, 512.0f
};

/* ------------------------------------------------------------------ */

/* ORIGIN  NONE - ours. The original has no equivalent because
 *         maybe_Game_InitRace seeds the camera record */
void camera_reset(MR_Camera *c)
{
	if (!c)
		return;
	memset(c, 0, sizeof(*c));
}

/*
 * ORIGIN  Camera_UpdateChase @ 0x0046f840, the composed matrix at
 *         0x0046fbfe
 *
 * v * M for M = Ry(a), row-vector convention:
 *
 *     [ ca, 0, -sa ]
 *     [  0, 1,   0 ]
 *     [ sa, 0,  ca ]
 *
 * THE MATRIX AT 0x0046fbfe IS EASY TO READ TRANSPOSED, and the
 * mistake is invisible in the maths: y passes through either way, the
 * boom length is unchanged by a rotation, and only the X SIGN moves.
 * A camera mirrored about the track axis looks entirely plausible.
 *
 * CAUGHT BY COMPARING THE EYE AGAINST MOTO.EXE, both after the same
 * 20-frame left: the original puts it at (+306, +217, -374) relative
 * to the bike and the port had (-386, +254, -285) - same length, X
 * negated. Both offsets fed in have v[0] = 0, so X is the only
 * component the sign can show up in. camera.txt 4.5.
 */
static void cam_rot_y(float out[3], const float v[3], float ca, float sa)
{
	out[0] = v[0] * ca + v[2] * sa;
	out[1] = v[1];
	out[2] = v[2] * ca - v[0] * sa;
}

/*
 * ORIGIN  Camera_UpdateChase @ 0x0046f840, the look-slot branch at
 *         0x0046ff9a
 *
 * RIGID, AND THAT IS THE POINT. The chase path lerps the eye at 0.2
 * a frame and then re-normalises the distance with the boom. The
 * lerp lags by roughly four frames of travel, and the lag points
 * backward along the direction of travel - so a sideways offset run
 * through it slides round behind the bike as speed rises until all
 * that is in frame is the rear wheel. The original's look slot has
 * neither lerp nor boom: eye and orientation are placed outright
 * from the bike, every frame.
 *
 * THE OFFSET IS THE ORIGINAL'S, camera.txt 1.2 slot 2 - 250 units
 * to the side, 150 up, level with the bike rather than ahead or
 * behind it.
 *
 * [open] camera_look_slot(c, pos, m, 2) and (.., 3) are the same two
 * placements by the original's own route, and could retire this.
 *
 * IT USES matOrientation, NOT matBikeRoot, AND THAT IS DELIBERATE.
 * The original's look slot rides matBikeRoot (racer+0x2e55), which
 * carries the lean and the wheelie pitch - so a bike doing either
 * looks level and the camera does the moving. This view exists to
 * WATCH the lean and the wheelie, so it hangs off the heading
 * alone and stays upright. [deliberate divergence]
 */
void camera_side(MR_Camera *c, const float racerPos[3],
                 const float racerMatrix[12], int right)
{
	float local[3];
	float rot[3];
	float yaw, a, ca, sa;
	int   i;

	if (!c || !racerPos || !racerMatrix)
		return;

	yaw = (float)atan2(racerMatrix[2], racerMatrix[8]);
	a   = -yaw;
	ca  = (float)cos(a);
	sa  = (float)sin(a);

	local[0] = right ? CAM_SIDE_X : -CAM_SIDE_X;
	local[1] = CAM_SIDE_Y;
	local[2] = 0.0f;
	cam_rot_y(rot, local, ca, sa);

	/*
	 * THE TARGET IS LEVEL WITH THE EYE, which reproduces the
	 * slot's zero pitch: pLookAngles[2] is (0, pi/2, 0), so the
	 * original looks horizontally and the bike sits a little low
	 * in frame. Aiming at racerPos instead would tilt down.
	 */
	for (i = 0; i < 3; i++) {
		c->eye[i]     = racerPos[i] + rot[i];
		c->viewEye[i] = c->eye[i];
		c->prevEye[i] = c->eye[i];
		c->target[i]  = racerPos[i];
	}
	c->target[1] += CAM_SIDE_Y;

	/*
	 * PRIMED, so switching back to the chase lerps out of here
	 * rather than snapping from wherever the chase last was.
	 */
	c->primed = 1;
}


/*
 * ORIGIN  Camera_Update @ 0x004700e0, at 0x00470190 - the mode 6 arm
 *
 * A SLICE, AND WHY: mode 6 is inline in the MODE DISPATCHER, whose
 * other two arms the port already factors out. Importing
 * Camera_Update whole would import the dispatch twice.
 *
 * THE CALLER OWNS THE GATES, not this function - camera.txt 3.2.1. A
 * crashed rider and a held look-back both fall through to the chase.
 *
 * THE EYE IS THE VIEW EYE. No boom, so nothing to disagree about.
 */
void camera_first_person(MR_Camera *c, const MR_Racer *r)
{
	const MR_BikeCarac *bc;
	const float *M;
	float prevEye[3], prevTgt[3];
	float k, lerpY, dEye, dTgt;
	int   i, persoAnim;

	if (!c || !r)
		return;

	M         = r->matOrientation;
	bc        = bike_at(r->bikeIndex);
	persoAnim = (bc && bc->persoAnim != 0);
	k         = persoAnim ? CAM_FP_WHEELIE_K_PA : CAM_FP_WHEELIE_K;
	lerpY     = persoAnim ? CAM_FP_LERP_Y_PA    : CAM_FP_LERP_Y;

	for (i = 0; i < 3; i++) {
		prevEye[i] = c->eye[i];
		prevTgt[i] = c->target[i];
	}

	/* Vec3_Copy(pPrevEye, pEye) at 0x004701d0. Mode 6 never reads it;
	 * mode 7 does, the moment the player switches back. */
	for (i = 0; i < 3; i++)
		c->prevEye[i] = prevEye[i];

	dEye = CAM_FP_UP - k * r->wheelieAngle;
	dTgt = CAM_FP_UP + k * r->wheelieAngle;

	/* Rows then translation: row 1 up, row 2 forward. The translation
	 * goes in whether or not it happens to be zero. */
	for (i = 0; i < 3; i++) {
		c->eye[i]    = r->position[i] + M[9 + i] + dEye * M[3 + i];
		c->target[i] = r->position[i] + M[9 + i] + dTgt * M[3 + i]
		               + CAM_FP_AHEAD * M[6 + i];
	}

	/* ONLY Y IS SMOOTHED. The original lerps all three and two of the
	 * factors are 1.0, so it emits (v - prev) + prev for x and z. */
	c->eye[1]    = (c->eye[1]    - prevEye[1]) * lerpY + prevEye[1];
	c->target[1] = (c->target[1] - prevTgt[1]) * lerpY + prevTgt[1];

	for (i = 0; i < 3; i++)
		c->viewEye[i] = c->eye[i];

	/* 0x00470515..0x0047052d. A QUARTER of the lean, and it lands on
	 * the VIEW MATRIX rather than the eye - camera_apply_roll. */
	c->roll = (int)(r->lean * CAM_FP_ROLL_SHARE
	                * (float)TRIG_IDX_PER_RAD);

	/* Switching back to the chase eases out rather than snapping. */
	c->primed = 1;
}


/*
 * ORIGIN  Camera_Update @ 0x004700e0, at 0x00470554
 *
 * view = view * Rz(a), all FOUR rows, the translation included.
 *
 * THE ORIGINAL BUILDS A FULL THREE-ANGLE EULER and two of the angles
 * are literally zero - visible in the folding, which reads
 * g_cosTable[0] and g_cosTable[0x3000] as bare absolute addresses.
 * DO NOT GENERALISE FROM WHAT SURVIVED: if pitch or yaw is ever
 * wanted here, go back and read 0x00470554.
 */
void camera_apply_roll(float view[12], int roll)
{
	float c, s, x, y;
	int   i;

	if (!view || (roll & TRIG_MASK) == 0)
		return;

	c = trig_cos(roll);
	s = trig_sin(roll);

	for (i = 0; i < 12; i += 3) {
		x = view[i];
		y = view[i + 1];
		view[i]     = x * c - y * s;
		view[i + 1] = x * s + y * c;
		/* view[i + 2] is untouched - it is the roll axis. */
	}
}


/* ORIGIN  Camera_UpdateChase @ 0x0046f840 */
void camera_update(MR_Camera *c, const float racerPos[3],
                   const float racerMatrix[12], float dt)
{
	float local[3];
	float rot[3];
	float raw[3];
	float tgt[3];
	float d[3];
	float yaw, a, ca, sa, boom, len;
	int   i, fresh;

	if (!c || !racerPos || !racerMatrix)
		return;

	if (c->boomSlot < 0 || c->boomSlot >= CAM_BOOM_SLOTS)
		c->boomSlot = 0;
	boom = CAM_BOOM_TABLE[c->boomSlot] + CAM_BOOM_BASE;

	/*
	 * TERM 1 OF THE FINISH CAMERA - THE BOOM RUNS OUT, 0x0046f8f4.
	 *
	 * The ramp is advanced here and nowhere else, and the term is
	 * taken off the BASE rather than off the table-adjusted boom -
	 * the original reads flOrbitRadius twice for exactly that.
	 * At the end of the ramp it is -484 * -0.5 = 242, so the boom
	 * goes -484 -> -726 and the camera ends up half again as far
	 * back as it sits during the race.
	 */
	if (c->selectFlag) {
		if (c->lookYaw > CAM_FINISH_RAMP_END)
			c->lookYaw += dt * CAM_FINISH_RAMP_STEP;
		boom -= CAM_BOOM_BASE * CAM_FINISH_BOOM_K *
		        c->lookYaw * CAM_FINISH_SCALE;
	}

	/*
	 * Racer_GetYaw (0x00490220), then the FCHS at 0x0046f996. The
	 * function looks far more involved than this in the decompiler,
	 * but the racer matrix's translation is ZERO, which collapses it
	 * to the textbook yaw extraction - see its plate comment.
	 */
	yaw = (float)atan2(racerMatrix[2], racerMatrix[8]);
	a   = -yaw;

	/*
	 * TERM 4 - THE ORBIT, and this one is [inferred], not read.
	 *
	 * The ramp swings the eye ROUND the bike as well as pulling
	 * the boom out and dropping the eye. In the original it falls
	 * out of the composed matrix Camera_UpdateChase builds from
	 * three angles through __ftol - the block its plate comment
	 * marks unread, because the decompiler lost the operands.
	 *
	 * THE EVIDENCE IS A MEASUREMENT, not a guess at the code.
	 * With both games paused on the same 4th-place finish every
	 * magnitude already agreed - boom 726.2 against 726.8, eye y
	 * 127.3 against 127.0, the aim point identical - and only the
	 * bearing of the eye from the bike differed:
	 *
	 *     MOTO   46.8 degrees      ours   182.3 degrees
	 *
	 * 135.5 degrees apart, and lookYaw had ramped to -2.3646 rad
	 * = -135.5 degrees. Adding it here reproduces MOTO's bearing
	 * to a tenth of a degree, and at lookYaw 0 it is identity, so
	 * the racing camera is untouched.
	 *
	 * IT IS STILL AN INFERENCE. The real block may apply the same
	 * rotation somewhere else, or fold in a pitch this one does
	 * not have. Settling it needs a trace hook on the __ftol
	 * operands, the way Dd_DrawHorizon's were recovered.
	 */
	if (c->selectFlag)
		a += c->lookYaw;
	ca  = (float)cos(a);
	sa  = (float)sin(a);

	/* ---- the raw eye ------------------------------------------ */
	local[0] = 0.0f;
	local[1] = CAM_LOCAL_UP;
	local[2] = boom;
	cam_rot_y(rot, local, ca, sa);
	for (i = 0; i < 3; i++)
		raw[i] = racerPos[i] + rot[i];

	/*
	 * FIRST FRAME SNAPS. The original restores the eye from +0xc8
	 * and lerps against it; with no seeded record of our own,
	 * lerping from the origin would walk the camera in from (0,0,0)
	 * over a dozen frames.
	 */
	fresh = !c->primed;
	if (fresh) {
		c->primed = 1;
		for (i = 0; i < 3; i++)
			c->prevEye[i] = raw[i];
	}

	for (i = 0; i < 3; i++)
		c->eye[i] = c->prevEye[i] +
	                (raw[i] - c->prevEye[i]) * CAM_LERP_EYE;

	/* ---- the target ------------------------------------------- */
	if (c->selectFlag) {
		/*
		 * TERM 2 - IT LOOKS AT THE BIKE, 0x0046fe4a. The racing
		 * arm aims 1680 units AHEAD of the rider, which is what
		 * keeps the road in shot; the finished arm drops that
		 * and aims at the machine itself, lifted by the same 64
		 * the eye uses. Together with the boom running out,
		 * that is the swing round to the side as it coasts.
		 *
		 * The original writes matrix ROW 1 scaled by 64 plus the
		 * matrix translation. Our frame is Ry alone, so row 1 is
		 * (0,1,0) and the translation is zero - see the note on
		 * cam_rot_y. If the unread orientation block ever lands
		 * (camera.txt, Camera_UpdateChase OPEN), this is one of
		 * the two places it changes.
		 */
		tgt[0] = racerPos[0];
		tgt[1] = racerPos[1] + CAM_LOCAL_UP;
		tgt[2] = racerPos[2];
	} else {
		local[0] = CAM_TARGET_X;
		local[1] = CAM_TARGET_Y;
		local[2] = CAM_TARGET_Z;
		cam_rot_y(rot, local, ca, sa);
		for (i = 0; i < 3; i++)
			tgt[i] = racerPos[i] + rot[i];
	}

	/*
	 * THE TARGET SNAPS ON THE FIRST FRAME TOO. Priming it to racerPos
	 * instead looks reasonable and is not a value the camera ever
	 * holds, so it makes the opening frames wrong in a less obvious
	 * way than an unprimed eye would. The original never faces this
	 * because maybe_Game_InitRace seeds the record.
	 */
	if (fresh) {
		for (i = 0; i < 3; i++)
			c->target[i] = tgt[i];
	} else {
		/* Three rates, and y is the odd one out - 0x004d140c. */
		c->target[0] += (tgt[0] - c->target[0]) * CAM_LERP_TGT_XZ;
		c->target[1] += (tgt[1] - c->target[1]) * CAM_LERP_TGT_Y;
		c->target[2] += (tgt[2] - c->target[2]) * CAM_LERP_TGT_XZ;
	}

	/*
	 * THE LOOK-AT IS BUILT HERE, from the PRE-boom eye (0x0046fece
	 * runs before 0x0046fed3). The caller needs that point, not the
	 * final one, or it will not reproduce the original's view.
	 */
	for (i = 0; i < 3; i++)
		c->viewEye[i] = c->eye[i];

	/*
	 * prevEye is the post-lerp, PRE-boom eye [verified, 0x0046ff76
	 * sits between the normalise and the boom].
	 */
	for (i = 0; i < 3; i++)
		c->prevEye[i] = c->eye[i];

	/* ---- the boom --------------------------------------------- */
	for (i = 0; i < 3; i++)
		d[i] = c->eye[i] - racerPos[i];
	len = (float)sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
	if (len != 0.0f) {
		for (i = 0; i < 3; i++)
			d[i] /= len;
		for (i = 0; i < 3; i++)
			c->eye[i] = racerPos[i] - d[i] * boom;
	}

	/* ---- and the lift, on both branches ----------------------- */
	c->eye[1] += CAM_LIFT;

	/*
	 * TERM 3 - AND THEN SOME OF IT COMES BACK OFF, 0x0046ffd8.
	 * The lift is added unconditionally and the finished arm
	 * scrubs two thirds of it off as the ramp runs, so the eye
	 * settles from +191 to about +64 - down to the bike's own
	 * height, which is what turns the chase shot into a level
	 * one.
	 */
	if (c->selectFlag) {
		c->eye[1] -= c->lookYaw * CAM_FINISH_SCALE *
		             CAM_LIFT * CAM_FINISH_EYE_K;
	}
}

/* ================================================================== */
/* THE TRACKSIDE SHOT DIRECTOR.                                       */
/*                                                                    */
/* ORIGIN  Camera_UpdateTrackside @ 0x0046e9f0                        */
/*                                                                    */
/* What runs the camera once the race is won and the bike is riding   */
/* itself, and the same director a replay and the attract demo use.   */
/* It cuts between six shots on a frame timer.                        */
/*                                                                    */
/* IT IS A TIMER **AND** A POSITION TEST, and either half alone gives */
/* the wrong model. The TIMER decides WHEN to cut; the TRACK decides  */
/* WHAT to cut to - a fixed camera if the track placed one within 23  */
/* segments, an orbit if it did not. See camera.txt 5.2-5.3, which is */
/* the home for the shot table and the search; it is not repeated.    */
/* ================================================================== */


/*
 * Durations in dt UNITS, from the reload switch at 0x0046ee5a - the
 * same unit dtScaled carries, so ONE IS TEN MILLISECONDS (K_DT_SCALE).
 * The 250 of a fixed shot is 2.5 seconds, or 150 frames at 60fps.
 *
 * NOT frames: the decrement at the tail of camera_update_trackside
 * subtracts dt, not one, which is what makes a shot last the same
 * wall-clock time whatever the frame rate.
 */
#define CAM_SHOT_DT_FIXED      250
#define CAM_SHOT_DT_CHASE      600
#define CAM_SHOT_DT_ORBIT      300
#define CAM_SHOT_DT_ORBIT_PI   400
#define CAM_SHOT_DT_FIXEDZOOM  260
#define CAM_SHOT_DT_LOOKSLOT   400

/*
 *   0x004d13d0  -pi/200 (double)  the orbit's rate
 *   0x004d13d8   260.0            shot 2 rides this far off the road
 *   0x004d13dc   560.0            shot 3 rides higher
 *   0x004d13e0   0.25             and EASES up to it instead of
 *                                 clamping, which is the one real
 *                                 difference between the two orbits
 *   0x004d1348   1.0              zoom floor
 *   0x004d13e4   3.0              zoom ceiling
 *   0x004d1398   256.0            shot 4's framing scale
 */
#define CAM_ORBIT_RATE      (-0.015707963267948966)
#define CAM_ORBIT_CLEAR       260.0f
#define CAM_ORBIT_PI_CLEAR    560.0f
#define CAM_ORBIT_PI_LERP       0.25f
#define CAM_SHOT_ZOOM_MIN       1.0f
#define CAM_SHOT_ZOOM_MAX       3.0f
#define CAM_SHOT_ZOOM_SCALE   256.0f

/* 0x004d13c4 and 0x004d13c8 - the shot pick's scale, 0..11. */
#define CAM_PICK_SCALE  1.5259021893143654e-05f
#define CAM_PICK_SPAN   12.0f

/*
 * THE EIGHT BIKE-LOCAL PLACEMENTS, camera.txt 1.2, read live off the
 * record's pLookOffset. Shot 5 picks one of slots 2..7 - never 0 or
 * 1, which are the forward and look-back views the PLAYER uses.
 *
 * THE ANGLES ARE CARRIED beside the offsets because camera_look_slot
 * needs them - the original builds a view matrix from pLookAngles and
 * does not aim at anything.
 *
 * THE DIRECTOR STILL IGNORES THEM, and that stays a deliberate
 * divergence: for every slot 2..7 the table points back at the bike
 * anyway - the one 250 left is yawed +pi/2, the one 250 right -pi/2,
 * and both high ones are pitched down at what they are behind - so
 * aiming at the machine is the same shot by a shorter route. It is
 * camera_look_slot, on the player's slot 1, that needs the real
 * rotation. [deliberate divergence, for slots 2..7 only]
 */
static const float CAM_LOOK_OFFSET[CAM_LOOK_SLOTS][3] = {
	{    0.0f, -160.0f, 1680.0f },   /* 0 far ahead - player's     */
	{    0.0f,  160.0f,  -20.0f },   /* 1 look back  - player's    */
	{ -250.0f,  150.0f,    0.0f },   /* 2 left side                */
	{  250.0f,  150.0f,    0.0f },   /* 3 right side               */
	{  -70.0f,  120.0f,  200.0f },   /* 4 rear left                */
	{   70.0f,  120.0f,  200.0f },   /* 5 rear right               */
	{    0.0f,  400.0f,  400.0f },   /* 6 high behind              */
	{    0.0f,  500.0f,  500.0f }    /* 7 higher                   */
};

/*
 * pLookAngles, camera+0x17c, the same eight in the same order. Euler
 * radians, built by Camera_InitRecord's Vec3_Copy3/FUN_0046d4f0 pairs.
 *
 * THE SCALAR AT +0x1dc IS 64.0 FOR ALL EIGHT AND IS NOT CARRIED. It is
 * loaded at 0x0046fc04 into the same FPU block as the offset and does
 * NOT reach the eye - a measured eye matches the offset alone, at two
 * different bike headings. Do not invent a use for it; read
 * 0x0046fc04 onward. [open]
 */
static const float CAM_LOOK_ANGLES[CAM_LOOK_SLOTS][3] = {
	{       0.0f,  0.0f,       0.0f },   /* 0 straight ahead        */
	{       0.0f,  3.1415927f, 0.0f },   /* 1 yawed 180 - look back */
	{       0.0f,  1.5707964f, 0.0f },   /* 2 yawed +90             */
	{       0.0f, -1.5707964f, 0.0f },   /* 3 yawed -90             */
	{       0.0f,  3.1415927f, 0.0f },   /* 4                       */
	{       0.0f,  3.1415927f, 0.0f },   /* 5                       */
	{ -0.7853982f, 3.1415927f, 0.0f },   /* 6 pitched -45           */
	{ -0.7853982f, 3.1415927f, 0.0f }    /* 7 pitched -45           */
};

/*
 * ORIGIN  NONE - ours.
 *
 * v * M for the 3x4 M the racer carries, column-major basis vectors
 * with the translation last - the same convention transform.h uses.
 */
static void cam_xform34(float out[3], const float m[12], const float v[3])
{
	int i;

	for (i = 0; i < 3; i++)
		out[i] = m[i] * v[0] + m[3 + i] * v[1] + m[6 + i] * v[2] + m[9 + i];
}

/*
 * ORIGIN  Camera_UpdateChase @ 0x0046f840, at 0x0046fbfe
 *
 * THE LOOK BRANCH. nLookBackKey is an INDEX and a non-zero one takes a
 * whole different path from the chase - no boom, no settle, no lerp,
 * and the eye placed outright in bike space.
 *
 * MEASURED TWICE AGAINST MOTO. Slot 1 forced into the live record
 * [verified]:
 *
 *   at the grid, bike yaw 0
 *     racerPos (-128, 0, 2058)      eye (-128, 160, 2038)
 *     view     Ry(pi) exactly
 *   mid-corner, bike yaw ~111 deg
 *     racerPos (-1224.68, 0, 2924.14)
 *     predicted eye (-1206.03, 160, 2931.36)
 *     measured  eye (-1206.35, 160, 2931.15)
 *
 * so eye = racerPos + matOrientation * offset, and nothing else.
 *
 * THE VIEW IS THE TRANSPOSE of the camera's world orientation, which
 * is what a view matrix is. Measured mid-corner the record held the
 * transpose of euler(angles) * matOrientation to the digit; taking the
 * product without transposing mirrors the scene.
 *
 * AND IT DOES NOT WRITE target. The original leaves camera+0xa4 at
 * whatever the previous frame put there - confirmed by reading it
 * unchanged at the forward value while slot 1 was live - and that
 * leftover is what makes the release pan in cockpit view. camera.txt 6.
 *
 * ONE LIMIT: both samples are pure yaw, and slots 6 and 7 carry a
 * PITCH. Ry rotations commute, so neither sample can settle whether
 * the original composes euler * matrix or matrix * euler. Slot 1 is
 * unaffected; a pitched slot needs a third measurement. [open]
 */
void camera_look_slot(MR_Camera *c, const float racerPos[3],
                      const float racerMatrix[12], int slot)
{
	float rot[12];
	float orient[12];
	int   i, j;

	if (!c || !racerPos || !racerMatrix)
		return;
	if (slot <= 0 || slot >= CAM_LOOK_SLOTS)
		return;

	cam_xform34(c->eye, racerMatrix, CAM_LOOK_OFFSET[slot]);
	for (i = 0; i < 3; i++) {
		c->eye[i]    += racerPos[i];
		c->viewEye[i] = c->eye[i];
	}

	mat34_from_euler(rot, CAM_LOOK_ANGLES[slot]);   /* nine only */
	rot[9] = rot[10] = rot[11] = 0.0f;
	mat34_multiply(orient, rot, racerMatrix);

	/* The view is the inverse, and for a rotation that is the
	 * transpose. The translation stays zero: the renderer positions
	 * from the eye, not from the matrix. */
	for (i = 0; i < 3; i++)
		for (j = 0; j < 3; j++)
			c->view[i * 3 + j] = orient[j * 3 + i];
	c->view[9] = c->view[10] = c->view[11] = 0.0f;
	c->viewValid = 1;

	/* target IS NOT TOUCHED - see above. */

	c->primed = 1;
}


/*
 * ORIGIN  Camera_UpdateTrackside @ 0x0046e9f0, the selection half
 *
 * Runs ONLY when the timer has expired. Which shot follows depends
 * on whether the track has a camera near the bike:
 *
 *     from a FIXED shot   a camera, and 7 times in 8   stay fixed
 *                         otherwise                    pick an orbit
 *     from an ORBIT       a camera                     cut to fixed
 *                         otherwise                    pick an orbit
 *
 * so the director leans on placed cameras and falls back to orbiting
 * itself when the track gives it nothing - which is why a tunnel
 * always orbits. camera.txt 5.3.
 */
static void cam_pick_orbit(MR_Camera *c)
{
	/*
	 * ORIGIN  Camera_UpdateTrackside @ 0x0046e9f0, at 0x0046eae5
	 *
	 * THE SCALE IS TWELFTHS, NOT THIRDS, and that is the whole
	 * character of the fallback. The decompiler lost the two
	 * multiplies; the disassembly has them as 1/65535 and 12.0, so
	 * the pick runs 0..11 and only THREE of the twelve outcomes are
	 * an orbit:
	 *
	 *     0   orbit, running        1 in 12
	 *     1   orbit, pinned         1 in 12
	 *     2   a bike-local slot     1 in 12
	 *     3+  the ordinary chase    9 in 12
	 *
	 * SO A STRETCH WITH NO TRACKSIDE CAMERA MOSTLY JUST CHASES.
	 * Read as thirds - which is what the decompiler's shape
	 * suggests - the chase becomes unreachable and the camera
	 * orbits every cut, which is not what retail does.
	 */
	/*
	 * STREAM 0, NOT THE CRT rand(). Camera_UpdateTrackside calls
	 * Rand_Next (0x004b1160) directly with a literal 0 pushed -
	 * 0x0046eabe - so the director has a stream of its own and
	 * cannot move the simulation's. rand.h.
	 */
	int pick = (int)((float)(rand_next(0) & 0xffff) *
	                 CAM_PICK_SCALE * CAM_PICK_SPAN);

	if (pick == 0) {
		c->shot = CAM_SHOT_ORBIT;
		/* Which way round it swings is a coin toss, 0x0046edc8. */
		c->orbitYaw = (rand_next(0) & 1) ? 3.1415927f : -3.1415927f;
	} else if (pick == 1) {
		c->shot = CAM_SHOT_ORBIT_PI;
	} else if (pick == 2) {
		c->shot = CAM_SHOT_LOOKSLOT;
	} else {
		c->shot = CAM_SHOT_CHASE;
	}
}

static void cam_reload_timer(MR_Camera *c)
{
	switch (c->shot) {
	case CAM_SHOT_FIXED:      c->shotTimer = CAM_SHOT_DT_FIXED;     break;
	case CAM_SHOT_CHASE:      c->shotTimer = CAM_SHOT_DT_CHASE;     break;
	case CAM_SHOT_ORBIT:      c->shotTimer = CAM_SHOT_DT_ORBIT;     break;
	case CAM_SHOT_ORBIT_PI:   c->shotTimer = CAM_SHOT_DT_ORBIT_PI;  break;
	case CAM_SHOT_FIXED_ZOOM: c->shotTimer = CAM_SHOT_DT_FIXEDZOOM; break;
	case CAM_SHOT_LOOKSLOT:
		c->shotTimer = CAM_SHOT_DT_LOOKSLOT;
		/*
		 * SLOTS 2..7 ONLY, and the original spins on rand until it
		 * gets one - a do/while that rejects 0 and 1 rather than
		 * biasing a modulo. The two it rejects are the player's own
		 * forward and look-back views.
		 */
		do {
			c->lookSlot = (int)(rand_next(0) & 7);
		} while (c->lookSlot < 2);
		/* The two side views are cut 100 dt units short, 0x0046eeb8. */
		if (c->lookSlot == 2 || c->lookSlot == 3)
			c->shotTimer -= 100;
		break;
	default: break;
	}
}

void camera_update_trackside(MR_Camera *c, float dt, const MR_Racer *r,
                             const MR_Trk *t)
{
	float camPos[3];
	int   haveCam;

	if (!c || !r || !t)
		return;

	/* ---- the selection, only on expiry ------------------------ */
	if (c->shotTimer < 0) {
		trk_find_nearest_camera(t, r->segmentPrev, r->position, camPos);
		/* (0,0,0) is the "none" sentinel - see trk.h. */
		haveCam = (camPos[0] != 0.0f || camPos[1] != 0.0f ||
		           camPos[2] != 0.0f);

		if (c->shot == CAM_SHOT_FIXED || c->shot == CAM_SHOT_FIXED_ZOOM) {
			/* Already on a placed camera: hold it unless the dice
			 * say otherwise, 1 in 8 - 0x0046ed7e. */
			if (!haveCam || (rand_next(0) & 7) == 0) {
				cam_pick_orbit(c);
			} else {
				c->eye[0] = camPos[0];
				c->eye[1] = camPos[1];
				c->eye[2] = camPos[2];
			}
		} else if (c->shot == CAM_SHOT_ORBIT) {
			if (!haveCam) {
				c->shot = CAM_SHOT_FIXED;
			} else {
				rand_next(0);    /* consumed, 0x0046ee0e */
				c->shot = CAM_SHOT_FIXED_ZOOM;
				c->eye[0] = camPos[0];
				c->eye[1] = camPos[1];
				c->eye[2] = camPos[2];
			}
		} else {
			/* chase, pinned orbit and look-slot all land here */
			if (!haveCam) {
				cam_pick_orbit(c);
			} else {
				c->shot = (rand_next(0) & 1) ? CAM_SHOT_FIXED
				                            : CAM_SHOT_FIXED_ZOOM;
				c->eye[0] = camPos[0];
				c->eye[1] = camPos[1];
				c->eye[2] = camPos[2];
			}
		}
		cam_reload_timer(c);
	}

	/* ---- the shot, every frame -------------------------------- */
	switch (c->shot) {
	case CAM_SHOT_CHASE:
		/*
		 * BACK TO THE ORDINARY CHASE. The director does not place
		 * the eye at all here - it hands the frame to mode 7, which
		 * is camera_update. lookYaw is cleared so the finish
		 * camera's ramp does not leak into it.
		 */
		c->orbitYaw   = 0.0f;
		c->lookYaw    = 0.0f;
		c->lookSlot   = 0;
		c->directorEye = 0;
		/*
		 * AND THE ZOOM GOES BACK TO 1. The original's case 1
		 * does this and it is not decoration: shot 4 leaves the
		 * projection at up to 3x to frame the bike, and without
		 * this the CHASE inherits it - the camera in its usual
		 * place, magnified, right behind the helmet.
		 */
		screen3d_set_proj_zoom(1.0f);
		break;

	case CAM_SHOT_ORBIT:
	case CAM_SHOT_ORBIT_PI: {
		float off[3];
		float ground;
		float s, cth;

		if (c->shot == CAM_SHOT_ORBIT_PI) {
			/*
			 * PINNED AT HALF A TURN. The original spells it out of
			 * the table at indices 0x1000 and 0x2000 - cos 90 and
			 * cos 180 - which multiply out to (0, 0, -radius), so
			 * the camera sits straight along the bike's own z.
			 */
			c->orbitYaw = 3.1415927f;
			off[0] = 0.0f;
			off[1] = 0.0f;
			off[2] = -CAM_BOOM_BASE;
		} else {
			int idx = (int)((double)c->orbitYaw * TRIG_IDX_PER_RAD);

			s   = trig_sin(idx);
			cth = trig_cos(idx);
			off[0] = s   * CAM_BOOM_BASE;
			off[1] = 0.0f;
			off[2] = cth * CAM_BOOM_BASE;
		}

		cam_xform34(c->eye, r->matOrientation, off);
		c->eye[0] += r->position[0];
		c->eye[1] += r->position[1];
		c->eye[2] += r->position[2];

		if (c->shot == CAM_SHOT_ORBIT)
			c->orbitYaw += dt * (float)CAM_ORBIT_RATE;

		/*
		 * AND IT WILL NOT GO THROUGH THE ROAD. Both orbits query
		 * the ground under the eye and lift it clear - but shot 2
		 * CLAMPS and shot 3 EASES a quarter of the way each frame,
		 * which is the only real difference between them.
		 */
		ground = trk_ground_height_at(t, r->segmentPrev, c->eye);
		if (c->shot == CAM_SHOT_ORBIT) {
			ground += CAM_ORBIT_CLEAR;
			if (c->eye[1] < ground)
				c->eye[1] = ground;
		} else {
			ground += CAM_ORBIT_PI_CLEAR;
			if (c->eye[1] < ground)
				c->eye[1] += (ground - c->eye[1]) * CAM_ORBIT_PI_LERP;
		}

		screen3d_set_proj_zoom(1.0f);
		c->directorEye = 1;
		break;
	}

	case CAM_SHOT_FIXED_ZOOM: {
		/*
		 * THE SHOT THAT FRAMES ITSELF. The eye was placed on a
		 * track camera when the cut happened and does not move;
		 * what changes is the PROJECTION, set so the bike keeps a
		 * constant size on screen as it rides past:
		 *
		 *     zoom = (clipHeight / 2) / ((projBaseX * 256) / dist)
		 *
		 * clamped to 1..3. Past the far clamp the bike would be a
		 * speck, past the near one it would fill the frame.
		 */
		float dx = c->eye[0] - r->position[0];
		float dy = c->eye[1] - r->position[1];
		float dz = c->eye[2] - r->position[2];
		float dist = (float)sqrt(dx * dx + dy * dy + dz * dz);
		float zoom;

		if (dist > 0.0f && g_projBaseX != 0.0f) {
			zoom = (float)(g_clipHeight / 2) /
				   ((g_projBaseX * CAM_SHOT_ZOOM_SCALE) / dist);
			if (zoom < CAM_SHOT_ZOOM_MIN)
				zoom = CAM_SHOT_ZOOM_MIN;
			if (zoom > CAM_SHOT_ZOOM_MAX)
				zoom = CAM_SHOT_ZOOM_MAX;
			screen3d_set_proj_zoom(zoom);
		}
		c->directorEye = 1;
		break;
	}

	case CAM_SHOT_LOOKSLOT: {
		/*
		 * A BIKE-LOCAL PLACEMENT, off the LEANED root matrix rather
		 * than the flat orientation - so the shot rolls with the
		 * machine. See CAM_LOOK_OFFSET for what is not carried.
		 */
		int slot = c->lookSlot;

		if (slot < 0 || slot >= CAM_LOOK_SLOTS)
			slot = 2;
		c->orbitYaw = 0.0f;
		cam_xform34(c->eye, r->matBikeRoot, CAM_LOOK_OFFSET[slot]);
		c->eye[0] += r->position[0];
		c->eye[1] += r->position[1];
		c->eye[2] += r->position[2];
		screen3d_set_proj_zoom(1.0f);
		c->directorEye = 1;
		break;
	}

	case CAM_SHOT_FIXED:
	default:
		/* The eye is wherever the cut left it - on the camera. */
		c->lookSlot = 0;
		screen3d_set_proj_zoom(1.0f);
		c->directorEye = 1;
		break;
	}

	/* Every shot but the chase aims at the machine. */
	if (c->directorEye) {
		c->target[0] = r->position[0];
		c->target[1] = r->position[1];
		c->target[2] = r->position[2];
		c->viewEye[0] = c->eye[0];
		c->viewEye[1] = c->eye[1];
		c->viewEye[2] = c->eye[2];
	}

	/*
	 * ORIGIN  Camera_UpdateTrackside @ 0x0046e9f0, at 0x0046f7c8
	 *
	 * IT COUNTS DOWN BY dt, NOT BY ONE, so the durations are in dt
	 * UNITS and a shot lasts the same wall-clock time whatever the
	 * frame rate. At the 1.667 a 60fps frame delivers, the 250 of a
	 * fixed shot is 150 frames - two and a half seconds.
	 *
	 * DECREMENTING BY ONE MAKES EVERY SHOT 1.67x TOO LONG at
	 * 60fps, and frame-rate dependent into the bargain: the
	 * camera sits on a fixed trackside shot long after the bike
	 * has gone behind something, which is exactly how it reads.
	 */
	c->shotTimer = (int)((float)c->shotTimer - dt);
}

/* ================================================================== */
/* THE POST-RACE SEQUENCE.                                            */
/*                                                                    */
/* ORIGIN  FUN_00412a50 @ 0x00412a50, called every frame from         */
/*         maybe_Game_Tick at 0x0047d9f9                              */
/*                                                                    */
/* Crossing the line does not end the race - it starts a timer. Seven */
/* seconds normally, FOURTEEN when the bike is to keep riding itself, */
/* and the extra seven are the credits' window.                       */
/*                                                                    */
/* The state lives in one millisecond countdown with two values that  */
/* are not durations: -1 means idle and 1 means the timer has run out */
/* and the transition is due. That is the original's shape and it is  */
/* kept, because the reload arithmetic depends on it.                 */
/* ================================================================== */

#define RACE_SEQ_IDLE        (-1)
#define RACE_SEQ_DUE           1
#define RACE_SEQ_MS         7000   /* 0x00412a8f */
#define RACE_SEQ_MS_CREDITS 14000  /* 0x00412ab4 */
#define RACE_SEQ_CUE        0x3e   /* FUN_00404da0(0x3e, 500)        */
/*
 * THE GAME-OVER COUNTDOWN IS A SECOND TIMER, not the one above.
 * DAT_004e9828 against the sequence's own DAT_004e9824, and it
 * runs 3000 ms from the clock expiring to the continue menu.
 * 0x00412a72.
 */
#define RACE_OVER_MS        3000

static int   s_race_autoRide;         /* DAT_00504ea0  */
static int   s_race_creditsRunning;   /* DAT_004f15f4  */
static int   s_race_seqTimer = RACE_SEQ_IDLE;   /* DAT_004e9824 */
/*
 * THE CLOCK RAN OUT - 0x0052c238, set by race_clock_tick and
 * read by the post-race sequence, which waits 3000 ms on it
 * and then opens the continue menu.  [open - the menu]
 */
static int   s_race_gameOver;
/* 0x004f0c54 - when the timeout line last spoke, not a flag. */
static int   s_race_timeoutMs = -100000;
static int   s_race_overTimer = RACE_SEQ_IDLE;  /* DAT_004e9828 */
static int   s_race_continueShown;
static int   s_race_promptShown;    /* the net arm, ours */
static int   s_race_ghostSaved;
static float s_race_savedSpeedMax;    /* DAT_00504ea4  */
/*
 * DAT_004dbe38 IS A SAVE SLOT IN THE ORIGINAL AND A FLAG HERE,
 * and that is a deliberate divergence.
 *
 * FUN_00412960 hides the counter for the auto-ride by writing 0
 * into g_cfgShowCounter - the CONFIG global - and keeping the old
 * value in DAT_004dbe38 to put back 14 seconds later.  COPYING THAT
 * CORRUPTS THE INI: options_save writes whatever
 * g_options.showCounter happens to hold, so ANY exit that skips the
 * restore - quit race, the continue menu, restarting, closing the
 * window - saves ShowCounter=0 and the counter never comes back on
 * any later run.
 *
 * The original is exposed to the same hazard and gets away with
 * it because its takeover only arms on a first-place single-race
 * finish and always runs its restore.  A port with a quit key in
 * the middle of that window does not get away with it.
 *
 * So the config value is never written.  This flag sits beside it
 * and hud_counter_type reads both, which means no exit path can
 * leak and there is nothing to restore.
 */
static int   s_race_counterHidden;

/* ORIGIN  NONE - ours. Reads what the original keeps in
 *         DAT_00504ea0, the director's gate in Camera_SelectView */
int race_auto_ride(void)
{
	return s_race_autoRide;
}

/*
 * ORIGIN  FUN_0048aca0 @ 0x0048aca0 - A STUB
 *
 * The real one arms the credits: it stores the variant, sets the
 * running flag the sequence waits on, and starts CD audio track 12.
 * The credits themselves are a self-contained subsystem with their
 * own object arrays and teardown (FUN_0048acd0, FUN_0048ae40,
 * FUN_0048a9e0, FUN_0048b070) and are not ported.
 *
 * LEAVING THE FLAG CLEAR IS WHAT MAKES THIS SAFE: the sequence then
 * finds the credits already finished and falls straight through to
 * the restore, so the fourteen-second window expires on its TIMER
 * rather than on the credits ending. Behaviourally close; not the
 * same thing. [open - the credits subsystem]
 */
static void race_credits_begin(int variant)
{
	mrlog("race: credits would start here, variant %d "
	      "(not ported)\n", variant);
	s_race_creditsRunning = 0;
}

/*
 * ORIGIN  FUN_00412960 @ 0x00412960, whole
 *
 * FOUR GATES, AND THE LAST ONE IS THE INTERESTING ONE: the auto-ride
 * is for a WINNER. finishPosition > 0 refuses, so second place
 * onward simply stops at the line - which is why testing a fourth
 * place finish shows none of this.
 *
 * g_dwReplayMode's arm is not reproduced: its only action there is to
 * clear a flag that is already clear. The port's replay is its own
 * recording of the local racer and does not touch that global.
 * [deliberate divergence]
 */
static int race_auto_ride_gate(void)
{
	MR_Racer *r;

	if (s_race_autoRide)
		return 1;
	if (g_soloMode == 0)
		return 0;
	if (g_selectedMode != 1)        /* single race only */
	    return 0;

	r = &s_race_racers[s_race_localIndex];
	if (r->finishPosition > 0)      /* first place only */
	    return 0;

	racer_ai_takeover(r, &s_race_savedSpeedMax);

	s_race_autoRide        = 1;
	s_race_counterHidden   = 1;
	s_race_cam.selectFlag  = 0;
	s_race_cam.lookYaw     = 0.0f;
	s_race_cam.shot        = CAM_SHOT_CHASE;
	s_race_cam.shotTimer   = -1;    /* cut on the next frame */
	mrlog("race: auto-ride armed - the winner keeps riding\n");
	return 1;
}

/* ORIGIN  NONE - ours. The flag that stands in for DAT_004dbe38;
 *         hud_counter_type reads this beside the config value. */
int race_counter_hidden(void)
{
	return s_race_counterHidden;
}


/* ORIGIN  FUN_00410120 @ 0x00410120, its sequence half */
void race_post_race_reset(void)
{
	s_race_autoRide       = 0;
	s_race_counterHidden  = 0;
	s_race_creditsShown   = 0;
	s_race_creditsRunning = 0;
	s_race_seqTimer       = RACE_SEQ_IDLE;
	s_race_gameOver       = 0;
	s_race_timeoutMs      = -100000;
	s_race_overTimer      = RACE_SEQ_IDLE;
	s_race_continueShown  = 0;
	s_race_promptShown    = 0;
	s_race_ghostSaved     = 0;
}

/* ORIGIN  FUN_00412a50 @ 0x00412a50 */
void race_post_race_tick(int frameDeltaMs)
{
	if (!race_ready())
		return;

	/*
	 * DAT_0052c238 IS THE CLOCK-RAN-OUT FLAG, and NOT an "everybody
	 * else has finished" path - which is the reading it invites and
	 * which would make it unreachable in solo.  It is the reverse:
	 * race_clock_tick sets it when the race clock goes 500 ms past
	 * zero, the original counts 3000 ms down on DAT_004e9828, and
	 * then calls FUN_004358b0 - the CONTINUE menu - ONLY when
	 * g_nSoloMode is set.  SOLO IS THE ONLY WAY TO REACH IT.
	 *
	 * s_race_gameOver is that flag.  The countdown and the menu
	 * are not built. [open]
	 */
	if (!race_over_hud()) {
		/*
		 * THE OTHER WAY A RACE ENDS.  Finishing raises
		 * race_over_hud and runs the sequence below; the
		 * CLOCK running out raises nothing and comes through
		 * here instead, on its own 3000 ms timer.
		 *
		 * ORIGIN  FUN_00412a50 @ 0x00412a50, at 0x00412a72 -
		 *         the DAT_0052c238 arm
		 *
		 *     -1   arm it at 3000
		 *     >0   count down
		 *      0   solo -> FUN_004358b0, the continue menu
		 *
		 * THE 3000 ms IS WHAT KEEPS THE VOICES APART.  GAME
		 * OVER is queued when the clock expires and TRY AGAIN
		 * comes three seconds later, by which time the queue
		 * has long drained - and FUN_004358b0 opens with
		 * Snd_StopAll anyway, so anything still pending is
		 * DISCARDED rather than spoken over it.
		 *
		 * TRY AGAIN IS PLAYED DIRECT, not queued - like the
		 * countdown digits and unlike every other announcer
		 * line.  Queueing it behind a Snd_StopAll would be
		 * silent.
		 */
		if (!s_race_gameOver)
			return;

		if (s_race_overTimer == RACE_SEQ_IDLE) {
			s_race_overTimer = RACE_OVER_MS;
			return;
		}
		if (s_race_overTimer > 0) {
			s_race_overTimer -= frameDeltaMs;
			if (s_race_overTimer < 0)
				s_race_overTimer = 0;
			return;
		}

		/*
		 * ONCE, WHERE THE ORIGINAL CALLS IT EVERY FRAME.
		 * 0x00412a85 reaches FUN_004358b0 with the timer
		 * parked at 0 and nothing clearing it, so it rebuilds
		 * the whole menu object per frame.  Reproducing that
		 * would restart the sound and leak the widget set;
		 * whatever stops it over there has not been found.
		 * [deliberate divergence]
		 */
		if (g_soloMode != 0 && !s_race_continueShown) {
			s_race_continueShown = 1;
			mrlog("race:  continue prompt\n");
			/* THE MENU SILENCES THE RACE AND SPEAKS TRY
			   AGAIN ITSELF - FUN_004358b0 opens with
			   Snd_StopAll and then Snd_PlayCue(0x44, 0).
			   Doing either here would be doing it twice. */
			hud_continue_open();
		}
		return;
	}

	if (s_race_seqTimer == RACE_SEQ_IDLE) {
		s_race_seqTimer = RACE_SEQ_MS;
		/*
		 * RACE COMPLETED GOES THROUGH THE ANNOUNCER QUEUE,
		 * which is what keeps it OFF the result line.
		 *
		 * ORIGIN  FUN_00412a50 @ 0x00412b3e -
		 *         Snd_QueueVoice(0x3e, 500), not a direct
		 *         play.  audio_cue here instead speaks at
		 *         once, over the top of the position line
		 *         race_update_progress queued a moment
		 *         earlier.
		 *
		 * THE ORDER FALLS OUT OF THE QUEUE BEING FIFO. The
		 * finish queues the result first and this follows,
		 * so it lands after whatever was said about the
		 * placing - and after nothing at all on a second or
		 * third place, which says only this.
		 */
		audio_cue_delayed(RACE_SEQ_CUE, SND_VOICE_DELAY_MS);

		if (!s_race_autoRide && race_auto_ride_gate())
			s_race_seqTimer = RACE_SEQ_MS_CREDITS;
		return;
	}

	if (s_race_seqTimer == RACE_SEQ_DUE) {
		if (s_race_autoRide && !s_race_creditsShown) {
			/* 0x00412b0c - which of two variants is a coin
			 * toss weighted 15:1, not an even one. */
			race_credits_begin((rand() & 0xffff) > 0xfff);
			s_race_creditsShown = 1;
		}
		if (s_race_autoRide && !s_race_creditsRunning) {
			racer_restore_human(&s_race_racers[s_race_localIndex],
			                    s_race_savedSpeedMax);
			s_race_counterHidden  = 0;
			s_race_autoRide       = 0;
			s_race_creditsShown   = 0;
			s_race_seqTimer       = 0;
			mrlog("race: auto-ride over, player restored\n");
		} else if (!s_race_autoRide) {
			s_race_seqTimer = 0;
		}
		return;
	}

	if (s_race_seqTimer == 0) {
		/*
		 * THE DISPATCH, and it is per mode. A solo single race
		 * gets the post-race question - FUN_00437b70, [REPLAY]
		 * YES/NO - and practice gets the continue prompt, with
		 * time attack writing its ghost on the way.
		 * Championship goes to page 0x18 or FUN_004580f0 and is
		 * not reached here yet. [open]
		 */
		if (g_soloMode == 0) {
			if (!s_race_promptShown) {
				s_race_promptShown = 1;
				mrlog("race:  net post-race prompt\n");
				hud_prompt_open();
			}
			return;
		}

		if (g_selectedMode == 1) {
			hud_prompt_open();
		} else if (g_selectedMode == 0) {
			/*
			 * ONCE, WHERE THE DISPATCH RUNS EVERY FRAME. The
			 * timer parks at 0 and nothing clears it, so this
			 * arm repeats for as long as the results page is
			 * up - and the original has the same shape, which
			 * would promote and rewrite a 150 KB file per
			 * frame. See the note on the continue menu below.
			 * [deliberate divergence]
			 */
			if (g_timeAttack != 0 && !s_race_ghostSaved) {
				s_race_ghostSaved = 1;
				race_ghost_race_done(&s_race_racers[s_race_localIndex]);
			}

			/*
			 * FUN_00464680 RUNS FIRST IN THE ORIGINAL - the
			 * bestscor.dat table, 25 records of 0xf9 keyed by
			 * track plus the pocket and reverse suffixes. Not
			 * ported, so practice records nothing but the
			 * ghost. [open - docs/formats/bestscor.txt]
			 */
			if (!s_race_continueShown) {
				s_race_continueShown = 1;
				hud_continue_open();
			}
		}
		return;
	}

	s_race_seqTimer -= frameDeltaMs;
	if (s_race_seqTimer < 0)
		s_race_seqTimer = RACE_SEQ_DUE;
}

/* ================================================================== */
/* THE RACE TICK.                                                     */
/* Sources: maybe_Race_TickClock 0x0047dae0, Race_TickAllRacers       */
/* 0x0047df40, Racer_TickDispatch 0x0045d540. See race.h and          */
/* docs/subsystems/racers-convergence.txt 9.25.                     */
/* ================================================================== */

/* g_frameDelta, published by the dispatch before it forks. */
static float s_frameDelta = 0.0f;

#define K_DT_TICK_SCALE   0.1f     /* FLT_TICK_DT_SCALE 0x004d1970  */
#define K_DT_GUARD        0.0f     /* 0x004d1900 - a ZERO test      */
#define K_DT_GUARD_VALUE  1.0f     /* what a negative step becomes  */


/* ORIGIN  NONE - ours. Reads what Racer_TickDispatch publishes
 *         into g_frameDelta */
float race_frame_delta(void)
{
	return s_frameDelta;
}


/* ORIGIN  NONE - ours. See s_race_camView. */
const float *race_camera_view(void)
{
	return s_race_camView;
}

const float *race_camera_eye(void)
{
	return s_race_camEyePos;
}


/* ORIGIN  Race_TickAllRacers @ 0x0047df40, its first three lines */
float race_scale_dt(float dtScaled)
{
	dtScaled *= K_DT_TICK_SCALE;

	/*
	 * A NEGATIVE GUARD, NOT A FLOOR - see race.h. The
	 * comparison is against zero and the replacement is 1.0, which
	 * is enormous next to the 3.3 a normal frame produces.
	 */
	if (dtScaled < K_DT_GUARD)
		dtScaled = K_DT_GUARD_VALUE;

	return dtScaled;
}


/*
 * ORIGIN  maybe_Race_TickClock @ 0x0047dae0, the inline per-racer test
 *
 * Which racers the clock touches. Active, still racing, and either us
 * or an AI - a REMOTE human's clock is run by the machine that owns
 * it, not by ours.
 */
static int clock_eligible(const MR_Racer *r, int index, int localIndex)
{
	if (!r->active)
		return 0;
	if (r->raceState != RACESTATE_RACING_0 &&
	    r->raceState != RACESTATE_RACING_1) {
		return 0;
	}
	return (index == localIndex) || (r->humanPlayer == 0);
}


/* ORIGIN  maybe_Race_TickClock @ 0x0047dae0, the g_bPreRace
 *         transition */
void race_clock_start(MR_RaceClock *c, MR_Racer *racers, int count,
                      int localIndex, int segmentCount,
                      int hasTimeLimit, int trackTimeMs,
                      int difficultyBonusMs, int invertHalfTest,
                      int *lapTimes)
{
	int i;

	c->preRace = 0;
	c->started = 1;

	/*
	 * The time limit. trackTimeMs is the track record's field at
	 * track+0xda; solo adds a per-difficulty bonus from the table
	 * at 0x00515fe0, network adds a flat two minutes.
	 *
	 * AND THIS ASSIGNMENT IS ALL hasTimeLimit GATES. The JL at
	 * 0x0047dd74 jumps past the reload and nothing else, so the
	 * clock zeroing and the raceState assignment below happen
	 * whether the track carries a limit or not.
	 */
	if (hasTimeLimit)
		c->timerMs = trackTimeMs + difficultyBonusMs;

	for (i = 0; i < count; i++) {
		MR_Racer *r = &racers[i];
		int       j;

		/*
		 * NOTE THIS TEST IS NOT clock_eligible - the reset asks
		 * only active plus us-or-AI, and does NOT look at
		 * raceState. It is about to assign raceState below, so
		 * filtering on it here would be circular.
		 */
		if (!r->active)
			continue;
		if (i != localIndex && r->humanPlayer != 0)
			continue;

		r->raceClockMs = 0;
		for (j = 0; j < RACE_LAP_SLOTS; j++)
			lapTimes[i * RACE_LAP_SLOTS + j] = 0;
	}

	/*
	 * raceState 1 by which half of the track the grid slot is on,
	 * with invertHalfTest flipping it. The original walks
	 * g_racerCount entries from index 0 and tests
	 * startSegment < 0 || >= half.
	 *
	 * The two arms assign 1 on OPPOSITE values of the flag, which
	 * collapses to this equality - and unlike the loops above this
	 * one does not test active at all.
	 */
	for (i = 0; i < count; i++) {
		MR_Racer *r    = &racers[i];
		int       past = (r->segmentStart < 0)
		                 || (r->segmentStart >= segmentCount / 2);

		/*
		 * THE NO-TIME SENTINEL, 0x7f7f7f7f. FUN_00411010 fills
		 * the record tables with it at race start and every
		 * reader tests against it rather than against zero.
		 * Left at 0 it reads as a perfect lap that nothing can
		 * beat, so no best lap is ever recorded and the BEST
		 * LAP block stays empty all race.
		 */
		r->bestLapMs = 0x7f7f7f7f;

		if (past == (invertHalfTest != 0))
			r->raceState = RACESTATE_RACING_1;
	}
}


/* ORIGIN  maybe_Race_TickClock @ 0x0047dae0 */
int race_clock_tick(MR_RaceClock *c, MR_Racer *racers, int count,
                    int localIndex, int frameDeltaMs, int *lapTimes)
{
	int i;

	c->started = 0;

	/*
	 * THE WHOLE FUNCTION IS GATED. maybe_Race_TickClock opens with
	 * `if (DAT_004dc0ac != 0)` and does nothing at all otherwise -
	 * so the clock can be suspended wholesale. What writes that
	 * flag is unread, which is why it is a field here rather than
	 * being assumed set. [open]
	 */
	if (!c->enabled)
		return 0;

	/*
	 * 1. The countdown, or the limit - the same variable.
	 *
	 * IN SOLO THE DECREMENT IS UNCONDITIONAL. The original guards it
	 * with a four-term disjunction whose first term is
	 * `g_nSoloMode != 0`, so solo always decrements; the rest of the
	 * condition holds every machine on the grid until the network
	 * handshake completes. [OPTIONS_CUSTOM] FreezeRaceTimer is a DEBUG
	 * OVERRIDE, off by default, so unset this line IS that
	 * unconditional decrement.
	 *
	 * AND preRace IS IN THE TEST FOR A REASON. This variable is TWO
	 * TIMERS - the six-second grid countdown first, the race limit
	 * afterwards, with preRace the only thing that says which (race.h,
	 * racers.txt 9.8.4). Freezing it flatly holds the bike on the grid
	 * for ever, because the countdown never expires. mr_place.py guards
	 * it the same way - racers-convergence.txt 9.46.16.
	 *
	 * o IT SKIPS THE PENALTY BY NEVER ARMING IT. With the timer held
	 *   off zero the expiry arm below never runs, so raceState is never
	 *   written and racer_apply_drive never sees a reason to hold the
	 *   brake. Nothing suppresses the forced stop; it simply never
	 *   happens.
	 */
	if (c->preRace || !g_options.freezeRaceTimer)
		c->timerMs -= frameDeltaMs;

	/*
	 * THE TIME LIMIT EXPIRING, AND IT IS ONE ARM WITH TWO
	 * HALVES - not two independent checks.
	 *
	 * ORIGIN  maybe_Race_TickClock @ 0x0047dae0, at
	 *         0x0047de56 onward
	 *
	 *     if (timer < 0) {
	 *         if (timer + 500 < 0)  GAME OVER
	 *         else                  the TIMEOUT nag
	 *     }
	 *
	 * SO THE NAG LIVES IN A 500 ms WINDOW, between the clock
	 * passing zero and game over - not 'while the clock is
	 * low', which is what this looked like from the call site
	 * alone.  Its own rate limit is 2000 ms, wider than the
	 * window, so in practice it speaks ONCE.
	 *
	 * Confirmed by putting MOTO's clock at -200 and stepping:
	 * the arm ran and queued 0x42.  At any positive clock it
	 * is never reached at all.  [verified - live]
	 *
	 * PRACTICE IS UNTIMED and the gate is HERE, on the arm,
	 * not on the clock: mode 0 with no time attack never runs
	 * any of this and the timer simply counts on into deep
	 * negatives with nothing reading it.
	 *
	 * THE raceState TERM IS NOT A RE-ENTRY LATCH.  It also
	 * stops a racer who has FINISHED - state 2 - being
	 * overwritten as timed out.
	 *
	 * [open] obj+0x0c gates the nag in the original, on the
	 * game object at 0x0052c478.  It read 1 throughout a
	 * normal race and through the window, so it is taken as
	 * true here; what would make it false is unknown.
	 *
	 * [open] Both banners - FUN_00482120 for GAME OVER and
	 * FUN_00481af0 for TIMEOUT, the blinking one.
	 */
	if (c->enabled && !c->preRace && c->timerMs < 0 &&
	    (g_selectedMode != 0 || g_timeAttack) &&
	    count > localIndex &&
	    (racers[localIndex].raceState == RACESTATE_RACING_0 ||
	     racers[localIndex].raceState == RACESTATE_RACING_1)) {

		if (c->timerMs + 500 < 0) {
			/*
			 * raceState 3 IS TIMED OUT, and this is its only
			 * writer anywhere - local racer only.
			 */
			s_race_gameOver = 1;
			racers[localIndex].raceState = RACESTATE_TIMED_OUT;

			/*
			 * THE BANNER BEFORE THE VOICE, which is the
			 * order the eight instructions at 0x0047deac
			 * run in: the flag, FUN_00482120(0), then
			 * Snd_QueueVoice(0x39, 500).
			 */
			hud_banner_gameover();
			audio_cue_delayed(SND_VOICE_GAMEOVER,
			                  SND_VOICE_DELAY_MS);
			mrlog("race:  GAME OVER - clock out\n");
		} else {
			/*
			 * THE BANNER IS RAISED EVERY FRAME OF THE
			 * WINDOW and the VOICE is rate limited - the
			 * original calls FUN_00481af0 outside its own
			 * 2000 ms test, at 0x0047df09, and only the
			 * Snd_QueueVoice sits inside it.
			 *
			 * hud_banner_timeout is idempotent, so the
			 * repeat costs nothing and the blink does not
			 * restart.
			 */
			hud_banner_timeout();

			if (s_race_timeoutMs + 2000 <
			    (int)(tm_now() * 1000.0)) {
				s_race_timeoutMs = (int)(tm_now() * 1000.0);
				audio_cue_delayed(SND_VOICE_TIMEOUT,
				                  SND_VOICE_DELAY_MS);
			}
		}
	}

	/*
	 * 2. The per-racer clocks, but ONLY once the grid has cleared.
	 *    This runs before the transition test below, which is why
	 *    the starting frame advances nothing.
	 */
	if (!c->preRace) {
		for (i = 0; i < count; i++) {
			MR_Racer *r = &racers[i];
			int       lap;

			if (!clock_eligible(r, i, localIndex))
				continue;

			r->raceClockMs += frameDeltaMs;

			lap = r->lapCounter;
			if (lap >= 0 && lap < RACE_LAP_SLOTS)
				lapTimes[i * RACE_LAP_SLOTS + lap] += frameDeltaMs;
			/*
			 * THE ORIGINAL HAS NO SUCH BOUNDS TEST - it
			 * indexes g_lapTimes by the raw 16-bit lapCounter
			 * and would walk into the next racer's row on a
			 * ninth lap. Guarded here because a reconstruction
			 * that reproduces an out-of-bounds write is
			 * reproducing a crash, not a behaviour. Flagged
			 * rather than silent. [deliberate divergence]
			 */
		}
	}

	/* 3. The transition, tested as < 0 rather than == 0. */
	if (c->timerMs < 0) {
		if (c->preRace) {
			/*
			 * THE RACE STARTS. The caller supplies the limit
			 * terms because they come from the track record
			 * and the difficulty table, neither of which this
			 * module owns.
			 */
			return 1;
		}

		/*
		 * THE LIMIT EXPIRED. The original allows a 500 unit
		 * grace, re-shows a warning and plays a jingle no more
		 * often than every 2000 units, and only then writes
		 * raceState 3 - for the LOCAL racer alone.
		 *
		 * The warning and the jingles are presentation and are
		 * not transcribed; the state write is.
		 *
		 * THE WHOLE ARM IS GATED, three terms at 0x0047de56,
		 * and the gate is not decoration - it is what makes a
		 * PRACTICE race untimed. Mode 0 is practice in a solo
		 * game (menu.c, page 5: the mode is the button index
		 * minus one), and in it this arm never runs at all.
		 * The timer still counts on down into deep negatives;
		 * nothing reads it. An import that drops the gate
		 * locks the bike to the brake forty seconds into a
		 * practice lap with nothing on screen to say why.
		 *
		 * o AND raceState 0-OR-1 IS THE SECOND TERM, not a
		 *   latch - it does BOTH jobs: it keeps a racer who
		 *   has FINISHED, state 2, from being overwritten as
		 *   timed out, and it stops the arm re-firing once it
		 *   has written 3. A plain latch covers only the
		 *   second.
		 *
		 * o THE THIRD TERM, DAT_004e9830 == 0, IS OMITTED.
		 *   It suppresses the timeout during the attract
		 *   demo, which this port does not have, so the term
		 *   can never be false here. Named rather than
		 *   dropped silently. [open - the flag's identity;
		 *   its writers point at the attract demo but the
		 *   polarity is not verified]
		 */
		{
			int localState = (localIndex >= 0 && localIndex < count)
			                 ? racers[localIndex].raceState : -1;
			if ((localState == RACESTATE_RACING_0 ||
			     localState == RACESTATE_RACING_1) &&
			    (g_selectedMode != 0 || g_timeAttack != 0) &&
			    c->timerMs + 500 < 0) {
				racers[localIndex].raceState = RACESTATE_TIMED_OUT;
			}
		}
	}

	return 0;
}


/*
 * dtScaled BACK TO MILLISECONDS. g_frameDeltaScaled is 3.3 at a 33 ms
 * step (racers.txt 9.2), so the scale is a tenth and this undoes it.
 * The AI needs ms because racer_update_lean divides by it.
 */
#define RACE_DT_UNSCALE  10.0f

/* ORIGIN  Racer_TickDispatch @ 0x0045d540 */
void racer_tick_dispatch(MR_Racer *r, unsigned int buttons,
                         int axis0, int axis1, float dtScaled)
{
	/*
	 * PUBLISH THE STEP FIRST. Functions further down the chain read
	 * g_frameDelta rather than being passed it.
	 */
	s_frameDelta = dtScaled;

	if (r->humanPlayer == 0) {
		/*
		 * THE AI ARM. The original hands it ONLY the delta - no
		 * buttons, no axes - so an AI does not drive through a
		 * synthetic controller. The carac and the track are
		 * globals there and parameters here.
		 *
		 * NO DRIVETRAIN IS PASSED. s_race_drive is racer 1's and
		 * an opponent has none; racer_tick_ai takes NULL and
		 * scales its own engineSpeed instead.
		 */
		{
			MR_AiEnv env;

			/*
			 * THE GRID HOLD. race_pre_race is g_bPreRace, and
			 * racer_tick_ai reads it to suppress the wheelie, the
			 * steering and the drive while the countdown runs - a
			 * literal 0 here launches the field on the first frame,
			 * see race_pre_race.
			 */
			env.preRace       = race_pre_race();
			env.viewedSegment = s_race_racers[s_race_localIndex].segmentStart;
			env.viewedIndex   = 1;
			env.dtMs          = dtScaled * RACE_DT_UNSCALE;
			env.raceDir       = g_reverseEnabled ? -1 : 1;
			env.racers        = s_race_racers;
			env.racerCount    = s_race_racerCount;
			/* racer_tick_remote only; racer_tick_ai ignores it. */
			env.selectedMode  = g_selectedMode;
			/*
			 * 0x004dc144 IS THE REVERSE FLAG. maybe_Game_LoadRace
			 * copies the settings dword at 0x00542878 into it at
			 * 0x0047b460, and MenuPage01_Loading_Tick copies the
			 * SAME dword into g_reverseEnabled at 0x0040b854 - one
			 * source, two latches. It swaps the lane windows from
			 * ahead to behind, which is right: running the track
			 * backwards makes a higher segment index the way the
			 * bike came from.
			 */
			env.mirrorWindow  = g_reverseEnabled;

			if (racer_tick_ai(r, bike_at(r->bikeIndex),
			                  &s_race_trk, NULL, &env,
			                  dtScaled) == RACER_CRASH_DONE) {
				/*
				 * WITHOUT THIS THE RIDER STAYS DOWN for the
				 * rest of the race - still lying on the track a
				 * lap later, and shovable like scenery. The
				 * player's arm in race_tick_physics tests the
				 * same thing.
				 *
				 * VIEWED RACER 1, the same constant the player
				 * arm passes: it is the only racer this port
				 * watches, and the gate that keeps the field
				 * down together reads it. [open - when the AI
				 * runs]
				 */
				(void)race_reset_crashes(r->slotIndex, 1);
			}
		}
		return;
	}

	/*
	 * maybe_Racer_Tick IS race_tick_physics.
	 *
	 * THE BUTTONS ARE NOT PASSED ON because race_tick_physics reads
	 * them off the racer through race_poll_input. The original hands
	 * them down as arguments; the port keeps them on MR_Racer, so
	 * they arrive by a different road rather than not at all.
	 */
	(void)buttons;
	(void)axis0;
	(void)axis1;
	race_tick_physics(r, dtScaled);
	/*
	 * AND THE RIDER'S PITCH FILTER, ONCE PER STEP.
	 *
	 * maybe_Racer_UpdateSkeleton is reached from four places and
	 * all four are ticks - maybe_Racer_Tick 0x0045d7d8,
	 * maybe_Racer_TickAI 0x0046c783, maybe_Racer_TickRemote
	 * 0x0045f12c and maybe_Game_Tick 0x0047d8df - so every
	 * racer's pose advances exactly once a simulation step and
	 * never on a rendered frame.
	 *
	 * OURS POSES FROM THE DRAW because racer_update_pose needs
	 * the camera distance for its far fade, and the draw is the
	 * only place that exists. Everything in there is a pure
	 * function of its inputs EXCEPT poseBlend, a quarter-step
	 * filter with no dt - so posing from the draw advances that
	 * one field on every frame the app renders, PAUSED ONES
	 * INCLUDED, and the rider finishes settling into his pose
	 * after the game has stopped. Confirmed by hook: eight calls
	 * with no input sent and the contacts frozen.
	 *
	 * This is racer_ai_lean_step's hazard one subsystem over, and
	 * race_update's dt > 0 guard does NOT reach it - that guard
	 * covers the TICK and this runs in the DRAW.
	 */
	racer_pose_pitch_tick(r, bike_at(r->bikeIndex));
}


/* ================================================================== */
/* RACER AGAINST RACER COLLISION                                      */
/*                                                                    */
/* maybe_Game_CollideRacers (0x0047ecd0) and its three helpers. The   */
/* gates, the constants and the four outcomes are written up in       */
/* docs/subsystems/racer-collision.txt; this file carries why the C   */
/* is shaped the way it is.                                           */
/* ================================================================== */

#define K_SWEEP_EPS            1e-05f   /* 0x004d0388, a double      */
#define K_COLLIDE_RADIUS        48.0f   /* the literal at both args  */
#define K_COLLIDE_T_MIN         0.05f   /* 0x004d1988, a double      */
#define K_COLLIDE_SPEED      0.78125f   /* 0x004d1990, a double      */
#define K_COLLIDE_DV_HARD       25.0f   /* the literal 0x41c80000    */
#define K_COLLIDE_DV_SOFT       12.5f   /* 0x004d19a0                */
#define K_COLLIDE_LEAN      0.698132f   /* 0x004d19a8, 40 degrees    */
#define K_COLLIDE_FAST     22.222221f   /* 0x004d19b0, 80 km/h       */
#define K_COLLIDE_SLOW     5.5555553f   /* 0x004d19d0, 20 km/h       */
#define K_COLLIDE_AI_KEEP        0.9f   /* 0x004d1928                */
#define K_COLLIDE_PUSH           1.1f   /* 0x004d1940                */
#define K_COLLIDE_HUMAN_KEEP    0.95f   /* 0x004d19c8                */
#define K_COLLIDE_BLEND        200.0f   /* the literal 0x43480000    */
#define K_COLLIDE_WHEEL_RADIUS  -96.0f  /* 0x004d19b8                */
#define K_COLLIDE_WHEELBASE_R   -48.0f  /* 0x004d19bc                */
#define K_COLLIDE_WHEELBASE_F    48.0f  /* 0x004d1980                */

/* The pair bitmap is 25 x 25 on the original's stack. */
#define RACE_COLLIDE_MAX 25

/*
 * ORIGIN  Math_SweepSpheres @ 0x00411f00, whole.
 *
 * SWEPT SPHERE AGAINST SWEPT SPHERE. Both move in a straight line over
 * the step; solve for the first time their separation equals the sum
 * of the radii. A textbook quadratic, and the two constants at
 * 0x004d0390 and 0x004d0394 are its 2 and its 4.
 *
 *     d = (a1 - a0) - (b1 - b0)        relative motion
 *     p = a0 - b0                      relative start
 *     a = |d|^2                        below 1e-05 -> not moving
 *     b = 2 * dot(d, p)
 *     c = |p|^2 - (rA + rB)^2
 *
 * IT DOES NOT TEST t <= 1. Both roots must be non-negative and the
 * SMALLER is returned; bounding it to the step is the caller's job.
 *
 * *tOut is set to 10000 on every early return - the original's own
 * "no hit" marker, written even though it also returns 0.
 */
static int race_sweep_spheres(const float a0[3], const float b0[3],
                              const float a1[3], const float b1[3],
                              float rA, float rB, float *tOut)
{
	float d[3], p[3];
	float qa, qb, disc, root, t0, t1;
	int   i;

	for (i = 0; i < 3; i++) {
		d[i] = (a1[i] - a0[i]) - (b1[i] - b0[i]);
		p[i] = a0[i] - b0[i];
	}

	*tOut = 10000.0f;

	qa = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
	if ((float)fabs((double)qa) < K_SWEEP_EPS)
		return 0;

	qb = (d[0] * p[0] + d[1] * p[1] + d[2] * p[2]) * 2.0f;
	disc = qb * qb
	     - (p[0] * p[0] + p[1] * p[1]
	        + (p[2] * p[2] - (rA + rB) * (rA + rB))) * qa * 4.0f;
	if (disc < 0.0f)
		return 0;

	root = (float)sqrt((double)disc);
	t0 = (root - qb) / (qa * 2.0f);
	t1 = -(root + qb) / (qa * 2.0f);
	if (t1 < t0) {
		float s = t0; t0 = t1; t1 = s;
	}

	/*
	 * BOTH ROOTS NON-NEGATIVE. The original spells it as an UNSIGNED
	 * compare of the float bits against 0x80000001, which admits
	 * every positive float and both zeroes and rejects everything
	 * else - so it is exactly ">= 0.0f", negative zero included.
	 */
	if (t1 < 0.0f || t0 < 0.0f)
		return 0;

	*tOut = t0;
	return 1;
}


/*
 * ORIGIN  Trk_SegmentDelta @ 0x0046c9f0, whole.
 *
 * Segments from a to b going FORWARD round the circuit, so it is never
 * negative and it wraps at the start/finish line.
 */
static int race_segment_delta(int a, int b)
{
	if (a <= b)
		return b - a;
	return (b - a) + s_race_trk.segmentCount;
}


/*
 * ORIGIN  Racer_ClearStunt @ 0x00466220, whole.
 *
 * Two flags cleared on a racer that has just been hit; the second only
 * for a human player in a solo game, which is what its two tests come
 * to.
 */
static void race_clear_stunt(MR_Racer *r)
{
	r->stuntOrOverspeed = 0;
	if (r->humanPlayer != 0 && r->soloMode != 0)
		r->stuntHeld = 0;
}


/*
 * ORIGIN  maybe_Game_CollideRacers @ 0x0047ecd0, inlined at 0x0047f2a0
 *
 * Rebuild one racer's two wheel contacts from its orientation. This is
 * maybe_Racer_PlaceWheels' arithmetic, and the collision INLINES it
 * rather than calling it - which it has to, because PlaceWheels also
 * rewrites positionPrev and the whole point here is that the position
 * moved and the history did not.
 */
static void race_replace_wheels(MR_Racer *r, const MR_BikeCarac *c)
{
	float up, along;
	int   i;

	if (!c)
		return;

	up    = c->rayonAr * K_COLLIDE_WHEEL_RADIUS;
	along = c->axeDistance * K_COLLIDE_WHEELBASE_R;
	for (i = 0; i < 3; i++)
		r->rearWheelContact[i] = up * r->matOrientation[3 + i]
	                           + along * r->matOrientation[6 + i]
	                           + r->matOrientation[9 + i]
	                           + r->position[i];

	up    = c->rayonAv * K_COLLIDE_WHEEL_RADIUS;
	along = c->axeDistance * K_COLLIDE_WHEELBASE_F;
	for (i = 0; i < 3; i++)
		r->frontWheelContact[i] = up * r->matOrientation[3 + i]
	                            + along * r->matOrientation[6 + i]
	                            + r->matOrientation[9 + i]
	                            + r->position[i];
}


/*
 * ORIGIN  NONE - ours; the original inlines this.
 *
 * dst = v normalised, or v unchanged when it has no length. Returns
 * the length either way, which race_collide_racers uses to turn a
 * frame's displacement into a speed. NEITHER COLLISION OUTCOME WANTS
 * IT - the original divides by the length there and discards it.
 */
static float race_norm3(float dst[3], const float v[3])
{
	float len = (float)sqrt((double)(v[0] * v[0] + v[1] * v[1]
	                                 + v[2] * v[2]));
	int   i;

	for (i = 0; i < 3; i++)
		dst[i] = v[i];
	if (len != 0.0f) {
		float inv = 1.0f / len;

		for (i = 0; i < 3; i++)
			dst[i] = v[i] * inv;
	}
	return len;
}


/*
 * ORIGIN  maybe_Game_CollideRacers @ 0x0047ecd0, the rewind
 *
 * The push each racer is rewound to.
 *
 *     position = (position - positionPrev) * t + positionPrev - n
 *
 * so it goes back to where it was when the spheres touched and is
 * then displaced by ONE UNIT of the normal. positionPrev is left
 * alone, which is what makes the next frame's sweep start from the
 * un-corrected history.
 */
static void race_collide_separate(MR_Racer *r, const float n[3], float t)
{
	int i;

	for (i = 0; i < 3; i++)
		r->position[i] = (r->position[i] - r->positionPrev[i]) * t
	                   + r->positionPrev[i] - n[i];
	race_replace_wheels(r, bike_at(r->bikeIndex));
	r->flImpactBlend = K_COLLIDE_BLEND;
}


/*
 * ORIGIN  NONE - ours. The original keeps the gear on the racer at
 *         +0x7c and needs no such lookup.
 *
 * THE GEARBOX BELONGS TO THE PLAYER AND ONLY TO THE PLAYER.
 *
 * racer_begin_crash drops the gearbox to first, and the port has
 * exactly ONE MR_Gearbox - the local rider's - because the
 * original keeps its gear on the racer at +0x7c and the port has
 * not retired the separate object yet. Handing it to an AI's
 * crash would put the PLAYER into first gear every time two
 * opponents touched, which is a bug the per-racer original
 * cannot have.
 *
 * NULL is fine: racer_begin_crash still sets r->gear, which is
 * the field the original actually writes.
 */
static MR_Gearbox *race_gearbox_of(const MR_Racer *r)
{
	return (r && r->humanPlayer) ? &s_race_gearbox : NULL;
}


/*
 * ORIGIN  maybe_Game_CollideRacers @ 0x0047ecd0, at 0x0047f7c6 and
 *         0x0047f8f0 - the FASTER racer's two branches.
 *
 * Whichever of the pair was travelling faster is slowed to the OTHER
 * one's speed, scaled: 0.95 for a human, 0.9 for the AI. Head-on
 * stops it dead (human) or drops it to 20 km/h (AI) and hands it a
 * push along the other's direction.
 *
 * THE HUMAN PATH RESYNCS THE DRIVETRAIN and the AI path does not: the
 * human goes through SetLongSpeed / SyncRoadSpeed / SpeedToEngine,
 * while the AI just writes speed, engineSpeed and aiRoadSpeed flat.
 */
static void race_collide_fast(MR_Racer *r, const float nFast[3],
                              const float nSlow[3], float spSlow,
                              int crash, int hard, int headOn)
{
	const MR_BikeCarac *c = bike_at(r->bikeIndex);
	int   human = (r->humanPlayer != 0);
	int   i;

	if (!headOn) {
		if (!crash) {
			r->riderStateBits |= 4;
		} else {
			/*
			 * BOTH SPEEDS CLAMPED TO 80 km/h BEFORE THE CRASH,
			 * the racer's own and the one it is about to be set
			 * to. The original clamps the local copy of spSlow,
			 * which is why the assignment below sees the capped
			 * value.
			 */
			if (r->speed > K_COLLIDE_FAST)
				r->speed = K_COLLIDE_FAST;
			if (spSlow > K_COLLIDE_FAST)
				spSlow = K_COLLIDE_FAST;
			racer_begin_crash(r, race_gearbox_of(r), nFast);
			r->riderStateBits |= 8;
		}
		r->speed = spSlow * (human ? K_COLLIDE_HUMAN_KEEP
		                           : K_COLLIDE_AI_KEEP);
	} else {
		if (human) {
			/* The push goes in before the outcome bits here. */
			for (i = 0; i < 3; i++)
				r->collideDir[i] = nSlow[i];
			r->collideSpeed = K_COLLIDE_SLOW;
		}
		if (!hard) {
			r->riderStateBits |= 0x10;
		} else {
			racer_begin_crash(r, race_gearbox_of(r), nFast);
			r->riderStateBits |= 0x20;
		}
		if (human) {
			r->speed = 0.0f;
		} else {
			r->speed = K_COLLIDE_SLOW;
			for (i = 0; i < 3; i++)
				r->moveDir[i] = nSlow[i];
		}
	}

	if (human) {
		physics_set_long_speed(r->matOrientation, r->velDir,
		                       &r->speed, r->realSpeedMax, r->speed);
		physics_sync_road_speed(&r->gearboxRoadSpeed, r->speed,
		                        r->realSpeedMax);
		r->engineSpeed = physics_speed_to_engine(c, r->gear,
		                     &r->gearboxRoadSpeed, r->engineSpeed,
		                     r->driveDisengaged);
	} else {
		r->engineSpeed = r->speed;
		r->aiRoadSpeed = r->speed;
	}
}


/*
 * ORIGIN  maybe_Game_CollideRacers @ 0x0047ecd0, at 0x0047fa1c and
 *         0x0047fc80 - the SLOWER racer's two branches.
 *
 * THE ONE THAT GOT HIT GETS A COMBINED DIRECTION in three of the four
 * cells:
 *
 *     v = nFast * speedFast + nSlow * speedSlow
 *
 * which is the momentum of the pair pointed at the racer that was run
 * into. The exception is a human taking a same-direction shunt, which
 * simply takes the faster racer's direction.
 *
 * ONLY THE DIRECTION IS KEPT. v is normalised in place and its LENGTH
 * IS DISCARDED - the original divides by it and never stores it
 * [verified, the normalise at 0x0047fb8e for a human and 0x0047fd08
 * for an AI]. The push is always a speed or a difference of two:
 *
 *     human, head-on    spFast - spSlow, floored at 20 km/h
 *     human, shunt      spFast * 1.1 - speed, floored at 0
 *     AI, head-on       spFast - spSlow
 *     AI, shunt         spFast
 *
 * with the AI pair then scaled by 1.1 and floored at 20 km/h in the
 * common tail.
 *
 * A HUMAN NEVER CRASHES FROM A SAME-DIRECTION HIT here - that branch
 * has no crash test at all and always sets bit 4. Being rear-ended
 * shoves you; it does not put you off.
 */
static void race_collide_slow(MR_Racer *r, const float nFast[3],
                              const float nSlow[3], float spFast,
                              float spSlow, int crash, int hard,
                              int headOn)
{
	const MR_BikeCarac *c = bike_at(r->bikeIndex);
	int   human = (r->humanPlayer != 0);
	float v[3], n[3];
	int   i;

	for (i = 0; i < 3; i++)
		v[i] = nFast[i] * spFast + nSlow[i] * spSlow;

	if (human) {
		if (!headOn) {
			race_norm3(n, nFast);
			for (i = 0; i < 3; i++)
				r->collideDir[i] = n[i];
			/*
			 * THE PUSH IS WHAT IS LEFT OF THE OTHER BIKE'S SPEED
			 * after this one's own is taken out, and it is
			 * clamped at zero rather than allowed to go
			 * negative.
			 */
			r->collideSpeed = spFast * K_COLLIDE_PUSH - r->speed;
			if (r->collideSpeed < 0.0f)
				r->collideSpeed = 0.0f;
			r->riderStateBits |= 4;
		} else {
			race_norm3(n, v);
			for (i = 0; i < 3; i++)
				r->collideDir[i] = n[i];
			if (!hard) {
				r->riderStateBits |= 0x10;
			} else {
				racer_begin_crash(r, race_gearbox_of(r), nSlow);
				r->riderStateBits |= 0x20;
			}
			r->collideSpeed = spFast - spSlow;
			if (r->collideSpeed < K_COLLIDE_SLOW)
				r->collideSpeed = K_COLLIDE_SLOW;
			r->speed = 0.0f;
			physics_set_long_speed(r->matOrientation, r->velDir,
			                       &r->speed, r->realSpeedMax,
			                       0.0f);
			physics_sync_road_speed(&r->gearboxRoadSpeed,
			                        r->speed, r->realSpeedMax);
			r->engineSpeed = physics_speed_to_engine(c, r->gear,
			                     &r->gearboxRoadSpeed,
			                     r->engineSpeed,
			                     r->driveDisengaged);
		}
		return;
	}

	/*
	 * THE AI PUTS THE COMBINED DIRECTION IN moveDir, not collideDir,
	 * and normalises it in place.
	 */
	race_norm3(n, v);
	for (i = 0; i < 3; i++)
		r->moveDir[i] = n[i];

	if (!headOn) {
		if (!crash) {
			r->riderStateBits |= 4;
		} else {
			racer_begin_crash(r, race_gearbox_of(r), r->moveDir);
			r->riderStateBits |= 8;
		}
		r->speed = spFast;
	} else {
		if (!hard) {
			r->riderStateBits |= 0x10;
		} else {
			racer_begin_crash(r, race_gearbox_of(r), nSlow);
			r->riderStateBits |= 0x20;
		}
		r->speed = spFast - spSlow;
	}
	r->speed *= K_COLLIDE_PUSH;
	if (r->speed < K_COLLIDE_SLOW)
		r->speed = K_COLLIDE_SLOW;
	r->engineSpeed = r->speed;
	r->aiRoadSpeed = r->speed;
}


/*
 * ORIGIN  maybe_Game_CollideRacers @ 0x0047ecd0
 *
 * RACER AGAINST RACER, once per frame. Vtable slot +0x18 of the game
 * object, which is why nothing in the racer tick leads to it.
 *
 * *** IT DOES NOTHING IN PRACTICE MODE. *** The whole body is under
 * `if (g_nSelectedMode != 0)`, so bikes pass through each other in
 * mode 0 and collide in a single race and a championship. That is the
 * original's behaviour and it is the first thing to check when a test
 * shows no collision at all.
 *
 * Three gates, cheapest first: both active, within FOUR SEGMENTS
 * forward-wrapping, then a swept sphere test at radius 48 each with
 * the contact time bounded to 0.05 .. 1.0.
 *
 * *** A QUALIFYING IMPACT ONLY CRASHES ONE TIME IN FOUR. ***
 * Game_RandNext(0x2b) & 3 must come out zero or the crash is demoted
 * to a shove. A test that produces one hit and sees no crash has
 * proved nothing.
 *
 * NOT REPRODUCED, and both are network-only:
 *
 *   o the local-player test. The original picks its branch from
 *     f_2dcc against the player id at DAT_0050dbdc + 0x400 in a
 *     network game; solo picks on humanPlayer, which is what this
 *     does. [open]
 *   o FUN_00456bf0, the collision message sent to the other machine.
 *
 * THE RNG IS OURS. Game_RandNext takes a call-site tag and keeps its
 * own sequence; rand() gives the same one-in-four rate against a
 * different stream, so WHICH impacts crash will differ from the
 * original even though how often will not. [open - Game_RandNext]
 */
void race_collide_racers(MR_Racer *racers, int count, float dtScaled)
{
	unsigned char paired[RACE_COLLIDE_MAX][RACE_COLLIDE_MAX];
	int i, j, k;

	if (!racers || dtScaled <= 0.0f)
		return;
	/*
	 * [DEBUG] collideinpractice=1 LIFTS THE PRACTICE GATE, FOR TESTING
	 * ONLY. Off by default, named in the ini, and it changes nothing
	 * when unset - it is the only way to exercise this code without
	 * driving a whole single race first.
	 */
	if (g_selectedMode == 0 && !s_race_collideInPractice)
		return;
	if (count > RACE_COLLIDE_MAX)
		count = RACE_COLLIDE_MAX;

	memset(paired, 0, sizeof paired);

	for (i = 1; i < count; i++) {
		MR_Racer *ra = &racers[i];

		if (!ra->active)
			continue;

		for (j = i + 1; j < count; j++) {
			MR_Racer *rb = &racers[j];
			MR_Racer *fast, *slow;
			float dA[3], dB[3], nA[3], nB[3];
			float nFast[3], nSlow[3];
			float spA, spB, spFast, spSlow, dv, t;
			int   crash, hard, headOn;

			if (!rb->active)
				continue;
			if (paired[i][j] || paired[j][i])
				continue;
			if (race_segment_delta(ra->segmentStart,
			                       rb->segmentStart) >= 4)
			    continue;

			if (!race_sweep_spheres(ra->positionPrev,
			                        rb->positionPrev,
			                        ra->position, rb->position,
			                        K_COLLIDE_RADIUS,
			                        K_COLLIDE_RADIUS, &t))
			    continue;
			/* t <= 1.0f and t >= 0.05f, both float-bits
			 * compares in the original. */
			if (t > 1.0f || t < K_COLLIDE_T_MIN)
				continue;

			paired[i][j] = 1;
			paired[j][i] = 1;

			for (k = 0; k < 3; k++) {
				dA[k] = ra->position[k] - ra->positionPrev[k];
				dB[k] = rb->position[k] - rb->positionPrev[k];
			}
			spA = (race_norm3(nA, dA) / dtScaled)
			    * K_COLLIDE_SPEED;
			spB = (race_norm3(nB, dB) / dtScaled)
			    * K_COLLIDE_SPEED;

			/* The faster of the pair is handled first. */
			if (spA <= spB) {
				fast = rb; slow = ra;
				spFast = spB; spSlow = spA;
				for (k = 0; k < 3; k++) {
					nFast[k] = nB[k];
					nSlow[k] = nA[k];
				}
			} else {
				fast = ra; slow = rb;
				spFast = spA; spSlow = spB;
				for (k = 0; k < 3; k++) {
					nFast[k] = nA[k];
					nSlow[k] = nB[k];
				}
			}
			dv = (float)fabs((double)(spFast - spSlow));

			crash = (dv > K_COLLIDE_DV_HARD)
			     || (dv > K_COLLIDE_DV_SOFT
			         && (float)fabs((double)fast->lean)
			            > K_COLLIDE_LEAN)
			     || (dv > K_COLLIDE_DV_SOFT
			         && (float)fabs((double)slow->lean)
			            > K_COLLIDE_LEAN)
			     || fast->wheelieAngle != 0.0f
			     || slow->wheelieAngle != 0.0f;

			hard = (spFast > K_COLLIDE_FAST)
			    || (spSlow > K_COLLIDE_FAST)
			    || fast->wheelieAngle != 0.0f
			    || slow->wheelieAngle != 0.0f;

			/*
			 * AIRBORNE PLUS A HUMAN FORCES BOTH. Landing on
			 * somebody always crashes, and it is the only place
			 * the two flags are set together.
			 */
			if ((!fast->onGround || !slow->onGround)
				&& (fast->humanPlayer || slow->humanPlayer)) {
				crash = 1;
				hard  = 1;
			}

			if ((rand() & 3) != 0)
				crash = 0;

			/*
			 * HEAD ON IS THE TWO TRAVEL DIRECTIONS, not the
			 * contact normal - so two bikes converging on the
			 * same line still count as same-direction.
			 */
			headOn = (nFast[0] * nSlow[0] + nFast[1] * nSlow[1]
			          + nFast[2] * nSlow[2]) < 0.0f;

			/*
			 * ORDER DIVERGENCE, and it is safe. The original moves
			 * BOTH racers, then runs both outcome blocks; this does
			 * one racer end to end and then the other. Nothing in
			 * an outcome block reads the other racer's position,
			 * and the two travel directions were snapshotted above
			 * before either moved - which is the only thing the
			 * original's ordering was protecting.
			 */
			race_collide_separate(fast, nFast, t);
			race_clear_stunt(fast);
			race_collide_fast(fast, nFast, nSlow, spSlow,
			                  crash, hard, headOn);

			race_collide_separate(slow, nSlow, t);
			race_clear_stunt(slow);
			race_collide_slow(slow, nFast, nSlow, spFast, spSlow,
			                  crash, hard, headOn);

			/*
			 * BOTH RE-LEAN. The original passes no debug block, and
			 * racer_update_lean wants MILLISECONDS - dtScaled * 10,
			 * the same conversion race_tick_physics makes.
			 */
			racer_update_lean(fast, bike_at(fast->bikeIndex),
			                  &s_race_trk, fast->speed,
			                  dtScaled * RACE_DT_UNSCALE,
			                  race_replay_mode(), NULL);
			racer_update_lean(slow, bike_at(slow->bikeIndex),
			                  &s_race_trk, slow->speed,
			                  dtScaled * RACE_DT_UNSCALE,
			                  race_replay_mode(), NULL);
		}
	}
}


/*
 * THE VIEWED RACER'S AUDIO RECORD.
 *
 * ORIGIN  the standalone record at 0x005034b0. The original keeps
 *         four more at 0x005032c0 for the nearest OTHER racers,
 *         picked through the slot table at 0x00504210 - that
 *         selection is reconstructed, see RACE_AUDIO_SLOTS.
 *
 * THE PAN IS PROVISIONAL AND THE MASTER IS NOT. The master comes off
 * race_audio_master's distance ramp like everyone else's; the pan is
 * held centred because the viewed bike sits at the camera, and that
 * is said rather than given an invented falloff.
 *
 * A ZERO-FILLED RECORD NAMES VOICE 0, which is a real slot - so this
 * must go through racer_audio_init before anything reads it, and
 * racer_audio_reset will NOT do: it stops the voices the fields name,
 * and on a fresh static that is voice 0. The one-shot flag is what
 * makes the order safe whichever entry point runs first.
 */
static MR_RacerAudio s_race_viewedAudio;
static int           s_race_audioReady;

static MR_RacerAudio *race_audio_record(void)
{
	if (!s_race_audioReady) {
		racer_audio_init(&s_race_viewedAudio);
		s_race_audioReady = 1;
	}
	return &s_race_viewedAudio;
}
#define RACE_AUDIO_PAN    0.0f

/*
 * THE AUDIBILITY SELECTION, and it is what makes the other bikes
 * audible at all.
 *
 * ORIGIN  maybe_Snd_TickRace @ 0x004079e0
 *
 * FOUR slots out of the whole field, nearest first. A racer that
 * stays in earshot KEEPS ITS SLOT from frame to frame, and that is
 * the part that matters rather than the selection: the slot owns
 * the audio record, and the record owns the engine voice and the
 * gear it is playing. Rebuild the table from scratch each frame
 * and every audible bike restarts its loop every frame.
 */
#define RACE_AUDIO_SLOTS     4             /* 0x00504210       */
#define RACE_AUDIO_DIST_MAX  10240.0f      /* 0x004d0280       */
#define RACE_AUDIO_DIST_K    0.033203125f  /* 0x004d0284, 17/512 */
#define RACE_AUDIO_DOP_SCALE 0.01f         /* 0x004d0268       */
#define RACE_AUDIO_DOP_K     2.1701389e-05f /* 1/46080          */
#define RACE_AUDIO_PAN_DEAD  0.15f         /* 0x004d012c       */
#define RACE_AUDIO_PAN_K     75.294118f    /* 0x004d0138, 1280/17 */
#define RACE_AUDIO_PAN_FULL  64.0f         /* PAN_K * 0.85     */

typedef struct {
	int   used;    /* +0x00 */
	int   racer;   /* +0x04 */
	float dist;    /* +0x08 */
} MR_RaceAudioSlot;

static MR_RaceAudioSlot s_race_audioSlots[RACE_AUDIO_SLOTS];
static MR_RacerAudio    s_race_aiAudio[RACE_AUDIO_SLOTS];
static int              s_race_aiReady;
static float            s_race_audioPrevEye[3];
static int              s_race_audioEyePrimed;

/* Same trap as the viewed record - see race_audio_record. */
static MR_RacerAudio *race_ai_record(int slot)
{
	int i;

	if (!s_race_aiReady) {
		for (i = 0; i < RACE_AUDIO_SLOTS; i++)
			racer_audio_init(&s_race_aiAudio[i]);
		s_race_aiReady = 1;
	}
	return &s_race_aiAudio[slot];
}

/* ORIGIN  the ramp and clamp at 0x0040819a..0x004081e2. */
static int race_audio_master(float dist)
{
	int v = (int)((RACE_AUDIO_DIST_MAX - dist) * RACE_AUDIO_DIST_K);

	if (v > 255)
		v = 255;
	if (v < 0)
		v = 0;
	return v;
}

/*
 * ORIGIN  maybe_Snd_TickRace @ 0x004079e0, at 0x004081e4
 *
 * The 2D CROSS of the camera's forward with the direction to the
 * bike, which is the sine of the angle off the look axis - so it
 * is zero dead ahead and +/-1 out to the sides. Y is ignored.
 *
 * THE DEAD ZONE IS THE PART WORTH KEEPING. Anything inside 0.15
 * collapses to centre and everything past it is shifted IN by the
 * same 0.15, so a bike straight ahead cannot jitter between ears
 * and the ends are still reachable: 0.85 is what remains, and
 * 0.85 * 75.294 is exactly 64.
 */
static float race_audio_pan(const float pos[3], const float eye[3],
                            float fwdX, float fwdZ)
{
	float dx  = pos[0] - eye[0];
	float dz  = pos[2] - eye[2];
	float len = (float)sqrt(dx * dx + dz * dz);
	float p;

	if (len == 0.0f)
		return 0.0f;
	dx /= len;
	dz /= len;

	p = dx * fwdZ - dz * fwdX;
	if (p >= RACE_AUDIO_PAN_DEAD)
		p -= RACE_AUDIO_PAN_DEAD;
	else if (p <= -RACE_AUDIO_PAN_DEAD)
		p += RACE_AUDIO_PAN_DEAD;
	else
		p = 0.0f;

	/*
	 * The original hands the mixer an INTEGER -64..+64; ours
	 * takes -1..+1. Truncating first and scaling after keeps
	 * the original's resolution rather than inventing more.
	 */
	return (float)(int)(p * RACE_AUDIO_PAN_K) / RACE_AUDIO_PAN_FULL;
}

/*
 * ORIGIN  maybe_Snd_TickRace @ 0x004079e0, at 0x00408094
 *
 * TWO factors, one per moving end, and the sign is the whole
 * story: the bike closing on the camera raises its note, the
 * camera pulling away from the bike lowers it. The constant is
 * 1/46080 both times - positive against the source's velocity,
 * negative against the listener's.
 *
 * dt IS g_frameDeltaScaled (0x004e9808), which Race_TickAllRacers
 * scales and floors at 1.0 before anything reads it - the same
 * value race_frame_delta publishes. [inferred - the two globals
 * are distinct in the DB and carry the same step]
 */
static float race_audio_doppler(const float pos[3],
                                const float prevPos[3],
                                const float eye[3],
                                const float prevEye[3], float dt)
{
	float dx  = eye[0] - pos[0];
	float dy  = eye[1] - pos[1];
	float dz  = eye[2] - pos[2];
	float len = (float)sqrt(dx * dx + dy * dy + dz * dz);
	float k, src, cam;

	if (dt <= 0.0f)
		return 1.0f;
	if (len != 0.0f) {
		dx /= len;
		dy /= len;
		dz /= len;
	}
	k = 1.0f / (dt * RACE_AUDIO_DOP_SCALE);

	src = dx * k * (pos[0] - prevPos[0])
	    + dz * k * (pos[2] - prevPos[2])
	    + dy * k * (pos[1] - prevPos[1]);
	cam = dx * k * (eye[0] - prevEye[0])
	    + dz * k * (eye[2] - prevEye[2])
	    + dy * k * (eye[1] - prevEye[1]);

	return (src *  RACE_AUDIO_DOP_K + 1.0f) *
	       (cam * -RACE_AUDIO_DOP_K + 1.0f);
}

/* ORIGIN  maybe_Snd_TickRace @ 0x004079e0 */
void race_audio_tick(void)
{
	const MR_Racer     *v = race_viewed_racer();
	const MR_BikeCarac *c;
	const float        *eye = s_race_cam.eye;
	float               prevEye[3];
	float               fwdX, fwdZ, len, dt;
	int                 viewed, i, j, n, slot;
	struct { int racer; float dist; } cand[RACER_MAX];

	if (!v || !v->active) {
		racer_audio_reset(race_audio_record());
		for (i = 0; i < RACE_AUDIO_SLOTS; i++) {
			racer_audio_reset(race_ai_record(i));
			s_race_audioSlots[i].used = 0;
		}
		s_race_audioEyePrimed = 0;
		return;
	}

	viewed = (int)(v - s_race_racers);
	dt     = race_frame_delta();

	/*
	 * THE EYE'S OWN VELOCITY needs last frame's eye, and on the
	 * first frame there is no sensible one - the original seeds
	 * it from the camera record before the loop for exactly this
	 * reason, so a cut does not arrive as a huge Doppler spike.
	 */
	if (!s_race_audioEyePrimed) {
		s_race_audioPrevEye[0] = eye[0];
		s_race_audioPrevEye[1] = eye[1];
		s_race_audioPrevEye[2] = eye[2];
		s_race_audioEyePrimed  = 1;
	}
	prevEye[0] = s_race_audioPrevEye[0];
	prevEye[1] = s_race_audioPrevEye[1];
	prevEye[2] = s_race_audioPrevEye[2];

	/*
	 * THE DISTANCE IS FROM THE CAMERA EYE, not from the bike
	 * being watched - so a trackside shot makes the field loud
	 * and the player quiet, which is the point of it.
	 */
	for (i = 1; i < s_race_racerCount; i++) {
		MR_Racer *r = &s_race_racers[i];
		float     dx, dy, dz;

		if (!r->active)
			continue;
		dx = r->position[0] - eye[0];
		dy = r->position[1] - eye[1];
		dz = r->position[2] - eye[2];
		r->sndDistToEye = (float)sqrt(dx * dx + dy * dy + dz * dz);
	}

	/* The camera's forward, flattened - pan only reads XZ. */
	fwdX = s_race_cam.target[0] - eye[0];
	fwdZ = s_race_cam.target[2] - eye[2];
	len  = (float)sqrt(fwdX * fwdX + fwdZ * fwdZ);
	if (len != 0.0f) {
		fwdX /= len;
		fwdZ /= len;
	}

	/* The viewed racer: pan 0 and pitch 1.0 literally - 9.8. */
	c = bike_at(v->bikeIndex);
	racer_audio_tick(race_audio_record(), v, c, &s_race_trk,
	         race_audio_master(v->sndDistToEye),
	         RACE_AUDIO_PAN, 1, 1.0f);

	/* Everyone else, nearest first, cut at 10240. */
	n = 0;
	for (i = 1; i < s_race_racerCount; i++) {
		if (i == viewed || !s_race_racers[i].active)
			continue;
		if (s_race_racers[i].sndDistToEye >= RACE_AUDIO_DIST_MAX)
			continue;
		cand[n].racer = i;
		cand[n].dist  = s_race_racers[i].sndDistToEye;
		n++;
	}
	for (i = 1; i < n; i++) {   /* insertion sort; n is tiny */
		int   ri = cand[i].racer;
		float di = cand[i].dist;

		j = i - 1;
		while (j >= 0 && cand[j].dist > di) {
			cand[j + 1] = cand[j];
			j--;
		}
		cand[j + 1].racer = ri;
		cand[j + 1].dist  = di;
	}
	if (n > RACE_AUDIO_SLOTS)
		n = RACE_AUDIO_SLOTS;

	/* Keep first: a racer already holding a slot stays in it. */
	for (i = 0; i < RACE_AUDIO_SLOTS; i++) {
		int keep = 0;

		if (!s_race_audioSlots[i].used)
			continue;
		for (j = 0; j < n; j++) {
			if (cand[j].racer == s_race_audioSlots[i].racer) {
				s_race_audioSlots[i].dist = cand[j].dist;
				cand[j].racer = -1;   /* claimed */
				keep = 1;
				break;
			}
		}
		if (!keep) {
			s_race_audioSlots[i].used = 0;
			racer_audio_reset(race_ai_record(i));
		}
	}

	/* Then hand the free slots to whoever is left. */
	for (j = 0; j < n; j++) {
		if (cand[j].racer < 0)
			continue;
		for (slot = 0; slot < RACE_AUDIO_SLOTS; slot++)
			if (!s_race_audioSlots[slot].used)
				break;
		if (slot == RACE_AUDIO_SLOTS)
			break;
		s_race_audioSlots[slot].used  = 1;
		s_race_audioSlots[slot].racer = cand[j].racer;
		s_race_audioSlots[slot].dist  = cand[j].dist;
		racer_audio_reset(race_ai_record(slot));
	}

	/* Each audible bike, with its own volume, pan and pitch. */
	for (i = 0; i < RACE_AUDIO_SLOTS; i++) {
		const MR_Racer *r;

		if (!s_race_audioSlots[i].used)
			continue;
		r = &s_race_racers[s_race_audioSlots[i].racer];
		c = bike_at(r->bikeIndex);
		racer_audio_tick(
		    race_ai_record(i), r, c, &s_race_trk,
		    race_audio_master(s_race_audioSlots[i].dist),
		    race_audio_pan(r->position, eye, fwdX, fwdZ),
		    0,   /* bPlayerVoices: never for an AI bike */
		    race_audio_doppler(r->position, r->positionPrev,
		                       eye, prevEye, dt));
	}

	/*
	 * THE TRACK'S OWN SOUNDS, off the SAME eye the racers use.
	 * Two distance models measured from two different points
	 * would put a gull and a bike at odds over which is nearer.
	 */
	objsnd_tick(&s_race_trk, eye, fwdX, fwdZ,
	            s_race_racers[s_race_localIndex].segmentStart);

	s_race_audioPrevEye[0] = eye[0];
	s_race_audioPrevEye[1] = eye[1];
	s_race_audioPrevEye[2] = eye[2];
}

/*
 * ANYTHING THAT CHANGES THE SAMPLE BANK MUST CALL THIS FIRST, and
 * must then RELOAD THE TRACK'S OBJECT SOUNDS afterwards. The two
 * halves are not optional and they are not interchangeable:
 * audio_bank_free drops the PCM under any voice still holding it,
 * and objsnd's samples live in that same table because
 * objsnd_load binds them through audio_bind_cue.
 *
 * race_load is the caller that has to obey this, and its comment
 * carries what getting the order wrong looks like.
 */
void race_audio_stop(void)
{
	racer_audio_reset(race_audio_record());
	objsnd_reset();
	/* AND THE ANNOUNCER, or the next race opens with the last
	   one's commentary still queued behind a 500 ms delay. */
	audio_cue_delayed_reset();
	s_race_wrongWay = 0;
	/* Leaving a race while paused would otherwise hold the next
	   bank's loops down from the moment they start. */
	audio_set_paused(0);
}

/* ORIGIN  Race_TickAllRacers @ 0x0047df40, the solo loop only */
/* ================================================================== */
/* THE TIME-ATTACK GHOST.                                             */
/* ================================================================== */

/*
 * ORIGIN  the record maybe_Ghost_RecordFrame writes, 0x34 bytes, read
 *         off its stores at 0x004191c0 and its reads in
 *         maybe_Ghost_PlaybackFrame at 0x004192a0.
 */
typedef struct {
	float time;         /* +0x00  the stream clock when taken     */
	float speed;        /* +0x04                                  */
	float pos[3];       /* +0x08                                  */
	float aim[3];       /* +0x14  matOrientation row2 + row3      */
	int   flags;        /* +0x20  stuntOrOverspeed                */
	int   event;        /* +0x24  ghostEvent - a crash            */
	float eventDir[3];  /* +0x28  the crash launch direction      */
} MR_GhostFrame;

typedef char MR_GhostFrame_size
        [(sizeof(MR_GhostFrame) == 0x34) ? 1 : -1];

/*
 * ORIGIN  the int[6] both ghost functions index, 0x18 bytes. The
 *         game holds TWO of them, adjacent: the one being played
 *         back at 0x0052c0d8 and the one being recorded at
 *         0x0052c0f0, loaded into ECX at 0x0047e031 and 0x0047e10d.
 *         So a time-attack lap races the PREVIOUS lap while taping
 *         the current one.
 */
typedef struct {
	int           count;      /* [0] frames written            */
	int           cursor;     /* [1] playback position         */
	int           capacity;   /* [2] the recorder's only bound */
	MR_GhostFrame *frames;    /* [3]                           */
	float         lapTime;    /* [4] set at save, not in tick  */
	float         clock;      /* [5] advanced by BOTH halves   */
} MR_GhostStream;

/*
 * 10000 IS THE ORIGINAL'S, read out of a ghost file MOTO.EXE wrote:
 * DATA\ile01\timatak__1.bin carries capacity 10000 in the
 * that Ghost_Save takes from stream[2]. The recorder simply stops
 * when it is full - count < capacity is its only bound - so this
 * also sets how long a lap can be taped, about five and a half
 * minutes at 30 fps.
 *
 * Still no writer found for 0x0052c0e0 itself, but the value is no
 * longer a guess. See docs/formats/timatak.txt 3.2.
 */
#define GHOST_MAX_FRAMES   10000

static MR_GhostFrame  s_ghostPlayFrames[GHOST_MAX_FRAMES];
static MR_GhostFrame  s_ghostRecFrames[GHOST_MAX_FRAMES];
static MR_GhostStream s_ghostPlay;
static MR_GhostStream s_ghostRec;

/* ORIGIN  maybe_Ghost_RecordFrame @ 0x004191c0 */
static void ghost_record_frame(MR_GhostStream *g, float dt, float speed,
                               const float pos[3], const float aim[3],
                               int flags, int event,
                               const float eventDir[3])
{
	MR_GhostFrame *f;
	int            i;

	if (!g || !g->frames || g->count >= g->capacity)
		return;

	g->clock += dt;

	f = &g->frames[g->count];
	f->time  = g->clock;
	f->speed = speed;
	for (i = 0; i < 3; i++) {
		f->pos[i]      = pos[i];
		f->aim[i]      = aim[i];
		f->eventDir[i] = eventDir[i];
	}
	f->flags = flags;
	f->event = event;
	g->count++;
}

/* ORIGIN  maybe_Ghost_PlaybackFrame @ 0x004192a0 */
static int ghost_playback_frame(MR_GhostStream *g, float dt,
                                float *outSpeed, float outPos[3],
                                float outAim[3], int *outFlags,
                                int *outEvent, float outEventDir[3])
{
	const MR_GhostFrame *a;
	const MR_GhostFrame *b;
	int   wasCursor;
	int   prev;
	int   advanced = 0;
	float t;
	float inv;
	int   i;

	if (!g || !g->frames || g->count == 0)
		return 0;

	*outEvent = 0;
	wasCursor = g->cursor;
	g->clock += dt;

	/* 0x004192e4  walk the cursor up to the clock. */
	while (g->cursor < g->count
	       && g->frames[g->cursor].time < g->clock) {
		advanced = 1;
		g->cursor++;
	}

	if (g->cursor >= g->count)
		return advanced;

	prev = g->cursor - 1;
	if (prev < 0)
		prev = 0;
	a = &g->frames[prev];
	b = &g->frames[g->cursor];

	/*
	 * 0x00419333  the fraction between the two records, clamped to
	 * 0..1. A zero span would divide by zero; the original does not
	 * guard it because the recorder only ever appends with a
	 * strictly advancing clock.
	 */
	t = (b->time != a->time)
	  ? (g->clock - a->time) / (b->time - a->time)
	  : 0.0f;
	if (t < 0.0f)
		t = 0.0f;
	if (t > 1.0f)
		t = 1.0f;
	inv = 1.0f - t;

	for (i = 0; i < 3; i++) {
		outPos[i] = a->pos[i] * inv + b->pos[i] * t;
		outAim[i] = a->aim[i] * inv + b->aim[i] * t;
	}
	*outSpeed = a->speed * inv + b->speed * t;

	/*
	 * 0x004193f5  THE FLAGS DO NOT LERP - they snap at the halfway
	 * point, taking whichever record the clock is nearer.
	 */
	*outFlags = (t < 0.5f) ? a->flags : b->flags;

	/*
	 * 0x00419420  AND NO EVENT IS MISSED. Every record the cursor
	 * stepped OVER is scanned, so a crash recorded between two
	 * sampled frames still fires.
	 */
	for (i = wasCursor; i <= g->cursor && i < g->count; i++) {
		if (g->frames[i].event != 0) {
			int k;

			*outEvent = 1;
			for (k = 0; k < 3; k++)
				outEventDir[k] = g->frames[i].eventDir[k];
		}
	}
	return advanced;
}

/*
 * ORIGIN  NONE - ours. The original's streams are statics whose
 *         array and capacity are set up somewhere this project has
 *         not read, so this does that job explicitly.
 *
 * BOTH ARMS CALL IT, because either can run first and a load into an
 * unwired stream would read zero frames without saying so.
 */
static void race_ghost_wire(void)
{
	if (s_ghostPlay.frames)
		return;

	s_ghostPlay.frames   = s_ghostPlayFrames;
	s_ghostPlay.capacity = GHOST_MAX_FRAMES;
	s_ghostRec.frames    = s_ghostRecFrames;
	s_ghostRec.capacity  = GHOST_MAX_FRAMES;
}


/* ORIGIN  Ghost_Empty @ 0x004191a0 - count, cursor and clock. */
static void ghost_empty(MR_GhostStream *g)
{
	g->count  = 0;
	g->cursor = 0;
	g->clock  = 0.0f;
}


/*
 * ORIGIN  Ghost_Rewind @ 0x004191b0 - cursor and clock ONLY.
 *
 * IT DOES NOT TOUCH count, and that is the whole mechanism by which
 * a taped lap survives a race load. See formats/timatak.txt 1.
 */
static void ghost_rewind(MR_GhostStream *g)
{
	g->cursor = 0;
	g->clock  = 0.0f;
}


/*
 * ORIGIN  Ghost_Rewind @ 0x004191b0 and Ghost_Empty @ 0x004191a0,
 *         both as maybe_Game_InitRace calls them at 0x0047c010.
 *         The wiring is ours: the original's streams are statics
 *         whose array and capacity are set up somewhere this
 *         project has not read.
 *
 *         THE PLAYBACK count IS NEVER RESET AT RACE LOAD. That is
 *         the mechanism by which a taped lap survives into the
 *         next race, so Ghost_Rewind touches cursor and clock only
 *         and Ghost_Empty is called on the recorder alone. Zeroing
 *         the playback count here leaves slot 2 active with no
 *         frames to play - race_build_grid activates it from
 *         race_ghost_has_lap before this runs.
 */
static void race_ghost_reset(void)
{
	race_ghost_wire();
	ghost_rewind(&s_ghostPlay);
	ghost_empty(&s_ghostRec);
}


/*
 * THE 0xf9 RECORD IS A BESTSCOR ENTRY - a name and the 48 checkpoint
 * splits - and the ghost file carries one because the ghost IS a
 * best-score entry with a lap attached. An empty record is all 0x7f.
 * See docs/formats/timatak.txt.
 */
#define GHOST_REC_BYTES    0xf9
#define GHOST_REC_SPLITS   0x0d
#define GHOST_REC_EMPTY    0x7f
#define GHOST_NAME         "SHADOW"

static unsigned char s_ghostEntry[GHOST_REC_BYTES];


/*
 * ORIGIN  Ghost_Promote @ 0x004190d0
 *
 * THE DECOMPILER RENDERS THE ARGUMENTS BACKWARDS - see the plate
 * comment. It is __thiscall, and the one call site at 0x00412d67
 * pushes the RECORD stream with ECX holding the PLAYBACK one, so the
 * lap just taped becomes the lap to beat.
 *
 * dst keeps its OWN frames array; only the records are copied. The
 * original bounds the copy by src->count alone, which is safe here
 * because both streams are GHOST_MAX_FRAMES.
 */
static void race_ghost_promote(MR_GhostStream *dst,
                               const MR_GhostStream *src)
{
	int i;

	for (i = 0; i < src->count; i++)
		dst->frames[i] = src->frames[i];

	dst->count    = src->count;
	dst->cursor   = 0;
	dst->capacity = src->capacity;
	dst->lapTime  = src->lapTime;
	dst->clock    = 0.0f;
}


/*
 * ORIGIN  the path Ghost_Save builds inline at 0x004138e3, and
 *         Ghost_Load identically at 0x004136c6
 *
 * THE POCKET-BIKE SUFFIX IS ABSENT ON PURPOSE. Both halves of the
 * original strcat "P" into the BESTSCOR KEY buffer at 0x0053f3c0
 * rather than into the path at 0x0053f678 - settled in the
 * instructions at 0x00413958, not inferred - so a pocket ghost is
 * one character shorter than a normal one. The two names still
 * differ, so the files do not collide, and reproducing it is what
 * keeps our filenames the same as retail's.
 */
static int race_ghost_path(char *out, int n)
{
	const MR_Track *t   = track_at(g_selectedTrack);
	const char     *dir = get_data_dir();
	char            leaf[64];
	char            rel[MAX_PATH];

	if (!t || !dir || !out || n <= 0)
		return 0;

	_snprintf(leaf, sizeof(leaf), "timatak%s%s%1d.bin",
	          g_reverseEnabled ? "R" : "_",
	          g_pocketEnabled  ? ""  : "_",
	          g_raceLaps);
	leaf[sizeof(leaf) - 1] = 0;

	/*
	 * THE ORIGINAL'S PREFIX IS EMPTY AND OURS CANNOT BE. g_szDataDir
	 * at 0x004e8380 is "", so retail builds "data\ileNN\timatak..."
	 * relative to the working directory and relies on being started
	 * in the install root. Our data directory is an ini setting
	 * resolved against the EXECUTABLE (config.c), so the bare
	 * relative path fopens nothing and the save silently failed.
	 * track_asset_name strips the track path's own "data\" prefix,
	 * which get_data_dir then supplies.
	 */
	if (!track_asset_name(t, leaf, rel, (int)sizeof(rel)))
		return 0;
	if ((int)(strlen(dir) + strlen(rel)) >= n)
		return 0;

	strcpy(out, dir);
	strcat(out, rel);
	return 1;
}


/* ORIGIN  FUN_00412a50 @ 0x00412d71, the record it hands Ghost_Save */
static void race_ghost_build_entry(const MR_Racer *r)
{
	int i;

	memset(s_ghostEntry, GHOST_REC_EMPTY, sizeof(s_ghostEntry));
	memcpy(s_ghostEntry, GHOST_NAME, sizeof(GHOST_NAME));

	for (i = 0; i < 48; i++) {
		memcpy(s_ghostEntry + GHOST_REC_SPLITS + i * 4,
		       &r->checkpointSplits[i], 4);
	}
}


/*
 * ORIGIN  Ghost_Save @ 0x004138c0
 *
 * NOT AN ASSET, SO NOT asset_load. This is a file the game WRITES
 * into the install beside the track data, which is the one case
 * rule 10 does not cover.
 */
static void race_ghost_save(void)
{
	char  path[MAX_PATH];
	FILE *f;
	int   i;

	if (!race_ghost_path(path, (int)sizeof(path)))
		return;

	f = fopen(path, "wb");
	if (!f) {
		mrlog_err("ghost: could not write %s\n", path);
		return;
	}

	/* The three header words the load compares before trusting it. */
	fwrite(&g_pocketEnabled,  4, 1, f);
	fwrite(&g_reverseEnabled, 4, 1, f);
	fwrite(&g_raceLaps,       4, 1, f);

	fwrite(s_ghostEntry, GHOST_REC_BYTES, 1, f);
	fwrite(&s_ghostPlay.count,    4, 1, f);
	fwrite(&s_ghostPlay.capacity, 4, 1, f);
	fwrite(&s_ghostPlay.lapTime,  4, 1, f);

	for (i = 0; i < s_ghostPlay.count; i++)
		fwrite(&s_ghostPlay.frames[i], sizeof(MR_GhostFrame), 1, f);

	fclose(f);
	mrlog("ghost: saved %d frames to %s\n", s_ghostPlay.count, path);
}


/*
 * ORIGIN  Ghost_Load @ 0x004136a0
 *
 * ANY HEADER MISMATCH ABANDONS THE LOAD and leaves count at 0, which
 * is how a ghost taped over three laps is not offered to a five-lap
 * race. The original leaves the stream empty rather than clearing it
 * again - maybe_Game_LoadRace has already called Ghost_Empty.
 *
 * THE FILE'S CAPACITY IS DISCARDED. The original freads it over
 * stream[2], and that word is the recorder's only bound, so trusting
 * a file would let it describe a buffer bigger than the one we have.
 */
static void race_ghost_load(void)
{
	char  path[MAX_PATH];
	FILE *f;
	int   word;
	int   count;
	int   capacity;
	int   i;

	if (!race_ghost_path(path, (int)sizeof(path)))
		return;

	f = fopen(path, "rb");
	if (!f)
		return;

	if (fread(&word, 4, 1, f) != 1 || word != g_pocketEnabled)
		goto done;
	if (fread(&word, 4, 1, f) != 1 || word != g_reverseEnabled)
		goto done;
	if (fread(&word, 4, 1, f) != 1 || word != g_raceLaps)
		goto done;

	if (fread(s_ghostEntry, GHOST_REC_BYTES, 1, f) != 1)
		goto done;

	s_ghostPlay.cursor = 0;
	s_ghostPlay.clock  = 0.0f;

	if (fread(&count, 4, 1, f) != 1)
		goto done;
	if (fread(&capacity, 4, 1, f) != 1)
		goto done;
	if (fread(&s_ghostPlay.lapTime, 4, 1, f) != 1)
		goto done;

	(void)capacity;
	if (count < 0)
		count = 0;
	if (count > s_ghostPlay.capacity)
		count = s_ghostPlay.capacity;

	for (i = 0; i < count; i++) {
		if (fread(&s_ghostPlay.frames[i], sizeof(MR_GhostFrame),
		          1, f) != 1) {
			count = i;
			break;
		}
	}
	s_ghostPlay.count = count;

	mrlog("ghost: loaded %d frames from %s\n", count, path);
done:
	fclose(f);
}


/*
 * ORIGIN  maybe_Game_LoadRace @ 0x0047b420, its ghost arm at
 *         0x0047b42f
 *
 * THE PLAYBACK STREAM IS EMPTIED BEFORE THE LOAD, and before time
 * attack is tested at all. Two things depend on that: turning time
 * attack off leaves no ghost on the grid, and RESET TIME ATTACK
 * having deleted the file cannot leave the old lap live in memory.
 * race_ghost_reset only REWINDS, which is InitRace's job - the two
 * run back to back in race_load because it does both jobs.
 */
static void race_ghost_load_race(void)
{
	race_ghost_wire();
	ghost_empty(&s_ghostPlay);
	if (g_timeAttack != 0 && g_selectedMode == 0)
		race_ghost_load();
	ghost_empty(&s_ghostRec);
}


/*
 * ORIGIN  MenuPage09_Results_Tick @ 0x0044d540, its reset arm - the
 *         path builder at 0x0044e305 and crt_remove at 0x0044e3db
 *
 * RESET TIME ATTACK DELETES THE FILE, nothing more. The original
 * builds the same path Ghost_Save writes, calls remove() on it and
 * goes to page 6; it does not touch the in-memory stream, because
 * the next maybe_Game_LoadRace empties it and then fails to reload
 * it. Ours empties it here too, so the button takes effect without
 * depending on that.
 */
void race_ghost_erase(void)
{
	char path[MAX_PATH];

	if (!race_ghost_path(path, (int)sizeof(path)))
		return;

	if (remove(path) == 0)
		mrlog("ghost: erased %s\n", path);
	else
		mrlog("ghost: nothing to erase at %s\n", path);

	ghost_empty(&s_ghostPlay);
}


/*
 * ORIGIN  FUN_00412a50 @ 0x00412d61, the time-attack half of its
 *         practice arm
 *
 * PROMOTION HAPPENS ONCE, AT RACE END - not per lap. The gate is the
 * local racer's finishPosition, so an abandoned run leaves the saved
 * ghost alone.
 */
void race_ghost_race_done(const MR_Racer *r)
{
	if (!r)
		return;

	/*
	 * finishPosition IS ZERO-BASED AND THE GHOST IS A RACER, so in
	 * time attack this says YOU BEAT THE GHOST. Losing to it leaves
	 * the saved lap alone, which is how the file only ever holds the
	 * best run.
	 *
	 * SAYS WHY WHEN IT DOES NOT TAPE. A missing file is otherwise
	 * indistinguishable from a bug, and these two returns are the
	 * only ways to reach one.
	 */
	if (r->finishPosition != 0 || s_ghostRec.count == 0) {
		mrlog("ghost: lap not taped - finished %d, %d frames\n",
		      r->finishPosition + 1, s_ghostRec.count);
		return;
	}

	s_ghostRec.lapTime = (float)r->raceClockMs;
	race_ghost_promote(&s_ghostPlay, &s_ghostRec);
	race_ghost_build_entry(r);
	race_ghost_save();
}

int race_ghost_has_lap(void)
{
	return s_ghostPlay.count > 0;
}

/*
 * ORIGIN  Race_TickAllRacers @ 0x0047df40, its time-attack arm at
 *         0x0047dfc0
 *
 * TIME ATTACK IS A DIFFERENT TICK, NOT AN EXTRA RACER. It drives the
 * player, services the ghost in slot 2 and RETURNS - the main loop
 * below never runs, so no AI is ticked at all.
 */
static void race_tick_time_attack(MR_Racer *racers, int count,
                                  float dtScaled)
{
	MR_Racer *me  = &racers[1];
	MR_Racer *gst = (count > 2) ? &racers[2] : NULL;
	float     aim[3];
	int       i;

	racer_tick_dispatch(me, me->inputWord | me->inputWord2,
	                    me->axis0, me->axis1, dtScaled);

	if (gst && gst->active) {
		float wasTarget[3];
		float eventDir[3];
		int   event = 0;
		int   flags = 0;

		/*
		 * THE PREVIOUS TARGET IS KEPT, exactly as the network
		 * receiver keeps it: placeWheels1 is the last
		 * placeWheels0, and racer_remote_apply_state takes
		 * moveDir from the pair. 0x0047dff9.
		 */
		for (i = 0; i < 3; i++)
			wasTarget[i] = gst->placeWheels0[i];

		if (ghost_playback_frame(&s_ghostPlay, dtScaled,
		                         &gst->speed, gst->placeWheels0,
		                         gst->aimDir, &flags,
		                         &event, eventDir)) {
			/* nothing - the return says only that the cursor
			 * moved, and the original ignores it too. */
		}
		gst->stuntOrOverspeed = flags;

		for (i = 0; i < 3; i++)
			gst->placeWheels1[i] = wasTarget[i];
		gst->engineSpeed = gst->speed;

		if (event)
			(void)racer_begin_crash(gst, NULL, eventDir);

		{
			MR_AiEnv env;
			const MR_BikeCarac *bc = bike_at(gst->bikeIndex);

			memset(&env, 0, sizeof env);
			env.preRace       = race_pre_race();
			env.viewedSegment = me->segmentStart;
			env.viewedIndex   = 1;
			env.dtMs          = dtScaled * RACE_DT_UNSCALE;
			env.raceDir       = g_reverseEnabled ? -1 : 1;
			env.racers        = racers;
			env.racerCount    = count;
			env.selectedMode  = g_selectedMode;

			if (bc)
				(void)racer_tick_remote(gst, bc, &s_race_trk,
			                            &env, dtScaled);
		}
	}

	/*
	 * AND THE CURRENT LAP IS TAPED. aim is matOrientation row 2
	 * plus row 3, which is the same aim vector the network packet
	 * carries - 0x0047e0f0.
	 */
	for (i = 0; i < 3; i++)
		aim[i] = me->matOrientation[6 + i] + me->matOrientation[9 + i];

	ghost_record_frame(&s_ghostRec, dtScaled, me->speed, me->position,
	                   aim, me->stuntOrOverspeed, me->ghostEvent,
	                   me->f_31a1);

	/* A SEND-ONCE LATCH, cleared after taping it - the same shape
	 * the network builder uses for the same field. 0x0047e135. */
	me->ghostEvent = 0;
}

void race_tick_all(MR_Racer *racers, int count, float dtScaled)
{
	int i;

	/*
	 * 0x0047dfc0  TIME ATTACK TAKES THE WHOLE TICK. No AI runs at
	 * all - the loop below is not entered.
	 */
	if (g_soloMode != 0 && g_selectedMode == 0 && g_timeAttack != 0) {
		race_tick_time_attack(racers, count, dtScaled);
		return;
	}

	/*
	 * SLOT 0 IS UNUSED and the loop starts at 1 - g_racers is
	 * 0x00581218 while the local player is index 1 at 0x00584421.
	 * racer.h keeps that indexing rather than packing from zero so
	 * ours and theirs line up.
	 */
	for (i = 1; i < count; i++) {
		MR_Racer *r = &racers[i];

		if (!r->active)
			continue;

		/*
		 * A SLOT WE DO NOT OWN IS REMOTE. The original asks
		 * Net_FindPlayerSlot and sends those to
		 * maybe_Racer_TickRemote rather than to the AI; with a
		 * relay the answer is simply "not our peer id". In solo
		 * every active slot dispatches as before.
		 */
		if (!g_soloMode && i != s_race_localIndex) {
			MR_AiEnv            env;
			const MR_BikeCarac *bc;

			race_net_drain_slot(i);
			bc = bike_at(r->bikeIndex);
			memset(&env, 0, sizeof env);
			env.preRace       = race_pre_race();
			env.viewedSegment =
			    racers[s_race_localIndex].segmentStart;
			env.viewedIndex   = s_race_localIndex;
			env.dtMs          = dtScaled * RACE_DT_UNSCALE;
			env.raceDir       = g_reverseEnabled ? -1 : 1;
			env.racers        = racers;
			env.racerCount    = count;
			env.selectedMode  = g_selectedMode;
			if (bc)
				(void)racer_tick_remote(r, bc, &s_race_trk, &env,
			                            dtScaled);
			continue;
		}

		racer_tick_dispatch(r, r->inputWord | r->inputWord2,
		                    r->axis0, r->axis1, dtScaled);
	}
}


/*
 * ORIGIN  NONE - ours. The original's DAT_004ea510, raised on every
 *         reset path and consumed once by Camera_UpdateTrackside at
 *         0x0046edb9, which then re-arms its own 600-tick timer and
 *         clears this. A one-shot signal, not a state flag.
 *
 *         The DB calls it g_nCrashResetSignal - Ghidra's naming
 *         check requires a Hungarian prefix on globals and ours does
 *         not. Same datum, two conventions.
 */
int g_crashResetSignal;

/* ORIGIN  FUN_004681d0 @ 0x004681d0, the gate and the cascade */
int race_reset_crashes(int index, int viewedIndex)
{
	MR_Racer *r;
	int       n;
	int       i;

	if (index < 0 || index >= s_race_racerCount)
		return 0;
	if (viewedIndex < 0 || viewedIndex >= s_race_racerCount)
		return 0;

	/*
	 * THE GATE - see race.h. Anyone but the viewed racer has to wait
	 * for the viewed racer to be up.
	 */
	if (index != viewedIndex
	    && s_race_racers[viewedIndex].crashLatch != 0)
	    return 0;

	r = &s_race_racers[index];

	/*
	 * Only racer 1 is simulated here, so only it has a drivetrain, a
	 * gearbox and a bike record to resync. Everyone else takes the AI
	 * path, which needs none of them - the same NULLs the header
	 * describes.
	 */
	if (index == 1) {
		n = racer_reset_crash(r, &s_race_drive, &s_race_gearbox,
		                      bike_at(r->bikeIndex));
	} else {
		n = racer_reset_crash(r, NULL, NULL, NULL);
	}

	g_crashResetSignal = 1;

	if (index != viewedIndex)
		return n;

	/* The cascade. Starts at 1 - racer 0 is never visited. */
	for (i = 1; i < s_race_racerCount; i++) {
		if (i == index)
			continue;
		if (s_race_racers[i].active == 0)
			continue;
		if (s_race_racers[i].crashLatch == 0)
			continue;
		n += race_reset_crashes(i, viewedIndex);
	}

	return n;
}


/* ================================================================== */
/* THE NETWORK SIDE OF THE RACE.                                      */
/*                                                                    */
/* netplay.c decodes and validates; this applies. The division is the */
/* original's too - case 0x66 of FUN_00455280 writes racer fields and */
/* maybe_Racer_TickRemote integrates them, which is why onState here  */
/* only stores and racer_remote_apply_state does the moving.          */
/* ================================================================== */

/*
 * 2048.0, [0x004d0d70]. Past this the dead reckoning is abandoned and
 * the bike is snapped - see race_net_on_state.
 */
#define RACE_NET_JUMP_DIST  2048.0f

static unsigned int s_race_netTick;

/*
 * ONE STATE PER SLOT PER FRAME. Measured through the agent pipe:
 * the arrivals come in PAIRS - two dispatched in one frame, then
 * two frames with none - so applying each as it lands advances a
 * peer two steps and then freezes it, which reads as a strobe.
 * The mode-0 arm of racer_remote_apply_state assigns the position
 * outright, so there is no smoothing downstream to hide it.
 * netplay-relay.txt 3.2.
 */
#define RACE_NET_QUEUE    8
#define RACE_NET_OVER_GRACE  15.0   /* seconds, tm_now */
#define RACE_NET_CATCHUP  4   /* deeper than this, take two */

static MR_NetMsgState s_race_netQ[NET_PEER_MAX][RACE_NET_QUEUE];
static int            s_race_netQHead[NET_PEER_MAX];
static int            s_race_netQCount[NET_PEER_MAX];
static unsigned int   s_race_netQTick[NET_PEER_MAX];
static double         s_race_netOverAt;

/* ORIGIN  NONE - ours. The send half of netplay.txt 3. */
static int race_net_build_state(int slot, MR_NetMsgState *out)
{
	const MR_Racer *r;

	if (slot < 1 || slot >= s_race_racerCount)
		return 0;
	r = &s_race_racers[slot];

	memset(out, 0, sizeof(*out));
	out->slot    = slot;
	out->speed   = r->speed;
	/* NOT r->aimDir: stale on a human racer - netplay-relay.txt 3.1. */
	racer_heading(r, out->aimDir);
	out->position[0] = r->position[0];
	out->position[1] = r->position[1];
	out->position[2] = r->position[2];
	out->raceClockMs = r->raceClockMs;
	out->bestLapMs   = r->bestLapMs;

	/*
	 * MASKED TO THE ORIGINAL'S FIELD WIDTHS even though we send ints.
	 * The flag word gave these 2, 1, 4, 3, 5 and 8 bits (netplay.txt 4)
	 * and the gameplay code is written for those ranges; sending wider
	 * would let one side act on a value the other cannot hold.
	 */
	out->raceState        = r->raceState & 3;
	out->stuntOrOverspeed = r->stuntOrOverspeed ? 1 : 0;
	out->lapCounter       = r->lapCounter & 0xf;
	out->checkpointIndex  = r->checkpointIndex & 7;
	out->finishPosition   = r->finishPosition & 0x1f;
	out->tickInputWord    = r->tickInputWord & 0xff;
	return 1;
}

/*
 * ORIGIN  NONE - ours
 *
 * A FULL QUEUE DROPS THE OLDEST, not the newest. A queue that
 * fills means this machine is behind, and the stale end of it is
 * what nobody wants to watch.
 */
static void race_net_on_state(const MR_NetMsgState *st)
{
	int slot = st->slot;
	int tail;

	if (slot < 1 || slot >= NET_PEER_MAX)
		return;

	/* In-order transport, so a tick we have seen is a duplicate. */
	if (s_race_netQTick[slot] != 0 && st->tick <= s_race_netQTick[slot])
		return;
	s_race_netQTick[slot] = st->tick;

	if (s_race_netQCount[slot] == RACE_NET_QUEUE) {
		s_race_netQHead[slot] =
		    (s_race_netQHead[slot] + 1) % RACE_NET_QUEUE;
		s_race_netQCount[slot]--;
	}
	tail = (s_race_netQHead[slot] + s_race_netQCount[slot])
	       % RACE_NET_QUEUE;
	s_race_netQ[slot][tail] = *st;
	s_race_netQCount[slot]++;
}


/*
 * ORIGIN  NONE - ours. Per race, from race_load.
 *
 * s_race_overNet LATCHES THE RESULTS BANNER - race_update re-arms
 * s_race_overHud from it every frame, so clearing the HUD flag
 * alone leaves "1ST POSITION" up for the session.
 *
 * s_race_netTick IS NOT RESET: it is our sequence number and a
 * peer drops anything it has seen, so restarting it stalls that
 * peer until it climbs past the old high water mark.
 */
static void race_net_reset(void)
{
	memset(s_race_netQHead,  0, sizeof s_race_netQHead);
	memset(s_race_netQCount, 0, sizeof s_race_netQCount);
	memset(s_race_netQTick,  0, sizeof s_race_netQTick);
	s_race_netOverAt = 0.0;
	s_race_overNet   = 0;
}


/* ORIGIN  NONE - ours. One queued state, two if we are behind. */
static void race_net_drain_slot(int slot)
{
	int take;

	if (slot < 1 || slot >= NET_PEER_MAX)
		return;
	take = (s_race_netQCount[slot] > RACE_NET_CATCHUP) ? 2 : 1;
	while (take-- > 0 && s_race_netQCount[slot] > 0) {
		race_net_apply_state(&s_race_netQ[slot][s_race_netQHead[slot]]);
		s_race_netQHead[slot] =
		    (s_race_netQHead[slot] + 1) % RACE_NET_QUEUE;
		s_race_netQCount[slot]--;
	}
}


/*
 * ORIGIN  NONE - ours. The original posts a time and waits for a
 *         peer to broadcast 0x6c; nothing in the reconstruction
 *         sent it, so a net race never reached the results page.
 *
 * THE HOST DECIDES, once. Everyone home ends it immediately; one
 * still riding gets RACE_NET_OVER_GRACE from the FIRST finish and
 * is then brought in, so a rider who never finishes cannot leave
 * the field on the track for ever.
 */
static void race_net_check_over(void)
{
	int    riding = 0;
	int    home   = 0;
	double now;
	int    i;

	if (g_soloMode || s_race_overNet || !netplay_is_host())
		return;

	for (i = 1; i < s_race_racerCount; i++) {
		if (!s_race_racers[i].active)
			continue;
		if (s_race_racers[i].raceState == RACESTATE_FINISHED)
			home++;
		else
			riding++;
	}

	if (home == 0) {
		s_race_netOverAt = 0.0;
		return;
	}
	if (riding > 0) {
		now = tm_now();
		if (s_race_netOverAt == 0.0) {
			s_race_netOverAt = now;
			return;
		}
		if (now - s_race_netOverAt < RACE_NET_OVER_GRACE)
			return;
	}

	mrlog("net: race over, %d home %d riding\n", home, riding);
	netplay_send_race_over();
	s_race_overNet = 1;
}


/* ORIGIN  NONE - ours. Call once a tick while in a net race. */
void race_net_send_state(void)
{
	MR_NetMsgState st;

	if (g_soloMode != 0 || !race_ready())
		return;
	if (!race_net_build_state(s_race_localIndex, &st))
		return;

	st.tick = ++s_race_netTick;
	netplay_send_state(&st);
}

/*
 * ORIGIN  maybe_Net_Dispatch case 0x66 	 0x004553a0 - the STORE half
 * only; the integration is racer_remote_apply_state.
 */
static void race_net_apply_state(const MR_NetMsgState *st)
{
	MR_Racer *r;
	float     d[3];
	float     dist2;

	if (!race_ready() || st->slot < 1 || st->slot >= s_race_racerCount)
		return;
	r = &s_race_racers[st->slot];

	r->active = 1;

	/*
	 * ONE FLOAT FEEDS BOTH SPEEDS, so a remote racer's engine speed IS
	 * its road speed and no drivetrain runs for it [verified,
	 * netplay.txt 3].
	 */
	r->speed       = st->speed;
	r->engineSpeed = st->speed;

	r->aimDir[0] = st->aimDir[0];
	r->aimDir[1] = st->aimDir[1];
	r->aimDir[2] = st->aimDir[2];

	/* The old target becomes the previous one, then the new arrives. */
	r->placeWheels1[0] = r->placeWheels0[0];
	r->placeWheels1[1] = r->placeWheels0[1];
	r->placeWheels1[2] = r->placeWheels0[2];
	r->placeWheels0[0] = st->position[0];
	r->placeWheels0[1] = st->position[1];
	r->placeWheels0[2] = st->position[2];

	/*
	 * TOO FAR TO RECKON, SO SNAP. Past RACE_NET_JUMP_DIST the chase
	 * would take visibly long and run through whatever is between, so
	 * the target goes into placeWheels2 and the chase restarts from
	 * where the bike actually is.
	 */
	d[0]  = r->position[0] - st->position[0];
	d[1]  = r->position[1] - st->position[1];
	d[2]  = r->position[2] - st->position[2];
	dist2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
	if (dist2 > RACE_NET_JUMP_DIST * RACE_NET_JUMP_DIST) {
		r->placeWheels2[0] = st->position[0];
		r->placeWheels2[1] = st->position[1];
		r->placeWheels2[2] = st->position[2];
		r->placeWheels1[0] = r->position[0];
		r->placeWheels1[1] = r->position[1];
		r->placeWheels1[2] = r->position[2];
	}

	r->raceClockMs      = st->raceClockMs;
	r->bestLapMs        = st->bestLapMs;
	r->raceState        = st->raceState & 3;
	r->stuntOrOverspeed = st->stuntOrOverspeed ? 1 : 0;
	r->lapCounter       = (short)(st->lapCounter & 0xf);
	r->checkpointIndex  = (short)(st->checkpointIndex & 7);
	r->finishPosition   = st->finishPosition & 0x1f;
	r->tickInputWord    = st->tickInputWord & 0xff;

	if (st->crash)
		racer_begin_crash(r, &s_race_gearbox, st->crashDir);

	/*
	 * THE HOST REBUILDS AND FORWARDS - it does not pass these bytes on.
	 * What the other clients receive is the host's own view of this
	 * racer, which is what makes every client agree; netplay.txt 3.1
	 * and netplay-relay.txt 5.4. Simplifying this to a byte forward is
	 * the one change here that would silently break agreement.
	 */
	if (netplay_is_host()) {
		MR_NetMsgState out;

		if (race_net_build_state(st->slot, &out)) {
			out.tick  = st->tick;
			out.crash = st->crash;
			out.crashDir[0] = st->crashDir[0];
			out.crashDir[1] = st->crashDir[1];
			out.crashDir[2] = st->crashDir[2];
			netplay_send_state(&out);
		}
	}
}

/* ORIGIN  maybe_Net_Dispatch case 0x6e 	 0x004556f8 - netplay.txt 6 */
static void race_net_on_collision(const MR_NetMsgCollision *c)
{
	MR_Racer *r;

	if (!race_ready() || c->slot < 1 || c->slot >= s_race_racerCount)
		return;
	r = &s_race_racers[c->slot];

	/*
	 * [open] THE TWO ARMS ARE NOT RECONSTRUCTED. The original's blend
	 * arm rebuilds a speed from the distance travelled and re-syncs the
	 * gearbox; the direct arm assigns the vector outright. Only the
	 * crash half is wired here, so a net collision shoves nothing yet.
	 * netplay.txt 6.
	 */
	if (c->crash)
		racer_begin_crash(r, &s_race_gearbox, c->crashDir);
}

/* ORIGIN  maybe_Net_Dispatch case 0xc9 	 0x00455b00 */
static void race_net_on_place(const MR_NetMsgPlace *p)
{
	MR_Racer *r;

	if (!race_ready() || p->slot < 1 || p->slot >= s_race_racerCount)
		return;
	r = &s_race_racers[p->slot];

	/*
	 * THE BASIS ARRIVES WHOLE. The original rebuilds it from two angles
	 * through g_cosTable; we send the nine floats, so there is no table
	 * and no handedness to disagree about - netplay-relay.txt 5.2.
	 */
	memcpy(r->matOrientation, p->basis, sizeof(r->matOrientation));
	r->position[0] = p->position[0];
	r->position[1] = p->position[1];
	r->position[2] = p->position[2];
	r->raceClockMs = p->raceClockMs;

	/* A teleport invalidates the chase, so restart it from here. */
	r->placeWheels0[0] = p->position[0];
	r->placeWheels0[1] = p->position[1];
	r->placeWheels0[2] = p->position[2];
	r->placeWheels1[0] = p->position[0];
	r->placeWheels1[1] = p->position[1];
	r->placeWheels1[2] = p->position[2];

	/* The original clears the lap pair on a place, at 0x00455c8c. */
	r->lapCounter      = 0;
	r->checkpointIndex = 0;
}

/* ORIGIN  maybe_Net_Dispatch case 0x6c 	 0x004556f0 */
static void race_net_on_race_over(void)
{
	s_race_overNet = 1;
}

/* ORIGIN  NONE - ours. 0xd6's table layout is [open]. */
static void race_net_on_results(int slot, const unsigned char *blob,
                                int length)
{
	(void)slot;
	(void)blob;
	(void)length;
}

/*
 * ORIGIN  NONE - ours
 *
 * REGISTERED, NOT CALLED DIRECTLY, so netplay.c never needs racer.h
 * and this file never needs a socket. It is also the one place to put
 * a log when two instances disagree - and since the original's
 * netplay cannot be run, that comparison is the only oracle there is.
 *
 * [open] THE LOBBY HANDLERS BELONG TO menu.c - onJoin, onLeave,
 * onLobby, onStart and onChat are left null here.
 */
void race_net_install(void)
{
	MR_NetHandlers h;

	memset(&h, 0, sizeof(h));
	h.onState     = race_net_on_state;
	h.onCollision = race_net_on_collision;
	h.onPlace     = race_net_on_place;
	h.onRaceOver  = race_net_on_race_over;
	h.onResults   = race_net_on_results;
	netplay_set_handlers(&h);
	s_race_netTick = 0;
	race_net_reset();
}
