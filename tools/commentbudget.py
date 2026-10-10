#!/usr/bin/env python3
"""Comment budget for src/. See README-commentbudget.txt.

    python tools/commentbudget.py            the working tree diff
    python tools/commentbudget.py --reduce   a REDUCTION pass: comment
                                             must go DOWN
    python tools/commentbudget.py --blocks   every oversized block in src/

THE DEFAULT GATE PERMITS GROWTH AND THAT IS DELIBERATE: net comment may
not exceed net code, so a change that adds code may explain it. It is
the wrong gate for a pass whose whole purpose is to shrink the comment,
and five files grew while being swept under it - menu.c by 103 lines,
racerdraw.c by 71, physics.c by 70. --reduce requires the net to be
NEGATIVE and ignores the code delta entirely.
"""

import argparse
import os
import subprocess
import sys

MAX_BLOCK = 12          # lines in one /* */ or // run
HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(os.path.dirname(HERE), 'src')
CODE_EXT = ('.c', '.h', '.cpp')


def is_comment(line):
    t = line.strip()
    return t.startswith(('/*', '*', '//')) or t.endswith('*/')


def diff_lines():
    """{path: (added, removed)} for src/ in the working tree."""
    out = subprocess.run(['git', 'diff', '-U0', '--', 'src'],
                         capture_output=True, text=True).stdout
    path, d = None, {}
    for line in out.splitlines():
        if line.startswith('+++ b/'):
            path = line[6:]
            d.setdefault(path, ([], []))
        elif not path:
            continue
        elif line.startswith('+') and not line.startswith('+++'):
            d[path][0].append(line[1:])
        elif line.startswith('-') and not line.startswith('---'):
            d[path][1].append(line[1:])
    return d


def blocks(text):
    """Yield (start_line, length) for each run of comment lines."""
    run = 0
    start = 0
    for n, line in enumerate(text.splitlines(), 1):
        if is_comment(line):
            if not run:
                start = n
            run += 1
        else:
            if run:
                yield start, run
            run = 0
    if run:
        yield start, run


def counts(lines):
    c = sum(1 for l in lines if is_comment(l))
    k = sum(1 for l in lines if l.strip() and not is_comment(l))
    return c, k


def report_diff(reduce_mode=False):
    bad = 0
    tc = tk = 0
    touched = 0            # files whose comments the diff actually moves
    for path, (add, rem) in sorted(diff_lines().items()):
        if not path.endswith(CODE_EXT):
            continue
        ac, ak = counts(add)
        rc, rk = counts(rem)
        # NET, so replacing a long comment with a short one counts as
        # the reduction it is rather than the addition it contains.
        c, k = ac - rc, ak - rk
        tc += c
        tk += k
        if not (ac or ak or rc or rk):
            continue
        if ac or rc:
            touched += 1
        if reduce_mode:
            # A file whose comments were not touched at all is neutral -
            # a reduction pass is allowed to carry an unrelated code fix.
            over = (ac or rc) and c >= 0
            flag = '  GREW' if over else ''
        else:
            over = c > max(k, 0)
            flag = '  OVER' if over else ''
        print('%-18s comment %+4d  code %+4d%s' % (path, c, k, flag))
        if over:
            bad += 1
    if tc or tk:
        print('%-18s comment %+4d  code %+4d' % ('TOTAL', tc, tk))
    if reduce_mode:
        if touched and tc >= 0:
            print('\na reduction pass must REMOVE comment and this one is '
                  '%+d - shorten a block or move it to docs/' % tc)
            bad += 1
    elif tc > max(tk, 0):
        print('\nthis change adds more comment than code - move the '
              'explanation to docs/ and leave a one-line pointer')
        bad += 1
    return bad


def report_blocks():
    n = 0
    for root, _, files in os.walk(SRC):
        for f in sorted(files):
            if not f.endswith(CODE_EXT):
                continue
            p = os.path.join(root, f)
            with open(p, encoding='utf-8', errors='replace') as fh:
                text = fh.read()
            for start, length in blocks(text):
                if length > MAX_BLOCK:
                    print('%s:%d  %d-line block'
                          % (os.path.relpath(p), start, length))
                    n += 1
    if n:
        print('\n%d block(s) over %d lines' % (n, MAX_BLOCK))
    return 0            # advisory: the tree is full of them already


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--blocks', action='store_true')
    ap.add_argument('--reduce', action='store_true',
                    help='net comment must be NEGATIVE')
    a = ap.parse_args()
    if a.blocks:
        sys.exit(report_blocks())
    sys.exit(report_diff(a.reduce))


if __name__ == '__main__':
    main()
