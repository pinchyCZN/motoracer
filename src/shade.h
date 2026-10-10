/*
 * shade.h - the hardware shade ramp.
 *
 * The table at DAT_005d43b0, which D3D_DrawScene uses to give each
 * vertex its own colour. This is what makes the hardware path lit: the
 * engine computes intensity on the CPU and looks the shaded channel up
 * here, which is why D3D_EmitProcessVertices asks Direct3D for
 * TRANSFORM and not TRANSFORMLIGHT.
 *
 * NOT THE SAME TABLE AS g_shadeLut. That one is at 0x004f62f0, is 32 x
 * 32 entries of four bytes, belongs to the SOFTWARE rasterizer, and
 * docs/subsystems/rasterizer-pipeline.txt section 31.2 covers it - it
 * still has no writer. This one is 32 x 256 bytes, belongs to the
 * hardware path, and its writer is found. The names are similar and
 * the tables are not; do not merge them.
 */

#ifndef MRACER_SHADE_H
#define MRACER_SHADE_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 32 rows of 256 bytes = 8192. The row is a five-bit intensity, the
 * column is an eight-bit channel value, and the entry is that channel
 * scaled by that intensity.
 */
#define SHADE_LEVELS 32
#define SHADE_STEPS  256

/*
 * The intensity window D3D_DrawScene reads out of MR_Vertex.shade:
 * bits 13..17, five bits, 0..31. Read as the 19.13 fixed point the
 * field actually is, those are integer bits 0..4.
 *
 * D3D_WriteVertex reads a WIDER window of the same field - bits 10..17,
 * eight bits - so the two back-end entry points use the same number at
 * two precisions. See docs/structs/MR_Vertex.txt section 6.3.
 */
#define SHADE_MASK   0x3e000
#define SHADE_SHIFT  13
#define SHADE_MAX    31

void shade_init(void);

/*
 * shade_apply - the colour computation of D3D_DrawScene.
 *
 * Takes the primitive's flat colour and a vertex shade word, returns
 * the shaded diffuse. The primitive's own ALPHA BYTE is preserved,
 * which is what carries the 0x80 the half blend depends on.
 */
unsigned int shade_apply(unsigned int flatColor, int shadeWord);

/* One channel through the ramp, for callers that have already split
 * the colour. */
unsigned char shade_channel(int level, unsigned char channel);

#ifdef __cplusplus
}
#endif

#endif /* MRACER_SHADE_H */
