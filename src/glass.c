/*
 * glass.c - see glass.h.
 */

#include <windows.h>
#include <stdlib.h>
#include <string.h>

#include "glass.h"
#include "mesh.h"
#include "prim.h"
#include "bank.h"
#include "model.h"
#include "transform.h"
#include "cull.h"
#include "queue.h"
#include "drawscene.h"
#include "timing.h"
#include "audio.h"
#include "log.h"

/* The bank Menu_LoadResources creates empty at 0x0040aad1, purely to
 * hold the capture. Nothing loads it from a file. */
#define GLASS_BANK        2
#define GLASS_CAPTURE_W   0x100
#define GLASS_CAPTURE_H   0xc0

#define GLASS_MESH        "MENU\\MESHES\\VITRE.3DF"

/* 0x4b0 in Menu_Tick, and _DAT_004d02b4 is its reciprocal. */
#define GLASS_DURATION_MS 1200.0f

/*
 * THE VIRTUAL CLOCK, NOT THE REAL ONE, and the choice matters.
 *
 * The original drives this from timeGetTime, so the honest
 * transcription is tm_real_now. But the whole point of the virtual
 * clock is that SPACE and A can freeze and step the game, and an
 * effect that reads the wall clock ignores both - the shards would
 * keep tumbling while everything around them stood still, and a
 * 1200 ms effect is exactly the thing worth stepping through a frame
 * at a time.
 *
 * So this reads tm_now, like every other animation in the port. It is
 * a divergence in the sense that a paused game is not a state the
 * original has; when the clock is running normally the two are the
 * same clock and the behaviour is identical.
 */

/*
 * The spin amplitudes, 0x004d1848, 0x004d1840 and 0x004d1838, in the
 * order the code stores them. They are exact fractions of a turn -
 * 1.8pi, 1.5pi and 1.7pi, i.e. 0.90, 0.75 and 0.85 - which is what
 * says they were authored rather than tuned.
 */
#define GLASS_SPIN_0      5.654867f
#define GLASS_SPIN_1      4.712389f
#define GLASS_SPIN_2      5.340708f

/* 0x004d186c and 0x004d1870. The pane flies from camZ 1911 to 61,
 * through the 256 near plane and out past the viewer. */
#define GLASS_Z_RAMP      -1850.0f
#define GLASS_Z_BASE      1911.0f

/* 0x004d1828. */
#define GLASS_THIRD       0.3333333f

/* The rand() normaliser, 0x004d182c and 0x004d1830. RAND_MAX on this
 * compiler is the same 32767 the original was built with, but the
 * value is written out rather than taken from the header - the
 * original's arithmetic does not change if a library disagrees. */
#define GLASS_RAND_MAX    32767.0f

static MESH          s_mesh;
static int           s_meshOk;

static float        *s_verts;      /* shardCount * 9 floats            */
static MR_Primitive *s_prims;      /* shardCount * 2                   */
static float        *s_pos;        /* shardCount * 3, the centroids    */
static float        *s_rot;        /* shardCount * 3, the spin RATES   */
static int           s_shards;

static int    s_ready;
static int    s_requested;
static int    s_capturePending;
static int    s_active;
static double s_startMs;
static float  s_progress;

/* ------------------------------------------------------------------ */

static void glass_free_arrays(void)
{
	free(s_verts);  s_verts = NULL;
	free(s_prims);  s_prims = NULL;
	free(s_pos);    s_pos   = NULL;
	free(s_rot);    s_rot   = NULL;
	s_shards = 0;
}

/*
 * One shard's random spin rate on one axis, exactly as the original
 * spells it:
 *
 *     (rand() - 32767.0) * (1/32767.0) * amplitude
 *
 * NOTE THE RANGE IS [-amplitude, 0], NOT SYMMETRIC. rand() runs 0..
 * RAND_MAX, so subtracting RAND_MAX can only give a negative result -
 * every shard spins the SAME WAY and only the rate varies. Reading it
 * as a centred random and writing (rand()/RAND_MAX - 0.5) would give
 * a tumble that is half as fast and goes both ways.
 */
static float glass_spin_rate(float amplitude)
{
	return ((float)rand() - GLASS_RAND_MAX) * (1.0f / GLASS_RAND_MAX) *
	       amplitude;
}

/*
 * MenuGlass_BuildShards (0x00479630). Cuts the loaded pane into one
 * independent triangle per face.
 *
 * The original rewrites the MESH in place - it frees the loaded
 * vertex and face arrays and swaps its own in, leaving faceCount at N
 * while the face array holds 2N records. We keep the arrays beside
 * the mesh instead. That is a deliberate difference: the original's
 * layout is what forces its draw loop to step the face pointer by two
 * against a count of N, and there is no reason to reproduce a trap.
 */
