/*
 * audio.c - the sound back end. XAudio2, in C.
 *
 * THIS FILE IS C AND HOLDS EVERY DECISION. The sample store, the
 * bank table, the voice pool, the cue dispatch, the volumes and the
 * retrigger guard are all here. What is NOT here is the COM: ten
 * one-line calls into XAudio2, which live in xa2.cpp behind the C
 * surface in xa2.h.
 *
 * THAT SPLIT IS FORCED BY THE SDK HEADER, not chosen. xaudio2.h
 * does carry an #ifndef __cplusplus block of IXAudio2_* vtable
 * macros, so the API is meant to be usable from C - but the header
 * itself will not compile as C. Two lines see to that:
 *
 *   :61    interface __declspec(uuid("2B02E3CF-...")) IXAudio2;
 *   :1201  XAUDIO2_STDAPI, which begins  extern "C"
 *
 * The first can be blanked with a scoped #define uuid(x). THE
 * SECOND CANNOT BE, and the trick that looks like it would is a
 * trap: blanking __stdcall to swallow the declaration also blanks
 * the STDMETHODCALLTYPE inside every DECLARE_INTERFACE_ above it,
 * turning all fifty vtable slots __cdecl. That builds, links, and
 * corrupts the stack on the first call.
 *
 * Verified identical in SDK 10.0.16299.0 and 10.0.17134.0.
 *
 * So this is rule 11's COM exemption, same as d3d9rt.cpp - but
 * pared to the COM alone.
 */

/*
 * What it was, and what still holds:
 *
 * Every entry point in audio.h, implemented as a no-op that keeps
 * enough state to answer the queries honestly. This is deliberate and
 * not a placeholder to be embarrassed about:
 *
 *   - the game can call audio from the day the first menu page needs
 *     a click, without the caller being written twice;
 *   - a machine with no audio device takes exactly this path, so the
 *     silent path is one that has to work anyway;
 *   - and it keeps the decision about XAudio2 versus anything else
 *     out of the way until there is something to play.
 *
 * audio_init returns 1. It succeeds at being silent. A back end that
 * genuinely fails to open a device also returns 1 here and reports
 * through audio_ready(), so callers never branch on init.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "xa2.h"
#include "music.h"
#include "wave.h"
#include "options.h"
#include "timing.h"
#include "log.h"

static int   s_ready;
static int   s_musicTrack;
static int   s_musicLoop;
static float s_musicVolume = 1.0f;
static float s_sfxVolume   = 1.0f;
static int   s_nextHandle  = 1;

/*
 * THE HANDLE TABLE.
 *
 * ORIGIN  g_nSndHandles @ 0x00519ff8 - 256 ints, one per cue id,
 *         refilled with -1 by every loader before it starts.
 *
 * -1 MEANS SILENTLY INAUDIBLE, NOT AN ERROR. Snd_LoadSample returns
 * -1 on failure and the player drops a -1 handle, so an id that did
 * not load costs nothing at cue time. Ours holds an MR_Sound, where
 * the same role is played by MR_SOUND_NONE.
 */
#define AUDIO_CUE_IDS 256
static MR_Sound s_handles[AUDIO_CUE_IDS];

/*
 * THE SAMPLE STORE, and the reason the port keeps the PCM when the
 * original throws it away: Csample_Load hands its buffer to
 * DirectSound and GlobalFrees the copy, but XAudio2 does not copy,
 * so the block has to outlive playback. See audio-cues.txt 4.1.
 *
 * Slot 0 is never used - MR_SOUND_NONE is 0, so a handle is
 * one-based and a zeroed struct field reads as no sound.
 *
 * 64 IS THE MENU BANK WITH ROOM, NOT A MEASURED BOUND. The menu
 * bank is 18 (7 + 11); the largest race bank is 30 + 18 + 16 = 64
 * exactly, so this is sized for it and the loader says so if a
 * bank ever overruns rather than dropping samples quietly.
 */
/*
 * 64 WAS EXACTLY FULL. A race bank is Divers 18 + engine 16 +
 * voices 30 = 64, so the per-track object sounds - up to 48 more
 * named by SOUND.BPT - had nowhere to go. 128 leaves headroom
 * without pretending to know the original's own limit.
 */
#define AUDIO_MAX_SAMPLES 129         /* 128 usable, slot 0 reserved */
static MR_Wave s_samples[AUDIO_MAX_SAMPLES];
static int     s_sampleCount;         /* highest slot used, 1-based */

/*
 * A {name, id} pair table.
 *
 * ORIGIN  the six tables at 0x004db680..0x004db920, stride 8.
 *
 * THE ID DECIDES THE GROUP AND THE TABLE DECIDES THE FILE, and they
 * are independent - which is why the same id names a different file
 * in each bank. See audio-cues.txt 5.2.
 */
typedef struct {
	const char *name;
	int         id;
} MR_CuePair;

/*
 * ORIGIN  table C @ 0x004db7d8, 7 entries, category "Menu\\".
 *
 * Transcribed from memory rather than from a listing, in the
 * table's own order. The loop that reads it starts on the ID field
 * at 0x004db7dc and ends at 0x004db814.
 */
static const MR_CuePair kBankMenuSfx[] = {
	{ "deplett",  0x28 },
	{ "select",   0x29 },
	{ "valide",   0x2a },
	{ "goktm",    0x2b },
	{ "goninja",  0x2c },
	{ "title",    0x2d },
	{ "continue", 0x03 }
};

/*
 * ORIGIN  table B @ 0x004db780, 11 entries, "Voices\\Menu\\".
 *
 * The ids run contiguously from 0x52 apart from entrname, which
 * sits at 0x36 down among the sound effects. Loop 0x004db784 to
 * 0x004db7dc.
 */
static const MR_CuePair kBankMenuVoice[] = {
	{ "entrname", 0x36 },
	{ "slctrans", 0x52 },
	{ "selerace", 0x53 },
	{ "selectmd", 0x54 },
	{ "playsolo", 0x55 },
	{ "playlink", 0x56 },
	{ "makurchc", 0x57 },
	{ "letsgo",   0x58 },
	{ "cngrecor", 0x59 },
	{ "cngratim", 0x5a },
	{ "yutrygan", 0x5b }
};


/*
 * ORIGIN  table D @ 0x004db810, 18 entries, \\"Divers\\\\".
 *         Race ambience, surfaces and the two brakes.
 *
 * Transcribed from memory in the table's own order.
 */
static const MR_CuePair kBankDivers[] = {
	{ "bip1",     0x01 },
	{ "bip2",     0x02 },
	{ "chckpvox", 0x06 },
	{ "continue", 0x03 },
	{ "deplett",  0x28 },
	{ "select",   0x29 },
	{ "pause",    0x2e },
	{ "derap",    0x0a },
	{ "brake1",   0x1a },
	{ "brake2",   0x1b },
	{ "crash",    0x12 },
	{ "rouleman", 0x11 },
	{ "wterdeb",  0x14 },
	{ "wterpas",  0x15 },
	{ "wterfin",  0x16 },
	{ "gravier",  0x17 },
	{ "glace",    0x18 },
	{ "herbe",    0x19 }
};

