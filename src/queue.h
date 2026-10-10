/*
 * queue.h - the primitive queue.
 *
 * Everything between Screen3D_BeginFrame and Screen3D_DrawScene only
 * QUEUES. No pixels are touched by game code at all - the whole frame
 * is submitted in one call, and the queue is what carries it. Both
 * renderers consume the same structure; the software one walks it one
 * way and the hardware one the other, and neither knows which producer
 * filled it.
 *
 * It is an array of 8000 linked-list HEADS indexed by a depth key,
 * with the nodes themselves coming from one flat pool. That is a
 * bucket sort with 8000 buckets and no comparisons, which in 1997 was
 * the cheap way to depth sort a scene.
 *
 * See docs/subsystems/renderer.txt sections 3.1 to 3.4.
 */

#ifndef MRACER_QUEUE_H
#define MRACER_QUEUE_H

#include "prim.h"
#include "transform.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 8000 heads at DAT_00655d38. The ftol path clamps to 0..0x1f3e, so
 * bucket 7999 is reachable ONLY through bucketMode 2 - and the walk
 * draws it last, unconditionally, outside the primitive limit. It is
 * the "always on top" bucket.
 */
#define QUEUE_BUCKETS      8000
#define QUEUE_BUCKET_MAX   0x1f3e   /* 7998, the ftol clamp        */
#define QUEUE_BUCKET_TOP   7999     /* bucketMode 2 only           */

/*
 * UNITS PER BUCKET. THE ORIGINAL IS 4, not 1: 0x004d1f9c reads 0.25
 * and Screen3D_QueuePrimitive multiplies the key by it before the
 * ftol at 0x0049c5ca. So the original orders out to 4 * 7998 = 31,992
 * units, which is why its 18000 far plane fits with room to spare,
 * and everything beyond shares the clamp bucket and paints in
 * arbitrary order.
 *
 * This said "the original is 1 - the bucket IS the integer camera
 * depth" and that was never read out of the binary.
 *
 * That is invisible at retail settings because the far clip is
 * 18000 and little of consequence is out there. Raise the draw
 * distance and it stops being invisible: enormous unordered water
 * meshes land in one bucket and paint over the scene.
 *
 * Scaling trades near-field resolution for range. 1.0 is the
 * original; 8.0 covers 64000 units at 8-unit granularity, which is
 * far finer than any face this engine draws.
 */
void queue_set_units_per_bucket(float u);

/*
 * The split-primitive pool. The original is MR_Primitive[2500] at
 * 0x0062ebf0 and it holds the triangles a clipped face is broken into
 * - they cannot live in the mesh's own face array, which is shared and
 * const for the frame.
 */
#define QUEUE_SPLIT_MAX    2500

/* The node pool, 0x1d4b entries at DAT_0065f0e0. Queueing past it is
 * silently dropped, which is the original's behaviour. */
#define QUEUE_NODE_MAX     0x1d4b   /* 7499                        */

/*
 * The per-frame primitive limit, the float at 0x0062e438, set to
 * 2000.0f in Screen3D_Open. The walk compares a running count against
 * it and silently stops DISPATCHING past it - it keeps walking. A port
 * that omits the limit draws MORE than the original, which is a
 * difference to make deliberately rather than by accident.
 */
#define QUEUE_PRIM_LIMIT   2000.0f

/*
 * g_queuePrimLimit - a DEBUG limit on top of QUEUE_PRIM_LIMIT.
 *
 * Negative means no limit, and that is the default, so an unset build
 * behaves exactly as it did before this existed.
 *
 * WHAT IT IS FOR. The queue is the only place in the pipeline where
 * every primitive of a frame exists at once, in draw order, before any
 * of them has been rasterized. Capping the dispatch there and walking
 * the cap upward draws the frame one primitive at a time, which turns
 * "there is a stray line on the screen" into "primitive 47 is the
 * stray line" - and primitive 47 can then be traced back to its
 * object, its face and its vertices.
 *
 * It is a limit on DISPATCH, not on the walk: the walk still visits
 * and clears every bucket, because the walk clearing bucket heads is
 * how the queue is emptied for the next frame. Skipping it would
 * leave last frame's nodes linked and the frame after would draw
 * both. That is the same reason the original keeps walking past its
 * own limit rather than breaking out.
 *
 * UNLIKE the original's limit, this one also applies to bucket 7999.
 * The game exempts the always-on-top bucket from its budget, which is
 * right for a HUD, but a debug limit that cannot reach some of the
 * primitives is not much of a bisection tool. Limit 0 draws nothing,
 * which is the contract you want when bisecting.
 */
