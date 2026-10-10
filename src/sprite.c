/*
 * sprite.c - MR_Sprite: loading and the four blit modes.
 *
 * Reconstructed from Sprite_LoadFromFile (0x0043e7f0), Sprite_Draw
 * (0x0043ede0) and Sprite_SetPosition (0x0043fa40). The write-up is
 * docs/structs/MR_Sprite.txt; section 5 covers the blitter.
 */

#include <windows.h>
#include <mmsystem.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sprite.h"
#include "framebuffer.h"
#include "trig.h"
#include "lez.h"
#include "tga.h"
#include "log.h"

/* 1 if path ends in ".lez", case-insensitively. */
static int has_lez_extension(const char *path)
{
	const char *dot;

	if (!path)
		return 0;
	dot = strrchr(path, '.');
	if (!dot)
		return 0;
	return _stricmp(dot, ".lez") == 0;
}

int sprite_load(MR_Sprite *s, const char *path, int kind)
{
	LEZ_IMAGE img;

	if (!s || !path)
		return 0;
	memset(s, 0, sizeof(*s));

	if (has_lez_extension(path)) {
		if (!lez_load(path, &img))
			return 0;
		s->width  = img.width;
		s->height = img.height;
		s->bitmap = img.pixels;      /* ownership moves to the sprite */
	} else {
		int w;
		int h;
		unsigned short *px;
		if (!tga_load(path, &w, &h, &px))
			return 0;
		s->width  = w;
		s->height = h;
		s->bitmap = px;
	}

	s->kind = kind;

	/*
	 * KIND 4 IS DOUBLED HORIZONTALLY. The original does this inside
	 * Lez_Decode's kind-4 branch, which allocates a flat 0x10000
	 * bytes and writes each of the 128 source rows twice; the result
	 * is a 256x128 tile. Doing it here instead produces the identical
	 * buffer and keeps the .lez decoder about decoding - the
	 * difference is where the work sits, not what comes out.
	 */
	if (kind == SPRITE_KIND_WAVE && s->bitmap) {
		int             w = s->width;
		int             h = s->height;
		unsigned short *dbl;
		int             y;

		if (w != SPRITE_WAVE_TILE_W || h != SPRITE_WAVE_TILE_H) {
			mrlog_err("sprite: kind 4 %s is %dx%d, expected %dx%d\n",
			          path, w, h, SPRITE_WAVE_TILE_W,
			          SPRITE_WAVE_TILE_H);
			return 1;               /* usable, just not scrollable */
		}
		dbl = (unsigned short *)malloc((size_t)SPRITE_WAVE_ROW_PX *
		                               (size_t)h * sizeof(*dbl));
		if (!dbl)
			return 1;
		for (y = 0; y < h; y++) {
			unsigned short *d = dbl + (size_t)y * SPRITE_WAVE_ROW_PX;
			unsigned short *r = s->bitmap + (size_t)y * w;

			memcpy(d,     r, (size_t)w * sizeof(*d));
			memcpy(d + w, r, (size_t)w * sizeof(*d));
		}
		free(s->bitmap);
		s->bitmap = dbl;
		s->width  = SPRITE_WAVE_ROW_PX;
	}

	/*
	 * The original records the allocation alignment it used here, 16,
	 * because it hand-aligns the decoded buffer and has to recover the
	 * original pointer to free it. We use plain malloc, so the value
	 * is carried for layout fidelity and nothing reads it.
	 */
	s->bitmapAlign = 0x10;

	/*
	 * DIVERGENCE: the original defers decoding. Sprite_LoadFromFile
	 * stores the compressed file in lezImage and leaves bitmap null;
	 * Sprite_Draw calls Lez_Decode on first draw. We decode eagerly,
	 * so lezImage stays null. Nothing observable depends on which,
	 * beyond when the work happens.
	 */
	s->lezImage = NULL;
	return 1;
}

void sprite_free(MR_Sprite *s)
{
	if (!s)
		return;
	if (s->bitmap)
		free(s->bitmap);
	memset(s, 0, sizeof(*s));
}

