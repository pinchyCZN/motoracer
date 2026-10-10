#!/usr/bin/env python3
"""mr_place.py - put the bike somewhere, pointing somewhere, for tests.

    python tools/mr_place.py                    report, change nothing
    python tools/mr_place.py --yaw 90           turn it 90 degrees
    python tools/mr_place.py --rel-yaw -90      turn it 90 to the left
    python tools/mr_place.py --at X Y Z         move it
    python tools/mr_place.py --spot wall-left   a recorded test spot
    python tools/mr_place.py --list             what spots exist
    python tools/mr_place.py --to mracer        place OUR PORT
    python tools/mr_place.py --to both          place both alike
    python tools/mr_place.py --speed 0          and set the run speed
    python tools/mr_place.py --gear 1           and set the gear

SPEED AND GEAR ARE PART OF A PLACEMENT, and leaving them out cost a
whole session. A position and a heading are not a controlled start:
the bike also arrives with whatever speed and whatever GEAR the last
test left it in, and second gear is too tall to launch, so one side
bogs near 1 m/s while the other pulls away. That looked in turn like a
cornering fault, a set_slip_speed latch and a clutch plateau, and it
was none of them - see racers-convergence.txt 9.18.11.

    --speed writes speed, speedPrev and gearboxRoadSpeed together,
            because the drivetrain resyncs them to each other and a
            lone speed write is gone within a frame.
    --gear   writes the gear on both sides. THE TWO KEEP IT IN
            DIFFERENT PLACES: MOTO at racer+0x7c, the port in
            s_race_gearbox. racer+0x7c EXISTS in our struct and
            nothing reads it, so writing it there silently did
            nothing while the same write on MOTO worked - a
            gear-matched run that was not.

--to both NOW VERIFIES. It reads position, heading, speed and gear
back from both games after writing and prints MATCHED or a per-field
complaint, because every one of those three false defects came from
trusting a placement that had not taken.

BOTH SIDES OR NEITHER. The point of writing a placement is that the
same impact happens twice, so --to both is usually the one to
reach for: it sets MOTO and mracer from one set of numbers and
reports each, and a difference in the two reports is then a
difference between the two worlds rather than in how they were set
up.

THE TWO SIDES ARE ADDRESSED DIFFERENTLY, and they have to be. MOTO
is written by absolute address, because its symbols live in Ghidra
and not in the binary. The port is written BY SYMBOL through its
own agent pipe, against the linker .map that Release now emits.

AND THEY STORE THE HEADING DIFFERENTLY, which matters more. MOTO
keeps it in the orientation matrix at racer+0xbc and rebuilds that
matrix from itself every frame, so writing the matrix IS writing
the heading. The port keeps a scalar, s_race_yawWorld, and derives
the matrix and moveDir from it - so writing the port's matrix
achieves nothing and the scalar is what has to change. Neither is
wrong; they are simply not the same shape, and a tool that assumed
one layout for both would silently no-op on one side.

WHY THIS EXISTS. Wall and crash behaviour depends on the angle and the
speed of the impact, and driving to a particular angle through
SEND_INPUT is guesswork - the bike arrives somewhere slightly
different every run, so two captures are never of the same event.
Writing the position and the heading directly makes an impact
REPEATABLE, which is what turns a capture into a measurement.

IT ONLY WRITES DATA. Position is racer+0x30 and the orientation is
racer+0xbc; both are plain floats in the racer record, so this is
GET_MEM and SET_MEM and needs nothing in the DLL. It does not touch
.text and it is not a cheat - it is a test fixture, and the game
carries on from wherever it is put.

THE ORIENTATION LAYOUT [verified, read live]:

    +0xbc   lateral  = ( cos yaw, 0,  sin yaw)
    +0xc8   up       = (       0, 1,        0)
    +0xd4   forward  = (-sin yaw, 0,  cos yaw)
    +0xe0   translation, zero in ordinary riding

so yaw comes back out as atan2(-forward.x, forward.z). See
docs/subsystems/racers.txt 9.29.9 for where those rows come from and
why the translation rides with them.

MOVEDIR IS WRITTEN TOO, and it has to be. racer+0x130 is where the
bike POINTS and racer+0x18c is where it is GOING, and only the second
one moves it. Turning the matrix alone leaves a bike that faces the
wall and drives past it, which looks like a broken placement and is
not one. --keep-move opts out when a mismatch is the point of the
test - a slide, or a spin.

A FLAT YAW IS WHAT THIS WRITES. The real matrix carries terrain pitch
- UpdateGroundContact rebuilds it from two ground probes every frame -
so a placed bike is momentarily flatter than a driven one and the
first tick after a placement corrects it. That is fine for an impact
test and wrong for anything measuring attitude.
"""

import argparse
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bridge_mcp_mrhook as mr          # noqa: E402

G_RACERS = 0x00581218
STRIDE   = 0x3209
NEWLINE  = chr(10)
OFF_POS  = 0x30
OFF_MAT  = 0xbc
OFF_MOVE = 0x18c
#
# velDir, racer+0x130. WHERE THE BIKE IS GOING as a unit vector, as
# against moveDir at +0x18c. It has to be placed too: race.c reads it
# in physics_set_long_speed BEFORE racer_integrate re-seeds it from
# the heading, so frame one of a placed run consumes whatever the
# PREVIOUS test left in it. That made the port non-deterministic from
# an identical placement - 11.4517 one run and 11.4762 the next, with
# MOTO giving 11.4806 both times.
#
OFF_VELDIR = 0x130
#
# THE BARS. steerRate ramps by 33 a frame while a steer key is held
# and steering is the angle it drives, clamped at the lock stop of
# 600. Both are placement state: a bike whose rate is already at 726
# slams to full lock on its FIRST frame, while a freshly-zeroed one
# takes four frames to get there. Left alone, MOTO entered a turn
# with 660 already wound on and the port with 0, which read as a
# 2.9-degree steering-rate defect that did not exist.
#
OFF_STEER_RATE = 0x10c
OFF_STEERING   = 0x110
#
# THE RUN SPEED TRIPLE - speed, speedPrev, gearboxRoadSpeed - written
# as one twelve-byte block. The drivetrain keeps them consistent with
# each other every frame, so setting speed alone does not stick.
#
OFF_SPEED = 0x124
#
# THE GEAR, and it is on the RACER in MOTO. The port keeps it in
# s_race_gearbox instead; see MRACER_GEARBOX below.
#
OFF_GEAR = 0x7c
#
# THE REST OF THE GEARBOX. Setting the gear NUMBER and nothing else
# leaves a shift half-finished - timerMs counting, state non-zero, a
# pending request - and the next launch then behaves like the gear it
# is shifting INTO. mode and savedEngine are left alone: mode is the
# auto/manual choice and is not test state.
#
OFF_GB_TIMER   = 0x80
OFF_GB_STATE   = 0x84
OFF_GB_PENDING = 0x8c
#
# engineSpeed. MR_Racer is byte-exact between the two, so one offset
# serves both sides - the same reason OFF_MAT is used on the port.
#
OFF_ENGINE = 0xec
#
# THE CRASH STATE, and a placement is not a controlled start without
# it. maybe_Racer_BeginCrash sets crashLatch and the whole input chain
# is gated on it - maybe_Racer_ApplyDrive and maybe_Racer_ApplySteering
# both return early - so a bike placed after a crash sits there with
# the throttle open and decelerates to nothing. It cost a whole descent
# run: MOTO read crashLatch 1 against the port's 0 and coasted to a
# stop while the port drove away, which reads exactly like a physics
# divergence and is not one.
#
# THESE ARE THE FIELDS Racer_EndCrash CLEARS (racer.c, 0x00466140's
# tail) plus the two a wall hit leaves behind.
#
OFF_CRASHLATCH = 0x30c1
OFF_CRASHTIMER = 0x30c5
OFF_RIDERBITS  = 0x2dac
OFF_IMPACTBLEND = 0x31d1
#
# AND THE PENDING RECOVERY YAW, which is the one that bit hardest.
# A bike parked after a wall contact still has racer+0x2e31 loaded,
# and UpdateGroundContact drains it into matOrientation a step a
# frame WHETHER OR NOT ANYTHING IS TOUCHING A WALL. So a placement
# that leaves it set turns the bike off line during the run with no
# input at all - MOTO came out 17 degrees round and missed the wall
# the test existed to hit, while the port, freshly reset, went
# straight. It looks like a steering divergence and it is stale
# state. racers-convergence.txt 9.18.33.
#
OFF_LEAN = 0x2e31
#
# AND THE WHEELIE. Same story a fifth time: a bike parked mid-wheelie
# keeps its angle, and maybe_Racer_UpdateWheelie only decays it while
# the tick runs - so a placement leaves it counting down into the
# next test. It is not cosmetic: racer_steer_torque HALVES the steer
# torque while wheelieAngle is non-zero, so the side with a leftover
# wheelie turns less hard and leans less, and the two sides part
# company before the test has begun. Measured: MOTO entered a lean
# test still at 1.4049 and built 14 degrees of lean where the port,
# clean, built 34.
#
OFF_WHEELIE_ANGLE = 0x307d
OFF_WHEELIE_TIMER = 0x3081
OFF_WHEELIE_SCALE = 0x00b8
OFF_STUNT_REQ     = 0x2e85
OFF_STUNT_HELD    = 0x31f9
#
# brakeHoldMs, racer+0xb0 (instance+0x8c). THE BRAKE RAMP TIMER, and
# it is placement state for the same reason the gear is: the brake is
# six times weaker for its first 200 ms, so a bike whose counter is
# already past 200 brakes at full strength from frame one while a
# freshly-reset one ramps. Left alone it reads whatever the last test
# left - 660 ms was measured on the port - and the two sides then ramp
# differently, which looks exactly like a constant force error.
#
OFF_BRAKEHOLD = 0xb0
#
# The port keeps it in s_race_drive, not in the racer - the same split
# as the gear. MR_Drivetrain is engineSpeed, airTime, brakeHoldMs,
# speed, gearboxRoadSpeed, four bytes each.
#
MRACER_DRIVE = 's_race_drive'
MRACER_DRIVE_BRAKEHOLD = 8
#
# CURRENT SEGMENT IS +0x31bd, NOT +0x31c1. The two sit next to each
# other and +0x31c1 is reliably curSegment+1, so reading it looks
# right and silently reports the NEXT segment's walls - which on a
# straight are identical and on a curve are not. Verified against
# the segment index UpdateGroundContact hands the wall clip.
#
OFF_SEG  = 0x31bd
#
# segmentStart, racer+0x31c1. THE SEARCH HINT, and placing without it
# strands the bike. Trk_FindSegmentForPoint does not sweep the whole
# track - it works out from where the racer already thinks it is - so
# a placement that jumps a long way leaves MOTO resolving to a segment
# near the OLD one. Measured: dropped onto a bridge at segment 662 it
# reported segment 33, found no ground under itself and fell to y = 0,
# with wall distances in the tens of thousands.
#
# Both fields have to be written, and the segment has to be worked out
# from the POSITION rather than carried over, which is what
# segment_for_point below does.
#
#
# THE RACE CLOCK AND THE TIMEOUT, all MOTO-side. racers.txt 9.8.3:
# g_raceTimer is the grid countdown while g_bPreRace is set and the
# TIME LIMIT afterwards, counting down by the frame delta. When it
# goes negative past a 500-unit grace, maybe_Race_TickClock sets
# DAT_0052c238 and writes raceState 3 into the LOCAL racer - which is
# the Game Over page.
#
# A TEST SESSION TIMES OUT BECAUSE NOTHING EVER RESTARTS THE RACE.
# Frames accumulate across every placement and every hook run, so a
# long session walks the limit down even though no single test is
# slow.  The port cannot hit this - its MR_RaceClock is not wired to
# anything yet - so only MOTO goes to Game Over, and it does it
# silently as far as the pipe is concerned.
#
G_RACE_TIMER   = 0x004dc0a8
G_PRERACE      = 0x004dc0b4
G_TIMEOUT_FLAG = 0x0052c238
OFF_RACESTATE  = 0x2de5

