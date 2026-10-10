/*
 * mesh.h - the .3df geometry format and its paired .map.
 *
 * Reconstructed from Load3df (0x00420590) and Load3dfAndMap
 * (0x00420a50). The format was already fully decoded before this
 * loader was written: docs/formats/mesh-3df-map.txt, verified against
 * all 128 .3df and all 124 paired .map files with every byte
 * accounted for, with a reference reader at tools/mesh3df.py.
 *
 * So this is a transcription, and tools/mesh3df.py is what it should
 * agree with.
 *
 * SCOPE. Geometry, the per-face texture record and the smoothed
 * vertex normals. Flat-colour resolution is prim.c's, because it
 * needs the bank pages and their palettes; the loader here only
 * carries the .map fields it reads out of.
 *
 * PLANE DISTANCES ARE STILL NOT COMPUTED. The original keeps one per
 * face beside the normal and nothing in the port consumes it yet.
 */

#ifndef MRACER_MESH_H
#define MRACER_MESH_H

#include "transform.h"		/* MR_Vertex */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * THE TWO ENTRY POINTS SCALE DIFFERENTLY, and by more than a little.
 * docs/formats/mesh-3df-map.txt listed "are the two vertex scale
 * globals equal?" as an open question; they are not, and they are not
 * even the same width [verified from the instructions]:
 *
 *     Load3df        FMUL double ptr [0x004d0688]   2.8
 *     Load3dfAndMap  FMUL float  ptr [0x004d0690]   96.0
 *
 * A factor of about 34 between the same file loaded two ways. Load3df
 * only applies its scale when its third argument is non-zero;
 * Load3dfAndMap always applies its own.
 */
#define MESH_SCALE_3DF      2.8f
#define MESH_SCALE_3DF_MAP 96.0f

#define MESH_MAX_FACE_VERTS 4

typedef struct {
	float x;
	float y;
	float z;
} MESH_VERTEX;

typedef struct {
	int          count;                     /* 3 or 4                 */
	unsigned int flags;                     /* the .3df face flags    */
	int          index[MESH_MAX_FACE_VERTS];

	/* From the .map, when one was loaded. */
	int          hasMap;
	float        u[MESH_MAX_FACE_VERTS];
	float        v[MESH_MAX_FACE_VERTS];
	unsigned int mapFlags;
	int          bank;                      /* low 16 bits of mapFlags */
	unsigned int textureId;

	/*
	 * THE RECORD'S OWN VERTEX COUNT, which is NOT the same thing as
	 * count above and must not be substituted for it. count is the
	 * .3df face's vertex count, 3 or 4; this is the .map record's n,
	 * and a record with n < 3 describes no polygon at all and takes a
	 * different path in the loader. -1 means no .map file was loaded.
	 *
	 * The whole 3D menu font is n = 0 - every record of all 30 files -
	 * so this is not a rare case. See mesh-3df-map.txt section 2.1.
	 */
	int          mapRecordN;

	/*
	 * Record +0x02, a 5-6-5 colour - the same format as a bank page,
	 * NOT the 5-5-5 that section 5.3 of mesh-3df-map.txt mentions in
	 * passing (that sentence is about the texture id on another
	 * branch). It is the flat colour for an n < 3 face, which is the
	 * only thing giving the 3D font its colour: those faces have no
	 * UVs to sample a bank with.
	 *
	 * o THE LIGHTING IS BAKED IN. 18 distinct values across the 26
	 *   letters, all one hue at different brightnesses, so the shading
	 *   is authored per face rather than computed from a light. There
	 *   is no light source to find.
	 */
	unsigned short mapColor;
} MESH_FACE;

typedef struct {
	int          vertexCount;
	int          faceCount;
	MESH_VERTEX *vertices;
	MESH_FACE   *faces;

	/*
	 * NEITHER LOADER FILLS THESE, in the original or here, which is
	 * why +0x08 and +0x10 looked for a long time like fields with
	 * no writer. Mesh_ComputeVertexNormals (0x0041fa10) is the only
	 * one, and maybe_Game_LoadBikeMeshes calls it on FOUR MESHES:
	 * the two front wheels and the two rear ones, one pair per
	 * articulated LOD slot. Everything else leaves them null and
	 * every consumer returns at once when they are.
	 */
	float       *faceNormals;    /* +0x08  faceCount * 3          */
	float       *vertexNormals;  /* +0x10  vertexCount * 3        */
} MESH;

