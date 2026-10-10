/*
 * racerdraw.c - maybe_Racer_Draw, brought over whole.
 *
 * ONE ORIGINAL FUNCTION, 6.5 KB, AND ITS OWN FILE. 0x0045f290 to
 * 0x00461a96 is where the fork, the swingarm, the steering yaw, the
 * two wheel placements, the rider placement and the part-matrix walk
 * all live, and they belong together because they ARE one function.
 * Splitting any of them back out reintroduces rule 15's defect: a
 * slice silently drops the branches nobody knew to look for.
 *
 * IT IS NOT GAMEPLAY, so rule 13's five-file set does not cover it;
 * it is a draw, and putting 6.5 KB of drawing into racer.c or skl.c
 * would mix concerns worse than a file of its own does.
 *
 * IF THE DECOMPILATION LOOKS UNUSABLE, CHECK THE PROTOTYPES FIRST.
 * Twelve callees here were undeclared, several with a RET n purge
 * Ghidra could not see, and the output was garbage in the way a
 * genuinely hard function looks - return addresses stored into
 * locals, every call with an empty argument list, eleven extraout_
 * phantoms. Declaring them fixed all of it, and none of it was this
 * function being difficult. whole-function-import.txt import 9 has
 * the table; CLAUDE.md 15.1 is the general rule.
 *
 * __ftol IS STILL A HOLE AND ALWAYS WILL BE. It takes its argument on
 * the FPU stack and this Ghidra has no calling convention that can
 * name ST0, so the FLD/FMUL feeding it are invisible and consecutive
 * calls lose all but the last return. Wherever the listing shows a
 * bare __ftol() the original is converting an angle to a cosine-table
 * index:
 *
 *     i   = (int)(a * 2607.594587618272)   0x004d0300
 *     cos = g_cosTable[i & 0x3fff]
 *     sin = g_cosTable[(i - 0x1000) & 0x3fff]
 *
 * THAT DOUBLE IS TRIG_IDX_PER_RAD, which is 0x4000 over the game's
 * TYPED 2*pi and NOT over the true one - see trig.h. 0x004d0300 is
 * its third home in the image.
 *
 * so a run of three of them followed by table loads is an INLINE
 * euler-to-matrix build - what mat34_from_euler does out of line.
 * Those are transcribed as mat34_from_euler and marked.
 */

#include <math.h>
#include <string.h>
#include <stdlib.h>

#include "racer.h"
#include "racerdraw.h"
#include "model.h"
#include "trig.h"
#include "bikes.h"
#include "skl.h"
#include "mesh.h"
#include "transform.h"
#include "cull.h"
#include "detail.h"
#include "log.h"
#include "trk.h"

/*
 * THE UNITS. 48 is the half-wheelbase form, read out of the image
 * rather than assumed - see racers.txt. The world units per metre
 * is BIKE_UNITS_PER_M in bikes.h.
 */
#define RD_HALF_AXLE        48.0f    /* 0x004d0e98                 */
#define RD_ONE               1.0f    /* 0x004d0e70                 */

/*
 * STEERING TO RADIANS, AND THE TWO SCALES ARE THE SAME MAGNITUDE
 * WITH OPPOSITE SIGN. Both are DOUBLES - FMUL double ptr at
 * 0x0045fb80 and 0x0045fe35 - and read as floats they are 3.37e12,
 * which is the trap this image sets over and over.
 *
 *     0x004d1048   +pi/4096    the WHEEL matrix
 *     0x004d1058   -pi/4096    the FORK pose angle
 *
 * So the fork part counter-rotates against the wheel mesh. That is
 * not a scale difference and it is not redundancy: writing the same
 * sign into both turns the fork the wrong way, and a port that used
 * one constant for both would look right only when steering is zero.
 */
#define RD_STEER_WHEEL   0.000766990393942821   /* 0x004d1048  +pi/4096 */
#define RD_STEER_FORK   -0.000766990393942821   /* 0x004d1058  -pi/4096 */

#define RD_FORKDIVE_K      40.0f    /* 0x004d0f8c, float            */

/*
 * 0.125, read as a float at FMUL float ptr [0x004d1050] - three
 * sites, all float. It is multiplied by BIKE_UNITS_PER_M (96) at the
 * point of use, exactly as the original does.
 *
 * KEPT UNFOLDED although 0.125 * 96 is 12: two constants at two
 * addresses are easier to check against a listing than one
 * product.
 */
#define RD_BOUNCE_K         0.125f  /* 0x004d1050, float            */

/*
 * 2.0, and it sits on both sides of the PersoAnim fork - MULTIPLYING
 * the speed in the clamped form and DIVIDING the top speed in the
 * unclamped one. So an ordinary bike loses all steering authority at
 * half its top speed, and a PersoAnim bike only at twice it, which
 * is to say never.
 */
#define RD_SPEED_K          2.0f    /* 0x004d0f60, float            */

/* The rider lift when he is down. PersoAnim picks between them. */
#define RD_RIDER_LIFT_ANIM 32.0f    /* 0x004d1024                   */
#define RD_RIDER_LIFT      64.0f    /* 0x004d1060                   */


/*
 * NEVER INDEX pose BY BYTE OFFSET. Use MR_RacerPart - racer.h lays
 * it out and asserts the offsets, and nothing here should restate
 * them.
 *
 * THE REASON IS THAT GETTING IT WRONG STILL COMPILES. Arithmetic
 * written against a byte array - r->pose + i*100 + 12 - goes on
 * compiling once pose is MR_RacerPart[5][20], and silently steps
 * 2000 bytes instead of one. At part 1 that reads 224000 bytes past
 * the pose.
 */

/* The rear arm. skl.h calls it SKL_PART_SWINGARM. */
#define RD_PART_SWINGARM 2

/* Pilote, the second subtree root. racer.h: null parent. */
#define RD_PART_RIDER_ROOT 3

/*
 * THE PART INDICES, and they are PART numbers rather than skeleton
 * members - skl.h names the same five. The remap between them is
 *
 *     member  0  1   2  3   4  5 ...
 *     part    0  1  15  2  16  3 ...
 *
 * [verified, racers.txt 9.59.4], which is why the swingarm is member
 * 3 and part 2 and both numbers appear below.
 */
#define RD_PART_CORPS         0
#define RD_PART_FORK          1
#define RD_PART_WHEEL_FRONT  15
#define RD_PART_WHEEL_REAR   16

/*
 * PART 3 IS ONE PART DOING TWO JOBS, and both are the original.
 *
 *   o it is the SECOND WALK ROOT - Skl_BuildPartMatrices is called on
 *     it with matRiderRoot, so the rider hangs off it;
 *   o it is the ARM PIVOT - the swingarm angle is measured about its
 *     pivot, read at racer + slot*2000 + 0x7b0, which is
 *     0x63c + 3*100 + 0x48 [verified, 0x0045ffdf].
 *
 * That is not a contradiction on a motorcycle: the swingarm pivot and
 * the rider root both sit mid-frame, within a few units of each other.
 * Two names for one index, spelled out so neither reads as a typo.
 */
#define RD_PART_ARM_PIVOT     3

/* The loop COUNTER the swingarm arrives on - member 3, part 2. */
#define RD_MEMBER_SWINGARM    3

/*
 * maybe_Racer_ShadeLevel's two returns (0x00465d60): 0x10 inside a
 * shaded track segment and 0x1f everywhere else.
 * racerdraw_shade_level below picks between them from
 * MR_TrkSegment.shaded - see trk.h - so a bike darkens under a
 * bridge and brightens coming out.
 */
#define RD_SHADE_FULL      0x1f
#define RD_SHADE_DARK      0x10
/* maybe_Racer_ShadeParts: 20.0f at 0x004d0f98, 32/3 at 0x004d109c. */
#define RD_SHADE_BIAS      20.0f
#define RD_SHADE_SCALE     10.666667f

/*
 * ORIGIN  FUN_00410120 @ 0x0041014e
 *
 * THE WORLD LIGHT DIRECTION, g_flLightDir at 0x0052c228. It is a
 * compile-time constant, not data: the init writes the immediates
 * 0xbf000000 and 0xbf800000 and then EBX, which is zero there.
 * Confirmed live in the running game as -0.5, -1.0, 0.0.
 *
 * maybe_Racer_ShadeParts normalises its own copy, so this stays as
 * the original stores it - unnormalised.
 */
static const float g_rdLightDir[3] = { -0.5f, -1.0f, 0.0f };

/* The two wheel slots, in the order the original stores them. */
#define RD_WHEEL_REAR         0
#define RD_WHEEL_FRONT        1
/* 0x004d13b0, a float. The draw-distance table is scaled by it. */
#define RD_CULL_DISTANCE_K 2.0f

/* The shadow, all four read out of maybe_Racer_BuildShadow. */
#define RD_SHADOW_HALF_WIDTH   40.0f   /* 0x42200000  */
#define RD_SHADOW_MID           0.5f   /* 0x004d1018  */
#define RD_SHADOW_LIFT_MID      8.0f   /* 0x004d1020  */
#define RD_SHADOW_LIFT_CORNER   4.0f   /* 0x004d0f80  */

/*
 * THE SHADOW IS TEXTURED, and this is what makes it a bike shape
 * rather than a rectangle. Racer_Create builds both quads as kind
 * 0x2c in colour 0, and both sample bank 1, taking two adjacent
 * rectangles out of one atlas row.
 *
 * THE UVs GO IN AS PIXELS, NOT NORMALISED, and that is a DIVERGENCE
 * FROM THE ORIGINAL'S STORED VALUES ON PURPOSE. RD_SHADOW_UV_SCALE
 * IS 1.0 FOR THAT REASON: it marks where the original's 1/255 would
 * go, kept as a named no-op so the difference is visible at the
 * multiply rather than only here.
 *
 * MOTO holds them normalised - 113/255 and so on, computed as
 * pixel * (1.0f/255.0f). Our rasterizer wants texels: every prim
 * that renders here carries 0..255, and the bike's own faces read
 * 225, 233, 249 and the like. COPYING THE ORIGINAL'S FLOATS
 * VERBATIM SAMPLES TEXEL (0,0) AT ALL FOUR CORNERS, which is the
 * colour key, and the whole shadow keys away and vanishes.
 *
 * SAME TRAP AS bankIndex, one field over: a value that is right in
 * the original's units is wrong in ours, and byte-identical to MOTO
 * is the wrong test for both. See docs/racer-draw-audit.txt 61.
 */
#define RD_SHADOW_UV_SCALE    1.0f
#define RD_SHADOW_BANK        1
#define RD_SHADOW_FLAT_COLOR  0xff000000u
#define RD_SHADOW_PRIM_KIND   0x2c

/*
 * ORIGIN  NONE - ours
 *
 * A BOUNDS-CHECKED ACCESSOR into our pose array. The original
 * indexes racer+0x63c inline at every site and cannot fail, because
 * the slot is known good by then; we call this from blocks that run
 * before the slot is validated.
 */
static float *rd_part_angle(MR_Racer *r, int part)
{
	int slot;

	if (!r)
		return NULL;

	slot = r->skeletonSlot;
	if (slot < 0 || slot >= RACER_LOD_SLOTS)
		return NULL;
	if (part < 0 || part >= RACER_PARTS_PER_LOD)
		return NULL;

	return r->pose[slot][part].angle;
}


/*
 * ORIGIN  Vec3_TransformByMatrix34 @ 0x00410010
 *
 * out = v through a 3x4 - rotation then translation.
 */
static void rd_transform(float out[3], const float v[3],
                         const float m[12])
{
	int i;

	for (i = 0; i < 3; i++)
		out[i] = v[0] * m[i] + v[1] * m[3 + i] + v[2] * m[6 + i]
	             + m[9 + i];
}


/*
 * ORIGIN  NONE - ours
 *
 * out = e * b, where e is a bare 3x3 and b a 3x4. mat34_from_euler
 * gives nine floats and mat34_multiply wants twelve, so the euler
 * result is widened with a zero translation - which is what the
 * original composes, its FromEuler writing zeroes into rows 9..11.
 */
static void rd_compose(float out[12], const float e[9],
                       const float b[12])
{
	float m[12];
	int   i;

	for (i = 0; i < 9; i++)
		m[i] = e[i];
	m[9] = 0.0f;
	m[10] = 0.0f;
	m[11] = 0.0f;

	mat34_multiply(out, m, b);
}


/*
 * ORIGIN  maybe_Racer_Draw @ 0x0045f290, at 0x0045f2d0..0x0045f5a0
 *
 * THE FAR LOD, skeletonSlot >= 2, and it is a different program
 * rather than a cheaper version of the same one. ONE MESH, no
 * skeleton, no part loop, no wheels, no rider - g_meshSkl[slot]
 * drawn whole.
 *
 *     m = FromEuler(0, +pi/2, 0) * FromEuler(0, 0, -lean) * root
 *     p = rearWheelContact + (0, RoueRayonAr * 96, 0) through m
 *     m.translation = p - camera.eye
 *     view = m * camera.view
 *
 * FOUR THINGS ARE ABSENT and each is a place a port would wrongly
 * reuse the near path:
 *
 *   o NO SUSPENSION PITCH. The far mesh never pitches.
 *   o NO STEERING, and no rearWheelSpin or frontWheelSpin either.
 *   o NO crashFallSide - it is -lean unconditionally, so a crashed
 *     bike at distance does NOT lie down. The near path substitutes
 *     crashFallSide for the lean and this does not.
 *   o NO MIDPOINT. It is placed from the REAR CONTACT lifted by the
 *     rear tyre radius, not from the wheelbase midpoint plus the two
 *     offsets. So the far and near LODs do not agree about where the
 *     bike is, and the jump at the LOD boundary is in the original.
 *
 * THE QUARTER TURN IS REAL. FromEuler(0, pi/2, 0) is composed on top
 * of the lean, so the far mesh is authored facing ninety degrees off
 * the near one. Dropping it because it looks redundant leaves every
 * distant bike sideways.
 *
 * IT PASSES A DIFFERENT LIVERY FROM THE NEAR PATH, and that is what
 * the two-LOD split really is here. The far branch pushes
 * racer+0x170 - livery - as Screen3D_CullAndQueueFaces' fourth
 * argument [verified, 0x0045f4f8 feeding the call at 0x0045f518],
 * where the near path dispatches on racer+0x5fc, liveryRider.
 * Racer_Create writes the same value to both, so they agree in a
 * race; racer.h has the page where they do not.
 *
 * THE PORT FOLLOWS THAT SPLIT, at the call in racerdraw_racer.
 *
 * SHADING is racerdraw_far_shade below; when it yields a level
 * rather than 0x1f, maybe_Mesh_ShadeVertices runs over the mesh.
 */
void racerdraw_far_lod(const MR_Racer *r, const MR_BikeCarac *c,
                       const float camEye[3], const float camView[12],
                       float out[12])
{
	float root[12];
	float rot[3];
	float e[9];
	float t[12];
	float m[12];
	float v[3];
	float p[3];
	int   i;

	if (!r || !c || !camEye || !camView || !out)
		return;

	memcpy(root, r->matBikeRoot, sizeof root);

	/* -lean, and NOT crashFallSide - see above. */
	rot[0] = 0.0f;
	rot[1] = 0.0f;
	rot[2] = -r->lean;
	mat34_from_euler(e, rot);
	rd_compose(t, e, root);

	rot[0] = 0.0f;
	rot[1] = (float)MR_HALF_PI_F;
	rot[2] = 0.0f;
	mat34_from_euler(e, rot);
	rd_compose(m, e, t);

	v[0] = 0.0f;
	v[1] = c->rayonAr * BIKE_UNITS_PER_M;
	v[2] = 0.0f;
	rd_transform(p, v, m);

	for (i = 0; i < 3; i++)
		m[9 + i] = (p[i] + r->rearWheelContact[i]) - camEye[i];

	mat34_multiply(out, m, camView);
}


/*
 * ORIGIN  maybe_Racer_Draw @ 0x0045f290, at 0x0045f4e0
 *
 * The far LOD's shade choice, and the OR is the point: an explicit
 * level of 0 is treated as "no shading" and takes 0x1f, the same as
 * the renderer flag being off. A port that only tested the flag
 * would draw those bikes black.
 */
int racerdraw_far_shade(int rendererFlag, int shadeLevel)
{
	if (rendererFlag == 0 || shadeLevel == 0)
		return 0x1f;
	return shadeLevel;
}


/*
 * ORIGIN  maybe_Racer_Draw @ 0x0045f290, at 0x0045f6a0
 *
 * The speed factor every steering term is scaled by. PersoAnim picks
 * between a clamped and an unclamped form, which is not a tidy-up
 * opportunity - the pocket bike really does get an unclamped one and
 * can therefore drive the factor negative at speed.
 */
float racerdraw_steer_factor(const MR_BikeCarac *c, float speed,
                             int persoAnim)
{
	float f;

	if (!c || c->realSpeedMax == 0.0f)
		return RD_ONE;

	if (persoAnim == 0) {
		f = RD_ONE - (speed * RD_SPEED_K) / c->realSpeedMax;
		if (f < 0.0f)
			f = 0.0f;
		if (f > RD_ONE)
			f = RD_ONE;
	} else {
		f = RD_ONE - speed / (c->realSpeedMax * RD_SPEED_K);
	}

	return f;
}


