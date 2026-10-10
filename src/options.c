/*
 * options.c - [OPTIONS] in mracer.ini.
 *
 * See options.h for what is stored and what deliberately is not.
 */

#include <windows.h>

#include "options.h"
#include "framebuffer.h"
#include "menures.h"
#include <string.h>
#include "config.h"
#include "audio.h"
#include "log.h"

/*
 * TWO SECTIONS, AND THE SPLIT IS "IS THIS THE GAME'S SETTING OR
 * OURS".
 *
 * [OPTIONS] holds keys the original has - the CONFIG.BPT items of
 * subsystems/config-file.txt section 2, under the original's own
 * names, so the two files can be compared without a translation
 * table. Some of them have no row on the options page and are
 * driven by an in-race hotkey instead (ShowCounter, CounterType,
 * SmallFonts); they are still the GAME'S settings and belong here.
 *
 * [OPTIONS_CUSTOM] holds the ones this project invented. Keeping
 * them apart means a reader can tell at a glance which half of the
 * file is evidence about MotoRacer and which half is ours.
 */
#define OPT_SECTION        "OPTIONS"
#define OPT_SECTION_CUSTOM "OPTIONS_CUSTOM"

/* Short aliases, so the table rows fit inside 80 columns. */
#define SEC OPT_SECTION
#define OWN OPT_SECTION_CUSTOM

MR_Options g_options;

/*
 * The eight wave formats, in the order FUN_00409570 visits them. See
 * the note in options.h: it is a three-digit odometer whose carries
 * are gated on the device, and the gating is what we cannot have.
 */
static const MR_WaveFormat kWaveFormat[OPT_WAVE_FORMATS] = {
	{  8, 11025, 0 },
	{ 16, 11025, 0 },
	{  8, 22050, 1 },
	{ 16, 22050, 1 },
	{  8, 11025, 1 },
	{ 16, 11025, 1 },
	{  8, 22050, 0 },
	{ 16, 22050, 0 }
};

const MR_WaveFormat *options_wave_format(int index)
{
	if (index < 0 || index >= OPT_WAVE_FORMATS)
		return NULL;
	return &kWaveFormat[index];
}

/*
 * ONE TABLE, THE WAY Settings_LoadAll IS ONE FUNCTION. Every row is
 * key, where it lands, the range it is clamped to, and the default.
 *
 * o THE DEFAULTS ARE THE ORIGINAL'S where CONFIG.BPT records one -
 *   subsystems/config-file.txt section 2 lists the shipped values,
 *   and the three volumes default to 32, full [verified].
 *
 * o CLAMPING RATHER THAN REJECTING is deliberate. The file is meant
 *   to be hand-edited, and an out-of-range index into kWaveFormat or
 *   kP11Mode would read off the end of a table. Clamping makes a
 *   typo harmless and visible instead of fatal and silent.
 */
typedef struct {
	const char *section;
	const char *key;
	int        *field;
	int         lo;
	int         hi;
	int         def;
} OPT_ENTRY;

static OPT_ENTRY *opt_table(int *countOut)
{
	static OPT_ENTRY table[] = {
	/* section         key                  field                 lo hi def */
	{ SEC, "CurrentDifficulty", &g_options.difficulty,      0,  2, 1 },
	{ SEC, "Controller",        &g_options.controller,      0,  2, 0 },
	{ SEC, "CurrentDetail",     &g_options.detail,          0,  4, 4 },
	{ SEC, "NbComputerCut",     &g_options.opponents,       0,  1, 1 },
	{ SEC, "VoicesState",       &g_options.voices,          0,  1, 1 },
	{ SEC, "SoundState",        &g_options.sounds,          0,  1, 1 },
	{ SEC, "AudioState",        &g_options.music,           0,  1, 1 },
	{ SEC, "ScreenMode",        &g_options.screenMode,      0,
			                    OPT_SCREEN_MODES - 1,           2 },
	{ SEC, "Mode_169",          &g_options.wide169,         0,  1, 1 },
	{ SEC, "ShowCounter",       &g_options.showCounter,     0,  1, 1 },
	{ SEC, "CounterType",       &g_options.counterType,     0,  2, 1 },
	{ SEC, "SpeedUnit",         &g_options.speedUnit,       0,  1, 0 },
	{ SEC, "SmallFonts",        &g_options.smallFonts,      0,  1, 1 },
	/*
	 * THE ORIGINAL'S KEY IS D3DOnlyEmulation AND WE DO NOT USE
	 * IT.  That setting is tri-state - 0 hardware point, 2
	 * hardware bilinear, anything else the SOFTWARE rasterizer
	 * - and its third state does not exist here.  The name is
	 * also wrong for what it does; renderer-mode.txt 1.1 says
	 * so.  Nothing reads CONFIG.BPT for options, so there is no
	 * round trip to preserve and no reason to inherit a
	 * misleading name for one bit of it.
	 */
	{ SEC, "TextureFilter",     &g_options.textureFilter,   0,  1, 0 },
	/*
	 * THE MAP, cycled by F9. Five values, not four - 4 is off, and
	 * the original cycles (mode + 1) %% 5 at 0x004321d7.
	 */
	{ SEC, "MapMode",           &g_options.mapMode,         0,  4, 4 },
	{ SEC, "WaveFormat",        &g_options.waveFormat,      0,
			                    OPT_WAVE_FORMATS - 1,           3 },
	{ SEC, "EnumSession",       &g_options.sessionRefresh,  0, 30, 0 },

	/* Ours. See the note on OPT_SECTION_CUSTOM. */
	{ OWN, "Language",          &g_options.language,        0,
			                    MENURES_LANG_COUNT - 1,         0 },
	{ OWN, "Wide169Crop",       &g_options.wide169Crop,     0,  1, 0 },
	/*
	 * A DEBUG OVERRIDE, off by default. Set it and the grid
	 * countdown lasts no time at all, which is a testing
	 * convenience only - see race_clock_reset for why it does
	 * not change what the race does.
	 */
	{ OWN, "SkipCountdown",     &g_options.skipCountdown,   0,  1, 0 },
	{ OWN, "ShowFps",           &g_options.showFps,         0,  1, 0 },
	/*
	 * ANOTHER DEBUG OVERRIDE, and -1 - the default - keeps the
	 * built-in. The value is the CEILING in MILLISECONDS, so 4000
	 * is what the game already does; the normal remount lands
	 * CRASH_HOLD_MS earlier, a whole second before it. 0 is
	 * instant. See racer_begin_crash.
	 */
	{ OWN, "CrashRecoveryMs",   &g_options.crashRecoveryMs,
			                                               -1,
			                                            10000, -1 },
	/*
	 * AND A THIRD DEBUG OVERRIDE, off by default. Set it and the
	 * race timer stops counting down, so the limit never expires
	 * and the player is never locked to the brake. The GRID
	 * countdown still runs - see race_clock_tick for why that is
	 * not automatic.
	 */
	{ OWN, "FreezeRaceTimer",   &g_options.freezeRaceTimer, 0,  1, 0 }
	};

	*countOut = (int)(sizeof(table) / sizeof(table[0]));
	return table;
}

