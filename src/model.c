/*
 * model.c - see model.h.
 */

#include <windows.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

#include "model.h"
#include "trig.h"
#include "transform.h"
#include "cull.h"
#include "prim.h"
#include "log.h"

/*
 * ModelInstance_Init (0x0048bce0).
 *
 * The original writes about twenty other dwords - three flags set to 1
 * at +0x44, +0x58 and +0x88, a 1.0f at +0x94, six consecutive dwords
 * cleared at +0x70 and the rest zeroed. None of them has a known
 * reader, so none is reproduced. What IS reproduced is the part
 * ModelInstance_Draw goes on to read.
 */
/* ORIGIN  ModelInstance_Init @ 0x0048bce0 */
void model_init(MR_ModelInstance *m, const float pos[3],
                const float rot[3], MESH *mesh)
{
	if (!m)
		return;
	memset(m, 0, sizeof(*m));
	if (rot) {
		m->rotation[0] = rot[0];
		m->rotation[1] = rot[1];
		m->rotation[2] = rot[2];
	}
	if (pos) {
		m->position[0] = pos[0];
		m->position[1] = pos[1];
		m->position[2] = pos[2];
	}
	m->mesh        = mesh;
	/*
	 * ModelInstance_Init ZEROES it like every other dword, and
	 * 0 is a real livery. Racer_Create overwrites it straight
	 * after; anything else that never sets it draws with
	 * palette 0, which is the bank default.
	 */
	m->livery = 0;
	m->prims       = NULL;
	m->primCount   = 0;
}

void model_set_prims(MR_ModelInstance *m, MR_Primitive *prims, int count)
{
	if (!m)
		return;
	m->prims     = prims;
	m->primCount = count;
}

/*
 * THE ANGLE CONVERSION, read from the disassembly at 0x0048c060:
 *
 *     FLD   float ptr [ESI]            the angle, in radians
 *     FMUL  double ptr [0x004d1bf8]    TRIG_IDX_PER_RAD
 *     CALL  __ftol                     truncate to an index
 *     ECX = idx & 0x3fff               cos
 *     EAX = (idx - 0x1000) & 0x3fff    sin
 *
 * THE TABLE AND THE CONVERSION ARE DERIVED FROM THE SAME TYPED 2*pi,
 * and that is a consistency check on both: a table built from one
 * 2*pi and indexed by a conversion derived from another would drift
 * by half an entry at the far end. They match. See trig.h.
 */

/* ORIGIN  Mat34_ToEuler @ 0x0046d270 */
void mat34_to_euler(float out[3], const float m[12])
{
	double c, a, b, p, y;

	if (!out || !m)
		return;

	/*
	 * c IS THE COSINE OF THE PITCH, recovered from row 2 rather
	 * than from another element - 1 - m[6]^2 under a square root.
	 * 0x004d0ec8 is the 1.0 and 0x004d0e50 the 1e-5 the result is
	 * tested against.
	 */
	c = sqrt(1.0 - (double)m[6] * m[6]);

	if (c > 1e-5) {
		p = atan2((double)m[6], c);
		y = atan2(-(double)m[7] / c, (double)m[8] / c);
		a = (double)m[0] / c;
		b = -((double)m[3] / c);
	} else {
		/*
		 * GIMBAL LOCK, and the original picks the sign off the
		 * FLOAT BIT of m[6] - CMP EAX,0x80000000 / JBE - not a
		 * numeric comparison. Same answer for every value except
		 * -0.0, which takes the negative branch here and the
		 * positive one under a plain m[6] < 0 test.
		 */
		p = (m[6] < 0.0f) ? -MR_HALF_PI_F : MR_HALF_PI_F;
		y = 0.0;
		a = (double)m[4];
		b = (double)m[1];
	}

	out[0] = (float)y;
	out[1] = (float)p;
	out[2] = (float)atan2(b, a);
}


