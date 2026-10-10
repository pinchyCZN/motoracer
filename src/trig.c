/*
 * trig.c - see trig.h.
 */

#include <math.h>

#include "trig.h"

float g_cosTable[TRIG_TABLE_SIZE];

/*
 * Trig_BuildCosTable (0x004901e0), read from the disassembly. Sixteen
 * instructions, and two of them are worth transcribing carefully.
 *
 *   004901e3  XOR EAX,EAX
 *   004901e5  JMP 0x004901ee
 *   004901e7  FSTP float ptr [EAX*0x4 + 0x5fcb7c]
 *   004901ee  MOV dword ptr [ESP],EAX
 *   004901f2  INC EAX
 *   004901f3  FILD dword ptr [ESP]
 *   004901f7  CMP EAX,0x4000
 *   004901fc  FMUL double ptr [0x004d1e38]      ; 6.283185307178
 *   00490202  FMUL float ptr [0x004d1e40]       ; 1.0f/16384
 *   00490208  FCOS
 *   0049020a  JL 0x004901e7
 *   0049020c  FSTP float ptr [EAX*0x4 + 0x5fcb7c]
 *
 * 1. THE BASE IS 0x5fcb7c, NOT 0x5fcb80, and the index register has
 *    already been incremented when the store happens. So the store
 *    address is (i+1)*4 + 0x5fcb7c, which is i*4 + 0x5fcb80 - entry i,
 *    as intended. Reading the store address literally and concluding
 *    the table starts at 0x5fcb7c would be wrong by one entry.
 *
 * 2. THE COSINE IS COMPUTED BEFORE THE BOUND IS TESTED, so the last
 *    value falls out of the loop with a live FPU stack and is stored
 *    by the duplicate FSTP at 0x0049020c. That is a compiler's loop
 *    rotation, not a source-level special case. All 0x4000 entries are
 *    written; entry 0x3fff is written by the copy.
 *
 *    It also means the two stores are the ONLY writes into the whole
 *    0x10000-byte span - which is how the function was found. A search
 *    for a table-filling loop turned up nothing; asking the reference
 *    manager which instructions WRITE into the span turned up exactly
 *    these two.
 *
 * The two-stage multiply (by 2*pi as a double, then by 1/16384 as a
 * float) is kept as written rather than folded into one constant. The
 * intermediate is an 80-bit FPU register in the original, so folding
 * would change nothing there - but it costs nothing to match, and the
 * shape says plainly what the source said: an angle in turns, scaled
 * to radians.
 */
/* ORIGIN  Trig_BuildCosTable @ 0x004901e0 */
void trig_build_table(void)
{
	int i;

	for (i = 0; i < TRIG_TABLE_SIZE; i++) {
		double a = (double)i * TRIG_TWO_PI * (1.0f / (float)TRIG_TABLE_SIZE);
		g_cosTable[i] = (float)cos(a);
	}
}
