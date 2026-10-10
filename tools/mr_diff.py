#!/usr/bin/env python3
"""mr_diff.py - snapshot memory, do a thing, print only what changed.

    python tools/mr_diff.py snap                 baseline racer 1
    python tools/mr_diff.py diff                 what has changed since
    python tools/mr_diff.py run --frames 30 --hold accel
                                                 snap, drive, diff
    python tools/mr_diff.py snap --addr 5dabd8 --size 256
    python tools/mr_diff.py diff --min 4         only runs >= 4 bytes

THE STRUCT-DISCOVERY LOOP, MADE ONE COMMAND. Pause, snapshot, do
exactly one thing, snapshot again, and look at what moved. That is how
every field in MR_Racer.txt was found and it was hand-rolled every
time - two GET_MEM dumps into a scratch file and a Python one-liner to
compare them, re-typed from scratch on each question.

WHAT MAKES THE OUTPUT READABLE IS THE NAMING, NOT THE DIFF. A raw
byte diff of a racer is a wall of offsets. Every changed run here is
resolved against the block map first, so a change reads as

    slot 0 part 5 angles.y      rather than    +0x08a0

and a run that lands on a known scalar is named outright. The map
comes from mr_dump_racer, so the two tools cannot drift apart.

RUN MODE IS THE ONE THAT ANSWERS QUESTIONS. "run --frames 30 --hold
accel" pauses the game, snapshots, holds a key for a bounded number of
frames, pauses again and snapshots - so the ONLY thing that happened
between the two reads is the input asked for. Anything that changed is
downstream of it.

o A DIFF IS NOT A CAUSE. Thirty frames of anything moves the clock,
  the RNG and every physics scalar. Read the output as a candidate
  list to narrow, not as an answer - and prefer one frame over thirty
  when the question allows it.
"""

import json
import os
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bridge_mcp_mrhook as mr          # noqa: E402
import mr_dump_racer as dump            # noqa: E402

BASELINE = os.path.join(tempfile.gettempdir(), "mr_diff_baseline.json")

# SEND_INPUT bit names, from Input_OnKeyDown - see mrhook README 10.
HOLD_BITS = {
    "accel": 0x01, "brake": 0x02, "left": 0x04, "right": 0x08,
    "gearup": 0x10, "geardown": 0x20,
}

PART_FIELDS = (
    (0x00, 4, "nextSibling"), (0x04, 4, "parent"), (0x08, 4, "firstChild"),
    (0x0c, 12, "angles"), (0x18, 48, "matrix"), (0x48, 12, "pivot"),
    (0x54, 4, "firstVertex"), (0x58, 4, "vertexCount"),
    (0x5c, 4, "faceCount"), (0x60, 4, "faces"),
)

INST_FIELDS = (
    (0x00, 12, "rotation"), (0x0c, 12, "position"), (0x18, 12, "posPrev"),
    (0x24, 12, "posPrev2"), (0x34, 4, "pMesh"), (0x98, 48, "matrix"),
    (0x14c, 4, "detailLevel"),
)

AXIS = ".x", ".y", ".z"


# ------------------------------------------------------------------ #
# Naming an offset                                                    #
# ------------------------------------------------------------------ #

