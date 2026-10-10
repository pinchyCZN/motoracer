/*
 * racer.c - ONE COMPETITOR. Everything that operates on a single
 * MR_Racer, which is rule 13's test for what belongs here.
 *
 * It started as the tail of maybe_Game_InitRace (0x0047bf70) - the
 * grid placement is still the first thing in the file - and grew to
 * hold the whole per-racer frame:
 *
 *     placement       racer_place_from_grid, racer_place_wheels,
 *                     racer_set_track_pos
 *     the tick order  racer_tick_order, and the guards around it
 *     orientation     the chassis attitude, the suspension, the
 *                     lean, the two orient_* frames
 *     the wheels      contacts, ground, spin, skid width
 *     the crash       begin, update, reset, the rider root
 *     the AI          the tick and its helpers
 *     the dash        speed, tacho, the needle
 *     effects         the wheel FX gates and particle counts
 *     audio           the per-racer voices
 *
 * WHAT IS NOT HERE, and where it went: anything concerning MORE THAN
 * ONE racer is race.c, the simulation is physics.c, the carac table
 * is bikes.c, grid geometry is grid.c and the draw is racerdraw.c.
 * CLAUDE.md 13 has the table and the reasoning.
 *
 * MR_Racer IS THE ORIGINAL'S RECORD BYTE FOR BYTE - see racer.h,
 * which is also where every field's offset and the assertions live.
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "options.h"
#include "menu.h"
#include <stdlib.h>
#include <float.h>
#include <math.h>

#include "racerdraw.h"
#include "trig.h"
#include "fx.h"
#include "bikes.h"
#include "racer.h"
#include "rand.h"
#include "timing.h"
#include "skl.h"
#include "model.h"
#include "log.h"

/*
 * ORIGIN  Racer_Reset @ 0x0045bb30
 *
 * THE FOUR STORES A memset DOES NOT REPRODUCE. Racer_Reset zeroes
 * about thirty fields and sets these:
 *
 *     0x0045bb92  placeWheels2 = position
 *     0x0045bc2a  onGroundPrev = 1
 *     0x0045bc30  onGround     = 1
 *     0x0045bc48  unk_0020     = 1.0f
 *     0x0045bc4f  poseStaticPied = pilot rot 4, staticPied
 *
 * poseStaticPied IS NOT SAFE AT ZERO. It is the base pose the
 * low-speed stance blends FROM, and the original stores staticPied
 * there before the racer is ever ticked; left zero it reads as
 * index 0, static.rot, and the rider stands wrong at every speed
 * below 20 km/h - which is every standing start.
 *
 * onGround = 1 IS THE OTHER ONE THAT MATTERS. A racer that resets
 * to 0 spends its first tick on the airborne path, which gives
 * Racer_UpdateSuspension a different chassis attitude to chase.
 * THAT ENDS IN THE SAME PLACE AS THE ZEROED MATRIX BELOW - see the
 * note at the matBikeRoot store for the mechanism and why one frame
 * of it is permanent.
 *
 * [open] THE GATED TAIL IS NOT TRANSCRIBED - Racer_Reset's own
 * trackTotalProgress seed from 0x0052c10c. Note that
 * racer_place_from_grid seeds the same field from the grid slot,
 * which is maybe_Game_InitRace's seeding and a different thing;
 * this is the only statement of the gap.
 */
static void racer_reset_nonzero(MR_Racer *r)
{
	int i;

	if (!r)
		return;

	for (i = 0; i < 3; i++)
		r->placeWheels2[i] = r->position[i];

	r->onGround       = 1;
	r->onGroundPrev   = 1;
	r->poseStaticPied = PILOT_ROT_STATIC_PIED;

	/* ONE, not zero - maybe_Race_StartPending @ 0x004112b2. */
	r->checkpointIndex = 1;

	/*
	 * AN ALL-ZERO 3x4 IS NOT A STATE THE ORIGINAL EVER HAS, and our
	 * memset creates one. racer_chassis_attitude then takes its
	 * degenerate branch at 0x0045ca5c - scale is 0 - and returns
	 * -pi/2, which racer_update_suspension feeds straight into
	 * headingSmoothed as -pi/2 over RetardCadre 16 = -0.0981748.
	 *
	 * ONE FRAME OF THAT IS PERMANENT. headingSmoothed wraps into
	 * [0, 2pi], so the negative becomes 2pi - 0.098 and the return
	 * step is delta/16; by the time it has crept back to within a few
	 * microradians of the turn the step is below one float ULP at
	 * 6.28 and it stops. It parks there for the whole race, three
	 * cosine-table indices away from the zero MOTO holds.
	 *
	 * Racer_ComposeOrientation 0x0045cb00 is what fills this properly;
	 * identity is the pre-race value it overwrites.
	 */
	memset(r->matBikeRoot, 0, sizeof r->matBikeRoot);
	r->matBikeRoot[0] = 1.0f;
	r->matBikeRoot[4] = 1.0f;
	r->matBikeRoot[8] = 1.0f;

	/* racer+0x20 = 1.0f; the field is still named padding. */
	{
		float one = 1.0f;
		memcpy(r->unk_0020, &one, sizeof one);
	}
}

/* ORIGIN  maybe_Game_InitRace, its placement tail */
int racer_place_from_grid(MR_Racer *racers, int count,
                          const MR_GridSlot *slots, int segmentCount,
                          int reverse, int localIndex)
{
	int i;
	int placed = 0;

	if (!racers || !slots || count < 2 || count > RACER_MAX ||
	    segmentCount <= 0)
	    return 0;

	memset(racers, 0, (size_t)count * sizeof(*racers));

	/*
	 * THE STORES A memset DOES NOT REPRODUCE, and both
	 * constructors make them - Racer_Reset @ 0x0045bb30 and
	 * Racer_Create. See racer_reset_nonzero.
	 */
	for (i = 0; i < count; i++)
		racer_reset_nonzero(&racers[i]);

	for (i = 1; i < count; i++) {
		MR_Racer *r = &racers[i];
		int       seg = slots[i].segment;
		int       signedSeg;

		r->active  = 1;
		r->isLocal = (i == 1);      /* index 1 is the local player */

		/*
		 * THE SHADOW PRIMITIVES ARE BUILT ONCE, HERE, because this
		 * is where the memset above wipes them. Racer_Create does
		 * the same at 0x0045c010 - the two quads never change, only
		 * the six vertices under them, which the tick refills.
		 */
		racerdraw_init_shadow(r);

		/*
		 * WHAT FOLLOWS IS FOUR FIELDS THE memset ZEROES AND
		 * NOTHING ELSE WRITES, AND ZERO IS A PLAUSIBLE VALUE FOR
		 * EVERY ONE OF THEM. That is the whole hazard: none of
		 * them reads as an error, each one reads as a bike that
		 * behaves slightly wrong, and the measured cost is on
		 * each below. Add a field to MR_Racer and ask what its
		 * zero means before trusting the memset.
		 */

		/*
		 * A STRAIGHT LINE IS AN INFINITE RADIUS, and the memset
		 * above leaves it zero - which the damping reads as the
		 * tightest possible corner and halves every step.
		 *
		 * MOTO.EXE HOLDS FLT_MAX HERE and a watchpoint on it saw
		 * ZERO writes across sixteen frames of hard cornering, so
		 * whatever maintains it is not on the player's path. See
		 * racer_integrate for what reads it.
		 */
		r->turnRadius[0] = FLT_MAX;
		r->turnRadius[1] = FLT_MAX;

		/*
		 * humanPlayer AND soloMode BOTH GATE REAL BEHAVIOUR, and at
		 * zero the local player takes every AI branch.
		 *
		 * IT IS NOT COSMETIC: racer_steer_torque then falls back to
		 * the AI constant 0.004 instead of RotationTransfert with
		 * its speed fade, and the bike turns 1.62x too fast -
		 * 0.004 * 3.3335 * 0.01 * 600 = 0.08001 rad a frame
		 * measured against MOTO.EXE's 0.04928. racer_lean_response
		 * and the brake gate read them too.
		 *
		 * HARDCODING humanPlayer HANDS A GUEST'S OWN BIKE TO THE AI,
		 * so it comes in as a parameter. soloMode IS 1 IN A NET RACE
		 * TOO - 0x00410f65 writes it and nothing writes 0; a 0 costs
		 * the wheelie and the lap close-out. netplay-relay.txt 3.6.
		 */
		r->humanPlayer = (i == localIndex);
		r->soloMode    = 1;

		/*
		 * THE NAME, racer +0x2dd4. These are values READ LIVE OUT
		 * OF A RUNNING MOTO rather than transcribed - "COMPUTER"
		 * on every AI, the player profile name on racer 1 -
		 * because Racer_Ctor only zeroes the first byte and
		 * [open] nothing has been found that writes the real
		 * one.
		 *
		 * o UPPERCASE, AND THAT IS NOT A STYLE CHOICE. NAME.FNT
		 *   carries A-Z from 0x41 and nothing below it, so a
		 *   lower-case letter draws as a different bank or as
		 *   nothing. The string at 0x004de394 is "Computer" in
		 *   mixed case and the racer holds "COMPUTER", so
		 *   something upper-cases it on the way in.
		 *
		 * o AN EMPTY NAME IS NOT HARMLESS. The BEST LAP block
		 *   pads to eight cells with '@', which is a full-width
		 *   BLANK glyph in that face - so a zeroed name draws as
		 *   a bare coloured bar with no text.
		 *
		 * THE LOCAL NAME IS g_playerName, which page 15 - the
		 * name-entry screen - writes on the way out and reads
		 * back from [PLAYER] name. It is already folded to
		 * upper case and clamped to eight characters there, so
		 * this only has to copy it.
		 *
		 * ON OUR SLOT for the same reason; race.c overwrites all of
		 * them from the lobby in a net game.
		 */
		if (i == localIndex) {
			strncpy(r->playerName, g_playerName,
			        sizeof(r->playerName) - 1);
			r->playerName[sizeof(r->playerName) - 1] = 0;
		} else {
			strcpy(r->playerName, "COMPUTER");
		}

		/*
		 * AND slotIndex, WHICH IS THE RACER'S OWN INDEX. The
		 * original tests this->slotIndex == g_viewedRacerIndex -
		 * racers.txt 9.12 - and race_reset_crashes is passed it as
		 * the racer to bring back up.
		 *
		 * AT ZERO THE CRASH NEVER ENDS. With slotIndex 0 on racer
		 * 1, the reset asks to raise racer 0 while racer 1 is still
		 * down - exactly the case the gate refuses - so it returns
		 * 0 every frame, the latch stays set and the drivetrain
		 * stays gated off. Measured: crashTimerMs ran to -161 over
		 * 160 frames with crashLatch still 1 and speed at zero.
		 *
		 * MOTO.EXE READS 1 HERE for racer 1, at racer+0x2dd0.
		 */
		r->slotIndex = i;

		r->position[0] = slots[i].pos[0];
		r->position[1] = slots[i].pos[1];
		r->position[2] = slots[i].pos[2];
		/*
		 * THE ORIENTATION HAS TWO ARMS. maybe_Game_InitRace
		 * branches on g_nRaceReverse:
		 *
		 *   forward   matOrientation = the segment matrix,
		 *             all twelve floats, straight copy
		 *   reverse   forward = normalise(row3 - row2), then
		 *             row0 = up x forward, row1 = forward x row0,
		 *             and ROW 3 IS ZEROED
		 *
		 * THE PAIR IS SYMMETRIC AND THAT IS THE TELL. A racer's
		 * own heading is matOrientation row3 + row2
		 * (racer_apply_drive), so the forward arm heads along
		 * segRow3 + segRow2 and the reverse arm zeroes row3 so
		 * its heading is exactly the segRow3 - segRow2 it was
		 * built from. Reversing flips row2's contribution and
		 * nothing else.
		 *
		 * A PLACEMENT BUG HERE IS INVISIBLE ON THE AI, and that
		 * is worth knowing before trusting a capture.
		 * racer_tick_ai PLACES an opponent every frame and
		 * rebuilds its orientation from the line follower, so an
		 * opponent corrects itself on frame one whatever this
		 * loop wrote. ONLY THE PLAYER KEEPS IT. Check racer 1.
		 */
		if (!reverse) {
			memcpy(r->matOrientation, slots[i].matrix,
			       sizeof(r->matOrientation));
		} else {
			float fwd[3];

			fwd[0] = slots[i].matrix[9]  - slots[i].matrix[6];
			fwd[1] = slots[i].matrix[10] - slots[i].matrix[7];
			fwd[2] = slots[i].matrix[11] - slots[i].matrix[8];

			/* The memset zeroes row 3, as the original does. */
			memset(r->matOrientation, 0, sizeof(r->matOrientation));
			if (!racer_frame_from_forward(r->matOrientation, fwd)) {
				/*
				 * A forward parallel to world up has no basis.
				 * The original does not guard it; a zero matrix
				 * would be worse than the forward orientation,
				 * so fall back rather than leave it. [ours]
				 */
				memcpy(r->matOrientation, slots[i].matrix,
				       sizeof(r->matOrientation));
			}
		}

		r->segmentStart = seg;
		r->segmentPrev   = seg;      /* +0x31bd = +0x31c1 */

		/*
		 * THE PROGRESS PAIR IS SEEDED WITH A SIGNED SEGMENT. A
		 * segment at or past half the track length has the length
		 * subtracted, which makes progress lap-relative from the
		 * first frame instead of jumping by a whole lap the moment
		 * a racer wraps past zero.
		 *
		 * o REVERSE TAKES THE OTHER ARM. Below half the track the
		 *   value is negated; at or above it, it becomes
		 *   segmentCount - seg. Both arms are the original's and
		 *   neither is exercised by a normal race, so they are
		 *   transcribed rather than tested.
		 */
		if (!reverse) {
			signedSeg = seg;
			if (segmentCount / 2 <= signedSeg)
				signedSeg -= segmentCount;
		} else {
			if (seg < segmentCount / 2)
				signedSeg = -seg;
			else
				signedSeg = segmentCount - seg;
		}
		r->trackTotalProgress = signedSeg;     /* +0x31b5 */
		r->progressHigh = signedSeg;     /* +0x31b9 */
		r->subSegment    = 0;             /* +0x31b1 */
		placed++;
	}
	return placed;
}

/* ================================================================== */
/* THE POST-RACE AI TAKEOVER.                                         */
/*                                                                    */
/* Win a single race and the game keeps riding your bike for you       */
/* while the credits run. It is not a separate AI: the tick dispatch   */
/* already forks on humanPlayer, so clearing that ONE FIELD is the     */
/* whole handover. Everything else here is making the handover not     */
/* look like a glitch - seeding the AI's speeds from the ones the      */
/* player arrived with, and dropping whatever was being held down.     */
/*                                                                    */
/* race.c owns when this happens; see race_auto_ride_gate.             */
/* ================================================================== */

/*
 * The AI rides at 80 per cent of the speed the player's bike is
 * capable of - a winner cruising, not racing.
 *
 * 0x004d03a8 IS A DOUBLE holding exactly 0.8, so this literal has
 * no f - as a float it was 0.800000011920929.
 */
#define RACER_AI_SPEED_SCALE  0.8

/*
 * ORIGIN  FUN_00412960 @ 0x00412960, its field writes
 *
 * THE ORDER IS THE ORIGINAL'S and two lines of it matter:
 * engineSpeed and aiRoadSpeed are seeded BEFORE realSpeedMax is
 * scaled down, so the AI inherits the speed actually being carried
 * rather than the reduced ceiling. Scaling first would brake the
 * bike on the frame of the handover.
 *
 * savedRealSpeedMax comes back out so the restore can put it back;
 * the caller keeps it because the sequence, not the racer, is what
 * remembers across the pause.
 */
void racer_ai_takeover(MR_Racer *r, float *savedRealSpeedMax)
{
	if (!r)
		return;
	if (savedRealSpeedMax)
		*savedRealSpeedMax = r->realSpeedMax;

	r->humanPlayer = 0;          /* +0x08 - the handover itself */

	r->engineSpeed = r->speed;   /* +0xec  <- +0x124 */
	r->aiRoadSpeed = r->speed;   /* +0x1a4 <- +0x124 */

	/* FLD float / FMUL double / FSTP float, 0x004129d4. */
	r->realSpeedMax = (float)((double)r->realSpeedMax
	                          * RACER_AI_SPEED_SCALE);

	/*
	 * AND DROP THE PLAYER'S INPUT, or a key still held at the line
	 * fights the AI for as long as it is down. The original clears
	 * the whole input record - both axes to centre, every button
	 * off - in Input_Clear (0x00470fc0); ours only has to forget
	 * the word the tick reads.
	 */
	r->tickInputWord = 0;        /* +0x2dc8 */
}

/*
 * ORIGIN  FUN_00412a50 @ 0x00412a50, the restore in state 1
 *
 * The mirror, run when the credits finish. speed comes back FROM
 * engineSpeed, which is the reverse of the seeding above.
 *
 * NOT TRANSCRIBED: the gearbox re-seat the original runs last,
 * FUN_0048df90 @ 0x0048df90 - it walks the gear up or down until the
 * ratio suits the road speed and returns the engine speed to match.
 * It lands one frame before the game leaves for the menus, so what
 * gear the player nominally resumes in is not observable. Left out
 * rather than guessed: its ratio table is reached by a NEGATIVE
 * index off MinAdherenceDerap and getting that wrong silently
 * corrupts a bike. [open]
 */
void racer_restore_human(MR_Racer *r, float savedRealSpeedMax)
{
	if (!r)
		return;
	r->humanPlayer      = 1;
	r->realSpeedMax     = savedRealSpeedMax;
	r->shiftState       = 0;     /* +0x84 */
	r->pendingDownshift = 0;     /* +0x8c */
	r->speed            = r->engineSpeed;
}

/* ORIGIN  NONE - ours */
void racer_log(const MR_Racer *racers, int count, const char *what)
{
	int i;

	if (!racers || count < 2)
		return;
	mrlog("racer: %s %d placed, index 1 is the local player\n",
	      what ? what : "(race)", count - 1);
	for (i = 1; i < count; i++) {
		if (i > 2 && i < count - 1)
			continue;
		mrlog("racer: %2d%s seg %4d prog %5d pos %10.1f %8.1f %10.1f\n",
		      i, racers[i].isLocal ? "*" : " ",
		      racers[i].segmentStart, racers[i].trackTotalProgress,
		      racers[i].position[0], racers[i].position[1],
		      racers[i].position[2]);
	}
}

/* ------------------------------------------------------------------ */

/* ORIGIN  NONE - ours */
float racer_segment_length(const MR_Trk *t, int seg)
{
	const MR_TrkSegment *a;
	const MR_TrkSegment *b;
	float dx, dy, dz;

	if (!t || !t->segments || t->segmentCount <= 0)
		return 0.0f;
	if (seg < 0 || seg >= t->segmentCount)
		return 0.0f;

	a = &t->segments[seg];
	b = &t->segments[(seg + 1) % t->segmentCount];
	dx = b->pos[0] - a->pos[0];
	dy = b->pos[1] - a->pos[1];
	dz = b->pos[2] - a->pos[2];
	return (float)sqrt(dx * dx + dy * dy + dz * dz);
}

/* ORIGIN  NONE - ours. Not physics; see the header */
void racer_set_track_pos(MR_Racer *r, const MR_Trk *t,
                         float segPos, float lateral)
{
	const MR_TrkSegment *a;
	const MR_TrkSegment *b;
	int   i, n;
	float f;

	if (!r || !t || !t->segments || t->segmentCount <= 0)
		return;

	n = t->segmentCount;

	/*
	 * fmod then floor, so a negative segPos wraps the same way a
	 * positive one does rather than reflecting about zero.
	 */
	segPos = (float)fmod(segPos, (double)n);
	if (segPos < 0.0f)
		segPos += (float)n;

	i = (int)segPos;
	if (i < 0)  i = 0;
	if (i >= n) i = n - 1;
	f = segPos - (float)i;

	a = &t->segments[i];
	b = &t->segments[(i + 1) % n];

	/*
	 * ORIENTATION IS THE SEGMENT'S, NOT INTERPOLATED. Blending two
	 * rotation matrices component-wise does not produce a rotation,
	 * and the segments are close enough together that the step is
	 * not visible. THIS IS THE RAIL, NOT THE SIMULATION - a racer
	 * ridden by the physics gets its attitude from
	 * racer_orient_grounded; see racer.h.
	 */
	memcpy(r->matOrientation, a->matrix, sizeof(r->matOrientation));

	/* Row 0 is the lateral axis - game-object.txt 6.4. */
	r->position[0] = a->pos[0] + (b->pos[0] - a->pos[0]) * f
	            + a->matrix[0] * lateral;
	r->position[1] = a->pos[1] + (b->pos[1] - a->pos[1]) * f
	            + a->matrix[1] * lateral;
	r->position[2] = a->pos[2] + (b->pos[2] - a->pos[2]) * f
	            + a->matrix[2] * lateral;

	/*
	 * THE PREVIOUS SEGMENT, AND NOTHING ELSE. The progress pair and
	 * the sub-segment belong to race_update_progress; writing them
	 * from here puts two owners on one field.
	 */
	r->segmentPrev = i;
}


/* ================================================================== */
/* THE PER-RACER TICK.                                                */
/* Sources: maybe_Racer_Tick 0x0045d580, maybe_Racer_TickAI            */
/* 0x0046bf80. See racer.h and docs/subsystems/racers.txt 9.26.        */
/* ================================================================== */

#define K_DT_GUARD_VALUE   1.0f    /* both guards substitute this   */
#define K_HEADING_RATE     0.0025  /* 0x004d0f38, double            */
#define K_FORK_DECAY       0.01    /* 0x004d0ed8, double, PER FRAME */
#define K_FX_SLIP_MIN      0.04    /* 0x004d0f40, double            */

#define INPUT_BRAKE        2u      /* MASK 2, i.e. bit 1            */

/* sin(wheelieAngle) * this is the bike pitch. 0x004d1218, a
 * DOUBLE holding -pi/4 - read as a float it is 3.37e12.
 *
 * SPELLED AS THE EXPRESSION, not as digits: a typed-out -0.785398
 * is short by 1.6e-7.                                         */
#define K_WHEELIE_PITCH  (-MR_PI / 4.0)

/* The suspension, Racer_UpdateSuspension 0x00466a80.            */
#define K_SUSP_DT_MIN        3.0f      /* 0x40400000            */
#define K_SUSP_DT_MAX        5.0f      /* 0x40a00000            */
#define K_SUSP_RELAX         5.184     /* 0x004d1128, a DOUBLE  */
#define K_SUSP_REARM         0.5f      /* 0x004d1018, a float   */
#define K_ATTITUDE_EPS       0.0001    /* 0x004d0eb8, a DOUBLE  */
#define RACER_ORIENT_EPS  1.0e-5f      /* 0x004d0e50, a FLOAT   */

/* maybe_Racer_ApplyCornering's two, read out of .rdata. */
#define CORNER_GRIP           1.0f        /* FUN_00459f80 -> 0x004d0e48 */
#define CORNER_RADIUS_SCALE   0.0078125f  /* 0x004d0ee0, = 1/128        */

/*
 * maybe_Racer_UpdateAirborne's constants, read out of the binary.
 *
 *   AIR_TIME_STEP        0x004d0f20  double 2.592
 *   AIR_LONGSPEED_MIN    0x004d0ea8  double 0.1
 *   AIR_DIVE_ANGLE       0x004666cf  float  0x3cc90fdb = pi/128
 *   AIR_ATT_LIMIT        0x004d0f10  double +1.4726215564 = pi/2 - pi/32
 *                        0x004d0f18  the same value negated
 *   AIR_HEADING_RATE     0x004d10f0  double -pi/1600
 *   AIR_HEADING_FLOOR    0x004d10f8  double -pi/16
 *   AIR_HEADING_FLOOR_SET 0x00466724 float 0xbe490fdb
 *
 * THE FLOOR IS COMPARED AS A DOUBLE AND STORED AS A FLOAT, and they
 * are not the same number - -0.196349540849 against -0.1963495463.
 * The original really does use both, so both are kept.
 *
 * AIR_ATT_LIMIT IS 84.375 DEGREES, which is 15/64 of a turn, so it is
 * exactly 3840 of the 16384 table indices - a round number in the
 * engine's own angle units rather than in degrees.
 *
 * THE THREE THAT ARE EXACT EXPRESSIONS ARE WRITTEN AS SUCH, and
 * the widths below match the table above. Typed out as digits they
 * were short by 3.7e-9 to 7.6e-9; as expressions they cannot
 * drift.
 */
#define AIR_TIME_STEP           2.592
#define AIR_LONGSPEED_MIN       0.1f
#define AIR_DIVE_ANGLE          0.024543693f
#define AIR_ATT_LIMIT           (MR_HALF_PI - MR_PI / 32.0)
#define AIR_HEADING_RATE       (-MR_PI / 1600.0)
#define AIR_HEADING_FLOOR      -0.196349540849
#define AIR_HEADING_FLOOR_SET  -0.1963495463f

/*
 * The manual-shift edge latch, DAT_004ea4c8.
 *
 * IT IS ONE GLOBAL FOR THE WHOLE GAME, not a per-racer field - the
 * original reads and writes the same dword whichever racer is
 * ticking. Only a human player reaches it, and there is one of those,
 * so the sharing never shows. Kept as a global to match.
 */
static int g_racerShiftLatch;   /* 0x004ea4c8 */

/*
 * ORIGIN  NONE - ours
 *
 * THE ONLY STATE OUTSIDE MR_Racer THAT A PHYSICS TICK CARRIES ACROSS
 * FRAMES, which is what makes race_autopilot_probe possible at all:
 * the rollout saves this, simulates, and puts it back, so a probe
 * cannot leave a spurious gear change behind.
 *
 * It stays static. Two accessors are a smaller hole in this file
 * than a header declaration, and they are the whole interface.
 */
int racer_shift_latch_get(void)
{
	return g_racerShiftLatch;
}

void racer_shift_latch_set(int v)
{
	g_racerShiftLatch = v;
}


/* ORIGIN  maybe_Racer_Tick @ 0x0045d580, its dt guard */
float racer_guard_dt(float dt)
{
	/*
	 * The original is `if ((int)dt < 1) dt = 1.0f` on the FLOAT'S
	 * BITS. Bits < 1 covers +0.0f and every negative, so the
	 * condition is "not strictly positive" - which is what this
	 * says without the pun.
	 */
	if (!(dt > 0.0f))
		dt = K_DT_GUARD_VALUE;

	return dt;
}


/* ORIGIN  maybe_Racer_Tick @ 0x0045d580, the racer+0x2ea5 block */
void racer_update_heading_ref(MR_Racer *r, float frameDelta)
{
	/*
	 * Recover only when NOT braking, or when essentially stopped.
	 * The original's speed test is also a float-bits compare and
	 * means "speed <= +0.0".
	 */
	if ((r->inputWord & INPUT_BRAKE) != 0 && r->speed > 0.0f)
		return;

	if (r->headingRef >= 0.0f) {
		r->headingRef = 0.0f;
	} else {
		r->headingRef = (float)((double)r->headingRef
		                        + (double)frameDelta * K_HEADING_RATE);
	}
}


/* ORIGIN  maybe_Racer_Tick @ 0x0045d580, at 0x0045d63e */
void racer_decay_fork_dive(MR_Racer *r)
{
	/*
	 * Toward zero from both sides by a fixed step, snapping to
	 * exactly zero on overshoot. The original writes the two arms
	 * as separate ifs with a goto between them, and tests the sign
	 * on the float bits both times.
	 */
	if (r->forkDive > 0.0f) {
		r->forkDive = (float)((double)r->forkDive - K_FORK_DECAY);
		if (r->forkDive < 0.0f)
			r->forkDive = 0.0f;
	} else if (r->forkDive < 0.0f) {
		r->forkDive = (float)((double)r->forkDive + K_FORK_DECAY);
		if (r->forkDive > 0.0f)
			r->forkDive = 0.0f;
	}
}


/* ORIGIN  maybe_Racer_Tick @ 0x0045d580, at 0x0045d6db */
void racer_latch_stunt_request(MR_Racer *r, int countdown)
{
	/*
	 * THE BUTTON IS NOT THE REQUEST. racer_update_wheelie ramps on
	 * stuntOrOverspeed (+0x2e85); the key handler sets stuntHeld
	 * (+0x31f9). This two-instruction copy is the only bridge, and
	 * it is gated three ways.
	 *
	 * 0045d6c4  the countdown - no wheelie off the line.
	 * 0045d6cd  humanPlayer, then 0045d6d4 soloMode. A remote or an
	 * AI racer has its request written by maybe_Racer_TickRemote
	 * (0x0045eae0) or maybe_Racer_TickAI (0x0046c0e7) instead, so
	 * copying a button they do not have would overwrite it.
	 */
	if (countdown)
		return;
	if (r->humanPlayer == 0 || r->soloMode == 0)
		return;

	r->stuntOrOverspeed = r->stuntHeld;
}


/* ORIGIN  maybe_Racer_Tick @ 0x0045d580, at 0x0045d676 */
int racer_fx_gate(int onGround, float slipAbs)
{
	if (!onGround)
		return 0;
	return (fabs((double)slipAbs) >= K_FX_SLIP_MIN);
}


/*
 * THE INLINE WORK COMES FIRST and is not in this table - the four
 * functions above, plus zeroing handSkeleton, riderStateBits and
 * springCompression, plus Racer_SavePrevFrame. The table starts where
 * the call list does.
 */
static const MR_RacerTickStep s_tickOrder[] = {
	{ "maybe_Racer_UpdateWheelie",     0x004660f0, "!preRace" },
	{ "maybe_Racer_ApplyDrive",        0x00465da0, "!crashLatch" },
	{ "maybe_Racer_ApplySteering",     0x00466240, "!preRace && !crashLatch" },
	{ "ModelInstance_Integrate",       0x0048d620, NULL },
	{ "maybe_Racer_ApplyCornering",    0x0045d110, NULL },
	{ "maybe_Racer_UpdateGroundContact", 0x00468ca0, "returns groundFlag" },
	{ "maybe_Racer_UpdateAirborne",    0x00466660, NULL },
	{ "maybe_Racer_UpdateForkDive",    0x004667e0, "writes forkDive" },
	{ "Racer_SpinWheels",              0x00466940, NULL },
	{ "Racer_UpdateSuspension",        0x00466a80, NULL },
	{ "maybe_Racer_BuildShadow",       0x00461aa0, NULL },
	{ "maybe_Racer_UpdateLean",        0x00469c60, NULL },
	{ "maybe_Racer_UpdateCrash",       0x004683e0, NULL },
	{ "maybe_Racer_UpdateSkeleton",    0x00467b30, "given groundFlag" },
	{ "maybe_Racer_EmitWheelFx",       0x00466d70, "groundFlag" },
	{ "Racer_UpdateDash",              0x00467930, NULL },
	{ "maybe_Trk_LaneOfRacer",         0x0046d340, "result -> lane" }
};


/* ORIGIN  maybe_Racer_Tick @ 0x0045d580, its call list - DATA, not
 *         code */
const MR_RacerTickStep *racer_tick_order(int *count)
{
	if (count)
		*count = (int)(sizeof(s_tickOrder) / sizeof(s_tickOrder[0]));

	return s_tickOrder;
}


#define INPUT_ACCEL        1u
#define ANALOG_CENTRE      0x8000
#define ANALOG_NEG_DIV     2000    /* the IDIV at 0x00465ef9 */

/* ORIGIN  maybe_Racer_ApplyDrive @ 0x00465da0 */
void racer_apply_drive(MR_Racer *r, const MR_BikeCarac *c,
                       float wheelieAngle,
                       unsigned int buttons, int axis0, int preRace,
                       MR_DriveDemand *out,
                       MR_Drivetrain *d, const MR_Gearbox *gb,
                       float dtScaled)
{
	int   racing;
	float len;
	int   i;

	if (!r || !out)
		return;

	/* ---- the input overrides, in the original's order --------- */
	racing = (r->raceState == 0 || r->raceState == 1);

	if (!racing && r->humanPlayer != 0)
		buttons = INPUT_BRAKE;          /* finished -> brake ONLY */

	if (preRace) {
		buttons |= INPUT_BRAKE;         /* the grid */
	} else if (wheelieAngle != 0.0f) {
		buttons |= INPUT_ACCEL;         /* a wheelie implies throttle */
	}

	if ((buttons & INPUT_BRAKE) != 0) {
		r->stuntOrOverspeed = 0;
		if (r->humanPlayer != 0 && r->soloMode != 0)
			r->stuntHeld = 0;           /* braking cancels it */
	}

	out->buttons = buttons;

	/* ---- the direction --------------------------------------- */
	for (i = 0; i < 3; i++)
		out->dir[i] = r->matOrientation[9 + i] + r->matOrientation[6 + i];

	len = (float)sqrt((double)out->dir[0] * out->dir[0]
	                  + (double)out->dir[1] * out->dir[1]
	                  + (double)out->dir[2] * out->dir[2]);
	/*
	 * An EXACT zero test against _DAT_004d0e54, not an epsilon -
	 * the same idiom as physics_slip_speed's guard.
	 */
	if (len != 0.0f) {
		for (i = 0; i < 3; i++)
			out->dir[i] /= len;
	}

	/*
	 * ---- the demand, AND NOT ONE OF THESE ARMS MAY RETURN ----
	 *
	 * This function has a tail past the demand. Writing the three
	 * arms as early returns reads naturally and skips it.
	 */
	if (axis0 != ANALOG_CENTRE && racing) {
		int n = ANALOG_CENTRE - axis0;

		/* The asymmetry is the original's. See racer.h. */
		out->num = (n < 0) ? (n / ANALOG_NEG_DIV) : n;
		out->den = ANALOG_CENTRE;
	} else if ((buttons & (INPUT_ACCEL | INPUT_BRAKE)) == 0) {
		out->num = 0;                   /* coast */
		out->den = 1;
	} else {
		out->num = 0;
		if ((buttons & INPUT_BRAKE) == 0 || preRace) {
			if ((buttons & INPUT_ACCEL) != 0)
				out->num = 1;
		} else {
			out->num = -1;
		}
		out->den = 1;
	}

	/*
	 * ---- AND THE DRIVETRAIN, FROM HERE ----
	 *
	 * maybe_Racer_ApplyDrive CALLS maybe_Physics_TickDrivetrain;
	 * it does not sit beside it, and the nesting is the point.
	 *
	 * ONE GATE COVERS BOTH. maybe_Racer_Tick branches around the
	 * whole of ApplyDrive at 0x0045d6fb, so skipping this function
	 * skips the drivetrain with it - which is what a crashed bike
	 * needs. Calling the two side by side instead forces the crash
	 * gate to be written twice, and the second copy is the one that
	 * gets forgotten.
	 *
	 * d MAY BE NULL for a caller that wants the demand alone.
	 */
	if (d && c) {
		/*
		 * THE 1.1x TORQUE GATE IS THE GEARBOX MODE, racer+0x88,
		 * and it MUST come from the gearbox. Every live sample
		 * reads 1 - automatic - so a hardcoded 1 looks correct
		 * and silently gives manual the automatic engine, which
		 * is the mode that does NOT get the multiplier. See
		 * engine_torque_at.
		 */
		physics_drivetrain_frame(d, c, r->realSpeedMax,
		                         gb ? gb->gear : 1,
		                         out->num, out->den, dtScaled,
		                         r->wheelieScale,
		                         gb ? gb->mode : 1, preRace,
		                         gb && gb->state != 0,
		                         r->matOrientation, r->velDir,
		                         out->dir[1], r->flAirTime,
		                         r->turnBrakeFlag);
	}
}


#define INPUT_TURN         0xcu    /* left | right, tested as CL,0xc */
#define STEER_MIN_SPEED    (2.0/3.6)      /* 0x004d10c0, exact      */
#define STEER_ANALOG_SCALE (1.0 / 32768.0) /* 0x004d10c8 = 1/0x8000 */
#define STEER_RETURN_SCALE (1.0 / 6.0)     /* 0x004d10d0            */
#define TURNBRAKE_MIN_SPEED 2.0f   /* 0x40000000, compared as bits  */
#define AXIS_TURN_LO       0x4000
#define AXIS_TURN_HI       0xc000
#define MOTOTYPE_DIRT      1

/* ORIGIN  maybe_Racer_ApplySteering @ 0x00466240 */
void racer_apply_steering(MR_Racer *r, const MR_BikeCarac *c,
                          unsigned int buttons, int axis1,
                          float slipAbs, float dtScaled)
{
	double limit;
	double yaw;
	double moving;

	if (!r || !c)
		return;

	limit  = (double)c->roueLimiteAngulaire;
	moving = ((double)r->speed >= STEER_MIN_SPEED);

	if (axis1 != (int)ANALOG_CENTRE) {
		/* ---- ANALOG ------------------------------------------ */
		double n = moving ? (double)(axis1 - ANALOG_CENTRE) : 0.0;

		r->leanMode = 1;
		r->steering = (float)(n * limit * STEER_ANALOG_SCALE);
	} else if ((buttons & INPUT_TURN) != 0 && moving) {
		/* ---- DIGITAL ----------------------------------------- */
		double want = ((buttons & 4u) != 0) ? -1.0 : 1.0;
		double v;

		r->leanMode = 0;

		/*
		 * NO COAST-OVER. If the bars are already over the other
		 * way the angle is SNAPPED to zero rather than being
		 * driven through, so a direction change starts from
		 * centre every time.
		 */
		if ((want < 0.0 && r->steering > 0.0f)
			|| (want > 0.0 && r->steering < 0.0f))
		    r->steering = 0.0f;

		r->steerRate = (float)((double)r->steerRate
		                       + (double)c->roueAccAngulaire
		                         * (double)dtScaled);

		v = (double)r->steering + (double)r->steerRate * (double)dtScaled
		    * want;
		if (v > limit)
			v = limit;
		else if (v < -limit)
			v = -limit;
		r->steering = (float)v;
	} else {
		/* ---- CENTRING ---------------------------------------- */
		double k = (double)r->steerReturnScale
		           * (double)c->roueVitesseRetour * STEER_RETURN_SCALE;
		double step;

		r->steerRate = 0.0f;

		if (k < (double)c->roueVitesseRetour)
			k = (double)c->roueVitesseRetour;

		step = k * (double)dtScaled;
		if (r->steering > 0.0f) {
			r->steering = (float)((double)r->steering - step);
			if (r->steering < 0.0f)
				r->steering = 0.0f;      /* snap at the crossing */
		} else if (r->steering < 0.0f) {
			r->steering = (float)((double)r->steering + step);
			if (r->steering > 0.0f)
				r->steering = 0.0f;
		}
	}

	/* ---- turnBrakeFlag, which gates braking ------------------- */

	/*
	 * SET TO 1 FIRST, every racer every frame, at 0x00466504 -
	 * before the two guards that can clear it. Only a human on a
	 * MotoType 1 bike reaches the clearing code.
	 */
	r->turnBrakeFlag = 1;

	if (r->humanPlayer != 0 && c->motoType == MOTOTYPE_DIRT) {
		if (axis1 == (int)ANALOG_CENTRE) {
			/* Braking AND turning together clears it. */
			if ((buttons & INPUT_BRAKE) != 0
			    && (buttons & INPUT_TURN) != 0)
			    r->turnBrakeFlag = 0;
		} else if (axis1 < AXIS_TURN_LO || axis1 > AXIS_TURN_HI) {
			r->turnBrakeFlag = 0;
		}

		/* Forced back on below 2 m/s, so a near-stopped bike can
		 * always brake. */
		if (r->speed < TURNBRAKE_MIN_SPEED)
			r->turnBrakeFlag = 1;
	}
	/*
	 * AND THE TORQUE, which is this same function's tail and not
	 * a separate one: 0x0046658f builds the euler (0, yaw, 0)
	 * with Vec3_Set, turns it into a matrix and hands it to
	 * Racer_ComposeOrientation.
	 *
	 * THE VECTOR IS PURE YAW, which is what a steering torque
	 * must be. It is NOT in the pose array, which is where four
	 * separate searches looked for it.
	 *
	 * THE YAW IS AN INCREMENT - it scales with dt - and
	 * ComposeOrientation MULTIPLIES rather than assigns, which
	 * is what makes that work.
	 *
	 * IT HAS TO HAPPEN HERE, IMMEDIATELY AFTER THE BARS. Apply
	 * the torque from anywhere that also rebuilds matOrientation
	 * - race.c rebuilding it from the segment matrix, say - and
	 * the terrain fit UpdateGroundContact computed this frame is
	 * discarded before anything reads it.
	 */
	yaw = (double)racer_steer_torque(r, c, r->wheelieAngle,
	                                 slipAbs, dtScaled);
	racer_compose_yaw(r->matOrientation, (float)yaw);
}


/* Units per metre is BIKE_UNITS_PER_M in bikes.h - one name for
 * 0x004d102c, which this file used to spell twice. */
#define K_WHEEL_RADIUS    -192.0f   /* 0x004d1030, twice it         */
#define K_WHEELBASE_REAR   -48.0f   /* 0x004d0e94                   */
#define K_WHEELBASE_FRONT   48.0f   /* 0x004d0e98                   */

/* ORIGIN  maybe_Racer_UpdateGroundContact @ 0x00468ca0, its tail -
 *         and the identical block in maybe_Racer_TickAI @ 0x0046bf80 */
void racer_wheel_contacts(const MR_Racer *r, const MR_BikeCarac *c,
                          float rearOut[3], float frontOut[3])
{
	if (!r || !c)
		return;

	racer_wheel_contacts_m(r->matOrientation, r->position, c,
	                       rearOut, frontOut);
}


/*
 * ORIGIN  maybe_Racer_Draw @ 0x0045f290, at 0x0045f87c - which
 *         rebuilds racer+0x1B4 from racer+0x338 and the wheelbase
 *
 * See racer.h for why this is not racer_wheel_contacts.
 */
void racer_wheel_ground(const MR_Racer *r, const MR_BikeCarac *c,
                        float rearOut[3], float frontOut[3])
{
	const float *m;
	float        along;
	int          i;

	if (!r || !c)
		return;

	m = r->matOrientation;

	if (rearOut) {
		along = c->axeDistance * K_WHEELBASE_REAR;
		for (i = 0; i < 3; i++)
			rearOut[i] = m[6 + i] * along + m[9 + i] + r->position[i];
	}
	if (frontOut) {
		along = c->axeDistance * K_WHEELBASE_FRONT;
		for (i = 0; i < 3; i++)
			frontOut[i] = m[6 + i] * along + m[9 + i] + r->position[i];
	}
}


/*
 * ORIGIN  Racer_SpinWheels @ 0x00466940
 *
 * THE WHEEL SPIN, and the whole of it. Called once per racer per
 * tick from maybe_Racer_Tick (0x0045d7b4), maybe_Racer_TickRemote
 * (0x0045f0f6) and maybe_Racer_TickAI (0x0046c74d).
 *
 * Each wheel's angle advances by the distance ITS OWN CONTACT POINT
 * moved along the bike's forward axis, over the wheel's radius:
 *
 *     axis  = matOrientation row3 + row2      (m[9..11] + m[6..8])
 *     rear += dot(rearContact - rearContactPrev, axis)
 *             / (rayonAr * frameDelta * 96)
 *     front+= dot(frontContact - frontContactPrev, axis)
 *             / (rayonAv * frameDelta * 96)
 *
 * 96.0 is 0x004d102c. The two radii differ, so the front turns
 * faster than the rear - measured on MOTO creeping, the rear went
 * 7.89 to 12.08 radians while the front went 8.71 to 13.33.
 *
 * THE AXIS IS ROW3 PLUS ROW2, not row2 alone. That is the original's
 * expression and it is not obviously a direction at all; transcribed
 * as it stands rather than corrected to the forward vector.
 *
 * BRAKING LOCKS THE WHEELS. Bit 1 of the input word returns early,
 * so a braking bike slides with its wheels stopped. For a human the
 * live inputWord is tested; for everyone else tickInputWord.
 *
 * IT ACCUMULATES WITHOUT WRAPPING. The angle grows without bound and
 * is only ever fed to mat34_from_euler, which masks the table index,
 * so the float loses precision eventually and the original does not
 * care. Wrapping it here would diverge.
 */
void racer_spin_wheels(MR_Racer *r, const MR_BikeCarac *c, float frameDelta)
{
	float axis[3];
	float dr, df, den;
	int   i;

	if (!r || !c)
		return;

	/* 0x00466940..0x0046696d - the brake gate. */
	if (r->humanPlayer) {
		if (r->inputWord & 2)
			return;
	} else if (r->tickInputWord & 2) {
		return;
	}

	for (i = 0; i < 3; i++)
		axis[i] = r->matOrientation[9 + i] + r->matOrientation[6 + i];

	dr = df = 0.0f;
	for (i = 0; i < 3; i++) {
		dr += (r->rearWheelContact[i] - r->rearWheelContactPrev[i])
		      * axis[i];
		df += (r->frontWheelContact[i] - r->frontWheelContactPrev[i])
		      * axis[i];
	}

	den = c->rayonAr * frameDelta * BIKE_UNITS_PER_M;
	if (den != 0.0f)
		r->rearWheelSpin[0] += dr / den;
	den = c->rayonAv * frameDelta * BIKE_UNITS_PER_M;
	if (den != 0.0f)
		r->frontWheelSpin[0] += df / den;
}


/*
 * ORIGIN  FUN_00458da0 @ 0x00458da0
 *
 * Wrap to [-pi, pi] by repeated addition. The original really does
 * loop rather than use fmod, and the loop is what a large angle
 * costs it.
 */
float math_wrap_pi(float a)
{
	if (a < (float)-MR_PI) {
		do {
			a = a + (float)MR_TWO_PI;
		} while (a < (float)-MR_PI);
	}
	if ((float)MR_PI < a) {
		do {
			a = a - (float)MR_TWO_PI;
		} while ((float)MR_PI < a);
	}
	return a;
}


/*
 * ORIGIN  FUN_0045ca00 @ 0x0045ca00
 *
 * The chassis attitude that racer_update_suspension lags. Everything
 * it reads is matBikeRoot: the forward row plus the translation is
 * one point, the lateral row plus the translation another, and the
 * angle comes out of the two.
 */
float racer_chassis_attitude(const MR_Racer *r)
{
	const float *m = r->matBikeRoot;
	double fx, fy, fz, lx, lz;
	double horizSq, scale, num;

	fx = (double)m[6] + m[9];
	fy = (double)m[7] + m[10];
	fz = (double)m[8] + m[11];
	lx = (double)m[0] + m[9];
	lz = (double)m[2] + m[11];

	horizSq = fz * fz + fx * fx;
	scale   = sqrt(horizSq) * sqrt(fy * fy + horizSq);

	/* 0045ca5c  the degenerate case: straight up or straight down. */
	if (fabs(scale) < K_ATTITUDE_EPS)
		return (fy > 0.0) ? (float)MR_HALF_PI_F : (float)-MR_HALF_PI_F;

	num = fz * fy * lx - fx * fy * lz;

	return (float)atan2(num / scale, horizSq / scale);
}


/* ORIGIN  Racer_UpdateSuspension @ 0x00466a80 */
void racer_update_suspension(MR_Racer *r, const MR_BikeCarac *c,
                             float frameDelta)
{
	float  dtc;
	float  target, rawTarget, delta;
	double compRate;

	if (!c)
		return;

	/*
	 * 00466a8e  the frame delta is CLAMPED TO [3, 5] before anything
	 * uses it, so a stalled frame cannot throw the spring. The
	 * original compares the float bits with JG/JGE, which is the
	 * same ordering for positive floats.
	 */
	dtc = frameDelta;
	if (dtc > K_SUSP_DT_MAX)
		dtc = K_SUSP_DT_MAX;
	else if (dtc < K_SUSP_DT_MIN)
		dtc = K_SUSP_DT_MIN;

	r->springAmplitude = c->ressorAmplitudeDec * r->springAmplitude;
	r->springCompression = (float)((double)dtc * K_SUSP_RELAX
	                               + (double)r->springCompression);

	/*
	 * 00466aed  FORCED NON-POSITIVE EVERY FRAME. The compression is
	 * a downward impulse, so the relax term above only ever walks it
	 * back up to zero and never past. This is the store a watchpoint
	 * on racer+0x2e95 sees three times a frame.
	 */
	if (r->springCompression > 0.0f)
		r->springCompression = 0.0f;

	compRate = (double)r->springCompression / dtc;

	r->springPhase = r->springAmplitude / c->ressorAmplitudeFactor
	               + r->springPhase;

	/*
	 * 00466b2f  A HARD HIT RESTARTS THE SPRING. When the compression
	 * rate outruns the amplitude the phase is snapped to pi/2 - the
	 * peak of the sine - and the amplitude jumps half way to the new
	 * rate, so the bounce begins at full deflection instead of
	 * fading in.
	 */
	if ((double)r->springAmplitude > compRate) {
		r->springPhase = (float)MR_HALF_PI_F;
		r->springAmplitude =
		    (float)((compRate - (double)r->springAmplitude)
		            * K_SUSP_REARM + (double)r->springAmplitude);
	}
	if (r->springAmplitude < c->ressorAmplitudeMin)
		r->springAmplitude = c->ressorAmplitudeMin;

	r->suspensionAngle = 0.0f;
	r->springBounce = (float)(sin((double)r->springPhase)
	                          * (double)r->springAmplitude);

	/*
	 * 00466b9f  THE TARGET the frame chases: the chassis attitude,
	 * less the heading reference, plus the fork dive.
	 */
	rawTarget = (float)((double)racer_chassis_attitude(r)
	                    - (double)r->headingRef + (double)r->forkDive);
	target = rawTarget;

	/* 00466bbc  both angles into [0, 2pi] before they are compared. */
	if (target < 0.0f) {
		do {
			target = target + (float)MR_TWO_PI;
		} while (target < 0.0f);
	}
	while (target > (float)MR_TWO_PI)
		target = target - (float)MR_TWO_PI;

	if (r->headingSmoothed < 0.0f) {
		do {
			r->headingSmoothed = r->headingSmoothed + (float)MR_TWO_PI;
		} while (r->headingSmoothed < 0.0f);
	}
	while (r->headingSmoothed > (float)MR_TWO_PI)
		r->headingSmoothed = r->headingSmoothed - (float)MR_TWO_PI;

	delta = target - r->headingSmoothed;

	/*
	 * 00466c67  THE SHORT WAY ROUND. Both are in [0, 2pi], so a gap
	 * of more than pi means the pair straddles the wrap and the
	 * frame would chase the long way round the circle.
	 */
	if (fabs((double)delta) > MR_PI) {
		if ((double)r->headingSmoothed + MR_PI < (double)target)
			target = (float)((double)target - MR_TWO_PI);
		if ((double)target < (double)r->headingSmoothed - MR_PI)
			target = (float)((double)target + MR_TWO_PI);
		delta = target - r->headingSmoothed;
	}

	/* 00466ceb  THE FRAME LAGS THE WHEELS by 1/RetardCadre. */
	r->headingSmoothed = delta / c->retardCadre + r->headingSmoothed;

	/*
	 * 00466cf6  AND THE OUTPUT IS MEASURED AGAINST THE UNWRAPPED
	 * TARGET, not the wrapped one - the raw value was saved before
	 * any of the wrapping ran.
	 */
	r->suspensionAngle = r->headingSmoothed - rawTarget;
	r->suspensionAngle = math_wrap_pi(r->suspensionAngle);

	if (r->suspensionAngle > c->angleSuspension)
		r->suspensionAngle = c->angleSuspension;
	if (r->suspensionAngle < -c->angleSuspension)
		r->suspensionAngle = -c->angleSuspension;
}


/* ORIGIN  maybe_Racer_UpdateLean @ 0x00469c60, at 0x0046a343 */
void racer_wheelie_swing_contact(float frontIo[3], const float rear[3],
                                 float wheelieAngle)
{
	double p, c, sn, h, h2, y2;
	float  d[3];

	/*
	 * THE WHEELIE PIVOTS ON THE REAR CONTACT, and it is not a
	 * rotation of the bike matrix at all. UpdateLean swings the
	 * FRONT CONTACT about the rear one and rebuilds matBikeRoot
	 * from the wheel-to-wheel line, so the frame, the fork, the
	 * swingarm and the draw placement all inherit the pitch from
	 * the geometry. racers-convergence.txt 9.18.40.
	 *
	 * COMPOSING THE PITCH ONTO THE DRAW MATRIX GIVES THE RIGHT
	 * ANGLE ABOUT THE WRONG POINT: the bike turns about its own
	 * middle and the rear wheel sinks into the road as the nose
	 * comes up. It looks right from every angle except the one
	 * that shows the rear contact.
	 */
	if (!frontIo || !rear || wheelieAngle == 0.0f)
		return;

	d[0] = frontIo[0] - rear[0];
	d[1] = frontIo[1] - rear[1];
	d[2] = frontIo[2] - rear[2];

	h = sqrt((double)d[0] * d[0] + (double)d[2] * d[2]);
	if (h <= 0.0)
		return;

	/*
	 * NOSE UP IS POSITIVE HERE. K_WHEELIE_PITCH is the original's
	 * own -PI/4 and carries the sign the DRAW matrix wanted; the
	 * contact swing runs the other way, so it is negated. The sign
	 * is measured, not derived: whole-function-import.txt 7 has the
	 * full wheelie against MOTO.
	 */
	p  = sin((double)wheelieAngle) * -K_WHEELIE_PITCH;
	c  = cos(p);
	sn = sin(p);

	h2 = h * c - (double)d[1] * sn;
	y2 = h * sn + (double)d[1] * c;

	frontIo[0] = (float)((double)rear[0] + (double)d[0] * (h2 / h));
	frontIo[1] = (float)((double)rear[1] + y2);
	frontIo[2] = (float)((double)rear[2] + (double)d[2] * (h2 / h));
}


/* ORIGIN  maybe_Racer_UpdateLean @ 0x00469c60, at 0x00469e2b */
void racer_wheel_contacts_m(const float mat[12], const float pos[3],
                            const MR_BikeCarac *c,
                            float rearOut[3], float frontOut[3])
{
	const float *m;
	float        h;
	float        up;
	float        along;
	int          i;

	if (!mat || !pos || !c)
		return;

	m = mat;
	h = c->hauteurCadre * BIKE_UNITS_PER_M;

	if (rearOut) {
		up    = c->rayonAr * K_WHEEL_RADIUS + h;
		along = c->axeDistance * K_WHEELBASE_REAR;
		for (i = 0; i < 3; i++) {
			rearOut[i] = m[3 + i] * up + m[6 + i] * along
			             + m[9 + i] + pos[i];
		}
	}

	if (frontOut) {
		up    = c->rayonAv * K_WHEEL_RADIUS + h;
		along = c->axeDistance * K_WHEELBASE_FRONT;
		for (i = 0; i < 3; i++) {
			frontOut[i] = m[3 + i] * up + m[6 + i] * along
			              + m[9 + i] + pos[i];
		}
	}
}


#define FX_SEG_BEHIND        10     /* the window, behind the camera */
#define FX_SEG_AHEAD         30     /* and ahead of it               */
#define SKID_WIDEN           2.0    /* 0x004d0e88, shared with the
                                     * centripetal doubler           */
#define SKID_NARROW          1.0f   /* FLD1                          */
#define SKID_MIN             1.0f   /* 0x3f800000                    */
#define SKID_MAX            12.0f   /* 0x41400000                    */
#define SKID_AI             5.0f    /* 0x40a00000, pinned            */
#define FX_PARTICLES_HUMAN    24
#define FX_PARTICLES_AI        4
#define FX_SPEED_MIN         5.0f   /* 0x40a00000                    */
#define FX_SLIP_MIN          0.5f   /* 0x3f000000                    */
#define FX_WHEELIE_WINDOW   25.0f   /* 0x41c80000                    */
#define FX_ROT_SCALE     0.03125    /* 0x004d1150, a DOUBLE = 1/32   */
#define FX_SPRAY_LERP    0.03125f   /* 0x004d1158, a FLOAT - the two
                                     * are the same number at
                                     * different widths, and reading
                                     * 0x004d1158 as a double gives
                                     * 5.06e-315                     */
#define FX_SPRAY_LIFT      10.0f    /* 0x004d10a0, a FLOAT           */
#define FX_SPRAY_SIZE        14     /* the literal 0xe at 0x00467909 */
#define FX_SPRAY_COLOUR  0x2104u    /* Color_Pack16(4, 4, 4)         */

/* ORIGIN  maybe_Racer_UpdateSkeleton @ 0x00467b30, its slot choice */
int racer_select_skeleton_slot(int crashLatch, int handSkeleton,
                               int lodSlot)
{
	int slot = (crashLatch || handSkeleton) ? 1 : lodSlot;

	/*
	 * The caller fills the pose here, but only when slot < 2.
	 *
	 * THE SECOND FORCE IS NOT REDUNDANT. SelectLod cannot have run
	 * when handSkeleton is set, but the pose update can have moved
	 * the slot underneath - so the original writes 1 again after
	 * the fill, and so does this.
	 */
	if (handSkeleton)
		slot = 1;

	return slot;
}


/*
 * ORIGIN  NONE - ours. Vec3_TransformByMatrix34 (0x00410010) spelled
 * out; the emitter needs it and racerdraw's copy is file-local.
 */
static void racer_transform_by_matrix34(float out[3], const float v[3],
                                        const float m[12])
{
	int i;

	for (i = 0; i < 3; i++)
		out[i] = v[0] * m[i] + v[1] * m[3 + i] + v[2] * m[6 + i]
	           + m[9 + i];
}


/*
 * racer_heading - the bike's forward direction.
 *
 * ORIGIN  maybe_Racer_PlaceWheels @ 0x0045c92e, the store at
 *         0x0045c9cd. Racer_AiFollowLine @ 0x0046b517 builds
 *         racer+0x198 the same way before normalising it.
 */
void racer_heading(const MR_Racer *r, float out[3])
{
	int i;

	if (!r || !out)
		return;
	for (i = 0; i < 3; i++)
		out[i] = r->matOrientation[6 + i] + r->matOrientation[9 + i];
}


/* maybe_Racer_PlaceWheels' three .rdata constants. */
#define K_WHEEL_RADIUS_SCALE  -96.0f  /* 0x004d0e90                 */
#define K_PLACE_MID             0.0f  /* 0x004d0ea0 - reads ZERO,
                                       * so the +0x4bc triple
                                       * collapses to row3 + pos  */

/*
 * ORIGIN  maybe_Racer_PlaceWheels @ 0x0045c7c0
 *
 * WHOLE. Puts a racer somewhere and places everything that hangs
 * off its position - both wheel instances, the three history
 * copies, the segment and one unreconstructed triple.
 *
 * THE WHEEL PAIR IS THE POINT, and it is NOT racer_wheel_ground:
 *
 *     rear  = pos + M * (0, -96 * rayonAr, -48 * axeDistance)
 *     front = pos + M * (0, -96 * rayonAv, +48 * axeDistance)
 *
 * USE THE WRONG ONE AND THE BIKE FLOATS. Under an identity
 * orientation the ground builder gives y = 0 for both wheels where
 * this gives -30.72 and -27.84, so a menu placing its wheels the
 * race way stands the machine thirty units off the floor.
 * runtime-capture.txt 6.1.
 *
 * racer+0x1b4 and racer+0x338 are DUAL PURPOSE, which is what makes
 * that easy to get wrong: they are the position fields of the wheel
 * model instances at racer+0x1a8 and +0x32c, so at rest they hold
 * the OFFSET this writes and in a race the ground fit overwrites
 * them with world CONTACT points carrying no radius term.
 *
 * AND THIS REALLY DOES WRITE +-84.0 IN Z. A read of racer+0x1b4
 * after a DRAW sees 83.9753 instead, because maybe_Racer_Draw
 * rebuilds the front contact at 0x0045f87c by stepping the
 * wheelbase along the normalised rear-to-front direction - which is
 * tilted, the two radii differing. Not a defect here, and not float
 * noise; do not fudge this arithmetic to match a capture.
 * menu-pages.txt 2.7.4.5.
 *
 * ONE HOLE, at 0x0045c8a4: a triple at racer+0x4bc inside the
 * unreconstructed block at +0x350. It collapses to row3 + position
 * - K_PLACE_MID above - and is written only because the original
 * writes it. [open]
 *
 * THE SEGMENT LOOKUP NEEDS A TRACK and page 10 has none, so it is
 * skipped when t is NULL - the original always has one loaded by
 * the time it runs.
 */
void racer_place_wheels(MR_Racer *r, const MR_BikeCarac *c,
                        const float pos[3], const MR_Trk *t)
{
	float v[3];
	float out[3];
	int   i;

	if (!r || !c || !pos)
		return;

	for (i = 0; i < 3; i++) {
		r->position[i]      = pos[i];
		r->positionPrev[i]  = pos[i];
		r->positionPrev2[i] = pos[i];
	}

	v[0] = 0.0f;
	v[1] = c->rayonAr * K_WHEEL_RADIUS_SCALE;
	v[2] = c->axeDistance * K_WHEELBASE_REAR;
	racer_transform_by_matrix34(out, v, r->matOrientation);
	for (i = 0; i < 3; i++)
		r->rearWheelContact[i] = out[i] + r->position[i];

	v[0] = 0.0f;
	v[1] = c->rayonAv * K_WHEEL_RADIUS_SCALE;
	v[2] = c->axeDistance * K_WHEELBASE_FRONT;
	racer_transform_by_matrix34(out, v, r->matOrientation);
	for (i = 0; i < 3; i++)
		r->frontWheelContact[i] = out[i] + r->position[i];

	/* 0x0045c8a4 - the marked hole at racer+0x4bc. */
	{
		float mid = c->axeDistance * K_PLACE_MID;
		float *q  = (float *)(r->unk_0350 + 0x16c);

		for (i = 0; i < 3; i++)
			q[i] = r->matOrientation[3 + i] * mid
		         + r->matOrientation[9 + i] + r->position[i];
	}

	if (t) {
		/*
		 * segmentStart ONLY. segmentPrev is the PREVIOUS segment
		 * and race_update_progress owns it - copying the current
		 * one over it here made the frame-to-frame delta zero and
		 * silently disabled progress, the standings and the
		 * checkpoint sweep.
		 */
		r->segmentStart = trk_find_segment_for_point(t,
		                      r->segmentStart, r->position, NULL);
	}

	for (i = 0; i < 3; i++) {
		r->placeWheels0[i] = pos[i];
		r->placeWheels1[i] = pos[i];
		r->placeWheels2[i] = pos[i];
	}

	/* 0x0045c92e  racer+0x198 is the AI's aim direction. */
	racer_heading(r, r->aimDir);
}


/*
 * ORIGIN  Trk_SurfaceAtPoint @ 0x0040ec60, by way of trk.c.
 *
 * Returns the surface id under the racer, which gates the tyre
 * trail. 0 is the road and draws nothing.
 */
static int racer_surface_at_point(const MR_Racer *r, const MR_Trk *trk)
{
	/*
	 * THE BIKE'S POSITION, NOT A WHEEL CONTACT. 0x00466f31 loads
	 * EDI with racer+0x30 and pushes its three floats, and the
	 * segment is segmentStart from racer+0x31c1.
	 */
	if (!r || !trk)
		return 0;
	return trk_surface_at_point(trk, r->segmentStart, r->position);
}


/*
 * ORIGIN  DAT_004ea508 and DAT_004ea50c, the trail's cadence
 *
 * A free-running phase and a per-frame toggle, both advanced only by
 * the VIEWED racer so the field shares one rhythm. Statics rather
 * than MR_Racer fields because that is what they are in the original.
 */
#define TRAIL_PHASE_WRAP   50.0f   /* 0x004d1064, a FLOAT           */
#define TRAIL_PHASE_EMIT   30.0f   /* the 0x41f00000 bit compare    */
#define TRAIL_SIZE_K       64.0f   /* 0x004d1060                    */
#define TRAIL_SIZE_MIN        4    /* CMP EAX,4 / JLE at 0x00466f8a */
#define TRAIL_SLIP_ONE      1.0f   /* 0x004d0e70                    */
#define TRAIL_SLIP_K        5.0f   /* 0x004d0f78                    */
#define TRAIL_SPEED_K       0.55   /* 0x004d1138, a DOUBLE          */
#define TRAIL_LIFE          0x28   /* the literal 40                */
#define TRAIL_GREY_A  ((9u << 11) | (9u << 6) | 9u)   /* Pack16(9,9,9) */
#define TRAIL_GREY_B  ((8u << 11) | (8u << 6) | 8u)   /* Pack16(8,8,8) */

static float s_racer_trailPhase;
static int   s_racer_trailToggle;

/*
 * ORIGIN  maybe_Racer_EmitWheelFx @ 0x00466d70
 *
 * WHOLE. Skid marks, tyre trail and spray, called once per racer per
 * tick from all three tick variants and only when
 * maybe_Racer_UpdateGroundContact set its flag.
 *
 * THE FIRST BLOCK IS COPY PROTECTION AND IS NOT RECONSTRUCTED. The
 * original opens with check B - the eight-byte table walked against
 * maybe_CdAudio_GetTrackPos - and on failure draws ILLEGAL VERSION
 * and falls straight through. Unlike the other three sites it has NO
 * FIELD PAYLOAD, so skipping it changes nothing a racer can see. See
 * copy-protection.txt 5.2.
 *
 * ARGUMENTS THE ORIGINAL READS AS GLOBALS: the viewed racer's
 * segment and index, and g_frameDelta. Passed in rather than
 * mirrored, the same way MR_PoseEnv carries the pose globals.
 */
void racer_emit_wheel_fx(MR_Racer *r, unsigned int buttons,
                         const MR_Trk *trk, int viewedSegment,
                         int viewedIndex, float frameDelta)
{
	float slip, slipAbs;
	float lat[3], latNeg[3], v[3];
	float a[3], b[3];
	int   surface, i, n;

	if (!r || !trk)
		return;

	/* 1. THE RANGE GATE, 0x00466f26. FX exist only near the camera. */
	if (!racer_fx_in_range(r->segmentStart, viewedSegment,
	                       trk->segmentCount))
	    return;

	/*
	 * velDir (+0x130), NOT moveDir (+0x18c). Every caller of
	 * maybe_Racer_SlipSpeed passes racer+0x24, and the function
	 * reads instance+0x10c off it - which is velDir. The two
	 * fields are EQUAL while the bike goes where it points, so a
	 * straight-line A/B cannot tell them apart; in a braking turn
	 * they diverge by 270x and the skid ribbon widens on the wrong
	 * signal.
	 */
	slip    = physics_slip_speed(r->matOrientation, r->velDir, r->speed);
	slipAbs = (slip < 0.0f) ? -slip : slip;

	/*
	 * 2. THE TRAIL, 0x00466f5e. Gated on onGround AND a POSITIVE
	 * surface id, so it is the road that decides whether a tyre
	 * leaves a trail at all.
	 */
	surface = racer_surface_at_point(r, trk);
	if (r->onGround && surface > 0) {
		float d[3], len;

		for (i = 0; i < 3; i++)
			d[i] = r->rearWheelContact[i] - r->rearWheelContactPrev[i];
		len = (float)sqrt((double)d[0] * d[0] + (double)d[1] * d[1]
		                  + (double)d[2] * d[2]);
		if (len != 0.0f) {
			float inv = 1.0f / len;

			for (i = 0; i < 3; i++)
				d[i] *= inv;
		}
		/*
		 * THE SPECK SIZE, two __ftol results blended 1:2 -
		 * 0x00466f71..0x00467011, both operands recovered from the
		 * disassembly because the decompiler lost them.
		 *
		 *   n = (int)(speed / realSpeedMax * 64.0)   0x004d1060
		 *       and NOTHING is emitted unless n > 4
		 *   A = matOrientation row2 + its translation - the image
		 *       of the unit Z basis point, the same construction
		 *       camera_view_yaw uses, so A is the heading
		 *   m = (int)(n * (1.0 - |dot(A, moveDir)|) * 5.0)
		 *       0x004d0e70 and 0x004d0f78
		 *   size = (n + 2 * m) / 3
		 *
		 * SO IT GROWS WITH SPEED AND WITH SLIP. dot is 1 when the
		 * bike goes where it points, which makes m zero and the
		 * speck its smallest; sliding opens it up.
		 */
		{
			const MR_BikeCarac *bc = bike_at(r->bikeIndex);
			float A[3], dot, fabsDot;
			int   nSpeed, mTerm, size;

			/*
			 * THE CARAC'S realSpeedMax, NOT THE RACER'S. The
			 * original divides by racer+0xb4, but that field
			 * HAS NO WRITER in this port - MOTO holds 80.0 and
			 * ours holds 0.0, so following the original here
			 * divided by zero and the n > 4 gate could never
			 * pass. Every other limiter in the port already
			 * reads the carac for the same reason; physics.c
			 * around physics_set_long_speed says why.
			 */
			nSpeed = (bc && bc->realSpeedMax != 0.0f)
			       ? (int)(r->speed / bc->realSpeedMax
			               * TRAIL_SIZE_K)
			       : 0;
			if (nSpeed > TRAIL_SIZE_MIN
			    && s_racer_trailPhase < TRAIL_PHASE_EMIT) {
				for (i = 0; i < 3; i++)
					A[i] = r->matOrientation[6 + i]
				         + r->matOrientation[9 + i];
				dot = A[0] * r->moveDir[0]
				    + A[1] * r->moveDir[1]
				    + A[2] * r->moveDir[2];
				fabsDot = (dot < 0.0f) ? -dot : dot;
				mTerm = (int)((float)nSpeed
				              * (TRAIL_SLIP_ONE - fabsDot)
				              * TRAIL_SLIP_K);
				size = (nSpeed + mTerm * 2) / 3;

				fx_spawn_trail(size, r->position, d, NULL,
				               (float)((double)r->speed
				                       * TRAIL_SPEED_K),
				               TRAIL_LIFE,
				               TRAIL_GREY_B, TRAIL_GREY_A,
				               surface);
			}
		}

		/*
		 * AND THE CADENCE, 0x00467183 - a free-running phase that
		 * only the VIEWED racer advances, so the whole field emits
		 * on one rhythm rather than each on its own.
		 *
		 * It rises by frameDelta, wraps at 50.0, and the spawn
		 * above is gated on it being under 30.0 - so roughly nine
		 * ticks of specks then six of nothing, which is the
		 * streak-then-pause the trail draws on the ground.
		 *
		 * THE SECOND TEST IS THE SAME THRESHOLD read as integer
		 * bits (0x42480000 is 50.0f). Belt and braces in the
		 * original; reproduced rather than tidied.
		 */
		if (r->slotIndex == viewedIndex) {
			s_racer_trailPhase += frameDelta;
			if (TRAIL_PHASE_WRAP < s_racer_trailPhase)
				s_racer_trailPhase -= TRAIL_PHASE_WRAP;
			if (s_racer_trailPhase > TRAIL_PHASE_WRAP)
				s_racer_trailPhase = 0.0f;
			s_racer_trailToggle = (s_racer_trailToggle == 0);
		}
	}

	/*
	 * 3. NOTHING BELOW FIVE UNITS A SECOND, 0x004671f4. The whole
	 * remainder - ribbons and spray both - sits behind it.
	 */
	if (r->speed <= FX_SPEED_MIN)
		return;

	/*
	 * A FRESH RIBBON ON THE RISING EDGE, of the brake bit or of
	 * fxGate. Two slots, one per wheel, and the width resets with
	 * them. maybe_Fx_AllocSlot is a ring of sixteen, so the oldest
	 * mark is recycled rather than the new one refused.
	 */
	if (((r->tickInputWord & 2u) == 0 && (buttons & 2u) != 0)
		|| (r->fxGatePrev == 0 && r->fxGate != 0)) {
		r->fxSlot0   = fx_ribbon_alloc_slot();
		r->skidWidth = SKID_MIN;
		r->fxSlot1   = fx_ribbon_alloc_slot();
		if (!r->humanPlayer)
			r->skidWidth = SKID_AI;
	}

	/* The half-width, out along the bike's own lateral axis. */
	v[0] = r->skidWidth;
	v[1] = 0.0f;
	v[2] = 0.0f;
	racer_transform_by_matrix34(lat, v, r->matOrientation);
	v[0] = -r->skidWidth;
	racer_transform_by_matrix34(latNeg, v, r->matOrientation);

	/*
	 * 4. THE TWO RIBBONS, 0x00467459 and 0x00467518 - rear wheel to
	 * fxSlot0, front to fxSlot1. Braking, or sliding hard enough,
	 * and only with the wheels down.
	 */
	if (((buttons & 2u) != 0
	     || (r->fxGate != 0 && slipAbs > FX_SLIP_MIN))
	    && r->onGround) {
		for (i = 0; i < 3; i++) {
			a[i] = r->rearWheelContact[i] + lat[i];
			b[i] = r->rearWheelContact[i] + latNeg[i];
		}
		fx_ribbon_append(r->fxSlot0, a, b, r->segmentStart, trk);
		for (i = 0; i < 3; i++) {
			a[i] = r->frontWheelContact[i] + lat[i];
			b[i] = r->frontWheelContact[i] + latNeg[i];
		}
		fx_ribbon_append(r->fxSlot1, a, b, r->segmentStart, trk);

		if (r->humanPlayer) {
			r->skidWidth     = racer_skid_width(r->skidWidth,
			                                   r->skidWidthPrev,
			                                   slipAbs, 1);
			r->skidWidthPrev = slipAbs;
		}
	}

	/*
	 * 5. THE SPRAY, 0x0046757e. A wheelie in its first 25 units, OR
	 * the brake, OR a hard slide - and the wheels down.
	 *
	 * THE COUNT IS THE ORIGINAL'S LOD CUT - 24 particles for a human
	 * rider and 4 for an AI - and is kept.
	 */
	if (((((r->wheelieAngle != 0.0f)
		   && r->wheelieTimer < FX_WHEELIE_WINDOW)
	      || (buttons & 2u) != 0)
	     || (r->fxGate != 0 && slipAbs > FX_SLIP_MIN))
	    && r->onGround) {
		n = racer_fx_particle_count(r->humanPlayer);
		for (i = 0; i < n; i++) {
			float pos[3], vel[3], rot[3];
			float throwFwd, throwLat;
			int   life;
			int   r1, r2, r3, r4, r5, r6;
			int   k;

			/*
			 * SIX DRAWS PER PARTICLE AND THE ORDER MATTERS -
			 * one for the life, three for the spin, two for the
			 * throw. Getting the order wrong gives a plausible
			 * spray that will never match a capture.
			 *
			 * STREAM 1, NOT THE CRT rand().
			 * maybe_Racer_EmitWheelFx calls maybe_Rand
			 * (0x00458cd0) at eight sites, and that is
			 * Rand_Next(stream 1). The simulation's draws - the
			 * crash fall side, the AI sink, the wall roll - go
			 * through Game_RandNext (0x00458ce0), the CRT
			 * generator. Twenty-four particles at six draws each
			 * taken from the SIMULATION's stream would move every
			 * later draw in it, which is what makes a replay stop
			 * matching. rand.h.
			 */
			r1 = rand_next(1);
			r2 = rand_next(1);
			r3 = rand_next(1);
			r4 = rand_next(1);

			/*
			 * THE SPIN IS PER PARTICLE HERE. maybe_Fx_Burst draws
			 * its three rands ONCE for the whole burst, so a crash
			 * puff spins as one body while the spray does not.
			 */
			rot[0] = (float)fx_sext(r4, 0x1f) * (float)FX_ROT_SCALE;
			rot[1] = (float)fx_sext(r3, 0x1f) * (float)FX_ROT_SCALE;
			rot[2] = (float)fx_sext(r2, 0x1f) * (float)FX_ROT_SCALE;

			/*
			 * THE SPREAD FLIPS WITH buttons & 4 - steering one way
			 * throws the spray the other.
			 */
			r5 = rand_next(1);
			r6 = rand_next(1);
			throwFwd = (float)(r5 & 3);
			throwLat = (float)((buttons & 4u) ? (r6 & 3)
			                                  : -(r6 & 3));

			/*
			 * THE MIDDLE ROW DROPS OUT - the original multiplies
			 * m[3], m[4] and m[5] by 0.0, so the throw is a 0..3
			 * square in the bike's LATERAL and FORWARD axes plus the
			 * translation row. A bike going down sideways throws its
			 * spray sideways.
			 */
			for (k = 0; k < 3; k++) {
				vel[k] = r->matOrientation[k] * throwLat
				       + r->matOrientation[6 + k] * throwFwd
				       + r->matOrientation[9 + k];
			}

			/*
			 * LERPED BACK ALONG THE WHEEL'S OWN TRAVEL, i steps of
			 * 1/32, so the 24 particles lay a trail behind the tyre
			 * instead of stacking on one point. Plus the 10.0 lift -
			 * measured, a spray at a bike on y = 0 reads y = 6.9
			 * after 6.6 ticks of the -0.0864 gravity.
			 */
			for (k = 0; k < 3; k++) {
				pos[k] = (r->rearWheelContactPrev[k]
				          - r->rearWheelContact[k])
				         * FX_SPRAY_LERP * (float)i
				       + r->rearWheelContact[k];
			}
			pos[1] += FX_SPRAY_LIFT;

			/* 30 or 60. rand() is never negative, so fx_sext is an
			 * identity here and the mask is what does the work. */
			life = (fx_sext(r1, 1) * 2 + 2) * 15;

			fx_spawn_particle(FX_SPRAY_SIZE, pos, vel, rot, life,
			                  FX_SPRAY_COLOUR, 5);
		}
	}
	(void)viewedIndex;
	(void)frameDelta;
}


/* ORIGIN  maybe_Racer_EmitWheelFx @ 0x00466d70, its range gate */
int racer_fx_in_range(int segment, int viewedSegment, int segmentCount)
{
	int d;

	if (segmentCount <= 0)
		return 0;

	/* The signed wrapped difference - see racer.h on why this form. */
	d = segment - viewedSegment;
	while (d < -segmentCount / 2)
		d += segmentCount;
	while (d > segmentCount / 2)
		d -= segmentCount;

	return (d >= -FX_SEG_BEHIND && d <= FX_SEG_AHEAD);
}


/* ORIGIN  maybe_Racer_EmitWheelFx @ 0x00466d70, at 0x00467546 */
float racer_skid_width(float width, float prevLateralAbs,
                       float lateralAbs, int humanPlayer)
{
	double w;

	/* An AI never runs the update - a constant-width ribbon. */
	if (!humanPlayer)
		return SKID_AI;

	/*
	 * The original compares the two magnitudes having cleared the
	 * sign bit with AND 0x7fffffff, which is what fabs is here.
	 */
	if (prevLateralAbs < lateralAbs)
		w = (double)width + SKID_WIDEN;
	else
		w = (double)width - (double)SKID_NARROW;

	if (w < (double)SKID_MIN)
		w = (double)SKID_MIN;
	else if (w > (double)SKID_MAX)
		w = (double)SKID_MAX;

	return (float)w;
}


/* ORIGIN  maybe_Racer_EmitWheelFx @ 0x00466d70, the spray count */
int racer_fx_particle_count(int humanPlayer)
{
	return humanPlayer ? FX_PARTICLES_HUMAN : FX_PARTICLES_AI;
}


#define FORK_LOWPASS       0.5      /* 0x004d1018                    */
#define FORK_ACCEL_SCALE  -0.09817477  /* 0x004d1108, = -PI/32       */
#define FORK_CLAMP         0.19634954  /* 0x004d1110, = +PI/16       */
#define FORK_DT_SCALE      0.01     /* 0x004d0ed8, shared with the
                                     * decay and with cornering      */

/* ORIGIN  maybe_Racer_UpdateForkDive @ 0x004667e0 */
void racer_update_fork_dive(MR_Racer *r, float longSpeed,
                            float speedTail, float *prevAccel,
                            float frameDelta)
{
	double dv;
	double a;
	double want;

	if (!r || !prevAccel)
		return;

	dv = (double)longSpeed - (double)speedTail;

	/*
	 * steerReturnScale is an ACCELERATION - see racer.h. Guarded
	 * because the original would divide by zero on a zero step and
	 * only survives because the tick guards dt twice before here.
	 */
	if (frameDelta != 0.0f)
		r->steerReturnScale = (float)(dv / ((double)frameDelta
	                                        * FORK_DT_SCALE));

	/* A two-tap low pass on the same difference. */
	a = (dv + (double)*prevAccel) * FORK_LOWPASS;

	if (a != (double)*prevAccel) {
		want = a * FORK_ACCEL_SCALE;

		/* PEAK HOLD - it only grows here. */
		if (a != 0.0
		    && fabs((double)r->forkDive) < fabs(want))
		    r->forkDive = (float)want;

		if ((double)r->forkDive > FORK_CLAMP)
			r->forkDive = (float)FORK_CLAMP;
		else if ((double)r->forkDive < -FORK_CLAMP)
			r->forkDive = (float)-FORK_CLAMP;
	}

	*prevAccel = (float)a;
}


#define CRASH_FALL_SIDE  MR_HALF_PI_F   /* pi/2, either sign */

/*
 * THE CRASH CONSTANTS, all read out of MOTO.EXE. They are round
 * numbers in km/h converted: the rider loses 15 km/h a second
 * sliding and 2 km/h a second in the air, and gravity carries the
 * same 1/100 the dt does. Written in km/h exactly like Frein.
 *
 * WATCH THE WIDTHS. 0x004d1180 and 0x004d0ed8 are DOUBLES whose
 * low dwords read as plausible floats - 0.0 and 89128.96.
 */
#define CRASH_TIMER_START      400.0f     /* 0x00467e6f */
#define CRASH_UP_BIAS          0.6        /* 0x004d1180 */
#define CRASH_RIDER_SPEED_K    0.15       /* 0x004d1188 */
#define CRASH_DT_SCALE         0.01f      /* 0x004d1004 */
#define CRASH_LAUNCH_SCALE     128.0      /* 0x004d0ef8 */
#define CRASH_GRAVITY          -0.0981    /* 0x004d11a0, -9.81/100 */
#define CRASH_AIR_DRAG         (-2.0/3.6) /* 0x004d11a8, exact     */
#define CRASH_GROUND_DRAG     (-15.0/3.6) /* 0x004d1190, exact     */
#define CRASH_BOUNCE           0.01       /* 0x004d0ed8 */
#define CRASH_FX_THRESHOLD     0.05       /* 0x004d1198 */
#define CRASH_DECAY            0.9        /* 0x004d11b8 */
#define CRASH_WALL_DAMP        0.5        /* 0x004d0f08 */
#define CRASH_AIR_CLEARANCE    8          /* an INT add, 0x004684f6 */
#define CRASH_HOLD_MS          100.0f     /* 0x42c7ffff as a bit test */

/*
 * ONE TIMER UNIT IS TEN MILLISECONDS, and flCrashTimerMs is a
 * MISNOMER - it does not hold ms. g_frameDeltaScaled is 3.3 per
 * 33 ms tick, so a unit is a centisecond, and the 400 above is
 * FOUR SECONDS rather than four hundred milliseconds. Reading it
 * the other way makes the delay four hundred per cent short.
 * racers-convergence.txt 9.66.1 has the arithmetic and 9.17.2 the measurement.
 *
 * Only the ini override below needs it; nothing in the original
 * converts, because nothing in the original talks in ms.
 */
#define CRASH_TIMER_UNIT_MS     10.0f

/*
 * THE LAY-DOWN PITCH, AND IT IS NOT crashFallSide - the two are
 * easy to confuse because both are +-pi/2. This one is a literal
 * -pi/2 about X, reaching BeginCrash as the constant angle-table
 * indices 0x2000 and 0x3000 at 0x00467fe0 and UpdateCrash as the
 * plain float at 0x00468640. Both sites use it whichever way the
 * rider went down, so a crash to the left and one to the right
 * start from the same attitude and only the YAW rate separates
 * them.
 */
#define CRASH_LAY_PITCH        (-MR_HALF_PI_F)


/*
 * racer_lay_rider_root - put the rider flat, keeping his heading.
 *
 * ORIGIN  NONE - ours.
 *
 * NOT A SLICE. No function in MOTO.EXE does this - it is INLINE
 * code inside two the port imports WHOLE, maybe_Racer_BeginCrash
 * (0x00467e30) and maybe_Racer_UpdateCrash (0x004683e0), each
 * keeping its own single-address tag so the A/B loop still
 * attaches on both sides. What this factors out is a repeated
 * IDIOM within them, so a two-address ORIGIN would claim it was
 * imported from sites it was not.
 *
 * BOTH SITES BUILD THE SAME MATRIX:
 *
 *     yaw          = Racer_GetYaw(matRiderRoot)
 *     matRiderRoot = Euler(-pi/2, 0, 0) * Euler(0, -yaw, 0)
 *
 * AND ONLY ONE OF THEM DECOMPILES THAT WAY, which is why this
 * resembles neither. 0x00468640 names the builder outright;
 * 0x00467fb4 is the same sequence with the euler constant-folded
 * into raw sin-table reads at 0x005fcb80 and 0x00608b80, so no
 * call survives. Transcribed from the readable site, applied to
 * the folded one, and the equivalence then MEASURED on both games
 * rather than assumed - same rider root, and a yaw decay of
 * 0.0464, 0.0410, 0.0357 on each side.
 *
 * ONLY THE YAW SURVIVES: a scalar is all that is read out of the
 * old matrix, so any pitch or roll it carried is discarded.
 *
 * UpdateCrash CALLS THIS ON EVERY GROUNDED FRAME, which is what
 * confines the free tumble to the air. A port that lets the tumble
 * keep running on the ground rolls the rider through the track.
 * port-divergences.txt 2.23.
 */
void racer_lay_rider_root(MR_Racer *r)
{
	float lay[3];
	float spin[3];
	float layRot[9];
	float spinRot[9];
	float a[12];
	float b[12];
	float out[12];
	int   i;

	if (!r)
		return;

	lay[0]  = CRASH_LAY_PITCH;
	lay[1]  = 0.0f;
	lay[2]  = 0.0f;
	spin[0] = 0.0f;
	spin[1] = -racer_matrix_yaw(r->matRiderRoot);
	spin[2] = 0.0f;

	mat34_from_euler(layRot, lay);
	mat34_from_euler(spinRot, spin);

	for (i = 0; i < 9; i++) {
		a[i] = layRot[i];
		b[i] = spinRot[i];
	}
	a[9] = a[10] = a[11] = 0.0f;
	b[9] = b[10] = b[11] = 0.0f;

	mat34_multiply(out, a, b);
	for (i = 0; i < 12; i++)
		r->matRiderRoot[i] = out[i];
}

/*
 * racer_begin_crash - throw the rider off the bike.
 *
 * ORIGIN  maybe_Racer_BeginCrash @ 0x00467e30
 *
 * ONE-SHOT: it returns immediately if crashLatch is already set,
 * and setting that flag is what latches the state.
 *
 * flRiderSpeed / flRiderSpeedRef IS A PROGRESS FRACTION - 1.0 at
 * the throw, 0 when he stops sliding. +0x30d5 IS A SPEED AND NOT
 * A TIMER, whatever its name suggests; the countdown is +0x30c5.
 *
 * THE 0.6 UPWARD BIAS IS ADDED BEFORE THE NORMALISE, so a rider
 * hit flat is still thrown upward and arcs rather than skidding,
 * and a bigger horizontal impact tilts the launch flatter. One
 * add for behaviour that would otherwise need a special case.
 *
 * THE FALL SIDE COMES FROM THE LEAN, and only a dead-upright
 * crash is a coin toss: flLeanTarget, then lean, then rand bit 0.
 * So a rider dropped mid-corner goes over the inside.
 */
int racer_begin_crash(MR_Racer *r, MR_Gearbox *gb, const float dir[3])
{
	float pick;
	float len;
	int   i;

	if (!r || !dir)
		return 0;
	if (r->crashLatch != 0)
		return 0;              /* already down - one-shot */

	for (i = 0; i < 3; i++)
		r->f_31a1[i] = dir[i];
	r->ghostEvent = 1;

	/*
	 * BACK TO FIRST GEAR. The original writes racer+0x7c; our
	 * gearbox is still a separate object, so both are set until
	 * MR_Gearbox is retired the way MR_Crash just was. [open]
	 */
	r->gear = 1;
	if (gb)
		gb->gear = 1;

	r->crashLatch     = 1;

	/*
	 * [OPTIONS_CUSTOM] CrashRecoveryMs - A DEBUG OVERRIDE, and -1
	 * is the default, so unset this takes the original branch
	 * unchanged. CLAUDE.md rule 5a.
	 *
	 * THE VALUE IS THE CEILING, not the wait you feel. It lands in
	 * CRASH_TIMER_START, and racer_update_crash has TWO exits: the
	 * timer reaching 0, and the rider having stopped with the
	 * timer already down to CRASH_HOLD_MS. So the normal remount
	 * is a whole second earlier than whatever is set here -
	 * 4000 gives retail's 3 s and 4 s.
	 *
	 * AND IT IS MILLISECONDS GOING IN, centiseconds once stored:
	 * see CRASH_TIMER_UNIT_MS. The divide is not decoration.
	 */
	if (g_options.crashRecoveryMs >= 0) {
		r->flCrashTimerMs = (float)g_options.crashRecoveryMs
		                    / CRASH_TIMER_UNIT_MS;
	} else {
		r->flCrashTimerMs = CRASH_TIMER_START;
	}

	for (i = 0; i < 3; i++)
		r->crashLaunchDir[i] = dir[i];
	r->crashLaunchDir[1] = (float)((double)r->crashLaunchDir[1]
	                               + CRASH_UP_BIAS);
	len = (float)sqrt((double)
	          (r->crashLaunchDir[0] * r->crashLaunchDir[0]
	           + r->crashLaunchDir[1] * r->crashLaunchDir[1]
	           + r->crashLaunchDir[2] * r->crashLaunchDir[2]));
	if (len != 0.0f) {
		for (i = 0; i < 3; i++)
			r->crashLaunchDir[i] /= len;
	}

	r->flRiderSpeed    = (float)((double)r->speed
	                             * CRASH_RIDER_SPEED_K);
	r->flRiderSpeedRef = r->flRiderSpeed;

	/* He starts a whole bike-speed along the launch. */
	for (i = 0; i < 3; i++)
		r->riderPosition[i] = r->position[i]
	                          + r->crashLaunchDir[i] * r->speed;

	pick = 0.0f;
	if (r->flLeanTarget != 0.0f)
		pick = r->flLeanTarget;
	else if (r->lean != 0.0f)
		pick = r->lean;

	if (pick != 0.0f) {
		r->crashFallSide = (pick < 0.0f) ? -CRASH_FALL_SIDE
		                                 :  CRASH_FALL_SIDE;
	} else {
		/*
		 * Game_RandNext at 0x00458ce0 is a bare wrapper around
		 * the CRT rand(), so this IS the original's generator.
		 * Bit 0 set falls LEFT.
		 */
		r->crashFallSide = (rand() & 1) ? -CRASH_FALL_SIDE
		                                :  CRASH_FALL_SIDE;
	}

	/*
	 * THE RIDER STARTS THE CRASH ALREADY ON HIS SIDE. 0x00467fb4
	 * keeps only matRiderRoot's YAW and lays a flat frame at that
	 * heading down by -pi/2 - see racer_lay_rider_root.
	 *
	 * matRiderRoot IS NOT EMPTY WHEN WE GET HERE, and that is the
	 * whole reason the yaw survives: race_draw_racers copies the
	 * bike's draw matrix into it on every frame the rider is still
	 * aboard, exactly as maybe_Racer_Draw does at 0x00460db7.
	 *
	 * SEEDING IT FROM matOrientation INSTEAD throws the rider
	 * standing bolt upright. It reads as the safe thing to do on
	 * the assumption that a fresh crash finds the matrix zeroed,
	 * and it does not.
	 */
	racer_lay_rider_root(r);

	r->f_30e1[0] = 0.0f;
	r->f_30e1[1] = r->crashFallSide;
	r->f_30e1[2] = r->crashFallSide;

	/*
	 * THE DUST AT THE CRASH POINT, 0x004681b3 - and it is the last
	 * thing the original does before returning. Six particles, a
	 * life of 15 or 31, spread through the BIKE'S frame.
	 */
	fx_burst(6, r->position, 0xf, 0x10u, r->matOrientation);
	return 1;
}


#define CRASH_RESET_RECOVER  200.0f   /* 0x43480000 at 0x00468331 */

/* ORIGIN  FUN_004681d0 @ 0x004681d0, the per-racer half */
int racer_reset_crash(MR_Racer *r, MR_Drivetrain *d, MR_Gearbox *gb,
                      const MR_BikeCarac *carac)
{
	if (!r)
		return 0;

	if (r->humanPlayer != 0) {
		/*
		 * THE HUMAN PATH. The bike is stopped outright rather than
		 * decayed - SetLongSpeed with a literal 0 pushed as the
		 * argument (PUSH EDI with EDI zeroed, at 0x004681f9) - and
		 * then the engine speed is rebuilt from whatever gear is
		 * selected so the two cannot disagree afterwards.
		 */
		r->collideSpeed = 0.0f;

		if (d) {
			/*
			 * A DOCUMENTED DIVERGENCE. The original stops the
			 * bike with SetLongSpeed(instance, 0.0), which
			 * zeroes only the FORWARD component and keeps the
			 * other two - so a bike sliding sideways when the
			 * crash ends keeps that drift. We zero the speed
			 * outright, because MR_Drivetrain does not carry
			 * velDir and inventing one here would be worse
			 * than recording the gap. [open]
			 */
			d->speed = 0.0f;        /* racer+0x124 */
			r->speed = 0.0f;

			/*
			 * ANY PATH THAT MOVES speed WITHOUT GOING THROUGH
			 * THE GEARBOX NEEDS THIS CALL AFTER IT, and the
			 * omission is invisible: the drivetrain rebuilds
			 * speed FROM gearboxRoadSpeed the next frame, so the
			 * crash speed comes straight back. It must land on
			 * the RACER - MR_Drivetrain is a mirror, reloaded
			 * from the racer each frame, so a zero written only
			 * there is discarded. Gearbox_SyncRoadSpeed is at
			 * 0x0046820f. racers-convergence.txt 9.66.2.
			 */
			if (carac) {
				physics_sync_road_speed(&r->gearboxRoadSpeed,
				                        r->speed,
				                        carac->realSpeedMax);
				d->gearboxRoadSpeed = r->gearboxRoadSpeed;
			}

			if (gb && carac) {
				r->engineSpeed =
				    physics_speed_to_engine(carac, gb->gear,
				                            &r->gearboxRoadSpeed,
				                            r->engineSpeed,
				                            gb->state != 0);
				d->engineSpeed      = r->engineSpeed;
				d->gearboxRoadSpeed = r->gearboxRoadSpeed;
			}
		}
	} else {
		/*
		 * THE AI PATH. No gearbox work - engineSpeed is simply
		 * zeroed - plus the 200.0 recovery counter that a wall hit
		 * also sets (racers.txt 9.26 step 3).
		 */
		if (d)
			d->engineSpeed = 0.0f;
		r->aiRoadSpeed = 0.0f;
		r->flImpactBlend = CRASH_RESET_RECOVER;
	}

	r->flCrashTimerMs = 0.0f;
	r->crashLatch    = 0;

	return 1;
}


#define GROUND_CLEARANCE       32   /* MOV EAX,0x20 at 0x00469427 */
#define GROUND_CLEARANCE_TIGHT  8   /* MOV EAX,0x8  at 0x00469437 */
#define LAND_SPRING_SCALE   -2.0f   /* 0x004d1028, a FLOAT        */
#define KMH_TO_MS       (1.0 / 3.6) /* 0x004d0f70, shared with the
                                     * crash decay gate           */
#define RIDER_BIT_LANDED     0x80   /* OR at 0x004694c4           */

/* ORIGIN  NONE - ours. FUN_0046d610 is the original's cross. */
static void vec3_cross(float out[3], const float a[3], const float b[3])
{
	float x = a[1] * b[2] - b[1] * a[2];
	float y = b[0] * a[2] - b[2] * a[0];
	float z = b[1] * a[0] - b[0] * a[1];

	out[0] = x;
	out[1] = y;
	out[2] = z;
}


/* ORIGIN  Vec3_Normalize @ 0x0040fcd0 */
static float vec3_normalize(float v[3])
{
	double len;

	len = sqrt((double)v[0] * v[0] + (double)v[1] * v[1]
	           + (double)v[2] * v[2]);
	if (len != 0.0) {
		v[0] = (float)(v[0] / len);
		v[1] = (float)(v[1] / len);
		v[2] = (float)(v[2] / len);
	}
	return (float)len;
}


/*
 * ------------------------------------------------------------------
 * maybe_Racer_UpdateGroundContact's HELPERS - not slices of it. The
 * function itself is imported WHOLE as racer_update_ground_contact,
 * far below, and these are pieces it calls. They predate that
 * import, which is why they carry two-address ORIGIN tags where
 * rule 15 would now want one.
 * ------------------------------------------------------------------
 */

/* ORIGIN  maybe_Racer_UpdateGroundContact @ 0x00468ca0, at 0x004691cf */
float racer_wall_impact_cos(const float wallNormal[3],
                            const float travel[3])
{
	if (!wallNormal || !travel)
		return 0.0f;

	return wallNormal[0] * travel[0] + wallNormal[1] * travel[1]
	     + wallNormal[2] * travel[2];
}
/* ORIGIN  maybe_Racer_UpdateGroundContact @ 0x00468ca0, at 0x00469688 */
int racer_frame_from_forward(float m[12], const float forward[3])
{
	static const float worldUp[3] = { 0.0f, 1.0f, 0.0f };
	float fwd[3];
	float lateral[3];
	float up[3];
	int   i;

	if (!m || !forward)
		return 0;

	for (i = 0; i < 3; i++)
		fwd[i] = forward[i];
	if (vec3_normalize(fwd) == 0.0f)
		return 0;               /* see racer.h - ours, not the original */

	/*
	 * LATERAL COMES FROM WORLD UP, which is why the terrain can never
	 * roll the bike - only pitch and yaw it.
	 */
	vec3_cross(lateral, worldUp, fwd);
	if (vec3_normalize(lateral) == 0.0f)
		return 0;               /* forward was parallel to up */

	vec3_cross(up, fwd, lateral);
	vec3_normalize(up);

	/* Rows in the order the original stores them. */
	for (i = 0; i < 3; i++) {
		m[0 + i] = lateral[i];
		m[3 + i] = up[i];
		m[6 + i] = fwd[i];
	}
	return 1;
}


/* ORIGIN  maybe_Racer_UpdateGroundContact @ 0x00468ca0, at 0x0046974b */
int racer_orient_airborne(MR_Racer *r, const float moveDir[3])
{
	if (!r || !moveDir)
		return 0;

	return racer_frame_from_forward(r->matOrientation, moveDir);
}


/* ORIGIN  maybe_Racer_UpdateGroundContact @ 0x00468ca0, at 0x00469582 */
int racer_orient_grounded(MR_Racer *r, const MR_Trk *t, int segment,
                          float front[3], float rear[3])
{
	float fwd[3];
	int   i;

	if (!r || !t || !front || !rear)
		return 0;

	/*
	 * SNAP BOTH CONTACTS TO THE ROAD, EACH IN ITS OWN SEGMENT, so
	 * the two wheels can sit on different pieces of it. That is
	 * what lets the frame pitch across a segment boundary instead
	 * of hitching there - sample both in `segment` and it hitches.
	 * racers.txt 9.40.4.
	 */
	{
		int segFront = trk_find_segment_for_point(t, segment, front,
		                                          NULL);
		int segRear  = trk_find_segment_for_point(t, segment, rear,
		                                          NULL);

		if (trk_segment_has_surface(t, segFront))
			front[1] = trk_ground_height_at(t, segFront, front);
		if (trk_segment_has_surface(t, segRear))
			rear[1]  = trk_ground_height_at(t, segRear, rear);
	}

	for (i = 0; i < 3; i++)
		fwd[i] = front[i] - rear[i];

	return racer_frame_from_forward(r->matOrientation, fwd);
}


/*
 * THE WALL-CRASH CONSTANTS ARE DOUBLES IN THE BINARY and the
 * comparisons are done at double width. Writing them as floats here
 * would round the thresholds before the compare - the constant-width
 * trap this project keeps hitting.
 *
 * WALL_INC_MID IS NOT LOADED AS A CONSTANT AT ALL. The original
 * compares the cosine's BIT PATTERN against 0x3f000000 with an
 * integer CMP, which for a positive float is the same as <= 0.5f.
 * Written as the float it means, with the idiom recorded rather
 * than reproduced.
 */
#define WALL_PUSHBACK      32.0     /* 0x004d1024 */
#define WALL_INC_HARD       0.98    /* 0x004d11d0 */
#define WALL_INC_MID        0.5     /* 0x3f000000, an int compare */
#define WALL_SPD_HARD  (100.0/3.6)  /* 0x004d11d8 */
#define WALL_SPD_MID    (80.0/3.6)  /* 0x004d11e0 */
#define WALL_SPD_LOW    (20.0/3.6)  /* 0x004d0f50 */
#define WALL_SCRAPE_KEEP    0.75    /* 0x004d0ff0 */
#define WALL_INC_FROM       1.0     /* 0x004d0e70 */
#define WALL_GLANCE_KEEP    0.9     /* 0x004d11b8, the crash decay's */
#define WALL_REBOUND_KEEP   0.5     /* 0x004d0f08 */
#define WALL_REBOUND_FLOOR (5.0/3.6)/* 0x004d11e8 and 0x3fb1c71c */
#define WALL_BACK_KEEP      0.3     /* 0x004d11f8 */
#define WALL_SPEED_MIN     (1.0/3.6)/* 0x004d0f70 and 0x3e8e38e4 */
#define WALL_SPEED_KEEP     0.8     /* 0x004d11c0 */
#define WALL_SPEED_FLOOR (2.0/3.6)  /* 0x004d10c0 and 0x3f0e38e4 -
                                     * the threshold and the value
                                     * are the same number          */
#define AI_WALL_SPEED_KEEP   0.5     /* 0x004d1018                  */
#define AI_IMPACT_RECOVER  200.0f    /* 0x004d101c                  */
#define AI_LEAN_GROW_ANIM    1.5     /* 0x004d1038                  */
#define AI_LEAN_GROW         1.2     /* 0x004d0fe0                  */
#define AI_LEAN_CLAMP        0.7853982f /* pi/4; 0x004d1218/0x12c8  */
#define AI_LEAN_LERP         0.5     /* 0x004d0f08                  */

/*
 * ORIGIN  maybe_Racer_UpdateLean @ 0x00469c60
 *
 * BOTH TICK PATHS REACH IT. maybe_Racer_TickAI calls UpdateLean at
 * 0x0046c61f as well as the player tick, so this cannot sit
 * anywhere only the player runs: an AI that never reaches it
 * amplifies a lean nothing has set and pins at the pi/4 clamp.
 * racers.txt 9.47.5.2.
 *
 * dbg MAY BE NULL and is diagnostics only - race.c copies it back
 * into the statics its logging and mr_place read. Nothing in the
 * lean itself depends on it.
 */
void racer_update_lean(MR_Racer *r, const MR_BikeCarac *c,
                      const MR_Trk *t, float driveSpeed, float dtMs,
                      int replayMode, MR_LeanDbg *dbg)
{
	MR_LeanDbg scratch;

	if (!r || !c || !t)
		return;
	if (!dbg) {
		scratch.yawOrient = 0.0f;
		scratch.yawFrame  = 0.0f;
		scratch.leanDelta = 0.0f;
		dbg = &scratch;
	}

	float front[3], rear[3], fwd[3], frame[12], travel[12];
	float leanImpact;
	int   i;

	/*
	 * THE IMPACT COUNTER FIRST, at the very top of the lean the
	 * way the original does it (0x00469c8e) - it is the only
	 * site that decrements flImpactBlend, so it has to run once
	 * a frame whatever the rest of this block decides. Calling
	 * it from inside the frame-rebuild branch below would let
	 * the counter stick whenever that branch is skipped.
	 */
	leanImpact = racer_lean_impact_decay(r, r->frameDelta);

	/*
	 * THE TRAVEL FRAME, then the contacts TAKEN IN IT.
	 *
	 * This is the step that makes the two yaws differ at all. The
	 * original does NOT take the contacts in matOrientation: it
	 * builds a frame from alpha*heading + (1-alpha)*moveDir and
	 * samples the wheels there, so matBikeRoot carries the
	 * direction of travel while matOrientation carries the body's
	 * heading. Their difference is the SLIP ANGLE.
	 *
	 * Sampling the contacts in matOrientation instead makes the
	 * rebuilt frame equal to matOrientation to three decimals and
	 * the delta identically zero - see racer.h on racer_lean_delta.
	 */
	{
		/*
		 * racer+0xb4, NOT THE CARAC. 0x00469cd1 divides the speed
		 * by the RACER's own realSpeedMax:
		 *
		 *     blend = 1.0 - speed / racer+0xb4 * 8.0
		 *
		 * For racer 1 the two are the same number - Racer_Create
		 * copies the carac's RealSpeedMax in - which is why the
		 * substitution went unnoticed for as long as only the
		 * player was ticked. AN OPPONENT'S COPY COMES FROM THE
		 * ComputerSpeed TABLE INSTEAD (75.0 against the carac's
		 * 80.0 on ile01), so reading the carac here made the
		 * travel-frame blend wrong for every AI and the lean with
		 * it. racers.txt section 4.
		 */
		float alpha = racer_lean_blend_weight(driveSpeed,
		                                      r->realSpeedMax,
		                                      leanImpact);

		memcpy(travel, r->matOrientation, sizeof travel);
		if (racer_travel_frame(travel, r, alpha)) {
			/*
			 * *** TWO WHOLE ARMS, AND PersoAnim PICKS ONE. ***
			 *
			 *     00469e17  TEST EDI,EDI          the carac's PersoAnim
			 *     00469e19  JZ   00469f3b         road bike -> the rebuild
			 *     00469e1f  ...                   dirt bike -> the swing only
			 *
			 * A ROAD BIKE REBUILDS BOTH CONTACTS FROM position AND SNAPS
			 * THEM TO THE GROUND. A DIRT BIKE DOES NEITHER - it uses the
			 * pair maybe_Racer_UpdateGroundContact already left in
			 * racer+0x1b4 and racer+0x338, and only swings the front one
			 * for a wheelie.
			 *
			 * RUNNING THE REBUILD AND THE SNAP FOR EVERY BIKE MAKES DIRT
			 * JUMPS INVISIBLE, and it survives a physics check: the camera
			 * follows racer+0x30 and flies correctly, but the BIKE IS
			 * DRAWN FROM THE CONTACTS (racerdraw_rebuild_contacts ->
			 * racerdraw_draw_position) and the snap pins those to the
			 * terrain for the whole flight. The camera rises and the bike
			 * stays down. racers-convergence.txt 9.46.13.
			 */
			if (c->persoAnim == 0) {
				/* ---- THE ROAD BIKE, 0x00469f3b -------------------- */
				racer_wheel_contacts_m(travel, r->position, c, rear,
				                       front);

				/*
				 * ON segmentStart, NOT segment - both
				 * Trk_FindSegmentForPoint calls are passed racer+0x31c1
				 * (0x0046a0xx), the same field UpdateGroundContact hands
				 * Trk_GroundHeightAt.
				 */
				{
					int sf = trk_find_segment_for_point(t,
					                                    r->segmentStart,
					                                    front, NULL);
					int sr = trk_find_segment_for_point(t,
					                                    r->segmentStart,
					                                    rear, NULL);

					if (trk_segment_has_surface(t, sf))
						front[1] = trk_ground_height_at(t, sf, front);
					if (trk_segment_has_surface(t, sr))
						rear[1] = trk_ground_height_at(t, sr, rear);
				}

				/*
				 * THE WHEELIE TIPS THE NOSE IN THE GEOMETRY, pivoting on
				 * the rear contact - 0x0046a343 - and it runs AFTER the
				 * snap, which would otherwise pull the raised front wheel
				 * back onto the road.
				 */
				racer_wheelie_swing_contact(front, rear, r->wheelieAngle);

				/*
				 * AND PUBLISH THE PAIR - 0x0046a4ec stores the swung front
				 * into racer+0x1b4 and the rear into racer+0x338. Every
				 * other consumer reads those, so leaving the swing local
				 * gives two sources of truth for one pair of points.
				 */
				for (i = 0; i < 3; i++) {
					r->frontWheelContact[i] = front[i];
					r->rearWheelContact[i]  = rear[i];
				}

				for (i = 0; i < 3; i++)
					fwd[i] = front[i] - rear[i];

				/*
				 * matBikeRoot FROM THE CROSS-PRODUCT FRAME on this arm -
				 * Vec3_Cross((0,1,0), fwd) then Vec3_Cross(fwd, right),
				 * with the translation row zeroed. The dirt arm below
				 * builds the same field a different way; the bike is
				 * drawn on it either way. racers.txt 9.62.
				 */
				memcpy(frame, travel, sizeof frame);
				if (racer_frame_from_forward(frame, fwd))
					memcpy(r->matBikeRoot, frame,
				           sizeof r->matBikeRoot);
			} else {
				/* ---- THE DIRT BIKE, 0x00469e1f -------------------- */
				/*
				 * THE CONTACTS ARE TAKEN AS THEY STAND. Nothing here
				 * rebuilds or snaps them - which is exactly what lets a
				 * dirt bike leave the ground and still be drawn where it
				 * is.
				 */
				for (i = 0; i < 3; i++) {
					front[i] = r->frontWheelContact[i];
					rear[i]  = r->rearWheelContact[i];
				}

				/*
				 * THE SWING, 0x00469ed8..0x00469f0d, and ONLY THE FRONT
				 * CONTACT IS STORED BACK - the rear is the pivot and the
				 * original never writes it on this arm.
				 */
				racer_wheelie_swing_contact(front, rear, r->wheelieAngle);
				for (i = 0; i < 3; i++)
					r->frontWheelContact[i] = front[i];

				/*
				 * AND matBikeRoot IS A TWO-POINT LookAt HERE, not the
				 * cross-product frame - 0x00469f2b calls
				 * maybe_Mat34_LookAt(rearContact, frontContact) and copies
				 * the result straight in.
				 */
				mat34_look_at(r->matBikeRoot, r->rearWheelContact,
				              r->frontWheelContact);

				for (i = 0; i < 3; i++)
					fwd[i] = front[i] - rear[i];
			}

			{
				/*
				 * THE LEAN YAW COMES FROM travel, NOT FROM frame,
				 * and the difference only shows in a wheelie.
				 *
				 * The original keeps them apart deliberately: it
				 * builds ONE LookAt from the blended travel
				 * direction, copies it to sample the wheel contacts
				 * in, swings the front contact for the wheelie and
				 * rebuilds matBikeRoot from the swung pair - then
				 * takes the lean's yaw from the ORIGINAL LookAt at
				 * 0x0046a962, which the swing never touched.
				 *
				 * frame is our rebuilt one and carries the wheelie
				 * pitch, 45 degrees at full stick.
				 * racer_matrix_yaw is atan2(m[2], m[8]), which is
				 * not a heading once the frame is pitched - so a
				 * slip angle differenced from it is meaningless and
				 * the lean runs away instead of decaying.
				 * racers-convergence.txt 9.18.40.
				 */
				dbg->yawOrient = racer_matrix_yaw(r->matOrientation);
				dbg->yawFrame  = racer_matrix_yaw(travel);
				/*
				 * TWO LEAN CURVES, PICKED BY PersoAnim, and both
				 * are live. UpdateLean loads it into EDI at
				 * 0x00469c75 - g_bikeCarac[racer+0x78], the
				 * record's FIRST int - and 0x0046a641 jumps past
				 * the whole arctangent block when it is zero.
				 *
				 *     PersoAnim == 0   the piecewise curve, store
				 *                      at 0x0046ac4f. THE ROAD
				 *                      BIKES.
				 *     PersoAnim != 0   the arctangent, store at
				 *                      0x0046a8ab. THE DIRT BIKES.
				 *
				 * PersoAnim IS NOT 0 ON EVERY SHIPPED BIKE - the
				 * KTM sets it - and assuming it is collapses these
				 * two branches onto one wrong curve. Both are
				 * hook-confirmed: on the KTM the atan store fires
				 * every frame and the piecewise one never does; on
				 * SPEED it is exactly the other way round.
				 * racers.txt 9.70.
				 */
				if (c->persoAnim) {
					dbg->leanDelta =
					    racer_lean_delta_live(dbg->yawOrient,
					                          dbg->yawFrame);
					/*
					 * THE ARCTANGENT NEEDS ALL THREE, and the
					 * gate is a plain AND at 0x0046a62b:
					 * humanPlayer, soloMode, PersoAnim. A
					 * PersoAnim bike failing either of the
					 * first two takes the speed-softened
					 * divide at 0x0046abb7 instead - hooked,
					 * that arm is where racers 2..5 land while
					 * racer 1 takes the atan.
					 */
					if (r->humanPlayer && r->soloMode)
						r->lean = racer_lean_atan(
					                  dbg->leanDelta,
					                  dtMs,
					                  driveSpeed);
					else
						r->lean = racer_lean_ai(
					                  dbg->leanDelta,
					                  driveSpeed);
				} else {
					/*
					 * racer+0xb4 HERE TOO. There is exactly
					 * one FDIV by [ESI+0xb4] in UpdateLean,
					 * at 0x00469cd1, and both this and the
					 * blend weight above model that same
					 * instruction - so both read the racer's
					 * copy, not the carac's.
					 */
					dbg->leanDelta =
					    racer_lean_delta(dbg->yawOrient,
					                     dbg->yawFrame,
					                     driveSpeed,
					                     r->realSpeedMax,
					                     leanImpact);

					/*
					 * A FOURTH GATE, AND A FOURTH FORMULA.
					 * 0x0046abee..0x0046ac15 guards the
					 * piecewise store with THREE more tests
					 * before it:
					 *
					 *     humanPlayer != 0
					 *     soloMode    != 0
					 *     g_dwReplayMode not 1 and not 2
					 *
					 * and anything failing them jumps to
					 * 0x0046acf3, which stores -delta and
					 * returns: NO dt, NO GAIN, NO CLAMP. So
					 * the rider RESPONSE - the thing
					 * racer_lean_response models - belongs to
					 * a LIVE human solo rider only. An AI, a
					 * replay and a demo all take the geometric
					 * lean straight, which is right for
					 * something played back rather than
					 * ridden.
					 *
					 * RUN racer_lean_response FOR AN AI AND
					 * ITS LEAN COMES OUT AT ~82 PER CENT OF
					 * MOTO'S, with the delta feeding it correct
					 * to two per cent. The input looks fine and
					 * the formula is the wrong one, so the
					 * search goes to the wrong side.
					 *
					 * g_dwReplayMode is 0x004e9834 - 0 live,
					 * 1 playback, 2 demo (replay.txt).
					 *
					 * replayMode CARRIES OUR OWN REPLAY IN
					 * ITS PLACE. race_replay_mode is not a
					 * transcription of that global, but
					 * playback drives the real physics
					 * through a human solo racer - so
					 * without this test the rider response
					 * runs where the original would take the
					 * geometric lean, and the bike leans
					 * differently on playback than it did
					 * when the inputs were recorded.
					 */
					if (r->humanPlayer && r->soloMode
					    && !replayMode)
					    r->lean = racer_lean_response(
					                  dbg->leanDelta,
					                  dtMs,
					                  r->leanMode);
					else
						r->lean = -dbg->leanDelta;
				}
			}
		}
	}
}

#define AI_CLEAR_STUNT       8.0f    /* 0x004d1020, also the sink k */
#define AI_CLEAR_NORMAL     32.0f    /* 0x004d1024                  */

/*
 * ORIGIN  maybe_Racer_TickAI @ 0x0046bf80
 *
 * THE AI IS PLACED, NOT SIMULATED. It calls none of the player's
 * physics - no drive, no steering, no cornering, no drivetrain.
 * racer_ai_follow_line moves the rider, speed is assigned from
 * engineSpeed outright, and matOrientation is REBUILT from two
 * ground samples every frame rather than accumulated.
 *
 * THE FRAME IS COMPLETE. Every call it makes is imported and wired
 * in - racer_ai_follow_line, racer_ai_is_corner (0x0046c7a0, the
 * corner query that gates the stunt and the ground sink),
 * racer_ai_lane_step (0x0046cd20, inside the line follower) and
 * racer_update_dash (0x00467930, the tail). racer_update_lean is
 * called here too, as the original does.
 *
 * WHAT REMAINS IS INSIDE THOSE, not here, and each carries its own
 * [open] - the tacho's 0x0048e180 in racer_update_dash being the
 * only one left in this chain.
 *
 * EVERY SEGMENT READ AND WRITE IS segmentStart, racer+0x31c1 - nine
 * sites in the original and not one touches +0x31bd. The player's
 * paths keep the two in step; the AI leaves `segment` alone.
 */
int racer_tick_ai(MR_Racer *r, const MR_BikeCarac *c, const MR_Trk *t,
                   MR_Drivetrain *d, const MR_AiEnv *env, float dtScaled)
{
	int crash;

	float entry[3];
	float hit[3];
	float rear[3];
	float front[3];
	float fwd[3];
	float groundY;
	double len;
	int   savedInput;
	int   persoAnim;
	int   nearQuery;
	int   stuntAir;
	int   airborne;
	int   sink;
	int   segF;
	int   segR;
	int   i;

	if (!r || !c || !t || !env)
		return RACER_CRASH_NONE;

	/*
	 * 1. THE ENTRY SNAPSHOT. tickInputWord is saved here and put
	 * back just before EmitWheelFx - see the swap at the tail.
	 */
	savedInput        = r->tickInputWord;
	persoAnim         = c->persoAnim;
	r->handSkeleton   = 0;
	r->riderStateBits = 0;
	racer_save_prev_frame(r, dtScaled);

	for (i = 0; i < 3; i++)
		entry[i] = r->position[i];

	/* 2. THE LINE FOLLOWER, 0x0046bfe1 - where the AI lives. */
	/*
	 * THE SCALED DELTA, NOT MILLISECONDS. The original hands
	 * Racer_AiFollowLine the same param_2 TickAI got - 3.3 at a
	 * 33 ms step - and every dt term in it is scaled that way: the
	 * throttle ramp, the engine bleed, gravity and the move.
	 * Passing ms made an opponent leave the grid at ten times the
	 * right speed. racer_update_lean below is the opposite case and
	 * genuinely wants ms.
	 */
	racer_ai_follow_line(r, c, t, env, dtScaled);

	r->springCompression = 0.0f;

	/*
	 * 3. THE WALL, 0x0046bffd. Far simpler than the player's: no
	 * severity ladder, no crash roll - half the engine speed and
	 * the scrape bit. An AI cannot be knocked off by a rail.
	 */
	if (trk_clip_move_against_walls(t, r->segmentStart, entry, r->position,
	                                hit, r->moveDir, r->wallNormal)) {
		for (i = 0; i < 3; i++)
			r->position[i] = hit[i];

		r->segmentStart = trk_find_segment_for_point(t, r->segmentStart,
		                                        r->position, NULL);
		if (d)
			d->engineSpeed = (float)((double)d->engineSpeed
		                             * AI_WALL_SPEED_KEEP);
		r->engineSpeed = (float)((double)r->engineSpeed
		                         * AI_WALL_SPEED_KEEP);
		r->flImpactBlend   = AI_IMPACT_RECOVER;
		r->riderStateBits |= 1;
	}

	/* 4. NO DRIVETRAIN. The speed IS the engine speed. */
	stuntAir = 0;
	r->speed = r->engineSpeed;

	/*
	 * 5. THE STUNT TEST, 0x0046c0ad - and the query is "am I in a
	 * corner", eight segments of look-ahead. 1 on a bend.
	 */
	nearQuery = racer_ai_is_corner(t, r->segmentStart, 8, env->raceDir);

	/*
	 * The original spells this with two gotos crossing between the
	 * persoAnim arms (0x0046c0c2 and 0x0046c0f3). Both arms test
	 * nearQuery the same way, so it reads as the two tests below;
	 * all the persoAnim arm adds is stuntAir.
	 */
	if (persoAnim != 0 && nearQuery == 0)
		stuntAir = 1;

	if (nearQuery != 0) {
		r->stuntOrOverspeed = 0;
	} else if ((char)rand() == 0
	           && r->realSpeedMax < (d ? d->engineSpeed
	                                   : r->engineSpeed)) {
		r->stuntOrOverspeed = 1;
	}

	if (!env->preRace)
		racer_update_wheelie(r, dtScaled);

	/*
	 * 6. AIRBORNE, 0x0046c12f. The same 8 and 32 the player uses,
	 * but chosen on the stunt flag rather than on axis0 - which is
	 * the cross-check that the pair really is two ride-height
	 * tolerances. ONLY A persoAnim RIDER CAN LEAVE THE GROUND.
	 */
	groundY  = trk_ground_height_at(t, r->segmentStart, r->position);
	airborne = stuntAir
	         ? (groundY + AI_CLEAR_STUNT  < r->position[1])
	         : (groundY + AI_CLEAR_NORMAL < r->position[1]);

	if (airborne && persoAnim != 0) {
		r->onGround = 0;
	} else {
		/*
		 * 7. THE LANDING, 0x0046c18d. Same -2.0 spring and same
		 * 0x80 bit as the player, plus something the player does
		 * NOT do: the heading is flattened into XZ, so an AI
		 * keeps its direction and loses all vertical travel in
		 * one step.
		 */
		if (r->onGroundPrev == 0) {
			r->springCompression =
			    (float)(((double)r->positionPrev[1]
			             - (double)groundY) * LAND_SPRING_SCALE);
			r->flImpactBlend = AI_IMPACT_RECOVER;

			r->moveDir[1] = 0.0f;
			len = sqrt((double)r->moveDir[0] * r->moveDir[0]
			           + (double)r->moveDir[2] * r->moveDir[2]);
			if (len != 0.0) {
				r->moveDir[0] = (float)(r->moveDir[0] / len);
				r->moveDir[1] = 0.0f;
				r->moveDir[2] = (float)(r->moveDir[2] / len);
			}
			r->riderStateBits |= RIDER_BIT_LANDED;
		}

		/*
		 * 8. THE GROUND SINK, 0x0046c211. A persoAnim rider the
		 * query says is clear is SUNK a random amount - FSUBR at
		 * 0x0046c24e, so it is ground MINUS the jitter:
		 *
		 *     n = (int)(engineSpeed / realSpeedMax * 8.0)
		 *     if (n) position.y = ground - rand() % n
		 *
		 * so it scales with how fast the bike is going - dirt
		 * roughness, and the reason a dirt AI never sits flat.
		 * racer+0xb4 is the AI own copy, not the carac; see
		 * racers.txt section 4.
		 */
		sink = 0;
		if (persoAnim != 0 && nearQuery == 0) {
			float k = (r->realSpeedMax != 0.0f)
			        ? (r->engineSpeed / r->realSpeedMax
			           * AI_CLEAR_STUNT)
			        : 0.0f;
			int   n = (int)k;

			if (n != 0)
				sink = rand() % n;
		}

		r->position[1] = groundY - (float)sink;
		r->segmentStart     = trk_find_segment_for_point(t, r->segmentStart,
		                                            r->position, NULL);
		r->onGround      = 1;
		r->flAirTime     = 0.0f;
		r->onGroundPair1 = 1;
		r->onGroundPair2 = 1;

		/*
		 * 9. THE ORIENTATION, 0x0046c2e1. The axle points - along
		 * row 2 only, no frame-height term - each sampled in its
		 * OWN segment, then crossed. The SAME construction the
		 * player uses (racers.txt 9.40); only the source of the
		 * two points differs.
		 */
		for (i = 0; i < 3; i++) {
			rear[i]  = r->position[i] + r->matOrientation[9 + i]
			           + r->matOrientation[6 + i]
			             * (c->axeDistance * K_WHEELBASE_REAR);
			front[i] = r->position[i] + r->matOrientation[9 + i]
			           + r->matOrientation[6 + i]
			             * (c->axeDistance * K_WHEELBASE_FRONT);
		}

		segF = trk_find_segment_for_point(t, r->segmentStart, front, NULL);
		segR = trk_find_segment_for_point(t, r->segmentStart, rear, NULL);
		if (trk_segment_has_surface(t, segF))
			front[1] = trk_ground_height_at(t, segF, front);
		if (trk_segment_has_surface(t, segR))
			rear[1]  = trk_ground_height_at(t, segR, rear);

		for (i = 0; i < 3; i++)
			fwd[i] = front[i] - rear[i];

		racer_frame_from_forward(r->matOrientation, fwd);

		/* ROW3 IS ZEROED - the AI matrix carries no translation. */
		r->matOrientation[9]  = 0.0f;
		r->matOrientation[10] = 0.0f;
		r->matOrientation[11] = 0.0f;
	}

	/*
	 * 10. THE STORED CONTACTS, 0x0046c529 - the chassis reference
	 * pair, the same arithmetic racer_wheel_contacts holds for the
	 * player. Reached on BOTH paths, airborne included.
	 */
	racer_wheel_contacts(r, c, r->rearWheelContact,
	                     r->frontWheelContact);

	/*
	 * 11. THE LEAN, 0x0046c61f. UpdateLean sets it and the AI step
	 * then amplifies it - "pick a direction, then commit to it
	 * hard", which is why an AI bike leans so much more decisively
	 * than the player's.
	 *
	 * NO DIAGNOSTICS. The three yaws are race.c's logging for racer
	 * 1; an opponent has nowhere to put them.
	 */
	racer_update_lean(r, c, t, r->speed, env->dtMs, 0, NULL);
	racer_ai_lean_step(r, persoAnim);

	/*
	 * 12. THE SHARED TAIL, 0x0046c73c. THE tickInputWord SWAP IS
	 * THE ODD DETAIL: the CURRENT value goes to EmitWheelFx and the
	 * ENTRY value is written back first, so the buttons the line
	 * follower synthesised exist for exactly one call. An AI bike
	 * throws dust as if its rider were on the gas because, as far
	 * as the effects are concerned, he always is.
	 */
	racer_spin_wheels(r, c, dtScaled);
	racer_update_suspension(r, c, dtScaled);
	racerdraw_build_shadow(r, t);

	{
		unsigned int synthesised = (unsigned int)r->tickInputWord;

		r->tickInputWord = savedInput;
		racer_emit_wheel_fx(r, synthesised, t, env->viewedSegment,
		                    env->viewedIndex, dtScaled);
	}

	/*
	 * AND THE RETURN VALUE MATTERS. racer_update_crash reports
	 * RACER_CRASH_DONE when the rider has finished his flight -
	 * either the timer went negative or he stopped sliding - and
	 * SOMETHING HAS TO ACT ON IT. Only race.c can: putting a
	 * racer back on the bike walks the whole field, which rule 13
	 * puts in race_reset_crashes.
	 *
	 * DISCARD IT AND AI RIDERS STAY DOWN FOR EVER. The timer goes
	 * on decrementing past zero, the latch never clears, and the
	 * bodies lie on the track a lap later, shovable like scenery.
	 * The player's arm in race_tick_physics tests this, so an AI
	 * arm that does not reads as a crash bug rather than as the
	 * plumbing it is.
	 */
	crash = racer_update_crash(r, t);
	/*
	 * maybe_Racer_UpdateSkeleton(r, 1) - racer_select_skeleton_slot
	 * holds its slot choice and the rest of it lives in racerdraw,
	 * driven from the draw path rather than from here.
	 */
	racer_update_dash(r, c, r->frameDelta);
	return crash;
}


/* ORIGIN  maybe_Racer_TickAI @ 0x0046bf80, at 0x0046c62a */
void racer_ai_lean_step(MR_Racer *r, int persoAnim)
{
	double v;

	if (!r)
		return;

	if (!persoAnim && r->flImpactBlend != 0.0f) {
		/*
		 * A wall hit snaps an ordinary AI upright - and note this
		 * arm skips the clamp entirely, jumping straight to the
		 * lerp at 0x0046c723.
		 */
		r->lean = 0.0f;
	} else {
		v = (double)r->lean * (persoAnim ? AI_LEAN_GROW_ANIM
		                                 : AI_LEAN_GROW);

		/* MULTIPLIERS ABOVE 1 - this drives outward, not to zero. */
		r->lean = (float)v;
		if (r->lean < -AI_LEAN_CLAMP)
			r->lean = -AI_LEAN_CLAMP;
		if (r->lean > AI_LEAN_CLAMP)
			r->lean = AI_LEAN_CLAMP;
	}

	/* The lerp runs on EVERY branch, including the upright one. */
	r->lean = (float)((double)r->flLeanPrev
	                  + ((double)r->lean - (double)r->flLeanPrev)
	                    * AI_LEAN_LERP);
	r->flLeanPrev = r->lean;
}


#define AI_LANE_SPEED       0.025    /* 0x004d12b8, 2.5% a lane     */
#define AI_THROTTLE_RAMP  (1.0/6.0)  /* 0x004d12b0                  */
#define AI_BLEND_SCALE      0.005f   /* 0x004d100c, = 1/200         */
#define AI_BLEND_FLOOR      0.01f    /* 0x004d0ed8                  */
#define AI_ALIGN_FLOOR      0.5f     /* 0x3f000000, an int compare  */
#define AI_STEP_SCALE       1.28     /* 0x004d1010, = 128 * 0.01    */

#define LANE_HALF          120.0f   /* 0x004d1320, half a lane      */
#define LANE_OUTER         600.0f   /* 0x004d1324, past lane 2      */
#define LANE_SPAN            720    /* 0x2d0, three lanes of 0xf0   */
#define LANE_STRIDE          240    /* 0xf0 - the same 240          */
                                    /* racer_ai_lane_step ramps to  */

/*
 * ORIGIN  maybe_Trk_LaneOfRacer @ 0x0046d340
 *
 * WHICH LANE IS THIS RACER STANDING IN - measured off its position,
 * not chosen. maybe_Racer_Tick calls it at 0x0045d7f2 and stores the
 * answer in racer+0x31d9.
 *
 * THAT IS THE ASYMMETRY WORTH KNOWING: an AI PICKS its lane and
 * drives to it, while the PLAYER's lane is read back from where the
 * bike actually is. The AI's blocking and overtaking logic then
 * treats both the same way, which is how a human ends up being
 * overtaken round the outside.
 *
 *     d  = dot(row0, position - origin) - dot(row0, row3)
 *     for lane 0,1,2 with off = 0, 240, 480:
 *         base = off + segment +0xb0
 *         if (base - 120 < d && d < base + 120) return lane
 *     d < (+0xb0) - 120   -> 0
 *     (+0xb0) + 600 < d   -> 2
 *     otherwise           -> -1
 *
 * SO THE LANES ARE 240 WIDE AND THE TEST IS +-120 ABOUT EACH
 * CENTRE, which is the same 240 racer_ai_lane_step ramps chargeMeter
 * by - the two agree on the geometry from opposite ends.
 *
 * IT CAN RETURN -1, for a bike in a gap the three bands do not
 * cover, and the original stores that in the lane field without
 * comment. Anything reading the lane has to tolerate it.
 */
int racer_lane_of_position(const MR_Racer *r, const MR_Trk *t)
{
	const MR_TrkSegment *sg;
	double d;
	float  lineOff;
	int    off;
	int    lane;

	if (!r || !t || r->segmentStart < 0
	    || r->segmentStart >= t->segmentCount)
	    return 0;

	sg = &t->segments[r->segmentStart];

	d = (double)sg->matrix[0] * (r->position[0] - sg->pos[0])
	  + (double)sg->matrix[1] * (r->position[1] - sg->pos[1])
	  + (double)sg->matrix[2] * (r->position[2] - sg->pos[2])
	  - ((double)sg->matrix[9]  * sg->matrix[0]
	     + (double)sg->matrix[10] * sg->matrix[1]
	     + (double)sg->matrix[11] * sg->matrix[2]);

	lineOff = sg->lineOffset;

	for (lane = 0, off = 0; off < LANE_SPAN;
	     off += LANE_STRIDE, lane++) {
		double base = (double)off + (double)lineOff;

		/*
		 * THE BOUNDS ARE TRUNCATED TO INT before the compare, at
		 * 0x0046d3ce and 0x0046d3db, so a bike within a unit of a
		 * band edge is decided by the truncation. Kept.
		 */
		if ((double)(int)(base - LANE_HALF) < d
		    && d < (double)(int)(base + LANE_HALF))
		    return lane;
	}

	if (d < (double)(int)((double)lineOff - LANE_HALF))
		return 0;
	if ((double)(int)((double)lineOff + LANE_OUTER) < d)
		return 2;
	return -1;
}


#define AI_CORNER_DOT      0.995   /* 0x004d1300                  */
#define DASH_KMH           0.028125 /* 0x004d1160                  */
#define DASH_RATE_GAIN     100.0    /* 0x004d1168                  */
#define DASH_RATE_DAMP       0.5    /* 0x004d0f08                  */
#define DASH_RATE_HI        40.0f   /* 0x42200000                  */
#define DASH_RATE_LO      (-50.0f)  /* 0xc2480000                  */
#define DASH_DT_SCALE       0.01    /* 0x004d0ed8                  */
#define DASH_TACHO_IDLE      800    /* 0x004d0ee8                  */
#define DASH_KMH_TO_MS  (1.0/3.6)   /* 0x004d0f70, exact           */

/*
 * ORIGIN  FUN_0046c7a0 @ 0x0046c7a0
 *
 * IS THE TRACK BENDING between here and `ahead` segments on? It
 * takes two chords from this segment's origin - one to the far
 * segment, one to the half-way segment - flattens both into XZ,
 * normalises them, and compares:
 *
 *     |dot(halfChord, fullChord)| > 0.995   ->  0, a straight
 *     otherwise                             ->  1, a corner
 *
 * Both look-aheads are multiplied by g_nRaceDirection, so it reads
 * the way the race is actually run.
 *
 * RETURNS 1 FOR A CORNER, and that is worth stating because the
 * caller uses it to SUPPRESS things: maybe_Racer_TickAI clears
 * stuntOrOverspeed on a corner and only rolls for a stunt, or
 * applies the PersoAnim ground sink, on a straight. AI riders do
 * not pull wheelies through bends.
 */
int racer_ai_is_corner(const MR_Trk *t, int segment, int ahead,
                       int raceDir)
{
	const MR_TrkSegment *here;
	float full[2];
	float half[2];
	float len;
	int   segFar;
	int   segMid;

	if (!t || t->segmentCount == 0 || segment < 0
	    || segment >= t->segmentCount)
	    return 0;

	segFar = trk_wrap_segment_index(t, ahead * raceDir + segment);
	segMid = trk_wrap_segment_index(t, (ahead * raceDir) / 2 + segment);
	here   = &t->segments[segment];

	full[0] = t->segments[segFar].pos[0] - here->pos[0];
	full[1] = t->segments[segFar].pos[2] - here->pos[2];
	half[0] = t->segments[segMid].pos[0] - here->pos[0];
	half[1] = t->segments[segMid].pos[2] - here->pos[2];

	len = (float)sqrt((double)full[0] * full[0]
	                  + (double)full[1] * full[1]);
	if (len != 0.0f) {
		full[0] = (float)(full[0] / len);
		full[1] = (float)(full[1] / len);
	}
	len = (float)sqrt((double)half[0] * half[0]
	                  + (double)half[1] * half[1]);
	if (len != 0.0f) {
		half[0] = (float)(half[0] / len);
		half[1] = (float)(half[1] / len);
	}

	len = half[0] * full[0] + half[1] * full[1];
	if (len < 0.0f)
		len = -len;
	return (len > (float)AI_CORNER_DOT) ? 0 : 1;
}


/*
 * ORIGIN  FUN_00467930 @ 0x00467930
 *
 * THE DASHBOARD - and the speedometer is measured, not reported.
 *
 *     d  = rearWheelContact - rearWheelContactPrev
 *     speedKmhRaw = (int)( |d| / (g_frameDelta * 0.01) * 0.028125 )
 *
 * SO THE NEEDLE READS HOW FAR THE REAR TYRE ACTUALLY WENT, not
 * engineSpeed. A bike being dragged sideways, or one whose wheel is
 * spinning, shows what it is doing over the ground.
 *
 * AN AI's NEEDLE IS DAMPED AND THE PLAYER's IS NOT:
 *
 *     rate = (rate + (needle - prev)/g_frameDelta * 100) * 0.5
 *     rate clamped to [-50, +40]        0x42200000, 0xc2480000
 *     needle = prev + (int)(rate * g_frameDelta * 0.01)
 *     needle floored at 0
 *
 * which is what racer.h has always said about +0x120 without saying
 * who wrote it.  ONLY THE AI PATH DOES - racer+0x8 non-zero returns
 * early through the tacho - so this is the fourth field in this
 * subsystem that read as dead because every capture watched racer 1.
 *
 * THE HUMAN SOLO ARM RETURNS EARLY with just the tacho:
 *
 *     tachoRpm = (int)(RegimeMax / MoteurVitesseMax * engineSpeed)
 *                carac 29 over carac 6
 *
 * THE FULL SCALE IS RegimeMax, NEVER A CONSTANT. It is 10000 on this
 * bike, so a hardcoded 12000 reads a plausible 1.2x high everywhere
 * and still pins exactly at a gear top. racers-convergence.txt 9.46.17.
 *
 * ONE HOLE, at 0x00467aed. The final tacho multiplies RegimeMax by
 * whatever maybe_Gearbox_AutoSelect (0x0048e180) left on the FPU
 * stack, and neither that function nor the value is read yet. The
 * idle floor of 800 below it IS read. [open - 0x0048e180]
 */
void racer_update_dash(MR_Racer *r, const MR_BikeCarac *c,
                       float frameDelta)
{
	double d[3];
	double len;
	int    prev;
	int    i;

	if (!r || !c || frameDelta == 0.0f)
		return;

	for (i = 0; i < 3; i++)
		d[i] = (double)r->rearWheelContact[i]
	         - (double)r->rearWheelContactPrev[i];

	len  = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
	prev = r->speedKmhNeedle;

	r->speedKmhRaw    = (int)(len / ((double)frameDelta * DASH_DT_SCALE)
	                          * DASH_KMH);
	r->speedKmhNeedle = r->speedKmhRaw;

	if (r->humanPlayer == 0) {
		double rate = ((double)(r->speedKmhNeedle - prev)
		               / (double)frameDelta * DASH_RATE_GAIN
		               + (double)r->needleRate) * DASH_RATE_DAMP;

		/*
		 * THE CLAMP IS ON THE BITS, NOT THE VALUE, and that is not
		 * a detail. 0x004679ee..0x00467a18:
		 *
		 *     if ((int)bits < 0x42200001)          <= +40.0f
		 *         if ((uint)bits > 0xc2480000)     < -50.0f
		 *             bits = 0xc2480000
		 *     else
		 *         bits = 0x42200000
		 *
		 * WRITTEN AS A VALUE COMPARE IT LETS NaN THROUGH. NaN fails
		 * both `>` and `<`, so it survives the clamp, and the next
		 * frame's rate = (... + NaN) * 0.5 is NaN again - the
		 * accumulator is damped, so once poisoned it never
		 * recovers. Measured: two of eleven opponents sat at NaN
		 * with every input to them finite.
		 *
		 * THE BIT COMPARE CATCHES IT FOR FREE. A positive NaN is
		 * 0x7Fxxxxxx, larger than 0x42200001, so it clamps to
		 * +40.0f; a negative one is 0xFFxxxxxx, which is a negative
		 * int and then above 0xc2480000 unsigned, so it clamps to
		 * -50.0f. That is the original's behaviour and it is the
		 * reason to transcribe the idiom rather than tidy it.
		 */
		{
			float        v = (float)rate;
			unsigned int bits;

			memcpy(&bits, &v, sizeof bits);
			if ((int)bits < 0x42200001) {
				if (bits > 0xc2480000u)
					bits = 0xc2480000u;
			} else {
				bits = 0x42200000u;
			}
			memcpy(&v, &bits, sizeof v);
			r->needleRate = v;
			rate = (double)v;
		}

		r->speedKmhNeedle = (int)(rate * (double)frameDelta
		                          * DASH_DT_SCALE) + prev;
		if (r->speedKmhNeedle < 0)
			r->speedKmhNeedle = 0;
	} else if (r->soloMode != 0) {
		if (c->moteurVitesseMax != 0.0f)
			r->tachoRpm = (int)((double)c->regimeMax
		                        / (double)c->moteurVitesseMax
		                        * (double)r->engineSpeed);
		return;
	}

	/*
	 * THE GEARBOX IS ASKED WITH THE NEEDLE, NOT THE ENGINE, and the
	 * original does it by staging the value through racer+0xec:
	 * save it, write needle * (1/3.6), call, put it back. SO THE
	 * AUTO SHIFT IS DECIDED ON WHAT THE BIKE IS DOING OVER THE
	 * GROUND rather than on the drivetrain's own number - and a dump
	 * taken between frames shows the restored value, which is what
	 * made racer+0xec read ambiguously.
	 *
	 * OURS PASSES THE SPEED instead of staging it, because nothing
	 * here needs the side effect. THE NEGATIVE CLAMP IS KEPT: the
	 * original zeroes the temporary when it comes out negative
	 * (0x00467ac2), and reverse gear can make it so.
	 */
	{
		float ground = (float)((double)r->speedKmhNeedle
		                       * DASH_KMH_TO_MS);
		float rev;

		if (ground < 0.0f)
			ground = 0.0f;
		rev = gearbox_auto_select(c, ground, &r->gear);
		r->tachoRpm = (int)((double)rev * (double)c->regimeMax);
	}

	if (r->tachoRpm < DASH_TACHO_IDLE)
		r->tachoRpm = DASH_TACHO_IDLE;
}


#define AI_LANE_WIDTH      240.0f   /* lane * 0xf0, the target     */
#define AI_LANE_RAMP     1.066667   /* 0x004d1308, units a step     */
#define AI_KMH_TO_MS    (1.0/3.6)   /* 0x004d0f70, exact            */
#define AI_YIELD_LERP      0.03125  /* 0x004d1158                   */
#define AI_YIELD_TRIM        0.95   /* 0x004d1310                   */
#define AI_BRAKE_SHOW   11.111111   /* 0x004d1318, m/s of change    */
#define AI_LANE_MAX             3   /* lanes are 0, 1 and 2         */
#define AI_LANE_CLEAR           7   /* segments the blocker must be */
                                    /* ahead before a lane change   */

/*
 * ORIGIN  FUN_0046c970 @ 0x0046c970
 *
 * Is seg inside the wrapped window [mine - behind, mine + ahead)?
 * Both bounds go through Trk_WrapSegmentIndex, so the test has two
 * arms: the ordinary one, and the one where the window straddles the
 * start/finish line and the range is inverted.
 */
static int ai_seg_in_window(const MR_Trk *t, int seg, int mine,
                            int ahead, int behind)
{
	int lo;
	int hi;

	if (!t || t->segmentCount == 0)
		return 0;

	lo = trk_wrap_segment_index(t, mine - behind);
	hi = trk_wrap_segment_index(t, ahead + mine);

	if (hi < lo)
		return (seg >= lo && seg < t->segmentCount)
	        || (seg >= 0 && seg < hi);
	return seg >= lo && seg < hi;
}


/*
 * ORIGIN  FUN_0046c9f0 @ 0x0046c9f0
 *
 * Segments from mine forward to theirs, wrapping once. Always
 * positive, so a rider just behind the line reads as nearly a whole
 * lap ahead rather than as a negative.
 */
static int ai_seg_ahead(const MR_Trk *t, int mine, int theirs)
{
	if (mine <= theirs)
		return theirs - mine;
	return (theirs - mine) + (t ? t->segmentCount : 0);
}


/*
 * ORIGIN  FUN_0046ca10 @ 0x0046ca10
 *
 * The NEAREST other racer sitting in `lane` inside the window, by
 * slot, or -1. Skips this racer by slotIndex rather than by pointer.
 */
static int ai_nearest_in_lane(const MR_Racer *racers, int count,
                              const MR_Racer *r, const MR_Trk *t,
                              int lane, int ahead, int behind)
{
	int best = 0x7ffffffe;
	int found = -1;
	int i;

	if (!racers || !r || !t)
		return -1;

	for (i = 1; i < count; i++) {
		const MR_Racer *o = &racers[i];
		int             d;

		if (r->slotIndex == i || o->lane != lane)
			continue;
		if (!ai_seg_in_window(t, o->segmentStart, r->segmentStart,
		                      ahead, behind))
		    continue;

		d = ai_seg_ahead(t, r->segmentStart, o->segmentStart);
		if (d < best) {
			best  = d;
			found = i;
		}
	}
	return found;
}


/*
 * ORIGIN  FUN_0046cb40 @ 0x0046cb40
 *
 * Walk lanes 0..2, take the nearest occupant of each, and return the
 * lane whose occupant is the FASTEST - writing that racer's slot back
 * through blockerOut. So when every lane is blocked the AI moves to
 * the one it will lose least time behind, rather than the emptiest.
 */
static int ai_fastest_lane(const MR_Racer *racers, int count,
                           const MR_Racer *r, const MR_Trk *t,
                           int ahead, int behind, int *blockerOut)
{
	float bestSpeed = 0.0f;
	int   bestLane = -1;
	int   lane;

	for (lane = 0; lane < AI_LANE_MAX; lane++) {
		/*
		 * NOT `near`. windef.h defines it away to nothing, and the
		 * error it produces points at the line after.
		 */
		int   slot = ai_nearest_in_lane(racers, count, r, t, lane,
		                                ahead, behind);
		float sp;

		if (slot < 0)
			continue;
		sp = (float)((double)racers[slot].speedKmhRaw * AI_KMH_TO_MS);
		if (sp > bestSpeed) {
			bestSpeed = sp;
			bestLane  = lane;
			if (blockerOut)
				*blockerOut = slot;
		}
	}
	return bestLane;
}


/*
 * ORIGIN  FUN_0046cd20 @ 0x0046cd20
 *
 * THE LANE LOGIC, AND chargeMeter IS ITS OUTPUT rather than its
 * subject - the name describes the field this writes, not the
 * decision above it. That output is the rider's lateral offset from
 * the racing line, ramped toward lane * 240 at 1.0667 units a step,
 * and the line follower adds it to the segment's own offset to get
 * the point it aims at. Three lanes, 240 units apart, is what
 * spreads a field across the road.
 *
 * THE DECISION ABOVE IT:
 *
 *   1. IF THE INSIDE IS CLEAR, TAKE IT. lane > 0 and nobody in
 *      lane-1 within a half window - the AI drifts back toward lane
 *      0 whenever it can, so the field collapses to the racing line
 *      when it is not fighting.
 *   2. Otherwise find the nearest rider in MY lane inside the
 *      window. None, and there is nothing to do.
 *   3. ONLY ACT IF I AM FASTER THAN HIM. The sign convention is
 *      INVERTED from the usual - the comparison yields -1 when OURS
 *      is the greater - and reading it the natural way makes the AI
 *      try to overtake only the bikes it cannot catch.
 *   4. Take the first completely empty lane within one of mine.
 *   5. Failing that, ask ai_fastest_lane, check the blocker is at
 *      least 7 segments ahead, and move.
 *   6. STILL STUCK: slow to his speed. engineSpeed lerps 1/32 of the
 *      way, is trimmed by 0.95, and if the change exceeds 11.1 m/s
 *      tickInputWord becomes 2 - THE BRAKE - so an AI closing on a
 *      slower bike throws brake dust like a rider would.
 *
 * THE WINDOW SIZES ARE SWAPPED BY THE REVERSE FLAG, 0x004dc144 -
 * the race's own latched copy of it.  Forward: ahead 12 and
 * behind 0 for the same-lane test, 6 and 4 for the inside
 * check, 12 and 12 for the empty-lane sweep.  Reverse swaps
 * ahead and behind throughout, because a higher segment index
 * is then the way the bike came from.
 */
void racer_ai_lane_step(MR_Racer *racers, int count, MR_Racer *r,
                        const MR_Trk *t, int raceDir, int mirrorWindow,
                        float dtScaled)
{
	int   ahead;
	int   behind;
	int   sideAhead;
	int   sideBehind;
	int   blocker;
	int   lane;
	float target;
	float mySpeed;

	if (!racers || !r || !t || count < 2)
		return;

	if (!mirrorWindow) {
		sideAhead = 12; sideBehind = 8;
		ahead = 12; behind = 0;
	} else {
		sideAhead = 8; sideBehind = 12;
		ahead = 0; behind = 12;
	}

	/* 1. THE INSIDE, 0x0046cd60 - half the window either way. */
	if (r->lane > 0) {
		if (ai_nearest_in_lane(racers, count, r, t, r->lane - 1,
		                       sideAhead / 2, sideBehind / 2) < 0) {
			r->lane -= 1;
			goto ramp;
		}
	}

	/* 2. THE BIKE IN FRONT, 0x0046cde8. */
	blocker = ai_nearest_in_lane(racers, count, r, t, r->lane,
	                             ahead, behind);
	if (blocker < 0)
		goto ramp;

	/*
	 * 3. AM I FASTER? The original's comparison returns -1 when ours
	 * is the greater and it acts only on -1, so this really is
	 * "faster than him" however backwards it reads.
	 */
	mySpeed = (float)((double)r->speedKmhRaw * AI_KMH_TO_MS);
	if (mySpeed <= (float)((double)racers[blocker].speedKmhRaw
	                       * AI_KMH_TO_MS))
	    goto ramp;

	/* 4. AN EMPTY LANE, 0x0046cf0e - first of 0, 1, 2 with nobody. */
	for (lane = 0; lane < AI_LANE_MAX; lane++) {
		if (ai_nearest_in_lane(racers, count, r, t, lane, 12, 12) < 0)
			break;
	}
	if (lane >= AI_LANE_MAX)
		lane = -1;

	/* AND ONLY IF IT IS NEXT DOOR - |lane - mine| < 2. */
	if (lane >= 0) {
		int d = lane - r->lane;

		if (d < 0)
			d = -d;
		if (d < 2) {
			r->lane = lane;
			goto ramp;
		}
	}

	/* 5. THE LEAST BAD LANE, 0x0046cfa8. */
	{
		int want = ai_fastest_lane(racers, count, r, t, ahead, behind,
		                           &blocker);
		int d    = want - r->lane;

		if (d < 0)
			d = -d;

		if (want >= 0 && want != r->lane && d < 2) {
			int gap = racers[blocker].segmentStart - r->segmentStart;

			if (gap < 0)
				gap += t->segmentCount;
			if (gap * raceDir >= AI_LANE_CLEAR)
				r->lane = want;
		}
	}

	/*
	 * 6. STILL BEHIND HIM - MATCH HIS SPEED AND SHOW THE BRAKE.
	 * 0x0046d0f5.
	 */
	{
		float his = (float)((double)racers[blocker].speedKmhRaw
		                    * AI_KMH_TO_MS);

		if (mySpeed > his) {
			float was = r->engineSpeed;

			r->engineSpeed = (float)((double)r->engineSpeed
			                         + ((double)his
			                            - (double)r->engineSpeed)
			                           * AI_YIELD_LERP);
			r->engineSpeed = (float)((double)r->engineSpeed
			                         * AI_YIELD_TRIM);
			r->aiRoadSpeed = r->engineSpeed;

			if ((double)(was - r->engineSpeed) > AI_BRAKE_SHOW
			    || (double)(r->engineSpeed - was) > AI_BRAKE_SHOW)
			    r->tickInputWord = 2;
		}
	}

ramp:
	/*
	 * 7. THE OFFSET ITSELF, 0x0046d1ad - and only on the ground, so
	 * a jumping bike holds the line it left with.
	 */
	if (r->onGround == 0)
		return;

	target = (float)r->lane * AI_LANE_WIDTH;
	if (r->chargeMeter < target) {
		r->chargeMeter = (float)((double)r->chargeMeter
		                         + (double)dtScaled * AI_LANE_RAMP);
		if (r->chargeMeter > target)
			r->chargeMeter = target;
	} else if (r->chargeMeter > target) {
		r->chargeMeter = (float)((double)r->chargeMeter
		                         - (double)dtScaled * AI_LANE_RAMP);
		if (r->chargeMeter < target)
			r->chargeMeter = target;
	}
}


/*
 * ORIGIN  Racer_AiFollowLine @ 0x0046b120, at 0x0046b1e8 and 0x0046b47a
 *
 * THE POINT ON THE RACING LINE THIS RIDER IS AIMING AT.
 *
 *     offset = segment[seg] +0xb0 + chargeMeter
 *     target = segment.origin + transform((offset, 0, 0), matrix)
 *
 * Row 0 of a segment's matrix is its LATERAL axis, so the offset
 * moves the point ACROSS the road: the segment field is the line's
 * own displacement and chargeMeter is this rider's on top of it. A
 * field of AI with different chargeMeter values drives a fan of
 * parallel lines, which is how they spread out.
 *
 * +0xb0 IS lineOffset IN OUR LAYOUT - the dword the file stores after
 * the four planes, read as a float.
 *
 * IT IS NOT planes[3][0] AT +0x70, AND THAT MISTAKE HIDES: +0x70 is
 * wallPlane1's normal x, a flat -1.0 on ile01, so a follower reading
 * the offset there tracks the centreline give or take a unit and
 * looks roughly right. See trk.h.
 */
void racer_ai_target_point(const MR_Trk *t, int seg, float chargeMeter,
                           float out[3])
{
	const MR_TrkSegment *sg;
	float                offset;
	int                  i;

	if (!t || !out || seg < 0 || seg >= t->segmentCount)
		return;

	sg     = &t->segments[seg];
	offset = sg->lineOffset + chargeMeter;

	for (i = 0; i < 3; i++)
		out[i] = sg->pos[i] + sg->matrix[i] * offset
	             + sg->matrix[9 + i];
}


/*
 * ORIGIN  Racer_AiFollowLine @ 0x0046b120, at 0x0046b2b0
 *
 * Fold an angle into [0, 2pi) with the original's two while loops.
 * The follow line wraps each yaw SEPARATELY before comparing them,
 * and only then brings the second within pi of the first - wrapping
 * the difference instead agrees almost always and disagrees across
 * the seam, which is where an AI would visibly snap.
 */
float racer_ai_wrap_turn(float a)
{
	while (a < 0.0f)
		a = (float)((double)a + MR_TWO_PI);
	while ((double)a >= MR_TWO_PI)
		a = (float)((double)a - MR_TWO_PI);
	return a;
}


#define AI_STEER_DELTA      0.25f    /* 0x004d0f88                  */
#define AI_STEER_GAIN       12.0f    /* 0x004d1148, PersoAnim only  */
#define AI_STEER_TO_UNITS 1303.797294 /* 0x004d12d8, = 4096/pi -    */
                                     /* 8192 units to the turn      */
#define AI_ENGINE_DECAY  0.0555556   /* 0x004d12e0 -, 0x004d12e8 +  */
#define AI_AIR_GRAVITY    0.245250   /* 0x004d12f8                  */

/*
 * ORIGIN  Racer_AiFollowLine @ 0x0046b120
 *
 * THE ONLY PLACE THE AI MAKES A DECISION. Everything else in
 * racer_tick_ai reacts to what this did.
 *
 * IT DOES NOT EMULATE A DRIVER. No throttle, no brake, no steering
 * angle - it never touches the player's inputs. Each step it picks a
 * point on the racing line ahead, turns the bike toward it, blends
 * its speed by how well aligned it already is, and integrates.
 *
 * THE ONE INPUT IT SYNTHESISES is tickInputWord = 1 at the very top,
 * which is "accelerate held". racer_tick_ai owns what happens to it
 * after that.
 *
 * THE LANE LOGIC IS racer_ai_lane_step (0x0046cd20), called below.
 */
void racer_ai_follow_line(MR_Racer *r, const MR_BikeCarac *c,
                          const MR_Trk *t, const MR_AiEnv *env,
                          float dtScaled)
{
	float aim[3];
	float target[3];
	float toTarget[3];
	float look[12];
	float inv[12];
	float euler[3];
	float yawBike;
	float yawAim;
	float delta;
	float blend;
	float len;
	int   persoAnim;
	int   dir;
	int   farAhead;
	int   nearAhead;
	int   seg;
	int   i;

	if (!r || !c || !t || !env || t->segmentCount <= 0)
		return;

	persoAnim = c->persoAnim;

	/*
	 * THE LOOK-AHEAD, 0x0046b1d9..0x0046b1f9, AND IT IS NOT ZERO.
	 *
	 *     dir = g_nRaceDirection (0x004dc140), +1 or -1
	 *     PersoAnim   far = dir * 9    near = dir * 2
	 *     otherwise   far = dir * 16   near = dir * 4
	 *
	 * added to segmentStart before the wrap at 0x0046b202 and
	 * 0x0046b5e1.  A DIRT BIKE LOOKS HALF AS FAR AHEAD AS A ROAD
	 * BIKE, which is what lets it take a tighter line.
	 *
	 * AIMING AT THE CURRENT SEGMENT INSTEAD SENDS THE BIKE
	 * BACKWARDS. The target lands on the point the rider is
	 * standing on, the bearing to it is meaningless, and the
	 * backwards guard then flips moveDir and engineSpeed every
	 * frame - the racer walks down the segment numbers at a
	 * negative engine speed.
	 */
	dir       = env->raceDir;
	farAhead  = persoAnim ? (dir * 9) : (dir * 16);
	nearAhead = persoAnim ? (dir * 2) : (dir * 4);

	/* 1. THE SYNTHESISED INPUT, 0x0046b12f. */
	r->tickInputWord = 1;

	/*
	 * 2. THE THROTTLE, 0x0046b13c. It ramps toward the lane's top
	 * speed and is capped there - no torque curve, no gearbox.
	 *
	 * THE GOTO IS REAL: a crashed racer on the grounded arm skips
	 * BOTH the ramp and the charge meter, while a crashed racer on
	 * the other arm skips only the ramp. Written as the original
	 * spells it rather than folded.
	 */
	if (!env->preRace && (r->onGround != 0 || persoAnim == 0)) {
		if (r->crashLatch == 0) {
			float cap = racer_ai_top_speed(r->realSpeedMax, r->lane);

			r->aiRoadSpeed = racer_ai_throttle_step(r->aiRoadSpeed,
			                                        dtScaled,
			                                        r->realSpeedMax,
			                                        r->lane);
			if (cap < r->aiRoadSpeed)
				r->aiRoadSpeed = cap;

			racer_ai_lane_step(env->racers, env->racerCount, r, t,
			                   env->raceDir, env->mirrorWindow,
			                   dtScaled);
		}
	} else if (r->crashLatch == 0) {
		racer_ai_lane_step(env->racers, env->racerCount, r, t,
		                   env->raceDir, env->mirrorWindow, dtScaled);
	}

	/*
	 * 3. THE TARGET POINT, 0x0046b1e8.
	 *
	 *     seg    = wrap(segmentStart + farAhead)
	 *     offset = segment[seg].lineOffset + chargeMeter
	 *
	 * racer_ai_target_point has what the offset means.
	 */
	seg = trk_wrap_segment_index(t, r->segmentStart + farAhead);
	racer_ai_target_point(t, seg, r->chargeMeter, target);

	/*
	 * 4. THE TURN, 0x0046b24d, and it is skipped entirely while
	 * crashed - a downed AI keeps whatever heading it had.
	 */
	if (r->crashLatch == 0) {
		mat34_look_at(look, r->position, target);
		mat34_invert_rigid(inv, look);

		yawBike = -racer_matrix_yaw(r->matOrientation);
		yawAim  = racer_matrix_yaw(inv);

		/*
		 * BOTH YAWS ARE WRAPPED INTO [0, 2pi) FIRST and only then
		 * brought within pi of each other - the original's two
		 * while loops each, at 0x0046b2b0 and 0x0046b300. Wrapping
		 * the DIFFERENCE instead gives the same answer almost
		 * always and the wrong one across the seam.
		 */
		yawBike = racer_ai_wrap_turn(yawBike);
		yawAim  = racer_ai_wrap_turn(yawAim);
		if ((double)yawAim > (double)yawBike + MR_PI)
			yawAim -= (float)MR_TWO_PI;
		if ((double)yawAim < (double)yawBike - MR_PI)
			yawAim += (float)MR_TWO_PI;

		delta = racer_ai_wrap_turn((yawAim - yawBike) * AI_STEER_DELTA);
		if (delta > (float)MR_PI)
			delta -= (float)MR_TWO_PI;

		/*
		 * THE STEERING ANGLE IS PersoAnim-ONLY. A road AI reports
		 * zero steering however hard it is turning - the front
		 * wheel simply does not visibly steer. The dirt bikes get
		 * delta * 12, clamped to +-pi/4, in units of 4096/pi.
		 */
		if (persoAnim == 0) {
			r->steering = 0.0f;
		} else {
			float sv = delta * AI_STEER_GAIN;

			if (sv < -AI_LEAN_CLAMP)
				sv = -AI_LEAN_CLAMP;
			if (sv > AI_LEAN_CLAMP)
				sv = AI_LEAN_CLAMP;
			r->steering = (float)((double)sv * AI_STEER_TO_UNITS);
		}

		/* The turn goes in as a yaw-only Euler triple. */
		euler[0] = 0.0f;
		euler[1] = delta;
		euler[2] = 0.0f;
		racer_compose_orientation(r->matOrientation, euler);
	}

	/*
	 * 5. THE AIM VECTOR, 0x0046b3a4 - matOrientation row2 plus row3,
	 * normalised. racer_place_wheels builds the same thing at
	 * 0x0045c92e.
	 */
	for (i = 0; i < 3; i++)
		r->aimDir[i] = r->matOrientation[6 + i] + r->matOrientation[9 + i];
	len = (float)sqrt((double)r->aimDir[0] * r->aimDir[0]
	                  + (double)r->aimDir[1] * r->aimDir[1]
	                  + (double)r->aimDir[2] * r->aimDir[2]);
	if (len != 0.0f) {
		for (i = 0; i < 3; i++)
			r->aimDir[i] = (float)(r->aimDir[i] / len);
	}
	for (i = 0; i < 3; i++)
		aim[i] = r->aimDir[i];

	/*
	 * 6. THE BACKWARDS GUARD, 0x0046b40e. If the bike is travelling
	 * against its own aim, BOTH the direction and the engine speed
	 * are negated - so it keeps moving the same way in the world and
	 * is simply described as reversing.
	 */
	if (aim[0] * r->moveDir[0] + aim[1] * r->moveDir[1]
	    + aim[2] * r->moveDir[2] < 0.0f) {
		for (i = 0; i < 3; i++)
			r->moveDir[i] = -r->moveDir[i];
		r->engineSpeed = -r->engineSpeed;
	}

	/*
	 * 7. THE TARGET AGAIN, 0x0046b5db, AND NEARER THIS TIME - the
	 * short look-ahead, not the long one. The far point sets the
	 * heading; the near point is what the travel direction is
	 * actually pulled toward in step 12b.
	 */
	seg = trk_wrap_segment_index(t, r->segmentStart + nearAhead);
	racer_ai_target_point(t, seg, r->chargeMeter, target);

	/*
	 * 8. THE ENGINE BLEEDS OFF TOWARD ZERO, 0x0046b64a, while the
	 * impact counter is clear and the bike is moving, or whenever it
	 * is crashed. 1/18 a step, and the step that would cross zero
	 * lands exactly on it instead.
	 */
	if ((r->flImpactBlend == 0.0f && r->engineSpeed != 0.0f)
		|| r->crashLatch != 0) {
		/*
		 * THE TESTS ARE ON THE FLOAT's BITS, not its value - the
		 * idiom of racers.txt 9.7.5. (int)bits < 1 is "zero or
		 * negative" and (uint)bits < 0x80000001 is "zero or
		 * positive".
		 *
		 * READING (int)engineSpeed < 1 AS A VALUE COMPARISON is
		 * wrong in the one place it matters: a speed between 0 and
		 * 1 would take the accelerate arm and be pushed UP.
		 */
		if (r->engineSpeed <= 0.0f) {
			r->engineSpeed = (float)((double)r->engineSpeed
			                         + dtScaled * AI_ENGINE_DECAY);
			if (r->engineSpeed > 0.0f)
				r->engineSpeed = 0.0f;
		} else {
			r->engineSpeed = (float)((double)r->engineSpeed
			                         - dtScaled * AI_ENGINE_DECAY);
			if (r->engineSpeed < 0.0f)
				r->engineSpeed = 0.0f;
		}
	}

	/* 9. THE BLEND, 0x0046b6be - impact counter over 200, floored. */
	blend = racer_ai_blend(r->flImpactBlend);

	/*
	 * 10. THE SPEED, 0x0046b6f0. The aim and the current travel are
	 * mixed by how well ALIGNED they already are - floored at 0.5,
	 * so even a bike pointing across its own motion keeps half the
	 * commanded speed - then capped at the lane's top speed and
	 * lerped in over the blend.
	 */
	if (r->engineSpeed >= 0.0f) {
		if (r->onGround != 0 || persoAnim == 0) {
			r->engineSpeed = racer_ai_speed_blend(r->engineSpeed,
			                                      r->aiRoadSpeed,
			                                      blend,
			                                      r->moveDir, aim,
			                                      r->realSpeedMax,
			                                      r->lane);
		}
	} else {
		r->aiRoadSpeed = 0.0f;
	}

	/* 11. THE DIRECTION TO THE TARGET, 0x0046b8f0. */
	for (i = 0; i < 3; i++)
		toTarget[i] = target[i] - r->position[i];
	len = (float)sqrt((double)toTarget[0] * toTarget[0]
	                  + (double)toTarget[1] * toTarget[1]
	                  + (double)toTarget[2] * toTarget[2]);
	if (len != 0.0f) {
		for (i = 0; i < 3; i++)
			toTarget[i] = (float)(toTarget[i] / len);
	}

	r->segmentStart = trk_find_segment_for_point(t, r->segmentStart,
	                                             r->position, NULL);

	if (r->onGround == 0 && persoAnim != 0) {
		/*
		 * 12a. AIRBORNE, 0x0046b95e - and ONLY a PersoAnim rider
		 * can be. The direction carries the speed as a velocity,
		 * gravity is subtracted from y, and the pair are split back
		 * apart; then matOrientation is rebuilt from the result, so
		 * a jumping dirt bike points along its arc.
		 */
		for (i = 0; i < 3; i++)
			r->moveDir[i] = r->moveDir[i] * r->engineSpeed;
		r->moveDir[1] = (float)((double)r->moveDir[1]
		                        - dtScaled * AI_AIR_GRAVITY);

		len = (float)sqrt((double)r->moveDir[0] * r->moveDir[0]
		                  + (double)r->moveDir[1] * r->moveDir[1]
		                  + (double)r->moveDir[2] * r->moveDir[2]);
		r->engineSpeed = len;
		if (len != 0.0f) {
			for (i = 0; i < 3; i++)
				r->moveDir[i] = (float)(r->moveDir[i] / len);
		}

		r->aimDir[1] = r->moveDir[1];
		racer_frame_from_forward(r->matOrientation, r->moveDir);
		r->matOrientation[9]  = 0.0f;
		r->matOrientation[10] = 0.0f;
		r->matOrientation[11] = 0.0f;

		if (r->realSpeedMax < r->engineSpeed)
			r->engineSpeed = r->realSpeedMax;
	} else {
		/*
		 * 12b. ON THE GROUND. The travel direction is pulled toward
		 * the target over the blend - a crashed racer is skipped,
		 * so it slides on its old heading - and a PersoAnim rider
		 * then takes its vertical from the aim rather than from the
		 * target, which is what keeps a dirt bike following the
		 * ground's pitch.
		 */
		if (r->crashLatch == 0)
			racer_ai_steer_toward(r->moveDir, toTarget, blend);
		if (persoAnim != 0)
			r->moveDir[1] = r->aimDir[1];

		len = (float)sqrt((double)r->moveDir[0] * r->moveDir[0]
		                  + (double)r->moveDir[1] * r->moveDir[1]
		                  + (double)r->moveDir[2] * r->moveDir[2]);
		if (len != 0.0f) {
			for (i = 0; i < 3; i++)
				r->moveDir[i] = (float)(r->moveDir[i] / len);
		}
	}

	/* 13. THE MOVE, 0x0046bd0a. Nothing moves on the grid. */
	if (!env->preRace) {
		racer_ai_integrate(r->position, r->moveDir, r->engineSpeed,
		                   dtScaled);
	} else {
		r->engineSpeed = 0.0f;
		r->aiRoadSpeed = 0.0f;
	}

	r->segmentStart = trk_find_segment_for_point(t, r->segmentStart,
	                                             r->position, NULL);
}


/* ORIGIN  FUN_0046b120 @ 0x0046b120, at 0x0046b1a0 */
float racer_ai_top_speed(float realSpeedMax, int lane)
{
	return (float)((double)realSpeedMax
	               + (double)lane * (double)realSpeedMax
	                 * AI_LANE_SPEED);
}


/* ORIGIN  FUN_0046b120 @ 0x0046b120, at 0x0046b17c */
float racer_ai_throttle_step(float target, float dtScaled,
                             float realSpeedMax, int lane)
{
	float cap;

	target = (float)((double)target
	                 + (double)dtScaled * AI_THROTTLE_RAMP);
	cap    = racer_ai_top_speed(realSpeedMax, lane);
	if (target > cap)
		target = cap;
	return target;
}


/* ORIGIN  FUN_0046b120 @ 0x0046b120, at 0x0046b6be */
float racer_ai_blend(float flImpactBlend)
{
	float a = flImpactBlend * AI_BLEND_SCALE;

	if (a < AI_BLEND_FLOOR)
		a = AI_BLEND_FLOOR;
	return a;
}


/* ORIGIN  FUN_0046b120 @ 0x0046b120, at 0x0046b6f0 */
float racer_ai_speed_blend(float speed, float target, float alpha,
                           const float moveDir[3],
                           const float forward[3],
                           float realSpeedMax, int lane)
{
	double c;
	double a;
	double b;
	double w[3];
	double mag;
	double cap;
	int    i;

	if (!moveDir || !forward)
		return speed;

	/*
	 * HOW WELL THE BIKE POINTS WHERE IT IS GOING. Floored at 0.5, so
	 * even a fully sideways AI keeps half its target speed.
	 */
	c = (double)moveDir[0] * forward[0]
	    + (double)moveDir[1] * forward[1]
	    + (double)moveDir[2] * forward[2];
	if (c < 0.0)
		c = -c;
	if (c < (double)AI_ALIGN_FLOOR)
		c = (double)AI_ALIGN_FLOOR;

	a = (double)target * c;                 /* along the bike       */
	b = (1.0 - c) * (double)speed;          /* along the travel     */
	for (i = 0; i < 3; i++)
		w[i] = (double)forward[i] * a + (double)moveDir[i] * b;

	mag = sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
	cap = (double)racer_ai_top_speed(realSpeedMax, lane);
	if (mag > cap)
		mag = cap;

	/* Low-pass toward the blended magnitude. */
	return (float)((double)alpha * (double)speed
	               + (1.0 - (double)alpha) * mag);
}


/* ORIGIN  FUN_0046b120 @ 0x0046b120, at 0x0046bb8a */
void racer_ai_steer_toward(float moveDir[3], const float toTarget[3],
                           float alpha)
{
	double len;
	int    i;

	if (!moveDir || !toTarget)
		return;

	for (i = 0; i < 3; i++) {
		moveDir[i] = (float)((double)moveDir[i] * (double)alpha
		                     + (double)toTarget[i]
		                       * (1.0 - (double)alpha));
	}

	len = sqrt((double)moveDir[0] * moveDir[0]
	           + (double)moveDir[1] * moveDir[1]
	           + (double)moveDir[2] * moveDir[2]);
	if (len != 0.0) {
		for (i = 0; i < 3; i++)
			moveDir[i] = (float)(moveDir[i] / len);
	}
}


/* ORIGIN  FUN_0046b120 @ 0x0046b120, at 0x0046bc8a */
void racer_ai_integrate(float pos[3], const float moveDir[3],
                        float speed, float dtScaled)
{
	double step;
	int    i;

	if (!pos || !moveDir)
		return;

	step = (double)speed * (double)dtScaled * AI_STEP_SCALE;
	for (i = 0; i < 3; i++)
		pos[i] = (float)((double)pos[i] + (double)moveDir[i] * step);
}


#define LEAN_BLEND_SCALE    0.005f  /* 0x004d100c, = 1/200         */
#define LEAN_PLAYER_SCALE (-30.0f)  /* 0x004d1288, a FLOAT         */
#define LEAN_BREAK          0.3     /* 0x004d11f8 / 0x004d12a0     */
#define LEAN_GAIN           1.4     /* 0x004d1290                  */
#define LEAN_OFFSET         0.12    /* 0x004d1298, keeps it C0     */
#define LEAN_CLAMP          1.04f   /* 0x004d1228 / 0x004d1238     */
#define LEAN_DT_FLOOR      20.0f    /* the leanMode 0 substitution */

/* ORIGIN  maybe_Racer_UpdateLean @ 0x00469c60, at 0x00469c8e */
float racer_lean_impact_decay(MR_Racer *r, float frameDelta)
{
	if (!r)
		return 0.0f;

	if (r->flImpactBlend != 0.0f) {
		r->flImpactBlend -= frameDelta;
		if (r->flImpactBlend < 0.0f)
			r->flImpactBlend = 0.0f;
	}
	/* The same 1/200 Racer_AiFollowLine normalises with. */
	return r->flImpactBlend * LEAN_BLEND_SCALE;
}


/* ORIGIN  maybe_Racer_UpdateLean @ 0x00469c60, at 0x0046ac4a */
float racer_lean_response(float yawDelta, float dt, int leanMode)
{
	double v;

	/*
	 * A FLOOR ON dt WHEN leanMode IS 0 - stops a fast frame turning a
	 * small yaw change into an enormous lean.
	 */
	if (leanMode == 0 && dt < LEAN_DT_FLOOR)
		dt = LEAN_DT_FLOOR;
	if (dt == 0.0f)
		return 0.0f;

	v = ((double)yawDelta / (double)dt) * (double)LEAN_PLAYER_SCALE;

	/*
	 * THE GAIN BREAK. Continuous by construction: at 0.3 exactly,
	 * 0.3*1.4 - 0.12 is 0.3, so there is no step where the slope
	 * changes.
	 */
	if (v > LEAN_BREAK)
		v = v * LEAN_GAIN - LEAN_OFFSET;
	else if (v < -LEAN_BREAK)
		v = v * LEAN_GAIN + LEAN_OFFSET;

	if (v > (double)LEAN_CLAMP)
		v = (double)LEAN_CLAMP;
	else if (v < -(double)LEAN_CLAMP)
		v = -(double)LEAN_CLAMP;

	return (float)v;
}


#define GLANCE_MS          400.0f   /* the pose duration           */


#define POSE_SPEED_GATE   (20.0/3.6)  /* 0x004d0f50                 */
#define POSE_LOW_SCALE      0.18      /* 0x004d0f58                 */
#define POSE_PITCH_SCALE    4.0       /* 0x004d0f80                 */
#define POSE_PITCH_SMOOTH   0.25      /* 0x004d0f88                 */
#define POSE_LEAN_SPD_BASE (30.0/3.6) /* 0x004d0fa8                 */
#define POSE_LEAN_SPD_MUL   0.12      /* 0x004d0fb0                 */
#define POSE_LEAN_SPD_HI   (60.0/3.6) /* 0x004d0fb8                 */
#define POSE_LEAN_W_SLOW    4.3478261 /* 0x004d0fc0, = 2/0.46       */
#define POSE_LEAN_W_FAST    2.1739130 /* 0x004d0fc4, = 1/0.46       */
#define POSE_LEAN_MIX_BASE  0.46      /* 0x004d0fc8                 */
#define POSE_LEAN_MIX_MUL   1.0869565 /* 0x004d0fcc, = 1/0.92       */
#define POSE_LOCAL_LEAN_MUL 2.0       /* 0x004d0f60                 */
#define POSE_CRASH_BASE   400.0       /* 0x004d0fa4                 */
#define POSE_CRASH_SCALE    0.02      /* 0x004d0fe8                 */
#define POSE_CAM_NEAR     900.0       /* 0x004d0ffc                 */
#define POSE_CAM_FAR     1000.0       /* the 0x447a0000 compare     */
#define POSE_CAM_SCALE      0.01      /* 0x004d1004                 */

/* The original's clamp, written the same way every time. */
static float pose_clamp(double v, double lo, double hi)
{
	if (v < lo)
		v = lo;
	if (v > hi)
		v = hi;
	return (float)v;
}


/* ORIGIN  maybe_Racer_UpdatePose @ 0x0045d810, at 0x0045dbe0 */
float racer_pose_pitch(const float front[3], const float rear[3],
                       float axeDistance, float *poseBlend)
{
	double den;
	double p;

	if (!front || !rear || !poseBlend)
		return 0.0f;

	den = (double)axeDistance * (double)K_WHEELBASE_FRONT;
	if (den == 0.0)
		return *poseBlend;

	p = (((double)front[1] - (double)rear[1]) / den) * POSE_PITCH_SCALE;
	p = (double)pose_clamp(p, -1.0, 1.0);

	/* A quarter-step filter - see racer.h. */
	*poseBlend = (float)((double)*poseBlend
	                     + (p - (double)*poseBlend) * POSE_PITCH_SMOOTH);
	return *poseBlend;
}


/*
 * ORIGIN  maybe_Racer_UpdatePose @ 0x0045d810, at 0x0045de60 - the
 *         poseBlend write, lifted to the tick.
 *
 * THE FILTER HAS NO dt, so it has to run once per SIMULATION STEP.
 * In the original that is automatic: UpdatePose is reached only from
 * UpdateSkeleton and all four of its callers are ticks. The port
 * poses from the draw - it needs the camera distance for the fade in
 * step 6 - so the one piece of state in there is advanced here
 * instead and the draw reads it.
 *
 * THE GATES ARE UpdatePose'S OWN: PersoAnim, and no wheelie. The
 * original writes poseBlend only on that branch, so a suppressed
 * pose freezes the filter rather than letting it drift.
 */
void racer_pose_pitch_tick(MR_Racer *r, const MR_BikeCarac *c)
{
	float front[3];
	float rear[3];

	if (!r || !c)
		return;
	if (!c->persoAnim || r->wheelieAngle != 0.0f)
		return;

	racer_wheel_ground(r, c, rear, front);
	(void)racer_pose_pitch(front, rear, c->axeDistance, &r->poseBlend);
}


/*
 * ORIGIN  maybe_Racer_UpdatePose @ 0x0045d810, at 0x0045df10
 *
 * THE FLAG IS PersoAnim, NOT "is this the local player".
 *
 * maybe_Racer_UpdateSkeleton pushes g_bikeCarac[bikeIndex].PersoAnim
 * at 0x00467bbe - [EAX*4 + 0x509398] with EAX = 73*bikeIndex, the
 * same expression the ground fit reads it by - and UpdatePose takes
 * it into EDI at 0x0045d869.
 *
 * PASSING isPlayer INSTEAD COSTS FOUR DIVERGENCES:
 *
 *     stance   sat is (0xf0/2)/3.6 = 120 km/h, not (0x78/2)/3.6
 *     lean     the SPEED curve, not the angle curve
 *     pitch    SKIPPED - no JUMP or DESCENT pose at all
 *     pre-race and stunt   likewise never run
 *
 * AND IT IS WRONG IN BOTH DIRECTIONS. Every SPEED bike reads
 * PersoAnim 0 (9.29.9), so on a road track the original's branches
 * are all dead and isPlayer runs every one of them; the eight
 * motocross bikes read 1, so on dirt the original takes them for
 * EVERY racer while isPlayer takes them only for the rider.
 *
 * MEASURED: hooking 0x0045df8f, the angle path, took ZERO hits
 * through a full turn while 0x0045df40, the speed path, took eight.
 * EDI reads 0 in every register dump from every hook placed in this
 * function. racers-convergence.txt 9.18.41.2.
 *
 * THE PITCH ONE IS THE "RIDER STANDS UP ON THE PEGS DOWNHILL"
 * REPORT: 0x0045db94 is TEST EDI,EDI / JZ 0x0045dc44, and 0x0045dc44
 * is past the whole pitch block.
 */
float racer_pose_lean_weight(float lean, float speed, int persoAnim,
                             float *mix)
{
	double w = (lean < 0.0f) ? -(double)lean : (double)lean;
	double m;

	if (persoAnim) {
		/* A PersoAnim bike slides because of LEAN. */
		w *= POSE_LOCAL_LEAN_MUL;
		m  = (w - POSE_LEAN_MIX_BASE) * POSE_LEAN_MIX_MUL;
		w *= POSE_LEAN_W_FAST;
	} else {
		/* EVERY SHIPPED BIKE slides because of SPEED. */
		m = ((double)speed - POSE_LEAN_SPD_BASE) * POSE_LEAN_SPD_MUL;
		w *= ((double)speed < POSE_LEAN_SPD_HI) ? POSE_LEAN_W_SLOW
		                                        : POSE_LEAN_W_FAST;
	}

	if (mix)
		*mix = pose_clamp(m, 0.0, 1.0);
	return pose_clamp(w, 0.0, 1.0);
}


/* ORIGIN  maybe_Racer_UpdatePose @ 0x0045d810, at 0x0045e1f5 */
float racer_pose_crash_weight(float crashTimerMs)
{
	return pose_clamp((POSE_CRASH_BASE - (double)crashTimerMs)
	                  * POSE_CRASH_SCALE, -1.0, 1.0);
}


/*
 * THE STANCE SATURATION SPEEDS, AND THE PAIR IS EASY TO SWAP. The
 * LOCAL player's is 0xf0 = 240 halved = 120 km/h and the PersoAnim
 * one is 0x78 = 120 halved = 60. Swap them and the tuck saturates
 * at half the speed it should, uniformly - the weight comes out
 * exactly 2.5x MOTO's at every sample, which looks like a gain
 * error rather than a swapped constant.
 *
 * MEASURED at the blend call, hooking 0x0045db88 where all three
 * arguments are already pushed; the capture is racers-convergence.txt 9.18.41.
 * ECX reads 0x000000f0 there, and (240/2)/3.6 - 20/3.6 is exactly
 * 100/3.6.
 *
 * ONLY THE LOCAL VALUE IS MEASURED. The remote one is the other half
 * of the pair and no remote racer has been captured, so it is an
 * inference from the swap. [open - capture an AI racer]
 */
#define POSE_STANCE_ANIM    (60.0/3.6)   /* 0x78 halved, PersoAnim  */
#define POSE_STANCE_PLAIN  (120.0/3.6)   /* 0xf0 halved  [verified] */
#define POSE_FINISH_BASE     5.0f        /* 0x004d0f78              */
#define POSE_FINISH_SCALE    0.1f        /* 0x004d0f7c              */
#define POSE_STUNT_OVERSHOOT 1.2         /* 0x004d0fe0              */

/* ORIGIN  maybe_Racer_UpdatePose @ 0x0045d810, at 0x0045db6f */
float racer_pose_stance_weight(float speed, int persoAnim)
{
	double sat = persoAnim ? POSE_STANCE_ANIM : POSE_STANCE_PLAIN;
	double den = sat - POSE_SPEED_GATE;

	if (den <= 0.0)
		return 0.0f;
	return pose_clamp(((double)speed - POSE_SPEED_GATE) / den,
	                  0.0, 1.0);
}


/* ORIGIN  maybe_Racer_UpdatePose @ 0x0045d810, at 0x0045dae0 */
int racer_pose_finish_index(int finishPosition, int racerCount)
{
	if (finishPosition == 0)
		return PILOT_ROT_STATIC_WIN;
	if (finishPosition == racerCount - 1)
		return PILOT_ROT_FINISH_8;
	if (finishPosition == racerCount - 2)
		return PILOT_ROT_FINISH_7;
	if (finishPosition == 1)
		return PILOT_ROT_FINISH_2;
	if (finishPosition == 2)
		return PILOT_ROT_FINISH_3;
	return PILOT_ROT_FINISH_OTHER;
}


/* ORIGIN  maybe_Racer_UpdatePose @ 0x0045d810, at 0x0045e0f8 */
float racer_pose_stunt_step(int elapsed, int duration, int frameCount,
                            int *frame)
{
	double t;
	double scaled;
	int    k;

	if (!frame)
		return 0.0f;
	*frame = 0;
	if (duration <= 0 || frameCount <= 0)
		return 0.0f;

	/*
	 * THE 1.2 OVERSHOOT IS WHAT MAKES THE STUNT HOLD - see racer.h.
	 * sin peaks at 1, so scaling past it and clamping pins t at the
	 * last frame through the middle of the move.
	 */
	t = sin(((double)elapsed / (double)duration) * MR_PI)
	    * POSE_STUNT_OVERSHOOT;
	t = (double)pose_clamp(t, 0.0, 1.0);

	scaled = (double)frameCount * t;
	k = (int)scaled;
	if (k > frameCount - 1)
		k = frameCount - 1;
	if (k < 0)
		k = 0;

	*frame = k;
	return (float)(scaled - (double)k);
}


/* A table entry, or NULL when the bike's list does not reach it. */
static const MR_AnimFrame *pose_at(const MR_PilotRots *p, int i)
{
	if (!p || i < 0 || i >= p->frameCount || i >= PILOT_ROT_MAX)
		return NULL;
	return &p->frame[i];
}


#define POSE_PRERACE_INDEX   31          /* 0x0051f034                */
#define POSE_PRERACE_RATE     0.001f     /* 0x004d0f48                */
/*
 * THE COUNTDOWN RAMP IS THREE POSES, 31 -> 32 -> staticHaut, read
 * live from the pointer pair at 0x004ea4d8 / 0x004ea4dc. Both are
 * non-NULL on a road bike, so this path is real rather than dirt-
 * only. 0x004ea4d4 is NULL and is never reached: the index that
 * would select it is clamped to 1 first.
 */
#define POSE_MOUNT_A_INDEX   32          /* [0x004ea4d8]              */
#define POSE_MOUNT_B_INDEX   32          /* [0x004ea4d8], k = 0       */
#define POSE_MOUNT_C_INDEX    3          /* [0x004ea4dc], k = 1       */
#define POSE_PODIUM_HOLD    175.0f       /* 0x004d0f64                */
#define POSE_PODIUM_RISE     25.0f       /* the 0x41c80001 compare    */
#define POSE_PODIUM_RATE      0.04f      /* 0x004d0f68                */
#define POSE_CRASH_LIMIT    400.0f       /* the 0x43c80000 compare    */
#define POSE_CRASH_REF_MUL    0.75       /* 0x004d0ff0                */
#define POSE_CRASH_SLIDE_MUL  3.0        /* 0x004d0ff8                */
#define POSE_GLANCE_SPEED    40.0f       /* 0x004d0f8c, km/h needle   */
#define POSE_GLANCE_CLOSE 262144.0f      /* 0x48800000, distance sq   */
#define POSE_GLANCE_FASTER   20.0f       /* 0x004d0f98, needle delta  */
#define POSE_GLANCE_CONE      0.7f       /* the 0x3f333333 compare    */
#define POSE_GLANCE_MS      400.0f       /* 0x004d0fa4                */
#define POSE_STUNT_SPEED     18.0555556  /* 0x004d0fd0, 65 km/h       */
#define POSE_STUNT_HEIGHT    64.0        /* 0x004d0fd8                */


/*
 * ORIGIN  maybe_Racer_UpdatePose @ 0x0045d810, at 0x0045dc31
 *
 * THE GLANCE OVER THE SHOULDER. While none is running this scans for
 * the nearest other racer and starts one when that racer is close,
 * inside a narrow cone, and measurably faster. While one IS running
 * it blends the chosen look pose in on a half-sine.
 *
 * The cone is taken between the two bikes CONTACT-POINT axes rather
 * than their matrices, so it follows where each bike points on the
 * ground.
 *
 * IT ONLY FIRES ON THE SECOND FRAME THE SAME RIVAL IS NEAREST.
 * glanceOther holds last frame's pick and the test is against it, so
 * a bike sweeping through the search does not trigger a glance.
 */
static void racer_pose_glance(MR_AnimFrame *result, MR_Racer *r,
                              const MR_PoseEnv *env,
                              const MR_PilotRots *rots)
{
	const MR_AnimFrame *b;
	MR_Racer           *rival   = NULL;
	float               bestSq = 3.402823466e+38f;
	float               ax, az, bx, bz, k, dot, cross;
	int                 i;

	if ((float)r->speedKmhNeedle <= POSE_GLANCE_SPEED)
		return;

	if (r->glancePose != 0) {
		int elapsed = env->nowMs - r->f_3075;

		b = pose_at(rots, (int)r->glancePose);
		if (b && r->glanceTimer != 0.0f) {
			anim_blend_angle_frames(result, result, b,
			    (float)sin(((double)elapsed
			                / (double)r->glanceTimer) * MR_PI));
		}
		if (r->glanceTimer < (float)elapsed)
			r->glancePose = 0;
		return;
	}

	if (!env->racers || env->racerCount < 2)
		return;

	for (i = 0; i < env->racerCount; i++) {
		MR_Racer *o = &env->racers[i];
		float     dx, dy, dz, d;

		if (o == r || o->active == 0)
			continue;
		dx = o->position[0] - r->position[0];
		dy = o->position[1] - r->position[1];
		dz = o->position[2] - r->position[2];
		d  = dx * dx + dy * dy + dz * dz;
		if (d < bestSq) {
			bestSq = d;
			rival   = o;
		}
	}

	if (rival && r->onGround != 0
	 && r->glanceOther == (unsigned int)(size_t)rival
	 && bestSq < POSE_GLANCE_CLOSE
	 && (float)(rival->speedKmhNeedle - r->speedKmhNeedle)
	        > POSE_GLANCE_FASTER) {
		ax = r->frontWheelContact[0] - r->rearWheelContact[0];
		az = r->frontWheelContact[2] - r->rearWheelContact[2];
		k  = 1.0f / (float)sqrt((double)(ax * ax + az * az));
		ax *= k;
		az *= k;

		bx = rival->rearWheelContact[0] - r->rearWheelContact[0];
		bz = rival->rearWheelContact[2] - r->rearWheelContact[2];
		k  = 1.0f / (float)sqrt((double)(bx * bx + bz * bz));
		bx *= k;
		bz *= k;

		dot   = bx * ax + bz * az;
		cross = bx * az - bz * ax;

		if ((float)r->f_3071 < dot && dot > -POSE_GLANCE_CONE
		 && dot < POSE_GLANCE_CONE) {
			r->glancePose = (cross > 0.0f) ? PILOT_ROT_GLANCE_L
			                               : PILOT_ROT_GLANCE_R;
			r->glanceTimer = POSE_GLANCE_MS;
		}
		r->f_3075 = env->nowMs;
		r->f_3071 = (int)dot;
	}
	r->glanceOther = (unsigned int)(size_t)rival;
}


/*
 * ORIGIN  maybe_Racer_UpdatePose @ 0x0045d810, at 0x0045e023
 *
 * THE STUNT. Starts on a RISING EDGE of the stunt button while
 * airborne, fast, and high enough above the last ground height, and
 * only when none is already running. The next one is drawn in advance
 * into stuntNext.
 *
 * A ROAD BIKE HAS NO STUNTS. KTMCROSS ships ten SAUTDELIRE*.ROT;
 * SPEED and SPEEDPKT ship none, so stuntCount is 0 and this never
 * fires on tarmac.
 */
static void racer_pose_stunt(MR_AnimFrame *result, MR_Racer *r,
                             const MR_PoseEnv *env,
                             const MR_PilotRots *rots)
{
	const MR_StuntPose *sp;
	const MR_AnimFrame *a;
	const MR_AnimFrame *b;
	int                 elapsed;
	int                 frame;
	float               w;

	if (!env->stunts || env->stuntCount <= 0)
		return;

	if (r->onGround == 0
	 && (double)r->speed > POSE_STUNT_SPEED
	 && (double)(r->rearWheelContact[1] - r->f_2db0) >= POSE_STUNT_HEIGHT
	 && r->stuntPressed != 0
	 && r->stuntPressedPrev == 0
	 && r->stuntPose == 0) {
		int next = r->stuntNext;

		if (next < 0 || next >= env->stuntCount)
			next = 0;
		r->stuntPoseId     = env->stunts[next].poseIndex;
		r->stuntPose       = (unsigned int)(next + 1);
		r->stuntStartStamp = env->nowMs;

		/*
		 * TWO WRITES AND THE SECOND WINS. The original calls
		 * Rand_Next and stores it, then immediately overwrites
		 * with g_framePrevMs % count - so the rand is a DEAD
		 * STORE and the choice is the clock.
		 */
		r->stuntNext = env->nowMs % env->stuntCount;
	}

	if (r->stuntPose == 0)
		return;

	sp      = &env->stunts[r->stuntPose - 1];
	elapsed = env->nowMs - r->stuntStartStamp;

	w = racer_pose_stunt_step(elapsed, sp->durationMs, sp->frameCount,
	                          &frame);

	/*
	 * FRAME 0 BLENDS FROM THE POSE SO FAR, NOT FROM ITSELF.
	 * 0x0045e15d is a JLE that skips the store at 0x0045e171, so
	 * on frame 0 the 'from' side is whatever that stack slot
	 * already held - the pose the earlier passes built - and the
	 * weight carries the rider INTO the stunt.
	 *
	 * PASSING THE STUNT'S OWN FRAME 0 ON BOTH SIDES makes the
	 * blend a constant and throws w away: the rider snaps into
	 * the pose, holds it, and snaps back. Seven of the KTM's nine
	 * stunts have frameCount 1, so for those the k > 0 arm never
	 * runs at all and the whole animation IS this weight.
	 *
	 * w IS A 0 -> 1 -> 0 HUMP - racer_pose_stunt_step takes
	 * sin(elapsed/duration * pi) - which is the ease in and the
	 * ease out in one number. That shape only makes sense
	 * against a fixed 'from'.
	 */
	b = pose_at(rots, sp->poseIndex + frame);
	if (frame > 0) {
		a = pose_at(rots, sp->poseIndex + frame - 1);
		if (a && b)
			anim_blend_angle_frames(result, a, b, w);
	} else if (b) {
		anim_blend_angle_frames(result, result, b, w);
	}

	if (elapsed > sp->durationMs)
		r->stuntPose = 0;
	r->handSkeleton = sp->handSkeleton;
}


/*
 * ORIGIN  maybe_Racer_UpdatePose @ 0x0045d810, at 0x0045e2f6
 *
 * How many parts the LOD slot has, from the per-slot array at
 * racer+0x2d4c. The write-back copies the smaller of this and the
 * blended frame's own joint count.
 */
static int racer_pose_slot_count(const MR_Racer *r, int slot)
{
	int n;

	if (slot < 0 || slot >= RACER_LOD_SLOTS)
		return 0;
	memcpy(&n, &r->perSlotArrays[slot * 4], sizeof n);
	if (n < 0)
		n = 0;
	if (n > RACER_PARTS_PER_LOD)
		n = RACER_PARTS_PER_LOD;
	return n;
}


/*
 * ORIGIN  maybe_Racer_UpdatePose @ 0x0045d810
 *
 * THE RIDER POSE, whole. Seven passes stack onto one frame, each
 * blending the running result against another .rot:
 *
 *     1  the base stance, or the pre-race and podium variants
 *     2  the pitch          suppressed by a wheelie
 *     3  the glance         at a bike drawing alongside
 *     4  the lean
 *     5  the stunt          airborne, local, rising edge
 *     6  the crash
 *     7  the camera fade
 *
 * THE PRE-RACE BRANCH JUMPS STRAIGHT TO 6, so the countdown rider
 * gets no pitch, glance, lean or stunt.
 *
 * The podium ramp and the stunt table are transcribed but inert:
 * env->podiumMode is 0 and env->stunts is NULL because nothing loads
 * them yet, so both take the false edge.
 */
void racer_update_pose(MR_Racer *r, int persoAnim, const MR_PoseEnv *env,
                       const MR_BikeCarac *c, const float front[3],
                       const float rear[3], float cameraDist,
                       MR_AnimFrame *out)
{
	MR_AnimFrame        base;
	MR_AnimFrame        result;
	MR_AnimFrame        tmp;
	const MR_AnimFrame *a;
	const MR_AnimFrame *b;
	const MR_PilotRots *rots;
	float               w;
	float               mix;
	float               speed;
	int                 i;

	if (!r || !env || !env->rots || !c || !front || !rear)
		return;
	rots  = env->rots;
	speed = r->speed;

	/*
	 * THE THREE CLEARS AT THE TOP, 0x0045d810..0x0045d84e. Three
	 * twenty-joint loops, which is where the 20 in MR_AnimFrame is
	 * confirmed from independently of the 0xf4 stride.
	 */
	memset(&base, 0, sizeof base);
	memset(&result, 0, sizeof result);
	memset(&tmp, 0, sizeof tmp);

	/* ---- 1. the base stance ------------------------------------- */

	if (persoAnim && env->preRace) {
		a = pose_at(rots, PILOT_ROT_STATIC_PIED);
		b = pose_at(rots, POSE_PRERACE_INDEX);
		if (a && b) {
			anim_blend_angle_frames(&result, a, b,
			    (float)(2000 - env->raceTimer) * POSE_PRERACE_RATE);
		}
		r->poseStaticPied = POSE_PRERACE_INDEX;
		goto crash_and_fade;
	}

	if ((double)speed >= POSE_SPEED_GATE) {
		r->poseStaticPied = PILOT_ROT_STATIC_PIED;

		if (r->finishPosition == 0 && r->raceState == 2
		 && env->podiumMode != 0 && env->podiumTimer > 0.0f) {
			float pt = env->podiumTimer;

			if (pt >= POSE_PODIUM_HOLD) {
				a = pose_at(rots, PILOT_ROT_STATIC_WIN);
				b = pose_at(rots, PILOT_ROT_STATIC_HAUT);
				w = (pt - POSE_PODIUM_HOLD) * POSE_PODIUM_RATE;
			} else if (pt >= POSE_PODIUM_RISE) {
				a = pose_at(rots, PILOT_ROT_STATIC_WIN);
				b = a;
				w = 0.0f;
			} else {
				a = pose_at(rots, PILOT_ROT_STATIC);
				b = pose_at(rots, PILOT_ROT_STATIC_WIN);
				w = pt * POSE_PODIUM_RATE;
			}
			if (a && b)
				anim_blend_angle_frames(&base, a, b, w);
			r->handSkeleton = 1;
		} else if (r->humanPlayer != 0 && r->raceState == 2
		        && (double)speed >= POSE_FINISH_BASE) {
			a = pose_at(rots, PILOT_ROT_STATIC_HAUT);
			b = pose_at(rots, racer_pose_finish_index(
			        r->finishPosition, env->fieldSize));
			if (a && b) {
				anim_blend_angle_frames(&base, a, b,
				    (float)(((double)speed - POSE_FINISH_BASE)
				            * POSE_FINISH_SCALE));
			}
			r->handSkeleton = 1;
		} else {
			a = pose_at(rots, PILOT_ROT_STATIC_HAUT);
			b = pose_at(rots, PILOT_ROT_STATIC);
			w = racer_pose_stance_weight(speed, persoAnim);
			if (a && b)
				anim_blend_angle_frames(&base, a, b, w);
		}
	} else {
		w = (float)((double)speed * POSE_LOW_SCALE);
		a = pose_at(rots, (int)r->poseStaticPied);

		if ((int)r->poseStaticPied == POSE_PRERACE_INDEX) {
			/*
			 * THE COUNTDOWN DISMOUNT, 0x0045db2c - two frames
			 * of a get-off-the-line move selected by the
			 * integer part of a doubled weight, through the
			 * pointer pair at 0x004ea4d4 / 0x004ea4d8.
			 */
			int k;

			w = (float)pose_clamp((double)w, 0.0, 1.0);
			w *= (float)POSE_LOCAL_LEAN_MUL;
			k  = (int)w;
			if (k > 0)
				k = 1;
			w -= (float)k;
			if (k > 0)
				a = pose_at(rots, POSE_MOUNT_A_INDEX);
			b = pose_at(rots, (k > 0) ? POSE_MOUNT_C_INDEX
			                          : POSE_MOUNT_B_INDEX);
		} else {
			b = pose_at(rots, PILOT_ROT_STATIC_HAUT);
		}
		if (a && b)
			anim_blend_angle_frames(&base, a, b, w);
	}

	/* ---- 2. the pitch ------------------------------------------- */

	/*
	 * TWO GATES, 0x0045dbc0: PersoAnim, and no wheelie. A WHEELIE
	 * SUPPRESSES THE PITCH POSE - the bike pitches up from
	 * wheelieAngle and the rider holds the base stance - and
	 * poseBlend stops advancing with it, because the original
	 * writes it only on the live branch.
	 *
	 * PersoAnim IS NOT "is this the local player". See
	 * racer_pose_lean_weight, which has the flag, the four
	 * divergences that follow from confusing the two, and the
	 * "rider stands up on the pegs downhill" report.
	 */
	w = 0.0f;
	b = &base;
	if (persoAnim && r->wheelieAngle == 0.0f) {
		/*
		 * READ, NOT ADVANCE. poseBlend is a quarter-step filter
		 * with NO dt and this function runs from the DRAW, so
		 * advancing it here steps the rider's pose on every
		 * RENDERED frame - paused ones included, which is how it
		 * shows: the rider goes on settling into his pose for
		 * about a sixth of a second after the game has stopped.
		 *
		 * racer_pose_pitch_tick does the advance, once per
		 * simulation step, and its header has why the original
		 * cannot have this problem. Everything else here is a
		 * pure function of frozen inputs and is harmless to
		 * recompute per frame, which is why only this one field
		 * showed.
		 *
		 * AN ACCUMULATOR WITH NO dt ON A PATH THAT IS NOT A
		 * SIMULATION STEP is the general shape; racer_ai_lean_step
		 * is the other one in this file.
		 */
		w = r->poseBlend;
		/* DESCENT at exactly zero - the test is on the sign bit. */
		b = pose_at(rots, (w > 0.0f) ? PILOT_ROT_JUMP
		                             : PILOT_ROT_DESCENT);
		if (w < 0.0f)
			w = -w;
		if (!b) {
			b = &base;
			w = 0.0f;
		}
	}
	anim_blend_angle_frames(&result, &base, b, w);

	/* ---- 3. the glance ------------------------------------------ */
	racer_pose_glance(&result, r, env, rots);

	/* ---- 4. the lean -------------------------------------------- */
	if (r->lean != 0.0f) {
		int slide = (r->lean >= 0.0f) ? PILOT_ROT_SLIDE_R
		                              : PILOT_ROT_SLIDE_L;
		int lean  = (r->lean >= 0.0f) ? PILOT_ROT_LEAN_R
		                              : PILOT_ROT_LEAN_L;

		w = racer_pose_lean_weight(r->lean, speed, persoAnim, &mix);

		/*
		 * The operand order differs by path: one goes slide ->
		 * lean with speed, the other lean -> slide with angle,
		 * and PersoAnim picks between them. racer_pose_lean_weight
		 * has which path the player takes and how it was measured.
		 */
		if (persoAnim) {
			a = pose_at(rots, lean);
			b = pose_at(rots, slide);
		} else {
			a = pose_at(rots, slide);
			b = pose_at(rots, lean);
		}
		if (a && b) {
			anim_blend_angle_frames(&tmp, a, b, mix);
			anim_blend_angle_frames(&result, &result, &tmp, w);
		}
	}

	/* ---- 5. the stunt ------------------------------------------- */
	if (persoAnim)
		racer_pose_stunt(&result, r, env, rots);

crash_and_fade:
	/* ---- 6. the crash, else 7. the camera fade ------------------ */
	if (r->crashLatch == 0) {
		if (cameraDist > POSE_CAM_NEAR && cameraDist < POSE_CAM_FAR) {
			b = pose_at(rots, PILOT_ROT_STATIC);
			if (b) {
				anim_blend_angle_frames(&result, &result, b,
				    (float)(((double)cameraDist - POSE_CAM_NEAR)
				            * POSE_CAM_SCALE));
			}
		}
	} else if (r->flCrashTimerMs < POSE_CRASH_LIMIT) {
		b = pose_at(rots, PILOT_ROT_CRASH_FALL);
		if (b) {
			anim_blend_angle_frames(&result, &result, b,
			    racer_pose_crash_weight(r->flCrashTimerMs));
		}

		/*
		 * THE SECOND CRASH PASS, 0x0045e277 - how far the rider
		 * has slowed below three quarters of his reference speed,
		 * through a half-sine and applied NEGATED so it unwinds.
		 */
		w = (float)((double)r->flRiderSpeedRef * POSE_CRASH_REF_MUL);
		if (r->flRiderSpeed < w && w != 0.0f) {
			double u = (((double)w - (double)r->flRiderSpeed)
			            / (double)w) * POSE_CRASH_SLIDE_MUL;

			u = pose_clamp(u, -1.0, 1.0);
			b = pose_at(rots, PILOT_ROT_CRASH_DOWN);
			if (b) {
				anim_blend_angle_frames(&result, &result, b,
				    (float)-sin(u * MR_HALF_PI + MR_PI));
			}
		}
		r->handSkeleton = 1;
	}

	/* ---- the slot, and the write-back --------------------------- */

	/* handSkeleton FORCES SLOT 1 - see racer_select_skeleton_slot. */
	if (r->handSkeleton != 0)
		r->skeletonSlot = 1;

	if (r->skeletonSlot >= 0 && r->skeletonSlot < RACER_LOD_SLOTS) {
		int n = racer_pose_slot_count(r, r->skeletonSlot);

		if (n > result.count)
			n = result.count;
		for (i = 0; i < n; i++) {
			r->pose[r->skeletonSlot][i].angle[0]
			    = result.angle[i][0];
			r->pose[r->skeletonSlot][i].angle[1]
			    = result.angle[i][1];
			r->pose[r->skeletonSlot][i].angle[2]
			    = result.angle[i][2];
		}
	}

	if (out)
		*out = result;

	/*
	 * THE EDGE LATCH, 0x0045e36c. stuntPressed is cleared every
	 * frame, so the input pass has to re-set it; that is what makes
	 * the test in pass 5 a rising edge rather than a level.
	 */
	r->stuntPressedPrev = r->stuntPressed;
	r->stuntPressed     = 0;
}


#define LEAN_SPEED_GATE_MUL  8.0     /* 0x004d1020 */
#define LEAN_DELTA_MUL       2.0     /* 0x004d0f60 */
#define LEAN_DELTA_CLAMP     1.0471976f /* pi/3; 0x004d1250/0x004d1260 */

/* ORIGIN  Racer_GetYaw @ 0x00490220 */
float racer_matrix_yaw(const float m[12])
{
	if (!m)
		return 0.0f;
	return (float)atan2((double)m[2], (double)m[8]);
}


static double lean_wrap(double a)
{
	while (a < 0.0)
		a += MR_PI * 2.0;
	while (a > MR_PI * 2.0)
		a -= MR_PI * 2.0;
	return a;
}


/* ORIGIN  maybe_Racer_UpdateLean @ 0x00469c60, at 0x0046a962 */
#define LEAN_SPEED_CAP     50.0f    /* 0x42480000 at 0x0046a87e     */
#define LEAN_ATAN_K  1.445912045170292 /* 0x004d1220, a DOUBLE       */

/*
 * ORIGIN  maybe_Racer_UpdateLean @ 0x00469c60, at 0x0046a823
 *
 * THE LIVE yawDelta, and it is a PLAIN WRAP of the two yaws into
 * (-pi, pi] - 0x0046a823..0x0046a870, a while loop subtracting 2*pi
 * then one more subtraction past pi.
 *
 * NO SPEED GATE, NO DOUBLING, NO pi/3 CLAMP. racer_lean_delta has all
 * three and they belong to the OTHER branch - the 2.0 at 0x004d0f60
 * is read at 0x0046ab00, inside it. Measured, MOTO's yawDelta at this
 * point is 0.7137 where the raw wrapped difference is 0.718 and the
 * doubled one would be 1.44.
 */
float racer_lean_delta_live(float yawOrient, float yawFrame)
{
	double d = lean_wrap((double)yawOrient) - lean_wrap((double)yawFrame);

	if (d > MR_PI)
		d -= MR_PI * 2.0;
	if (d < -MR_PI)
		d += MR_PI * 2.0;
	return (float)d;
}

/*
 * ORIGIN  maybe_Racer_UpdateLean @ 0x00469c60, at 0x0046a874
 *
 * THE LEAN THE GAME ACTUALLY USES, and it is an ARCTANGENT:
 *
 *     lean = atan( (-yawDelta / dt) * min(speed, 50.0) * 1.4459 )
 *     clamped to +-1.04
 *
 * VERIFIED BY HOOK, four consecutive frames of a steady donut, the
 * prediction equal to the stored lean to five decimals every time:
 *
 *     yawDelta  cappedSpeed    lean
 *     -0.71370     39.6791   0.89246
 *     -0.71378     39.7855   0.89382
 *     -0.71418     39.8901   0.89537
 *     -0.71456     39.9929   0.89688
 *
 * IT IS SPEED-DEPENDENT, which the piecewise curve below is not, and
 * the atan is what bounds it - the +-1.04 clamp only bites past about
 * 1.9 radians of input, where atan is already flattening.
 *
 * THE SPEED CAP IS AN INTEGER COMPARE of the float's bits against
 * 0x42480000, which is the usual positive-float trick; speed is never
 * negative here.
 */
#define LEAN_AI_SPEED_K    0.045   /* 0x004d1270, a DOUBLE          */
#define LEAN_AI_HALF         0.5   /* 0x004d0f08, shared             */
#define LEAN_AI_BASE         3.0   /* 0x004d1278                     */
#define LEAN_AI_GAIN       (-1.3)  /* 0x004d1280                     */

/*
 * ORIGIN  maybe_Racer_UpdateLean @ 0x00469c60, at 0x0046abb7
 *
 * THE THIRD LEAN, and it is what a PersoAnim bike uses when it is NOT
 * the human solo player:
 *
 *     lean = yawDelta * -1.3 / (3.0 - speed * 0.045 * 0.5)
 *
 * A SPEED-SOFTENED DIVIDE, not an arctangent and not the piecewise
 * curve - and it has NO CLAMP. The divisor falls from 3.0 toward zero
 * as speed rises, so the gain grows with speed; at the 50-ish speeds
 * a race reaches it is about -0.6 and climbing.
 *
 * WHO GETS IT. The arctangent's gate is a three-way AND at
 * 0x0046a62b..0x0046a643 - humanPlayer AND soloMode AND PersoAnim -
 * and everything that fails it while still having PersoAnim set
 * falls through to here. Hooked on the dirt track, the four hits came
 * from racers 2, 3, 4 and 5; racer 1 took the arctangent every frame.
 *
 * IT RETURNS IMMEDIATELY AFTER. 0x0046abdf is followed by the
 * epilogue, so nothing later in UpdateLean touches the lean on this
 * path.
 *
 * NO LIVE COMPARISON YET. race.c ticks the AI racers, so a
 * PersoAnim opponent on a dirt track does reach this - but no A/B
 * against MOTO has been run on an opponent's lean, so the
 * arithmetic is transcribed rather than confirmed.
 * [open - capture an AI lean]
 */
float racer_lean_ai(float yawDelta, float speed)
{
	double d = LEAN_AI_BASE - (double)speed * LEAN_AI_SPEED_K
	                          * LEAN_AI_HALF;

	if (d == 0.0)
		return 0.0f;
	return (float)(((double)yawDelta / d) * LEAN_AI_GAIN);
}

float racer_lean_atan(float yawDelta, float dt, float speed)
{
	double v;

	if (dt == 0.0f)
		return 0.0f;
	if (speed > LEAN_SPEED_CAP)
		speed = LEAN_SPEED_CAP;

	v = atan2((-(double)yawDelta / (double)dt) * (double)speed
	          * LEAN_ATAN_K, 1.0);

	if (v > (double)LEAN_CLAMP)
		v = (double)LEAN_CLAMP;
	else if (v < -(double)LEAN_CLAMP)
		v = -(double)LEAN_CLAMP;
	return (float)v;
}

/*
 * THE BRANCH BELOW IS DEAD IN THE SHIPPED GAME, and so is
 * racer_lean_delta's gate/doubling/clamp. A hook on its store at
 * 0x0046ac4f took ZERO HITS across 44 frames of donut, straight,
 * braking, wheelie and a left turn, while the atan store at
 * 0x0046a8ab fired every frame.
 *
 * Kept, transcribed, and NOT called - see racer.h.
 */
float racer_lean_delta(float yawOrient, float yawFrame, float speed,
                       float realSpeedMax, float flImpactFloor)
{
	double d;
	double gate;

	/* The two yaws, wrapped and taken the short way round. */
	d = lean_wrap((double)yawOrient) - lean_wrap((double)yawFrame);
	if (d > MR_PI)
		d -= MR_PI * 2.0;
	if (d < -MR_PI)
		d += MR_PI * 2.0;

	/*
	 * THE SPEED AUTHORITY. The original computes
	 * 1 - speed/realSpeedMax*8, clamps it at zero, and then uses
	 * 1 minus THAT - so the gate is 0 at rest and 1 once the bike is
	 * past realSpeedMax/8.
	 */
	gate = 1.0;
	if (realSpeedMax > 0.0f) {
		double t = 1.0 - ((double)speed / (double)realSpeedMax)
		                 * LEAN_SPEED_GATE_MUL;
		if (t < 0.0)
			t = 0.0;

		/*
		 * THE SAME IMPACT FLOOR AS racer_lean_blend_weight, because
		 * IN THE ORIGINAL THERE IS ONLY ONE OF THESE. 00469ccb
		 * computes 1 - speed/realSpeedMax*8, clamps it, floors it at
		 * flImpactBlend/200 and keeps the result; both the blend
		 * weight and this gate are that one value. Splitting it into
		 * two functions here made it possible to floor one and not
		 * the other, and that is exactly what happened.
		 *
		 * MEASURED with the floor on the blend weight alone: the
		 * impact counter matched MOTO to the bit, and the lean was
		 * still about three times MOTO's. The ratio is the tell -
		 * gate without the floor over gate with it predicted 3.03,
		 * 3.11 and 6.7 at three steps where the measured ratios were
		 * 3.06, 3.04 and 6.5. racers-convergence.txt 9.18.36.
		 */
		if (t < (double)flImpactFloor)
			t = (double)flImpactFloor;

		gate = 1.0 - t;
	}

	d = lean_wrap(d * gate * LEAN_DELTA_MUL);
	if (d > MR_PI)
		d -= MR_PI * 2.0;

	if (d > (double)LEAN_DELTA_CLAMP)
		d = (double)LEAN_DELTA_CLAMP;
	if (d < -(double)LEAN_DELTA_CLAMP)
		d = -(double)LEAN_DELTA_CLAMP;

	return (float)d;
}


#define LEAN_IMPACT_MUL      0.005f  /* 0x004d100c */

/* ORIGIN  maybe_Racer_UpdateLean @ 0x00469c60, at 0x00469cd6 */
float racer_lean_blend_weight(float speed, float realSpeedMax,
                              float flImpactFloor)
{
	double a;

	if (realSpeedMax <= 0.0f)
		return 1.0f;

	a = 1.0 - ((double)speed / (double)realSpeedMax) * LEAN_SPEED_GATE_MUL;
	if (a < 0.0)
		a = 0.0;

	/*
	 * THE IMPACT FLOOR - 0x00469cfc..0x00469d13, a plain max() of
	 * the speed authority against flImpactBlend/200, which
	 * racer_lean_impact_decay returns.
	 *
	 * THIS IS WHAT KEEPS THE BIKE UPRIGHT AGAINST A RAIL. Every wall
	 * path sets flImpactBlend to 200, so the floor is 1.0 on the
	 * frame of a hit. This weight is the alpha that blends heading
	 * against travel, so alpha 1 makes the sampled frame the HEADING
	 * outright; matBikeRoot then equals matOrientation, the two yaws
	 * racer_lean_delta differences are identical, and the lean is
	 * zero. It eases back as the counter decays and every fresh
	 * bounce re-arms it.
	 *
	 * MEASURED, throttle-and-steer into a rail from a standstill:
	 * MOTO's lean collapses to 0.1 degrees at the first contact and
	 * stays under 0.7 for the whole ride, while ours - with the
	 * floor missing - climbed past 34 degrees, the bike laid over
	 * into the wall as if cornering. That is the "clipped deep into
	 * the rail" report. racers-convergence.txt 9.18.36.
	 */
	if (a < (double)flImpactFloor)
		a = (double)flImpactFloor;

	return (float)a;
}


/* ORIGIN  maybe_Racer_UpdateLean @ 0x00469c60, at 0x00469d3f */
int racer_travel_frame(float frame[12], const MR_Racer *r, float alpha)
{
	float blend[3];
	float dot;
	int   i;

	if (!frame || !r)
		return 0;

	/*
	 * THE TRAVEL DIRECTION, blended toward the bike's own heading as
	 * it slows. matOrientation transformed by (0,0,1) is just its
	 * forward row.
	 */
	dot = 0.0f;
	for (i = 0; i < 3; i++)
		dot += r->matOrientation[6 + i] * r->moveDir[i];

	for (i = 0; i < 3; i++) {
		float fwd = r->matOrientation[6 + i];
		float mov = (dot < 0.0f) ? fwd : r->moveDir[i];

		blend[i] = fwd * alpha + mov * (1.0f - alpha);
	}

	return racer_frame_from_forward(frame, blend);
}


#define STEER_TORQUE_K     0.01     /* 0x004d1004                    */
#define STEER_FADE_MAX     0.5      /* 0x004d0f08, and the fade floor
                                     * 0x004d0f28 is 0.0             */
#define STEER_HALF         0.5      /* 0x004d1018                    */
#define STEER_TORQUE_AI    0.004f   /* 0x004d10d8                    */
#define STEER_GRIP_SLIP    0.1      /* 0x004d0ea8                    */
#define STEER_GRIP_MAX     1.6      /* 0x004d10e0                    */

/*
 * ORIGIN  maybe_Racer_ApplySteering @ 0x00466240, its tail from
 *         0x0046651d to the FUN_0040fd20 call
 *
 * NOT A SEPARATE ORIGINAL - 0x0046658f is inline in
 * maybe_Racer_ApplySteering and nothing else calls this. It is its
 * own function here only because it is long.
 *
 * The steering TORQUE - how far the bike's heading turns this frame.
 * The original builds a PURE YAW vector (0, y, 0), turns it into a
 * matrix with FUN_0040fd40 and multiplies it into matOrientation in
 * FUN_0045cb00; y is what this returns.
 *
 * TWO SPEED FADES EXIST AND THEY ARE NOT THE SAME ONE. This is the
 * physics fade, off vitesseMaxBoite. The visual fade in
 * maybe_Racer_Draw is off realSpeedMax and scales the drawn angle.
 * Do not merge them.
 *
 * THE RESULT IS RADIANS, and the SIGN IS NEGATED, both measured
 * against MOTO.EXE rather than derived - Ghidra loses the scale
 * inside FUN_0040fd40, so the units cannot be read off the
 * decompilation. Holding left gives steer = -600 (the lock stop) and
 * the body yaw RISES by 0.0467 rad a frame; this formula gives
 * 0.0028 * 3.3 * 0.01 * 600 * 0.926 = 0.0513 before the first frames
 * of ramp-up are averaged in. racers-convergence.txt 9.61.
 */
float racer_steer_torque(const MR_Racer *r, const MR_BikeCarac *c,
                         float wheelieAngle, float slipAbs,
                         float dtScaled)
{
	double factor;
	double grip;

	if (!r || !c)
		return 0.0f;

	if (!r->humanPlayer) {
		factor = STEER_TORQUE_AI;
	} else {
		double fade = 0.0;

		if (c->vitesseMaxBoite > 0.0f) {
			fade = ((double)r->speed / (double)c->vitesseMaxBoite)
			       * STEER_FADE_MAX;
		}
		if (fade < 0.0)
			fade = 0.0;
		else if (fade >= STEER_FADE_MAX)
			fade = STEER_FADE_MAX;

		factor = (double)c->rotationTransfert
		         - fade * (double)c->rotationTransfert * STEER_HALF;

		if (wheelieAngle != 0.0f)
			factor *= STEER_HALF;
	}

	/*
	 * GRIP. A sliding bike turns harder for the same bar angle, and
	 * braking into a corner pins it at the maximum.
	 */
	grip = 1.0;
	if (r->fxGate) {
		grip = fabs((double)slipAbs) * STEER_GRIP_SLIP + 1.0;
		if (grip < 1.0)
			grip = 1.0;
		else if (grip >= STEER_GRIP_MAX)
			grip = STEER_GRIP_MAX;
		if ((r->inputWord & 2u) != 0 && (r->inputWord & 0xcu) != 0)
			grip = STEER_GRIP_MAX;
	}

	return (float)-(grip * factor * (double)dtScaled * STEER_TORQUE_K
	                * (double)r->steering);
}
/* ------------------------------------------------------------------ */
/*
 * WHAT maybe_Racer_UpdateGroundContact READS. Every one is an
 * address in MOTO.EXE, read live rather than fitted:
 *
 *   the speed thresholds are round km/h over 3.6 - 100, 80, 20, 2
 *   and 1 - which is the tell that they were written as km/h.
 *
 *   THE YAW DRAIN IS 0.0235619 RAD, and 0.0235619 * 3.3 in degrees
 *   is 4.455 a frame. Measured against MOTO.EXE on a square hit:
 *   4.43 to 4.45 a frame, then a dead stop. That is the constant.
 */
#define RACER_WALL_PUSH            32.0f      /* 0x004d1024 */
#define RACER_WALL_DAMP            0.75       /* 0x004d0ff0 */

#define RACER_WALL_AIRSLIDE        0.8        /* 0x004d11c0 */
#define RACER_WALL_HARD_COS        0.98       /* 0x004d11d0 */
#define RACER_WALL_HARD_SPEED     (100.0/3.6) /* 0x004d11d8, exact   */
#define RACER_WALL_CRASH_SPEED     (80.0/3.6) /* 0x004d11e0, exact   */
#define RACER_WHEELIE_CRASH_SPEED  (20.0/3.6) /* 0x004d0f50, exact   */
#define RACER_WALL_BOUNCE          0.9        /* 0x004d11b8 */
#define RACER_WALL_SLIDE           0.5        /* 0x004d0f08 */
#define RACER_WALL_SLIDE_MIN       (5.0/3.6)  /* 0x004d11e8, exact   */
#define RACER_WALL_SCRAPE          0.3        /* 0x004d11f8 */
#define RACER_SPEED_1KMH           (1.0/3.6)  /* 0x004d0f70 */
#define RACER_SPEED_2KMH           (2.0/3.6)  /* 0x004d10c0 */
#define RACER_IMPACT_BLEND         200.0f     /* 0x004693d5 */
#define RACER_KMH_TO_MS            (1.0/3.6)  /* 0x004d0f70 */
#define RACER_YAW_DRAIN            0.0235619  /* 0x004d1208 / 1210 */


/*
 * racer_compose_yaw - matOrientation = Ry(yaw) * matOrientation.
 *
 * ORIGIN  Racer_ComposeOrientation @ 0x0045cb00
 *
 * The original builds the rotation with Vec3_Set(0, yaw, 0) then
 * maybe_Mat34_FromEuler and passes the 3x4 by value. Only the Y
 * component is ever non-zero on this path, so this is that case
 * written out. It MULTIPLIES - the yaw is an increment.
 */
void racer_compose_yaw(float m[12], float yaw)
{
	/*
	 * THE TABLE, NOT libm. maybe_Mat34_FromEuler indexes g_cosTable;
	 * using an exact cosine here makes the orientation a fixed point
	 * of its own rebuild, so the matrix never quantises. MOTO does
	 * quantise - placed at exactly 40 degrees it walks to table index
	 * -1817 over two frames and stops, while an exact rebuild sits at
	 * -1820 from the first frame. Three indices is 0.076 degrees of
	 * heading, which is 0.074 units of wheel contact.
	 */
	int    i  = (int)((double)yaw * TRIG_IDX_PER_RAD);
	double c  = trig_cos(i);
	double s  = trig_sin(i);
	int    k;

	/*
	 * THE SENSE MATTERS AND THIS ONE IS EMPIRICAL. With
	 * forward = (-sin p, 0, cos p) and yaw = atan2(-x, z), this
	 * form gives a new yaw of p + a; the transpose gives p - a.
	 *
	 * THE TRANSPOSE WAS SHIPPED BRIEFLY AND REVERSED THE BARS -
	 * left steered right - and it reversed the wall recovery with
	 * them, paying the pending yaw out AWAY from its target. One
	 * sign, two symptoms, and the second one is invisible unless
	 * you already know the first.
	 */
	for (k = 0; k < 12; k += 3) {
		double x = (double)m[k];
		double z = (double)m[k + 2];

		m[k]     = (float)(x * c - z * s);
		m[k + 2] = (float)(x * s + z * c);
	}
}


/*
 * racer_orient_quantise - the tail of Racer_ComposeOrientation.
 *
 * ORIGIN  Racer_ComposeOrientation @ 0x0045cb00, at 0x0045cc36
 *
 * THE ORIGINAL NEVER CARRIES A CONTINUOUS ORIENTATION. Every frame it
 * multiplies matOrientation, hands the result to FUN_0046d270 to
 * decompose into (roll, pitch, yaw), scales each by 0x4000/2pi,
 * TRUNCATES with __ftol, and rebuilds the 3x3 from
 * g_cosTable[i & 0x3fff]. So all three angles are quantised, not just
 * the heading - and the yaw the game reads back afterwards is atan2 of
 * two REBUILT elements that carry pitch and roll in them.
 *
 * Quantising a scalar heading reproduces neither the other two angles
 * nor that coupling.
 *
 * THE DECOMPOSE, FUN_0046d270 @ 0x0046d270:
 *
 *     sp = m[6];  s = sqrt(1.0 - sp*sp)      1.0 at 0x004d0ec8
 *     if (s > 1e-5) {                        eps at 0x004d0e50
 *         pitch = atan2(sp, s)
 *         roll  = atan2(-m[7]/s, m[8]/s)
 *         cy = m[0]/s;  sy = -m[3]/s
 *     } else {
 *         pitch = +-pi/2;  roll = 0
 *         cy = m[4];  sy = m[1]
 *     }
 *     yaw = atan2(sy, cy)
 *
 * THE REBUILD IS VERIFIED AGAINST THE ORIGINAL, instruction by
 * instruction, by tracing the x87 stack through
 * 0x0045cc36..0x0045ccd2. All nine elements agree, and they are the
 * same nine maybe_Mat34_FromEuler (0x0040fd40) writes - checked term
 * by term against mat34_from_euler in model.c.
 *
 *     m[0] = cB*cC                m[1] = sC*cA + sB*cC*sA
 *     m[2] = sC*sA - sB*cC*cA     m[3] = -(sC*cB)
 *     m[4] = cC*cA - sC*sB*sA     m[5] = cC*sA + sC*sB*cA
 *     m[6] = sB                   m[7] = -(cB*sA)
 *     m[8] = cB*cA
 *
 * with A = roll, B = pitch, C = yaw.
 *
 * THE TABLE, NOT libm. Computing cos() in double is a DIFFERENT
 * FUNCTION at the quarter turns: cos(3pi/2) in double is -1.83697e-16
 * where the table holds -1.190342746e-12, and neither is zero.
 *
 * THE TRANSLATION IS NOT TOUCHED HERE. The original zeroes
 * matOrientation+0x24..0x2c because its orientation matrix is pure
 * rotation and position lives at racer+0x30. Ours carries a
 * translation the original does not keep there, so zeroing it would be
 * transcribing an assumption rather than a behaviour.
 *
 * IT BELONGS TO ONE RACER, so rule 13 puts it here rather than in
 * race.c, and racer_update_airborne needs it as well as the compose.
 * A placement constraint is invisible once satisfied; this is the
 * note that keeps it from being moved back.
 */
void racer_orient_quantise(float *m)
{
	double sp, s, pitch, roll, yaw, cy, sy;
	float  cp_, sp_, cr, sr, cyw, syw;
	int    ip, ir, iy;

	sp = (double)m[6];
	s  = 1.0 - sp * sp;
	s  = (s > 0.0) ? sqrt(s) : 0.0;

	if (s > RACER_ORIENT_EPS) {
		pitch = atan2(sp, s);
		roll  = atan2(-(double)m[7] / s, (double)m[8] / s);
		cy    = (double)m[0] / s;
		sy    = -((double)m[3] / s);
	} else {
		pitch = (sp < 0.0) ? -MR_HALF_PI : MR_HALF_PI;
		roll  = 0.0;
		cy    = (double)m[4];
		sy    = (double)m[1];
	}
	yaw = atan2(sy, cy);

	ir = (int)((double)(float)roll  * TRIG_IDX_PER_RAD);
	ip = (int)((double)(float)pitch * TRIG_IDX_PER_RAD);
	iy = (int)((double)(float)yaw   * TRIG_IDX_PER_RAD);

	cr  = trig_cos(ir);  sr  = trig_sin(ir);
	cp_ = trig_cos(ip);  sp_ = trig_sin(ip);
	cyw = trig_cos(iy);  syw = trig_sin(iy);

	m[0] = cyw * cp_;
	m[1] = sp_ * cyw * sr + syw * cr;
	m[2] = syw * sr - sp_ * cyw * cr;
	m[3] = -(syw * cp_);
	m[4] = cyw * cr - syw * sp_ * sr;
	m[5] = syw * sp_ * cr + cyw * sr;
	m[6] = sp_;
	m[7] = -(cp_ * sr);
	m[8] = cp_ * cr;
}


/*
 * racer_compose_orientation - fold a rotation into matOrientation.
 *
 * ORIGIN  Racer_ComposeOrientation @ 0x0045cb00
 *
 *     0045cb0a  ESI = racer + 0xbc                 matOrientation
 *     0045cb19  Matrix34_Multiply(scratch, arg, matOrientation)
 *     0045cb1e  matOrientation = scratch           twelve floats
 *     0045cc36  the decompose-and-rebuild above
 *
 * NOT A SETTER. maybe_Racer_ApplySteering builds a yaw that SCALES
 * WITH dt at 0x00466625, so it can only be a per-frame increment; an
 * absolute set would snap the bike to a fixed heading every tick.
 *
 * Matrix34_Multiply(out, a, b) is out = a * b [verified in
 * mat34_multiply], and the original passes the incoming rotation as a
 * and matOrientation as b - so the rotation is applied on the LEFT.
 *
 * THE CALLER PASSES AN EULER TRIPLE, not a matrix, at the site that
 * matters: maybe_Racer_UpdateAirborne builds (angle, 0, 0) with
 * Vec3_Set at 0x004666e3 and maybe_Mat34_FromEuler at 0x004666ed. So
 * this takes the triple and builds the matrix itself.
 *
 * ORDER IS (roll, pitch, yaw) - euler[0] is the one that produces
 * row1 = (0, cA, sA) and row2 = (0, -sA, cA), which is what MOTO's
 * braking matrix reads live.
 */
void racer_compose_orientation(float m[12], const float euler[3])
{
	float rot[12];
	float prod[12];
	int   i;

	if (!m || !euler)
		return;

	mat34_from_euler(rot, euler);       /* nine elements only */
	rot[9] = rot[10] = rot[11] = 0.0f;

	mat34_multiply(prod, rot, m);
	for (i = 0; i < 12; i++)
		m[i] = prod[i];

	racer_orient_quantise(m);
}


/*
 * racer_dive_angle - pass an angle through, or kill it.
 *
 * ORIGIN  FUN_0045d0c0 @ 0x0045d0c0
 *
 *     a = Math_WrapAnglePi(a)
 *     if (attitude > +AIR_ATT_LIMIT || attitude < -AIR_ATT_LIMIT)
 *         a = 0
 *
 * THIS IS WHY THE BRAKE DIVE DOES NOT ACCUMULATE. Once the chassis has
 * pitched past the band the function returns zero, so holding the brake
 * composes ONE step and then nothing. Measured on MOTO: four brake
 * frames give row1 = (0, 0.999699, 0.024541), and 0.024541229 is
 * sin(pi/128) to eight places - one step, not four.
 */
static float racer_dive_angle(const MR_Racer *r, float angle)
{
	float a   = math_wrap_pi(angle);
	float att = racer_chassis_attitude(r);

	if (att > AIR_ATT_LIMIT || att < -AIR_ATT_LIMIT)
		a = 0.0f;
	return a;
}


/*
 * racer_update_airborne - the air timer, the brake dive, the manual
 * gear latch.
 *
 * ORIGIN  maybe_Racer_UpdateAirborne @ 0x00466660
 *
 * maybe_Racer_Tick calls it at 0x0045d7a6, AFTER UpdateGroundContact
 * (0x0045d79c) has set onGround and BEFORE UpdateForkDive.
 *
 * THE AIR TIMER, racer+0x0f0, and this is its only writer pair:
 *
 *     00466672  AIRBORNE  flAirTime += 2.592, a HARDCODED per-frame
 *                         step, NOT scaled by dt
 *     0046668d  GROUNDED  flAirTime = 0
 *
 * 2.592 = 3.3 * pi/4 to four places, and 3.3 is g_frameDeltaScaled at
 * 33 ms, so the step was tuned for a fixed 33 ms frame. A real
 * framerate dependency, not a dt-scaled rate. [inferred]
 *
 * THE AIRBORNE ARM JUMPS PAST THE GROUNDED BLOCK ONLY - the JMP at
 * 0x00466688 lands on 0x00466735, so the gear latch and the shift tick
 * still run in the air.
 *
 * IT IS THE ONLY THING THAT PUTS PITCH INTO matOrientation ON A
 * GROUNDED BIKE, which is why it cannot be skipped. Without it
 * the port's matrix is a pure yaw - row1 exactly (0,1,0) against
 * MOTO's (-0.023506, 0.999700, 0.006898) - and because row0 . row2 is
 * zero by construction, physics_slip_speed then returns a STRUCTURAL
 * zero. That silently killed both the skid ribbon's width and the
 * MotoType 1 grip curve. See docs/subsystems/fx-particles.txt 10.4.
 *
 * NOT REPEATED HERE, because race_tick_physics already runs both at
 * its own measured placement:
 *
 *     0x00466730  Gearbox_RequestAutoShift  -> race.c, the shift table
 *     0x004667d0  Gearbox_TickShift         -> race.c gearbox_tick_shift
 *
 * Calling them again would tick the shift timer twice a frame. The
 * manual up/down latch below has no counterpart in race.c and IS new.
 */
void racer_update_airborne(MR_Racer *r, unsigned int buttons,
                           MR_Gearbox *gb, const MR_BikeCarac *c,
                           float frameDelta)
{
	if (!r)
		return;

	if (r->onGround == 0) {
		/* 0x00466672 */
		r->flAirTime = (float)((double)r->flAirTime
		                       + AIR_TIME_STEP);
	} else {
		float longSpeed;

		r->flAirTime = 0.0f;                  /* 0x0046668d */

		/* 0x0046669c, on model instance 0. */
		longSpeed = physics_long_speed(r->matOrientation,
		                               r->velDir, r->speed);

		/* 0x004666a9 the brake bit, 0x004666b5 the 0.1 floor. */
		if ((buttons & 2) != 0 && longSpeed > AIR_LONGSPEED_MIN) {
			float euler[3];

			euler[0] = racer_dive_angle(r, AIR_DIVE_ANGLE);
			euler[1] = 0.0f;
			euler[2] = 0.0f;
			racer_compose_orientation(r->matOrientation, euler);

			/* 0x004666f9  headingRef += frameDelta * -pi/1600 */
			/* FLD float / FMUL double / FADD float / FSTP
			 * float, 0x004666f9. */
			r->headingRef = (float)((double)frameDelta
			                        * AIR_HEADING_RATE
			                        + (double)r->headingRef);
			if ((double)r->headingRef < AIR_HEADING_FLOOR)
				r->headingRef = AIR_HEADING_FLOOR_SET;
		}
		/* 0x00466730 Gearbox_RequestAutoShift - see the header. */
	}

	/* ---- the manual gear latch, 0x00466735 ------------------- */
	if (r->humanPlayer != 0) {
		if ((buttons & 0x10) != 0 && (buttons & 0x20) != 0) {
			/* 0x0046674b  both held: arm and do nothing */
			g_racerShiftLatch = 1;
		} else if ((buttons & 0x10) != 0) {
			if (g_racerShiftLatch == 0) {
				if (r->f_0088 == 0)
					gearbox_request_upshift(gb, c);
				g_racerShiftLatch = 1;
			}
		} else if ((buttons & 0x20) != 0) {
			if (g_racerShiftLatch == 0) {
				if (r->f_0088 == 0)
					gearbox_request_downshift(gb);
				g_racerShiftLatch = 1;
			}
		} else {
			g_racerShiftLatch = 0;      /* 0x004667b1 */
		}
	}

	/* 0x004667d0 Gearbox_TickShift - see the header. */
}


/*
 * racer_save_prev_frame - copy this frame's state into the racer's
 * previous-frame slots, ready for the next step.
 *
 * ORIGIN  Racer_SavePrevFrame @ 0x00465fa0
 *
 * THE FIRST CALL IN maybe_Racer_Tick, at 0x0045d5bf. Nine
 * previous-frame pairs in one place:
 *
 *     +0x140..+0x16c  <- +0xbc..+0xe8   matOrientation, twelve floats
 *     +0x048          <- +0x03c         position TWO frames ago
 *     +0x03c          <- +0x030         position LAST frame
 *     +0x2da8         <- LongSpeed()    speedTail
 *     +0x054          <- g_frameDelta
 *     +0x2da4         <- +0x2da0        fxGate, previous
 *     +0x1c0          <- +0x1b4         front wheel contact, previous
 *     +0x344          <- +0x338         rear wheel contact, previous
 *     +0x2dbc         <- +0x2db8        onGround, previous
 *
 * THE POSITION ORDER IS LOAD-BEARING. +0x48 is loaded from +0x3c
 * BEFORE +0x3c is loaded from +0x30, so the three slots are a genuine
 * two-frame history rather than two copies of the same frame.
 * racer_apply_cornering fits its turn radius through all three;
 * leave positionPrev2 unwritten and it fits through the ORIGIN.
 *
 * IT ENDS BY FLUSHING A NEGATIVE SPEED. The original's test is
 * (uint)racer[+0x124] > 0x80000000, the float-sign-as-integer idiom -
 * it means "if the speed went negative, clamp to zero", not a
 * magnitude comparison.
 *
 * THE CONTACT AND onGround PAIRS ARE WRITTEN TWICE A FRAME IN THE
 * ORIGINAL, and that is not a transcription error. This function does
 * +0x344 <- +0x338 at the top of the tick, and
 * maybe_Racer_UpdateGroundContact does the same store again at
 * 0x00468d01 as part of its own THREE-deep shift
 * (+0x350 <- +0x344 <- +0x338, at 0x00468cd7). Nothing moves the
 * contacts between the two, so both writes store the same value. The
 * port reproduces both rather than tidying one away.
 */
void racer_save_prev_frame(MR_Racer *r, float frameDelta)
{
	int i;

	if (!r)
		return;

	for (i = 0; i < 12; i++)
		r->matOrientationPrev[i] = r->matOrientation[i];

	r->speedTail = physics_long_speed(r->matOrientation, r->velDir,
	                                  r->speed);

	/* prev2 from prev BEFORE prev from position - see above. */
	for (i = 0; i < 3; i++) {
		r->positionPrev2[i] = r->positionPrev[i];
		r->positionPrev[i]  = r->position[i];
	}

	r->frameDelta  = frameDelta;
	r->fxGatePrev  = r->fxGate;

	for (i = 0; i < 3; i++) {
		r->rearWheelContactPrev[i]  = r->rearWheelContact[i];
		r->frontWheelContactPrev[i] = r->frontWheelContact[i];
	}

	r->onGroundPrev = r->onGround;

	if (r->speed < 0.0f)
		r->speed = 0.0f;
}


/*
 * math_turn_radius_3pt - fit a circle through three plan-view points
 * and return a SIGNED radius.
 *
 * ORIGIN  maybe_Math_TurnRadius3Pt @ 0x0045cd10
 *
 * X AND Z ONLY. Y is never touched, so this is a plan-view fit and a
 * jump does not disturb it.
 *
 *   1  degenerate check, epsilon 1e-4: b==c or a==b gives FLT_MAX
 *   2  chord lengths d1 = |b-a|, d2 = |c-b|
 *   3  CLAMP EACH CHORD TO [128.0, 800.0]
 *   4  rebuild a point either side of b at half the clamped chord
 *      length along each original chord direction
 *   5  intersect the two perpendicular bisectors -> the centre
 *   6  r = |b - centre|, NEGATED when the 2D cross of the two rebuilt
 *      chords is negative
 *   7  return r * 2.0
 *   8  parallel bisectors - a straight line - gives FLT_MAX
 *
 * THE CHORD CLAMP IS THE INTERESTING PART. Rescaling both chords into
 * [128, 800] before fitting makes the result depend on the SHAPE of
 * the three points and not on how far the bike travelled between
 * them. That is frame-rate independence done in geometry rather than
 * by dividing by dt, and a port that drops the clamp gets a radius
 * that changes with framerate.
 *
 * THE 0.5 AND THE 2.0 ARE A PAIR - step 4 halves and step 7 doubles,
 * so the fit runs at half scale. Whether the result is therefore a
 * radius or a diameter is not settled by the maths; it is called a
 * radius because the caller divides v*v by it to get a centripetal
 * acceleration, which only works for a radius.
 *
 * THE CENTRE OUT-PARAMS ARE NEVER USED. The single caller passes NULL
 * for both, so they are carried here only to keep the shape.
 *
 * READ OUT OF THE DISASSEMBLY, NOT THE DECOMPILER. Ghidra renders
 * this one with the eight float arguments scrambled into the
 * return-storage pointer and it cannot be used - CLAUDE.md 15.1. The
 * body below was recovered by tracing the x87 stack through
 * 0x0045cd10..0x0045d090.
 *
 * AND NOT FROM THE PLATE COMMENT EITHER. Working from a prose
 * description gets the determinant's operand order, the numerator
 * and the second degeneracy guard wrong - see
 * whole-function-import.txt 4.
 *
 * [verified] THIS VERSION REPRODUCES MOTO'S STORED RADIUS EXACTLY.
 * Fed MOTO's own three positions - (-24738.395, 2049.712),
 * (-24862.984, 1863.514), (-24981.336, 1673.093) - it returns
 * 13339.3017, and 13339.3017 / 128 is 104.2133, which is the value
 * MOTO had in racer+0x18 at that instant to seven significant
 * figures. The /128 is the caller's CORNER_RADIUS_SCALE.
 */
float math_turn_radius_3pt(float ax, float az, float bx, float bz,
                           float cx, float cz,
                           float *pCentreX, float *pCentreZ)
{
	const float eps = 1.0e-4f;
	float  d1x, d1z, d2x, d2z, d1, d2, c1, c2;
	float  p1x, p1z, p2x, p2z;
	float  u, v, w, x, y;
	double num, den;
	float  centreX, centreZ, r, cross;

	d1x = bx - ax;  d1z = bz - az;
	d2x = cx - bx;  d2z = cz - bz;

	/* 0x0045cd47 - b==c, then 0x0045cd92 - a==b. */
	if ((fabsf(d2z) < eps && fabsf(d2x) < eps)
		|| (fabsf(d1z) < eps && fabsf(d1x) < eps)) {
		if (pCentreX) *pCentreX = 0.0f;
		if (pCentreZ) *pCentreZ = 0.0f;
		return FLT_MAX;
	}

	d1 = (float)sqrt((double)(d1x * d1x + d1z * d1z));
	d2 = (float)sqrt((double)((bx - cx) * (bx - cx)
	                          + (bz - cz) * (bz - cz)));

	/* 0x0045ce3d - high clamp first, then low, on the float bits. */
	c1 = d1;  if (c1 > 800.0f) c1 = 800.0f;  if (c1 < 128.0f) c1 = 128.0f;
	c2 = d2;  if (c2 > 800.0f) c2 = 800.0f;  if (c2 < 128.0f) c2 = 128.0f;

	/* 0x0045ce8b - a point either side of b at HALF the clamped chord. */
	p2x = ((c2 / d2) * d2x) * 0.5f + bx;
	p2z = ((c2 / d2) * d2z) * 0.5f + bz;
	p1x = (((ax - bx) / d1) * c1) * 0.5f + bx;
	p1z = (((az - bz) / d1) * c1) * 0.5f + bz;

	u = p2x - bx;
	v = p2z - bz;
	w = p1x - ax;
	x = p1z - az;
	y = p2z - p1z;

	/*
	 * 0x0045cf90 - both are computed in the x87 stack and stored as
	 * DOUBLES, so they are kept in double here.
	 */
	num = ((double)u * p2x + (double)v * y) * x - (double)p1x * w * v;
	den = (double)x * u - (double)w * v;

	/* 0x0045cfa6 then 0x0045cfcb - the guard is on den and on V. */
	if (!(fabsf((float)den) > eps) || !(fabsf(v) > eps)) {
		if (pCentreX) *pCentreX = 0.0f;
		if (pCentreZ) *pCentreZ = 0.0f;
		return FLT_MAX;
	}

	centreX = (float)(num / den);
	centreZ = ((p2x - centreX) / v) * u + p2z;

	if (pCentreX) *pCentreX = centreX;
	if (pCentreZ) *pCentreZ = centreZ;

	r = (float)sqrt((double)((bx - centreX) * (bx - centreX)
	                         + (bz - centreZ) * (bz - centreZ)));

	/* 0x0045d060 - and the operand order here decides the SIGN. */
	cross = (p2x - p1x) * (centreZ - p1z)
	        - (centreX - p1x) * (p2z - p1z);
	if (cross < 0.0f)
		r = -r;

	return r * 2.0f;
}


/*
 * racer_apply_cornering - the camber pull, the centripetal term, and
 * the lateral velocity write.
 *
 * ORIGIN  maybe_Racer_ApplyCornering @ 0x0045d110
 *
 * maybe_Racer_Tick calls it at 0x0045d792 - BETWEEN
 * ModelInstance_Integrate seeding velDir at 0x0045d770 and
 * UpdateGroundContact at 0x0045d79c. That window is the whole reason
 * this function matters: it is where the bike's velocity acquires a
 * lateral component that its heading does not have.
 *
 * WHAT IT DOES:
 *
 *     if (flAirTime > 2.592) { SlipSpeed(); return; }   airborne
 *     longSpeed = LongSpeed()
 *     slip      = SlipSpeed()
 *     grip      = 1.0                       FUN_00459f80, a constant
 *     [the copy-protection block - see below]
 *     fit the turn radius through the last three positions
 *     dy    = groundHeight(pos) + offY - groundHeight(pos + lateral)
 *     out   = (camber + centripetal) * dt * 0.01 + slip
 *     out  -= grip * 9.81 * dt * 0.01     toward zero, snapping
 *     SetSlipSpeed(out)
 *
 * THE AIRBORNE ARM CALLS SlipSpeed AND THROWS THE RESULT AWAY. It is
 * dead code in the original - transcribed as a comment rather than a
 * call, since our physics_slip_speed has no side effects (its plate
 * comment records it as read-only with respect to its receiver).
 *
 * THE CAMBER PROBE STEPS ONE UNIT ALONG ROW 0 plus the translation
 * row, samples the road there, and differences it against the height
 * under the bike - so dy is how much the ground rises one unit to the
 * side. physics_camber saturates it through dy/sqrt(dy*dy+1).
 *
 * THE CENTRIPETAL TERM IS MotoType 1 ONLY. For every other bike the
 * radius is still FITTED AND STORED but never used, and the
 * asymmetry is the original's: the MotoType 1 branch does not
 * pre-clear turnRadius[0] to FLT_MAX and the other branch does.
 *
 * NOT RECONSTRUCTED: the copy-protection block at 0x0045d18a. It
 * reads the game's own bytes at 0x00442761, and on failure zeroes
 * realSpeedMax and wheelieScale on the local racer - two independent
 * kills. It only fires in championship past index 2. A rebuilt
 * executable has no bytes to check and is legitimate by construction,
 * so the pass branch is taken unconditionally here, exactly as
 * engine_torque_at does. See docs/subsystems/copy-protection.txt 5.1.
 */
void racer_apply_cornering(MR_Racer *r, const MR_Trk *t,
                           const MR_BikeCarac *c, float dtScaled)
{
	float longSpeed, slip, grip;
	float localRadius = FLT_MAX;
	float p[3];
	float dy;
	float camber, centripetal, out;

	if (!r || !c)
		return;

	/* 0x0045d123 - airborne does nothing but a discarded read. */
	if (r->flAirTime > AIR_TIME_STEP)
		return;

	longSpeed = physics_long_speed(r->matOrientation, r->velDir,
	                               r->speed);
	slip      = physics_slip_speed(r->matOrientation, r->velDir,
	                               r->speed);
	grip      = CORNER_GRIP;          /* FUN_00459f80 returns 1.0 */

	/* 0x0045d224 - the radius fit, gated on f_000c and speed. */
	if (c->motoType == MOTOTYPE_DIRT) {
		if (r->f_000c == 0 && r->speed > 1.0f) {
			localRadius = math_turn_radius_3pt(
			                  r->positionPrev2[0], r->positionPrev2[2],
			                  r->positionPrev[0],  r->positionPrev[2],
			                  r->position[0],      r->position[2],
			                  NULL, NULL) * CORNER_RADIUS_SCALE;
			r->turnRadius[0] = localRadius;
		}
	} else {
		r->turnRadius[0] = FLT_MAX;
		if (r->f_000c == 0 && r->speed > 1.0f) {
			r->turnRadius[0] = math_turn_radius_3pt(
			                       r->positionPrev2[0], r->positionPrev2[2],
			                       r->positionPrev[0],  r->positionPrev[2],
			                       r->position[0],      r->position[2],
			                       NULL, NULL) * CORNER_RADIUS_SCALE;
		}
	}

	/* 0x0045d3a0 - the camber probe, one unit along row 0. */
	p[0] = r->position[0];
	p[1] = r->position[1];
	p[2] = r->position[2];

	if (!t || t->segmentCount == 0)
		p[1] = 0.0f;
	else
		p[1] = trk_ground_height_at(t, r->segmentStart, p);

	p[0] += r->matOrientation[9]  + r->matOrientation[0];
	p[1] += r->matOrientation[1]  + r->matOrientation[10];
	p[2] += r->matOrientation[11] + r->matOrientation[2];

	dy = p[1];
	if (t && t->segmentCount != 0)
		dy = p[1] - trk_ground_height_at(t, r->segmentStart, p);

	camber      = physics_camber(dy);
	centripetal = physics_centripetal(longSpeed, localRadius,
	                                  c->motoType, r->driveDisengaged);

	out = physics_cornering_step(camber, centripetal, slip, grip,
	                             dtScaled);

	/*
	 * 0x0045d50c - and THIS is what writes velDir laterally.
	 *
	 * THE CLAMP COMES FROM THE CARAC, NOT THE RACER. The original
	 * clamps against instance+0x90 - racer+0xb4 - and MOTO holds
	 * 80.0 there; the port holds 0.0 because nothing writes the
	 * field. Passing it therefore clamped the lateral component to
	 * ZERO on every frame, so velDir never came off the heading,
	 * slip stayed at denormal noise, and speed and gearboxRoadSpeed
	 * could never come apart - see racers.txt 9.18.23.2.
	 *
	 * Same substitution as everywhere else in the port, and the same
	 * field the copy protection zeroes to pin a pirated bike
	 * (copy-protection.txt 5.1). r->realSpeedMax having no writer
	 * remains open.
	 */
	physics_set_slip_speed(r->matOrientation, r->velDir, &r->speed,
	                       c->realSpeedMax, out);
}


/*
 * racer_wall_recovery_yaw - HOW FAR to turn the bike off a rail.
 *
 * ORIGIN  maybe_Racer_WallRecoveryYaw @ 0x0046ad10
 *
 * IMPORT 2. Its only caller is the wall path of
 * racer_update_ground_contact, which stores the result straight
 * into racer+0x2e31 for the drain at 0x00469bb0 to pay out one
 * fixed step a frame. So this decides HOW FAR to turn and the
 * drain decides how fast.
 *
 * THE ANGLE COMES IN NEGATED. The caller does Racer_GetYaw on
 * matOrientation and FCHS at 0x0046913c, so flNegYaw is minus the
 * bike's current heading.
 *
 * WHAT THE ORIGINAL DOES, and why this is shorter than 1024 bytes:
 *
 *   1. build the bike's forward for that yaw and dot it with the
 *      wall normal. POSITIVE means it already points away from the
 *      wall, and the function returns 0 - nothing to correct.
 *   2. build the bike's LEFT the same way; if that dots negative
 *      against the normal, negate the normal. This is what picks
 *      WHICH WAY along the wall to turn.
 *   3. take the yaw of a look-at built from the normal, use it as
 *      an index into the 0x4000-entry cosine table, and build
 *      (cos*K, 0, sin*K) from it - the direction ALONG the wall.
 *   4. return the shortest signed arc from the bike's heading to
 *      that direction, wrapped through 2pi and folded at pi.
 *
 * STEP 3 REDUCES. g_cosTable[i] is cos and g_cosTable[i-0x1000] is
 * sin; g_cosTable[0] is 1 and g_cosTable[0x3000] is 0, so the three
 * components collapse to (-K*cos aN, 0, K*sin aN) with K the float
 * -100.0 at 0x004d12a8. Under yaw(v) = atan2(-v.x, v.z) that is
 *
 *     aTarget = pi/2 - aNormal
 *
 * which is why this is trigonometry rather than a table walk. The
 * magnitude K drops out of an atan2 entirely.
 *
 * BEWARE 0x004d12a8. Read as a double it is 0.0, which would make
 * the whole block dead code; it is a FLOAT and it is -100.0. The
 * high dword next to it is zero, which is what makes the wrong
 * width look plausible.
 *
 * A SQUARE HIT CANNOT CHECK THIS FUNCTION. A wall normal of
 * (-1, 0, 0) gives aNormal = pi/2, where the quarter turn and its
 * mirror image agree, so a sign error in step 3 passes every
 * square-on test - see the note on aTarget below.
 *
 * CHECKED ON AN OBLIQUE WALL instead, hooked at the store in
 * UpdateGroundContact (0x00469152) during a live descent:
 *
 *     wallNormal  (-0.638950, 0, -0.769248)
 *     bike yaw    +0.449548 rad
 *     MOTO raw    -0.415169 rad   captured at the store
 *     ours        -0.414911 rad   from the field and the drain
 *
 * 0.015 degrees apart. The pending yaw then drains identically at
 * 0.15551 a step, and the forward row converges the same way -
 * MOTO -0.551, -0.668, -0.768 against ours -0.547, -0.660, -0.758.
 *
 * DO NOT MODEL THIS FUNCTION WITH atan2(-fwd.x, fwd.z) TO CHECK IT.
 * That drops the forward row's y - -0.255 here - and comes out 0.74
 * degrees off - large enough to read as a real residual.
 * racer_get_yaw is the yaw this function is handed, and it is not
 * the xz projection.
 */
float racer_wall_recovery_yaw(const MR_Racer *r,
                              const float wallNormal[3], float flNegYaw)
{
	double bikeYaw = -(double)flNegYaw;
	double aNormal;
	double aTarget;
	double d;
	float  n[3];
	float  fwd[3];
	float  left[3];
	int    i;

	(void)r;
	if (!wallNormal)
		return 0.0f;

	for (i = 0; i < 3; i++)
		n[i] = wallNormal[i];

	/*
	 * The frame for the angle handed in, in the engine's own
	 * convention - forward is (-sin, 0, cos) and left is the same
	 * shape a quarter turn round.
	 */
	/*
	 * FROM THE BIKE'S OWN YAW, NOT THE NEGATED ONE, and this is
	 * the line the recovery hung on for two sessions.
	 *
	 * The original is handed -yaw and builds its frame from that,
	 * because Racer_GetYaw's sign convention is the opposite of
	 * ours and the two negations cancel inside it. Transcribing
	 * the negation literally into OUR convention negates it once
	 * - so the forward came out pointing away from the wall, the
	 * early-out dot was +1 instead of -1, and the function
	 * returned 0 on every single impact.
	 *
	 * MEASURED: MOTO sets the pending yaw to -81.09 degrees on a
	 * square hit at 22 m/s and drains it at 4.455 a frame while
	 * the heading climbs -90 -> -54. The FIELD is negative while
	 * the TURN is positive, which is that same convention gap
	 * seen from the other side.
	 */
	fwd[0]  = -(float)sin(bikeYaw);
	fwd[1]  = 0.0f;
	fwd[2]  =  (float)cos(bikeYaw);
	/*
	 * left IS forward TURNED A QUARTER, NOT MIRRORED - the same
	 * slip as step 3's aTarget, in the other operand.
	 *
	 * The original builds this vector by putting (-1, 0, 0)
	 * through Mat34_FromEuler(0, flCurrentYaw, 0), and
	 * flCurrentYaw is MINUS the bike's yaw, so it comes out
	 * (-cos b, 0, -sin b). Writing +sin b gives that vector
	 * reflected about the x axis.
	 *
	 * IT ONLY MATTERS ON THE SIDE OF THE TEST BELOW, so the
	 * reflected form does nothing until a wall comes in at an
	 * angle where the two disagree - and a square hit gives
	 * EXACTLY ZERO either way, which is why a square-hit check
	 * cannot catch it.
	 *
	 * MEASURED at two contacts on one descent, MOTO's raw return
	 * against each branch of the test:
	 *
	 *     n=(-0.6390,0,-0.7692)  MOTO -23.70 deg
	 *         keep -24.46   negated +155.54
	 *     n=(-0.5017,0,-0.8650)  MOTO  -5.81 deg
	 *         keep  -6.69   negated +173.31
	 *
	 * MOTO keeps at both. With +sin b this negated at the second
	 * and asked for a 173-degree turn. racers-convergence.txt 9.18.34.
	 */
	left[0] = -(float)cos(bikeYaw);
	left[1] = 0.0f;
	left[2] = -(float)sin(bikeYaw);

	/* 1. Already heading away from the wall. 0x0046ad60. */
	if (fwd[0] * n[0] + fwd[1] * n[1] + fwd[2] * n[2] > 0.0f)
		return 0.0f;

	/* 2. Which way along it. 0x0046adb8. */
	/*
	 * THE EPSILON IS NOT A FUDGE. The original builds this vector
	 * through the 0x4000-entry angle table, where a quarter turn
	 * is EXACTLY zero; cos(-pi/2) in float is -4.37e-08, and a
	 * strict < 0 test flips on that. Measured: a square hit gave
	 * left.n = -4.4e-08, which is noise about a true zero and not
	 * a side.
	 *
	 * A SQUARE HIT IS ZERO WHICHEVER WAY left IS BUILT, so this
	 * test's own sign error survived every square-hit check too -
	 * see the note on left above. The epsilon guards the noise; it
	 * was never what made the answer right.
	 */
	if (left[0] * n[0] + left[1] * n[1] + left[2] * n[2] < -1e-6f) {
		for (i = 0; i < 3; i++)
			n[i] = -n[i];
	}

	/* 3. Along the wall, from the reduction above. */
	/*
	 * A QUARTER TURN, NOT A REFLECTION. This read
	 * MR_HALF_PI - aNormal, which mirrors the angle about
	 * pi/4 instead of rotating it, and the two agree ONLY when
	 * aNormal is exactly pi/2 - a square hit on a rail running
	 * down +Z. Every check written into this function was a
	 * square hit, so the bug survived three of them.
	 *
	 * MEASURED at a 140-degree wall on the descent, MOTO raw
	 * -0.414401 rad against our +1.328: the target came out at
	 * -50.286 degrees where the along-wall direction is
	 * +50.286, so the bike turned 76 degrees the WRONG WAY and
	 * drove back into the bank. That is the snag.
	 *
	 * Which of the two along-wall directions is already decided
	 * by step 2 negating n, so this is a fixed rotation and not
	 * a choice. racers.txt 9.29.12.3.
	 */
	aNormal = atan2(-(double)n[0], (double)n[2]);
	aTarget = aNormal - MR_HALF_PI;

	/*
	 * 4. The shortest signed arc. The original wraps both angles
	 * into [0, 2pi) with two while loops, brings the second
	 * within pi of the first, subtracts, wraps again and folds
	 * anything over pi - which is this.
	 */
	/*
	 * BIKE MINUS TARGET, measured rather than derived. Two live
	 * contacts, captured at the store in UpdateGroundContact:
	 *
	 *     heading -90.00 -> MOTO stored -90.00
	 *     heading -27.40 -> MOTO stored -27.40
	 *
	 * so the pending IS the bike's heading when the target is 0,
	 * and the difference runs bike - target. The other way round
	 * is what this had, and it turned the bike into the wall on
	 * every impact after the first.
	 */
	d = bikeYaw - aTarget;
	while (d < 0.0)
		d += MR_TWO_PI;
	while (d >= MR_TWO_PI)
		d -= MR_TWO_PI;
	if (d > MR_PI)
		d -= MR_TWO_PI;

	return (float)d;
}


/* ------------------------------------------------------------------ */
/*
 * racer_update_ground_contact - THE HUMAN PLAYER'S MOVER.
 *
 * ORIGIN  maybe_Racer_UpdateGroundContact @ 0x00468ca0
 *
 * IMPORT 1 under docs/whole-function-import.txt, and the first
 * function brought over whole rather than sliced. Racer_TickDispatch
 * forks on racer+0x8 into three integrators that share nothing - AI,
 * network and human - and this is the human one. It also rebuilds
 * matOrientation, so it feeds camber, slip and slope.
 *
 * THE TRACK IS A PARAMETER, not a global. The original reaches
 * g_pTrkSegments at 0x0052c108 through an implicit this; we pass
 * MR_Trk. That is the only structural change made on purpose.
 *
 * ONE HOLE REMAINS, MARKED AND NOT DROPPED - CLAUDE.md 15.1:
 *
 *   0x00469010  the championship protection block. Deliberately not
 *               reproduced - docs/subsystems/copy-protection.txt.
 *
 * ONE KNOWN DIVERGENCE, AND IT IS INACTIVE. At 0x004695f8 the
 * original gates the per-probe segment lookups on PersoAnim: with it
 * SET, and outside the low-air case, both ground probes reuse the
 * racer's own segment. racer_orient_grounded always resolves each
 * probe in its own segment.
 *
 * INACTIVE ON ROAD, LIVE ON DIRT. The eight SPEED bikes read
 * PersoAnim 0, so both games always search there and the difference
 * never fires - which is every race run so far. The eight MOTOCROSS
 * bikes read 1, so the original skips both lookups on dirt and this
 * becomes a real divergence. racers.txt 9.29.9.
 *
 * THE FIT ITSELF IS VERIFIED. Same position, same heading, same
 * frame count: both sides rebuild fwd = (+0.842790, +0.074523,
 * +0.533059), bit for bit. racers-convergence.txt 9.18.32.
 *
 * Returns 1, as the original does on every path.
 */
int racer_update_ground_contact(MR_Racer *r, const MR_Trk *t,
                                const MR_BikeCarac *c, MR_Gearbox *gb,
                                int nAxis0, int bLowAirThreshold,
                                int bNoWalls)
{
	float move[3];
	float dir[3];
	float dirBefore[3];
	float hitPoint[3];
	float rear[3];
	float front[3];
	float groundY;
	float moveLen;
	float threshold;
	int   segBefore;
	int   segHit;
	int   segRear;
	int   segFront;
	int   hit;
	int   i;

	(void)nAxis0;

	if (!r || !t || !c)
		return 1;

	r->onGroundPair1 = 0;
	r->onGroundPair2 = 0;

	/*
	 * THE CONTACT HISTORY GOES BACK THREE FRAMES, shifted here and
	 * nowhere else. The oldest copy of each pair lives inside the
	 * unnamed spans - rear at unk_0350+0, front at unk_01cc+0 -
	 * which is why neither blob is simply padding.
	 */
	for (i = 0; i < 3; i++) {
		*(float *)(r->unk_0350 + i * 4) = r->rearWheelContactPrev[i];
		r->rearWheelContactPrev[i] = r->rearWheelContact[i];
		*(float *)(r->unk_01cc + i * 4) = r->frontWheelContactPrev[i];
		r->frontWheelContactPrev[i] = r->frontWheelContact[i];
	}
	for (i = 0; i < 3; i++) {
		*(float *)(r->unk_0350 + 0x184 + i * 4) =
		    *(float *)(r->unk_0350 + 0x178 + i * 4);
		*(float *)(r->unk_0350 + 0x178 + i * 4) =
		    *(float *)(r->unk_0350 + 0x16c + i * 4);
	}

	r->onGroundPrev = r->onGround;

	segBefore = r->segmentStart;
	r->segmentStart = trk_find_segment_for_point(t, segBefore,
	                                             r->position, NULL);

	/* The frame's move, and its direction as a unit vector. */
	for (i = 0; i < 3; i++)
		move[i] = r->position[i] - r->positionPrev[i];
	moveLen = (float)sqrt((double)(move[0] * move[0]
	                               + move[1] * move[1]
	                               + move[2] * move[2]));
	for (i = 0; i < 3; i++)
		dir[i] = move[i];
	if (moveLen != 0.0f) {
		float inv = 1.0f / moveLen;

		for (i = 0; i < 3; i++)
			dir[i] = move[i] * inv;
	}

	/* 0x00469010  the championship protection block. Not ours. */

	/*
	 * THE CLIP RUNS TWICE, AGAINST TWO SEGMENTS - 0x00468fe9 and
	 * 0x0046909f. Once against the segment the move STARTED in;
	 * then, if that pass missed or its hit point landed in a
	 * different segment, again against the one being entered.
	 *
	 * ON A STRAIGHT THE TWO PASSES ARE INDISTINGUISHABLE, because
	 * consecutive segments carry the same two wall planes. On a bend
	 * they are not, and the second pass is the one that actually
	 * stops the bike - which is why a single-pass port looked
	 * correct everywhere except a corner.
	 *
	 * dir GOES IN AS THE TRAVEL DIRECTION AND COMES BACK REFLECTED,
	 * so it has to be restored before the retry.
	 */
	for (i = 0; i < 3; i++)
		dirBefore[i] = dir[i];

	/*
	 * [DEBUG] nowalls RIDES THROUGH THE RAILS, and it has to be
	 * honoured HERE now that the clip lives inside this function.
	 * IT SITS NEXT TO bLowAirThreshold IN THE PARAMETER LIST and
	 * the two are both ints. Swap them and nowalls moves the
	 * airborne threshold instead of the walls, silently - a turn
	 * test with the rails supposedly off drives straight into
	 * one.
	 */
	hit = 0;
	if (bNoWalls) {
		/* nothing to clip against */
	} else if (segBefore != r->segmentStart) {
		hit = trk_clip_move_against_walls(t, segBefore,
		                                  r->positionPrev,
		                                  r->position, hitPoint,
		                                  dir, r->wallNormal);
		if (hit) {
			segHit = trk_find_segment_for_point(t, segBefore,
			                                    hitPoint, NULL);
			if (segHit != segBefore)
				hit = 0;    /* fall through to pass two */
		}
	}
	if (!hit && !bNoWalls) {
		for (i = 0; i < 3; i++)
			dir[i] = dirBefore[i];
		hit = trk_clip_move_against_walls(t, r->segmentStart,
		                                  r->positionPrev,
		                                  r->position, hitPoint,
		                                  dir, r->wallNormal);
	}

	if (hit) {
		/* Out of the wall by 32 along its normal. 0x004690ae. */
		for (i = 0; i < 3; i++)
			r->position[i] = hitPoint[i]
		                     + r->wallNormal[i] * RACER_WALL_PUSH;
		r->segmentStart =
		    trk_find_segment_for_point(t, r->segmentStart,
		                               r->position, NULL);
		for (i = 0; i < 3; i++)
			r->collideDir[i] = dir[i];

		/* 0x00469137  the pending recovery yaw. A marked hole. */
		r->flLeanTarget =
		    racer_wall_recovery_yaw(r, r->wallNormal,
		                            -racer_matrix_yaw(
		                                r->matOrientation));

		if (r->onGroundPrev == 0) {
			/*
			 * AIRBORNE INTO A WALL is a flat one-in-four that
			 * ignores the geometry entirely, and it stops the
			 * bike dead either way.
			 */
			if ((rand() & 3) == 0) {
				(void)racer_begin_crash(r, gb, dir);
				r->riderStateBits |= 2;
			} else {
				r->riderStateBits |= 1;
			}
			r->collideSpeed = (float)((double)r->speed
			                    * RACER_WALL_AIRSLIDE);
			if ((double)r->collideSpeed < RACER_SPEED_2KMH)
				r->collideSpeed = (float)RACER_SPEED_2KMH;
			r->speed = 0.0f;
		} else {
			float impact = racer_wall_impact_cos(r->wallNormal,
			                                     dir);
			float impactAbs = (impact < 0.0f) ? -impact : impact;

			/*
			 * SQUARE AND FAST ALWAYS GOES DOWN: |cos| over 0.98
			 * at over 100 km/h. Otherwise it is one chance in
			 * four, and only above 80 km/h at more than a
			 * glance - or a wheelie above 20 km/h at any angle,
			 * which is the third condition and the one that
			 * reads oddly until you notice racer+0x307d is the
			 * wheelie angle rather than a damage state.
			 */
			if ((double)impactAbs <= RACER_WALL_HARD_COS
			    || (double)r->speed <= RACER_WALL_HARD_SPEED) {
				if (((rand() & 3) == 0
				     && impactAbs > 0.5f
				     && (double)r->speed
				        > RACER_WALL_CRASH_SPEED)
				    || (r->wheelieAngle != 0.0f
				        && (double)r->speed
				           > RACER_WHEELIE_CRASH_SPEED)) {
					(void)racer_begin_crash(r, gb, dir);
					r->riderStateBits |= 2;
				} else {
					r->riderStateBits |= 1;
				}
			} else {
				(void)racer_begin_crash(r, gb, dir);
				r->riderStateBits |= 2;
			}

			/*
			 * A QUARTER OF THE SPEED GOES INTO THE RAIL BEFORE
			 * THE BRANCH - 0x004692b3 stores it back at
			 * 0x004692d3 - so both arms below work on the
			 * damped value. Applying it afterwards leaves the
			 * speed right, because 0.75 and 0.3 commute, and
			 * the slide a third too high.
			 */
			r->speed = (float)((double)r->speed
			                   * RACER_WALL_DAMP);

			/*
			 * A GLANCE BOUNCES; A HEAD-ON SCRAPES, and the test
			 * at 0x004692e4 is THREE-DIMENSIONAL: the reflected
			 * direction dotted against the frame's ORIGINAL
			 * move, against 0.0.
			 *
			 * THE VERTICAL TERM DECIDES REAL IMPACTS. The bike
			 * is falling every frame and the reflection only
			 * negates the component along the wall normal, so
			 * the y term survives untouched and always pushes
			 * the dot positive. Measured on one impact at two
			 * angles: square on, -77.4 (scrape both ways); at
			 * 50 degrees, +30.1 in 3D but -3.4 in the
			 * horizontal plane alone. Drop y and the second
			 * one scrapes when MOTO.EXE bounces it.
			 */
			if (dir[0] * move[0] + dir[1] * move[1]
			    + dir[2] * move[2] >= 0.0f) {
				for (i = 0; i < 3; i++)
					r->moveDir[i] = dir[i];
				r->speed = (1.0f - impactAbs) * r->speed;
				r->speed = (float)((double)r->speed
				                   * RACER_WALL_BOUNCE);

				/*
				 * AND THE SLIP IS ZEROED ON A BOUNCE.
				 *
				 * ORIGIN  maybe_Racer_UpdateGroundContact
				 *         @ 0x00468e30, at 0x00469388
				 *
				 * THE ARGUMENT IS A LITERAL 0, so the lateral
				 * velocity is set to ZERO and the bike leaves
				 * the rail along the reflected direction with
				 * nothing sideways surviving. Read speed *
				 * 0.9 as the argument instead and it looks
				 * like a bike slammed sideways at ninety per
				 * cent of its speed; the tell is that both
				 * stores land on racer+0x124.
				 * racers.txt 9.18.23.3.
				 *
				 * The SECOND of Racer_SetSlipSpeed's two call
				 * sites; the first is in
				 * racer_apply_cornering.
				 *
				 * UNTESTED AND UNTESTABLE HERE: the branch
				 * needs a wall impact, and a hook on it took
				 * zero hits across twelve frames of braking
				 * in a turn. Transcribed from the code, not
				 * from behaviour. racers.txt 9.18.23.2.
				 */
				physics_set_slip_speed(r->matOrientation,
				                       r->velDir, &r->speed,
				                       c->realSpeedMax, 0.0f);
			} else {
				float was = r->speed;

				/*
				 * THE DIRECTION IS LEFT ALONE. The bike
				 * keeps pointing where it was and loses
				 * most of its speed - a scrape along the
				 * rail rather than a bounce off it.
				 */
				r->collideSpeed = (float)((double)was
				                    * RACER_WALL_SLIDE);
				if ((double)r->collideSpeed < RACER_WALL_SLIDE_MIN)
					r->collideSpeed = (float)RACER_WALL_SLIDE_MIN;
				r->speed = (float)((double)was
				                   * RACER_WALL_SCRAPE);
			}
			if ((double)r->speed < RACER_SPEED_1KMH)
				r->speed = (float)RACER_SPEED_1KMH;

			/*
			 * THE DRIVETRAIN RESYNC AFTER AN IMPACT, whole.
			 *
			 * ORIGIN  maybe_Racer_UpdateGroundContact @ 0x00468ca0,
			 *         at 0x004693aa..0x004693cf
			 *
			 *     004693aa  CALL Racer_SetLongSpeed   (speed)
			 *     004693b1  CALL Gearbox_SyncRoadSpeed
			 *     004693ca  CALL Gearbox_SpeedToEngine
			 *     004693cf  FSTP [ESI+0xec]           engineSpeed
			 *
			 * THE MIDDLE CALL IS THE ONE THAT MATTERS, and omitting
			 * it is invisible for exactly one frame: the impact
			 * lowers speed but not gearboxRoadSpeed, and the next
			 * frame rebuilds speed from it, handing back everything
			 * the strike took. racers-convergence.txt 9.66.2 has the rule this
			 * is one of the two sites for.
			 *
			 * SpeedToEngine then keeps the engine where it is,
			 * because a shift is pending across the impact and its
			 * disengaged branch passes the engine through - which is
			 * why MOTO's engine free-revs on through the strike.
			 *
			 * The 200.0 stored to instance+0x31d1 at 0x004693d5 is
			 * NOT transcribed. [open]
			 */
			physics_set_long_speed(r->matOrientation, r->velDir,
			                       &r->speed, c->realSpeedMax,
			                       r->speed);
			physics_sync_road_speed(&r->gearboxRoadSpeed, r->speed,
			                        c->realSpeedMax);
			r->engineSpeed = physics_speed_to_engine(c, gb->gear,
			                                         &r->gearboxRoadSpeed,
			                                         r->engineSpeed,
			                                         gb->state != 0);
		}


		r->flImpactBlend = RACER_IMPACT_BLEND;
		r->stuntOrOverspeed = 0;
		if (r->humanPlayer != 0 && r->soloMode != 0)
			r->stuntHeld = 0;
	}

	/*
	 * THE RAIL CONTAINMENT, every frame, before the ground height -
	 * which is the order the original has it at 0x00469409 and
	 * 0x0046941e.  It clamps the position back inside the segment's
	 * two wall planes; without it repeated shoves at a rail dig the
	 * bike progressively into the mesh.  trk.c has the whole of it.
	 *
	 * ON segmentStart, NOT segment: the original passes
	 * racer+0x31c1, the same field it hands Trk_GroundHeightAt on
	 * the next line.
	 *
	 * AND IT IS THE SECOND THING nowalls HAS TO SWITCH OFF. The
	 * clip above stops the bike CROSSING a rail; this clamps it
	 * back inside one it is already past, every frame. Gate only
	 * the clip and nowalls appears to do nothing, because the
	 * containment goes on holding the bike against the wall - and
	 * it reads as a regression in the clip, since nowalls worked
	 * before the containment existed.
	 */
	if (!bNoWalls)
		trk_contain_point(t, r->segmentStart, r->position);

	groundY = trk_ground_height_at(t, r->segmentStart, r->position);

	/*
	 * AIRBORNE IS 32 ABOVE THE GROUND, OR 8 UNDER THROTTLE. Both
	 * decompile as tiny floats - 4.48416e-44 and 1.12104e-44 - which
	 * are the INT bit patterns 32 and 8 seen through a FILD, not
	 * constants of that value.
	 *
	 * THE FLAG IS THE ANALOG THROTTLE AXIS, not a boolean: the
	 * original carries racer+0x31f1 down three call levels and then
	 * selects a ground tolerance with it. maybe_Game_ReadInput
	 * writes 0x8000 on every path a keyboard or pad takes, so the
	 * 32 arm is reachable only by a joystick throttle sitting at
	 * exactly its zero extreme. Kept as found rather than
	 * simplified to a constant 8 - see racer.h on the parameter.
	 */
	threshold = (float)(bLowAirThreshold ? GROUND_CLEARANCE_TIGHT
	                                     : GROUND_CLEARANCE);

	if (r->position[1] > groundY + threshold) {
		r->onGround = 0;
		if (r->humanPlayer != 0 && r->soloMode != 0)
			r->stuntHeld = 0;
		if (r->speed != 0.0f)
			(void)racer_orient_airborne(r, r->moveDir);
	} else {
		if (r->onGroundPrev == 0) {
			/* Touchdown. The compression is the overshoot. */
			r->springCompression =
			    (r->positionPrev[1] - groundY) * LAND_SPRING_SCALE;
			r->speed = (float)((double)r->speedKmhNeedle
			                   * RACER_KMH_TO_MS);
			/* 0x004694a4  the same drivetrain resync. */
			r->riderStateBits |= 0x80;
		}
		r->position[1] = groundY;
		r->onGround = 1;
		r->flAirTime = 0.0f;
		r->onGroundPair1 = 1;
		r->onGroundPair2 = 1;

		/*
		 * THE FRAME COMES FROM TWO GROUND PROBES, fore and aft
		 * along the forward row at half a wheelbase, each snapped
		 * to the road in its own segment. See the divergence note
		 * in the header on PersoAnim.
		 */
		racer_wheel_ground(r, c, rear, front);
		(void)racer_orient_grounded(r, t, r->segmentStart,
		                            front, rear);
	}

	/*
	 * BOTH WHEEL CONTACTS, from the frame just rebuilt. These carry
	 * the frame-height and wheel-radius lift that the probes above
	 * deliberately do not.
	 */
	racer_wheel_contacts(r, c, r->rearWheelContact,
	                     r->frontWheelContact);

	segRear = trk_find_segment_for_point(t, r->segmentStart,
	                                     r->rearWheelContact, NULL);
	r->f_2db0 = trk_ground_height_at(t, segRear, r->rearWheelContact);
	if (r->onGround != 0) {
		segFront = trk_find_segment_for_point(t, r->segmentStart,
		                                      r->frontWheelContact,
		                                      NULL);
		r->frontWheelContact[1] =
		    trk_ground_height_at(t, segFront, r->frontWheelContact);
		r->rearWheelContact[1] = r->f_2db0;
	}

	/*
	 * AND PAY OUT ONE STEP OF THE PENDING YAW - 0x00469bb0. A FIXED
	 * RATE, not a decay, clamped to zero on the crossing.
	 *
	 * racer+0x2e31 IS NOT A LEAN TARGET despite the DB's field name.
	 * It is a pending heading correction in radians, set on a wall
	 * hit and paid out until it runs out - which is what turns a
	 * bike that has hit a rail until it runs along it. See
	 * docs/subsystems/racers.txt 9.29.12.3.
	 */
	if (r->flLeanTarget != 0.0f) {
		float step;

		if (r->flLeanTarget > 0.0f) {
			step = (float)((double)r->frameDelta
			               * RACER_YAW_DRAIN);
			r->flLeanTarget -= step;
			if (r->flLeanTarget < 0.0f)
				r->flLeanTarget = 0.0f;
		} else {
			step = (float)((double)r->frameDelta
			               * -RACER_YAW_DRAIN);
			r->flLeanTarget -= step;
			if (r->flLeanTarget > 0.0f)
				r->flLeanTarget = 0.0f;
		}
		/*
		 * NEGATED, and this is the convention seam. flLeanTarget is
		 * stored in Racer_GetYaw's sign, which runs opposite to
		 * ours - MOTO holds -81.09 while turning the heading UP
		 * from -90 toward 0. Our value matches its byte for byte;
		 * only the direction it is applied in has to flip.
		 *
		 * DOING IT HERE, rather than negating the recovery's return,
		 * keeps the field itself comparable with MOTO in an A/B -
		 * which is how the inversion was caught: same pending, one
		 * heading climbing to 0 and the other running away from it.
		 */
		racer_compose_yaw(r->matOrientation, -step);
	}

	return 1;
}


/*
 * racer_update_crash - one frame of the rider off the bike.
 *
 * ORIGIN  maybe_Racer_UpdateCrash @ 0x004683e0
 *
 * IMPORT 3.
 *
 * THE WHOLE BODY IS GATED ON crashLatch, so nothing here runs
 * unless racer_begin_crash has latched it.
 *
 * THE AIRBORNE TEST IS NOT A PLAIN COMPARE. The ground height is
 * TRUNCATED and 8 added as an INTEGER, so the rider is airborne
 * only while pos.y >= (int)groundY + 8 - and the landing snaps
 * him to the truncated height, not the real one.
 *
 * THE BOUNCE RESTITUTION IS 0.01, so he hits once and stays down.
 *
 * IT DOES NOT RESET HIM ITSELF. The original calls
 * Racer_ResetCrash, which walks the whole field and needs the
 * racer array; rule 13 puts that in race.c as race_reset_crashes.
 * So this returns RACER_CRASH_DONE and the caller runs the
 * cascade - the one structural change made on purpose.
 *
 * THE ROOT MATRIX IS TOUCHED TWICE A FRAME and the two are easy to
 * confuse. The grounded arm RE-LAYS it flat from its own yaw
 * (racer_lay_rider_root, 0x00468640); the tail then composes the
 * free tumble onto whatever that left. So in the air the tumble
 * accumulates and on the ground it is reset under the tumble every
 * frame, leaving only the yaw rate visible.
 *
 * STILL A MARKED HOLE: the smoke at 0x004686f6, and the gearbox
 * resync at 0x00468a6a. [open]
 */
int racer_update_crash(MR_Racer *r, const MR_Trk *t)
{
	float from[3];
	float hitPoint[3];
	float dir[3];
	float normal[3];
	float dt;
	float groundY;
	float len;
	int   seg;
	int   i;

	if (!r || !t || r->crashLatch == 0)
		return RACER_CRASH_NONE;

	r->flCrashTimerMs -= r->frameDelta;
	if (r->flCrashTimerMs < 0.0f)
		return RACER_CRASH_DONE;

	dt = r->frameDelta * CRASH_DT_SCALE;

	for (i = 0; i < 3; i++)
		from[i] = r->riderPosition[i];

	/* pos += launchDir * riderSpeed * 128 * dt */
	for (i = 0; i < 3; i++)
		r->riderPosition[i] +=
	        (float)((double)r->crashLaunchDir[i] * r->flRiderSpeed
	                * CRASH_LAUNCH_SCALE * dt);

	seg = trk_find_segment_for_point(t, r->segmentStart,
	                                 r->riderPosition, NULL);
	for (i = 0; i < 3; i++)
		dir[i] = r->crashLaunchDir[i];
	if (trk_clip_move_against_walls(t, seg, from, r->riderPosition,
	                                hitPoint, dir, normal)) {
		for (i = 0; i < 3; i++)
			r->riderPosition[i] = hitPoint[i];
		seg = trk_find_segment_for_point(t, r->segmentStart,
		                                 r->riderPosition, NULL);
		r->flRiderSpeed = (float)((double)r->flRiderSpeed
		                          * CRASH_WALL_DAMP);
	}

	groundY = trk_ground_height_at(t, seg, r->riderPosition);

	if (r->riderPosition[1]
	    >= (float)((int)groundY + CRASH_AIR_CLEARANCE)) {
		/* Fold the speed in, add gravity, take it back out. */
		for (i = 0; i < 3; i++)
			r->crashLaunchDir[i] *= r->flRiderSpeed;
		r->crashLaunchDir[1] =
		    (float)((double)r->crashLaunchDir[1]
		            + r->frameDelta * CRASH_GRAVITY);
		len = (float)sqrt((double)
		          (r->crashLaunchDir[0] * r->crashLaunchDir[0]
		           + r->crashLaunchDir[1] * r->crashLaunchDir[1]
		           + r->crashLaunchDir[2] * r->crashLaunchDir[2]));
		r->flRiderSpeed = len;
		if (len != 0.0f) {
			for (i = 0; i < 3; i++)
				r->crashLaunchDir[i] /= len;
		}
		r->flRiderSpeed = (float)((double)r->flRiderSpeed
		                          + dt * CRASH_AIR_DRAG);
	} else {
		r->riderPosition[1] = (float)(int)groundY;
		r->crashLaunchDir[1] =
		    (float)(-(double)r->crashLaunchDir[1] * CRASH_BOUNCE);
		len = (float)sqrt((double)
		          (r->crashLaunchDir[0] * r->crashLaunchDir[0]
		           + r->crashLaunchDir[1] * r->crashLaunchDir[1]
		           + r->crashLaunchDir[2] * r->crashLaunchDir[2]));
		if (len != 0.0f) {
			for (i = 0; i < 3; i++)
				r->crashLaunchDir[i] /= len;
		}

		/*
		 * HE IS DOWN, SO PUT HIM DOWN FLAT. 0x00468640 rebuilds
		 * the rider root from its own yaw and the same -pi/2 lay
		 * BeginCrash used, every frame he is touching the
		 * ground - so the free tumble below only ever runs in
		 * the air.
		 */
		racer_lay_rider_root(r);

		/*
		 * AND THE ROLL RATE DIES ON CONTACT. Zeroing f_30e1[2]
		 * here is permanent - nothing writes it again before
		 * ResetCrash - so after the first touch the only rate
		 * left is the yaw, which is what slides him round on
		 * the spot rather than rolling him through the track.
		 */
		r->f_30e1[2] = 0.0f;

		r->flRiderSpeed = (float)((double)r->flRiderSpeed
		                          + dt * CRASH_GROUND_DRAG);
		if ((double)r->crashLaunchDir[1] > CRASH_FX_THRESHOLD) {
			/*
			 * THE RIDER HITS. Four particles through the
			 * RIDER'S frame, life 7 or 11, then the bit at
			 * 0x0046870d.
			 *
			 * IT IS NOT A ONE-SHOT. Nothing in UpdateCrash
			 * tests bit 0x40 - the only access to
			 * riderStateBits in the whole function is that
			 * OR - and crashLaunchDir.y is rewritten every
			 * frame at 0x0046856a. So the burst fires on
			 * each frame the rider is coming down hard,
			 * which is what makes a tumble trail dust
			 * rather than puff once.
			 */
			fx_burst(4, r->riderPosition, 7, 4u,
			         r->matRiderRoot);
			r->riderStateBits |= 0x40;
		}
	}
	if (r->flRiderSpeed < 0.0f)
		r->flRiderSpeed = 0.0f;

	/*
	 * THE TUMBLE. f_30e1 holds the per-axis rates that
	 * racer_begin_crash set to (0, fallSide, fallSide) - so the
	 * rider rolls and yaws about the side he went down, and never
	 * pitches. They are scaled by the PROGRESS FRACTION and
	 * composed onto matRiderRoot:
	 *
	 *     rate = f_30e1 * (flRiderSpeed / flRiderSpeedRef) * dt
	 *     matRiderRoot = matRiderRoot * Euler(rate)
	 *
	 * SO HE SPINS FAST WHEN THROWN AND STOPS AS HE STOPS SLIDING,
	 * because flRiderSpeed decays to zero against a
	 * flRiderSpeedRef that never changes. One ratio does the whole
	 * animation curve - there is no separate timer for it.
	 *
	 * THE ROTATION IS ON THE RIGHT, and the two orders are not
	 * interchangeable. 0x004689xx multiplies each of the four rows
	 * of matRiderRoot by the same 3x3, which is
	 * matRiderRoot * Euler - the rates are read in the RIDER'S own
	 * frame. Reverse the operands and a yaw rate about his up axis
	 * becomes a world-axis tilt: grounded, with rate (0, y, 0),
	 * the right order slides his heading round and the wrong one
	 * lifts him out of the road.
	 *
	 * The original composes the three angle-table lookups by hand
	 * rather than calling the euler builder; mat34_from_euler is
	 * that builder, with the same table and the same __ftol, so
	 * the two agree to the bit.
	 */
	if (r->flRiderSpeedRef != 0.0f) {
		float rate[3];
		float rot[9];
		float step[12];
		float out[12];
		float k = (r->flRiderSpeed / r->flRiderSpeedRef) * dt;

		for (i = 0; i < 3; i++)
			rate[i] = r->f_30e1[i] * k;
		mat34_from_euler(rot, rate);

		for (i = 0; i < 9; i++)
			step[i] = rot[i];
		step[9] = step[10] = step[11] = 0.0f;

		mat34_multiply(out, r->matRiderRoot, step);
		for (i = 0; i < 12; i++)
			r->matRiderRoot[i] = out[i];
	}

	/*
	 * STILL SLIDING, OR THE TIMER STILL HIGH? Then decay the
	 * bike's own speed; otherwise the rider is done.
	 *
	 * THE TIMER TEST IS AN INTEGER COMPARE ON A FLOAT BIT PATTERN
	 * - 0x42c7ffff is just under 100.0f - the same idiom the sign
	 * tests use, and it reads as dead code in the decompiler.
	 */
	if ((double)r->flRiderSpeed >= RACER_SPEED_1KMH
	    || r->flCrashTimerMs > CRASH_HOLD_MS) {
		if (r->humanPlayer == 0) {
			r->engineSpeed = (float)((double)r->engineSpeed
			                         * CRASH_DECAY);
		} else {
			r->speed  = (float)((double)r->speed * CRASH_DECAY);
			/* 0x00468a6a  the gearbox resync. A marked hole. */
			r->collideSpeed = (float)((double)r->collideSpeed * CRASH_DECAY);
		}
	} else {
		r->handSkeleton = 1;
		return RACER_CRASH_DONE;
	}

	r->handSkeleton = 1;
	return RACER_CRASH_ACTIVE;
}


/* ------------------------------------------------------------------ */
/*
 * THE INTEGRATOR'S CONSTANTS, read live. The instance offsets in the
 * comments are the ORIGINAL's; instance = racer + 0x24, so
 * instance+0x168 is racer+0x18c and so on.
 */
#define INT_DT_SCALE        0.01f      /* 0x004d1c10 */
#define INT_AIRTIME_GATE    2.592      /* 0x004d1c18 */
#define INT_RESYNC_EPS      0.001f     /* 0x004d1c80 */
#define INT_GRAVITY         24.525     /* 0x004d1c20, 2.5 * 9.81 */
#define INT_MOVE_SCALE      128.0      /* 0x004d1c30, units per metre */
#define INT_DAMP_K_ROAD     6.0        /* 0x004d1c38, PersoAnim 0 */
#define INT_DAMP_K_DIRT     11.0       /* 0x004d1c40 */
#define INT_DAMP_FLOOR      0.5        /* 0x004d1c48 */
#define INT_BLEND_SCALE     2.0f       /* 0x004d1c54 */
#define INT_BLEND_HI        0.9        /* 0x004d1c58 */
#define INT_BLEND_LO        0.85       /* 0x004d1c68 */
#define INT_SLIDE_DECAY     33.3333333 /* 0x004d1c88 */

/*
 * THE GROUNDED VERTICAL PUSH, and it is the answer to
 * port-divergences 2.13.
 *
 *     -1255.68  =  -128 * 9.81
 *
 * so it is gravity in the same 128-units-per-metre scale the
 * displacement uses. It is added STRAIGHT TO THE DISPLACEMENT rather
 * than integrated into the velocity, which is why the vertical step
 * measured off MOTO.EXE was a CONSTANT -41.4374 a frame whatever the
 * speed: dt is frameDelta * 0.01 = 0.033, and 0.033 * -1255.68 is
 * -41.437.
 *
 * OUR PORT USED THE AIRBORNE FORMULA HERE - racer_gravity_step, which
 * integrates 2.5 g into the polar velocity - and got about -3.4. That
 * is the whole of the 12x gap.
 */
#define INT_GROUND_PUSH     -1255.68   /* 0x004d1c78 */


/*
 * racer_integrate - the ordinary position advance.
 *
 * ORIGIN  ModelInstance_Integrate @ 0x0048d620
 *
 * IMPORT 5. maybe_Racer_Tick calls it at 0x0045d770 with ECX =
 * racer+0x24, the BODY instance, and instance+0x0c is racer+0x30 - so
 * moving the instance is what moves the racer. THIS is the mover;
 * UpdateGroundContact only reacts, which is why its wall clip takes
 * from = positionPrev and to = position.
 *
 * TWO BRANCHES, forked on flAirTime against 2.592:
 *
 *   GROUNDED  the heading is rebuilt from the matrix, moveDir is
 *             blended towards it, and the displacement is
 *             moveDir * speed * damp * 128 * dt with the constant
 *             downward push added.
 *
 *   AIRBORNE  gravity is integrated INTO the velocity at 2.5 g, the
 *             magnitude folded back into speed, and the displacement
 *             is the plain moveDir * speed * 128 * dt.
 *
 * AND BOTH ADD THE WALL SLIDE, in the tail.
 *
 * THE LOW-SPEED DAMPING reads its speed through a POINTER at
 * instance+0xe4, whose target is not identified - so r->speed is used
 * here on the strength of racers.txt 9.29.13, which derives the same
 * taper and its |v| = 12 floor for K = 6. [open - verify the pointer]
 *
 * THE velDir WRITE BELOW IS THE ORIGINAL'S, NOT A PLACEHOLDER.
 * ModelInstance_Integrate does the same thing at 0x0048d641: it
 * transforms (0,0,1) through matOrientation, normalises, and stores
 * the result to instance+0x10c..0x114 at 0x0048d6c5. So velDir IS
 * seeded from the heading, here and there, and the value it carries
 * afterwards is only as good as the matrix it was seeded from.
 *
 * THE MATRIX IT IS SEEDED FROM HAS TO CARRY PITCH. A yaw-only
 * matOrientation makes physics_slip_speed - row0 . velDir * speed -
 * return a STRUCTURAL zero, because row0 . row2 is zero by
 * construction, and the skid ribbon then never widens and the
 * MotoType 1 grip curve never leaves 1.0. racer_update_airborne is
 * what puts the pitch there.
 */
void racer_integrate(MR_Racer *r, const MR_BikeCarac *c, float frameDelta,
                     int bAllowLift)
{
	float dt;
	float heading[3];
	float disp[3];
	float speedBefore;
	float len;
	float damp;
	float blend;
	float scale;
	int   i;

	if (!r || !c)
		return;

	speedBefore = r->speed;
	dt = frameDelta * INT_DT_SCALE;

	/*
	 * THE HEADING IS THE MATRIX'S FORWARD, rebuilt every frame by
	 * transforming (0,0,1) - so the bars point the bike even while
	 * momentum carries it elsewhere.
	 */
	for (i = 0; i < 3; i++)
		heading[i] = r->matOrientation[6 + i];
	len = (float)sqrt((double)(heading[0] * heading[0]
	                           + heading[1] * heading[1]
	                           + heading[2] * heading[2]));
	if (len != 0.0f) {
		for (i = 0; i < 3; i++)
			heading[i] /= len;
	}
	for (i = 0; i < 3; i++)
		r->velDir[i] = heading[i];

	if ((double)r->flAirTime <= INT_AIRTIME_GATE) {
		/* ---- GROUNDED ---------------------------------------- */
		float dot = r->moveDir[0] * heading[0]
		            + r->moveDir[1] * heading[1]
		            + r->moveDir[2] * heading[2];

		/* Travelling backwards against the heading costs speed. */
		if (dot < 0.0f)
			r->speed *= (dot < 0.0f) ? -dot : dot;

		/*
		 * THE LOW-SPEED DAMPING, in [0.5, 1.0]: full step when
		 * fast, half when crawling, so a stationary bike does not
		 * lurch. K is 6 on the road and 11 on the dirt.
		 */
		{
			/*
			 * IT IS THE TURN RADIUS, not a speed. The original reads
			 * it through a pointer at instance+0xe4 - which points at
			 * the RACER ITSELF, so +0x18 is racer+0x18, turnRadius.
			 *
			 * So this is a CORNER damping: a tight radius shortens the
			 * step, a straight line does not touch it. K is 6 on the
			 * road and 11 on the dirt, so a dirt bike loses more in a
			 * tight corner.
			 *
			 * THE DAMPING IS REAL AND SMALL.
			 * maybe_Racer_ApplyCornering fits racer+0x18 every frame
			 * the bike is moving - MOTO reads 43.63, 55.00 and
			 * 104.21 in three separate corners - so with K = 6 the
			 * factor is 0.942 at 104.21 and 0.862 at 43.63.
			 *
			 * READING +0x18 AS A SPEED LANDS NEAR 1.0, which is
			 * close enough to hide the substitution. It is a radius.
			 * A watchpoint on it can also miss the writes and
			 * suggest the field is never touched -
			 * whole-function-import.txt 6.2.
			 */
			double v = (double)r->turnRadius[0];
			double k = (c->persoAnim == 0) ? INT_DAMP_K_ROAD
			                               : INT_DAMP_K_DIRT;

			if (v < 0.0)
				v = -v;
			damp = (float)(1.0 - (v != 0.0 ? k / v : k));
			if (damp > 1.0f)
				damp = 1.0f;
			else if ((double)damp < INT_DAMP_FLOOR)
				damp = (float)INT_DAMP_FLOOR;
		}

		/*
		 * THE SLIP BLEND, clamped to [0.85, 0.9]: moveDir eases
		 * towards the heading rather than snapping to it, which is
		 * what a bike sliding out of a corner does.
		 */
		blend = (c->realSpeedMax > 0.0f)
		        ? (r->speed / c->realSpeedMax) * INT_BLEND_SCALE
		        : 0.0f;
		if ((double)blend > INT_BLEND_HI)
			blend = (float)INT_BLEND_HI;
		if ((double)blend < INT_BLEND_LO)
			blend = (float)INT_BLEND_LO;

		for (i = 0; i < 3; i++)
			r->moveDir[i] = heading[i] * (1.0f - blend)
		                    + r->moveDir[i] * blend;
		/*
		 * AND Y IS PUT BACK UNBLENDED at 0x0048da8f - the original
		 * overwrites it with the heading's own y before
		 * renormalising. Y does not enter the yaw, so this only
		 * matters if the full 3D direction is reconstructed.
		 */
		r->moveDir[1] = heading[1];

		len = (float)sqrt((double)(r->moveDir[0] * r->moveDir[0]
		                           + r->moveDir[1] * r->moveDir[1]
		                           + r->moveDir[2] * r->moveDir[2]));
		if (len != 0.0f) {
			for (i = 0; i < 3; i++)
				r->moveDir[i] /= len;
		}

		scale = (float)((double)r->speed * damp * INT_MOVE_SCALE * dt);
		for (i = 0; i < 3; i++)
			disp[i] = r->moveDir[i] * scale;

		/*
		 * THE PUSH DOWN. Applied when the caller does not allow a
		 * lift, or when the step is already downward - so a bike
		 * on the ground is pressed onto it every frame and the
		 * ground snap in UpdateGroundContact takes it back.
		 */
		if (!bAllowLift || disp[1] <= 0.0f)
			disp[1] = (float)((double)disp[1]
		                      + (double)dt * INT_GROUND_PUSH);

		/*
		 * AND THE WALL SLIDE IS FLATTENED - 0x0048da0e, the block
		 * gated on collideSpeed being non-zero. Its Y is replaced by the
		 * HEADING's Y, signed to agree with the slide's own
		 * direction, and the vector renormalised.
		 *
		 * WITHOUT IT THE SLIDE PUSHES THE BIKE INTO THE GROUND.
		 * The direction stored at the wall hit is the clip's
		 * reflected 3D vector, which carries the frame's downward
		 * step: measured (-0.92, -0.38, 0) on ours against MOTO's
		 * (-1.00, 0.00, 0.00) for the same impact. Two fifths of
		 * the escape push goes straight down, which pins the bike
		 * to the rail with the gas on.
		 */
		if (r->collideSpeed != 0.0f) {
			float *slide = r->collideDir;
			float  sy    = heading[1];

			if (slide[0] * heading[0] + slide[1] * heading[1]
			    + slide[2] * heading[2] < 0.0f)
			    sy = -heading[1];
			slide[1] = sy;

			len = (float)sqrt((double)(slide[0] * slide[0]
			                           + slide[1] * slide[1]
			                           + slide[2] * slide[2]));
			if (len != 0.0f) {
				for (i = 0; i < 3; i++)
					slide[i] /= len;
			}
		}
	} else {
		/* ---- AIRBORNE ---------------------------------------- */
		len = (float)sqrt((double)(r->moveDir[0] * r->moveDir[0]
		                           + r->moveDir[1] * r->moveDir[1]
		                           + r->moveDir[2] * r->moveDir[2]));
		if (len != 0.0f) {
			for (i = 0; i < 3; i++)
				r->moveDir[i] /= len;
		}

		/* Fold the speed in, add gravity, take the magnitude back. */
		for (i = 0; i < 3; i++)
			r->moveDir[i] *= r->speed;
		r->moveDir[1] = (float)((double)r->moveDir[1]
		                        - (double)dt * INT_GRAVITY);
		r->speed = (float)sqrt((double)
		               (r->moveDir[0] * r->moveDir[0]
		                + r->moveDir[1] * r->moveDir[1]
		                + r->moveDir[2] * r->moveDir[2]));
		if (r->speed != 0.0f) {
			for (i = 0; i < 3; i++)
				r->moveDir[i] /= r->speed;
		}

		scale = (float)((double)r->speed * INT_MOVE_SCALE * dt);
		for (i = 0; i < 3; i++)
			disp[i] = r->moveDir[i] * scale;

		if (r->speed > c->realSpeedMax)
			r->speed = c->realSpeedMax;
	}

	/*
	 * THE WALL SLIDE, on BOTH branches. racer+0x174 is the direction
	 * the wall clip wrote and collideSpeed its speed - so a bike that has
	 * scraped a rail keeps being pushed along it, fading out at
	 * dt * 33.333 a frame. This is what carries you down a wall
	 * instead of stopping against it.
	 */
	scale = (float)((double)r->collideSpeed * INT_MOVE_SCALE * dt);
	for (i = 0; i < 3; i++)
		disp[i] += r->collideDir[i] * scale;

	/* The Euler add. 0x0048dc23. */
	for (i = 0; i < 3; i++) {
		*(float *)(r->unk_00fc + i * 4) = disp[i];
		r->position[i] += disp[i];
	}

	/*
	 * THE GEARBOX RESYNC. 0x0048dd05.
	 *
	 * ORIGIN  ModelInstance_Integrate @ 0x0048d620, at 0x0048dd05
	 *
	 *     if (|speed - speedAtEntry| > 0.001 && !airborne) {
	 *         long = maybe_Racer_LongSpeed()          0x0048ca10
	 *         k    = FUN_0048eb40()                   0x0048eb40
	 *         instance+0x108 += (long - instance+0x108) * k
	 *     }
	 *
	 * FUN_0048eb40 IS THE GRIP CURVE - the same function
	 * physics_grip already is. It transforms velDir * speed into
	 * body space, takes the LATERAL component, and for MotoType 1
	 * returns 1 - 0.05*|slip| between 0.01 and 20; for every other
	 * bike it returns max(1.0, MinAdherenceWheel).
	 *
	 * SO ON A ROAD BIKE k IS 1.0 AND THE LERP IS A PLAIN COPY -
	 * gearboxRoadSpeed becomes the longitudinal velocity exactly.
	 *
	 * THIS IS HOW THE DRAG REACHES THE ROAD SPEED. The drag is
	 * subtracted from speed at 0x00490090 and nothing else carries
	 * it; LongSpeed reads velDir and speed, so the reduced speed
	 * arrives here one tick later. See racers.txt 9.18.19.
	 *
	 * THE GATE IS ON THE FRAME'S SPEED CHANGE, 0.001 at 0x004d1c80,
	 * and on the airborne test at instance+0xcc - a bike in the air
	 * keeps its road speed.
	 */
	if (r->flAirTime == 0.0f
	    && (float)fabs((double)(r->speed - speedBefore)) > INT_RESYNC_EPS) {
		const MR_BikeCarac *bc = bike_at(r->bikeIndex);

		if (bc && bc->loaded) {
			float lng = physics_long_speed(r->matOrientation,
			                               r->velDir, r->speed);
			float slp = physics_slip_speed(r->matOrientation,
			                               r->velDir, r->speed);
			float k   = physics_grip(bc, (slp < 0.0f) ? -slp : slp);

			r->gearboxRoadSpeed += (lng - r->gearboxRoadSpeed) * k;
		}
	}

	/* And the slide fades. 0x0048dd05 tail. */
	if (r->collideSpeed > 0.0f) {
		r->collideSpeed = (float)((double)r->collideSpeed
		                    - (double)dt * INT_SLIDE_DECAY);
		if (r->collideSpeed < 0.0f)
			r->collideSpeed = 0.0f;
	} else if (r->collideSpeed < 0.0f) {
		r->collideSpeed = (float)((double)r->collideSpeed
		                    + (double)dt * INT_SLIDE_DECAY);
		if (r->collideSpeed > 0.0f)
			r->collideSpeed = 0.0f;
	}
}


/* ================================================================ */
/* Per-racer audio - maybe_Snd_TickRacer                            */
/* ================================================================ */

/*
 * IT BELONGS IN racer.c BY RULE 13'S OWN TEST - it operates on a
 * single MR_Racer and does nothing else: one call per racer per
 * frame, reading that racer and its carac. audio.c owns the voice
 * layer; this owns what a bike sounds like.
 *
 * THIS IS A SKELETON, in the sense rule 15.1 allows. The whole
 * function is not here yet. What IS here is the engine note, derived
 * end to end from the disassembly; every other voice the original
 * drives is named here with its cue so the gap is visible rather than
 * silently absent:
 *
 *   0x11 roll loop, 0x12 crash, 0x0e/0x0f impact, 0x0b..0x0d
 *   landing, 0x13 wheel thump, 0x14..0x16 water, 0x17..0x19
 *   surfaces, 0x1c scrape, 0x1d..0x1f depart, 0x21 motostp,
 *   0x3d/0x4b the rider voice lines.
 *
 * Most of those hang off float expressions the decompiler renders as
 * a bare __ftol() with no arguments, so each needs its own pass over
 * the assembly. The engine note needed exactly that, and it is why
 * this one is finished and the others are not.
 */

/*
 * THE CUE PER GEAR. ORIGIN  0x004db618 (road) and 0x004db638 (dirt),
 * chosen at 0x004063ac on g_bikeCarac[bike].PersoAnim.
 *
 * ROAD MAPS ITS GEARS ONTO TWO SAMPLES because the Ninja ships only
 * Gear1 and Gear5. That is why the restart test compares the TABLE
 * ENTRY and not the gear - see racer_audio_tick.
 */
static const int kEngineCueRoad[8] = {
	0, 0x22, 0x22, 0x26, 0x26, 0x26, 0x26, 0
};
static const int kEngineCueDirt[8] = {
	0, 0x22, 0x23, 0x24, 0x25, 0x26, 0x26, 0xff
};

/*
 * THE PITCH RANGE PER GEAR, IN HERTZ. ORIGIN  0x004db5a8 (road) and
 * 0x004db5e0 (dirt), selected in the same two branches.
 *
 * SEVEN ENTRIES, NOT EIGHT: the dirt table ends exactly where the
 * road cue table at 0x004db618 begins, which is what fixes the
 * count. Index 7 of the cue tables above is therefore unreachable
 * for a pitch, and the caller requires gear > 0 anyway.
 *
 * THESE ARE ABSOLUTE PLAYBACK RATES AND THEY GO HIGH. Road gears 3
 * to 6 run to 40000 Hz on a 14000 Hz sample - a ratio of 2.86, above
 * XAudio2's DEFAULT ceiling of 2.0. xa2.cpp asks for 4.0 for exactly
 * this reason; without it the top of every gear would flatten out
 * with no error anywhere.
 */
typedef struct {
	int lo;
	int hi;
} MR_EngineRange;

static const MR_EngineRange kEngineHzRoad[7] = {
	{     0,     0 },
	{  8000, 48000 },
	{ 10000, 48000 },
	{  2000, 40000 },
	{  2000, 40000 },
	{  2000, 40000 },
	{  2000, 40000 }
};
static const MR_EngineRange kEngineHzDirt[7] = {
	{     0,     0 },
	{  9000, 29000 },
	{  1000, 26000 },
	{  1000, 26000 },
	{  1500, 16000 },
	{  1000, 16000 },
	{  1000, 16000 }
};

/* The rpm fraction is clamped to this floor - 0x004d0190. */
#define RACER_AUDIO_RPM_MIN    0.05f

/* record[0x13], stepped 1000 a frame toward 3000 while the surface
 * class is 3 and back down otherwise. */
#define RACER_AUDIO_DRIFT_STEP 1000

/*
 * THE REV BLIP. One draw in four on the THROTTLE'S RISING EDGE
 * adds 2000 plus another draw modulo 3000 [0x0040714b]. It is
 * an edge, not a level - tickInputWord bit 0 set now and clear
 * last frame - so holding the throttle does not keep blipping.
 */
#define RACER_AUDIO_BLIP_ONE_IN  4
#define RACER_AUDIO_BLIP_BASE 2000     /* 0x7d0 */
#define RACER_AUDIO_BLIP_SPAN 3000     /* 0xbb8 */

/*
 * THE ENGINE VOLUME. 250 for anyone the player is not riding;
 * 255 while the throttle is down or a wheelie is up; and on a
 * trailing throttle a slow wander, 240 less a draw modulo 50,
 * re-rolled no oftener than every 50 ms on the road bike and
 * 100 ms on the dirt one [0x004071ff].
 */
#define RACER_AUDIO_VOL_AI     250      /* 0xfa */
#define RACER_AUDIO_VOL_ON     255
#define RACER_AUDIO_VOL_BASE   240      /* 0xf0 */
#define RACER_AUDIO_VOL_SPAN    50      /* 0x32 */
#define RACER_AUDIO_VOL_MS_ROAD 50
#define RACER_AUDIO_VOL_MS_DIRT 100

/* Every draw here is stream 4 - see rand.h on why that matters. */
#define RACER_AUDIO_RAND_STREAM 4

/*
 * THE THREE TERMS THE NOTE CARRIES BESIDES THE GEAR RAMP.
 *
 * The wheelie span is the SAME constant the skid pitch uses
 * (0x004d0228), and the slope divisor is BIKE_AXLE_K - the
 * same 48.0 that places the two wheel contact points in
 * bikes.h. Neither is a coincidence: the slope term is the
 * height difference between those very points.
 */
#define RACER_AUDIO_WHEELIE_SPAN 4000.0f  /* 0x004d0228 */
#define RACER_AUDIO_SLOPE_SPAN   1800.0f  /* 0x004d0240 */

/*
 * THE LOW-SPEED VOLUME RAMP, and the three numbers are
 * exactly continuous: the fade reaches full at 20 km/h,
 * which is the same speed that switches it off. 1/3.6 and
 * 3.6/19 are metres per second against km/h, so read the
 * pair as 1 km/h and 20 km/h.
 */
#define RACER_AUDIO_VOL_SPEED_FULL 5.5555556f  /* 0x004d0248 */
#define RACER_AUDIO_VOL_SPEED_ZERO 0.2777778f  /* 0x004d0250 */
#define RACER_AUDIO_VOL_SPEED_K    0.18947368f /* 0x004d0258 */

/*
 * THE IDLE LOOP - cue 0x21, 'motostp', and it is a SEPARATE
 * SAMPLE from the gear loops rather than one of them slowed
 * down. Below 29 km/h it runs alongside the gear loop, and
 * below 5 km/h the gear loop stops dead and leaves this one
 * holding the engine on its own - which is the rumble a
 * stationary bike makes.
 *
 * The two fades are written to cross: the gear loop reaches
 * full at 20 km/h and zero at 1, this one full at 10 km/h and
 * zero at 29. Both volume slopes are 19/3.6 wide, which is
 * what makes them the same curve twice.
 */
#define RACER_AUDIO_IDLE_CUE       0x21
#define RACER_AUDIO_IDLE_SPEED_ON  8.0555556f  /* 0x004d0208 */
#define RACER_AUDIO_IDLE_VOL_K    48.315789f   /* 0x004d0210 */
#define RACER_AUDIO_IDLE_HZ_SPAN   8000.0f     /* 0x004d0218 */
#define RACER_AUDIO_IDLE_HZ_BASE  16050.0f     /* 0x004d021c */
#define RACER_AUDIO_ENGINE_OFF     1.3888889f  /* 0x004d0220 */

/*
 * THE THROTTLE BLIP - the three DEPART samples. 'Depart' is the
 * race start, which is why it sounds like the menu's rev: it is
 * the same family of recording.
 */
#define RACER_AUDIO_DEPART_LO      0x1d
#define RACER_AUDIO_DEPART_HI      0x1f
#define RACER_AUDIO_DEPART_SPEED   5.0f        /* 0x004d0238 */
#define RACER_AUDIO_DEPART_VOL     200

/*
 * THE SKID VOICE'S NUMBERS, all read at 0x00406e0f onward.
 *
 * The pitch span is the interesting one: the loop is played
 * at its base rate plus a full 4000 Hz scaled by speed over
 * the BIKE'S OWN top speed, so a slow bike and a fast one
 * sound the same at their respective limits.
 */
#define RACER_AUDIO_SKID_MIN_SPEED  8.3333f  /* 0x004d01e8 */
#define RACER_AUDIO_SKID_RATE       22050    /* 0x5622     */
#define RACER_AUDIO_SKID_RATE_BRAKE 12050    /* 0x2f12     */
#define RACER_AUDIO_SKID_VOL        230      /* 0xe6       */
#define RACER_AUDIO_SKID_FADE_STEP  50       /* 0x32       */
#define RACER_AUDIO_SKID_PITCH_SPAN 4000.0f  /* 0x004d0228 */
#define RACER_AUDIO_SKID_RPM_FLOOR  0.2f     /* 0x004d0230 */

#define RACER_AUDIO_CUE_GRAVIER 0x17
#define RACER_AUDIO_CUE_GLACE   0x18
#define RACER_AUDIO_CUE_HERBE   0x19
#define RACER_AUDIO_CUE_BRAKE   0x1a    /* and 0x1b */
#define RACER_AUDIO_CUE_WHEEL   0x13    /* WHEEL, the thud */
#define RACER_AUDIO_WHEEL_SCALE  121.59437652220804  /* 004d0200 */
#define RACER_AUDIO_WHEEL_FLOOR  0x40                /* 00406bd1 */
#define RACER_AUDIO_WHEEL_CAP    MR_HALF_PI  /* 0x004d0158 */

/*
 * THE AUDIO SURFACE CLASS, and it is not the surface id.
 *
 * ORIGIN  maybe_Racer_SurfaceUnder @ 0x00465d20, then the remap
 *         at 0x00406400.
 *
 * Two steps, and the first is the one that surprises:
 *
 *   class = Trk_SurfaceAtPoint(rear wheel contact) + 9
 *
 * THE +9 IS NOT A SURFACE, IT IS THE FLAG. SurfaceUnder adds it
 * when its third argument is set, and Snd_TickRacer always sets
 * it - so the audio never sees a class below 9 before the remap.
 * The switch it feeds carries cases 3, 7 and 8 that nothing can
 * reach from here; they belong to the unflagged caller.
 *
 * Then four of the nine fold back down:
 *
 *    9 -> 6   surface 0, the road   - the BRAKE squeal
 *   11 -> 5   surface 2             - gravier
 *   12 -> 4   surface 3             - herbe
 *    1 -> 2   unreachable with the flag set
 *
 * and 10 (surface 1) and 13 (surface 4) pass through unchanged.
 *
 * IN THE AIR IT IS ZERO, which silences every surface voice -
 * that test comes before the remap.
 */
#define RACER_AUDIO_SURFACE_FLAG 9

int racer_audio_surface(const MR_Racer *r, const MR_Trk *trk)
{
	int c;

	if (!r || !trk || !r->onGround)
		return 0;

	c = trk_surface_at_point(trk, r->segmentStart, r->rearWheelContact)
	    + RACER_AUDIO_SURFACE_FLAG;

	switch (c) {
	case 1:  return 2;
	case 9:  return 6;
	case 11: return 5;
	case 12: return 4;
	default: break;
	}
	return c;
}

/*
 * ORIGIN  NONE - ours. See racer.h on why this is separate
 * from racer_audio_reset.
 *
 * EVERY VOICE FIELD MUST APPEAR HERE. Adding one and
 * forgetting this line leaves it naming voice 0, which is a
 * real slot and will be stopped out from under whatever owns
 * it. A positional initialiser goes stale silently, which is
 * why the record is not built from one.
 */
void racer_audio_init(MR_RacerAudio *a)
{
	if (!a)
		return;
	memset(a, 0, sizeof *a);
	a->idle     = MR_VOICE_NONE;
	a->engine   = MR_VOICE_NONE;
	a->depart   = MR_VOICE_NONE;
	a->skid     = MR_VOICE_NONE;
	a->prevGear = -1;
}

/* ORIGIN  FUN_00406190 plus the reset block at 0x004062c8. */
void racer_audio_reset(MR_RacerAudio *a)
{
	if (!a)
		return;
	audio_voice_stop(a->engine);
	audio_voice_stop(a->idle);
	audio_voice_stop(a->depart);
	audio_voice_stop(a->skid);
	racer_audio_init(a);
}

/* ORIGIN  maybe_Snd_TickRacer @ 0x00406290 - the engine note and
   the skid voice. */
void racer_audio_tick(MR_RacerAudio *a, const MR_Racer *r,
                      const MR_BikeCarac *c, const MR_Trk *trk,
                      int master, float pan, int playerVoices,
                      float velocityPitch)
{
	const int            *cue;
	const MR_EngineRange *range;
	int                   dirt;
	int                   gear;
	float                 rpm;
	int                   hz;
	int                   vol;
	int                   brakeBit;
	int                   idleVol;

	if (!a || !r || !c)
		return;

	/*
	 * A RISING EDGE ON crashLatch SILENCES THE BIKE COMPLETELY and
	 * it restarts from idle. The original drops all nine voice
	 * slots and a dozen counters; ours has one voice to drop.
	 */
	if (r->crashLatch == 0 && a->prevCrash != 0)
		racer_audio_reset(a);

	a->master = master;

	/*
	 * THE DRIVER'S FLAG IS ANDed WITH humanPlayer, not trusted
	 * outright [0x004063f2]. So an AI bike loses these voices
	 * even if a caller asks for them.
	 */
	playerVoices = playerVoices && r->humanPlayer;

	dirt  = (c->persoAnim != 0);
	cue   = dirt ? kEngineCueDirt : kEngineCueRoad;
	range = dirt ? kEngineHzDirt  : kEngineHzRoad;

	/*
	 * THE RPM FRACTION, and it is the whole of the pitch input:
	 * tachoRpm over the bike's own redline, clamped to [0.05, 1.0]
	 * [verified, 0x004063d6 onward]. The floor is not a guard - a
	 * stopped engine still turns the sample over slowly.
	 */
	rpm = (c->regimeMax > 0.0f)
	          ? ((float)r->tachoRpm / c->regimeMax) : 0.0f;
	if (rpm > 1.0f)
		rpm = 1.0f;
	else if (rpm < RACER_AUDIO_RPM_MIN)
		rpm = RACER_AUDIO_RPM_MIN;

	gear = r->gear;

	/*
	 * ORIGIN  maybe_Snd_TickRacer @ 0x00406290, at 0x00406b72
	 *
	 * THE VOLUME IS THE PEAK, NOT THE CURRENT ANGLE: this fires
	 * on the frame the angle reaches zero, so reading the angle
	 * here is silent every time.  See audio-cues.txt 9.14.
	 */
	if (a->prevWheelie != 0.0f && r->wheelieAngle == 0.0f &&
	    r->onGround != 0) {
		MR_Voice v;
		int      wv;

		if ((double)a->peakWheelie > RACER_AUDIO_WHEEL_CAP)
			a->peakWheelie = (float)RACER_AUDIO_WHEEL_CAP;

		wv = (int)((double)a->peakWheelie *
		           RACER_AUDIO_WHEEL_SCALE)
		     + RACER_AUDIO_WHEEL_FLOOR;

		v = audio_voice_start(RACER_AUDIO_CUE_WHEEL,
		                      SND_FLAG_POLYPHONIC);
		audio_voice_set_volume(v, (master * wv) >> 8);
		audio_voice_set_pan(v, pan);
		a->peakWheelie = 0.0f;
	}

	/*
	 * THE IDLE LOOP, record[8] at +0x20.
	 *
	 * ORIGIN  maybe_Snd_TickRacer @ 0x00406290, at 0x00406c10
	 *
	 * THIS RUNS BEFORE THE GEAR LOOP AND THAT IS STRUCTURAL,
	 * not tidiness: below 5 km/h it sets state 2, and state 2
	 * is what stops the gear loop from starting below.
	 */
	if (r->speed >= RACER_AUDIO_IDLE_SPEED_ON) {
		if (a->idle != MR_VOICE_NONE) {
			audio_voice_stop(a->idle);
			a->idle = MR_VOICE_NONE;
			if (a->state == 2)
				a->state = 0;
		}
	} else {
		if (a->idle == MR_VOICE_NONE) {
			a->idle = audio_voice_start(
			        RACER_AUDIO_IDLE_CUE,
			        SND_FLAG_LOOP | SND_FLAG_POLYPHONIC);
			mrlog("idle: motostp cue 0x%02x, voice %d\n",
			      RACER_AUDIO_IDLE_CUE, (int)a->idle);
		}

		/*
		 * Loudest standing still, gone by 29 km/h. The
		 * clamp is the original's - the product reaches
		 * 389 at a standstill.
		 */
		idleVol = (int)((RACER_AUDIO_IDLE_SPEED_ON - r->speed) *
		                RACER_AUDIO_IDLE_VOL_K);
		if (idleVol > 255)
			idleVol = 255;
		if (idleVol < 0)
			idleVol = 0;
		audio_voice_set_volume(a->idle, (master * idleVol) >> 8);
		audio_voice_set_pan(a->idle, pan);

		/*
		 * Pitched off the rev counter like the gear loop,
		 * so blipping the throttle at a standstill is
		 * still heard. record[0x1c] is added here too in
		 * the original and is always zero - see the note.
		 */
		audio_voice_set_freq(a->idle,
		        (int)((rpm * RACER_AUDIO_IDLE_HZ_SPAN +
		               RACER_AUDIO_IDLE_HZ_BASE) *
		              velocityPitch));

		if (r->speed >= RACER_AUDIO_ENGINE_OFF) {
			if (a->state == 2)
				a->state = 0;
		} else {
			/*
			 * Under 5 km/h the gear loop is stopped
			 * outright, not faded. record[0xa] at +0x28
			 * is cleared beside it in the original; no
			 * code anywhere assigns that slot a voice, so
			 * it is vestigial like record[0x1c].
			 */
			audio_voice_stop(a->engine);
			a->engine = MR_VOICE_NONE;
			a->state  = 2;
		}
	}

	/*
	 * START, OR RESTART ONLY WHEN THE SAMPLE CHANGES.
	 *
	 * The gate is state != 2 and gear > 0, so neutral is silent -
	 * which is why entry 0 of both cue tables is 0.
	 *
	 * AND THE COMPARISON IS table[prevGear] AGAINST table[gear],
	 * NOT prevGear AGAINST gear [verified, 0x004070b2]. On the road
	 * bike gears 3 to 6 all name Gear5, so shifting through them
	 * must NOT retrigger the loop; comparing gears instead would
	 * restart it on every shift and stutter audibly.
	 */
	if (a->state != 2 && gear > 0 && gear < 7) {
		int changed = (a->engine != MR_VOICE_NONE) &&
		              a->prevGear >= 0 && a->prevGear < 8 &&
		              gear != a->prevGear &&
		              cue[a->prevGear] != cue[gear];

		if (changed) {
			audio_voice_stop(a->engine);
			a->engine = MR_VOICE_NONE;
		}
		if (a->engine == MR_VOICE_NONE && cue[gear] != 0) {
			/*
			 * MODE 3 IS LOOP | POLYPHONIC [verified, the PUSH
			 * 3 at 0x004070da]. Looping is obvious; polyphonic
			 * matters because every racer runs its own voice
			 * over the same few samples.
			 */
			a->engine = audio_voice_start(cue[gear],
			                              SND_FLAG_LOOP |
			                              SND_FLAG_POLYPHONIC);
			a->state  = 1;
			mrlog("engine: gear %d -> cue 0x%02x, %d..%d Hz, "
			      "voice %d%s\n", gear, cue[gear],
			      range[gear].lo, range[gear].hi, (int)a->engine,
			      dirt ? " (dirt)" : " (road)");
		}
	}

	if (a->state == 1 && a->engine != MR_VOICE_NONE) {
		/*
		 * THE DRIFT, record[0x13]: 1000 Hz a frame toward 3000
		 * while the surface class is 3, and back down
		 * otherwise [0x0040717d].
		 *
		 * IT CAN ONLY EVER FALL, AND THAT IS THE ORIGINAL'S
		 * BEHAVIOUR, not a gap here. The class arrives with
		 * SurfaceUnder's +9 already on it and the remap sends
		 * nothing to 3, so the rising arm is unreachable for
		 * this caller - see racer_audio_surface. Starting at
		 * zero it therefore stays at zero, and the term
		 * contributes nothing in the shipped game.
		 */
		if (a->revDrift > 0)
			a->revDrift -= RACER_AUDIO_DRIFT_STEP;
		if (a->revDrift < 0)
			a->revDrift = 0;

		/*
		 * THE NOTE. lo + (hi - lo) * rpm
		 * [0x00407112..0x0040713b].
		 *
		 * ONE TRUNCATION, AT THE END. The original keeps the
		 * whole expression on the FPU stack and calls __ftol
		 * once, so lo is added as a float. Rounding the
		 * product first would be off by a hertz.
		 */
		hz = (int)((float)range[gear].lo +
		           (float)(range[gear].hi - range[gear].lo) *
		           rpm);

		/*
		 * THE BLIP, on the throttle's RISING edge only.
		 */
		if ((r->tickInputWord & 1) != 0 &&
		    (a->prevInput & 1) == 0 &&
		    (rand_next(RACER_AUDIO_RAND_STREAM) %
		     RACER_AUDIO_BLIP_ONE_IN) == 0) {
			hz += RACER_AUDIO_BLIP_BASE +
			      (rand_next(RACER_AUDIO_RAND_STREAM) %
			       RACER_AUDIO_BLIP_SPAN);
		}

		/*
		 * THE DRIFT IS SUBTRACTED, NOT ADDED - SUB ESI,[ECX]
		 * at 0x004071b1. It reads as an addition in the
		 * decompiler only because the term is always zero
		 * (see above), which is exactly why the sign went
		 * unnoticed: nothing can hear the difference.
		 */
		hz -= a->revDrift;

		/*
		 * THE WHEELIE TERM [0x004071a5]. 4000 Hz PER RADIAN,
		 * not a 4000 Hz ceiling - wheelieAngle is radians and
		 * the tick's own peak hold clamps at PI/2, so a full
		 * wheelie adds about 6280 Hz. It is much the largest
		 * of the three terms.
		 */
		hz = (int)((float)hz +
		           r->wheelieAngle * RACER_AUDIO_WHEELIE_SPAN);

		/*
		 * THE SLOPE TERM [0x004071c2..0x004071ea], and it is
		 * the bike's PITCH rather than the ground's: the
		 * height difference between the two wheel contact
		 * points over the wheelbase. Nose-up lowers the
		 * note, nose-down raises it.
		 */
		if (c->axeDistance != 0.0f)
			hz -= (int)((r->frontWheelContact[1] -
		                 r->rearWheelContact[1]) /
		                (c->axeDistance * BIKE_AXLE_K) *
		                RACER_AUDIO_SLOPE_SPAN);

		/*
		 * record[0x1c] at +0x70 is added here in the
		 * original and is NOT reconstructed, because the
		 * only code in the image that writes it -
		 * FUN_004073f0 - stores zero. It is a vestigial
		 * term, not a gap. [verified]
		 *
		 */
		hz = (int)((float)hz * velocityPitch);
		audio_voice_set_freq(a->engine, hz);

		/*
		 * THE VOLUME. The wander is per-RACER state, not a
		 * per-frame draw: it is re-rolled on a timer and held
		 * in between, which is what makes it a wander rather
		 * than a buzz.
		 */
		vol = RACER_AUDIO_VOL_AI;
		if (r->humanPlayer) {
			if ((r->tickInputWord & 1) == 0 &&
			    r->wheelieAngle == 0.0f) {
				int nowMs = (int)(tm_now() * 1000.0);
				int lim   = dirt ? RACER_AUDIO_VOL_MS_DIRT
				                 : RACER_AUDIO_VOL_MS_ROAD;

				if (nowMs - a->engineVolMs > lim) {
					a->engineVol =
					    RACER_AUDIO_VOL_BASE -
					    (rand_next(RACER_AUDIO_RAND_STREAM) %
					     RACER_AUDIO_VOL_SPAN);
					a->engineVolMs = nowMs;
				}
				vol = a->engineVol;
			} else {
				vol = RACER_AUDIO_VOL_ON;
			}
		}

		/*
		 * The surface-3 jitter is unreachable for this caller
		 * for the same reason the drift's rising arm is - see
		 * racer_audio_surface - so it is not implemented.
		 *
		 * THE LOW-SPEED FADE IS REACHABLE [0x004072ac]. Under
		 * 20 km/h the whole volume is scaled linearly down to
		 * nothing at 1 km/h, which is what stops a bike on the
		 * grid from idling at full volume.
		 *
		 * NOT CLAMPED AT ZERO, and that is the original -
		 * below 1 km/h the product goes negative and the
		 * mixer clamps it. The class-3 branch is the only
		 * place the original floors this value.
		 */
		if (r->speed < RACER_AUDIO_VOL_SPEED_FULL)
			vol = (int)((float)vol *
		                (r->speed -
		                 RACER_AUDIO_VOL_SPEED_ZERO) *
		                RACER_AUDIO_VOL_SPEED_K);
		audio_voice_set_volume(a->engine, (master * vol) >> 8);
		audio_voice_set_pan(a->engine, pan);
	}

	/*
	 * THE SKID VOICE, record[0xc] at +0x30.
	 *
	 * ORIGIN  maybe_Snd_TickRacer @ 0x00406290, at 0x00406e0f
	 *
	 * Held on the BRAKE BIT and, to start, only with the wheels
	 * down and above 30 km/h. The surface class picks the sample
	 * - and on the road, class 6, where most of a race is spent,
	 * that sample is one of the two BRAKE squeals at the lower
	 * 12050 Hz base, not a scrape. That is the sound that masks
	 * the downshift.
	 *
	 * THE ONGROUND TEST GUARDS THE VOICE, NOT THE RELEASE
	 * [0x00406e1c against 0x00406f59]. Braking in mid-air does
	 * not start a skid and does not fade a running one either.
	 */
	brakeBit = ((r->tickInputWord & 2) != 0);

	if (brakeBit && r->onGround) {
		if (a->skid == MR_VOICE_NONE &&
		    r->speed > RACER_AUDIO_SKID_MIN_SPEED) {
			int cls  = racer_audio_surface(r, trk);
			int scue = -1;

			a->skidVol  = RACER_AUDIO_SKID_VOL;
			a->skidRate = RACER_AUDIO_SKID_RATE;

			/*
			 * Cases 3, 7 and 8 cannot arrive here - the class
			 * carries SurfaceUnder's +9 and the remap never
			 * produces them. They are kept because the
			 * original's jump table has them, and dropping
			 * them would hide that the table is shared with a
			 * caller we have not reconstructed.
			 */
			switch (cls) {
			case 3: case 5: case 8: case 10:
				scue = RACER_AUDIO_CUE_GRAVIER;
				break;
			case 4:
				scue = RACER_AUDIO_CUE_HERBE;
				break;
			case 6: case 7:
				/*
				 * THE TWO BRAKE SAMPLES ALTERNATE AT RANDOM.
				 * The original's abs-then-restore-sign dance
				 * is a branchless signed % 2; rand_next is
				 * never negative, so & 1 is the same thing.
				 */
				a->skidRate = RACER_AUDIO_SKID_RATE_BRAKE;
				scue = RACER_AUDIO_CUE_BRAKE +
				       (rand_next(RACER_AUDIO_RAND_STREAM) & 1);
				break;
			case 13:
				scue = RACER_AUDIO_CUE_GLACE;
				break;
			default:
				break;
			}

			if (scue != -1) {
				a->skid = audio_voice_start(scue,
				                            SND_FLAG_LOOP |
				                            SND_FLAG_POLYPHONIC);
				mrlog("skid: class %d -> cue 0x%02x, %d Hz, "
				      "voice %d\n", cls, scue,
				      a->skidRate, (int)a->skid);
			}
			a->skidFading = 0;
		}

		if (a->skid != MR_VOICE_NONE) {
			/*
			 * THE PITCH RISES WITH SPEED, NOT WITH RPM
			 * [0x00406eea] - base rate plus the full 4000 Hz
			 * at the bike's own top speed.
			 */
			int shz = a->skidRate;

			if (c->realSpeedMax > 0.0f)
				shz += (int)(r->speed *
			                 RACER_AUDIO_SKID_PITCH_SPAN /
			                 c->realSpeedMax);
			shz = (int)((float)shz * velocityPitch);
			audio_voice_set_freq(a->skid, shz);
			audio_voice_set_volume(a->skid,
			                       (master * a->skidVol) >> 8);
			audio_voice_set_pan(a->skid, pan);
		}
	}

	/*
	 * THE RELEASE. Letting the brake go starts the fade, and so
	 * does falling under a fifth of the redline - which is what
	 * stops the squeal carrying on into a standstill.
	 *
	 * The clamp on rpm above cannot change this test: its floor
	 * is 0.05, already below the threshold.
	 */
	if ((a->skid != MR_VOICE_NONE && !brakeBit) ||
	    rpm < RACER_AUDIO_SKID_RPM_FLOOR)
	    a->skidFading = 1;

	if (a->skidFading) {
		/*
		 * skidVol IS NOT FLOORED AT ZERO, and that is the
		 * original's arithmetic rather than an oversight: with
		 * no voice up and the engine idling it keeps counting
		 * down. 50 a frame needs days to reach INT_MIN, so it
		 * is left alone rather than quietly diverging.
		 */
		a->skidVol -= RACER_AUDIO_SKID_FADE_STEP;
		if (a->skidVol < 0) {
			audio_voice_stop(a->skid);
			a->skid       = MR_VOICE_NONE;
			a->skidFading = 0;
		} else if (a->skid != MR_VOICE_NONE) {
			audio_voice_set_volume(a->skid,
			                       (master * a->skidVol) >> 8);
			audio_voice_set_pan(a->skid, pan);
		}
	}

	/*
	 * THE THROTTLE BLIP, record[0xb] at +0x2c.
	 *
	 * ORIGIN  maybe_Snd_TickRacer @ 0x00406290, at 0x00406fcd
	 *
	 * A ONE-SHOT ON THE THROTTLE'S RISING EDGE, and the
	 * player's alone. IT IS NOT THE REV BUMP IN THE ENGINE
	 * NOTE - that only shifts the running loop's pitch. This
	 * is a whole separate sample, and it is why tapping the
	 * gas at a standstill makes a noise at all: down there
	 * the gear loop is stopped and only the idle loop is up,
	 * so there is nothing for a pitch bump to act on.
	 *
	 * Which sample depends on the bike:
	 *
	 *   dirt   any of the three, Rand_Next(4) % 3
	 *   road   0x1f - min((int)(speed/RealSpeedMax * 5), 2),
	 *          so DEPART3 standing still and DEPART1 at speed
	 */
	if (playerVoices && (r->tickInputWord & 1) != 0 &&
	    (a->prevInput & 1) == 0) {
		int dcue;

		if (dirt) {
			dcue = RACER_AUDIO_DEPART_LO +
			       (rand_next(RACER_AUDIO_RAND_STREAM) % 3);
		} else {
			int k = 0;

			if (c->realSpeedMax > 0.0f)
				k = (int)(r->speed / c->realSpeedMax *
			              RACER_AUDIO_DEPART_SPEED);
			if (k > 2)
				k = 2;
			dcue = RACER_AUDIO_DEPART_HI - k;
		}

		/*
		 * POLYPHONIC AND NO RETRIGGER GUARD, which is the
		 * original's own: 0x0040703a pushes flags 2 and
		 * tests nothing.  Blipping the throttle fast really
		 * does stack copies, on both builds.  [verified]
		 *
		 * It piles up hardest under the debug fast-forward,
		 * where many game frames pass inside one blip's
		 * playing time - twenty voices on one sample, seen.
		 * That is the pool's problem to absorb, not a reason
		 * to guard here; see audio_voice_for.
		 */
		a->depart = audio_voice_start(dcue,
		                              SND_FLAG_POLYPHONIC);
		audio_voice_set_volume(
		    a->depart,
		    (master * RACER_AUDIO_DEPART_VOL) >> 8);
		audio_voice_set_pan(a->depart, pan);
		mrlog("depart: cue 0x%02x, voice %d%s\n",
		      dcue, (int)a->depart,
		      dirt ? " (dirt)" : " (road)");
	}

	/* The previous-frame copies every edge above triggers on. */
	a->prevRpmFrac = rpm;
	a->prevInput   = r->tickInputWord;
	a->prevGear    = gear;
	a->prevCrash   = r->crashLatch;
	a->prevWheelie = r->wheelieAngle;
	if (a->peakWheelie < r->wheelieAngle)
		a->peakWheelie = r->wheelieAngle;
}

#define REMOTE_YAW_TAKE     0.9f    /* 0x3f666666 at 0x0045e5ce */
#define REMOTE_CHASE_K      1.28    /* 0x004d1010               */
#define REMOTE_POS_LERP     0.5f    /* 0x004d1018               */
#define REMOTE_WALL_DAMP    0.5f    /* 0x004d1018, same word    */
#define REMOTE_LEAN_GROW    1.2     /* 0x004d0fe0, a DOUBLE     */
#define REMOTE_LEAN_DIRT    1.5     /* 0x004d1038, a DOUBLE     */
#define REMOTE_LEAN_LERP    0.5     /* 0x004d0f08               */

/* ORIGIN  Math_AngleDeltaScaled @ 0x0045b1b0 */
static float math_angle_delta_scaled(float a, float b, float scale)
{
	double x = (double)a;
	double y = (double)b;
	double d;

	while (x < 0.0)           x += MR_PI * 2.0;
	while (x >= MR_PI * 2.0)  x -= MR_PI * 2.0;
	while (y < 0.0)           y += MR_PI * 2.0;
	while (y >= MR_PI * 2.0)  y -= MR_PI * 2.0;

	/*
	 * THE SHORT WAY ROUND, which is the whole point: b is pulled to
	 * within pi of a before the subtraction, so a bike at 359
	 * degrees turning to 1 does not take the long way.
	 */
	if (y > x + MR_PI)  y -= MR_PI * 2.0;
	if (y < x - MR_PI)  y += MR_PI * 2.0;

	d = (y - x) * (double)scale;
	while (d < 0.0)           d += MR_PI * 2.0;
	while (d >= MR_PI * 2.0)  d -= MR_PI * 2.0;
	return (float)d;
}

/* ORIGIN  Racer_RemoteApplyState @ 0x0045e560 */
void racer_remote_apply_state(MR_Racer *r, const MR_Trk *t,
                              int selectedMode, float dtScaled)
{
	float look[12];
	float inv[12];
	float eye[3];
	float euler[3];
	float dir[3];
	float rest[3];
	float len;
	float diff;
	float blend;
	float keep;
	float step;
	int   i;

	if (!r || !t)
		return;

	r->engineSpeed = r->speed;

	/*
	 * 1. 0x0045e588  TURN toward the reported heading, 0.9 of the
	 * way each tick. It is not assigned, so a peer's bike swings
	 * round rather than snapping to the angle in the packet.
	 */
	for (i = 0; i < 3; i++)
		eye[i] = 0.0f;
	mat34_look_at(look, eye, r->aimDir);

	/*
	 * THE LOOK-AT IS INVERTED BEFORE ITS YAW IS TAKEN - 0x0045e5a1,
	 * Math_InvertRigidMatrix @ 0x0040fe80, between the look-at and
	 * Matrix34_YawOf. A look-at is a VIEW matrix, so its 3x3 is the
	 * transpose of the orientation it describes and yaw(R^T) is
	 * -yaw(R). Taking the yaw of the un-inverted matrix therefore
	 * steers toward the MIRROR of the reported heading: straights
	 * look right, because yaw 0 negates to itself, and every corner
	 * swings the bike the wrong way until it is travelling sideways.
	 * racer_ai_follow_line does the same pair at 0x0046b24d.
	 */
	mat34_invert_rigid(inv, look);
	diff = math_angle_delta_scaled(-racer_matrix_yaw(r->matOrientation),
	                               racer_matrix_yaw(inv),
	                               REMOTE_YAW_TAKE);
	if ((double)diff > MR_PI)
		diff -= (float)(MR_PI * 2.0);

	r->steering = 0.0f;
	euler[0] = 0.0f;
	euler[1] = diff;
	euler[2] = 0.0f;
	racer_compose_orientation(r->matOrientation, euler);

	/*
	 * 2. 0x0045e64a  moveDir COMES FROM WHERE THE PEER HAS BEEN,
	 * not from what it reports it is doing: the last two received
	 * positions give the travel direction.
	 *
	 * placeWheels0/1/2 are the network slots despite the name - the
	 * received position, the previous one and the chase point.
	 * docs/subsystems/netplay.txt 5.
	 */
	for (i = 0; i < 3; i++)
		dir[i] = r->placeWheels0[i] - r->placeWheels1[i];

	if ((float)fabs((double)dir[0]) > RACER_ORIENT_EPS
	 || (float)fabs((double)dir[1]) > RACER_ORIENT_EPS
	 || (float)fabs((double)dir[2]) > RACER_ORIENT_EPS) {
		len = (float)sqrt((double)dir[0] * dir[0]
		                  + (double)dir[1] * dir[1]
		                  + (double)dir[2] * dir[2]);
		if (len != 0.0f) {
			for (i = 0; i < 3; i++)
				dir[i] = dir[i] / len;
		}
		blend = (float)((double)r->flImpactBlend * AI_BLEND_SCALE);
		keep  = 1.0f - blend;
		for (i = 0; i < 3; i++)
			r->moveDir[i] = dir[i] * keep + r->moveDir[i] * blend;
	}

	if (selectedMode == 0) {
		/*
		 * 3. 0x0045e8d1  MODE 0 TAKES THE POSITION OUTRIGHT, with
		 * no chase and no lerp. It is also the arm the time-attack
		 * ghost runs, which is why a ghost reads as a peer.
		 */
		for (i = 0; i < 3; i++)
			r->position[i] = r->placeWheels0[i];
		r->segmentStart = trk_find_segment_for_point(t, r->segmentStart,
		                                             r->position, NULL);
		return;
	}

	/*
	 * 4. 0x0045e7a2  the chase point walks toward the reported
	 * position at the racer's own speed and SNAPS when it would
	 * overshoot - the dot going negative is that test.
	 */
	for (i = 0; i < 3; i++)
		dir[i] = r->placeWheels0[i] - r->placeWheels2[i];
	len = (float)sqrt((double)dir[0] * dir[0]
	                  + (double)dir[1] * dir[1]
	                  + (double)dir[2] * dir[2]);
	if (len > 0.0f) {
		for (i = 0; i < 3; i++)
			dir[i] = dir[i] / len;
		step = (float)((double)r->engineSpeed * dtScaled
		               * REMOTE_CHASE_K);
		for (i = 0; i < 3; i++)
			r->placeWheels2[i] += dir[i] * step;

		for (i = 0; i < 3; i++)
			rest[i] = r->placeWheels0[i] - r->placeWheels2[i];
		if (rest[0] * dir[0] + rest[1] * dir[1]
		    + rest[2] * dir[2] <= 0.0f) {
			for (i = 0; i < 3; i++)
				r->placeWheels2[i] = r->placeWheels0[i];
		}
	}

	/*
	 * 5. 0x0045e88a  and the bike only ever moves HALF WAY to the
	 * chase point, which is the second of the two smoothing stages.
	 */
	for (i = 0; i < 3; i++)
		r->position[i] += (r->placeWheels2[i] - r->position[i])
	                      * REMOTE_POS_LERP;
	r->segmentStart = trk_find_segment_for_point(t, r->segmentStart,
	                                             r->position, NULL);
}

/* ORIGIN  maybe_Racer_TickRemote @ 0x0045e940 */
int racer_tick_remote(MR_Racer *r, const MR_BikeCarac *c, const MR_Trk *t,
                      const MR_AiEnv *env, float dtScaled)
{
	float was[3];
	float hit[3];
	float front[3];
	float rear[3];
	float fwd[3];
	float groundY;
	int   savedInput;
	int   persoAnim;
	int   nearQuery;
	int   stuntAir;
	int   airborne;
	int   sink;
	int   segF;
	int   segR;
	int   crash;
	int   i;

	if (!r || !c || !t || !env)
		return 0;

	savedInput        = r->tickInputWord;
	persoAnim         = c->persoAnim;
	r->handSkeleton   = 0;
	r->riderStateBits = 0;

	racer_save_prev_frame(r, dtScaled);

	for (i = 0; i < 3; i++)
		was[i] = r->position[i];

	racer_remote_apply_state(r, t, env->selectedMode, dtScaled);
	r->springCompression = 0.0f;

	/*
	 * 0x0045e9e6  THE WALLS ARE CLIPPED, NOT IMPACTED. A remote
	 * racer runs none of the player's ground contact, so this is
	 * all that stops a reported position putting a bike through a
	 * rail - and it halves both speeds instead of running the wall
	 * response.
	 */
	if ((float)fabs((double)(was[0] - r->position[0])) > RACER_ORIENT_EPS
	 || (float)fabs((double)(was[1] - r->position[1])) > RACER_ORIENT_EPS
	 || (float)fabs((double)(was[2] - r->position[2])) > RACER_ORIENT_EPS) {
		if (trk_clip_move_against_walls(t, r->segmentStart, was,
		                                r->position, hit, r->moveDir,
		                                r->wallNormal)) {
			for (i = 0; i < 3; i++)
				r->position[i] = hit[i];
			r->segmentStart  = trk_find_segment_for_point(t,
			                       r->segmentStart, r->position, NULL);
			r->aiRoadSpeed   = r->aiRoadSpeed * REMOTE_WALL_DAMP;
			r->engineSpeed   = r->engineSpeed * REMOTE_WALL_DAMP;
			r->flImpactBlend = RACER_IMPACT_BLEND;
		}
	}

	r->speed  = r->engineSpeed;
	stuntAir  = 0;
	nearQuery = racer_ai_is_corner(t, r->segmentStart, 8, env->raceDir);

	/* 0x0045eabb  the same stunt gate TickAI carries. */
	if (persoAnim != 0 && nearQuery == 0)
		stuntAir = 1;

	if (nearQuery != 0)
		r->stuntOrOverspeed = 0;
	else if ((char)rand() == 0 && r->realSpeedMax < r->aiRoadSpeed)
		r->stuntOrOverspeed = 1;

	if (!env->preRace)
		racer_update_wheelie(r, dtScaled);

	groundY  = trk_ground_height_at(t, r->segmentStart, r->position);
	airborne = stuntAir
	         ? (groundY + AI_CLEAR_STUNT  < r->position[1])
	         : (groundY + AI_CLEAR_NORMAL < r->position[1]);

	if (airborne && persoAnim != 0) {
		r->onGround = 0;
	} else {
		if (r->onGroundPrev == 0) {
			float flat;

			/* 0x0045eb8c  the landing, and it flattens moveDir. */
			r->springCompression =
			    (float)((double)(r->positionPrev[1] - groundY)
			            * LAND_SPRING_SCALE);
			r->flImpactBlend = RACER_IMPACT_BLEND;
			r->moveDir[1]    = 0.0f;
			flat = (float)sqrt((double)r->moveDir[0] * r->moveDir[0]
			                   + (double)r->moveDir[2] * r->moveDir[2]);
			if (flat != 0.0f) {
				r->moveDir[0] = r->moveDir[0] / flat;
				r->moveDir[1] = 0.0f;
				r->moveDir[2] = r->moveDir[2] / flat;
			}
			r->riderStateBits |= 0x80;
		}

		sink = 0;
		if (persoAnim != 0 && nearQuery == 0) {
			float k = (r->realSpeedMax != 0.0f)
			        ? (r->engineSpeed / r->realSpeedMax
			           * AI_CLEAR_STUNT)
			        : 0.0f;
			int   n = (int)k;

			if (n != 0)
				sink = rand() % n;
		}

		r->position[1]   = groundY - (float)sink;
		r->segmentStart  = trk_find_segment_for_point(t, r->segmentStart,
		                                              r->position, NULL);
		r->onGround      = 1;
		r->flAirTime     = 0.0f;
		r->onGroundPair1 = 1;
		r->onGroundPair2 = 1;

		for (i = 0; i < 3; i++) {
			rear[i]  = r->position[i] + r->matOrientation[9 + i]
			           + r->matOrientation[6 + i]
			             * (c->axeDistance * K_WHEELBASE_REAR);
			front[i] = r->position[i] + r->matOrientation[9 + i]
			           + r->matOrientation[6 + i]
			             * (c->axeDistance * K_WHEELBASE_FRONT);
		}

		segF = trk_find_segment_for_point(t, r->segmentStart, front, NULL);
		segR = trk_find_segment_for_point(t, r->segmentStart, rear, NULL);
		front[1] = trk_ground_height_at(t, segF, front);
		rear[1]  = trk_ground_height_at(t, segR, rear);

		for (i = 0; i < 3; i++)
			fwd[i] = front[i] - rear[i];

		racer_frame_from_forward(r->matOrientation, fwd);
		r->matOrientation[9]  = 0.0f;
		r->matOrientation[10] = 0.0f;
		r->matOrientation[11] = 0.0f;
	}

	racer_wheel_contacts(r, c, r->rearWheelContact,
	                     r->frontWheelContact);

	racer_update_lean(r, c, t, r->speed, env->dtMs, 0, NULL);

	/*
	 * 0x0045ef9c  THE LEAN DECAYS INSTEAD OF BEING DRIVEN. A road
	 * bike grows it by 1.2 while nothing has hit it and zeroes it
	 * when something has; a dirt bike is zeroed outright. The half
	 * lerp afterwards is what makes that a slide and not a jump.
	 */
	if (persoAnim == 0) {
		if ((float)fabs((double)r->flImpactBlend) == 0.0f)
			r->lean = (float)((double)r->lean * REMOTE_LEAN_GROW);
		else
			r->lean = 0.0f;
	} else {
		r->lean = (float)((double)r->lean * REMOTE_LEAN_DIRT);
	}
	r->lean = (float)(((double)r->lean - r->flLeanPrev)
	                  * REMOTE_LEAN_LERP + r->flLeanPrev);
	r->flLeanPrev = r->lean;

	racer_spin_wheels(r, c, dtScaled);
	racer_update_suspension(r, c, dtScaled);
	racerdraw_build_shadow(r, t);

	{
		unsigned int synthesised = (unsigned int)r->tickInputWord;

		r->tickInputWord = savedInput;
		racer_emit_wheel_fx(r, synthesised, t, env->viewedSegment,
		                    env->viewedIndex, dtScaled);
	}

	crash = racer_update_crash(r, t);
	/*
	 * maybe_Racer_UpdateSkeleton(r, 1) at 0x0045f12c - ours poses
	 * from the DRAW instead, the same divergence every other tick
	 * carries. See race.c's note on the four tick call sites.
	 */
	racer_update_dash(r, c, r->frameDelta);

	/*
	 * MEASURED, NOT CHOSEN - the remote tick reads the lane back off
	 * the position the way the player's does, where an AI picks one.
	 */
	r->lane = racer_lane_of_position(r, t);
	return crash;
}
