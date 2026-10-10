/*
 * tracks.c - DATA\LIST.BPT. See tracks.h.
 */

#include <stdlib.h>
#include <string.h>

#include "tracks.h"
#include "bpt.h"
#include "log.h"

static MR_Track *s_tracks;
static int       s_count;
static char      s_listName[TRACK_NAME_MAX];
static int       s_pocketAvailable;
static int       s_reverseAvailable;

void tracks_free(void)
{
	if (s_tracks)
		free(s_tracks);
	s_tracks           = NULL;
	s_count            = 0;
	s_listName[0]      = 0;
	s_pocketAvailable  = 0;
	s_reverseAvailable = 0;
}

int tracks_load(const char *logicalName)
{
	MR_Script sc;
	char      word[TRACK_NAME_MAX];
	int       n = 0;
	int       i;

	tracks_free();

	if (!bpt_open(&sc, logicalName))
		return 0;

	if (!bpt_read_string(&sc, s_listName, (int)sizeof(s_listName)) ||
	    !bpt_read_int(&sc, &s_pocketAvailable) ||
	    !bpt_read_int(&sc, &s_reverseAvailable) ||
	    !bpt_read_int(&sc, &n)) {
		mrlog_err("tracks: %s is too short to be a list\n", logicalName);
		bpt_close(&sc);
		return 0;
	}

	/*
	 * A COUNT OUT OF A FILE IS AN ALLOCATION SIZE, so it is bounded
	 * before it is used. The original allocates count * 0x130 with no
	 * check at all; a corrupt or mis-decoded file would ask for
	 * gigabytes there and for a bad pointer here.
	 */
	if (n <= 0 || n > 64) {
		mrlog_err("tracks: %s says %d islands, which is not credible\n",
		          logicalName, n);
		bpt_close(&sc);
		return 0;
	}

	s_tracks = (MR_Track *)calloc((size_t)n, sizeof(MR_Track));
	if (!s_tracks) {
		bpt_close(&sc);
		return 0;
	}

	for (i = 0; i < n; i++) {
		MR_Track *t = &s_tracks[i];

		if (!bpt_read_string(&sc, t->name, (int)sizeof(t->name)) ||
		    !bpt_read_string(&sc, t->path, (int)sizeof(t->path)) ||
		    !bpt_read_string(&sc, word, (int)sizeof(word)) ||
		    !bpt_read_int(&sc, &t->laps)) {
			mrlog_err("tracks: %s ran out after %d of %d islands\n",
			          logicalName, i, n);
			break;
		}
		/* stricmp against "ON" at 0x004e1dbc [verified]. */
		t->available = (_stricmp(word, "ON") == 0);
	}

	s_count = i;
	bpt_close(&sc);

	mrlog("tracks: %s, list \"%s\", %d island(s), pocket %d reverse %d\n",
	      logicalName, s_listName, s_count, s_pocketAvailable,
	      s_reverseAvailable);
	for (i = 0; i < s_count; i++) {
		mrlog("        %-12s %-14s %s %d lap(s)\n", s_tracks[i].name,
		      s_tracks[i].path, s_tracks[i].available ? "ON " : "off",
		      s_tracks[i].laps);
	}
	return s_count;
}

int tracks_count(void)
{
	return s_count;
}

const MR_Track *track_at(int index)
{
	if (index < 0 || index >= s_count)
		return NULL;
	return &s_tracks[index];
}

const char *tracks_list_name(void)   { return s_listName; }
int tracks_pocket_available(void)    { return s_pocketAvailable; }
int tracks_reverse_available(void)   { return s_reverseAvailable; }

int track_asset_name(const MR_Track *t, const char *leaf,
                     char *out, int size)
{
	const char *p;
	int         n;

	if (!t || !leaf || !out || size <= 0)
		return 0;

	p = t->path;
	if (_strnicmp(p, "data\\", 5) == 0)
		p += 5;
	else if (_strnicmp(p, "data/", 5) == 0)
		p += 5;

	n = (int)(strlen(p) + strlen(leaf));
	if (n >= size)
		return 0;

	strcpy(out, p);
	strcat(out, leaf);

	/* assets.c uppercases when it looks a member up, but the log
	 * lines read better if we do it here too. */
	for (n = 0; out[n]; n++) {
		if (out[n] >= 'a' && out[n] <= 'z')
			out[n] = (char)(out[n] - 'a' + 'A');
		else if (out[n] == '/')
			out[n] = '\\';
	}
	return 1;
}
