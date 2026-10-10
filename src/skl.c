/*
 * skl.c - .SKL skeletons and .ROT poses. See skl.h.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "skl.h"
#include "assets.h"
#include "bpt.h"
#include "tracks.h"
#include "trig.h"
#include "model.h"
#include "log.h"

/*
 * THE TWO VERTEX FORMATS, and the sign of the first word picks one.
 *
 * A NEGATIVE member count means the file stores floats, and the real
 * count follows it; otherwise the counts are int16. The two carry
 * different scales, 3.5 against 10.0, applied as the file is read
 * [verified, the local_20 branch in FUN_00462c60]. Both shipped
 * skeletons are the float form.
 */
#define SKL_SCALE_FLOAT 3.5f
#define SKL_SCALE_INT16 10.0f

typedef struct {
	const unsigned char *d;
	unsigned int         size;
	unsigned int         pos;
	int                  bad;
} SKL_Reader;

static int rd_i16(SKL_Reader *r)
{
	int v;

	if (r->pos + 2 > r->size) {
		r->bad = 1;
		return 0;
	}
	v = (int)(short)((unsigned int)r->d[r->pos] |
	                 ((unsigned int)r->d[r->pos + 1] << 8));
	r->pos += 2;
	return v;
}

static float rd_f32(SKL_Reader *r)
{
	unsigned int u;
	float        f;

	if (r->pos + 4 > r->size) {
		r->bad = 1;
		return 0.0f;
	}
	u = (unsigned int)r->d[r->pos] |
	    ((unsigned int)r->d[r->pos + 1] << 8) |
	    ((unsigned int)r->d[r->pos + 2] << 16) |
	    ((unsigned int)r->d[r->pos + 3] << 24);
	memcpy(&f, &u, sizeof f);
	r->pos += 4;
	return f;
}

void skl_free(MR_Skeleton *s)
{
	if (!s)
		return;
	if (s->bind)
		free(s->bind);
	mesh_free(&s->mesh);
	memset(s, 0, sizeof *s);
}

/*
 * One face record: the vertex count, a second count, a 5-6-5 colour,
 * then FIVE index/u/v triples whatever the count says. Slot n repeats
 * slot 0 to close the polygon and the rest are -1.
 */
static void read_face(SKL_Reader *r, MESH_FACE *f)
{
	int n;
	int kind;
	int colour;
	int i;

	n      = rd_i16(r);
	kind   = rd_i16(r);
	colour = rd_i16(r);
	(void)kind;

	memset(f, 0, sizeof *f);
	if (n < 3)
		n = 3;
	if (n > MESH_MAX_FACE_VERTS)
		n = MESH_MAX_FACE_VERTS;
	f->count = n;

	for (i = 0; i < SKL_FACE_SLOTS; i++) {
		int idx = rd_i16(r);
		int u   = rd_i16(r);
		int v   = rd_i16(r);

		if (i < n) {
			f->index[i] = idx;
			f->u[i]     = (float)u;
			f->v[i]     = (float)v;
		}
	}

	/*
	 * THE FACES CARRY NO BANK INDEX, so they are all relative bank 0
	 * and prim_rebase_banks moves them onto whichever slot the caller
	 * loaded bnk00_00 into - the same rule the .map faces follow.
	 */
	f->hasMap      = 1;
	f->bank        = 0;
	f->mapFlags    = 0;
	f->textureId   = 0;
	f->mapRecordN  = n;
	f->mapColor    = (unsigned short)colour;
}

