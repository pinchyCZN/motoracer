"""check_physics.py - verify src/physics.c against the live captures.

Run:  python tools/check_physics.py     exit 1 if anything drifted

Mirrors the arithmetic of src/physics.c value for value and checks it
against the numbers captured off the running game and recorded in
docs/subsystems/racers-convergence.txt 9.17, 9.18 and 9.19.  IT EXECUTES NOTHING
FROM build/ - CLAUDE.md rule 9 - and it is not a unit test of the C.
It is a check that the C and the measurements still say the same thing.

THIS CAUGHT A REAL BUG ON ITS FIRST RUN.  Every engine-gain prediction
came out 1.1000x too high, which is the copy-protection torque
multiplier: physics.h had told callers to pass torqueBonus = 1 on the
reasoning that a legitimate copy always earns it.  The live game says
otherwise - g_codeCheckPassed is 1 but racer+0x88 is also 1, and the
multiply is gated on that being ZERO - so the bonus never applies and
a port passing 1 would accelerate ten per cent too hard.

KEEP IT IN STEP WITH src/physics.c BY HAND.  A second implementation
that silently drifts from the first is worse than none, because it
goes on reporting ok.  If you change a constant or an order of
operations there, change it here in the same edit.

THE AIRBORNE GATE IS OUTSIDE WHAT THIS CHECKS, and it is not where an
earlier version of this note said.  The drive branch has NO airborne
flag: physics_drivetrain_frame sends an airborne bike to the free-rev
arm one level up and never reaches it.  Every capture behind this
harness is a GROUNDED frame either way, so the mirror below is
unaffected.  If a capture off a jump is ever added, the free-rev and
coast arms have to be modelled here too.

THE FUNCTION IT MIRRORS IS `static` NOW, a piece of
physics_drivetrain_frame rather than a public entry.  That changes
nothing here: this file executes nothing from build/ (rule 9) and
compares arithmetic, not linkage.

TWO ROWS IN THE AT-SPEED TABLE ARE EXPECTED TO MISS and are excluded
below rather than fitted:  the first frame after an unpause is a
documented outlier (racers-convergence.txt 9.17.8.2) and one frame ran 34 ms
against the 33 this harness assumes.
"""
import math
import struct

K_RPM_ANCHOR = 1000.0
K_GRAVITY = 9.81
K_TORQUE_FLOOR = 0.2
K_TORQUE_BONUS = 1.1
K_DT_SCALE = 10.0
K_GAIN_CAP = 0.03
K_DIV_THRESHOLD = 0.0001
K_DIV_LOWSPEED = 10000.0
K_IDLE_RPM = 1200.0
K_MILLI = 0.001
K_DRAG_GROUND = 0.3
K_DRAG_AIR = 1.6
K_SLIP_MIN = 0.01
K_SLIP_MAX = 20.0
K_SLIP_RATE = -0.05
K_GRIP_FULL = 1.0
K_BRAKE_MIN = 10.0
K_BRAKE_MAX = 140.0
K_BRAKE_RAMP_MS = 200.0
K_BRAKE_RAMP = 1.0 / 6.0
K_KMH_TO_MS = 1.0 / 3.6
K_SLOPE = 0.004905
K_SLOPE_MAX = 0.55
K_WHEELIE_UP = 0.0471238898
K_WHEELIE_DOWN = -0.0251327412
K_WHEELIE_MAX = 1.5707963705
K_WHEELIE_CAP = 200.0
K_WHEELIE_THUMP = -32.0
K_AIR_STEP = 2.592
K_CAMBER_EPS = 1.0
K_CAMBER_G = 9.81
K_CORNER_DT = 0.01
K_DISENGAGED_MUL = 2.0
K_RPM_TO_RADS = math.pi / 30.0


def f32(x):
    return struct.unpack('<f', struct.pack('<f', x))[0]


class Carac(object):
    pass


