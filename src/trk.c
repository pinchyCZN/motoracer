/*
 * trk.c - the track: Trk_Load (0x0040d650) and the geometry queries
 * that run against what it builds. Also AnimDecor (animDecor.cpp) and
 * the Plane_* helpers, which live in the same pool as the track code
 * rather than in a file of their own.
 *
 * The format is docs/formats/trk.txt; tools/trk.py is the reference
 * reader and the two agree section for section. The loader is a
 * memory-buffer reader over the asset, with every take() bounds
 * checked, and a final assertion that the parse landed exactly on the
 * end of the file.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

#include "trk.h"
#include "menu.h"       /* g_reverseEnabled - the search runs
                         * the way the bike does */

/*
 * ONE CONSTANT, TWO NAMES. Both are 0x004d2090 - 0x3727c5ac,
 * 9.9999997e-06 - the degeneracy guard Plane_IntersectSegment tests
 * each component of (to - from) against before dividing.
 *
 * o IT IS 1e-5, NOT 1e-6. The two look alike at a glance and 1e-6
 *   makes the test ten times stricter about calling a segment
 *   degenerate. 1e-6 would be 0x358637bd.
 *
 * o THE SECOND NAME EXISTS BECAUSE WE INLINED THE CALLEE.
 *   trk_contain_point does the intersect arithmetic itself where
 *   Trk_ContainPointInSegment calls Plane_IntersectSegment, so our
 *   copy needs the guard the callee would have applied. They cannot
 *   drift apart in the original - there is only the one.
 *
 * o THE CLIP'S 0x004d02c8 IS A SECOND COPY OF THE VALUE, in a
 *   different pool - CLIP_MOVE_EPS below. So the two guards are
 *   redundant in the original too, which is why nothing broke
 *   visibly. There is no third copy.
 */
#define TRK_PLANE_EPS   1e-5
#define TRK_CONTAIN_EPS 1e-5
#include "assets.h"
#include "transform.h"
#include "model.h"
#include "cull.h"
#include "log.h"
#include "detail.h"

/* Sanity bounds. The original has none of these - it trusts the file -
 * but a corrupt count here allocates gigabytes or walks off the end,
 * and every shipped track is orders of magnitude under them. */
#define TRK_MAX_COUNT (1 << 22)

typedef struct {
	const unsigned char *d;
	unsigned int         size;
	unsigned int         p;
	int                  bad;
} TrkReader;

/*
 * ORIGIN  NONE - ours. The original reads through a CArchive-like
 *         vtable call, so there is no buffer cursor to match
 */
static const void *rd_take(TrkReader *r, unsigned int n)
{
	const void *at;

	if (r->bad)
		return NULL;
	if (n > r->size || r->p > r->size - n) {
		r->bad = 1;
		return NULL;
	}
	at = r->d + r->p;
	r->p += n;
	return at;
}

/* ORIGIN  NONE - ours */
static unsigned int rd_u32(TrkReader *r)
{
	const void *at = rd_take(r, 4);
	unsigned int v;

	if (!at)
		return 0;
	memcpy(&v, at, 4);
	return v;
}

/* ORIGIN  NONE - ours. The bound is ours too - see TRK_MAX_COUNT */
static int rd_count(TrkReader *r)
{
	unsigned int v = rd_u32(r);

	if (r->bad)
		return 0;
	if (v > TRK_MAX_COUNT) {
		r->bad = 1;
		return 0;
	}
	return (int)v;
}

/* ORIGIN  NONE - ours */
static void rd_floats(TrkReader *r, float *out, int n)
{
	const void *at = rd_take(r, (unsigned int)n * 4);

	if (at)
		memcpy(out, at, (size_t)n * 4);
}

/*
 * ORIGIN  Trk_ReadFace @ 0x00423240, whole
 *
 * A face. THE FILE ORDER IS NOT THE STRUCT ORDER, which is why this
 * is a function rather than one memcpy - Trk_ReadFace reads the 58
 * bytes in file order into six scattered struct offsets. The offsets
 * below are FILE offsets; trk.h has the mapping.
 */
static void rd_face(TrkReader *r, MR_Primitive *f)
{
	const unsigned char *b = (const unsigned char *)rd_take(r, 58);
	unsigned short       c;
	int                  k;

	if (!b)
		return;

	memcpy(&f->kind,       b + 0, 2);
	memcpy(&f->flags,      b + 2, 2);
	memcpy(&c,             b + 4, 2);
	memcpy(&f->bankIndex,  b + 6, 2);
	for (k = 0; k < 4; k++)
		memcpy(&f->vertexIndex[k], b + 8 + k * 2, 2);
	memcpy(&f->vertexIndex[4], b + 16, 2);   /* 0xffff = PRIM_NO_WRAP */
	memcpy(f->uv,          b + 18, 40);

	f->colorIndex    = c;
	f->effectiveKind = f->kind;

	/*
	 * THE COLOUR EXPANSION, from Trk_ReadFace @ 0x00423240 - the same
	 * function the version gate below reads. Five bits a channel
	 * shifted up three to fill eight - and the bit positions are the
	 * engine's own 16-bit layout, red at 11 and green at 6, NOT
	 * RGB555. trk.txt 3.1 records why that distinction matters.
	 */
	f->flatColor = (unsigned int)((((c & 0x7c0) >> 6) << 8 |
	                               (unsigned int)(c >> 11) << 16 |
	                               (c & 0x1f)) << 3);
}

/* ORIGIN  Trk_ReadMesh @ 0x00422a20, whole */
static int rd_mesh(TrkReader *r, MR_TrkMesh *m, int version, int keep)
{
	int i;

	memset(m, 0, sizeof(*m));

	m->vertexCount = rd_count(r);
	if (r->bad)
		return 0;

	if (keep && m->vertexCount > 0) {
		m->verts = (float *)malloc((size_t)m->vertexCount * 3 *
		                           sizeof(float));
		m->vertExtra = (unsigned int *)malloc((size_t)m->vertexCount *
		                                      sizeof(unsigned int));
		if (!m->verts || !m->vertExtra) {
			r->bad = 1;
			return 0;
		}
	}
	for (i = 0; i < m->vertexCount; i++) {
		const unsigned char *v = (const unsigned char *)rd_take(r, 16);

		if (!v)
			return 0;
		if (keep) {
			memcpy(m->verts + i * 3, v, 12);
			memcpy(m->vertExtra + i, v + 12, 4);
		}
	}

	/*
	 * VERSION 6 WOULD CHANGE THE FACE RECORD, and WE REFUSE WHERE THE
	 * ORIGINAL DOES NOT. Trk_ReadFace's whole body is under
	 * "if (version < 6)", so on 6 it returns having read nothing and
	 * WITHOUT ADVANCING THE READER - every following field then comes
	 * out of the wrong bytes. Failing the load is the honest version
	 * of that, and nothing models what 6 changes anyway.
	 */
	if (version >= 6) {
		r->bad = 1;
		return 0;
	}

	m->primCount = rd_count(r);
	if (r->bad)
		return 0;

	if (keep && m->primCount > 0) {
		m->prims = (MR_Primitive *)calloc((size_t)m->primCount,
		                                  sizeof(MR_Primitive));
		if (!m->prims) {
			r->bad = 1;
			return 0;
		}
	}
	for (i = 0; i < m->primCount; i++) {
		if (keep) {
			rd_face(r, &m->prims[i]);
		} else if (!rd_take(r, 58)) {
			return 0;
		}
		if (r->bad)
			return 0;
	}
	return 1;
}

static void free_mesh(MR_TrkMesh *m)
{
	free(m->verts);
	free(m->vertExtra);
	free(m->prims);
	memset(m, 0, sizeof(*m));
}

/*
 * ORIGIN  NONE - ours
 *
 * A length-prefixed name, truncated to size - 1; callers pass
 * TRK_NAME_MAX, which is the loader's own 15-character limit.
 */
static void rd_name(TrkReader *r, char *out, int size)
{
	int         n = rd_count(r);
	const char *s;

	out[0] = '\0';
	if (r->bad)
		return;
	s = (const char *)rd_take(r, (unsigned int)n);
	if (!s)
		return;
	if (n > size - 1)
		n = size - 1;
	memcpy(out, s, (size_t)n);
	out[n] = '\0';
}

/*
 * ORIGIN  AnimDecor_Load @ 0x0048b7e0, animDecor.cpp
 *
 * The records are kept and the walk is byte-exact, so trk_load's
 * landed-on-the-end assertion covers this too.
 *
 * IT CREATES NOTHING. Each record names an entry of instances[] by
 * index and the animator drives that entry's rot[] and pos[]; see
 * MR_AnimDecor in trk.h and docs/subsystems/anim-decor.txt.
 *
 * The tag has already been consumed by the caller.
 */
static int trk_load_anim_decor(TrkReader *r, MR_Trk *out)
{
	unsigned int sub;
	int          i;

	sub = rd_u32(r);
	if (r->bad)
		return 0;
	if (sub == 0)
		return 1;               /* tag present, nothing follows */

	out->animDecorObjects = rd_count(r);
	if (r->bad)
		return 0;

	/*
	 * THE TWO LIMITS ARE THE LOADER'S OWN - 63 and 79 - and it aborts
	 * the game with a fatal message box on either. Ours fails the
	 * load instead.
	 *
	 * "TROP DE FRAMES DANS L'ANIM" MISNAMES WHAT IT GUARDS: 0x4f is
	 * tested against the FIRST count, pathCount, and the frame count
	 * is never checked at all.
	 */
	if (out->animDecorObjects > 63) {
		r->bad = 1;
		return 0;
	}

	/*
	 * THE RECORD, 0x0048b95c..0x0048ba2c. Eight dwords when the
	 * subversion is 1 and sixteen otherwise, the rest zero -
	 * the original memsets sixteen before every read.
	 *
	 *     0  packed:  type = >>16 & 0xff, axis = >>24,
	 *                 decor index = & 0xffff
	 *     1  the time seed        x 0.1
	 *     2  lo   3  hi   4  phase
	 *     5  the period           x 0.1
	 *     6..8   position FROM    9..11  position TO
	 *
	 * TYPE 8 SCALES ITS PHASE TOO, at 0x0048ba0e - it is a hold
	 * time there rather than an angle, so it wants the same
	 * unit as the period.
	 */
	for (i = 0; i < out->animDecorObjects; i++) {
		const unsigned char *rec;
		unsigned int         first, kind;
		float                f[16];
		MR_AnimDecor        *ad = NULL;
		int                  k;

		rec = (const unsigned char *)rd_take(r, (sub == 1) ? 0x20 : 0x40);
		if (!rec)
			return 0;
		memcpy(&first, rec, 4);
		kind = (first >> 16) & 0xff;    /* bits 16..23 */

		memset(f, 0, sizeof f);
		memcpy(f, rec, (sub == 1) ? 0x20 : 0x40);

		if (out->animDecorCount < TRK_ANIMDECOR_MAX) {
			ad = &out->animDecor[out->animDecorCount++];
			memset(ad, 0, sizeof *ad);
			ad->type       = (int)kind;
			ad->axis       = (int)(first >> 24);
			ad->decorIndex = (int)(first & 0xffff);
			ad->time       = f[1] * TRK_ANIM_TIME_SCALE;
			ad->lo         = f[2];
			ad->hi         = f[3];
			ad->phase      = f[4];
			ad->period     = f[5] * TRK_ANIM_TIME_SCALE;
			for (k = 0; k < 3; k++) {
				ad->from[k] = f[6 + k];
				ad->to[k]   = f[9 + k];
			}
			if (ad->type == 8)
				ad->phase *= TRK_ANIM_TIME_SCALE;
		}

		if (sub > 2 && kind == 7) {
			unsigned int paths = (unsigned int)rd_count(r);
			unsigned int frames;
			const void  *block;

			if (r->bad)
				return 0;
			if (paths == 0)
				continue;
			if (paths > 79) {      /* 0x4f, and it is PATHS */
				r->bad = 1;
				return 0;
			}
			frames = (unsigned int)rd_count(r);
			if (r->bad)
				return 0;
			block = rd_take(r, paths * frames * 12);
			if (!block)
				return 0;
			if (ad) {
				ad->pathCount  = (int)paths;
				ad->frameCount = (int)frames;
				ad->path = (float *)malloc(paths * frames * 12);
				if (ad->path)
					memcpy(ad->path, block,
				           paths * frames * 12);
			}
		}
	}
	return 1;
}

