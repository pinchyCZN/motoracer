/*
 * objsnd.c - the per-track object sounds. See objsnd.h.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "objsnd.h"
#include "audio.h"
#include "trig.h"
#include "bpt.h"
#include "rand.h"
#include "timing.h"
#include "options.h"
#include "log.h"
#include "model.h"   /* mat34_from_euler_decor */

/*
 * THE DRAW IS 128 WORLD UNITS TO THE METRE, and the radii in the
 * script are metres. 0x004d0140 holds the 128 and 0x004d0148 the
 * 255 the falloff spans [verified, 0x00404fb0].
 */
#define OBJSND_UNITS_PER_M  128.0f
#define OBJSND_VOL_SPAN     255.0f

/* Rand stream 4, the mixer's - see rand.h on why that matters. */
#define OBJSND_RAND_STREAM  4

/* The states of 0x00405030, in the order it numbers them. */
#define ST_WAIT     0
#define ST_START    1
#define ST_FADEIN   2
#define ST_STOP     3
#define ST_FADEOUT  4
#define ST_SUSTAIN  5

static MR_SoundDesc s_desc[OBJSND_MAX_DESC];
static int          s_descCount;

/*
 * THE INTERN TABLE. A name appears once however many descriptors
 * mention it, and its position here IS its cue id less the base
 * [0x004056c0].
 */
static char         s_name[OBJSND_MAX_NAMES][16];
static int          s_nameCount;

static MR_ObjSound  s_bound[OBJSND_MAX_BOUND];
static int          s_boundCount;


/* ORIGIN  ObjSnd_InternName @ 0x004056c0 - look up, or append and assign. */
static int objsnd_intern(const char *name)
{
	int i;

	for (i = 0; i < s_nameCount; i++) {
		if (strcmp(s_name[i], name) == 0)
			return OBJSND_ID_BASE + i;
	}
	if (s_nameCount >= OBJSND_MAX_NAMES) {
		mrlog_err("objsnd: more than %d names - '%s' dropped\n",
		          OBJSND_MAX_NAMES, name);
		return -1;
	}
	_snprintf(s_name[s_nameCount], sizeof s_name[0], "%s", name);
	s_name[s_nameCount][sizeof s_name[0] - 1] = 0;
	return OBJSND_ID_BASE + s_nameCount++;
}


/*
 * ORIGIN  the __strnicmp chain at 0x0042eb00.
 *
 * THE SEPARATOR IS '|' AND THE TEST IS A PREFIX TEST, not a whole
 * token compare - the original walks the string looking for a bar
 * and tries each name against what follows it. An unknown flag is a
 * message box there and a log line here; it is never fatal.
 */
static unsigned int objsnd_flags(const char *s)
{
	static const struct { const char *word; unsigned int bit; } kFlag[] = {
		{ "LOOPED",   OBJSND_LOOPED   },
		{ "PAN",      OBJSND_PAN      },
		{ "RANDPAN",  OBJSND_RANDPAN  },
		{ "VOL",      OBJSND_VOL      },
		{ "FADEIN",   OBJSND_FADEIN   },
		{ "FADEOUT",  OBJSND_FADEOUT  },
		{ "FADEFREQ", OBJSND_FADEFREQ },
		{ "INFPLAY",  OBJSND_INFPLAY  },
		{ "SINVOL",   OBJSND_SINVOL   },
		{ "SINFREQ",  OBJSND_SINFREQ  }
	};
	unsigned int out = 0;
	const char  *p = s;

	while (p && *p) {
		int i;
		int hit = 0;

		/*
		 * RANDPAN BEFORE PAN, FADEFREQ BEFORE FADEIN. The table
		 * is ordered so a prefix test cannot match the shorter
		 * word first - the original gets this by testing PAN
		 * with a length of 3 and RANDPAN with 7, which comes to
		 * the same thing.
		 */
		for (i = (int)(sizeof kFlag / sizeof kFlag[0]) - 1;
		     i >= 0; i--) {
			size_t n = strlen(kFlag[i].word);

			if (_strnicmp(p, kFlag[i].word, (int)n) == 0) {
				out |= kFlag[i].bit;
				hit = 1;
				break;
			}
		}
		if (!hit)
			mrlog_err("objsnd: unknown flag near '%s'\n", p);

		p = strchr(p, '|');
		if (p)
			p++;
	}
	return out;
}


