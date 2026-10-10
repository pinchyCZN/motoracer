/*
 * trk.h - the .trk track file: segments, meshes, props, links.
 *
 * From Trk_Load (0x0040d650). The whole circuit lives here - the
 * segment spine the grid and the AI are placed against, the meshes
 * that are the road surface and scenery, named prop templates with
 * their placements, and an optional animated-decor block.
 *
 * The format is written up in docs/formats/trk.txt and tools/trk.py is
 * the reference reader: all ten shipped tracks parse there with every
 * byte consumed.
 *
 *
 * THE SEGMENT SPINE IS WHAT THE GRID IS BUILT ON.
 * maybe_Game_InitRace (0x0047bf70) places every racer against the
 * segment array and overwrites Level.Spt's StartPosition, so nothing
 * about the layout can be read out of the manifest. The formula is
 * in grid.h and the evidence in game-object.txt 6.4; neither is
 * restated here.
 *
 *
 * A VERSIONED FORMAT, AND WE READ ALL OF IT. Sections are gated on the
 * version at several points; every shipped track is version 5 but the
 * guards are transcribed rather than assumed away, because a version-5
 * reader that silently mis-parses a version-4 file is worse than one
 * that says so.
 *
 * THE PARSE IS BYTE-EXACT OR IT FAILS. trk_load consumes the file and
 * then checks it landed exactly on the end. That is the same
 * discipline level.c uses and it is the only real defence a binary
 * format has: a wrong field width usually still "works" and produces
 * garbage geometry, whereas a wrong field width that must also land on
 * the last byte almost never does.
 */

#ifndef MRACER_TRK_H
#define MRACER_TRK_H

#include "prim.h"
#include "mesh.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Drawing limits. The prim budget leaves headroom under
 * QUEUE_NODE_MAX (7499) for whatever else the frame queues. */
#define TRK_DRAW_MAX_MESHES   512
#define TRK_DRAW_PRIM_BUDGET 6800

#define TRK_NAME_MAX 16         /* the loader's own 15-char limit + NUL */

/*
 * A track segment. 132 bytes in the file plus a variable index array;
 * 0xfc in the original's memory, which is where the grid code's
 * stride of 252 comes from.
 */