/*
 * THE NAME IS A LOGICAL ASSET NAME, NOT A PATH. It goes to
 * asset_load, which looks inside the .BKF archives first - so
 * "MENU\\MESHES\\VITRE.3DF" is right and anything that has been
 * through data_path() is wrong. CLAUDE.md rule 10.
 *
 * These parameters were called "path" until a caller believed it and
 * passed a fully-qualified one, which asset_load cannot split into an
 * archive stem and a member. It failed silently for several sessions.
 *
 * mesh_load - read a .3df. Pass 0.0f for scale to leave the
 * coordinates as stored, which is what Load3df does when its third
 * argument is zero.
 */
/*
 * mesh_centre - the centre of the bounding box, in mesh units.
 *
 * A .3df carries no origin convention: medium\corps.3df puts the
 * origin by the rear axle while the menu title models are centred on
 * theirs. Anything that wants to spin a mesh about its middle has to
 * measure it. Writes {0,0,0} for an empty mesh.
 */
void mesh_centre(const MESH *m, float out[3]);

int mesh_load(MESH *m, const char *logical, float scale);

/*
 * mesh_scale - Mesh_Scale (0x004204b0), transcribed.
 *
 *     for (i = 0; i < vertexCount; i++) {
 *         v[i].x *= sx;  v[i].y *= sy;  v[i].z *= sz;
 *     }
 *
 * PER AXIS, unlike the uniform scale mesh_load takes. Page 14 needs it
 * that way - its three title meshes are scaled by (0.04, 0.03875,
 * 0.04) and friends, which are deliberately not equal.
 *
 * o THE ORIGINAL ALSO REBUILDS THE FACE NORMALS afterwards, when the
 *   mesh has any: it walks faceCount calling 0x0041f920. Our MESH
 *   carries no normals, so there is nothing to rebuild - a real gap
 *   rather than a divergence, and it will matter when lighting does.
 */
void mesh_scale(MESH *m, float sx, float sy, float sz);

/*
 * mesh_center_on_bounds - Mesh_CenterOnBounds (0x00420250).
 *
 *   1  min/max over every vertex
 *   2  centre = (min + max) * 0.5
 *   3  SUBTRACT that centre from every vertex
 *   4  recompute every face normal and plane distance
 *   5  write the old centre through outCentre, if given
 *
 * WITHOUT THIS A MESH KEEPS ITS AUTHORED OFFSET, and for this game
 * that is not a rounding detail. Page 25's D, S and I are modelled as
 * ONE WORD - laid out along +z at centres -69.5, +0.4 and +52.5, and
 * tiling with no gaps - so a loader that skips the recentre gives each
 * letter its position in the word PLUS its animation target, and they
 * pile up on each other. The camera model carries an offset of
 * (-61.5, 29.9, -60.5) and lands off centre for the same reason.
 *
 * Called on a mesh immediately BEFORE mesh_scale, by five sites in the
 * original: page 25's enter (twice), MenuTools_CreateSceneObject,
 * maybe_Game_LoadBikeMeshes, FUN_0047b600 and FUN_0040ed10.
 *
 * o NOT EVERY PAGE CALLS IT. Page 14 does not, and its title meshes
 *   keep their offsets - which is correct, and is why the title screen
 *   looks right without it. Adding it there "for consistency" would
 *   move that art.
 *
 * NOT REPRODUCED: the original also stores half extents at +0x48,
 * +0x4c and +0x50 and a radius at +0x58, all truncated to int.
 * Nothing in the reconstruction reads them yet.
 */
void mesh_center_on_bounds(MESH *m, float outCentre[3]);

/*
 * mesh_load_with_map - read a .3df and its paired .map.
 *
 * Applies the game's own consistency rule, which is narrower than it
 * looks: only records with n >= 3 are checked, and such a record must
 * have n equal to 3, or 4 when the mesh face has a fourth index.
 * Records with n < 3 are never compared against the mesh at all -
 * checking them reports false failures on shipped data.
 */
