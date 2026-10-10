/*
 * hud.c - the in-race overlay: the escape menu, the dials and
 * counters, the map, the lap list and the countdown.
 *
 * See docs/subsystems/in-race-menu.txt for the widget framework the
 * MENU half is a reduction of, docs/subsystems/hud-counter.txt for
 * the dials, and hud.h for why none of it is in menu.c.
 *
 * WHAT THE MENU HALF DOES AND DOES NOT REPRODUCE. The original
 * builds the escape menu out of a generic widget object - two text
 * widgets per row, an alignment mode, a per-row entry animation, a
 * mouse cursor and a hotkey derived from the label. This file
 * reproduces the ROWS, their ORDER, their LABELS in all six
 * languages, their vertical spacing and what each one does. It does
 * NOT reproduce the widget object, the slide-in, the mouse or the
 * accelerators - see the header of hud_menu_draw and
 * in-race-menu.txt section 8.
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "hud.h"
#include "audio.h"
#include "font.h"
#include "fontdef.h"
#include "framebuffer.h"
#include "render.h"
#include "trig.h"
#include "log.h"
#include "menu.h"
#include "menures.h"
#include "options.h"
#include "race.h"
#include "trk.h"
#include "cull.h"
#include "transform.h"
#include "timing.h"
#include "racer.h"
#include "transform.h"
#include "prim.h"
#include "queue.h"
#include "bank.h"
#include "bikes.h"
#include "sprite.h"

/* ------------------------------------------------------------------ */
/* The labels                                                          */
/* ------------------------------------------------------------------ */
/*
 * TRANSCRIBED FROM g_localizedStrings (0x004e28e8), ids 104, 105 and
 * 110, all six languages. The original reaches them through the
 * runtime slots at 0x0053b788, 0x0053b78c and 0x0053b7a0, which
 * Lang_SetLanguage fills from that table; the port has no
 * Lang_SetLanguage, so the three rows it needs are written out here.
 * See in-race-menu.txt section 7.
 *
 * THE HIGH BYTES ARE REAL AND ARE NOT ASCII. "\xc4" is A-umlaut and
 * "\xd3" is O-acute in the .FNT code page. They are written as
 * escapes so every byte of this FILE stays under 0x80 - CLAUDE.md
 * rule 6 is about the source, not about what the source produces.
 *
 * The order is the language table's: Us, Fr, Gr, It, Sp, Sw.
 */
static const char *const kRestart[MENURES_LANG_COUNT] = {
	"[RESTART]",
	"[RECOMMENCER]",
	"[NEUSTART]",
	"[RICOMINCIA]",
	"[REINICIAR]",
	"[STARTA OM]"
};

static const char *const kQuitRace[MENURES_LANG_COUNT] = {
	"[QUIT RACE]",
	"[QUITTER LA COURSE]",
	"[RENNEN VERLASSEN]",
	"[ESCI DALLA GARA]",
	"[ABANDONAR LA CARRERA]",
	"[AVSLUTA LOPP]"
};

static const char *const kContinueRace[MENURES_LANG_COUNT] = {
	"[CONTINUE RACE]",
	"[CONTINUER LA COURSE]",
	"[RENNEN FORTSETZEN]",
	"[CONTINUA LA CORSA]",
	"[CONTINUAR LA CARRERA]",
	"[FORTS\xc4TT LOPP]"
};

/*
 * THE FOUR IN-RACE QUESTION WORDS, read out of MOTO's .rdata.
 *
 * The strings are grouped by CONCEPT and run Sw, Sp, It, Gr, Fr,
 * Us in ascending address, with the resource slot aimed at the
 * one for the CURRENT language - those slots carry one language
 * only and are filled at load:
 *
 *     0x0053b774  the CONTINUE title    0x004e0410 in English
 *     0x0053b778  the REPLAY title      0x004e03c8
 *     0x0053b77c  YES                   0x004e0394
 *     0x0053b780  NO                    0x004e036c
 *
 * A GROUP IS NOT ALWAYS SIX. Read byte for byte over
 * 0x004e0350..0x004e0420, the CONTINUE, YES and NO groups are
 * complete and the REPLAY one holds FOUR - Swedish, Spanish,
 * German and English. The Italian and French slots are three
 * and five bytes of alignment padding, not strings, so there is
 * nothing below to find and both fall back to the English.
 *
 * [REPRIS] IS SWEDISH, NOT FRENCH - the Swedish word for a
 * replay, and the rest of that language's replay strings say so
 * plainly: REPRIS LADDAD, AVSLUTA REPRIS, FEL VERSION AV
 * REPRISFIL. There is no [REPRISE] anywhere in the image.
 */
static const char *const kReplayAsk[MENURES_LANG_COUNT] = {
	"[REPLAY]",
	"[REPLAY]",             /* fr - not in the image */
	"[WIEDERHOLUNG]",
	"[REPLAY]",             /* it - not in the image */
	"[REPETICI\xd3N]",
	"[REPRIS]"
};

/* The continue menu's title - 0x004e03d4..0x004e0410. */
static const char *const kContinue[MENURES_LANG_COUNT] = {
	"[CONTINUE]",
	"[CONTINUER]",
	"[WEITER]",
	"[CONTINUA]",
	"[CONTINUAR]",
	"[FORTS\xc4TT]"
};

static const char *const kYes[MENURES_LANG_COUNT] = {
	"[YES]", "[OUI]", "[JA]", "[S\xcc]", "[SI]", "[JA]"
};

static const char *const kNo[MENURES_LANG_COUNT] = {
	"[NO]", "[NON]", "[NEIN]", "[NO]", "[NO]", "[NEJ]"
};

/* Not localised in the original either - a literal at 0x004e8aec. */
#define HUD_TITLE "MOTO RACER"

/* ------------------------------------------------------------------ */
/* Geometry                                                            */
/* ------------------------------------------------------------------ */
/*
 * THE GEOMETRY, ALL OF IT VERIFIED. Authored for 640x480; the home
 * coordinates are OFFSETS resolved by Text_ApplyAlignment
 * (0x00417900) against a flags word carried on the widget at +0x08.
 *
 * Read live off MOTO with the menu up, 640x480:
 *
 *     widget          flags   home    measured   drawn at
 *     MOTO RACER      0xc2    0, 5    136 x 22   252, 213
 *     [RESTART]       0x62    0, 0     65 x  9   287, 240
 *     [QUIT RACE]     0x62    0,22     81 x  9   279, 251
 *     [CONTINUE RACE] 0x62    0,44    113 x  9   263, 262
 *
 * and every one of those falls out of the two flag words below.
 * The rows came out 11 apart rather than 22 because that MOTO has
 * g_cfgSmallFonts set, which halves them; see hud_scale_home.
 */
#define HUD_ROW_DY      22          /* verified: 0, 22, 44           */
#define HUD_TITLE_DY     5          /* verified                      */

/*
 * The alignment flags the two widget kinds carry at +0x08.
 * Text_ApplyAlignment reads them as:
 *
 *   0x62  x += (W - textW)/2       bit 1 set, 3 and 4 clear
 *         y += H/2                 bit 6 set, 7 clear, 5 set
 *
 *   0xc2  x += (W - textW)/2       same horizontal rule
 *         y  = (H/2 - y) - textH   bits 6 AND 7 set
 *
 * so the rows hang DOWN from the middle of the screen and the title
 * is stacked UP off the same line by its own height.
 */
#define HUD_ALIGN_ROW   0x62
#define HUD_ALIGN_TITLE 0xc2

/* FUN_00481970's Menu_DrawText: x 0, y 0x19, align 0xc2. */
#define HUD_PLACE_X     0
#define HUD_PLACE_Y     0x19

#define HUD_ROW_COUNT    3

/* The authored resolution every coordinate above is measured in. */
#define HUD_AUTHOR_W   640
#define HUD_AUTHOR_H   480

/* ------------------------------------------------------------------ */

enum {
	HUD_ROW_RESTART = 0,
	HUD_ROW_QUIT,
	HUD_ROW_CONTINUE
};

/*
 * THE FONT IDS THIS FILE ASKS FOR, all naming the LowRes half
 * of a pair where there is one - fontdef_resolve swaps to the
 * HiRes twin when small fonts are off and the screen mode is 2,
 * which is what Font_Get does.
 *
 * DO NOT PICK THE FACE ON g_options.smallFonts ALONE. Font_Get
 * needs the screen mode too, and choosing on smallFonts by itself
 * agrees with the original only because ScreenMode happens to
 * default to 2. The manifest carries the class and the faces, so
 * nothing here needs to name a file:
 *
 *    0  ChronoLowRes    Direct            Chrono
 *    2  PlayerLowRes    Pal               Name
 *    3  TimeLowRes      Direct            PosTime
 *    9  MenuLink        AntiAlias8Transp  MenuLink+MenuLin2
 *   10  SpritesLowRes   Pal               Sprites
 *   12  Messages        AntiAlias8Transp  Msg
 *   13  GameMsgLowRes   AntiAlias8Transp  PosTime+PosTimeL
 *   16  SpeedLcdLowRes  Pal               SpeedLcd
 *   19  PosLowRes       Direct            PosTime
 *   23  MotorLcdLowRes  Pal               MotorLcd
 *   26  FinalPosLowRes  Pal               FinalPos
 *   33  GameMenuLowRes  Pal               GameMenu
 *
 * 3 AND 19 ARE DIFFERENT FONTS OVER THE SAME FILE, both Direct
 * over PosTime - and 13 is that file again as AntiAlias8Transp.
 * The face supplies pixels and the font supplies colour, so
 * one .FNT legitimately serves three ids.
 *
 * THESE ARE THE IDS THIS FILE OPENS WITH. The id the ORIGINAL
 * hands a WIDGET is a different question, often the HiRes twin,
 * and those are declared at their point of use - MENULINK 9,
 * GAMEMENU 32, MESSAGES 0x0c, GAMEMSG 0x0e. A pair that looks
 * duplicated is answering both questions.
 */
#define HUD_FONT_CHRONO    0
#define HUD_FONT_NAME      2
#define HUD_FONT_TIME      3
#define HUD_FONT_TITLE     9
#define HUD_FONT_SPRITES  10
#define HUD_FONT_BIGMSG   12
#define HUD_FONT_MSG      13
#define HUD_FONT_SPEED    16
#define HUD_FONT_POS      19
#define HUD_FONT_GEAR     23
#define HUD_FONT_PLACE    26
#define HUD_FONT_ROW      33

static int      s_open;
static int      s_sel;
/*
 * WHICH MENU IS UP. The original builds a fresh object for each -
 * InGameMenu_OpenEscape 0x00436420 against FUN_00437b70 - but they
 * are the same widget machinery with different rows, so one mode
 * flag is the port's whole difference.
 */
#define HUD_MENU_ESCAPE    0
#define HUD_MENU_POSTRACE  1
#define HUD_MENU_CONTINUE  2
static int      s_menuMode;
static unsigned s_blink;         /* the frame counter mode 3 uses    */

/*
 * TWO FONTS, AND THEY ARE NOT THE SAME SHAPE. Read live off the
 * widgets: the title asks Font_Get for id 9 and the rows for id 32
 * - which is HUD_FONT_ROW's HiRes twin, not a second id. The table
 * above asks for 33 and Font_Get resolves it.
 * DATA\FONTS.BPT names them and gives their class and their faces:
 *
 *   9   MenuLink       AntiAlias8Transp  HiRes\MenuLink.Raw 32..95
 *                                        HiRes\MenuLin2.Raw 192..211
 *   32  GameMenuHiRes  Pal               HiRes\GameMenu.Raw
 *
 * and the archive ships those as .FNT - the config names the source
 * art, the bank holds the packed font.
 *
 * *** THE ROW FONT IS A PALETTED BITMAP FONT. *** That is why its
 * glyphs come with an embossed face and rounded corners: the
 * decoration is IN THE ART, one character at a time, and the colours
 * are the face's own 256-entry palette rather than anything the
 * caller sets. It is not a sprite per word and it is not per
 * language - the same glyphs draw every translation.
 *
 * The measured heights confirm the pairing: 22 for the title, 9 for
 * the rows.
 */
static MR_Font  s_titleFont;
static MR_Font  s_rowFont;
static int      s_fontOk;
static int      s_fontTried;

/*
 * THE COUNTER'S OWN FACES, and they are LCD ART.
 *
 * Hud_DrawCounter asks Font_Get for ids 0x10 and 0x17, which
 * FONTS.BPT names SpeedLcdLowRes and MotorLcdLowRes - and Font_Get
 * swaps each for its HiRes twin, 15 SpeedLcdHiRes and 22
 * MotorLcdHiRes, unless SmallFonts is on. See MR_Font.txt
 * section 5.
 *
 * BOTH ARE Pal FACES, so the green plate behind the speed readout
 * and the black seven-segment digits are in the ART, not in a
 * colour this code sets. Giving a Pal face colours is wrong for the
 * same reason it is wrong for GameMenu.
 */
static MR_Font  s_speedFont;      /* SpeedLcd - the km/h readout   */
static MR_Font  s_gearFont;       /* MotorLcd - the gear digit     */
static int      s_counterFontOk;
static int      s_counterFontTried;
static int      s_counterFontSmall = -1;

/* The escape menu is titled MOTO RACER; the post-race one asks
 * the question - [REPLAY] - over its two answers. */
static const char *menu_title(void)
{
	int lang = g_options.language;

	if (lang < 0 || lang >= MENURES_LANG_COUNT)
		lang = 0;
	if (s_menuMode == HUD_MENU_POSTRACE)
		return kReplayAsk[lang];
	if (s_menuMode == HUD_MENU_CONTINUE)
		return kContinue[lang];
	return HUD_TITLE;
}

/* The post-race prompt is a yes/no question - two rows, not three. */
static int row_count(void)
{
	/*
	 * THERE IS NO RACE LEFT TO CONTINUE. Escape during the
	 * post-race sequence gives MOTO RACER over RESTART and QUIT
	 * alone [observed] - CONTINUE RACE is the last row, so
	 * dropping it is exactly the two that remain.
	 */
	if (s_menuMode == HUD_MENU_POSTRACE ||
	    s_menuMode == HUD_MENU_CONTINUE)
	    return 2;
	return race_over_hud() ? (HUD_ROW_COUNT - 1) : HUD_ROW_COUNT;
}

static const char *row_label(int row)
{
	int lang = g_options.language;

	if (lang < 0 || lang >= MENURES_LANG_COUNT)
		lang = 0;
	if (s_menuMode == HUD_MENU_POSTRACE ||
	    s_menuMode == HUD_MENU_CONTINUE)
	    return (row == 0) ? kYes[lang] : kNo[lang];
	switch (row) {
	case HUD_ROW_RESTART:  return kRestart[lang];
	case HUD_ROW_QUIT:     return kQuitRace[lang];
	default:               return kContinueRace[lang];
	}
}

static void hud_font_load(void)
{
	if (s_fontTried)
		return;
	s_fontTried = 1;

	/*
	 * MenuLink declares TWO faces - 32..95 in the first and
	 * 192..211 in the second - which is what MR_Font's four face
	 * slots are for. The second holds the accented letters the
	 * other five languages need.
	 */
	s_fontOk = fontdef_open(&s_titleFont, HUD_FONT_TITLE);

	/*
	 * THE ROW FACE IS THE SMALL-FONT SWAP. This asks for
	 * HUD_FONT_ROW, 33, GameMenuLowRes - the LowRes half, as
	 * everything in this file does - and fontdef_resolve answers
	 * with 32, GameMenuHiRes, unless g_cfgSmallFonts is on. The
	 * two are the same 189 glyphs at 18 and 9 pixels.
	 */
	s_fontOk = s_fontOk &&
	           fontdef_open(&s_rowFont, HUD_FONT_ROW);

	/*
	 * THE TITLE'S COLOURS ARE CONFIGURED, NOT BAKED. MenuLink
	 * has no palette - it is an AntiAlias8Transp face whose
	 * pixels are coverage - so the colour comes from
	 * DATA\FONTS.BPT, which gives font 9 a foreground of
	 * (200,200,0) and a background of (150,150,0). Dark yellow
	 * on a darker yellow, NOT WHITE - an AntiAlias8Transp face
	 * left uncoloured draws white.
	 *
	 * THE ROW FONT TAKES NO COLOURS AND MUST NOT BE GIVEN ANY.
	 * Font 32 appears in neither FontForeColors nor
	 * FontBackColors, because a Pal face carries its own
	 * 256-entry palette: the red plate, the white letterforms
	 * and the dark-red bottom edge are all in the art.
	 */
	if (s_fontOk) {
		static const unsigned char fore[3] = { 200, 200, 0 };
		static const unsigned char back[3] = { 150, 150, 0 };

		font_set_colors(&s_titleFont, fore, back);
	}

	if (!s_fontOk)
		mrlog_err("hud: escape-menu fonts did not load\n");
	else
		mrlog("hud: title %d tall, rows %d tall%s\n",
	          font_height(&s_titleFont), font_height(&s_rowFont),
	          g_options.smallFonts ? " (small fonts)" : "");
}


/*
 * ORIGIN  Text_ApplyAlignment @ 0x00417900, whole.
 *
 * x and y are IN/OUT: an offset goes in and a screen coordinate
 * comes out. Only the branches the escape menu uses are exercised,
 * but all of them are here - it is a small function and a slice of
 * it would be worse than the whole.
 */
static void hud_align(int textW, int textH, int *x, int *y, unsigned flags)
{
	int W = g_frameWidth;
	int H = g_frameHeight;
	int px = *x;
	int py = *y;

	if ((flags & 6) == 0) {
		if (flags & 0x18)
			px = (W - px) - textW;
	} else if ((flags & 0x18) == 0) {
		px += (flags & 1) ? W / 2 : (W - textW) / 2;
	} else {
		px = (W / 2 - px) - textW;
	}

	if ((flags & 0x40) == 0) {
		if (flags & 0x80)
			py = (H - py) - textH;
	} else if ((flags & 0x80) == 0) {
		py += (flags & 0x20) ? H / 2 : (H - textH) / 2;
	} else {
		py = (H / 2 - py) - textH;
	}

	*x = px;
	*y = py;
}


/*
 * ORIGIN  Font_Get @ 0x004903d0, the twelve pairs
 *
 * Whether a font id has a LowRes/HiRes twin - which is the same
 * question as whether Font_Get's second out-parameter comes back 0.
 * Every id that is NOT one of the twelve pairs falls into Font_Get's
 * `default:` arm, which sets that flag to 1.
 *
 * o AND THAT FLAG IS WHAT DECIDES THE HALVING, not the SmallFonts
 *   setting on its own. TextWidget_Layout halves the authored
 *   coordinates only when SmallFonts is on AND the font has a twin:
 *
 *       if ((!smallFonts && mode == 2) || substituted)  no halving
 *       else                                            x/2, y/2
 *
 *   A font with no LowRes form is not drawn any smaller, so halving
 *   its position would move it for no reason.
 *
 * IT IS WHY THE ESCAPE MENU MEASURED WITH ITS TITLE UNHALVED AND
 * ITS ROWS HALVED. The title is id 9, MenuLink, unpaired; the rows
 * are id 32, GameMenuHiRes, paired. The two land on opposite sides
 * of this table, which is the whole of it.
 *
 * It also put GET READY on top of the countdown number. That is
 * id 12, Messages, unpaired - so its authored y of 0x1e must NOT
 * halve, while the number's id 14 must.
 */
static int hud_font_has_twin(int fontId)
{
	switch (fontId) {
	case 0:  case 4:              /* Chrono   */
	case 1:  case 5:              /* Moto     */
	case 2:  case 6:              /* Player   */
	case 3:  case 7:              /* Time     */
	case 10: case 11:             /* Sprites  */
	case 13: case 14:             /* GameMsg  */
	case 15: case 16:             /* SpeedLcd */
	case 19: case 20:             /* Pos      */
	case 22: case 23:             /* MotorLcd */
	case 25: case 26:             /* FinalPos */
	case 28: case 29:             /* Counter  */
	case 32: case 33:             /* GameMenu */
		return 1;
	}
	return 0;
}

static int hud_scale_font(int v, int hasTwin);

/* The banner art outlives a race but not the process; its statics
   are declared with the rest of the banner, far below. */
static void hud_banner_art_free(void);

/* ORIGIN  NONE - ours. hud_scale_font for a font known to have a
 * twin, which is every caller that does not ask. */
static int hud_scale(int v)
{
	return hud_scale_font(v, 1);
}

/*
 * ORIGIN  TextWidget_Layout @ 0x004165c0, the scaling half
 *
 * Authored coordinates are for 640x480 and the layout scales them
 * for the smaller modes:
 *
 *     if ((!smallFonts && screenMode == 2) || noTwin)
 *         mode 1  ->  x * 0x200/0x280,  y * 0x180/0x1e0
 *         else        unchanged
 *     else
 *         x / 2,  y / 2
 *
 * hasTwin is the inverse of Font_Get's second out-parameter - see
 * hud_font_has_twin, which is what makes the decision PER FONT
 * rather than per frame.
 */
static int hud_scale_font(int v, int hasTwin)
{
	/*
	 * SMALL FONTS HALVE IT, but only for a font that actually has
	 * a smaller form to be drawn in. That is the else branch.
	 */
	if (g_options.smallFonts && hasTwin)
		v /= 2;
	if (g_frameWidth == HUD_AUTHOR_W && g_frameHeight == HUD_AUTHOR_H)
		return v;
	/*
	 * FROM THE HEIGHT, NOT THE WIDTH. The original's three modes
	 * are all 4:3 so the two ratios are the same number and the
	 * choice never came up; at 854x480 the width says 1.33 and
	 * the height says 1.0, and 1.0 is right - a wider frame is
	 * more picture, not bigger letters.
	 */
	return v * g_frameHeight / HUD_AUTHOR_H;
}


void hud_free(void)
{
	if (s_fontOk) {
		font_free(&s_titleFont);
		font_free(&s_rowFont);
	}
	s_fontOk = 0;
	s_fontTried = 0;
	hud_banner_art_free();
}

/* ------------------------------------------------------------------ */
/* Open and close                                                      */
/* ------------------------------------------------------------------ */

void hud_menu_open(void)
{
	if (s_open || !race_ready())
		return;

	/*
	 * THE PAUSE GATE IS THE ORIGINAL'S. Input_OnKeyDown tests
	 * DAT_004dc038 before it calls InGameMenu_OpenEscape, so escape
	 * does nothing while the game is paused. tm_paused is this
	 * port's equivalent flag.
	 */
	if (tm_paused())
		return;

	/*
	 * PAST THE LINE THE KEY ASKS THE QUESTION INSTEAD. Escape
	 * during the post-race sequence gives [REPLAY] YES/NO on
	 * MOTO, not the in-game menu [observed]; answering NO is
	 * what drops to RESTART/QUIT.
	 */
	if (race_over_hud()) {
		hud_prompt_open();
		return;
	}

	s_open     = 1;
	s_menuMode = HUD_MENU_ESCAPE;
	s_sel      = HUD_ROW_CONTINUE;   /* see below */
	s_blink = 0;
	hud_font_load();

	/*
	 * TWO CUES ON OPEN, and the second one is not obvious.
	 * InGameMenu_OpenEscape plays 0x03 at its top
	 * (0x00436446), and then at its tail it reads the DEFAULT
	 * ROW's enter callback back out and calls it - which is
	 * InGameMenu_OnRowEnter, and that plays 0x29. So the menu
	 * arrives with the open sound and the selection landing
	 * on CONTINUE, not with one sound.
	 *
	 * BOTH ARE ONE-SHOTS, so the pause that fires on the same
	 * frame does not touch them - audio_set_paused holds loops
	 * only. That is the original's rule doing the work rather
	 * than an exemption for the menu. See audio.h.
	 */
	audio_cue(SND_MENU_OPEN, 0);
	audio_cue(SND_MENU_HOVER, 0);

	/*
	 * NOT REPRODUCED: the cursor LoadCursorA(0xe4) each button
	 * takes.
	 *
	 * THE INITIAL SELECTION IS A GUESS. The original points
	 * menu+0x54 at button 1 - the second entry in the array, and
	 * button 0 is the title - then calls that button's enter
	 * handler. Which ROW that is depends on the button order, and
	 * the safest landing spot for a key that is one press from
	 * restarting the race is CONTINUE. [open]
	 */
	mrlog("hud: escape menu open\n");
}

/*
 * ORIGIN  FUN_00437b70 @ 0x00437b70
 *
 * The post-race question - [REPLAY] over [YES] and [NO]. The
 * original builds a whole menu object for it; ours reuses the
 * escape menu's rows in a second mode.
 *
 * NOT REPRODUCED: the fourth row -saverecord adds (DAT_004e8924),
 * which saves the record to a .Sav - the port has neither the
 * flag nor the file format. [open]
 *
 * NO PAUSE GATE HERE, unlike hud_menu_open: this is raised BY the
 * game at the end of the sequence, not by a key.
 */
void hud_prompt_open(void)
{
	/*
	 * THE race_ready TEST IS hud_menu_open'S AND IT BELONGS
	 * HERE TOO.  This one is raised by the game rather than by
	 * a key, which is why it has no PAUSE gate - but a prompt
	 * over a race that no longer exists is not something the
	 * player can answer or dismiss.
	 */
	if (s_open || !race_ready())
		return;
	s_open     = 1;
	s_menuMode = HUD_MENU_POSTRACE;
	s_sel      = 1;          /* NO - see hud_menu_open's note */
	s_blink    = 0;
	hud_font_load();

	/*
	 * THE QUESTION SILENCES THE RACE FIRST. FUN_00437b70
	 * calls Snd_StopAll (0x00424020 -> Csample_StopAll) and
	 * only then plays 0x03 - so the engine note is cut, not
	 * paused, because there is no race to go back to.
	 */
	audio_stop_all();
	audio_cue(SND_MENU_OPEN, 0);
	audio_cue(SND_MENU_HOVER, 0);
	mrlog("hud: post-race prompt open\n");
}


/*
 * ORIGIN  FUN_004358b0 @ 0x004358b0
 *
 * THE CONTINUE MENU - the third in-race menu, raised when the
 * clock has run out and the GAME OVER sequence has finished.
 * *g_pEscapeMenu = 5 where the escape menu is 1; ours is one
 * more mode on the same flattened widget set.
 *
 * IT DESTROYS WHATEVER MENU IS UP FIRST and builds its own -
 * three rows, of which row 0 carries no widgets and is the
 * title, and rows 1 and 2 are YES and NO.  The default
 * selection is ROW 1, read straight off the builder: it stores
 * rowBase + 0x6c, one row stride in, and then calls that row's
 * enter handler - which is where the second cue comes from.
 *
 * TRY AGAIN IS PLAYED HERE, NOT QUEUED.  The builder opens with
 * Snd_StopAll and then Snd_PlayCue(0x44, 0), so anything still
 * pending in the voice ring is DISCARDED rather than spoken
 * over it - and a queued line would be discarded too.
 *
 * NO OPEN BLIP.  The escape menu plays 0x03 and then 0x29; this
 * one plays the voice and then 0x29.
 *
 * [open] THE TITLE SLIDES IN FROM THE LEFT, on animation style
 * 1 - the polar seek, which this file's flattened items cannot
 * carry because they are not widgets.  The same gap applies to
 * every escape-menu row; the numbers and the sizing are in
 * in-race-menu.txt 6.2.4.
 *
 * [open] Its visibility mode is 7, a fixed intensity of 25, so
 * the title is drawn PARTLY TRANSPARENT where the escape
 * menu's is opaque.  Not reproduced - hud_draw_items passes 0.
 */
