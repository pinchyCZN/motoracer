/*
 * physics.c - the drivetrain. See physics.h for what is and is not
 * here, and docs/subsystems/racers.txt 9.10, 9.11, 9.17, 9.18 and
 * 9.19 for the evidence behind every constant.
 *
 * Constants are given with the .rdata address they were read from so
 * a reader can check any of them without re-deriving it. They are
 * doubles in the original where noted, and that is not pedantry: the
 * FADD at 0x00466678 adds a DOUBLE to a float accumulator, and the
 * rounding that produces is visible in the eighth term of a captured
 * sequence. Where the original computes in double, so does this.
 */

#include <math.h>
#include <string.h>

#include "physics.h"
#include "racer.h"
#include "trig.h"

/* ---------------------------------------------------------------- */
/* Constants, all read out of .rdata rather than fitted.            */

#define K_RPM_ANCHOR      1000.0     /* 0x004d1d80 / 0x004d1d88      */
#define K_GRAVITY            9.81    /* 0x004d1d90, kgf.m -> N.m     */
#define K_TORQUE_FLOOR       0.2     /* 0x004d1d98                   */
#define K_DRIVE_BONUS        1.1     /* 0x004d1da0                   */

#define K_DT_SCALE          10.0f    /* 0x004d1da8, back to real ms  */
#define K_DT_MAX           500.0f    /* 0x004d1dac                   */
#define K_GAIN_CAP           0.03f   /* 0x004d1dc8, per ms           */
#define K_MILLI              0.001   /* 0x004d1c80                   */

/*
 * THE LOW-SPEED GUARD. Below the threshold the original MULTIPLIES by
 * 10000 instead of dividing by the road speed - and 10000 is exactly
 * 1/0.0001, so it is "clamp the divisor to 0.0001" written as a
 * multiply to dodge the division.
 */
#define K_DIV_THRESHOLD      0.0001  /* 0x004d1c90                   */
#define K_DIV_LOWSPEED   10000.0     /* 0x004d1dc0, = 1/threshold    */

/* The engine idles here, in rpm. */
#define K_IDLE_RPM        1200.0f    /* 0x004d1db0                   */

#define K_DRAG_GROUND        0.3     /* 0x004d1e2c                   */
#define K_DRAG_AIR           1.6     /* 0x004d1e28                   */

/*
 * The wheelie. Its dt is the TICK's scaled dt, not the drivetrain's
 * milliseconds - see physics.h.
 */
/*
 * SPELLED TO THE FULL 17 DIGITS BECAUSE THEY ARE ACCUMULATED.
 * Both are added to the wheelie angle every frame, so a truncated
 * literal drifts - and these were truncated: 0.0471238898 is 5.5e5
 * ulp from the binary's constant and -0.0251327412 is 8.3e6.
 *
 * o 0x004d10b0 IS -0.016*PI/2 exactly, but do not write it that
 *   way. A compile-time fold in x87 80-bit need not land on the
 *   same double as one done in 53 bits, which is precisely what
 *   0x004d10a8 demonstrates.
 * o 0x004d10a8 IS NOT 0.03*PI/2. Every association order of that
 *   product gives a6ba225b9f20a83f and the binary holds
 *   a7ba225b9f20a83f - one ulp higher. The 1997 compiler folded it
 *   to a value IEEE double arithmetic does not reproduce. It is
 *   exactly -1.875 times the constant below, 0.03/0.016, which is
 *   the only derivation that does.
 *
 * Verified by reading .rdata: a7ba225b9f20a83f and
 * c3718bb665bc99bf.
 */
#define K_WHEELIE_UP     0.047123889803846901   /* 0x004d10a8  */
#define K_WHEELIE_DOWN  -0.025132741228718346   /* 0x004d10b0  */
#define K_WHEELIE_MAX    MR_HALF_PI_F  /* 0x3fc90fdb, a FLOAT pi/2  */
#define K_WHEELIE_CAP          200.0    /* 0x43480000                */
#define K_WHEELIE_THUMP        -32.0    /* 0xc2000000                */
#define K_WHEELIE_SCALE          2.0    /* 0x40000000                */

#define K_AIR_STEP             2.592    /* 0x004d0f20, per FRAME     */

#define K_SLOPE          0.004905    /* 0x004d1d60, and it is        */
                                     /* 9.81/2000 - gravity again    */
/*
 * The airborne gate the TAIL's drag uses - racer+0xf0 compared
 * against this, not the != 0 test the drive path takes.
 *
 * A SECOND .rdata CONSTANT, NOT K_AIR_STEP. Both hold 2.592 and
 * they are different addresses serving different jobs: 0x004d0f20
 * is the per-frame increment, 0x004d1c18 the threshold. One frame
 * of air time is exactly the gate, so the values agree by design -
 * but folding them would tie a threshold to a step size. racer.c
 * calls this one INT_AIRTIME_GATE.
 */
#define RACE_AIRTIME_GATE  2.592     /* 0x004d1c18, a DOUBLE        */

#define K_SLOPE_MAX          0.55    /* 0x004d1d68 / 0x3f0ccccd      */
/*
 * BELOW THIS, A BRAKED BIKE GETS NO SLOPE TERM AT ALL.
 * 0x004d1ca0, a DOUBLE - 0.1 - tested at 0x0048fd9b against the
 * speed while the original's local_8 (num < 0 || preRace) is set.
 * It is what holds a stopped bike on a hill under the brake.
 */
#define K_SLOPE_BRAKE_CUT    0.1     /* 0x004d1ca0, a DOUBLE - and
                                      * GB_SWEEP_STEP is the same
                                      * constant under another
                                      * meaning  */

#define K_BRAKE_MIN         10.0f    /* 0x41200000, an immediate     */
#define K_BRAKE_MAX        140.0f    /* 0x004d1df0 / 0x430c0000      */
#define K_BRAKE_RAMP_MS    200.0     /* 0x004d1e00                   */
/*
 * A FLOAT IN .rdata, NOT A DOUBLE - 0x3e2aaaab, which is
 * 0.1666666716337204 and not the 0.16666666666666666 that
 * (1.0 / 6.0) gives. The original is FLD float / FMUL float /
 * FSTP float at 0x0048f9cd.
 *
 * The f suffix is what picks the right VALUE; it does not keep the
 * arithmetic in single precision, because force is a double and the
 * compiler folds this to a promoted double (000000605555c53f,
 * checked in the emitted object). That is fine here - one multiply
 * and one rounding to float on return, as the original does.
 */
#define K_BRAKE_RAMP        (1.0f / 6.0f)  /* 0x004d1e08, a FLOAT */
#define K_KMH_TO_MS         (1.0 / 3.6)  /* 0x004d1e10, 0.27777778   */

#define K_SLIP_MIN           0.01    /* 0x004d1d48                   */
#define K_SLIP_MAX          20.0     /* 0x004d1d50                   */
#define K_SLIP_RATE         -0.05    /* 0x004d1d58                   */
#define K_GRIP_FULL          1.0     /* 0x004d1c00                   */

/*
 * PI/30 turns rpm into radians per second, which is what makes
 * power = torque * angular velocity come out in watts. The original
 * has it as a folded double constant; it is spelled out here because
 * "0.10471975511965977" in a source file is unreadable and the
 * identity is the whole point.
 */
#define K_RPM_TO_RADS  (MR_PI / 30.0)

/*
 * Cornering. K_CAMBER_G is 9.81 like K_GRAVITY but is a DIFFERENT
 * .rdata constant serving a different purpose, so it is named
 * separately rather than shared.
 */
#define K_CAMBER_EPS         1.0     /* 0x004d0ec8, under the sqrt  */
#define K_CAMBER_G           9.81    /* 0x004d0f30, gravity again   */
#define K_CORNER_DT          0.01    /* 0x004d0ed8, and it is the
                                      * same double as the fork
                                      * dive decay                  */
#define K_RADIUS_SCALE       0.0078125  /* 0x004d0ee0, = 1/128      */
#define K_DISENGAGED_MUL     2.0     /* 0x004d0e88                  */

/* 0x004d1c08, a DOUBLE - see physics.h. */
#define K_VELDIR_EPS       1e-5

#define MOTOTYPE_DIRT  1


/* ---------------------------------------------------------------- */

/* ORIGIN  Engine_TorqueAt @ 0x0048ee30 */
float engine_torque_at(const MR_BikeCarac *c, float engineSpeed,
                       float wheelieScale, int driveInterlock)
{
	double rpm;
	double m;
	double b;
	double t;

	/*
	 * engineSpeed -> rpm. regimeMax and moteurVitesseMax are the
	 * two ends of the same scale, and this line is what says so.
	 */
	if (c->moteurVitesseMax == 0.0f)
		return (float)K_TORQUE_FLOOR;
	rpm = ((double)c->regimeMax / (double)c->moteurVitesseMax)
	      * (double)engineSpeed;

	/*
	 * Two segments through three points:
	 *
	 *     (1000,            couple1000)
	 *     (regimeCoupleMax, coupleMax)        <- peak, by
	 *     (regimeMax,       coupleRegimeMax)     construction
	 *
	 * so peak torque is coupleMax at regimeCoupleMax exactly, and
	 * the .bpt field names describe the geometry rather than
	 * approximating it.
	 */
	if (rpm >= (double)c->regimeCoupleMax) {
		double den = (double)c->regimeMax - (double)c->regimeCoupleMax;

		m = (den == 0.0) ? 0.0
		    : ((double)c->coupleRegimeMax - (double)c->coupleMax) / den;
		b = (double)c->coupleMax - (double)c->regimeCoupleMax * m;
	} else {
		double den = (double)c->regimeCoupleMax - K_RPM_ANCHOR;

		m = (den == 0.0) ? 0.0
		    : ((double)c->coupleMax - (double)c->couple1000) / den;
		b = (double)c->couple1000 + m * -K_RPM_ANCHOR;
	}

	/* kgf.m -> N.m. This is a unit conversion, not a fudge. */
	t = (m * rpm + b) * K_GRAVITY;

	/* The engine never makes exactly zero, so a bike creeps. */
	if (t < K_TORQUE_FLOOR)
		t = K_TORQUE_FLOOR;

	/*
	 * THE 1.1x, gated on racer+0x88 alone. The original also
	 * required g_codeCheckPassed; that half is dropped because a
	 * rebuilt executable is legitimate by construction. See
	 * physics.h.
	 *
	 * DO NOT FOLD IT INTO THE CURVE. A flat 10 per cent buried in
	 * the torque line is invisible to a trace diff, which is the
	 * one check this function has; left as a parameter, getting it
	 * wrong shows up as every predicted gain being 1.1000x out -
	 * a ratio clean enough to name its own cause.
	 */
	if (!driveInterlock)
		t *= K_DRIVE_BONUS;

	return (float)(t * (double)wheelieScale);
}


/* ORIGIN  maybe_Physics_TickDrivetrain @ 0x0048ef30, the grip block -
 *         SIX identical copies, e.g. 0x0048f7f7..0x0048f869 */
float physics_grip(const MR_BikeCarac *c, float slipAbs)
{
	/*
	 * 1.0 IS STORED FIRST AND ONLY THEN OVERWRITTEN - see
	 * 0x0048f7ef, MOV dword [ESP+0x1c],0x3f800000, immediately
	 * before the block. That ordering is the whole behaviour of
	 * the upper bound: outside the band grip is a clean 1.0
	 * rather than a clamped 0.
	 */
	double grip = K_GRIP_FULL;

	if (c->motoType == MOTOTYPE_DIRT) {
		if (slipAbs > K_SLIP_MIN && slipAbs < K_SLIP_MAX)
			grip = K_GRIP_FULL + K_SLIP_RATE * (double)slipAbs;
		/*
		 * else: left at 1.0. Below 0.01 that is "going straight";
		 * above 20.0 it is the cliff described in physics.h.
		 */
	} else {
		/*
		 * max(1.0, minAdherenceWheel). The original compares the
		 * float BITS against 0x3f800000 as integers, which is
		 * valid for positive floats and is just how the compiler
		 * emitted it - not a trick worth reproducing.
		 */
		if ((double)c->minAdherenceWheel > K_GRIP_FULL)
			grip = (double)c->minAdherenceWheel;
	}

	return (float)grip;
}


/* ORIGIN  maybe_Physics_TickDrivetrain @ 0x0048ef30, at 0x0049003e */
float physics_drag(const MR_BikeCarac *c, float speed, float dtMs,
                   int airborne)
{
	double k = airborne ? K_DRAG_AIR : K_DRAG_GROUND;

	if (c->motoMasse == 0.0f)
		return 0.0f;

	return (float)((double)speed * (double)speed * (double)dtMs * k
	               / (double)c->motoMasse * K_MILLI);
}


/*
 * ORIGIN  NONE - ours. The shared body of maybe_Racer_SlipSpeed
 *         @ 0x0048d0f0 and maybe_Racer_LongSpeed @ 0x0048ca10,
 *         which are two near-identical functions differing only in
 *         which matrix row the final dot product uses - row 0 for
 *         lateral, row 2 for longitudinal.
 */
