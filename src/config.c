/*
 * config.c - host config / INI helpers.
 *
 * Adapted from cdino_pc/config.c. Same shape: a small table-driven
 * reader and writer over GetPrivateProfileString, with the file placed
 * beside the executable, plus a clamp that keeps a restored rect on a
 * monitor that still exists.
 *
 * The window-placement half of this is only half the job. Reading the
 * rect to SAVE is the part that has to be right, and that lives in
 * main.cpp - see normal_rect_screen there.
 */

#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "config.h"

#define WINDOW_SECTION_NAME  "WINDOW"
#define CONSOLE_SECTION_NAME "CONSOLE"
#define PATHS_SECTION_NAME   "PATHS"

/*
 * Where the game's DATA tree lives. Written into the ini on first run
 * so it is visible and editable, rather than being a constant only the
 * source knows about.
 */
/*
 * RELATIVE TO THE EXECUTABLE, not to the working directory. The
 * deliverable is an exe someone drops into their MotoRacer install
 * next to DATA - CLAUDE.md rule 10 - so the default has to find
 * that with no ini and no configuration at all.
 *
 * A relative value read FROM the ini is resolved the same way. That
 * is the part that matters in practice: a shortcut, a file manager
 * or a debugger can each set a different working directory, and a
 * game that only starts when launched from its own folder is a
 * classic and avoidable failure. An absolute value is left alone,
 * so a development ini can still point somewhere else entirely.
 */
#define DEFAULT_DATA_DIR ".\\DATA\\"

#define CLAMP_WIN_MIN_W 200
#define CLAMP_WIN_MIN_H 150

typedef enum {
	INI_TYPE_INT = 0,
	INI_TYPE_STRING
} INI_TYPE;

typedef struct {
	const char *section;
	const char *key;
	INI_TYPE    type;
	void       *ptr;      /* int*, or char* for STRING */
	int         str_max;  /* STRING only: bytes at ptr including NUL */
} INI_ENTRY;

/*
 * get_exe_dir - the directory the executable lives in, with a trailing
 * backslash. Built once from GetModuleFileName(NULL). Returns "" on
 * failure, which callers treat as "no prefix".
 */
const char *get_exe_dir(void)
{
	static char s_dir[MAX_PATH];
	static int  s_ready = 0;
	char *slash;
	DWORD n;

	if (s_ready)
		return s_dir;

	s_dir[0] = 0;
	n = GetModuleFileNameA(NULL, s_dir, MAX_PATH);
	if (n == 0 || n >= MAX_PATH) {
		s_dir[0] = 0;
		s_ready = 1;
		return s_dir;
	}

	slash = strrchr(s_dir, '\\');
	if (!slash)
		slash = strrchr(s_dir, '/');
	if (slash)
		slash[1] = 0;        /* keep the separator */
	else
		s_dir[0] = 0;

	s_ready = 1;
	return s_dir;
}

/*
 * is_absolute_path - Windows rules, and all three cases matter here.
 *
 *   C:\...      a drive-qualified path
 *   \\server\...  UNC
 *   \...        rooted on the current drive - not something we should
 *               second-guess by gluing the exe directory in front
 */
static int is_absolute_path(const char *p)
{
	if (!p || !p[0])
		return 0;
	if (p[0] == '\\' || p[0] == '/')
		return 1;
	if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) &&
	    p[1] == ':')
	    return 1;
	return 0;
}

/*
 * get_ini_file - path of "<exe_dir>\<exe_stem>.ini" for this process.
 * Built once from GetModuleFileName(NULL); later calls return the same
 * static MAX_PATH buffer. Returns "" on failure.
 */
/*
 * exe_path_with_ext - "<exe_dir>\<exe_stem>" plus the given
 * extension, or "" if it will not fit.
 *
 * Factored out when the log file needed the same rule as the ini.
 * Two callers wanting the same path with a different suffix is
 * exactly the point at which copying the function is the wrong move -
 * the ".exe"-stripping is fiddly enough to get subtly different in a
 * second copy.
 */
