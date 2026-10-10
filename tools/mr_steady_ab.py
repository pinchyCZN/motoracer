#!/usr/bin/env python3
"""mr_steady_ab.py - compare MOTO and the port at a STEADY STATE.

    python tools/mr_steady_ab.py --gear 6
    python tools/mr_steady_ab.py --gear 6 --modes 0,1
    python tools/mr_steady_ab.py --gear 1 --frames 240

WHY THIS EXISTS. mr_ab drives both games the same number of frames and
compares the tracks, which works until the experiment needs a button
PULSED on a particular frame - a gear change. The two harnesses do not
agree on when a held key first reaches the game, so a shift arms one
frame apart and every number after it is offset. See
whole-function-import.txt 6.3.

A FIXED POINT HAS NO SUCH PROBLEM. Hold the throttle in a fixed gear
long enough and the speed converges on the balance between torque and
drag, or on whichever clamp bites first. That value depends on the
bike, the gear and the gearbox mode - and NOT on which frame anything
arrived. So the test is: force the gear, hold z, wait, read.

NOTHING IS SHIFTED. The gear is WRITTEN on both sides, which is what
removes the pulse. That also means this measures one gear in
isolation rather than a drive through the box.

WHAT IT IS FOR. The manual gearbox differs from the automatic in four
ways and two of them show up here:

    torque x1.1         a higher terminal speed against the same drag
    realSpeedMax +2.0   a higher clamp, IF the clamp is reached at all

The other two - the 150/200 ms clutch delays and the multi-gear
downshift - are transient and need mr_ab.

FORCING A GEAR IS ONLY VALID IN MANUAL, OR ABOVE THE SHIFT POINT.
This is the trap the tool walks into if you let it. In AUTOMATIC the
requester runs every frame and downshifts as soon as roadSpeed falls
below T[gear-1] - 2, so a forced gear 6 at 40 m/s is measured in
whatever gear the game shifted to instead - and the column comes back
non-monotone, which looks like noise and is not.

    ile01 bike 0: T[5] = 72.9, so gear 6 in AUTOMATIC is only
    self-consistent above about 74 m/s.

So compare the two modes ABOVE the top shift point, or compare manual
against manual.

THE MODE SWITCH MOVES THE TORQUE GATE AND NOT THE CAP. --modes
rewrites racer+0x88, which is what Engine_TorqueAt reads, but it does
NOT re-derive racer+0xb4 - so both runs keep whatever cap the race was
loaded with. That is deliberate: holding the cap equal leaves the
1.1x torque as the only variable between the two columns, which is
what makes the comparison clean.

It also means this tool does NOT measure the +2.0 cap bonus. To do
that, write racer+0xb4 as well - and expect no change in gear 6 on
ile01 bike 0, where terminal speed is drag-limited at about 79.5 and
never reaches either 80 or 82.

READING THE RESULT. The delta column is the acceleration at that
speed. Where it crosses zero is the terminal speed for that gear and
mode. Manual accelerates harder everywhere - the 1.1x torque - and
the gap widens near the crossing, where net force is small and a ten
per cent torque change is a large relative one.

THE FIRST PROBE OF EVERY SWEEP IS UNRELIABLE - not just the first
after a reset. The bike needs a tick or two to settle into the segment
it was placed in, and until it has, the two sides disagree by of order
0.1. DISCARD ROW ONE OF EACH MODE.

It is worth being firm about this because the artefact is easy to
promote to a finding: a lone mismatched row in an otherwise exact
column looks like a real divergence at that speed. Re-run before
believing one. A gear 3 sweep showed manual disagreeing at 50 and
automatic agreeing there; the re-run had manual exact at 50 twice and
AUTOMATIC disagreeing at its own first row instead.
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

# racer offsets, byte-exact on both sides
OFF_GEAR = 0x007c
OFF_MODE = 0x0088
OFF_CAP = 0x00b4
OFF_SPEED = 0x0124
OFF_ENGINE = 0x00ec
OFF_ROADSPD = 0x012c


def le(val, fmt='<i'):
    return struct.pack(fmt, val).hex()


def moto_set(index, off, hexval):
    mr.pipe_command("SET_MEM %08x %s"
                    % (mr_place.racer_base(index) + off, hexval))


def moto_get(index, off, fmt='<f'):
    n = struct.calcsize(fmt)
    r = mr.pipe_command("GET_MEM %08x %d"
                        % (mr_place.racer_base(index) + off, n))
    return struct.unpack(fmt, bytes.fromhex(r.split()[-1]))[0]


def port_set(index, off, hexval):
    stride = mr_place.mracer_layout()[0]
    mr.pipe_command("SET_MEM name=s_race_racers value=%s offset=%d"
                    % (hexval, index * stride + off), target='mracer')


def port_get(index, off, fmt='<f'):
    n = struct.calcsize(fmt)
    stride = mr_place.mracer_layout()[0]
    reply = mr.pipe_command("GET_MEM name=s_race_racers len=%d offset=%d"
                            % (n, index * stride + off), target='mracer')
    if not reply.startswith('OK'):
        raise RuntimeError(reply.strip())
    return struct.unpack(fmt,
                         bytes.fromhex(reply.split(NEWLINE)[1].strip()))[0]


def port_set_speed(index, spd):
    """Write the port's speed pair in BOTH of its homes.

    THE PORT KEEPS A SEPARATE MR_Drivetrain and copies racer -> drive
    at the top of race_tick_physics and drive -> racer at the end.
    Writing only the racer is therefore a RACE: if the write lands
    after that tick's drive_load, the stale drivetrain value survives
    the frame and is stored straight back over it.  Measured - a
    racer written to 49.6 read 49.600 immediately and 69.341 one
    frame later, the drivetrain's own number.

    That is not a port bug; it is what a split struct costs a harness
    that writes while the game may be mid-frame.  Writing both homes
    makes the order stop mattering.

    It is also why a placement can look like it did nothing: mr_place
    zeroes the racer speeds and the drivetrain keeps the old ones.
    """
    hexspd = struct.pack('<f', spd).hex()
    port_set(index, OFF_SPEED, hexspd)
    port_set(index, OFF_ROADSPD, hexspd)
    # MR_Drivetrain: engineSpeed, airTime, brakeHoldMs, speed, road
    for off in (12, 16):
        mr.pipe_command("SET_MEM name=s_race_drive value=%s offset=%d"
                        % (hexspd, off), target='mracer')


def port_gearbox_set(idx, val):
    """MR_Gearbox is the port's own struct: gear, timer, state, mode."""
    mr.pipe_command("SET_MEM name=s_race_gearbox value=%s offset=%d"
                    % (le(val), idx * 4), target='mracer')


