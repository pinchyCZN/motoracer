/*
 * cull.c - see cull.h and the plate comment on Screen3D_CullAndQueueFaces
 * (0x0049c110). Read from the disassembly as well as the decompiler,
 * because two of the tests are idioms the decompiler flattens.
 */

#include <windows.h>

#include "cull.h"
#include "log.h"
#include "queue.h"

int g_viewMaxX       = 639;
int g_viewMaxY       = 479;
int g_viewLeftMargin;
int g_viewTopMargin;
int g_rendererHardware = 1;

static int s_examined;
static int s_submitted;

/*
 * THE GUARD BAND IS A BIASED COUNTER TESTED UNSIGNED, and that is the
 * whole trick.
 *
 * Start at 3. For each of the four vertices, add 1 when it is off the
 * RIGHT edge and subtract 1 when it is off the LEFT. Then reject when
 * the result, READ AS UNSIGNED, is 7 or more.
 *
 *   all four off the right  ->  3 + 4 = 7        -> rejected
 *   all four off the left   ->  3 - 4 = -1       -> 0xffffffff,
 *                                                   also rejected
 *   anything else           ->  0..6             -> kept
 *
 * So one comparison rejects a face that is entirely off one side,
 * either side. The unsigned test is what makes the negative case fall
 * out for free.
 *
 * The "off the right" test is ALSO unsigned - g_viewMaxX < (uint)x -
 * which folds "negative" into "very large", so a vertex off the left
 * enters the branch and then the signed test against the left margin
 * distinguishes it. Two unsigned comparisons doing four signed ones.
 *
 * A triangle counts vertex 0 twice, because MR_Primitive stores
 * vertexIndex[3] = vertexIndex[0] for a triangle. That is deliberate:
 * it keeps the arithmetic at four terms so the +/-4 endpoints still
 * mean "all of them".
 */
static int guard_band(const MR_Vertex *v[4], int isY)
{
	unsigned int n = 3;
	unsigned int maxEdge;
	int          minEdge;
	int          i;

	maxEdge = (unsigned int)(isY ? g_viewMaxY : g_viewMaxX);
	minEdge = isY ? g_viewTopMargin : g_viewLeftMargin;

	for (i = 0; i < 4; i++) {
		int c = isY ? v[i]->screenY : v[i]->screenX;
		if (maxEdge < (unsigned int)c) {
			if (c < minEdge)
				n -= 2;
			n += 1;
		}
	}
	return n < 7;
}

static int outside_viewport(const MR_Vertex *v)
{
	return v->screenY < g_viewTopMargin  || v->screenY > g_viewMaxY ||
	       v->screenX < g_viewLeftMargin || v->screenX > g_viewMaxX;
}

float g_backPlaneZ;


/*
 * THE SORT DIRECTION, and it is the ONE difference between the two
 * submitters that is not cosmetic.
 *
 * Screen3D_CullAndQueueFacesEx (0x0049b7f0) takes a fourth argument
 * this port has ignored since it was written: 0 picks the face's
 * NEAREST corner as the depth key, non-zero the FARTHEST, which is
 * what the plain submitter always does.  Every shipped caller
 * passes 1, so the stub was equivalent - and that is exactly why it
 * survived.
 *
 * It is honoured now so the shadow can be tested against it.  A
 * static rather than a parameter keeps the public signatures the
 * original's; cull_and_queue_faces_ex sets it and puts it back.
 */
static int s_sortFarthest = 1;

/*
 * THE TRANSLUCENT SUBMITTER'S DEPTH COMPRESSION, 0x0049bca0. Applied
 * to the key and to the key only - the near-plane and back-plane tests
 * still run on the true camZ, exactly as the original's do.
 */
static float s_depthScale = 1.0f;
static float s_depthBias;

