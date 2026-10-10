/*
 * assets.h - THE ONE PLACE AN ASSET IS RESOLVED.
 *
 * CLAUDE.md rule 10: the deliverable is an executable someone drops
 * into an existing MotoRacer installation, so the reconstruction must
 * read assets where a stock install keeps them - inside the .BKF
 * archives. decomp/DATA/extracted/ is not shipped data; it came from an
 * extraction tool and will not exist on anyone else's machine.
 *
 * Everything that loads a file goes through asset_load. That is the
 * point of this file: one chokepoint, so where assets come from is one
 * change and not twenty. No loader calls fopen on a game asset any
 * more.
 *
 *
 * LOGICAL NAMES. A caller asks for a name in the form the extracted
 * tree mirrors, with the archive stem as the first component:
 *
 *     PAGES\TITLEDSI\D.3DF
 *     MENU\BITMAP\LODANIM.LEZ
 *     ILE01\LOAD.LEZ
 *
 * and asset_load splits the first component off, opens
 * <datadir>\PAGES.BKF, and looks up the member "TITLEDSI\D.3DF". That
 * mapping is not a convention we invented - it is what the archives
 * already contain. Member names carry their subdirectory with a
 * backslash, so a member name IS the tail of the logical name.
 * Verified: 403 members across four archives compare byte-identical
 * against the extracted tree.
 *
 * RESOLUTION ORDER, archive first on purpose:
 *
 *   1  the .BKF member          - what a stock install has
 *   2  <datadir>\<logical>      - loose files: .BPT, .trk, and a
 *                                 player who unpacked their own game
 *   3  <datadir>\extracted\<logical>
 *                               - the pre-extracted tree, LAST, and
 *                                 only while the rule 10 testing
 *                                 exemption stands
 *
 * Archive first means the stock path is the one being exercised every
 * run, so it cannot quietly rot while the extracted tree covers for
 * it. If step 3 ever fires for something step 1 should have had, that
 * is a bug worth seeing - so it says so.
 *
 * A leading "extracted\" on a logical name is STRIPPED before any of
 * this. Old mracer.ini files hold path settings written when the
 * extracted tree was the only source, and silently accepting them
 * costs four lines.
 */

#ifndef MRACER_ASSETS_H
#define MRACER_ASSETS_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Scans the data directory for .BKF archives and opens each one's
 * directory. Returns the number opened; zero is not fatal, it just
 * means every lookup falls through to the loose-file paths.
 */
int  asset_init(void);
void asset_shutdown(void);

/*
 * Loads an asset into a fresh buffer. Caller frees. Returns NULL and
 * reports if it cannot be found by any route.
 */
void *asset_load(const char *logical, unsigned int *sizeOut);

/* Does it resolve at all? Cheap - no member is read. */
int asset_exists(const char *logical);

/* Counters, so a run can say how much came from where. */
void asset_stats(int *fromArchive, int *fromLoose, int *fromExtracted,
                 int *missing);


/*
 * asset_name_from_script - a path out of a script into an asset name.
 *
 * Script_Open (0x004242a0) uppercases the path and then, unless it
 * holds ".DEM", truncates at the FIRST dot and appends BPT - so
 * "Data\Config.Spt" opens DATA\CONFIG.BPT. That is the game's rule
 * and it is reproduced here.
 *
 * The "data\" prefix then comes off, because the original hands the
 * result to fopen and we hand it to asset_load: what is left,
 * "SPEED\CARAC1.BPT", is already the archive stem plus a member.
 * CLAUDE.md rule 10.
 *
 * rewriteExt SWAPS THE EXTENSION FOR BPT and is only right for a
 * SCRIPT path. A .3df, a .raw or a .rot is asked for by the name it
 * already has, so those callers pass 0 - getting this backwards asks
 * the archive for a member that does not exist.
 */
int asset_name_from_script(const char *path, char *out, int size,
                           int rewriteExt);

#ifdef __cplusplus
}
#endif

#endif /* MRACER_ASSETS_H */