/*
 * ORIGIN  the AnimDecor animator @ 0x0048b370, animDecor.cpp
 *
 * ONE OBJECT, ONE FRAME. The caller at 0x0048b7a0 - which
 * maybe_Game_Tick runs at 0x0047d877 - walks the list and calls this
 * for each.
 *
 * IT DRIVES AN ENTRY OF instances[], nothing of its own: an angle
 * into rot[axis] (the original's instance +0x14, +0x18, +0x1c) or a
 * position into pos[] (+0x20, +0x24, +0x28). The mesh is already
 * loaded and already drawn; this is the only thing that was missing.
 *
 * THE WRAP IS AN fmod WRITTEN LONG. The original computes
 * time - (int)(time / period) * period, which the decompiler renders
 * as an algebraic zero because it loses the truncation - case 7 still
 * shows the __ftol. Transcribed as the fmod it is.
 *
 * A BAD TYPE ABORTS THE GAME in the original: "BAD AnimDecor type",
 * a message box and exit(1). Ours leaves the object alone - a port
 * has no business killing the process over one decoration.
 *
 * NOTHING UPSTREAM SCREENS THE TYPE, since 63 and 79 bound the
 * COUNTS, so one type 6 object reaches the default arm - which is
 * why that arm has to be real and not an assert.
 */
void trk_anim_decor_tick(MR_Trk *t, float dtScaled)
{
	int i;

	if (!t || !t->instances)
		return;

	for (i = 0; i < t->animDecorCount; i++) {
		MR_AnimDecor   *a = &t->animDecor[i];
		MR_TrkInstance *in;
		double          f, w, period;
		int             haveAngle = 0;

		if (a->type == 0)
			continue;
		if (a->decorIndex < 0 || a->decorIndex >= t->instanceCount)
			continue;

		in = &t->instances[a->decorIndex];
		a->time += dtScaled;

		period = (double)a->period;
		if (period == 0.0)
			continue;

		w = 0.0;
		switch (a->type) {
		case 1:                         /* triangle, an angle    */
		case 4:                         /* triangle, a position  */
			f = (double)a->time
			    - (double)(int)((double)a->time / period) * period;
			a->time = (float)f;
			w = (f / period) * TRK_ANIM_TRI;
			if (w > 1.0)
				w = TRK_ANIM_TRI - w;
			if (a->type == 1) {
				w = ((double)a->hi - (double)a->lo) * w
				    + (double)a->lo;
				haveAngle = 1;
			}
			break;

		case 2:                         /* sine between lo and hi */
			f = (double)a->time
			    - (double)(int)((double)a->time / period) * period;
			a->time = (float)f;
			w = sin((f / period) * TRK_ANIM_TWOPI
			        + (double)a->phase);
			w = ((double)a->lo + (double)a->hi
			     + w * ((double)a->hi - (double)a->lo))
			    * TRK_ANIM_HALF;
			haveAngle = 1;
			break;

		case 3:                         /* linear ramp - the rotor */
		case 5:
			f = (double)a->time
			    - (double)(int)((double)a->time / period) * period;
			a->time = (float)f;
			w = (f / period) * (double)a->lo + (double)a->phase;
			haveAngle = 1;
			break;

		case 7:
			/*
			 * THE VERTEX SWAP - the bird's wings.
			 *
			 *     wrap = (pathCount + 1) * period
			 *     step = (int)(time / period)
			 *     0            -> the template's own vertices
			 *     1..pathCount -> path[step - 1]
			 *
			 * The original stores a POINTER into the instance at
			 * +0x4c and restores +0x54's on step 0; ours records
			 * the step and trk_draw substitutes the array.
			 *
			 * 19 PATHS OF 26 VEC3 AND THE GULL HAS 26 VERTICES -
			 * read live off named[10] - so each path is a whole
			 * replacement set, not an offset or a bone.
			 */
			{
				double wrap = (double)(a->pathCount + 1) * period;

				if (a->pathCount <= 0 || wrap <= 0.0)
					continue;
				f = (double)a->time
				    - (double)(int)((double)a->time / wrap) * wrap;
				a->time  = (float)f;
				a->frame = (int)(f / period);
				if (a->frame < 0)
					a->frame = 0;
				if (a->frame > a->pathCount)
					a->frame = a->pathCount;
			}
			continue;

		case 8:                         /* position, with a hold  */
			period = (double)a->phase + (double)a->period;
			if (period == 0.0)
				continue;
			f = (double)a->time
			    - (double)(int)((double)a->time / period) * period;
			a->time = (float)f;
			if (f > (double)a->period)
				w = 1.0;
			else
				w = f / (double)a->period;
			break;

		default:
			/* Type 6 and out of range - see the header. */
			continue;
		}

		if (haveAngle) {
			if (a->axis >= 0 && a->axis <= 2)
				in->rot[a->axis] = (float)w;
		} else {
			int k;

			for (k = 0; k < 3; k++)
				in->pos[k] = (float)(((double)a->to[k]
			                          - (double)a->from[k]) * w
			                         + (double)a->from[k]);
		}
	}
}


/* ORIGIN  NONE - ours */
void trk_free(MR_Trk *t)
{
	int i, k;

	if (!t)
		return;
	/* The type-7 path blocks, one malloc each. */
	for (i = 0; i < t->animDecorCount; i++) {
		free(t->animDecor[i].path);
		t->animDecor[i].path = NULL;
	}
	t->animDecorCount = 0;

	if (t->segments) {
		for (i = 0; i < t->segmentCount; i++)
			free(t->segments[i].indices);
		free(t->segments);
	}
	if (t->meshes) {
		for (i = 0; i < t->meshCount; i++)
			free_mesh(&t->meshes[i]);
		free(t->meshes);
	}
	if (t->named) {
		for (i = 0; i < t->namedCount; i++)
			for (k = 0; k < 3; k++)
				free_mesh(&t->named[i].mesh[k]);
		free(t->named);
	}
	free(t->instances);
	memset(t, 0, sizeof(*t));
}