int skl_load(MR_Skeleton *s, const char *logical)
{
	SKL_Reader    r;
	unsigned char *buf;
	unsigned int   size = 0;
	int            floatFmt;
	int            n;
	int            nv;
	int            i;
	int            faceTotal = 0;
	int            written = 0;
	float          scale;
	int            firstOfMember[SKL_MAX_MEMBERS];
	int            countOfMember[SKL_MAX_MEMBERS];
	unsigned int   facesStart;

	if (!s)
		return 0;
	memset(s, 0, sizeof *s);

	buf = (unsigned char *)asset_load(logical, &size);
	if (!buf) {
		mrlog_err("skl: cannot open %s\n", logical);
		return 0;
	}
	r.d = buf;
	r.size = size;
	r.pos = 0;
	r.bad = 0;

	n = rd_i16(&r);
	floatFmt = (n < 0);
	if (floatFmt) {
		n = rd_i16(&r);
		scale = SKL_SCALE_FLOAT;
	} else {
		scale = SKL_SCALE_INT16;
	}
	if (n <= 0 || n > SKL_MAX_MEMBERS) {
		mrlog_err("skl: %s claims %d members, the loader's own limit "
		          "is %d\n", logical, n, SKL_MAX_MEMBERS);
		free(buf);
		return 0;
	}
	s->memberCount = n;

	for (i = 0; i < n; i++)
		s->member[i].parent = rd_i16(&r);

	/*
	 * THE REMAP IS KEPT, AND IT MATTERS. It used to be read and
	 * discarded on the argument that the parent list already
	 * describes the tree. It does - but the remap is not about the
	 * tree, it is the MEMBER-TO-PART PERMUTATION, and a .rot is
	 * indexed by PART.
	 *
	 * Rot_ApplyToRacer (0x0045b480) copies pose triple i to part i,
	 * and part = remap[member]. So member i wears angle[remap[i]],
	 * and using angle[i] silently poses the wrong bone - which is
	 * what this loader did.
	 *
	 * THE REMAP IS NOT THE IDENTITY, whatever a capture once seemed
	 * to show. On SPEED\CORPS.SKL it is
	 *
	 *     member  0  1   2  3   4  5 ...
	 *     part    0  1  15  2  16  3 ...
	 *
	 * [verified, racers.txt 9.59.4], and this file used to claim the
	 * opposite two lines further down. Three things agree with the
	 * table and none with the identity: SKL_PART_WHEEL_FRONT is 15
	 * and is member 2; the geometry |origin[2] - origin[1]| is the
	 * fork L0; and skl_swingarm found its arm member by
	 * remap[i] == 2 and model.c measured that member as mat[3], so
	 * remap[3] == 2.
	 *
	 * The int16 form carries no remap; there the two orders agree.
	 */
	for (i = 0; i < n; i++)
		s->remap[i] = i;
	if (floatFmt) {
		for (i = 0; i < n; i++) {
			int v = rd_i16(&r);
	
			s->remap[i] = (v >= 0 && v < n) ? v : i;
		}
	}

	nv = rd_i16(&r);
	if (nv <= 0 || r.bad) {
		mrlog_err("skl: %s has %d vertices\n", logical, nv);
		free(buf);
		return 0;
	}

	s->bind = (MESH_VERTEX *)calloc((size_t)nv, sizeof(MESH_VERTEX));
	if (!s->bind) {
		free(buf);
		return 0;
	}
	for (i = 0; i < nv; i++) {
		if (floatFmt) {
			s->bind[i].x = rd_f32(&r) * scale;
			s->bind[i].y = rd_f32(&r) * scale;
			s->bind[i].z = rd_f32(&r) * scale;
		} else {
			s->bind[i].x = (float)rd_i16(&r);
			s->bind[i].y = (float)rd_i16(&r);
			s->bind[i].z = (float)rd_i16(&r);
		}
	}

	/*
	 * TWO PASSES OVER THE MEMBERS, because the face total is not in
	 * the file: the first counts, the second reads. Cheaper than
	 * growing an array, and it validates the layout on the way - a
	 * mis-read here walks off the end rather than producing subtly
	 * wrong geometry.
	 */
	facesStart = r.pos;
	for (i = 0; i < n && !r.bad; i++) {
		int nf;

		if (floatFmt) {
			s->member[i].origin[0] = rd_f32(&r) * scale;
			s->member[i].origin[1] = rd_f32(&r) * scale;
			s->member[i].origin[2] = rd_f32(&r) * scale;
		} else {
			s->member[i].origin[0] = (float)rd_i16(&r);
			s->member[i].origin[1] = (float)rd_i16(&r);
			s->member[i].origin[2] = (float)rd_i16(&r);
		}
		firstOfMember[i] = rd_i16(&r);
		countOfMember[i] = rd_i16(&r);
		nf               = rd_i16(&r);
		if (nf < 0 || r.pos + (unsigned int)nf * 36u > r.size) {
			r.bad = 1;
			break;
		}
		r.pos += (unsigned int)nf * 36u;
		faceTotal += nf;
	}
	if (!r.bad) {
		int nf = rd_i16(&r);

		/*
		 * AND ONE MORE GROUP AFTER THE LAST MEMBER. The loader writes
		 * it into slot memberCount - one past the end of the member
		 * array - so it belongs to no bone and never moves. On the
		 * shipped bikes it is the biggest group of the file.
		 *
		 * IT STILL DRAWS, AND IT NEEDS NO MATRIX. maybe_Racer_Draw
		 * transforms memberCount runs of vertices but queues
		 * memberCount+1 runs of faces (the INC EAX at 0x00461a06 and
		 * 0x00461a85), all against the ONE vertex base the first
		 * transform returned. So these faces index vertices other
		 * members already moved - they are the glue stitching the
		 * rigid parts together. Merging them into the single MESH
		 * below reproduces that exactly.
		 *
		 * WHICH IS ALSO A CONSTRAINT ON DRAWING PER PART. The
		 * original's pool is APPENDED to, so a glue face only lands
		 * on the right vertices because the members are visited in
		 * ascending, contiguous run order. See racers.txt 9.64.
		 */
		if (nf < 0 || r.pos + (unsigned int)nf * 36u > r.size)
			r.bad = 1;
		else
			faceTotal += nf;
	}
	if (r.bad || faceTotal <= 0) {
		mrlog_err("skl: %s did not decode - %u of %u bytes read\n",
		          logical, r.pos, size);
		free(buf);
		skl_free(s);
		return 0;
	}

	for (i = 0; i < n; i++) {
		s->member[i].firstVertex = firstOfMember[i];
		s->member[i].vertexCount = countOfMember[i];
	}

	/*
	 * DO THE RUNS TILE THE POOL? The draw appends each member's
	 * transformed slice in member order and then indexes faces off the
	 * base of the first, so a gap or an out-of-order run would shift
	 * every vertex after it. Empty members are pivots and carry no
	 * run, so they are skipped rather than required to be in place.
	 * See skl.h.
	 */
	{
		int next = 0;

		s->runsPacked = 1;
		for (i = 0; i < n; i++) {
			if (s->member[i].vertexCount <= 0)
				continue;
			if (s->member[i].firstVertex != next) {
				s->runsPacked = 0;
				break;
			}
			next += s->member[i].vertexCount;
		}
		if (s->runsPacked && next != nv)
			s->runsPacked = 0;
		if (!s->runsPacked)
			mrlog("skl: %s runs do not tile %d vertices - the draw will bake\n",
		          logical, nv);
	}

	s->mesh.vertexCount = nv;
	s->mesh.faceCount   = faceTotal;
	s->mesh.vertices    = (MESH_VERTEX *)calloc((size_t)nv,
	                                            sizeof(MESH_VERTEX));
	s->mesh.faces       = (MESH_FACE *)calloc((size_t)faceTotal,
	                                          sizeof(MESH_FACE));
	if (!s->mesh.vertices || !s->mesh.faces) {
		free(buf);
		skl_free(s);
		return 0;
	}

	r.pos = facesStart;
	r.bad = 0;
	for (i = 0; i < n && !r.bad; i++) {
		int nf;
		int k;

		if (floatFmt) {
			(void)rd_f32(&r); (void)rd_f32(&r); (void)rd_f32(&r);
		} else {
			(void)rd_i16(&r); (void)rd_i16(&r); (void)rd_i16(&r);
		}
		(void)rd_i16(&r);
		(void)rd_i16(&r);
		nf = rd_i16(&r);
		/* the member's slice of the flat list starts here */
		s->member[i].firstFace = written;
		for (k = 0; k < nf && written < faceTotal; k++)
			read_face(&r, &s->mesh.faces[written++]);
		s->member[i].faceCount = written - s->member[i].firstFace;
	}
	if (!r.bad) {
		int nf = rd_i16(&r);
		int k;

		for (k = 0; k < nf && written < faceTotal; k++)
			read_face(&r, &s->mesh.faces[written++]);
	}

	free(buf);

	mrlog("skl: %s  %d members, %d vertices, %d faces, %s form\n",
	      logical, n, nv, written, floatFmt ? "float" : "int16");

	/*
	 * RE-CENTRE ON THE ROOT JOINT, which the original does too:
	 * after loading, every vertex is moved so that the first
	 * member's origin is at zero [verified, racers.txt section 6].
	 *
	 * o IT IS INVISIBLE ON THE BODY ALONE and essential the
	 *   moment anything else is placed beside it. The wheel
	 *   offsets are measured from the RACER position, and after
	 *   this the racer position and the root joint are the same
	 *   point. Without it the wheels sit a joint-offset away.
	 *
	 *   The check is that the numbers then agree: a rear wheel
	 *   centre lands at y -30.7 with a radius of 30.7, so its
	 *   bottom is y -61 - and the re-centred body's lowest
	 *   vertex is y -62. The ground comes out flat under both.
	 *
	 * The member origins move with the vertices, so the tree
	 * still composes: translating the whole model changes no
	 * relative distance.
	 */
	{
		float root[3];

		root[0] = s->member[0].origin[0];
		root[1] = s->member[0].origin[1];
		root[2] = s->member[0].origin[2];
		for (i = 0; i < nv; i++) {
			s->bind[i].x -= root[0];
			s->bind[i].y -= root[1];
			s->bind[i].z -= root[2];
		}
		for (i = 0; i < n; i++) {
			s->member[i].origin[0] -= root[0];
			s->member[i].origin[1] -= root[1];
			s->member[i].origin[2] -= root[2];
		}
	}

	skl_apply_pose(s, NULL);
	return 1;
}