/*
 * ORIGIN  FUN_00403880 @ 0x00403880
 *
 * THE DECOR EULER, AND IT IS NOT THE MODEL INSTANCES'. Props are
 * drawn by FUN_004022f0 - vtable+0x0c of the decor class at
 * 0x004d0110 - and it calls this, never ModelInstance_Draw's inline
 * block. The port used mat34_from_euler for both, which is the
 * racer's convention, and every prop on every track was oriented by
 * the wrong matrix.
 *
 * IT IS transpose(Ry(rot[1]) . Rx(rot[0]) . Rz(rot[2])), pinned by
 * hooking 0x00403994 with the input forced to (0.3, 0.7, 1.1) on
 * every decor object and fitting all 288 order/sign/transpose
 * combinations to the nine captured floats:
 *
 *     this form        total error 0.00072   - the cosine table
 *     mat34_from_euler total error 0.69161
 *
 * so the two are not the same matrix and no relabelling makes them
 * one.
 *
 *     out0 = cb*cr + sb*sp*sr      out1 = cp*sr      out2 = -sb*cr + cb*sp*sr
 *     out3 = -cb*sr + sb*sp*cr     out4 = cp*cr      out5 =  sb*sr + cb*sp*cr
 *     out6 = sb*cp                 out7 = -sp        out8 =  cb*cp
 *
 * WHY IT WENT UNNOTICED: a palm carries yaw only, and with pitch and
 * roll at zero both conventions agree. It took a Corsair whose mesh
 * sits 17000 units off its own origin - so twenty degrees of tilt is
 * 5800 units of altitude - to make it visible, and then it read as
 * an aeroplane flying through a hill.
 */
void mat34_from_euler_decor(float out[9], const float rot[3])
{
	int   ip, ib, ir;
	float cp, sp, cb, sb, cr, sr;

	if (!out || !rot)
		return;

	ip = (int)(rot[0] * TRIG_IDX_PER_RAD);
	ib = (int)(rot[1] * TRIG_IDX_PER_RAD);
	ir = (int)(rot[2] * TRIG_IDX_PER_RAD);

	cp = trig_cos(ip);  sp = trig_sin(ip);
	cb = trig_cos(ib);  sb = trig_sin(ib);
	cr = trig_cos(ir);  sr = trig_sin(ir);

	out[0] =  cb * cr + sb * sp * sr;
	out[1] =  cp * sr;
	out[2] = -sb * cr + cb * sp * sr;
	out[3] = -cb * sr + sb * sp * cr;
	out[4] =  cp * cr;
	out[5] =  sb * sr + cb * sp * cr;
	out[6] =  sb * cp;
	out[7] = -sp;
	out[8] =  cb * cp;
}