# THE FINISH SET-UP, and these are the whole of it.
#
# race_update_progress closes a lap when the segment index wraps,
# increments lapCounter, and then finishes the racer when
#
#     lapCounter == s_race_laps
#
# so putting the bike a few segments short of the line with
# lapCounter one BELOW the lap count means the next crossing is the
# last one.  That is the only way to reach the finish camera, the
# victory pose and the results HUD without driving the whole race.
OFF_LAPCOUNTER = 0x2f7b        # short
OFF_PROG       = 0x31b5        # trackTotalProgress
OFF_PROGHIGH   = 0x31b9        # the high-water mark beside it

G_RACELAPS = 0x004dc074        # MOTO's lap count for this race
G_RACEREVERSE = 0x004dc144     # the RACE's copy, not the menu's
                               # 0x004dc068 - maybe_Race_
                               # UpdateProgress reads this one
OFF_ACTIVE     = 0x0068        # a racer nobody counts is invisible
OFF_RACECLOCK  = 0x2ddd
OFF_FINISHPOS  = 0x2f85
OFF_BESTLAP    = 0x2de1
RACE_TIMER_TOPUP = 20 * 60 * 1000      # twenty minutes of race

OFF_AIRTIME  = 0xf0
OFF_ONGROUND = 0x2db8
#
# THE AIR STATE HAS TO BE RESET WITH THE POSITION, or a placement
# lands a bike that is flying on one side and grounded on the other.
# racer_integrate forks on flAirTime and the drivetrain's airborne arm
# skips two speed couplings, so a single stale frame is worth about a
# metre a second - which then compounds all the way down the straight.
# A jump comparison run without this reads as a physics difference and
# is not one. racers-convergence.txt 9.46.13.
#
OFF_SEGSTART = 0x31c1

G_TRACK  = 0x0052c108          # segment array base, then the count
G_COUNT  = 0x00509bac          # g_racerCount
OFF_ACT  = 0x68                # the active flag
PORT_MAX = 32                  # GRID_MAX_SLOTS, the port array size

#
# THE PORT. Offsets come from g_race_layout, which race.c exports so
# that this file never hand-copies an offset out of racer.h - see
# the comment on it there.
#
MRACER_LAYOUT = 'g_race_layout'
MRACER_RACERS = 's_race_racers'

# Where the extracted track files live, for --finish. Both games load
# the same .trk, so reading it needs neither of them running.
DATA_DIR = os.path.join(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))), 'decomp', 'DATA')
MRACER_YAW    = 's_race_yawWorld'
MRACER_COUNT  = 's_race_racerCount'
MRACER_GEARBOX = 's_race_gearbox'
SEG_SIZE = 252
#
# THE ENTRY PLANE. The loader overwrites the file's plane 0 with it,
# and it is the ONLY plane Trk_FindSegmentForPoint looks at.
#
ENTRY_PLANE = 0x40
WALL_A   = 0x60                # the two wall planes, {nx,ny,nz,d}
WALL_B   = 0x70

#
# RECORDED TEST SPOTS. Each is (x, y, z, yaw degrees, what it is for).
# Add one whenever a run turns out to be worth repeating - that is the
# whole point of the file.
#
SPOTS = {
    "grid": (None, None, None, None,
             "wherever the last reset put it - use --rel-yaw from here"),

    # ile01 segment 3 is a straight 2560 wide, walls at x -1280/+1280.
    # Sitting at one rail facing the other gives 2480 of run-up and an
    # exactly square hit - moveDir comes out (1, 0, 0) against a
    # (-1, 0, 0) normal, so |cos| is 1.0000 and the scrape branch is
    # reached. Accelerate for about 70 frames.
    "square-right": (-1200.0, 0.0, 2058.0, -90.0,
                     "square into the right rail, 2480 run-up"),
    "square-left": (1200.0, 0.0, 2058.0, 90.0,
                    "square into the left rail, mirror of the above"),

    # THE DIRT ARENA, not ile01 - load a dirt bike and this track
    # before using it, because the coordinates mean nothing on the
    # road tracks. Measured at the grid: forward and moveDir are both
    # (0, 0, 1), so the straight runs in +z.
    #
    # STAY UNDER ABOUT 90 FRAMES OF FULL THROTTLE. The three rises a
    # run from here meets are at z = 10434, 19754 and 22735
    # (racers-convergence.txt 9.46.10), and the big ramp is at
    # z = 18563 - so a flat comparison has roughly 5300 units of run
    # before the first one. Watch onGround; a jump ends the flat test
    # whether it was wanted or not.
    "dirt-flat": (0.0, 0.0, 5130.0, 0.0,
                  "the dirt arena grid straight - flat to z=10434"),

    # Mid-straight, pointing down the track, 1280 of clearance either
    # side. NOTHING IS IN THE WAY, which is the point: a wheelie test
    # has to be able to run for a hundred frames without a rail
    # touching the result, and every other spot here aims AT a wall.
    "glance-left": (-1200.0, 0.0, 2058.0, 15.0,
                    "a GLANCING hit on the left rail. Fifteen degrees "
                    "closes the 80-unit gap in about six frames and "
                    "bounces; square on scrapes instead, and 45 degrees "
                    "at 20 m/s crashes MOTO into a race reset. RUN "
                    "mr_cheats.py --off nowalls FIRST or the bike rides "
                    "straight through - racers.txt 9.18.23.3"),
    "straight": (0.0, 0.0, 2058.0, 0.0,
                 "down the straight, clear either side - stunt tests"),
}