typedef struct {
	float pos[3];
	/*
	 * 4x3: rows 0..2 are the rotation and row 3 is the translation.
	 * The grid reads row 0 as the segment's LATERAL axis and row 3 as
	 * an offset, which is what fixes this layout rather than the
	 * transpose - see game-object.txt 6.4.
	 */
	float matrix[12];
	/*
	 * THE MESH INDEX - which track mesh ("zone") covers this
	 * segment. Read at 0x0040ce9f as segment+0x3c.
	 *
	 * IT MUST BE READ, NOT COMPUTED. It is usually segment/4, and
	 * on eight of the ten shipped tracks it is exactly that - but
	 * ile04 differs on 780 of 782 segments and Track06 on 366 of
	 * 435, where meshes cover 2, 3 or 5 segments instead. [verified
	 * by parsing all ten .trk files]
	 */
	unsigned int  meshIndex;

	/*
	 * THE FILE'S FOUR PLANES, and in memory the original keeps them
	 * at +0x40, +0x50, +0x60 and +0x70:
	 *
	 *     planes[0]  plane0 - and the loader OVERWRITES it with the
	 *                segment's entry plane
	 *     planes[1]  plane1 - no reader and no writer found
	 *     planes[2]  wallPlane0
	 *     planes[3]  wallPlane1
	 *
	 * THE WALLS ARE INPUTS. maybe_Trk_BuildSegmentPlanes reads them
	 * to CLIP the segment's nominal corners, which is how the road
	 * gets its real width. See MR_TrkSegment.txt 3.
	 */
	float planes[4][4];         /* four planes, vec3 plus a scalar */

	/*
	 * THE THREE PLANES THE LOADER BUILDS, at +0x80, +0x90 and +0xa0
	 * in the original. NOT in the file - they are computed from the
	 * clipped corners, which is why a track loaded without that step
	 * has no road surface at all.
	 *
	 * ground[0] and ground[1] are the two triangles of the segment
	 * quad and selector is the VERTICAL plane through its diagonal;
	 * trk_ground_height_at picks a triangle by which side of the
	 * selector the point falls on.
	 *
	 * trk_build_segment_planes FILLS THESE, and trk_load calls it
	 * before returning.
	 */
	float ground[2][4];
	float selector[4];
	/*
	 * THE RACING LINE - segment +0xb0 in the original's memory, and
	 * the dword the file stores immediately after the four planes.
	 *
	 * A LATERAL OFFSET ALONG MATRIX ROW 0, in world units, saying
	 * where on the road the line runs for this segment. ile01 spans
	 * -432 to +128 over 367 distinct values and varies smoothly
	 * segment to segment; ile04 spans -368 to +112 with plateaus
	 * through its constant-radius corners. Well inside the +-1024
	 * band, which is what a racing line should be.
	 *
	 * IT IS A FLOAT. Read as a u32 the values are nonsense, which
	 * is what kept it hidden. Racer_AiFollowLine reads it and aims
	 * there; Trk_LaneOfRacer centres its three lane bands on it.
	 *
	 * IT IS NOT planes[3][0]. That is wallPlane1's normal x at
	 * +0x70, a flat -1.0 on ile01 with the wall distance 1280
	 * beside it, so a line built from it IS THE CENTRELINE give or
	 * take a unit - wrong in a way that looks plausible on a
	 * straight and only shows up in corners. The offset map:
	 *
	 *     +0x40..+0x7f   the file's four planes
	 *     +0x80..+0xaf   the three the loader BUILDS
	 *     +0xb0          this
	 *
	 * so there is no fourth plane for +0xb0 to be the front of.
	 */
	float         lineOffset;   /* +0x0b0 */
	unsigned short *indices;    /* version > 0 */
	int             indexCount;
	/*
	 * THE SURFACE BAND, memory +0x0f0 and +0x0f4, and the LAST TWO
	 * WORDS of the segment record - version > 3 only.
	 *
	 * Trk_SurfaceAtPoint (0x0040ec60) projects the query point onto
	 * matrix row 0 and returns surfaceIn when the result falls
	 * between these and surfaceOut otherwise. They read -1024.0 and
	 * +1024.0 on every segment of ile01, against walls at 1280, so
	 * the band is NARROWER than the track is wide.
	 *
	 * FLOATS, NOT WORDS - as u32 they are nonsense, the same trap
	 * as lineOffset above.
	 */
	float         bandLo;       /* +0x0f0, version > 3 */
	float         bandHi;       /* +0x0f4, version > 3 */

	/*
	 * THE SECOND PER-SEGMENT SECTION, read well after the segment
	 * records - see the note on linkCount below. Trk_Load walks it
	 * with the SEGMENT stride and writes straight into the segment,
	 * so these are segment fields that merely arrive late.
	 *
	 * File order is NOT offset order: the four dwords are read into
	 * +0xec, +0xe8, +0xe4 then +0xe0, at 0x0040dff8..0x0040e08b.
	 */
	unsigned short linkA;       /* +0x0d8, u16              */
	unsigned int   linkB;       /* +0x0da, u16 stored wide  */
	unsigned short linkC;       /* +0x0de, u16              */
	unsigned int   extra;       /* +0x0ec, file order FIRST */
	unsigned int   surfaceOut;  /* +0x0e8, outside the band */
	unsigned int   surfaceIn;   /* +0x0e4, inside the band  */
	/*
	 * SHADED. maybe_Racer_ShadeLevel (0x00465d60) is the whole
	 * consumer: nonzero gives 0x10 and zero gives 0x1f, so a bike
	 * darkens under a bridge and brightens coming out.
	 *
	 * IT IS A BOOLEAN IN EVERY SHIPPED TRACK - 0 or 1 and nothing
	 * else across all ten, from 3 of 716 segments on bercy to 263
	 * of 919 on ile08.
	 */
	unsigned int   shaded;      /* +0x0e0, file order LAST  */

	/*
	 * WHICH SIGNAL SPANS THIS SEGMENT IS INSIDE - seven bits,
	 * one per object-sound signal kind.  NOT READ FROM THE
	 * FILE: Trk_Load computes it in a pass of its own at
	 * 0x0040e0da, right after AnimDecor_Load.
	 *
	 * THE ENCODING IS A TOGGLE, AND THAT IS THE WHOLE TRICK.
	 * `extra` (+0x0ec) carries a signal kind 6..12 on the
	 * segments where a span STARTS or ENDS, and anything
	 * outside that range - it is -1 on most segments - means
	 * no change.  Walking the track in order and XORing bit
	 * (kind - 6) at each marker leaves every segment holding
	 * the set of spans it lies inside.
	 *
	 * So two markers bound a REGION rather than naming a
	 * point: ile01 opens kind 8 at segment 0, and every
	 * segment along the water carries bit 2 until it is
	 * toggled off again.  Reading the markers as points
	 * instead would put the surf in one spot.
	 *
	 * The accumulator starts at 0 and is NOT reset per lap,
	 * so an unclosed span simply runs to the end of the
	 * track.  See objsnd.h and object-sounds.txt 7.1.
	 */
	unsigned int   signalFlags; /* +0x0f8, computed */
} MR_TrkSegment;