/*
 * ORIGIN  maybe_Racer_Draw @ 0x0045f290, at 0x0045f5a0..0x0045f830
 *
 * THREE MATRICES COME OFF matBikeRoot, NOT TWO, and getting that
 * wrong is easy: the original builds all three into the SAME stack
 * slot in turn and copies each out before the next overwrites it.
 * READ AS TWO CHAINS IT MISLABELS THE REAR WHEEL MATRIX AS THE
 * BODY.
 *
 *   rear  = FromEuler(rearWheelSpin) * FromEuler(0,0,roll) * root
 *
 *   front = FromEuler(frontWheelSpin)
 *         * FromEuler(0, steer*factor*+pi/4096, 0)
 *         * FromEuler(0, 0, roll)
 *         * FromEuler(-suspensionAngle, 0, 0) * root
 *
 *   body  = FromEuler(0, 0, roll)
 *         * FromEuler(-suspensionAngle, 0, 0) * root
 *
 * out = a * b throughout, so read each chain right to left.
 *
 * SO THE BODY IS THE FRONT CHAIN WITHOUT THE STEERING. The front
 * wheel gets the bars and frontWheelSpin on top of what the body has; the
 * rear gets neither, and gets rearWheelSpin and no suspension pitch
 * instead. Three different attitudes for three different meshes.
 *
 * THE BODY MATRIX IS WHAT REACHES matRiderRoot, which is how it can
 * be checked against MOTO at racer+0x3085 without any new
 * instrumentation. It matches to 0.02 of a degree through a whole
 * wheelie.
 *
 * roll IS crashFallSide WHEN DOWN, negated: a crashed bike lies on
 * the side the rider went down.
 *
 * rearWheelSpin AND frontWheelSpin ARE EULER TRIPLES whose X
 * component racer_spin_wheels advances by contact distance over
 * wheel radius; it is their only writer. See racer.h.
 */
void racerdraw_build_matrices(const MR_Racer *r, float steerFactor,
                              float rollOut[12], float rearOut[12],
                              float frontOut[12], float bodyOut[12],
                              float frontWheelOut[12])
{
	float root[12];
	float rot[3];
	float e[9];
	float t[12];
	float roll;
	int   i;

	if (!r || !rollOut || !rearOut || !frontOut || !bodyOut
	    || !frontWheelOut)
	    return;

	memcpy(root, r->matBikeRoot, sizeof root);
	roll = -(r->crashLatch != 0 ? r->crashFallSide : r->lean);

	/*
	 * THE ROLL-ONLY INTERMEDIATE IS AN OUTPUT, not scratch. The
	 * original keeps it in local_18c and the rear spring offset is
	 * transformed by IT rather than by the finished rear matrix -
	 * so rearWheelSpin is in the wheel mesh's attitude and not in the
	 * offset that places it.
	 */
	rot[0] = 0.0f;
	rot[1] = 0.0f;
	rot[2] = roll;
	mat34_from_euler(e, rot);
	rd_compose(rollOut, e, root);

	mat34_from_euler(e, r->rearWheelSpin);
	rd_compose(rearOut, e, rollOut);

	/* ---- the BODY matrix, which the FRONT one extends ------ */
	rot[0] = -r->suspensionAngle;
	rot[1] = 0.0f;
	rot[2] = 0.0f;
	mat34_from_euler(e, rot);
	rd_compose(t, e, root);

	rot[0] = 0.0f;
	rot[1] = 0.0f;
	rot[2] = roll;
	mat34_from_euler(e, rot);
	rd_compose(bodyOut, e, t);

	/* ---- and the FRONT wheel, the bars on top of the body -- */
	rot[0] = 0.0f;
	rot[1] = (float)((double)r->steering * steerFactor
	                 * RD_STEER_WHEEL);
	rot[2] = 0.0f;
	mat34_from_euler(e, rot);
	rd_compose(t, e, bodyOut);

	/*
	 * THE SPIN IS NOT IN frontOut. frontOut is the bars on the body
	 * and is what places the fork and builds the front frame; only
	 * the wheel MESH gets frontWheelSpin on top. FOLD IT IN HERE AND THE
	 * FORK PIVOT ORBITS, taking the wheel with it.
	 */
	for (i = 0; i < 12; i++)
		frontOut[i] = t[i];

	mat34_from_euler(e, r->frontWheelSpin);
	rd_compose(frontWheelOut, e, frontOut);
}

/*
 * ORIGIN  maybe_Racer_Draw @ 0x0045f290, at 0x0045f7a0..0x0045f830
 *
 * THE CONTACT PAIR IS REBUILT HERE, from the rear one and the unit
 * vector between them, and the front one is WRITTEN BACK:
 *
 *     d     = normalize(front - rear)
 *     front = rear + d * RoueAxeDistance * 96      <- stored
 *     mid   = rear + d * RoueAxeDistance * 48
 *
 * so whatever the tick left in racer+0x1b4 is replaced by a point
 * exactly one wheelbase from the rear contact along the same line.
 * The port's own wheelie swing survives that, because it moved both
 * the direction and the rear point rather than the length.
 */
void racerdraw_rebuild_contacts(MR_Racer *r, const MR_BikeCarac *c,
                                float midOut[3])
{
	float d[3];
	float len;
	int   i;

	if (!r || !c)
		return;

	for (i = 0; i < 3; i++)
		d[i] = r->frontWheelContact[i] - r->rearWheelContact[i];

	len = (float)sqrt((double)d[0] * d[0] + (double)d[1] * d[1]
	                  + (double)d[2] * d[2]);
	if (len == 0.0f)
		return;
	for (i = 0; i < 3; i++)
		d[i] /= len;

	for (i = 0; i < 3; i++) {
		r->frontWheelContact[i] = r->rearWheelContact[i]
		    + d[i] * c->axeDistance * BIKE_UNITS_PER_M;
		if (midOut)
			midOut[i] = r->rearWheelContact[i]
		        + d[i] * c->axeDistance * RD_HALF_AXLE;
	}
}


/*
 * ORIGIN  maybe_Racer_Draw @ 0x0045f290, at 0x0045f830..0x0045f960
 *
 * THE TWO VERTICAL OFFSETS, and the second one is the swingarm
 * squat. Both are built in bike-local space and transformed by a
 * matrix, then added to the rebuilt contact midpoint - AND NOT BY
 * THE SAME MATRIX:
 *
 *     rear  offset  ->  the ROLL-ONLY intermediate
 *     frame offset  ->  the BODY matrix
 *
 * The rear one is transformed before rearWheelSpin is applied and before
 * any suspension pitch; the frame one after the pitch. So the two
 * offsets are in different frames by one rotation each, and using
 * the finished rear matrix for the first - or the wheel matrix for
 * the second - is wrong in a way that only shows when the
 * suspension moves.
 *
 *     rear.y  = RoueRayonAr * 0.125 * 96 * springBounce
 *             + RoueRayonAr * 48
 *
 *     front.y = forkDive * 40 + HauteurCadre * 96
 *
 * THE springBounce TERM HAS NEVER BEEN COMPARED AGAINST MOTO.
 * racer_update_suspension drives it - sin(springPhase) *
 * springAmplitude - and it lands here scaled by the rear tyre
 * radius, so a loaded spring sits the bike lower at the back. But
 * every capture taken so far read springBounce 0 on every frame, so
 * the term is live and untested rather than inert. See
 * port-divergences.txt 203 and 245.
 *
 * THE FRONT OFFSET IS THE FORK DIVE, on the frame height. forkDive
 * is the peak-hold of longitudinal acceleration, so the nose lifts
 * under power and drops under braking through this term.
 *
 * BOTH RIDE THE MATRIX THEY ARE TRANSFORMED BY, not world up, so
 * they separate the moment the suspension pitch moves - which is
 * exactly what a single shared "up" axis would hide.
 */
void racerdraw_offsets(const MR_Racer *r, const MR_BikeCarac *c,
                       const float rollMat[12], const float bodyMat[12],
                       float rearOut[3], float frontOut[3])
{
	float v[3];

	if (!r || !c)
		return;

	if (rearOut) {
		v[0] = 0.0f;
		v[1] = (float)((double)c->rayonAr * RD_BOUNCE_K
		               * BIKE_UNITS_PER_M * (double)r->springBounce
		               + (double)c->rayonAr * RD_HALF_AXLE);
		v[2] = 0.0f;
		rd_transform(rearOut, v, rollMat);
	}

	if (frontOut) {
		v[0] = 0.0f;
		v[1] = (float)((double)r->forkDive * RD_FORKDIVE_K
		               + (double)c->hauteurCadre * BIKE_UNITS_PER_M);
		v[2] = 0.0f;
		rd_transform(frontOut, v, bodyMat);
	}
}


/*
 * THE LOD DISTANCE BANDS, the float immediates 0x447a0000,
 * 0x451c4000 and 0x453b8000 - 1000, 2500 and 3000. THOSE ARE BIT
 * PATTERNS, NOT ADDRESSES: every other constant in this file cites
 * an address and these three do not.
 */
#define RD_LOD_NEAR   1000.0f
#define RD_LOD_MID    2500.0f
#define RD_LOD_FAR    3000.0f

/*
 * g_nLodSklDisabled. NOT "level of detail off" - it means the
 * ARTICULATED body is not to be used, so a near racer falls to the
 * medium mesh instead of slot 0. Zero is the shipping value and the
 * port has no setting that writes it. [open - what sets it]
 */
int g_nLodSklDisabled = 0;


/*
 * ORIGIN  maybe_Racer_SelectLod @ 0x0045f1c0
 *
 * PICKS THE BODY MESH FROM THE CAMERA DISTANCE, and it runs in the
 * TICK, not the draw. Its only caller in the original is FUN_00467b30,
 * which is reached from maybe_Game_Tick and three racer updates - all
 * gameplay. maybe_Racer_Draw then takes -1 for its lod argument and
 * uses whatever this left in racer+0x638.
 *
 * THAT IS WHY A MENU PAGE DRAWS THE ARTICULATED MODEL. Nothing on a
 * menu calls this, so racer+0x638 keeps the 0 Racer_Ctor left, and
 * page 10 gets corps.skl rather than the medium body. Confirmed live
 * in all 23 captures of that page [verified, runtime-capture.txt 6].
 *
 * THE DISTANCE IS DIVIDED BY THE PROJECTION ZOOM, so the bands are in
 * screen terms rather than world units - zooming in keeps the near
 * model for longer.
 *
 *     dist >= 3000               -> 4  very low
 *     2500 <= dist < 3000        -> 3  low
 *     !disabled and outside      -> 0  the articulated skeleton
 *              1000..2500
 *     otherwise                  -> 2  medium
 *
 * THE THIRD TEST HAS A REDUNDANT INNER BRANCH in the original which
 * assigns 0 on both sides. It is not transcribed as two branches
 * because there is nothing to preserve - both arms are the same store
 * and no path reaches one without the other.
 */
void racerdraw_select_lod(MR_Racer *r, const float camEye[3])
{
	float d[3];
	float dist;
	int   i;

	if (!r || !camEye)
		return;

	for (i = 0; i < 3; i++)
		d[i] = r->position[i] - camEye[i];

	dist = (float)sqrt((double)d[0] * d[0] + (double)d[1] * d[1]
	                   + (double)d[2] * d[2]);
	if (g_projZoom != 0.0f)
		dist /= g_projZoom;

	r->cameraDist = dist;

	if (dist >= RD_LOD_FAR) {
		r->skeletonSlot = 4;
		return;
	}
	if (dist >= RD_LOD_MID) {
		r->skeletonSlot = 3;
		return;
	}
	if (!g_nLodSklDisabled && dist < RD_LOD_NEAR) {
		r->skeletonSlot = 0;
		return;
	}
	r->skeletonSlot = 2;
}

/* The side-plane spread, [0x004dbfe4]; g_projZoom scales it. */
#define RD_FRUSTUM_SIDE_K   0.6f

/* The four view planes, as normal[3] plus d. racerdraw_set_frustum
   fills them once a frame and racerdraw_visible tests them. */
static float s_frustum[4][4];
static int   s_frustumOk;


static void vec3_normalise_local(float v[3])
{
	double len = sqrt((double)v[0] * v[0] + (double)v[1] * v[1]
	                  + (double)v[2] * v[2]);

	if (len > 0.0) {
		v[0] = (float)(v[0] / len);
		v[1] = (float)(v[1] / len);
		v[2] = (float)(v[2] / len);
	}
}


/*
 * ORIGIN  FUN_0046e8b0 @ 0x0046e8b0
 *
 * THE PER-RACER CULL: a draw-distance test, then the frustum.
 *
 *     d = pos - camera.eye
 *     if (table[g_cfgDetailLevel * 0x24] * 2.0f < |d|)  return 0
 *     radius *= -4.0
 *     four Plane_Distance tests against radius
 *
 * THE CONSTANTS ARE READ, NOT GUESSED. 0x004d13b0 is 2.0f and
 * 0x004d13b8 is the DOUBLE -4.0 - the usual width trap, and the
 * reason the radius scale is not a float here.
 *
 * THE TABLE AT 0x0051d1bc IS ZERO IN THE IMAGE, filled at run time,
 * so its values can only come from a live read. Mind the stride: the
 * record is 0x24 and the distance is its FIRST FLOAT, while +0x0c
 * holds an unrelated int of 5500 that a mis-strided read picks up as
 * a plausible-looking distance.
 *
 * THE FRUSTUM IS WHAT CULLS A RACER WHO HAS PULLED AHEAD, measured
 * on both binaries: the distance half needs 36000 and never fires,
 * while the far plane cuts at 18000 + 4 * radius. A ghost at 7996
 * units was still drawn and one at 20048 was not.
 */
int racerdraw_visible(const float pos[3], float radius,
                      const float camEye[3])
{
	/*
	 * 0x0051d1bc, the first float of each 0x24 record. The table is
	 * zero in the image and filled at run time, so these are read
	 * live [verified].
	 *
	 * g_cfgDetailLevel REACHES 5, not 3 - read as 4 in a race.
	 */
	static const float distanceTable[6] = {
		18000.0f, 18000.0f, 15000.0f, 18000.0f, 18000.0f, 18000.0f
	};
	float d[3];
	float pd[4];
	float limit;
	float margin;
	int   level;
	int   i;

	if (!pos || !camEye)
		return 1;

	for (i = 0; i < 3; i++)
		d[i] = pos[i] - camEye[i];

	level = g_cfgDetailLevel;
	if (level < 0 || level >= (int)(sizeof distanceTable
	                                / sizeof distanceTable[0]))
	    level = 0;

	limit = distanceTable[level] * RD_CULL_DISTANCE_K;
	if (limit * limit <
	    d[0] * d[0] + d[1] * d[1] + d[2] * d[2])
	    return 0;

	/*
	 * AND THEN THE FRUSTUM, which is what actually culls a racer who
	 * has pulled ahead - the distance test above almost never fires.
	 *
	 * radius *= -4.0 (0x004d13b8, a DOUBLE), then the original's four
	 * tests in its own order and senses: planes 0 and 1 reject
	 * outright, planes 2 and 3 must BOTH read inside.
	 */
	if (!s_frustumOk)
		return 1;

	margin = radius * -4.0f;
	for (i = 0; i < 4; i++) {
		pd[i] = s_frustum[i][0] * pos[0] + s_frustum[i][1] * pos[1]
		      + s_frustum[i][2] * pos[2] + s_frustum[i][3];
	}

	if (pd[0] <= margin)          /* behind the eye */
	    return 0;
	if (-margin <= pd[1])         /* past the far plane */
	    return 0;
	if (!(margin < pd[2]))
		return 0;
	if (!(margin < pd[3]))
		return 0;
	return 1;
}


/*
 * ORIGIN  FUN_0046e320 @ 0x0046e320, whole
 *
 * Build the four view planes the cull tests. Called once a frame,
 * where the original builds them into the camera record from
 * maybe_Game_RenderFrame at 0x00480166.
 *
 * ALL FOUR NORMALS COME OFF THE VIEW MATRIX. fwd is the inverse's
 * third column and right its first, which for our row convention is
 * {view[2], view[5], view[8]} and {view[0], view[3], view[6]} - the
 * same two vectors race.c already takes for the segment span.
 *
 *     0  near      fwd,                      -(eye . n)
 *     1  far       fwd,                      -(far + eye . fwd)
 *     2  side      normalise(fwd + k right), -(eye . n)
 *     3  side      normalise(fwd - k right), -(eye . n)
 *
 * k is g_projZoom * 0.6 - the 0.6 is [0x004dbfe4] - and the far
 * distance is the same table entry the test above reads.
 *
 * CHECKED AGAINST THE LIVE PLANES, not just transcribed: a paused
 * MOTO with fwd (0, -0.1028, 0.9947), right (1, 0, 0) and k 0.6
 * holds plane 2 as (0.5145, -0.0881, 0.8530), and this arithmetic
 * reproduces it to four places.
 *
 * READING THE PLANES AFTER THE FRAME TELLS YOU THE LAST WRITE, NOT
 * THE ONE THE RACERS WERE CULLED AGAINST. maybe_Game_RenderFrame
 * builds the frustum twice - 0x00480166 and 0x004801f7 - and the
 * racers are drawn between them, so a paused read shows the second
 * build's far distance and not the one that mattered.
 */
void racerdraw_set_frustum(const float view[12], const float eye[3],
                           float farDist)
{
	float fwd[3], right[3], n[3];
	int   i, j;

	s_frustumOk = 0;
	if (!view || !eye)
		return;

	fwd[0]   = view[2];  fwd[1]   = view[5];  fwd[2]   = view[8];
	right[0] = view[0];  right[1] = view[3];  right[2] = view[6];

	for (j = 0; j < 3; j++) {
		s_frustum[0][j] = fwd[j];
		s_frustum[1][j] = fwd[j];
	}
	s_frustum[0][3] = -(eye[0] * fwd[0] + eye[1] * fwd[1]
	                    + eye[2] * fwd[2]);
	s_frustum[1][3] = s_frustum[0][3] - farDist;

	for (i = 0; i < 2; i++) {
		float k = g_projZoom * RD_FRUSTUM_SIDE_K;

		if (i == 1)
			k = -k;
		for (j = 0; j < 3; j++)
			n[j] = fwd[j] + k * right[j];
		vec3_normalise_local(n);
		for (j = 0; j < 3; j++)
			s_frustum[2 + i][j] = n[j];
		s_frustum[2 + i][3] = -(eye[0] * n[0] + eye[1] * n[1]
		                        + eye[2] * n[2]);
	}
	s_frustumOk = 1;
}

