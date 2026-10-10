/*
 * framebuffer.c - the 16 bpp back buffer and its present.
 *
 * See framebuffer.h for what this replaces and why the format is
 * fixed at RGB565.
 */

#include <windows.h>
#include <stdlib.h>

#include "framebuffer.h"

unsigned short *g_frameBuffer   = NULL;
unsigned short *g_overlayBuffer  = NULL;
unsigned short *g_overlayBuffer2 = NULL;

/* What g_frameBuffer was while the overlay is redirected. */
static unsigned short *s_fbSaved;
int             g_frameStridePx = 0;
int             g_frameWidth    = 0;
int             g_frameHeight   = 0;
int             g_clipWidth     = 0;
int             g_clipHeight    = 0;
int             g_fb2dOriginX   = 0;

void fb_set_2d_origin(int x)
{
	g_fb2dOriginX = x;
}

/*
 * A BITMAPINFO whose colour table is the three BI_BITFIELDS masks.
 * bmiColors is declared as one RGBQUAD, so the masks need room the
 * struct does not have; this is the usual way to give it that room.
 */
typedef struct {
	BITMAPINFOHEADER hdr;
	DWORD            mask[3];
} FB_BITMAPINFO;

static FB_BITMAPINFO s_bmi;

/*
 * fb_create - allocate the back buffer.
 *
 * The stride is rounded up so each row is a whole number of DWORDs,
 * which is what GDI requires of a DIB. At 16 bpp that means an even
 * pixel count: 640 needs no padding, 641 gets one pixel of it.
 */
int fb_create(int w, int h)
{
	size_t bytes;

	fb_destroy();

	if (w <= 0 || h <= 0)
		return 0;

	g_frameWidth    = w;
	g_frameHeight   = h;
	g_frameStridePx = (w + 1) & ~1;
	g_clipWidth     = w;
	g_clipHeight    = h;
	g_fb2dOriginX   = (w > FB_DESIGN_W) ? (w - FB_DESIGN_W) / 2 : 0;

	bytes = (size_t)g_frameStridePx * (size_t)h * sizeof(unsigned short);
	g_frameBuffer = (unsigned short *)malloc(bytes);
	if (!g_frameBuffer) {
		g_frameWidth    = 0;
		g_frameHeight   = 0;
		g_frameStridePx = 0;
		g_clipWidth     = 0;
		g_clipHeight    = 0;
		g_fb2dOriginX   = 0;
		return 0;
	}
	memset(g_frameBuffer, 0, bytes);

	/*
	 * The overlay layer is the same shape, so every blitter can
	 * be pointed at it without knowing. A failure here is not
	 * fatal: fb_overlay_begin reports it and the caller skips
	 * the overlay pass.
	 */
	g_overlayBuffer  = (unsigned short *)malloc(bytes);
	g_overlayBuffer2 = (unsigned short *)malloc(bytes);

	ZeroMemory(&s_bmi, sizeof(s_bmi));
	s_bmi.hdr.biSize   = sizeof(BITMAPINFOHEADER);
	s_bmi.hdr.biWidth  = g_frameStridePx;   /* padding included */
	s_bmi.hdr.biHeight = -h;                /* negative: top-down */
	s_bmi.hdr.biPlanes = 1;
	s_bmi.hdr.biBitCount    = 16;
	s_bmi.hdr.biCompression = BI_BITFIELDS;
	s_bmi.mask[0] = 0xf800;                 /* red   */
	s_bmi.mask[1] = 0x07e0;                 /* green */
	s_bmi.mask[2] = 0x001f;                 /* blue  */

	return 1;
}

void fb_destroy(void)
{
	if (g_frameBuffer) {
		free(g_frameBuffer);
		g_frameBuffer = NULL;
	}
	if (g_overlayBuffer) {
		free(g_overlayBuffer);
		g_overlayBuffer = NULL;
	}
	if (g_overlayBuffer2) {
		free(g_overlayBuffer2);
		g_overlayBuffer2 = NULL;
	}
	s_fbSaved = NULL;
	g_frameWidth    = 0;
	g_frameHeight   = 0;
	g_frameStridePx = 0;
	g_clipWidth     = 0;
	g_clipHeight    = 0;
	g_fb2dOriginX   = 0;
}

