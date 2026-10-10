#!/usr/bin/env python3
"""mr_ab.py - run the SAME experiment on MOTO.EXE and on our port.

    python tools/mr_ab.py --spot square-right --frames 64 --step 8
    python tools/mr_ab.py --keys z --spot square-right

WHAT IT IS FOR. Every measurement in this project up to now has been
one-sided: read MOTO, reason about the port, guess. This places both
games identically, drives them the same number of frames with the same
input, and prints the two tracks beside each other with the deltas.

IT ONLY WORKS BECAUSE THE STRUCT IS BYTE-EXACT. MR_Racer carries the
DB's layout verbatim, so the same offset means the same field on both
sides and there is no translation table to get wrong. See
docs/whole-function-import.txt section 3.

THE FRAME COUNTS ARE NOT THE SAME THING, and this is the one trap.
MOTO's send_input runs N of ITS frames; ours runs N of ours, and the
two engines do not agree on what a frame is - MOTO's own harness
reports fewer frames run than asked when it clamps. So compare the
SHAPE of the two tracks, and treat a constant ratio between them as a
timebase difference rather than a physics difference.
"""

import argparse
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bridge_mcp_mrhook as mr          # noqa: E402
import mr_place                          # noqa: E402

NEWLINE = chr(10)

# Byte-exact, so one table serves both.
F = {
    'position':   (0x0030, '3f'),
    'moveDir':    (0x018c, '3f'),
    'speed':      (0x0124, '1f'),
    'forward':    (0x00d4, '3f'),
    'segment':    (0x31bd, '1i'),
    'onGround':   (0x2db8, '1i'),
    'crashLatch': (0x30c1, '1i'),
    'flLeanTgt':  (0x2e31, '1f'),
    'f_0180':     (0x0180, '1f'),
    'wheelie':    (0x307d, '1f'),
    'lean':       (0x2e1d, '1f'),
    'steering':   (0x0110, '1f'),
    'engineSpd':  (0x00ec, '1f'),
    'wheelTimer': (0x3081, '1f'),
    'stuntFlag':  (0x2e85, '1i'),
    'wheelScale': (0x00b8, '1f'),
    'front':      (0x01b4, '3f'),
    'rear':       (0x0338, '3f'),
    'suspAngle':  (0x2e8d, '1f'),
    'bounce':     (0x2e91, '1f'),
    'spring':     (0x2e95, '1f'),
    'phase':      (0x2e99, '1f'),
    'amplitude':  (0x2e9d, '1f'),
    'headSmooth': (0x2ea1, '1f'),

    # The gearbox block, racer+0x7c..0x8c, plus the two speeds
    # the clamps act on.  realSpdMax is the RACER copy - the one
    # manual bumps by 2.0 - not the carac field.
    'gear':       (0x007c, '1i'),
    'gbTimer':    (0x0080, '1i'),
    'gbState':    (0x0084, '1i'),
    'gbMode':     (0x0088, '1i'),
    'realSpdMax': (0x00b4, '1f'),
    'gbRoadSpd':  (0x012c, '1f'),

    # The brake block. brakeHold is the RAMP TIMER - the brake is
    # six times weaker for its first 200 ms - and the port keeps it
    # in s_race_drive, not the racer, so sample() overrides it the
    # way it does the gearbox. turnBrake GATES BRAKING outright.
    'turnBrake':  (0x00ac, '1i'),
    'brakeHold':  (0x00b0, '1i'),
    'speedPrev':  (0x0128, '1f'),
}


def moto_read(index, off, fmt):
    n = struct.calcsize('<' + fmt)
    addr = mr_place.racer_base(index) + off
    r = mr.pipe_command("GET_MEM %08x %d" % (addr, n))
    return struct.unpack('<' + fmt, bytes.fromhex(r.split()[-1]))


def port_read(index, off, fmt):
    n = struct.calcsize('<' + fmt)
    stride = mr_place.mracer_layout()[0]
    cmd = ("GET_MEM name=s_race_racers len=%d offset=%d"
           % (n, index * stride + off))
    reply = mr.pipe_command(cmd, target='mracer')
    if not reply.startswith('OK'):
        raise RuntimeError(reply.strip())
    return struct.unpack('<' + fmt,
                         bytes.fromhex(reply.split(NEWLINE)[1].strip()))


