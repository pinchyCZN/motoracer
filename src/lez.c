/*
 * lez.c - the .LEZ image format: LZ77 over an 8-bit palettised image.
 *
 * Reconstructed from Lez_LoadFile (0x0043e700) and Lez_Decode
 * (0x0043eb00). See docs/formats/lez.txt for the layout and for what
 * was verified against the shipped data.
 *
 * TWO DELIBERATE DIVERGENCES FROM THE ORIGINAL, both marked below:
 * every read is bounds-checked, and the palette is always built out to
 * 256 entries. The original does neither. It can afford not to,
 * because it only ever opens files it shipped.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lez.h"
#include "assets.h"
#include "framebuffer.h"
#include "log.h"

#define LEZ_MAGIC 0x315A454CU       /* 'LEZ1' little-endian */

/* Compressed header, all little-endian u32. */
#define LEZ_OFF_MAGIC     0x00
#define LEZ_OFF_USIZE     0x04
#define LEZ_OFF_MATCHES   0x08      /* offset of the u16 token stream */
#define LEZ_OFF_LITERALS  0x0c      /* offset of the literal bytes    */
#define LEZ_OFF_DIMS      0x10      /* height in low 16, width in high */
#define LEZ_OFF_CONTROL   0x14      /* first control dword            */

/* Decompressed image header. */
#define IMG_OFF_NPAL      0x05      /* i16, unaligned                 */
#define IMG_OFF_WIDTH     0x0c      /* i16                            */
#define IMG_OFF_HEIGHT    0x0e      /* i16                            */
#define IMG_OFF_PALETTE   0x12      /* npal entries of b,g,r          */

