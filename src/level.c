/*
 * level.c - "<track>Level.Spt", transcribed from FUN_00427e80.
 *
 * The read order IS the format; see level.h. Everything here is a
 * straight sequence of bpt reads in that order, with the two counted
 * runs bounded.
 *
 * THE ORDER IS CHECKED, NOT TRUSTED. tools/level_spt.py holds the same
 * sequence and runs it against every shipped island; all eight consume
 * to exactly the last record at three different file lengths. If this
 * file and that script ever disagree, one of them is wrong and the
 * script is the one that can be re-run in a second.
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "level.h"
#include "bpt.h"
#include "log.h"
#include "tracks.h"
#include "bank.h"

/* Every read goes through these so a short file stops the parse at the
 * first missing value rather than filling the rest with garbage. */
#define RD_INT(dst)   do { if (!bpt_read_int(&s, &(dst))) \
	                           goto shortfile; } while (0)
#define RD_FLOAT(dst) do { if (!bpt_read_float(&s, &(dst))) \
	                           goto shortfile; } while (0)
#define RD_STR(dst)   do { if (!bpt_read_string(&s, (dst), \
	                                            (int)sizeof(dst))) \
	                           goto shortfile; } while (0)

/*
 * StartPosition is read as an INT and stored into a float - the
 * original's three (float) casts at 0x00427ea0 and its neighbours.
 * Kept as its own step so the conversion is visible rather than
 * hidden in a macro.
 */
static int read_int_as_float(MR_Script *s, float *dst)
{
	int v;

	if (!bpt_read_int(s, &v))
		return 0;
	*dst = (float)v;
	return 1;
}

int level_load(MR_Level *out, const char *logicalName)
{
	MR_Script s;
	int       i, k;
	int       opened = 0;

	if (!out || !logicalName)
		return 0;
	memset(out, 0, sizeof(*out));

	if (!bpt_open(&s, logicalName)) {
		mrlog_err("level: %s did not open\n", logicalName);
		return 0;
	}
	opened = 1;

	{
		int newMotorEngine;
		RD_INT(newMotorEngine);
		(void)newMotorEngine;   /* read and discarded, as the original
		                         * does - it stores it and nothing on
		                         * this path reads it back. */
	}

	RD_STR(out->land);
	RD_STR(out->frames3d);
	RD_STR(out->decor);
	RD_STR(out->decorInv);
	RD_STR(out->net);
	RD_STR(out->map);
	RD_STR(out->moto);
	RD_STR(out->pocket);
	RD_STR(out->balise);
	RD_STR(out->smallMap);
	RD_STR(out->bank);

	/*
	 * THE BANK TABLE, AND SLOT 0 IS SHORT BY ONE STRING. FileBank
	 * above already consumed slot 0's first name, so slot 0 reads only
	 * its D3D name and its flags; slots 1..19 read both names and the
	 * flags. That asymmetry is in the original - the loop is entered
	 * after the first pair has been read - and getting it wrong shifts
	 * every later value by one.
	 */
	RD_STR(out->banks[0].pageD3D);
	RD_INT(out->banks[0].flags);
	memcpy(out->banks[0].page, out->bank, sizeof(out->banks[0].page));
	for (i = 1; i < LEVEL_BANKS; i++) {
		RD_STR(out->banks[i].page);
		RD_STR(out->banks[i].pageD3D);
		RD_INT(out->banks[i].flags);
	}

	RD_INT(out->stepGlobal);
	RD_INT(out->yScaleLevel);

	if (!read_int_as_float(&s, &out->startPos[0]) ||
	    !read_int_as_float(&s, &out->startPos[1]) ||
	    !read_int_as_float(&s, &out->startPos[2]))
	    goto shortfile;

	RD_INT(out->computeLandOptimise);

	/* The nine zone keys: base then four per-detail-level. */
	for (k = 0; k < LEVEL_ZONE_LODS; k++) RD_FLOAT(out->lenBPlane[k]);
	for (k = 0; k < LEVEL_ZONE_LODS; k++) RD_FLOAT(out->lenRLPlane[k]);
	for (k = 0; k < LEVEL_ZONE_LODS; k++) RD_FLOAT(out->flatZ[k]);
	for (k = 0; k < LEVEL_ZONE_LODS; k++) RD_FLOAT(out->horizFlatZ[k]);
	for (k = 0; k < LEVEL_ZONE_LODS; k++) RD_INT(out->mediumZone[k]);
	for (k = 0; k < LEVEL_ZONE_LODS; k++) RD_INT(out->hightZone[k]);
	for (k = 0; k < LEVEL_ZONE_LODS; k++) RD_FLOAT(out->gouraudTexturedZ[k]);
	for (k = 0; k < LEVEL_ZONE_LODS; k++) RD_FLOAT(out->texturedZ[k]);
	for (k = 0; k < LEVEL_ZONE_LODS; k++) RD_FLOAT(out->gouraudZ[k]);

	for (i = 0; i < LEVEL_TUNING; i++)
		for (k = 0; k < 3; k++)
			RD_FLOAT(out->tuning[i][k]);

	/*
	 * THE FIRST COUNTED RUN. The original reads one ComputerSpeed
	 * unconditionally and then NbComputer-1 more, which is the same
	 * as NbComputer of them and reads better this way.
	 */
	RD_INT(out->nbComputer);
	if (out->nbComputer < 1 || out->nbComputer > LEVEL_MAX_CPU) {
		mrlog_err("level: %s NbComputer %d out of range\n",
		          logicalName, out->nbComputer);
		goto bad;
	}
	for (i = 0; i < out->nbComputer; i++)
		RD_FLOAT(out->computerSpeed[i]);

	for (k = 0; k < 3; k++)
		RD_INT(out->timeSkill[k]);

	/* And the second. */
	RD_INT(out->nbCheckPoint);
	if (out->nbCheckPoint < 1 || out->nbCheckPoint > LEVEL_MAX_CP) {
		mrlog_err("level: %s NbCheckPoint %d out of range\n",
		          logicalName, out->nbCheckPoint);
		goto bad;
	}
	for (i = 0; i < out->nbCheckPoint; i++)
		RD_INT(out->timeCheckPoint[i]);

	RD_FLOAT(out->factorPocket);
	RD_INT(out->audioTrack);

	bpt_close(&s);
	return 1;

shortfile:
	mrlog_err("level: %s ended early - the read order does not fit "
	          "this file\n", logicalName);
bad:
	if (opened)
		bpt_close(&s);
	memset(out, 0, sizeof(*out));
	return 0;
}