void hud_continue_open(void)
{
	if (s_open || !race_ready())
		return;
	s_open     = 1;
	s_menuMode = HUD_MENU_CONTINUE;
	s_sel      = 0;          /* YES - the builder's own default */
	s_blink    = 0;
	hud_font_load();

	audio_stop_all();
	audio_cue_delayed_reset();
	audio_cue(SND_VOICE_TRYAGAIN, 0);
	audio_cue(SND_MENU_HOVER, 0);
	mrlog("hud: continue menu open\n");
}

void hud_menu_close(void)
{
	if (!s_open)
		return;
	s_open = 0;
	mrlog("hud: escape menu closed\n");
}

void hud_menu_toggle(void)
{
	if (s_open)
		hud_menu_close();
	else
		hud_menu_open();
}

int hud_menu_active(void)
{
	return s_open;
}

/* ------------------------------------------------------------------ */
/* The rows                                                            */
/* ------------------------------------------------------------------ */

/*
 * ORIGIN  InGameMenu_OnRestart @ 0x00437610
 *
 * The original stops the replay, stops the race and posts
 * g_menuPendingRequest |= 0xb, which the frame loop turns into a
 * reload. The port has no pending-request word; re-entering page 1
 * is the same thing, because page 1 IS the loader and its enter
 * handler calls race_load.
 */
static void row_restart(void)
{
	hud_menu_close();
	mrlog("hud: RESTART\n");
	menu_restart_page();
}

/*
 * ORIGIN  InGameMenu_OnQuitRace @ 0x00437890
 *
 * g_menuPendingRequest |= 0x20c in a solo game. The original also
 * paints menu resource 0xb1 over the frame and presents it so the
 * tear-down is covered, and in a championship marks the local racer
 * state 3 before leaving; neither is reproduced.
 *
 * IT LANDS ON PAGE 9, THE TOP 5 SCREEN - not the main menu.
 * InGameMenu_OnQuitRace hands the game back and the menu chain
 * resumes at the results page; only its ABORT row reaches page 3,
 * and only in a championship.
 *
 * DO NOT SHORTCUT TO PAGE 3. It skips the results screen, and it
 * also leaves the RACE sample bank resident - so the menu then
 * runs on Divers\ cues instead of Menu\ ones. Page 9's
 * chain is what swaps the bank back; see audio-cues.txt 5.5.
 */
static void row_quit_race(void)
{
	hud_menu_close();
	mrlog("hud: QUIT RACE -> page 9\n");
	/*
	 * IT DOES NOT CALL race_free ITSELF. page01_leave does,
	 * and the transition runs it - freeing here would leave
	 * that call looking at a torn-down track.
	 */
	menu_begin_transition(MENU_PAGE_RESULTS);
}

/*
 * ORIGIN  InGameMenu_OnContinueRace @ 0x00437ae0
 *
 * Sets the close flag, clears the shift and control flags and calls
 * FUN_00470fc0, which is unidentified. Closing is the whole of the
 * visible effect.
 */
static void row_continue(void)
{
	hud_menu_close();
	mrlog("hud: CONTINUE RACE\n");
}

/*
 * ORIGIN  FUN_00437b70 @ 0x00437b70, its YES handler
 *
 * A REPLAY IS THE RACE RUN AGAIN, from the grid, with the recorded
 * inputs instead of the keyboard - so YES restarts and arms the
 * playback rather than rewinding anything in place. The arm
 * survives the reload; see race_replay_start.
 */
static void row_replay_yes(void)
{
	if (!race_replay_start()) {
		mrlog("hud: REPLAY - nothing recorded\n");
		row_quit_race();
		return;
	}
	hud_menu_close();
	mrlog("hud: REPLAY\n");
	menu_restart_page();
}

/*
 * ORIGIN  FUN_00436210 @ 0x00436210 - the continue menu's YES
 *
 * IT IS THE SAME REQUEST RESTART POSTS.  g_menuPendingRequest
 * goes to 0xb in a solo game, and 0xb is bit 0 plus bit 1 plus
 * bit 3 in the dispatcher at 0x004587e0 - reload the race, tick
 * it, clear the replay mode - with NO page bit, so the menu
 * chain does not move.  The race simply starts again.
 */
static void row_continue_yes(void)
{
	hud_menu_close();
	mrlog("hud: CONTINUE - YES, restart\n");
	menu_restart_page();
}

/*
 * ORIGIN  FUN_00436250 @ 0x00436250 - the continue menu's NO
 *
 * THE DESTINATION DEPENDS ON THE MODE, and the two requests
 * decode against the dispatcher at 0x004587e0, whose page bits
 * are 0x100 -> 5, 0x200 -> 9, 0x400 -> 6, 0x800 -> 1,
 * 0x1000 -> 7, 0x2000 -> 14, 0x4000 -> 8:
 *
 *     championship (2) or practice (0)   0x20c  ->  page 9
 *     single race (1)                    0x40c  ->  page 6
 *
 * and the shared low nibble 0xc is bit 2, stop the race, plus
 * bit 3, clear the replay mode.  So NO in a championship goes
 * exactly where QUIT RACE goes - the TOP 5 screen - and NO in a
 * single race goes back to RACE SELECT.
 *
 * NOT REPRODUCED: the cover-up before the tear-down - see
 * row_quit_race, which omits the same thing. This one also
 * resets the viewport and projection first.
 */
static void row_continue_no(void)
{
	int page = (g_selectedMode == 2 || g_selectedMode == 0)
	               ? MENU_PAGE_RESULTS : MENU_PAGE_RACE;

	hud_menu_close();
	mrlog("hud: CONTINUE - NO -> page %d\n", page);
	menu_begin_transition(page);
}

static void row_activate(int row)
{
	if (s_menuMode == HUD_MENU_CONTINUE) {
		if (row == 0)
			row_continue_yes();
		else
			row_continue_no();
		return;
	}
	if (s_menuMode == HUD_MENU_POSTRACE) {
		if (row == 0) {
			row_replay_yes();
		} else {
			/*
			 * NO DOES NOT LEAVE THE RACE. It hands over to the
			 * ordinary in-game menu, which past the line is
			 * RESTART and QUIT [observed]. The prompt stays
			 * open - only its mode changes.
			 */
			s_menuMode = HUD_MENU_ESCAPE;
			s_sel      = HUD_ROW_RESTART;
		}
		return;
	}
	switch (row) {
	case HUD_ROW_RESTART: row_restart();    break;
	case HUD_ROW_QUIT:    row_quit_race();  break;
	default:              row_continue();   break;
	}
}

int hud_menu_on_key(int vkey)
{
	if (!s_open)
		return 0;

	switch (vkey) {
	/*
	 * A ROW GAINING THE SELECTION IS WHAT MAKES THE SOUND,
	 * not the key. The original hangs InGameMenu_OnRowEnter
	 * off each row's widget and it plays 0x29 whether the
	 * selection arrived by key or by pointer - which is why
	 * both arms below cue it and neither cues on the way out
	 * (InGameMenu_OnRowLeave is silent).
	 */
	case VK_UP:
		s_sel = (s_sel + row_count() - 1) % row_count();
		audio_cue(SND_MENU_HOVER, 0);
		return 1;
	case VK_DOWN:
		s_sel = (s_sel + 1) % row_count();
		audio_cue(SND_MENU_HOVER, 0);
		return 1;
	case VK_RETURN:
		row_activate(s_sel);
		return 1;
	case 'Q':
		/* THE POST-RACE PROMPT HAS NO WAY OUT but answering it -
		 * the original's escape menu is the one with a CONTINUE
		 * row, and there is no race left to continue here. */
		if (s_menuMode == HUD_MENU_ESCAPE && !race_over_hud())
			hud_menu_close();
		return 1;
	}
	/*
	 * EVERYTHING ELSE IS SWALLOWED while the menu is up, which is
	 * what the original does by returning 0 from
	 * InGameMenu_ServiceAll - the race never sees the frame, so it
	 * never sees the keys either.
	 */
	return 1;
}

/* ------------------------------------------------------------------ */
/* Draw                                                                */
/* ------------------------------------------------------------------ */
/*
 * NOT REPRODUCED, and deliberately not approximated:
 *
 *   THE SLIDE. Every row of the escape menu is animated, and so
 *   is the title of every one of the three menus - each entering
 *   from its own place, the rows after their own delay. It is a
 *   POLAR SEEK rather than a lerp: the widget turns its velocity
 *   toward the target by a fraction of the angular error and
 *   closes at a speed proportional to the distance still to run,
 *   in a frame where y is measured UPWARD. That overshoots and
 *   settles rather than easing, so it must not be approximated.
 *
 *   IT IS NO LONGER A RESEARCH PROBLEM. Widget_StepAnimation @
 *   0x00416b40 and its six constants are read, and every start,
 *   target, initial velocity, gain and delay the two builders
 *   pass is tabulated - with what the port would have to grow to
 *   run them, and the three things still unread. All of it is in
 *   in-race-menu.txt 6.2.3 and 6.2.4; none of it is repeated
 *   here.
 *
 *   The blocker on this side is structural rather than unknown:
 *   the title and rows are drawn from hud_item_rect every frame,
 *   so there is nowhere to keep a position between frames.
 *
 *   THE MOUSE. Each button loads its own cursor and the menu is
 *   clickable; the port draws only.
 *
 * WHAT IS REPRODUCED: the selected row BLINKS. Each row is two
 * widgets in the original, a steady one and a copy with visibility
 * mode 3, and the enter handler swaps to the blinking copy. Mode 3
 * with no period counts frames and masks with 2 (TextWidget_Draw,
 * 0x00416da0), which is two frames on and two off - so this is the
 * original's flicker, not a chosen one.
 */
static int blink_visible(void)
{
	return (s_blink & 2u) != 2u;
}

/*
 * THE TITLE AND THE ROWS SCALE DIFFERENTLY, and it is the font
 * that decides. The title is font id 9, MenuLink, which has no
 * LowRes twin; the rows are the GameMenu pair, which does. So with
 * SmallFonts on the rows halve and the title does not - the
 * arrangement in-race-menu.txt 6.2 measured off MOTO and could
 * only describe. See hud_font_has_twin.
 *
 * These two ask the twin question; HUD_FONT_TITLE and HUD_FONT_ROW
 * open the face. See the id table's note.
 */
#define HUD_FONT_MENULINK   9
#define HUD_FONT_GAMEMENU  32

/*
 * hud_item_rect - where one item lands, and how big it is.
 *
 * Shared by the draw and by the overlay rect, so the two cannot
 * disagree about where the menu is.
 */
static void hud_item_rect(MR_Font *f, int fontId, const char *text,
                          int homeY, unsigned flags,
                          int *x, int *y, int *w, int *h)
{
	*w = font_text_width(f, text);
	*h = font_height(f);
	*x = 0;
	*y = hud_scale_font(homeY, hud_font_has_twin(fontId));
	hud_align(*w, *h, x, y, flags);
}

/* The items, in draw order. Run twice - see hud_menu_draw. */
static void hud_draw_items(void)
{
	int i;
	int x, y, w, h;

	hud_item_rect(&s_titleFont, HUD_FONT_MENULINK, menu_title(),
	              HUD_TITLE_DY,
	              HUD_ALIGN_TITLE, &x, &y, &w, &h);
	font_draw_text(&s_titleFont, x, y, menu_title(), 0);

	/* row_count(), NOT the constant - the post-race menus are
	 * SHORTER, and the draw has to agree with the navigation or
	 * it paints a row nothing can select. */
	for (i = 0; i < row_count(); i++) {
		const char *t = row_label(i);

		if (i == s_sel && !blink_visible())
			continue;
		hud_item_rect(&s_rowFont, HUD_FONT_GAMEMENU, t,
		              i * HUD_ROW_DY,
		              HUD_ALIGN_ROW, &x, &y, &w, &h);
		font_draw_text(&s_rowFont, x, y, t, 0);
	}
}

/* ================================================================== */
/* THE COUNTER - the speedometer and rev counter, lower right.        */
/*                                                                    */
/* Transcribed from Hud_DrawCounter (0x00484810) and Hud_DialInit     */
/* (0x00485be0). Every number below carries the address it came from  */
/* or is in the table of docs/subsystems/hud-counter.txt section 3.   */
/* ================================================================== */

/*
 * MR_HudDial - the dial descriptor Hud_DialInit builds, 0x30 in the
 * original. Ours is the same eight fields in the same order; nothing
 * depends on the layout, so it is a plain struct.
 */
typedef struct {
	int    cx;
	int    cy;
	int    minValue;
	int    maxValue;
	double angleMin;
	double angleMax;
	double radPerUnit;
	double radius;
} MR_HudDial;


/*
 * ORIGIN  Hud_DialInit @ 0x00485be0, whole
 *
 * Three screen points in, a dial out. See hud-counter.txt 4.1 for
 * why both atan2 calls negate Y and why one angle is pushed a whole
 * turn negative.
 */
static void hud_dial_init(MR_HudDial *d, int cx, int cy,
                          int zeroX, int zeroY, int fullX, int fullY,
                          int minValue, int maxValue, int reversed)
{
	double dx = (double)(zeroX - cx);
	double dy = (double)(zeroY - cy);

	d->cx       = cx;
	d->cy       = cy;
	d->minValue = minValue;
	d->maxValue = maxValue;

	d->radius   = sqrt(dx * dx + dy * dy);
	d->angleMin = atan2((double)(cy - zeroY), dx);
	d->angleMax = atan2((double)(cy - fullY), (double)(fullX - cx));

	if (d->angleMin <  0.0)        d->angleMin += MR_TWO_PI;
	if (d->angleMin >= MR_TWO_PI) d->angleMin -= MR_TWO_PI;
	if (d->angleMax <  0.0)        d->angleMax += MR_TWO_PI;
	if (d->angleMax >= MR_TWO_PI) d->angleMax -= MR_TWO_PI;

	if (!reversed) {
		if (d->angleMin < d->angleMax)
			d->angleMax -= MR_TWO_PI;
	} else {
		if (d->angleMax < d->angleMin)
			d->angleMin -= MR_TWO_PI;
	}

	d->radPerUnit = (maxValue == minValue)
	                ? 0.0
	                : (d->angleMax - d->angleMin) /
	                  (double)(maxValue - minValue);
}

/*
 * THE LAYOUT, section 3 of the document. Two columns, picked by
 * `hi` - full size is 640x480 with big fonts, everything else is the
 * other one.
 */
typedef struct {
	int speedTextX, speedTextY;
	int gearTextX,  gearTextY;
	int spdHubX, spdHubY, spdZeroX, spdZeroY, spdFullX, spdFullY;
	int tacHubX, tacHubY, tacZeroX, tacZeroY, tacFullX, tacFullY;
	int shadowX, shadowY;
	int shiftX,  shiftY;
} HUD_CounterLayout;

static const HUD_CounterLayout kCounterLayout[2] = {
	/* half size, 0x00484a9b */
	{ 0xe2, 0x4f,  0x1f, 0x33,
	  0xe3, 0x44,  0xdb, 0x63,  0xf9, 0x2a,
	  0xb7, 0x4f,  0xb1, 0x63,  0xb7, 0x39,
	  -2, -2,      0x9a, 0x1e },
	/* full size, 0x004849df */
	{ 0xcf, 0xd2,  0x43, 0x6a,
	  0xcd, 0xbf,  0xbb, 0xfa,  0xf2, 0x8e,
	  0x77, 0xd3,  0x6c, 0xf7,  0x77, 0xaa,
	  -3, -3,      0x3d, 0x74 }
};

static MR_HudDial s_dialSpeedo;
static MR_HudDial s_dialTacho;
static int        s_counterHi   = -1;   /* the `hi` it was built for */
static int        s_counterType = -1;
static int        s_counterOx;          /* 0x005dbc80 */
static int        s_counterOy;          /* 0x005dbc84 */
static int        s_tachoShown;         /* 0x004f0e10 */

/*
 * Two needles at once: four vertices and four primitives for the
 * speedo, three and two for the tacho. The original keeps them in
 * .data at 0x005f1328 and 0x005f1408; ours are file statics for the
 * same reason - the queue holds POINTERS to them until the walk.
 */
static MR_Vertex    s_spdVerts[8];    /* 0..3 needle, 4..7 shadow    */
static MR_Primitive s_spdPrims[4];
static MR_Vertex    s_tacVerts[6];    /* 0..2 needle, 3..5 shadow    */
static MR_Primitive s_tacPrims[2];

#define HUD_COUNTER_MAX_KMH 300
#define HUD_COUNTER_MAX_RPM 16000

/* The tacho needle moves at most this much in one frame, 0x00485706. */
#define HUD_TACHO_SLEW      3000

/* Slack around the readouts in the overlay composite, so a glyph
 * that overhangs its origin is not sheared off. */
#define HUD_COUNTER_PAD     4

/* Queue depths: the needle in front of its shadow, both in front of
 * the dial face at 15 - see hud-counter.txt 5.2. */
#define HUD_NEEDLE_Z        4.0f
#define HUD_SHADOW_Z        8.0f

/*
 * Color_Pack16 is (r << 11) | (g << 6) | b - NOTE THE GAP AT BIT 5.
 * fx.c packs the same way; it is 5-5-5 in a 16-bit word, not 5-6-5.
 */
#define HUD_PACK16(r, g, b) \
    ((unsigned short)(((r) << 11) | ((g) << 6) | (b)))

#define HUD_NEEDLE_DARK   HUD_PACK16(0x19, 0x10, 0)
#define HUD_NEEDLE_LIGHT  HUD_PACK16(0x1f, 0x10, 0)
#define HUD_NEEDLE_SHADOW HUD_PACK16(0, 0, 0)

/*
 * ORIGIN  Face_InitTriangle @ 0x00422be0, whole
 *
 * kind 0 - flat, untextured. The 5-5-5 unpack keeps the original's
 * shape exactly, including the shift left by three that turns five
 * bits into eight.
 */
static void hud_face_triangle(MR_Primitive *p, unsigned short colour,
                              int i0, int i1, int i2)
{
	unsigned int r = (unsigned int)(colour >> 11);
	unsigned int g = (unsigned int)((colour & 0x7c0) >> 6);
	unsigned int b = (unsigned int)(colour & 0x1f);
	int          i;

	memset(p, 0, sizeof(*p));
	p->kind          = 0;
	p->effectiveKind = 0;
	p->colorIndex    = colour;
	p->flatColor     = ((r << 16) | (g << 8) | b) << 3;
	p->bankIndex     = 0;
	p->flags         = 0;

	p->vertexIndex[0] = (unsigned short)i0;
	p->vertexIndex[1] = (unsigned short)i1;
	p->vertexIndex[2] = (unsigned short)i2;
	p->vertexIndex[3] = (unsigned short)i0;   /* the wrap repeats 0 */
	p->vertexIndex[4] = PRIM_NO_WRAP;

	for (i = 0; i < PRIM_SLOTS * 2; i++)
		p->uv[i] = 0.0f;
}

/*
 * ORIGIN  Hud_DrawCounter @ 0x00484810, at 0x00485284
 *
 * One needle vertex. (along, across) is dial-local polar; the Y flip
 * is the original's, and so is truncating each term to int SEPARATELY
 * before adding it to the hub - __ftol per term, with the cluster
 * origin added afterwards. See hud-counter.txt 5.1.
 */
static void hud_needle_point(MR_Vertex *v, const MR_HudDial *d,
                             double angle, int along, int across,
                             int shadow)
{
	double c  = cos(angle);
	double sn = sin(angle);

	float z;

	v->screenX = d->cx + (int)((double)along * c)
	                   - (int)((double)across * sn) + s_counterOx;
	v->screenY = d->cy - (int)((double)along * sn)
	                   - (int)((double)across * c) + s_counterOy;

	/*
	 * DIVERGENCE, AND A FORCED ONE: THE VERTEX IS BACK-PROJECTED.
	 *
	 * The original submits SCREEN coordinates and uses camZ only as
	 * the 1/z the texture mapper wants - 2048 for a needle, 1.0 for
	 * a shadow, neither of which orders anything. The port's scene
	 * path went to CAMERA space instead: drawscene.c's fill_vertex
	 * reads camX/camY/camZ and lets the device's matrix project
	 * them, precisely so D3D9 can clip properly (see the note there).
	 *
	 * So a screen coordinate has to be turned back into one:
	 *
	 *     screenX = viewCentreX + camX/camZ
	 *     screenY = viewCentreY - camY/camZ
	 *
	 * inverted here.  WRITING camX = camY = 0 AND TRUSTING screenX
	 * COLLAPSES EVERY NEEDLE ONTO THE PROJECTION CENTRE - four
	 * degenerate triangles in the middle of the screen, drawing
	 * nothing visible.
	 *
	 * z IS JUST PAST THE NEAR PLANE so the needles win the depth
	 * test against everything in the track. The queue depth still
	 * orders needle against shadow; this only has to be nearer than
	 * the world.
	 */
	z = g_nearPlaneZ * 1.25f;
	if (z <= 0.0f)
		z = 1.0f;

	v->camX  = (float)(v->screenX - g_viewCentreX) * z;
	v->camY  = (float)(g_viewCentreY - v->screenY) * z;
	v->camZ  = z;
	v->invZ  = 1.0f / z;
	v->shade = 0x3e000;               /* intensity 31, full bright */

	/*
	 * [open] shadow IS DEAD - every caller passes 0 and
	 * hud_shadow_of makes the shadows. It selected the original's
	 * camZ 2048 against a shadow's 1.0 (hud-counter.txt 5.2),
	 * which the single z above replaces.
	 */
	(void)shadow;
}

/*
 * The shadow copy of a needle vertex: the same point moved by the
 * layout's offset - which is NEGATIVE, so this is down and right -
 * and back-projected again, because camX/camY carry the pixel.
 */
static void hud_shadow_of(MR_Vertex *dst, const MR_Vertex *src,
                          const HUD_CounterLayout *L)
{
	float z = src->camZ;

	*dst = *src;
	dst->screenX -= L->shadowX;
	dst->screenY -= L->shadowY;
	dst->camX = (float)(dst->screenX - g_viewCentreX) * z;
	dst->camY = (float)(g_viewCentreY - dst->screenY) * z;
}

/*
 * ORIGIN  Hud_DrawCounter @ 0x00484810, at 0x00485284
 *
 * The angle the needle sits at for value, clamped to the dial -
 * hud-counter.txt 5.1's first line,
 * clamp(value, min, max) * radPerUnit + angleMin.
 */
static double hud_dial_angle(const MR_HudDial *d, int value)
{
	if (value < d->minValue) value = d->minValue;
	if (value > d->maxValue) value = d->maxValue;
	return (double)value * d->radPerUnit + d->angleMin;
}

/*
 * ORIGIN  Hud_DrawCounter @ 0x00484810, the layout block at 0x00484e60
 *
 * Rebuilt only when the size switch or the counter type changes,
 * which is what the original's three-way compare against 0x004f0e30
 * .. 0x004f0e40 does.
 */
static void hud_counter_layout(int hi, int type)
{
	const HUD_CounterLayout *L = &kCounterLayout[hi ? 1 : 0];

	s_counterHi   = hi;
	s_counterType = type;

	/*
	 * TYPE 1 MOVES THE WHOLE CLUSTER by minus the shift and is the
	 * only type that gets dials at all. The shift is uniform, so it
	 * does not change either dial's angles - only where they sit.
	 */
	if (type == 1) {
		int sx = L->shiftX;
		int sy = L->shiftY;

		hud_dial_init(&s_dialSpeedo,
		              L->spdHubX  - sx, L->spdHubY  - sy,
		              L->spdZeroX - sx, L->spdZeroY - sy,
		              L->spdFullX - sx, L->spdFullY - sy,
		              0, HUD_COUNTER_MAX_KMH, 0);
		hud_dial_init(&s_dialTacho,
		              L->tacHubX  - sx, L->tacHubY  - sy,
		              L->tacZeroX - sx, L->tacZeroY - sy,
		              L->tacFullX - sx, L->tacFullY - sy,
		              0, HUD_COUNTER_MAX_RPM, 0);
	}
}

/*
 * Loaded separately from the escape menu's faces so a missing LCD
 * font costs the counter and not the menu.
 */
static void hud_counter_font_load(void)
{
	/* The same SmallFonts reload as hud_race_font_load - see there. */
	if (s_counterFontTried && s_counterFontSmall == g_options.smallFonts)
		return;
	if (s_counterFontTried && s_counterFontOk) {
		font_free(&s_speedFont);
		font_free(&s_gearFont);
		s_counterFontOk = 0;
	}
	s_counterFontTried = 1;
	s_counterFontSmall = g_options.smallFonts;

	s_counterFontOk =
	    fontdef_open(&s_speedFont, HUD_FONT_SPEED) &&
	    fontdef_open(&s_gearFont, HUD_FONT_GEAR);

	if (!s_counterFontOk)
		mrlog_err("hud: counter LCD fonts did not load\n");
	else
		mrlog("hud: counter fonts - speed %d tall, gear %d tall\n",
	          font_height(&s_speedFont), font_height(&s_gearFont));
}

/* The size switch, 0x004851da. */
static int hud_counter_hi(void)
{
	return (!g_options.smallFonts && g_options.screenMode == 2);
}

/*
 * ORIGIN  Hud_DrawCounter @ 0x00484810, at 0x00484d60
 *
 * THE ONLY THING IN THE COUNTER TIED TO THE FRAME. Everything else
 * is an offset from here, so the cluster hugs the bottom-right
 * corner at 640x480 and at 854x480 alike.
 *
 * The original reads g_viewWidth and g_viewTopMargin + g_viewHeight;
 * ours has no top margin and the clip rect IS the viewport.
 */
static void hud_counter_origin(int hi)
{
	if (hi) {
		s_counterOx = g_clipWidth  - 195;
		s_counterOy = g_clipHeight - 140;
	} else {
		s_counterOx = g_clipWidth  - 102;
		s_counterOy = g_clipHeight -  74;
	}
}

/* mph, 0x00484d1c: v * 1000 / 1609, done in fixed point. */
static int hud_counter_speed(int kmh)
{
	if (!g_options.speedUnit)
		return kmh;
	return (int)(((__int64)kmh * 0x3e800) / 0x649) >> 8;
}

