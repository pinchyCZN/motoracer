/*
 * racerdraw.h - maybe_Racer_Draw (0x0045f290), in pieces.
 *
 * racerdraw_racer IS maybe_Racer_Draw, WHOLE. Every other function
 * here is one named, contiguous block of that original, carrying the
 * address range it came from, and racerdraw_racer calls them in the
 * original's order. It owns the transform and the face queue as well,
 * which is what lets it exist at all - see its definition for why
 * wiring the blocks into the old draw could not work.
 *
 * SO THE TWO-ADDRESS ORIGIN TAGS HERE ARE NOT SLICES in rule 15's
 * sense. A slice is a fragment written from the part somebody needed;
 * these are the whole function cut on its own seams. The 6.5 KB was
 * brought over a block at a time so each could be checked against
 * the listing, and the blocks stayed because the seams are the
 * original's.
 *
 * MR_RacerPart is laid out in racer.h - all 100 bytes, with offset
 * assertions.
 *
 * THE RACE PATH NO LONGER USES THE SLICES. race_bike_place and the
 * bike_draw layer are deleted outright. skl_build_anim,
 * skl_refork_front and skl_swingarm STILL EXIST AND MUST NOT BE
 * DELETED - menu.c page 10 draws a bike with no MR_Racer behind it
 * and every entry point here takes one, so they are kept for that
 * caller alone. See skl.h.
 *
 * DO NOT REACH FOR THEM FROM THE RACE PATH AGAIN; keeping two
 * implementations beside each other is how the port ended up with
 * two of one behaviour three times running.
 */

#ifndef MRACER_RACERDRAW_H
#define MRACER_RACERDRAW_H

#include "racer.h"
#include "bikes.h"
#include "skl.h"
#include "mesh.h"
#include "prim.h"
#include "transform.h"
#include "trk.h"

/*
 * racerdraw_far_lod - skeletonSlot >= 2. One mesh, no skeleton, no
 * wheels, no rider. It has NO suspension pitch, NO steering, NO
 * crashFallSide, and it is placed from the REAR CONTACT rather than
 * the wheelbase midpoint - so the two LODs genuinely disagree about
 * where the bike is. There is also a +pi/2 yaw that is not optional.
 */
void racerdraw_far_lod(const MR_Racer *r, const MR_BikeCarac *c,
                       const float camEye[3], const float camView[12],
                       float out[12]);

/*
 * racerdraw_far_shade - 0x1f when the renderer flag is clear OR the
 * level is 0. The OR matters: an explicit 0 means unshaded, not black.
 */
int racerdraw_far_shade(int rendererFlag, int shadeLevel);

/*
 * racerdraw_steer_factor - the speed falloff every steering term is
 * scaled by. PersoAnim picks a clamped or an unclamped form.
 */
float racerdraw_steer_factor(const MR_BikeCarac *c, float speed,
                             int persoAnim);

/*
 * racerdraw_build_matrices - THREE matrices off matBikeRoot, not two.
 * rear gets rearWheelSpin and no suspension pitch; body gets the
 * pitch; front is the body plus the bars and frontWheelSpin.
 *
 * THE ORIGINAL BUILDS ALL THREE IN ONE STACK SLOT IN TURN, so they
 * read as two chains unless each is copied out before the next
 * overwrites it - and read as two, the rear wheel matrix is
 * mislabelled as the body.
 *
 * THE WHEEL MATRICES CARRY THE SPIN AND NOTHING ELSE DOES.
 * rearOut is MOTO's +0x268 and frontWheelOut its front counterpart;
 * rollOut (+0x298) and frontOut are spin-free and are what the
 * frames, the fork pivot, the arm and the swingarm must use.
 */
void racerdraw_build_matrices(const MR_Racer *r, float steerFactor,
                              float rollOut[12], float rearOut[12],
                              float frontOut[12], float bodyOut[12],
                              float frontWheelOut[12]);

/*
 * racerdraw_bind_skeleton - POPULATE r->pose FROM THE .skl SKELETON.
 *
 * The original keeps its bones in the racer as MR_RacerPart[5][20];
 * this port keeps them in MR_Skeleton. Every block of
 * maybe_Racer_Draw that touches a bone reads the pose array, so
 * without this bridge none of them has anywhere to write.
 *
 * Call it before racerdraw_part_walks, every frame - it is one pass
 * over seventeen members. frame may be NULL to build the tree without
 * angles.
 */
