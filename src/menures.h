/*
 * menures.h - the menu resource manifest.
 *
 * The game names its menu sprites by NUMBER, not by path. Sprite_Load
 * (0x0043e8e0) takes a resource id, looks it up in a 209-entry table,
 * and builds the path from a per-entry localisation flag plus the file
 * name. The table itself is static at 0x004e3e20 and is expanded into
 * runtime descriptors by Lang_SetLanguage.
 *
 * The whole thing is transcribed here, ids 0 to 208. It is written up
 * in docs/subsystems/menu-resources.txt, including what the three
 * flags mean - which took a while to settle and where two earlier
 * guesses were wrong.
 */

#ifndef MRACER_MENURES_H
#define MRACER_MENURES_H

#ifdef __cplusplus
extern "C" {
#endif

#define MENURES_COUNT 209

typedef struct {
	int         id;
	const char *name;
	int         preload;    /* Menu_LoadResources creates it at init */
	int         localised;  /* path prefix is the per-language dir   */
	int         kind;       /* 0..3 blit mode, 4 the wave variant    */
} MENURES_ENTRY;

/* The entry for an id, or NULL if the id is out of range. Ids are
 * dense from 0 to 208, so this is a direct index. */
const MENURES_ENTRY *menures_get(int id);

/*
 * THE SIX LANGUAGES. Sprite_Load picks the localised prefix out of a
 * six-entry table at 0x004e8b60 indexed by g_language (0x004dbce8),
 * which the original sets from the command line - -us -fr -gr -it -sp
 * -sw, documented in the flag block at 0x004dc664 - and defaults to 0,
 * English. It is NOT one of the 36 settings in CONFIG.BPT.
 *
 * The port stores it as [OPTIONS] Language because it has no command
 * line to speak of; that key is ours, not the original's.
 */
#define MENURES_LANG_COUNT 6

/* "MenuUs" .. "MenuSw", the original's own names, for logging.
 * "?" if lang is out of range. */
const char *menures_lang_name(int lang);

/*
 * menures_path - resolve a resource id to a LOGICAL ASSET NAME.
 *
 * A localised resource comes from the current language's directory,
 * everything else from the shared "MENU\BITMAP\\". Neither is a
 * setting any more; see menures.c.
 *
 * Returns 1 on success, 0 for a bad id or if the name would not fit.
 */
int menures_path(int id, char *out, int size);

/*
 * menures_path_us - the same resource from the ENGLISH directory.
 *
 * The original's per-file fallback: a localised sprite that will not
 * open is retried against MenuUs, so a partly translated release
 * loses one sprite at a time rather than failing outright. Returns 0
 * when there is nothing to fall back to - a shared resource, or a
 * language that is already English.
 */
int menures_path_us(int id, char *out, int size);

#ifdef __cplusplus
}
#endif

#endif /* MRACER_MENURES_H */