/* Nothing at all when ShowCounter is clear; otherwise the type. */
static int hud_counter_type(void)
{
	if (!g_options.showCounter)
		return 0;
	/* The auto-ride hides it WITHOUT writing the option - see
	   race_counter_hidden. */
	if (race_counter_hidden())
		return 0;
	return g_options.counterType;
}

/*
 * THE DIAL FACE - the plate the needles turn over.
 *
 * ORIGIN  Hud_DrawSignMeshes @ 0x00486310, the slot-0 arm
 *
 * IT IS A MESH, NOT A SPRITE AND NOT A FONT GLYPH. Read live out of
 * a paused MOTO: the template named "counter", four vertices and two
 * faces - ONE DOUBLE-SIDED QUAD, 94 by 67 units, textured from a
 * 256-page at texels (14,178) to (107,245). Every circle, tick mark,
 * red band, black pie segment and the green LCD plate behind the
 * speed readout is painted into that texture.
 *
 * AND IT SHIPS INSIDE EVERY .trk. The string "counter" appears in
 * every track file and nowhere else in the data except DESIGN.BKF's
 * directory. So it is an ordinary decor template, the track's own
 * art, reached the way maybe_Trk_CountCameras reaches the CAMERAs -
 * by name.
 *
 * [open] "EVERY" IS TEN. decomp/DATA holds ten .trk files, ile07
 * absent; app-init.txt 300 and hud-counter.txt 550 say eleven.
 * Unsettled - the stock disc was not to hand.
 *
 * THE SAME FUNCTION DRAWS THE SIX CORNER WARNING SIGNS from slots 1
 * to 6 of the same table: g_45, d_45, g_90, d_90, g_chica, d_chica,
 * gauche and droite, all anchored at (320, 400) and blinking on a
 * timer under DisplaySignal. Not reconstructed - see
 * docs/subsystems/hud-counter.txt 7.
 */

/*
 * The record maybe_Hud_BindCornerSigns writes for slot 0, read live
 * at 0x005d2eb8: mesh index 14, anchor (93, 0), flag +0x10 cleared.
 * The anchor is all we need - the index is into a global mesh array
 * the port does not have.
 */
#define HUD_FACE_ANCHOR_X   93
#define HUD_FACE_ANCHOR_Y   0

/*
 * THE PLATE IS AUTHORED AT THE HALF-SIZE LAYOUT and scaled up for
 * the full one [verified, 0x00486464]:
 *
 *     screenMode == 2 && !smallFonts    1.9, 2.0
 *     otherwise                         1.0, 1.0
 *
 * Which is the other half of why the cluster is bigger with big
 * fonts on: 94 * 1.9 is 179 and 67 * 2.0 is 134, against the
 * full-size layout's 195 by 140 footprint.
 */
#define HUD_FACE_SCALE_X    1.9f
#define HUD_FACE_SCALE_Y    2.0f

/* Behind both needles - they queue at 4 and 8. */
#define HUD_FACE_Z          15.0f

/* The plate is one quad, so two primitives and four vertices. */
static MR_Vertex s_faceVerts[8];

void hud_counter_draw_face(void)
{
	const MR_TrkMesh *mesh;
	float             sx, sy, ox, oy;
	int               hi = hud_counter_hi();
	int               i;
	int               n;

	if (hud_counter_type() != 1)
		return;

	/*
	 * THE FLAG Hud_DrawCounter WRITES INTO THE RECORD is
	 * (CounterType == 1), at 0x00484cec - which is why type 2
	 * draws its digits over bare track with no plate behind them.
	 */
	mesh = race_find_named_mesh("counter", 7);
	if (!mesh || mesh->vertexCount <= 0 || mesh->primCount <= 0)
		return;

	n = mesh->vertexCount;
	if (n > (int)(sizeof(s_faceVerts) / sizeof(s_faceVerts[0])))
		n = (int)(sizeof(s_faceVerts) / sizeof(s_faceVerts[0]));

	sx = hi ? HUD_FACE_SCALE_X : 1.0f;
	sy = hi ? HUD_FACE_SCALE_Y : 1.0f;

	/*
	 * ANCHORED TO THE BOTTOM RIGHT, and note the Y flip: the mesh's
	 * +Y runs UP the screen [verified, 0x0048650c onward].
	 *
	 *     offsetX = g_viewMaxX - anchorX * scaleX
	 *     offsetY = g_viewMaxY - anchorY * scaleY
	 *     screenX = vx * scaleX + offsetX
	 *     screenY = offsetY - vy * scaleY
	 *
	 * With the anchor at (93, 0) and scale 1 that puts the plate at
	 * x 546..640, y 412..479 in a 640x480 frame - the corner. The
	 * anchor is an INSET, not a position; app-init.txt 285 has the
	 * reading and why the opposite one is tempting.
	 */
	ox = (float)g_viewMaxX - (float)HUD_FACE_ANCHOR_X * sx;
	oy = (float)g_viewMaxY - (float)HUD_FACE_ANCHOR_Y * sy;

	for (i = 0; i < n; i++) {
		const float *v = &mesh->verts[i * 3];
		MR_Vertex   *d = &s_faceVerts[i];
		float        z;

		d->screenX = (int)(v[0] * sx + ox);
		d->screenY = (int)(oy - v[1] * sy);

		/*
		 * Back-projected for the same reason the needles are -
		 * see hud_needle_point. The original writes camZ 2048 and
		 * invZ 1/2048 here too, and for the same non-reason: the
		 * queue depth is what orders it.
		 */
		z = g_nearPlaneZ * 1.25f;
		if (z <= 0.0f)
			z = 1.0f;

		d->camX  = (float)(d->screenX - g_viewCentreX) * z;
		d->camY  = (float)(g_viewCentreY - d->screenY) * z;
		d->camZ  = z;
		d->invZ  = 1.0f / z;
		d->shade = 0x3e000;
	}

	/*
	 * ONLY THE FRONT-FACING HALF, and this is a DIVERGENCE IN WHERE
	 * the test happens rather than in whether it happens.
	 *
	 * The two faces are the SAME QUAD WOUND BOTH WAYS, and their
	 * UVs are mirrored with them - face 0 maps v2 to u 0.418 and
	 * face 1 maps v3 to it. The original queues both and lets the
	 * rasterizer's own winding test throw one away, which is the
	 * whole reason the pair exists: whichever way the quad ends up
	 * facing, exactly one of them draws.
	 *
	 * OUR BACK END DOES NOT CULL. cull_and_queue_faces is where the
	 * port's winding test lives, so queueing a primitive directly
	 * goes round it and BOTH halves draw - the plate comes out as
	 * two gauges mirrored about a vertical axis.
	 *
	 * This is the same cross product cull.c uses, against the same
	 * 1.0f, so backface and degenerate-area rejection stay one
	 * comparison. Doing it here rather than routing through
	 * cull_and_queue_faces keeps the queue depth at the original's
	 * 15; that function derives its own from camZ.
	 */
	for (i = 0; i < mesh->primCount; i++) {
		const MR_Primitive *pr = &mesh->prims[i];
		const MR_Vertex    *a  = &s_faceVerts[pr->vertexIndex[0]];
		const MR_Vertex    *b  = &s_faceVerts[pr->vertexIndex[1]];
		const MR_Vertex    *c  = &s_faceVerts[pr->vertexIndex[2]];
		float               facing;

		if (pr->vertexIndex[0] >= (unsigned short)n ||
		    pr->vertexIndex[1] >= (unsigned short)n ||
		    pr->vertexIndex[2] >= (unsigned short)n)
		    continue;

		facing = (float)((a->screenY - b->screenY) *
		                 (c->screenX - b->screenX) +
		                 (c->screenY - b->screenY) *
		                 (b->screenX - a->screenX));
		if (facing <= 1.0f)
			continue;

		queue_primitive((MR_Primitive *)pr, HUD_FACE_Z,
		                s_faceVerts, 0, 0);
	}
}

void hud_counter_draw_needles(void)
{
	const MR_Racer          *r    = race_viewed_racer();
	int                      hi   = hud_counter_hi();
	int                      type = hud_counter_type();
	const HUD_CounterLayout *L;
	int                      speed;
	int                      tacho;
	int                      d;
	double                   a;
	int                      i;

	if (type != 1 || !r)
		return;

	if (hi != s_counterHi || type != s_counterType)
		hud_counter_layout(hi, type);
	hud_counter_origin(hi);
	L = &kCounterLayout[hi ? 1 : 0];

	speed = hud_counter_speed(r->speedKmhNeedle);
	if (speed < 0)
		speed = 0;

	/* ---- the speedometer: four points, two triangles ------------ */
	a = hud_dial_angle(&s_dialSpeedo, speed);

	/* back, tip, left, right - hud-counter.txt 5.1 */
	hud_needle_point(&s_spdVerts[0], &s_dialSpeedo, a,
	                 -5 * (1 + hi), 0, 0);
	hud_needle_point(&s_spdVerts[1], &s_dialSpeedo, a,
	                 (int)s_dialSpeedo.radius, 0, 0);
	hud_needle_point(&s_spdVerts[2], &s_dialSpeedo, a,
	                 0, -3 * (1 + hi), 0);
	hud_needle_point(&s_spdVerts[3], &s_dialSpeedo, a,
	                 0, 3 * hi + 3, 0);

	for (i = 0; i < 4; i++)
		hud_shadow_of(&s_spdVerts[4 + i], &s_spdVerts[i], L);

	/*
	 * TWO SHADES, WHICH IS WHY THE NEEDLE LOOKS LIT DOWN ONE EDGE.
	 * Only the speedo is two-toned; the tacho below uses the bright
	 * colour for its single triangle.
	 */
	hud_face_triangle(&s_spdPrims[0], HUD_NEEDLE_DARK,   0, 1, 2);
	hud_face_triangle(&s_spdPrims[1], HUD_NEEDLE_LIGHT,  0, 3, 1);
	hud_face_triangle(&s_spdPrims[2], HUD_NEEDLE_SHADOW, 0, 1, 2);
	hud_face_triangle(&s_spdPrims[3], HUD_NEEDLE_SHADOW, 0, 3, 1);

	queue_primitive(&s_spdPrims[0], HUD_NEEDLE_Z, s_spdVerts,     0, 0);
	queue_primitive(&s_spdPrims[1], HUD_NEEDLE_Z, s_spdVerts,     0, 0);
	queue_primitive(&s_spdPrims[3], HUD_SHADOW_Z, s_spdVerts + 4, 0, 0);
	queue_primitive(&s_spdPrims[2], HUD_SHADOW_Z, s_spdVerts + 4, 0, 0);

	/* ---- the rev counter: three points, one triangle ------------ */
	/*
	 * THE DIAL IS NORMALISED TO THE REDLINE.
	 *
	 * ORIGIN  Hud_DrawCounter @ 0x00484810, at 0x00485844
	 *
	 *     shown = racer+0x11c / RegimeMax * 16000
	 *
	 * FILD racer+0x11c, FDIV g_bikeCarac+0x74 - field 29,
	 * RegimeMax - then FMUL the dword at 0x005f14bc, which
	 * reads 0x3e80 = 16000 and is the dial's own full scale.
	 *
	 * THE TWO NUMBERS ARE NOT THE SAME SCALE, so DO NOT FEED
	 * ONE INTO THE OTHER.  tachoRpm is real rpm and tops out
	 * at RegimeMax - 12000 on the stock bike - while the dial
	 * sweeps 0..16000.  Passed straight through, the needle
	 * parks at 12000/16000 = THREE QUARTERS of full sweep and
	 * never enters the red at all.
	 *
	 * Measured against a paused MOTO at the same 179 km/h in
	 * gear 3: both racers held tachoRpm 10913/10907, but
	 * MOTO's displayed value was 14550 and ours 10907.
	 * 10913 / 12000 * 16000 = 14550.67.
	 */
	{
		const MR_BikeCarac *bc = bike_at(r->bikeIndex);

		tacho = r->tachoRpm;
		if (bc && bc->regimeMax > 0.0f)
			tacho = (int)((double)tacho / (double)bc->regimeMax
		                  * (double)HUD_COUNTER_MAX_RPM);
	}
	/*
	 * SLEW LIMITED, and this is the only damping on either needle -
	 * see hud-counter.txt 6.1. Without it a gear change throws the
	 * needle across the dial in a single frame.
	 */
	d = tacho - s_tachoShown;
	if (d >  HUD_TACHO_SLEW) d =  HUD_TACHO_SLEW;
	if (d < -HUD_TACHO_SLEW) d = -HUD_TACHO_SLEW;
	s_tachoShown += d;
	if (s_tachoShown < 0)
		s_tachoShown = 0;

	a = hud_dial_angle(&s_dialTacho, s_tachoShown);

	hud_needle_point(&s_tacVerts[0], &s_dialTacho, a,
	                 -2 * (1 + hi), 2 * hi + 2, 0);
	hud_needle_point(&s_tacVerts[1], &s_dialTacho, a,
	                 (int)s_dialTacho.radius, 0, 0);
	hud_needle_point(&s_tacVerts[2], &s_dialTacho, a,
	                 -2 * (1 + hi), -2 * (1 + hi), 0);

	for (i = 0; i < 3; i++)
		hud_shadow_of(&s_tacVerts[3 + i], &s_tacVerts[i], L);

	hud_face_triangle(&s_tacPrims[0], HUD_NEEDLE_LIGHT,  0, 1, 2);
	hud_face_triangle(&s_tacPrims[1], HUD_NEEDLE_SHADOW, 0, 1, 2);

	queue_primitive(&s_tacPrims[0], HUD_NEEDLE_Z, s_tacVerts,     0, 0);
	queue_primitive(&s_tacPrims[1], HUD_SHADOW_Z, s_tacVerts + 3, 0, 0);
}

/*
 * The digits. Into the OVERLAY layer, for the reason hud_menu_draw
 * gives: the 2D layer goes down before the geometry, so anything put
 * there sits behind whatever is near the camera.
 *
 * THE TWO FACES ARE THE ORIGINAL'S. Font_Get(0x10) is the speed
 * and Font_Get(0x17) the gear - SpeedLcdLowRes and MotorLcdLowRes,
 * loaded by hud_counter_font_load, not the escape menu's row font.
 * The gear digit uses the original's own glyph set at gear + 0x37.
 *
 * [open] WHETHER THE READOUT'S YELLOW DROP SHADOW IS THERE IS
 * UNVERIFIED. It is a property of the face rather than anything
 * this code draws, so loading the real face should bring it -
 * but confirming that needs the game on screen.
 */
static void hud_counter_draw_digits(void)
{
	const MR_Racer          *r    = race_viewed_racer();
	const HUD_CounterLayout *L;
	int   hi   = hud_counter_hi();
	int   type = hud_counter_type();
	int   sx;
	int   sy;
	char  text[16];

	if (type == 0 || !r)
		return;

	hud_counter_font_load();
	if (!s_counterFontOk)
		return;

	if (hi != s_counterHi || type != s_counterType)
		hud_counter_layout(hi, type);
	hud_counter_origin(hi);

	L  = &kCounterLayout[hi ? 1 : 0];
	sx = (type == 1) ? L->shiftX : 0;
	sy = (type == 1) ? L->shiftY : 0;

	/*
	 * LEFT ALIGNED AND TOP ANCHORED, both readouts, and that is
	 * VERIFIED rather than assumed.
	 *
	 * The original passes flags 0x21 to Font_DrawTextInternal, which
	 * hands them to Font_ApplyAlignment (0x00492c20): the horizontal
	 * nibble is flags & 0x1f and the vertical is flags & 0xe0, and
	 * 0x21 gives 1 and 0x20 - neither of which matches any case, so
	 * both coordinates pass through untouched. That function's own
	 * guard proves it is deliberate: it skips Font_MeasureText for
	 * exactly the (0x1f) == 1 && (0xe0) == 0x20 combination, the
	 * one case where nothing needs measuring.
	 *
	 * SO THE SPEED READOUT IS NOT RIGHT JUSTIFIED. It grows
	 * rightward from the authored x. Right justification is what
	 * h == 8 or 0x10 does, and nothing here asks for it.
	 *
	 * NOTE Font_ApplyAlignment IS NOT Text_ApplyAlignment
	 * (0x00417900, hud_align above). Two schemes, two functions:
	 * this one serves Font_DrawText, that one serves the menu
	 * widgets, and their bit layouts disagree.
	 */
	/*
	 * "%3d", NOT "%d" - the format string at 0x004f10e0, read out
	 * of the running game.
	 *
	 * THAT IS WHERE THE RIGHT JUSTIFICATION LIVES. The alignment
	 * flag is 0x21, which Font_ApplyAlignment leaves alone (see
	 * above); the readout right-aligns because printf pads it to
	 * three characters and SPEEDLCD's SPACE GLYPH IS 16 PIXELS
	 * WIDE - the same as every digit. So a one-digit speed draws
	 * two blank LCD cells and then the digit, and the number grows
	 * leftward out of a fixed three-cell field.
	 *
	 * The face covers 0x20..0x39 for exactly this reason: 0x21 to
	 * 0x2f are one-pixel filler, and the only glyphs that matter
	 * are the space and the ten digits.
	 */
	sprintf(text, "%3d", hud_counter_speed(r->speedKmhNeedle));
	font_draw_text(&s_speedFont,
	               s_counterOx + L->speedTextX - sx,
	               s_counterOy + L->speedTextY - sy, text, 0);

	/*
	 * THE GEAR TEXT IS NOT SHIFTED, AND THAT IS NOT AN OVERSIGHT.
	 *
	 * The type-1 block at 0x00484b74 subtracts the shift from the
	 * SPEED text (0x005f14e8/ec) and from the six dial points on
	 * the stack, and touches 0x005f1500/04 - the gear - not at all
	 * [verified, six SUBs and no seventh].
	 *
	 * The effect is that the gear digit lands just below and right
	 * of the SHIFTED tacho hub, which is where it belongs: inside
	 * the small dial. Subtracting the shift here as well threw it
	 * 61 pixels left and 116 up at full size, and 154 left in the
	 * half-size layout, where the shift X is bigger.
	 *
	 * gear + '7' is the original's, at 0x00485073. MotorLcd holds
	 * fourteen glyphs from 0x30, so 0x37 upward is the second half
	 * of the face.
	 */
	text[0] = (char)('7' + r->gear);
	text[1] = 0;
	font_draw_text(&s_gearFont,
	               s_counterOx + L->gearTextX,
	               s_counterOy + L->gearTextY, text, 0);
}

/*
 * The rectangle the digits occupy, so main.cpp can put them back on
 * top of the geometry. The whole bottom-right corner rather than a
 * measured box: the composite is one quad either way, and a rect
 * that grew and shrank with the digit count would shimmer.
 */
int hud_counter_overlay_rect(int *x, int *y, int *w, int *h)
{
	const MR_Racer          *r    = race_viewed_racer();
	const HUD_CounterLayout *L;
	int  hi   = hud_counter_hi();
	int  type = hud_counter_type();
	int  sx, sy;
	int  x0, y0, x1, y1;
	int  gx, gy;
	char text[16];

	if (type == 0 || !r || !s_counterFontOk)
		return 0;

	hud_counter_origin(hi);
	L  = &kCounterLayout[hi ? 1 : 0];
	sx = (type == 1) ? L->shiftX : 0;
	sy = (type == 1) ? L->shiftY : 0;

	/*
	 * MEASURED, NOT THE WHOLE CORNER, and it has to be both.
	 *
	 * THE GEAR DIGIT SITS ABOVE THE ORIGIN under type 1, so a rect
	 * starting at the cluster origin shears its top off. And a rect
	 * running to the frame edge makes the composite quad sample
	 * stale opaque texels out of the shared 2D-layer texture, which
	 * draws a faint rectangle around the whole corner.
	 *
	 * Two small boxes unioned is correct and much less to
	 * composite.
	 */
	sprintf(text, "%3d", hud_counter_speed(r->speedKmhNeedle));
	x0 = s_counterOx + L->speedTextX - sx;
	y0 = s_counterOy + L->speedTextY - sy;
	x1 = x0 + font_text_width(&s_speedFont, text);
	y1 = y0 + font_height(&s_speedFont);

	text[0] = (char)('7' + r->gear);
	text[1] = 0;
	gx = s_counterOx + L->gearTextX;
	gy = s_counterOy + L->gearTextY;
	if (gx < x0) x0 = gx;
	if (gy < y0) y0 = gy;
	if (gx + font_text_width(&s_gearFont, text) > x1)
		x1 = gx + font_text_width(&s_gearFont, text);
	if (gy + font_height(&s_gearFont) > y1)
		y1 = gy + font_height(&s_gearFont);

	x0 -= HUD_COUNTER_PAD;  y0 -= HUD_COUNTER_PAD;
	x1 += HUD_COUNTER_PAD;  y1 += HUD_COUNTER_PAD;
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > g_clipWidth)  x1 = g_clipWidth;
	if (y1 > g_clipHeight) y1 = g_clipHeight;
	if (x1 <= x0 || y1 <= y0)
		return 0;

	if (x) *x = x0;
	if (y) *y = y0;
	if (w) *w = x1 - x0;
	if (h) *h = y1 - y0;
	return 1;
}

/* ================================================================== */
/* THE TIME BANNER AND ITS COUNTDOWN, upper right.                    */
/*                                                                    */
/* ORIGIN  FUN_00482380 @ 0x00482380, the first half                  */
/*                                                                    */
/* See docs/subsystems/hud-race.txt 5.2. The second half of that      */
/* function is the POS block, which waits on                          */
/* maybe_Race_FinishingPosition.                                      */
/* ================================================================== */

/*
 * THE BANNER IS ONE GLYPH. "TIME" is character 'D' of the Sprites
 * face - a Pal font whose characters are whole words, 35 of them
 * from 'A'. Nothing in the binary spells the word, which is why it
 * cannot be found by searching for it. hud-race.txt 3 has the sheet.
 */
#define HUD_SPR_TIME      "D"

/* Insets from the RIGHT edge and rows from the top, 0x004823f4 and
 * 0x00482419. Flag 8 is right-aligned; see font_draw_text_aligned. */
#define HUD_TIME_BANNER_X 8
#define HUD_TIME_BANNER_Y 0x5a
#define HUD_TIME_DIGITS_X 8
#define HUD_TIME_DIGITS_Y 0x73
#define HUD_ALIGN_RIGHT   8

/*
 * THE COLOUR TABLE IS AT 0x004f0dd0, twelve entries of four bytes,
 * RGB with a pad. The original passes them BY ADDRESS, so a comment
 * citing 0x004f0de8 means yellow rather than an index [verified]:
 *
 *     dd0 black   dd4 WHITE   dd8 black   ddc grey
 *     de0 cyan    de4 magenta de8 YELLOW  dec RED
 *     df0 blue    df4 GREEN   df8 19,188,5 dfc ORANGE
 *
 * Two entries are black and the port cannot tell which slot a call
 * meant; both give the same pixels. The six named below are the ones
 * this file uses.
 */
static const unsigned char kHudWhite[3]  = { 255, 255, 255 };
static const unsigned char kHudBlack[3]  = {   0,   0,   0 };
static const unsigned char kHudYellow[3] = { 255, 255,   0 };
static const unsigned char kHudRed[3]    = { 255,   0,   0 };
static const unsigned char kHudGreen[3]  = {   0, 255,   0 };
static const unsigned char kHudOrange[3] = { 255, 160,   0 };


/* The blink counter, 0x004f0e1c. Advanced once per drawn frame. */
static unsigned int s_hudBlink;

static MR_Font s_sprFont;      /* SpritesHiRes, the banners  */
static MR_Font s_timeFont;     /* TimeHiRes, the digits      */
static int     s_raceFontOk;
static int     s_raceFontTried;
static int     s_raceFontSmall = -1;

static void hud_race_font_load(void)
{
	/*
	 * RELOADED WHEN SmallFonts MOVES. The original does not latch
	 * at all - Hud_DrawCounter and the race HUD call Font_Get every
	 * frame and it answers with whichever twin the flag selects, so
	 * Ctrl+F swaps the faces live. Ours loads once and would
	 * otherwise keep the wrong pair, which is the same class of
	 * bug as the unhalved rectangle: everything simply vanishes.
	 */
	if (s_raceFontTried && s_raceFontSmall == g_options.smallFonts)
		return;
	if (s_raceFontTried && s_raceFontOk) {
		font_free(&s_sprFont);
		font_free(&s_timeFont);
		s_raceFontOk = 0;
	}
	s_raceFontTried = 1;
	s_raceFontSmall = g_options.smallFonts;

	s_raceFontOk =
	    fontdef_open(&s_sprFont, HUD_FONT_SPRITES) &&
	    fontdef_open(&s_timeFont, HUD_FONT_TIME);

	if (!s_raceFontOk)
		mrlog_err("hud: race HUD fonts did not load\n");
	else
		mrlog("hud: race fonts - sprites %d tall, time %d tall\n",
	          font_height(&s_sprFont), font_height(&s_timeFont));
}

/*
 * ORIGIN  FUN_00413410 @ 0x00413410
 *
 * THE RACE HUD IS GATED ON g_bPreRace AS A WHOLE, one branch
 * around the entire dispatcher rather than a test inside each
 * block:
 *
 *     if (g_bPreRace) { FUN_00413030(...); return; }
 *     FUN_00413180(...);
 *
 * FUN_00413180 is what draws TIME, POS, the lap list and the
 * network scoreboard, so NONE of them appear while the grid
 * counts down. FUN_00413030 draws the laps banner, the falling
 * counter and GET READY instead, plus FUN_00482830 case 1 - best
 * lap, the one block common to both.
 *
 * o SO A POSITION READ DURING THE COUNTDOWN IS NEVER SEEN. DO NOT
 *   DIAGNOSE A STANDINGS FAULT FROM ONE: retail has not started
 *   drawing the block yet, so whatever the field holds there is
 *   never on screen.
 *
 * The pre-race arm IS transcribed - hud_draw_countdown and
 * hud_draw_go, called from hud_draw, with hud_draw_laps_banner for
 * FUN_00482220's N LAPS. The digits FADE rather than drop, and that
 * is correct: nothing Widget_ShowText builds ever moves. See the
 * note above the message-widget struct.
 */
static int hud_race_hud_visible(void)
{
	const MR_RaceClock *c = race_clock();

	return c != NULL && !c->preRace;
}

/*
 * ORIGIN  Hud_DrawInRace @ 0x00413180, at 0x00413312
 *
 * WHICH OF THE TWO POSITION BLOCKS THE RACING ARM DRAWS, and it is a
 * MODE test rather than a layout preference:
 *
 *     !solo || g_nSelectedMode != 0 || g_nTimeAttack
 *         -> Hud_DrawTimeAndPos  0x00482380   TIME countdown AND POS
 *     else
 *         -> Hud_DrawPosOnly     0x00482620   POS alone, base y 0x55
 *
 * Mode 0 is PRACTICE in a solo game - menu.c page 5, the mode is the
 * button index minus one - so A PLAIN PRACTICE LAP GETS NO TIME
 * COUNTDOWN AT ALL. That is not cosmetic: the same mode test gates
 * the time limit itself in race_clock_tick, and a practice race is
 * never timed out. A HUD that keeps counting down in practice shows
 * a number nothing acts on.
 */
