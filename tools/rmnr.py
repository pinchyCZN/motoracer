#!/usr/bin/env python3
"""
rmnr.py -- reader for the MotoRacer obfuscated .WAV files.

The 250 sound files in SFX.BKF are named .WAV but begin with the magic
"rmnr" instead of "RIFF". They are ordinary PCM RIFF WAVE files with a
positional cipher applied.

From FUN_00440d30, the loader, and FUN_00440d00, the transform:

    for (i = 0; i < len; i++)
        buf[i] = buf[i] + (-8 - i) * 4;

so plain[i] = (cipher[i] - 0x20 - 4*i) & 0xff. The key repeats every 64
bytes and the counter restarts at zero on every call.

The loader decodes three regions independently, each restarting i at 0:

    1. the first 0x24 bytes, the RIFF/WAVE/fmt header
    2. the 8-byte data chunk header, after seeking past any fmt bytes
       beyond the standard 16
    3. the payload

Bytes between the two headers, the cbSize field of an 18-byte fmt chunk,
are seeked over and never decoded.

The trigger is a lowercase 'r' in byte 0: FUN_00440d30 tests
(char)hdr[0] == 'r' and skips the transform when it is 'R'. The game
therefore reads plain RIFF files too.

Usage:
    python tools/rmnr.py --check PATH [PATH ...]
    python tools/rmnr.py --info  PATH
    python tools/rmnr.py --wav OUT.wav PATH
"""

import argparse
import os
import struct
import sys

HDR = 0x24          # bytes the loader reads for the RIFF/WAVE/fmt header


def unmask(buf):
    """FUN_00440d00, with the counter restarting at zero."""
    return bytes((b - 0x20 - 4 * i) & 0xFF for i, b in enumerate(buf))


def parse(path):
    t = parse_bytes(open(path, "rb").read())
    t["path"] = path
    return t


def parse_bytes(raw):
    """The same reader over bytes already in hand - so a member can
    come straight out of SFX.BKF, which is where the GAME reads it
    from. Rule 10: the extracted tree is not shipped data."""
    if len(raw) < HDR + 8:
        raise ValueError("too short: %d bytes" % len(raw))

    # FUN_00440d30 keys the whole thing off one lowercase letter.
    masked = raw[0] == ord("r")
    hdr = unmask(raw[:HDR]) if masked else raw[:HDR]

    riff, riff_sz, wave, fmt, fmt_sz = struct.unpack_from("<4sI4s4sI", hdr, 0)
    if riff != b"RIFF" or wave != b"WAVE" or fmt != b"fmt ":
        raise ValueError("not RIFF/WAVE/fmt after unmasking: %r %r %r"
                         % (riff, wave, fmt))

    tag, ch, rate, avg, align, bits = struct.unpack_from("<HHIIHH", hdr, 0x14)

    # Seek CUR by fmt_sz - 16, past cbSize on an 18-byte fmt chunk.
    dpos = HDR + (fmt_sz - 16)
    dhdr = raw[dpos:dpos + 8]
    if len(dhdr) < 8:
        raise ValueError("data chunk header runs off the end")
    if masked:
        dhdr = unmask(dhdr)
    dtag, dsz = struct.unpack("<4sI", dhdr)
    if dtag != b"data":
        raise ValueError("no data chunk at %d: %r" % (dpos, dtag))

    body = raw[dpos + 8:dpos + 8 + dsz]
    if len(body) != dsz:
        raise ValueError("data chunk wants %d bytes, %d present"
                         % (dsz, len(body)))
    pcm = unmask(body) if masked else body

    rest = raw[dpos + 8 + dsz:]
    if dsz & 1:
        rest = rest[1:]              # RIFF word-alignment pad

    return {
        "path": None, "size": len(raw), "masked": masked,
        "trailing": rest[:4],
        "riff_size": riff_sz, "fmt_size": fmt_sz, "format": tag,
        "channels": ch, "rate": rate, "avg_bytes": avg,
        "block_align": align, "bits": bits,
        "data_at": dpos + 8, "data_size": dsz, "pcm": pcm,
        "tail": len(raw) - (dpos + 8 + dsz),
    }


