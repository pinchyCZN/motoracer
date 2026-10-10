#!/usr/bin/env python3
"""Comment ratio for src/ files -- the rule 16 sweep's progress measure.

    python tools/cratio.py src/menu.c [...]

A line counts as comment if it is inside a /* */ block or starts with //;
a line carrying code AND a trailing comment counts as code.  Blank lines
are in the denominator.
"""
import sys


def ratio(text):
    total = com = 0
    block = False
    for line in text.replace('\r\n', '\n').split('\n'):
        total += 1
        t = line.strip()
        if block:
            com += 1
            if '*/' in t:
                block = False
        elif t.startswith('//'):
            com += 1
        elif t.startswith('/*'):
            com += 1
            if '*/' not in t[2:]:
                block = True
    return com, total


if __name__ == '__main__':
    for p in sys.argv[1:]:
        with open(p, encoding='ascii') as f:
            c, n = ratio(f.read())
        print('%-24s %6d lines %6d comment  %5.2f%%' % (p, n, c, 100.0 * c / n))
