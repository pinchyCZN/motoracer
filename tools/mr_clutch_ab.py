#!/usr/bin/env python3
"""mr_clutch_ab.py - measure the SHIFT TIMER on both games.

    python tools/mr_clutch_ab.py --gear 3 --speed 50.0          upshift
    python tools/mr_clutch_ab.py --gear 4 --speed 49.5 --coast  downshift
    python tools/mr_clutch_ab.py --gear 3 --speed 50.0 --mode 0 --keys z,b

WHAT IS BEING MEASURED. MR_Gearbox.timerMs is the clutch delay, and
it is the one thing that differs between the two gearbox modes in a
way a steady state cannot show:

    manual     upshift 150 ms, downshift 200 ms
    automatic  upshift 200 ms, downshift 300 ms

THE TRIGGER IS THE WHOLE TRICK. A manual shift needs a button pulsed
on a frame, and the two harnesses do not deliver a held key on the
same frame - mr_ab measured the arm one frame apart every time, which
offsets the whole countdown and makes the two look different when
they are not. See whole-function-import.txt 6.3.

SO USE THE AUTOMATIC REQUESTER AND PRESS NOTHING. Gearbox_RequestAuto
Shift fires by itself the moment roadSpeed crosses T[gear], and that
crossing is INTERNAL to each game. Both sides start from the same
speed, accelerate identically - which 6.4 established to four
decimals - and therefore cross at the same frame. No input is timed,
so no offset is possible.

    ile01 bike 0:  T[1] 17.8  T[2] 34.5  T[3] 50.9
                   T[4] 65.7  T[5] 72.9   downshift is T[gear-1] - 2

--coast releases the throttle so the bike decelerates through the
threshold below it instead, which is what exercises the DOWNSHIFT
timer.

MANUAL STILL NEEDS THE BUTTON, and --keys z,b holds it. The latch at
0x00466735 turns a held button into exactly ONE shift, so holding is
safe - but the arm lands one frame apart, so read the DECREMENT RATE
and extrapolate the start rather than trusting the first sample.
"""

import argparse
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bridge_mcp_mrhook as mr          # noqa: E402
import mr_place                          # noqa: E402
import mr_steady_ab as st                # noqa: E402

NEWLINE = chr(10)


def moto_gearbox(index):
    """gear, timer, state from the racer - byte-exact on MOTO."""
    base = mr_place.racer_base(index)
    r = mr.pipe_command("GET_MEM %08x 20" % (base + 0x7c))
    g, t, s, m, p = struct.unpack('<5i',
                                  bytes.fromhex(r.split()[-1]))
    return g, t, s


def port_gearbox():
    reply = mr.pipe_command("GET_MEM name=s_race_gearbox len=20",
                            target='mracer')
    if not reply.startswith('OK'):
        raise RuntimeError(reply.strip())
    g, t, s, m, p = struct.unpack(
        '<5i', bytes.fromhex(reply.split(NEWLINE)[1].strip()))
    return g, t, s


def main():
    ap = argparse.ArgumentParser(description="the clutch delay, both games")
    ap.add_argument("--spot", default="straight")
    ap.add_argument("--gear", type=int, default=3)
    ap.add_argument("--speed", type=float, default=50.0)
    ap.add_argument("--mode", type=int, default=1,
                    help="1 automatic - fires with no input at all")
    ap.add_argument("--keys", default="z")
    ap.add_argument("--coast", action="store_true",
                    help="no throttle, so it downshifts instead")
    ap.add_argument("--engine", type=float, default=4.9,
                    help="engineSpeed; the road speed follows from it")
    ap.add_argument("--frames", type=int, default=24)
    ap.add_argument("--racer", type=int, default=1)
    args = ap.parse_args()

    keys = "" if args.coast else args.keys

    x, y, z, yawDeg, _ = mr_place.SPOTS[args.spot]
    rad = math.radians(yawDeg)
    _, mat, _ = mr_place.get_state(args.racer)
    mr_place.set_pos(args.racer, [x, y, z])
    mr_place.set_yaw(args.racer, rad, mat[9:12])
    mr_place.set_move(args.racer, rad)
    mr_place.mracer_place(args.racer, [x, y, z], rad, False)
    st.setup(args.racer, args.gear, args.mode)

    """
    SET THE ENGINE, NOT JUST THE SPEED.

    The drivetrain RE-DERIVES roadSpeed from engineSpeed through the
    gear ratio on the very next tick, so a speed written on its own
    survives exactly zero frames: both games jumped off the written
    49.2 immediately, and to DIFFERENT values - 52.8 and 57.6 -
    because each still held its own engine speed.  That is not a
    divergence, it is the write being undone.

    engineSpeed is the independent variable here.  Give both sides
    the same one and the resync puts both on the same road speed,
    whatever that speed turns out to be - which is all this test
    needs, since it measures the TIMER and not the speed.
    """
    hexeng = struct.pack('<f', args.engine).hex()
    st.moto_set(args.racer, st.OFF_ENGINE, hexeng)
    st.port_set(args.racer, st.OFF_ENGINE, hexeng)
    mr.pipe_command("SET_MEM name=s_race_drive value=%s offset=0"
                    % hexeng, target='mracer')

    hexspd = struct.pack('<f', args.speed).hex()
    for off in (st.OFF_SPEED, st.OFF_ROADSPD):
        st.moto_set(args.racer, off, hexspd)
    st.port_set_speed(args.racer, args.speed)

    print("gear %d, mode %d (%s), from %.1f m/s, %s"
          % (args.gear, args.mode,
             "manual" if args.mode == 0 else "automatic",
             args.speed, "coasting" if args.coast else "on " + keys))
    print("                    MOTO.EXE           |"
          "            mracer")
    print("  frame   gear timer state   road    |"
          "  gear timer state   road")

    for f in range(args.frames):
        ag, at, as_ = moto_gearbox(args.racer)
        bg, bt, bs = port_gearbox()
        ar = st.moto_get(args.racer, st.OFF_ROADSPD)
        br = st.port_get(args.racer, st.OFF_ROADSPD)
        print("  %5d   %3d %5d %5d %7.3f    |  %3d %5d %5d %7.3f"
              % (f, ag, at, as_, ar, bg, bt, bs, br))
        st.drive(keys, 1)


if __name__ == "__main__":
    main()
