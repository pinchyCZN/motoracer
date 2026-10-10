#!/usr/bin/env python3
"""
fnt.py - read MotoRacer .FNT bitmap font faces.

The oracle for the format, in the same role tools/bkf.py plays for the
archives: the reconstruction in src/font.c must agree with this.

    python tools/fnt.py --list   DATA/extract/FONTES/HIRES/CHRONO.FNT
    python tools/fnt.py --show 0 DATA/extract/FONTES/HIRES/CHRONO.FNT
    python tools/fnt.py --check  DATA/extract/FONTES/*/*.FNT

--check is the useful one: it re-derives the file length from the
header and fails if it does not land exactly on the real length. All 33
faces in FONTES.BKF pass, which is what makes the layout verified
rather than plausible.

THE FORMAT

    u32  firstChar          character code of glyph 0
    u32  charCount
    u32  glyphHeight        one height for every glyph in the face
    u32  paletteEntries     0 or 256
    u8   palette[entries][3]        RGB triples, ONLY when entries != 0
    u32  width[charCount]           per-glyph advance, in pixels
    u8   pixels[sum(width) * glyphHeight]

Glyphs are concatenated with no padding and no alignment. Each is
width*height bytes, ROW MAJOR - width across, then the next row.

A face is either direct or palettised, and the two differ only by that
palette block: paletteEntries is 0 for the fonts the game declares as
type Direct or AntiAlias8, and 256 for the ones it declares as Pal.
See docs/formats/fnt.txt.
"""

import argparse
import glob
import struct
import sys

HEADER = struct.Struct('<4I')


class FntError(Exception):
    pass


class Fnt(object):
    def __init__(self, data, name='<mem>'):
        self.name = name
        if len(data) < HEADER.size:
            raise FntError('%s: shorter than a header' % name)

        (self.first_char, self.char_count,
         self.glyph_height, self.palette_entries) = HEADER.unpack_from(data, 0)

        if self.char_count == 0 or self.char_count > 512:
            raise FntError('%s: implausible char count %d'
                           % (name, self.char_count))
        if self.glyph_height == 0 or self.glyph_height > 256:
            raise FntError('%s: implausible height %d'
                           % (name, self.glyph_height))
        if self.palette_entries not in (0, 256):
            raise FntError('%s: unexpected palette entry count %d'
                           % (name, self.palette_entries))

        off = HEADER.size

        self.palette = None
        if self.palette_entries:
            need = self.palette_entries * 3
            if off + need > len(data):
                raise FntError('%s: truncated in the palette' % name)
            raw = data[off:off + need]
            self.palette = [tuple(raw[i * 3:i * 3 + 3])
                            for i in range(self.palette_entries)]
            off += need

        need = self.char_count * 4
        if off + need > len(data):
            raise FntError('%s: truncated in the width table' % name)
        self.widths = list(struct.unpack_from('<%dI' % self.char_count,
                                              data, off))
        off += need
        if any(w == 0 or w > 500 for w in self.widths):
            raise FntError('%s: implausible glyph width' % name)

        self.pixels_at = off
        self.pixels_len = sum(self.widths) * self.glyph_height
        self.data = data
        self.declared_len = off + self.pixels_len

    def glyph_offset(self, index):
        """Byte offset of glyph `index` within the file."""
        if index < 0 or index >= self.char_count:
            raise FntError('%s: glyph %d out of range' % (self.name, index))
        skip = sum(self.widths[:index]) * self.glyph_height
        return self.pixels_at + skip

    def glyph_rows(self, index):
        """The glyph as a list of `glyph_height` bytes-rows."""
        off = self.glyph_offset(index)
        w = self.widths[index]
        return [self.data[off + y * w: off + y * w + w]
                for y in range(self.glyph_height)]

    def last_char(self):
        return self.first_char + self.char_count - 1


def load(path):
    with open(path, 'rb') as f:
        return Fnt(f.read(), path)


def cmd_list(f):
    print('%s' % f.name)
    print('   chars     0x%02x..0x%02x  (%d)'
          % (f.first_char, f.last_char(), f.char_count))
    print('   height    %d' % f.glyph_height)
    print('   palette   %s'
          % ('%d RGB triples' % f.palette_entries if f.palette_entries
             else 'none - direct'))
    print('   widths    min %d  max %d  total %d'
          % (min(f.widths), max(f.widths), sum(f.widths)))
    print('   pixels    %d bytes at 0x%x' % (f.pixels_len, f.pixels_at))


def cmd_show(f, index):
    if index >= f.char_count:
        raise FntError('%s: only %d glyphs' % (f.name, f.char_count))
    code = f.first_char + index
    ch = chr(code) if 32 <= code < 127 else '?'
    print("%s  glyph %d = 0x%02x '%s'  %dx%d"
          % (f.name, index, code, ch, f.widths[index], f.glyph_height))
    ramp = ' .:-=+*#%@'

    # A DIRECT face stores intensity; a PALETTISED one stores an index,
    # so brightness has to come from the palette or every glyph renders
    # blank. Getting this wrong is not a display bug - it is the same
    # mistake a loader would make.
    def level(b):
        if f.palette is None:
            return b
        r, g, bl = f.palette[b]
        return (r * 30 + g * 59 + bl * 11) // 100

    for row in f.glyph_rows(index):
        out = []
        for b in row:
            out.append(ramp[min(len(ramp) - 1, level(b) * len(ramp) // 256)])
        print('   ' + ''.join(out))


def cmd_check(f, actual):
    ok = (f.declared_len == actual)
    print('%-46s %s  header says %d, file is %d'
          % (f.name, 'OK  ' if ok else 'FAIL', f.declared_len, actual))
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[1])
    ap.add_argument('--list', action='store_true',
                    help='print the header of each file')
    ap.add_argument('--show', type=int, metavar='N',
                    help='render glyph N as ASCII art')
    ap.add_argument('--check', action='store_true',
                    help='verify the derived length against the real one')
    ap.add_argument('paths', nargs='+')
    args = ap.parse_args()

    paths = []
    for p in args.paths:
        hits = glob.glob(p)
        paths.extend(hits if hits else [p])

    bad = 0
    for p in sorted(paths):
        try:
            with open(p, 'rb') as fh:
                raw = fh.read()
            f = Fnt(raw, p)
        except (FntError, IOError) as e:
            print('%-46s FAIL  %s' % (p, e))
            bad += 1
            continue

        if args.check:
            if not cmd_check(f, len(raw)):
                bad += 1
        elif args.show is not None:
            cmd_show(f, args.show)
        else:
            cmd_list(f)

    if args.check:
        print('\n%d file(s), %d failure(s).' % (len(paths), bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
