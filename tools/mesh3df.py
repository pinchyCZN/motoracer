#!/usr/bin/env python3
"""
mesh3df.py -- reader for the MotoRacer .3df mesh format.

Layout, from FUN_00420590 in MOTO.EXE and confirmed byte-exactly against
LAUREAT.3DF:

    u16   version         always 1; the loader rejects anything else
    u16   vertexCount
    vertex[vertexCount]   3 x float32, x y z
    face records, repeating:
        u16 n             vertices in this face; 0 terminates the list
        u16 flags         material or texture selector
        u16 index[n]      indices into the vertex array

The loader allocates 12 bytes per vertex and 0x40 per face, treats n == 3
and n != 3 on separate paths (triangle versus polygon), and afterwards
computes a plane distance per face, so faces carry a normal downstream.

Usage:
    python tools/mesh3df.py --check PATH [PATH ...]
    python tools/mesh3df.py --info  PATH
    python tools/mesh3df.py --stats PATH [PATH ...]   csv for correlation
    python tools/mesh3df.py --obj OUT.obj PATH        export geometry
"""

import argparse
import os
import struct
import sys


def parse(path):
    d = open(path, "rb").read()
    ver, nv = struct.unpack_from("<HH", d, 0)
    if ver != 1:
        raise ValueError("version %d, expected 1" % ver)
    off = 4
    need = nv * 12
    if off + need > len(d):
        raise ValueError("vertex block overruns file")
    verts = []
    for i in range(nv):
        verts.append(struct.unpack_from("<fff", d, off + i * 12))
    off += need

    faces = []
    while True:
        if off + 2 > len(d):
            raise ValueError("face list ran off the end")
        n = struct.unpack_from("<H", d, off)[0]
        off += 2
        if n == 0:
            break
        if off + 2 + n * 2 > len(d):
            raise ValueError("face record overruns file")
        flags = struct.unpack_from("<H", d, off)[0]
        off += 2
        idx = struct.unpack_from("<%dH" % n, d, off)
        off += n * 2
        faces.append((flags, list(idx)))
    return {
        "path": path, "size": len(d), "version": ver,
        "verts": verts, "faces": faces, "consumed": off,
    }


MAP_HDR = 0x200
MAP_REC = 0x80


def parse_map(path, nfaces):
    """Parse the paired .map: 512-byte header then one 128-byte record
    per face. Offsets are from FUN_00420a50's stack frame."""
    d = open(path, "rb").read()
    if len(d) < MAP_HDR:
        raise ValueError("shorter than the 512-byte header")
    if not d[1] & 0x80:
        raise ValueError("header byte1 0x%02x lacks bit 0x80" % d[1])
    recs = []
    for i in range(nfaces):
        o = MAP_HDR + i * MAP_REC
        if o + MAP_REC > len(d):
            raise ValueError("record %d overruns file" % i)
        r = d[o:o + MAP_REC]
        n = struct.unpack_from("<H", r, 0x00)[0]
        u = struct.unpack_from("<4i", r, 0x22)
        v = struct.unpack_from("<4i", r, 0x32)
        flags = struct.unpack_from("<I", r, 0x48)[0]
        tex = struct.unpack_from("<I", r, 0x4E)[0]
        recs.append({"n": n, "u": u, "v": v, "flags": flags,
                     "bank": flags & 0xFFFF, "tex": tex})
    return recs, len(d)


