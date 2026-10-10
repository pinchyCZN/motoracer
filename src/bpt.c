/*
 * bpt.c - the .bpt binary script reader. See bpt.h.
 */

#include <stdlib.h>
#include <string.h>

#include "bpt.h"
#include "rand.h"
#include "assets.h"
#include "log.h"

#define BPT_SEED 0x1a2b3c4dU
#define BPT_MULT 16807U

/*
 * Rand_Next (0x004b1160), the Park-Miller "minimal standard" Lehmer
 * generator with Schrage's method so the multiply cannot overflow 32
 * bits.
 *
 * THE STATE AND THE RETURN VALUE DIFFER. The state keeps the full
 * 31-bit value; the value handed back is reduced modulo 0x7fff when it
 * exceeds it, and only its LOW BYTE is used as the mask. Feeding the
 * reduced value back as the state instead would decode the first byte
 * correctly and nothing after it.
 */
/*
 * ORIGIN  Rand_Next @ 0x004b1160
 *
 * THE SAME GENERATOR THE GAME USES, over this file's own state
 * rather than one of the eight streams. It lives in rand.c so
 * there is one copy of the arithmetic; the note about the state
 * and the return value differing is there too, and it is the
 * detail that decodes the first byte and nothing after it.
 */
static unsigned int rng_next(unsigned int *state)
{
	return (unsigned int)rand_step(state);
}

int bpt_open(MR_Script *s, const char *logicalName)
{
	if (!s)
		return 0;
	memset(s, 0, sizeof *s);

	s->data = (unsigned char *)asset_load(logicalName, &s->size);
	if (!s->data) {
		mrlog_err("bpt: cannot open %s\n", logicalName);
		return 0;
	}
	s->pos = 0;
	s->rng = BPT_SEED;
	return 1;
}

void bpt_close(MR_Script *s)
{
	if (!s)
		return;
	if (s->data)
		free(s->data);
	memset(s, 0, sizeof *s);
}

/* Four masked bytes, little-endian. */
static int bpt_read_raw32(MR_Script *s, unsigned int *out)
{
	unsigned int v = 0;
	int          i;

	if (!s || !s->data || s->pos + 4 > s->size)
		return 0;
	for (i = 0; i < 4; i++) {
		unsigned int b = s->data[s->pos + i] ^ (rng_next(&s->rng) & 0xff);
		v |= b << (i * 8);
	}
	s->pos += 4;
	if (out)
		*out = v;
	return 1;
}

int bpt_read_int(MR_Script *s, int *out)
{
	unsigned int v;

	if (!bpt_read_raw32(s, &v))
		return 0;
	if (out)
		*out = (int)v;
	return 1;
}

int bpt_read_float(MR_Script *s, float *out)
{
	unsigned int v;
	float        f;

	if (!bpt_read_raw32(s, &v))
		return 0;
	memcpy(&f, &v, sizeof f);
	if (out)
		*out = f;
	return 1;
}

int bpt_read_string(MR_Script *s, char *out, int size)
{
	unsigned int n;
	unsigned int i;

	if (out && size > 0)
		out[0] = 0;
	if (!s || !s->data || size <= 0 || s->pos + 4 > s->size)
		return 0;

	/*
	 * THE LENGTH IS PLAINTEXT and the generator does not advance over
	 * it - Script_ReadString freads it, freads the bytes, and only
	 * then calls Crypt_Xor on the bytes [verified, 0x00424e60].
	 */
	n = (unsigned int)s->data[s->pos]
	  | ((unsigned int)s->data[s->pos + 1] << 8)
	  | ((unsigned int)s->data[s->pos + 2] << 16)
	  | ((unsigned int)s->data[s->pos + 3] << 24);

	if (s->pos + 4 + n > s->size)
		return 0;
	s->pos += 4;

	for (i = 0; i < n; i++) {
		char c = (char)(s->data[s->pos + i] ^ (rng_next(&s->rng) & 0xff));

		/* Truncate into the caller's buffer, but keep consuming so
		 * the generator stays in step with the file. */
		if ((int)i < size - 1)
			out[i] = c;
	}
	out[(int)n < size - 1 ? (int)n : size - 1] = 0;
	s->pos += n;
	return 1;
}
