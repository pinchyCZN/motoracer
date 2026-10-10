/*
 * options.h - the [OPTIONS] settings, and the page that edits them.
 *
 * The analogue of Settings_LoadAll (0x0042ae30), which reads 36 named
 * integers out of CONFIG.BPT and distributes them across the config
 * globals. We read the same settings out of mracer.ini instead, under
 * one [OPTIONS] section, and keep the original's key names wherever
 * one exists - CurrentDifficulty, NbComputerCut, Mode_169 - so the two
 * can be compared without a translation table.
 *
 * EVERY VALUE IS AN INTEGER, and where the original stored something
 * else we store an INDEX. That is a project decision, not the
 * original's: it keeps the file hand-editable, keeps parsing to atoi,
 * and means a corrupt value can always be clamped into range rather
 * than rejected.
 *
 * THREE SETTINGS ARE NOT HERE:
 *
 *   the three VOLUMES, which live in audio.c because that is what
 *   applies them - options_load pushes them in and options_save reads
 *   them back out, so there is exactly one copy;
 *
 *   D3D, because the port has one renderer and it is always on;
 *
 *   InputDevice, which the original stores as 1, 4 or 2. We store the
 *   BINDING COLUMN 0..2 instead, under "Controller"; the map is
 *   0 -> 1 keyboard, 1 -> 4 joystick, 2 -> 2 joypad [verified,
 *   MenuPage11_Options_Enter 0x00443040].
 */

#ifndef MRACER_OPTIONS_H
#define MRACER_OPTIONS_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * THE WAVE OUTPUT ROW IS ONE INDEX over eight combinations, and the
 * ORDER is the order FUN_00409570 cycles them [inferred, read out of
 * that function - it is a three-digit odometer]:
 *
 *   toggle the sample depth 8 <-> 16; going back to 8 carries;
 *   the carry toggles the rate 11025 <-> 22050; going UP to 22050
 *   carries again;
 *   that carry toggles mono <-> stereo.
 *
 * o WE CANNOT REPRODUCE THE GATING. Every one of those steps is
 *   conditional on the sound device reporting that it supports the
 *   next format, so on real hardware the cycle skips entries. With no
 *   audio back end there is nothing to ask, so all eight are offered.
 *
 * o AND THE INDEX IS OURS. The original stores the three fields
 *   separately as Wave16Bits, Stereo and ReplayRate. One index is
 *   what makes the row an integer like every other row; the three
 *   fields are recoverable from it exactly.
 */
#define OPT_WAVE_FORMATS 8

typedef struct {
	int bits;         /* 8 or 16                        */
	int rateHz;       /* 11025 or 22050                 */
	int stereo;       /* 0 mono, 1 stereo               */
} MR_WaveFormat;

/* NULL if index is out of range. */
const MR_WaveFormat *options_wave_format(int index);

/*
 * The three screen modes the original supports, as an index. 0 is
 * 320x240, 1 is 512x384, 2 is 640x480 - the decode Settings_LoadAll
 * applies to ScreenMode, and any other value is coerced to 2
 * [verified, subsystems/settings.txt section 3].
 *
 * NOTHING APPLIES THIS YET. The reconstruction renders at 640x480 and
 * letterboxes; the setting is stored so the resolution work has
 * somewhere to read from, and the row says so when it is changed.
 *
 * Mode_169 IS applied - see options_frame_size below. The two are
 * independent in the original too: the widescreen flag scales the
 * surface height and leaves the mode alone.
 */
#define OPT_SCREEN_MODES 3

typedef struct {
	int difficulty;      /* CurrentDifficulty  0..2                  */
	int controller;      /* Controller         0..2, binding column  */
	int detail;          /* CurrentDetail      0..4, shown as 1..5   */
	int opponents;       /* NbComputerCut      0 = 24, 1 = 12        */
	int voices;          /* VoicesState        0/1                   */
	int sounds;          /* SoundState         0/1                   */
	int music;           /* AudioState         0/1, the CD           */
	int screenMode;      /* ScreenMode         0..2                  */
	int wide169;         /* Mode_169           0/1                   */
	int wide169Crop;     /* Wide169Crop        0/1, ours         */
	int skipCountdown;   /* SkipCountdown      0/1, ours, DEBUG  */
	int showFps;         /* ShowFps            0/1, ours, DEBUG  */
	int crashRecoveryMs; /* CrashRecoveryMs    -1 = built-in,    */
	                     /*                    ours, DEBUG       */
	int freezeRaceTimer; /* FreezeRaceTimer    0/1, ours, DEBUG  */

	int showCounter;     /* ShowCounter        0/1               */
	int counterType;     /* CounterType        0..2              */
	int speedUnit;       /* SpeedUnit          0 km/h, 1 mph     */
	int waveFormat;      /* WaveFormat         0..7                  */
	int sessionRefresh;  /* EnumSession        0..30, 0 = manual     */

	/*
	 * THE MENU LANGUAGE, 0..5, and this key is OURS. The original
	 * has no stored setting for it: g_language (0x004dbce8) starts
	 * at 0 and is only ever written by the command line - -us -fr
	 * -gr -it -sp -sw, in the flag block at 0x004dc664. The port
	 * has no command line worth the name, so it is a setting here.
	 * The ORDER is the original's, from the table at 0x004e8b60.
	 * See menures.h.
	 */
	int language;        /* Language           0..5, 0 = English     */

	/*
	 * THE SMALL-FONT SWITCH, and this one IS the original's.
	 * SmallFonts is one of the 36 settings in CONFIG.BPT and it
	 * drives g_cfgSmallFonts (0x004f0e08). Ctrl+F toggles it in
	 * a race.
	 *
	 * It does TWO things, both in the in-race text path: Font_Get
	 * answers a request for a HiRes font with its LowRes twin,
	 * and TextWidget_Layout halves the authored coordinates. On
	 * the escape menu that is GAMEMENU.FNT at 9 pixels instead of
	 * 18, with the rows 11 apart instead of 22.
	 *
	 * DEFAULT 0 - the large fonts. The original defaults it the
	 * same way; the MOTO this was measured against had it on.
	 */
	int mapMode;         /* MapMode            0..4, F9 cycles  */
	int smallFonts;      /* SmallFonts         0/1, 0 = large      */
	int textureFilter;   /* TextureFilter      0/1, ours        */
} MR_Options;

