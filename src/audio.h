/*
 * audio.h - the audio seam.
 *
 * The same shape as render.h: a back-end-agnostic C interface that the
 * game calls, with the actual API behind it. render.h earned that
 * shape because DirectDraw had to be replaced wholesale; audio has the
 * same problem twice over. The back end is XAudio2, in xa2.cpp, and
 * nothing above this header names it.
 *
 *
 * WHAT THE ORIGINAL DOES [verified, from the import table]:
 *
 *   MUSIC IS RED BOOK CD AUDIO, driven through mciSendCommandA. The
 *   music is not a file at all - it is tracks 2..n of the game disc,
 *   played by the drive. There is nothing to decode and nothing to
 *   mix; the game asks the CD to play a track and the analogue output
 *   lands in the mixer. That is also why the protection patch at
 *   0x00444e73 is described as "CD-audio playback": no disc, no music.
 *
 *   SOUND EFFECTS ARE DirectSound, with the winmm mixer* API alongside
 *   for volume.
 *
 * NEITHER SURVIVES THE PORT. Nobody has the disc in the drive, and
 * DirectSound is deprecated - it still loads on Windows 10 but is
 * emulated, gets no hardware mixing, and is not something to build on
 * in 2026.
 *
 *
 * SO THE MUSIC HAS TO COME FROM FILES, and that is a genuine content
 * question rather than a code one: a stock install has no music files,
 * because the music was on the disc. Whatever we do here, a drop-in
 * EXE cannot conjure the soundtrack out of an installed DATA folder.
 * That is a real limit of CLAUDE.md rule 10 and it should be recorded
 * as one rather than discovered later:
 *
 *   - SFX come out of the archives and are fine.
 *   - MUSIC needs files the user supplies - ripped from their own
 *     disc, most likely - in a directory we define.
 *
 * The interface below therefore keeps music addressed BY TRACK NUMBER,
 * exactly as the original addresses it. A back end resolves track 4 to
 * whatever it can find; the game code never learns the difference.
 *
 *
 * THE SAMPLES ARE NOT PLAIN WAV FILES. Every one of SFX.BKF's 250
 * members carries a .WAV extension and none of them is RIFF - all
 * 250 begin "rmnr" [verified]. They are ordinary uncompressed PCM
 * RIFF WAVE under a positional cipher; wave.c reads them and
 * docs/formats/rmnr.txt owns the format. A back end that hands
 * these bytes straight to a WAV loader gets nothing - the same trap
 * as the texture pages, which are "mhwanh" under a .RAW
 * extension.
 *
 * The path the original builds is
 *
 *     data\sfx\ + (16_Bits\ | 8_Bits\) + <category> + <name> + .wav
 *
 * and SFX.BKF holds both trees, 125 members each. Under rule 10 that
 * is an archive member name, not a path: strip "data\" and the rest
 * is the member.
 *
 *
 * THE BACK END IS XAudio2, chosen for the constraint that chose D3D9
 * for the renderer: it ships in Windows 10 as XAudio2_9.dll with no
 * redistributable.
 *
 * MUSIC DECODING IS STILL OPEN. Media Foundation decodes MP3, AAC
 * and WMA with no extra libraries and does not decode FLAC or Opus;
 * dr_flac and libopus are a single header and a small static library
 * if those are wanted. The point of this header is that the choice
 * cannot leak into game code either way. [open]
 */

#ifndef MRACER_AUDIO_H
#define MRACER_AUDIO_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A loaded sound effect. Opaque handle, 0 meaning none, so callers can
 * hold one in a struct without knowing what a back end stores.
 */
typedef int MR_Sound;
#define MR_SOUND_NONE 0

/*
 * audio_init - bring the device up. Returns 0 on failure.
 *
 * FAILURE IS NOT FATAL AND MUST NOT BE. A machine with no audio device
 * should run the game silently, not refuse to start. Every call below
 * is a no-op when the device is down.
 */
int  audio_init(void);
void audio_shutdown(void);
int  audio_ready(void);

/*
 * audio_update - called once per frame from the main loop.
 *
 * AUDIO GETS NO THREAD IN GAME CODE. XAudio2 runs its own mixing
 * thread; what is left on our side is starting and stopping voices
 * and advancing streamed music, which is per-frame work. A music
 * stream does want a worker so a disk hitch cannot stall a frame -
 * but that worker belongs INSIDE the back end, behind this call.
 */
