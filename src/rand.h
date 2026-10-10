/*
 * rand.h - the game's random numbers, per stream.
 *
 * ORIGIN  Rand_Next @ 0x004b1160, Rand_Seed @ 0x004b1140
 *
 * Park-Miller "minimal standard" Lehmer, multiplier 16807 (0x41a7),
 * modulus 2^31 - 1. The state is an ARRAY indexed by a stream number
 * (0x004f2590), and that is the whole point of the design.
 *
 * SEPARATE STREAMS ARE WHY AUDIO CANNOT PERTURB THE SIMULATION. The
 * mixer draws from stream 4 and the trackside camera from stream 0;
 * a shared generator would mean a sample skipped for being out of
 * earshot changed where the camera pointed, and a replay would stop
 * matching. The split is what makes that impossible rather than
 * unlikely.
 *
 * Streams seen in the binary: 0 (Camera_UpdateTrackside), 1
 * (maybe_Rand, the FX spray and the finish jingles), 4
 * (maybe_Snd_TickRacer) and 6 (the replay file). Crypt_Xor passes
 * one in a register and has not been read.
 *
 * THE STREAMS DO NOT START AT ZERO. The state array is
 * INITIALISED DATA with seven hand-picked seeds in it, and only
 * stream 6 is ever re-seeded at run time. Treating the array as
 * bss leaves every unseeded stream stuck on the generator's fixed
 * point - see rand.c.
 */

#ifndef MRACER_RAND_H
#define MRACER_RAND_H

#ifdef __cplusplus
extern "C" {
#endif

/* Seven, measured: 0x004f2590 runs to 0x004f25ab and the string
   "Class CD:" starts at 0x004f25ac. */
#define RAND_STREAMS 7

/*
 * 0..0x7ffe, the original's range - the value is reduced modulo
 * 0x7fff on the way out while the STATE keeps its full 31 bits. A
 * generator that fed the reduced value back would have a far shorter
 * period, so the two must not be conflated.
 *
 * An out-of-range stream returns 0 rather than reading past the
 * array; the original indexes unchecked.
 */
int  rand_next(int stream);
void rand_seed(int stream, int seed);

/*
 * The generator itself, over a caller's own state. bpt.c decodes
 * scripts with this on a per-file state rather than a stream, which
 * is the same arithmetic against a different variable - see
 * docs/formats/bpt.txt 6.
 */
int  rand_step(unsigned int *state);

#ifdef __cplusplus
}
#endif

#endif /* MRACER_RAND_H */
