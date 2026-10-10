/*
 * autopilot.c - a look-ahead pursuit driver for the local racer.
 *
 * ORIGIN  NONE - ours. There is no original; MOTO.EXE has no such
 *         thing, and every function here is tagged the same way so an
 *         untagged one reads as an oversight rather than as ours by
 *         default.
 *
 * See autopilot.h for why this is not a sixth gameplay file and why
 * the game's own AI is no use for it.
 *
 *
 * THE MODEL
 *
 * Each frame, everything measured along the bike's OWN row 0 - its
 * lateral axis - so that no term needs a handedness convention:
 *
 *     aimDot   where a point N segments ahead lies, +1 hard toward
 *              +row0, -1 hard away from it
 *     lateral  how far off the segment centreline the bike sits,
 *              in the same axis
 *     want     aimDot - gain*lateral
 *
 * then bang-bang on want with hysteresis, and a target speed from
 * the look-ahead curvature.
 *
 * A TRACK IS A ONE-DIMENSIONAL RAIL, which is why nothing cleverer is
 * warranted. There is no route to choose and no branch to search: the
 * only decisions are how far from the centre to sit and how fast to
 * arrive.
 *
 * IT DRIVES CONSERVATIVELY ON PURPOSE. A slower clean lap beats a
 * fast one into a wall, because the point is to REACH THE FINISH
 * unattended, not to set a time.
 *
 *
 * TWO RULES THIS FILE IS BUILT ON, both of them easy to break by
 * writing the obvious thing:
 *
 *   o STEER THROUGH THE ANALOG PATH, NOT THE DIGITAL ONE.
 *     racer_apply_steering's digital path integrates - holding a
 *     button ramps the bars toward the lock stop and keeps turning
 *     - so proportional control of it overshoots by construction.
 *     The analog path sets the angle directly. Every damping,
 *     hysteresis and anti-windup mechanism a rate actuator would
 *     need is therefore absent on purpose; see autopilot.h.
 *
 *   o NOTHING SPELLS ITS OWN LEFT AND RIGHT. Every steering
 *     decision goes through AP_TURN_SIGN. A block that works out
 *     its own handedness will eventually disagree with the main
 *     path, and a recovery that disagrees drives the bike further
 *     off the road precisely when it fires.
 */
#include <math.h>
#include <stdlib.h>

#include "autopilot.h"
#include "menu.h"
#include "bikes.h"
#include "log.h"
#include "race.h"

int g_autopilot;
int g_autopilotLog;


/* ------------------------------------------------------------------ */
/* Tuning. Every one of these is OURS - there is no original to read   */
/* them out of, so they are named rather than buried as literals.      */
/* ------------------------------------------------------------------ */

/*
 * HOW FAR AHEAD TO AIM, in segments, and it SCALES WITH SPEED.
 * ile01 averages 514 units a segment, so six is about 3000 units and
 * fourteen about 7200.
 *
 * A FIXED LOOK-AHEAD IS WRONG AT BOTH ENDS - too far and the bike
 * cuts the corner, too near and it arrives before it has turned. At
 * a fixed eight the aim term swings +-0.2 to +-0.3 frame to frame,
 * the aim point jumping sideways as segments go by.
 *
 * Time-to-target is what wants to be constant, so the distance grows
 * with speed. About 1.2 seconds of travel across the range.
 */
#define AP_LOOK_MIN         4
#define AP_LOOK_MAX         14
#define AP_LOOK_KMH_PER_SEG 16      /* one more segment per 16 km/h */

/*
 * AND IT SHORTENS WHERE THE ROAD BENDS, which speed alone does not
 * do well enough.
 *
 * AIMING PAST THE END OF A CORNER IS AIMING AT THE WRONG CORNER.
 * ile01's bridge is an S-bend whose lobes are about eight segments
 * each, and the speed-based look-ahead there is eleven - so the aim
 * point sat on the OPPOSITE lobe, curving the other way, for most
 * of segments 404 to 422. The controller steered for a corner it
 * had not reached through the one it was in.
 *
 * Scaling the look-ahead down by the curvature at the aim point
 * pulls it back inside the lobe: a straight keeps the full distance
 * and the tightest corner on the track keeps this fraction of it.
 */
#define AP_LOOK_CURVE_MIN   0.40f

/*
 * THE BRAKING LOOK-AHEAD IS NOT A WINDOW. It is the whole track,
 * resolved once into a speed profile - see ap_build_profile.
 *
 * A window can only answer "is there a corner within N". Arriving
 * too fast is a PLANNING failure, and the question is "how fast may
 * I be going HERE so that I can still be slow enough THERE" - which
 * the backward pass answers for every segment at once, and cheaper
 * per frame than scanning.
 */

/*
 * AND THE CURVATURE IS MEASURED LOCALLY, over this many segments,
 * then the WORST in the window is what counts.
 *
 * DO NOT MEASURE IT BY COMPARING THE TWO ENDS OF THE WINDOW. That
 * is wrong in a way that is easy to miss: an S-BEND WHOSE EXIT
 * LINES UP WITH ITS ENTRY READS AS PERFECTLY STRAIGHT. Left then
 * right cancels, the bike is told the road is flat, and it arrives
 * at the first half at full speed. A tight corner in the middle of
 * the window is invisible for the same reason.
 */
#define AP_CURVE_STEP       4

/*
 * THE LATERAL TERM'S WEIGHT, as a fraction of the road's own HALF
 * WIDTH rather than as world units.
 *
 * THE ROAD IS NOT A CONSTANT WIDTH, and a gain in world units
 * assumes it is. Read out of ile01: the band is +-1024 for
 * most of the lap, +-512 across the bridge at segments 404 to 423,
 * and +-384 through 424 to 435, which are the narrowest on the
 * track. A gain in world units gives the same demand for the same
 * offset on all three - so where the road halves, the correction
 * silently halves in relative terms exactly where it is needed
 * most. That is the bridge section this kept failing on.
 *
 * Dividing by seg->bandHi/bandLo makes it self-scaling: the number
 * below is what a bike at the very edge of the road contributes,
 * whatever the road happens to be doing.
 *
 * SIZE IT AGAINST THE PROPORTIONAL GAIN, NOT AGAINST A DEADBAND.
 * A value that was decisive as most of a 0.14 bang-bang threshold
 * is only a nudge once AP_BAR_GAIN multiplies it: at 0.085 a bike
 * 63 per cent of the way to the wall asks for 0.09 of lock and
 * drifts into the narrows anyway. At 0.25 a bike at the edge asks
 * for 0.63 of lock, which is a correction rather than a
 * suggestion.
 */
#define AP_LATERAL_K        0.25f

/*
 * THERE IS NO DAMPING TERM ANY MORE, and the reason is worth
 * keeping: it only ever existed to stop a RATE actuator
 * overshooting. With the analog path the bar angle is set directly
 * and cannot overshoot, so subtracting the current bar from the
 * demand for it would be the controller fighting itself.
 *
 * The same goes for the steering deadband's hysteresis and the bar
 * cap's anti-windup: all three were artefacts of bang-banging a
 * ramp. See autopilot.h.
 */

/*
 * Steering deadband, and the wider band it must cross to let go -
 * hysteresis. Dimensionless, the same space as sin(heading error).
 */
#define AP_STEER_ON         0.14f
#define AP_STEER_OFF        0.05f

/*
 * DO NOT CAP THE BAR ANGLE. A cap looks obviously right and is
 * wrong, which is why it is worth a paragraph.
 *
 * THE ARGUMENT FOR ONE IS A BICYCLE MODEL, and the bicycle model
 * does not apply: this engine's steering feeds a TORQUE, not a
 * kinematic steer angle. So the fact that a lap sits at 26 degrees
 * of lock where the geometry wants about one says nothing about
 * whether the demand is too large.
 *
 * THE ENGINE REALLY DOES WANT LARGE VALUES. Capped at 0.35 and on
 * direct angle control, ile01 took 2:50 with the bike
 * under-steering into walls on every hard curve, against 1:35
 * uncapped. The demand exceeds 0.35 for 34 per cent of samples -
 * precisely the 34 per cent where steering is being asked for.
 *
 * A proportional law needs no cap: on line the demand is small, so
 * the angle is small. The clamp that remains is the lock stop.
 */