/*
 * ORIGIN  NONE - ours. THE BRIDGE, and it is why anything else in
 * this file can run at all.
 *
 * THE ORIGINAL KEEPS ITS SKELETON IN THE RACER, as MR_RacerPart
 * pose[5][20] at racer+0x63c. This port keeps it in MR_Skeleton,
 * loaded from the .skl - a different shape holding the same thing.
 * Every block of maybe_Racer_Draw that touches a bone needs the
 * pose array, so without this bridge none of them can write
 * anywhere.
 *
 * SO THE POSE ARRAY IS POPULATED FROM THE SKELETON instead of being
 * loaded. It costs one pass over seventeen members:
 *
 *     part          = skeleton remap[member]
 *     pose.pivot    = member.origin
 *     pose.angle    = frame.angle[part]     already by PART
 *     vertex range  = member.firstVertex / vertexCount
 *     parent        = &pose[remap[member.parent]]
 *
 * THE CHILD LIST IS BUILT PUSH-FRONT, ascending over parts, which
 * leaves child pointing at the HIGHEST-indexed child and next
 * descending from there. That is the order the DB records for the
 * real array - part 0's child is part 2, not part 1 - and getting it
 * backwards would still walk every part, just in the wrong order.
 *
 * faceCount AND faces STAY ZERO, because MR_SklMember has no
 * per-member face list. racerdraw_queue_parts is the only consumer
 * and it skips every part on faceCount == 0 - it runs and draws
 * nothing, which is not the same as not running. The blocker is a
 * type mismatch; see the note at the field below.
 */
void racerdraw_bind_skeleton(MR_Racer *r, const MR_Skeleton *skl,
                             const MR_AnimFrame *frame)
{
	MR_RacerPart *pose;
	int slot, i, part;

	if (!r || !skl)
		return;

	slot = r->skeletonSlot;
	if (slot < 0 || slot >= RACER_LOD_SLOTS)
		return;

	pose = &r->pose[slot][0];
	memset(pose, 0, RACER_PARTS_PER_LOD * sizeof(MR_RacerPart));

	/*
	 * THE SLOT'S PART COUNT, at perSlotArrays[slot] - racer+0x2d4c.
	 * MOTO holds (17, 17, 0, 0, 0).
	 *
	 * IT IS NOT COSMETIC. maybe_Racer_ShadeParts loops
	 * i < partCount[slot], so a zero makes the whole per-vertex
	 * shading pass run zero times and write nothing - with correct
	 * normals sitting right next to it. racer_pose_slot_count and
	 * racerdraw_queue_parts read the same field.
	 */
	{
		int n = skl->memberCount;

		if (n < 0)
			n = 0;
		if (n > RACER_PARTS_PER_LOD)
			n = RACER_PARTS_PER_LOD;
		memcpy(&r->perSlotArrays[slot * 4], &n, sizeof n);
	}

	/* Fields first, tree second - the tree needs every node to exist. */
	for (i = 0; i < skl->memberCount; i++) {
		part = skl->remap[i];
		if (part < 0 || part >= RACER_PARTS_PER_LOD)
			continue;

		pose[part].pivot[0] = skl->member[i].origin[0];
		pose[part].pivot[1] = skl->member[i].origin[1];
		pose[part].pivot[2] = skl->member[i].origin[2];
		pose[part].firstVertex = skl->member[i].firstVertex;
		pose[part].vertexCount = skl->member[i].vertexCount;
		/*
		 * THE FACE SLICE IS NOT COPIED HERE. Skl_Load keeps it at
		 * +0x698/+0x69c of the same record and MR_RacerPart has the
		 * fields, but part->faces is MR_Primitive * while the
		 * skeleton's own faces are MESH_FACE - the loader form, with
		 * int indices and no 0x40 stride. The two are not the same
		 * array and assigning one to the other would type-confuse
		 * racerdraw_queue_parts. racerdraw_build_normals reads the
		 * skeleton's slice directly instead.
		 *
		 * [open] THE GAP IS AN MR_Primitive-FORM PER-PART SLICE.
		 * Until one exists racerdraw_queue_parts draws nothing.
		 * This is the only statement of that gap in the file.
		 */

		if (frame && part < ANIM_MAX_JOINTS) {
			pose[part].angle[0] = frame->angle[part][0];
			pose[part].angle[1] = frame->angle[part][1];
			pose[part].angle[2] = frame->angle[part][2];
		}
	}

	for (i = 0; i < skl->memberCount; i++) {
		int parentMember = skl->member[i].parent;
		int parentPart;

		part = skl->remap[i];
		if (part < 0 || part >= RACER_PARTS_PER_LOD)
			continue;

		/*
		 * THE RIDER ROOT IS SEVERED, AND WITHOUT THIS THE BIKE
		 * COMES APART.
		 *
		 * CORPS.SKL gives member 5 - part 3, Pilote - a parent of
		 * member 0, so the file describes ONE tree. The racer's
		 * live tree is TWO: racer.h records a null parent at both
		 * part 0 and part 3, and racerdraw_part_walks walks them
		 * as separate roots with different matrices.
		 *
		 * The walk recurses SIBLINGS with the parent's matrix -
		 * skl_build_part_matrices_rec(next, parent). Leaving part 3 on
		 * part 0's child chain means the rider walk reaches parts
		 * 1 and 2 as its siblings and rewrites the fork and the
		 * swingarm with matRiderRoot, silently, after the bike
		 * walk got them right.
		 */
		if (part == RD_PART_RIDER_ROOT)
			continue;

		if (parentMember < 0 || parentMember >= skl->memberCount)
			continue;

		parentPart = skl->remap[parentMember];
		if (parentPart < 0 || parentPart >= RACER_PARTS_PER_LOD)
			continue;

		pose[part].parent = &pose[parentPart];
		pose[part].next   = pose[parentPart].child;   /* push front */
		pose[parentPart].child = &pose[part];
	}
}


/*
 * ORIGIN  NONE - ours. The shared half of the two originals below.
 *
 * L, the joint-local matrix: the euler of the part angles, with the
 * translation set so the rotation happens about the pivot rather
 * than about the model origin.
 */
static void skl_part_local(const MR_RacerPart *part, float local[12])
{
	float e[9];
	int   i;

	mat34_from_euler(e, part->angle);

	for (i = 0; i < 9; i++)
		local[i] = e[i];

	for (i = 0; i < 3; i++)
		local[9 + i] = -part->pivot[0] * e[i]
	                   - part->pivot[1] * e[3 + i]
	                   - part->pivot[2] * e[6 + i]
	                   + part->pivot[i];
}


/*
 * ORIGIN  Skl_BuildPartMatricesRec @ 0x00464190
 *
 * Every non-root part is posed here. It is a different function from
 * the root entry, with a different composition rule:
 *
 *     root  0x00463d90   RET 0x30   m = inv(parent) * L
 *     rec   0x00464190   RET        m = parent * L
 *
 * The sibling takes the shared parent, the child takes this matrix.
 * The original recurses on ->next by CALL and descends to ->child by
 * looping back into its own frame at 0x0046448b.
 *
 * GOTCHA: BOTH WALK ROOTS STORE AN IDENTITY ROTATION, so at depth 1
 * every candidate composition rule collapses to L and parts 1, 2 and
 * 4 agree under all of them. Only depth 2 and below discriminate; a
 * check that stops at the first level of children cannot see a wrong
 * rule here.
 */
static void skl_build_part_matrices_rec(MR_RacerPart *part,
                                        const float parent[12])
{
	float local[12];

	while (part) {
		skl_part_local(part, local);
		mat34_multiply(part->matrix, parent, local);

		if (part->next)
			skl_build_part_matrices_rec(part->next, parent);

		/* The child descends in the same frame - 0x00464486. */
		parent = part->matrix;
		part   = part->child;
	}
}


/*
 * ORIGIN  Skl_BuildPartMatrices @ 0x00463d90
 *
 * The root entry, and it poses exactly one part; everything below it
 * goes to skl_build_part_matrices_rec. maybe_Racer_Draw calls it
 * twice, on part 0 with the body matrix and on part 3 with
 * matRiderRoot.
 *
 * The incoming parent arrives BY VALUE - hence RET 0x30 here against
 * RET in the recursive half - and is inverted before composing:
 *
 *     3x3    a1 a4 a7 / a2 a5 a8 / a3 a6 a9      the TRANSPOSE
 *     trans  -(t . row0), -(t . row1), -(t . row2)
 *
 * which is mat34_invert_rigid.
 *
 * Neither root has a sibling - part 0 and part 3 both read next =
 * NULL - so the ->next arm is transcribed but unexercised.
 */
static void skl_build_part_matrices(MR_RacerPart *part,
                                    const float parent[12])
{
	float local[12];
	float inv[12];

	if (!part)
		return;

	skl_part_local(part, local);

	/* inv(parent) * L - the ROOT rule, and only the root. */
	mat34_invert_rigid(inv, parent);
	mat34_multiply(part->matrix, inv, local);

	if (part->next)
		skl_build_part_matrices_rec(part->next, part->matrix);
	if (part->child)
		skl_build_part_matrices_rec(part->child, part->matrix);
}


/*
 * ORIGIN  maybe_Racer_Draw @ 0x0045f290, at 0x00460f00..0x00460f70
 *
 * TWO SKELETON WALKS, TWO ROOTS, TWO MATRICES:
 *
 *     Skl_BuildPartMatrices(pose[slot*20 + 0], bodyMat)
 *     Skl_BuildPartMatrices(pose[slot*20 + 3], matRiderRoot)
 *
 *     0x00460f16  LEA ECX,[EAX+EBX+0x63c]   part 0, corps
 *     0x00460f51  LEA ECX,[EAX+EBX+0x768]   part 3, Pilote
 *
 * 0x768 - 0x63c is 300, three part records, so the second root
 * really is part 3 and not a different array.
 *
 * The bike subtree hangs off part 0 with the body draw matrix and
 * the rider subtree off part 3 with matRiderRoot. They are separate
 * trees in one array, which is why the rider can be thrown while the
 * bike keeps riding: only the second walk's matrix changes.
 *
 * THE MATRIX IS PASSED BY VALUE. Skl_BuildPartMatrices ends RET 0x30
 * - forty-eight bytes, exactly a 3x4. THE TWELVE "undefined4"
 * PARAMETERS IN THE DB ARE ONE MATRIX, not twelve values; the plate
 * at 0x00463d90 carries the right prototype in prose.
 *
 * THE ORIGINAL COPIES IT TO A STACK SLOT FIRST, with Matrix34_Copy,
 * rather than passing the racer field directly - because passing by
 * value means the callee may write to it and matRiderRoot must
 * survive.
 *
 * The composition rule is not the same at every level: the root
 * inverts the incoming matrix and the recursive half does not. See
 * skl_build_part_matrices and skl_build_part_matrices_rec.
 */
void racerdraw_part_walks(MR_Racer *r, const float bodyMat[12])
{
	int slot;

	if (!r || !bodyMat)
		return;

	slot = r->skeletonSlot;
	if (slot < 0 || slot >= RACER_LOD_SLOTS)
		return;

	skl_build_part_matrices(&r->pose[slot][0], bodyMat);
	skl_build_part_matrices(&r->pose[slot][3], r->matRiderRoot);
}


/*
 * ORIGIN  maybe_Racer_Draw @ 0x0045f290, at 0x00460f70..0x00461490
 *
 * THE FORK PART MATRIX IS OVERWRITTEN AFTER THE WALK, which is the
 * whole reason the walk cannot be left to do the job:
 *
 *     R      = euler(pose[fork].angle)        the steering, inline
 *     local  = R applied about -pivot, then + scaledTip
 *     out    = local composed with the PARENT part's matrix
 *     pose[fork].matrix = out
 *
 * so the fork is placed from the SCALED TIP racerdraw_fork_tip
 * produced, not from its bind pivot, and rotated by the angle
 * racerdraw_steer_pose wrote. The walk built a matrix for it a
 * moment earlier and this throws that away.
 *
 * THE PARENT MATRIX IS READ AT +0x18 and its translation at
 * +0x3c..+0x44 - MR_RacerPart's own fields, laid out in racer.h
 * and in the plate at 0x00463d90.
 *
 * IT IS GUARDED ON THE PARENT EXISTING. A root part has none and the
 * override is skipped entirely, leaving whatever the walk produced.
 *
 * THE ROTATION IS THE SAME INLINE COSINE-TABLE BUILD as the arm
 * matrix, but with all THREE angles live this time - so it really is
 * a general euler here and mat34_from_euler is the right transcription
 * rather than a shortcut.
 */
void racerdraw_fork_part_matrix(const float angle[3], const float pivot[3],
                                const float scaledTip[3],
                                const float parentMat[12], float out[12])
{
	float e[9];
	float local[12];
	float neg[3];
	int   i;

	if (!angle || !pivot || !scaledTip || !out)
		return;

	mat34_from_euler(e, angle);

	neg[0] = -pivot[0];
	neg[1] = -pivot[1];
	neg[2] = -pivot[2];

	for (i = 0; i < 9; i++)
		local[i] = e[i];

	/*
	 * THE PIVOT IS ROTATED AND NEGATED INTO THE TRANSLATION, which
	 * is what turns a rotation about the origin into one about the
	 * pivot - then the scaled tip moves it to where the fork now
	 * reaches. Both in one translation row rather than two composes.
	 */
	for (i = 0; i < 3; i++)
		local[9 + i] = neg[0] * e[i] + neg[1] * e[3 + i]
	                   + neg[2] * e[6 + i] + scaledTip[i];

	if (parentMat)
		mat34_multiply(out, local, parentMat);
	else
		memcpy(out, local, 12 * sizeof(float));
}


/*
 * ORIGIN  maybe_Racer_Draw @ 0x0045f290, at 0x0045f960..0x0045f9d0
 *
 * THE DRAW POSITION IS THE MIDPOINT PLUS BOTH OFFSETS, summed and
 * then added:
 *
 *     draw = mid + (rearOffset + frontOffset)
 *
 * and NOT mid plus one offset along one axis. The two offsets ride
 * different matrices - racerdraw_offsets - so their sum is only
 * parallel to the bike's up axis while the suspension AND the
 * steering are both at rest.
 *
 * MEASURING IT AT REST IS THE TRAP, and the port fell into it once:
 * a bike standing still makes the sum look like one scalar times
 * one axis, and a placement written from that observation comes
 * apart the moment anything moves.
 */
void racerdraw_draw_position(const float mid[3], const float rear[3],
                             const float front[3], float out[3])
{
	int i;

	if (!mid || !rear || !front || !out)
		return;

	for (i = 0; i < 3; i++)
		out[i] = mid[i] + (rear[i] + front[i]);
}


/*
 * ORIGIN  maybe_Racer_Draw @ 0x0045f290, at 0x0045f9d0..0x0045fa30
 *
 * THE VIEW MATRIX IS REBUILT FROM AN EULER, not composed directly,
 * and that is not a detour worth optimising away:
 *
 *     rel   = drawPos - camera.eye          camera+0x98
 *     euler = Mat34_ToEuler2(bodyMat)
 *     m     = Mat34_FromEulerPos(euler, rel)
 *     view  = m * camera.view               camera+0x68
 *
 * Going matrix -> euler -> matrix DISCARDS ANY SCALE OR SHEAR the
 * body matrix carried and re-orthonormalises it. Composing bodyMat
 * with the camera directly would keep them, so reproduce the round
 * trip rather than shortcutting it.
 *
 * [open] DOES bodyMat EVER CARRY SCALE? Unread, and THREE things
 * in this file turn on the answer - this is the only statement of
 * the question:
 *
 *   o this round trip, which exists to discard it;
 *   o racerdraw_fork_scale, where posed == bindLen only while the
 *     chain is orthonormal;
 *   o racerdraw_arm_angle, whose point-difference axes differ from
 *     reading the matrix rows only under scale.
 *
 * The third is the strongest hint that the answer is yes: the
 * original pays for three extra transforms there, which nobody
 * does by accident.
 */
void racerdraw_view_matrix(const float bodyMat[12], const float drawPos[3],
                           const float camEye[3], const float camView[12],
                           float out[12])
{
	float euler[3];
	float rel[3];
	float m[12];
	int   i;

	if (!bodyMat || !drawPos || !camEye || !camView || !out)
		return;

	for (i = 0; i < 3; i++)
		rel[i] = drawPos[i] - camEye[i];

	mat34_to_euler(euler, bodyMat);
	mat34_from_euler(m, euler);
	m[9]  = rel[0];
	m[10] = rel[1];
	m[11] = rel[2];

	mat34_multiply(out, m, camView);
}


/*
 * ORIGIN  maybe_Racer_Draw @ 0x0045f290, at 0x0045fa30..0x0045fd60
 *
 * THE SETUP IS PART OF IT. 0x0045fa30 onward inits the two matrix
 * slots, builds the two spring offsets and transforms the fork
 * pivot; the solve proper starts at 0x0045fc00.
 *
 * ONE PIECE OF THAT RANGE IS NOT OURS: 0x0045fb16 and 0x0045fb20
 * are Mat34_ToEuler2 and Mat34_FromEulerPos - the FRONT frame,
 * which racerdraw_rear_frame builds from its other call site. Same
 * function, two sites; the tag on rear_frame names the rear one.
 *
 * THE FORK SOLVE. skl_refork_front does the same job for page 10,
 * which has no MR_Racer to hand these; the race path comes here.
 * Everything is measured from ONE pivot - part 1, the fork - taken
 * into world through the FRONT wheel matrix:
 *
 *     pivotW = pose[slot*20 + 1].pivot  through frontMat
 *     steerM = FromEuler(0, steer*factor*pi/4096, 0) * bodyMat
 *     dir    = (pose[tip].pivot - pose[1].pivot) through steerM
 *     tipW   = pivotW + dir
 *     A      = |tipW - pivotW|                  the posed length
 *     B      = |frontContact - pivotW| - RoueRayonAv * 96
 *     wheelW = pivotW + normalize(tipW - pivotW) * B
 *
 * so the wheel is placed by walking B down the fork axis from the
 * pivot - B being how far the CONTACT is, less the tyre radius. The
 * fork points at the wheel and the wheel sits on the fork.
 *
 * THE STEERING IS APPLIED TO THE BODY MATRIX HERE, AGAIN. It was
 * already in the front wheel matrix; this is a second, separate
 * composition for the fork DIRECTION. Two consumers of one angle,
 * which is why the wheel can be yawed while the fork line is not.
 *
 * tip IS NOT A CONSTANT PART INDEX. It comes out of the skeleton
 * table at unk_2d88[slot] + 8 - a per-LOD member index - so a port
 * that hardcodes 15 is right only for the LOD it was read on.
 */