/*
 * A FACE IS AN MR_Primitive. The 58 bytes in the file are a PACKED
 * form of the 0x40-byte struct the rest of the engine already draws,
 * and the loader's out-of-order reads are simply the unpacking:
 *
 *     file  0 (2)  -> +0x00  kind
 *     file  2 (2)  -> +0x38  flags
 *     file  4 (2)  -> +0x02  colorIndex
 *     file  6 (2)  -> +0x36  bankIndex
 *     file  8 (8)  -> +0x2c  vertexIndex[0..3]
 *     file 16 (2)  -> +0x34  vertexIndex[4], the wrap slot
 *     file 18 (40) -> +0x04  uv[5][2]
 *                     +0x3c  flatColor, computed from colorIndex
 *
 * 2+2+2+2+8+2+40 = 58, with every field accounted for.
 *
 * o AND THE TRIANGLE MARKER IS ALREADY PRIM_NO_WRAP. trk.txt calls
 *   file+16 a marker that is -1 for a triangle; as a u16 that is
 *   0xffff, which is exactly the "no wrap slot" value every quad test
 *   in the engine uses. No conversion, no vertexCount field - the
 *   existing primitive path reads it directly.
 */
typedef struct {
	float        *verts;        /* three floats each, WORLD SPACE */
	unsigned int *vertExtra;    /* the fourth dword of each vertex */
	int           vertexCount;
	MR_Primitive *prims;
	int           primCount;
} MR_TrkMesh;

/* A prop template: a name and three meshes, presumably three levels
 * of detail. Nothing has confirmed that, so they are just [0..2]. */
typedef struct {
	char       name[TRK_NAME_MAX];
	MR_TrkMesh mesh[3];
} MR_TrkNamed;

/*
 * A placement. Instances whose name begins "CAMERA" are not props -
 * maybe_Trk_CountCameras picks them out by name after loading, which
 * is how a track authors its camera positions. See trk.txt section 4.
 */
typedef struct {
	char         name[TRK_NAME_MAX];
	float        pos[3];
	/*
	 * THREE EULER ANGLES IN RADIANS. The engine turns a triple into
	 * a 0x4000 table index by multiplying by the double at
	 * 0x004d13a0, which is TRIG_IDX_PER_RAD BIT FOR BIT: 0x4000
	 * over the game's TYPED 2*pi, NOT over the true one. The two
	 * part company at the eleventh digit - see trig.h. So these go
	 * straight into mat34_from_euler. A palm carries yaw only; a
	 * boat carries pitch and roll.
	 */
	float        rot[3];
	int          namedIndex;    /* which MR_TrkNamed it instantiates */

	/*
	 * THE OBJECT'S SOUND DESCRIPTOR, or -1 when it is silent.
	 *
	 * This is the id SOUND.BPT's soundDesc records carry, and it
	 * is what attaches a sound to a piece of decor - see
	 * objsnd.h. Read live off MOTO on the speed bay: six objects
	 * named Gull05 hold 0, which is the mouet-1/2/3 descriptor,
	 * and corsair3/corsair4 hold 8. Everything else holds -1.
	 *
	 * IT IS ONLY IN version > 4 FILES, and the default must be -1
	 * rather than the 0 a calloc leaves - 0 is a perfectly good
	 * descriptor id, so a zeroed field would silently bind every
	 * object on an old track to the first sound in the script.
	 *
	 * [inferred - the file field is the one the original reads
	 * into decor object +0x44, and it is the only unexplained
	 * per-instance value; the live +0x44 values match what this
	 * position holds. An id with no descriptor is logged and
	 * skipped, so a wrong reading is loud rather than silent.]
	 */
	int          soundDesc;     /* version > 4, else -1 */

	/*
	 * NOT DRAWN WHEN 0. The original's decor object carries this at
	 * +0x2c, and its draw - FUN_004022f0, vtable slot +0xc - opens
	 * with `if (obj+0x2c != 0)` and otherwise returns having drawn
	 * nothing. Trk_Load builds every instance with it set, and
	 * maybe_Trk_CountCameras then CLEARS it on the CAMERAs. That is the
	 * whole reason a camera marker has a mesh, a position and a
	 * rotation and is still invisible in retail.
	 *
	 * Read live off MOTO on the speed bay: 1 on every one of the
	 * props, 0 on all twenty CAMERAs and on nothing else.
	 */
	int          drawable;
} MR_TrkInstance;

