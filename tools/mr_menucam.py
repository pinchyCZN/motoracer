#!/usr/bin/env python3
"""mr_menucam.py - freeze and single-step the page 10 bike spin.

    python tools/mr_menucam.py                 report, change nothing
    python tools/mr_menucam.py --pause         stop the turntable
    python tools/mr_menucam.py --resume        let it spin again
    python tools/mr_menucam.py --step 5        advance 5 degrees
    python tools/mr_menucam.py --step -2       and back

WHAT ACTUALLY SPINS. Nothing about the bike moves. The model sits
still and the CAMERA orbits it - MenuPage10_BikeSelect_Tick advances
camera.flLookYaw once per frame:

    if (g_nBikeSpinDir == 0) {
        dt = timeGetTime() - g_dwBikeSpinLast;   clamped to 1000
        g_dwBikeSpinLast = timeGetTime();
        camera.flLookYaw += dt * 0.000613592;    rad per ms
    }

so a full turn takes 10.2 seconds at 35.16 degrees a second.

PAUSING THE VIRTUAL CLOCK WOULD NOT STOP IT. The spin reads
timeGetTime() directly, not the game clock mrhook's virtual clock
replaces. That is why this is a poke and not a clock change.

SPIN DIR 2 IS THE PAUSE, and it is not a hack - it is the one value
the page's own branches all decline. 0 spins, +1 and -1 mean a bike
change is sliding, and every pan test is an equality against those
three. 2 falls through to LAB_0044efa3 without advancing the yaw and
without starting a slide, and nothing resets it. Restoring 0 resumes
from where it stopped, because the timestamp is only updated inside
the branch that was skipped.

WHY A SCRIPT AND NOT A HOTKEY IN THE DLL. The same rule mr_cheats.py
and mr_freeze.py follow: GET_MEM and SET_MEM already express this, so
adding an mrhook keybind would mean editing mrhook.c, rebuilding,
redeploying and restarting the game to change a step size.

ADDRESSES [verified, read live on page 10]:

    0x005420d0   g_nBikeSpinDir      0 spin, 2 paused, +-1 sliding
    0x005161fc   camera.flLookYaw    MR_Camera +0x3c, radians
    0x004d0c58   the rate, a DOUBLE  0.000613592 rad/ms

The camera record is g_abCameraRecords[g_nActiveCamera] and lands at
0x005161c0 on this page. IT IS NOT A FIXED ADDRESS in general - the
index at 0x004dc0f8 selects it - so this reads the index rather than
assuming. See docs/subsystems/menu-pages.txt.
"""

import argparse
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bridge_mcp_mrhook as mr          # noqa: E402

ADDR_SPIN_DIR = 0x005420D0
ADDR_CAM_BASE = 0x005161C0     # g_abCameraRecords[0]
ADDR_CAM_INDEX = 0x004DC0F8
CAMERA_STRIDE = 568            # sizeof(MR_Camera)
OFF_LOOK_YAW = 0x3C

SPIN_RUNNING = 0
SPIN_PAUSED = 2


def _read(addr, size):
    reply = mr.pipe_command("GET_MEM %08x %d" % (addr, size))
    if not reply.startswith("OK"):
        return None
    return bytes.fromhex(reply.split()[-1])


def _write(addr, raw):
    cmd = "SET_MEM %08x %s" % (addr, raw.hex())
    return mr.pipe_command(cmd).startswith("OK")


def yaw_address():
    """The active camera's flLookYaw, not a hardcoded slot."""
    raw = _read(ADDR_CAM_INDEX, 4)
    if raw is None:
        return None
    index = struct.unpack("<i", raw)[0]
    if index < 0 or index > 8:
        print("camera index %d is out of range - is the game on page 10?"
              % index)
        return None
    return ADDR_CAM_BASE + index * CAMERA_STRIDE + OFF_LOOK_YAW


def read_state():
    addr = yaw_address()
    if addr is None:
        return None, None, None
    dir_raw = _read(ADDR_SPIN_DIR, 4)
    yaw_raw = _read(addr, 4)
    if dir_raw is None or yaw_raw is None:
        return None, None, None
    return (addr,
            struct.unpack("<i", dir_raw)[0],
            struct.unpack("<f", yaw_raw)[0])


def cmd_status():
    addr, spin, yaw = read_state()
    if addr is None:
        return 1
    name = {0: "spinning", 2: "PAUSED"}.get(spin, "sliding (bike change)")
    print("  g_nBikeSpinDir  %d   %s" % (spin, name))
    print("  flLookYaw       %.6f rad   %.3f deg   at %08x"
          % (yaw, math.degrees(yaw), addr))
    return 0


def cmd_pause(paused):
    addr, spin, yaw = read_state()
    if addr is None:
        return 1
    if spin not in (SPIN_RUNNING, SPIN_PAUSED):
        print("  a bike change is sliding (dir %d) - let it finish first"
              % spin)
        return 1
    want = SPIN_PAUSED if paused else SPIN_RUNNING
    if not _write(ADDR_SPIN_DIR, struct.pack("<i", want)):
        print("  write failed")
        return 1
    print("  %s" % ("paused" if paused else "spinning again"))
    return 0


def cmd_step(degrees):
    addr, spin, yaw = read_state()
    if addr is None:
        return 1
    if spin != SPIN_PAUSED:
        # Stepping while it spins would be overwritten within the frame.
        print("  not paused - pausing first")
        if cmd_pause(True):
            return 1
        addr, spin, yaw = read_state()
    new = yaw + math.radians(degrees)
    if not _write(addr, struct.pack("<f", new)):
        print("  write failed")
        return 1
    print("  %+.3f deg   %.3f -> %.3f deg"
          % (degrees, math.degrees(yaw), math.degrees(new)))
    return 0


def main():
    ap = argparse.ArgumentParser(
        description="freeze and step the page 10 bike turntable")
    ap.add_argument("--pause", action="store_true")
    ap.add_argument("--resume", action="store_true")
    ap.add_argument("--step", type=float, metavar="DEG")
    args = ap.parse_args()

    if args.pause and args.resume:
        print("pick one of --pause and --resume")
        return 2
    if args.pause:
        return cmd_pause(True)
    if args.resume:
        return cmd_pause(False)
    if args.step is not None:
        return cmd_step(args.step)
    return cmd_status()


if __name__ == "__main__":
    sys.exit(main())
