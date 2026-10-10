#!/usr/bin/env python3
"""Find block comments sitting above the WRONG declaration.

A comment whose FIRST line names a function should be followed by a
declaration of that function.  Both halves are well-formed C, so
nothing else catches this; see docs/comment-defects.txt 2.1.

    python tools/orphan_check.py src/trk.h [...]      report
    python tools/orphan_check.py --all                every src/ file

CRLF FILES MUST BE NORMALISED ON READ.  Splitting on "
" alone
leaves a "" on every line, and the block scanner then takes the
bare "/*" opener as the title - "" is truthy, so the real title
never gets read and the whole block is skipped.  28 of src/'s 102
files are CRLF in the worktree and every one of them was invisible
to this check.

Match on the TITLE LINE ONLY.  A comment body legitimately names
other functions and matching those gives four false hits per real one.

Two false-positive classes are inherent and are reported, not
suppressed:  a title naming the ORIGINAL where the declaration
carries the port's name, and a macro or struct the title describes in
prose.  Forward declarations between the two are skipped.

IT ALSO REPORTS STACKED ORIGIN TAGS - adjacent comment blocks where
BOTH carry an ORIGIN, so one declaration has two tags and the upper
belongs to no function.  The check above cannot see this: its scan
for the next declaration steps over comment lines and finds the right
one.  Two in trk.c, whose own functions sat ninety lines below and
untagged.

REQUIRING BOTH IS WHAT MAKES IT USABLE.  Flagging any comment pair
under an ORIGIN reports 34 across the tree, nearly all of them the
ordinary shape - a tag block, then a short note on the first
statement.
"""
import glob
import re
import sys

# a title line that names a function: an identifier at the start,
# followed by "(" or " - " or " -- ".
TITLE = re.compile(r'^\s*\*?\s*([A-Za-z_][A-Za-z0-9_]*)\s*(\(|-\s|$)')
DECL = re.compile(r'^\s*(?:[A-Za-z_][A-Za-z0-9_]*[\s*]+)+'
                  r'([A-Za-z_][A-Za-z0-9_]*)\s*\(')
DEFINE = re.compile(r'^\s*#\s*define\s+([A-Za-z_][A-Za-z0-9_]*)')
SKIP = re.compile(r'^\s*(struct|union|enum|typedef)\b[^;{]*;\s*$')
# words that open a sentence rather than name a function
PROSE = {
    'the', 'this', 'a', 'an', 'it', 'and', 'but', 'so', 'not', 'one',
    'two', 'three', 'four', 'five', 'six', 'every', 'each', 'all',
    'what', 'when', 'where', 'why', 'how', 'if', 'for', 'in', 'on',
    'at', 'to', 'of', 'do', 'don', 'both', 'these', 'those', 'there',
    'here', 'that', 'its', 'our', 'ours', 'they', 'we', 'you', 'is',
    'are', 'was', 'were', 'no', 'yes', 'see', 'note', 'origin',
    'open', 'measured', 'verified', 'inferred', 'observed', 'guess',
}


def check(path):
    with open(path, encoding='ascii', newline='') as f:
        lines = f.read().replace('\r\n', '\n').split('\n')

    hits = []
    i = 0
    while i < len(lines):
        if not lines[i].lstrip().startswith('/*'):
            i += 1
            continue

        start = i
        # the title is the first line carrying words
        title = None
        j = i
        while j < len(lines):
            body = lines[j].lstrip()
            if body.startswith('/*'):
                body = body[2:]
            body = body.lstrip('* \t')
            if body and title is None:
                title = body
            if '*/' in lines[j]:
                break
            j += 1
        i = j + 1

        if title is None:
            continue
        m = TITLE.match(title)
        if not m:
            continue
        name = m.group(1)
        if name.lower() in PROSE or name.isupper():
            continue

        # The next declaration, skipping blanks, forward decls and a
        # RUN OF #defines - a function's own constants legitimately
        # sit between its comment and its declaration.  Fall back to
        # the first #define only if no function follows.
        k = i
        first = None
        hit = None
        while k < len(lines) and k < i + 40:
            t = lines[k].strip()
            if not t or t.startswith('/*') or t.startswith('*'):
                k += 1
                continue
            if t.startswith('//') or SKIP.match(lines[k]):
                k += 1
                continue
            d = DECL.match(lines[k])
            if d:
                hit = (d.group(1), k + 1)
                break
            d = DEFINE.match(lines[k])
            if d:
                if first is None:
                    first = (d.group(1), k + 1)
                k += 1
                continue
            break
        if hit is None:
            hit = first
        if hit is None:
            continue
        if hit[0] != name:
            hits.append((start + 1, name, hit[0], hit[1]))

    return hits


def stacked(path):
    """Adjacent comment blocks where BOTH carry an ORIGIN.

    Two tags for one declaration, so the upper belongs to no
    function.  Requiring both is what makes this precise: an ORIGIN
    block followed by a short note on the first statement is the
    ordinary shape and is not a defect.
    """
    with open(path, encoding='ascii', newline='') as f:
        lines = f.read().replace('\r\n', '\n').split('\n')

    out = []
    for i in range(len(lines) - 1):
        if not lines[i].strip().endswith('*/'):
            continue
        if not lines[i + 1].strip().startswith('/*'):
            continue
        k = i
        while k >= 0 and not lines[k].strip().startswith('/*'):
            k -= 1
        up = lines[max(k, 0):i + 1]
        # A BANNER IS OFTEN A RUN OF ONE-LINE COMMENTS, so its rule
        # is not inside either block. Check the whole contiguous
        # comment run or every banner reports as a stacked pair.
        lo, hi = max(k, 0), i
        while lo > 0 and lines[lo - 1].strip().startswith(('/*', '*')):
            lo -= 1
        while hi + 1 < len(lines):
            nxt = lines[hi + 1].strip()
            if not nxt.startswith(('/*', '*')):
                break
            hi += 1
        if any('====' in x or '----' in x for x in lines[lo:hi + 1]):
            continue
        j = i + 1
        while j < len(lines) and '*/' not in lines[j]:
            j += 1
        low = lines[i + 1:j + 1]
        upTag = any('ORIGIN' in x for x in up)
        loTag = any('ORIGIN' in x for x in low)
        # Two tags against one declaration, OR a bare one-line tag
        # leaning on a comment - a lone ORIGIN belongs on a
        # declaration and nowhere else.
        bare = ((len(up) == 1 and upTag) or (len(low) == 1 and loTag))
        if not (upTag and loTag) and not bare:
            continue
        src = up if upTag else low
        tag = ' '.join(x.strip().lstrip('/*').strip() for x in src)
        if upTag and loTag:
            why = 'two tags, upper belongs to no function'
        else:
            why = 'lone tag leaning on a block'
        out.append((k + 1, why, tag.split('ORIGIN', 1)[1].strip()[:44]))
    return out


def main(argv):
    paths = argv[1:]
    if not paths or paths[0] == '--all':
        paths = sorted(glob.glob('src/*.h') + glob.glob('src/*.c'))

    total = 0
    for p in paths:
        for line, title, got, dline in check(p):
            print('%s:%d  comment titled %-32s -> %s (line %d)'
                  % (p, line, title, got, dline))
            total += 1
    for p in paths:
        for line, why, tag in stacked(p):
            print('%s:%d  %s: %s' % (p, line, why, tag))
            total += 1
    print('\n%d suspect comment(s).' % total)
    return 1 if total else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