ROAD = Carac()                 # racers-convergence.txt 9.17.1
ROAD.motoType = 2
ROAD.moteurVitesseMax = 7.0
ROAD.motoMasse = 280.0
ROAD.realSpeedMax = 80.0
ROAD.regimeMax = 12000.0
ROAD.regimeCoupleMax = 10500.0
ROAD.coupleMax = 15.0
ROAD.couple1000 = 6.0
ROAD.coupleRegimeMax = 2.0
ROAD.minAdherenceWheel = 0.0
ROAD.frein = 0.0

DIRT = Carac()                 # racers.txt 9.18.6
DIRT.motoType = 1
DIRT.moteurVitesseMax = 8.0
DIRT.motoMasse = 150.0
DIRT.realSpeedMax = 46.0
DIRT.regimeMax = 10000.0
DIRT.regimeCoupleMax = 6500.0
DIRT.coupleMax = 7.0
DIRT.couple1000 = 1.0
DIRT.coupleRegimeMax = 1.0
DIRT.minAdherenceWheel = 0.0
DIRT.frein = 80.0


def engine_torque_at(c, engine_speed, wheelie_scale, interlock):
    rpm = (c.regimeMax / c.moteurVitesseMax) * engine_speed
    if rpm >= c.regimeCoupleMax:
        den = c.regimeMax - c.regimeCoupleMax
        m = 0.0 if den == 0.0 else (c.coupleRegimeMax - c.coupleMax) / den
        b = c.coupleMax - c.regimeCoupleMax * m
    else:
        den = c.regimeCoupleMax - K_RPM_ANCHOR
        m = 0.0 if den == 0.0 else (c.coupleMax - c.couple1000) / den
        b = c.couple1000 + m * -K_RPM_ANCHOR
    t = (m * rpm + b) * K_GRAVITY
    if t < K_TORQUE_FLOOR:
        t = K_TORQUE_FLOOR
    if not interlock:
        t *= K_TORQUE_BONUS
    return f32(t * wheelie_scale)


def physics_grip(c, slip):
    grip = K_GRIP_FULL
    if c.motoType == 1:
        if slip > K_SLIP_MIN and slip < K_SLIP_MAX:
            grip = K_GRIP_FULL + K_SLIP_RATE * slip
    else:
        if c.minAdherenceWheel > K_GRIP_FULL:
            grip = c.minAdherenceWheel
    return f32(grip)


def physics_drag(c, speed, dt_ms, airborne):
    k = K_DRAG_AIR if airborne else K_DRAG_GROUND
    return f32(speed * speed * dt_ms * k / c.motoMasse * K_MILLI)


def body_axis_speed(m, vel, speed, row):
    """Shared body: row 0 is lateral, row 2 longitudinal - the
    original ships two near-identical functions differing only here."""
    x, y, z = vel[0], vel[1], vel[2]
    ln = math.sqrt(x * x + y * y + z * z)
    if ln != 0.0:
        inv = 1.0 / ln
        x *= inv
        y *= inv
        z *= inv
    vx, vy, vz = x * speed, y * speed, z * speed
    r = row * 3
    return f32(m[r] * vx + m[r + 1] * vy + m[r + 2] * vz
               - (m[r] * m[9] + m[r + 1] * m[10] + m[r + 2] * m[11]))


def physics_slip_speed(m, vel, speed):
    return body_axis_speed(m, vel, speed, 0)


def physics_long_speed(m, vel, speed):
    return body_axis_speed(m, vel, speed, 2)


def physics_wheelie_tick(w, dt, requested):
    if requested and w['timer'] < K_WHEELIE_CAP:
        w['angle'] = f32(w['angle'] + dt * K_WHEELIE_UP)
        if w['angle'] > K_WHEELIE_MAX:
            w['angle'] = f32(K_WHEELIE_MAX)
        w['timer'] = f32(w['timer'] + dt)
    elif w['angle'] > 0.0:
        w['angle'] = f32(w['angle'] + dt * K_WHEELIE_DOWN)
        if w['angle'] < 0.0:
            w['landThump'] = K_WHEELIE_THUMP
            w['angle'] = 0.0
    if w['angle'] != 0.0:
        w['scale'] = 2.0
    else:
        w['scale'] = 1.0
        if not requested:
            w['timer'] = 0.0
    return w


