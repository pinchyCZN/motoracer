"""gen_detail_table.py - emit src/detail_table.inc.

Parses the feature table out of docs/subsystems/detail-levels.txt
section 6, rebuilds the reverse index the way Screen3D_Open (0x004995a0)
does, and CHECKS the result against that section's own "low" and "near"
reduction columns for all seventy kinds before emitting anything. A hand
transcription of seventy rows would not be checkable; this is.

    python tools/gen_detail_table.py src/detail_table.inc

Exits non-zero on any mismatch. Re-run it if section 6 of the note ever
changes - the .inc is generated, not authored.
"""
import re, sys

doc = open(r'N:\DEV\MOTORACER\docs\subsystems\detail-levels.txt',
           encoding='ascii').read()
body = doc[doc.index('6.  The whole table'):
           doc.index('7.  What a port has to do')]

row = re.compile(r'0x([0-9a-f]{2}) ([0-9a-f]{2}) [.X]{8} +'
                 r'(0x[0-9a-f]{2}|-) +(0x[0-9a-f]{2}|-)')
rows = {}
for m in row.finditer(body):
    kind = int(m.group(1), 16)
    bits = int(m.group(2), 16)
    low  = None if m.group(3) == '-' else int(m.group(3), 16)
    near = None if m.group(4) == '-' else int(m.group(4), 16)
    rows[kind] = (bits, low, near)

assert sorted(rows) == list(range(0x46)), sorted(set(range(0x46)) - set(rows))
feature = [rows[k][0] for k in range(0x46)]

# Screen3D_Open's loop: first kind to claim a pattern wins, with ZERO as
# the not-yet-assigned sentinel - which is the bug section 1.1 is about.
reverse = [0] * 0x100
for kind in range(0x46):
    if reverse[feature[kind]] == 0:
        reverse[feature[kind]] = kind

# The two literal stores that bite (section 1.1). featureTable[0] = 0x20
# is already reflected in the section 6 table.
reverse[0x00] = 0
reverse[0x20] = 0

# ---- check against the doc's own reduction columns -------------------
bad = 0
for kind in range(0x46):
    bits, low, near = rows[kind]
    if bits & 0x40:                      # exempt; doc prints a dash
        if low is not None or near is not None:
            print('exempt kind with a reduction column:', hex(kind)); bad += 1
        continue
    got_low  = reverse[bits & ~0x85]
    b = bits | 0x20 if (bits & 1) else bits
    got_near = reverse[b & ~0x85]
    if got_low != low or got_near != near:
        print(f'MISMATCH kind {kind:#04x} bits {bits:#04x}: '
              f'low doc {low:#04x} got {got_low:#04x}, '
              f'near doc {near:#04x} got {got_near:#04x}')
        bad += 1

print(f'{len(rows)} kinds parsed, {bad} mismatches')
if bad:
    sys.exit(1)

# reverseIndex[0] would be 0x16 without the patch - confirm the claim.
r2 = [0] * 0x100
for kind in range(0x46):
    if r2[feature[kind]] == 0:
        r2[feature[kind]] = kind
print('without the patch, reverseIndex[0] =', hex(r2[0]), '(doc says 0x16)')

# ---- emit -----------------------------------------------------------
out = []
out.append('/* Generated from docs/subsystems/detail-levels.txt section 6 by')
out.append(' * tools/gen_detail_table.py, and checked against that notes')
out.append(' * own reduction columns for all 70 kinds. Do not edit. */')
out.append('')
out.append('static const unsigned char kFeatureTable[DETAIL_KIND_COUNT] = {')
for base in range(0, 0x46, 8):
    chunk = feature[base:base+8]
    # A TAB, not four spaces: the project indents with tabs and aligns
    # with spaces (tools/retab.py). Emitting spaces here would make
    # every regeneration of this file undo the convention silently, and
    # a generated file is exactly where that would go unnoticed. The
    # trailing run before the comment stays spaces - that is alignment.
    out.append('\t' + ' '.join('0x%02x,' % v for v in chunk) +
               '   /* 0x%02x */' % base)
out.append('};')
print('\n'.join(out[:6]), '...')

open(sys.argv[1], 'w', encoding='ascii', newline='\n').write(
    '\n'.join(out) + '\n')
print('wrote', sys.argv[1])