static float body_axis_speed(const float *m, const float *velDir,
                             float speed, int row)
{
	double x = (double)velDir[0];
	double y = (double)velDir[1];
	double z = (double)velDir[2];
	double len;
	double vx, vy, vz;
	double lateral;

	/*
	 * 0048d132  FSQRT / FCOM [0x004d1bec] / TEST AH,0x40
	 * 0x004d1bec is 0.0f, and the test is C3 - EQUAL - so the
	 * normalise is skipped only on an exactly zero vector.
	 *
	 * AN EQUALITY TEST, NOT AN EPSILON GUARD, and the file has
	 * both: the two velDir normalises further down compare against
	 * K_VELDIR_EPS because the original does there. Do not make
	 * them agree.
	 */
	len = sqrt(x * x + y * y + z * z);
	if (len != 0.0) {
		double inv = 1.0 / len;

		x *= inv;
		y *= inv;
		z *= inv;
	}

	vx = x * (double)speed;
	vy = y * (double)speed;
	vz = z * (double)speed;

	/*
	 * row0 . v  -  row0 . t. The second term is the invM[9] the
	 * original builds at 0x0048d1c9..0x0048d23b, and it is zero
	 * whenever matOrientation carries no translation.
	 *
	 * [open] NO RACER MATRIX HAS BEEN OBSERVED CARRYING ONE, so
	 * the term has never been seen to contribute anything.
	 * Transcribed regardless, because the original computes it -
	 * dropping it would be a guess that it is always zero.
	 */
	lateral = (double)m[row * 3 + 0] * vx
	          + (double)m[row * 3 + 1] * vy
	          + (double)m[row * 3 + 2] * vz
	          - ((double)m[row * 3 + 0] * (double)m[9]
	             + (double)m[row * 3 + 1] * (double)m[10]
	             + (double)m[row * 3 + 2] * (double)m[11]);

	return (float)lateral;
}


/* ORIGIN  maybe_Racer_SlipSpeed @ 0x0048d0f0 */
float physics_slip_speed(const float *m, const float *velDir,
                         float speed)
{
	return body_axis_speed(m, velDir, speed, 0);   /* row 0, lateral */
}


/* ORIGIN  maybe_Racer_LongSpeed @ 0x0048ca10 */
float physics_long_speed(const float *m, const float *velDir,
                         float speed)
{
	return body_axis_speed(m, velDir, speed, 2);   /* row 2, forward */
}


/* ORIGIN  maybe_Racer_UpdateWheelie @ 0x004660f0 */
void racer_update_wheelie(MR_Racer *r, float dtScaled)
{
	int requested;

	/* 004660f3  the crash reset comes FIRST and skips everything. */
	if (r->crashLatch != 0) {
		r->wheelieAngle = 0.0f;
		r->wheelieTimer = 0.0f;
		r->stuntOrOverspeed = 0;
		r->wheelieScale = 1.0f;
		/*
		 * 0046611a and 00466125  the button latch is released only
		 * for a human in solo. A remote or an AI racer never had it
		 * set, and clearing it for them would fight the netcode.
		 */
		if (r->humanPlayer != 0 && r->soloMode != 0)
			r->stuntHeld = 0;
		return;
	}

	/*
	 * 00466137  the request is read ONCE and the value is reused at
	 * 00466200 below, after the ramp has run. Re-reading the field
	 * there would be wrong on the frame ApplyDrive clears it.
	 */
	requested = r->stuntOrOverspeed;

	/*
	 * 00466147  CMP EAX,0x43480000 / JGE - the timer is compared as
	 * float BITS against 200.0f. THE CAP GATES THE RAMP, NOT THE
	 * ANGLE: once the timer reaches 200 the decay branch runs with
	 * the button still held.
	 */
	if (requested && (double)r->wheelieTimer < K_WHEELIE_CAP) {
		r->wheelieAngle = (float)((double)r->wheelieAngle
		                          + (double)dtScaled * K_WHEELIE_UP);
		if ((double)r->wheelieAngle > K_WHEELIE_MAX)
			r->wheelieAngle = (float)K_WHEELIE_MAX;
		r->wheelieTimer += dtScaled;
	} else if (r->wheelieAngle > 0.0f) {
		r->wheelieAngle = (float)((double)r->wheelieAngle
		                          + (double)dtScaled * K_WHEELIE_DOWN);
		if (r->wheelieAngle < 0.0f) {
			/*
			 * THE LANDING THUMP, one shot, at 0x004661b8. It is an
			 * impulse into the SUSPENSION - springCompression,
			 * +0x2e95 - which is why a wheelie ends with the forks
			 * visibly loading up. maybe_Racer_Tick zeroes that field
			 * at the top of every frame, so it is a one-frame kick.
			 *
			 * The original tests the float sign bit with
			 * CMP EAX,0x80000000 / JBE.
			 */
			r->springCompression = (float)K_WHEELIE_THUMP;
			r->wheelieAngle = 0.0f;
		}
	}

	/* 004661ca  TEST EAX,0x7fffffff - sign-masked zero test. */
	if (r->wheelieAngle != 0.0f) {
		r->wheelieScale = (float)K_WHEELIE_SCALE;
	} else {
		r->wheelieScale = 1.0f;
		if (!requested)
			r->wheelieTimer = 0.0f;
	}
}

/* ORIGIN  maybe_Racer_UpdateAirborne @ 0x00466660 */
float physics_air_time(float airTime, int onGround)
{
	if (onGround)
		return 0.0f;
	return (float)((double)airTime + K_AIR_STEP);
}


/* ORIGIN  maybe_Physics_TickDrivetrain @ 0x0048ef30, at 0x0048fdac */
float physics_slope(float dirY, float dtMs, int airborne)
{
	double t;

	/*
	 * 0048fdb4  MOV EAX,[ESI+0xcc] / TEST EAX,0x7fffffff / JNZ
	 * Airborne leaves the term at the zero stored at 0x0048fdac.
	 */
	if (airborne)
		return 0.0f;

	t = -(double)dirY * (double)dtMs * K_SLOPE;

	/* Upper bound only - uphill is unbounded. See physics.h. */
	if (t > K_SLOPE_MAX)
		t = K_SLOPE_MAX;

	return (float)t;
}


/* ORIGIN  maybe_Physics_TickDrivetrain @ 0x0048ef30, at 0x0048f904 */
int physics_brake_gated(int braking, int turnBrakeFlag, int rampOverride,
                        int preRace)
{
	/*
	 * 0048f904  TEST EBP,EBP / JL      braking?
	 * 0048f908  else: preRace ? fall through : skip the whole block
	 * 0048f915  turnBrakeFlag  != 0 -> run
	 * 0048f91f  rampOverride   != 0 -> run
	 * 0048f927  preRace        != 0 -> run
	 *           otherwise the counter is zeroed and the block skipped
	 */
	if (!braking && !preRace)
		return 0;
	return (turnBrakeFlag != 0) || (rampOverride != 0) || (preRace != 0);
}


/* ORIGIN  maybe_Physics_TickDrivetrain @ 0x0048ef30, at 0x0048f962 */
float physics_brake_force(const MR_BikeCarac *c, int brakeHoldMs,
                          int rampOverride)
{
	double force = (double)c->frein;

	if (force < (double)K_BRAKE_MIN)
		force = (double)K_BRAKE_MIN;
	else if (force > (double)K_BRAKE_MAX)
		force = (double)K_BRAKE_MAX;

	/*
	 * THE RAMP. Six times weaker for the first 200 ms of a brake.
	 * Measured: 13.3333 for six frames, then 80.0 on the frame
	 * after the counter passed 200. racers.txt 9.21.
	 */
	if ((double)brakeHoldMs < K_BRAKE_RAMP_MS && !rampOverride)
		force *= K_BRAKE_RAMP;

	return (float)force;
}


/* ORIGIN  maybe_Physics_TickDrivetrain @ 0x0048ef30, at 0x0048f9df */
float physics_brake_step(const MR_BikeCarac *c, int brakeHoldMs,
                         float dtMs, int rampOverride)
{
	double force = (double)physics_brake_force(c, brakeHoldMs,
	                                           rampOverride);

	/* km/h -> m/s, then per-millisecond. */
	return (float)(force * K_KMH_TO_MS * (double)dtMs * K_MILLI);
}


/* ORIGIN  maybe_Physics_TickDrivetrain @ 0x0048ef30, at 0x0048f9f6 */
void physics_apply_brake(MR_Drivetrain *d, float step, float dtMs)
{
	if (d->gearboxRoadSpeed > 0.0f) {
		d->gearboxRoadSpeed -= step;
		if (d->gearboxRoadSpeed < 0.0f)
			d->gearboxRoadSpeed = 0.0f;   /* overshoot */
	} else if (d->gearboxRoadSpeed < 0.0f) {
		d->gearboxRoadSpeed += step;
		if (d->gearboxRoadSpeed > 0.0f)
			d->gearboxRoadSpeed = 0.0f;   /* overshoot */
	}

	/*
	 * The counter advances by the WHOLE MILLISECONDS of the frame -
	 * the captured ladder was 0, 36, 69, 102, 136, 169, 202, 235,
	 * 268 against per-frame dt of 36, 33, 33, 34, 33, 33, 33, 33.
	 * It is an int in the original and FILD reads it as one.
	 */
	d->brakeHoldMs += (int)dtMs;
}


/* ORIGIN  maybe_Physics_TickDrivetrain @ 0x0048ef30, the accelerate
 *         path */
