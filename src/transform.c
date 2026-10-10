/*
 * transform.c - object space to screen space.
 *
 * From Screen3D_TransformVertsFlat (0x0049b2c0), read out of the
 * disassembly rather than the decompiled C, because the two things
 * that matter most about this function are invisible in the C: the
 * precision it runs at, and how it rounds. See section "The control
 * word" below.
 */

#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "transform.h"
#include "log.h"

int   g_viewCentreX = 320;
int   g_viewCentreY = 240;

/*
 * THE PROJECTION, as base times zoom.
 *
 * Read out of Screen3D_Open (0x004995a0) and the four accessors at
 * 0x0049a0e0, 0x0049a100, 0x0049a120 and 0x0049a150.
 *
 * The SCALES are what the transform multiplies by. The BASES and the
 * ZOOM exist so a caller can change one without recomputing the other,
 * and every setter rewrites the scales it affects - which is why
 * nothing outside this file ever assigns a scale directly.
 */
float g_projBaseX   = 480.0f;
float g_projBaseY   = 480.0f;
float g_projZoom    = 1.0f;
float g_projScaleX  = 480.0f;
float g_projScaleY  = 480.0f;
float g_nearPlaneZ  = 1.0f;

/*
 * Screen3D_SetProjBaseX  0x0049a0e0
 * Screen3D_SetProjBaseY  0x0049a100
 *
 * One axis of the base, and that axis' scale re-derived. The original
 * multiplies in opposite operand order between the two - zoom*f for X,
 * f*zoom for Y - which is a codegen artefact, not a difference.
 */
void screen3d_set_proj_base_x(float f)
{
	g_projBaseX  = f;
	g_projScaleX = g_projZoom * f;
}

void screen3d_set_proj_base_y(float f)
{
	g_projBaseY  = f;
	g_projScaleY = f * g_projZoom;
}

/*
 * Screen3D_SetProjZoom  0x0049a120
 *
 * Both scales at once, from the two bases:
 *
 *     FLD  [g_projBaseY] ; FMUL [esp+4]
 *     FLD  [g_projBaseX] ; FMUL [esp+4]
 *     FXCH ; FSTP [g_projScaleY] ; FSTP [g_projScaleX]
 *     MOV  eax,[esp+4]   ; MOV  [g_projZoom],eax
 *
 * Note the zoom is stored LAST, and as a raw dword copy rather than
 * through the FPU. Neither detail changes the result - the bases are
 * not the scales, so nothing here reads what it just wrote - but the
 * shape is the same load-everything-before-storing-anything one that
 * Matrix34_Multiply has, where it DID matter.
 */
/* ORIGIN  Screen3D_SetProjZoom @ 0x0049a120 */
void screen3d_set_proj_zoom(float f)
{
	g_projScaleY = g_projBaseY * f;
	g_projScaleX = g_projBaseX * f;
	g_projZoom   = f;
}

/*
 * Screen3D_GetProjZoom  0x0049a150 - six bytes, FLD and RET.
 */
/* ORIGIN  Screen3D_GetProjZoom @ 0x0049a150 */
float screen3d_get_proj_zoom(void)
{
	return g_projZoom;
}

