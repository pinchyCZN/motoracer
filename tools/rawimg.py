#!/usr/bin/env python3
"""
rawimg.py -- reader for the .RAW texture pages ("mhwanh" format).

The 32-byte header is big-endian, which is unusual for an x86 game and
suggests the format came from a Commodore or Atari lineage:

    +0x00  char[6]  magic "mhwanh"
    +0x06  u16      version, always 4 in the shipped data
    +0x08  u16      width
    +0x0a  u16      height
    +0x0c  u16      colour count; 0 means 24-bit truecolour
    +0x0e  u16      horizontal resolution, dpi
    +0x10  u16      vertical resolution, dpi
    +0x12  14 bytes reserved, zero in every file seen
    +0x20  palette, colours * 3 bytes RGB, present only when
           colour count is non-zero
    ...    pixels: 3 bytes each when truecolour, otherwise one
           palette index per pixel

Usage:
    python tools/rawimg.py --check PATH [PATH ...]
    python tools/rawimg.py --info  PATH
    python tools/rawimg.py --ppm OUT.ppm PATH
"""

import argparse
import os
import struct
import sys

MAGIC = b"mhwanh"
HDR = 0x20


def parse(path):
    d = open(path, "rb").read()
    if d[:6] != MAGIC:
        raise ValueError("bad magic %r" % d[:6])
    ver, w, h, ncol, dpix, dpiy = struct.unpack_from(">HHHHHH", d, 6)
    palbytes = ncol * 3
    bpp = 3 if ncol == 0 else 1
    need = HDR + palbytes + w * h * bpp
    return {
        "path": path, "size": len(d), "version": ver, "w": w, "h": h,
        "colours": ncol, "dpi": (dpix, dpiy), "palbytes": palbytes,
        "bpp": bpp, "expected": need, "data": d,
    }


def check(paths):
    bad = 0
    for p in paths:
        name = os.path.basename(p)
        try:
            m = parse(p)
        except Exception as e:
            print("%-22s FAIL  %s" % (name, e))
            bad += 1
            continue
        problems = []
        if m["size"] != m["expected"]:
            problems.append("size %d != expected %d"
                            % (m["size"], m["expected"]))
        if m["version"] != 4:
            problems.append("version %d" % m["version"])
        kind = "true24" if m["colours"] == 0 else "pal%d" % m["colours"]
        print("%-22s %s %4dx%-4d %-8s dpi=%dx%d %s"
              % (name, "OK  " if not problems else "FAIL",
                 m["w"], m["h"], kind, m["dpi"][0], m["dpi"][1],
                 "; ".join(problems)))
        if problems:
            bad += 1
    print("\n%d file(s) failed." % bad)
    return 1 if bad else 0


def info(path):
    m = parse(path)
    for k in ("version", "w", "h", "colours", "dpi", "palbytes",
              "bpp", "expected", "size"):
        print("  %-10s %s" % (k, m[k]))
    print("  reserved   %s"
          % " ".join("%02x" % b for b in m["data"][0x12:0x20]))


def ppm(path, out):
    m = parse(path)
    d, w, h = m["data"], m["w"], m["h"]
    off = HDR + m["palbytes"]
    with open(out, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (w, h))
        if m["colours"] == 0:
            f.write(d[off:off + w * h * 3])
        else:
            pal = d[HDR:HDR + m["palbytes"]]
            px = d[off:off + w * h]
            f.write(bytes(pal[i * 3 + c] for i in px for c in (0, 1, 2)))
    print("wrote %s (%dx%d, %s)"
          % (out, w, h, "truecolour" if m["colours"] == 0
             else "%d-colour" % m["colours"]))


def main():
    ap = argparse.ArgumentParser(description="MotoRacer .RAW reader")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--info", action="store_true")
    ap.add_argument("--ppm")
    ap.add_argument("paths", nargs="+")
    a = ap.parse_args()
    if a.ppm:
        ppm(a.paths[0], a.ppm)
        return 0
    if a.info:
        info(a.paths[0])
        return 0
    return check(a.paths)


if __name__ == "__main__":
    sys.exit(main())
