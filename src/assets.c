/*
 * assets.c - see assets.h.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "assets.h"
#include "bkf.h"
#include "config.h"
#include "log.h"

/* Twenty-three archives ship; the cap is generous so a modded install
 * with extras still works. */
#define ASSET_MAX_ARCHIVES 64

typedef struct {
	char  stem[64];      /* "PAGES", upper case                        */
	BKF  *bank;
} ASSET_ARCHIVE;

static ASSET_ARCHIVE s_arch[ASSET_MAX_ARCHIVES];
static int           s_archCount;

static int s_statArchive;
static int s_statLoose;
static int s_statExtracted;
static int s_statMissing;

static void upcase(char *s)
{
	for (; *s; s++) {
		if (*s >= 'a' && *s <= 'z')
			*s = (char)(*s - 'a' + 'A');
	}
}

int asset_init(void)
{
	WIN32_FIND_DATAA fd;
	HANDLE           h;
	char             pattern[MAX_PATH];
	const char      *dir = get_data_dir();

	asset_shutdown();

	if (!dir || !dir[0])
		return 0;
	if (strlen(dir) + 6 >= sizeof(pattern))
		return 0;
	strcpy(pattern, dir);
	strcat(pattern, "*.BKF");

	h = FindFirstFileA(pattern, &fd);
	if (h == INVALID_HANDLE_VALUE) {
		mrlog("assets: no .BKF archives in %s\n", dir);
		mrlog("        falling back to loose files - see CLAUDE.md "
		       "rule 10\n");
		return 0;
	}

	do {
		char  path[MAX_PATH];
		char *dot;
		BKF  *b;

		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			continue;
		if (s_archCount >= ASSET_MAX_ARCHIVES)
			break;
		if (strlen(dir) + strlen(fd.cFileName) >= sizeof(path))
			continue;

		strcpy(path, dir);
		strcat(path, fd.cFileName);

		b = bkf_open(path);
		if (!b)
			continue;    /* bkf_open has already said why */

		if (strlen(fd.cFileName) >= sizeof(s_arch[s_archCount].stem))
			continue;
		strcpy(s_arch[s_archCount].stem, fd.cFileName);
		dot = strrchr(s_arch[s_archCount].stem, '.');
		if (dot)
			*dot = 0;
		upcase(s_arch[s_archCount].stem);

		s_arch[s_archCount].bank = b;
		s_archCount++;
	} while (FindNextFileA(h, &fd));
	FindClose(h);

	mrlog("assets: %d .BKF archive(s) open in %s\n", s_archCount, dir);
	return s_archCount;
}

void asset_shutdown(void)
{
	int i;
	for (i = 0; i < s_archCount; i++) {
		bkf_close(s_arch[i].bank);
		s_arch[i].bank = NULL;
	}
	s_archCount = 0;
}

/*
 * Strip a leading "extracted\" or "extracted/". An mracer.ini written
 * before the archives were read holds path settings that start with it,
 * and honouring them costs almost nothing.
 */
static const char *strip_extracted(const char *logical)
{
	static const char kPrefix[] = "extracted";
	size_t n = sizeof(kPrefix) - 1;

	if (_strnicmp(logical, kPrefix, n) == 0 &&
	    (logical[n] == '\\' || logical[n] == '/'))
	    return logical + n + 1;
	return logical;
}

/* Split "PAGES\TITLEDSI\D.3DF" into stem "PAGES" and member
 * "TITLEDSI\D.3DF". Returns 0 if there is no separator at all. */
static int split_logical(const char *logical, char *stem, int stemSize,
                         const char **member)
{
	const char *sep = logical;
	int         n;

	while (*sep && *sep != '\\' && *sep != '/')
		sep++;
	if (!*sep)
		return 0;

	n = (int)(sep - logical);
	if (n <= 0 || n >= stemSize)
		return 0;
	memcpy(stem, logical, (size_t)n);
	stem[n] = 0;
	upcase(stem);

	*member = sep + 1;
	return 1;
}

static BKF *find_archive(const char *stem)
{
	int i;
	for (i = 0; i < s_archCount; i++) {
		if (strcmp(s_arch[i].stem, stem) == 0)
			return s_arch[i].bank;
	}
	return NULL;
}

/* Read a loose file whole. */
static void *read_loose(const char *path, unsigned int *sizeOut)
{
	FILE *f;
	long  n;
	void *buf;

	f = fopen(path, "rb");
	if (!f)
		return NULL;
	if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) <= 0 ||
	    fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return NULL;
	}
	buf = malloc((size_t)n);
	if (!buf) {
		fclose(f);
		return NULL;
	}
	if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
		free(buf);
		fclose(f);
		return NULL;
	}
	fclose(f);
	if (sizeOut)
		*sizeOut = (unsigned int)n;
	return buf;
}