void mat34_from_euler(float out[9], const float rot[3])
{
	int   i0, i1, i2;
	float c0, s0, c1, s1, c2, s2;

	if (!out || !rot)
		return;

	/* __ftol truncates toward zero; a plain cast is the same. */
	i0 = (int)(rot[0] * TRIG_IDX_PER_RAD);
	i1 = (int)(rot[1] * TRIG_IDX_PER_RAD);
	i2 = (int)(rot[2] * TRIG_IDX_PER_RAD);

	c0 = trig_cos(i0);  s0 = trig_sin(i0);
	c1 = trig_cos(i1);  s1 = trig_sin(i1);
	c2 = trig_cos(i2);  s2 = trig_sin(i2);

	/*
	 * The nine products, from the x87 block at 0x0048c0fb..0x0048c187
	 * inside ModelInstance_Draw.
	 *
	 * o AN EARLIER VERSION OF THIS FUNCTION WAS WRONG, and it is worth
	 *   knowing how, because both of the ways it was "checked" passed.
	 *
	 *   It had rot[0] and rot[1] driving each other's terms. That form
	 *   is ALSO orthonormal with determinant +1 for every input - a
	 *   rotation matrix, just not this one - so the orthonormality
	 *   argument that used to sit in this comment proved nothing. And
	 *   the decompiler appeared to confirm it, because Ghidra does not
	 *   model the x87 stack: its extraout_ST0 / extraout_ST1 names are
	 *   guesses, and reading them as though they were real variables
	 *   reproduces whatever you already believed.
	 *
	 *   What settled it was simulating the instruction sequence
	 *   symbolically, stack and all, and then checking the result
	 *   against something physical: every flat billboard mesh in the
	 *   game is planar in X (ellipse, moto, racer, dsint, cam,
	 *   cadre1), so the rest rotation (0, pi/2, 0) has to be the 90
	 *   degree turn that faces one at a camera on +Z. The old form
	 *   returned the IDENTITY for that input and left every billboard
	 *   edge-on; this one maps local X to (0, 0, -1), face-on.
	 *
	 * o THE VISIBLE CONSEQUENCE was page 14. With the old matrix the
	 *   title animation's two halves rotated about the wrong axes -
	 *   vertical then in-plane, the second half spinning the quad
	 *   about its own normal while edge-on, which is why it collapsed
	 *   to a knife-edge sliver and never came back. With this one the
	 *   first half flips about the HORIZONTAL axis and the second
	 *   rotates about the VERTICAL, which is what the retail recording
	 *   shows.
	 *
	 * Angles: 0 is rot[0], 1 is rot[1], 2 is rot[2].
	 */
	out[0] =  c2 * c1;
	out[1] =  s2 * c0 + c2 * s1 * s0;
	out[2] =  s2 * s0 - c2 * s1 * c0;
	out[3] = -s2 * c1;
	out[4] =  c2 * c0 - s2 * s1 * s0;
	out[5] =  s2 * s1 * c0 + c2 * s0;
	out[6] =  s1;
	out[7] = -c1 * s0;
	out[8] =  c1 * c0;
}

/*
 * Matrix34_Multiply (0x00403a80), transcribed.
 *
 * EVERY LOAD BEFORE EVERY STORE, so out may alias a or b. The original
 * is fully unrolled with 69 float temporaries for exactly that reason;
 * the locals here do the same job.
 */
/* ORIGIN  Matrix34_Multiply @ 0x00403a80 */
void mat34_multiply(float out[12], const float a[12], const float b[12])
{
	float r[12];
	int   i;

	if (!out || !a || !b)
		return;

	for (i = 0; i < 3; i++) {
		r[i * 3 + 0] = a[i * 3 + 0] * b[0] + a[i * 3 + 1] * b[3] +
		               a[i * 3 + 2] * b[6];
		r[i * 3 + 1] = a[i * 3 + 0] * b[1] + a[i * 3 + 1] * b[4] +
		               a[i * 3 + 2] * b[7];
		r[i * 3 + 2] = a[i * 3 + 0] * b[2] + a[i * 3 + 1] * b[5] +
		               a[i * 3 + 2] * b[8];
	}
	/* The translation row is a point, so b's translation is added. */
	r[9]  = a[9] * b[0] + a[10] * b[3] + a[11] * b[6] + b[9];
	r[10] = a[9] * b[1] + a[10] * b[4] + a[11] * b[7] + b[10];
	r[11] = a[9] * b[2] + a[10] * b[5] + a[11] * b[8] + b[11];

	for (i = 0; i < 12; i++)
		out[i] = r[i];
}

/*
 * ModelInstance_Draw (0x0048c030), transcribed.
 *
 *     dp = this->position - camera->position
 *     R  = euler(this->rotation)
 *     M  = R * view,  translation = dp * view + view.translation
 *     verts = transform_verts_flat(M, mesh->vertices,
 *                                  mesh->vertexCount, 0x1f)
 *     cull_and_queue_faces(verts, mesh->faces, mesh->faceCount)
 *
 * THE POSITION IS RELATIVE TO THE CAMERA BEFORE THE VIEW MATRIX IS
 * APPLIED. The original subtracts camera+0x98 from the instance's
 * position and only then multiplies. That keeps the numbers small
 * around the origin, which matters for a single-precision pipeline on
 * a large track, and it is why the view matrix's own translation is
 * added rather than being the whole story.
 *
 * o THE SHADE IS A LITERAL 0x1f - the brightest of the 32 levels, flat
 *   across every vertex. Model instances are not lit.
 *
 * o THE FACES DO NOT COME FROM THE MESH HERE, and that is a defect in
 *   our mesh.c rather than a divergence from the original. See the
 *   note in model.h. An earlier version of this function cast
 *   mesh->faces to MR_Primitive* - it COMPILED, because both are
 *   pointers, and it would have fed the queue a differently-shaped
 *   struct. Caught by checking the two layouts rather than the build.
 *
 * o THE LIVERY IS PASSED NOW. It used not to be, and the field was
 *   called detailLevel; both were wrong. The original selects
 *   between Screen3D_CullAndQueueFaces with the livery and
 *   Screen3D_CullAndQueueFacesEx with 1 on its SIGN, and ours
 *   folds the negative case into the one function because our Ex
 *   is still a stub. See docs/subsystems/liveries.txt.
 */
