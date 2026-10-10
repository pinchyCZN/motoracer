/*
 * rawtex.h - the .RAW texture page format.
 *
 * Not a bespoke format: these are "mhwanh" files, with a 32-byte
 * BIG-ENDIAN header, carrying either 24-bit truecolour or 8-bit
 * paletted pixels. The big-endianness in an x86 game suggests the
 * format arrived from a Commodore or Atari lineage.
 *
 * Fully decoded before this loader was written - see
 * docs/formats/raw-texture.txt, verified across all 465 texture files
 * with the computed payload size matching the file size exactly, and
 * with a reference reader at tools/rawimg.py.
 *
 * So this is a transcription, and tools/rawimg.py is the oracle.
 */

#ifndef MRACER_RAWTEX_H
#define MRACER_RAWTEX_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	int            width;
	int            height;
	int            colours;      /* 0 means 24-bit truecolour */
	unsigned char *palette;      /* colours * 3, RGB; NULL if truecolour */
	unsigned char *pixels;       /* w*h*3 truecolour, else w*h indices  */
} RAWTEX;

/* Returns 1 on success. Rejects anything without the magic, and
 * anything whose payload does not match the file size exactly - which
 * is the check that pinned the layout in the first place. */
int rawtex_load(RAWTEX *t, const char *path);

void rawtex_free(RAWTEX *t);

#ifdef __cplusplus
}
#endif

#endif /* MRACER_RAWTEX_H */
