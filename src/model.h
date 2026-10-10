/*
 * model.h - MR_ModelInstance: a mesh placed in the world.
 *
 * A position, an orientation and the mesh to draw. NOT A MENU TYPE -
 * Racer_Create embeds three of these in every racer, at +0x24, +0x1a8
 * and +0x32c, filled from a body mesh and g_meshRoueArriere and
 * g_meshRoueAvant [verified]. So a bike is three placed meshes and the
 * wheels are instances in their own right.
 *
 * It gets its own file rather than joining mesh.c or menu.c precisely
 * because BOTH the menus and the racers use it. mesh.c loads meshes;
 * this places them. menu.c would be the wrong home for something the
 * bikes depend on.
 *
 * From ModelInstance_Init (0x0048bce0) and ModelInstance_Draw
 * (0x0048c030). See docs/structs/MR_ModelInstance.txt.
 */

#ifndef MRACER_MODEL_H
#define MRACER_MODEL_H

#include "mesh.h"
#include "prim.h"
#include "skl.h"
#include "transform.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The original's record is 0x184 bytes and most of it is unread -
 * about twenty dwords the initialiser zeroes with no known reader, and
 * three flags it sets to 1.
 *
 * o THIS STRUCT IS NOT THAT LAYOUT AND DOES NOT PRETEND TO BE. It
 *   holds the four fields that are settled, at names rather than
 *   offsets. Reproducing 0x184 bytes of mostly-unknown padding would
 *   claim knowledge that does not exist; the Ghidra type carries the
 *   real offsets for anyone reading the binary.
 */
typedef struct MR_ModelInstance {
	float rotation[3];      /* +0x00, Euler, RADIANS */
	float position[3];      /* +0x0c                 */
	MESH *mesh;             /* +0x34, for its vertices */
	/*
	 * THE LIVERY - WHICH PALETTE OF THE BANK RESOLVES THIS
	 * MESH. This field was called detailLevel and that was
	 * wrong.
	 *
	 * Racer_Create writes param_3 & 7 here, or -1, on all FOUR
	 * of a racer's instances, and ModelInstance_Draw hands it
	 * to Screen3D_CullAndQueueFaces as the argument that picks
	 * Screen3D_QueuePrimitivePaletted. Masking with 7 against a
	 * bank of eight palettes is what settles it: a detail level
	 * has no reason to wrap at eight.
	 *
	 * -1 means NOT PALETTED, and takes the Ex path in the
	 * original. See docs/subsystems/liveries.txt.
	 */
	int   livery;           /* +0x14c                */

	/*
	 * THE FACES COME SEPARATELY, AND THAT IS OUR PROBLEM NOT THE
	 * ORIGINAL'S. ModelInstance_Draw reads mesh->faces at +0x00 and
	 * hands it straight to Screen3D_CullAndQueueFaces, because in the
	 * original a mesh's faces ARE MR_Primitives - Load3dfAndMap builds
	 * them that way.
	 *
	 * Our mesh.c predates MR_Primitive being in src/ and emits its own
	 * simpler MESH_FACE, so the two are not interchangeable and
	 * casting between them would feed the queue garbage. The converter
	 * lives in prim.c - prim_build_from_mesh - which is where it ended
	 * up after the geometry test harness that first held it was
	 * removed.
	 *
	 * So the instance takes the converted primitives explicitly. When
	 * mesh.c is rewritten to emit MR_Primitive directly, these two
	 * fields go away and mesh->faces is used as the original uses it.
	 */
	MR_Primitive *prims;
	int           primCount;
} MR_ModelInstance;

/*
 * ModelInstance_Init(this, position, rotation, mesh).
 *
 * The argument order is the original's and it is worth keeping even
 * though it reads oddly: POSITION FIRST, THEN ROTATION, even though
 * the struct stores rotation first. That is what the call sites pass.
 */
void model_init(MR_ModelInstance *m, const float pos[3],
                const float rot[3], MESH *mesh);

/*
 * Attach the converted primitives. See the note in the struct: this
 * exists only because mesh.c does not emit MR_Primitive yet.
 */
void model_set_prims(MR_ModelInstance *m, MR_Primitive *prims, int count);

/*
 * model_draw - ModelInstance_Draw, transcribed.
 *
 *   view34   the camera's 3x4, 12 floats. In the original this is
 *            camera + 0x68 of a 0x238-byte record.
 *   camPos   the camera's position, camera + 0x98 - which is the
 *            global the game calls g_cameraPos.
 *
 * TAKING THE TWO PIECES RATHER THAN A CAMERA POINTER is deliberate.
 * The 0x238 record has not been mapped, and inventing a layout for it
 * to make this signature prettier would be claiming knowledge we do
 * not have. These are the only two things the function reads.
 *
 * Transforms the mesh and submits its faces to the queue. Draws
 * nothing directly - queue_draw does that, later in the frame.
 */
