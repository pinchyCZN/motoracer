#!/usr/bin/env python3
"""
skl_pose_diff.py - check our skeleton poser against the original's own
                   output, captured at run time by tools/mrhook.

    python tools/skl_pose_diff.py CORPS.SKL mrcapture.txt [--capture N]
    python tools/skl_pose_diff.py CORPS.SKL mrcapture.txt --rots DIR

The first form poses the skeleton with every .rot in the SKL's folder
and reports which one reproduces the captured vertices; the second
points at a different folder of poses. --capture picks a block other
than the first.

WHY THIS EXISTS. src/skl.c cannot be run - CLAUDE.md rule 9 forbids
executing anything the build produces - so this mirrors its maths in
Python instead. It is a MIRROR, not an independent implementation: if
src/skl.c changes, this changes with it, and a disagreement between the
two is a bug in this file rather than evidence about the game.

The transform, member origins, re-centring and Euler order all follow
src/skl.c and src/model.c exactly. See docs/formats/skl-rot.txt.
"""

import argparse
import math
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import skl                                          # noqa: E402

TRIG_TABLE_SIZE = 0x4000
TRIG_MASK = 0x3FFF
TRIG_QUARTER = 0x1000
TRIG_TWO_PI = 6.283185307178
# A .ROT turn is 0x1000; the trig table's is 0x4000 - Rot_Load
# scales by the double at 0x004d0e68 = 2*pi/0x1000.
ROT_TURN = 0x1000
RADIANS_TO_INDEX = TRIG_TABLE_SIZE / TRIG_TWO_PI

COS = [math.cos(i * TRIG_TWO_PI / TRIG_TABLE_SIZE)
       for i in range(TRIG_TABLE_SIZE)]


def tcos(i):
    return COS[i & TRIG_MASK]


def tsin(i):
    return COS[(i - TRIG_QUARTER) & TRIG_MASK]


def from_euler(rot):
    """mat34_from_euler (src/model.c), the nine products from 0x0048c0fb."""
    i0 = int(rot[0] * RADIANS_TO_INDEX)
    i1 = int(rot[1] * RADIANS_TO_INDEX)
    i2 = int(rot[2] * RADIANS_TO_INDEX)
    c0, s0 = tcos(i0), tsin(i0)
    c1, s1 = tcos(i1), tsin(i1)
    c2, s2 = tcos(i2), tsin(i2)
    return [c2 * c1,
            s2 * c0 + c2 * s1 * s0,
            s2 * s0 - c2 * s1 * c0,
            -s2 * c1,
            c2 * c0 - s2 * s1 * s0,
            s2 * s1 * c0 + c2 * s0,
            s1,
            -c1 * s0,
            c1 * c0,
            0.0, 0.0, 0.0]


def mat34_multiply(a, b):
    """out = a * b (src/model.c 0x00403a80)."""
    r = [0.0] * 12
    for i in range(3):
        r[i * 3 + 0] = a[i * 3 + 0] * b[0] + a[i * 3 + 1] * b[3] + \
                       a[i * 3 + 2] * b[6]
        r[i * 3 + 1] = a[i * 3 + 0] * b[1] + a[i * 3 + 1] * b[4] + \
                       a[i * 3 + 2] * b[7]
        r[i * 3 + 2] = a[i * 3 + 0] * b[2] + a[i * 3 + 1] * b[5] + \
                       a[i * 3 + 2] * b[8]
    r[9] = a[9] * b[0] + a[10] * b[3] + a[11] * b[6] + b[9]
    r[10] = a[9] * b[1] + a[10] * b[4] + a[11] * b[7] + b[10]
    r[11] = a[9] * b[2] + a[10] * b[5] + a[11] * b[8] + b[11]
    return r


def read_rot(path):
    """4 magic + 3 int16 of header + 17 angle triples (src/skl.c).

    The header is NOT member 0's angles. Reading 18 triples from
    offset 4 also totals 112 bytes, so the mistake is invisible by
    inspection - see docs/formats/skl-rot.txt section 3.
    """
    d = open(path, "rb").read()
    if d[:3] != b"ROT":
        raise ValueError("%s: not a ROT file" % path)
    out = []
    for i in range(17):
        t = []
        for j in range(3):
            o = 10 + (i * 3 + j) * 2
            v = d[o] | (d[o + 1] << 8)
            t.append(v - 0x10000 if v >= 0x8000 else v)
        out.append(t)
    out.append([0, 0, 0])          # pad to the loader's member ceiling
    return out