/* ORIGIN  MenuGlass_BuildShards @ 0x00479630 */
static int glass_build_shards(void)
{
	int i;

	s_shards = s_mesh.faceCount;
	if (s_shards <= 0)
		return 0;

	s_verts = (float *)malloc((size_t)s_shards * 9 * sizeof(float));
	s_prims = (MR_Primitive *)malloc((size_t)s_shards * 2 *
	                                 sizeof(MR_Primitive));
	s_pos   = (float *)malloc((size_t)s_shards * 3 * sizeof(float));
	s_rot   = (float *)malloc((size_t)s_shards * 3 * sizeof(float));
	if (!s_verts || !s_prims || !s_pos || !s_rot) {
		glass_free_arrays();
		return 0;
	}
	memset(s_prims, 0, (size_t)s_shards * 2 * sizeof(MR_Primitive));

	for (i = 0; i < s_shards; i++) {
		const MESH_FACE *f  = &s_mesh.faces[i];
		float           *v  = &s_verts[i * 9];
		float           *p  = &s_pos[i * 3];
		float           *r  = &s_rot[i * 3];
		MR_Primitive    *pa = &s_prims[i * 2];
		MR_Primitive    *pb = &s_prims[i * 2 + 1];
		float            uv[6];
		float            cx = 0.0f;
		float            cy = 0.0f;
		int              k;

		/* 1. de-share: the shard gets its own copy of three vertices */
		for (k = 0; k < 3; k++) {
			const MESH_VERTEX *src = &s_mesh.vertices[f->index[k]];

			v[k * 3]     = src->x;
			v[k * 3 + 1] = src->y;
			v[k * 3 + 2] = src->z;
			cx += src->x;
			cy += src->y;
		}

		/* 2. the centroid in X and Y only, and it IS the shard's
		 *    position for the rest of the effect. */
		cx *= GLASS_THIRD;
		cy *= GLASS_THIRD;
		p[0] = cx;
		p[1] = cy;
		p[2] = 0.0f;

		/* 3. three spin rates. See the note on glass_spin_rate. The
		 *    original draws them in the order 2, 1, 0 and stores them
		 *    0, 1, 2 - the amplitudes below are already in storage
		 *    order, but the CALL order decides which rand() each axis
		 *    gets, so it is kept. */
		r[2] = glass_spin_rate(GLASS_SPIN_2);
		r[1] = glass_spin_rate(GLASS_SPIN_1);
		r[0] = glass_spin_rate(GLASS_SPIN_0);

		/*
		 * 4. UVs from each vertex's own position, BEFORE recentring,
		 *    and truncated to whole texels the way __ftol truncates.
		 *
		 *    u spans 0..256 and v spans 192..0 - v is FLIPPED, because
		 *    the capture is stored top-down while y runs up. That is
		 *    the 256x192 the capture writes, which is how two
		 *    separately read functions confirm each other.
		 */
		for (k = 0; k < 3; k++) {
			uv[k * 2] = (float)(int)((v[k * 3] + 320.0f) *
			                         256.0f * (1.0f / 640.0f));
			uv[k * 2 + 1] = (float)(int)((v[k * 3 + 1] + 240.0f) *
			                             192.0f * (-1.0f / 480.0f) +
			                             192.0f);
		}

		/* 5. recentre on the centroid and flatten z, so the shard
		 *    spins about itself and the pane is exactly planar. */
		for (k = 0; k < 3; k++) {
			v[k * 3]     -= cx;
			v[k * 3 + 1] -= cy;
			v[k * 3 + 2]  = 0.0f;
		}

		/*
		 * 6. TWO primitives over the same three vertices, wound both
		 *    ways, so a shard stays visible while it tumbles. The
		 *    single-sided cull would otherwise drop it for half of
		 *    every rotation.
		 *
		 *    Kind 0x10 and colour 0xffff are the literals the original
		 *    passes; 0xffff unpacks through the 5-6-5 expansion to
		 *    0xf8f8f8, which drawscene then floors with an opaque
		 *    alpha byte.
		 */
		pa->kind = pa->effectiveKind = 0x10;
		pa->colorIndex = 0xffff;
		pa->flatColor  = 0x00f8f8f8u;
		pa->bankIndex  = GLASS_BANK;
		pa->vertexIndex[0] = 0;
		pa->vertexIndex[1] = 1;
		pa->vertexIndex[2] = 2;
		pa->uv[0] = uv[0]; pa->uv[1] = uv[1];
		pa->uv[2] = uv[2]; pa->uv[3] = uv[3];
		pa->uv[4] = uv[4]; pa->uv[5] = uv[5];

		*pb = *pa;
		pb->vertexIndex[1] = 2;
		pb->vertexIndex[2] = 1;
		pb->uv[2] = uv[4]; pb->uv[3] = uv[5];
		pb->uv[4] = uv[2]; pb->uv[5] = uv[3];

		/* The triangle wrap convention: slot 3 repeats slot 0 and
		 * slot 4 is the marker that says "three, not four". */
		pa->vertexIndex[3] = pa->vertexIndex[0];
		pb->vertexIndex[3] = pb->vertexIndex[0];
		pa->vertexIndex[4] = pb->vertexIndex[4] = PRIM_NO_WRAP;
		pa->uv[6] = pa->uv[0]; pa->uv[7] = pa->uv[1];
		pb->uv[6] = pb->uv[0]; pb->uv[7] = pb->uv[1];
	}
	return 1;
}

