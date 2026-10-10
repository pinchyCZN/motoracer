/*
 * bikes.h - the eight bikes of a track's bike bank.
 *
 * WHAT THE EIGHT CARAC FILES ARE, finally settled. formats/bpt-carac.txt
 * decoded CARAC1..CARAC8 in each of the four banks and left "what the
 * eight variants select is not established" as an open question with
 * upgrade levels and per-track setups as candidates. Page 10 answers
 * it: the bike-select page indexes 0..7 straight into g_bikeCarac, so
 * THE EIGHT VARIANTS ARE THE EIGHT SELECTABLE BIKES.
 *
 * The chain from a track to its bikes is four files deep, and every
 * link is a .bpt [verified]:
 *
 *   LIST.BPT              island path, "data\ile01\"
 *   ILE01\LEVEL.BPT       [7] the bike manifest, "data\motoSpd.spt"
 *   MOTOSPD.BPT           the marker "MotoCarac", then eight names
 *   SPEED\CARAC1..8.BPT   73 values each; 1 to 4 are the stat bars
 *
 * o THE MARKER IS SCANNED FOR, NOT INDEXED. "MotoCarac" sits at
 *   record 49 in MOTOSPD and PKTSPEED, 81 in MOTOKTM and 60 in
 *   PKTKTM, because the animation list before it differs per bank.
 *   A fixed index would read four different things.
 */

#ifndef MRACER_BIKES_H
#define MRACER_BIKES_H

#include "tracks.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BIKE_COUNT 8

/*
 * Carac_Load (0x0042a580) reads all 73 values into a 0x124-byte
 * record - 73 * 4 exactly - and g_bikeCarac at 0x00509278 is an array
 * of eight of them. THE FILE INDEX AND THE MEMORY OFFSET ARE THE SAME
 * THING: value n sits at +4n. That was checked against a live read of
 * the running game rather than assumed, on all ten fields the physics
 * uses, and every one matched [verified]. See racers.txt 9.18.6.
 *
 * So the index comments below double as memory offsets, and a value
 * read out of Ghidra by offset can be found here by dividing by four.
 *
 * The menu needs the first five; the rest is the handling model.
 */
