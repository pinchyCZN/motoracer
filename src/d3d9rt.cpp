/*
 * d3d9rt.cpp - the Direct3D 9 back end behind render.h.
 *
 * Replaces Dsi_d3d.cpp. Not a translation of it: the original talks to
 * Direct3D Immediate Mode with EXECUTE BUFFERS, an API that has not
 * shipped in a Windows SDK since the 1990s and has no mechanical
 * equivalent. What is reproduced is the SEMANTICS - the vertex format,
 * the three blend modes, the state order, the batching shape - all of
 * which are catalogued in docs/subsystems/d3d-execute-buffers.txt and
 * docs/subsystems/d3d-render-state.txt.
 *
 *
 * NO DIRECTX REDISTRIBUTABLE. THIS IS A CONSTRAINT, NOT AN ACCIDENT.
 *
 * A clean Windows 10 install must run this with nothing downloaded.
 * That rules out more than it first appears to:
 *
 *   o d3d9.h, d3d9caps.h and d3d9types.h come from the WINDOWS SDK -
 *     they are in Include\<ver>\shared. Nothing here includes a header
 *     from the June 2010 DirectX SDK.
 *
 *   o NO D3DX. d3dx9.h and its d3dx9_43.dll are the redistributable,
 *     and they are the usual way a D3D9 program accidentally acquires
 *     that dependency - D3DXMatrixPerspectiveFovLH, D3DXCreateTexture
 *     FromFile, ID3DXFont. None of them appear here. Textures are
 *     built from our own .RAW loader, there are no matrices at all
 *     because the vertices are pre-transformed, and there is no font.
 *
 *   o NO SHADERS, so no d3dcompiler_47.dll and no bytecode. The
 *     fixed-function pipeline does exactly what the original asks for:
 *     one texture stage set to MODULATE. That is not a compromise, it
 *     is a closer match than a shader would be.
 *
 *   o NO D3DDEVTYPE_REF FALLBACK. The reference rasterizer only exists
 *     where the SDK debug runtime is installed. Asking for it on a
 *     clean machine fails, so this refuses to create a device rather
 *     than pretending to.
 *
 *   o d3d9.dll is loaded BY NAME with LoadLibrary rather than linked,
 *     so the executable carries no import from it and a machine
 *     without it gets a printed message instead of a loader dialog.
 *     The original does the same thing for DirectDraw - "Couldn't
 *     LoadLibrary DDraw" is a string in the binary. Also means
 *     d3d9.lib is not in the link line.
 *
 * The one thing this cannot promise is a DRIVER. D3D9 on a clean Win10
 * runs on the Microsoft Basic Display Adapter through WARP-backed
 * software, but a HAL device is not guaranteed on every machine. When
 * CreateDevice fails, r_init returns 0 and the caller falls back to
 * the GDI present in framebuffer.c, which needs nothing at all.
 */

#include <windows.h>
#include <math.h>          /* floor, for the HSV sector */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <d3d9.h>

#include "render.h"
#include "framebuffer.h"
#include "bank.h"
#include "log.h"

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

/*
 * The vertex pool. The original's is bounded by the execute buffer's
 * size and it handles the overflow with maybe_D3D_BufferFull, which
 * flushes and continues; ours refuses to overrun instead. The number
 * is ours - the original's budget lives in the buffer allocation, not
 * in a constant we could copy.
 */
#define R_MAX_VERTICES 16384

/* Expanded triangle-list vertices held between flushes. */
#define R_MAX_BATCH    (R_MAX_VERTICES * 3)

/* The 2D layer is 640x480, which is not a power of two. Rather than
 * ask the caps whether this device wants POW2 textures, allocate the
 * next power of two and use a sub-rectangle. Costs 1 MB and removes a
 * conditional that could only ever be wrong on hardware we cannot
 * test. */
#define R_FB_TEX_W 1024
#define R_FB_TEX_H 512

#define R_FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | \
               D3DFVF_TEX1)

/*
 * THE CLIPPED PATH, and the reason there are two vertex formats.
 *
 * R_FVF is PRE-TRANSFORMED - the position is already in screen space,
 * so D3D9 does no transform and, crucially, NO FRUSTUM CLIPPING. It
 * rasterises in 2D against a finite guard band, and a triangle whose
 * off-screen corner lands outside that band can be dropped whole by
 * the driver. That showed as faces popping in and out at the edges of
 * the screen on page 25, on geometry that was otherwise correct.
 *
 * R_FVF3 is UNTRANSFORMED. Scene geometry is submitted in the camera
 * space the software pipeline already produces - camX, camY, camZ
 * straight out of MR_Vertex - and a projection matrix does the divide,
 * so D3D9 clips it against a real frustum.
 *
 * o THE SOFTWARE CULL, QUEUE AND NEAR CLIP ARE UNCHANGED. They are
 *   faithful to the original and they still run; this changes only how
 *   the survivors reach the device. The near-plane clip becomes
 *   belt-and-braces rather than load-bearing, which is what it is in
 *   the original too.
 *
 * o IT ALSO CLOSES A LATENT BUG. camX/camY/camZ are written for EVERY
 *   vertex, but screenX/screenY only when camZ passes the near plane
 *   (transform.c, and MR_Vertex.txt section 7.2 says so), so the
 *   pre-transformed path read whatever the previous user of that pool
 *   slot left behind on any face the software clip did not rebuild.
 *
 * Only two things draw: this batch, which is always scene geometry,
 * and the framebuffer blit, which is a screen-aligned quad and stays
 * pre-transformed. Each sets its own FVF at draw time rather than
 * relying on one set at init.
 */
#define R_FVF3 (D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | \
                D3DFVF_TEX1)

struct R_Vertex3 {
	float        x, y, z;   /* camera space, projScale already folded in */
	unsigned int color;
	unsigned int specular;
	float        tu, tv;
};

typedef IDirect3D9 * (WINAPI *PFN_Direct3DCreate9)(UINT);

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
	IDirect3DTexture9 *tex;
	int                valid;
} R_BankTex;

static HMODULE             s_d3dDll;
static IDirect3D9         *s_d3d;
static IDirect3DDevice9   *s_dev;
static IDirect3DTexture9  *s_fbTex;
/*
 * THE OVERLAY GETS ITS OWN TEXTURE, and that is a bug fix.
 *
 * Both layers used to share s_fbTex: r_draw_framebuffer uploads the
 * whole 2D layer into it, then the overlay composite uploads a small
 * sub-rect on top. The overlay quad's UVs stop at that sub-rect's
 * edge, and any sample there - a linear tap reaching a neighbouring
 * texel, or the driver rounding the dirty rect outward - picks up
 * the 2D layer's leftovers, which are OPAQUE. That drew a faint
 * rectangle around whatever the overlay composited.
 *
 * Point sampling removed three sides of it and left the bottom,
 * which is the tell that it is an UPLOAD boundary and not only a
 * filter tap. A separate texture, cleared to alpha 0 once at
 * creation, has nothing to bleed from at all.
 */
static IDirect3DTexture9  *s_overlayTex;
static D3DPRESENT_PARAMETERS s_pp;
static HWND                s_hwnd;
static int                 s_ready;
static int                 s_inScene;
static int                 s_lost;

/* Design space, and the letterbox that maps it onto the client area. */
static int   s_designW = 640;
static int   s_designH = 480;
static float s_viewScale = 1.0f;
static float s_viewOffX;
static float s_viewOffY;

static R_Vertex *s_pool;
static int       s_poolUsed;
static R_Vertex *s_batch;
static int       s_batchUsed;
static int       s_batchIsLine;

/* The clipped path. The batch is converted into this at flush
 * time; keeping R_Vertex as the pool type leaves the public
 * interface and its 0x20-byte assertion alone. */
static R_Vertex3 *s_batch3;

/* The frustum r_set_projection was last given, and whether the
 * matrix on the device still matches it. */
static float s_projCentreX = 320.0f;
static float s_projCentreY = 240.0f;
static float s_projNear    = 1.0f;
static float s_projFar     = 1.0f;
static int   s_projDirty   = 1;

/* The four caches of d3d-render-state.txt section 4. Same idea, same
 * purpose: skip the write when nothing changed. */
static int s_curTexture = -2;
static int s_curPalette = -2;
static int s_curBlend   = -1;
static int s_curFilter  = -1;
static int s_curDither  = -1;
static int s_curZTest   = -1;
static int s_curZWrite  = -1;
static int s_curAlphaTest = -1;

/*
 * ONE TEXTURE PER BANK AND PALETTE. Slot 0 is the only one a
 * truecolour bank ever uses; an indexed bank fills a slot the
 * first time a face asks for that livery. See r_set_texture.
 */
static R_BankTex s_bankTex[BANK_COUNT][BANK_MAX_PALETTES];

static int s_statTris;
static int s_statBatches;
static int s_statTrisLast;
static int s_statBatchesLast;

/* ------------------------------------------------------------------ */
/* Blend selection - d3d-render-state.txt section 3, transcribed       */
/* ------------------------------------------------------------------ */

int r_blend_for_kind(unsigned int effectiveKind, int textured, int keyed16)
{
	switch (effectiveKind) {
	case 0x44:
	case 0x45:
		return R_BLEND_HALF;        /* D3D_SetBlendModeFlatAlpha(1)   */
	case 0x0c:
		return R_BLEND_ADD_MOD;     /* ONE / SRCCOLOR                 */
	case 0x2c:
	case 0x3e:
		return R_BLEND_HALF;        /* D3D_SetBlendMode(1, 1)         */
	default:
		break;
	}

	/*
	 * The only conditional row. In the original it needs -Alpha set
	 * AND bank flags 0x10 and 0x02 - a colour-KEYED 16-BIT texture -
	 * which is exactly the case with genuinely transparent texels.
	 * Blending those at a flat 50 per cent would show the key colour
	 * through, which is why this row is not the half blend.
	 */
	if (textured && keyed16)
		return R_BLEND_SRCALPHA;

	return R_BLEND_NONE;
}