void objsnd_reset(void)
{
	int i;

	for (i = 0; i < s_boundCount; i++)
		audio_voice_stop(s_bound[i].slot.voice);

	memset(s_desc, 0, sizeof s_desc);
	memset(s_name, 0, sizeof s_name);
	memset(s_bound, 0, sizeof s_bound);
	s_descCount  = 0;
	s_nameCount  = 0;
	s_boundCount = 0;
}


/*
 * ORIGIN  ObjSnd_ParseScript @ 0x0042e850
 *
 * THE GRAMMAR, and every value is positional:
 *
 *     soundDesc <id> <n> <name>*n <flags> <13 ints> <2 floats>
 *     ambiance  <n> <descId>*n
 *     signal    <id> <n> <value>*n
 *
 * ambiance and signal are PARSED AND NOT ACTED ON. Both register
 * entries the original plays without a decor object behind them -
 * the AT_GLOBAL and AT_SEGMENT arms of the placement - and neither
 * is reconstructed yet. They are read so the stream stays in step,
 * which is the part that cannot be skipped: a .bpt has no framing,
 * so one unread value desynchronises everything after it.
 */
/* Both defined below, beside the rest of the slot code. */
static void objsnd_arm(MR_SoundSlot *s);
static void objsnd_add_entry(int kind, int descId, int signalKind);


int objsnd_load(const char *logicalName)
{
	MR_Script s;
	char      word[128];
	int       guard;

	objsnd_reset();

	if (!logicalName || !bpt_open(&s, logicalName)) {
		/* Most tracks have no script. Not an error. */
		return 0;
	}

	for (guard = 0; guard < 4096; guard++) {
		if (!bpt_read_string(&s, word, (int)sizeof word))
			break;

		if (strcmp(word, "soundDesc") == 0) {
			MR_SoundDesc d;
			int          i;
			int          count = 0;
			int          first = -1;

			memset(&d, 0, sizeof d);
			if (!bpt_read_int(&s, &d.id) ||
			    !bpt_read_int(&s, &count))
			    break;

			for (i = 0; i < count; i++) {
				int id;

				if (!bpt_read_string(&s, word,
				                     (int)sizeof word))
				    goto done;
				id = objsnd_intern(word);
				if (i == 0)
					first = id;
			}
			d.firstSound  = first;
			d.sampleCount = count;

			if (!bpt_read_string(&s, word, (int)sizeof word))
				break;
			d.flags = objsnd_flags(word);

			if (!bpt_read_int(&s, &d.waitBase)      ||
			    !bpt_read_int(&s, &d.waitRand)      ||
			    !bpt_read_int(&s, &d.sustainBase)   ||
			    !bpt_read_int(&s, &d.sustainRand)   ||
			    !bpt_read_int(&s, &d.fadeInMs)      ||
			    !bpt_read_int(&s, &d.fadeOutMs)     ||
			    !bpt_read_int(&s, &d.repeatBase)    ||
			    !bpt_read_int(&s, &d.repeatRand)    ||
			    !bpt_read_int(&s, &d.freqBase)      ||
			    !bpt_read_int(&s, &d.freqRand)      ||
			    !bpt_read_int(&s, &d.fadeFreqDelta) ||
			    !bpt_read_int(&s, &d.volume)        ||
			    !bpt_read_int(&s, &d.pan)           ||
			    !bpt_read_float(&s, &d.radiusIn)    ||
			    !bpt_read_float(&s, &d.radiusOut))
			    break;

			if (s_descCount < OBJSND_MAX_DESC)
				s_desc[s_descCount++] = d;
			else
				mrlog_err("objsnd: more than %d descriptors\n",
			              OBJSND_MAX_DESC);

		} else if (strcmp(word, "ambiance") == 0) {
			int n = 0, i, v;

			if (!bpt_read_int(&s, &n))
				break;
			for (i = 0; i < n; i++) {
				if (!bpt_read_int(&s, &v))
					goto done;
				objsnd_add_entry(OBJSND_AT_GLOBAL, v, 0);
			}

		} else if (strcmp(word, "signal") == 0) {
			int kind = 0, n = 0, i, v;

			/*
			 * THE FIRST NUMBER IS THE SIGNAL KIND, NOT AN ID.
			 * The n values after it are descriptor ids, one
			 * entry each. Reading it the other way round
			 * parses just as cleanly and binds the wrong
			 * sounds to the wrong places.
			 */
			if (!bpt_read_int(&s, &kind) || !bpt_read_int(&s, &n))
				break;
			for (i = 0; i < n; i++) {
				if (!bpt_read_int(&s, &v))
					goto done;
				objsnd_add_entry(OBJSND_AT_SEGMENT, v, kind);
			}

		} else {
			/*
			 * THE STREAM IS NOW LOST. Without framing there is
			 * no way back in, so stop rather than read noise
			 * as descriptors.
			 */
			mrlog_err("objsnd: unknown command '%s' - stopping\n",
			          word);
			break;
		}
	}
done:
	bpt_close(&s);

	/*
	 * THE SAMPLES, ONE PER INTERNED NAME [0x004059a0]. The path is
	 * the Divers bank's, and audio.c picks 8- or 16-bit for us.
	 */
	{
		int i;
		int loaded = 0;

		for (i = 0; i < s_nameCount; i++) {
			if (audio_bind_cue(OBJSND_ID_BASE + i, "DIVERS\\",
			                   s_name[i]))
			    loaded++;
			else
				mrlog_err("objsnd: sample not found for "
			              "object sound '%s'\n", s_name[i]);
		}
		mrlog("objsnd: %s - %d descriptors, %d samples (ids "
		      "0x%02x..0x%02x)\n", logicalName, s_descCount,
		      loaded, OBJSND_ID_BASE,
		      OBJSND_ID_BASE + s_nameCount - 1);

		/*
		 * RE-ARM WHAT THE PARSE ARMED. ambiance and signal
		 * entries are appended while the script is still being
		 * read, which is before any of this ran - so a
		 * descriptor with freqBase < 1 asked audio_cue_rate for
		 * the rate of a sample that did not exist yet and got 0.
		 * Silent, and it would have read as a missing sound
		 * rather than a wrong pitch.
		 *
		 * The decor entries are armed later still, by
		 * objsnd_bind, and never see this.
		 */
		for (i = 0; i < s_boundCount; i++)
			objsnd_arm(&s_bound[i].slot);
	}
	return s_descCount;
}


