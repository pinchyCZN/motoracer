/*
 * tga.c - the .TGA loader, from Bitmap_LoadFile (0x0043e280).
 *
 * See tga.h: this path is exercised by no shipped file, so it is
 * reconstructed but unverified.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tga.h"
#include "framebuffer.h"
#include "log.h"

#define TGA_HDR_SIZE 18

/*
 * The original reads the 18-byte header and rejects, with a fatal
 * error and an assert line number, anything that is not exactly:
 *
 *     [0]  idLength        == 0     "TGA File numid not supported"
 *     [1]  colorMapType    == 0     "TGA File must be no paletted"
 *     [2]  imageType       == 2     "TGA File imgtyp not supported"
 *     [7]  colorMapDepth   == 0     "TGA File must be no paletted"
 *     [16] pixelDepth      == 24    "TGA File must be 24 bits"
 *
 * imageType 2 is uncompressed true-colour, so there is no RLE path.
 */
int tga_load(const char *path, int *width, int *height,
             unsigned short **pixels)
{
	FILE *f;
	unsigned char hdr[TGA_HDR_SIZE];
	unsigned char *src = NULL;
	unsigned short *dst = NULL;
	int w;
	int h;
	int x;
	int y;
	size_t n;

	if (!width || !height || !pixels)
		return 0;
	*width = 0;
	*height = 0;
	*pixels = NULL;

	f = fopen(path, "rb");
	if (!f) {
		mrlog_err("tga: cannot open %s\n", path);
		return 0;
	}
	if (fread(hdr, 1, TGA_HDR_SIZE, f) != TGA_HDR_SIZE) {
		mrlog("tga: %s is shorter than a header\n", path);
		fclose(f);
		return 0;
	}

	if (hdr[0] != 0 || hdr[1] != 0 || hdr[2] != 2 || hdr[7] != 0 ||
	    hdr[16] != 24) {
		mrlog_err("tga: %s unsupported: id=%u map=%u type=%u mapdepth=%u "
		       "depth=%u\n", path, hdr[0], hdr[1], hdr[2], hdr[7],
		       hdr[16]);
		fclose(f);
		return 0;
	}

	w = (int)hdr[12] | ((int)hdr[13] << 8);
	h = (int)hdr[14] | ((int)hdr[15] << 8);
	if (w <= 0 || h <= 0) {
		mrlog("tga: %s has bad dimensions %dx%d\n", path, w, h);
		fclose(f);
		return 0;
	}

	/*
	 * The original seeks forward by hdr[17], the image descriptor
	 * byte, before reading pixels. That is almost certainly meant to
	 * be idLength - which it has already required to be zero - so for
	 * a well-formed file with descriptor 0 it is a no-op. Reproduced
	 * because a file with a non-zero descriptor would load differently
	 * in the original, and matching that matters more than being
	 * right about a case no shipped file exercises.
	 */
	if (hdr[17] != 0 && fseek(f, (long)hdr[17], SEEK_CUR) != 0) {
		fclose(f);
		return 0;
	}

	n = (size_t)w * (size_t)h * 3u;
	src = (unsigned char *)malloc(n);
	if (!src) {
		fclose(f);
		return 0;
	}
	if (fread(src, 1, n, f) != n) {
		mrlog_err("tga: %s is truncated\n", path);
		free(src);
		fclose(f);
		return 0;
	}
	fclose(f);

	dst = (unsigned short *)malloc((size_t)w * (size_t)h *
	                               sizeof(unsigned short));
	if (!dst) {
		free(src);
		return 0;
	}

	/*
	 * Targa stores rows bottom-up and pixels as B, G, R. The original
	 * walks the source backwards a row at a time to flip it, which is
	 * the same thing this does with an index.
	 */
	for (y = 0; y < h; y++) {
		const unsigned char *row = src + (size_t)(h - 1 - y) * w * 3;
		unsigned short *out = dst + (size_t)y * w;
		for (x = 0; x < w; x++) {
			out[x] = FB_RGB565(row[x * 3 + 2],   /* r */
			                   row[x * 3 + 1],   /* g */
			                   row[x * 3 + 0]);  /* b */
		}
	}

	free(src);
	*width  = w;
	*height = h;
	*pixels = dst;
	return 1;
}