static const char *exe_path_with_ext(char *buf, const char *ext)
{
	char *dot;
	char *slash;
	DWORD n;

	buf[0] = 0;
	n = GetModuleFileNameA(NULL, buf, MAX_PATH);
	if (n == 0 || n >= MAX_PATH) {
		buf[0] = 0;
		return buf;
	}

	/* Prefer last '.' after the final path separator (strip ".exe"). */
	slash = strrchr(buf, '\\');
	if (!slash)
		slash = strrchr(buf, '/');
	dot = strrchr(buf, '.');
	if (dot && (!slash || dot > slash))
		*dot = 0;

	if (strlen(buf) + strlen(ext) < MAX_PATH)
		strcat(buf, ext);
	else
		buf[0] = 0;
	return buf;
}

const char *get_ini_file(void)
{
	static char s_ini[MAX_PATH];
	static int  s_ready = 0;

	if (!s_ready) {
		exe_path_with_ext(s_ini, ".ini");
		s_ready = 1;
	}
	return s_ini;
}

/* Strip trailing separators in place; returns the new length. */
static int trim_seps(char *s)
{
	int n = (int)strlen(s);

	while (n > 0 && (s[n - 1] == '\\' || s[n - 1] == '/'))
		s[--n] = 0;
	return n;
}

/*
 * ORIGIN  NONE - ours
 *
 * Create every component of a path, not just the last. 1 if the
 * directory exists afterwards.
 */
static int make_dirs(const char *path)
{
	char work[MAX_PATH];
	int  i;

	if (!path || !path[0])
		return 0;
	strncpy(work, path, MAX_PATH - 1);
	work[MAX_PATH - 1] = 0;
	if (trim_seps(work) == 0)
		return 0;

	for (i = 0; work[i]; i++) {
		char save;

		if (i == 0 || (work[i] != '\\' && work[i] != '/'))
			continue;
		if (i == 2 && work[1] == ':')
			continue;
		save    = work[i];
		work[i] = 0;
		if (!CreateDirectoryA(work, NULL) &&
		    GetLastError() != ERROR_ALREADY_EXISTS) {
			work[i] = save;
			return 0;
		}
		work[i] = save;
	}

	if (!CreateDirectoryA(work, NULL) &&
	    GetLastError() != ERROR_ALREADY_EXISTS)
	    return 0;
	return 1;
}

/*
 * WHERE THE LOG GOES, and by default that is BESIDE THE EXE - so a
 * copy of the game in its own folder writes its own log, which is
 * what makes two instances on one machine debuggable.
 *
 * [DEBUG] logpath overrides it. The whole path is created if it does
 * not exist, and if that fails the exe's folder is used rather than
 * losing the log.
 *
 * "" means "beside the exe"; get_log_file turns that into a path.
 */
const char *get_log_dir(void)
{
	static char s_dir[MAX_PATH];
	static int  s_ready = 0;

	if (!s_ready) {
		s_ready  = 1;
		s_dir[0] = 0;
		if (get_ini_string("DEBUG", "logpath", s_dir,
		                   (int)sizeof(s_dir))) {
			trim_seps(s_dir);
			if (s_dir[0] && !make_dirs(s_dir))
				s_dir[0] = 0;
		}
	}
	return s_dir;
}

const char *get_log_file(void)
{
	static char s_log[MAX_PATH];
	static int  s_ready = 0;

	if (!s_ready) {
		const char *dir = get_log_dir();

		exe_path_with_ext(s_log, ".log");
		if (dir && dir[0]) {
			/*
			 * THE EXE'S OWN STEM, not a literal "mracer" - a
			 * renamed exe keeps its log named after it in the
			 * override folder too.
			 */
			char        stem[MAX_PATH];
			const char *leaf;

			strncpy(stem, s_log, MAX_PATH - 1);
			stem[MAX_PATH - 1] = 0;
			leaf = strrchr(stem, '\\');
			if (!leaf)
				leaf = strrchr(stem, '/');
			leaf = leaf ? leaf + 1 : stem;
			_snprintf(s_log, MAX_PATH, "%s\\%s", dir, leaf);
		}
		s_log[MAX_PATH - 1] = 0;
		s_ready = 1;
	}
	return s_log;
}

/* 1 if get_ini_file() path exists as a normal file. */
static int does_ini_exist(void)
{
	const char *fname;
	DWORD attr;

	fname = get_ini_file();
	if (!fname || !fname[0])
		return 0;
	attr = GetFileAttributesA(fname);
	if (attr == INVALID_FILE_ATTRIBUTES)
		return 0;
	if ((attr & FILE_ATTRIBUTE_DIRECTORY) != 0)
		return 0;
	return 1;
}

