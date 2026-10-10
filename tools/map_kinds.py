"""map_kinds.py - apply the decoded .map-flags-to-kind table of
docs/formats/mesh-3df-map.txt section 5 to every shipped .map and report
the distribution.

    python tools/map_kinds.py

Checks the decode against real data rather than only against the
decompilation, and answers "which kinds does the game actually produce".

ONE TRAP IT AVOIDS. 123 of the 124 paired .map files carry one extra
128-byte block past the last face record. Load3dfAndMap reads exactly
512 + faces * 128 bytes and ignores the rest, so that block is NOT a
face. Counting it adds ~600 phantom records and shifts every total -
kind 0x0d came out as 179 rather than 119 before this was bounded by
the paired .3df's real face count.
"""
import struct, glob, os, collections

ROOT = r'N:\DEV\MOTORACER\decomp\DATA\extracted'

BITS = [(0x010000, 'no-filter'), (0x020000, 'second-pass'),
        (0x040000, 'kind 0x0d'), (0x080000, 'translucent'),
        (0x100000, 'no-reduce'), (0x200000, 'tier3'),
        (0x400000, 'depth-bias')]


def kind_for(flags, n, select, bank):
    """Load3dfAndMap, post-constructor block."""
    if n < 3:
        k = 0                                   # Prim_SetKind(0)
        if flags & 0x010000:
            k = 2
        if flags & 0x040000:
            k = 0x0d
        if flags & 0x080000:
            k = 0x0c
        return k
    tier2 = (select != 0 and bank != 0)
    if not (flags & 0x010000):
        k = 0x10 if tier2 else 1
        if flags & 0x200000:
            k = 0x15
    else:
        k = 0x26 if tier2 else 2
        if flags & 0x200000:
            k = 0x27
    if flags & 0x040000:
        k = 0x0d
    if flags & 0x080000:
        k = 8
    return k


def face_count(path3df):
    """Walk the .3df face list: u16 n / u16 flags / u16 index[n], n==0 ends."""
    d = open(path3df, 'rb').read()
    if len(d) < 4 or struct.unpack_from('<H', d, 0)[0] != 1:
        return None
    nv = struct.unpack_from('<H', d, 2)[0]
    o = 4 + nv * 12
    n_faces = 0
    while o + 4 <= len(d):
        n = struct.unpack_from('<H', d, o)[0]
        if n == 0 or o + 4 + n*2 > len(d):
            break
        n_faces += 1
        o += 4 + n*2
    return n_faces


files = glob.glob(os.path.join(ROOT, '**', '*.MAP'), recursive=True)
unpaired = 0
kinds0 = collections.Counter()
kinds1 = collections.Counter()
bitcount = collections.Counter()
small = total = 0
combos = collections.Counter()
banks = collections.Counter()

for path in files:
    d = open(path, 'rb').read()
    # The loader reads exactly 512 + faces*128 and IGNORES the rest, so
    # bound by the paired mesh - otherwise the conventional trailing
    # 128-byte block counts as a face record and skews everything.
    p3 = path[:-4] + '.3DF'
    if not os.path.exists(p3):
        unpaired += 1
        continue
    n_rec = face_count(p3)
    if n_rec is None:
        unpaired += 1
        continue
    n_rec = min(n_rec, (len(d) - 0x200) // 0x80)
    for i in range(n_rec):
        r = d[0x200 + i*0x80: 0x200 + (i+1)*0x80]
        if len(r) < 0x80:
            break
        n = struct.unpack_from('<I', r, 0)[0]
        fl = struct.unpack_from('<I', r, 0x48)[0]
        bank = fl & 0xffff
        total += 1
        if n < 3:
            small += 1
        banks[bank] += 1
        hi = fl & 0xffff0000
        combos[hi] += 1
        for m, name in BITS:
            if fl & m:
                bitcount[name] += 1
        kinds0[kind_for(fl, n, 0, bank)] += 1
        kinds1[kind_for(fl, n, 1, bank)] += 1

print(f'{len(files)} .map files, {unpaired} unpaired/skipped, '
      f'{total} face records, {small} with n < 3')
print()
print('flag bits set, over all records:')
for m, name in BITS:
    print(f'  {m:#08x} {name:<14} {bitcount[name]:>6}')
print()
print('distinct high-flag combinations:')
for hi, c in sorted(combos.items(), key=lambda kv: -kv[1]):
    names = ' | '.join(n for m, n in BITS if hi & m) or '(none)'
    print(f'  {hi:#010x}  {c:>6}  {names}')
print()
print('resulting kinds, faceKindSelect = 0:')
for k, c in sorted(kinds0.items()):
    print(f'  0x{k:02x}  {c:>6}')
print('resulting kinds, faceKindSelect = 1:')
for k, c in sorted(kinds1.items()):
    print(f'  0x{k:02x}  {c:>6}')
print()
print('bank indices:', dict(sorted(banks.items())))
