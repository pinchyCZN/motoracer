#!/usr/bin/env python3
"""
trk.py -- reader for the MotoRacer .trk track format.

Structure taken from FUN_0040d650 (the loader) and its read helpers:

    FUN_004902f0   12 bytes   vec3
    FUN_00490320   48 bytes   4x3 matrix
    FUN_004b1120   16 bytes   vec3 + scalar, a plane
    FUN_00422a20              a mesh: vertex count, then 16 bytes per
                              vertex, then optionally a face count and
                              58 bytes per face
    FUN_00423240   58 bytes   one face, version < 6 only

The file is a versioned scene graph. Sections are gated on the version
word, with separate guards for > 0, > 1, > 2, > 3 and > 4; all shipped
tracks are version 5.

Usage:
    python tools/trk.py --check PATH [PATH ...]
    python tools/trk.py --info  PATH
"""

import argparse
import os
import struct
import sys


class Reader(object):
    def __init__(self, data):
        self.d = data
        self.p = 0

    def take(self, n):
        if self.p + n > len(self.d):
            raise ValueError("read of %d at %d overruns file (%d)"
                             % (n, self.p, len(self.d)))
        v = self.d[self.p:self.p + n]
        self.p += n
        return v

    def u32(self):
        return struct.unpack("<I", self.take(4))[0]

    def i32(self):
        return struct.unpack("<i", self.take(4))[0]


def read_mesh(r, ver, with_faces=True, collect=None):
    """FUN_00422a20. When collect is a list, geometry is appended to it.

    A face is 58 bytes but the loader reads its fields out of order, so
    the file layout is not the struct layout:

        file  0  ->  +0x00   flags
        file  2  ->  +0x38
        file  4  ->  +0x02   colour, RGB555
        file  6  ->  +0x36
        file  8  ->  +0x2c   four u16 vertex indices, then a marker
        file 18  ->  +0x04   40 bytes, uv and shading data

    The marker at struct +0x34, file offset 16, is -1 for a triangle and
    anything else for a quad. That is exactly the test the .map loader
    applies in FUN_00420a50.
    """
    nv = r.u32()
    if nv < 0 or nv > 1 << 22:
        raise ValueError("implausible vertex count %d at %d" % (nv, r.p))
    vblock = r.take(nv * 16)             # vec3 + one dword per vertex
    nf = 0
    verts = []
    faces = []
    if collect is not None:
        for i in range(nv):
            verts.append(struct.unpack_from("<fff", vblock, i * 16))
    if with_faces:
        nf = r.u32()
        if nf < 0 or nf > 1 << 22:
            raise ValueError("implausible face count %d at %d" % (nf, r.p))
        if ver >= 6:
            raise ValueError("version %d faces not modelled" % ver)
        fblock = r.take(nf * 58)
        if collect is not None:
            for i in range(nf):
                o = i * 58
                idx = struct.unpack_from("<4H", fblock, o + 8)
                marker = struct.unpack_from("<h", fblock, o + 16)[0]
                colour = struct.unpack_from("<H", fblock, o + 4)[0]
                n = 3 if marker == -1 else 4
                faces.append((list(idx[:n]), colour))
    if collect is not None:
        collect.append((verts, faces))
    return nv, nf