/* ORIGIN  Trk_Load @ 0x0040d650 */
int trk_load(MR_Trk *out, const char *logicalName)
{
	TrkReader     r;
	void         *blob;
	unsigned int  size = 0;
	const char   *magic;
	int           i, k;

	if (!out || !logicalName)
		return 0;
	memset(out, 0, sizeof(*out));

	blob = asset_load(logicalName, &size);
	if (!blob) {
		mrlog_err("trk: %s did not load\n", logicalName);
		return 0;
	}
	r.d = (const unsigned char *)blob;
	r.size = size;
	r.p = 0;
	r.bad = 0;

	magic = (const char *)rd_take(&r, 4);
	if (!magic || memcmp(magic, "TRK", 4) != 0) {
		mrlog_err("trk: %s is not a .trk\n", logicalName);
		goto fail;
	}
	out->version = (int)rd_u32(&r);
	if (out->version < 0 || out->version > 5) {
		mrlog_err("trk: %s version %d is not modelled\n",
		          logicalName, out->version);
		goto fail;
	}

	/* ---- segments ------------------------------------------- */
	out->segmentCount = rd_count(&r);
	if (r.bad)
		goto shortfile;
	if (out->segmentCount > 0) {
		out->segments = (MR_TrkSegment *)calloc(
		    (size_t)out->segmentCount, sizeof(MR_TrkSegment));
		if (!out->segments)
			goto fail;
	}
	for (i = 0; i < out->segmentCount; i++) {
		MR_TrkSegment *s = &out->segments[i];

		rd_floats(&r, s->pos, 3);
		rd_floats(&r, s->matrix, 12);
		s->meshIndex = rd_u32(&r);
		rd_floats(&r, &s->planes[0][0], 16);
		/* A FLOAT, NOT A DWORD - the racing line. See trk.h. */
		rd_floats(&r, &s->lineOffset, 1);
		if (out->version > 0) {
			s->indexCount = rd_count(&r);
			if (r.bad)
				goto shortfile;
			if (s->indexCount > 0) {
				const void *at = rd_take(&r,
				    (unsigned int)s->indexCount * 2);

				if (!at)
					goto shortfile;
				s->indices = (unsigned short *)malloc(
				    (size_t)s->indexCount * 2);
				if (!s->indices)
					goto fail;
				memcpy(s->indices, at, (size_t)s->indexCount * 2);
			}
		}
		if (out->version > 3) {
			/* THE SURFACE BAND, floats and not words - trk.h. */
			rd_floats(&r, &s->bandLo, 1);
			rd_floats(&r, &s->bandHi, 1);
		}
		if (r.bad)
			goto shortfile;
	}

	/* ---- meshes --------------------------------------------- */
	out->meshCount = rd_count(&r);
	if (r.bad)
		goto shortfile;
	if (out->meshCount > 0) {
		out->meshes = (MR_TrkMesh *)calloc((size_t)out->meshCount,
		                                   sizeof(MR_TrkMesh));
		if (!out->meshes)
			goto fail;
	}
	for (i = 0; i < out->meshCount; i++) {
		if (!rd_mesh(&r, &out->meshes[i], out->version, 1))
			goto shortfile;
	}

	/* ---- named templates and instances ---------------------- */
	if (out->version > 0) {
		out->namedCount = rd_count(&r);
		if (r.bad)
			goto shortfile;
		if (out->namedCount > 0) {
			out->named = (MR_TrkNamed *)calloc(
			    (size_t)out->namedCount, sizeof(MR_TrkNamed));
			if (!out->named)
				goto fail;
		}
		for (i = 0; i < out->namedCount; i++) {
			rd_name(&r, out->named[i].name, TRK_NAME_MAX);
			for (k = 0; k < 3; k++) {
				if (!rd_mesh(&r, &out->named[i].mesh[k],
				             out->version, 1))
				    goto shortfile;
			}
		}

		out->instanceCount = rd_count(&r);
		if (r.bad)
			goto shortfile;
		if (out->instanceCount > 0) {
			out->instances = (MR_TrkInstance *)calloc(
			    (size_t)out->instanceCount, sizeof(MR_TrkInstance));
			if (!out->instances)
				goto fail;
		}
		for (i = 0; i < out->instanceCount; i++) {
			MR_TrkInstance *n = &out->instances[i];

			rd_name(&r, n->name, TRK_NAME_MAX);
			rd_floats(&r, n->pos, 3);
			rd_floats(&r, n->rot, 3);
			n->namedIndex = (int)rd_u32(&r);
			n->soundDesc = -1;
			if (out->version > 4)
				n->soundDesc = (int)rd_u32(&r);
			n->drawable = 1;         /* until a camera claims it */
			if (r.bad)
				goto shortfile;
		}
	}

	/* ---- link table ----------------------------------------- */
	if (out->version > 1) {
		out->linkCount = rd_count(&r);
		if (r.bad)
			goto shortfile;
		/*
		 * THIS WRITES INTO THE SEGMENTS, not into a link array.
		 * Trk_Load's loop at 0x0040df49 indexes g_pTrkSegments by the
		 * SEGMENT stride (i*0xfc, built as ((i<<6)-i)<<2) and stores
		 * each field at segment+0xd8..+0xec.
		 *
		 * THE ORIGINAL DOES NOT BOUNDS-CHECK i AGAINST segmentCount
		 * [verified, the loop at 0x0040df33 tests only the link
		 * count]. The two counts are equal in every shipped track, so
		 * the skip below never fires on real data; it is here because
		 * a malformed file would otherwise write past the array.
		 */
		for (i = 0; i < out->linkCount; i++) {
			const void *at = rd_take(&r, 6);
			MR_TrkSegment *s;
			unsigned short a, b, c;
			unsigned int   d[4];

			if (!at)
				goto shortfile;
			memcpy(&a, (const unsigned char *)at + 0, 2);
			memcpy(&b, (const unsigned char *)at + 2, 2);
			memcpy(&c, (const unsigned char *)at + 4, 2);
			if (out->version > 3) {
				for (k = 0; k < 4; k++)
					d[k] = rd_u32(&r);
			} else {
				for (k = 0; k < 4; k++)
					d[k] = 0;
			}
			if (r.bad)
				goto shortfile;
			if (i >= out->segmentCount)
				continue;
			s = &out->segments[i];
			s->linkA      = a;
			s->linkB      = b;
			s->linkC      = c;
			/* file order is +0xec, +0xe8, +0xe4, +0xe0 */
			s->extra      = d[0];
			s->surfaceOut = d[1];
			s->surfaceIn  = d[2];
			s->shaded     = d[3];
		}
	}

	/* ---- AnimDecor, optional -------------------------------- */
	if (out->version > 2) {
		unsigned int mark;
		unsigned int bias, damp;

		/* The horizon's two terms - bias INT, damp FLOAT. See
		   MR_Trk.horizonYBias. */
		bias = rd_u32(&r);
		damp = rd_u32(&r);
		if (r.bad)
			goto shortfile;
		out->horizonYBias = (int)bias;
		memcpy(&out->horizonDamp, &damp, sizeof out->horizonDamp);

		mark = r.p;
		{
			const char *tag = (const char *)rd_take(&r, 10);

			if (tag && memcmp(tag, "AnimDecor", 9) == 0) {
				out->animDecorPresent = 1;
				if (!trk_load_anim_decor(&r, out))
					goto shortfile;
				out->animDecorBytes = r.p - mark;
			} else {
				/* NOT AnimDecor: the loader seeks back the
				 * ten bytes and the section is simply
				 * absent. trk.txt section 6. */
				r.bad = 0;
				r.p = mark;
			}
		}
	}

	/*
	 * THE SIGNAL SPANS, computed rather than read.
	 *
	 * ORIGIN  Trk_Load @ 0x0040e0da, the pass immediately after
	 *         AnimDecor_Load - which is why it sits here and
	 *         not up beside the link section that fills
	 *         `extra`.
	 *
	 * XOR, NOT OR. `extra` marks the segments where a span
	 * opens or closes, so each marker TOGGLES its bit and
	 * every segment stores the running set. See trk.h.
	 */
	{
		unsigned int acc = 0;

		for (i = 0; i < out->segmentCount; i++) {
			int kind = (int)out->segments[i].extra;

			if (kind >= TRK_SIGNAL_BASE &&
			    kind < TRK_SIGNAL_BASE + TRK_SIGNAL_KINDS) {
				acc ^= 1u << (kind - TRK_SIGNAL_BASE);
			}
			out->segments[i].signalFlags = acc;
		}
	}

	/*
	 * THE BYTE-EXACT CHECK. A binary format's field widths are almost
	 * never falsified by the data alone - a wrong one still parses
	 * and produces garbage - but a wrong one that must also land on
	 * the final byte very nearly always is.
	 */
	if (r.p != r.size) {
		mrlog_err("trk: %s consumed %u of %u bytes - the layout does "
		          "not fit this file\n", logicalName, r.p, r.size);
		goto fail;
	}

	free(blob);

	/*
	 * THE ROAD SURFACE IS NOT IN THE FILE - build it. Without this
	 * every ground query returns 0.0 and nothing that touches the
	 * terrain works. See trk.h.
	 */
	trk_build_segment_planes(out);

	/*
	 * LATER THAN THE ORIGINAL RUNS IT, deliberately. Trk_Load calls
	 * it at 0x0040df0e, immediately BEFORE the link section; ours
	 * runs after the planes are built. All it needs is the instance
	 * list, which exists by either point. maybe_Game_InitRace calls
	 * it again at 0x0047d5b4, so it is not a load-time act only.
	 */
	trk_count_cameras(out);
	return 1;

shortfile:
	mrlog_err("trk: %s ended early at %u of %u\n", logicalName,
	          r.p, r.size);
fail:
	free(blob);
	trk_free(out);
	return 0;
}

/*
 * ORIGIN  maybe_Trk_CountCameras @ 0x00473240
 *
 * THE TRACK AUTHORS ITS REPLAY CAMERAS AS DECOR. A camera position is
 * an ordinary instance of an ordinary template - it has a mesh, a
 * position and three euler angles like a palm tree does - and the
 * only thing that separates it from scenery is its NAME. This walks
 * the instances once after the load and claims every one whose
 * template name begins "CAMERA", counting it and CLEARING ITS
 * DRAWABLE FLAG.
 *
 * *** THAT CLEAR IS THE ONLY REASON THEY ARE INVISIBLE. *** Without
 * it the port drew them like any other prop: the blue rectangular
 * boxes standing head-height beside the track, pointing at the rider,
 * exactly where the trackside camera swings during the replay -
 * because that is precisely what they are.
 *
 * 256 IS A HARD LIMIT and the original says so out loud, with an
 * AfxMessageBox naming the offending instance. Past the cap it stops
 * counting but still clears the flag, so the extra cameras are
 * invisible and unusable rather than invisible and counted. Kept as
 * the original has it.
 *
 * IT TESTS THE TEMPLATE NAME, NOT THE INSTANCE NAME: strncmp of
 * instance+0x5c - the 8 bytes copied out of the template mesh -
 * against the literal "CAMERA", case sensitive. Instance+0x04 is
 * only what the overflow message names. Both read "CAMERA" here (the
 * instances are CAMERA_000..CAMERA_019), and Trk_FindNearestCamera
 * matches the INSTANCE name instead, trying "CAMERA" then "camera"
 * - two strncmps, so a mixed spelling fails both. Following the
 * original on each rather than picking one.
 *
 * ONE GATE IS DROPPED: the original skips any instance whose +0x38
 * is not 1. It is 1 on everything a real track places (decor.txt 3)
 * and we have no such field; the namedIndex check below is ours.
 *
 * WHAT IT DOES NOT DO: build a camera array. Nothing indexes the
 * count. Trk_FindNearestCamera re-walks the instances of the segments
 * near the bike every time it is asked, so the placements stay in the
 * decor table and there is no second list to keep in step.
 * [open - the replay camera itself is not ported]
 */
void trk_count_cameras(MR_Trk *t)
{
	int i;

	if (!t)
		return;
	t->cameraCount = 0;
	for (i = 0; i < t->instanceCount; i++) {
		MR_TrkInstance *in = &t->instances[i];
		const char     *tn;

		if (in->namedIndex < 0 || in->namedIndex >= t->namedCount)
			continue;
		tn = t->named[in->namedIndex].name;
		if (strncmp(tn, "CAMERA", 6) != 0)
			continue;

		if (t->cameraCount < TRK_CAMERA_MAX)
			t->cameraCount++;
		else
			mrlog_err("trk: more than %d cameras - %s ignored\n",
		              TRK_CAMERA_MAX, in->name);
		in->drawable = 0;
	}
}

/*
 * ORIGIN  Trk_FindNearestCamera @ 0x0040d4f0, whole
 *
 * The nearest trackside CAMERA decor to a point, searched ALONG THE
 * TRACK rather than through space: a window of 23 segments from the
 * bike's own, forward normally and backward in reverse, walking each
 * segment's decor list.
 *
 * WHAT IS NOT TESTED MATTERS MORE THAN WHAT IS. There is no line of
 * sight, no frustum test, no occlusion, and Y IS IGNORED - only x
 * and z enter the distance. "A camera is available" means nothing
 * more than "a decor object whose name begins CAMERA exists within
 * the next 23 segments".
 *
 * o SO A TUNNEL ALWAYS ORBITS, and it is not a tunnel special case.
 *   A stretch with no CAMERA within 23 segments returns nothing, the
 *   director reads that as "no camera" and falls back to its random
 *   orbit shots. A tunnel is simply the longest such stretch.
 *   See camera.txt 5.3.
 *
 * o (0,0,0) IS THE "NONE" SENTINEL, and the caller tests the vector
 *   rather than a return value - maybe_Vec3_Differs @ 0x00470860 is
 *   only asking whether it came back non-zero. A camera placed
 *   exactly at the world origin would therefore read as no camera at
 *   all. That is the original's contract and it is kept: giving this
 *   a boolean return would quietly diverge on a track we have not
 *   looked at.
 *
 * o IT MATCHES THE INSTANCE NAME, case both ways, where
 *   trk_count_cameras matches the TEMPLATE name. The two disagree in
 *   the original as well and each is followed rather than picking
 *   one - see the note there.
 *
 * The segment index wraps modulo the count, so the window crosses
 * the start line instead of stopping at it.
 */