/*
 * get_ini_value - read one integer key from the exe .ini.
 * Returns 1 if the file exists and the key was present; else 0.
 */
int get_ini_value(const char *section, const char *key, int *val)
{
	const char *fname;
	char buf[32];

	if (!section || !key || !val)
		return 0;
	if (!does_ini_exist())
		return 0;
	fname = get_ini_file();
	if (GetPrivateProfileStringA(section, key, "",
	        buf, (DWORD)sizeof(buf), fname) == 0)
	    return 0;
	*val = atoi(buf);
	return 1;
}

/*
 * write_ini_value - write one integer key to the exe .ini.
 * Creates or updates the file. Returns 1 on success, 0 on failure.
 */
int write_ini_string(const char *section, const char *key,
                     const char *val)
{
	const char *fname;

	if (!section || !key || !val)
		return 0;
	fname = get_ini_file();
	if (!fname || !fname[0])
		return 0;
	return WritePrivateProfileStringA(section, key, val, fname) ? 1 : 0;
}

int write_ini_value(const char *section, const char *key, int val)
{
	const char *fname;
	char buf[32];

	if (!section || !key)
		return 0;
	fname = get_ini_file();
	if (!fname || !fname[0])
		return 0;
	wsprintfA(buf, "%d", val);
	if (!WritePrivateProfileStringA(section, key, buf, fname))
		return 0;
	return 1;
}

/*
 * get_ini_string - read one string key into str[size].
 * Returns 1 if the file exists and the key was present; else 0.
 */
int get_ini_string(const char *section, const char *key, char *str,
                   int size)
{
	const char *fname;
	DWORD n;

	if (!section || !key || !str || size <= 1)
		return 0;
	if (!does_ini_exist())
		return 0;
	fname = get_ini_file();
	str[0] = 0;
	n = GetPrivateProfileStringA(section, key, "",
	        str, (DWORD)size, fname);
	return (n > 0) ? 1 : 0;
}

/*
 * read_ini_entries - load keys from fname into entry ptrs.
 * Returns 1 if any key was present and written; skips NULL ptrs.
 */
static int read_ini_entries(const char *fname, INI_ENTRY *ents, int nent)
{
	char buf[256];
	int i;
	int got = 0;

	if (!fname || !fname[0] || !ents || nent <= 0)
		return 0;

	for (i = 0; i < nent; i++) {
		INI_ENTRY *e = &ents[i];
		DWORD n;

		if (!e->ptr || !e->section || !e->key)
			continue;
		n = GetPrivateProfileStringA(e->section, e->key, "",
		        buf, (DWORD)sizeof(buf), fname);
		if (n == 0)
			continue;
		if (e->type == INI_TYPE_INT) {
			*(int *)e->ptr = atoi(buf);
			got = 1;
		} else if (e->type == INI_TYPE_STRING) {
			if (e->str_max > 0) {
				strncpy((char *)e->ptr, buf,
				        (size_t)(e->str_max - 1));
				((char *)e->ptr)[e->str_max - 1] = 0;
				got = 1;
			}
		}
	}
	return got;
}

/*
 * write_ini_entries - store entry ptrs into fname section/keys.
 * Returns 1 if any key was written successfully; skips NULL ptrs.
 */
static int write_ini_entries(const char *fname, INI_ENTRY *ents,
                             int nent)
{
	char buf[256];
	int i;
	int got = 0;

	if (!fname || !fname[0] || !ents || nent <= 0)
		return 0;

	for (i = 0; i < nent; i++) {
		INI_ENTRY *e = &ents[i];
		const char *val;

		if (!e->ptr || !e->section || !e->key)
			continue;
		if (e->type == INI_TYPE_INT) {
			wsprintfA(buf, "%d", *(int *)e->ptr);
			val = buf;
		} else if (e->type == INI_TYPE_STRING) {
			val = (const char *)e->ptr;
		} else {
			continue;
		}
		if (WritePrivateProfileStringA(e->section, e->key, val, fname))
			got = 1;
	}
	return got;
}

/*
 * clamp_win_rect_to_monitor - snap x/y/w/h onto the nearest monitor
 * work area. Tiny or negative sizes become at least CLAMP_WIN_MIN_W x
 * CLAMP_WIN_MIN_H; size is capped to the work area. If any edge is
 * outside by one or more pixels, slide onto that edge.
 *
 * Call after loading positions from the ini: the saved rect may name a
 * monitor that has since been unplugged or moved.
 */
