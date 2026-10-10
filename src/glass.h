/*
 * glass.h - the menu page transition, a shattering pane of glass.
 *
 * From menu/menuTools.cpp. The pane is data\menu\meshes\vitre.3df -
 * "vitre" is French for a pane of glass - 59 vertices and 99
 * irregular triangles, flat, spanning exactly 640.8 x 480.3, i.e.
 * authored in screen pixels.
 *
 * THE WHOLE EFFECT, in call order [verified]:
 *
 *   Menu_LoadResources      Bankw_CreateBank(2) - an EMPTY page, no
 *   0x0040aa..0x0040abbf    file behind it - then Load3df on the
 *                           pane and MenuGlass_BuildShards to cut it
 *                           up. Runs ONCE per run.
 *   Menu_BeginTransition    captures the screen into bank 2 and
 *   0x0040c4a0              raises g_nTransitionRequested.
 *   Menu_Tick 0x00409cfe    sees the request, sets transitionActive
 *                           and stamps a 1200 ms window.
 *   Menu_UpdateTransition   progress 0..1 across it, calls the draw.
 *   0x0040c500
 *   Menu_DrawTransitionEffect  per shard, per frame.
 *   0x00479c10
 *
 * o THE CAPTURE HAPPENS BEFORE THE PAGE CHANGES, which is the whole
 *   trick: bank 2 holds a picture of the page the player is LEAVING,
 *   and the shards carry it away.
 *
 * o THE SHARDS ARE IN CAMERA SPACE. The draw builds a matrix per
 *   shard and hands it straight to transform_verts_flat with no view
 *   and no camera anywhere. That is why the pane sits at camZ 1911,
 *   the same 1911 the menu uses as its projection scale: at that
 *   distance the projection is 1:1 and 640.8 x 480.3 units cover the
 *   screen pixel for pixel.
 *
 * o HOW RANDOM IT ACTUALLY IS. Each shard gets three rand() spin
 *   rates - but at BUILD time, which is once per run. Every
 *   transition in a session therefore tumbles identically, and the
 *   break pattern never varies at all, being the mesh's own
 *   triangulation. What makes each transition look different is the
 *   texture. See docs/subsystems/menu-pages.txt section 4.3.1.1.
 */
#ifndef GLASS_H
#define GLASS_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Load the pane and cut it into shards, and create the capture bank.
 * Safe to call twice; the second call rebuilds, which is what
 * MenuPage11_Options_Tick does when the display mode changes - and is
 * the only thing that ever re-rolls the spin rates.
 *
 * Returns 1 when the effect is usable. On failure the transition
 * simply does not draw, which is the right failure: a menu that
 * changes page without an effect still works.
 */
int  glass_init(void);
void glass_shutdown(void);
int  glass_ready(void);

/*
 * Menu_BeginTransition. Ask for a transition: the screen is captured
 * at the end of THIS frame and the clock starts on the next one.
 *
 * The two-step is the original's, not an artefact - it raises a
 * request flag and Menu_Tick arms transitionActive on a later pass.
 */
void glass_request(void);

/*
 * The capture half of glass_request, which cannot happen where the
 * request is made.
 *
 * CALL IT AFTER THE SCENE IS DRAWN AND BEFORE THE PRESENT. Our back
 * buffer is D3DSWAPEFFECT_DISCARD, so its contents are undefined once
 * Present has run; the original reads its own persistent software
 * framebuffer and has no such constraint. See r_capture_screen.
 */
void glass_capture_if_pending(void);

/* Arm if requested, advance the clock, and queue the shards. Call it
 * once a frame, after the page has drawn its own 3D. */
void glass_update(void);

/* Non-zero while the effect is running. */
int  glass_active(void);

#ifdef __cplusplus
}
#endif

#endif /* GLASS_H */
