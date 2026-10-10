#!/usr/bin/env python3
"""mr_wheel_ab.py - diff the wheel draw matrices, MOTO against our port.

    python tools/mr_wheel_ab.py
    python tools/mr_wheel_ab.py --rear 0.7 --front 1.3

Both games must already be running with their mrhook agent up. See
tools/README.txt section 5.
"""
import argparse
import math
import os
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bridge_mcp_mrhook as mr          # noqa: E402
import mr_place                         # noqa: E402

POS, MAT, MOVE, ENG, SPD, GBX = 0x30, 0xbc, 0x18c, 0xec, 0x124, 0x12c
OFF_REAR, OFF_FRONT = 0x2e3d, 0x2e49
AT = (-128.0, 0.0, 2058.0)
IDENT = (1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0)
HOOK_INST_DRAW = '0048c70e'             # inside maybe_Inst_DrawWithMatrix


def main():
    ap = argparse.ArgumentParser(description="wheel matrix A/B")
    ap.add_argument("--racer", type=int, default=1)
    ap.add_argument("--rear", type=float, default=0.7)
    ap.add_argument("--front", type=float, default=1.3)
    ap.add_argument("--settle", type=int, default=4)
    args = ap.parse_args()

    mo = mr_place.G_RACERS + args.racer * mr_place.STRIDE
    po = args.racer * mr_place.mracer_layout()[0]
    here = os.path.dirname(os.path.abspath(__file__))

    def mset(o, d):
        mr.pipe_command("SET_MEM %08x %s" % (mo + o, d.hex()))

    def pset(o, d):
        mr.pipe_command("SET_MEM name=s_race_racers value=%s offset=%d"
                        % (d.hex(), po + o), target='mracer')

    def psym(name, n):
        """None when the symbol is absent - an older build lacks it."""
        r = mr.pipe_command("GET_MEM name=%s len=%d offset=0" % (name, n),
                            target='mracer')
        try:
            return bytes.fromhex(r.strip().split(chr(10))[-1].strip())
        except ValueError:
            return None

    subprocess.run([sys.executable, os.path.join(here, 'mr_reset.py')],
                   capture_output=True)
    mr.pipe_command("SEND_INPUT keys= frames=1 tap=2", target='mracer',
                    timeout_ms=20000)
    mr.pipe_command("SEND_INPUT keys= frames=2", target='mracer',
                    timeout_ms=20000)
    mr.pipe_command("SEND_INPUT 0 2")

    z = struct.pack('<f', 0.0)
    for st in (mset, pset):
        st(POS, struct.pack('<3f', *AT))
        st(MAT, struct.pack('<12f', *[float(v) for v in IDENT]))
        st(MOVE, struct.pack('<3f', 0.0, 0.0, 1.0))
        st(ENG, z)
        st(SPD, z)
        st(GBX, z)
    for _ in range(args.settle):
        mr.pipe_command("SEND_INPUT 0 30", timeout_ms=30000)
        mr.pipe_command("SEND_INPUT keys= frames=30", target='mracer',
                        timeout_ms=30000)

    # THE SPIN IS FORCED EQUAL rather than driven to it - see README.
    for st in (mset, pset):
        st(OFF_REAR, struct.pack('<3f', args.rear, 0.0, 0.0))
        st(OFF_FRONT, struct.pack('<3f', args.front, 0.0, 0.0))

    mr.pipe_command('SET_HOOK 0 %s 7 2 EBX+0:32,EBX+20:16' % HOOK_INST_DRAW)
    mr.pipe_command("SEND_INPUT 0 1", timeout_ms=30000)
    trace = mr.pipe_command('GET_TRACE')
    mr.pipe_command('CLEAR_HOOK')
    mr.pipe_command("SEND_INPUT keys= frames=1", target='mracer',
                    timeout_ms=30000)

    moto = {}
    for part in trace.split('|')[1:]:
        if 'ebp=' not in part or 'c0=' not in part:
            continue
        ebp = int(part.split('ebp=')[1].split()[0], 16)
        raw = (bytes.fromhex(part.split('c0=')[1].split()[0])
               + bytes.fromhex(part.split('c1=')[1].split()[0]))
        moto[{0x1a8: 'front', 0x32c: 'rear'}.get(ebp - mo, '?')] = \
            struct.unpack('<12f', raw)

    ours = {}
    for nm, sym in (('rear', 'g_rdRearMat'),
                    ('front', 'g_rdFrontWheelMat')):
        raw = psym(sym, 48)
        if raw is not None:
            ours[nm] = struct.unpack('<12f', raw)

    print("spin forced   rear %.4f   front %.4f rad" % (args.rear, args.front))
    print()
    print("%-6s %-11s %-11s %s" % ("wheel", "MOTO angle", "ours angle",
                                   "ROTATION dmax"))
    print("-" * 48)
    bad = 0
    for nm in ('rear', 'front'):
        if nm not in moto:
            print("%-6s  no MOTO hit - is the Fx path on?" % nm)
            continue
        if nm not in ours:
            print("%-6s  our mirror is missing - rebuild Debug" % nm)
            continue
        m, o = moto[nm], ours[nm]
        am = math.atan2(-m[5], m[4])
        ao = math.atan2(-o[5], o[4])
        d = max(abs(a - b) for a, b in zip(m[:9], o[:9]))
        if d > 1e-5:
            bad += 1
        print("%-6s %11.5f %11.5f %13.6f" % (nm, am, ao, d))
    print()
    def spin(get):
        return (struct.unpack('<f', get(OFF_REAR, 4))[0],
                struct.unpack('<f', get(OFF_FRONT, 4))[0])

    def mget(o, n):
        r = mr.pipe_command("GET_MEM %08x %d" % (mo + o, n))
        return bytes.fromhex(r.strip().split()[-1])

    def pget(o, n):
        r = mr.pipe_command("GET_MEM name=s_race_racers len=%d offset=%d"
                            % (n, po + o), target='mracer')
        return bytes.fromhex(r.strip().split(chr(10))[-1].strip())

    def pos(get, o):
        return struct.unpack('<3f', get(o, 12))

    def drive(held, keys, n):
        mr.pipe_command("SEND_INPUT %d %d" % (held, n), timeout_ms=30000)
        mr.pipe_command("SEND_INPUT keys=%s frames=%d" % (keys, n),
                        target='mracer', timeout_ms=30000)

    # ---- the rate, per unit of distance: timebase-free ----
    m0, p0 = spin(mget), spin(pget)
    mp0, pp0 = pos(mget, POS), pos(pget, POS)
    drive(1, 'z', 25)
    m1, p1 = spin(mget), spin(pget)
    md = math.sqrt(sum((a - b) ** 2 for a, b in zip(pos(mget, POS), mp0)))
    pd = math.sqrt(sum((a - b) ** 2 for a, b in zip(pos(pget, POS), pp0)))
    print()
    print("%-6s %-14s %-14s %s" % ("wheel", "MOTO rad/unit",
                                   "ours rad/unit", "rel err"))
    print("-" * 54)
    for i, nm in ((0, 'rear'), (1, 'front')):
        a = (m1[i] - m0[i]) / md if md else 0.0
        b = (p1[i] - p0[i]) / pd if pd else 0.0
        e = abs(a - b) / abs(a) if a else 0.0
        if e > 0.01:
            bad += 1
        print("%-6s %14.8f %14.8f %8.4f%%" % (nm, a, b, e * 100))

    # ---- the brake gate: bit 1 must freeze both while still sliding ----
    m0, p0 = spin(mget), spin(pget)
    drive(2, 'x', 20)
    m1, p1 = spin(mget), spin(pget)
    print()
    print("braking, spin must not advance:")
    print("   MOTO rear %+.5f front %+.5f | ours rear %+.5f front %+.5f"
          % (m1[0] - m0[0], m1[1] - m0[1], p1[0] - p0[0], p1[1] - p0[1]))
    for v in (m1[0] - m0[0], m1[1] - m0[1], p1[0] - p0[0], p1[1] - p0[1]):
        if abs(v) > 1e-4:
            bad += 1

    raw = psym('g_rdRearFrameW', 48)
    if raw is not None:
        fw = struct.unpack('<12f', raw)
        off = max(abs(fw[i] - IDENT[i]) for i in range(9))
        print("rearFrameW off identity by %.6f   (must stay ~0: the "
              "frame is spin-free)" % off)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
