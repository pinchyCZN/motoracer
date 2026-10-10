/*
 * fx.h - the particle seam. NO SYSTEM YET; this is a stub.
 *
 * The same shape as audio.h and for the same reason: name the thing
 * the original has, put it in the call graph where the original puts
 * it, and let a run say it is missing rather than look complete.
 *
 *
 * WHAT THE ORIGINAL DOES [verified]:
 *
 *   maybe_Fx_DrawAllParticles (0x004575c0) is TWENTY-TWO
 *   INSTRUCTIONS. It takes no arguments and loops 260 times calling
 *   maybe_Fx_DrawParticle. maybe_Game_RenderFrame calls it once per
 *   frame, after the racers and before Screen3D_DrawScene.
 *
 * SO THE FUNCTION IS TRIVIAL AND THE SUBSYSTEM BEHIND IT IS NOT.
 * The emitters are the real work - maybe_Racer_EmitWheelFx and the
 * 1599-byte trail emitter at 0x004b00e0, which is game code sitting
 * above the MFC cliff. None of that is reconstructed, so a faithful
 * import of the 22-instruction loop would walk 260 empty slots.
 *
 * The seam exists so the call site is real and the gap is visible.
 */

#ifndef MRACER_FX_H
#define MRACER_FX_H

#include "prim.h"
#include "trk.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the particle pool ------------------------------------------ */
/*
 * g_fxParticles @ 0x0052c498 - 128 slots of 260 bytes. The pool is a
 * MEMBER of g_game (0x0052c478 + 0x20), which is why a displacement
 * search finds only the three functions that take the base directly.
 *
 * THE TICK AND THE DRAW ARE GAME-LEVEL, NOT RACER-LEVEL.
 * maybe_Fx_TickAllParticles has one caller, maybe_Game_Tick at
 * 0x0047d887; maybe_Fx_DrawAllParticles has one, Game_RenderFrame at
 * 0x00480279. Only the SPAWN sits inside the racer tick, through
 * maybe_Racer_EmitWheelFx. Hanging the tick off the racer would run
 * it once per racer.
 */
#define FX_PARTICLE_COUNT 128

typedef struct {
	int           kind;             /* +0x000  5 = smoke; 1..4 other */
	float         position[3];      /* +0x004 */
	float         velocity[3];      /* +0x010 */
	float         rotation[3];      /* +0x01c  accumulated */
	float         rotationRate[3];  /* +0x028  added each tick */
	float         speed;            /* +0x034  KINDS 1..4 ONLY - the
	                                 * magnitude the direction at
	                                 * +0x010 is normalised against.
	                                 * Kind 5 never reads it. */
	int           lifeSpawned;      /* +0x038  never decremented */
	float         life;             /* +0x03c  THE COUNTDOWN */
	MR_Primitive  quad[2];          /* +0x040  front and back */
	float         corners[4][3];    /* +0x0c0  model space, Z = 0 */
	int           size;             /* +0x0f0  an INT, not a float */
	int           height;           /* +0x0f4  KINDS 1..4 - the quad's
	                                 * top edge. Grows, then shrinks,
	                                 * and the particle DIES when it
	                                 * goes negative. */
	int           sizeSpawned;      /* +0x0f8  KINDS 1..4 - the size as
	                                 * spawned; 2x it is the cap that
	                                 * flips growth into decay. */
	int           inUse;            /* +0x0fc  the allocator's test */
	unsigned char unk_0100[4];      /* +0x100 */
} MR_FxParticle;                    /* 260 = 0x104 */

typedef char mr_fxparticle_is_260[(sizeof(MR_FxParticle) == 260) ? 1 : -1];

/*
 * fx_particle_reset_all - maybe_Fx_ResetParticlePool (0x00413dd0).
 * Frees all 128.
 */
void fx_particle_reset_all(void);