static void physics_tick_drivetrain(MR_Drivetrain *d,
                             const MR_BikeCarac *c,
                             float realSpeedMax,
                             float dtScaled, float slipAbs,
                             float wheelieScale,
                             int driveInterlock, int gear,
                             float target, int disengaged,
                             const float *mat, float *velDir,
                             int airborne)
{
	float  dtMs;
	double rpm;
	double power;
	double gain;
	double cap;
	double idle;
	float  grip;
	float  drag;

	if (c->motoMasse == 0.0f || c->moteurVitesseMax == 0.0f)
		return;

	/*
	 * 1. THE dt RESCALE. Back to raw milliseconds, capped. Every
	 *    term below is per-millisecond because of this line.
	 */
	dtMs = dtScaled * K_DT_SCALE;
	if (dtMs > K_DT_MAX)
		dtMs = K_DT_MAX;

	/*
	 * 2. THE IDLE FLOOR, and WITHOUT IT A STOPPED BIKE NEVER MOVES.
	 *
	 *        idle = moteurVitesseMax / regimeMax * 1200
	 *        if (engineSpeed < idle) engineSpeed = idle
	 *
	 *    The engine idles at 1200 rpm, expressed in engineSpeed
	 *    units. At rest the gearbox reports zero engine speed, so
	 *    zero rpm, so zero torque - this is what breaks that
	 *    circle. racers-convergence.txt 9.17.7.4 measured engineSpeed pinned at
	 *    exactly 0.7000 for the first two frames off the line, and
	 *    0.7 is what this expression gives for the road bike.
	 *
	 *    The original applies it only when accelerating and on the
	 *    ground; this function is the accelerate path, so the
	 *    condition is already met.
	 */
	idle = (double)c->moteurVitesseMax / (double)c->regimeMax
	       * (double)K_IDLE_RPM;
	if ((double)d->engineSpeed < idle)
		d->engineSpeed = (float)idle;

	/*
	 * 3. THE ENGINE. power = angular velocity * torque, and the
	 *    drive force at the wheel is power / road speed, so the
	 *    gain is force * dt / mass.
	 *
	 *    THE LOW-SPEED GUARD IS THE ORIGINAL'S, now read: at or
	 *    below 0.0001 it MULTIPLIES BY 10000 rather than dividing.
	 *    Since 10000 is 1/0.0001 that is just clamping the divisor,
	 *    but written as a multiply - and it is what lets the bike
	 *    pull away from a standstill instead of dividing by zero.
	 */
	rpm = ((double)c->regimeMax / (double)c->moteurVitesseMax)
	      * (double)d->engineSpeed;
	power = rpm * K_RPM_TO_RADS
	        * (double)engine_torque_at(c, d->engineSpeed,
	                                   wheelieScale, driveInterlock);

	if ((double)d->gearboxRoadSpeed > K_DIV_THRESHOLD)
		gain = power / (double)d->gearboxRoadSpeed;
	else
		gain = power * K_DIV_LOWSPEED;

	gain = (double)dtMs * gain / (double)c->motoMasse * K_MILLI;

	/*
	 * THE ACCELERATION CAP, and it is the LAUNCH LIMITER. At a
	 * standstill the low-speed branch makes the raw gain enormous -
	 * power times ten thousand - so this is what actually decides
	 * how hard a bike leaves the line. It binds only at a
	 * standstill, which is why no capture ever saw it bind; see
	 * racers-convergence.txt 9.32.3.
	 */
	cap = (double)dtMs * K_GAIN_CAP;
	if (gain > cap)
		gain = cap;

	d->gearboxRoadSpeed += (float)gain;

	d->engineSpeed = physics_speed_to_engine(c, gear,
	                                         &d->gearboxRoadSpeed,
	                                         d->engineSpeed,
	                                         disengaged);
	if (d->engineSpeed > c->moteurVitesseMax)
		d->engineSpeed = c->moteurVitesseMax;
	if (d->engineSpeed > target)
		d->engineSpeed = target;
	d->gearboxRoadSpeed = physics_engine_to_speed(c, realSpeedMax,
	                                              gear,
	                                              &d->engineSpeed,
	                                              d->gearboxRoadSpeed);

	/* 3. Clamp to the bike's real top speed. */
	if (d->gearboxRoadSpeed > realSpeedMax)
		d->gearboxRoadSpeed = realSpeedMax;

	/*
	 * 4. THE GEARBOX RESYNC, 0x0048f4a6..0x0048f507.
	 *
	 *        0048f4a6  engine = SpeedToEngine(roadSpeed)
	 *        0048f4ba  clamp to carac field 6, MoteurVitesseMax
	 *        0048f4d9  clamp to the THROTTLE TARGET, [ESP+0x10]
	 *        0048f502  roadSpeed = EngineToSpeed(engine)
	 *
	 *    DO NOT DROP THIS BECAUSE THE ROAD SPEED ROUND-TRIPS. The
	 *    trip IS lossless for gearboxRoadSpeed - byte-identical on
	 *    8 of 8 captured frames - and is NOT lossless for the
	 *    engine, and the engine it leaves behind is the ROUNDED
	 *    one that the next frame builds on. Measuring the road
	 *    speed alone says the step does nothing; it is the wrong
	 *    thing to measure.
	 *
	 *    MEASURED on the 1 -> 2 upshift, drive frame, entry engine
	 *    6.96622 on both sides. A hook at 0x0048f4ee reads MOTO's
	 *    engine as 7.25550 with the road speed still 12.7099 and
	 *    the target cap 8.0; the plain formula on 12.7099 gives
	 *    7.26281, which is what the port had. The round trip is
	 *    the whole of the 0.0073.
	 *
	 *    IT ONLY SHOWS AT A SHIFT because on an ordinary frame the
	 *    tail recomputes the engine from the road speed anyway and
	 *    erases the difference. On the frame a shift is requested
	 *    the NEXT frame takes the free-rev arm, which ADDS to the
	 *    engine instead of rebuilding it, so the residue survives.
	 *    racers-convergence.txt 9.46.18.
	 *
	 * 5. maybe_Racer_SetLongSpeed then syncs speed to road speed.
	 *    At this point in a captured frame the two were equal to
	 *    the bit.
	 *
	 *
	 *    NO AIRBORNE GATE HERE, AND THAT IS DELIBERATE: THE GATE
	 *    IS THE CALLER'S. physics_drivetrain_frame reaches this
	 *    function only from the drive branch, the `else` of
	 *        if (target < engine || num < 1) ...
	 *        else if (skipSync || airborne) ...   free rev
	 *        else                                 THIS
	 *    so an airborne bike free-revs at 0x0048f6f0 and never
	 *    arrives. A gate inside here would be unreachable, and an
	 *    unreachable gate reads as though the original had one at
	 *    this level. It does not.
	 *
	 *    STEPS 5 AND 6 ARE ONE CALL IN THE ORIGINAL, and one
	 *    call here - see below.
	 */

	/*
	 * 6. THE TYRE-SLIP LERP. On tarmac in a straight line this
	 *    converges completely and is not an independent loss; on a
	 *    dirt bike sliding it is what lets wheel speed and real
	 *    speed diverge.
	 *
	 *    The lerp target is the BODY-SPACE LONGITUDINAL velocity.
	 *    Here that is d->speed, which is what step 5 just made it -
	 *    correct for the accelerate path and NOT correct in
	 *    general, because a sliding bike's forward velocity is not
	 *    its speed. When the caller can supply the body-space
	 *    component this should take it as an argument. [open]
	 *
	 *    IT IS Racer_SetLongSpeed, NOT AN ASSIGNMENT TO speed
	 *    FOLLOWED BY A LERP ON gbrs.
	 *
	 *    ORIGIN  the drive branch's own ending, e.g.
	 *            0x0048f5xx / 0x0048f8xx:
	 *
	 *        Racer_SetLongSpeed((gbrs - bodyZ) * k + bodyZ)
	 *
	 *    THE CALL IS WHAT REVIVES velDir FROM A DEAD STOP.
	 *    SetLongSpeed zeroes velDir below 1e-5 (physics.h), so a
	 *    stopped bike has no direction at all and every body-space
	 *    read returns 0. Anything here that writes speed and gbrs
	 *    WITHOUT touching velDir leaves the tail taking its scalar
	 *    from a measured body z of zero, the throttle with nothing
	 *    to push against, and the bike unable to move off the
	 *    line. The two halves only work together.
	 *
	 *    UNGATED FOR THE SAME REASON AS STEP 5 - the caller's
	 *    drive branch is the gate - and GROUNDED ONLY, as the
	 *    original gates it.
	 */
	grip = physics_grip(c, slipAbs);
	if (!airborne && mat && velDir) {
		float bodyZ = physics_long_speed(mat, velDir, d->speed);

		physics_set_long_speed(mat, velDir, &d->speed,
		                       realSpeedMax,
		                       (d->gearboxRoadSpeed - bodyZ)
		                       * grip + bodyZ);
	} else {
		/*
		 * NO MATRIX TO WORK IN - the airborne arm, and the
		 * unit tests that call this with nothing.  Keep the
		 * old stand-in so neither changes behaviour.
		 */
		d->speed = d->gearboxRoadSpeed;
	}

	/*
	 *    A SECOND SetLongSpeed CALL SITS HERE, worth about -0.0065
	 *    a frame on both speeds at 24 m/s - roughly a quarter of
	 *    the drag. The call is located; its argument is not
	 *    quantified, so it is left out rather than guessed at.
	 *    racers-convergence.txt 9.17.9.5 step 5. [open]
	 *
	 * 7. DRAG, on speed rather than on road speed.
	 */
	/*
	 * NOT SUBTRACTED HERE - race.c OWNS THIS SITE.
	 *
	 * race.c transcribes the same original function's tail from
	 * 0x0048fec5 (SetLongSpeed), 0x00490090 (this drag) and
	 * 0x00490140 (the lerp), in that order. Subtracting here as
	 * well applies the ONE drag the original has TWICE a frame -
	 * and because this copy runs BEFORE SetLongSpeed rebuilds
	 * speed, the already-dragged value also feeds the longitudinal
	 * scalar, so the second application compounds the first.
	 *
	 * MEASURED when it did: a v-squared deficit, ours slower,
	 * growing from -0.007 per cent at 1.9 m/s to -0.260 per cent
	 * at 11.5 over sixteen frames of a standing launch.
	 *
	 * The call is kept so the site stays visible and the drag is
	 * still computed for anyone reading the step order; only the
	 * subtraction moves. Steps 6 and 8 below are likewise
	 * overwritten by race.c's `gearboxRoadSpeed = v`.
	 */
	drag = physics_drag(c, d->speed, dtMs, d->airTime != 0.0f);
	(void)drag;

	/*
	 * 8. THE FINAL LERP pulls road speed back down onto speed. Its
	 *    factor comes from FUN_0048eb40, WHICH IS physics_grip - the
	 *    two were measured EQUAL at the function tail on every
	 *    captured frame, so full convergence is what the original
	 *    does, whatever the factor is. Transcribed as the observed
	 *    behaviour with the gap named rather than as a guessed
	 *    coefficient. racers-convergence.txt 9.17.9.2. [open]
	 */
	d->gearboxRoadSpeed = d->speed;
}


/* ORIGIN  maybe_Racer_ApplyCornering @ 0x0045d110, the slope term */
float physics_camber(float groundDeltaY)
{
	double dy = (double)groundDeltaY;

	/*
	 * A SMOOTH BOUNDED SIGN FUNCTION. dy/sqrt(dy^2+1) saturates at
	 * +-1, so a cliff edge produces the same lateral pull as a
	 * steep bank rather than an enormous one.
	 */
	return (float)(dy / sqrt(dy * dy + K_CAMBER_EPS) * K_CAMBER_G);
}


/* ORIGIN  maybe_Racer_ApplyCornering @ 0x0045d110, the v*v/r term */
float physics_centripetal(float speed, float radius, int motoType,
                          int driveDisengaged)
{
	double a;

	if (motoType != MOTOTYPE_DIRT)
		return 0.0f;
	if (radius == 0.0f)
		return 0.0f;

	a = ((double)speed * (double)speed) / (double)radius;

	/* Coasting with no brake corners TWICE as hard. */
	if (driveDisengaged)
		a *= K_DISENGAGED_MUL;

	return (float)a;
}


/* ORIGIN  maybe_Racer_ApplyCornering @ 0x0045d110, its tail from
 *         0x0045d4c0 */
float physics_cornering_step(float camber, float centripetal,
                             float slipSpeed, float friction,
                             float dtScaled)
{
	double out;
	double drop;

	out  = ((double)camber + (double)centripetal)
	       * (double)dtScaled * K_CORNER_DT + (double)slipSpeed;
	drop = (double)friction * K_CAMBER_G * (double)dtScaled
	       * K_CORNER_DT;

	/*
	 * Toward zero from either side, snapping at the crossing. The
	 * third instance of this idiom in the subsystem - see
	 * physics_apply_brake and racer_decay_fork_dive.
	 */
	if (out > 0.0) {
		out -= drop;
		if (out < 0.0)
			out = 0.0;
	} else if (out < 0.0) {
		out += drop;
		if (out > 0.0)
			out = 0.0;
	}

	return (float)out;
}


/* ORIGIN  maybe_Gearbox_EngineToSpeed @ 0x0048dd90, the ratio
 *         lookup - and maybe_Gearbox_SpeedToEngine for the inverse */
float physics_gear_top_speed(const MR_BikeCarac *c, int gear)
{
	if (!c || gear < 1 || gear > 6)
		return 0.0f;
	return c->rapportDeBoite[gear - 1] * c->vitesseMaxBoite;
}


/* ORIGIN  NONE - ours. Derived from the ratios alone; the real shift
 *         logic is gearbox_tick_shift, which IS
 *         maybe_Gearbox_TickShift @ 0x0048e6e0. */
int physics_gear_for_speed(const MR_BikeCarac *c, float roadSpeed)
{
	int top;
	int g;

	if (!c)
		return 1;

	top = (int)c->nbSpeed;
	if (top < 1)
		top = 1;
	else if (top > 6)
		top = 6;

	for (g = 1; g < top; g++) {
		if ((double)roadSpeed <= (double)physics_gear_top_speed(c, g))
			return g;
	}
	return top;
}


/*
 * The engine constants this frame needs, all read live and recorded on
 * the plate at 0x0048ef30.
 *
 * THE DECAYS ARE DOUBLES. As floats they are garbage - 0.0006 and
 * -0.001 are per-millisecond rates multiplied by a dt, so single
 * precision loses them against an engine speed of order 1.
 */
#define K_DT_TO_MS           10.0    /* 0x004d1da8, float            */
#define K_DT_CEILING         500.0   /* 0x004d1dac, float            */
#define K_ENGINE_RISE        0.0006  /* 0x004d1dd0, coasting/air     */
#define K_ENGINE_DECAY      -0.001   /* 0x004d1dd8, throttle < engine*/
#define K_COAST_DECAY       -0.0006  /* 0x004d1de0                   */
#define K_DECAY_FLOOR_RPM    1000.0  /* 0x004d1d80, decay stops here */


/*
 * ORIGIN  maybe_Physics_TickDrivetrain @ 0x0048ef30, at 0x0048f6f0
 *         - THE RATIO THE ENGINE DECAY IS SCALED BY, per gear.
 *
 * THE TABLE IS AT 0x004f17e0 AND GHIDRA HID IT BEHIND A STRING. The
 * original indexes 0x004f17d4 + gear*4 + 8, and 0x004f17d4 is the
 * twelve bytes "AnimDecor" plus three NULs - so the floats begin where the
 * string ends and Ghidra attributed the lot to s_AnimDecor_. The +8
 * in the address arithmetic is what steps over the tail of it, which
 * is why index 0 reads the string's last four zero bytes rather than
 * a real coefficient.
 *
 * GETTING THIS WRONG SHOWS UP IN THE COAST DECAY AND NOWHERE ELSE.
 * At a flat 1.0 the port shed 15.4 per cent too much speed coasting
 * and the engine decayed 4.5 times too fast, 0.966 a frame against
 * MOTO's 0.992. racers-convergence.txt 9.18.25.
 *
 * Read out of the binary [verified], 0x004f17e0 upward.
 */
static float physics_engine_decay_ratio(int gear)
{
	/*
	 * Index 0 is the string's tail, and it is a real zero as far as
	 * the game is concerned - gear 0 is neutral and decays nothing.
	 */
	static const float k[7] = {
		0.00f,      /* 0x004f17dc - the string tail, reads as 0.0  */
		0.25f,      /* 0x004f17e0                                 */
		0.20f,      /* 0x004f17e4                                 */
		0.16f,      /* 0x004f17e8                                 */
		0.12f,      /* 0x004f17ec                                 */
		0.08f,      /* 0x004f17f0                                 */
		0.05f       /* 0x004f17f4                                 */
	};

	if (gear < 0 || gear > 6)
		return 0.0f;
	return k[gear];
}


/* The second half of physics_drivetrain_frame; see the call at its
 * foot. */
static void physics_drivetrain_tail(MR_Drivetrain *d,
                             const MR_BikeCarac *c,
                             float realSpeedMax,
                             const float *mat, float *velDir,
                             float dirY, float flAirTime, float dtMs,
                             int braking, int gear, int skipSync,
                             int disengaged);