int rot_load(MR_SklPose *p, const char *logical)
{
	unsigned char *buf;
	unsigned int   size = 0;
	int            i;
	int            j;

	if (!p)
		return 0;
	memset(p, 0, sizeof *p);

	buf = (unsigned char *)asset_load(logical, &size);
	if (!buf)
		return 0;

	/*
	 * "ROT", A THREE-int16 HEADER, THEN SEVENTEEN ANGLE TRIPLES.
	 *
	 *     char magic[4]    "ROT" and a NUL
	 *     i16  hdr[3]      NOT angles - skipped, see below
	 *     i16  angle[17][3]
	 *
	 * 4 + 6 + 17*6 = 112, the size of every .rot the game ships.
	 *
	 * o THIS USED TO READ 18 TRIPLES FROM OFFSET 4, and the bug was
	 *   invisible because BOTH READINGS FILL 112 BYTES EXACTLY -
	 *   4 + 18*6 and 4 + 6 + 17*6 are the same number. No size check,
	 *   no leftover-bytes check and no "it decodes cleanly" can tell
	 *   them apart. It put every member's angles on the bone before
	 *   it.
	 *
	 * o Rot_Load (0x0045b340) settles it [verified]: four magic bytes
	 *   compared against "ROT", then THREE separate two-byte reads,
	 *   and only then the triple loop - which is bounded at 0x14, not
	 *   18. It scales each angle by the double at 0x004d0e68, which is
	 *   0.0015339807878856412 = 2*pi/0x1000 - NOT 2*pi/0x4000. See
	 *   skl_apply_pose for why that distinction cost three sessions.
	 *
	 * o THE HEADER IS READ AND DISCARDED by the original too. It looks
	 *   like a small vec3 and is constant per archive rather than per
	 *   pose - (-7, 19, -70) throughout SPEED and SPEEDPKT. We keep it
	 *   so a future reader does not have to go and find it again.
	 */
	if (size < SKL_ROT_BYTES || memcmp(buf, "ROT", 3) != 0) {
		mrlog_err("skl: %s is not a %d-byte ROT file\n", logical,
		          SKL_ROT_BYTES);
		free(buf);
		return 0;
	}

	for (j = 0; j < 3; j++) {
		unsigned int o = 4u + (unsigned int)j * 2u;

		p->header[j] = (short)((unsigned int)buf[o] |
		                       ((unsigned int)buf[o + 1] << 8));
	}

	for (i = 0; i < SKL_ROT_MEMBERS; i++) {
		for (j = 0; j < 3; j++) {
			unsigned int o = 10u + (unsigned int)(i * 3 + j) * 2u;

			p->angle[i][j] = (short)((unsigned int)buf[o] |
			                         ((unsigned int)buf[o + 1] << 8));
		}
	}
	p->loaded = 1;
	free(buf);
	return 1;
}

