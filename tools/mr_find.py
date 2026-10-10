#!/usr/bin/env python3
"""mr_find.py - find a value in a live racer, and say what it is.

    python tools/mr_find.py 84.0            float, default tolerance
    python tools/mr_find.py 3               int 3 AND float 3.0
    python tools/mr_find.py --int 12000     integer only
    python tools/mr_find.py --hex c3fb0000  an exact byte pattern
    python tools/mr_find.py --str DSI       an ASCII run
    python tools/mr_find.py --at 30c1       whatever is THERE, then
                                            every other copy of it
    python tools/mr_find.py 84.0 --all      every racer, not just 1
    python tools/mr_find.py 0.5 --addr 4d0e88 --size 256

"WHICH OFFSET HOLDS 84.0 RIGHT NOW" WAS THIRTEEN GET_MEM CALLS AND A
HAND-WRITTEN LOOP. It is how the instance stride, the wheel offsets
and the shadow lift biases were each pinned down, and it was retyped
every time.

IT SCANS EVERY BYTE OFFSET, NOT EVERY DWORD. That is the default on
purpose: the racer tail is byte-misaligned throughout - +0x2e1d,
+0x2e55, +0x3085, +0x319d are all 1 mod 4 - so a dword-aligned scan
misses most of the interesting half of the record. Pass --aligned to
restrict it when the noise is worse than the misses.

EVERY HIT IS NAMED through the same block map mr_diff uses, so a
result reads as "instance 1 (front wheel) position.y" rather than an
offset to go and look up.

o A MATCH IS A COINCIDENCE UNTIL IT REPEATS. Four bytes of anything
  will occasionally read as the float you asked for, and the tail has
  enough pointers to throw plausible-looking hits. Treat the output as
  candidates: the one that means something usually appears in a
  structure you recognise, or in the same place across two racers.
"""

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mr_dump_racer as dump            # noqa: E402
import mr_diff as diff                  # noqa: E402

# THIRTEEN SLOTS: 0 is the unused one, 1 is the local player, 2..12
# are the AI. --all was scanning to 7 and missing five racers, which
# is how a value the HUD was showing came back "0 hits".
RACER_MAX = 13
DEFAULT_TOL = 1e-4


def scan_float(b, target, tol, step):
    for o in range(0, len(b) - 3, step):
        v = struct.unpack_from("<f", b, o)[0]
        if v != v:                      # NaN never matches
            continue
        if abs(v - target) <= tol * max(1.0, abs(target)):
            yield o, "%.6f" % v


def scan_int(b, target, step):
    want = struct.pack("<i", target) if -2 ** 31 <= target < 2 ** 31 \
        else struct.pack("<I", target & 0xffffffff)
    for o in range(0, len(b) - 3, step):
        if b[o:o + 4] == want:
            yield o, "%d" % target


def scan_bytes(b, want, step):
    for o in range(0, len(b) - len(want) + 1, step):
        if b[o:o + len(want)] == want:
            yield o, want.hex()


def report(hits, b, base, named):
    n = 0
    for off, shown in hits:
        what = diff.describe(off) if named else ""
        print("  +0x%04x  %08x  %-16s %s"
              % (off, base + off, shown, what.rstrip()))
        n += 1
    return n


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__.strip().split("\n\n")[0])
        print(__doc__.strip().split("\n\n")[1])
        return 2

    literal = None
    kind = None
    racer = 1
    every = False
    addr = None
    size = 1024
    tol = DEFAULT_TOL
    step = 1
    at = None

    while args:
        a = args.pop(0)
        if a == "--int":
            kind, literal = "int", args.pop(0)
        elif a == "--hex":
            kind, literal = "hex", args.pop(0)
        elif a == "--str":
            kind, literal = "str", args.pop(0)
        elif a == "--at":
            at = int(args.pop(0), 16)
        elif a == "--racer":
            racer = int(args.pop(0))
        elif a == "--all":
            every = True
        elif a == "--addr":
            addr = int(args.pop(0), 16)
        elif a == "--size":
            size = int(args.pop(0))
        elif a == "--tol":
            tol = float(args.pop(0))
        elif a == "--aligned":
            step = 4
        elif a.startswith("--"):
            sys.stderr.write("unknown option %r\n" % a)
            return 2
        else:
            literal = a

    if literal is None and at is None:
        sys.stderr.write("nothing to look for\n")
        return 2

    targets = list(range(1, RACER_MAX)) if every else [racer]
    if addr is not None:
        targets = [None]

    total = 0
    for index in targets:
        if index is None:
            b, base, named = dump.read_range(addr, size), addr, False
            title = "%08x, %d bytes" % (addr, size)
        else:
            b, base = dump.snapshot(index)
            named, title = True, "racer %d" % index
        if b is None:
            return 1

        # --at reads the value THERE and then looks for it everywhere
        # else. That is how an alias is found - the speed at +0x124 and
        # its copy at +0x2da8 are the same number in two places.
        if at is not None:
            want = b[at:at + 4]
            fv = struct.unpack_from("<f", b, at)[0]
            iv = struct.unpack_from("<i", b, at)[0]
            print("%s: +0x%04x holds %s  = %d  = %sf"
                  % (title, at, want.hex(), iv,
                     "%.6f" % fv if abs(fv) < 1e9 else "?"))
            hits = [(o, s) for o, s in scan_bytes(b, want, step) if o != at]
            print("  %d other copies:" % len(hits))
            total += report(hits, b, base, named)
            continue

        print("%s:" % title)
        if kind == "hex":
            hits = scan_bytes(b, bytes.fromhex(literal), step)
        elif kind == "str":
            hits = scan_bytes(b, literal.encode("ascii"), 1)
        elif kind == "int":
            hits = scan_int(b, int(literal, 0), step)
        else:
            # A BARE LITERAL IS TRIED BOTH WAYS unless it has a dot.
            # "3" as an int and 3.0 as a float are different bit
            # patterns and the caller rarely knows which one they want.
            hits = list(scan_float(b, float(literal), tol, step))
            if "." not in literal and "e" not in literal.lower():
                try:
                    hits += list(scan_int(b, int(literal, 0), step))
                except ValueError:
                    pass
                hits.sort()
        total += report(hits, b, base, named)

    print("%d hit%s" % (total, "" if total == 1 else "s"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
