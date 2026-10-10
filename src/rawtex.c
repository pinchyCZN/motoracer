/*
 * rawtex.c - the .RAW ("mhwanh") texture page format.
 * See rawtex.h and docs/formats/raw-texture.txt.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rawtex.h"
#include "assets.h"
#include "log.h"

#define RAW_HEADER_SIZE 0x20

/* Every field after the magic is big-endian. */
static unsigned int be16(const unsigned char *p)
{
	return ((unsigned int)p[0] << 8) | (unsigned int)p[1];
}

/*
 * The name is a LOGICAL ASSET NAME, not a filesystem path - see
 * assets.h and CLAUDE.md rule 10. "PAGES\TITLEDSI\BNK00_00.RAW"
 * resolves as member "TITLEDSI\BNK00_00.RAW" of PAGES.BKF.
 *
 * This used to read the file incrementally through a FILE *. Reading
 * the whole asset up front is what the archive route needs anyway, and
 * it turns the size check below into a comparison against a number we
 * already have rather than a seek to the end.
 */
int rawtex_load(RAWTEX *t, const char *name)
{
	unsigned char *data;
	unsigned int   fileSize = 0;
	size_t         palBytes;
	size_t         pixBytes;
	size_t         want;
	unsigned int   version;

	if (!t || !name)
		return 0;
	memset(t, 0, sizeof(*t));

	data = (unsigned char *)asset_load(name, &fileSize);
	if (!data)
		return 0;

	if (fileSize < RAW_HEADER_SIZE) {
		mrlog("raw: %s is shorter than a header\n", name);
		free(data);
		return 0;
	}
	if (memcmp(data, "mhwanh", 6) != 0) {
		mrlog("raw: %s is not an mhwanh file\n", name);
		free(data);
		return 0;
	}
	version = be16(data + 6);
	if (version != 4) {
		/* Every shipped file is version 4. Refuse rather than guess. */
		mrlog("raw: %s has version %u, expected 4\n", name, version);
		free(data);
		return 0;
	}

	t->width   = (int)be16(data + 8);
	t->height  = (int)be16(data + 10);
	t->colours = (int)be16(data + 12);
	/* +0x0e and +0x10 are dpi, metadata the game ignores. +0x12 is
	 * 14 reserved bytes, zero in every file examined. */

	if (t->width <= 0 || t->height <= 0 || t->colours < 0 ||
	    t->colours > 256) {
		mrlog("raw: %s has a bad header %dx%d colours=%d\n",
		       name, t->width, t->height, t->colours);
		free(data);
		return 0;
	}

	palBytes = (size_t)t->colours * 3u;
	pixBytes = (size_t)t->width * (size_t)t->height *
	           (t->colours == 0 ? 3u : 1u);

	/*
	 * THE SIZE CHECK IS THE POINT. This formula reproduces the file
	 * size for all 465 shipped files with no exceptions, which is what
	 * pinned the layout. Anything that does not match it is not a file
	 * we understand, so refuse it rather than read a prefix.
	 */
	want = (size_t)RAW_HEADER_SIZE + palBytes + pixBytes;
	if ((size_t)fileSize != want) {
		mrlog("raw: %s is %u bytes, the header implies %u\n",
		       name, fileSize, (unsigned)want);
		free(data);
		return 0;
	}

	if (palBytes > 0) {
		t->palette = (unsigned char *)malloc(palBytes);
		if (!t->palette) {
			free(data);
			rawtex_free(t);
			return 0;
		}
		memcpy(t->palette, data + RAW_HEADER_SIZE, palBytes);
	}

	t->pixels = (unsigned char *)malloc(pixBytes);
	if (!t->pixels) {
		free(data);
		rawtex_free(t);
		return 0;
	}
	memcpy(t->pixels, data + RAW_HEADER_SIZE + palBytes, pixBytes);

	free(data);
	return 1;
}

void rawtex_free(RAWTEX *t)
{
	if (!t)
		return;
	if (t->palette) free(t->palette);
	if (t->pixels)  free(t->pixels);
	memset(t, 0, sizeof(*t));
}
