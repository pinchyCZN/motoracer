/*
 * horizon.c - the scrolling 2D sky panorama. See horizon.h.
 */

#include <windows.h>

#include "horizon.h"
#include "bank.h"
#include "framebuffer.h"
#include "transform.h"
#include "log.h"
#include "trig.h"

#include <math.h>
#include <stdlib.h>

/*
 * TWO 256x128 STRIPS STACKED IN ONE 256x256 PAGE, so the panorama
 * is 512 wide and 128 tall and strip 1 is the left half of a period.
 * The rects are FUN_00480930's - dd-statics.txt 7.2.
 */
#define HZ_X0     0
#define HZ_Y0     0
#define HZ_X1     255
#define HZ_Y1     127
#define HZ_2_Y0   128

#define HZ_W      (HZ_X1 - HZ_X0 + 1)     /* 256, one strip           */
#define HZ_H      (HZ_Y1 - HZ_Y0 + 1)     /* 128                      */
#define HZ_PERIOD (2 * HZ_W)              /* 512, one full panorama   */

/* The scalars are read, not guessed - dd-statics.txt 7.2.1. */
#define HZ_NEG_WRAP  314.1592653589793    /* 0x004d00d8, = 100*pi     */
#define HZ_DIST      50000.0f             /* 0x004d00f0               */

static int s_slot = -1;
static int s_loaded;

/* ------------------------------------------------------------------ */

/* ORIGIN  maybe_Track_SetHorizonBanks @ 0x00410350, the HiRes arm */
int horizon_load(const MR_Track *t, int slot)
{
	char name[MAX_PATH];

	s_loaded = 0;
	s_slot   = -1;

	if (!track_asset_name(t, "BANK\\HORIZHI.RAW", name, (int)sizeof(name)))
		return 0;

	if (!bank_load(slot, name, 1)) {
		mrlog_err("horizon: %s did not load\n", name);
		return 0;
	}

	s_slot   = slot;
	s_loaded = 1;
	mrlog("horizon: %s -> bank %d\n", name, slot);
	return 1;
}

/* ORIGIN  NONE - ours */
void horizon_free(void)
{
	s_loaded = 0;
	s_slot   = -1;
}

/* ------------------------------------------------------------------ */

/*
 * ORIGIN  NONE - ours. The original draws the strip as a sprite.
 *
 * Opaque: the horizon is a backdrop, so no colour key - the original
 * uses mode 1 and no key either.
 */
static void hz_blit(const MR_Bank *b, int srcY0, int dstX, int dstY)
{
	int x0, x1, y;

	x0 = (dstX < 0) ? -dstX : 0;
	x1 = HZ_W;
	if (dstX + x1 > g_clipWidth)
		x1 = g_clipWidth - dstX;
	if (x1 <= x0)
		return;

	for (y = 0; y < HZ_H; y++) {
		const unsigned short *src;
		unsigned short       *dst;
		int                   dy = dstY + y;
		int                   x;

		if (dy < 0 || dy >= g_clipHeight)
			continue;

		src = b->page + (srcY0 + y) * BANK_PAGE_W + HZ_X0;
		dst = g_frameBuffer + dy * g_frameStridePx + dstX;
		for (x = x0; x < x1; x++)
			dst[x] = src[x];
	}
}

/* ORIGIN  NONE - ours. The original queues a screen quad instead. */
/* Rows [y0, y1) across the full clip width. */
static void hz_fill(int y0, int y1, unsigned short c)
{
	int y, x;

	if (y0 < 0)
		y0 = 0;
	if (y1 > g_clipHeight)
		y1 = g_clipHeight;

	for (y = y0; y < y1; y++) {
		unsigned short *dst = g_frameBuffer + y * g_frameStridePx;
		for (x = 0; x < g_clipWidth; x++)
			dst[x] = c;
	}
}

/* ------------------------------------------------------------------ */