/* ------------------------------------------------------------------ */
/* Device                                                             */
/* ------------------------------------------------------------------ */

static void compute_letterbox(int clientW, int clientH)
{
	float sx;
	float sy;

	if (clientW < 1) clientW = 1;
	if (clientH < 1) clientH = 1;

	sx = (float)clientW / (float)s_designW;
	sy = (float)clientH / (float)s_designH;
	s_viewScale = (sx < sy) ? sx : sy;

	s_viewOffX = ((float)clientW - (float)s_designW * s_viewScale) * 0.5f;
	s_viewOffY = ((float)clientH - (float)s_designH * s_viewScale) * 0.5f;

	/* The letterbox is baked into the projection, so it has to be
	 * rebuilt whenever the letterbox moves. */
	s_projDirty = 1;
}

/*
 * Pick a depth format the device actually claims. D16 is the one every
 * part supports; the two 24-bit formats are tried first only because
 * they are what a modern driver would rather give us.
 */
static D3DFORMAT choose_depth_format(D3DFORMAT adapterFormat)
{
	static const D3DFORMAT order[] = {
		D3DFMT_D24S8, D3DFMT_D24X8, D3DFMT_D16
	};
	int i;

	for (i = 0; i < (int)(sizeof(order) / sizeof(order[0])); i++) {
		if (FAILED(s_d3d->CheckDeviceFormat(D3DADAPTER_DEFAULT,
		                                    D3DDEVTYPE_HAL,
		                                    adapterFormat,
		                                    D3DUSAGE_DEPTHSTENCIL,
		                                    D3DRTYPE_SURFACE,
		                                    order[i])))
		    continue;
		if (FAILED(s_d3d->CheckDepthStencilMatch(D3DADAPTER_DEFAULT,
		                                         D3DDEVTYPE_HAL,
		                                         adapterFormat,
		                                         adapterFormat,
		                                         order[i])))
		    continue;
		return order[i];
	}
	return D3DFMT_UNKNOWN;
}

/* Everything that has to be re-established after a create or a Reset.
 * Managed textures survive both, so this is render state only. */
static void apply_initial_state(void)
{
	s_dev->SetFVF(R_FVF);

	/*
	 * CULLING IS OFF, and that is the faithful choice rather than a
	 * lazy one. Screen3D_CullAndQueueFaces has already back-face
	 * culled on the CPU before anything reaches a back end, so a
	 * second cull here could only remove geometry the engine meant to
	 * keep - and the winding the engine emits has not been verified
	 * against a D3D convention.
	 */
	s_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	s_dev->SetRenderState(D3DRS_LIGHTING, FALSE);
	/*
	 * SPECULAR IS OFF, and that is what the original does. The
	 * execute-buffer state block emits SPECULARENABLE from
	 * DAT_0053bb34, which Cd3d_Ctor zeroes in a run of stores right
	 * after XOR EAX,EAX and which nothing else in the image touches -
	 * the only two references to that address are that write and the
	 * read. So the specular component D3D_DrawScene computes for every
	 * vertex is never used. Leaving this TRUE would be a trap for
	 * whoever later writes the specular value faithfully: it would add
	 * the colour to itself.
	 */
	s_dev->SetRenderState(D3DRS_SPECULARENABLE, FALSE);
	s_dev->SetRenderState(D3DRS_CLIPPING, TRUE);

	/*
	 * ALPHATESTENABLE is always 0 in the binary - see
	 * d3d-render-state.txt section 2.1 - and this used to stay at 0
	 * on the grounds that the original resolves keying with a real
	 * blend instead.
	 *
	 * THAT REASONING WAS COMPLETE FOR THE ORIGINAL AND NOT FOR US.
	 * Its software path has no depth buffer, so a blend is all it
	 * needs; ours has one, and a blend does not stop an alpha-zero
	 * fragment from WRITING DEPTH. See r_set_alpha_test in render.h.
	 *
	 * Off by default; drawscene.c turns it on for keyed geometry only.
	 */
	s_dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
	s_dev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER);
	s_dev->SetRenderState(D3DRS_ALPHAREF, 0);

	/*
	 * TEXTUREMAPBLEND was removed after DirectX 6. The binary only
	 * ever sets it to MODULATE or DECAL, chosen from the device's
	 * dwTextureBlendCaps in D3D_EnumDeviceCallback - modulate when
	 * supported, decal otherwise. Every modern part supports modulate,
	 * so this is the fixed stage setup that replaces it.
	 */
	s_dev->SetTextureStageState(0, D3DTSS_COLOROP,   D3DTOP_MODULATE);
	s_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
	s_dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
	s_dev->SetTextureStageState(0, D3DTSS_ALPHAOP,   D3DTOP_MODULATE);
	s_dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
	s_dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
	s_dev->SetTextureStageState(1, D3DTSS_COLOROP,   D3DTOP_DISABLE);
	s_dev->SetTextureStageState(1, D3DTSS_ALPHAOP,   D3DTOP_DISABLE);

	s_dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
	s_dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);

	/* Force every cache to re-emit. */
	s_curTexture = -2;
	s_curPalette = -2;
	s_curBlend   = -1;
	s_curFilter  = -1;
	s_curDither  = -1;
	s_curZTest   = -1;
	s_curZWrite  = -1;
	s_curAlphaTest = -1;

	r_set_blend(R_BLEND_NONE);
	/*
	 * POINT, NOT LINEAR, AND THE OPTIONS PAGE IS THE EVIDENCE.
	 *
	 * The D3D row on page 11 has THREE states, not two: OFF, ON and
	 * FILTERED. Bilinear filtering is a setting the player turns on,
	 * so the default a page draws with is UNFILTERED [verified - the
	 * row cycles through 0x0053be0c, and the string FILTERED is at
	 * 0x004e1d40 with its five translations].
	 *
	 * o LINEAR WAS ACTIVELY WRONG FOR THIS ART. A third of the bike
	 *   body's faces sample one texel or less, so a filter that
	 *   blends neighbours turns a spoke into a smear. 1997 texture
	 *   art was authored to be point-sampled.
	 *
	 * o THE 2D BLIT KEEPS ITS OWN RULE. r_present sets the filter
	 *   again from the window scale just before it draws the
	 *   letterbox quad, so the framebuffer still smooths when the
	 *   window is not 1:1. The two decisions are independent and
	 *   were sharing one piece of state by accident.
	 */
	r_set_filter(0);
	r_set_dither(0);
	r_set_depth(1, 1);
	r_set_texture(-1, -1);
}

static void fill_present_params(int clientW, int clientH,
                                D3DFORMAT depthFormat)
{
	ZeroMemory(&s_pp, sizeof(s_pp));
	s_pp.Windowed               = TRUE;
	s_pp.SwapEffect             = D3DSWAPEFFECT_DISCARD;
	s_pp.BackBufferFormat       = D3DFMT_UNKNOWN;  /* desktop format */
	s_pp.BackBufferWidth        = (UINT)((clientW > 0) ? clientW : 1);
	s_pp.BackBufferHeight       = (UINT)((clientH > 0) ? clientH : 1);
	s_pp.BackBufferCount        = 1;
	s_pp.hDeviceWindow          = s_hwnd;
	s_pp.EnableAutoDepthStencil = (depthFormat != D3DFMT_UNKNOWN);
	s_pp.AutoDepthStencilFormat = depthFormat;
	s_pp.PresentationInterval   = D3DPRESENT_INTERVAL_ONE;
}

