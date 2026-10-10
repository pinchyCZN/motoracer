/*
 * tracks.h - the island list, out of DATA\LIST.BPT.
 *
 * FUN_00428d60 is the loader, and it reads a fixed sequence:
 *
 *     string  ListName          "PLUGIN"
 *     int     PocketAvailable
 *     int     ReverseAvailable
 *     int     NumberIslande     the track count
 *     then that many of:
 *         string  name          "SPEED_BAY"
 *         string  path          "data\ile01\"
 *         string  ON or OFF     compared with stricmp against "ON"
 *         int     laps          the default lap count
 *
 * [verified, and confirmed against the shipped file: eight islands,
 * three of them ON.]
 *
 * o THE THIRD STRING IS A WORD, NOT A FLAG. The loader compares it
 *   with "ON" at 0x004e1dbc - the same string the options page shows -
 *   and stores the result of the compare. Anything else is off.
 *
 * o AND THE PATH CARRIES ITS OWN "data\" PREFIX, which is a path and
 *   not an asset name. track_asset_name strips it; see there.
 *
 * The datadisk adds LIST01.BPT and so on, which is what page 11's
 * scan counts. Only the base list is loaded so far.
 */

#ifndef MRACER_TRACKS_H
#define MRACER_TRACKS_H

#ifdef __cplusplus
extern "C" {
#endif

#define TRACK_NAME_MAX 32
#define TRACK_PATH_MAX 260

typedef struct {
	char name[TRACK_NAME_MAX];   /* SPEED_BAY                     */
	char path[TRACK_PATH_MAX];   /* data\ile01\ - with the prefix */
	int  available;              /* the ON/OFF word, as 1 or 0    */
	int  laps;                   /* the default lap count         */
} MR_Track;

/*
 * tracks_load - read the list. Returns the count, or 0.
 *
 * logicalName is an asset name: "LIST.BPT" is loose in DATA on a
 * stock install, so it resolves through the loose-file arm of
 * asset_load rather than out of an archive.
 */
int tracks_load(const char *logicalName);
void tracks_free(void);

int             tracks_count(void);
const MR_Track *track_at(int index);

/* From the list header, not from a track. */
const char *tracks_list_name(void);
int         tracks_pocket_available(void);
int         tracks_reverse_available(void);

/*
 * track_asset_name - "data\ile01\" plus "MAP.LEZ" -> "ILE01\MAP.LEZ".
 *
 * The list stores a PATH, complete with a leading "data\" and a
 * trailing separator, because the original passes it to fopen. Ours
 * has to reach the same bytes inside ILE01.BKF, so the prefix comes
 * off and what is left is already the archive stem plus a separator -
 * which is exactly the shape asset_load splits on. See CLAUDE.md
 * rule 10.
 *
 * Returns 0 if the result would not fit.
 */
int track_asset_name(const MR_Track *t, const char *leaf,
                     char *out, int size);

#ifdef __cplusplus
}
#endif

#endif /* MRACER_TRACKS_H */