extern int g_queuePrimLimit;

/* bucketMode, the fifth argument of Screen3D_QueuePrimitive. */
#define QUEUE_MODE_DEPTH   0        /* bucket from the depth key   */
#define QUEUE_MODE_NEAREST 1        /* bucket 0                    */
#define QUEUE_MODE_TOP     2        /* bucket 7999                 */

/*
 * MR_QueueNode, 0x18 bytes. Producer and consumer agree field for
 * field - Screen3D_QueuePrimitive writes it and D3D_DrawScene reads
 * it.
 */
typedef struct MR_QueueNode {
	MR_Primitive        *prim;      /* +0x00                        */
	float                depth;     /* +0x04 also the bucket source */
	struct MR_QueueNode *next;      /* +0x08                        */
	MR_Vertex           *vertices;  /* +0x0c base, 0x1c stride      */
	/*
	 * THE LIVERY, -1 WHEN THE FACE IS NOT PALETTED.
	 *
	 * A DELIBERATE DIVERGENCE, AND THE ONLY ONE IN THIS
	 * NODE. The original stores the PALETTE POINTER here,
	 * because its span renderer indexes the table per texel.
	 * Ours resolves a palette into a whole texture at upload
	 * time, so what the draw needs is WHICH palette, not
	 * where it is. Same four bytes, same offset.
	 */
	int                  palette;   /* +0x10, -1 = not paletted     */
	int                  texSlot;   /* +0x14 or -1                  */
} MR_QueueNode;

typedef char mr_queuenode_is_0x18[(sizeof(MR_QueueNode) == 0x18) ? 1 : -1];

/*
 * The traversal direction, g_spanBufferEnabled.
 *
 *   0  buckets DOWNWARD, far to near - the painter's algorithm
 *   1  buckets UPWARD, near to far, then Screen3D_FlushSpans
 *
 * The original defaults this to 1, because the span buffer needs
 * front-to-back: whatever draws first claims its pixels. THE
 * RECONSTRUCTION DEFAULTS IT TO 0, and that is a deliberate divergence
 * - see queue.c.
 */
extern int g_spanBufferEnabled;

int  queue_create(void);
void queue_destroy(void);

/* Screen3D_Open's seeding: heads cleared, low 7999, high -1. The
 * inverted range is how the walk knows the queue is empty without a
 * separate flag. */
void queue_reset(void);

/*
 * Screen3D_QueuePrimitive. Two jobs: pick the depth bucket, and apply
 * level of detail. hwFlags is built by the cull stage - bit 0 "crosses
 * the near plane", bit 1 "extends outside the viewport" - and is a
 * request for clipping work, not a state.
 */
void queue_primitive(MR_Primitive *prim, float depth, MR_Vertex *vertices,
                     unsigned int hwFlags, int bucketMode);

/*
 * Screen3D_QueuePrimitivePaletted (0x0049ca70). The same node in the
 * same bucket, plus the livery the face is to be resolved with.
 *
 * Screen3D_CullAndQueueFaces picks between the two on one test - the
 * face's bank has palettes or it does not - and that is the test the
 * port makes too. NOT REPRODUCED: the extra kind-0x2a substitution
 * the paletted sibling applies under g_cfgDetailLevel <= 1.
 */
void queue_primitive_paletted(MR_Primitive *prim, float depth,
                              MR_Vertex *vertices, unsigned int hwFlags,
                              int bucketMode, int palette);

/*
 * The walk of Screen3D_DrawScene. dispatch is called once per
 * primitive; in the original it is Screen3D_DispatchPrimitive on the
 * software path, and D3D_DrawScene carries its own copy of this walk
 * on the hardware one.
 */
void queue_walk(void (*dispatch)(const MR_QueueNode *node));

/*
 * The range of buckets actually used this frame. Worth watching: the
 * bucket index is __ftol of a depth in WORLD UNITS, clamped at 7998, so
 * a scene whose camera sits farther than about 8000 units away puts
 * every primitive in the same bucket and the sort does nothing. The
 * bucket key assumes a particular world scale, and that assumption is
 * not written down anywhere in the binary.
 */
void queue_bucket_range(int *lo, int *hi);

/* Triangles produced by the near-plane clip this frame. */
int queue_clipped_count(void);

/* Counters for the frame just walked. */
void queue_stats(int *queued, int *dispatched, int *droppedNearPlane,
                 int *droppedFull);

#ifdef __cplusplus
}
#endif

#endif /* MRACER_QUEUE_H */
