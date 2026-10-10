/*
 * fontdef.h - the font manifest, read from DATA\FONTS.BPT.
 *
 * WHY THIS EXISTS: font ids are POSITIONAL AND LIVE IN DATA. Font 31
 * is DataDiskMenu because it is the 31st entry of Fonts.Spt, not
 * because anything in the executable says so - so a port that names
 * face files directly has hardcoded a lookup, and gets a different
 * font the moment it guesses wrong. Page 14 did exactly that once and
 * the comment there still says so.
 *
 * The schema is Font_LoadAll's read ORDER, transcribed - see
 * docs/structs/MR_Font.txt 5.1 for why a parser cannot do anything
 * else. A .bpt has no type tags.
 */

#ifndef MRACER_FONTDEF_H
#define MRACER_FONTDEF_H

#include "font.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Font_LoadAll rejects a count above 0x30 and the shipped file
 * declares 35. Four faces is MR_Font's own limit.
 */
#define FONTDEF_MAX        48
#define FONTDEF_FACES_MAX   4

typedef struct {
	char name[16];        /* ChronoLowRes, MenuLink, ...        */
	char cls[24];         /* Direct, AntiAlias8Transp, Pal, ... */
	char cond[24];        /* NoCondition, InGameHiRes, ...      */

	/* Each face covers one character range of one file. */
	char faceName[FONTDEF_FACES_MAX][64];   /* logical asset names */
	int  faceFirst[FONTDEF_FACES_MAX];
	int  faceLast[FONTDEF_FACES_MAX];
	int  faceCount;

	int           spacingA;
	int           spacingB;
	unsigned char fore[3];
	unsigned char back[3];
	int           hasFore;
	int           hasBack;

	int valid;
} MR_FontDef;

/*
 * fontdef_load - read the manifest once. Returns the font count, or 0.
 *
 * Idempotent: a second call returns the count already parsed.
 */
int fontdef_load(void);

/* The entry for an id, or NULL when it is out of range or absent. */
const MR_FontDef *fontdef_get(int id);

/*
 * fontdef_face - the logical asset name of face f of font id, or NULL.
 *
 * THE MANIFEST NAMES A FILE THAT IS NOT SHIPPED, so this is not the
 * string out of the file: FontData says
 * Data\Fontes\HiRes\MenuLink.Raw and FONTES.BKF holds
 * HIRES\MENULINK.FNT. The prefix is stripped - rule 10 wants a
 * logical name anyway - and the extension swapped, which is what
 * the original does too [verified - the ".FNT" literal lives
 * in the g_fontTable statics at 0x004f1840]. A path record is an
 * authoring-time name, not a member name. See MR_Font.txt 5.2.
 */
const char *fontdef_face(int id, int f);

/*
 * fontdef_open - build an MR_Font from the manifest entry for id.
 *
 * The class, every face and the colours come from the file, so a
 * caller names an ID and nothing else. 0 if the id is absent or its
 * class is one font.c does not implement.
 *
 * COLOURS ARE APPLIED ONLY WHERE THE MANIFEST CARRIES THEM. Those
 * sections are sparse - 17 foreground and 20 background entries
 * across 35 fonts - and a Pal face brings its own, so defaulting
 * the rest would be inventing a colour.
 */
int fontdef_open(MR_Font *f, int id);

/*
 * fontdef_resolve - the id Font_Get would actually use.
 *
 * Twelve LowRes/HiRes pairs, swapped when small fonts are off AND
 * the screen mode is 2. fontdef_open already applies it, so this is
 * only for a caller that wants to know.
 */
int fontdef_resolve(int id);

#ifdef __cplusplus
}
#endif

#endif /* MRACER_FONTDEF_H */
