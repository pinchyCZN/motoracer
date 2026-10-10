/*
 * cull.h - the stage between the transform and the queue.
 *
 * Takes an ALREADY transformed MR_Vertex array and a face array, culls,
 * and submits what survives. From Screen3D_CullAndQueueFaces
 * (0x0049c110), whose plate comment is the specification.
 *
 * THERE ARE THREE SUBMITTERS IN THE ORIGINAL, not one, and they share a
 * single body - same near-plane rejection, same guard-band counters,
 * same choice between a camera-space triple product and a screen-space
 * 2D cross product, same 1.0f minimum-area test, same hwFlags bits.
 * They differ only in the depth key:
 *
 *   Screen3D_CullAndQueueFaces      0x0049c110   6 callers
 *       farthest camZ; splits paletted from plain
 *   Screen3D_CullAndQueueFacesEx    0x0049b7f0  12 callers
 *       nearest OR farthest by argument, +1024.0 when flags bit 1 is
 *       set. The only reader of MR_Primitive flags bit 1 in the image.
 *   Screen3D_CullAndQueueTranslucent 0x0049bca0  1 caller
 *       farthest * 0.2 - nearPlaneZ, and it REWRITES the kind. Its
 *       caller feeds it g_meshGuidon and g_meshCockpit: it is the
 *       first-person handlebars and dashboard.
 *
 * Only the first is reconstructed here, because it is the one the
 * static-geometry path uses. See rasterizer-pipeline.txt section 50.
 *
 * A NOTE FOR WHEN THE OTHER TWO ARRIVE. Both of their depth tricks -
 * the +1024.0 bias and the times-0.2 - are SORT-KEY tricks that only
 * make sense against a bucketed painter's walk. A real depth buffer
 * cannot express either as a bias. The faithful translation is to keep
 * the ORDERING: draw them in an earlier or later pass rather than
 * moving a z value.
 */

#ifndef MRACER_CULL_H
#define MRACER_CULL_H

#include "prim.h"
#include "transform.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The guard band and the viewport bounds. The original keeps these as
 * globals and the cull reads them directly.
 *
 *     g_viewMaxX       0x0062e448
 *     g_viewLeftMargin 0x00624374
 *     g_viewMaxY       0x00625784
 *     g_viewTopMargin  0x006d1040
 */
extern int g_viewMaxX;
extern int g_viewMaxY;
extern int g_viewLeftMargin;
extern int g_viewTopMargin;

/*
 * Set when a hardware back end is up. It gates the "extends outside
 * the viewport" test that produces hwFlags bit 1 - the software path
 * never computes that bit at all, because its span clippers handle
 * the edges themselves.
 */
extern int g_rendererHardware;

/*
 * cull_and_queue_faces - per face, in order:
 *
 *   1  skip if (flags & 1) and kind == 0x0d
 *   2  find the vertex with the GREATEST camZ; that vertex is both the
 *      near-plane test subject and the depth key
 *   3  reject the face if that maximum camZ <= nearPlaneZ
 *   4  if ANY of the four vertices is at or behind the near plane, set
 *      hwFlags bit 0 and compute the facing test as a scalar triple
 *      product in CAMERA space; otherwise run the guard band and
 *      compute it as a 2D cross product of the SCREEN coordinates
 *   5  on hardware only, set hwFlags bit 1 if any vertex is outside
 *      the viewport
 *   6  submit when the facing value exceeds 1.0f
 */
/*
 * THE BACK PLANE, in camera z. LenBPlane out of the level manifest -
 * 18000 on ile01. A face is dropped only when its NEAREST corner is
 * beyond it, so a mesh straddling the plane keeps the half in front.
 *
 * That per-face granularity is the point. Culling whole meshes
 * against it does nothing for the ocean, which is one enormous mesh
 * with a near edge at the shore and a far edge past the horizon.
 *
 * 0 disables it.
 */
extern float g_backPlaneZ;


/*
 * THE FOURTH ARGUMENT IS THE LIVERY, and -1 means "this face is not
 * paletted". It is ModelInstance_Draw's +0x14c and Racer_Create's
 * param_3 & 7 - which palette of the face's bank resolves it - and it
 * is why every AI rider in a retail race wears a different colour off
 * ONE page. See docs/subsystems/liveries.txt.
 *
 * It used to be missing, and that is the whole reason the port drew
 * twenty identical bikes.
 */
/* Returns the number of faces actually QUEUED, which is what a
 * caller budgeting queue space needs - not the number offered. */
int cull_and_queue_faces(MR_Vertex *verts, MR_Primitive *faces,
                          int faceCount, int livery);

/*
 * cull_and_queue_faces_ex - Screen3D_CullAndQueueFacesEx.
 *
 * THE FOURTH ARGUMENT IS READ: it selects the sort direction, and
 * cull_and_queue_faces_ex is the only way to reach the farthest-
 * corner sort. The DEPTH BIAS and the stricter kind-0x0d skip are
 * still not reproduced and are marked at the definition.
 */
/*
 * cull_and_queue_translucent - Screen3D_CullAndQueueTranslucent
 * (0x0049bca0), the first-person cockpit's submitter.
 *
 * Same cull as cull_and_queue_faces. It then forces every non-zero
 * kind into the blended family and compresses the depth key to a fifth
 * so the faces land in the nearest buckets. IT REWRITES faces IN
 * PLACE, as the original does.
 */
int cull_and_queue_translucent(MR_Vertex *verts, MR_Primitive *faces,
                               int faceCount);

/* 0x004d1f98. A fifth of the true depth - see the definition. */
#define CULL_TRANSLUCENT_DEPTH_K 0.2f

int cull_and_queue_faces_ex(MR_Vertex *verts, MR_Primitive *faces,
                            int faceCount, int flag);

/* Faces examined and faces submitted since the last cull_reset_stats. */
void cull_stats(int *examined, int *submitted);
void cull_reset_stats(void);

#ifdef __cplusplus
}
#endif

#endif /* MRACER_CULL_H */