static int loose_path(char *out, int size, const char *prefix,
                      const char *logical)
{
	const char *dir = get_data_dir();

	if (!dir || (int)(strlen(dir) + strlen(prefix) + strlen(logical)) >= size)
		return 0;
	strcpy(out, dir);
	strcat(out, prefix);
	strcat(out, logical);
	return 1;
}

void *asset_load(const char *logical, unsigned int *sizeOut)
{
	char        stem[64];
	char        path[MAX_PATH];
	const char *member;
	const char *name;
	BKF        *bank;
	void       *buf;

	if (sizeOut)
		*sizeOut = 0;
	if (!logical || !logical[0])
		return NULL;

	name = strip_extracted(logical);

	/* 1. The archive - what a stock install has. */
	if (split_logical(name, stem, (int)sizeof(stem), &member)) {
		bank = find_archive(stem);
		if (bank) {
			int idx = bkf_find(bank, member);
			if (idx >= 0) {
				buf = bkf_read(bank, idx, sizeOut);
				if (buf) {
					s_statArchive++;
					return buf;
				}
			}
		}
	}

	/* 2. A loose file under the data directory. */
	if (loose_path(path, (int)sizeof(path), "", name)) {
		buf = read_loose(path, sizeOut);
		if (buf) {
			s_statLoose++;
			return buf;
		}
	}

	/*
	 * 3. The pre-extracted tree, LAST, and only while the rule 10
	 * testing exemption stands. It says so out loud on the first few,
	 * because anything reaching here that an archive should have had
	 * is a bug in the mapping, not a feature.
	 */
	if (loose_path(path, (int)sizeof(path), "extracted\\", name)) {
		buf = read_loose(path, sizeOut);
		if (buf) {
			s_statExtracted++;
			if (s_statExtracted <= 5) {
				mrlog("assets: %s came from the PRE-EXTRACTED tree, "
				       "not an archive\n", name);
			}
			return buf;
		}
	}

	s_statMissing++;
	mrlog_err("assets: cannot find %s\n", name);
	return NULL;
}

int asset_exists(const char *logical)
{
	char        stem[64];
	char        path[MAX_PATH];
	const char *member;
	const char *name;
	BKF        *bank;

	if (!logical || !logical[0])
		return 0;
	name = strip_extracted(logical);

	if (split_logical(name, stem, (int)sizeof(stem), &member)) {
		bank = find_archive(stem);
		if (bank && bkf_find(bank, member) >= 0)
			return 1;
	}
	if (loose_path(path, (int)sizeof(path), "", name) &&
	    GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES)
	    return 1;
	if (loose_path(path, (int)sizeof(path), "extracted\\", name) &&
	    GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES)
	    return 1;
	return 0;
}

void asset_stats(int *fromArchive, int *fromLoose, int *fromExtracted,
                 int *missing)
{
	if (fromArchive)   *fromArchive   = s_statArchive;
	if (fromLoose)     *fromLoose     = s_statLoose;
	if (fromExtracted) *fromExtracted = s_statExtracted;
	if (missing)       *missing       = s_statMissing;
}

/* ORIGIN  Script_Open @ 0x004242a0, its name rule. See assets.h. */
int asset_name_from_script(const char *path, char *out, int size,
                           int rewriteExt)
{
	const char *p = path;
	int         i;
	int         n;

	if (!path || !out || size <= 0)
		return 0;

	if (_strnicmp(p, "data\\", 5) == 0 || _strnicmp(p, "data/", 5) == 0)
		p += 5;

	n = (int)strlen(p);
	if (n + 5 >= size)
		return 0;

	strcpy(out, p);
	for (i = 0; out[i]; i++) {
		if (out[i] >= 'a' && out[i] <= 'z')
			out[i] = (char)(out[i] - 'a' + 'A');
		else if (out[i] == '/')
			out[i] = '\\';
	}

	/* Only a script path gets its extension swapped. A .3df or a
	 * .raw is asked for by the name it already has. */
	if (!rewriteExt || strstr(out, ".DEM"))
		return 1;

	for (i = 0; out[i]; i++) {
		if (out[i] == '.') {
			out[i + 1] = 0;
			strcat(out, "BPT");
			return 1;
		}
	}
	strcat(out, ".BPT");
	return 1;
}