/*
 * ORIGIN  maybe_Physics_TickDrivetrain @ 0x0048ef30
 *
 * ONE FRAME OF THE LONGITUDINAL MODEL - engine, gearbox, drag - and
 * the WHOLE function rather than a branch of it. 1205 instructions,
 * cyclomatic complexity 98.
 *
 * *** DO NOT SPLIT THIS INTO A BRANCH PER THROTTLE STATE ***
 *
 * maybe_Racer_ApplyDrive calls the original on ALL FOUR paths -
 * accelerate, brake, coast, analog - and COAST IS (num=0, den=1),
 * NOT A SKIP. The engine work lives in here, so a reconstruction
 * that grows its own four branches and lets the coast one do drag
 * only leaves the engine at 0.0 off-throttle where MOTO idles at
 * 0.7 - and torque follows engine speed, so the whole of
 * acceleration then runs about half. Measured in
 * docs/racer-draw-audit.txt 9; the general form is racers.txt
 * 9.32.2.
 *
 * THE SHAPE, from the decompilation:
 *
 *     skipSync = preRace || disengaged
 *     airborne = airTime != 0            a float-BITS test, sign masked
 *     braking  = num < 0 || preRace
 *     dtMs     = min(dtScaled * 10, 500)
 *
 *     if (num >= 0)
 *         target = (num/den) * MoteurVitesseMax     a commanded ENGINE
 *         if (target < engine || num < 1)   coast / lifting off
 *         else if (skipSync || airborne)    free rev
 *         else                              THE DRIVE BRANCH
 *     if (braking)                          the brake branch
 *     drag                                  air or ground
 *     tail: clamp gearboxRoadSpeed, then SpeedToEngine
 *
 * THE THROTTLE IS A COMMANDED ENGINE SPEED, not a torque scale -
 * (num/den) * MoteurVitesseMax. At full digital throttle that is the
 * redline, so it only bites on the analog path.
 *
 * THE TAIL SYNC IS UNCONDITIONAL (bar skipSync) and is what keeps the
 * engine alive with no throttle at all. It is the single most
 * important line here.
 *
 * HOLES, each marked at its address rather than dropped:
 *   o HOLE 0x0048fa60 - the brake branch's slip resync and
 *     brake-hold tail
 *   o HOLE 0x0048fc00..0x0048ff00 - the ground drag block's
 *     orientation transform, slope and lateral work. Only the v^2
 *     term is here.
 * See docs/whole-function-import.txt 7.11.
 *
 * The six repeated "resync long speed with a grip factor" blocks are
 * not holes - they are physics_grip plus physics_set_long_speed,
 * factored once. The entry clamps, the 0x0048f7a0 resync, the decay
 * ratio and the whole tail have since been imported; see the
 * addresses at each site.
 */
void physics_drivetrain_frame(MR_Drivetrain *d, const MR_BikeCarac *c,
                              float realSpeedMax,
                              int gear, int num, int den, float dtScaled,
                              float wheelieScale, int driveInterlock,
                              int preRace, int disengaged,
                              const float *mat, float *velDir,
                              float dirY, float flAirTime,
                              int turnBrakeFlag)
{
	double dtMs;
	float  target;
	float  drag;
	int    skipSync;
	int    airborne;
	int    braking;

	if (!d || !c || den == 0)
		return;

	skipSync = (preRace != 0) || (disengaged != 0);
	airborne = (d->airTime != 0.0f);
	braking  = (num < 0) || (preRace != 0);

	dtMs = (double)dtScaled * K_DT_TO_MS;
	if (dtMs > K_DT_CEILING)
		dtMs = K_DT_CEILING;

	/*
	 * THE ENTRY CLAMPS AND THE AIRBORNE ARM,
	 * 0x0048ef60..0x0048f1a0. Was a marked hole; transcribed.
	 *
	 *   0048efaf  if (speed > realSpeedMax) speed = realSpeedMax
	 *   0048efd2  airTime & 0x7fffffff non-zero -> 0x0048f0fb,
	 *             skipping the grounded velocity work
	 *   0048f0fb  clamp gearboxRoadSpeed to +-realSpeedMax
	 *   0048f139  if (EDI == 0)
	 *   0048f149      engineSpeed = SpeedToEngine(&gearboxRoadSpeed,
	 *                                             gear, engineSpeed)
	 *   0048f154  both paths converge here
	 *
	 * *** IN THE AIR THE ENGINE FOLLOWS THE ROAD SPEED. *** That is
	 * the whole point of the block and it is the opposite of what
	 * the coast arm below does. The road speed is only clamped - so
	 * the bike coasts, losing the 1.6 airborne drag and nothing else
	 * - and the engine is RE-DERIVED from it at the top of every
	 * frame, which is what throws away whatever the throttle revved
	 * it to on the frame before.
	 *
	 * WITHOUT IT, HOLDING THE GAS ACCELERATES THE BIKE IN MID-AIR.
	 * Measured off the dirt arena's first ramp: MOTO 32.28 -> 30.00
	 * through the flight against 33.48 -> 40.43 with the re-derive
	 * missing, about 0.87 a frame of torque that should not exist.
	 * racers-convergence.txt 9.46.10.
	 *
	 * IT DOES NOT RETURN. 0x0048f14e falls into 0x0048f154 and the
	 * throttle logic below still runs; the engine it revs is simply
	 * overwritten next frame. Transcribed as the original has it
	 * rather than as an early exit.
	 *
	 * EDI AT 0x0048f137 IS NOT IDENTIFIED. It gates the conversion
	 * and has the shape of the clutch or disengaged; `disengaged` is
	 * used here because it is the flag this port already carries for
	 * that gate, and physics_speed_to_engine takes it anyway.
	 * [open - what EDI is]
	 */
	if (d->speed > realSpeedMax)
		d->speed = realSpeedMax;

	if (airborne) {
		if (d->gearboxRoadSpeed > realSpeedMax)
			d->gearboxRoadSpeed = realSpeedMax;
		else if (d->gearboxRoadSpeed < -realSpeedMax)
			d->gearboxRoadSpeed = -realSpeedMax;

		d->engineSpeed = physics_speed_to_engine(c, gear,
		                                         &d->gearboxRoadSpeed,
		                                         d->engineSpeed,
		                                         disengaged);
	}

	if (num >= 0) {
		target = (float)(((double)num / (double)den)
		                 * (double)c->moteurVitesseMax);

		if (target < d->engineSpeed || num < 1) {
			/*
			 * LIFTING OFF, OR COASTING. The throttle commands less
			 * engine than there is, so the engine decays rather
			 * than driving. This is the branch a four-way split
			 * omits - see the header.
			 */
			if (skipSync || airborne) {
				d->engineSpeed = (float)((double)d->engineSpeed
				                 * (dtMs * K_COAST_DECAY + 1.0));
				if (d->engineSpeed < 0.0f)
					d->engineSpeed = 0.0f;
				if (d->engineSpeed > c->moteurVitesseMax)
					d->engineSpeed = c->moteurVitesseMax;
				if (airborne)
					d->gearboxRoadSpeed = physics_engine_to_speed(
				                    c, realSpeedMax, gear, &d->engineSpeed,
				                    d->gearboxRoadSpeed);
			} else if ((double)d->engineSpeed >
			           (double)c->moteurVitesseMax
			           / (double)c->regimeMax * K_DECAY_FLOOR_RPM) {
				d->engineSpeed = (float)((double)d->engineSpeed
				    + (double)physics_engine_decay_ratio(gear)
				      * (double)d->engineSpeed * dtMs * K_ENGINE_DECAY);
				if (d->engineSpeed < 0.0f)
					d->engineSpeed = 0.0f;
				d->gearboxRoadSpeed = physics_engine_to_speed(
				        c, realSpeedMax, gear, &d->engineSpeed,
				        d->gearboxRoadSpeed);

				/*
				 * AND THE SLIP RESYNC, 0x0048f73e..0x0048f888.
				 * Was `HOLE 0x0048f7a0`; read out and imported
				 * whole. GROUNDED ONLY - 0x0048f74a jumps clear
				 * of it when instance+0xcc is non-zero.
				 *
				 *   0048f775  FLD  [ESI+0x100]   speed, into body
				 *   0048f7f7  the grip block = physics_grip
				 *   0048f869  FLD  [ESI+0x108]   gearboxRoadSpeed
				 *   0048f86f  FSUB [ESP+0x18]    - the body long
				 *   0048f878  FMUL k
				 *   0048f87c  FADD [ESP+0x1c]    + the body long
				 *   0048f883  CALL Racer_SetLongSpeed
				 *
				 * The same lerp-then-SetLongSpeed idiom as
				 * 0x0048fbbc, and it is what puts the velocity
				 * back together after the decay has pulled road
				 * speed down. racers-convergence.txt 9.18.25.
				 */
				if (!airborne && mat && velDir) {
					float lng = physics_long_speed(mat, velDir,
					                              d->speed);
					float slp = physics_slip_speed(mat, velDir,
					                              d->speed);
					float k   = physics_grip(c, (slp < 0.0f)
					                            ? -slp : slp);

					physics_set_long_speed(mat, velDir, &d->speed,
					        realSpeedMax,
					        lng + (d->gearboxRoadSpeed - lng) * k);
				}
			}
		} else if (skipSync || airborne) {
			/* FREE REV - no load, so the engine just rises. */
			d->engineSpeed = (float)((double)d->engineSpeed
			        + (double)c->moteurVitesseMax * dtMs * K_ENGINE_RISE);
			if (d->engineSpeed < 0.0f)
				d->engineSpeed = 0.0f;
			if (d->engineSpeed > c->moteurVitesseMax)
				d->engineSpeed = c->moteurVitesseMax;
			if (airborne)
				d->gearboxRoadSpeed = physics_engine_to_speed(
			                c, realSpeedMax, gear, &d->engineSpeed,
			                d->gearboxRoadSpeed);
		} else {
			/*
			 * THE DRIVE BRANCH, already transcribed and already
			 * held to measured numbers by
			 * tools/check_physics.py. Calling it rather than
			 * re-transcribing keeps that test meaningful.
			 *
			 * PASS THE REAL SLIP, NEVER A ZERO. physics_grip
			 * returns 1.0 for a zero slip, so a hardcoded 0
			 * makes the drive step's tyre lerp run at full grip
			 * and pull gearboxRoadSpeed all the way onto the
			 * speed every frame.
			 *
			 * A STRAIGHT LINE HIDES IT COMPLETELY: the slip IS
			 * zero there, so acceleration and braking match MOTO
			 * to float noise. In a turn they do not. On the dirt
			 * bike, full left at 40 m/s, the slip reaches 0.98 by
			 * the eighth command on BOTH sides - it is computed
			 * correctly either way, the question is only whether
			 * the drive step is handed it - and at MotoType 1's
			 * 1 - 0.05*|slip| that is 0.951 grip against 1.0, so
			 * a zero gains speed through the corner: 0.027 apart
			 * at two commands, 0.199 by fifteen. racers.txt
			 * 9.46.24.
			 */
			{
				float slp = (mat && velDir)
				            ? physics_slip_speed(mat, velDir, d->speed)
				            : 0.0f;

				if (slp < 0.0f)
					slp = -slp;
				physics_tick_drivetrain(d, c, realSpeedMax,
				                        dtScaled, slp,
				                        wheelieScale, driveInterlock,
				                        gear, target, disengaged,
				                        mat, velDir,
				                        ((double)d->airTime >
				                         RACE_AIRTIME_GATE));
			}
		}
	}

	/*
	 * *** THE BRAKE IS GATED, AND THIS IS THE DONUT. ***
	 *
	 * 0x0048f904 does not just ask whether the brake is held:
	 *
	 *     0048f915  turnBrakeFlag != 0 -> run
	 *     0048f91f  driveInterlock != 0 -> run
	 *     0048f927  preRace != 0 -> run
	 *               otherwise the counter is zeroed and the
	 *               whole deceleration block is SKIPPED
	 *
	 * and racer_apply_steering clears turnBrakeFlag for a HUMAN
	 * on a MotoType 1 bike that is braking AND turning at once
	 * (0x00466504 and below). So on a dirt bike, gas and brake
	 * and a direction held together do NOT brake: the bike
	 * keeps its speed and slides, which is the endless donut.
	 * Below 2 m/s the flag is forced back on so a stopped bike
	 * can always brake.
	 *
	 * racers.txt 9.65.
	 *
	 * THE THIRD CONDITION IS NOT driveInterlock. Read off the
	 * disassembly rather than the decompiler:
	 *
	 *     0048f904  TEST EBP,EBP        the drive demand
	 *     0048f906  JL 0048f915         num < 0 -> the tests
	 *     0048f915  EAX = [ESI+0x88]    instance+0x88, which is
	 *                                   racer+0xac - turnBrakeFlag
	 *     0048f91f  EAX = [ESP+0x7c]    A STACK LOCAL
	 *     0048f927  EAX = g_bPreRace
	 *     0048f930  [ESI+0x8c] = 0      else zero and skip
	 *
	 * PASSING driveInterlock THERE MAKES THE GATE UNCONDITIONAL,
	 * because the caller hands it a literal 1 - the brake then
	 * runs whatever turnBrakeFlag says and the donut stops the
	 * bike. Measured with that wiring: turnBrakeFlag went to 0
	 * correctly, gas and brake and right held, and the speed
	 * still fell 19.7 to 9.3.
	 *
	 * [ESP+0x7c] IS NOT IDENTIFIED and is passed as 0. That is
	 * the reading that makes turnBrakeFlag matter at all; if it
	 * turns out to be non-zero in some state, the brake comes
	 * back on there. [open - what [ESP+0x7c] is]
	 *
	 * The two nearby fields are easy to swap: driveInterlock is
	 * racer+0x88 and turnBrakeFlag is racer+0xac. physics.h has
	 * the derivation for both.
	 */
	if (physics_brake_gated(braking, turnBrakeFlag, 0, preRace)) {
		float step = physics_brake_step(c, d->brakeHoldMs,
		                                (float)dtMs, 0);

		physics_apply_brake(d, step, (float)dtMs);
		if (!skipSync)
			d->engineSpeed = physics_speed_to_engine(
		                    c, gear, &d->gearboxRoadSpeed,
		                    d->engineSpeed, disengaged);
		/* HOLE  0x0048fa60 - the slip resync and the brake-hold tail. */
	} else {
		/*
		 * THE RAMP TIMER RESETS WHEN THE BRAKE IS RELEASED, and
		 * physics_apply_brake only ever ADDS to it - so without
		 * this line the counter climbs for the whole race and is
		 * past 200 for every brake after the first.
		 *
		 * 0x0048f930 clears instance+0x8c on the frame the three
		 * brake conditions all fail - driveInterlock at
		 * instance+0x88, the brake flag, and the countdown at
		 * 0x004dc0b4.
		 *
		 * WHAT IT COSTS: the first 200 ms of braking is meant to
		 * be SIX TIMES WEAKER. Measured per frame inside the
		 * drivetrain, braking into a turn, MOTO's step is 0.15278
		 * (100/6 ramped) against 0.91667 (full) without the
		 * reset, shedding 0.195 m/s a frame against 0.923.
		 * STRAIGHT-LINE BRAKING HIDES IT, because both sides are
		 * long past 200 ms by the time a straight test measures
		 * anything.
		 */
		d->brakeHoldMs = 0;
	}

	/*
	 * DRAG. The ground path in the original is a long block -
	 * orientation transform, slope, lateral - before it arrives at the
	 * same v^2 term. Only the term is here.
	 * HOLE  0x0048fc00..0x0048ff00.
	 */
	/*
	 * THE DRAG IS NOT APPLIED HERE. It is subtracted from speed
	 * after physics_set_long_speed (0x00490090), then the road
	 * speed is lerped toward the longitudinal velocity
	 * (0x00490140), which is how the original carries it across.
	 * A `gearboxRoadSpeed -= drag` here skips both steps - see
	 * racers.txt 9.18.20.
	 */
	(void)drag;

	/*
	 * THE CLAMP AND THE ENGINE RESYNC BELONG AT THE END OF THE
	 * TAIL, NOT HERE. maybe_Physics_TickDrivetrain is ONE function
	 * and its order is drive, brake, drag/roll/lerp, clamp,
	 * SpeedToEngine - so a clamp at this point rebuilds the engine
	 * from a road speed the drag has not been taken off.
	 * racers-convergence.txt 9.46.18.2 has the measured pair.
	 *
	 * AND THEN THE TAIL, one call, the way maybe_Racer_ApplyDrive
	 * makes one call to maybe_Physics_TickDrivetrain.
	 * physics_drivetrain_tail is a static half of this function,
	 * kept separate only because 300 lines in one body is worse to
	 * read than two named ones.
	 *
	 * dtMs AND braking ARE THIS FUNCTION'S OWN. The original
	 * computes both once, at the top, and uses them throughout:
	 * dtMs carries the 500 ms ceiling and braking is num < 0 OR
	 * preRace, not num < 0 alone.
	 */
	physics_drivetrain_tail(d, c, realSpeedMax,
	                        mat, velDir, dirY, flAirTime,
	                        (float)dtMs, braking, gear, skipSync,
	                        disengaged);
}


