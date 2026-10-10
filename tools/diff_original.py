# diff_original.py - byte-diff the shipped MOTO.EXE against the untouched
# ORIGINAL_MOTO.EXE and report every difference as a virtual address.
#
# The copy of MOTO.EXE this project analyses has been modified: the CD
# protection is defeated and two 16-bit colour-depth gates are relaxed.
# Structural scans (NOP runs, E9+90 tails) find only the length-changing
# patches; three of the seven are equal-length and invisible to them.
# This is the authoritative check - run it after replacing either binary.
#
#   "C:\Program Files\Python310\python.exe" tools/diff_original.py
#
# Exits 1 if the set of differences is not the 7 known sites.

import struct
import sys

ORIG = "N:/DEV/MOTORACER/decomp/ORIGINAL_MOTO.EXE"
CRAK = "N:/DEV/MOTORACER/decomp/MOTO.EXE"

# the seven verified sites, by virtual address
KNOWN = {
    0x00414331: "protection: JNZ -> NOP NOP (PId message box)",
    0x00442785: "protection: XOR EAX,EAX -> JMP (disc check returns 1)",
    0x00442f44: "protection: JZ -> NOP NOP (CD-present local)",
    0x00444e73: "protection: JZ -> NOP NOP (CD-audio playback)",
    0x0045a7ca: "colour depth: JZ -> JGE, insn at 0x0045a7c9 (bpp >= 16)",
    0x0047642d: "colour depth: JZ -> JGE (bpp == 16 becomes bpp >= 16)",
    0x0047e8db: "protection: JZ -> JMP (orphans ILLEGAL VERSION block)",
}

GAP = 8  # bytes of agreement tolerated inside one reported run


def sections(buf):
    e_lfanew = struct.unpack_from("<I", buf, 0x3C)[0]
    if buf[e_lfanew:e_lfanew + 4] != b"PE\0\0":
        raise SystemExit("not a PE file")
    coff = e_lfanew + 4
    nsec = struct.unpack_from("<H", buf, coff + 2)[0]
    optsz = struct.unpack_from("<H", buf, coff + 16)[0]
    opt = coff + 20
    base = struct.unpack_from("<I", buf, opt + 28)[0]
    tab = opt + optsz
    out = []
    for i in range(nsec):
        o = tab + i * 40
        name = buf[o:o + 8].rstrip(b"\0").decode("ascii", "replace")
        vaddr, rawptr, rawsz = (struct.unpack_from("<I", buf, o + 12)[0],
                                struct.unpack_from("<I", buf, o + 20)[0],
                                struct.unpack_from("<I", buf, o + 16)[0])
        out.append((name, vaddr, rawptr, rawsz))
    return base, out


def main():
    a = open(ORIG, "rb").read()
    b = open(CRAK, "rb").read()
    if len(a) != len(b):
        print("sizes differ: %d vs %d" % (len(a), len(b)))
    base, secs = sections(a)

    def to_va(off):
        for name, vaddr, rawptr, rawsz in secs:
            if rawptr <= off < rawptr + rawsz:
                return base + vaddr + (off - rawptr), name
        return None, "<unmapped>"

    diffs = [i for i in range(min(len(a), len(b))) if a[i] != b[i]]
    runs = []
    for i in diffs:
        if runs and i - runs[-1][1] <= GAP:
            runs[-1][1] = i
        else:
            runs.append([i, i])

    print("%d differing byte(s) in %d run(s)\n" % (len(diffs), len(runs)))
    seen = set()
    for lo, end in runs:
        hi = end + 1
        va, sec = to_va(lo)
        seen.add(va)
        tag = KNOWN.get(va, "*** UNKNOWN - INVESTIGATE ***")
        print("VA 0x%08x  [%s]  file 0x%06x  %d byte(s)"
              % (va, sec, lo, hi - lo))
        print("   was: " + " ".join("%02x" % c for c in a[lo:hi]))
        print("   now: " + " ".join("%02x" % c for c in b[lo:hi]))
        print("   %s\n" % tag)

    missing = set(KNOWN) - seen
    extra = seen - set(KNOWN)
    if missing:
        print("MISSING known sites: "
              + ", ".join("0x%08x" % v for v in sorted(missing)))
    if extra:
        print("UNEXPECTED sites: "
              + ", ".join("0x%08x" % v for v in sorted(extra)))
    if missing or extra:
        return 1
    print("all 7 known sites present, nothing else differs")
    return 0


if __name__ == "__main__":
    sys.exit(main())