static const MR_SoundDesc *objsnd_find(int id)
{
	int i;

	for (i = 0; i < s_descCount; i++) {
		if (s_desc[i].id == id)
			return &s_desc[i];
	}
	return NULL;
}


/*
 * ORIGIN  ObjSnd_ArmSlot @ 0x00404ee0 - arm a slot from its descriptor.
 *
 * EVERY SPREAD IS DRAWN HERE, ONCE PER PLAY. That is what stops six
 * gulls on the same descriptor calling in unison: each draws its own
 * wait, its own pitch and its own sample.
 */
static void objsnd_arm(MR_SoundSlot *s)
{
	const MR_SoundDesc *d = s->desc;

	s->sound = d->firstSound;
	if (d->sampleCount > 1) {
		s->sound = d->firstSound +
		           (rand_next(OBJSND_RAND_STREAM) % d->sampleCount);
	}

	s->voice   = MR_VOICE_NONE;
	s->stopped = 1;

	/*
	 * A freqBase UNDER 1 MEANS "THE SAMPLE'S OWN RATE", and the
	 * value is then an OFFSET from it - mouet asks for -1500, so
	 * the gulls call a little below the recording.
	 */
	s->freq = d->freqBase;
	if (d->freqBase < 1)
		s->freq = d->freqBase + audio_cue_rate(s->sound);
	if (d->freqRand)
		s->freq += rand_next(OBJSND_RAND_STREAM) % d->freqRand;

	s->wait = d->waitBase;
	if (d->waitRand)
		s->wait += rand_next(OBJSND_RAND_STREAM) % d->waitRand;

	s->sustain = d->sustainBase;
	if (d->sustainRand)
		s->sustain += rand_next(OBJSND_RAND_STREAM) % d->sustainRand;

	s->repeats = d->repeatBase;
	if (d->repeatRand)
		s->repeats += rand_next(OBJSND_RAND_STREAM) % d->repeatRand;

	s->fadeIn  = d->fadeInMs;
	s->fadeOut = d->fadeOutMs;
	s->volume  = d->volume;          /* master applied by audio.c */
	s->stamp   = (unsigned int)(tm_now() * 1000.0);
	s->state   = ST_WAIT;
	s->lastDist = 0.0f;

	s->cutoff = d->radiusOut * OBJSND_UNITS_PER_M;
	s->slope  = 0.0f;
	if (d->radiusOut > d->radiusIn) {
		s->slope = OBJSND_VOL_SPAN /
		           ((d->radiusOut - d->radiusIn) *
		            OBJSND_UNITS_PER_M);
	}
}


