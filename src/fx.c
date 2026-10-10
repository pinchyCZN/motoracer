/*
 * fx.c - the particle seam and the skid ribbons. See fx.h.
 */

/*
 * <windows.h> FOR MAX_PATH, which bank.h uses in MR_Bank.name
 * without including it. Every other consumer of that header -
 * bank.c, drawscene.c, glass.c, horizon.c, level.c, prim.c -
 * does the same, so this follows the house convention rather
 * than changing a shared header.
 */
#include <windows.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "fx.h"
#include "rand.h"
#include "log.h"
#include "bank.h"
#include "cull.h"
#include "transform.h"
#include "trig.h"
#include "model.h"


/* ------------------------------------------------------------------ */
/* THE PARTICLE POOL - the smoke and the wheel spray                   */
/* ------------------------------------------------------------------ */

/*
 * ORIGIN  g_fxParticles @ 0x0052c498, 128 slots of 260 bytes.
 */
static MR_FxParticle s_particles[FX_PARTICLE_COUNT];

/*
 * THE TICK CONSTANTS, and WATCH THE WIDTHS - the three the tick
 * integrates with are DOUBLES whose low dwords read as absurd floats,
 * which is the same trap as 0x004d12a8 in the wall recovery.
 */
#define FX_GRAVITY    (-0.0864)   /* 0x004d2070, DOUBLE, velocity.y  */
#define FX_DRAG         (0.8)     /* 0x004d2078, DOUBLE, x and z     */
#define FX_SIZE_GROW    (0.6)     /* 0x004d2058, DOUBLE              */

#define FX_PARTICLE_KIND     0x44   /* both InitQuad calls pass it   */
#define FX_TRAIL_KIND        0x26   /* the trail's, NOT the smoke's  */
#define FX_TRAIL_GROW        3.84f  /* 0x004d202c, a FLOAT           */
#define FX_TRAIL_SHRINK      1.28f  /* 0x004d2030, a FLOAT           */
#define FX_TRAIL_GRAVITY  (-0.024525) /* 0x004d2038, a DOUBLE        */
#define FX_TRAIL_DT_SCALE     0.01f /* 0x004d2048, a FLOAT           */
#define FX_TRAIL_MOVE_SCALE  128.0  /* 0x004d2050, a DOUBLE          */

/*
 * THE PARTICLE ATLAS, AND ITS BANK INDEX IS NOT PORTABLE.
 *
 * The original writes a literal 1 into the primitive, and MOTO's bank
 * 1 is data\speed\bnk00_01.raw - read out of its bank table,
 * has stride 824 and its name at +76. That page is loaded whatever
 * track is running, so "SPEED" here is a shared asset directory
 * rather than the Speed Bay track.
 *
 * OUR LOADER PUTS THE SAME FILE SOMEWHERE ELSE - bank 4 in a live
 * race, because the port's level list starts at RACE_TRK_BANK_BASE
 * and its contents differ from the original's. Copying the literal 1
 * across bound a bank that is not loaded.
 *
 * AND AN UNBOUND BANK DRAWS BLACK, not untextured. prim_is_textured
 * correctly returns 0 and the COLOUR comes out of the bright flat
 * path - but the blend ladder's texture bit is built from the KIND
 * alone, without asking whether a page is bound, so the stage
 * modulates against nothing. That is the whole symptom: black balls
 * where retail has white ones.
 *
 * So the bank is resolved BY NAME, once, and the particles simply do
 * not draw if it is missing - which is better than drawing black.
 */
#define FX_PARTICLE_COLOUR 0xffdfu  /* Color_Pack16(0x80,0x80,0x80)  */
#define FX_PARTICLE_FLAT 0x00f8f8f8u/* the 5-5-5 unpack, NO alpha    */
#define FX_PARTICLE_SHADE    0x1f   /* the transform's last argument */

/*
 * THE TWO UV SETS, AND THE SPAWN PAIR IS NEVER SEEN. The constructor
 * writes (225,0)(255,0)(255,31)(225,31) and the tick overwrites them
 * with these before the first draw, because the tick runs first.
 * Both are constants - there is no frame animation, whatever the
 * eight stacked puff frames in the atlas suggest.
 */
static const float s_fxUvSpawn[8] = {
	225.0f,   0.0f, 255.0f,   0.0f,
	255.0f,  31.0f, 225.0f,  31.0f
};
static const float s_fxUvTick[8] = {
	224.0f, 160.0f, 255.0f, 160.0f,
	255.0f, 191.0f, 224.0f, 191.0f
};

/*
 * THE BIKE'S EFFECTS ATLAS, resolved once and cached. Negative
 * caching would be wrong: the banks load AFTER fx_particle_reset_all
 * runs, so a miss early in a race must not stick.
 */
static int s_fxBank = -1;

/* ORIGIN  maybe_Fx_ResetParticlePool @ 0x00413dd0 */
void fx_particle_reset_all(void)
{
	int i;

	for (i = 0; i < FX_PARTICLE_COUNT; i++)
		s_particles[i].inUse = 0;

	/*
	 * AND FORGET THE ATLAS. The bike's page is a DIFFERENT BANK on a
	 * different level - KTMCROSS where a road track had SPEED - so a
	 * cached index from the last race would draw out of whatever now
	 * occupies that slot. Race load calls this, so the lookup runs
	 * again against the banks actually loaded.
	 */
	s_fxBank = -1;
}


MR_FxParticle *fx_particle_at(int slot)
{
	if (slot < 0 || slot >= FX_PARTICLE_COUNT)
		return NULL;
	return &s_particles[slot];
}