/*
 * ANIMATED DECOR - the helicopter, its rotor, the birds.
 *
 * ORIGIN  AnimDecor_Load @ 0x0048b7e0, animDecor.cpp
 *
 * IT CREATES NOTHING. Each record names an entry of the decor table
 * this same file already loads - instances[] - and the animator
 * writes that entry's own rot[] and pos[] every frame. The mesh is
 * already there and already drawn; without this it simply sits at the
 * values the file gave it.
 *
 * THE INDEX IS DIRECT AND THE ORDER IS CONFIRMED. record.decorIndex
 * is packed & 0xffff into DAT_004dc0e4, and our instances[] is the
 * same table in the same order - checked live on the dirt arena, 74
 * entries both sides, names slot for slot, and finish_000 and the two
 * CAMERAs agreeing to the digit. The ones that DISAGREED were exactly
 * agusta_004, helice_004 and the three flash lights: the animated
 * ones, mid-animation on MOTO and static on ours.
 */
/* 0x004d1bd8, a DOUBLE. The seed and the period are scaled by it
 * at load, which puts them in the same 0.1-millisecond unit the
 * tick's delta already uses - 3.3 at 33 ms. */
#define TRK_ANIM_TIME_SCALE  0.1f
/* THESE THREE ARE FLOATS in the original, not doubles - 0x004d1bc8
 * and 0x004d1bcc are four bytes apart, and 0x004d1bc8 reads
 * 6.2831854820251465, which is 6.2831853f. Ours are double literals
 * and differ in the eighth digit. */
#define TRK_ANIM_TRI     2.0f    /* 0x004d1bb0 and 0x004d1bc0 */
#define TRK_ANIM_TWOPI   6.2831855f /* 0x004d1bc8             */
#define TRK_ANIM_HALF    0.5f    /* 0x004d1bcc                */

#define TRK_ANIMDECOR_MAX  64       /* the loader aborts past 63   */
#define TRK_ANIMPATH_MAX   80       /* and past 79 frames          */

/* 0x100 in maybe_Trk_CountCameras, and the original names it out
 * loud: past this it raises "MAX_NB_CAMERA=%d" on the offending
 * instance. */
#define TRK_CAMERA_MAX     256

typedef struct {
	int   type;        /* 1..8; 0 is an empty slot             */
	int   axis;        /* packed >> 24, which angle to drive   */
	int   decorIndex;  /* packed & 0xffff, into instances[]    */

	float time;        /* the running accumulator              */
	float lo;          /* range low, or the rate on 3 and 5    */
	float hi;          /* range high                           */
	float phase;       /* phase, or the base angle             */
	float period;

	float from[3];     /* position lerp, types 4 and 8         */
	float to[3];

	/*
	 * TYPE 7 IS A GEOMETRY SWAP, NOT A POSITION PATH. path holds
	 * pathCount WHOLE REPLACEMENT VERTEX SETS of frameCount vec3
	 * each - a gull's wing is 19 of them - and the animator picks
	 * one by the step. trk_draw passes it to trk_queue_mesh_verts
	 * in place of the template's own array; frameCount must equal
	 * the mesh's vertexCount or it is not this mesh's animation.
	 */
	int   pathCount;
	int   frameCount;
	float *path;       /* pathCount * frameCount vec3          */
	/*
	 * THE CURRENT STEP, 0 = the template's own vertices and
	 * 1..pathCount = path[step-1]. The draw reads it.
	 */
	int   frame;
} MR_AnimDecor;


typedef struct {
	int             version;

	MR_TrkSegment  *segments;
	int             segmentCount;

	MR_TrkMesh     *meshes;
	int             meshCount;

	MR_TrkNamed    *named;
	int             namedCount;

	MR_TrkInstance *instances;
	int             instanceCount;

	/*
	 * THE IN-RACE MAP, built by trk_build_map from the segment
	 * array - the original keeps the same mesh inside the track
	 * object at +0x28. Baked at map scale and centred on its own
	 * bounds, so the draw only translates and rotates it.
	 */
	MESH            mapMesh;
	float           mapOrigin[3];
	float           mapScale;

	/*
	 * NOT A LINK TABLE, AND NOT ITS OWN ARRAY. The count is read
	 * where a link section would be and equals segmentCount in every
	 * shipped track (788, 648, 622, 782, 668, 435, 919, 716, 1048,
	 * 848 - exact on all ten), because the loop it introduces writes
	 * per-segment fields into the segments themselves. The name is
	 * kept because formats/trk.txt describes the file that way.
	 */
	int             linkCount;

	/*
	 * AnimDecor is optional. animDecorBytes is the section's size
	 * and 0 when absent, so the byte-exact end check covers it
	 * whether or not every record inside was understood.
	 */
	int             animDecorPresent;
	MR_AnimDecor    animDecor[TRK_ANIMDECOR_MAX];
	int             animDecorCount;
	int             animDecorObjects;
	unsigned int    animDecorBytes;

	/* ORIGIN  Trk_Load @ 0x0040e0a5, 0x0040e0b9 -> 0x004db2b0,
	   0x004db2b4. PER TRACK, not constants - dd-statics.txt 7.2.4. */
	int             horizonYBias;
	float           horizonDamp;

	/*
	 * How many instances trk_count_cameras claimed. The original
	 * keeps it in g_trkCameraCount and caps it at 256, which is the
	 * only thing the number is used for; the replay camera does NOT
	 * index it - Trk_FindNearestCamera re-walks the instances by
	 * name. Kept because the cap is a real limit of the format.
	 */
	int             cameraCount;
} MR_Trk;