def physics_camber(dy):
    return f32(dy / math.sqrt(dy * dy + K_CAMBER_EPS) * K_CAMBER_G)


def physics_centripetal(speed, radius, moto_type, disengaged):
    if moto_type != 1 or radius == 0.0:
        return 0.0
    a = (speed * speed) / radius
    if disengaged:
        a *= K_DISENGAGED_MUL
    return f32(a)


def physics_cornering_step(camber, centri, slip, friction, dt):
    out = (camber + centri) * dt * K_CORNER_DT + slip
    drop = friction * K_CAMBER_G * dt * K_CORNER_DT
    if out > 0.0:
        out -= drop
        if out < 0.0:
            out = 0.0
    elif out < 0.0:
        out += drop
        if out > 0.0:
            out = 0.0
    return f32(out)


def physics_air_time(air, on_ground):
    if on_ground:
        return 0.0
    return f32(air + K_AIR_STEP)


def physics_slope(dir_y, dt_ms, airborne):
    if airborne:
        return 0.0
    t = -dir_y * dt_ms * K_SLOPE
    if t > K_SLOPE_MAX:
        t = K_SLOPE_MAX
    return f32(t)


def physics_brake_force(c, hold_ms, ramp_override):
    force = c.frein
    if force < K_BRAKE_MIN:
        force = K_BRAKE_MIN
    elif force > K_BRAKE_MAX:
        force = K_BRAKE_MAX
    if hold_ms < K_BRAKE_RAMP_MS and not ramp_override:
        force *= K_BRAKE_RAMP
    return f32(force)


def physics_brake_step(c, hold_ms, dt_ms, ramp_override):
    force = physics_brake_force(c, hold_ms, ramp_override)
    return f32(force * K_KMH_TO_MS * dt_ms * K_MILLI)


def engine_gain(c, engine_speed, road_speed, dt_scaled, wheelie, interlock):
    dt_ms = dt_scaled * K_DT_SCALE
    # the idle floor - without it a stopped bike makes no torque
    idle = c.moteurVitesseMax / c.regimeMax * K_IDLE_RPM
    if engine_speed < idle:
        engine_speed = idle
    rpm = (c.regimeMax / c.moteurVitesseMax) * engine_speed
    power = rpm * K_RPM_TO_RADS * engine_torque_at(c, engine_speed,
                                                   wheelie, interlock)
    # the low-speed guard: multiply by 1/threshold instead of dividing
    if road_speed > K_DIV_THRESHOLD:
        gain = power / road_speed
    else:
        gain = power * K_DIV_LOWSPEED
    gain = dt_ms * gain / c.motoMasse * K_MILLI
    return min(gain, dt_ms * K_GAIN_CAP)


fail = 0


def check(label, got, want, tol):
    global fail
    ok = abs(got - want) <= tol
    if not ok:
        fail += 1
    print('  %-34s got %-12.6f want %-12.6f %s'
          % (label, got, want, 'ok' if ok else 'FAIL'))


print('GRIP CURVE - racers.txt 9.19.1, dirt bike, 6 dp')
for slip, want in ((0.576639, 0.971168), (1.17717, 0.941142),
                   (3.05132, 0.847434)):
    check('|slip| %.6f' % slip, physics_grip(DIRT, slip), want, 1e-6)
check('|slip| 1.95e-07 (below band)', physics_grip(DIRT, 1.95202e-07),
      1.0, 0.0)
check('|slip| 25.0 (above band, CLIFF)', physics_grip(DIRT, 25.0),
      1.0, 0.0)
check('road bike, slip irrelevant', physics_grip(ROAD, 3.05132),
      1.0, 0.0)

print()
print('DRAG - racers-convergence.txt 9.17.9.3, road bike, dt 3.3, C->D drop')
for speed, want in ((23.9644, 0.02029), (24.4334, 0.02110),
                    (24.9073, 0.02192), (25.3863, 0.02278),
                    (25.8703, 0.02365)):
    check('speed %.4f' % speed, physics_drag(ROAD, speed, 33.0, 0),
          want, 5e-5)
check('airborne is 5.33x', physics_drag(ROAD, 23.9644, 33.0, 1),
      0.02029 * (1.6 / 0.3), 3e-4)

