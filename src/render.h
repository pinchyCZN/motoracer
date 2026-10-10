/*
 * render.h - the renderer seam.
 *
 * This is the interface the original already has, not a new one. From
 * docs/subsystems/renderer.txt section 3.4: the hardware renderer is
 * six geometry calls and six state calls, and everything above them
 * only QUEUES primitives. Screen3D_DrawScene is a single if - one
 * branch runs the software rasterizer, the other hands the same queued
 * list to D3D_DrawScene and returns.
 *
 *     D3D_BeginExecuteBuffer   ->  r_begin_scene
 *     D3D_WriteVertex          ->  r_write_vertex
 *     D3D_EmitTriangle         ->  r_emit_triangle
 *     D3D_EmitLine             ->  r_emit_line
 *     D3D_EmitProcessVertices  ->  nothing; see below
 *     D3D_CloseExecuteBuffer   ->  r_end_scene
 *
 *     D3D_SetBlendMode          \
 *     D3D_SetBlendModeFlatAlpha  |
 *     D3D_SetBlendModeSrcAlpha   >- r_set_blend / r_set_filter /
 *     D3D_SetTextureFilter       |  r_set_dither / r_set_texture
 *     D3D_SetDither             /
 *     D3D_InvalidateBatch      ->  r_invalidate_batch
 *
 * D3D_EmitProcessVertices has no equivalent because its dwFlags is 2,
 * D3DPROCESSVERTICES_TRANSFORM and not TRANSFORMLIGHT: the engine
 * lights on the CPU through a shade table and hands Direct3D vertices
 * that are already in screen space. A modern pipeline takes those
 * directly. See docs/subsystems/d3d-execute-buffers.txt section 3.1.
 *
 * WHY A SEAM AND NOT JUST D3D9 CALLS. The software rasterizer still
 * has to be portable to, and there is a real chance a second back end
 * (a D3D11 or a GL one) is wanted later. More immediately: keeping the
 * seam means the call SHAPES stay comparable to the binary, so a
 * divergence is visible as a divergence rather than being absorbed
 * into whatever the API of the day wants.
 */

#ifndef MRACER_RENDER_H
#define MRACER_RENDER_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* The vertex                                                          */
/* ------------------------------------------------------------------ */

/*
 * D3D_WriteVertex fills a standard D3DTLVERTEX and advances by 0x20.
 * This IS that struct - screen space, pre-lit, with 1/z in rhw - and
 * D3D9 still accepts exactly it as
 *
 *     D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX1
 *
 * so the geometry needs no conversion, no matrices and no shader. See
 * docs/subsystems/d3d-execute-buffers.txt section 4.3.
 *
 * WHAT sx, sy, sz HOLD DEPENDS ON THE PATH. The 2D and overlay
 * calls submit FRAME coordinates, 0..g_frameWidth-1 by
 * 0..g_frameHeight-1 and not device pixels - the back end applies
 * the letterbox scale itself, which the original needs no equivalent
 * of because it owns the display mode. THE SCENE PATH SUBMITS CAMERA
 * SPACE and lets the projection matrix do the work; see
 * r_set_projection, and fill_vertex in drawscene.c.
 */
typedef struct {
	float        sx;
	float        sy;
	float        sz;        /* 0..1 depth; 1.0 when there is no z buffer */
	float        rhw;       /* 1/z; UNUSED on the scene path            */
	unsigned int color;     /* 0xAARRGGBB, already lit                   */
	unsigned int specular;
	float        tu;
	float        tv;
} R_Vertex;

typedef char r_vertex_is_0x20[(sizeof(R_Vertex) == 0x20) ? 1 : -1];

/* ------------------------------------------------------------------ */
/* Blend modes                                                         */
/* ------------------------------------------------------------------ */

/*
 * There are exactly three blends in the whole hardware renderer, plus
 * opaque, selected by primitive kind in a flat ladder at 0x004afaac.
 * docs/subsystems/d3d-render-state.txt section 3.
 */
#define R_BLEND_NONE     0   /* D3D_SetBlendMode(0, 0)                */
#define R_BLEND_HALF     1   /* INVSRCALPHA / INVSRCALPHA, see below  */
#define R_BLEND_ADD_MOD  2   /* ONE / SRCCOLOR - kind 0x0c only       */
#define R_BLEND_SRCALPHA 3   /* SRCALPHA / INVSRCALPHA                */

/*
 * R_BLEND_HALF sets BOTH factors to INVSRCALPHA, which looks like a
 * bug and is not. The primitive's alpha is stamped to 0x80 just before
 * the state is chosen, so 1-srcAlpha is 0.5 both times and the blend
 * is a symmetric half. THE INTENT IS 50 PER CENT; the pair is only
 * equivalent to the conventional SRCALPHA / INVSRCALPHA because the
 * alpha is hardcoded. If anything ever varies that alpha the two stop
 * agreeing. Section 3.1.
 */