def _read(addr, size):
    reply = mr.pipe_command("GET_MEM %08x %d" % (addr, size))
    if not reply.startswith("OK"):
        raise RuntimeError(reply.strip())
    return bytes.fromhex(reply.split()[-1])


def _write(addr, raw):
    reply = mr.pipe_command("SET_MEM %08x %s" % (addr, raw.hex()))
    if not reply.startswith("OK"):
        raise RuntimeError(reply.strip())


def racer_base(index):
    return G_RACERS + index * STRIDE


def get_state(index):
    base = racer_base(index)
    pos = struct.unpack("<3f", _read(base + OFF_POS, 12))
    mat = struct.unpack("<12f", _read(base + OFF_MAT, 48))
    yaw = math.atan2(-mat[6], mat[8])
    return pos, mat, yaw


def set_pos(index, pos):
    _write(racer_base(index) + OFF_POS, struct.pack("<3f", *pos))


def set_yaw(index, yawRad, keepTranslation):
    c = math.cos(yawRad)
    s = math.sin(yawRad)
    m = [c, 0.0, s,
         0.0, 1.0, 0.0,
         -s, 0.0, c] + list(keepTranslation)
    _write(racer_base(index) + OFF_MAT, struct.pack("<12f", *m))


def set_move(index, yawRad):
    """Point BOTH travel directions where the bike faces."""
    dir3 = struct.pack("<3f", -math.sin(yawRad), 0.0, math.cos(yawRad))
    _write(racer_base(index) + OFF_MOVE, dir3)
    _write(racer_base(index) + OFF_VELDIR, dir3)
    _write(racer_base(index) + OFF_STEER_RATE, struct.pack("<2f", 0.0, 0.0))


def get_drive(index):
    """(speed, gear) for MOTO."""
    base = racer_base(index)
    speed = struct.unpack("<f", _read(base + OFF_SPEED, 4))[0]
    gear = struct.unpack("<i", _read(base + OFF_GEAR, 4))[0]
    return speed, gear


def set_speed(index, v):
    """speed, speedPrev and gearboxRoadSpeed, and the ENGINE.

    The drivetrain resyncs the three speeds to each other every frame,
    so a lone speed write is gone before anything reads it. engineSpeed
    goes with them because it is what the torque curve is read at - a
    bike placed at rest with a spinning engine is not at rest.
    """
    base = racer_base(index)
    _write(base + OFF_SPEED, struct.pack("<3f", v, v, v))
    _write(base + OFF_ENGINE, struct.pack("<f", 0.0))
    _write(base + OFF_BRAKEHOLD, struct.pack("<i", 0))


def clear_crash(index):
    """Put the rider back on the bike. See OFF_CRASHLATCH above."""
    base = racer_base(index)
    _write(base + OFF_CRASHLATCH, struct.pack("<i", 0))
    _write(base + OFF_CRASHTIMER, struct.pack("<f", 0.0))
    _write(base + OFF_RIDERBITS, struct.pack("<I", 0))
    _write(base + OFF_IMPACTBLEND, struct.pack("<f", 0.0))
    _write(base + OFF_LEAN, struct.pack("<f", 0.0))
    _write(base + OFF_WHEELIE_ANGLE, struct.pack("<f", 0.0))
    _write(base + OFF_WHEELIE_TIMER, struct.pack("<f", 0.0))
    _write(base + OFF_WHEELIE_SCALE, struct.pack("<f", 1.0))
    _write(base + OFF_STUNT_REQ, struct.pack("<i", 0))
    _write(base + OFF_STUNT_HELD, struct.pack("<i", 0))
    _write(base + OFF_AIRTIME, struct.pack("<f", 0.0))
    _write(base + OFF_ONGROUND, struct.pack("<i", 1))


def race_timed_out(index):
    """True when MOTO has already dropped this racer into Game Over.

    raceState 3 is the timeout; 0 and 1 are racing. Read it BEFORE
    trusting a comparison - a timed-out MOTO still answers GET_MEM and
    still accepts SEND_INPUT, it just does not advance the race, so
    the diff looks like a physics divergence and is not one.
    """
    st = struct.unpack("<i", _read(racer_base(index) + OFF_RACESTATE, 4))[0]
    return st == 3, st


def race_timer_topup():
    """Wind the time limit back up so a session cannot time out.

    ONLY WHILE RACING. g_raceTimer is the grid countdown when
    g_bPreRace is set, and topping it up there would hold the bike on
    the grid instead - two timers in one variable, racers.txt 9.8.4.

    Returns (applied, before, after).
    """
    pre = struct.unpack("<i", _read(G_PRERACE, 4))[0]
    before = struct.unpack("<i", _read(G_RACE_TIMER, 4))[0]
    if pre:
        return False, before, before
    if before >= RACE_TIMER_TOPUP:
        return False, before, before
    _write(G_RACE_TIMER, struct.pack("<i", RACE_TIMER_TOPUP))
    return True, before, RACE_TIMER_TOPUP


def race_unstick(index):
    """UNTESTED. Try to pull MOTO back out of Game Over in place.

    It writes the three things maybe_Race_TickClock set on the way in
    - raceState back to 1, the flag at 0x0052c238 back to 0, and a
    fresh time limit - on the theory that nothing else latched. It has
    never actually been run against a timed-out race, because the one
    time we hit Game Over the session was restarted before this
    existed. If it does not work, restart MOTO; do not build anything
    on top of it until it has been seen to work once.
    """
    _write(racer_base(index) + OFF_RACESTATE, struct.pack("<i", 1))
    _write(G_TIMEOUT_FLAG, struct.pack("<i", 0))
    _write(G_RACE_TIMER, struct.pack("<i", RACE_TIMER_TOPUP))


def set_gear(index, g):
    """The gear, and the shift machinery with it."""
    base = racer_base(index)
    _write(base + OFF_GEAR, struct.pack("<i", g))
    _write(base + OFF_GB_TIMER, struct.pack("<i", 0))
    _write(base + OFF_GB_STATE, struct.pack("<i", 0))
    _write(base + OFF_GB_PENDING, struct.pack("<i", 0))


_SEGCACHE = None


def _segments():
    """The whole segment array, read once and kept.

    segment_for_point sweeps all 788 of them, and reading each record
    over the pipe as it goes takes minutes. One bulk read takes about a
    second, so the cache is the difference between a usable helper and
    an unusable one. The track does not change under a running race; if
    it ever does, clear this.
    """
    global _SEGCACHE
    if _SEGCACHE is not None:
        return _SEGCACHE
    segBase, segCount = struct.unpack("<2I", _read(G_TRACK, 8))
    need = segCount * SEG_SIZE
    buf = b""
    while len(buf) < need:
        n = min(4096, need - len(buf))   # GET_MEM caps at 4096
        buf += _read(segBase + len(buf), n)
    _SEGCACHE = (segCount, buf)
    return _SEGCACHE


def segment_planes(seg):
    """(normal, d) for both walls of one segment, from the cache."""
    count, buf = _segments()
    rec = buf[seg * SEG_SIZE:(seg + 1) * SEG_SIZE]
    return [struct.unpack_from("<4f", rec, off) for off in (WALL_A, WALL_B)]


def segment_distances(seg, pos):
    """Signed distance from pos to each of a segment's two walls."""
    return [n[0] * pos[0] + n[1] * pos[1] + n[2] * pos[2] + n[3]
            for n in segment_planes(seg)]


def entry_distance(seg, pos):
    """Signed distance from pos to a segment's ENTRY plane, plane 0."""
    count, buf = _segments()
    n = struct.unpack_from("<4f", buf, seg * SEG_SIZE + ENTRY_PLANE)
    return n[0] * pos[0] + n[1] * pos[1] + n[2] * pos[2] + n[3]


def segment_centre(seg):
    count, buf = _segments()
    return struct.unpack_from("<3f", buf, seg * SEG_SIZE)


