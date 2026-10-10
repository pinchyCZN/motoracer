/*
 * music.c - the cue sheet parser and the streaming player.
 *
 * ORIGIN  NONE - ours. MOTO.EXE plays the CD through
 *         mciSendCommandA and has no file, no decoder and no cue
 *         sheet anywhere in it. See music.h for why this exists.
 *
 * TWO HALVES, ONE FILE. The parser has no caller but the player and
 * the player has no source but the parser, so a cuesheet.h would be
 * a header nothing else includes - which CLAUDE.md 13 names as the
 * sign of an imaginary module. They are sections here instead.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "music.h"
#include "config.h"
#include "options.h"
#include "audio.h"
#include "log.h"
#include "timing.h"
#include "xa2.h"

/* ================================================================== */
/* RED BOOK AUDIO, which is what a .bin of an AUDIO track holds.      */
/* ================================================================== */

/*
 * 44100 Hz, 16 bit, stereo, little endian - and that is XAudio2's
 * own format, so there is no decode and no conversion anywhere in
 * this file. The bytes go from the file to the voice untouched.
 */
#define CD_SECTOR_BYTES   2352
#define CD_SECTORS_PER_S  75
#define CD_BYTES_PER_S    (CD_SECTOR_BYTES * CD_SECTORS_PER_S)

#define MUSIC_RATE        44100
#define MUSIC_CHANNELS    2
#define MUSIC_BITS        16

/*
 * TWO SECONDS A BUFFER, THREE BUFFERS - about a megabyte, and the
 * size is chosen against LOADING rather than against the frame rate.
 *
 * The pump runs from audio_update, so a frame that takes 30 ms needs
 * nothing like this much. What it has to survive is a TRACK LOAD:
 * the frame loop stops for as long as the level takes to read, and
 * an underrun there is an audible gap exactly where the original's
 * CD kept playing. Six seconds of queued audio covers it.
 *
 * A dedicated thread would remove the question entirely and is the
 * honest long answer; it also needs a lock around the ring. This is
 * the measurable version - if a load ever outlasts six seconds the
 * log says so.
 */
#define MUSIC_BUF_SECTORS 150                     /* exactly 2.0 s */
#define MUSIC_BUF_BYTES   (MUSIC_BUF_SECTORS * CD_SECTOR_BYTES)
#define MUSIC_BUFFERS     3

#define MUSIC_MAX_TRACKS  32

typedef struct {
	int  number;                 /* the CD's own track number */
	char path[MAX_PATH];         /* the .bin, resolved        */
	long startByte;              /* INDEX 01, in bytes        */
	long lengthBytes;
} MR_MusicTrack;

static MR_MusicTrack s_tracks[MUSIC_MAX_TRACKS];
static int           s_trackCount;          /* AUDIO tracks only */
static int           s_ready;

static FILE       *s_file;
static XA2_Voice  *s_voice;
static int         s_curTrack;              /* 0 when stopped */
static int         s_curIndex;              /* into s_tracks  */
static int         s_loop;
static long        s_pos;                   /* bytes read from start */
static int         s_drained;               /* no more to submit */

/*
 * XAUDIO2 READS THE BLOCK IN PLACE, so a buffer handed to the voice
 * must not be touched until the voice is done with it. The ring is
 * only ever written at s_next, and s_next is only advanced after a
 * submit - so with the queue-depth test below, the slot about to be
 * written is always one the voice has finished.
 */
static unsigned char s_buf[MUSIC_BUFFERS][MUSIC_BUF_BYTES];
static int           s_next;

static int           s_pausedVoice;


/* ================================================================== */
/* 1. THE CUE SHEET                                                    */
/* ================================================================== */

/*
 * ONE FILE PER TRACK, which is the layout this supports and the one
 * the reference rip uses:
 *
 *     FILE "Moto Racer (USA) (Track 02).bin" BINARY
 *       TRACK 02 AUDIO
 *         INDEX 00 00:00:00
 *         INDEX 01 00:01:00
 *
 * THE DATA TRACK IS NOT MUSIC. Track 01 is MODE1/2352 and holds the
 * game, so only TRACK lines saying AUDIO are kept. That is also why
 * music_track_count answers 12 for a thirteen track disc.
 *
 * INDEX 01 IS THE START, NOT INDEX 00. The two differ only where a
 * track has a PREGAP - on this rip only track 02 does, one second of
 * it - and starting at INDEX 00 there would play that second of
 * silence before the music. It is the kind of mistake that looks
 * like a stutter on one track and nothing on the rest.
 *
 * A SINGLE-FILE CUE IS NOT HANDLED. Many rips put every track in one
 * .bin with the TRACK lines carrying offsets into it; that needs
 * each track's length to come from the NEXT track's start rather
 * than from the file size. Deliberately out of scope - the parser
 * below would report such a cue as one enormous track apiece.
 */