/*
 * want is a signed demand of order one; this turns it into a
 * fraction of the lock stop.
 *
 * SIZED FROM MEASUREMENT rather than picked. Over a logged lap the
 * mean demand was 0.332, and the digital run that lapped in 1:35
 * was near full lock through the corners. 2.5 puts an average
 * demand around 0.8 of lock and saturates where the old controller
 * saturated, while staying proportional - and therefore smooth -
 * everywhere the bike is on line and the demand is small.
 */
#define AP_BAR_GAIN         2.5f

/* The game's analog axis: 0x8000 centred, +-0x7fff of travel. */
#define AP_AXIS_CENTRE      0x8000
#define AP_AXIS_SPAN        0x7fff

/*
 * THE SPEED CEILING IS THE BIKE'S OWN, not a number. realSpeedMax
 * is per-bike - 80 m/s on the superbike, which is 288 km/h.
 *
 * A FIXED CEILING BELOW THE BIKE'S REAL TOP SPEED MAKES IT BRAKE ON
 * THE STRAIGHTS: the profile asks for less than the bike is already
 * doing, and the controller obeys. At 170 against an observed 264
 * km/h it did exactly that, on every straight.
 *
 * 3.6 IS THE VERIFIED CONVERSION, not an assumption:
 * racer_apply_steering's STEER_MIN_SPEED is 2/3.6 for a 2 km/h
 * threshold, so racer speeds are m/s.
 */
#define AP_SPEED_KMH_PER    3.6f
#define AP_SPEED_MIN        65
/*
 * THE CURVATURE THAT ASKS FOR AP_SPEED_MIN IS NOT A CONSTANT - it
 * is measured off the track at load, and it has to be.
 *
 * Local curvature over AP_CURVE_STEP segments, from the shipped
 * files:
 *
 *              median   tightest
 *     ile01     0.105      0.279
 *     ile04     0.314      1.257
 *
 * FOUR AND A HALF TIMES APART on the tightest corner, and ile04's
 * MEDIAN is tighter than ile01's worst. Any fixed number is badly
 * wrong on one of them: at 0.25 a bike would sit on the speed floor
 * for most of ile04, and at 1.0 it would carry full speed through
 * everything ile01 has.
 *
 * So the scale is the tightest corner on THIS track, and the
 * schedule reads "how tight is this corner compared with the worst
 * one here" - which is the question that transfers.
 */
#define AP_CURVE_SCALE_MIN  0.12f   /* guard a near-straight track */

/*
 * THE CORNERING LIMIT, in (km/h) squared per world unit of radius -
 * v = sqrt(A * R), which is the physics rather than a curve fit.
 *
 * A LINEAR RAMP IN CURVATURE IS THE WRONG SHAPE. Cornering speed
 * goes as the square root of the radius; scaling speed down linearly
 * as curvature rises asks for a lateral acceleration that varies
 * with how tight the corner is, which no tyre does.
 *
 * MEASURED. Taking the old linear schedule and working back what
 * lateral acceleration it implied, segment by segment:
 *
 *                 p10    median     max
 *     ile01      0.704    0.872    0.945
 *     ile04      1.888    3.832    4.253
 *
 * On ile01 it is roughly constant, which is why the ramp worked
 * there at all. ON ile04 IT ASKS FOR FOUR TIMES THE GRIP, and that
 * is the per-track curvature scale doing it: a tighter track gets
 * normalised back into the same 65..170 band, so the tighter the
 * track the more it over-drives. ile04 would be over-speed on 447
 * of its 782 segments.
 *
 * [open] 1.50 IS A STEP IN AN UNFINISHED BINARY SEARCH, not a
 * settled value. The last clean result was 1.15: zero slip on all
 * 934 corner samples of a logged lap - the game's own fxGate,
 * onGround && |slipAbs| >= 0.04 - with the bike holding a mean 69
 * units off the line, half a metre on a ten-metre road. Neither
 * limit had been found, so the next step up is what is in the file.
 *
 * TWO ACCEPTANCE TESTS, AND BOTH MATTER. Slip is the obvious one,
 * but a bike can run WIDE without ever sliding, so mean |off| is
 * the other: if it climbs from 69 while slip stays 0, the limit is
 * the front tyre's grip rather than the rear's and the number is
 * still too high. Lap times alone cannot tell those apart.
 */
#define AP_LAT_ACCEL        1.50f

/*
 * HOW HARD THE BIKE CAN SLOW, in (km/h) squared per world unit, for
 * the backward pass in ap_build_profile.
 *
 * v*v = u*u + 2*a*s, and THIS IS THE 2a, not the a - ap_build_profile
 * uses it as sqrt(vNext*vNext + AP_DECEL*d) directly. So a full 170
 * to 65 needs 20562 units, about forty of ile01's segments.
 *
 * 1.2 IS 0.60 g. The world is 128 units to the metre (race.c,
 * RACE_UNITS_PER_METRE_DEFAULT, measured there against a straight
 * run), so a = AP_DECEL * 128 / 12.96 / 2.
 *
 * DO NOT RAISE IT TOWARD 1 g. 2.0 works out at 1.01 g, which is the
 * absolute limit of a motorcycle brake on dry tarmac - no margin,
 * and while the same tyres are being asked to corner. A profile
 * that plans braking it cannot deliver arrives at every corner too
 * fast. Measured on a logged lap, 133 to 80 km/h over segments 386
 * to 407 and about 10750 units is 0.53, with coasting in it, so the
 * real brake is above 0.53 and nowhere near 2.0.
 *
 * LOWER IS SAFER: it brakes from further out.
 */
#define AP_DECEL            1.2f

/*
 * THE LOOK-AHEAD SIMULATOR.
 *
 * race_autopilot_probe runs the REAL physics forward with a button
 * word held and puts every byte back - see race.c. This uses it as
 * a VETO, not as a chooser: the controller decides as it always
 * did, the probe asks "does that end badly", and only then are
 * alternatives tried.
 *
 * THAT ORDERING IS THE WHOLE COST ARGUMENT. Scoring six candidates
 * every frame is six rollouts a frame - a hundred-odd physics ticks
 * where the game does one. Vetoing costs ONE, and escalates only
 * where the controller is about to be wrong, which is rare and is
 * exactly where the budget is worth spending.
 *
 * AND race_tick_physics MUST NOT POLL INPUT WHILE PROBING. The poll
 * lives INSIDE the tick, so a nested tick would overwrite the very
 * word being tested and every candidate would score the same. See
 * s_race_dryRun in race.c.
 *
 * THE SCORE AVOIDS SEGMENT-RELATIVE TERMS ON PURPOSE. At the bridge
 * the segment lookup is itself degrading - the log walks 431, 412,
 * 405, 407 in four samples - so "how far off the line did I end up"
 * is measured against a reference that is part of the problem.
 * Distance made good toward the aim point plus the speed still on
 * the bike needs none of it, and a wall bleeds speed hard enough to
 * show up in both terms.
 */
/*
 * EVERY DURATION HERE IS MILLISECONDS. NOTHING COUNTS FRAMES, AND
 * ANYTHING THAT DOES IS WRONG AT ANY OTHER FRAME RATE.
 *
 * TAB fast-forward works by making each frame integrate MORE time -
 * timing.h, tm_set_speed - so at 10x on a 60 fps display a frame is
 * 166 ms. Counting frames then gives a different controller at every
 * clock speed, and the fast-forwarded one is what a test session
 * actually measures:
 *
 *   a probe every 3 frames becomes every half second, reacting at
 *   6 Hz instead of 60
 *   a stuck backstop of 120 frames waits twenty seconds, not two
 *   a horizon of 18 ticks looks THREE SECONDS ahead instead of 300
 *   ms, which is close to meaningless to veto on
 *
 * The horizon is the subtle one: race_autopilot_probe steps
 * race_tick_physics N times and a tick is however long the current
 * frame is.
 */