void racerdraw_bind_skeleton(MR_Racer *r, const MR_Skeleton *skl,
                             const MR_AnimFrame *frame);

/*
 * racerdraw_part_walks - the two Skl_BuildPartMatrices walks: the bike
 * subtree off part 0 with the body matrix, the rider off part 3 with
 * matRiderRoot.
 *
 * THE MATRIX IS PASSED BY VALUE - RET 0x30 is forty-eight bytes,
 * exactly a 3x4. The DB's twelve undefined4 parameters are that one
 * matrix; see the plate at 0x00463d90.
 */
void racerdraw_part_walks(MR_Racer *r, const float bodyMat[12]);

/*
 * racerdraw_fork_part_matrix - the fork's part matrix is OVERWRITTEN
 * after the walk, placed from the scaled tip and rotated by the pose
 * angle. Skipped when the part has no parent.
 */
void racerdraw_fork_part_matrix(const float angle[3], const float pivot[3],
                                const float scaledTip[3],
                                const float parentMat[12], float out[12]);

/*
 * racerdraw_draw_position - mid + (rearOffset + frontOffset). Two
 * vectors on two different matrices, not one scalar on one axis.
 */
void racerdraw_draw_position(const float mid[3], const float rear[3],
                             const float front[3], float out[3]);

/*
 * racerdraw_view_matrix - the body matrix round-tripped through an
 * euler and composed with the camera. The round trip is the original's
 * and drops any scale; see the definition.
 */
void racerdraw_view_matrix(const float bodyMat[12], const float drawPos[3],
                           const float camEye[3], const float camView[12],
                           float out[12]);

/*
 * racerdraw_fork_solve - the fork pivot, the wheel point on its axis,
 * and the two lengths racerdraw_fork_scale needs. skl_refork_front
 * does the same job for page 10.
 */
void racerdraw_fork_solve(const MR_Racer *r, const MR_BikeCarac *c,
                          float steerFactor, const float frontMat[12],
                          const float bodyMat[12], const float bindPivot[3],
                          const float bindTip[3], float pivotOut[3],
                          float wheelOut[3], float *lenOut, float *reachOut);

/*
 * racerdraw_rebuild_contacts - re-derives the front contact one
 * wheelbase from the rear along their shared direction, and writes it
 * back to racer+0x1b4. midOut, which may be NULL, gets the midpoint.
 */
void racerdraw_rebuild_contacts(MR_Racer *r, const MR_BikeCarac *c,
                                float midOut[3]);

/*
 * racerdraw_offsets - the two vertical offsets added to the contact
 * midpoint. The REAR one carries springBounce - the swingarm squat -
 * and rides the ROLL-ONLY matrix; the FRAME one carries forkDive and
 * rides the BODY matrix. One rotation apart, and not interchangeable.
 */
void racerdraw_offsets(const MR_Racer *r, const MR_BikeCarac *c,
                       const float rollMat[12], const float bodyMat[12],
                       float rearOut[3], float frontOut[3]);

/*
 * racerdraw_rear_frame - A FRAME FROM A CHAIN MATRIX AT A POSITION,
 * round-tripped through an euler as the original does. The name says
 * "rear" because that is where it was first read; it is used THREE
 * times and only two of them are the rear:
 *
 *   front chain at drawPos       the frame the fork is solved in
 *   rear  chain at drawPos       the frame the arm angle is measured
 *                                in, and that places the rear wheel
 *   rear  chain at drawPos-eye   the frame the arm is DRAWN in
 *
 * One block, three calls. Do not specialise it to the rear.
 */
void racerdraw_rear_frame(const float rearMat[12], const float drawPos[3],
                          float out[12]);

/*
 * racerdraw_fork_scale - the fork stretch, and it is a RATIO.
 */
float racerdraw_fork_scale(float posedLen, float bindLen, float reach);

/*
 * racerdraw_front_wheel_offset - the wheel is drawn at an OFFSET from
 * its own contact, because the instance already carries the contact as
 * its position. Handing it the absolute point doubles the placement.
 */
