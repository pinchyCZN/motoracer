#!/usr/bin/env python3
"""
level_spt.py -- label a decoded Level.Spt against the reader's key order.

A level manifest is a .bpt (see tools/bpt.py) holding 194 positional
values with no names in the file. The names live only in the reader,
FUN_00427e80, which pulls them in a fixed order through
Settings_ReadInt / Settings_ReadString / Script_ReadFloat. This script
holds that order and prints the file against it, so the mapping is
generated rather than transcribed by hand.

TWO SELF-CHECKS MAKE THE MAPPING FALSIFIABLE, and they are the reason
this is worth a script rather than a table in a document:

    NbComputer   is followed by exactly that many ComputerSpeed floats
    NbCheckPoint is followed by exactly that many TimeCheckPoint ints

Both are counts read from the file itself, so a wrong read order
almost certainly lands the count on the wrong value and the stream
runs off the end - or ends early. Getting both to consume the file
exactly is a strong check.

Usage:
    python tools/level_spt.py PATH...          labelled dump
    python tools/level_spt.py --check PATH...  self-checks only, exit 1
"""

import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bpt


def f32(v):
    """Reinterpret a stored int as the float the reader sees.

    The reader is inconsistent on purpose and the document records it:
    Settings_ReadInt returns an integer that some callers then store
    into a float field - StartPositionX is (float)atoi - while
    Script_ReadFloat parses a float directly. So a value is a float
    EITHER by conversion or by bit pattern, and which one applies is a
    property of the key, not of the file.
    """
    return struct.unpack("<f", struct.pack("<i", v))[0]


# name, kind: 'i' int, 'f' float-by-bits, 'n' int-stored-then-converted,
# 's' string. A count in braces is resolved at run time.
HEAD = [
    ("NewMotorEngine", "i"),
    ("FileLand", "s"),
    ("FileFrame3d", "s"),
    ("FileDecor", "s"),
    ("FileDecorInv", "s"),
    ("FileNet", "s"),
    ("FileMap", "s"),
    ("FileMoto", "s"),
    ("FilePocket", "s"),
    ("FileBalise", "s"),
    ("FileSmallMap", "s"),
    ("FileBank", "s"),
]

# Then the bank table: a sentinel string, an int, and 19 more triples.
# The reader walks a fixed 20 slots of 0x40 bytes; see the document.
BANK_SLOTS = 20

# Nine keys that each carry one value plus four per-detail-level ones.
ZONES = [
    ("LenBPlane", "f"),
    ("LenRLPlane", "f"),
    ("FlatZ", "f"),
    ("HorizFlatZ", "f"),
    ("MediumZone", "i"),
    ("HightZone", "i"),
    ("gouraudTexturedZ", "f"),
    ("texturedZ", "f"),
    ("gouraudZ", "f"),
]

# Thirteen AI tuning keys, three floats each.
TUNING = [
    "RATTRAPAGE_SPEED", "RATTRAPAGE_CURVE", "RATTRAPAGE_COUPLE",
    "ATTENTE1_SPEED", "ATTENTE1_CURVE", "ATTENTE1_COUPLE",
    "ATTENTE2_SPEED", "ATTENTE2_CURVE", "ATTENTE2_COUPLE",
    "CURVE_RANGE", "COUPLE_OUT_RANGE", "COUPLE_IN_RANGE",
    "SPEED_FACTOR",
]


def label(recs):
    """Walk recs in the reader's order. Returns (rows, problems)."""
    rows = []
    bad = []
    i = [0]

    def take(name, kind):
        if i[0] >= len(recs):
            bad.append("ran out of records at %s" % name)
            return None
        t, v = recs[i[0]]
        want = "str" if kind == "s" else "int"
        if t != want:
            bad.append("[%d] %s: expected %s, got %s" %
                       (i[0], name, want, t))
        shown = v
        if kind == "f":
            shown = f32(v)
        elif kind == "n":
            shown = float(v)
        rows.append((i[0], name, t, shown))
        i[0] += 1
        return v

    for name, kind in HEAD:
        take(name, kind)

    # The bank table. Slot 0's two names were already taken as FileBank
    # plus the sentinel, so the first slot reads one string and one int
    # and the remaining slots read two strings and an int.
    take("BankSentinel[0]", "s")
    take("BankFlags[0]", "i")
    for n in range(1, BANK_SLOTS):
        take("BankPage[%d]" % n, "s")
        take("BankPageD3D[%d]" % n, "s")
        take("BankFlags[%d]" % n, "i")

    take("StepGlobal", "i")
    take("YScaleLevel", "i")
    take("StartPositionX", "n")
    take("StartPositionY", "n")
    take("StartPositionZ", "n")
    take("ComputeLandOptimise", "i")

    for name, kind in ZONES:
        take(name, kind)
        for d in range(4):
            take("%s[%d]" % (name, d + 1), kind)

    for name in TUNING:
        for k in range(3):
            take("%s[%d]" % (name, k), "f")

    n = take("NbComputer", "i")
    if n is None or not (1 <= n <= 64):
        bad.append("NbComputer out of range: %r" % n)
        n = 0
    for k in range(n):
        take("ComputerSpeed[%d]" % k, "f")

    take("TimeSkill", "i")
    take("TimeSkill[1]", "i")
    take("TimeSkill[2]", "i")

    c = take("NbCheckPoint", "i")
    if c is None or not (1 <= c <= 64):
        bad.append("NbCheckPoint out of range: %r" % c)
        c = 0
    for k in range(c):
        take("TimeCheckPoint[%d]" % k, "i")

    take("FactorPocket", "f")
    take("AudioTrack", "i")

    if i[0] != len(recs):
        bad.append("consumed %d of %d records - the reader's order does "
                   "not account for the whole file" % (i[0], len(recs)))
    return rows, bad


def main():
    ap = argparse.ArgumentParser(description="MotoRacer Level.Spt labeller")
    ap.add_argument("paths", nargs="+")
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()

    rc = 0
    for path in args.paths:
        with open(path, "rb") as fh:
            data = fh.read()
        recs = bpt.records(data)
        rows, bad = label(recs)

        print("== %s  (%d records)" % (os.path.basename(path), len(recs)))
        if not args.check:
            for idx, name, t, v in rows:
                if isinstance(v, float):
                    print("  [%3d] %-24s %s" % (idx, name, "%.6g" % v))
                else:
                    print("  [%3d] %-24s %s" % (idx, name, v))
        for b in bad:
            print("  PROBLEM: %s" % b)
            rc = 1
        if not bad:
            print("  OK - the reader's order accounts for every record.")
        print()
    return rc


if __name__ == "__main__":
    sys.exit(main())