/*
 * THE TWO ENGINE BANKS, and one flag picks between them: cues 0x22
 * to 0x26 are the per-gear loops, indexed through the tables at
 * 0x004db618 (road) and 0x004db638 (dirt).
 *
 * KTM SHIPS FIVE GEAR SAMPLES AND NINJA TWO, which is why the road
 * table maps eight gears onto 0x22 and 0x26 alone. The shift code
 * must compare the TABLE ENTRY, not the gear - see audio-cues.txt
 * 9.4.
 *
 * Three ids name different files between them: 0x1c is grince on
 * the KTM and suspens1 on the Ninja, and 0x0b is land1 against
 * land. The id means nothing without the bank.
 */
/*
 * ORIGIN  table E @ 0x004db8a0, 16 entries, \\"Ktm\\\\".
 *
 * Transcribed from memory in the table's own order.
 */
static const MR_CuePair kBankKtm[] = {
	{ "motostp", 0x21 },
	{ "Gear5",   0x26 },
	{ "Gear4",   0x25 },
	{ "Gear3",   0x24 },
	{ "Gear2",   0x23 },
	{ "Gear1",   0x22 },
	{ "depart1", 0x1d },
	{ "depart2", 0x1e },
	{ "depart3", 0x1f },
	{ "grince",  0x1c },
	{ "choc1",   0x0e },
	{ "choc2",   0x0f },
	{ "land1",   0x0b },
	{ "land2",   0x0c },
	{ "land3",   0x0d },
	{ "wheel",   0x13 }
};

/*
 * ORIGIN  table F @ 0x004db920, 11 entries, \\"Ninja\\\\".
 *
 * Transcribed from memory in the table's own order.
 */
static const MR_CuePair kBankNinja[] = {
	{ "motostp",  0x21 },
	{ "Gear5",    0x26 },
	{ "Gear1",    0x22 },
	{ "depart1",  0x1d },
	{ "depart2",  0x1e },
	{ "depart3",  0x1f },
	{ "suspens1", 0x1c },
	{ "choc1",    0x0e },
	{ "choc2",    0x0f },
	{ "land",     0x0b },
	{ "wheel",    0x13 }
};

/*
 * ORIGIN  table A @ 0x004db680, 30 entries, \\"Voices\\\\".
 *         The countdown, the lap calls and the rider's own
 *         commentary. 43 files ship; these 30 are wired.
 *
 * Transcribed from memory in the table's own order.
 */
static const MR_CuePair kBankRaceVoice[] = {
	{ "3",        0x30 },
	{ "2",        0x31 },
	{ "1",        0x32 },
	{ "go",       0x33 },
	{ "ckckpnt",  0x34 },
	{ "finlap",   0x37 },
	{ "fuel",     0x38 },
	{ "gameover", 0x39 },
	{ "lechef",   0x3a },
	{ "newrecor", 0x3b },
	{ "onegear",  0x3c },
	{ "oouups",   0x3d },
	{ "racecmpl", 0x3e },
	{ "runlast",  0x3f },
	{ "running4", 0x40 },
	{ "stillrun", 0x41 },
	{ "timeout",  0x42 },
	{ "tryagain", 0x44 },
	{ "watarokt", 0x45 },
	{ "wrongway", 0x46 },
	{ "youlost",  0x47 },
	{ "yuralosr", 0x48 },
	{ "yurdalst", 0x49 },
	{ "opopadow", 0x4a },
	{ "ouch",     0x4b },
	{ "yeehaaa",  0x4c },
	{ "yeees",    0x4e },
	{ "yeaah",    0x4f },
	{ "WellDone", 0x50 },
	{ "wouaou",   0x51 }
};

#define ARRAY_COUNT(a) ((int)(sizeof (a) / sizeof (a)[0]))

static void audio_bank_free(void);

/*
 * 0..AUDIO_VOL_MAX, indexed by AUDIO_VOL_*. Full until the ini
 * loads them - the original reads its three from the mixer, which is
 * a source we deliberately do not have.
 */
static int s_volume[AUDIO_VOL_COUNT] = {
	AUDIO_VOL_MAX, AUDIO_VOL_MAX, AUDIO_VOL_MAX
};


/*
 * THE VOICE POOL - what DuplicateSoundBuffer was for.
 *
 * ORIGIN  FUN_00441a40, the lazy duplicator: with cue flag 0x02 set
 *         it checks GetStatus, walks a chain of duplicate buffers
 *         through [+4], and calls DuplicateSoundBuffer only when
 *         every one of them is already playing.
 *
 * A DUPLICATE IS A SECOND PLAYBACK STATE OVER THE SAME SAMPLES, so
 * in XAudio2 it is simply a second source voice - there is nothing
 * to duplicate. Voices are therefore bound to a SAMPLE, not pooled
 * generically, which also means the format always matches and no
 * voice is ever created for the wrong rate.
 *
 * The original's slot table caps at 0x400. This is far smaller
 * because it is per-process rather than per-sample-chain, and it
 * says so when it runs out instead of failing quietly.
 */
#define AUDIO_MAX_VOICES 48

typedef struct {
	XA2_Voice *voice;
	MR_Sound   sound;      /* which sample it is bound to */
	int        owned;      /* handed out by audio_voice_start */
	int        loop;       /* started with SND_FLAG_LOOP      */
	int        held;       /* paused by audio_set_paused      */
} MR_VoiceSlot;

static MR_VoiceSlot s_voices[AUDIO_MAX_VOICES];
static int          s_voiceCount;
static int          s_paused;

/*
 * A VOICE STARTED WHILE THE MIXER IS PAUSED IS BORN HELD.
 *
 * ORIGIN  NONE - ours.
 *
 * audio_set_paused IS EDGE-TRIGGERED: it sweeps the pool once
 * when the state changes and returns early ever after. Anything
 * started after that edge escaped the hold and played straight
 * through the pause.
 *
 * THE SPACE-BAR PAUSE NEVER SHOWED IT. A frozen clock hands
 * race_update a zero dt, so the sub-step body - and with it
 * race_audio_tick - is gated off and nothing new starts. The
 * agent's stepped runs DO advance the clock while tm_paused() is
 * set, so the engine kept re-arming its loop behind a pause and
 * sounded on for ever.
 *
 * LOOPS ONLY, matching the sweep: a one-shot in the air is left
 * to finish, which is what lets a menu cue through a pause.
 */
static void audio_hold_if_paused(int slot)
{
	if (!s_paused || !s_voices[slot].loop)
		return;
	xa2_voice_pause(s_voices[slot].voice);
	s_voices[slot].held = 1;
}

/*
 * THE TWO GROUPS, AND THE SPLIT IS NOT WHERE YOU WOULD GUESS: ids
 * 0x2f..0x5c are one group and EVERYTHING ELSE is the other.
 * Snd_PlayCue keeps an enable flag, a volume and a last-played id
 * per group; 0x2f..0x5c is speech. See audio-cues.txt 1.1.
 */
#define SND_GROUP_SPEECH_LO 0x2f
#define SND_GROUP_SPEECH_HI 0x5c

static int s_lastCue[2] = { -1, -1 };   /* [0] sfx, [1] speech */

