/*
 * wave.c - the .WAV reader for SFX.BKF. See wave.h and
 * docs/formats/rmnr.txt.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wave.h"
#include "assets.h"
#include "log.h"

/* The four tags, little-endian as they sit in the file. */
#define WAVE_TAG_RIFF  0x46464952u      /* "RIFF" */
#define WAVE_TAG_WAVE  0x45564157u      /* "WAVE" */
#define WAVE_TAG_FMT   0x20746d66u      /* "fmt " */
#define WAVE_TAG_DATA  0x61746164u      /* "data" */

/*
 * The three region lengths Wave_Read unmasks, and the fmt size it
 * subtracts from. 0x24 is RIFF through wBitsPerSample; 8 is the data
 * chunk header; 16 is the standard fmt chunk, so the seek skips
 * whatever a longer one adds.
 */
#define WAVE_HDR_BYTES   0x24
#define WAVE_CHUNK_BYTES 8
#define WAVE_FMT_STD     16

/* Field offsets inside the 0x24-byte header. */
#define WAVE_OFF_RIFF      0
#define WAVE_OFF_WAVE      8
#define WAVE_OFF_FMT      12
#define WAVE_OFF_FMTSIZE  16
#define WAVE_OFF_CHANNELS 22
#define WAVE_OFF_RATE     24
#define WAVE_OFF_BITS     34