/*
 * THE DRIVETRAIN TAIL, AND IT IS THE SECOND HALF OF ONE ORIGINAL
 * FUNCTION.  maybe_Physics_TickDrivetrain runs the drive step, then
 * the gear shift test, then this.  The port splits it at that seam
 * because the shift test reads gearboxRoadSpeed BETWEEN the two -
 * after the drive step has added its increment and before the roll,
 * the drag and the lerp pull it back down - and the gearbox state it
 * writes belongs to race.c.  The split is the original's own seam,
 * not a convenience.
 *
 * IT IS NOT A SECOND TRANSCRIPTION, AND MUST NOT BECOME ONE.  Two
 * copies of these terms - one here, one in a caller - put terms in
 * the wrong copy: drag gets applied twice, the slope gets applied to
 * speed instead of to the scalar.  One term, one home.
 *
 * ORIGIN  maybe_Physics_TickDrivetrain @ 0x0048ef30, its tail,
 *         WHOLE as of this import - every site from the slope to
 *         the last engine resync, in the original's order:
 *
 *         0x0048fd9b  the brake's slope cut-off
 *         0x0048fdf0  slope onto the measured body z
 *         0x0048fe30  rolling resistance, with the zero snap
 *         0x0048fec5  Racer_SetLongSpeed(v)
 *         0x0048ff10  lerp 1, grounded only
 *         0x0048ffdd  clamp to +/-realSpeedMax
 *         0x0049001b  engine resync
 *         0x00490090  drag, on speed
 *         0x00490140  lerp 2, grounded only
 *         0x00490158  clamp again
 *         0x00490196  engine resync again
 *
 * IMPORTING IT WHOLE IS WHAT BROUGHT lerp 1 AND THE BRAKE
 * CUT-OFF. A slice of this tail omits both, and without them the
 * slope term integrates instead of settling - see the scalar note
 * above.
 */
