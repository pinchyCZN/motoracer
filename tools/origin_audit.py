"""Report ORIGIN tags in src/ that are missing or malformed.

    python tools/origin_audit.py [FILE ...]      default: src/*.c

Rule 13 asks every transcribed function to carry an ORIGIN naming the
Ghidra function it came from, or "NONE - ours".  This finds the ones
that do not, so an untagged function reads as an oversight rather than
as ours by default.

THE BRACE LINE MAY CARRY THE BODY. "static T *f(void)" followed by
"{ return x; }" is a definition too, and requiring a BARE "{" missed
four of them in menu.c.

A ONE-LINE BODY IS STILL A DEFINITION.  "int f(void) { return x; }"
ends with } rather than {, and requiring the brace hid two real gaps
in menu.c - menu_get_activated and menu_clear_activated, both of
which have originals in the DB.

WINDOW is how far above a definition the tag may sit.  A long block
comment can push a real tag past it, so a hit is a prompt to look, not
proof of a missing tag.

A FILE MAY DECLARE ITSELF OURS ONCE, AT THE TOP.  If the LEADING
comment block carries an ORIGIN saying "NONE - ours", every function
in the file is covered and the per-function gaps are not reported -
54 identical one-line tags in agent.c would restate one fact 54
times, which rule 14.1 forbids.  Tags that ARE present are still
shape-checked, so a transcribed function added to such a file still
has to carry its own.

The block must be the file's FIRST, before any code.  A function tag
further down is that function's, not the file's: menu.c's first
ORIGIN is at line 70 and trk.c's at 67, and neither file is ours.

IT ALSO CHECKS THE TAGS THAT EXIST, because those are the likelier
defect: three of menu.c's six pre-existing tags were wrong.  A tag must
carry an address in .text or say "NONE - ours".  What it CANNOT check
is whether the address is the ENTRY rather than a byte inside the
function's body - that needs the Ghidra DB, and it was the third
defect.  Resolve an address before trusting a tag that carries one.
"""

import glob
import re
import sys

WINDOW = 40
TEXT_LO, TEXT_HI = 0x00401000, 0x004cf5ff
TAG = re.compile(r'ORIGIN\s+')
ADDR = re.compile(r'0x00[0-9a-fA-F]{6}')
DEF = re.compile(r'^[A-Za-z_][A-Za-z0-9_ \t\*]*\(')


def block_above(lines, i):
    """The comment block directly above line i, or '' if there is none.

    A tag must be adjacent to what it tags.  Searching a window finds
    the PREVIOUS function's tag and shape-checks the wrong text.
    """
    k = i - 1
    if k < 0 or not lines[k].strip().endswith('*/'):
        return ''
    while k >= 0 and not lines[k].strip().startswith('/*'):
        k -= 1
    return '""" + B + """n'.join(lines[max(k, 0):i])


def file_is_ours(lines):
    """True if the file's LEADING comment block declares NONE - ours."""
    i = 0
    while i < len(lines) and not lines[i].strip():
        i += 1
    if i >= len(lines) or not lines[i].strip().startswith('/*'):
        return False
    blk = []
    while i < len(lines):
        blk.append(lines[i])
        if '*/' in lines[i]:
            break
        i += 1
    txt = ' '.join(blk)
    return 'ORIGIN' in txt and 'NONE - ours' in txt


def audit(path):
    lines = open(path, encoding='ascii').read().split('\n')
    ours = file_is_ours(lines)
    missing = []
    for i, line in enumerate(lines):
        if not DEF.match(line) or i + 1 >= len(lines):
            continue
        t = line.rstrip()
        nxt = lines[i + 1].strip()
        # A BODY CAN OPEN THREE WAYS and all three are definitions:
        #   ... )            ... ) {        ... ) { return x; }
        #   {                <body>
        # and the brace line may carry the body with it.
        oneline = '{' in t and t.endswith('}')
        opens = (nxt == '{' or nxt.startswith('{')) or t.endswith('{')
        if not opens and not oneline:
            continue
        above = '\n'.join(lines[max(0, i - WINDOW):i])
        blk = block_above(lines, i)
        m = TAG.search(blk) if blk else None
        if not m:
            if 'ORIGIN' not in above and not ours:
                missing.append((i + 1, line.strip()[:64], 'no ORIGIN'))
            continue
        rest = blk[m.start():]
        if 'NONE - ours' in rest:
            continue
        addrs = [int(a, 16) for a in ADDR.findall(rest)]
        if not addrs:
            missing.append((i + 1, line.strip()[:64], 'no address'))
        elif not any(TEXT_LO <= a <= TEXT_HI for a in addrs):
            missing.append((i + 1, line.strip()[:64], 'address not in .text'))
    return missing


def main():
    paths = sys.argv[1:] or sorted(glob.glob('src/*.c'))
    total = 0
    for p in paths:
        missing = audit(p)
        total += len(missing)
        if missing:
            print('%s: %d' % (p, len(missing)))
            for ln, name, why in missing:
                print('  %5d  %-50s %s' % (ln, name, why))
    print('\n%d defect(s) across %d file(s)' % (total, len(paths)))
    return 1 if total else 0


if __name__ == '__main__':
    sys.exit(main())