/*
 * ORIGIN  ObjSnd_AddAmbiance @ 0x00406030
 *         ObjSnd_AddSignal   @ 0x004060a0
 *
 * Both append to the SAME list ObjSnd_BindObjects appends to,
 * and both run during the parse - so the script's entries come
 * first and the decor objects after, which is the order MOTO's
 * list reads back in.
 *
 * AN UNKNOWN DESCRIPTOR IS DROPPED SILENTLY HERE, unlike the
 * decor path, which raises a message box.  Matched: a signal
 * naming a descriptor the script has not defined YET is not
 * an error the original reports.
 */
static void objsnd_add_entry(int kind, int descId, int signalKind)
{
	const MR_SoundDesc *d = objsnd_find(descId);
	MR_ObjSound        *o;

	if (!d)
		return;
	if (s_boundCount >= OBJSND_MAX_BOUND) {
		mrlog_err("objsnd: too many objects with a sound\n");
		return;
	}

	o = &s_bound[s_boundCount];
	memset(o, 0, sizeof *o);
	o->kind       = kind;
	o->segment    = -1;
	o->instance   = -1;
	o->signalKind = signalKind;
	o->slot.desc  = d;
	objsnd_arm(&o->slot);
	s_boundCount++;
}


/*
 * WHERE AN OBJECT'S SOUND ACTUALLY COMES FROM.
 *
 * ORIGIN  ObjSnd_TickAll @ 0x00405bc0, its kind-0 arm at
 *         0x00405cd0 onward
 *
 * NOT THE INSTANCE ORIGIN.  The original rotates an EMITTER
 * OFFSET by the instance's own euler triple and adds the
 * instance position, and the offset is the first vertex of
 * the object's mesh - `*(instance+0x4c) + 4` in the
 * decompiler, which is the model's vertex array.
 *
 * THIS IS NOT A REFINEMENT, IT IS THE WHOLE EFFECT FOR
 * ANYTHING THAT TURNS.  ile01's corsair is the case that
 * proves it: its instance NEVER TRANSLATES - a write
 * watchpoint on MOTO's copy caught nothing across a flypast
 * - and only its three euler angles move.  The model origin
 * is the PIVOT of the circular flight path and the aeroplane
 * geometry sits 17610 units, 137 metres, out on the end of
 * that arm.  Rotating the instance is what flies it.
 *
 * Read the sound off the pivot instead and it never moves:
 * the distance is constant, so the volume is constant, and
 * the plane drones at one level through a pass that should
 * swell and fade.  MOTO's distance ran 4156 -> 476 -> 6802
 * over 64 frames where ours sat at 16203 throughout.
 * [verified - live, both builds]
 *
 * MESH 0 OF THE THREE, and the original instead follows
 * whichever LOD the instance currently points at.  All three
 * of corsair3's share a first vertex, so nothing separates
 * them here; a template whose LODs disagree would move its
 * emitter as the detail level changed.  [open]
 */