static int audio_cue_is_speech(int soundId)
{
	return soundId >= SND_GROUP_SPEECH_LO && soundId <= SND_GROUP_SPEECH_HI;
}

/*
 * The gain a group's cue plays at. The original has none - it drives
 * the WINDOWS MIXER instead - so applying these as a source-voice
 * gain is a deliberate divergence, stated in audio.h.
 */
/*
 * SPEECH AND EFFECTS SCALE SEPARATELY, and that is the
 * original's design rather than an embellishment: it carries
 * g_nCfgSfxVolume and g_nCfgVoicesVolume as two words and every
 * play site chooses between them on the cue id.
 *
 * Both start at full, so this changes nothing audible until
 * somebody sets one.  See audio.h.
 */
static int s_groupVolume[AUDIO_GROUP_COUNT] = {
	AUDIO_VOL_MAX, AUDIO_VOL_MAX
};

void audio_set_group_volume(int group, int value)
{
	if (group < 0 || group >= AUDIO_GROUP_COUNT)
		return;
	if (value < 0)
		value = 0;
	if (value > AUDIO_VOL_MAX)
		value = AUDIO_VOL_MAX;
	s_groupVolume[group] = value;
}

int audio_group_volume(int group)
{
	if (group < 0 || group >= AUDIO_GROUP_COUNT)
		return 0;
	return s_groupVolume[group];
}


/*
 * THE MUSIC GAIN, and it is the CD channel's analogue of
 * audio_group_gain: master times its own channel.
 *
 * THERE IS NO WAVE TERM IN IT. WaveVolume scales the SAMPLES;
 * on the original the CD was a separate mixer line that the
 * wave slider never touched, and the options page keeps the
 * two apart for the same reason.  Folding wave in here would
 * make the music follow a slider the disc never answered to.
 *
 * music.c asks for this EVERY FRAME rather than being pushed
 * a value, so moving the slider on the options page is heard
 * immediately and needs no notification path.
 */
float audio_music_gain(void)
{
	float master = (float)s_volume[AUDIO_VOL_MASTER] / AUDIO_VOL_MAX;
	float cd     = (float)s_volume[AUDIO_VOL_CD]     / AUDIO_VOL_MAX;

	return master * cd;
}


static float audio_group_gain(int speech)
{
	float master = (float)s_volume[AUDIO_VOL_MASTER] / AUDIO_VOL_MAX;
	float wave   = (float)s_volume[AUDIO_VOL_WAVE]   / AUDIO_VOL_MAX;
	int   group  = speech ? AUDIO_GROUP_SPEECH : AUDIO_GROUP_SFX;

	return master * wave
	       * ((float)s_groupVolume[group] / AUDIO_VOL_MAX);
}

/*
 * A voice to play this sample on.
 *
 * layer 0 - reuse the sample's first voice, restarting it.
 * layer 1 - find one of its voices that is idle, else make another.
 */
/* Returns a SLOT INDEX, not a voice - the caller has to stamp the
 * loop flag on it, and audio_set_paused reads that. -1 on
 * failure. */
static int audio_voice_for(MR_Sound snd, int layer)
{
	int i;

	for (i = 0; i < s_voiceCount; i++) {
		/*
		 * AN OWNED SLOT IS NOT AVAILABLE TO THE CUE LAYER. The
		 * engine note holds its voice for the whole race; without
		 * this, a cue on the same sample would seize it and the
		 * next retune would drive somebody else's sound.
		 */
		if (s_voices[i].owned || s_voices[i].sound != snd)
			continue;
		if (!layer)
			return i;
		if (!xa2_voice_busy(s_voices[i].voice))
			return i;
	}

	if (s_voiceCount >= AUDIO_MAX_VOICES) {
		/*
		 * FULL OF IDLE SLOTS BOUND TO OTHER SAMPLES.
		 *
		 * A slot is created for one sample and stays bound to
		 * it for ever, so the loop above can only reuse a slot
		 * that already belongs to THIS sample.  Once 48 exist,
		 * a new cue is refused however many of them are
		 * finished - caught with 48 allocated, TWO owned and
		 * twenty of them holding one throttle blip.
		 *
		 * That is why the log fills with a cue that never got
		 * near the pool itself: 0x1e exhausts it and 0x1d,
		 * which has idle slots of its own nowhere to be found,
		 * is refused with it.
		 *
		 * So REBIND a finished one.  Not owned, not looping,
		 * not held by the pause, and not still sounding - the
		 * voice is destroyed and remade because the sample
		 * format is fixed at creation and the new sample need
		 * not match the old one's rate or width.
		 */
		for (i = 0; i < s_voiceCount; i++) {
			if (s_voices[i].owned || s_voices[i].loop ||
			    s_voices[i].held)
			    continue;
			if (xa2_voice_busy(s_voices[i].voice))
				continue;

			xa2_voice_destroy(s_voices[i].voice);
			s_voices[i].voice =
			        xa2_voice_create(s_samples[snd].channels,
			                         s_samples[snd].sampleRate,
			                         s_samples[snd].bits);
			if (!s_voices[i].voice)
				return -1;
			s_voices[i].sound = snd;
			return i;
		}

		mrlog_err("audio:  voice pool full at %d - cue dropped\n",
		          s_voiceCount);
		return -1;
	}

	s_voices[s_voiceCount].voice =
	        xa2_voice_create(s_samples[snd].channels,
	                         s_samples[snd].sampleRate,
	                         s_samples[snd].bits);
	if (!s_voices[s_voiceCount].voice)
		return -1;
	s_voices[s_voiceCount].sound = snd;
	return s_voiceCount++;
}

/* Every voice, because the samples they point at are about to go. */
static void audio_voices_free(void)
{
	int i;

	for (i = 0; i < s_voiceCount; i++)
		xa2_voice_destroy(s_voices[i].voice);
	memset(s_voices, 0, sizeof s_voices);
	s_voiceCount = 0;
	s_paused     = 0;
	s_lastCue[0] = -1;
	s_lastCue[1] = -1;
}

/*
 * ORIGIN  NONE - ours. The original opens DirectSound in Cdd's
 *         init and has no equivalent entry point.
 *
 * IT ALWAYS RETURNS 1, EVEN WITH NO DEVICE. A machine with no
 * sound card, no xaudio2_9.dll, or audio already exclusively held
 * takes the silent path - which is the same path the stub took, so
 * it is one that has to work anyway. Callers branch on
 * audio_ready(), never on this.
 */
static void audio_apply_mute(void);

int audio_init(void)
{
	s_ready      = 0;
	s_musicTrack = 0;

	if (!xa2_open())
		return 1;                       /* silent; xa2.cpp said why */

	s_ready = 1;

	/*
	 * THE MUSIC IS OPTIONAL AND ITS ABSENCE IS NORMAL.
	 * music_init returns 0 when there is no cue sheet named in
	 * the ini, which is what a stock install looks like.  The
	 * return is deliberately ignored: no music is not a failed
	 * audio init.
	 */
	(void)music_init();
	audio_apply_mute();   /* a reopen resets the mastering voice */
	return 1;
}

