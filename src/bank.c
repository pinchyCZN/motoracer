/*
 * bank.c - texture banks, the software half.
 * See bank.h and docs/subsystems/texture-banks.txt.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bank.h"
#include "rawtex.h"
#include "framebuffer.h"
#include "log.h"
#include "render.h"

static MR_Bank s_banks[BANK_COUNT];

/* See bank.h. The four are per bike DIRECTORY, and only two distinct
 * pages sit behind them - SPEED is byte-identical to SPEEDPKT and
 * KTMCROSS to KTMPKT. */
static const char * const s_bikeAtlas[] = {
	"SPEEDPKT\\BNK00_01.RAW",
	"SPEED\\BNK00_01.RAW",
	"KTMCROSS\\BNK00_01.RAW",
	"KTMPKT\\BNK00_01.RAW"
};

int bank_is_bike_atlas(const char *name)
{
	size_t n;
	int    i;

	if (!name)
		return 0;
	n = strlen(name);

	for (i = 0; i < (int)(sizeof s_bikeAtlas / sizeof s_bikeAtlas[0]);
	     i++) {
		size_t m = strlen(s_bikeAtlas[i]);
		char   before;

		if (n < m)
			continue;
		if (_stricmp(name + (n - m), s_bikeAtlas[i]) != 0)
			continue;
		/*
		 * THE TAIL MUST START AT A PATH BOUNDARY, or a directory
		 * merely ENDING in one of these names would match.
		 */
		before = (n > m) ? name[n - m - 1] : '\\';
		if (before != '\\' && before != '/')
			continue;
		return 1;
	}
	return 0;
}

MR_Bank *bank_get(int index)
{
	if (index < 0 || index >= BANK_COUNT)
		return NULL;
	return &s_banks[index];
}

/*
 * load_palettes - the base palette, plus up to seven siblings.
 *
 * texture-banks.txt section 4.1: every entry in the array is a palette
 * of 256 16-bit entries, 0x200 bytes, not a mix of palettes and
 * sub-pages as an earlier reading guessed. The non-zero mode path then
 * looks for SIBLING files - the same name with suffixes 1 through 7 -
 * and stops at the first that will not open. So one bank can carry
 * eight palettes: the original plus seven alternates.
 *
 * Alternate palettes for one image is how a 1997 title recolours the
 * same texture, most obviously for the four bikes, of which only two
 * distinct images exist.
 */
static void load_palettes(MR_Bank *b, const RAWTEX *t, const char *path,
                          int mode)
{
	int i;
	unsigned short *pal;

	if (t->colours <= 0 || !t->palette)
		return;

	pal = (unsigned short *)malloc(0x200);
	if (!pal)
		return;
	memset(pal, 0, 0x200);
	for (i = 0; i < t->colours && i < 256; i++) {
		pal[i] = FB_RGB565(t->palette[i * 3 + 0],
		                   t->palette[i * 3 + 1],
		                   t->palette[i * 3 + 2]);
	}
	b->palette[b->paletteCount++] = pal;

	if (mode == 0)
		return;

	/*
	 * The siblings. The original splits the path and rebuilds it with
	 * the numeric suffix; we replace the extension the same way. Not
	 * exercised by any file in the extracted tree examined so far, so
	 * the loop is written to fail quietly.
	 */
	for (i = 1; i <= 7 && b->paletteCount < BANK_MAX_PALETTES; i++) {
		char sib[MAX_PATH];
		char *dot;
		RAWTEX st;

		if (strlen(path) + 4 >= sizeof(sib))
			break;
		strcpy(sib, path);
		dot = strrchr(sib, '.');
		if (!dot)
			break;
		sprintf(dot, ".%d", i);

		if (!rawtex_load(&st, sib))
			break;                      /* stop at the first missing */
		if (st.colours > 0 && st.palette) {
			int k;
			pal = (unsigned short *)malloc(0x200);
			if (pal) {
				memset(pal, 0, 0x200);
				for (k = 0; k < st.colours && k < 256; k++) {
					pal[k] = FB_RGB565(st.palette[k * 3 + 0],
					                   st.palette[k * 3 + 1],
					                   st.palette[k * 3 + 2]);
				}
				b->palette[b->paletteCount++] = pal;
			}
		}
		rawtex_free(&st);
	}
}