void audio_update(void);

/* Master volumes, 0.0 to 1.0. */
void audio_set_music_volume(float v);
void audio_set_sfx_volume(float v);

/*
 * MUSIC, addressed by CD track number the way the original does.
 * track 1 is the data track, so music starts at 2.
 */
void audio_music_play(int track, int loop);
/*
 * audio_music_stop is CdAudio_StopCue (0x004049e0) - MCI_STOP and
 * clear the repeat flag. Its ONE caller in the whole binary is
 * MenuPage25_DsiLogo_Leave, at 0x00453bff [verified].
 *
 * LEAVING PAGE 25 HAS TO STOP THE MUSIC. Track 13 starts with mode 0
 * so it does not loop, but the intro can be skipped at any point,
 * and without this the intro would keep playing underneath the title
 * page while track 3 was asked to start. Nothing else in the game
 * stops the CD explicitly; every other page just asks for its own
 * track.
 */
void audio_music_stop(void);
int  audio_music_playing(void);

/*
 * ---------------------------------------------------------------
 * THE GAME'S OWN CUES, BY NUMBER
 * ---------------------------------------------------------------
 *
 * The two entry points below are the shape the ORIGINAL calls, and
 * they are deliberately not the load/play pair further down. Game code
 * never holds a sound handle: it passes a small integer, and a table
 * at 0x00519ff8 turns that into a handle.
 *
 *   audio_cue        FUN_004049f0(soundId, flags)
 *   audio_music_cue  FUN_00404990(track, mode)
 *
 * audio_cue - play a sound effect by id. FUN_004049f0 [verified].
 *
 * THE ID SPACE IS SPLIT IN TWO and the split is not where you would
 * guess: ids 0x2f..0x5c are SPEECH and EVERYTHING ELSE is effects.
 * Each group has its own enable flag, its own volume and its own
 * "last id played" for the retrigger guard, so the id alone decides
 * which setting silences a cue. See docs/subsystems/audio-cues.txt
 * 1.1.
 *
 * THE ID DECIDES THE GROUP, THE BANK DECIDES THE FILE, and a port
 * must not collapse the two. A handle table of 256 entries is filled
 * by whichever bank is resident - menu or race - and the SAME id
 * names different files in each: 0x29 is Menu\select.wav in the
 * menus and Divers\select.wav in a race. So "cue 0x29" is only
 * meaningful together with the current bank, and a global
 * id-to-filename map would be wrong. See
 * docs/subsystems/audio-cues.txt section 5.
 *
 * The three flag bits are decoded in three different functions,
 * so reading Snd_PlayCue alone makes them look like one flag.
 */
#define SND_FLAG_LOOP         0x01  /* DSBPLAY_LOOPING               */
#define SND_FLAG_POLYPHONIC   0x02  /* duplicate rather than restart */
#define SND_FLAG_NO_RETRIGGER 0x04  /* skip if this id was the last  */
                                    /* one played in its group       */

/*
 * The ids read off their call sites [verified]. Only cues the
 * reconstruction actually reaches are named; the full table is in
 * docs/subsystems/audio-cues.txt.
 */
/*
 * 0x03 IS THE IN-RACE MENU'S OPEN SOUND, and it is a race-bank
 * cue: Divers\continue.wav. InGameMenu_OpenEscape plays it
 * at 0x00436446 and the post-race question plays it again from
 * FUN_00437b70. Table C carries the same id for the menus, so
 * like 0x28 and 0x29 it names a different file per bank.
 */
#define SND_MENU_OPEN       0x03
#define SND_MENU_MOVE       0x28    /* Menu\deplett.wav             */
#define SND_MENU_HOVER      0x29    /* Menu\select.wav              */
/*
 * 0x2a IS CONFIRM, NOT GLASS. valide.wav is French for "confirm";
 * Menu_Tick fires it whenever a transition ARMS, and every page
 * change arms the glass, so it merely plays OVER the shatter. No
 * sample in SFX.BKF is glass-named - nothing matching verre, vitre,
 * glass, casse, bris or break among the 250.
 */
