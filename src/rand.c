/*
 * rand.c - Rand_Next and Rand_Seed. See rand.h.
 */

#include "rand.h"

/*
 * ORIGIN  Rand_Next @ 0x004b1160, the arithmetic
 *
 *     hi = (state >> 16) * 16807
 *     r  = ((hi & 0x7fff) << 16) + (state & 0xffff) * 16807 + (hi >> 15)
 *     if (r < 0) r -= 0x7fffffff
 *
 * THE ORIGINAL SHIFTS SIGNED and this shifts unsigned, which is the
 * same thing here: the step below drives the state non-negative and
 * keeps it there, so the sign bit is never set going in. This is the
 * form bpt.c has decoded every shipped script with.
 *
 * THE STATE AND THE RETURN VALUE ARE NOT THE SAME NUMBER. The state
 * keeps all 31 bits; the value handed back is reduced modulo 0x7fff
 * when it exceeds it. Feeding the reduced value back would shorten
 * the period drastically - and in bpt.c's case would decode the
 * first byte of a file correctly and nothing after it.
 */
#define RAND_MULT 16807U

int rand_step(unsigned int *state)
{
	unsigned int s;
	unsigned int hi;
	unsigned int lo;
	unsigned int r;

	if (!state)
		return 0;

	s  = *state;
	hi = (s >> 16) * RAND_MULT;
	lo = (s & 0xffffU) * RAND_MULT;
	r  = ((hi & 0x7fffU) << 16) + lo + (hi >> 15);

	if (r & 0x80000000U)
		r = r - 0x7fffffffU;
	*state = r;
	return (int)((r > 0x7fffU) ? (r % 0x7fffU) : r);
}

/*
 * THE SEEDS ARE INITIALISED DATA AND THEY MATTER.
 *
 * ORIGIN  the array at 0x004f2590, seven dwords, read out of the
 *         image. It is NOT bss.
 *
 * A ZERO STREAM IS A DEAD STREAM: 0 is the Lehmer generator's
 * fixed point, so a stream that starts at 0 returns 0 for ever.
 * Nothing seeds stream 4 at run time - the only Rand_Seed call in
 * the image is the replay file's, on stream 6 - so if these were
 * zeros the mixer's every draw would be 0. That is not abstract:
 * it made the brake cue permanently brake1, the throttle blip
 * fire on every edge instead of one in four, and the engine
 * volume wander sit still at 240.
 *
 * Confirmed against the running retail game: streams 1, 4 and 6
 * had moved off these values and 0, 2, 3 and 5 had not, which is
 * exactly the set the binary draws from.
 */
static unsigned int s_state[RAND_STREAMS] = {
	0x89696798u, 0xbadebeafu, 0x39859835u, 0x33243094u,
	0x1a3d5e7fu, 0x45f8a6c1u, 0x12345678u
};

int rand_next(int stream)
{
	if (stream < 0 || stream >= RAND_STREAMS)
		return 0;
	return rand_step(&s_state[stream]);
}

void rand_seed(int stream, int seed)
{
	if (stream < 0 || stream >= RAND_STREAMS)
		return;
	s_state[stream] = (unsigned int)seed;
}
