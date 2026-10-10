/*
 * physics.h - the drivetrain: engine, gearbox, grip and drag.
 *
 * THIS FILE IS MEASURED AGAINST THE RUNNING GAME, not just derived
 * from the decompiled arithmetic. Everything here was predicted and
 * then checked against live captures taken with hardware watchpoints
 * and trace hooks while somebody rode the bike - racers-convergence.txt 9.17,
 * 9.18 and 9.19. The agreement is not approximate:
 *
 *   the engine gain      ratio 1.000 over eight frames, twice, at a
 *                        standing start and again at 44 m/s
 *   the drag term        ratio 0.999 to 1.000 over five frames
 *   the grip curve       six decimal places on four samples
 *   the torque curve     predicts a live engine it had never been
 *                        run against
 *
 * SO A DIFFERENCE BETWEEN THIS FILE AND THE ORIGINAL IS A BUG HERE,
 * not an open question. Treat a mismatch as something to fix rather
 * than something to investigate.
 *
 * physics_drivetrain_frame IS maybe_Physics_TickDrivetrain (0x0048ef30)
 * WHOLE - all three drive paths and the tail - with its remaining
 * holes marked at its own declaration. Nothing is summarised here,
 * because a list of gaps in a file header goes stale the moment one
 * is filled and nothing points back.
 *
 *
 * UNITS, AND THE ONE THAT BITES
 *
 * Every speed in the racer subsystem is METRES PER SECOND. The
 * speedometer is speed * 3.6 truncated to an int, so RealSpeedMax 80
 * is 288 km/h and the dirt bike's 46 is 165.6. Torque in the .bpt is
 * kgf.m and engine_torque_at converts with 9.81.
 *
 * THE DRIVETRAIN'S dt IS NOT THE TICK'S dt.
 * maybe_Physics_TickDrivetrain multiplies by 10 at entry and caps at
 * 500, so it works in RAW MILLISECONDS while the rest of the tick
 * uses a 0.1-scaled unit. MISS THAT AND EVERY PREDICTION IS OUT BY
 * EXACTLY TEN. physics_drivetrain_frame takes the tick's scaled dt
 * and does the multiply itself, so callers pass what they already
 * have.
 */
#ifndef MRACER_PHYSICS_H
#define MRACER_PHYSICS_H

#include "bikes.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The drivetrain's own state. These live inside MODEL INSTANCE 0 in
 * the original, not directly in the racer, which is why the two
 * offset columns differ by 0x24 throughout - instance+N is racer+N+0x24.
 * Both are given because Ghidra shows the instance offsets (ESI is the
 * instance in maybe_Physics_TickDrivetrain) and mr_dump_racer.py shows
 * the racer ones.
 */
typedef struct {
	                          /* instance   racer                  */
	float engineSpeed;        /* +0x0c8     +0x0ec                 */
	float airTime;            /* +0x0cc     +0x0f0                 */
	int   brakeHoldMs;        /* +0x08c     +0x0b0   see below     */
	float speed;              /* +0x100     +0x124   m/s           */
	float gearboxRoadSpeed;   /* +0x108     +0x12c   m/s           */
} MR_Drivetrain;

/*
 * engine_torque_at - Engine_TorqueAt, 0x0048ee30. The whole engine
 * map in twenty lines, and it is a two-segment piecewise-linear
 * torque curve through three points.
 *
 * engineSpeed is NOT rpm; the first thing the function does is
 * convert, by regimeMax / moteurVitesseMax, which is also what pins
 * those two fields as the two ends of one scale.
 *
 * wheelieScale (racer+0xb8) MULTIPLIES the result, so a wheelie is
 * worth more torque - 1.6 while one is held. Pass 1.0f for none.
 *
 * driveInterlock IS racer+0x88 - THE GEARBOX MODE, the same
 * field MR_Gearbox.mode carries.  Engine_TorqueAt is __thiscall
 * on the instance and gates on instance+0x64, and the instance
 * is racer+0x24, so instance+0x64 is racer+0x88 [verified,
 * 0x0048ee30].
 *
 * NOT racer+0xac, WHICH IS NEARBY AND IS turnBrakeFlag - that one
 * belongs to the brake gate and has nothing to do with this
 * multiplier. See physics_brake_gated.
 *
 * The original multiplies by 1.1 when it is ZERO - and that
 * gate is HALF GAMEPLAY, half copy protection: the full
 * condition is `g_codeCheckPassed && racer+0x88 == 0`.
 *
 * THE PROTECTION HALF IS GONE FROM THIS PORT. A rebuilt executable
 * has no bytes to check and is legitimate by construction, so it
 * takes the pass branch; only the gameplay gate survives. See
 * docs/subsystems/copy-protection.txt for the mechanism and
 * racers-convergence.txt 9.20.2 for how it was found.
 *
 * SO THE 1.1x IS THE MANUAL GEARBOX'S TORQUE BONUS.  racer+0x88
 * read 1 in every live sample - thirteen captured acceleration
 * frames among them - because every one of those was taken in
 * AUTOMATIC, which is the bike page's default.
 *
 * SO AN AI NEVER GETS THE BONUS AND THE PLAYER GETS IT ONLY IN
 * MANUAL. FUN_00411010 writes the field once as the race starts -
 * 1 for everyone but the local racer - and race.c's gearbox-mode
 * note at the assignment has the three cases.
 *
 * Returns torque in N.m, floored at 0.2 so the engine never makes
 * exactly zero and a bike always creeps forward on the throttle.
 */