/* ------------------------------------------------------------------ */

int glass_init(void)
{
	glass_shutdown();

	/*
	 * Load3df, NOT Load3dfAndMap - the original calls 0x00420590 at
	 * 0x0040abb4 with applyScale 0. Two consequences worth naming:
	 * the pane keeps its authored 640.8 x 480.3 size, and it is NOT
	 * mirrored, because Mesh_MirrorXAndFlipWinding runs only in the
	 * paired loaders. The build below discards the loaded faces and
	 * makes its own primitives anyway, so the winding question never
	 * arises here.
	 */
	if (!mesh_load(&s_mesh, GLASS_MESH, 1.0f)) {
		mrlog_err("glass: %s did not load\n", GLASS_MESH);
		return 0;
	}
	s_meshOk = 1;

	if (!glass_build_shards()) {
		mrlog_err("glass: %s produced no shards\n", GLASS_MESH);
		glass_shutdown();
		return 0;
	}

	/*
	 * The capture bank. wantKey is 0 here, unlike every menu page
	 * bank: Menu_LoadResources passes 0 where page 25 passes 1, and it
	 * has to - a screenshot is opaque and keying its blacks would
	 * punch holes in the shards.
	 */
	if (!bank_create_empty(GLASS_BANK, GLASS_CAPTURE_W, GLASS_CAPTURE_H)) {
		mrlog_err("glass: bank %d could not be created\n", GLASS_BANK);
		glass_shutdown();
		return 0;
	}
	bank_alloc_texture_slot(GLASS_BANK, 0, 0, 0, 16);

	s_ready = 1;
	mrlog("glass: %d shards from %s\n", s_shards, GLASS_MESH);
	return 1;
}

void glass_shutdown(void)
{
	glass_free_arrays();
	if (s_meshOk) {
		mesh_free(&s_mesh);
		s_meshOk = 0;
	}
	s_ready = s_requested = s_capturePending = s_active = 0;
	s_progress = 0.0f;
}

int glass_ready(void)  { return s_ready; }
int glass_active(void) { return s_active; }

void glass_request(void)
{
	if (!s_ready)
		return;
	s_requested      = 1;
	s_capturePending = 1;
}

void glass_capture_if_pending(void)
{
	if (!s_capturePending)
		return;
	s_capturePending = 0;

	if (!bank_capture_screen(GLASS_BANK)) {
		/*
		 * The page keeps its previous contents, so the shatter shows a
		 * stale screen rather than garbage. Worth a line, because the
		 * symptom - a transition wearing the wrong picture - is
		 * otherwise very hard to attribute.
		 */
		static int warned;

		if (!warned) {
			warned = 1;
			mrlog_err("glass: screen capture failed; the shatter will "
			          "wear a stale image\n");
		}
	}
}

/*
 * Menu_DrawTransitionEffect (0x00479c10), per shard.
 *
 * NO VIEW MATRIX AND NO CAMERA. The original builds the matrix from
 * the shard's own euler triple and its centroid and hands it straight
 * to Screen3D_TransformVertsFlat, so the shards live in CAMERA SPACE.
 * Reaching for the menu camera here produces a pane that is the wrong
 * size and in the wrong place, and looks like a projection bug.
 */
