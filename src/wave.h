/*
 * wave.h - the .WAV reader for SFX.BKF.
 *
 * The 250 sound files are named .WAV and begin "rmnr" rather than
 * "RIFF". They are ORDINARY uncompressed PCM RIFF WAVE files with a
 * positional cipher over them - no compression, no container. See
 * docs/formats/rmnr.txt, which owns the format; tools/rmnr.py is the
 * oracle this reader is checked against.
 */

#ifndef MRACER_WAVE_H
#define MRACER_WAVE_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * What the original keeps in a WAVEFORMATEX, minus the fields it
 * recomputes rather than reads.
 *
 * ONLY THREE FIELDS COME FROM THE FILE - channels, sampleRate and
 * bits. Wave_Read forces wFormatTag to 1 and derives nBlockAlign and
 * nAvgBytesPerSec, so the file's copies of those are ignored. A port
 * that trusted them would be trusting bytes the game never reads.
 */
typedef struct {
	int            channels;
	int            sampleRate;
	int            bits;         /* 8 or 16 - both trees ship */

	unsigned char *pcm;          /* malloc, wave_free releases  */
	unsigned int   pcmBytes;
} MR_Wave;

/*
 * wave_load - read one sample by LOGICAL name, through asset_load.
 *
 * ORIGIN  Wave_Read @ 0x00440d30
 *
 * The name is archive-relative, as rule 10 requires - so
 * "SFX\16_BITS\MENU\SELECT.WAV", never a path. Returns 0 on failure
 * with *out zeroed.
 */
int  wave_load(const char *logical, MR_Wave *out);
void wave_free(MR_Wave *w);

/*
 * wave_unmask - the cipher, in place.
 *
 * ORIGIN  Wave_Unmask @ 0x00440d00
 *
 *     plain[i] = (cipher[i] - 0x20 - 4*i) & 0xff
 *
 * EXPOSED ON PURPOSE. It is the one piece worth testing on its own:
 * stateless, positional, and the counter RESTARTS at zero on every
 * call. wave_load calls it three times per file on three separate
 * buffers - see the note there.
 */
void wave_unmask(unsigned char *buf, unsigned int len);

/*
 * wave_log - one line per field, for the known-answer test.
 *
 * tools/rmnr.py --table prints the same numbers straight out of
 * SFX.BKF, so a Debug run's log diffs against the oracle without
 * anything having to be run twice. See docs/formats/rmnr.txt.
 */
void wave_log(const MR_Wave *w, const char *what);

/*
 * wave_selftest - decode the menu bank and log every header.
 *
 * THE KNOWN-ANSWER TEST. It prints exactly what
 *
 *     python tools/rmnr.py --table decomp/DATA/SFX.BKF
 *                          --match "16_BITS\\MENU"
 *
 * prints from the oracle, so checking the decoder is a literal
 * diff of two identical-format lists rather than a reading of two
 * different tables.
 *
 * A DEBUG OVERRIDE: off unless [DEBUG] wavecheck=1, and it only
 * loads and frees - nothing else in the game sees it.
 */
void wave_selftest(void);

#ifdef __cplusplus
}
#endif

#endif /* MRACER_WAVE_H */
