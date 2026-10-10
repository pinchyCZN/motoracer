/*
 * horizon.h - the scrolling 2D sky panorama.
 *
 * THERE IS NO DOME AND NO SKYBOX. The sky is one 256x256 .RAW page
 * cut into two strips, tiled alternately across the screen and
 * scrolled by the camera's heading, with the area above and below
 * filled flat from the strip's own corner texels.
 * docs/subsystems/dd-statics.txt 7.2.
 *
 * FUN_00480930 registers the sprites, Dd_DrawHorizon @ 0x00402960
 * draws them, maybe_Track_SetHorizonBanks @ 0x00410350 loads the
 * page. Between the three that is the whole sky.
 *
 * [open] only the HiRes variant is built. The original keeps three
 * and picks by g_cfgScreenMode; dd-statics.txt 7.2.2.
 */

#ifndef MRACER_HORIZON_H
#define MRACER_HORIZON_H

#include "tracks.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * horizon_load - read "<track>BANK\HORIZHI.RAW" into bank slot `slot`.
 *
 * bank_load MODE 1, NOT 0: the mode-0 skip that swallows a 256x256
 * truecolour page does not apply on this path. dd-statics.txt 7.2.2.
 *
 * Returns 1 on success. On failure horizon_draw is a no-op, so a
 * missing page costs a black sky and nothing else.
 */
int horizon_load(const MR_Track *t, int slot);

/*
 * horizon_draw - paint the sky into the framebuffer.
 *
 * CALL BEFORE THE 3D BRACKET: this is a backdrop and writes every
 * pixel it covers, so anything drawn first is lost.
 *
 * viewEye fixes the yaw and pitch; of eye only the HEIGHT is used.
 * yBias and damp are MR_Trk's, per track.
 *
 * `lens` must be the projection scale the 3D pass is about to use.
 * The horizon carries none of its own - the original reads the live
 * g_projScaleX at 0x006d20c4 so the sky and the world stay
 * consistent through a zoom. projection.txt.
 */
void horizon_draw(const float eye[3], const float viewEye[3],
                  const float target[3], float lens,
                  int yBias, float damp);

/* Forget the bank slot. Does not free the bank itself. */
void horizon_free(void);

#ifdef __cplusplus
}
#endif

#endif /* MRACER_HORIZON_H */
