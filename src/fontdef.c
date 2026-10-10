/*
 * fontdef.c - DATA\FONTS.BPT, in the order Font_LoadAll reads it.
 *
 * ORIGIN  Font_LoadAll @ 0x00426460
 *
 * SIX SECTIONS, each a record list that ends on an id of -1. The
 * schema is in docs/structs/MR_Font.txt 5.1; the short version is
 * that a .bpt carries no type tags, so the read ORDER is the format
 * and guessing desynchronises the stream.
 *
 * WHAT IS NOT HERE: Font_LoadAll also constructs a font object per
 * entry and calls Font_Rebuild over the table at the end. This reads
 * the MANIFEST only - which class each id wants and which files its
 * faces come from - and leaves building to font.c, because the port
 * already has a working font builder and only ever lacked the table.
 */

#include <stdio.h>
#include <string.h>

#include "fontdef.h"
#include "bpt.h"
#include "options.h"
#include "log.h"

#define FONTDEF_FILE      "FONTS.BPT"
#define FONTDEF_END       (-1)
#define FONTDEF_MAX_COUNT 0x30    /* Font_LoadAll throws above this */

static MR_FontDef s_fonts[FONTDEF_MAX];
static int        s_count;
static int        s_tried;

/*
 * THE FACE PATH FIXUP, and it is two changes.
 *
 * "Data\Fontes\HiRes\MenuLink.Raw" has to become
 * "FONTES\HIRES\MENULINK.FNT":
 *
 *   o the leading "Data\" goes, because assets.c takes a LOGICAL
 *     name and a stock install has no such directory - rule 10;
 *   o the extension becomes .FNT, because that is what FONTES.BKF
 *     actually holds. Only SYMBOL.RAW and DELPHINE\DEBUG.RAW are
 *     genuinely .RAW.
 *
 * THE ORIGINAL DOES THE SAME SWAP [verified] - the ".FNT" literal
 * sits in the g_fontTable statics block at 0x004f1840. A path record
 * is an authoring-time name, not a member name.
 */
static void fontdef_fix_path(const char *in, char *out, int max)
{
	const char *p = in;
	char       *dot;
	int         n;

	if (!in || !out || max <= 0)
		return;

	/* Drop a leading "Data\" or "data/" however it is spelled. */
	if ((p[0] == 'D' || p[0] == 'd') &&
	    (p[1] == 'A' || p[1] == 'a') &&
	    (p[2] == 'T' || p[2] == 't') &&
	    (p[3] == 'A' || p[3] == 'a') &&
	    (p[4] == '\\' || p[4] == '/')) {
		p += 5;
	}

	for (n = 0; n + 1 < max && p[n]; n++) {
		char c = p[n];

		if (c == '/')
			c = '\\';
		out[n] = (char)((c >= 'a' && c <= 'z') ? (c - 'a' + 'A') : c);
	}
	out[n] = 0;

	dot = strrchr(out, '.');
	if (dot && (int)(dot - out) + 5 < max)
		strcpy(dot, ".FNT");
}

static void fontdef_store_face(MR_FontDef *f, int first, int last,
                             const char *path)
{
	int i;

	if (f->faceCount >= FONTDEF_FACES_MAX) {
		mrlog_err("fontdef:  %s wants a fifth face - dropped\n",
		          f->name);
		return;
	}
	i = f->faceCount++;
	f->faceFirst[i] = first;
	f->faceLast[i]  = last;
	fontdef_fix_path(path, f->faceName[i], (int)sizeof f->faceName[i]);
}

static MR_FontDef *fontdef_at(int id)
{
	if (id < 0 || id >= s_count || id >= FONTDEF_MAX)
		return NULL;
	return &s_fonts[id];
}