/*
 * COMPOSE PARENT THEN LOCAL - out = parent * local.
 *
 * This was the other way round, and the error is invisible at the
 * root and grows with depth: a capture reproduced parts 0 to 4
 * exactly and drifted to 0.36 by the third level. Skl_BuildPartMatrices
 * (0x00463d90) composes the incoming parent matrix on the LEFT.
 * [verified - with this order the whole of staticPied reproduces the
 * captured matrices to 0.0004, which is the dump's own precision]
 */
static void skl_compose(float out[12], const float parent[12],
                        const float local[12])
{
	mat34_multiply(out, parent, local);
}

/*
 * INVERT A RIGID TRANSFORM - transpose the 3x3, negate the translation
 * through it. Valid only because the 3x3 is orthonormal, which it is:
 * it is a product of Euler rotations.
 *
 *     v' = v * M + t      =>      v = (v' - t) * transpose(M)
 *
 * maybe_Racer_Draw builds exactly this inline at 0x004615c1, element by
 * element, immediately before it transforms a part's vertices - see
 * skl_apply_pose for why that matters.
 *
 * IT LIVES IN model.c now, beside mat34_multiply, because bike_draw
 * needs it too: the draw composes each member's inverse with the view
 * rather than baking a model-space mesh. Declared in model.h.
 */

/*
 * THE ANGLE SOURCE, shared by both entry points.
 *
 * skl_apply_pose reads a .ROT's shorts and scales them;
 * skl_apply_anim reads an MR_AnimFrame that is already radians. The
 * REMAP applies either way - member i wears part remap[i] - so it
 * lives here rather than in the callers.
 */
typedef struct {
	const MR_SklPose  *pose;    /* shorts, needs scaling */
	const MR_AnimFrame *anim;   /* radians, ready to use */
} SKL_AngleSource;

static void skl_member_rot(const MR_Skeleton *s, const SKL_AngleSource *src,
                           int i, float rot[3])
{
	int k;
	int part;

	rot[0] = rot[1] = rot[2] = 0.0f;
	if (!s || !src)
		return;

	part = (i >= 0 && i < s->memberCount) ? s->remap[i] : i;
	if (part < 0 || part >= SKL_MAX_MEMBERS)
		part = i;

	if (src->anim) {
		if (part < src->anim->count && part < ANIM_MAX_JOINTS) {
			for (k = 0; k < 3; k++)
				rot[k] = src->anim->angle[part][k];
		}
		return;
	}
	if (src->pose && src->pose->loaded) {
		for (k = 0; k < 3; k++) {
			rot[k] = (float)((double)src->pose->angle[part][k] *
			                 TRIG_TWO_PI / (double)SKL_ROT_TURN);
		}
	}
}