/*
 * fx_spawn_particle - maybe_Fx_SpawnParticle (0x00457620).
 *
 * A LINEAR SCAN for the first slot whose inUse is clear, and A FULL
 * POOL SILENTLY DROPS THE REQUEST. With 24 particles a frame from the
 * wheel spray the pool does fill - 122 of 128 busy two frames after a
 * crash - so that is ordinary behaviour, not an error path.
 */
void fx_spawn_particle(int size, const float pos[3], const float vel[3],
                       const float rotRate[3], int life,
                       unsigned short colour, int kind);

/*
 * fx_spawn_trail - maybe_Fx_SpawnTrail (0x004576a0).
 *
 * THE TYRE TRAIL - the dirt specks a wheel leaves OFF THE ROAD, and
 * the other half of the particle pool. It scans the SAME 128 slots
 * fx_spawn_particle does and forwards to the kinds-1..4 constructor.
 *
 * kind IS THE SURFACE ID, straight out of Trk_SurfaceAtPoint, and it
 * picks both the atlas rectangle and the colour - so the specks are
 * sand on the beach and green on grass. SURFACE 0 IS THE ROAD and
 * falls out of the switch without marking the slot in use, which is
 * why the trail stops on tarmac and resumes off it.
 *
 * greyA and greyB are passed by the caller and NEVER READ; the colour
 * comes from the switch instead. Kept in the signature because the
 * original has them.
 */
void fx_spawn_trail(int size, const float pos[3], const float dir[3],
                    const float rot[3], float speed, int life,
                    unsigned short greyA, unsigned short greyB,
                    int kind);

/*
 * fx_sext - the ((v ^ s) - s & M ^ s) - s idiom the emitters share.
 *
 * Take the magnitude, mask it, put the sign back. maybe_Rand wraps the
 * CRT rand() and never returns negative, so s is always 0 and this
 * reduces to v & M - but it is transcribed rather than simplified
 * because the simplification is a property of the generator, not of
 * this code.
 */
int fx_sext(int v, unsigned int mask);

/*
 * fx_burst - maybe_Fx_Burst (0x00467c40).
 *
 * A puff of `count` particles spread through the caller's OWN frame.
 * Both call sites are in the crash; see fx.c for what one particle
 * gets and how it differs from the wheel spray.
 *
 * life is (rand() & lifeMask) + lifeBase, and at both call sites the
 * mask is a single bit - so the burst is half short-lived and half
 * long-lived rather than evenly spread.
 */
void fx_burst(int count, const float pos[3], int lifeBase,
              unsigned int lifeMask, const float m[12]);

/*
 * fx_tick_particle - maybe_Fx_TickParticle (0x004b0730), the kind-5
 * arm. fx_tick_all_particles walks the pool and calls it.
 */
void fx_tick_particle(MR_FxParticle *p, float dt);

/* fx_tick_all_particles - maybe_Fx_TickAllParticles (0x00457590). */
void fx_tick_all_particles(float dt);

/* Read-only access, for the harness and the draw. */
MR_FxParticle *fx_particle_at(int slot);

/*
 * fx_draw_all_particles - maybe_Fx_DrawAllParticles (0x004575c0).
 *
 * detailLevel is g_cfgDetailLevel (0x004dc098): 1 or less draws
 * NOTHING, 2 draws every eighth particle across the whole pool, 3 and
 * up draw every one. Returns the primitives queued.
 */
int fx_draw_all_particles(const float camView[12], const float camEye[3],
                          float camYaw, int detailLevel);


