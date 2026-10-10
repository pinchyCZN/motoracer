/*
 * skl.h - the articulated bike-and-rider model, and its poses.
 *
 * A .SKL is a rigidly skinned model: a bone tree, one shared vertex
 * pool, and a run of that pool owned by each bone. A .ROT is ONE POSE
 * for that tree - not an animation clip - so the twelve .rot files a
 * bike manifest names are twelve stances rather than twelve
 * sequences.
 *
 * Page 10 needs them because the bike select screen draws slot 0, the
 * articulated model, not the medium static body: maybe_Racer_SelectLod
 * is only ever called from gameplay, so racer+0x638 keeps the 0 that
 * Racer_Create leaves. See subsystems/menu-pages.txt section 2.7.5.
 *
 * The loader is FUN_00462c60 in player.cpp, and it reads with plain
 * fread through File_ReadU16 (0x00458d40) and File_ReadFloat
 * (0x00458d70) - no .bpt masking. Both formats are written up in
 * formats/skl-rot.txt; the layout below was verified by decoding
 * SPEED\CORPS.SKL and KTMCROSS\CORPS.SKL to the byte.
 */

#ifndef MRACER_SKL_H
#define MRACER_SKL_H

#include "mesh.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * EIGHTEEN IS THE LOADER'S OWN LIMIT, not ours: it prints
 * "%d members: Fichier moto invalide" and aborts above 0x12
 * [verified]. Both shipped skeletons use seventeen.
 */
#define SKL_MAX_MEMBERS 18

/*
 * THE .ROT FILE: 4 magic + 3 int16 of header + 17 angle triples.
 * 4 + 6 + 17*6 = 112, the size of every .rot the game ships.
 *
 * SKL_ROT_MEMBERS IS 17, NOT SKL_MAX_MEMBERS. Reading 18 triples
 * from offset 4 also lands on 112 bytes exactly, which is how that
 * bug survived - see the comment in rot_load.
 *
 * SKL_ROT_TURN IS 0x1000, NOT THE TRIG TABLE'S 0x4000. Rot_Load
 * scales by 2*pi/0x1000, so a .rot angle is FOUR TIMES the trig
 * index it resembles. See skl_apply_pose.
 */
#define SKL_ROT_TURN    0x1000
#define SKL_ROT_MEMBERS 17
#define SKL_ROT_BYTES   (4 + 3 * 2 + SKL_ROT_MEMBERS * 3 * 2)

/*
 * A face always stores FIVE index/u/v triples whatever its real
 * vertex count, with the first index repeated to close the polygon
 * and -1 filling the rest.
 */
#define SKL_FACE_SLOTS 5

typedef struct {
	int   parent;            /* -1 for the root                     */
	float origin[3];         /* joint position, in the parent frame */
	int   firstVertex;       /* into the shared pool                */
	int   vertexCount;       /* the runs sum to the pool exactly    */
	/*
	 * THE MEMBER'S SLICE OF mesh.faces. The file stores faces per
	 * member and the loader appends them in member order, so the
	 * range already exists in the flat list - this only records it.
	 *
	 * Skl_Load keeps the same thing at member+0x698 and +0x69c, and
	 * racerdraw_bind_skeleton copies both onto MR_RacerPart, where
	 * racerdraw_build_normals and racerdraw_queue_parts read them.
	 */
	int   firstFace;
	int   faceCount;
} MR_SklMember;