void audio_shutdown(void)
{
	s_ready      = 0;
	s_musicTrack = 0;
	music_free();           /* the voice, before the device */
	audio_bank_free();      /* the PCM, before the device goes */
	xa2_close();
}

int audio_ready(void)
{
	return s_ready;
}

/*
 * THE ANNOUNCER QUEUE - Snd_QueueVoice @ 0x00404da0, eight slots.
 *
 * ORIGIN  0x005028ec[8]  cue ids
 *         0x0050290c[8]  delays, ms
 *         0x005028d8     write index, (i + 1) & 7
 *         0x005028d4     read index
 *
 * THIS IS THE WHOLE COMMENTATOR.  Ten sites queue into it and
 * nothing plays an announcer line any other way except the
 * countdown, which is deliberately immediate.  Every caller
 * passes 500 ms, which is why each line lands a beat after the
 * event that caused it rather than on top of it.
 * audio-cues.txt 8.2 has the table.
 *
 * The original stores a DELAY per entry and counts it down;
 * storing an absolute due time instead is the same thing with
 * the subtraction done once.
 *
 * A RING, NOT A POOL, AND THE DIFFERENCE IS AUDIBLE.  This used
 * to take the first free slot, so two lines queued in one frame
 * came out in slot order rather than the order they happened.
 * The original advances one write index and drains from one read
 * index, so it is strictly first in first out.  head/tail here
 * are free-running counters masked on use - the same thing
 * without the wrap arithmetic at every site.
 *
 * FULL MEANS DROPPED, SILENTLY, and that is faithful: the
 * original compares the NEXT write index against the read index
 * and returns without storing, so a ninth line in flight is lost
 * rather than displacing an earlier one.
 *
 * VIRTUAL SECONDS, NOT REAL ONES. This read tm_real_now, on the
 * argument that a queued voice line should not stop when the debug
 * pause does. That is the wrong way round: a cue scheduled three
 * seconds out and fired while the game is frozen announces
 * something that has not happened yet, and stepping a frame at a
 * time is exactly when you want to hear the cue land on the frame
 * that caused it.
 *
 * It also made this the last thing in the frame loop still
 * integrating outside the dt gate - see frame-loop.txt 1.5, and
 * camera.txt for what that cost when the chase camera did it.
 *
 * tm_now and tm_real_now are THE SAME CLOCK whenever the game is
 * running normally, so nothing changes outside a debug pause.
 */
#define AUDIO_DELAYED_MAX 8

/*
 * THE DELAY IS THE GAP AFTER A LINE, NOT A WAIT BEFORE IT.
 *
 * This stored an absolute due time and fired everything that had
 * come due in the same frame, which put the checkpoint blip and
 * the word on top of each other and half a second behind the
 * crossing.  Retail plays the blip ON the checkpoint and the word
 * well after it, one then the other.
 *
 * WHAT THE RUNNING GAME SHOWS.  The ring at 0x005028ec is drained
 * ONE ENTRY AT A TIME - hand-feeding it advanced the read index by
 * exactly one per consume - and the drain is on ANOTHER THREAD: a
 * hardware watchpoint on the read index caught the write not at
 * all while the value changed under it, and no function in the
 * database reads 0x005028cc, which the enqueue clears and the
 * drain sets back to 1 when the queue empties.  It is the mixer
 * thread.
 *
 * [inferred] The 500 every caller passes is therefore the SPACING
 * between consecutive lines rather than a lead-in: the first
 * entry goes out at once and the next waits.  That is what makes
 * the blip land on the geometry, and it is read off behaviour
 * plus the live ring rather than off the drain, which is not in
 * the database to read.
 */
static struct {
	int soundId;
	int gapMs;
} s_delayed[AUDIO_DELAYED_MAX];

static int          s_lastCueSlot = -1;
static int          s_delayedSlot = -1;  /* the line speaking */
static unsigned int s_delayedHead;   /* next to drain */
static unsigned int s_delayedTail;   /* next to fill  */
static double       s_delayedFree;   /* when the line is clear */

int audio_cue_delayed(int soundId, int delayMs)
{
	unsigned int slot;

	if (s_delayedTail - s_delayedHead >= AUDIO_DELAYED_MAX)
		return 0;                   /* all eight in flight */

	slot = s_delayedTail & (AUDIO_DELAYED_MAX - 1);
	s_delayed[slot].soundId = soundId;
	s_delayed[slot].gapMs   = delayMs;
	s_delayedTail++;
	return 1;
}


/*
 * Forget everything queued.  A race teardown must call this or
 * the next one opens with the last one's commentary.
 */
void audio_cue_delayed_reset(void)
{
	s_delayedHead = 0;
	s_delayedTail = 0;
	s_delayedFree = 0.0;
	s_delayedSlot = -1;
}

void audio_update(void)
{
	double now = tm_now();

	/*
	 * THE MUSIC PUMP, and it goes first: it is the one thing
	 * here with a deadline.  Everything below is millisecond
	 * bookkeeping on a queue, while a late music_tick is an
	 * underrun and an audible gap.  See music.c.
	 */
	music_tick();

	/*
	 * ONE LINE PER DRAIN, IN ORDER, AND NEVER TWO AT ONCE.
	 *
	 * The line is clear when the gap the last entry asked for
	 * has passed; until then the rest of the queue waits, in
	 * the order the race put it there.  Firing everything
	 * that had come due in one pass is what made the
	 * checkpoint blip and its word speak over each other.
	 */
	/*
	 * AND NOT WHILE THE LAST LINE IS STILL SPEAKING.
	 *
	 * The gap alone is not enough.  A win is announced with
	 * a phrase well over half a second long and RACE
	 * COMPLETED follows it; released on the gap only, the
	 * second talks over the tail of the first.  Retail lets
	 * the phrase finish and then speaks.  [observed]
	 *
	 * So both must be true: the gap has passed AND the
	 * voice the last release started is quiet.  A short
	 * blip therefore still clears in its 500 ms and only a
	 * long line extends the wait.
	 */
	if (s_delayedSlot >= 0 && s_delayedSlot < s_voiceCount &&
	    s_voices[s_delayedSlot].voice &&
	    xa2_voice_busy(s_voices[s_delayedSlot].voice))
	    return;

	if (s_delayedHead != s_delayedTail && now >= s_delayedFree) {
		unsigned int slot = s_delayedHead &
		                    (AUDIO_DELAYED_MAX - 1);

		s_lastCueSlot = -1;
		audio_cue(s_delayed[slot].soundId, 0);
		s_delayedSlot = s_lastCueSlot;
		s_delayedFree = now
		                + s_delayed[slot].gapMs / 1000.0;
		s_delayed[slot].soundId = 0;
		s_delayedHead++;
	}
}

void audio_set_music_volume(float v)
{
	s_musicVolume = v;
}

void audio_set_sfx_volume(float v)
{
	s_sfxVolume = v;
}

/*
 * THE STATE LIVES IN music.c NOW, because that is what actually
 * knows: it owns the file, the position and the voice. These keep
 * s_musicTrack in step only so a reader of this file is not looking
 * at a stale global.
 */