#define AP_PROBE_HORIZON_MS 300
#define AP_PROBE_MS         50      /* between probes               */
#define AP_PROBE_SPEED_K    0.35f   /* speed retained, vs distance  */
#define AP_PROBE_CRASH      1.0e6f

/*
 * WHAT COUNTS AS "THAT ENDS BADLY".
 *
 * A PREDICTED CRASH IS THE WRONG TEST and vetoing on it alone never
 * fires. The bike does not CRASH at the bridge - it scrapes, stalls
 * and spends fifteen seconds turning round, and none of that sets
 * flCrashTimerMs. Traced live, that veto escalated on no frame of a
 * whole lap, so the rollout was pure overhead.
 *
 * SPEED LOST OVER THE HORIZON IS THE SIGNAL. A clean line keeps most
 * of it; anything that touches a wall scrubs it hard. Predicting
 * less than this fraction of the speed the bike has now is the cue
 * to go looking for something better.
 */
#define AP_PROBE_KEEP       0.72f

/*
 * The two steering alternatives, as multiples of the angle the
 * controller asked for. MORE and LESS lock rather than left and
 * right: the controller has already decided which way to go and is
 * far more often wrong about how hard.
 */
#define AP_PROBE_MORE       1.6f
#define AP_PROBE_LESS       0.4f

/*
 * THERE IS NO CORNER FEEDFORWARD, AND IT MUST NOT COME BACK.
 *
 * THE ARGUMENT FOR ONE IS PLAUSIBLE AND WRONG: want = aim -
 * gain*off looks like proportional control, and proportional
 * control against a constant disturbance settles with a
 * steady-state error, so a constant-radius corner appears to need a
 * feedforward term. But
 * aim is not a position error - it is a PURSUIT BEARING, and the
 * bearing to a point ahead on a circular path is already non-zero
 * and constant. Pursuit handles constant curvature by construction;
 * there is no error to feed forward against.
 *
 * WHAT ONE ACTUALLY DOES is double-count in steady corners and go
 * out of phase at transitions, because it read the curvature HERE
 * while the aim term reads the road AHEAD. ile01's bridge is the
 * worst case on the track - an S-bend at half width - and for most
 * of segments 404 to 422 the two have OPPOSITE SIGNS:
 *
 *     seg 404-410   curvature here -0.209, at the aim point +0.209
 *     seg 414-422   curvature here +0.209, at the aim point -0.084
 *
 * so it holds the bars into one lobe while the aim term asks for
 * the other.
 */
/*
 * THE COAST BAND, either side of the target.
 *
 * KEEP IT NARROW. A twelve-wide dead zone is most of the difference
 * between a corner speed and a straight one, so the bike drifts
 * through the band rather than driving to it: at 6 it coasted 53
 * per cent of a measured lap, neither accel nor brake set.
 *
 * 2 keeps the hysteresis that stops accel and brake alternating
 * frame to frame, without handing back the throttle for free. The
 * SAFETY ENVELOPE IS UNCHANGED: this does not raise the target, it
 * just makes the bike actually hold it.
 */
#define AP_SPEED_BAND       2

/*
 * AND BEING OFF LINE IS ITSELF A REASON TO SLOW DOWN - but only
 * once it is far enough off to matter.
 *
 * IT MUST NOT BITE FROM THE CENTRELINE OUT. The bike is ALWAYS
 * slightly off line, so a term that starts at zero offset brakes
 * continuously and never gets back on the throttle. Measured at
 * ile01 segment 407: 142 units off a 512 half-width is 28 per cent,
 * a perfectly ordinary racing deviation, and it turned the
 * profile's 91 km/h into 80 through the whole bridge approach.
 *
 * The physical limit is grip and the profile already has that. This
 * is a SAFETY MARGIN on top, so it should do nothing until the bike
 * is genuinely near the edge: free within AP_OFFLINE_FREE of the
 * band, then ramping to AP_OFFLINE_SCALE at the verge.
 */
#define AP_OFFLINE_FREE     0.50f
#define AP_OFFLINE_SCALE    0.55f

/*
 * THE BARS ARE DEAD BELOW 2 km/h - STEER_MIN_SPEED in racer.c - so
 * a slow bike has NO STEERING AUTHORITY AT ALL. Every speed rule
 * here exists because of that one fact: the floor under the target,
 * the turn-around cap, and the stuck backstop all keep enough speed
 * on the bike for the bars to answer.
 *
 * BOTH OBVIOUS RECOVERIES ARE WRONG. Holding the throttle off the
 * band PINS a bike against a
 * wall - fifteen seconds at 3 to 57 km/h going nowhere. Cutting it
 * is worse: speed decays, the bars stop answering, and a direction
 * key gets pressed into a dead actuator for ever.
 */

/*
 * THE STUCK BACKSTOP COUNTS BOTH WAYS. DO NOT RESET IT ON SPEED: a
 * bike grinding along a wall bounces through any threshold every
 * few frames, so a reset clears the counter before it can reach its
 * limit and the backstop never fires. Rising two and falling one
 * means a bike that is mostly stopped accumulates even while it
 * twitches.
 */
#define AP_STUCK_KMH        20
#define AP_STUCK_RISE       2       /* ms of credit per ms stopped  */
#define AP_STUCK_LIMIT_MS   2000

/*
 * WHICH WAY A POSITIVE BAR ANGLE SWINGS THE NOSE, +1 meaning toward
 * the bike's +row0. MEASURED, not guessed - and the measurement is
 * on record rather than being repeated every run.
 *
 * DO NOT LEARN IT AT RUN TIME. Accumulating
 * sum += dot(fwd - fwdPrev, rightPrev) * barPrev does work - it is
 * how the value below was obtained, over a clean run of ile01
 * segments 3 to 113 - but the failure mode is unrecoverable. In a
 * long constant-radius corner the nose swing is dominated by the
 * CORNER rather than by the bars, so the product can stay negative
 * long enough to flip a sum that has no forgetting and no
 * normalisation.
 *
 * AND A FLIP IS SELF-REINFORCING. sign picks the steering axis and
 * the button pair, so inverting it makes every correction steer
 * AWAY from the target, which grows the demand, which steers
 * further away: the bars go to full lock and stay there. Logged at
 * segment 113, then fifteen seconds pinned against a wall 500 units
 * outside the band.
 *
 * A measurement worth making once is not worth making every frame.
 */
#define AP_TURN_SIGN        1.0f

/*
 * POINTING THE WRONG WAY IS ITS OWN STATE.
 *
 * aimDot is the SIDEWAYS component of the bearing to the target. On
 * its own it cannot tell ahead from behind: a bike facing EXACTLY
 * BACKWARDS reads aimDot close to zero, which the deadband accepts
 * as "aligned, drive on". So the worst possible heading looked like
 * the best one, and the throttle stayed in.
 *
 * The log caught it where the track crosses a bridge and narrows -
 * segments 415 to 423 on ile01, aim flipping between +0.99 and
 * -0.99 sample to sample at 1 to 10 km/h, the segment index walking
 * BACKWARDS, and the accel bit set through all of it. The bike was
 * facing across the road being driven into the wall it was already
 * against.
 *
 * fwdDot - the forward component - is what distinguishes them:
 *
 *     >  AP_ALIGN_DOT   pointing roughly at the target, drive
 *     <= AP_ALIGN_DOT   turn first, and carry only enough speed to
 *                       steer with (AP_TURN_KMH)
 *
 * and while turning the demand SATURATES to the side aimDot names,
 * so the bars go over properly instead of dithering around a
 * near-zero sideways component.
 */
#define AP_ALIGN_DOT        0.35f   /* about 70 degrees off        */
#define AP_TURN_KMH         35      /* enough for the bars to work */