static unsigned int rd32(const unsigned char *p)
{
	return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
	       ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

static int rd16s(const unsigned char *p)
{
	return (int)(short)((unsigned short)p[0] |
	                    ((unsigned short)p[1] << 8));
}

/*
 * Read a whole asset. Caller frees.
 *
 * This used to fopen a path. It now goes through assets.c, which
 * resolves the name inside a .BKF archive first - see CLAUDE.md rule
 * 10. Nothing else in this file changed: the decoder always worked
 * from a buffer, so swapping where the buffer comes from was the whole
 * conversion.
 */
static unsigned char *read_file(const char *name, size_t *size_out)
{
	unsigned int n = 0;
	void        *buf = asset_load(name, &n);

	*size_out = (size_t)n;
	return (unsigned char *)buf;
}

/*
 * lez_decompress - LZ77 with a separate control bitstream.
 *
 * Three streams, each at its own offset in the file: control dwords
 * from 0x14, u16 match tokens, and literal bytes. One control bit per
 * item, MSB first:
 *
 *     1 -> copy one literal byte
 *     0 -> a 16-bit token: distance = bits 0..9, length = bits 10..15
 *          plus 3, copied byte by byte from earlier output
 *
 * The bit reader is the shift-until-zero idiom the original compiles
 * to: the word is pre-shifted with a 1 in bit 0 as a sentinel, so the
 * word reaching zero is exactly the signal to refill, and the carry
 * out of the shift is the bit. Reproduced faithfully because getting
 * it subtly wrong desynchronises the streams rather than failing.
 */
static int lez_decompress(const unsigned char *src, size_t src_size,
                          unsigned char *dst, unsigned int dst_size)
{
	unsigned int  match_off;
	unsigned int  lit_off;
	unsigned int  ctrl_off;
	unsigned int  bits;
	unsigned int  op;
	int           cf;

	match_off = rd32(src + LEZ_OFF_MATCHES);
	lit_off   = rd32(src + LEZ_OFF_LITERALS);
	ctrl_off  = LEZ_OFF_CONTROL;

	if (match_off >= src_size || lit_off >= src_size)
		return 0;

	bits = rd32(src + ctrl_off);
	ctrl_off += 4;
	cf   = (int)(bits >> 31);
	bits = (bits << 1) | 1u;

	op = 0;
	for (;;) {
		if (cf) {
			/* Literal. */
			if (lit_off >= src_size || op >= dst_size)
				return 0;
			dst[op++] = src[lit_off++];
		} else {
			unsigned int tok;
			unsigned int dist;
			unsigned int len;
			unsigned int sp;

			if (match_off + 1 >= src_size)
				return 0;
			tok = (unsigned int)src[match_off] |
			      ((unsigned int)src[match_off + 1] << 8);
			match_off += 2;

			dist = tok & 0x3ffu;
			len  = (tok >> 10) + 3u;

			/* DIVERGENCE: the original trusts the stream. */
			if (dist == 0 || dist > op || op + len > dst_size)
				return 0;

			sp = op - dist;
			while (len--)
				dst[op++] = dst[sp++];   /* byte at a time: may overlap */
		}

		if (op >= dst_size)
			break;

		cf   = (int)(bits >> 31);
		bits <<= 1;
		if (bits == 0) {
			unsigned int d;
			if (ctrl_off + 4 > src_size)
				return 0;
			d = rd32(src + ctrl_off);
			ctrl_off += 4;
			bits = (d << 1) | (unsigned int)cf;
			cf   = (int)(d >> 31);
		}
	}
	return 1;
}

/*
 * build_palette - the decompressed image's palette as RGB565.
 *
 * Entries are three bytes, BLUE first, then green, then red. The
 * original converts them in place over the top of themselves, reading
 * three bytes ahead of the two it writes; we build a separate table
 * because there is no reason to be clever about 512 bytes.
 *
 * npal == 0 means a 256-level greyscale ramp, which the original
 * builds into g_lezDefaultPalette.
 *
 * DIVERGENCE: 256 entries are always produced. The original converts
 * only npal of them, so a pixel whose index is >= npal samples
 * whatever bytes follow. Ours are black instead of arbitrary.
 */
static void build_palette(const unsigned char *img, unsigned int img_size,
                          int npal, unsigned short pal[256])
{
	int i;

	memset(pal, 0, 256 * sizeof(unsigned short));

	if (npal <= 0) {
		for (i = 0; i < 256; i++)
			pal[i] = FB_RGB565(i, i, i);
		return;
	}
	if (npal > 256)
		npal = 256;

	for (i = 0; i < npal; i++) {
		unsigned int off = IMG_OFF_PALETTE + (unsigned int)i * 3u;
		if (off + 2 >= img_size)
			break;
		pal[i] = FB_RGB565(img[off + 2],    /* r */
		                   img[off + 1],    /* g */
		                   img[off + 0]);   /* b */
	}
}

int lez_load(const char *name, LEZ_IMAGE *out)
{
	unsigned char *file = NULL;
	unsigned char *img  = NULL;
	size_t         file_size = 0;
	unsigned int   usize;
	int            npal;
	int            w;
	int            h;
	unsigned int   px_off;
	unsigned short pal[256];
	int            i;
	int            n;

	if (!out)
		return 0;
	memset(out, 0, sizeof(*out));

	file = read_file(name, &file_size);
	if (!file)
		return 0;

	if (file_size < LEZ_OFF_CONTROL + 4 ||
	    rd32(file + LEZ_OFF_MAGIC) != LEZ_MAGIC) {
		mrlog("lez: %s is not a LEZ1 file\n", name);
		free(file);
		return 0;
	}

	usize = rd32(file + LEZ_OFF_USIZE);
	if (usize < IMG_OFF_PALETTE) {
		mrlog_err("lez: %s has an implausible size %u\n", name, usize);
		free(file);
		return 0;
	}

	img = (unsigned char *)malloc(usize);
	if (!img) {
		free(file);
		return 0;
	}
	if (!lez_decompress(file, file_size, img, usize)) {
		mrlog_err("lez: %s failed to decompress\n", name);
		free(img);
		free(file);
		return 0;
	}
	free(file);
	file = NULL;

	npal = rd16s(img + IMG_OFF_NPAL);
	w    = rd16s(img + IMG_OFF_WIDTH);
	h    = rd16s(img + IMG_OFF_HEIGHT);

	if (w <= 0 || h <= 0 || npal < 0 || npal > 256) {
		mrlog("lez: %s has a bad image header %dx%d npal=%d\n",
		       name, w, h, npal);
		free(img);
		return 0;
	}

	build_palette(img, usize, npal, pal);

	px_off = IMG_OFF_PALETTE + (unsigned int)npal * 3u;
	if (px_off + (unsigned int)w * (unsigned int)h > usize) {
		mrlog_err("lez: %s is truncated: need %u, have %u\n", name,
		       px_off + (unsigned int)w * (unsigned int)h, usize);
		free(img);
		return 0;
	}

	out->pixels = (unsigned short *)malloc((size_t)w * (size_t)h *
	                                       sizeof(unsigned short));
	if (!out->pixels) {
		free(img);
		return 0;
	}
	out->width  = w;
	out->height = h;

	n = w * h;
	for (i = 0; i < n; i++)
		out->pixels[i] = pal[img[px_off + (unsigned int)i]];

	free(img);
	return 1;
}

void lez_free(LEZ_IMAGE *img)
{
	if (!img)
		return;
	if (img->pixels)
		free(img->pixels);
	img->pixels = NULL;
	img->width  = 0;
	img->height = 0;
}