def segment_for_point(pos):
    """Which segment contains this point - swept over the whole track.

    Slower than the game's own search and that is the point: it does
    not need a hint, so it can be used to SET the hint after a jump.

    THE TEST IS THE GAME'S OWN, AND IT IS THE ENTRY PLANE, NOT THE
    WALLS. Trk_FindSegmentForPoint (0040c7e0) walks from its hint
    over plane 0 and stops on the segment it has entered whose
    SUCCESSOR it has not - so that pair of comparisons, and no other,
    decides the answer. This used to sweep the two WALL planes and
    keep the tightest fit, which is a different criterion with no
    relation to the game's: it put the bike on segment 59 where the
    game said 109, and on 110 where the game said 100. A placement
    that starts on the wrong segment reads its ground height off the
    wrong plane, so every frame after it is measuring the harness.

    ONE SWEEP CANNOT BE UNAMBIGUOUS, because entry planes are
    infinite too - the stuck-bike point satisfies the pair on 109 and
    on 527. The game breaks the tie by walking LOCALLY from a hint it
    already trusts; with no hint, the nearest segment centre picks
    the same one. Checked against three points the game had already
    answered: 109, 9 and 100, all exact.
    """
    count, buf = _segments()
    cand = [seg for seg in range(count)
            if entry_distance(seg, pos) >= 0.0
            and entry_distance((seg + 1) % count, pos) < 0.0]
    if not cand:
        return None

    def far(seg):
        c = segment_centre(seg)
        return sum((c[k] - pos[k]) ** 2 for k in range(3))

    return min(cand, key=far)


def set_segment(index, seg):
    """Both the segment and the search hint."""
    base = racer_base(index)
    _write(base + OFF_SEG, struct.pack("<i", seg))
    _write(base + OFF_SEGSTART, struct.pack("<i", seg))


def walls(index):
    """The two wall planes of the segment the bike is standing in.

    Returned as (normal, signed distance) pairs. A POSITIVE distance
    is inside the track - the normals face inwards - so the pair also
    says how much room there is either side, which is what a run-up
    for an impact test is measured in.
    """
    base = racer_base(index)
    segBase, segCount = struct.unpack("<2I", _read(G_TRACK, 8))
    seg = struct.unpack("<i", _read(base + OFF_SEG, 4))[0]
    if seg < 0 or seg >= segCount:
        return None
    pos = struct.unpack("<3f", _read(base + OFF_POS, 12))
    rec = _read(segBase + seg * SEG_SIZE, SEG_SIZE)
    out = []
    for off in (WALL_A, WALL_B):
        n = struct.unpack_from("<4f", rec, off)
        out.append((n[:3], n[0] * pos[0] + n[1] * pos[1]
                    + n[2] * pos[2] + n[3]))
    return seg, out


# ------------------------------------------------------------------
# the port
# ------------------------------------------------------------------

def _mracer(cmd):
    reply = mr.pipe_command(cmd, target='mracer')
    if not reply.startswith('OK'):
        raise RuntimeError(reply.strip())
    return reply


def _mracer_get(name, size, offset=None):
    cmd = 'GET_MEM name=%s len=%d' % (name, size)
    if offset is not None:
        cmd += ' offset=%d' % offset
    return bytes.fromhex(_mracer(cmd).split(NEWLINE)[1].strip())


def _mracer_set(name, raw, offset=None):
    cmd = 'SET_MEM name=%s value=%s' % (name, raw.hex())
    if offset is not None:
        cmd += ' offset=%d' % offset
    return _mracer(cmd)


def mracer_layout():
    """(stride, posOff, moveOff, segOff, speedOff, gearOff) out of
    the port. race.c exports it so this file never hand-copies an
    offset out of racer.h.

    SIX INTS SINCE speed AND gear JOINED THE PLACEMENT. An older port
    exports four; that is read as "cannot set speed or gear here"
    rather than as an error, so the tool still places position and
    heading against a stale build.
    """
    raw = _mracer_get(MRACER_LAYOUT, 24)
    if len(raw) >= 24:
        return struct.unpack('<6i', raw)
    stride, posOff, moveOff, segOff = struct.unpack('<4i', raw[:16])
    return stride, posOff, moveOff, segOff, None, None


def mracer_state(index):
    stride, posOff, moveOff, segOff, speedOff, gearOff = \
        mracer_layout()
    base = index * stride
    pos = struct.unpack('<3f', _mracer_get(MRACER_RACERS, 12,
                                           base + posOff))
    yaw = struct.unpack('<f', _mracer_get(MRACER_YAW, 4))[0]
    # travel now lives in moveDir, not a scalar - read it there
    stride, posOff, moveOff, segOff, speedOff, gearOff = \
        mracer_layout()
    mv  = struct.unpack('<3f', _mracer_get(MRACER_RACERS, 12,
                                           index * stride + moveOff))
    mov = math.atan2(-mv[0], mv[2])
    seg = struct.unpack('<i', _mracer_get(MRACER_RACERS, 4,
                                          base + segOff))[0]
    return pos, yaw, mov, seg


def mracer_place(index, pos, yawRad, keepMove):
    """Position, orientation and travel - the SAME fields as MOTO.

    THIS USED TO WRITE THE SCALAR s_race_yawWorld and that stopped
    working. Before the whole-function imports the port kept its
    heading in that scalar and derived the matrix from it; since
    racer_apply_steering was imported the MATRIX is the state and the
    scalar is only read back from it, so writing the scalar placed
    nothing and the bike kept its old heading.

    The struct is byte-exact now, so this writes matOrientation at
    +0xbc and moveDir at +0x18c exactly as the MOTO path does - one
    layout, both games.
    """
    stride, posOff, moveOff, segOff, speedOff, gearOff = \
        mracer_layout()
    base = index * stride
    c = math.cos(yawRad)
    s = math.sin(yawRad)
    mat = [c, 0.0, s,
           0.0, 1.0, 0.0,
           -s, 0.0, c,
           0.0, 0.0, 0.0]
    _mracer_set(MRACER_RACERS, struct.pack('<3f', *pos), base + posOff)
    _mracer_set(MRACER_RACERS, struct.pack('<12f', *mat), base + OFF_MAT)
    if not keepMove:
        dir3 = struct.pack('<3f', -s, 0.0, c)
        _mracer_set(MRACER_RACERS, dir3, base + moveOff)
        _mracer_set(MRACER_RACERS, dir3, base + OFF_VELDIR)
        _mracer_set(MRACER_RACERS, struct.pack('<2f', 0.0, 0.0),
                    base + OFF_STEER_RATE)
    # s_race_yawWorld is still read back for logging; s_race_yawMove
    # is GONE - the old slip model that owned it was removed once
    # racer_integrate took over moveDir, so writing it now fails.
    _mracer_set(MRACER_YAW, struct.pack('<f', yawRad))


def mracer_drive(index):
    """(speed, gear) for the port, or (speed, None) on an old build."""
    stride, posOff, moveOff, segOff, speedOff, gearOff = \
        mracer_layout()
    if speedOff is None:
        return None, None
    speed = struct.unpack('<f', _mracer_get(MRACER_RACERS, 4,
                                            index * stride + speedOff))[0]
    gear = struct.unpack('<i', _mracer_get(MRACER_GEARBOX, 4,
                                           gearOff))[0]
    return speed, gear


def moto_segment_centre(seg):
    """MOTO's segment centre: origin PLUS the row-3 translation.

    segment_centre above returns the origin alone, which is not the
    same point - the grid builder, the autopilot and the racing line
    all work from origin + row3.  A finish placed on the bare origin
    sits off to one side of the road.
    """
    count, buf = _segments()
    o = struct.unpack_from("<3f", buf, seg * SEG_SIZE)
    m = struct.unpack_from("<12f", buf, seg * SEG_SIZE + 0x0c)
    return (o[0] + m[9], o[1] + m[10], o[2] + m[11])


def moto_race_laps():
    """g_nRaceLaps - the lap count this race was started with."""
    return struct.unpack("<i", _read(G_RACELAPS, 4))[0]