static int hud_time_and_pos(void)
{
	return (g_soloMode == 0 || g_selectedMode != 0 || g_timeAttack != 0);
}

/*
 * ORIGIN  Hud_DrawInRace @ 0x00413180, at 0x004132b4
 *
 * THE RACE IS OVER FOR THE VIEWED RACER, and this is a whole arm of
 * the dispatch rather than a detail:
 *
 *     raceState 2 or 3   finished, or timed out
 *         solo     FUN_00482830 - the BEST LAP block - and RETURN
 *         network  FUN_00482f00 - the scoreboard - and RETURN
 *     raceState 0 or 1   still racing, the position blocks
 *
 * THE RETURN IS THE POINT. Past the line there is no TIME countdown,
 * no POS and no lap list - the whole of the in-race readout goes
 * away and the best-lap block is left alone on screen. A port that
 * keeps drawing them shows a live race HUD over a bike that has
 * stopped racing, with a countdown nothing acts on.
 *
 * raceState 3 TAKES THE SAME ARM as 2. Timed out and finished look
 * identical here; what separates them is the standings bucket
 * (race_compute_standings) and the pose.
 */
static int hud_race_over(void)
{
	const MR_Racer *r = race_viewed_racer();

	if (!r)
		return 0;
	return (r->raceState != 0 && r->raceState != 1);
}

/*
 * THE PRACTICE LAYOUT, from the 0x55 the dispatch passes as
 * Hud_DrawPosOnly's fourth argument and the three offsets added to it
 * at 0x0048268b, 0x00482727 and 0x004826bf. Sixty pixels above the
 * timed layout, because there is no TIME block overhead to clear.
 *
 * These live up here rather than beside the draw because
 * hud_time_overlay_rect and hud_pos_overlay_rect both need them and
 * both come first.
 */
#define HUD_POSONLY_BASE_Y    0x55
#define HUD_POSONLY_BANNER_Y  (HUD_POSONLY_BASE_Y + 0x05)   /*  90 */
#define HUD_POSONLY_NUMBER_Y  (HUD_POSONLY_BASE_Y + 0x1e)   /* 115 */
#define HUD_POSONLY_FIELD_Y   (HUD_POSONLY_BASE_Y + 0x34)   /* 137 */

/* Both banner and field are drawn at x 8 here, not at the 0 the  */
/* timed layout uses; the number's x is computed - see the draw.  */
#define HUD_POSONLY_X         8
#define HUD_POSONLY_INSET     9

/* ================================================================== */
/* THE GRID COUNTDOWN - what the race draws while preRace is set.     */
/*                                                                    */
/* ORIGIN  Hud_DrawCountdown @ 0x00413030                             */
/*                                                                    */
/* See docs/subsystems/hud-race.txt 1 for the gate that selects this  */
/* over the in-race HUD, and in-race-menu.txt 6.2.3 for the widget    */
/* animation and visibility modes it drives.                          */
/* ================================================================== */

/*
 * A FLATTENED TEXT WIDGET, the same trade this file already makes
 * for the escape menu: the fields that matter, not the 0xc2-byte
 * object with its vtable.
 *
 * The original builds these through Widget_ShowText (0x004188b0) ->
 * Menu_DrawText (0x00417d20) -> Widget_Init (0x00416170), and they
 * live in a global array until something replaces them. Ours are two
 * statics, because Hud_DrawCountdown only ever has two.
 *
 * o NOTHING WIDGET_SHOWTEXT BUILDS EVER MOVES. Its fixed argument
 *   tail passes zero for the direction (+0x58/+0x5c), the velocity
 *   (+0x40) and the acceleration (+0x38), and a constant 0 reaches
 *   the animation style at +0x14 - so Widget_StepAnimation returns
 *   immediately for every one of them. The motion styles exist for
 *   the menu pages, which call Menu_DrawText directly.
 *
 *   THIS MATTERS BECAUSE THE COUNTDOWN LOOKS LIKE IT SHOULD DROP.
 *   The thing that actually falls is the LAPS BANNER - see the note
 *   on hud_draw_laps_banner - and the digits only fade.
 */
typedef struct {
	int          live;
	int          visMode;    /* +0x10 */
	int          fadeMs;     /* +0x8c */
	int          periodNeg;  /* +0x90 */
	int          periodPos;  /* +0x98 */
	unsigned int startMs;    /* +0x88 */
	unsigned int frames;     /* +0x94 */
	int          homeX;      /* +0x78 */
	int          homeY;      /* +0x7c */
	unsigned int flags;      /* +0x08, Text_ApplyAlignment's scheme */
	MR_Font     *font;
	char         text[24];

	/*
	 * THE MOTION HALF, used only by a widget built through
	 * Menu_DrawText directly - which in the race means the LAPS
	 * banner alone. Widget_ShowText zeroes all of it.
	 */
	int          fontId;     /* +0xb4 - decides the halving   */
	int          fontPaired; /* hud_font_has_twin(fontId)     */
	int          animStyle;  /* +0x14 */
	int          curX;       /* +0x70 */
	int          curY;       /* +0x74 */
	double       dirX;       /* +0x58 */
	double       dirY;       /* +0x5c */
	double       vel;        /* +0x40 */
	double       accel;      /* +0x38 */

	/*
	 * A SPRITE WIDGET DRAWS ART INSTEAD OF TEXT, and it is
	 * the same widget otherwise - same lifetime, same style
	 * promotion, same slide.  FUN_00418160 is Menu_DrawText's
	 * sibling and hands the identical argument tail to
	 * Widget_Init; only what gets blitted differs.
	 *
	 * NULL for every text widget.  See hud_widget_draw.
	 */
	MR_Sprite   *sprite;
} HUD_Widget;

static HUD_Widget s_cdNumber;    /* 0x004e98d0 in the original */
static HUD_Widget s_cdReady;     /* 0x004e98e8                 */

/*
 * The two faces the countdown needs. BOTH SHARE A FILE WITH A FONT
 * THIS FILE ALREADY LOADS AND NEITHER SHARES ITS CLASS, which is
 * why they get their own MR_Font rather than reusing one.
 *
 * Read live out of a running MOTO, off g_fontTable and each object's
 * name at +0x08 and face path at +0x5250:
 *
 *     id 12  Messages        AntiAlias8Transp  HiRes/Msg
 *     id 13  GameMsgLowRes   AntiAlias8Transp  LowRes/PosTime
 *     id 14  GameMsgHiRes    AntiAlias8Transp  HiRes/PosTime
 *
 * o SO THE COUNTDOWN DIGITS AND THE RACE CLOCK ARE THE SAME FACE IN
 *   DIFFERENT CLASSES. s_timeFont loads PosTime as a Direct font for
 *   the TIME readout; id 14 loads the same file as an
 *   AntiAlias8Transp one. The face supplies pixels and the font
 *   supplies colour, so one file legitimately serves both.
 *
 * o ID 14 CARRIES TWO FACES, NOT ONE - faceCount at +0x524c reads
 *   2, and the second path record is PosTimeL. POSTIME supplies
 *   the DIGITS and POSTIMEL the LETTERS, so a font loaded with
 *   only the first draws "4" where the game draws "4 LAPS" and
 *   nothing at all for a word. Id 12 is a single face.
 *
 * o THE STORED PATHS END IN .raw AND THE SHIPPED FILES ARE .FNT.
 *   The loader substitutes the extension - the ".FNT" literal sits
 *   in the g_fontTable statics block at 0x004f1840. A path record is
 *   an authoring-time name, not a member name.
 */
static MR_Font s_msgFont;      /* id 14/13, the digits */
static MR_Font s_bigMsgFont;   /* id 12, GET READY     */
static int     s_cdFontOk;
static int     s_cdFontTried;
static int     s_cdFontSmall = -1;

static void hud_countdown_font_load(void)
{
	if (s_cdFontTried && s_cdFontSmall == g_options.smallFonts)
		return;
	if (s_cdFontTried && s_cdFontOk) {
		font_free(&s_msgFont);
		font_free(&s_bigMsgFont);
		s_cdFontOk = 0;
	}
	s_cdFontTried = 1;
	s_cdFontSmall = g_options.smallFonts;

	/*
	 * ID 12 IS NoCondition, so Font_Get never swaps it and there is
	 * no LowRes twin to load - MSG.FNT exists only under HIRES.
	 */
	s_cdFontOk =
	    fontdef_open(&s_msgFont, HUD_FONT_MSG) &&
	    fontdef_open(&s_bigMsgFont, HUD_FONT_BIGMSG);

	if (!s_cdFontOk)
		mrlog_err("hud: countdown fonts did not load\n");
}

/*
 * ORIGIN  Widget_ShowText @ 0x004188b0 + Widget_Init @ 0x00416170,
 *         the fields a Widget_ShowText widget can actually carry
 *
 * The original destroys the widget registered to the owner key and
 * builds a new one; ours overwrites the struct, which is the same
 * thing when there is exactly one slot per key.
 *
 * o THE FADE DOWNGRADE IS THE ORIGINAL'S, 0x004188f8: a fade mode
 *   with no duration would divide by zero in TextWidget_Draw, so it
 *   is turned into mode 0 rather than guarded at the point of use.
 */
static void hud_widget_show(HUD_Widget *w, const char *text, int fadeMs,
                            unsigned int flags, int x, int y,
                            int visMode, MR_Font *font, int fontId, int period,
                            int animStyle, double dirX, double dirY,
                            double accel, double vel)
{
	if (fadeMs == 0 && (visMode == 1 || visMode == 2))
		visMode = 0;

	memset(w, 0, sizeof(*w));
	w->live    = 1;
	w->visMode = visMode;
	w->fadeMs  = fadeMs;
	w->flags   = flags;
	w->homeX   = x;
	w->homeY   = y;
	w->font    = font;
	w->fontId  = fontId;
	w->fontPaired = hud_font_has_twin(fontId);
	w->startMs = (unsigned int)(tm_now() * 1000.0);

	/* +0x90 or +0x98 by SIGN, Widget_Init at 0x0041627e. */
	if (period < 0)
		w->periodNeg = -period;
	else
		w->periodPos = period;

	strncpy(w->text, text, sizeof(w->text) - 1);
	w->text[sizeof(w->text) - 1] = 0;

	w->animStyle = animStyle;
	w->dirX      = dirX;
	w->dirY      = dirY;
	w->accel     = accel;
	w->vel       = vel;

	/*
	 * AN ANIMATED WIDGET IS ALIGNED ONCE, at creation, because the
	 * stepper accumulates into the SAME pair the draw reads -
	 * Widget_Init stores the post-alignment position into +0x70
	 * and Widget_StepAnimation adds to it. Re-aligning every frame
	 * would undo the motion every frame.
	 */
	w->curX = hud_scale_font(x, w->fontPaired);
	w->curY = hud_scale_font(y, w->fontPaired);
	if (font) {
		hud_align(font_text_width(font, w->text),
		          font_height(font), &w->curX, &w->curY, flags);
	}
}

/*
 * ORIGIN  TextWidget_Draw @ 0x00416da0, the visibility switch
 *
 * Returns 0 when the widget is invisible this frame, else fills
 * *alpha with the original's 0..100 intensity.
 *
 * o 0..100 WHERE HIGHER IS MORE TRANSPARENT, and 0 is opaque -
 *   the same direction the port's font_draw_text level runs, so
 *   nothing needs inverting.
 *
 *   THE EVIDENCE IS THAT "GO" HAS TO GO AWAY. It is visibility mode
 *   2 over 500 ms and nothing ever destroys its widget, so if a
 *   rising alpha meant rising opacity it would fade up and then sit
 *   on the screen for the rest of the race. Under this reading it
 *   flashes and clears, which is what the word is for. The LAPS
 *   banner fading as it accelerates off the bottom, and each
 *   countdown digit clearing as the next replaces it, follow the
 *   same way round.
 *
 *   Mode 0 leaves the value at 0, so "default intensity" and "fully
 *   opaque" are the same number and the mode needs no special case.
 *
 *   o AGAINST IT: modes 5, 6 and 7 are fixed 75, 50 and 25, which
 *     under this reading is a ramp from most to least transparent -
 *     a strange order to number that way. Nothing in the race uses
 *     those three, so the argument above wins. [open]
 */
static int hud_widget_alpha(HUD_Widget *w, int *alpha)
{
	unsigned int now = (unsigned int)(tm_now() * 1000.0);
	int          a   = 0;
	int          elapsed = (int)(now - w->startMs);

	/*
	 * SET BEFORE THE SWITCH, because the blink arms return out of
	 * it without ever assigning one - exactly as TextWidget_Draw
	 * leaves its local_4 at the 0 it was initialised to. Zero is
	 * full intensity, so a blinking widget is either fully drawn
	 * or not drawn at all; it does not fade.
	 *
	 * LEAVING IT TO A TAIL ASSIGNMENT LOSES THE BLINK MODES
	 * ENTIRELY. The caller then scales whatever its uninitialised
	 * local held, which can land past the blitter's 0xe1 cutoff and
	 * drop every pixel - so a mode-3 widget vanishes while a
	 * mode-2 one beside it draws fine.
	 */
	*alpha = 0;

	switch (w->visMode) {
	case 1:                                  /* fade OUT over +0x8c */
		if (w->fadeMs > 0)
			a = 100 - (elapsed * 100) / w->fadeMs;
		else if (w->periodNeg > 0)
			a = (1 - elapsed % w->periodNeg) * 100;
		break;
	case 2:                                  /* fade IN over +0x8c  */
		if (w->fadeMs > 0)
			a = (elapsed * 100) / w->fadeMs;
		else if (w->periodNeg > 0)
			a = (elapsed % w->periodNeg) * 100;
		break;
	case 3:                                  /* BLINK               */
		if (w->periodNeg == 0) {
			return (int)((w->frames & (unsigned int)w->periodPos)
			             != (unsigned int)w->periodPos);
		}
		return (elapsed % (w->periodNeg * 2)) < w->periodNeg;
	case 4: {                                /* ping-pong intensity */
		int per = w->periodNeg ? w->periodNeg : 500;
		int t   = elapsed % (per * 2);

		if (t > per)
			t = per * 2 - t;
		a = ((t << 4) / per) * 100 >> 4;
		break;
	}
	case 5: a = 75; break;
	case 6: a = 50; break;
	case 7: a = 25; break;
	default: a = 0; break;                   /* mode 0, full        */
	}

	/*
	 * NOT CLAMPED, and that is deliberate. A fade that runs past
	 * its duration keeps going, the scaled bias passes the
	 * blitter's 0xe1 cutoff, and every pixel is dropped - so the
	 * text stays GONE instead of settling at nearly-invisible.
	 * That is what finally clears GO, which nothing destroys.
	 */
	*alpha = a;
	return 1;
}

/*
 * ORIGIN  Widget_StepAnimation @ 0x00416b40, style 2 at 0x00416d5d
 *
 * Read off the FPU ops rather than the decompiler, which loses
 * which doubles the two __ftol calls consume:
 *
 *     vel   += accel
 *     curX   = (int)(curX + dirX * vel)
 *     curY   = (int)(curY + dirY * vel)
 *
 * o dirX/dirY ARE A DIRECTION HERE AND A DESTINATION IN STYLE 1.
 *   The same pair at +0x58/+0x5c means different things depending
 *   on +0x14, and there is no arrival test in style 2 - it
 *   accelerates away until the widget is destroyed.
 *
 * [open] STYLE 1, the polar seek the escape menu rows use, is not
 * implemented - nothing in the race needs it. It is written out in
 * docs/subsystems/in-race-menu.txt 6.2.3 when it is wanted.
 */
static void hud_widget_step(HUD_Widget *w)
{
	if (w->animStyle != 2)
		return;

	w->vel += w->accel;
	w->curX = (int)(w->curX + w->dirX * w->vel);
	w->curY = (int)(w->curY + w->dirY * w->vel);
}

/*
 * ONCE PER FRAME, NOT ONCE PER DRAW, AND THE TWO PASSES MAKE IT
 * MANDATORY.
 *
 * hud_draw renders the overlay TWICE, over black and over white,
 * and reads the DIFFERENCE between the two as coverage. So ANYTHING
 * THAT CHANGES BETWEEN THE PASSES IS READ AS ALPHA: a blink that
 * toggles draws in one pass and not the other, and the difference
 * comes out as the whole glyph being transparent; a widget that
 * steps between them draws in two places.
 *
 * The original has no such constraint - Widget_StepAnimation is
 * called from TextWidget_Draw, because it draws once. Ours splits
 * the two so both passes see identical state.
 */
static void hud_widget_tick(HUD_Widget *w)
{
	if (!w->live)
		return;
	w->frames++;

	/*
	 * THE STYLE PROMOTION, AND IT IS WHAT MAKES A BANNER
	 * LEAVE THE SCREEN.
	 *
	 * ORIGIN  Widget_TickAll @ 0x00418790, at 0x004187f0
	 *
	 * Widget_StepAnimation moves styles 1 and 2 only, so 4
	 * reads as 'never moves' from the stepper alone.  It is
	 * not: when the lifetime runs out the tick REWRITES the
	 * style, and only then does the widget start sliding.
	 *
	 *     3 -> 1   restart the clock, fly to a target
	 *     4 -> 2   accelerate away along dir
	 *
	 * So style 4 means SIT STILL, THEN LEAVE - which is what
	 * GAME OVER and TIMEOUT both do, motionless for their
	 * whole duration and then out to the right.
	 *
	 * THE DEFAULTS ARE THE ORIGINAL'S: no direction means
	 * DOWNWARD, no velocity means 1.0.  Both are filled in
	 * here rather than at creation, so a caller that passes
	 * neither still gets motion.
	 *
	 * ZEROING THE DURATION is what stops it promoting twice,
	 * and it also takes the widget out of reach of the
	 * mode-2 retire below.
	 */
	if (w->fadeMs > 0) {
		unsigned int now = (unsigned int)(tm_now() * 1000.0);

		if ((int)(now - w->startMs) > w->fadeMs) {
			if (w->animStyle == 3) {
				/* [open] style 1 is the polar seek and
				   hud_widget_step does not implement it, so
				   this promotion currently parks the widget.
				   Nothing in the race uses style 3. */
				w->startMs   = now;
				w->animStyle = 1;
			} else if (w->animStyle == 4) {
				w->fadeMs = 0;
				if (w->dirX == 0.0 && w->dirY == 0.0)
					w->dirY = 1.0;
				if (w->vel == 0.0)
					w->vel = 1.0;
				w->animStyle = 2;
			}
		}
	}

	hud_widget_step(w);

	/*
	 * A FINISHED MODE 2 FADE RETIRES ITSELF. Past its duration
	 * the alpha keeps climbing, the scaled bias passes the
	 * blitter's 0xe1 cutoff and every pixel is dropped, so the
	 * widget is drawing nothing - dropping it here is what the
	 * screen already shows, and it lets the overlay rectangle
	 * close.
	 *
	 * MODE 1 IS NOT RETIRED: its alpha falls toward 0, so it
	 * gets MORE visible with time, not less.
	 */
	if (w->visMode == 2 && w->fadeMs > 0) {
		unsigned int now = (unsigned int)(tm_now() * 1000.0);

		if ((int)(now - w->startMs) > w->fadeMs)
			w->live = 0;
	}

	/*
	 * AND A SLIDING WIDGET DIES WHEN IT IS FULLY OUTSIDE.
	 *
	 * ORIGIN  Widget_IsAlive @ 0x00416a60, its four bounds
	 *         tests - the caller destroys the slot when they
	 *         say no.
	 *
	 * NOTHING ELSE STOPS IT.  Style 2 has no arrival test and
	 * the promotion zeroed the duration, so without this the
	 * banner accelerates away for ever, holding its slot and
	 * its overlay rectangle.
	 */
	if (w->live && w->animStyle == 2) {
		int tw = w->sprite ? w->sprite->width
		       : ((w->font && w->text[0])
		             ? font_text_width(w->font, w->text) : 0);
		int th = w->sprite ? w->sprite->height
		       : (w->font ? font_height(w->font) : 0);

		if (w->curX + tw < 0 || w->curX >= g_clipWidth ||
		    w->curY + th < 0 || w->curY >= g_frameHeight)
		    w->live = 0;
	}
}

/*
 * ORIGIN  Font_DrawTextInternal @ 0x00493820, at 0x00493d47
 *
 * THE 0..100 ALPHA IS SCALED BY FONT KIND before it reaches the
 * blitter, and the three scales have nothing in common:
 *
 *     kind 5  AntiAlias8        (a << 11)    / 100 >> 8    0..8
 *     kind 6  AntiAlias8Transp  (a * 0xe000) / 100 >> 8    0..224
 *     hardware renderer         (a * 0x3f00) / 100 >> 8    0..63
 *
 * o THE RANGES ARE THE BLITTERS', NOT A COINCIDENCE.
 *   blit_shaded_transp adds the bias to the glyph byte's top three
 *   bits and drops the pixel at 0xe1, so 224 is exactly "fully
 *   transparent" for kind 6. Feeding it a raw 0..100 - or worse a
 *   0..7, which is AntiAlias8's range and not this one - collapses
 *   the whole fade into the first few per cent of its duration.
 *
 * o THE HARDWARE ARM IS NOT OURS. It is chosen by g_rendererReady
 *   and g_rendererMode, which select the D3D immediate-mode path;
 *   this port rasterises into a framebuffer and presents that, so
 *   the software arm is the right one. Written out anyway, because
 *   a future back end will have to decide. [open]
 *
 * o EVERY OTHER KIND PASSES THROUGH UNSCALED, harmlessly: what is
 *   left is Direct and Pal, which do not blend, so no blitter
 *   reads the bias.
 */
static int hud_widget_intensity(const MR_Font *f, int alpha)
{
	if (f->kind == 5)
		return ((alpha << 11) / 100) >> 8;
	if (f->kind == 6)
		return ((alpha * 0xe000) / 100) >> 8;
	return alpha;
}

static void hud_widget_draw(HUD_Widget *w)
{
	int x, y, tw, th, alpha, level;

	if (!w->live)
		return;

	/*
	 * THE SPRITE ARM, AND IT IGNORES THE ALPHA.
	 *
	 * A face blends with what is under it and a sprite does
	 * not: the blitter is a colour-keyed copy with no
	 * intensity argument anywhere in it.  Every sprite
	 * banner the race raises is visibility mode 0 anyway -
	 * FUN_00482120 passes 0 - so the value would be 0 even
	 * if there were somewhere to put it.
	 *
	 * INTO THE OVERLAY, LIKE THE TEXT.  hud_draw has
	 * redirected g_frameBuffer by the time this runs, and
	 * an opaque copy is read correctly by the two-pass
	 * difference: a pixel written lands identically over
	 * black and over white, so the difference is zero and
	 * the compositor reads it as full coverage.  A keyed-out
	 * pixel keeps the fill and reads as transparent.
	 */
	if (w->sprite) {
		w->sprite->x = w->curX;
		w->sprite->y = w->curY;
		sprite_draw(w->sprite, SPRITE_MODE_KEYED, 100, 0);
		return;
	}

	if (!w->font || !w->text[0])
		return;
	if (!hud_widget_alpha(w, &alpha))
		return;

	if (w->animStyle != 0) {
		x = w->curX;
		y = w->curY;
	} else {
		tw = font_text_width(w->font, w->text);
		th = font_height(w->font);
		x  = hud_scale_font(w->homeX, w->fontPaired);
		y  = hud_scale_font(w->homeY, w->fontPaired);
		hud_align(tw, th, &x, &y, w->flags);
	}

	/*
	 * THE COLOURS ARE FIXED FOR EVERY WIDGET_SHOWTEXT WIDGET - the
	 * helper's argument tail passes &DAT_004f0dec and &DAT_004f0dd8
	 * unconditionally, and TextWidget_Draw saves the font's own
	 * pair, sets these, draws and restores. Read live off the
	 * colour table at 0x004f0dd4: RED on BLACK.
	 *
	 * [open] Hud_DrawCountdown calls Font_SetForeColour(245,255,12)
	 * and Font_SetBackColour(200,200,0) on pFontGameMsg immediately
	 * before putting GET READY up, and that save-and-restore throws
	 * both away. Either they are vestigial or something else reads
	 * them; transcribed as the code runs rather than as the call
	 * suggests.
	 */
	font_set_colors(w->font, kHudRed, kHudBlack);

	level = hud_widget_intensity(w->font, alpha);
	font_draw_text(w->font, x, y, w->text, level);
}

/*
 * ORIGIN  Hud_DrawLapsBanner @ 0x00482220
 *
 * "%d LAPS", or "%d LAP" below two, centred at y 0x14 in font id
 * 0xe. The style index is the caller's, from the seconds left, and
 * it selects a fade and an animation together [verified, read off
 * the Menu_DrawText argument list]:
 *
 *     0   fade 0,    visMode 0, anim 0, period 0       static
 *     1   fade 0,    visMode 4, anim 0, period -1000   pulsing
 *     2   fade 4000, visMode 2, anim 2, period 0       falls away
 *
 * o THE BANNER IS THE ONLY THING IN THE COUNTDOWN THAT MOVES, and
 *   the numbers that move it are literals in the call: direction
 *   (0, 1) from arguments 17 and 18, and an acceleration that
 *   arrives as the two halves of a double, 0x9999999a and
 *   0x3fc99999 - exactly 0.2 - in arguments 23 and 24. The digits
 *   and GET READY go through Widget_ShowText and cannot move at all.
 *
 * o THE DIRECTION AND ACCELERATION ARE PASSED FOR EVERY STYLE. Only
 *   the animation index gates whether the stepper reads them, so
 *   they are literal here too rather than per-style.
 *
 * o IT IS REBUILT ONLY WHEN THE STYLE CHANGES, which is what makes
 *   the fall start once instead of restarting every frame. The
 *   original keeps the last style in 0x004f0e18 and destroys the
 *   widget when it differs; ours compares the same way.
 */
/* Font ids, as Widget_ShowText passes them. See hud_font_has_twin. */
#define HUD_FONT_MESSAGES 0x0c   /* Messages, HiRes\Msg      - unpaired */
#define HUD_FONT_GAMEMSG  0x0e   /* GameMsgHiRes, PosTime(+L) - paired   */

#define HUD_LAPS_ACCEL 0.2

static HUD_Widget s_cdLaps;          /* 0x004e9868 in the original */
static int        s_cdLapsStyle = -1;

