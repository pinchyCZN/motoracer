/*
 * sprite.h - MR_Sprite, the 2D drawable the menu is built out of.
 *
 * The struct is laid out to match the original exactly: 0x74 bytes,
 * every field at the offset docs/structs/MR_Sprite.txt records. That
 * is deliberate even though nothing yet depends on it - the sizes are
 * load-bearing elsewhere in the engine, and keeping them means a field
 * found later lands where the analysis says it does.
 *
 * The blitter is the important part. docs/structs/MR_Sprite.txt
 * section 5: it writes u16 pixels straight into g_frameBuffer and
 * never enters the rasterizer, so the whole 2D menu layer is
 * independent of the renderer rewrite.
 */

#ifndef MRACER_SPRITE_H
#define MRACER_SPRITE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Sprite kinds. 0..3 double as the default draw mode when Sprite_Draw
 * is passed SPRITE_MODE_USE_KIND; 4 is the wave variant, which has its
 * own update and its own draw and is not implemented here. */
#define SPRITE_KIND_WAVE 4

/* The kind-4 tile: 128x128 in the file, doubled to 256 wide on load. */
#define SPRITE_WAVE_TILE_W  128
#define SPRITE_WAVE_TILE_H  128
#define SPRITE_WAVE_ROW_PX  256

/* Draw modes. See sprite_draw. */
#define SPRITE_MODE_OPAQUE        0
#define SPRITE_MODE_KEYED         1
#define SPRITE_MODE_STIPPLE       2
#define SPRITE_MODE_STIPPLE_KEYED 3
#define SPRITE_MODE_USE_KIND      5

/* Anchors for sprite_set_position. */
#define SPRITE_ANCHOR_NONE     0
#define SPRITE_ANCHOR_CENTRE_X 1
#define SPRITE_ANCHOR_ON_POINT 2

/* The colour-keyed modes treat this exact value as transparent. */
#define SPRITE_COLOR_KEY 0

typedef struct {
	int             x;              /* 0x00 */
	int             y;              /* 0x04 */
	int             width;          /* 0x08 */
	int             height;         /* 0x0c */
	int             moveActive;     /* 0x10 */
	int             kind;           /* 0x14 */
	unsigned int    waveOffsetX;    /* 0x18 */
	unsigned int    waveOffsetY;    /* 0x1c */
	int             waveOutA;       /* 0x20 */
	int             waveOutB;       /* 0x24 */
	unsigned int    waveStartMs;    /* 0x28 */
	unsigned short *bitmap;         /* 0x2c  decoded RGB565 */
	void           *lezImage;       /* 0x30  compressed source */
	unsigned int    lezFileSize;    /* 0x34 */
	int             waveArgA;       /* 0x38 */
	int             bitmapAlign;    /* 0x3c */
	int             waveArgB;       /* 0x40 */
	int             originX;        /* 0x44 */
	int             originY;        /* 0x48 */
	int             deltaX;         /* 0x4c */
	int             deltaY;         /* 0x50 */
	unsigned int    startMs;        /* 0x54 */
	int             durationMs;     /* 0x58 */
	unsigned int    endMs;          /* 0x5c */
	int             easingMode;     /* 0x60 */
	unsigned int    wavePhase0;     /* 0x64 */
	unsigned int    wavePhase1;     /* 0x68 */
	unsigned int    wavePhase2;     /* 0x6c */
	unsigned int    wavePhase3;     /* 0x70 */
} MR_Sprite;

/* Fails the build if the layout ever drifts from the original. */
typedef char mr_sprite_is_0x74[(sizeof(MR_Sprite) == 0x74) ? 1 : -1];

/*
 * sprite_load - fill a sprite from a file, choosing the loader by
 * extension exactly as Sprite_LoadFromFile does: .LEZ goes to the LEZ
 * decoder, anything else to the Targa loader.
 *
 * Returns 1 on success. The original treats a missing file as fatal
 * and exits; we report and return 0.
 */
int sprite_load(MR_Sprite *s, const char *path, int kind);

/* Release the decoded pixels and zero the sprite. */
void sprite_free(MR_Sprite *s);

/*
 * sprite_set_position - place the sprite, applying an anchor.
 *
 * pos points at two ints, x then y, and IS MODIFIED IN PLACE by the
 * anchoring - which the original does too, and which callers rely on.
 */
void sprite_set_position(MR_Sprite *s, int *pos, int anchor);

