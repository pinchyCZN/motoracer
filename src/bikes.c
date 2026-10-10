/*
 * bikes.c - the bike bank behind a track. See bikes.h.
 */

#include <stdlib.h>
#include <string.h>

#include "bikes.h"
#include "assets.h"
#include "bpt.h"
#include "log.h"

static MR_BikeCarac s_bikes[BIKE_COUNT];
static int          s_count;

/* The bike manifest this track resolved to. Kept so the pose
 * loader can re-scan it for the variable-length PilotRot list,
 * which runs past BIKE_PATHS. */
static char         s_manifest[TRACK_PATH_MAX];

/* The model paths out of the bike manifest, converted once. */
static char s_paths[BIKE_PATHS][TRACK_PATH_MAX];
static int  s_pathCount;

void bikes_free(void)
{
	memset(s_bikes, 0, sizeof s_bikes);
	memset(s_paths, 0, sizeof s_paths);
	s_count     = 0;
	s_pathCount = 0;
}

const char *bikes_asset(int which)
{
	if (which < 0 || which >= s_pathCount || !s_paths[which][0])
		return NULL;
	return s_paths[which];
}

int bikes_count(void)
{
	return s_count;
}

const MR_BikeCarac *bike_at(int index)
{
	if (index < 0 || index >= s_count)
		return NULL;
	return &s_bikes[index];
}


/*
 * The island manifest names the bike manifest at record 7. The seven
 * strings before it are the track, the frame and decor scripts, the
 * net file and the map - see formats/list-bpt.txt for the chain.
 */
/*
 * LEVEL.BPT NAMES TWO BIKE MANIFESTS, not one.
 *
 * ORIGIN  FUN_00427e80 @ 0x00428194, and the choice at
 *         Game_LoadBikeAndBanks 0x0040a5bb
 *
 *     record 7   FileMoto, keyed     data\motoSpd.spt
 *     record 8   unkeyed follow-on   data\PktSpeed.spt
 *
 * The original stores them at 0x0051d068 and 0x0051d0a8 and picks
 * between them on one global:
 *
 *     MOV EAX,[g_nPocketBikeSelected]   0x004dc078
 *     TEST EAX,EAX
 *     JZ  use 0x51d068                  the full-size bike
 *     PUSH 0x51d0a8                     the pocket bike
 *
 * so pocket mode is not a variant of a bike - it is a different
 * manifest naming different models, banks and carac files.
 */
static int bikes_read_level(const MR_Track *t, char *out, int size,
                            int pocket)
{
	MR_Script sc;
	char      name[TRACK_PATH_MAX];
	char      s[TRACK_PATH_MAX];
	int       i;
	int       v;
	int       ok = 0;

	if (!track_asset_name(t, "LEVEL.BPT", name, (int)sizeof(name)))
		return 0;
	if (!bpt_open(&sc, name))
		return 0;

	if (bpt_read_int(&sc, &v)) {
		for (i = 0; i < 7; i++) {
			if (!bpt_read_string(&sc, s, (int)sizeof(s)))
				break;
		}
		/* record 8 follows straight on - see the note above */
		if (i == 7 && pocket &&
		    !bpt_read_string(&sc, s, (int)sizeof(s)))
		    i = 0;
		if (i == 7)
			ok = asset_name_from_script(s, out, size, 1);
	}
	bpt_close(&sc);
	if (!ok)
		mrlog_err("bikes: %s has no bike manifest at record %d\n",
	              name, pocket ? 8 : 7);
	return ok;
}

/*
 * Scan the bike manifest for the "MotoCarac" marker and take the eight
 * names after it. Bounded, because a file that does not hold the
 * marker would otherwise read to the end of a 1.5 KB script one string
 * at a time and report nothing useful.
 */
#define BIKES_MANIFEST_SCAN_MAX 256

static int bikes_read_manifest(const char *manifest,
                               char names[BIKE_COUNT][TRACK_PATH_MAX])
{
	MR_Script sc;
	char      s[TRACK_PATH_MAX];
	int       i;
	int       found = 0;

	if (!bpt_open(&sc, manifest))
		return 0;

	for (i = 0; i < BIKES_MANIFEST_SCAN_MAX; i++) {
		if (!bpt_read_string(&sc, s, (int)sizeof(s)))
			break;
		if (strcmp(s, "MotoCarac") == 0) {
			found = 1;
			break;
		}
		/* The first eighteen are the bank and the models; keep
		 * them on the way past. */
		if (i < BIKE_PATHS &&
		    asset_name_from_script(s, s_paths[i], TRACK_PATH_MAX, 0)) {
			s_pathCount = i + 1;
		}

		/*
		 * A STUNT POSE CARRIES TWO VALUES AFTER IT, and skipping
		 * them is what lets this scan reach MotoCarac at all.
		 *
		 * ORIGIN  maybe_Bike_ReadManifest @ 0x0042a345, the
		 *         PilotRot loop: an entry beginning "_" with a
		 *         DIGIT calls FUN_00425080 twice and stores the
		 *         digit as a stunt index.
		 *
		 * "___name" is NOT one - isdigit('_') is false - so it
		 * takes no values. MOTOKTM has both forms.
		 *
		 * THE STREET MANIFESTS HAVE NO STUNT ENTRIES, which is why
		 * this went unnoticed: MOTOSPD is 58 strings and 58
		 * records, MOTOKTM is 72 strings and 90. Without the skip,
		 * bpt_read_string reads the value 800 as a length, the
		 * stream desynchronises, the scan stops eleven strings
		 * short of the marker, and no carac loads for the dirt
		 * bikes at all.
		 */
		if (s[0] == '_' && s[1] >= '0' && s[1] <= '9') {
			float skip;

			if (!bpt_read_float(&sc, &skip) ||
			    !bpt_read_float(&sc, &skip))
			    break;
		}
	}
	if (!found) {
		mrlog_err("bikes: %s has no MotoCarac marker\n", manifest);
		bpt_close(&sc);
		return 0;
	}

	for (i = 0; i < BIKE_COUNT; i++) {
		if (!bpt_read_string(&sc, s, (int)sizeof(s)))
			break;
		if (!asset_name_from_script(s, names[i], TRACK_PATH_MAX, 1))
			break;
	}
	bpt_close(&sc);
	return i;
}