void audio_music_play(int track, int loop)
{
	s_musicTrack = track;
	s_musicLoop  = loop;

	if (track <= 0) {
		music_stop();
		s_musicTrack = 0;
		return;
	}
	if (!music_play(track, loop))
		s_musicTrack = 0;
}

void audio_music_stop(void)
{
	s_musicTrack = 0;
	music_stop();
}

int audio_music_playing(void)
{
	return music_playing();
}

/*
 * The two numeric cues. Silent, but they SAY SO ONCE PER ID, which is
 * the whole value of having them wired before a back end exists: the
 * log answers "did that moment try to make a sound" without anybody
 * having to read the disassembly again.
 *
 * Once per id and not once per call - the hover cue would otherwise
 * fill the log on its own.
 */
static unsigned char s_cueSeen[256];

int audio_cue(int soundId, unsigned int flags)
{
	int        speech;
	MR_Sound   snd;
	int        slot;

	if (soundId < 0 || soundId > 0xff) {
		/*
		 * The original puts up a message box here, and only on the
		 * outer branch of its id split. A log line is the same
		 * information without a modal dialog in a game loop.
		 */
		mrlog_err("audio:  cue id 0x%x is out of range\n", soundId);
		return -1;
	}

	speech = audio_cue_is_speech(soundId);

	/*
	 * THE RETRIGGER GUARD, cue flag bit 2. Dropped when this id is
	 * already the group's last - so a cue asked for every frame does
	 * not restart every frame. Note it compares against the GROUP's
	 * last id, not the voice's state.
	 */
	if ((flags & SND_FLAG_NO_RETRIGGER) && s_lastCue[speech] == soundId)
		return -1;

	/* The two option toggles, one per group. */
	if (speech ? !g_options.voices : !g_options.sounds)
		return -1;

	snd = s_handles[soundId];
	if (!s_cueSeen[soundId]) {
		s_cueSeen[soundId] = 1;
		if (snd == MR_SOUND_NONE)
			mrlog("audio:  cue 0x%02x (flags 0x%x) - no sample\n",
		          soundId, flags);
		else
			mrlog("audio:  cue 0x%02x (flags 0x%x) -> sample %d, "
		          "%d Hz %d bit, %u bytes\n",
		          soundId, flags, (int)snd,
		          s_samples[snd].sampleRate, s_samples[snd].bits,
		          s_samples[snd].pcmBytes);
	}

	/*
	 * AN ID THAT NEVER LOADED IS SILENTLY INAUDIBLE, not an error.
	 * Snd_LoadSample returns -1 and the player drops a -1 handle, so
	 * this has to cost nothing.
	 */
	if (snd == MR_SOUND_NONE || !s_ready)
		return -1;

	slot = audio_voice_for(snd, (flags & SND_FLAG_POLYPHONIC) != 0);
	if (slot < 0)
		return -1;

	s_voices[slot].loop = (flags & SND_FLAG_LOOP) != 0;
	s_voices[slot].held = 0;
	xa2_voice_volume(s_voices[slot].voice,
	                 audio_group_gain(speech));
	if (!xa2_voice_play(s_voices[slot].voice, s_samples[snd].pcm,
	                    s_samples[snd].pcmBytes,
	                    s_voices[slot].loop))
	    return -1;
	audio_hold_if_paused(slot);

	/*
	 * WHICH SLOT IT LANDED ON, for the announcer drain - it
	 * has to know when the line it released has finished
	 * speaking, and audio_cue returns the cue id rather than
	 * a voice.
	 */
	s_lastCueSlot = slot;

	s_lastCue[speech] = soundId;
	return soundId;
}

void audio_music_cue(int track, int mode)
{
	/*
	 * Straight through to audio_music_play, which is the same request
	 * with the mode bits already split out. Nothing here loops or
	 * restarts because nothing here plays; the state is kept so
	 * audio_music_playing stays honest.
	 */
	int loop = (mode & CD_MODE_LOOP) ? 1 : 0;

	/*
	 * -1 LEAVES WHATEVER IS PLAYING ALONE. FUN_004b18e0 tests
	 * for it before anything else, so it is not "stop" and
	 * not "random" - it is a request to do nothing.
	 */
	if (track < 0)
		return;

	/*
	 * TRACK 0 MEANS RANDOM, NOT SILENCE.
	 *
	 * The original picks rand() % trackCount + 1, which on a
	 * real disc numbers the AUDIO tracks from 1. Ours cannot:
	 * track 1 of this disc is the MODE1/2352 data track and
	 * picking it would ask for the game itself.  music_track_at
	 * indexes the audio tracks the cue actually found, so the
	 * data track is never in the draw.  [deliberate divergence]
	 */
	if (track == 0) {
		int n = music_track_count();

		if (n <= 0)
			return;
		track = music_track_at(rand() % n);
		if (track <= 0)
			return;
	}

	/*
	 * BIT 0 IS "DO NOT RESTART IF IT IS ALREADY PLAYING", and
	 * it is why the main menu survives being re-entered without
	 * the music jumping back to the top.  It needs a real
	 * answer about what is playing, which music.c keeps.
	 */
	if ((mode & CD_MODE_KEEP) && music_current_track() == track &&
	    music_playing())
	    return;

	mrlog("audio:  music track %d mode %d%s%s\n", track, mode,
	      loop ? " loop" : "", (mode & CD_MODE_KEEP) ? " keep" : "");
	audio_music_play(track, loop);
}

static int s_bank = AUDIO_BANK_NONE;

/*
 * audio_wave_bits - 8 or 16, the parallel sample tree to read from.
 *
 * ORIGIN  NONE - ours. The original tests g_cfgWave16Bits inline
 *         at the second piece of the filename.
 *
 * SFX.BKF CARRIES BOTH TREES, 125 members each, so this selects a
 * directory and not a decode option.
 */
static int audio_wave_bits(void)
{
	const MR_WaveFormat *f = options_wave_format(g_options.waveFormat);

	return (f && f->bits == 8) ? 8 : 16;
}

/*
 * ORIGIN  the five-piece concatenation every loader performs,
 *         Snd_LoadMenuBank @ 0x00403f90 onward.
 *
 * DIVERGENCE: the original prefixes "data\\sfx\\" and opens a path.
 * Ours builds a LOGICAL name - archive stem first - because rule 10
 * forbids touching a game asset with fopen. SFX is the archive;
 * everything after it is the member name, which already carries its
 * subdirectories with backslashes.
 */
static void audio_bank_name(char *out, int max, const char *category,
                            const char *stem)
{
	_snprintf(out, max, "SFX\\%s\\%s%s.WAV",
	          audio_wave_bits() == 8 ? "8_BITS" : "16_BITS",
	          category, stem);
	out[max - 1] = 0;
}

/*
 * Take a slot and decode into it. Returns MR_SOUND_NONE on any
 * failure, which is the -1 the original's Snd_LoadSample returns.
 */
static MR_Sound audio_store_load(const char *logical)
{
	MR_Wave w;

	if (s_sampleCount + 1 >= AUDIO_MAX_SAMPLES) {
		mrlog_err("audio:  sample store full at %d - %s dropped\n",
		          s_sampleCount, logical);
		return MR_SOUND_NONE;
	}
	if (!wave_load(logical, &w))
		return MR_SOUND_NONE;

	s_sampleCount++;
	s_samples[s_sampleCount] = w;
	return (MR_Sound)s_sampleCount;
}