int fb_overlay_begin(int pass)
{
	int             n = g_frameStridePx * g_frameHeight;
	int             i;
	unsigned short *p;
	unsigned short  fill;

	if (!g_overlayBuffer || !g_overlayBuffer2 || s_fbSaved)
		return 0;

	p    = pass ? g_overlayBuffer2 : g_overlayBuffer;
	fill = pass ? 0xffff : 0x0000;
	for (i = 0; i < n; i++)
		p[i] = fill;

	s_fbSaved     = g_frameBuffer;
	g_frameBuffer = p;
	return 1;
}

void fb_overlay_end(void)
{
	if (!s_fbSaved)
		return;
	g_frameBuffer = s_fbSaved;
	s_fbSaved     = NULL;
}

void fb_clear(unsigned short color)
{
	int    n;
	int    i;
	unsigned short *p;

	if (!g_frameBuffer)
		return;

	p = g_frameBuffer;
	n = g_frameStridePx * g_frameHeight;
	for (i = 0; i < n; i++)
		p[i] = color;
}

/*
 * fb_blit - scale the back buffer into a device context.
 *
 * The window is resizable, so the client area is rarely 4:3. The
 * image is scaled to the largest rect that fits with the aspect ratio
 * intact and centred; the margins are painted black.
 *
 * COLORONCOLOR is nearest-neighbour. HALFTONE would smooth the result,
 * which is wrong for art authored at a fixed resolution, and it also
 * needs SetBrushOrgEx to avoid a shifting dither pattern.
 */
void fb_blit(HDC hdc, int client_w, int client_h)
{
	int dst_w;
	int dst_h;
	int dst_x;
	int dst_y;
	RECT bar;

	if (!g_frameBuffer || !hdc)
		return;
	if (client_w <= 0 || client_h <= 0)
		return;

	/* Largest 4:3-preserving rect that fits the client area. */
	dst_w = client_w;
	dst_h = (int)((__int64)client_w * g_frameHeight / g_frameWidth);
	if (dst_h > client_h) {
		dst_h = client_h;
		dst_w = (int)((__int64)client_h * g_frameWidth / g_frameHeight);
	}
	if (dst_w <= 0 || dst_h <= 0)
		return;

	dst_x = (client_w - dst_w) / 2;
	dst_y = (client_h - dst_h) / 2;

	/* Letterbox margins. Only paint what is actually margin. */
	if (dst_y > 0) {
		bar.left = 0; bar.top = 0;
		bar.right = client_w; bar.bottom = dst_y;
		FillRect(hdc, &bar, (HBRUSH)GetStockObject(BLACK_BRUSH));
		bar.top = dst_y + dst_h; bar.bottom = client_h;
		FillRect(hdc, &bar, (HBRUSH)GetStockObject(BLACK_BRUSH));
	}
	if (dst_x > 0) {
		bar.left = 0; bar.top = 0;
		bar.right = dst_x; bar.bottom = client_h;
		FillRect(hdc, &bar, (HBRUSH)GetStockObject(BLACK_BRUSH));
		bar.left = dst_x + dst_w; bar.right = client_w;
		FillRect(hdc, &bar, (HBRUSH)GetStockObject(BLACK_BRUSH));
	}

	SetStretchBltMode(hdc, COLORONCOLOR);
	StretchDIBits(hdc,
	              dst_x, dst_y, dst_w, dst_h,
	              0, 0, g_frameWidth, g_frameHeight,
	              g_frameBuffer,
	              (const BITMAPINFO *)&s_bmi,
	              DIB_RGB_COLORS,
	              SRCCOPY);
}

void fb_present(HWND hwnd)
{
	HDC  hdc;
	RECT rc;

	if (!hwnd || !g_frameBuffer)
		return;
	if (!GetClientRect(hwnd, &rc))
		return;

	hdc = GetDC(hwnd);
	if (!hdc)
		return;
	fb_blit(hdc, rc.right - rc.left, rc.bottom - rc.top);
	ReleaseDC(hwnd, hdc);
}
