/*
 * clip.c - see clip.h.
 */

#include <windows.h>
#include <string.h>

#include "clip.h"

/*
 * Screen3D_ClipEdgeNearHw (0x00497d60), read from the disassembly.
 *
 * TWO THINGS THE DECOMPILER HIDES, and the second is a defect.
 *
 * 1. THE DIVISION COMES FIRST. The binary computes
 *
 *        out->camX = (nearZ - a->camZ) * ((b->camX - a->camX) / dz)
 *                    + a->camX
 *
 *    dividing each delta by dz and then scaling, rather than forming
 *    t once and multiplying. Same value in exact arithmetic, different
 *    rounding in single precision. Transcribed as written.
 *
 *    Note there is no projection scale anywhere in here. camX and camY
 *    arrive ALREADY viewport-scaled - Screen3D_TransformVertsFlat
 *    folds the scales into the matrix once per call - so interpolating
 *    them keeps them scaled and the projection below is just the
 *    divide. Two stages agreeing on that is a good sign the reading of
 *    both is right.
 *
 * 2. THE SHADE IS INTERPOLATED IN INTEGER ARITHMETIC, and badly:
 *
 *        dzi = (int)(b->camZ - a->camZ);          // __ftol, truncates
 *        ds  = b->shade - a->shade;
 *        if (dzi != 0) ds /= dzi;                 // INTEGER divide
 *        out->shade = (int)(a->shade + (nearZ - a->camZ) * ds);
 *
 *    That is a shade slope PER WHOLE UNIT OF Z, computed with a
 *    truncating integer divide, then scaled by a float distance. It is
 *    not the lerp the position gets.
 *
 *    o WHEN |dz| < 1 THE INTEGER IS ZERO AND THE DIVIDE IS SKIPPED
 *      ENTIRELY, leaving ds as the full shade difference and
 *      multiplying it by the distance. For a short, nearly
 *      z-parallel edge that produces a wildly wrong intensity rather
 *      than a small error.
 *
 *    Reproduced rather than corrected: it is what the game does, the
 *    reconstruction is supposed to match observed behaviour, and a
 *    "fixed" lerp here would quietly diverge from the original exactly
 *    where the original looks worst. The plate comment on the function
 *    said only "shade = interpolated"; this is that expanded.
 */
/* ORIGIN  Screen3D_ClipEdgeNearHw @ 0x00497d60 */
float clip_edge_near(const MR_Vertex *a, const MR_Vertex *b,
                     MR_Vertex *out)
{
	float dz;
	float span;
	float t;
	int   dzi;
	int   ds;

	dz   = b->camZ - a->camZ;
	span = g_nearPlaneZ - a->camZ;

	if (dz == 0.0f) {
		/* Not reachable through clip_polygon_near, which only calls
		 * this for an edge that genuinely straddles the plane, but a
		 * divide by zero here would be silent and fatal. */
		*out = *a;
		return 0.0f;
	}

	out->camX = span * ((b->camX - a->camX) / dz) + a->camX;
	out->camY = span * ((b->camY - a->camY) / dz) + a->camY;
	out->camZ = g_nearPlaneZ;
	out->invZ = 1.0f / g_nearPlaneZ;

	/*
	 * screenX = centreX + (int)(invZ * camX), with the truncation
	 * applied to the PRODUCT and the centre added as an integer
	 * afterwards. transform.c rounds the whole sum instead - but the
	 * centre is an integer, so adding it before or after makes no
	 * difference and the two stages agree.
	 */
	out->screenX = g_viewCentreX + (int)(out->invZ * out->camX);
	out->screenY = g_viewCentreY - (int)(out->invZ * out->camY);

	dzi = (int)dz;
	ds  = b->shade - a->shade;
	if (dzi != 0)
		ds /= dzi;
	out->shade = (int)((float)a->shade + span * (float)ds);

	t = span / dz;
	return t;
}

/*
 * Sutherland-Hodgman against the single plane camZ >= nearPlaneZ.
 *
 * THE UV INTERPOLATION IS CORRECT, and this used to say it was merely
 * probable. The original's UV work lives in
 * Screen3D_ClipPolygonViewport (0x00497080, was FUN_00497080), which
 * has now been read - and it does something different, for a reason
 * that turns out to justify what is done here.
 *
 * That function is the 2D VIEWPORT clipper. It works in SCREEN space,
 * where interpolating u and v linearly is wrong, so it premultiplies
 * both by 1/z, carries u/z, v/z and 1/z through four edge passes, and
 * divides back out at the end.
 *
 * THIS clipper works in CAMERA space, before the projection divide,
 * where the whole transform is still affine. There, u and v ARE linear
 * in the same parameter as the position, so a plain lerp with the t
 * that clip_edge_near returns is exact - no premultiply needed, and
 * adding one would be wrong.
 *
 * Two clippers, two spaces, two different correct answers. The
 * viewport one is not reproduced at all: D3D9 clips to the viewport
 * itself, perspective-correctly.
 */
/* ORIGIN  Screen3D_ClipPolygonViewport @ 0x00497080 */
int clip_polygon_near(const MR_Vertex *const *in, const float *inUV,
                      int n, MR_Vertex *out, float *outUV)
{
	int   count = 0;
	int   i;
	int   allIn = 1;
	int   anyIn = 0;

	if (!in || !inUV || !out || !outUV || n < 3 || n > 4)
		return 0;

	for (i = 0; i < n; i++) {
		if (in[i]->camZ >= g_nearPlaneZ)
			anyIn = 1;
		else
			allIn = 0;
	}
	if (!anyIn)
		return 0;               /* wholly behind - nothing to draw */
	if (allIn) {
		for (i = 0; i < n; i++) {
			out[i]        = *in[i];
			outUV[i * 2]     = inUV[i * 2];
			outUV[i * 2 + 1] = inUV[i * 2 + 1];
		}
		return n;
	}

	for (i = 0; i < n; i++) {
		const MR_Vertex *cur  = in[i];
		const MR_Vertex *next = in[(i + 1) % n];
		const float     *cuv  = &inUV[i * 2];
		const float     *nuv  = &inUV[((i + 1) % n) * 2];
		int curIn  = (cur->camZ  >= g_nearPlaneZ);
		int nextIn = (next->camZ >= g_nearPlaneZ);

		if (curIn) {
			if (count >= CLIP_MAX_CORNERS)
				return 0;
			out[count]           = *cur;
			outUV[count * 2]     = cuv[0];
			outUV[count * 2 + 1] = cuv[1];
			count++;
		}

		if (curIn != nextIn) {
			float t;

			if (count >= CLIP_MAX_CORNERS)
				return 0;
			/*
			 * The vertex IN FRONT is always the first argument, so the
			 * crossing is computed from the same end whichever way the
			 * edge is walked. That matters: the interpolation is not
			 * symmetric once the integer shade divide is involved.
			 */
			if (curIn)
				t = clip_edge_near(cur, next, &out[count]);
			else
				t = clip_edge_near(next, cur, &out[count]);

			if (curIn) {
				outUV[count * 2]     = cuv[0] + t * (nuv[0] - cuv[0]);
				outUV[count * 2 + 1] = cuv[1] + t * (nuv[1] - cuv[1]);
			} else {
				outUV[count * 2]     = nuv[0] + t * (cuv[0] - nuv[0]);
				outUV[count * 2 + 1] = nuv[1] + t * (cuv[1] - nuv[1]);
			}
			count++;
		}
	}

	return (count >= 3) ? count : 0;
}
