/*
 * bkf.h - the .BKF asset bank.
 *
 * This is how a STOCK INSTALL stores almost everything. DATA holds
 * twenty-three .BKF archives - MENU, MENUUS, PAGES, ILE01 to ILE11,
 * the bike banks, SFX - and 74 MB of the game's 92 MB is inside one.
 * See CLAUDE.md rule 10: the deliverable is an executable someone
 * drops into an existing installation, so this is the only asset
 * source that can be relied on.
 *
 * The format was decoded before this reader was written, and
 * tools/bkf.py is the oracle:
 *
 *     u32          entry count
 *     entry[count] char name[36]   NUL-terminated, garbage after
 *                  u32  offset     absolute, from the file start
 *                  u32  size
 *     ...          member data, tiling the rest of the file
 *
 * The directory is 4 + count * 44 bytes, which is always the first
 * member's offset, and the members tile the file with no gaps and no
 * overlap - all twenty-three archives pass that check.
 *
 * MEMBER NAMES CARRY THEIR SUBDIRECTORY WITH A BACKSLASH -
 * "TITLEDSI\BNK00_00.RAW", "BANK\BNK00_02.RAW" - so a member name is
 * already the relative path an extracted tree mirrors. That is what
 * makes assets.c's mapping trivial.
 *
 * VERIFIED: 403 members across PAGES, MENU, MENUUS and ILE01 compare
 * byte-for-byte identical against the pre-extracted tree, with none
 * missing and none differing.
 */

#ifndef MRACER_BKF_H
#define MRACER_BKF_H

#ifdef __cplusplus
extern "C" {
#endif

#define BKF_NAME_LEN   36
#define BKF_ENTRY_SIZE 44

typedef struct BKF BKF;

/*
 * Opens the archive and reads its directory. The file stays OPEN and
 * members are read on demand - an archive is up to 6 MB and there is
 * no reason to hold all of it, or to re-read the directory per lookup.
 */
BKF *bkf_open(const char *path);
void bkf_close(BKF *a);

int  bkf_count(const BKF *a);
const char *bkf_name(const BKF *a, int index);
unsigned int bkf_size(const BKF *a, int index);

/*
 * bkf_find - member index, or -1.
 *
 * Case-insensitive, and treats '/' and '\\' as the same separator.
 * Both matter: the archives are all upper case, the ini and our own
 * string literals are not necessarily, and a member name uses a
 * backslash where a caller may well have typed a slash.
 */
int bkf_find(const BKF *a, const char *member);

/* Reads a member into a fresh buffer. Caller frees. */
void *bkf_read(BKF *a, int index, unsigned int *sizeOut);

#ifdef __cplusplus
}
#endif

#endif /* MRACER_BKF_H */