static void objsnd_emitter_pos(const MR_Trk *trk, int instance,
                               float out[3])
{
	const MR_TrkInstance *in = &trk->instances[instance];
	const MR_TrkMesh     *m;
	const float          *v;
	float                 r[9];

	out[0] = in->pos[0];
	out[1] = in->pos[1];
	out[2] = in->pos[2];

	if (!trk->named || in->namedIndex < 0 ||
	    in->namedIndex >= trk->namedCount)
	    return;

	m = &trk->named[in->namedIndex].mesh[0];
	if (!m->verts || m->vertexCount < 1)
		return;

	/*
	 * THE DECOR EULER, NOT THE MODEL INSTANCES' - the same
	 * one trk_draw uses to place the prop, because the sound
	 * has to come from where the thing is DRAWN.  Using
	 * mat34_from_euler here would tilt the emitter off the
	 * model on anything carrying pitch or roll.
	 */
	mat34_from_euler_decor(r, in->rot);
	v = m->verts;

	out[0] = v[0] * r[0] + v[1] * r[3] + v[2] * r[6] + in->pos[0];
	out[1] = v[0] * r[1] + v[1] * r[4] + v[2] * r[7] + in->pos[1];
	out[2] = v[0] * r[2] + v[1] * r[5] + v[2] * r[8] + in->pos[2];
}


/*
 * ORIGIN  Trk_IsSegmentNearPlayer @ 0x0040d350
 *
 * Is seg inside the window of +/-window segments around cur,
 * wrapping at the end of the track?
 *
 * THE COMPARISONS ARE STRICT AT BOTH ENDS in the original, so a
 * segment exactly `window` away is OUT. Kept as it is: the two
 * boundary segments are the difference between matching MOTO
 * and not, and there is no reason to improve on it.
 *
 * THE WRAPPED CASE IS THE NORMAL ONE near the start line - at
 * segment 3 of ile01's 788 the window is (731,788) plus
 * [0,63), which is why it is written as two ranges rather than
 * a distance.
 */
static int objsnd_segment_near(const MR_Trk *trk, int seg, int cur,
                               int window)
{
	int n = trk->segmentCount;
	int lo, hi;

	if (n <= 0)
		return 0;
	lo = ((n - window) + cur) % n;
	hi = (cur + window) % n;

	if (hi < lo)
		return (lo < seg && seg < n) || (seg > -1 && seg < hi);
	return lo < seg && seg < hi;
}


int objsnd_bind(const MR_Trk *trk)
{
	int seg;

	/*
	 * NO RESET HERE. The signal and ambiance entries were
	 * already appended during objsnd_load, and this APPENDS
	 * the decor objects after them - the order MOTO's list
	 * reads back in. objsnd_reset, at the top of objsnd_load,
	 * is what clears the list.
	 *
	 * So this must be called ONCE per load. Twice would bind
	 * every object a second time.
	 */
	if (!trk || s_descCount == 0)
		return 0;

	/*
	 * WALKED PER SEGMENT, NOT OVER instances[], AND THE SEGMENT
	 * IS THE POINT. A flat walk finds the same objects - it
	 * found exactly MOTO's eleven on ile01 - but it cannot say
	 * WHERE each one is on the track, and that is what decides
	 * whether it may be heard at all. See objsnd_tick.
	 *
	 * An instance listed by two segments binds TWICE here, as
	 * it does in the original: two slots, two independent
	 * random draws. No shipped track does it.
	 */
	for (seg = 0; seg < trk->segmentCount; seg++) {
		const MR_TrkSegment *sg = &trk->segments[seg];
		int                  k;

		if (!sg->indices)
			continue;

		for (k = 0; k < sg->indexCount; k++) {
			int                   id = (int)sg->indices[k];
			const MR_TrkInstance *in;
			const MR_SoundDesc   *d;

			if (id < 0 || id >= trk->instanceCount)
				continue;
			in = &trk->instances[id];
			if ((int)in->soundDesc < 0)
				continue;

			d = objsnd_find((int)in->soundDesc);
			if (!d) {
				/*
				 * The original raises a message box here.
				 * A log line is enough: a track naming a
				 * descriptor it did not define is an
				 * authoring fault, not something the port
				 * can fix.
				 */
				mrlog_err("objsnd: no sound description %d "
				          "for '%s'\n",
				          (int)in->soundDesc, in->name);
				continue;
			}
			if (s_boundCount >= OBJSND_MAX_BOUND) {
				mrlog_err("objsnd: too many objects with "
				          "a sound\n");
				goto done;
			}

			memset(&s_bound[s_boundCount], 0,
			       sizeof s_bound[0]);
			s_bound[s_boundCount].kind     = OBJSND_AT_OBJECT;
			s_bound[s_boundCount].segment  = seg;
			s_bound[s_boundCount].instance = id;
			s_bound[s_boundCount].slot.desc = d;
			objsnd_arm(&s_bound[s_boundCount].slot);
			s_boundCount++;
		}
	}
done:

	{
		int k, obj = 0, sig = 0, glob = 0;

		for (k = 0; k < s_boundCount; k++) {
			if (s_bound[k].kind == OBJSND_AT_OBJECT)
				obj++;
			else if (s_bound[k].kind == OBJSND_AT_SEGMENT)
				sig++;
			else
				glob++;
		}
		mrlog("objsnd: %d bound - %d object, %d signal, "
		      "%d ambiance\n", s_boundCount, obj, sig, glob);
	}
	return s_boundCount;
}