def parse(path, collect=None):
    r = Reader(open(path, "rb").read())
    if r.take(4) != b"TRK\x00":
        raise ValueError("bad magic")
    ver = r.i32()
    nseg = r.u32()

    idx_total = 0
    for _ in range(nseg):
        r.take(12)                       # vec3
        r.take(48)                       # 4x3 matrix
        r.take(4)
        r.take(4 * 16)                   # four planes
        r.take(4)
        if ver > 0:
            n = r.u32()
            idx_total += n
            r.take(n * 2)
        if ver > 3:
            r.take(8)

    nmesh = r.u32()
    mesh_v = mesh_f = 0
    for _ in range(nmesh):
        v, f = read_mesh(r, ver, collect=collect)
        mesh_v += v
        mesh_f += f

    nnamed = ninst = 0
    if ver > 0:
        nnamed = r.u32()
        for _ in range(nnamed):
            ln = r.u32()
            r.take(ln)                   # name
            for _ in range(3):
                read_mesh(r, ver)
        ninst = r.u32()
        for _ in range(ninst):
            ln = r.u32()
            r.take(ln)                   # name
            r.take(12)                   # vec3
            r.take(12)                   # three dwords
            r.take(4)                    # index into the named arrays
            if ver > 4:
                r.take(4)

    nlink = 0
    if ver > 1:
        nlink = r.u32()
        for _ in range(nlink):
            r.take(6)                    # three u16
            if ver > 3:
                r.take(16)               # four dwords

    # FUN_0048b7e0, animDecor.cpp. Two dwords, then an optional block
    # introduced by a 10-byte "AnimDecor" tag; if the tag is absent the
    # loader seeks back 10 bytes and the section is simply not there.
    nanim = 0
    anim_paths = 0
    if ver > 2:
        r.take(8)
        tag = r.take(10)
        if tag[:9] == b"AnimDecor":
            sub = r.u32()
            if sub > 0:
                nanim = r.u32()
                if nanim > 63:
                    raise ValueError("animated object count %d exceeds the "
                                     "loader's limit of 63" % nanim)
                for _ in range(nanim):
                    rec = r.take(0x20 if sub == 1 else 0x40)
                    kind = (struct.unpack("<I", rec[:4])[0] >> 16) & 0xFF
                    if sub > 2 and kind == 7:
                        npath = r.u32()
                        if npath > 0:
                            if npath > 79:
                                raise ValueError("anim frame count %d "
                                                 "exceeds 79" % npath)
                            nframe = r.u32()
                            anim_paths += npath
                            r.take(npath * nframe * 12)
        else:
            r.p -= 10                    # tag absent, rewind

    tail = len(r.d) - r.p
    return {
        "path": path, "size": len(r.d), "version": ver,
        "segments": nseg, "seg_indices": idx_total,
        "meshes": nmesh, "mesh_verts": mesh_v, "mesh_faces": mesh_f,
        "named": nnamed, "instances": ninst, "links": nlink,
        "anim": nanim, "anim_paths": anim_paths,
        "consumed": r.p, "tail": tail,
    }


def check(paths):
    bad = 0
    for p in paths:
        name = os.path.basename(p)
        try:
            t = parse(p)
        except Exception as e:
            print("%-16s FAIL  %s" % (name, e))
            bad += 1
            continue
        status = "OK  " if t["tail"] == 0 else "FAIL"
        print("%-16s %s v%d seg=%-5d mesh=%-4d verts=%-7d faces=%-7d "
              "named=%-3d inst=%-4d anim=%-3d tail=%d"
              % (name, status, t["version"], t["segments"], t["meshes"],
                 t["mesh_verts"], t["mesh_faces"], t["named"],
                 t["instances"], t["anim"], t["tail"]))
        if t["tail"] != 0:
            bad += 1
    print("\n%d file(s) failed." % bad)
    return 1 if bad else 0


def obj(path, out):
    """Export the track's meshes as one OBJ.

    Each mesh keeps its own vertex array in the file, so indices are
    rebased per mesh as they are appended. No segment transform is
    applied: this exports the meshes as stored, which is what is needed
    to confirm the geometry is being read correctly.
    """
    meshes = []
    t = parse(path, collect=meshes)
    base = 0
    nv = nf = 0
    with open(out, "w") as f:
        f.write("# %s: %d meshes from %s\n"
                % (os.path.basename(out), len(meshes),
                   os.path.basename(path)))
        for mi, (verts, faces) in enumerate(meshes):
            if not verts:
                continue
            f.write("g mesh%03d\n" % mi)
            for v in verts:
                f.write("v %.4f %.4f %.4f\n" % v)
            for idx, colour in faces:
                if any(i >= len(verts) for i in idx):
                    continue             # skip a face that indexes out
                f.write("f %s\n"
                        % " ".join(str(base + i + 1) for i in idx))
                nf += 1
            base += len(verts)
            nv += len(verts)
    print("wrote %s: %d meshes, %d verts, %d faces"
          % (out, len(meshes), nv, nf))
    return t


def info(path):
    t = parse(path)
    for k in ("version", "size", "segments", "seg_indices", "meshes",
              "mesh_verts", "mesh_faces", "named", "instances", "links",
              "consumed", "tail"):
        print("  %-12s %s" % (k, t[k]))


def main():
    ap = argparse.ArgumentParser(description="MotoRacer .trk reader")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--info", action="store_true")
    ap.add_argument("--obj", metavar="OUT.obj")
    ap.add_argument("paths", nargs="+")
    a = ap.parse_args()
    if a.obj:
        obj(a.paths[0], a.obj)
        return 0
    if a.info:
        info(a.paths[0])
        return 0
    return check(a.paths)


if __name__ == "__main__":
    sys.exit(main())