void sprite_set_position(MR_Sprite *s, int *pos, int anchor)
{
	if (!s || !pos)
		return;

	if (anchor == SPRITE_ANCHOR_CENTRE_X) {
		/*
		 * A MARKED DEVIATION, and the only one in this file.
		 *
		 * The original centres against a LITERAL: Sprite_SetPosition
		 * at 0x0043fa40 is
		 *
		 *     *pos = 0x280 - this->width >> 1;
		 *
		 * with 0x280 = 640 as an immediate, not a global [verified].
		 * It can afford that because it owns the display mode and
		 * every mode it offers is 640 wide or narrower with the same
		 * 4:3 shape.
		 *
		 * FB_DESIGN_W IS THAT LITERAL, and using it is not a
		 * divergence - the constant simply has a name now. A sprite
		 * centred here is centred within the block of authored art,
		 * and sprite_draw then puts the whole block wherever
		 * g_fb2dOriginX says. Centring on g_frameWidth instead would
		 * centre it twice and land it 107 pixels right of centre in
		 * a 16:9 frame.
		 */
		pos[0] = (FB_DESIGN_W - s->width) >> 1;
	} else if (anchor == SPRITE_ANCHOR_ON_POINT) {
		pos[0] -= s->width >> 1;
		pos[1] -= s->height >> 1;
	}
	s->x = pos[0];
	s->y = pos[1];
}

/*
 * The four constants the easing is built from, read out of the
 * binary at 0x004d07a8, 0x004d07d8, 0x004d07e0 and 0x004d07e8. They
 * are pi, pi/2, 1.0 and 0.5, which together remap sin over a half
 * period into a 0..1 ease. Named rather than inlined because the
 * shape of the curve is the thing worth being able to see.
 */

/* 0x004d07d0: seconds to milliseconds. */
#define SPRITE_SECONDS_TO_MS 1000.0

void sprite_start_move(MR_Sprite *s, int *target, float durationSec,
                       int easingMode, int anchorMode)
{
	if (!s || !target)
		return;

	s->startMs    = timeGetTime();
	s->durationMs = (int)((double)durationSec * SPRITE_SECONDS_TO_MS);
	s->endMs      = s->startMs + (unsigned int)s->durationMs;

	/* The anchor is applied to the TARGET, before the delta is taken,
	 * and it writes back through the caller's pointer. */
	if (anchorMode == SPRITE_ANCHOR_CENTRE_X) {
		/* The same constant as sprite_set_position, for the same
		 * reason - see the note there. The original hardcodes 0x280
		 * at this site too. */
		target[0] = (FB_DESIGN_W - s->width) >> 1;
	} else if (anchorMode == SPRITE_ANCHOR_ON_POINT) {
		target[1] -= s->height >> 1;
		target[0] -= s->width >> 1;
	}

	s->deltaX     = target[0] - s->x;
	s->deltaY     = target[1] - s->y;
	s->originX    = s->x;
	s->originY    = s->y;
	s->easingMode = easingMode;
	s->moveActive = 1;
}

void sprite_update_move(MR_Sprite *s)
{
	unsigned int now;
	unsigned int end;
	double t;
	double e;

	if (!s)
		return;
	if (s->kind == SPRITE_KIND_WAVE)
		return;                      /* the wave route, not implemented */
	if (!s->moveActive)
		return;

	now = timeGetTime();
	end = s->endMs;

	/*
	 * THE CLOCK IS CLAMPED, NOT THE INTERPOLANT, and the finish is
	 * detected by comparing the clamped clock to endMs rather than by
	 * testing t against 1.0. That is why the last frame lands exactly
	 * on the target with no floating-point residue.
	 */
	if (now > end)
		now = end;

	/* DIVERGENCE: the original divides unguarded, so a zero duration
	 * gives 0/0 and a NaN position. Treat it as already finished. */
	if (s->durationMs <= 0) {
		t = 1.0;
	} else {
		t = (double)(now - s->startMs) / (double)s->durationMs;
	}

	if (s->easingMode == 0) {
		e = t;
	} else if (s->easingMode == 1) {
		e = (sin(t * MR_PI - MR_HALF_PI) + 1.0) * 0.5;
	} else {
		return;                      /* unknown mode: do not move */
	}

	s->x = (int)((double)s->originX + (double)s->deltaX * e);
	s->y = (int)((double)s->originY + (double)s->deltaY * e);

	if (now == end)
		s->moveActive = 0;
}

void sprite_update_wave(MR_Sprite *s, int dx, int dy)
{
	if (!s || s->kind != SPRITE_KIND_WAVE)
		return;
	if (s->waveStartMs == 0)
		s->waveStartMs = timeGetTime();

	/*
	 * MASKED, NOT WRAPPED BY COMPARISON, which is what makes the
	 * scroll free: the offsets simply live in 0..127 for ever.
	 */
	s->waveOffsetX = (s->waveOffsetX + (unsigned int)dx) & 0x7f;
	s->waveOffsetY = (s->waveOffsetY + (unsigned int)dy) & 0x7f;
}