float engine_torque_at(const MR_BikeCarac *c, float engineSpeed,
                       float wheelieScale, int driveInterlock);

/*
 * physics_grip - the tyre-slip lerp factor.
 *
 * MotoType 1 - the dirt bikes - get a curve; everything else gets a
 * flat floor. slipAbs is the ABSOLUTE body-space lateral velocity, in
 * m/s, from maybe_Racer_SlipSpeed.
 *
 *     MotoType 1 and 0.01 < |slip| < 20    1.0 - 0.05 * |slip|
 *     MotoType 1 otherwise                 1.0
 *     everything else                      max(1.0, MinAdherenceWheel)
 *
 * THE 20.0 UPPER BOUND IS A CLIFF, NOT A CLAMP, and it is faithfully
 * reproduced here. Past it grip does not saturate at zero - it jumps
 * back to a clean 1.0, because the original stores 1.0 first and only
 * overwrites it inside the band. So the bike grips hardest exactly
 * when it is most sideways. That reads as backwards physics and as a
 * perfectly ordinary guard against a negative lerp factor, which
 * would extrapolate and diverge. It has never been observed live - a
 * donut only reached |slip| 3.05 - so it is transcribed from the code
 * and flagged, not tuned. racers.txt 9.19.3.
 */
float physics_grip(const MR_BikeCarac *c, float slipAbs);

/*
 * physics_drag - the speed lost to drag in one frame, always positive.
 *
 *     speed^2 * dtMs * K / motoMasse * 0.001
 *
 * where K is 0.3 on the ground and 1.6 IN THE AIR - leaving the ground
 * multiplies drag by 5.33. THE AIRBORNE BRANCH FIRES ON EVERY JUMP
 * and is simply never reached on a tarmac track, which is what makes
 * it look unreachable. racers.txt 9.18.5.
 *
 * dtMs is the drivetrain's millisecond dt, not the tick's.
 */
float physics_drag(const MR_BikeCarac *c, float speed, float dtMs,
                   int airborne);

/*
 * physics_slip_speed - the bike's LATERAL velocity, m/s.
 *
 * maybe_Racer_SlipSpeed, 0x0048d0f0. This is the third and last
 * value physics_tick_drivetrain needed from outside; pass its
 * absolute value as slipAbs.
 *
 *     v      = normalize(velDir) * speed
 *     result = row0 . v  -  row0 . t
 *
 * where row0 is matOrientation's first row - the bike's LATERAL axis,
 * the one whose Y is exactly zero because
 * maybe_Racer_UpdateGroundContact builds it by crossing the wheel
 * contact vector with world up. So the result is how fast the bike is
 * travelling SIDEWAYS, and the grip curve reads its magnitude.
 *
 * m is the 12-float matOrientation (instance+0x98, racer+0xbc): three
 * rows of three, then the translation. velDir is instance+0x10c.
 *
 * THE ORIGINAL BUILDS A WHOLE 3x4 INVERSE AND READS A QUARTER OF IT -
 * a generic matrix-inverse idiom reused, with eight of the twelve
 * terms dead. This transcribes the four live ones rather than the
 * machinery; the arithmetic is identical. The plate comment on
 * 0x0048d0f0 has the instruction ranges.
 *
 * THE TRANSLATION TERM IS ALWAYS ZERO IN PRACTICE. matOrientation's
 * translation row read 0,0,0 live [verified], which is what makes the
 * expression a plain dot product with row0. It is kept because the
 * code computes it and a non-zero translation would change the
 * answer - but note it would also mean transforming a VELOCITY as
 * though it were a POINT, which is only harmless while t is zero.
 *
 * DO NOT TRANSCRIBE THIS ONE FROM THE DECOMPILER. Ghidra renders the
 * return using three locals it never shows assigned - Matrix34_Copy
 * writes them through a pointer - so the decompile looks plausible
 * and is not usable. Still true as of a re-check; it is a pointer
 * write, not a missing prototype, so rule 15.1 does not apply.
 * racers.txt 9.24.
 */
float physics_slip_speed(const float *m, const float *velDir,
                         float speed);

/*
 * physics_long_speed - the bike's LONGITUDINAL velocity, m/s.
 *
 * maybe_Racer_LongSpeed (0x0048ca10), which was FUN_0048ca10. THE
 * SAME FUNCTION BODY AS physics_slip_speed WITH A DIFFERENT MATRIX
 * ROW: SlipSpeed dots row 0, the lateral axis; this dots row 2, the
 * forward one. The two openings are byte-for-byte identical and the
 * only difference is which four slots of the built inverse get
 * multiplied out at the end.
 *
 * So the original ships two near-identical 450-byte functions. This
 * keeps one implementation and selects the row, which is a
 * simplification of FORM only - the arithmetic is unchanged.
 *
 * It is the READ side of maybe_Racer_SetLongSpeed, and it is what
 * maybe_Racer_ApplyCornering squares for v*v/r and what
 * maybe_Racer_UpdateForkDive differences to get an acceleration.
 */
float physics_long_speed(const float *m, const float *velDir,
                         float speed);