/*
 * trk_load - read a .trk by logical asset name, e.g. "ILE01\\ile01.trk".
 *
 * Returns 0 and logs on a bad magic, an unsupported version, an
 * implausible count, a short read, or a parse that does not finish
 * exactly at the end of the file. On failure *out is zeroed and
 * nothing is left allocated.
 */
int  trk_load(MR_Trk *out, const char *logicalName);

/*
 * trk_count_cameras - maybe_Trk_CountCameras (0x00473240), whole.
 *
 * Claims the instances whose TEMPLATE name begins "CAMERA" as the
 * track's replay camera positions: counts them and clears their
 * drawable flag so the prop pass skips them. trk_load calls it, the
 * same as Trk_Load does.
 */
void trk_count_cameras(MR_Trk *t);
/*
 * trk_build_map - build the in-race map outline from the segment
 * array. See docs/subsystems/hud-map.txt. Returns 0 if there is
 * nothing to build.
 */
int trk_build_map(MR_Trk *t);

void trk_free(MR_Trk *t);

/* Log the headline counts, which are the numbers trk.txt section 7
 * tabulates for all ten tracks - so a load can be checked against a
 * table that already exists. */
void trk_log(const MR_Trk *t, const char *what);

/*
 * trk_draw - queue every mesh. Returns how many were queued.
 *
 * The caller brackets this with queue_reset/vertexpool_reset and
 * follows it with queue_walk, exactly as page 10 does for the bike.
 * view34 and camPos are the same pair model_draw takes.
 *
 * camSegment AND camForward SELECT THE WORLD MESHES - the ring
 * window of 0x0040ce60, the camera's mesh plus a span each way along
 * the track. Pass camSegment < 0 for the old distance sort, which is
 * what a caller with no track position (page 10's preview) needs.
 *
 * backPlane is LenBPlane from the level manifest - 18000 on ile01 -
 * and is an EXTRA CLIP, NOT THE SELECTION: the original has no
 * distance test on the world meshes at all. Pass 0 for none.
 *
 * propsOut and meshesOut are optional and report the two halves
 * SEPARATELY, because the combined figure cannot be read against
 * meshCount - the prop instances are folded into it and the world
 * meshes are deliberately ring-windowed, so one number conflates
 * what is NEAR THE CAMERA with what EXISTS.
 */
int trk_draw(const MR_Trk *t, const float *view34, const float *camPos,
             float backPlane, float propClip,
             int camSegment, const float *camForward,
             const int *segList, int segCount,
             int *propsOut, int *meshesOut);

/*
 * trk_anim_decor_tick - the AnimDecor animator, 0x0048b370, once per
 * SIMULATION STEP. dtScaled is the tick's 0.1-millisecond delta,
 * which is the unit the loader already scaled the times into.
 *
 * It drives entries of instances[] - an angle into rot[axis] or a
 * position into pos[] - and trk_draw already builds its matrix from
 * those, so nothing else has to change to see it.
 */
void trk_anim_decor_tick(MR_Trk *t, float dtScaled);

/*
 * trk_zone_span - how far the ring window reaches, and which way.
 * One dot product picks 10/1, 5/5 or 1/10. In MESHES; the segment
 * lister multiplies by the four segments a mesh covers.
 */
void trk_zone_span(const MR_Trk *t, int camSegment,
                   const float camForward[3], int *aheadOut,
                   int *behindOut);

/* Shift every face's bankIndex into our global slot pool. */
void trk_rebase_banks(MR_Trk *t, int delta);


/* ------------------------------------------------------------------ */
/* PLANE HELPERS AND THE GROUND QUERY.                                 */

/*
 * A plane is four floats: the normal in [0..2] and d in [3], so that
 * n . p + d is the signed distance for a unit normal. That is the
 * layout the file uses and the one every consumer assumes.
 */

/*
 * plane_distance - signed distance from a point to a plane.
 * ORIGIN  Plane_Distance
 */
float plane_distance(const float plane[4], const float p[3]);

