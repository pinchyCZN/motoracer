#!/usr/bin/env python3
"""
bkf.py -- reader for the MotoRacer .bkf asset bank.

Layout, derived from the files themselves:

    u32          entry count
    entry[count] 44 bytes each:
                     char name[36]   NUL-terminated, may hold garbage
                                     after the terminator
                     u32  offset     absolute, from start of file
                     u32  size
    ...          member data

The directory is 4 + count * 44 bytes, which is always the first
member's offset. Names may carry a subdirectory with a backslash,
for example BANK\\BNK00_02.RAW.

Usage:
    python tools/bkf.py --list PATH [PATH ...]
    python tools/bkf.py --check PATH [PATH ...]     structural checks
    python tools/bkf.py --extract PATH OUTDIR
    python tools/bkf.py --verify PATH REFDIR        byte-compare members
"""

import argparse
import os
import struct
import sys

ENTRY = 44
NAMELEN = 36


def read_dir(path):
    """Return (count, [(name, offset, size)], filesize)."""
    data = open(path, "rb").read()
    count = struct.unpack_from("<I", data, 0)[0]
    if count <= 0 or count > 100000:
        raise ValueError("implausible entry count %d" % count)
    need = 4 + count * ENTRY
    if need > len(data):
        raise ValueError("directory of %d entries overruns file" % count)
    out = []
    for i in range(count):
        base = 4 + i * ENTRY
        raw = data[base:base + NAMELEN]
        nul = raw.find(b"\x00")
        name = (raw if nul < 0 else raw[:nul]).decode("latin-1")
        off, size = struct.unpack_from("<II", data, base + NAMELEN)
        out.append((name, off, size))
    return count, out, len(data), data


def check(paths):
    bad = 0
    for p in paths:
        name = p.replace("\\", "/").split("/")[-1]
        try:
            count, ents, fsize, _ = read_dir(p)
        except Exception as e:
            print("%-16s FAIL  %s" % (name, e))
            bad += 1
            continue

        problems = []
        dirsize = 4 + count * ENTRY
        if ents and ents[0][1] != dirsize:
            problems.append("first offset %d != dir size %d"
                            % (ents[0][1], dirsize))
        # members should tile the file with no gaps and no overlap
        cur = dirsize
        for n, off, size in ents:
            if off != cur:
                problems.append("%s at %d, expected %d" % (n, off, cur))
                cur = off
            cur += size
        if cur != fsize:
            problems.append("members end at %d, file is %d" % (cur, fsize))
        if any(size < 0 for _, _, size in ents):
            problems.append("negative size")

        total = sum(s for _, _, s in ents)
        print("%-16s %s %4d entries  dir=%-6d data=%-9d %s"
              % (name, "OK  " if not problems else "FAIL", count, dirsize,
                 total, "; ".join(problems[:3])))
        if problems:
            bad += 1
    print("\n%d bank(s) failed." % bad)
    return 1 if bad else 0


def listing(paths):
    for p in paths:
        count, ents, fsize, _ = read_dir(p)
        print("== %s  (%d entries, %d bytes)" % (p, count, fsize))
        for n, off, size in ents:
            print("   %-32s %9d %9d" % (n, off, size))


def extract(path, outdir):
    count, ents, fsize, data = read_dir(path)
    n = 0
    for name, off, size in ents:
        rel = name.replace("\\", os.sep)
        dest = os.path.join(outdir, rel)
        d = os.path.dirname(dest)
        if d and not os.path.isdir(d):
            os.makedirs(d)
        open(dest, "wb").write(data[off:off + size])
        n += 1
    print("extracted %d member(s) to %s" % (n, outdir))


def verify(path, refdir):
    """Byte-compare every member against an already-extracted tree."""
    count, ents, fsize, data = read_dir(path)
    same = missing = differ = 0
    for name, off, size in ents:
        rel = name.replace("\\", "/")
        cand = os.path.join(refdir, *rel.split("/"))
        if not os.path.isfile(cand):
            # try a case-insensitive match within the directory
            d, base = os.path.split(cand)
            hit = None
            if os.path.isdir(d):
                for f in os.listdir(d):
                    if f.lower() == base.lower():
                        hit = os.path.join(d, f)
                        break
            if hit is None:
                print("  MISSING  %s" % rel)
                missing += 1
                continue
            cand = hit
        ref = open(cand, "rb").read()
        mem = data[off:off + size]
        if ref == mem:
            same += 1
        else:
            print("  DIFFER   %s  bank=%d ref=%d" % (rel, len(mem), len(ref)))
            differ += 1
    print("\nidentical %d, differ %d, missing %d, of %d entries"
          % (same, differ, missing, count))
    return 1 if differ else 0


def main():
    ap = argparse.ArgumentParser(description="MotoRacer .bkf bank reader")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--extract", metavar="OUTDIR")
    ap.add_argument("--verify", metavar="REFDIR")
    ap.add_argument("paths", nargs="+")
    a = ap.parse_args()
    if a.extract:
        extract(a.paths[0], a.extract)
        return 0
    if a.verify:
        return verify(a.paths[0], a.verify)
    if a.list:
        listing(a.paths)
        return 0
    return check(a.paths)


if __name__ == "__main__":
    sys.exit(main())