int r_init(HWND hwnd, int designW, int designH)
{
	PFN_Direct3DCreate9 create;
	D3DDISPLAYMODE      mode;
	D3DCAPS9            caps;
	D3DFORMAT           depth;
	RECT                rc;
	DWORD               behaviour;
	HRESULT             hr;

	if (s_ready)
		return 1;

	s_hwnd    = hwnd;
	s_designW = (designW > 0) ? designW : 640;
	s_designH = (designH > 0) ? designH : 480;

	s_pool   = (R_Vertex *)malloc(sizeof(R_Vertex) * R_MAX_VERTICES);
	s_batch  = (R_Vertex *)malloc(sizeof(R_Vertex) * R_MAX_BATCH);
	s_batch3 = (R_Vertex3 *)malloc(sizeof(R_Vertex3) * R_MAX_BATCH);
	if (!s_pool || !s_batch || !s_batch3) {
		mrlog("d3d9: out of memory for the vertex pool\n");
		r_shutdown();
		return 0;
	}

	/* By name, not by import - see the header comment. */
	s_d3dDll = LoadLibraryA("d3d9.dll");
	if (!s_d3dDll) {
		mrlog_err("d3d9: LoadLibrary d3d9.dll failed (%lu)\n",
		       GetLastError());
		r_shutdown();
		return 0;
	}
	create = (PFN_Direct3DCreate9)GetProcAddress(s_d3dDll,
	                                             "Direct3DCreate9");
	if (!create) {
		mrlog("d3d9: d3d9.dll has no Direct3DCreate9\n");
		r_shutdown();
		return 0;
	}

	s_d3d = create(D3D_SDK_VERSION);
	if (!s_d3d) {
		mrlog("d3d9: Direct3DCreate9 returned null\n");
		r_shutdown();
		return 0;
	}

	if (FAILED(s_d3d->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &mode))) {
		mrlog_err("d3d9: GetAdapterDisplayMode failed\n");
		r_shutdown();
		return 0;
	}

	depth = choose_depth_format(mode.Format);
	if (depth == D3DFMT_UNKNOWN)
		mrlog("d3d9: no usable depth format - running without one\n");

	if (!GetClientRect(hwnd, &rc)) {
		rc.left = rc.top = 0;
		rc.right  = s_designW;
		rc.bottom = s_designH;
	}
	fill_present_params(rc.right - rc.left, rc.bottom - rc.top, depth);

	behaviour = D3DCREATE_SOFTWARE_VERTEXPROCESSING;
	if (SUCCEEDED(s_d3d->GetDeviceCaps(D3DADAPTER_DEFAULT,
	                                   D3DDEVTYPE_HAL, &caps)) &&
	    (caps.DevCaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT) != 0) {
		behaviour = D3DCREATE_HARDWARE_VERTEXPROCESSING;
	}
	behaviour |= D3DCREATE_FPU_PRESERVE;

	/*
	 * D3DCREATE_FPU_PRESERVE matters here more than in a typical
	 * program. Without it D3D9 puts the x87 unit into single
	 * precision, and this codebase transcribes arithmetic that ran
	 * under the ORIGINAL's control word - MR_Vertex.txt section 7.3
	 * documents Screen3D_TransformVertsFlat setting precision and two
	 * exception masks itself. Letting the API silently change rounding
	 * underneath transcribed float code is exactly the kind of
	 * divergence that is impossible to find later.
	 */
	hr = s_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
	                         behaviour, &s_pp, &s_dev);
	if (FAILED(hr)) {
		mrlog_err("d3d9: CreateDevice failed (0x%08lx) - "
		       "falling back to the GDI present\n", (unsigned long)hr);
		r_shutdown();
		return 0;
	}

	hr = s_dev->CreateTexture(R_FB_TEX_W, R_FB_TEX_H, 1, 0,
	                          D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
	                          &s_fbTex, NULL);
	if (FAILED(hr)) {
		mrlog_err("d3d9: could not create the 2D layer texture "
		       "(0x%08lx)\n", (unsigned long)hr);
		r_shutdown();
		return 0;
	}

	hr = s_dev->CreateTexture(R_FB_TEX_W, R_FB_TEX_H, 1, 0,
	                          D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
	                          &s_overlayTex, NULL);
	if (FAILED(hr)) {
		mrlog_err("d3d9: could not create the overlay texture "
		       "(0x%08lx)\n", (unsigned long)hr);
		r_shutdown();
		return 0;
	}

	/*
	 * CLEARED ONCE, TO FULLY TRANSPARENT. Nothing ever writes
	 * outside the rect a composite uploads, so the margins stay
	 * alpha 0 for the life of the device - which is the whole
	 * point of the texture being separate.
	 */
	{
		D3DLOCKED_RECT lr;

		if (SUCCEEDED(s_overlayTex->LockRect(0, &lr, NULL, 0))) {
			int row;

			for (row = 0; row < R_FB_TEX_H; row++)
				memset((unsigned char *)lr.pBits + row * lr.Pitch,
			           0, R_FB_TEX_W * 4);
			s_overlayTex->UnlockRect(0);
		}
	}

	compute_letterbox(rc.right - rc.left, rc.bottom - rc.top);
	apply_initial_state();

	s_ready = 1;
	mrlog("d3d9: device up, %ux%u back buffer, %s vertex processing\n",
	       s_pp.BackBufferWidth, s_pp.BackBufferHeight,
	       (behaviour & D3DCREATE_HARDWARE_VERTEXPROCESSING)
	           ? "hardware" : "software");
	mrlog("      depth %s, fixed function, no D3DX, no shaders\n",
	       (depth == D3DFMT_D24S8) ? "D24S8" :
	       (depth == D3DFMT_D24X8) ? "D24X8" :
	       (depth == D3DFMT_D16)   ? "D16"   : "none");
	return 1;
}

void r_shutdown(void)
{
	int i;

	for (i = 0; i < BANK_COUNT; i++) {
		for (int q = 0; q < BANK_MAX_PALETTES; q++) {
			if (s_bankTex[i][q].tex) {
				s_bankTex[i][q].tex->Release();
				s_bankTex[i][q].tex = NULL;
			}
			s_bankTex[i][q].valid = 0;
		}
	}
	if (s_fbTex) { s_fbTex->Release(); s_fbTex = NULL; }
	if (s_overlayTex) { s_overlayTex->Release();
		                s_overlayTex = NULL; }
	if (s_dev)   { s_dev->Release();   s_dev   = NULL; }
	if (s_d3d)   { s_d3d->Release();   s_d3d   = NULL; }
	if (s_d3dDll) { FreeLibrary(s_d3dDll); s_d3dDll = NULL; }

	free(s_pool);  s_pool  = NULL;
	free(s_batch); s_batch = NULL;
	free(s_batch3); s_batch3 = NULL;

	s_poolUsed  = 0;
	s_batchUsed = 0;
	s_ready     = 0;
	s_inScene   = 0;
	s_lost      = 0;
}

int r_ready(void)
{
	return s_ready;
}

/*
 * A Reset destroys everything in D3DPOOL_DEFAULT. Nothing here is in
 * D3DPOOL_DEFAULT - textures are MANAGED and geometry goes out through
 * DrawPrimitiveUP from ordinary memory - so a Reset needs no resource
 * teardown at all, only the render state re-applied. That is the
 * reason for both choices; a dynamic vertex buffer would be faster and
 * would drag a lost-device recreation path in behind it.
 */
static int try_reset(int clientW, int clientH)
{
	HRESULT hr;

	fill_present_params(clientW, clientH, s_pp.AutoDepthStencilFormat);
	hr = s_dev->Reset(&s_pp);
	if (FAILED(hr))
		return 0;

	compute_letterbox(clientW, clientH);
	apply_initial_state();
	s_lost = 0;
	return 1;
}

void r_set_design_size(int w, int h)
{
	if (w <= 0 || h <= 0)
		return;
	if (w == s_designW && h == s_designH)
		return;

	s_designW = w;
	s_designH = h;

	/*
	 * The DEVICE does not change - the back buffer is the client
	 * area and the client area has not moved. Only the mapping of
	 * frame pixels onto it has, which is compute_letterbox's whole
	 * job. s_fbTex is 1024x512 and covers either frame width, so
	 * there is nothing to reallocate.
	 */
	if (s_ready)
		compute_letterbox((int)s_pp.BackBufferWidth,
	                      (int)s_pp.BackBufferHeight);
}

void r_resize(int clientW, int clientH)
{
	if (!s_ready || !s_dev)
		return;
	if (clientW < 1 || clientH < 1)
		return;      /* minimised; nothing to do until it comes back */
	if ((UINT)clientW == s_pp.BackBufferWidth &&
	    (UINT)clientH == s_pp.BackBufferHeight)
	    return;

	if (!try_reset(clientW, clientH))
		s_lost = 1;  /* r_begin_scene retries */
}

/* ------------------------------------------------------------------ */
/* Textures                                                            */
/* ------------------------------------------------------------------ */

/*
 * One source pixel of the capture, in the game's 640x480 space, as a
 * device pixel of the render target. The letterbox lives here so the
 * caller never sees it; sampling the CENTRE of the scaled footprint
 * rather than its corner is what keeps the grab stable when the
 * window is not an exact multiple of 640x480.
 */
static void capture_device_pixel(int gx, int gy, int maxX, int maxY,
                                 int *outX, int *outY)
{
	int x = (int)(s_viewOffX + ((float)gx + 0.5f) * s_viewScale);
	int y = (int)(s_viewOffY + ((float)gy + 0.5f) * s_viewScale);

	if (x < 0)    x = 0;
	if (y < 0)    y = 0;
	if (x > maxX) x = maxX;
	if (y > maxY) y = maxY;
	*outX = x;
	*outY = y;
}

/*
 * capture_block - how many device pixels make up one GAME pixel.
 *
 * THIS IS NOT A DIVERGENCE, IT IS THE RECONSTRUCTION OF THE ORIGINAL'S
 * SOURCE. MenuTools_CaptureScreenToBank samples a 640x480 SOFTWARE
 * framebuffer, so its "pixel 0 of every 5" is a real, final screen
 * pixel. Ours reads a D3D render target that may be several times
 * that, where a single point sample is one of nine or more device
 * pixels covering the same game pixel - so any fine detail aliases,
 * and the capture comes out grainy in a way the original never was.
 *
 * Averaging the block that maps to one game pixel yields what that
 * game pixel WOULD have been at 640x480, which is exactly the value
 * the original would have decimated. The 2-of-every-5 pattern is then
 * applied to that, unchanged.
 *
 * At a 640x480 client s_viewScale is exactly 1.0 and the block is
 * 1x1, so this is bit-for-bit the old behaviour where the old
 * behaviour was already right.
 */
static int capture_block(void)
{
	int n = (int)(s_viewScale + 0.5f);

	if (n < 1) n = 1;
	if (n > 8) n = 8;               /* 8x8 is 64 reads; past that the
	                                 * extra accuracy is invisible. */
	return n;
}