static unsigned int rd32(const unsigned char *p)
{
	return (unsigned int)p[0] | ((unsigned int)p[1] << 8)
	     | ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

static unsigned short rd16(const unsigned char *p)
{
	return (unsigned short)((unsigned int)p[0] | ((unsigned int)p[1] << 8));
}

/*
 * ORIGIN  Wave_Unmask @ 0x00440d00, whole
 *
 * The original writes
 *
 *     buf[i] = buf[i] + (-8 - (char)i) * 4;
 *
 * in char arithmetic, which is the same as subtracting 0x20 + 4*i.
 * Written as the subtraction because that is the form the format note
 * states and the form a reader checks against.
 *
 * 4*i WRAPS AT 256, so the key repeats every 64 bytes. That is worth
 * knowing when eyeballing a hex dump: a 64-byte period is the cipher,
 * not structure in the data.
 */
void wave_unmask(unsigned char *buf, unsigned int len)
{
	unsigned int i;

	if (!buf)
		return;
	for (i = 0; i < len; i++)
		buf[i] = (unsigned char)(buf[i] - 0x20u - 4u * i);
}

/*
 * ORIGIN  Wave_Read @ 0x00440d30, whole
 *
 * THE CIPHER IS TRIGGERED BY LETTER CASE, not by a magic. Masking
 * adds 0x20 to byte 0, so the 'R' of "RIFF" arrives as lowercase 'r'
 * - and "IFF" becomes "mnr" by the same arithmetic. The original
 * tests nothing but that one character, which is why it reads plain
 * RIFF files unchanged and why there is no "rmnr" constant anywhere
 * in the binary.
 *
 * THREE REGIONS, EACH RESTARTING THE COUNTER. This is the detail a
 * decoder gets wrong: the file is NOT one masked stream. The header,
 * the data chunk header and the payload are unmasked separately with
 * i back at zero each time, and the bytes BETWEEN the first two - the
 * cbSize of an 18-byte fmt chunk - are seeked over and never
 * unmasked at all. Treating the file as one stream gives garbage
 * from byte 0x24 onward.
 *
 * o THE TAGS ARE CHECKED ON PLAINTEXT, after unmasking, and all four
 *   of them. It is a real format check, just a late one.
 *
 * o ONLY THREE FIELDS ARE READ - see wave.h.
 *
 * DIVERGENCE: the original streams through File_Open and seeks; ours
 * takes the whole member from asset_load and walks it in memory,
 * because that is what rule 10's archive reader hands back. The
 * arithmetic and the region boundaries are identical.
 */
int wave_load(const char *logical, MR_Wave *out)
{
	unsigned char *file = NULL;
	unsigned char  hdr[WAVE_HDR_BYTES];
	unsigned char  chunk[WAVE_CHUNK_BYTES];
	unsigned int   size = 0;
	unsigned int   fmtSize;
	unsigned int   dataAt;
	unsigned int   dataBytes;
	int            masked;

	if (!out)
		return 0;
	memset(out, 0, sizeof(*out));
	if (!logical)
		return 0;

	file = (unsigned char *)asset_load(logical, &size);
	if (!file)
		return 0;

	if (size < WAVE_HDR_BYTES + WAVE_CHUNK_BYTES) {
		mrlog_err("wave:   %s is %u bytes - too short for a header\n",
		          logical, size);
		free(file);
		return 0;
	}

	memcpy(hdr, file, WAVE_HDR_BYTES);

	/* The whole test, and it is one character. */
	masked = (hdr[0] == 'r');
	if (masked)
		wave_unmask(hdr, WAVE_HDR_BYTES);

	fmtSize = rd32(hdr + WAVE_OFF_FMTSIZE);
	if (fmtSize < WAVE_FMT_STD) {
		mrlog_err("wave:   %s has a %u-byte fmt chunk\n",
		          logical, fmtSize);
		free(file);
		return 0;
	}

	/*
	 * The seek the original makes between the two headers. Every
	 * shipped file has an 18-byte fmt chunk, so this steps over the
	 * two cbSize bytes - which are therefore never unmasked.
	 */
	dataAt = WAVE_HDR_BYTES + (fmtSize - WAVE_FMT_STD);
	if (dataAt + WAVE_CHUNK_BYTES > size) {
		mrlog_err("wave:   %s ends before its data chunk\n", logical);
		free(file);
		return 0;
	}

	memcpy(chunk, file + dataAt, WAVE_CHUNK_BYTES);
	if (masked)
		wave_unmask(chunk, WAVE_CHUNK_BYTES);

	if (rd32(hdr + WAVE_OFF_RIFF) != WAVE_TAG_RIFF ||
	    rd32(hdr + WAVE_OFF_WAVE) != WAVE_TAG_WAVE ||
	    rd32(hdr + WAVE_OFF_FMT)  != WAVE_TAG_FMT  ||
	    rd32(chunk)               != WAVE_TAG_DATA) {
		mrlog_err("wave:   %s is not a WAVE file%s\n", logical,
		          masked ? " even unmasked" : "");
		free(file);
		return 0;
	}

	dataBytes = rd32(chunk + 4);
	if (dataAt + WAVE_CHUNK_BYTES + dataBytes > size) {
		/*
		 * TRUNCATE RATHER THAN REFUSE. The original asks for
		 * exactly this many bytes and fails if it cannot read
		 * them; ours already holds the member, so a short file is
		 * a damaged archive and saying so beats a silent zero.
		 */
		mrlog_err("wave:   %s claims %u data bytes, %u available\n",
		          logical, dataBytes,
		          size - dataAt - WAVE_CHUNK_BYTES);
		free(file);
		return 0;
	}

	out->pcm = (unsigned char *)malloc(dataBytes ? dataBytes : 1);
	if (!out->pcm) {
		free(file);
		return 0;
	}
	memcpy(out->pcm, file + dataAt + WAVE_CHUNK_BYTES, dataBytes);
	free(file);

	if (masked)
		wave_unmask(out->pcm, dataBytes);

	out->pcmBytes   = dataBytes;
	out->channels   = (int)rd16(hdr + WAVE_OFF_CHANNELS);
	out->sampleRate = (int)rd32(hdr + WAVE_OFF_RATE);
	out->bits       = (int)rd16(hdr + WAVE_OFF_BITS);
	return 1;
}

void wave_free(MR_Wave *w)
{
	if (!w)
		return;
	free(w->pcm);
	memset(w, 0, sizeof(*w));
}

/* ORIGIN  NONE - ours, the known-answer test's half. */
void wave_log(const MR_Wave *w, const char *what)
{
	if (!w)
		return;
	mrlog("wave:   %-28s %5d Hz %2d bit %dch %7u bytes\n",
	      what ? what : "(sample)", w->sampleRate, w->bits,
	      w->channels, w->pcmBytes);
}

/*
 * ORIGIN  NONE - ours.
 *
 * THE LIST MIRRORS tools/rmnr.py's --match FILTER, not a table in
 * the game: the point is to decode the same members the oracle
 * does, in the same order, so the two outputs diff. The real menu
 * bank is chosen by the pair tables in audio-cues.txt 5.2 and is a
 * different question.
 *
 * 16-BIT ONLY. SFX.BKF ships a parallel 8_BITS tree and nothing
 * has established whether the two differ in more than sample
 * width - rmnr.txt's open questions. Checking one tree proves the
 * decoder; checking both would be checking the data.
 */
void wave_selftest(void)
{
	static const char *const kMenu[] = {
		"SFX\\16_BITS\\MENU\\CONTINUE.WAV",
		"SFX\\16_BITS\\MENU\\DEPLETT.WAV",
		"SFX\\16_BITS\\MENU\\GOKTM.WAV",
		"SFX\\16_BITS\\MENU\\GONINJA.WAV",
		"SFX\\16_BITS\\MENU\\PODIUM1.WAV",
		"SFX\\16_BITS\\MENU\\PODIUM2.WAV",
		"SFX\\16_BITS\\MENU\\SELECT.WAV",
		"SFX\\16_BITS\\MENU\\TITLE.WAV",
		"SFX\\16_BITS\\MENU\\VALIDE.WAV"
	};
	int i;
	int ok = 0;

	mrlog("wave:   self-check, %d menu samples\n",
	      (int)(sizeof kMenu / sizeof kMenu[0]));
	for (i = 0; i < (int)(sizeof kMenu / sizeof kMenu[0]); i++) {
		MR_Wave w;

		if (!wave_load(kMenu[i], &w)) {
			mrlog_err("wave:   %-28s LOAD FAILED\n",
			          kMenu[i]);
			continue;
		}
		/* The name without the archive stem, so the line matches
		 * the oracle's member name exactly. */
		wave_log(&w, kMenu[i] + 4);
		wave_free(&w);
		ok++;
	}
	mrlog("wave:   self-check %d/%d decoded\n", ok,
	      (int)(sizeof kMenu / sizeof kMenu[0]));
}