/* ORIGIN  Menu_DrawTransitionEffect @ 0x00479c10 */
static void glass_draw(void)
{
	float zBase = s_progress * GLASS_Z_RAMP + GLASS_Z_BASE;
	int   i;

	queue_reset();
	vertexpool_reset();

	for (i = 0; i < s_shards; i++) {
		const float *p = &s_pos[i * 3];
		const float *r = &s_rot[i * 3];
		float        angles[3];
		float        m[12];
		MR_Vertex   *verts;

		angles[0] = r[0] * s_progress;
		angles[1] = r[1] * s_progress;
		angles[2] = r[2] * s_progress;

		mat34_from_euler(m, angles);
		m[9]  = p[0];
		m[10] = p[1];
		m[11] = zBase + p[2];

		verts = transform_verts_flat(m, &s_verts[i * 9], 3, 0x1f);
		if (!verts)
			continue;

		/*
		 * The original calls Screen3D_CullAndQueueFacesEx with
		 * sortByFarthest = 1, which is bit for bit what
		 * Screen3D_CullAndQueueFaces always does. Its two other
		 * differences - the unconditional kind-0x0d skip and the
		 * depth-bias flag - cannot fire on a kind-0x10 face with no
		 * flags, so the plain submitter is the same function here.
		 */
		cull_and_queue_faces(verts, &s_prims[i * 2], 2, -1);
	}

	/*
	 * POINT SAMPLING, AGAINST THE FEATURE TABLE, AND DELIBERATELY.
	 *
	 * Kind 0x10's feature byte is 0x81 - bit 3 clear - so the table
	 * asks for bilinear. Taken literally that magnifies a 256x192
	 * capture 2.5x with linear filtering, and the shatter comes out
	 * looking like somebody resampled the screenshot. It is soft in
	 * exactly the way retail is not.
	 *
	 * o THE FILTER BIT IS THE D3D PATH'S ANSWER, AND THE D3D PATH
	 *   NEVER SHOWED THIS EFFECT. Retail's hardware shatter is black:
	 *   MenuTools_CaptureScreenToBank reads the SOFTWARE framebuffer,
	 *   which on the hardware path holds only the 2D layer, so the
	 *   shards carry nothing. See menu-pages.txt 4.3.1.4 - it is a
	 *   real bug and it is visible in retail footage.
	 *
	 * o SO WHAT PLAYERS ACTUALLY SAW IS THE SOFTWARE RASTERISER'S,
	 *   and that is point sampled. We already decline to reproduce
	 *   the black-shatter bug, which means we show the software
	 *   path's CONTENT; showing it with the hardware path's FILTER is
	 *   a combination that shipped on neither. Point matches what the
	 *   effect looked like.
	 *
	 * Blocky is correct here. The capture is 256x192 by design -
	 * 2-of-every-5 nearest neighbour, read out of the assembly - so
	 * every texel is a real screen pixel and the edges should be
	 * crisp rather than smeared.
	 */
	drawscene_force_point_filter(1);
	queue_walk(drawscene_dispatch);
	drawscene_force_point_filter(0);
}

void glass_update(void)
{
	double now;

	if (!s_ready)
		return;

	/*
	 * The arm is a SEPARATE PASS from the request, and that is the
	 * original's shape: Menu_BeginTransition only raises the flag and
	 * Menu_Tick stamps the window later. Doing both at once would
	 * start the clock a frame before the captured image exists.
	 */
	if (s_requested && !s_capturePending) {
		s_requested = 0;
		s_active    = 1;
		s_startMs   = tm_now() * 1000.0;
		s_progress  = 0.0f;

		/*
		 * THE SHATTER SOUND FIRES HERE, WITH THE ARM, and that is
		 * where the original puts it too - Menu_Tick 0x00409d65,
		 * inside the same branch that stamps the 1200 ms window and
		 * immediately before it clears the request [verified]. NOT in
		 * Menu_BeginTransition, which makes no sound call at all.
		 *
		 * The distinction is worth keeping: the request and the arm
		 * are a frame apart, so putting the cue on the request would
		 * play the sound one frame before the glass exists.
		 *
		 * o IT IS NOT A GLASS SOUND. 0x2a is Menu\valide.wav -
		 *   "confirm" - and it fires on EVERY page transition, of
		 *   which the shatter is merely the visual. SFX.BKF has no
		 *   glass-named sample at all. Named SND_TRANSITION for that
		 *   reason; see audio.h.
		 */
		audio_cue(SND_TRANSITION, 0);
	}

	if (!s_active)
		return;

	now = tm_now() * 1000.0 - s_startMs;
	if (now >= (double)GLASS_DURATION_MS) {
		s_active = 0;
		return;
	}
	s_progress = (float)(now / (double)GLASS_DURATION_MS);
	glass_draw();
}
