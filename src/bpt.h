/*
 * bpt.h - the .bpt binary script reader.
 *
 * A .bpt is the compiled form of a text .spt script: an obfuscated,
 * POSITIONAL dump of the values the text script would have produced.
 * The binary form stores no names at all, so a reader knows what each
 * value means only because it asks for them in the order the game
 * wrote them. Getting one read out of order desynchronises everything
 * after it.
 *
 * The format is fully decoded in docs/formats/bpt.txt and
 * tools/bpt.py is the reference implementation. Three rules matter:
 *
 *   THE MASK is a byte-wise XOR with the low byte of successive
 *   outputs from a Park-Miller generator, stream 6, seeded with
 *   0x1a2b3c4d when the file is opened (Crypt_Xor 0x0045a4a0,
 *   Rand_Next 0x004b1160).
 *
 *   A STRING is a PLAINTEXT little-endian u32 length followed by that
 *   many masked bytes. The generator does NOT advance over the
 *   length. That single detail broke every earlier attempt at the
 *   format: advancing four steps across it puts the stream four out
 *   of step for the whole rest of the file.
 *
 *   THE STREAM STARTS AT BYTE 0 for the files we read. It is not
 *   universal - see bpt.txt section 3.1 - so a new file class wants
 *   tools/bpt.py --scan before it is trusted.
 *
 * The original's reader is Script_Open (0x004242a0), Script_ReadValue
 * (0x00425080), Script_ReadFloat (0x00424c00) and Script_ReadString
 * (0x00424e60).
 */

#ifndef MRACER_BPT_H
#define MRACER_BPT_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	unsigned char *data;
	unsigned int   size;
	unsigned int   pos;
	unsigned int   rng;     /* the generator state, full 32 bits */
} MR_Script;

/*
 * bpt_open - load a script through asset_load and seed the stream.
 *
 * logicalName is an asset name in the assets.c sense, never a path -
 * "LIST.BPT" for a loose file in DATA, "SPEED\CARAC2.BPT" for an
 * archive member. Returns 0 if the file is missing.
 *
 * THE EXTENSION IS NOT REWRITTEN HERE. Script_Open uppercases its
 * argument and swaps whatever extension it has for BPT, so the game
 * can ask for Data\Config.Spt and get DATA\CONFIG.BPT. Callers here
 * name the file they actually want.
 */
int  bpt_open(MR_Script *s, const char *logicalName);
void bpt_close(MR_Script *s);

/*
 * Each returns 1 on success and 0 at end of file. A failed read does
 * NOT advance the stream, so a caller that runs off the end stops
 * cleanly rather than returning noise.
 */
int  bpt_read_int(MR_Script *s, int *out);
int  bpt_read_float(MR_Script *s, float *out);

/*
 * Reads at most size-1 bytes and always NUL-terminates. A string
 * longer than the buffer is TRUNCATED but the stream still advances
 * over all of it, so the reader stays in step.
 */
int  bpt_read_string(MR_Script *s, char *out, int size);

#ifdef __cplusplus
}
#endif

#endif /* MRACER_BPT_H */
