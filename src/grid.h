/*
 * grid.h - the starting grid, from maybe_Game_InitRace (0x0047bf70).
 *
 * Places racers on track segments. This is the whole of the grid: the
 * original has no other notion of one, no pole position and no grid
 * table - just a segment index per racer and a side to sit on.
 *
 *
 * IT IS NOT Level.Spt's StartPosition. maybe_Game_CreateRacers drops
 * every racer on that single point and maybe_Game_InitRace then
 * overwrites all of it, so StartPosition is a spawn and nothing about
 * the layout can be read out of the manifest. See
 * docs/subsystems/game-object.txt 6.4.
 *
 *
 * THE FORMULA [verified]:
 *
 *     direction = reverse ? -1 : +1
 *     segment   = wrap(3 * racerCount * direction)
 *     if (reverse) segment = wrap(segment - 3 * direction)
 *
 *     for i = racerCount-1 down to 1:
 *         segment = wrap(segment - 3 * direction)
 *         pos = seg.position
 *             + 10 * seg.forward
 *             + lateral * seg.right
 *         lateral = ((i & 1) == 0) ? +128 : -128
 *
 * which collapses to RACER i ON SEGMENT 3*i*direction. The start value
 * exists only so the first decrement lands racer racerCount-1 on
 * 3*(racerCount-1).
 *
 * o ONE STEP AND ONE PLACEMENT PER RACER, so each of the 24 gets its
 *   OWN segment three apart and the lateral only alternates the side.
 *   A staggered single file, zig-zagging - NOT rows of two. On ile01
 *   three segments is about 1540 units, roughly nine bike lengths, so
 *   only a handful of the field is ever in view.
 *
 * o RACER 1 IS THE LOCAL PLAYER and index 0 is unused, which is why
 *   the loop stops at 1 and why grid_build indexes its output BY
 *   RACER INDEX rather than packing from zero. Keeping the indices
 *   aligned with the original's matters more than one wasted slot.
 *
 * o AND THE PLAYER STARTS LAST. Increasing segment index is forward -
 *   maybe_Race_UpdateProgress negates the segment delta only in
 *   reverse mode and treats a negative step as wrong-way. So with 24
 *   bikes the player is on segment 3 and the last opponent on 72.
 *
 * o MODE 0 GETS NEITHER THE STEP NOR THE LATERAL. It forces the
 *   opponent count to 1, so a two-bike race needs no stagger and the
 *   original gives it none - every racer lands on the start segment.
 */

#ifndef MRACER_GRID_H
#define MRACER_GRID_H

#include "trk.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 25 racer slots exist in the original; 24 are ever placed. */
#define GRID_MAX_SLOTS 32

/* The two constants, both literals in maybe_Game_InitRace. */
#define GRID_SEGMENT_STEP  3
#define GRID_FORWARD_NUDGE 10.0f    /* 0x41200000 at 0x0047c16b */
#define GRID_LATERAL       128.0f   /* the +-0x80 idiom          */

typedef struct {
	int   segment;      /* the wrapped segment index it sits on   */
	int   lateral;      /* +128, -128, or 0 in mode 0             */
	float pos[3];       /* world position                         */
	float matrix[12];   /* the segment's orientation, copied      */
} MR_GridSlot;

/*
 * grid_build - fill out[1 .. racerCount-1] with grid slots.
 *
 * out is indexed BY RACER INDEX: out[0] is zeroed and unused, out[1]
 * is the local player. maxSlots must be at least racerCount.
 *
 * reverse is the race-direction flag - 0x004dc144 in the original,
 * which is also what g_nRaceDirection is derived from, so one input
 * gives both. selectedMode is g_nSelectedMode.
 *
 * Returns the number of slots filled, or 0 on bad arguments.
 */
int  grid_build(MR_GridSlot *out, int maxSlots, const MR_Trk *trk,
                int racerCount, int reverse, int selectedMode);

/* Log the first few slots and the last, which is enough to check the
 * formula by hand against a track whose opening segments are known. */
void grid_log(const MR_GridSlot *slots, int racerCount, const char *what);

#ifdef __cplusplus
}
#endif

#endif /* MRACER_GRID_H */