int r_capture_screen(unsigned short *dst, int dstW, int dstH,
                     int dstPitchPx)
{
	IDirect3DSurface9 *rt  = NULL;
	IDirect3DSurface9 *sys = NULL;
	D3DSURFACE_DESC    desc;
	D3DLOCKED_RECT     lr;
	int                ok = 0;
	int                dx, dy;
	int                maxX, maxY;
	int                blk;

	if (!s_ready || !s_dev || !dst || dstW <= 0 || dstH <= 0 ||
	    dstPitchPx < dstW)
	    return 0;

	if (FAILED(s_dev->GetRenderTarget(0, &rt)))
		return 0;
	if (FAILED(rt->GetDesc(&desc)))
		goto done;

	/*
	 * Only these two formats are read. The back buffer is created with
	 * D3DFMT_UNKNOWN, i.e. the desktop format, so in practice it is one
	 * of the 32-bit ones; R5G6B5 is handled because a 16-bit desktop is
	 * exactly the mode this game was born in and it would be perverse
	 * to fail there.
	 */
	if (desc.Format != D3DFMT_X8R8G8B8 && desc.Format != D3DFMT_A8R8G8B8 &&
	    desc.Format != D3DFMT_R5G6B5) {
		static int warned;

		if (!warned) {
			warned = 1;
			mrlog("d3d9: cannot capture render target format %u\n",
			      (unsigned)desc.Format);
		}
		goto done;
	}

	/*
	 * SYSTEMMEM offscreen plain is the only destination
	 * GetRenderTargetData accepts, and it must match the source in size
	 * and format. Allocated per call on purpose: this runs once per
	 * page transition, and a cached surface would have to be released
	 * and rebuilt on every device Reset for no measurable gain.
	 */
	if (FAILED(s_dev->CreateOffscreenPlainSurface(desc.Width, desc.Height,
	                                              desc.Format,
	                                              D3DPOOL_SYSTEMMEM,
	                                              &sys, NULL)))
	    goto done;
	if (FAILED(s_dev->GetRenderTargetData(rt, sys)))
		goto done;
	if (FAILED(sys->LockRect(&lr, NULL, D3DLOCK_READONLY)))
		goto done;

	maxX = (int)desc.Width  - 1;
	maxY = (int)desc.Height - 1;
	blk  = capture_block();

	for (dy = 0; dy < dstH; dy++) {
		unsigned short *row = dst + (size_t)dy * (size_t)dstPitchPx;
		int             gy  = 5 * (dy >> 1) + ((dy & 1) ? 2 : 0);

		for (dx = 0; dx < dstW; dx++) {
			int gx = 5 * (dx >> 1) + ((dx & 1) ? 2 : 0);
			int sx, sy, bx, by;
			unsigned int r = 0, g = 0, b = 0;

			capture_device_pixel(gx, gy, maxX, maxY, &sx, &sy);

			/* Average the device block this game pixel covers -
			 * see capture_block. */
			for (by = 0; by < blk; by++) {
				int py = sy - blk / 2 + by;
				const unsigned char *srow;

				if (py < 0)    py = 0;
				if (py > maxY) py = maxY;
				srow = (const unsigned char *)lr.pBits +
				       (size_t)py * lr.Pitch;

				for (bx = 0; bx < blk; bx++) {
					int px = sx - blk / 2 + bx;

					if (px < 0)    px = 0;
					if (px > maxX) px = maxX;
					if (desc.Format == D3DFMT_R5G6B5) {
						unsigned int p = *(const unsigned short *)
						                 (srow + (size_t)px * 2);

						r += ((p >> 11) & 0x1f) << 3;
						g += ((p >> 5)  & 0x3f) << 2;
						b += (p & 0x1f) << 3;
					} else {
						unsigned int p = *(const unsigned int *)
						                 (srow + (size_t)px * 4);

						r += (p >> 16) & 0xff;
						g += (p >> 8)  & 0xff;
						b += p & 0xff;
					}
				}
			}
			{
				unsigned int n = (unsigned int)(blk * blk);

				r /= n;
				g /= n;
				b /= n;
			}
			row[dx] = (unsigned short)(((r >> 3) << 11) |
			                           ((g >> 2) << 5)  |
			                           (b >> 3));
		}
	}

	sys->UnlockRect();
	ok = 1;

done:
	if (sys) sys->Release();
	if (rt)  rt->Release();
	return ok;
}

void r_invalidate_bank(int bank)
{
	if (bank < 0 || bank >= BANK_COUNT)
		return;
	for (int q = 0; q < BANK_MAX_PALETTES; q++) {
		if (s_bankTex[bank][q].tex) {
			s_bankTex[bank][q].tex->Release();
			s_bankTex[bank][q].tex = NULL;
		}
		s_bankTex[bank][q].valid = 0;
	}
	if (s_curTexture == bank) {
		s_curTexture = -2;
		s_curPalette = -2;
	}
}

/*
 * Bank pages are RGB565 and could go up as D3DFMT_R5G6B5 unchanged.
 * They go up as A8R8G8B8 instead, for one reason: a colour-keyed page
 * needs per-texel alpha, and R5G6B5 has none. Using one format for
 * both keeps a single upload path, and 256x256 pages make the memory
 * cost irrelevant.
 *
 * The 5-to-8 widening here REPLICATES the high bits into the low ones.
 * That is deliberately NOT what the original's flat-colour expansion
 * does - MR_Primitive.txt section 6.1 places each 5-bit channel at the
 * top of its 8-bit field with no replicate, so its white comes out
 * 0xf8f8f8. These are different paths: that one widens a primitive's
 * flat COLOUR, this one widens a TEXEL for sampling. A texture that
 * never reaches full white would be a visible error, so this replicates
 * and the flat-colour path, when it is written, must not.
 */
/* ------------------------------------------------------------------ */
/* THE TWO SPECIAL RECTANGLES ON THE BIKE'S PARTICLE ATLAS              */
/* ------------------------------------------------------------------ */

/*
 * ORIGIN  maybe_D3D_LoadRawToTexture @ 0x0041cef2..0x0041cf7e
 *
 * bnk00_01.raw is the bike's effects atlas and it carries two regions
 * whose alpha is NOT the ordinary colour key. Everything outside them
 * is fully opaque; see d3d-textures.txt 5.2.
 *
 *     Rect A   x 222..255, y 0..255    eight 34x32 dust-puff frames
 *     Rect B   x   0..119, y 0..40     the bike's shadow silhouette
 *
 * RECT A IS BRIGHTENED ON UPLOAD, and that is the whole reason the
 * smoke looked black. The source texels are genuinely dark - the
 * frame the particles sample reads RGB (4, 9, 4) out of (31, 63, 31)
 * - and the original does:
 *
 *     Color_RgbToHsv(r, g, b, &h, &s, &v)
 *     alpha = (int)(v * 16)        0x004d0640, brightness-derived
 *     v     = v * 5                0x004d0648, a FIVE-FOLD lift
 *     Color_HsvToRgb(h, s, v, &r, &g, &b)
 *
 * so what reaches the texture is a bright, soft-edged puff. Uploading
 * the raw texel instead gives a dark opaque blob.
 *
 * o v * 5 OVERFLOWS AND IS MEANT TO. Color_HsvToRgb clamps each
 *   channel at 255 and prints "Depassement pour teinte R %d" when it
 *   does, so saturation is the designed behaviour rather than a bug
 *   we are reproducing.
 *
 * o THE ALPHA IS FOUR BITS, 0..15, and v == 1.0 yields 16. The
 *   original stores it in a 4-bit field; we clamp, because a
 *   wrapped 16 would make the brightest texel transparent.
 *
 * RECT B IS A FLAT ALPHA OF 8 - the 0x0041cf79 MOV EDI,8 - which is
 * half of 15. The shadow is authored at blue 16 rather than black so
 * it sits one step above the colour key.
 *
 * NOT GATED ON keyBlack. The original selects the block by the path
 * match below and by fmt->alphaBits == 4, so it fires on the bike
 * atlas even though that atlas registers with keyBlack 0. Our back end
 * uploads A8R8G8B8 unconditionally, so the format half is always true
 * here and only the path match remains.
 *
 * WHICH PAGES GET THE RECTANGLES. The original strcmps the bank's
 * stored source path against exactly four literals [verified,
 * 0x0041c2f0..0x0041c3b5]:
 *
 *     data\SpeedPkt\bnk00_01.raw     data\speed\bnk00_01.raw
 *     data\ktmcross\bnk00_01.raw     data\KTMpkt\bnk00_01.raw
 *
 * Only TWO distinct pages exist behind those four names - SPEED is
 * byte-identical to SPEEDPKT, KTMCROSS to KTMPKT - so the four are per
 * bike DIRECTORY, not per layout.
 *
 * THE MATCH LIVES IN bank_is_bike_atlas, shared with the FX pool,
 * which wants the SAME PAGE for a different reason - it draws the
 * smoke and the tyre specks out of it. Both used to answer the
 * question separately, and the FX side answered it with a single
 * hardcoded bike, so a dirt level drew no particles at all. See
 * bank.h for the four literals and the boundary rule.
 */

/*
 * The two rectangles. THE HALF-SIZE PAGE HALVES EVERY BOUND - the
 * second arm at 0x0041c3b9, taken when bank flag 8 is set - and no
 * stock data reaches it, because the atlas ships full size.
 */
typedef struct {
	int ax0, ax1, ay0, ay1;
	int bx0, bx1, by0, by1;
} MR_BankRects;

static const MR_BankRects kRectsFull  = { 222, 255, 0, 255, 0, 119, 0, 40 };
static const MR_BankRects kRectsSmall = { 111, 127, 0, 127, 0,  59, 0, 20 };

#define RECT_B_ALPHA   8
#define RECT_A_V_SCALE   5.0
#define RECT_A_ALPHA_MUL 16.0
#define RECT_ALPHA_MAX   15

/* ORIGIN  Color_RgbToHsv @ 0x0041bb20. h in degrees, s and v in 0..1;
 * h is 400.0 - the 0x004d05f8 sentinel - when the colour is grey. */
static void rgb_to_hsv(unsigned int r, unsigned int g, unsigned int b,
                       double *h, double *s, double *v)
{
	double rf = (double)r * (1.0 / 255.0);   /* 0x004d05e8 */
	double gf = (double)g * (1.0 / 255.0);
	double bf = (double)b * (1.0 / 255.0);
	double mx = rf > gf ? rf : gf;
	double mn = rf < gf ? rf : gf;
	double d;

	if (bf > mx) mx = bf;
	if (bf < mn) mn = bf;

	*v = mx;
	*s = (mx == 0.0) ? 0.0 : (mx - mn) / mx;
	if (*s == 0.0) {
		*h = 400.0;
		return;
	}

	d = mx - mn;
	if (rf == mx)       *h = (gf - bf) / d;
	else if (gf == mx)  *h = (bf - rf) / d + 2.0;   /* 0x004d0600 */
	else                *h = (rf - gf) / d + 4.0;   /* 0x004d0608 */

	*h *= 60.0;                                     /* 0x004d0610 */
	if (*h < 0.0)
		*h += 360.0;                            /* 0x004d0618 */
}