void trk_find_nearest_camera(const MR_Trk *t, int segment,
                             const float pos[3], float out[3])
{
	const MR_TrkInstance *best = NULL;
	float                 bestD2 = TRK_CAM_SEARCH_FAR;
	int                   i, from, to;

	if (!out)
		return;
	out[0] = out[1] = out[2] = 0.0f;
	if (!t || !pos || t->segmentCount <= 0)
		return;

	/*
	 * THE WINDOW RUNS THE WAY THE BIKE DOES. Forward it is
	 * [seg, seg+23); in reverse [seg-23, seg). Searching forward in
	 * a reverse race looks at the track already ridden.
	 */
	if (!g_reverseEnabled) {
		from = segment;
		to   = segment + TRK_CAM_SEARCH_SEGS;
	} else {
		from = segment - TRK_CAM_SEARCH_SEGS;
		to   = segment;
	}

	for (i = from; i < to; i++) {
		const MR_TrkSegment *sg;
		int                  at, k;

		/* One count added before the modulo, exactly as the original
		 * does it - enough for the reverse window's negative end.
		 * The guard under it is ours; the original has none. */
		at = (i + t->segmentCount) % t->segmentCount;
		if (at < 0)
			continue;
		sg = &t->segments[at];
		if (!sg->indices)
			continue;

		for (k = 0; k < sg->indexCount; k++) {
			const MR_TrkInstance *in;
			int                   id = (int)sg->indices[k];
			float                 dx, dz, d2;

			if (id < 0 || id >= t->instanceCount)
				continue;
			in = &t->instances[id];

			if (strncmp(in->name, "CAMERA", 6) != 0 &&
			    strncmp(in->name, "camera", 6) != 0)
			    continue;

			dx = in->pos[0] - pos[0];
			dz = in->pos[2] - pos[2];
			d2 = dx * dx + dz * dz;
			if (d2 < bestD2) {
				bestD2 = d2;
				best   = in;
			}
		}
	}

	if (best) {
		out[0] = best->pos[0];
		out[1] = best->pos[1];
		out[2] = best->pos[2];
	}
}

/* ORIGIN  NONE - ours */
void trk_log(const MR_Trk *t, const char *what)
{
	int i;
	int verts = 0, faces = 0;

	if (!t)
		return;
	for (i = 0; i < t->meshCount; i++) {
		verts += t->meshes[i].vertexCount;
		faces += t->meshes[i].primCount;
	}
	mrlog("trk: %s v%d segments=%d meshes=%d verts=%d faces=%d\n",
	      what ? what : "(track)", t->version, t->segmentCount,
	      t->meshCount, verts, faces);
	mrlog("trk: named=%d instances=%d links=%d anim=%s(%d)\n",
	      t->namedCount, t->instanceCount, t->linkCount,
	      t->animDecorPresent ? "yes" : "no", t->animDecorObjects);
}

/*
 * ORIGIN  NONE - ours
 *
 * One mesh, already positioned by `local`. Returns the prims queued,
 * or -1 if the vertex pool is spent.
 *
 * verts OVERRIDES THE TEMPLATE'S ARRAY and is NULL for the ordinary
 * case. It exists for AnimDecor type 7, which animates a prop by
 * swapping whole vertex sets - see trk_anim_decor_tick.
 */
static int trk_queue_mesh_verts(const MR_TrkMesh *m, const float *verts,
                                const float *local, const float *view34)
{
	float      composed[12];
	MR_Vertex *v;

	if (!m || m->vertexCount <= 0 || m->primCount <= 0)
		return 0;
	mat34_multiply(composed, local, view34);
	v = transform_verts_flat(composed, verts ? verts : m->verts,
	                         m->vertexCount, 0x1f);
	if (!v)
		return -1;
	/* RETURN THE QUEUED COUNT, NOT primCount. The caller spends
	 * this against a budget, so returning the offered count charges
	 * for every face the culler discards and caps coverage at a
	 * fraction of the track however it is sorted.
	 *
	 * -1: the track and its props are not paletted. */
	return cull_and_queue_faces(v, m->prims, m->primCount, -1);
}

/*
 * NEAREST POINT ON THE MESH'S BOUNDING BOX.
 *
 * DO NOT SORT BY THE FIRST VERTEX. It reads as a good enough proxy
 * and it is, for a mesh spanning four segments - but one spanning the
 * whole map then sorts by whichever corner happens to be first in the
 * file. The ocean orders as though it sat at its far edge, and the
 * nearest-first budget drops it while drawing what is behind it.
 */
static float trk_mesh_dist2(const MR_TrkMesh *m, const float camPos[3])
{
	float lo[3], hi[3], d[3];
	int   i, k;

	if (m->vertexCount <= 0)
		return 3.0e38f;

	for (k = 0; k < 3; k++)
		lo[k] = hi[k] = m->verts[k];
	for (i = 1; i < m->vertexCount; i++) {
		for (k = 0; k < 3; k++) {
			float v = m->verts[i * 3 + k];
			if (v < lo[k]) lo[k] = v;
			if (v > hi[k]) hi[k] = v;
		}
	}

	for (k = 0; k < 3; k++) {
		d[k] = 0.0f;
		if (camPos[k] < lo[k])
			d[k] = lo[k] - camPos[k];
		else if (camPos[k] > hi[k])
			d[k] = camPos[k] - hi[k];
	}
	return d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
}

/*
 * trk_draw - queue the track: the world meshes, and the props.
 *
 * TWO KINDS OF GEOMETRY, AND THEY ARE IN DIFFERENT SPACES.
 *
 * o THE MESHES ARE WORLD SPACE. Measured over the file: mesh 0 spans
 *   z 512..2560, exactly where segments 0..3 sit, and mesh 50 is out
 *   at x -26573 where segments 200..203 are. So they take no model
 *   matrix at all, only world-to-camera.
 *
 * o THE PROPS ARE LOCAL SPACE, INSTANCED. A named template is drawn
 *   once per MR_TrkInstance, rotated by the instance's three euler
 *   angles and translated to its position. palmier's own vertices run
 *   y 0..1939 with the origin at the trunk base, and its nineteen
 *   instances place it around the circuit.
 *
 * THE ANGLES ARE RADIANS. The engine turns a euler triple into a
 * 0x4000-entry table index by multiplying by the double at
 * 0x004d13a0, which is TRIG_IDX_PER_RAD - 0x4000 over the game's
 * TYPED 2*pi, NOT over the true one; see trig.h. So they go
 * straight into mat34_from_euler with no conversion. [verified]
 *
 * NEAREST FIRST, WITHIN ONE SHARED BUDGET. QUEUE_NODE_MAX is 7499 and
 * ile01 alone has 11408 mesh faces before a single prop, so a whole
 * track does not fit - which is not a defect, it is that THE ORIGINAL
 * NEVER QUEUES ONE. LenBPlane, MediumZone, HightZone and the
 * per-segment index arrays are its visibility system and none of that
 * is reconstructed.
 *
 * So this orders by distance and stops at the budget. That is a
 * DEBUG-VIEW LIMIT, not a reconstruction of the zone system: no
 * invented radius, no invented fade, just "as much as the queue
 * holds, closest first".
 *
 * o PROPS GO FIRST, and that is a judgement rather than a finding.
 *   They are small, they are what a viewer notices missing - the
 *   palms, the buildings, the crowd - and they cost about a fifth of
 *   what the road does. If the budget ever has to be argued about,
 *   this is the line to argue with.
 */