/* ORIGIN  ModelInstance_Draw @ 0x0048c030 */
/*
 * bike_draw - the single place a bike is drawn. See model.h.
 *
 * ORIGIN  maybe_Racer_Draw @ 0x0045f2c0, which draws the body and
 *         both wheels in one pass off one orientation
 */
/* ORIGIN  maybe_Racer_Draw @ 0x0045f290, built inline at 0x004615c1 */
/*
 * ORIGIN  maybe_Mat34_LookAt @ 0x0046bdc0
 *
 * NOT camera_look_at. That is FUN_0046e140, it builds a VIEW matrix
 * as a yaw times a pitch, and it is the camera's. This one builds a
 * frame whose row 2 IS the direction from -> to, and it is what the
 * AI line follower aims with.
 *
 *     d    = normalise(to - from)
 *     lat  = normalise(( d.z, 0, -d.x ))
 *     up   = normalise(( -d.x*d.y, d.z*d.z + d.x*d.x, -d.z*d.y ))
 *     rows = lat, up, d, (0,0,0)
 *
 * THE up ROW IS NOT A CROSS PRODUCT WRITTEN AS ONE. The original
 * spells it out in components off local_14 = -d.x and local_38 = d.z,
 * which is the same vector a cross would give but arrives by a
 * different route; it is transcribed as written rather than tidied,
 * because the two disagree on sign when d is vertical.
 *
 * BOTH NORMALISATIONS ARE GUARDED and simply do not write when the
 * length is zero, so a degenerate aim leaves the previous row in
 * place rather than producing NaN.
 */
void mat34_look_at(float out[12], const float from[3], const float to[3])
{
	float d[3];
	float lat[3];
	float up[3];
	float len;
	int   i;

	if (!out || !from || !to)
		return;

	for (i = 0; i < 3; i++)
		d[i] = to[i] - from[i];

	len = (float)sqrt((double)d[0] * d[0] + (double)d[1] * d[1]
	                  + (double)d[2] * d[2]);
	if (len != 0.0f) {
		for (i = 0; i < 3; i++)
			d[i] = (float)(d[i] / len);
	}

	/* local_14 is -d.x and local_1c is d.z, in the original's names. */
	lat[0] = d[2];
	lat[1] = 0.0f;
	lat[2] = -d[0];

	up[0] = -d[0] * d[1];
	up[1] = d[2] * d[2] + d[0] * d[0];
	up[2] = -(d[2] * d[1]);

	len = (float)sqrt((double)d[0] * d[0] + (double)d[2] * d[2]);
	if (len != 0.0f) {
		lat[0] = (float)(d[2] / len);
		lat[2] = (float)(-d[0] / len);
	}

	len = (float)sqrt((double)up[0] * up[0] + (double)up[1] * up[1]
	                  + (double)up[2] * up[2]);
	if (len != 0.0f) {
		for (i = 0; i < 3; i++)
			up[i] = (float)(up[i] / len);
	}

	out[0] = lat[0]; out[1] = lat[1]; out[2] = lat[2];
	out[3] = up[0];  out[4] = up[1];  out[5] = up[2];
	out[6] = d[0];   out[7] = d[1];   out[8] = d[2];
	out[9] = 0.0f;   out[10] = 0.0f;  out[11] = 0.0f;
}