/*
 * ORIGIN  maybe_Prim_InitQuad @ 0x00422d20, plus the Prim_SetColorIndex
 *         that follows each of its two calls
 *
 * THE CONVENTION, NOT THE RECORD. CLAUDE.md puts the primitive queue
 * on the replace-list, and the DB comment on that function says the
 * same: what a port needs is four corners plus a CLOSING DUPLICATE,
 * UVs in texels and indices into the caller's vertex array. Our
 * MR_Primitive already has that shape, so this fills it directly
 * rather than reproducing the original's u16 pokes.
 *
 * READ OUT OF A LIVE PARTICLE rather than derived [verified]:
 *
 *     kind and effectiveKind  0x44
 *     colorIndex              0xffdf
 *     bankIndex               1        the constructor's last arg
 *     flags                   0
 *     flatColor               0x00f8f8f8
 *     vertexIndex             0,1,2,3,0  and  3,2,1,0,3
 *
 * 0xffdf IS Color_Pack16(0x80,0x80,0x80) SATURATED. The packer is
 * (r << 11) | (g << 6) | b on FIVE-bit channels, so 0x80 clamps to 31
 * in all three and 0xf800|0x7c0|0x1f is 0xffdf. Computing it from
 * 0x80 >> 3 gives 0x8410 and a visibly darker puff.
 *
 * AND THE ALPHA BYTE IS ZERO HERE, unlike the ribbon and the shadow,
 * which both read 0xff in that position. 0x00f8f8f8 is the plain
 * 5-5-5 unpack shifted left 3 with nothing added.
 */
/*
 * ORIGIN  NONE - ours. The original stores a literal bank index
 *         of 1; see bank_is_bike_atlas for why we search.
 *
 * Cached because it walks the bank table, and negative caching would
 * be wrong: the banks are loaded after fx_particle_reset_all runs, so
 * a miss early in a race must not stick.
 */

static int fx_particle_bank(void)
{
	int i;

	if (s_fxBank >= 0)
		return s_fxBank;

	for (i = 0; i < BANK_COUNT; i++) {
		const MR_Bank *b = bank_get(i);

		if (!b || !b->loaded || !b->page)
			continue;
		if (bank_is_bike_atlas(b->name)) {
			s_fxBank = i;
			mrlog("fx: particle atlas %s -> bank %d\n", b->name, i);
			return i;
		}
	}
	return -1;
}


/*
 * THE FLAT COLOUR IS THE PACKED ONE UNPACKED AND SHIFTED LEFT THREE,
 * with no alpha - 0xffdf gives 0x00f8f8f8, which is what a live smoke
 * particle reads. Computing it here rather than tabulating it keeps
 * the trail's four colours honest against their Color_Pack16 calls.
 */
static unsigned int fx_flat_from_packed(unsigned short c)
{
	unsigned int r = (unsigned int)((c >> 11) & 0x1f);
	unsigned int g = (unsigned int)((c >> 6) & 0x1f);
	unsigned int b = (unsigned int)(c & 0x1f);

	return ((r << 3) << 16) | ((g << 3) << 8) | (b << 3);
}

static void fx_prim_init_quad_kind(MR_Primitive *p, int i0, int i1,
                                   int i2, int i3, const float uv[8],
                                   int kind, unsigned short colour)
{
	int k;

	if (!p || !uv)
		return;
	memset(p, 0, sizeof *p);
	p->kind          = (unsigned short)kind;
	p->effectiveKind = (unsigned short)kind;
	p->colorIndex    = colour;
	p->flatColor     = fx_flat_from_packed(colour);
	p->bankIndex     = (unsigned short)fx_particle_bank();
	p->flags         = 0;

	for (k = 0; k < 8; k++)
		p->uv[k] = uv[k];
	/* THE FIFTH PAIR REPEATS THE FIRST - the closing corner. */
	p->uv[8] = uv[0];
	p->uv[9] = uv[1];

	p->vertexIndex[0] = (unsigned short)i0;
	p->vertexIndex[1] = (unsigned short)i1;
	p->vertexIndex[2] = (unsigned short)i2;
	p->vertexIndex[3] = (unsigned short)i3;
	p->vertexIndex[4] = (unsigned short)i0;
}


/*
 * BOTH QUADS, WOUND OPPOSITE WAYS. 0,1,2,3 then 3,2,1,0, which is
 * what makes the billboard DOUBLE-SIDED - a port relying on backface
 * culling alone loses half of every particle.
 */
static void fx_build_quads_kind(MR_FxParticle *p, const float uv[8],
                                int kind, unsigned short colour)
{
	fx_prim_init_quad_kind(&p->quad[0], 0, 1, 2, 3, uv, kind, colour);
	fx_prim_init_quad_kind(&p->quad[1], 3, 2, 1, 0, uv, kind, colour);
}

static void fx_build_quads(MR_FxParticle *p, const float uv[8])
{
	fx_build_quads_kind(p, uv, FX_PARTICLE_KIND, FX_PARTICLE_COLOUR);
}


/*
 * ORIGIN  maybe_Fx_InitParticle @ 0x004afe90
 *
 * The constructor. Its sole caller is the scan below, which hands it
 * the slot it found.
 *
 * THE LIFETIME IS STORED TWICE - an int that keeps the spawn value
 * and a float that counts down. Only the float is decremented.
 *
 * THE TWO QUADS ARE NOT BUILT HERE YET. The original calls
 * maybe_Prim_InitQuad twice with reversed vertex orders, 0,1,2,3 then
 * 3,2,1,0, which makes the billboard DOUBLE-SIDED - backface culling
 * alone would make half the spray vanish. That is draw-side and waits
 * on the same read as fx_draw_all_particles; the tick rebuilds both
 * quads every frame anyway, with DIFFERENT UVs from the ones written
 * here, so the spawn pair is never seen. [open]
 */