void racerdraw_front_wheel_offset(const MR_Racer *r,
                                  const float wheelPoint[3],
                                  float out[3]);

/*
 * racerdraw_fork_tip - the bind-space fork tip, re-pointed along the
 * BIND direction at the scaled length. The other half of
 * racerdraw_fork_scale, and the part that stretches the mesh.
 */
void racerdraw_fork_tip(const float bindPivot[3], const float bindTip[3],
                        float posed, float reach, float out[3]);

/*
 * racerdraw_arm_angle - the swingarm, a SIGNED angle about the pivot
 * axis. The axes are derived by transforming points, not by reading
 * matrix rows, which differs only under scale - see the [open] at
 * racerdraw_view_matrix in the definition.
 */
float racerdraw_arm_angle(const MR_Racer *r, const float rearMat[12],
                          const float bindPivot3[3]);

/*
 * racerdraw_steer_pose - the bars reach the fork through the POSE,
 * on a different scale from the wheel matrix. Part 1's angle triple.
 */
void racerdraw_steer_pose(MR_Racer *r, float steerFactor);

/*
 * racerdraw_arm_matrix - the swingarm's own matrix, a pure X rotation
 * by (FourcheAngleArriere + the solved angle) composed onto the rear
 * frame. The original builds it inline from the cosine table; two of
 * its three euler angles are constant zero.
 */
void racerdraw_arm_matrix(float restAngle, float armAngle,
                          const float rearFrame[12], float out[12]);

/*
 * racerdraw_rear_wheel_offset - the rear wheel goes wherever the ARM
 * puts it: its part pivot through the arm matrix, as an offset from
 * the stored contact. Nothing places the rear wheel directly.
 */
void racerdraw_rear_wheel_offset(const MR_Racer *r, const float armMat[12],
                                 const float bindPivot[3], float out[3]);

/*
 * racerdraw_part_is_rider - parts 3..14 are the rider, everything else
 * the bike. There is no flag; the RANGE is the test, and it is what
 * decides which draw position a part is placed against.
 */
int racerdraw_part_is_rider(int part);

/*
 * racerdraw_part_matrix - one part's view matrix: the RIGID INVERSE of
 * its part matrix, the draw position added into the translation row,
 * then composed with the camera. The inverse is required because
 * skl_transform_verts bakes with it.
 */
void racerdraw_part_matrix(const float partMat[12], const float drawPos[3],
                           const float camEye[3], const float camView[12],
                           float out[12]);

/*
 * racerdraw_shading_enabled - the gate on the per-vertex pass,
 * 0x0046199a. It TAKES THE FLAG rather than reading the global, so
 * the caller names which one: pass g_rdShadeParts, never a literal.
 * racer-draw-audit.txt 45 records what a hardcoded 1 cost.
 */
int racerdraw_shading_enabled(int globalFlag);

/*
 * The queue callback: Screen3D_CullAndQueueFaces, or its Ex form when
 * ex is set - the original picks between them on the sign of the cull
 * flag and passes 1 to the Ex one.
 */
typedef void (*MR_RacerDrawQueue)(void *verts, void *faces, int count,
                                  int flag, int ex);

/*
 * racerdraw_queue_parts - the face queue, whole. It runs ONE part
 * further than the transform loop, does NOT remap the index, and
 * queues every part against the FIRST part's vertex base. All three
 * are the original's; see the definition before aligning any of them.
 *
 * IT DRAWS NOTHING TODAY and is not called: every part has
 * faceCount 0, because MR_SklMember has no per-member face list in
 * MR_Primitive form. racerdraw_racer queues the whole mesh in one
 * call instead. The gap is tagged at racerdraw_bind_skeleton.
 *
 * partCount is perSlotArrays[slot], passed in because that field is
 * still raw bytes in MR_Racer.
 */
void racerdraw_queue_parts(MR_Racer *r, int partCount, void *vertexBase,
                           int cullFlag, MR_RacerDrawQueue queue);

/*
 * racerdraw_place_rider - matRiderRoot and riderPosition, which live
 * in the draw rather than the tick. The translation rows stay ZERO,
 * as the original leaves them.
 */