/*
 * skl_build_source - the tree walk, and NOTHING ELSE.
 *
 * Split out of the old skl_apply_source so the matrices exist as a
 * value a caller can see and adjust before any vertex moves. That is
 * the shape of the original: Skl_BuildPartMatrices @ 0x00463d90 and
 * its walker FUN_00464190 fill MR_RacerPart+0x18 for every part, and
 * maybe_Racer_Draw then OVERWRITES part 15's before drawing, to
 * re-anchor the front bodywork along the fork axis. A design that
 * bakes straight to vertices has nowhere to put that step.
 *
 * The count is bounded by the loader, which refuses a .skl with more
 * than SKL_MAX_MEMBERS members, so s->mat[] cannot be overrun here.
 */
static void skl_build_source(MR_Skeleton *s, const SKL_AngleSource *src)
{
	int i;

	for (i = 0; i < s->memberCount; i++) {
		float local[12];
		float rot[3];
		int   parent = s->member[i].parent;

		/*
		 * A .ROT TURN IS 0x1000, NOT THE TRIG TABLE'S 0x4000.
		 *
		 * Rot_Load (0x0045b340) scales each int16 by the double at
		 * 0x004d0e68, which is 0.0015339807878856412 = 2*pi/0x1000
		 * [verified]. Everything else in the engine - the cosine
		 * table, mat34_from_euler, trig.h - uses 0x4000, so a .rot
		 * angle is FOUR TIMES the index it looks like.
		 *
		 * o THIS WAS WRONG HERE FOR THREE SESSIONS, and it was wrong
		 *   because the constant was ASSERTED rather than computed:
		 *   0x3f5921fb54442d18 shares its mantissa with pi/2 and was
		 *   read off as 2*pi/0x4000 at a glance. Its exponent is ten
		 *   below pi/2's, not twelve. A runtime capture settled it:
		 *   every angle in the racer is exactly 4x the file value.
		 *
		 * o AND MEMBER i WEARS angle[remap[i]]. Rot_ApplyToRacer
		 *   (0x0045b480) copies pose triple i to PART i, and
		 *   part = remap[member]. Indexing by member poses the wrong
		 *   bone; on corps.skl it swaps the wheels with the forks.
		 */
		skl_member_rot(s, src, i, rot);

		/*
		 * A BONE ROTATES ABOUT ITS OWN JOINT; IT DOES NOT TRANSLATE
		 * TO IT.
		 *
		 * The bind vertices are already in model space, in the bind
		 * pose - the origins are where the joints ARE, not offsets
		 * from the parent. So a member transform is
		 *
		 *     v' = (v - origin) * R + origin
		 *        = v * R + (origin - origin * R)
		 *
		 * and the translation term is that difference, not the origin
		 * itself.
		 *
		 * o THE TEST IS THE IDENTITY POSE. With every angle zero R is
		 *   the identity, the difference is zero, and the model is
		 *   left exactly as the file holds it. Using the origin as
		 *   the translation instead moves every bone by its own joint
		 *   and every child by the accumulated joints of its whole
		 *   chain - so the deeper a limb sits in the tree the further
		 *   it flies. A format whose identity pose destroys the model
		 *   is not a plausible format.
		 *
		 *   That is what the first attempt did, and it looked like it
		 *   sounds: a rider with stretched arms, torso and neck
		 *   floating off a bike whose front end had run away forward.
		 *   [inferred - the loader builds a linked tree rather than
		 *   composing matrices, and its walk has not been read]
		 */
		mat34_from_euler(local, rot);
		{
			const float *o = s->member[i].origin;

			local[9]  = o[0] - (o[0] * local[0] + o[1] * local[3] +
			                    o[2] * local[6]);
			local[10] = o[1] - (o[0] * local[1] + o[1] * local[4] +
			                    o[2] * local[7]);
			local[11] = o[2] - (o[0] * local[2] + o[1] * local[5] +
			                    o[2] * local[8]);
		}

		if (parent >= 0 && parent < s->memberCount && parent != i)
			skl_compose(s->mat[i], s->mat[parent], local);
		else
			memcpy(s->mat[i], local, sizeof local);
	}
}


/*
 * skl_transform_verts - pose the mesh from whatever s->mat now holds.
 *
 * Reads the matrices rather than recomputing them, so an override
 * applied between the two survives into the vertices.
 */
