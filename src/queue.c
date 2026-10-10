/*
 * queue.c - see queue.h, docs/subsystems/renderer.txt sections 3.3 and
 * 3.4, and the plate comments on Screen3D_QueuePrimitive (0x0049c590)
 * and Screen3D_DrawScene (0x0049ad90).
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "queue.h"
#include "detail.h"
#include "clip.h"

/*
 * DIVERGENCE, DELIBERATE, AND THE ONLY ONE IN THIS FILE THAT CHANGES
 * WHAT IS DRAWN.
 *
 * The original sets g_spanBufferEnabled to 1 in Screen3D_Open, so the
 * front-to-back walk is the normal path and the painter's walk is the
 * exception. That ordering exists for the SPAN BUFFER: whatever draws
 * first claims its pixels, so it must be the nearest thing.
 *
 * There is no span buffer here and there is a real depth buffer, so
 * front-to-back buys nothing - and it actively breaks blending, since
 * a translucent primitive drawn before what is behind it has nothing
 * to blend against. Back-to-front is the ordering a D3D9 back end
 * wants. renderer.txt section 3.4.2 says the same thing from the other
 * direction: with a depth buffer the opaque geometry no longer needs
 * the order, but the deferred - translucent - kinds still do.
 */
int g_spanBufferEnabled = 0;
int g_queuePrimLimit    = -1;   /* debug; see queue.h */

static MR_Primitive  *s_split;
static int            s_splitUsed;
static MR_QueueNode  *s_nodes;
static MR_QueueNode **s_buckets;
static int            s_nodeCount;

/*
 * The live bucket range. DAT_0068b030 is the low water mark and starts
 * at 7999; DAT_0065e1d0 is the high and starts at -1. The range starts
 * INVERTED - high below low - which is how the walk knows the queue is
 * empty without a separate flag.
 */
static int s_lowBucket  = QUEUE_BUCKET_TOP;
static int s_highBucket = -1;

static int s_statBucketLo;
static int s_statBucketHi;
static int s_statQueued;
static int s_statDispatched;
static int s_statClipped;
static int s_statDroppedNear;
static int s_statDroppedFull;

int queue_create(void)
{
	queue_destroy();

	s_split   = (MR_Primitive *)malloc(sizeof(MR_Primitive) *
	                                  QUEUE_SPLIT_MAX);
	s_nodes   = (MR_QueueNode *)malloc(sizeof(MR_QueueNode) *
	                                   QUEUE_NODE_MAX);
	s_buckets = (MR_QueueNode **)malloc(sizeof(MR_QueueNode *) *
	                                    QUEUE_BUCKETS);
	if (!s_nodes || !s_buckets || !s_split) {
		queue_destroy();
		return 0;
	}
	detail_init();
	queue_reset();
	return 1;
}

void queue_destroy(void)
{
	free(s_split);   s_split   = NULL;
	free(s_nodes);   s_nodes   = NULL;
	free(s_buckets); s_buckets = NULL;
	s_nodeCount = 0;
}

void queue_reset(void)
{
	if (s_buckets)
		memset(s_buckets, 0, sizeof(MR_QueueNode *) * QUEUE_BUCKETS);
	s_nodeCount  = 0;
	s_splitUsed  = 0;
	s_lowBucket  = QUEUE_BUCKET_TOP;
	s_highBucket = -1;

	s_statClipped     = 0;
	s_statBucketLo    = QUEUE_BUCKET_TOP;
	s_statBucketHi    = -1;
	s_statQueued      = 0;
	s_statDispatched  = 0;
	s_statDroppedNear = 0;
	s_statDroppedFull = 0;
}

/* ------------------------------------------------------------------ */

/*
 * The bucket. __ftol truncates toward zero, so this is a cast and not
 * the round-to-nearest of the transform - two different conversions in
 * the same subsystem, and mixing them up would shift every primitive
 * by up to one bucket.
 *
 * A NEGATIVE bucket returns without queueing anything at all. That is
 * the original: nothing clamps a negative depth up to zero, it is
 * simply dropped. A face nearer than the origin is thrown away.
 */
/* 1.0 units per bucket, the original. See queue_set_units_per_bucket. */
static float s_bucketRecip = 1.0f;

void queue_set_units_per_bucket(float u)
{
	s_bucketRecip = (u > 0.0f) ? (1.0f / u) : 1.0f;
}