typedef struct {
	int          memberCount;

	/*
	 * THE MEMBER-TO-PART PERMUTATION from the file. A .rot is indexed
	 * by PART, so member i takes angle[remap[i]]. See skl_load.
	 */
	int          remap[SKL_MAX_MEMBERS];
	MR_SklMember member[SKL_MAX_MEMBERS];

	/*
	 * THE BIND POSE AND THE POSED COPY. bind is what the file holds;
	 * mesh.vertices is rewritten by skl_apply_pose and is what the
	 * geometry path sees, so the MESH can be handed to prim.c and
	 * model.c unchanged.
	 */
	MESH_VERTEX *bind;

	/*
	 * THE RUNS TILE THE POOL, ascending and with no gaps, so that
	 * appending each member's transformed slice in member order lands
	 * every vertex at its own shared-pool index.
	 *
	 * IT IS NOT A COSMETIC PROPERTY. The original transforms per
	 * member into a bump-allocated pool and queues the faces of ALL
	 * members - plus the trailing group - against the base the first
	 * transform returned. A face index is therefore a pool offset, and
	 * a gap or a reorder in the runs shifts every vertex after it. The
	 * shipped .skl files all satisfy this; the loader checks rather
	 * than assumes, and bike_draw bakes instead when it does not hold.
	 * See racers.txt 9.64.
	 */
	int          runsPacked;

	/*
	 * THE COMPOSED WORLD MATRIX PER MEMBER. The original stores one
	 * per part at MR_RacerPart+0x18 and DRAWS from them, and so does
	 * bike_draw: each member's slice of bind is transformed by the
	 * inverse of its matrix composed with the view. Keeping them is
	 * also what lets a caller adjust a single part between the walk
	 * and the draw - which the original does for part 15, re-anchoring
	 * the front bodywork - racers-convergence.txt 9.59.3.
	 *
	 * skl_transform_verts still bakes a posed model-space copy into
	 * mesh.vertices for callers that want to measure the pose. The
	 * draw does not read it.
	 *
	 * Valid after skl_build_matrices; indexed by MEMBER, not part.
	 */
	float mat[SKL_MAX_MEMBERS][12];
	MESH         mesh;
} MR_Skeleton;

/*
 * A pose is three angles per member in the game's own units, where a
 * full turn is TRIG_TABLE_SIZE - the same fixed point trig.h uses.
 */
typedef struct {
	short angle[SKL_MAX_MEMBERS][3];

	/* The three int16 after the magic. Rot_Load reads and discards
	 * them; kept so the next reader does not have to rediscover
	 * that they are there. Constant per archive, not per pose. */
	short header[3];
	int   loaded;
} MR_SklPose;

/*
 * Both take an asset name, never a path - "SPEED\CORPS.SKL",
 * "SPEED\STATICPIED.ROT". Return 0 on failure.
 */
int  skl_load(MR_Skeleton *s, const char *logical);
void skl_free(MR_Skeleton *s);
int  rot_load(MR_SklPose *p, const char *logical);

/*
 * skl_apply_pose - walk the tree and rewrite s->mesh.vertices.
 *
 * Rigid binding: every vertex belongs to exactly one member and is
 * transformed by that member's world matrix alone. There is no
 * weighting to blend, which is why the runs partition the pool
 * exactly rather than overlapping.
 *
 * A NULL pose is the bind pose with every joint at its origin.
 */
void skl_apply_pose(MR_Skeleton *s, const MR_SklPose *p);


/* ------------------------------------------------------------------ */
/*
 * THE RUNTIME POSE FRAME, and it is NOT MR_SklPose.
 *
 * MR_SklPose is what a .ROT file holds: SHORT angles for up to 18
 * members. What the animation blender works on is a different,
 * expanded record - an int count followed by FLOAT triples for up to
 * TWENTY joints:
 *
 *     int   count
 *     float angle[20][3]
 *
 * which is 4 + 240 = 244 bytes = 0xf4, and 0xf4 is exactly the stride
 * maybe_Racer_UpdatePose walks the stunt pose table with
 * (g_abPilotRot + id * 0xf4). The 20 is confirmed independently by
 * the three 20-iteration clear loops at the top of that function.
 *
 * SO THE .ROT SHORTS ARE EXPANDED TO FLOATS AT LOAD TIME and the
 * blender never sees the file format. Keeping the two types separate
 * matters: blending MR_SklPose directly would read shorts as floats.
 * [verified from the 0xf4 stride and the float loads at 0x0045b520]
 */
#define ANIM_MAX_JOINTS 20

typedef struct {
	int   count;                        /* how many joints are live */
	float angle[ANIM_MAX_JOINTS][3];    /* radians, three per joint */
} MR_AnimFrame;                         /* 244 bytes = 0xf4        */

/*
 * skl_build_pose / skl_build_anim / skl_transform_verts - the same
 * work in two halves.
 *
 * skl_apply_* is build followed immediately by transform. Split them
 * when a part needs adjusting first: build, write s->mat[member], then
 * transform. The original works this way because it must -
 * maybe_Racer_Draw replaces part 15's matrix after the walk and before
 * it draws. Matrices are indexed by MEMBER; remap[] maps a .rot part
 * to one.
 */
