/*
 * drawscene.h - the hardware consumer of the queue.
 *
 * This is D3D_DrawScene (0x004af570) minus the walk: queue.c owns the
 * walk, and this is what it dispatches to. The original duplicates the
 * walk instead - Screen3D_DrawScene has one copy for the software path
 * and D3D_DrawScene carries its own - which is a difference in shape,
 * not in behaviour. Sharing it is the one place this reconstruction
 * tidies the original, and it is recorded here so it is not mistaken
 * for a finding.
 *
 * Per primitive the original writes its vertices, sets any needed
 * state, and emits a triangle or a line, with a quad becoming two
 * triangles through the mode 2 then mode 1 pair. That is what this
 * does, through render.h.
 */

#ifndef MRACER_DRAWSCENE_H
#define MRACER_DRAWSCENE_H

#include "queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * THE FAR PLANE. Scene vertices go to the renderer in camera space
 * and a projection matrix maps depth, so this is now the f in that
 * matrix rather than a divisor applied per vertex - see
 * r_set_projection. It still has to bound the scene: geometry beyond
 * it is clipped away by D3D9, which the pre-transformed path could
 * not do.
 *
 * It is ours either way. The original writes sz = 1.0 when there is
 * no z buffer and a scaled camZ otherwise, and the scale depends on
 * the depth format.
 */
extern float g_drawSceneZFar;


/*
 * g_cfgGouraud - DAT_004dc1d8, the -NoGouraud switch. DEFAULT 1, so
 * the hardware path IS Gouraud shaded and every corner gets its own
 * colour. See drawscene.c: an earlier reading of this got it backwards
 * and the correction is worth reading once.
 */
extern int g_cfgGouraud;

/* The dispatch. Hand this to queue_walk. */
void drawscene_dispatch(const MR_QueueNode *node);

/*
 * drawscene_force_point_filter - override the feature table's filter
 * bit for the next walk. 0 restores the table.
 *
 * ONE CALLER, AND IT SHOULD STAY THAT WAY: the glass shatter. Its
 * texture is a 256x192 screen capture stretched back over the whole
 * screen, a 2.5x magnification, and kind 0x10's feature byte is 0x81
 * - bit 3 clear, so the table asks for LINEAR. That combination never
 * shipped: linear is the D3D path's answer and the D3D path never
 * showed this effect at all, because retail's shatter comes out black
 * there. What retail actually displays is the SOFTWARE rasteriser's
 * point sampling. See glass.c and menu-pages.txt 4.3.1.4.
 */
void drawscene_force_point_filter(int on);

/* Reset the per-frame counters. */
void drawscene_begin(void);
/*
 * drawscene_set_depth_test - turn the per-primitive depth test
 * off for a pass that has to sit over the scene.
 *
 * The original does this per VERTEX - Hud_DrawMap pins invZ to
 * 1.0 so the map always wins - but our vertices reach the
 * renderer in camera space and the depth comes from camZ, so
 * invZ has nothing to feed. Same intent, one level up.
 *
 * REMEMBER TO PUT IT BACK. It is global state, not a parameter.
 */
void drawscene_set_depth_test(int on);

void drawscene_stats(int *triangles, int *lines, int *skipped);

#ifdef __cplusplus
}
#endif

#endif /* MRACER_DRAWSCENE_H */