/* mm:ss:ff -> bytes. ff is a 1/75 second CD frame, not a video one. */
static long cue_msf_to_bytes(int mm, int ss, int ff)
{
	return ((long)mm * 60L * CD_SECTORS_PER_S +
	        (long)ss * CD_SECTORS_PER_S +
	        (long)ff) * CD_SECTOR_BYTES;
}

/* The directory the cue itself sits in, with its trailing slash. */
static void cue_dir_of(const char *cuePath, char *dir, int size)
{
	int i;
	int cut = -1;

	for (i = 0; cuePath[i] && i < size - 1; i++) {
		dir[i] = cuePath[i];
		if (cuePath[i] == '\\' || cuePath[i] == '/')
			cut = i;
	}
	dir[cut + 1] = 0;
}

static long file_size_of(const char *path)
{
	FILE *f = fopen(path, "rb");
	long  n;

	if (!f)
		return -1;
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return -1;
	}
	n = ftell(f);
	fclose(f);
	return n;
}

/* The first quoted run in a line, or 0 when there is none. */
static int cue_quoted(const char *line, char *out, int size)
{
	const char *a = strchr(line, '"');
	const char *b;
	int         n;

	if (!a)
		return 0;
	b = strchr(a + 1, '"');
	if (!b)
		return 0;
	n = (int)(b - a - 1);
	if (n <= 0 || n >= size)
		return 0;
	memcpy(out, a + 1, (size_t)n);
	out[n] = 0;
	return 1;
}

/* Skip leading blanks; the cue's indentation is decorative. */
static const char *cue_trim(const char *s)
{
	while (*s == ' ' || *s == '\t')
		s++;
	return s;
}

static int cue_parse(const char *cuePath)
{
	FILE *f = fopen(cuePath, "r");
	char  line[512];
	char  dir[MAX_PATH];
	char  name[MAX_PATH];
	char  pending[MAX_PATH];
	int   haveFile = 0;
	int   open = -1;            /* the track being built, or -1 */

	if (!f) {
		mrlog_err("music: cannot open %s\n", cuePath);
		return 0;
	}
	cue_dir_of(cuePath, dir, sizeof dir);
	pending[0] = 0;
	s_trackCount = 0;

	while (fgets(line, sizeof line, f)) {
		const char *p = cue_trim(line);

		if (_strnicmp(p, "FILE", 4) == 0) {
			/*
			 * The name is relative to the CUE, not to the
			 * working directory - a rip opened from anywhere
			 * else would otherwise find nothing.
			 */
			if (cue_quoted(p, name, sizeof name)) {
				_snprintf(pending, sizeof pending, "%s%s",
				          dir, name);
				pending[sizeof pending - 1] = 0;
				haveFile = 1;
			}
			open = -1;
		} else if (_strnicmp(p, "TRACK", 5) == 0) {
			int num = 0;

			open = -1;
			if (!haveFile)
				continue;
			if (sscanf(p, "%*s %d", &num) != 1)
				continue;
			/* AUDIO only; MODE1/2352 is the game's data. */
			if (!strstr(p, "AUDIO"))
				continue;
			if (s_trackCount >= MUSIC_MAX_TRACKS) {
				mrlog_err("music: more than %d tracks\n",
				          MUSIC_MAX_TRACKS);
				break;
			}
			open = s_trackCount++;
			memset(&s_tracks[open], 0, sizeof s_tracks[open]);
			s_tracks[open].number = num;
			_snprintf(s_tracks[open].path,
			          sizeof s_tracks[open].path, "%s", pending);
			s_tracks[open].path[MAX_PATH - 1] = 0;
		} else if (_strnicmp(p, "INDEX", 5) == 0 && open >= 0) {
			int idx = 0, mm = 0, ss = 0, ff = 0;

			if (sscanf(p, "%*s %d %d:%d:%d",
			           &idx, &mm, &ss, &ff) == 4 && idx == 1)
			    s_tracks[open].startByte =
			        cue_msf_to_bytes(mm, ss, ff);
		}
	}
	fclose(f);

	/* One file per track, so the rest of the file IS the track. */
	{
		int i;
		int kept = 0;

		for (i = 0; i < s_trackCount; i++) {
			long sz = file_size_of(s_tracks[i].path);

			if (sz < 0) {
				mrlog_err("music: track %d missing - %s\n",
				          s_tracks[i].number, s_tracks[i].path);
				continue;
			}
			if (s_tracks[i].startByte >= sz) {
				mrlog_err("music: track %d pregap past the "
				          "end\n", s_tracks[i].number);
				continue;
			}
			s_tracks[i].lengthBytes = sz - s_tracks[i].startByte;
			if (kept != i)
				s_tracks[kept] = s_tracks[i];
			kept++;
		}
		s_trackCount = kept;
	}
	return s_trackCount > 0;
}


