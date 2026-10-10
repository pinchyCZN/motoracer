/*
 * mrmod_stubs.c - the externals src/trk.c references and this DLL
 * never reaches.
 *
 * Compiling a real translation unit out of src/ means satisfying
 * everything it names, not everything it runs. trk.c calls eight
 * functions and reads one global that belong to the rest of the port;
 * plane_distance touches none of them, so they are answered here
 * rather than dragging assets.c, log.c, config.c, transform.c, cull.c,
 * model.c and detail.c into a DLL that wants eight lines of maths.
 *
 * THESE ARE NOT PLACEHOLDERS FOR MISSING WORK. They are a boundary: a
 * replacement that stays inside its own arithmetic never crosses it.
 * The moment one does - the first time a hooked function needs to
 * transform vertices or queue faces - the answer is NOT to implement
 * these, it is to point them at MOTO own Screen3D_TransformVertsFlat
 * (0x0049b2c0) and Screen3D_CullAndQueueFaces (0x0049c110). That is
 * what this file becomes.
 *
 * ANYTHING THAT REACHES ONE OF THESE IS A BUG IN THE HOOK CHOICE, so
 * they are written to fail obviously rather than plausibly - null,
 * zero, nothing drawn.
 */

#include <stddef.h>

struct MR_Vertex;
struct MR_Primitive;

/* detail.h - read by trk_list_visible_segments for the decor gate. */
int g_cfgDetailLevel = 0;

/* assets.h */
void *asset_load(const char *logicalName, unsigned int *sizeOut)
{
	(void)logicalName;
	if (sizeOut)
		*sizeOut = 0;
	return NULL;
}

/* log.h */
void mrlog(const char *fmt, ...)
{
	(void)fmt;
}

void mrlog_err(const char *fmt, ...)
{
	(void)fmt;
}

/* model.h */
void mat34_from_euler(float out[9], const float rot[3])
{
	(void)out;
	(void)rot;
}

void mat34_multiply(float out[12], const float a[12], const float b[12])
{
	(void)out;
	(void)a;
	(void)b;
}

/* prim.h */
void prim_rebase_banks(struct MR_Primitive *prims, int count, int bank)
{
	(void)prims;
	(void)count;
	(void)bank;
}

/* transform.h */
struct MR_Vertex *transform_verts_flat(const float *m, const float *src,
                                       int count, int intensity)
{
	(void)m;
	(void)src;
	(void)count;
	(void)intensity;
	return NULL;
}

/* cull.h */
int cull_and_queue_faces(struct MR_Vertex *verts, struct MR_Primitive *faces,
                         int faceCount)
{
	(void)verts;
	(void)faces;
	(void)faceCount;
	return 0;
}