/*
 * AND A FLOOR UNDER ALL OF IT. The off-line penalty SCALES the
 * profile speed and the turn-around cap REPLACES it - they do not
 * all compound, because stacking them asked for 19 km/h off the
 * road in a corner facing the wrong way, which is barely above the
 * 2 km/h at which the bars stop answering.
 *
 * The floor still has work to do: step 2's backward pass can take a
 * segment below AP_SPEED_MIN, and the off-line scale then takes it
 * lower. Nothing below this is ever a useful thing to ask for.
 */
#define AP_MIN_KMH          30

/* The game's own bits, from racer+0x31e9 - see race_poll_input. */
#define AP_BTN_ACCEL        1u
#define AP_BTN_BRAKE        2u
#define AP_BTN_LEFT         4u
#define AP_BTN_RIGHT        8u

/* raceState values, as race.c defines them. */
#define AP_RACING_0         0
#define AP_RACING_1         1


/*
 * The latched state, which is the whole reason autopilot_reset exists.
 *
 * IT CLEARS THE GATES, NOT EVERYTHING. s_probeWord and s_probeAxis
 * are read only while s_probeHoldMs > 0, and s_curveScale only after
 * ap_curve_scale has seen a new s_curveTrk - so zeroing those two is
 * enough to make the rest unreachable. Not an oversight.
 */
static float s_stuckMs;
static int   s_engaged;
static unsigned int s_probeWord;
static int          s_probeAxis;
static float        s_probeHoldMs;
static float        s_probeMs;

/* The tightest local curvature on the loaded track - see
 * ap_curve_scale. Recomputed when the track pointer changes. */
static const MR_Trk *s_curveTrk;
static float         s_curveScale = 1.0f;
static float        *s_vlim;      /* km/h per segment */


/* ORIGIN  NONE - ours */
void autopilot_reset(void)
{
	s_stuckMs     = 0.0f;
	s_engaged     = 0;
	s_probeHoldMs = 0.0f;
	s_probeMs     = 0.0f;
	s_curveTrk    = NULL;
	free(s_vlim);
	s_vlim        = NULL;
}


/*
 * ORIGIN  NONE - ours
 *
 * The signed distance from a segment's centreline, along matrix row 0.
 *
 * THE FORMULA IS trk_surface_at_point's, not a new one - it is the
 * projection that function performs before testing the band, lifted
 * so the distance itself can be used rather than the surface id it
 * returns. Keeping them identical matters: the autopilot's notion of
 * "on the road" is then the GAME's, band and all.
 */
static float ap_lateral_of(const MR_TrkSegment *s, const float p[3])
{
	const float *n = &s->matrix[0];

	return n[0] * (p[0] - s->pos[0])
	     + n[1] * (p[1] - s->pos[1])
	     + n[2] * (p[2] - s->pos[2])
	     - (n[0] * s->matrix[9] + n[1] * s->matrix[10]
	        + n[2] * s->matrix[11]);
}


/* ORIGIN  NONE - ours. A segment's centre in world space: its origin
 * plus the row-3 translation, which is how the grid builder reads it. */
static void ap_segment_centre(const MR_TrkSegment *s, float out[3])
{
	out[0] = s->pos[0] + s->matrix[9];
	out[1] = s->pos[1] + s->matrix[10];
	out[2] = s->pos[2] + s->matrix[11];
}


/* ORIGIN  NONE - ours. Normalise an xz pair in place. */
static void ap_norm2(float v[2])
{
	float len = (float)sqrt((double)v[0] * v[0] + (double)v[1] * v[1]);

	if (len != 0.0f) {
		v[0] /= len;
		v[1] /= len;
	}
}


/*
 * ORIGIN  NONE - ours
 *
 * The bike's forward, flattened.
 *
 * matOrientation[9..11] + [6..8] IS THE GAME'S OWN FORWARD, and it is
 * taken from racer_apply_drive rather than invented - that function
 * builds out->dir from exactly this sum and normalises it. Using a
 * different row would steer against a bike that thinks it is pointing
 * somewhere else.
 *
 * The y component is dropped because the steering decision is a
 * heading, and a crest or a dip should not read as a turn.
 */
static void ap_bike_forward(const MR_Racer *r, float out[2])
{
	out[0] = r->matOrientation[9] + r->matOrientation[6];
	out[1] = r->matOrientation[11] + r->matOrientation[8];
	ap_norm2(out);
}


/*
 * ORIGIN  NONE - ours
 *
 * The bike's own lateral axis - matOrientation row 0, the same row
 * trk_surface_at_point uses on a segment. maybe_Game_InitRace copies
 * the segment matrix straight into matOrientation, so the two agree
 * about which way "+row0" points while the bike is on the track.
 *
 * EVERYTHING IS MEASURED IN THIS AXIS rather than in left and right,
 * and that is the point: a dot product against it needs no handedness
 * convention and no guess about which way a cross product runs. The
 * ONE thing that cannot be derived - whether a positive bar angle
 * swings the nose toward +row0 or away - is AP_TURN_SIGN, measured
 * once and recorded rather than assumed.
 */
static void ap_bike_right(const MR_Racer *r, float out[2])
{
	out[0] = r->matOrientation[0];
	out[1] = r->matOrientation[2];
	ap_norm2(out);
}


/* ORIGIN  NONE - ours. Signed angle between two xz pairs; only its
 * magnitude is used, so no handedness convention is implied. */
static float ap_signed_angle(const float a[2], const float b[2])
{
	float dot   = a[0] * b[0] + a[1] * b[1];
	float cross = a[0] * b[1] - a[1] * b[0];

	return (float)atan2((double)cross, (double)dot);
}


/*
 * ORIGIN  NONE - ours
 *
 * A segment's forward, taken from the GEOMETRY rather than from a
 * matrix row.
 *
 * THIS IS DELIBERATE AND IT IS NOT THE OBVIOUS CHOICE. Row 0 is the
 * lateral axis - trk_surface_at_point and the grid builder both use
 * it that way, verified - but which of the remaining rows is
 * "forward" is NOT settled. The racer builds its own heading as
 * matOrientation row3 + row2 going forward and row3 - row2 in
 * reverse (maybe_Game_InitRace), so row 3 is part of the direction
 * sum even though the grid code treats the same row as a plain
 * translation. Picking a row here would be a guess.
 *
 * The difference between two segment centres needs no such guess: it
 * is which way the track goes, by construction, and it carries the
 * race direction for free.
 */
static void ap_segment_forward(const MR_Trk *t, int seg, int dir,
                               float out[2])
{
	float a[3];
	float b[3];

	ap_segment_centre(&t->segments[trk_wrap_segment_index(t, seg)], a);
	ap_segment_centre(&t->segments[trk_wrap_segment_index(t, seg + dir)],
	                  b);

	out[0] = b[0] - a[0];
	out[1] = b[2] - a[2];
	ap_norm2(out);
}


/*
 * ORIGIN  NONE - ours
 *
 * THE TIGHTEST BIT OF ROAD within the braking window.
 *
 * Local curvature is the angle between the track's forward here and
 * its forward AP_CURVE_STEP segments on; the walk takes the largest
 * of those over the whole window. Measuring between two forwards
 * rather than by sampling points keeps a long segment and a short
 * one weighing the same.
 *
 * TAKING THE MAXIMUM IS THE POINT. Any measure that looks only at
 * where the window ends is blind to an S-bend - the two halves
 * cancel and the road reads as straight - and to a tight corner
 * with an aligned exit. Both are places to be slow.
 */
static float ap_curvature(const MR_Trk *t, int seg, int dir, int window)
{
	float worst = 0.0f;
	int   i;

	for (i = 0; i <= window; i++) {
		float fa[2];
		float fb[2];
		float c;

		ap_segment_forward(t, seg + i * dir, dir, fa);
		ap_segment_forward(t, seg + (i + AP_CURVE_STEP) * dir, dir, fb);

		if ((fa[0] == 0.0f && fa[1] == 0.0f) ||
		    (fb[0] == 0.0f && fb[1] == 0.0f)) {
			continue;
		}
		c = (float)fabs((double)ap_signed_angle(fa, fb));
		if (c > worst)
			worst = c;
	}
	return worst;
}


