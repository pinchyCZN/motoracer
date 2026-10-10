/*
 * racer.h - one competitor.
 *
 * THE STRUCT IS THE ORIGINAL'S RECORD, BYTE FOR BYTE. Every field
 * carries the offset it came from, #pragma pack(1) holds the layout,
 * and MR_Racer_prefix_ends asserts that the original's 0x3209 bytes
 * end exactly where our own fields begin. That is what lets
 * moto_get_mem and mracer_get_mem read the same field at the same
 * offset on both sides - CLAUDE.md 15.2.
 *
 * SO A FIELD PLACED BY GUESSWORK IS WORSE THAN ONE LEFT AS PADDING:
 * the padding would look like knowledge. Anything still unidentified
 * keeps an f_NNNN or unk_NNNN name.
 *
 * docs/structs/MR_Racer.txt IS THE DESCRIPTION and this is the
 * layout. The block map, the pose array, the tail and the
 * field-by-field evidence live there; the offsets below and the
 * assertions at the end of the file are the record, so none of it is
 * repeated here.
 *
 *
 * THREE THINGS THAT BITE WHOEVER EDITS THIS FILE
 *
 * o MOST OF WHAT READS LIKE A RACER FIELD IS INSIDE INSTANCE 0.
 *   +0x30 is instance+0x0c, +0xb4 is instance+0x90, +0xbc is
 *   instance+0x98, and the whole +0x110..+0x124 run is instance+0xec
 *   onward. Laying out an opaque 0x184-byte instance and then
 *   appending these DOUBLE-COUNTS 388 BYTES. MR_Racer.txt 3.1.
 *
 * o +0x31b1 IS A FLOAT and reads as an int. Nothing writes it by
 *   name: maybe_Race_UpdateProgress hands the track a pointer to it
 *   at 0x0047e422 and the track writes through it, which is a class
 *   of write a displacement search cannot see. Its four neighbours
 *   +0x31b5..+0x31c1 really are ints - segment indices.
 *
 * o INDEX 1 IS THE LOCAL PLAYER, index 0 unused. g_racers is
 *   0x00581218 and index 1 is 0x00584421, which is g_menuRacer. The
 *   indexing is kept rather than packed from zero so ours and theirs
 *   line up.
 *
 *
 * TWO PLACES A PORT GETS THE MATRIX WRONG, written up rather than
 * repeated:
 *
 * o +0xbc CARRIES NO ROLL, which is NOT the same as "yaw only" - it
 *   pitches over bumps, because UpdateGroundContact crosses the two
 *   wheel contacts with world up. Built as a pure Y rotation the bike
 *   sits flat on every jump face on the dirt tracks. MR_Racer.txt 8.
 *
 * o STEERING (+0x110) FADES TO NOTHING AT HALF MAX SPEED, and the
 *   ANGLE IS NEVER STORED - this input is. Leave the fade out and the
 *   forks saw visibly at racing speed where the original holds them
 *   straight. racer-draw-pipeline.txt 7; +0x2e39 next to it is fork
 *   dive, not steering.
 */
#ifndef MRACER_RACER_H
#define MRACER_RACER_H

#include <stddef.h>
#include "bikes.h"
#include "grid.h"
#include "trk.h"
#include "physics.h"
#include "skl.h"
#include "audio.h"   /* MR_Voice, for MR_RacerAudio */