/*
 * plane_intersect_segment - where the segment a->b crosses the plane.
 *
 *     t   = -((n . a + d) / (n . (b - a)))
 *     out = a + (b - a) * t
 *
 * BOTH DEGENERATE CASES RETURN a rather than failing: a zero-length
 * segment, and a segment parallel to the plane. So a caller cannot
 * tell "no intersection" from "the intersection is at a", and the
 * original does not try to.
 *
 * THIS IS WHAT CLIPS THE ROAD. maybe_Trk_BuildSegmentPlanes takes the
 * segment's nominal corners at +-2048 and clips each against a wall
 * plane with this, so the road ends up as wide as the walls allow.
 *
 * ORIGIN  Plane_IntersectSegment @ 0x004b0f60
 */
void plane_intersect_segment(const float plane[4], float out[3],
                             const float a[3], const float b[3]);

/* Trk_WrapSegmentIndex (0x0040ec30) - fold an index into 0..count-1. */
int trk_wrap_segment_index(const MR_Trk *t, int i);

/*
 * trk_surface_at_point - Trk_SurfaceAtPoint (0x0040ec60).
 *
 * The surface id under a point, and the thing that decides whether a
 * tyre leaves a trail: 0 is the road and the effect's constructor
 * declines to build anything for it.
 *
 * A SIGNED DISTANCE ALONG MATRIX ROW 0 from the segment origin,
 * offset by the row-3 translation, tested against the band.
 */
int trk_surface_at_point(const MR_Trk *t, int segment,
                         const float p[3]);

/*
 * trk_contain_point - clamp a point back inside a segment's two
 * wall planes. Trk_ContainPointInSegment 0x0040cd90; the original
 * calls it every frame on the racer position, just before the
 * ground height. Without it wall penetration accumulates.
 */
void trk_contain_point(const MR_Trk *t, int segment, float p[3]);

/*
 * trk_ground_height_at - the road height under a point.
 *
 *     if (segCount < 1)                       return 0
 *     pl = plane_distance(selector, p) >= 0 ? ground[0] : ground[1]
 *     if (pl.y == 0)                          return 0
 *     return -((pl.x*p.x + pl.z*p.z + pl.d) / pl.y)
 *
 * THE STANDARD PLANE-HEIGHT SOLVE: for a*x + b*y + c*z + d = 0,
 * y = -(a*x + c*z + d) / b. A VERTICAL ground plane returns 0.0
 * rather than dividing by zero, and so does a track with no segments.
 * seg->ground and seg->selector come from trk_build_segment_planes,
 * which trk_load calls before returning.
 *
 * ORIGIN  Trk_GroundHeightAt @ 0x0040c8b0
 */
float trk_ground_height_at(const MR_Trk *t, int segment,
                           const float p[3]);

/*
 * trk_segment_has_surface - whether a segment's road surface exists.
 *
 * trk_ground_height_at returns 0.0 both for "this segment has no
 * usable ground plane" and for "the road really is at y = 0", and a
 * caller that snaps the bike to it cannot tell those apart - the
 * second is fine and the first drops the rider through the world.
 *
 * So ask this first. It is true when either ground plane has a
 * non-zero Y in its normal, which is exactly the condition
 * trk_ground_height_at needs to avoid dividing by zero.
 */
int trk_segment_has_surface(const MR_Trk *t, int segment);


/*
 * plane_from_normal_point - a plane through p with normal n, stored
 * as n in [0..2] and d = -(n . p) in [3].
 * ORIGIN  Plane_FromNormalPoint @ 0x004b0f20
 */
void plane_from_normal_point(float out[4], const float n[3],
                             const float p[3]);

/*
 * trk_build_segment_planes - build every segment's road surface.
 *
 * trk_load CALLS IT before returning. Anything that builds an
 * MR_Trk by another route must call it too: without it seg->ground
 * and seg->selector are zero and trk_ground_height_at returns 0.0
 * everywhere.
 *
 * For each segment i, with j = (i+1) % segmentCount:
 *
 *     A = clip(seg[i].wall0, seg[i].position -> seg[i].position + row0*-2048)
 *     B = clip(seg[i].wall1, seg[i].position -> seg[i].position + row0*+2048)
 *     C = clip(seg[j].wall0, seg[j].position -> seg[j].position + row0*-2048)
 *     D = clip(seg[j].wall1, seg[j].position -> seg[j].position + row0*+2048)
 *
 *     planes[0] = plane(normalize(row2), seg[i].position)   the ENTRY plane
 *     selector  = plane(normalize(Bz-Cz, 0, Cx-Bx), C)
 *     ground[0] = plane(normalize((C-A) x (B-A)), A)
 *     ground[1] = plane(normalize((C-D) x (B-D)), D)
 *
 * THE QUAD IS SPLIT ON THE DIAGONAL B->C - this segment's RIGHT
 * corner to the next one's LEFT. ground[0] is the triangle A,B,C and
 * ground[1] is B,C,D. The selector is the VERTICAL plane through that
 * diagonal, so trk_ground_height_at picks a triangle by which side of
 * it a point falls on, which is a question about x and z only.
 *
 * +-2048 IS NOMINAL. Every corner is clipped against a wall plane, so
 * the road is as wide as the walls allow and 4096 is only its
 * maximum. On the track measured, the walls sat at +-1024.
 *
 * EACH CORNER CLIPS AGAINST ITS OWN SEGMENT'S WALL, on its own side -
 * left corners against wall0, right against wall1, and the next
 * segment's corners against the NEXT segment's walls, not this one's.
 *
 * ALL OF THAT WAS PINNED AGAINST THE RUNNING GAME, not read out of
 * the decompiler, which loses the argument flow here. Both cross
 * products and the selector reproduce the live planes to five decimal
 * places on a curved, sloped segment. See MR_TrkSegment.txt 3.
 */
