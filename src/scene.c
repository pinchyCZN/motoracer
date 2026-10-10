/*
 * scene.c - MR_MenuSceneObject. See scene.h and
 * docs/structs/MR_MenuSceneObject.txt.
 */

#include <windows.h>
#include <mmsystem.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

#include "scene.h"
#include "mesh.h"
#include "trig.h"
#include "log.h"

/*
 * The easing constants, read out of the binary at 0x004d1880,
 * 0x004d1888, 0x004d1890 and 0x004d1898: pi, pi/2, 1.0 and 0.5, with
 * the duration multiplier 1000.0 at 0x004d18a8.
 *
 * THEY ARE THE SPRITE'S, byte for byte - the 2D and 3D menu layers
 * share one easing model. Restated here rather than pulled out of
 * sprite.c because the two are separate constant pools in the
 * original and folding them would assert a relationship the binary
 * does not have.
 */
#define SCENE_SECONDS_TO_MS 1000.0

/* 0x004d1aa0 holds MR_TWO_PI's own eight bytes; the wrap limit
 * for mode 2 reads it as a float. */
#define SCENE_TWO_PI        ((float)MR_TWO_PI)

/* 0x004d18c8, the -0.5 that turns a bounds sum into a centre. */
#define SCENE_HALF_NEG     -0.5f

int scene_object_load(MR_SceneObject *o, const char *mesh3df,
                      const char *meshMap)
{
	if (!o)
		return 0;
	memset(o, 0, sizeof(*o));

	o->mesh = (MESH *)calloc(1, sizeof(MESH));
	if (!o->mesh) {
		mrlog_err("scene: out of memory for %s\n",
		          mesh3df ? mesh3df : "(null)");
		return 0;
	}

	/*
	 * MESH_SCALE_3DF_MAP, NOT ZERO, on the paired path - Load3dfAndMap
	 * always applies its own 96.0 and Load3df applies its argument
	 * only when non-zero. Passing 0 here would load every glyph 96
	 * times too small, which is sub-pixel and indistinguishable from
	 * not drawing at all. mesh.h has the long version.
	 */
	if (meshMap) {
		if (!mesh_load_with_map(o->mesh, mesh3df, meshMap,
		                        MESH_SCALE_3DF_MAP))
		    goto fail;
	} else {
		if (!mesh_load(o->mesh, mesh3df, 0.0f))
			goto fail;
	}

	/* CreateSceneObject ends with Mesh_CenterOnBounds. */
	mesh_center_on_bounds(o->mesh, NULL);
	o->loaded = 1;
	return 1;

fail:
	free(o->mesh);
	o->mesh = NULL;
	return 0;
}

void scene_object_free(MR_SceneObject *o)
{
	if (!o)
		return;
	if (o->mesh) {
		if (o->loaded)
			mesh_free(o->mesh);
		free(o->mesh);
		o->mesh = NULL;
	}
	o->loaded = 0;
}

void scene_set_vec(MR_SceneObject *o, const float v[3])
{
	if (!o || !v)
		return;
	o->vec[0]    = v[0];
	o->vec[1]    = v[1];
	o->vec[2]    = v[2];
	o->vecActive = 0;
}

void scene_set_target(MR_SceneObject *o, const float v[3])
{
	if (!o || !v)
		return;
	o->target[0]    = v[0];
	o->target[1]    = v[1];
	o->target[2]    = v[2];
	o->targetActive = 0;
}

void scene_set_vec_pair(MR_SceneObject *o, const float vec[3],
                        const float target[3])
{
	scene_set_vec(o, vec);
	scene_set_target(o, target);
}

/*
 * The two starters are one function with two sets of fields, which is
 * what the struct doc means by "one layout, twice". Kept as two so a
 * reader can put either next to its original without a translation
 * step.
 */
void scene_start_vec_move(MR_SceneObject *o, const float to[3],
                          float durationSec, int mode)
{
	int k;

	if (!o || !to)
		return;

	o->vecStartMs    = timeGetTime();
	o->vecDurationMs = (unsigned int)((double)durationSec *
	                                  SCENE_SECONDS_TO_MS);
	o->vecEndMs      = o->vecStartMs + o->vecDurationMs;

	for (k = 0; k < 3; k++) {
		o->vecStart[k] = o->vec[k];
		/* A DESTINATION FOR 0 AND 1, A RATE FOR 2. See scene.h. */
		o->vecDelta[k] = (mode == SCENE_MODE_SPIN)
		               ? to[k] : to[k] - o->vec[k];
	}
	o->vecMode   = mode;
	o->vecActive = 1;
}

void scene_start_target_move(MR_SceneObject *o, const float to[3],
                             float durationSec, int mode)
{
	int k;

	if (!o || !to)
		return;

	o->targetStartMs    = timeGetTime();
	o->targetDurationMs = (unsigned int)((double)durationSec *
	                                     SCENE_SECONDS_TO_MS);
	o->targetEndMs      = o->targetStartMs + o->targetDurationMs;

	for (k = 0; k < 3; k++) {
		o->targetStart[k] = o->target[k];
		o->targetDelta[k] = (mode == SCENE_MODE_SPIN)
		                  ? to[k] : to[k] - o->target[k];
	}
	o->targetMode   = mode;
	o->targetActive = 1;
}

/*
 * WRAP INTO +/- 2*PI. The original does this by repeated subtraction,
 * with the negative case handled by negating, looping and negating
 * back - cheap while the angle stays near range and unbounded if it
 * ever does not. fmod is better behaved and marginally different, and
 * the difference is recorded rather than hidden: for the angles the
 * menu actually produces the two agree.
 */