void mat34_invert_rigid(float out[12], const float m[12])
{
	out[0] = m[0];  out[1] = m[3];  out[2] = m[6];
	out[3] = m[1];  out[4] = m[4];  out[5] = m[7];
	out[6] = m[2];  out[7] = m[5];  out[8] = m[8];

	out[9]  = -(m[0] * m[9] + m[1] * m[10] + m[2] * m[11]);
	out[10] = -(m[3] * m[9] + m[4] * m[10] + m[5] * m[11]);
	out[11] = -(m[6] * m[9] + m[7] * m[10] + m[8] * m[11]);
}


/*
 * bike_draw_body_parts - the body, one member at a time.
 *
 * ORIGIN  maybe_Racer_Draw @ 0x0045f290, the vertex loop whose
 *         transform call is at 0x0046197a and the face loop at
 *         0x004619f7
 *
 * pv is the placement already composed with the view, so the only
 * per-member work is the inverse and one more multiply - which is
 * what the original does inside its loop.
 *
 * THE BASE COMES FROM THE FIRST SLICE, not from the pool. The pool is
 * a bump allocator shared with everything else drawn this frame, so
 * the bike's vertices start wherever the previous object left off;
 * subtracting firstVertex turns that into the origin the face indices
 * are relative to. The original keeps the same pointer for the same
 * reason - see racers.txt 9.64.
 */
/*
 * bike_draw AND ITS TWO HELPERS ARE GONE, and so is MR_BikeDraw.
 *
 * THEY HAD NO ORIGINAL. The layer sat between the racer draw and the
 * transform and made two choices maybe_Racer_Draw does not: it
 * inverted EVERY member matrix unconditionally, where the original
 * special-cases counters 0 and 3 out of the inverse; and it
 * multiplied every part by a matrix carrying the placement, so part
 * matrices here were in MODEL space while the original walks from the
 * BODY matrix and has them already placed.
 *
 * That mismatch is why three attempts to wire the imported blocks in
 * at this seam each made the picture worse. racerdraw_racer owns the
 * whole per-racer draw now - transform and face queue included - and
 * both the race and page 10 go through it, as all three of the
 * original callers go through maybe_Racer_Draw.
 *
 * See docs/subsystems/racer-draw-pipeline.txt.
 */



MR_Vertex *model_draw(const MR_ModelInstance *m, const float *view34,
                      const float camPos[3])
{
	float rot[9];
	float local[12];
	float composed[12];
	float dp[3];
	MR_Vertex *verts;
	int i;

	if (!m || !m->mesh || !view34 || !camPos)
		return NULL;
	if (m->mesh->vertexCount <= 0)
		return NULL;
	if (!m->prims || m->primCount <= 0) {
		/*
		 * Once, not per frame. An instance with a mesh but no
		 * primitives is a caller mistake, not a normal state, and it
		 * is otherwise indistinguishable from "the renderer is
		 * broken" - which is exactly how it presented the first time.
		 */
		static int warned;
		if (!warned) {
			warned = 1;
			mrlog_err("model: instance has a mesh but no primitives - "
			       "nothing will draw. See model.h.\n");
		}
		return NULL;
	}

	dp[0] = m->position[0] - camPos[0];
	dp[1] = m->position[1] - camPos[1];
	dp[2] = m->position[2] - camPos[2];

	mat34_from_euler(rot, m->rotation);
	for (i = 0; i < 9; i++)
		local[i] = rot[i];
	local[9]  = dp[0];
	local[10] = dp[1];
	local[11] = dp[2];

	mat34_multiply(composed, local, view34);

	/*
	 * MESH_VERTEX is exactly three floats, so this cast is the layout
	 * and not an assertion about it - unlike the faces, which are a
	 * different type entirely.
	 */
	verts = transform_verts_flat(composed, (const float *)m->mesh->vertices,
	                             m->mesh->vertexCount, 0x1f);
	if (!verts)
		return NULL;
	cull_and_queue_faces(verts, m->prims, m->primCount,
	                     m->livery);
	return verts;
}