#
# BIT 6 IS THE STUNT BUTTON and it is NOT in the input word - the
# DLL routes it to action6Down instead. See mrhook.c. Our port
# takes it as the "c" key, which is the same key MOTO binds it to.
#
#
# b AND v ARE THE GEAR BUTTONS, bits 0x10 and 0x20 - upshift and
# downshift.  They do nothing in AUTOMATIC: the manual latch at
# 0x00466735 tests racer+0x88 before it requests, so a run with
# gbMode 1 will show them ignored on both sides.
#
# The letters are the PORT's binding.  MOTO binds Higher Gear to
# Z and Lower Gear to A, but the harness writes the input word
# directly, so only the bit matters here.
#
KEYBITS = (("left", 4), ("right", 8), ("z", 1), ("x", 2),
           ("c", 0x40), ("b", 0x10), ("v", 0x20))


def keymask(keys):
    """The port's key list as MOTO's hold mask."""
    rest = keys
    mask = 0
    for name, bit in KEYBITS:
        if name in rest:
            mask |= bit
            rest = rest.replace(name, "")
    return mask


def keylist(keys):
    """The same key string as the PORT wants it: COMMA separated.

    THE PORT PARSES NAMES AND MOTO TAKES A RAW WORD, and the two do
    not accept the same string.  agent.c splits keys= on commas, so
    "zb" is ONE unknown token and the whole SEND_INPUT is rejected -
    silently, because nothing here reads the reply.  "z" worked only
    because a single key happens to be a valid token on its own,
    which is why this went unnoticed until the first two-key run.

    A rejected run leaves the port sitting where it was while MOTO
    drives off, and that reads as the port ignoring the input.
    """
    rest = keys
    out = []
    for name, _ in KEYBITS:
        if name in rest:
            out.append(name)
            rest = rest.replace(name, "")
    return ",".join(out)


#
# THE STUNT BUTTON IS BIT 6 AND IT IS NOT IN THE INPUT WORD.
#
# The DLL writes stuntHeld (+0x31f9) and stuntPressed (+0x3201)
# for it, because Input_OnKeyDown case 6 does - see mrhook.c and
# docs/runtime-capture.txt 4.0.-1. mr_ab used to write those two
# fields itself, from before the DLL could; that is gone rather
# than left beside it, so the harness path is the one under test.
#
# A FLAT WHEELIE COLUMN IS USUALLY A CRASHED BIKE, not a dead
# button: once crashLatch is set, UpdateWheelie clears the angle,
# the timer and stuntHeld every frame. Read gnd/crash first.
#

def port_gearbox():
    """MR_Gearbox: gear, timerMs, state, mode, pending."""
    reply = mr.pipe_command("GET_MEM name=s_race_gearbox len=20",
                            target='mracer')
    if not reply.startswith('OK'):
        raise RuntimeError(reply.strip())
    raw = bytes.fromhex(reply.split(NEWLINE)[1].strip())
    return struct.unpack('<5i', raw)


def sample(index):
    a, b = {}, {}
    for name, (off, fmt) in F.items():
        a[name] = moto_read(index, off, fmt)
        b[name] = port_read(index, off, fmt)
    """
    THE PORT KEEPS THE SHIFT TIMER AND STATE IN ITS OWN STRUCT and
    mirrors only the GEAR back into the racer, so racer+0x80 and
    +0x84 read a flat 0/0 on our side however the shift is going.
    That is a reporting hole, not a stalled gearbox - it made a
    working shift look like a dead one.  Read them where they live.
    """
    gb = port_gearbox()
    b['gear'] = (gb[0],)
    b['gbTimer'] = (gb[1],)
    b['gbState'] = (gb[2],)
    b['gbMode'] = (gb[3],)
    """
    AND THE BRAKE RAMP TIMER LIVES IN s_race_drive TOO - racer+0xb0
    is dead on our side, so reading it there reports a flat 0 while
    the ramp is running. MR_Drivetrain is engineSpeed, airTime,
    brakeHoldMs, speed, gearboxRoadSpeed - four bytes each.
    """
    reply = mr.pipe_command("GET_MEM name=s_race_drive len=4"
                            " offset=8", target='mracer')
    if not reply.startswith('OK'):
        raise RuntimeError(reply.strip())
    b['brakeHold'] = struct.unpack(
        '<i', bytes.fromhex(reply.split(NEWLINE)[1].strip()))
    return a, b