void sprite_draw_wave(MR_Sprite *s)
{
	int dy;
	int srcY;

	if (!s || s->kind != SPRITE_KIND_WAVE || !s->bitmap)
		return;
	if (s->width != SPRITE_WAVE_ROW_PX)
		return;                     /* not doubled; sprite_load said so */

	srcY = (int)(s->waveOffsetY & 0x7f);

	for (dy = 0; dy < g_frameHeight; dy++) {
		const unsigned short *src = s->bitmap +
		                            (size_t)((srcY + dy) & 0x7f) *
		                            SPRITE_WAVE_ROW_PX +
		                            (s->waveOffsetX & 0x7f);
		unsigned short *dst = g_frameBuffer +
		                      (size_t)dy * g_frameStridePx;
		int             x;

		if (!g_frameBuffer)
			break;
		/*
		 * FIVE COPIES OF 128 PIXELS, at x = 0, 128, 256, 384, 512.
		 * That is 640 exactly, which is why the original hardcodes
		 * five and does not loop to the frame width. Ours stops at
		 * the frame width so a wider frame does not run off the end.
		 */
		for (x = 0; x < g_frameWidth; x += SPRITE_WAVE_TILE_W) {
			int n = g_frameWidth - x;

			if (n > SPRITE_WAVE_TILE_W)
				n = SPRITE_WAVE_TILE_W;
			memcpy(dst + x, src, (size_t)n * sizeof(*dst));
		}
	}
}

void sprite_draw_scaled(MR_Sprite *s, int mode, int percent)
{
	const unsigned short *src;
	unsigned short       *dst;
	int rows;
	int cols;
	int sx0 = 0;
	int sy0 = 0;
	int dx;
	int dy;
	int x;
	int y;

	if (!s || !s->bitmap || !g_frameBuffer)
		return;
	if (s->kind == SPRITE_KIND_WAVE)
		return;
	if (mode == SPRITE_MODE_USE_KIND)
		mode = s->kind;
	if (mode < 0 || mode > SPRITE_MODE_STIPPLE_KEYED)
		return;
	if (percent < 1)
		percent = 1;
	if (percent > 100)
		percent = 100;

	cols = (s->width  * percent) / 100;
	rows = (s->height * percent) / 100;

	/* Clipped in DESTINATION space, as sprite_draw is, with the
	   source offset carried separately - the step is not 1:1. */
	dx = s->x + g_fb2dOriginX;
	dy = s->y;
	if (dx < 0) {
		cols += dx;
		sx0 = -dx;
		dx = 0;
	}
	if (dy < 0) {
		rows += dy;
		sy0 = -dy;
		dy = 0;
	}
	if (dx + cols > g_clipWidth)
		cols = g_clipWidth - dx;
	if (dy + rows > g_clipHeight)
		rows = g_clipHeight - dy;
	if (cols <= 0 || rows <= 0)
		return;

	dst = g_frameBuffer + (size_t)dy * g_frameStridePx + dx;

	for (y = 0; y < rows; y++) {
		/*
		 * THE SOURCE ROW IS RECOMPUTED, not stepped. An
		 * accumulator would carry its rounding error down the
		 * whole sprite; this cannot drift. The clamp is belt and
		 * braces - the floor in rows above already keeps the
		 * index inside the bitmap.
		 */
		int sy = ((y + sy0) * 100) / percent;

		if (sy >= s->height)
			sy = s->height - 1;
		src = s->bitmap + (size_t)sy * s->width;

		switch (mode) {
		case SPRITE_MODE_OPAQUE:
			for (x = 0; x < cols; x++)
				dst[x] = src[((x + sx0) * 100) / percent];
			break;

		case SPRITE_MODE_KEYED:
			for (x = 0; x < cols; x++) {
				unsigned short p = src[((x + sx0) * 100) / percent];

				if (p != SPRITE_COLOR_KEY)
					dst[x] = p;
			}
			break;

		case SPRITE_MODE_STIPPLE:
			for (x = y & 1; x < cols; x += 2)
				dst[x] = src[((x + sx0) * 100) / percent];
			break;

		case SPRITE_MODE_STIPPLE_KEYED:
			for (x = y & 1; x < cols; x += 2) {
				unsigned short p = src[((x + sx0) * 100) / percent];

				if (p != SPRITE_COLOR_KEY)
					dst[x] = p;
			}
			break;
		}
		dst += g_frameStridePx;
	}
}


