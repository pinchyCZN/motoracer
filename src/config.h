/*
 * config.h - INI settings stored beside the executable.
 *
 * The file is <exe_dir>\<exe_stem>.ini, so mracer.exe reads and writes
 * mracer.ini in its own directory. Nothing here touches the registry;
 * the original game used both, and we are keeping to the .ini half.
 */

#ifndef MRACER_CONFIG_H
#define MRACER_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

/* Path of "<exe_dir>\<exe_stem>.ini". Static buffer, "" on failure. */
const char *get_ini_file(void);

/*
 * Path of "<exe_dir>\<exe_stem>.log". Same rule as the ini, and it
 * shares the implementation - see exe_path_with_ext.
 */
/*
 * THE LOG GOES BESIDE THE EXE unless [DEBUG] logpath says otherwise -
 * so a second copy of the game in its own folder writes its own log,
 * which is what makes two instances on one machine debuggable.
 *
 * "" means "beside the exe". A logpath whose folders cannot be
 * created falls back to that rather than losing the log.
 */
const char *get_log_dir(void);

const char *get_log_file(void);

/* Generic accessors, for settings beyond window placement.
 * The get_ forms return 1 when the file exists and the key was
 * present, 0 otherwise, and leave the caller's value untouched on 0. */
int get_ini_value(const char *section, const char *key, int *val);
int write_ini_value(const char *section, const char *key, int val);
int get_ini_string(const char *section, const char *key, char *str,
                   int size);

/*
 * write_ini_string - write one string key to the exe .ini.
 *
 * THE REGISTRY IS NOT USED AND WILL NOT BE. The original keeps player
 * data under HKEY_LOCAL_MACHINE; this project keeps everything in the
 * .ini beside the exe, which is inspectable, portable, and does not
 * need an installer or elevation to work. Recorded as a deliberate
 * divergence rather than an omission.
 */
int write_ini_string(const char *section, const char *key,
                     const char *val);

/* Snap x/y/w/h onto the nearest monitor's work area. Call after
 * loading a saved rect - the monitor layout may have changed since. */
void clamp_win_rect_to_monitor(int *x, int *y, int *w, int *h);

/* [WINDOW] and [CONSOLE] x/y/w/h. The get_ forms return 1 if at least
 * one key was present; NULL pointers are skipped. Rects are window
 * rects in screen coordinates, not client rects. */
int get_win_pos_ini(int *x, int *y, int *w, int *h);
int save_win_pos_ini(int x, int y, int w, int h);
int get_console_pos_ini(int *x, int *y, int *w, int *h);
int save_console_pos_ini(int x, int y, int w, int h);

/*
 * get_exe_dir - the directory this executable lives in, with a
 * trailing backslash. "" if it cannot be determined.
 */
const char *get_exe_dir(void);

/*
 * [PATHS] datadir - where the game's DATA tree lives.
 *
 * Defaults to ".\\DATA\\" RELATIVE TO THE EXECUTABLE, so dropping
 * mracer.exe into a MotoRacer install works with no ini and no
 * configuration. A relative value read from the ini is resolved the
 * same way; an absolute one is left alone.
 *
 * get_data_dir() returns it with a trailing backslash guaranteed. If
 * the key is absent the built-in default is used AND written back, so
 * the key shows up in the ini after the first run and can be edited
 * without knowing it existed.
 *
 * data_path() joins a relative path onto it, e.g.
 *     data_path(buf, sizeof(buf), "ILE01.BKF")
 *
 * It is for LOOSE files only now. Game assets go through
 * asset_load() in assets.h, which looks inside the .BKF archives
 * first - CLAUDE.md rule 10.
 * Returns 1 on success, 0 if the result would not fit.
 */
const char *get_data_dir(void);
int data_path(char *out, int size, const char *relative);

/*
 * THE MENU'S ASSET PREFIXES ARE NOT SETTINGS - they were, and
 * should not have been. The shared one is a literal in the
 * original ("data\\menu\\bitmap\\" at 0x004e8ca0) and the
 * localised one is chosen from a six-entry language table, so
 * neither is a free-form path. Both live in menures.c now, and
 * the language index is [OPTIONS] Language. See menures.h.
 *
 * The selected track's directory is not a setting either: it
 * comes from the track's own LIST.BPT record at +0x24, per
 * island. See track_asset_name.
 */

#ifdef __cplusplus
}
#endif

#endif /* MRACER_CONFIG_H */