def consistent(t):
    """The checks the loader's own arithmetic implies."""
    bad = []
    if t["riff_size"] != t["size"] - 8:
        bad.append("riff size %d != file - 8 (%d)"
                   % (t["riff_size"], t["size"] - 8))
    if t["format"] != 1:
        bad.append("format tag %d is not PCM" % t["format"])
    align = (t["bits"] // 8) * t["channels"]
    if t["block_align"] != align:
        bad.append("blockAlign %d != %d" % (t["block_align"], align))
    if t["avg_bytes"] != align * t["rate"]:
        bad.append("avgBytes %d != %d" % (t["avg_bytes"], align * t["rate"]))
    # An odd-sized data chunk is followed by a RIFF word-alignment pad
    # byte, and three files carry a plaintext LIST/INFO chunk after that.
    # The loader stops at the end of the data chunk and never sees
    # either, which is why the trailing chunks were left unmasked.
    tail = t["tail"] - (t["data_size"] & 1)
    if tail and t["trailing"] != b"LIST":
        bad.append("%d unexplained trailing bytes" % tail)
    return bad


def wav(t):
    """Rebuild a standard 16-byte-fmt PCM RIFF file."""
    n = len(t["pcm"])
    return (struct.pack("<4sI4s4sIHHIIHH4sI",
                        b"RIFF", 36 + n, b"WAVE", b"fmt ", 16,
                        1, t["channels"], t["rate"], t["avg_bytes"],
                        t["block_align"], t["bits"], b"data", n) + t["pcm"])


def check(paths):
    bad = 0
    for p in paths:
        name = p.replace("\\", "/").split("/")[-1]
        try:
            t = parse(p)
        except Exception as e:
            print("%-16s FAIL  %s" % (name, e))
            bad += 1
            continue
        problems = consistent(t)
        print("%-16s %s %s %5d Hz %2d bit %dch  data=%-7d %s"
              % (name, "OK  " if not problems else "FAIL",
                 "masked" if t["masked"] else "plain ",
                 t["rate"], t["bits"], t["channels"], t["data_size"],
                 "; ".join(problems)))
        if problems:
            bad += 1
    print("\n%d file(s) failed." % bad)
    return 1 if bad else 0


def info(path):
    t = parse(path)
    for k in ("size", "masked", "riff_size", "fmt_size", "format",
              "channels", "rate", "avg_bytes", "block_align", "bits",
              "data_at", "data_size", "tail", "trailing"):
        print("  %-12s %s" % (k, t[k]))
    for problem in consistent(t):
        print("  PROBLEM      %s" % problem)


def table(archive, want):
    """Emit the line src/wave.c logs, for every matching member.

    THE FORMAT IS COPIED FROM wave_log ON PURPOSE. The port logs
    one of these per sample it loads and this prints the same from
    the oracle, so verifying the C decoder is a literal diff rather
    than a reading of two different tables."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import bkf

    raw = open(archive, "rb").read()
    ents = bkf.read_dir(archive)[1]
    hits = 0
    for name, off, size in sorted(ents):
        if want and want.upper() not in name.upper():
            continue
        try:
            t = parse_bytes(raw[off:off + size])
        except Exception as e:
            print("wave:   %-28s PARSE FAILED %s" % (name, e))
            continue
        hits += 1
        print("wave:   %-28s %5d Hz %2d bit %dch %7d bytes"
              % (name, t["rate"], t["bits"],
                 t["channels"], len(t["pcm"])))
    return 0 if hits else 1


def main():
    ap = argparse.ArgumentParser(description="MotoRacer rmnr .WAV reader")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--info", action="store_true")
    ap.add_argument("--wav", metavar="OUT.wav")
    ap.add_argument("--table", metavar="ARCHIVE",
                    help="read members from a .BKF and print the line "
                         "src/wave.c logs, for a known-answer diff")
    ap.add_argument("--match", default="",
                    help="substring filter for --table, e.g. MENU")
    ap.add_argument("paths", nargs="*")
    a = ap.parse_args()
    if a.table:
        return table(a.table, a.match)
    if a.wav:
        t = parse(a.paths[0])
        open(a.wav, "wb").write(wav(t))
        print("wrote %s: %d Hz %d bit %dch, %d bytes of PCM"
              % (a.wav, t["rate"], t["bits"], t["channels"], len(t["pcm"])))
        return 0
    if a.info:
        info(a.paths[0])
        return 0
    return check(a.paths)


if __name__ == "__main__":
    sys.exit(main())