/*
 * physics_set_long_speed - SET the longitudinal speed, keeping the
 * other two body-space components.
 *
 * maybe_Racer_SetLongSpeed (0x0048cbe0), and it completes the family:
 * physics_slip_speed READS the lateral component, physics_long_speed
 * READS the longitudinal one, and this WRITES the longitudinal one.
 * Seven callers in the original, including the drivetrain, the
 * gearbox and the crash handler.
 *
 *     v = clamp(v, -realSpeedMax, +realSpeedMax)
 *     b = (normalize(velDir) * speed - t) . rows      world -> body
 *     world = b0*row0 + b1*row1 + v*row2 + t          body -> world
 *     speed  = |world|
 *     velDir = speed > 1e-5 ? world/speed : (0,0,0)
 *
 * IT REPLACES ONE COMPONENT AND KEEPS TWO. b0 and b1 survive from the
 * current velocity; only the one pairing with row 2 is overwritten.
 * That is what makes it a "set the forward speed" primitive rather
 * than a velocity setter, and it is why a bike can be given a new
 * road speed without losing its slide.
 *
 * SPEED IS DERIVED, NOT STORED INDEPENDENTLY - it is recomputed as
 * the magnitude of the rebuilt world velocity on every call. Any
 * reconstruction keeping speed and velDir as separate state has to
 * re-derive it here or the two will drift apart.
 *
 * BELOW 1e-5 THE DIRECTION IS ZEROED rather than left stale, so a
 * stopped bike has velDir == (0,0,0) and every normalize() downstream
 * is guarded against it. The constant is a DOUBLE at 0x004d1c08;
 * reading it as a float gives -1.4e-33 and a test that never fires.
 *
 * m is the 12-float matOrientation. velDir and speed are updated in
 * place.
 */
void physics_set_long_speed(const float *m, float *velDir, float *speed,
                            float realSpeedMax, float v);

/*
 * physics_set_slip_speed - SET the LATERAL speed, keeping the other
 * two body-space components. Racer_SetSlipSpeed (0x0048d2c0).
 *
 * The exact mirror of physics_set_long_speed - the same function with
 * the new value paired against ROW 0 instead of ROW 2 - and it
 * completes the family: slip_speed and long_speed READ the two
 * components, set_long_speed and this WRITE them.
 *
 * ONE CALLER, racer_apply_cornering at 0x0045d50c, and it is the ONLY
 * thing in the game that puts a lateral component into velDir.
 */
void physics_set_slip_speed(const float *m, float *velDir, float *speed,
                            float realSpeedMax, float v);

/*
 * racer_update_wheelie - the wheelie, which is the game's BOOST
 * MECHANIC rather than decoration. maybe_Racer_UpdateWheelie,
 * 0x004660f0.
 *
 * Four functions found in four separate reads describe one mechanic:
 * this ramps the angle and sets the scale, racer_apply_drive forces
 * the throttle bit on while the angle is up, racer_apply_steering
 * multiplies steering authority DOWN by it, and engine_torque_at
 * multiplies torque UP by it. Pull a wheelie for more drive at the
 * cost of steering, for at most about two seconds.
 *
 *   requested && timer < 200:
 *       angle += dtScaled * 0.03*PI/2, clamped to PI/2
 *       timer += dtScaled
 *   otherwise, if angle > 0:
 *       angle += dtScaled * -0.016*PI/2
 *       if it crossed zero: springCompression = -32, angle = 0
 *
 *   scale = angle != 0 ? 2.0 : 1.0
 *   and when the scale falls back to 1.0 with nothing requested,
 *   the timer is zeroed so the next wheelie starts fresh.
 *
 * THE SCALE IS 2.0 - A STRAIGHT DOUBLING. The original picks between
 * 2.0 and 1.2 on g_codeCheckPassed; a rebuilt executable is
 * legitimate by construction and takes 2.0, so the branch is gone
 * and only the value remains.
 *
 * THE LOSING BRANCH IS 1.2, NOT 1.6 - 0x004661e8 holds 0x3f99999a.
 * The penalty is described in copy-protection.txt.
 *
 * ITS dt IS THE TICK'S SCALED dt (3.3 at 33 ms), NOT the
 * drivetrain's millisecond dt. The timer cap of 200 is in those
 * units, which is why it works out to 2.00 seconds. Two functions in
 * the same subsystem using different time units is exactly the kind
 * of thing that silently ruins a port.
 *
 * THE CAP GATES THE RAMP, NOT THE ANGLE. Once the timer reaches 200
 * the ramp stops and the decay runs even with the button still held.
 *
 * THE REQUEST FLAG IS stuntOrOverspeed (+0x2e85), NOT stuntHeld.
 * The button sets stuntHeld (+0x31f9); maybe_Racer_Tick copies it
 * across at 0x0045d6db, but only for a human in solo and only once
 * the countdown is over. Reading stuntHeld here instead would
 * wheelie through the countdown and ignore ApplyDrive's clear.
 *
 * IT TAKES THE RACER, not a state of its own, and ALL FOUR VALUES
 * BELONG TO IT. Mirror one of them into a side struct and the other
 * three go invisible: every consumer reads the racer field, so the
 * timer never reaches its cap and the boost never reaches the engine
 * while the angle alone looks right. See src/README.txt.
 */