/*
 * Returns the transformed vertex slice, or NULL if nothing was drawn.
 * The return value exists for diagnostics: a caller that wants to know
 * WHERE a model landed on screen has no other way to ask, and "it did
 * not appear" is otherwise indistinguishable from "it appeared as a
 * sub-pixel dot".
 */

MR_Vertex *model_draw(const MR_ModelInstance *m, const float *view34,
                      const float camPos[3]);

/*
 * mat34_from_euler - the euler-to-matrix helper, inlined all over the
 * original and written out once here.
 *
 *     index = (int)(radians * (0x4000 / 2pi))
 *     c = g_cosTable[index & 0x3fff]
 *     s = g_cosTable[(index - 0x1000) & 0x3fff]
 *
 * per angle, then the nine products in model.c. out is 9 floats, the
 * 3x3 only - the caller supplies the translation.
 */
/*
 * mat34_from_euler_decor - FUN_00403880, the DECOR euler.
 *
 * Props do not use the model instances' composition. This is
 * transpose(Ry(rot[1]) . Rx(rot[0]) . Rz(rot[2])); mat34_from_euler
 * is a different matrix and using it on decor tilts anything that
 * carries pitch or roll. See model.c.
 */
void mat34_from_euler_decor(float out[9], const float rot[3]);

void mat34_from_euler(float out[9], const float rot[3]);

/*
 * mat34_to_euler - the inverse, Mat34_ToEuler (0x0046d270).
 *
 * maybe_Racer_Draw round-trips a matrix through this and back
 * before composing it with the camera, which discards any scale or
 * shear - see racerdraw.c. Reads only the 3x3.
 */
void mat34_to_euler(float out[3], const float m[12]);

/*
 * mat34_invert_rigid - the inverse of an orthonormal 3x4.
 *
 *     v' = v * M + t   =>   v = (v' - t) * transpose(M)
 *
 * maybe_Racer_Draw builds exactly this inline at 0x004615c1, element
 * by element, immediately before it transforms a part's vertices.
 * Only valid for a matrix that is a product of rotations - which the
 * skeleton walk's are, being built from Euler angles.
 */
/*
 * mat34_look_at - maybe_Mat34_LookAt (0x0046bdc0), whole.
 *
 * A frame whose row 2 is the direction from -> to. NOT camera_look_at
 * below, which is the camera's view matrix and a different
 * construction entirely. The AI line follower aims with this one.
 */
void mat34_look_at(float out[12], const float from[3], const float to[3]);

void mat34_invert_rigid(float out[12], const float m[12]);

/*
 * mat34_multiply - Matrix34_Multiply (0x00403a80).
 *
 * out = a * b, 3x4 by 3x4. out may alias a or b: every load happens
 * before every store, which is a property of the original worth
 * keeping. See src/README.txt section 29.
 */
void mat34_multiply(float out[12], const float a[12], const float b[12]);

/*
 * camera_look_at - FUN_0046e140, transcribed.
 *
 * Builds the 3x4 view matrix a camera needs to point at a target. The
 * original writes it into the camera record at +0x68 and page 14 calls
 * it every frame with a target of (0,0,0).
 *
 *     d   = normalise(target - camPos)
 *     hyp = sqrt(d.x^2 + d.z^2)          the horizontal length
 *     A   = yaw   [[d.z/hyp, 0, d.x/hyp], [0,1,0], [-d.x/hyp, 0, d.z/hyp]]
 *     B   = pitch [[1,0,0], [0, hyp, d.y], [0, -d.y, hyp]]
 *     view = A * B,  translation zero
 *
 * o IT IS NOT IDENTITY EVEN WHEN THE CAMERA LOOKS DOWN -Z. For page
 *   14 - camera at (0,0,4500) looking at the origin - d is (0,0,-1),
 *   so A is a 180 degree yaw and the view is
 *
 *       [-1 0 0][0 1 0][0 0 -1]
 *
 *   Assuming identity there put every model BEHIND the camera: the
 *   ellipse's camZ came out -44500 instead of +44500, and the whole
 *   title scene was clipped away every frame while looking exactly
 *   like a mesh or transform bug.
 *
 * Returns 0 and leaves view untouched when the target coincides with
 * the camera, or when the direction is straight up or down - the
 * original guards on hyp being non-zero and simply does not write.
 */
/*
 * camera_view_yaw - FUN_00490220, the tail of Camera_LookAt.
 *
 * The angle the original stores in camera+0xc0 once the view matrix is
 * built. Radians, in [-pi, pi], and it is the NEGATED heading of the
 * view - so RotY(camera_view_yaw(v)) composed with v cancels the yaw
 * and leaves the pitch, which is exactly what a billboard wants.
 * fx_draw_all_particles is the consumer.
 */
float camera_view_yaw(const float view[12]);

int camera_look_at(float view[12], const float camPos[3],
                   const float target[3]);

#ifdef __cplusplus
}
#endif

#endif /* MRACER_MODEL_H */