/*
 * ORIGIN  Color_HsvToRgb @ 0x0041bd30
 *
 * The decompiler collapses the six sector bodies - they are pure x87 -
 * but the constants pin the form: h is degrees, 0x004d0620 is 1/60 so
 * the sector is floor(h/60), and each channel is clamped at 255 with
 * an overflow message. This is the textbook conversion with that clamp.
 */
static void hsv_to_rgb(double h, double s, double v,
                       unsigned int *r, unsigned int *g, unsigned int *b)
{
	double f, p, q, t;
	double rf, gf, bf;
	int    sector;

	if (s == 0.0) {
		rf = gf = bf = v;
	} else {
		if (h == 360.0)
			h = 0.0;
		sector = (int)floor(h * (1.0 / 60.0));      /* 0x004d0620 */
		f = h * (1.0 / 60.0) - (double)sector;
		p = v * (1.0 - s);
		q = v * (1.0 - s * f);
		t = v * (1.0 - s * (1.0 - f));
		switch (sector) {
		case 0:  rf = v; gf = t; bf = p; break;
		case 1:  rf = q; gf = v; bf = p; break;
		case 2:  rf = p; gf = v; bf = t; break;
		case 3:  rf = p; gf = q; bf = v; break;
		case 4:  rf = t; gf = p; bf = v; break;
		default: rf = v; gf = p; bf = q; break;
		}
	}

	/* The 0x100 test and the clamp, including the overflow case. */
	{
		int ri = (int)(rf * 255.0);
		int gi = (int)(gf * 255.0);
		int bi = (int)(bf * 255.0);

		*r = (unsigned int)(ri < 0x100 ? (ri < 0 ? 0 : ri) : 0xff);
		*g = (unsigned int)(gi < 0x100 ? (gi < 0 ? 0 : gi) : 0xff);
		*b = (unsigned int)(bi < 0x100 ? (bi < 0 ? 0 : bi) : 0xff);
	}
}


static IDirect3DTexture9 *bank_texture(int bank, int pal)
{
	MR_Bank            *b;
	IDirect3DTexture9  *tex;
	D3DLOCKED_RECT      lr;
	int                 keyed;
	const MR_BankRects *rects;
	int                 x;
	int                 y;
	HRESULT             hr;

	if (bank < 0 || bank >= BANK_COUNT)
		return NULL;

	b = bank_get(bank);
	/*
	 * -1 IS "WHATEVER THE BANK IS SET TO", which is what every
	 * caller meant before liveries existed. A livery past the
	 * end of the palette array falls back the same way rather
	 * than failing: a bike with fewer palettes than racers is
	 * a data question, not a reason to draw nothing.
	 */
	if (!b || pal < 0 || pal >= b->paletteCount)
		pal = b ? b->activePalette : 0;
	if (pal < 0 || pal >= BANK_MAX_PALETTES)
		pal = 0;

	if (s_bankTex[bank][pal].valid)
		return s_bankTex[bank][pal].tex;

	s_bankTex[bank][pal].valid = 1;  /* do not retry a failure */

	if (!b || !b->loaded || !b->page)
		return NULL;
	if (b->isIndexed && !b->palette[pal]) {
		mrlog_err("d3d9: bank %d is indexed with no usable palette\n",
		          bank);
		return NULL;
	}

	/*
	 * BLACK IS TRANSPARENT, AND wantKey IS WHAT DECIDES IT.
	 *
	 * D3D_AllocTextureSlot's wantKey argument sets bank flag 0x10 and,
	 * on the DirectDraw side, calls SetColorKey with a DDCOLORKEY of
	 * {0, 0} - black [verified, 0x0041b54b]. maybe_D3D_LoadRawToTexture
	 * then reaches the same result a second way for a truecolour .RAW:
	 * a texel gets alpha 0 whenever
	 *
	 *     r < 1<<(8-g_bitsR) && g < 1<<(8-g_bitsG) && b < 1<<(8-g_bitsB)
	 *
	 * which for a 565 surface is r<8 && g<4 && b<8 - exactly the texels
	 * whose RGB565 word is zero. So the test below is the same test,
	 * already quantised.
	 *
	 * o THIS IS A BANK PROPERTY, NOT A PAGE'S CHOICE, which is the
	 *   whole point of reading it off the flag. Every menu artwork bank
	 *   passes wantKey 1; the glass capture bank passes 0, because a
	 *   screenshot's blacks are picture and must stay opaque. That one
	 *   argument gets all four banks right on its own.
	 *
	 * o NOT maybe_D3D_RegisterTexture's keyBlack, which every caller
	 *   here passes as 0 and which does something else entirely - see
	 *   bank.h. Confusing the two is what produced the divergence this
	 *   replaced.
	 */
	keyed = (b->flags & BANK_FLAG_KEYED) != 0;

	/*
	 * Is this the bike's effects atlas, and at which size? See the
	 * rect notes above.
	 */
	rects = bank_is_bike_atlas(b->name)
	       ? ((b->flags & BANK_FLAG_SMALL) ? &kRectsSmall
	                                       : &kRectsFull)
	       : NULL;

	hr = s_dev->CreateTexture(BANK_PAGE_W, BANK_PAGE_H, 1, 0,
	                          D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
	                          &tex, NULL);
	if (FAILED(hr)) {
		mrlog_err("d3d9: CreateTexture for bank %d failed (0x%08lx)\n",
		       bank, (unsigned long)hr);
		return NULL;
	}
	if (FAILED(tex->LockRect(0, &lr, NULL, 0))) {
		tex->Release();
		return NULL;
	}

	/*
	 * AN INDEXED PAGE KEYS ON INDEX 0, NOT ON BLACK.
	 * maybe_D3D_LoadRawToTexture has two branches and they do not
	 * agree: the truecolour one drops a texel whose RGB quantises to
	 * zero, the indexed one drops index 0 whatever colour it happens
	 * to name [verified]. Keying an indexed page on black instead
	 * would punch holes wherever the palette holds a dark entry,
	 * which is not the same set of texels and not the artist's.
	 *
	 * o AND WHICH PALETTE IS A BANK PROPERTY. One page plus eight
	 *   palettes is how the game recolours a bike; see
	 *   bank_set_palette.
	 */
	if (b->isIndexed) {
		const unsigned short *tbl = b->palette[pal];

		for (y = 0; y < BANK_PAGE_H; y++) {
			unsigned int        *dst = (unsigned int *)
			        ((unsigned char *)lr.pBits + y * lr.Pitch);
			const unsigned char *src = (const unsigned char *)b->page +
			        (size_t)y * (BANK_PAGE_W * 2);

			for (x = 0; x < BANK_PAGE_W; x++) {
				unsigned int idx = src[x];
				unsigned short p = tbl[idx];
				unsigned int   r = (p >> 11) & 0x1f;
				unsigned int   g = (p >> 5)  & 0x3f;
				unsigned int   bl = p & 0x1f;
				unsigned int   a = (keyed && idx == 0) ? 0u : 0xffu;

				r = (r << 3) | (r >> 2);
				g = (g << 2) | (g >> 4);
				bl = (bl << 3) | (bl >> 2);
				dst[x] = (a << 24) | (r << 16) | (g << 8) | bl;
			}
		}
		tex->UnlockRect(0);
		s_bankTex[bank][pal].tex = tex;
		return tex;
	}

	for (y = 0; y < BANK_PAGE_H; y++) {
		unsigned int   *dst = (unsigned int *)
		                      ((unsigned char *)lr.pBits + y * lr.Pitch);
		unsigned short *src = b->page + (size_t)y * BANK_PAGE_W;

		for (x = 0; x < BANK_PAGE_W; x++) {
			unsigned short p = src[x];
			unsigned int   r = (p >> 11) & 0x1f;
			unsigned int   g = (p >> 5)  & 0x3f;
			unsigned int   bl = p & 0x1f;
			unsigned int   a;

			r = (r << 3) | (r >> 2);
			g = (g << 2) | (g >> 4);
			bl = (bl << 3) | (bl >> 2);

			if (keyed && p == BANK_COLOUR_KEY) {
				/*
				 * THE KEY WINS INSIDE THE RECTANGLES TOO. The
				 * original keys at the surface with a DDCOLORKEY,
				 * which is independent of the per-texel alpha, so
				 * Rect B's black surround is cut away rather than
				 * drawn at its flat half-alpha.
				 */
				a = 0u;
			} else if (rects && x >= rects->ax0 && x <= rects->ax1
			           && y >= rects->ay0 && y <= rects->ay1) {
				double       h, sat, v;
				int          a4;

				rgb_to_hsv(r, g, bl, &h, &sat, &v);
				a4 = (int)(v * RECT_A_ALPHA_MUL);
				if (a4 > RECT_ALPHA_MAX)
					a4 = RECT_ALPHA_MAX;
				if (a4 < 0)
					a4 = 0;
				v *= RECT_A_V_SCALE;
				hsv_to_rgb(h, sat, v, &r, &g, &bl);
				a = (unsigned int)((a4 * 255) / RECT_ALPHA_MAX);
			} else if (rects && x >= rects->bx0 && x <= rects->bx1
			           && y >= rects->by0 && y <= rects->by1) {
				a = (unsigned int)
				    ((RECT_B_ALPHA * 255) / RECT_ALPHA_MAX);
			} else {
				a = 0xffu;
			}

			dst[x] = (a << 24) | (r << 16) | (g << 8) | bl;
		}
	}
	tex->UnlockRect(0);

	s_bankTex[bank][pal].tex = tex;
	return tex;
}

