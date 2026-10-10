/*
 * framebuffer.h - the 16 bpp back buffer and its present.
 *
 * This is the host replacement for the DirectDraw back surface. The
 * original locks a DDraw surface and hands the raw pointer and its
 * pitch to the software rasterizer as g_frameBuffer and
 * g_frameStridePx; those names are kept here so transcribed code reads
 * the same. See docs/subsystems/rasterizer-pipeline.txt and
 * docs/structs/MR_Sprite.txt section 5.
 *
 * Format is fixed at RGB565, top-down. The original derived its masks
 * at run time from whatever DirectDraw reported, 565 or 555, into
 * g_maskR / g_shlR / g_shrR - see docs/subsystems/pixel-format.txt. We
 * do not have to be adaptive: we own the buffer, so we pick 565 and
 * the GDI present converts to whatever the desktop is.
 */

#ifndef MRACER_FRAMEBUFFER_H
#define MRACER_FRAMEBUFFER_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The back buffer. g_frameStridePx is in PIXELS, not bytes, so a row
 * starts at g_frameBuffer + y * g_frameStridePx. It is not necessarily
 * g_frameWidth: DIB rows are DWORD aligned, so an odd width is padded
 * by one pixel.
 */
extern unsigned short *g_frameBuffer;
extern int g_frameStridePx;
extern int g_frameWidth;
extern int g_frameHeight;

/*
 * The clip rectangle every blit is bounded by: 0..g_clipWidth-1 and
 * 0..g_clipHeight-1. fb_create sets them to the full buffer. They are
 * separate from the buffer size in the original too, which is how it
 * confines drawing to a sub-rect of the surface.
 */
extern int g_clipWidth;
extern int g_clipHeight;

/*
 * FB_DESIGN_W, FB_DESIGN_H - the resolution the 2D art is authored
 * at, and the coordinate space every menu page is written in.
 *
 * The original owns the display mode and every mode it offers is
 * 4:3, so for it these and the frame size are the same number -
 * Sprite_SetPosition centres against the literal 0x280. The moment
 * the frame can be 16:9 they part company: the frame widens and the
 * art does not.
 */
#define FB_DESIGN_W 640
#define FB_DESIGN_H 480

/*
 * g_fb2dOriginX - where the FB_DESIGN_W-wide block of authored art
 * sits inside the frame.
 *
 * Zero at 4:3. At 854x480 it is 107, which is what makes a menu page
 * draw centred rather than flush left with 214 black columns beside
 * it.
 *
 * IT IS ADDED BY sprite_draw AND font_draw_text AND BY NOTHING ELSE.
 * Those are the two blitters that place authored art. Everything
 * that is meant to cover the whole frame - the scrolling background
 * in sprite_draw_wave, the horizon band, the framebuffer quad - goes
 * a different route and is unaffected, which is the point of putting
 * the offset there rather than in fb_create.
 *
 * THE RACE LEAVES IT AT ZERO. hud_align anchors against the real
 * frame, which is what a HUD should do at any aspect, so an origin
 * on top of it would push the corners back inward.
 */
extern int g_fb2dOriginX;
void fb_set_2d_origin(int x);

/* Pack 8:8:8 into 5:6:5. The low bits of each channel are discarded,
 * which is the same direction the original loses them in. */
#define FB_RGB565(r, g, b)                          \
    ((unsigned short)((((r) & 0xf8) << 8) |         \
                      (((g) & 0xfc) << 3) |         \
                      (((b) & 0xf8) >> 3)))

/* Allocate the back buffer. Returns 1 on success. Safe to call again;
 * the previous buffer is released first. */
int fb_create(int w, int h);

/* Release the back buffer. Safe on a buffer that was never created. */
void fb_destroy(void);

/* Fill every pixel, padding included. */
void fb_clear(unsigned short color);

/*
 * THE OVERLAY LAYER, and why there has to be one.
 *
 * g_frameBuffer is drawn as a full-screen quad BEFORE the geometry,
 * which is right for a menu page - the original draws a page's
 * sprites and only then calls Screen3D_DrawScene. It is wrong for
 * anything the original draws AFTER the scene, and the in-race menus
 * are exactly that: they float over a running race.
 *
 * Compositing a rectangle of g_frameBuffer back on top does not work,
 * because by then that rectangle holds the HORIZON the race drew into
 * it - the escape menu came out sitting on an opaque slab of sky.
 *
 * So overlay content gets its own buffer, cleared to a key colour that
 * the blit turns into alpha 0. Callers do not address it directly:
 * fb_overlay_begin points g_frameBuffer at it so every existing font
 * and sprite blitter draws into it unchanged, and fb_overlay_end puts
 * the real one back.
 */
/*
 * TWO BUFFERS, BLACK AND WHITE, AND THAT IS THE WHOLE TRICK.
 *
 * A colour key was the obvious way to make the untouched parts of the
 * overlay transparent, and it is not good enough: blit_shaded_transp
 * BLENDS WITH THE DESTINATION, so every anti-aliased edge came out
 * part key colour. A magenta key gave the title a pink halo; a black
 * key would give it a dark one and would also punch holes through any
 * glyph pixel that is genuinely black - which the row font's hotkey
 * letters are.
 *
 * So the overlay is drawn TWICE, once over black and once over white,
 * and the alpha is recovered from the difference. For a blend of the
 * form out = s - (s - d) * a:
 *
 *     over black   c0 = s * (1 - a)
 *     over white   c1 = s * (1 - a) + a
 *     c1 - c0      = a                 the coverage, exactly
 *     c0                               the colour, premultiplied
 *
 * An untouched pixel gives c0 = 0, c1 = white, so alpha 0. A pixel
 * written flat by a non-blending blitter gives c0 = c1 = s, alpha 1.
 * Both fall out of the same two lines, with no key and no reserved
 * colour.
 */
extern unsigned short *g_overlayBuffer;    /* the over-black pass */
extern unsigned short *g_overlayBuffer2;   /* the over-white pass */

/*
 * Clear one overlay pass and redirect drawing into it.
 *
 *     pass 0   cleared to black
 *     pass 1   cleared to white
 *
 * Returns 0 if there is no overlay buffer, in which case nothing was
 * redirected and fb_overlay_end must not be called.
 *
 * NOT NESTABLE. Draw the same content in both passes; anything that
 * differs between them is read as coverage and will come out wrong.
 */
int  fb_overlay_begin(int pass);
void fb_overlay_end(void);

/* Blit to a device context, scaled into client_w x client_h with the
 * aspect ratio preserved and the margins painted black. */
void fb_blit(HDC hdc, int client_w, int client_h);

/* fb_blit against the window's own DC, sized from its client rect. */
void fb_present(HWND hwnd);

#ifdef __cplusplus
}
#endif

#endif /* MRACER_FRAMEBUFFER_H */