int cull_and_queue_faces(MR_Vertex *verts, MR_Primitive *faces,
                         int faceCount, int livery)
{
	int f;
	int queued = 0;

	if (!verts || !faces)
		return 0;

	for (f = 0; f < faceCount; f++) {
		MR_Primitive    *prim = &faces[f];
		const MR_Vertex *v[4];
		const MR_Vertex *farthest;
		unsigned int     hwFlags = 0;
		float            facing  = 0.0f;
		float            key;
		int              i;

		/*
		 * The kind-0x0d skip. Note it is CONDITIONAL here - flag bit 0
		 * must also be set - where Screen3D_CullAndQueueFacesEx skips
		 * 0x0d unconditionally. The two submitters really do differ on
		 * this.
		 */
		if ((prim->flags & PRIM_FLAG_SECOND_PASS) &&
		    prim->kind == PRIM_KIND_SKIPPED)
		    continue;

		s_examined++;

		for (i = 0; i < 4; i++)
			v[i] = &verts[prim->vertexIndex[i]];

		/* The farthest corner: the depth key AND the near-plane test
		 * subject. Sorting by the farthest corner is what makes the
		 * back-to-front walk correct. */
		farthest = v[0];
		for (i = 1; i < 4; i++) {
			if (s_sortFarthest) {
				if (farthest->camZ < v[i]->camZ)
					farthest = v[i];
			} else {
				if (v[i]->camZ < farthest->camZ)
					farthest = v[i];
			}
		}
		if (farthest->camZ <= g_nearPlaneZ)
			continue;

		/*
		 * AND THE BACK PLANE. Dropped only when every corner is
		 * beyond it, so a face straddling the plane survives.
		 *
		 * This is also what stops the distant water painting over
		 * the track. The queue is a BACK-TO-FRONT painter's walk
		 * keyed on the farthest corner, and a single ocean quad
		 * thousands of units across can have a farthest corner
		 * nearer than a small distant road face - so it sorts in
		 * front of geometry it is actually behind. Large polygons
		 * break painter ordering; the back plane removes the ones
		 * big enough to do it.
		 */
		/*
		 * CULL ON THE SAME CORNER THE SORT KEYS ON. This tested the
		 * NEAREST corner first, on the reasoning that a face
		 * straddling the plane should keep its near half. It has no
		 * near half - nothing here clips - so a quad running from
		 * 2000 to 17800 was kept whole and drawn to its far corner.
		 *
		 * ile01 has faces spanning up to 15816 units, 374 of them
		 * over 5000 [measured], and the biggest are the water. So
		 * the sea reached out past everything the plane had removed
		 * and filled the gap where the road used to be, which is
		 * what "the track looks flooded in the distance" was.
		 *
		 * The farthest corner is already this face's depth key for
		 * the back-to-front walk. Culling on the nearest while
		 * sorting on the farthest is two different opinions about
		 * where the face is.
		 */
		if (g_backPlaneZ > 0.0f) {
			if (farthest->camZ > g_backPlaneZ)
				continue;
		}

		if (v[0]->camZ <= g_nearPlaneZ || v[1]->camZ <= g_nearPlaneZ ||
		    v[2]->camZ <= g_nearPlaneZ || v[3]->camZ <= g_nearPlaneZ) {
			/*
			 * Some corner is behind the near plane, so the screen
			 * coordinates of at least one vertex were never written
			 * and the 2D cross product would read stale pool slots.
			 * The facing test falls back to a scalar triple product in
			 * CAMERA space, which needs no projection.
			 *
			 * hwFlags bit 0 says so: it is a request for clipping
			 * work, not a state.
			 */
			float ax = v[1]->camX - v[2]->camX;
			float ay = v[1]->camY - v[2]->camY;
			float az = v[1]->camZ - v[2]->camZ;
			float bx = v[1]->camX - v[0]->camX;
			float by = v[1]->camY - v[0]->camY;
			float bz = v[1]->camZ - v[0]->camZ;

			hwFlags = 1;
			facing = farthest->camZ * (ay * bx - ax * by) +
			         farthest->camY * (ax * bz - az * bx) +
			         farthest->camX * (az * by - ay * bz);
		} else if (guard_band(v, 0) && guard_band(v, 1)) {
			/* Every corner projects, so the cheap screen-space cross
			 * product decides the winding. */
			facing = (float)((v[0]->screenY - v[1]->screenY) *
			                 (v[2]->screenX - v[1]->screenX) +
			                 (v[2]->screenY - v[1]->screenY) *
			                 (v[1]->screenX - v[0]->screenX));
		}

		/* Bit 1, hardware only: extends outside the viewport. */
		if (g_rendererHardware) {
			for (i = 0; i < 4; i++) {
				if (outside_viewport(v[i])) {
					hwFlags |= 2;
					break;
				}
			}
		}

		/*
		 * THE FACING TEST, and it is spelled as a BIT-PATTERN COMPARE
		 * in the binary:
		 *
		 *     MOV EAX,dword ptr [ESP + 0x14]   ; the float's bits
		 *     CMP EAX,0x3f800000               ; as a SIGNED int
		 *     JLE reject
		 *
		 * Not a float-to-int conversion, which would reject
		 * everything: a screen-space cross product on a 640x480 screen
		 * never exceeds about 307200, and 0x3f800000 as an integer is
		 * a billion. For positive IEEE floats the bit ordering matches
		 * the value ordering, and a negative float has its sign bit
		 * set so it compares as a negative integer - so this is
		 * exactly "facing > 1.0f", and that is how it is written here.
		 *
		 * The constant is 1.0f, the same minimum-area value the
		 * emitters use, so BACKFACE REJECTION AND DEGENERATE-AREA
		 * REJECTION ARE ONE COMPARISON.
		 */
		if (facing > 1.0f) {
			/*
			 * THE PALETTED SPLIT, on the one test the original
			 * makes: the face's bank has palettes or it does not.
			 * A caller with no livery to give passes -1 and takes
			 * the plain path, which is every caller but the racer
			 * draw.
			 *
			 * NOT REPRODUCED: the kind-0x2a substitution the
			 * paletted sibling applies under g_cfgDetailLevel <= 1.
			 */
			key = farthest->camZ * s_depthScale + s_depthBias;
			if (livery >= 0) {
				queue_primitive_paletted(prim, key,
				                         verts, hwFlags,
				                         QUEUE_MODE_DEPTH, livery);
			} else {
				queue_primitive(prim, key, verts,
				                hwFlags, QUEUE_MODE_DEPTH);
			}
			s_submitted++;
			queued++;
		}
	}
	return queued;
}