static void fx_init_particle(MR_FxParticle *p, int size,
                             const float pos[3], const float vel[3],
                             const float rotRate[3], int life,
                             unsigned short colour, int kind)
{
	float s2;
	int   i;

	(void)colour;
	(void)kind;                     /* the original hardcodes 5 */

	p->kind = 5;
	p->size = size;
	for (i = 0; i < 3; i++) {
		p->position[i]     = pos[i];
		p->velocity[i]     = vel[i];
		p->rotation[i]     = 0.0f;
		p->rotationRate[i] = rotRate[i];
	}
	p->lifeSpawned = life;
	p->life        = (float)life;

	/* The four corners, +-2 * size with Z = 0. */
	s2 = (float)(size * 2);
	p->corners[0][0] = -s2; p->corners[0][1] =  s2; p->corners[0][2] = 0.0f;
	p->corners[1][0] =  s2; p->corners[1][1] =  s2; p->corners[1][2] = 0.0f;
	p->corners[2][0] =  s2; p->corners[2][1] = -s2; p->corners[2][2] = 0.0f;
	p->corners[3][0] = -s2; p->corners[3][1] = -s2; p->corners[3][2] = 0.0f;

	fx_build_quads(p, s_fxUvSpawn);
	p->inUse = 1;
}


/* ORIGIN  maybe_Fx_SpawnParticle @ 0x00457620 */
void fx_spawn_particle(int size, const float pos[3], const float vel[3],
                       const float rotRate[3], int life,
                       unsigned short colour, int kind)
{
	int i;

	if (!pos || !vel || !rotRate)
		return;

	for (i = 0; i < FX_PARTICLE_COUNT; i++) {
		if (s_particles[i].inUse == 0) {
			fx_init_particle(&s_particles[i], size, pos, vel,
			                 rotRate, life, colour, kind);
			return;
		}
	}
	/* Full pool - the original returns having done nothing. */
}


/*
 * ORIGIN  maybe_Racer_EmitWheelFx @ 0x00466d70, its
 *         ((v ^ s) - s & M ^ s) - s idiom
 *
 * Moved here from racer.c when fx_burst turned out to need the same
 * thing. See fx.h.
 */
int fx_sext(int v, unsigned int mask)
{
	int s = v >> 31;                    /* 0 or -1 */
	int a = (v ^ s) - s;                /* |v| */

	return ((a & (int)mask) ^ s) - s;
}

/*
 * THE BURST'S CONSTANTS ARE THE SPRAY'S, and two of the three are
 * literally the same address - so racer.c's FX_ROT_SCALE and
 * FX_SPRAY_LIFT name the same bytes these do. They are declared twice
 * because the ORIGINAL has two independent emitters reading them, and
 * collapsing that here would assert a shared definition the binary
 * does not have.
 */
#define FX_BURST_ROT_SCALE  0.03125   /* 0x004d1150, a DOUBLE = 1/32  */
#define FX_BURST_LIFT         10.0f   /* 0x004d10a0, a FLOAT          */
#define FX_BURST_COLOUR     0x2104u   /* Color_Pack16(4, 4, 4)        */

/*
 * ORIGIN  maybe_Fx_Burst @ 0x00467c40
 *
 * TWO CALL SITES, BOTH IN THE CRASH [verified], read off their pushes:
 *
 *     0x004681b3  racer_begin_crash   6 at position,      matOrientation
 *     0x00468708  racer_update_crash  4 at riderPosition, matRiderRoot
 *
 * so six puff at the bike as it goes down and four more where the
 * rider lands, the second only when crashLaunchDir.y clears 0.05.
 *
 * THE ROTATION RATES ARE DRAWN ONCE, BEFORE THE LOOP. Every particle
 * in one burst therefore spins together, and only the offset, the
 * size and the life differ - which is what makes a burst read as one
 * puff rather than as a firework. THE WHEEL SPRAY IS THE OPPOSITE and
 * draws its rates per particle; do not copy one to the other.
 *
 * THE OFFSET IS A 0..3 CUBE IN THE CALLER'S FRAME, so the dust
 * spreads along the bike's own axes and a machine going down sideways
 * throws it sideways. ALL THREE ROWS COUNT HERE, unlike the spray,
 * which multiplies the middle row by zero.
 *
 * THE THREE COEFFICIENTS ARE CONSUMED IN REVERSE of the order they
 * are drawn - the last draw scales row0 - exactly as the shared rates
 * are. Getting that backwards still looks like dust, so it will not
 * show up in a screenshot; it shows up in a pool diff.
 *
 * THE Y LIFT IS APPLIED ONCE, to the argument, before the loop, so
 * every particle in the burst shares it.
 */