/*
 * ORIGIN  NONE - ours
 *
 * THE NARROWEST THE ROAD GETS within the look-ahead.
 *
 * DO NOT NORMALISE THE LATERAL ERROR AGAINST THE HALF WIDTH WHERE
 * THE BIKE IS - that is always too late. ile01 tapers from 1024 to
 * 512 over segments 401 to 403 and again to 384 at 424, so a bike
 * sitting 900 units out is comfortably inside the road at 400 and
 * 400 units beyond the wall by 404, having done nothing wrong in
 * between.
 *
 * So the error is measured against the TIGHTEST width it is about
 * to meet. Being 900 out reads as off the road while there is still
 * road to correct on, which is the whole point.
 */
static float ap_min_half(const MR_Trk *t, int seg, int dir, int window,
                         int positive)
{
	float worst = 1.0e9f;
	int   i;

	for (i = 0; i <= window; i++) {
		const MR_TrkSegment *sg =
		    &t->segments[trk_wrap_segment_index(t, seg + i * dir)];
		float h = positive ? sg->bandHi : -sg->bandLo;

		if (h > 1.0f && h < worst)
			worst = h;
	}
	if (worst > 1.0e8f)
		worst = 1.0f;
	return worst;
}




/*
 * ORIGIN  NONE - ours
 *
 * WHICH WAY the track turns here and by how much, over
 * AP_CURVE_STEP segments.
 *
 * EVERY CONSUMER TAKES THE MAGNITUDE - the speed profile, the
 * look-ahead shortening and ap_curvature all wrap it in fabs, and
 * there is no feedforward left to feed. [open] NOTHING READS THE
 * SIGN AT ALL: autopilot_input's ffCurve is the only signed caller
 * and it is a dead store - see there.
 */
static float ap_signed_curvature(const MR_Trk *t, int seg, int dir)
{
	float fa[2];
	float fb[2];

	ap_segment_forward(t, seg, dir, fa);
	ap_segment_forward(t, seg + AP_CURVE_STEP * dir, dir, fb);

	if ((fa[0] == 0.0f && fa[1] == 0.0f) ||
	    (fb[0] == 0.0f && fb[1] == 0.0f)) {
		return 0.0f;
	}
	return ap_signed_angle(fa, fb);
}




/*
 * ORIGIN  NONE - ours
 *
 * THE SPEED PROFILE - the fastest this bike may be travelling at
 * every segment of the track, built once and then read.
 *
 * WHY A TABLE AND NOT A LOOK-AHEAD. Arriving at a corner too fast
 * is not a control failure, it is a PLANNING one: the brakes have
 * to come on long before anything a short window can see. Going
 * from 170 to 65 takes 20562 units at AP_DECEL - about forty of
 * ile01's segments - and a "worst curvature within N segments" test
 * can only ever answer yes or no, never "start easing now".
 *
 * TWO STEPS, the standard racing-line construction:
 *
 *   1  every segment gets the speed its OWN curvature allows
 *   2  a BACKWARD pass drags each one down until it is slow enough
 *      to reach its neighbour's limit:
 *      v = sqrt(vnext^2 + AP_DECEL*d), AP_DECEL being the 2a
 *
 * Step 2 is what turns a corner into a braking zone that starts in
 * the right place. Three passes because the track is a LOOP - a
 * constraint can propagate past the start - and three is enough for
 * the wrap to settle.
 *
 * A PASS BEFORE BOTH finds the curvature scale - the tightest
 * corner on this track - because the schedule is relative to it: a
 * threshold in radians does not transfer between tracks, and
 * ile04's MEDIAN corner is tighter than ile01's worst. It needs the
 * whole track before step 1 can use it, so it cannot be folded into
 * step 1's own loop.
 */
static void ap_build_profile(const MR_Trk *t, int dir, float vmax)
{
	int   n = t->segmentCount;
	int   pass;
	int   i;
	float worst = 0.0f;

	if (s_curveTrk == t)
		return;

	free(s_vlim);
	s_vlim     = NULL;
	s_curveTrk = t;

	if (n <= 0)
		return;

	/* ---- the curvature scale: the tightest corner here ------- */
	for (i = 0; i < n; i++) {
		float fa[2];
		float fb[2];
		float c;

		ap_segment_forward(t, i, dir, fa);
		ap_segment_forward(t, i + AP_CURVE_STEP * dir, dir, fb);
		if ((fa[0] == 0.0f && fa[1] == 0.0f) ||
		    (fb[0] == 0.0f && fb[1] == 0.0f)) {
			continue;
		}
		c = (float)fabs((double)ap_signed_angle(fa, fb));
		if (c > worst)
			worst = c;
	}
	if (worst < AP_CURVE_SCALE_MIN)
		worst = AP_CURVE_SCALE_MIN;
	s_curveScale = worst;

	s_vlim = (float *)malloc((size_t)n * sizeof(float));
	if (!s_vlim) {
		mrlog_err("auto:   no room for a %d segment speed profile\n", n);
		return;
	}

	/* ---- step 1: what each segment's own curvature allows ---- */
	for (i = 0; i < n; i++) {
		float c = (float)fabs((double)
		                      ap_signed_curvature(t, i, dir));
		float arc = 0.0f;
		float v;
		int   k;

		/*
		 * THE ARC IS MEASURED, not assumed. Segment lengths
		 * vary - ile01 runs 327 to 512 - and the radius is
		 * arc over angle, so using a nominal length would
		 * misjudge exactly the short segments that corners
		 * are made of.
		 */
		for (k = 0; k < AP_CURVE_STEP; k++) {
			float a[3];
			float b[3];

			ap_segment_centre(&t->segments[
			    trk_wrap_segment_index(t, i + k * dir)], a);
			ap_segment_centre(&t->segments[
			    trk_wrap_segment_index(t, i + (k + 1) * dir)], b);
			arc += (float)sqrt((double)(b[0] - a[0]) * (b[0] - a[0])
			                   + (double)(b[2] - a[2]) * (b[2] - a[2]));
		}

		if (c > 1.0e-4f && arc > 1.0f) {
			v = (float)sqrt((double)AP_LAT_ACCEL * arc / c);
		} else {
			v = vmax;
		}
		if (v > vmax)
			v = vmax;
		if (v < (float)AP_SPEED_MIN)
			v = (float)AP_SPEED_MIN;
		s_vlim[i] = v;
	}

	/* ---- step 2: the backward pass --------------------------- */
	for (pass = 0; pass < 3; pass++) {
		for (i = 0; i < n; i++) {
			int   here = trk_wrap_segment_index(t, -i * dir);
			int   next = trk_wrap_segment_index(t, here + dir);
			float a[3];
			float b[3];
			float d;
			float v;

			ap_segment_centre(&t->segments[here], a);
			ap_segment_centre(&t->segments[next], b);
			d = (float)sqrt((double)(b[0] - a[0]) * (b[0] - a[0])
			                + (double)(b[2] - a[2]) * (b[2] - a[2]));

			v = (float)sqrt((double)s_vlim[next] * s_vlim[next]
			                + (double)AP_DECEL * d);
			if (v < s_vlim[here])
				s_vlim[here] = v;
		}
	}

	{
		float lo = s_vlim[0];
		float hi = s_vlim[0];

		for (i = 1; i < n; i++) {
			if (s_vlim[i] < lo) lo = s_vlim[i];
			if (s_vlim[i] > hi) hi = s_vlim[i];
		}
		mrlog("auto:   profile %d segs, cap %.0f kmh, scale %.3f, "
		      "speed %.0f..%.0f kmh\n", n, (double)vmax,
		      (double)s_curveScale, (double)lo, (double)hi);
	}
}




/*
 * ORIGIN  NONE - ours
 *
 * A bar fraction in the bike's +row0 space, as the game's analog
 * axis. 0x8000 is centred and the span either side is the lock
 * stop; sign carries it into the engine's convention, where a
 * positive steering value is a right turn.
 *
 * NEVER RETURNS DEAD CENTRE. That value means "no stick" and would
 * drop the engine back onto the digital path mid-corner.
 */