/*
 * ORIGIN  Screen3D_CullAndQueueTranslucent @ 0x0049bca0
 *
 * The cull is byte-for-byte Screen3D_CullAndQueueFaces, so this is the
 * plain submitter plus the two things the original does immediately
 * before queueing. Its plate comment is the specification.
 *
 * IT REWRITES THE MESH, not a copy. The original stores the forced
 * kind back into the face array it was handed, so g_meshGuidon and
 * g_meshCockpit are permanently translucent after their first frame.
 * The mapping is idempotent, which is why nothing notices.
 *
 * THE 0.2 IS A SORT-KEY TRICK AND NOT A POSITION. A fifth of the true
 * depth minus the near plane drops every face into the lowest buckets,
 * so the cockpit paints over the world instead of sorting against it.
 * When the back end gains a real depth buffer this has to become a
 * separate overlay pass - see cull.h.
 */
int cull_and_queue_translucent(MR_Vertex *verts, MR_Primitive *faces,
                               int faceCount)
{
	float saveScale = s_depthScale;
	float saveBias  = s_depthBias;
	int   queued, f;

	if (!verts || !faces)
		return 0;

	for (f = 0; f < faceCount; f++) {
		int kind = faces[f].kind;

		if (kind == 0)
			continue;
		kind = (kind == 1 || kind == PRIM_KIND_TRANSLUCENT)
		       ? PRIM_KIND_TRANSLUCENT
		       : PRIM_KIND_TRANSLUCENT_TEX;
		/*
		 * BOTH FIELDS. The original goes through Prim_SetKind, which
		 * writes +0x00 AND +0x3a - and the dispatcher reads +0x3a and
		 * never +0x00 (prim.h). Writing only kind left effectiveKind
		 * holding whatever the loader built, so the rewrite had no
		 * effect on what was drawn at all.
		 */
		faces[f].kind          = (unsigned short)kind;
		faces[f].effectiveKind = (unsigned short)kind;
	}

	s_depthScale = CULL_TRANSLUCENT_DEPTH_K;
	s_depthBias  = -g_nearPlaneZ;
	queued = cull_and_queue_faces(verts, faces, faceCount, -1);
	s_depthScale = saveScale;
	s_depthBias  = saveBias;
	return queued;
}


void cull_stats(int *examined, int *submitted)
{
	if (examined)  *examined  = s_examined;
	if (submitted) *submitted = s_submitted;
}

void cull_reset_stats(void)
{
	s_examined  = 0;
	s_submitted = 0;
}


/*
 * ORIGIN  Screen3D_CullAndQueueFacesEx @ 0x0049b7f0. NOT IMPLEMENTED;
 * it forwards to the plain cull.
 *
 * The original picks between the two on the sign of a cull flag and
 * passes 1 as the extra argument. Three places use the Ex form and
 * all three matter to us: the racer shadow, the track zone meshes,
 * and the translucent pass.
 *
 * WHAT THE FOURTH ARGUMENT DOES IS UNREAD. 0x0049b7f0 has not been
 * decompiled, so forwarding is a guess about equivalence rather than
 * a known one - which is exactly why this is a named stub and not a
 * silent alias. IF CULLING LOOKS WRONG ON A SHADOW OR A ZONE MESH,
 * THIS IS THE FIRST PLACE TO LOOK. [open]
 */
int cull_and_queue_faces_ex(MR_Vertex *verts, MR_Primitive *faces,
                            int faceCount, int flag)
{
	int save = s_sortFarthest;
	int queued;

	/*
	 * NO LONGER A STUB for the sort direction - see s_sortFarthest.
	 *
	 * STILL NOT REPRODUCED, and both are marked rather than silent:
	 *   o the DEPTH BIAS. The original is the only reader of
	 *     PRIM_FLAG_DEPTH_BIAS in the image and adds 1024.0 to the key
	 *     when it is set. No shipped primitive this port builds sets
	 *     it - the racer shadow's flags read 0x0000 live - so nothing
	 *     would change today. [open]
	 *   o the STRICTER kind-0x0d skip: the Ex variant drops every such
	 *     face where the plain one also wants flag bit 0.
	 */
	s_sortFarthest = (flag != 0);
	queued = cull_and_queue_faces(verts, faces, faceCount, -1);
	s_sortFarthest = save;
	return queued;
}