/*
 * FUN_0046e140, transcribed. See model.h for why identity is wrong.
 *
 * The two matrices are built as literals in the original and then
 * multiplied, rather than being composed by hand - so this keeps the
 * same shape, including the zero translation on both.
 */
int camera_look_at(float view[12], const float camPos[3],
                   const float target[3])
{
	float d[3];
	float len, hyp;
	float a[12];
	float b[12];
	int   i;

	if (!view || !camPos || !target)
		return 0;

	d[0] = target[0] - camPos[0];
	d[1] = target[1] - camPos[1];
	d[2] = target[2] - camPos[2];

	len = (float)sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
	if (len == 0.0f)
		return 0;
	d[0] /= len;
	d[1] /= len;
	d[2] /= len;

	hyp = (float)sqrt(d[0] * d[0] + d[2] * d[2]);
	if (hyp == 0.0f)
		return 0;               /* the original's guard: it writes nothing */

	for (i = 0; i < 12; i++) {
		a[i] = 0.0f;
		b[i] = 0.0f;
	}

	/* A - yaw about the vertical axis. */
	a[0] =  d[2] / hyp;
	a[2] =  d[0] / hyp;
	a[4] =  1.0f;
	a[6] = -d[0] / hyp;
	a[8] =  d[2] / hyp;

	/* B - pitch. hyp and d[1] are a cosine/sine pair, since d is a
	 * unit vector and hyp^2 + d[1]^2 == 1. */
	b[0] =  1.0f;
	b[4] =  hyp;
	b[5] =  d[1];
	b[7] = -d[1];
	b[8] =  hyp;

	mat34_multiply(view, a, b);
	return 1;
}

/*
 * ORIGIN  FUN_00490220 @ 0x00490220
 *
 * THE CAMERA'S YAW, AND IT IS THE TAIL OF Camera_LookAt. The original
 * calls this with ECX = camera+0x68 - the view matrix just built, not
 * the camera - and stores the result into camera+0xc0 [verified,
 * 0x0046e2ff..0x0046e304]. That field is what the particle billboard
 * turns by, which is why the two belong together.
 *
 * It works on the images of the unit Y and Z basis POINTS, each a row
 * plus the translation, and returns
 *
 *     atan2(v2.x * v1.y - v1.x * v2.y,  v1.z)
 *
 * with both terms divided by |v1| - a scale atan2 cancels. The
 * division is there only so the degeneracy test is scale-free.
 *
 * NOT atan2(row2.x, row2.z), though the two agree to about a tenth of
 * a degree; the difference is the camera's pitch leaking through the
 * cross term.
 *
 * THE DEGENERATE ARM TESTS THE SIGN BIT AS AN INTEGER, so a v1.x of
 * -0.0 takes the negative branch where a float compare would not.
 *
 * Verified against the live camera+0xc0 at four headings, equal to
 * five decimals: 0.00000, -0.61447, -1.29248, -1.99184.
 */
float camera_view_yaw(const float view[12])
{
	float v1[3];
	float v2[2];
	float len;
	int   bits;

	if (!view)
		return 0.0f;

	v1[0] = view[6] + view[9];
	v1[1] = view[7] + view[10];
	v1[2] = view[8] + view[11];
	v2[0] = view[3] + view[9];
	v2[1] = view[4] + view[10];

	len = (float)sqrt(v1[0] * v1[0] + v1[1] * v1[1] + v1[2] * v1[2]);
	if (fabs((double)len) < 0.0001) {        /* 0x004d1e50 */
		memcpy(&bits, &v1[0], sizeof bits);
		return (bits > 0) ? MR_HALF_PI_F     /* 0x004d1e5c */
		                  : -MR_HALF_PI_F;  /* 0x004d1e60 */
	}
	return (float)atan2((v2[0] * v1[1] - v1[0] * v2[1]) / len,
	                    v1[2] / len);
}