static float scene_wrap(float a)
{
	if (a >= SCENE_TWO_PI || a <= -SCENE_TWO_PI)
		a = (float)fmod((double)a, (double)SCENE_TWO_PI);
	return a;
}

/*
 * One channel of the tick. Returns nothing; clears active itself.
 *
 * THE CLOCK IS CLAMPED, NOT THE INTERPOLANT, and completion is
 * detected by comparing the clamped clock to endMs rather than by
 * testing t against 1.0 - the same shape as sprite_update_move, and
 * the reason the last frame lands exactly on the destination.
 */
static void scene_tick_channel(float cur[3], const float start[3],
                               const float delta[3],
                               unsigned int startMs, unsigned int endMs,
                               unsigned int durationMs, int mode,
                               int *active)
{
	unsigned int now = timeGetTime();
	double t;
	double e;
	int    k;

	if (mode == SCENE_MODE_SPIN) {
		/*
		 * MODE 2 IGNORES endMs AND NEVER CLEARS active. delta is an
		 * angular rate, so this is a free-running spin - the turning
		 * logo, the rotating bike - and it is why vecRebase exists,
		 * to restart the timebase without stopping the motion.
		 */
		t = (double)(now - startMs) / SCENE_SECONDS_TO_MS;
		for (k = 0; k < 3; k++)
			cur[k] = scene_wrap(start[k] + delta[k] * (float)t);
		return;
	}

	if (now > endMs)
		now = endMs;

	/* DIVERGENCE: the original divides unguarded, so a zero duration
	 * gives 0/0 and a NaN. Treat it as already finished. */
	if (durationMs == 0)
		t = 1.0;
	else
		t = (double)(now - startMs) / (double)durationMs;

	if (mode == SCENE_MODE_EASE)
		e = (sin(t * MR_PI - MR_HALF_PI) + 1.0) * 0.5;
	else if (mode == SCENE_MODE_LINEAR)
		e = t;
	else
		return;                    /* unknown mode: do not move */

	for (k = 0; k < 3; k++)
		cur[k] = start[k] + delta[k] * (float)e;

	if (now == endMs)
		*active = 0;
}

void scene_update(MR_SceneObject *o)
{
	if (!o)
		return;

	/*
	 * The rebase, which applies to the vec channel only and only in
	 * spin mode: it restarts the timebase so a spin can be re-anchored
	 * without a discontinuity.
	 */
	if (o->vecRebase && o->vecMode == SCENE_MODE_SPIN) {
		o->vecRebase  = 0;
		o->vecStartMs = timeGetTime();
	}

	if (o->vecActive)
		scene_tick_channel(o->vec, o->vecStart, o->vecDelta,
	                       o->vecStartMs, o->vecEndMs, o->vecDurationMs,
	                       o->vecMode, &o->vecActive);

	if (o->targetActive)
		scene_tick_channel(o->target, o->targetStart, o->targetDelta,
	                       o->targetStartMs, o->targetEndMs,
	                       o->targetDurationMs, o->targetMode,
	                       &o->targetActive);
}

/* Bounds of the mesh, or 0 if there is nothing to measure. */
static int scene_mesh_bounds(const MESH *m, float lo[3], float hi[3])
{
	int i, k;

	if (!m || m->vertexCount <= 0)
		return 0;
	for (k = 0; k < 3; k++) {
		lo[k] =  FLT_MAX;
		hi[k] = -FLT_MAX;
	}
	for (i = 0; i < m->vertexCount; i++) {
		float v[3];

		v[0] = m->vertices[i].x;
		v[1] = m->vertices[i].y;
		v[2] = m->vertices[i].z;
		for (k = 0; k < 3; k++) {
			if (v[k] < lo[k]) lo[k] = v[k];
			if (v[k] > hi[k]) hi[k] = v[k];
		}
	}
	return 1;
}

static void scene_centre_and_scale(MESH *m, const float lo[3],
                                   const float hi[3], float scale)
{
	float centre[3];
	int   i, k;

	for (k = 0; k < 3; k++)
		centre[k] = (hi[k] + lo[k]) * SCENE_HALF_NEG;

	for (i = 0; i < m->vertexCount; i++) {
		m->vertices[i].x = (m->vertices[i].x + centre[0]) * scale;
		m->vertices[i].y = (m->vertices[i].y + centre[1]) * scale;
		m->vertices[i].z = (m->vertices[i].z + centre[2]) * scale;
	}
}

float scene_normalise_mesh_to_size(MR_SceneObject *o, float half)
{
	float lo[3], hi[3];
	float scale;
	int   k;

	if (!o || !o->mesh || !scene_mesh_bounds(o->mesh, lo, hi))
		return 1.0f;

	scale = FLT_MAX;
	for (k = 0; k < 3; k++) {
		/* A ZERO EXTENT BECOMES 1, so a flat mesh does not divide by
		 * zero and simply does not constrain the fit. */
		float ext = (hi[k] - lo[k]) * 0.5f;
		float s;

		if (ext == 0.0f)
			ext = 1.0f;
		s = half / ext;
		if (s < scale)             /* THE SMALLEST RATIO WINS */
		    scale = s;
	}

	scene_centre_and_scale(o->mesh, lo, hi, scale);
	return scale;
}

void scene_scale_mesh_by(MR_SceneObject *o, float scale)
{
	float lo[3], hi[3];

	if (!o || !o->mesh || !scene_mesh_bounds(o->mesh, lo, hi))
		return;
	scene_centre_and_scale(o->mesh, lo, hi, scale);
}

void scene_mirror_mesh(MR_SceneObject *o)
{
	if (!o || !o->mesh)
		return;
	mesh_mirror_x_flip_winding(o->mesh);
}
