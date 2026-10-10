/*
 * scene.h - MR_MenuSceneObject, the 3D menu animation layer.
 *
 * The original's menuTools object: one mesh plus TWO independently
 * animated vec3 channels. Every 3D menu page is built from these -
 * the turning DSI logo, the rotating bike, the flying letters on the
 * name-entry page. See docs/structs/MR_MenuSceneObject.txt, where the
 * 0x94-byte record and the animation model are fully worked out.
 *
 *
 * THE TWO CHANNELS ARE THE SAME LAYOUT TWICE, and they are NOT
 * interchangeable in meaning:
 *
 *     vec     +0x00   EULER ANGLES, radians
 *     target  +0x0c   POSITION
 *
 * Each has its own start, delta, start/end time, duration and mode,
 * so a letter can be flying to a keyboard slot while independently
 * spinning to rest. That independence is the whole point of the
 * struct and it is why page 15 can scatter and settle in one step.
 *
 *
 * THE STRUCT BELOW IS DELIBERATELY NOT THE ORIGINAL'S LAYOUT, which
 * CLAUDE.md rule 5b allows as long as the difference is written down.
 * Dropped: createArg3/createArg4 (stored by the constructor, read by
 * nothing), pBuf1c/pBuf8c/pBuf90 (three heap pointers the destructor
 * frees with no writer found), and unknown50/unknown7c (symmetric,
 * written by nothing, read by nothing). If any of those five turn out
 * to matter, the doc has their offsets.
 */

#ifndef MRACER_SCENE_H
#define MRACER_SCENE_H

#include "mesh.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * THE THREE MODES, from MenuTools_UpdateMoves (0x0047a3d0). Modes 0
 * and 1 are the same two the 2D sprite layer has, with the same
 * constants - one easing model serves both layers, so this shares
 * sprite.c's curve rather than restating it.
 */
#define SCENE_MODE_LINEAR     0   /* cur = start + delta * t         */
#define SCENE_MODE_EASE       1   /* sine ease in and out            */
#define SCENE_MODE_SPIN       2   /* free-running; never completes   */

typedef struct {
	float        vec[3];            /* +0x00  Euler angles, radians  */
	float        target[3];         /* +0x0c  position               */
	MESH        *mesh;              /* +0x18                         */

	int          targetActive;      /* +0x20                         */
	int          vecActive;         /* +0x24                         */
	int          vecRebase;         /* +0x28                         */

	float        vecStart[3];       /* +0x2c                         */
	float        vecDelta[3];       /* +0x40  a RATE when mode 2     */
	unsigned int vecEndMs;          /* +0x4c                         */
	unsigned int vecDurationMs;     /* +0x54                         */
	unsigned int vecStartMs;        /* +0x58                         */
	int          vecMode;           /* +0x5c                         */

	float        targetStart[3];    /* +0x60                         */
	float        targetDelta[3];    /* +0x6c                         */
	unsigned int targetEndMs;       /* +0x78                         */
	unsigned int targetDurationMs;  /* +0x80                         */
	unsigned int targetStartMs;     /* +0x84                         */
	int          targetMode;        /* +0x88                         */

	int          loaded;
} MR_SceneObject;

/*
 * scene_object_load - MenuTools_CreateSceneObject (0x004794b0).
 *
 * Zeroes both channels, loads the mesh, and centres it on its bounds.
 * A NULL map name takes the Load3df path instead of Load3dfAndMap,
 * exactly as the original branches on it.
 *
 * *** IT LOADS THROUGH THE MIRRORING PATH. *** Load3dfAndMap applies
 * Mesh_MirrorXAndFlipWinding, so the mesh arrives mirrored, and a
 * caller that wants the file's own handedness has to mirror it BACK
 * with scene_mirror_mesh. Page 15 does exactly that - see the note
 * there. This is not a quirk worth hiding behind a flag: the original
 * has two separate calls and so do we.
 *
 * Returns 0 on failure, and the object is left safe to free.
 */
int  scene_object_load(MR_SceneObject *o, const char *mesh3df,
                       const char *meshMap);
void scene_object_free(MR_SceneObject *o);

/*
 * The two plain setters, which also CANCEL that channel's animation.
 * FUN_0047a380 sets vec and clears vecActive; FUN_0047a3a0 sets
 * target and clears targetActive.
 */
void scene_set_vec(MR_SceneObject *o, const float v[3]);
void scene_set_target(MR_SceneObject *o, const float v[3]);

/* MenuTools_SetVecPair (0x0047a340) - both at once, both cancelled. */
void scene_set_vec_pair(MR_SceneObject *o, const float vec[3],
                        const float target[3]);

/*
 * MenuTools_StartVecMove (0x0047a820) and
 * maybe_MenuTools_StartTargetMove (0x0047a8b0).
 *
 * o THE ARGUMENT IS A DESTINATION FOR MODES 0 AND 1 AND A RATE FOR
 *   MODE 2. The starters subtract the current value to get a delta
 *   when the mode is not 2, and store the argument as given when it
 *   is. So a mode-2 "move" is an angular velocity that never
 *   arrives, which is what makes it a spin.
 */
void scene_start_vec_move(MR_SceneObject *o, const float to[3],
                          float durationSec, int mode);
void scene_start_target_move(MR_SceneObject *o, const float to[3],
                             float durationSec, int mode);

/*
 * MenuTools_UpdateMoves (0x0047a3d0). Advances both channels. Call
 * once per frame per object, before drawing.
 */
void scene_update(MR_SceneObject *o);

/*
 * MenuTools_NormaliseMeshToSize (0x0047a960) - fit the mesh inside a
 * cube of half-extent `half`, centred on its own bounds, and RETURN
 * the scale used.
 *
 * o THE SMALLEST RATIO WINS, so the mesh is fitted inside the box on
 *   every axis rather than filling it on one. Uniform, no distortion.
 *
 * o IT MUTATES THE MESH, so calling it twice compounds. That is also
 *   why the returned scale matters: page 15 normalises ONE glyph and
 *   then applies the same scale to the other 27 with
 *   scene_scale_mesh_by, so the alphabet keeps its relative letter
 *   sizes instead of every glyph being blown up to the same box.
 */
float scene_normalise_mesh_to_size(MR_SceneObject *o, float half);

/* FUN_0047abd0 - centre on bounds and scale by a GIVEN factor. */
void scene_scale_mesh_by(MR_SceneObject *o, float scale);

/*
 * FUN_0047ad40 - mirror in X and reverse every face's winding.
 *
 * The same operation as Mesh_MirrorXAndFlipWinding, reached a
 * different way: the original builds a rotation matrix from four
 * constant g_cosTable entries - cos(0), cos(90), cos(180), cos(270)
 * = 1, 0, -1, 0 - which multiplies out to exactly (x,y,z) ->
 * (-x,y,z), and then walks the faces calling Face_ReverseWinding.
 * Written out as the mirror it is rather than as a matrix multiply
 * by a matrix that is knowable at compile time.
 */
void scene_mirror_mesh(MR_SceneObject *o);

#ifdef __cplusplus
}
#endif

#endif /* MRACER_SCENE_H */