print()
print('ENGINE GAIN - racers-convergence.txt 9.17.7.2, road bike,'
      ' standing start')
for road, eng, want in ((1.0800, 0.7000, 0.83256),
                        (1.9126, 0.7313, 0.49469),
                        (2.4072, 0.9204, 0.51884),
                        (2.9261, 1.1188, 0.54415),
                        (3.4702, 1.3268, 0.57068),
                        (4.0409, 1.5450, 0.59847),
                        (4.6394, 1.7738, 0.62759)):
    check('road %.4f eng %.4f' % (road, eng),
          engine_gain(ROAD, eng, road, 3.3, 1.0, 1), want, 2e-3)

print()
print('ENGINE GAIN - racers-convergence.txt 9.17.8.1, road bike, at speed')
# 42.6366 (post-unpause outlier) and 43.4451 (a 34 ms frame) are
# excluded - see the module docstring.  They are listed here so the
# exclusion is visible rather than looking like missing data.
for road, eng, want in ((42.9165, 5.4720, 0.36890),
                        (43.1807, 5.5057, 0.37035),
                        (43.7182, 5.5743, 0.37329),
                        (43.9853, 5.6083, 0.37476)):
    check('road %.4f eng %.4f' % (road, eng),
          engine_gain(ROAD, eng, road, 3.3, 1.0, 1), want, 2e-3)

print()
print('BRAKE - racers.txt 9.21, dirt bike, Frein 80, one hard stop')
# hold counter at the START of each frame, from the watchpoint ladder
# 0, 36, 69, 102, 136, 169, 202, 235 - and the force switches on the
# frame after it crosses 200.
BRAKE = ((0,   36.0, 13.3333, 0.133333),
         (36,  33.0, 13.3333, 0.122222),
         (69,  33.0, 13.3333, 0.122222),
         (102, 34.0, 13.3333, 0.125926),
         (136, 33.0, 13.3333, 0.122222),
         (169, 33.0, 13.3333, 0.122222),
         (202, 33.0, 80.0000, 0.733333),
         (235, 33.0, 80.0000, 0.733333))
for hold, dt, wforce, wstep in BRAKE:
    check('hold %3d ms force' % hold,
          physics_brake_force(DIRT, hold, 0), wforce, 1e-4)
    check('hold %3d ms step  (dt %.0f)' % (hold, dt),
          physics_brake_step(DIRT, hold, dt, 0), wstep, 1e-5)

print()
print('SLOPE - racers.txt 9.22, two slopes, five frame times')
SLOPE = ((-0.025715, 200.0, 0.025227), (-0.025715, 129.0, 0.016271),
         (-0.025715,  32.0, 0.004036), (-0.025714,  39.0, 0.004919),
         (-0.025715,  33.0, 0.004162), (-0.263128,  33.0, 0.042591),
         (-0.263151,  34.0, 0.043886), (-0.263151,  33.0, 0.042595))
for dir_y, dt, want in SLOPE:
    check('dirY %9.6f dt %5.1f' % (dir_y, dt),
          physics_slope(dir_y, dt, 0), want, 1e-6)
check('airborne forces zero', physics_slope(-0.263151, 33.0, 1), 0.0, 0.0)
# The clamp cannot fire at 33 ms - |dirY| would have to exceed 3.4 -
# but it does at the 500 ms dt cap, which is what it is there for.
check('clamp unreachable at 33 ms',
      physics_slope(-1.0, 33.0, 0), 1.0 * 33.0 * K_SLOPE, 1e-6)
check('clamp fires at the dt cap',
      physics_slope(-0.263151, 500.0, 0), K_SLOPE_MAX, 1e-6)

print()
print('AIR TIMER - racers.txt 9.18.3, the captured ladder')
air = 0.0
for n in range(1, 9):
    air = physics_air_time(air, 0)
    check('frame %d airborne' % n, air, 2.592 * n, 2e-5)
check('landing resets', physics_air_time(air, 1), 0.0, 0.0)

