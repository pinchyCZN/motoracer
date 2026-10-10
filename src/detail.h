/*
 * detail.h - the level-of-detail system, which is one table lookup.
 *
 * Screen3D_Open builds two tables in one loop over kinds 0 to 0x45:
 * featureTable maps a kind to eight feature bits, and reverseIndex maps
 * a bit pattern back to the FIRST kind that has it.
 * Screen3D_QueuePrimitive then reduces detail by STRIPPING BITS AND
 * LOOKING THE KIND BACK UP:
 *
 *     prim->effectiveKind = reverseIndex[modified featureTable[kind]];
 *
 * So reducing detail SUBSTITUTES ONE KIND FOR ANOTHER rather than
 * branching inside the rasterizer, and neither back end knows it
 * happened. Full treatment in docs/subsystems/detail-levels.txt.
 *
 * The table itself is GENERATED, not transcribed - src/detail_table.inc
 * comes out of tools/gen_detail_table.py, which parses section 6 of
 * that note and checks its own output against the note's reduction
 * columns for all seventy kinds before emitting anything.
 */

#ifndef MRACER_DETAIL_H
#define MRACER_DETAIL_H

#ifdef __cplusplus
extern "C" {
#endif

/* Seventy kinds, 0x00 to 0x45. A superset of both the 32 entries in
 * g_spanTable and the 43 the software dispatcher switches on, so this
 * is the master classification. */
#define DETAIL_KIND_COUNT 0x46

/*
 * The eight bits. Four are controls and four name real rendering
 * features. Bits 0, 7 and 2 nest strictly and are stripped together;
 * bits 1, 3, 4 and 5 identify one of twelve families.
 */
#define DETAIL_TIER1        0x01   /* quality tier 1                  */
#define DETAIL_GOURAUD      0x02   /* Gouraud shading                 */
#define DETAIL_TIER3        0x04   /* quality tier 3                  */
#define DETAIL_NO_FILTER    0x08   /* do NOT filter this kind         */
#define DETAIL_TRANSLUCENT  0x10   /* 50/50 translucent               */
#define DETAIL_PERSPECTIVE  0x20   /* perspective-correct             */
#define DETAIL_EXEMPT       0x40   /* exempt from reduction           */
#define DETAIL_TIER2        0x80   /* quality tier 2                  */

/* The three tier bits, which are what a reduction strips. */
#define DETAIL_TIER_MASK   (DETAIL_TIER1 | DETAIL_TIER2 | DETAIL_TIER3)

/* Detail level, g_cfgDetailLevel. 0 is the coarsest. */
extern int g_cfgDetailLevel;

/*
 * Set non-zero to bypass reduction entirely, which is what the menu
 * gate and DAT_004f1a18 do in the original. Menu pages get full
 * detail regardless of the setting.
 */
extern int g_detailBypass;

void detail_init(void);

/* featureTable[kind], or 0 for a kind out of range. */
unsigned int detail_bits(unsigned int kind);

/*
 * The substitution, exactly as Screen3D_QueuePrimitive performs it.
 * depth is the face's depth key; the two literal thresholds are 3072.0
 * and 6144.0 and they are the near-distance upgrade of bit 5.
 */
unsigned int detail_effective_kind(unsigned int kind, unsigned int flags,
                                   float depth);

#ifdef __cplusplus
}
#endif

#endif /* MRACER_DETAIL_H */