def pitch(d):
    """The angle of the wheel-to-wheel line, in degrees."""
    v = [d['front'][i] - d['rear'][i] for i in range(3)]
    h = math.hypot(v[0], v[2])
    return math.degrees(math.atan2(v[1], h)) if h or v[1] else 0.0


def span(d):
    """The wheelbase, which a correct swing preserves exactly."""
    v = [d['front'][i] - d['rear'][i] for i in range(3)]
    return math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2])


def yaw(v):
    return math.degrees(math.atan2(-v[0], v[2]))


def show(tag, a, b):
    print("  %-9s %28s | %-28s" % (tag, "MOTO.EXE", "mracer"))
    print("  %-9s %28s | %-28s"
          % ("pos", "%9.1f %7.1f %9.1f" % a['position'],
             "%9.1f %7.1f %9.1f" % b['position']))
    print("  %-9s %28.2f | %-28.2f"
          % ("heading", yaw(a['forward']), yaw(b['forward'])))
    print("  %-9s %28.2f | %-28.2f"
          % ("travel", yaw(a['moveDir']), yaw(b['moveDir'])))
    print("  %-9s %28.3f | %-28.3f"
          % ("speed", a['speed'][0], b['speed'][0]))
    print("  %-9s %28.3f | %-28.3f"
          % ("slide", a['f_0180'][0], b['f_0180'][0]))
    print("  %-9s %28d | %-28d"
          % ("segment", a['segment'][0], b['segment'][0]))
    print("  %-9s %28s | %-28s"
          % ("gnd/crash", "%d / %d" % (a['onGround'][0], a['crashLatch'][0]),
             "%d / %d" % (b['onGround'][0], b['crashLatch'][0])))
    print("  %-9s %28.4f | %-28.4f"
          % ("wheelie", a['wheelie'][0], b['wheelie'][0]))
    print("  %-9s %28.4f | %-28.4f"
          % ("lean", a['lean'][0], b['lean'][0]))
    print("  %-9s %28.4f | %-28.4f"
          % ("steering", a['steering'][0], b['steering'][0]))
    print("  %-9s %28.1f | %-28.1f"
          % ("engineSpd", a['engineSpd'][0], b['engineSpd'][0]))
    print("  %-9s %28s | %-28s"
          % ("brk ms/gate", "%4d / %d" % (a['brakeHold'][0],
                                           a['turnBrake'][0]),
             "%4d / %d" % (b['brakeHold'][0], b['turnBrake'][0])))
    print("  %-9s %28.3f | %-28.3f"
          % ("speedPrev", a['speedPrev'][0], b['speedPrev'][0]))
    print("  %-9s %28s | %-28s"
          % ("gear t/s", "%d  %4d / %d" % (a['gear'][0],
                                           a['gbTimer'][0],
                                           a['gbState'][0]),
             "%d  %4d / %d" % (b['gear'][0], b['gbTimer'][0],
                               b['gbState'][0])))
    print("  %-9s %28s | %-28s"
          % ("mode/cap", "%d / %.2f" % (a['gbMode'][0],
                                        a['realSpdMax'][0]),
             "%d / %.2f" % (b['gbMode'][0],
                            b['realSpdMax'][0])))
    print("  %-9s %28.3f | %-28.3f"
          % ("gbRoadSpd", a['gbRoadSpd'][0], b['gbRoadSpd'][0]))
    print("  %-9s %28s | %-28s"
          % ("stunt f/t", "%d / %.1f" % (a['stuntFlag'][0],
                                         a['wheelTimer'][0]),
             "%d / %.1f" % (b['stuntFlag'][0], b['wheelTimer'][0])))
    print("  %-9s %28.3f | %-28.3f"
          % ("wheelScale", a['wheelScale'][0], b['wheelScale'][0]))
    """
    THE CONTACT PAIR IS THE WHEELIE POSE. The angle says how far
    the state machine has ramped; these two points say where the
    bike actually is. A matched angle with a rear contact off the
    ground is a bike pivoting on the wrong point - which is what a
    field-by-field comparison of the state machine cannot see.
    """
    print("  %-9s %28s | %-28s"
          % ("front y/z", "%9.2f %9.2f" % (a['front'][1],
                                           a['front'][2]),
             "%9.2f %9.2f" % (b['front'][1], b['front'][2])))
    print("  %-9s %28s | %-28s"
          % ("rear y/z", "%9.2f %9.2f" % (a['rear'][1], a['rear'][2]),
             "%9.2f %9.2f" % (b['rear'][1], b['rear'][2])))
    print("  %-9s %28.3f | %-28.3f"
          % ("pitchDeg", pitch(a), pitch(b)))
    print("  %-9s %28.3f | %-28.3f"
          % ("wheelbase", span(a), span(b)))
    print("  %-9s %28.4f | %-28.4f"
          % ("suspAngle", a['suspAngle'][0], b['suspAngle'][0]))
    print("  %-9s %28.4f | %-28.4f"
          % ("bounce", a['bounce'][0], b['bounce'][0]))
    print("  %-9s %28.4f | %-28.4f"
          % ("spring", a['spring'][0], b['spring'][0]))
    print("  %-9s %28.4f | %-28.4f"
          % ("amplitude", a['amplitude'][0], b['amplitude'][0]))
    print("  %-9s %28.4f | %-28.4f"
          % ("phase", a['phase'][0], b['phase'][0]))