print()
print('WHEELIE - racers.txt 9.23, timing from 9.17.3')
w = {'angle': 0.0, 'timer': 0.0, 'scale': 1.0, 'landThump': 0.0}
DT = 3.3
n = 0
while w['angle'] < K_WHEELIE_MAX - 1e-6 and n < 100:
    physics_wheelie_tick(w, DT, 1)
    n += 1
check('frames to reach PI/2', float(n), 11.0, 0.5)
check('scale while up', w['scale'], 2.0, 0.0)
while w['timer'] < K_WHEELIE_CAP and n < 200:
    physics_wheelie_tick(w, DT, 1)
    n += 1
check('frames to the 200 cap', w['timer'] / DT, 60.6, 0.7)
check('seconds to the cap', w['timer'] / DT * 0.033, 2.0, 0.03)
# button STILL held - the cap gates the ramp, so it must now decay
d = 0
while w['angle'] > 0.0 and d < 100:
    physics_wheelie_tick(w, DT, 1)
    d += 1
check('decays with button held', float(d), 19.0, 1.0)
check('landing thump fired', w['landThump'], -32.0, 0.0)
check('scale back to 1.0', w['scale'], 1.0, 0.0)
w2 = {'angle': 0.0, 'timer': 0.0, 'scale': 1.0, 'landThump': 0.0}
physics_wheelie_tick(w2, DT, 1)
check('scale is always 2.0 now', w2['scale'], 2.0, 0.0)

print()
print('SLIP SPEED - racers.txt 9.24 (self-consistency, not live)')
# rows then translation; row0 is the LATERAL axis
IDENT = [1, 0, 0,  0, 1, 0,  0, 0, 1,  0, 0, 0]
check('sideways at 10 m/s',
      physics_slip_speed(IDENT, [1.0, 0.0, 0.0], 10.0), 10.0, 1e-5)
check('straight ahead gives zero',
      physics_slip_speed(IDENT, [0.0, 0.0, 1.0], 10.0), 0.0, 1e-6)
check('velDir is normalised first',
      physics_slip_speed(IDENT, [7.0, 0.0, 0.0], 10.0), 10.0, 1e-5)
check('45 degrees of slide',
      physics_slip_speed(IDENT, [1.0, 0.0, 1.0], 10.0),
      10.0 / math.sqrt(2.0), 1e-5)
YAW = [0.6, 0.0, 0.8,  0.0, 1.0, 0.0,  -0.8, 0.0, 0.6,  0, 0, 0]
check('yawed lateral axis',
      physics_slip_speed(YAW, [1.0, 0.0, 0.0], 10.0), 6.0, 1e-5)
check('zero velDir skips the normalise',
      physics_slip_speed(IDENT, [0.0, 0.0, 0.0], 10.0), 0.0, 0.0)
# the -(row0 . t) term, zero in every live sample but computed
TRANS = [1, 0, 0,  0, 1, 0,  0, 0, 1,  2.5, 0, 0]
check('translation term is subtracted',
      physics_slip_speed(TRANS, [1.0, 0.0, 0.0], 10.0), 7.5, 1e-5)

print()
print('CORNERING - racers.txt 9.28 (self-consistency)')
check('camber flat', physics_camber(0.0), 0.0, 0.0)
check('camber dy=1 is g/sqrt2', physics_camber(1.0),
      9.81 / math.sqrt(2.0), 1e-5)
check('camber is antisymmetric', physics_camber(-1.0),
      -9.81 / math.sqrt(2.0), 1e-5)
# the bounded sign function: a cliff pulls no harder than a steep bank
check('camber SATURATES at g', physics_camber(1.0e6), 9.81, 1e-3)
check('centripetal v*v/r', physics_centripetal(10.0, 100.0, 1, 0),
      1.0, 1e-6)
check('centripetal doubles when disengaged',
      physics_centripetal(10.0, 100.0, 1, 1), 2.0, 1e-6)
check('centripetal is MotoType 1 only',
      physics_centripetal(10.0, 100.0, 2, 0), 0.0, 0.0)
# FLT_MAX radius is "going straight" - the term must vanish
check('straight (FLT_MAX radius) gives ~0',
      physics_centripetal(80.0, 3.4028235e38, 1, 0), 0.0, 1e-30)