def setup(index, gear, mode):
    """Force BOTH sides into the same gear and gearbox mode."""
    for off, val in ((OFF_GEAR, gear), (OFF_MODE, mode),
                     (0x0080, 0), (0x0084, 0), (0x008c, 0)):
        moto_set(index, off, le(val))
        port_set(index, off, le(val))
    # the port reads the gearbox from its own struct, not the racer
    for i, val in ((0, gear), (1, 0), (2, 0), (3, mode), (4, 0)):
        port_gearbox_set(i, val)

    """
    AND CLEAR THE MANUAL SHIFT LATCH ON BOTH.

    0x004ea4c8 in MOTO, g_racerShiftLatch in the port: it is set when
    a gear button is seen and cleared only when NEITHER is held, which
    is what turns a held button into one shift instead of a shift a
    frame.  A run that ends with the button still down therefore
    leaves it SET, and the next run's first frame sees an already-held
    button and refuses to shift at all.

    Measured the hard way: two identical manual downshift runs, the
    first shifting and the second doing nothing.
    """
    mr.pipe_command("SET_MEM 004ea4c8 %s" % le(0))
    mr.pipe_command("SET_MEM name=g_racerShiftLatch value=%s" % le(0),
                    target='mracer')


# The port takes NAMES, MOTO takes the raw word - see mr_ab.keylist.
KEYBITS = (("left", 4), ("right", 8), ("z", 1), ("x", 2),
           ("c", 0x40), ("b", 0x10), ("v", 0x20))


def keymask(keys):
    """A comma list as MOTO's hold mask.

    THIS USED TO BE `1 if 'z' in keys else 0`, which silently sent
    MOTO NOTHING for any run that did not use the throttle - so a
    manual downshift test drove the port with v and MOTO with an
    empty word, and MOTO sat there looking like it had ignored the
    button.  Every key the port understands has to reach MOTO too.
    """
    mask = 0
    for tok in keys.split(","):
        tok = tok.strip()
        for name, bit in KEYBITS:
            if tok == name:
                mask |= bit
    return mask


def drive(keys, frames):
    mr.pipe_command("SEND_INPUT %x %d" % (keymask(keys), frames),
                    timeout_ms=60000)
    mr.pipe_command("SEND_INPUT keys=%s frames=%d" % (keys, frames),
                    target='mracer', timeout_ms=60000)


