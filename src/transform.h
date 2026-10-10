/*
 * transform.h - object space to screen space.
 *
 * Reconstructed from Screen3D_TransformVertsFlat (0x0049b2c0). The
 * record it fills is written up in docs/structs/MR_Vertex.txt and the
 * projection in docs/subsystems/rasterizer-pipeline.txt section 50.3.
 *
 * This is the first piece of the RENDERER that src/ carries, and it is
 * the half that does not have to be rewritten: it is plain arithmetic
 * with no DirectDraw and no Direct3D in it. What comes after it - the
 * cull, the queue, the span buffer, the execute buffers - is the part
 * being replaced.
 */

#ifndef MRACER_TRANSFORM_H
#define MRACER_TRANSFORM_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The post-transform vertex. 0x1c bytes, laid out to match.
 *
 * Everything in it is output: no texture coordinates, no colour, no
 * normal. UVs are per FACE and lighting is resolved before this stage,
 * so the rasterizer never sees object space.
 */
typedef struct {
	float camX;      /* 0x00  camera space, ALREADY viewport-scaled */
	float camY;      /* 0x04  camera space, ALREADY viewport-scaled */
	float camZ;      /* 0x08  tested against the near plane         */
	int   shade;     /* 0x0c  packed: intensity << 13, 19.13 fixed  */
	int   screenX;   /* 0x10  written only if camZ passes near      */
	int   screenY;   /* 0x14  written only if camZ passes near      */
	float invZ;      /* 0x18  1/camZ                                */
} MR_Vertex;

typedef char mr_vertex_is_0x1c[(sizeof(MR_Vertex) == 0x1c) ? 1 : -1];

/*
 * Viewport and projection state. The original keeps these as globals
 * and so do we, because the transform reads them directly rather than
 * taking them as arguments.
 *
 * g_projScaleX and g_projScaleY are the DERIVED scales - base times
 * zoom. See rasterizer-pipeline.txt section 50.3.
 */
extern int   g_viewCentreX;
extern int   g_viewCentreY;
extern float g_projBaseX;
extern float g_projBaseY;
extern float g_projZoom;
extern float g_projScaleX;
extern float g_projScaleY;
extern float g_nearPlaneZ;

/*
 * The projection accessors. Set the base or the zoom; never assign a
 * scale directly, because the scales are derived and the next setter
 * call would overwrite the assignment without warning.
 *
 *     screen3d_open_projection   Screen3D_Open         0x004995a0
 *     screen3d_set_proj_base_x   Screen3D_SetProjBaseX 0x0049a0e0
 *     screen3d_set_proj_base_y   Screen3D_SetProjBaseY 0x0049a100
 *     screen3d_set_proj_zoom     Screen3D_SetProjZoom  0x0049a120
 *     screen3d_get_proj_zoom     Screen3D_GetProjZoom  0x0049a150
 *
 * At 640x480 both bases are 480. See transform.c for why that is 480
 * and not the half-width.
 */
void  screen3d_open_projection(int width, int height);
void  screen3d_set_proj_base_x(float f);
void  screen3d_set_proj_base_y(float f);
void  screen3d_set_proj_zoom(float f);
float screen3d_get_proj_zoom(void);

/*
 * The vertex pool. The original is a fixed array with a bump cursor
 * and NO BOUND CHECK; ours takes a capacity and refuses to overrun.
 * Reset it once per frame.
 */
int  vertexpool_create(int capacity);
void vertexpool_destroy(void);
void vertexpool_reset(void);
int  vertexpool_used(void);

/*
 * Hand out n consecutive slots, or NULL. The near-plane clipper uses
 * this: its new vertices live in the same pool as the transformed
 * ones, exactly as the original's do.
 */
MR_Vertex *vertexpool_alloc(int n);
int  vertexpool_capacity(void);

/*
 * transform_verts_flat - one mesh, one shade for every vertex.
 *
 *   m      12 floats, a 3x4 matrix in COLUMN-MAJOR order: m[0..2] is
 *          the first basis vector, m[3..5] the second, m[6..8] the
 *          third, m[9..11] the translation.
 *   src    3 floats per vertex, x y z
 *   count  vertices
 *   shade  intensity; stored as intensity << 13
 *
 * Returns the base of the slice just filled, or NULL if the pool is
 * full. The caller hands that straight to the cull stage.
 */
MR_Vertex *transform_verts_flat(const float *m, const float *src,
                                int count, int intensity);

/*
 * transform_verts_shaded - Screen3D_TransformVertsShaded (0x0049b490).
 *
 * The per-vertex-intensity sibling of transform_verts_flat, and it is
 * NOT the simple variant it looks like. Two things about its interface
 * are genuinely surprising, and both were read from the disassembly
 * because the decompiler renders them misleadingly.
 *
 * 1. THE Y COORDINATE COMES FROM A SEPARATE ARRAY [verified].
 *
 *        xz    stride 3 floats; only [0] and [2] are read
 *        y     stride 1 float
 *
 *    In the loop that is FLD [ESI] / FLD [ESI+8] for x and z against
 *    FLD [EAX] for y, with ESI advancing by 12 and EAX by 4. The y
 *    pointer is also the argument the function NULL-checks on entry:
 *    pass NULL and it returns the pool base having done nothing.
 *
 *    So this is a heightfield-shaped interface - a base mesh supplying
 *    x and z, and a parallel stream supplying y. animDecor.cpp is the
 *    caller's neighbourhood, so deforming decorative geometry with
 *    per-vertex lighting is the obvious reading [inferred].
 *
 * 2. THE INTENSITY ARRAY IS OPTIONAL, and when it is NULL the shade
 *    field IS NOT WRITTEN AT ALL [verified]. The slot keeps whatever
 *    the previous user of that pool entry left in it. That is the same
 *    stale-slot hazard MR_Vertex.txt section 7.2 records for screenX
 *    and screenY, one field over.
 *
 * WHAT IS NOT REPRODUCED. The original takes two more arguments this
 * one does not:
 *
 *   o an OBJECT pointer, whose +0x68 matrix and negated +0x98..+0xa0
 *     translation are combined with two globals into a SECOND matrix
 *
 *   o an OVERRIDE LIST of 12-byte records - { float x; int index;
 *     float z; } - and when a record's index matches the current
 *     vertex, that vertex is transformed by the second matrix from the
 *     record's own x and z instead
 *
 * That is a per-vertex substitution mechanism for a feature nothing in
 * src/ produces yet, and it needs an object type that has not been
 * reconstructed. Pass the base path only; the arithmetic below is the
 * same either way, just with the other matrix.
 *
 * Returns the base of the slice filled, or NULL. The projection, the
 * near-plane rule and the x87 control word are identical to
 * transform_verts_flat - including that FISTP ROUNDS.
 */
MR_Vertex *transform_verts_shaded(const float *m,
                                  const float *xz, const float *y,
                                  int count, const int *intensity);

#ifdef __cplusplus
}
#endif

#endif /* MRACER_TRANSFORM_H */