static int bucket_for(float depth, int bucketMode, int *out)
{
	int b;

	if (bucketMode == QUEUE_MODE_NEAREST) {
		*out = 0;
		return 1;
	}
	if (bucketMode == QUEUE_MODE_TOP) {
		*out = QUEUE_BUCKET_TOP;
		return 1;
	}

	b = (int)(depth * s_bucketRecip);
	if (b < 0)
		return 0;
	if (b > QUEUE_BUCKET_MAX)
		b = QUEUE_BUCKET_MAX;
	*out = b;
	return 1;
}

/*
 * Append one node. Split out of queue_primitive so the clip path can
 * queue its fan triangles without duplicating the bookkeeping.
 */
static int s_queuePalette = -1;    /* set around a paletted call */

static void append_node(MR_Primitive *prim, float depth,
                        MR_Vertex *vertices, int bucket)
{
	MR_QueueNode *node;

	if (s_nodeCount >= QUEUE_NODE_MAX) {
		s_statDroppedFull++;
		return;
	}
	node = &s_nodes[s_nodeCount++];
	node->prim     = prim;
	node->depth    = depth;
	node->vertices = vertices;
	node->palette  = s_queuePalette;
	node->texSlot  = -1;

	node->next        = s_buckets[bucket];
	s_buckets[bucket] = node;
	s_statQueued++;
}

/*
 * queue_clipped - clip a face against the near plane and queue the
 * pieces, which is what Screen3D_ClipTriangleNear exists to do.
 *
 * The shape follows Screen3D_QueuePrimitive's own split path:
 *
 *   o the new vertices go into the SAME per-frame bump pool the
 *     transform fills, and the queued node points at that slice with
 *     indices relative to it - the original does exactly this, taking
 *     &g_vertexPool[g_vertexPoolCount] as the node's vertex base and
 *     then using indices 0, i+1, i+2
 *
 *   o the polygon is fanned from corner 0, and each triangle gets its
 *     own MR_Primitive out of a split pool, because the mesh's face
 *     array is shared and must not be written
 *
 *   o a triangle is stored as a degenerate quad - vertexIndex[3]
 *     repeats [0] and [4] is 0xffff - which is what
 *     Screen3D_MakeTriangle does and what every "is this a quad" test
 *     downstream reads
 */
static void queue_clipped(MR_Primitive *src, float depth,
                          MR_Vertex *vertices, int bucket)
{
	const MR_Vertex *in[4];
	float            inUV[8];
	MR_Vertex        clipped[CLIP_MAX_CORNERS];
	float            clipUV[CLIP_MAX_CORNERS * 2];
	MR_Vertex       *slice;
	int              n;
	int              count;
	int              i;

	n = (src->vertexIndex[4] != PRIM_NO_WRAP) ? 4 : 3;
	for (i = 0; i < n; i++) {
		in[i]          = &vertices[src->vertexIndex[i]];
		inUV[i * 2]     = src->uv[i * 2];
		inUV[i * 2 + 1] = src->uv[i * 2 + 1];
	}

	count = clip_polygon_near(in, inUV, n, clipped, clipUV);
	if (count < 3) {
		s_statDroppedNear++;
		return;
	}

	slice = vertexpool_alloc(count);
	if (!slice) {
		s_statDroppedNear++;
		return;
	}
	for (i = 0; i < count; i++)
		slice[i] = clipped[i];

	for (i = 0; i + 2 < count; i++) {
		MR_Primitive *t;

		if (s_splitUsed >= QUEUE_SPLIT_MAX) {
			s_statDroppedFull++;
			return;
		}
		t = &s_split[s_splitUsed++];

		/*
		 * Copy the source wholesale, then overwrite what the split
		 * changes. Screen3D_MakeTriangle copies kind, colour index,
		 * bank index and flags across the same way - and note it takes
		 * the EFFECTIVE kind and writes it to both kind fields, so a
		 * split triangle carries the reduced kind as though it had
		 * been authored that way. A port must not "restore" the
		 * original kind here.
		 */
		*t = *src;
		t->kind = t->effectiveKind = src->effectiveKind;

		t->vertexIndex[0] = 0;
		t->vertexIndex[1] = (unsigned short)(i + 1);
		t->vertexIndex[2] = (unsigned short)(i + 2);
		t->vertexIndex[3] = 0;
		t->vertexIndex[4] = PRIM_NO_WRAP;

		t->uv[0] = clipUV[0];
		t->uv[1] = clipUV[1];
		t->uv[2] = clipUV[(i + 1) * 2];
		t->uv[3] = clipUV[(i + 1) * 2 + 1];
		t->uv[4] = clipUV[(i + 2) * 2];
		t->uv[5] = clipUV[(i + 2) * 2 + 1];
		/* The wrap slot for a triangle repeats corner 0. The original
		 * writes uv[6] and uv[7] here and leaves uv[8..9] alone; we
		 * set them too rather than inherit whatever the copy brought. */
		t->uv[6] = t->uv[8] = clipUV[0];
		t->uv[7] = t->uv[9] = clipUV[1];

		append_node(t, depth, slice, bucket);
		s_statClipped++;
	}
}