struct MR_Racer;
void racer_update_wheelie(struct MR_Racer *r, float dtScaled);
/*
 * physics_air_time - the air timer, racer+0x0f0.
 *
 * maybe_Racer_UpdateAirborne, 0x00466660. Accumulates a HARDCODED
 * 2.592 per frame while off the ground and resets to zero the instant
 * the bike lands. Feeds two things in this file: the drag coefficient
 * (1.6 airborne against 0.3) and the slope term (zero airborne).
 *
 * 2.592 IS PER FRAME AND NOT SCALED BY dt, and it equals 3.3 * PI/4
 * to four places - tuned for a fixed 33 ms frame. That is a real
 * framerate dependency and it is reproduced rather than fixed, per
 * CLAUDE.md rule 5: at double the framerate a bike ages through the
 * air twice as fast. Contrast the brake hold counter, which
 * accumulates real milliseconds. racers.txt 9.18.4.
 *
 * Verified to the bit over eight airborne frames.
 */
float physics_air_time(float airTime, int onGround);

/*
 * physics_slope - gravity resolved along the bike's forward vector,
 * as a speed increment for this frame.
 *
 *     if (airborne)  return 0
 *     t = -dirY * dtMs * 0.004905
 *     if (t > 0.55)  t = 0.55
 *
 * DOWNHILL ACCELERATES, UPHILL RETARDS. dirY is the Y component of
 * the bike's forward direction - matOrientation row3 + row2,
 * normalised - so it is negative pointing downhill, and the leading
 * minus turns that into a positive gain. 0.004905 is 9.81/2000.
 *
 * [verified] against the running game to 1.00000 on eight consecutive
 * frames covering two slopes (dirY -0.0257 and -0.2632) and five
 * frame times (32, 33, 34, 129 and 200 ms). racers.txt 9.22.
 *
 * GRAVITY IS SWITCHED OFF IN THE AIR. The original tests airTime at
 * 0x0048fdbf and leaves the term at zero when the bike is off the
 * ground, so a jump is pure ballistics as far as the drivetrain is
 * concerned. Reproduce that; it is not an oversight.
 *
 * THE 0.55 CLAMP IS A LONG-FRAME GUARD, NOT A SLOPE LIMIT, and the
 * arithmetic says so: reaching it at 33 ms needs |dirY| > 3.4, which
 * is impossible for a component of a normalised vector. It becomes
 * reachable only as the frame stretches - at the 500 ms dt cap a
 * dirY of -0.225 is enough, and -0.263 was observed on an ordinary
 * hill. So it exists for the same reason the dt cap does: to stop one
 * long frame delivering an enormous impulse. A port that drops it
 * will be fine until it stutters on a slope. [inferred - the
 * threshold arithmetic is exact, the intent is a reading]
 *
 * THE CLAMP IS ONE-SIDED. Uphill values pass through unbounded,
 * because the comparison is only against the upper limit.
 */
float physics_slope(float dirY, float dtMs, int airborne);

/*
 * physics_brake_force - the braking force for this frame, in the same
 * km/h-ish units Frein is stored in.
 *
 *     force = clamp(Frein, 10.0, 140.0)
 *     if (brakeHoldMs < 200 && rampOverride == 0)  force *= 1/6
 *
 * THERE IS A 200 MILLISECOND RAMP-IN AT ONE SIXTH STRENGTH, and it is
 * the most surprising thing in the drivetrain. A brake that has just
 * been applied is SIX TIMES WEAKER than one that has been held for a
 * fifth of a second. Watched live it is unmistakable: the force sat
 * at 13.3333 - Frein 80 over six - for six frames and switched to a
 * clean 80.0 on the seventh, the frame after brakeHoldMs crossed 200.
 * racers.txt 9.21.
 *
 * rampOverride is [ESP+0x7c] in the original, computed at 0x0048efab
 * from a field two pointers deep off instance+0xe4. It read 0 for
 * every captured frame so the ramp was never skipped that way, and
 * what it means is unread. Pass 0. [open]
 */
float physics_brake_force(const MR_BikeCarac *c, int brakeHoldMs,
                          int rampOverride);

/*
 * physics_brake_step - how much road speed one braking frame removes.
 *
 *     physics_brake_force(...) * (1/3.6) * dtMs * 0.001
 *
 * THE 1/3.6 IS THE km/h TO m/s CONVERSION, the same constant the
 * speedometer uses in the other direction. So Frein is authored in
 * km/h like the speeds the player sees, and the .bpt value 80 means
 * 80 km/h of speed shed per second at full strength.
 */
float physics_brake_step(const MR_BikeCarac *c, int brakeHoldMs,
                         float dtMs, int rampOverride);

/*
 * physics_apply_brake - integrate one brake step into road speed.
 *
 * Pulls gearboxRoadSpeed TOWARD ZERO FROM EITHER SIGN and clamps to
 * exactly zero on overshoot, so braking in reverse works and nothing
 * oscillates about zero. The original does the sign test on the
 * float's bit pattern as a signed integer, which is just how the
 * compiler emitted it.
 *
 * Also advances brakeHoldMs by dtMs. THE CALLER MUST ZERO IT WHENEVER
 * THE BRAKE IS NOT APPLIED - the original does that at 0x0048f930, on
 * the path that skips this block entirely, and getting it wrong gives
 * a bike that brakes at full strength from the first frame.
 */
void physics_apply_brake(MR_Drivetrain *d, float step, float dtMs);