/*
 * sprite_start_move - begin a timed move to target.
 *
 * durationSec IS IN SECONDS, not milliseconds. The original takes a
 * float and multiplies by 1000.0 before converting; its callers pass
 * 0.7f and 1.0f. Getting this wrong makes every menu animation a
 * thousand times too fast rather than obviously broken, so it is
 * spelled out in the name.
 *
 * target points at two ints and IS MODIFIED IN PLACE by the anchor,
 * exactly as in sprite_set_position.
 *
 *   easingMode 0  linear
 *   easingMode 1  sine ease-in-ease-out
 *   anything else leaves the sprite where it is
 */
void sprite_start_move(MR_Sprite *s, int *target, float durationSec,
                       int easingMode, int anchorMode);

/*
 * sprite_update_move - advance a running move. Call once per frame.
 *
 * Clears moveActive on the frame the clock reaches endMs, and lands
 * exactly on the target because the clock is clamped rather than the
 * interpolant.
 *
 * The original takes four more arguments that exist only for the
 * kind-4 wave route, which is not implemented here.
 */
void sprite_update_move(MR_Sprite *s);

/*
 * THE KIND-4 ROUTE, which is a different sprite entirely.
 *
 * Sprite_UpdateMove and Sprite_Draw both branch on kind == 4 into
 * maybe_Sprite_UpdateWave (0x0043f840) and maybe_Sprite_DrawWave
 * (0x0043f0f0). Only four resources are kind 4 - paralax1, paralax2,
 * paralax3 and one more - and they are the scrolling menu
 * backgrounds.
 *
 * THE TILE IS 256 WIDE BY 128 TALL, decoded from a 128x128 .lez with
 * every row written TWICE side by side. That doubling is the whole
 * trick: a horizontal scroll of 0..127 can then read 128 contiguous
 * pixels from any offset without a wrap test in the inner loop.
 * lez.txt section 4 records the 0x10000-byte buffer; 0x10000 / 0x200
 * bytes per row is the 128 rows.
 *
 * sprite_update_wave adds dx and dy to the scroll offsets and masks
 * both to 0..127. Page 15 passes (0, 1), so its background creeps one
 * pixel a frame and wraps every 128.
 *
 * o THE FOUR COSINE PHASES ARE NOT USED BY THE DRAW. UpdateWave
 *   advances wavePhase0..3, masks them to 0x7ff and derives waveOutA
 *   and waveOutB - and the draw reads NEITHER. So the background is a
 *   plain scroll with no wobble, and the wave fields are vestigial on
 *   this path. Worth knowing before hunting for a distortion that is
 *   not there.
 */
void sprite_update_wave(MR_Sprite *s, int dx, int dy);

/*
 * Draw a kind-4 background. It covers the whole frame opaquely - five
 * 128-pixel copies across and 480 rows down, straight into the
 * framebuffer - so it neither reads nor needs a position, a mode or a
 * reveal.
 */
void sprite_draw_wave(MR_Sprite *s);

/*
 * sprite_draw - blit into g_frameBuffer, clipped to g_clipWidth and
 * g_clipHeight.
 *
 *   mode 0  opaque copy
 *   mode 1  colour-keyed: pixels equal to SPRITE_COLOR_KEY are skipped
 *   mode 2  stippled opaque: every other pixel, the start column
 *           alternating per row, giving 50% screen door
 *   mode 3  stippled and keyed
 *   mode 5  use the sprite's own kind as the mode
 *
 * reveal and shrink are percentages, clamped to 0..100. reveal skips
 * the top ((100 - reveal) * height) / 100 rows, so it wipes upward;
 * shrink removes (shrink * width) / 100 from the right.
 */
void sprite_draw(MR_Sprite *s, int mode, int reveal, int shrink);

/*
 * sprite_draw_scaled - the same blit at a percentage of its size.
 *
 * NOT shrink ABOVE, WHICH IS A CROP: that removes columns from the
 * right and leaves the rest at native size, which is what makes it
 * a bar-length control. This RESAMPLES, nearest neighbour.
 *
 * percent is clamped to 1..100 - it only ever shrinks, because no
 * caller wants a magnified 8-bit-era bitmap. OURS; the original
 * has no scaled 2D path at all. 
 */
void sprite_draw_scaled(MR_Sprite *s, int mode, int percent);

#ifdef __cplusplus
}
#endif

#endif /* MRACER_SPRITE_H */