void racerdraw_fork_solve(const MR_Racer *r, const MR_BikeCarac *c,
                          float steerFactor, const float frontMat[12],
                          const float bodyMat[12], const float bindPivot[3],
                          const float bindTip[3], float pivotOut[3],
                          float wheelOut[3], float *lenOut, float *reachOut)
{
	float rot[3];
	float e[9];
	float steerM[12];
	float d[3];
	float dir[3];
	float axis[3];
	float pivotW[3];
	float posed, reach;
	int   i;

	if (!r || !c)
		return;

	rd_transform(pivotW, bindPivot, frontMat);

	rot[0] = 0.0f;
	rot[1] = (float)((double)r->steering * steerFactor
	                 * RD_STEER_WHEEL);
	rot[2] = 0.0f;
	mat34_from_euler(e, rot);
	rd_compose(steerM, e, bodyMat);

	for (i = 0; i < 3; i++)
		d[i] = bindTip[i] - bindPivot[i];

	/*
	 * DIRECTION ONLY - the translation rows are deliberately not
	 * added. The original transforms this as a delta between two
	 * pivots, then ADDS pivotW; adding the translation here would
	 * count the placement twice.
	 */
	for (i = 0; i < 3; i++)
		dir[i] = d[0] * steerM[i] + d[1] * steerM[3 + i]
	             + d[2] * steerM[6 + i];

	posed = (float)sqrt((double)dir[0] * dir[0] + (double)dir[1] * dir[1]
	                    + (double)dir[2] * dir[2]);
	if (posed == 0.0f)
		return;
	for (i = 0; i < 3; i++)
		axis[i] = dir[i] / posed;

	for (i = 0; i < 3; i++)
		d[i] = r->frontWheelContact[i] - pivotW[i];
	reach = (float)sqrt((double)d[0] * d[0] + (double)d[1] * d[1]
	                    + (double)d[2] * d[2])
	        - c->rayonAv * BIKE_UNITS_PER_M;

	for (i = 0; i < 3; i++) {
		if (pivotOut)
			pivotOut[i] = pivotW[i];
		if (wheelOut)
			wheelOut[i] = pivotW[i] + axis[i] * reach;
	}
	if (lenOut)
		*lenOut = posed;
	if (reachOut)
		*reachOut = reach;
}


/*
 * ORIGIN  maybe_Racer_Draw @ 0x0045f290, at 0x0045fd60..0x0045fdc0
 *
 * THE FORK SCALE, AND IT IS A RATIO - the term that makes the fork
 * extend at all:
 *
 *     pivotWorld = pose[slot*20 + 1].pivot through the wheel matrix
 *     A          = |tip - pivotWorld|      the posed fork length
 *     B          = |frontContact - pivotWorld| - RoueRayonAv * 96
 *     S          = |bindTip - bindPivot| * A / B
 *
 * B is the distance from the fork pivot to where the wheel actually
 * IS, less the tyre radius - so as the wheel moves away from the
 * pivot B grows and S shrinks. The result scales the bind-space fork
 * so its tip lands on the wheel.
 *
 * IT IS NOT A LENGTH AND MUST NOT BE CLAMPED. B can be small when the
 * suspension is fully compressed and the ratio is what the original
 * uses regardless.
 *
 * posed IS bindLen, SO S REDUCES TO bindLen*bindLen/reach.
 * racerdraw_fork_solve builds posed by rotating (bindTip -
 * bindPivot) through steerM and taking its length, and a rotation
 * preserves length - which is why skl_refork_front spells the same
 * scale L0*L0/B and the two do NOT disagree. See skl.c.
 *
 * BOTH SPELLINGS ARE KEPT ON PURPOSE. The original measures the
 * length twice and this follows it; skl.c collapses the pair. They
 * part company only if bodyMat ever carries scale, which is the
 * open question at racerdraw_view_matrix.
 */
float racerdraw_fork_scale(float posedLen, float bindLen, float reach)
{
	if (reach == 0.0f)
		return bindLen;
	return (bindLen * posedLen) / reach;
}


/*
 * ORIGIN  maybe_Racer_Draw @ 0x0045f290, at 0x0045fd60..0x0045fda0
 *
 * THE FRONT WHEEL IS DRAWN AT AN OFFSET FROM ITS OWN CONTACT, not
 * at an absolute point:
 *
 *     offset = wheelPoint - frontWheelContact
 *
 * because maybe_Inst_DrawWithMatrix adds the instance's own position
 * - which IS racer+0x1b4, the contact - to whatever it is handed. So
 * the sum is the solved wheel point and the offset is the correction
 * from where the contact says the wheel is to where the fork puts it.
 *
 * DRAWING IT AT wheelPoint DIRECTLY WOULD DOUBLE THE PLACEMENT. The
 * instance position is not a spare field to ignore.
 */
void racerdraw_front_wheel_offset(const MR_Racer *r,
                                  const float wheelPoint[3],
                                  float out[3])
{
	int i;

	if (!r || !wheelPoint || !out)
		return;

	for (i = 0; i < 3; i++)
		out[i] = wheelPoint[i] - r->frontWheelContact[i];
}


/*
 * ORIGIN  maybe_Racer_Draw @ 0x0045f290, at 0x0045fda0..0x0045fe10
 *
 * THE FORK TIP IS MOVED IN BIND SPACE, which is the other half of
 * racerdraw_fork_scale and the part that actually stretches the mesh:
 *
 *     S   = |bindTip - bindPivot| * posed / reach
 *     tip = bindPivot + normalize(bindTip - bindPivot) * S
 *
 * so the bind-space fork is re-pointed along its own axis at the
 * scaled length. The result is what the part matrices are built
 * from, so the whole fork subtree follows.
 *
 * IT IS THE BIND DIRECTION, NOT THE POSED ONE. The scale came from
 * the posed lengths; the direction it is applied along is the bind
 * one, and mixing those up leaves the fork pointing at the wheel
 * with the wrong length or the right length in the wrong direction.
 */
void racerdraw_fork_tip(const float bindPivot[3], const float bindTip[3],
                        float posed, float reach, float out[3])
{
	float d[3];
	float len, s;
	int   i;

	if (!bindPivot || !bindTip || !out)
		return;

	for (i = 0; i < 3; i++)
		d[i] = bindTip[i] - bindPivot[i];

	len = (float)sqrt((double)d[0] * d[0] + (double)d[1] * d[1]
	                  + (double)d[2] * d[2]);
	if (len == 0.0f)
		return;

	s = racerdraw_fork_scale(posed, len, reach);

	for (i = 0; i < 3; i++)
		out[i] = bindPivot[i] + (d[i] / len) * s;
}


/*
 * ORIGIN  maybe_Racer_Draw @ 0x0045f290, at 0x0045fe10..0x0046019a
 *
 * THE REAR DRAW FRAME, and it is built the same way the view matrix
 * is - through an euler and back:
 *
 *     euler = Mat34_ToEuler(rearMat)
 *     out   = Mat34_FromEulerPos(euler, drawPos)
 *
 * so the rear wheel is drawn on the REAR chain's orientation at the
 * SAME draw position the body uses. Orientation from one matrix,
 * position from the shared placement.
 *
 * THAT IS WHY THE REAR WHEEL DOES NOT PITCH WITH THE SUSPENSION and
 * the front one does: the rear chain has no suspension pitch in it,
 * the front chain does, and both are drawn at the same point.
 */
void racerdraw_rear_frame(const float rearMat[12], const float drawPos[3],
                          float out[12])
{
	float euler[3];
	float e[9];
	int   i;

	if (!rearMat || !drawPos || !out)
		return;

	mat34_to_euler(euler, rearMat);
	mat34_from_euler(e, euler);

	for (i = 0; i < 9; i++)
		out[i] = e[i];
	out[9]  = drawPos[0];
	out[10] = drawPos[1];
	out[11] = drawPos[2];
}


/*
 * ORIGIN  maybe_Racer_Draw @ 0x0045f290, at 0x00460340
 *
 * THE SOLVED SWINGARM ANGLE. The original stores it here and NOTHING
 * READS IT - 0x00580a70 has exactly one xref in the image and it is
 * this write. A dead store, kept because it is the only point in the
 * swingarm chain where the two sides can be compared without a hook.
 */
float g_armAngle;

/*
 * ORIGIN  NONE - ours.
 *
 * READ-ONLY MIRRORS OF THE DRAW, so its stages can be diffed
 * against MOTO without a hook - the swingarm chain, the per-part
 * view matrices and the far LOD. Written, never read; the draw
 * behaves identically whether or not anything looks at these.
 */
float g_rdRearMat[12];
float g_rdFrontWheelMat[12];   /* the spinning FRONT one */
float g_rdRearFrameW[12];
float g_rdArmW[12];
float g_rdFrontWheelOff[3];
float g_rdRearWheelOff[3];

/* One view matrix per part, as the part loop builds them. */
float g_rdPartView[RACER_PARTS_PER_LOD][12];
int   g_rdPartViewSeen[RACER_PARTS_PER_LOD];

/* The two special-cased members, 0 and RD_MEMBER_SWINGARM. */
float g_rdViewPart0[12];
float g_rdViewPart3[12];

/* The far-LOD view matrix, for the 0x0045f43a comparison. */
float g_rdFarLod[12];
float g_rdFarLodPos[3];

/*
 * ORIGIN  maybe_Racer_Draw @ 0x0045f290, at 0x0046019a..0x0046030d
 *
 * THE SWINGARM ANGLE, and the axes are DERIVED BY TRANSFORMING
 * POINTS rather than read out of the matrix rows:
 *
 *     P = pivot3            through the rear draw matrix
 *     X = pivot3 - (1,0,0)  through the same
 *     Z = pivot3 - (0,0,1)  through the same
 *     u = normalize(X - P)      the negated lateral axis
 *     w = Z - P                 the negated forward axis
 *     d = rearWheelContact - P
 *     angle = atan2(dot(u, w x d), dot(d, w))
 *
 * THE POINT-DIFFERENCE FORM IS NOT EQUIVALENT TO READING ROWS if the
 * matrix carries scale, and the original went to the trouble - three
 * transforms instead of none. See the [open] at
 * racerdraw_view_matrix, which this is evidence for.
 *
 * A SIGNED ANGLE ABOUT u, NOT THE ANGLE BETWEEN d AND w. The
 * numerator is the component of the cross product ALONG the pivot
 * axis, which is what gives the arm a direction. atan2(|cross|, dot)
 * agrees only while d is square to the bike and is wrong the moment
 * it is not - it matched two bikes to six decimals and missed the
 * pocket dirt bike, whose arm pivot sits 8.8 off the centreline.
 *
 * THE 1/(|w| * |d|) FACTOR THE ORIGINAL APPLIES TO BOTH ARGUMENTS
 * CANCELS. atan2 is invariant under a common positive scale, so it
 * is dropped here rather than reproduced - the only place in this
 * file where the arithmetic is not followed literally, and it is
 * exact rather than a simplification.
 *
 * THE CROSS IS w x d. Taking it the other way negates the numerator
 * and mirrors the arm: the magnitude stays exactly right, so the
 * error reads as a wrong swingarm angle rather than as a broken one.
 * Measured against the original stash at 0x00580a70 - MOTO 0.413850,
 * d x w gives -0.413851 - on a straight bike and again rotated 40
 * degrees, so it is not an artefact of an identity frame.
 *
 * w came back as exactly -matBikeRoot row 2 under a trace hook at
 * 0x0046031d, to four decimals, on a frame where suspensionAngle was
 * at its clamp.
 */
float racerdraw_arm_angle(const MR_Racer *r, const float rearMat[12],
                          const float bindPivot3[3])
{
	float P[3], X[3], Z[3];
	float u[3], w[3], d[3], cr[3];
	float p[3];
	float len;
	int   i;

	if (!r || !rearMat || !bindPivot3)
		return 0.0f;

	rd_transform(P, bindPivot3, rearMat);

	p[0] = bindPivot3[0] - RD_ONE;
	p[1] = bindPivot3[1];
	p[2] = bindPivot3[2];
	rd_transform(X, p, rearMat);

	p[0] = bindPivot3[0];
	p[1] = bindPivot3[1];
	p[2] = bindPivot3[2] - RD_ONE;
	rd_transform(Z, p, rearMat);

	for (i = 0; i < 3; i++) {
		u[i] = X[i] - P[i];
		w[i] = Z[i] - P[i];
		d[i] = r->rearWheelContact[i] - P[i];
	}

	len = (float)sqrt((double)u[0] * u[0] + (double)u[1] * u[1]
	                  + (double)u[2] * u[2]);
	if (len != 0.0f) {
		for (i = 0; i < 3; i++)
			u[i] /= len;
	}

	/* w x d, NOT d x w - see the note above. */
	cr[0] = w[1] * d[2] - w[2] * d[1];
	cr[1] = w[2] * d[0] - w[0] * d[2];
	cr[2] = w[0] * d[1] - w[1] * d[0];

	g_armAngle = (float)atan2((double)(u[0] * cr[0] + u[1] * cr[1]
	                                   + u[2] * cr[2]),
	                          (double)(d[0] * w[0] + d[1] * w[1]
	                                   + d[2] * w[2]));
	return g_armAngle;
}


/*
 * ORIGIN  maybe_Racer_Draw @ 0x0045f290, at 0x0045fe10
 *
 * THE BARS REACH THE FORK THROUGH THE POSE, not through a matrix -
 * and this is the second consumer of the steering angle, on its own
 * scale. Part 1 is the fork; its angle triple is overwritten every
 * frame with a pure Y rotation.
 *
 * THE WHEEL MATRIX'S ANGLE IS A STACK TEMPORARY AND THIS ONE IS
 * NOT: 0x0045fe5c addresses pose[slot][1].angle directly. A port
 * that builds only the wheel matrix has a fork that never turns.
 */
void racerdraw_steer_pose(MR_Racer *r, float steerFactor)
{
	float *angle;

	if (!r)
		return;

	angle = rd_part_angle(r, RD_PART_FORK);
	if (!angle)
		return;

	angle[0] = 0.0f;
	angle[1] = (float)((double)r->steering * steerFactor * RD_STEER_FORK);
	angle[2] = 0.0f;
}


/*
 * ORIGIN  maybe_Racer_Draw @ 0x0045f290, at 0x0046030d..0x004604b0
 *
 * THE RANGE INCLUDES THE MATRIX BUILD, not just the angle: the
 * angle ends at 0x004603f2 and the inline cosine-table rotation
 * runs from there. Verified by hooking 0x004603b9 - the angle there
 * is racer+0x2e55 carac 27 plus the solved arm angle, -0.550000 +
 * 0.413850, which is what this function computes.
 *
 * THE ARM MATRIX IS A PURE X ROTATION, and proving that is what the
 * __ftol pattern is for. The original builds it inline from the
 * cosine table rather than calling FromEuler, and the listing is a
 * page of table lookups; substituting the two constants collapses
 * it:
 *
 *     g_cosTable[0]      = cos(0) = 1
 *     g_cosTable[0x3000] = 0
 *
 * and every term with a 0x3000 lookup in it vanishes, leaving
 *
 *     row0 = (1,    0,   0)
 *     row1 = (0,  cos, sin)
 *     row2 = (0, -sin, cos)
 *
 * SO THE INLINE BUILD IS AN OPTIMISED THREE-ANGLE EULER WITH TWO OF
 * THE ANGLES ZERO. The compiler could not know they were constant,
 * so it emitted the general form; there is no reason to reproduce a
 * general rotation here.
 *
 * THE ANGLE IS THE CARAC REST PLUS THE SOLVED ONE:
 *
 *     angle = FourcheAngleArriere + racerdraw_arm_angle(...)
 *
 * FourcheAngleArriere is carac 27, which the port already loads as
 * swingarmRest. It is NEGATIVE - about -31.5 degrees on bike 0 - so
 * the rest position is the arm already swung down.
 *
 * THE SOLVED ANGLE IS ALSO STASHED AT 0x00580a70 on the way past -
 * the dead store g_armAngle mirrors, above.
 */
void racerdraw_arm_matrix(float restAngle, float armAngle,
                          const float rearFrame[12], float out[12])
{
	double a;
	float  m[12];
	float  c, sn;
	int    i;

	if (!rearFrame || !out)
		return;

	/*
	 * THE TABLE, NOT libm. The original indexes g_cosTable the same
	 * way every other rotation in the draw does, and the quantisation
	 * is coarse enough to matter: one index is 3.8e-4 rad, which over
	 * a 70-unit swingarm is about 0.013 units of wheel position.
	 */
	a  = (double)restAngle + (double)armAngle;
	i  = (int)(a * TRIG_IDX_PER_RAD);
	c  = trig_cos(i);
	sn = trig_sin(i);

	m[0] = 1.0f;  m[1] = 0.0f;  m[2]  = 0.0f;
	m[3] = 0.0f;  m[4] = c;     m[5]  = sn;
	m[6] = 0.0f;  m[7] = -sn;   m[8]  = c;
	m[9] = 0.0f;  m[10] = 0.0f; m[11] = 0.0f;

	mat34_multiply(out, m, rearFrame);
}