#define SND_TRANSITION      0x2a    /* Menu\valide.wav              */
/*
 * THE TWO BIKE START SAMPLES, chosen by the bike's PERSOANIM -
 * carac 72, not MotoType. Page 10 computes (PersoAnim == 0) + 0x2b
 * at 0x0044f5d1, so Speed bikes (PersoAnim 0) get 0x2c goninja and
 * Motocross (1) gets 0x2b goktm [verified].
 *
 * MotoType is 2 and 1 for the two disciplines and never 0, so
 * testing it instead gives goktm for everything. See menu.c.
 */
#define SND_BIKE_START_A    0x2b    /* Menu\goktm.wav               */
#define SND_BIKE_START_B    0x2c    /* Menu\goninja.wav             */
#define SND_TITLE_BIKE_PASS 0x2d    /* Menu\title.wav               */
/*
 * THE MENU SPEECH TABLE, read out of bank B at 0x004db780 - eleven
 * {name, id} pairs loaded from Voices\Menu\ when voices are on
 * [verified]. The ids are contiguous from 0x52 apart from entrname
 * at 0x36 - which is still inside the 0x2f..0x5c speech range, so
 * it is grouped and gated as speech like the other ten. Not
 * contiguous is not the same as not speech.
 */
/*
 * THE RACE COMMENTATOR.  Every one of these is in the speech
 * range 0x2f..0x5c and so is gated on the voices setting and
 * scaled by the speech volume - except the two beeps and the
 * checkpoint blip, which are effects and are listed here because
 * they play alongside a spoken line.
 *
 * audio-cues.txt 8.2 names the site that queues each one.
 */
#define SND_BIP             0x01    /* Divers\bip1 - 3, 2, 1      */
#define SND_BIP_GO          0x02    /* Divers\bip2 - GO           */
#define SND_CHECKPOINT_BLIP 0x06    /* Divers\chckpvox            */
#define SND_VOICE_THREE     0x30
#define SND_VOICE_TWO       0x31
#define SND_VOICE_ONE       0x32
#define SND_VOICE_GO        0x33
#define SND_VOICE_CKCKPNT   0x34    /* checkpoint               */
#define SND_VOICE_FINLAP    0x37    /* final lap                */
#define SND_VOICE_FUEL      0x38
#define SND_VOICE_GAMEOVER  0x39
#define SND_VOICE_NEWRECOR  0x3b
#define SND_VOICE_ONEGEAR   0x3c
#define SND_VOICE_RACECMPL  0x3e
#define SND_VOICE_RUNLAST   0x3f
#define SND_VOICE_RUNNING4  0x40
#define SND_VOICE_STILLRUN  0x41
#define SND_VOICE_TIMEOUT   0x42
#define SND_VOICE_WATAROKT  0x45
#define SND_VOICE_WRONGWAY  0x46
#define SND_VOICE_YOULOST   0x47    /* finished 4th or worse    */
#define SND_VOICE_YURALOSR  0x48    /* finished 3rd or better   */
#define SND_VOICE_TRYAGAIN  0x44    /* the continue menu        */
#define SND_VOICE_YURDALST  0x49    /* running last             */
#define SND_VOICE_YEAAH     0x4f    /* won, one of two          */
#define SND_VOICE_WELLDONE  0x50    /* won, the other           */

/* The announcer's delay, and every one of the ten sites uses it. */
#define SND_VOICE_DELAY_MS  500

#define SND_VOICE_ENTRNAME  0x36    /* entrname - page 15, name entry */
#define SND_VOICE_SLCTRANS  0x52    /* slctrans                       */
#define SND_VOICE_SELERACE  0x53    /* selerace - page 6              */
#define SND_VOICE_SELECTMD  0x54    /* selectmd - page 5, mode select */
#define SND_VOICE_PLAYSOLO  0x55    /* playsolo - page 3 SOLO         */
#define SND_VOICE_PLAYLINK  0x56    /* playlink - page 3 LINK         */
#define SND_VOICE_MAKURCHC  0x57    /* makurchc                       */
#define SND_VOICE_LETSGO    0x58    /* letsgo                         */
#define SND_VOICE_CNGRECOR  0x59    /* cngrecor                       */
#define SND_VOICE_CNGRATIM  0x5a    /* cngratim                       */
#define SND_VOICE_YUTRYGAN  0x5b    /* yutrygan                       */

