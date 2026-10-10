/*
 * tga.h - the .TGA loader.
 *
 * Reconstructed from Bitmap_LoadFile (0x0043e280), which despite its
 * name reads Targa, not Windows BMP: its four failure messages all
 * say "TGA File ...".
 *
 * NO SHIPPED DATA USES IT. All 152 menu images under
 * DATA/extracted/MENU/BITMAP are .LEZ, and there is not one .TGA
 * anywhere in the tree. Sprite_LoadFromFile still routes to it for any
 * extension that is not .LEZ, so it is presumably how art entered the
 * pipeline during development. See docs/formats/tga.txt.
 *
 * That also means THIS CODE IS UNVERIFIED against real data, unlike
 * the .LEZ path. It matches what the original accepts; nothing has
 * been decoded with it.
 */

#ifndef MRACER_TGA_H
#define MRACER_TGA_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * tga_load - read a 24-bit uncompressed Targa into RGB565.
 * Same contract as lez_load: 1 on success, out->pixels then owned by
 * the caller. The original rejects anything else outright with a fatal
 * error; we report and return 0.
 */
int tga_load(const char *path, int *width, int *height,
             unsigned short **pixels);

#ifdef __cplusplus
}
#endif

#endif /* MRACER_TGA_H */