/*
 * ORIGIN  maybe_Racer_Draw @ 0x0045f290, at 0x004604b0..0x00460da0
 *
 * IT STARTS AT THE MULTIPLY THAT FEEDS IT. 0x004604ba builds
 * +0x040 = +0x010 x +0x12c and 0x004604c8 then builds
 * +0x010 = +0x040 x camera, so the chain begins at 0x004604b0.
 *
 * THE REAR WHEEL, placed the same way the front one is: its own part
 * pivot taken through the arm matrix, then expressed as an OFFSET
 * from the stored contact because the instance already carries that
 * as its position.
 *
 *     pivotW = pose[slot*20 + part].pivot  through armMat
 *     offset = pivotW - rearWheelContact
 *
 * SO THE ARM CARRIES THE WHEEL. Nothing places the rear wheel
 * directly - it goes wherever the arm's tip lands, which is what
 * makes the swingarm angle visible at all.
 *
 * part IS NOT A CONSTANT. It comes out of the skeleton table at
 * unk_2d88[slot] + 0x10 - a different entry from the fork's, which
 * is at +8 - so the two ends of the bike index different members and
 * a port that hardcodes either is right only for one LOD.
 *
 * THE MESH VARIANT IS SWAPPED FIRST when the racer is shaded:
 * maybe_Mesh_SetPrimVariant on the rear wheel mesh with
 * (shadeLevel != 0x1f), which flips primitive kinds 0x10 <-> 0x15
 * and 0x26 <-> 0x27. 0x1f is the unshaded level, so a bike in a
 * shadowed segment draws its wheels through the other primitive set.
 */
void racerdraw_rear_wheel_offset(const MR_Racer *r, const float armMat[12],
                                 const float bindPivot[3], float out[3])
{
	float pivotW[3];
	int   i;

	if (!r || !armMat || !bindPivot || !out)
		return;

	rd_transform(pivotW, bindPivot, armMat);

	for (i = 0; i < 3; i++)
		out[i] = pivotW[i] - r->rearWheelContact[i];
}


/*
 * ORIGIN  maybe_Racer_Draw @ 0x0045f290, at 0x00461490..0x004619c0
 *
 * THE PART LOOP, and three things in it are easy to get wrong.
 *
 * FIRST, THE PART INDEX IS REMAPPED. The loop counter is not the
 * part: if the skeleton table at unk_2d88[slot] - 0x14 is non-null
 * it is an array and part = remap[i]. Only when it is null does the
 * counter stand in for the index.
 *
 * SECOND, TWO COUNTER VALUES ARE SPECIAL-CASED, and they are 0 and
 * 3 - the two subtree roots. Ghidra types the counter as a FLOAT and
 * prints the second test as "== 4.2039e-45", which is the bit
 * pattern of the integer 3 and not a small number. Reading it as a
 * float comparison would put the rider root in the general path.
 *
 *     i == 0   transform with the prebuilt VIEW matrix
 *     i == 3   its own branch at 0x004614cf - NOT an identity
 *     else     the general path below
 *
 * MEMBER 3 DOES NOT GET AN IDENTITY - it has its own composition at
 * 0x004614cf. The port's matches MOTO at both headings measured,
 * dmax 0.00003 and 0.00026; racer-draw-audit.txt 31.
 *
 * THIRD, PARTS 3..14 ARE THE RIDER and everything else is the bike:
 *
 *     part < 3 || part > 14   ->  the BIKE draw position
 *     otherwise               ->  the RIDER draw position
 *
 * which is the same split that lets a thrown rider keep his own
 * placement while the bike rides on. There is no flag anywhere - the
 * range IS the test.
 */
int racerdraw_part_is_rider(int part)
{
	return !(part < 3 || part > 14);
}


/*
 * ORIGIN  maybe_Racer_Draw @ 0x0045f290, at 0x004614f0..0x004619c0
 *
 * ONE PART'S VIEW MATRIX:
 *
 *     inv        = rigid inverse of pose[part].matrix
 *     inv.trans += drawPos - camera.eye
 *     out        = inv * camera.view
 *
 * THE INVERSE IS NOT A DETOUR. skl_transform_verts bakes with the
 * inverse, so the matrix handed to the transform has to be inverted
 * for the vertices to come out forward - our skl.c already carries
 * that note for the swingarm, and this is where it comes from.
 *
 * IT IS THE RIGID INVERSE - transpose of the 3x3, translation
 * -R^T * t - which is only correct if the part matrix is
 * orthonormal. The original assumes that everywhere, which is the
 * ONE piece of evidence pointing the other way on the [open] at
 * racerdraw_view_matrix: a rigid inverse taken unconditionally
 * says the engine keeps scale out, where racerdraw_arm_angle's
 * point-difference axes say it does not trust that. Both are the
 * original's, in one function.
 *
 * THE DRAW POSITION IS ADDED AFTER THE INVERSE, into the translation
 * row - not composed as a separate translation matrix. Same result,
 * and it is what the original writes.
 */
void racerdraw_part_matrix(const float partMat[12], const float drawPos[3],
                           const float camEye[3], const float camView[12],
                           float out[12])
{
	float inv[12];
	int   i;

	if (!partMat || !drawPos || !camEye || !camView || !out)
		return;

	mat34_invert_rigid(inv, partMat);

	for (i = 0; i < 3; i++)
		inv[9 + i] += drawPos[i] - camEye[i];

	mat34_multiply(out, inv, camView);
}


/*
 * ORIGIN  g_nShadeParts @ 0x004ea4d0
 *
 * THE GATE ON THE PER-VERTEX PASS, and it is OFF IN A RACE.
 * maybe_Racer_Draw tests it at 0x0046199a and skips
 * maybe_Racer_ShadeParts entirely when it is zero. Read live out of
 * the running game mid-race: zero. [verified]
 *
 * Its only writers are MenuPage10_BikeSelect_Enter (0x0044e4f9) and
 * _Leave (0x0044ed11), and MenuPage10_BikeSelect_Tick reads it - so
 * it belongs to the bike-select page, not to racing.
 *
 * DEFAULTING THIS TO 1 PUTS SHADING ON THE BIKE THAT THE ORIGINAL
 * DOES NOT DRAW. The pass is correct and its input is correct; it
 * simply must not run here.
 *
 * THE CALL SITE IS 0x004619c0..0x00461a20, and it is where the
 * eighteen stack arguments behind maybe_Racer_ShadeParts' RET 0x48
 * are pushed - see racerdraw_shade_parts for which of them are
 * actually read.
 *
 * 0x004ea4cc IS SET TO 0xff IMMEDIATELY AFTER, on BOTH paths out of
 * maybe_Racer_Draw - the shaded one and the far-LOD one - so it is
 * reset per racer rather than per frame. [open - what reads it]
 */
static int g_rdShadeParts;

/* ORIGIN  NONE - ours. Sets the mirror of g_nShadeParts above. */
void racerdraw_set_shade_parts(int on)
{
	g_rdShadeParts = (on != 0);
}

/* ORIGIN  NONE - ours. Reads it back. */
int racerdraw_shade_parts_enabled(void)
{
	return g_rdShadeParts;
}


/*
 * ORIGIN  maybe_Racer_Draw @ 0x0045f290, at 0x0046199a
 *
 * The gate itself: TEST EAX,EAX / JZ past the shade call. It takes
 * the flag rather than reading a global so the caller names which
 * one - pass g_rdShadeParts, never a literal. See section 45 of
 * docs/racer-draw-audit.txt for what a hardcoded 1 cost here.
 */
int racerdraw_shading_enabled(int globalFlag)
{
	return globalFlag != 0;
}


/*
 * ORIGIN  maybe_Mesh_SetPrimVariant @ 0x004221d0
 *
 * Swap a mesh's prim KINDS between a lit and an unlit variant, in
 * place. The whole original is this loop:
 *
 *     shaded ? (0x10 -> 0x15, 0x26 -> 0x27)
 *            : (0x15 -> 0x10, 0x27 -> 0x26)
 *
 * over prims[i] for i < this->primCount, stride 0x40. Nothing else
 * is touched - no vertices, no colours, only the kind field. Our
 * MR_Primitive is already 0x40 with kind at +0x00, so the walk is
 * the same one.
 *
 * WE PASS THE ARRAY, THE ORIGINAL PASSES A MESH. Its argument is a
 * 0x70-byte mesh record holding the prim pointer at +0x00 and the
 * count at +0x2c; the port keeps those two as separate fields on
 * MR_RacerDrawSet, so they are handed over directly. That is the
 * only divergence.
 *
 * IT MUTATES SHARED STATE, AND THAT IS THE ORIGINAL'S DESIGN. The
 * two meshes are per-LOD-SLOT globals - front from 0x0051ac58 and
 * rear from 0x0052c240, both indexed by skeletonSlot * 0x70 and
 * assigned at 0x0045f544 - so every racer in a slot shares one prim
 * array. Racer A on a shaded segment flips it and racer B off one
 * flips it back, and the last writer before the draw wins. Do not
 * "fix" this by making it per-racer.
 *
 * IT MATCHES NOTHING ON THE BIKE WE HAVE MEASURED. Both wheel
 * meshes carry 48 prims of kinds 1 and 2 - in MOTO and in the port,
 * identically - and none of 0x10, 0x15, 0x26 or 0x27. So the loop
 * runs and swaps zero prims. That is not a reason to leave it out:
 * the call is where the original's is, and it will fire the moment
 * a mesh turns up carrying a blended kind. It IS a reason not to
 * expect a visible change from it. [open - which bike or setting
 * produces kind 0x10 or 0x26 on a wheel]
 */
void racerdraw_set_prim_variant(MR_Primitive *prims, int count, int shaded)
{
	int i;

	if (!prims || count <= 0)
		return;

	for (i = 0; i < count; i++) {
		if (shaded) {
			if (prims[i].kind == 0x10)
				prims[i].kind = 0x15;
			else if (prims[i].kind == 0x26)
				prims[i].kind = 0x27;
		} else {
			if (prims[i].kind == 0x15)
				prims[i].kind = 0x10;
			else if (prims[i].kind == 0x27)
				prims[i].kind = 0x26;
		}
	}
}


/*
 * ORIGIN  maybe_Racer_Draw @ 0x0045f290, at 0x00461a20..0x00461a96
 *
 * THE QUEUE WALK, whole. It draws nothing today because faceCount
 * is 0 on every part - the one gap, tagged at
 * racerdraw_bind_skeleton - but the walk itself is complete, and
 * three things in it would be got wrong by a reimplementation.
 *
 * IT RUNS ONE PART FURTHER THAN THE TRANSFORM LOOP. That loop ends
 * at perSlotArrays[slot]; this one ends at perSlotArrays[slot] + 1.
 * So the last part's faces are queued from vertices nothing in this
 * function transformed - whatever the previous racer or the previous
 * frame left in the pool. Whether that is a defect in the original
 * or a part transformed somewhere else is NOT established.
 * Reproduce it; do not tidy it. [open]
 *
 * IT DOES NOT REMAP THE INDEX. The transform loop takes
 * part = remap[i] when the skeleton table has one; this indexes
 * pose[slot*20 + i] with the raw counter. On a skeleton whose remap
 * is not the identity the two loops walk different parts.
 *
 * EVERY PART QUEUES AGAINST ONE VERTEX BASE - the pointer
 * Screen3D_TransformVertsFlat returned for counter 0.
 * TransformVertsFlat appends into a shared pool and returns where
 * that batch began, so face indices are relative to the FIRST batch
 * and later parts' vertices follow it. Queuing each part against
 * its own returned base would look more correct and would be wrong.
 *
 * THE CULL MODE IS A FLAG AT unk_0350 + 0x2ac: negative selects
 * Screen3D_CullAndQueueFacesEx with 1, otherwise the plain call
 * takes the flag itself as its argument.
 *
 * partCount COMES FROM perSlotArrays[slot]. The caller passes it
 * rather than this reaching in, so the one unlaid-out field does
 * not spread.
 *
 * It is read through racerdraw_part_count, which carries the
 * [open] on typing perSlotArrays.
 */
void racerdraw_queue_parts(MR_Racer *r, int partCount, void *vertexBase,
                           int cullFlag, MR_RacerDrawQueue queue)
{
	int slot;
	int i;

	if (!r || !queue || partCount == -1)
		return;

	slot = r->skeletonSlot;
	if (slot < 0 || slot >= RACER_LOD_SLOTS)
		return;

	/*
	 * partCount + 1, AND THE RAW INDEX. Both are the original's and
	 * both disagree with the transform loop - see above. Do not
	 * quietly align them.
	 */
	for (i = 0; i < partCount + 1 && i < RACER_PARTS_PER_LOD; i++) {
		MR_RacerPart *part = &r->pose[slot][i];

		if (part->faceCount == 0)
			continue;

		if (cullFlag < 0)
			queue(vertexBase, part->faces, part->faceCount, 1, 1);
		else
			queue(vertexBase, part->faces, part->faceCount, cullFlag, 0);
	}
}


/*
 * ORIGIN  maybe_Racer_Draw @ 0x0045f290, at 0x00460da9..0x00460f00
 *
 * THE BRANCH BELOW IS THE ORIGINAL'S, not a reconstruction of it.
 * 0x00460da9 loads racer+0x30c1 and 0x00460db1 jumps to the second
 * store when it is set - that is crashLatch, asserted at +0x30c1 in
 * racer.h - and the store at 0x00460dbe is matRiderRoot at +0x3085.
 *
 * THE RIDER PLACEMENT LIVES IN THE DRAW, not in the tick.
 *
 * crashLatch CLEAR: matRiderRoot takes the bike's own draw matrix and
 * riderPosition its draw position, every frame. Neither is crash
 * state - they track the bike all race and are simply abandoned once
 * the rider is thrown. racer_begin_crash reads the yaw back out of
 * matRiderRoot, so a port that leaves it zeroed until the crash
 * starts throws every rider down the world +Z axis.
 *
 * crashLatch SET: matRiderRoot is left alone - it holds the tumble -
 * and the DRAWN position is riderPosition lifted in Y by 32 when the
 * bike has PersoAnim and 64 when it does not. That is not a fudge:
 * racer_update_crash pins riderPosition.y to the truncated ground
 * height, so the simulated value is the road surface and the model
 * has to be lifted onto it.
 *
 * MOTO LEAVES matRiderRoot's TRANSLATION AT ZERO - read live, all
 * three components. WRITING THE DRAW POSITION THERE makes
 * riderPosition and matRiderRoot disagree about what the body
 * origin is, and the fork and swingarm measurements that follow
 * read as a divergence when nothing is wrong.
 */
void racerdraw_place_rider(MR_Racer *r, const float bodyMat[12],
                           const float bodyPos[3], int persoAnim,
                           float drawOut[3])
{
	int i;

	if (!r || !drawOut)
		return;

	if (r->crashLatch == 0) {
		for (i = 0; i < 9; i++)
			r->matRiderRoot[i] = bodyMat[i];
		/*
		 * ROWS 9..11 STAY ZERO. See the note above - this is
		 * what the original does and it matters.
		 */
		r->matRiderRoot[9]  = 0.0f;
		r->matRiderRoot[10] = 0.0f;
		r->matRiderRoot[11] = 0.0f;

		for (i = 0; i < 3; i++) {
			r->riderPosition[i] = bodyPos[i];
			drawOut[i] = bodyPos[i];
		}
	} else {
		drawOut[0] = r->riderPosition[0];
		drawOut[1] = r->riderPosition[1]
		           + (persoAnim ? RD_RIDER_LIFT_ANIM : RD_RIDER_LIFT);
		drawOut[2] = r->riderPosition[2];
	}
}

/*
 * ORIGIN  maybe_Racer_ShadeLevel @ 0x00465d60
 *
 * The whole original is two lines:
 *
 *     if (g_nTrkSegmentCount == 0) return 0x1f;
 *     return (-(seg[racer->segmentStart].f_e0 == 0) & 0xf) + 0x10;
 *
 * which is 0x10 when the flag is set and 0x1f when it is clear - a
 * bike darkens under a bridge and brightens coming out.
 *
 * THE FLAG IS segment+0xe0, AND IT ARRIVES LATE. It is not in the
 * segment record; it is the LAST of four dwords in the second
 * per-segment section, the one formats/trk.txt calls the link
 * table. See the MR_TrkSegment comment in trk.h.
 *
 * IT READS racer+0x31c1 - segmentStart, the segment the race
 * assigned, not a per-frame current segment. That is the original's
 * choice and it is transcribed rather than corrected.
 */
int racerdraw_shade_level(const MR_Racer *r, const MR_Trk *t)
{
	if (!r || !t || t->segmentCount <= 0)
		return RD_SHADE_FULL;
	if (r->segmentStart < 0 || r->segmentStart >= t->segmentCount)
		return RD_SHADE_FULL;
	return t->segments[r->segmentStart].shaded ? RD_SHADE_DARK
	                                           : RD_SHADE_FULL;
}


/*
 * ORIGIN  NONE - ours
 *
 * THE ACCESSORS FOR perSlotArrays, and they exist so that one
 * unlaid-out field cannot spread through the file.
 *
 * [open] perSlotArrays IS unsigned char[60] at racer+0x2d4c and
 * holds TWO arrays, reached by memcpy at four sites in this file:
 *
 *     racer+0x2d4c   int    partCount[5]     at slot*4
 *     racer+0x2d60   void  *normals[5]       at (5 + slot)*4
 *
 * Typing it is a racer.h and a Ghidra struct change together, rule
 * 5b. This is the only statement of that gap.
 *
 * THE NORMAL ARRAY IS FILLED BY racerdraw_build_normals, which the
 * race runs once per racer per articulated slot. It is NULL before
 * that runs and on any slot the builder skipped, so every caller
 * must handle NULL.
 */
static int racerdraw_part_count(const MR_Racer *r, int slot)
{
	int n;

	if (!r || slot < 0 || slot >= RACER_LOD_SLOTS)
		return 0;
	memcpy(&n, &r->perSlotArrays[slot * 4], sizeof n);
	if (n < 0)
		n = 0;
	if (n > RACER_PARTS_PER_LOD)
		n = RACER_PARTS_PER_LOD;
	return n;
}