check('cornering step snaps to zero from +',
      physics_cornering_step(0.0, 0.0, 0.001, 100.0, 3.3), 0.0, 0.0)
check('cornering step snaps to zero from -',
      physics_cornering_step(0.0, 0.0, -0.001, 100.0, 3.3), 0.0, 0.0)
check('cornering step keeps a large value',
      physics_cornering_step(0.0, 0.0, 5.0, 0.0, 3.3), 5.0, 1e-6)
# The airTime threshold IS the air step, so the test reads "more than
# ONE frame airborne".
#
# BOTH SIDES MUST BE FLOAT. The original compares
# (float)_DAT_004d0f20 < airTime - the double is narrowed to float
# FIRST - and f32(2.592) is 2.5920000076..., which is strictly greater
# than the double 2.592. Comparing the float32 timer against the
# double threshold makes one frame look like it triggers when it does
# not. This harness got that wrong on its first run.
THRESH = f32(K_AIR_STEP)
check('one airborne frame does NOT exceed it',
      float(physics_air_time(0.0, 0) > THRESH), 0.0, 0.0)
check('two airborne frames do',
      float(physics_air_time(physics_air_time(0.0, 0), 0) > THRESH),
      1.0, 0.0)

print()
print('SKID WIDTH - racers.txt 9.30 (self-consistency)')


def racer_skid_width(w, prev_lat, lat, human):
    if not human:
        return 5.0
    w = w + 2.0 if prev_lat < lat else w - 1.0
    return f32(min(max(w, 1.0), 12.0))


check('AI is pinned at 5', racer_skid_width(1.0, 0.0, 9.0, 0), 5.0, 0.0)
check('widens by 2 as the slide grows',
      racer_skid_width(3.0, 1.0, 2.0, 1), 5.0, 0.0)
check('narrows by 1 otherwise',
      racer_skid_width(3.0, 2.0, 1.0, 1), 2.0, 0.0)
check('clamps low at 1', racer_skid_width(1.0, 2.0, 1.0, 1), 1.0, 0.0)
check('clamps high at 12', racer_skid_width(12.0, 1.0, 2.0, 1), 12.0, 0.0)
# it opens twice as fast as it closes - 2.0 up against 1.0 down
check('opens twice as fast as it closes',
      (racer_skid_width(5.0, 1.0, 2.0, 1) - 5.0)
      / (5.0 - racer_skid_width(5.0, 2.0, 1.0, 1)), 2.0, 1e-6)

print()
print('LAUNCH - racers-convergence.txt 9.32, the case that was broken')
# A stopped bike must pull away. Before the idle floor and the
# low-speed guard were transcribed it could not: zero road speed
# skipped the gain entirely and zero engine speed made no torque.
idle_road = ROAD.moteurVitesseMax / ROAD.regimeMax * K_IDLE_RPM
check('road bike idles at 0.7', idle_road, 0.7, 1e-6)
g0 = engine_gain(ROAD, 0.0, 0.0, 3.3, 1.0, 1)
check('gain from a standstill is positive', float(g0 > 0.0), 1.0, 0.0)
check('and it is the CAP that limits it', g0, 33.0 * K_GAIN_CAP, 1e-6)
# and it still matches the measured rolling numbers
check('rolling gain unchanged (road 4.6394)',
      engine_gain(ROAD, 1.7738, 4.6394, 3.3, 1.0, 1), 0.62759, 2e-3)

print()
print('GEARBOX - racers.txt 9.33, the shift state machine')
GB_UP = {0: 150, 1: 200}
GB_DOWN = {0: 200, 1: 300}


def gb_new(mode):
    return {'gear': 1, 'timerMs': 0, 'state': 0, 'mode': mode,
            'pending': 0}


def gb_up(g, nbSpeed):
    if g['state'] != 0:
        return
    if nbSpeed <= g['gear']:
        g['gear'] = int(nbSpeed)
        return
    if g['gear'] + g['pending'] < nbSpeed:
        g['pending'] += 1
        g['timerMs'] = GB_UP[1 if g['mode'] else 0]
        g['state'] = 1