/*
 * The projection half of Screen3D_Open (0x004995a0) - the only place
 * that ever sets the bases from a resolution:
 *
 *     g_projZoom  = 1.0
 *     g_projBaseX = g_projScaleX = 307200.0 / width
 *     g_projBaseY = g_projScaleY = 230400.0 / height
 *
 * 307200 is 640*480 and 230400 is 480*480, held as doubles at
 * 0x004d1f50 and 0x004d1f58. All five call sites push 0x280 and 0x1e0
 * - 640 and 480 - so both bases come out at 480, and the numerators
 * are really just "480, scaled by how far the actual resolution is
 * from the design one".
 *
 * o 480 IS THE FOCAL LENGTH IN PIXELS. Not 320. For a 640 wide image
 *   that is a 67 degree horizontal field, not the 90 degrees this
 *   reconstruction assumed while the value was still a guess.
 *   Everything the pipeline drew was 1.5x too small, uniformly -
 *   which is the kind of error that reads as "the art is wrong"
 *   rather than "the projection is wrong".
 *
 * DIVERGENCE: the zero guards. The original divides unconditionally,
 * and since Screen3D_Open masks the zero-divide exception it would
 * produce an infinity rather than trap. Ours refuses to, because an
 * infinite scale poisons every vertex downstream and no legitimate
 * caller passes zero.
 *
 * DIVERGENCE: THE X FORMULA IS DROPPED WHEN THE FRAME IS NOT 4:3.
 *
 * The two numerators are redundant. 307200 is 640*480 and 230400 is
 * 480*480, so for any width/height of 4/3,
 *
 *     230400 / height = 230400 / (3*width/4) = 307200 / width
 *
 * identically - and all three modes the original offers are 4:3, so
 * g_projBaseX has never once differed from g_projBaseY in the shipped
 * game. The projection is square-pixel and the X formula carries no
 * information.
 *
 * KEEPING IT AND WIDENING THE FRAME WOULD SQUASH THE PICTURE:
 * 307200/854 is 359.7 against a vertical 480, so the horizontal
 * magnification would fall BELOW the vertical and the image would
 * compress sideways. The bug would read as a bad aspect ratio rather
 * than as a wrong constant, which is why this is spelled out here.
 *
 * So at 4:3 this is the original's arithmetic to the bit, and at any
 * other aspect both bases come from the height. See
 * docs/subsystems/projection.txt 7.
 *
 * o IT BARELY MATTERS IN PRACTICE, and that is worth knowing before
 *   anyone debugs a widescreen lens here. These values are dead on
 *   arrival: menu_set_page overwrites both with 1911 on every page
 *   change and race_draw_3d with RACE_VIEW_LENS every frame, and
 *   BOTH of those set the two bases to the SAME number - so the port
 *   is square-pixel everywhere regardless. This is the start-up
 *   value and the one that would be wrong for the first few frames.
 */
/* ORIGIN  Screen3D_Open @ 0x004995a0 */
void screen3d_open_projection(int width, int height)
{
	g_projZoom  = 1.0f;
	g_projBaseY = g_projScaleY =
	    (height > 0) ? (float)(230400.0 / (double)height) : 480.0f;

	if (width > 0 && height > 0 && width * 3 == height * 4) {
		g_projBaseX = g_projScaleX = (float)(307200.0 / (double)width);
	} else {
		g_projBaseX = g_projScaleX = g_projBaseY;
	}
}

static MR_Vertex *s_pool;
static int        s_poolUsed;
static int        s_poolCapacity;

int vertexpool_create(int capacity)
{
	vertexpool_destroy();
	if (capacity <= 0)
		return 0;
	s_pool = (MR_Vertex *)malloc((size_t)capacity * sizeof(MR_Vertex));
	if (!s_pool)
		return 0;
	/*
	 * DIVERGENCE, and a deliberate one. The original does not clear
	 * the pool, and MR_Vertex.txt section 7.2 points out that a vertex
	 * behind the near plane therefore keeps whatever the previous user
	 * of that slot left in screenX and screenY. We zero it once at
	 * creation so a first frame is at least deterministic, but we do
	 * NOT clear per frame - reproducing the stale-slot behaviour is
	 * the point, since the clipper is what makes it visible.
	 */
	memset(s_pool, 0, (size_t)capacity * sizeof(MR_Vertex));
	s_poolCapacity = capacity;
	s_poolUsed = 0;
	return 1;
}

void vertexpool_destroy(void)
{
	if (s_pool)
		free(s_pool);
	s_pool = NULL;
	s_poolUsed = 0;
	s_poolCapacity = 0;
}