void trk_build_segment_planes(MR_Trk *t);


/*
 * trk_clip_move_against_walls - test a move against the segment's two
 * side walls and BOUNCE the direction off whichever it crossed.
 *
 * maybe_Trk_ClipMoveAgainstWalls (0x0040ca80), __thiscall on the
 * track with six stack arguments - RET 0x18 confirms the count.
 *
 *     if every |from - to| component <= 1e-5        return 0
 *
 *     for wall in { planes[2], planes[3] }:         seg+0x60, +0x70
 *         if plane_distance(wall, to) < 0:          outside
 *             hitPoint = intersect(wall, from, to)
 *             normal   = wall
 *             dir     -= 2 * dot(dir, normal) * normal
 *             normalise(dir)
 *             hit = 1
 *
 * IT REFLECTS, IT DOES NOT SLIDE. The factor at 0x004d02d8 is 2.0,
 * so the expression is dir - 2*(dir.n)*n: the mirror image about the
 * wall plane. A slide would drop the 2 and leave dir - (dir.n)*n,
 * the projection into the plane - and that is the whole feel of a
 * wall. A slide lets a bike scrape along and keep most of its
 * heading; a reflection sends it back out at the incident angle.
 * See racers.txt 9.39.2.
 *
 * BECAUSE BOTH VECTORS ARE UNIT, THE REFLECTION PRESERVES LENGTH
 * EXACTLY, so the renormalise that follows is defensive against
 * float drift rather than load-bearing. Keeping it anyway: it is
 * what the original does, and the caller's dir really is unit
 * (racer+0x18c measured 1.000000 live).
 *
 * BOTH WALLS ARE TESTED, not just the first match. The original
 * UNROLLS the two and neither returns early, so a move into a corner
 * reflects twice and the second reflection operates on the direction
 * the first already turned. hitPoint and normal are overwritten by
 * the second hit.
 *
 * from and to are the move; hitPoint and normal are outputs; dir is
 * IN OUT. The caller passes racer+0x18c as dir and racer+0x31c5 as
 * normal, then pushes the bike out along that normal by 32.0.
 *
 * Returns non-zero if either wall was crossed.
 */
int trk_clip_move_against_walls(const MR_Trk *t, int segment,
                                const float from[3], const float to[3],
                                float hitPoint[3], float dir[3],
                                float normal[3]);


/*
 * trk_find_segment_for_point - which segment a point is in.
 *
 * Trk_FindSegmentForPoint (0x0040c7e0), __thiscall on the track with
 * three stack arguments - RET 0xc.
 *
 * IT IS A LINEAR WALK FROM A HINT, not a search. Given a segment to
 * start from it steps forward or backward one segment at a time until
 * the entry plane test flips, which is cheap precisely because the
 * caller always has a good hint: last frame's segment.
 *
 *     d = plane_distance(segments[seg].planes[0], p)
 *
 *     d >= 0   walk FORWARD while the next segment's d is still >= 0;
 *              return the LAST one that was
 *     d <  0   walk BACKWARD until d >= 0; return that one
 *
 * planes[0] is the segment's ENTRY plane - trk_load overwrites the
 * file's plane0 with one built from the segment's own position and
 * direction. So d >= 0 means "the point has passed this segment's
 * start", and the answer is the last segment it has entered but whose
 * successor it has not.
 *
 * BOTH WALKS WRAP AROUND the segment list, which is what makes it
 * work on a closed circuit.
 *
 * NEITHER WALK IS BOUNDED IN THE ORIGINAL. If every entry plane
 * reported the same sign - degenerate planes, or a point nowhere near
 * the track - it would loop forever. Ours stops after segmentCount
 * steps and returns the hint unchanged. THIS IS A DELIBERATE
 * DIVERGENCE: a hang is the one failure mode that gives no
 * information at all, and the guard costs one comparison per step.
 *
 * The original also clamps seg to 0 when it is >= segmentCount but
 * does NOT check for negative, so a negative hint indexes out of
 * bounds. Ours clamps both ways.
 *
 * outDist may be NULL; when given it receives the plane distance at
 * the segment returned.
 */
