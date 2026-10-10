#!/usr/bin/env python3
"""
pose_cmp.py - diff our port's skeleton against the original's, stage by
              stage, and name the first stage that disagrees.

    python tools/pose_cmp.py mrcapture.txt mrposedbg.txt

mrcapture.txt is written by tools/mrhook inside a running MOTO.EXE.

MRPOSEDBG.TXT HAS NO WRITER RIGHT NOW. It came from src/posedbg.h, a
temporary header that dumped our skeleton in mrhook's layout; that
header was deleted once the rider pose was solved, because diagnostic
scaffolding left in src/ is worse than diagnostic scaffolding you
rewrite. This tool is kept because the LADDER is the reusable part -
recreating the dump is twenty minutes, rediscovering that you need a
staged comparison cost three sessions.

To use it again, write the dump from wherever the question is, in the
layout read_parts() below expects:

    part <n>  verts <first>..<last> faces <n>
      ANGLE  <3 radians>   units <3 in 0x4000 units>
      mat    <9 floats>
      trn    <3 floats>   pos <3 floats>

    bind vertices <n>      then "  b<i>  x y z"
    posed vertices <n>     then "  v<i>  x y z"

WHY A LADDER. The pipeline is .skl parse -> .rot angles -> matrices ->
vertices, and each stage consumes the one before it. Comparing only the
end result says "wrong" without saying where, and comparing only the
matrices - which is what skl_pose_diff.py does - cannot see a stage the
capture does not carry. Reporting the FIRST stage that differs is the
only form of this test that localises anything.

Exit status is 0 when every stage agrees.
"""

import argparse
import math
import re
import sys

TOL_VERT = 0.05
TOL_MAT = 0.01
TOL_ANGLE = 0.01


def read_blocks(path):
    return open(path, "r").read()


def read_parts(text):
    """part index -> {angle, units, mat, first, count}. Line based."""
    out = {}
    cur = None
    for line in text.split("\n"):
        t = line.strip()
        m = re.match(r"^part (\d+)\s+verts\s+(-?\d+)\.\.(-?\d+)", t)
        if m:
            first, last = int(m.group(2)), int(m.group(3))
            cur = {"first": first, "count": last - first + 1}
            out[int(m.group(1))] = cur
            continue
        if cur is None:
            continue
        if t.startswith("ANGLE"):
            f = [float(x) for x in re.findall(r"-?\d+\.\d+", t)]
            if len(f) >= 6:
                cur["angle"] = f[0:3]
                cur["units"] = f[3:6]
        elif t.startswith("mat"):
            cur["rot"] = [float(x) for x in re.findall(r"-?\d+\.\d+", t)]
        elif t.startswith("trn"):
            f = [float(x) for x in re.findall(r"-?\d+\.\d+", t)]
            if cur.get("rot") and len(f) >= 3:
                cur["mat"] = cur["rot"] + f[0:3]
    return {k: v for k, v in out.items() if v.get("mat")}


def read_verts(text, tag):
    """The run of '  <tag>N  x y z' lines, as a list indexed by N."""
    out = {}
    pat = re.compile(r"^" + tag + r"(\d+)\s+(-?\d+\.\d+)\s+(-?\d+\.\d+)"
                     r"\s+(-?\d+\.\d+)")
    for line in text.split("\n"):
        m = pat.match(line.strip())
        if m:
            out[int(m.group(1))] = (float(m.group(2)), float(m.group(3)),
                                    float(m.group(4)))
    if not out:
        return []
    return [out.get(i) for i in range(max(out) + 1)]


def invert_rigid(m):
    """The inverse maybe_Racer_Draw builds at 0x004615c1 before it
    transforms a part's vertices. Stage 4 used to apply the matrix
    itself, which is the premise that made this whole comparison agree
    with itself while the screen disagreed."""
    return [m[0], m[3], m[6],
            m[1], m[4], m[7],
            m[2], m[5], m[8],
            -(m[0] * m[9] + m[1] * m[10] + m[2] * m[11]),
            -(m[3] * m[9] + m[4] * m[10] + m[5] * m[11]),
            -(m[6] * m[9] + m[7] * m[10] + m[8] * m[11])]


