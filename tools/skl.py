#!/usr/bin/env python3
"""
skl.py -- reader for the MotoRacer .SKL skeleton and .ROT pose formats.

Layout, from FUN_00462c60 (Skl_Load) in player.cpp and confirmed
byte-exactly against SPEED\\CORPS.SKL and KTMCROSS\\CORPS.SKL: both
decode with nothing left over.

    i16   memberCount        NEGATIVE means the vertices are floats and
                             the real count follows; positive means the
                             vertices are int16
    i16   parent[members]    -1 for the root
    i16   remap[members]     float form only; the loader uses it to
                             order its tree, the parent list already
                             says the same thing
    i16   vertexCount
    vertex[vertexCount]      float x,y,z * 3.5   or   i16 x,y,z * 10.0
    member[members]:
        origin               float x,y,z * 3.5   or   i16 x,y,z
        i16 firstVertex      into the shared pool
        i16 vertexCount      the runs partition the pool exactly
        i16 faceCount
        face[faceCount]
    i16   trailingFaceCount  one more group, owned by no member
    face[trailingFaceCount]

A face is 36 bytes:

    i16 n                    3 or 4, the real vertex count
    i16 kind                 3 or 4 as well, almost always 3
    i16 colour               5-6-5
    5 x { i16 vertexIndex, i16 u, i16 v }

Five triples are always stored: slot n repeats slot 0 to close the
polygon and the rest are -1.

A .ROT is one POSE, not a clip: the magic "ROT\\0" then 18 triples of
i16 Euler angles, one per member, in the game's fixed point where a
full turn is 0x4000. 112 bytes, and every .rot the game ships is
exactly that size.

Usage:
    python tools/skl.py --info  PATH        members, tree and counts
    python tools/skl.py --check PATH ...    decode and report leftovers
    python tools/skl.py --pose  PATH        dump a .rot
"""

import argparse
import struct
import sys

MAX_MEMBERS = 0x12
FACE_BYTES = 36
ROT_MAGIC = b"ROT\x00"


class Reader(object):
    def __init__(self, data):
        self.d = data
        self.p = 0

    def i16(self):
        v = struct.unpack_from("<h", self.d, self.p)[0]
        self.p += 2
        return v

    def f32(self):
        v = struct.unpack_from("<f", self.d, self.p)[0]
        self.p += 4
        return v


def read_faces(r, n):
    out = []
    for _ in range(n):
        cnt = r.i16()
        kind = r.i16()
        colour = r.i16()
        vs = [(r.i16(), r.i16(), r.i16()) for _ in range(5)]
        out.append({"n": cnt, "kind": kind, "colour": colour & 0xFFFF,
                    "verts": vs[:cnt]})
    return out


def parse(path):
    d = open(path, "rb").read()
    r = Reader(d)
    n = r.i16()
    float_form = n < 0
    if float_form:
        n = r.i16()
        scale = 3.5
    else:
        scale = 10.0
    if n <= 0 or n > MAX_MEMBERS:
        raise ValueError("%d members, the loader's limit is %d"
                         % (n, MAX_MEMBERS))

    parent = [r.i16() for _ in range(n)]
    remap = [r.i16() for _ in range(n)] if float_form else None
    nv = r.i16()

    verts = []
    for _ in range(nv):
        if float_form:
            verts.append((r.f32() * scale, r.f32() * scale, r.f32() * scale))
        else:
            verts.append((float(r.i16()), float(r.i16()), float(r.i16())))

    members = []
    for i in range(n):
        if float_form:
            org = (r.f32() * scale, r.f32() * scale, r.f32() * scale)
        else:
            org = (float(r.i16()), float(r.i16()), float(r.i16()))
        first = r.i16()
        count = r.i16()
        nf = r.i16()
        if nf < 0 or r.p + nf * FACE_BYTES > len(d):
            raise ValueError("member %d: face count %d at offset %d"
                             % (i, nf, r.p))
        members.append({"parent": parent[i], "origin": org, "first": first,
                        "count": count, "faces": read_faces(r, nf)})

    nf = r.i16()
    if nf < 0 or r.p + nf * FACE_BYTES > len(d):
        raise ValueError("trailing face count %d at offset %d" % (nf, r.p))
    tail = read_faces(r, nf)

    return {"path": path, "size": len(d), "float_form": float_form,
            "scale": scale, "members": members, "remap": remap,
            "verts": verts, "tail": tail, "consumed": r.p}