int objsnd_bound_count(void)
{
	return s_boundCount;
}


/* The mixer wants -1..+1; the script speaks the original's -64..+64. */
static float objsnd_pan_out(int pan)
{
	if (pan < -64)
		pan = -64;
	if (pan > 64)
		pan = 64;
	return (float)pan / 64.0f;
}


/*
 * ORIGIN  ObjSnd_TickSlot @ 0x00405030 - one sounding object, one frame.
 *
 * audible is the placement's own verdict: a segment too far round
 * the track to matter, or the detail level turned down, and the
 * voice is dropped without consulting the distance at all.
 */
static void objsnd_slot_tick(MR_SoundSlot *s, int audible,
                             const float pos[3], const float eye[3],
                             float fwdX, float fwdZ)
{
	const MR_SoundDesc *d = s->desc;
	unsigned int        now = (unsigned int)(tm_now() * 1000.0);
	unsigned int        age;
	int                 vol = 255;
	int                 pan;

	if (!d)
		return;

	/* A clock that went backwards - a reset - is one frame. */
	if (now < s->stamp)
		s->stamp = now;
	age = now - s->stamp;

	pan = d->pan;

	if (!audible) {
		if (!s->stopped) {
			audio_voice_stop(s->voice);
			s->voice   = MR_VOICE_NONE;
			s->stopped = 1;
		}
		return;
	}

	/*
	 * THE DISTANCE MODEL, and it is the racers' one: measured from
	 * the CAMERA EYE, silent past radiusOut, full inside radiusIn.
	 * Crossing out of range STOPS the voice rather than leaving it
	 * inaudible, which is what keeps eight slots enough for a
	 * track with forty sounding objects.
	 */
	if ((d->flags & OBJSND_VOL) != 0) {
		float dx = pos[0] - eye[0];
		float dy = pos[1] - eye[1];
		float dz = pos[2] - eye[2];
		float dist = (float)sqrt(dx * dx + dy * dy + dz * dz);

		if (dist > s->cutoff) {
			if (s->cutoff < s->lastDist && !s->stopped) {
				audio_voice_stop(s->voice);
				s->voice   = MR_VOICE_NONE;
				s->stopped = 1;
			}
			s->lastDist = dist;
			return;
		}
		vol = (int)((s->cutoff - dist) * s->slope);
		if (vol > 255)
			vol = 255;
		if (vol < 0)
			vol = 0;
		s->lastDist = dist;
	}

	/*
	 * PAN IS THE SINE OF THE ANGLE OFF THE CAMERA'S FORWARD -
	 * the 2D cross of the flattened forward with the direction to
	 * the object, which is the same expression race_audio_pan
	 * uses for the bikes. Zero dead ahead, +/-64 to the sides.
	 *
	 * NO DEAD ZONE HERE, unlike the racers': a gull is a point
	 * that moves past you and the jitter the racers' zone exists
	 * to suppress does not arise.
	 */
	if ((d->flags & OBJSND_PAN) != 0) {
		float dx  = pos[0] - eye[0];
		float dz  = pos[2] - eye[2];
		float len = (float)sqrt(dx * dx + dz * dz);

		if (len > 0.0f) {
			dx /= len;
			dz /= len;
			pan = (int)((dx * fwdZ - dz * fwdX) * 64.0f);
		}
	}

	/* A LOOPED voice that lost its slot is re-armed, not resumed. */
	if ((d->flags & OBJSND_LOOPED) != 0 && s->state == ST_SUSTAIN &&
	    s->stopped) {
		s->voice = audio_voice_start(s->sound,
		                             SND_FLAG_POLYPHONIC |
		                             (d->flags & OBJSND_LOOPED));
		s->stopped = (s->voice == MR_VOICE_NONE);
	}

	switch (s->state) {
	case ST_WAIT:
		if (age > (unsigned int)s->wait)
			s->state = ST_START;
		break;

	case ST_START:
		s->voice = audio_voice_start(s->sound,
		                             SND_FLAG_POLYPHONIC |
		                             (d->flags & OBJSND_LOOPED));
		s->stopped = (s->voice == MR_VOICE_NONE);
		audio_voice_set_freq(s->voice, s->freq);
		s->stamp = now;

		if ((d->flags & OBJSND_FADEIN) == 0) {
			s->state = ST_SUSTAIN;
			audio_voice_set_volume(s->voice,
			                       (s->volume * vol) >> 8);
		} else {
			s->state = ST_FADEIN;
			audio_voice_set_volume(s->voice, 0);
		}

		/*
		 * RANDPAN IS DRAWN AT THE TRIGGER, not per frame - the
		 * birds scatter around you and then hold still while
		 * each call plays out.
		 */
		if ((d->flags & OBJSND_RANDPAN) != 0)
			pan = (rand_next(OBJSND_RAND_STREAM) & 0x7f) - 0x40;
		audio_voice_set_pan(s->voice, objsnd_pan_out(pan));
		break;

	case ST_FADEIN: {
		int up = s->fadeIn > 0
			     ? (int)(((float)age / (float)s->fadeIn) *
			             (float)s->volume)
			     : s->volume;

		if (up > s->volume)
			up = s->volume;
		audio_voice_set_volume(s->voice, (up * vol) >> 8);
		audio_voice_set_pan(s->voice, objsnd_pan_out(pan));

		if ((d->flags & OBJSND_FADEFREQ) != 0 && s->fadeIn > 0) {
			audio_voice_set_freq(s->voice,
				    s->freq + (d->fadeFreqDelta * (int)age) /
				              s->fadeIn);
		}
		if (age > (unsigned int)s->fadeIn) {
			s->stamp = now;
			s->state = ST_SUSTAIN;
		}
		break;
	}

	case ST_SUSTAIN: {
		int level = s->volume;

		if ((d->flags & OBJSND_INFPLAY) == 0 &&
			age > (unsigned int)s->sustain) {
			s->state = ((d->flags & OBJSND_FADEOUT) != 0)
				       ? ST_FADEOUT : ST_STOP;
			s->stamp = now;
		}

		/*
		 * THE TWO LFOs SHARE sustainBase AS THEIR PERIOD - the
		 * original reads descriptor +0x18 for both, which is
		 * the same field the sustain length comes from. vent
		 * asks for 22000 ms, so the wind breathes once every
		 * twenty-two seconds and never repeats against the
		 * other three copies of itself.
		 */
		if (d->sustainBase > 0) {
			double ph = ((double)age / (double)d->sustainBase) *
				        MR_TWO_PI;

			if ((d->flags & OBJSND_SINVOL) != 0)
				level = s->volume +
				        (int)(sin(ph) * (double)s->volume);
			if ((d->flags & OBJSND_SINFREQ) != 0) {
				audio_voice_set_freq(s->voice,
					    s->freq +
					    (int)(sin(ph) *
					          (double)d->fadeFreqDelta));
			}
		}
		if (level < 0)
			level = 0;
		audio_voice_set_volume(s->voice, (level * vol) >> 8);
		audio_voice_set_pan(s->voice, objsnd_pan_out(pan));
		break;
	}

	case ST_FADEOUT: {
		int left = s->fadeOut > 0
			       ? (int)(((float)(s->fadeOut - (int)age) /
			                (float)s->fadeOut) * (float)s->volume)
			       : 0;

		if (left < 0)
			left = 0;
		audio_voice_set_volume(s->voice, (left * vol) >> 8);
		audio_voice_set_pan(s->voice, objsnd_pan_out(pan));

		if ((d->flags & OBJSND_FADEFREQ) != 0 && s->fadeOut > 0) {
			audio_voice_set_freq(s->voice,
				    s->freq + (d->fadeFreqDelta *
				               (s->fadeOut - (int)age)) /
				              s->fadeOut);
		}
		if (age > (unsigned int)s->fadeOut) {
			s->stamp = now;
			s->state = ST_STOP;
		}
		break;
	}

	case ST_STOP:
		audio_voice_stop(s->voice);
		s->voice   = MR_VOICE_NONE;
		s->stopped = 1;
		if (--s->repeats <= 0) {
			/* Out of plays - draw a fresh set and wait. */
			objsnd_arm(s);
		} else {
			s->stamp = now;
			s->state = ST_WAIT;
		}
		break;

	default:
		s->state = ST_WAIT;
		break;
	}
}


