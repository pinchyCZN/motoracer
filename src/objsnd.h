/*
 * objsnd.h - the per-track OBJECT SOUNDS, out of SOUND.BPT.
 *
 * ORIGIN  the sound half of animDecor.cpp. Six functions, all in the
 *         0x00404xxx-0x00406xxx block:
 *
 *             0x0042e700  Script_Open on "%sSound.Spt"
 *             0x0042e850  the parser - soundDesc / ambiance / signal
 *             0x004056c0  name -> id, interning as it goes
 *             0x004059a0  load the samples the descriptors named
 *             0x00405790  bind a descriptor to every decor instance
 *             0x00405bc0  the per-frame placement
 *             0x00405030  the per-voice state machine
 *
 * THIS IS NOT "A FEW MORE CUES". It is a small sound-design engine:
 * a per-track script names samples, gives them fade envelopes, LFOs
 * on volume and pitch, random pitch and pan, repeat counts and a
 * distance falloff - and then attaches the result to decor objects
 * so a gull, a corsair or a passing aeroplane carries its own sound
 * through the world.
 *
 * WHY IT IS ITS OWN MODULE. audio.c owns the mixer - samples, voices,
 * cue ids, the pause rule. This owns a SCRIPT and a state machine
 * that drive it. Nothing here touches a voice except through the
 * audio.h handle layer, and audio.c knows nothing about descriptors.
 *
 * THE SCRIPT IS "Sound.Spt" AND THE FILE IS SOUND.BPT. The loader
 * rewrites any .spt request to .bpt - the same rule that turns
 * Data\Config.Spt into DATA\CONFIG.BPT - so the shipped member is
 * ILE01\SOUND.BPT and it is read with the ordinary bpt.h primitives.
 * See docs/formats/bpt.txt.
 */

#ifndef MRACER_OBJSND_H
#define MRACER_OBJSND_H

#include "trk.h"
#include "audio.h"   /* MR_Voice, for MR_SoundSlot */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * THE FLAG WORD, parsed from a pipe-separated string like
 * "LOOPED|FADEIN|FADEOUT|FADEFREQ" [verified, 0x0042eb00 onward -
 * ten __strnicmp tests in a chain].
 *
 * PAN AND VOL ARE THE POSITIONAL PAIR and they are opted into per
 * descriptor, which is the whole reason some things on a track
 * follow the camera and others do not. ressac has VOL and not PAN -
 * surf gets quieter with distance but stays centred, because it is
 * not a point source.
 */
#define OBJSND_LOOPED    0x001
#define OBJSND_PAN       0x002   /* pan by direction from the eye   */
#define OBJSND_RANDPAN   0x004   /* a fresh random pan per trigger  */
#define OBJSND_VOL       0x008   /* attenuate by distance           */
#define OBJSND_FADEIN    0x010
#define OBJSND_FADEOUT   0x020
#define OBJSND_FADEFREQ  0x040   /* ramp pitch across a fade        */
#define OBJSND_INFPLAY   0x080   /* never leave sustain             */
#define OBJSND_SINVOL    0x100   /* sine LFO on volume              */
#define OBJSND_SINFREQ   0x200   /* sine LFO on pitch               */

#define OBJSND_MAX_DESC   32     /* 0x0042ec20, the throw at 0x20   */
#define OBJSND_MAX_NAMES  48     /* 0x00502718..0x0050289c / 8      */
#define OBJSND_MAX_BOUND  40     /* 0x00405790, "Too many object.." */

/*
 * HOW FAR ROUND THE TRACK AN OBJECT SOUND CARRIES, in segments.
 * 0x3c, the literal ObjSnd_TickAll passes to the window test at
 * 0x00405bf9.
 *
 * THIS IS THE ONLY THING KEEPING DISTANT PROPS QUIET, and it is
 * not the distance falloff - a descriptor with a big radiusOut
 * is audible from far enough away that the whole track is in
 * range. Without the window the aeroplane halfway round ile01
 * sounds from the grid.
 */
#define OBJSND_SEG_WINDOW 60

/*
 * AND THE WINDOW THE SIGNAL SEARCH USES, which is a DIFFERENT
 * number - 0x28, passed at 0x00405bab. 40 segments either way for
 * a span, 60 for a decor object.
 */
#define OBJSND_SIGNAL_WINDOW 40

/*
 * THE FIRST CUE ID THE SCRIPT MAY CLAIM.
 *
 * ORIGIN  0x00405716, `return iVar2 + 0x5d`.
 *
 * Object-sound ids are ASSIGNED AT LOAD, counting up from here as
 * the parser meets each new name - they are not constants and they
 * differ between tracks. That is why none of these samples has a
 * hardcoded id anywhere in audio.c, and why searching MOTO.EXE for
 * "ressac" finds nothing.
 */
#define OBJSND_ID_BASE    0x5d

/*
 * A DESCRIPTOR - 19 dwords in the original, at 0x00502930 with a
 * stride of 0x4c [verified, the copy loop at 0x0042ebbe].
 *
 * The field ORDER is the script's own, because a .bpt stores no
 * names: the parser simply reads them in sequence. Getting one out
 * of order desynchronises the rest of the file.
 *
 * EVERY "Rand" FIELD IS A SPREAD, NOT A MAXIMUM. The slot takes
 * base + Rand_Next(4) % rand, so 0 means "always exactly base"
 * [0x00404ee0].
 */