void skl_build_pose(MR_Skeleton *s, const MR_SklPose *p);
void skl_transform_verts(MR_Skeleton *s);

/*
 * The two parts the front-fork re-anchor names, as PART indices -
 * remap[] turns them into members. Part 1 is FourcheAv, the fork
 * pivot; part 15 is the front bodywork, which the original calls
 * RoueAV in its record ordering even though the wheel itself is a
 * separate mesh.
 */

/*
 * The parts the wheel geometry names, as PART indices - remap[]
 * turns them into members. Part 1 is FourcheAv, the fork pivot.
 *
 * PARTS 15 AND 16 ARE THE WHEEL NODES, and the original's names
 * RoueAV and RoueAR are literal: at rest the front wheel centre
 * lands exactly on part 15's pivot, and the rear tyre is drawn on
 * part 16's pivot with no adjustment at all. The tyres themselves
 * are separate meshes.
 */
#define SKL_PART_FORK_FRONT    1
#define SKL_PART_SWINGARM      2   /* the rear arm, 12 vertices  */
#define SKL_PART_ARM_PIVOT     3   /* a pure joint, no geometry  */
#define SKL_PART_WHEEL_FRONT  15
#define SKL_PART_WHEEL_REAR   16

/*
 * PAGE 10 ONLY, BOTH OF THEM. The race path no longer calls
 * skl_refork_front or skl_swingarm - racerdraw.c has the whole of
 * maybe_Racer_Draw and does the fork and the arm properly, with the
 * scale ratio the fork always needed and the arm angle measured in
 * the right frame.
 *
 * THEY SURVIVE BECAUSE menu.c STILL USES THEM. Page 10 draws a bike
 * with no MR_Racer behind it, and every racerdraw entry point takes
 * one. Migrating that page is its own piece of work; deleting these
 * before it happens breaks the bike-select screen.
 *
 * DO NOT REACH FOR THEM FROM THE RACE PATH AGAIN. They are slices of
 * a function that is now imported whole, kept alive for one caller.
 *
 * skl_refork_front - slide part 15 down the fork axis.
 *
 * Call BETWEEN skl_build_* and skl_transform_verts; it edits one
 * matrix and nothing else. Doing it after the vertices are posed
 * is too late, and that is the whole reason the walk and the
 * transform are separable.
 */
/*
 * Returns 1 when it ran, and then wheelOut - which may be NULL -
 * carries the FRONT WHEEL CENTRE, in the same space as the posed
 * mesh. DRAW THE WHEEL THERE, NOT AT THE CARAC OFFSET. Returns 0
 * if the skeleton has no fork or the geometry is degenerate, and
 * then wheelOut is untouched.
 */
/*
 * anim_blend_angle_frames - lerp two poses, per joint, per angle.
 *
 * Anim_BlendAngleFrames (0x0045b4e0), __thiscall with the output in
 * ECX. It is what every pose pass in maybe_Racer_UpdatePose goes
 * through - the base pose, the pitch, the glance, the lean, the
 * stunt and the crash are all this function applied in sequence,
 * each one blending the running result against another frame.
 *
 *     out.count = min(a.count, b.count)
 *
 *     t <= 0     copy a
 *     t >= 1     copy b
 *     otherwise, for every joint and each of its three angles:
 *
 *         y = b.angle
 *         if (y > x + pi) y -= 2pi        take the SHORT way round
 *         if (y < x - pi) y += 2pi
 *         r = x + (y - x) * t
 *         wrap r into [0, 2pi]
 *
 * IT IS THE SHORTEST-ARC WRAP THAT MAKES IT AN ANGLE BLEND rather
 * than a numeric one. Without it a joint at 350 degrees blending to
 * 10 would sweep backwards through the whole circle; with it, it
 * takes the twenty-degree path. Any port that lerps these as plain
 * floats will produce a rider who occasionally spins a limb the long
 * way round.
 *
 * THE COUNT IS A MINIMUM, NOT A REQUIREMENT. Blending frames with
 * different joint counts silently uses the smaller, so a pose with
 * fewer joints TRUNCATES the result rather than failing - which is
 * how the hand skeleton's extra members drop out when blended
 * against a body-only pose.
 *
 * THE ENDPOINT CASES ARE GUARDED AGAINST SELF-COPY: at t <= 0 it
 * copies a only if out is not already a, and likewise at t >= 1. So
 * blending a frame against itself at an endpoint is a genuine no-op
 * and not an overlapping copy.
 *
 * OUTPUT IS IN [0, 2pi], not [-pi, pi]. Worth knowing before
 * comparing an angle against zero.
 */