/* ------------------------------------------------------------------ */
/* Batching                                                            */
/* ------------------------------------------------------------------ */

/*
 * Build and install the projection, folding the letterbox into it.
 *
 * The software pipeline hands us camera space with the viewport scale
 * ALREADY MULTIPLIED IN - transform.c folds g_projScaleX/Y into the
 * matrix coefficients once per call - and the screen mapping it would
 * have applied is
 *
 *     screenX = centreX + camX / camZ
 *     screenY = centreY - camY / camZ            note the MINUS
 *
 * followed by the design-to-device letterbox that r_write_vertex used
 * to apply by hand:  device = off + screen * scale - 0.5.
 *
 * All of that composes into one matrix, which is why the viewport is
 * left alone at full render-target size. Putting the letterbox in a
 * D3D viewport instead would also move the SCISSOR, and the
 * pre-transformed framebuffer blit draws outside the 3D region.
 *
 * With a row-vector matrix and clipW = camZ:
 *
 *     NDCx = (2*scale/cw) * camX/camZ + (2*(offX - 0.5 + scale*cx)/cw - 1)
 *     NDCy = (2*scale/ch) * camY/camZ + (1 - 2*(offY - 0.5 + scale*cy)/ch)
 *
 * The y row carries no negation because the D3D viewport already
 * flips: device_y = (1 - NDCy) * ch/2. The minus in the software
 * mapping and the flip in the viewport are the same sign, taken once.
 *
 * DEPTH IS NOT THE OLD MAPPING. The pre-transformed path wrote a
 * LINEAR camZ/zFar into sz; a projection matrix cannot express that,
 * because it would need camZ squared. This is the ordinary 1/z form,
 * so it is monotonic in camZ with more precision near the eye. The
 * queue's back-to-front bucket walk is what actually orders the scene,
 * and the z-buffer only has to separate faces WITHIN one object.
 *
 * o THAT TRADE WANTS A 24-BIT BUFFER, which is what r_init asks for
 *   first. At the menu range - a model about 100 units deep at 5400 -
 *   1/z leaves roughly 590 usable levels of 24-bit depth and only two
 *   or three of 16-bit, where the old linear mapping had about
 *   thirteen. So on a device that fell back to D16 this would trade
 *   edge popping for z-fighting on the flat faces of the camera
 *   model. Worth knowing before blaming the mirror again.
 */
static void apply_projection(void)
{
	D3DMATRIX proj;
	D3DMATRIX id;
	float     cw, ch, n, f;

	if (!s_dev)
		return;

	cw = (float)s_pp.BackBufferWidth;
	ch = (float)s_pp.BackBufferHeight;
	n  = s_projNear;
	f  = s_projFar;
	if (cw <= 0.0f || ch <= 0.0f || n <= 0.0f || f <= n) {
		/*
		 * Once, not per flush. A degenerate frustum leaves whatever
		 * matrix was there before, so the scene either vanishes or
		 * draws at the wrong scale - and the far plane now CLIPS
		 * rather than clamping, so forgetting to set g_drawSceneZFar
		 * empties the screen instead of flattening the depth. That
		 * is worth a line in the log rather than a silent black
		 * frame.
		 */
		static int warned;

		if (!warned) {
			warned = 1;
			mrlog("d3d9: bad frustum, near %.1f far %.1f at %.0fx%.0f "
			      "- is g_drawSceneZFar set?\n", n, f, cw, ch);
		}
		return;
	}

	memset(&proj, 0, sizeof(proj));
	proj._11 = 2.0f * s_viewScale / cw;
	proj._31 = 2.0f * (s_viewOffX - 0.5f + s_viewScale * s_projCentreX) / cw
	           - 1.0f;
	proj._22 = 2.0f * s_viewScale / ch;
	proj._32 = 1.0f -
	           2.0f * (s_viewOffY - 0.5f + s_viewScale * s_projCentreY) / ch;
	proj._33 = f / (f - n);
	proj._34 = 1.0f;                 /* clipW = camZ */
	proj._43 = -n * f / (f - n);

	memset(&id, 0, sizeof(id));
	id._11 = id._22 = id._33 = id._44 = 1.0f;

	s_dev->SetTransform(D3DTS_WORLD, &id);
	s_dev->SetTransform(D3DTS_VIEW, &id);
	s_dev->SetTransform(D3DTS_PROJECTION, &proj);
	s_projDirty = 0;
}

static void flush_batch(void)
{
	UINT count;

	if (s_batchUsed <= 0 || !s_inScene)
		return;

	/*
	 * Convert to the untransformed format. sx/sy/sz carry camX, camY
	 * and camZ here - see fill_vertex in drawscene.c - and rhw is
	 * dropped, because the projection recomputes it.
	 */
	{
		int i;

		for (i = 0; i < s_batchUsed; i++) {
			s_batch3[i].x        = s_batch[i].sx;
			s_batch3[i].y        = s_batch[i].sy;
			s_batch3[i].z        = s_batch[i].sz;
			s_batch3[i].color    = s_batch[i].color;
			s_batch3[i].specular = s_batch[i].specular;
			s_batch3[i].tu       = s_batch[i].tu;
			s_batch3[i].tv       = s_batch[i].tv;
		}
	}

	if (s_projDirty)
		apply_projection();
	s_dev->SetFVF(R_FVF3);

	if (s_batchIsLine) {
		count = (UINT)(s_batchUsed / 2);
		if (count > 0)
			s_dev->DrawPrimitiveUP(D3DPT_LINELIST, count,
		                           s_batch3, sizeof(R_Vertex3));
	} else {
		count = (UINT)(s_batchUsed / 3);
		if (count > 0) {
			s_dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, count,
			                       s_batch3, sizeof(R_Vertex3));
			s_statTris += (int)count;
		}
	}
	s_batchUsed = 0;
	s_statBatches++;
}

/*
 * D3D_InvalidateBatch writes -100 into the open batch key so the next
 * primitive re-emits its texture handle and starts a fresh header.
 * Here the equivalent is simply flushing what is pending: a state
 * change that landed inside an open batch would otherwise apply to
 * geometry already counted in it, which is the same hazard.
 */
void r_invalidate_batch(void)
{
	flush_batch();
}

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

void r_set_texture(int bank, int palette)
{
	IDirect3DTexture9 *tex;

	/*
	 * THE CACHE KEY IS THE PAIR. Two racers on one bank differ
	 * only in livery, so comparing the bank alone would skip
	 * the SetTexture between them and draw the second in the
	 * first one's colours.
	 */
	if (!s_ready || (bank == s_curTexture && palette == s_curPalette))
		return;
	flush_batch();
	s_curTexture = bank;
	s_curPalette = palette;

	tex = (bank >= 0) ? bank_texture(bank, palette) : NULL;
	s_dev->SetTexture(0, tex);

	if (tex) {
		s_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
		s_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
	} else {
		/* Untextured: the diffuse colour alone, which is what an
		 * untextured face carries in flatColor. */
		s_dev->SetTextureStageState(0, D3DTSS_COLOROP,
		                            D3DTOP_SELECTARG2);
		s_dev->SetTextureStageState(0, D3DTSS_ALPHAOP,
		                            D3DTOP_SELECTARG2);
	}
}

void r_set_blend(int mode)
{
	if (!s_ready || mode == s_curBlend)
		return;
	flush_batch();
	s_curBlend = mode;

	switch (mode) {
	case R_BLEND_HALF:
		/* BOTH factors INVSRCALPHA. Not a typo - see render.h. */
		s_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
		s_dev->SetRenderState(D3DRS_SRCBLEND,  D3DBLEND_INVSRCALPHA);
		s_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
		break;
	case R_BLEND_ADD_MOD:
		s_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
		s_dev->SetRenderState(D3DRS_SRCBLEND,  D3DBLEND_ONE);
		s_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_SRCCOLOR);
		break;
	case R_BLEND_SRCALPHA:
		s_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
		s_dev->SetRenderState(D3DRS_SRCBLEND,  D3DBLEND_SRCALPHA);
		s_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
		break;
	default:
		s_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
		break;
	}
}

void r_set_filter(int on)
{
	if (!s_ready || on == s_curFilter)
		return;
	flush_batch();
	s_curFilter = on;
	s_dev->SetSamplerState(0, D3DSAMP_MAGFILTER,
	                       on ? D3DTEXF_LINEAR : D3DTEXF_POINT);
	s_dev->SetSamplerState(0, D3DSAMP_MINFILTER,
	                       on ? D3DTEXF_LINEAR : D3DTEXF_POINT);
}

void r_set_dither(int on)
{
	if (!s_ready || on == s_curDither)
		return;
	flush_batch();
	s_curDither = on;
	s_dev->SetRenderState(D3DRS_DITHERENABLE, on ? TRUE : FALSE);
}

void r_set_projection(float centreX, float centreY,
                      float nearZ, float farZ)
{
	if (centreX == s_projCentreX && centreY == s_projCentreY &&
	    nearZ == s_projNear && farZ == s_projFar)
	    return;

	/* Anything already batched was built for the old frustum. */
	flush_batch();

	s_projCentreX = centreX;
	s_projCentreY = centreY;
	s_projNear    = nearZ;
	s_projFar     = farZ;
	s_projDirty   = 1;
}

void r_set_alpha_test(int on)
{
	if (!s_ready)
		return;
	if (on == s_curAlphaTest)
		return;
	flush_batch();
	s_curAlphaTest = on;
	s_dev->SetRenderState(D3DRS_ALPHATESTENABLE, on ? TRUE : FALSE);
}