void fx_burst(int count, const float pos[3], int lifeBase,
              unsigned int lifeMask, const float m[12])
{
	float rot[3];
	float base[3];
	int   r1, r2, r3;
	int   i, k;

	if (!pos || !m)
		return;

	/*
	 * STREAM 1, NOT THE CRT rand(). maybe_Fx_Burst calls maybe_Rand
	 * (0x00458cd0) at eight sites and that is Rand_Next(stream 1);
	 * the simulation's draws go through Game_RandNext (0x00458ce0),
	 * which is the CRT generator. Drawing effects from the
	 * simulation's stream is what makes a replay stop matching -
	 * rand.h.
	 */
	r1 = rand_next(1);
	r2 = rand_next(1);
	r3 = rand_next(1);
	rot[0] = (float)fx_sext(r3, 0x1f) * (float)FX_BURST_ROT_SCALE;
	rot[1] = (float)fx_sext(r2, 0x1f) * (float)FX_BURST_ROT_SCALE;
	rot[2] = (float)fx_sext(r1, 0x1f) * (float)FX_BURST_ROT_SCALE;

	base[0] = pos[0];
	base[1] = pos[1] + FX_BURST_LIFT;
	base[2] = pos[2];

	for (i = 0; i < count; i++) {
		float off[3];
		float c1, c2, c3;
		int   size;
		int   life;

		/* 40 or 80 - ((rand & 1) * 5 + 5) * 8 at 0x00467d06. */
		size = ((rand_next(1) & 1) * 5 + 5) * 8;

		c1 = (float)(rand_next(1) & 3);
		c2 = (float)(rand_next(1) & 3);
		c3 = (float)(rand_next(1) & 3);
		for (k = 0; k < 3; k++)
			off[k] = m[k] * c3 + m[3 + k] * c2
		           + m[6 + k] * c1 + m[9 + k];

		life = (int)((unsigned int)rand_next(1) & lifeMask) + lifeBase;

		fx_spawn_particle(size, base, off, rot, life,
		                  (unsigned short)FX_BURST_COLOUR, 5);
	}
}

/* ------------------------------------------------------------------ */
/* THE TYRE TRAIL - kinds 1..4, the dirt a wheel leaves off the road   */
/* ------------------------------------------------------------------ */

/*
 * ORIGIN  FUN_004b00e0 @ 0x004b00e0
 *
 * The kinds-1..4 constructor, and the SECOND initialiser that writes
 * this pool - which is why "nothing spawns kinds 1..4" was wrong. The
 * kind is the SURFACE ID from Trk_SurfaceAtPoint, and the switch has
 * an arm for 1, 2, 3 and 4 only. SURFACE 0 IS THE ROAD: it takes the
 * default, returns without setting inUse, and the slot stays free -
 * which is exactly why the specks stop on tarmac.
 *
 * A DIFFERENT PRIMITIVE KIND FROM THE SMOKE, 0x26 against 0x44, and
 * its own colour per surface.
 *
 * THE TWO GREYS THE CALLER PASSES ARE NEVER READ. maybe_Racer_EmitWheelFx
 * builds Color_Pack16(9,9,9) and (8,8,8) and hands them over, and the
 * body uses the switch's colour instead. Reproduced, not tidied.
 *
 * SIZE IS HALF THE COUNT ARGUMENT and the corner half-width half of
 * that again - so the speck really is only a few texels across.
 */
static void fx_init_trail(MR_FxParticle *p, int size,
                          const float pos[3], const float dir[3],
                          const float rot[3], float speed, int life,
                          int kind)
{
	static const float kTrailUv[4][8] = {
		{ 113.0f, 174.0f, 186.0f, 174.0f,
		  186.0f, 230.0f, 113.0f, 230.0f },   /* 1 */
		{   2.0f, 109.0f, 108.0f, 109.0f,
		  108.0f, 206.0f,   2.0f, 206.0f },   /* 2 */
		{ 113.0f, 109.0f, 186.0f, 109.0f,
		  186.0f, 170.0f, 113.0f, 170.0f },   /* 3 */
		{ 133.0f,  43.0f, 216.0f,  43.0f,
		  216.0f, 105.0f, 133.0f, 106.0f }    /* 4 */
	};
	/* Color_Pack16(r, g, b) = (r << 11) | (g << 6) | b, 5 bits each. */
	static const unsigned short kTrailColour[4] = {
		(9u << 11) | (8u << 6) | 6u,          /* 1  brown  */
		(0x14u << 11) | (0x0du << 6) | 5u,    /* 2  sand   */
		(0u << 11) | (0x0bu << 6) | 4u,       /* 3  green  */
		(0x13u << 11) | (0x14u << 6) | 0x15u  /* 4  pale   */
	};
	float half;
	int   i;

	if (!p || kind < 1 || kind > 4)
		return;                 /* the default arm - nothing at all */

	p->size        = size / 2;
	p->sizeSpawned = size;
	half           = (float)((size / 2) / 2);

	p->kind = kind;
	for (i = 0; i < 3; i++) {
		p->position[i]     = pos[i];
		p->velocity[i]     = dir[i];
		p->rotation[i]     = 0.0f;
		p->rotationRate[i] = rot ? rot[i] : 0.0f;
	}
	p->speed       = speed;
	p->lifeSpawned = life;
	p->life        = (float)life;

	/*
	 * NOT THE SMOKE'S CORNERS. This quad stands on y = 0 and rises to
	 * `speed`, rather than being centred on the origin - the tick
	 * rebuilds it from `height` every frame anyway.
	 */
	p->corners[0][0] = -half; p->corners[0][1] = speed;
	p->corners[1][0] =  half; p->corners[1][1] = speed;
	p->corners[2][0] =  half; p->corners[2][1] = 0.0f;
	p->corners[3][0] = -half; p->corners[3][1] = 0.0f;
	for (i = 0; i < 4; i++)
		p->corners[i][2] = 0.0f;

	fx_build_quads_kind(p, kTrailUv[kind - 1], FX_TRAIL_KIND,
	                    kTrailColour[kind - 1]);

	p->height = 1;
	p->inUse  = 1;
}

/*
 * ORIGIN  maybe_Fx_SpawnTrail @ 0x004576a0
 *
 * A linear scan of the same pool fx_spawn_particle walks, and a full
 * pool silently drops the request exactly as that one does.
 */