/*
 * ORIGIN  FUN_00462580 @ 0x00462580
 *
 * SMOOTH VERTEX NORMALS FOR ONE LOD SLOT, built once per racer.
 * Racer_Create calls it twice and nothing else calls it at all -
 * 0x0045c35d and 0x0045c4dc, one per articulated slot.
 *
 * The original, in order:
 *
 *   free racer->normals[slot] if set
 *   normals = new float[n*3], counts = new short[n], both zeroed
 *   for each part, for each of its faces:
 *       e1 = v[i1] - v[i0]; e2 = v[i2] - v[i0]
 *       f  = (e1.z*e2.y - e1.y*e2.z,
 *             e1.x*e2.z - e1.z*e2.x,
 *             e1.y*e2.x - e1.x*e2.y)
 *       if (|f| != 0) f /= |f|
 *       normals[i0] += f; normals[i1] += f; normals[i2] += f
 *       counts[i0]++;     counts[i1]++;     counts[i2]++
 *   for each vertex: n /= count; if (|n| != 0) n /= |n|
 *   free counts
 *
 * THE CROSS PRODUCT IS REVERSED. Read literally it is cross(e2,e1),
 * the negation of the usual cross(e1,e2). Transcribed as it stands -
 * flipping it would invert every normal and the sign convention has
 * to match whatever maybe_Racer_ShadeParts expects.
 *
 * THE 1.0 IS A DOUBLE. 0x004d0ec8 holds 00 00 00 00 00 00 f0 3f,
 * which is 1.0 as a double, not a float zero - reading four bytes
 * there says 0 and makes the whole function look like it zeroes its
 * output. The zero it compares lengths against is 0x004d0e54.
 *
 * DIVISION BY THE COUNT IS UNGUARDED in the original: a vertex in no
 * face has count 0 and produces inf or nan, which the length test
 * then does not rescue. Ours skips those vertices and leaves them
 * zero. That is a DELIBERATE DIVERGENCE - the alternative is
 * propagating nan into the shade and out to the rasterizer.
 *
 * WE WALK THE SKELETON, NOT THE PART RECORDS. The original reads
 * each part's face list at +0x698/+0x69c, in MR_Primitive form. Our
 * skeleton keeps the same slices as MR_SklMember.firstFace and
 * faceCount over mesh.faces, which is MESH_FACE - the loader form,
 * int indices. Same faces, same order, different record.
 *
 * THE VERTICES ARE skl->bind, NOT THE POSED COPY, and that is not
 * interchangeable. These are built ONCE per racer at create time,
 * and racerdraw_shade_parts carries the light INTO each part's
 * local frame by the transpose of that part's 3x3 - so the normals
 * it meets there have to be in bind space. Building them from the
 * posed vertices would rotate them twice.
 */
void racerdraw_build_normals(MR_Racer *r, const MR_Skeleton *skl, int slot)
{
	const MESH_VERTEX *v;
	float             *normals;
	short             *counts;
	int                n, i, j;

	if (!r || !skl || slot < 0 || slot >= RACER_LOD_SLOTS)
		return;

	n = skl->mesh.vertexCount;
	v = skl->bind;
	if (n <= 0 || !v || !skl->mesh.faces)
		return;

	racerdraw_free_normals(r, slot);

	normals = (float *)calloc((size_t)n * 3, sizeof(float));
	counts  = (short *)calloc((size_t)n, sizeof(short));
	if (!normals || !counts) {
		free(normals);
		free(counts);
		return;
	}

	for (i = 0; i < skl->memberCount && i < SKL_MAX_MEMBERS; i++) {
		const MR_SklMember *mb = &skl->member[i];

		for (j = 0; j < mb->faceCount; j++) {
			const MESH_FACE *f = &skl->mesh.faces[mb->firstFace + j];
			int   i0, i1, i2, k;
			float e1[3], e2[3], nv[3], len;

			if (f->count < 3)
				continue;
			i0 = f->index[0];
			i1 = f->index[1];
			i2 = f->index[2];
			if (i0 < 0 || i0 >= n || i1 < 0 || i1 >= n ||
			    i2 < 0 || i2 >= n)
			    continue;

			e1[0] = v[i1].x - v[i0].x;
			e1[1] = v[i1].y - v[i0].y;
			e1[2] = v[i1].z - v[i0].z;
			e2[0] = v[i2].x - v[i0].x;
			e2[1] = v[i2].y - v[i0].y;
			e2[2] = v[i2].z - v[i0].z;

			/* cross(e2, e1) - see the note above. */
			nv[0] = e1[2] * e2[1] - e1[1] * e2[2];
			nv[1] = e1[0] * e2[2] - e1[2] * e2[0];
			nv[2] = e1[1] * e2[0] - e1[0] * e2[1];

			len = (float)sqrt(nv[0] * nv[0] + nv[1] * nv[1]
			                + nv[2] * nv[2]);
			if (len != 0.0f) {
				float inv = 1.0f / len;

				nv[0] *= inv;
				nv[1] *= inv;
				nv[2] *= inv;
			}

			for (k = 0; k < 3; k++) {
				normals[i0 * 3 + k] += nv[k];
				normals[i1 * 3 + k] += nv[k];
				normals[i2 * 3 + k] += nv[k];
			}
			counts[i0]++;
			counts[i1]++;
			counts[i2]++;
		}
	}

	for (i = 0; i < n; i++) {
		float *p = normals + i * 3;
		float  len;

		/* The original has no guard here - see above. */
		if (counts[i] <= 0)
			continue;
		p[0] /= (float)counts[i];
		p[1] /= (float)counts[i];
		p[2] /= (float)counts[i];

		len = (float)sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
		if (len != 0.0f) {
			float inv = 1.0f / len;

			p[0] *= inv;
			p[1] *= inv;
			p[2] *= inv;
		}
	}

	free(counts);
	memcpy(&r->perSlotArrays[(5 + slot) * 4], &normals, sizeof normals);
}


/*
 * ORIGIN  FUN_00462580 @ 0x00462580, at 0x004625a4
 *
 * The head of the builder: free whatever the slot already holds.
 * Split out because Racer_Create runs the builder twice and the port
 * needs the same release on teardown.
 */
void racerdraw_free_normals(MR_Racer *r, int slot)
{
	float *p;

	if (!r || slot < 0 || slot >= RACER_LOD_SLOTS)
		return;
	memcpy(&p, &r->perSlotArrays[(5 + slot) * 4], sizeof p);
	free(p);
	p = NULL;
	memcpy(&r->perSlotArrays[(5 + slot) * 4], &p, sizeof p);
}


/*
 * ORIGIN  NONE - ours
 *
 * The per-LOD vertex normals, which maybe_Racer_ShadeParts reads at
 * 0x00462b7d. racerdraw_build_normals fills them; NULL means it has
 * not run for this slot. See racerdraw_part_count for the layout.
 */
static const float *racerdraw_slot_normals(const MR_Racer *r, int slot)
{
	/* NOT const: &p must be a plain void* for memcpy. */
	float *p;

	if (!r || slot < 0 || slot >= RACER_LOD_SLOTS)
		return NULL;
	/* +0x2d60 is 20 bytes past +0x2d4c, so index 5 + slot. */
	memcpy(&p, &r->perSlotArrays[(5 + slot) * 4], sizeof p);
	return p;
}


/*
 * ORIGIN  maybe_Racer_ShadeParts @ 0x00462a40
 *
 * The per-vertex shading pass: it lights every part's vertices by
 * NORMAL against a world light direction, and writes the result
 * into the output vertex array shifted left 13.
 *
 * IT HAS NOTHING TO DO WITH THE SEGMENT SHADE FLAG, and the two
 * are easy to conflate because both end up as an intensity. That
 * one is the shade LEVEL, computed by maybe_Racer_ShadeLevel and
 * pushed into each part's transform at 0x0046147c - see
 * racerdraw_racer. THIS FUNCTION TAKES NO shadeLevel ARGUMENT and
 * the original has none.
 *
 * THE SIGNATURE IS 18 STACK DWORDS AND IT USES FOUR OF THEM.
 * The caller at 0x004619e8 pushes, in order: one dword, the light
 * direction from g_flLightDir (0x0052c228..0x0052c230), the camera
 * 3x4 BY VALUE (48 bytes, copied onto the stack at 0x004619c5),
 * the constant 1.0f, and the destination vertex array. Only the
 * light and the destination are read. The matrix and the 1.0f are
 * passed and ignored - verified by reading every operand in the
 * body, which touches [ESP+0x30], [ESP+0x34], [ESP+0x38] and
 * [ESP+0x70] and nothing else above the frame.
 *
 * So the port takes the three arguments that matter.
 *
 * THE LIGHT IS NORMALISED IN PLACE, and only when its length is
 * not exactly 0.0 (FCOM against 0x004d0e54, which is 0.0f). A zero
 * light is left alone rather than producing a divide.
 *
 * THE PER-PART TRANSFORM IS BY THE TRANSPOSE. Each part's 3x3 is
 * applied as m[0],m[3],m[6] down the first result, which is M^T
 * times L - the light carried into the part's local frame.
 *
 * NO LOWER CLAMP. The original clamps only the top: CMP EAX,0x20 /
 * JL keep / MOV EAX,0x1f. A negative intensity is shifted left 13
 * as a negative int, and that is transcribed rather than fixed.
 *
 * ITS INPUT IS racerdraw_build_normals, the port's FUN_00462580,
 * which race.c runs for both articulated slots of every racer at
 * load. This returns early on a slot the builder skipped.
 */
