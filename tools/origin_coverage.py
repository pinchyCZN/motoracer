#!/usr/bin/env python3
"""origin_coverage.py - measure and extend ORIGIN tag coverage in src/.

CLAUDE.md rule 13 requires every transcribed function to carry

    /* ORIGIN  Ghidra_Name @ 0xADDRESS */

naming the original it came from. The convention arrived after most of
src/ was written, so coverage is partial and this tool makes the number
auditable instead of a guess.

    python tools/origin_coverage.py            report coverage
    python tools/origin_coverage.py --suggest  show what --fix would add
    python tools/origin_coverage.py --fix      insert the safe tags

WHY IT ONLY TAGS A NARROW PATTERN

   A first attempt tagged any function with an address in a nearby
   comment. Half the results were wrong: vertexpool_alloc picked up
   Screen3D_ClipTriangleNear because that name appeared two lines
   above in an unrelated note, and font_classes picked up an address
   out of a table initialiser.

   So the accepted pattern is deliberately strict. In the comment
   block DIRECTLY above the function, on ONE line, there must be a
   Ghidra-looking symbol followed by an address:

       Screen3D_SetProjBaseX  0x0049a0e0
       Font_SetColors (0x00490b00)

   and exactly one such pair in the block. Anything else is left for
   a human, because a wrong ORIGIN tag is worse than no tag - it
   sends the next reader to the wrong function with full confidence.
"""

import io
import os
import re
import sys

NL = chr(10)
SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'src')

FNDEF = re.compile(r'^[A-Za-z_][A-Za-z0-9_ \*]*\b([a-z_][a-z0-9_]*)\s*\(')
# A Ghidra symbol is CamelCase or maybe_/FUN_ prefixed and has an _ in it.
PAIR = re.compile(r'\b((?:maybe_|FUN_)?[A-Za-z][A-Za-z0-9]*_[A-Za-z0-9_]*)'
                  r'\s*\(?\s*(0x00[0-9a-f]{6})\)?')


def is_def(lines, i):
    """A definition, not a prototype: an opening brace within 4 lines."""
    l = lines[i]
    if not FNDEF.match(l) or l.rstrip().endswith(';'):
        return False
    for x in lines[i:i + 4]:
        if x.strip() == '{' or x.rstrip().endswith('{'):
            return True
    return False


def comment_block_above(lines, i):
    """The contiguous comment lines directly above line i."""
    out = []
    j = i - 1
    while j >= 0:
        t = lines[j].strip()
        if t.endswith('*/') or t.startswith('*') or t.startswith('/*'):
            out.insert(0, lines[j])
            j -= 1
            continue
        break
    return out


def tokens(name):
    """Lowercase word tokens of a symbol, splitting _ and camelCase."""
    out = []
    for part in name.split('_'):
        if not part:
            continue
        cur = ''
        for ch in part:
            if ch.isupper() and cur:
                out.append(cur.lower())
                cur = ch
            else:
                cur += ch
        if cur:
            out.append(cur.lower())
    return [t for t in out if t not in ('maybe', 'fun')]


def agrees(cname, gname):
    """Does the C name plausibly correspond to the Ghidra name?

    The FIRST token of a Ghidra name is its module prefix - Mesh_,
    Script_, Font_ - and matching on that alone is what produced
    mesh_center_on_bounds -> Mesh_Scale. So the prefix is dropped and
    at least one of the REMAINING tokens must appear in the C name.
    Conservative by design: it rejects some correct pairs
    (blit_shaded -> FontShaded_DrawGlyph) rather than accept a wrong
    one.
    """
    g = tokens(gname)
    if len(g) < 2:
        return False
    c = set(tokens(cname))
    return any(t in c for t in g[1:])


def scan(path):
    lines = io.open(path, encoding='ascii', newline='').read().split(NL)
    total = tagged = safe = 0
    adds = []
    for i in range(len(lines)):
        if not is_def(lines, i):
            continue
        total += 1
        block = comment_block_above(lines, i)
        text = NL.join(block)
        if 'ORIGIN' in text:
            tagged += 1
            continue
        pairs = PAIR.findall(text)
        uniq = sorted(set(pairs))
        if len(uniq) == 1:
            m = FNDEF.match(lines[i])
            if m and agrees(m.group(1), uniq[0][0]):
                safe += 1
                adds.append((i, uniq[0][0], uniq[0][1], lines[i]))
    return lines, total, tagged, safe, adds


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else ''
    files = sorted(f for f in os.listdir(SRC) if f.endswith('.c'))
    gt = gg = gs = 0
    for f in files:
        p = os.path.join(SRC, f)
        lines, total, tagged, safe, adds = scan(p)
        gt += total
        gg += tagged
        gs += safe
        if total == 0:
            continue
        pct = (100 * tagged) // total
        flag = '' if not safe else ('   +%d safe' % safe)
        print('%-16s %3d defs  %3d tagged  %3d%%%s' % (f, total, tagged,
                                                       pct, flag))
        if mode == '--suggest':
            for _, sym, ad, decl in adds:
                print('      %s @ %s   <- %s' % (sym, ad, decl.strip()[:56]))
        if mode == '--fix' and adds:
            for i, sym, ad, _ in reversed(adds):
                lines.insert(i, '/* ORIGIN  %s @ %s */' % (sym, ad))
            io.open(p, 'w', encoding='ascii',
                    newline='').write(NL.join(lines))
    print()
    print('TOTAL %d defs, %d tagged (%d%%), %d safely taggable'
          % (gt, gg, (100 * gg) // gt if gt else 0, gs))
    if mode == '--fix':
        print('tags inserted - re-run without --fix to confirm')


if __name__ == '__main__':
    main()