/*
 * bikes_display_lift - how far the model sits above its own origin.
 *
 * ORIGIN  maybe_Racer_Draw @ 0x0045f828..0x0045ff62, read off the
 *         two addends rather than fitted:
 *
 *     EAX       = ( 0,  96 * hauteurCadre,        0 )
 *     ESP+0x1bc = ( 0, -48 * (rayonAv + rayonAr), 0 )
 *     B.x       =  48 * rayonAr
 *
 * so the 48 * rayonAr cancels and the lift is 96 * hauteurCadre -
 * 48 * rayonAv:
 *
 *     set             hautCadre  rAv   rAr    lift   measured
 *     street            0.58     0.29  0.32  41.760   41.7598
 *     pocket street     0.30     0.15  0.15  21.600   21.6000
 *     pocket dirt       0.30     0.15  0.15  21.600   21.6000
 *     full dirt         0.55     0.30  0.30  38.400   38.4000
 *
 * all four to 2e-4.  [verified live]
 *
 * THE LANDMINE IS rAr FOR rAv. 96 * hauteurCadre - 48 * rayonAr is
 * off by one letter and exact on THREE of the four sets, because
 * rayonAv equals rayonAr on both pocket sets AND on the full-size
 * dirt bike. Only the street bike, 0.29 against 0.32, tells the two
 * apart - so a formula fitted to any bike but that one fits
 * perfectly and is still wrong. A fixed 41.7598 and 144 * rayonAv
 * pass the same way. This is why the formula is READ off the two
 * addends above rather than fitted to measurements.
 *
 * IT IS NOT THE TRACK POSITION. On the menu the original's racer
 * sits at +0x30 = (0,0,0) while this reads 41.76, so it is a local
 * placement that the fork and swingarm measure against. The track
 * position is applied separately, camera-relative, at draw time -
 * feeding r->position in here puts the pivot thousands of units away
 * and collapses both wheels.
 */
float bikes_display_lift(const MR_BikeCarac *b)
{
	return b ? BIKE_UNITS_PER_M * b->hauteurCadre - BIKE_AXLE_K * b->rayonAv
	         : 0.0f;
}