typedef struct {
	int motoType;    /* index 0                       */
	int gfxAcc;      /* index 1, and 2 to 4 follow it */
	int gfxSpe;
	int gfxGri;
	int gfxBra;

	/*
	 * THE HANDLING MODEL. Only the fields the drivetrain in
	 * physics.c actually reads are named; the rest of the 73 are
	 * still just indices and naming them here would assert more
	 * than has been earned.
	 *
	 * "Couple" is torque and "Regime" is rpm, and the three
	 * torque fields are the three points of a piecewise-linear
	 * curve - (1000, couple1000), (regimeCoupleMax, coupleMax),
	 * (regimeMax, coupleRegimeMax). The .bpt names describe the
	 * geometry exactly. TORQUE IS IN kgf.m, not N.m, which is why
	 * engine_torque_at multiplies by 9.81.
	 */
	float frein;             /* 5   braking force                */
	float moteurVitesseMax;  /* 6   the redline, in engineSpeed  */
	float motoMasse;         /* 7   kg; divides the drive force  */
	float roueAccAngulaire;  /* 9   how fast the bars pull over.
	                          *     MotoType 1 is CLAMPED to
	                          *     3.0..6.0 at load - bpt-carac 5 */
	float roueVitesseRetour; /* 11  the self-centring speed      */
	float hauteurCadre;      /* 22  frame height, x96 for the
	                          *     wheel contact points       */
	float roueLimiteAngulaire; /* 13 the lock stop               */
	float realSpeedMax;      /* 18  m/s. 80 = 288 km/h           */
	float vitesseMaxBoite;   /* 19  gearbox top speed            */
	float rotationTransfert; /* 20  scales the steering TORQUE   */
	float nbSpeed;           /* 28  gear count - and the         */
	                         /*     protection subtracts 1.0     */
	                         /*     from it, see 5.3             */
	float regimeMax;         /* 29  rpm at the redline           */
	float rapportDeBoite[6]; /* 30-35  the six gear ratios       */
	float regimeCoupleMax;   /* 36  rpm at peak torque           */
	float coupleMax;         /* 37  peak torque, kgf.m           */
	float couple1000;        /* 38  torque at 1000 rpm           */
	float coupleRegimeMax;   /* 39  torque at the redline        */
	float zoneRouge;         /* 40                               */
	float minAdherenceWheel; /* 41  grip floor for MotoType != 1 */

	/*
	 * The chassis spring, all five read by racer_update_suspension.
	 * Ressor is the French ressort, a spring; Cadre is the frame.
	 *
	 * Read live off MOTO for bike 0 on ile01:
	 *
	 *     retardCadre            16.000
	 *     angleSuspension         0.1963
	 *     ressorAmplitudeDec      0.885
	 *     ressorAmplitudeMin    -10.000
	 *     ressorAmplitudeFactor  10.000
	 *
	 * ressorAmplitudeMin IS NEGATIVE and that is not a misreading.
	 * The amplitude runs downward, so the floor is a limit on how
	 * far the spring may extend rather than a small positive
	 * value; a port that clamps it at zero instead kills the
	 * bounce outright.
	 */
	float retardCadre;       /* 8   how slowly the frame     */
	                         /*     follows the wheels       */
	float angleSuspension;   /* 12  the clamp on the output  */
	float ressorAmplitudeDec;    /* 23  decay per frame      */
	float ressorAmplitudeMin;    /* 24  the amplitude floor  */
	float ressorAmplitudeFactor; /* 25  divides amplitude    */
	                             /*     into phase advance   */

	/*
	 * The wheel geometry, carac indices 14 to 16 and floats.
	 * maybe_Racer_PlaceWheels (0x0045c7c0) builds the two wheel
	 * offsets from them and three constants in .rdata:
	 *
	 *     rear  = (0, -96 * rayonAr, -48 * axeDistance)
	 *     front = (0, -96 * rayonAv, +48 * axeDistance)
	 *
	 * -96 is 0x004d0e90 and the two 48s are 0x004d0e94 and
	 * 0x004d0e98 [verified]. 96 is also the Load3dfAndMap scale,
	 * so axeDistance is a wheelbase and the halves go either
	 * side of the root joint.
	 */
	float axeDistance;
	float rayonAv;
	float rayonAr;

	/*
	 * THE SWINGARM REST ANGLE, radians, carac index 27.
	 *
	 * ORIGIN  read at 0x0046031d as [bike*292 + 0x005092e4]
	 *
	 * The rear arm sits here unloaded; the draw adds a tracking
	 * arctangent on top. Bike 0 reads -0.55. See racers.txt 9.59.4.
	 */
	float swingarmRest;

	/*
	 * PersoAnim, carac index 72, an INT - formats/bpt-carac.txt 293.
	 *
	 * Used as a boolean. maybe_Racer_UpdateGroundContact reads it at
	 * 0x00468cb6 and gates the per-probe segment lookups on it: with
	 * PersoAnim clear, each ground probe is resolved in its own
	 * segment; with it set, both reuse the racer's.
	 */
	int persoAnim;

	int loaded;
} MR_BikeCarac;

/*
 * THE MANIFEST NAMES THE MODELS TOO, and page 10 needs them: the
 * first eighteen records are the bank, the skeleton and the three
 * static level-of-detail bodies with their .map files.
 *
 * The order is fixed and was read off MOTOSPD.BPT, MOTOKTM.BPT,
 * PKTSPEED.BPT and PKTKTM.BPT, which agree [verified]. Only the
 * ANIMATION list after them varies in length, which is why the
 * MotoCarac marker has to be scanned for rather than indexed.
 */
#define BIKE_PATH_BANK0        0    /* data\speed\bnk00_00.raw     */
#define BIKE_PATH_BANK1        1
#define BIKE_PATH_SKL          2    /* corps.skl, the articulated   */
#define BIKE_PATH_SKL_HAND     3
#define BIKE_PATH_WHEEL_R      4    /* rouear.3df                   */
#define BIKE_PATH_WHEEL_R_MAP  5
#define BIKE_PATH_WHEEL_F      6    /* roueav.3df                   */
#define BIKE_PATH_WHEEL_F_MAP  7
#define BIKE_PATH_MED          8    /* medium\corps.3df             */
#define BIKE_PATH_MED_MAP      9
#define BIKE_PATH_LOW         10
#define BIKE_PATH_LOW_MAP     11
#define BIKE_PATH_VLOW        12
#define BIKE_PATH_VLOW_MAP    13
#define BIKE_PATH_GUIDON      14
#define BIKE_PATH_GUIDON_MAP  15
#define BIKE_PATH_COCKPIT     16
#define BIKE_PATH_COCKPIT_MAP 17