void skl_transform_verts(MR_Skeleton *s)
{
	int i;

	if (!s || !s->bind || !s->mesh.vertices)
		return;

	/*
	 * RIGID BINDING: one member per vertex, no weights. The runs
	 * partition the pool exactly - on SPEED\CORPS.SKL the seventeen
	 * counts sum to 240, which is the vertex count - so every vertex
	 * is written once and none is left at the bind pose.
	 */
	for (i = 0; i < s->memberCount; i++) {
		float        inv[12];
		const float *m     = inv;
		int          first = s->member[i].firstVertex;
		int          count = s->member[i].vertexCount;
		int          k;

		/*
		 * THE VERTICES ARE TRANSFORMED BY THE INVERSE OF THE PART
		 * MATRIX, NOT BY THE MATRIX.
		 *
		 * world[i] is what the original keeps at record +0x18, and
		 * ours reproduces it to 0.0004 [verified against a runtime
		 * capture]. But maybe_Racer_Draw does NOT apply it. At
		 * 0x004615c1 it builds the transpose of the 3x3 with the
		 * translation negated through it - an exact inverse for an
		 * orthonormal matrix - and transforms the part's slice of
		 * the bind pool with THAT, composed with the view.
		 *
		 * o APPLYING IT FORWARD BENDS EVERY JOINT THE WRONG WAY.
		 *   Not subtly: the spine's -41 degrees leans the torso back
		 *   instead of forward, the left leg extends instead of
		 *   folding onto the peg, and the head swings off the neck.
		 *
		 * o THE TEST THAT SETTLED IT NEEDS NO RUNTIME. The .skl's
		 *   trailing group is 91 faces that all span two or more
		 *   members - the glue stitching the rigid parts together.
		 *   Their edges must stay about as short as the bind pose's,
		 *   because that is what stitching means. Measured:
		 *
		 *       bind pose      max 45.17   mean 18.82
		 *       forward        max 101.63  mean 26.76
		 *       inverse        max 50.33   mean 18.84
		 *
		 *   Forward doubles them. Inverse holds the model together.
		 *
		 * o AND IT EXPLAINS THE LONG DETOUR. Every check that
		 *   compared MATRICES passed, because our matrices were
		 *   right all along; only their application was wrong. A
		 *   test has to reach the vertices to see this.
		 */
		mat34_invert_rigid(inv, s->mat[i]);

		if (first < 0 || count <= 0 || first + count > s->mesh.vertexCount)
			continue;
		for (k = first; k < first + count; k++) {
			const MESH_VERTEX *v = &s->bind[k];

			s->mesh.vertices[k].x = v->x * m[0] + v->y * m[3] +
			                        v->z * m[6] + m[9];
			s->mesh.vertices[k].y = v->x * m[1] + v->y * m[4] +
			                        v->z * m[7] + m[10];
			s->mesh.vertices[k].z = v->x * m[2] + v->y * m[5] +
			                        v->z * m[8] + m[11];
		}
	}
}


/*
 * skl_refork_front - slide the front bodywork down the fork.
 *
 * ORIGIN  maybe_Racer_Draw @ 0x0045fc29..0x0045fdde, consumed at
 *         0x00461153
 *
 * THE ORIGINAL DOES NOT DRAW PART 15 WHERE THE WALK PUTS IT. After
 * the tree is built it recomputes that one part's pivot:
 *
 *     A = |pivot15 - pivot1|        the rest fork length
 *     L = |wheel   - pivot1|        the only runtime term
 *     B = L - 96 * rayonAv
 *     S = A * A / B
 *     pivot15' = pivot1 + S * normalize(pivot15 - pivot1)
 *
 * and stores R * (-origin15) + pivot15'. A member's matrix already
 * maps its own origin to its posed pivot, so that is a PURE
 * TRANSLATION by pivot15' - pivot15. Verified on a live capture:
 * the perpendicular distance from pivot 1 was unchanged at 45.2107
 * across the store and only the along-axis component moved,
 * -1.2286 -> -7.5981.
 *
 * A IS NOT AN INDEPENDENT TERM. The original measures |pivot15 -
 * pivot1| twice, once as A and once as |d|, so A == |d| and the
 * whole thing is S = L0*L0 / B. Transcribed that way rather than
 * as the original's two calls.
 *
 * PART 15 IS A LEAF - no member names member 2 as its parent - so
 * there is nothing below it to carry the shift into. That is why
 * one matrix is enough.
 *
 * See docs/subsystems/racers.txt 9.59.4.
 */


/*
 * skl_pivot_of - a member's pivot, WHICH IS ITS BIND ORIGIN.
 *
 * NOT the origin run through the member's own matrix. The original
 * keeps these as record[part].position and they hold the BIND value:
 * measured on page 10, record[16].position reads (0.08641, -32.73438,
 * -71.56070), which is member 4's bind origin exactly, while that
 * pose's matrix would put it 8.13 lower. The pose lives in the
 * MATRICES; the pivots stay put and the matrices turn about them.
 *
 * Posing them dropped the rear wheel by that 8.13.
 */