void fx_spawn_trail(int size, const float pos[3], const float dir[3],
                    const float rot[3], float speed, int life,
                    unsigned short greyA, unsigned short greyB,
                    int kind)
{
	int i;

	(void)greyA;
	(void)greyB;            /* passed by the original, never read */

	if (!pos || !dir)
		return;

	for (i = 0; i < FX_PARTICLE_COUNT; i++) {
		if (s_particles[i].inUse == 0) {
			fx_init_trail(&s_particles[i], size, pos, dir, rot,
			              speed, life, kind);
			return;
		}
	}
}

/*
 * ORIGIN  maybe_Fx_TickParticle @ 0x004b0730, the kinds 1..4 arm
 *
 * THE SPECK SPREADS AND THEN COLLAPSES. Two counters move together:
 * `size` is the quad's width and `height` its top edge.
 *
 *     while  sizeSpawned * 2 >= size  and  size <= 0x100
 *         both += dt * 3.84        0x004d202c, growing
 *     otherwise
 *         height -= dt * 1.28      0x004d2030, collapsing
 *         size   += dt * 1.28      and it keeps spreading
 *
 * BOTH ARE TRUNCATED TO INT every frame by the same __ftol the rest
 * of this file uses. THE PARTICLE DIES WHEN height GOES NEGATIVE -
 * there is no life countdown on this arm at all, unlike kind 5.
 *
 * THE BALLISTICS ARE THE CRASH'S, not the smoke's: the direction is
 * kept normalised and the magnitude in `speed`, gravity is folded in
 * and the pair renormalised each frame.
 *
 *     v      = dir * speed;  v.y += dt * -0.024525      0x004d2038
 *     speed  = |v|;  dir = v / speed
 *     pos   += dir * speed * dt * 0.01 * 128.0   0x004d2048, 0x004d2050
 */
static void fx_tick_trail(MR_FxParticle *p, float dt)
{
	float v[3];
	float step;
	float half;
	int   i;

	if (p->sizeSpawned * 2 < p->size || p->size > 0x100) {
		float k = dt * FX_TRAIL_SHRINK;

		p->height = (int)((float)p->height - k);
		p->size   = (int)((float)p->size + k);
	} else {
		float k = dt * FX_TRAIL_GROW;

		p->size   = (int)((float)p->size + k);
		p->height = (int)((float)p->height + k);
	}

	if (p->height < 0) {
		p->inUse = 0;
	} else {
		half = (float)(p->size / 2);
		p->corners[0][0] = -half;
		p->corners[0][1] = (float)p->height;
		p->corners[1][0] =  half;
		p->corners[1][1] = (float)p->height;
		p->corners[2][0] =  half;
		p->corners[2][1] = 0.0f;
		p->corners[3][0] = -half;
		p->corners[3][1] = 0.0f;
		for (i = 0; i < 4; i++)
			p->corners[i][2] = 0.0f;
	}

	for (i = 0; i < 3; i++)
		v[i] = p->velocity[i] * p->speed;
	v[1] = dt * (float)FX_TRAIL_GRAVITY + v[1];

	p->speed = (float)sqrt((double)(v[1] * v[1] + v[0] * v[0]
	                                + v[2] * v[2]));
	/*
	 * NO GUARD ON THE DIVIDE, as the original has none. speed cannot
	 * reach zero here: gravity puts a non-zero y in every frame.
	 */
	for (i = 0; i < 3; i++)
		p->velocity[i] = v[i] / p->speed;

	step = p->speed * dt * (float)FX_TRAIL_DT_SCALE
	       * (float)FX_TRAIL_MOVE_SCALE;
	for (i = 0; i < 3; i++)
		p->position[i] += p->velocity[i] * step;
}

/*
 * ORIGIN  maybe_Fx_TickParticle @ 0x004b0730, the kind-5 arm
 *
 * THE SIZE GROWS AND THE CORNERS LAG IT BY ONE STEP. 0x004b094b reads
 * the size, 0x004b095e takes EDI = 2*size from THAT value, and only
 * then does 0x004b0974 store size + dt*0.6 back. So a puff EXPANDS as
 * it fades and the corners are built from the PRE-increment size.
 *
 * MEASURED: dt is about 3.3, so the growth is 1.98 a tick and
 * truncation makes it +1. Reading one slot every second tick gave
 * size 16, 18, 20 with corners -30, -34, -38 - which is 2*(size-1)
 * and looks like an off-by-one until you see the store order.
 *
 * ONLY GRAVITY IS INTEGRATED. X and Z are damped by a plain per-frame
 * multiply and never accelerated, so a burst drifts with its launch
 * velocity, sinks and stops. There is no ground collision: a particle
 * spawned below the road falls through it.
 */