/* ================================================================== */
/* 2. THE PLAYER                                                       */
/* ================================================================== */

/*
 * AudioState is the on/off switch and CDVolume the level; the
 * music plays at their product.  Both are live values, not
 * captured at start.
 */
static float music_gain(void)
{
	return g_options.music ? audio_music_gain() : 0.0f;
}


static int music_find(int track)
{
	int i;

	for (i = 0; i < s_trackCount; i++)
		if (s_tracks[i].number == track)
			return i;
	return -1;
}

int music_init(void)
{
	char path[MAX_PATH];

	music_free();
	path[0] = 0;
	if (!get_ini_string("PATHS", "CueSheet", path, sizeof path) ||
	    !path[0]) {
		mrlog("music: no [PATHS] CueSheet - music off\n");
		return 0;
	}
	if (!cue_parse(path)) {
		mrlog_err("music: %s gave no playable tracks - music off\n",
		          path);
		s_trackCount = 0;
		return 0;
	}

	s_ready = 1;
	mrlog("music: %s - %d audio track%s\n", path, s_trackCount,
	      s_trackCount == 1 ? "" : "s");
	{
		int i;

		for (i = 0; i < s_trackCount; i++)
			mrlog("music:   track %2d  %7ld KB  +%ld\n",
		          s_tracks[i].number,
		          s_tracks[i].lengthBytes / 1024,
		          s_tracks[i].startByte);
	}
	return 1;
}

void music_free(void)
{
	music_stop();
	if (s_voice) {
		xa2_voice_destroy(s_voice);
		s_voice = NULL;
	}
	s_trackCount = 0;
	s_ready      = 0;
}

int music_track_count(void)
{
	return s_trackCount;
}

int music_track_at(int n)
{
	if (n < 0 || n >= s_trackCount)
		return 0;
	return s_tracks[n].number;
}

int music_current_track(void)
{
	return s_curTrack;
}

int music_playing(void)
{
	return s_curTrack != 0;
}

void music_stop(void)
{
	if (s_voice)
		xa2_voice_stop(s_voice);
	if (s_file) {
		fclose(s_file);
		s_file = NULL;
	}
	s_curTrack    = 0;
	s_curIndex    = -1;
	s_pausedVoice = 0;
	s_pos      = 0;
	s_next     = 0;
	s_drained  = 0;
}

/*
 * Fill one ring slot and hand it over. Returns 0 when there is
 * nothing left to send, which is the end of a track that is not
 * looping.
 */
static int music_submit_one(void)
{
	unsigned char *dst = s_buf[s_next];
	size_t         want;
	size_t         got;
	long           left;

	if (!s_file || s_curIndex < 0)
		return 0;

	left = s_tracks[s_curIndex].lengthBytes - s_pos;
	if (left <= 0) {
		if (!s_loop)
			return 0;
		/*
		 * BACK TO INDEX 01, not to byte zero - the pregap is
		 * not part of the music and a loop through it would
		 * add a second of silence every time round.
		 */
		s_pos = 0;
		fseek(s_file, s_tracks[s_curIndex].startByte, SEEK_SET);
		left = s_tracks[s_curIndex].lengthBytes;
	}

	want = (size_t)MUSIC_BUF_BYTES;
	if ((long)want > left)
		want = (size_t)left;

	got = fread(dst, 1, want, s_file);
	if (got == 0)
		return 0;
	s_pos += (long)got;

	/*
	 * A SHORT READ IS STILL A BUFFER. It happens on the last block
	 * of a track and on a truncated rip; submitting what arrived
	 * keeps the music going to the real end instead of clipping a
	 * whole buffer's worth off it.
	 */
	if (!xa2_voice_submit(s_voice, dst, (unsigned int)got))
		return 0;
	s_next = (s_next + 1) % MUSIC_BUFFERS;
	return 1;
}