/*
 * o PASS MESH_SCALE_3DF_MAP, NOT 0. Load3dfAndMap ALWAYS scales; the
 *   argument exists only because this shares mesh_load underneath.
 *   Zero silently loads the mesh 96 times too small, which draws,
 *   transforms and culls perfectly and is invisible on screen.
 *
 *   That has now cost one debugging session. The parameter should
 *   arguably not exist on this entry point at all - the original has
 *   no such choice - and it is kept only because both callers pass
 *   the same constant and removing it is a wider change than it looks.
 */
/*
 * mesh_mirror_x_flip_winding - Mesh_MirrorXAndFlipWinding (0x00422070).
 *
 * Negates every vertex's X and reverses every face's index and UV
 * order. The two halves are ONE operation: mirroring alone turns the
 * model inside out, and the winding reversal is what puts the faces
 * back to front-facing.
 *
 * mesh_load_with_map ALREADY CALLS IT, because Load3dfAndMap does -
 * which is the whole reason the title logos read the right way round.
 * It is exposed because a caller can legitimately need a SECOND
 * application: MenuTools pages load through the mirroring path and
 * then mirror back, and the two cancel. See scene.h and the page 15
 * note in menu.c.
 *
 * o SO CALLING THIS IS NOT AUTOMATICALLY A BUG, and neither is not
 *   calling it. Getting it wrong is invisible on a symmetric mesh and
 *   obvious on a letter, which is exactly the trap that made the
 *   camera on page 25 read as hollow for two sessions.
 */
void mesh_mirror_x_flip_winding(MESH *m);

int mesh_load_with_map(MESH *m, const char *logical3df,
                       const char *logicalMap,
                       float scale);

/*
 * mesh_compute_vertex_normals - Mesh_ComputeVertexNormals
 * (0x0041fa10), the only writer of vertexNormals.
 *
 * Smoothed normals: one normal per face, summed into each vertex it
 * touches, divided by the contribution count and normalised. The
 * divide is redundant after the normalise; the original does both.
 *
 * CALL IT ON WHEELS AND NOTHING ELSE, because that is what
 * maybe_Game_LoadBikeMeshes does - g_meshRoueAvant[0..1] and
 * g_meshRoueArriere[0..1], the loop at 0x00413646. The bike bodies
 * and the three far-LOD meshes are deliberately left without, and
 * that is what makes mesh_shade_vertices a no-op for them.
 */
void mesh_compute_vertex_normals(MESH *m);

/*
 * mesh_shade_vertices - maybe_Mesh_ShadeVertices (0x00422540).
 *
 * Writes a 5-bit intensity into every transformed vertex's shade
 * field from the angle between its normal and the light. Returns at
 * once unless the mesh has vertexNormals, which is why it costs
 * nothing on the meshes that do not have them.
 *
 * *** THIS IS WHY A WHEEL IS NEVER AS BRIGHT AS THE BIKE ***
 *
 * The result lands in 8..24 for a normalised normal, so it CANNOT
 * reach 0x1f. maybe_Inst_DrawWithMatrix transforms a wheel with a
 * literal 0x1f at 0x0048c94d and then calls this at 0x0048c9b2 to
 * overwrite it, so a wheel is always dimmer than an unshaded body -
 * and, because the light is a fixed direction, it does NOT follow
 * the segment shade flag the way the body does. Through a tunnel
 * the bike pulses light and dark while the wheels sit at a steady
 * dim. Leave this call out and they glow.
 *
 * THE MATRIX IS THE INSTANCE'S OWN ORIENTATION, which for a wheel
 * carries its spin. EBX is written once in maybe_Inst_DrawWithMatrix,
 * at 0x0048c6d8 from the argument the DB calls "camera", and the
 * call site settles what that actually is: 0x0045fd0a pushes a STACK
 * MATRIX for it while 0x0045fd0b pushes the camera object separately
 * as the argument before. The body confirms it from the other side -
 * the camera's 3x3, translation and eye are read off that other
 * argument at +0x68, +0x8c and +0x98.
 *
 * So the light is carried into the wheel's frame and the shading
 * stays fixed in the world as the wheel turns. Hand it the camera
 * matrix instead and the bands are painted in MESH space and
 * revolve with the tyre.
 */
void mesh_shade_vertices(const MESH *m, const float light[3],
                         const float orient[12], MR_Vertex *verts);

void mesh_free(MESH *m);

#ifdef __cplusplus
}
#endif

#endif /* MRACER_MESH_H */