static int clamp_to(const OPT_ENTRY *e, int v)
{
	if (v < e->lo)
		return e->lo;
	if (v > e->hi)
		return e->hi;
	return v;
}

/*
 * The three volumes, which are audio.c's and are only passed through
 * here. Key names are the original's; the range is 0..32, the same
 * integer CONFIG.BPT holds and Settings_LoadAll hands to
 * Mixer_SetVolume [verified, 0x0042b2d5].
 */
static const struct {
	const char *key;
	int         channel;
} kVolume[] = {
	{ "MasterVolume", AUDIO_VOL_MASTER },
	{ "CDVolume",     AUDIO_VOL_CD     },
	{ "WaveVolume",   AUDIO_VOL_WAVE   }
};

#define OPT_VOLUMES ((int)(sizeof(kVolume) / sizeof(kVolume[0])))

void options_load(void)
{
	OPT_ENTRY *table;
	int        n;
	int        i;

	table = opt_table(&n);

	for (i = 0; i < n; i++) {
		int v;

		if (get_ini_value(table[i].section, table[i].key, &v))
			*table[i].field = clamp_to(&table[i], v);
		else
			*table[i].field = table[i].def;
	}

	for (i = 0; i < OPT_VOLUMES; i++) {
		int v;

		/* audio_set_volume clamps to 0..AUDIO_VOL_MAX itself. */
		if (get_ini_value(OPT_SECTION, kVolume[i].key, &v))
			audio_set_volume(kVolume[i].channel, v);
	}

	mrlog("options: difficulty %d, detail %d, wave format %d, "
	      "volumes %d/%d/%d\n",
	      g_options.difficulty, g_options.detail, g_options.waveFormat,
	      audio_volume(AUDIO_VOL_MASTER), audio_volume(AUDIO_VOL_CD),
	      audio_volume(AUDIO_VOL_WAVE));
}

void options_save(void)
{
	OPT_ENTRY *table;
	int        n;
	int        i;

	table = opt_table(&n);

	for (i = 0; i < n; i++)
		write_ini_value(table[i].section, table[i].key,
	                    *table[i].field);

	for (i = 0; i < OPT_VOLUMES; i++)
		write_ini_value(OPT_SECTION, kVolume[i].key,
	                    audio_volume(kVolume[i].channel));
}

int g_videoSettingsDirty;

void options_frame_size(int *w, int *h)
{
	int fw = FB_DESIGN_W;
	int fh = FB_DESIGN_H;

	if (g_options.wide169) {
		if (g_options.wide169Crop) {
			/*
			 * ORIGIN  Screen3D_SetViewport @ 0x00499ba0, the
			 *         widescreen branch at 0x00499cb5
			 *
			 * The original computes 9*W/16 scaled by (4/3)
			 * over the mode's aspect, in which the width
			 * cancels: it is height * 3 / 4 for any mode.
			 * Width is not touched.
			 */
			fh = FB_DESIGN_H * 3 / 4;
		} else {
			/*
			 * 16:9 of the SAME height, rounded to an even width
			 * so the DIB stride needs no padding. 480*16/9 is
			 * 853.33; 854 is the usual rounding and is what the
			 * letterbox maths is happiest with.
			 */
			fw = 854;
		}
	}

	if (w) *w = fw;
	if (h) *h = fh;
}