/* ORIGIN  Dd_DrawHorizon @ 0x00402960 */
void horizon_draw(const float eye[3], const float viewEye[3],
                  const float target[3], float lens,
                  int yBias, float damp)
{
	MR_Bank        *b;
	float           d[3];
	float           len, yaw, pitch, y, kS, r, yp, zp, proj;
	int             viewMaxX, s, x, yBand, parity;
	int             firstY0, secondY0;
	unsigned short  sky, ground;

	if (!s_loaded || !g_frameBuffer)
		return;

	b = bank_get(s_slot);
	if (!b || !b->page || !b->loaded)
		return;

	/*
	 * viewEye, NOT eye. The yaw and pitch come off the view matrix,
	 * which Camera_LookAt builds from the PRE-boom eye; only the
	 * HEIGHT below comes from the post-boom one. Using eye here lifts
	 * the band by whatever the boom pitches.
	 */
	d[0] = target[0] - viewEye[0];
	d[1] = target[1] - viewEye[1];
	d[2] = target[2] - viewEye[2];
	len  = (float)sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
	if (len == 0.0f)
		return;
	d[0] /= len;
	d[1] /= len;
	d[2] /= len;

	/*
	 * atan2 OF THE VIEW DIRECTION, REFLECTED. The binary takes
	 * atan2(m[8], m[6]) off the camera record at 0x00402990, and the
	 * record stores its basis in COLUMNS - so that is
	 * atan2(fwd.z, right.z) = atan2(d[2], -d[0]), pi minus our
	 * heading.
	 *
	 * READING THOSE AS ROWS ALSO TYPECHECKS and is wrong: row 1 comes
	 * back near world up either way, so it looks settled, and the sky
	 * then scrolls the WRONG WAY round a corner. dd-statics.txt 7.2.4
	 * has the test that separates them.
	 */
	yaw = (float)atan2(d[2], -d[0]);
	while (yaw < 0.0f)
		yaw += (float)HZ_NEG_WRAP;
	y = (float)fmod(yaw, MR_TWO_PI);

	/*
	 * NEGATED ON PURPOSE - this is the GAME'S pitch, not ours. The
	 * view matrix's third row is (d.x, -d.y, d.z), so a camera
	 * pitched down gives the matrix the opposite sign to d
	 * [verified, dd-statics.txt 7.2.3]. Negating here keeps
	 * everything below the binary's formula verbatim, signs and all,
	 * rather than ours with compensating flips through it.
	 */
	pitch = -d[1];
	if (pitch >  1.0f) pitch =  1.0f;
	if (pitch < -1.0f) pitch = -1.0f;
	pitch = (float)asin(pitch);

	/*
	 * ONE 512-PX PERIOD SPANS 2pi/s OF HEADING, so the sky's
	 * horizontal field is 90 degrees against the 3D pass's 78. That
	 * mismatch is the binary's, not ours.
	 *
	 * s COMES FROM THE DESIGN WIDTH: it is a mode selector and a
	 * widescreen frame re-phases it - dd-statics.txt 7.2.6.
	 */
	viewMaxX = g_clipWidth - 1;
	s = ((FB_DESIGN_W - 1) * 6) / 640;
	if (s < 1)
		s = 1;
	kS = (float)MR_TWO_PI / (float)s;

	/* y mod kS, by the binary's truncate-and-subtract. y >= 0 here,
	 * so a C cast and the original's __ftol agree. */
	r = y - (float)((int)(y / kS)) * kS;
	x = -(HZ_PERIOD + (int)((r / kS) * (float)HZ_PERIOD));

	/*
	 * A point HZ_DIST out along the pitched view direction, LIFTED TO
	 * THE EYE - the true horizon - projected through the live lens and
	 * damped. eye[1] is the binary's camera+0x9c. Verified to the
	 * pixel in dd-statics.txt 7.2.3.
	 */
	yp   = eye[1] * (float)cos(pitch) - HZ_DIST * (float)sin(pitch);
	zp   = eye[1] * (float)sin(pitch) + HZ_DIST * (float)cos(pitch);
	proj = (zp != 0.0f) ? (lens / zp) * yp * damp : 0.0f;

	/*
	 *     y = trunc(proj) - h - yBias + topMargin + centreRow
	 *
	 * Screen3D_GetViewportRows (0x0049a010) hands back the margin and
	 * centre row; at 640x480 unlettered those are 0 and 240, so the
	 * margin contributes nothing. yBias does NOT cancel.
	 */
	yBand = (int)proj - HZ_H - yBias + (g_clipHeight / 2);

	/*
	 * WRAP THE OFFSET BACK ONTO THE SCREEN, COUNTING STRIPS: the
	 * parity decides which strip goes down first, or the seam jumps
	 * every time the heading passes a strip width. Even parity draws
	 * strip 2 first [verified, 0x00402d7a]. dd-statics.txt 7.2.
	 */
	parity = 0;
	while (x + HZ_W < 0) {
		x += HZ_W;
		parity ^= 1;
	}
	/*
	 * AND IT FOLLOWS THE VIEW CENTRE. The phase above anchors the
	 * panorama to x = 0 while the world projects about
	 * g_viewCentreX, and the two agree only while the frame is the
	 * design width. Zero at 4:3 - dd-statics.txt 7.2.7.
	 */
	x += g_viewCentreX - FB_DESIGN_W / 2;
	while (x > 0) {
		x -= HZ_W;
		parity ^= 1;
	}

	firstY0  = parity ? HZ_Y0   : HZ_2_Y0;
	secondY0 = parity ? HZ_2_Y0 : HZ_Y0;

	/*
	 * The strip's own corner texels, inset by one, are the ONLY sky
	 * and ground colour stored anywhere [verified, 0x00402ec9 and
	 * 0x00403054]. dd-statics.txt 7.2.
	 */
	sky    = b->page[(HZ_Y0 + 1) * BANK_PAGE_W + (HZ_X0 + 1)];
	ground = b->page[(HZ_Y1 - 1) * BANK_PAGE_W + (HZ_X1 - 1)];

	hz_fill(0, yBand, sky);
	hz_fill(yBand + HZ_H, g_clipHeight, ground);

	while (x <= viewMaxX) {
		hz_blit(b, firstY0, x, yBand);
		x += HZ_W;
		hz_blit(b, secondY0, x, yBand);
		x += HZ_W;
	}
}