def transform(v, m):
    return (v[0] * m[0] + v[1] * m[3] + v[2] * m[6] + m[9],
            v[0] * m[1] + v[1] * m[4] + v[2] * m[7] + m[10],
            v[0] * m[2] + v[1] * m[5] + v[2] * m[8] + m[11])


def dist(a, b):
    return math.sqrt(sum((a[i] - b[i]) ** 2 for i in range(3)))


def stage(name, worst, at, tol, detail=""):
    ok = worst <= tol
    print("  %-26s worst %10.4f  %-14s %s"
          % (name, worst, at, "ok" if ok else "<-- DIFFERS"))
    if detail and not ok:
        print("        %s" % detail)
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("capture", help="mrcapture.txt from mrhook")
    ap.add_argument("ours", help="mrposedbg.txt from the port")
    a = ap.parse_args()

    ctext = read_blocks(a.capture)
    otext = read_blocks(a.ours)

    # The capture repeats per bike; the first block is enough - the pose
    # is identical in all of them (runtime-capture.txt section 7).
    cparts = read_parts(ctext.split("g_pilotRot")[0])
    oparts = read_parts(otext)
    cbind = read_verts(ctext, "v")
    obind = read_verts(otext, "b")
    oposed = read_verts(otext, "v")

    print("capture: %d parts, %d bind vertices" % (len(cparts), len(cbind)))
    print("ours   : %d parts, %d bind, %d posed"
          % (len(oparts), len(obind), len(oposed)))
    print("")

    allok = True

    # 1. the .skl parse
    n = min(len(cbind), len(obind))
    if n:
        worst, at = max(((dist(cbind[i], obind[i]), i) for i in range(n)
                         if cbind[i] and obind[i]), default=(0.0, -1))
        d = [tuple(obind[i][k] - cbind[i][k] for k in range(3))
             for i in range(n) if cbind[i] and obind[i]]
        mean = tuple(sum(x[k] for x in d) / len(d) for k in range(3))
        allok &= stage("1. .skl bind vertices", worst, "v%d" % at, TOL_VERT,
                       "mean offset %.3f %.3f %.3f - a constant offset here "
                       "is the root re-centring" % mean)
    else:
        print("  1. .skl bind vertices     no data")

    common = sorted(set(cparts) & set(oparts))

    # 2. the .rot angles
    worst, at = 0.0, ""
    for p in common:
        ca, oa = cparts[p].get("angle"), oparts[p].get("angle")
        if not ca or not oa:
            continue
        for k in range(3):
            if abs(ca[k] - oa[k]) > worst:
                worst, at = abs(ca[k] - oa[k]), "part %d axis %d" % (p, k)
    allok &= stage("2. .rot angles", worst, at, TOL_ANGLE,
                   "a whole-table shift means the wrong .rot or the wrong "
                   "index; a scale means the wrong angle unit")

    # 3. the matrices
    worst, at = 0.0, ""
    for p in common:
        for k in range(12):
            d = abs(cparts[p]["mat"][k] - oparts[p]["mat"][k])
            if d > worst:
                worst, at = d, "part %d elem %d" % (p, k)
    allok &= stage("3. world matrices", worst, at, TOL_MAT,
                   "rotation elements 0..8 mean Euler order; translation "
                   "9..11 mean composition order or the joint origins")

    # 4. the posed vertices - the game's own, rebuilt from its matrices
    truth = {}
    for p, rec in cparts.items():
        if rec["count"] <= 0:
            continue
        for k in range(rec["first"], rec["first"] + rec["count"]):
            if k < len(cbind) and cbind[k]:
                truth[k] = transform(cbind[k], invert_rigid(rec["mat"]))
    worst, at = 0.0, -1
    for k, t in truth.items():
        if k < len(oposed) and oposed[k]:
            d = dist(oposed[k], t)
            if d > worst:
                worst, at = d, k
    allok &= stage("4. posed vertices", worst, "v%d" % at, TOL_VERT,
                   "reached only if 1-3 agree; a difference here is the "
                   "vertex loop or the member-to-run mapping")

    print("")
    if allok:
        print("EVERY STAGE AGREES. The geometry our port computes is the")
        print("geometry the original computes, so anything still wrong on")
        print("screen is downstream of skl.c - prims, culling, the camera")
        print("or the instance transform.")
        return 0
    print("Fix the FIRST stage marked DIFFERS; the later ones consume it.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