#define R_HALF_BLEND_ALPHA 0x80

/*
 * r_blend_for_kind - the ladder of section 3, transcribed.
 *
 * keyed16 is "the bank carries a colour-keyed 16-bit texture", which
 * in the original is bank flags 0x10 and 0x02 both set AND the -Alpha
 * switch on. That is the only conditional row.
 */
int r_blend_for_kind(unsigned int effectiveKind, int textured, int keyed16);

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

/*
 * r_init - bring up the back end on hwnd.
 *
 * designW / designH are the coordinate space vertices arrive in - the
 * FRAME size, which is 640x480 at 4:3 and 854x480 at 16:9. It is what
 * the letterbox maps onto the client area, so it is also what decides
 * where the black bars go.
 *
 * NOT the size the 2D art is authored at; that is FB_DESIGN_W/H and
 * the two only agree at 4:3. See framebuffer.h.
 *
 * Returns 0 if no device could be created, which is NOT fatal - the
 * caller falls back to the GDI present of framebuffer.c. The original
 * does the same shape of thing, loading DDRAW.DLL by name and
 * complaining rather than failing to link.
 */
int  r_init(HWND hwnd, int designW, int designH);
void r_shutdown(void);
int  r_ready(void);

/* Client area changed. Recomputes the letterbox and resets the device.
 * Cheap and idempotent when the size has not actually changed. */
void r_resize(int clientW, int clientH);

/*
 * The FRAME size changed - the aspect setting moved, not the window.
 * Recomputes the letterbox against the client area the device already
 * has. The back buffer and the device are untouched; only where the
 * picture lands inside them moves.
 *
 * Safe before r_init and on the GDI fallback: it stores the size and
 * does nothing else.
 */
void r_set_design_size(int w, int h);

/* ------------------------------------------------------------------ */
/* The frame                                                           */
/* ------------------------------------------------------------------ */

/*
 * r_begin_scene clears and opens the frame. Note the original does NOT
 * clear colour - the queue walk repaints every pixel - and clears z
 * AFTER the present rather than before the next frame. We clear colour
 * because the letterbox margins would otherwise keep whatever was
 * there, and because nothing yet guarantees full coverage.
 */
int  r_begin_scene(void);
void r_end_scene(void);
void r_present(void);

/*
 * r_draw_framebuffer - put the software 2D layer on screen.
 *
 * The sprite layer writes u16 pixels straight into g_frameBuffer and
 * never enters the rasterizer (MR_Sprite section 5). On the original
 * both layers land in the same DirectDraw surface; here the 2D layer
 * is uploaded as a texture and drawn as a full-screen quad before the
 * geometry, which is the same ORDER - menu pages draw their sprites in
 * the page tick and then call Screen3D_DrawScene.
 */
void r_draw_framebuffer(void);

/*
 * r_draw_overlay_rect - one rectangle of the 2D layer, drawn ON TOP.
 *
 * Same pixels, same texture and same opaque blit as
 * r_draw_framebuffer - just a rectangle of it, drawn after the
 * geometry instead of before. For debug text that must stay legible
 * over the scene: the 2D layer as a whole is the BACKGROUND, so
 * anything written into it is hidden by the first building that
 * stands in front of it.
 *
 * IT CARRIES ITS BACKGROUND WITH IT, and must. Colour keying the
 * framebuffer's zero pixels away leaves bare glyphs, and white
 * letters over pale geometry cannot be read - the cleared framebuffer
 * behind the text is what made the counter legible to begin with.
 *
 * CALL IT INSIDE THE SCENE AND AFTER THE GEOMETRY. The rect is in
 * framebuffer pixels; the caller measures its own text with
 * font_text_width and font_height.
 *
 * OURS, NOT THE ORIGINAL'S, as a MECHANISM - MOTO.EXE has one
 * surface and lets the call order decide, so it needs nothing like
 * this.
 *
 * IT IS ALSO THE RIGHT PLACE FOR GAME 2D THE ORIGINAL DRAWS AFTER
 * THE SCENE, and the in-race menus are exactly that: they float
 * over a running race, so the 2D layer alone hides them behind
 * anything near the camera. WHAT BELONGS IN THE 2D LAYER IS WHAT THE
 * ORIGINAL DRAWS BEFORE Screen3D_DrawScene - the menu pages.
 */
void r_draw_overlay_rect(int x, int y, int w, int h);