def field_of(off, table, size):
    """Name and component for an offset inside a record."""
    for base, width, name in table:
        if base <= off < base + width:
            if width in (12, 48) and name != "matrix":
                return name + AXIS[(off - base) // 4 % 3]
            if name == "matrix":
                return "matrix[%d]" % ((off - base) // 4)
            return name
    return "+0x%02x" % off


def describe(off):
    """Where an offset lands in the racer block map."""
    for soff, kind, what in dump.SCALARS:
        # A SCALAR IS FOUR BYTES UNLESS IT IS A VEC3. Using 12 for all
        # of them made every label in a dense run point one field too
        # low - +0x114 came out as "steering" when steering is +0x110.
        width = 12 if kind == "v" else 4
        if soff <= off < soff + width:
            if kind == "v":
                return what + AXIS[(off - soff) // 4]
            return what
    for moff, what in dump.MATRICES:
        if moff <= off < moff + 48:
            return "matrix[%d] of %s" % ((off - moff) // 4, what.split(" -")[0])

    lo = dump.INST_BASE
    hi = lo + dump.INST_COUNT * dump.INST_SIZE
    if lo <= off < hi:
        i = (off - lo) // dump.INST_SIZE
        return "instance %d (%s) %s" % (
            i, dump.INST_NAME[i],
            field_of((off - lo) % dump.INST_SIZE, INST_FIELDS,
                     dump.INST_SIZE))

    lo = dump.POSE_BASE
    hi = lo + dump.POSE_SLOTS * dump.POSE_PARTS * dump.PART_SIZE
    if lo <= off < hi:
        n = (off - lo) // dump.PART_SIZE
        return "slot %d part %-2d %s" % (
            n // dump.POSE_PARTS, n % dump.POSE_PARTS,
            field_of((off - lo) % dump.PART_SIZE, PART_FIELDS,
                     dump.PART_SIZE))

    if 0x2d4c <= off < 0x2d60:
        return "partCount[%d]" % ((off - 0x2d4c) // 4)
    if 0x2dd4 <= off < 0x2de8:
        return "PLAYER NAME, ASCII"
    if 0x2ea9 <= off < 0x2ef1:
        return "SHADOW vertex %d%s" % ((off - 0x2ea9) // 12,
                                       AXIS[(off - 0x2ea9) // 4 % 3])
    if 0x2ef1 <= off < 0x2f71:
        return "SHADOW face %d" % ((off - 0x2ef1) // 64)
    if 0x30ed <= off < 0x311d:
        return "rider shadow vertex %d" % ((off - 0x30ed) // 12)
    if 0x311d <= off < 0x319d:
        return "rider shadow face %d" % ((off - 0x311d) // 64)
    if off >= 0x2d88:
        return "tail"
    return ""


# ------------------------------------------------------------------ #
# Snapshots                                                           #
# ------------------------------------------------------------------ #

def take(racer, addr, size):
    if addr is None:
        data, base = dump.snapshot(racer)
        return data, base, racer
    return dump.read_range(addr, size), addr, None


def save(data, base, racer):
    with open(BASELINE, "w") as f:
        json.dump({"base": base, "racer": racer, "hex": data.hex()}, f)


def load():
    if not os.path.exists(BASELINE):
        sys.stderr.write("no baseline - run 'mr_diff.py snap' first\n")
        return None
    with open(BASELINE) as f:
        d = json.load(f)
    return bytes.fromhex(d["hex"]), d["base"], d["racer"]


# ------------------------------------------------------------------ #
# The diff                                                            #
# ------------------------------------------------------------------ #

def runs(old, new, minlen):
    """Contiguous changed spans, widened to 4-byte boundaries so a
    single changed byte still prints as the dword it belongs to."""
    out = []
    i = 0
    n = min(len(old), len(new))
    while i < n:
        if old[i] == new[i]:
            i += 1
            continue
        start = i & ~3           # align DOWN so a run starts on a dword
        while i < n and old[i] != new[i]:
            i += 1
        end = (i + 3) & ~3       # and align up
        if out and start <= out[-1][1]:
            out[-1] = (out[-1][0], max(out[-1][1], end))
        else:
            out.append((start, min(end, n)))
        i = end
    return [(a, b) for a, b in out if b - a >= minlen]


def show(old, new, base, racer, minlen):
    spans = runs(old, new, minlen)
    if not spans:
        print("nothing changed in %d bytes" % min(len(old), len(new)))
        return
    total = sum(b - a for a, b in spans)
    print("%d runs, %d bytes changed of %d" % (len(spans), total, len(new)))
    print()
    for a, b in spans:
        what = describe(a) if racer is not None else ""
        print("+0x%04x  %-42s %d bytes" % (a, what.rstrip(), b - a))
        for o in range(a, min(b, a + 48), 4):
            if o + 4 > len(new):
                break
            ov = struct.unpack_from("<I", old, o)[0]
            nv = struct.unpack_from("<I", new, o)[0]
            if ov == nv:
                continue
            of = struct.unpack_from("<f", old, o)[0]
            nf = struct.unpack_from("<f", new, o)[0]
            fs = ""
            # Show the float reading only when at least one side looks
            # like a real one - otherwise it is noise on an int field.
            if any(1e-6 < abs(v) < 1e9 for v in (of, nf)):
                fs = "   %12.4f -> %-12.4f" % (of, nf)
            print("          +0x%04x  %08x -> %08x  %11d -> %-11d%s"
                  % (o, ov, nv, struct.unpack_from("<i", old, o)[0],
                     struct.unpack_from("<i", new, o)[0], fs))
        if b - a > 48:
            print("          ... %d more bytes" % (b - a - 48))


# ------------------------------------------------------------------ #

def drive(frames, hold):
    mask = 0
    for name in hold:
        if name not in HOLD_BITS:
            sys.stderr.write("unknown hold %r, want one of %s\n"
                             % (name, ", ".join(sorted(HOLD_BITS))))
            return False
        mask |= HOLD_BITS[name]
    reply = mr.pipe_command("SEND_INPUT %x %x" % (mask, frames),
                            frames * 1000 // 60 + 8000)
    if not reply.startswith("OK"):
        sys.stderr.write("%s\n" % reply.strip())
        return False
    return True


def main():
    args = sys.argv[1:]
    if not args or args[0] not in ("snap", "diff", "run"):
        print(__doc__.strip().split("\n\n")[0])
        print(__doc__.strip().split("\n\n")[1])
        return 2
    mode = args.pop(0)

    racer, addr, size, minlen, frames, hold = 1, None, 256, 1, 1, []
    while args:
        a = args.pop(0)
        if a == "--racer":
            racer = int(args.pop(0))
        elif a == "--addr":
            addr = int(args.pop(0), 16)
        elif a == "--size":
            size = int(args.pop(0))
        elif a == "--min":
            minlen = int(args.pop(0))
        elif a == "--frames":
            frames = int(args.pop(0))
        elif a == "--hold":
            hold = args.pop(0).split(",")
        else:
            sys.stderr.write("unknown option %r\n" % a)
            return 2

    if mode == "snap":
        data, base, r = take(racer, addr, size)
        if data is None:
            return 1
        save(data, base, r)
        print("baseline: %d bytes at %08x%s"
              % (len(data), base, "" if r is None else " (racer %d)" % r))
        return 0

    if mode == "run":
        data, base, r = take(racer, addr, size)
        if data is None:
            return 1
        print("holding %s for %d frames" % (",".join(hold) or "nothing",
                                            frames))
        if not drive(frames, hold):
            return 1
        after, _, _ = take(racer, addr, size)
        if after is None:
            return 1
        show(data, after, base, r, minlen)
        return 0

    got = load()
    if got is None:
        return 1
    old, base, r = got
    new, _, _ = take(r if r is not None else 1, None if r is not None
                     else base, len(old))
    if new is None:
        return 1
    show(old, new, base, r, minlen)
    save(new, base, r)
    return 0


if __name__ == "__main__":
    sys.exit(main())