int trk_find_segment_for_point(const MR_Trk *t, int segment,
                               const float p[3], float *outDist);

/*
 * THE VISIBLE SEGMENT LIST - half of
 * maybe_Trk_DrawZonesAndListSegments (0x0040ce60).
 *
 * THAT FUNCTION IS TWO FUNCTIONS: it draws the track geometry around
 * the camera, and as a by-product it LISTS the segments in that
 * window. maybe_Race_DrawRacers then draws only racers whose
 * segmentStart is in the list.
 *
 * THIS LIST - NOT the frustum and NOT the 18000-unit draw distance -
 * IS WHAT STOPS A DISTANT RACER BEING DRAWN. Measured from both
 * ends and they agree: AI racers 14 to 30
 * segments ahead are all drawn, 46 to 60 are not.
 *
 * A NULL list means NO FILTERING. Note that in the shipped binary
 * that branch is DEAD - maybe_Race_DrawRacers has one caller and it
 * never passes NULL - so it is a safety valve here, not a mode.
 */
int trk_list_visible_segments(const MR_Trk *t, int camSegment,
                              const float camForward[3],
                              int *out, int maxOut);

/*
 * trk_draw_segment_decor - the per-segment decor dispatch, still a
 * stub.
 *
 * EACH SEGMENT'S indices[] IS THE DECOR LIST, and trk_draw already
 * draws the props from it. Checked live: segment 0 names decor 0,
 * FINISH_000 on the start line, and the lists run in segment
 * order.
 *
 * What is left for this function is the original's SHAPE - the
 * dispatch inside trk_list_visible_segments' own loops rather than a
 * pass of its own.
 */
void trk_draw_segment_decor(const MR_Trk *t, int segment);

/*
 * SEVEN SIGNAL KINDS, 6..12.  Enforced twice in the original:
 * ObjSnd_AddSignal rejects anything outside the range, and Trk_Load's
 * own span pass tests it at 0x0040e10d before setting a bit.
 * kind - TRK_SIGNAL_BASE is the bit in MR_TrkSegment.signalFlags.
 */
#define TRK_SIGNAL_BASE   6
#define TRK_SIGNAL_KINDS  7

/*
 * trk_find_signal_segments - nearest segment carrying each
 * signal bit, within +/-window segments of cur.
 *
 * out[] must be pre-filled by the caller, exactly as the
 * original requires - this only overwrites a slot it finds
 * something nearer for, so a bit with nothing in the window
 * keeps whatever was there.  Fill it with -1.
 */
void trk_find_signal_segments(const MR_Trk *t,
                              int out[TRK_SIGNAL_KINDS],
                              int cur, int window);

/* The 0x17 window and the 1e13 "nothing yet" floor, both
 * literals in Trk_FindNearestCamera. */
#define TRK_CAM_SEARCH_SEGS  23
#define TRK_CAM_SEARCH_FAR   1.0e13f

/*
 * trk_find_nearest_camera - the nearest CAMERA decor along the
 * track, or (0,0,0) when there is none within the window.
 *
 * ORIGIN  Trk_FindNearestCamera @ 0x0040d4f0
 *
 * THE SENTINEL IS THE OUT VECTOR, not a return value - this returns
 * void and the caller tests out[] for non-zero, which is what the
 * original does. See the definition for why that is kept rather
 * than tidied.
 */
void trk_find_nearest_camera(const MR_Trk *t, int segment,
                             const float pos[3], float out[3]);

/*
 * THE SPAN, track object field [9] at 0x0052c12c.
 *
 * IT IS A HARD-CODED CONSTRUCTOR CONSTANT, not a file field and not
 * per-track: MOV dword ptr [ESI + 0x24], 0xa at 0x0040c5b4 in the
 * Track constructor, and no other writer exists. Confirmed live from
 * both ends in one capture - field [9] read 10, and the list the game
 * had just built was 49 entries, 25 forward and 24 backward, which is
 * ahead = behind = 5 = span / 2. [verified]
 */
#define TRK_ZONE_SPAN        10

/*
 * The facing thresholds, DOUBLES at 0x004d02f0 and 0x004d02f8 -
 * +0.25 and -0.25. Reading them as floats gives nonsense, the same
 * width trap as the cull radius.
 */
#define TRK_ZONE_FACING      0.25

/*
 * Worst case is span*4+5 forward plus span*4+4 backward = 89, at
 * (10, 1) or (1, 10). The original writes into a fixed global with
 * 1024 bytes of clear space after it; 128 is generous and bounded.
 */
#define TRK_SEGMENT_LIST_MAX 128

#ifdef __cplusplus
}
#endif

#endif /* MRACER_TRK_H */