def main():
    ap = argparse.ArgumentParser(description="the same run on both games")
    ap.add_argument("--spot", default="square-right")
    ap.add_argument("--frames", type=int, default=64)
    ap.add_argument("--step", type=int, default=16)
    ap.add_argument("--keys", default="z",
                    help="mracer keys; MOTO gets the matching bitmask")
    ap.add_argument("--pre-keys", default="",
                    help="held for --pre frames BEFORE the run proper")
    ap.add_argument("--pre", type=int, default=0,
                    help="warm-up frames, not sampled")
    ap.add_argument("--racer", type=int, default=1)
    args = ap.parse_args()

    mask = keymask(args.keys)
    preMask = keymask(args.pre_keys)

    """
    PLACE BOTH IN PROCESS, not by shelling out. The first version ran
    mr_place.py through os.system and the interpreter path has a space
    in it, so the placement silently failed and the two games ran from
    their own grids - which look like a physics difference and are not.
    """
    if args.spot not in mr_place.SPOTS:
        raise SystemExit("no spot %r - try mr_place.py --list" % args.spot)
    x, y, z, yawDeg, _ = mr_place.SPOTS[args.spot]
    if x is None:
        raise SystemExit("spot %r has no coordinates" % args.spot)
    pos = [x, y, z]
    rad = math.radians(yawDeg)

    print("placing both at %s: %.0f %.0f %.0f yaw %.0f"
          % (args.spot, x, y, z, yawDeg))
    _, mat, _ = mr_place.get_state(args.racer)
    mr_place.set_pos(args.racer, pos)
    mr_place.set_yaw(args.racer, rad, mat[9:12])
    mr_place.set_move(args.racer, rad)
    mr_place.mracer_place(args.racer, pos, rad, False)

    """
    AND ZERO THE SPEED ON BOTH SIDES. A placement moves the bike
    and leaves it travelling at whatever the last run left, so two
    games that have been driven different amounts start the warm-up
    from different speeds - which then reads as an acceleration
    difference and is not one. Measured: 34 against 64 km/h at the
    end of a supposedly matched 40-frame warm-up.

    speed (+0x124) and gearboxRoadSpeed (+0x12c) are the pair the
    drivetrain integrates; zeroing both is a standing start.
    """
    for off in (0x0124, 0x012c):
        mr.pipe_command("SET_MEM %08x 00000000"
                        % (mr_place.racer_base(args.racer) + off))
        stride = mr_place.mracer_layout()[0]
        mr.pipe_command("SET_MEM name=s_race_racers value=00000000"
                        " offset=%d" % (args.racer * stride + off),
                        target='mracer')
    """
    AND THE PORT'S OTHER COPY.  MR_Drivetrain is loaded from the racer
    at the top of race_tick_physics and stored back at the end, so
    zeroing only the racer is a race: if the write lands after that
    tick's drive_load the stale speed survives and is written straight
    back.  That is what a placement that "did nothing" looks like -
    one side starting a run at the previous run's plateau.

    engineSpeed goes too, or the first tick re-derives the road speed
    from it and undoes the standing start.
    """
    for off in (0, 12, 16):        # engineSpeed, speed, gearboxRoadSpeed
        mr.pipe_command("SET_MEM name=s_race_drive value=00000000"
                        " offset=%d" % off, target='mracer')
    mr.pipe_command("SET_MEM %08x 00000000"
                    % (mr_place.racer_base(args.racer) + 0x00ec))
    mr.pipe_command("SET_MEM name=s_race_racers value=00000000"
                    " offset=%d" % (args.racer * stride + 0x00ec),
                    target='mracer')

    """
    AND RESET THE GEARBOX, for exactly the reason the speeds are
    zeroed above.  A placement leaves the bike in whatever gear the
    last run left it in, and two sides in different gears produce
    two different acceleration curves from the same throttle - which
    reads as a physics difference and is not one.  It bites hardest
    in MANUAL, where nothing shifts back down on its own, so the
    gears only ever ratchet upward across runs.

    THE PORT KEEPS THE GEARBOX IN ITS OWN STRUCT, s_race_gearbox,
    not in the racer - so the two sides need different writes for
    the same four fields.  gear 1, timer 0, state 0, pending 0.
    """
    for off, val in ((0x007c, 1), (0x0080, 0), (0x0084, 0), (0x008c, 0)):
        mr.pipe_command("SET_MEM %08x %s"
                        % (mr_place.racer_base(args.racer) + off,
                           struct.pack('<i', val).hex()))
    """
    AND ZERO THE BRAKE RAMP TIMER. The brake is six times weaker for
    its first 200 ms, so a bike whose counter is already past 200
    brakes at full strength from frame one while a freshly-reset one
    ramps - which looks exactly like a constant force error. 660 ms
    was measured left over on the port. racers.txt 9.21.2.
    """
    mr.pipe_command("SET_MEM %08x 00000000"
                    % (mr_place.racer_base(args.racer) + 0x00b0))
    mr.pipe_command("SET_MEM name=s_race_drive value=00000000"
                    " offset=8", target='mracer')

    # MR_Gearbox: gear, timerMs, state, mode, pending - mode is left
    # alone, it is the AUTO/MANUAL choice and not run state.
    for idx, val in ((0, 1), (1, 0), (2, 0), (4, 0)):
        mr.pipe_command("SET_MEM name=s_race_gearbox value=%s offset=%d"
                        % (struct.pack('<i', val).hex(), idx * 4),
                        target='mracer')

    # our side needs one tick to resolve the segment it was placed in
    mr.pipe_command("SEND_INPUT keys= frames=1", target='mracer',
                    timeout_ms=8000)

    """
    THE WARM-UP EXISTS BECAUSE A WHEELIE NEEDS SPEED. Holding stunt
    from a standstill measures nothing, and accelerating INSIDE the
    sampled run mixes the ramp with the speed build-up. So the two
    games are driven the same number of frames on --pre-keys first,
    unsampled, and the run proper starts from a matched rolling
    state - which the frame 0 sample then shows.
    """
    """
    THE WARM-UP IS CHUNKED AT --step, NOT SENT AS ONE COMMAND.
    MOTO overruns a large frame batch and is exact on four or fewer
    (racers-convergence.txt 9.18.15), so one 60-frame warm-up put it
    about 1.6x further than the port and the run proper then started
    from 25.7 m/s against 17.8 - which reads as a steering difference
    for the whole arc, because the yaw fade is speed-dependent.
    """
    if args.pre:
        print("warm-up: %d frames on %r, in %d-frame batches"
              % (args.pre, args.pre_keys or "nothing", args.step))
        left = args.pre
        while left > 0:
            n = min(args.step, left)
            mr.pipe_command("SEND_INPUT %x %d" % (preMask, n),
                            timeout_ms=20000)
            mr.pipe_command("SEND_INPUT keys=%s frames=%d"
                            % (keylist(args.pre_keys), n),
                            target='mracer', timeout_ms=20000)
            left -= n

    done = 0
    while done <= args.frames:
        a, b = sample(args.racer)
        print()
        print("frame %d" % done)
        show(str(done), a, b)
        if done == args.frames:
            break
        n = min(args.step, args.frames - done)
        mr.pipe_command("SEND_INPUT %x %d" % (mask, n), timeout_ms=20000)
        mr.pipe_command("SEND_INPUT keys=%s frames=%d"
                        % (keylist(args.keys), n),
                        target='mracer', timeout_ms=20000)
        done += n
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        print("mr_ab: %s: %s" % (type(exc).__name__, exc))
        sys.exit(1)