void r_set_depth(int test, int write)
{
	if (!s_ready)
		return;
	if (test == s_curZTest && write == s_curZWrite)
		return;
	flush_batch();
	s_curZTest  = test;
	s_curZWrite = write;
	s_dev->SetRenderState(D3DRS_ZENABLE,
	                      test ? D3DZB_TRUE : D3DZB_FALSE);
	s_dev->SetRenderState(D3DRS_ZWRITEENABLE, write ? TRUE : FALSE);
}

/* ------------------------------------------------------------------ */
/* Geometry                                                            */
/* ------------------------------------------------------------------ */

void r_reset_vertices(void)
{
	s_poolUsed = 0;
}

int r_vertex_count(void)
{
	return s_poolUsed;
}

int r_write_vertex(const R_Vertex *v)
{
	R_Vertex *d;

	if (!s_ready || !v || s_poolUsed >= R_MAX_VERTICES)
		return -1;

	d = &s_pool[s_poolUsed];
	*d = *v;

	/*
	 * NO TRANSFORM HERE ANY MORE. This used to map design space to
	 * device pixels and apply the half-pixel shift, because the
	 * vertex went to D3D pre-transformed. Scene geometry now arrives
	 * in camera space and the projection matrix carries the letterbox
	 * and the half pixel - see apply_projection - so the vertex is
	 * stored exactly as drawscene built it.
	 *
	 * The original has no transform here either: it owns the display
	 * mode, so its screen coordinates ARE device coordinates. The
	 * letterbox is ours, the price of a resizable window, and it now
	 * lives in one matrix rather than on every vertex.
	 */
	return s_poolUsed++;
}

static void push(int index)
{
	if (index < 0 || index >= s_poolUsed || s_batchUsed >= R_MAX_BATCH)
		return;
	s_batch[s_batchUsed++] = s_pool[index];
}

void r_emit_triangle(int a, int b, int c)
{
	if (!s_ready || !s_inScene)
		return;
	if (s_batchIsLine) {
		flush_batch();
		s_batchIsLine = 0;
	}
	if (s_batchUsed + 3 > R_MAX_BATCH)
		flush_batch();
	push(a);
	push(b);
	push(c);
}

void r_emit_line(int a, int b)
{
	if (!s_ready || !s_inScene)
		return;
	if (!s_batchIsLine) {
		flush_batch();
		s_batchIsLine = 1;
	}
	if (s_batchUsed + 2 > R_MAX_BATCH)
		flush_batch();
	push(a);
	push(b);
}

/* ------------------------------------------------------------------ */
/* The frame                                                           */
/* ------------------------------------------------------------------ */

int r_begin_scene(void)
{
	HRESULT hr;
	RECT    rc;

	if (!s_ready)
		return 0;

	if (s_lost) {
		hr = s_dev->TestCooperativeLevel();
		if (hr == D3DERR_DEVICELOST)
			return 0;                       /* still gone; skip */
		if (hr == D3DERR_DEVICENOTRESET || FAILED(hr)) {
			if (!GetClientRect(s_hwnd, &rc))
				return 0;
			if (!try_reset(rc.right - rc.left, rc.bottom - rc.top))
				return 0;
		} else {
			s_lost = 0;
		}
	}

	s_poolUsed    = 0;
	s_batchUsed   = 0;
	s_batchIsLine = 0;
	s_statTris    = 0;
	s_statBatches = 0;

	/*
	 * The original clears Z but NOT colour, because the queue walk
	 * repaints every pixel - and it clears Z after the present rather
	 * than before the next frame. We clear both, here: the letterbox
	 * margins are outside anything the game draws, and nothing yet
	 * guarantees full coverage. A port that later reaches full
	 * coverage can drop the colour clear and match.
	 */
	if (FAILED(s_dev->Clear(0, NULL,
	                        D3DCLEAR_TARGET |
	                        (s_pp.EnableAutoDepthStencil
	                             ? D3DCLEAR_ZBUFFER : 0),
	                        D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0)))
	    return 0;

	if (FAILED(s_dev->BeginScene()))
		return 0;

	/*
	 * CONFINE DRAWING TO THE DESIGN RECTANGLE.
	 *
	 * The back buffer is the whole client area, but the GAME is 640 by
	 * 480 and always was: g_viewCentreX/Y stay at 320/240 and the
	 * projection base comes from Screen3D_Open as 307200/width and
	 * 230400/height, none of which a resize touches. Widening the
	 * window does not widen the field of view - it only gives the
	 * scaled 640x480 image more room, with bars around it.
	 *
	 * WITHOUT A SCISSOR, THE BARS ARE NOT EMPTY. The game legitimately
	 * holds geometry outside its own screen - page 14 slides its logos
	 * in from world x = -12000, and the cull deliberately keeps a
	 * partially visible face WHOLE rather than splitting it (see
	 * guard_band in cull.c). On the original that surplus falls off
	 * the edge of a fullscreen 640x480 mode and is simply never
	 * rasterised. Here the render target extends past the letterbox,
	 * so it lands in the margins and the logos are visible sliding in
	 * across the black.
	 *
	 * o THE UNTRANSFORMED CHANGE MADE IT OBVIOUS RATHER THAN CAUSING
	 *   IT. While vertices went pre-transformed, D3D9 dropped those
	 *   far-off-screen triangles at its 2D guard band - which is what
	 *   the edge popping was. Clipping them properly against the
	 *   frustum means they survive, and the frustum covers the whole
	 *   back buffer, so they draw. One gap, two symptoms.
	 *
	 * The scissor goes on AFTER the clear, so the margins still clear
	 * to black, and it is set per frame because a device Reset drops
	 * it along with every other state.
	 */
	{
		RECT sc;
		LONG maxX = (LONG)s_pp.BackBufferWidth;
		LONG maxY = (LONG)s_pp.BackBufferHeight;

		sc.left   = (LONG)s_viewOffX;
		sc.top    = (LONG)s_viewOffY;
		sc.right  = (LONG)(s_viewOffX + (float)s_designW * s_viewScale +
		                   0.5f);
		sc.bottom = (LONG)(s_viewOffY + (float)s_designH * s_viewScale +
		                   0.5f);

		if (sc.left   < 0)    sc.left   = 0;
		if (sc.top    < 0)    sc.top    = 0;
		if (sc.right  > maxX) sc.right  = maxX;
		if (sc.bottom > maxY) sc.bottom = maxY;
		if (sc.right  <= sc.left) sc.right  = sc.left + 1;
		if (sc.bottom <= sc.top)  sc.bottom = sc.top + 1;

		s_dev->SetScissorRect(&sc);
		s_dev->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
	}

	s_inScene = 1;
	return 1;
}

void r_end_scene(void)
{
	if (!s_ready || !s_inScene)
		return;
	flush_batch();
	s_dev->EndScene();
	s_inScene = 0;
	s_statTrisLast    = s_statTris;
	s_statBatchesLast = s_statBatches;
}

void r_present(void)
{
	HRESULT hr;

	if (!s_ready)
		return;
	if (s_inScene)
		r_end_scene();

	hr = s_dev->Present(NULL, NULL, NULL, NULL);
	if (hr == D3DERR_DEVICELOST || hr == D3DERR_DEVICEREMOVED)
		s_lost = 1;
}

void r_frame_stats(int *triangles, int *batches)
{
	if (triangles) *triangles = s_statTrisLast;
	if (batches)   *batches   = s_statBatchesLast;
}

/* ------------------------------------------------------------------ */
/* The 2D layer                                                        */
/* ------------------------------------------------------------------ */

/*
 * fb_blit_rect - ONE RECTANGLE of the software 2D layer, as a quad.
 *
 * The whole-screen call is the 2D layer going down as the background
 * (see r_draw_framebuffer). The same body also serves the debug
 * overlay, which has to be drawn AFTER the geometry instead, so it is
 * parameterised by rect rather than copied.
 *
 * BOTH CASES ARE OPAQUE, and for the overlay that is the whole point.
 * A colour key on framebuffer pixel 0 was tried first and is wrong:
 * it leaves bare glyphs, and white letters over pale geometry are
 * unreadable. What made the counter legible in the first place was
 * the cleared framebuffer behind it, so the overlay has to carry that
 * background across with it rather than drop it.
 *
 * THE RECT IS IN FRAMEBUFFER PIXELS and is mapped onto the design
 * rectangle proportionally, which is what keeps the whole-screen case
 * byte-identical to what it was: px 0, pw fbW maps to 0 .. s_designW.
 */