int trk_draw(const MR_Trk *t, const float *view34, const float *camPos,
             float backPlane, float propClip,
             int camSegment, const float *camForward,
             const int *segList, int segCount,
             int *propsOut, int *meshesOut)
{
	float local[12];
	int   i, a, b, n;
	int   budget = 0;
	int   queued = 0;
	int   props = 0;
	int   order[TRK_DRAW_MAX_MESHES];
	float dist[TRK_DRAW_MAX_MESHES];

	if (propsOut)
		*propsOut = 0;
	if (meshesOut)
		*meshesOut = 0;

	if (!t || !view34 || !camPos)
		return 0;

	/* ---- the props ------------------------------------------- */
	/*
	 * *** PER VISIBLE SEGMENT, WHICH IS WHERE DECOR COMES FROM. ***
	 *
	 * maybe_Trk_DrawZonesAndListSegments dispatches decor through
	 * vtable slot +0xc for each segment in the ring window, so a
	 * prop is bounded by how far along the TRACK it is - not by
	 * how far it is from the eye.
	 *
	 * EACH SEGMENT'S indices[] IS THAT DECOR LIST, parsed by the
	 * loader. Checked live on the speed bay: segment 0 names
	 * decor 0, which is FINISH_000 on the start line, and the
	 * lists run 0, 1, 2, 3 in segment order.
	 *
	 * WITHOUT IT EVERY PROP ON THE TRACK DRAWS. A Corsair meant
	 * for deep in the lap shows from the grid - it is a short
	 * radial distance away and a long way along the track, which
	 * is exactly the case a distance clip cannot tell apart.
	 */
	n = 0;
	if (segList && segCount > 0
	    && t->instanceCount <= TRK_DRAW_MAX_MESHES) {
		unsigned char seen[TRK_DRAW_MAX_MESHES];
		int           si;

		memset(seen, 0, sizeof seen);
		for (si = 0; si < segCount && n < TRK_DRAW_MAX_MESHES; si++) {
			const MR_TrkSegment *sg;
			int                  k;

			if (segList[si] < 0 || segList[si] >= t->segmentCount)
				continue;
			sg = &t->segments[segList[si]];
			if (!sg->indices)
				continue;
			for (k = 0; k < sg->indexCount
			     && n < TRK_DRAW_MAX_MESHES; k++) {
				int id = (int)sg->indices[k];

				if (id < 0 || id >= t->instanceCount)
					continue;
				if (seen[id])
					continue;      /* two segments can name one */
				seen[id] = 1;
				order[n++] = id;
			}
		}
		for (i = 0; i < n; i++) {
			const MR_TrkInstance *in = &t->instances[order[i]];
			float dx = in->pos[0] - camPos[0];
			float dy = in->pos[1] - camPos[1];
			float dz = in->pos[2] - camPos[2];

			dist[order[i]] = dx * dx + dy * dy + dz * dz;
		}
		propClip = 0.0f;       /* the window IS the limit */
	} else {
		n = t->instanceCount;
		if (n > TRK_DRAW_MAX_MESHES)
			n = TRK_DRAW_MAX_MESHES;
		for (i = 0; i < n; i++) {
			const MR_TrkInstance *in = &t->instances[i];
			float dx = in->pos[0] - camPos[0];
			float dy = in->pos[1] - camPos[1];
			float dz = in->pos[2] - camPos[2];

			order[i] = i;
			dist[i] = dx * dx + dy * dy + dz * dz;
		}
	}
	for (a = 1; a < n; a++) {
		int   oi = order[a];
		float di = dist[oi];

		for (b = a - 1; b >= 0 && dist[order[b]] > di; b--)
			order[b + 1] = order[b];
		order[b + 1] = oi;
	}
	for (a = 0; a < n; a++) {
		const MR_TrkInstance *in = &t->instances[order[a]];
		const MR_TrkMesh     *m;
		const float          *verts;
		float                 rot[9];
		int                   got;
		int                   k;

		/*
		 * THE RADIAL CLIP IS THE NO-segList FALLBACK ONLY. With a
		 * segment list the window is already the limit and the
		 * branch above sets propClip to 0, so this never fires -
		 * props are bounded along the TRACK, as the original
		 * bounds them. Without one there is no track-relative
		 * information here at all; see the props note above for
		 * what that costs.
		 */
		if (propClip > 0.0f
		    && dist[order[a]] > propClip * propClip)
		    break;

		/*
		 * THE PER-OBJECT DRAW GATE, FUN_004022f0's first line.
		 * Clear on the CAMERA markers and set on everything else;
		 * see trk_count_cameras.
		 */
		if (!in->drawable)
			continue;

		if (in->namedIndex < 0 || in->namedIndex >= t->namedCount)
			continue;
		m = &t->named[in->namedIndex].mesh[0];   /* lod 0 */

		/*
		 * AND THE TYPE-7 VERTEX SWAP. A gull's wing animation is
		 * 19 whole replacement vertex sets; the animator picked
		 * one and this is where it is used. Step 0 is the
		 * template's own array.
		 */
		verts = NULL;
		for (k = 0; k < t->animDecorCount; k++) {
			const MR_AnimDecor *ad = &t->animDecor[k];

			if (ad->type != 7 || ad->decorIndex != order[a])
				continue;
			if (ad->frame <= 0 || !ad->path)
				break;
			if (ad->frameCount != m->vertexCount)
				break;          /* not this mesh's animation */
			verts = ad->path
			        + (size_t)(ad->frame - 1) * ad->frameCount * 3;
			break;
		}
		if (m->primCount <= 0)
			continue;
		/* Charged against what was QUEUED - see
		 * trk_queue_mesh_verts. The other limit is the pool
		 * running out, which returns -1 below. */
		if (budget >= TRK_DRAW_PRIM_BUDGET)
			break;

		/*
		 * THE DECOR EULER, NOT THE MODEL INSTANCES'. Props are
		 * drawn by FUN_004022f0, which calls FUN_00403880;
		 * ModelInstance_Draw's block never sees them. See
		 * mat34_from_euler_decor.
		 */
		mat34_from_euler_decor(rot, in->rot);
		for (i = 0; i < 9; i++)
			local[i] = rot[i];
		local[9]  = in->pos[0] - camPos[0];
		local[10] = in->pos[1] - camPos[1];
		local[11] = in->pos[2] - camPos[2];

		got = trk_queue_mesh_verts(m, verts, local, view34);
		if (got < 0)
			break;
		budget += got;
		queued++;
	}

	props = queued;

	/* ---- the world meshes ------------------------------------ */
	memset(local, 0, sizeof(local));
	local[0] = local[4] = local[8] = 1.0f;
	local[9]  = -camPos[0];
	local[10] = -camPos[1];
	local[11] = -camPos[2];

	/*
	 * *** THE RING WINDOW, 0x0040ce60. ***
	 *
	 * The original does not pick world meshes by distance at
	 * all. It draws the mesh covering the CAMERA SEGMENT plus a
	 * fixed span each way along the track - ten and one, or
	 * five and five, or one and ten, by which way the camera
	 * faces - and nothing else. No bounding volume, no tree, no
	 * precomputed set: a closed circuit is a loop and you can
	 * only see so far along it.
	 *
	 * A MESH COVERS FOUR SEGMENTS - 716/179 on the dirt arena,
	 * 788/197 on ile01, exact on both - which is where
	 * trk_list_visible_segments' *4 comes from.
	 *
	 * NEAREST-FIRST IS THE WRONG ORDER FOR LARGE FLAT DISTANT
	 * MESHES, which is what the fallback below still does and
	 * why it pops harshly: far scenery is the first thing a
	 * budget drops, and it comes and goes as the near count
	 * changes. A window pops once, at a fixed place on the
	 * track, which is what retail does.
	 *
	 * THE FALLBACK IS FOR A CALLER WITH NO CAMERA SEGMENT -
	 * page 10's preview has no track position.
	 */
	n = 0;
	if (camSegment >= 0 && camSegment < t->segmentCount
	    && t->segments && t->meshCount > 0 && t->segmentCount > 0
	    && t->meshCount <= TRK_DRAW_MAX_MESHES) {
		int ahead, behind, k, camMesh;

		/*
		 * THE CAMERA SEGMENT NAMES ITS OWN MESH. Read, never
		 * computed:
		 *
		 *     local_70 = segment[segIdx] + 0x3c
		 *     zone     = zones[local_70 % meshCount]
		 *
		 * at 0x0040ce9f, and the neighbours step that MESH index
		 * by one, not the segment index.
		 *
		 * DO NOT COMPUTE IT AS camSegment / (segmentCount /
		 * meshCount). That agrees by luck on the five shipped
		 * tracks whose ratio is exactly 4; on the other five it
		 * truncates to 3 - ile08 919/230, ile04 782/198,
		 * Track06 435/110 - so the window centres a third too
		 * far along, the error GROWS WITH DISTANCE DOWN THE
		 * COURSE, and it clamps at the last mesh. The ground
		 * vanishes from under the bike partway round, worst on
		 * the tracks with the most meshes.
		 */
		camMesh = (int)(t->segments[camSegment].meshIndex
		                % (unsigned int)t->meshCount);

		trk_zone_span(t, camSegment, camForward, &ahead, &behind);

		for (k = -behind; k <= ahead && n < TRK_DRAW_MAX_MESHES; k++) {
			int idx = (camMesh + k) % t->meshCount;

			if (idx < 0)
				idx += t->meshCount;
			order[n] = idx;
			dist[idx] = trk_mesh_dist2(&t->meshes[idx], camPos);
			n++;
		}
	} else {
		n = t->meshCount;
		if (n > TRK_DRAW_MAX_MESHES)
			n = TRK_DRAW_MAX_MESHES;
		for (i = 0; i < n; i++) {
			order[i] = i;
			dist[i] = trk_mesh_dist2(&t->meshes[i], camPos);
		}
	}
	for (a = 1; a < n; a++) {
		int   oi = order[a];
		float di = dist[oi];

		for (b = a - 1; b >= 0 && dist[order[b]] > di; b--)
			order[b + 1] = order[b];
		order[b + 1] = oi;
	}
	for (a = 0; a < n; a++) {
		const MR_TrkMesh *m = &t->meshes[order[a]];
		int               got;

		/* LenBPlane. Sorted nearest-first, so the first mesh past
		 * the back plane ends the walk. */
		if (backPlane > 0.0f &&
		    dist[order[a]] > backPlane * backPlane)
		    break;

		if (m->primCount <= 0)
			continue;
		/* Charge what is queued, not what is offered - see the
		 * note in the prop loop above. */
		if (budget >= TRK_DRAW_PRIM_BUDGET)
			break;
		got = trk_queue_mesh_verts(m, NULL, local, view34);
		if (got < 0)
			break;
		budget += got;
		queued++;
	    }

	/* Split on purpose - race.c's draw log says why. */
	if (propsOut)
		*propsOut = props;
	if (meshesOut)
		*meshesOut = queued - props;
	return queued;
}

/*
 * ORIGIN  NONE - ours. Cf. Mesh_RebaseBankIndices @ 0x0045c710,
 *         which does this for the bike's two mesh arrays.
 *
 * trk_rebase_banks - shift every face's bankIndex by delta.
 *
 * A face's bankIndex indexes the LEVEL MANIFEST'S twenty entries,
 * 0..19. Our bank slots are a global pool shared with the menu
 * pages, so the track's twenty live at some base and every
 * primitive has to move to match - the same thing page 10 does to
 * the bike with prim_rebase_banks.
 */
void trk_rebase_banks(MR_Trk *t, int delta)
{
	int i, k;

	if (!t)
		return;
	for (i = 0; i < t->meshCount; i++)
		prim_rebase_banks(t->meshes[i].prims,
	                      t->meshes[i].primCount, delta);
	for (i = 0; i < t->namedCount; i++)
		for (k = 0; k < 3; k++)
			prim_rebase_banks(t->named[i].mesh[k].prims,
	                          t->named[i].mesh[k].primCount, delta);
}


/* ================================================================== */
/* PLANE HELPERS AND THE GROUND QUERY. See trk.h.                     */
/* ================================================================== */

/* ORIGIN  Plane_Distance @ 0x004b10c0 */
float plane_distance(const float plane[4], const float p[3])
{
	if (!plane || !p)
		return 0.0f;

	return (float)((double)plane[0] * p[0]
	               + (double)plane[1] * p[1]
	               + (double)plane[2] * p[2]
	               + (double)plane[3]);
}


/* ORIGIN  Plane_IntersectSegment @ 0x004b0f60 */
void plane_intersect_segment(const float plane[4], float out[3],
                             const float a[3], const float b[3])
{
	double den;
	double t;
	int    i;

	if (!plane || !out || !a || !b)
		return;

	/*
	 * A zero-length segment collapses to its start. The original
	 * tests each component against an epsilon rather than the
	 * length, so a very thin but non-zero segment can take this
	 * branch too.
	 */
	if (fabs((double)a[0] - b[0]) <= TRK_PLANE_EPS
	    && fabs((double)a[1] - b[1]) <= TRK_PLANE_EPS
	    && fabs((double)a[2] - b[2]) <= TRK_PLANE_EPS) {
		for (i = 0; i < 3; i++)
			out[i] = a[i];
		return;
	}

	den = (double)plane[0] * ((double)b[0] - a[0])
	      + (double)plane[1] * ((double)b[1] - a[1])
	      + (double)plane[2] * ((double)b[2] - a[2]);

	/* Parallel to the plane - also collapses to the start. */
	if (den == 0.0) {
		for (i = 0; i < 3; i++)
			out[i] = a[i];
		return;
	}

	t = -(((double)plane[0] * a[0] + (double)plane[1] * a[1]
	       + (double)plane[2] * a[2] + (double)plane[3]) / den);

	for (i = 0; i < 3; i++)
		out[i] = (float)((double)a[i] + ((double)b[i] - a[i]) * t);
}


/*
 * ORIGIN  Trk_ContainPointInSegment @ 0x0040cd90, whole.
 *
 * THE RAIL CONTAINMENT, the second of the two wall routines.
 * maybe_Trk_ClipMoveAgainstWalls (0x0040ca80) REFLECTS a move off a
 * rail; this one CLAMPS THE POSITION back inside the segment.
 * maybe_Racer_UpdateGroundContact calls it EVERY FRAME at
 * 0x00469409 - on &racer->position and the racer's segmentStart at
 * +0x31c1 - immediately before Trk_GroundHeightAt at 0x0046941e.
 *
 * WITHOUT IT PENETRATION ACCUMULATES.  The reflect handles a
 * single hit, so the first contact looks right; nothing then
 * pushes the bike back out, so repeated shoves at a rail dig it
 * progressively deeper until it is inside the mesh and the reflect
 * fires every frame - which reads as being snagged and unable to
 * steer away.  A slow angled approach into a bridge rail, then
 * throttle, does it in three or four contacts.
 *
 * THE LINE IT PULLS BACK ALONG runs from the SEGMENT'S OWN pos to
 * the point, not along the wall normal, so the correction follows
 * the track rather than the rail.  Both planes are tested in turn
 * and the second sees whatever the first left.
 *
 * THE THRESHOLD IS 0.0 (0x004d02d0), so the test is simply
 * "beyond the rail".
 */
