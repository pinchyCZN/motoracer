/*
 * trig.h - the engine's cosine table.
 *
 * g_cosTable at 0x005fcb80, built by Trig_BuildCosTable (0x004901e0)
 * from App_Init. 0x4000 floats, one full turn, and it is the angle
 * source for every rotation in the game: 335 references, of which the
 * only two writes are the two stores in the builder.
 *
 * ANGLES ARE 14-BIT FIXED POINT. 0x4000 to the turn, 0x1000 to the
 * quarter, so an angle is an int and wrapping is a mask rather than a
 * fmod. That is the convention the whole engine speaks; a port that
 * switches to radians has to convert at every boundary, so this keeps
 * it.
 *
 * IT IS COSINE, NOT SINE, and that was an open question until the
 * builder was found. It matters more than it looks: roughly 150 sites
 * read entry 0 as a bare absolute address, and cos(0) is 1.0 where
 * sin(0) would be 0.0 - the difference between a multiply that does
 * nothing and one that erases its operand.
 *
 * See docs/structs/MR_MenuCamera.txt section 4.
 */

#ifndef MRACER_TRIG_H
#define MRACER_TRIG_H

#ifdef __cplusplus
extern "C" {
#endif

#define TRIG_TABLE_SIZE 0x4000      /* one turn                    */
#define TRIG_MASK       0x3fff      /* wrap, as the callers do     */
#define TRIG_QUARTER    0x1000      /* 90 degrees                  */

/*
 * THE LITERAL IS NOT 2*pi AND THAT IS DELIBERATE HERE.
 *
 * The double at 0x004d1e38 is 6.283185307178 - thirteen significant
 * digits, short of the true 2*pi by 1.6e-12, which is about 7150 ulp.
 * Somebody typed it out rather than deriving it.
 *
 * Reproduced exactly rather than corrected. Using M_PI would give a
 * table that differs from the game's in the last bit or two of some
 * entries, and the whole point of a table is that everything reading
 * it agrees. The error is far below float precision anyway: the built
 * table is bit-identical to one built from the true 2*pi for all but a
 * handful of entries.
 */
#define TRIG_TWO_PI     6.283185307178

/*
 * AND THE GAME HAS A SECOND 2*pi, THE ORDINARY ONE. Do not fold the
 * two together: they are different constants at different addresses
 * serving different jobs, and each is the right answer in its own
 * place [verified, read_memory on every address below].
 *
 *   TRIG_TWO_PI       0x004d1e38   6.283185307178      TYPED
 *                                  builds the cosine table, and
 *                                  everything that INDEXES the table
 *
 *   MR_TWO_PI         0x004d0e58   6.283185307179586   TRUE
 *                     0x004d00e0   the same eight bytes
 *                     0x004d1a48   the same eight bytes
 *                                  ordinary angle arithmetic - the
 *                                  rider, the horizon, the HUD
 *
 *   -MR_TWO_PI        0x004d0d30   the negation, menu.c's spin
 *
 *   as a FLOAT         0x004d1bc8  6.2831854820251465  trk.h's
 *                                  anim-decor constant
 *
 * USING THE WRONG ONE IS INVISIBLE AND WRONG. They differ by 1.6e-12,
 * so nothing will look broken; it is the quantiser that cares. Match
 * the address the code you are transcribing actually loads.
 *
 * See docs/subsystems/trig-table.txt 2.2
 */
#define MR_TWO_PI       6.283185307179586

/* 0x004d0e60, the double directly after MR_TWO_PI's - true pi, and
 * the same rule applies: the table never uses it, ordinary angle
 * arithmetic does. */
#define MR_PI           3.141592653589793

/*
 * AND pi/2 EXISTS AT TWO WIDTHS, which is the one place in this
 * family where picking wrong is visible-ish: they are 4.4e-8 apart,
 * a thousand times the gap between the two 2*pi.
 *
 *   MR_HALF_PI     0x004d0eb0   1.5707963267948966   a DOUBLE
 *                  0x004d0158   the same eight bytes
 *
 *   MR_HALF_PI_F   0x3fc90fdb   1.5707963705062866   a FLOAT
 *                  0x004d1e5c   immediate, and its negation at
 *                  0x004d1e60
 *
 * MATCH THE WIDTH THE ORIGINAL LOADS, not the width of the
 * expression you are writing. A site that loads the float immediate
 * and promotes it wants MR_HALF_PI_F even in double arithmetic;
 * MR_HALF_PI there is wrong by 4.4e-8.
 *
 * (float)MR_HALF_PI and MR_HALF_PI_F are the SAME float, so a site
 * that only ever casts to float may use either.
 */
#define MR_HALF_PI      1.5707963267948966
#define MR_HALF_PI_F    1.5707963705062866f

extern float g_cosTable[TRIG_TABLE_SIZE];

/*
 * trig_build_table - Trig_BuildCosTable, transcribed.
 *
 * Call once at startup, before anything reads the table. The original
 * calls it from App_Init and nowhere else.
 */
void trig_build_table(void);

/*
 * THE CALLERS TAKE A PAIR, always the same way: index i, and its
 * partner at (i - 0x1000) & 0x3fff. Since cos(t - 90deg) = sin(t),
 * that pair is (cos t, sin t) - checked numerically against the built
 * table, not assumed from the identity.
 *
 * So an angle costs two masked array reads and no function call, which
 * is why the original inlines it everywhere instead of calling sinf.
 */
#define trig_cos(i) (g_cosTable[(i) & TRIG_MASK])
#define trig_sin(i) (g_cosTable[((i) - TRIG_QUARTER) & TRIG_MASK])

/*
 * RADIANS TO TABLE INDEX. ONE constant in the binary, pooled at
 * FIVE addresses, all holding the same eight bytes 2b ce c9 6d 30 5f
 * a4 40 [verified, read_memory on each]:
 *
 *     0x004d0ed0   Racer_ComposeOrientation
 *     0x004d0300   maybe_Racer_Draw's inline euler builds
 *     0x004d13a0   decor placement, MR_TrkInstance.rot
 *     0x004d1bf8   Mat34_FromEuler at 0x0048c060
 *     0x004d0ca8   the menu spin, menu.c 7465
 *
 * So one here.
 *
 * IT IS BUILT FROM TRIG_TWO_PI AND NOT FROM THE TRUE 2*pi, and the
 * difference is not academic: the binary reads 2607.5945876182718,
 * which is 0x4000 / 6.283185307178 bit for bit, while 0x4000 / the
 * true 2*pi is 2607.5945876176133 - 1448 ulp away.  Spelling it the
 * obvious way puts a systematic error into every angle the engine
 * quantises.
 *
 * Racer_ComposeOrientation multiplies by it three times, once per
 * Euler angle, and truncates each with __ftol (0x0045cba9, 0x0045cbd8,
 * 0x0045cc07).  Reproducing that quantisation exactly is the whole
 * point, so the constant has to be the one the game typed.
 *
 * The inverse is OURS - the original keeps the index and never
 * converts back - but it uses the same 2*pi so the round trip closes.
 */
#define TRIG_IDX_PER_RAD  (TRIG_TABLE_SIZE / TRIG_TWO_PI)
#define TRIG_RAD_PER_IDX  (TRIG_TWO_PI / TRIG_TABLE_SIZE)

/*
 * THE TABLE, NOT libm, AND THE SUBSTITUTION ONLY LOOKS SAFE. cos() in
 * double and g_cosTable disagree at the quarter turns - neither is
 * zero, and they are not the same non-zero - so calling libm instead
 * seeds a denormal into matOrientation that survives all the way into
 * the rider's pose and costs a trig index on every joint. The table is
 * bit-identical between the two games, so reading it is exact rather
 * than merely closer. racer-draw-audit.txt 48.
 */

#ifdef __cplusplus
}
#endif

#endif /* MRACER_TRIG_H */