extern MR_Options g_options;

/*
 * options_load - defaults, then whatever [OPTIONS] holds, then clamp.
 *
 * Call it once at startup, BEFORE audio_init needs a volume and
 * before the first page runs. A key that is absent or unreadable
 * leaves the default; a key that is out of range is clamped rather
 * than rejected, so a hand-edited ini cannot index a table off its
 * end.
 */
void options_load(void);

/*
 * options_save - write the whole section back.
 *
 * Called when the options page is left, which covers ACCEPT, escape
 * and any transition out of it. Not called on a crash or a kill, and
 * that is accepted: these are preferences, not progress.
 */
void options_save(void);

/*
 * options_frame_size - the back buffer the settings ask for.
 *
 * THE ORIGINAL'S 16:9 IS A CROP, NOT A WIDENING, and that is worth
 * stating because it is the opposite of what the option sounds like.
 * With Mode_169 set it scales the surface to 640x360 and splits the
 * 120 lost scanlines into equal top and bottom margins; the
 * projection is not touched at all, and cannot be - the lens is
 * looked up by PIXEL COUNT out of a table of enumerated 4:3 modes,
 * and 640x360 is in neither. So the shipped option shows LESS of the
 * world vertically and no more of it horizontally. See
 * docs/subsystems/projection.txt 5.2 and 5.4.
 *
 *     Mode_169=0                 640 x 480    4:3
 *     Mode_169=1                 854 x 480    16:9, Hor+   DEFAULT
 *     Mode_169=1 Wide169Crop=1   640 x 360    16:9, the original's
 *
 * 854x480 IS A DEVIATION AND A DELIBERATE ONE. The port's projection
 * is square-pixel - every site sets g_projBaseX and g_projBaseY to
 * the same number - so widening the frame opens the horizontal field
 * from 67.4 to 83.3 degrees and leaves the vertical at 53.1. That is
 * Hor+, which is the right convention for a racer and is what
 * projection.txt 7 has recommended since before this was built.
 * Wide169Crop is there so the original's behaviour can still be had.
 *
 * APPLIED WHEN g_videoSettingsDirty SAYS SO, not only at startup.
 */
void options_frame_size(int *w, int *h);

/*
 * g_videoSettingsDirty - "the display settings moved; re-derive the
 * frame from them".
 *
 * ORIGIN  the global at 0x004e9ce4, twenty-one writers and three
 *         readers.  maybe_Game_RenderFrame's last act is
 *
 *             if (g_videoSettingsDirty)
 *                 maybe_Screen3D_UpdateScreen(...)
 *
 *         at 0x00480496, which re-runs Screen3D_SetViewport against
 *         the current settings.  Menu_ToggleWidescreen (0x0043a370)
 *         is two instructions: raise this, then flip
 *         g_cfgWidescreen169.  F4 does the same inline at
 *         0x00431fd2.  See subsystems/mode-change.txt.
 *
 * WHICH MEANS THE ORIGINAL HAS AN APPLY-NOW PATH and this is it.  An
 * earlier note here claimed display settings were deferred to a
 * restart; that came from an experiment that watched Screen3D_Open
 * and the two projection bases, and 16:9 touches NEITHER - it changes
 * the VIEWPORT.  The experiment could not have seen it.  Corrected in
 * camera.txt 1.18.
 *
 * OURS, THOUGH: the out-of-race options page raising it.  The
 * original's page 11 edits a SAVED COPY of the flag (0x00504e80) and
 * the live one is not restored until the menu is left, because
 * Menu_SetPage forces the whole menu to 640x480 4:3 on the way in.
 * We have no such save/restore, so the page toggles the real setting
 * and asks for it to be applied - which is the behaviour a player
 * expects from a row that says ON.
 */
extern int g_videoSettingsDirty;

#ifdef __cplusplus
}
#endif

#endif /* MRACER_OPTIONS_H */