void anim_blend_angle_frames(MR_AnimFrame *out, const MR_AnimFrame *a,
                             const MR_AnimFrame *b, float t);


/*
 * THE PILOT POSE TABLE. Bike_LoadPilotRots (0x00410a10) fills an
 * array of MR_AnimFrame at 0x0051d2a8 by calling Rot_Load once per
 * entry of a parallel path list at 0x005191e4, count at 0x005191e0:
 *
 *     for (i = 0; i < count; i++)
 *         Rot_Load(&table[i], paths[i]);        stride 0xf4 / 0x40
 *
 * SO THE INDEX IS JUST THE LOAD ORDER, and the mapping is whatever
 * the path list says. Both the table and the list are UNINITIALISED
 * in the image - reading them statically gives zeros - so the order
 * below was read out of a running game.
 *
 * THE PATHS CARRY THE BIKE NAME - data\speed\... - so the set is
 * PER BIKE and the table length varies. A road bike loads 31; a dirt
 * bike loads more, because KTMCROSS ships SAUTDELIRE*.ROT stunt poses
 * that SPEED does not. That is what reconciles a "32 frames" count
 * with the index 44 seen in a dirt-bike trace:
 * neither is a bound, they are two different bikes.
 *
 * [verified, live read on a road bike]
 */
enum {
	PILOT_ROT_STATIC        =  0,  /* static.rot       the neutral  */
	PILOT_ROT_LEAN_L        =  1,  /* gauche.rot                    */
	PILOT_ROT_LEAN_R        =  2,  /* droite.rot                    */
	PILOT_ROT_STATIC_HAUT   =  3,  /* staticHaut.rot   riding base  */
	PILOT_ROT_STATIC_PIED   =  4,  /* staticPied.rot   foot down    */
	PILOT_ROT_SLIDE_L       =  5,  /* derapGauche.rot  derapage     */
	PILOT_ROT_SLIDE_R       =  6,  /* derapDroite.rot               */
	PILOT_ROT_JUMP          =  7,  /* saut.rot         pitch up     */
	PILOT_ROT_STATIC_WIN    =  8,  /* staticgagne.rot  leading      */
	PILOT_ROT_DESCENT       =  9,  /* descent.rot      pitch down   */
	PILOT_ROT_FINISH_2      = 10,  /* fin2.rot                      */
	PILOT_ROT_FINISH_3      = 11,  /* fin3.rot                      */
	PILOT_ROT_FINISH_7      = 12,  /* fin7.rot                      */
	PILOT_ROT_FINISH_8      = 13,  /* fin8.rot                      */
	PILOT_ROT_FINISH_OTHER  = 14,  /* finautre.rot                  */
	PILOT_ROT_CRASH_FALL    = 15,  /* chuteDos.rot     onto his back */
	PILOT_ROT_CRASH_DOWN    = 16,  /* staticDos.rot    lying there  */
	PILOT_ROT_PODIUM_FIRST  = 17,  /* a1,b1,a2,b2,a3,b3,a4,b4,      */
	PILOT_ROT_PODIUM_LAST   = 28,  /* c1,d1,e1,f1                   */
	PILOT_ROT_GLANCE_R      = 29,  /* lookdroite.rot                */
	PILOT_ROT_GLANCE_L      = 30   /* lookgauche.rot                */
};


/*
 * A STUNT is not one pose but a RUN of consecutive frames.
 *
 * maybe_Bike_ReadManifest marks them in the PilotRot list with a
 * "_N_" prefix - "_3_sautdelire1.rot" - where N is how many frames
 * the animation spans. maybe_Racer_UpdatePose then walks the run,
 * blending frame k against frame k-1, which is why the stunt path
 * indexes the table by (base + k * 0xf4).
 *
 * THE TWO EXTRA INTEGERS ARE DURATION THEN HAND SKELETON, in that
 * order, and they follow the name in the stream. Read off
 * maybe_Bike_ReadManifest:
 *
 *     0042a1e1  record+0x08 = the '_N_' digit - 0x30
 *     0042a1ef  record+0x04 = the pose index
 *     0042a1fe  Settings_ReadInt -> record+0x0c   duration
 *     0042a209  Settings_ReadInt -> record+0x00   hand skeleton
 *
 * They are parsed now. While they were not, durationMs read 0 and
 * racer_pose_stunt_step returned 0, so a stunt ended on the frame it
 * started.
 */
