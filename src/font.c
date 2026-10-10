/*
 * font.c - see font.h.
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "font.h"
#include "assets.h"
#include "framebuffer.h"
#include "log.h"
#include "options.h"

/* ---------------------------------------------------------------- */
/* .FNT faces                                                       */

static unsigned int rd32(const unsigned char *p)
{
	return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
	       ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

/*
 * The layout, from docs/formats/fnt.txt. Verified byte-exact against
 * all 33 faces in FONTES.BKF by tools/fnt.py --check: the length
 * derived from the header lands on the real file length every time.
 *
 *     u32 firstChar
 *     u32 charCount
 *     u32 glyphHeight
 *     u32 paletteEntries          0 or 256
 *     u8  palette[entries][3]     RGB, only when entries != 0
 *     u32 width[charCount]
 *     u8  pixels[sum(width) * glyphHeight]
 *
 * THE PALETTE COMES BEFORE THE WIDTH TABLE, which is the one thing
 * about the order that is not guessable - the obvious reading puts the
 * widths straight after the fixed header, and that is wrong for the 20
 * palettised faces.
 */
int font_face_load(MR_FontFace *face, const char *logical)
{
	unsigned int         size = 0;
	unsigned char       *blob;
	unsigned int         off, need, total;
	int                  i;

	if (!face || !logical)
		return 0;
	memset(face, 0, sizeof(*face));

	blob = (unsigned char *)asset_load(logical, &size);
	if (!blob)
		return 0;
	if (size < 16) {
		mrlog("font: %s is shorter than a header\n", logical);
		free(blob);
		return 0;
	}

	face->blob           = blob;
	face->firstChar      = (int)rd32(blob + 0);
	face->charCount      = (int)rd32(blob + 4);
	face->glyphHeight    = (int)rd32(blob + 8);
	face->paletteEntries = (int)rd32(blob + 12);

	if (face->charCount <= 0 || face->charCount > 512 ||
	    face->glyphHeight <= 0 || face->glyphHeight > 256 ||
	    (face->paletteEntries != 0 && face->paletteEntries != 256)) {
		mrlog_err("font: %s has an implausible header "
		       "(count %d, height %d, palette %d)\n",
		       logical, face->charCount, face->glyphHeight,
		       face->paletteEntries);
		font_face_free(face);
		return 0;
	}

	off = 16;
	if (face->paletteEntries) {
		need = (unsigned int)face->paletteEntries * 3;
		if (off + need > size) {
			mrlog_err("font: %s truncated in the palette\n", logical);
			font_face_free(face);
			return 0;
		}
		face->palette = blob + off;
		off += need;
	}

	need = (unsigned int)face->charCount * 4;
	if (off + need > size) {
		mrlog_err("font: %s truncated in the width table\n", logical);
		font_face_free(face);
		return 0;
	}
	face->widths = (const unsigned int *)(blob + off);
	off += need;

	/*
	 * Glyphs are concatenated with no padding, so the offset of each
	 * is the running sum of the widths before it times the height.
	 * Precomputed once rather than walked per draw - the original does
	 * the same, keeping a pointer per character in its own table.
	 */
	face->glyphOffset = (unsigned int *)
	    malloc(sizeof(unsigned int) * (size_t)face->charCount);
	if (!face->glyphOffset) {
		font_face_free(face);
		return 0;
	}

	total = 0;
	for (i = 0; i < face->charCount; i++) {
		unsigned int w = face->widths[i];
		if (w == 0 || w > 500) {
			mrlog("font: %s glyph %d has width %u\n", logical, i, w);
			font_face_free(face);
			return 0;
		}
		face->glyphOffset[i] = total * (unsigned int)face->glyphHeight;
		total += w;
	}

	need = total * (unsigned int)face->glyphHeight;
	if (off + need > size) {
		mrlog_err("font: %s truncated in the pixels "
		       "(want %u, have %u)\n", logical, need, size - off);
		font_face_free(face);
		return 0;
	}
	face->pixels = blob + off;
	return 1;
}

void font_face_free(MR_FontFace *face)
{
	if (!face)
		return;
	free(face->glyphOffset);
	free(face->blob);
	memset(face, 0, sizeof(*face));
}

const unsigned char *font_face_glyph(const MR_FontFace *face, int ch,
                                     int *widthOut)
{
	int idx;

	if (widthOut)
		*widthOut = 0;
	if (!face || !face->pixels)
		return NULL;
	idx = ch - face->firstChar;
	if (idx < 0 || idx >= face->charCount)
		return NULL;
	if (widthOut)
		*widthOut = (int)face->widths[idx];
	return face->pixels + face->glyphOffset[idx];
}


/* ---------------------------------------------------------------- */
/* The colour LUT                                                   */

/*
 * MR_Font+0x418, and it is the whole design. See font.h.
 *
 * Font_BuildCharRange (0x00490b90, slot 1) does exactly this
 * [verified]: it walks a 32-entry gradient from backColor to
 * foreColor in 16.16 fixed point, packing each step through
 * Color_Pack888ToSurface into a scratch array, and then fills
 * this+0x418 + i*2 with ramp32[i & 0x1f] for i over the requested
 * range.
 *
 *     step = ((fore - back) << 16) >> 5      per channel
 *
 * o SO THE RANGE ARGUMENT IS A LUT INDEX RANGE, NOT A CHARACTER
 *   RANGE, even though slot 0 passes (1, 0xff) and the obvious
 *   reading is "every character". Nothing about it is per character;
 *   the same 32 colours repeat eight times across the 256 entries.
 */
static unsigned short lerp565(const unsigned char back[3],
                              const unsigned char fore[3], int step)
{
	int r = back[0] + (fore[0] - back[0]) * step / FONT_RAMP_STEPS;
	int g = back[1] + (fore[1] - back[1]) * step / FONT_RAMP_STEPS;
	int b = back[2] + (fore[2] - back[2]) * step / FONT_RAMP_STEPS;
	return FB_RGB565(r, g, b);
}

static void slot1_build_lut(MR_Font *f, int firstIndex, int lastIndex)
{
	unsigned short ramp[FONT_RAMP_STEPS];
	int i;

	for (i = 0; i < FONT_RAMP_STEPS; i++)
		ramp[i] = lerp565(f->backColor, f->foreColor, i);

	if (firstIndex < 0)        firstIndex = 0;
	if (lastIndex > 0xff)      lastIndex  = 0xff;
	for (i = firstIndex; i <= lastIndex; i++)
		f->colorLut[i] = ramp[i & FONT_RAMP_MASK];
}

/*
 * FontPal_BuildColorLut (0x00495bd0), transcribed. 89 bytes.
 *
 *     if (this->palettePixels) {
 *         for (i = first; i <= last; i++)
 *             lut[i] = pack(palettePixels[(i % paletteCount) * 3]);
 *     }
 *
 * A MODULO, NOT A MASK - the base uses i & 0x1f for its 32-step ramp,
 * and this uses i % paletteCount, so a palette of any size tiles
 * across the 256 entries. Same idea, and the only reason the base can
 * use a mask is that 32 is a power of two.
 *
 * o IT ALSO RESOLVED TWO FIELDS. MR_Font+0x549a is a pointer to the
 *   face's RGB triples and +0x549e is the entry count - the whole of
 *   the eight-byte packed region MR_Font.txt had as unknown. The
 *   guard on +0x549a being non-null is what makes it a pointer rather
 *   than a count.
 */
/* ORIGIN  FontPal_BuildColorLut @ 0x00495bd0 */
static void slot1_build_lut_pal(MR_Font *f, int firstIndex, int lastIndex)
{
	const MR_FontFace *face = NULL;
	int i, n;

	for (i = 0; i < f->faceCount; i++) {
		if (f->faces[i].palette) {
			face = &f->faces[i];
			break;
		}
	}
	if (!face || face->paletteEntries <= 0)
		return;                         /* the null-pointer guard */

	n = face->paletteEntries;
	if (firstIndex < 0)   firstIndex = 0;
	if (lastIndex > 0xff) lastIndex  = 0xff;
	for (i = firstIndex; i <= lastIndex; i++) {
		const unsigned char *e = face->palette + (i % n) * 3;
		f->colorLut[i] = FB_RGB565(e[0], e[1], e[2]);
	}
}

/*
 * FontMoto_BuildColorLut (0x004960e0), transcribed. A HYBRID, and the
 * only LUT builder that fills two disjoint zones:
 *
 *     lut[1 .. 14]    a 14-step gradient, (fore*k + back*(14-k)) / 14
 *     lut[32 .. 62]   the face palette, palettePixels[k % count]
 *
 * Everything else is left as it was.
 *
 * o IT IGNORES ITS RANGE ARGUMENTS ENTIRELY - the decompiler renders
 *   it as __fastcall with one parameter. The zones are hardcoded. So
 *   a Moto font cannot have part of its LUT rebuilt; slot 0 calling
 *   it with (1, 0xff) rebuilds exactly these two zones either way.
 *
 * FOURTEEN STEPS, NOT THIRTY-TWO, and that matches the data: the MOTO
 * faces hold pixel values 0..15, which land in the gradient zone. The
 * palette zone at 32..62 is for something else in the same face.
 */
/* ORIGIN  FontMoto_BuildColorLut @ 0x004960e0 */
static void slot1_build_lut_moto(MR_Font *f, int firstIndex, int lastIndex)
{
	const MR_FontFace *face = NULL;
	int i, k;

	(void)firstIndex; (void)lastIndex;   /* the original ignores them */

	for (i = 0; i < f->faceCount; i++) {
		if (f->faces[i].palette) {
			face = &f->faces[i];
			break;
		}
	}

	if (face && face->paletteEntries > 0) {
		int n = face->paletteEntries;
		for (k = 0x20; k <= 0x3e; k++) {
			const unsigned char *e = face->palette + (k % n) * 3;
			f->colorLut[k] = FB_RGB565(e[0], e[1], e[2]);
		}
	}

	for (k = 1; k <= 14; k++) {
		int w = 14 - k;
		int r = (f->foreColor[0] * k + f->backColor[0] * w) / 14;
		int g = (f->foreColor[1] * k + f->backColor[1] * w) / 14;
		int b = (f->foreColor[2] * k + f->backColor[2] * w) / 14;
		f->colorLut[k] = FB_RGB565(r, g, b);
	}
}

/*
 * CustomColor's slot 1 is a bare RET 0x8 [verified]. Its LUT is never
 * rebuilt, which is consistent with its slot 0 ignoring colour
 * changes: nothing it depends on can change.
 */
static void slot1_ignore(MR_Font *f, int firstIndex, int lastIndex)
{
	(void)f; (void)firstIndex; (void)lastIndex;
}


/* ---------------------------------------------------------------- */
/* Slot 0                                                           */

/*
 * Font_SetColors (0x00490b00), transcribed in behaviour.
 *
 * It compares three bytes at this+0x5494 against the incoming fore
 * colour and three at this+0x5497 against the back colour, copies each
 * in only if it differs, and calls slot 1 over (1, 0xff) if EITHER
 * changed - once, not twice.
 *
 * The early-out matters: rebuilding is a 256-entry fill and menu code
 * sets the same colour every frame.
 */
/* ORIGIN  Font_SetColors @ 0x00490b00 */
static void slot0_set_colors(MR_Font *f, const unsigned char fore[3],
                             const unsigned char back[3])
{
	int changed = 0;

	if (fore && memcmp(f->foreColor, fore, 3) != 0) {
		memcpy(f->foreColor, fore, 3);
		changed = 1;
	}
	if (back && memcmp(f->backColor, back, 3) != 0) {
		memcpy(f->backColor, back, 3);
		changed = 1;
	}
	if (changed)
		f->cls->build_lut(f, 1, 0xff);
}

/* CustomColor and CustomColorTrame: a three-byte RET 0x8 [verified]. */
static void slot0_ignore(MR_Font *f, const unsigned char fore[3],
                         const unsigned char back[3])
{
	(void)f; (void)fore; (void)back;
}


/* ---------------------------------------------------------------- */
/* The glyph blitters - slot 6                                      */

/*
 * Three of the eight are reconstructed here, from the two that were
 * read plus the base they share:
 *
 *   blit_plain   the opaque LUT store          Font_DrawGlyph
 *   blit_trame   the same, through a halftone  FontDirectTrame_DrawGlyph
 *   blit_shaded  the 8-level alpha blend       FontShaded_DrawGlyph
 *
 * WHAT IS NOT RECONSTRUCTED: FontCustomColor_DrawGlyph (0x00494170),
 * FontCustomColorTrame_DrawGlyph (0x00494590), FontPal_DrawGlyph
 * (0x00495840) and FontMoto_DrawGlyph (0x00495d50) have not been read.
 * They are mapped onto the nearest of the three above and the class
 * table says which, so nothing here pretends to be what it is not.
 *
 * ALL OF THEM SHARE A STRUCTURE the two read copies make plain:
 *
 *   - skip pixel value 0 entirely - it is the only transparency the
 *     opaque paths have;
 *   - clip the glyph rectangle against the target before looping,
 *     adjusting both pointers, rather than testing per pixel;
 *   - advance the pen by charSpacing + width afterwards;
 *   - and take a HARDWARE PATH INSTEAD when the font owns a surface
 *     and the menu is not active, queueing a screen quad built from
 *     the prebuilt MR_Primitive at this+0x1248 + ch*0x40.
 *
 * o THAT LAST POINT SETTLES WHAT MR_FontGlyph IS. MR_Font.txt had it
 *   as 0x40 bytes of "ten normalised coordinates and seven small
 *   constants... likely a Direct3D execute-buffer fragment [guess]".
 *   Both blitters pass it to Screen3D_QueueScreenQuad as an
 *   MR_Primitive*, and MR_Primitive is 0x40 bytes. It is a primitive.
 *
 * The hardware path is not reproduced: our renderer takes a
 * framebuffer, and queueing a textured quad per glyph needs the atlas
 * that goes with it.
 */

static void put565(int x, int y, unsigned short c)
{
	if (x < 0 || y < 0 || x >= g_frameWidth || y >= g_frameHeight)
		return;
	g_frameBuffer[y * g_frameStridePx + x] = c;
}

/* Resolve a character to its face and glyph, or 0. */
static int glyph_of(MR_Font *f, int ch, const MR_FontFace **faceOut,
                    const unsigned char **pixOut, int *wOut)
{
	int fi = f->charFace[ch & 0xff];
	const unsigned char *g;
	int w;

	if (fi < 0)
		return 0;
	g = font_face_glyph(&f->faces[fi], ch, &w);
	if (!g || w <= 0)
		return 0;
	*faceOut = &f->faces[fi];
	*pixOut  = g;
	*wOut    = w;
	return 1;
}

/*
 * Font_DrawGlyph. The opaque store: every non-zero pixel becomes its
 * LUT entry, and zero is left alone.
 */
static void blit_plain(MR_Font *f, int x, int y, int ch, int level)
{
	const MR_FontFace   *face;
	const unsigned char *g;
	int w, gx, gy;

	(void)level;
	if (!glyph_of(f, ch, &face, &g, &w))
		return;
	for (gy = 0; gy < face->glyphHeight; gy++)
		for (gx = 0; gx < w; gx++) {
		    unsigned int b = g[gy * w + gx];
		    if (b)
			    put565(x + gx, y + gy, f->colorLut[b]);
	    }
}

/*
 * FontDirectTrame_DrawGlyph (0x00492d20), transcribed.
 *
 * "Trame" is French for a halftone screen, which MR_Font.txt guessed
 * from the name. It is now confirmed: the ONLY difference from the
 * plain blitter is a dissolve test around the same store [verified].
 *
 *     if (level == 0 ||
 *         level * 2 < (((row & 15) * (row & 15) +
 *                       (col & 15) * (col & 15)) & 0x7f))
 *         *dst = lut[b];
 *
 * A 16x16 cell of squared distances masked to 7 bits, compared against
 * twice the level. So level 0 draws everything and higher levels erode
 * the glyph through a fixed pattern - a dissolve, not a fade, which is
 * exactly what a halftone screen does on a display that cannot blend.
 *
 * o THE COUNTERS RUN DOWNWARD IN THE ORIGINAL. row and col are the
 *   REMAINING row and column counts, not positions from the top left,
 *   so the pattern is anchored to the bottom right of the glyph. That
 *   is reproduced rather than tidied: anchoring it to the top left
 *   would shift the pattern by whatever the glyph size is and make a
 *   dissolving line of text ripple differently.
 */
static void blit_trame(MR_Font *f, int x, int y, int ch, int level)
{
	const MR_FontFace   *face;
	const unsigned char *g;
	int w, gx, gy, h;

	if (!glyph_of(f, ch, &face, &g, &w))
		return;
	h = face->glyphHeight;

	for (gy = 0; gy < h; gy++) {
		int rowDown = h - gy;               /* the original's counter */
		for (gx = 0; gx < w; gx++) {
			int colDown = w - gx;
			unsigned int b = g[gy * w + gx];
			int rr, cc, thresh;
			if (!b)
				continue;
			if (level != 0) {
				rr = rowDown & 0xf;
				cc = colDown & 0xf;
				thresh = (rr * rr + cc * cc) & 0x7f;
				if (!(level * 2 < thresh))
					continue;
			}
			put565(x + gx, y + gy, f->colorLut[b]);
		}
	}
}

/*
 * FontShaded_DrawGlyph (0x00494bb0), transcribed.
 *
 * THE PIXEL BYTE CARRIES BOTH COLOUR AND ALPHA:
 *
 *     colour = colorLut[b]        all eight bits index the LUT
 *     level  = (b >> 5) + bias    the top three bits, plus the caller's
 *
 * level 0 stores the colour outright; 1..7 blend it with what is
 * already there; 8 and above draw nothing, which is what the
 * original's `level < 8` guard means.
 *
 * The original blends through the seven precomputed tables at
 * this+0x54a6 - FontShaded_Ctor fills them as (i * n) / 8 for n from 7
 * down to 1, which is the 0x1c00 that makes an AntiAlias8 object
 * 0x70a6 instead of 0x54a6 [verified]. It indexes one table by the
 * source channel and its complement by the destination channel and
 * adds them, per channel, using the surface's own mask and shift
 * globals.
 *
 * o COMPUTED HERE RATHER THAN TABULATED. The tables exist to avoid a
 *   multiply per channel per pixel on a 1997 CPU; the arithmetic they
 *   encode is a plain alpha blend and that is what is written below.
 *   This is a deliberate divergence in MEANS, not in result.
 *
 * o ONE THING IS NOT FULLY PINNED DOWN, and it is flagged rather than
 *   smoothed over. The decompiled table addressing - base + level*0x400
 *   for the source and 0x74a6 - level*0x400 for the destination -
 *   does not line up with the constructor's fill order without an
 *   off-by-one somewhere, and the two readings differ in whether the
 *   weights sum to 8/8 or 7/8. Weights that do not sum to one would
 *   darken every blended pixel, so the sensible reading is taken:
 *   src*(8-level) + dst*level, all over 8. The DIRECTION is certain -
 *   level 0 is the opaque special case, so rising level must mean
 *   rising transparency - and only the exact table indexing is
 *   unresolved. Reading the blitter's disassembly would settle it.
 */
/* ORIGIN  FontShaded_DrawGlyph @ 0x00494bb0 */
static void blit_shaded(MR_Font *f, int x, int y, int ch, int bias)
{
	const MR_FontFace   *face;
	const unsigned char *g;
	int w, gx, gy;

	if (!glyph_of(f, ch, &face, &g, &w))
		return;

	for (gy = 0; gy < face->glyphHeight; gy++) {
		for (gx = 0; gx < w; gx++) {
			unsigned int b = g[gy * w + gx];
			int level, px, py;
			unsigned short sc, dc;
			int sr, sg, sb, dr, dg, db;

			if (!b)
				continue;
			level = (int)(b >> 5) + bias;
			if (level < 0 || level >= FONT_LEVELS)
				continue;                   /* the `< 8` guard */

			px = x + gx;
			py = y + gy;
			if (px < 0 || py < 0 || px >= g_frameWidth || py >= g_frameHeight)
				continue;

			sc = f->colorLut[b];
			if (level == 0) {
				g_frameBuffer[py * g_frameStridePx + px] = sc;
				continue;
			}

			dc = g_frameBuffer[py * g_frameStridePx + px];
			sr = (sc >> 11) & 0x1f;  sg = (sc >> 5) & 0x3f;  sb = sc & 0x1f;
			dr = (dc >> 11) & 0x1f;  dg = (dc >> 5) & 0x3f;  db = dc & 0x1f;

			sr = (sr * (FONT_LEVELS - level) + dr * level) / FONT_LEVELS;
			sg = (sg * (FONT_LEVELS - level) + dg * level) / FONT_LEVELS;
			sb = (sb * (FONT_LEVELS - level) + db * level) / FONT_LEVELS;

			g_frameBuffer[py * g_frameStridePx + px] =
			    (unsigned short)((sr << 11) | (sg << 5) | sb);
		}
	}
}


/*
 * FontCustomColor_DrawGlyph (0x00494170), transcribed.
 *
 * IT DOES NOT USE THE LUT AT ALL. It computes the gradient per pixel,
 * inline:
 *
 *     step = b & 0x1f;
 *     out  = (fore * step + back * (31 - step)) / 31;
 *
 * o WHICH IS WHY ITS SLOT 0 AND SLOT 1 ARE BARE RETURNS. There is no
 *   table to maintain, so setting a colour needs no rebuild and takes
 *   effect on the very next glyph. That is what "CustomColor" means,
 *   and the three-byte RET 0x8 overrides stop looking like stubs and
 *   start looking like the point.
 *
 * The divisor is 31 here and 32 in the base's LUT ramp, so the two
 * gradients are not bit-identical. Reproduced as written.
 *
 * It costs a divide per channel per pixel, which is why the other
 * classes tabulate instead.
 */
static void blit_customcolor(MR_Font *f, int x, int y, int ch, int level)
{
	const MR_FontFace   *face;
	const unsigned char *g;
	int w, gx, gy;

	(void)level;
	if (!glyph_of(f, ch, &face, &g, &w))
		return;
	for (gy = 0; gy < face->glyphHeight; gy++)
		for (gx = 0; gx < w; gx++) {
		    unsigned int b = g[gy * w + gx];
		    int step, iw, r, gg, bb;
		    if (!b)
			    continue;
		    step = (int)(b & 0x1f);
		    iw   = 0x1f - step;
		    r  = (f->foreColor[0] * step + f->backColor[0] * iw) / 31;
		    gg = (f->foreColor[1] * step + f->backColor[1] * iw) / 31;
		    bb = (f->foreColor[2] * step + f->backColor[2] * iw) / 31;
		    put565(x + gx, y + gy, FB_RGB565(r, gg, bb));
	    }
}

/*
 * FontCustomColorTrame_DrawGlyph (0x00494590), transcribed. Exactly
 * blit_customcolor with the halftone test from blit_trame around the
 * store - the same 16x16 cell, the same downward-counting row and
 * column, the same level*2 comparison [verified].
 *
 * The pairing is perfectly regular: Direct/DirectTrame differ by that
 * test and so do CustomColor/CustomColorTrame. Four classes, two
 * behaviours, one screen.
 */
/* ORIGIN  FontCustomColorTrame_DrawGlyph @ 0x00494590 */
static void blit_customcolor_trame(MR_Font *f, int x, int y, int ch,
                                   int level)
{
	const MR_FontFace   *face;
	const unsigned char *g;
	int w, gx, gy, h;

	if (!glyph_of(f, ch, &face, &g, &w))
		return;
	h = face->glyphHeight;

	for (gy = 0; gy < h; gy++) {
		int rowDown = h - gy;
		for (gx = 0; gx < w; gx++) {
			int colDown = w - gx;
			unsigned int b = g[gy * w + gx];
			int step, iw, r, gg, bb, rr, cc, thresh;
			if (!b)
				continue;
			if (level != 0) {
				rr = rowDown & 0xf;
				cc = colDown & 0xf;
				thresh = (rr * rr + cc * cc) & 0x7f;
				if (!(level * 2 < thresh))
					continue;
			}
			step = (int)(b & 0x1f);
			iw   = 0x1f - step;
			r  = (f->foreColor[0] * step + f->backColor[0] * iw) / 31;
			gg = (f->foreColor[1] * step + f->backColor[1] * iw) / 31;
			bb = (f->foreColor[2] * step + f->backColor[2] * iw) / 31;
			put565(x + gx, y + gy, FB_RGB565(r, gg, bb));
		}
	}
}

/*
 * FontShadedTransp_DrawGlyph (0x00495280), transcribed - and reading
 * it settled the question blit_shaded had to leave open.
 *
 * This one blends with EXPLICIT ARITHMETIC rather than through the
 * seven ramp tables, so there is no indexing to misread [verified]:
 *
 *     alpha = bias + (b > 0x1f ? (b & 0xe0) : 0);   // 0,32,...,224
 *     if (alpha < 0xe1) {
 *         alpha = (alpha << 8) / 0xe0;              // 0..224 -> 0..256
 *         out   = src - ((src - dst) * alpha >> 8); // per channel
 *     }
 *
 * o SO ALPHA IS level/7, NOT level/8. Level 0 is fully opaque and
 *   level 7 is FULLY TRANSPARENT - the destination survives
 *   untouched. That is the difference between the two shaded classes
 *   and it is what "Transp" means: AntiAlias8's /8 tables can never
 *   reach zero ink, and this can.
 *
 * o AND IT CONFIRMS THE DIRECTION blit_shaded assumed. Rising level
 *   means rising transparency. That was the half of the AntiAlias8
 *   reading that was certain; this is independent evidence for it.
 *
 * The channel work is done through the surface's own masks in the
 * original, which is how one expression covers 555 and 565. Ours is
 * 565 only, so the channels are unpacked instead.
 */
/* ORIGIN  FontShadedTransp_DrawGlyph @ 0x00495280 */
static void blit_shaded_transp(MR_Font *f, int x, int y, int ch, int bias)
{
	const MR_FontFace   *face;
	const unsigned char *g;
	int w, gx, gy;

	if (!glyph_of(f, ch, &face, &g, &w))
		return;

	for (gy = 0; gy < face->glyphHeight; gy++) {
		for (gx = 0; gx < w; gx++) {
			unsigned int b = g[gy * w + gx];
			int alpha, px, py;
			unsigned short sc, dc;
			int sr, sg, sb, dr, dg, db;

			if (!b)
				continue;
			alpha = bias + ((b > 0x1f) ? (int)(b & 0xe0) : 0);
			if (alpha < 0 || alpha >= 0xe1)
				continue;
			alpha = (alpha << 8) / 0xe0;

			px = x + gx;
			py = y + gy;
			if (px < 0 || py < 0 || px >= g_frameWidth || py >= g_frameHeight)
				continue;

			sc = f->colorLut[b];
			dc = g_frameBuffer[py * g_frameStridePx + px];
			sr = (sc >> 11) & 0x1f;  sg = (sc >> 5) & 0x3f;  sb = sc & 0x1f;
			dr = (dc >> 11) & 0x1f;  dg = (dc >> 5) & 0x3f;  db = dc & 0x1f;

			sr = sr - (((sr - dr) * alpha) >> 8);
			sg = sg - (((sg - dg) * alpha) >> 8);
			sb = sb - (((sb - db) * alpha) >> 8);

			g_frameBuffer[py * g_frameStridePx + px] =
			    (unsigned short)((sr << 11) | (sg << 5) | sb);
		}
	}
}


/* ---------------------------------------------------------------- */
/* The class table                                                  */

/*
 * The eight configured types, with the vtable each installs and the
 * kind its constructor stores [verified, MR_Font.txt section 4].
 *
 * The two Trame entries are NOT separate classes in the original -
 * Font_LoadAll builds a plain object and assigns a different vtable
 * over the top, in the caller. Modelling them as classes costs nothing
 * and is indistinguishable from outside.
 *
 * The trailing address on each row is that class's REAL slot-6
 * function. Where the blitter column does not correspond to a function
 * that was read, the row says so.
 */
#define FONT_BASE_SLOTS  slot0_set_colors, slot1_build_lut
#define FONT_CC_SLOTS    slot0_ignore,     slot1_ignore
#define FONT_PAL_SLOTS   slot0_set_colors, slot1_build_lut_pal
#define FONT_MOTO_SLOTS  slot0_set_colors, slot1_build_lut_moto

/*
 * ALL EIGHT ARE NOW READ. Every row below is a transcription, not a
 * stand-in, and the trailing address is the function it came from.
 *
 * THREE OF THE EIGHT BLITTERS ARE THE SAME ROUTINE COMPILED THREE
 * TIMES. Font_DrawGlyph, FontPal_DrawGlyph and FontMoto_DrawGlyph are
 * all 898 bytes and functionally identical - 113 and 251 bytes differ
 * respectively, and every difference is register allocation. Pal and
 * Moto specialise entirely in SLOT 1, by filling the LUT differently;
 * their blitters had nothing to change.
 *
 * That is worth knowing before reading any of the others: a byte-level
 * difference between two of these functions does not imply a
 * behavioural one.
 */
static const MR_FontClass s_classes[] = {
	{ "Direct",           1, 0x004d1e68,
	  FONT_BASE_SLOTS, blit_plain },             /* 0x00493100 */
	{ "DirectTrame",      1, 0x004d0710,
	  FONT_BASE_SLOTS, blit_trame },             /* 0x00492d20 */
	{ "CustomColor",      3, 0x004d1e88,
	  FONT_CC_SLOTS,   blit_customcolor },       /* 0x00494170 */
	{ "CustomColorTrame", 3, 0x004d06f0,
	  FONT_CC_SLOTS,   blit_customcolor_trame }, /* 0x00494590 */
	{ "AntiAlias8",       5, 0x004d1ea8,
	  FONT_BASE_SLOTS, blit_shaded },            /* 0x00494bb0 */
	{ "AntiAlias8Transp", 6, 0x004d1ec8,
	  FONT_BASE_SLOTS, blit_shaded_transp },     /* 0x00495280 */
	{ "Pal",              7, 0x004d1ee8,
	  FONT_PAL_SLOTS,  blit_plain },             /* 0x00495840 */
	{ "Moto",             8, 0x004d1f08,
	  FONT_MOTO_SLOTS, blit_plain },             /* 0x00495d50 */
	{ NULL, 0, 0, NULL, NULL, NULL }
};

const MR_FontClass *font_classes(void)
{
	return s_classes;
}

const MR_FontClass *font_class_by_name(const char *name)
{
	int i;
	if (!name)
		return NULL;
	for (i = 0; s_classes[i].name; i++)
		if (_stricmp(s_classes[i].name, name) == 0)
			return &s_classes[i];
	return NULL;
}


/* ---------------------------------------------------------------- */
/* Fonts                                                            */

/* Rebuild charFace/charWidth from the attached faces. Not a vtable
 * slot in the original - it does this inside face loading. */
static void font_remap_chars(MR_Font *f)
{
	int ch, i;

	for (ch = 0; ch < FONT_CHARS; ch++) {
		f->charFace[ch]  = -1;
		f->charWidth[ch] = 0;
		for (i = 0; i < f->faceCount; i++) {
			int w;
			if (font_face_glyph(&f->faces[i], ch, &w)) {
				f->charFace[ch]  = (signed char)i;
				f->charWidth[ch] = w;
				break;      /* faces are searched in order, first wins */
			}
		}
	}

	f->digitCell = 0;
	for (ch = '0'; ch <= '9'; ch++)
		if (f->charWidth[ch] > f->digitCell)
			f->digitCell = f->charWidth[ch];
}

int font_init(MR_Font *f, const char *typeName)
{
	const MR_FontClass *cls;
	int ch;

	if (!f)
		return 0;
	cls = font_class_by_name(typeName);
	if (!cls) {
		mrlog_err("font: unknown type \"%s\"\n",
		          typeName ? typeName : "(null)");
		return 0;
	}

	memset(f, 0, sizeof(*f));
	f->cls  = cls;
	f->kind = cls->kind;
	for (ch = 0; ch < FONT_CHARS; ch++)
		f->charFace[ch] = -1;

	/* White on black until the configuration says otherwise. */
	f->foreColor[0] = f->foreColor[1] = f->foreColor[2] = 0xff;
	f->cls->build_lut(f, 0, 0xff);
	return 1;
}

void font_free(MR_Font *f)
{
	int i;
	if (!f)
		return;
	for (i = 0; i < f->faceCount; i++)
		font_face_free(&f->faces[i]);
	memset(f, 0, sizeof(*f));
}

int font_add_face(MR_Font *f, const char *logical)
{
	if (!f || f->faceCount >= FONT_MAX_FACES)
		return 0;
	if (!font_face_load(&f->faces[f->faceCount], logical))
		return 0;

	/*
	 * One height for the whole font. The format gives each FACE a
	 * height and nothing forces four faces to agree; the original
	 * keeps a single glyphHeight on the object, so the first face
	 * wins and a later disagreement is worth saying out loud.
	 */
	if (f->faceCount == 0)
		f->glyphHeight = f->faces[0].glyphHeight;
	else if (f->faces[f->faceCount].glyphHeight != f->glyphHeight)
		mrlog("font: %s is %d tall, the font is %d - keeping %d\n",
	           logical, f->faces[f->faceCount].glyphHeight,
	           f->glyphHeight, f->glyphHeight);

	f->faceCount++;
	f->loaded = 1;
	font_remap_chars(f);
	f->cls->build_lut(f, 0, 0xff);
	return 1;
}

void font_build_lut(MR_Font *f, int firstIndex, int lastIndex)
{
	if (f && f->cls && f->cls->build_lut)
		f->cls->build_lut(f, firstIndex, lastIndex);
}

void font_set_colors(MR_Font *f, const unsigned char fore[3],
                     const unsigned char back[3])
{
	if (f && f->cls && f->cls->set_colors)
		f->cls->set_colors(f, fore, back);
}

void font_set_tabular_digits(MR_Font *f, int on)
{
	if (f)
		f->tabularDigits = on;
}

int font_height(const MR_Font *f)
{
	return f ? f->glyphHeight : 0;
}

/* The advance for one character, honouring tabular digits. */
static int advance_of(const MR_Font *f, unsigned char ch)
{
	if (f->tabularDigits && ch >= '0' && ch <= '9' && f->digitCell > 0)
		return f->digitCell;
	return f->charWidth[ch];
}

int font_text_width(const MR_Font *f, const char *s)
{
	int w    = 0;
	int line = 0;
	int n    = 0;

	if (!f || !s)
		return 0;
	for (; *s; s++) {
		unsigned char ch = (unsigned char)*s;

		/*
		 * A NEWLINE BREAKS THE LINE, so a multi-line string
		 * measures as its WIDEST line - Font_MeasureText tests
		 * 0x0a at 0x00492b7e. Without this the two lines of a
		 * racer callout measure as one long one.
		 */
		if (ch == '\n') {
			if (line > w)
				w = line;
			line = 0;
			n    = 0;
			continue;
		}
		if (f->charWidth[ch] == 0)
			continue;
		if (n++)
			line += f->charSpacing;
		line += advance_of(f, ch);
	}
	return (line > w) ? line : w;
}

/* ORIGIN  Font_DrawTextInternal @ 0x00493820, the test at 0x00493a5c */
int font_layout_divisor(void)
{
	return (g_options.smallFonts || g_options.screenMode != 2) ? 2 : 1;
}

/* ORIGIN  Font_ApplyAlignment @ 0x00492c20, whole */
void font_draw_text_aligned(MR_Font *f, int x, int y, const char *s,
                            unsigned int flags, int level)
{
	unsigned int h = flags & 0x1f;
	unsigned int v = flags & 0xe0;
	int          tw = 0;
	int          th = 0;

	if (!f || !s)
		return;

	/*
	 * THE COORDINATES ARE HALVED IN SMALL MODE, and this is the
	 * step that is easy to miss because it happens in the CALLER of
	 * the alignment, not in it.
	 *
	 * ORIGIN  Font_DrawTextInternal @ 0x00493820, at 0x00493a5c
	 *
	 *     if ((!g_menu || !g_menu->active) && param_1 == 0 &&
	 *         (g_cfgSmallFonts || g_cfgScreenMode != 2)) {
	 *         x /= 2;  y /= 2;
	 *     }
	 *
	 * param_1 is the first argument of the internal draw, and the
	 * two wrappers disagree about it: FUN_00493e60 and Font_DrawText
	 * pass 0 and DO halve, FUN_00493fe0 passes 1 and does not. The
	 * race HUD goes through the first and the counter through the
	 * second, which is why the counter's layout table carries its
	 * own half-size column and this one does not.
	 *
	 * VERIFIED AGAINST A RUNNING MOTO. A hook just past the
	 * alignment call caught the TIME banner at y 45 and its digits
	 * at 57, against the 0x5a and 0x73 the code passes - exactly
	 * halved, with 115/2 truncating to 57.
	 *
	 * o HALVE FIRST, THEN ALIGN. The original's order, and it
	 *   matters: the alignment subtracts from the full viewport
	 *   width either way, so halving afterwards would move a
	 *   right-aligned string to the middle of the screen.
	 */
	{
		int div = font_layout_divisor();

		x /= div;
		y /= div;
	}

	/*
	 * THE MEASURE IS SKIPPED FOR EXACTLY ONE FLAG WORD, and that is
	 * the original's own guard rather than an optimisation of ours:
	 * h == 1 with v == 0x20 is the "no alignment" case, the only one
	 * that needs neither a width nor a height.
	 */
	if (h != 1 || v != 0x20) {
		tw = font_text_width(f, s);
		th = font_height(f);
	}

	/*
	 * AGAINST THE VIEWPORT, not against any caller-supplied box.
	 * The original uses g_viewWidth and g_viewHeight; ours has no
	 * top margin and the clip rect IS the viewport.
	 */
	if (h == 2 || h == 4)
		x = (g_clipWidth - tw) / 2;
	else if (h == 8 || h == 0x10)
		x = (g_clipWidth - x) - tw;

	if (v == 0x40)
		y = (g_clipHeight - th) / 2;
	else if (v == 0x80)
		y = (g_clipHeight - y) - th;

	font_draw_text(f, x, y, s, level);
}

void font_draw_text(MR_Font *f, int x, int y, const char *s, int level)
{
	int n = 0;
	int left;

	if (!f || !s || !f->loaded || !g_frameBuffer)
		return;

	/*
	 * The 2D origin, once, at the start of the run - so the glyph
	 * advances below stay in authored units and a caller that
	 * measured with font_text_width gets what it measured. Zero
	 * unless the frame is wider than the art; see framebuffer.h.
	 */
	x += g_fb2dOriginX;
	left = x;
	for (; *s; s++) {
		unsigned char ch = (unsigned char)*s;
		int cw = f->charWidth[ch];
		int adv;

		/*
		 * A NEWLINE BREAKS THE LINE - FUN_00493490 tests 0x0a at
		 * 0x004936a9 - and the racer callout depends on it: its
		 * pointer glyph is alone on a second line.
		 *
		 * THE ORIGINAL RE-ALIGNS EVERY LINE, re-measuring the rest
		 * of the string from just past the newline (0x00493708) so
		 * a centred block centres line by line. Ours carries the x
		 * it started at, which is the same answer for flag 0x21 -
		 * no alignment - and wrong for centred multi-line text.
		 * [open]
		 */
		if (ch == '\n') {
			x = left;
			y += f->glyphHeight;
			n  = 0;
			continue;
		}
		if (cw == 0)
			continue;                       /* absent, and no advance */
		if (n++)
			x += f->charSpacing;
		adv = advance_of(f, ch);
		/* Centre a narrow digit in its cell so the column stays put. */
		f->cls->draw_glyph(f, x + (adv - cw) / 2, y, ch, level);
		x += adv;
	}
}
