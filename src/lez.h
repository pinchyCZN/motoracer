/*
 * lez.h - the .LEZ image format.
 *
 * A "LEZ1" file is an LZ77-compressed 8-bit palettised image. The
 * original loads the raw file in Lez_LoadFile (0x0043e700) and defers
 * the decompression to the first draw, in Lez_Decode (0x0043eb00); we
 * do both at once, because nothing here is expensive enough to be
 * worth deferring.
 *
 * The format is written up in docs/formats/lez.txt.
 *
 * Output is RGB565, matching the back buffer. The original converts
 * the palette through the run-time masks g_shrR / g_shlR and friends,
 * which is what makes it work at 555 as well; we are fixed at 565, so
 * the conversion is a constant.
 */

#ifndef MRACER_LEZ_H
#define MRACER_LEZ_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	int             width;
	int             height;
	unsigned short *pixels;   /* width * height, RGB565, top-down */
} LEZ_IMAGE;

/*
 * lez_load - read and decode a .LEZ into RGB565.
 *
 * name is a LOGICAL ASSET NAME, not a filesystem path - see
 * assets.h. "ILE01\\LOAD.LEZ" resolves inside ILE01.BKF.
 * Returns 1 on success, and then out->pixels must be released with
 * lez_free(). Returns 0 on any failure, having printed the reason and
 * left out zeroed.
 */
int lez_load(const char *name, LEZ_IMAGE *out);

/* Release the pixels. Safe on a zeroed or already-freed image. */
void lez_free(LEZ_IMAGE *img);

#ifdef __cplusplus
}
#endif

#endif /* MRACER_LEZ_H */