/* An older name for the same cue, kept because it reads better at
 * the call site than the eight-letter file name does. */
#define SND_PODIUM          SND_VOICE_YUTRYGAN

/*
 * RETURNS THE CUE ID, OR -1 WHEN NOTHING STARTED - not a voice.
 * Out of range, group disabled, no sample loaded, retrigger guard,
 * no free voice and device down all give -1.
 *
 * THE ORIGINAL GATES A DELAY ON THAT RETURN. Page 3 arms a deadline
 * only when Snd_PlayCue actually started the line, so a build with
 * no audio never waits and the menus stay responsive - rather than
 * a special case bolted on for one. See MenuPage03_MainMenu_Tick
 * at 0x00442e37, and page 10 in menu.c for the shape in the port.
 */
int  audio_cue(int soundId, unsigned int flags);

/*
 * audio_cue_delayed - play soundId after delayMs.
 *
 * FUN_00404e10 (0x00404e10) is VARIADIC: an int count followed by
 * that many (delayMs, soundId) pairs. It walks a table of eight
 * slots of 100 bytes each at 0x00504244, takes the first free one,
 * and appends up to eight pairs to it [verified].
 *
 * o SO ONE CALL IS ONE GROUP OF CUES, not one cue. ALL FOUR MENU
 *   SITES PASS A COUNT OF 1 - page 3 makurchc, page 5 selectmd,
 *   page 6 selerace and page 15 entrname, every one at 600 ms
 *   [verified, the four xrefs to 0x00404e10]. So the group is never
 *   used for what it can do, and the single-cue form is all the
 *   reconstruction needs until a site turns up that wants more.
 *
 * Returns 0 when the queue is full. The cue goes out through
 * audio_cue when it comes due.
 */
int  audio_cue_delayed(int soundId, int delayMs);
void audio_cue_delayed_reset(void);

/*
 * audio_music_cue - start a CD music track. FUN_00404990 [verified].
 *
 * mode is TWO INDEPENDENT BITS, read out of FUN_004b19f0's tests
 * rather than guessed:
 *
 *     bit 0 (1)  do not restart if this track is already playing
 *     bit 1 (2)  repeat when it ends
 *
 * which is why the main menu passes 3 and the title page passes 2:
 * both loop, but only the main menu survives being re-entered without
 * restarting the music.
 *
 * TRACK 0 MEANS RANDOM, not silence - FUN_004b18e0 picks
 * rand() % trackCount + 1 when the track is 0, and treats -1 as "leave
 * whatever is playing alone".
 */
#define CD_MODE_ONCE      0
#define CD_MODE_KEEP      1
#define CD_MODE_LOOP      2
#define CD_MODE_LOOP_KEEP 3

#define CD_TRACK_MENU     3         /* pages 3 and 14                */
#define CD_TRACK_INTRO    0x0d      /* page 25, the DSI logo         */

void audio_music_cue(int track, int mode);

/*
 * WHICH BANK IS RESIDENT, because a cue id alone does not identify a
 * file. The original has two loaders - Snd_LoadMenuBank, called from
 * Menu_LoadResources, and Snd_LoadRaceBank, called per bike - and
 * each fills the same 256-entry handle table from different
 * directories. Id 0x29 is Menu\select.wav under one and
 * Divers\select.wav under the other.
 *
 * audio_load_bank loads the bank's samples and records which one is
 * resident, so a cue logged against the wrong bank is visible rather
 * than silently mapped to the wrong sample.
 */
#define AUDIO_BANK_NONE       0
#define AUDIO_BANK_MENU       1     /* Menu\ + Voices\Menu\           */
#define AUDIO_BANK_RACE_NINJA 2     /* Ninja\ + Divers\ + Voices\     */
#define AUDIO_BANK_RACE_KTM   3     /* Ktm\   + Divers\ + Voices\     */

void audio_load_bank(int bank);
int  audio_current_bank(void);

/*
 * SOUND EFFECTS. The logical name is an asset name in the assets.c
 * sense - resolved out of a .BKF, never a path. See CLAUDE.md rule 10.
 */