void vertexpool_reset(void)    { s_poolUsed = 0; }

/*
 * vertexpool_alloc - hand out n consecutive slots.
 *
 * The clipper's new vertices go into THE SAME per-frame bump pool the
 * transform fills. That is the original's arrangement, not a
 * convenience: Screen3D_ClipTriangleNear writes 0x006d20d8 in fourteen
 * places, which is the pool Screen3D_TransformVertsFlat advances.
 *
 * So a clip-heavy frame consumes slots on top of the transformed ones
 * and NOTHING BOUNDS EITHER in the original. Here it returns NULL
 * instead of running off the end.
 */
MR_Vertex *vertexpool_alloc(int n)
{
	MR_Vertex *p;

	if (n <= 0 || !s_pool || s_poolUsed + n > s_poolCapacity)
		return NULL;
	p = &s_pool[s_poolUsed];
	s_poolUsed += n;
	return p;
}
int  vertexpool_used(void)     { return s_poolUsed; }
int  vertexpool_capacity(void) { return s_poolCapacity; }

/*
 * round_nearest - what FISTP does here.
 *
 * THE CONTROL WORD. The original brackets the whole loop in:
 *
 *     FSTCW  save
 *     AND 0xfcff      clear bits 8,9  - precision control to SINGLE
 *     OR  0x000c      set bits 2,3    - mask zero-divide and overflow
 *     FLDCW  modified
 *     ... loop ...
 *     FLDCW  save
 *
 * Rounding control, bits 10 and 11, is NOT touched, so it stays at
 * round-to-nearest-even and FISTP ROUNDS. It does not truncate.
 *
 * That is worth being careful about because the usual MSVC truncation
 * idiom looks similar and is not: it is AND 0xf3ff / OR 0x0c00, which
 * touches entirely different bits. Reading one for the other gives a
 * transform that is off by up to a pixel everywhere, and by a whole
 * pixel in a consistent direction on negative coordinates.
 *
 * lrintf honours the current rounding mode, so it is the equivalent.
 * A cast is not: a cast truncates toward zero.
 */
static int round_nearest(float v)
{
	return (int)lrintf(v);
}

MR_Vertex *transform_verts_flat(const float *m, const float *src,
                                int count, int intensity)
{
	MR_Vertex *out;
	MR_Vertex *v;
	int i;
	int shade;
	/* The matrix with the viewport scales folded in, exactly as the
	 * original does once per call rather than per vertex. */
	float xx, xy, xz, xw;
	float yx, yy, yz, yw;
	float zx, zy, zz, zw;

	if (!m || !src || count <= 0 || !s_pool)
		return NULL;

	/* DIVERGENCE: the original has no bound check at all. */
	if (s_poolUsed + count > s_poolCapacity) {
		/*
		 * ONCE. A full pool is full again on the next frame and
		 * every frame after, so an unguarded line here writes
		 * sixty a second and buries everything else - which is
		 * the failure mode log.h exists to warn about.
		 */
		static int warned;
		if (!warned) {
			warned = 1;
			mrlog_err("transform: vertex pool full, %d of %d, "
			          "wanted %d - geometry dropped\n",
			          s_poolUsed, s_poolCapacity, count);
		}
		return NULL;
	}

	out = s_pool + s_poolUsed;
	s_poolUsed += count;

	xx = m[0] * g_projScaleX;
	xy = m[3] * g_projScaleX;
	xz = m[6] * g_projScaleX;
	xw = m[9] * g_projScaleX;

	yx = m[1] * g_projScaleY;
	yy = m[4] * g_projScaleY;
	yz = m[7] * g_projScaleY;
	yw = m[10] * g_projScaleY;

	zx = m[2];
	zy = m[5];
	zz = m[8];
	zw = m[11];

	/* structs/MR_Vertex.txt section 7.1: the producer states the
	 * 19.13 format outright. Bits 0..12 are always zero here, and a
	 * caller sets the full-bright or literal-colour flags by passing a
	 * large intensity rather than by OR-ing anything afterwards. */
	shade = intensity << 13;

	for (i = 0; i < count; i++) {
		float x = src[0];
		float y = src[1];
		float z = src[2];
		src += 3;

		v = &out[i];
		v->camX = x * xx + y * xy + z * xz + xw;
		v->camY = x * yx + y * yy + z * yz + yw;
		v->camZ = x * zx + y * zy + z * zz + zw;
		v->shade = shade;

		/* Zero divide is masked in the original, so this is infinity
		 * rather than a fault when camZ is zero. */
		v->invZ = 1.0f / v->camZ;

		/*
		 * Screen coordinates are CONDITIONAL. A vertex behind the near
		 * plane keeps whatever was in the slot. Every consumer has to
		 * test camZ before reading these.
		 */
		if (v->camZ >= g_nearPlaneZ) {
			v->screenX = round_nearest((float)g_viewCentreX +
			                           v->invZ * v->camX);
			v->screenY = round_nearest((float)g_viewCentreY -
			                           v->invZ * v->camY);
		}
	}

	return out;
}