/*
 * r_draw_hud_rect - the same, from the OVERLAY layer, keyed.
 *
 * r_draw_overlay_rect re-composites a rectangle of the 2D layer, and
 * during a race that layer holds the horizon - so using it for the
 * in-race menu pastes a slab of sky over the track. This one reads
 * g_overlayBuffer instead, where FB_OVERLAY_KEY means transparent.
 *
 * See fb_overlay_begin in framebuffer.h for how content gets there.
 */
void r_draw_hud_rect(int x, int y, int w, int h);

/*
 * r_darken_rect - halve the brightness of a screen rectangle.
 *
 * ORIGIN  maybe_Screen3D_FadeRect @ 0x0049a5a0
 *
 * THE PANEL BEHIND THE IN-RACE MENUS. The original computes the
 * bounding box of the menu's widgets, inflates it by five pixels and
 * darkens it - which is what makes the text readable over a running
 * race. Its software path is literally
 *
 *     *p = v >> 1;
 *     *p &= ~((1 << (shlR - 1)) | (1 << (shlG - 1)));
 *
 * - exactly half, with the two borrowed low bits cleared - and its
 * hardware path queues a screen quad of kind 0x2c. A multiply blend
 * against 50 per cent grey is the same thing.
 *
 * NOT REPRODUCED: the five-pixel SOFT BORDER. The original ramps the
 * scale from full brightness at the outer edge to half at the inner
 * one, five pixels in from every side, so the panel has no hard edge.
 * That needs either a nine-slice or a generated ramp texture; the
 * interior is the part that makes the menu legible.
 */
void r_darken_rect(int x, int y, int w, int h);

/* ------------------------------------------------------------------ */
/* Geometry                                                            */
/* ------------------------------------------------------------------ */

/* Returns the vertex index, or -1 if the pool is full. The original
 * has no bound check here either; it has maybe_D3D_BufferFull instead,
 * which flushes and continues. */
/*
 * r_set_projection - the frustum scene geometry is projected with.
 *
 * Scene vertices are submitted UNTRANSFORMED, in the camera space the
 * software pipeline already produces, so that D3D9 clips them against
 * a real frustum. SUBMIT THEM PRE-TRANSFORMED AND THERE IS NO
 * FRUSTUM CLIPPING AT ALL: D3D9 rasterises in 2D against a finite
 * guard band and drops triangles whose off-screen corner falls
 * outside it, which shows as faces popping at the edges of the
 * screen.
 *
 * centreX/centreY are g_viewCentreX/Y - where the projection lands on
 * screen. nearZ/farZ bound the depth range. The letterbox and the
 * half-pixel shift are folded into the same matrix, so nothing else
 * transforms a vertex any more.
 *
 * o nearZ SHOULD SIT INSIDE g_nearPlaneZ, not on it. The software
 *   clipper emits vertices at exactly g_nearPlaneZ, and a vertex
 *   exactly on the plane is a coin toss in floating point. Half of it
 *   is a comfortable margin and costs nothing.
 *
 * o CALL IT BEFORE THE QUEUE WALK. It is cheap and self-caching -
 *   unchanged arguments do nothing at all - but it flushes when the
 *   frustum really does change, so calling it mid-walk would split
 *   batches for no reason.
 */
void r_set_projection(float centreX, float centreY,
                      float nearZ, float farZ);

int  r_write_vertex(const R_Vertex *v);
void r_reset_vertices(void);
int  r_vertex_count(void);

/*
 * The three winding cases of D3D_EmitTriangle, which differ only in
 * which vertices they name and which edges they antialias:
 *
 *   mode 2  ->  v, v+1, v+2   wFlags 0x300, two edges
 *   mode 1  ->  v, v+2, v+3   wFlags 0x1f,  strip continuation
 *   other   ->  v, v+1, v+2   wFlags 0x700, all three edges
 *
 * Modes 2 and 1 in sequence are a quad as two triangles sharing an
 * edge. The edge flags were antialiasing hints for 1997 parts and have
 * no D3D9 equivalent, so they are dropped; the VERTEX ORDER is what
 * survives, and that is what r_emit_triangle takes.
 */
void r_emit_triangle(int a, int b, int c);
void r_emit_line(int a, int b);

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

/* bank < 0 is untextured. The texture is created from the MR_Bank page
 * on first use and cached. */
/*
 * palette is the LIVERY - which of the bank's palettes resolves this
 * face - or -1 for the bank's own active palette, which is what every
 * caller wanted before liveries existed.
 *
 * THE BACK END KEEPS ONE TEXTURE PER (BANK, PALETTE). The original
 * does not need to: its span renderer walks an indexed page through a
 * palette pointer per primitive, so recolouring costs nothing. Ours
 * resolves a palette into RGBA at upload time, so the equivalent is a
 * texture each - built on first use, so a race that only ever shows
 * three liveries only ever builds three.
 */