/*
 * One pair table into the handle table. Returns how many loaded.
 *
 * A MISS IS LOGGED AND SURVIVED. wave_load has already said why;
 * this leaves the id at MR_SOUND_NONE and carries on, because that
 * is what the original does and because one absent file must not
 * cost the other seventeen.
 */
static int audio_bank_table(const MR_CuePair *tbl, int count,
                            const char *category)
{
	char name[160];
	int  i;
	int  loaded = 0;

	for (i = 0; i < count; i++) {
		MR_Sound snd;

		audio_bank_name(name, (int)sizeof name, category, tbl[i].name);
		snd = audio_store_load(name);
		if (snd == MR_SOUND_NONE) {
			mrlog_err("audio:  cue 0x%02x %s did not load\n",
			          tbl[i].id, name);
			continue;
		}
		s_handles[tbl[i].id] = snd;
		loaded++;
	}
	return loaded;
}

/* Release every decoded sample and empty the handle table. */
static void audio_bank_free(void)
{
	int i;

	/*
	 * VOICES FIRST, ALWAYS. A source voice reads the PCM in place,
	 * so freeing a sample while a voice still holds it is a
	 * use-after-free that would surface as a burst of noise rather
	 * than a crash.
	 */
	audio_voices_free();

	for (i = 1; i <= s_sampleCount; i++)
		wave_free(&s_samples[i]);
	s_sampleCount = 0;
	memset(s_handles, 0, sizeof s_handles);
}

/* Decoded PCM held by the store, for the one line that reports it. */
static unsigned int audio_store_bytes(void)
{
	unsigned int total = 0;
	int          i;

	for (i = 1; i <= s_sampleCount; i++)
		total += s_samples[i].pcmBytes;
	return total;
}

/*
 * ORIGIN  Snd_LoadMenuBank @ 0x00403f90 for AUDIO_BANK_MENU.
 *
 * THE RACE BANK IS PICKED BY THE BIKE, not by the track: the
 * argument Snd_LoadRaceBank takes is
 * g_bikeCarac[g_nSelectedBike].PersoAnim, which is 1 on every
 * Motocross bike and 0 on every Speed one. race.c chooses; this
 * only loads. See audio-cues.txt 5.5.
 */
void audio_load_bank(int bank)
{
	static const char *const kName[] = {
		"none", "menu", "race/ninja", "race/ktm"
	};
	int sfx   = 0;
	int voice = 0;

	if (bank < AUDIO_BANK_NONE || bank > AUDIO_BANK_RACE_KTM) {
		mrlog_err("audio:  bank %d is not a bank\n", bank);
		return;
	}

	audio_bank_free();
	s_bank = bank;
	memset(s_cueSeen, 0, sizeof s_cueSeen);

	if (bank == AUDIO_BANK_RACE_NINJA || bank == AUDIO_BANK_RACE_KTM) {
		const MR_CuePair *engine;
		int               engineCount;

		/*
		 * ORIGIN  Snd_LoadRaceBank @ 0x004044c0. Divers always,
		 *         then ONE of the two engine banks, then the
		 *         race speech if voices are on.
		 *
		 * NO ID COLLIDES BETWEEN THE THREE, so the load order
		 * cannot change which file an id resolves to. Worth
		 * knowing before anyone reorders them.
		 */
		engine      = (bank == AUDIO_BANK_RACE_KTM)
		                  ? kBankKtm : kBankNinja;
		engineCount = (bank == AUDIO_BANK_RACE_KTM)
		                  ? ARRAY_COUNT(kBankKtm)
		                  : ARRAY_COUNT(kBankNinja);

		sfx  = audio_bank_table(kBankDivers,
		                        ARRAY_COUNT(kBankDivers), "DIVERS\\");
		sfx += audio_bank_table(engine, engineCount,
		                        (bank == AUDIO_BANK_RACE_KTM)
		                            ? "KTM\\" : "NINJA\\");
		if (g_options.voices)
			voice = audio_bank_table(kBankRaceVoice,
		                             ARRAY_COUNT(kBankRaceVoice),
		                             "VOICES\\");
	} else if (bank == AUDIO_BANK_MENU) {
		sfx = audio_bank_table(kBankMenuSfx,
		                       ARRAY_COUNT(kBankMenuSfx), "MENU\\");
		/*
		 * THE SPEECH HALF IS GATED, exactly as the original
		 * gates it on g_nCfgVoicesEnabled. With voices off the
		 * eleven ids stay MR_SOUND_NONE and every cue in
		 * 0x36 and 0x52..0x5b is inaudible - which is the
		 * setting doing its job, not a load failure.
		 */
		if (g_options.voices)
			voice = audio_bank_table(kBankMenuVoice,
		                             ARRAY_COUNT(kBankMenuVoice),
		                             "VOICES\\MENU\\");
	}

	mrlog("audio:  bank -> %s, %d sfx + %d voice = %d samples, "
	      "%u KB, %d bit tree\n",
	      kName[bank], sfx, voice, s_sampleCount,
	      (unsigned)(audio_store_bytes() / 1024u), audio_wave_bits());
}

int audio_current_bank(void)
{
	return s_bank;
}

/*
 * ORIGIN  Snd_LoadSample @ 0x00423f80, as reached from the bank
 *         loaders.
 *
 * The same store the banks use, so a caller-loaded sound is freed
 * by the next bank change like any other. That is a DIVERGENCE
 * worth knowing: the original has no caller-loaded samples at all.
 */
MR_Sound audio_sound_load(const char *logicalName)
{
	if (!logicalName)
		return MR_SOUND_NONE;
	return audio_store_load(logicalName);
}

/*
 * FREES THE PCM, NOT THE SLOT. Slots are reclaimed only by a bank
 * change, because the handle table indexes them and a reused slot
 * would silently re-point a cue id at a different sample.
 */
void audio_sound_free(MR_Sound s)
{
	if (s > MR_SOUND_NONE && s <= s_sampleCount)
		wave_free(&s_samples[s]);
}

/*
 * ORIGIN  NONE - ours. The original has no caller-driven play; every
 *         sample it starts goes through Snd_PlayCue by id.
 *
 * PAN AND PITCH ARE ACCEPTED AND IGNORED, and that is recorded
 * rather than hidden: the original's Snd_SetPan is called with 0 by
 * every loader, and nothing in the menus asks for either. They stay
 * in the signature because the racer mixer will want both - see
 * audio-cues.txt 9.1 - and changing the signature later is worse
 * than carrying two unused arguments now.
 */
int audio_sound_play(MR_Sound s, float volume, float pan, float pitch)
{
	int slot;

	(void)pan; (void)pitch;
	if (s <= MR_SOUND_NONE || s > s_sampleCount || !s_ready)
		return 0;

	slot = audio_voice_for(s, 1);       /* a caller play may layer */
	if (slot < 0)
		return 0;
	s_voices[slot].loop = 0;
	s_voices[slot].held = 0;
	xa2_voice_volume(s_voices[slot].voice,
	                 volume * audio_group_gain(0));
	if (!xa2_voice_play(s_voices[slot].voice, s_samples[s].pcm,
	                    s_samples[s].pcmBytes, 0))
	    return 0;
	return (int)s;
}

