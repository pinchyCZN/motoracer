/*
 * detail.c - see detail.h and docs/subsystems/detail-levels.txt.
 */

#include <string.h>

#include "detail.h"
#include "prim.h"

int g_cfgDetailLevel = 3;
int g_detailBypass;

#include "detail_table.inc"

/* reverseIndex is indexed by the FULL BYTE, so 0x100 entries, even
 * though only twelve patterns are ever produced. */
static unsigned char s_reverse[0x100];
static int           s_ready;

/*
 * The near-distance upgrade thresholds. The original compares the
 * depth's FLOAT BITS as an integer against 0x45400000 and 0x45c00000,
 * which is the usual trick for a positive-float compare; those are
 * 3072.0f and 6144.0f. Comparing as floats here is equivalent for
 * positive depths and says what it means.
 */
#define DETAIL_NEAR_LOW  3072.0f
#define DETAIL_NEAR_HIGH 6144.0f

void detail_init(void)
{
	int kind;

	if (s_ready)
		return;

	memset(s_reverse, 0, sizeof(s_reverse));

	/*
	 * The loop, exactly as Screen3D_Open runs it: the FIRST kind to
	 * claim a pattern wins, with ZERO as the not-yet-assigned
	 * sentinel.
	 */
	for (kind = 0; kind < DETAIL_KIND_COUNT; kind++) {
		unsigned int bits = kFeatureTable[kind];
		if (s_reverse[bits] == 0)
			s_reverse[bits] = (unsigned char)kind;
	}

	/*
	 * *** THE SENTINEL COLLISION, AND THE PATCH THAT FIXES IT ***
	 *
	 * Zero is the loop's "not yet assigned" marker, but kind 0 is a
	 * legitimate kind whose pattern is also zero. So when kind 0 is
	 * processed the guard writes 0 over 0, the slot still reads as
	 * unassigned, and kind 0x16 - which also computes pattern 0 -
	 * claims it. Without this store reverseIndex[0] would be 0x16.
	 *
	 * Six literal stores follow the loop in the original; three are
	 * no-ops restating what the loop already produced, and these two
	 * are the ones that bite. Transcribed rather than "cleaned up":
	 * a port that uses -1 as the sentinel and drops the patch changes
	 * which kind pattern 0 resolves to.
	 *
	 * (The third real store, featureTable[0] = 0x20, is already baked
	 * into the generated table - section 6 of the note prints the
	 * post-patch value.)
	 *
	 * detail-levels.txt section 1.1. Reproduced independently by
	 * tools/gen_detail_table.py, which replays the loop and prints
	 * 0x16 when the patch is withheld.
	 */
	s_reverse[0x00] = 0;
	s_reverse[0x20] = 0;

	s_ready = 1;
}

unsigned int detail_bits(unsigned int kind)
{
	if (kind >= DETAIL_KIND_COUNT)
		return 0;
	if (!s_ready)
		detail_init();
	return kFeatureTable[kind];
}

unsigned int detail_effective_kind(unsigned int kind, unsigned int flags,
                                   float depth)
{
	unsigned int bits;

	if (!s_ready)
		detail_init();
	if (kind >= DETAIL_KIND_COUNT)
		return kind;

	bits = kFeatureTable[kind];

	/*
	 * Three ways out with no substitution at all: the menu gate, the
	 * exempt bit, and DAT_004f1a18. A menu page therefore always
	 * draws at full detail whatever the setting says.
	 */
	if (g_detailBypass || (bits & DETAIL_EXEMPT))
		return kind;

	switch (g_cfgDetailLevel) {
	case 0:
	case 1:
		if ((bits & DETAIL_TIER1) && depth < DETAIL_NEAR_LOW)
			bits |= DETAIL_PERSPECTIVE;
		bits &= ~DETAIL_TIER_MASK;
		break;

	case 2:
		if ((bits & DETAIL_TIER1) && depth < DETAIL_NEAR_LOW)
			bits |= DETAIL_PERSPECTIVE;
		/* The one place MR_Primitive flags bit 3 is read. */
		if (!(flags & PRIM_FLAG_NO_REDUCE))
			bits &= ~DETAIL_TIER_MASK;
		break;

	case 3:
		if (depth < DETAIL_NEAR_LOW && (bits & DETAIL_TIER1))
			bits |= DETAIL_PERSPECTIVE;
		break;

	case 4:
	case 5:
		if (depth < DETAIL_NEAR_HIGH && (bits & DETAIL_TIER1))
			bits |= DETAIL_PERSPECTIVE;
		break;

	default:
		break;
	}

	/*
	 * THREE FAMILIES HAVE NO FLOOR, so stripping the tier yields a
	 * pattern no kind owns and reverseIndex returns 0. Two of the
	 * three are "Gouraud + do not filter"; the third, family 0x30, is
	 * flat translucent perspective - and reducing one of its members
	 * turns a TRANSLUCENT primitive OPAQUE, because kind 0 is opaque.
	 * That is the one case where reduction loses a feature rather
	 * than a quality tier, and it is the original's behaviour, not a
	 * defect in this transcription.
	 */
	return s_reverse[bits & 0xff];
}