def gb_tick(g, nbSpeed, dt):
    if g['state'] == 0:
        return 0
    g['timerMs'] -= dt
    if g['timerMs'] >= 1:
        return 0
    if g['state'] == 1:
        g['pending'] = 0
        g['state'] = 0
        if g['gear'] < nbSpeed:
            g['gear'] += 1
            return 1
        g['gear'] = int(nbSpeed)
    return 0


# A shift is NOT instant: at 33 ms a 200 ms timer needs 7 ticks.
g = gb_new(1)
gb_up(g, 6.0)
check('upshift arms a 200 ms timer', float(g['timerMs']), 200.0, 0.0)
n = 0
while gb_tick(g, 6.0, 33) == 0 and n < 20:
    n += 1
# 200 - 6*33 = 2, still >= 1; the SEVENTH tick takes it negative.
check('six 33 ms ticks pass, the 7th shifts', float(n), 6.0, 0.0)
check('the gear then moves to 2', float(g['gear']), 2.0, 0.0)
# manual mode is quicker
g = gb_new(0)
gb_up(g, 6.0)
check('manual arms 150 ms instead', float(g['timerMs']), 150.0, 0.0)
# at the top gear the request clamps rather than doing nothing
g = gb_new(1)
g['gear'] = 6
gb_up(g, 6.0)
check('at the top it clamps, not no-ops', float(g['gear']), 6.0, 0.0)
check('and arms nothing', float(g['state']), 0.0, 0.0)

print()
print('SPEED FAMILY - racers.txt 9.34, read against write')


def body_from_world(m, w):
    return [m[i*3+0]*(w[0]-m[9]) + m[i*3+1]*(w[1]-m[10])
            + m[i*3+2]*(w[2]-m[11]) for i in range(3)]


def physics_set_long_speed(m, vel, speed, realMax, v):
    v = max(-realMax, min(realMax, v))
    ln = math.sqrt(sum(c*c for c in vel))
    n = [c/ln for c in vel] if ln != 0.0 else list(vel)
    w = [c*speed for c in n]
    b = body_from_world(m, w)
    w = [b[0]*m[0+i] + b[1]*m[3+i] + v*m[6+i] + m[9+i] for i in range(3)]
    ln = math.sqrt(sum(c*c for c in w))
    if ln <= 1e-5:
        return [0.0, 0.0, 0.0], f32(ln)
    return [f32(c/ln) for c in w], f32(ln)


# A yawed, tilted frame - an identity matrix would hide index errors.
M = [0.6, 0.0, 0.8,  0.0, 1.0, 0.0,  -0.8, 0.0, 0.6,  0.0, 0.0, 0.0]

# THE ROUND TRIP. set the forward speed, then read it back with the
# independently transcribed reader - they must agree.
for want in (10.0, -4.0, 0.0, 37.5):
    vel, sp = physics_set_long_speed(M, [1.0, 0.0, 0.0], 12.0, 80.0, want)
    got = physics_long_speed(M, vel, sp)
    check('set %6.1f -> long_speed reads it back' % want, got, want, 2e-4)

# and the lateral component must SURVIVE the write
vel0 = [1.0, 0.0, 0.0]
lat0 = physics_slip_speed(M, vel0, 12.0)
vel1, sp1 = physics_set_long_speed(M, vel0, 12.0, 80.0, 25.0)
check('lateral component is preserved',
      physics_slip_speed(M, vel1, sp1), lat0, 2e-4)

# the clamp, and the zeroed direction
vel2, sp2 = physics_set_long_speed(M, [0.0, 0.0, 1.0], 5.0, 46.0, 999.0)
check('clamped to realSpeedMax', physics_long_speed(M, vel2, sp2),
      46.0, 2e-3)
vel3, sp3 = physics_set_long_speed(M, [0.0, 0.0, 0.0], 0.0, 80.0, 0.0)
check('a stopped bike gets a ZERO direction',
      float(vel3 == [0.0, 0.0, 0.0]), 1.0, 0.0)

print()
print('%d failure(s)' % fail)
raise SystemExit(1 if fail else 0)