static void hud_draw_laps_banner(int style)
{
	static const int kFadeMs[3]  = {    0,     0, 4000 };
	static const int kVisMode[3] = {    0,     4,    2 };
	static const int kAnim[3]    = {    0,     0,    2 };
	static const int kPeriod[3]  = {    0, -1000,    0 };

	int  laps = race_lap_count();
	char text[24];

	if (style < 0 || style > 2 || laps <= 0)
		return;

	if (style != s_cdLapsStyle) {
		s_cdLapsStyle = style;

		sprintf(text, laps < 2 ? "%d LAP" : "%d LAPS", laps);
		hud_widget_show(&s_cdLaps, text, kFadeMs[style], 0x22,
		                0, 0x14, kVisMode[style], &s_msgFont,
		                HUD_FONT_GAMEMSG, kPeriod[style], kAnim[style],
		                0.0, 1.0, HUD_LAPS_ACCEL, 0.0);
	}
}

/*
 * ORIGIN  Hud_CountdownSound @ 0x004082f0
 *
 * TWO SOUNDS PER STEP, not one: a beep always, and the spoken
 * digit only while n is below 4.  So a countdown that starts
 * higher ticks silently until it reaches three.
 *
 * IT DOES NOT GO THROUGH THE ANNOUNCER QUEUE.  Every other
 * spoken line is queued with a 500 ms delay; these play
 * straight through, which is what puts the digits ON the beat
 * instead of behind it.
 *
 * EDGE-GATED TWICE, and both gates are the original's - the
 * caller on the displayed number and this one on
 * DAT_00504564.  The second is kept because it is what makes
 * this safe to call from anywhere, not because the caller
 * needs it today.
 *
 * THE VOICES SETTING SILENCES THE DIGITS AND LEAVES THE
 * BEEPS, because the ids fall either side of the 0x2f..0x5c
 * speech range and audio_cue routes on that alone.  Nothing
 * here has to test the setting.
 */
static int s_cdSoundLast = -1;

static void hud_countdown_sound(int n)
{
	int beep;
	int voice;

	if (n == s_cdSoundLast)
		return;
	s_cdSoundLast = n;

	switch (n) {
	case 3:  beep = SND_BIP;    voice = SND_VOICE_THREE; break;
	case 2:  beep = SND_BIP;    voice = SND_VOICE_TWO;   break;
	case 1:  beep = SND_BIP;    voice = SND_VOICE_ONE;   break;
	case 0:  beep = SND_BIP_GO; voice = SND_VOICE_GO;    break;
	default:
		/*
		 * THE ORIGINAL HAS NO DEFAULT and reads an
		 * uninitialised beep id here.  It never reaches it -
		 * Hud_DrawCountdown only calls with 1..4 - but the
		 * hole is real, so this arm exists rather than
		 * reproducing a read of whatever was on the stack.
		 */
		return;
	}

	audio_cue(beep, 0);
	if (n < 4)
		audio_cue(voice, 0);
}


/*
 * THE TIME OUT BANNER.
 *
 * ORIGIN  FUN_00481af0 @ 0x00481af0, its Menu_DrawText arm
 *
 * THE HELPER HAS TWO BRANCHES and the one the game takes is
 * NOT the sprite one: DAT_004dc180 reads 1 live, which picks
 * Menu_DrawText with mode 3 and style 4.  The sprite branch
 * is style 0 and would never move, so reading that one gives
 * a static banner and the wrong answer.
 *
 * IT IS ONE CHARACTER IN THE SPRITES FACE.  The original
 * builds a two-byte string holding 99 - 'c' - and that glyph
 * IS the words TIME OUT, 152 wide.  See hud-race.txt 3 for
 * the sheet; the same trick gives WRONG WAY, FINAL LAP and
 * CHECK POINT as '`', 'a' and 'b'.
 *
 * STYLE 4 - it sits still for its 2000 ms and only then
 * slides, which is hud_widget_tick's promotion rather than
 * anything here.  Velocity 2.0, acceleration 5.0.
 *
 * [inferred] THE DIRECTION.  GAME OVER's is (2, 0) and is
 * read straight out of its call; this one's arguments run
 * through Menu_DrawText, whose signature is not mapped.  The
 * observed slide is to the RIGHT and the sibling banner is
 * (2, 0), so that is what this uses.
 *
 * THE BLINK MASK IS 2 AND IT IS TIED TO THE FRAME RATE,
 * not to time.  Widget_Init's param_8 lands in +0x98 with
 * +0x90 left at 0, which is the arm of the mode-3 test that
 * counts FRAMES: on while (counter & mask) != mask, so mask 2
 * is two frames on and two off.  A mask of 1 toggles every
 * frame instead, which at any modern frame rate reads as a
 * dimmer banner rather than a blinking one.
 *
 * IT WILL NOT RESTART ITSELF - the original checks
 * Widget_QueryById first and builds nothing while its widget
 * is up, which is what stops the blink resetting on every
 * frame of the 500 ms window.
 */
static HUD_Widget s_timeoutBanner;

void hud_banner_timeout(void)
{
	hud_font_load();
	if (!s_fontOk)
		return;
	if (s_timeoutBanner.live)
		return;

	hud_widget_show(&s_timeoutBanner, "c", 2000, 0x42, 0, 0,
	                3, &s_sprFont, HUD_FONT_SPRITES, 2,
	                4, 2.0, 0.0, 5.0, 2.0);
}



/* ================================================================== */
/* THE GAME OVER BANNER - a SPRITE, where TIME OUT is a glyph.        */
/* ================================================================== */

/*
 * ORIGIN  FUN_00482120 @ 0x00482120, its param_1 == 0 arm,
 *         raised from maybe_Race_TickClock at 0x0047dec0
 *
 * THE CLOCK RUNS OUT AND THREE THINGS HAPPEN IN ORDER, all in
 * eight instructions: DAT_0052c238 goes to 1, this banner is
 * built, and Snd_QueueVoice(0x39, 500) speaks GAME OVER.  The
 * banner comes FIRST, which is why race.c raises it above the cue.
 *
 * IT IS A SPRITE AND THE SIBLING BANNER IS NOT, and the reason is
 * in the registry rather than in either banner.  FUN_00480930
 * branches on DAT_004dc180, which reads 1 live, and that branch
 * registers exactly TWO sprites - Game_over_LR and GameOver2_HR.
 * Every other in-race banner - TIME OUT, WRONG WAY, FINAL LAP,
 * CHECK POINT - is registered only in the other branch, so at 1
 * their keys resolve to nothing and those banners MUST come out of
 * the Sprites face instead.  See hud_banner_timeout.
 *
 * THE RECTS ARE maybe_DdBank_AddSprite ARGUMENTS 3..6, and they
 * are (x0, y0, x1, y1) inclusive [verified, 0x00480930]:
 *
 *     Game_over_LR    Texts\LowRes.RAW     (0, 15)-(255,127)
 *     GameOver1_HR    Texts\GameOver.RAW   (0, 89)-(255,255)
 *     GameOver2_HR    Texts\HiRes.RAW      (0, 98)-(247,113)
 *
 * WHICH ONE IS THE SAME TEST THE COUNTER AND THE FONTS MAKE.
 * FUN_00418160 splits on g_cfgScreenMode == 2 && !g_cfgSmallFonts
 * and reads a different member of each element for the key; the
 * elements this call passes are
 *
 *     { Game_over_LR, GameOver1_HR, 0, 0 }
 *     { 0,            GameOver2_HR, 0, 0 }
 *
 * so the low branch draws ONE sprite - element 1 keys to 0, which
 * maybe_DdBank_FindSprite does not resolve - and the high branch
 * draws two.
 *
 * [open] THE SECOND HIGH-RESOLUTION SPRITE.  Element 1 is a
 * 248x16 strip out of HiRes.RAW and its placement is decided by
 * FUN_00416430, which is not read; both elements pass offset
 * (0, 0), and FUN_00418160 sizes the pair as the SUM of the two
 * widths and the SUM of the two heights, which cannot be where
 * they land.  Element 0 is drawn in both modes; the strip is not.
 */

/* A free slot: the menu takes 0, 1 and 3, the race 8..30. */
#define HUD_BANNER_BANK   44

#define HUD_GO_LR_PAGE  "DESIGN\\TEXTS\\LOWRES.RAW"
#define HUD_GO_LR_X0    0
#define HUD_GO_LR_Y0    15
#define HUD_GO_LR_X1    255
#define HUD_GO_LR_Y1    127

#define HUD_GO_HR_PAGE  "DESIGN\\TEXTS\\GAMEOVER.RAW"
#define HUD_GO_HR_X0    0
#define HUD_GO_HR_Y0    89
#define HUD_GO_HR_X1    255
#define HUD_GO_HR_Y1    255

static MR_Sprite s_goSprite;
static int       s_goArtOk;
static int       s_goArtTried;

/*
 * Cut the banner out of its page ONCE and keep it.
 *
 * THE PAGE IS 8-BIT PALETTED AND THE SPRITE IS NOT.  The .RAW
 * carries its own 256-entry palette - "mhwanh", a 32-byte header,
 * big-endian dimensions - and bank_load leaves the indices in the
 * page with the palette beside them, one byte per pixel into a
 * 512-byte row.  sprite_draw wants RGB565, so the resolve happens
 * here rather than per frame.
 *
 * MODE 0, because this page has no numbered palette siblings and
 * mode 1 would cost seven failed archive lookups to find that out.
 * The mode-0 truecolour skip cannot fire - the file is paletted.
 *
 * ORIGIN  NONE - ours. The RECTS are maybe_DdBank_AddSprite's
 *         arguments 3..6 (see hud_banner_gameover's block), but
 *         the cut and the palette resolve are the port's: the
 *         original hands the page to DirectDraw and blits from a
 *         surface, where this owns a 16-bit copy.
 */
static int hud_banner_art(void)
{
	const char           *page;
	const MR_Bank        *b;
	const unsigned short *pal;
	const unsigned char  *idx;
	unsigned short       *dst;
	int                   x0, y0, x1, y1, w, h, x, y;

	if (s_goArtTried)
		return s_goArtOk;
	s_goArtTried = 1;

	/* FUN_00418160 at 0x004182a7 - the same test hud_counter_hi
	   makes, written out rather than shared so the counter does
	   not read as the thing deciding this. */
	if (!g_options.smallFonts && g_options.screenMode == 2) {
		page = HUD_GO_HR_PAGE;
		x0 = HUD_GO_HR_X0;  y0 = HUD_GO_HR_Y0;
		x1 = HUD_GO_HR_X1;  y1 = HUD_GO_HR_Y1;
	} else {
		page = HUD_GO_LR_PAGE;
		x0 = HUD_GO_LR_X0;  y0 = HUD_GO_LR_Y0;
		x1 = HUD_GO_LR_X1;  y1 = HUD_GO_LR_Y1;
	}

	if (!bank_load(HUD_BANNER_BANK, page, 0))
		return 0;

	b = bank_get(HUD_BANNER_BANK);
	if (!b || !b->page || !b->isIndexed || b->paletteCount < 1) {
		mrlog_err("hud: %s is not a paletted page\n", page);
		return 0;
	}

	/* The rect is inclusive at both ends. */
	if (x1 >= b->width)   x1 = b->width  - 1;
	if (y1 >= b->height)  y1 = b->height - 1;
	w = x1 - x0 + 1;
	h = y1 - y0 + 1;
	if (w <= 0 || h <= 0)
		return 0;

	dst = (unsigned short *)malloc((size_t)w * h * sizeof(*dst));
	if (!dst)
		return 0;

	pal = b->palette[b->activePalette];
	for (y = 0; y < h; y++) {
		/* One byte per pixel into a 512-byte row - see bank.h. */
		idx = (const unsigned char *)b->page +
		      (size_t)(y0 + y) * (BANK_PAGE_W * 2) + x0;
		for (x = 0; x < w; x++)
			dst[(size_t)y * w + x] = pal[idx[x]];
	}

	s_goSprite.width  = w;
	s_goSprite.height = h;
	s_goSprite.bitmap = dst;
	s_goSprite.kind   = SPRITE_MODE_KEYED;
	s_goArtOk = 1;

	mrlog("hud: game over banner %dx%d from %s\n", w, h, page);
	return 1;
}

/* ORIGIN  NONE - ours. hud_free calls it; the original's surface
 * goes with the bank. */
static void hud_banner_art_free(void)
{
	if (s_goSprite.bitmap) {
		free(s_goSprite.bitmap);
		s_goSprite.bitmap = NULL;
	}
	s_goArtOk    = 0;
	s_goArtTried = 0;
}

static HUD_Widget s_gameOverBanner;

/*
 * ORIGIN  FUN_00482120 @ 0x00482120, at 0x004821ac
 *
 * The argument tail is that call's own:
 *
 *     duration 3000     flags 0x42 - centred
 *     visibility 0      style 4    - sit still, then leave
 *     direction (2, 0)  velocity 2.0   acceleration 5.0
 *
 * The two doubles arrive as dword pairs, 0x40000000 and
 * 0x40140000 over zero low halves.
 *
 * ALIGNED ON THE SPRITE, NOT ON A STRING, and NOT SCALED: the art
 * is a fixed block of pixels, so hud_scale_font would shrink the
 * box that the blit still fills.  hud_align anchors against the
 * real frame, which is what centres it at any width.
 */
void hud_banner_gameover(void)
{
	int x, y;

	if (s_gameOverBanner.live)
		return;
	if (!hud_banner_art())
		return;

	hud_widget_show(&s_gameOverBanner, "", 3000, 0x42, 0, 0,
	                0, NULL, 0, 0,
	                4, 2.0, 0.0, 5.0, 2.0);

	x = 0;
	y = 0;
	hud_align(s_goSprite.width, s_goSprite.height, &x, &y, 0x42);
	s_gameOverBanner.curX   = x;
	s_gameOverBanner.curY   = y;
	s_gameOverBanner.sprite = &s_goSprite;
}

/*
 * The rectangle the two end-of-race banners occupy.
 *
 * THE WHOLE FRAME, for hud_countdown_overlay_rect's reason: both
 * are centred and both then accelerate off the RIGHT edge, so any
 * box tight enough to be worth computing would clip the slide.
 */
int hud_banner_overlay_rect(int *x, int *y, int *w, int *h)
{
	if (!s_timeoutBanner.live && !s_gameOverBanner.live)
		return 0;

	if (x) *x = 0;
	if (y) *y = 0;
	if (w) *w = g_clipWidth;
	if (h) *h = g_clipHeight;
	return 1;
}

static int s_cdLastNumber = -1;
static int s_goArmedMs;              /* DAT_004dbe44, see hud_draw_go */

/*
 * ORIGIN  Hud_DrawCountdown @ 0x00413030
 *
 * o THE NUMBER IS seconds/2 + 1, NOT THE SECONDS. g_nTimeSeconds is
 *   MenuTools_SplitTimeMs's seconds-within-the-minute field, so a
 *   6000 ms grid timer counts 4, 3, 2, 1 - each held two seconds
 *   except the first, which the timer passes through in one frame.
 *   The countdown is three steps and a flash, like a start tree.
 *
 * o AND GET READY STOPS BEFORE THE LAST NUMBER. Its guard is
 *   1 < n, so it is up for 4, 3 and 2 and gone for the final two
 *   seconds.
 *
 * o BOTH ARE REBUILT ONLY WHEN THE NUMBER CHANGES, which is what
 *   makes the fade and the blink restart per step instead of every
 *   frame.
 */
static void hud_draw_countdown(void)
{
	const MR_RaceClock *clock = race_clock();
	int                 seconds;
	int                 n;

	if (!clock)
		return;

	hud_countdown_font_load();
	if (!s_cdFontOk)
		return;

	/* MenuTools_SplitTimeMs @ 0x00481730 - the one field used here. */
	seconds = (clock->timerMs / 1000) % 60;
	if (seconds < 0)
		seconds = 0;

	/* The GO handoff, armed every frame. See hud_draw_go. */
	s_goArmedMs = 500;

	/*
	 * THE BANNER STYLE, 0x00413045. Note the thresholds are on
	 * the RAW SECONDS, not on the number being shown.
	 */
	hud_draw_laps_banner(seconds < 2 ? 2 : (seconds < 4 ? 1 : 0));

	n = seconds / 2 + 1;
	if (n != s_cdLastNumber) {
		char text[16];

		s_cdLastNumber = n;

		/*
		 * [open] THE PADDING IS NOT VERIFIED. This draws 04, 03,
		 * 02, 01 where a bare "%d" would draw 4, 3, 2, 1, and no
		 * format string has been read for this widget - unlike
		 * the speed readout's "%3d" at 0x004f10e0. The digits are
		 * single-digit for the whole countdown, so the leading
		 * zero is on screen or it is not.
		 */
		sprintf(text, "%02d", n);
		hud_widget_show(&s_cdNumber, text, 2000, 0x42, 0, 0,
		                2, &s_msgFont, HUD_FONT_GAMEMSG, 4,
		                0, 0.0, 0.0, 0.0, 0.0);

		hud_countdown_sound(n);

		if (1 < n) {
			hud_widget_show(&s_cdReady, "GET READY", 0, 0x42,
			                0, 0x1e, 3, &s_bigMsgFont,
			                HUD_FONT_MESSAGES, 4,
			                0, 0.0, 0.0, 0.0, 0.0);
		} else {
			s_cdReady.live = 0;
		}
	}
}

/*
 * ORIGIN  Hud_DrawInRace @ 0x00413180, at 0x004133b0
 *
 * GO IS NOT PART OF THE COUNTDOWN. It is drawn by the IN-RACE half
 * of the dispatcher, on the first frame after g_bPreRace clears, and
 * the two halves hand off through a single global - DAT_004dbe44:
 *
 *     Hud_DrawCountdown   *p = 500;                 every frame
 *     Hud_DrawInRace      if (0 < *p) { GO; *p = 0; }
 *
 * so the countdown arms it continuously and the first racing frame
 * spends it, once, as the fade duration. s_goArmedMs is that global.
 *
 * o IT REPLACES GET READY RATHER THAN JOINING IT. Both use widget
 *   key 0x004e98e8, and Widget_ShowText destroys whatever holds the
 *   key before building the new one.
 *
 * o AND IT DOES NOT BLINK. GET READY is visibility mode 3; GO is
 *   mode 2, a fade over those 500 ms. Different modes on the same
 *   key, which is why the two look nothing alike on screen.
 *
 * o y IS 0xf, NOT 0x1e - GO sits higher than GET READY did.
 *
 * [open] Font_SetForeColour(0,200,0) and Font_SetBackColour(0,180,0)
 * run immediately before this call, exactly as they do before GET
 * READY, and TextWidget_Draw saves and restores the font's pair
 * around the draw so neither reaches the screen. TWO independent
 * sites doing the same pointless thing is weak evidence that the
 * save-and-restore reading is wrong somewhere; recorded rather than
 * acted on.
 */
static void hud_draw_go(void)
{
	if (s_goArmedMs > 0) {
		hud_widget_show(&s_cdReady, "GO", s_goArmedMs, 0x42,
		                0, 0xf, 2, &s_msgFont,
		                HUD_FONT_GAMEMSG, 4,
		                0, 0.0, 0.0, 0.0, 0.0);

		/*
		 * AND THE GO SOUND, WHICH IS NOT THE COUNTDOWN'S.
		 *
		 * ORIGIN  Hud_DrawInRace @ 0x00413180, at 0x004133e6 -
		 *         Hud_CountdownSound(0), between the widget
		 *         and the flag clear, which is the order kept
		 *         here.
		 *
		 * CASE 0 IS UNREACHABLE FROM THE COUNTDOWN and that is
		 * not an oversight in either build: the drawer passes
		 * seconds / 2 + 1, which is one at its lowest, so the
		 * GO arm of the switch can only ever be entered from
		 * HERE.  Looking for the missing GO in
		 * hud_draw_countdown is looking in the wrong function.
		 *
		 * It brings bip2 with it - the higher beep, the one
		 * that sounds different from the three that came
		 * before it.
		 */
		hud_countdown_sound(0);
		s_goArmedMs = 0;
	}
}

/*
 * ORIGIN  NONE - ours. Draws what the original keeps in the
 *         widget array at 0x0053ba68
 *
 * EVERY MESSAGE WIDGET, IN BOTH PHASES, and that is the point.
 *
 * The original keeps its widgets in one global array and draws the
 * whole array every frame; the race phase does not enter into it.
 *
 * SO DO NOT GATE THIS ON THE PHASE. A widget stops being drawn when
 * it RETIRES, not when the phase changes - and the LAPS banner is
 * the case that proves it: created in the last two seconds of the
 * countdown, accelerating downward, and still crossing the screen
 * after the race has started. A phase-gated draw deletes it
 * mid-fall. See hud_widget_tick.
 */
static void hud_draw_messages(void)
{
	hud_widget_draw(&s_cdLaps);
	hud_widget_draw(&s_cdNumber);
	hud_widget_draw(&s_cdReady);
	hud_widget_draw(&s_timeoutBanner);
	hud_widget_draw(&s_gameOverBanner);
}

/*
 * Advance every message widget once per frame. Called from hud_draw
 * OUTSIDE its two overlay passes - see hud_widget_tick.
 */
static void hud_messages_tick(void)
{
	hud_widget_tick(&s_cdNumber);
	hud_widget_tick(&s_cdReady);
	hud_widget_tick(&s_cdLaps);
	hud_widget_tick(&s_timeoutBanner);
	hud_widget_tick(&s_gameOverBanner);
}

/* Reset between races, so the first number rebuilds its fade. */
void hud_countdown_reset(void)
{
	s_cdLastNumber  = -1;
	s_cdNumber.live = 0;
	s_cdReady.live  = 0;
	s_cdLaps.live   = 0;
	s_cdLapsStyle   = -1;
	s_goArmedMs     = 0;

	/* THE ART SURVIVES A RESTART AND THE WIDGETS DO NOT - the
	   page is the same every race, and re-cutting it would
	   reload a 33 KB archive member for nothing. */
	s_timeoutBanner.live  = 0;
	s_gameOverBanner.live = 0;
}

/*
 * The rectangle the countdown occupies. Both widgets are centred, so
 * this is a band across the middle of the frame, not a corner.
 */
int hud_countdown_overlay_rect(int *x, int *y, int *w, int *h)
{
	if (!race_ready() || !s_cdFontOk)
		return 0;
	if (!s_cdNumber.live && !s_cdReady.live && !s_cdLaps.live)
		return 0;

	/*
	 * THE WHOLE FRAME, and that is not laziness. The digits and GET
	 * READY sit in a band across the middle, but the LAPS banner
	 * starts at the top and ACCELERATES OFF THE BOTTOM - so any box
	 * tight enough to be worth computing would clip it partway down.
	 */
	if (x) *x = 0;
	if (y) *y = 0;
	if (w) *w = g_clipWidth;
	if (h) *h = g_clipHeight;
	return 1;
}

/*
 * ORIGIN  FUN_00482380 @ 0x00482380, at 0x00482396 onward
 *
 * The countdown, its banner, and the flash that speeds up as the
 * time runs out.
 */
static void hud_draw_time(void)
{
	const MR_RaceClock  *clock = race_clock();
	const unsigned char *digitColour = kHudYellow;
	const unsigned char *bannerColour = kHudWhite;
	unsigned int         mask = 0xffffffffu;
	int                  timerMs;
	int                  seconds;
	char                 text[16];

	if (!race_ready() || !clock)
		return;

	hud_race_font_load();
	if (!s_raceFontOk)
		return;

	timerMs = clock->timerMs;
	seconds = timerMs / 1000;

	/*
	 * "00" ONCE THE TIMER HAS GONE NEGATIVE, not a negative number.
	 * The string is the literal at 0x004f1068 and the live format is
	 * "%02d" at 0x004dbf0c - two digits, zero padded.
	 */
	if (timerMs < 0)
		strcpy(text, "00");
	else
		sprintf(text, "%02d", seconds);

	/*
	 * THE LADDER, 0x004823b6 onward. The mask is ANDed with a frame
	 * counter, so & 1 blinks every other frame, & 2 two on two off,
	 * & 4 four on four off - FASTER THE LESS TIME IS LEFT - and
	 * 0xffffffff never blinks at all.
	 *
	 * Note the banner does NOT follow the digits through the whole
	 * ladder: the two urgent arms set both, and the orange and green
	 * arms leave the banner white.
	 */
	if (g_extendedTimeMs < 4000) {
		if (timerMs < 0) {
			digitColour = bannerColour = kHudRed;
			mask = 1;
		} else if (seconds < 3) {
			digitColour = bannerColour = kHudRed;
			mask = 2;
		} else if (seconds < 10) {
			digitColour = kHudOrange;
			mask = 4;
		} else {
			digitColour = kHudGreen;
		}
	} else {
		digitColour = bannerColour = kHudYellow;
	}

	/*
	 * THE BLINK ADVANCES WITH GAME TIME, NOT WITH FRAMES, and that
	 * is a DIVERGENCE the pause forces.
	 *
	 * The original bumps 0x004f0e1c once per rendered frame, which
	 * is the same thing there because it has no pause and no
	 * fast-forward. This port has both, and a readout still
	 * flashing behind a frozen race reads as a bug whatever the
	 * disassembly says.
	 */
	if (!tm_paused())
		s_hudBlink++;

	/*
	 * THE DIGITS TAKE A FOREGROUND AND A BACKGROUND - black, the
	 * colour at 0x004f0dd8 - because the Time face is a Direct one
	 * and its LUT is a ramp between the two.
	 */
	if (s_hudBlink & mask) {
		font_set_colors(&s_timeFont, digitColour, kHudBlack);
		font_draw_text_aligned(&s_timeFont, HUD_TIME_DIGITS_X,
		                       HUD_TIME_DIGITS_Y, text,
		                       HUD_ALIGN_RIGHT, 0);
	}

	/*
	 * THE BANNER'S COLOUR IS INERT and the call is kept anyway,
	 * because the original makes it: Sprites is a Pal face, and the
	 * Pal LUT builder reads the face's own palette and ignores
	 * whatever set_colors stored. The banner's colour is in the art.
	 *
	 * [open] The banner's blink test reads EBX, which the decompiler
	 * lost at 0x00482438. The structure says it is the same mask -
	 * the readout flashes as a unit - and that is what this does.
	 */
	if (s_hudBlink & mask) {
		font_set_colors(&s_sprFont, bannerColour, NULL);
		font_draw_text_aligned(&s_sprFont, HUD_TIME_BANNER_X,
		                       HUD_TIME_BANNER_Y, HUD_SPR_TIME,
		                       HUD_ALIGN_RIGHT, 0);
	}
}

/*
 * The rectangle the block occupies, for the overlay composite. The
 * banner is the widest thing in it and the digits the lowest.
 */
