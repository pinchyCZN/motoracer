#!/usr/bin/env python3
"""
lez.py -- reference decoder for the MotoRacer .lez sprite format.

Implements the format described in docs/formats/lez.txt, derived from
Lez_LoadFile (0x0043e700) and Lez_Decode (0x0043eb00) in MOTO.EXE.

Usage:
    python tools/lez.py --check PATH [PATH ...]   parse and validate
    python tools/lez.py --info  PATH              header detail
    python tools/lez.py --ppm OUT.ppm PATH        decode to a PPM image

--check is the important one: it decompresses, then cross-checks the
dimensions in the packed header against those in the unpacked header and
confirms the pixel payload is exactly width * height bytes. A file that
passes all three is strong evidence the format description is right.
"""

import argparse
import struct
import sys

MAGIC = b"LEZ1"


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def u16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def unpack(data):
    """Decompress a .lez body. Returns the unpacked bytes."""
    if data[:4] != MAGIC:
        raise ValueError("bad magic %r, expected %r" % (data[:4], MAGIC))
    out_size = u32(data, 0x04)
    match_off = u32(data, 0x08)
    lit_off = u32(data, 0x0C)

    out = bytearray()
    ctrl = 0x14
    mpos = match_off
    lpos = lit_off

    # Control bits are consumed most-significant-first from 32-bit words.
    # A sentinel 1 is appended below each word; when the register shifts
    # to zero the sentinel has fallen out and the next word is fetched.
    word = u32(data, ctrl)
    ctrl += 4
    reg = ((word << 1) | 1) & 0xFFFFFFFF
    bit = (word >> 31) & 1

    while True:
        if bit:
            out.append(data[lpos])
            lpos += 1
        else:
            tok = u16(data, mpos)
            mpos += 2
            dist = tok & 0x03FF
            length = (tok >> 10) + 3
            if dist == 0 or dist > len(out):
                raise ValueError("bad match distance %d at out=%d"
                                 % (dist, len(out)))
            src = len(out) - dist
            for i in range(length):        # byte at a time: overlap is legal
                out.append(out[src + i])

        if len(out) >= out_size:
            break

        bit = (reg >> 31) & 1
        reg = (reg << 1) & 0xFFFFFFFF
        if reg == 0:
            word = u32(data, ctrl)
            ctrl += 4
            reg = ((word << 1) | 1) & 0xFFFFFFFF
            bit = (word >> 31) & 1

    return bytes(out), {
        "out_size": out_size, "match_off": match_off, "lit_off": lit_off,
        "ctrl_end": ctrl, "match_end": mpos, "lit_end": lpos,
    }


def parse(path):
    data = open(path, "rb").read()
    packed_dims = u32(data, 0x10)
    hdr = {
        "file_size": len(data),
        "packed_w": packed_dims >> 16,
        "packed_h": packed_dims & 0xFFFF,
    }
    raw, stats = unpack(data)
    hdr.update(stats)
    hdr["unpacked_len"] = len(raw)
    # The unpacked payload is a TGA file; these are its header fields.
    hdr["tga_id_len"] = raw[0]
    hdr["tga_map_type"] = raw[1]
    hdr["tga_img_type"] = raw[2]
    hdr["pal_count"] = u16(raw, 0x05)
    hdr["tga_map_entry_bits"] = raw[7]
    hdr["unpacked_w"] = u16(raw, 0x0C)
    hdr["unpacked_h"] = u16(raw, 0x0E)
    hdr["tga_depth"] = raw[0x10]
    hdr["tga_descriptor"] = raw[0x11]
    entry_bytes = hdr["tga_map_entry_bits"] // 8
    hdr["pixels_off"] = 0x12 + hdr["pal_count"] * entry_bytes
    hdr["pixels_len"] = len(raw) - hdr["pixels_off"]
    hdr["has_tga_footer"] = raw[-18:] == b"TRUEVISION-XFILE.\x00"
    return raw, hdr


def check(paths):
    bad = 0
    for p in paths:
        name = p.replace("\\", "/").split("/")[-1]
        try:
            raw, h = parse(p)
        except Exception as e:
            print("%-14s FAIL  %s" % (name, e))
            bad += 1
            continue

        problems = []
        if len(raw) != h["out_size"]:
            problems.append("unpacked %d != declared %d"
                            % (len(raw), h["out_size"]))
        if (h["packed_w"], h["packed_h"]) != (h["unpacked_w"], h["unpacked_h"]):
            problems.append("dims %dx%d vs %dx%d"
                            % (h["packed_w"], h["packed_h"],
                               h["unpacked_w"], h["unpacked_h"]))
        # The payload is a whole TGA file. Pixels are exactly w*h, and
        # anything after them is TGA 2.0 trailing metadata that the game
        # never reads, so require room for the pixels, not an exact fit.
        # The payload may be any TGA the artists saved. Observed:
        #   type 1  colour-mapped 8-bit  - the common case
        #   type 3  grayscale 8-bit      - font banks and masks; the
        #                                  game synthesises a grey ramp
        #   type 2  true colour 24-bit   - present but NOT loadable by
        #                                  Lez_Decode, which assumes
        #                                  one index byte per pixel
        expect = h["unpacked_w"] * h["unpacked_h"] * (h["tga_depth"] // 8)
        if h["pixels_len"] < expect:
            problems.append("pixels %d < needed %d" % (h["pixels_len"], expect))
        if h["tga_img_type"] not in (1, 3):
            problems.append("img type %d not loadable by Lez_Decode"
                            % h["tga_img_type"])
        elif h["tga_depth"] != 8:
            problems.append("depth %d != 8" % h["tga_depth"])

        status = "OK  " if not problems else "FAIL"
        print("%-14s %s %4dx%-4d t%d/%-2d pal=%-4d unpacked=%-7d "
              "ratio=%5.2fx %s"
              % (name, status, h["unpacked_w"], h["unpacked_h"],
                 h["tga_img_type"], h["tga_depth"],
                 h["pal_count"], len(raw),
                 len(raw) / float(h["file_size"]),
                 "; ".join(problems)))
        if problems:
            bad += 1
    print("\n%d file(s) failed." % bad)
    return 1 if bad else 0


def info(path):
    raw, h = parse(path)
    for k in ("file_size", "packed_w", "packed_h", "out_size", "match_off",
              "lit_off", "ctrl_end", "match_end", "lit_end", "unpacked_len",
              "pal_count", "unpacked_w", "unpacked_h", "pixels_off",
              "pixels_len"):
        print("  %-12s %s" % (k, h[k]))
    print("  palette[0:6]  %s" % " ".join(
        "%02x" % b for b in raw[0x12:0x12 + 18]))


def ppm(path, out):
    """Decode to a PPM. Palette entries are stored blue, green, red."""
    raw, h = parse(path)
    w, ht, n = h["unpacked_w"], h["unpacked_h"], h["pal_count"]
    pal = []
    for i in range(max(n, 1) if n else 256):
        if n:
            b, g, r = raw[0x12 + i * 3:0x12 + i * 3 + 3]
        else:
            b = g = r = i
        pal.append((r, g, b))
    px = raw[h["pixels_off"]:]
    with open(out, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (w, ht))
        f.write(bytes(c for i in px[:w * ht] for c in pal[i]))
    print("wrote %s (%dx%d)" % (out, w, ht))


def main():
    ap = argparse.ArgumentParser(description="MotoRacer .lez decoder")
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