MR_Sound audio_sound_load(const char *logicalName);
void     audio_sound_free(MR_Sound s);

/*
 * THE THREE VOLUMES THE OPTIONS PAGE DRIVES.
 *
 * The original has no volume of its own. FUN_00470c70, FUN_00470d90
 * and FUN_00470e90 read and step a WINDOWS MIXER LINE through
 * mixerGetControlDetailsA and mixerSetControlDetails, one per
 * channel; the page only ever sees a 0..max integer.
 *
 * o THE CHANNEL NUMBERING IS THE BINARY AND IT IS NOT ROW ORDER.
 *   The master row passes 0, the WAVE row passes 2 and the CD row
 *   passes 1 [verified, the three calls from 0x00443e85 on].
 *
 * o THE RANGE IS 0..32. FUN_004708c0 writes 0x20 into all three max
 *   fields at startup [verified], and the slider bar is drawn by
 *   shrinking it by 100 - volume * 100 / max.
 *
 * o WE DO NOT TOUCH THE SYSTEM MIXER. Moving the Windows
 *   volume from inside a game is 1997 behaviour, and the CD volume
 *   has nothing to drive anyway now that the tracks are local files.
 *   These are our own values for the audio back end to apply. A
 *   deliberate divergence, not a transcription gap.
 */
#define AUDIO_VOL_MASTER 0
#define AUDIO_VOL_CD     1
#define AUDIO_VOL_WAVE   2
#define AUDIO_VOL_COUNT  3
#define AUDIO_VOL_MAX    32

int  audio_volume(int channel);

/*
 * THE TWO GROUP VOLUMES, kept apart because the ORIGINAL keeps
 * them apart: g_nCfgSfxVolume and g_nCfgVoicesVolume are two
 * separate config words and every play site picks one of them by
 * the cue id - see audio_cue_is_speech and the 0x2f..0x5c rule
 * above.
 *
 * THE OPTIONS SCREEN NEVER EXPOSED THEM.  Both are written once
 * at startup and nothing in the menus moves them, which is why
 * the split is invisible in play and easy to flatten by
 * accident.  Keep it: an engine that scales speech and effects
 * together cannot be given the control later without revisiting
 * every call site.
 *
 * 0..AUDIO_VOL_MAX, like the mixer channels.  Both default to
 * full, so nothing sounds different until something sets them.
 */
#define AUDIO_GROUP_SFX    0
#define AUDIO_GROUP_SPEECH 1
#define AUDIO_GROUP_COUNT  2

void audio_set_group_volume(int group, int value);
int  audio_group_volume(int group);
void audio_set_volume(int channel, int value);

/*
 * The gain the MUSIC plays at: MasterVolume times CDVolume,
 * each 0..AUDIO_VOL_MAX, as a multiplier in 0..1.
 *
 * WaveVolume is deliberately not part of it - see the
 * definition.  music.c reads this every frame.
 */
float audio_music_gain(void);

/*
 * Fire a loaded sample once. volume is 0.0 to 1.0, pan -1.0 left to
 * +1.0 right, pitch 1.0 for unmodified.
 *
 * NON-ZERO ON SUCCESS, AND NOT A VOICE HANDLE - it cannot be
 * retuned or stopped. Use audio_voice_start below for anything that
 * has to be held open.
 */
int  audio_sound_play(MR_Sound s, float volume, float pan, float pitch);
void audio_stop_all(void);

/*
 * Load one sample straight into a cue id, and read a cue's own
 * sample rate. Both exist for objsnd.c, whose ids are assigned
 * at track load rather than compiled in - see objsnd.h.
 */
int  audio_bind_cue(int soundId, const char *category,
                    const char *base);
int  audio_cue_rate(int soundId);