static void fb_blit_rect_src(int px, int py, int pw, int ph,
                             const unsigned short *src0,
                             const unsigned short *srcWhite)
{
	D3DLOCKED_RECT lr;
	R_Vertex       q[4];
	R_Vertex       dq[6];
	RECT           dirty;
	int            x;
	int            y;
	int            i;
	float          u0;
	float          v0;
	float          u1;
	float          v1;
	float          sx0;
	float          sy0;
	float          sx1;
	float          sy1;
	int            w;
	int            h;

	IDirect3DTexture9 *tex = srcWhite ? s_overlayTex : s_fbTex;

	if (!s_ready || !s_inScene || !tex || !src0)
		return;

	w = (g_frameWidth  < R_FB_TEX_W) ? g_frameWidth  : R_FB_TEX_W;
	h = (g_frameHeight < R_FB_TEX_H) ? g_frameHeight : R_FB_TEX_H;

	/* Clip the request to the part of the framebuffer that exists. */
	if (px < 0) { pw += px; px = 0; }
	if (py < 0) { ph += py; py = 0; }
	if (px + pw > w) pw = w - px;
	if (py + ph > h) ph = h - py;
	if (pw <= 0 || ph <= 0)
		return;

	dirty.left = px; dirty.top = py;
	dirty.right = px + pw; dirty.bottom = py + ph;
	if (FAILED(tex->LockRect(0, &lr, &dirty, 0)))
		return;

	for (y = 0; y < ph; y++) {
		unsigned int   *dst = (unsigned int *)
		                      ((unsigned char *)lr.pBits + y * lr.Pitch);
		const unsigned short *src = src0
		                    + (size_t)(py + y) * g_frameStridePx + px;

		for (x = 0; x < pw; x++) {
			unsigned short p = src[x];
			unsigned int   r = (p >> 11) & 0x1f;
			unsigned int   g = (p >> 5)  & 0x3f;
			unsigned int   b = p & 0x1f;

			r = (r << 3) | (r >> 2);
			g = (g << 2) | (g >> 4);
			b = (b << 3) | (b >> 2);

			/*
			 * NO srcWhite IS "ALL OPAQUE", which is what the 2D
			 * layer wants. With one, the green channel of the
			 * over-white pass minus this one IS the coverage -
			 * see fb_overlay_begin - and the colour here is
			 * already premultiplied by it.
			 */
			unsigned int a = 0xffu;

			if (srcWhite) {
				unsigned short wp = srcWhite[(size_t)(py + y) *
				                            g_frameStridePx + px + x];
				int gw = (wp >> 5) & 0x3f;
				int gb = (p >> 5) & 0x3f;
				int cov = 63 - (gw - gb);

				if (cov < 0)  cov = 0;
				if (cov > 63) cov = 63;
				a = (unsigned int)((cov * 255) / 63);
			}
			dst[x] = (a << 24) | (r << 16) | (g << 8) | b;
		}
	}
	tex->UnlockRect(0);

	u0 = (float)px / (float)R_FB_TEX_W;
	v0 = (float)py / (float)R_FB_TEX_H;
	u1 = (float)(px + pw) / (float)R_FB_TEX_W;
	v1 = (float)(py + ph) / (float)R_FB_TEX_H;

	sx0 = (float)px * (float)s_designW / (float)w;
	sy0 = (float)py * (float)s_designH / (float)h;
	sx1 = (float)(px + pw) * (float)s_designW / (float)w;
	sy1 = (float)(py + ph) * (float)s_designH / (float)h;

	for (i = 0; i < 4; i++) {
		q[i].sz       = 1.0f;   /* behind everything, z writes off */
		q[i].rhw      = 1.0f;
		q[i].color    = 0xffffffffu;
		q[i].specular = 0;
	}
	q[0].sx = sx0;               q[0].sy = sy0;
	q[0].tu = u0;                q[0].tv = v0;
	q[1].sx = sx1;               q[1].sy = sy0;
	q[1].tu = u1;                q[1].tv = v0;
	q[2].sx = sx1;               q[2].sy = sy1;
	q[2].tu = u1;                q[2].tv = v1;
	q[3].sx = sx0;               q[3].sy = sy1;
	q[3].tu = u0;                q[3].tv = v1;

	for (i = 0; i < 4; i++) {
		q[i].sx = s_viewOffX + q[i].sx * s_viewScale - 0.5f;
		q[i].sy = s_viewOffY + q[i].sy * s_viewScale - 0.5f;
	}

	dq[0] = q[0]; dq[1] = q[1]; dq[2] = q[2];
	dq[3] = q[0]; dq[4] = q[2]; dq[5] = q[3];

	/* Outside the batching path on purpose: this is not queue
	 * geometry, it is the 2D surface the queue draws over. */
	flush_batch();
	/* The blit is the one pre-transformed thing left, so it owns the
	 * FVF while it draws; flush_batch sets the other one back. */
	s_dev->SetFVF(R_FVF);
	s_curTexture = -2;
	s_curPalette = -2;
	r_set_blend(R_BLEND_NONE);
	/*
	 * POINT AT 1:1, LINEAR WHEN SCALED.
	 *
	 * This used to be an unconditional r_set_filter(0), on the
	 * grounds that point matches the GDI blit - and at a 640x480
	 * client it does, exactly, which is the case retail ever ran in.
	 *
	 * o BUT WE SCALE AND RETAIL DID NOT. The 2D layer is one 640x480
	 *   texture, so any other window size point-upscales all of it,
	 *   while the 3D is real geometry rendered at the window's own
	 *   resolution. The result is crisp letters against a visibly
	 *   blocky banner - an asymmetry the original never showed,
	 *   because there both layers were 640x480.
	 *
	 * The comparison is exact on purpose: compute_letterbox derives
	 * the scale as min(clientW/640, clientH/480), so a 640x480 client
	 * gives exactly 1.0f and takes the point path with no epsilon
	 * needed.
	 *
	 * o A/B THIS BY HAND with r_set_filter(0) for always-point or
	 *   r_set_filter(1) for always-linear. An integer scale is the
	 *   arguable case - 2x point is sharp rather than blocky, and
	 *   linear softens it - so if 2x looks worse, the condition to
	 *   try is s_viewScale != floorf(s_viewScale).
	 */
	r_set_filter(s_viewScale != 1.0f);
	r_set_depth(0, 0);
	/*
	 * PREMULTIPLIED ALPHA for the overlay, none for the 2D
	 * layer. ONE / INVSRCALPHA, because the colour written
	 * above is already multiplied by the coverage.
	 */
	if (srcWhite) {
		s_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
		s_dev->SetRenderState(D3DRS_SRCBLEND,  D3DBLEND_ONE);
		s_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
		s_curBlend = -1;      /* poked directly - drop the cache */
	}

	s_dev->SetTexture(0, tex);
	s_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
	s_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
	s_dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, dq, sizeof(R_Vertex));
	s_statBatches++;

	s_dev->SetTexture(0, NULL);
	s_curTexture = -2;
	s_curPalette = -2;
	if (srcWhite) {
		s_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
		s_curBlend = -1;
	}
	r_set_depth(1, 1);
}

static void fb_blit_rect(int px, int py, int pw, int ph)
{
	fb_blit_rect_src(px, py, pw, ph, g_frameBuffer, NULL);
}

void r_draw_framebuffer(void)
{
	fb_blit_rect(0, 0, R_FB_TEX_W, R_FB_TEX_H);
}

/*
 * R_OVERLAY_PAD - a margin of framebuffer around the text.
 *
 * The glyphs sit tight against their own bounding box, so without it
 * the plate ends flush with the ink on all four sides and the first
 * and last letters touch the geometry.
 */
#define R_OVERLAY_PAD 2

void r_draw_overlay_rect(int x, int y, int w, int h)
{
	fb_blit_rect(x - R_OVERLAY_PAD, y - R_OVERLAY_PAD,
	             w + R_OVERLAY_PAD * 2, h + R_OVERLAY_PAD * 2);
}

/*
 * ORIGIN  maybe_Screen3D_FadeRect @ 0x0049a5a0, the interior
 *
 * SRC ZERO, DEST SRCCOLOR, with the quad at 50 per cent grey: every
 * destination pixel is multiplied by a half. The original's software
 * path shifts right by one and clears the two borrowed bits, which is
 * the same result in the same place.
 *
 * The rect arrives in framebuffer pixels, so it goes through the same
 * letterbox transform the 2D layer uses.
 */
void r_darken_rect(int x, int y, int w, int h)
{
	R_Vertex q[4];
	R_Vertex dq[6];
	float    sx0, sy0, sx1, sy1;
	int      fw, fh;
	int      i;

	if (!s_ready || !s_inScene || w <= 0 || h <= 0)
		return;

	fw = (g_frameWidth  < R_FB_TEX_W) ? g_frameWidth  : R_FB_TEX_W;
	fh = (g_frameHeight < R_FB_TEX_H) ? g_frameHeight : R_FB_TEX_H;
	if (fw <= 0 || fh <= 0)
		return;

	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > fw) w = fw - x;
	if (y + h > fh) h = fh - y;
	if (w <= 0 || h <= 0)
		return;

	sx0 = (float)x * (float)s_designW / (float)fw;
	sy0 = (float)y * (float)s_designH / (float)fh;
	sx1 = (float)(x + w) * (float)s_designW / (float)fw;
	sy1 = (float)(y + h) * (float)s_designH / (float)fh;

	for (i = 0; i < 4; i++) {
		q[i].sz       = 1.0f;
		q[i].rhw      = 1.0f;
		q[i].color    = 0xff808080u;   /* the half */
		q[i].specular = 0;
		q[i].tu       = 0.0f;
		q[i].tv       = 0.0f;
	}
	q[0].sx = sx0; q[0].sy = sy0;
	q[1].sx = sx1; q[1].sy = sy0;
	q[2].sx = sx1; q[2].sy = sy1;
	q[3].sx = sx0; q[3].sy = sy1;

	for (i = 0; i < 4; i++) {
		q[i].sx = s_viewOffX + q[i].sx * s_viewScale - 0.5f;
		q[i].sy = s_viewOffY + q[i].sy * s_viewScale - 0.5f;
	}

	dq[0] = q[0]; dq[1] = q[1]; dq[2] = q[2];
	dq[3] = q[0]; dq[4] = q[2]; dq[5] = q[3];

	flush_batch();
	s_dev->SetFVF(R_FVF);
	s_dev->SetTexture(0, NULL);
	s_curTexture = -2;
	s_curPalette = -2;
	s_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
	s_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
	s_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
	s_dev->SetRenderState(D3DRS_SRCBLEND,  D3DBLEND_ZERO);
	s_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_SRCCOLOR);
	s_curBlend = -1;              /* poked directly - drop the cache */
	r_set_depth(0, 0);

	s_dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, dq, sizeof(R_Vertex));
	s_statBatches++;

	s_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
	r_set_depth(1, 1);
}

void r_draw_hud_rect(int x, int y, int w, int h)
{
	fb_blit_rect_src(x - R_OVERLAY_PAD, y - R_OVERLAY_PAD,
	                 w + R_OVERLAY_PAD * 2, h + R_OVERLAY_PAD * 2,
	                 g_overlayBuffer, g_overlayBuffer2);
}