def moto_finish(index, laps, before, place):
    """The MOTO side of --finish, field for field with the port's.

    THE STRUCT IS BYTE-EXACT, so every offset here is the one the
    port uses; what differs is only how the bytes are reached -
    absolute addresses through the MOTO pipe against name+offset
    through the port's map.  See mracer_finish for what each write
    is for.
    """
    count, _ = _segments()
    reverse = struct.unpack("<i", _read(G_RACEREVERSE, 4))[0] != 0
    seg, face = _finish_where(count, before, reverse)

    cx, cy, cz = moto_segment_centre(seg)
    nx, ny, nz = moto_segment_centre(face)
    yaw = math.atan2(nx - cx, nz - cz)

    set_pos(index, (cx, cy, cz))
    # set_yaw's third argument is the ROW-3 TRANSLATION to keep, not
    # a flag - the forward-mode orientation carries the segment's
    # row 3 and the racer's heading is row3 + row2, so zeroing it
    # would turn the bike (game-object.txt 6.4).
    keep = struct.unpack("<3f", _read(racer_base(index) + OFF_MAT + 36, 12))
    set_yaw(index, yaw, keep)
    set_move(index, yaw)
    set_segment(index, seg)
    clear_crash(index)

    base = racer_base(index)
    _write(base + OFF_LAPCOUNTER, struct.pack("<h", max(0, laps - 1)))
    _write(base + OFF_RACESTATE, struct.pack("<i", 1))
    _write(base + OFF_FINISHPOS, struct.pack("<i", 0))

    signed = _finish_progress(seg, count, reverse)
    _write(base + OFF_PROG, struct.pack("<i", signed))
    _write(base + OFF_PROGHIGH, struct.pack("<i", signed))

    ahead = max(0, place - 1)
    mine = struct.unpack("<i", _read(base + OFF_RACECLOCK, 4))[0]
    clocks, mine = _finish_clocks(mine, ahead)
    if ahead:
        _write(base + OFF_RACECLOCK, struct.pack("<i", mine))
    for k in range(ahead):
        ob = racer_base(2 + k)
        _write(ob + OFF_ACTIVE, struct.pack("<i", 1))
        _write(ob + OFF_RACESTATE, struct.pack("<i", 2))
        _write(ob + OFF_LAPCOUNTER, struct.pack("<h", laps))
        _write(ob + OFF_RACECLOCK, struct.pack("<i", clocks[k]))
        _write(ob + OFF_FINISHPOS, struct.pack("<i", k + 1))

    print("finish: MOTO racer %d on segment %d of %d, %d short of the "
          "line, lap %d of %d" % (index, seg, count, before,
                                  max(0, laps - 1), laps))
    if ahead:
        suffix = {1: "st", 2: "nd", 3: "rd"}.get(place, "th")
        print("finish: MOTO racers 2..%d marked home, so this one should "
              "come %d%s" % (1 + ahead, place, suffix))
    print("finish: drive on - %s" % _finish_hint(reverse))


def trk_centres(track):
    """Segment centres straight out of the .trk FILE.

    NOT OUT OF EITHER PROCESS, and that is deliberate.  _segments()
    reads MOTO's table, so anything built on it needs MOTO running -
    and the finish set-up is a PORT-ONLY operation.  Both games load
    the same file, tools/trk.py already walks it, and a file read
    needs no game at all.

    Returns [(x, z), ...] - origin + row 3, the same centre the grid
    builder and the autopilot use.
    """
    import trk as _trk

    path = os.path.join(DATA_DIR, track, track + '.trk')
    r = _trk.Reader(open(path, 'rb').read())
    if r.take(4) != b'TRK\x00':
        raise SystemExit('%s: bad magic' % path)
    ver = r.i32()
    n = r.u32()
    out = []
    for _ in range(n):
        pos = struct.unpack('<3f', r.take(12))
        m = struct.unpack('<12f', r.take(48))
        r.take(4)
        r.take(64)
        r.take(4)
        if ver > 0:
            k = r.u32()
            r.take(k * 2)
        if ver > 3:
            r.take(8)
        out.append((pos[0] + m[9], pos[2] + m[11]))
    return out


def mracer_race_laps():
    """The lap count the race was started with - race.c s_race_laps."""
    return struct.unpack('<i', _mracer_get('s_race_laps', 4))[0]


def _finish_hint(reverse):
    """What the driver has to do, which differs by direction."""
    if reverse:
        return ("head DOWN the segment numbers through zero - "
                "the lap closes on the index going low then high")
    return ("head UP the segment numbers - the wrap past zero "
            "is the line")


def _finish_where(count, before, reverse):
    """(segment to stand on, segment to face) for a finish setup.

    REVERSE IS NOT THE FORWARD CASE MIRRORED, because the lap test
    is not either. Forward closes a lap on the segment index
    WRAPPING past zero, so the bike stands just below the top of
    the array and drives up through it. Reverse instead tests the
    two indices directly - previous below the midpoint and new one
    above it (race.c, 0x0047e50e) - so the bike stands a few
    segments ABOVE zero and drives DOWN through it.

    Placed the forward way in a reverse race the bike is at the
    far end of the lap facing the wrong way, and no amount of
    driving reaches the test.
    """
    if not reverse:
        seg = (count - before) % count
        return seg, (seg + 1) % count
    seg = before % count
    return seg, (seg - 1) % count


def _finish_progress(seg, count, reverse):
    """The progress seed, as racer_place_from_grid computes it."""
    if not reverse:
        return seg - count if seg >= count // 2 else seg
    return -seg if seg < count // 2 else count - seg


def _finish_clocks(mine, ahead):
    """Clocks for the opponents due home AHEAD, and the player's.

    THE FINISHED BUCKET SORTS ON raceClockMs, ASCENDING, so coming
    second means somebody else's clock has to be SMALLER than
    yours - and on a placed race yours is tiny, because the race
    only started moments ago. Fixed clocks of 60s and 61s were
    therefore slower than a player finishing at 35s, and
    --finish 3 handed out first place every time.

    They are spaced a second apart BELOW the player's clock, with
    a second of margin for the segments still to drive.

    Returns (clocks, playerClock). WHEN THE CLOCK IS TOO SMALL TO
    FIT THEM UNDER, the player's is raised to make room and comes
    back changed - there is no ordering that gives the asked-for
    place otherwise, and quietly returning the wrong one would
    look exactly like the bug this replaced.
    """
    step = 1000
    need = step * (ahead + 1)
    if mine < need:
        mine = need
    top = mine - step
    return ([top - (ahead - 1 - k) * step for k in range(ahead)], mine)


def _mracer_clock(index, stride):
    """The port racer's raceClockMs."""
    return struct.unpack("<i", _mracer_get(MRACER_RACERS, 4,
                         index * stride + OFF_RACECLOCK))[0]


G_RACEOVERHUD = 0x00509ba0     # MOTO's post-race HUD gate


def mracer_attract(index, laps, track):
    """Force the post-race sequence without driving a lap.

    Places the bike a couple of segments short of the line and then
    DECLARES it home in first, which is what arms the sequence:
    race_post_race_tick watches race_over_hud, and the auto-ride
    gate then wants solo, single-race and finishPosition 0.

    SINGLE RACE ONLY. The gate refuses practice and championship
    outright, so this does nothing useful from either - the check
    below says so rather than leaving it looking broken.
    """
    stride = mracer_layout()[0]
    mracer_finish(index, laps, 2, 1, track)

    base = index * stride
    _mracer_set(MRACER_RACERS, struct.pack("<i", 2),
                base + OFF_RACESTATE)
    _mracer_set(MRACER_RACERS, struct.pack("<i", 0),
                base + OFF_FINISHPOS)
    _mracer_set("s_race_overHud", struct.pack("<i", 1))
    print("attract: port racer %d declared home first - "
          "the sequence should arm within a frame" % index)


def moto_attract(index, laps):
    """The MOTO side of --attract. Same idea, absolute addresses."""
    moto_finish(index, laps, 2, 1)

    base = racer_base(index)
    _write(base + OFF_RACESTATE, struct.pack("<i", 2))
    _write(base + OFF_FINISHPOS, struct.pack("<i", 0))
    _write(G_RACEOVERHUD, struct.pack("<i", 1))
    print("attract: MOTO racer %d declared home first" % index)