int bank_load(int index, const char *path, int mode)
{
	MR_Bank *b;
	RAWTEX   t;
	int      x;
	int      y;

	/* Bankw_LoadBank refuses anything above 0x2c. */
	if (index < 0 || index > BANK_MAX_INDEX) {
		mrlog("bank: index %d is out of range\n", index);
		return 0;
	}
	b = &s_banks[index];
	bank_free(index);

	if (!rawtex_load(&t, path))
		return 0;

	/*
	 * The page is allocated at the full 256x256x2 regardless of what
	 * the file holds - the original computes height * 512 and the
	 * shipped pages are 256 tall. A null result is FATAL there, with
	 * "MALLOC error in BANK creating."; we return failure instead.
	 */
	b->page = (unsigned short *)malloc(BANK_PAGE_BYTES);
	if (!b->page) {
		rawtex_free(&t);
		return 0;
	}
	memset(b->page, 0, BANK_PAGE_BYTES);
	b->ownsPage = 1;
	b->width    = t.width;
	b->height   = t.height;

	if (t.colours > 0) {
		/* Paletted: the page holds indices, one byte per pixel, and
		 * the palette is what resolves them. */
		b->isIndexed = 1;
		load_palettes(b, &t, path, mode);
		for (y = 0; y < t.height && y < BANK_PAGE_H; y++) {
			unsigned char *dst = (unsigned char *)b->page +
			                     (size_t)y * (BANK_PAGE_W * 2);
			const unsigned char *src = t.pixels + (size_t)y * t.width;
			int n = (t.width < BANK_PAGE_W) ? t.width : BANK_PAGE_W;
			memcpy(dst, src, (size_t)n);
		}
	} else {
		b->isIndexed = 0;

		/*
		 * *** THE MODE-0 SKIP, REPRODUCED DELIBERATELY ***
		 *
		 * A truecolour .RAW that is exactly 256x256 loaded with mode 0
		 * allocates the page, reads NO pixels into it, and returns
		 * success. Read from the disassembly at 0x004012dc, not from
		 * the decompiler; the jump target is the ordinary success exit
		 * and not an alternative bulk reader - that was checked,
		 * because a fast path would have been the comfortable
		 * explanation. See texture-banks.txt section 4.2.
		 *
		 * So a caller that wants pixels out of a 256x256 truecolour
		 * page must not pass mode 0. MenuPage25 passes 1.
		 */
		if (t.width == 0x100 && t.height == 0x100 && mode == 0) {
			mrlog("bank: %d %s 256x256 truecolour mode 0 - page left "
			       "empty, matching the original\n", index, path);
		} else {
			for (y = 0; y < t.height && y < BANK_PAGE_H; y++) {
				unsigned short *dst = b->page + (size_t)y * BANK_PAGE_W;
				const unsigned char *src = t.pixels +
				                           (size_t)y * t.width * 3u;
				int n = (t.width < BANK_PAGE_W) ? t.width : BANK_PAGE_W;
				for (x = 0; x < n; x++) {
					/* The original shifts each channel into a true
					 * RGB555 word and then converts that to the
					 * surface format. We are fixed at 565, so the two
					 * steps collapse into one. */
					dst[x] = FB_RGB565(src[x * 3 + 0],
					                   src[x * 3 + 1],
					                   src[x * 3 + 2]);
				}
			}
		}
	}

	strncpy(b->name, path, sizeof(b->name) - 1);
	b->name[sizeof(b->name) - 1] = 0;
	b->loaded = 1;

	mrlog("bank: %2d  %s\n", index, path);
	mrlog("      %dx%d %s, %d palette%s\n", t.width, t.height,
	       b->isIndexed ? "indexed" : "truecolour",
	       b->paletteCount, b->paletteCount == 1 ? "" : "s");

	rawtex_free(&t);
	return 1;
}

void bank_set_palette(int index, int palette)
{
	MR_Bank *b = bank_get(index);

	if (!b || palette < 0 || palette >= b->paletteCount)
		return;
	if (b->activePalette == palette)
		return;
	b->activePalette = palette;
	r_invalidate_bank(index);
}