def recentre(s):
    """src/skl.c: subtract member 0's origin from vertices AND origins."""
    root = s["members"][0]["origin"]
    s["verts"] = [(v[0] - root[0], v[1] - root[1], v[2] - root[2])
                  for v in s["verts"]]
    for m in s["members"]:
        o = m["origin"]
        m["origin"] = (o[0] - root[0], o[1] - root[1], o[2] - root[2])


def world_matrices(s, angles):
    """The matrix stage of src/skl.c skl_apply_pose, on its own.

    These are what the game keeps per body part, at
    racer + 0x63C + (lod*20 + part)*100 + 0x18, so they are directly
    comparable with a capture.
    """
    n = len(s["members"])
    remap = s.get("remap") or list(range(n))
    world = [None] * n
    for i in range(n):
        # member i wears the PART triple, and a .rot turn is 0x1000
        src = remap[i] if i < len(remap) else i
        rot = [angles[src][k] * TRIG_TWO_PI / ROT_TURN
               for k in range(3)] if angles else [0.0, 0.0, 0.0]
        local = from_euler(rot)
        o = s["members"][i]["origin"]
        local[9] = o[0] - (o[0] * local[0] + o[1] * local[3] + o[2] * local[6])
        local[10] = o[1] - (o[0] * local[1] + o[1] * local[4] + o[2] * local[7])
        local[11] = o[2] - (o[0] * local[2] + o[1] * local[5] + o[2] * local[8])
        p = s["members"][i]["parent"]
        world[i] = mat34_multiply(world[p], local) \
            if 0 <= p < n and p != i else local
    return world


def invert_rigid(m):
    """src/skl.c mat34_invert_rigid - transpose, translation negated
    through it. maybe_Racer_Draw builds this inline at 0x004615c1."""
    return [m[0], m[3], m[6],
            m[1], m[4], m[7],
            m[2], m[5], m[8],
            -(m[0] * m[9] + m[1] * m[10] + m[2] * m[11]),
            -(m[3] * m[9] + m[4] * m[10] + m[5] * m[11]),
            -(m[6] * m[9] + m[7] * m[10] + m[8] * m[11])]


def apply_pose(s, angles):
    """src/skl.c skl_apply_pose.

    THE INVERSE, NOT THE MATRIX. world_matrices() returns what the game
    stores at record +0x18; Draw transforms the vertices by its inverse.
    """
    n = len(s["members"])
    world = world_matrices(s, angles)
    out = list(s["verts"])
    for i in range(n):
        m = invert_rigid(world[i])
        first = s["members"][i]["first"]
        count = s["members"][i]["count"]
        if first < 0 or count <= 0 or first + count > len(out):
            continue
        for k in range(first, first + count):
            v = s["verts"][k]
            out[k] = (v[0] * m[0] + v[1] * m[3] + v[2] * m[6] + m[9],
                      v[0] * m[1] + v[1] * m[4] + v[2] * m[7] + m[10],
                      v[0] * m[2] + v[1] * m[5] + v[2] * m[8] + m[11])
    return out


def read_capture(path, index):
    text = open(path, "r").read()
    blocks = re.split(r"={16,}", text)
    caps = [blocks[i + 1] for i in range(len(blocks) - 1)
            if blocks[i].strip().startswith("capture ")]
    if not caps:
        raise ValueError("%s holds no capture blocks" % path)
    if index < 1 or index > len(caps):
        raise ValueError("capture %d of %d" % (index, len(caps)))
    rows = re.findall(
        r"^\s+v\d+\s+(-?\d+\.\d+)\s+(-?\d+\.\d+)\s+(-?\d+\.\d+)",
        caps[index - 1], re.M)
    return [tuple(float(c) for c in r) for r in rows], len(caps)