void trk_contain_point(const MR_Trk *t, int segment, float p[3])
{
	const MR_TrkSegment *seg;
	int                  w;

	if (!t || !t->segments || !p)
		return;
	if (segment < 0 || segment >= t->segmentCount)
		return;

	seg = &t->segments[segment];

	/* planes[2] and planes[3] are the two walls - trk.h. */
	for (w = 2; w <= 3; w++) {
		const float *pl = seg->planes[w];
		double       d;
		double       dir[3];
		double       den;
		double       s;
		int          i;

		d = (double)pl[0] * (double)p[0]
		    + (double)pl[1] * (double)p[1]
		    + (double)pl[2] * (double)p[2] + (double)pl[3];
		if (d >= 0.0)
			continue;

		for (i = 0; i < 3; i++)
			dir[i] = (double)p[i] - (double)seg->pos[i];

		/*
		 * BOTH DEGENERATE CASES PUT THE POINT AT THE SEGMENT'S
		 * pos rather than failing, which is what the original
		 * does - see Plane_IntersectSegment 0x004b0f60.
		 */
		if (fabs(dir[0]) <= TRK_CONTAIN_EPS
		    && fabs(dir[1]) <= TRK_CONTAIN_EPS
		    && fabs(dir[2]) <= TRK_CONTAIN_EPS) {
			for (i = 0; i < 3; i++)
				p[i] = seg->pos[i];
			continue;
		}

		den = (double)pl[0] * dir[0] + (double)pl[1] * dir[1]
		      + (double)pl[2] * dir[2];
		if (den == 0.0) {
			for (i = 0; i < 3; i++)
				p[i] = seg->pos[i];
			continue;
		}

		s = -(((double)pl[0] * (double)seg->pos[0]
		       + (double)pl[1] * (double)seg->pos[1]
		       + (double)pl[2] * (double)seg->pos[2]
		       + (double)pl[3]) / den);

		for (i = 0; i < 3; i++)
			p[i] = (float)((double)seg->pos[i] + dir[i] * s);
	}
}


/* ORIGIN  Trk_GroundHeightAt @ 0x0040c8b0 */
float trk_ground_height_at(const MR_Trk *t, int segment,
                           const float p[3])
{
	const MR_TrkSegment *seg;
	const float         *pl;

	if (!t || !t->segments || t->segmentCount < 1 || !p)
		return 0.0f;
	/* The range check is ours - the original indexes unchecked. */
	if (segment < 0 || segment >= t->segmentCount)
		return 0.0f;

	seg = &t->segments[segment];

	/*
	 * Which triangle. The original tests >= 0 against the selector,
	 * so a point exactly ON the diagonal takes ground[0].
	 */
	pl = (plane_distance(seg->selector, p) >= 0.0f)
	     ? seg->ground[0] : seg->ground[1];

	/* A vertical ground plane has no height. THE ORIGINAL'S OWN
	 * guard, ABS(pl[1]) != 0.0, not a port addition. */
	if (pl[1] == 0.0f)
		return 0.0f;

	return (float)(-(((double)pl[0] * p[0] + (double)pl[2] * p[2]
	                  + (double)pl[3]) / (double)pl[1]));
}


/* The nominal half-width before the walls clip it. A literal at four
 * sites in maybe_Trk_BuildSegmentPlanes. */
#define TRK_NOMINAL_HALF_WIDTH 2048.0f

/* ORIGIN  Plane_FromNormalPoint @ 0x004b0f20 */
void plane_from_normal_point(float out[4], const float n[3],
                             const float p[3])
{
	if (!out || !n || !p)
		return;

	out[0] = n[0];
	out[1] = n[1];
	out[2] = n[2];
	out[3] = (float)(-((double)n[0] * p[0] + (double)n[1] * p[1]
	                   + (double)n[2] * p[2]));
}


static void trk_vec_normalize(float v[3])
{
	double len = sqrt((double)v[0] * v[0] + (double)v[1] * v[1]
	                  + (double)v[2] * v[2]);

	if (len == 0.0)
		return;
	v[0] = (float)(v[0] / len);
	v[1] = (float)(v[1] / len);
	v[2] = (float)(v[2] / len);
}


static void trk_cross(float out[3], const float u[3], const float v[3])
{
	out[0] = (float)((double)u[1] * v[2] - (double)u[2] * v[1]);
	out[1] = (float)((double)u[2] * v[0] - (double)u[0] * v[2]);
	out[2] = (float)((double)u[0] * v[1] - (double)u[1] * v[0]);
}


/*
 * One corner: the segment origin pushed sideways by the nominal
 * half-width along the matrix's LATERAL row, then clipped against the
 * wall plane on that side.
 *
 * The matrix is row-vector: v*M = v.x*row0 + v.y*row1 + v.z*row2 +
 * row3, and v is (+-halfWidth, 0, 0), so only row0 and the
 * translation contribute.
 */
static void trk_segment_corner(float out[3], const MR_TrkSegment *seg,
                               float halfWidth, const float wall[4])
{
	/* NOT "far" - MSVC still reserves the 16-bit memory-model
	 * keywords through windows.h. */
	float edge[3];
	int   i;

	for (i = 0; i < 3; i++) {
		edge[i] = seg->pos[i] + seg->matrix[9 + i]
		          + seg->matrix[0 + i] * halfWidth;
	}

	plane_intersect_segment(wall, out, seg->pos, edge);
}


/*
 * ORIGIN  maybe_Trk_DrawZonesAndListSegments @ 0x0040ce60, the split
 *         at 0x0040ce9a
 *
 * HOW FAR THE RING WINDOW REACHES, and which way. One dot product
 * between the camera's forward and the camera segment's first plane
 * normal picks one of three splits, and TRK_ZONE_SPAN is the 10 the
 * Track constructor hard-codes at 0x0040c5b4.
 *
 *     facing along  ->  10 ahead, 1 behind
 *     side on       ->   5 and 5
 *     facing back   ->   1 ahead, 10 behind
 *
 * IN MESHES, NOT SEGMENTS. trk_list_visible_segments multiplies by
 * four because a mesh covers four segments - 716/179 on the dirt
 * arena and 788/197 on ile01, exact on both.
 */
void trk_zone_span(const MR_Trk *t, int camSegment,
                   const float camForward[3], int *aheadOut,
                   int *behindOut)
{
	const MR_TrkSegment *seg;
	double dot;
	int    ahead, behind;

	ahead  = TRK_ZONE_SPAN / 2;
	behind = TRK_ZONE_SPAN / 2;

	if (t && t->segments && camForward
	    && camSegment >= 0 && camSegment < t->segmentCount) {
		seg = &t->segments[camSegment];
		dot = (double)seg->planes[0][0] * camForward[0]
		    + (double)seg->planes[0][1] * camForward[1]
		    + (double)seg->planes[0][2] * camForward[2];

		if (dot >= TRK_ZONE_FACING) {
			ahead  = TRK_ZONE_SPAN;
			behind = 1;
		} else if (dot <= -TRK_ZONE_FACING) {
			ahead  = 1;
			behind = TRK_ZONE_SPAN;
		}
	}

	if (aheadOut)
		*aheadOut = ahead;
	if (behindOut)
		*behindOut = behind;
}


/*
 * ORIGIN  maybe_Trk_DrawZonesAndListSegments @ 0x0040ce60
 *         the LIST half; the geometry half is section 9 below.
 *
 * The span each way is trk_zone_span's; this is what is done with it.
 *
 * THE TWO RUNS ARE NOT THE SAME LENGTH:
 *
 *     forward    ahead*4 + 5 entries, STARTING AT the camera segment
 *     backward   behind*4 + 4 entries, starting one behind it
 *
 * The backward loop starts at i = 1 and runs while i <= behind*4+4,
 * so it is +4 and not +5 - a live capture gives 49 entries at
 * span/2 = 5, which is 25 + 24 where +5 would have given 54.
 * [verified, 0x0040d268]
 *
 * THE *4 IS A LITERAL in the emit loops and has nothing to do with
 * the roughly-four-segments-per-mesh grouping. Do not "derive" it
 * from meshIndex.
 *
 * BOTH RUNS WRAP modulo the segment count, so the list crosses the
 * start/finish line rather than stopping at it.
 *
 * THE AXIS IS planes[0]. trk_build_segment_planes rebuilds it as the
 * entry plane from a normalised matrix[6..8], so its normal IS the
 * segment forward - which is what the original reads at +0x40, also
 * after its own plane builder has overwritten the file value. The
 * plane builder therefore has to run before this does.
 */
int trk_list_visible_segments(const MR_Trk *t, int camSegment,
                              const float camForward[3],
                              int *out, int maxOut)
{
	const MR_TrkSegment *seg;
	double dot;
	int    ahead, behind, i, n = 0;

	if (!t || !t->segments || t->segmentCount < 1 || !camForward || !out)
		return 0;
	if (camSegment < 0 || camSegment >= t->segmentCount)
		return 0;

	(void)seg;
	(void)dot;
	trk_zone_span(t, camSegment, camForward, &ahead, &behind);

	/*
	 * THE DECOR DISPATCH IS INSIDE THESE LOOPS in the original, not
	 * a pass of its own - each segment is emitted and its decor
	 * drawn in the same iteration, and the whole thing is skipped
	 * when g_cfgDetailLevel is 0. Keeping that shape means the stub
	 * sits exactly where the body will go.
	 */

	/* ahead*4+5, and i == 0 is the camera segment itself. */
	for (i = 0; i <= ahead * 4 + 4 && n < maxOut; i++) {
		out[n] = (camSegment + i) % t->segmentCount;
		if (g_cfgDetailLevel != 0)
			trk_draw_segment_decor(t, out[n]);
		n++;
	}

	/* behind*4+4, starting ONE behind. */
	for (i = 1; i <= behind * 4 + 4 && n < maxOut; i++) {
		int back = camSegment - i;

		if (back < 0)
			back += t->segmentCount;
		out[n] = back;
		if (g_cfgDetailLevel != 0)
			trk_draw_segment_decor(t, out[n]);
		n++;
	}

	return n;
}


/*
 * ORIGIN  maybe_Trk_DrawZonesAndListSegments @ 0x0040ce60, at
 *         0x0040d1ee and 0x0040d2eb - the DECOR dispatch only.
 *
 * PER-SEGMENT DECOR, AND IT IS THE ONE PART OF THAT FUNCTION THAT IS
 * NOT PORTABLE YET. For each segment in the list the original walks
 * decorCount items and makes a virtual call per item:
 *
 *     class = *(short *)(segment + 0xb4 + 2*k)
 *     (**(DAT_004dc0e4[class] + 0xc))(screen3d, camera)
 *
 * with the count at segment+0xd4, and the whole thing skipped when
 * g_cfgDetailLevel is 0.
 *
 * WE DO PARSE BOTH FIELDS. Each segment's indexCount/indices IS
 * that list - the count the original keeps at +0xd4 and the u16s
 * it keeps at +0xb4, read in the main segment loop all along.
 * trk_find_nearest_camera and objsnd_bind both walk it.
 *
 * What is left for THIS function is the class table at
 * DAT_004dc0e4 and its vtables, which are a second subsystem
 * again. [open]
 */