/*
 * physics_brake_gated - whether the deceleration block runs at all.
 *
 * AND THE PLAYER ON A DIRT BIKE CANNOT BRAKE IN A CORNER. The
 * original reaches the block only when one of three things is true
 * (0x0048f915 onward): turnBrakeFlag is set, rampOverride is set, or
 * the pre-race flag is set. turnBrakeFlag lives at racer+0xac and is
 * CLEARED while braking and steering at once, which skips
 * deceleration altogether.
 *
 * BUT IT IS SET TO 1 UNCONDITIONALLY, every racer every frame, at
 * 0x00466504 - before the two guards that can clear it. Only a HUMAN
 * (0x0046651d) on a MotoType 1 bike (0x00466532) reaches the clearing
 * code. So a street bike and every AI rider always have it set and
 * always brake, and the handicap belongs to the player on a dirt bike
 * alone:
 *
 *     human + MotoType 1     cannot brake mid-corner
 *     human + MotoType 2     brakes normally
 *     AI     + either        brakes normally
 *
 * A PORT MUST SET IT TO 1 AT THE TOP OF THE STEERING UPDATE, not
 * merely leave it alone - it is rebuilt each frame, not latched.
 *
 * NOTE THE REST OF THE BRAKE MODEL IS BIKE-INDEPENDENT. The clamps,
 * the 200 ms threshold, the 1/6 and the 1/3.6 are all hardcoded and
 * there is no MotoType branch anywhere inside the block. Frein is the
 * only thing a bike brings to braking.
 *
 * AND THE BLOCK RUNS EVERY FRAME OF THE GRID COUNTDOWN, with no
 * input at all, because the pre-race flag is one of the three things
 * that opens it.
 *
 * braking is the sign of the throttle demand: negative brakes.
 */
int physics_brake_gated(int braking, int turnBrakeFlag, int rampOverride,
                        int preRace);

/* ------------------------------------------------------------------ */
/*
 * CORNERING - the LATERAL force balance, as distinct from the
 * drivetrain's longitudinal one. maybe_Racer_ApplyCornering
 * (0x0045d110).
 *
 * THERE ARE TWO GRAVITY TERMS IN THIS SUBSYSTEM AND THEY ARE NOT THE
 * SAME TERM. physics_slope is the LONGITUDINAL gradient, taken from
 * the bike's forward vector and fed to the drivetrain.
 * physics_camber below is the LATERAL tilt, taken by sampling the
 * GROUND HEIGHT at two points a unit apart across the bike, and fed
 * to the cornering step. Both are scaled by 9.81, from two different
 * .rdata constants. Merging them would be an easy and wrong
 * simplification.
 *
 * THE WHOLE FUNCTION IS SKIPPED AFTER ONE FRAME IN THE AIR:
 *
 *     if (airTime > 2.592) { maybe_Racer_SlipSpeed(); return; }
 *
 * and 2.592 IS THE AIR TIMER'S PER-FRAME STEP (physics_air_time). So
 * the test is not a tuned threshold at all - it reads "have I been
 * airborne for MORE THAN ONE FRAME". Cornering runs on the first
 * airborne frame and on none after it. [verified - the step and the
 * threshold are the same .rdata double, 0x004d0f20]
 *
 * That early-out also calls maybe_Racer_SlipSpeed and THROWS THE
 * RESULT AWAY. Since that helper never writes its receiver (9.24),
 * this is dead code - the second instance of the same pattern after
 * maybe_Snd_TickRacer's (9.19.4).
 *
 * THE COPY-PROTECTION CHECK IN THE MIDDLE OF THIS FUNCTION IS NOT
 * TRANSCRIBED, and deliberately. It reads the game's own bytes at
 * 0x00442761, which a rebuilt executable does not have, and its
 * payload zeroes realSpeedMax and wheelieScale - two independent
 * kills that leave the bike unable to move. Per CLAUDE.md, what a
 * port must NOT do is reproduce the PENALTY with the flag left at
 * zero. See copy-protection.txt 5.1-5.2 and racers.txt 9.11.3.
 */

/*
 * physics_camber - the lateral gravity term.
 *
 *     dy / sqrt(dy*dy + 1.0) * 9.81
 *
 * where dy is the GROUND HEIGHT DIFFERENCE between the bike's
 * position and a point one unit along matOrientation row 0 - the
 * lateral axis. The dy/sqrt(dy^2+1) form is a smooth, bounded sign
 * function: it saturates at +-1 instead of running away on a cliff
 * edge, which a raw gradient would.
 *
 * The caller samples both heights with Trk_GroundHeightAt and passes
 * the difference; when the track has no segments the original uses
 * the raw y instead, which this cannot know about and the caller must
 * handle.
 */
float physics_camber(float groundDeltaY);

/*
 * physics_centripetal - v*v/r, MotoType 1 only.
 *
 * radius comes from a three-point circle fit through positionPrev2,
 * positionPrev and position, SCALED BY 1/128 (0x004d0ee0), and is
 * FLT_MAX when the bike is going straight - which is what identifies
 * the field as a turn radius at all.
 *
 * driveDisengaged DOUBLES IT (0x004d0e88 = 2.0). So a MotoType 1 bike
 * coasting with no brake corners twice as hard, which is the third
 * thing that flag does after 9.18.7 found it toggling.
 *
 * Returns 0 for a non-MotoType-1 bike; the caller still adds the
 * camber.
 */
float physics_centripetal(float speed, float radius, int motoType,
                          int driveDisengaged);