void clamp_win_rect_to_monitor(int *x, int *y, int *w, int *h)
{
	RECT rc;
	RECT work;
	HMONITOR mon;
	MONITORINFO mi;
	int ww;
	int wh;

	if (!x || !y || !w || !h)
		return;

	if (*w < CLAMP_WIN_MIN_W)
		*w = CLAMP_WIN_MIN_W;
	if (*h < CLAMP_WIN_MIN_H)
		*h = CLAMP_WIN_MIN_H;

	rc.left   = *x;
	rc.top    = *y;
	rc.right  = *x + *w;
	rc.bottom = *y + *h;

	mon = MonitorFromRect(&rc, MONITOR_DEFAULTTONEAREST);
	memset(&mi, 0, sizeof(mi));
	mi.cbSize = sizeof(mi);
	if (!GetMonitorInfoA(mon, &mi))
		return;

	work = mi.rcWork;
	ww = work.right - work.left;
	wh = work.bottom - work.top;
	if (ww <= 0 || wh <= 0)
		return;

	if (*w > ww)
		*w = ww;
	if (*h > wh)
		*h = wh;

	if (*x < work.left)
		*x = work.left;
	if (*y < work.top)
		*y = work.top;
	if (*x + *w > work.right)
		*x = work.right - *w;
	if (*y + *h > work.bottom)
		*y = work.bottom - *h;

	/* Full-size rect can undershoot after the opposite-edge slide. */
	if (*x < work.left)
		*x = work.left;
	if (*y < work.top)
		*y = work.top;
}

/*
 * get_console_pos_ini - read CONSOLE x/y/w/h from the exe .ini.
 * Returns 1 if at least one key was present and written through its
 * pointer; 0 if no ini, or none of the keys exist.
 */
int get_console_pos_ini(int *x, int *y, int *w, int *h)
{
	INI_ENTRY ents[4] = {
		{ CONSOLE_SECTION_NAME, "x", INI_TYPE_INT, NULL, 0 },
		{ CONSOLE_SECTION_NAME, "y", INI_TYPE_INT, NULL, 0 },
		{ CONSOLE_SECTION_NAME, "w", INI_TYPE_INT, NULL, 0 },
		{ CONSOLE_SECTION_NAME, "h", INI_TYPE_INT, NULL, 0 }
	};

	ents[0].ptr = x;
	ents[1].ptr = y;
	ents[2].ptr = w;
	ents[3].ptr = h;

	if (!does_ini_exist())
		return 0;
	return read_ini_entries(get_ini_file(), ents, 4);
}

/*
 * save_console_pos_ini - write CONSOLE x/y/w/h to the exe .ini.
 * Creates the file if needed. Returns 1 if any key write succeeded.
 */
int save_console_pos_ini(int x, int y, int w, int h)
{
	const char *fname;
	INI_ENTRY ents[4] = {
		{ CONSOLE_SECTION_NAME, "x", INI_TYPE_INT, NULL, 0 },
		{ CONSOLE_SECTION_NAME, "y", INI_TYPE_INT, NULL, 0 },
		{ CONSOLE_SECTION_NAME, "w", INI_TYPE_INT, NULL, 0 },
		{ CONSOLE_SECTION_NAME, "h", INI_TYPE_INT, NULL, 0 }
	};

	ents[0].ptr = &x;
	ents[1].ptr = &y;
	ents[2].ptr = &w;
	ents[3].ptr = &h;

	fname = get_ini_file();
	if (!fname || !fname[0])
		return 0;
	return write_ini_entries(fname, ents, 4);
}

/*
 * get_win_pos_ini - read WINDOW x/y/w/h from the exe .ini.
 * Same return rules as get_console_pos_ini.
 */
int get_win_pos_ini(int *x, int *y, int *w, int *h)
{
	INI_ENTRY ents[4] = {
		{ WINDOW_SECTION_NAME, "x", INI_TYPE_INT, NULL, 0 },
		{ WINDOW_SECTION_NAME, "y", INI_TYPE_INT, NULL, 0 },
		{ WINDOW_SECTION_NAME, "w", INI_TYPE_INT, NULL, 0 },
		{ WINDOW_SECTION_NAME, "h", INI_TYPE_INT, NULL, 0 }
	};

	ents[0].ptr = x;
	ents[1].ptr = y;
	ents[2].ptr = w;
	ents[3].ptr = h;

	if (!does_ini_exist())
		return 0;
	return read_ini_entries(get_ini_file(), ents, 4);
}

