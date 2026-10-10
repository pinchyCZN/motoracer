#!/usr/bin/env python3
"""mr_steer_ab.py - front wheel PLACEMENT vs steering, MOTO against ours.

RUN ONE ANGLE PER INVOCATION for a result you can trust:

    python tools/mr_steer_ab.py --angles=" 600"

A multi-angle sweep is convenient but NOT reliable. Each row leaves the
bike turned and MOTO runs a minimum of two frames per request, so a row
occasionally draws at the forced angle instead of the decayed one and
reads several units out. A row whose two post-run steer values are not
99 apart did not tick the way the others did - discard it.

    python tools/mr_steer_ab.py
    python tools/mr_steer_ab.py --angles 0,-600,600

Both games must already be running with their mrhook agent up. See
tools/README.txt section 6.
"""
import argparse
import os
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bridge_mcp_mrhook as mr          # noqa: E402
import mr_place                         # noqa: E402

POS, MAT, MOVE = 0x30, 0xbc, 0x18c
ENG, SPD, GBX = 0xec, 0x124, 0x12c
STEER, FORK, SUSP = 0x0110, 0x2e39, 0x2e8d
CONTACT_FRONT = 0x1b4
AT = (-128.0, 0.0, 2058.0)
IDENT = (1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0)
HOOK = '0048c70e'                       # inside maybe_Inst_DrawWithMatrix
INST_FRONT = 0x1a8


def main():
    ap = argparse.ArgumentParser(description="front wheel vs steering")
    ap.add_argument("--racer", type=int, default=1)
    ap.add_argument("--angles", default="0,-300,-600,300,600")
    ap.add_argument("--settle", type=int, default=4)
    args = ap.parse_args()

    mo = mr_place.G_RACERS + args.racer * mr_place.STRIDE
    po = args.racer * mr_place.mracer_layout()[0]
    here = os.path.dirname(os.path.abspath(__file__))
    f = lambda v: struct.pack('<f', v)                       # noqa: E731

    def mset(o, d):
        mr.pipe_command("SET_MEM %08x %s" % (mo + o, d.hex()))

    def pset(o, d):
        mr.pipe_command("SET_MEM name=s_race_racers value=%s offset=%d"
                        % (d.hex(), po + o), target='mracer')

    def psym(name, n):
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
    for st in (mset, pset):
        st(POS, struct.pack('<3f', *AT))
        st(MAT, struct.pack('<12f', *[float(v) for v in IDENT]))
        st(MOVE, struct.pack('<3f', 0.0, 0.0, 1.0))
        st(ENG, f(0.0))
        st(SPD, f(0.0))
        st(GBX, f(0.0))
    for _ in range(args.settle):
        mr.pipe_command("SEND_INPUT 0 30", timeout_ms=30000)
        mr.pipe_command("SEND_INPUT keys= frames=30", target='mracer',
                        timeout_ms=30000)

    print("%-9s %-24s %-24s %s"
          % ("steering", "MOTO offset", "ours offset", "dmax"))
    print("-" * 74)
    bad = 0
    def mget(o, n):
        r = mr.pipe_command("GET_MEM %08x %d" % (mo + o, n))
        return bytes.fromhex(r.strip().split()[-1])

    def pget(o, n):
        r = mr.pipe_command("GET_MEM name=s_race_racers len=%d offset=%d"
                            % (n, po + o), target='mracer')
        return bytes.fromhex(r.strip().split(chr(10))[-1].strip())

    def steer_of(get):
        return struct.unpack('<f', get(STEER, 4))[0]

    for sv in [float(a) for a in args.angles.split(",")]:
        # FORCED, not driven - see README section 6.
        #
        # THE TICK DECAYS IT ONCE BEFORE THE DRAW, by the same 99 per
        # tick on both sides, so both DRAW at forced-99 and the
        # comparison is valid.
        #
        # THE LAST COLUMN IS NOT THE DRAWN ANGLE. MOTO runs a MINIMUM
        # OF TWO frames for a one-frame request, so by the time it is
        # read back it has ticked twice while ours ticked once - that
        # is the whole of the 402-against-501 it shows, and it is
        # harness accounting, not a physics divergence. The hook fires
        # on the FIRST draw, which is the one that matters.
        #
        # RE-PLACE BEFORE EVERY ROW. Each row ticks both games, which
        # moves the bike and its wheel contacts, and `reach` is
        # measured from a contact - so without this the rows drift and
        # later ones read worse than earlier ones purely from order.
        # That drift once looked like a left/right asymmetry.
        for st in (mset, pset):
            st(POS, struct.pack('<3f', *AT))
            st(MAT, struct.pack('<12f', *[float(v) for v in IDENT]))
            st(MOVE, struct.pack('<3f', 0.0, 0.0, 1.0))
            st(ENG, f(0.0))
            st(SPD, f(0.0))
            st(GBX, f(0.0))
        # ZERO THE STEERING FIRST. A row that ran at lock leaves the
        # bike turned, and re-placing the position does not undo the
        # steering angle - it decays at 99 a tick and needs the time.
        for st in (mset, pset):
            st(STEER, f(0.0))
        for _ in range(args.settle):
            mr.pipe_command("SEND_INPUT 0 30", timeout_ms=30000)
            mr.pipe_command("SEND_INPUT keys= frames=30", target='mracer',
                            timeout_ms=30000)
            for st in (mset, pset):
                st(POS, struct.pack('<3f', *AT))
                st(MAT, struct.pack('<12f',
                                    *[float(v) for v in IDENT]))
                st(MOVE, struct.pack('<3f', 0.0, 0.0, 1.0))
                st(SPD, f(0.0))
        for st in (mset, pset):
            st(STEER, f(sv))
            st(FORK, f(0.0))
            st(SUSP, f(0.0))
        mr.pipe_command('SET_HOOK 0 %s 7 2 ESP+c0:12' % HOOK)
        mr.pipe_command("SEND_INPUT 0 1", timeout_ms=30000)
        trace = mr.pipe_command('GET_TRACE')
        mr.pipe_command('CLEAR_HOOK')
        mr.pipe_command("SEND_INPUT keys= frames=1", target='mracer',
                        timeout_ms=30000)

        moto = None
        for part in trace.split('|')[1:]:
            if 'ebp=' not in part or 'c0=' not in part:
                continue
            if int(part.split('ebp=')[1].split()[0], 16) - mo != INST_FRONT:
                continue
            moto = struct.unpack(
                '<3f', bytes.fromhex(part.split('c0=')[1].split()[0]))
        raw = psym('g_rdFrontWheelOff', 12)
        if moto is None or raw is None:
            print("%-9.1f  no sample" % sv)
            continue
        ours = struct.unpack('<3f', raw)
        d = max(abs(a - b) for a, b in zip(moto, ours))
        if d > 1e-3:
            bad += 1
        sm2, sp2 = steer_of(mget), steer_of(pget)
        print("%-9.1f %7.3f %7.3f %7.3f  %7.3f %7.3f %7.3f  %8.4f"
              "   post-run steer %7.1f/%7.1f"
              % (sv, moto[0], moto[1], moto[2],
                 ours[0], ours[1], ours[2], d, sm2, sp2))
    print()
    print("Y must not move with steer - it is a rotation about the bike's")
    print("own Y axis. A drifting Y means reach is moving, which means the")
    print("fork pivot is turning when only the axis should.")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