/*
 * ORIGIN  Screen3D_QueuePrimitivePaletted @ 0x0049ca70
 *
 * It is queue_primitive with a livery, so it is queue_primitive with
 * a livery: the palette is stashed for append_node to stamp on every
 * node the call produces - the clip path can produce several - and
 * put back afterwards, so an unpaletted caller can never inherit it.
 */
void queue_primitive_paletted(MR_Primitive *prim, float depth,
                              MR_Vertex *vertices, unsigned int hwFlags,
                              int bucketMode, int palette)
{
	int saved = s_queuePalette;

	s_queuePalette = palette;
	queue_primitive(prim, depth, vertices, hwFlags, bucketMode);
	s_queuePalette = saved;
}

void queue_primitive(MR_Primitive *prim, float depth, MR_Vertex *vertices,
                     unsigned int hwFlags, int bucketMode)
{
	int bucket;

	if (!s_nodes || !s_buckets || !prim)
		return;

	if (!bucket_for(depth, bucketMode, &bucket))
		return;

	if (s_highBucket < bucket) s_highBucket = bucket;
	if (bucket < s_lowBucket)  s_lowBucket  = bucket;
	if (s_statBucketHi < bucket) s_statBucketHi = bucket;
	if (bucket < s_statBucketLo) s_statBucketLo = bucket;

	/* Level of detail: one substitution, and both back ends read only
	 * the result. detail.h. */
	prim->effectiveKind = (unsigned short)
	    detail_effective_kind(prim->kind, prim->flags, depth);

	/*
	 * THE NEAR-PLANE CLIP.
	 *
	 * On the hardware path with hwFlags non-zero the original splits
	 * the face here - Screen3D_MakeTriangle when no vertex crosses the
	 * near plane, Screen3D_ClipTriangleNear when one does.
	 *
	 * Bit 0 is the one that matters here: a corner behind the near
	 * plane has NO VALID screenX or screenY, because transform.c
	 * writes those only when camZ passes, so nothing downstream can
	 * draw it.
	 *
	 * BIT 1 IS SKIPPED, AND IT NO LONGER NEEDS DOING. It means
	 * "extends outside the viewport", and the original splits the
	 * face so its software rasteriser never sees an off-screen
	 * coordinate. We let the hardware do it instead.
	 *
	 * o THAT ONLY BECAME TRUE WHEN THE VERTICES STOPPED BEING
	 *   PRE-TRANSFORMED. The note here used to say "D3D9 clips", which
	 *   was false while render.h submitted D3DFVF_XYZRHW: screen-space
	 *   vertices are never clipped in 3D, they are rasterised in 2D
	 *   against a finite guard band, and a triangle with a corner far
	 *   outside that band can be dropped whole by the driver. It
	 *   showed as faces popping in and out at the edges of the screen
	 *   on page 25 - the letters as they swung wide, the camera model
	 *   as it receded.
	 *
	 * o SCENE GEOMETRY NOW GOES UNTRANSFORMED, in camera space, with
	 *   a projection matrix on the device, so D3D9 clips it against a
	 *   real frustum. See r_set_projection in render.h. The claim in
	 *   this comment is finally the truth rather than the assumption
	 *   it started as.
	 */
	if (hwFlags & 1) {
		queue_clipped(prim, depth, vertices, bucket);
		return;
	}

	/* Head insertion, so a bucket ends up in reverse queue order. The
	 * original does the same and nothing depends on the order within
	 * a bucket. */
	append_node(prim, depth, vertices, bucket);
}

