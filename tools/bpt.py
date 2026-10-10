#!/usr/bin/env python3
"""
bpt.py -- decoder for the MotoRacer .bpt binary script format.

A .bpt is the compiled form of a text script the source calls .spt. The
loader rewrites any requested path to the .BPT extension unless the name
contains .DEM, so "Data\\Config.Spt" opens DATA\\CONFIG.BPT.

The obfuscation is a byte-wise XOR against a Park-Miller "minimal
standard" Lehmer generator, multiplier 16807, modulus 2^31-1, seeded with
0x1a2b3c4d when the file is opened (FUN_004242a0 -> FUN_004b1140(6, ...)).
FUN_0045a4a0 applies it, using the identity ~(a & k) & (a | k), which is
XOR written out longhand.

Values are stored positionally with no names: the binary form is read by
taking the next value in sequence, which is why the names in
Settings_LoadAll never appear in the file.

Records are a mix of 32-bit values and strings. A string is a PLAINTEXT
little-endian u32 length followed by that many masked bytes. The length
is not part of the RNG stream, so the generator must not be advanced
across it -- that is why a straight byte-for-byte decode of a file
containing strings desynchronises after the first one.

Usage:
    python tools/bpt.py --dump    PATH     decoded 32-bit values
    python tools/bpt.py --records PATH     values and strings, in order
    python tools/bpt.py --raw     PATH     decoded bytes, hex + ascii
    python tools/bpt.py --scan    PATH     try both stream alignments
"""

import argparse
import os
import struct
import sys

SEED = 0x1A2B3C4D
MULT = 16807


class Lehmer(object):
    """Reproduces FUN_004b1160, including Schrage's overflow fixup and
    the fact that the state keeps the full value while the *returned*
    value is reduced mod 0x7fff."""

    def __init__(self, seed=SEED):
        self.s = seed

    def next(self):
        s = self.s
        hi = (s >> 16) * MULT            # arithmetic shift; s stays >= 0
        lo = (s & 0xFFFF) * MULT
        r = ((hi & 0x7FFF) << 16) + lo + (hi >> 15)
        r &= 0xFFFFFFFF
        if r & 0x80000000:               # signed: negative
            r = (r - 0x7FFFFFFF) & 0xFFFFFFFF
        self.s = r
        v = r
        if v > 0x7FFF:
            v %= 0x7FFF
        return v


def decode(data, skip=0):
    """XOR every byte with the low byte of successive RNG outputs.

    skip advances the generator before decoding, for testing whether the
    stream starts at the first byte of the file or after a plain header.
    """
    g = Lehmer()
    for _ in range(skip):
        g.next()
    return bytes(b ^ (g.next() & 0xFF) for b in data)


def show(data, limit=40):
    out = []
    n = min(len(data) // 4, limit)
    for i in range(n):
        v = struct.unpack_from("<i", data, i * 4)[0]
        f = struct.unpack_from("<f", data, i * 4)[0]
        fs = ("%.4g" % f) if 1e-6 < abs(f) < 1e9 else ""
        out.append("  [%2d] %11d  0x%08x  %s" % (i, v, v & 0xFFFFFFFF, fs))
    return "\n".join(out)


def records(data, max_str=64):
    """Walk a .bpt as a stream of 32-bit values and strings.

    A string is a plaintext little-endian u32 length followed by that many
    masked bytes; the generator does not advance over the length. Nothing
    in the file marks which is which -- the reader knows only because the
    caller invokes Script_ReadValue, Script_ReadFloat or Script_ReadString
    in a fixed order. So this guesses, but it verifies before committing:
    a string is accepted only if its bytes actually decode to printable
    text. The generator state is a single integer, so the trial decode is
    free and is rolled back on rejection.

    That check matters. Taking any plausible length as a string, without
    decoding it, misfires on files full of small integers and desynchronises
    the stream from that point on.

    Returns a list of ("int", v) and ("str", s) pairs.
    """
    g = Lehmer()
    out = []
    p = 0
    while p + 4 <= len(data):
        n = struct.unpack_from("<I", data, p)[0]
        if 1 <= n <= max_str and p + 4 + n <= len(data):
            saved = g.s
            trial = bytes(data[p + 4 + i] ^ (g.next() & 0xFF) for i in range(n))
            if all(32 <= c < 127 for c in trial):
                out.append(("str", trial.decode("latin-1")))
                p += 4 + n
                continue
            g.s = saved
        b = bytes(data[p + i] ^ (g.next() & 0xFF) for i in range(4))
        out.append(("int", struct.unpack("<i", b)[0]))
        p += 4
    return out


def main():
    ap = argparse.ArgumentParser(description="MotoRacer .bpt decoder")
    ap.add_argument("--dump", action="store_true")
    ap.add_argument("--raw", action="store_true")
    ap.add_argument("--scan", action="store_true")
    ap.add_argument("--records", action="store_true")
    ap.add_argument("--skip", type=int, default=0)
    ap.add_argument("paths", nargs="+")
    a = ap.parse_args()

    for p in a.paths:
        raw = open(p, "rb").read()
        print("== %s  (%d bytes)" % (os.path.basename(p), len(raw)))
        if a.scan:
            for skip in (0, 4):
                d = decode(raw[4:] if skip else raw, 0)
                print("-- stream from offset %d" % (4 if skip else 0))
                print(show(d, 12))
            continue
        if a.records:
            for i, (kind, v) in enumerate(records(raw)):
                if kind == "str":
                    print("  [%3d] str  %r" % (i, v))
                else:
                    print("  [%3d] int  %d" % (i, v))
            continue
        body = raw[4:] if a.skip else raw
        d = decode(body, 0)
        if a.raw:
            for i in range(0, min(len(d), 256), 16):
                chunk = d[i:i + 16]
                hexs = " ".join("%02x" % b for b in chunk)
                txt = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
                print("  %04d  %-47s |%s|" % (i, hexs, txt))
        else:
            print(show(d))
    return 0


if __name__ == "__main__":
    sys.exit(main())