/*
 * Screen3D_TransformVertsShaded, base path. See transform.h for what is
 * deliberately absent.
 *
 * The matrix is folded exactly as the flat transform folds it: elements
 * 0, 3, 6 and 9 carry g_projScaleX and 1, 4, 7 and 10 carry
 * g_projScaleY, while 2, 5, 8 and 11 are copied raw. Two functions
 * written years apart in the same file agreeing on that is worth more
 * than either one alone.
 */
MR_Vertex *transform_verts_shaded(const float *m,
                                  const float *xz, const float *y,
                                  int count, const int *intensity)
{
	MR_Vertex *base;
	MR_Vertex *v;
	float      xx, xy, xz_, xw;
	float      yx, yy, yz, yw;
	float      zx, zy, zz, zw;
	int        i;

	if (!m || !xz || count <= 0)
		return NULL;

	/*
	 * THE NULL Y ARRAY IS THE ORIGINAL'S EARLY OUT, and note where it
	 * sits: before the pool cursor is advanced. So a call with no y
	 * stream reserves nothing and returns the base as if it had.
	 */
	if (!y)
		return &s_pool[s_poolUsed];

	if (s_poolUsed + count > s_poolCapacity)
		return NULL;

	base = &s_pool[s_poolUsed];
	s_poolUsed += count;

	xx = m[0] * g_projScaleX;  xy  = m[3] * g_projScaleX;
	xz_ = m[6] * g_projScaleX; xw  = m[9] * g_projScaleX;
	yx = m[1] * g_projScaleY;  yy  = m[4] * g_projScaleY;
	yz = m[7] * g_projScaleY;  yw  = m[10] * g_projScaleY;
	zx = m[2];                 zy  = m[5];
	zz = m[8];                 zw  = m[11];

	v = base;
	for (i = 0; i < count; i++, v++) {
		float px = xz[i * 3];
		float pz = xz[i * 3 + 2];   /* [1] is NOT read - y comes from y[] */
		float py = y[i];

		v->camX = px * xx + py * xy + pz * xz_ + xw;
		v->camY = px * yx + py * yy + pz * yz + yw;
		v->camZ = px * zx + py * zy + pz * zz + zw;

		/* Optional, and NOT written when absent - the slot keeps
		 * whatever was there. */
		if (intensity)
			v->shade = intensity[i] << 13;

		v->invZ = 1.0f / v->camZ;

		if (v->camZ >= g_nearPlaneZ) {
			v->screenX = round_nearest((float)g_viewCentreX +
			                           v->invZ * v->camX);
			v->screenY = round_nearest((float)g_viewCentreY -
			                           v->invZ * v->camY);
		}
	}
	return base;
}