int hud_time_overlay_rect(int *x, int *y, int *w, int *h)
{
	int div, bw, dh, x0, y0, x1, y1;

	if (!race_ready() || !s_raceFontOk || !hud_race_hud_visible())
		return 0;
	/*
	 * Nothing to composite when practice draws no TIME block,
	 * and nothing at all once the race is over - see
	 * hud_race_over. A rect for a block that is no longer
	 * drawn composites the stale band it used to occupy.
	 */
	if (!hud_time_and_pos() || hud_race_over())
		return 0;

	/*
	 * THE SAME HALVING THE DRAW GETS, and IT FAILS SILENTLY IF NOT.
	 * With small fonts on the text moves to y 45..73; a rectangle
	 * left at 86..153 makes the composite copy a region the text is
	 * not in, so the whole block vanishes with nothing logged and
	 * nothing drawn to hint at why.
	 *
	 * One divisor, from font.h, rather than a second copy of the
	 * condition.
	 */
	div = font_layout_divisor();

	bw = font_text_width(&s_sprFont, HUD_SPR_TIME);
	dh = font_height(&s_timeFont);

	x1 = g_clipWidth - HUD_TIME_BANNER_X / div + HUD_COUNTER_PAD;
	x0 = g_clipWidth - HUD_TIME_BANNER_X / div - bw - HUD_COUNTER_PAD;
	y0 = HUD_TIME_BANNER_Y / div - HUD_COUNTER_PAD;
	y1 = HUD_TIME_DIGITS_Y / div + dh + HUD_COUNTER_PAD;

	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > g_clipWidth)  x1 = g_clipWidth;
	if (y1 > g_clipHeight) y1 = g_clipHeight;
	if (x1 <= x0 || y1 <= y0)
		return 0;

	if (x) *x = x0;
	if (y) *y = y0;
	if (w) *w = x1 - x0;
	if (h) *h = y1 - y0;
	return 1;
}

/* ================================================================== */
/* BEST LAP - the block at the very top right.                        */
/*                                                                    */
/* ORIGIN  FUN_00482830 @ 0x00482830, case 0                          */
/*                                                                    */
/* See docs/subsystems/hud-race.txt 5.1.                              */
/* ================================================================== */

/* "C" is the BEST LAP banner; see the Sprites sheet, hud-race.txt 3. */
#define HUD_SPR_BESTLAP   "C"

#define HUD_BL_BANNER_X   0
#define HUD_BL_BANNER_Y   5
#define HUD_BL_ROW_Y      0x1b   /* 27, local_a4's initial value */
#define HUD_BL_NAME_DY    (-2)   /* the name sits two rows higher */
#define HUD_BL_TIME_GAP   5      /* 0x00482d0e */

/*
 * The row pitch is the PLAYER face height plus this, read out of
 * g_fontTable[6] at 0x00482d4e rather than being a constant.
 */
#define HUD_BL_ROW_GAP    4

/*
 * THE SENTINEL FOR "NO TIME YET", 0x7f7f7f7f. FUN_00411010 fills the
 * record tables with it at race start and every reader tests against
 * it rather than against zero.
 */
#define HUD_BL_NO_TIME    0x7f7f7f7fu

/*
 * THE NAME IS PADDED TO EIGHT AND THEN SHIFTED, and the shift is
 * choosing a COLOUR.
 *
 * NAME.FNT holds 128 glyphs from 0x20 and carries the alphabet THREE
 * TIMES over. It is a Pal face with a 256-entry palette, and the
 * banks are these [verified, palette read out of the file]:
 *
 *     0x40..0x5f   '@' then A-Z, indices 1 and 5..11
 *                  WHITE (index 7 is 255,255,255) shaded to light
 *                  red
 *     0x60..0x7f   the same set, indices 2..7 - AMBER
 *     0x80..0x9f   the same set, indices 11..15
 *                  RED (255,0,0) fading to BLACK (index 15)
 *
 * o THE BASE BANK IS WHITE, NOT RED, and the two are worth keeping
 *   straight: the difference between the base bank and the third is
 *   the difference between a legible name and a black bar. The
 *   DIGITS the lap list draws through this same face - '1' is 0x31
 *   - are in the white family too, which is why the two blocks
 *   match when the name is unshifted and do not when it is not.
 *
 * so adding 0x20 or 0x40 to every character of an ASCII name picks
 * the second or third bank. The original does exactly that
 * [verified, 0x00482bd0]:
 *
 *     bVar4 set        every char += '@'   the track record, DARK RED
 *     else if !human   every char += ' '   an AI, AMBER
 *     else             unchanged           the player, RED
 *
 * o AND THE DARK-RED BANK IS WHY THE NAME READS AS AN UNREADABLE
 *   BAR on a dark track. It is the art, not a drawing fault.
 *
 * o THE PAD CHARACTER IS '@' BECAUSE 0x40 IS A FULL-WIDTH GLYPH in
 *   this face - the one before 'A' - so padding does not change the
 *   string's width. Every other filler slot is one pixel wide.
 */
#define HUD_BL_NAME_CELLS 8

/* ================================================================== */
/* THE RACER CALLOUT - the caption over the other rider's head.       */
/* ================================================================== */

/*
 * MotoHiRes, and it is NOT one of the fonts the rest of this file
 * opens. Font_Get(5) at 0x004816f5 fills the global the callout
 * reads, and 5 is MotoHiRes - the twin of MotoLowRes, id 1. See
 * docs/structs/MR_Font.txt's pair table.
 */
#define HUD_FONT_MOTO      1

/*
 * EVERY PUNCTUATION CHARACTER IN THE FORMAT IS A GLYPH INDEX, not
 * punctuation - the same convention the lap list uses for ':' ';'
 * '<' and "[]". '^' and the backslash are the frame pieces either
 * side of the name and ']' is the pointer, alone on the second
 * line. Reading them as ASCII is how this ends up drawing literal
 * carets.
 *
 * ORIGIN  the format at 0x004f10bc
 */
#define HUD_CALLOUT_FMT    "%c^%s\\\n]"

/* position + 0x29, so place 0 is ')' - a BADGE, not a digit. */
#define HUD_CALLOUT_BADGE  0x29

/* 0x5d is ']', the pointer the caption centres on. */
#define HUD_CALLOUT_TICK   0x5d

/* 0x004d1a20, a FLOAT - the head, not the bike. */
#define HUD_CALLOUT_HEAD_Y 125.0f

/*
 * ORIGIN  FUN_00459240 @ 0x00459240
 *
 * THE CAPTION IS IN THE RIDER'S OWN COLOUR, not a fixed one: entry
 * 0xc6 of his LIVERY palette out of bank 0 on the first three
 * tracks, 0xf0 beyond them. 0x0e39 is the fallback the original
 * starts with and keeps when the racer has no livery or that
 * palette is absent.
 *
 * racer+0x170 is the livery; the original indexes
 * MR_Bank_ARRAY_004f92f8[0].pPalette with it directly.
 */
#define HUD_CALLOUT_PAL_LOW   0xc6
#define HUD_CALLOUT_PAL_HIGH  0xf0
#define HUD_CALLOUT_FALLBACK  0x0e39

static unsigned short hud_callout_colour(const MR_Racer *r)
{
	MR_Bank *b;
	int      t = g_selectedTrack;
	int      idx;

	if (!r || r->livery < 0 || r->livery >= BANK_MAX_PALETTES)
		return HUD_CALLOUT_FALLBACK;

	b = bank_get(0);
	if (!b || !b->palette[r->livery])
		return HUD_CALLOUT_FALLBACK;

	idx = (t == 0 || t == 1 || t == 2) ? HUD_CALLOUT_PAL_LOW
	                                   : HUD_CALLOUT_PAL_HIGH;
	return b->palette[r->livery][idx];
}


static MR_Font s_motoFont;
static int     s_motoFontOk;
/*
 * THE UNION OF EVERY CALLOUT DRAWN THIS FRAME, for the compositor.
 *
 * Our HUD draws into an overlay buffer and main.cpp uploads only the
 * RECTANGLES each element declares, so a caption with no rectangle
 * reaches the screen solely where it happens to overlap another
 * element's - and vanishes for good once the rider's projected
 * position moves clear of them. That reads as a timer and is not
 * one. The original has no such step: it draws straight into the
 * one DirectDraw surface.
 */
static int     s_calloutRectOk;
static int     s_calloutX0, s_calloutY0, s_calloutX1, s_calloutY1;
static int     s_motoFontTried;
static int     s_motoFontSmall = -1;

/*
 * ORIGIN  Font_MeasureText @ 0x00492b10, its height half
 *
 * ITS LAST ARGUMENT IS A STOP-AT-NEWLINE FLAG, and that is the whole
 * reason this takes one too. The height starts at one glyphHeight;
 * on a 0x0a the original either ACCUMULATES another line - plus
 * MR_Font+0xa1c of leading, which our MR_Font does not model - or,
 * when the flag is set, BREAKS THE LOOP and leaves the height at a
 * single line.
 *
 * The racer callout passes 1, so its measured height is ONE line
 * however many it draws. Mistaking that for the full block lifts the
 * caption a line too far and opens a gap over the rider's head.
 *
 * font.h exposes only a width, which is why this lives here.
 */
static int hud_text_height(const MR_Font *f, const char *s,
                           int stopAtNewline)
{
	int lines = 1;

	if (!f || !s)
		return 0;
	if (!stopAtNewline) {
		for (; *s; s++) {
			if (*s == '\n')
				lines++;
		}
	}
	return lines * font_height(f);
}


static void hud_callout_font_load(void)
{
	/* The same SmallFonts reload as hud_race_font_load - see there. */
	if (s_motoFontTried && s_motoFontSmall == g_options.smallFonts)
		return;
	if (s_motoFontTried && s_motoFontOk) {
		font_free(&s_motoFont);
		s_motoFontOk = 0;
	}
	s_motoFontTried = 1;
	s_motoFontSmall = g_options.smallFonts;

	s_motoFontOk = fontdef_open(&s_motoFont, HUD_FONT_MOTO);
	if (!s_motoFontOk)
		mrlog_err("hud: callout font did not load\n");
}


/*
 * ORIGIN  FUN_004818a0 @ 0x004818a0
 *
 * Transform one world point by the camera and project it. Returns 0
 * when it is not on screen, which is what makes a callout vanish as
 * the rider pulls away rather than sticking to an edge.
 *
 * THE ORIGINAL'S VISIBILITY TEST IS ON THE FLOAT BITS:
 * (unsigned)camZ < 0x80000001 passes every positive float and -0.0,
 * so it is a SIGN test - in front of the camera - and not a near
 * plane. Ours uses the near plane instead, because the reciprocal
 * below divides by camZ and a point at 0.001 would project to
 * coordinates in the millions. [deliberate divergence]
 *
 * The arithmetic is transform_verts_flat's, with the viewport scales
 * applied at projection time rather than folded into the matrix -
 * the same result, and this needs no vertex-pool slot.
 */
static int hud_project_world_point(const float m[12], const float p[3],
                                   int *outX, int *outY)
{
	float cx, cy, cz, inv;

	if (!m || !p || !outX || !outY)
		return 0;

	cx = m[0] * p[0] + m[3] * p[1] + m[6] * p[2] + m[9];
	cy = m[1] * p[0] + m[4] * p[1] + m[7] * p[2] + m[10];
	cz = m[2] * p[0] + m[5] * p[1] + m[8] * p[2] + m[11];

	if (cz < g_nearPlaneZ)
		return 0;

	inv   = 1.0f / cz;
	*outX = (int)((float)g_viewCentreX + inv * cx * g_projScaleX);
	*outY = (int)((float)g_viewCentreY - inv * cy * g_projScaleY);
	return 1;
}


/*
 * ORIGIN  Hud_DrawRacerCallouts @ 0x00483670, whole
 *
 * A two-line caption at each racer's projected head, with a pointer
 * glyph underneath. See docs/subsystems/hud-race.txt 7.2.
 *
 * IT NEVER LABELS THE RACER BEING VIEWED, which is why there is a tag
 * on the ghost and none on you. In solo the caller only reaches it in
 * practice with time attack on, so the ghost is the only thing it
 * ever labels there.
 *
 * THE TEXT IS playerName, and for the ghost that is "COMPUTER"
 * [verified live] - racer_create gives every non-human racer that
 * name and nothing points this at the ghost record's "SHADOW".
 */
static void hud_draw_racer_callouts(void)
{
	const float    *view   = race_camera_view();
	const float    *eye    = race_camera_eye();
	const MR_Racer *viewed = race_viewed_racer();
	int             count  = race_racer_count();
	int             i;

	if (!view || !eye)
		return;

	hud_callout_font_load();
	if (!s_motoFontOk)
		return;

	for (i = 1; i < count; i++) {
		const MR_Racer *r = race_racer_at(i);
		char            text[48];
		float           head[3];
		int             sx, sy;

		if (!r || r == viewed)
			continue;

		/*
		 * AND ONLY IF THE BIKE ITSELF WAS DRAWN. The original's two
		 * gates are racer+0x68 and racer+0x00 - active, and
		 * drawnThisFrame, which race_draw_3d clears for everyone
		 * each frame and sets only for a racer that survived the
		 * cull. So the caption goes when the rider goes, which is
		 * the behaviour rather than a side effect: it is not the
		 * projection that drops it at distance.
		 */
		if (!r->active || !r->drawnThisFrame)
			continue;

		/*
		 * RELATIVE TO THE EYE, then transformed - the original
		 * subtracts the eye itself rather than folding it into the
		 * matrix, so the matrix it uses carries no translation.
		 */
		head[0] = r->position[0] - eye[0];
		head[1] = (r->position[1] - eye[1]) + HUD_CALLOUT_HEAD_Y;
		head[2] = r->position[2] - eye[2];

		if (!hud_project_world_point(view, head, &sx, &sy))
			continue;

		/* THE LIVE STANDING, NOT r->finishPosition, which is written
		 * only at the line. 0x0048377a; hud-race.txt 7.2. */
		_snprintf(text, sizeof(text), HUD_CALLOUT_FMT,
		          HUD_CALLOUT_BADGE + race_finishing_position(i),
		          r->playerName);
		text[sizeof(text) - 1] = 0;

		/*
		 * X CENTRES ON THE POINTER, not on the text - half the ']'
		 * advance, so the tick lands on the rider whatever the name
		 * is.
		 *
		 * Y COMES OFF TWICE AND BOTH ARE ONE LINE. 0x0048374c
		 * takes a glyphHeight (and [0x005d893c], which reads 0),
		 * then 0x004837c8 subtracts the MEASURED height - and the
		 * measure is called with the stop-at-newline flag, so it
		 * is ONE line however many the caption draws. Two lines
		 * total: the pointer lands on the projected point and the
		 * name sits directly above it. Three leaves a gap over the
		 * rider's head; one puts the tick on his back.
		 */
		sx -= s_motoFont.charWidth[HUD_CALLOUT_TICK] / 2;
		sy -= font_height(&s_motoFont)
		      + hud_text_height(&s_motoFont, text, 1);

		/*
		 * THE COLOUR IS SET PER RACER, then the LUT rebuilt -
		 * 0x004837b0 feeds Font_SetForeColourPacked and
		 * 0x004837c4 calls build_lut(1, 255) through the vtable.
		 * Our framebuffer is fixed 565, so the unpack is the
		 * original's masks and shifts with the constants folded.
		 */
		{
			unsigned short  c = hud_callout_colour(r);
			unsigned char   fore[3];

			fore[0] = (unsigned char)(((c >> 11) & 0x1f) << 3);
			fore[1] = (unsigned char)(((c >>  5) & 0x3f) << 2);
			fore[2] = (unsigned char)(( c        & 0x1f) << 3);
			font_set_colors(&s_motoFont, fore, NULL);
		}

		/*
		 * font_draw_text, NOT font_draw_text_aligned - these are
		 * DEVICE pixels and must not be halved.
		 *
		 * The original passes flags 0x21, which is no alignment at
		 * all, through FUN_00493f60 - and that wrapper hands
		 * Font_DrawTextInternal param_1 = 1, which is exactly the
		 * flag that SKIPS the layout divisor at 0x00493a5c. Going
		 * through the aligned entry point applied the divisor to a
		 * projected coordinate and put the caption in the upper
		 * left of the screen.
		 */
		font_draw_text(&s_motoFont, sx, sy, text, 0);

		{
			int w = font_text_width(&s_motoFont, text);
			/* The REAL extent, so both lines composite - this
			   one is not the measure the lift uses. */
			int h = hud_text_height(&s_motoFont, text, 0);

			if (!s_calloutRectOk) {
				s_calloutRectOk = 1;
				s_calloutX0 = sx;
				s_calloutY0 = sy;
				s_calloutX1 = sx + w;
				s_calloutY1 = sy + h;
			} else {
				if (sx < s_calloutX0)      s_calloutX0 = sx;
				if (sy < s_calloutY0)      s_calloutY0 = sy;
				if (sx + w > s_calloutX1)  s_calloutX1 = sx + w;
				if (sy + h > s_calloutY1)  s_calloutY1 = sy + h;
			}
		}
	}
}


/* ORIGIN  NONE - ours. See s_calloutRectOk. */
int hud_callout_overlay_rect(int *x, int *y, int *w, int *h)
{
	int x0, y0, x1, y1;

	if (!race_ready() || !s_calloutRectOk)
		return 0;

	x0 = s_calloutX0;  y0 = s_calloutY0;
	x1 = s_calloutX1;  y1 = s_calloutY1;

	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > g_clipWidth)  x1 = g_clipWidth;
	if (y1 > g_clipHeight) y1 = g_clipHeight;
	if (x1 <= x0 || y1 <= y0)
		return 0;

	if (x) *x = x0;
	if (y) *y = y0;
	if (w) *w = x1 - x0;
	if (h) *h = y1 - y0;
	return 1;
}


static MR_Font s_nameFont;     /* PlayerHiRes, NAME.FNT   */
static MR_Font s_chronoFont;   /* ChronoHiRes, CHRONO.FNT */
static int     s_blFontOk;
static int     s_blFontTried;
static int     s_blFontSmall = -1;

static void hud_bestlap_font_load(void)
{
	/* The same SmallFonts reload as hud_race_font_load - see there. */
	if (s_blFontTried && s_blFontSmall == g_options.smallFonts)
		return;
	if (s_blFontTried && s_blFontOk) {
		font_free(&s_nameFont);
		font_free(&s_chronoFont);
		s_blFontOk = 0;
	}
	s_blFontTried = 1;
	s_blFontSmall = g_options.smallFonts;

	s_blFontOk =
	    fontdef_open(&s_nameFont, HUD_FONT_NAME) &&
	    fontdef_open(&s_chronoFont, HUD_FONT_CHRONO);

	if (!s_blFontOk)
		mrlog_err("hud: best-lap fonts did not load\n");
}

/*
 * ORIGIN  FUN_00482830 @ 0x00482830, case 0 at 0x0048289c
 *
 * Who currently holds the best lap. The original caches the answer
 * in 0x004f0db8 and only rebuilds it - by packing every racer's
 * bestLapMs with its index and qsorting - when the cached racer goes
 * inactive.
 *
 * DIVERGENCE, AND AN EXACT ONE: ours takes the minimum directly. The
 * sort's first element IS the minimum, and its tie-break is by the
 * index packed into the key's high bits, which is the same racer a
 * strict `<` scan keeps. The cache is a cost optimisation with no
 * effect on the answer.
 */
static int hud_best_lap_racer(void)
{
	int best = -1;
	int bestMs = 0;
	int i;
	int n = race_racer_count();

	for (i = 0; i < n; i++) {
		const MR_Racer *r = race_racer_at(i);

		if (!r || !r->active)
			continue;
		if ((unsigned int)r->bestLapMs == HUD_BL_NO_TIME)
			continue;
		if (best < 0 || r->bestLapMs < bestMs) {
			best = i;
			bestMs = r->bestLapMs;
		}
	}
	return best;
}

/* ORIGIN  FUN_00482830 @ 0x00482830, the msgScratch build at 0x00482b50 */
static void hud_bestlap_name(const MR_Racer *r, char *out, int size)
{
	int shift = r->humanPlayer ? 0 : ' ';
	int i;

	for (i = 0; i < HUD_BL_NAME_CELLS && i < size - 1; i++) {
		char c = r->playerName[i];

		/*
		 * PADDED WITH '@' TO EIGHT CELLS, not space-padded and not
		 * trimmed - the original strncat's a run of '@' onto the
		 * short name at 0x00482b8c. The pad takes the shift too,
		 * so it lands on the right bank's own blank.
		 */
		if (c == 0)
			c = '@';
		out[i] = (char)(c + shift);
	}
	out[i] = 0;
}

/*
 * ORIGIN  FUN_00482830 @ 0x00482830, whole
 *
 * IT IS A LIST, NOT A LINE, and that is the shape of the function:
 * one do-while that runs THREE TIMES in a race, feeding a different
 * SOURCE into the same row each pass [verified]:
 *
 *     case 0   the best lap held by any racer this session
 *     case 1   the record at 0x00581118, name at +0, time at +0xf5
 *     case 4   the record at param_2 (0x005d25b0), same layout
 *
 * and it stops after the third because that pass clears bVar5. The
 * countdown's call passes param_1 = 1, which skips the loop entirely
 * and draws case 1 alone - which is why the grid shows one row and
 * the race shows up to three.
 *
 * o A SOURCE WITH NO TIME DRAWS NO ROW. Each case tests its time
 *   against the 0x7f7f7f7f sentinel first, so the list grows as
 *   records appear rather than reserving space for them.
 *
 * o THE BANNER IS DRAWN ONCE, before the first row that survives
 *   that test - bVar3 in the original - so a block with no rows
 *   shows no heading either.
 *
 * o THE ROW PITCH IS A FONT HEIGHT PLUS FOUR, 0x00482d4e:
 *   g_fontTable[6]->glyphHeight + 4, the Player face. Not a
 *   constant, so a LowRes face packs the rows tighter.
 *
 * o EVERY DRAW SETS ITS OWN COLOUR, and that is not belt and
 *   braces. s_chronoFont is SHARED with the lap list, which
 *   draws first and leaves the font GREEN when it has
 *   highlighted a best lap - so this block inherited green times
 *   for the rest of the race. The original has the same hazard
 *   and answers it the other way round, restoring the colour
 *   right after its green row at 0x00483c4e; setting it here
 *   covers any future caller too.
 *
 * [open] CASE 4 HAS NO SOURCE HERE, and only case 4. Cases 0 and 1
 * both draw - case 1 reads Data\bestscor.dat through
 * race_track_record, so the track record does stack under the
 * session row. See the note where case 4's row would go.
 */
static void hud_draw_best_lap(void)
{
	const MR_Racer *r;
	char            name[16];
	char            time[16];
	int             idx;
	int             ms, sec, nameW;
	int             rowY;
	int             div;

	if (!race_ready())
		return;

	hud_bestlap_font_load();
	if (!s_blFontOk || !s_raceFontOk)
		return;

	idx = hud_best_lap_racer();
	if (idx < 0 && !race_track_record())
		return;                 /* no source has a time - no block */
	r = (idx >= 0) ? race_racer_at(idx) : NULL;

	div = font_layout_divisor();

	/*
	 * The banner, right-aligned at (0, 5) - the mirror of the lap
	 * list's left-aligned one at (2, 5). The two corners are
	 * deliberately symmetric.
	 */
	font_set_colors(&s_sprFont, kHudWhite, NULL);
	font_draw_text_aligned(&s_sprFont, HUD_BL_BANNER_X, HUD_BL_BANNER_Y,
	                       HUD_SPR_BESTLAP, HUD_ALIGN_RIGHT, 0);

	rowY = HUD_BL_ROW_Y;

	/* CASE 0, the session best - absent until somebody sets one. */
	if (r) {
		hud_bestlap_name(r, name, (int)sizeof(name));

		ms  = r->bestLapMs;
		sec = ms / 1000;
		sprintf(time, "%02d:%02d:%02d", (sec / 60) % 60, sec % 60,
		        (ms % 1000) / 10);

		nameW = font_text_width(&s_nameFont, name);

		font_draw_text_aligned(&s_nameFont, 0, rowY + HUD_BL_NAME_DY,
		                       name, HUD_ALIGN_RIGHT, 0);

		/*
		 * THE TIME SITS JUST LEFT OF THE NAME. The original computes
		 * x = (nameOut - timeWidth) - 5 and pre-doubles it for the
		 * halving draw; ours right-aligns to the same place, which is
		 * the arrangement retail shows. The 5 is the original's.
		 *
		 * PRE-MULTIPLIED, AND THAT IS THE ORIGINAL'S OWN x *= 2 at
		 * 0x00482d1c. nameW is a MEASURED width, already in real
		 * pixels, and font_draw_text_aligned HALVES whatever inset it
		 * is given - so an inset built from a measured width must be
		 * doubled first or the time lands on top of the name.
		 */
		font_set_colors(&s_chronoFont, kHudWhite, kHudBlack);
		font_draw_text_aligned(&s_chronoFont,
		                       (nameW + HUD_BL_TIME_GAP) * div,
		                       rowY, time, HUD_ALIGN_RIGHT, 0);

		rowY += font_height(&s_nameFont) + HUD_BL_ROW_GAP;
	}

	/*
	 * CASE 1, 0x00482a9b: the all-time track record out of
	 * Data\bestscor.dat, which race_load reads. THIS IS THE ROW
	 * RETAIL SHOWS FROM THE MOMENT THE RACE STARTS - the session
	 * row above it has no time until somebody completes a lap, so
	 * a block with only that row looks empty for the whole of lap
	 * one.
	 *
	 * o ITS NAME TAKES THE '@' SHIFT into the DARK RED bank,
	 *   because bVar4 is set for this case. A record row reading
	 *   as an unreadable bar on a dark track is the art, not a
	 *   fault.
	 */
	{
		const MR_TrackRecord *rec = race_track_record();

		if (rec) {
			int i;

			for (i = 0; i < HUD_BL_NAME_CELLS &&
			     i < (int)sizeof(name) - 1; i++) {
				char c = rec->name[i];

				if (c == 0)
					c = '@';
				name[i] = (char)(c + '@');
			}
			name[i] = 0;

			ms  = rec->timeMs;
			sec = ms / 1000;
			sprintf(time, "%02d:%02d:%02d", (sec / 60) % 60,
			        sec % 60, (ms % 1000) / 10);

			nameW = font_text_width(&s_nameFont, name);

			font_draw_text_aligned(&s_nameFont, 0,
			                       rowY + HUD_BL_NAME_DY, name,
			                       HUD_ALIGN_RIGHT, 0);
			font_set_colors(&s_chronoFont, kHudWhite, kHudBlack);
			font_draw_text_aligned(&s_chronoFont,
			                       (nameW + HUD_BL_TIME_GAP) * div,
			                       rowY, time, HUD_ALIGN_RIGHT, 0);

			rowY += font_height(&s_nameFont) + HUD_BL_ROW_GAP;
		}
	}

	/*
	 * 0x00482ae0: CASE 4 would follow, the record at 0x005d25b0.
	 * FUN_00411010 clears it to the sentinel at race start and only
	 * FUN_00458310 - not transcribed - ever writes one, so it has
	 * nothing to draw yet. rowY is advanced above so the arithmetic
	 * is already right when it lands.
	 */
	(void)rowY;
}