int fontdef_load(void)
{
	MR_Script   sc;
	MR_FontDef *f;
	char        buf[128];
	int         id;
	int         a, b, c;

	if (s_tried)
		return s_count;
	s_tried = 1;

	if (!bpt_open(&sc, FONTDEF_FILE)) {
		mrlog_err("fontdef:  no " FONTDEF_FILE " - no font table\n");
		return 0;
	}

	if (!bpt_read_int(&sc, &s_count))
		goto done;
	if (s_count < 0 || s_count > FONTDEF_MAX_COUNT) {
		mrlog_err("fontdef:  count %d is not a font count\n", s_count);
		s_count = 0;
		goto done;
	}
	if (s_count > FONTDEF_MAX)
		s_count = FONTDEF_MAX;

	/*
	 * SECTION 1, FontTypes: id, name, type, condition.
	 *
	 * A SECOND INT SITS BETWEEN THE COUNT AND THE FIRST ID in the
	 * shipped file - both read 35 - and which of the two is the
	 * count cannot be told apart by reading Font_LoadAll, because
	 * Settings_ReadInt collapses to a single read on a compiled
	 * .bpt and the key name is never matched against anything.
	 *
	 * So this DETECTS it rather than assuming: an id must be
	 * inside 0..count, which the stray value is not. The log says
	 * which way it went, so a file that disagrees is visible
	 * instead of silently parsed into nonsense.
	 */
	if (!bpt_read_int(&sc, &id))
		goto done;
	if (id < 0 || id >= s_count) {
		mrlog("fontdef:  skipping a %d between the count and the "
		      "first id\n", id);
		if (!bpt_read_int(&sc, &id))
			goto done;
	}

	while (id != FONTDEF_END) {
		f = fontdef_at(id);
		if (!bpt_read_string(&sc, buf, (int)sizeof buf))
			goto done;
		if (f) {
			strncpy(f->name, buf, sizeof f->name - 1);
			f->valid = 1;
		}
		if (!bpt_read_string(&sc, buf, (int)sizeof buf))
			goto done;
		if (f)
			strncpy(f->cls, buf, sizeof f->cls - 1);
		if (!bpt_read_string(&sc, buf, (int)sizeof buf))
			goto done;
		if (f)
			strncpy(f->cond, buf, sizeof f->cond - 1);

		if (!bpt_read_int(&sc, &id))
			goto done;
	}

	/* SECTION 2, FontSpacings: id, a, b. */
	if (!bpt_read_int(&sc, &id))
		goto done;
	while (id != FONTDEF_END) {
		if (!bpt_read_int(&sc, &a) || !bpt_read_int(&sc, &b))
			goto done;
		f = fontdef_at(id);
		if (f) {
			f->spacingA = a;
			f->spacingB = b;
		}
		if (!bpt_read_int(&sc, &id))
			goto done;
	}

	/* SECTION 3, FontForeColors: id, r, g, b. */
	if (!bpt_read_int(&sc, &id))
		goto done;
	while (id != FONTDEF_END) {
		if (!bpt_read_int(&sc, &a) || !bpt_read_int(&sc, &b) ||
		    !bpt_read_int(&sc, &c)) {
			goto done;
		}
		f = fontdef_at(id);
		if (f) {
			f->fore[0] = (unsigned char)a;
			f->fore[1] = (unsigned char)b;
			f->fore[2] = (unsigned char)c;
			f->hasFore = 1;
		}
		if (!bpt_read_int(&sc, &id))
			goto done;
	}

	/* SECTION 4, FontBackColors: the same shape. */
	if (!bpt_read_int(&sc, &id))
		goto done;
	while (id != FONTDEF_END) {
		if (!bpt_read_int(&sc, &a) || !bpt_read_int(&sc, &b) ||
		    !bpt_read_int(&sc, &c)) {
			goto done;
		}
		f = fontdef_at(id);
		if (f) {
			f->back[0] = (unsigned char)a;
			f->back[1] = (unsigned char)b;
			f->back[2] = (unsigned char)c;
			f->hasBack = 1;
		}
		if (!bpt_read_int(&sc, &id))
			goto done;
	}

	/*
	 * SECTION 5, FontData: id, firstChar, lastChar, kind, path.
	 *
	 * THE SECOND AND THIRD ARE A RANGE, NOT A START AND A COUNT -
	 * Font_LoadAll hands (last - first) + 1 to the face adder,
	 * which is what settles it. MenuLink reads 32,95 and 192,211.
	 */
	if (!bpt_read_int(&sc, &id))
		goto done;
	while (id != FONTDEF_END) {
		if (!bpt_read_int(&sc, &a) || !bpt_read_int(&sc, &b) ||
		    !bpt_read_int(&sc, &c)) {
			goto done;
		}
		if (!bpt_read_string(&sc, buf, (int)sizeof buf))
			goto done;
		f = fontdef_at(id);
		if (f)
			fontdef_store_face(f, a, b, buf);
		if (!bpt_read_int(&sc, &id))
			goto done;
	}

done:
	bpt_close(&sc);

	if (s_count > 0) {
		int i;
		int withFaces = 0;

		for (i = 0; i < s_count; i++) {
			if (s_fonts[i].faceCount > 0)
				withFaces++;
		}
		mrlog("fontdef:  %d fonts, %d with faces\n", s_count,
		      withFaces);
	}
	return s_count;
}