typedef struct {
	int          id;            /* +0x00 matched against the decor */
	int          firstSound;    /* +0x04 cue id of the first sample */
	int          sampleCount;   /* +0x08 how many, picked at random */
	unsigned int flags;         /* +0x0c the word above            */

	int          waitBase;      /* +0x10 silence before a trigger  */
	int          waitRand;      /* +0x14                           */
	int          sustainBase;   /* +0x18 also the LFO period       */
	int          sustainRand;   /* +0x1c                           */
	int          fadeInMs;      /* +0x20                           */
	int          fadeOutMs;     /* +0x24                           */
	int          repeatBase;    /* +0x28 plays before going quiet  */
	int          repeatRand;    /* +0x2c                           */
	int          freqBase;      /* +0x30 <1 means the sample rate  */
	int          freqRand;      /* +0x34                           */
	int          fadeFreqDelta; /* +0x38 pitch swing over a fade   */
	int          volume;        /* +0x3c 0..255                    */
	int          pan;           /* +0x40 the fixed pan             */
	float        radiusIn;      /* +0x44 full volume inside this   */
	float        radiusOut;     /* +0x48 silent beyond this        */
} MR_SoundDesc;

/*
 * WHERE A BOUND SOUND SITS. The three arms of 0x00405bc0, and the
 * difference between them is audible: a gull moves with its model,
 * a wind anchored to a segment does not, and an arena rumble has no
 * position at all.
 */
#define OBJSND_AT_OBJECT   0     /* the decor instance's own place */
#define OBJSND_AT_SEGMENT  1     /* a track segment's origin       */
#define OBJSND_AT_GLOBAL   2     /* no position - always audible   */

/*
 * ONE PLAYING SOUND - the original's 25-dword slot.
 *
 * THE STATE MACHINE IS THE POINT [0x00405030]:
 *
 *     0 wait -> 1 start -> 2 fadeIn -> 5 sustain -> 4 fadeOut
 *                              \                       |
 *                               `--- (no FADEIN) ------ 3 stop
 *
 * and 3 decrements the repeat count, re-arming at 0 if any are
 * left. INFPLAY pins it in 5 for ever.
 */
typedef struct {
	const MR_SoundDesc *desc;
	MR_Voice     voice;
	int          sound;         /* the sample this play chose     */
	int          volume;        /* base, already master-scaled    */
	unsigned int stamp;         /* ms at the last state change    */
	int          wait;
	int          sustain;
	int          fadeIn;
	int          fadeOut;
	int          repeats;
	int          freq;
	int          state;
	/*
	 * THESE THREE ARE FLOATS AND THAT IS NOT COSMETIC. The
	 * original keeps them at slot +0x30, +0x34 and +0x38 and
	 * reads them with FCOM and FMUL, so they are floats there
	 * too - the decompiler only shows (int) because the slot is
	 * typed as int*.
	 *
	 * slope IS ALWAYS LESS THAN ONE. 255 / ((100 - 30) * 128) is
	 * 0.028 for the gulls, so holding it in an int truncates it
	 * to zero and every VOL sound on the track plays at volume
	 * nothing - silently, because the voice still starts.
	 */
	float        cutoff;        /* radiusOut in world units       */
	float        slope;         /* 255 / (radiusOut - radiusIn)   */
	float        lastDist;
	int          stopped;
} MR_SoundSlot;

/* One decor object, segment span or global, that owns a sound. */
typedef struct {
	int                 kind;      /* OBJSND_AT_*                 */
	int                 segment;   /* where AT_OBJECT was found   */
	int                 instance;  /* index into trk->instances   */

	/*
	 * AT_SEGMENT ONLY, and it is NOT a segment index - it is
	 * the signal KIND, 6..12, which every frame resolves to a
	 * different segment as the player moves.  The original
	 * keeps it at header +0x08.
	 */
	int                 signalKind;

	MR_SoundSlot        slot;
} MR_ObjSound;

/* Forget every descriptor, name and binding. Safe any time. */
void objsnd_reset(void);

/*
 * Parse one track's SOUND.BPT and load the samples it names.
 * logicalName is what track_asset_name builds, e.g. ILE01\SOUND.BPT.
 * Returns the descriptor count, or 0 when the track has no script -
 * which is not an error; most do not.
 */
int  objsnd_load(const char *logicalName);

/*
 * Attach every decor instance that carries a descriptor id to its
 * descriptor. Returns how many were bound.
 */
int  objsnd_bind(const MR_Trk *trk);

/*
 * One frame. eye is the camera position the distance and pan are
 * measured from, and fwdX/fwdZ its normalised forward flattened
 * to the ground - the same pair race_audio_pan crosses against,
 * so a gull and a bike agree about which side they are on.
 *
 * curSegment is the LOCAL PLAYER'S segment, and it is what
 * decides which objects are close enough round the track to be
 * heard at all - see OBJSND_SEG_WINDOW. The original reads it
 * off the track object at +0x20; we have no such field, so it
 * is passed.
 */
void objsnd_tick(const MR_Trk *trk, const float eye[3],
                 float fwdX, float fwdZ, int curSegment);

/* How many objects are sounding - for the log and for tests. */
int  objsnd_bound_count(void);

#ifdef __cplusplus
}
#endif

#endif /* MRACER_OBJSND_H */