/*
 * save_win_pos_ini - write WINDOW x/y/w/h to the exe .ini.
 * Creates the file if needed. Returns 1 if any key write succeeded.
 */
int save_win_pos_ini(int x, int y, int w, int h)
{
	const char *fname;
	INI_ENTRY ents[4] = {
		{ WINDOW_SECTION_NAME, "x", INI_TYPE_INT, NULL, 0 },
		{ WINDOW_SECTION_NAME, "y", INI_TYPE_INT, NULL, 0 },
		{ WINDOW_SECTION_NAME, "w", INI_TYPE_INT, NULL, 0 },
		{ WINDOW_SECTION_NAME, "h", INI_TYPE_INT, NULL, 0 }
	};

	ents[0].ptr = &x;
	ents[1].ptr = &y;
	ents[2].ptr = &w;
	ents[3].ptr = &h;

	fname = get_ini_file();
	if (!fname || !fname[0])
		return 0;
	return write_ini_entries(fname, ents, 4);
}

/*
 * get_data_dir - [PATHS] datadir, with a trailing backslash.
 *
 * Resolved once. A missing or empty key falls back to
 * DEFAULT_DATA_DIR and writes it back, so the first run leaves a key
 * in the ini that can be pointed somewhere else.
 *
 * A RELATIVE VALUE - from the ini or from the default - is resolved
 * against the EXECUTABLE'S directory, never the working directory.
 */
const char *get_data_dir(void)
{
	static char s_dir[MAX_PATH];
	static int  s_ready = 0;
	size_t n;

	if (s_ready)
		return s_dir;

	s_dir[0] = 0;
	if (!get_ini_string(PATHS_SECTION_NAME, "datadir", s_dir,
	                    (int)sizeof(s_dir)) || s_dir[0] == 0) {
		strncpy(s_dir, DEFAULT_DATA_DIR, sizeof(s_dir) - 1);
		s_dir[sizeof(s_dir) - 1] = 0;
		WritePrivateProfileStringA(PATHS_SECTION_NAME, "datadir",
		                           s_dir, get_ini_file());
	}

	/*
	 * A RELATIVE VALUE IS RESOLVED AGAINST THE EXECUTABLE, not the
	 * working directory - see DEFAULT_DATA_DIR above. A leading ".\\"
	 * is stripped first so the joined path stays readable in the
	 * console line that prints it.
	 */
	if (!is_absolute_path(s_dir)) {
		const char *exe = get_exe_dir();
		const char *rel = s_dir;
		char        joined[MAX_PATH];

		while ((rel[0] == '.' && (rel[1] == '\\' || rel[1] == '/')))
			rel += 2;

		if (exe[0] && strlen(exe) + strlen(rel) < sizeof(joined)) {
			strcpy(joined, exe);
			strcat(joined, rel);
			strncpy(s_dir, joined, sizeof(s_dir) - 1);
			s_dir[sizeof(s_dir) - 1] = 0;
		}
	}

	/* Guarantee the trailing separator so callers can just concatenate. */
	n = strlen(s_dir);
	if (n > 0 && s_dir[n - 1] != '\\' && s_dir[n - 1] != '/' &&
	    n + 1 < sizeof(s_dir)) {
		s_dir[n]     = '\\';
		s_dir[n + 1] = 0;
	}

	s_ready = 1;
	return s_dir;
}

/*
 * data_path - join a relative path onto the data directory.
 * Returns 1 on success, 0 if it would not fit in out[size].
 */
int data_path(char *out, int size, const char *relative)
{
	const char *dir;
	size_t need;

	if (!out || size <= 0)
		return 0;
	out[0] = 0;
	if (!relative)
		return 0;

	dir = get_data_dir();

	/* Do not let a leading separator on the relative part turn the
	 * result into an absolute path. */
	while (*relative == '\\' || *relative == '/')
		relative++;

	need = strlen(dir) + strlen(relative) + 1;
	if (need > (size_t)size)
		return 0;

	strcpy(out, dir);
	strcat(out, relative);
	return 1;
}