/*
 * THE HANDLE LAYER. See audio.h for why it exists beside audio_cue.
 * A handle is an index into the voice pool; a slot marked owned is
 * invisible to the cue layer until it is stopped.
 */
static int audio_voice_valid(MR_Voice v)
{
	return v >= 0 && v < s_voiceCount && s_voices[v].voice != NULL;
}

/*
 * ORIGIN  the tail of ObjSnd_LoadSamples @ 0x00405b4a.
 *
 * LOAD ONE SAMPLE INTO A CHOSEN CUE ID. The bank tables are
 * compiled in and their ids are constants; the per-track object
 * sounds are neither - SOUND.BPT names them and their ids are
 * handed out at load. This is audio_bank_table for one entry,
 * so the path building and the 8/16-bit choice stay in here.
 */
int audio_bind_cue(int soundId, const char *category,
                   const char *base)
{
	char     name[160];
	MR_Sound snd;

	if (soundId < 0 || soundId >= AUDIO_CUE_IDS || !base)
		return 0;

	audio_bank_name(name, (int)sizeof name, category, base);
	snd = audio_store_load(name);
	if (snd == MR_SOUND_NONE)
		return 0;
	s_handles[soundId] = snd;
	return 1;
}


/*
 * The sample's own rate. A soundDesc with freqBase under 1 is
 * asking for an OFFSET from this rather than an absolute Hz.
 */
int audio_cue_rate(int soundId)
{
	MR_Sound snd;

	if (soundId < 0 || soundId >= AUDIO_CUE_IDS)
		return 0;
	snd = s_handles[soundId];
	if (snd == MR_SOUND_NONE)
		return 0;
	return s_samples[snd].sampleRate;
}


/* ORIGIN  maybe_Snd_StartVoice @ 0x00423fd0 */
MR_Voice audio_voice_start(int soundId, unsigned int flags)
{
	MR_Sound snd;
	int      i;
	int      slot = -1;

	if (soundId < 0 || soundId > 0xff || !s_ready)
		return MR_VOICE_NONE;
	snd = s_handles[soundId];
	if (snd == MR_SOUND_NONE)
		return MR_VOICE_NONE;

	/*
	 * RECLAIM FINISHED ONE-SHOTS BEFORE LOOKING FOR A SLOT.
	 *
	 * A voice stays owned until audio_voice_stop, and NOTHING
	 * EVER STOPS A ONE-SHOT - it simply ends. Without this sweep
	 * every fire-and-forget cue retires a slot permanently and
	 * the pool is exhausted after 48 of them; blipping the
	 * throttle is enough to do it in under a minute.
	 *
	 * A LOOP IS NEVER SWEPT. It is not busy in the gap between
	 * buffers, so reclaiming on !busy alone would pull the engine
	 * note out from under the racer that owns it. Nor is a HELD
	 * voice - pausing stops it, which also reads as not busy.
	 *
	 * The handle a caller still holds goes stale when its slot is
	 * reused. That is the original's behaviour too - record[0xb]
	 * is a bare int there - and a stale stop costs one wrong
	 * silence rather than a leak.
	 */
	for (i = 0; i < s_voiceCount; i++) {
		if (s_voices[i].owned && !s_voices[i].loop &&
		    !s_voices[i].held &&
		    !xa2_voice_busy(s_voices[i].voice))
		    s_voices[i].owned = 0;
	}

	/*
	 * A FREE SLOT ON THIS SAMPLE FIRST, then a new one. Reusing an
	 * idle voice of the right sample avoids creating a second one
	 * per gear every time the engine note changes.
	 */
	for (i = 0; i < s_voiceCount; i++) {
		if (!s_voices[i].owned && s_voices[i].sound == snd &&
		    !xa2_voice_busy(s_voices[i].voice)) {
			slot = i;
			break;
		}
	}
	if (slot < 0) {
		if (s_voiceCount >= AUDIO_MAX_VOICES) {
			/*
			 * NO SLOT ON THIS SAMPLE AND NO ROOM FOR ONE, so
			 * take a finished slot off another sample and
			 * rebind it - the voice is destroyed and remade
			 * because its format is fixed at creation.
			 *
			 * WITHOUT THIS THE POOL NEVER RECOVERS.  A slot
			 * belongs to the first sample that used it for
			 * ever, so once 48 exist every new sound is
			 * refused no matter how many have finished -
			 * caught with 48 allocated, NONE owned, none
			 * held, and twenty of them still nailed to one
			 * throttle blip.  The fast-forward fills it and
			 * dropping back to normal speed never gets it
			 * back, which is the symptom: the race goes
			 * quiet and stays quiet.
			 *
			 * audio_voice_for has the same guard for the cue
			 * layer.  BOTH PATHS NEED IT; they allocate
			 * independently and only this one reports "not
			 * started".
			 */
			for (i = 0; i < s_voiceCount; i++) {
				if (s_voices[i].owned || s_voices[i].loop ||
				    s_voices[i].held)
				    continue;
				if (xa2_voice_busy(s_voices[i].voice))
					continue;

				xa2_voice_destroy(s_voices[i].voice);
				s_voices[i].voice =
				        xa2_voice_create(
				                s_samples[snd].channels,
				                s_samples[snd].sampleRate,
				                s_samples[snd].bits);
				if (!s_voices[i].voice)
					return MR_VOICE_NONE;
				s_voices[i].sound = snd;
				slot = i;
				break;
			}

			if (slot < 0) {
				mrlog_err("audio:  voice pool full at %d - "
				          "cue 0x%02x not started\n",
				          s_voiceCount, soundId);
				return MR_VOICE_NONE;
			}
		}
		if (slot < 0) {
			s_voices[s_voiceCount].voice =
			        xa2_voice_create(
			                s_samples[snd].channels,
			                s_samples[snd].sampleRate,
			                s_samples[snd].bits);
			if (!s_voices[s_voiceCount].voice)
				return MR_VOICE_NONE;
			s_voices[s_voiceCount].sound = snd;
			slot = s_voiceCount++;
		}
	}

	s_voices[slot].owned = 1;
	s_voices[slot].loop  = (flags & SND_FLAG_LOOP) != 0;
	s_voices[slot].held  = 0;
	xa2_voice_volume(s_voices[slot].voice, audio_group_gain(0));
	xa2_voice_pan(s_voices[slot].voice, 0.0f);
	xa2_voice_rate(s_voices[slot].voice, 1.0f);
	if (!xa2_voice_play(s_voices[slot].voice, s_samples[snd].pcm,
	                    s_samples[snd].pcmBytes,
	                    (flags & SND_FLAG_LOOP) != 0)) {
		s_voices[slot].owned = 0;
		return MR_VOICE_NONE;
	}
	audio_hold_if_paused(slot);
	return (MR_Voice)slot;
}

/* ORIGIN  Snd_StopVoice @ 0x00424000 */
void audio_voice_stop(MR_Voice v)
{
	if (!audio_voice_valid(v))
		return;
	xa2_voice_stop(s_voices[v].voice);
	s_voices[v].owned = 0;
}