const MR_FontDef *fontdef_get(int id)
{
	const MR_FontDef *f = fontdef_at(id);

	return (f && f->valid) ? f : NULL;
}

/*
 * fontdef_resolve - the LowRes/HiRes swap, from Font_Get.
 *
 * ORIGIN  Font_Get @ 0x004903d0, the two switch blocks.
 *
 * TWELVE PAIRS, and the SAME twelve in both directions - one branch
 * maps every LowRes id to its HiRes twin and the other maps every
 * HiRes id back. An id in neither list is returned unchanged.
 *
 * THE CONDITION IS BOTH HALVES: small fonts OFF and screen mode 2,
 * which is 640x480. Testing g_options.smallFonts alone happens to
 * agree because ScreenMode defaults to 2 - but it stops agreeing
 * the moment somebody puts 0 or 1 in the ini, and then every
 * in-game font is the wrong size with nothing to point at.
 */
static const struct { int lo, hi; } kFontPairs[] = {
	{  0,  4 },  /* Chrono   */
	{  1,  5 },  /* Moto     */
	{  2,  6 },  /* Player   */
	{  3,  7 },  /* Time     */
	{ 10, 11 },  /* Sprites  */
	{ 13, 14 },  /* GameMsg  */
	{ 16, 15 },  /* SpeedLcd */
	{ 19, 20 },  /* Pos      */
	{ 23, 22 },  /* MotorLcd */
	{ 26, 25 },  /* FinalPos */
	{ 29, 28 },  /* Counter  */
	{ 33, 32 }   /* GameMenu */
};

int fontdef_resolve(int id)
{
	int wantHi = (g_options.smallFonts == 0 &&
	              g_options.screenMode == 2);
	int i;

	for (i = 0; i < (int)(sizeof kFontPairs / sizeof kFontPairs[0]);
	     i++) {
		if (wantHi && id == kFontPairs[i].lo)
			return kFontPairs[i].hi;
		if (!wantHi && id == kFontPairs[i].hi)
			return kFontPairs[i].lo;
	}
	return id;
}

/*
 * The point of the table: a caller names an id and gets a built
 * font, with no file name anywhere in its own source.
 */
int fontdef_open(MR_Font *f, int id)
{
	const MR_FontDef *d;
	int               n;

	/* The caller names the font; the resolution picks the twin. */
	id = fontdef_resolve(id);
	d  = fontdef_get(id);

	if (!f || !d)
		return 0;
	if (d->faceCount <= 0) {
		mrlog_err("fontdef: font %d (%s) has no face\n", id,
		          d->name);
		return 0;
	}
	if (!font_init(f, d->cls)) {
		mrlog_err("fontdef: font %d (%s) wants class %s\n",
		          id, d->name, d->cls);
		return 0;
	}

	/*
	 * EVERY FACE, NOT JUST THE FIRST. Id 14 carries POSTIME for the
	 * digits and POSTIMEL for the letters; built from one of them it
	 * draws numbers and nothing else.
	 */
	for (n = 0; n < d->faceCount; n++) {
		if (!font_add_face(f, d->faceName[n])) {
			mrlog_err("fontdef: font %d face %d (%s) failed\n",
			          id, n, d->faceName[n]);
			font_free(f);
			return 0;
		}
	}

	if (d->hasFore || d->hasBack) {
		font_set_colors(f, d->hasFore ? d->fore : NULL,
		                d->hasBack ? d->back : NULL);
	}
	return 1;
}

const char *fontdef_face(int id, int n)
{
	const MR_FontDef *f = fontdef_get(id);

	if (!f || n < 0 || n >= f->faceCount)
		return NULL;
	return f->faceName[n];
}
