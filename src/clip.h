/*
 * clip.h - the near-plane clipper.
 *
 * A face that crosses the near plane arrives from the cull stage with
 * hwFlags bit 0 set, and it CANNOT BE DRAWN AS IT STANDS: transform.c
 * writes screenX and screenY only when camZ passes the near plane, so
 * at least one corner has no valid screen position and nothing
 * downstream can recover it. The original splits such a face here;
 * queue.c used to drop it.
 *
 * From Screen3D_ClipTriangleNear (0x00497e40) and its only callee
 * Screen3D_ClipEdgeNearHw (0x00497d60). See
 * docs/subsystems/detail-levels.txt section 10.
 *
 *
 * WHAT IS TRANSCRIBED AND WHAT IS NOT.
 *
 * clip_edge_near IS a transcription. Screen3D_ClipEdgeNearHw is 224
 * bytes and was read instruction by instruction, including two details
 * the decompiler hides - see clip.c.
 *
 * THE CASE STRUCTURE IS NOT. The original unrolls six cases - three
 * ways a triangle can have one vertex in front of the plane, three
 * ways it can have two - across 5652 bytes, and that body was read
 * structurally rather than line by line: twelve edge clips in six
 * pairs, ten output-buffer advances, fourteen vertex-pool writes.
 * Which case emits how many triangles, and their winding, were left
 * open.
 *
 * So this implements the same clip as ONE Sutherland-Hodgman loop
 * rather than six unrolled cases. That is a deliberate divergence in
 * SHAPE, not in result: clipping a convex polygon against one plane by
 * walking its edges and keeping the inside points plus the crossings
 * produces exactly the "one vertex in front gives a triangle, two give
 * a quad" split the unrolled cases enumerate.
 *
 * THERE IS A SECOND CLIPPER IN THE ORIGINAL and it is not this one.
 * Screen3D_ClipPolygonViewport (0x00497080) clips in SCREEN space
 * against the four viewport edges, and every triangle from both split
 * paths goes through it. It is not reproduced: D3D9 clips to the
 * viewport itself. What reading it settled is the UV question - see
 * clip.c.
 *
 * WINDING DOES NOT MATTER HERE and that is worth saying rather than
 * leaving as a worry. Sutherland-Hodgman emits its output in input
 * order, so winding is preserved either way - but even if it were not,
 * the cull has already run by the time a face reaches the queue, and
 * the D3D9 back end draws with CULLMODE_NONE precisely because the
 * engine culls on the CPU. Nothing downstream tests winding again.
 */

#ifndef MRACER_CLIP_H
#define MRACER_CLIP_H

#include "transform.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A convex polygon of at most four corners clipped against one plane
 * gains at most one corner, so five is the true bound. Six gives room
 * for a degenerate case to be caught by a length test rather than by
 * memory corruption.
 */
#define CLIP_MAX_CORNERS 6

/*
 * clip_edge_near - Screen3D_ClipEdgeNearHw, transcribed.
 *
 * a is the corner IN FRONT of the near plane and b the one behind;
 * out receives the crossing. Returns the parameter t along a->b, which
 * the original does not need - MR_Vertex carries no texture
 * coordinates - but a caller interpolating UVs does.
 */
float clip_edge_near(const MR_Vertex *a, const MR_Vertex *b,
                     MR_Vertex *out);

/*
 * clip_polygon_near - clip a 3 or 4 corner face against camZ >= near.
 *
 * in     the corners, as pointers into the transformed vertex slice
 * inUV   two floats per corner, in the same order
 * n      3 or 4
 * out    receives up to CLIP_MAX_CORNERS vertices
 * outUV  two floats per output corner
 *
 * Returns the output corner count: 0 when the whole face is behind the
 * plane, n when none of it is, otherwise 3, 4 or 5.
 */
int clip_polygon_near(const MR_Vertex *const *in, const float *inUV,
                      int n, MR_Vertex *out, float *outUV);

#ifdef __cplusplus
}
#endif

#endif /* MRACER_CLIP_H */
