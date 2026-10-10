/*
 * mesh.c - the .3df geometry format and its paired .map.
 *
 * See mesh.h for scope and for the two scale factors. The format
 * write-up is docs/formats/mesh-3df-map.txt.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "mesh.h"
#include "assets.h"
#include "log.h"

/* .map record layout, offsets within each 128-byte record. */
#define MAP_HEADER_SIZE   0x200
#define MAP_RECORD_SIZE   0x80
#define MAP_OFF_N         0x00
#define MAP_OFF_COLOR     0x02
#define MAP_OFF_U0        0x22
#define MAP_OFF_V0        0x32
#define MAP_OFF_FLAGS     0x48
#define MAP_OFF_TEXID     0x4e

static unsigned int rd32(const unsigned char *p)
{
	return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
	       ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

static unsigned int rd16(const unsigned char *p)
{
	return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}

static float rdf32(const unsigned char *p)
{
	union { unsigned int i; float f; } u;
	u.i = rd32(p);
	return u.f;
}

/*
 * Read a whole asset. Caller frees. Goes through assets.c, so the
 * name is a LOGICAL ASSET NAME resolved inside a .BKF archive first -
 * CLAUDE.md rule 10. The parsers always worked from a buffer, so this
 * was the whole conversion.
 */
static unsigned char *read_file(const char *name, size_t *size_out)
{
	unsigned int n = 0;
	void        *buf = asset_load(name, &n);

	*size_out = (size_t)n;
	return (unsigned char *)buf;
}

/*
 * parse_3df - the geometry.
 *
 *     u16 version        always 1; the loader rejects anything else
 *     u16 vertexCount
 *     float32 x,y,z      per vertex
 *     face records, repeating:
 *         u16 n          vertices in this face; 0 ends the list
 *         u16 flags
 *         u16 index[n]
 *
 * The face list is walked twice: once to count, once to fill. The
 * original allocates 0x40 bytes per face up front because it knows the
 * count from elsewhere; we do not, so we count first.
 */
static int parse_3df(MESH *m, const unsigned char *buf, size_t size,
                     float scale, const char *path)
{
	size_t p;
	size_t faceStart;
	int    i;
	int    v;
	int    nfaces;

	if (size < 4) {
		mrlog_err("mesh: %s is too short\n", path);
		return 0;
	}
	if (rd16(buf) != 1) {
		mrlog("mesh: %s has version %u, expected 1\n", path, rd16(buf));
		return 0;
	}

	m->vertexCount = (int)rd16(buf + 2);
	p = 4;
	if (p + (size_t)m->vertexCount * 12u > size) {
		mrlog_err("mesh: %s truncated in the vertex array\n", path);
		return 0;
	}

	m->vertices = (MESH_VERTEX *)malloc((size_t)m->vertexCount *
	                                    sizeof(MESH_VERTEX));
	if (!m->vertices && m->vertexCount > 0)
		return 0;

	for (i = 0; i < m->vertexCount; i++) {
		m->vertices[i].x = rdf32(buf + p + 0);
		m->vertices[i].y = rdf32(buf + p + 4);
		m->vertices[i].z = rdf32(buf + p + 8);
		if (scale != 0.0f) {
			m->vertices[i].x *= scale;
			m->vertices[i].y *= scale;
			m->vertices[i].z *= scale;
		}
		p += 12;
	}

	/* First pass: count the faces. */
	faceStart = p;
	nfaces = 0;
	while (p + 4 <= size) {
		unsigned int n = rd16(buf + p);
		if (n == 0)
			break;
		if (n > MESH_MAX_FACE_VERTS) {
			/* The format permits it and the loader has a polygon path,
			 * but no shipped mesh uses it. Refuse rather than truncate
			 * silently. */
			mrlog("mesh: %s has a face with %u vertices; only up to %d "
			       "is handled\n", path, n, MESH_MAX_FACE_VERTS);
			return 0;
		}
		p += 4 + (size_t)n * 2u;
		if (p > size) {
			mrlog_err("mesh: %s truncated in the face list\n", path);
			return 0;
		}
		nfaces++;
	}

	m->faceCount = nfaces;
	m->faces = (MESH_FACE *)calloc((size_t)(nfaces > 0 ? nfaces : 1),
	                               sizeof(MESH_FACE));
	if (!m->faces)
		return 0;

	/*
	 * NO .map UNTIL ONE IS READ. calloc would leave mapRecordN at 0,
	 * which reads as "a record with n = 0" and would hand every
	 * map-less mesh the n < 3 flat colour - black, since mapColor is
	 * zero too. The sentinel has to be set explicitly.
	 */
	for (i = 0; i < nfaces; i++)
		m->faces[i].mapRecordN = -1;

	/* Second pass: fill them in. */
	p = faceStart;
	for (i = 0; i < nfaces; i++) {
		unsigned int n = rd16(buf + p);
		m->faces[i].count = (int)n;
		m->faces[i].flags = rd16(buf + p + 2);
		for (v = 0; v < (int)n; v++)
			m->faces[i].index[v] = (int)rd16(buf + p + 4 + (size_t)v * 2u);
		p += 4 + (size_t)n * 2u;
	}
	return 1;
}

void mesh_centre(const MESH *m, float out[3])
{
	float lo[3];
	float hi[3];
	int   i;

	if (!out)
		return;
	out[0] = 0.0f;
	out[1] = 0.0f;
	out[2] = 0.0f;
	if (!m || m->vertexCount <= 0 || !m->vertices)
		return;

	lo[0] = hi[0] = m->vertices[0].x;
	lo[1] = hi[1] = m->vertices[0].y;
	lo[2] = hi[2] = m->vertices[0].z;
	for (i = 1; i < m->vertexCount; i++) {
		const MESH_VERTEX *v = &m->vertices[i];

		if (v->x < lo[0]) lo[0] = v->x;
		if (v->x > hi[0]) hi[0] = v->x;
		if (v->y < lo[1]) lo[1] = v->y;
		if (v->y > hi[1]) hi[1] = v->y;
		if (v->z < lo[2]) lo[2] = v->z;
		if (v->z > hi[2]) hi[2] = v->z;
	}
	for (i = 0; i < 3; i++)
		out[i] = (lo[i] + hi[i]) * 0.5f;
}

int mesh_load(MESH *m, const char *logical, float scale)
{
	unsigned char *buf;
	size_t size;
	int ok;

	if (!m || !logical)
		return 0;
	memset(m, 0, sizeof(*m));

	buf = read_file(logical, &size);
	if (!buf)
		return 0;
	ok = parse_3df(m, buf, size, scale, logical);
	free(buf);
	if (!ok)
		mesh_free(m);
	return ok;
}

/*
 * FUN_00422070, the last thing the PAIRED loaders do - Load3dfAndMap at
 * 0x004215c4 and Mesh_LoadPair at 0x00421aba. Load3df, the mesh-only
 * loader, does NOT call it, so this belongs in the with-map path alone.
 * [verified]
 *
 * It is a HANDEDNESS CONVERSION and it does two halves that only make
 * sense together:
 *
 *   1  NEGATE X on every vertex. The original spells it as a matrix
 *      built from g_cosTable at 0, 0x1000, 0x2000 and 0x3000 - which
 *      are cos 0, cos pi/2, cos pi and cos 3pi/2, i.e. 1, 0, -1, 0 -
 *      so the nine products collapse to
 *
 *          x = -x     y = -(-y) = y     z = z
 *
 *      Read as literals rather than as a rotation, or the table
 *      indices look like a general transform and hide a sign.
 *
 *   2  REVERSE EVERY FACE, indices and UVs together, via
 *      Face_ReverseWinding.
 *
 * A mirror flips winding, so reversing after it puts the faces back
 * the right way round for the mirrored geometry. Doing ONE half is
 * worse than doing neither.
 *
 * THE NET WINDING OVER THE WHOLE LOAD IS REVERSED FROM FILE ORDER,
 * because the .3df loop reverses twice already:
 *
 *     Face_InitQuad / Face_InitTriangle   idx3, idx2, idx1, idx0
 *     Face_ReverseWinding                 back to file order
 *     .map rebuild via FUN_00422d20       order preserved
 *     THIS                                reversed again
 *
 * Miss this function and you get file order plus unmirrored geometry -
 * two wrongs that cancel on a flat billboard and cancel nowhere else.
 * That is exactly how it hid: page 14 is billboards, which are planar
 * in x, so negating x moves them 0.94 units and changes nothing
 * visible, while the winding alone decides which of the two coincident
 * faces is drawn. Page 25 draws closed solids and open shells, where
 * the mirror is the whole difference between a raised relief and a
 * hollow one.
 */
void mesh_mirror_x_flip_winding(MESH *m)
{
	int i;
	int k;

	for (i = 0; i < m->vertexCount; i++)
		m->vertices[i].x = -m->vertices[i].x;

	for (i = 0; i < m->faceCount; i++) {
		MESH_FACE *f = &m->faces[i];
		int        n = f->count;

		for (k = 0; k < n / 2; k++) {
			int   ti = f->index[k];
			float tu = f->u[k];
			float tv = f->v[k];

			f->index[k]         = f->index[n - 1 - k];
			f->u[k]             = f->u[n - 1 - k];
			f->v[k]             = f->v[n - 1 - k];
			f->index[n - 1 - k] = ti;
			f->u[n - 1 - k]     = tu;
			f->v[n - 1 - k]     = tv;
		}
	}
}

int mesh_load_with_map(MESH *m, const char *logical3df,
                       const char *logicalMap,
                       float scale)
{
	unsigned char *buf;
	size_t size;
	size_t need;
	int i;
	int v;

	if (!mesh_load(m, logical3df, scale))
		return 0;

	buf = read_file(logicalMap, &size);
	if (!buf) {
		mesh_free(m);
		return 0;
	}

	/*
	 * Only one byte of the 512-byte header is tested: byte 1 must have
	 * bit 0x80 set, or the game reports the file version as too old.
	 * Observed files begin 01 80.
	 */
	if (size < MAP_HEADER_SIZE || (buf[1] & 0x80) == 0) {
		mrlog("mesh: %s is not a usable map (header byte 1 = 0x%02x)\n",
		       logicalMap, size >= 2 ? buf[1] : 0);
		free(buf);
		mesh_free(m);
		return 0;
	}

	need = (size_t)MAP_HEADER_SIZE +
	       (size_t)m->faceCount * (size_t)MAP_RECORD_SIZE;
	if (size < need) {
		mrlog("mesh: %s holds %u bytes, needs %u for %d faces\n",
		       logicalMap, (unsigned)size, (unsigned)need, m->faceCount);
		free(buf);
		mesh_free(m);
		return 0;
	}
	/*
	 * Anything past that is ignored. 123 of the 124 shipped maps carry
	 * one extra unread 128-byte block and one does not, so the padding
	 * is conventional rather than structural - a reader must not
	 * require it.
	 */

	for (i = 0; i < m->faceCount; i++) {
		const unsigned char *r = buf + MAP_HEADER_SIZE +
		                         (size_t)i * MAP_RECORD_SIZE;
		MESH_FACE *f = &m->faces[i];
		unsigned int n = rd16(r + MAP_OFF_N);

		/*
		 * THE GAME'S OWN CONSISTENCY RULE, and it is narrower than it
		 * looks. Only records with n >= 3 are validated; a record with
		 * n < 3 takes a separate path that never reads UVs and never
		 * compares against the mesh. Checking those too reports false
		 * failures on shipped data - 2ND has four such records against
		 * quad faces and 3RD has one.
		 */
		if (n >= 3) {
			unsigned int want = (f->count >= 4) ? 4u : 3u;
			if (n != want) {
				mrlog("mesh: %s record %d has n=%u, face has %d\n",
				       logicalMap, i, n, f->count);
				free(buf);
				mesh_free(m);
				return 0;
			}
			for (v = 0; v < MESH_MAX_FACE_VERTS; v++) {
				f->u[v] = (float)(int)rd32(r + MAP_OFF_U0 + v * 4);
				f->v[v] = (float)(int)rd32(r + MAP_OFF_V0 + v * 4);
			}
			f->hasMap = 1;
		}

		f->mapFlags   = rd32(r + MAP_OFF_FLAGS);
		f->bank       = (int)(f->mapFlags & 0xffffu);
		f->textureId  = rd32(r + MAP_OFF_TEXID);

		/*
		 * READ FOR EVERY RECORD, n < 3 included. hasMap above is set
		 * only for n >= 3 because it gates the UVs, and using it to
		 * gate these two as well would throw away the only colour an
		 * n < 3 face has.
		 */
		f->mapRecordN = (int)n;
		f->mapColor   = (unsigned short)rd16(r + MAP_OFF_COLOR);
	}

	free(buf);

	/* The original runs this last, after the map and before the face
	 * normals. See the comment on the function. */
	mesh_mirror_x_flip_winding(m);
	return 1;
}

/*
 * ORIGIN  Mesh_ComputeFaceNormal @ 0x0041f920
 *
 * The cross product of two edges of the face's first three vertices,
 * normalised. Written out in the original's own term order rather
 * than as a tidy cross product, because the sign convention is the
 * whole content of the function and a rearranged expression is one
 * transposition away from lighting the mesh from behind.
 */
static void mesh_face_normal(MESH *m, int face)
{
	const MESH_FACE   *f  = &m->faces[face];
	const MESH_VERTEX *a  = &m->vertices[f->index[0]];
	const MESH_VERTEX *b  = &m->vertices[f->index[1]];
	const MESH_VERTEX *c  = &m->vertices[f->index[2]];
	float             *n  = m->faceNormals + (size_t)face * 3;
	float              len;

	n[0] = (c->z - a->z) * (b->y - a->y) - (b->z - a->z) * (c->y - a->y);
	n[1] = (b->z - a->z) * (c->x - a->x) - (c->z - a->z) * (b->x - a->x);
	n[2] = (c->y - a->y) * (b->x - a->x) - (b->y - a->y) * (c->x - a->x);

	/*
	 * THE ORIGINAL DOES NOT GUARD THIS - 0x0041f9d8 divides by the
	 * length unconditionally, so a degenerate face gives it an
	 * infinity. We guard, because a nan here propagates through the
	 * vertex sum into every vertex the face touches and out to the
	 * rasterizer, and there is no shipped mesh where the two
	 * behaviours differ.
	 */
	len = (float)sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
	if (len != 0.0f) {
		n[0] /= len;
		n[1] /= len;
		n[2] /= len;
	}
}

/* ORIGIN  Mesh_ComputeVertexNormals @ 0x0041fa10 */
void mesh_compute_vertex_normals(MESH *m)
{
	int *counts;
	int  i, f, k;

	if (!m || m->vertexCount <= 0 || m->faceCount <= 0)
		return;
	if (!m->vertices || !m->faces)
		return;

	free(m->vertexNormals);
	m->vertexNormals = NULL;

	if (!m->faceNormals) {
		m->faceNormals = (float *)calloc((size_t)m->faceCount * 3,
		                                 sizeof(float));
		if (!m->faceNormals)
			return;
		for (f = 0; f < m->faceCount; f++)
			mesh_face_normal(m, f);
	}

	m->vertexNormals = (float *)calloc((size_t)m->vertexCount * 3,
	                                   sizeof(float));
	counts = (int *)calloc((size_t)m->vertexCount, sizeof(int));
	if (!m->vertexNormals || !counts) {
		free(counts);
		free(m->vertexNormals);
		m->vertexNormals = NULL;
		return;
	}

	/*
	 * IT IS THE FACE'S OWN VERTEX COUNT, and the original reads it
	 * as "3, or 4 when the wrap slot at +0x34 is not 0xffff" rather
	 * than from a count field. MESH_FACE.count is that same number.
	 */
	for (f = 0; f < m->faceCount; f++) {
		const MESH_FACE *fc = &m->faces[f];
		const float     *fn = m->faceNormals + (size_t)f * 3;
		int              n  = (fc->count > 4) ? 4 : fc->count;

		for (i = 0; i < n; i++) {
			int vi = fc->index[i];

			if (vi < 0 || vi >= m->vertexCount)
				continue;
			for (k = 0; k < 3; k++)
				m->vertexNormals[(size_t)vi * 3 + k] += fn[k];
			counts[vi]++;
		}
	}

	for (i = 0; i < m->vertexCount; i++) {
		float *n = m->vertexNormals + (size_t)i * 3;
		float  len;

		/*
		 * A VERTEX NO FACE TOUCHES divides by zero in the original
		 * and comes out nan. Skipping it leaves the zero the
		 * allocation gave it, which the shade loop then reads as a
		 * dot of 0 - a mid-grey rather than a nan. No shipped mesh
		 * has one.
		 */
		if (counts[i] > 0) {
			float inv = 1.0f / (float)counts[i];

			for (k = 0; k < 3; k++)
				n[k] *= inv;
		}

		len = (float)sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
		if (len != 0.0f) {
			float inv = 1.0f / len;

			for (k = 0; k < 3; k++)
				n[k] *= inv;
		}
	}

	free(counts);
}

/*
 * ORIGIN  maybe_Mesh_ShadeVertices @ 0x00422540
 *
 * See mesh.h for why a wheel is never as bright as the bike.
 *
 * THE MATRIX IS THE INSTANCE'S OWN ORIENTATION - for a wheel, the
 * one carrying its spin - and NOT the camera. See mesh.h for how
 * the call site settles that; getting it wrong paints the shading
 * into mesh space so it revolves with the tyre.
 *
 * THE LIGHT TRANSFORM IS SIMPLER THAN IT LOOKS IN THE DECOMPILER.
 * The head builds the transpose of the 3x3 into a local, then
 * multiplies the light through it TRANSPOSED AGAIN at 0x00422650
 * onward, so the two cancel: what comes out is the matrix's own
 * rotation applied to the light about its translation,
 *
 *     L'[i] = row_i(orient) . (L - t)
 *
 * with t = orient[9..11]. Written that way here. Do not "restore"
 * the transpose - it is already accounted for.
 *
 * TWO TRUNCATIONS, NOT ONE, and they are the reason the range is
 * 8..24. Sixteen bands come out of the dot product and are then
 * halved into eight, so the value is quantised twice; a single
 * multiply-and-round gives different bands and the 0x1f clamp - which
 * cannot fire for a normalised normal - starts to.
 */
void mesh_shade_vertices(const MESH *m, const float light[3],
                         const float orient[12], MR_Vertex *verts)
{
	float lx, ly, lz, len;
	float l[3];
	int   i;

	if (!m || !light || !orient || !verts || !m->vertexNormals)
		return;
	if (m->vertexCount <= 0)
		return;

	for (i = 0; i < 3; i++)
		l[i] = orient[i * 3 + 0] * (light[0] - orient[9])
	         + orient[i * 3 + 1] * (light[1] - orient[10])
	         + orient[i * 3 + 2] * (light[2] - orient[11]);

	/* 0x004d0664 is 0.0f, and the test is for equality, not a
	 * tolerance. */
	len = (float)sqrt(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
	if (len != 0.0f) {
		float inv = 1.0f / len;

		for (i = 0; i < 3; i++)
			l[i] *= inv;
	}

	lx = -l[0];
	ly = -l[1];
	lz = -l[2];

	for (i = 0; i < m->vertexCount; i++) {
		const float *n = m->vertexNormals + (size_t)i * 3;
		float        d = n[2] * lz + n[1] * ly + n[0] * lx;
		int          band;
		int          s;

		/* 16.0f at 0x004d069c, 1.0f at 0x004d06a0, 0.5f at
		 * 0x004d0680. */
		band = (int)(16.0f - d * 16.0f);
		s    = (int)(((float)band + 1.0f) * 0.5f);
		s   += 8;
		if (s >= 0x20)
			s = 0x1f;
		verts[i].shade = s << 13;
	}
}

void mesh_free(MESH *m)
{
	if (!m)
		return;
	if (m->vertices) free(m->vertices);
	if (m->faces)    free(m->faces);
	/* Mesh_Free frees both regardless, which is correct given they
	 * are initialised to 0 and most meshes never gain them. */
	free(m->faceNormals);
	free(m->vertexNormals);
	memset(m, 0, sizeof(*m));
}

/*
 * Mesh_Scale (0x004204b0). See mesh.h for what is not reproduced.
 */
void mesh_center_on_bounds(MESH *m, float outCentre[3])
{
	float mn[3];
	float mx[3];
	float c[3];
	int   i;
	int   k;

	if (outCentre) {
		outCentre[0] = 0.0f;
		outCentre[1] = 0.0f;
		outCentre[2] = 0.0f;
	}
	if (!m || !m->vertices || m->vertexCount <= 0)
		return;

	/*
	 * The original seeds with +/- 999999 rather than the first vertex.
	 * Kept: a mesh whose coordinates exceed that would behave the same
	 * way there as here, and matching the seed matches the failure.
	 */
	for (k = 0; k < 3; k++) {
		mn[k] =  999999.0f;
		mx[k] = -999999.0f;
	}
	for (i = 0; i < m->vertexCount; i++) {
		const float *v = &m->vertices[i].x;
		for (k = 0; k < 3; k++) {
			if (v[k] > mx[k]) mx[k] = v[k];
			if (v[k] < mn[k]) mn[k] = v[k];
		}
	}

	for (k = 0; k < 3; k++)
		c[k] = (mn[k] + mx[k]) * 0.5f;

	for (i = 0; i < m->vertexCount; i++) {
		float *v = &m->vertices[i].x;
		for (k = 0; k < 3; k++)
			v[k] -= c[k];
	}

	if (outCentre) {
		outCentre[0] = c[0];
		outCentre[1] = c[1];
		outCentre[2] = c[2];
	}

	/*
	 * The original recomputes every face normal and plane distance
	 * here. Ours does not carry per-face normals - the cull works from
	 * screen-space and camera-space cross products, never from a stored
	 * normal - so there is nothing to bring back into step. Recorded so
	 * that whoever adds stored normals knows this is one of the places
	 * that has to maintain them.
	 */
}

void mesh_scale(MESH *m, float sx, float sy, float sz)
{
	int i;

	if (!m || !m->vertices)
		return;
	for (i = 0; i < m->vertexCount; i++) {
		m->vertices[i].x *= sx;
		m->vertices[i].y *= sy;
		m->vertices[i].z *= sz;
	}
}