void bank_alloc_texture_slot(int index, int wantKey, int paletted,
                             int palBits, int rgbBits)
{
	MR_Bank *b = bank_get(index);

	if (!b)
		return;

	/*
	 * The ORs in issue order, from 0x0041b24a onward. rgbBits of 16 is
	 * the only source width page 14 uses; the paletted widths are here
	 * because leaving them out would make the next caller think the
	 * function only handles one case.
	 */
	if (rgbBits == 16)
		b->flags |= BANK_FLAG_SRC16;
	else if (rgbBits == 8)
		b->flags |= BANK_FLAG_SRC8;

	if (paletted)
		b->flags |= (palBits == 4) ? BANK_FLAG_PAL4 : BANK_FLAG_PAL8;

	/*
	 * The key bit is SET OR CLEARED, never just set - the original
	 * clears it at 0x0041b56b when wantKey is zero. A bank reused for
	 * an unkeyed page would otherwise keep the previous page's key.
	 *
	 * AND THE FLAG IS WHAT KEYS THE TEXTURE. It is read again in
	 * bank_texture (d3d9rt.cpp), which is why this is the only switch
	 * a page needs to touch.
	 */
	if (wantKey)
		b->flags |= BANK_FLAG_KEYED;
	else
		b->flags &= (unsigned short)~BANK_FLAG_KEYED;

	/*
	 * The flag is what the back end keys on, and the texture is built
	 * lazily on first use - so a bank whose texture already exists has
	 * to be dropped. Every current caller allocates the slot straight
	 * after loading the page, before anything draws, so this never
	 * fires; it is here so that a caller which reuses a bank does not
	 * silently keep the previous page's key.
	 */
	r_invalidate_bank(index);
}

int bank_create_empty(int index, int width, int height)
{
	MR_Bank *b;

	if (index < 0 || index > BANK_MAX_INDEX) {
		mrlog("bank: index %d is out of range\n", index);
		return 0;
	}
	if (width <= 0 || width > BANK_PAGE_W ||
	    height <= 0 || height > BANK_PAGE_H) {
		mrlog("bank: %dx%d is not a usable page size\n", width, height);
		return 0;
	}

	b = &s_banks[index];
	bank_free(index);

	/* Full 256x256x2 whatever the requested size, exactly as bank_load
	 * does - the width and height describe the USED region. */
	b->page = (unsigned short *)malloc(BANK_PAGE_BYTES);
	if (!b->page)
		return 0;
	memset(b->page, 0, BANK_PAGE_BYTES);

	b->ownsPage  = 1;
	b->width     = width;
	b->height    = height;
	b->isIndexed = 0;
	b->loaded    = 1;
	strcpy(b->name, "<screen capture>");
	return 1;
}

int bank_capture_screen(int index)
{
	MR_Bank *b = bank_get(index);

	if (!b || !b->page || b->isIndexed)
		return 0;

	/*
	 * THE CAPTURE IS 256x192 INTO A 256-WIDE PAGE, so the pitch is the
	 * page width and not the capture width. Getting that wrong shears
	 * the image by 64 pixels a row, which looks like a transform bug
	 * rather than a stride bug.
	 */
	if (!r_capture_screen(b->page, 0x100, 0xc0, BANK_PAGE_W))
		return 0;

	/* The page changed underneath the texture, so the back end has to
	 * upload it again. The original does the same thing explicitly,
	 * with maybe_D3D_ReloadTexture right after the copy. */
	r_invalidate_bank(index);
	return 1;
}

void bank_free(int index)
{
	MR_Bank *b;
	int i;

	b = bank_get(index);
	if (!b)
		return;
	/* ownsPage is the only thing distinguishing a system-memory page
	 * the engine frees itself from a driver surface it must not
	 * touch. Here everything is ours, but the field is kept. */
	if (b->page && b->ownsPage)
		free(b->page);
	for (i = 0; i < BANK_MAX_PALETTES; i++) {
		if (b->palette[i])
			free(b->palette[i]);
	}
	memset(b, 0, sizeof(*b));
}

void bank_free_all(void)
{
	int i;
	for (i = 0; i < BANK_COUNT; i++)
		bank_free(i);
}