static void skl_pivot_of(const MR_Skeleton *s, int i, float out[3])
{
	out[0] = s->member[i].origin[0];
	out[1] = s->member[i].origin[1];
	out[2] = s->member[i].origin[2];
}


/*
 * skl_to_world - a point through the racer's world 3x4.
 *
 * ORIGIN  FUN_00410010 @ 0x00410010, out = point * matrix with the
 *         translation at matrix+0x24
 */
static void skl_to_world(float out[3], const float p[3],
                         const float m[12])
{
	out[0] = p[0] * m[0] + p[1] * m[3] + p[2] * m[6] + m[9];
	out[1] = p[0] * m[1] + p[1] * m[4] + p[2] * m[7] + m[10];
	out[2] = p[0] * m[2] + p[1] * m[5] + p[2] * m[8] + m[11];
}


/* ORIGIN  maybe_Racer_Draw @ 0x00460c43, drawn by FUN_0048c400 */
/* ORIGIN  Skl_BuildPartMatrices @ 0x00463d90 + FUN_00464190 */
static void skl_apply_source(MR_Skeleton *s, const SKL_AngleSource *src)
{
	if (!s || !s->bind || !s->mesh.vertices)
		return;

	skl_build_source(s, src);
	skl_transform_verts(s);
}


/* ------------------------------------------------------------------ */
/* THE POSE BLENDER. See skl.h for the frame layout and why it is not
 * MR_SklPose.                                                        */


/* ORIGIN  Anim_BlendAngleFrames @ 0x0045b4e0 */
void anim_blend_angle_frames(MR_AnimFrame *out, const MR_AnimFrame *a,
                             const MR_AnimFrame *b, float t)
{
	int n;
	int i;
	int k;

	if (!out || !a || !b)
		return;

	/* The SMALLER count wins - a short pose truncates the result. */
	n = (a->count <= b->count) ? a->count : b->count;
	if (n > ANIM_MAX_JOINTS)
		n = ANIM_MAX_JOINTS;
	out->count = n;

	if (t <= 0.0f) {
		/* Guarded against copying a frame onto itself. */
		if (out != a) {
			for (i = 0; i < n; i++)
				for (k = 0; k < 3; k++)
					out->angle[i][k] = a->angle[i][k];
		}
		return;
	}

	if (t >= 1.0f) {
		if (out != b) {
			for (i = 0; i < n; i++)
				for (k = 0; k < 3; k++)
					out->angle[i][k] = b->angle[i][k];
		}
		return;
	}

	for (i = 0; i < n; i++) {
		for (k = 0; k < 3; k++) {
			/*
			 * FLOAT, NOT DOUBLE. The original keeps the whole
			 * blend in one FPU float slot and compares against
			 * float 0.0 at 0x004d0e54; widening the intermediate
			 * moves the sign test and so moves the wrap.
			 */
			float x = a->angle[i][k];
			float y = b->angle[i][k];
			float r;

			/*
			 * THE SHORT WAY ROUND. Without this a joint at 350
			 * degrees blending to 10 sweeps backwards through the
			 * whole circle.
			 */
			if (y > x + (float)MR_PI)
				y -= (float)MR_TWO_PI;
			if (y < x - (float)MR_PI)
				y += (float)MR_TWO_PI;

			r = x + (y - x) * t;

			/*
			 * Normalised into [0, 2pi], not [-pi, pi].
			 *
			 * THIS RUNS ONLY ON THE BLEND PATH. Both copy paths
			 * above return without it, which is why a negative
			 * angle survives in MOTO: at rest every pass copies.
			 * A pass that blends here - even with a == b, where
			 * the lerp changes nothing - still wraps, and one
			 * wrap costs a trig index because the index cast
			 * truncates toward zero. See racer-draw-audit.txt 47.
			 */
			while (r < 0.0f)
				r += (float)MR_TWO_PI;
			while (r > (float)MR_TWO_PI)
				r -= (float)MR_TWO_PI;

			out->angle[i][k] = r;
		}
	}
}


/* ------------------------------------------------------------------ */
/* THE PILOT POSE SET. See skl.h and racers.txt 9.52.                  */

/* ORIGIN  Rot_Load @ 0x0045b340, the short-to-radian expansion */
void anim_frame_from_pose(MR_AnimFrame *out, const MR_SklPose *p)
{
	int i;
	int k;

	if (!out)
		return;

	out->count = SKL_ROT_MEMBERS;
	for (i = 0; i < ANIM_MAX_JOINTS; i++) {
		for (k = 0; k < 3; k++) {
			if (!p || !p->loaded || i >= SKL_ROT_MEMBERS) {
				out->angle[i][k] = 0.0f;
				continue;
			}
			/*
			 * NO REMAP HERE - the table is indexed by part; the
			 * permutation belongs to skl_apply_pose. See skl.h.
			 */
			out->angle[i][k] =
			    (float)((double)p->angle[i][k] * TRIG_TWO_PI
			            / (double)SKL_ROT_TURN);
		}
	}
}