def check(paths):
    bad = 0
    for p in paths:
        name = os.path.basename(p)
        try:
            m = parse(p)
        except Exception as e:
            print("%-18s FAIL  %s" % (name, e))
            bad += 1
            continue
        problems = []
        if m["consumed"] != m["size"]:
            problems.append("consumed %d of %d" % (m["consumed"], m["size"]))
        nv = len(m["verts"])
        for flags, idx in m["faces"]:
            if any(i >= nv for i in idx):
                problems.append("index out of range")
                break
        sizes = sorted({len(i) for _, i in m["faces"]})

        # Cross-check the paired .map the way the game does: its record
        # count must match the mesh, and each record's vertex count must
        # agree with the corresponding face.
        mapinfo = ""
        mp = os.path.splitext(p)[0] + ".MAP"
        if not os.path.isfile(mp):
            mp = os.path.splitext(p)[0] + ".map"
        if os.path.isfile(mp):
            try:
                recs, msz = parse_map(mp, len(m["faces"]))
                need = MAP_HDR + len(m["faces"]) * MAP_REC
                pad = msz - need
                # The loader only validates records with n >= 3; an
                # n < 3 record takes a separate, unchecked path and
                # carries no UVs. Apply the same rule here.
                mism = sum(1 for r, (_, idx) in zip(recs, m["faces"])
                           if r["n"] >= 3 and r["n"] != len(idx))
                untex = sum(1 for r in recs if r["n"] < 3)
                banks = sorted({r["bank"] for r in recs})
                mapinfo = "map:pad=%-4d untex=%-3d banks=%s" % (
                    pad, untex, ",".join(str(b) for b in banks[:4]))
                if mism:
                    problems.append("%d map/mesh vertex-count mismatches"
                                    % mism)
                if pad < 0:
                    problems.append("map too short by %d" % -pad)
            except Exception as e:
                problems.append("map: %s" % e)

        print("%-18s %s v=%-5d f=%-5d sides=%-8s %s %s"
              % (name, "OK  " if not problems else "FAIL", nv,
                 len(m["faces"]), ",".join(str(s) for s in sizes),
                 mapinfo, "; ".join(problems)))
        if problems:
            bad += 1
    print("\n%d file(s) failed." % bad)
    return 1 if bad else 0


def stats(paths):
    """CSV so .map sizes can be correlated against mesh counts."""
    print("name,verts,faces,tris,quads,ngons,flagset,3df_size,map_size")
    for p in paths:
        try:
            m = parse(p)
        except Exception:
            continue
        mp = os.path.splitext(p)[0] + ".MAP"
        if not os.path.isfile(mp):
            mp = os.path.splitext(p)[0] + ".map"
        msz = os.path.getsize(mp) if os.path.isfile(mp) else -1
        sides = [len(i) for _, i in m["faces"]]
        flags = sorted({f for f, _ in m["faces"]})
        print("%s,%d,%d,%d,%d,%d,%s,%d,%d"
              % (os.path.basename(p), len(m["verts"]), len(m["faces"]),
                 sides.count(3), sides.count(4),
                 sum(1 for s in sides if s > 4),
                 "|".join(str(f) for f in flags[:6]),
                 m["size"], msz))


def info(path):
    m = parse(path)
    print("  version   %d" % m["version"])
    print("  vertices  %d" % len(m["verts"]))
    print("  faces     %d" % len(m["faces"]))
    print("  consumed  %d of %d bytes" % (m["consumed"], m["size"]))
    xs = [v[0] for v in m["verts"]]
    ys = [v[1] for v in m["verts"]]
    zs = [v[2] for v in m["verts"]]
    if xs:
        print("  bounds    x %.3f .. %.3f" % (min(xs), max(xs)))
        print("            y %.3f .. %.3f" % (min(ys), max(ys)))
        print("            z %.3f .. %.3f" % (min(zs), max(zs)))
    for i, (flags, idx) in enumerate(m["faces"][:8]):
        print("  face %-3d  n=%d flags=%-5d %s" % (i, len(idx), flags, idx))


def obj(path, out):
    m = parse(path)
    with open(out, "w") as f:
        for v in m["verts"]:
            f.write("v %.6f %.6f %.6f\n" % v)
        for flags, idx in m["faces"]:
            f.write("f %s\n" % " ".join(str(i + 1) for i in idx))
    print("wrote %s: %d verts, %d faces"
          % (out, len(m["verts"]), len(m["faces"])))


def main():
    ap = argparse.ArgumentParser(description="MotoRacer .3df reader")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--info", action="store_true")
    ap.add_argument("--stats", action="store_true")
    ap.add_argument("--obj")
    ap.add_argument("paths", nargs="+")
    a = ap.parse_args()
    if a.obj:
        obj(a.paths[0], a.obj)
        return 0
    if a.info:
        info(a.paths[0])
        return 0
    if a.stats:
        stats(a.paths)
        return 0
    return check(a.paths)


if __name__ == "__main__":
    sys.exit(main())