/* ---- the skid ribbons ------------------------------------------ */
/*
 * THE OTHER FX POOL, AND IT IS NOT THE PARTICLES. A ribbon is the
 * black strip a sliding tyre leaves on the road. g_fxRibbons at
 * 0x00542a28 is 16 slots of 5640 bytes, and unlike the particles it
 * is INDEXED rather than pooled: a racer owns fxSlot0 and fxSlot1
 * across frames and keeps appending to them, so there is an
 * allocator for one and not the other.
 *
 * THE RECORD IS FULLY ACCOUNTED FOR, 5640 = 0x1608:
 *
 *     +0x0000   64 x MR_Primitive   the strip's triangles
 *     +0x1000  128 x vec3           the vertices
 *     +0x1600  int                  quad count, capped at 62
 *     +0x1604  int                  vertex write index
 *
 * racers.txt 9.16.1 called the first 4096 bytes "NOT touched" and
 * guessed at 32 bytes per vertex. They are the PRIMITIVES: the draw
 * at 0x004573e0 passes ribbon+0 to Screen3D_CullAndQueueFacesEx as
 * the face array, and 4096 / 64 is 64 faces.
 *
 * A RIBBON STOPS RATHER THAN WRAPS. At 62 quads the appender returns
 * having done nothing - no eviction, no ring - so a long slide draws
 * a mark of bounded length and then simply stops extending. That is
 * behaviour to reproduce, not a limit to raise.
 */
#define FX_RIBBON_COUNT      16
#define FX_RIBBON_PRIMS      64
#define FX_RIBBON_VERTS     128
#define FX_RIBBON_QUAD_MAX   62   /* 0x3e; 62 quads use 124 vertices */

typedef struct {
	MR_Primitive prim[FX_RIBBON_PRIMS];   /* +0x0000 */
	float        vert[FX_RIBBON_VERTS][3];/* +0x1000 */
	int          quadCount;               /* +0x1600 */
	int          vertexCount;             /* +0x1604 */
} MR_FxRibbon;

typedef char mr_fxribbon_is_5640[(sizeof(MR_FxRibbon) == 5640) ? 1 : -1];

/* _DAT_004d0dcc - the z-fight lift the appender adds to both ends. */
#define FX_RIBBON_LIFT   2.0f
/* The shade the draw transforms with, 0x1f in FUN_004573e0. */
#define FX_RIBBON_SHADE  0x1f

/*
 * ORIGIN  Face_InitTriangle @ 0x00422be0
 *
 * kind and effectiveKind both 0x0c, colourIndex 0x4208 expanded to
 * flatColor 0x404040, indices i0 i1 i2 i0 then PRIM_NO_WRAP - the
 * 0xffff in slot 4 is what says the polygon does NOT close through a
 * fourth vertex, which is how a triangle is spelled here.
 */
void fx_face_init_triangle(MR_Primitive *p, int i0, int i1, int i2);

/* FUN_00457120 - one ribbon: build its 64 faces, zero its counts. */
void fx_ribbon_reset(int slot);

/* FUN_00457300 - all sixteen. */
void fx_ribbon_reset_all(void);

/*
 * maybe_Fx_UpdateRibbon (0x00457390) and maybe_Fx_AppendRibbonQuad
 * (0x00457200) together: bounds-check the slot, then append one
 * quad's two vertices. BOTH INCOMING Y VALUES ARE DISCARDED and
 * replaced with the track height plus 2.0, so the mark lies on the
 * ground whatever the bike was doing.
 */
void fx_ribbon_append(int slot, const float a[3], const float b[3],
                      int segHint, const MR_Trk *trk);

/*
 * maybe_Fx_AllocSlot (0x00457350) - hand out the next of sixteen and
 * RESET IT. A ring, not a free list: the counter wraps at 15 and the
 * oldest mark is simply overwritten, which is what lets a player
 * brake as often as they like without ever running out. Only the two
 * counts are cleared; the 64 faces the initialiser built stay.
 */
int  fx_ribbon_alloc_slot(void);

/* FUN_004573e0 - transform and queue every live ribbon. */
int  fx_ribbon_draw_all(const float camView[12], const float camEye[3]);

/* For tests and the harness. */
const MR_FxRibbon *fx_ribbon_at(int slot);

#ifdef __cplusplus
}
#endif

#endif