def info(path):
    s = parse(path)
    nf = sum(len(m["faces"]) for m in s["members"]) + len(s["tail"])
    print("%s  %d bytes, %s form, scale %.1f"
          % (path, s["size"], "float" if s["float_form"] else "int16",
             s["scale"]))
    print("  %d members, %d vertices, %d faces"
          % (len(s["members"]), len(s["verts"]), nf))
    run = 0
    for i, m in enumerate(s["members"]):
        ok = "" if m["first"] == run else "  <- RUN BREAK"
        run = m["first"] + m["count"]
        print("  %2d parent %3d  verts %3d..%-3d  faces %3d  org %s%s"
              % (i, m["parent"], m["first"], m["first"] + m["count"] - 1,
                 len(m["faces"]),
                 "(%7.1f %7.1f %7.1f)" % m["origin"], ok))
    print("  trailing group: %d faces" % len(s["tail"]))
    print("  runs end at %d, vertexCount is %d" % (run, len(s["verts"])))
    print("  consumed %d of %d bytes" % (s["consumed"], s["size"]))


def check(paths):
    bad = 0
    for p in paths:
        try:
            s = parse(p)
        except Exception as e:
            print("%-40s FAIL %s" % (p, e))
            bad += 1
            continue
        left = s["size"] - s["consumed"]
        run = 0
        for m in s["members"]:
            run = m["first"] + m["count"]
        note = "ok" if left == 0 and run == len(s["verts"]) else "MISMATCH"
        print("%-40s %s  %d left, runs end %d of %d"
              % (p, note, left, run, len(s["verts"])))
        if note != "ok":
            bad += 1
    return 1 if bad else 0


ROT_MEMBERS = 17
ROT_BYTES = 4 + 3 * 2 + ROT_MEMBERS * 3 * 2


def pose(path):
    """4 magic + 3 int16 of header + 17 angle triples.

    NOT 18 triples from offset 4. Both readings total 112 bytes, which
    is exactly how that bug went unnoticed here and in src/skl.c; the
    wrong one puts every member's angles on the bone before it.
    Rot_Load (0x0045b340) reads the magic, then three separate two-byte
    values, then the triples. See docs/formats/skl-rot.txt section 3.
    """
    d = open(path, "rb").read()
    if d[:4] != ROT_MAGIC:
        print("%s: magic is %r, expected %r" % (path, d[:4], ROT_MAGIC))
        return 1
    if len(d) != ROT_BYTES:
        print("%s: %d bytes, expected %d" % (path, len(d), ROT_BYTES))
        return 1
    hdr = struct.unpack_from("<3h", d, 4)
    vals = struct.unpack_from("<%dh" % (ROT_MEMBERS * 3), d, 10)
    print("%s  %d bytes" % (path, len(d)))
    print("  header (read and discarded by the game): %6d %6d %6d" % hdr)
    for i in range(ROT_MEMBERS):
        print("  %2d: %6d %6d %6d"
              % (i, vals[i * 3], vals[i * 3 + 1], vals[i * 3 + 2]))
    return 0


def main():
    ap = argparse.ArgumentParser(description="MotoRacer .skl / .rot reader")
    ap.add_argument("--info", metavar="PATH")
    ap.add_argument("--check", nargs="+", metavar="PATH")
    ap.add_argument("--pose", metavar="PATH")
    a = ap.parse_args()

    if a.info:
        info(a.info)
        return 0
    if a.check:
        return check(a.check)
    if a.pose:
        return pose(a.pose)
    ap.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