/* ORIGIN  Bike_LoadPilotRots @ 0x00410a10, with the PilotRot list
 *         scan from maybe_Bike_ReadManifest @ 0x0042a120 */
int pilot_rots_load(MR_PilotRots *out, const char *manifest,
                    int firstRecord)
{
	MR_Script  sc;
	MR_SklPose pose;
	char       s[TRACK_PATH_MAX];
	char       logical[TRACK_PATH_MAX];
	int        i;

	if (!out || !manifest)
		return 0;

	memset(out, 0, sizeof *out);

	if (!bpt_open(&sc, manifest))
		return 0;

	/* Skip the model paths; the poses start at firstRecord. */
	for (i = 0; i < firstRecord; i++) {
		if (!bpt_read_string(&sc, s, (int)sizeof(s))) {
			mrlog_err("pilotrots: %s ended before record %d\n",
			          manifest, firstRecord);
			bpt_close(&sc);
			return 0;
		}
	}

	/*
	 * THE LIST IS TERMINATED BY A RECORD WITH NO '.' IN IT - the
	 * MotoCarac marker. There is no count anywhere.
	 */
	for (i = 0; i < PILOT_ROT_MAX; i++) {
		const char *path;

		if (!bpt_read_string(&sc, s, (int)sizeof(s)))
			break;
		if (strchr(s, '.') == NULL)
			break;

		path = s;

		/*
		 * "_N_name.rot" declares an N-FRAME ANIMATION. The digit is
		 * the run length and the path starts at character 3.
		 */
		if (s[0] == '_' && s[1] >= '0' && s[1] <= '9') {
			/*
			 * AND TWO MORE INTEGERS FOLLOW THE NAME.
			 *
			 * ORIGIN  maybe_Bike_ReadManifest @ 0x0042a1c3
			 *
			 *     0042a1e1  record+0x08 = the '_N_' digit - 0x30
			 *     0042a1ef  record+0x04 = the pose index
			 *     0042a1fe  Settings_ReadInt -> record+0x0c
			 *     0042a209  Settings_ReadInt -> record+0x00
			 *
			 * so the pair is DURATION then HAND SKELETON, in that
			 * order, per stunt entry. skl.h called them "two more
			 * integers ... which land either side of the frame
			 * count" and left them unread; without the duration
			 * racer_pose_stunt_step divides by zero's worth of
			 * time, returns 0, and the stunt ENDS ON THE FRAME IT
			 * STARTS - which is half of why no stunt was ever
			 * visible. The other half was the input edge; see
			 * race_stunt_edge.
			 *
			 * THEY ARE READ WHETHER OR NOT THE TABLE HAS ROOM,
			 * because they are in the STREAM - skipping them on a
			 * full table would desynchronise every later record.
			 */
			int dur = 0;
			int hand = 0;

			(void)bpt_read_int(&sc, &dur);
			(void)bpt_read_int(&sc, &hand);

			if (out->stuntCount < STUNT_POSE_MAX) {
				out->stunt[out->stuntCount].poseIndex  = i;
				out->stunt[out->stuntCount].frameCount =
				    s[1] - '0';
				out->stunt[out->stuntCount].durationMs   = dur;
				out->stunt[out->stuntCount].handSkeleton = hand;
				out->stuntCount++;
			}
			path = s + 3;
		}

		if (!asset_name_from_script(path, logical, (int)sizeof(logical), 0))
			continue;

		memset(&pose, 0, sizeof pose);
		if (!rot_load(&pose, logical)) {
			mrlog_err("pilotrots: %s failed\n", logical);
			continue;
		}
		anim_frame_from_pose(&out->frame[i], &pose);
	}

	bpt_close(&sc);
	out->frameCount = i;
	mrlog("pilotrots: %s -> %d frames, %d stunts\n",
	      manifest, out->frameCount, out->stuntCount);
	return out->frameCount;
}


/* ORIGIN  Rot_ApplyToRacer @ 0x0045b480 */
void skl_apply_pose(MR_Skeleton *s, const MR_SklPose *p)
{
	SKL_AngleSource src;

	src.pose = p;
	src.anim = NULL;
	skl_apply_source(s, &src);
}


/*
 * THE TWO-STEP FORM. Same walk, stopping after the matrices so the
 * caller can adjust one before it reaches the vertices. Call
 * skl_transform_verts to finish; skipping it leaves the mesh at
 * whatever the last pose put there.
 */
void skl_build_pose(MR_Skeleton *s, const MR_SklPose *p)
{
	SKL_AngleSource src;

	if (!s || !s->bind || !s->mesh.vertices)
		return;
	src.pose = p;
	src.anim = NULL;
	skl_build_source(s, &src);
}