void fx_tick_particle(MR_FxParticle *p, float dt)
{
	int   sizeOld;
	float s2;
	int   i;

	if (!p || p->kind <= 0)
		return;

	if (p->kind < 5) {
		/*
		 * KINDS 1..4 ARE THE TYRE TRAIL, and the kind is the
		 * surface id. fx_spawn_trail is what puts them here.
		 */
		fx_tick_trail(p, dt);
		return;
	}
	if (p->kind != 5)
		return;

	sizeOld = p->size;
	p->size = (int)((double)sizeOld + (double)dt * FX_SIZE_GROW);

	s2 = (float)(sizeOld * 2);
	p->corners[0][0] = -s2; p->corners[0][1] =  s2; p->corners[0][2] = 0.0f;
	p->corners[1][0] =  s2; p->corners[1][1] =  s2; p->corners[1][2] = 0.0f;
	p->corners[2][0] =  s2; p->corners[2][1] = -s2; p->corners[2][2] = 0.0f;
	p->corners[3][0] = -s2; p->corners[3][1] = -s2; p->corners[3][2] = 0.0f;

	fx_build_quads(p, s_fxUvTick);

	p->velocity[1] = (float)((double)p->velocity[1]
	                         + (double)dt * FX_GRAVITY);
	p->position[1] += p->velocity[1] * dt;
	p->velocity[0] = (float)((double)p->velocity[0] * FX_DRAG);
	p->position[0] += p->velocity[0] * dt;
	p->velocity[2] = (float)((double)p->velocity[2] * FX_DRAG);
	p->position[2] += p->velocity[2] * dt;

	for (i = 0; i < 3; i++)
		p->rotation[i] += p->rotationRate[i];

	p->life -= dt;

	/*
	 * The original tests the float's SIGN BIT - CMP 0x80000000 / JBE
	 * - so -0.0 does NOT free the slot. A plain < 0.0f agrees.
	 */
	if (p->life < 0.0f)
		p->inUse = 0;
}


/* ORIGIN  maybe_Fx_TickAllParticles @ 0x00457590 */
void fx_tick_all_particles(float dt)
{
	int i;

	for (i = 0; i < FX_PARTICLE_COUNT; i++) {
		if (s_particles[i].inUse)
			fx_tick_particle(&s_particles[i], dt);
	}
}



/*
 * ORIGIN  maybe_Fx_DrawParticle @ 0x004b0be0, the kind-5 arm at
 *         0x004b0dae, and maybe_Fx_DrawAllParticles @ 0x004575c0
 *
 * THE BILLBOARD FRAME, read off the x87 stack and then measured at
 * the call - fx-particles.txt 7.4:
 *
 *     frame        = RotY(camera[0xc0] * K2080)
 *     frame.transl = particle.pos - camera.pos
 *     M            = frame * cameraView
 *     verts        = TransformVertsFlat(M, corners, 4, 0x1f)
 *                    CullAndQueueFacesEx(verts, quad, 2, 1)
 *
 * THE ROTATION IS A REAL BILLBOARD, and camera+0xc0 is the CAMERA'S
 * YAW - see camera_view_yaw. The frame collapses to a pure Y turn by
 * that angle: the compiler emitted a three-axis euler build whose
 * other two angles are the constants g_cosTable[0] = 1.0 and
 * g_cosTable[0x3000] = 0.0, so nearly every term vanishes.
 *
 *     row0  ( cos, 0, -sin)   row1  (0, 1, 0)   row2  ( sin, 0, cos)
 *
 * BECAUSE camera+0xc0 IS THE NEGATED HEADING, that turn CANCELS the
 * view's yaw: the quad ends up facing the camera with only the pitch
 * left in. Captured at 0x004b0ed9 with the camera 30 degrees off
 * axis, the composed matrix came back as
 *
 *     ( 1.00000 -0.00001 -0.00007)
 *     ( 0.00000  0.99623 -0.08677)     the view's pitch, alone
 *     ( 0.00007  0.08677  0.99623)
 *
 * [verified]. An earlier capture found this matrix equal to the view
 * matrix unchanged and concluded the turn was identity - that sample
 * was taken looking straight down the track, where the yaw is 0 and
 * every billboard is degenerate. LEAVING IT AS IDENTITY MAKES SMOKE
 * GO EDGE-ON as the camera swings, and vanish near 90 degrees.
 *
 * THE EYE IS FOLDED INTO THE VIEW, as fx_ribbon_draw_all does, which
 * is the same arithmetic as the original's separate translation: the
 * corners are MODEL space, so translating by pos - eye and rotating
 * equals rotating (pos - eye) and adding the view's own translation.
 *
 * DETAIL LEVEL GATES IT, and the counter is GLOBAL rather than per
 * particle - so at level 2 it is every eighth PARTICLE across the
 * whole pool, not every eighth frame of one.
 */
static int s_fxDrawCounter;         /* DAT_004f258c */

int fx_draw_all_particles(const float camView[12], const float camEye[3],
                          float camYaw, int detailLevel)
{
	float view[12];
	float frame[12];
	int   slot, i, queued = 0;
	int   period;
	int   ai;

	if (!camView || !camEye)
		return 0;
	if (detailLevel <= 1)
		return 0;                   /* nothing is drawn at all */
	period = (detailLevel < 3) ? 8 : 1;

	/*
	 * THE BILLBOARD FRAME, built once - it depends on the camera and
	 * not on the particle. Only the translation moves per slot.
	 *
	 * The index is a TRUNCATION, the __ftol at 0x004b3048, and the
	 * table is the game's own - so this turns by the same quantised
	 * angle the original does rather than by a libm cosine.
	 */
	ai = (int)(camYaw * TRIG_IDX_PER_RAD);
	for (i = 0; i < 12; i++)
		frame[i] = 0.0f;
	frame[0] =  trig_cos(ai);
	frame[2] = -trig_sin(ai);
	frame[4] =  1.0f;
	frame[6] =  trig_sin(ai);
	frame[8] =  trig_cos(ai);

	for (i = 0; i < 9; i++)
		view[i] = camView[i];
	for (i = 0; i < 3; i++)
		view[9 + i] = camView[9 + i]
	                - (camEye[0] * camView[i]
	                   + camEye[1] * camView[3 + i]
	                   + camEye[2] * camView[6 + i]);

	for (slot = 0; slot < FX_PARTICLE_COUNT; slot++) {
		MR_FxParticle *p = &s_particles[slot];
		MR_Vertex     *v;
		float          m[12];

		/*
		 * KINDS 1..4 DRAW THROUGH THE SAME BLOCK. 0x004b0c44 sends
		 * kind 5 to a DUPLICATE of the arm at 0x004b0c52 - the two
		 * are instruction for instruction the same billboard - so
		 * one path serves both families here.
		 */
		if (!p->inUse || p->kind <= 0 || p->kind > 5)
			continue;
		if (p->quad[0].bankIndex == 0xffff)
			continue;           /* no atlas - fx_particle_bank */

		s_fxDrawCounter++;
		if (s_fxDrawCounter < period)
			continue;
		s_fxDrawCounter = 0;

		/*
		 * frame.transl IS THE PARTICLE POSITION, not pos - eye,
		 * because `view` already carries the eye folded in - see
		 * above. The product is the same either way.
		 */
		frame[9]  = p->position[0];
		frame[10] = p->position[1];
		frame[11] = p->position[2];
		mat34_multiply(m, frame, view);

		v = transform_verts_flat(m, &p->corners[0][0], 4,
		                         FX_PARTICLE_SHADE);
		if (!v)
			continue;
		queued += cull_and_queue_faces_ex(v, &p->quad[0], 2, 1);
	}
	return queued;
}