/* ORIGIN  Snd_SetVolume @ 0x00424030 - 0..255 in the original. */
void audio_voice_set_volume(MR_Voice v, int vol)
{
	if (!audio_voice_valid(v))
		return;
	if (vol < 0)
		vol = 0;
	if (vol > 255)
		vol = 255;
	xa2_voice_volume(s_voices[v].voice,
	                 (vol / 255.0f) * audio_group_gain(0));
}

/*
 * ORIGIN  maybe_Snd_SetFrequency @ 0x00424050
 *
 * THE ARGUMENT IS ABSOLUTE HERTZ because DirectSound's was.
 * XAudio2 wants a multiple of the source's own rate, and the
 * source's rate is here - so the division belongs here and xa2.cpp
 * stays free of it.
 */
/*
 * ORIGIN  maybe_Snd_SetFrequency @ 0x00424050, which forwards to
 *         Csample_SetFrequency @ 0x00441dc0 - the guard at
 *         0x00441de6..0x00441df4
 *
 * THE WINDOW IS THE WHOLE POINT, and it is a SKIP rather than a
 * clamp:
 *
 *     [sample+0x14] = hz            stored whatever it is
 *     if (hz <= 1000)   return      too low  - no call
 *     if (hz >= 44100)  return      too high - NO CALL
 *     IDirectSoundBuffer::SetFrequency(hz)
 *
 * So above 44100 the buffer KEEPS THE LAST FREQUENCY IT WAS GIVEN.
 * The note does not clamp to 44100 and it does not jump - it simply
 * STOPS CLIMBING at whatever it last reached inside the window.
 *
 * WITHOUT THIS THE FIRST TWO GEARS SING. Road gears 1 and 2 run to
 * 48000 Hz on a 22050 Hz sample, so the top of each is outside the
 * window and the original stops tracking there - ratio 2.0 - while a
 * port that just sets the rate reaches 2.36 and is audibly sharp.
 * Gears 3 to 6 peak near 40000 on a 14000 Hz sample, inside the
 * window, which is why only the bottom two gears were wrong and why
 * this went unnoticed: every value feeding the pitch matched MOTO
 * exactly.  See hud-counter.txt 6.0 for the same shape of bug.
 *
 * 44100 IS NOT A DIRECTSOUND LIMIT. DSBFREQUENCY_MAX is 200000; this
 * ceiling is the game's own.
 */
#define AUDIO_FREQ_MIN 1000
#define AUDIO_FREQ_MAX 44100

void audio_voice_set_freq(MR_Voice v, int hz)
{
	int rate;

	if (!audio_voice_valid(v) || hz <= 0)
		return;
	if (hz <= AUDIO_FREQ_MIN || hz >= AUDIO_FREQ_MAX)
		return;
	rate = s_samples[s_voices[v].sound].sampleRate;
	if (rate <= 0)
		return;
	xa2_voice_rate(s_voices[v].voice, (float)hz / (float)rate);
}

/* ORIGIN  maybe_Snd_SetPan @ 0x00424070 */
void audio_voice_set_pan(MR_Voice v, float pan)
{
	if (!audio_voice_valid(v))
		return;
	xa2_voice_pan(s_voices[v].voice, pan);
}

/*
 * ORIGIN  Csample_LengthMs @ 0x00441fa0
 *
 *     frames = dataBytes / ((bitsPerSample >> 3) * channels)
 *     ms     = frames * 1000 / sampleRate
 *
 * THE TRUNCATION ORDER IS THE ORIGINAL'S: the byte count is
 * divided to whole frames BEFORE the multiply by 1000, so the
 * answer is floored to the frame rather than to the millisecond.
 * Keeping the order matters only at the margin, and costs nothing.
 */
int audio_cue_length_ms(int soundId)
{
	const MR_Wave *w;
	unsigned int   frameBytes;

	if (soundId < 0 || soundId > 0xff)
		return 0;
	if (s_handles[soundId] == MR_SOUND_NONE)
		return 0;

	w          = &s_samples[s_handles[soundId]];
	frameBytes = (unsigned int)(w->bits / 8) *
	             (unsigned int)w->channels;
	if (frameBytes == 0 || w->sampleRate <= 0)
		return 0;
	return (int)((w->pcmBytes / frameBytes) * 1000u /
	             (unsigned int)w->sampleRate);
}

/*
 * ORIGIN  FUN_00441e80 @ 0x00441e80, via FUN_00424090.
 * See audio.h for the two rules this keeps and the one it
 * deliberately breaks.
 */
/* ORIGIN  NONE - ours. See audio.h on why the original has none. */
static int s_muteUser;
static int s_muteFocus;

static void audio_apply_mute(void)
{
	xa2_master_volume((s_muteUser || s_muteFocus) ? 0.0f : 1.0f);
}

void audio_set_muted(int muted)
{
	s_muteUser = muted ? 1 : 0;
	audio_apply_mute();
	mrlog("audio:  %s\n", s_muteUser ? "muted" : "unmuted");
}

int audio_muted(void)
{
	return s_muteUser;
}

void audio_set_focus(int hasFocus)
{
	s_muteFocus = hasFocus ? 0 : 1;
	audio_apply_mute();
}


void audio_set_paused(int paused)
{
	int i;

	paused = (paused != 0);
	if (paused == s_paused)
		return;
	s_paused = paused;

	for (i = 0; i < s_voiceCount; i++) {
		if (!s_voices[i].voice)
			continue;
		if (paused) {
			/*
			 * LOOPS ONLY, and only ones actually sounding -
			 * the original's GetStatus PLAYING test plus its
			 * flag bit 0. A one-shot in the air finishes,
			 * which is what lets the menu cue through.
			 */
			if (s_voices[i].loop &&
			    xa2_voice_busy(s_voices[i].voice)) {
				xa2_voice_pause(s_voices[i].voice);
				s_voices[i].held = 1;
			}
		} else if (s_voices[i].held) {
			xa2_voice_resume(s_voices[i].voice);
			s_voices[i].held = 0;
		}
	}
	mrlog("audio:  %s%s\n", paused ? "paused" : "resumed",
	      paused ? " (loops held)" : "");
}

int audio_paused(void)
{
	return s_paused;
}

int audio_voice_playing(MR_Voice v)
{
	if (!audio_voice_valid(v))
		return 0;
	return xa2_voice_busy(s_voices[v].voice);
}

int audio_volume(int channel)
{
	if (channel < 0 || channel >= AUDIO_VOL_COUNT)
		return 0;
	return s_volume[channel];
}

void audio_set_volume(int channel, int value)
{
	if (channel < 0 || channel >= AUDIO_VOL_COUNT)
		return;
	if (value < 0)
		value = 0;
	if (value > AUDIO_VOL_MAX)
		value = AUDIO_VOL_MAX;
	s_volume[channel] = value;
}

void audio_stop_all(void)
{
	int i;

	for (i = 0; i < s_voiceCount; i++)
		xa2_voice_stop(s_voices[i].voice);
	s_lastCue[0] = -1;
	s_lastCue[1] = -1;
	s_musicTrack = 0;
}