/* The rectangle the block occupies, for the overlay composite. */
int hud_bestlap_overlay_rect(int *x, int *y, int *w, int *h)
{
	int div, x0, y0, x1, y1;

	int rows;

	if (!race_ready() || !s_blFontOk)
		return 0;

	/* One row per source that has a time - see hud_draw_best_lap. */
	rows = (hud_best_lap_racer() >= 0) + (race_track_record() != NULL);
	if (rows == 0)
		return 0;

	div = font_layout_divisor();

	x0 = 0;
	x1 = g_clipWidth;
	y0 = HUD_BL_BANNER_Y / div - HUD_COUNTER_PAD;
	y1 = HUD_BL_ROW_Y / div
	     + (rows - 1) * (font_height(&s_nameFont) + HUD_BL_ROW_GAP)
	     + font_height(&s_chronoFont) + HUD_COUNTER_PAD;

	if (y0 < 0) y0 = 0;
	if (y1 > g_clipHeight) y1 = g_clipHeight;
	if (y1 <= y0)
		return 0;

	if (x) *x = x0;
	if (y) *y = y0;
	if (w) *w = x1 - x0;
	if (h) *h = y1 - y0;
	return 1;
}

/* ================================================================== */
/* THE LAP LIST - the block in the upper LEFT corner.                  */
/*                                                                    */
/* ORIGIN  Hud_DrawLapList @ 0x00483860 - a SKELETON                  */
/*                                                                    */
/* The original is 1219 bytes and the decompiler loses most of it:    */
/* it formats through MFC CString::Format, whose arguments do not     */
/* survive, and the draw coordinates ride in registers. Everything    */
/* below with an address was read out of the DISASSEMBLY instead.     */
/*                                                                    */
/* WHAT IS NOT TRANSCRIBED, each marked in place:                     */
/*                                                                    */
/*   0x00483967  the track-record test, which needs the record name   */
/*               and time globals at 0x005d25b0 / 0x005d26a5.         */
/*   0x00483c68  the "[]" record marker it gates, at x 0xc0.          */
/*   0x004838a0  the replay and spectator arms of the entry gate.     */
/*   throughout  DAT_004f0dc4, a mask that hides individual rows.     */
/* ================================================================== */

/* "B" is the LAP banner; the same Sprites sheet as TIME and BEST LAP. */
#define HUD_SPR_LAP        "B"

#define HUD_LAP_BANNER_X   2       /* 0x004838f6 */
#define HUD_LAP_BANNER_Y   5
#define HUD_LAP_COUNTER_DX 10      /* 0x00483a04, ADD EBX,0xa */
#define HUD_LAP_ROW_Y      0x19    /* 25, the first row */
#define HUD_LAP_ROW_DY     0x10    /* 16, ADD uVar8,0x10 */
#define HUD_LAP_TIME_X     0x3c    /* 60, PUSH 0x3c at 0x00483bd4 */
#define HUD_LAP_WINDOW     2       /* lapCounter-2 .. lapCounter */

/*
 * THE SEPARATORS ARE GLYPHS, NOT PUNCTUATION. The completed-lap
 * format at 0x004f1098 is "%02d:%02d;%02d" and the placeholder at
 * 0x004f10b0 is "00:00;00" - so ':' and ';' index whatever the
 * Chrono face draws in those slots, and the lap counter's "%d<%d"
 * (0x004f10d4) uses '<' the same way. Reading them as ASCII
 * punctuation is how a reconstruction ends up printing "1<4" where
 * the game shows a divider.
 */
#define HUD_LAP_TIME_FMT   "%02d:%02d;%02d"
#define HUD_LAP_TIME_NONE  "00:00;00"
#define HUD_LAP_COUNT_FMT  "%d<%d"

/*
 * ORIGIN  Hud_DrawLapList @ 0x00483860, the row loop at 0x00483ac0
 *
 * o THE WINDOW IS THE LAST THREE LAPS while racing, and the WHOLE
 *   race once finished. The original sets first = lapCounter - 2 in
 *   the racing arm and 0 in the finished one, and clamps last to
 *   raceLaps - 1 either way.
 *
 * o THE ROW NUMBER IS SPACE-PADDED BY HAND, not by printf. The
 *   format is " %c%c]" and the tens character is a literal space
 *   below ten (0x00483ca7):
 *
 *       tens  = n < 10 ? ' ' : '0' + n / 10
 *       units = '0' + n % 10
 *
 *   which is the same width as a digit in this face, so the bracket
 *   stays put. "%2d]" would have been the obvious way and would
 *   have given a different column.
 *
 * o THE BEST LAP ROW TURNS GREEN - Font_SetForeColour from the
 *   colour table's 0x004f0df4 onto the Chrono font, then a LUT
 *   rebuild, then back afterwards (0x00483b91). Only when the race
 *   is over and there is more than one lap.
 */
static void hud_draw_lap_list(void)
{
	const MR_Racer *r     = race_viewed_racer();
	int             laps  = race_lap_count();
	int             first, last, lap, row, shown;
	int             best  = -1;
	int             done;
	char            text[32];
	int             x;
	int             div;

	/*
	 * 0x004838a0: the original's gate also has a REPLAY arm and a
	 * SPECTATOR arm - not transcribed, so this draws for the viewed
	 * racer in every mode.
	 */
	if (!race_ready() || !r || laps <= 0)
		return;

	hud_race_font_load();
	hud_bestlap_font_load();
	if (!s_raceFontOk || !s_blFontOk)
		return;

	done = (r->lapCounter >= laps);

	/*
	 * THE AUTHORED COORDINATES HALVE, and this block has to do it
	 * itself. Font_DrawTextInternal halves x and y when its first
	 * argument is 0 and SmallFonts is on or the mode is not 2, and
	 * Font_DrawText passes that 0 - so every coordinate below is
	 * authored at full size and arrives halved.
	 *
	 * The port applies it inside font_draw_text_ALIGNED only, so a
	 * block drawing through plain font_draw_text - which this one
	 * does, everything here being flag 0x21, no alignment - has to
	 * DIVIDE FOR ITSELF. Omit it and the rows space at 16 where
	 * they should be 8, which also drops the second row out of the
	 * composite rectangle.
	 */
	div = font_layout_divisor();

	/* The banner, font id 11. */
	font_set_colors(&s_sprFont, kHudWhite, NULL);
	font_draw_text(&s_sprFont, HUD_LAP_BANNER_X / div,
	               HUD_LAP_BANNER_Y / div, HUD_SPR_LAP, 0);

	/*
	 * THE COUNTER READS "lap OF laps" AND IS CLAMPED, 0x00483a1a:
	 * lapCounter + 1, but never past the lap total - so the last
	 * lap shows 4<4 rather than 5<4.
	 *
	 * ITS X IS THE BANNER DRAW'S OUT-PARAMETER PLUS TEN.
	 * Font_DrawText's seventh argument is written back and the
	 * original adds 0xa to it. THE VALUE THIS USE NEEDS IS READ -
	 * the note below measures it as the end x of the drawn text -
	 * so measuring the banner and adding ten reproduces it.
	 *
	 * [open] THE REST OF THAT ARGUMENT IS NOT. It is an int array
	 * and hud_draw_pos_only reads slots 0 and 2 from it; only the
	 * end x has been measured.
	 */
	shown = r->lapCounter + 1;
	if (shown > laps)
		shown = laps;
	sprintf(text, HUD_LAP_COUNT_FMT, shown, laps);

	/*
	 * MEASURED, AND THE OUT-PARAMETER IS NOW KNOWN. A hook on
	 * 0x00483a2f caught EBX at 0x4d, 77, with the banner drawn
	 * at x 1 - authored 2, halved - and the Sprites glyph 'B'
	 * 66 wide. 1 + 66 + 10 = 77, so Font_DrawText's seventh
	 * argument is the END X OF THE DRAWN TEXT: the post-
	 * alignment x plus the measured width, in real pixels.
	 *
	 * o SO THE GAP IS NOT HALVED AND THE WIDTH IS NOT EITHER.
	 *   Only the banner's own x is authored. And the counter
	 *   goes through FUN_00493f60, which passes 1 to
	 *   Font_DrawTextInternal and so does NOT halve - which is
	 *   why 77 can be a real coordinate at all.
	 */
	x = HUD_LAP_BANNER_X / div
	    + font_text_width(&s_sprFont, HUD_SPR_LAP)
	    + HUD_LAP_COUNTER_DX;
	font_set_colors(&s_chronoFont, kHudWhite, kHudBlack);
	/* y is NOT halved either - same non-halving draw. */
	font_draw_text(&s_chronoFont, x, HUD_LAP_BANNER_Y, text, 0);

	if (done) {
		int i;

		first = 0;
		for (i = 0; i < laps && i < RACE_LAP_SLOTS; i++) {
			int t = race_viewed_lap_time(i);

			if (t > 0 && (best < 0 || t < race_viewed_lap_time(best)))
				best = i;
		}
	} else {
		first = r->lapCounter - HUD_LAP_WINDOW;
	}

	if (first < 0)
		first = 0;
	last = r->lapCounter;
	if (last > laps - 1)
		last = laps - 1;

	/*
	 * DAT_004f0dc4: the original masks individual rows out of this
	 * loop - not transcribed, so every row in the window draws.
	 */
	for (lap = first, row = 0; lap <= last; lap++, row++) {
		int y  = (HUD_LAP_ROW_Y + row * HUD_LAP_ROW_DY) / div;
		int ms = race_viewed_lap_time(lap);
		int n  = lap + 1;
		char mark[8];

		/* The row number, hand-padded - see the note above. */
		mark[0] = ' ';
		mark[1] = (n < 10) ? ' ' : (char)('0' + n / 10);
		mark[2] = (char)('0' + n % 10);
		mark[3] = ']';
		mark[4] = 0;
		font_set_colors(&s_nameFont, kHudWhite, NULL);
		font_draw_text(&s_nameFont, 0, y, mark, 0);

		if (lap > r->lapCounter) {
			strcpy(text, HUD_LAP_TIME_NONE);
		} else {
			int sec = ms / 1000;

			sprintf(text, HUD_LAP_TIME_FMT, (sec / 60) % 60,
			        sec % 60, (ms % 1000) / 10);
		}

		/* The best lap goes green once the race is over. */
		if (done && laps > 1 && lap == best)
			font_set_colors(&s_chronoFont, kHudGreen, kHudBlack);
		else
			font_set_colors(&s_chronoFont, kHudWhite, kHudBlack);

		font_draw_text(&s_chronoFont, HUD_LAP_TIME_X / div, y, text, 0);

		/*
		 * 0x00483967 and 0x00483c68: the track-record TEST and the
		 * "[]" marker at x 0xc0 it gates, drawn in the Player face
		 * when this lap set the record. Both need the record name
		 * and time globals at 0x005d25b0 / 0x005d26a5 - not
		 * transcribed.
		 */
	}
}

/* The rectangle the lap list occupies, for the overlay composite. */
int hud_laplist_overlay_rect(int *x, int *y, int *w, int *h)
{
	const MR_Racer *r;
	int             rows, div, y1;

	if (!race_ready() || !s_raceFontOk || !s_blFontOk)
		return 0;
	if (!hud_race_hud_visible() || hud_race_over())
		return 0;              /* see hud_race_over */

	r = race_viewed_racer();
	if (!r || race_lap_count() <= 0)
		return 0;

	rows = r->lapCounter + 1;
	if (rows > HUD_LAP_WINDOW + 1)
		rows = HUD_LAP_WINDOW + 1;
	if (rows < 1)
		rows = 1;

	div = font_layout_divisor();
	y1  = (HUD_LAP_ROW_Y + rows * HUD_LAP_ROW_DY) / div
	      + font_height(&s_chronoFont) + HUD_COUNTER_PAD;
	if (y1 > g_clipHeight)
		y1 = g_clipHeight;

	if (x) *x = 0;
	if (y) *y = 0;
	/*
	 * [open] HUD_LAP_TIME_X TWICE, halved as the inset and whole as
	 * a stand-in for the time's width. Fits at div 1, over-wide at
	 * div 2. font_text_width on HUD_LAP_TIME_NONE would measure it,
	 * as every other rect here does.
	 */
	if (w) *w = HUD_LAP_TIME_X / div + HUD_LAP_TIME_X;
	if (h) *h = y1;
	return 1;
}



/* ================================================================== */
/* POS - the position and the field size, under the countdown.        */
/*                                                                    */
/* ORIGIN  FUN_00482380 @ 0x00482380, the second half at 0x00482488   */
/* ================================================================== */

/* "A" is the small RED "POS"; see the Sprites sheet. */
#define HUD_SPR_POS       "A"

#define HUD_POS_BANNER_X  0
#define HUD_POS_BANNER_Y  0x96   /* 150 */
#define HUD_POS_FIELD_X   8
#define HUD_POS_FIELD_Y   0xab   /* 171 */

/*
 * '<' IS THE SEPARATOR GLYPH of the Chrono face, and the string is
 * the literal "<%d" at 0x004f1060 - what reads on screen as "/12".
 * The number is the count of active racers.
 */
#define HUD_POS_FIELD_FMT "<%d"

/*
 * PosHiRes, and the file is POSTIME.FNT - the SAME face the
 * countdown uses.
 *
 * IT IS NOT POSTIMEL.FNT, which is 'A'..'Z' WITH NO DIGITS AT ALL.
 * font_draw_text skips a character the face does not carry, so a
 * position number drawn through POSTIMEL renders NOTHING while its
 * banner and "/12" show normally - a blank where a digit belongs,
 * with nothing logged.
 *
 * Read off the LIVE font object: g_fontTable's Pos entry has widths
 * 25 at 0x20, 1 across 0x21..0x2f and 23 across 0x30..0x39, and
 * glyphHeight 17 - an exact match for LOWRES\POSTIME.FNT and
 * nothing else.
 *
 * o DO NOT IDENTIFY A FACE BY ELIMINATION AGAINST THE ARCHIVE. Two
 *   font ids naming one file is ordinary here - 3, 13 and 19 all
 *   sit over PosTime - so "this name is taken" proves nothing.
 */
static MR_Font s_posFont;      /* PosHiRes, POSTIME.FNT */
static int     s_posFontOk;
static int     s_posFontTried;
static int     s_posFontSmall = -1;

static void hud_pos_font_load(void)
{
	/* The same SmallFonts reload as hud_race_font_load - see there. */
	if (s_posFontTried && s_posFontSmall == g_options.smallFonts)
		return;
	if (s_posFontTried && s_posFontOk) {
		font_free(&s_posFont);
		s_posFontOk = 0;
	}
	s_posFontTried = 1;
	s_posFontSmall = g_options.smallFonts;

	s_posFontOk =
	    fontdef_open(&s_posFont, HUD_FONT_POS);

	if (!s_posFontOk)
		mrlog_err("hud: position font did not load\n");
}

/* ORIGIN  FUN_00482380 @ 0x00482380, the second half at 0x00482488 */
static void hud_draw_pos(void)
{
	char text[16];
	int  pos, field, w;

	if (!race_ready())
		return;

	hud_pos_font_load();
	if (!s_posFontOk || !s_blFontOk || !s_raceFontOk)
		return;

	/*
	 * ONE-BASED, AND NOTHING IS DRAWN WHEN THE RACER IS NOT IN THE
	 * STANDINGS. maybe_Race_FinishingPosition returns -1 then, and
	 * the original's `if (0 < iVar1)` after the increment drops the
	 * whole block.
	 */
	pos = race_finishing_position(race_viewed_index()) + 1;
	if (pos <= 0)
		return;

	field = race_standings_count();

	font_set_colors(&s_sprFont, kHudWhite, NULL);
	font_draw_text_aligned(&s_sprFont, HUD_POS_BANNER_X,
	                       HUD_POS_BANNER_Y, HUD_SPR_POS,
	                       HUD_ALIGN_RIGHT, 0);

	sprintf(text, HUD_POS_FIELD_FMT, field);
	font_draw_text_aligned(&s_chronoFont, HUD_POS_FIELD_X,
	                       HUD_POS_FIELD_Y, text, HUD_ALIGN_RIGHT, 0);

	/*
	 * THE NUMBER SITS LEFT OF THE BANNER, on the banner's own row.
	 *
	 * The original computes its inset from the same unread
	 * out-parameter the best-lap time uses - iVar7 at 0x0048250a,
	 * filled by the POS banner's draw - and pre-doubles it. Ours
	 * builds the inset from the banner's measured width instead and
	 * multiplies by the divisor so the halving cancels, exactly as
	 * hud_draw_best_lap does.
	 *
	 * [open] Only the END X slot of that out-parameter has been
	 * measured - hud_draw_lap_list's banner note. Which slot
	 * iVar7 takes here is not established.
	 */
	w = font_text_width(&s_sprFont, HUD_SPR_POS);
	sprintf(text, "%d", pos);
	font_draw_text_aligned(&s_posFont,
	                       (w + HUD_POS_FIELD_X) * font_layout_divisor(),
	                       HUD_POS_BANNER_Y, text, HUD_ALIGN_RIGHT, 0);
}

int hud_pos_overlay_rect(int *x, int *y, int *w, int *h)
{
	int div, y0, y1;

	if (!race_ready() || !s_posFontOk || !hud_race_hud_visible())
		return 0;
	if (hud_race_over())            /* see hud_race_over */
	    return 0;
	if (race_finishing_position(race_viewed_index()) < 0)
		return 0;

	div = font_layout_divisor();

	/*
	 * WHICHEVER OF THE TWO LAYOUTS IS LIVE. The practice block
	 * sits sixty pixels higher, and a rectangle built for the
	 * other one composites a band the text is not in - the same
	 * failure the halving causes, reached a different way.
	 */
	if (hud_time_and_pos()) {
		y0 = HUD_POS_BANNER_Y / div - HUD_COUNTER_PAD;
		y1 = HUD_POS_FIELD_Y / div + font_height(&s_chronoFont)
		     + HUD_COUNTER_PAD;
	} else {
		y0 = HUD_POSONLY_BANNER_Y / div - HUD_COUNTER_PAD;
		y1 = HUD_POSONLY_FIELD_Y / div + font_height(&s_chronoFont)
		     + HUD_COUNTER_PAD;
	}

	if (y0 < 0) y0 = 0;
	if (y1 > g_clipHeight) y1 = g_clipHeight;
	if (y1 <= y0)
		return 0;

	if (x) *x = 0;
	if (y) *y = y0;
	if (w) *w = g_clipWidth;
	if (h) *h = y1 - y0;
	return 1;
}

/* ================================================================== */
/* POS ALONE - THE PRACTICE LAYOUT.                                   */
/*                                                                    */
/* ORIGIN  Hud_DrawPosOnly @ 0x00482620, whole                        */
/* ================================================================== */

/*
 * THE SAME THREE FONTS AND THE SAME THREE STRINGS as hud_draw_pos,
 * at different coordinates. It is not a variant of that function in
 * the original either - it is its own 0x12c bytes, reached from the
 * dispatch's other arm.
 *
 * o THREE PRECONDITIONS, ALL THE ORIGINAL'S. The viewed racer's
 *   raceState must be non-zero (0x00482641), there must be more than
 *   one racer in the standings (0x0048264e), and the finishing
 *   position must not be -1 (0x00482665). Alone on the track it
 *   draws nothing at all, which is the retail behaviour.
 */
static void hud_draw_pos_only(void)
{
	const MR_Racer *r;
	char            text[16];
	int             pos, field, w;

	if (!race_ready())
		return;

	hud_pos_font_load();
	if (!s_posFontOk || !s_blFontOk || !s_raceFontOk)
		return;

	r = race_viewed_racer();
	if (!r || r->raceState == 0)
		return;

	field = race_standings_count();
	if (field < 2)
		return;

	pos = race_finishing_position(race_viewed_index()) + 1;
	if (pos <= 0)
		return;

	font_set_colors(&s_sprFont, kHudWhite, NULL);
	font_draw_text_aligned(&s_sprFont, HUD_POSONLY_X,
	                       HUD_POSONLY_BANNER_Y, HUD_SPR_POS,
	                       HUD_ALIGN_RIGHT, 0);

	sprintf(text, HUD_POS_FIELD_FMT, field);
	font_draw_text_aligned(&s_chronoFont, HUD_POSONLY_X,
	                       HUD_POSONLY_FIELD_Y, text,
	                       HUD_ALIGN_RIGHT, 0);

	/*
	 * THE NUMBER'S INSET COMES FROM THE "/N" FIELD, NOT THE BANNER,
	 * and that is the one thing that differs from hud_draw_pos - the
	 * out-parameter is passed on the CHRONO draw here (0x004826c7)
	 * and on the banner draw there. Measure before the scratch is
	 * overwritten.
	 *
	 * The original computes 2*out[2] - 2*out[0] + 9 at 0x0048270f,
	 * doubling only the two out values and not the 9 - so the 9 is
	 * in pre-halving units like every other coordinate here, and
	 * the width is multiplied by the divisor to survive the halving
	 * font_draw_text_aligned applies. Ours measures the string
	 * instead of reading the out-parameter, exactly as hud_draw_pos
	 * and hud_draw_best_lap do.
	 *
	 * [open] It is an int ARRAY and this reads slots 0 and 2. Only
	 * the end x has been measured - see hud_draw_lap_list's banner
	 * note - so which slot holds what is still unread, and that is
	 * the one place the measured-width substitution could be
	 * wrong.
	 */
	w = font_text_width(&s_chronoFont, text);
	sprintf(text, "%d", pos);
	font_draw_text_aligned(&s_posFont,
	                       w * font_layout_divisor() + HUD_POSONLY_INSET,
	                       HUD_POSONLY_NUMBER_Y, text,
	                       HUD_ALIGN_RIGHT, 0);
}

/* ================================================================== */
/* THE FINISHING POSITION, after the flag.                            */
/*                                                                    */
/* ORIGIN  FUN_00481970 @ 0x00481970, whole                           */
/* ================================================================== */

/*
 * "1st", "2nd", "3rd", "4th" - the readout that appears once the
 * race is over.
 *
 * IT IS NOT PART OF Hud_DrawInRace, and that is why adding that
 * function's finished arm did not produce it. The HUD dispatcher one
 * level up (FUN_004806e0) has a whole post-race branch of its own,
 * and this is what it calls. The in-race HUD simply stops drawing;
 * something else starts.
 *
 * THE SUFFIX IS A GLYPH, NOT A STRING. The format at 0x004f1044 is
 * "%d%c" and the character is picked by position:
 *
 *     0 -> ':'   1 -> ';'   2 -> '<'   3 and up -> '='
 *
 * so the face carries st, nd, rd and th at 0x3a..0x3d and the code
 * never spells them. A port that wrote "1st" would need four
 * strings and would get the wrong glyphs.
 *
 * THE POSITION COMES FROM TWO PLACES, by state: a FINISHED racer
 * uses the finishPosition it was given when it crossed, and anyone
 * else is asked live. They differ - the live answer moves as the
 * field comes home behind you, and the stored one does not.
 *
 * THE FACE IS FINALPOS.FNT - see hud_place_font_load for how that
 * was settled. Menu_DrawText is handed font 5 and align 0xc2; the
 * port has no id-5 mapping, but only one shipped face carries the
 * glyphs this function needs, so the id did not have to be resolved
 * to find it.
 */
/*
 * FINALPOS.FNT, AND THE NAME IS THE EVIDENCE.
 *
 * The readout draws "%d%c" where the char is a suffix glyph, so the
 * face has to carry the digits AND 0x3a..0x3d. Read out of the
 * shipped files, FINALPOS is chars 0x2f..0x3d - digits 0x30..0x39
 * plus exactly those four - and nothing else in FONTES covers them.
 * HiRes is 39 tall with glyphs up to 375 wide, which is what a
 * full-width "1st" across the screen wants; the LowRes twin is the
 * same range at 19.
 *
 * AND THE WRONG FACE FAILS SILENTLY. font_draw_text SKIPS A
 * CHARACTER THE FACE DOES NOT CARRY rather than substituting one,
 * so a letters-only face - GET READY's, say - draws a digit as
 * nothing at all and leaves no trace. A FACE THAT DOES NOT CARRY
 * THE GLYPH IS INDISTINGUISHABLE FROM A BLOCK THAT NEVER RAN.
 *
 * Third site in this file, after POSTIMEL.FNT and PosTimeL. When a
 * readout is missing entirely, suspect the face before the dispatch.
 */
static MR_Font s_placeFont;
static int     s_placeFontOk;
static int     s_placeFontTried;
static int     s_placeFontSmall = -1;

static void hud_place_font_load(void)
{
	/* The same SmallFonts reload as hud_race_font_load - see there. */
	if (s_placeFontTried && s_placeFontSmall == g_options.smallFonts)
		return;
	if (s_placeFontTried && s_placeFontOk) {
		font_free(&s_placeFont);
		s_placeFontOk = 0;
	}
	s_placeFontTried = 1;
	s_placeFontSmall = g_options.smallFonts;

	s_placeFontOk =
	    /*
	     * "Pal", NOT AntiAlias8Transp. FINALPOS.FNT carries 256
	     * palette entries, and fnt.py's rule is that a face with a
	     * palette block is one the game declares as Pal - the
	     * AntiAlias8 and Direct faces have none. Loaded as
	     * AntiAlias8Transp the glyphs come out grey where retail
	     * draws them green, because the shaded blit ignores the
	     * palette the file brought with it.
	     */
	    fontdef_open(&s_placeFont, HUD_FONT_PLACE);

	if (!s_placeFontOk)
		mrlog_err("hud: finishing-position font did not load\n");
}


/*
 * ORIGIN  FUN_00481970 @ 0x00481970, at 0x004819f9
 *
 * The suffix glyph for a finishing position - see
 * hud_draw_finish_place. Written as the original's chain rather
 * than as a table, because the last arm is an arithmetic fallthrough
 * ((pos != 2) + '<') and a table would hide that the fourth glyph
 * covers every place from fourth down.
 */
static char hud_place_suffix(int place)
{
	if (place == 0)
		return ':';
	if (place == 1)
		return ';';
	return (char)((place != 2) + '<');
}