/* ------------------------------------------------------------------ */
/* THE SKID RIBBONS                                                    */
/* ------------------------------------------------------------------ */

/*
 * ORIGIN  g_fxRibbons @ 0x00542a28
 *
 * Sixteen, which is two per racer across the eight-bike field.
 */
static MR_FxRibbon s_ribbons[FX_RIBBON_COUNT];

/* ORIGIN  g_nFxNext - the ring cursor maybe_Fx_AllocSlot walks. */
static int s_ribbonNext;

/*
 * ORIGIN  FUN_00457120 @ 0x00457120, its Color_Pack16(8, 8, 8)
 *
 * 0x4208. Color_Pack16 is (r << 11) | (g << 6) | b and is 5-5-5
 * positioned on a 565 surface, NOT true 565 - green gets five bits at
 * shift 6 and bit 5 is never written. See pixel-format.txt.
 *
 * Face_InitTriangle expands it to flatColor 0x404040, a dark grey,
 * by the same 5-5-5 unpack the shadow's constructor uses.
 */
#define FX_RIBBON_COLOUR   0x4208u
#define FX_RIBBON_KIND     0x0c

/*
 * THE MARK IS NOT TEXTURED. Kind 0x0c is one of the three
 * prim_is_textured rejects outright, so a ribbon draws as its flat
 * colour and nothing else - no bank, no UVs. That is why the record
 * has no texture fields to fill.
 */

/*
 * ORIGIN  Face_InitTriangle @ 0x00422be0
 *
 * flatColor is derived, not chosen: the constructor unpacks the
 * 5-5-5 colourIndex and shifts left 3, so 0x4208 becomes 0x404040.
 * Written out rather than hardcoded so the colour and the fill stay
 * one fact.
 */
void fx_face_init_triangle(MR_Primitive *p, int i0, int i1, int i2)
{
	unsigned int c = FX_RIBBON_COLOUR;
	unsigned int r = (c >> 11) & 0x1f;
	unsigned int g = (c & 0x7c0) >> 6;
	unsigned int b = c & 0x1f;

	if (!p)
		return;
	memset(p, 0, sizeof *p);
	p->kind          = FX_RIBBON_KIND;
	p->effectiveKind = FX_RIBBON_KIND;
	p->colorIndex    = (unsigned short)c;
	/*
	 * ALPHA 0xff, AND THE CONSTRUCTOR DOES NOT PUT IT THERE.
	 * Face_InitTriangle writes a bare 0x00404040 from the 5-5-5
	 * unpack, but MOTO's live ribbons all read 0xff404040 - so
	 * something after it fills the byte at +0x3f. Read out of the
	 * running game rather than derived, exactly as the shadow's
	 * 0xff000000 was.
	 *
	 * WITHOUT IT THE MARK IS TOO PALE. Same field, same symptom and
	 * same cause as racerdraw_init_shadow. [open - who sets it]
	 */
	p->flatColor     = 0xff000000u
	                 | (((r << 16) | (g << 8) | b) << 3);
	p->vertexIndex[0] = (unsigned short)i0;
	p->vertexIndex[1] = (unsigned short)i1;
	p->vertexIndex[2] = (unsigned short)i2;
	p->vertexIndex[3] = (unsigned short)i0;
	p->vertexIndex[4] = PRIM_NO_WRAP;
	p->bankIndex      = 0;
	p->flags          = 0;
}


/* ORIGIN  FUN_00457120 @ 0x00457120 */
void fx_ribbon_reset(int slot)
{
	MR_FxRibbon *r;
	int          i;

	if (slot < 0 || slot >= FX_RIBBON_COUNT)
		return;
	r = &s_ribbons[slot];
	memset(r, 0, sizeof *r);

	/*
	 * THIRTY-TWO ITERATIONS, TWO TRIANGLES EACH, and the indices
	 * advance by two - a triangle strip laid over the vertex pairs
	 * the appender writes:
	 *
	 *     Face_InitTriangle(0x0c, c, i,     i + 1, i + 2)
	 *     Face_InitTriangle(0x0c, c, i + 1, i + 3, i + 2)
	 *
	 * so the 64 faces cover all 128 vertices with no gaps.
	 */
	for (i = 0; i < FX_RIBBON_PRIMS / 2; i++) {
		int           v = i * 2;
		MR_Primitive *a = &r->prim[i * 2 + 0];
		MR_Primitive *b = &r->prim[i * 2 + 1];

		fx_face_init_triangle(a, v + 0, v + 1, v + 2);
		fx_face_init_triangle(b, v + 1, v + 3, v + 2);
	}

	r->quadCount   = 0;
	r->vertexCount = 0;
}