def read_both(index):
    a = (moto_get(index, OFF_SPEED), moto_get(index, OFF_ENGINE),
         moto_get(index, OFF_ROADSPD), moto_get(index, OFF_CAP))
    b = (port_get(index, OFF_SPEED), port_get(index, OFF_ENGINE),
         port_get(index, OFF_ROADSPD), port_get(index, OFF_CAP))
    return a, b


def probe(index, gear, mode, spd, frames, spot, eng):
    """Put the bike at speed spd in gear and measure what one short
    block of throttle does to it.  Returns (moto_delta, port_delta)."""
    x, y, z, yawDeg, _ = mr_place.SPOTS[spot]
    rad = math.radians(yawDeg)

    _, mat, _ = mr_place.get_state(index)
    mr_place.set_pos(index, [x, y, z])
    mr_place.set_yaw(index, rad, mat[9:12])
    mr_place.set_move(index, rad)
    mr_place.mracer_place(index, [x, y, z], rad, False)
    setup(index, gear, mode)

    """
    THE ENGINE IS THE INDEPENDENT VARIABLE, not the speed.

    The drivetrain re-derives roadSpeed from engineSpeed through the
    gear ratio on the next tick, so writing a speed and leaving the
    engine alone lets each side jump straight back to ITS OWN number.
    The probe then compares two different speeds and reports the
    difference as a physics residual.

    Setting engine as well is what restores the exact match: with the
    same engine and the same speed on both, the resync lands both in
    the same place.  See mr_clutch_ab, where the same omission made a
    port upshift look like a divergence.
    """
    hexeng = struct.pack('<f', eng).hex()
    moto_set(index, OFF_ENGINE, hexeng)
    port_set(index, OFF_ENGINE, hexeng)
    mr.pipe_command("SET_MEM name=s_race_drive value=%s offset=0"
                    % hexeng, target='mracer')

    hexspd = struct.pack('<f', spd).hex()
    for off in (OFF_SPEED, OFF_ROADSPD):
        moto_set(index, off, hexspd)
    port_set_speed(index, spd)

    a0, b0 = read_both(index)
    drive("z", frames)
    a1, b1 = read_both(index)
    return (a0, a1, b0, b1)


def main():
    ap = argparse.ArgumentParser(description="probe the drivetrain curve")
    ap.add_argument("--spot", default="straight")
    ap.add_argument("--gear", type=int, default=6)
    ap.add_argument("--modes", default="0,1")
    ap.add_argument("--frames", type=int, default=6,
                    help="short, so the bike never leaves the straight")
    ap.add_argument("--speeds", default="20,40,60,70,78,82",
                    help="m/s probe points")
    ap.add_argument("--engine", type=float, default=4.6,
                    help="engineSpeed written to both before each probe")
    ap.add_argument("--racer", type=int, default=1)
    args = ap.parse_args()

    """
    PROBING BEATS CONVERGING, and the first version of this script
    converged.  Holding top gear to its terminal speed takes hundreds
    of frames, and at 60 m/s the bike runs out of straight and crashes
    long before it settles - MOTO read 60, then 47, then 0.

    So sample the CURVE instead of its endpoint: set a speed, hold the
    throttle for a handful of frames, and read how much it moved.  The
    sign says which side of the fixed point that speed is on, and the
    magnitude is the acceleration there.  Six frames at 60 m/s travels
    about 450 units, which the straight swallows.

    IT IS STILL FRAME-ROBUST.  The throttle is held from the first
    frame of the block to the last, so there is no pulse to land on
    the right frame - the same reason the plateau test worked.
    """
    for mode in [int(m) for m in args.modes.split(",")]:
        print()
        print("=== gear %d, mode %d (%s), %d frames ==="
              % (args.gear, mode, "manual" if mode == 0 else "automatic",
                 args.frames))
        print("   from        MOTO to   d        port to   d      d(A-B)")
        for spd in [float(v) for v in args.speeds.split(",")]:
            a0, a1, b0, b1 = probe(args.racer, args.gear, mode, spd,
                                   args.frames, args.spot, args.engine)
            da = a1[0] - a0[0]
            db = b1[0] - b0[0]
            print("  %6.1f   %8.3f %7.3f   %8.3f %7.3f   %8.4f"
                  % (spd, a1[0], da, b1[0], db, da - db))


if __name__ == "__main__":
    main()