int bikes_load_for_track(const MR_Track *t, int pocket)
{
	char manifest[TRACK_PATH_MAX];
	char names[BIKE_COUNT][TRACK_PATH_MAX];
	int  n;
	int  i;

	bikes_free();
	if (!t)
		return 0;

	if (!bikes_read_level(t, manifest, (int)sizeof(manifest), pocket))
		return 0;

	n = bikes_read_manifest(manifest, names);
	if (n <= 0)
		return 0;

	/* Remembered for bikes_manifest(); see the note on the static. */
	strncpy(s_manifest, manifest, sizeof s_manifest - 1);
	s_manifest[sizeof s_manifest - 1] = 0;

	for (i = 0; i < n; i++) {
		MR_Script sc;

		if (!bpt_open(&sc, names[i]))
			continue;
		/*
		 * Index 0 is MotoType and 1 to 4 are the four stat bars.
		 * Indices 5 onwards are floats - the handling model.
		 *
		 * THIS USED TO STOP AT 16, because the menu only wanted
		 * the wheel geometry. It now reads to 41 so the physics
		 * has the engine, the gearbox and the grip floor. The
		 * loop still walks every index in order rather than
		 * seeking, because a .bpt is a sequential script and
		 * index n is simply the (n+1)th value in it.
		 */
		if (bpt_read_int(&sc, &s_bikes[i].motoType) &&
		    bpt_read_int(&sc, &s_bikes[i].gfxAcc) &&
		    bpt_read_int(&sc, &s_bikes[i].gfxSpe) &&
		    bpt_read_int(&sc, &s_bikes[i].gfxGri) &&
		    bpt_read_int(&sc, &s_bikes[i].gfxBra)) {
			int   k;
			float f = 0.0f;
			int   ok = 1;

			/*
			 * 5..71 ARE FLOATS AND 72 IS AN INT. Indices 0..4 and 72 are
			 * integers, the other 67 floats - bpt-carac.txt 298. This
			 * loop used to stop at 41 because nothing past it was
			 * needed; PersoAnim is 72, so it now walks the floats out to
			 * 71 and reads 72 with the integer reader.
			 */
			for (k = 5; k <= 71 && ok; k++) {
				ok = bpt_read_float(&sc, &f);
				if (!ok)
					break;
				switch (k) {
				case  5: s_bikes[i].frein            = f; break;
				case  6: s_bikes[i].moteurVitesseMax = f; break;
				case  7: s_bikes[i].motoMasse        = f; break;
				case  9: s_bikes[i].roueAccAngulaire = f; break;
				case  8: s_bikes[i].retardCadre      = f; break;
				case 12: s_bikes[i].angleSuspension  = f; break;
				case 23: s_bikes[i].ressorAmplitudeDec = f; break;
				case 24: s_bikes[i].ressorAmplitudeMin = f; break;
				case 25: s_bikes[i].ressorAmplitudeFactor = f; break;
				case 11: s_bikes[i].roueVitesseRetour = f; break;
				case 13: s_bikes[i].roueLimiteAngulaire = f; break;
				case 14: s_bikes[i].axeDistance      = f; break;
				case 15: s_bikes[i].rayonAv          = f; break;
				case 16: s_bikes[i].rayonAr          = f; break;
				case 27: s_bikes[i].swingarmRest     = f; break;
				case 18: s_bikes[i].realSpeedMax     = f; break;
				case 19: s_bikes[i].vitesseMaxBoite  = f; break;
				case 20: s_bikes[i].rotationTransfert = f; break;
				case 22: s_bikes[i].hauteurCadre      = f; break;
				case 28: s_bikes[i].nbSpeed          = f; break;
				case 29: s_bikes[i].regimeMax        = f; break;
				case 36: s_bikes[i].regimeCoupleMax  = f; break;
				case 37: s_bikes[i].coupleMax        = f; break;
				case 38: s_bikes[i].couple1000       = f; break;
				case 39: s_bikes[i].coupleRegimeMax  = f; break;
				case 40: s_bikes[i].zoneRouge        = f; break;
				case 41: s_bikes[i].minAdherenceWheel = f; break;
				default:
					if (k >= 30 && k <= 35)
						s_bikes[i].rapportDeBoite[k - 30] = f;
					break;
				}
			}
			if (ok)
				ok = bpt_read_int(&sc, &s_bikes[i].persoAnim);

			/*
			 * THE ONE ADJUSTMENT MADE AFTER READING, and it is not
			 * cosmetic: on a MotoType 1 bike Carac_Load clamps
			 * RoueAccelerationAngulaire to 3.0..6.0.
			 * formats/bpt-carac.txt records it; nothing did it.
			 *
			 * MEASURED: the file gives the KTM 30.0 and MOTO holds
			 * 6.0 live - exactly the clamp - so our bars accelerated
			 * FIVE TIMES too fast and hit the lock stop in one frame
			 * where the original takes six.
			 *
			 * IT LOOKED LIKE A STEERING BUG AND IS NOT. The ramp at
			 * 0x0046638f..0x004663af is steerRate += acc*dt then
			 * steering -= steerRate*dt, which is what racer.c already
			 * had, instruction for instruction. Only the input was
			 * wrong. racers-convergence.txt 9.46.23.3.
			 *
			 * THE OTHER ADJUSTMENT IN THAT NOTE IS THE PROTECTION
			 * PENALTY on NbSpeed and is deliberately NOT reproduced -
			 * CLAUDE.md, and it is not a property of the format.
			 */
			if (s_bikes[i].motoType == 1) {
				if (s_bikes[i].roueAccAngulaire < 3.0f)
					s_bikes[i].roueAccAngulaire = 3.0f;
				else if (s_bikes[i].roueAccAngulaire > 6.0f)
					s_bikes[i].roueAccAngulaire = 6.0f;
			}

			s_bikes[i].loaded = ok;
		}
		bpt_close(&sc);
	}

	s_count = n;
	mrlog("bikes: %s -> %s, %d bike(s)\n", t->name, manifest, s_count);
	for (i = 0; i < s_count; i++) {
		mrlog("       %d  acc %3d  spe %3d  gri %3d  bra %3d  "
		      "wheelbase %.2f radii %.2f/%.2f\n", i,
		      s_bikes[i].gfxAcc, s_bikes[i].gfxSpe,
		      s_bikes[i].gfxGri, s_bikes[i].gfxBra,
		      s_bikes[i].axeDistance, s_bikes[i].rayonAv,
		      s_bikes[i].rayonAr);
	}
	return s_count;
}


/* ORIGIN  NONE - ours. The original keeps the manifest open across
 *         the whole load; we re-open it for the pose list. */
const char *bikes_manifest(void)
{
	return s_manifest[0] ? s_manifest : NULL;
}