static void hud_draw_finish_place(void)
{
	const MR_Racer *r = race_viewed_racer();
	char            text[8];
	int             place;
	int             tw, th, px, py;

	if (!race_ready() || !r)
		return;

	/*
	 * THE OUTER GATE IS g_nRaceOverHud, NOT raceState - one global
	 * about the LOCAL racer, tested by the dispatcher's post-race
	 * branch. The viewed racer's raceState turns non-zero at the
	 * start line, which would put this on screen for the whole
	 * race.
	 */
	if (!race_over_hud())
		return;

	/*
	 * THE PLAQUE WINS. race.c draws a 3D 1ST/2ND/3RD for the
	 * first three places of a real race; this text readout is
	 * what the original falls back to when that declines -
	 * practice, or fourth and worse. Never both.
	 */
	if (race_final_place() >= 0)
		return;

	hud_place_font_load();
	if (!s_placeFontOk)
		return;

	/*
	 * THE ORIGINAL'S GATES, less the network and ghost ones this
	 * port cannot reach: the racer must have started racing at all
	 * (raceState non-zero) and have some clock on it.
	 */
	if (r->raceState == 0 || r->raceClockMs <= 0)
		return;

	/* raceState 2 is FINISHED. */
	place = (r->raceState == 2)
	        ? r->finishPosition
	        : race_finishing_position(race_viewed_index());
	if (place < 0)
		return;

	/*
	 * "%d%c/", 0x004f1044 - AND 0x2f IS THE WORD POS, not a
	 * solidus: 375 pixels against 43 for a suffix and 26..61 for a
	 * digit. Drop it and the number and suffix still draw, so it
	 * reads as a font fault rather than a missing character.
	 */
	sprintf(text, "%d%c/", place + 1, hud_place_suffix(place));

	/*
	 * THE MENU SCHEME, NOT THE RACE HUD ONE. This goes through
	 * Menu_DrawText, so 0xc2 is hud_align's (Text_ApplyAlignment),
	 * not font_draw_text_aligned's. The same bits mean different
	 * things: the menu scheme centres it, the race scheme leaves y
	 * at 0x19 - 150-odd pixels too high. hud-race.txt 1.2.
	 *
	 * NO font_layout_divisor EITHER: that halving is
	 * Font_DrawTextInternal's and the menu path skips it.
	 */
	tw = font_text_width(&s_placeFont, text);
	th = font_height(&s_placeFont);
	px = HUD_PLACE_X;
	py = HUD_PLACE_Y;
	hud_align(tw, th, &px, &py, HUD_ALIGN_TITLE);

	/* NO font_set_colors - the Pal class has no set_colors hook
	 * and the face's own palette carries the colour. */
	font_draw_text(&s_placeFont, px, py, text, 0);
}


int hud_place_overlay_rect(int *x, int *y, int *w, int *h)
{
	int px, th, y0, y1;

	if (!race_ready() || !s_placeFontOk || !race_over_hud())
		return 0;
	if (race_final_place() >= 0)      /* see the draw */
	    return 0;

	/* The SAME placement the draw computes - hud_align, no
	 * divisor. A rect that disagrees with the draw clears one
	 * band and paints another. */
	th = font_height(&s_placeFont);
	px = HUD_PLACE_X;
	y0 = HUD_PLACE_Y;
	hud_align(0, th, &px, &y0, HUD_ALIGN_TITLE);
	y0 -= HUD_COUNTER_PAD;
	y1  = y0 + th + 2 * HUD_COUNTER_PAD;

	if (y0 < 0) y0 = 0;
	if (y1 > g_clipHeight) y1 = g_clipHeight;
	if (y1 <= y0)
		return 0;

	if (x) *x = 0;
	if (y) *y = y0;
	if (w) *w = g_clipWidth;
	if (h) *h = y1 - y0;
	return 1;
}


/* ================================================================== */
/* THE IN-RACE MAP.                                                   */
/*                                                                    */
/* ORIGIN  Hud_DrawMap @ 0x0040f2d0, the LINE path                    */
/*                                                                    */
/* See docs/subsystems/hud-map.txt. trk_build_map makes the outline.  */
/* ================================================================== */

/*
 * FIVE MODES, cycled by F9. Two independent choices plus off:
 *
 *     0  rotating, corner      2  fixed, corner
 *     1  rotating, centred     3  fixed, centred
 *     4  off
 *
 * "rotating" turns the map to the viewed racer's yaw so the rider
 * always points up the screen; "fixed" holds the map still and lets
 * the riders move around it.
 */
#define HUD_MAP_MODES        5
#define HUD_MAP_OFF          4

/*
 * 0x0040f2f2 and 0x0040f5a0, and the pairs the original selects
 * with g_viewWidescreen. THE _W VALUES ARE FOR A CROPPED FRAME:
 * retail's 16:9 cuts the height to 3/4, and these put the map back
 * where the lost height would otherwise have pushed it. A frame
 * that is wider rather than shorter wants the plain values and an
 * X correction instead - see hud_map_widen_dx.
 */
#define HUD_MAP_CORNER_X    (-2000.0f)
#define HUD_MAP_CORNER_Y    (-1200.0f)
#define HUD_MAP_CORNER_Y_W   (-500.0f)
#define HUD_MAP_CORNER_Z     4000.0f
#define HUD_MAP_CENTRE_Z     2000.0f
#define HUD_MAP_CENTRE_Z_W   2500.0f

/* [0x004d0338], the per-frame ease toward the target placement. */
#define HUD_MAP_EASE         0.2f

/* Rider triangle half-size, 0x0040f7c8 - smaller on the big map. */
#define HUD_MAP_TRI          50.0f
#define HUD_MAP_TRI_CENTRED  30.0f

/* Screen3D_QueuePrimitive's depth argument, as float bit patterns. */
#define HUD_MAP_DEPTH_LINE   15.0f   /* 0x41700000 */
#define HUD_MAP_DEPTH_RIDER  10.0f   /* 0x41200000 */

#define HUD_MAP_RIDERS_MAX   25      /* Trk_BuildMap's fixed 25 tris */

/*
 * THE DRAW READS flatColor, NOT colorIndex. drawscene_dispatch
 * takes the 0x00RRGGBB word and never looks at the index, so a
 * colour written there draws with whatever flatColor holds - zero
 * for a hand-built mesh, black on a dark track. These expand the
 * original's five-bit values to eight.
 */
#define HUD_MAP_C8(v)  ((unsigned int)(((v) * 255) / 31))
#define HUD_MAP_RGB(r, g, b) \
    ((HUD_MAP_C8(r) << 16) | (HUD_MAP_C8(g) << 8) |\
     HUD_MAP_C8(b))

static MR_Primitive *s_mapPrims;
static int           s_mapPrimCount;
static const MR_Trk *s_mapTrk;

static MESH          s_mapRiderMesh;
static MR_Primitive *s_mapRiderPrims;
static int           s_mapRiderPrimCount;

/*
 * The live placement, eased toward the target every frame.
 * Trk_BuildMap seeds it to the corner values at 0x0040f288.
 */
static float s_mapPlace[3] = {
	HUD_MAP_CORNER_X, HUD_MAP_CORNER_Y, HUD_MAP_CORNER_Z
};

void hud_map_reset(void)
{
	if (s_mapPrims) {
		free(s_mapPrims);
		s_mapPrims = NULL;
	}
	if (s_mapRiderPrims) {
		free(s_mapRiderPrims);
		s_mapRiderPrims = NULL;
	}
	mesh_free(&s_mapRiderMesh);
	memset(&s_mapRiderMesh, 0, sizeof(s_mapRiderMesh));
	s_mapPrimCount      = 0;
	s_mapRiderPrimCount = 0;
	s_mapTrk            = NULL;
	s_mapPlace[0] = HUD_MAP_CORNER_X;
	s_mapPlace[1] = HUD_MAP_CORNER_Y;
	s_mapPlace[2] = HUD_MAP_CORNER_Z;
}

/*
 * ORIGIN  Trk_BuildMap @ 0x0040ed10, the rider mesh at 0x0040f200
 *
 * TWENTY-FIVE TRIANGLES, FIXED, built once - 75 vertices and 25
 * primitives, one per racer with a red default. The field is capped
 * at 25 on the map whatever the racer count is, and that is the
 * original's cap, not ours.
 */
static int hud_map_build_riders(void)
{
	int i;

	if (s_mapRiderPrims)
		return 1;

	s_mapRiderMesh.vertexCount = HUD_MAP_RIDERS_MAX * 3;
	s_mapRiderMesh.faceCount   = HUD_MAP_RIDERS_MAX;
	s_mapRiderMesh.vertices    = (MESH_VERTEX *)
	    calloc((size_t)s_mapRiderMesh.vertexCount, sizeof(MESH_VERTEX));
	s_mapRiderMesh.faces       = (MESH_FACE *)
	    calloc((size_t)s_mapRiderMesh.faceCount, sizeof(MESH_FACE));
	if (!s_mapRiderMesh.vertices || !s_mapRiderMesh.faces) {
		mesh_free(&s_mapRiderMesh);
		memset(&s_mapRiderMesh, 0, sizeof(s_mapRiderMesh));
		return 0;
	}

	for (i = 0; i < HUD_MAP_RIDERS_MAX; i++) {
		s_mapRiderMesh.faces[i].count      = 3;
		s_mapRiderMesh.faces[i].flags      = 0;
		s_mapRiderMesh.faces[i].index[0]   = i * 3;
		s_mapRiderMesh.faces[i].index[1]   = i * 3 + 1;
		s_mapRiderMesh.faces[i].index[2]   = i * 3 + 2;
		s_mapRiderMesh.faces[i].mapRecordN = -1;
	}

	if (!prim_build_from_mesh(&s_mapRiderMesh, &s_mapRiderPrims,
	                          &s_mapRiderPrimCount)) {
		mesh_free(&s_mapRiderMesh);
		memset(&s_mapRiderMesh, 0, sizeof(s_mapRiderMesh));
		return 0;
	}
	return 1;
}

/*
 * hud_map_widen_dx - the corner map's X correction for a frame WIDER
 * than the design one.
 *
 * ORIGIN  NONE - ours, and it has to be: the original has no mode
 *         that changes the width. Screen3D_SetViewport's widescreen
 *         branch computes height * 3 / 4 and leaves the width alone.
 *
 * The map is a camera-space point projected with a resolution-
 * independent lens, so its offset from the centre is a fixed pixel
 * count while the centre moves with the frame: widen the frame and
 * the map slides inward by half the extra width.
 *
 *     dx = -(W - FB_DESIGN_W)/2 * z / lens
 *
 * Exactly zero at 640, so 4:3 and the cropped 16:9 keep the
 * original's own numbers. THE LENS IS READ LIVE, not written as
 * 394, because the map's size and its offset both scale with it.
 */
static float hud_map_widen_dx(void)
{
	float extra = (float)(g_frameWidth - FB_DESIGN_W) * 0.5f;

	if (extra <= 0.0f || g_projScaleX <= 0.0f)
		return 0.0f;
	return -extra * (HUD_MAP_CORNER_Z / g_projScaleX);
}

/*
 * ORIGIN  Hud_DrawMap @ 0x0040f2d0
 *
 * o THE PLACEMENT EASES, IT DOES NOT SNAP. Each component moves a
 *   fraction of the way to its target every frame, so changing mode
 *   slides the map across instead of cutting.
 *
 * o THE WIDESCREEN CONSTANTS ARE THE ORIGINAL'S - see the note on
 *   HUD_MAP_CORNER_Y_W for which frame they are for.
 *
 * o THE VIEWED RACER IS DRAWN LAST and at depth 0, so it sits over
 *   the field.
 */
void hud_draw_map(void)
{
	const MR_Trk *t;
	MR_Vertex    *v;
	float         m[12];
	float         target[3];
	int           centred;
	int           rotating;
	int           cropped;
	int           i;
	int           n;
	unsigned int   grey;

	if (!race_ready())
		return;
	if (g_options.mapMode < 0 || g_options.mapMode >= HUD_MAP_OFF)
		return;

	t = race_trk();
	if (!t || t->mapMesh.vertexCount <= 0)
		return;

	if (s_mapTrk != t) {
		if (s_mapPrims) {
			free(s_mapPrims);
			s_mapPrims = NULL;
		}
		s_mapPrimCount = 0;
		s_mapTrk = t;
	}
	if (!s_mapPrims) {
		if (!prim_build_from_mesh(&t->mapMesh, &s_mapPrims,
		                          &s_mapPrimCount)) {
			return;
		}

		/*
		 * THE LINE FLAG GOES ON THE PRIMITIVE, NOT THE FACE - the
		 * original's order too, 0x10 stamped at +0x38 after
		 * Face_InitTriangle. prim_build_from_mesh derives its flags
		 * from the .map data and DROPS a flag set on MESH_FACE, so
		 * without this every outline draws as a zero-area triangle:
		 * nothing on screen, no error.
		 */
		for (i = 0; i < s_mapPrimCount; i++)
			s_mapPrims[i].flags |= PRIM_FLAG_LINE;
	}
	if (!hud_map_build_riders())
		return;

	rotating = (g_options.mapMode == 0 || g_options.mapMode == 1);
	centred  = (g_options.mapMode == 1 || g_options.mapMode == 3);
	/*
	 * ASK ABOUT THE HEIGHT, NOT THE ASPECT. The original's test is
	 * the g_viewWidescreen FLAG at 0x0040f2fe, and its one
	 * widescreen mode is a CROP - so for retail "wide" and
	 * "shorter" coincide and either test would do.
	 *
	 * They come apart on a true 16:9, which grows the width and
	 * leaves the height alone. Both constants below undo a loss of
	 * HEIGHT, so an aspect test applies them to a frame that has
	 * lost none: corner map 69 pixels too high, centred one 20 per
	 * cent too small.
	 */
	cropped  = (g_frameHeight < FB_DESIGN_H);

	if (!centred) {
		target[0] = HUD_MAP_CORNER_X + hud_map_widen_dx();
		target[1] = cropped ? HUD_MAP_CORNER_Y_W : HUD_MAP_CORNER_Y;
		target[2] = HUD_MAP_CORNER_Z;
	} else {
		/*
		 * THE CENTRED MAP NEEDS NO X CORRECTION. It is at x = 0,
		 * which projects to the centre of the frame whatever the
		 * width is. Only the corner one is measured from an edge.
		 */
		target[0] = 0.0f;
		target[1] = 0.0f;
		target[2] = cropped ? HUD_MAP_CENTRE_Z_W : HUD_MAP_CENTRE_Z;
	}
	for (i = 0; i < 3; i++) {
		s_mapPlace[i] += (target[i] - s_mapPlace[i]) * HUD_MAP_EASE;
	}

	/*
	 * THE MATRIX. Identity for the fixed modes; a yaw about the
	 * screen normal for the rotating ones, taken from the viewed
	 * racer so the rider ends up pointing up the screen.
	 */
	for (i = 0; i < 9; i++)
		m[i] = 0.0f;
	m[0] = m[4] = m[8] = 1.0f;
	if (rotating) {
		const MR_Racer *r = race_viewed_racer();

		/*
		 * FROM moveDir, NOT FROM A YAW ANGLE. The original calls
		 * Matrix34_YawOf on the racer and feeds the result through
		 * g_cosTable; this port has no equivalent, and the racer's
		 * heading is already a unit vector at +0x18c. Building the
		 * rotation straight from it is the same rotation without a
		 * round trip through an angle. [deliberate divergence]
		 */
		if (r) {
			float dx  = r->moveDir[0];
			float dz  = r->moveDir[2];
			float len = (float)sqrt((double)(dx * dx + dz * dz));

			if (len > 1.0e-6f) {
				dx /= len;
				dz /= len;
				/*
				 * R SUCH THAT R . (dx, dz) = (0, 1), which puts
				 * the heading up the screen. The transform reads
				 * camX from m[0],m[3] and camY from m[1],m[4].
				 *
				 * SWAPPING m[1] AND m[3] GIVES THE TRANSPOSE -
				 * for a rotation that is the INVERSE, so the map
				 * turns the wrong way and reads as mirrored.
				 */
				m[0] =  dz;  m[3] = -dx;
				m[1] =  dx;  m[4] =  dz;
			}
		}
	}
	m[9]  = s_mapPlace[0];
	m[10] = s_mapPlace[1];
	m[11] = s_mapPlace[2];

	/*
	 * THE OUTLINE, baked at map scale by trk_build_map, so this is
	 * only the placement transform.
	 *
	 * THE FOURTH ARGUMENT IS A SHADE, NOT A FLAG WORD. The original
	 * passes 0x10 for the corner map and 0x18 for the centred one,
	 * which is half of why the large map reads lighter; the colour
	 * index is the other half.
	 */
	v = transform_verts_flat(m, (const float *)t->mapMesh.vertices,
	                         t->mapMesh.vertexCount,
	                         centred ? 0x18 : 0x10);
	if (!v)
		return;

	grey = centred ? HUD_MAP_RGB(0x10, 0x10, 0x10)
	               : HUD_MAP_RGB(8, 8, 8);
	for (i = 0; i < s_mapPrimCount; i++) {
		/*
		 * THE FIRST PRIMITIVE IS WHITE, and it is the START LINE.
		 * The outline is walked from segment 0, so primitive 0 is
		 * the piece across the start, and retail shows a light grey
		 * segment there [observed].
		 */
		s_mapPrims[i].flatColor =
		    (i == 0) ? HUD_MAP_RGB(0x1f, 0x1f, 0x1f) : grey;
		queue_primitive(&s_mapPrims[i], HUD_MAP_DEPTH_LINE, v, 2, 0);
	}

	/*
	 * THE RIDERS. Three vertices each, in the map's own space: the
	 * racer's world x and z with the outline's origin taken off and
	 * the same scale applied, then a triangle built around it.
	 */
	n = race_racer_count();
	if (n > HUD_MAP_RIDERS_MAX + 1)
		n = HUD_MAP_RIDERS_MAX + 1;

	for (i = 1; i < n; i++) {
		const MR_Racer *r = race_racer_at(i);
		MESH_VERTEX    *tv = &s_mapRiderMesh.vertices[(i - 1) * 3];
		float           half = centred ? HUD_MAP_TRI_CENTRED
		                               : HUD_MAP_TRI;
		float           x;
		float           y;

		if (!r || !r->active) {
			tv[0].x = tv[0].y = tv[0].z = 0.0f;
			tv[1] = tv[0];
			tv[2] = tv[0];
			continue;
		}

		x = (r->position[0] - t->mapOrigin[0]) * t->mapScale;
		y = (r->position[2] - t->mapOrigin[1]) * t->mapScale;

		/*
		 * AN ARROWHEAD IN THE RACER'S OWN FRAME, 0x0040f858.
		 * The original builds a rotation from the racer's yaw
		 * and puts the three corners at
		 *
		 *     corner 0   pos - size * (R + F)   rear, one side
		 *     corner 1   pos + size * 2.0 * F   the NOSE
		 *     corner 2   pos + size * (R - F)   rear, other
		 *
		 * where the 2.0 is [0x004d02d8]. Three sizes long and
		 * two wide, so which way a rider FACES is readable off
		 * the map. An axis-aligned triangle throws that away
		 * and makes every rider look like he is heading north.
		 *
		 * F is moveDir, the same source the map rotation uses.
		 */
		{
			float fx  = r->moveDir[0];
			float fy  = r->moveDir[2];
			float len = (float)sqrt((double)(fx * fx + fy * fy));
			float rx;
			float ry;

			if (len > 1.0e-6f) {
				fx /= len;
				fy /= len;
			} else {
				fx = 0.0f;
				fy = 1.0f;
			}
			rx =  fy;
			ry = -fx;

			tv[0].x = x - half * (rx + fx);
			tv[0].y = y - half * (ry + fy);
			tv[1].x = x + half * 2.0f * fx;
			tv[1].y = y + half * 2.0f * fy;
			tv[2].x = x + half * (rx - fx);
			tv[2].y = y + half * (ry - fy);
			tv[0].z = 0.0f;
			tv[1].z = 0.0f;
			tv[2].z = 0.0f;
		}
	}

	v = transform_verts_flat(m, (const float *)s_mapRiderMesh.vertices,
	                         s_mapRiderMesh.vertexCount, 0x1f);
	if (!v)
		return;

	for (i = 1; i < n; i++) {
		const MR_Racer *r = race_racer_at(i);

		if (!r || !r->active || i == 1)
			continue;
		if (i - 1 >= s_mapRiderPrimCount)
			break;
		/*
		 * TWO SHADES OF RED, alternating on bit 1 of the racer
		 * index - 0x0040fa10. Not one colour for the whole field.
		 */
		s_mapRiderPrims[i - 1].flatColor =
		    (i & 2) ? HUD_MAP_RGB(0x1f, 0, 0)
		            : HUD_MAP_RGB(0x1f, 8, 8);
		queue_primitive(&s_mapRiderPrims[i - 1],
		                HUD_MAP_DEPTH_RIDER, v, 2, 0);
	}

	/* The viewed racer, YELLOW, last and at depth 0 - which is why
	 * the field loop above skips i == 1 after building its
	 * triangle. */
	if (s_mapRiderPrimCount > 0) {
		s_mapRiderPrims[0].flatColor = HUD_MAP_RGB(0x1f, 0x1f, 0);
		queue_primitive(&s_mapRiderPrims[0], 0.0f, v, 2, 0);
	}
}

/*
 * hud_draw - EVERYTHING THE RACE PUTS OVER ITSELF, in one place.
 *
 * The overlay is ONE buffer and fb_overlay_begin CLEARS it, so two
 * independent two-pass builds would erase each other. Everything
 * that lives in the overlay therefore draws from inside this
 * function's loop, and the callers ask for their own rectangle back
 * afterwards.
 */
void hud_draw(void)
{
	int pass;
	int origin;

	if (!s_open && hud_counter_type() == 0 && !race_ready())
		return;
	if (s_open && !s_fontOk)
		return;

	s_blink++;
	hud_messages_tick();

	/* HERE, not in the draw - hud-race.txt 7. */
	s_calloutRectOk = 0;

	/*
	 * NO 2D ORIGIN ON THE HUD. hud_align already anchors against
	 * the real frame, so a widened frame moves the corners out
	 * where they belong; adding framebuffer.h's block offset on
	 * top of that would pull them 107 pixels back inward. The
	 * save-and-restore is so this does not depend on who called
	 * it or what they had set.
	 */
	origin = g_fb2dOriginX;
	fb_set_2d_origin(0);

	/*
	 * INTO THE OVERLAY LAYER, NOT THE 2D ONE. The 2D layer goes
	 * down before the geometry and already holds the horizon by
	 * now, so text put there lands behind the track and
	 * compositing its rectangle back brings a slab of SKY with it.
	 *
	 * TWICE, OVER BLACK AND OVER WHITE, because the title font
	 * blends with whatever is under it and an overlay has
	 * nothing under it. The difference between the two passes IS
	 * the coverage; see fb_overlay_begin. The blink is advanced
	 * once, above, so both passes draw the same thing - if they
	 * did not, the difference would be read as coverage.
	 */
	for (pass = 0; pass < 2; pass++) {
		if (!fb_overlay_begin(pass))
			break;
		hud_counter_draw_digits();
		/*
		 * BEST LAP AND THE COUNTER DRAW IN BOTH PHASES; TIME
		 * and POS only once the grid has cleared. See
		 * hud_race_hud_visible.
		 */
		if (hud_race_hud_visible() && !hud_race_over()) {
			/*
			 * ORIGIN  Hud_DrawInRace @ 0x00413180 - the
			 *         callouts at 0x0041329e, then ONE OF TWO
			 *         POSITION BLOCKS at 0x00413312, by mode.
			 *         See hud_time_and_pos.
			 *
			 * THE CALLOUT GATE IS A MODE TEST: a network game,
			 * or practice with time attack on - so in solo the
			 * only thing it ever labels is the ghost.
			 *
			 * NONE OF THIS ONCE THE VIEWED RACER HAS FINISHED,
			 * where the original RETURNS after its best-lap
			 * block on raceState 2 or 3 - but it draws the
			 * callouts BEFORE that return, so retail keeps them
			 * for a finished viewer and this block loses them.
			 * [deliberate divergence - see hud_race_over]
			 *
			 * [open] THE NETWORK ARM IS NOT TRANSCRIBED.
			 * FUN_00482f00 draws a scoreboard instead of the
			 * best lap when g_nSoloMode is 0; solo takes the
			 * best-lap arm, which is what this port can reach.
			 */
			if (!g_soloMode ||
			    (g_selectedMode == 0 && g_timeAttack != 0)) {
				hud_draw_racer_callouts();
			}

			if (hud_time_and_pos()) {
				hud_draw_time();
				hud_draw_pos();
			} else {
				hud_draw_pos_only();
			}
			hud_draw_lap_list();
			hud_draw_go();
		} else if (!hud_race_over()) {
			hud_draw_countdown();
		}
		hud_draw_messages();
		hud_draw_best_lap();
		hud_draw_finish_place();
		if (s_open)
			hud_draw_items();
		fb_overlay_end();
	}

	fb_set_2d_origin(origin);
}

/*
 * THE MENU IS 2D AND IT MUST GO ON TOP OF THE RACE.
 *
 * In the original both layers land in one DirectDraw surface and
 * call order decides, so the in-race menus draw AFTER the scene.
 * Ours uploads the 2D layer as a texture drawn BEFORE the geometry -
 * right for a menu page, wrong here, where a palm tree in front of
 * the camera covers the words. So the caller re-composites this
 * rectangle after the geometry.
 *
 * It is the union of the four items plus a margin: the blink makes
 * one row come and go, and a rect that changed size with it would
 * flicker at the edges.
 */
int hud_menu_overlay_rect(int *x, int *y, int *w, int *h)
{
	int i;
	int x0, y0, x1, y1;
	int ix, iy, iw, ih;

	/*
	 * race_ready BECAUSE THAT IS WHAT GATES hud_draw, AND THE TWO
	 * MUST AGREE. This rect tells main.cpp to lift a box out of the
	 * OVERLAY buffer, which only hud_draw refills - so reporting
	 * one when the draw did not run promotes whatever was left in
	 * there, which is last frame's menu.
	 */
	if (!s_open || !s_fontOk || !race_ready())
		return 0;

	hud_item_rect(&s_titleFont, HUD_FONT_MENULINK, menu_title(),
	              HUD_TITLE_DY,
	              HUD_ALIGN_TITLE, &ix, &iy, &iw, &ih);
	x0 = ix; y0 = iy; x1 = ix + iw; y1 = iy + ih;

	/* row_count(), NOT the constant - see hud_draw_items. */
	for (i = 0; i < row_count(); i++) {
		const char *t = row_label(i);

		hud_item_rect(&s_rowFont, HUD_FONT_GAMEMENU, t,
		              i * HUD_ROW_DY,
		              HUD_ALIGN_ROW, &ix, &iy, &iw, &ih);
		if (ix < x0) x0 = ix;
		if (iy < y0) y0 = iy;
		if (ix + iw > x1) x1 = ix + iw;
		if (iy + ih > y1) y1 = iy + ih;
	}

	/*
	 * THE INFLATE IS THE ORIGINAL'S FIVE. InGameMenu_DrawAll unions
	 * the widgets' boxes into menu+0x68..+0x74, insets that by 5 on
	 * each side, and hands it to maybe_Screen3D_FadeRect - so the
	 * panel is the text's bounding box plus a five-pixel margin.
	 */
	x0 -= 5; y0 -= 5; x1 += 5; y1 += 5;
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > g_frameWidth)  x1 = g_frameWidth;
	if (y1 > g_frameHeight) y1 = g_frameHeight;
	if (x1 <= x0 || y1 <= y0)
		return 0;

	*x = x0;
	*y = y0;
	*w = x1 - x0;
	*h = y1 - y0;
	return 1;
}