int music_play(int track, int loop)
{
	int idx;

	if (!s_ready)
		return 0;

	idx = music_find(track);
	if (idx < 0) {
		mrlog_err("music: no track %d in the cue\n", track);
		return 0;
	}

	music_stop();

	s_file = fopen(s_tracks[idx].path, "rb");
	if (!s_file) {
		mrlog_err("music: cannot open %s\n", s_tracks[idx].path);
		return 0;
	}
	if (fseek(s_file, s_tracks[idx].startByte, SEEK_SET) != 0) {
		fclose(s_file);
		s_file = NULL;
		return 0;
	}

	if (!s_voice) {
		s_voice = xa2_voice_create(MUSIC_CHANNELS, MUSIC_RATE,
		                           MUSIC_BITS);
		if (!s_voice) {
			mrlog_err("music: no voice - music off\n");
			fclose(s_file);
			s_file = NULL;
			return 0;
		}
	}

	s_curTrack = track;
	s_curIndex = idx;
	s_loop     = loop;
	s_pos      = 0;
	s_next     = 0;
	s_drained  = 0;

	/*
	 * PRIME THE WHOLE RING BEFORE STARTING. Submitting one buffer
	 * and letting the pump catch up leaves the first two frames a
	 * buffer short, which is audible as a tick at the head of
	 * every track.
	 */
	{
		int i;

		for (i = 0; i < MUSIC_BUFFERS; i++) {
			if (!music_submit_one()) {
				s_drained = 1;
				break;
			}
		}
	}

	xa2_voice_volume(s_voice, music_gain());
	if (!xa2_voice_start(s_voice)) {
		music_stop();
		return 0;
	}

	mrlog("music: track %d%s, %ld KB\n", track, loop ? " loop" : "",
	      s_tracks[idx].lengthBytes / 1024);
	return 1;
}

void music_tick(void)
{
	if (!s_curTrack || !s_voice)
		return;

	/*
	 * THE PAUSE KEY SILENCES THE MUSIC.  [deliberate divergence]
	 *
	 * The original cannot do this and does not try: its music is
	 * a spinning disc, and pausing the game leaves the drive
	 * playing.  Ours is a file, so it can be held - and being
	 * able to shut the music up without leaving the race is
	 * worth more than matching a limitation of 1997 hardware.
	 *
	 * IT IS A PAUSE AND NOT A STOP, so releasing it picks the
	 * track up where it left off rather than restarting it.
	 * That is also what a real CD would have done, had it been
	 * able to.
	 *
	 * THE ESCAPE MENU IS NOT THIS.  Q opens the in-race menu
	 * through hud.c's own s_open flag and never touches the
	 * clock, so the music keeps playing over it - which IS what
	 * the original does.  Only the space bar, which stops the
	 * clock, reaches here.
	 *
	 * tm_step_frames pauses too, so a scripted mr_ab run holds
	 * the music for its duration.  Harmless, and it keeps the
	 * two in step rather than letting a stepped run drift.
	 */
	{
		int want = tm_paused() ? 1 : 0;

		if (want != s_pausedVoice) {
			s_pausedVoice = want;
			if (want)
				xa2_voice_pause(s_voice);
			else
				xa2_voice_resume(s_voice);
		}
		if (s_pausedVoice)
			return;
	}

	/*
	 * THE OPTION IS A GAIN, NOT A STOP. AudioState off silences
	 * the music and leaves it running, so turning it back on
	 * picks up where the disc would have been rather than
	 * restarting the track - which is what a real CD does.
	 *
	 * AND IT IS RE-READ EVERY FRAME, so the options page's
	 * CD slider is heard as it moves without any notification
	 * path back to here.
	 */
	xa2_voice_volume(s_voice, music_gain());

	while (!s_drained && xa2_voice_queued(s_voice) < MUSIC_BUFFERS) {
		if (!music_submit_one()) {
			s_drained = 1;
			break;
		}
	}

	/*
	 * AND THE END IS WHEN THE VOICE RUNS DRY, not when the file
	 * does. A track that has been fully read still has up to six
	 * seconds queued ahead of it.
	 */
	if (s_drained && xa2_voice_queued(s_voice) == 0)
		music_stop();
}