void trk_draw_segment_decor(const MR_Trk *t, int segment)
{
	(void)t;
	(void)segment;
	MR_STUB_ONCE("decor in maybe_Trk_DrawZonesAndListSegments"
	             " @ 0x0040ce60, at 0x0040d1ee",
	             "per-segment AnimDecor objects");
}


/*
 * ORIGIN  Trk_FindSignalSegments @ 0x0040d3b0
 *
 * For each of the seven signal bits, the nearest segment
 * carrying it within +/-window segments of cur.
 *
 * NEAREST IS MEASURED FROM cur's ORIGIN IN 3D, not along the
 * track - so on a hairpin a segment forty ahead can win over
 * one ten ahead. Y is included and there is no line of sight.
 *
 * out[] IS NOT INITIALISED HERE. The original leaves that to
 * ObjSnd_TickAll, which fills all seven with -1 first; a bit
 * with nothing in the window is simply never written.
 */
void trk_find_signal_segments(const MR_Trk *t,
                              int out[TRK_SIGNAL_KINDS],
                              int cur, int window)
{
	float best[TRK_SIGNAL_KINDS];
	const float *from;
	int    i, bit, start;

	if (!t || !t->segments || t->segmentCount <= 0)
		return;
	if (cur < 0 || cur >= t->segmentCount)
		return;

	for (bit = 0; bit < TRK_SIGNAL_KINDS; bit++)
		best[bit] = FLT_MAX;   /* 3.4028235e+38 in the original */

	from  = t->segments[cur].pos;
	start = ((t->segmentCount - window) + cur) % t->segmentCount;

	for (i = 0; i < window * 2; i++) {
		const MR_TrkSegment *sg;
		unsigned int         flags;
		float                dx, dy, dz, d2;
		int                  at = start + i;

		/*
		 * ONE SUBTRACTION, as the original has. It is enough
		 * whenever 2*window <= segmentCount, which holds on
		 * every shipped track - ile01 is 788 against 80. The
		 * bounds test below is ours: the original would read
		 * past the array on a track short enough to wrap
		 * twice, and we would rather skip than crash.
		 */
		if (at >= t->segmentCount)
			at -= t->segmentCount;
		if (at < 0 || at >= t->segmentCount)
			continue;

		sg    = &t->segments[at];
		flags = sg->signalFlags;
		if (flags == 0)
			continue;

		dx = sg->pos[0] - from[0];
		dy = sg->pos[1] - from[1];
		dz = sg->pos[2] - from[2];
		d2 = dx * dx + dz * dz + dy * dy;

		for (bit = 0; bit < TRK_SIGNAL_KINDS; bit++) {
			if ((flags & (1u << bit)) != 0 && d2 < best[bit]) {
				out[bit]  = at;
				best[bit] = d2;
			}
		}
	}
}



/*
 * ORIGIN  Trk_WrapSegmentIndex @ 0x0040ec30
 *
 * Two while loops, not a modulo - the original's own shape, and it
 * matters only in that a wildly out-of-range index spins rather than
 * folding. grid.c has held a private copy of this since before there
 * was a second caller; that copy stays where it is until something
 * makes it worth merging.
 */
int trk_wrap_segment_index(const MR_Trk *t, int i)
{
	int n;

	if (!t || t->segmentCount <= 0)
		return 0;
	n = t->segmentCount;
	while (i < 0)
		i += n;
	while (i >= n)
		i -= n;
	return i;
}


/*
 * ORIGIN  Trk_SurfaceAtPoint @ 0x0040ec60
 *
 * The first function to decode fields inside a segment.
 *
 *     n = matrix row 0, t = matrix row 3
 *     d = n . (p - origin) - n . t
 *     return (bandLo < d && d < bandHi) ? surfaceIn : surfaceOut
 *
 * d IS A SIGNED DISTANCE ALONG ROW 0 from the segment origin, offset
 * by the matrix translation - so the band is a strip either side of
 * the segment's centre line and the test is "am I on the road".
 *
 * THE BAND IS NARROWER THAN THE TRACK. On ile01 it is -1024..+1024
 * against guard rails at 1280, so there is verge inside the walls -
 * which is what the tyre trail draws on.
 *
 * o SURFACE 0 IS THE ROAD and produces no trail at all: the effect's
 *   constructor has arms for 1..4 only and returns without marking
 *   the pool slot in use. There is no separate road test anywhere.
 *
 * o THE RANGE CHECK IS OURS. The original indexes unchecked and
 *   relies on the caller: both of them pass racer->segmentStart,
 *   which the ground fit keeps valid. Same split as
 *   trk_ground_height_at, whose vertical-plane guard IS the
 *   original's.
 */
int trk_surface_at_point(const MR_Trk *t, int segment, const float p[3])
{
	const MR_TrkSegment *s;
	const float         *n;
	float                d;

	if (!t || !t->segments || !p)
		return 0;
	if (segment < 0 || segment >= t->segmentCount)
		return 0;

	s = &t->segments[segment];
	n = &s->matrix[0];              /* row 0, used as a direction   */

	d = n[0] * (p[0] - s->pos[0])
	  + n[1] * (p[1] - s->pos[1])
	  + n[2] * (p[2] - s->pos[2])
	  - (n[0] * s->matrix[9] + n[1] * s->matrix[10]
	     + n[2] * s->matrix[11]);

	if (s->bandLo < d && d < s->bandHi)
		return (int)s->surfaceIn;
	return (int)s->surfaceOut;
}


/* ORIGIN  maybe_Trk_BuildSegmentPlanes @ 0x0040e5c0 */
void trk_build_segment_planes(MR_Trk *t)
{
	int i;

	if (!t || !t->segments || t->segmentCount < 1)
		return;

	for (i = 0; i < t->segmentCount; i++) {
		MR_TrkSegment *si = &t->segments[i];
		MR_TrkSegment *sj = &t->segments[(i + 1) % t->segmentCount];
		float A[3], B[3], C[3], D[3];
		float n[3], u[3], v[3];
		int   k;

		/*
		 * planes[0] is OVERWRITTEN with the segment's entry
		 * plane - the matrix's forward row through the origin,
		 * which is what a point-in-segment test wants. Whatever
		 * the file had there is discarded.
		 */
		for (k = 0; k < 3; k++)
			n[k] = si->matrix[6 + k];
		trk_vec_normalize(n);
		plane_from_normal_point(si->planes[0], n, si->pos);

		/*
		 * The four corners. LEFT clips against wall0
		 * (planes[2]), RIGHT against wall1 (planes[3]), and the
		 * next segment's corners against the NEXT segment's
		 * walls - verified live, its corners miss this
		 * segment's walls by tens of units and hit its own to
		 * within 0.05.
		 */
		trk_segment_corner(A, si, -TRK_NOMINAL_HALF_WIDTH,
		                   si->planes[2]);
		trk_segment_corner(B, si, TRK_NOMINAL_HALF_WIDTH,
		                   si->planes[3]);
		trk_segment_corner(C, sj, -TRK_NOMINAL_HALF_WIDTH,
		                   sj->planes[2]);
		trk_segment_corner(D, sj, TRK_NOMINAL_HALF_WIDTH,
		                   sj->planes[3]);

		/*
		 * The selector: a VERTICAL plane through the diagonal
		 * B->C. Its Y is a literal zero in the original, and x
		 * and z are the diagonal's components swapped with one
		 * negated - a 90-degree turn in XZ.
		 */
		n[0] = B[2] - C[2];
		n[1] = 0.0f;
		n[2] = C[0] - B[0];
		trk_vec_normalize(n);
		plane_from_normal_point(si->selector, n, C);

		/* ground[0] - the triangle A, B, C. */
		for (k = 0; k < 3; k++) {
			u[k] = C[k] - A[k];
			v[k] = B[k] - A[k];
		}
		trk_cross(n, u, v);
		trk_vec_normalize(n);
		plane_from_normal_point(si->ground[0], n, A);

		/*
		 * ground[1] - the triangle B, C, D. NOTE THE OPPOSITE
		 * WINDING: the original's normal here comes out facing
		 * the other way from ground[0]'s, and that is not a
		 * transcription slip - it was measured. The height
		 * solve divides by the normal's Y, so the sign cancels.
		 */
		for (k = 0; k < 3; k++) {
			u[k] = C[k] - D[k];
			v[k] = B[k] - D[k];
		}
		trk_cross(n, u, v);
		trk_vec_normalize(n);
		plane_from_normal_point(si->ground[1], n, D);
	}
}


/* ORIGIN  NONE - ours. A guard the original does not need because it
 *         always builds its planes; see trk.h. */
int trk_segment_has_surface(const MR_Trk *t, int segment)
{
	const MR_TrkSegment *seg;

	if (!t || !t->segments || t->segmentCount < 1)
		return 0;
	if (segment < 0 || segment >= t->segmentCount)
		return 0;

	seg = &t->segments[segment];
	return (seg->ground[0][1] != 0.0f) || (seg->ground[1][1] != 0.0f);
}


#define CLIP_MOVE_EPS     1.0e-5f   /* 0x004d02c8 */
#define CLIP_DIST_ZERO    0.0f      /* 0x004d02d0 */
#define CLIP_REFLECT      2.0       /* 0x004d02d8 - a REFLECTION */

/*
 * One wall. Returns non-zero if the move crossed it.
 *
 * ORIGIN  maybe_Trk_ClipMoveAgainstWalls @ 0x0040ca80, the body that
 *         appears twice - once for seg+0x60 and once for seg+0x70.
 */
static int clip_one_wall(const float wall[4],
                         const float from[3], const float to[3],
                         float hitPoint[3], float dir[3],
                         float normal[3])
{
	double d;
	double len;
	int    i;

	if (plane_distance(wall, to) >= CLIP_DIST_ZERO)
		return 0;               /* still inside */

	plane_intersect_segment(wall, hitPoint, from, to);

	for (i = 0; i < 3; i++)
		normal[i] = wall[i];

	/*
	 * dir -= 2 * dot(dir, normal) * normal.
	 *
	 * THE 2 MAKES THIS A REFLECTION, not a slide - see trk.h. Drop
	 * it and the bike would scrape along the wall instead of
	 * bouncing off it.
	 */
	d = (double)dir[0] * normal[0]
	    + (double)dir[1] * normal[1]
	    + (double)dir[2] * normal[2];

	for (i = 0; i < 3; i++)
		dir[i] = (float)((double)dir[i]
	                     - d * CLIP_REFLECT * (double)normal[i]);

	len = sqrt((double)dir[0] * dir[0]
	           + (double)dir[1] * dir[1]
	           + (double)dir[2] * dir[2]);
	if (len != 0.0) {
		for (i = 0; i < 3; i++)
			dir[i] = (float)(dir[i] / len);
	}

	return 1;
}