static void physics_drivetrain_tail(MR_Drivetrain *d,
                             const MR_BikeCarac *c,
                             float realSpeedMax,
                             const float *mat, float *velDir,
                             float dirY, float flAirTime, float dtMs,
                             int braking, int gear, int skipSync,
                             int disengaged)
{
	int airborne = ((double)flAirTime > RACE_AIRTIME_GATE);

	/*
	 * THE FIRST OF THE TWO SetLongSpeed CALLS - 0x0048fbbc.
	 *
	 * ORIGIN  maybe_Physics_TickDrivetrain @ 0x0048ef30,
	 *         at 0x0048fbbc
	 *
	 *     0048fba8  FSUB [ESP+0x18]     ; X - A
	 *     0048fbac  SUB  ESP,4
	 *     0048fbb1  FMUL [ESP+0x20]     ; * k
	 *     0048fbb5  FADD [ESP+0x1c]     ; + A
	 *     0048fbb9  FSTP [ESP]
	 *     0048fbbc  CALL Racer_SetLongSpeed
	 *
	 * THIS IS WHAT LEAVES THE TILT IN velDir.  SetLongSpeed
	 * replaces only the row-2 body component and rebuilds speed as
	 * |w|, so with the other two preserved the speed comes out
	 * ABOVE the longitudinal - measured, 19.68011 against 19.67410.
	 * That 0.006 is the frame-2 decoupling, and it needs BOTH
	 * calls: one of the two leaves speed and the longitudinal
	 * equal. racers.txt 9.18.23.4.
	 *
	 * A IS UNREAD.  The argument is the grip-lerp idiom again,
	 * (X - A) * k + A, and only X is identified: k is
	 * physics_grip, which returns 1.0 for MotoType 2, and the
	 * argument then measured EXACTLY gearboxRoadSpeed - so X is
	 * gearboxRoadSpeed and A cannot be separated while k is 1.
	 * Transcribed at k = 1 with the gap named rather than guessed;
	 * a MotoType 1 bike, where grip leaves 1.0, needs A read first.
	 * [open]
	 *
	 * *** IT IS THE BRAKE PATH'S, AND IT HAS NO AIRBORNE GATE. ***
	 *
	 * Hooked at 0x0048fbc1, the instruction AFTER the call - the
	 * CALL itself cannot be hooked, being relative, and the stub
	 * would relocate it. Driven through all four input states:
	 *
	 *     throttle, grounded     0 hits
	 *     throttle, airborne     0 hits
	 *     coast                  0 hits
	 *     brake,    grounded     every frame, airTime 0
	 *     brake,    airborne     every frame, airTime 5.184,
	 *                            7.776, 10.368, 12.96
	 *
	 * So it is NOT in the shared tail and NOT grounded-only: a
	 * braking bike runs it in mid-air.
	 *
	 * A WATCHPOINT THAT SEES NO WRITER PROVES NOTHING UNLESS THE
	 * RUN EXERCISED THE PATH. An `!airborne` guard here is what
	 * you get from a watchpoint that saw no airborne write during
	 * a run that held the THROTTLE - and on the throttle this
	 * site never fires at all. The table above is the evidence
	 * because it drives every input state, not one.
	 * racers-convergence.txt 9.46.15.
	 */
	if (braking)
		physics_set_long_speed(mat, velDir, &d->speed,
	                           realSpeedMax,
	                           d->gearboxRoadSpeed);

	/*
	 * THE SLOPE GOES IN HERE, INTO THE SCALAR - not onto
	 * speed at the ground-contact seam further down.
	 *
	 * ORIGIN  maybe_Physics_TickDrivetrain @ 0x0048ef30,
	 *         at 0x0048fdf0
	 *
	 *     0048fd91  FSTP [ESP+0x10]     scalar = body z
	 *     0048fdb4  TEST [ESI+0xcc]     airborne -> term 0
	 *     0048fdc1  FLD  [ESP+0x98]     dirY
	 *     0048fdc8  FCHS
	 *     0048fdca  FMUL [ESP+0xa8]     * dtMs
	 *     0048fdd1  FMUL [0x004d1d60]   * 0.004905
	 *     0048fde8  ...clamped to 0.55 at 0x004d1d68
	 *     0048fdf0  FLD  [ESP+0x4c]
	 *     0048fdf4  FADD [ESP+0x84]     + body z
	 *     0048fdfb  FSTP [ESP+0x10]     scalar = z + slope
	 *
	 * IT IS NOT A HILL, IT IS THE BRAKE DIVE. dirY is the
	 * forward vector's Y, so pitching nose-down under braking
	 * feeds a positive term - which is why a missing slope
	 * shows as a constant deficit while braking and not on the
	 * flat. See racers.txt 9.18.23.
	 *
	 * *** THE SCALAR IS THE MEASURED BODY z. IT IS NOT
	 *     gearboxRoadSpeed. ***
	 *
	 * The two COINCIDE ON A DRIVEN BIKE, which is why a hook at
	 * 0x0048fe7b reads the scalar a constant 0.00397 above
	 * gearboxRoadSpeed and equal to it once that offset is
	 * taken off. Do not conclude from that they are the same
	 * quantity.
	 *
	 * SUBSTITUTING ONE FOR THE OTHER TURNS A FEEDBACK LOOP INTO
	 * AN INTEGRATOR, and on a hill that is the whole
	 * difference. Reading the ACTUAL longitudinal velocity back
	 * out of speed and velDir every frame gives the slope
	 * something that answers: at rest on an incline body z
	 * settles at +0.0106 against a slope of -0.0106 and v is 0.
	 * Read the stored road speed instead and nothing ever
	 * contradicts the slope, so it sums frame on frame and the
	 * bike creeps away uphill gaining speed - 40 idle frames
	 * from one pose, MOTO holding -0.0106 against -0.41.
	 *
	 * physics_long_speed IS THE TRANSFORM AT
	 * 0x0048fc0e..0x0048fd8a, so the five matrix helpers it
	 * calls still need not be imported - but it has to be
	 * CALLED, not assumed away.
	 *
	 * AT PARITY SINCE, on a dirY of 0.104 pointed up the hill:
	 * dirY, speed and gearboxRoadSpeed identical to five
	 * decimals on both sides and steady over 40 idle frames,
	 * 0.00816 / -0.00816.  A 30-frame uphill launch agrees to
	 * 9.742619 against 9.742608 with gear and tachoRpm exact,
	 * and brake-to-a-stop then 60 frames of release holds 0 on
	 * both.  A 45-frame full-left turn-in from 16.6 m/s is
	 * identical on speed and gbrs to four decimals, yaw within
	 * 0.1 deg, and 0.1 world units apart after 2400 travelled.
	 * [verified - live, both builds]
	 *
	 * THE LATERAL TERM IS STILL UNEXERCISED.  d->speed is now
	 * |world| rather than the longitudinal scalar, so it carries
	 * the sideways component - but on a road bike grip is 1.0
	 * and the slip lerp converges completely, so slip measured
	 * 0.0000 through every corner above.  Proving that half
	 * needs MotoType 1, where grip is 1 - 0.05*|slip|.
	 * [open - dirt is untested]
	 */
	float bodyZ = physics_long_speed(mat, velDir, d->speed);
	float v     = bodyZ;
	float roll;

	/*
	 * AND THE BRAKE SUPPRESSES THE SLOPE OUTRIGHT below
	 * 0.1 m/s - 0x0048fd9b, against the double at 0x004d1ca0.
	 * braking is the original's own local_8, num < 0 ||
	 * preRace, which is what this parameter already carries.
	 *
	 * THIS IS WHY A BRAKED BIKE STAYS PUT ON A HILL and why
	 * releasing the brake is what starts it moving.  Without
	 * it the slope keeps working through the brake and the
	 * bike creeps while stopped.
	 */
	if (!braking || d->speed > K_SLOPE_BRAKE_CUT)
		v = bodyZ + physics_slope(dirY, dtMs, airborne);

	/*
	 * ROLLING RESISTANCE, every frame - after the drive step and
	 * on top of the v-squared drag. Sign preserved, toward zero.
	 *
	 * IT IS APPLIED TO THE LONGITUDINAL SCALAR, NOT TO THE SPEED.
	 * In the original that scalar is the stack local [ESP+0x10]
	 * the whole drive-brake-drag-roll chain has been assembling;
	 * the roll lands on it at 0x0048fe82 (negative) or 0x0048fe9d
	 * (positive), with the zero crossing snapped at 0x0048feb6.
	 */
	roll = physics_rolling_resistance(c, d->speed, dtMs);

	if (v > 0.0f) {
		v -= roll;
		if (v < 0.0f)
			v = 0.0f;          /* 0x0048feb6, the snap */
	} else if (v < 0.0f) {
		v += roll;
		if (v > 0.0f)
			v = 0.0f;
	}

	/*
	 * AND THEN IT GOES THROUGH Racer_SetLongSpeed, which is the
	 * whole point of the assembly.
	 *
	 * ORIGIN  maybe_Physics_TickDrivetrain @ 0x0048ef30,
	 *         at 0x0048fec5
	 *
	 * physics_set_long_speed replaces ONLY the row-2 component
	 * of the body-space velocity and keeps the other two, so the
	 * bike's lateral and vertical motion survive the drive step
	 * instead of being flattened. It then rebuilds speed as
	 * |world| and rewrites velDir.
	 *
	 * DO NOT WRITE `gearboxRoadSpeed = speed` HERE. On a straight
	 * the two agree exactly - with velDir along row 2 the other
	 * body components are zero, so |world| is |v| - so that
	 * substitution passes every straight-line test and loses the
	 * lateral and vertical motion everywhere else.
	 *
	 * THE CLAMP IS THE RACER COPY, instance+0x90 - racer+0xb4,
	 * which this port calls MR_Racer.realSpeedMax. It arrives as
	 * a parameter; the caller passes r->realSpeedMax.
	 *
	 * IT IS NOT THE CARAC FIELD OF THE SAME NAME. race_load
	 * seeds each racer from its own carac and then diverges:
	 * opponents take computerSpeed[] scaled by lane, and a
	 * manual rider gets +2.0. Reading carac->realSpeedMax
	 * instead flattens every AI racer onto the player's top
	 * speed. An unseeded 0.0 clamps v to zero and the bike does
	 * not move at all, which is what the field being dead used
	 * to cause.
	 *
	 * THE COPY PROTECTION ZEROES racer+0xb4 to pin a pirated
	 * bike at a standstill (copy-protection.txt 5.1).  That
	 * payload is not reproduced - a rebuilt executable takes the
	 * pass branch - so reading the field is safe here.
	 */
	/*
	 * THE SPEED REBUILD IS GROUNDED ONLY, and unlike the call
	 * above this one is MEASURED. Hooked at 0x0048feca through
	 * the same four states: it fires on every GROUNDED frame,
	 * throttle and brake alike, and NOT ONCE in twenty airborne
	 * frames on either. racers-convergence.txt 9.46.15.
	 *
	 * v is gearboxRoadSpeed plus the slope, so running it in the
	 * air would assign the speed off the free ramp a second time.
	 *
	 * gearboxRoadSpeed ITSELF STILL MOVES. The ramp is the
	 * original's behaviour - MOTO's gbrs climbs through a flight
	 * just as ours does - and only the speed must stay off it.
	 */
	if (!airborne)
		physics_set_long_speed(mat, velDir,
	                           &d->speed,
	                           realSpeedMax, v);
	/*
	 * ##### LERP 1 OF 2, AND v IS NEVER ASSIGNED TO gbrs.
	 *
	 * ORIGIN  maybe_Physics_TickDrivetrain @ 0x0048ef30,
	 *         at 0x0048ff10..0x0048ffd7
	 *
	 * What stood here was `d->gearboxRoadSpeed = v`, and the
	 * original has no such assignment anywhere: v goes into
	 * Racer_SetLongSpeed and NOWHERE else.  The road speed is
	 * then blended toward the body z that call just produced.
	 *
	 *     vel  = normalize(velDir) * speed
	 *     out  = inverse(matOrientation) * vel
	 *     gbrs += (out.z - gbrs) * k
	 *
	 * SAME k AS LERP 2.  The original inlines the factor here
	 * and calls FUN_0048eb40 for the other, but the two are
	 * the same expression - the 0.01..20 slip band at rate
	 * -0.05 for MotoType 1, max(1, MinAdherenceWheel)
	 * otherwise.  On a road bike k is 1.0 and this is a plain
	 * assignment of the measured longitudinal velocity.
	 *
	 * GROUNDED ONLY - 0x0048fed5 jumps straight to the clamp
	 * when racer+0xf0 is non-zero.
	 */
	if (!airborne) {
		float lng1 = physics_long_speed(mat, velDir, d->speed);
		float slp1 = physics_slip_speed(mat, velDir, d->speed);
		float k1   = physics_grip(c, (slp1 < 0.0f) ? -slp1 : slp1);

		d->gearboxRoadSpeed +=
		        (lng1 - d->gearboxRoadSpeed) * k1;
	}

	/*
	 * THE FIRST CLAMP AND THE FIRST ENGINE RESYNC.
	 *
	 * ORIGIN  at 0x0048ffdd..0x0049003e
	 *
	 * THE ORIGINAL CLAMPS AND RESYNCS TWICE - here and again
	 * after the second lerp.  Both are kept.  Dropping this
	 * one lets an out-of-range road speed reach the drag.
	 */
	if (d->gearboxRoadSpeed > realSpeedMax)
		d->gearboxRoadSpeed = realSpeedMax;
	else if (d->gearboxRoadSpeed < -realSpeedMax)
		d->gearboxRoadSpeed = -realSpeedMax;

	if (!skipSync)
		d->engineSpeed = physics_speed_to_engine(c, gear,
	                                             &d->gearboxRoadSpeed,
	                                             d->engineSpeed,
	                                             disengaged);

	/*
	 * THE DRAG, ON THE SPEED SetLongSpeed HAS JUST REBUILT.
	 *
	 * ORIGIN  maybe_Physics_TickDrivetrain @ 0x0048ef30,
	 *         at 0x00490090
	 *
	 * 0x00490090 is past Racer_SetLongSpeed at 0x0048fec5 and
	 * does NOT touch gearboxRoadSpeed.
	 */
	{
		float drag = physics_drag(c, d->speed, dtMs, airborne);

		d->speed -= drag;
		if (d->speed < 0.0f)
			d->speed = 0.0f;
	}

	/*
	 * AND THE ROAD SPEED LERPS TOWARD THE LONGITUDINAL VELOCITY.
	 * THIS is what carries the drag across.
	 *
	 * ORIGIN  maybe_Physics_TickDrivetrain @ 0x0048ef30,
	 *         at 0x00490140
	 *
	 *     0049011a  invert matOrientation
	 *     0049012a  body = world velocity through it
	 *     0049013b  k = FUN_0048eb40, which IS physics_grip:
	 *                    read out, its two branches are the same
	 *                    band (0.01..20, rate -0.05) for MotoType 1
	 *                    and max(1.0, MinAdherenceWheel) otherwise.
	 *     00490140  FLD  [ESP+0x18]        body.z, longitudinal
	 *     00490144  FSUB [ESI+0x108]
	 *     0049014a  FMULP                  * k
	 *     0049014c  FADD [ESI+0x108]
	 *     00490152  FSTP [ESI+0x108]
	 *
	 * ON A ROAD BIKE k IS 1.0 and the lerp is a plain copy - the
	 * road speed simply becomes the longitudinal velocity, drag
	 * included.
	 *
	 * THERE ARE THREE COPIES OF THIS IDIOM across two functions,
	 * and this is the one that matters. The identical lerp at
	 * 0x0048dd05 in ModelInstance_Integrate is gated on the speed
	 * change WITHIN that function and so never fires on a grounded
	 * frame; a data watchpoint on instance+0x108 is what separates
	 * them.
	 *
	 * *** THE !airborne GATE IS A DIVERGENCE. *** The instruction
	 * at 0x00490140 has no gate - do not "restore" one by deleting
	 * it after reading the disassembly. It is read off MOTO's
	 * behaviour instead: in the air gearboxRoadSpeed IS NOT THE
	 * BIKE'S SPEED, but a free ramp of exactly +1.7424 a frame
	 * through a jump with the gas held - 5.5 times the engine's
	 * +0.3168, which is the gear ratio - while the real speed
	 * decays on drag alone. The two are decoupled until touchdown,
	 * when gbrs snaps to the speed. Ungated, this lerp makes gbrs
	 * shadow the speed all the way through a flight instead of
	 * ramping.
	 *
	 * NECESSARY, NOT SUFFICIENT. It does NOT explain the speed
	 * itself climbing in the air - this writes gbrs, not speed -
	 * and that is still open. racers-convergence.txt 9.46.10.
	 */
	if (!airborne) {
		float lng = physics_long_speed(mat,
		                               velDir,
		                               d->speed);
		float slp = physics_slip_speed(mat,
		                               velDir,
		                               d->speed);
		float k   = physics_grip(c, (slp < 0.0f) ? -slp : slp);

		d->gearboxRoadSpeed +=
		        (lng - d->gearboxRoadSpeed) * k;
	}

	/*
	 * AND THE CLAMP AND THE ENGINE RESYNC, LAST - the original's
	 * own last act, after the drag and the lerp have finished with
	 * the road speed. Clamping before them instead rebuilds the
	 * engine from a pre-drag road speed; racers-convergence.txt 9.46.18.2 has
	 * the measured pair.
	 *
	 * THE ENGINE IS RESYNCHRONISED ON EVERY PATH, which is what
	 * keeps it alive with no throttle at all.
	 */
	if (d->gearboxRoadSpeed > realSpeedMax)
		d->gearboxRoadSpeed = realSpeedMax;
	else if (d->gearboxRoadSpeed < -realSpeedMax)
		d->gearboxRoadSpeed = -realSpeedMax;

	if (!skipSync)
		d->engineSpeed = physics_speed_to_engine(c, gear,
	                                             &d->gearboxRoadSpeed,
	                                             d->engineSpeed,
	                                             disengaged);
}


/*
 * ORIGIN  maybe_Gearbox_EngineToSpeed @ 0x0048dd90
 *
 * ENGINE SPEED BACK TO ROAD SPEED - the inverse of
 * physics_speed_to_engine. The pair bracket the drive step:
 * speed -> engine, cap the engine, engine -> speed.
 *
 * THE ENGINE IS PASSED BY ADDRESS because the clamp writes it back:
 * when realSpeedMax bites, the engine is recomputed from the capped
 * speed rather than left disagreeing with it.
 *
 * THE ORIGINAL SUBTRACTS 9.0 FROM THE CLAMP WHEN g_windowTokenOk IS
 * CLEAR. That is a copy-protection payload - catalogued in
 * subsystems/copy-protection.txt - and it is NOT reproduced: a
 * rebuilt binary has no bytes to check and is legitimate by
 * construction, so it takes the pass branch. Same rule as the
 * driveInterlock gate in engine_torque_at.
 *
 * THE CLAMP VALUE IS THE CALLER'S. The original clamps against
 * the RACER copy at instance+0x90 - MR_Racer.realSpeedMax,
 * racer+0xb4 - and this function takes whatever it is handed, so
 * which one it gets is a property of each call site and not of
 * this code.
 *
 * THE TWO ARE NOT THE SAME NUMBER. Racer_Create adds 2.0 to the
 * racer copy for a human, solo, manual rider, and race_load gives
 * opponents computerSpeed[] scaled by lane - so a call passing the
 * carac caps that bike where the original does not. The drivetrain
 * path passes the racer copy (racer.c, at the
 * physics_drivetrain_frame call); other sites pass the carac and
 * say so locally. racers.txt 9.33.2.1 has the table of which
 * original sites read which. [open - the carac sites predate the
 * racer field having writers and want re-reading one at a time]
 */
float physics_engine_to_speed(const MR_BikeCarac *c,
                              float realSpeedMax, int gear,
                              float *engine, float speed)
{
	double top;
	double v;

	if (!c || !engine)
		return speed;
	if (gear < 1 || (double)gear > (double)c->nbSpeed)
		return speed;

	/* The same 0.0001 cutover the drive branch divides by. */
	if ((double)*engine < K_DIV_THRESHOLD)
		return speed;

	top = (double)physics_gear_top_speed(c, gear);
	if (top <= 0.0 || (double)c->moteurVitesseMax <= 0.0)
		return speed;

	v = (double)*engine / (double)c->moteurVitesseMax * top;
	if (v < 0.0)
		v = 0.0;

	if ((double)realSpeedMax < v) {
		v = (double)realSpeedMax;
		*engine = (float)((double)c->moteurVitesseMax / top * v);
	}
	return (float)v;
}


/*
 * ORIGIN  Gearbox_SyncRoadSpeed @ 0x0048edd0
 *
 * ROAD SPEED FOLLOWS THE BODY SPEED, clamped to realSpeedMax.
 * Four lines in the original and the whole of it:
 *
 *     instance+0x108 = instance+0x100;            gbox = speed
 *     if (instance+0x90 < instance+0x100)         over the cap
 *         instance+0x108 = instance+0x90;
 *     else if (instance+0x108 < -instance+0x90)
 *         instance+0x108 = -instance+0x90;
 *
 * NOTE THE else. The negative clamp is only reached when the
 * positive one did not fire, which is how the original wrote it.
 *
 * WHAT IT IS FOR: anything that changes speed WITHOUT going
 * through the gearbox - a wall impact is the case that found it -
 * leaves gearboxRoadSpeed stale, and the next frame rebuilds speed
 * from it. Measured across a rail glance, the port dropped speed
 * to 7.9250 with gbox still at 14.9829 and had 14.9701 back one
 * frame later. racers.txt 9.18.23.3.
 */
void physics_sync_road_speed(float *roadSpeed, float speed,
                             float realSpeedMax)
{
	if (!roadSpeed)
		return;

	*roadSpeed = speed;
	if (realSpeedMax < speed) {
		*roadSpeed = realSpeedMax;
		return;
	}
	if (*roadSpeed < -realSpeedMax)
		*roadSpeed = -realSpeedMax;
}