void racerdraw_shade_parts(MR_Racer *r, const float light[3],
                           MR_Vertex *verts)
{
	float L[3];
	float len;
	int   slot, i, j;
	int   partCount;
	const float *normals;

	if (!r || !light || !verts)
		return;

	slot = r->skeletonSlot;
	if (slot < 0 || slot >= RACER_LOD_SLOTS)
		return;

	/* 0x00462a40..0x00462a98 - normalise, unless the length is 0. */
	L[0] = light[0];
	L[1] = light[1];
	L[2] = light[2];
	len = (float)sqrt(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
	if (len != 0.0f) {
		float inv = 1.0f / len;

		L[0] *= inv;
		L[1] *= inv;
		L[2] *= inv;
	}

	partCount = racerdraw_part_count(r, slot);
	normals   = racerdraw_slot_normals(r, slot);
	if (!normals)
		return;

	/* 0x00462a9c..0x00462c13 - the two loops. */
	for (i = 0; i < partCount; i++) {
		const MR_RacerPart *part;
		const float        *m;
		float               a, b, c;
		int                 first, count;

		if (i >= RACER_PARTS_PER_LOD)
			break;
		part  = &r->pose[slot][i];
		m     = part->matrix;
		first = part->firstVertex;
		count = part->vertexCount;

		/* 0x00462ad2..0x00462b38 - the light through M^T. */
		a = m[0] * L[0] + m[3] * L[1] + m[6] * L[2];
		b = m[1] * L[0] + m[4] * L[1] + m[7] * L[2];
		c = m[2] * L[0] + m[5] * L[1] + m[8] * L[2];

		if (count <= 0)
			continue;

		/* 0x00462b42..0x00462b5e - negated once, outside the loop. */
		a = -a;
		b = -b;
		c = -c;

		for (j = 0; j < count; j++) {
			int          idx = first + j;
			const float *n   = normals + idx * 3;
			float        d;
			int          v;

			d = n[0] * a + n[1] * b + n[2] * c;
			v = (int)(RD_SHADE_BIAS - d * RD_SHADE_SCALE);
			if (v >= 0x20)
				v = 0x1f;
			verts[idx].shade = v << 13;
		}
	}
}


/*
 * THE FIRST-PERSON COCKPIT'S CONSTANTS, read out of 0x00462110.
 *
 *   0x004d1068   2/pi     wheelieAngle scale            (double)
 *   0x004d1070  -0.25     chassis attitude scale
 *   0x004d0f88   0.25     the attitude filter rate
 *   0x004d1074 -80.0      the wheelie term, up axis
 *   0x004d1078  10.0      springBounce, up axis         (double)
 *   0x004d1080 350.0      the up-axis offset, SUBTRACTED (double)
 *   0x004d0f78   5.0      springBounce, forward axis
 *   0x004d1088 350.0      the forward-axis offset
 *   0x004d1090   0.4      the share of the lean it rolls (double)
 *   0x004d1098   1/3500   steering to radians
 *
 * ALL TEN VERIFIED BY read_memory, VALUE AND WIDTH, and the
 * literals below match. 10.0, 350.0 and -350.0 are doubles in the
 * image but exact either way; 1/3500 really is a float, and
 * 1.0f/3500.0f reproduces its bits. The two that are neither -
 * 2/pi and 0.4 - are spelled as doubles, 2/pi as the expression.
 */
#define COCKPIT_WHEELIE_K    (2.0 / MR_PI)          /* 2/pi           */
#define COCKPIT_ATTITUDE_K  -0.25f
#define COCKPIT_FILTER_K     0.25f
#define COCKPIT_WHEELIE_UP -80.0f
#define COCKPIT_BOUNCE_UP   10.0f
#define COCKPIT_UP        -350.0f
#define COCKPIT_BOUNCE_FWD   5.0f
#define COCKPIT_FWD        350.0f
#define COCKPIT_ROLL_K       0.4
#define COCKPIT_STEER_K      (1.0f / 3500.0f)

/* 0x004ea4e4, a global in the original too - one cockpit, one filter. */
static float s_cockpitAttitude;

/* ORIGIN  NONE - ours. The original never clears it; a race that ends
 * and restarts inherits the last frame's pitch, which is invisible
 * because the filter converges in a handful of frames. Cleared here
 * anyway so a restart is deterministic. */
void racerdraw_cockpit_reset(void)
{
	s_cockpitAttitude = 0.0f;
}

/* ORIGIN  FUN_00462110 @ 0x00462110, at 0x004622a2 and 0x00462536 */
static int rd_cockpit_mesh(const float m[12], const MR_CockpitPart *part)
{
	MR_Vertex *v;

	if (!part->mesh || !part->prims || part->primCount <= 0 ||
	    part->mesh->vertexCount <= 0)
	    return 0;

	v = transform_verts_flat(m, (const float *)part->mesh->vertices,
	                         part->mesh->vertexCount, 0x1f);
	if (!v)
		return 0;
	return cull_and_queue_translucent(v, part->prims, part->primCount);
}

/*
 * ORIGIN  FUN_00462110 @ 0x00462110
 *
 * THE FIRST-PERSON COCKPIT - the handlebars and the dash, drawn in
 * place of the bike when the racer being ridden is also the one being
 * viewed in mode 6. maybe_Race_DrawRacers picks it over
 * maybe_Racer_Draw at 0x00458bdf; race_draw_racers does the same.
 *
 * IT IS A PURE CAMERA-SPACE PLACEMENT, and that is the thing to
 * understand before reading the maths: no view matrix, no world
 * position, nothing from the track. The eye is already on the bike, so
 * the bars only ever need a fixed offset in front of it - the same
 * shape as glass.c's pane. Every matrix below comes from bike STATE.
 *
 *     attitude += (chassis_attitude(r) * -0.25 - attitude) * 0.25
 *     Mpitch    = euler(attitude, 0, 0)
 *     Mroll     = euler(0, 0, -lean * 0.4)
 *     up        = bounce*10 + wheelie*(2/pi)*-80 - 350
 *     fwd       = bounce*5  + 350
 *     P         = Mpitch * (0, up, fwd)
 *
 *     bars = Mpitch * Mroll * eulerPos((0, steering/3500, 0), P)
 *     dash = Mpitch * Mroll * eulerPos((0, 0, 0),            P)
 *
 * so the two differ in the steering yaw and nothing else. THE
 * TRANSLATION SURVIVES THE COMPOSITION UNCHANGED - Mpitch and Mroll
 * both have a zero translation, so the product's is exactly P.
 *
 * THE FILTER IS WHY THE DASH LAGS THE CHASSIS. A quarter per frame, so
 * over a jump face the cockpit pitches behind the bike rather than
 * with it.
 *
 * TWO THINGS HERE ARE THE COMPILER AND NOT THE DESIGN, and both look
 * like findings if read as written: the dash's euler is FOLDED TO
 * CONSTANTS at 0x0046247f - the cos-table reads are g_cosTable[0] and
 * g_cosTable[0x3000] taken as bare absolute addresses, 1 and 0, so the
 * rotation is the identity - and P IS RECOMPUTED INLINE at 0x00462300
 * instead of being kept, because the compiler wanted its three
 * components on the FPU stack to store as the dash's translation.
 *
 * NOT DRAWN AT ALL with the rear-view key held, in a replay, or on the
 * pocket bike. That gate is at 0x0046213e and the caller owns it.
 */
int racerdraw_cockpit(const MR_Racer *r, float wheelieAngle,
                      const MR_CockpitSet *set)
{
	float Mpitch[12], Mroll[12], Mlocal[12], T[12], M[12];
	float euler[3], local[3], P[3];
	float up, fwd, target;
	int   queued = 0;
	int   i;

	if (!r || !set)
		return 0;

	/* 0x00462170. The only frame state this function keeps. */
	target = racer_chassis_attitude(r) * COCKPIT_ATTITUDE_K;
	s_cockpitAttitude += (target - s_cockpitAttitude) * COCKPIT_FILTER_K;

	euler[0] = s_cockpitAttitude;
	euler[1] = 0.0f;
	euler[2] = 0.0f;
	mat34_from_euler(Mpitch, euler);        /* nine elements only */
	Mpitch[9] = Mpitch[10] = Mpitch[11] = 0.0f;

	/* 0x004621b8 - the anchor, in the pitched frame. */
	up  = (float)((double)r->springBounce * COCKPIT_BOUNCE_UP
	              + (double)wheelieAngle * COCKPIT_WHEELIE_K
	                * COCKPIT_WHEELIE_UP
	              + COCKPIT_UP);
	fwd = r->springBounce * COCKPIT_BOUNCE_FWD + COCKPIT_FWD;

	local[0] = 0.0f;
	local[1] = up;
	local[2] = fwd;
	rd_transform(P, local, Mpitch);

	/* 0x0046221f - NEGATED, and that is the whole reason the lean
	 * reads on screen: the view rolls one way and the bars the other. */
	euler[0] = 0.0f;
	euler[1] = 0.0f;
	euler[2] = (float)(-(double)r->lean * COCKPIT_ROLL_K);
	mat34_from_euler(Mroll, euler);
	Mroll[9] = Mroll[10] = Mroll[11] = 0.0f;

	mat34_multiply(T, Mpitch, Mroll);

	/* THE HANDLEBARS, 0x00462267. racer+0x110 is in units of pi/4096
	 * and saturates at +-600, so the bars swing +-9.8 degrees. */
	euler[0] = 0.0f;
	euler[1] = r->steering * COCKPIT_STEER_K;
	euler[2] = 0.0f;
	mat34_from_euler(Mlocal, euler);
	for (i = 0; i < 3; i++)
		Mlocal[9 + i] = P[i];
	mat34_multiply(M, T, Mlocal);
	queued += rd_cockpit_mesh(M, &set->part[COCKPIT_PART_GUIDON]);

	/* THE DASH, 0x004624fc. Identity rotation, the same anchor. */
	for (i = 0; i < 9; i++)
		Mlocal[i] = (i % 4 == 0) ? 1.0f : 0.0f;
	for (i = 0; i < 3; i++)
		Mlocal[9 + i] = P[i];
	mat34_multiply(M, T, Mlocal);
	queued += rd_cockpit_mesh(M, &set->part[COCKPIT_PART_DASH]);

	return queued;
}


/*
 * ORIGIN  maybe_Inst_DrawWithMatrix @ 0x0048c6d0
 *
 * THERE ARE TWO OF THESE AND THIS IS THE RACE ONE. Each wheel site
 * picks between 0x0048c6d0 and maybe_Inst_DrawWithMatrixNoFx at
 * 0x0048c400 on g_nRacerDrawFx (0x004dc130) - front at 0x0045fcb6,
 * rear at 0x00460cc6. The flag reads 1 in a race, so the Fx helper
 * is the one the race takes; it is cleared only for the bike-select
 * turntable and the podium. The NoFx path is therefore menu-only and
 * its absence here is not a hole in the draw.
 *
 * BOTH BRANCHES COMPUTE THE SAME OFFSET before the call - the draw
 * position minus the instance's own position - so they differ in
 * effects, not in placement. That is why the wheel positions verify
 * against MOTO even though only one helper is implemented.
 *
 * THE Fx BRANCH ALSO SWAPS THE PRIM VARIANT before the draw -
 * racerdraw_racer does both wheels, and racerdraw_set_prim_variant
 * is the transcription. The shade LEVEL is what darkens the
 * transform and this is the second, finer half of the same effect.
 *
 * ONE WHEEL INSTANCE. The original computes
 *
 *     world = inst.position + offset - camera.eye
 *
 * and composes only the passed matrix 3x3 with the camera view.
 *
 * THE INSTANCE POSITION ALIASES THE CONTACT POINT - the front
 * instance is racer+0x1a8 and 0x1a8+0x0c is 0x1b4, frontWheelContact;
 * the rear is racer+0x32c and 0x32c+0x0c is 0x338, rearWheelContact.
 * So the offsets racerdraw_front_wheel_offset and
 * racerdraw_rear_wheel_offset produce are WORLD deltas added to the
 * contacts.
 *
 * THEY ARE NOT ROTATED. bike_draw turned its wheel offset through the
 * bike orientation before adding it, because its offsets were in bike
 * space. These are not, and rotating them here would place both
 * wheels wrong the moment the bike leans.
 */
static int rd_draw_instance(const MESH *mesh, MR_Primitive *prims,
                            int primCount, const float orient[12],
                            const float worldPos[3], const float camEye[3],
                            const float camView[12], int shade,
                            int livery)
{
	float      local[12];
	float      composed[12];
	MR_Vertex *v;
	int        i;

	if (!mesh || !prims || primCount <= 0 || mesh->vertexCount <= 0)
		return 0;

	for (i = 0; i < 9; i++)
		local[i] = orient[i];
	for (i = 0; i < 3; i++)
		local[9 + i] = worldPos[i] - camEye[i];

	mat34_multiply(composed, local, camView);

	v = transform_verts_flat(composed, (const float *)mesh->vertices,
	                         mesh->vertexCount, shade);
	if (!v)
		return 0;

	/*
	 * AND THEN IT OVERWRITES THE SHADE IT JUST PASSED IN.
	 * maybe_Inst_DrawWithMatrix transforms with a literal 0x1f at
	 * 0x0048c94d and calls maybe_Mesh_ShadeVertices at 0x0048c9b2,
	 * so the flat level only survives on a mesh with no vertex
	 * normals - which is every mesh but the four wheels.
	 *
	 * *** orient, NOT camView - AND THE TYRE IS HOW YOU SEE IT ***
	 *
	 * orient carries frontWheelSpin, so passing it here takes the
	 * light into the wheel's own frame and the shading stays put in
	 * the world while the wheel turns. Pass the camera instead and
	 * the bands are painted in MESH space and rotate WITH the tyre -
	 * a pale stripe round the centre tread that revolves, obvious
	 * from behind and invisible from the side.
	 */
	mesh_shade_vertices(mesh, g_rdLightDir, orient, v);

	return cull_and_queue_faces(v, prims, primCount, livery);
}


/*
 * ORIGIN  maybe_Racer_Draw @ 0x0045f290..0x00461a96
 *
 * THE WHOLE PER-RACER DRAW, and the reason every other function in
 * this file exists. The blocks above are its pieces in its order;
 * this is the function that runs them.
 *
 * IT OWNS THE TRANSFORM AND THE FACE QUEUE, which is the whole point.
 * A layer that only builds matrices and hands them on places every
 * part a second time, in the wrong space; that mismatch is set out
 * in docs/subsystems/racer-draw-pipeline.txt 3.
 *
 * THE MATRIX SPACES, stated once so no reader has to re-derive
 * them:
 *
 *   bodyMat/rearMat/frontMat  WORLD orientation at the origin. The
 *                             original builds all three in one stack
 *                             slot in turn, so copies are kept.
 *   pose[].matrix             ALREADY PLACED - the walk takes bodyMat
 *                             as its root, not the identity.
 *   armW                      WORLD. Places the rear wheel.
 *   armC                      CAMERA-RELATIVE. Draws the swingarm.
 *   viewPart0/viewPart3       fully composed to camera, UNINVERTED.
 *
 * THE TWO SPECIAL CASES ARE COUNTERS 0 AND 3, NOT 0 AND "the rider".
 * Both hand a prebuilt matrix straight to the transform with no
 * inverse - CMP EDI,0x3 at 0x004614cf, and the matrix pointers at
 * ESP+0x3cc and ESP+0x240:
 *
 *   counter 0 -> member 0 -> part 0,  corps     viewPart0
 *   counter 3 -> member 3 -> part 2,  SWINGARM  viewPart3
 *
 * because the remap is member 0,1,2,3,4 -> part 0,1,15,2,16
 * [verified, racers.txt 9.59.4]. THE RIDER IS NOT A SPECIAL CASE. He
 * enters through the second walk root and through a TRANSLATION
 * substitution in the general path - riderPos instead of drawPos, the
 * same orientation machinery as every other part.
 *
 * THE VERTEX BASE IS THE COUNTER-0 RETURN. TransformVertsFlat appends
 * into a shared pool, so face indices are relative to the FIRST slice.
 * The "- first" is OURS and is a no-op on shipped data, where part 0
 * begins at vertex 0; the original keeps the pointer raw.
 */
int racerdraw_racer(MR_Racer *r, const MR_RacerDrawSet *set,
                    const float camView[12], const float camEye[3],
                    int lod, int *budgetLeft, const MR_Trk *trk)
{
	const MR_BikeCarac *c;
	const MR_Skeleton  *skl;
	MR_RacerPart       *pose;
	MR_Vertex          *base = NULL;
	int                 shade;
	float rollMat[12], rearMat[12], frontMat[12], bodyMat[12];
	float frontWheelMat[12];
	float mid[3], rearOff[3], frontOff[3], drawPos[3], riderPos[3];
	float viewPart0[12], viewPart3[12];
	float frontFrame[12], rearFrameW[12], rearFrameC[12];
	float armW[12], armC[12];
	float pivotW[3], wheelPoint[3], scaledTip[3];
	float frontWheelOff[3], rearWheelOff[3];
	float camRel[3];
	float steerF, armAngle;
	float posedLen = 0.0f, reach = 0.0f;
	int   slot, persoAnim, i, queued = 0;

	if (!r || !set || !camView || !camEye)
		return 0;

	skl = set->skl;
	if (!skl || !skl->bind || !skl->runsPacked)
		return 0;

	c = bike_at(r->bikeIndex);
	if (!c)
		return 0;
	persoAnim = c->persoAnim;

	/*
	 * THE LOD ARGUMENT, 0x0045f2c5. The race passes -1 and the store
	 * is skipped, so the slot is whatever racerdraw_select_lod left
	 * during the tick. A caller that passes a real slot also replaces
	 * matBikeRoot with matOrientation, because a racer that has never
	 * ticked has no bike root.
	 */
	if (lod != -1) {
		r->skeletonSlot = lod;
		memcpy(r->matBikeRoot, r->matOrientation, 12 * sizeof(float));
	}

	slot = r->skeletonSlot;
	if (slot < 0 || slot >= RACER_LOD_SLOTS)
		return 0;
	pose = &r->pose[slot][0];

	/*
	 * NO BIKE ROOT YET MEANS NO PLACEMENT. Before the first tick the
	 * matrix is all zeros and every position built from it collapses
	 * onto the origin.
	 */
	if (r->matBikeRoot[0] == 0.0f && r->matBikeRoot[2] == 0.0f)
		return 0;

	if (slot >= 2) {
		/*
		 * THE FAR LOD: one mesh, no skeleton, no wheels, no rider.
		 * It is placed from the REAR CONTACT rather than the
		 * wheelbase midpoint, so the two LODs genuinely disagree
		 * about where the bike is.
		 *
		 * THE ORIGINAL SPLITS HERE AND THE TWO ARMS DIFFER IN ONE
		 * THING ONLY - the shade handed to the transform:
		 *
		 *   0x0045f43a  shaded - transform with the racer's shade
		 *               LEVEL, then maybe_Mesh_ShadeVertices
		 *               (0x00422540), then join at 0x0045f4eb
		 *   0x0045f4b5  unshaded - transform with a literal 0x1f
		 *
		 * selected at 0x0045f40f on g_nRacerDrawFx and 0x0045f417
		 * on the level being non-zero. Both hold in a race, so the
		 * SHADED arm is the one a race takes, and that is what
		 * this does.
		 *
		 * maybe_Mesh_ShadeVertices IS A NO-OP HERE, and calling it
		 * would be one too: it returns at once unless the mesh has
		 * vertexNormals, and the only meshes that ever get them are
		 * the four WHEEL meshes - see MR_Mesh.txt 5. g_meshMotoMed,
		 * Low and VeryLow have none, so the per-vertex pass never
		 * runs on a far LOD in MOTO either. It IS transcribed, as
		 * mesh_shade_vertices, because rd_draw_instance needs it.
		 */
		float      far[12];
		MR_Vertex *v;

		if (!set->lodMesh[slot] || !set->lodPrims[slot])
			return 0;

		shade = racerdraw_far_shade(1, racerdraw_shade_level(r, trk));
		racerdraw_far_lod(r, c, camEye, camView, far);
		memcpy(g_rdFarLod, far, sizeof g_rdFarLod);
		/* keyed by position - racerdraw_racer has no racer index */
		memcpy(g_rdFarLodPos, r->position, sizeof g_rdFarLodPos);
		v = transform_verts_flat(far,
		        (const float *)set->lodMesh[slot]->vertices,
		        set->lodMesh[slot]->vertexCount, shade);
		if (!v)
			return 0;
		/*
		 * THE FAR LOD CULLS ON livery (+0x170), NOT liveryRider
		 * (+0x5fc) - the near path uses the other one. MOTO loads
		 * racer+0x170 at 0x0045f4f8 and pushes it at 0x0045f501 as
		 * Screen3D_CullAndQueueFaces' fourth argument.
		 *
		 * race_load writes both fields from the same value, so the
		 * two agree in a race; they diverge only where something
		 * rewrites one of them, as the bike-select page does.
		 */
		queued = cull_and_queue_faces(v, set->lodPrims[slot],
		                              set->lodPrimCount[slot],
		                              r->livery);
		if (budgetLeft)
			*budgetLeft -= queued;
		return queued;
	}

	/* --- the placement chain, 0x0045f6a0..0x0045f9d0 --- */

	steerF = racerdraw_steer_factor(c, r->speed, persoAnim);
	racerdraw_build_matrices(r, steerF, rollMat, rearMat, frontMat,
	                         bodyMat, frontWheelMat);
	racerdraw_rebuild_contacts(r, c, mid);
	racerdraw_offsets(r, c, rollMat, bodyMat, rearOff, frontOff);
	racerdraw_draw_position(mid, rearOff, frontOff, drawPos);

	racerdraw_view_matrix(bodyMat, drawPos, camEye, camView, viewPart0);

	for (i = 0; i < 3; i++)
		camRel[i] = drawPos[i] - camEye[i];

	/* --- the fork and the front wheel, 0x0045fa30..0x0045fe10 --- */

	/*
	 * FROM rollMat, NOT frontMat - THE STEER MUST NOT BE IN HERE.
	 * MOTO builds this frame at 0x0045fb0f from body +0x298, which is
	 * roll x root: no steer, no spin. The frame then transforms the
	 * fork pivot at 0x0045fb49, so the PIVOT DOES NOT SWING when the
	 * bars turn - only the fork AXIS does, through steerM inside
	 * racerdraw_fork_solve.
	 *
	 * PASSING frontMat APPLIES THE STEER TWICE. Measured at a forced
	 * steering of 600: MOTO moves the wheel 16.15 in x, frontMat
	 * gives 30.78 - a factor of 1.91, the pivot and the axis each
	 * turning when only one should. It also drags Y off its fixed
	 * 30.576, because a swinging pivot changes reach, and reach
	 * scales an axis that points down the fork.
	 */
	racerdraw_rear_frame(rollMat, drawPos, frontFrame);
	racerdraw_fork_solve(r, c, steerF, frontFrame, bodyMat,
	                     pose[RD_PART_FORK].pivot,
	                     pose[RD_PART_WHEEL_FRONT].pivot,
	                     pivotW, wheelPoint, &posedLen, &reach);
	racerdraw_front_wheel_offset(r, wheelPoint, frontWheelOff);
	racerdraw_fork_tip(pose[RD_PART_FORK].pivot,
	                   pose[RD_PART_WHEEL_FRONT].pivot,
	                   posedLen, reach, scaledTip);
	racerdraw_steer_pose(r, steerF);

	/* --- the swingarm and the rear wheel, 0x0045fe10..0x004604c0 --- */

	/*
	 * THE SPIN-FREE MATRIX FEEDS THE FRAME, THE ARM AND THE SWINGARM.
	 * rollMat is MOTO's +0x298, roll x root; rearMat is +0x268, which
	 * is rearWheelSpin composed onto it and is ONLY the wheel mesh's
	 * attitude - the copy at +0x334 that reaches
	 * maybe_Inst_DrawWithMatrix.
	 *
	 * rollMat AND rearMat ARE EQUAL WHENEVER THE WHEEL IS NOT
	 * TURNING, because the only thing between them is rearWheelSpin - the
	 * rear wheel's spin angle. So passing rearMat here verifies
	 * CLEAN against a stationary bike and turns the swingarm and the
	 * whole rear frame the moment the wheel starts spinning.
	 *
	 * A COMPARISON AGAINST A PARKED BIKE CANNOT SEE THIS. It is the
	 * same shape as the identity-rotation trap at
	 * skl_build_part_matrices_rec: two candidate inputs that agree
	 * in the state easiest to capture.
	 */
	racerdraw_rear_frame(rollMat, drawPos, rearFrameW);
	armAngle = racerdraw_arm_angle(r, rearFrameW,
	                               pose[RD_PART_ARM_PIVOT].pivot);
	racerdraw_arm_matrix(c->swingarmRest, armAngle, rearFrameW, armW);

	memcpy(g_rdRearMat,    rearMat,    sizeof g_rdRearMat);
	memcpy(g_rdFrontWheelMat, frontWheelMat,
	       sizeof g_rdFrontWheelMat);
	memcpy(g_rdRearFrameW, rearFrameW, sizeof g_rdRearFrameW);
	memcpy(g_rdArmW,       armW,       sizeof g_rdArmW);
	racerdraw_rear_wheel_offset(r, armW, pose[RD_PART_WHEEL_REAR].pivot,
	                            rearWheelOff);
	memcpy(g_rdFrontWheelOff, frontWheelOff, sizeof g_rdFrontWheelOff);
	memcpy(g_rdRearWheelOff,  rearWheelOff,  sizeof g_rdRearWheelOff);

	/*
	 * THE SAME ARM AT THE CAMERA-RELATIVE POSITION, which is the
	 * counter-3 matrix. armW places the rear wheel in the world;
	 * armC draws the arm itself. One angle, two frames.
	 */
	racerdraw_rear_frame(rollMat, camRel, rearFrameC);
	racerdraw_arm_matrix(c->swingarmRest, armAngle, rearFrameC, armC);
	mat34_multiply(viewPart3, armC, camView);

	/* --- the rider, then the two walks, 0x00460db7..0x00461490 --- */

	racerdraw_place_rider(r, bodyMat, drawPos, persoAnim, riderPos);

	/*
	 * THE POSE IS ALREADY BOUND, AND BINDING IT HERE WOULD BE A BUG.
	 *
	 * racerdraw_bind_skeleton belongs to the CALLER and must run
	 * before this function, because it carries the animation frame.
	 * The original does not need the call at all: it keeps the pose
	 * in the racer and fills the angles during the TICK, so by the
	 * time the draw runs they are simply there. Our bind stands in
	 * for that, and calling it here with no frame - the only frame
	 * this function has - would zero every angle and freeze the
	 * rider mid-race.
	 *
	 * It also has to precede racerdraw_steer_pose above, which writes
	 * pose[1].angle; a bind after that would erase the steering.
	 */
	racerdraw_part_walks(r, bodyMat);

	if (pose[RD_PART_WHEEL_FRONT].parent)
		racerdraw_fork_part_matrix(pose[RD_PART_WHEEL_FRONT].angle,
	            pose[RD_PART_WHEEL_FRONT].pivot, scaledTip,
	            pose[RD_PART_WHEEL_FRONT].parent->matrix,
	            pose[RD_PART_WHEEL_FRONT].matrix);

	/*
	 * THE SHADE LEVEL, COMPUTED ONCE. The original calls
	 * maybe_Racer_ShadeLevel at 0x0045f5e8, keeps the result in one
	 * frame slot (+0x128) and pushes it into every part's transform
	 * at 0x0046147c. It is 0x10 on a shaded segment and 0x1f
	 * elsewhere, and it is THE BIKE'S HALF OF THE TUNNEL DARKENING.
	 *
	 * THE WHEELS ARE NOT DARKENED THIS WAY AND MUST NOT BE. They go
	 * through rd_draw_instance, which transforms them at a literal
	 * 0x1f and then lets mesh_shade_vertices overwrite it from a
	 * FIXED light direction - so they sit at a steady dim while the
	 * bike pulses. Handing this level to the wheels instead would
	 * make them track the segment flag, which the original does not
	 * do. racerdraw_shade_parts is a third thing again, and belongs
	 * to the bike-select page.
	 */
	shade = racerdraw_shade_level(r, trk);

	/* --- the part loop, 0x00461490..0x004619c0 --- */

	for (i = 0; i < skl->memberCount; i++) {
		int          first = skl->member[i].firstVertex;
		int          count = skl->member[i].vertexCount;
		int          part  = skl->remap[i];
		float        m[12];
		const float *use;
		MR_Vertex   *v;

		/* A pivot carries no geometry - members 1, 4 and 5. */
		if (count <= 0)
			continue;
		if (first < 0 || first + count > skl->mesh.vertexCount)
			break;

		if (i == 0) {
			use = viewPart0;
			memcpy(g_rdViewPart0, viewPart0, sizeof g_rdViewPart0);
		} else if (i == RD_MEMBER_SWINGARM) {
			use = viewPart3;
			memcpy(g_rdViewPart3, viewPart3, sizeof g_rdViewPart3);
		} else {
			if (part < 0 || part >= RACER_PARTS_PER_LOD)
				continue;
			racerdraw_part_matrix(pose[part].matrix,
			        racerdraw_part_is_rider(part) ? riderPos : drawPos,
			        camEye, camView, m);
			use = m;

			memcpy(g_rdPartView[part], m, sizeof m);
			g_rdPartViewSeen[part] = 1;
		}

		v = transform_verts_flat(use,
		                         (const float *)&skl->bind[first],
		                         count, shade);
		if (!v)
			break;
		if (!base)
			base = v - first;
	}

	if (!base)
		return 0;

	/*
	 * THE SHADING PASS IS OPTIONAL, gated on the global at
	 * 0x004ea4d0 - which is OFF in a race and belongs to the
	 * bike-select page. See g_rdShadeParts.
	 *
	 * THE LIGHT COMES FROM THE CALLER in the original too: 0x004619ca
	 * pushes g_flLightDir before the call, and g_rdLightDir carries
	 * the same constant.
	 *
	 * Both halves are whole - racerdraw_shade_parts and its input
	 * from racerdraw_build_normals, which race.c fills for both
	 * articulated slots of every racer at load.
	 */
	if (racerdraw_shading_enabled(g_rdShadeParts))
		racerdraw_shade_parts(r, g_rdLightDir, base);

	/*
	 * ONE CALL FOR THE WHOLE MESH. The original loops the parts and
	 * queues each against this same base, running ONE group past the
	 * last member; our prim array was built from the whole mesh,
	 * whose face list already carries that trailing group. Same set,
	 * same base, and the queue sorts into depth buckets so the order
	 * between groups does not matter. racerdraw_queue_parts is the
	 * literal transcription and stays uncalled beside this.
	 *
	 * IT TAKES INSTANCE 3'S LIVERY, NOT INSTANCE 0'S.
	 * maybe_Racer_Draw dispatches the part loop on racer+0x5fc
	 * [verified, 0x00461a30], and the bike select page is where the
	 * difference shows: its tick rewrites +0x5fc and the two wheels
	 * every frame and leaves +0x170 alone. See racer.h.
	 */
	queued = cull_and_queue_faces(base, set->prims,
	                              set->primCount, r->liveryRider);

	/* --- the two wheels --- */

	/*
	 * THE PRIM VARIANT, ONE PER WHEEL, BEFORE ITS DRAW. The original
	 * does the front at 0x0045fcd6 and the rear at 0x00460ce6, each
	 * inside the g_nRacerDrawFx branch and each with SETNZ on
	 * (shade != 0x1f) - so the argument is simply "on a shaded
	 * segment". Front takes racer+0x1dc, rear racer+0x360.
	 */
	if (racerdraw_shading_enabled(1)) {
		racerdraw_set_prim_variant(set->wheelPrims[RD_WHEEL_FRONT],
		        set->wheelPrimCount[RD_WHEEL_FRONT],
		        shade != RD_SHADE_FULL);
		racerdraw_set_prim_variant(set->wheelPrims[RD_WHEEL_REAR],
		        set->wheelPrimCount[RD_WHEEL_REAR],
		        shade != RD_SHADE_FULL);
	}

	if (set->wheelOk[RD_WHEEL_FRONT]) {
		float pt[3];

		for (i = 0; i < 3; i++)
			pt[i] = r->frontWheelContact[i] + frontWheelOff[i];
		queued += rd_draw_instance(set->wheelMesh[RD_WHEEL_FRONT],
		        set->wheelPrims[RD_WHEEL_FRONT],
		        set->wheelPrimCount[RD_WHEEL_FRONT],
		        frontWheelMat, pt, camEye, camView, RD_SHADE_FULL,
		        r->liveryFrontWheel);
	}

	if (set->wheelOk[RD_WHEEL_REAR]) {
		float pt[3];

		for (i = 0; i < 3; i++)
			pt[i] = r->rearWheelContact[i] + rearWheelOff[i];
		queued += rd_draw_instance(set->wheelMesh[RD_WHEEL_REAR],
		        set->wheelPrims[RD_WHEEL_REAR],
		        set->wheelPrimCount[RD_WHEEL_REAR],
		        rearMat, pt, camEye, camView, RD_SHADE_FULL,
		        r->liveryRearWheel);
	}

	if (budgetLeft)
		*budgetLeft -= queued;
	return queued;
}

/*
 * ORIGIN  maybe_Racer_BuildShadow @ 0x00461aa0
 *
 * THE BIKE SHADOW IS A STRETCHED HEXAGON ON THE TWO WHEEL CONTACTS,
 * not a rectangle on the racer origin and not a projection of the
 * bike geometry:
 *
 *     lat = (40, 0, 0) through matOrientation      0x42200000
 *     lon = (0, 0, 40) through matOrientation
 *
 *     v0 = front + lon - lat      v3 = rear - lon + lat
 *     v1 = front + lon + lat      v4 = rear - lon - lat
 *     v2, v5 = the midpoints, averaged with 0.5
 *
 * THEN EVERY VERTEX DROPS ONTO THE GROUND INDEPENDENTLY, and the two
 * MIDPOINTS ARE LIFTED TWICE AS FAR as the four corners - 8.0 against
 * 4.0. That is what keeps the middle of the shadow off the road on a
 * crest, and it is confirmed by a live read on the off-road track:
 * the six y values were -3.72, -3.75, +1.00, -2.42, -2.40, +1.02, and
 * the pair standing ~4 units proud is exactly v2 and v5 [verified].
 *
 * THE ORIGINAL PROJECTS WITH Trk_DropPointToGround (0x0040c960,
 * named by this sweep) and we use
 * trk_ground_height_at (Trk_GroundHeightAt, 0x0040c8b0). They are not
 * the same function - ours returns the height and we write it into y,
 * theirs returns the projected point - but they are THE SAME
 * OPERATION, and that is read rather than assumed:
 *
 *     lifted = (p.x, p.y + 10000.0, p.z)     0x004d02d4, a float
 *     out    = Plane_IntersectSegment(ground, p, lifted)
 *
 * The segment differs only in Y, so it is VERTICAL and the result is
 * a straight drop onto the ground plane, not a projection along its
 * normal. It also returns (0,0,0) when segmentCount < 1, the same
 * guard ours has. [verified]
 */
void racerdraw_build_shadow(MR_Racer *r, const MR_Trk *trk)
{
	float lat[3], lon[3];
	float *v;
	int    i;

	if (!r)
		return;

	for (i = 0; i < 3; i++) {
		lat[i] = RD_SHADOW_HALF_WIDTH * r->matOrientation[i];
		lon[i] = RD_SHADOW_HALF_WIDTH * r->matOrientation[6 + i];
	}

	for (i = 0; i < 3; i++) {
		r->bikeShadow.vert[0][i] = r->frontWheelContact[i] + lon[i] - lat[i];
		r->bikeShadow.vert[1][i] = r->frontWheelContact[i] + lon[i] + lat[i];
		r->bikeShadow.vert[3][i] = r->rearWheelContact[i]  - lon[i] + lat[i];
		r->bikeShadow.vert[4][i] = r->rearWheelContact[i]  - lon[i] - lat[i];
	}
	for (i = 0; i < 3; i++) {
		r->bikeShadow.vert[2][i] = RD_SHADOW_MID *
		    (r->bikeShadow.vert[1][i] + r->bikeShadow.vert[3][i]);
		r->bikeShadow.vert[5][i] = RD_SHADOW_MID *
		    (r->bikeShadow.vert[0][i] + r->bikeShadow.vert[4][i]);
	}

	if (!trk || !trk->segments)
		return;

	for (i = 0; i < 6; i++) {
		int seg;

		v = r->bikeShadow.vert[i];
		seg = trk_find_segment_for_point(trk, r->segmentStart, v, NULL);
		v[1] = trk_ground_height_at(trk, seg, v);
		v[1] += (i == 2 || i == 5) ? RD_SHADOW_LIFT_MID
		                           : RD_SHADOW_LIFT_CORNER;
	}
}


/*
 * ORIGIN  Racer_Create @ 0x0045c010 and 0x0045c08e
 *
 * THE TWO SHADOW QUADS, and every field here is read rather than
 * chosen. Racer_Ctor default-constructs the primitives with 60000 in
 * every index slot; Racer_Create then fills them through 0x00422de0,
 * whose arguments are (kind, colour, i0, i1, i2, i3):
 *
 *     0x0045c015   (0x2c, 0, 0, 1, 2, 5)
 *     0x0045c08e   (0x2c, 0, 5, 2, 3, 4)
 *
 * so both quads are kind 0x2c in colour 0 - BLACK - and together they
 * tile the hexagon. The fifth index slot is the wrap entry and takes a
 * copy of the first, which is what 0x00422de0 does with param_4 at
 * both +0x2c and +0x34.
 *
 * IT RUNS ONCE PER RACER. The geometry changes every tick; the
 * primitives never do.
 */
void racerdraw_init_shadow(MR_Racer *r)
{
	static const unsigned short kIndex[2][4] = {
		{ 0, 1, 2, 5 },
		{ 5, 2, 3, 4 }
	};
	/*
	 * THE ATLAS RECTANGLE PER QUAD, in PIXELS: u0, u1, v0, v1.
	 * Read out of the constructed primitives in the running game and
	 * cross-checked against the immediates Racer_Create pushes - 114.0
	 * is 0x42e40000 at 0x0045bfce.
	 */
	static const float kRect[2][4] = {
		{ 113.0f, 58.0f, 2.0f, 34.0f },
		{  56.0f,  2.0f, 2.0f, 34.0f }
	};
	int q, i;

	if (!r)
		return;

	memset(&r->bikeShadow.prim[0], 0, 2 * sizeof(MR_Primitive));
	for (q = 0; q < 2; q++) {
		MR_Primitive *pr = &r->bikeShadow.prim[q];
		float u0 = kRect[q][0] * RD_SHADOW_UV_SCALE;
		float u1 = kRect[q][1] * RD_SHADOW_UV_SCALE;
		float v0 = kRect[q][2] * RD_SHADOW_UV_SCALE;
		float v1 = kRect[q][3] * RD_SHADOW_UV_SCALE;

		pr->kind          = RD_SHADOW_PRIM_KIND;
		pr->effectiveKind = RD_SHADOW_PRIM_KIND;
		pr->colorIndex    = 0;
		/*
		 * ALPHA 0xff, AND IT IS THE WHOLE POINT. +0x3f is the alpha
		 * byte; leaving flatColor at 0 draws the quad fully
		 * transparent-keyed, and the shadow comes out as a plain
		 * grey rectangle instead of the bike silhouette.
		 */
		pr->flatColor     = RD_SHADOW_FLAT_COLOR;
		pr->bankIndex     = RD_SHADOW_BANK;

		/* (u0,v0) (u0,v1) (u1,v1) (u1,v0) - see the note above. */
		pr->uv[0] = u0;  pr->uv[1] = v0;
		pr->uv[2] = u0;  pr->uv[3] = v1;
		pr->uv[4] = u1;  pr->uv[5] = v1;
		pr->uv[6] = u1;  pr->uv[7] = v0;
		/*
		 * SLOT 4 IS THE WRAP, and it takes the first corner like every
		 * other prim in this engine. The original keeps an atlas edge
		 * there instead - 114 and 57, each one past the quad's own u0 -
		 * which belongs to its own UV convention and not to ours.
		 */
		pr->uv[8] = u0;
		pr->uv[9] = v0;

		for (i = 0; i < 4; i++)
			pr->vertexIndex[i] = kIndex[q][i];
		pr->vertexIndex[4] = kIndex[q][0];   /* the wrap slot */
	}
}


/*
 * ORIGIN  maybe_Racer_DrawShadow @ 0x00461f70
 *
 * SIX VERTICES, TWO FACES, FLAT 0x1f, with g_cfgDetailLevel FORCED TO
 * 3 across the queue and restored after. That override is the only one
 * in the racer path and it is not optional: a port that ignores it
 * loses the shadow at low detail and draws it wrong at high.
 *
 * [open] THE RIDER QUAD IS NOT DRAWN. The original gates it on
 * maybe_g_appInitStarted && crashLatch; racer_begin_crash does set
 * crashLatch, so the gate opens - what is missing is the geometry.
 * Its four vertices and two primitives are laid out in racer.h and
 * left unfilled rather than half-built.
 *
 * THE ORIGINAL CALLS Screen3D_CullAndQueueFacesEx WITH 1 and we
 * call cull_and_queue_faces_ex WITH 0. The flag is read - it picks
 * the sort direction - so that is a real difference and it is a
 * DELIBERATE one; see the note at the call.
 */
int racerdraw_shadow(MR_Racer *r, const float camView[12],
                     const float camEye[3])
{
	float      view[12];
	MR_Vertex *v;
	int        save, queued, i;

	if (!r || !camView || !camEye)
		return 0;

	for (i = 0; i < 9; i++)
		view[i] = camView[i];
	for (i = 0; i < 3; i++)
		view[9 + i] = camView[9 + i]
	              - (camEye[0] * camView[i]
	                 + camEye[1] * camView[3 + i]
	                 + camEye[2] * camView[6 + i]);

	v = transform_verts_flat(view, &r->bikeShadow.vert[0][0], 6,
	                         RD_SHADE_FULL);
	if (!v)
		return 0;

	save = g_cfgDetailLevel;
	g_cfgDetailLevel = 3;

	/*
	 * SORTED BY ITS NEAREST CORNER, AND THE ORIGINAL SORTS BY ITS
	 * FARTHEST. A DELIBERATE DIVERGENCE - the fourth argument is 0
	 * here and 1 at 0x004620a3.
	 *
	 * THE SHADOW IS ONE QUAD ABOUT 250 UNITS LONG and a road face is
	 * shorter, so it STRADDLES SEVERAL of them. Keyed on its farthest
	 * corner, the moment that corner crosses a seam the key is taken
	 * in the segment AHEAD and exceeds the key of the road face the
	 * TAIL is still lying on - so that face draws after the shadow in
	 * the back-to-front walk and paints the tail out. The inversion
	 * happens in one step as the key crosses, which is why it SNAPS
	 * rather than fading.
	 *
	 * cull.c records the same failure one scale up: an ocean quad
	 * thousands of units across can have a farthest corner nearer
	 * than a small distant road face and sorts in front of it.
	 *
	 * o THE SIDE CAMERA HIDES IT, which is the confirming evidence.
	 *   The bug needs the shadow's LONG AXIS along the view: only
	 *   then does its far corner reach past a seam while its tail
	 *   sits on a nearer face. Viewed from the side that axis is
	 *   perpendicular to the view, every corner is at nearly the same
	 *   camZ, and there is no inversion to have.
	 *
	 * [open] WHY RETAIL DOES NOT SHOW IT. It passes 1 and so should
	 * invert the same way. Its z-buffer is off by default, which is
	 * the likeliest reason, but that is not established - so this is
	 * recorded as a divergence rather than as a correction.
	 */
	queued = cull_and_queue_faces_ex(v, &r->bikeShadow.prim[0], 2, 0);
	g_cfgDetailLevel = save;

	return queued;
}