/* ORIGIN  FUN_00457300 @ 0x00457300 */
void fx_ribbon_reset_all(void)
{
	int i;

	for (i = 0; i < FX_RIBBON_COUNT; i++)
		fx_ribbon_reset(i);
}


/*
 * ORIGIN  maybe_Fx_UpdateRibbon @ 0x00457390
 *         maybe_Fx_AppendRibbonQuad @ 0x00457200
 *
 * The wrapper only bounds-checks the slot and resolves the record;
 * the appender is the whole of the work, so the two are one function
 * here.
 *
 * EACH CALL RE-LOOKS-UP THE SEGMENT for both points. segHint is a
 * search start, not an answer, and the original does not trust it.
 *
 * THE Y VALUES ARE THROWN AWAY. The caller passes the wheel contact
 * plus a lateral offset; both are replaced with the track height
 * there plus 2.0. The 2.0 is a z-fight lift, not a physical offset -
 * _DAT_004d0dcc.
 *
 * THE QUAD COUNT LAGS THE VERTICES BY ONE PAIR, because a quad needs
 * a previous pair to build against. That is what makes 62 quads and
 * 128 vertices agree: 62 * 2 + 2 = 126, and the last pair is written
 * but never covered.
 */
void fx_ribbon_append(int slot, const float a[3], const float b[3],
                      int segHint, const MR_Trk *trk)
{
	MR_FxRibbon *r;
	float        pa[3], pb[3];
	int          seg, i;

	if (slot < 0 || slot >= FX_RIBBON_COUNT)
		return;
	r = &s_ribbons[slot];
	if (r->quadCount >= FX_RIBBON_QUAD_MAX)
		return;
	/* g_nTrkSegmentCount == 0 in the original. */
	if (!trk || !trk->segments || trk->segmentCount <= 0)
		return;
	if (!a || !b)
		return;
	if (r->vertexCount + 2 > FX_RIBBON_VERTS)
		return;

	for (i = 0; i < 3; i++) {
		pa[i] = a[i];
		pb[i] = b[i];
	}
	seg   = trk_find_segment_for_point(trk, segHint, pa, NULL);
	pa[1] = trk_ground_height_at(trk, seg, pa) + FX_RIBBON_LIFT;
	seg   = trk_find_segment_for_point(trk, segHint, pb, NULL);
	pb[1] = trk_ground_height_at(trk, seg, pb) + FX_RIBBON_LIFT;

	for (i = 0; i < 3; i++)
		r->vert[r->vertexCount][i] = pa[i];
	r->vertexCount++;
	for (i = 0; i < 3; i++)
		r->vert[r->vertexCount][i] = pb[i];
	r->vertexCount++;

	if (r->vertexCount > 2)
		r->quadCount += 2;
}


/*
 * ORIGIN  maybe_Fx_AllocSlot @ 0x00457350
 *
 * THE DECOMPILER CALLS IT void AND IT IS NOT - the index is left in
 * EAX and the caller stores it straight into racer+0x2f71.
 *
 * It clears only +0x1600 and +0x1604, the two counts. The faces are
 * not rebuilt, because FUN_00457120 laid them out once and they do
 * not depend on the data.
 */
int fx_ribbon_alloc_slot(void)
{
	int i = s_ribbonNext;

	s_ribbonNext++;
	if (s_ribbonNext > FX_RIBBON_COUNT - 1)
		s_ribbonNext = 0;
	s_ribbons[i].quadCount   = 0;
	s_ribbons[i].vertexCount = 0;
	return i;
}


/*
 * ORIGIN  FUN_004573e0 @ 0x004573e0
 *
 * The view it builds is the camera's 3x3 with the eye folded into the
 * translation - the same shape racerdraw_shadow builds, and for the
 * same reason: the ribbon's vertices are already in world space.
 *
 * A RIBBON WITH NO QUADS IS SKIPPED ENTIRELY, which is the original's
 * `*piVar4 != 0 && 0 < *piVar4` - a doubled test that reads as
 * belt-and-braces on a count that cannot be negative.
 */
int fx_ribbon_draw_all(const float camView[12], const float camEye[3])
{
	float view[12];
	int   slot, i, queued = 0;

	if (!camView || !camEye)
		return 0;

	for (i = 0; i < 9; i++)
		view[i] = camView[i];
	for (i = 0; i < 3; i++)
		view[9 + i] = camView[9 + i]
	                - (camEye[0] * camView[i]
	                   + camEye[1] * camView[3 + i]
	                   + camEye[2] * camView[6 + i]);

	for (slot = 0; slot < FX_RIBBON_COUNT; slot++) {
		MR_FxRibbon *r = &s_ribbons[slot];
		MR_Vertex   *v;

		if (r->quadCount <= 0)
			continue;
		v = transform_verts_flat(view, &r->vert[0][0],
		                         r->vertexCount, FX_RIBBON_SHADE);
		if (!v)
			continue;
		queued += cull_and_queue_faces_ex(v, &r->prim[0],
		                                  r->quadCount, 1);
	}
	return queued;
}


/* ORIGIN  NONE - ours. For the harness. */
const MR_FxRibbon *fx_ribbon_at(int slot)
{
	if (slot < 0 || slot >= FX_RIBBON_COUNT)
		return NULL;
	return &s_ribbons[slot];
}