def mracer_finish(index, laps, before, place, track):
    """Put the bike just short of the line on its last lap.

    THE LINE IS THE SEGMENT WRAP, not a plane in the track.
    race_update_progress closes a lap when the segment index rolls
    past zero, increments lapCounter, and finishes the racer when

        lapCounter == s_race_laps

    so a bike a few segments short of the end of the array with
    lapCounter one BELOW the count is one crossing from home. That
    is the only way to reach the finish camera, the victory pose and
    the results HUD without driving the whole race.

    place=N marks N-1 opponents as already home, so the player comes
    in Nth. race_compute_standings buckets on raceState and sorts
    the finished by raceClockMs - a smaller clock is further up the
    order - and race_finishing_position then just counts them. That
    is what makes every finishing scenario reachable, podium
    included.
    """
    stride, posOff, moveOff, segOff, speedOff, gearOff = mracer_layout()
    centres = trk_centres(track)
    total = len(centres)
    reverse = struct.unpack("<i", _mracer_get("g_reverseEnabled",
                                                4))[0] != 0
    seg, face = _finish_where(total, before, reverse)

    cx, cz = centres[seg]
    nx, nz = centres[face]
    yaw = math.atan2(nx - cx, nz - cz)

    # The y comes off the bike rather than the file - the centre pair
    # is flat and the ground height belongs to the segment planes.
    cy = struct.unpack('<3f', _mracer_get(MRACER_RACERS, 12,
                                          index * stride + posOff))[1]

    mracer_place(index, (cx, cy, cz), yaw, False)
    mracer_set_segment(index, seg)

    base = index * stride
    _mracer_set(MRACER_RACERS, struct.pack('<h', max(0, laps - 1)),
                base + OFF_LAPCOUNTER)
    _mracer_set(MRACER_RACERS, struct.pack('<i', 1), base + OFF_RACESTATE)
    _mracer_set(MRACER_RACERS, struct.pack('<i', 0), base + OFF_FINISHPOS)

    # THE PROGRESS PAIR HAS TO AGREE WITH THE SEGMENT, seeded exactly
    # as racer_place_from_grid seeds it - otherwise the very next
    # frame reads the jump we just made as a lap all by itself.
    signed = _finish_progress(seg, total, reverse)
    _mracer_set(MRACER_RACERS, struct.pack('<i', signed), base + OFF_PROG)
    _mracer_set(MRACER_RACERS, struct.pack('<i', signed), base + OFF_PROGHIGH)

    ahead = max(0, place - 1)
    clocks, mine = _finish_clocks(_mracer_clock(index, stride), ahead)
    if ahead:
        _mracer_set(MRACER_RACERS, struct.pack("<i", mine),
                    base + OFF_RACECLOCK)
    for k in range(ahead):
        ob = (2 + k) * stride
        _mracer_set(MRACER_RACERS, struct.pack('<i', 1), ob + OFF_ACTIVE)
        _mracer_set(MRACER_RACERS, struct.pack('<i', 2), ob + OFF_RACESTATE)
        _mracer_set(MRACER_RACERS, struct.pack('<h', laps),
                    ob + OFF_LAPCOUNTER)
        _mracer_set(MRACER_RACERS, struct.pack('<i', clocks[k]),
                    ob + OFF_RACECLOCK)
        _mracer_set(MRACER_RACERS, struct.pack('<i', k + 1),
                    ob + OFF_FINISHPOS)

    print('finish: racer %d on segment %d of %d, %d short of the line, '
          'lap %d of %d' % (index, seg, total, before, max(0, laps - 1),
                            laps))
    if ahead:
        suffix = {1: 'st', 2: 'nd', 3: 'rd'}.get(place, 'th')
        print('finish: racers 2..%d marked home, so this one should come '
              '%d%s' % (1 + ahead, place, suffix))
    print('finish: drive on - %s' % _finish_hint(reverse))


def mracer_set_segment(index, seg):
    """The port's segment and its search hint.

    BOTH, for the same reason MOTO needs both: UpdateGroundContact
    re-resolves from segmentStart, so setting only `segment` leaves
    the search starting from wherever the bike used to be. MR_Racer
    is byte-exact across the two, so OFF_SEG and OFF_SEGSTART serve
    here as well - the same reason OFF_ENGINE does.
    """
    stride = mracer_layout()[0]
    base = index * stride
    _mracer_set(MRACER_RACERS, struct.pack('<i', seg), base + OFF_SEG)
    _mracer_set(MRACER_RACERS, struct.pack('<i', seg), base + OFF_SEGSTART)


def mracer_clear_crash(index):
    stride = mracer_layout()[0]
    base = index * stride
    _mracer_set(MRACER_RACERS, struct.pack('<i', 0), base + OFF_CRASHLATCH)
    _mracer_set(MRACER_RACERS, struct.pack('<f', 0.0), base + OFF_CRASHTIMER)
    _mracer_set(MRACER_RACERS, struct.pack('<I', 0), base + OFF_RIDERBITS)
    _mracer_set(MRACER_RACERS, struct.pack('<f', 0.0), base + OFF_IMPACTBLEND)
    _mracer_set(MRACER_RACERS, struct.pack('<f', 0.0), base + OFF_LEAN)
    _mracer_set(MRACER_RACERS, struct.pack('<f', 0.0),
                base + OFF_WHEELIE_ANGLE)
    _mracer_set(MRACER_RACERS, struct.pack('<f', 0.0),
                base + OFF_WHEELIE_TIMER)
    _mracer_set(MRACER_RACERS, struct.pack('<f', 1.0),
                base + OFF_WHEELIE_SCALE)
    _mracer_set(MRACER_RACERS, struct.pack('<i', 0), base + OFF_STUNT_REQ)
    _mracer_set(MRACER_RACERS, struct.pack('<i', 0), base + OFF_STUNT_HELD)
    _mracer_set(MRACER_RACERS, struct.pack('<f', 0.0), base + OFF_AIRTIME)
    _mracer_set(MRACER_RACERS, struct.pack('<i', 1), base + OFF_ONGROUND)


def mracer_set_speed(index, v):
    stride, posOff, moveOff, segOff, speedOff, gearOff = \
        mracer_layout()
    if speedOff is None:
        raise RuntimeError('this build exports no speed offset - '
                           'rebuild for --speed')
    _mracer_set(MRACER_RACERS, struct.pack('<3f', v, v, v),
                index * stride + speedOff)
    _mracer_set(MRACER_RACERS, struct.pack('<f', 0.0),
                index * stride + OFF_ENGINE)
    #
    # BOTH COPIES ON THE PORT, and the RACER one is the one that
    # counts: race.c treats the racer as the state and s_race_drive
    # as its mirror, so zeroing the mirror alone is undone on the
    # next frame. Measured - the mirror read 0 and the racer read
    # 1320, and the brake did not ramp.
    #
    _mracer_set(MRACER_DRIVE, struct.pack('<i', 0),
                MRACER_DRIVE_BRAKEHOLD)
    _mracer_set(MRACER_RACERS, struct.pack('<i', 0),
                index * stride + OFF_BRAKEHOLD)


def mracer_set_gear(index, g):
    """THE PORT'S GEAR IS NOT racer+0x7c.

    That field exists in MR_Racer and nothing reads it, so writing it
    on this side is a silent no-op while the identical write on MOTO
    works. The port's gear lives in s_race_gearbox.
    """
    stride, posOff, moveOff, segOff, speedOff, gearOff = \
        mracer_layout()
    if gearOff is None:
        raise RuntimeError('this build exports no gear offset - '
                           'rebuild for --gear')
    _mracer_set(MRACER_GEARBOX, struct.pack('<i', g), gearOff)
    #
    # MR_Gearbox is gear, timerMs, state, mode, pending, savedEngine -
    # four bytes each - so the shift machinery sits just past the gear.
    #
    _mracer_set(MRACER_GEARBOX, struct.pack('<i', 0), gearOff + 4)
    _mracer_set(MRACER_GEARBOX, struct.pack('<i', 0), gearOff + 8)
    _mracer_set(MRACER_GEARBOX, struct.pack('<i', 0), gearOff + 16)


def verify_both(index):
    """Read the placement back from both games and say if it took.

    EVERY FALSE DEFECT THIS HARNESS HAS PRODUCED came from trusting a
    write that had not landed - a gear poked into a field nothing
    reads, a nowalls flag undone by the reset that followed it. So
    the placement is checked rather than assumed.
    """
    mpos, mmat, myaw = get_state(index)
    mspd, mgear = get_drive(index)
    ppos, pyaw, pmov, pseg = mracer_state(index)
    pspd, pgear = mracer_drive(index)

    bad = []
    for i, axis in enumerate('xyz'):
        if abs(mpos[i] - ppos[i]) > 0.5:
            bad.append('pos.%s %.3f vs %.3f' % (axis, mpos[i], ppos[i]))
    dyaw = (math.degrees(myaw - pyaw) + 540.0) % 360.0 - 180.0
    if abs(dyaw) > 0.5:
        bad.append('yaw %.3f deg apart' % dyaw)
    if pspd is not None and abs(mspd - pspd) > 0.05:
        bad.append('speed %.3f vs %.3f' % (mspd, pspd))
    if pgear is not None and mgear != pgear:
        bad.append('gear %d vs %d' % (mgear, pgear))

    #
    # AND THE SEGMENT, which is the one that got away. The two sides
    # were placed on 59 and 109 and this said MATCHED, because it
    # only compared what it had written rather than everything the
    # next frame reads. A wrong segment reads the ground height off
    # the wrong plane and the divergence looks like physics.
    #
    segStride = mracer_layout()[0]
    mseg = struct.unpack('<i', _read(racer_base(index) + OFF_SEG, 4))[0]
    pseg = struct.unpack('<i', _mracer_get(MRACER_RACERS, 4,
                                           index * segStride + OFF_SEG))[0]
    if mseg != pseg:
        bad.append('segment %d vs %d' % (mseg, pseg))

    mcr = struct.unpack('<i', _read(racer_base(index)
                                    + OFF_CRASHLATCH, 4))[0]
    pcr = struct.unpack('<i', _mracer_get(MRACER_RACERS, 4,
                                          index * segStride
                                          + OFF_CRASHLATCH))[0]
    if mcr or pcr:
        bad.append('crashLatch %d vs %d - a downed rider cannot '
                   'accelerate' % (mcr, pcr))

    mln = struct.unpack('<f', _read(racer_base(index) + OFF_LEAN, 4))[0]
    pln = struct.unpack('<f', _mracer_get(MRACER_RACERS, 4,
                                          index * segStride
                                          + OFF_LEAN))[0]
    if mln or pln:
        bad.append('pending yaw %.5f vs %.5f - it will steer the bike '
                   'with no input' % (mln, pln))

    mwh = struct.unpack('<f', _read(racer_base(index)
                                    + OFF_WHEELIE_ANGLE, 4))[0]
    pwh = struct.unpack('<f', _mracer_get(MRACER_RACERS, 4,
                                          index * segStride
                                          + OFF_WHEELIE_ANGLE))[0]
    if mwh or pwh:
        bad.append('wheelieAngle %.4f vs %.4f - it halves the steer '
                   'torque' % (mwh, pwh))

    meng = struct.unpack('<f', _read(racer_base(index) + OFF_ENGINE, 4))[0]
    stride = mracer_layout()[0]
    peng = struct.unpack('<f', _mracer_get(MRACER_RACERS, 4,
                                           index * stride + OFF_ENGINE))[0]
    if abs(meng - peng) > 0.05:
        bad.append('engineSpeed %.3f vs %.3f' % (meng, peng))

    if bad:
        print('  NOT MATCHED: ' + ('; '.join(bad)))
        return False
    print('  MATCHED  pos/yaw/speed/gear/engine agree on both sides')
    return True


