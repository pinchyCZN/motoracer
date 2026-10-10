/*
 * shade.c - see shade.h.
 *
 * The table is EMPTY IN THE IMAGE - all 8192 bytes are zero - and is
 * built at run time. Its only writer is a loop at the end of Cdd_ctor2
 * (0x00473950), reached from Cd3d_Ctor, and the loop is nine
 * instructions:
 *
 *     XOR EDI,EDI                  ; shade level
 *   outer:
 *     XOR EBP,EBP                  ; accumulator
 *     MOV EBX,0x100                ; 256 columns
 *   inner:
 *     MOV EAX,EBP                  ; take the accumulator
 *     INC ESI                      ; flat index
 *     CDQ                          ; and divide it by 32,
 *     AND EDX,0x1f                 ;   signed, toward zero
 *     ADD EBP,EDI                  ; accumulator += level
 *     ADD EAX,EDX
 *     SAR EAX,0x5
 *     DEC EBX
 *     MOV byte ptr [ESI + 0x5d43af],AL
 *     JNZ inner
 *     INC EDI
 *     CMP EDI,0x20
 *     JL outer
 *
 * The accumulator makes it a multiplication by repeated addition, so
 * the whole 8 KB table is
 *
 *     table[level][channel] = channel * level / 32
 *
 * which is a plain linear ramp with no gamma of any kind. In 1997 this
 * bought a table lookup instead of a multiply and a shift per channel
 * per vertex.
 */

#include <string.h>

#include "shade.h"

static unsigned char s_table[SHADE_LEVELS * SHADE_STEPS];
static int           s_ready;

void shade_init(void)
{
	int level;
	int c;

	if (s_ready)
		return;

	for (level = 0; level < SHADE_LEVELS; level++) {
		/* The original accumulates rather than multiplying, which is
		 * the same result; written as the multiply it is. */
		for (c = 0; c < SHADE_STEPS; c++)
			s_table[level * SHADE_STEPS + c] =
		        (unsigned char)((c * level) / SHADE_LEVELS);
	}
	s_ready = 1;
}

unsigned char shade_channel(int level, unsigned char channel)
{
	if (!s_ready)
		shade_init();
	if (level < 0) level = 0;
	if (level > SHADE_MAX) level = SHADE_MAX;
	return s_table[level * SHADE_STEPS + channel];
}

unsigned int shade_apply(unsigned int flatColor, int shadeWord)
{
	unsigned int row;
	unsigned int r;
	unsigned int g;
	unsigned int b;

	if (!s_ready)
		shade_init();

	/*
	 * The original computes the ROW OFFSET in one step -
	 * (shade & 0x3e000) >> 5 - which is the five-bit intensity times
	 * 256 without ever forming the intensity itself. Same thing.
	 */
	row = ((unsigned int)shadeWord & SHADE_MASK) >> 5;

	r = s_table[row + ((flatColor >> 16) & 0xff)];
	g = s_table[row + ((flatColor >>  8) & 0xff)];
	b = s_table[row + ( flatColor        & 0xff)];

	/*
	 * THE PRIMITIVE'S ALPHA BYTE SURVIVES, and the shaded RGB replaces
	 * the rest. That is what carries the 0x80 the half blend depends
	 * on - if this returned an opaque colour the blend of
	 * d3d-render-state.txt section 3.1 would stop being 50 per cent.
	 *
	 * The original ALSO writes the same shaded RGB into the vertex's
	 * SPECULAR field with alpha forced to 0xff. That store is inert:
	 * SPECULARENABLE is emitted from DAT_0053bb34, which Cd3d_Ctor
	 * zeroes in a run of stores right after XOR EAX,EAX and nothing
	 * else in the image touches - the only two references to that
	 * address are that write and the read in D3D_EmitRenderStates. So
	 * the specular component is computed every vertex and never used,
	 * and this reconstruction does not compute it.
	 *
	 * That is worth knowing rather than just skipping: a port that DID
	 * write the specular faithfully, on a device where specular is
	 * enabled, would add the colour to itself and roughly double every
	 * lit surface.
	 */
	return (flatColor & 0xff000000u) | (r << 16) | (g << 8) | b;
}