/* ORIGIN  maybe_Trk_ClipMoveAgainstWalls @ 0x0040ca80 */
int trk_clip_move_against_walls(const MR_Trk *t, int segment,
                                const float from[3], const float to[3],
                                float hitPoint[3], float dir[3],
                                float normal[3])
{
	const MR_TrkSegment *seg;
	int hit = 0;

	if (!t || !t->segments || !from || !to || !hitPoint || !dir
	    || !normal)
	    return 0;
	if (segment < 0 || segment >= t->segmentCount)
		return 0;

	/*
	 * A move too small to matter. All THREE components must be under
	 * the epsilon - the original returns early only if every one is,
	 * so a move that is tiny in x and y but large in z still counts.
	 */
	if (fabsf(from[0] - to[0]) <= CLIP_MOVE_EPS
	    && fabsf(from[1] - to[1]) <= CLIP_MOVE_EPS
	    && fabsf(from[2] - to[2]) <= CLIP_MOVE_EPS)
	    return 0;

	seg = &t->segments[segment];

	/*
	 * BOTH walls, not the first match - the original unrolls the
	 * two blocks and neither returns early. Into a corner the
	 * direction reflects twice, the second off the already-
	 * reflected vector.
	 *
	 * AND hitPoint AND normal ARE OVERWRITTEN BY THE SECOND HIT, so
	 * on a double hit the caller gets wall 1's normal against a
	 * direction that has bounced off both. maybe_Racer_UpdateGround-
	 * Contact dots those two together for its impact cosine.
	 */
	if (clip_one_wall(seg->planes[2], from, to, hitPoint, dir, normal))
		hit = 1;
	if (clip_one_wall(seg->planes[3], from, to, hitPoint, dir, normal))
		hit = 1;

	return hit;
}


/* ORIGIN  Trk_FindSegmentForPoint @ 0x0040c7e0 */
int trk_find_segment_for_point(const MR_Trk *t, int segment,
                               const float p[3], float *outDist)
{
	float d;
	int   n;
	int   steps;

	if (!t || !t->segments || !p)
		return 0;

	n = t->segmentCount;
	if (n < 1)
		return 0;

	/*
	 * THE LOW CLAMP AND THE steps < n BOUND BELOW ARE BOTH OURS.
	 * The original tests only "seg >= count", so a negative hint
	 * indexes before the array, and neither walk is bounded - equal
	 * signs on every entry plane would spin forever. Unreachable on
	 * well-formed track data, which is why it can do without them.
	 */
	if (segment >= n || segment < 0)
		segment = 0;

	d = plane_distance(t->segments[segment].planes[0], p);

	if (d >= CLIP_DIST_ZERO) {
		/*
		 * Forward: keep going while the NEXT segment has also been
		 * entered, and hand back the last one that had.
		 */
		for (steps = 0; steps < n; steps++) {
			int next = (segment + 1) % n;
			float dn = plane_distance(t->segments[next].planes[0],
			                          p);
			if (dn < CLIP_DIST_ZERO) {
				d = dn;
				break;
			}
			segment = next;
			d = dn;
		}
	} else {
		/* Backward: until the point is past an entry plane again. */
		for (steps = 0; steps < n; steps++) {
			segment--;
			if (segment < 0)
				segment = n - 1;
			d = plane_distance(t->segments[segment].planes[0], p);
			if (d >= CLIP_DIST_ZERO)
				break;
		}
	}

	if (outDist)
		*outDist = d;
	return segment;
}

/* ================================================================== */
/* THE IN-RACE MAP MESH.                                              */
/*                                                                    */
/* ORIGIN  Trk_BuildMap @ 0x0040ed10, the LINE path                   */
/*                                                                    */
/* See docs/subsystems/hud-map.txt.                                   */
/* ================================================================== */

/*
 * THE MAP IS GENERATED FROM THE SEGMENT ARRAY, not loaded. The
 * original builds two meshes into the track object - the outline at
 * track+0x28 and the rider triangles at track+0xa4 - and this is the
 * first of them.
 *
 * o TWO GEOMETRIES, CHOSEN BY THE DEVICE. With hardware lines it
 *   emits one LINE every eight segments; without them it emits a
 *   QUAD every four, using the segment's edge offsets, so the map
 *   becomes a ribbon with width. Only the line path is built here -
 *   this port's rasteriser draws lines (PRIM_FLAG_LINE), so the quad
 *   path is unreachable. Its shape is in hud-map.txt 1.1 for the day
 *   a back end needs it. [deliberate divergence, narrow]
 *
 * o THE COUNTS FALL OUT OF THE SEGMENT COUNT:
 *
 *       primCount = segmentCount / 4          197 for ile01's 788
 *       vertCount = primCount / 2 + 1         99
 *
 *   and the walk steps 0x7e0 bytes against a 0xfc segment, which is
 *   EIGHT segments a vertex. Both numbers were read back out of a
 *   running MOTO before this was written.
 *
 * o THE FACE IS A DEGENERATE TRIANGLE. Face_InitTriangle(0, black,
 *   i, i+1, i) has only two distinct indices, and the primitive then
 *   gets 0x10 - PRIM_FLAG_LINE - stamped at +0x38. A line is a
 *   triangle with a repeated corner, not a separate primitive kind.
 */
#define TRK_MAP_SEGS_PER_PRIM  4
#define TRK_MAP_SEGS_PER_VERT  8

/*
 * ORIGIN  Trk_BuildMap @ 0x0040ed10
 *
 * The scale is [0x004d031c] / track[0x20] - 1280.0f over the
 * flattened outline's bounding-CIRCLE radius. hud-map.txt 1.2.
 */
#define TRK_MAP_TARGET_RADIUS 1280.0f

int trk_build_map(MR_Trk *t)
{
	int   i;
	int   nPrim;
	int   nVert;
	float centre[3];
	float lo[3];
	float hi[3];
	float radius;

	if (!t || !t->segments || t->segmentCount <= 0)
		return 0;

	mesh_free(&t->mapMesh);
	memset(&t->mapMesh, 0, sizeof(t->mapMesh));

	nPrim = t->segmentCount / TRK_MAP_SEGS_PER_PRIM;
	nVert = nPrim / 2 + 1;
	if (nPrim <= 0 || nVert <= 1)
		return 0;

	t->mapMesh.vertices =
	    (MESH_VERTEX *)calloc((size_t)nVert, sizeof(MESH_VERTEX));
	t->mapMesh.faces =
	    (MESH_FACE *)calloc((size_t)nPrim, sizeof(MESH_FACE));
	if (!t->mapMesh.vertices || !t->mapMesh.faces) {
		mesh_free(&t->mapMesh);
		memset(&t->mapMesh, 0, sizeof(t->mapMesh));
		return 0;
	}
	t->mapMesh.vertexCount = nVert;
	t->mapMesh.faceCount   = nPrim;

	/*
	 * ONE VERTEX EVERY EIGHT SEGMENTS, FLATTENED. The original
	 * copies the segment's three position floats and then writes
	 * z over y and zeroes z - a top-down projection that throws the
	 * height away. The last vertex repeats the first to close the
	 * loop, which is why the count is prim/2 + 1 and not prim/2.
	 */
	for (i = 0; i < nVert - 1; i++) {
		int s = (i * TRK_MAP_SEGS_PER_VERT) % t->segmentCount;

		t->mapMesh.vertices[i].x = t->segments[s].pos[0];
		t->mapMesh.vertices[i].y = t->segments[s].pos[2];
		t->mapMesh.vertices[i].z = 0.0f;
	}
	t->mapMesh.vertices[nVert - 1] = t->mapMesh.vertices[0];

	/*
	 * THE FACES, 0x0040f0a0. Two distinct indices and the third a
	 * repeat - a line drawn as a degenerate triangle. The original
	 * pushes (0, 0, 0, i, i+1, i) and bounds the loop on the
	 * primitive count at track+0x54 [verified].
	 *
	 * [open] THERE ARE TWICE AS MANY PRIMITIVES AS VERTICES, and
	 * this is not explained. ile01 measures 197 prims against 99
	 * verts (hud-map.txt 6, both live), so the original's i+1
	 * reaches 197 against an array of 99 - indices from 99 on are
	 * past the vertex count it reports.
	 *
	 * OURS WRAPS WITH % nVert, which keeps it in bounds and makes
	 * faces 99..196 REDRAW the first half of the outline. Harmless
	 * on screen, twice the primitives it needs, and NOT what the
	 * original does - what that draws for i >= 99 is unestablished.
	 * Do not read the wrap as a transcription.
	 */
	for (i = 0; i < nPrim; i++) {
		int a = i % nVert;
		int b = (i + 1) % nVert;

		t->mapMesh.faces[i].count    = 3;
		t->mapMesh.faces[i].flags    = PRIM_FLAG_LINE;
		t->mapMesh.faces[i].index[0] = a;
		t->mapMesh.faces[i].index[1] = b;
		t->mapMesh.faces[i].index[2] = a;
		t->mapMesh.faces[i].mapRecordN = -1;
	}

	/*
	 * CENTRED ON ITS OWN BOUNDS AND BAKED AT MAP SCALE, so the draw
	 * only has to translate and rotate it. The rider triangles are
	 * built in world units and scaled at draw time instead.
	 * hud-map.txt 1.2.
	 */
	mesh_center_on_bounds(&t->mapMesh, centre);

	lo[0] = hi[0] = t->mapMesh.vertices[0].x;
	lo[1] = hi[1] = t->mapMesh.vertices[0].y;
	for (i = 1; i < nVert; i++) {
		float x = t->mapMesh.vertices[i].x;
		float y = t->mapMesh.vertices[i].y;

		if (x < lo[0]) lo[0] = x;
		if (x > hi[0]) hi[0] = x;
		if (y < lo[1]) lo[1] = y;
		if (y > hi[1]) hi[1] = y;
	}
	/*
	 * THE DIVISOR IS THE BOUNDING-CIRCLE RADIUS, NOT THE LONGEST
	 * SIDE. The longer axis makes the map 64 per cent of retail's
	 * on ile01; the map ROTATES, so it must fit its circumscribed
	 * circle at any angle. hud-map.txt 1.2.
	 */
	radius = sqrtf((hi[0] - lo[0]) * (hi[0] - lo[0])
	             + (hi[1] - lo[1]) * (hi[1] - lo[1])) * 0.5f;
	if (radius < 1.0f)
		radius = 1.0f;

	t->mapScale     = TRK_MAP_TARGET_RADIUS / radius;
	t->mapOrigin[0] = centre[0];
	t->mapOrigin[1] = centre[1];
	t->mapOrigin[2] = centre[2];

	mesh_scale(&t->mapMesh, t->mapScale, t->mapScale, t->mapScale);

	mrlog("trk:    map %d prims / %d verts, radius %.1f, "
	      "scale %.5f\n", nPrim, nVert, radius, t->mapScale);
	return 1;
}

