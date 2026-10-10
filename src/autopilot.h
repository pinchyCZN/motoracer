/*
 * autopilot.h - a DEBUG DRIVER for the local racer.
 *
 * It synthesises the same button word the keyboard produces and hands
 * it to race_poll_input. Nothing downstream can tell the difference:
 * the drivetrain, the steering, the cornering and the collision all
 * run exactly as they do for a human. It is a pair of hands, not a
 * shortcut.
 *
 * WHY IT EXISTS. Reaching the end of a race costs a full four laps,
 * or one lap of practice, driven by hand - which is why the finish
 * sequence had never been worked on. With this and TAB it is under a
 * minute, and the simulation arrives at the line in the state it
 * would really be in.
 *
 *
 * IT IS NOT A SIXTH GAMEPLAY FILE - CLAUDE.md RULE 13
 *
 * The five-file rule governs GAMEPLAY LOGIC, and this simulates
 * nothing: it reads state and returns a button word. That puts it
 * with timing.c and model.c, which are not gameplay and keep their
 * own homes, and not with physics.c or racer.c.
 *
 * Do not fold it into racer.c the way racetick.c was folded back.
 * The test in rule 13's table is "its output feeds a speed or a
 * force" - this one's output feeds an INPUT, which is the opposite
 * end of the pipe. Agreed with the user before it was written.
 *
 *
 * WHY NOT racer_tick_ai, WHICH ALREADY DRIVES THE TRACK
 *
 * Because it does not drive it. maybe_Racer_TickAI PLACES a rider -
 * "no drive, no steering, no cornering, no drivetrain" (racer.h) -
 * and its line follower "never touches the player's inputs". Routing
 * the player through it would bypass the very physics this exists to
 * exercise, and would prove nothing about the finish.
 *
 * Its racing line is out of reach anyway: the target point needs the
 * per-segment offset at segment+0xb0, which MR_TrkSegment does not
 * expose. [open]
 */
#ifndef MRACER_AUTOPILOT_H
#define MRACER_AUTOPILOT_H

#include "racer.h"
#include "trk.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The flag. OFF BY DEFAULT and toggled by F8, which is the ONLY way
 * it is turned on - there is deliberately no ini key, so a stale
 * config can never start a race with the bike driving itself. Unset,
 * nothing in this file runs and race_poll_input behaves exactly as
 * it did.
 */
extern int g_autopilot;

/*
 * [DEBUG] autopilotlog=1 - print one diagnostic line in thirty
 * frames: the segment, the lateral offset, the aim terms, the
 * angle asked for and the one achieved, the curvature, the speeds
 * and whether the tyres are sliding. Rule 8 means nobody on this
 * side can watch it drive, so the numbers it made its decision from
 * have to come back out in the log.
 *
 * slip IS racer->fxGate, which is onGround && |slipAbs| >= 0.04 -
 * the game's own "these tyres are sliding" test (racer.h). It is
 * there to settle whether the cornering limit has margin left: a
 * bike holding the line at target speed WITHOUT slip has grip in
 * hand and AP_LAT_ACCEL can go up; one that slips is already at the
 * limit and the answer is no. That question cannot be settled by
 * looking at lap times.
 */
extern int g_autopilotLog;


/*
 * autopilot_input - the driving word, if the autopilot is driving.
 *
 * Returns 0 when it is not, and the caller reads the keyboard as
 * usual - the SAME CONTRACT agent_input_get already has, deliberately,
 * because race_poll_input now has two overrides in a row and they
 * should not differ in shape. Bits are the game's: 1 accelerate,
 * 2 brake, 4 left, 8 right.
 *
 * racers/racerCount are PASSED AND UNUSED. Opponent avoidance is a
 * later option and the signature should not churn when it lands; the
 * mode this exists for - practice - has nobody to avoid.
 *
 * axis1 COMES BACK AS AN ANALOG STEERING POSITION, 0x8000 centred,
 * and that is why this controller needs no damping.
 *
 * racer_apply_steering has two paths. The DIGITAL one integrates a
 * RATE - holding a button ramps the bar toward the lock stop and
 * keeps going - which is what a damping term, a hysteresis band and
 * a bar cap exist to contain. The ANALOG one sets the angle
 * DIRECTLY:
 *
 *     steering = (axis1 - 0x8000) * roueLimiteAngulaire / 32768
 *
 * so there is no ramp and no saturation dynamics to fight. Asking
 * for an ANGLE is an easier problem than bang-banging a rate.
 *
 * IT IS THE GAME'S OWN JOYSTICK PATH, not an invention.
 * maybe_Game_ReadInput centres both axes for any racer whose
 * deviceType is not 2 or 4 - that is, for a KEYBOARD player. A
 * stick player drives this branch every frame. The autopilot is not
 * a keyboard.
 *
 * o THE BUTTONS STILL CARRY THE STEER BITS, because things
 *   downstream read them - the steer-torque helper pins grip when
 *   brake and a direction are held together. The analog angle and
 *   the digital bits agree rather than competing.
 *
 * o leanMode BECOMES 1, which is the one behavioural difference:
 *   racer_lean_response stops applying its 20 ms dt floor. That is
 *   what a stick player gets too.
 *
 * THROTTLE STAYS DIGITAL. axis0's analog branch divides its
 * negative side by 2000, so it is an engine-braking curve rather
 * than a brake, and the brake lives on the button.
 */
int autopilot_input(const MR_Racer *r, const MR_Trk *t,
                    const MR_Racer *racers, int racerCount,
                    unsigned int *buttons, int *stunt, int *axis1);

/* Clears the latched state, so a new race does not inherit a lap's
 * worth of hysteresis. Call from the race reset. */
void autopilot_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* MRACER_AUTOPILOT_H */
