/*
 * music.h - CD music, streamed from a .cue / .bin rip.
 *
 * THE ORIGINAL PLAYS THE CD AND WE CANNOT. MOTO.EXE drives the drive
 * through mciSendCommandA and asks for a TRACK NUMBER; there is no
 * file and no decoder anywhere in it. A drop-in replacement has to
 * put something in that place, and a .bin/.cue rip of the same disc
 * is the closest thing to the original source there is: the audio
 * tracks are the CD's own bytes, uncompressed and unconverted.
 *
 * SO THE TRACK NUMBERS ARE THE ORIGINAL'S. audio_music_cue asks for
 * track 3 or track 13 exactly as it always did; this layer turns a
 * number into a file offset. Nothing above it knows a cue sheet
 * exists.
 *
 * IT IS OPTIONAL AND IT FAILS QUIET. No key in the ini, a missing
 * file, or a cue that does not parse leaves music_init returning 0
 * and every later call a no-op - the game runs exactly as it does
 * today, silently. That is deliberate: CLAUDE.md rule 10 wants an
 * executable that works dropped into a stock install, and a stock
 * install has no rip.
 *
 * WHY NOT asset_load. Rule 10 is about the game's own data, which
 * lives in the .BKF archives and must be read from them. A music rip
 * is the USER'S file, named by absolute path in the ini and sitting
 * wherever they keep it, so it is opened with fopen like any other
 * outside file. See music.c.
 */

#ifndef MRACER_MUSIC_H
#define MRACER_MUSIC_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Parse the cue named by [PATHS] CueSheet and get ready to play.
 *
 * Returns 1 when at least one audio track is available, 0 otherwise.
 * Safe to call when the key is absent; that is the ordinary case.
 */
int  music_init(void);

/* Stop, close the file and drop the track table. */
void music_free(void);

/*
 * THE PUMP. Call once per frame from audio_update.
 *
 * Reads the next block off disk and hands it to the voice whenever
 * the queue has room. Nothing happens if no track is playing.
 */
void music_tick(void);

/*
 * Start a track, 1-based, exactly as the game numbers them. loop
 * repeats from the track's own start when it ends.
 *
 * Returns 1 if the track is now playing.
 */
int  music_play(int track, int loop);

void music_stop(void);

/* Non-zero while a track is playing. */
int  music_playing(void);

/*
 * Which track is playing, or 0. This is what answers CD_MODE_KEEP -
 * "do not restart if this track is already playing" - which the main
 * menu relies on to survive being re-entered.
 */
int  music_current_track(void);

/*
 * How many AUDIO tracks the cue holds - the data track is not
 * counted. Track 0 means RANDOM in the original, and this is the
 * count that random is taken over.
 */
int  music_track_count(void);

/*
 * The n'th audio track's number, 0-based, so a random pick does not
 * have to assume the audio tracks are contiguous or that they start
 * at 2. Returns 0 when n is out of range.
 */
int  music_track_at(int n);

#ifdef __cplusplus
}
#endif

#endif /* MRACER_MUSIC_H */
