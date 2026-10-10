#!/usr/bin/env python3
"""mr_levelcam.py - hold the side camera level through a wheelie.

    python tools/mr_levelcam.py            level it until Ctrl-C
    python tools/mr_levelcam.py --seconds 20
    python tools/mr_levelcam.py --off      one pass, restore nothing

THE SIDE CAMERA ROLLS WITH THE BIKE because Camera_UpdateChase builds
its view from racer+0x2e55, not from matBikeRoot. Measured through a
wheelie at wheelieAngle pi/2: matBikeRoot m[7] stays 0.00000 and
racer+0x2e55 m[7] reads 0.70711. The look-slot path at 0x0046f8a4
copies that matrix, offsets the eye through it, composes
pLookAngles[slot] onto it and inverts - so every bit of pitch and roll
in it ends up in the view.

WHAT THIS DOES: rewrites racer+0x2e55 as a pure yaw each poll, keeping
its translation. The camera reads it fresh every frame, so a poll is
enough - the same reason mrhook can re-assert the look slot from a
thread rather than a hook.

IT IS A PROBE, NOT A SETTING. maybe_Racer_Draw copies racer+0x2e55 at
0x0045f2e5 into the slot that becomes rearMat, so levelling it may
move the bike's own geometry as well as the camera. Watch the bike,
not just the horizon, and see docs/subsystems/camera.txt.
"""
import argparse
import math
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bridge_mcp_mrhook as mr          # noqa: E402
import mr_place                         # noqa: E402

OFF_CAMMAT = 0x2e55


def read_mat(base):
    reply = mr.pipe_command("GET_MEM %08x 48" % (base + OFF_CAMMAT))
    return struct.unpack('<12f', bytes.fromhex(reply.strip().split()[-1]))


def write_level(base, mat):
    """Same yaw, same translation, no pitch and no roll."""
    yaw = math.atan2(mat[2], mat[8])
    c, s = math.cos(yaw), math.sin(yaw)
    out = [c, 0.0, s,
           0.0, 1.0, 0.0,
           -s, 0.0, c,
           mat[9], mat[10], mat[11]]
    mr.pipe_command("SET_MEM %08x %s"
                    % (base + OFF_CAMMAT, struct.pack('<12f', *out).hex()))
    return math.degrees(yaw)


def main():
    ap = argparse.ArgumentParser(description="keep the side camera level")
    ap.add_argument("--racer", type=int, default=1)
    ap.add_argument("--seconds", type=float, default=0.0,
                    help="stop after this long; 0 means until Ctrl-C")
    ap.add_argument("--off", action="store_true",
                    help="report the matrix and change nothing")
    args = ap.parse_args()

    base = mr_place.G_RACERS + args.racer * mr_place.STRIDE
    mat = read_mat(base)
    print("racer %d  racer+0x2e55" % args.racer)
    print("  rot %s" % " ".join("%8.4f" % v for v in mat[:9]))
    print("  pos %s" % " ".join("%9.3f" % v for v in mat[9:]))
    if args.off:
        return 0

    print("levelling - Ctrl-C to stop")
    started = time.time()
    n = 0
    try:
        while True:
            mat = read_mat(base)
            write_level(base, mat)
            n += 1
            if args.seconds and (time.time() - started) >= args.seconds:
                break
    except KeyboardInterrupt:
        pass
    print("%d passes in %.1f s" % (n, time.time() - started))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        print("mr_levelcam: %s: %s" % (type(exc).__name__, exc))
        sys.exit(1)