/*
 * AND THE POSE LIST FOLLOWS THE MODELS, in the same order in all
 * four banks [verified - MOTOSPD and MOTOKTM agree name for name
 * from record 18 to 30, and only the LENGTH of the list differs].
 * Every one is a .rot, and a .rot is a single stance rather than
 * a clip; see skl.h.
 */
#define BIKE_PATH_ROT_STATIC      18
#define BIKE_PATH_ROT_GAUCHE      19
#define BIKE_PATH_ROT_DROITE      20
#define BIKE_PATH_ROT_STATICHAUT  21
#define BIKE_PATH_ROT_STATICPIED  22   /* feet down, the grid */
#define BIKE_PATH_ROT_DERAPGAUCHE 23
#define BIKE_PATH_ROT_DERAPDROITE 24
#define BIKE_PATH_ROT_SAUT        25
#define BIKE_PATH_ROT_GAGNE       26
#define BIKE_PATH_ROT_DESCENT     27

#define BIKE_PATHS                28

/*
 * The path at that index, already converted from a script path into
 * an asset name - "data\speed\medium\corps.3df" comes back as
 * "SPEED\MEDIUM\CORPS.3DF". NULL if nothing is loaded.
 *
 * NOTE THE EXTENSION IS NOT REWRITTEN for these: only .spt names
 * become .BPT, and a .3df stays a .3df.
 */
const char *bikes_asset(int which);

/*
 * bikes_load_for_track - walk the chain above. Returns the number of
 * bikes whose carac file was read, or 0.
 *
 * NOT THE SAME AS Game_LoadBikeAndBanks (which page 10's Enter calls):
 * that also loads the models, the textures and the animation set. This
 * reads the numbers the page draws and nothing else.
 */
/*
 * BIKE_UNITS_PER_M IS THE WORLD UNITS PER METRE, 0x004d102c, and it
 * is ONE constant in the image however many names a port gives it.
 * Thirteen read sites across maybe_Racer_Draw, Racer_SpinWheels,
 * UpdateGroundContact, UpdateLean, TickRemote and TickAI - every
 * place a carac length in metres becomes a world length.
 *
 * DO NOT CONFUSE IT WITH 0x004d0e90, which is -96.0f and is the
 * wheel radius scale.
 *
 * BIKE_AXLE_K IS POSITIVE AND 0x004d0e94 IS -48.0f. The sign lives
 * in the minus of "96*h - 48*r", so the macro does not equal the
 * address it cites; +48.0f is next door at 0x004d0e98 and which one
 * the original loads is not read yet. [open]
 */
#define BIKE_UNITS_PER_M 96.0f
#define BIKE_AXLE_K      48.0f

/*
 * bikes_display_lift - the model's own vertical placement, which
 * the fork and swingarm geometry is measured against. NOT the
 * track position. Menu and race must pass the same value.
 */
float bikes_display_lift(const MR_BikeCarac *b);

/*
 * pocket selects LEVEL.BPT record 8 over record 7 - a different
 * bike manifest entirely, not a variant. The original gates it on
 * g_nPocketBikeSelected at 0x004dc078; ours is g_pocketEnabled.
 */
int bikes_load_for_track(const MR_Track *t, int pocket);

/*
 * bikes_manifest - the bike manifest bikes_load_for_track resolved to,
 * or NULL.
 *
 * IT IS NEEDED BECAUSE BIKE_PATHS STOPS AT 28. The manifest's PilotRot
 * list is VARIABLE LENGTH - terminated by a line with no dot in it,
 * not by a count - and runs well past record 27 into the finish, crash,
 * podium and glance poses. bikes.c deliberately keeps only the prefix
 * it needs; pilot_rots_load re-scans for the whole thing.
 */
const char *bikes_manifest(void);
void bikes_free(void);

int                 bikes_count(void);
const MR_BikeCarac *bike_at(int index);

#ifdef __cplusplus
}
#endif

#endif /* MRACER_BIKES_H */