#ifdef __cplusplus
extern "C" {
#endif

#define RACER_MAX GRID_MAX_SLOTS

/*
 * #pragma pack(1) IS REQUIRED, not a preference. The original's tail
 * is byte-misaligned throughout - lean at +0x2e1d, curSegment at
 * +0x31bd, the wall normal at +0x31c5 - and the stride 0x3209 is
 * itself odd. Natural alignment cannot reproduce any of that.
 * Unaligned float access is fine on x86 and this build is Win32.
 */
#pragma pack(push, 1)

#define RACER_PARTS_PER_LOD  20
#define RACER_LOD_SLOTS       5

/*
 * THE TWO SHADOW MESHES THE RACER CARRIES, laid out from
 * Racer_Ctor and maybe_Racer_DrawShadow rather than guessed.
 *
 * Racer_Ctor constructs six vertices at +0x2ea9 with stride 0xc and
 * two primitives at +0x2ef1 with stride 0x40 (0x0045b90a..0x0045b94a),
 * and 6*12 + 2*64 is exactly the 200 bytes that span reaches to
 * +0x2f71. The rider pair is the same shape with four vertices: 48 +
 * 128 = 176, +0x30ed to +0x319d. Both arithmetics close, which is what
 * makes this a layout rather than a guess.
 *
 * THE VERTICES ARE PLAIN float[3], NOT MR_Vertex. They are the SOURCE
 * handed to Screen3D_TransformVertsFlat, which reads three floats per
 * vertex; the transformed MR_Vertex output goes to the shared pool.
 */
typedef struct MR_RacerShadow {
	float        vert[6][3];   /* +0x00 the ground-projected hexagon */
	MR_Primitive prim[2];      /* +0x48 two black quads              */
} MR_RacerShadow;              /* 200 bytes                          */

typedef struct MR_RacerRiderShadow {
	float        vert[4][3];   /* +0x00 */
	MR_Primitive prim[2];      /* +0x30 */
} MR_RacerRiderShadow;         /* 176 bytes */

typedef char MR_RacerShadow_is_200
        [(sizeof(MR_RacerShadow) == 200) ? 1 : -1];
typedef char MR_RacerRiderShadow_is_176
        [(sizeof(MR_RacerRiderShadow) == 176) ? 1 : -1];


/*
 * MR_RacerPart - one bone of a racer's skeleton. 100 bytes, all of
 * them accounted for [verified - Skl_BuildPartMatrices 0x00463d90;
 * docs/structs/MR_RacerPart.txt has the layout and the tree].
 *
 * THE TREE POINTERS RUN SIBLING, PARENT, CHILD - NOT the child-first
 * order the field order suggests. MR_RacerPart.txt 2.1 has the proof
 * and 3 has the two trees, including the one thing that does not
 * follow from the field names: the chains are PUSH-FRONT, so a port
 * that appends walks the tree in the opposite order.
 */
typedef struct MR_RacerPart {
	struct MR_RacerPart *next;     /* +0x00  NEXT SIBLING, not child */
	struct MR_RacerPart *parent;   /* +0x04  null at the two roots   */
	struct MR_RacerPart *child;    /* +0x08  FIRST CHILD             */
	float                angle[3]; /* +0x0c  radians                 */
	float                matrix[12]; /* +0x18  what the walk writes  */
	float                pivot[3]; /* +0x48  the joint               */
	int                  firstVertex; /* +0x54  into the LOD mesh    */
	int                  vertexCount;  /* +0x58                      */
	int                  faceCount;    /* +0x5c                      */
	void                *faces;        /* +0x60  MR_Primitive *      */
} MR_RacerPart;

typedef char MR_RacerPart_off_angle
        [(offsetof(MR_RacerPart, angle) == 0x0c) ? 1 : -1];
typedef char MR_RacerPart_off_matrix
        [(offsetof(MR_RacerPart, matrix) == 0x18) ? 1 : -1];
typedef char MR_RacerPart_off_pivot
        [(offsetof(MR_RacerPart, pivot) == 0x48) ? 1 : -1];
typedef char MR_RacerPart_off_faces
        [(offsetof(MR_RacerPart, faces) == 0x60) ? 1 : -1];
typedef char MR_RacerPart_size
        [(sizeof(MR_RacerPart) == 100) ? 1 : -1];


typedef struct MR_Racer {
	int            drawnThisFrame;   /* +0x0000 */
	int            soloMode;   /* +0x0004 */
	int            humanPlayer;   /* +0x0008 */
	int            f_000c;   /* +0x000c */
	int            f_0010;   /* +0x0010 */
	int            f_0014;   /* +0x0014 */
	float          turnRadius[2];   /* +0x0018 */
	unsigned char  unk_0020[16];   /* +0x0020 */
	float          position[3];   /* +0x0030 */
	float          positionPrev[3];   /* +0x003c */
	float          positionPrev2[3];   /* +0x0048 */
	float          frameDelta;   /* +0x0054 */
	unsigned char  unk_0058[16];   /* +0x0058 */
	int            active;   /* +0x0068 */
	unsigned char  unk_006c[12];   /* +0x006c */
	int            bikeIndex;   /* +0x0078 */
	int            gear;   /* +0x007c */
	int            shiftTimerMs;   /* +0x0080 */
	int            shiftState;   /* +0x0084 */
	int            f_0088;   /* +0x0088 */
	int            pendingDownshift;   /* +0x008c */
	unsigned char  unk_0090[28];   /* +0x0090 */
	int            turnBrakeFlag;   /* +0x00ac */
	/*
	 * THE BRAKE HOLD, in milliseconds - the original instance+0x8c.
	 * maybe_Physics_TickDrivetrain reads it to decide whether the
	 * brake is still in its soft opening ramp, and zeroes it on every
	 * non-brake path.
	 */
	int            brakeHoldMs;   /* +0x00b0 */
	float          realSpeedMax;   /* +0x00b4 */
	float          wheelieScale;   /* +0x00b8 */
	float          matOrientation[12];   /* +0x00bc */
	float          engineSpeed;   /* +0x00ec */
	float          flAirTime;   /* +0x00f0 */
	float          prevLongAccel;   /* +0x00f4 */
	float          steerReturnScale;   /* +0x00f8 */
	unsigned char  unk_00fc[12];   /* +0x00fc */
	unsigned int   f_0108;   /* +0x0108 */
	float          steerRate;   /* +0x010c */
	float          steering;   /* +0x0110 */
	int            speedKmhRaw;   /* +0x0114 */
	int            speedKmhNeedle;   /* +0x0118 */
	int            tachoRpm;   /* +0x011c */
	/*
	 * A FLOAT, NOT AN INT, AND IT READS AS A PLAUSIBLE INTEGER - a
	 * live dump of an AI shows -1035468800, which is -50.0f read as
	 * one. Racer_UpdateDash uses FADD/FSTP/FLD at 0x004679dc,
	 * 0x004679e8 and 0x00467a1e, and the two clamps store the raw bit
	 * patterns 0x42200000 and 0xc2480000 - +40.0f and -50.0f - as
	 * dwords. Typed int anywhere, every store renders as (int)(...).
	 */
	float          needleRate;   /* +0x0120 */
	float          speed;   /* +0x0124 */
	float          speedPrev;   /* +0x0128 */
	float          gearboxRoadSpeed;   /* +0x012c */
	float          velDir[3];   /* +0x0130 */
	int            driveDisengaged;   /* +0x013c */
	float          matOrientationPrev[12];   /* +0x0140 */
	/*
	 * THE LIVERY - instance 0's +0x14c, and the racer's own
	 * colour. Racer_Create writes param_3 & 7 here, or -1 for
	 * "not paletted", and writes the same value to the other
	 * three instances at +0x2f4, +0x478 and +0x5fc [verified].
	 *
	 * It is NOT an index into anything the racer owns: it picks
	 * one of the eight palettes of the ONE bike page, which is
	 * how a race shows twenty colours off a single texture.
	 * See docs/subsystems/liveries.txt.
	 */
	int            livery;   /* +0x0170 */
	/*
	 * THE COLLISION PUSH, direction and magnitude, and it is
	 * written by ONE function: maybe_Game_CollideRacers. Racer
	 * against racer sets +0x174 to a unit direction and +0x180
	 * to how hard, and racer_integrate is the only consumer -
	 * disp += collideDir * collideSpeed * scale.
	 * See docs/subsystems/racer-collision.txt.
	 */
	float          collideDir[3];   /* +0x0174 */
	float          collideSpeed;   /* +0x0180 */
	float          skidWidthPrev;   /* +0x0184 */
	float          skidWidth;   /* +0x0188 */
	/*
	 * MAINTAINED BY THE SLIP INTEGRATOR, not derived where it is
	 * read. Rebuilding it from the frame-to-frame displacement
	 * gives the same number - the position is integrated along
	 * it - and goes wrong the moment anything else moves the
	 * bike.
	 */
	float          moveDir[3];   /* +0x018c */
	/*
	 * THE AI's AIM VECTOR AND ITS TARGET SPEED, and +0x198 IS A
	 * float[3], NOT A POINTER. Typed as one, every access renders as
	 * *(float *)(unk_0198 + 0xc) - pointer arithmetic that reads as a
	 * separate object and is really just racer+0x1a4.
	 *
	 * READ LIVE off three AI racers: it is a UNIT VECTOR, |v| = 1.0000
	 * on all three, and it is NOT moveDir at +0x18c.
	 * Racer_AiFollowLine builds it from matOrientation row2 + row3 and
	 * normalises, then blends the bike's travel toward it.
	 *
	 *     racer 2   aim (-0.4876, -0.0969, 0.8677)  |v| 1.0000
	 *               moveDir (-0.2713, -0.1203, 0.9549)
	 *
	 * RACER 1 HOLDS (0,0,1) AND 0.0 and always will - nothing on the
	 * player path writes either.
	 */
	float          aimDir[3];   /* +0x0198 */
	float          aiRoadSpeed;   /* +0x01a4 */
	unsigned char  unk_0198_01a8[12];   /* +0x01a8 */
	float          frontWheelContact[3];   /* +0x01b4 */
	float          frontWheelContactPrev[3];   /* +0x01c0 */
	unsigned char  unk_01cc[0x128];   /* +0x01cc */
	/* [open - the DB has +0x2f4, +0x478 and +0x5fc inside two
	   undefined arrays and cannot name them without carving those
	   up. */
	int            liveryFrontWheel;   /* +0x02f4, instance 1 */
	unsigned char  unk_02f8[0x40];   /* +0x02f8 */
	float          rearWheelContact[3];   /* +0x0338 */
	float          rearWheelContactPrev[3];   /* +0x0344 */
	unsigned char  unk_0350[0x128];   /* +0x0350 */
	int            liveryRearWheel;   /* +0x0478, instance 2 */
	unsigned char  unk_047c[0x180];   /* +0x047c */
	/*
	 * INSTANCE 3'S LIVERY, AND IT IS THE ONE THAT COLOURS THE
	 * BIKE YOU LOOK AT. Instance 3 has no mesh; maybe_Racer_Draw
	 * dispatches the POSE ARRAY - the bike body and the rider,
	 * part by part - on this field [verified, 0x00461a30]. So
	 * the "body" livery is here, not at +0x170.
	 *
	 * Racer_Create writes the same value to both, which is why
	 * the difference is invisible in a race. IT IS NOT INVISIBLE
	 * ON THE BIKE SELECT PAGE: MenuPage10_BikeSelect_Tick
	 * rewrites +0x5fc, +0x2f4 and +0x478 every frame as the
	 * selection spins and leaves +0x170 alone [verified, the
	 * three stores at 0x0044efbb]. Reading +0x170 there gives
	 * the livery the page was entered with.
	 */
	int            liveryRider;   /* +0x05fc, instance 3 */
	unsigned char  unk_0600[0x34];   /* +0x0600 */
	float          cameraDist;   /* +0x0634 */
	int            skeletonSlot;   /* +0x0638 */
	/*
	 * THE POSE ARRAY, five LOD slots of twenty parts. 5*20*100 is
	 * exactly 10000, so typing it costs nothing in layout.
	 */
	MR_RacerPart   pose[RACER_LOD_SLOTS][RACER_PARTS_PER_LOD];   /* +0x063c */
	unsigned char  perSlotArrays[60];   /* +0x2d4c */
	unsigned char  unk_2d88[24];   /* +0x2d88 */
	int            fxGate;   /* +0x2da0 */
	int            fxGatePrev;   /* +0x2da4 */
	float          speedTail;   /* +0x2da8 */
	unsigned int   riderStateBits;   /* +0x2dac */
	float          f_2db0;   /* +0x2db0 */
	int            f_2db4;   /* +0x2db4 */
	int            onGround;   /* +0x2db8 */
	int            onGroundPrev;   /* +0x2dbc */
	int            onGroundPair1;   /* +0x2dc0 */
	int            onGroundPair2;   /* +0x2dc4 */
	int            tickInputWord;   /* +0x2dc8 */
	int            f_2dcc;   /* +0x2dcc */
	int            slotIndex;   /* +0x2dd0 */
	char           playerName[9];   /* +0x2dd4 */
	int            raceClockMs;   /* +0x2ddd */
	int            bestLapMs;   /* +0x2de1 */
	int            raceState;   /* +0x2de5 */
	unsigned char  unk_2de9[12];   /* +0x2de9 */
	float          placeWheels0[3];   /* +0x2df5 */
	float          placeWheels1[3];   /* +0x2e01 */
	int            dead_2e0d;   /* +0x2e0d */
	float          placeWheels2[3];   /* +0x2e11 */
	float          lean;   /* +0x2e1d */
	float          flLeanPrev;   /* +0x2e21 */
	unsigned char  unk_2e25[4];   /* +0x2e25 */
	int            leanMode;   /* +0x2e29 */
	unsigned char  unk_2e2d[4];   /* +0x2e2d */
	float          flLeanTarget;   /* +0x2e31 */
	unsigned char  unk_2e35[4];   /* +0x2e35 */
	float          forkDive;   /* +0x2e39 */
	/*
	 * THE TWO WHEEL SPIN ANGLES, and they are EULER TRIPLES rather
	 * than scalars: racerdraw composes each with mat34_from_euler
	 * onto the matching wheel's matrix. Only [0] is ever written -
	 * [1] and [2] stay zero - so the spin is a pitch about the
	 * axle.
	 *
	 * racer_spin_wheels is the ONLY writer, and advances each by
	 * the distance that wheel's contact point moved along the
	 * bike's forward axis over that wheel's radius. Without it the
	 * wheels stand still; braking locks them.
	 *
	 * REAR COMES FIRST, which is the binary's order and not a
	 * mistake - rearWheelContact and frontWheelContact are the
	 * other way round at +0x1b4 and +0x338.
	 */
	float          rearWheelSpin[3];   /* +0x2e3d, from rayonAr */
	float          frontWheelSpin[3];   /* +0x2e49, from rayonAv */
	float          matBikeRoot[12];   /* +0x2e55 */
	int            stuntOrOverspeed;   /* +0x2e85 */
	int            dead_2e89;   /* +0x2e89 */
	float          suspensionAngle;   /* +0x2e8d */
	float          springBounce;   /* +0x2e91 */
	float          springCompression;   /* +0x2e95 */
	float          springPhase;   /* +0x2e99 */
	float          springAmplitude;   /* +0x2e9d */
	float          headingSmoothed;   /* +0x2ea1 */
	float          headingRef;   /* +0x2ea5 */
	MR_RacerShadow bikeShadow;   /* +0x2ea9 */
	int            fxSlot0;   /* +0x2f71 */
	int            fxSlot1;   /* +0x2f75 */
	short          checkpointIndex;   /* +0x2f79 */
	short          lapCounter;   /* +0x2f7b */
	int            f_2f7d;   /* +0x2f7d */
	int            f_2f81;   /* +0x2f81 */
	int            finishPosition;   /* +0x2f85 */
	int            checkpointSplits[48];   /* +0x2f89 */
	/*
	 * THE AUDIO MIXER'S DISTANCE FROM THE CAMERA EYE, refreshed
	 * once a frame by the sound driver [maybe_Snd_TickRace
	 * 0x004079e0]. NOT cameraDist at +0x634, which is the chase
	 * boom - these are different numbers about different things.
	 */
	float          sndDistToEye;   /* +0x3049 */
	unsigned int   poseStaticPied;   /* +0x304d */
	float          poseBlend;   /* +0x3051 */
	int            stuntStartStamp;   /* +0x3055 */
	float          f_3059;   /* +0x3059 */
	unsigned int   stuntPose;   /* +0x305d */
	int            stuntNext;   /* +0x3061 */
	int            stuntPoseId;   /* +0x3065 */
	unsigned int   glancePose;   /* +0x3069 */
	unsigned int   glanceOther;   /* +0x306d */
	int            f_3071;   /* +0x3071 */
	int            f_3075;   /* +0x3075 */
	float          glanceTimer;   /* +0x3079 */
	float          wheelieAngle;   /* +0x307d */
	float          wheelieTimer;   /* +0x3081 */
	float          matRiderRoot[12];   /* +0x3085 */
	float          riderPosition[3];   /* +0x30b5 */
	int            crashLatch;   /* +0x30c1 */
	float          flCrashTimerMs;   /* +0x30c5 */
	float          crashLaunchDir[3];   /* +0x30c9 */
	float          flRiderSpeed;   /* +0x30d5 */
	float          flRiderSpeedRef;   /* +0x30d9 */
	float          crashFallSide;   /* +0x30dd */
	float          f_30e1[3];   /* +0x30e1 */
	MR_RacerRiderShadow riderShadow;   /* +0x30ed */
	int            handSkeleton;   /* +0x319d */
	float          f_31a1[3];   /* +0x31a1 */
	int            ghostEvent;   /* +0x31ad */
	/*
	 * A FLOAT, NOT AN INT, and the declaration is a lie the struct
	 * keeps because nothing in the port writes it yet.
	 * maybe_Race_UpdateProgress hands the track a POINTER to this
	 * field at 0x0047e422 and the track writes a float through it -
	 * read live as 0xc3fb0000, a clean -502.0. It is the
	 * sub-segment fraction Race_ComputeStandings ORs into the low
	 * twelve bits of its sort key.
	 */
	float          subSegment;   /* +0x31b1 */
	/*
	 * THE ACCUMULATED SIGNED PROGRESS, and the name is wrong.
	 *
	 * MR_Racer.txt 22.3 read it as a per-TRACK constant because it
	 * was identical across all eight finishers - which is what
	 * accumulated progress looks like when everyone finished the
	 * same distance. 1432 is segmentCount x laps. Mid-race it is
	 * not constant: eight seconds into ile01 it reads 3.
	 *
	 * Race_ComputeStandings sorts the racing bucket on this, shifted
	 * up twelve bits. race_update_progress seeds it at grid placement
	 * and adds the wrapped segment delta to it every frame.
	 */
	int            trackTotalProgress;   /* +0x31b5 */
	/*
	 * THE HIGH-WATER MARK of trackTotalProgress, and it is only ever
	 * RAISED: maybe_Race_UpdateProgress does
	 *
	 *     if (highWater < accumulated) highWater = accumulated;
	 *
	 * so reversing lowers the accumulator but cannot lower this. The
	 * checkpoint sweep runs only on a frame that raises it, which is
	 * what stops a lap being gained by backing over a line and
	 * driving forward again.
	 */
	int            progressHigh;   /* +0x31b9 */
	int            segmentPrev;   /* +0x31bd */
	/*
	 * THE CURRENT SEGMENT, refreshed every frame, and the name is
	 * the original's rather than a description - InitRace assigns it
	 * and then it rolls. It is the hint every
	 * trk_find_segment_for_point call passes.
	 */
	int            segmentStart;   /* +0x31c1 */
	float          wallNormal[3];   /* +0x31c5 */
	float          flImpactBlend;   /* +0x31d1 */
	float          chargeMeter;   /* +0x31d5 */
	int            lane;   /* +0x31d9 */
	int            cameraIndex;   /* +0x31dd */
	int            dead_31e1;   /* +0x31e1 */
	int            dead_31e5;   /* +0x31e5 */
	unsigned int   inputWord;   /* +0x31e9 */
	unsigned int   inputWord2;   /* +0x31ed */
	int            axis0;   /* +0x31f1 */
	int            axis1;   /* +0x31f5 */
	int            stuntHeld;   /* +0x31f9 */
	int            action6Down;   /* +0x31fd */
	int            stuntPressed;   /* +0x3201 */
	int            stuntPressedPrev;   /* +0x3205 */

	/*
	 * PAST THE ORIGINAL. Everything above is the Ghidra DB's
	 * layout VERBATIM, so an imported function can use the offsets
	 * the decompiler gives it and moto_get_mem / mracer_get_mem
	 * read the same field at the same offset. Everything below is
	 * ours and claims nothing about the original.
	 */
	int            isLocal;   /* ours - which slot the local player rides */
	float          moveDirClipped[3];   /* ours - the reflected direction */
} MR_Racer;
#pragma pack(pop)

/*
 * THE LAYOUT IS CHECKED AT COMPILE TIME. A field asserted at the
 * wrong offset is a build error; a field quietly moved by an edit
 * above it would otherwise be a wrong number in a capture three
 * sessions later. See docs/whole-function-import.txt section 3.
 */
typedef char MR_Racer_off_brakeHoldMs
        [(offsetof(MR_Racer, brakeHoldMs) == 0x00b0) ? 1 : -1];
typedef char MR_Racer_off_soloMode
        [(offsetof(MR_Racer, soloMode) == 0x0004) ? 1 : -1];
typedef char MR_Racer_off_humanPlayer
        [(offsetof(MR_Racer, humanPlayer) == 0x0008) ? 1 : -1];
typedef char MR_Racer_off_position
        [(offsetof(MR_Racer, position) == 0x0030) ? 1 : -1];
typedef char MR_Racer_off_positionPrev
        [(offsetof(MR_Racer, positionPrev) == 0x003c) ? 1 : -1];
typedef char MR_Racer_off_bikeIndex
        [(offsetof(MR_Racer, bikeIndex) == 0x0078) ? 1 : -1];
typedef char MR_Racer_off_realSpeedMax
        [(offsetof(MR_Racer, realSpeedMax) == 0x00b4) ? 1 : -1];
typedef char MR_Racer_off_matOrientation
        [(offsetof(MR_Racer, matOrientation) == 0x00bc) ? 1 : -1];
typedef char MR_Racer_off_steering
        [(offsetof(MR_Racer, steering) == 0x0110) ? 1 : -1];
typedef char MR_Racer_off_speedKmhNeedle
        [(offsetof(MR_Racer, speedKmhNeedle) == 0x0118) ? 1 : -1];
typedef char MR_Racer_off_speed
        [(offsetof(MR_Racer, speed) == 0x0124) ? 1 : -1];
typedef char MR_Racer_off_velDir
        [(offsetof(MR_Racer, velDir) == 0x0130) ? 1 : -1];
typedef char MR_Racer_off_collideSpeed
        [(offsetof(MR_Racer, collideSpeed) == 0x0180) ? 1 : -1];
typedef char MR_Racer_off_moveDir
        [(offsetof(MR_Racer, moveDir) == 0x018c) ? 1 : -1];
typedef char MR_Racer_off_frontWheelContact
        [(offsetof(MR_Racer, frontWheelContact) == 0x01b4) ? 1 : -1];
typedef char MR_Racer_off_rearWheelContact
        [(offsetof(MR_Racer, rearWheelContact) == 0x0338) ? 1 : -1];
typedef char MR_Racer_off_riderStateBits
        [(offsetof(MR_Racer, riderStateBits) == 0x2dac) ? 1 : -1];
typedef char MR_Racer_off_onGround
        [(offsetof(MR_Racer, onGround) == 0x2db8) ? 1 : -1];
typedef char MR_Racer_off_onGroundPrev
        [(offsetof(MR_Racer, onGroundPrev) == 0x2dbc) ? 1 : -1];
typedef char MR_Racer_off_lean
        [(offsetof(MR_Racer, lean) == 0x2e1d) ? 1 : -1];
typedef char MR_Racer_off_flLeanTarget
        [(offsetof(MR_Racer, flLeanTarget) == 0x2e31) ? 1 : -1];
typedef char MR_Racer_off_forkDive
        [(offsetof(MR_Racer, forkDive) == 0x2e39) ? 1 : -1];
typedef char MR_Racer_off_prevLongAccel
        [(offsetof(MR_Racer, prevLongAccel) == 0xf4) ? 1 : -1];
typedef char MR_Racer_off_matBikeRoot
        [(offsetof(MR_Racer, matBikeRoot) == 0x2e55) ? 1 : -1];
typedef char MR_Racer_off_rearWheelSpin
        [(offsetof(MR_Racer, rearWheelSpin) == 0x2e3d) ? 1 : -1];
typedef char MR_Racer_off_frontWheelSpin
        [(offsetof(MR_Racer, frontWheelSpin) == 0x2e49) ? 1 : -1];
typedef char MR_Racer_off_springBounce
        [(offsetof(MR_Racer, springBounce) == 0x2e91) ? 1 : -1];
typedef char MR_Racer_off_springCompression
        [(offsetof(MR_Racer, springCompression) == 0x2e95) ? 1 : -1];
typedef char MR_Racer_off_headingRef
        [(offsetof(MR_Racer, headingRef) == 0x2ea5) ? 1 : -1];
typedef char MR_Racer_off_poseBlend
        [(offsetof(MR_Racer, poseBlend) == 0x3051) ? 1 : -1];
typedef char MR_Racer_off_wheelieAngle
        [(offsetof(MR_Racer, wheelieAngle) == 0x307d) ? 1 : -1];
typedef char MR_Racer_off_crashLatch
        [(offsetof(MR_Racer, crashLatch) == 0x30c1) ? 1 : -1];
typedef char MR_Racer_off_subSegment
        [(offsetof(MR_Racer, subSegment) == 0x31b1) ? 1 : -1];
typedef char MR_Racer_off_trackTotalProgress
        [(offsetof(MR_Racer, trackTotalProgress) == 0x31b5) ? 1 : -1];
typedef char MR_Racer_off_progressHigh
        [(offsetof(MR_Racer, progressHigh) == 0x31b9) ? 1 : -1];
typedef char MR_Racer_off_segmentPrev
        [(offsetof(MR_Racer, segmentPrev) == 0x31bd) ? 1 : -1];
typedef char MR_Racer_off_segmentStart
        [(offsetof(MR_Racer, segmentStart) == 0x31c1) ? 1 : -1];
typedef char MR_Racer_off_wallNormal
        [(offsetof(MR_Racer, wallNormal) == 0x31c5) ? 1 : -1];
typedef char MR_Racer_off_flImpactBlend
        [(offsetof(MR_Racer, flImpactBlend) == 0x31d1) ? 1 : -1];
typedef char MR_Racer_off_stuntHeld
        [(offsetof(MR_Racer, stuntHeld) == 0x31f9) ? 1 : -1];
typedef char MR_Racer_prefix_ends
        [(offsetof(MR_Racer, isLocal) == 0x3209) ? 1 : -1];

/* Eight lap slots per racer - g_lapTimes is int[racer][8], indexed by
 * lapCounter. maybe_Race_TickClock adds the frame delta to
 * lapTimes[slot][lapCounter] every frame and zeroes all eight at the
 * start. */
#define RACE_LAP_SLOTS 8

/*
 * racer_place_from_grid - fill racers[1 .. count-1] from grid slots.
 *
 * Transcribes the tail of maybe_Game_InitRace as well as the
 * placement: the assigned segment is copied to the current one, and
 * the progress pair is seeded with a SIGNED form of it - a segment at
 * or past half the track length has the track length subtracted, so
 * that progress is lap-relative from the first frame rather than
 * jumping when the racer wraps.
 *
 * segmentCount is the track's, needed for that halving. reverse is
 * the race-direction flag.
 *
 * localIndex decides who gets humanPlayer - a network player is
 * not always slot 1.
 *
 * Returns the number of racers placed.
 */
int  racer_place_from_grid(MR_Racer *racers, int count,
                           const MR_GridSlot *slots, int segmentCount,
                           int reverse, int localIndex);

/*
 * racer_set_track_pos - place a racer at a fractional position along
 * the segment chain.
 *
 * segPos is a segment index plus a fraction and wraps at the end of
 * the chain; lateral is the lane offset, the same one the grid slot
 * carries, applied along the segment's LATERAL axis (matrix row 0 -
 * see game-object.txt 6.4).
 *
 * THIS IS NOT PHYSICS AND DOES NOT PRETEND TO BE. The bike is put
 * where the track says, with the track's orientation - no speed, no
 * grip, no lean, nothing the carac table describes.
 *
 * [open] NO CALLER since the autoSpeed rail was removed. Kept as
 * part of the placement trio racer.c names at the top, and because
 * a scripted placement is the obvious next use.
 */
void racer_set_track_pos(MR_Racer *r, const MR_Trk *t,
                         float segPos, float lateral);

/* The world distance from segment i to the next, for turning a speed
 * in units per second into a step in segPos. Returns 0 if the index
 * is out of range. */
float racer_segment_length(const MR_Trk *t, int seg);

/*
 * racer_ai_takeover / racer_restore_human - the post-race handover.
 *
 * ORIGIN  FUN_00412960 @ 0x00412960 and FUN_00412a50 @ 0x00412a50
 *
 * CLEARING humanPlayer IS THE WHOLE HANDOVER - the tick dispatch
 * already forks on it. The rest is seeding the AI from the speeds
 * the player arrived with so the bike does not lurch.
 *
 * race.c decides WHEN; see race_auto_ride_gate. The saved ceiling
 * lives with the sequence rather than the racer, because it has to
 * outlive the handover.
 */
void racer_ai_takeover(MR_Racer *r, float *savedRealSpeedMax);
void racer_restore_human(MR_Racer *r, float savedRealSpeedMax);

void racer_log(const MR_Racer *racers, int count, const char *what);


/* ------------------------------------------------------------------ */
/*
 * THE PER-RACER TICK - what Racer_TickDispatch forks to.
 *
 * race.c owns the fork; this owns what is on each side of it.
 * maybe_Racer_Tick (0x0045d580) is the HUMAN branch and
 * maybe_Racer_TickAI (0x0046bf80) is the AI one, and the single most
 * useful thing to know about them is that THEY DO NOT SHARE A PHYSICS
 * MODEL.
 *
 * THE HUMAN TICK IS DYNAMIC. THE AI TICK IS KINEMATIC.
 *
 * The human branch runs the chain physics.c implements - drive,
 * steering, cornering, the drivetrain, ground contact - and the
 * bike's position falls out of it.
 *
 * The AI branch calls NONE of that. Instead it moves the rider along
 * the track with FUN_0046b120, sets speed = engineSpeed directly,
 * SNAPS the bike to the ground (position[1] = ground height,
 * onGround = 1, airTime = 0) unless a stunt is running, and BUILDS
 * matOrientation FROM THE TWO WHEEL CONTACT POINTS sampled off the
 * track rather than from any accumulated state.
 *
 * So an AI rider is placed, not simulated. Consequences a port needs
 * up front: AI bikes cannot be made to behave by feeding them the
 * same physics, their airTime is pinned at zero so they never take
 * the 1.6 airborne drag, and any bug in the drivetrain is invisible
 * on them. It also explains why every live measurement session
 * watched racer 1 - hooking the drivetrain catches the player and
 * nothing else, because nothing else goes through it.
 *
 * See docs/subsystems/racers.txt 9.26.
 */

/*
 * racer_guard_dt - the human tick's own step guard, and it is NOT the
 * one race_scale_dt applies.
 *
 *     if ((int)dt < 1) dt = 1.0f;
 *
 * THAT IS THE FLOAT'S BIT PATTERN COMPARED AS A SIGNED INT. Bits < 1
 * means +0.0f or any negative, so it is "dt must be strictly
 * positive", and the replacement is again 1.0 - enormous next to the
 * 3.3 a normal frame gives.
 *
 * So the step is guarded TWICE on the way in, once in race_scale_dt
 * as a float compare and once here as an integer one, both
 * substituting the same absurd value. Reproduce both; they are cheap
 * and a port that keeps only one will differ on exactly the frames
 * that matter.
 */
float racer_guard_dt(float dt);

/*
 * racer_update_heading_ref - racer+0x2ea5, every frame.
 *
 *     if (!(inputWord & 2) || speed <= 0)
 *         if (headingRef >= 0)  headingRef = 0
 *         else                  headingRef += frameDelta * 0.0025
 *
 * MASK 2 IS BRAKE - bit 1, not bit 2; the two conventions read the
 * same way round for this one field and not for any other. The
 * recovery runs whenever the player is NOT braking, or is
 * essentially stopped.
 *
 * THE FIELD ONLY EVER GOES NEGATIVE. maybe_Racer_UpdateAirborne
 * pushes it down by frameDelta * -PI/1600 while the brake is held and
 * the bike is sliding, floored at -PI/16; this pulls it back up to
 * zero and then pins it there. A brake-induced lean that unwinds when
 * the brake is released. [inferred - the two writers and the clamp
 * are read, the visual meaning is not]
 */
void racer_update_heading_ref(MR_Racer *r, float frameDelta);

/*
 * racer_decay_fork_dive - racer+0x2e39, toward zero from BOTH sides
 * by a fixed 0.01 per frame, with overshoot snapped to exactly zero.
 *
 * PER FRAME, NOT SCALED BY dt - the third such constant in this
 * subsystem after the air timer's 2.592 and the wheelie's cap being
 * in scaled units.
 *
 * FUN_004667e0 pushes the field back out each frame from a
 * longitudinal acceleration with a peak-hold, so what a viewer sees
 * is this decay racing that. From its own +-PI/16 clamp it unwinds in
 * under twenty frames.
 *
 * IT IS FORK DIVE AND LIFT, NOT STEERING - proven with a trace hook
 * on the front wheel matrix, which rotates about X.
 *
 * THREE THINGS MAKE IT LOOK LIKE STEERING and none of them settle it:
 * the +-PI/16 clamp, the self-centring decay, and an apparent sign
 * reversal. Steering is racer+0x110, and it rotates about Y.
 */
void racer_decay_fork_dive(MR_Racer *r);

/*
 * racer_fx_gate - racer+0x2da0, which gates the wheel effects.
 *
 *     fxGate = onGround && fabs(slipAbs) >= 0.04
 *
 * so skid marks and spray need the bike both down and sliding, and
 * 0.04 m/s is a very low bar - well below a visible slide.
 */
int racer_fx_gate(int onGround, float slipAbs);

/*
 * racer_latch_stunt_request - the stunt button becomes the wheelie
 * request. maybe_Racer_Tick, at 0x0045d6db.
 *
 * stuntHeld (+0x31f9) is what the key handler writes;
 * stuntOrOverspeed (+0x2e85) is what racer_update_wheelie ramps on.
 * Nothing else connects the two, so leaving this out is a stunt
 * button that does nothing at all - and it looks exactly like a
 * broken wheelie rather than like missing wiring.
 *
 * Gated on the countdown, humanPlayer and soloMode - see the
 * comment on the definition for why each one is there.
 */
void racer_latch_stunt_request(MR_Racer *r, int countdown);

/*
 * The call order of maybe_Racer_Tick, as data rather than as a
 * comment so it can be walked and printed.
 *
 * THE ORDER IS THE DELIVERABLE. Each entry names the original
 * function, its address, and the condition guarding it, and NOTHING
 * HERE CALLS ANYTHING - it is a reference for the order, not a
 * dispatch table. Most of the seventeen are transcribed by now,
 * spread across racer.c, physics.c and racerdraw.c.
 *
 * A port that builds the chain in a different order gets subtly
 * different physics, because ground contact runs AFTER cornering
 * and the skeleton runs after everything.
 *
 * KEEP THE NAMES IN STEP WITH THE DB. They are data here, not
 * prose, so a stale FUN_ name is printed rather than merely read.
 */
typedef struct {
	const char *name;
	unsigned    address;
	const char *guard;      /* NULL when unconditional */
} MR_RacerTickStep;

/* Returns the table and writes its length to *count. */
const MR_RacerTickStep *racer_tick_order(int *count);


/*
 * The drivetrain's arguments, as racer_apply_drive computes them.
 *
 * THIS IS THE BRIDGE FROM INPUT TO PHYSICS. physics.c deliberately
 * knows nothing about buttons, raceState or the grid; this turns all
 * of that into the two numbers and the direction the drivetrain
 * actually consumes.
 */
typedef struct {
	/*
	 * matOrientation[9..11] + [6..8], normalised unless the length
	 * is exactly zero. ITS Y COMPONENT IS THE dirY THE SLOPE TERM
	 * USES - physics_slope's argument comes from here, which is
	 * what ties the two together. The UNNORMALISED sum is what the
	 * ghost recorder stores at record +0x14, so the two agree on
	 * the quantity. What it means geometrically is still not
	 * settled beyond "the bike's forward vector". [inferred]
	 */
	float dir[3];

	/*
	 * The drive demand as a numerator over a denominator, passed
	 * to the drivetrain exactly so. Its SIGN is what the drivetrain
	 * branches on: negative brakes, zero coasts, positive drives.
	 */
	int num;
	int den;

	/* The button word after the overrides below. */
	unsigned int buttons;
} MR_DriveDemand;

/*
 * racer_apply_drive - turn this tick's input into a drive demand.
 *
 * maybe_Racer_Tick calls the original only when crashLatch is clear.
 *
 * THE INPUT OVERRIDES, in order and before anything else:
 *
 *   o raceState not in {0,1} AND humanPlayer  ->  buttons = 2.
 *     A human who has FINISHED or TIMED OUT is forced to brake and
 *     nothing else. Not "input ignored" - the brake is HELD.
 *   o preRace                                 ->  buttons |= 2.
 *     Everyone brakes on the grid.
 *   o otherwise wheelieAngle != 0             ->  buttons |= 1.
 *     A WHEELIE IMPLIES THROTTLE - the bike cannot be on its back
 *     wheel and coasting.
 *   o brake bit set -> stuntOrOverspeed = 0, and stuntHeld = 0 for a
 *     human in solo. BRAKING CANCELS THE WHEELIE.
 *
 * THE ANALOG PATH IS NOT SYMMETRIC, and this is read out of the
 * disassembly at 0x00465ef9 rather than inferred:
 *
 *     n = 0x8000 - axis0
 *     n >= 0  ->  (n,        0x8000)
 *     n <  0  ->  (n / 2000, 0x8000)
 *
 * so one direction passes through and the other is divided by two
 * thousand, reducing a full deflection to about 0.0005 of the digital
 * unit. Whether that is deliberate or a defect is open - but it is
 * what the binary does, so it is what this does. [open]
 *
 * wheelieAngle is a parameter rather than read off r, though the only
 * caller passes r->wheelieAngle. The field is MR_Racer's own, at
 * +0x307d, which is where the original reads it too.
 *
 * IT CALLS THE DRIVETRAIN ITSELF, which is what the original does -
 * maybe_Racer_ApplyDrive to maybe_Physics_TickDrivetrain. Pass the
 * drivetrain and the gearbox and it runs the whole step; pass NULL
 * for d and you get the demand alone.
 */
void racer_apply_drive(MR_Racer *r, const MR_BikeCarac *c,
                       float wheelieAngle,
                       unsigned int buttons, int axis0, int preRace,
                       MR_DriveDemand *out,
                       MR_Drivetrain *d, const MR_Gearbox *gb,
                       float dtScaled);


/*
 * racer_spin_wheels - Racer_SpinWheels @ 0x00466940. Advances the two
 * wheel angles rearWheelSpin[0] and frontWheelSpin[0] by the
 * distance each contact point moved along the bike's forward axis,
 * over that wheel's radius.
 * racerdraw reads both and this is their only writer, so without it
 * the wheels stand still. Braking (input bit 1) locks them.
 */
void racer_spin_wheels(MR_Racer *r, const MR_BikeCarac *c,
                       float frameDelta);

/*
 * racer_apply_steering - the writer of steer (+0x110), and of
 * turnBrakeFlag.
 *
 * maybe_Racer_Tick calls the original only when crashLatch is clear
 * AND the grid has cleared - unlike drive, steering is dead on the
 * grid.
 *
 * axis1 IS THE STEERING AXIS; axis0 is throttle and goes to
 * racer_apply_drive. Three paths, chosen by whether axis1 is centred:
 *
 *   ANALOG    axis1 != 0x8000
 *       leanMode = 1
 *       n = (speed < 0.5556) ? 0 : axis1 - 0x8000
 *       steer = n * roueLimiteAngulaire / 0x8000
 *
 *   DIGITAL   centred axis, left or right held, and moving
 *       leanMode = 0
 *       if steer has the WRONG SIGN, snap it to 0 - no coast-over
 *       steerRate += roueAccAngulaire * dt
 *       steer     += steerRate * dt, clamped to +-roueLimiteAngulaire
 *
 *   CENTRING  otherwise
 *       steerRate = 0
 *       k = max(steerReturnScale * roueVitesseRetour / 6,
 *               roueVitesseRetour)
 *       step steer toward 0 by k*dt, snapping at the crossing
 *
 * YOU CANNOT STEER BELOW 2 km/h. The threshold is 0.5555556 m/s,
 * which is 2/3.6 exactly, and it gates all three: the analog path
 * zeroes its own input and the digital paths fall through to
 * centring. A stationary bike will not turn.
 *
 * THE ANALOG SCALE IS 1/0x8000, so the axis offset is normalised to
 * +-1 and multiplied by the lock stop. Both constants are exact
 * ratios rather than tuned numbers, which is worth noticing before
 * anyone "cleans them up".
 *
 * turnBrakeFlag IS SET TO 1 UNCONDITIONALLY FIRST, every racer every
 * frame, and only a HUMAN on a MotoType 1 bike reaches the code that
 * can clear it - while braking and turning together, or with axis1
 * outside 0x4000..0xc000. Below 2.0 m/s it is forced back to 1.
 *
 * IT GATES BRAKING. physics_brake_gated reads it: cleared, the whole
 * deceleration block is skipped. So the player on a dirt bike cannot
 * brake in a corner, while street bikes and every AI always can.
 * See racers.txt 9.21.4.
 *
 * THE STEERING TORQUE AT THE END OF THE ORIGINAL IS racer_steer_torque
 * plus the composition beside it - see THE TURN below. Its speed fade
 * is off VitesseMaxBoite, NOT RealSpeedMax, and is DISTINCT from the
 * visual fade in maybe_Racer_Draw. Do not merge the two.
 */
void racer_apply_steering(MR_Racer *r, const MR_BikeCarac *c,
                          unsigned int buttons, int axis1,
                          float slipAbs, float dtScaled);

/*
 * racer_place_wheels - maybe_Racer_PlaceWheels (0x0045c7c0), whole.
 *
 * Puts a racer at pos and places everything hanging off it, including
 * BOTH WHEEL INSTANCES at their rest offsets:
 *
 *     rear  = pos + M * (0, -96 * rayonAr, -48 * axeDistance)
 *     front = pos + M * (0, -96 * rayonAv, +48 * axeDistance)
 *
 * THIS IS NOT racer_wheel_ground. racer+0x1b4 and +0x338 are the
 * POSITION FIELDS of the two wheel model instances, so they hold a
 * rest OFFSET here and a world CONTACT once the ground fit has run.
 * The ground builder has no radius term, so using it to stand a bike
 * still leaves it about thirty units in the air.
 *
 * t may be NULL where no track is loaded - the segment lookup is
 * then skipped. The original always has one.
 */
void racer_place_wheels(MR_Racer *r, const MR_BikeCarac *c,
                        const float pos[3], const MR_Trk *t);


/*
 * racer_wheel_contacts - where the two wheels touch, in world space.
 *
 *     h     = HauteurCadre * 96
 *     rear  = pos + row3 + row1*(RoueRayonAr * -192 + h)
 *                        + row2*(RoueAxeDistance * -48)
 *     front = pos + row3 + row1*(RoueRayonAv * -192 + h)
 *                        + row2*(RoueAxeDistance * +48)
 *
 * row1 is matOrientation's UP axis and row2 its FORWARD, so the frame
 * height lifts both points and the wheelbase pushes them fore and
 * aft. The two 48s are the half-wheelbase either side of the root
 * joint; the 96 is the same mesh scale Load3dfAndMap uses, and -192
 * is twice it.
 *
 * THIS IS THE INPUT TO ALMOST EVERYTHING ELSE. The orientation matrix
 * is rebuilt by crossing the two contact points' ground heights, and
 * that matrix is what physics_camber samples across, what
 * physics_slip_speed dots against and where physics_slope's dirY
 * comes from. Get these two points wrong and every one of those is
 * wrong downstream.
 *
 * THE SAME ARITHMETIC APPEARS TWICE in the original - here and in
 * maybe_Racer_TickAI - which is why it is worth having once.
 *
 * THIS IS NOT THE PAIR THE RACER STORES, and the name oversells
 * it. racer+0x1B4 and racer+0x338 hold the GROUND contacts, and
 * they carry no radius term at all - see racer_wheel_ground. The
 * two coincide for the front wheel of every shipped bike, because
 * hauteurCadre is exactly twice rayonAv and the up term cancels,
 * which is how the difference stayed hidden: only the rear wheel
 * shows it, and only by the 96 * (hauteurCadre - 2 * rayonAr) that
 * the bigger rear tyre leaves behind.
 *
 * What this pair actually is, then, is a CHASSIS reference - where
 * each tyre's bottom would sit if the frame were level with both
 * axles at one height. That is the right thing to sample terrain
 * with and the wrong thing to draw from.
 */
void racer_wheel_contacts(const MR_Racer *r, const MR_BikeCarac *c,
                          float rearOut[3], float frontOut[3]);


/*
 * racer_wheel_ground - the pair the racer actually stores.
 *
 *     rear  = pos + row2 * (axeDistance * -48)      racer+0x338
 *     front = pos + row2 * (axeDistance * +48)      racer+0x1B4
 *
 * No up term. Both points sit at the racer's own height, on the
 * forward axis, half a wheelbase either side. Measured live on the
 * street bike:
 *
 *     rear  - pos   (+83.9185, 0.0000, -3.6997)   |.| = 84.0000
 *     front - pos   (-83.9186, 0.0000, +3.6997)   |.| = 84.0000
 *
 * with 48 * axeDistance = 84.0 exactly. maybe_Racer_Draw rebuilds
 * the front one from the rear at 0x0045f87c as
 * rear + normalize(front - rear) * 96 * axeDistance, which is the
 * same relation stated the other way round and confirms there is
 * no vertical component between them.
 *
 * IT IS SYMMETRIC BY CONSTRUCTION AND THE STORED PAIR IS NOT.
 * racer_update_ground_contact - import 1, whole - fits racer+0x1B4
 * and +0x338 to the terrain, so on a slope those two sit at
 * different heights while this pair is always mirrored about pos.
 * ANYTHING THAT WANTS THE REAL CONTACTS MUST READ THE FIELDS; this
 * computes where they would sit on the flat.
 */
void racer_wheel_ground(const MR_Racer *r, const MR_BikeCarac *c,
                        float rearOut[3], float frontOut[3]);


/* ------------------------------------------------------------------ */
/*
 * POSE AND EFFECTS, and the two originals here have DIFFERENT
 * transcription status - do not read one from the other.
 *
 * maybe_Racer_UpdateSkeleton (0x00467b30) IS SLICED. Only its slot
 * choice is here; the pose fill needs maybe_Racer_SelectLod and the
 * g_abPilotRot rest-pose table, neither of which is read.
 *
 * maybe_Racer_EmitWheelFx (0x00466d70) IS WHOLE, with three callees
 * still stubbed and each marked at its own site - the surface lookup,
 * the trail spawner and the particle spawner.
 *
 * What the slices here capture is the DECISIONS - which slot, whether
 * to emit at all, how wide, how many - because those are
 * self-contained, cheap to get wrong, and the part a port has to
 * match for the game to look right rather than merely work.
 *
 * EmitWheelFx OPENS WITH A COPY-PROTECTION CHECK, deliberately not
 * reproduced. It is the one of the four sites with NO field payload,
 * so skipping it changes nothing a racer can see - see
 * docs/subsystems/copy-protection.txt 5.2, check B.
 */

/*
 * racer_select_skeleton_slot - which rider skeleton to pose.
 *
 *     slot = (crashLatch || handSkeleton) ? 1 : lodSlot
 *     ... the pose is filled only when slot < 2 ...
 *     if (handSkeleton) slot = 1
 *
 * handSkeleton FORCES SLOT 1 TWICE, before and after the fill, and
 * the second store is NOT redundant: maybe_Racer_SelectLod cannot
 * have run in that case, but the pose update can have moved the slot
 * underneath. Reproduce both.
 *
 * lodSlot is maybe_Racer_SelectLod's result, which is unread - the
 * caller supplies it.
 */
int racer_select_skeleton_slot(int crashLatch, int handSkeleton,
                               int lodSlot);

/*
 * racer_fx_in_range - whether this racer emits wheel effects at all.
 *
 * Nothing is emitted unless the racer's segment lies within
 * [viewed - 10, viewed + 30] around the viewed racer's segment,
 * wrapped. SO EFFECTS EXIST ONLY NEAR THE CAMERA, and the forward
 * extent is 30 segments - the same number the proximity flag at
 * racer+0x0 switches on, MR_Racer.txt 23.3.
 *
 * The original wraps each bound through Trk_WrapSegmentIndex, which
 * is unread; this computes the signed wrapped difference instead,
 * which is equivalent on a closed track and clearer. [inferred - the
 * bounds are read, the wrap helper is not]
 */
int racer_fx_in_range(int segment, int viewedSegment, int segmentCount);

/*
 * racer_skid_width - the skid ribbon half-width, racer+0x188.
 *
 *     widen  by +2.0 while |lateral| exceeds skidWidthPrev
 *     shrink by -1.0 otherwise
 *     clamp to [1.0, 12.0]
 *
 * SO IT WIDENS AS THE SLIDE WORSENS AND NARROWS TWICE AS SLOWLY as
 * it opens. Pass the new |lateral| back as prevLateralAbs next frame;
 * the original stores it in racer+0x184.
 *
 * AN AI IS PINNED AT 5.0 and never runs the update at all - so an AI
 * bike lays a constant-width ribbon however hard it is sliding.
 *
 * The +2.0 is 0x004d0e88, THE SAME .rdata DOUBLE that doubles the
 * centripetal term in physics_centripetal. Third shared constant in
 * the subsystem, after the air step and the 0.01 small-step.
 */
float racer_skid_width(float width, float prevLateralAbs,
                       float lateralAbs, int humanPlayer);

/*
 * racer_fx_particle_count - the spray particle count.
 *
 *     humanPlayer ? 24 : 4
 *
 * A straight level-of-detail cut, and a six-to-one one. Worth
 * reproducing: matching it is most of why a crowded field looks like
 * the original rather than like a particle demo.
 */
int racer_fx_particle_count(int humanPlayer);

/*
 * racer_emit_wheel_fx - skid marks, tyre trail and spray. Called once
 * per racer per tick, and only when the ground contact flag is set.
 * The ribbon path is complete; see the section note above for what is
 * still stubbed.
 */
void racer_emit_wheel_fx(MR_Racer *r, unsigned int buttons,
                         const MR_Trk *trk, int viewedSegment,
                         int viewedIndex, float frameDelta);


/*
 * racer_update_fork_dive - maybe_Racer_UpdateForkDive (0x004667e0).
 * It writes TWO fields, and neither is what its name suggests.
 *
 * 1. steerReturnScale (+0x0f8) IS THE LONGITUDINAL ACCELERATION:
 *
 *        accel = (longSpeed - speedTail) / (frameDelta * 0.01)
 *
 *    a delta-speed over a delta-time. That closes what the field is -
 *    racer_apply_steering multiplies the self-centring rate by it, so
 *    THE BARS SELF-CENTRE FASTER UNDER ACCELERATION OR BRAKING and at
 *    the plain rate while coasting.
 *
 * 2. forkDive (+0x2e39) IS A PEAK HOLD OF THAT ACCELERATION:
 *
 *        a = ((longSpeed - speedTail) + prevAccel) * 0.5
 *        if (|forkDive| < |a * -PI/32|)  forkDive = a * -PI/32
 *        clamp to +-PI/16
 *
 *    THE SCALE IS EXACTLY HALF THE CLAMP, so an acceleration of 1.0
 *    gives half deflection and 2.0 saturates. THE SIGN IS NEGATIVE:
 *    accelerating lifts the front, braking dives it.
 *
 *    IT ONLY EVER GROWS HERE. racer_decay_fork_dive shrinks it by a
 *    fixed 0.01 per frame, so what a viewer sees is this peak hold
 *    racing that decay - which is why 9.26.3 had to describe them
 *    together.
 *
 * prevAccel is racer+0x0f4, PER RACER, and is updated in place.
 *
 * NOT TRANSCRIBED: the two per-wheel air timers the original also
 * keeps, advanced by frameDelta * 6.272 while the matching
 * onGroundPair flag is clear. They live inside the wheel sub-records
 * and nothing has read them. [open]
 */
void racer_update_fork_dive(MR_Racer *r, float longSpeed,
                            float speedTail, float *prevAccel,
                            float frameDelta);


/*
 * racer_begin_crash - throw the rider off the bike. ONE-SHOT.
 *
 * maybe_Racer_BeginCrash (0x00467e30). It returns immediately if
 * crashLatch is already set, and setting that flag IS the latch - so
 * a rider who is already down cannot be re-crashed, and every
 * consumer downstream keys off the same bit.
 *
 * The racer-state half is here and the ballistics half is
 * physics_crash_begin; the split follows rule 13 - this touches one
 * MR_Racer, that produces a velocity.
 *
 * WHICH SIDE HE FALLS COMES FROM THE LEAN, and it is a three-step
 * fallback:
 *
 *     racer+0x2e31 if non-zero      sign decides
 *     else lean    if non-zero      sign decides
 *     else the RNG                  a coin toss
 *
 * so a rider dropped mid-corner always goes over the way he was
 * already leaning, and only a dead-upright crash is random. fallSide
 * is +pi/2 or -pi/2 and becomes two of the three tumble rates.
 *
 * WHAT racer+0x2e31 IS remains unread - it is checked ahead of lean
 * and behaves like one, so it is probably a commanded or target lean.
 * Getting it wrong only picks the wrong side of a crash. [open]
 *
 * dir is the impact direction in world space and need not be
 * normalised. Returns non-zero if this call latched the crash, zero
 * if one was already running.
 */
int racer_begin_crash(MR_Racer *r, MR_Gearbox *gb, const float dir[3]);


/*
 * racer_reset_crash - put ONE racer back on the bike.
 *
 * The per-racer half of FUN_004681d0 (0x004681d0), which is both
 * maybe_Racer_UpdateCrash's early-out and what it calls when the crash
 * has finally run down - so this is the remount, and it is what
 * clears crashLatch.
 *
 * THE TWO PATHS DIFFER, and not in the direction you would guess:
 *
 *     humanPlayer  collideSpeed = 0, speed (+0x124) = 0,
 *                  SetLongSpeed(0) - the bike is STOPPED - then
 *                  engineSpeed is rebuilt from the gear through
 *                  SpeedToEngine. A real gearbox resync.
 *
 *     AI           engineSpeed = 0, aiRoadSpeed = 0,
 *                  flImpactBlend = 200.0. No gearbox work at all.
 *
 * The AI gets the cheap version and the 200.0 recovery counter; the
 * human gets the expensive one. That is the same split
 * maybe_Racer_UpdateCrash's tail makes, so the two functions agree on
 * which racer deserves the arithmetic.
 *
 * Both paths clear crashLatch and crashTimerMs, and both raise
 * g_crashResetSignal.
 *
 * d, gb and carac are only read on the human path and may be NULL for
 * an AI racer, which is what the cascade passes. Returns non-zero if a
 * reset actually happened.
 *
 * THE GATE IS THE CALLER'S, not this function's - see
 * race_reset_crashes. This does the work unconditionally once asked.
 */
int racer_reset_crash(MR_Racer *r, MR_Drivetrain *d, MR_Gearbox *gb,
                      const MR_BikeCarac *carac);


/* ------------------------------------------------------------------ */
/*
 * GROUND CONTACT - maybe_Racer_UpdateGroundContact (0x00468ca0),
 * which racer.c imports WHOLE as import 1.
 *
 * What follows are the two pieces worth stating away from it: the
 * contact test, and the shared frame construction both of its arms
 * end with. Both are read off the writes, not inferred.
 */

/* ------------------------------------------------------------------ */
/*
 * THE matOrientation REBUILD. Both arms of
 * maybe_Racer_UpdateGroundContact end here, and 9.29.3 was right that
 * the matrix is built two different ways - but they turn out to be
 * the SAME construction with a different forward vector:
 *
 *     forward = normalize(source)
 *     lateral = normalize(worldUp x forward)
 *     up      = normalize(forward x lateral)
 *     rows    = { lateral, up, forward }
 *
 *     GROUNDED  source = frontContact - rearContact, both snapped
 *               to the ground under their OWN segments
 *     AIRBORNE  source = racer+0x18c, the movement direction
 *
 * SO THE BIKE CAN NEVER ROLL FROM THE TERRAIN. lateral comes from
 * worldUp every time, so the frame has pitch and yaw and no roll at
 * all - which is exactly what MR_Racer.txt meant by "pitch and yaw
 * come from the two wheel contact points, roll cannot". The lean is
 * composed in later, by a different matrix.
 *
 * THE ROW ORDER IS CONFIRMED, not assumed. The stores at
 * 0x004696f2-0x00469746 put lateral at racer+0xbc, up at +0xc8 and
 * forward at +0xd4, which is what physics.h has been claiming about
 * row1 and row2 on the strength of how they are used. Now it is read
 * off the writes.
 */

/*
 * racer_frame_from_forward - the shared construction above.
 *
 * m is the 12-float matOrientation; only rows 0..2 are touched, the
 * translation is left alone. forward need not be normalised.
 *
 * A DEGENERATE forward - zero length, or exactly parallel to world up
 * - leaves the matrix untouched and returns 0. The original does not
 * guard this: it would produce a zero lateral and Vec3_Normalize's
 * own divide-by-zero check would leave that zero, giving a singular
 * matrix. Ours refuses instead, because a singular matOrientation
 * poisons physics_camber, physics_slip_speed and physics_slope all at
 * once and the failure would surface far from here. THIS IS A
 * DELIBERATE DIVERGENCE, not a transcription of the original. [open]
 */
int racer_frame_from_forward(float m[12], const float forward[3]);

/* ------------------------------------------------------------------ */
/*
 * PIECES OF maybe_Racer_UpdateGroundContact (0x00468ca0), declared
 * separately because each is testable on its own. THE FUNCTION
 * ITSELF IS IMPORTED WHOLE in racer.c - its header carries the one
 * hole that remains and the known divergence.
 *
 * 4026 bytes covering the human player's whole relationship with the
 * ground: the wall clip, the terrain snap, the matOrientation rebuild
 * and the decision that a wall put the rider down. racers.txt 9.29.6
 * through 9.29.15 is the read.
 *
 * IT IS NOT THE MOVER, which is the thing to keep straight. The
 * position has already advanced by the time it runs -
 * ModelInstance_Integrate did that, on racer+0x24 - so it CORRECTS a
 * position rather than producing one. racers.txt 9.29.11.
 */

/* ------------------------------------------------------------------ */
/*
 * THE TURN, which is the last link in the steering chain.
 *
 * racer_steer_torque BELOW CARRIES THE WHOLE TAIL - the speed fade,
 * the wheelie halving, the grip term with its 1.6
 * brake-into-a-corner cap, and the AI branch - under the same ORIGIN.
 * What is composed here is the GENERAL form rather than the Y-only
 * one, which is the only reason both exist.
 *
 *     yaw = -(grip * factor * dt * K * steer)   racer_steer_torque
 *     matOrientation = Ry(yaw) * matOrientation
 *
 * THE YAW IS AN INCREMENT. It scales with dt, so it can only be a
 * per-frame delta - and Racer_ComposeOrientation multiplies rather
 * than assigns, which is what makes that work.
 *
 * r->steering IS A PHYSICS INPUT, NOT DRAW-ONLY, and an operand
 * search says otherwise: ApplySteering reaches it through a POINTER
 * at 0x00466409 and multiplies at 0x00466625, neither of which a
 * displacement search can see. racers.txt 9.29.23.
 */

/*
 * racer_wall_impact_cos - how square the hit was.
 *
 * dot(wallNormal, travel), both of them out parameters of the wall
 * clip. The callers take |.| by masking the sign bit, so the sign
 * carries no information: 1 is head-on, 0 is a graze.
 */
float racer_wall_impact_cos(const float wallNormal[3],
                            const float travel[3]);
/*
 * racer_orient_airborne - rebuild from the direction of travel.
 *
 * ORIGIN site 0x0046974b. The original skips the rebuild entirely
 * when racer+0x124 is +-0.0, written as TEST EAX,0x7fffffff on the
 * bit pattern - a stationary bike in the air keeps whatever
 * orientation it had, because a direction cannot be recovered from a
 * zero velocity. That test is the caller's; this function only does
 * the rebuild.
 *
 * moveDir is racer+0x18c, already a unit vector in practice. The
 * original still runs it through Vec3_Sub against a zeroed temporary
 * and then normalises it - a copy written as a subtraction.
 */
int racer_orient_airborne(MR_Racer *r, const float moveDir[3]);

/*
 * racer_orient_grounded - rebuild from the two wheel contacts.
 *
 * ORIGIN site 0x00469582. front and rear come from
 * racer_wheel_contacts and are modified IN PLACE: each has its Y
 * replaced by the ground height under it.
 *
 * EACH POINT IS SAMPLED IN ITS OWN SEGMENT, not the racer's. The
 * original re-runs Trk_FindSegmentForPoint for the front and rear
 * points separately before calling Trk_GroundHeightAt on each
 * (0x00469609-0x0046965b), so a bike straddling a segment boundary
 * gets a front wheel on one piece of road and a rear wheel on the
 * other. That is what lets the frame pitch correctly across a crest
 * instead of snapping when the racer's own segment index changes.
 *
 * The re-find is itself skipped by the original when the tight
 * clearance is asked for and an internal flag is clear
 * (0x00469601), which is a third thing that parameter does and is
 * still unexplained.
 */
int racer_orient_grounded(MR_Racer *r, const MR_Trk *t, int segment,
                          float front[3], float rear[3]);
/* ------------------------------------------------------------------ */
/*
 * THE AI TICK. maybe_Racer_TickAI (0x0046bf80), the other arm of
 * Racer_TickDispatch's fork on humanPlayer, and it shares no physics
 * with the human one - see THE PER-RACER TICK above for the contrast
 * and for what it costs a measurement.
 *
 * racer.c HOLDS THE TICK ITSELF, with its remaining holes marked in
 * place. What is declared here are the pieces more than one caller
 * needs: the lean step, the wall response and the ground snap.
 */

/*
 * MR_LeanDbg - the three yaws racer_update_lean publishes.
 *
 * DIAGNOSTICS, NOT STATE. race.c logs them and tools/mr_place.py
 * reads them; the lean does not depend on any of them. Pass NULL to
 * discard.
 */
typedef struct {
	float yawOrient;   /* yaw(matOrientation) - the body heading  */
	float yawFrame;    /* yaw(the travel frame) - where it goes   */
	float leanDelta;   /* the wrapped difference the lean is from */
} MR_LeanDbg;

/*
 * racer_update_lean - maybe_Racer_UpdateLean (0x00469c60), the body.
 *
 * Called by the player tick and by racer_tick_ai, which is why it is
 * here and not inline in race_tick_physics. See racer.c.
 */
void racer_update_lean(MR_Racer *r, const MR_BikeCarac *c,
                       const MR_Trk *t, float driveSpeed, float dtMs,
                       int replayMode, MR_LeanDbg *dbg);

/*
 * MR_AiEnv - what TickAI reads that is NOT on the racer.
 *
 * The original takes none of it: g_bPreRace and g_nRaceDirection are
 * globals, the roster is one, and the viewed pair are read off the
 * camera inside maybe_Racer_EmitWheelFx. This port passes them down,
 * which is the convention the rest of racer.c follows -
 * racer_emit_wheel_fx takes the same viewed pair. Bundled rather than
 * spread across the parameter list because they travel together.
 */
typedef struct {
	int preRace;         /* g_bPreRace 0x004dc0b4 - no wheelie on   */
	                     /* the grid                                */
	int viewedSegment;   /* the CAMERA's racer, not this one - the  */
	int viewedIndex;     /* fx range test is camera-relative        */
	/*
	 * MILLISECONDS, NOT THE SCALED STEP. racer_update_lean divides
	 * by it and the player's call site passes ms; dtScaled is the
	 * 3.3-per-tick value and would be wrong here by a factor of ten.
	 */
	float dtMs;
	/*
	 * g_nRaceDirection (0x004dc140), +1 or -1 - NOT a boolean. It
	 * scales the line follower's look-ahead, so the sign is what
	 * sends an AI round the track the right way.
	 */
	int raceDir;
	/*
	 * THE ROSTER, because the lane logic reads every other racer's
	 * lane, segment and speed to decide this one's.
	 */
	MR_Racer *racers;
	int       racerCount;
	/*
	 * THE REVERSE FLAG, 0x004dc144 - the race's own latched copy of
	 * it, not a separate setting. It swaps the lane windows from
	 * ahead to behind, which is right: running the track backwards
	 * makes a higher segment index the way the bike came from.
	 */
	int mirrorWindow;
	/*
	 * g_nSelectedMode. ONLY THE REMOTE TICK READS IT: mode 0
	 * takes a peer's reported position outright where every
	 * other mode interpolates toward it. racer_tick_ai ignores
	 * this field.
	 */
	int selectedMode;
} MR_AiEnv;

/*
 * racer_tick_ai - maybe_Racer_TickAI (0x0046bf80), WHOLE. See racer.c
 * for the twelve numbered steps and the holes still to fill.
 *
 * d MAY BE NULL. The original scales a speed through racer+0x198 as
 * well as racer+0xec on a wall hit; this port has one drivetrain and
 * it belongs to racer 1, so an opponent passes NULL and only its own
 * engineSpeed is scaled. [open - racer+0x198]
 *
 * RETURNS racer_update_crash's verdict - RACER_CRASH_DONE when the
 * rider has finished his flight and wants putting back on the bike.
 * The caller has to act on it; only race.c can.
 */
int racer_tick_ai(MR_Racer *r, const MR_BikeCarac *c, const MR_Trk *t,
                   MR_Drivetrain *d, const MR_AiEnv *env, float dtScaled);

/*
 * racer_remote_apply_state - FUN_0045e560 (0x0045e560), WHOLE.
 *
 * Consumes the network state a peer reported: steers the
 * orientation 0.9 of the way toward aimDir and dead reckons the
 * position toward it. placeWheels0/1/2 are the network slots, not
 * wheel placement - docs/subsystems/netplay.txt 5.
 *
 * selectedMode 0 ASSIGNS THE POSITION instead of interpolating,
 * and that is also the time-attack ghost's arm.
 */
void racer_remote_apply_state(MR_Racer *r, const MR_Trk *t,
                              int selectedMode, float dtScaled);

/*
 * racer_tick_remote - maybe_Racer_TickRemote (0x0045e940), WHOLE.
 *
 * The third tick: a racer this machine does not drive. The same
 * integrator as racer_tick_ai with the AI driver replaced by
 * racer_remote_apply_state, so the ground, the orientation rebuild
 * and the whole tail are shared with it - the original duplicates
 * them too.
 *
 * NOT WIRED YET. Race_TickAllRacers reaches the original from two
 * places, a network peer and the time-attack ghost, and this port
 * has neither. [open]
 *
 * RETURNS racer_update_crash's verdict, as racer_tick_ai does.
 */
int racer_tick_remote(MR_Racer *r, const MR_BikeCarac *c,
                      const MR_Trk *t, const MR_AiEnv *env,
                      float dtScaled);

/*
 * racer_ai_lean_step - maybe_Racer_TickAI at 0x0046c62a.
 *
 * Kept separate from the body above because it is the one block of
 * TickAI with a verified measurement behind it and a correction on
 * record: the multipliers are ABOVE 1, so the lean is driven OUT to
 * the clamp, not decayed to zero. racers.txt 9.47.3.
 */
void racer_ai_lean_step(MR_Racer *r, int persoAnim);


/* ------------------------------------------------------------------ */
/*
 * THE LINE FOLLOWER. FUN_0046b120 (0x0046b120), 3005 bytes, and the
 * only place the AI actually makes a decision. maybe_Racer_TickAI
 * calls it once per step and everything else in that function reacts
 * to what it did.
 *
 * IT IS A PURSUIT STEERING MODEL, not a controller emulator. Each
 * step it picks a target point on the racing line ahead, lerps the
 * movement direction toward it, blends the speed by how well aligned
 * the bike already is, and integrates. No throttle, no brake, no
 * steering angle - the AI never touches the player's inputs.
 *
 * THE ONE INPUT IT DOES SYNTHESISE is tickInputWord = 1 at the very
 * top, which is "accelerate held". That is the value
 * maybe_Racer_EmitWheelFx sees before TickAI restores the entry
 * value (9.47), so an AI bike throws up dust as if its rider were on
 * the gas - which he always is.
 *
 * THE TARGET POINT is segment.origin + row0 of its matrix, scaled by
 * (racingLine + chargeMeter). Row0 is the segment's LATERAL axis, so
 * the segment's +0xb0 is a PER-SEGMENT RACING-LINE OFFSET - trk.h
 * carries it - and chargeMeter is this rider's own displacement from
 * it.
 */

/*
 * racer_ai_follow_line - Racer_AiFollowLine (0x0046b120), WHOLE.
 *
 * The only place the AI makes a decision. Picks a point on the racing
 * line ahead, turns toward it, blends speed by alignment, integrates.
 * See racer.c for the thirteen steps and the one hole.
 */
void racer_ai_follow_line(MR_Racer *r, const MR_BikeCarac *c,
                          const MR_Trk *t, const MR_AiEnv *env,
                          float dtScaled);

/*
 * racer_ai_lane_step - FUN_0046cd20 (0x0046cd20), whole.
 *
 * THE LANE LOGIC, and chargeMeter is its output - the rider's
 * lateral offset from the racing line, ramped toward lane * 240.
 * Three lanes 240 apart is what spreads a field across the road.
 * See racer.c for the seven steps.
 */
void racer_ai_lane_step(MR_Racer *racers, int count, MR_Racer *r,
                        const MR_Trk *t, int raceDir, int mirrorWindow,
                        float dtScaled);

/*
 * racer_lane_of_position - maybe_Trk_LaneOfRacer (0x0046d340), whole.
 *
 * Which lane the racer is STANDING in, measured off its position.
 * The player's lane is read back this way; an AI's is chosen. CAN
 * RETURN -1 for a bike in a gap between the bands, and the original
 * stores that without comment.
 */
int racer_lane_of_position(const MR_Racer *r, const MR_Trk *t);

/*
 * racer_ai_is_corner - FUN_0046c7a0 (0x0046c7a0), whole.
 *
 * 1 when the track BENDS within `ahead` segments, 0 on a straight.
 * The caller uses it to suppress: an AI does not pull wheelies
 * through bends. See racer.c.
 */
int racer_ai_is_corner(const MR_Trk *t, int segment, int ahead,
                       int raceDir);

/*
 * racer_update_dash - FUN_00467930 (0x00467930), with one hole.
 *
 * The speedometer is MEASURED off the rear tyre's displacement, not
 * taken from engineSpeed, and an AI's needle is damped where the
 * player's is not. See racer.c.
 */
void racer_update_dash(MR_Racer *r, const MR_BikeCarac *c,
                       float frameDelta);

/*
 * The gearbox shift latch - the ONLY cross-frame state a physics
 * tick keeps outside MR_Racer. Exposed so race_autopilot_probe can
 * save and restore it around a rollout; nothing else should touch
 * it. See racer.c.
 */
int  racer_shift_latch_get(void);
void racer_shift_latch_set(int v);

/* Racer_AiFollowLine at 0x0046b1e8 - the point on the line ahead. */
void racer_ai_target_point(const MR_Trk *t, int seg, float chargeMeter,
                           float out[3]);

/* Racer_AiFollowLine at 0x0046b2b0 - fold into [0, 2pi). */
float racer_ai_wrap_turn(float a);

/*
 * racer_ai_top_speed - the AI's speed cap, spread by lane.
 *
 *     realSpeedMax + lane * realSpeedMax * 0.025
 *
 * TWO AND A HALF PER CENT PER LANE, which is how a field of
 * identical bikes ends up strung out instead of driving as one
 * block. It is applied twice - once clamping the throttle ramp and
 * once clamping the blended speed.
 */
float racer_ai_top_speed(float realSpeedMax, int lane);

/*
 * TWO DIFFERENT DELTAS LIVE IN THE AI TICK, and mixing them is a
 * factor of ten. racer_ai_follow_line and every helper below take the
 * SCALED delta - 3.3 at a 33 ms step, the value TickAI is handed, and
 * their parameters are named dtScaled. racer_update_lean and
 * MR_AiEnv.dtMs take MILLISECONDS and say so.
 *
 * racer_ai_throttle_step - ramp the AI's TARGET road speed.
 *
 *     target += dt * (1/6)
 *     if (target > racer_ai_top_speed(...)) target = that
 *
 * Skipped entirely while crashed, and while pre-race.
 */
float racer_ai_throttle_step(float target, float dtScaled,
                             float realSpeedMax, int lane);

/*
 * racer_ai_blend - the impact-recovery blend factor, and what
 * flImpactBlend (+0x31d1) IS FOR.
 *
 *     alpha = flImpactBlend * 0.005, floored at 0.01
 *
 * 0.005 IS 1/200 AND THE FIELD IS SET TO EXACTLY 200.0 ON AN IMPACT,
 * so alpha is 1.0 the instant after a hit and decays to the 0.01
 * floor as the counter runs down. It weights the OLD state against
 * the new in both the speed blend and the direction lerp:
 *
 *     alpha = 1     ignore the racing line completely
 *     alpha = 0.01  follow it almost exactly
 *
 * WHICH MEANS A WALL HIT MAKES AN AI STOP STEERING for a while and
 * coast on whatever heading it was left with - which is why the wall
 * paths set 200.0. The units are "fraction of the way back to
 * normal, times 200". [verified]
 */
float racer_ai_blend(float flImpactBlend);

/*
 * racer_ai_speed_blend - the AI's cornering model, done kinematically.
 *
 *     c = |dot(moveDir, forward)|, floored at 0.5
 *     w = forward * (target * c) + moveDir * ((1 - c) * speed)
 *     mag = min(|w|, racer_ai_top_speed(...))
 *     speed = alpha*speed + (1 - alpha)*mag
 *
 * MISALIGNMENT COSTS SPEED, and that is the whole of the AI's
 * cornering. c is 1 when the bike points where it is going and falls
 * toward the 0.5 floor as it slides; the target speed is scaled by
 * it, so a sideways AI simply cannot reach its cap. No grip curve,
 * no slip angle, no centripetal term - one dot product.
 *
 * forward is matOrientation row2. Returns the new speed.
 */
float racer_ai_speed_blend(float speed, float target, float alpha,
                           const float moveDir[3],
                           const float forward[3],
                           float realSpeedMax, int lane);

/*
 * racer_ai_steer_toward - the lerp that IS the following.
 *
 *     moveDir = moveDir*alpha + toTarget*(1 - alpha)
 *     normalise
 *
 * toTarget is the unit vector from the bike to the target point.
 * Skipped while crashed - a downed AI keeps its heading.
 */
void racer_ai_steer_toward(float moveDir[3], const float toTarget[3],
                           float alpha);

/*
 * racer_ai_integrate - move the bike.
 *
 *     position += moveDir * speed * dt * 1.28
 *
 * 1.28 IS 128 * 0.01 - the same launch scale and the same 1/100
 * metre step physics_crash_tick uses, folded into one constant. A
 * third place that pairing turns up, after the crash ballistics and
 * the gravity terms.
 *
 * Skipped while pre-race, which is what pins the AI to the grid.
 */
void racer_ai_integrate(float pos[3], const float moveDir[3],
                        float speed, float dtScaled);


/* ------------------------------------------------------------------ */
/*
 * THE LEAN. maybe_Racer_UpdateLean (0x00469c60), 4266 bytes, and it
 * is FOUR different formulas behind three flags - persoAnim,
 * humanPlayer && soloMode, and the replay mode. It also rebuilds the
 * wheel contacts and matBikeRoot, with a wheelie rotation folded in.
 *
 * racer.c CARRIES THE BODY as racer_update_lean, and the pieces
 * lifted out of it are declared here and below -
 * racer_lean_impact_decay, racer_lean_response,
 * racer_wheel_contacts_m and racer_wheelie_swing_contact. The lean
 * paths none of those cover are in racers.txt 9.49. [open]
 */

/*
 * racer_lean_impact_decay - where impactBlend actually counts down.
 *
 *     if (impactBlend != 0) {
 *         impactBlend -= frameDelta
 *         if (impactBlend < 0) impactBlend = 0
 *     }
 *     returns impactBlend * 0.005
 *
 * THE SETTER, THE READER AND THE DECREMENTER ARE THREE DIFFERENT
 * FUNCTIONS, which is what makes this one easy to miss: every wall
 * path sets 200.0, Racer_AiFollowLine consumes impactBlend * 0.005,
 * and the countdown is HERE, at the very top of UpdateLean, using the
 * same 0x004d100c (1/200) to normalise.
 *
 * SO THE RECOVERY IS FRAME-RATE COUPLED, not tick-count based: the
 * counter falls by g_frameDelta, so an impact fades over a fixed
 * amount of GAME TIME rather than a fixed number of frames.
 *
 * The returned value is used here as a floor on the lean's own
 * blend, which is why a bike that has just hit something leans less
 * readily.
 */
float racer_lean_impact_decay(MR_Racer *r, float frameDelta);

/*
 * racer_lean_delta_live / racer_lean_atan - THE LEAN THE GAME USES.
 *
 *     yawDelta = wrap(yawOrient - yawFrame)          a plain wrap
 *     lean     = atan((-yawDelta / dt) * min(speed, 50) * 1.4459)
 *                clamped to +-1.04
 *
 * Verified by hook against four consecutive donut frames, exact to
 * five decimals. See racer.c.
 *
 * racer_lean_response and racer_lean_delta BELOW ARE THE OTHER
 * BRANCH AND IT IS DEAD - a hook on its store took zero hits across
 * donut, straight, braking, wheelie and turning. They are kept
 * transcribed and uncalled. Do not wire them back in.
 */
float racer_lean_delta_live(float yawOrient, float yawFrame);
float racer_lean_atan(float yawDelta, float dt, float speed);

/*
 * racer_lean_ai - maybe_Racer_UpdateLean at 0x0046abb7.
 *
 * The THIRD lean curve: what a PersoAnim bike uses when it is not the
 * human solo player.  lean = yawDelta * -1.3 / (3.0 - speed * 0.0225),
 * no clamp.  See racer.c for the three-way gate that routes to it.
 */
float racer_lean_ai(float yawDelta, float speed);

/*
 * racer_lean_response - the ordinary player's lean curve.
 *
 *     lean = (yawDelta / dt) * -30.0
 *     if (lean >  0.3) lean = lean*1.4 - 0.12
 *     if (lean < -0.3) lean = lean*1.4 + 0.12
 *     clamp to +-1.04
 *
 * A PIECEWISE-LINEAR RESPONSE WITH A GAIN BREAK AT 0.3 RADIANS, and
 * the offset is chosen to make it CONTINUOUS: at exactly 0.3,
 * 0.3*1.4 - 0.12 is 0.3. So small lean is linear and anything past
 * about 17 degrees is steepened by 40 per cent with no visible step.
 *
 * That is a deliberately shaped control curve, not a clamp - the
 * bike responds gently near upright and increasingly hard as it goes
 * over, which is what makes it feel like it has weight.
 *
 * THE 1.04 CLAMP IS 59.6 DEGREES, a real motorcycle lean limit, and
 * both the threshold and the clamp value are the same number - the
 * house pattern again.
 *
 * dt is DAT_004dc150, a smoothed frame delta, and the original
 * substitutes 20.0 for it when leanMode is 0 and dt is below 20 - a
 * floor that stops a fast frame from producing an enormous lean.
 */
float racer_lean_response(float yawDelta, float dt, int leanMode);

/*
 * racer_steer_torque - how far the heading turns this frame, RADIANS.
 *
 * The tail of the original's steering: a pure yaw, faded off
 * vitesseMaxBoite (NOT realSpeedMax - that is the separate visual
 * fade), halved during a wheelie, and multiplied by a grip term that
 * rises as the bike slides. Sign and units are measured against
 * MOTO.EXE; see the note on the definition.
 */
float racer_steer_torque(const MR_Racer *r, const MR_BikeCarac *c,
                         float wheelieAngle, float slipAbs,
                         float dtScaled);

/*
 * racer_lean_blend_weight - how much of the bike's own heading goes
 * into the travel direction. 1 at rest, 0 once speed passes
 * realSpeedMax/8. The SAME weight, complemented, scales the delta.
 *
 * flImpactFloor IS racer_lean_impact_decay's RETURN. Passing 0 is not
 * a neutral default - it is the bug this parameter closes; see the
 * note on the floor in racer.c.
 */
float racer_lean_blend_weight(float speed, float realSpeedMax,
                              float flImpactFloor);

/*
 * math_wrap_pi - wrap an angle to [-pi, pi]. FUN_00458da0,
 * 0x00458da0. The original loops rather than using fmod.
 */
float math_wrap_pi(float a);

/*
 * racer_chassis_attitude - the bike frame attitude the suspension
 * chases. FUN_0045ca00, 0x0045ca00.
 *
 * IT READS matBikeRoot AND NOTHING ELSE, so it is only as good as
 * that matrix. Anything that leaves the bike root stale leaves the
 * suspension chasing a stale target.
 */
float racer_chassis_attitude(const MR_Racer *r);

/*
 * racer_update_suspension - the chassis spring and the body bounce.
 * Racer_UpdateSuspension, 0x00466a80. Tenth in the tick order.
 *
 * It owns racer+0x2e8d..+0x2ea4: the suspension angle, the vertical
 * bounce, the spring compression, its phase and its amplitude, and
 * the smoothed chassis heading that lags the wheels by RetardCadre.
 *
 * springCompression IS AN INPUT AS WELL AS AN OUTPUT. Other code
 * kicks it negative - the wheelie touchdown writes -32 - and this
 * walks it back toward zero at 5.184 per clamped frame, forcing it
 * non-positive on the way. So it is the CONSUMER of that kick, and
 * without it the value simply latches: measured against MOTO, ours
 * sat at -32 for the rest of the race where MOTO showed 0 every
 * frame and the kick on the touchdown frame alone.
 *
 * frameDelta is g_frameDelta, and it is clamped to [3, 5] inside.
 */
void racer_update_suspension(MR_Racer *r, const MR_BikeCarac *c,
                             float frameDelta);

/*
 * racer_wheelie_swing_contact - swing the FRONT wheel contact up
 * about the REAR one. maybe_Racer_UpdateLean, at 0x0046a343.
 *
 * This is how the wheelie reaches the picture. The original never
 * rotates the bike matrix: it moves the front contact point and
 * rebuilds matBikeRoot from the wheel-to-wheel line, so the pivot
 * is the rear tyre exactly as on a real bike. Rotating the drawn
 * matrix instead gives the right angle about the wrong point and
 * buries the rear wheel in the road.
 *
 * frontIo is rotated in place; rear is untouched and stays on the
 * ground. The wheelbase is preserved.
 */
void racer_wheelie_swing_contact(float frontIo[3], const float rear[3],
                                 float wheelieAngle);

/*
 * racer_wheel_contacts_m - the contacts in an ARBITRARY frame.
 *
 * UpdateLean does not use matOrientation for this. It builds a frame
 * from the travel direction and takes the contacts in THAT, which is
 * how the travel direction reaches matBikeRoot at all.
 */
void racer_wheel_contacts_m(const float mat[12], const float pos[3],
                            const MR_BikeCarac *c,
                            float rearOut[3], float frontOut[3]);

/*
 * racer_travel_frame - the frame the bike is actually TRAVELLING in,
 * built from alpha*heading + (1-alpha)*moveDir.
 *
 * A backwards moveDir - dot(heading, moveDir) < 0 - falls back to the
 * heading. [inferred] from the guard at 0x00469d0d.
 */
int racer_travel_frame(float frame[12], const MR_Racer *r, float alpha);

/*
 * racer_lean_delta - condition the SLIP ANGLE before the response
 * curve.
 *
 * yawOrient is yaw(matOrientation), the bike's HEADING, and yawFrame
 * is yaw(racer_travel_frame), where it is actually GOING - both
 * atan2(m[2], m[8]). Their difference is the slip angle: a heading
 * DISCREPANCY rather than a frame-to-frame delta, and zero on a
 * straight. racers.txt 9.60.
 *
 * BOTH YAWS MUST NOT COME FROM THE SAME MATRIX. A frame rebuilt from
 * wheel contacts derived from r->matOrientation has its own yaw to
 * three decimals: the contacts differ by row1*dU + row2*dA, row1's XZ
 * part is parallel to row2's, so the forward vector is XZ-parallel to
 * row2 and atan2(-fwd.x/|fwd.xz|, ...) is unchanged. Ground-snapping
 * does not rescue it either, because snapping only moves Y.
 *
 *     d    = wrap(yawOrient - yawFrame)
 *     gate = 1 - clamp(1 - speed/realSpeedMax * 8, 0, inf)
 *     d    = wrap(d * gate * 2.0)
 *     clamp d to +-pi/3
 *
 * THE GATE IS A SPEED AUTHORITY, zero at rest and full above
 * realSpeedMax/8 (36 km/h on SPEED). A stopped bike cannot lean.
 *
 * VERIFIED END TO END against a paused capture at 186 km/h:
 * d 0.175962, gate 1.0, x2 -> 0.351924, /33.0 x -30 -> -0.319930,
 * piecewise -> -0.327900 against a live lean of -0.327893.
 */
float racer_lean_delta(float yawOrient, float yawFrame, float speed,
                       float realSpeedMax, float flImpactFloor);

/* The yaw of a 3x4 matrix: atan2(row0.z, row2.z). Racer_GetYaw. */
float racer_matrix_yaw(const float m[12]);

/* matOrientation row2 plus its translation, as PlaceWheels
 * builds racer+0x198. Stale on a human racer - see race.c. */
void racer_heading(const MR_Racer *r, float out[3]);

/*
 * racer_lay_rider_root - put the rider flat, keeping his heading.
 *
 * matRiderRoot = Euler(-pi/2, 0, 0) * Euler(0, -yaw(matRiderRoot), 0)
 *
 * Called at the crash entry and again on EVERY grounded frame of
 * the fall, which is what confines the free tumble to the air.
 * Only the yaw of the old matrix survives. See racer.c.
 */
void racer_lay_rider_root(MR_Racer *r);


/* ------------------------------------------------------------------ */
/*
 * THE POSE BLENDER. maybe_Racer_UpdatePose (0x0045d810), 2935 bytes.
 * It stacks seven anim_blend_angle_frames passes into one set of
 * joint angles and writes them to the racer's pose array: a base pose
 * chosen by speed, a pitch pose from the wheelie, the GLANCE, the
 * lean, the stunt, the crash, and a camera-distance fade.
 * racers.txt 9.50.
 *
 * WHAT IS DECLARED HERE ARE THE WEIGHTS AND THE POSE CHOICES - the
 * decidable part. The blending itself is anim_blend_angle_frames and
 * the frames come from pilot_rots_load.
 *
 * EVERY WEIGHT IS CLAMPED TO [-1, 1] OR [0, 1] BY THE ORIGINAL, in
 * the same shape each time: compare the float against a constant and
 * substitute. The blender itself also treats t <= 0 and t >= 1 as
 * copies, so an out-of-range weight is harmless - but the clamps are
 * transcribed because they are what the original does and because
 * the intermediate value is sometimes reused.
 */


/*
 * racer_pose_pitch - the wheelie/descent lean of the body.
 *
 *     p = ((front.y - rear.y) / (axeDistance * 48)) * 4.0
 *     clamp to [-1, 1]
 *     poseBlend += (p - poseBlend) * 0.25         a smoothing filter
 *
 * THE SIGN THEN PICKS THE POSE: positive takes saut.rot (index 7),
 * negative takes descent.rot (index 9) with the weight negated. So
 * one signed number selects between two mirrored poses and supplies
 * the blend - the third instance of the float-sign-as-selector idiom
 * in this struct.
 *
 * THE 0.25 SMOOTHING IS WHY THE RIDER DOES NOT SNAP. The raw ratio
 * jumps as the contacts cross a segment boundary; a quarter-step
 * filter turns that into a lean that takes about four frames to
 * arrive.
 *
 * poseBlend is IN OUT (racer+0x3051). Returns the signed weight.
 */
float racer_pose_pitch(const float front[3], const float rear[3],
                       float axeDistance, float *poseBlend);

/*
 * racer_pose_lean_weight - how hard the rider leans, and toward what.
 *
 * The lean pass blends between a LEAN pose and a SLIDE pose - gauche
 * against derapGauche, droite against derapDroite - and then applies
 * the result at a weight derived from the lean angle.
 *
 *     remote/AI   mix = (speed - 30 km/h) * 0.12       slide by speed
 *                 w   = |lean| * (speed < 60 km/h ? 4.3478 : 2.1739)
 *
 *     local       w   = |lean| * 2.0
 *                 mix = (w - 0.46) * 1.0870            slide by LEAN
 *                 w  *= 2.1739
 *
 * THE TWO PATHS DISAGREE ABOUT WHAT CAUSES A SLIDE. For a remote or
 * AI rider the slide pose comes in with SPEED; for the local player
 * it comes in with LEAN ANGLE. The local rider therefore looks like
 * he is sliding because of what he is doing, and everyone else looks
 * like they are sliding because of how fast they are going - which is
 * cheaper and, at a distance, indistinguishable.
 *
 * 4.3478 IS EXACTLY TWICE 2.1739, and 2.1739 is 1/0.46 while 1.0870
 * is 1/0.92. So the three constants are one number, 0.46, and its
 * reciprocals - the slow rider leans twice as readily as the fast
 * one.
 *
 * mix is written through; the return is the outer weight. Sign of
 * lean chooses the pose pair, so both are magnitudes.
 *
 * THE MIX RUNS IN OPPOSITE DIRECTIONS on the two paths, and the
 * caller has to know which: remote blends FROM slide TOWARD lean,
 * local blends FROM lean TOWARD slide. racer_update_pose orders the
 * two operands accordingly.
 */
float racer_pose_lean_weight(float lean, float speed, int persoAnim,
                             float *mix);

/*
 * racer_pose_crash_weight - the downed rider's blend.
 *
 *     (400 - crashTimerMs) * 0.02, clamped to [-1, 1]
 *
 * 400 is the value maybe_Racer_BeginCrash writes into crashTimerMs
 * (9.36.1), so this is 0 at the instant of the crash and reaches 1
 * fifty units later. The pose it drives is chuteDos.rot, the fall.
 */
float racer_pose_crash_weight(float crashTimerMs);



/*
 * racer_pose_stance_weight - how far the rider has crouched.
 *
 *     (speed - 20 km/h) / (saturation - 20 km/h)
 *
 * blended from staticHaut (index 3) toward static (index 0) - and
 * that direction is the right way round: HAUT IS HIGH. staticHaut is
 * the upright stance and static is the tucked one, so a rising weight
 * folds the rider down onto the tank.
 *
 * THE SATURATION SPEED DEPENDS ON WHO IS WATCHING. The original picks
 * 0xf0 or 0x78 at the top of the function and halves it:
 *
 *     remote / AI    120 km/h
 *     local player    60 km/h
 *
 * so the player's own rider reaches a full crouch at half the speed
 * everyone else needs. That is the same local-versus-remote split the
 * lean pass makes (9.53.2) - detail where it is looked at, cheaper
 * elsewhere.
 */
float racer_pose_stance_weight(float speed, int persoAnim);

/*
 * racer_pose_finish_index - which end-of-race pose.
 *
 *     0            staticgagne   he won
 *     1            fin2
 *     2            fin3
 *     count - 2    fin7          second to last
 *     count - 1    fin8          last
 *     anything else finautre
 *
 * SIX DISTINCT POSES FOR FINISHING, and the interesting ones are at
 * BOTH ENDS: the game has a specific animation for coming second to
 * last, not just for winning. finautre - "other" - covers the
 * anonymous middle.
 *
 * Only used while humanPlayer, raceState is 2 and speed is at least
 * 5, blended in over (speed - 5) * 0.1. Returns a PILOT_ROT_* index.
 */
int racer_pose_finish_index(int finishPosition, int racerCount);

/*
 * racer_pose_stunt_step - walk a stunt's run of frames.
 *
 *     t = sin(elapsed / duration * pi) * 1.2, clamped to [0, 1]
 *     k = min((int)(frameCount * t), frameCount - 1)
 *     blend frame k-1 against frame k by (frameCount * t - k)
 *
 * A STUNT IS A RUN OF CONSECUTIVE TABLE FRAMES (skl.h MR_StuntPose),
 * and this is what walks it. The sine makes the animation ease out
 * and back rather than run linearly.
 *
 * THE 1.2 IS WHAT MAKES IT HOLD. sin peaks at 1.0, so scaling by 1.2
 * and clamping means t sits pinned at 1 through the middle of the
 * move - the rider reaches the extreme of the stunt and STAYS there
 * for a while before coming back. Without the overshoot he would
 * touch the last frame for one instant and immediately return.
 *
 * Writes the frame index through *frame and returns the fractional
 * weight between frame-1 and frame. The caller ends the stunt when
 * elapsed exceeds duration.
 */
float racer_pose_stunt_step(int elapsed, int duration, int frameCount,
                            int *frame);



/*
 * THE GLOBALS maybe_Racer_UpdatePose READS, passed instead of being
 * global. The original is __thiscall(racer, isLocal) and reaches for
 * eleven globals; the port hands them in so the function stays
 * testable and so a missing one is visible as a NULL rather than as
 * a silently wrong branch.
 *
 * stunts NULL and stuntCount 0 disable the stunt pass, which is the
 * correct behaviour for a road bike anyway - SPEED and SPEEDPKT ship
 * no SAUTDELIRE*.ROT.
 */
typedef struct MR_PoseEnv {
	const MR_PilotRots *rots;         /* g_abPilotRot     0x0051d2a8 */
	MR_Racer           *racers;       /* g_racers         0x00581218 */
	int                 racerCount;   /* g_racerCount     0x00509bac */
	int                 localIndex;   /* g_localRacerIndex 0x004ea4c0 */
	int                 preRace;      /* g_bPreRace       0x004dc0b4 */
	int                 raceTimer;    /* g_raceTimer      0x004dc0a8 */
	int                 nowMs;        /* g_framePrevMs    0x0050bbc8 */
	int                 fieldSize;    /* DAT_00519fe4                */
	int                 podiumMode;   /* DAT_00504ea0                */
	float               podiumTimer;  /* DAT_005dabd0                */
	const MR_StuntPose *stunts;       /* g_abStuntPoseTable          */
	int                 stuntCount;   /* g_nStuntPoseCount 0x005cf4fc */
} MR_PoseEnv;


/*
 * racer_pose_pitch_tick - advance the rider's pitch filter, ONCE PER
 * SIMULATION STEP.
 *
 * poseBlend is a quarter-step filter with no dt and racer_update_pose
 * runs from the DRAW, so the advance has to live OUT of it - left
 * inside, a paused game goes on posing the rider for the few frames
 * the filter takes to converge. See racer.c.
 */
void racer_pose_pitch_tick(MR_Racer *r, const MR_BikeCarac *c);

/*
 * racer_update_pose - the whole of maybe_Racer_UpdatePose 0x0045d810.
 *
 * Stacks the seven blend passes and writes the result into
 * r->pose[slot]. out receives the same frame so the caller can bind a
 * skeleton from it.
 *
 * poseStaticPied HOLDS AN INDEX HERE, NOT A POINTER. The original
 * stores the address of a pose frame in racer+0x304d and compares it
 * against one particular frame; our poses live in MR_PilotRots by
 * index, so the field carries the index and -1 for none. Deliberate
 * divergence - the field is still at +0x304d and still four bytes.
 */
void racer_update_pose(MR_Racer *r, int persoAnim, const MR_PoseEnv *env,
                       const MR_BikeCarac *c, const float front[3],
                       const float rear[3], float cameraDist,
                       MR_AnimFrame *out);



/* ------------------------------------------------------------------ */
/* Rotate a 3x4 matrix's rows about world Y, in the sense that raises
 * racer_matrix_yaw. */
void  racer_compose_yaw(float m[12], float yaw);

/*
 * racer_save_prev_frame - Racer_SavePrevFrame (0x00465fa0), the FIRST
 * call in maybe_Racer_Tick.
 *
 * Nine previous-frame pairs at once: matOrientationPrev, the two-deep
 * position history, speedTail, frameDelta, fxGatePrev, both wheel
 * contact prevs and onGroundPrev - then it flushes a negative speed
 * to zero.
 *
 * THE POSITION ORDER MATTERS: positionPrev2 takes positionPrev BEFORE
 * positionPrev takes position, which is what makes it a real
 * two-frame history. racer_apply_cornering fits its turn radius
 * through all three.
 *
 * Four of those nine had NO WRITER AT ALL in the port before this -
 * matOrientationPrev, speedTail, positionPrev2 and fxGatePrev.
 */
void  racer_save_prev_frame(MR_Racer *r, float frameDelta);

/*
 * math_turn_radius_3pt - signed plan-view turn radius through three
 * successive positions. maybe_Math_TurnRadius3Pt (0x0045cd10).
 *
 * FLT_MAX means STRAIGHT, from both degenerate exits. Each chord is
 * clamped to [128, 800] BEFORE the fit, which makes the answer depend
 * on the shape of the three points rather than on how far the bike
 * travelled between them - frame-rate independence done in geometry.
 * Drop the clamp and the radius changes with framerate.
 */
float math_turn_radius_3pt(float ax, float az, float bx, float bz,
                           float cx, float cz,
                           float *pCentreX, float *pCentreZ);

/*
 * racer_apply_cornering - maybe_Racer_ApplyCornering (0x0045d110).
 *
 * Runs in the tick at 0x0045d792, BETWEEN racer_integrate seeding
 * velDir and racer_update_ground_contact. It fits the turn radius,
 * probes the road one unit to the side for camber, and ends by
 * calling physics_set_slip_speed - which is the only thing in the
 * game that gives velDir a lateral component.
 *
 * Does nothing while airborne (flAirTime > 2.592). The centripetal
 * term is MotoType 1 only, though the radius is fitted and stored for
 * every bike.
 */
void  racer_apply_cornering(MR_Racer *r, const MR_Trk *t,
                            const MR_BikeCarac *c, float dtScaled);

/*
 * racer_orient_quantise - decompose matOrientation to three Euler
 * angles, quantise each to the 14-bit table, and rebuild the 3x3.
 *
 * The tail of Racer_ComposeOrientation (0x0045cb00). ALL THREE ANGLES
 * are quantised, not just the heading, and the yaw the game reads back
 * afterwards is atan2 of two rebuilt elements that carry pitch and roll
 * in them. Was race_orient_quantise in race.c.
 */
void  racer_orient_quantise(float *m);

/*
 * racer_compose_orientation - fold an Euler triple into matOrientation
 * and re-quantise. Racer_ComposeOrientation (0x0045cb00), whole.
 *
 * euler is (roll, pitch, yaw) and is applied on the LEFT:
 * matOrientation = rotation * matOrientation.
 */
void  racer_compose_orientation(float m[12], const float euler[3]);

/*
 * racer_update_airborne - maybe_Racer_UpdateAirborne (0x00466660).
 *
 * Called from the tick at 0x0045d7a6, after ground contact has set
 * onGround. Three jobs:
 *
 *   o the air timer, flAirTime - += 2.592 a frame airborne, 0 on the
 *     ground, and this is its only writer pair;
 *   o THE BRAKE DIVE - while the brake is held and longSpeed > 0.1 it
 *     composes a nose-down pi/128 into matOrientation. This is the
 *     ONLY source of pitch on a grounded bike, and without it
 *     physics_slip_speed returns a structural zero;
 *   o the manual up/down gear latch on button bits 0x10 and 0x20.
 *
 * The dive does not accumulate: racer_dive_angle returns 0 once the
 * chassis attitude leaves +-(pi/2 - pi/32), so holding the brake gives
 * one step and then nothing.
 *
 * Gearbox_RequestAutoShift and Gearbox_TickShift are part of the
 * original function but are NOT repeated here - race_tick_physics
 * already runs both at its own measured placement.
 */
void  racer_update_airborne(MR_Racer *r, unsigned int buttons,
                            MR_Gearbox *gb, const MR_BikeCarac *c,
                            float frameDelta);
float racer_wall_recovery_yaw(const MR_Racer *r,
                              const float wallNormal[3], float yaw);
/*
 * IMPORT 1 - see docs/whole-function-import.txt and CLAUDE.md 15.
 *
 * racer_update_ground_contact is maybe_Racer_UpdateGroundContact
 * whole: the clip, the crash decision, the ground snap, the frame
 * rebuild, the wheel contacts and the pending-yaw drain, in the
 * original's order. The slices it replaces are gone.
 */
int   racer_update_ground_contact(MR_Racer *r, const MR_Trk *t,
                                  const MR_BikeCarac *c,
                                  MR_Gearbox *gb, int nAxis0,
                                  int bLowAirThreshold, int bNoWalls);

/*
 * racer_update_crash - one frame of the rider off the bike.
 *
 * Returns RACER_CRASH_DONE when the rider has finished and the
 * caller should run race_reset_crashes; the original calls
 * Racer_ResetCrash itself, but that walks the whole field and so
 * lives in race.c under rule 13.
 */
#define RACER_CRASH_NONE    0
#define RACER_CRASH_ACTIVE  1
#define RACER_CRASH_DONE    2

int racer_update_crash(MR_Racer *r, const MR_Trk *t);

/*
 * racer_integrate - THE MOVER. See racer.c; bAllowLift gates the
 * constant downward push on the grounded branch.
 */
void racer_integrate(MR_Racer *r, const MR_BikeCarac *c,
                     float frameDelta, int bAllowLift);

/*
 * MR_RacerAudio - the per-racer audio record.
 *
 * ORIGIN  the 0x78-byte record maybe_Snd_TickRacer (0x00406290)
 *         takes in ECX. Two instances in the original: an array of
 *         four at 0x005032c0 for audible OTHER racers, and a
 *         standalone one at 0x005034b0 for the viewed racer.
 *
 * DIVERGENCE: ONE RECORD PER RACER, NOT FOUR PLUS ONE. The
 * original's four-slot array is an audibility budget - it picks the
 * four nearest through the slot table at 0x00504210. Keeping a
 * record per racer costs a few hundred bytes and removes a whole
 * layer from the thing being brought up; the SELECTION still has to
 * exist, and it is what decides which records get ticked.
 *
 * NAMED FIELDS, NOT INDICES. The original is an int array and
 * audio-cues.txt 9.2 maps the indices; the comments carry them so a
 * trace can be matched up.
 */
typedef struct {
	int          state;        /* [0]    0 idle, 1 running, 2 low  */
	MR_Voice     idle;         /* [8]    +0x20, the motostp loop  */
	MR_Voice     engine;       /* [9]    +0x24, the per-gear loop   */
	MR_Voice     depart;       /* [0xb]  +0x2c, the throttle blip */
	MR_Voice     skid;         /* [0xc]  +0x30, the surface loop  */
	int          skidFading;   /* [0xe]  +0x38, the release ramp  */
	int          skidRate;     /* [0xf]  +0x3c, Hz before speed   */
	int          skidVol;      /* [0x10] +0x40, 230 down by 50    */
	int          master;       /* [0xd]  +0x34, distance attenuation*/
	int          prevGear;     /* [0x19] +0x64                     */
	float        prevRpmFrac;  /* [0x17] +0x5c                     */
	unsigned int prevInput;    /* [0x16] +0x58                     */
	int          prevCrash;    /* [0x1d] +0x74                     */
	float        prevWheelie;  /* [0x1a] +0x68                     */
	float        peakWheelie;  /* [0x1b] +0x6c, a PEAK HOLD        */
	int          revDrift;     /* [0x13] +0x4c, 0..3000 Hz         */
	int          engineVol;    /* [0x15] +0x54                     */
	int          engineVolMs;  /* [0x18] +0x60                     */
} MR_RacerAudio;

/*
 * racer_audio_surface - the class the mixer switches on, which is
 * NOT the surface id: SurfaceUnder adds 9 and four values then fold
 * back down. 0 in the air. See racer.c.
 */
int racer_audio_surface(const MR_Racer *r, const MR_Trk *trk);

/*
 * ORIGIN  NONE - ours.
 *
 * PUT A RECORD INTO ITS EMPTY STATE WITHOUT TOUCHING THE
 * MIXER. A zero-filled record is NOT empty: voice handles are
 * slot indices and 0 is a real slot, so every voice field has
 * to be set to MR_VOICE_NONE before first use.
 *
 * racer_audio_reset cannot serve here because it STOPS the
 * voices the fields name, and on a fresh static that means
 * stopping voice 0 - whatever happens to be playing in it.
 * Use this for a record that has never been used, and reset
 * for one that has.
 */
void racer_audio_init(MR_RacerAudio *a);

/* Silence the racer and clear the record - the crash reset, and the
 * per-race one. ORIGIN  FUN_00406190 plus the block at 0x004062c8. */
void racer_audio_reset(MR_RacerAudio *a);

/*
 * ORIGIN  maybe_Snd_TickRacer @ 0x00406290
 *
 * master is the distance attenuation, 0..255; pan is -1..+1.
 * A SKELETON - see racer.c for which branches are live.
 *
 * trk is wanted for the surface class the skid voice switches
 * on; a null one simply leaves that voice silent.
 *
 * velocityPitch MULTIPLIES EVERY FREQUENCY this racer plays -
 * engine, idle and skid alike. It is the caller's Doppler term
 * and it is exactly 1.0 for the racer being watched; only the
 * other bikes get a real one. See race.c.
 *
 * playerVoices gates the voices only the person holding the
 * controller gets - the throttle blip today. The driver passes
 * 1 for the viewed racer and 0 for every AI bike, and it is
 * ANDed with humanPlayer inside, exactly as the original clears
 * it at 0x004063f2.
 */
void racer_audio_tick(MR_RacerAudio *a, const MR_Racer *r,
                      const MR_BikeCarac *c, const MR_Trk *trk,
                      int master, float pan, int playerVoices,
                      float velocityPitch);

#ifdef __cplusplus
}
#endif

#endif /* MRACER_RACER_H */