void r_set_texture(int bank, int palette);
void r_set_blend(int mode);
void r_set_filter(int on);
void r_set_dither(int on);
void r_set_depth(int test, int write);

/*
 * r_invalidate_batch - D3D_InvalidateBatch, 0x0041e730.
 *
 * In the original this writes -100 into the open batch key so the next
 * triangle re-emits its texture handle and starts a fresh header. It
 * is a batching optimisation and not semantics: a back end that sets
 * state per draw call produces identical output, more slowly. Kept
 * because the CALL SITES are meaningful - every state emitter ends
 * with one - and dropping it would lose that shape.
 */
void r_invalidate_batch(void);

/*
 * THERE IS NO SET-THE-KEY ENTRY POINT, deliberately. Keying is read
 * off bank flag 0x10 (BANK_FLAG_KEYED) when the texture is built, so
 * bank_alloc_texture_slot's wantKey argument - the original's own
 * switch - is the only thing that decides it. See bank_texture in
 * d3d9rt.cpp.
 */

/*
 * r_set_alpha_test - reject fully transparent texels outright.
 *
 * A PORT ADDITION, and it exists because of another one. The original
 * leaves ALPHATESTENABLE at 0 throughout and resolves colour keying
 * with a blend - which is complete, because its software path has NO
 * DEPTH BUFFER and draws strictly in the queue's sorted order.
 *
 * Ours has a depth buffer, and that changes what a blend alone can
 * do. Alpha blending does not skip the DEPTH WRITE for a fragment
 * whose alpha is zero: the keyed-away texels of a billboard would
 * still stamp the whole quad's rectangle into the depth buffer and
 * occlude whatever is behind it. So keyed geometry needs the test as
 * well as the blend - the test to keep transparent texels out of the
 * depth buffer, the blend to stay faithful to the ladder.
 *
 * GREATER against a reference of 0, because the upload writes exactly
 * 0 or 255 - see bank_texture in d3d9rt.cpp. Anything between would
 * do; 0 is the choice that cannot interact with the flat 0x80 the
 * half-blend kinds carry.
 */
void r_set_alpha_test(int on);

/* Drop a cached texture, e.g. after the bank is reloaded. */
void r_invalidate_bank(int bank);

/*
 * r_capture_screen - grab the rendered frame into a 16-bit RGB565
 * buffer, downsampled the way the original downsamples.
 *
 * This is the source of the glass shatter's texture.
 * MenuTools_CaptureScreenToBank (0x00479260) reads the SOFTWARE
 * framebuffer and decimates it into bank 2; the shards are then
 * textured with the page the player is leaving. We cannot read the
 * software framebuffer, because our 3D never goes there - it goes to
 * D3D - and on a page like 25 the 3D IS the picture. So the capture
 * has to come off the render target instead.
 *
 * THE DECIMATION IS COPIED EXACTLY, because it is visible. The
 * original takes pixels 0 and 2 out of every 5, in both axes:
 *
 *     srcX = 5*(dstX/2) + (dstX & 1 ? 2 : 0)
 *     srcY = 5*(dstY/2) + (dstY & 1 ? 2 : 0)
 *
 * which turns 640x480 into 256x192 - and 256x192 is exactly the UV
 * range MenuGlass_BuildShards writes, u over 0..256 and v over
 * 0..192. Two independently read pieces of code agreeing on an odd
 * number is what confirms the reading. It is nearest-neighbour and
 * unevenly spaced, so a "properly" resampled capture would not match;
 * matching costs nothing.
 *
 * srcX/srcY are in the game's 640x480 space. This maps them through
 * the letterbox to device pixels itself, so the caller thinks only in
 * game coordinates.
 *
 * o CALL IT AFTER THE SCENE IS DRAWN AND BEFORE r_present. The swap
 *   chain is D3DSWAPEFFECT_DISCARD, so the back buffer's contents are
 *   UNDEFINED once Present has run - capturing at the top of a frame
 *   returns whatever the driver felt like leaving there. The original
 *   has no such constraint because its framebuffer persists, which is
 *   why this rule is ours and has to be written down.
 *
 * Returns 1 on success, 0 if the device is not ready, the format is
 * one it cannot read, or any D3D call fails. On failure dst is left
 * untouched.
 */
int r_capture_screen(unsigned short *dst, int dstW, int dstH,
                     int dstPitchPx);

/* Counters for the last completed frame. */
void r_frame_stats(int *triangles, int *batches);

#ifdef __cplusplus
}
#endif

#endif /* MRACER_RENDER_H */