/*
 * ORIGIN  ObjSnd_TickAll @ 0x00405bc0 - place every bound sound.
 *
 * THE ORIGINAL ROTATES AN EMITTER OFFSET by the instance's own
 * euler triple and adds the instance position [0x00405cd0 onward,
 * through g_cosTable]. Ours uses the instance position itself: the
 * animated decor already writes pos[] every frame - AnimDecor_Load
 * drives the gulls and the helicopter rotor through it - so a
 * moving object moves its sound. What is missing is the offset
 * WITHIN the object, which matters only for something whose sound
 * should not come from its origin.
 * [open - the emitter offset at instance+0x4c]
 */
void objsnd_tick(const MR_Trk *trk, const float eye[3],
                 float fwdX, float fwdZ, int curSegment)
{
	int i;
	int sigSeg[TRK_SIGNAL_KINDS];

	if (!trk || !eye || audio_paused())
		return;

	/*
	 * WHERE EACH SIGNAL SPAN IS *RIGHT NOW*, rebuilt every
	 * frame because it moves with the player. -1 first: the
	 * search only overwrites a slot it finds something for.
	 */
	for (i = 0; i < TRK_SIGNAL_KINDS; i++)
		sigSeg[i] = -1;
	trk_find_signal_segments(trk, sigSeg, curSegment,
	                         OBJSND_SIGNAL_WINDOW);

	for (i = 0; i < s_boundCount; i++) {
		MR_ObjSound *o = &s_bound[i];
		float        pos[3];
		int          audible = 1;

		switch (o->kind) {
		case OBJSND_AT_OBJECT:
			/*
			 * DETAIL 0 SKIPS THE SLOT ENTIRELY - it does not
			 * tick it with audible 0. That is the original's
			 * shape at 0x00405be9, and the difference is
			 * real: a voice already sounding is left alone
			 * rather than released.
			 */
			if (g_options.detail == 0)
				continue;
			if (o->instance < 0 ||
			    o->instance >= trk->instanceCount) {
				audible = 0;
				pos[0] = pos[1] = pos[2] = 0.0f;
				break;
			}
			/*
			 * HOW FAR ROUND THE TRACK, not how far away. The
			 * distance falloff alone is not enough - ile01's
			 * aeroplane carries far enough to be heard from
			 * the grid, half a lap before it is in sight.
			 */
			audible = objsnd_segment_near(trk, o->segment,
			                              curSegment,
			                              OBJSND_SEG_WINDOW);
			objsnd_emitter_pos(trk, o->instance, pos);
			break;

		case OBJSND_AT_SEGMENT: {
			/*
			 * signalKind IS NOT A SEGMENT. It selects one of
			 * the seven spans, and the span resolves to a
			 * DIFFERENT segment every frame as the player
			 * moves along it - which is what makes surf
			 * follow you down the beach instead of sitting
			 * at one post.
			 *
			 * Nothing in range leaves -1 and the sound is
			 * simply inaudible this frame.
			 */
			int bit = o->signalKind - TRK_SIGNAL_BASE;
			int at  = (bit >= 0 && bit < TRK_SIGNAL_KINDS)
				          ? sigSeg[bit] : -1;

			if (at < 0 || at >= trk->segmentCount) {
				audible = 0;
				pos[0] = pos[1] = pos[2] = 0.0f;
				break;
			}
			pos[0] = trk->segments[at].pos[0];
			pos[1] = trk->segments[at].pos[1];
			pos[2] = trk->segments[at].pos[2];
			break;
		}

		default:                      /* OBJSND_AT_GLOBAL */
			pos[0] = pos[1] = pos[2] = 0.0f;
			break;
		}

		objsnd_slot_tick(&o->slot, audible, pos, eye, fwdX, fwdZ);
	}
}
