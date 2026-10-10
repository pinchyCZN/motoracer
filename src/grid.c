/*
 * grid.c - maybe_Game_InitRace's placement loop, transcribed.
 *
 * See grid.h for the formula and where each constant is read.
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "grid.h"
#include "log.h"

/*
 * Trk_WrapSegmentIndex (0x0040ec30), which is two while loops rather
 * than a modulo - kept that shape because it is one instruction's
 * worth of behaviour and a % on a negative operand is not the same
 * thing in C89 anyway.
 */
/* ORIGIN  Trk_WrapSegmentIndex @ 0x0040ec30 */
static int wrap_segment(int i, int count)
{
	if (count <= 0)
		return 0;
	while (i < 0)
		i += count;
	while (i >= count)
		i -= count;
	return i;
}

int grid_build(MR_GridSlot *out, int maxSlots, const MR_Trk *trk,
               int racerCount, int reverse, int selectedMode)
{
	int direction;
	int segment;
	int i;

	if (!out || !trk || !trk->segments || trk->segmentCount <= 0)
		return 0;
	if (racerCount < 2 || racerCount > maxSlots)
		return 0;

	memset(out, 0, (size_t)maxSlots * sizeof(*out));

	/*
	 * ONE FLAG GIVES BOTH. maybe_Game_LoadRace sets g_nRaceDirection
	 * to -1 and then to +1 when the reverse flag is clear, and
	 * g_nRaceReverse is that same flag - both come from
	 * DAT_00542878. So deriving one from the other here cannot get
	 * them out of step, which passing two arguments could.
	 */
	direction = reverse ? -1 : 1;

	segment = wrap_segment(GRID_SEGMENT_STEP * racerCount * direction,
	                       trk->segmentCount);

	/* The reverse-only extra step, at 0x0047c0f7. */
	if (reverse) {
		segment = wrap_segment(segment - GRID_SEGMENT_STEP * direction,
		                       trk->segmentCount);
	}

	/*
	 * BACKWARDS, FROM THE LAST RACER TO RACER 1. Index 0 is unused
	 * and the original's loop stops when its pointer reaches the
	 * base of the racer array.
	 */
	for (i = racerCount - 1; i >= 1; i--) {
		const MR_TrkSegment *s;
		MR_GridSlot         *g = &out[i];

		if (selectedMode != 0) {
			segment = wrap_segment(
			    segment - GRID_SEGMENT_STEP * direction,
			    trk->segmentCount);
		}
		s = &trk->segments[segment];

		/*
		 * (0, 0, 10) THROUGH THE SEGMENT'S MATRIX IS 10 UNITS ALONG
		 * ITS FORWARD AXIS. With the row-vector convention the rest
		 * of this engine uses, v * M for v = (0,0,10) is just
		 * 10 * row2, so the general transform the original calls
		 * reduces to this. Written out rather than routed through a
		 * matrix helper because the reduction is the interesting
		 * part.
		 *
		 * o THE MATRIX HAS NO TRANSLATION TO WORRY ABOUT. All 788
		 *   of ile01's segments carry an exactly zero translation
		 *   row and all three rotation rows are unit length
		 *   [verified, measured over the file]. So whether
		 *   Vec3_TransformByMatrix34 applies a translation is
		 *   unobservable here, and the seg+0x30 addend the original
		 *   adds below contributes nothing. It is transcribed
		 *   anyway - see the lateral step - so a hypothetical
		 *   non-zero segment would behave as the original does.
		 */
		g->pos[0] = s->pos[0] + GRID_FORWARD_NUDGE * s->matrix[6];
		g->pos[1] = s->pos[1] + GRID_FORWARD_NUDGE * s->matrix[7];
		g->pos[2] = s->pos[2] + GRID_FORWARD_NUDGE * s->matrix[8];

		if (selectedMode != 0) {
			/*
			 * THE SIDE ALTERNATES WITH THE RACER INDEX, and this
			 * is only a side rule - it does NOT pair racers up.
			 * Each already has its own segment from the step
			 * above.
			 */
			float lateral = ((i & 1) == 0) ? GRID_LATERAL
			                               : -GRID_LATERAL;

			g->lateral = (int)lateral;
			g->pos[0] += lateral * s->matrix[0] + s->matrix[9];
			g->pos[1] += lateral * s->matrix[1] + s->matrix[10];
			g->pos[2] += lateral * s->matrix[2] + s->matrix[11];
		}

		g->segment = segment;
		memcpy(g->matrix, s->matrix, sizeof(g->matrix));
	}

	return racerCount - 1;
}

void grid_log(const MR_GridSlot *slots, int racerCount, const char *what)
{
	int i;

	if (!slots || racerCount < 2)
		return;

	mrlog("grid: %s %d racers\n",
	      what ? what : "(grid)", racerCount - 1);
	for (i = 1; i < racerCount; i++) {
		/* The front, the back and the player are the interesting
		 * ones; the middle is arithmetic. */
		if (i > 3 && i < racerCount - 1)
			continue;
		mrlog("grid:  racer %2d seg %4d lat %+5d pos %10.1f %8.1f %10.1f\n",
		      i, slots[i].segment, slots[i].lateral,
		      slots[i].pos[0], slots[i].pos[1], slots[i].pos[2]);
	}
}