/* ------------------------------------------------------------------ */

void queue_walk(void (*dispatch)(const MR_QueueNode *node))
{
	MR_QueueNode *node;
	int           count = 0;
	int           high;
	int           i;

	if (!s_buckets || !dispatch)
		return;

	high = s_highBucket;

	/*
	 * The high-water clamp, transcribed because it looks wrong and is
	 * not:
	 *
	 *     if (high > 0x1f3e) high = 0x1f3e;
	 *     if (high < 0x1f36) high = 0x1f3e;
	 *
	 * The second line forces the walk to the top of the range whenever
	 * the tracked high is below 7990 - which is almost every frame. It
	 * reads like a bug until you notice THE WALK ALSO CLEARS EACH
	 * BUCKET HEAD IT PASSES. Forcing the high end guarantees every
	 * bucket from the low mark upward is cleared for the next frame.
	 * The cost is scanning thousands of empty buckets; the benefit is
	 * not needing a separate clear pass.
	 */
	if (high > QUEUE_BUCKET_MAX)
		high = QUEUE_BUCKET_MAX;
	if (high < 0x1f36)
		high = QUEUE_BUCKET_MAX;

	if (s_lowBucket <= high) {
		if (g_spanBufferEnabled == 0) {
			/* Far to near: the painter's algorithm. */
			for (i = high; i >= s_lowBucket; i--) {
				for (node = s_buckets[i]; node; node = node->next) {
					if ((float)count <= QUEUE_PRIM_LIMIT) {
						count++;
						if (g_queuePrimLimit < 0 ||
						    count <= g_queuePrimLimit)
						    dispatch(node);
					}
				}
				s_buckets[i] = NULL;
			}
		} else {
			/* Near to far, what the span buffer needs. */
			for (i = s_lowBucket; i <= high; i++) {
				for (node = s_buckets[i]; node; node = node->next) {
					if ((float)count <= QUEUE_PRIM_LIMIT) {
						count++;
						if (g_queuePrimLimit < 0 ||
						    count <= g_queuePrimLimit)
						    dispatch(node);
					}
				}
				s_buckets[i] = NULL;
			}
		}
	}

	/*
	 * BUCKET 7999 IS DRAWN LAST, IN BOTH DIRECTIONS, AND IS EXEMPT
	 * FROM THE PRIMITIVE LIMIT.
	 *
	 * This was not obvious from the decompilation, which shows a
	 * separate global DAT_0065da34 walked after the loop. That address
	 * is not a separate list: the bucket array base is 0x00655d38 and
	 * 0x00655d38 + 7999*4 is 0x0065da34 exactly, so it is the LAST
	 * BUCKET, which the ftol path can never reach because it clamps at
	 * 7998. Only bucketMode 2 puts anything there.
	 *
	 * So it is the "always on top, always drawn, never budgeted"
	 * bucket - which is what a HUD or an overlay wants.
	 *
	 * The stat is taken BEFORE this loop, so it keeps meaning what it
	 * used to: primitives dispatched against the game's own budget.
	 * The debug limit below still counts the top bucket, because a
	 * bisection tool that cannot reach some primitives is not much of
	 * a bisection tool - but that is a debug concern and it does not
	 * get to redefine the statistic.
	 */
	s_statDispatched = count;

	for (node = s_buckets[QUEUE_BUCKET_TOP]; node; node = node->next) {
		count++;
		if (g_queuePrimLimit < 0 || count <= g_queuePrimLimit)
			dispatch(node);
	}
	s_buckets[QUEUE_BUCKET_TOP] = NULL;

	s_lowBucket  = QUEUE_BUCKET_TOP;
	s_highBucket = -1;
	s_nodeCount  = 0;
}

void queue_bucket_range(int *lo, int *hi)
{
	if (lo) *lo = s_statBucketLo;
	if (hi) *hi = s_statBucketHi;
}

int queue_clipped_count(void)
{
	return s_statClipped;
}

void queue_stats(int *queued, int *dispatched, int *droppedNearPlane,
                 int *droppedFull)
{
	if (queued)           *queued           = s_statQueued;
	if (dispatched)       *dispatched       = s_statDispatched;
	if (droppedNearPlane) *droppedNearPlane = s_statDroppedNear;
	if (droppedFull)      *droppedFull      = s_statDroppedFull;
}