/*
 * physics_cornering_step - combine, integrate, and pull toward zero.
 *
 *     out = (camber + centripetal) * dt * 0.01 + slipSpeed
 *     out is then moved TOWARD ZERO by friction * 9.81 * dt * 0.01,
 *     snapping to exactly zero at the crossing
 *
 * THE TOWARD-ZERO-WITH-SNAP IDIOM IS THE THIRD IN THIS SUBSYSTEM,
 * after the brake (9.21.1) and the fork dive (9.26.3). All three
 * test the float's sign bit and all three snap rather than
 * overshoot; recognising it is worth more than any one of them.
 *
 * dt is the tick's SCALED step. The 0.01 factor is the same .rdata
 * double as the fork-dive decay (0x004d0ed8), used here as a dt
 * scale rather than as a per-frame step.
 *
 * THE RESULT IS THE NEW LATERAL SPEED. It goes to Racer_SetSlipSpeed
 * (0x0048d2c0) - physics_set_slip_speed above - which is the only
 * thing in the game that puts a lateral component into velDir. So
 * the loop closes: cornering writes the sideways velocity,
 * physics_slip_speed reads it back, and the grip curve consumes its
 * magnitude on the next frame.
 */
float physics_cornering_step(float camber, float centripetal,
                             float slipSpeed, float friction,
                             float dtScaled);


/* ------------------------------------------------------------------ */
/*
 * THE GEARBOX, the speed side only.
 *
 * maybe_Gearbox_EngineToSpeed (0x0048dd90) converts engine speed to
 * road speed and maybe_Gearbox_SpeedToEngine goes the other way. The
 * ratio for a gear is rapportDeBoite[gear-1]; the original indexes it
 * as MinAdherenceDerap[gear-13], which lands on the same floats because
 * MinAdherenceDerap is carac field 42 and the ratios are 30..35.
 *
 * physics_gear_for_speed BELOW IS NOT THE SHIFT LOGIC. It answers the
 * derived question "which gear can carry this road speed", which
 * follows from the ratios alone, so a caller using it gets instant,
 * hysteresis-free shifts. THE REAL ONE IS gearbox_tick_shift, which
 * is maybe_Gearbox_TickShift (0x0048e6e0) with its timer, its state
 * and its hysteresis - declared at the end of this file.
 *
 * EngineToSpeed ALSO CARRIES A PROTECTION PENALTY - it subtracts a
 * constant from the speed cap when g_windowTokenOk is clear. Not
 * transcribed, per the same rule as everywhere else.
 */

/* The top road speed a gear can carry: rapportDeBoite[gear-1] *
 * vitesseMaxBoite. Returns 0 for an out-of-range gear. */
float physics_gear_top_speed(const MR_BikeCarac *c, int gear);

/*
 * The lowest gear whose top speed covers roadSpeed, clamped to
 * [1, nbSpeed]. DERIVED FROM THE RATIOS, not from the shift logic -
 * see above.
 */
int physics_gear_for_speed(const MR_BikeCarac *c, float roadSpeed);

/* Gearbox_SyncRoadSpeed 0x0048edd0 - road speed follows the body
 * speed, clamped to realSpeedMax. See physics.c. */
void physics_sync_road_speed(float *roadSpeed, float speed,
                             float realSpeedMax);

/*
 * maybe_Gearbox_SpeedToEngine's arithmetic:
 *
 *     engine = moteurVitesseMax / (ratio * vitesseMaxBoite) * speed
 *
 * saturating at moteurVitesseMax, which is the redline.
 *
 * ROADSPEED IS IN OUT. This IS the rev limiter: when the engine
 * would pass the red line it is pinned and *roadSpeed is written
 * back down to what the gear can carry. Pass a COPY if you only
 * want the engine figure - see MR_Racer.txt 9.2.
 */
float physics_speed_to_engine(const MR_BikeCarac *c, int gear,
                              float *roadSpeed, float engine,
                              int disengaged);

/*
 * physics_engine_to_speed - maybe_Gearbox_EngineToSpeed (0x0048dd90),
 * the INVERSE of physics_speed_to_engine. engine is by address: the
 * realSpeedMax clamp writes it back so the two cannot disagree.
 *
 * The original protection subtraction is deliberately not reproduced;
 * see the definition.
 */
float physics_engine_to_speed(const MR_BikeCarac *c,
                              float realSpeedMax, int gear,
                              float *engine, float speed);

/*
 * physics_drivetrain_frame - maybe_Physics_TickDrivetrain (0x0048ef30)
 * WHOLE, as a skeleton with its holes marked. One frame of the
 * longitudinal model: engine, gearbox, drag.
 *
 * CALL IT ON EVERY PATH, coast included. num/den is the signed drive
 * fraction the original passes: accelerate (1,1), brake (-1,1), COAST
 * (0,1), analog (0x8000-axis, 0x8000). Coast is not a skip - the
 * engine work lives in here, and branching around it is what left our
 * engine at zero off-throttle.
 *
 * IT NOW CARRIES ITS OWN TAIL. The drag, the roll, the slope, the two
 * Racer_SetLongSpeed calls, the road-speed lerp and the closing clamp
 * and engine resync are all inside it - a static second half it calls
 * at its foot. race.c makes ONE call, the way maybe_Racer_ApplyDrive
 * makes one call to maybe_Physics_TickDrivetrain, so the crash gate
 * covers the whole of it and nothing can run half.
 *
 * dirY IS THE DRIVE DEMAND'S FORWARD Y, what the slope term reads, and
 * flAirTime IS THE RACER'S, not the drivetrain mirror's - the tail's
 * airborne test is against 2.592 where the head's is a plain non-zero.
 *
 * THE DRIVE BRANCH AND THE TAIL ARE STATIC IN physics.c, so the
 * drivetrain has ONE public entry, as the original does. The terms
 * still declared publicly below - physics_drag, physics_slope,
 * physics_rolling_resistance - ARE NOT RIVALS TO IT: they are pieces
 * it calls, exposed because tools/check_physics.py holds each one to
 * a captured number. That harness is a Python MIRROR and executes
 * nothing from build/, so linkage does not reach it.
 *
 * See the definition and docs/whole-function-import.txt 7.11.
 */