static int ap_axis_of(float b, float sign)
{
	int a;

	if (b >  1.0f) b =  1.0f;
	if (b < -1.0f) b = -1.0f;

	a = AP_AXIS_CENTRE + (int)(b * sign * (float)AP_AXIS_SPAN);
	if (a < 0)      a = 0;
	if (a > 0xffff) a = 0xffff;
	if (a == AP_AXIS_CENTRE)
		a = AP_AXIS_CENTRE + 1;
	return a;
}


/*
 * ORIGIN  NONE - ours
 *
 * Score one candidate - a button word and a steering angle -
 * by simulating it.
 *
 * DISTANCE MADE GOOD toward the aim direction, plus the speed still
 * on the bike at the end. A wall costs both - it stops the bike
 * short AND scrubs the speed - so one number covers hitting things
 * without needing to ask whether anything was hit.
 *
 * A CRASH IS NOT A SCORE, it is a disqualification, which is why the
 * penalty is enormous rather than merely large.
 */
static float ap_probe_score(unsigned int buttons, int axis1,
                            const float to[2], const float from[3],
                            float dtMs, float *endSpeed)
{
	MR_AutoProbe pr;
	float        dx;
	float        dz;

	if (endSpeed)
		*endSpeed = -1.0f;

	int ticks;

	/*
	 * A fixed horizon in TIME - see AP_PROBE_HORIZON_MS.
	 *
	 * THE 40-TICK CAP PUTS THE FRAME DEPENDENCE BACK ABOVE ABOUT
	 * 133 fps, where 300 ms no longer fits in 40 ticks: the horizon
	 * falls to 278 ms at 144 and 167 ms at 240. It is a bound on
	 * the cost of one probe, not a tuning knob, and the rollout is
	 * real physics - 40 ticks is already 40x a frame's work. Raise
	 * it and the veto gets slower in exactly the frames where it
	 * has least time.
	 */
	ticks = (int)((float)AP_PROBE_HORIZON_MS / dtMs);
	if (ticks < 2)
		ticks = 2;
	if (ticks > 40)
		ticks = 40;

	if (!race_autopilot_probe(buttons, axis1, ticks, 0.0f, &pr))
		return 0.0f;

	if (endSpeed)
		*endSpeed = pr.speed;

	if (pr.crashed)
		return -AP_PROBE_CRASH;

	dx = pr.position[0] - from[0];
	dz = pr.position[2] - from[2];

	return dx * to[0] + dz * to[1] + AP_PROBE_SPEED_K * pr.speed;
}


/*
 * ORIGIN  NONE - ours
 *
 * THE ENTRY POINT. Same contract as agent_input_get: 1 means "I am
 * driving, use this word", 0 means "read the keyboard".
 */
