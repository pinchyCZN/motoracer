/*
 * level.h - the per-track manifest, out of "<track>Level.Spt".
 *
 * THIS IS THE FILE THE WHOLE RACE HANGS OFF. Page 1's tick builds the
 * name with sprintf("%sLevel.Spt", trackPath) and hands it to
 * FUN_00427e80, which reads 194-odd positional values naming every
 * other asset a race needs and every tuning constant the AI uses.
 * Nothing about a race can be loaded until this is read.
 *
 * o .Spt IS .BPT ON DISK, AND WE NAME THE .BPT OURSELVES. The
 *   ORIGINAL asks for "data\ile01\Level.Spt" and its Script_Open
 *   uppercases the name and swaps the extension for BPT. bpt_open
 *   here deliberately does NOT do that - bpt.h says so outright and
 *   tells callers to "name the file they actually want" - so
 *   level_load must be handed "ILE01\\Level.Bpt", which under rule 10
 *   is a member of ILE01.BKF.
 *
 *   THIS COMMENT USED TO CLAIM bpt_open REWROTE IT. It does not, and
 *   the file that says so is the one this includes. The call site was
 *   passing "Level.Spt" and every load failed with "cannot find
 *   ILE01\\LEVEL.SPT" - a wrong claim written from the tooling's
 *   docstring rather than from our own header.
 *
 * o THE FILE HAS NO NAMES IN IT. Values are positional and the names
 *   below exist only in the reader. That is why the order in level.c
 *   is load-bearing and why it is checked rather than trusted - see
 *   tools/level_spt.py, which holds the same order and verifies it
 *   against the shipped files.
 *
 * THE TWO SELF-CHECKS THAT MAKE THIS FALSIFIABLE, and they are the
 * reason the read order can be believed:
 *
 *     NbComputer   is followed by exactly that many ComputerSpeed
 *     NbCheckPoint is followed by exactly that many TimeCheckPoint
 *
 * Both counts come out of the file, so a wrong order lands the count
 * on the wrong value and the stream ends early or overruns. All eight
 * shipped islands consume to exactly the last record, and they do it
 * at THREE DIFFERENT LENGTHS - 194, 195 and 196 - because those two
 * counts vary per level. A fixed-length misreading cannot do that.
 * [verified]
 *
 * WHAT IS NOT HERE: nothing reads these fields yet. This is the first
 * step towards the starting grid and it is deliberately inert - a
 * parser with no consumer, verifiable on its own, before anything
 * tries to draw a track.
 */

#ifndef MRACER_LEVEL_H
#define MRACER_LEVEL_H

#include "tracks.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LEVEL_NAME_MAX   64

/*
 * TWENTY BANK SLOTS, and the count is the reader's not the file's.
 * FUN_00427e80 walks a fixed table - local_20 advances 0x40 bytes a
 * turn and stops at 0x51c9e7 - so the manifest always describes 20
 * texture banks whether it uses them or not. Unused ones are the
 * string "NULL" [verified, and every shipped island has 20].
 */
#define LEVEL_BANKS      20

/* Bounds for the two counted runs. The original has no limit and
 * writes into fixed arrays; ours refuses a file that would overrun. */
#define LEVEL_MAX_CPU    32
#define LEVEL_MAX_CP     32

/* Nine "zone" keys, each one value plus four per-detail-level. */
#define LEVEL_ZONE_LODS  5

/* Thirteen AI tuning keys, three floats each. */
#define LEVEL_TUNING     13

typedef struct {
	char page[LEVEL_NAME_MAX];      /* bank\bnk00_01.raw          */
	char pageD3D[LEVEL_NAME_MAX];   /* bank\bnkD3D01.raw or NULL  */
	int  flags;
} MR_LevelBank;

typedef struct {
	/* The asset names, in the reader's order. */
	char land[LEVEL_NAME_MAX];      /* ile01.trk - the track mesh  */
	char frames3d[LEVEL_NAME_MAX];
	char decor[LEVEL_NAME_MAX];
	char decorInv[LEVEL_NAME_MAX];
	char net[LEVEL_NAME_MAX];
	char map[LEVEL_NAME_MAX];
	char moto[LEVEL_NAME_MAX];      /* data\motoSpd.spt, the bikes */
	char pocket[LEVEL_NAME_MAX];    /* data\PktSpeed.spt           */
	char balise[LEVEL_NAME_MAX];    /* balise.spt - the markers    */
	char smallMap[LEVEL_NAME_MAX];
	char bank[LEVEL_NAME_MAX];      /* the bike's own bank page 0  */

	MR_LevelBank banks[LEVEL_BANKS];

	int   stepGlobal;
	int   yScaleLevel;

	/*
	 * THE STARTING GRID. Read with Settings_ReadInt and stored into
	 * floats, so they are whole numbers in the file and floats
	 * everywhere after [verified, the three (float) casts].
	 */
	float startPos[3];

	int   computeLandOptimise;

	/* The nine zone keys. Index 0 is the base value and 1..4 are the
	 * per-detail-level overrides. */
	float lenBPlane[LEVEL_ZONE_LODS];
	float lenRLPlane[LEVEL_ZONE_LODS];
	float flatZ[LEVEL_ZONE_LODS];
	float horizFlatZ[LEVEL_ZONE_LODS];
	int   mediumZone[LEVEL_ZONE_LODS];
	int   hightZone[LEVEL_ZONE_LODS];
	float gouraudTexturedZ[LEVEL_ZONE_LODS];
	float texturedZ[LEVEL_ZONE_LODS];
	float gouraudZ[LEVEL_ZONE_LODS];

	/* RATTRAPAGE/ATTENTE1/ATTENTE2 x SPEED/CURVE/COUPLE, then
	 * CURVE_RANGE, COUPLE_OUT_RANGE, COUPLE_IN_RANGE, SPEED_FACTOR. */
	float tuning[LEVEL_TUNING][3];

	int   nbComputer;
	float computerSpeed[LEVEL_MAX_CPU];

	int   timeSkill[3];

	int   nbCheckPoint;
	int   timeCheckPoint[LEVEL_MAX_CP];

	/*
	 * PARSED, NOT YET USED - and it is the pocket bike's only
	 * gameplay effect. maybe_Race_SetOpponentSpeeds at 0x0047bc60
	 * scales every OPPONENT's racer+0xb4 by 1/factorPocket when the
	 * pocket bike is selected, so the AI is rebalanced against the
	 * smaller bike rather than the bike being handled differently.
	 * Nothing here reads it because the port does not tick the AI.
	 * [open - wire it when the AI lands]
	 */
	float factorPocket;
	int   audioTrack;               /* the race's CD music track   */
} MR_Level;

/*
 * level_load - read "<trackPath>Level.Spt" into out.
 *
 * logicalName is the .BPT name - "ILE01\\Level.Bpt" - because
 * bpt_open does NOT rewrite the extension; see the note at the top.
 * Returns 0 and logs on any short read, bad count or overrun; out is
 * zeroed first either way, so a failed load leaves a defined struct
 * rather than a half-filled one.
 */
int level_load(MR_Level *out, const char *logicalName);

/*
 * level_load_banks - load the twenty texture pages into bank slots
 * base..base+19. Returns how many loaded. See level.c for the three
 * things FUN_00410590 settles that the manifest alone does not.
 */

int level_load_banks(const MR_Level *lv, const MR_Track *t,
                     int base);

/* Log the headline fields - the names, the grid, the counts. */
void level_log(const MR_Level *lv, const char *what);

#ifdef __cplusplus
}
#endif

#endif /* MRACER_LEVEL_H */