/* ORIGIN  maybe_Gearbox_SpeedToEngine @ 0x0048deb0, the inverse
 *         of FUN_0048dd90 - physics_engine_to_speed above */
float physics_speed_to_engine(const MR_BikeCarac *c, int gear,
                              float *roadSpeed, float engine,
                              int disengaged)
{
	double den;
	double e;

	if (!c || !roadSpeed)
		return 0.0f;

	/*
	 * DISENGAGED PASSES THE ENGINE STRAIGHT BACK - 0x0048dec4,
	 * the first thing the original does. instance+0x60 is
	 * racer+0x84, the gearbox state, which the port carries as
	 * the disengaged flag.
	 *
	 * THIS IS WHAT KEEPS A HIGH ENGINE OVER A LOW ROAD SPEED.
	 * Measured across a rail glance: 3.5708 went in with road
	 * speed 7.9250 in gear 2, and 3.5708 came back where a plain
	 * conversion gives 7/36.6 * 7.925 = 1.5157.
	 */
	if (disengaged)
		return engine;

	den = (double)physics_gear_top_speed(c, gear);
	if (den == 0.0)
		return 0.0f;

	/*
	 * A SIGN TEST ON THE FLOAT BITS, not a magnitude test.
	 * < 0x80000001 admits every positive float and -0.0 and
	 * rejects the rest, so a NEGATIVE road speed skips the
	 * conversion and keeps whatever the caller passed. Writing it
	 * as *roadSpeed >= 0.0f changes what happens when the bike
	 * rolls backwards.
	 */
	{
		unsigned int bits;

		memcpy(&bits, roadSpeed, sizeof bits);
		if (bits < 0x80000001u)
			e = (double)c->moteurVitesseMax / den
		        * (double)*roadSpeed;
		else
			e = (double)engine;
	}

	{
		float        ef = (float)e;
		unsigned int bits;

		memcpy(&bits, &ef, sizeof bits);
		if (bits > 0x80000000u)          /* negative -> zero */
		    e = 0.0;
	}

	/*
	 * THE REV LIMITER, and it writes the ROAD SPEED back - which
	 * is why the original takes it by address. With the engine
	 * pinned the speed the gear can carry is exactly den.
	 */
	if (e > (double)c->moteurVitesseMax) {
		e = (double)c->moteurVitesseMax;
		*roadSpeed = (float)(den / (double)c->moteurVitesseMax * e);
	}

	return (float)e;
}


/* The shift timers, in milliseconds. Immediates in the requesters. */
/*
 * The sweep thresholds. BOTH ARE DOUBLES IN THE ORIGINAL -
 * 0x004d1ca0 holds 0.1 and 0x004d1d38 holds 0.97 - so reading
 * either as a 4-byte float gives garbage.
 *
 * DO NOT PUT AN f ON EITHER. That is the same mistake in a
 * quieter form: 0.97f promotes to 0.97000002861022949, 2.9e-8
 * off, and the original's own arithmetic is double either way -
 *
 *     0048e498  FMUL  double ptr [0x004d1d38]   then FCOMP a float
 *     0048e4b7  FADD  double ptr [0x004d1ca0]   onto a float v
 *
 * so a float constant diverges at both sites. GB_ENGINE_FRAC
 * gates a SHIFT, where a small error changes a discrete outcome,
 * and GB_SWEEP_STEP is ACCUMULATED - 100 adds of 0.1f drift
 * 1.9e-6 from 100 adds of 0.1.
 *
 * 0x004d1ca0 IS ALSO K_SLOPE_BRAKE_CUT at the top of this file.
 * Same constant, two meanings - a brake cut-off and a sweep step -
 * so the two names stay.
 */
#define GB_SWEEP_STEP      0.1      /* DAT_004d1ca0, a DOUBLE     */
#define GB_ENGINE_FRAC     0.97     /* DAT_004d1d38, a DOUBLE,
                                     * gears 1..3                 */

/* DAT_004d1de0, a DOUBLE. The engine bleeds off at this rate per
 * millisecond while the clutch is out. */
#define GB_ENGINE_COAST   -0.0006

/* The rolling-resistance constants. 0x004d1e18, 0x004d1e20 and
 * 0x004d1c48 are DOUBLES; 0x004d0e4c is a float. */
#define K_ROLL_BASE      0.02f      /* 0x004d0e4c, FUN_00459f90  */
#define K_ROLL_DIRT      0.5        /* 0x004d1c48, MotoType 1    */
#define K_ROLL_G         0.00981    /* 0x004d1e18, 9.81 / 1000   */
#define K_ROLL_RAMP      0.05       /* 0x004d1e20, below the cut */
#define K_ROLL_RAMP_CUT  20.0f      /* 0x41a00000                */

/* ORIGIN  maybe_Physics_TickDrivetrain @ 0x0048ef30, at 0x0048fe30 */
float physics_rolling_resistance(const MR_BikeCarac *c, float speed,
                                 float dtMs)
{
	double r;

	if (!c)
		return 0.0f;

	r = (double)K_ROLL_BASE;
	if (c->motoType == 1)
		r *= K_ROLL_DIRT;

	r *= (double)dtMs * K_ROLL_G;

	/*
	 * THE ORIGINAL COMPARES THE FLOAT BITS AS A SIGNED INT against
	 * 0x41a00000, so every NEGATIVE speed takes this branch too and
	 * the product goes negative with it. That is faithful, and it
	 * means rolling resistance pushes a reversing bike faster
	 * backwards. Reverse is not reachable in normal play, so it is
	 * transcribed as written rather than quietly fixed.
	 */
	if (speed < K_ROLL_RAMP_CUT)
		r *= (double)speed * K_ROLL_RAMP;

	return (float)r;
}


/*
 * 0x004d1dd0, a DOUBLE. The engine free-revs at this rate per ms
 * while the clutch is out and the throttle is still open.
 *
 * [open] THIS IS K_ENGINE_RISE, and GB_ENGINE_COAST above is
 * K_COAST_DECAY - same addresses, same values, and used in the
 * same two expressions. They exist twice because this function
 * does; see the note on physics_engine_declutch.
 */
#define GB_ENGINE_RISE    0.0006

/*
 * ORIGIN  maybe_Physics_TickDrivetrain @ 0x0048ef30, at 0x0048f8b0
 *
 * [open] NOTHING CALLS THIS, AND ITS TWO ARMS ARE ALREADY LIVE
 * INSIDE physics_drivetrain_frame - the multiplicative decay at
 * the K_COAST_DECAY site and the additive rise at the
 * K_ENGINE_RISE site, in the same expressions. So either this is a
 * SECOND original block that still needs wiring, or it is a
 * redundant transcription of the free-rev arm and should go with
 * its two duplicate constants.
 *
 * Settling it needs the block at 0x0048f8b0 read properly; that
 * address disassembles to [ESI+0xc8] compares rather than engine
 * arithmetic, so the tag itself is suspect.
 */
void physics_engine_declutch(MR_Drivetrain *d, const MR_BikeCarac *c,
                             float dtMs, float throttleTarget)
{
	double e;

	if (!d || !c)
		return;

	e = (double)d->engineSpeed;

	/*
	 * TWO ARMS, AND THE SIGN IS NOT OBVIOUS. With the throttle
	 * target BELOW the engine the engine decays multiplicatively;
	 * at or above it, it RISES additively toward the red line and
	 * clamps there. So holding the throttle through a shift makes
	 * the engine free-rev - clutch out, throttle open - rather than
	 * fall, and the original sits at exactly 12000 rpm for the whole
	 * 200 ms while the bike slows down.
	 *
	 * TRANSCRIBING ONLY THE DECAY ARM READS AS PLAUSIBLE AND IS
	 * WRONG: it puts the tachometer at 11685 where the original
	 * holds 12000.
	 */
	if ((double)throttleTarget < e)
		e *= 1.0 + (double)dtMs * GB_ENGINE_COAST;
	else
		e += (double)c->moteurVitesseMax * (double)dtMs * GB_ENGINE_RISE;

	if (e < 0.0)
		e = 0.0;
	if (e > (double)c->moteurVitesseMax)
		e = (double)c->moteurVitesseMax;
	d->engineSpeed = (float)e;
}


/* ORIGIN  Gearbox_BuildShiftTable @ 0x0048e3a0 */
void gearbox_build_shift_table(const MR_BikeCarac *c,
                               float table[GB_SHIFT_TABLE_MAX])
{
	float cap;
	float v;
	int   gear;
	int   i;

	if (!c || !table)
		return;
	for (i = 0; i < GB_SHIFT_TABLE_MAX; i++)
		table[i] = 0.0f;

	/*
	 * THE CAP. The original reads a carac field the decompiler
	 * renders as MinAdherenceDerap[(int)nbSpeed - 13], which is an
	 * artifact of one float array being indexed past its end - the
	 * arithmetic is ratio[nbSpeed-1] * vitesseMaxBoite, the top
	 * gear's top speed. Clamped by realSpeedMax.
	 * [inferred: the arithmetic is read, the field name is not]
	 *
	 * It did not bind on the stock bike - every threshold came from
	 * the crossover - so this arm is UNVERIFIED against a measured
	 * table and only matters for a bike geared past its top speed.
	 */
	cap = physics_gear_top_speed(c, (int)c->nbSpeed);
	if (cap <= 0.0f || c->realSpeedMax <= cap)
		cap = c->realSpeedMax;

	gear = 1;
	v    = 0.0f;
	while ((float)gear < c->nbSpeed && v < cap) {
		/* COPIES - the sweep must not be clamped by its own probe,
		 * which is why the original passes &local_c and not the
		 * loop variable. */
		float v1 = v, v2 = v;
		float e1 = physics_speed_to_engine(c, gear, &v1, 0.0f, 0);
		float e2 = physics_speed_to_engine(c, gear + 1, &v2, 0.0f, 0);

		/* torque * engineSpeed IS power. */
		float p1 = engine_torque_at(c, e1, 1.0f, 1) * e1;
		float p2 = engine_torque_at(c, e2, 1.0f, 1) * e2;

		if (p1 < p2
		    || (gear < 4 && c->moteurVitesseMax * GB_ENGINE_FRAC < e1)) {
			if (gear < GB_SHIFT_TABLE_MAX)
				table[gear] = v;
			gear++;
		}
		/*
		 * DOUBLE ADD, FLOAT STORE - the original's own shape,
		 * FADD double at 0x0048e4b7 onto the float v. The cast
		 * is explicit only to say so; without it MSVC warns
		 * C4305 about the truncation it is asked for.
		 */
		v = (float)((double)v + GB_SWEEP_STEP);
	}

	/* Any gear the sweep never reached hands over at the cap. */
	while ((float)gear < c->nbSpeed && gear < GB_SHIFT_TABLE_MAX) {
		table[gear] = cap;
		gear++;
	}
}


#define GB_UP_MS_AUTO      200
#define GB_UP_MS_MANUAL    150      /* 0x96 */
#define GB_DOWN_MS_AUTO    300
#define GB_DOWN_MS_MANUAL  200

#define GB_STATE_IDLE      0
#define GB_STATE_UP        1
#define GB_STATE_DOWN      2

/* ORIGIN  maybe_Gearbox_RequestUpshift @ 0x0048e0a0 */
void gearbox_request_upshift(MR_Gearbox *g, const MR_BikeCarac *c)
{
	if (!g || !c)
		return;
	if (g->state != GB_STATE_IDLE)
		return;                 /* a shift is already in flight */

	/*
	 * Already at or past the top. NOT A NO-OP - it writes the gear,
	 * so a cracked copy whose nbSpeed lost 1.0 gets forced down.
	 */
	if ((double)c->nbSpeed <= (double)g->gear) {
		g->gear = (int)c->nbSpeed;
		return;
	}

	if ((double)(g->gear + g->pending) < (double)c->nbSpeed) {
		g->pending++;
		g->timerMs = (g->mode != 0) ? GB_UP_MS_AUTO : GB_UP_MS_MANUAL;
		g->state   = GB_STATE_UP;
	}
}


/* ORIGIN  maybe_Gearbox_RequestDownshift @ 0x0048e120 */
void gearbox_request_downshift(MR_Gearbox *g)
{
	int state;

	if (!g)
		return;

	state = g->state;
	if (state != GB_STATE_IDLE && state != GB_STATE_DOWN)
		return;                 /* busy going up */

	if (g->gear < 2) {
		g->gear = 1;
		return;
	}

	/*
	 * pending goes MORE NEGATIVE each time, which is how several
	 * gears queue into one change. The timer is only armed on the
	 * first request - a second one while already counting down adds
	 * a gear without restarting the clock.
	 */
	if (g->gear + g->pending > 1) {
		g->pending--;
		if (state != GB_STATE_DOWN) {
			g->timerMs = (g->mode != 0) ? GB_DOWN_MS_AUTO
			                            : GB_DOWN_MS_MANUAL;
			g->state = GB_STATE_DOWN;
		}
	}
}


/* ORIGIN  maybe_Gearbox_TickShift @ 0x0048e6e0 */
/*
 * THE AUTOMATIC GEARBOX BANDS, doubles at 0x004d1ca8 on a stride of
 * 16. THE SHIFT POINT AND THE RANGE TOP ARE DIFFERENT NUMBERS -
 * gear 1 shifts up at 0.18 while its range runs to 0.23 - and that
 * gap is what stops the tachometer snapping to zero on every change.
 */
static const double kGearShiftRoad[5] = { 0.18, 0.33, 0.48, 0.62, 0.74 };
static const double kGearTopRoad[6]   = { 0.23, 0.38, 0.53, 0.67, 0.79,
	                                      1.00 };