int autopilot_input(const MR_Racer *r, const MR_Trk *t,
                    const MR_Racer *racers, int racerCount,
                    unsigned int *buttons, int *stunt, int *axis1)
{
	const MR_TrkSegment *seg;
	const MR_BikeCarac  *carac;
	unsigned int         w = 0;
	float                aim[3];
	float                fwd[2];
	float                right0[2];
	float                to[2];
	float                lateral;
	float                offLine;
	float                half;
	float                offFrac;
	float                frameSign;
	float                aimDot;
	float                fwdDot;
	float                aimEff;
	float                bar = 0.0f;
	float                sign;
	float                want;
	float                curve;
	float                ffCurve;
	float                barWant;
	int                  ax;
	float                dtMs;
	unsigned int         toPlus;
	unsigned int         toMinus;
	int                  dir;
	int                  here;
	int                  ahead;
	int                  look;
	int                  lookWide;
	int                  target;
	int                  speed;

	(void)racers;
	(void)racerCount;

	if (!buttons)
		return 0;
	if (axis1)
		*axis1 = AP_AXIS_CENTRE;

	if (!g_autopilot || !r || !t || !t->segments || t->segmentCount <= 0) {
		if (s_engaged) {
			mrlog("auto:   disengaged\n");
			s_engaged   = 0;
		}
		return 0;
	}

	/*
	 * PAST THE LINE, HANDS OFF - and this is not tidiness.
	 * racer_apply_drive forces buttons = INPUT_BRAKE for a finished
	 * human, and THAT FORCED BRAKE IS THE COAST-TO-STOP the finish
	 * camera plays over. Driving through it would replace the exact
	 * behaviour this was built to look at.
	 */
	if (r->raceState != AP_RACING_0 && r->raceState != AP_RACING_1) {
		if (s_engaged) {
			mrlog("auto:   crossed the line, handing back\n");
			s_engaged   = 0;
		}
		return 0;
	}

	if (!s_engaged) {
		mrlog("auto:   engaged\n");
		s_engaged = 1;
	}

	/*
	 * WHILE DOWN, HOLD NOTHING. The remount is a state machine with
	 * its own timer; leaning on the throttle through it does not
	 * speed it up and does change what it sees on the way out.
	 */
	if (r->flCrashTimerMs > 0.0f) {
		*buttons = 0;
		if (stunt)
			*stunt = 0;
		s_stuckMs     = 0.0f;
		return 1;
	}

	/*
	 * THE FRAME'S OWN LENGTH IN MILLISECONDS. race_frame_delta
	 * hands back dtScaled, which is a TENTH of the millisecond
	 * count - 1.667 at a 16.7 ms frame, 3.3 at 33 ms - so ten
	 * times it is the real step. race.c has the same conversion
	 * as RACE_DT_UNSCALE; see racers.txt 9.2.
	 *
	 * Every cadence here is measured against it; see AP_PROBE_MS
	 * for why none of them count frames.
	 */
	dtMs = race_frame_delta() * 10.0f;
	if (dtMs <= 0.0f)
		dtMs = 16.7f;

	dir  = g_reverseEnabled ? -1 : 1;
	/*
	 * THE CEILING COMES OFF THE BIKE. realSpeedMax is in m/s
	 * and is per-bike, so a pocket bike gets its own profile
	 * rather than a superbike's - see AP_SPEED_KMH_PER.
	 */
	ap_build_profile(t, dir,
	                 (r->realSpeedMax > 1.0f)
	                 ? r->realSpeedMax * AP_SPEED_KMH_PER
	                 : 170.0f);
	here = trk_wrap_segment_index(t, r->segmentStart);
	seg  = &t->segments[here];

	/*
	 * r->segmentStart IS THE CURRENT SEGMENT, not the grid slot the
	 * name suggests - race_update_progress overwrites it every frame
	 * from trk_find_segment_for_point. It is one frame stale here
	 * because race_poll_input runs first, which a look-ahead
	 * controller does not care about.
	 */
	look = AP_LOOK_MIN + r->speedKmhNeedle / AP_LOOK_KMH_PER_SEG;
	if (look > AP_LOOK_MAX)
		look = AP_LOOK_MAX;
	/*
	 * KEPT BEFORE THE CORNER SHORTENING. The width scan wants
	 * the LONG view - the whole point of it is to see a
	 * narrowing coming - while the aim point wants the short
	 * one. Sharing a look-ahead would trade one for the other.
	 */
	lookWide = look;

	/*
	 * PULL IT BACK INSIDE THE CORNER - see AP_LOOK_CURVE_MIN.
	 * Measured at the look-ahead the speed alone asked for, so
	 * a bend that the aim point has ALREADY overshot is what
	 * shortens it.
	 */
	{
		float c = (float)fabs((double)ap_signed_curvature(t,
		               trk_wrap_segment_index(t, here + look * dir),
		               dir)) / s_curveScale;
		float k;

		if (c > 1.0f)
			c = 1.0f;
		k = 1.0f - c * (1.0f - AP_LOOK_CURVE_MIN);
		look = (int)((float)look * k);
		if (look < AP_LOOK_MIN)
			look = AP_LOOK_MIN;
	}
	ahead = trk_wrap_segment_index(t, here + look * dir);

	/*
	 * AIM AT THE GAME'S OWN RACING LINE, NOT THE MIDDLE OF THE
	 * ROAD. racer_ai_target_point is what the AI follows:
	 *
	 *     offset = segment +0xb0 + chargeMeter
	 *     target = origin + row0 * offset + row3
	 *
	 * Row 0 is the lateral axis, so the segment field is the
	 * line's own displacement ACROSS the road - authored per
	 * segment, and on a narrow section it is emphatically not
	 * the centre. A controller aiming at the centreline there
	 * has to be wrong on one side or the other, which is what
	 * bounces a bike wall to wall through the narrows.
	 *
	 * chargeMeter 0 IS THE LINE ITSELF. An AI carries its own
	 * displacement on top so a field drives a fan of parallel
	 * lines; this wants the middle of that fan.
	 */
	racer_ai_target_point(t, ahead, 0.0f, aim);

	ap_bike_forward(r, fwd);
	ap_bike_right(r, right0);

	to[0] = aim[0] - r->position[0];
	to[1] = aim[2] - r->position[2];
	ap_norm2(to);

	/* + means the aim point lies toward the bike's own +row0. */
	aimDot  = to[0] * right0[0] + to[1] * right0[1];
	/* ...and this says whether it is ahead at all. */
	fwdDot  = to[0] * fwd[0] + to[1] * fwd[1];

	/*
	 * FACING THE WRONG WAY: SATURATE, do not trust aimDot's
	 * magnitude. At 180 degrees it is near zero and reads as
	 * aligned - see AP_ALIGN_DOT. Its SIGN is still the side
	 * to turn toward, and at exactly 180 either side is as
	 * good, so the tie goes right and the bike commits.
	 */
	aimEff = aimDot;
	if (fwdDot <= AP_ALIGN_DOT)
		aimEff = (aimDot >= 0.0f) ? 1.0f : -1.0f;
	/*
	 * AND THE CENTRELINE TERM MEASURES FROM THE LINE AS WELL.
	 * ap_lateral_of gives the displacement from the segment
	 * centre; subtracting the segment's own line offset turns
	 * it into the displacement from where the bike is MEANT to
	 * be. Leaving it against the centre would have the two
	 * terms pull to different places on every cambered or
	 * narrowed segment.
	 *
	 * lateral KEEPS ITS CENTRE MEANING for the band test
	 * below, because bandLo/bandHi are measured from the
	 * centre - that is the GAME's road test and it is not
	 * relative to the line.
	 */
	lateral = ap_lateral_of(seg, r->position);
	offLine = lateral - seg->lineOffset;

	/*
	 * THE ROAD'S OWN HALF WIDTH on the side the bike is on.
	 * Everything relative - the steering gain and the speed
	 * penalty - is scaled by it, because the road narrows to a
	 * third of its width in places and a controller tuned in
	 * world units does not notice.
	 */
	half = ap_min_half(t, here, dir, lookWide,
	                   (lateral > 0.0f));
	offFrac = offLine / half;
	/*
	 * SATURATED AT TWICE THE HALF WIDTH. Past that the bike is off
	 * the road and the recovery states own it; letting the term
	 * keep growing would just ask for more lock than the stop has.
	 */
	if (offFrac >  2.0f) offFrac =  2.0f;
	if (offFrac < -2.0f) offFrac = -2.0f;

	/*
	 * AND THEN INTO THE BIKE'S FRAME, WHICH IS NOT ALWAYS THE
	 * SEGMENT'S. Get this wrong and reverse mode drifts into a
	 * wall.
	 *
	 * lateral and offLine are measured against the SEGMENT's
	 * row 0, and they have to be: bandLo and bandHi live in
	 * that frame and ap_lateral_of deliberately mirrors
	 * trk_surface_at_point. aimDot is measured against the
	 * BIKE's row 0. Going forwards those are the same axis,
	 * because maybe_Game_InitRace copies the segment matrix
	 * straight into matOrientation.
	 *
	 * IN REVERSE THEY ARE OPPOSITE. The reverse arm builds
	 * row0 = worldUp x forward with forward negated, so the
	 * bike's row0 is minus the segment's. Summing a bike-frame
	 * aim term with a segment-frame cross-track term then
	 * pushes AWAY from the line, and it hides well: offLine is
	 * near zero at the start so the term is tiny, and it grows
	 * as the bike wanders until the road stops it.
	 *
	 * THE RELATIONSHIP IS MEASURED, NOT ASSUMED FROM THE
	 * REVERSE FLAG. A dot product of the two axes says which
	 * way they actually lie, which stays right whatever else
	 * ends up rotating a racer's frame.
	 */
	frameSign = (seg->matrix[0] * right0[0]
	             + seg->matrix[2] * right0[1] >= 0.0f)
	            ? 1.0f : -1.0f;
	offFrac *= frameSign;

	/*
	 * bar is LOGGED ONLY - no term uses it. It was the damping
	 * term's input; with the analog path there is nothing to damp.
	 */
	carac = bike_at(r->bikeIndex);
	if (carac && carac->roueLimiteAngulaire != 0.0f)
		bar = r->steering / carac->roueLimiteAngulaire;

	sign = AP_TURN_SIGN;

	ffCurve = ap_signed_curvature(t, ahead, dir);

	/*
	 * THE DEMAND, all of it in "+row0" units so that no term
	 * needs a handedness convention:
	 *
	 *     aimEff        where the track goes
	 *   - gain*offLine  pull back toward the racing line
	 *
	 * [open] ffCurve IS A DEAD STORE. It is not a term - the
	 * feedforward it was named for is gone - and it is not
	 * logged either: the diagnostic below prints curve, from
	 * ap_curvature. Either put it in the log, which is what
	 * signed curvature at the aim point is good for, or drop
	 * it.
	 */
	want = aimEff - AP_LATERAL_K * offFrac;

	toPlus  = (sign > 0.0f) ? AP_BTN_RIGHT : AP_BTN_LEFT;
	toMinus = (sign > 0.0f) ? AP_BTN_LEFT  : AP_BTN_RIGHT;

	/*
	 * THE BAR ANGLE, ASKED FOR DIRECTLY - autopilot.h. want is
	 * already a signed demand in the bike's +row0 space, so it
	 * only needs scaling and clamping.
	 *
	 * IT REACHES FULL LOCK WHERE THE DEMAND IS LARGE, which
	 * is what the engine wants - see AP_BAR_GAIN, and see
	 * AP_STEER_ON for why there is nothing to damp.
	 */
	{
		float b = want * AP_BAR_GAIN;

		/* The lock stop is the only clamp - see AP_BAR_GAIN. */
		if (b >  1.0f) b =  1.0f;
		if (b < -1.0f) b = -1.0f;
		barWant = b;

		/*
		 * AND THE DIGITAL BITS AGREE WITH IT. They no longer
		 * steer - the analog angle does - but the steer-torque
		 * helper reads them to pin grip when the brake is held
		 * with a direction, so they must tell the same story.
		 */
		if (b > AP_STEER_OFF)
			w |= toPlus;
		else if (b < -AP_STEER_OFF)
			w |= toMinus;
	}

	/*
	 * THE PROFILE ALREADY KNOWS. It was built backwards from
	 * every corner on the track, so the number here has the
	 * braking zone baked into it - no window to scan and no
	 * look-ahead to get wrong.
	 */
	curve  = ap_curvature(t, here, dir, AP_CURVE_STEP);
	/*
	 * NO PROFILE MEANS THE ALLOCATION FAILED, which is the
	 * only way s_vlim is null. Crawl rather than keep a
	 * second speed model alive for a case that never
	 * happens - two models in one file is how they drift.
	 */
	target = s_vlim ? (int)s_vlim[here] : AP_SPEED_MIN;
	speed  = r->speedKmhNeedle;

	/*
	 * THREE THINGS CUT THE TARGET, AND THEY DO NOT ALL STACK.
	 * In order:
	 *
	 *   o BEING OFF LINE scales it. The curvature schedule only
	 *     asks how tight the corner is, so without this a bike
	 *     already at the verge carries full corner speed into
	 *     the recovery.
	 *   o FACING THE WRONG WAY REPLACES it rather than scaling
	 *     again. Do not accelerate toward a target you are not
	 *     pointing at - carry just enough to turn with.
	 *   o THE FLOOR catches what is left. Below AP_MIN_KMH the
	 *     bars barely answer, so asking for less is asking to
	 *     be stuck.
	 */
	{
		float off = (float)fabs((double)offFrac);

		/* Nothing until AP_OFFLINE_FREE, then ramp. */
		off = (off - AP_OFFLINE_FREE) / (1.0f - AP_OFFLINE_FREE);
		if (off < 0.0f)
			off = 0.0f;
		if (off > 1.0f)
			off = 1.0f;
		target = (int)((float)target *
		               (1.0f - off * (1.0f - AP_OFFLINE_SCALE)));
	}
	if (fwdDot <= AP_ALIGN_DOT)
		target = AP_TURN_KMH;
	if (target < AP_MIN_KMH)
		target = AP_MIN_KMH;

	if (speed < target - AP_SPEED_BAND)
		w |= AP_BTN_ACCEL;
	else if (speed > target + AP_SPEED_BAND)
		w |= AP_BTN_BRAKE;
	/* else coast - neither bit, which racer_apply_drive reads as 0
	 * demand rather than as a brake. */

	/*
	 * OFF THE BAND: FULL LOCK FOR THE LINE.
	 *
	 * The band is the GAME's own road test - bandLo/bandHi, +-1024
	 * against walls at 1280 - so leaving it means the bike is on
	 * the verge, not merely off the racing line. This commits the
	 * angle rather than trusting the pursuit term, which is looking
	 * at a point the bike may not be able to reach from here.
	 *
	 * IT USES THE SAME sign AS EVERYTHING ELSE, and must. A block
	 * that spells its own left and right contradicts the main path
	 * - positive lateral steering RIGHT above and LEFT here - so
	 * the recovery drives the bike further off whenever it fires.
	 *
	 * IT SETS THE THROTTLE NOWHERE. One place owns that - the
	 * speed chain, which already knows it is off line and which
	 * way the bike faces, and has a floor under it.
	 */
	if (lateral <= seg->bandLo || lateral >= seg->bandHi) {
		/* offLine is segment-frame - see frameSign above. */
		float outward = offLine * frameSign;

		barWant = (outward > 0.0f) ? -1.0f : 1.0f;
		w &= ~(AP_BTN_LEFT | AP_BTN_RIGHT);
		w |= (outward > 0.0f) ? toMinus : toPlus;
	}

	if (speed < AP_STUCK_KMH)
		s_stuckMs += dtMs * AP_STUCK_RISE;
	else if (s_stuckMs > 0.0f)
		s_stuckMs -= dtMs;
	if (s_stuckMs < 0.0f)
		s_stuckMs = 0.0f;
	/*
	 * CREDIT CAPPED AT TWICE THE LIMIT, so a long stop cannot buy
	 * minutes of throttle to unwind at one ms per ms once the bike
	 * is moving again.
	 */
	if (s_stuckMs > (float)(AP_STUCK_LIMIT_MS * 2))
		s_stuckMs = (float)(AP_STUCK_LIMIT_MS * 2);

	/*
	 * STUCK: THROTTLE WINS, and it overrides the off-band arm
	 * above on purpose. A bike that has genuinely stopped needs
	 * speed before anything else it might want to do becomes
	 * possible, because the bars are dead below 2 km/h.
	 */
	if (s_stuckMs > (float)AP_STUCK_LIMIT_MS) {
		w &= ~AP_BTN_BRAKE;
		w |= AP_BTN_ACCEL;
	}

	ax = ap_axis_of(barWant, sign);

	/*
	 * THE VETO. Ask the real physics where this word leads; if
	 * it leads into a wall, try the obvious alternatives and
	 * take whichever ends up best. See AP_PROBE_HORIZON_MS.
	 */
	{
		s_probeMs += dtMs;

		if (s_probeMs >= (float)AP_PROBE_MS) {
			s_probeMs = 0.0f;
			unsigned int cand[5];
			int          cax[5];
			float        best;
			float        endV = -1.0f;
			int          bestI = 0;
			int          ci;

			/*
			 * THE CONTROLLER'S OWN CHOICE FIRST, so a tie
			 * always leaves it alone - the probe may only
			 * overrule on a strictly better outcome.
			 */
			/*
			 * FOUR ALTERNATIVES: two throttle, two steering.
			 *
			 * THE STEERING ONES VARY THE ANGLE, NOT THE BUTTON
			 * BITS. The bits have not steered anything since the
			 * move to the analog axis, so a candidate that only
			 * flips them simulates the same steering as the
			 * controller's own choice and leaves the throttle as
			 * the sole variable.
			 *
			 * MORE LOCK AND LESS, not left and right: the
			 * controller has already decided which way to go and
			 * is far more often wrong about HOW HARD.
			 */
			cand[0] = w;
			cax[0]  = ax;
			cand[1] = (w & ~AP_BTN_ACCEL) | AP_BTN_BRAKE;
			cax[1]  = ax;
			cand[2] = w & ~(AP_BTN_ACCEL | AP_BTN_BRAKE);
			cax[2]  = ax;
			cand[3] = w;
			cax[3]  = ap_axis_of(barWant * AP_PROBE_MORE, sign);
			cand[4] = w;
			cax[4]  = ap_axis_of(barWant * AP_PROBE_LESS, sign);

			best = ap_probe_score(cand[0], cax[0], to, r->position,
			                      dtMs, &endV);

			/*
			 * TROUBLE IS SPEED LOST, not a crash flag - a crash
			 * test alone never fires. See AP_PROBE_KEEP.
			 */
			if (best <= -AP_PROBE_CRASH * 0.5f
			 || (endV >= 0.0f && r->speed > 1.0f
			     && endV < r->speed * AP_PROBE_KEEP)) {
				for (ci = 1; ci < 5; ci++) {
					float sc = ap_probe_score(cand[ci], cax[ci],
					                          to, r->position,
					                          dtMs, NULL);

					if (sc > best) {
						best  = sc;
						bestI = ci;
					}
				}
				if (bestI != 0) {
					s_probeWord = cand[bestI];
					s_probeAxis = cax[bestI];
					/*
					 * ONE PROBE INTERVAL, not more. The word is
					 * held only until the next probe can look
					 * again; holding longer fights the steering
					 * hysteresis, which is already a latch.
					 */
					s_probeHoldMs = (float)AP_PROBE_MS;
				} else {
					s_probeHoldMs = 0.0f;
				}
			} else {
				s_probeHoldMs = 0.0f;
			}
		}
		if (s_probeHoldMs > 0.0f) {
			s_probeHoldMs -= dtMs;
			w  = s_probeWord;
			ax = s_probeAxis;
		}
	}

	/*
	 * THE DIAGNOSTIC, [DEBUG] autopilotlog=1 and throttled to one
	 * line in thirty frames. Every number the decision was made
	 * from, so a bad lap can be READ BACK rather than guessed at -
	 * rule 8 means nobody on this side can watch it run.
	 */
	if (g_autopilotLog) {
		static int n;

		if ((n++ % 30) == 0) {
			mrlog("auto:   seg %4d lat %+7.0f off %+7.0f aim %+.3f "
			      "fwd %+.2f "
			      "bar %+.3f ask %+.3f want %+.3f | "
			      "crv %.3f spd %3d/%3d slip %d btn %u\n",
			      here, (double)lateral, (double)offLine,
			      (double)aimDot, (double)fwdDot,
			      (double)bar, (double)barWant, (double)want,
			      (double)curve, speed, target,
			      r->fxGate ? 1 : 0, w);
		}
	}

	if (axis1)
		*axis1 = ax;

	*buttons = w;
	if (stunt)
		*stunt = 0;
	return 1;
}