void sprite_draw(MR_Sprite *s, int mode, int reveal, int shrink)
{
	const unsigned short *src;
	unsigned short *dst;
	int skip_rows;
	int rows;
	int cols;
	int dx;
	int dy;
	int x;
	int y;

	if (!s || !s->bitmap || !g_frameBuffer)
		return;

	if (s->kind == SPRITE_KIND_WAVE) {
		/* The wave variant has its own draw, maybe_Sprite_DrawWave at
		 * 0x0043f0f0, which is read but not reimplemented. */
		return;
	}

	if (mode == SPRITE_MODE_USE_KIND)
		mode = s->kind;
	if (mode < 0 || mode > SPRITE_MODE_STIPPLE_KEYED)
		return;

	if (reveal < 0)   reveal = 0;
	if (reveal > 100) reveal = 100;
	if (shrink < 0)   shrink = 0;
	if (shrink > 100) shrink = 100;

	/* reveal wipes upward: skip rows off the TOP. */
	skip_rows = ((100 - reveal) * s->height) / 100;
	rows      = s->height - skip_rows;
	cols      = s->width - (shrink * s->width) / 100;

	/*
	 * THE 2D ORIGIN GOES ON HERE, once, before the clip - so a
	 * sprite that hangs off a widened frame is still clipped
	 * against the real buffer.
	 *
	 * s->x stays in AUTHORED space, which is what menu_hit_test
	 * and sprite_hit compare the cursor against. Offsetting the
	 * stored position instead would put the art and the hit
	 * rectangle in the same place and both of them in the wrong
	 * one relative to the cursor.
	 */
	dx = s->x + g_fb2dOriginX;
	dy = s->y + skip_rows;
	src = s->bitmap + (size_t)skip_rows * s->width;

	/*
	 * DIVERGENCE, and a small one. The original folds the horizontal
	 * clip into a single expression that is wrong when a sprite is
	 * BOTH off the left edge and wider than the clip rect:
	 *
	 *     if (clipW < width + x)
	 *         cols = (cols - x) + (clipW - width);
	 *
	 * with x already clamped to 0, which subtracts the off-screen
	 * amount a second time. No menu sprite is wider than the screen,
	 * so it cannot fire on shipped art. Clipped correctly here; noted
	 * so a faithful transcription pass can decide differently.
	 */
	if (dx < 0) {
		cols += dx;
		src  += -dx;
		dx = 0;
	}
	if (dy < 0) {
		rows += dy;
		src  += (size_t)(-dy) * s->width;
		dy = 0;
	}
	if (dx + cols > g_clipWidth)
		cols = g_clipWidth - dx;
	if (dy + rows > g_clipHeight)
		rows = g_clipHeight - dy;

	if (cols <= 0 || rows <= 0)
		return;

	dst = g_frameBuffer + (size_t)dy * g_frameStridePx + dx;

	switch (mode) {
	case SPRITE_MODE_OPAQUE:
		for (y = 0; y < rows; y++) {
			memcpy(dst, src, (size_t)cols * sizeof(unsigned short));
			src += s->width;
			dst += g_frameStridePx;
		}
		break;

	case SPRITE_MODE_KEYED:
		for (y = 0; y < rows; y++) {
			for (x = 0; x < cols; x++) {
				if (src[x] != SPRITE_COLOR_KEY)
					dst[x] = src[x];
			}
			src += s->width;
			dst += g_frameStridePx;
		}
		break;

	/*
	 * The stippled modes draw every other pixel, with the start column
	 * alternating each row - a 50% screen door, not a blend. The
	 * original alternates by adding and then negating a +/-1 applied
	 * to BOTH the source and destination row steps, which comes to the
	 * same thing as keying the start column off the row parity.
	 *
	 * Both are read-free: they never sample the destination, so the
	 * effect is a dither pattern rather than real transparency.
	 */
	case SPRITE_MODE_STIPPLE:
		for (y = 0; y < rows; y++) {
			for (x = y & 1; x < cols; x += 2)
				dst[x] = src[x];
			src += s->width;
			dst += g_frameStridePx;
		}
		break;

	case SPRITE_MODE_STIPPLE_KEYED:
		for (y = 0; y < rows; y++) {
			for (x = y & 1; x < cols; x += 2) {
				if (src[x] != SPRITE_COLOR_KEY)
					dst[x] = src[x];
			}
			src += s->width;
			dst += g_frameStridePx;
		}
		break;

	default:
		break;
	}
}