void physics_drivetrain_frame(MR_Drivetrain *d, const MR_BikeCarac *c,
                              float realSpeedMax,
                              int gear, int num, int den, float dtScaled,
                              float wheelieScale, int driveInterlock,
                              int preRace, int disengaged,
                              const float *mat, float *velDir,
                              float dirY, float flAirTime,
                              int turnBrakeFlag);

/*
 * physics_rolling_resistance - the SECOND resistance, and it is
 * not drag.
 *
 * maybe_Physics_TickDrivetrain applies this to the longitudinal
 * speed in its tail, every frame, on top of the v-squared drag:
 *
 *     r = 0.02                  FUN_00459f90 @ 0x00459f90
 *     if (MotoType == 1) r *= 0.5
 *     d = r * dtMs * 0.00981    9.81/1000, so a = Crr * g
 *     if (speed < 20.0) d *= speed * 0.05
 *
 * then subtracted from the speed TOWARD ZERO, sign preserved.
 *
 * a = 0.02 * 9.81 = 0.1962 m/s2, ramped in linearly below 20 m/s
 * - an ordinary rolling-resistance coefficient times gravity.
 *
 * FUN_00459f90 TAKES THE POSITION AND IGNORES IT, returning a
 * constant 0.02. The three arguments are evaluated and discarded,
 * so this reads like a terrain-friction lookup that was stubbed
 * out - there is no per-surface rolling resistance in the shipped
 * game.  [verified]
 *
 * WITHOUT THIS the port sheds 0.0114 m/s per frame coasting at
 * 17.87 where the original sheds 0.017, and 0.0114 is exactly the
 * v-squared drag alone. 0.0113 + 0.0058 = 0.0171.
 */
float physics_rolling_resistance(const MR_BikeCarac *c, float speed,
                                 float dtMs);


/*
 * physics_engine_declutch - the engine while the clutch is out.
 *
 * maybe_Physics_TickDrivetrain @ 0x0048ef30 gates its ENTIRE drive
 * branch on
 *
 *     bVar3 = preRace || shiftState != 0 || driveDisengaged
 *
 * and when it is set the engine only decays - road speed is never
 * recomputed from it, so the bike coasts on drag for the whole
 * clutch delay.
 *
 *     target <  engine:  engine *= 1.0 + dtMs * -0.0006
 *     target >= engine:  engine += moteurVitesseMax * dtMs * 0.0006
 *     clamped to [0, moteurVitesseMax]
 *
 * SO A HELD THROTTLE FREE-REVS THE ENGINE TO THE RED LINE while the
 * bike coasts, which is the plateau before an upshift.
 *
 * MEASURED, not inferred. A hardware watchpoint on racer+0x12c
 * sees four writes a frame under drive - 0x0048f473 the increment,
 * 0x0048f50d the resync, then two lerps in the tail - and only the
 * two tail lerps once a shift is pending. See racers.txt 9.59.
 */
void physics_engine_declutch(MR_Drivetrain *d, const MR_BikeCarac *c,
                             float dtMs, float throttleTarget);


#define GB_SHIFT_TABLE_MAX 8

/* DAT_004d1d40, m/s. The downshift asks for one gear lower only
 * once road speed has fallen this far BELOW the threshold it came
 * up through, which is what stops it hunting at the boundary. A
 * double in the original. */
#define GB_DOWN_HYST       2.0f

/*
 * gearbox_build_shift_table - Gearbox_BuildShiftTable @ 0x0048e3a0.
 *
 * Fills table[1..nbSpeed-1] with the ROAD SPEED at which each gear
 * hands over. table[0] is unused. Call once per bike.
 *
 * THE CRITERION IS A POWER CROSSOVER, not a red line. Road speed is
 * swept in 0.1 m/s steps and a gear ends where the NEXT one starts
 * making more power - torque * engineSpeed being power. Nothing in
 * it is tuned.
 *
 * DO NOT REPLACE THIS WITH moteurVitesseMax * zoneRouge. That is
 * what the port did, it shifts early, and combined with an instant
 * gear change it ran about five per cent fast from the first shift
 * onward - see racers.txt 9.59.
 *
 * The secondary arm - engine past 0.97 of maximum, gears 1..3 only
 * - is the original's, immediate 0x004d1d38. All three constants
 * there are DOUBLES; read as float they are nonsense.
 */
void gearbox_build_shift_table(const MR_BikeCarac *c,
                               float table[GB_SHIFT_TABLE_MAX]);


