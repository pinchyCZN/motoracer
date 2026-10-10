/*
 * xa2.h - the XAudio2 COM surface, stated in C.
 *
 * WHY THIS FILE EXISTS: xaudio2.h does not compile as C, so audio.c
 * cannot see a single XAudio2 type. audio.c's banner names the two
 * offending lines and says why neither can be worked around.
 *
 * SO THE RULE HERE IS: NO POLICY. Every function below is a direct
 * translation of one COM call or one obvious pair of them. Which
 * voice to use, when to make another, what volume, whether to loop -
 * all of that is audio.c's, in C. If a function here starts taking a
 * decision, it is in the wrong file.
 *
 * The handles are opaque on purpose: a caller that could reach the
 * interface pointer would start calling through it.
 */

#ifndef MRACER_XA2_H
#define MRACER_XA2_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * xa2_open - the engine and the mastering voice.
 *
 * Returns 0 if audio is unavailable for any reason - no device, no
 * xaudio2_9.dll, the endpoint held exclusively - having already said
 * which in the log. THAT IS NOT AN ERROR THE GAME REACTS TO: it is
 * the silent path, which audio.c has to support regardless.
 *
 * Idempotent; a second call while open returns 1 and does nothing.
 */
int  xa2_open(void);
void xa2_close(void);

/*
 * A SOURCE VOICE - the playback half of what DirectSound fused into
 * one object. The samples are NOT here: the caller owns the PCM and
 * must keep it alive and unmoved until playback ends, because
 * XAudio2 reads the block in place rather than copying it. That is
 * the one real difference from IDirectSoundBuffer and it is why
 * audio.c holds the bank for as long as the bank is resident.
 *
 * A voice is fixed to the format it was created with, so the pool
 * that hands them out is keyed by format. NULL on failure.
 */
typedef struct XA2_Voice XA2_Voice;

XA2_Voice *xa2_voice_create(int channels, int sampleRate, int bits);
void       xa2_voice_destroy(XA2_Voice *v);

/* Non-zero while a buffer is still queued on this voice. */
int  xa2_voice_busy(XA2_Voice *v);

/*
 * xa2_voice_play - REWIND AND PLAY, which is deliberate.
 *
 * Csample_Play calls SetCurrentPosition(0) before every Play, so a
 * cue fired again on the same voice restarts rather than resuming.
 * Stop + FlushSourceBuffers + Submit + Start is the same thing in
 * XAudio2 terms. loop maps to XAUDIO2_LOOP_INFINITE, the
 * DSBPLAY_LOOPING that cue flag bit 0 carries.
 */
int  xa2_voice_play(XA2_Voice *v, const void *pcm, unsigned int bytes,
                    int loop);
void xa2_voice_stop(XA2_Voice *v);

/*
 * THE STREAMING THREE, and they exist because xa2_voice_play cannot
 * stream: it FLUSHES before every submit, which is right for a cue
 * fired twice and useless for a 45 MB music track.
 *
 * xa2_voice_submit QUEUES a block without disturbing what is already
 * playing, xa2_voice_start begins playback once enough is queued,
 * and xa2_voice_queued reports the depth so the caller knows when to
 * hand over the next one.
 *
 * THE CALLER STILL OWNS THE BYTES. XAudio2 reads a submitted block
 * in place, so a ring buffer must not overwrite a slot until the
 * voice has finished with it - which is what the depth is for.
 *
 * LOOPING IS NOT HERE ON PURPOSE. XAUDIO2_LOOP_INFINITE repeats ONE
 * buffer, which for a stream would repeat two seconds for ever; a
 * streamed loop is the reader seeking back, not the voice.
 */
int  xa2_voice_submit(XA2_Voice *v, const void *pcm, unsigned int bytes);
int  xa2_voice_start(XA2_Voice *v);
int  xa2_voice_queued(XA2_Voice *v);

/*
 * PAUSE IS A STOP THAT DOES NOT FLUSH, and that is the whole
 * difference. xa2_voice_stop above discards the queued buffer,
 * so the voice has nothing to go back to; this leaves the
 * buffer and the play cursor alone, and xa2_voice_resume picks
 * up at the same sample.
 */
void xa2_voice_pause(XA2_Voice *v);
void xa2_voice_resume(XA2_Voice *v);

/* 0.0 silent, 1.0 unattenuated. */
void xa2_voice_volume(XA2_Voice *v, float vol);

/*
 * The MASTERING voice, so it scales everything at once and
 * leaves every per-voice and per-group volume alone. Resets to
 * 1.0 if the engine is reopened.
 */
void xa2_master_volume(float vol);

/*
 * xa2_voice_rate - playback speed as a RATIO, not a frequency.
 *
 * DirectSound's SetFrequency takes absolute Hz; XAudio2 takes a
 * multiple of the source's own rate. The conversion needs the
 * sample's native rate, which is audio.c's to know, so it happens
 * there and this stays a ratio.
 *
 * THE CEILING IS FIXED AT VOICE CREATION and a ratio above it is
 * CLAMPED SILENTLY - see XA2_MAX_FREQ_RATIO in xa2.cpp for why the
 * default 2.0 is not enough.
 */
void xa2_voice_rate(XA2_Voice *v, float ratio);

/*
 * xa2_voice_pan - -1.0 hard left, 0.0 centre, +1.0 hard right.
 *
 * A no-op unless the source is mono and the device stereo, which is
 * every sample the game ships and the usual output. Anything else
 * is left centred rather than guessed at.
 */
void xa2_voice_pan(XA2_Voice *v, float pan);

#ifdef __cplusplus
}
#endif

#endif /* MRACER_XA2_H */