/*
 * DIRT BIKES HAVE FIVE GEARS, NOT SIX, and their own bands at
 * 0x004d1cf8. A real difference between the two bike families,
 * decided here and not in the carac table.
 */
static const double kGearShiftDirt[4] = { 0.15, 0.30, 0.45, 0.75 };
static const double kGearTopDirt[5]   = { 0.20, 0.35, 0.50, 0.80,
	                                      1.00 };

/*
 * ORIGIN  maybe_Gearbox_AutoSelect @ 0x0048e180
 *
 * Picks the gear from ROAD SPEED and returns the rev fraction that
 * becomes the tachometer.
 *
 *     gear = the first band speed falls under
 *     rev  = speed / (rangeTop[gear] * realSpeedMax), clamped 0..1
 *
 * and the caller turns rev into racer+0x11c as rev * RegimeMax with
 * a floor of 800, the idle.
 *
 * ITS SPEED ARGUMENT ARRIVES IN A FIELD, NOT A PARAMETER, in the
 * original: Racer_UpdateDash saves racer+0xec, writes
 * speedKmhNeedle * (1/3.6) into it, calls this, and puts the old
 * value back. SO THE AUTO SHIFT IS DECIDED ON WHAT THE BIKE IS DOING
 * OVER THE GROUND, not on the drivetrain's own number - and a dump
 * taken between frames shows the RESTORED value while the gear and
 * the tacho were computed from the temporary. That is what made
 * racer+0xec read ambiguously; see structs/MR_Racer.txt 9.5.
 *
 * This port passes the speed rather than staging it through the
 * field, because nothing here needs the side effect.
 *
 * THE CLAMP IS THE FLOAT-BITS IDIOM AGAIN: (int)bits < 0x3f800001 is
 * "at most 1.0", and the negative case falls through to the >= 0
 * test inside it.
 *
 * [open] WHICH realSpeedMax THIS READS IS UNVERIFIED. It takes the
 * carac field here. racers.txt 9.33.2.1 tables the five consumers
 * that were read out of the binary and this is not one of them, so
 * whether 0x0048e180 reads g_bikeCarac or racer+0xb4 has not been
 * checked. The two differ by 2.0 for a manual rider and by more
 * for an opponent, and this decides a GEAR - so it matters.
 */
float gearbox_auto_select(const MR_BikeCarac *c, float speed,
                          int *gearOut)
{
	const double *shift;
	const double *top;
	int           n;
	int           gear;
	double        rev;
	int           i;

	if (!c)
		return 0.0f;

	if (c->persoAnim == 0) {
		shift = kGearShiftRoad;
		top   = kGearTopRoad;
		n     = 5;
	} else {
		shift = kGearShiftDirt;
		top   = kGearTopDirt;
		n     = 4;
	}

	gear = n + 1;
	for (i = 0; i < n; i++) {
		if ((double)speed < shift[i] * (double)c->realSpeedMax) {
			gear = i + 1;
			break;
		}
	}

	rev = (double)speed / (top[gear - 1] * (double)c->realSpeedMax);
	if (rev > 1.0)
		rev = 1.0;
	else if (rev < 0.0)
		rev = 0.0;

	if (gearOut)
		*gearOut = gear;
	return (float)rev;
}


/* ORIGIN  maybe_Gearbox_TickShift @ 0x0048e6e0 */
int gearbox_tick_shift(MR_Gearbox *g, const MR_BikeCarac *c,
                       int dtMs, int preRace,
                       const float *shiftTable, float roadSpeed)
{
	int state;
	int want;

	if (!g || !c)
		return 0;

	state = g->state;
	if (state == GB_STATE_IDLE)
		return 0;

	/* The clutch delay. Nothing moves until this runs out. */
	g->timerMs -= dtMs;
	if (g->timerMs >= 1)
		return 0;

	if (state == GB_STATE_UP) {
		g->pending = 0;
		g->state   = GB_STATE_IDLE;

		if ((double)g->gear < (double)c->nbSpeed) {
			g->gear++;
			if (!preRace) {
				g->savedEngine = 0.0f;   /* caller resyncs */
				return 1;
			}
		} else {
			g->gear = (int)c->nbSpeed;
		}
		return 0;
	}

	if (state == GB_STATE_DOWN) {
		int gear = g->gear;

		want = g->pending;
		if (want < 0)
			want = -want;           /* the original takes abs */
		g->state   = GB_STATE_IDLE;
		g->pending = 0;

		if (gear < 2) {
			g->gear = 1;
			return 0;
		}

		if (gear - want < 1)
			want = gear - 1;

		if (want > 0) {
			/*
			 * THE MULTI-GEAR SEARCH, 0x0048e6e0's down branch.
			 *
			 *     if (mode != 0 && gear - want > 1) {
			 *         p = &table[gear - want]
			 *         do {
			 *             if (*p - 2.0 <= roadSpeed) break;
			 *             p--; want++;
			 *         } while (gear - want > 1);
			 *     }
			 *
			 * So the queued drop is a FLOOR, not the answer: the
			 * gearbox keeps dropping while the candidate gear's
			 * handover speed is still more than the hysteresis
			 * ABOVE the road speed - that gear is too tall to pull.
			 * It stops at gear 1.
			 *
			 * THE TABLE IS PER RACER IN THE ORIGINAL, at
			 * racer+0x90.. - the compiler based the access on
			 * instance+0x68, the pending field, which is why a
			 * displacement search never found it. Ours is the one
			 * gearbox_build_shift_table fills; same numbers, one
			 * copy, because the port runs one player gearbox.
			 *
			 * MEASURED before this went in: full left at 40 m/s in
			 * gear 5, MOTO drops straight to 3 and ours went to 4,
			 * asked again, and reached 3 five steps later - by which
			 * point MOTO was driving out of the corner at 42.3 m/s
			 * against our 39.0. racers-convergence.txt 9.46.18.3.
			 */
			if (g->mode != 0 && shiftTable) {
				while (gear - want > 1) {
					/*
					 * THE INDEX IS THE CANDIDATE GEAR MINUS ONE - the
					 * speed at which the gear BELOW hands over to it,
					 * which is the point below which the candidate
					 * cannot pull. It is the same index the downshift
					 * REQUEST above uses, table[gear - 1], and it has
					 * to be: one convention or the search disagrees
					 * with the test that started it.
					 *
					 * INDEXING BY THE CANDIDATE ITSELF DROPS ONE GEAR
					 * TOO MANY. Measured at 39.4 m/s from gear 5 with
					 * the table 17.8/34.5/50.9/65.7/72.9: by candidate
					 * it walks 4, 3, 2 and stops at 2, and MOTO stops
					 * at 3. By candidate-1 it tests 50.9 then 34.5 and
					 * stops at 3. The original's per-racer array is
					 * simply offset one from ours.
					 */
					float t = shiftTable[gear - want - 1];

					if (t - GB_DOWN_HYST <= roadSpeed)
						break;
					want++;
				}
			}

			g->gear = gear - want;
			if (!preRace)
				return 1;
		}
		return 0;
	}

	/* Any other state is cleared. */
	g->state   = GB_STATE_IDLE;
	g->pending = 0;
	return 0;
}


/*
 * ORIGIN  NONE - ours.
 *
 * World velocity into body space: b[i] = row_i . (w - t). The inverse
 * of the rebuild below, and the same transform body_axis_speed takes
 * one component of.
 */
static void body_from_world(const float *m, const double w[3],
                            double b[3])
{
	int i;

	for (i = 0; i < 3; i++) {
		b[i] = (double)m[i * 3 + 0] * (w[0] - (double)m[9])
		       + (double)m[i * 3 + 1] * (w[1] - (double)m[10])
		       + (double)m[i * 3 + 2] * (w[2] - (double)m[11]);
	}
}


/* ORIGIN  maybe_Racer_SetLongSpeed @ 0x0048cbe0 */
void physics_set_long_speed(const float *m, float *velDir, float *speed,
                            float realSpeedMax, float v)
{
	double n[3];
	double w[3];
	double b[3];
	double len;
	int    i;

	if (!m || !velDir || !speed)
		return;

	/* The clamp is the FIRST thing, before any transform. */
	if ((double)v > (double)realSpeedMax)
		v = realSpeedMax;
	else if ((double)v < -(double)realSpeedMax)
		v = -realSpeedMax;

	/* The current world velocity: normalize(velDir) * speed. */
	for (i = 0; i < 3; i++)
		n[i] = (double)velDir[i];
	len = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
	if (len != 0.0) {
		for (i = 0; i < 3; i++)
			n[i] /= len;
	}
	for (i = 0; i < 3; i++)
		w[i] = n[i] * (double)*speed;

	body_from_world(m, w, b);

	/* Rebuild with the row-2 component REPLACED. */
	for (i = 0; i < 3; i++) {
		w[i] = b[0] * (double)m[0 + i]
		       + b[1] * (double)m[3 + i]
		       + (double)v * (double)m[6 + i]
		       + (double)m[9 + i];
	}

	len = sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
	*speed = (float)len;

	if (len <= K_VELDIR_EPS) {
		/* Zeroed, not left stale - see physics.h. */
		velDir[0] = 0.0f;
		velDir[1] = 0.0f;
		velDir[2] = 0.0f;
		return;
	}

	for (i = 0; i < 3; i++)
		velDir[i] = (float)(w[i] / len);
}


/*
 * physics_set_slip_speed - SET the LATERAL speed, keeping the other
 * two body-space components.
 *
 * ORIGIN  Racer_SetSlipSpeed @ 0x0048d2c0
 *
 * THE EXACT MIRROR OF physics_set_long_speed. The two functions are
 * the same 450 bytes with one difference: SetLongSpeed pairs the new
 * value with ROW 2 and this pairs it with ROW 0. Compare 0x0048d381
 * here against 0x0048cd0d there - same three multiply-adds, the
 * incoming scalar simply moves from the row-2 slot to the row-0 one.
 *
 *     b     = (normalize(velDir) * speed - t) . rows      world -> body
 *     world = v*row0 + b1*row1 + b2*row2 + t              body -> world
 *     speed  = |world|
 *     velDir = speed > 1e-5 ? world/speed : (0,0,0)
 *
 * SO IT COMPLETES THE FAMILY, and the family is the whole point:
 * physics_slip_speed READS the lateral component, physics_long_speed
 * READS the longitudinal one, physics_set_long_speed WRITES the
 * longitudinal one, and this WRITES the lateral one.
 *
 * IT IS THE ONLY THING IN THE GAME THAT PUTS A LATERAL COMPONENT INTO
 * velDir. maybe_Racer_ApplyCornering is its single caller, at
 * 0x0045d50c. Without it velDir can only ever hold the heading plus
 * quantisation residue, physics_slip_speed returns noise, and both
 * the skid ribbon's width rule and the MotoType 1 grip curve are
 * reading a number that never means anything. See
 * docs/subsystems/fx-particles.txt 10.4.1.
 */
void physics_set_slip_speed(const float *m, float *velDir, float *speed,
                            float realSpeedMax, float v)
{
	double n[3];
	double w[3];
	double b[3];
	double len;
	int    i;

	if (!m || !velDir || !speed)
		return;

	/* The clamp is the FIRST thing, before any transform. */
	if ((double)v > (double)realSpeedMax)
		v = realSpeedMax;
	else if ((double)v < -(double)realSpeedMax)
		v = -realSpeedMax;

	for (i = 0; i < 3; i++)
		n[i] = (double)velDir[i];
	len = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
	if (len != 0.0) {
		for (i = 0; i < 3; i++)
			n[i] /= len;
	}
	for (i = 0; i < 3; i++)
		w[i] = n[i] * (double)*speed;

	body_from_world(m, w, b);

	/* Rebuild with the ROW-0 component REPLACED. */
	for (i = 0; i < 3; i++) {
		w[i] = (double)v * (double)m[0 + i]
		       + b[1] * (double)m[3 + i]
		       + b[2] * (double)m[6 + i]
		       + (double)m[9 + i];
	}

	len = sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
	*speed = (float)len;

	if (len <= K_VELDIR_EPS) {
		velDir[0] = 0.0f;
		velDir[1] = 0.0f;
		velDir[2] = 0.0f;
		return;
	}

	for (i = 0; i < 3; i++)
		velDir[i] = (float)(w[i] / len);
}


#define K_CRASH_DT         0.01f    /* 0x004d1004                    */
#define K_CRASH_LAUNCH   128.0      /* 0x004d0ef8                    */
#define K_CRASH_GRAVITY   -0.0981   /* 0x004d11a0, = -9.81/100       */
#define K_CRASH_DRAG_AIR (-2.0/3.6) /* 0x004d11a8, exact             */
#define K_CRASH_DRAG_GND (-15.0/3.6)/* 0x004d1190, exact             */
#define K_CRASH_BOUNCE     0.01     /* 0x004d0ed8, near-inelastic -
                                     * and the same constant as
                                     * K_CORNER_DT, under a
                                     * different meaning          */
#define K_CRASH_DECAY      0.9      /* 0x004d11b8, per frame         */
#define K_CRASH_END        (1.0 / 3.6)  /* 0x004d0f70                */
#define K_CRASH_TIMER0   400.0f    /* immediate at 0x00467e6f       */
#define K_CRASH_UPBIAS     0.6     /* 0x004d1180                    */
#define K_CRASH_SPEEDMUL   0.15    /* 0x004d1188                    */
#define K_CRASH_TIMER_LO 100.0f    /* 0x42c7ffff, as an int compare */