void racerdraw_place_rider(MR_Racer *r, const float bodyMat[12],
                           const float bodyPos[3], int persoAnim,
                           float drawOut[3]);

/*
 * MR_RacerDrawSet - ORIGIN NONE - ours, and it stands in for globals.
 *
 * maybe_Racer_Draw reaches straight out to g_meshSkl[5],
 * g_meshRoueAvant/Arriere and the per-LOD skeleton table at
 * racer+0x2d88. The port keeps those in race.c and menu.c statics, so
 * they arrive as one argument rather than as five new globals.
 *
 * lodMesh/lodPrims are indexed BY SLOT so slot 2, 3 and 4 land where
 * maybe_Racer_SelectLod puts them; slots 0 and 1 are the articulated
 * skeleton and are never read out of this array.
 */
typedef struct MR_RacerDrawSet {
	const MR_Skeleton  *skl;                        /* g_meshSkl[0]   */
	MR_Primitive       *prims;                      /* the WHOLE mesh */
	int                 primCount;

	const MESH         *wheelMesh[2];               /* rear, front    */
	MR_Primitive       *wheelPrims[2];
	int                 wheelPrimCount[2];
	int                 wheelOk[2];

	const MESH         *lodMesh[RACER_LOD_SLOTS];   /* slots 2..4     */
	MR_Primitive       *lodPrims[RACER_LOD_SLOTS];
	int                 lodPrimCount[RACER_LOD_SLOTS];
} MR_RacerDrawSet;

/*
 * racerdraw_racer - THE SINGLE ENTRY POINT, and the whole of
 * maybe_Racer_Draw. It builds every matrix, walks both subtrees,
 * transforms every part and queues the faces.
 *
 * THE TRANSFORM AND THE QUEUE BELONG TO IT, and that is what lets
 * it exist at all: a layer that only builds matrices and hands them
 * on places every part a second time, in the wrong space. See
 * docs/subsystems/racer-draw-pipeline.txt 3.
 *
 * lod is the original fourth argument: -1 from the race, meaning
 * "keep whatever slot racerdraw_select_lod left"; a real slot from a
 * menu, which also copies matOrientation into matBikeRoot.
 *
 * budgetLeft may be NULL, and is charged in faces QUEUED rather than
 * faces OFFERED - cull_and_queue_faces returns the first of those.
 * Subtracting the mesh's primCount instead overcharges every racer
 * by whatever the cull rejected.
 *
 * Returns the number of faces queued; 0 if the racer drew nothing.
 */
int racerdraw_racer(MR_Racer *r, const MR_RacerDrawSet *set,
                    const float camView[12], const float camEye[3],
                    int lod, int *budgetLeft, const MR_Trk *trk);

/*
 * racerdraw_set_frustum - FUN_0046e320, the four view planes
 * racerdraw_visible tests. Call once a frame before drawing racers;
 * without it the cull falls back to the distance test alone, which
 * never fires and draws every racer however far ahead it is.
 *
 * farDist is the same per-detail table entry the distance test
 * reads - 0x0051d1bc's first float, 18000 at the live detail level.
 */
void racerdraw_set_frustum(const float view[12], const float eye[3],
                           float farDist);

/*
 * racerdraw_visible - FUN_0046e8b0, the per-racer cull: the draw
 * distance, then the four planes above. The frustum is the half that
 * does the work - the distance test needs 36000 and effectively
 * never fires.
 */
int racerdraw_visible(const float pos[3], float radius,
                      const float camEye[3]);

/* The bike bounding sphere maybe_Race_DrawRacers passes to the cull. */
#define RACERDRAW_RACER_RADIUS  512.0f

/*
 * racerdraw_select_lod - maybe_Racer_SelectLod. Writes
 * r->skeletonSlot and r->cameraDist.
 *
 * THE ORIGINAL CALLS IT FROM THE TICK, from gameplay, and
 * maybe_Racer_Draw then takes -1 and uses whatever it left. That is
 * why a menu page - which never ticks a racer - draws the
 * articulated body.
 *
 * THIS PORT CALLS IT FROM THE DRAW, which is a DELIBERATE
 * DIVERGENCE: race_tick_all has no camera and the eye exists only
 * in the draw. Do not "correct" it to the tick - race.c's call site
 * carries the reason and the trap that goes with it.
 *
 * Do not call it at all unless the medium, low and very-low meshes
 * actually loaded, or a distant racer will select a mesh that is
 * not there and draw nothing.
 */
