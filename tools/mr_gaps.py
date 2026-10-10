#!/usr/bin/env python3
"""mr_gaps.py - what in a live racer is NOT named by the map yet.

    python tools/mr_gaps.py

Snapshots the racer, drives 30 frames of acceleration, snapshots
again, and reports every run of bytes that is non-zero OR changing and
that nothing in mr_dump_racer's tables claims. It is the "what is left
to find" report, and it picks the next field to chase.

IT MARKS BYTES, NOT DWORDS, AND THAT IS THE WHOLE POINT. The racer
tail sits at 1 mod 4, so a sweep walking 4-byte boundaries reports the
byte BEFORE a known field as unknown and prints its value shifted into
nonsense. The first version did exactly that and overstated the gap by
a factor of three. A coverage tool that is one byte out is worse than
none: it invents work.

Fill a gap it reports by adding the field to SCALARS in
mr_dump_racer.py - all four tools read that one table.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.join('N:', os.sep, 'DEV', 'MOTORACER', 'tools'))
import bridge_mcp_mrhook as mr          # noqa: E402
import mr_dump_racer as dump            # noqa: E402


def coverage():
    """A bytearray with 1 where some known field lives."""
    m = bytearray(dump.STRIDE)

    def mark(off, n):
        for i in range(off, min(off + n, dump.STRIDE)):
            m[i] = 1

    for off, kind, _ in dump.SCALARS:
        mark(off, {"v": 12, "h": 2}.get(kind, 4))
    for off, _ in dump.MATRICES:
        mark(off, 48)
    mark(dump.INST_BASE, dump.INST_COUNT * dump.INST_SIZE)
    mark(dump.POSE_BASE, dump.POSE_SLOTS * dump.POSE_PARTS * dump.PART_SIZE)
    mark(0x2d4c, 60)                    # the three per-slot arrays
    mark(0x2dd4, 9)                     # the player name, and
    mark(0x2f89, 48 * 4)                # the checkpoint splits
    mark(0x2f79, 4)                     # two 16-bit counters
    mark(0x2ea9, 0x2f71 - 0x2ea9)       # bike shadow verts + faces
    mark(0x30ed, 0x319d - 0x30ed)       # rider shadow verts + faces
    mark(0x0018, 8)                     # the two FLT_MAX
    return m


def main():
    before, base = dump.snapshot(1)
    mr.pipe_command("SEND_INPUT 1 1e", 12000)      # 30 frames of accel
    after, _ = dump.snapshot(1)

    m = coverage()
    print("bytes the map claims: %d of %d (%.0f%%)\n"
          % (sum(m), dump.STRIDE, 100.0 * sum(m) / dump.STRIDE))

    runs = []
    o = 0
    while o < dump.STRIDE:
        interesting = (before[o] or before[o] != after[o]) and not m[o]
        if not interesting:
            o += 1
            continue
        start = o
        while o < dump.STRIDE and not m[o] and (
                before[o] or before[o] != after[o]):
            o += 1
        runs.append((start, o))

    print("  offset   bytes  moved   as u32       as float")
    total = 0
    for a, b in runs:
        total += b - a
        moved = before[a:b] != after[a:b]
        fits = a + 4 <= len(before)
        u = struct.unpack_from("<I", before, a)[0] if fits else 0
        f = struct.unpack_from("<f", before, a)[0] if fits else 0
        fs = "%14.4f" % f if 1e-9 < abs(f) < 1e12 else ""
        print("  +0x%04x  %5d  %-6s  %08x %s"
              % (a, b - a, "yes" if moved else "", u, fs))
    print("\n%d bytes unclaimed" % total)


main()