def grid_copy():
    """Copy MOTO whole racer field onto the port.

    ONE RACER IS NOT ENOUGH FOR ANYTHING THAT READS THE FIELD.
    --to both places racer 1 and leaves the other twelve where each
    game happened to put them, which diverges immediately: MOTO runs
    13 racers and the port 3, tens of thousands of units apart. Any
    comparison of the cull, of the LOD of a non-local racer, or of
    the glance rival search is then measuring two different races
    rather than two implementations.

    MOTO IS THE SOURCE. The port count is set to match and every
    active racer is written with MOTO position, heading, travel
    direction and active flag.

    THE SEGMENT IS PART OF THE PLACEMENT, not an extra. The draw
    filters racers against the visible segment list, and that - not
    the cull - is what drops a racer who has pulled ahead. Copying
    position without segmentStart leaves the racer in a segment the
    other game is not listing, so it is silently never drawn and a
    comparison of anything downstream comes back empty.

    IT DOES NOT MAKE THE AI AGREE. The racers are placed alike for
    one frame; from the next tick each side steers its own. This is
    for measuring what the draw does with a given field, not for
    keeping two races in step.
    """
    count = struct.unpack("<i", _read(G_COUNT, 4))[0]
    if count < 1 or count > PORT_MAX:
        raise RuntimeError("MOTO racer count %d outside 1..%d"
                           % (count, PORT_MAX))

    stride, posOff, moveOff, segOff, speedOff, gearOff = \
        mracer_layout()
    placed = 0
    for i in range(1, count):
        base = racer_base(i)
        active = struct.unpack("<i", _read(base + OFF_ACT, 4))[0]
        pos = _read(base + OFF_POS, 12)
        mat = _read(base + OFF_MAT, 48)
        mov = _read(base + OFF_MOVE, 12)
        seg = _read(base + OFF_SEG, 4)
        pbase = i * stride
        _mracer_set(MRACER_RACERS, pos, pbase + posOff)
        _mracer_set(MRACER_RACERS, mat, pbase + OFF_MAT)
        _mracer_set(MRACER_RACERS, mov, pbase + moveOff)
        _mracer_set(MRACER_RACERS, seg, pbase + segOff)
        _mracer_set(MRACER_RACERS, struct.pack("<i", active),
                    pbase + OFF_ACT)
        if active:
            placed += 1

    _mracer_set(MRACER_COUNT, struct.pack("<i", count))
    print("grid: copied %d racers from MOTO, %d active, count now %d"
          % (count - 1, placed, count))

def mracer_report(index):
    try:
        pos, yaw, mov, seg = mracer_state(index)
    except Exception as exc:
        print('mracer: %s' % exc)
        return
    print('mracer racer %d' % index)
    print('  pos      %11.3f %9.3f %11.3f' % pos)
    print('  yaw      %.3f deg   (s_race_yawWorld)'
          % math.degrees(yaw))
    print('  travel   %.3f deg   (moveDir)' % math.degrees(mov))
    print('  segment  %d' % seg)


def report(index):
    pos, mat, yaw = get_state(index)
    print("racer %d at %08x" % (index, racer_base(index)))
    print("  pos      %11.3f %9.3f %11.3f" % pos)
    print("  forward  %11.6f %9.6f %11.6f" % (mat[6], mat[7], mat[8]))
    print("  yaw      %.3f deg" % math.degrees(yaw))
    print("  transl   %11.6f %9.6f %11.6f" % (mat[9], mat[10], mat[11]))

    w = walls(index)
    if w:
        seg, planes = w
        print("  segment  %d" % seg)
        for i, (n, dist) in enumerate(planes):
            print("  wall %d   n=(%7.4f %7.4f %7.4f)  %8.1f units away"
                  % (i, n[0], n[1], n[2], dist))
    print()
    print("  to record this as a spot, add to SPOTS in this file:")
    print('      "name": (%.1f, %.1f, %.1f, %.1f, "what it is for"),'
          % (pos[0], pos[1], pos[2], math.degrees(yaw)))


#
# THE CONTACT RADIUS, from maybe_Game_CollideRacers: it sweeps two
# spheres of 48 units each, so the pair touches at a centre distance of
# 96.  --pair places the two bikes a chosen gap apart so the sweep is
# guaranteed to cross that during one step.
#
COLLIDE_RADIUS = 48.0


def place_pair(index, other, gap, side, wantMoto, wantPort):
    """Put `other` a fixed distance from `index`, facing the same way.

    side "ahead" puts it down the bike's own forward axis, "left" and
    "right" put it alongside.  The two are left with the SAME heading,
    which is the case worth testing first: a rear-end shunt is the
    same-direction branch of the response and a head-on is not.
    """
    if wantMoto:
        pos, mat, yaw = get_state(index)
    else:
        pos, yaw, _m, _s = mracer_state(index)

    fwd = (math.sin(yaw), 0.0, math.cos(yaw))
    rgt = (math.cos(yaw), 0.0, -math.sin(yaw))
    axis = {"ahead": fwd, "behind": tuple(-v for v in fwd),
            "right": rgt, "left": tuple(-v for v in rgt)}[side]

    dest = [pos[i] + axis[i] * gap for i in range(3)]

    if wantMoto:
        #
        # ITS OWN TRANSLATION, NOT A FLAG. set_yaw rebuilds the whole
        # 3x4 and takes row 3 to put back; passing True made it try to
        # iterate a bool, which is what killed --pair the first time
        # it ever ran.
        #
        keep = get_state(other)[1][9:12]
        set_pos(other, dest)
        set_yaw(other, yaw, keep)
        set_move(other, yaw)
        seg = segment_for_point(dest)
        if seg is not None:
            set_segment(other, seg)
    if wantPort:
        mracer_place(other, dest, yaw, False)

    d = math.sqrt(sum((dest[i] - pos[i]) ** 2 for i in range(3)))
    print("  pair: racer %d at %s, racer %d %s by %.0f" %
          (index, " ".join("%.0f" % v for v in pos), other, side, gap))
    print("  centre distance %.0f, contact at %.0f - %s" %
          (d, COLLIDE_RADIUS * 2,
           "overlapping already" if d <= COLLIDE_RADIUS * 2
           else "closes when the gap is driven out"))
    print("  NOTE collision is OFF in practice mode - g_nSelectedMode")
    print("  must not be 0.  And a qualifying impact only CRASHES one")
    print("  time in four; the other three are a shove.")