typedef struct {
	int poseIndex;    /* +0x04 of the 16-byte record at 0x00580d08  */
	int frameCount;   /* +0x08, the N from the "_N_" prefix         */
	int durationMs;   /* +0x0c, Settings_ReadInt at 0x0042a1fe      */
	int handSkeleton; /* +0x00, Settings_ReadInt at 0x0042a209      */
} MR_StuntPose;

#define PILOT_ROT_MAX   64
#define STUNT_POSE_MAX  16

typedef struct {
	MR_AnimFrame frame[PILOT_ROT_MAX];
	int          frameCount;

	MR_StuntPose stunt[STUNT_POSE_MAX];
	int          stuntCount;
} MR_PilotRots;

/*
 * anim_frame_from_pose - expand a loaded .ROT into a blendable frame.
 *
 * A .ROT holds SHORT angles in SKL_ROT_TURN units; MR_AnimFrame holds
 * radians. This is the conversion the original does inside Rot_Load,
 * which is why the blender never sees the file format (9.51.1):
 *
 *     radians = angle * 2pi / 0x1000
 *
 * THE COUNT IS SKL_ROT_MEMBERS, 17, not SKL_MAX_MEMBERS. That is
 * confirmed from the other end: maybe_Racer_UpdatePose's plate
 * comment records a live table entry with "part count 17".
 *
 * NO REMAP IS APPLIED. skl_apply_pose permutes member i to
 * angle[remap[i]] when it poses a skeleton, but the blend table is
 * indexed by PART throughout - the remap belongs at the point the
 * pose meets a particular skeleton, not here. Applying it twice
 * would swap the wheels with the forks.
 */
void anim_frame_from_pose(MR_AnimFrame *out, const MR_SklPose *p);

/*
 * pilot_rots_load - load a bike's whole pose set from its manifest.
 *
 * Bike_LoadPilotRots (0x00410a10) with maybe_Bike_ReadManifest
 * (0x0042a120) in front of it.
 *
 * THE COMPILED .BPT HAS NO "PilotRot" KEY. maybe_Bike_ReadManifest
 * calls Settings_ReadString for it, but that reads the .SPT TEXT
 * source; the compiled manifest drops most key strings and carries
 * the meaning POSITIONALLY. Decoding MOTOSPD.BPT shows records 0..17
 * as the models, 18..48 as the poses, and record 49 as the literal
 * "MotoCarac" - one of the few markers that does survive.
 *
 * So the list starts at firstRecord and runs until a record with no
 * '.' in it, which is that MotoCarac marker. There is no count
 * anywhere.
 *
 * THE INDEX IS THE LOAD ORDER and nothing else (racers.txt 9.52), so
 * this function alone decides what every pose index in
 * maybe_Racer_UpdatePose means. The order for a road bike is in the
 * PILOT_ROT_* enum above; a dirt bike's differs, because it appends
 * stunt entries SPEED does not ship.
 *
 * IT CLEARS THE TABLE FIRST, AND THAT IS A DIVERGENCE. The original
 * never clears either array - maybe_Bike_ReadManifest and
 * Bike_LoadPilotRots simply overwrite the first count entries - so
 * after a dirt bike, a road bike's slots 31 and up still hold
 * KTMCROSS poses. Verified live: with SPEED loaded and
 * g_nPilotRotCount 31, path slot 31 read data\ktmcross\depart.rot
 * and pose frame 31 held 22 non-zero angles.
 *
 * THAT MATTERS BECAUSE THE PRE-RACE BLEND TARGETS INDEX 31. On a
 * dirt bike that is depart.rot and correct; on a road bike, which
 * ships no depart.rot, it is whatever was played before. Clearing
 * makes it a zero frame every time instead - predictable rather than
 * history-dependent. See racers.txt 9.53.4 and 9.53.5.
 *
 * Returns the number of frames loaded, 0 on failure.
 */
int pilot_rots_load(MR_PilotRots *out, const char *manifest,
                    int firstRecord);

#ifdef __cplusplus
}
#endif

#endif /* MRACER_SKL_H */