def read_parts(block):
    """The per-part records mrhook dumps. Returns index -> record.

    LINE-BASED ON PURPOSE. This used to be one multi-line regex and it
    silently matched nothing the first time the dump gained a field,
    reporting "0 parts" as though the capture were empty. Parsing each
    line on its own key means an added field is ignored rather than
    fatal.
    """
    out = {}
    cur = None
    for line in block.split("\n"):
        t = line.strip()
        m = re.match(r"^part (\d+)\s+verts\s+(-?\d+)\.\.(-?\d+)"
                     r"\s+faces\s+(\d+)", t)
        if m:
            first, last = int(m.group(2)), int(m.group(3))
            cur = {"first": first, "count": last - first + 1,
                   "faces": int(m.group(4)), "mat": None, "angle": None}
            out[int(m.group(1))] = cur
            continue
        if cur is None:
            continue
        if t.startswith("ANGLE"):
            f = [float(x) for x in re.findall(r"-?\d+\.\d+", t)]
            if len(f) >= 6:
                cur["angle"] = f[0:3]          # radians
                cur["units"] = f[3:6]          # same, in 0x4000 units
        elif t.startswith("mat"):
            cur["rot"] = [float(x) for x in re.findall(r"-?\d+\.\d+", t)]
        elif t.startswith("trn"):
            f = [float(x) for x in re.findall(r"-?\d+\.\d+", t)]
            if cur.get("rot") and len(f) >= 3:
                cur["mat"] = cur["rot"] + f[0:3]
                cur["pos"] = f[3:6] if len(f) >= 6 else None
    return {k: v for k, v in out.items() if v.get("mat")}


def map_parts_to_members(s, parts):
    """Match by vertex run, which is unique for every non-empty member.

    Empty members (three of them in CORPS.SKL, all zero-length runs at
    the same index) are skipped: they carry no vertices, so they cannot
    be told apart this way and contribute nothing to the comparison.
    """
    by_run = {}
    for i, mem in enumerate(s["members"]):
        if mem["count"] > 0:
            by_run.setdefault((mem["first"], mem["count"]), []).append(i)
    out = {}
    for p, rec in parts.items():
        if rec["count"] <= 0:
            continue
        cand = by_run.get((rec["first"], rec["count"]))
        if cand and len(cand) == 1:
            out[p] = cand[0]
    return out


def compare_parts(ours_world, parts, mapping):
    """Worst rotation-entry error and worst translation error."""
    worst_r = 0.0
    worst_t = 0.0
    worst_at = -1
    for p, mem in sorted(mapping.items()):
        got = parts[p]["mat"]
        mine = ours_world[mem]
        dr = max(abs(got[k] - mine[k]) for k in range(9))
        dt = math.sqrt(sum((got[9 + k] - mine[9 + k]) ** 2 for k in range(3)))
        if dr > worst_r:
            worst_r, worst_at = dr, p
        worst_t = max(worst_t, dt)
    return worst_r, worst_t, worst_at


def run_parts_mode(a, rotdir, rots):
    text = open(a.capture, "r").read()
    blocks = re.split(r"={16,}", text)
    caps = [blocks[i + 1] for i in range(len(blocks) - 1)
            if blocks[i].strip().startswith("capture ")]
    if not caps:
        print("%s holds no capture blocks" % a.capture)
        return 2
    block = caps[a.capture_index - 1]
    parts = read_parts(block)
    if not parts:
        print("no part records in capture %d - is this a dump from the\n"
              "older mrhook, before it learned to read them?"
              % a.capture_index)
        return 2

    s0 = skl.parse(a.sklpath)
    recentre(s0)
    mapping = map_parts_to_members(s0, parts)
    print("capture %d of %d: %d part records, %d matched to members by "
          "vertex run" % (a.capture_index, len(caps), len(parts), len(mapping)))
    print("")

    results = []
    for name in ["(identity)"] + rots:
        s = skl.parse(a.sklpath)
        recentre(s)
        angles = None if name == "(identity)" else \
            read_rot(os.path.join(rotdir, name))
        w = world_matrices(s, angles)
        wr, wt, at = compare_parts(w, parts, mapping)
        results.append((wr, wt, at, name))

    results.sort(key=lambda r: r[0])
    print("%-20s %12s %12s %s" % ("pose", "worst rot", "worst trn", "at part"))
    print("%-20s %12s %12s %s" % ("-" * 20, "-" * 12, "-" * 12, "-" * 7))
    for wr, wt, at, name in results:
        print("%-20s %12.5f %12.4f  part %d" % (name, wr, wt, at))
    print("")

    wr, wt, at, name = results[0]
    if wr <= a.tol:
        print("MATCH: %s reproduces the captured part matrices "
              "(worst rotation entry %.5f, worst translation %.4f)."
              % (name, wr, wt))
        return 0
    print("NO MATCH. Best is %s, worst rotation entry %.5f at part %d."
          % (name, wr, at))
    print("")
    print("Per part, best candidate:")
    s = skl.parse(a.sklpath)
    recentre(s)
    angles = None if name == "(identity)" else \
        read_rot(os.path.join(rotdir, name))
    w = world_matrices(s, angles)
    print("%6s %8s %12s %12s" % ("part", "member", "rot err", "trn err"))
    for p, mem in sorted(mapping.items()):
        got, mine = parts[p]["mat"], w[mem]
        dr = max(abs(got[k] - mine[k]) for k in range(9))
        dt = math.sqrt(sum((got[9 + k] - mine[9 + k]) ** 2 for k in range(3)))
        flag = "" if dr <= a.tol else "   <-"
        print("%6d %8d %12.5f %12.4f%s" % (p, mem, dr, dt, flag))
    return 1