def main():
    ap = argparse.ArgumentParser(
        description="place the bike for a repeatable impact test")
    ap.add_argument("--racer", type=int, default=1,
                    help="racer index; 1 is the local player")
    ap.add_argument("--yaw", type=float, help="absolute heading, degrees")
    ap.add_argument("--rel-yaw", type=float, help="turn by this many degrees")
    ap.add_argument("--at", type=float, nargs=3, metavar=("X", "Y", "Z"))
    ap.add_argument("--spot", help="a recorded spot from SPOTS")
    ap.add_argument("--list", action="store_true", help="show the spots")
    ap.add_argument("--keep-move", action="store_true",
                    help="leave the travel direction alone - for slides")
    ap.add_argument("--unstick", action="store_true",
                    help="UNTESTED: try to pull MOTO out of Game Over "
                         "in place rather than restarting it")
    ap.add_argument("--attract", action="store_true",
                    help="declare the bike home in first and let the "
                         "post-race sequence run - single race only")
    ap.add_argument("--finish", nargs="?", type=int, const=1,
                    metavar="PLACE",
                    help="put the bike just short of the line on its "
                         "last lap, so the next crossing finishes the "
                         "race. PLACE marks PLACE-1 opponents home "
                         "first, so the player comes in PLACE-th - use "
                         "it to reach every finish scenario. Port only.")
    ap.add_argument("--track", default="ile01",
                    help="track whose .trk --finish reads its segment "
                         "centres from; both games load the same file")
    ap.add_argument("--before", type=int, default=6, metavar="SEGS",
                    help="how many segments short of the line --finish "
                         "starts, default 6")
    ap.add_argument("--laps", type=int, default=0,
                    help="lap count to finish on; 0 reads the race's own")
    ap.add_argument("--grid", action="store_true",
                    help="copy MOTO whole racer field onto the port")
    ap.add_argument("--speed", type=float,
                    help="set speed, speedPrev and gearboxRoadSpeed")
    ap.add_argument("--gear", type=int,
                    help="set the gear - MOTO racer+0x7c, port "
                         "s_race_gearbox")
    ap.add_argument("--pair", type=int, metavar="OTHER",
                    help="place racer OTHER beside --racer for a "
                         "bike-to-bike collision test")
    ap.add_argument("--gap", type=float, default=120.0,
                    help="centre distance for --pair; contact is at 96")
    ap.add_argument("--side", default="ahead",
                    choices=("ahead", "behind", "left", "right"),
                    help="where --pair puts the other bike")
    ap.add_argument("--to", default="moto",
                    choices=("moto", "mracer", "both"),
                    help="which side to place; default moto")
    args = ap.parse_args()

    if args.list:
        for name in sorted(SPOTS):
            x, y, z, yaw, why = SPOTS[name]
            where = ("as reset" if x is None
                     else "%.0f %.0f %.0f yaw %.0f" % (x, y, z, yaw))
            print("  %-14s %-28s %s" % (name, where, why))
        return 0

    if args.grid:
        grid_copy()
        return 0

    wantMoto = args.to in ("moto", "both")
    wantPort = args.to in ("mracer", "both")

    if args.attract:
        if wantMoto:
            moto_attract(args.racer,
                         args.laps if args.laps > 0 else moto_race_laps())
        if wantPort:
            mracer_attract(args.racer,
                           args.laps if args.laps > 0
                           else mracer_race_laps(), args.track)
        return 0

    if args.finish is not None:
        laps = 0
        if wantPort:
            laps = args.laps if args.laps > 0 else mracer_race_laps()
        elif args.laps > 0:
            laps = args.laps
        else:
            laps = moto_race_laps()
        if laps <= 0:
            print("finish: the race reports %d laps - is one running?"
                  % laps)
            return 1
        if wantMoto:
            mlaps = args.laps if args.laps > 0 else moto_race_laps()
            moto_finish(args.racer, mlaps, args.before, args.finish)
        if wantPort:
            mracer_finish(args.racer, laps, args.before, args.finish,
                          args.track)
        return 0

    """
    THE NUMBERS COME FROM MOTO WHENEVER MOTO IS INVOLVED, so that
    --to both places the port at MOTO's own coordinates rather than at
    two independently-derived ones. With --to mracer the port supplies
    its own, which is what makes it usable when MOTO is not running.
    """
    if wantMoto:
        pos, mat, yaw = get_state(args.racer)
    else:
        pos, yaw, _mov, _seg = mracer_state(args.racer)
        mat = None

    newPos = list(pos)
    newYaw = yaw

    if args.spot:
        if args.spot not in SPOTS:
            print("no spot %r - try --list" % args.spot)
            return 2
        x, y, z, sy, _ = SPOTS[args.spot]
        if x is not None:
            newPos = [x, y, z]
            newYaw = math.radians(sy)

    if args.at:
        newPos = list(args.at)
    if args.yaw is not None:
        newYaw = math.radians(args.yaw)
    if args.rel_yaw is not None:
        newYaw = yaw + math.radians(args.rel_yaw)

    #
    # AN EXPLICIT REQUEST ALWAYS PLACES. This used to be the position
    # compare alone, against MOTO's current state - so running the
    # same --spot twice was a no-op, and because the compare is made
    # on the MOTO side it skipped THE PORT as well. A run that left
    # the two games disagreeing and reported NOT MATCHED was the tool
    # declining to do the one thing it was asked for.
    #
    asked = (args.spot is not None or args.at is not None
             or args.yaw is not None or args.rel_yaw is not None)
    moved = asked or (newPos != list(pos)) or (newYaw != yaw)

    pairAfter = args.pair

    if moved:
        if wantMoto:
            set_pos(args.racer, newPos)
            set_yaw(args.racer, newYaw, mat[9:12])
            if not args.keep_move:
                set_move(args.racer, newYaw)
        if wantPort:
            mracer_place(args.racer, newPos, newYaw, args.keep_move)
        print("placed racer %d in %s" % (args.racer, args.to))

    #
    # SPEED AND GEAR ARE INDEPENDENT OF `moved`. A test often wants
    # the bike left where it is and only the drive state reset, and
    # the placement above is skipped when nothing moved.
    #
    #
    # THE SEGMENT FOLLOWS THE POSITION. Worked out by sweeping the
    # track rather than asked of the game, because the game's own
    # search needs a hint that a jump has just invalidated.
    #
    if moved:
        #
        # THE PORT NEEDS IT TOO, and used to be left on whatever
        # segment the grid reset gave it. Two sides resolving from
        # different hints is the same defect as one side resolving
        # from a wrong one.
        #
        pos = get_state(args.racer)[0] if wantMoto else None
        if pos is None:
            pos = mracer_state(args.racer)[0]
        seg = segment_for_point(pos)
        if seg is not None:
            if wantMoto:
                set_segment(args.racer, seg)
            if wantPort:
                mracer_set_segment(args.racer, seg)
            print("  segment set to %d in %s" % (seg, args.to))
        else:
            print("  WARNING no segment contains that point - "
                  "the bike will fall through and read wall "
                  "distances in the tens of thousands")

        if wantMoto:
            clear_crash(args.racer)
        if wantPort:
            mracer_clear_crash(args.racer)
        print("  crash state cleared in %s" % args.to)

    if args.speed is not None:
        if wantMoto:
            set_speed(args.racer, args.speed)
        if wantPort:
            mracer_set_speed(args.racer, args.speed)
        print("speed set to %.3f in %s" % (args.speed, args.to))

    if args.gear is not None:
        if wantMoto:
            set_gear(args.racer, args.gear)
        if wantPort:
            mracer_set_gear(args.racer, args.gear)
        print("gear set to %d in %s" % (args.gear, args.to))

    #
    # THE CLOCK GUARD, on every placement, because the failure it
    # catches is SILENT: a timed-out MOTO still answers GET_MEM and
    # still accepts SEND_INPUT, it just does not advance the race.
    # See G_RACE_TIMER above.
    #
    if wantMoto:
        if args.unstick:
            race_unstick(args.racer)
            print("  UNSTICK applied - UNTESTED, see mr_place.py. If "
                  "MOTO still will not race, restart it.")
        out, st = race_timed_out(args.racer)
        if out:
            print("  *** MOTO HAS TIMED OUT - raceState 3, Game Over ***")
            print("      Every comparison from here is meaningless.")
            print("      Restart MOTO, or try --unstick.")
        elif st not in (0, 1):
            print("  note: MOTO raceState is %d, not 0 or 1 - not a "
                  "normal racing state." % st)
        else:
            applied, before, after = race_timer_topup()
            if applied:
                print("  race timer topped up: %d ms -> %d ms"
                      % (before, after))

    if wantMoto:
        report(args.racer)
    if wantPort:
        if wantMoto:
            print()
        mracer_report(args.racer)

    #
    # AND SAY WHETHER IT TOOK. Only --to both can check, because only
    # then are there two sides to compare.
    #
    matched = True
    if wantMoto and wantPort:
        print()
        matched = verify_both(args.racer)

    #
    # THE PAIR GOES IN EVEN WHEN THE VERIFY FAILED. It used to sit
    # after an early return, so any mismatch on racer 1 - including
    # one the placement was about to fix - silently skipped the second
    # bike and the tool looked like it had done nothing at all.
    #
    if pairAfter is not None:
        print()
        place_pair(args.racer, pairAfter, args.gap, args.side,
                   wantMoto, wantPort)

    return 0 if matched else 3


if __name__ == "__main__":
    try:
        sys.exit(main())
    except RuntimeError as exc:
        print("mr_place: %s" % exc)
        sys.exit(1)
    except Exception as exc:
        """
        A DOWN PIPE IS THE ORDINARY CASE, not a bug: one of the two
        games is usually not running. One line beats a traceback,
        because the traceback buries which SIDE failed.
        """
        print("mr_place: %s: %s" % (type(exc).__name__, exc))
        sys.exit(1)