void racerdraw_select_lod(MR_Racer *r, const float camEye[3]);

/* See the definition: NOT "LOD off" - it means "no articulated body". */
extern int g_nLodSklDisabled;

/*
 * The shadow. build runs in the TICK, as maybe_Racer_BuildShadow does
 * off both physics ticks; init runs once per racer, as Racer_Create
 * does; shadow draws, straight after racerdraw_racer.
 */
void racerdraw_init_shadow(MR_Racer *r);
void racerdraw_build_shadow(MR_Racer *r, const MR_Trk *trk);
int  racerdraw_shadow(MR_Racer *r, const float camView[12],
                      const float camEye[3]);

/*
 * racerdraw_shade_level - 0x10 inside a shaded track segment and
 * 0x1f everywhere else, read off MR_TrkSegment.shaded. It is the
 * TUNNEL DARKENING and has nothing to do with the per-vertex pass
 * below.
 */
int  racerdraw_shade_level(const MR_Racer *r, const MR_Trk *t);

/*
 * racerdraw_set_shade_parts - the port's g_nShadeParts (0x004ea4d0).
 * OFF by default, which is what a race measures; the bike-select
 * page is its only writer in the original.
 */
void racerdraw_set_shade_parts(int on);
int  racerdraw_shade_parts_enabled(void);

/*
 * racerdraw_build_normals - smooth vertex normals for one LOD slot,
 * built once per racer as Racer_Create does. Stored in the racer's
 * per-slot pointer array, which is what racerdraw_shade_parts reads.
 * racerdraw_free_normals releases one slot.
 */
void racerdraw_build_normals(MR_Racer *r, const MR_Skeleton *skl,
                             int slot);
void racerdraw_free_normals(MR_Racer *r, int slot);

/*
 * racerdraw_set_prim_variant - swap prim kinds between the lit and
 * unlit variant, 0x10 <-> 0x15 and 0x26 <-> 0x27. The prim array is
 * shared per LOD slot, so this is SHARED STATE - see the definition.
 */
void racerdraw_set_prim_variant(MR_Primitive *prims, int count,
                                int shaded);

/*
 * racerdraw_shade_parts - the per-vertex pass, lighting by NORMAL
 * against a world light direction. Gated on set_shade_parts, which
 * is off in a race.
 */
void racerdraw_shade_parts(MR_Racer *r, const float light[3],
                           MR_Vertex *verts);


/*
 * THE FIRST-PERSON COCKPIT MESHES - g_meshGuidon and g_meshCockpit,
 * loaded from the bike manifest's FileGuidon and FileCockpit at scale
 * 8.0 rather than 0.95. bike-assets.txt 2.
 *
 * Same shape as MR_RacerDrawSet: race.c owns the meshes and hands them
 * over rather than growing two more globals.
 */
#define COCKPIT_PART_GUIDON 0      /* the handlebars and forks */
#define COCKPIT_PART_DASH   1      /* the dash and screen      */
#define COCKPIT_PARTS       2

typedef struct MR_CockpitPart {
	const MESH   *mesh;
	MR_Primitive *prims;
	int           primCount;
} MR_CockpitPart;

typedef struct MR_CockpitSet {
	MR_CockpitPart part[COCKPIT_PARTS];
} MR_CockpitSet;

/*
 * racerdraw_cockpit - FUN_00462110 @ 0x00462110, the cockpit
 * overlay. Returns faces queued.
 *
 * wheelieAngle is the original's third argument, passed separately
 * because maybe_Race_DrawRacers reads it off the racer at the call
 * site rather than letting this function do it.
 *
 * THE CALLER OWNS THE GATE: rear-view key, replay mode and the pocket
 * bike all suppress it - 0x0046213e.
 */
int  racerdraw_cockpit(const MR_Racer *r, float wheelieAngle,
                       const MR_CockpitSet *set);

/* Forget the attitude filter. Ours - see the definition. */
void racerdraw_cockpit_reset(void);

#endif