def compare(ours, theirs):
    n = min(len(ours), len(theirs))
    worst = 0.0
    worst_at = -1
    total = 0.0
    for i in range(n):
        d = math.sqrt(sum((ours[i][k] - theirs[i][k]) ** 2 for k in range(3)))
        total += d
        if d > worst:
            worst, worst_at = d, i
    return worst, worst_at, total / n if n else 0.0


def per_member(s, ours, theirs):
    rows = []
    for i, m in enumerate(s["members"]):
        first, count = m["first"], m["count"]
        if count <= 0:
            rows.append((i, m["parent"], 0, 0.0))
            continue
        w = 0.0
        for k in range(first, min(first + count, len(theirs))):
            d = math.sqrt(sum((ours[k][j] - theirs[k][j]) ** 2
                              for j in range(3)))
            w = max(w, d)
        rows.append((i, m["parent"], count, w))
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("sklpath")
    ap.add_argument("capture")
    ap.add_argument("--capture-index", type=int, default=1)
    ap.add_argument("--rots")
    ap.add_argument("--tol", type=float, default=0.01)
    ap.add_argument("--parts", action="store_true",
                    help="compare against the per-part matrices instead of "
                         "the vertex pool - THE ONLY MEANINGFUL TEST, see "
                         "the note below")
    a = ap.parse_args()

    rotdir0 = a.rots or os.path.dirname(os.path.abspath(a.sklpath))
    rots0 = sorted(f for f in os.listdir(rotdir0)
                   if f.lower().endswith(".rot"))
    if a.parts:
        return run_parts_mode(a, rotdir0, rots0)

    print("WARNING: vertex-pool mode proves nothing about the pose.")
    print("         maybe_Racer_Draw keeps ONE bind pool per LOD and")
    print("         transforms a slice of it per body part, so that buffer")
    print("         holds the bind pose whatever the game is doing - the")
    print("         identity always wins. Use --parts. This mode is kept")
    print("         only as an end-to-end check of the .SKL parser, the")
    print("         load scale and the root-joint re-centring.")
    print("")

    theirs, ncaps = read_capture(a.capture, a.capture_index)
    print("capture %d of %d in %s: %d vertices"
          % (a.capture_index, ncaps, a.capture, len(theirs)))

    rotdir = a.rots or os.path.dirname(os.path.abspath(a.sklpath))
    rots = sorted(f for f in os.listdir(rotdir) if f.lower().endswith(".rot"))
    print("%d pose files in %s" % (len(rots), rotdir))
    print("")

    results = []
    for name in ["(identity)"] + rots:
        s = skl.parse(a.sklpath)
        recentre(s)
        angles = None if name == "(identity)" else \
            read_rot(os.path.join(rotdir, name))
        ours = apply_pose(s, angles)
        worst, at, mean = compare(ours, theirs)
        results.append((worst, mean, at, name, s, ours))

    results.sort(key=lambda r: r[0])
    print("%-20s %12s %12s %s" % ("pose", "worst", "mean", "worst vertex"))
    print("%-20s %12s %12s %s" % ("-" * 20, "-" * 12, "-" * 12, "-" * 12))
    for worst, mean, at, name, _, _ in results:
        print("%-20s %12.4f %12.4f  v%d" % (name, worst, mean, at))
    print("")

    worst, mean, at, name, s, ours = results[0]
    if worst <= a.tol:
        print("MATCH: %s reproduces the capture to %.4f units." % (name, worst))
        return 0

    print("NO MATCH. Best is %s at %.4f units. Per member:" % (name, worst))
    print("")
    print("%4s %7s %6s %12s" % ("mem", "parent", "verts", "worst"))
    for i, parent, count, w in per_member(s, ours, theirs):
        flag = "" if w <= a.tol else "   <-"
        print("%4d %7d %6d %12.4f%s" % (i, parent, count, w, flag))
    return 1


if __name__ == "__main__":
    sys.exit(main())