/*
 * level_load_banks - bring the track's twenty texture pages up.
 *
 * FUN_00410590 is the original, and three things there are not
 * guessable from the manifest alone:
 *
 * o THE FLAG'S BIT 0 IS THE COLOUR KEY. The loader does
 *
 *       if ((flags & 1) != 0) bank->wFlags |= 0x10;
 *
 *   and 0x10 is the keyed bit our blend ladder needs. So a manifest
 *   flag of 5 is a keyed page - fences, trees, anything with holes -
 *   and 4 is opaque. [verified]
 *
 * o paletted / palBits / rgbBits COME FROM THE .RAW HEADER, not the
 *   manifest: the loader peeks the header's colour count and passes
 *   (1, 8, 0) when it is non-zero and (0, 0, 16) when it is not.
 *   Every ile01 track page is truecolour with zero colours, so they
 *   all take the second form. [verified, headers read directly]
 *
 * o AND THE HARDWARE PATH UPLOADS A DIFFERENT FILE. Bankw_LoadBank
 *   reads bnk00_NN.raw while D3D registers the name 0x500 further on
 *   - bnkD3DNN.raw. Those are 128x128 where the software pages are
 *   256x256: half-size art for 1997 video memory.
 *
 *   WE LOAD THE 256x256 PAGES. Three reasons, and it is a deliberate
 *   divergence rather than an oversight: the uv data is in 256 texel
 *   space so the full-size page needs no rescale, only 17 of the 20
 *   D3D variants exist, and the VRAM budget that motivated the halves
 *   does not apply to us. It is also what the SOFTWARE path used, and
 *   the software path is the one whose output retail footage shows.
 */
static int p1_bank_name(const MR_Track *t, const char *raw,
                        char *out, int size)
{
	/*
	 * TWO ROOTS IN ONE LIST. Most entries are track-relative -
	 * "bank\bnk00_01.raw" - but FileBank is global, "data\moto\...".
	 * A leading "data\" means the name is already rooted, so it only
	 * needs the prefix stripping the way track_asset_name would.
	 */
	if (_strnicmp(raw, "data\\", 5) == 0 || _strnicmp(raw, "data/", 5) == 0) {
		int i;

		if ((int)strlen(raw + 5) >= size)
			return 0;
		strcpy(out, raw + 5);
		for (i = 0; out[i]; i++) {
			if (out[i] >= 'a' && out[i] <= 'z')
				out[i] = (char)(out[i] - 'a' + 'A');
			else if (out[i] == '/')
				out[i] = '\\';
		}
		return 1;
	}
	return track_asset_name(t, raw, out, size);
}

int level_load_banks(const MR_Level *lv, const MR_Track *t, int base)
{
	char name[MAX_PATH];
	int  i;
	int  loaded = 0;

	if (!lv || !t)
		return 0;

	for (i = 0; i < LEVEL_BANKS; i++) {
		const MR_LevelBank *b = &lv->banks[i];
		int                 slot = base + i;
		int                 wantKey;

		if (!b->page[0] || _stricmp(b->page, "NULL") == 0)
			continue;
		if (!p1_bank_name(t, b->page, name, (int)sizeof(name)))
			continue;

		/*
		 * MODE 1. bank.h is explicit that mode 0 on a 256x256
		 * truecolour file reads no pixels at all and still reports
		 * success - which is exactly what these pages are, so mode 0
		 * would give twenty blank textures and no error.
		 */
		if (!bank_load(slot, name, 1)) {
			mrlog_err("trkbank: %s did not load into slot %d\n",
			          name, slot);
			continue;
		}

		wantKey = (b->flags & 1) ? 1 : 0;
		bank_alloc_texture_slot(slot, wantKey, 0, 0, 16);
		loaded++;
	}
	mrlog("trkbank: %d of %d pages into slots %d..%d\n",
	      loaded, LEVEL_BANKS, base, base + LEVEL_BANKS - 1);
	return loaded;
}

void level_log(const MR_Level *lv, const char *what)
{
	if (!lv)
		return;
	mrlog("level: %s land=%s bank=%s moto=%s\n",
	      what ? what : "(level)", lv->land, lv->bank, lv->moto);
	mrlog("level: start=(%.0f %.0f %.0f) cpu=%d checkpoints=%d "
	      "cdtrack=%d\n",
	      lv->startPos[0], lv->startPos[1], lv->startPos[2],
	      lv->nbComputer, lv->nbCheckPoint, lv->audioTrack);
}