/*
 * THE HANDLE LAYER - a SECOND audio interface, not a helper on the
 * first one.
 *
 * ORIGIN  the five methods on the sound manager at 0x00516aa0:
 *         maybe_Snd_StartVoice   0x00423fd0  (cue, mode) -> voice
 *         Snd_StopVoice          0x00424000
 *         Snd_SetVolume          0x00424030
 *         maybe_Snd_SetFrequency 0x00424050
 *         maybe_Snd_SetPan       0x00424070
 *
 * audio_cue above is FIRE AND FORGET: it starts a sample and the
 * caller never hears of it again. That is the whole of the menu.
 * A RACE NEEDS THE OTHER SHAPE - hold a looping voice open for as
 * long as the bike is running and retune it every frame - and no
 * amount of audio_cue does that. See audio-cues.txt 9.
 *
 * A voice handle is owned by whoever started it. The cue layer will
 * not recycle it, so a handle stays valid until audio_voice_stop -
 * which is what makes it safe to keep one in a struct for a whole
 * race.
 */
typedef int MR_Voice;
#define MR_VOICE_NONE (-1)

/*
 * Start soundId and keep the voice. flags is the same word audio_cue
 * takes, so SND_FLAG_LOOP is how an engine note loops. Returns
 * MR_VOICE_NONE when the id has no sample, the device is down, or
 * the pool is full - all of which the caller treats the same way,
 * because every call below drops an invalid handle rather than
 * acting on it.
 */
MR_Voice audio_voice_start(int soundId, unsigned int flags);
void     audio_voice_stop(MR_Voice v);

/* 0..255, the original's range. Scaled by the group gain. */
void     audio_voice_set_volume(MR_Voice v, int vol);

/*
 * ABSOLUTE HERTZ, as DirectSound took. The conversion to XAudio2's
 * ratio needs the sample's own rate, which is why it happens here
 * and not in the caller.
 */
void     audio_voice_set_freq(MR_Voice v, int hz);

/* -1.0 left, 0.0 centre, +1.0 right. */
void     audio_voice_set_pan(MR_Voice v, float pan);

/* Non-zero while the voice still has audio queued. */
int      audio_voice_playing(MR_Voice v);

/*
 * audio_cue_length_ms - how long the sample behind a cue id runs.
 *
 * ORIGIN  Csample_LengthMs @ 0x00441fa0, reached through
 *         Snd_VoiceLengthMs @ 0x004240d0.
 *
 * THE MENU NEEDS THIS TO KNOW HOW LONG TO WAIT. Page 10 holds the
 * page change until its START cues have finished, and this is how
 * the original measures them. 0 when the id has no sample, which
 * is also "do not wait".
 */
int      audio_cue_length_ms(int soundId);

/*
 * audio_set_paused - silence the race without losing it.
 *
 * ORIGIN  FUN_00441e80, reached through FUN_00424090(1) from
 *         InGameMenu_OpenEscape at 0x00436446.
 *
 * ONLY LOOPING VOICES PAUSE, and that is the original's own
 * rule, not a simplification: it tests GetStatus for PLAYING
 * and the sample's flag word for bit 0, the LOOP bit. A
 * one-shot already in the air runs to completion - which is
 * why the escape menu can play its own cue on the frame it
 * pauses, and why nothing has to special-case the menu.
 *
 * DIVERGENCE, AND IT IS AN IMPROVEMENT: THE ORIGINAL RESTARTS
 * EACH LOOP. Its resume path calls Csample_Play, which does
 * SetCurrentPosition(0) before Play, so every engine note
 * jumps back to the top of its sample. XAudio2 can stop a
 * voice without discarding its buffer, so ours carries on
 * from the sample it was on. Same set of voices, no seam.
 *
 * Idempotent - calling it with the state it is already in does
 * nothing, so a caller may pass a flag every frame.
 */
void     audio_set_paused(int paused);

/*
 * audio_set_muted - the player's mute, and the window's.
 *
 * TWO INDEPENDENT REASONS, both scaling the mastering voice, so
 * alt-tabbing away and back does not undo a mute the player asked
 * for. Nothing here touches a group or per-voice volume, so the
 * options screen is unaffected.
 *
 * The original needed neither: it is DirectSound, and a buffer
 * without DSBCAPS_GLOBALFOCUS is silenced by the OS on focus loss
 * [inferred - maybe_Event_WndProc calls no audio function at all].
 * XAudio2 keeps playing, which is why this exists.
 */
void     audio_set_muted(int muted);
int      audio_muted(void);
void     audio_set_focus(int hasFocus);
int      audio_paused(void);

#ifdef __cplusplus
}
#endif

#endif /* MRACER_AUDIO_H */