/* ------------------------------------------------------------------ */
/*
 * THE GEARBOX. A shift is REQUESTED, waits out a timer, and only then
 * moves the gear - so a change is never instant and the delay is the
 * clutch.
 *
 * maybe_Gearbox_RequestUpshift   0x0048e0a0
 * maybe_Gearbox_RequestDownshift 0x0048e120
 * maybe_Gearbox_TickShift        0x0048e6e0
 *
 * The requesters are driven by input bits 0x10 and 0x20 from
 * maybe_Racer_UpdateAirborne, which is also what calls the tick - so
 * the whole gearbox hangs off that one function.
 *
 * mode IS racer+0x88, AND THIS IS WHAT IT IS FOR. IT IS NOT THE
 * SHIFT-IN-PROGRESS FLAG - that is state, the field above it in the
 * struct and four bytes below it in the racer. mode selects the
 * whole gearbox character:
 *
 *     mode 0   upshift 150 ms, downshift 200 ms, torque x1.1,
 *              NO multi-gear downshift
 *     mode 1   upshift 200 ms, downshift 300 ms, torque x1.0,
 *              multi-gear downshift ENABLED
 *
 * WHICH READS AS MANUAL AGAINST AUTOMATIC. The assisted mode shifts
 * more slowly and makes less torque, and in exchange it will drop
 * several gears at once for you. Our copy reads 1 - automatic - which
 * is why every captured frame showed the 1.1x not applying.
 * [inferred: the four behaviours are read, the name is not]
 */
typedef struct {
	                    /* instance   racer                       */
	int gear;           /* +0x58      +0x7c   1..nbSpeed          */
	int timerMs;        /* +0x5c      +0x80   counts DOWN         */
	int state;          /* +0x60      +0x84   0 idle, 1 up, 2 down */
	int mode;           /* +0x64      +0x88   see above           */
	int pending;        /* +0x68      +0x8c   SIGNED: + up, - down */
	float savedEngine;  /* +0x6c      +0x90   kept across a change */
} MR_Gearbox;

/*
 * Ask for one more gear up. Ignored while any shift is in flight, and
 * clamps to nbSpeed if already at the top.
 *
 * NOTE THE CLAMP WRITES THE GEAR DIRECTLY - at the top gear this is
 * not a no-op, it re-asserts gear = (int)nbSpeed. That matters because
 * the copy protection subtracts 1.0 from nbSpeed on a failed check, so
 * a cracked copy is silently forced DOWN a gear here.
 */
void gearbox_request_upshift(MR_Gearbox *g, const MR_BikeCarac *c);

/*
 * Ask for one more gear down. Accepted while idle OR while another
 * downshift is already pending, which is how several gears queue up -
 * pending goes more negative each time.
 */
void gearbox_request_downshift(MR_Gearbox *g);

/*
 * gearbox_auto_select - maybe_Gearbox_AutoSelect (0x0048e180), whole.
 *
 * The gear from ROAD SPEED, and the rev fraction 0..1 the tachometer
 * is built from. Six gears on a road bike, FIVE on a dirt one. The
 * band FRACTIONS are here, not in the carac; the carac supplies only
 * realSpeedMax, which scales them. See physics.c.
 */
float gearbox_auto_select(const MR_BikeCarac *c, float speed,
                          int *gearOut);

/*
 * gearbox_tick_shift - run the timer and apply the change.
 *
 * Returns non-zero on the frame the gear actually moves, so a caller
 * can resync the two speeds. dtMs is milliseconds.
 *
 * THE SPEED RESYNC IS THE CALLER'S JOB and is not done here. On a
 * real change the original recomputes engineSpeed and roadSpeed
 * through the new ratio, one from the other, and on an upshift it
 * also lerps the longitudinal speed by physics_grip. This returns
 * the fact of the change and leaves both to the caller.
 *
 * THE UPSHIFT LERP IS A PLUMBING GAP, NOT A TRANSCRIPTION ONE.
 * physics_set_long_speed, velDir and physics_grip all exist;
 * gearbox_tick_shift has no matrix, no velocity and no drivetrain to
 * reach them with. The original's sits at the foot of its upshift
 * arm, GROUNDED ONLY - it skips the whole block when racer+0xcc is
 * non-zero - and is the same lerp-then-SetLongSpeed idiom as
 * 0x0048f883 in the drivetrain. [open - pass the drivetrain in]
 *
 * THE MULTI-GEAR DOWNSHIFT IS IN, and it is why shiftTable and
 * roadSpeed are arguments. In mode 1 the queued drop is a FLOOR: the
 * gearbox keeps dropping while the candidate gear's handover speed is
 * still more than GB_DOWN_HYST above the road speed. Pass the table
 * gearbox_build_shift_table filled and the drivetrain's current road
 * speed; a NULL table disables the search, not the shift.
 *
 * THE ROUND TRIP IS IN, at race.c's call site rather than here:
 * SpeedToEngine then EngineToSpeed, so the two speeds leave a change
 * agreeing through the new ratio. BOTH ARMS DO IT in the original,
 * up and down, and both skip it while g_bPreRace is set. race.c
 * carries the note on where it belongs.
 */
int gearbox_tick_shift(MR_Gearbox *g, const MR_BikeCarac *c,
                       int dtMs, int preRace,
                       const float *shiftTable, float roadSpeed);


/* ------------------------------------------------------------------ */
#ifdef __cplusplus
}
#endif

#endif /* MRACER_PHYSICS_H */
