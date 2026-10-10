#!/usr/bin/env python3
"""mr_pose_ab.py - diff the posed skeleton, MOTO against our port.

    python tools/mr_pose_ab.py
    python tools/mr_pose_ab.py --settle 8 --racer 1

Both games must already be running with the mrhook agent up. See
tools/README.txt section 4.
"""
import argparse
import os
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bridge_mcp_mrhook as mr          # noqa: E402
import mr_place                         # noqa: E402

POS, MAT, MOVE, ENG, SPD, GBX = 0x30, 0xbc, 0x18c, 0xec, 0x124, 0x12c
POSE, PARTS, REC = 0x63c, 17, 100
A_ANG, A_MAT, A_PIV = 0x0c, 0x18, 0x48

AT = (-128.0, 0.0, 2058.0)
IDENT = (1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0)
NAMED = {15: "front bodywork/fork", 16: "rear wheel"}


def main():
    ap = argparse.ArgumentParser(description="pose A/B, MOTO vs port")
    ap.add_argument("--racer", type=int, default=1)
    ap.add_argument("--settle", type=int, default=5,
                    help="rounds of 30 frames to settle before reading")
    args = ap.parse_args()

    mo = mr_place.G_RACERS + args.racer * mr_place.STRIDE
    # mracer_layout() returns (stride, posOff, moveOff, segOff); the
    # port's stride differs from MOTO's, so index with its own.
    po = args.racer * mr_place.mracer_layout()[0]

    def mset(o, d):
        mr.pipe_command("SET_MEM %08x %s" % (mo + o, d.hex()))

    def pset(o, d):
        mr.pipe_command("SET_MEM name=s_race_racers value=%s offset=%d"
                        % (d.hex(), po + o), target='mracer')

    def mget(o, n):
        r = mr.pipe_command("GET_MEM %08x %d" % (mo + o, n))
        return bytes.fromhex(r.strip().split()[-1])

    def pget(o, n):
        r = mr.pipe_command("GET_MEM name=s_race_racers len=%d offset=%d"
                            % (n, po + o), target='mracer')
        return bytes.fromhex(r.strip().split(chr(10))[-1].strip())

    subprocess.run([sys.executable,
                    os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                 'mr_reset.py')], capture_output=True)
    mr.pipe_command("SEND_INPUT keys= frames=1 tap=2", target='mracer',
                    timeout_ms=20000)
    mr.pipe_command("SEND_INPUT keys= frames=2", target='mracer',
                    timeout_ms=20000)
    mr.pipe_command("SEND_INPUT 0 2")

    z = struct.pack('<f', 0.0)
    for setter in (mset, pset):
        setter(POS, struct.pack('<3f', *AT))
        setter(MAT, struct.pack('<12f', *[float(v) for v in IDENT]))
        setter(MOVE, struct.pack('<3f', 0.0, 0.0, 1.0))
        setter(ENG, z)
        setter(SPD, z)
        setter(GBX, z)

    mp = struct.unpack('<3f', mget(POS, 12))
    pp = struct.unpack('<3f', pget(POS, 12))
    ms = struct.unpack('<f', mget(SPD, 4))[0]
    ps = struct.unpack('<f', pget(SPD, 4))[0]
    print("start  MOTO pos %9.3f %7.3f %9.3f  speed %.4f" % (mp + (ms,)))
    print("       ours pos %9.3f %7.3f %9.3f  speed %.4f" % (pp + (ps,)))
    if max(abs(a - b) for a, b in zip(mp, pp)) > 0.01 or abs(ms - ps) > 0.01:
        print("START STATE MISMATCH - aborting")
        return 1
    print("       matched.")
    print()

    # headingSmoothed converges by delta/retardCadre and ours starts
    # off-target, so a read one frame after the reset compares a
    # transient rather than the model.
    for _ in range(args.settle):
        mr.pipe_command("SEND_INPUT 0 30", timeout_ms=30000)
        mr.pipe_command("SEND_INPUT keys= frames=30", target='mracer',
                        timeout_ms=30000)

    m = mget(POSE, PARTS * REC)
    p = pget(POSE, PARTS * REC)
    print("%-5s %-11s %-11s %s"
          % ("part", "angle dmax", "pivot dmax", "MATRIX dmax"))
    print("-" * 46)
    worst = 0.0
    for i in range(PARTS):
        o = i * REC

        def dmax(off, n):
            a = struct.unpack_from('<%df' % n, m, o + off)
            b = struct.unpack_from('<%df' % n, p, o + off)
            return max(abs(x - y) for x, y in zip(a, b))

        da, dv, dm = dmax(A_ANG, 3), dmax(A_PIV, 3), dmax(A_MAT, 12)
        worst = max(worst, dm)
        tail = "   <-- " + NAMED[i] if i in NAMED else ""
        print("%-5d %11.5f %11.5f %11.5f%s" % (i, da, dv, dm, tail))
    print()
    print("worst matrix dmax %.5f" % worst)
    return 0


if __name__ == "__main__":
    sys.exit(main())
