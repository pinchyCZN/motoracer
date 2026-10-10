/*
 * menu.c - see menu.h.
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "menu.h"
#include "sprite.h"
#include "menures.h"
#include "hud.h"
#include "font.h"
#include "fontdef.h"
#include "netplay.h"
#include "race.h"
#include "framebuffer.h"
#include "config.h"
#include "options.h"
#include "tracks.h"
#include "level.h"
#include "trk.h"
#include "grid.h"
#include "horizon.h"
#include "race.h"
#include "racer.h"
#include "racerdraw.h"
#include "bikes.h"
#include "skl.h"
#include "timing.h"
#include "model.h"
#include "mesh.h"
#include "prim.h"
#include "bank.h"
#include "queue.h"
#include "transform.h"
#include "render.h"
#include "drawscene.h"
#include "cull.h"
#include "glass.h"
#include "audio.h"
#include "scene.h"
#include "trig.h"
#include "log.h"

/* ---------------------------------------------------------------- */
/* Shared page helpers                                              */

static int s_page = MENU_PAGE_TITLE;

/* Where menu_begin_transition is headed, or -1. */
static int s_pendingPage = -1;

char g_playerName[MENU_NAME_MAX + 1];
int  g_cheatReverse;
int  g_cheatPocket;
int  g_cheatIsland;
int  g_reverseUnlocked;
int  g_reverseEnabled;
int  g_pocketUnlocked;
int  g_pocketEnabled;

static int s_netSavedValid;
static int s_netSavedPocket;
static int s_netSavedReverse;

/* ORIGIN  NONE - ours. Our .ini stands in for the save */
void menu_load_unlocks(void)
{
	g_reverseUnlocked = 0;
	g_reverseEnabled  = 0;
	g_pocketUnlocked  = 0;
	g_pocketEnabled   = 0;
	get_ini_value("PLAYER", "reverse_unlocked", &g_reverseUnlocked);
	get_ini_value("PLAYER", "reverse_enabled",  &g_reverseEnabled);
	get_ini_value("PLAYER", "pocket_unlocked",  &g_pocketUnlocked);
	get_ini_value("PLAYER", "pocket_enabled",   &g_pocketEnabled);
}

/* ORIGIN  NONE - ours. Our .ini stands in for the save */
void menu_save_unlocks(void)
{
	/*
	 * ENABLING KEEPS IT UNLOCKED, which is the original own rule and not
	 * a convenience: 0x00455224 sets the unlocked flag whenever the
	 * enabled one is on. Without it, switching reverse off and
	 * restarting would lose an option the player had earned.
	 */
	if (g_reverseEnabled) g_reverseUnlocked = 1;
	if (g_pocketEnabled)  g_pocketUnlocked  = 1;

	write_ini_value("PLAYER", "reverse_unlocked", g_reverseUnlocked);
	write_ini_value("PLAYER", "reverse_enabled",  g_reverseEnabled);
	write_ini_value("PLAYER", "pocket_unlocked",  g_pocketUnlocked);
	write_ini_value("PLAYER", "pocket_enabled",   g_pocketEnabled);
}

/*
 * The cursor, in design space. DAT_00519fe8 / DAT_00519fea.
 *
 * Seeded off screen so nothing reads as hovered before the first
 * WM_MOUSEMOVE arrives - the original leaves these wherever the last
 * page left them, but it also always has a cursor on screen.
 */
static int s_cursorX = -1;
static int s_cursorY = -1;

/*
 * ORIGIN  NONE - ours
 *
 * The cursor arrives in FRAME pixels and every button rectangle is in
 * AUTHORED ones, so the 2D origin comes back off here - the one place
 * that undoes what sprite_draw and font_draw_text put on.
 *
 * A pointer in the pillarbox beside a 16:9 menu maps to a negative x
 * and fails every hit test, which is what should happen.
 */
void menu_set_cursor(int x, int y)
{
	s_cursorX = x - g_fb2dOriginX;
	s_cursorY = y;
}

/*
 * ORIGIN  Menu_HitTest @ 0x0040c0c0, the rectangle test
 *
 * For a kind-2 (sprite) button, which reads the sprite's first four
 * ints - x, y, width, height - and is INCLUSIVE on all four edges
 * [verified]:
 *
 *     s->x <= mx && s->y <= my &&
 *     mx <= s->x + s->width && my <= s->y + s->height
 *
 * Note it is the sprite's PLACED rect, not a rectangle stored on the
 * button, so a sprite that moves drags its hit box with it. Kind-1
 * (text) buttons carry their own rect instead.
 *
 * NOT REPRODUCED: g_menuInputBlocked, which makes the whole test
 * return -1, and the hitTestDirty cache that lets a frame reuse the
 * previous answer. Neither changes the result for one button on one
 * page, and inventing state for them would be worse than leaving
 * them out and saying so.
 *
 * hitTestDirty IS reproduced for page 15, by a different means - see
 * p15_hit_test, where skipping the work matters because the search
 * would otherwise undo every keyboard selection.
 */
static int sprite_hit(const MR_Sprite *s, int mx, int my)
{
	if (!s)
		return 0;
	return s->x <= mx && s->y <= my &&
	       mx <= s->x + s->width && my <= s->y + s->height;
}

static int s_bmg;                       /* g_useBmgBranding         */

/* ORIGIN  NONE - ours */
void menu_set_bmg_branding(int on)
{
	s_bmg = on ? 1 : 0;
}

/* ORIGIN  NONE - ours */
int menu_current_page(void)
{
	return s_page;
}

/* ORIGIN  NONE - ours. See menu.h. */
int menu_page_modal(void)
{
	return s_page == MENU_PAGE_NAME;
}

/*
 * ORIGIN  NONE - ours
 *
 * Load one manifest resource and place it. Used by every page, which
 * is a good part of why they belong in one file.
 */
static int load_res(MR_Sprite *s, int id, int x, int y, int anchor,
                    const char *who)
{
	char path[MAX_PATH];
	int  pos[2];
	const MENURES_ENTRY *e;

	e = menures_get(id);
	if (!e)
		return 0;
	if (!menures_path(id, path, (int)sizeof(path))) {
		mrlog_err("%s: cannot build a path for resource %d\n", who, id);
		return 0;
	}
	if (!sprite_load(s, path, e->kind))
		return 0;

	pos[0] = x;
	pos[1] = y;
	sprite_set_position(s, pos, anchor);

	return 1;
}


/* ================================================================ */
/* The sprite button list                                           */
/* ================================================================ */

/*
 * Menu_AddSpriteButton / Menu_HitTest / Menu_Draw / Menu_GetActivated,
 * reduced to what the pages actually use. The original keeps these on
 * MR_Menu with eleven arguments per button - a colour key, a group id,
 * a text rect, an enable flag and more - and page 3 passes -1 or 0 for
 * most of them. What is left is a sprite, an optional LIT sprite, and
 * an optional key that activates it.
 *
 * KIND 2, SPRITE - TWO WAYS TO SHOW "NOT HOVERED", and which applies
 * is decided by whether the page supplied a second sprite [inferred,
 * from the two call shapes]:
 *
 *   a PAIR - SOLO_D and SOLO_OK, LINK_D and LINK_OK - draws the dark
 *   one normally and the lit one on hover. The _D and _OK suffixes are
 *   the artist saying exactly that.
 *
 *   a SINGLE sprite - options, exit, page 14 START - is drawn STIPPLED
 *   when not hovered and solid when it is.
 *
 *   SO A MISSING SECOND SPRITE IS NOT A MISSING FEATURE. Passing 0 for
 *   it is how a page asks for the stipple.
 *
 * KIND 1, TEXT, which page 11 is built almost entirely from.
 * Menu_AddTextButton (0x0040bd60) stores the string, a font and a
 * position, and FUN_00492a90 measures the text to get the hit rect,
 * (x, y, x + width - 1, y + height - 1).
 *
 *   SO A TEXT BUTTON HAS NO AUTHORED SIZE. It is exactly as big as
 *   what it says, which is why the original can change a value from
 *   "OFF" to "FILTERED" without touching any geometry.
 *
 *   ITS COLOUR IS THE HOVER STATE: yellow at 0x004f0de8 under the
 *   cursor, white at 0x004f0dd4 otherwise - two entries of the colour
 *   table hud.c maps [verified, read out of the image].
 */
#define MENU_MAX_BUTTONS 32     /* MAX_BUTTON, asserted at 32 in menu.cpp */

#define MENU_BTN_TEXT   1
#define MENU_BTN_SPRITE 2

typedef struct {
	int         kind;     /* MENU_BTN_*, MR_MenuButton +0x14     */
	MR_Sprite  *base;     /* kind 2: drawn when not hovered      */
	MR_Sprite  *lit;      /* kind 2: drawn when hovered, or NULL */
	int         key;      /* a VK that also activates it, or 0   */
	const char *text;     /* kind 1                              */
	MR_Font    *font;     /* kind 1                              */
	int         x, y;     /* kind 1 position                     */
	int         w, h;     /* kind 1 measured extent              */
	int         repeat;   /* kind 1: repeatOnHold, +0x54         */
} MR_MenuButton;

/* The two text colours, and the shadow they are drawn on. */
static const unsigned char kMenuTextWhite[3]  = { 0xff, 0xff, 0xff };
static const unsigned char kMenuTextYellow[3] = { 0xff, 0xff, 0x00 };
static const unsigned char kMenuTextBlack[3]  = { 0x00, 0x00, 0x00 };

static MR_MenuButton s_buttons[MENU_MAX_BUTTONS];
static int           s_buttonCount;
static int           s_hoverIndex   = -1;
static int           s_activated    = -1;
static int           s_mouseDown;

/*
 * The held-button repeat. MENU_REPEAT_DELAY is how long a press has to
 * be held before it starts repeating, and MENU_REPEAT_RATE the gap
 * between steps after that - both in real seconds, and both ours. See
 * the note in menu_hit_test for why there is nothing to transcribe.
 *
 * 0.09s is about eleven steps a second, so a volume crosses its whole
 * 0..32 range in three seconds of holding.
 */
#define MENU_REPEAT_DELAY 0.40
#define MENU_REPEAT_RATE  0.09

static int           s_repeatButton = -1;
static double        s_repeatDeadline;

/*
 * ORIGIN  Menu_ClearButtons @ 0x0040bd40
 *
 * THREE WRITES, AND activatedIndex IS NOT ONE OF THEM. It sets
 * buttonCount at +0x40, needsRedraw at +0x30 and g_menuInputBlocked,
 * and leaves activatedIndex at +0x18 alone [verified].
 *
 * o CLEARING BUTTONS ALSO UNBLOCKS MENU INPUT, which is the only
 *   writer of that flag this port has found - see the note over
 *   sprite_hit, where it is listed as not reproduced.
 *
 * o THAT MATTERS BECAUSE PAGE 11 CLEARS EVERY FRAME. A click lands
 *   in the window procedure between two ticks; if clearing also
 *   dropped the activation, the very next tick would throw it away
 *   and the page would be dead to the mouse. Clearing it here is
 *   harmless ONLY while every page builds its buttons once in
 *   Enter, which page 11 does not.
 *
 * o THE PAGE-CHANGE RESET LIVES IN menu_set_page instead, which is
 *   the one funnel every page change goes through.
 */
void menu_clear_buttons(void)
{
	s_buttonCount = 0;
}

/* ORIGIN  Menu_AddSpriteButton @ 0x0040bef0 */
int menu_add_sprite_button(MR_Sprite *base, MR_Sprite *lit, int key)
{
	MR_MenuButton *b;

	if (s_buttonCount >= MENU_MAX_BUTTONS || !base)
		return -1;
	b = &s_buttons[s_buttonCount];
	memset(b, 0, sizeof *b);
	b->kind = MENU_BTN_SPRITE;
	b->base = base;
	b->lit  = lit;
	b->key  = key;
	return s_buttonCount++;
}

/* ORIGIN  Menu_AddTextButton @ 0x0040bd60 */
int menu_add_text_button(const char *text, MR_Font *font, int x, int y,
                         int key, int repeatOnHold)
{
	MR_MenuButton *b;

	if (s_buttonCount >= MENU_MAX_BUTTONS || !text || !font)
		return -1;
	b = &s_buttons[s_buttonCount];
	memset(b, 0, sizeof *b);
	b->kind   = MENU_BTN_TEXT;
	b->key    = key;
	b->text   = text;
	b->font   = font;
	b->x      = x;
	b->y      = y;
	b->w      = font_text_width(font, text);
	b->h      = font_height(font);
	b->repeat = repeatOnHold;
	return s_buttonCount++;
}



/* ================================================================== */
/* MODAL TEXT CAPTURE - see menu.h.                                   */
/* ================================================================== */

#define MENU_CAPTURE_BLINK 450        /* ms per half cycle */

static char s_cap_text[MENU_CAPTURE_MAX + 1];
static char s_cap_allowed[96];
static int  s_cap_active;
static int  s_cap_len;
static int  s_cap_max;
static int  s_cap_done;
static int  s_cap_cancelled;
static int  s_cap_hasFilter;

/* ORIGIN  NONE - ours */
void menu_capture_begin(const char *allowed, int maxLen)
{
	int i;

	s_cap_text[0]   = 0;
	s_cap_len       = 0;
	s_cap_done      = 0;
	s_cap_cancelled = 0;
	s_cap_active    = 1;
	s_cap_max       = (maxLen > 0 && maxLen <= MENU_CAPTURE_MAX)
	                  ? maxLen : MENU_CAPTURE_MAX;

	s_cap_hasFilter = (allowed != NULL && *allowed != 0);
	s_cap_allowed[0] = 0;
	if (s_cap_hasFilter) {
		for (i = 0; i < (int)sizeof(s_cap_allowed) - 1 && allowed[i]; i++)
			s_cap_allowed[i] = allowed[i];
		s_cap_allowed[i] = 0;
	}
}

/* ORIGIN  NONE - ours */
void menu_capture_end(void)
{
	s_cap_active = 0;
}

/* ORIGIN  NONE - ours */
int menu_capture_active(void) { return s_cap_active; }

/* ORIGIN  NONE - ours */
const char *menu_capture_text(void) { return s_cap_text; }

/* ORIGIN  NONE - ours */
int menu_capture_len(void) { return s_cap_len; }

/* ORIGIN  NONE - ours */
int menu_capture_done(void) { return s_cap_done; }

/* ORIGIN  NONE - ours */
int menu_capture_cancelled(void) { return s_cap_cancelled; }

/* ORIGIN  NONE - ours */
void menu_capture_clear_result(void)
{
	s_cap_done      = 0;
	s_cap_cancelled = 0;
}

/* ORIGIN  NONE - ours */
int menu_capture_cursor_on(void)
{
	return ((GetTickCount() / MENU_CAPTURE_BLINK) & 1) == 0;
}

/*
 * ORIGIN  NONE - ours
 *
 * CONSUMES EVERY KEY, not just the ones it uses. Returning 0 for an
 * unhandled key would let it fall through to the debug block below,
 * which is the whole fault this exists to fix.
 */
int menu_capture_on_key(int vkey)
{
	if (!s_cap_active)
		return 0;

	switch (vkey) {
	case VK_ESCAPE:
		/* BACKS OUT OF THE FIELD, and does not close the game. */
		s_cap_cancelled = 1;
		s_cap_active    = 0;
		break;
	case VK_RETURN:
		s_cap_done   = 1;
		s_cap_active = 0;
		break;
	case VK_BACK:
		if (s_cap_len > 0)
			s_cap_text[--s_cap_len] = 0;
		break;
	default:
		break;
	}
	return 1;
}

/* ORIGIN  NONE - ours */
int menu_capture_on_char(int ch)
{
	const char *a;

	if (!s_cap_active)
		return 0;

	/* The control keys arrive through on_key; ignore them here. */
	if (ch < 0x20 || ch > 0x7e)
		return 1;
	if (s_cap_len >= s_cap_max)
		return 1;

	if (s_cap_hasFilter) {
		int up = (ch >= 'a' && ch <= 'z') ? ch - ('a' - 'A') : ch;

		for (a = s_cap_allowed; *a; a++) {
			if (*a == ch || *a == up) {
				s_cap_text[s_cap_len++] = (char)*a;
				s_cap_text[s_cap_len]   = 0;
				return 1;
			}
		}
		return 1;           /* outside the set: dropped, still consumed */
	}

	s_cap_text[s_cap_len++] = (char)ch;
	s_cap_text[s_cap_len]   = 0;
	return 1;
}

/* ORIGIN  NONE - ours. See menu.h. */
void menu_capture_paste(const char *text)
{
	const char *p = text;

	if (!s_cap_active || !p)
		return;

	s_cap_len     = 0;
	s_cap_text[0] = 0;

	while (*p == ' ' || *p == '\t')
		p++;
	for (; *p && *p != '\r' && *p != '\n'; p++)
		(void)menu_capture_on_char((unsigned char)*p);
}


/* ORIGIN  NONE - ours */
void menu_set_mouse_down(int down)
{
	s_mouseDown = down ? 1 : 0;
}

/*
 * ORIGIN  Menu_UpdateHover @ 0x004099a0, with Menu_HitTest @ 0x0040c0c0
 *
 * A RECTANGLE TEST, unlike page 15 - these buttons are 2D sprites at
 * known positions, so the rectangle is exactly right and the
 * proximity search page 15 needs would be wrong here.
 *
 * The hover sound fires on the ENTERING edge only, which is
 * Menu_UpdateHover's own rule: a change to an index that is not -1.
 *
 * o THE ORIGINAL'S TEST HAS A SIDE EFFECT AND OURS DOES NOT. Before
 *   testing a kind-2 button it calls Sprite_SetPosition to MOVE the
 *   sprite to the button's own spriteX/spriteY (+0x34, +0x38),
 *   whenever neither is -1. So a button may carry a position that
 *   overrides wherever the sprite was placed.
 *
 *   OURS HAS NO SUCH FIELD and always tests the sprite where it
 *   sits. That agrees with the original only where the page passes
 *   -1 for the pair - which every Menu_AddSpriteButton call read so
 *   far does, pages 6 and 10 included [verified]. A page that
 *   passed real coordinates would hit-test somewhere we do not.
 */
void menu_hit_test(void)
{
	int i;
	int hit = -1;

	for (i = 0; i < s_buttonCount; i++) {
		const MR_MenuButton *b = &s_buttons[i];
		int                  on;

		if (b->kind == MENU_BTN_TEXT) {
			on = b->w > 0 && b->h > 0 &&
			     s_cursorX >= b->x && s_cursorX <= b->x + b->w - 1 &&
			     s_cursorY >= b->y && s_cursorY <= b->y + b->h - 1;
		} else {
			on = b->base && sprite_hit(b->base, s_cursorX, s_cursorY);
		}
		if (on) {
			hit = i;
			break;
		}
	}
	if (hit != s_hoverIndex) {
		s_hoverIndex = hit;
		if (hit >= 0)
			audio_cue(SND_MENU_HOVER, 0);
	}

	/*
	 * REPEAT ON HOLD, AND THE DELAY IS OURS.
	 *
	 * The original has NO rate limit at all: Menu_Tick re-activates
	 * the hovered button on every frame the mouse bit is set in both
	 * this frame and the last [verified, 0x00409c00]. Two globals in
	 * that block look like a repeat timer - 0x00504878 takes
	 * timeGetTime and 0x00504e7c is zeroed beside it - and BOTH ARE
	 * WRITE-ONLY across all 239,438 instructions, so whatever was
	 * meant to read them never shipped.
	 *
	 * o SO THE ORIGINAL RATE IS ITS FRAME RATE, and that is why this
	 *   cannot be transcribed. A volume runs 0..32 in steps of one;
	 *   at our frame rate a single click lasts five or six frames and
	 *   jumps a fifth of the range, which leaves about six reachable
	 *   settings out of thirty-three.
	 *
	 * o A DELAY THEN A FIXED RATE is the fix, and it is a divergence
	 *   stated as one. A click is exactly one step; holding starts
	 *   stepping after MENU_REPEAT_DELAY and then runs at a rate in
	 *   SECONDS rather than in frames, so it is the same on any
	 *   machine - which the original's is not.
	 */
	if (!s_mouseDown || hit < 0 || !s_buttons[hit].repeat) {
		s_repeatButton = -1;
	} else if (hit != s_repeatButton) {
		s_repeatButton   = hit;
		s_repeatDeadline = tm_real_now() + MENU_REPEAT_DELAY;
	} else if (tm_real_now() >= s_repeatDeadline) {
		s_activated      = hit;
		s_repeatDeadline = tm_real_now() + MENU_REPEAT_RATE;
	}
}

/* ORIGIN  Menu_Draw @ 0x0040c1f0, the button layer only */
void menu_draw_buttons(void)
{
	int i;

	for (i = 0; i < s_buttonCount; i++) {
		MR_Sprite *b   = s_buttons[i].base;
		MR_Sprite *lit = s_buttons[i].lit;
		int        on  = (i == s_hoverIndex);

		if (s_buttons[i].kind == MENU_BTN_TEXT) {
			font_set_colors(s_buttons[i].font,
			                on ? kMenuTextYellow : kMenuTextWhite,
			                kMenuTextBlack);
			font_draw_text(s_buttons[i].font, s_buttons[i].x,
			               s_buttons[i].y, s_buttons[i].text, 0);
			continue;
		}
		if (!b)
			continue;
		sprite_update_move(b);
		if (lit) {
			/*
			 * THE LIT SPRITE IS MOVED ONTO THE DARK ONE, every time,
			 * and that is not a convenience - it is what Menu_Draw
			 * does:
			 *
			 *     pos = Sprite_GetPosition(spriteA);
			 *     Sprite_SetPosition(spriteB, pos, 0);
			 *     Sprite_Draw(spriteB, ...);
			 *
			 * in BOTH draw arms - 0x0040c422 before the mode-2 draw
			 * and 0x0040c43d before the mode-5 one [verified].
			 *
			 * SO A PAGE NEVER POSITIONS ITS HIGHLIGHT SPRITE, and must
			 * not try. Page 10 places autofla at y 564 and manufla at
			 * y 615 and never moves them again.
			 *
			 * o A LIT SPRITE LEFT WHERE THE PAGE PUT IT DRAWS THERE.
			 *   Page 10's pair sits eighty pixels apart, so a hover
			 *   would draw off the bottom of the screen while the dark
			 *   sprite is suppressed - the row reads as going dark
			 *   rather than lighting up.
			 *
			 * o AND A PAGE NEED NOT TWEEN BOTH HALVES OF A PAIR to
			 *   keep them aligned. The draw aligns them every frame,
			 *   so moving only the dark one is enough.
			 */
			int pos[2];

			pos[0] = b->x;
			pos[1] = b->y;
			sprite_set_position(lit, pos, SPRITE_ANCHOR_NONE);
			sprite_draw(on ? lit : b, SPRITE_MODE_USE_KIND, 100, 0);
		} else {
			sprite_draw(b, on ? SPRITE_MODE_USE_KIND
			                  : SPRITE_MODE_STIPPLE_KEYED, 100, 0);
		}
	}
}

/* ORIGIN  NONE - ours. The original reads menu->hoverIndex direct */
int  menu_hover_index(void) { return s_hoverIndex; }

/* ORIGIN  NONE - ours */
void menu_set_hover(int index)
{
	s_hoverIndex = (index >= 0 && index < s_buttonCount) ? index : -1;
}
/* ORIGIN  Menu_GetActivated @ 0x0040c1b0 */
int  menu_get_activated(void) { return s_activated; }

/* ORIGIN  Menu_ClearActivated @ 0x0040c1e0 */
void menu_clear_activated(void) { s_activated = -1; }

/*
 * ORIGIN  NONE - ours
 *
 * A click activates whatever is hovered; a key activates the button
 * that claimed it. Both go through the same slot so the page reads
 * one value and does not care which happened.
 */
static int menu_buttons_click(void)
{
	if (s_hoverIndex < 0)
		return 0;
	s_activated = s_hoverIndex;
	return 1;
}

/* ORIGIN  NONE - ours */
static int menu_buttons_key(int vkey)
{
	int i;

	for (i = 0; i < s_buttonCount; i++) {
		if (s_buttons[i].key && s_buttons[i].key == vkey) {
			s_activated = i;
			return 1;
		}
	}
	return 0;
}


/* ================================================================ */
/* Page 1 - the loading screen                                      */
/* ================================================================ */

/*
 * The original's tick does everything in one pass: build the paths,
 * construct the sprites, draw them, then load the level and switch
 * page. Since it never comes back, construction and drawing are not
 * separated there. They are here, because we do come back.
 */

#define RES_LODANIM   175
#define RES_LOADTXT   176
#define RES_SMALLREV  185
#define RES_SMALLPOK  186

/* Absolute, against the 640x480 design resolution. */
#define POS_LODANIM_X   498     /* 0x1f2 */
#define POS_LODANIM_Y   381     /* 0x17d */
#define POS_LOADTXT_X     0
#define POS_LOADTXT_Y   408     /* 0x198 */
#define POS_SMALLPOK_X    4
#define POS_SMALLPOK_Y  460     /* 0x1cc */
#define POS_SMALLREV_X    4
#define POS_SMALLREV_Y  470     /* 0x1d6 */

static MR_Sprite s_p1_background;
static MR_Sprite s_p1_lodanim;
static MR_Sprite s_p1_loadtxt;
static MR_Sprite s_p1_smallrev;
static MR_Sprite s_p1_smallpok;

static int s_p1_background_ok;
static int s_p1_lodanim_ok;
static int s_p1_loadtxt_ok;
static int s_p1_smallrev_ok;
static int s_p1_smallpok_ok;
static int s_p1_ok;

/*
 * [open] THE BADGES ARE UNCONDITIONAL HERE AND SHOULD NOT BE. The
 * original gates each on whether the player has unlocked reverse
 * tracks or the pocket bike, and the port HAS those flags -
 * menu_load_unlocks reads g_reverseUnlocked and g_pocketUnlocked out
 * of [PLAYER]. Page 3 already gates its own indicators on them. This
 * page was written before the ini existed and still shows both.
 */
static int s_p1_show_badges = 1;

/* Set once the level-load handover has fired, so it fires once. */
static int s_p1_handedOver;


static int s_p1_viewLogged;


/* ORIGIN  NONE - ours */
static void page01_enter(void)
{
	char rel[MAX_PATH];
	int  pos[2];

	/*
	 * Background: "%sload.lez" against THE SELECTED TRACK'S path, so
	 * "ILE01\load.lez" resolves as member LOAD.LEZ of ILE01.BKF.
	 * assets.c matches case-insensitively.
	 *
	 * o IT IS THE SELECTED TRACK'S, NOT A FIXED DIRECTORY. A single
	 *   directory for the whole run would show ile01's loading art
	 *   on every island. The original reads g_pTrackList +
	 *   g_nSelectedTrack * 0x130 + 0x24, the path string out of
	 *   the track's own LIST.BPT record
	 *   [verified, the sprintf at 0x0040b41e]. Each island ships its
	 *   own load.lez and that is the whole point of the format
	 *   string.
	 *
	 * o THERE IS NO FALLBACK, and that is deliberate. A [PATHS]
	 *   stand-in for a failed track list means a broken LIST.BPT
	 *   shows ile01's art on every island instead of saying so.
	 *   Failing loudly is worth more than a wrong background.
	 */
	{
		const MR_Track *t = track_at(g_selectedTrack);

		if (t && track_asset_name(t, "load.lez", rel, (int)sizeof(rel))) {
			s_p1_background_ok = sprite_load(&s_p1_background, rel, 0);
		}
		if (s_p1_background_ok) {
			pos[0] = 0;
			pos[1] = 0;
			sprite_set_position(&s_p1_background, pos, SPRITE_ANCHOR_NONE);
			mrlog("page1: background %s %dx%d (track %d, %s)\n", rel,
			       s_p1_background.width, s_p1_background.height,
			       g_selectedTrack, t ? t->name : "(no record)");
		}
	}
	if (!s_p1_background_ok) {
		mrlog("page1: no track background; giving up\n");
		return;
	}

	s_p1_lodanim_ok  = load_res(&s_p1_lodanim,  RES_LODANIM,
	                            POS_LODANIM_X,  POS_LODANIM_Y,
	                            SPRITE_ANCHOR_NONE, "page1");
	s_p1_loadtxt_ok  = load_res(&s_p1_loadtxt,  RES_LOADTXT,
	                            POS_LOADTXT_X,  POS_LOADTXT_Y,
	                            SPRITE_ANCHOR_NONE, "page1");
	s_p1_smallrev_ok = load_res(&s_p1_smallrev, RES_SMALLREV,
	                            POS_SMALLREV_X, POS_SMALLREV_Y,
	                            SPRITE_ANCHOR_NONE, "page1");
	s_p1_smallpok_ok = load_res(&s_p1_smallpok, RES_SMALLPOK,
	                            POS_SMALLPOK_X, POS_SMALLPOK_Y,
	                            SPRITE_ANCHOR_NONE, "page1");
	/*
	 * AND THE RACE, which race.c owns - the manifest, the track,
	 * its banks, the grid and the racers. A menu source file is
	 * the wrong home for a running race, and the hand-over is what
	 * the real game does: page 1 IS the loading screen.
	 */
	race_load(g_selectedTrack);

	s_p1_viewLogged = 0;

	mrlog("page1: lodanim %s (not drawn until the loader steps - "
	      "see page01_tick)\n", s_p1_lodanim_ok ? "loaded" : "MISSING");
	s_p1_handedOver = 0;
	s_p1_ok = 1;
}

/* ORIGIN  MenuPage01_Loading_Tick @ 0x0040b3f0 */
static void page01_tick(void)
{
	if (!s_p1_background_ok)
		return;

	/*
	 * Draw order and mode are the original's: every sprite goes out
	 * with mode 5, the sentinel meaning "use the sprite's own kind",
	 * fully revealed and unshrunk.
	 *
	 * A LOADED RACE OWNS THE WHOLE FRAME. The 2D layer goes down
	 * BEFORE the 3D bracket, so the loading art would sit behind
	 * the track and show through wherever no geometry covers it -
	 * which is most of the sky. Clear instead and draw none of it.
	 */
	if (race_ready()) {
		/* Black first, so a race that failed to load leaves an
		 * empty frame rather than last frame's. */
		fb_clear(0);

		/* THE FRAME IS THE RACE'S FROM HERE. The camera steps
		 * exactly once, inside race_update; the sky then reads
		 * it. Step it in both places and every lerp runs at
		 * double rate - camera.txt 4.8. */
		race_update();
		race_draw_2d();
		/*
		 * AND THE OVERLAY ON TOP. The escape menu is 2D and
		 * goes into the same framebuffer the race just drew
		 * into, which is the original order too - the in-race
		 * menus draw after the frame, not with the pages.
		 */
		hud_draw();
		return;
	}

	sprite_draw(&s_p1_background, SPRITE_MODE_USE_KIND, 100, 0);

	if (s_p1_loadtxt_ok)
		sprite_draw(&s_p1_loadtxt, SPRITE_MODE_USE_KIND, 100, 0);
	if (s_p1_show_badges && s_p1_smallrev_ok)
		sprite_draw(&s_p1_smallrev, SPRITE_MODE_USE_KIND, 100, 0);
	if (s_p1_show_badges && s_p1_smallpok_ok)
		sprite_draw(&s_p1_smallpok, SPRITE_MODE_USE_KIND, 100, 0);

	/*
	 * LODANIM IS DELIBERATELY NOT DRAWN, and that is the original's
	 * behaviour rather than a gap.
	 *
	 * Page 1's own tick loads it and places it at 498,381 and never
	 * draws it. What draws it is Menu_DrawLoadingScreen
	 * (0x004470d0), which the LOADER calls once per step through
	 * maybe_Loader_ReportStep (0x004421d0) - the function that also
	 * carries the step caption, "Init WAV samples ..." and friends.
	 * That function repaints this whole screen and then does
	 *
	 *     if (lodanim && g_nLoadAnimPhase && ...) draw lodanim;
	 *     ...
	 *     g_nLoadAnimPhase ^= 1;
	 *
	 * so LODANIM BLINKS, once per loading step [verified]. It is not
	 * a progress bar and it does not fill in. g_nLoadAnimPhase
	 * (0x004e90fc) initialises to 1 and is touched nowhere else in
	 * the image.
	 *
	 * o WHICH EXPLAINS THE GREY LOGO. The MotoRacer logo bottom
	 *   right is painted into the track's own load.lez in grey;
	 *   lodanim is the COLOURED one drawn over it on alternate
	 *   steps. So it alternates grey/colour while the level streams
	 *   and stops on whichever phase the last step left. Retail
	 *   reads as "grey, then colour" because the blink starts the
	 *   moment loading does.
	 *
	 * o SO ZERO LOADING STEPS MEANS GREY, which is what we have.
	 *   Drawing it here would show the COLOURED logo on a screen
	 *   that has loaded nothing. It is loaded and logged instead.
	 */

	/*
	 * THE FAILURE EXIT. Our level load runs in Enter - race_load -
	 * and a race that came up holds the page in the branch above, so
	 * only a FAILED load reaches here.
	 *
	 * THE ORIGINAL TAKES THE SAME EXIT. When FUN_00427e80 fails to
	 * parse "%sLevel.Spt", or Game_Load returns 0, page 1 calls
	 * Menu_SetPage(g_menu, 3) - back to the main menu.
	 *
	 * o AND IT IS Menu_SetPage, NOT A TRANSITION, SO IT DOES NOT
	 *   SHATTER. Both of page 1's exits are plain SetPage calls -
	 *   page 0 on success at 0x0040b7f0, page 3 on failure at
	 *   0x0040bb40, each with the page number pushed immediately
	 *   before [verified] - and neither arms the glass. That is why
	 *   retail never shatters the loading screen on the way into a
	 *   race. menu_begin_transition here would be wrong twice over:
	 *   the wrong exit AND an effect the original does not play.
	 */
	if (race_ready())
		return;

	if (!s_p1_handedOver) {
		s_p1_handedOver = 1;
		mrlog("page1: track %d, level load not built -> page %d\n",
		      g_selectedTrack, MENU_PAGE_MAIN);
		menu_goto_page(MENU_PAGE_MAIN);
	}
}


/* ORIGIN  NONE - ours */
static void page01_leave(void)
{
	race_free();
	if (s_p1_background_ok) sprite_free(&s_p1_background);
	if (s_p1_lodanim_ok)    sprite_free(&s_p1_lodanim);
	if (s_p1_loadtxt_ok)    sprite_free(&s_p1_loadtxt);
	if (s_p1_smallrev_ok)   sprite_free(&s_p1_smallrev);
	if (s_p1_smallpok_ok)   sprite_free(&s_p1_smallpok);

	s_p1_background_ok = 0;
	s_p1_lodanim_ok    = 0;
	s_p1_loadtxt_ok    = 0;
	s_p1_smallrev_ok   = 0;
	s_p1_smallpok_ok   = 0;
	s_p1_ok            = 0;
	s_p1_handedOver    = 0;
}


/* ================================================================ */
/* Page 14 - the title screen                                       */
/* ================================================================ */

/*
 * From MenuPage14_Title_Enter (0x004465f0) and _Tick (0x00446b80).
 *
 * TWO WHOLE SETS OF ART, chosen by g_useBmgBranding: title/title2/
 * title3/title4 for MotoRacer, and the GP variants for the Grand Prix
 * release. Both sets are in the manifest at ids 188..195 [verified].
 *
 * The four are LAYERS, not alternatives. The enter handler draws all
 * four in one frame, back to front, and the tick then draws only the
 * one the reveal has reached - DAT_005409bc counts 0, 1, 2, then 3,
 * and at 3 the whole title is up and the blinking strapline starts.
 *
 * THE THREE 3D MESHES. Enter loads ellipse, moto and racer
 * .3df/.map pairs from data\pages\titleMr\ (or titleGP), scales each
 * with branding-specific factors, and animates them along per-object
 * speed curves. The original keeps three model instances at
 * 0x00540500, 0x00540684 and 0x00540808 - stride 0x184, walked as
 * 0x61 floats.
 *
 * o OURS USES MR_ModelInstance, A DIFFERENT AND SIMPLER RECORD.
 *   That is the divergence CLAUDE.md 5b sanctions by name, not a
 *   gap: the 0x184 layout was never needed, so it was never
 *   worked out. Do not go looking for it to "finish" this page.
 */

/*
 * THE SPRITE POINTER TABLE IS g_menuObjects @ 0x00504880, 41 xrefs.
 * Menu_LoadResources fills it with MR_Sprite*, INDEXED BY RESOURCE
 * ID, and every page reads its sprites out of it [verified]. So a
 * decompile showing g_menuObjects is showing resource 0, the
 * backdrop - it is the base, not a separate object. The base is loaded as a
 * register in Menu_LoadResources, Menu_Tick, Menu_GotoPage,
 * Menu_UpdateHover and a dozen page handlers.
 *
 * That is what makes the DAT_00504xxx globals in any page handler
 * readable: subtract the base and divide by four to get the manifest
 * id. Page 14's own slots check out three ways -
 *
 *     0x00504b70 -> 188  title.lez       the four layers, and the
 *     0x00504b7c -> 191  title4.lez      enter handler draws them
 *                                        back to front
 *     0x005049b0 ->  76  start.lez       registered as a VK_RETURN
 *                                        button at y = 430
 *     0x005049d4 ->  85  Pointeur.lez    drawn only when fullscreen
 *                                        and on an enumerated device
 *
 * - and the last two are exactly what their guards in the handler say
 * they should be.
 */
#define MENU_SPRITE_TABLE 0x00504880

/* Manifest ids, from menures.c [verified]. */
#define RES_START      76       /* start.lez, localised */
#define RES_POINTER    85       /* Pointeur.lez         */
#define RES_TITLE_MR   188      /* title.lez    */
#define RES_TITLE2_MR  189
#define RES_TITLE3_MR  190
#define RES_TITLE4_MR  191
#define RES_TITLE_GP   192      /* titleGP.lez  */
#define RES_TITLE2_GP  193
#define RES_TITLE3_GP  194
#define RES_TITLE4_GP  195

/*
 * Menu_Tick special-cases this page: after 30000 ms with no input it
 * calls maybe_Menu_StartAttractDemo, which goes to page 1 [verified].
 */
#define TITLE_IDLE_MS  30000.0

/*
 * The strapline blink. The original counts frames, not time: a
 * counter increments every tick, the text draws while it is above 10,
 * and it resets past 20. So the duty cycle is half and the period is
 * twenty FRAMES - which at the original's 30 fps cap is about 0.67 s,
 * and at 144 would be a flicker.
 *
 * o REPRODUCED AS A TIME, NOT A FRAME COUNT, and that is a deliberate
 *   divergence. A frame-counted blink is exactly the kind of thing
 *   docs/subsystems/frame-loop.txt section 4 warns about: it means
 *   something different at every display rate. 20 frames at the rate
 *   the code was written for is 0.667 s, so that is what this uses.
 */
#define TITLE_BLINK_PERIOD 0.6667

/* Font 0x1f = 31, the id the original asks Font_Get for [verified]. */
#define TITLE_FONT_ID  31

/*
 * THE STRAPLINE IS YELLOW, CENTRED, AT y = 60 [verified].
 *
 * The colour was easy: the tick calls Font_Get(0x1f) then that font's
 * slot 0 - Font_SetColors - with 0x004f0de8 = (255,255,0) and
 * 0x004f0dd8 = black.
 *
 * THE POSITION NEEDS TWO MORE FUNCTIONS READ. The draw call passes
 * 0x3c and 0x22 - 60 and 34 - and taking them for x and y is the
 * obvious mistake, twice over:
 *
 *   1  The draw function is __thiscall and passes ITS OWN arg4 and
 *      arg5 BY ADDRESS to FUN_00417900, then reads them back as the
 *      final x and y. So they are in/out, not values.
 *
 *   2  FUN_00417900 is a text ALIGNMENT function, and the third value
 *      - 0x22 - is a FLAGS word, not a coordinate:
 *
 *          if ((flags & 6) && !(flags & 0x18) && !(flags & 1))
 *              x += (g_mainWidth - textWidth) / 2;
 *
 *      0x22 has bit 1 set and bits 3,4 clear, so it takes exactly that
 *      branch. x is an OFFSET FROM CENTRE and it is passed as 0.
 *
 * So: x centred, y = 60. Not (60, 34).
 *
 * o IN A CALL THIS WIDE, TWO SMALL INTEGERS ARE NOT A COORDINATE
 *   PAIR UNTIL THE CALLEE SAYS SO. 26 arguments with unknown roles
 *   make the reading look obvious, and here one is an in/out address
 *   and the other a flags word.
 */
#define TITLE_TEXT_Y   60

/*
 * Enter calls Sprite_SetPosition(sprite, {0, 0x1ae}, 1) - and THE
 * THIRD ARGUMENT IS 1, which is SPRITE_ANCHOR_CENTRE_X [verified].
 * So the x of 0 is not a left edge: the sprite is centred
 * horizontally and 430 is the only real coordinate.
 *
 * o ANCHOR 0 PUTS THE BUTTON BOTTOM LEFT, and that is the shape of
 *   mistake to watch for: the anchor was in the original's call all
 *   along, and a shared helper with no parameter for it DROPS AN
 *   ARGUMENT THE ORIGINAL CONSIDERED IMPORTANT, silently.
 */
#define POS_START_X   0
#define POS_START_Y 430     /* 0x1ae */

/*
 * ------------------------------------------------------------------
 * The three animated models.
 * ------------------------------------------------------------------
 *
 * The loading and the stride are in the page header above. What is
 * here is where the curves come from: FUN_00446460, called at the end
 * of enter, fills the start, end and duration tables, which are ZERO
 * in the image because they are set at run time [verified].
 *
 * THE TABLES, decoded from that function:
 *
 *   MotoRacer branding
 *     ellipse  (60, 42, -40000) -> (60, 42, -800)      1.0 s
 *     moto     (-12000, 44, -798) -> (42, 44, -798)    1.2 s
 *     racer    (12000, 25, -798) -> (40, 25, -798)     1.4 s
 *
 *   Grand Prix branding
 *     ellipse  (69, 26, -40000) -> (69, 26, -800)
 *     moto     (-12000, -90, -798) -> (252, -90, -798)
 *     racer    (12000, 40, -798) -> (-265, 40, -798)
 *
 * o THE ELLIPSE ZOOMS z FROM -40000 TO -800 - forty thousand units
 *   toward the camera in one second, which is the "oval rushes at you"
 *   the retail recording shows.
 *
 * o THE TABLE READS BACKWARDS AGAINST FOOTAGE, and it is not wrong.
 *   THE VIEW MATRIX IS A 180 DEGREE YAW, so it NEGATES x: moto's
 *   world x of -12000 is a CAMERA x of +12000, the right of the
 *   screen.
 *
 *       moto    camX  +12000 -> -42
 *       racer   camX  -12000 -> -40
 *
 *   So moto enters from the right and racer from the left, which is
 *   what the recording shows [observed]. DO NOT COMPARE A WORLD
 *   COORDINATE AGAINST A SCREEN DIRECTION - "the code says left" is
 *   a claim about world x, not about the screen.
 */
#define P14_MODELS 3

/* Enter loads the page's raw texture into bank 3. */
#define P14_BANK   3

static const char *kP14MeshMr[P14_MODELS][2] = {
	{ "PAGES\\TITLEMR\\ELLIPSE.3DF", "PAGES\\TITLEMR\\ELLIPSE.MAP" },
	{ "PAGES\\TITLEMR\\MOTO.3DF",    "PAGES\\TITLEMR\\MOTO.MAP"    },
	{ "PAGES\\TITLEMR\\RACER.3DF",   "PAGES\\TITLEMR\\RACER.MAP"   }
};
static const char *kP14MeshGp[P14_MODELS][2] = {
	{ "PAGES\\TITLEGP\\ELLIPSE.3DF", "PAGES\\TITLEGP\\ELLIPSE.MAP" },
	{ "PAGES\\TITLEGP\\MOTO.3DF",    "PAGES\\TITLEGP\\MOTO.MAP"    },
	{ "PAGES\\TITLEGP\\RACER.3DF",   "PAGES\\TITLEGP\\RACER.MAP"   }
};

/* Mesh_Scale factors, per branding, from the enter handler. */
static const float kP14ScaleMr[P14_MODELS][3] = {
	{ 0.04f, 0.03875f, 0.04f    },
	{ 0.04f, 0.036f,   0.0375f  },
	{ 0.04f, 0.033f,   0.0395f  }
};
static const float kP14ScaleGp[P14_MODELS][3] = {
	{ 0.04f, 0.0325f,  0.0455f  },
	{ 0.04f, 0.053f,   0.04825f },
	{ 0.04f, 0.0315f,  0.0315f  }
};

static const float kP14StartMr[P14_MODELS][3] = {
	{     60.0f, 42.0f, -40000.0f },
	{ -12000.0f, 44.0f,   -798.0f },
	{  12000.0f, 25.0f,   -798.0f }
};
static const float kP14EndMr[P14_MODELS][3] = {
	{     60.0f, 42.0f,   -800.0f },
	{     42.0f, 44.0f,   -798.0f },
	{     40.0f, 25.0f,   -798.0f }
};
static const float kP14StartGp[P14_MODELS][3] = {
	{     69.0f,  26.0f, -40000.0f },
	{ -12000.0f, -90.0f,   -798.0f },
	{  12000.0f,  40.0f,   -798.0f }
};
static const float kP14EndGp[P14_MODELS][3] = {
	{     69.0f,  26.0f,   -800.0f },
	{    252.0f, -90.0f,   -798.0f },
	{   -265.0f,  40.0f,   -798.0f }
};
static const float kP14Duration[P14_MODELS] = { 1.0f, 1.2f, 1.4f };

/*
 * The camera. Enter sets g_cameraPos - the +0x98 field of the 0x238
 * record - to (0, 0, 4500), and both enter and the tick call
 * Camera_LookAt with a target of (0, 0, 0) [verified].
 *
 * o THE VIEW IS NOT IDENTITY. Looking from (0,0,4500) at the origin
 *   gives a direction of (0,0,-1) and therefore a 180 degree yaw:
 *
 *       [-1 0 0][0 1 0][0 0 -1]
 *
 *   TREAT IT AS IDENTITY AND EVERY MODEL CLIPS AWAY SILENTLY - the
 *   ellipse's camZ comes out -44500, behind the camera. With the real
 *   matrix it is +44500 at the start and +5300 at the end, which is
 *   the zoom.
 *
 * Computed rather than hardcoded, because camera_look_at is what the
 * original runs every frame and the target will not always be the
 * origin.
 */
static const float kP14CamPos[3] = { 0.0f, 0.0f, 4500.0f };
static const float kP14Target[3] = { 0.0f, 0.0f, 0.0f };
static float       s_p14_view[12];

static MESH             s_p14_mesh[P14_MODELS];
static int              s_p14_mesh_ok[P14_MODELS];
static MR_ModelInstance s_p14_inst[P14_MODELS];
static MR_Primitive    *s_p14_prims[P14_MODELS];
static float            s_p14_t[P14_MODELS];      /* DAT_005420e8 */
static int              s_p14_models_ok;

static MR_Sprite s_p14_layer[4];
static int       s_p14_layer_ok[4];
static MR_Sprite s_p14_start;
static int       s_p14_start_ok;
static MR_Font   s_p14_font;
static int       s_p14_font_ok;
static double    s_p14_enter_time;
static double    s_p14_last_frame;
static double    s_p14_last_input;
static int       s_p14_reveal;      /* DAT_005409bc, 0..3 */

/* ORIGIN  MenuPage14_Title_Enter @ 0x004465f0 */
static void page14_enter(void)
{
	static const int idMr[4] = { RES_TITLE_MR, RES_TITLE2_MR,
		                         RES_TITLE3_MR, RES_TITLE4_MR };
	static const int idGp[4] = { RES_TITLE_GP, RES_TITLE2_GP,
		                         RES_TITLE3_GP, RES_TITLE4_GP };
	const int *ids = s_bmg ? idGp : idMr;
	int i;

	for (i = 0; i < 4; i++)
		s_p14_layer_ok[i] = load_res(&s_p14_layer[i], ids[i], 0, 0,
	                                 SPRITE_ANCHOR_NONE, "page14");

	/*
	 * THE START BUTTON, AND IT IS A REAL BUTTON. Enter positions
	 * sprite id 76 at (0, 0x1ae) and registers it:
	 *
	 *     Menu_AddSpriteButton(g_menu, start, 0, -1, 0x0d,
	 *                          -1, 0, NULL, 0, -1, -1, 0)
	 *
	 * at 0x00446a43 [verified]. The 0x0d is VK_RETURN as the button
	 * KEY CODE, and the -1, -1 position means Menu_Draw leaves the
	 * sprite where Enter put it.
	 *
	 * o SO RETURN IS NOT A SPECIAL CASE, it is this button's
	 *   shortcut. The widget gives all three - menu_hit_test the
	 *   hover and its entering-edge cue, menu_draw_buttons the
	 *   stipple-when-idle, and the key and the click landing in one
	 *   activation slot. Hit-testing a page's sprite by hand gets the
	 *   first and none of the rest; see menu_page_uses_buttons for
	 *   what that failure looks like.
	 */
	s_p14_start_ok = load_res(&s_p14_start, RES_START,
	                          POS_START_X, POS_START_Y,
	                          SPRITE_ANCHOR_CENTRE_X, "page14");

	menu_clear_buttons();
	if (s_p14_start_ok)
		menu_add_sprite_button(&s_p14_start, NULL, VK_RETURN);

	/*
	 * The strapline font. The tick asks Font_Get for id 0x1f = 31, and
	 * FONTS.BPT id 31 is "DataDiskMenu", type Direct [verified,
	 * MR_Font.txt section 5]. So the face is DATADISK.FNT and the
	 * class is Direct - which agrees with the data, since
	 * HIRES\DATADISK.FNT carries no palette and Direct fonts build
	 * their LUT from fore/back.
	 *
	 * o DO NOT PICK A FACE BECAUSE THE MENUS USE IT. MenuLink and
	 *   AntiAlias8Transp are the plausible guess here and are wrong;
	 *   the font id is in the decompilation and the id table is in
	 *   MR_Font.txt.
	 */
	if (!s_p14_font_ok) {
		s_p14_font_ok = fontdef_open(&s_p14_font, 31);
		if (s_p14_font_ok) {
			static const unsigned char yellow[3] = { 0xff, 0xff, 0x00 };
			static const unsigned char black[3]  = { 0x00, 0x00, 0x00 };
			font_set_colors(&s_p14_font, yellow, black);
		} else {
			mrlog_err("page14: font 31 did not load; no strapline\n");
		}
	}

	/*
	 * The bank the meshes texture from. Enter loads
	 * data\pages\titleMr\bnk00_00.raw into bank 3 and, on hardware,
	 * registers it with D3D [verified].
	 */
	bank_load(P14_BANK, s_bmg ? "PAGES\\TITLEGP\\BNK00_00.RAW"
	                   : "PAGES\\TITLEMR\\BNK00_00.RAW", 1);

	/*
	 * And the texture-slot flags, which is where the transparency
	 * comes from. Enter calls D3D_AllocTextureSlot at 0x00446690 with
	 * these arguments [verified, read off the pushes at
	 * 0x00446671..0x0044668e]:
	 *
	 *     bankIdx 3   small 0    wantKey 1   paletted 0
	 *     palBits 0   rgbBits 16 subIdx 0    w 0  h 0  pMemOut 0
	 *
	 * o WANTKEY IS 1 AND RGBBITS IS 16, so bank 3 carries both 0x10
	 *   and 0x02 - which is exactly the pair the blend ladder needs to
	 *   choose SRCALPHA. Without it the page draws its key colour,
	 *   black, as opaque: the title billboards appeared as black
	 *   squares with the art inside them.
	 *
	 * o NOT TO BE CONFUSED with maybe_D3D_RegisterTexture's keyBlack
	 *   argument, which page 14 passes as ZERO at 0x004466a7. That one
	 *   drives a different bit, 0x400, and it is NOT a colour key -
	 *   it keys green and PROTECTS near-black. See bank.h.
	 *
	 * o AND THIS IS THE ONLY CALL THE KEY NEEDS. wantKey 1 is what
	 *   makes black transparent, so there is nothing to switch on
	 *   afterwards. The three billboards sample 21, 29 and 32 per cent
	 *   black inside their UV rectangles, and that black is the
	 *   background around the oval and the lettering - without the key
	 *   they draw as black squares.
	 */
	bank_alloc_texture_slot(P14_BANK, 1, 0, 0, 16);

	/*
	 * THE TWO CUES ENTER ENDS ON, in this order [verified, the last
	 * two calls in MenuPage14_Title_Enter, 0x00446b27 and 0x00446b30]:
	 *
	 *     FUN_00404990(3, 2)      CD track 3, looping
	 *     FUN_004049f0(0x2d, 0)   the bike that races past as the
	 *                             logo slides in
	 *
	 * Page 3 asks for the SAME TRACK with mode 3, so the main menu
	 * keeps this music playing rather than restarting it.
	 *
	 * THE CUE PLAYS AND THE TRACK DOES NOT. audio_cue reaches
	 * xa2_voice_play, so the bike pass is audible; audio_music_cue
	 * records the request and returns, because nothing plays CD
	 * audio yet - see audio.c.
	 */
	audio_music_cue(CD_TRACK_MENU, CD_MODE_LOOP);
	audio_cue(SND_TITLE_BIKE_PASS, 0);

	s_p14_models_ok = 1;
	for (i = 0; i < P14_MODELS; i++) {
		const char *const *nm = s_bmg ? kP14MeshGp[i] : kP14MeshMr[i];
		const float *sc = s_bmg ? kP14ScaleGp[i] : kP14ScaleMr[i];
		const float *st = s_bmg ? kP14StartGp[i] : kP14StartMr[i];
		int nprims;

		/*
		 * MESH_SCALE_3DF_MAP, NOT ZERO. Load3dfAndMap ALWAYS applies
		 * its own scale of 96.0 - unlike Load3df, which applies its
		 * argument only when non-zero [verified, mesh.h].
		 *
		 * PASS 0 AND EVERY TITLE MESH LOADS 96 TIMES TOO SMALL. The
		 * quads still draw, transform and cull correctly; they come
		 * out sub-pixel, which on screen cannot be told apart from
		 * not being drawn.
		 */
		s_p14_mesh_ok[i] = mesh_load_with_map(&s_p14_mesh[i], nm[0],
		                                      nm[1], MESH_SCALE_3DF_MAP);
		if (!s_p14_mesh_ok[i]) {
			mrlog_err("page14: %s did not load\n", nm[0]);
			s_p14_models_ok = 0;
			continue;
		}
		mesh_scale(&s_p14_mesh[i], sc[0], sc[1], sc[2]);

		/*
		 * THE COUNT GOES IN A LOCAL, and that is not a style choice.
		 *
		 * Pass &s_p14_inst[i].primCount straight to
		 * prim_build_from_mesh and model_init memsets the whole
		 * instance afterwards, ZEROING IT AGAIN before
		 * model_set_prims reads it back: every model gets primCount
		 * 0, model_draw returns at its guard, and nothing draws.
		 *
		 * o WRITE INTO A STRUCT, CLEAR THE STRUCT, READ IT BACK.
		 *   Each statement is right on its own and the ORDER makes
		 *   them wrong, which is why re-reading them one at a time
		 *   does not find it.
		 */
		nprims = 0;
		if (!prim_build_from_mesh_bank(&s_p14_mesh[i], &s_p14_prims[i],
		                               &nprims, 0, P14_BANK)
		    || nprims <= 0) {
			mrlog_err("page14: %s produced no primitives\n", nm[0]);
			s_p14_models_ok = 0;
			continue;
		}

		/*
		 * THE BANK INDICES ARE REBASED, by the P14_BANK argument to
		 * the build above. Enter calls FUN_00422250(3) once per mesh
		 * right after loading the three of them, and that function
		 * ADDS 3 to every face's bank index [verified].
		 *
		 * The .map numbers its banks relative to its own page; the
		 * texture was loaded into global bank 3. Without the shift
		 * every face points at a bank nobody loaded, prim_is_textured
		 * returns 0, and the quad draws as its flatColor - which is
		 * 0xffffffff for a textured face. A WHITE SQUARE.
		 */

		/*
		 * The rotation every instance starts with is (0, PI/2, 0) -
		 * page 14 passes 0x3fc90fdb as the second component
		 * [verified].
		 */
		{
			static const float rot0[3] = { 0.0f, 1.5707964f, 0.0f };
			model_init(&s_p14_inst[i], st, rot0, &s_p14_mesh[i]);
			model_set_prims(&s_p14_inst[i], s_p14_prims[i], nprims);
		}
		s_p14_t[i] = 0.0f;
	}

	/*
	 * MenuPage14_Title_Enter pushes 0x3f800000 to Screen3D_SetProjZoom
	 * at 0x004469d2 [verified]. Zoom 1.0, so the scales come out equal
	 * to the bases menu_set_page just installed - 1911 on both axes.
	 *
	 * It is not a no-op even though 1.0 is the value Screen3D_Open
	 * leaves behind: any page that zoomed would still be zoomed, and
	 * page 14 is reachable from those pages.
	 */
	screen3d_set_proj_zoom(1.0f);


	s_p14_enter_time = tm_now();
	s_p14_last_frame = s_p14_enter_time;
	s_p14_last_input = s_p14_enter_time;
	s_p14_reveal     = 0;

	mrlog("page14: title screen, %s branding\n",
	       s_bmg ? "BMG/GP" : "MotoRacer");
}

/*
 * ORIGIN  MenuPage14_Title_Tick @ 0x00446b80, at 0x00446d27
 *
 * The per-object curve, transcribed from the tick's loop.
 *
 *     t[i] += dtMs / (duration[i] * 1000)
 *     u = clamp(t[i], 0, 1)
 *     position = lerp(start[i], end[i], u)
 *     if (t[i] > 1) finished++
 *
 * OBJECT 0 ALSO ROTATES, and only object 0 - which is the one-rotator-
 * two-sliders shape the retail recording shows [verified]:
 *
 *     u < 0.5:  rotation = (u * 2pi, -pi/2, pi)
 *     u >= 0.5: rotation = (0, u * 2pi + pi/2, 0)
 *
 * The constants are read from the binary [verified]: 0x004d0c08 is
 * 2.0f, 0x004d0c10 the double pi, 0x004d0c20 the double pi/2, and
 * the two floats this code writes literally are beside them -
 * 0x004d0c18 is -1.5707964f and 0x004d0c1c is 3.1415927f. So the
 * term is a FULL TURN over the animation, and the second half swaps
 * which axis carries it.
 *
 * WHICH AXIS IS WHICH. The mesh is a quad in the YZ plane - all four
 * vertices share an x - so its normal is local X, and what each half
 * of the animation does is best read as where that normal points.
 * With the rest pose (0, pi/2, 0) putting it at (0, 0, -1), face-on:
 *
 *   FIRST HALF, rot = (turn, -pi/2, pi): the normal sweeps to
 *   (0, sin turn, -cos turn) - VERTICALLY, so this is a rotation
 *   about the HORIZONTAL axis. The oval flips top over bottom.
 *
 *   SECOND HALF, rot = (0, turn + pi/2, 0): the normal sweeps to
 *   (cos B, 0, -sin B) - HORIZONTALLY, so a rotation about the
 *   VERTICAL axis, ending face-on again.
 *
 * Horizontal flip then vertical rotation, which is the order the
 * retail recording shows [observed]. Each half turns 180 degrees and
 * passes through edge-on at its midpoint; the mesh is double-sided -
 * two coincident faces of opposite winding - so the far side takes
 * over rather than the object vanishing.
 *
 * o THE WHOLE READING DEPENDS ON mat34_from_euler. Get that matrix
 *   wrong and the rest pose collapses to the identity: the billboard
 *   sits edge-on, both halves land on the wrong axes, and the second
 *   becomes a spin about the quad's OWN normal - a knife-edge sliver
 *   that never resolves. THE SYMPTOM IS ON THIS PAGE AND THE CAUSE
 *   IS IN model.c, which is why it is worth saying here.
 *
 * o THE END POSE IS FACE-ON. At u = 1 the term is 2pi + pi/2, which
 *   is pi/2 modulo a full turn. The trig macros mask with 0x3fff, so
 *   the out-of-range index wraps rather than reading off the end of
 *   the table.
 *
 * Returns the number of objects that have finished.
 */
static int page14_animate(double dtMs)
{
	int i, finished = 0;

	if (!s_p14_models_ok)
		return 0;

	for (i = 0; i < P14_MODELS; i++) {
		const float *st = s_bmg ? kP14StartGp[i] : kP14StartMr[i];
		const float *en = s_bmg ? kP14EndGp[i]   : kP14EndMr[i];
		float u;
		int   k;

		if (!s_p14_mesh_ok[i])
			continue;

		s_p14_t[i] += (float)(dtMs / (kP14Duration[i] * 1000.0));

		u = s_p14_t[i];
		if (u < 0.0f) u = 0.0f;
		if (u > 1.0f) u = 1.0f;

		if (i == 0) {
			float turn = u * 2.0f * 3.1415927f;
			if (u < 0.5f) {
				s_p14_inst[i].rotation[0] = turn;
				s_p14_inst[i].rotation[1] = -1.5707964f;
				s_p14_inst[i].rotation[2] = 3.1415927f;
			} else {
				s_p14_inst[i].rotation[0] = 0.0f;
				s_p14_inst[i].rotation[1] = turn + 1.5707964f;
				s_p14_inst[i].rotation[2] = 0.0f;
			}
		}

		if (s_p14_t[i] > 1.0f) {
			finished++;
			for (k = 0; k < 3; k++)
				s_p14_inst[i].position[k] = en[k];
		} else {
			for (k = 0; k < 3; k++)
				s_p14_inst[i].position[k] = (en[k] - st[k]) * u + st[k];
		}
	}
	return finished;
}

/* ORIGIN  MenuPage14_Title_Tick @ 0x00446b80 */
static void page14_tick(void)
{
	double now = tm_now();
	double dtMs;
	int    i;
	int    finished;

	/*
	 * dt in MILLISECONDS, clamped. The original reads timeGetTime and
	 * substitutes 100 when the gap exceeds 500 - 0x004d0bf4 is 500.0
	 * and 0x004d0bf8 beside it is 100.0. THE SUBSTITUTE IS NOT THE
	 * THRESHOLD, which is the easy misreading [verified].
	 */
	dtMs = (now - s_p14_last_frame) * 1000.0;
	if (dtMs > 500.0)
		dtMs = 100.0;
	if (dtMs < 0.0)
		dtMs = 0.0;
	s_p14_last_frame = now;

	finished = page14_animate(dtMs);

	/*
	 * THE PHASE IS THE COUNT OF FINISHED ANIMATIONS, and nothing
	 * else: the original computes the number of instances whose
	 * parameter has reached 1.0, raises DAT_005409bc to it and NEVER
	 * LOWERS IT [verified]. The three durations are 1.0, 1.2 and 1.4
	 * seconds, so the phase climbs 0, 1, 2, 3 over the animation.
	 *
	 * o DO NOT DRIVE IT FROM A TIMER. Advancing on a fixed 400 ms
	 *   looks roughly right and fights the real count - CLAUDE.md
	 *   rule 5a.
	 */
	if (finished > s_p14_reveal)
		s_p14_reveal = finished;
	if (s_p14_reveal > 3)
		s_p14_reveal = 3;

	/*
	 * ONE SPRITE, NOT A STACK OF LAYERS. The tick selects a single
	 * title sprite by phase - 0 draws title, 1 title2, 2 title3, and
	 * anything above 2 draws title4 and takes an early exit past the
	 * animation entirely [verified].
	 *
	 * o THE ENTER HANDLER IS THE ONE THAT LAYERS. It draws all four
	 *   in one frame, back to front - title4, title3, title2, title -
	 *   which is a single ESTABLISHING FRAME, not the steady state.
	 *   Read its behaviour as the tick's and you get a cumulative
	 *   stack every frame.
	 */
	i = (s_p14_reveal > 2) ? 3 : s_p14_reveal;
	if (s_p14_layer_ok[i])
		sprite_draw(&s_p14_layer[i], SPRITE_MODE_USE_KIND, 100, 0);

	/*
	 * The START button is registered by the enter handler and drawn
	 * from the widget list every frame, not gated on the phase.
	 */
	if (s_p14_start_ok) {
		/*
		 * THE MODE COMES FROM THE HOVER STATE, and this is the whole
		 * of the stipple. Menu_Draw builds a state word per button and
		 * switches on it [verified, 0x0040c1f0]:
		 *
		 *     bit 2  shares the hovered button's groupId
		 *     bit 1  IS the hovered button
		 *     bit 0  boundVar == boundValue
		 *     blink phase forces the state to 0
		 *
		 *     state 0  spriteB ? mode 5 : mode 3
		 *     state 2  spriteB ? mode 5 : mode 5
		 *     state 4, 5           mode 2
		 *     state 3              draws nothing at all
		 *
		 * START has only a spriteA and no bound variable or group, so
		 * it only ever reaches state 0 or state 2 - MODE 3 UNHOVERED,
		 * MODE 5 HOVERED. Mode 3 is stipple-plus-key, which is the
		 * chequerboard the retail button shows; mode 5 is the sentinel
		 * "use the sprite's own kind", i.e. solid.
		 *
		 * o HARDCODE THE MODE TO USE_KIND AND THE BUTTON DRAWS SOLID
		 *   IN BOTH STATES. The stipple is not missing from the
		 *   sprite layer - sprite.h carries mode 3 - so the thing to
		 *   check is the SELECTION, not the drawing.
		 *
		 * menu_draw_buttons applies exactly this rule to any
		 * single-sprite button, and menu_hit_test gives the hover cue
		 * on the entering edge only - Menu_UpdateHover plays 0x29
		 * when the index CHANGES to something that is not -1
		 * [verified, 0x00409a0c].
		 */
		menu_hit_test();
		menu_draw_buttons();

		if (menu_get_activated() >= 0) {
			/* Menu_GetActivated -> Menu_GotoPage(0xf) at 0x004470bb,
			 * with no glass transition [verified]. */
			menu_clear_activated();
			s_p14_last_input = now;
			menu_goto_page(MENU_PAGE_NAME);
			return;
		}
	}


	/*
	 * The strapline, once the title is fully up - which is what the
	 * retail recording shows too: it appears only after the logo slide
	 * finishes. The phase gate is the same fact from the other side.
	 *
	 * o THE BLINK IS IN OUR BINARY AND THE RECORDING DOES NOT BLINK.
	 *   The tick increments a counter every frame, draws while it is
	 *   above 10 and resets past 20 - a 50 per cent duty cycle over 20
	 *   frames [verified]. Kept, because the code is the authority;
	 *   recorded as a discrepancy in menu-pages.txt. The likeliest
	 *   explanation is that the recording is of 3.20 and ours is 3.22.
	 */
	if (s_p14_reveal >= 3 && s_p14_font_ok) {
		double phase = now / TITLE_BLINK_PERIOD;
		if ((((int)phase) & 1) == 0) {
			const char *msg = "FULL D3D VERSION";
			int w = font_text_width(&s_p14_font, msg);
			/* AUTHORED width, not the frame's: font_draw_text
			 * adds the 2D origin itself. */
			font_draw_text(&s_p14_font, (FB_DESIGN_W - w) / 2,
			               TITLE_TEXT_Y, msg, 0);
		}
	}

	/*
	 * The attract-demo timeout, which lives in Menu_Tick in the
	 * original rather than in the page. Kept with the page because it
	 * is the only page it applies to and putting a page number in the
	 * driver to serve one page is worse.
	 *
	 * [open] IT ONLY LOGS. The original calls
	 * maybe_Menu_StartAttractDemo, which goes to page 1 - PUSH 1
	 * before the Menu_SetPage at 0x00409b86 [verified]. Ours detects
	 * the idle and says so.
	 */
	if ((now - s_p14_last_input) * 1000.0 > TITLE_IDLE_MS) {
		mrlog("page14: idle %.0f s, attract demo would start "
		       "(page %d)\n", TITLE_IDLE_MS / 1000.0, MENU_PAGE_LOADING);
		s_p14_last_input = now;     /* do not spam */
	}
}

/* ORIGIN  MenuPage14_Title_Leave @ 0x00446b40 */
static void page14_leave(void)
{
	int i;
	for (i = 0; i < 4; i++) {
		if (s_p14_layer_ok[i])
			sprite_free(&s_p14_layer[i]);
		s_p14_layer_ok[i] = 0;
	}
	menu_clear_buttons();
	if (s_p14_start_ok) {
		sprite_free(&s_p14_start);
		s_p14_start_ok = 0;
	}
	for (i = 0; i < P14_MODELS; i++) {
		if (s_p14_prims[i]) {
			free(s_p14_prims[i]);
			s_p14_prims[i] = NULL;
		}
		if (s_p14_mesh_ok[i]) {
			mesh_free(&s_p14_mesh[i]);
			s_p14_mesh_ok[i] = 0;
		}
	}
	s_p14_models_ok = 0;
}

/*
 * ORIGIN  NONE - ours. The original binds VK_RETURN to the
 *         START button instead - see the note in the body.
 */
static int page14_on_key(int vkey)
{
	/*
	 * THE KEY GOES TO THE BUTTON, not to a test here. START
	 * registers VK_RETURN as its own key code, so the same
	 * activation slot serves the key and the click and the tick does
	 * not care which happened.
	 *
	 * The timestamp is this page's though: the attract demo counts
	 * idle time, and Menu_OnKey stamps lastInputTime on every key
	 * whether a button claims it or not [verified, 0x0040a0f0].
	 */
	s_p14_last_input = tm_now();
	return menu_buttons_key(vkey);
}


/* ================================================================ */
/* Page 15 - name entry, the 3D keyboard                            */
/* ================================================================ */

/*
 * The first page built on MR_MenuSceneObject rather than on
 * MR_ModelInstance - see scene.h - and menu-pages.txt section 2.3
 * has the full account.
 *
 * TWENTY-EIGHT GLYPHS IN ONE TABLE: 'a'..'z', then backspace, then
 * valid. That is not an accident of storage, it is what makes the
 * keyboard grid come out exactly full at 4 rows of 7.
 */
#define P15_GLYPHS     28
#define P15_BACKSPACE  26
#define P15_VALID      27
#define P15_GRID_COLS   7
#define P15_GRID_ROWS   4

/*
 * The grid, from the loop in MenuPage15_NameEntry_Enter
 * (0x00487070) [verified, constants read out of .rdata as doubles]:
 *
 *     x = col * (1/6) * 1100 - 550
 *     y = (3 - row) * (1/3) * 500 - 250
 *
 * 1100 wide by 500 tall, centred on the origin, 183.33 between
 * columns and 166.67 between rows. The row term counts DOWN from 3,
 * so row 0 is the top.
 */
#define P15_GRID_X_STEP (1.0f / 6.0f)
#define P15_GRID_X_SPAN 1100.0f
#define P15_GRID_X_BIAS  550.0f
#define P15_GRID_Y_STEP (1.0f / 3.0f)
#define P15_GRID_Y_SPAN  500.0f
#define P15_GRID_Y_BIAS  250.0f

/* The two sizes MenuTools_NormaliseMeshToSize is called with:
 * 0x42780000 is 62.0f and 0x429c0000 is 78.0f. */
#define P15_GLYPH_HALF   62.0f
#define P15_CADRE_HALF   78.0f

/*
 * The scatter the glyphs fly in from, and the ease that lands them.
 * rand() is normalised by 1/32768 and mapped to -1..1, then scaled.
 */
#define P15_RAND_SCALE  (1.0f / 32768.0f)
#define P15_SCATTER_X   2048.0f
#define P15_SCATTER_Y    512.0f
#define P15_SCATTER_Y0  2250.0f
#define P15_SCATTER_Z   1024.0f
#define P15_SCATTER_Z0  4096.0f
#define P15_SPIN_TURNS     2.0f
/* 0x004d1aa0, read as a double and used as a float. */
#define P15_TWO_PI      ((float)MR_TWO_PI)
#define P15_FLY_SECONDS    2.0f

/* Screen3D_SetProjBaseX/Y(1024.0) at the head of Enter. */
#define P15_PROJ_BASE   1024.0f

/* And what the tick leaves it at on the way out - NOT the 480 default. */
#define P15_PROJ_EXIT   1911.0f

/*
 * WHERE THE NAME LIVES. The original keeps it in the registry; we keep
 * it in the .ini beside the exe. See config.h - it is a deliberate
 * divergence, not a gap.
 */
#define P15_INI_SECTION "PLAYER"
#define P15_INI_KEY     "name"

/*
 * THE WAVE. Once the glyphs land, the whole keyboard ripples in Z.
 * It is driven by THREE separate animated objects rather than by a
 * clock, which is why it looks organic and why it starts smoothly:
 *
 *   a phase spinner   vec[0] free-running at 2*pi rad/s (mode 2)
 *   an amplitude ramp target[0] eased 0 -> 120 over 2 s, linear
 *   a highlight spin  vec[1] free-running at 2*pi rad/s (mode 2)
 *
 * and then, per glyph [verified, the double loop at 0x00487dac]:
 *
 *     z = cos(phase) * amplitude
 *     phase += pi/6      per column
 *     phase += 2*pi/14   per row
 *
 * o THE AMPLITUDE RAMP IS WHAT SEQUENCES IT. It only starts when the
 *   FIRST glyph's rotation move finishes, so the ripple grows out of
 *   the fly-in instead of snapping on at full size.
 */
/* Both are DOUBLES in the image and floats here, as P15_TWO_PI is. */
#define P15_WAVE_COL_STEP  0.5235988f    /* 0x004d1b60, pi/6        */
#define P15_WAVE_ROW_STEP  0.4487990f    /* 0x004d1b68, 2*pi/14     */
#define P15_WAVE_AMPLITUDE 120.0f        /* 0x42f00000              */
#define P15_WAVE_RAMP_SEC    2.0f
#define P15_SPIN_RATE   ((float)MR_TWO_PI) /* one turn per second   */

/*
 * The selection, 0..27. 26 is backspace and 27 is valid, so the two
 * function keys are simply the last two cells of the grid and the
 * arrow keys walk onto them like any other.
 */
#define P15_SEL_BACKSPACE  26
#define P15_SEL_VALID      27

/*
 * THE NINE NAME SLOTS, from the loop at the tail of Enter:
 *
 *     x = i * 140 - 490      i 0..8
 *     y = -325
 *     z = -600
 *
 * Eight are the name - it is clamped to eight characters - and the
 * ninth is written after the loop. The CURSOR sits on slot
 * [nameLength], which is why there has to be a ninth: TYPING an
 * eighth letter still moves it, onto slot 8. A full name arriving
 * from the ini does not - see the two guards at the fly-in handoff
 * in page15_tick.
 */
#define P15_NAME_SLOTS      9
#define P15_NAME_STEP     140.0f
#define P15_NAME_BIAS     490.0f
#define P15_NAME_Y       (-325.0f)
#define P15_NAME_Z       (-600.0f)

/*
 * The cursor's parked position before the letters land, and the ease
 * that brings it in. Enter puts it at (0, 800, 500); the tick moves
 * it onto slot 0 over one second once the fly-in finishes.
 */
#define P15_CURSEUR_PARK_Y 800.0f
#define P15_CURSEUR_PARK_Z 500.0f
#define P15_CURSEUR_IN_SEC   1.0f

/*
 * FUN_0047af50(camera, rot {0,0,0}, pos {0,0,-2400}) - the menu
 * camera build. We do not have MR_MenuCamera yet, so the equivalent
 * is expressed as a look-at from the same point: an identity rotation
 * at z = -2400 looks along +Z, and everything on this page lives at
 * positive z relative to it - the grid at 0, the name slots at -600,
 * the scatter out at 4096.
 *
 * o RECORDED AS A DEVIATION rather than a transcription. The original
 *   builds two 3x4 matrices and inverts one; camera_look_at is our
 *   only camera and it takes an eye and a target. For a camera with
 *   zero rotation the two agree, which is the only case this page
 *   uses - but the moment a menu camera rotates, this has to become
 *   the real thing. See structs/MR_MenuCamera.txt.
 */
static const float kP15CamPos[3] = { 0.0f, 0.0f, -2400.0f };
static const float kP15Target[3] = { 0.0f, 0.0f,     0.0f };

static MR_SceneObject   s_p15_glyph[P15_GLYPHS];
static MR_ModelInstance s_p15_inst[P15_GLYPHS];
static MR_Primitive    *s_p15_prims[P15_GLYPHS];
static float            s_p15_grid[P15_GLYPHS][3];
static int              s_p15_glyph_ok[P15_GLYPHS];

/*
 * THE 2D LAYER, which is only two sprites [verified, the head of the
 * tick at 0x00487bc4]:
 *
 *     Sprite_UpdateMove(res 2,   0, 1, 0, 0)   then Sprite_Draw
 *     Sprite_UpdateMove(res 105, 0, 0, 0, 0)   then Sprite_Draw
 *
 * Resource 2 is paralax3.lez, KIND 4 - the scrolling background. Its
 * update is passed (0, 1), so it creeps one pixel down the tile a
 * frame and wraps every 128. Resource 105 is enternam.lez, an
 * ordinary kind-1 sprite carrying both banners - the black MOTORACER
 * strip and the yellow ENTER YOUR NAME strip are one image.
 *
 * o THE BACKGROUND IS OPAQUE AND FULL SCREEN, so it is drawn FIRST
 *   and replaces the clear. Drawing it after anything else would
 *   erase it.
 */
#define P15_RES_BACKDROP    2      /* paralax3.lez, kind 4 */
#define P15_RES_BANNERS   105      /* enternam.lez         */

static MR_Sprite        s_p15_backdrop;
static MR_Sprite        s_p15_banners;
static int              s_p15_backdrop_ok;
static int              s_p15_banners_ok;

static MR_SceneObject   s_p15_cadre;
static MR_SceneObject   s_p15_curseur;
static int              s_p15_cadre_ok;
static int              s_p15_curseur_ok;
static MR_ModelInstance s_p15_cadre_inst;
static MR_ModelInstance s_p15_curseur_inst;
static MR_Primitive    *s_p15_cadre_prims;
static MR_Primitive    *s_p15_curseur_prims;
static float            s_p15_slotPos[P15_NAME_SLOTS][3];
static int              s_p15_nameLen;     /* 0x005f1a44 */
static int              s_p15_done;        /* 0x005f675c */
static int              s_p15_lastCurX = -1;
static int              s_p15_lastCurY = -1;
static int              s_p15_nprims[P15_GLYPHS];

/*
 * THE NAME SLOTS, 0x005f6530. Nine scene objects that BORROW a glyph's
 * mesh by pointer rather than loading one - the original copies +0x18
 * straight across. So a slot has animation state of its own and no
 * geometry of its own, and which letter it shows is decided at the
 * moment it is typed.
 */
static MR_SceneObject   s_p15_slotObj[P15_NAME_SLOTS];
static MR_ModelInstance s_p15_slotInst[P15_NAME_SLOTS];
static int              s_p15_slotGlyph[P15_NAME_SLOTS];  /* -1 = empty */
static char             s_p15_name[P15_NAME_SLOTS + 1];

/*
 * The three durations typing uses, all read off their call sites:
 * the slot's flight to the name line, the LETTER'S OWN hop back to
 * its cell, and the cursor's step to the next slot.
 */
#define P15_TYPE_SLOT_SEC   1.0f    /* 0x3f800000, ease  */
#define P15_TYPE_LETTER_SEC 1.5f    /* 0x3fc00000, ease  */
#define P15_TYPE_CURSOR_SEC 0.2f    /* 0x3e4ccccd, LINEAR */

/*
 * THE ONCE-FLAG, which is 0x004f1284 in the original. Enter loads 30
 * meshes and guards the whole block, so re-entering the page does not
 * reload - and, importantly, does not re-apply the mesh mutations
 * below, which are not idempotent.
 */
static int              s_p15_loaded;
static int              s_p15_ok;

/*
 * Three animation drivers with no mesh of their own. The original
 * gives each a full MR_MenuSceneObject too - at 0x005f6480,
 * 0x005f63e8 and 0x005f1690 - and reads one component out of each.
 */
static MR_SceneObject   s_p15_wavePhase;   /* 0x005f6480, vec[0]    */
static MR_SceneObject   s_p15_waveAmp;     /* 0x005f63e8, target[0] */
static MR_SceneObject   s_p15_spin;        /* 0x005f1690, vec[1]    */

static int              s_p15_sel;         /* 0x005f647c            */
static int              s_p15_flyingIn;    /* 0x005f6528            */
static float            s_p15_view[12];
static float            s_p15_eye[3];

/*
 * ORIGIN  MenuPage15_NameEntry_Tick @ 0x00487bb0, the random spin
 *         term - rand() mapped to -1..1, its exact expression.
 */
static float p15_rand_signed(void)
{
	return (float)rand() * P15_RAND_SCALE * 2.0f - 1.0f;
}

/*
 * ORIGIN  NONE - ours
 *
 * Build the drawable half of a scene object: primitives and a model
 * instance. The scene object owns the animation, the instance owns
 * the draw, and their rotation/position sit at the same offsets - so
 * each frame is a copy, not a conversion.
 *
 * o kP15Target IS BEING USED AS A ZERO VECTOR, not as the camera
 *   target it is named for. The initial position and rotation are
 *   both overwritten on the first frame by that copy, so the value
 *   never reaches the screen - but three call sites read as though
 *   the instance starts at the camera's aim point, and it does not.
 */
static int p15_build(MR_SceneObject *o, MR_ModelInstance *inst,
                     MR_Primitive **prims, int *outCount, const char *who)
{
	int nprims = 0;

	if (!prim_build_from_mesh(o->mesh, prims, &nprims) || nprims <= 0) {
		mrlog_err("page15: %s produced no primitives\n", who);
		return 0;
	}
	model_init(inst, kP15Target, kP15Target, o->mesh);
	model_set_prims(inst, *prims, nprims);
	if (outCount)
		*outCount = nprims;
	return 1;
}

/*
 * ORIGIN  MenuPage15_NameEntry_Enter @ 0x00487070, the per-glyph load
 *
 * Load one glyph and put it in the state the page wants it in.
 *
 * *** THE MIRROR IS APPLIED TWICE ON PURPOSE. ***
 * scene_object_load goes through mesh_load_with_map, which applies
 * Mesh_MirrorXAndFlipWinding because Load3dfAndMap does. Enter then
 * calls FUN_0047ad40 on every glyph, which is the SAME operation
 * again - it builds a matrix from cos(0), cos(90), cos(180), cos(270)
 * that multiplies out to (x,y,z) -> (-x,y,z), then reverses every
 * face's winding. So page 15's meshes end up in the .3df's own
 * handedness while every other 3D menu page's end up mirrored.
 *
 * o DO NOT "SIMPLIFY" THE PAIR AWAY. It is two calls in the original
 *   and it is two here, so that the day either side changes, the
 *   cancelled pair is what tells you. Dropping the second one draws
 *   the whole alphabet backwards - which, unlike the hollow camera on
 *   page 25, is impossible to miss and therefore a useful check.
 */
static int p15_load_glyph(int i, const char *name3df, const char *nameMap)
{
	if (!scene_object_load(&s_p15_glyph[i], name3df, nameMap)) {
		mrlog_err("page15: %s did not load\n", name3df);
		return 0;
	}

	/*
	 * THE SECOND MIRROR, AND IT IS FOR THE 28 GLYPHS ONLY. Enter's
	 * mirror loop runs over the table at 0x005f1880 and stops at 28;
	 * cadre and curseur are separate pointers and are NOT in it. So
	 * they keep the single mirror Load3dfAndMap applied and the
	 * glyphs get a second one that cancels it. Two objects loaded
	 * the same way, deliberately ending in opposite handedness.
	 */
	scene_mirror_mesh(&s_p15_glyph[i]);

	if (!p15_build(&s_p15_glyph[i], &s_p15_inst[i], &s_p15_prims[i],
	               &s_p15_nprims[i], name3df))
	    return 0;
	s_p15_glyph_ok[i] = 1;
	return 1;
}

/*
 * ORIGIN  FUN_00487a70 @ 0x00487a70, whole. idx is 0..25.
 *
 * FOUR THINGS MOVE, and the third is the one that gives the page its
 * character:
 *
 *   1  the slot borrows the glyph's mesh by pointer;
 *   2  it starts at the glyph's CURRENT rotation and its GRID cell,
 *      then flies to the name line over 1 s easing its spin to zero;
 *   3  THE TYPED LETTER ITSELF is teleported up to the park position
 *      and flies back to its own cell over 1.5 s - so the letter you
 *      pick visibly leaps out of the keyboard and drops back in;
 *   4  the cursor steps to the next slot over 0.2 s, LINEAR, not
 *      eased - it is the one move on this page that is not eased,
 *      which is what makes it feel like a caret rather than a
 *      flying object.
 *
 * and the selection follows the typed letter, which is why typing on
 * the keyboard drags the frame around.
 */
static void p15_type_letter(int idx)
{
	float zero[3] = { 0.0f, 0.0f, 0.0f };
	float park[3] = { 0.0f, P15_CURSEUR_PARK_Y, P15_CURSEUR_PARK_Z };
	int   n       = s_p15_nameLen;

	if (idx < 0 || idx >= 26 || n >= 8)
		return;
	if (!s_p15_glyph_ok[idx])
		return;

	/* 1 - borrow the mesh, and the primitives that go with it. */
	s_p15_slotGlyph[n]  = idx;
	s_p15_slotObj[n].mesh = s_p15_glyph[idx].mesh;
	model_init(&s_p15_slotInst[n], kP15Target, kP15Target,
	           s_p15_glyph[idx].mesh);
	model_set_prims(&s_p15_slotInst[n], s_p15_prims[idx],
	                s_p15_nprims[idx]);

	/* 2 - from the glyph's live rotation and its grid cell. */
	scene_set_vec_pair(&s_p15_slotObj[n], s_p15_glyph[idx].vec,
	                   s_p15_grid[idx]);
	scene_start_target_move(&s_p15_slotObj[n], s_p15_slotPos[n],
	                        P15_TYPE_SLOT_SEC, SCENE_MODE_EASE);
	scene_start_vec_move(&s_p15_slotObj[n], zero,
	                     P15_TYPE_SLOT_SEC, SCENE_MODE_EASE);

	s_p15_name[n]     = (char)('A' + idx);
	s_p15_name[n + 1] = 0;

	/* 3 - the letter leaps out and comes back. */
	scene_set_target(&s_p15_glyph[idx], park);
	scene_start_target_move(&s_p15_glyph[idx], s_p15_grid[idx],
	                        P15_TYPE_LETTER_SEC, SCENE_MODE_EASE);

	s_p15_nameLen = n + 1;
	s_p15_sel     = idx;

	/* 4 - the caret steps on. */
	if (s_p15_curseur_ok && s_p15_nameLen < P15_NAME_SLOTS)
		scene_start_target_move(&s_p15_curseur,
	                            s_p15_slotPos[s_p15_nameLen],
	                            P15_TYPE_CURSOR_SEC,
	                            SCENE_MODE_LINEAR);
}

/*
 * ORIGIN  MenuPage15_NameEntry_Tick @ 0x00487bb0, the case 8 arm
 *
 * The removed slot is thrown back up to the park position with a RANDOM spin -
 * three draws scaled by 2*pi and then by 3, so it tumbles hard on the
 * way out - and the selection jumps to the backspace cell.
 */
static void p15_backspace(void)
{
	float park[3] = { 0.0f, P15_CURSEUR_PARK_Y, P15_CURSEUR_PARK_Z };
	float spin[3];
	int   n;

	if (s_p15_nameLen <= 0)
		return;

	n = s_p15_nameLen - 1;
	s_p15_name[n] = 0;
	s_p15_nameLen = n;

	spin[0] = p15_rand_signed() * P15_TWO_PI * 3.0f;
	spin[1] = p15_rand_signed() * P15_TWO_PI * 3.0f;
	spin[2] = p15_rand_signed() * P15_TWO_PI * 3.0f;

	scene_start_target_move(&s_p15_slotObj[n], park,
	                        P15_TYPE_SLOT_SEC, SCENE_MODE_EASE);
	scene_start_vec_move(&s_p15_slotObj[n], spin,
	                     P15_TYPE_SLOT_SEC, SCENE_MODE_EASE);

	s_p15_sel = P15_SEL_BACKSPACE;

	if (s_p15_curseur_ok && s_p15_nameLen < P15_NAME_SLOTS)
		scene_start_target_move(&s_p15_curseur,
	                            s_p15_slotPos[s_p15_nameLen],
	                            P15_TYPE_CURSOR_SEC,
	                            SCENE_MODE_LINEAR);

	/*
	 * THE SLOT IS NOT MARKED EMPTY. It keeps drawing while it flies
	 * away, exactly as the original leaves it - clearing slotGlyph
	 * here would make the deleted letter vanish instead of being
	 * thrown off the top. It is overwritten by the next type.
	 */
}

/*
 * ORIGIN  MenuPage15_NameEntry_Tick @ 0x00487bb0, the proximity search
 *
 * p15_hit_test - the mouse selection, from the block the original
 * guards with g_menu->hitTestDirty.
 *
 * It is NOT a hit test in the usual sense. All 28 glyph positions are
 * projected and the nearest one to the cursor by SQUARED DISTANCE
 * wins, so the selection snaps to the closest letter whether or not
 * the pointer is over anything. For glyphs that ripple in and out of
 * the screen that is the right choice - a rectangle test would drop
 * the selection every time a letter waved away from the cursor.
 *
 * o Z IS FORCED TO ZERO. The points are built from each glyph's live
 *   x and y but at the GRID PLANE, so the wave does not change what
 *   the mouse picks. Without that the letters nearest the camera
 *   would project larger and win more often as the ripple passed.
 *
 * o AND IT ONLY RUNS WHEN THE POINTER MOVES. The original gates on
 *   hitTestDirty; we compare against the last cursor position. Either
 *   way it must not run every frame, or it would immediately undo
 *   every keyboard selection.
 */
static void p15_hit_test(void)
{
	float      pts[P15_GLYPHS][3];
	float      base[12];
	float      composed[12];
	MR_Vertex *v;
	int        i;
	int        best = 0x7fffffff;
	int        sel  = s_p15_sel;

	if (!s_p15_ok || s_cursorX < 0)
		return;

	for (i = 0; i < P15_GLYPHS; i++) {
		pts[i][0] = s_p15_glyph[i].target[0];
		pts[i][1] = s_p15_glyph[i].target[1];
		pts[i][2] = 0.0f;
	}

	/*
	 * Identity rotation with the eye subtracted, then the view - the
	 * same composition model_draw does for an object, with the object
	 * reduced to a point.
	 */
	for (i = 0; i < 9; i++)
		base[i] = 0.0f;
	base[0] = base[4] = base[8] = 1.0f;
	base[9]  = -s_p15_eye[0];
	base[10] = -s_p15_eye[1];
	base[11] = -s_p15_eye[2];
	mat34_multiply(composed, base, s_p15_view);

	/* The last argument is a SHADE intensity, and this is a hit test -
	 * only screenX and screenY are read, so its value is arbitrary. */
	v = transform_verts_flat(composed, (const float *)pts, P15_GLYPHS, 0x1f);
	if (!v)
		return;

	for (i = 0; i < P15_GLYPHS; i++) {
		/*
		 * BACK INTO FRAME SPACE for this one. screenX comes out
		 * of the 3D transform, which knows nothing about the
		 * block of authored art, so the offset menu_set_cursor
		 * took off has to go back on. Every OTHER hit test on
		 * this page compares against a sprite rectangle and
		 * wants the cursor exactly as it is.
		 */
		int dx = (s_cursorX + g_fb2dOriginX) - v[i].screenX;
		int dy = s_cursorY - v[i].screenY;
		int d2 = dx * dx + dy * dy;

		if (d2 < best) {
			best = d2;
			sel  = i;
		}
	}
	if (sel != s_p15_sel) {
		s_p15_sel = sel;
		audio_cue(SND_MENU_HOVER, 0);
	}
}

/* ORIGIN  MenuPage15_NameEntry_Enter @ 0x00487070 */
static void page15_enter(void)
{
	char name3df[64];
	char nameMap[64];
	int  i;
	int  row;
	int  col;

	screen3d_set_proj_base_x(P15_PROJ_BASE);
	screen3d_set_proj_base_y(P15_PROJ_BASE);

	/*
	 * *** THE ONCE-GUARD IS WIDER HERE THAN IN THE ORIGINAL, AND
	 * DELIBERATELY SO. ***
	 *
	 * Enter's guard at 0x004f1284 closes BEFORE the normalise, scale
	 * and mirror loop - those three run on every entry, not just the
	 * first. That is harmless for the scale (a mesh already fitted to
	 * 62 normalises to a factor of 1.0) but not for the mirror, which
	 * flips every time it is called.
	 *
	 * o AND PAGE 15 RE-ENTERS ITSELF. A matched cheat code calls
	 *   Menu_GotoPage(0xf) - this page - so typing REVERSE would run
	 *   Enter a second time and leave the whole alphabet mirrored
	 *   [inferred: the code path is verified, the visible result is a
	 *   prediction, since rule 8 forbids running the game].
	 *
	 * We put the mesh mutations inside the guard so they happen once.
	 * That is a divergence, and it is the one the project has already
	 * chosen elsewhere: reproduce motion and gameplay, not cosmetic
	 * defects. See the black-glass note in menu-pages.txt 4.3.1.4.
	 */
	if (!s_p15_loaded) {
		float scale = 1.0f;

		for (i = 0; i < P15_GLYPHS; i++) {
			if (i < 26) {
				sprintf(name3df, "MENU\\FONT3D\\L%c.3DF", 'A' + i);
				sprintf(nameMap, "MENU\\FONT3D\\L%c.MAP", 'A' + i);
			} else if (i == P15_BACKSPACE) {
				strcpy(name3df, "MENU\\FONT3D\\BACKSPAC.3DF");
				strcpy(nameMap, "MENU\\FONT3D\\BACKSPAC.MAP");
			} else {
				strcpy(name3df, "MENU\\FONT3D\\VALID.3DF");
				strcpy(nameMap, "MENU\\FONT3D\\VALID.MAP");
			}
			if (!p15_load_glyph(i, name3df, nameMap))
				continue;       /* the others still draw */

			/*
			 * ONE GLYPH SETS THE SCALE AND THE REST INHERIT IT.
			 * Enter normalises the first to a 62.0 half-extent, keeps
			 * the returned scale, and applies THAT to the other 27
			 * with MenuTools_ScaleMeshBy [verified]. Normalising each
			 * one independently would blow every glyph up to the same
			 * box and lose the relative widths - an 'i' as wide as a
			 * 'w'.
			 */
			if (i == 0)
				scale = scene_normalise_mesh_to_size(&s_p15_glyph[0],
			                                         P15_GLYPH_HALF);
			else
				scene_scale_mesh_by(&s_p15_glyph[i], scale);
		}

		/*
		 * NO SECOND MIRROR ON EITHER OF THESE - NOT EVERYTHING THE
		 * PAGE LOADS IS TREATED ALIKE, and p15_load_glyph says why.
		 */
		s_p15_cadre_ok = scene_object_load(&s_p15_cadre,
		                                   "MENU\\FONT3D\\CADRE.3DF",
		                                   "MENU\\FONT3D\\CADRE.MAP");
		if (s_p15_cadre_ok) {
			scene_normalise_mesh_to_size(&s_p15_cadre, P15_CADRE_HALF);
			s_p15_cadre_ok = p15_build(&s_p15_cadre, &s_p15_cadre_inst,
			                           &s_p15_cadre_prims, NULL,
			                           "cadre");
		} else {
			mrlog_err("page15: cadre did not load\n");
		}

		s_p15_curseur_ok = scene_object_load(&s_p15_curseur,
		                                     "MENU\\FONT3D\\CURSEUR.3DF",
		                                     "MENU\\FONT3D\\CURSEUR.MAP");
		if (s_p15_curseur_ok) {
			/*
			 * THE CURSOR TAKES THE GLYPHS SCALE, from the extra
			 * ScaleMeshBy immediately after the 28-glyph loop
			 * [verified, 0x00487620..0x0048762c: the saved scale is
			 * reloaded and applied with ECX = curseur].
			 *
			 * o WITHOUT IT THE STAR IS 96x TOO BIG - the raw
			 *   Load3dfAndMap scale - and sits so far across the
			 *   frustum that it covers most of the screen while it
			 *   spins. It is the one object on this page that is
			 *   neither normalised to a size of its own nor
			 *   mirrored, so it is easy to miss in both directions.
			 */
			scene_scale_mesh_by(&s_p15_curseur, scale);
			s_p15_curseur_ok = p15_build(&s_p15_curseur,
			                             &s_p15_curseur_inst,
			                             &s_p15_curseur_prims, NULL,
			                             "curseur");
		} else {
			mrlog_err("page15: curseur did not load\n");
		}

		s_p15_loaded = 1;
	}

	/*
	 * Menu_ClearButtons(g_menu) here in the original, and there is
	 * nothing for us to clear: menu_set_page empties the list on
	 * every page change, which is the one funnel they all go
	 * through. Page 15 registers no buttons in any case - it is not
	 * in menu_page_uses_buttons, and its selection is p15_hit_test's
	 * proximity search.
	 */

	/* The grid, 4 rows of 7, row 0 at the top. */
	i = 0;
	for (row = 0; row < P15_GRID_ROWS; row++) {
		for (col = 0; col < P15_GRID_COLS; col++) {
			s_p15_grid[i][0] = (float)col * P15_GRID_X_STEP *
			                   P15_GRID_X_SPAN - P15_GRID_X_BIAS;
			s_p15_grid[i][1] = (float)(3 - row) * P15_GRID_Y_STEP *
			                   P15_GRID_Y_SPAN - P15_GRID_Y_BIAS;
			s_p15_grid[i][2] = 0.0f;
			i++;
		}
	}

	/*
	 * SCATTER, THEN FLY IN. Both channels are started together with a
	 * 2 second sine ease: rotation to zero and position to the grid
	 * slot. This is the effect that reads as the reverse of the glass
	 * shatter.
	 *
	 * o ONE RANDOM DRAW FEEDS BOTH CHANNELS, and that is the original,
	 *   not a shortcut. r1 places the glyph in x AND spins it about
	 *   x, so the one that starts furthest left is also the one turned
	 *   hardest. Six independent draws would look subtly wrong.
	 */
	for (i = 0; i < P15_GLYPHS; i++) {
		float r1 = p15_rand_signed();
		float r2 = p15_rand_signed();
		float r3 = p15_rand_signed();
		float pos[3];
		float rot[3];
		float zero[3] = { 0.0f, 0.0f, 0.0f };

		if (!s_p15_glyph_ok[i])
			continue;

		pos[0] = r1 * P15_SCATTER_X;
		pos[1] = r2 * P15_SCATTER_Y + P15_SCATTER_Y0;
		pos[2] = r3 * P15_SCATTER_Z + P15_SCATTER_Z0;

		rot[0] = r1 * P15_SPIN_TURNS * P15_TWO_PI;
		rot[1] = r2 * P15_SPIN_TURNS * P15_TWO_PI;
		rot[2] = r3 * P15_SPIN_TURNS * P15_TWO_PI;

		scene_set_vec_pair(&s_p15_glyph[i], rot, pos);
		scene_start_vec_move(&s_p15_glyph[i], zero, P15_FLY_SECONDS,
		                     SCENE_MODE_EASE);
		scene_start_target_move(&s_p15_glyph[i], s_p15_grid[i],
		                        P15_FLY_SECONDS, SCENE_MODE_EASE);
	}

	/*
	 * The two sprites. load_res goes through the manifest, so these
	 * are ids and not filenames - menures.c turns 2 into paralax3.lez
	 * and 105 into enternam.lez, with the localised variant picked
	 * for 105 because its manifest entry says so.
	 *
	 * Enter positions 105 at (0,0) [verified, Sprite_SetPosition at
	 * the tail of Enter]. The backdrop takes no position at all - its
	 * draw covers the frame.
	 */
	s_p15_backdrop_ok = load_res(&s_p15_backdrop, P15_RES_BACKDROP,
	                             0, 0, SPRITE_ANCHOR_NONE, "page15");
	s_p15_banners_ok  = load_res(&s_p15_banners, P15_RES_BANNERS,
	                             0, 0, SPRITE_ANCHOR_NONE, "page15");

	/*
	 * The three drivers. They are plain animation state - no mesh -
	 * so they are zeroed and driven directly rather than loaded.
	 */
	{
		float zero[3] = { 0.0f, 0.0f, 0.0f };
		float waveRate[3] = { P15_SPIN_RATE, 0.0f, 0.0f };
		float spinRate[3] = { 0.0f, P15_SPIN_RATE, 0.0f };

		memset(&s_p15_wavePhase, 0, sizeof(s_p15_wavePhase));
		memset(&s_p15_waveAmp,   0, sizeof(s_p15_waveAmp));
		memset(&s_p15_spin,      0, sizeof(s_p15_spin));

		scene_set_vec_pair(&s_p15_wavePhase, zero, zero);
		scene_start_vec_move(&s_p15_wavePhase, waveRate, 1.0f,
		                     SCENE_MODE_SPIN);

		scene_set_vec_pair(&s_p15_spin, zero, zero);
		scene_start_vec_move(&s_p15_spin, spinRate, 1.0f,
		                     SCENE_MODE_SPIN);

		/* Amplitude starts at zero and is ramped by the tick. */
		scene_set_vec_pair(&s_p15_waveAmp, zero, zero);
	}
	/*
	 * The nine name slots, and the cursor parked off the top until
	 * the letters land.
	 */
	for (i = 0; i < P15_NAME_SLOTS; i++) {
		s_p15_slotPos[i][0] = (float)i * P15_NAME_STEP - P15_NAME_BIAS;
		s_p15_slotPos[i][1] = P15_NAME_Y;
		s_p15_slotPos[i][2] = P15_NAME_Z;
	}

	{
		float spinRate[3] = { 0.0f, P15_SPIN_RATE, 0.0f };
		float park[3]     = { 0.0f, P15_CURSEUR_PARK_Y,
			                  P15_CURSEUR_PARK_Z };
		float zero[3]     = { 0.0f, 0.0f, 0.0f };

		/*
		 * BOTH SPIN FOREVER, mode 2, a full turn a second - the same
		 * rate as the selection highlight. The frame reads as
		 * "rotating around the letter" because it is a flat ring
		 * spinning about Y while sitting on the letter's position.
		 */
		if (s_p15_cadre_ok)
			scene_start_vec_move(&s_p15_cadre, spinRate, 1.0f,
		                         SCENE_MODE_SPIN);
		if (s_p15_curseur_ok) {
			scene_set_vec_pair(&s_p15_curseur, zero, park);
			scene_start_vec_move(&s_p15_curseur, spinRate, 1.0f,
			                     SCENE_MODE_SPIN);
		}
	}

	for (i = 0; i < P15_NAME_SLOTS; i++) {
		memset(&s_p15_slotObj[i], 0, sizeof(s_p15_slotObj[i]));
		s_p15_slotGlyph[i] = -1;
	}
	memset(s_p15_name, 0, sizeof(s_p15_name));

	/*
	 * PRE-FILL FROM THE SAVED NAME. The original reads g_playerName,
	 * folds each character to upper case with & 0xdf and replaces
	 * anything STILL outside A-Z with A, clamping to eight
	 * [verified]. The fold only uppercases; it is the replacement
	 * that keeps a space or a digit out, and it has to, because
	 * there is no glyph for either.
	 *
	 * The slots are PARKED, not placed: they sit at (0, 800, 500)
	 * unrotated, and the tick flies them in once the keyboard has
	 * landed. So a saved name tumbles onto the bottom line AFTER the
	 * grid, which is the order the effect reads in.
	 */
	if (!get_ini_string(P15_INI_SECTION, P15_INI_KEY, g_playerName,
	                    (int)sizeof(g_playerName)))
	    g_playerName[0] = 0;
	{
		float park[3] = { 0.0f, P15_CURSEUR_PARK_Y, P15_CURSEUR_PARK_Z };
		float zero[3] = { 0.0f, 0.0f, 0.0f };
		int   n = 0;

		while (n < MENU_NAME_MAX && g_playerName[n]) {
			int ch = g_playerName[n] & 0xdf;
			int idx;

			if (ch < 0x41 || ch > 0x5a)
				ch = 0x41;
			idx = ch - 0x41;
			s_p15_name[n] = (char)ch;
			if (s_p15_glyph_ok[idx]) {
				s_p15_slotGlyph[n]    = idx;
				s_p15_slotObj[n].mesh = s_p15_glyph[idx].mesh;
				model_init(&s_p15_slotInst[n], kP15Target, kP15Target,
				           s_p15_glyph[idx].mesh);
				model_set_prims(&s_p15_slotInst[n], s_p15_prims[idx],
				                s_p15_nprims[idx]);
				scene_set_vec_pair(&s_p15_slotObj[n], zero, park);
			}
			n++;
		}
		s_p15_name[n] = 0;
		s_p15_nameLen = n;
	}

	s_p15_flyingIn = 1;
	s_p15_done     = 0;

	/*
	 * A NON-EMPTY NAME STARTS ON THE VALID CELL, so pressing
	 * enter straight away accepts what was saved. The original
	 * sets the selection to 0x1b when the pre-filled length is
	 * non-zero.
	 */
	s_p15_sel = s_p15_nameLen > 0 ? P15_SEL_VALID : 0;

	/* s_p15_ok means ONLY that the view matrix built. A glyph that
	 * failed to load is skipped individually, not page-wide. */
	s_p15_ok = camera_look_at(s_p15_view, kP15CamPos, kP15Target);
	if (!s_p15_ok)
		mrlog_err("page15: camera_look_at failed\n");
	for (i = 0; i < 3; i++)
		s_p15_eye[i] = kP15CamPos[i];

	/*
	 * ORIGIN  MenuPage15_NameEntry_Enter @ 0x00487070, at
	 *         0x00487a32: Snd_QueueDelayedCues(1, 600, 0x36).
	 *
	 * 0x36 is entrname, "enter your name", queued 600 ms out -
	 * the same delay pages 3, 5 and 6 use, which is about when
	 * the slide-in finishes.
	 *
	 * ITS ID IS THE ODD ONE OUT BUT ITS GROUP IS NOT. The other
	 * ten menu voices run 0x52..0x5b and this sits down at 0x36,
	 * which still falls inside the 0x2f..0x5c speech range - so
	 * it is gated by the voices option like the rest of them,
	 * and loads from Voices\Menu\ like the rest of them. It is
	 * merely not contiguous. See audio-cues.txt 1.1 and 5.2.
	 */
	audio_cue_delayed(SND_VOICE_ENTRNAME, 600);

	{
		int loaded = 0;

		for (i = 0; i < P15_GLYPHS; i++)
			loaded += s_p15_glyph_ok[i] ? 1 : 0;
		mrlog("page15: %d/%d glyphs, cadre %d, curseur %d, "
		      "projBase %.0f\n", loaded, P15_GLYPHS, s_p15_cadre_ok,
		      s_p15_curseur_ok, (double)P15_PROJ_BASE);
	}
}

/*
 * ORIGIN  MenuPage15_NameEntry_Tick @ 0x00487bb0, the draw half
 *
 * Inside the scene bracket - see menu_draw_page_3d. Four sets go
 * into one queue: the 28 glyphs, the cadre on the selected one, the
 * cursor, and whichever name slots hold a letter.
 *
 * o PER-GLYPH, NOT ALL-OR-NOTHING - see the note in the loop.
 *
 * o THE CADRE IS GATED ON THE SELECTED GLYPH HAVING LOADED, not on
 *   the cadre alone. A ring drawn round a letter that is not there
 *   marks an empty cell.
 */
static void page15_draw_3d(void)
{
	int i;
	int drawn = 0;

	if (!s_p15_ok)
		return;                 /* the view matrix is unusable */

	queue_reset();
	vertexpool_reset();

	/*
	 * PER-GLYPH, NOT ALL-OR-NOTHING. Returning early on one failed
	 * load blanks the whole page, which looks identical to the
	 * pipeline being wrong. Draw whatever loaded; the log says what
	 * did not.
	 */
	for (i = 0; i < P15_GLYPHS; i++) {
		if (!s_p15_glyph_ok[i])
			continue;
		model_draw(&s_p15_inst[i], s_p15_view, s_p15_eye);
		drawn++;
	}
	if (s_p15_cadre_ok && s_p15_glyph_ok[s_p15_sel]) {
		model_draw(&s_p15_cadre_inst, s_p15_view, s_p15_eye);
		drawn++;
	}
	if (s_p15_curseur_ok) {
		model_draw(&s_p15_curseur_inst, s_p15_view, s_p15_eye);
		drawn++;
	}
	for (i = 0; i < P15_NAME_SLOTS; i++) {
		if (s_p15_slotGlyph[i] < 0)
			continue;
		model_draw(&s_p15_slotInst[i], s_p15_view, s_p15_eye);
		drawn++;
	}
	if (drawn)
		queue_walk(drawscene_dispatch);
}

/* ORIGIN  MenuPage15_NameEntry_Tick @ 0x00487bb0 */
static void page15_tick(void)
{
	int i;
	int k;

	/*
	 * THE TICK ADVANCES, THE DRAW DRAWS, and they are separate
	 * because menu_tick runs inside menu_draw, BEFORE r_begin_scene
	 * - so A QUEUE WALKED OUTSIDE THE SCENE BRACKET IS SILENTLY
	 * THROWN AWAY and the page goes black. The 3D belongs in
	 * menu_draw_page_3d, which main.cpp calls between begin and end;
	 * its own comment says the same.
	 *
	 * MenuTools_UpdateMoves is called from the original's tick too,
	 * so this half is a transcription and not just a workaround.
	 *
	 * THE 2D LAYER FIRST. The backdrop is opaque and covers the
	 * whole frame, so it stands in for the clear; the banners go
	 * over it. Both happen before the 3D.
	 */
	if (s_p15_backdrop_ok) {
		sprite_update_wave(&s_p15_backdrop, 0, 1);
		sprite_draw_wave(&s_p15_backdrop);
	}
	if (s_p15_banners_ok) {
		sprite_update_move(&s_p15_banners);
		sprite_draw(&s_p15_banners, SPRITE_MODE_USE_KIND, 100, 0);
	}

	/* ONLY WHEN THE POINTER HAS MOVED, or it fights the keyboard
	 * for the selection every single frame. */
	if (s_cursorX != s_p15_lastCurX || s_cursorY != s_p15_lastCurY) {
		s_p15_lastCurX = s_cursorX;
		s_p15_lastCurY = s_cursorY;
		p15_hit_test();
	}

	scene_update(&s_p15_wavePhase);
	scene_update(&s_p15_waveAmp);
	scene_update(&s_p15_spin);
	if (s_p15_cadre_ok)
		scene_update(&s_p15_cadre);
	if (s_p15_curseur_ok)
		scene_update(&s_p15_curseur);
	for (i = 0; i < P15_NAME_SLOTS; i++) {
		if (s_p15_slotGlyph[i] < 0)
			continue;
		scene_update(&s_p15_slotObj[i]);
		for (k = 0; k < 3; k++) {
			s_p15_slotInst[i].rotation[k] = s_p15_slotObj[i].vec[k];
			s_p15_slotInst[i].position[k] = s_p15_slotObj[i].target[k];
		}
	}

	for (i = 0; i < P15_GLYPHS; i++)
		if (s_p15_glyph_ok[i])
			scene_update(&s_p15_glyph[i]);

	if (s_p15_flyingIn) {
		/*
		 * THE HANDOFF. The original watches the FIRST object's vec
		 * channel - "if (obj[0].vecActive == 0)" - and takes that one
		 * glyph as the signal for all 28. They all started together
		 * with the same duration, so it is sound, and it is cheaper
		 * than polling every object.
		 */
		if (!s_p15_glyph[0].vecActive) {
			float amp[3] = { P15_WAVE_AMPLITUDE, 0.0f, 0.0f };

			scene_start_target_move(&s_p15_waveAmp, amp,
			                        P15_WAVE_RAMP_SEC,
			                        SCENE_MODE_LINEAR);

			/*
			 * AND THE CURSOR COMES IN WITH IT, onto the slot the
			 * next letter would go in - but ONLY below eight.
			 *
			 * THE ORIGINAL GUARDS THE TWO CURSOR MOVES DIFFERENTLY,
			 * and it is not an oversight to tidy up:
			 *
			 *   HERE, the fly-in, CMP 8 / JGE at 0x00487d65 - so a
			 *   full SAVED name leaves the cursor parked off the top.
			 *
			 *   TYPING, FUN_00487a70, moves it with no guard at all,
			 *   after the length has been incremented - so typing an
			 *   eighth letter DOES put the cursor on the ninth slot.
			 *
			 * That is why there are nine slots and why p15_type_letter
			 * tests against P15_NAME_SLOTS while this tests 8. Ours
			 * keeps a bound the original does without. [verified]
			 *
			 * ANY SAVED NAME FLIES IN TOO, each slot easing from the
			 * park position onto the line - the same one-second
			 * ease a freshly typed letter gets.
			 */
			for (k = 0; k < s_p15_nameLen; k++) {
				float zero[3] = { 0.0f, 0.0f, 0.0f };

				if (s_p15_slotGlyph[k] < 0)
					continue;
				scene_start_target_move(&s_p15_slotObj[k], s_p15_slotPos[k],
				                        P15_TYPE_SLOT_SEC, SCENE_MODE_EASE);
				scene_start_vec_move(&s_p15_slotObj[k], zero,
				                     P15_TYPE_SLOT_SEC, SCENE_MODE_EASE);
			}

			if (s_p15_curseur_ok && s_p15_nameLen < 8)
				scene_start_target_move(&s_p15_curseur,
			                            s_p15_slotPos[s_p15_nameLen],
			                            P15_CURSEUR_IN_SEC,
			                            SCENE_MODE_EASE);
			s_p15_flyingIn = 0;
		}
	} else {
		/*
		 * THE RIPPLE. Phase advances by a column step within a row
		 * and a row step between rows, so the crest travels
		 * diagonally across the keyboard rather than in a straight
		 * line - pi/6 across and 2*pi/14 down do not divide evenly
		 * into each other.
		 */
		int   row;
		int   col;
		float rowPhase = s_p15_wavePhase.vec[0];
		float amp      = s_p15_waveAmp.target[0];

		i = 0;
		for (row = 0; row < P15_GRID_ROWS; row++) {
			float ph = rowPhase;

			for (col = 0; col < P15_GRID_COLS; col++) {
				s_p15_grid[i][2] = (float)cos(ph) * amp;
				if (s_p15_glyph_ok[i])
					s_p15_glyph[i].target[2] = s_p15_grid[i][2];
				ph += P15_WAVE_COL_STEP;
				i++;
			}
			rowPhase += P15_WAVE_ROW_STEP;
		}
	}

	/*
	 * THE SELECTED GLYPH SPINS AND THE REST DO NOT. The original
	 * copies one component - the highlight spinner's vec[1] - into
	 * the selected object and zeroes all three on every other, every
	 * frame. So "selected" is not a stored state on the glyph; it is
	 * recomputed from the index each frame.
	 */
	for (i = 0; i < P15_GLYPHS; i++) {
		if (!s_p15_glyph_ok[i])
			continue;
		if (i == s_p15_sel) {
			/*
			 * ONLY vec[1]. The original writes the spinner into the
			 * Y component alone and leaves X and Z as the animation
			 * left them - so a selected letter that is still flying
			 * in keeps tumbling on the other two axes.
			 */
			s_p15_glyph[i].vec[1] = s_p15_spin.vec[1];
		} else if (!s_p15_flyingIn) {
			/*
			 * *** PINNED ONLY ONCE LANDED. ***
			 * The zeroing lives in the WAVE branch of the original
			 * tick and not in the flying-in branch, where the
			 * unselected case does nothing but advance the moves.
			 *
			 * ZERO IN BOTH AND THE ROTATION CHANNEL IS OVERWRITTEN
			 * EVERY FRAME: the letters slide in flat instead of
			 * tumbling, while the scatter still sets a random
			 * rotation and the ease still runs. The work is thrown
			 * away before it can be drawn, so the fly-in looks
			 * unimplemented rather than broken.
			 */
			s_p15_glyph[i].vec[0] = 0.0f;
			s_p15_glyph[i].vec[1] = 0.0f;
			s_p15_glyph[i].vec[2] = 0.0f;
		}
		for (k = 0; k < 3; k++) {
			s_p15_inst[i].rotation[k] = s_p15_glyph[i].vec[k];
			s_p15_inst[i].position[k] = s_p15_glyph[i].target[k];
		}
	}

	/*
	 * THE EXIT. The tick's tail, transcribed [verified]:
	 *
	 *     strcpy(g_playerName, typed);
	 *     for each of the three codes:
	 *         if (stricmp(code, g_playerName) == 0) {
	 *             matched = 1; *code.target = code.value;
	 *         }
	 *     Screen3D_SetProjBaseX/Y(1911.0);
	 *     Menu_ClearActivated();
	 *     if (matched) { Menu_GotoPage(15); return; }
	 *     Menu_BeginTransition();
	 *     Menu_GotoPage(3);
	 *
	 * o A MATCHED CODE GOES BACK TO THIS PAGE AND SKIPS THE
	 *   TRANSITION, so the glass does not shatter for a cheat - the
	 *   name box just empties and you are still here. PUSH 0xf then
	 *   an immediate RET at 0x004886ac; the other arm calls
	 *   Menu_BeginTransition at 0x004886bf first [verified]. No
	 *   other self-transition has been found, but Menu_GotoPage has
	 *   about eighty call sites and they have not all been read.
	 *
	 * o AND THE PROJECTION BASE IS LEFT AT 1911, not put back to the
	 *   480 default or to the 1024 this page set. Page 3 inherits it.
	 */
	if (s_p15_done) {
		static const struct {
			const char *text;
			int        *target;
		} kCheats[] = {
			/*
			 * THE STORED STRING IS WHAT THE PLAYER TYPES. These are
			 * words spelled backwards and ENTERED backwards - there
			 * is no reversal step, the compare is a plain stricmp
			 * against the name exactly as typed. Typing REVERSE does
			 * nothing; CESREVER is the code.
			 */
			{ "CESREVER", &g_cheatReverse },
			{ "CTEKCOP",  &g_cheatPocket  },
			{ "CDNALSI",  &g_cheatIsland  }
		};
		int matched = 0;
		int c;

		s_p15_done = 0;
		strncpy(g_playerName, s_p15_name, MENU_NAME_MAX);
		g_playerName[MENU_NAME_MAX] = 0;

		for (c = 0; c < (int)(sizeof(kCheats) / sizeof(kCheats[0])); c++) {
			if (_stricmp(kCheats[c].text, g_playerName) == 0) {
				matched = 1;
				*kCheats[c].target = 1;
				mrlog("page15: cheat %s accepted\n", kCheats[c].text);
			}
		}

		/*
		 * TWO OF THE ORIGINAL'S TAIL CALLS ARE NOT HERE.
		 *
		 * The PROJECTION change is deferred to page15_leave and has
		 * to be - see the note there.
		 *
		 * Menu_ClearActivated is not needed: menu_set_page clears
		 * the activation on every page change, the same funnel that
		 * makes page15_enter's missing Menu_ClearButtons harmless.
		 */

		if (matched) {
			/*
			 * A CODE IS CONSUMED, NOT KEPT, and the page restarts with
			 * no on-screen acknowledgement whatsoever - the letters
			 * simply re-scatter. That is the original: no shatter, no
			 * message, straight back to an empty box.
			 *
			 * o SO A PLAYER WHO GENUINELY WANTED THAT NAME IS STUCK, in
			 *   a loop with no explanation, exactly as they are on an
			 *   empty name. Not reproduced as a bug and not fixed as
			 *   one - it is what the page does, and inventing an
			 *   indicator would be inventing retail behaviour.
			 *
			 * o AND THE NAME IS NOT SAVED. The write belongs BELOW the
			 *   branch: above it, entering a code persists CESREVER as
			 *   the player name and the box pre-fills with it next
			 *   launch. The original sets only its in-memory copy here
			 *   and never touches storage.
			 */
			menu_goto_page(MENU_PAGE_NAME);
		} else {
			write_ini_string(P15_INI_SECTION, P15_INI_KEY, g_playerName);
			mrlog("page15: name \"%s\", leaving for page %d\n",
			      g_playerName, MENU_PAGE_MAIN);
			menu_begin_transition(MENU_PAGE_MAIN);
		}
		return;
	}

	/*
	 * THE FRAME SITS ON THE SELECTED LETTER, every frame, taken from
	 * that letter's CURRENT position rather than from the grid - so
	 * it rides the wave with the letter instead of hovering at the
	 * rest height. The original does this with
	 * MenuTools_SetTarget(letters[sel] + 0xc), which is the letter's
	 * target vec3 read straight out of its scene object.
	 */
	if (s_p15_cadre_ok && s_p15_glyph_ok[s_p15_sel]) {
		scene_set_target(&s_p15_cadre, s_p15_glyph[s_p15_sel].target);
		for (k = 0; k < 3; k++) {
			s_p15_cadre_inst.rotation[k] = s_p15_cadre.vec[k];
			s_p15_cadre_inst.position[k] = s_p15_cadre.target[k];
		}
	}
	if (s_p15_curseur_ok) {
		for (k = 0; k < 3; k++) {
			s_p15_curseur_inst.rotation[k] = s_p15_curseur.vec[k];
			s_p15_curseur_inst.position[k] = s_p15_curseur.target[k];
		}
	}
}

/* ORIGIN  MenuPage15_NameEntry_Leave @ 0x00488790 */
static void page15_leave(void)
{
	/*
	 * *** THE PROJECTION BASE CHANGES HERE, NOT IN THE TICK. ***
	 *
	 * The original sets it to 1911 in the tick, immediately before
	 * Menu_BeginTransition - and that is safe THERE because the exit
	 * block is the last thing in the tick, after Screen3D_EndFrame
	 * and the final Screen3D_DrawScene. Every draw for that frame has
	 * already happened, so the new base only reaches the next page.
	 *
	 * o OUR TICK RUNS BEFORE THE 3D DRAW. menu_tick is called from
	 *   menu_draw; menu_draw_page_3d runs later, inside the scene
	 *   bracket. SET THE BASE IN THE TICK AND IT APPLIES TO THIS
	 *   FRAME: the glass captures a screen with the letters drawn
	 *   1911/1024 = 1.87x too large, and the shatter then carries a
	 *   zoomed image matching nothing that was ever on screen.
	 *
	 * Leave runs after glass_capture_if_pending - main.cpp captures,
	 * then applies the pending page, which is what calls this - so
	 * the change lands in the same place the original puts it,
	 * relative to the capture rather than relative to the tick.
	 *
	 * o AND PAGE 25 IS THE CONTROL. It changes no projection on the
	 *   way out and its capture is 1:1, so a capture that is wrong
	 *   only on page 15 is page 15's doing and not the glass's.
	 */
	screen3d_set_proj_base_x(P15_PROJ_EXIT);
	screen3d_set_proj_base_y(P15_PROJ_EXIT);
	if (s_p15_backdrop_ok) sprite_free(&s_p15_backdrop);
	if (s_p15_banners_ok)  sprite_free(&s_p15_banners);
	s_p15_backdrop_ok = 0;
	s_p15_banners_ok  = 0;

	/*
	 * THE MESHES ARE NOT FREED, matching the once-flag: Enter loads
	 * them behind a guard and nothing in the original releases them
	 * on the way out. Freeing here would work exactly once and then
	 * leave the flag lying, which is worse than leaking a fixed 30
	 * meshes for the life of the process.
	 */
}

/*
 * ORIGIN  MenuPage15_NameEntry_Tick @ 0x00487bb0, at 0x00488290
 *
 * THE ARROW KEYS, transcribed from the switch at 0x00488290. The wrap
 * is not a modulo: left off the start goes to 27 and right off the end
 * goes to 0, but up and down move by SEVEN and wrap by TWENTY-ONE, so
 * they step a column rather than folding the index. 0x15 is 21, which
 * is three rows.
 */
static int page15_on_key(int vkey)
{
	int sel = s_p15_sel;

	switch (vkey) {
	case VK_LEFT:
		sel = s_p15_sel - 1;
		if (sel < 0)
			sel = P15_SEL_VALID;
		break;
	case VK_RIGHT:
		sel = s_p15_sel + 1;
		if (sel > P15_SEL_VALID)
			sel = 0;
		break;
	case VK_UP:
		sel = s_p15_sel - P15_GRID_COLS;
		if (sel < 0)
			sel = s_p15_sel + 21;
		break;
	case VK_DOWN:
		sel = s_p15_sel + P15_GRID_COLS;
		if (sel > P15_SEL_VALID)
			sel = s_p15_sel - 21;
		break;
	case VK_BACK:
		/*
		 * Backspace deletes, and it does NOT need the selection to be
		 * on the backspace cell - it acts directly. The cell is where
		 * the selection ENDS UP, not a precondition.
		 */
		p15_backspace();
		audio_cue(SND_MENU_HOVER, 0);
		return 1;
	case VK_RETURN:
		/*
		 * ENTER FINISHES, when there is something to finish. The
		 * original also accepts SPACE for "activate whatever is
		 * selected" - type the letter, or backspace, or finish - but
		 * space is the debug pause key here, so that arm is left out
		 * rather than fighting it. THE MOUSE ALREADY PROVIDES IT -
		 * menu_on_click routes MENU_PAGE_NAME through the same
		 * three-way branch, with the selection set by proximity
		 * first, so nothing is lost by leaving the space arm out.
		 */
		if (s_p15_nameLen > 0)
			s_p15_done = 1;
		return 1;
	default:
		/*
		 * A LETTER KEY TYPES IT, folding case the way the original
		 * does with (key | 0x20) - 0x61. Typing moves the selection
		 * onto the letter as a side effect - that happens inside
		 * p15_type_letter, because it is the original's ordering.
		 */
		if ((vkey >= 'A' && vkey <= 'Z') || (vkey >= 'a' && vkey <= 'z')) {
			p15_type_letter((vkey | 0x20) - 'a');
			audio_cue(SND_MENU_HOVER, 0);
			return 1;
		}
		return 0;
	}

	s_p15_sel = sel;
	audio_cue(SND_MENU_HOVER, 0);
	return 1;
}


/* ================================================================ */
/* Page 3 - the main menu                                           */
/* ================================================================ */

/*
 * The first page with a real button list rather than one sprite
 * hit-tested by hand.
 *
 * SEVEN RESOURCES, resolved through the manifest:
 *
 *     3   maintit.lez   the title, slides down from above
 *     4/5 SOLO_D/_OK    a hover PAIR, slides in from the left
 *     6/7 LINK_D/_OK    a hover PAIR, slides in from the right
 *     75  options.lez   single sprite, second-stage slide up
 *     73  exit.lez      single sprite, bound to ESCAPE
 *
 * plus poc_D/poc_L and rev_D/rev_L, the two unlock indicators, which
 * only appear when their feature is unlocked. They are inert - see
 * the [open] beside THE INDICATORS in page3_enter.
 */
#define P3_RES_TITLE     3
#define P3_RES_SOLO_D    4
#define P3_RES_SOLO_OK   5
#define P3_RES_LINK_D    6
#define P3_RES_LINK_OK   7
#define P3_RES_EXIT     73
#define P3_RES_OPTIONS  75
#define P3_RES_BACKDROP  0      /* paralax1.lez, kind 4 */

/*
 * THE TWO UNLOCK INDICATORS. Each is a clickable label with a hover
 * pair, plus a shared ON/OFF word pair beside it:
 *
 *     88/89  poc_L / poc_D     the pocket bike label
 *     90/91  rev_L / rev_D     the reverse mode label
 *     92/93  rev_on_D / rev_on_L    the word ON, dark and lit
 *     94/95  rev_offD / rev_offL    the word OFF, dark and lit
 *
 * o THE ON/OFF WORDS ARE SHARED BY BOTH ROWS, one set of four sprite
 *   objects repositioned and redrawn twice per frame. That is why
 *   they are all named rev_* even though the pocket row uses them
 *   [guess: reverse was the first feature to need a toggle and the
 *   art was never renamed].
 *
 * o AND BOTH WORDS ARE ALWAYS DRAWN. The toggle decides which is LIT
 *   and which is DARK, not which is present: off draws ON dark and
 *   OFF lit, on draws the other way round. It reads as a two-state
 *   selector rather than a checkbox.
 */
#define P3_RES_POC_L    88
#define P3_RES_POC_D    89
#define P3_RES_REV_L    90
#define P3_RES_REV_D    91
#define P3_RES_ON_D     92
#define P3_RES_ON_L     93
#define P3_RES_OFF_D    94
#define P3_RES_OFF_L    95

/*
 * EACH LABEL MOVES ASIDE FOR THE OTHER. The x depends on whether the
 * OTHER feature is showing, so one indicator centres itself and two
 * split the space [verified, the pairs of literals in Enter].
 */
#define P3_POC_X_ALONE   227
#define P3_POC_X_SHARED  352
#define P3_POC_Y         288
#define P3_REV_X_ALONE   202
#define P3_REV_X_SHARED   72
#define P3_REV_Y         293

/* And the word pair, ON then OFF 80 apart. */
#define P3_WORD_GAP       80
#define P3_POC_WORD_X_ALONE  248
#define P3_POC_WORD_X_SHARED 373
#define P3_POC_WORD_Y        362
#define P3_REV_WORD_X_ALONE  250
#define P3_REV_WORD_X_SHARED 120
#define P3_REV_WORD_Y        363

/*
 * THE TWO-STAGE ENTRANCE, and the trigger for the second stage is not
 * a timer [verified]:
 *
 *     if (SOLO_D.moveActive == 0 && !stage2) { start options and
 *                                              exit moving; stage2 = 1 }
 *
 * So the bottom row waits for the TOP row to land. Chaining on the
 * move flag rather than on elapsed time means the two stages cannot
 * drift apart if a frame is slow.
 */
#define P3_SLIDE_SEC   1.0f     /* 0x3f800000 */
#define P3_SLIDE2_SEC  0.7f     /* 0x3f333333 */

/* Start positions, all off-screen or above. */
#define P3_TITLE_START_Y  (-80)
#define P3_SOLO_START_X  (-140)
#define P3_LINK_START_X    780
#define P3_ROW_Y           116
#define P3_BOTTOM_START_Y  500

/* And where they settle. */
#define P3_TITLE_END_Y       0
#define P3_SOLO_END_X       56
#define P3_LINK_END_X      363
#define P3_ROW_Y_LOCKED    177  /* no unlock icons: the row sits low */
#define P3_OPTIONS_END_X   240
#define P3_OPTIONS_END_Y   420
#define P3_EXIT_END_X      510
#define P3_EXIT_END_Y      430

/* The activated indices, in the order Enter adds them. */
#define P3_BTN_SOLO    0
#define P3_BTN_LINK    1
#define P3_BTN_OPTIONS 2
#define P3_BTN_EXIT    3

/* The two voice lines the tick plays are ids in audio.h - see the
 * menu speech table there. */

static MR_Sprite s_p3_backdrop;
static MR_Sprite s_p3_title;
static MR_Sprite s_p3_soloD;
static MR_Sprite s_p3_soloOk;
static MR_Sprite s_p3_linkD;
static MR_Sprite s_p3_linkOk;
static MR_Sprite s_p3_options;
static MR_Sprite s_p3_exit;
static int       s_p3_ok;
static int       s_p3_stage2;
static MR_Sprite s_p3_pocD, s_p3_pocL, s_p3_revD, s_p3_revL;
static MR_Sprite s_p3_onD,  s_p3_onL,  s_p3_offD, s_p3_offL;
static int       s_p3_pocBtn  = -1;
static int       s_p3_revBtn  = -1;
static int       s_p3_pocShow;
static int       s_p3_revShow;

/* ORIGIN  NONE - ours */
static void p3_move(MR_Sprite *sp, int x, int y, float secs)
{
	int t[2];

	t[0] = x;
	t[1] = y;
	sprite_start_move(sp, t, secs, 1, SPRITE_ANCHOR_NONE);
}

/* ORIGIN  NONE - ours */
static void p3_place(MR_Sprite *sp, int x, int y)
{
	int t[2];

	t[0] = x;
	t[1] = y;
	sprite_set_position(sp, t, SPRITE_ANCHOR_NONE);
}

/*
 * ORIGIN  NONE - ours, all four
 *
 * Which of the shared word sprites this row wants. Off draws ON dark
 * and OFF lit; on draws the reverse.
 */
static MR_Sprite *s_p3_pocketOn(void)
{ return g_pocketEnabled ? &s_p3_onL : &s_p3_onD; }
static MR_Sprite *s_p3_pocketOff(void)
{ return g_pocketEnabled ? &s_p3_offD : &s_p3_offL; }
static MR_Sprite *s_p3_reverseOn(void)
{ return g_reverseEnabled ? &s_p3_onL : &s_p3_onD; }
static MR_Sprite *s_p3_reverseOff(void)
{ return g_reverseEnabled ? &s_p3_offD : &s_p3_offL; }

/* ORIGIN  MenuPage03_MainMenu_Enter @ 0x004422c0 */
static void page3_enter(void)
{
	if (s_netSavedValid) {
		g_pocketEnabled  = s_netSavedPocket;
		g_reverseEnabled = s_netSavedReverse;
		s_netSavedValid  = 0;
		mrlog("page3: pocket %d reverse %d restored after link\n",
		      g_pocketEnabled, g_reverseEnabled);
	}

	s_p3_ok = 1;
	s_p3_ok &= load_res(&s_p3_backdrop, P3_RES_BACKDROP, 0, 0,
	                   SPRITE_ANCHOR_NONE, "page3");
	s_p3_ok &= load_res(&s_p3_title,   P3_RES_TITLE,   0, 0,
	                   SPRITE_ANCHOR_NONE, "page3");
	s_p3_ok &= load_res(&s_p3_soloD,   P3_RES_SOLO_D,  0, 0,
	                   SPRITE_ANCHOR_NONE, "page3");
	s_p3_ok &= load_res(&s_p3_soloOk,  P3_RES_SOLO_OK, 0, 0,
	                   SPRITE_ANCHOR_NONE, "page3");
	s_p3_ok &= load_res(&s_p3_linkD,   P3_RES_LINK_D,  0, 0,
	                   SPRITE_ANCHOR_NONE, "page3");
	s_p3_ok &= load_res(&s_p3_linkOk,  P3_RES_LINK_OK, 0, 0,
	                   SPRITE_ANCHOR_NONE, "page3");
	s_p3_ok &= load_res(&s_p3_options, P3_RES_OPTIONS, 0, 0,
	                   SPRITE_ANCHOR_NONE, "page3");
	s_p3_ok &= load_res(&s_p3_exit,    P3_RES_EXIT,    0, 0,
	                   SPRITE_ANCHOR_NONE, "page3");

	/* Start positions, then the slides. */
	p3_place(&s_p3_title,  0,                P3_TITLE_START_Y);
	p3_place(&s_p3_soloD,  P3_SOLO_START_X,  P3_ROW_Y);
	p3_place(&s_p3_linkD,  P3_LINK_START_X,  P3_ROW_Y);
	p3_place(&s_p3_options, P3_OPTIONS_END_X, P3_BOTTOM_START_Y);
	p3_place(&s_p3_exit,   580,              P3_BOTTOM_START_Y);

	/*
	 * THE ROW SITS LOWER WHEN NOTHING IS UNLOCKED. With none of the
	 * four unlock flags set the SOLO/LINK row lands at y 177; with any
	 * of them set it lands at 116 and the indicator icons take the
	 * space below. One layout rule, driven by save state.
	 */
	{
		int rowY = (g_reverseUnlocked || g_cheatReverse ||
		            g_pocketUnlocked  || g_cheatPocket)
		         ? P3_ROW_Y : P3_ROW_Y_LOCKED;

		p3_move(&s_p3_title, 0,             P3_TITLE_END_Y, P3_SLIDE_SEC);
		p3_move(&s_p3_soloD, P3_SOLO_END_X, rowY,           P3_SLIDE_SEC);
		p3_move(&s_p3_linkD, P3_LINK_END_X, rowY,           P3_SLIDE_SEC);
	}

	/*
	 * THE _OK HALVES ARE NEVER POSITIONED, here or in the original.
	 * Menu_Draw copies spriteA's position onto spriteB every time it
	 * draws a highlight, so a page gives the move to the dark sprite
	 * only and the lit one follows for free. menu_draw_buttons does
	 * the same; the note there has the addresses.
	 *
	 * Placing and tweening both halves is harmless HERE and wrong -
	 * see menu_draw_buttons for what it costs on a page whose pair
	 * is not already coincident.
	 */

	menu_clear_buttons();
	menu_add_sprite_button(&s_p3_soloD,   &s_p3_soloOk, 0);
	menu_add_sprite_button(&s_p3_linkD,   &s_p3_linkOk, 0);
	menu_add_sprite_button(&s_p3_options, NULL,         0);
	menu_add_sprite_button(&s_p3_exit,    NULL,         VK_ESCAPE);

	/*
	 * THE INDICATORS. Shown on unlocked OR cheated - the cheat is
	 * tested alongside the save flag, never written into it, which is
	 * why typing REVERSE reveals the button without earning it.
	 *
	 * [open] CLICKING ONE DOES NOTHING. It should toggle a flag that
	 * swaps the on/off pair the tick draws; nothing reads such a flag
	 * yet, so both indicators are inert.
	 */
	s_p3_revShow = (g_reverseUnlocked || g_cheatReverse);
	s_p3_pocShow = (g_pocketUnlocked  || g_cheatPocket);
	s_p3_pocBtn  = -1;
	s_p3_revBtn  = -1;

	if (s_p3_pocShow || s_p3_revShow) {
		load_res(&s_p3_pocD, P3_RES_POC_D, 0, 0, SPRITE_ANCHOR_NONE, "page3");
		load_res(&s_p3_pocL, P3_RES_POC_L, 0, 0, SPRITE_ANCHOR_NONE, "page3");
		load_res(&s_p3_revD, P3_RES_REV_D, 0, 0, SPRITE_ANCHOR_NONE, "page3");
		load_res(&s_p3_revL, P3_RES_REV_L, 0, 0, SPRITE_ANCHOR_NONE, "page3");
		load_res(&s_p3_onD,  P3_RES_ON_D,  0, 0, SPRITE_ANCHOR_NONE, "page3");
		load_res(&s_p3_onL,  P3_RES_ON_L,  0, 0, SPRITE_ANCHOR_NONE, "page3");
		load_res(&s_p3_offD, P3_RES_OFF_D, 0, 0, SPRITE_ANCHOR_NONE, "page3");
		load_res(&s_p3_offL, P3_RES_OFF_L, 0, 0, SPRITE_ANCHOR_NONE, "page3");
	}

	if (s_p3_pocShow) {
		int x = s_p3_revShow ? P3_POC_X_SHARED : P3_POC_X_ALONE;

		p3_place(&s_p3_pocD, x, P3_POC_Y);
		p3_place(&s_p3_pocL, x, P3_POC_Y);
		s_p3_pocBtn = menu_add_sprite_button(&s_p3_pocD, &s_p3_pocL, 0);
	}
	if (s_p3_revShow) {
		int x = s_p3_pocShow ? P3_REV_X_SHARED : P3_REV_X_ALONE;

		p3_place(&s_p3_revD, x, P3_REV_Y);
		p3_place(&s_p3_revL, x, P3_REV_Y);
		s_p3_revBtn = menu_add_sprite_button(&s_p3_revD, &s_p3_revL, 0);
	}

	s_p3_stage2 = 0;

	/*
	 * THE MUSIC AND THE VOICE.
	 *
	 *     CdAudio_PlayTrackCue(3, 3)          0x004426f5
	 *     FUN_00404e10(1, 600, 0x57)          0x00442703
	 *
	 * [verified]. MODE 3 IS BOTH BITS - repeat, AND do not restart a
	 * track that is already playing - so arriving here from the title
	 * screen, which started track 3 looping, does not cut the music
	 * off and begin it again. Page 5 does the same, which is what
	 * makes the whole menu one continuous piece of music.
	 *
	 * 0x57 is makurchc, "make your choice", queued 600 ms in - about
	 * when the buttons finish sliding.
	 */
	audio_music_cue(CD_TRACK_MENU, CD_MODE_LOOP_KEEP);
	audio_cue_delayed(SND_VOICE_MAKURCHC, 600);

	mrlog("page3:  %s\n", s_p3_ok ? "loaded" : "INCOMPLETE");
}

/* ORIGIN  MenuPage03_MainMenu_Leave @ 0x00442730 */
static void page3_leave(void)
{
	sprite_free(&s_p3_backdrop);
	sprite_free(&s_p3_title);
	sprite_free(&s_p3_soloD);
	sprite_free(&s_p3_soloOk);
	sprite_free(&s_p3_linkD);
	sprite_free(&s_p3_linkOk);
	sprite_free(&s_p3_options);
	sprite_free(&s_p3_exit);
	if (s_p3_pocShow || s_p3_revShow) {
		sprite_free(&s_p3_pocD); sprite_free(&s_p3_pocL);
		sprite_free(&s_p3_revD); sprite_free(&s_p3_revL);
		sprite_free(&s_p3_onD);  sprite_free(&s_p3_onL);
		sprite_free(&s_p3_offD); sprite_free(&s_p3_offL);
	}
	s_p3_pocShow = 0;
	s_p3_revShow = 0;
	menu_clear_buttons();
	s_p3_ok = 0;
}

/* ORIGIN  MenuPage03_MainMenu_Tick @ 0x00442790 */
static void page3_tick(void)
{
	int act;

	if (!s_p3_ok)
		return;

	/*
	 * STAGE TWO, chained on the top row landing rather than on a
	 * timer. moveActive is MR_Sprite +0x10 (sprite.h), and the
	 * original tests it on DAT_00504890 - which is sprite-table base
	 * 0x00504880 plus 0x10, so resource 4, SOLO_D. The same sprite
	 * this tests.
	 */
	if (!s_p3_stage2 && !s_p3_soloD.moveActive) {
		p3_move(&s_p3_options, P3_OPTIONS_END_X, P3_OPTIONS_END_Y,
		        P3_SLIDE2_SEC);
		p3_move(&s_p3_exit,    P3_EXIT_END_X,    P3_EXIT_END_Y,
		        P3_SLIDE2_SEC);
		s_p3_stage2 = 1;
	}

	menu_hit_test();

	/* The background scrolls DIAGONALLY here - the tick passes (1, 1),
	 * where page 15 passes (0, 1). Same sprite kind, different feel. */
	sprite_update_wave(&s_p3_backdrop, 1, 1);
	sprite_draw_wave(&s_p3_backdrop);

	sprite_update_move(&s_p3_title);
	sprite_draw(&s_p3_title, SPRITE_MODE_USE_KIND, 100, 0);

	menu_draw_buttons();

	/*
	 * THE WORD PAIR, drawn per row. Both words always appear; the
	 * toggle only decides which one is lit. The four sprites are
	 * shared, so each row repositions them before drawing - which is
	 * exactly what the original does, twice per frame.
	 */
	if (s_p3_pocShow) {
		int x = s_p3_revShow ? P3_POC_WORD_X_SHARED : P3_POC_WORD_X_ALONE;

		p3_place(s_p3_pocketOn(), x, P3_POC_WORD_Y);
		sprite_draw(s_p3_pocketOn(), SPRITE_MODE_USE_KIND, 100, 0);
		p3_place(s_p3_pocketOff(), x + P3_WORD_GAP, P3_POC_WORD_Y);
		sprite_draw(s_p3_pocketOff(), SPRITE_MODE_USE_KIND, 100, 0);
	}
	if (s_p3_revShow) {
		int x = s_p3_pocShow ? P3_REV_WORD_X_SHARED : P3_REV_WORD_X_ALONE;

		p3_place(s_p3_reverseOn(), x, P3_REV_WORD_Y);
		sprite_draw(s_p3_reverseOn(), SPRITE_MODE_USE_KIND, 100, 0);
		p3_place(s_p3_reverseOff(), x + P3_WORD_GAP, P3_REV_WORD_Y);
		sprite_draw(s_p3_reverseOff(), SPRITE_MODE_USE_KIND, 100, 0);
	}

	act = menu_get_activated();
	if (act < 0)
		return;
	menu_clear_activated();

	/*
	 * THE INDICATORS TOGGLE AND DO NOT NAVIGATE, so they are handled
	 * before the switch and return. The original tests the activated
	 * index against two stored button indices for the same reason -
	 * their position in the list depends on which of them exist.
	 */
	if (act == s_p3_pocBtn) {
		g_pocketEnabled ^= 1;
		menu_save_unlocks();
		return;
	}
	if (act == s_p3_revBtn) {
		g_reverseEnabled ^= 1;
		menu_save_unlocks();
		return;
	}

	switch (act) {
	case P3_BTN_SOLO:
		/*
		 * DIVERGENCE, DELIBERATE: WE SPEAK WITHOUT WAITING.
		 *
		 * MenuPage03_MainMenu_Tick holds the page change until the
		 * line ends. It arms a latch at 0x0053f554 when Snd_PlayCue
		 * returns something other than -1, stores a deadline at
		 * 0x0053f78c, and while that deadline stands it jumps past
		 * the ENTIRE activation switch at 0x00442e37 - so SOLO,
		 * LINK, OPTIONS and EXIT are all dead, not just the button
		 * pressed. playsolo is 18936 bytes at 11025 Hz 16-bit mono,
		 * so the freeze is about 860 ms.
		 *
		 * WE DO NOT DO THAT, by decision: menu transitions stay
		 * responsive and the line plays over the page change. Do
		 * not "restore" the wait by reading audio_cue's return -
		 * that is what it would take, and it is not wanted.
		 *
		 * Page 10 DOES hold, and that one is kept - it covers a
		 * race load rather than a menu hop.
		 */
		audio_cue(SND_VOICE_PLAYSOLO, 0);
		/*
		 * AND THIS IS WHERE g_soloMode IS SET. SOLO writes 1 at
		 * 0x00442df3 and LINK writes 0 at 0x00442e06 [verified].
		 * Page 5 reads it to decide whether PRACTICE is offered and
		 * where its back button goes, so the flag has to be written
		 * before the transition, not after it lands.
		 */
		g_soloMode = 1;
		/*
		 * [open] THE ORIGINAL CHOOSES PAGE 5 OR PAGE 26 HERE on a
		 * CD presence check. Untestable under rule 8, so this
		 * always takes page 5.
		 */
		menu_begin_transition(MENU_PAGE_MODE);
		break;
	case P3_BTN_LINK:
		/* Speaks without waiting - see the SOLO arm above. */
		audio_cue(SND_VOICE_PLAYLINK, 0);
		g_soloMode = 0;
		s_netSavedPocket  = g_pocketEnabled;
		s_netSavedReverse = g_reverseEnabled;
		s_netSavedValid   = 1;
		menu_begin_transition(MENU_PAGE_PROTOCOL);
		break;
	case P3_BTN_OPTIONS:
		menu_begin_transition(MENU_PAGE_OPTIONS);
		break;
	case P3_BTN_EXIT:
		/*
		 * EXIT GOES TO PAGE 4 AND DOES NOT SHATTER - PUSH 4 then
		 * Menu_GotoPage at 0x00442e4c, with no Menu_BeginTransition
		 * beside it [verified]. The tick has exactly three
		 * BeginTransition calls, which is one each for SOLO, LINK
		 * and OPTIONS. Page 4 closes the game; see its tick.
		 */
		menu_goto_page(MENU_PAGE_QUIT);
		break;
	default:
		break;
	}
}


/* ================================================================ */
/* Page 2 - protocol, the connection-method list                    */
/* ================================================================ */

/*
 * The page table record is at 0x004dbb58.
 *
 * THE PAGE IS A SERVICE-PROVIDER PICKER. Enter calls
 * DirectPlayEnumerateA with the callback at 0x00486750 and fills a
 * table of 0x110-byte records at 0x005f5ac8; Tick lays each one out
 * as a text button. So the empty rows are not a stub - THE LIST IS
 * THE ENUMERATION, and with no DirectPlay providers installed there
 * is nothing to list. The six row slots are painted by tableau.lez
 * regardless, which is why retail shows a frame with nothing in it
 * [observed].
 *
 * THIS IS A DELIBERATE SLICE - graphics and CANCEL only, the whole
 * of what CLAUDE.md rule 15 allows for a menu. What is NOT here:
 *
 *   the DirectPlay enumeration and the record table;
 *   the six text buttons and their VK_NUMPAD1.. keys;
 *   the up/down scroll pair (61..66), which Enter only positions
 *     when the provider count reaches 7 - so they are invisible on
 *     any machine this will run on;
 *   FUN_004542c0, which opens the chosen provider and goes to
 *     page 7 on success or back to page 3 on failure.
 *
 * Until networking exists there is nothing to enumerate, so the
 * slice and the whole behave identically on a modern machine: a
 * frame, a caption and a way out.
 */

#define P2_RES_BACKDROP   1     /* paralax2.lez, the checkerboard   */
#define P2_RES_TITRE     67     /* titre.lez, the PROTOCOL banner   */
#define P2_RES_STITRE    68     /* stitre.lez, the caption          */
#define P2_RES_TABLEAU   69     /* tableau.lez, the six-row frame   */
#define P2_RES_CANCEL    71     /* cancel.lez                       */

/* The four Sprite_SetPosition calls at the tail of Enter. */
#define P2_TITRE_X     0
#define P2_TITRE_Y     0
#define P2_STITRE_X   83       /* 0x53 */
#define P2_STITRE_Y  152       /* 0x98 */
#define P2_TABLEAU_X  83       /* 0x53 */
#define P2_TABLEAU_Y 197       /* 0xc5 */
#define P2_CANCEL_X    0
#define P2_CANCEL_Y  430       /* 0x1ae, and CENTRED - the third
                                * argument is 1 where the other
                                * three pass 0 */

static MR_Sprite s_p2_backdrop;
static MR_Sprite s_p2_titre;
static MR_Sprite s_p2_stitre;
static MR_Sprite s_p2_tableau;
static MR_Sprite s_p2_cancel;
static int       s_p2_ok;

/*
 * THE RELAY, AND THE ONLY ONE. One address, typed once and kept in
 * the ini - netplay-relay.txt 4.4. There is deliberately no list of
 * servers to choose between.
 */
#define P2_FONT          12
#define P2_ROW_DY        26
#define P2_ENTER_Y      224
#define P2_ADDR_Y   (P2_ENTER_Y + P2_ROW_DY)
#define P2_BTN_Y    (P2_ADDR_Y + P2_ROW_DY * 2)   /* a row of space */
#define P2_STATUS_Y (P2_BTN_Y + P2_ROW_DY)
#define P2_COL_X         92
#define P2_ADDR_MAX      64

static MR_Font s_p2_font;
static int     s_p2_fontOk;
static char    s_p2_addr[P2_ADDR_MAX];
static int     s_p2_editing;
static int     s_p2_btnAddr;
static int     s_p2_btnConnect;
static char    s_p2_status[64];

/* ORIGIN  NONE - ours */
static void p2_set_status(const char *text)
{
	int i;

	for (i = 0; i < (int)sizeof(s_p2_status) - 1 && text[i]; i++)
		s_p2_status[i] = text[i];
	s_p2_status[i] = 0;
}

/*
 * ORIGIN  NONE - ours
 *
 * host, or host:port. Splitting here is what lets the whole thing be
 * one line to type and one line to read back out of the ini.
 */
static void p2_connect(void)
{
	char host[P2_ADDR_MAX];
	int  port = 0;
	int  i;

	for (i = 0; i < P2_ADDR_MAX - 1 && s_p2_addr[i] &&
	     s_p2_addr[i] != ':'; i++)
	    host[i] = s_p2_addr[i];
	host[i] = 0;
	if (s_p2_addr[i] == ':')
		port = atoi(s_p2_addr + i + 1);

	if (!host[0]) {
		p2_set_status("enter the relay address first");
		return;
	}

	/* Kept, so this is typed once per machine. */
	write_ini_string("NET", "Relay", s_p2_addr);

	netplay_open();
	netplay_set_relay(host, port);
	if (netplay_connect(g_playerName))
		p2_set_status("connecting...");
	else
		p2_set_status("could not start the connection");
}

/* ORIGIN  MenuPage02_Protocol_Enter @ 0x00486860 */
static void page2_enter(void)
{
	s_p2_ok  = 1;
	s_p2_ok &= load_res(&s_p2_backdrop, P2_RES_BACKDROP, 0, 0,
	                    SPRITE_ANCHOR_NONE, "page2");
	s_p2_ok &= load_res(&s_p2_titre, P2_RES_TITRE,
	                    P2_TITRE_X, P2_TITRE_Y,
	                    SPRITE_ANCHOR_NONE, "page2");
	s_p2_ok &= load_res(&s_p2_stitre, P2_RES_STITRE,
	                    P2_STITRE_X, P2_STITRE_Y,
	                    SPRITE_ANCHOR_NONE, "page2");
	s_p2_ok &= load_res(&s_p2_tableau, P2_RES_TABLEAU,
	                    P2_TABLEAU_X, P2_TABLEAU_Y,
	                    SPRITE_ANCHOR_NONE, "page2");
	s_p2_ok &= load_res(&s_p2_cancel, P2_RES_CANCEL,
	                    P2_CANCEL_X, P2_CANCEL_Y,
	                    SPRITE_ANCHOR_CENTRE_X, "page2");

	/*
	 * THE NAME, BECAUSE PAGE 15 MAY NEVER HAVE RUN - it is the only
	 * other reader of [PLAYER] name, and a startpage that skips the
	 * intro reaches here without it.
	 */
	if (!g_playerName[0]) {
		if (!get_ini_string(P15_INI_SECTION, P15_INI_KEY, g_playerName,
		                    (int)sizeof(g_playerName)))
		    g_playerName[0] = 0;
	}

	/*
	 * THE HANDLERS, BEFORE ANY MESSAGE CAN ARRIVE, and nothing else
	 * calls this. MISSING IT LOOKS LIKE IT WORKS: netplay.c keeps
	 * the roster and chat itself, so only the handler-driven half -
	 * the join, the start, the race - silently does nothing.
	 */
	menu_net_install();

	s_p2_fontOk    = fontdef_open(&s_p2_font, P2_FONT);
	s_p2_editing   = 0;
	s_p2_status[0] = 0;
	s_p2_addr[0]   = 0;
	get_ini_string("NET", "Relay", s_p2_addr, (int)sizeof(s_p2_addr));

	mrlog("page2:  link - %s, relay \"%s\"\n",
	      s_p2_ok ? "loaded" : "INCOMPLETE", s_p2_addr);
}

/* ORIGIN  MenuPage02_Protocol_Tick @ 0x00486a60 */
static void page2_tick(void)
{
	if (!s_p2_ok)
		return;

	/*
	 * THE BACKDROP SCROLLS (-1, +1) HERE. Page 3 passes (1, 1) and
	 * page 11 (-1, -1); this one is 0xffffffff, 1 at 0x00486ba4.
	 * Three pages, three directions - so it is per page and not a
	 * property of the sprite.
	 */
	sprite_update_wave(&s_p2_backdrop, -1, 1);
	sprite_draw_wave(&s_p2_backdrop);

	netplay_poll();

	/* Connected - the game list owns everything from here. */
	if (netplay_state() == NET_BROWSING) {
		menu_begin_transition(MENU_PAGE_SESSION);
		return;
	}
	if (netplay_state() == NET_FAILED) {
		p2_set_status("the relay did not answer");
		netplay_close();
	}

	/* The address field closed: ENTER keeps it, ESCAPE drops it. */
	if (s_p2_editing && !menu_capture_active()) {
		if (menu_capture_done()) {
			int i;

			for (i = 0; i < P2_ADDR_MAX - 1 &&
			     menu_capture_text()[i]; i++)
			    s_p2_addr[i] = menu_capture_text()[i];
			s_p2_addr[i] = 0;
		}
		menu_capture_clear_result();
		s_p2_editing = 0;
	}

	menu_clear_buttons();
	s_p2_btnAddr = s_p2_btnConnect = -1;
	if (s_p2_fontOk && !s_p2_editing) {
		s_p2_btnAddr = menu_add_text_button("ENTER RELAY ADDRESS",
		                                    &s_p2_font, P2_COL_X,
		                                    P2_ENTER_Y, 0, 0);
		s_p2_btnConnect = menu_add_text_button("CONNECT", &s_p2_font,
		                                       P2_COL_X,
		                                       P2_BTN_Y, 0, 0);
	}
	menu_add_sprite_button(&s_p2_cancel, NULL, VK_ESCAPE);

	sprite_update_move(&s_p2_titre);
	sprite_draw(&s_p2_titre, SPRITE_MODE_USE_KIND, 100, 0);
	sprite_update_move(&s_p2_stitre);
	sprite_draw(&s_p2_stitre, SPRITE_MODE_USE_KIND, 100, 0);
	sprite_update_move(&s_p2_tableau);
	sprite_draw(&s_p2_tableau, SPRITE_MODE_USE_KIND, 100, 0);

	menu_hit_test();
	menu_draw_buttons();

	if (s_p2_fontOk) {
		char line[96];

		_snprintf(line, sizeof(line), "RELAY  %s%s",
		          s_p2_editing ? menu_capture_text() : s_p2_addr,
		          (s_p2_editing && menu_capture_cursor_on())
		          ? "_" : "");
		font_draw_text(&s_p2_font, P2_COL_X, P2_ADDR_Y, line, 0);
		if (s_p2_status[0])
			font_draw_text(&s_p2_font, P2_COL_X, P2_STATUS_Y,
		                   s_p2_status, 0);
	}

	if (menu_get_activated() >= 0) {
		int which = menu_get_activated();

		menu_clear_activated();
		if (which == s_p2_btnAddr) {
			s_p2_editing = 1;
			menu_capture_begin(NULL, P2_ADDR_MAX - 1);
		} else if (which == s_p2_btnConnect) {
			p2_connect();
		} else {
			/*
			 * CANCEL SHATTERS. The original's cancel arm is
			 * Menu_BeginTransition at 0x00486e42 then PUSH 3 and
			 * Menu_GotoPage at 0x00486e4f [verified] - unlike
			 * page 3's own EXIT, which goes straight there.
			 */
			netplay_close();
			menu_begin_transition(MENU_PAGE_MAIN);
		}
	}
}

/* ORIGIN  MenuPage02_Leave @ 0x00486a40 */
static void page2_leave(void)
{
	if (s_p2_fontOk) {
		font_free(&s_p2_font);
		s_p2_fontOk = 0;
	}
	sprite_free(&s_p2_backdrop);
	sprite_free(&s_p2_titre);
	sprite_free(&s_p2_stitre);
	sprite_free(&s_p2_tableau);
	sprite_free(&s_p2_cancel);
	s_p2_ok = 0;
}


/* ================================================================ */
/* Page 5 - mode select                                             */
/* ================================================================ */

/*
 * THE PAGE HAS TWO SHAPES AND g_soloMode PICKS ONE. Page 3 writes
 * that flag before it transitions here - 1 from SOLO at 0x00442df3,
 * 0 from LINK at 0x00442e06 [verified] - and every difference on
 * this page follows from it:
 *
 *   solo     PRACTICE, SINGLE RACE, CHAMPIONSHIP; back goes to the
 *            main menu, page 3
 *   network  SINGLE RACE and CHAMPIONSHIP only; back goes to the
 *            session list, page 7
 *
 * o PRACTICE IS NOT HIDDEN, IT IS LEFT OFFSCREEN. The prologue
 *   places it at x -242 and the network branch simply never moves
 *   it in or registers a button for it. Nothing tests a visibility
 *   flag - the sprite is just somewhere the screen is not.
 *
 * o AND THE THREE BUTTONS SLIDE IN FROM DIFFERENT EDGES. Practice
 *   and championship start off the left and right respectively and
 *   single race starts BELOW the screen at y 595, all arriving on
 *   the same row at y 115 over one second.
 */

/*
 * THE "2" SPRITE IS THE DARK ONE ON THIS PAGE, which is the reverse
 * of what page 3 taught. There the pair is SOLO_D and SOLO_OK and
 * the _D is spriteA, the idle art; here the pair is practic2 and
 * practice and PRACTIC2 IS spriteA.
 *
 * That is read off the calls, not guessed: the sprite table is
 * indexed by resource id from 0x00504880, and Enter passes
 * (0x005048a4, 0x005048a8) which is (id 9, id 10) - practic2 then
 * practice [verified]. Same for the other three pairs, all of which
 * put the numbered file first.
 */
#define P5_RES_BACKDROP     0    /* paralax1.lez, kind 4        */
#define P5_RES_TITLE        8    /* selmode.lez                 */
#define P5_RES_PRACTICE_D   9    /* practic2.lez - idle         */
#define P5_RES_PRACTICE_L  10    /* practice.lez - hovered      */
#define P5_RES_SINGLE_D    11    /* single2.lez                 */
#define P5_RES_SINGLE_L    12    /* single.lez                  */
#define P5_RES_CHAMP_D     13    /* champio2.lez                */
#define P5_RES_CHAMP_L     14    /* champion.lez                */
#define P5_RES_GP_D        15    /* Grdprix2.lez - the BMG pair */
#define P5_RES_GP_L        16    /* Grdprix.lez                 */
#define P5_RES_BACK        81    /* Btback.lez                  */

/* Every position in this page, straight out of Enter. */
#define P5_ROW_Y           115   /* 0x73, the row they all land on */
#define P5_PRACTICE_START_X (-242)  /* -0xf2 */
#define P5_SINGLE_START_Y   595     /* 0x253, below the screen */
#define P5_SINGLE_START_X   229     /* 0xe5  */
#define P5_CHAMP_START_X    700
#define P5_BACK_X           270     /* 0x10e */
#define P5_BACK_Y           430     /* 0x1ae */
#define P5_TITLE_START_Y    (-80)   /* 0xffffffb0 */

/* Solo: three modes, evenly spread. */
#define P5_SOLO_PRACTICE_X   18     /* 0x12  */
#define P5_SOLO_SINGLE_X    229     /* 0xe5  */
#define P5_SOLO_CHAMP_X     440     /* 0x1b8 */

/* Network: two modes, and they start further out. */
#define P5_NET_SINGLE_FROM (-263)   /* -0x107 */
#define P5_NET_CHAMP_FROM   715     /* 0x2cb  */
#define P5_NET_SINGLE_X     121     /* 0x79   */
#define P5_NET_CHAMP_X      331     /* 0x14b  */

#define P5_SLIDE_SEC        1.0f
#define P5_VOICE_DELAY_MS   600

/*
 * The button ids, and they are NOT the same in the two shapes. Back
 * is always 0; after that the modes are numbered in the order they
 * are added, and the network shape is missing practice.
 */
#define P5_BTN_BACK         0

/*
 * g_soloMode is 0x004dbffc and g_selectedMode is 0x004dc094.
 *
 * Both are read far outside the menus - g_soloMode alone has close
 * to a hundred readers across the racer, input, HUD, in-game menu
 * and game-object code - so they are globals here too rather than
 * page state.
 *
 * o g_soloMode HAS THREE WRITERS, not one. Page 3's SOLO and LINK
 *   arms at 0x00442df3 and 0x00442e06, and
 *   maybe_Menu_StartAttractDemo at 0x00409ada [verified]. The third
 *   is menu-side too, and the port does not have it - see the
 *   [open] on the attract timeout in page14_tick.
 */
int g_soloMode     = 1;    /* 1 solo, 0 network. SOLO is the default */
int g_selectedMode;        /* 0..2, and see page 5 for what 0 means  */

static MR_Sprite s_p5_backdrop;
static MR_Sprite s_p5_title;
static MR_Sprite s_p5_back;
static MR_Sprite s_p5_practiceD, s_p5_practiceL;
static MR_Sprite s_p5_singleD,   s_p5_singleL;
static MR_Sprite s_p5_champD,    s_p5_champL;
static int       s_p5_ok;
static int       s_p5_practiceBtn;   /* -1 in a network game */
static int       s_p5_singleBtn;
static int       s_p5_champBtn;

/* ORIGIN  NONE - ours */
static void p5_place(MR_Sprite *sp, int x, int y)
{
	int t[2];

	t[0] = x;
	t[1] = y;
	sprite_set_position(sp, t, SPRITE_ANCHOR_NONE);
}

/* ORIGIN  NONE - ours */
static void p5_move(MR_Sprite *sp, int x, int y)
{
	int t[2];

	t[0] = x;
	t[1] = y;
	sprite_start_move(sp, t, P5_SLIDE_SEC, 1, SPRITE_ANCHOR_NONE);
}

/*
 * g_bResultsPageSeen - 0x004e90c8, and it is what page 6 BACK
 * asks before it decides where to go.
 *
 * Written in exactly two places and read in exactly one
 * [verified]:
 *
 *     0 at the tail of MenuPage05_ModeSelect_Enter  0x00447700
 *     1 at the tail of MenuPage09_Results_Enter     0x0044d4fc
 *     read by MenuPage06_RaceSelect_Tick            0x00448d70
 *
 * SO IT MEANS "THE STANDINGS HAVE BEEN SHOWN SINCE THE LAST
 * VISIT TO MODE SELECT" - a championship is part-way through.
 * Choosing a mode starts a fresh one and clears it.
 *
 * It is set in every mode, not only championship; the one read
 * is already gated on g_selectedMode == 2, so the mode does not
 * need to be part of the flag.
 */
static int s_menu_resultsSeen;

/* ORIGIN  MenuPage05_ModeSelect_Enter @ 0x00447250 */
static void page5_enter(void)
{
	int champD = s_bmg ? P5_RES_GP_D : P5_RES_CHAMP_D;
	int champL = s_bmg ? P5_RES_GP_L : P5_RES_CHAMP_L;

	s_p5_ok = 1;
	s_p5_ok &= load_res(&s_p5_backdrop, P5_RES_BACKDROP, 0, 0,
	                    SPRITE_ANCHOR_NONE, "page5");
	s_p5_ok &= load_res(&s_p5_title, P5_RES_TITLE, 0, P5_TITLE_START_Y,
	                    SPRITE_ANCHOR_NONE, "page5");
	s_p5_ok &= load_res(&s_p5_back, P5_RES_BACK, P5_BACK_X, P5_BACK_Y,
	                    SPRITE_ANCHOR_NONE, "page5");

	/*
	 * THE PROLOGUE PLACES ALL THREE MODES OFFSCREEN, and it runs
	 * whichever shape follows. Practice at x -242, championship at
	 * x 700, single race BELOW at y 595.
	 */
	s_p5_ok &= load_res(&s_p5_practiceD, P5_RES_PRACTICE_D,
	                    P5_PRACTICE_START_X, P5_ROW_Y,
	                    SPRITE_ANCHOR_NONE, "page5");
	s_p5_ok &= load_res(&s_p5_practiceL, P5_RES_PRACTICE_L,
	                    P5_PRACTICE_START_X, P5_ROW_Y,
	                    SPRITE_ANCHOR_NONE, "page5");
	s_p5_ok &= load_res(&s_p5_singleD, P5_RES_SINGLE_D,
	                    P5_SINGLE_START_X, P5_SINGLE_START_Y,
	                    SPRITE_ANCHOR_NONE, "page5");
	s_p5_ok &= load_res(&s_p5_singleL, P5_RES_SINGLE_L,
	                    P5_SINGLE_START_X, P5_SINGLE_START_Y,
	                    SPRITE_ANCHOR_NONE, "page5");
	s_p5_ok &= load_res(&s_p5_champD, champD, P5_CHAMP_START_X,
	                    P5_ROW_Y, SPRITE_ANCHOR_NONE, "page5");
	s_p5_ok &= load_res(&s_p5_champL, champL, P5_CHAMP_START_X,
	                    P5_ROW_Y, SPRITE_ANCHOR_NONE, "page5");

	/*
	 * ONLY spriteA IS EVER MOVED. Both halves get the start position
	 * above - harmlessly, since menu_draw_buttons puts the highlight
	 * onto spriteA at draw time the way Menu_Draw does, so whatever
	 * spriteB was placed at is overwritten before it draws. See the
	 * note there.
	 */
	menu_clear_buttons();
	s_p5_practiceBtn = -1;

	menu_add_sprite_button(&s_p5_back, NULL, VK_ESCAPE);

	if (g_soloMode) {
		p5_move(&s_p5_practiceD, P5_SOLO_PRACTICE_X, P5_ROW_Y);
		p5_move(&s_p5_singleD,   P5_SOLO_SINGLE_X,   P5_ROW_Y);
		p5_move(&s_p5_champD,    P5_SOLO_CHAMP_X,    P5_ROW_Y);

		s_p5_practiceBtn = menu_add_sprite_button(&s_p5_practiceD,
		                                         &s_p5_practiceL, 0);
		s_p5_singleBtn   = menu_add_sprite_button(&s_p5_singleD,
		                                         &s_p5_singleL, 0);
		s_p5_champBtn    = menu_add_sprite_button(&s_p5_champD,
		                                         &s_p5_champL, 0);
	} else {
		/* Single race and championship start FURTHER OUT in this
		 * shape - -263 and 715 - and land closer together. */
		p5_place(&s_p5_singleD, P5_NET_SINGLE_FROM, P5_ROW_Y);
		p5_place(&s_p5_champD,  P5_NET_CHAMP_FROM,  P5_ROW_Y);

		p5_move(&s_p5_singleD, P5_NET_SINGLE_X, P5_ROW_Y);
		p5_move(&s_p5_champD,  P5_NET_CHAMP_X,  P5_ROW_Y);

		s_p5_singleBtn = menu_add_sprite_button(&s_p5_singleD,
		                                       &s_p5_singleL, 0);
		s_p5_champBtn  = menu_add_sprite_button(&s_p5_champD,
		                                       &s_p5_champL, 0);
	}

	/* The title drops in from above, same one second. */
	p5_move(&s_p5_title, 0, 0);

	/*
	 * MODE 3, NOT 2 - CdAudio_PlayTrackCue(3, 3) [verified], so the
	 * music carries on unbroken from page 3 through the glass. Page
	 * 14 is the one that starts it, with mode 2.
	 */
	audio_music_cue(CD_TRACK_MENU, CD_MODE_LOOP_KEEP);

	/*
	 * "selectmd" after 600 ms, which is roughly when the slide-in
	 * finishes. The voice is queued, not played - see audio_cue_delayed
	 * for the variadic table it comes from.
	 */
	audio_cue_delayed(SND_VOICE_SELECTMD, P5_VOICE_DELAY_MS);

	/* PICKING A MODE STARTS A FRESH CHAMPIONSHIP, so page 6's
	   back button stops offering the standings.  Last thing
	   the original's enter handler does, 0x00447700. */
	s_menu_resultsSeen = 0;

	mrlog("page5:  %s, %s\n", s_p5_ok ? "loaded" : "INCOMPLETE",
	      g_soloMode ? "solo" : "network");
}

/* ORIGIN  NONE - ours. Page 5's table record at 0x004dbb88 reads
 *         {0, 0x00447710, 0x00447250, 0} - likewise a NULL
 *         fnLeave [verified]. */
static void page5_leave(void)
{
	sprite_free(&s_p5_backdrop);
	sprite_free(&s_p5_title);
	sprite_free(&s_p5_back);
	sprite_free(&s_p5_practiceD);
	sprite_free(&s_p5_practiceL);
	sprite_free(&s_p5_singleD);
	sprite_free(&s_p5_singleL);
	sprite_free(&s_p5_champD);
	sprite_free(&s_p5_champL);
	menu_clear_buttons();
	s_p5_ok = 0;
}

/* ORIGIN  MenuPage05_ModeSelect_Tick @ 0x00447710 */
static void page5_tick(void)
{
	int act;
	int mode;

	if (!s_p5_ok)
		return;

	/*
	 * paralax1 THIS TIME, AND IT SCROLLS DIAGONALLY THE OTHER WAY.
	 * The backdrop is sprite index 0 - g_menuObjects itself, the
	 * base of the table - updated with (-1, 1) [verified,
	 * 0x00447719].
	 *
	 * THE SCROLL IS PER PAGE, NOT PER TILE. Five pages scroll and
	 * four directions are used: page 2 and page 5 both (-1, 1),
	 * page 3 (1, 1), page 6 (1, -1), page 11 (-1, -1). Two pages
	 * sharing a direction is what rules out its being a property
	 * of the sprite.
	 */
	sprite_update_wave(&s_p5_backdrop, -1, 1);
	sprite_draw_wave(&s_p5_backdrop);

	sprite_update_move(&s_p5_title);
	sprite_draw(&s_p5_title, SPRITE_MODE_USE_KIND, 100, 0);

	menu_hit_test();
	menu_draw_buttons();

	act = menu_get_activated();
	if (act < 0)
		return;
	menu_clear_activated();

	if (act == P5_BTN_BACK) {
		/*
		 * BACK GOES TWO DIFFERENT PLACES, and it is the same flag
		 * again: page 3 for a solo game, the session list for a
		 * network one [verified, 0x004478f6].
		 */
		menu_begin_transition(g_soloMode ? MENU_PAGE_MAIN
		                                 : MENU_PAGE_SESSION);
		return;
	}

	/*
	 * THE MODE IS THE BUTTON INDEX MINUS ONE, and that means it
	 * numbers DIFFERENT THINGS in the two shapes - 0 is practice in
	 * a solo game and single race in a network one, because practice
	 * simply is not in the network list [verified, 0x004478a6].
	 */
	mode = act - 1;
	g_selectedMode = mode;

	/*
	 * CHAMPIONSHIP CLEARS THE TWO UNLOCKABLES. The test is
	 * "mode is neither 0 nor 1", which in a solo game is exactly
	 * championship - and in a NETWORK game is never true at all,
	 * since the only modes there are 0 and 1. Transcribed as found:
	 * it may be deliberate, or it may be that the shift in what the
	 * numbers mean was not carried through to this test.
	 *
	 * o IT CLEARS THE IN-MEMORY FLAGS ONLY, and does not save. So a
	 *   championship turns reverse and pocket off for this run and
	 *   the ini still remembers them - which is the same rule page
	 *   3 follows when it toggles them.
	 *
	 *
	 * o AND IT CLEARS TWO MORE THINGS WE DO NOT. 0x004dc08c is
	 *   g_selectedTrack and 0x00509ba8 is the championship race
	 *   index - see page 9's hole - so choosing a mode resets the
	 *   track AND the position in the championship, which is what
	 *   "start a fresh one" means. Neither is cleared here.
	 */
	if (mode != 0 && mode != 1) {
		g_reverseEnabled = 0;
		g_pocketEnabled  = 0;
	}

	/*
	 * PAGE 6 FOR EVERY MODE. All three go to the same page and it
	 * reads g_selectedMode to decide what to be - arrows and a lap
	 * counter for practice, arrows only for a single race, neither
	 * for a championship.
	 */
	menu_begin_transition(MENU_PAGE_RACE);
}

/* ================================================================ */
/* Page 6 - race select                                             */
/* ================================================================ */

/*
 * THE MODE DECIDES WHICH PAGE THIS IS, and there are three shapes:
 *
 *   0 practice       arrows, a lap counter, a time-attack toggle
 *   1 single race    arrows, and the lap count is FORCED to the
 *                    track default every frame
 *   2 championship   no arrows at all - the championship picks the
 *                    track - plus a LOAD button and its own title
 *
 * The lap forcing is the whole reason mode 1 has no + and -:
 *
 *     if (g_soloMode && g_selectedMode != 0)
 *         laps = track[selected].defaultLaps;
 *
 * runs at the head of every tick [verified, 0x0044815c], before the
 * digits are drawn and before the +/- activation is dispatched, so a
 * change would be overwritten before it could be drawn. Only
 * practice lets you set the distance.
 */

#define P6_RES_BACKDROP    0    /* paralax1, kind 4, scroll 1, -1  */
#define P6_RES_TITLE      25    /* selrace.lez                     */
#define P6_RES_CHTITREG  114    /* CHTITREG.lez, the BMG title     */
#define P6_RES_CHTITRE   115    /* CHTITRE.lez                     */
#define P6_RES_LEFT       26    /* left.lez                        */
#define P6_RES_LEFT_L     27    /* left_l.lez, the flash           */
#define P6_RES_RIGHT      28    /* right.lez                       */
#define P6_RES_RIGHT_L    29    /* right_l.lez                     */
#define P6_RES_NUM0       30    /* num0.lez .. num9.lez are 30..39 */
#define P6_RES_MOIN       40    /* moin.lez, the minus             */
#define P6_RES_MOIN_L     41    /* moin_l.lez, hovered             */
#define P6_RES_MOIFLACH   42    /* moiflach.lez, the press flash   */
#define P6_RES_PLUS       43
#define P6_RES_PLUS_L     44
#define P6_RES_PLUFLACH   45
#define P6_RES_LAPS       46    /* laps.lez, the word              */
#define P6_RES_CONTINUE   77    /* continue.lez                    */
#define P6_RES_BTLOAD     79    /* BtLoad.lez, championship only   */
#define P6_RES_BTBACK     81
#define P6_RES_BTHISCOR   86    /* BtHiscor.lez                    */
#define P6_RES_TIMEATAK  171    /* timeatak.lez, drawn ON the photo */
#define P6_RES_ATTACKON  172
#define P6_RES_ATTACKOF  173
#define P6_RES_ATTAKFLA  174    /* attakfla.lez                    */
#define P6_RES_MAPTXT    187    /* maptxt.lez, drawn above the map */

/*
 * EVERY POSITION AND EVERY DURATION, straight out of Enter. The
 * split is by WHAT, not by which edge it comes from - both speeds
 * carry things in from the left and the right:
 *
 *   1.0s   the title, the track art, and the lap row
 *   0.7s   the two arrows and the right-hand button column
 *
 * The third duration below, 0.5s, is not an entrance at all - it is
 * the track-art swap, and it runs from the tick.
 */
#define P6_MAP_FROM_X    (-200)
#define P6_MAP_X            78    /* 0x4e  */
#define P6_MAP_Y           133    /* 0x85  */
#define P6_PHOTO_FROM_X    740    /* 0x2e4 */
#define P6_PHOTO_X         320    /* 0x140 */
#define P6_PHOTO_Y          91    /* 0x5b  */
#define P6_MAP_EXIT_Y    (-400)   /* 0xfffffe70, off the top    */
#define P6_PHOTO_EXIT_Y    580    /* 0x244, off the bottom      */
#define P6_TITLE_FROM_Y  (-100)
#define P6_LEFT_FROM_X   (-100)
#define P6_LEFT_X            6
#define P6_LEFT_Y          148    /* 0x94  */
#define P6_RIGHT_FROM_X    740
#define P6_RIGHT_X         578    /* 0x242 */
#define P6_RIGHT_Y         146    /* 0x92  */
#define P6_MOIN_X          266    /* 0x10a */
#define P6_PLUS_X          328    /* 0x148 */
#define P6_STEP_FROM_Y     580    /* 0x244 */
#define P6_STEP_Y          404    /* 0x194 */
#define P6_LAPS_X          412    /* 0x19c */
#define P6_LAPS_Y          351    /* 0x15f */
#define P6_CONTINUE_X      514    /* 0x202 */
#define P6_CONTINUE_Y      356    /* 0x164 */
#define P6_SIDE_X          514    /* 0x202, BtLoad and BtHiscor   */
#define P6_SIDE_Y          397    /* 0x18d, and they share it     */
#define P6_BACK_X          516    /* 0x204 */
#define P6_BACK_Y          438    /* 0x1b6 */
#define P6_TIMEATAK_X      342    /* 0x156 */
#define P6_TIMEATAK_Y      128    /* 0x80  */
#define P6_ATTACK_X        395    /* 0x18b */
#define P6_ATTACK_Y        393    /* 0x189 */
#define P6_DIGIT_UNITS_X   330    /* 0x14a */
#define P6_DIGIT_TENS_X    275    /* 0x113 */
#define P6_DIGIT_Y         334    /* 0x14e */
#define P6_MAPTXT_DY      (-43)   /* 0x2b above the map           */

#define P6_SLIDE_SLOW    1.0f
#define P6_SLIDE_FAST    0.7f     /* 0x3f333333 */
#define P6_SWAP_SEC      0.5f     /* 0x3f000000, the track change */
#define P6_LAPS_MIN         1    /* 0x00448c95, minus stops here */
#define P6_LAPS_MAX         8    /* 0x00448cfc, plus stops here  */
#define P6_DIGITS          10

/*
 * THE TRACK ART IS PER TRACK AND LOADED ON DEMAND. Enter allocates
 * two sprites for every island and marks them all unloaded, then
 * loads only the selected one; the tick loads a track pair the
 * first time that track is selected [verified].
 *
 * o A FAILED LOAD RETRIES FOREVER IN THE ORIGINAL, because the
 *   flag it sets on failure is the same value that means "not
 *   loaded yet". Ours keeps a third state so a missing file is
 *   reported once and then left alone.
 */
#define P6_ART_UNLOADED 0
#define P6_ART_OK       1
#define P6_ART_FAILED   2

typedef struct {
	MR_Sprite map;
	MR_Sprite photo;
	int       state;      /* P6_ART_*                        */
} P6_TrackArt;

static P6_TrackArt *s_p6_art;
static int          s_p6_artCount;

static MR_Sprite s_p6_backdrop;
static MR_Sprite s_p6_title;
static MR_Sprite s_p6_left,  s_p6_leftL;
static MR_Sprite s_p6_right, s_p6_rightL;
static MR_Sprite s_p6_moin,  s_p6_moinL,  s_p6_moinFlash;
static MR_Sprite s_p6_plus,  s_p6_plusL,  s_p6_plusFlash;
static MR_Sprite s_p6_laps;
static MR_Sprite s_p6_continue;
static MR_Sprite s_p6_btload;
static MR_Sprite s_p6_btback;
static MR_Sprite s_p6_bthiscor;
static MR_Sprite s_p6_timeatak;
static MR_Sprite s_p6_attackOn, s_p6_attackOff, s_p6_attackFlash;
static MR_Sprite s_p6_maptxt;
static MR_Sprite s_p6_digit[P6_DIGITS];
static int       s_p6_digitOk[P6_DIGITS];
static int       s_p6_ok;

/* The one-frame press flash, and which sprite it is. */
static MR_Sprite *s_p6_flash;

/* Button ids, filled per frame because the set changes with mode. */
static int s_p6_btnBack, s_p6_btnContinue, s_p6_btnLeft, s_p6_btnRight;
static int s_p6_btnHiscor, s_p6_btnMinus, s_p6_btnPlus, s_p6_btnAttack;
static int s_p6_btnLoad;

/*
 * g_selectedTrack is 0x004dc08c, g_raceLaps is 0x004dc074 and
 * g_timeAttack is 0x004dc148. All three outlive the page.
 */
int g_selectedTrack;
int g_raceLaps = 1;
int g_timeAttack;

/* 0x004dc058, "every track is selectable". The original never writes
 * it - six xrefs, all reads. Ours is [DEBUG] alltracks. */
static int s_p6_allTracks;

/* The previously selected track, so a change can slide the old art
 * out while the new one slides in. 0x005409ac. */
static int s_p6_shownTrack;
/*
 * WHICH TRACK THE LAP COUNT BELONGS TO.
 *
 * The distance is reset to the track default when the TRACK
 * CHANGES and at no other time - so a practice distance survives
 * starting the race, leaving it and coming back, and is lost the
 * moment a different island is chosen [observed on MOTO].
 *
 * MenuPage06_RaceSelect_Tick has exactly ONE write to
 * g_nRaceLaps (0x00448189) and it is the mode 1/2 forcing below;
 * the enter path does not touch it, which is what makes the value
 * persist. RESETTING ON ENTRY THROWS THE SELECTION AWAY between the
 * menu and the race.
 */
static int s_p6_lapsTrack = -1;

/* ORIGIN  NONE - ours */
static void p6_place(MR_Sprite *sp, int x, int y)
{
	int t[2];

	t[0] = x;
	t[1] = y;
	sprite_set_position(sp, t, SPRITE_ANCHOR_NONE);
}

/* ORIGIN  NONE - ours */
static void p6_move(MR_Sprite *sp, int x, int y, float secs)
{
	int t[2];

	t[0] = x;
	t[1] = y;
	sprite_start_move(sp, t, secs, 1, SPRITE_ANCHOR_NONE);
}

/*
 * ORIGIN  MenuPage06_RaceSelect_Tick @ 0x00448110, the map/photo load.
 *         The archive-relative naming is ours - rule 10
 *
 * Load one track's map and photo. The names come out of the island
 * list - "data\ile01\" plus map.lez - and go through
 * track_asset_name, which strips the "data\" a stock install does
 * not have inside its archives. CLAUDE.md rule 10.
 */
static void p6_load_art(int index)
{
	const MR_Track *t = track_at(index);
	char            name[TRACK_PATH_MAX];
	P6_TrackArt    *a;

	if (!t || index < 0 || index >= s_p6_artCount)
		return;
	a = &s_p6_art[index];
	if (a->state != P6_ART_UNLOADED)
		return;

	a->state = P6_ART_FAILED;
	if (!track_asset_name(t, "MAP.LEZ", name, (int)sizeof(name)) ||
	    !sprite_load(&a->map, name, 1))
	    return;
	if (!track_asset_name(t, "PHOTO.LEZ", name, (int)sizeof(name)) ||
	    !sprite_load(&a->photo, name, 1)) {
		sprite_free(&a->map);
		return;
	}
	a->state = P6_ART_OK;

	p6_place(&a->map,   P6_MAP_FROM_X,   P6_MAP_Y);
	p6_place(&a->photo, P6_PHOTO_FROM_X, P6_PHOTO_Y);
}

/* ORIGIN  MenuPage06_RaceSelect_Tick @ 0x00448110, the two selection
 *         walks
 *
 * Step the selection, skipping tracks the list marks off. The walk
 * stops when it comes back round, so a list with nothing available
 * does not spin. */
static void p6_step_track(int delta)
{
	int start = g_selectedTrack;
	int n     = tracks_count();
	const MR_Track *t;

	if (n <= 0)
		return;
	do {
		g_selectedTrack += delta;
		if (g_selectedTrack < 0)
			g_selectedTrack = n - 1;
		else if (g_selectedTrack >= n)
			g_selectedTrack = 0;
		t = track_at(g_selectedTrack);
	} while ((!t || !t->available) && !s_p6_allTracks &&
	         g_selectedTrack != start);

	t = track_at(g_selectedTrack);
	if (t) {
		g_raceLaps      = t->laps;
		s_p6_lapsTrack  = g_selectedTrack;
	}
}

/* ORIGIN  MenuPage06_RaceSelect_Enter @ 0x00447a30 */
static void page6_enter(void)
{
	const MR_Track *t;
	int             i;
	int             titleRes;

	s_p6_ok = 1;
	s_p6_ok &= load_res(&s_p6_backdrop, P6_RES_BACKDROP, 0, 0,
	                    SPRITE_ANCHOR_NONE, "page6");

	/*
	 * TWO TITLES, AND BOTH ARE PLACED AND MOVED in the original,
	 * unconditionally - selrace and CHTITRE slide in together and
	 * the tick draws whichever the mode calls for. The mode test
	 * below is ours: the original's Enter tests only the branding,
	 * which chooses CHTITRE or CHTITREG. One title is enough here.
	 */
	titleRes = P6_RES_TITLE;
	if (g_selectedMode == 2)
		titleRes = s_bmg ? P6_RES_CHTITREG : P6_RES_CHTITRE;
	s_p6_ok &= load_res(&s_p6_title, titleRes, 0, P6_TITLE_FROM_Y,
	                    SPRITE_ANCHOR_NONE, "page6");

	s_p6_ok &= load_res(&s_p6_left,   P6_RES_LEFT,   P6_LEFT_FROM_X,
	                    P6_LEFT_Y,  SPRITE_ANCHOR_NONE, "page6");
	s_p6_ok &= load_res(&s_p6_leftL,  P6_RES_LEFT_L, P6_LEFT_FROM_X,
	                    P6_LEFT_Y,  SPRITE_ANCHOR_NONE, "page6");
	s_p6_ok &= load_res(&s_p6_right,  P6_RES_RIGHT,  P6_RIGHT_FROM_X,
	                    P6_RIGHT_Y, SPRITE_ANCHOR_NONE, "page6");
	s_p6_ok &= load_res(&s_p6_rightL, P6_RES_RIGHT_L, P6_RIGHT_FROM_X,
	                    P6_RIGHT_Y, SPRITE_ANCHOR_NONE, "page6");

	s_p6_ok &= load_res(&s_p6_moin,  P6_RES_MOIN,  P6_MOIN_X,
	                    P6_STEP_FROM_Y, SPRITE_ANCHOR_NONE, "page6");
	s_p6_ok &= load_res(&s_p6_moinL, P6_RES_MOIN_L, P6_MOIN_X,
	                    P6_STEP_FROM_Y, SPRITE_ANCHOR_NONE, "page6");
	s_p6_ok &= load_res(&s_p6_moinFlash, P6_RES_MOIFLACH, 0, 0,
	                    SPRITE_ANCHOR_NONE, "page6");
	s_p6_ok &= load_res(&s_p6_plus,  P6_RES_PLUS,  P6_PLUS_X,
	                    P6_STEP_FROM_Y, SPRITE_ANCHOR_NONE, "page6");
	s_p6_ok &= load_res(&s_p6_plusL, P6_RES_PLUS_L, P6_PLUS_X,
	                    P6_STEP_FROM_Y, SPRITE_ANCHOR_NONE, "page6");
	s_p6_ok &= load_res(&s_p6_plusFlash, P6_RES_PLUFLACH, 0, 0,
	                    SPRITE_ANCHOR_NONE, "page6");

	s_p6_ok &= load_res(&s_p6_laps, P6_RES_LAPS, P6_PHOTO_FROM_X,
	                    P6_LAPS_Y, SPRITE_ANCHOR_NONE, "page6");
	s_p6_ok &= load_res(&s_p6_continue, P6_RES_CONTINUE,
	                    P6_PHOTO_FROM_X, P6_CONTINUE_Y,
	                    SPRITE_ANCHOR_NONE, "page6");
	s_p6_ok &= load_res(&s_p6_btload, P6_RES_BTLOAD, P6_PHOTO_FROM_X,
	                    P6_SIDE_Y, SPRITE_ANCHOR_NONE, "page6");
	s_p6_ok &= load_res(&s_p6_bthiscor, P6_RES_BTHISCOR,
	                    P6_PHOTO_FROM_X, P6_SIDE_Y,
	                    SPRITE_ANCHOR_NONE, "page6");
	s_p6_ok &= load_res(&s_p6_btback, P6_RES_BTBACK, P6_PHOTO_FROM_X,
	                    P6_BACK_Y, SPRITE_ANCHOR_NONE, "page6");

	s_p6_ok &= load_res(&s_p6_timeatak, P6_RES_TIMEATAK,
	                    P6_TIMEATAK_X, P6_TIMEATAK_Y,
	                    SPRITE_ANCHOR_NONE, "page6");
	s_p6_ok &= load_res(&s_p6_attackOn, P6_RES_ATTACKON, P6_ATTACK_X,
	                    P6_ATTACK_Y, SPRITE_ANCHOR_NONE, "page6");
	s_p6_ok &= load_res(&s_p6_attackOff, P6_RES_ATTACKOF, P6_ATTACK_X,
	                    P6_ATTACK_Y, SPRITE_ANCHOR_NONE, "page6");
	s_p6_ok &= load_res(&s_p6_attackFlash, P6_RES_ATTAKFLA, 0, 0,
	                    SPRITE_ANCHOR_NONE, "page6");
	s_p6_ok &= load_res(&s_p6_maptxt, P6_RES_MAPTXT, 0, 0,
	                    SPRITE_ANCHOR_NONE, "page6");

	for (i = 0; i < P6_DIGITS; i++) {
		s_p6_digitOk[i] = load_res(&s_p6_digit[i], P6_RES_NUM0 + i,
		                           0, 0, SPRITE_ANCHOR_NONE, "page6");
	}

	/* The island list, and the page cannot exist without it. */
	if (tracks_count() == 0)
		tracks_load("LIST.BPT");

	s_p6_artCount = tracks_count();
	s_p6_art = (P6_TrackArt *)calloc((size_t)(s_p6_artCount > 0 ?
	                                 s_p6_artCount : 1),
	                                 sizeof(P6_TrackArt));
	if (!s_p6_art)
		s_p6_ok = 0;

	/*
	 * START ON A TRACK THAT IS ACTUALLY AVAILABLE. Four of the eight
	 * shipped islands are ON; the rest are unlocked by playing. See
	 * docs/formats/list-bpt.txt 3.
	 *
	 * [open] FUN_00447920, Menu_ChooseStartTrack in the DB, does two
	 * things this does not. In championship it picks the track by
	 * NAME out of the championship record rather than letting the
	 * player choose - docs/structs/MR_ChampEntry.txt - and drops back
	 * to single race when the championship index runs past the end.
	 * And its walk ACCEPTS an OFF track while g_nSoloMode and
	 * DAT_004dbff8 are both clear: OFF only means unselectable in
	 * solo mode.
	 */
	if (g_selectedTrack < 0 || g_selectedTrack >= tracks_count())
		g_selectedTrack = 0;
	t = track_at(g_selectedTrack);
	if (t && !t->available && !s_p6_allTracks)
		p6_step_track(1);

	/*
	 * ONLY WHEN THE TRACK HAS CHANGED since the lap count was last
	 * set. Resetting unconditionally here discards a lap count the
	 * player chose on this same track.
	 */
	t = track_at(g_selectedTrack);
	if (t && s_p6_lapsTrack != g_selectedTrack) {
		g_raceLaps     = t->laps;
		s_p6_lapsTrack = g_selectedTrack;
	}

	p6_load_art(g_selectedTrack);
	s_p6_shownTrack = g_selectedTrack;

	p6_move(&s_p6_title, 0, 0, P6_SLIDE_SLOW);
	if (s_p6_art && s_p6_artCount > 0 &&
	    s_p6_art[g_selectedTrack].state == P6_ART_OK) {
		p6_move(&s_p6_art[g_selectedTrack].map, P6_MAP_X, P6_MAP_Y,
		        P6_SLIDE_SLOW);
		p6_move(&s_p6_art[g_selectedTrack].photo, P6_PHOTO_X,
		        P6_PHOTO_Y, P6_SLIDE_SLOW);
	}
	p6_move(&s_p6_moin,  P6_MOIN_X, P6_STEP_Y, P6_SLIDE_SLOW);
	p6_move(&s_p6_moinL, P6_MOIN_X, P6_STEP_Y, P6_SLIDE_SLOW);
	p6_move(&s_p6_plus,  P6_PLUS_X, P6_STEP_Y, P6_SLIDE_SLOW);
	p6_move(&s_p6_plusL, P6_PLUS_X, P6_STEP_Y, P6_SLIDE_SLOW);
	p6_move(&s_p6_laps,  P6_LAPS_X, P6_LAPS_Y, P6_SLIDE_SLOW);

	p6_move(&s_p6_left,   P6_LEFT_X,  P6_LEFT_Y,  P6_SLIDE_FAST);
	p6_move(&s_p6_leftL,  P6_LEFT_X,  P6_LEFT_Y,  P6_SLIDE_FAST);
	p6_move(&s_p6_right,  P6_RIGHT_X, P6_RIGHT_Y, P6_SLIDE_FAST);
	p6_move(&s_p6_rightL, P6_RIGHT_X, P6_RIGHT_Y, P6_SLIDE_FAST);
	p6_move(&s_p6_btback, P6_BACK_X,  P6_BACK_Y,  P6_SLIDE_FAST);
	p6_move(&s_p6_continue, P6_CONTINUE_X, P6_CONTINUE_Y,
	        P6_SLIDE_FAST);
	p6_move(&s_p6_btload,   P6_SIDE_X, P6_SIDE_Y, P6_SLIDE_FAST);
	p6_move(&s_p6_bthiscor, P6_SIDE_X, P6_SIDE_Y, P6_SLIDE_FAST);

	audio_music_cue(CD_TRACK_MENU, CD_MODE_LOOP_KEEP);
	/*
	 * CHAMPIONSHIP GETS NO VOICE. The queue is skipped entirely when
	 * the mode is 2 [verified].
	 *
	 * Two of the four pages that queue a line gate it: this one, and
	 * page 3 at 0x00442703, which gates both its line and its CD cue
	 * on the championship script parsing - FUN_00429a00, now
	 * Champ_ParseScript in the DB. Pages 5 and 15 queue theirs
	 * straight through.
	 */
	if (g_selectedMode != 2)
		audio_cue_delayed(SND_VOICE_SELERACE, 600);

	mrlog("page6:  %s, mode %d, track %d of %d\n",
	      s_p6_ok ? "loaded" : "INCOMPLETE", g_selectedMode,
	      g_selectedTrack, tracks_count());
}

/* ORIGIN  MenuPage06_RaceSelect_Leave @ 0x004480d0 */
static void page6_leave(void)
{
	int i;

	sprite_free(&s_p6_backdrop);
	sprite_free(&s_p6_title);
	sprite_free(&s_p6_left);       sprite_free(&s_p6_leftL);
	sprite_free(&s_p6_right);      sprite_free(&s_p6_rightL);
	sprite_free(&s_p6_moin);       sprite_free(&s_p6_moinL);
	sprite_free(&s_p6_moinFlash);
	sprite_free(&s_p6_plus);       sprite_free(&s_p6_plusL);
	sprite_free(&s_p6_plusFlash);
	sprite_free(&s_p6_laps);
	sprite_free(&s_p6_continue);
	sprite_free(&s_p6_btload);
	sprite_free(&s_p6_btback);
	sprite_free(&s_p6_bthiscor);
	sprite_free(&s_p6_timeatak);
	sprite_free(&s_p6_attackOn);   sprite_free(&s_p6_attackOff);
	sprite_free(&s_p6_attackFlash);
	sprite_free(&s_p6_maptxt);
	for (i = 0; i < P6_DIGITS; i++) {
		if (s_p6_digitOk[i])
			sprite_free(&s_p6_digit[i]);
		s_p6_digitOk[i] = 0;
	}
	if (s_p6_art) {
		for (i = 0; i < s_p6_artCount; i++) {
			if (s_p6_art[i].state == P6_ART_OK) {
				sprite_free(&s_p6_art[i].map);
				sprite_free(&s_p6_art[i].photo);
			}
		}
		free(s_p6_art);
		s_p6_art = NULL;
	}
	s_p6_artCount = 0;
	menu_clear_buttons();
	s_p6_ok = 0;
}

/* ORIGIN  MenuPage06_RaceSelect_Tick @ 0x00448110, the two lap
 *         digits of the lap count */
static void p6_draw_digit(int value, int x)
{
	if (value < 0 || value >= P6_DIGITS || !s_p6_digitOk[value])
		return;
	p6_place(&s_p6_digit[value], x, P6_DIGIT_Y);
	sprite_draw(&s_p6_digit[value], SPRITE_MODE_USE_KIND, 100, 0);
}

/* ORIGIN  MenuPage06_RaceSelect_Tick @ 0x00448110, the scan over
 *         g_nTrackArtIdle @ 0x005420d4
 *
 * Is any track sprite still moving? The original gates a new slide
 * on this so two changes cannot overlap. */
static int p6_art_idle(void)
{
	int i;

	for (i = 0; i < s_p6_artCount; i++) {
		if (s_p6_art[i].state != P6_ART_OK)
			continue;
		if (s_p6_art[i].map.moveActive || s_p6_art[i].photo.moveActive)
			return 0;
	}
	return 1;
}

/* ORIGIN  MenuPage06_RaceSelect_Tick @ 0x00448110 */
static void page6_tick(void)
{
	const MR_Track *t;
	int             act;
	int             i;
	int             champ = (g_selectedMode == 2);

	if (!s_p6_ok)
		return;

	/* The forced lap count - see the page note above. */
	t = track_at(g_selectedTrack);
	if (g_soloMode && g_selectedMode != 0 && t)
		g_raceLaps = t->laps;

	sprite_update_wave(&s_p6_backdrop, 1, -1);
	sprite_draw_wave(&s_p6_backdrop);

	sprite_update_move(&s_p6_title);
	sprite_draw(&s_p6_title, SPRITE_MODE_USE_KIND, 100, 0);

	/* A newly selected track loads its art the first time it is
	 * reached, then the two pairs cross over. */
	p6_load_art(g_selectedTrack);
	if (s_p6_shownTrack != g_selectedTrack && p6_art_idle()) {
		P6_TrackArt *in  = &s_p6_art[g_selectedTrack];
		P6_TrackArt *out = &s_p6_art[s_p6_shownTrack];

		if (in->state == P6_ART_OK) {
			p6_place(&in->map,   P6_MAP_FROM_X,   P6_MAP_Y);
			p6_place(&in->photo, P6_PHOTO_FROM_X, P6_PHOTO_Y);
			p6_move(&in->map,   P6_MAP_X,   P6_MAP_Y,   P6_SWAP_SEC);
			p6_move(&in->photo, P6_PHOTO_X, P6_PHOTO_Y, P6_SWAP_SEC);
		}
		if (out->state == P6_ART_OK) {
			p6_move(&out->map,   P6_MAP_X,   P6_MAP_EXIT_Y,
			        P6_SWAP_SEC);
			p6_move(&out->photo, P6_PHOTO_X, P6_PHOTO_EXIT_Y,
			        P6_SWAP_SEC);
		}
		s_p6_shownTrack = g_selectedTrack;
	}

	/* Every pair that is on screen or on its way off. */
	for (i = 0; i < s_p6_artCount; i++) {
		P6_TrackArt *a = &s_p6_art[i];

		if (a->state != P6_ART_OK)
			continue;
		if (a->map.moveActive || i == g_selectedTrack) {
			sprite_update_move(&a->map);
			sprite_draw(&a->map, SPRITE_MODE_USE_KIND, 100, 0);
			/* maptxt tracks the map sprite, wherever its slide
			 * has got to. */
			p6_place(&s_p6_maptxt, a->map.x, a->map.y + P6_MAPTXT_DY);
			sprite_draw(&s_p6_maptxt, SPRITE_MODE_USE_KIND, 100, 0);
		}
		if (a->photo.moveActive || i == g_selectedTrack) {
			sprite_update_move(&a->photo);
			sprite_draw(&a->photo, SPRITE_MODE_USE_KIND, 100, 0);
			if (g_timeAttack && g_selectedMode == 0) {
				/* The stamp rides on the photo, moving with it. */
				p6_place(&s_p6_timeatak, a->photo.x + 22,
				         a->photo.y + 37);
				sprite_draw(&s_p6_timeatak, SPRITE_MODE_USE_KIND,
				            100, 0);
			}
		}
	}

	/*
	 * EVERY MODE, INCLUDING CHAMPIONSHIP. The two digits and the laps
	 * label are drawn at function scope with no mode test
	 * [verified, 0x00448110], so championship shows the distance it
	 * will race even though it offers no + and -. Only the + and -
	 * BUTTONS are practice-only, and those are added below.
	 */
	p6_draw_digit(g_raceLaps % 10, P6_DIGIT_UNITS_X);
	p6_draw_digit(g_raceLaps / 10, P6_DIGIT_TENS_X);
	sprite_update_move(&s_p6_laps);
	sprite_draw(&s_p6_laps, SPRITE_MODE_USE_KIND, 100, 0);

	/*
	 * THE BUTTON SET IS REBUILT EVERY FRAME because it changes with
	 * the mode and with whether the time-attack toggle is offered.
	 * The original keeps a local per button holding its index and
	 * compares the activation against each; the ids here are the
	 * same thing named.
	 */
	menu_hit_test();
	menu_clear_buttons();

	s_p6_btnLeft = s_p6_btnRight = s_p6_btnHiscor = -1;
	s_p6_btnMinus = s_p6_btnPlus = s_p6_btnAttack = -1;
	s_p6_btnLoad = -1;

	s_p6_btnBack     = menu_add_sprite_button(&s_p6_btback, NULL,
	                                         VK_ESCAPE);
	if (champ) {
		s_p6_btnContinue = menu_add_sprite_button(&s_p6_continue,
		                                         NULL, 0);
		s_p6_btnLoad     = menu_add_sprite_button(&s_p6_btload,
		                                         NULL, 0);
	} else {
		s_p6_btnContinue = menu_add_sprite_button(&s_p6_continue,
		                                         NULL, VK_RETURN);
		s_p6_btnLeft     = menu_add_sprite_button(&s_p6_left, NULL,
		                                         VK_LEFT);
		s_p6_btnRight    = menu_add_sprite_button(&s_p6_right, NULL,
		                                         VK_RIGHT);
		s_p6_btnHiscor   = menu_add_sprite_button(&s_p6_bthiscor,
		                                         NULL, 0);

		if (g_selectedMode == 0) {
			s_p6_btnMinus = menu_add_sprite_button(&s_p6_moin,
			                                      &s_p6_moinL,
			                                      VK_SUBTRACT);
			s_p6_btnPlus  = menu_add_sprite_button(&s_p6_plus,
			                                      &s_p6_plusL, VK_ADD);

			/*
			 * THE TIME-ATTACK TOGGLE COMES AND GOES with the lap
			 * count. It is offered only while the chosen distance is
			 * no longer than the track default, and raising the laps
			 * past that TURNS TIME ATTACK OFF as well as hiding the
			 * button [verified, 0x0044838a].
			 */
			if (t && t->laps < g_raceLaps) {
				g_timeAttack = 0;
			} else {
				MR_Sprite *sp = g_timeAttack ? &s_p6_attackOn
				                             : &s_p6_attackOff;
				s_p6_btnAttack = menu_add_sprite_button(sp, sp, 0);
			}
		}
	}

	menu_draw_buttons();

	/*
	 * The press flash, one frame, over the top of everything.
	 *
	 * DIVERGENCE: the original draws each flash inline in its own
	 * activation handler, so it appears on the frame of the press.
	 * The handlers here run after this point, so a flash parks in
	 * s_p6_flash and appears on the NEXT frame.
	 */
	if (s_p6_flash) {
		sprite_draw(s_p6_flash, SPRITE_MODE_USE_KIND, 100, 0);
		s_p6_flash = NULL;
	}

	act = menu_get_activated();
	if (act < 0)
		return;
	menu_clear_activated();

	if (act == s_p6_btnLeft || act == s_p6_btnRight) {
		MR_Sprite *arrow = (act == s_p6_btnLeft) ? &s_p6_left
		                                         : &s_p6_right;
		MR_Sprite *flash = (act == s_p6_btnLeft) ? &s_p6_leftL
		                                         : &s_p6_rightL;

		p6_step_track(act == s_p6_btnLeft ? -1 : 1);
		p6_place(flash, arrow->x, arrow->y);
		s_p6_flash = flash;
		audio_cue(SND_MENU_MOVE, 0);
		return;
	}

	if (act == s_p6_btnMinus) {
		if (g_raceLaps > P6_LAPS_MIN) {
			g_raceLaps--;
			p6_place(&s_p6_moinFlash, s_p6_moin.x, s_p6_moin.y);
			s_p6_flash = &s_p6_moinFlash;
		}
		return;
	}
	if (act == s_p6_btnPlus) {
		if (g_raceLaps < P6_LAPS_MAX) {
			g_raceLaps++;
			p6_place(&s_p6_plusFlash, s_p6_plus.x, s_p6_plus.y);
			s_p6_flash = &s_p6_plusFlash;
		}
		return;
	}
	if (act == s_p6_btnAttack) {
		p6_place(&s_p6_attackFlash, s_p6_attackOn.x, s_p6_attackOn.y);
		s_p6_flash = &s_p6_attackFlash;
		g_timeAttack ^= 1;
		return;
	}

	if (act == s_p6_btnBack) {
		/*
		 * ORIGIN  MenuPage06_RaceSelect_Tick @ 0x00448110,
		 *         at 0x00448d70
		 *
		 * TWO CONDITIONS, NOT ONE:
		 *
		 *     if (mode == 2 && resultsSeen)  page 9
		 *     else                           page 5
		 *
		 * A championship that has not yet shown its
		 * standings backs out to MODE SELECT like every
		 * other mode; only one already under way returns to
		 * the standings, because that is where it came
		 * from.  Testing the mode alone would strand the
		 * first round on a page 9 that has nothing to show.
		 */
		menu_begin_transition((champ && s_menu_resultsSeen)
		                          ? MENU_PAGE_RESULTS
		                          : MENU_PAGE_MODE);
		return;
	}
	if (act == s_p6_btnContinue) {
		/*
		 * CONTINUE IS REFUSED ON A LOCKED TRACK, silently - the
		 * original clears the activation and does nothing at all
		 * [verified].
		 *
		 * AND IT IS NOT REDUNDANT WITH THE ARROWS. The arrows and
		 * this guard both demand available == 1, but the enter-time
		 * chooser does not - Menu_ChooseStartTrack accepts an OFF
		 * track outside solo mode, so it can land you on one the
		 * arrows would have skipped, and then this is the only
		 * thing stopping the race. See list-bpt.txt 2.
		 */
		if ((t && t->available) || s_p6_allTracks) {
			mrlog("page6:  %s, %d lap(s), attack %d -> page 10\n",
			      t ? t->name : "?", g_raceLaps, g_timeAttack);
			menu_begin_transition(MENU_PAGE_BIKE);
		}
		return;
	}
	if (act == s_p6_btnHiscor) {
		mrlog("page6:  hiscores -> page 9, not built\n");
		return;
	}
	if (act == s_p6_btnLoad) {
		mrlog("page6:  load -> page 20, championship load, not built\n");
		return;
	}
}

/* ================================================================ */
/* Page 10 - bike select                                            */
/* ================================================================ */

/*
 * EIGHT BIKES, AND THEY ARE THE EIGHT CARAC FILES - the page indexes
 * 0..7 straight into g_bikeCarac. See bikes.h for the four-file chain
 * from a track to its bikes.
 *
 * o THE SELECTION DOES NOT WRAP, unlike page 6's track list. Left is
 *   refused at 0 [verified, 0x0044f670] and right at 7 [verified,
 *   0x0044f7ba] - the arrows do nothing at all at the ends rather
 *   than rolling over.
 *
 * THE CHANGE ANIMATION IS NOT A SPIN, whatever it looks like: the
 * camera PANS and the bike stands still. There is no angular tween
 * and the model is never turned. See p10_pan_step below and
 * docs/subsystems/menu-pages.txt 2.7.4.6.
 */

#define P10_RES_BACKDROP  47    /* choosefo.lez, drawn once, static */
#define P10_RES_TITLE     48    /* choosemo.lez                     */
#define P10_RES_FEATURES  49    /* features.lez, the stats panel    */
#define P10_RES_FEATACC   50    /* and the four bars                */
#define P10_RES_FEATSPE   51
#define P10_RES_FEATGRI   52
#define P10_RES_FEATBRA   53
#define P10_RES_AUTO      54    /* auto.lez, lit                    */
#define P10_RES_AUTOFLA   55    /* autofla.lez, hovered             */
#define P10_RES_AUTOSHA   56    /* autosha.lez, shaded              */
#define P10_RES_MANU      57
#define P10_RES_MANUFLA   58
#define P10_RES_MANUSHA   59
#define P10_RES_LEFT      26
#define P10_RES_LEFT_L    27
#define P10_RES_RIGHT     28
#define P10_RES_RIGHT_L   29
#define P10_RES_START     76    /* start.lez                        */
#define P10_RES_BACK      81

/* Positions, straight out of Enter. Everything slides at one speed,
 * 1.0s - unlike page 6, which uses three. */
#define P10_TITLE_FROM_Y (-80)
#define P10_LEFT_FROM_X  (-100)
#define P10_LEFT_X          6
#define P10_LEFT_Y        148
#define P10_RIGHT_FROM_X  740
#define P10_RIGHT_X       578
#define P10_RIGHT_Y       146
#define P10_FEAT_X         27    /* 0x1b  */
#define P10_FEAT_FROM_Y   530    /* 0x212 */
#define P10_FEAT_Y        322    /* 0x142 */
#define P10_BAR_X         145    /* 0x91  */
#define P10_BAR0_FROM_Y   568    /* 0x238, and 0x1a apart      */
#define P10_BAR0_Y        360    /* 0x168, likewise            */
#define P10_BAR_STEP       27    /* 0x1b between the four bars */
#define P10_GEAR_X        401    /* 0x191 */
#define P10_AUTO_FROM_Y   564    /* 0x234 */
#define P10_AUTO_Y        356    /* 0x164 */
#define P10_MANU_FROM_Y   615    /* 0x267 */
#define P10_MANU_Y        407    /* 0x197 */
#define P10_START_FROM_X  740
#define P10_START_X       512    /* 0x200 */
#define P10_START_Y       373    /* 0x175 */
#define P10_BACK_X        512
#define P10_BACK_Y        417    /* 0x1a1 */
#define P10_SLIDE_SEC    1.0f

/*
 * THE BIKE IS THE ARTICULATED SKELETON, SLOT 0 - and the medium mesh
 * is the plausible wrong answer. maybe_Racer_SelectLod (0x0045f160)
 * would choose slot 2 at this distance, but it never runs on this
 * page, so the slot keeps the 0 Racer_Create leaves. Reading what a
 * function computes is not establishing that it runs. See
 * menu-pages.txt 2.7.4.6 and runtime-capture.txt 6.2.
 *
 * THE CAMERA ORBITS; THE MODEL NEVER TURNS. Turning the model would
 * look identical from the front and shade differently, which is why
 * the camera moves instead - and it is the same reason the bike
 * CHANGE is a camera pan rather than a moving bike.
 */
#define P10_BANK          3
/* .rdata holds these already signed: -96.0, then -48.0 and +48.0,
 * which are the rear and front axle signs. Ours are positive and the
 * signs are written into the formulas below. */
#define P10_WHEEL_RADIUS_K 96.0f   /* 0x004d0e90 holds -96.0        */
#define P10_WHEEL_AXLE_K   48.0f   /* 0x004d0e94 -48, 0x004d0e98 +48 */
#define P10_CAM_RADIUS 1500.0f     /* 0x44bb8000 in Enter        */

/*
 * PULLED BACK BY THE LOBBY, which wants the same bike smaller -
 * a bigger radius with the same projection IS the shrink, and it
 * costs nothing. page 10 never changes it.
 */
static float s_p10_camRadius = P10_CAM_RADIUS;

/* 0x004d0c58, a double: 2*pi/10240, one turn every 10.24 seconds */
#define P10_SPIN_RATE  0.0006135923152f

/*
 * Both doubles, and both scaled by cos(camAngle) before use - which
 * is what holds the excursion constant on SCREEN rather than in the
 * world. 250 is the screen edge exactly. See menu-pages.txt
 * 2.7.4.6.1; the frame-rate dependence is transcribed as-is and is
 * an open question there.
 */
#define P10_PAN_LIMIT 250.0f       /* 0x004d0c48                    */
#define P10_PAN_STEP  100.0f       /* 0x004d0c50, per FRAME, no dt  */

/*
 * TWO LIFTS STRADDLE THE LOOK-AT, so the view matrix is built for a
 * camera at y=200 while the scene is translated as if it were at
 * y=240.
 *
 * APPLYING EITHER ONE ALONE IS VISIBLY WRONG, which is the trap.
 * The lifts drop the bike about 50 px; the placement that offsets
 * them - maybe_Racer_Draw puts the bike at (0, 41.7597885,
 * -0.0123367), not at the origin - raises it about 53. So leaving
 * BOTH out also looks right, by cancellation rather than by being
 * correct. All four cases are tabulated in menu-pages.txt 2.7.4.7.
 */
#define P10_CAM_LIFT_VIEW 200.0f      /* 0x004d0c44, before the look-at */
#define P10_CAM_LIFT_DRAW  40.0f      /* 0x004d0bac, after it           */

#define P10_BARS 4
#define P10_TWEEN_MS 1000

static MR_Sprite s_p10_backdrop;
static MR_Sprite s_p10_title;
static MR_Sprite s_p10_features;
static MR_Sprite s_p10_bar[P10_BARS];
static MR_Sprite s_p10_auto, s_p10_autoFla, s_p10_autoSha;
static MR_Sprite s_p10_manu, s_p10_manuFla, s_p10_manuSha;
static MR_Sprite s_p10_left,  s_p10_leftL;
static MR_Sprite s_p10_right, s_p10_rightL;
static MR_Sprite s_p10_start;
static MR_Sprite s_p10_back;
static int       s_p10_ok;

/* The bike body: one mesh, one instance, drawn in the 3D bracket. */
static MR_Skeleton      s_p10_skl;
static MR_SklPose       s_p10_pose;
static int              s_p10_meshOk;
static MR_Primitive    *s_p10_prims;
static MR_ModelInstance s_p10_inst;

/*
 * THE WHEELS ARE SEPARATE MESHES, and the skeleton does not hold
 * them: Racer_Create attaches roueav and rouear as two more
 * renderable instances beside the body, at racer +0x1a8 and
 * +0x32c [verified, racers.txt section 6].
 *
 * maybe_Racer_PlaceWheels puts them at
 *
 *     rear  = (0, -96 * rayonAr, -48 * axeDistance)
 *     front = (0, -96 * rayonAv, +48 * axeDistance)
 *
 * from the racer position - which, after the skeleton is
 * re-centred on its root joint, is the model origin. See bikes.h
 * for the three .rdata constants.
 */
#define P10_WHEEL_REAR  0
#define P10_WHEEL_FRONT 1
#define P10_WHEELS      2

/*
 * g_menuRacer. THE MENU DRAWS A REAL RACER IN RETAIL, and that is the
 * whole reason this page can share the race draw.
 *
 * MenuPage10_BikeSelect_Tick at 0x0044f251 loads ECX with 0x00584421
 * - g_menuRacer, which is racer 1 - and calls maybe_Racer_Draw with
 * lod 0 (PUSH 0x0 at 0x0044f231). So the original has ONE per-racer
 * draw serving the track, this page and the podium - not three
 * separate paths.
 *
 * o THE SELECTED BIKE IS A FIELD OF THAT RACER, not a global sitting
 *   beside it. The DB labels 0x00584499 g_nSelectedBike; that
 *   address is g_menuRacer + 0x78, which structs/MR_Racer.txt gives
 *   as the bike index. Choosing a bike here IS writing the racer's
 *   livery field, which is why the shared draw needs nothing extra.
 *
 * o Passing lod 0 explicitly also copies matOrientation into
 *   matBikeRoot, which is what gives a racer that has never ticked a
 *   placement at all.
 */
static MR_Racer         s_p10_racer;
static MR_RacerDrawSet  s_p10_drawSet;

static MESH             s_p10_wheelMesh[P10_WHEELS];
static int              s_p10_wheelOk[P10_WHEELS];
static MR_Primitive    *s_p10_wheelPrims[P10_WHEELS];
static MR_ModelInstance s_p10_wheelInst[P10_WHEELS];

/*
 * The display placement: identity rotation, and [10] is the lift
 * p10_set_bike_world writes. PAGE 10 ONLY - the race passes the
 * racer's real position.
 */
static float s_p10_bikeWorld[12] = {
	1.0f, 0.0f, 0.0f,
	0.0f, 1.0f, 0.0f,
	0.0f, 0.0f, 1.0f,
	0.0f, 0.0f, 0.0f
};

/*
 * ORIGIN  NONE - ours. The original builds this matrix inline in
 *         maybe_Racer_Draw; the lift itself is read off
 *         0x0045f828..0x0045ff62 by bikes_display_lift.
 *
 * The display placement, NOT the track position. The formula, how it
 * was read off those two addends, and the landmine in it are all
 * with bikes_display_lift in bikes.c. Menu and race must pass the
 * same value.
 */
static void p10_set_bike_world(const MR_BikeCarac *b)
{
	s_p10_bikeWorld[10] = bikes_display_lift(b);
}

static int              s_p10_bankOk;
static float            s_p10_camAngle;
static double           s_p10_lastFrame;
static float            s_p10_view[12];

/*
 * THE SLIDE STATE, three globals in the original.
 *
 *   0x005420d0  spinDir     0 idle, -1 left pressed, +1 right
 *   0x005420dc  spinOffset  +1, -1 or 0 - added to the selected
 *                           bike to get the DISPLAYED one
 *   0x0053f540  target      the look-at target, a vec3
 *
 * spinOffset IS THE WHOLE TRICK: it cancels the decrement while the
 * old bike slides out, so ONE model does the whole change. See
 * menu-pages.txt 2.7.4.6.
 *
 * s_p10_targetDrawn EXISTS BECAUSE THE ORIGINAL BUILDS ITS VIEW
 * MATRIX BEFORE IT MOVES THE TARGET - Camera_LookAt at 0x0044edc2,
 * the slide arithmetic not until 0x0044ee23 - so every frame draws
 * with the previous frame's target. Our draw runs after the tick
 * instead, so the snapshot is what reproduces the lag.
 *
 * THE LAG IS LOAD-BEARING, NOT SLOPPY. It is what hides the livery
 * swap off the side of the screen and lands the pan exactly on
 * centre. Drop it and both go visibly wrong; the eight frames are
 * walked out in menu-pages.txt 2.7.4.6.2.
 */
static int   s_p10_spinDir;
static int   s_p10_spinOffset;
static float s_p10_target[3];
static float s_p10_targetDrawn[3];

static MR_Sprite *s_p10_flash;

/* The bar tween: where each bar came from, where it is going, and
 * when it started. */
static int    s_p10_barFrom[P10_BARS];
static int    s_p10_barTo[P10_BARS];
static double s_p10_tweenStart;
static int    s_p10_tweening;

static int s_p10_btnLeft, s_p10_btnRight, s_p10_btnStart, s_p10_btnBack;

/*
 * ORIGIN  MenuPage10_BikeSelect_Tick @ 0x0044ed30, at 0x0044f64a
 *
 * THE START HOLD, whose deadline the original keeps at 0x0053f78c.
 *
 * START fires two cues and then STAYS ON PAGE 10 until they
 * have finished - the original measures the sample with
 * Csample_LengthMs, adds timeGetTime to make a deadline, and
 * every tick after that jumps over the whole transition while
 * the clock is still below it.
 *
 * WITHOUT IT THE PORT CUTS ITS OWN AUDIO OFF. Leaving at once
 * reaches race_load, which swaps the sample bank, and the bank
 * free destroys every voice and releases the PCM underneath
 * them. Retail is additionally slow to load, so it had room to
 * spare; ours has none, and the hold is what makes the
 * difference rather than the loader's speed.
 *
 * DIVERGENCE: THE LONGEST, NOT THE LAST. The original stores
 * each length with MOV, so the deadline is whichever cue
 * started LAST - letsgo, at 1322 ms against the bike sample's
 * 697. Both calls are in the same tick, 31 bytes apart, so the
 * two agree today; taking the maximum keeps them agreeing if a
 * bike sample is ever the longer of the pair.
 */
static double s_p10_holdUntil;   /* tm_real_now seconds, 0 idle */
static int    s_p10_holdPage;
static int s_p10_btnAuto, s_p10_btnManu;

/*
 * g_selectedBike is 0x00584499 and g_autoGearbox is 0x004dc0a0.
 * The second is 1 for automatic and 0 for manual, and it decides
 * which of the two gearbox rows is drawn lit.
 */
int g_selectedBike;
int g_autoGearbox = 1;

/* ORIGIN  NONE - ours */
static void p10_place(MR_Sprite *sp, int x, int y)
{
	int t[2];

	t[0] = x;
	t[1] = y;
	sprite_set_position(sp, t, SPRITE_ANCHOR_NONE);
}

/* ORIGIN  NONE - ours. The original calls Sprite_StartMove inline,
 *         thirteen times in Enter, all at 1.0s. */
static void p10_move(MR_Sprite *sp, int x, int y)
{
	int t[2];

	t[0] = x;
	t[1] = y;
	sprite_start_move(sp, t, P10_SLIDE_SEC, 1, SPRITE_ANCHOR_NONE);
}

/*
 * ORIGIN  MenuPage10_BikeSelect_Enter @ 0x0044e480, the four shrinks
 *
 * The shrink for one bar of the current bike. The original really
 * does write it as (Gfx_x * -100) / 100 + 100 - LEA/SHL to make
 * x*100, NEG, then an actual IDIV by 100 and ADD 100, at
 * 0x0044e9a4..0x0044e9b7 and three more times for the other bars
 * [verified]. It reduces to 100 - x and the compiler never folded
 * it. The shrink-not-scale semantic is at P11_RES_SLIDER.
 */
static int p10_bar_target(int bar)
{
	const MR_BikeCarac *b = bike_at(g_selectedBike);
	int                 v = 0;

	if (!b)
		return 100;         /* nothing loaded: an empty bar */
	switch (bar) {
	case 0: v = b->gfxAcc; break;
	case 1: v = b->gfxSpe; break;
	case 2: v = b->gfxGri; break;
	case 3: v = b->gfxBra; break;
	}
	return 100 - v;
}

/*
 * ORIGIN  MenuPage10_BikeSelect, the stat-bar tween start:
 *         0x0044ea3c in _Enter, 0x0044f790 and 0x0044f8db in _Tick's
 *         two arrow arms. Original globals g_nStatTweenStart
 *         0x00540cc0 and g_nStatTweenEnd 0x00540cbc, end set to
 *         start + 0x3e8 - which is where P10_TWEEN_MS comes from.
 *
 * Start the one-second slide from the bars' current values to the
 * newly selected bike's.
 */
static void p10_start_tween(void)
{
	int i;

	for (i = 0; i < P10_BARS; i++) {
		s_p10_barFrom[i] = s_p10_barTo[i];
		s_p10_barTo[i]   = p10_bar_target(i);
	}
	s_p10_tweenStart = tm_now();
	s_p10_tweening   = 1;
}

/*
 * ORIGIN  NONE - ours. The original loads through
 *         Game_LoadBikeAndBanks @ 0x0040a4d0
 *
 * Load roueav and rouear and place them either side of the root
 * joint. They are ordinary static meshes - the .3df and .map pair
 * the manifest names - so this is the page 14 sequence again, once
 * per wheel.
 */
static void p10_load_wheels(const float bodyOffset[3])
{
	static const int kMeshPath[P10_WHEELS] = {
		BIKE_PATH_WHEEL_R, BIKE_PATH_WHEEL_F
	};
	static const int kMapPath[P10_WHEELS] = {
		BIKE_PATH_WHEEL_R_MAP, BIKE_PATH_WHEEL_F_MAP
	};
	static const float zero[3] = { 0.0f, 0.0f, 0.0f };
	const MR_BikeCarac *b = bike_at(g_selectedBike);
	int i;

	for (i = 0; i < P10_WHEELS; i++) {
		const char *mesh = bikes_asset(kMeshPath[i]);
		const char *map  = bikes_asset(kMapPath[i]);
		float       pos[3];
		int         nprims = 0;

		s_p10_wheelOk[i] = 0;
		if (!mesh || !map || !b)
			continue;
		if (!mesh_load_with_map(&s_p10_wheelMesh[i], mesh, map,
		                        MESH_SCALE_3DF_MAP)) {
			mrlog_err("page10: wheel %s did not load\n", mesh);
			continue;
		}
		if (!prim_build_from_mesh_bank(&s_p10_wheelMesh[i],
		                               &s_p10_wheelPrims[i], &nprims,
		                               0, P10_BANK) ||
		    nprims <= 0) {
			mesh_free(&s_p10_wheelMesh[i]);
			continue;
		}

		/*
		 * THIS POSITION PLACES NOTHING. racerdraw_racer places both
		 * wheels itself, from the racer contacts plus the offsets it
		 * solves each frame, and reads only .prims and .primCount off
		 * these instances. It is set so the instance is well formed,
		 * not because anything draws with it.
		 */
		pos[0] = bodyOffset[0];
		pos[1] = bodyOffset[1] - P10_WHEEL_RADIUS_K *
		         ((i == P10_WHEEL_REAR) ? b->rayonAr : b->rayonAv);
		pos[2] = bodyOffset[2] + ((i == P10_WHEEL_REAR)
		                          ? -P10_WHEEL_AXLE_K
		                          : P10_WHEEL_AXLE_K) * b->axeDistance;

		model_init(&s_p10_wheelInst[i], pos, zero, &s_p10_wheelMesh[i]);
		model_set_prims(&s_p10_wheelInst[i], s_p10_wheelPrims[i], nprims);
		s_p10_wheelOk[i] = 1;
	}
}

/* ORIGIN  MenuPage10_BikeSelect_Enter @ 0x0044e480 */
/*
 * ORIGIN  NONE - ours, lifted out of page10_enter unchanged.
 *
 * SHARED WITH THE NETPLAY LOBBY, which draws the same bike at a
 * different radius - menu-pages.txt 2.10.2. The mesh is PER TRACK,
 * not per bike: every bike on a track is the same model and the
 * choice is a palette, so this reloads on a track change only.
 */
/* ORIGIN  NONE - ours, lifted out of page10_leave unchanged. */
static void p10_free_bike(void)
{
	if (s_p10_prims) {
		free(s_p10_prims);
		s_p10_prims = NULL;
	}
	if (s_p10_meshOk) {
		skl_free(&s_p10_skl);
		s_p10_meshOk = 0;
	}
	{
		int w;

		for (w = 0; w < P10_WHEELS; w++) {
			if (!s_p10_wheelOk[w])
				continue;
			if (s_p10_wheelPrims[w]) {
				free(s_p10_wheelPrims[w]);
				s_p10_wheelPrims[w] = NULL;
			}
			mesh_free(&s_p10_wheelMesh[w]);
			s_p10_wheelOk[w] = 0;
		}
	}
	s_p10_bankOk = 0;
}


static int p10_load_bike(void)
{
	/*
	 * AND THE BIKE ITSELF. The manifest names the bank and the three
	 * static bodies, but this page draws the SKELETON rather than any
	 * of them - see the landmine at the top of the page. Everything
	 * else here is the sequence page 14 uses: load the pair, build
	 * primitives, rebase the bank, make an instance.
	 */
	s_p10_meshOk = 0;
	s_p10_prims  = NULL;
	s_p10_bankOk = 0;
	{
		const char *bankName  = bikes_asset(BIKE_PATH_BANK0);
		const char *bankName1 = bikes_asset(BIKE_PATH_BANK1);
		const char *body     = bikes_asset(BIKE_PATH_SKL);
		const char *pose     = bikes_asset(BIKE_PATH_ROT_STATICPIED);
		int         nprims   = 0;

		/*
		 * TWO BANKS, AND THE FLAGS ARE NOT PAGE 14'S.
		 *
		 * Game_LoadBikeAndBanks allocates both slots with paletted 1
		 * and palBits 8 [verified, the pushes at 0x0040a86c]. Page 14
		 * passes paletted 0 because that page is truecolour; this one
		 * is 256 colours, so page 14's arguments must not be copied
		 * here - they tell the blend ladder the wrong thing about the
		 * source.
		 *
		 * o AND THE MAP REFERENCES BOTH. Of medium\corps.map's 196
		 *   face records, 195 name relative bank 0 and one names bank
		 *   1, so bnk00_01.raw has to be in the next slot or that one
		 *   face has nowhere to sample from.
		 *
		 * o MODE 1, NOT 0. Mode 1 is what makes bank_load read the
		 *   sibling palettes, and those are the liveries.
		 */
		if (bankName) {
			s_p10_bankOk = bank_load(P10_BANK, bankName, 1);
			if (s_p10_bankOk)
				bank_alloc_texture_slot(P10_BANK, 1, 1, 8, 16);
			else
				mrlog_err("page10: bank %s did not load\n", bankName);
		}
		if (bankName1) {
			if (bank_load(P10_BANK + 1, bankName1, 1))
				bank_alloc_texture_slot(P10_BANK + 1, 1, 1, 8, 16);
		}

		/*
		 * THE ARTICULATED MODEL, AND THE POSE IT WEARS. Slot 0, for
		 * the reason given at the top of the page.
		 *
		 * o THE POSE IS staticPied. Game_LoadBikeAndBanks calls
		 *   Rot_ApplyToRacer(&g_menuRacer, 0) with ECX = 0x51d678, and
		 *   0x51d678 - g_pilotRot (0x51d2a8) = 4 * 0xF4, so the pose is
		 *   g_pilotRot[4] - index 4 of the manifest's PilotRot list
		 *   [verified].
		 *
		 * o A TEST THAT THE BIND POOL EQUALS THE BIND POSE CANNOT FAIL,
		 *   and one was once read as disproving that. maybe_Racer_Draw
		 *   never poses g_meshSkl's vertices - it transforms a slice per
		 *   part by a matrix - so the pool is unposed whatever the pose
		 *   is. Measure the per-part MATRICES instead.
		 *
		 * o THE POSE IS BAKED INTO THE VERTICES where the original keeps
		 *   one bind pool and a matrix per part. The matrices are kept
		 *   too, in MR_Skeleton.mat, so a part can be adjusted before
		 *   the bake - which is what part 15 needs. The .rot turn, the
		 *   per-part remap and skl_refork_front are all in skl.c.
		 */
		if (body)
			s_p10_meshOk = skl_load(&s_p10_skl, body);
		if (s_p10_meshOk && pose && rot_load(&s_p10_pose, pose)) {
			/*
			 * THE POSE IS LOADED AND DELIBERATELY NOT BAKED.
			 * racerdraw_racer solves the fork slide and the swingarm
			 * every frame from the same blocks maybe_Racer_Draw uses,
			 * so doing either here would place those parts twice. The
			 * draw reads skl->bind, the unposed pool.
			 */
		}

		if (!s_p10_meshOk) {
			mrlog_err("page10: bike skeleton %s did not load\n",
			          body ? body : "(no manifest)");
		} else if (!prim_build_from_mesh_bank(&s_p10_skl.mesh,
		                                &s_p10_prims, &nprims, 0,
		                                P10_BANK) || nprims <= 0) {
			mrlog_err("page10: bike skeleton produced no primitives\n");
			skl_free(&s_p10_skl);
			s_p10_meshOk = 0;
		} else {
			static const float zero[3] = { 0.0f, 0.0f, 0.0f };
			float offset[3];

			/* The build rebased already - see
			 * prim_build_from_mesh_bank. */

			/*
			 * DO NOT SHIFT THE MESH BY MINUS ITS BOUNDING-BOX CENTRE.
			 * The argument is reasonable and its premise is the wrong
			 * mesh: medium\corps.3df has its origin by the rear axle, so
			 * a camera on the world origin would pivot the bike about its
			 * rear wheel. But this page draws the SKELETON, and skl_load
			 * re-centres that on its root joint - the wheels come out
			 * symmetric at z +/-84. Subtracting the box centre
			 * (0.08, 28.80, -9.60) drops the bike 28.8 down and 9.6 back,
			 * which is the "sits too low" it looks like.
			 *
			 * The position comes from the stack instead, the way
			 * maybe_Racer_Draw builds its world matrix at 0x0045ff99 -
			 * the racer's own fields are all zero on this page. It is
			 * also what cancels the camera's 40; see P10_CAM_LIFT_DRAW
			 * and menu-pages.txt 2.7.4.7.
			 */
			offset[0] = s_p10_bikeWorld[9];
			offset[1] = s_p10_bikeWorld[10];
			offset[2] = s_p10_bikeWorld[11];

			model_init(&s_p10_inst, offset, zero, &s_p10_skl.mesh);
			model_set_prims(&s_p10_inst, s_p10_prims, nprims);
			p10_load_wheels(offset);
			mrlog("page10: bike %s, %d prims, centre %.0f %.0f %.0f\n",
			      body, nprims, -offset[0], -offset[1], -offset[2]);
		}
	}
	return s_p10_meshOk;
}


static void page10_enter(void)
{
	int i;

	/* A stale deadline would swallow the first START back here. */
	s_p10_holdUntil = 0.0;

	s_p10_ok = 1;
	s_p10_ok &= load_res(&s_p10_backdrop, P10_RES_BACKDROP, 0, 0,
	                     SPRITE_ANCHOR_NONE, "page10");
	s_p10_ok &= load_res(&s_p10_title, P10_RES_TITLE, 0,
	                     P10_TITLE_FROM_Y, SPRITE_ANCHOR_NONE,
	                     "page10");
	s_p10_ok &= load_res(&s_p10_features, P10_RES_FEATURES, P10_FEAT_X,
	                     P10_FEAT_FROM_Y, SPRITE_ANCHOR_NONE,
	                     "page10");

	for (i = 0; i < P10_BARS; i++) {
		s_p10_ok &= load_res(&s_p10_bar[i], P10_RES_FEATACC + i,
		                     P10_BAR_X, P10_BAR0_FROM_Y +
		                     i * P10_BAR_STEP, SPRITE_ANCHOR_NONE,
		                     "page10");
	}

	s_p10_ok &= load_res(&s_p10_auto, P10_RES_AUTO, P10_GEAR_X,
	                     P10_AUTO_FROM_Y, SPRITE_ANCHOR_NONE, "page10");
	s_p10_ok &= load_res(&s_p10_autoFla, P10_RES_AUTOFLA, P10_GEAR_X,
	                     P10_AUTO_FROM_Y, SPRITE_ANCHOR_NONE, "page10");
	s_p10_ok &= load_res(&s_p10_autoSha, P10_RES_AUTOSHA, P10_GEAR_X,
	                     P10_AUTO_Y, SPRITE_ANCHOR_NONE, "page10");
	s_p10_ok &= load_res(&s_p10_manu, P10_RES_MANU, P10_GEAR_X,
	                     P10_MANU_Y, SPRITE_ANCHOR_NONE, "page10");
	s_p10_ok &= load_res(&s_p10_manuFla, P10_RES_MANUFLA, P10_GEAR_X,
	                     P10_MANU_FROM_Y, SPRITE_ANCHOR_NONE, "page10");
	s_p10_ok &= load_res(&s_p10_manuSha, P10_RES_MANUSHA, P10_GEAR_X,
	                     P10_MANU_FROM_Y, SPRITE_ANCHOR_NONE, "page10");

	s_p10_ok &= load_res(&s_p10_left, P10_RES_LEFT, P10_LEFT_FROM_X,
	                     P10_LEFT_Y, SPRITE_ANCHOR_NONE, "page10");
	s_p10_ok &= load_res(&s_p10_leftL, P10_RES_LEFT_L, 0, 0,
	                     SPRITE_ANCHOR_NONE, "page10");
	s_p10_ok &= load_res(&s_p10_right, P10_RES_RIGHT, P10_RIGHT_FROM_X,
	                     P10_RIGHT_Y, SPRITE_ANCHOR_NONE, "page10");
	s_p10_ok &= load_res(&s_p10_rightL, P10_RES_RIGHT_L, 0, 0,
	                     SPRITE_ANCHOR_NONE, "page10");
	s_p10_ok &= load_res(&s_p10_start, P10_RES_START, P10_START_FROM_X,
	                     P10_START_Y, SPRITE_ANCHOR_NONE, "page10");
	s_p10_ok &= load_res(&s_p10_back, P10_RES_BACK, P10_START_FROM_X,
	                     P10_BACK_Y, SPRITE_ANCHOR_NONE, "page10");

	/*
	 * THE BIKE BANK COMES FROM THE TRACK, which is why this cannot
	 * be loaded any earlier: Enter calls Game_LoadBikeAndBanks with
	 * g_nSelectedTrack. We read the same chain for the numbers only.
	 */
	bikes_load_for_track(track_at(g_selectedTrack),
	                 g_pocketEnabled);

	p10_load_bike();

	/*
	 * THE LIVERY IS A PALETTE, NOT A PAGE. bank_load mode 1 reads
	 * BNK00_00.1 through .7 as sibling palettes of the one indexed
	 * page, giving eight, and the tick selects among them with
	 * (selectedBike + spinOffset) & 7.
	 *
	 * The bank ships ELEVEN siblings, .1 through .11, so a mask of 7
	 * leaves the last four unreachable from this page.
	 */
	bank_set_palette(P10_BANK, g_selectedBike & 7);

	/* Enter starts the orbit at 0 and puts the camera at z 1500 with
	 * zoom 1.0 - see P10_CAM_RADIUS, and the zoom below. */
	s_p10_camAngle  = 0.0f;
	s_p10_lastFrame = tm_now();

	s_p10_spinDir    = 0;
	s_p10_spinOffset = 0;
	for (i = 0; i < 3; i++) {
		s_p10_target[i]      = 0.0f;
		s_p10_targetDrawn[i] = 0.0f;
	}

	screen3d_set_proj_zoom(1.0f);
	if (g_selectedBike < 0 || g_selectedBike >= BIKE_COUNT)
		g_selectedBike = 0;

	/*
	 * THE BARS ANIMATE ON ENTRY, not only on a bike change. Enter
	 * writes the four targets, then writes all four FROM values with
	 * ESI - zero, from the XOR at 0x0044e4a2 - and arms the tween,
	 * g_nStatBarsTweening = 1 at 0x0044ea4b with a 1000 ms window
	 * [verified].
	 *
	 * SO THEY START FULL AND RETRACT, not grow. The stored quantity
	 * is a SHRINK - 0 is the whole sprite and 100 is empty, see
	 * p10_bar_target - so a FROM of 0 is four full-width bars pulling
	 * back to the bike's figures over the second.
	 *
	 * p10_start_tween is NOT what to call here: it carries the
	 * current values forward as the FROM, which is right for a bike
	 * change and wrong for an entry.
	 */
	for (i = 0; i < P10_BARS; i++) {
		s_p10_barFrom[i] = 0;
		s_p10_barTo[i]   = p10_bar_target(i);
	}
	s_p10_tweenStart = tm_now();
	s_p10_tweening   = 1;

	p10_move(&s_p10_title, 0, 0);
	p10_move(&s_p10_left,  P10_LEFT_X,  P10_LEFT_Y);
	p10_move(&s_p10_right, P10_RIGHT_X, P10_RIGHT_Y);
	p10_move(&s_p10_features, P10_FEAT_X, P10_FEAT_Y);
	for (i = 0; i < P10_BARS; i++)
		p10_move(&s_p10_bar[i], P10_BAR_X,
	             P10_BAR0_Y + i * P10_BAR_STEP);
	p10_move(&s_p10_auto,    P10_GEAR_X, P10_AUTO_Y);
	p10_move(&s_p10_manuSha, P10_GEAR_X, P10_MANU_Y);
	p10_move(&s_p10_start, P10_START_X, P10_START_Y);
	p10_move(&s_p10_back,  P10_BACK_X,  P10_BACK_Y);

	audio_music_cue(CD_TRACK_MENU, CD_MODE_LOOP_KEEP);

	mrlog("page10: %s, bike %d, gearbox %s\n",
	      s_p10_ok ? "loaded" : "INCOMPLETE", g_selectedBike,
	      g_autoGearbox ? "auto" : "manual");
}

/* ORIGIN  MenuPage10_BikeSelect_Leave @ 0x0044ecf0 */
static void page10_leave(void)
{
	int i;

	sprite_free(&s_p10_backdrop);
	sprite_free(&s_p10_title);
	sprite_free(&s_p10_features);
	for (i = 0; i < P10_BARS; i++)
		sprite_free(&s_p10_bar[i]);
	sprite_free(&s_p10_auto);    sprite_free(&s_p10_autoFla);
	sprite_free(&s_p10_autoSha);
	sprite_free(&s_p10_manu);    sprite_free(&s_p10_manuFla);
	sprite_free(&s_p10_manuSha);
	sprite_free(&s_p10_left);    sprite_free(&s_p10_leftL);
	sprite_free(&s_p10_right);   sprite_free(&s_p10_rightL);
	sprite_free(&s_p10_start);
	sprite_free(&s_p10_back);
	p10_free_bike();
	menu_clear_buttons();
	s_p10_ok = 0;
}

/*
 * ORIGIN  MenuPage10_BikeSelect_Tick @ 0x0044ed30, at 0x0044efa3
 *
 * THE DISPLAYED LIVERY, 0x0044efa3: (selected + spin offset) & 7.
 *
 * It is wanted in two places and must agree in both - see the two
 * callers - so it is a function rather than the expression twice.
 */
static int p10_livery(void)
{
	return (g_selectedBike + s_p10_spinOffset) & 7;
}

/* ORIGIN  NONE - ours, the scene-bracket split. Called from INSIDE
 *         the bracket - see menu_draw_page_3d. */
static void page10_draw_3d(void)
{
	float pos[3];

	if (!s_p10_meshOk)
		return;

	/*
	 * The camera orbits the origin at the radius Enter set, and
	 * LOOKS AT s_p10_targetDrawn RATHER THAN THE ORIGIN - which is
	 * the origin except while a bike change is sliding. The bike
	 * itself never moves.
	 */
	pos[0] = (float)sin(s_p10_camAngle) * s_p10_camRadius;
	pos[1] = P10_CAM_LIFT_VIEW;
	pos[2] = (float)cos(s_p10_camAngle) * s_p10_camRadius;

	/*
	 * BOTH LIFTS ARE APPLIED, AND THEY STRADDLE THE LOOK-AT exactly
	 * as 0x0044eda8 and 0x0044edde do. The view is built for a
	 * camera at 200 and the scene is then translated as if it were
	 * at 240.
	 *
	 * THE 200 ONLY TILTS. A look-at keeps its target centred, so
	 * raising the eye changes the angle and nothing else - that is
	 * the 7.6 degrees of nose-down view retail has and a level
	 * camera does not.
	 */
	if (!camera_look_at(s_p10_view, pos, s_p10_targetDrawn))
		return;
	pos[1] += P10_CAM_LIFT_DRAW;

	queue_reset();
	vertexpool_reset();

	/*
	 * THE SAME bike_draw THE RACE USES. Page 10 drew through
	 * MR_ModelInstance and the track built its matrices by hand, and
	 * the two drifted - every geometry bug found here showed up in
	 * one and not the other. The original has one maybe_Racer_Draw.
	 *
	 * THE BIKE NEVER TURNS HERE - the camera orbits it - so the
	 * orientation is the identity and the contacts fall straight out
	 * of racer_wheel_ground along the forward axis.
	 */
	{
		MR_AnimFrame frame;
		int          w;

		/*
		 * THIS memset IS WHY THE LIVERY IS SET HERE TOO. The
		 * draw rebuilds the whole racer every frame, so
		 * anything the TICK writes into it - the livery among
		 * them - is gone by the time the draw reads it. A tick
		 * that sets the livery writes the right value into a
		 * record about to be zeroed, and the page stays one
		 * colour.
		 */
		memset(&s_p10_racer, 0, sizeof s_p10_racer);
		s_p10_racer.active    = 1;
		s_p10_racer.bikeIndex = g_selectedBike;
		s_p10_racer.liveryRider      = p10_livery();
		s_p10_racer.liveryFrontWheel = s_p10_racer.liveryRider;
		s_p10_racer.liveryRearWheel  = s_p10_racer.liveryRider;
		s_p10_racer.matOrientation[0] = 1.0f;
		s_p10_racer.matOrientation[4] = 1.0f;
		s_p10_racer.matOrientation[8] = 1.0f;
		{
			const MR_BikeCarac *bc = bike_at(g_selectedBike);

			/*
			 * PLACE IT, DO NOT GROUND IT. The original runs
			 * maybe_Racer_PlaceWheels here, which writes the
			 * two wheel instances at their REST OFFSETS -
			 * (0, -96 * rayon, +-48 * axeDistance) - and that
			 * is what racer+0x1b4 and +0x338 hold on a page
			 * with no track.
			 *
			 * racer_wheel_ground, which stood here, is the
			 * RACE-TIME builder for the same two fields and
			 * carries NO RADIUS TERM. With the identity
			 * orientation this page uses it returns y = 0 for
			 * both wheels instead of -30.72 and -27.84, and
			 * the bike sat about thirty units too high.
			 *
			 * NO TRACK ON THIS PAGE, so the segment lookup is
			 * skipped - see racer_place_wheels.
			 */
			if (bc)
				racer_place_wheels(&s_p10_racer, bc,
			                       s_p10_inst.position, NULL);
			else
				memcpy(s_p10_racer.position,
			           s_p10_inst.position,
			           sizeof s_p10_racer.position);
		}

		/*
		 * THE POSE COMES FROM THE SAME .ROT skl_build_pose used, just
		 * expanded into the frame racerdraw_bind_skeleton wants. The
		 * remap is applied inside bind_skeleton, not here - applying
		 * it twice would swap the wheels with the forks.
		 */
		memset(&frame, 0, sizeof frame);
		if (s_p10_pose.loaded)
			anim_frame_from_pose(&frame, &s_p10_pose);
		racerdraw_bind_skeleton(&s_p10_racer, &s_p10_skl, &frame);

		memset(&s_p10_drawSet, 0, sizeof s_p10_drawSet);
		s_p10_drawSet.skl       = &s_p10_skl;
		s_p10_drawSet.prims     = s_p10_inst.prims;
		s_p10_drawSet.primCount = s_p10_inst.primCount;

		for (w = 0; w < P10_WHEELS; w++) {
			s_p10_drawSet.wheelOk[w]        = s_p10_wheelOk[w];
			s_p10_drawSet.wheelMesh[w]      = &s_p10_wheelMesh[w];
			s_p10_drawSet.wheelPrims[w]     = s_p10_wheelInst[w].prims;
			s_p10_drawSet.wheelPrimCount[w] = s_p10_wheelInst[w].primCount;
		}

		/*
		 * lod 0, exactly as 0x0044f231 pushes it. NO TRACK: this page
		 * has none loaded, and it does not need one - the original
		 * clears g_nRacerDrawFx on entry, so the shading pass that
		 * would read it never runs here.
		 */
		racerdraw_racer(&s_p10_racer, &s_p10_drawSet, s_p10_view, pos,
		                0, NULL, NULL);
	}
	queue_walk(drawscene_dispatch);
}

/* ORIGIN  MenuPage10_BikeSelect_Tick @ 0x0044ed30, the stat-bar
 *         tween - the drawn shrink for one bar, mid-tween. */
static int p10_bar_shrink(int bar, double u)
{
	double blend;

	if (!s_p10_tweening)
		return s_p10_barTo[bar];

	/*
	 * A SINE EASE OVER ONE SECOND, and this is the original's
	 * arithmetic exactly. It multiplies and subtracts its way to
	 * (sin(u*pi - pi/2) + 1) * 0.5 at 0x0044f482..0x0044f496, from
	 * four .rdata doubles: 0x004d0c10 pi, 0x004d0c20 pi/2,
	 * 0x004d0c28 1.0, 0x004d0c00 0.5 [verified]. The 1000 ms is the
	 * two timeGetTime stores.
	 */
	blend = (sin(u * MR_PI - MR_HALF_PI) + 1.0) / 2.0;
	return s_p10_barFrom[bar] +
	       (int)((s_p10_barTo[bar] - s_p10_barFrom[bar]) * blend);
}

/*
 * Which way the target is travelling this frame: +1 rising toward
 * +limit, -1 falling toward -limit, 0 not moving.
 *
 * THE ORIGINAL TESTS THE COSINE'S BIT PATTERN AS AN INTEGER: TEST
 * EAX,EAX/JG at 0x0044ee8e for positive, CMP EAX,0x80000000/JBE at
 * 0x0044eea4 for negative - the latter falling through only when
 * EAX is strictly above 0x80000000 unsigned, which is a negative
 * float with a non-zero magnitude. So BOTH reject a zero cosine,
 * -0.0 included, and a camera exactly side-on does nothing that
 * frame. That is the guard, not an oversight to tidy up: every
 * branch below divides by the cosine.
 *
 * The sign flip is what keeps the pan going the same way on screen
 * while the camera orbits behind the bike.
 */
static int p10_pan_dir(float c)
{
	if (c > 0.0f)
		return -s_p10_spinDir;
	if (c < 0.0f)
		return s_p10_spinDir;
	return 0;
}

/*
 * ORIGIN  MenuPage10_BikeSelect_Tick @ 0x0044ed30, at 0x0044eeab
 *
 * One frame of the pan, 0x0044eeab (rising) and 0x0044ef35
 * (falling). The two are mirror images, and neither is obviously
 * correct on its own - what makes them so is a shared invariant:
 *
 *     target.z = -(target.x / cos) * sin
 *
 * the line through the origin along the camera's right vector
 * (cos, 0, -sin). Nothing writes it in that form; both branches
 * accumulate matching increments into x and z, and every store
 * satisfies it, snaps included. See menu-pages.txt 2.7.4.6.1.
 */
static void p10_pan_step(int dir, float s, float c)
{
	float limit = (float)fabs(P10_PAN_LIMIT * c);
	float step  = (float)fabs(P10_PAN_STEP  * c);

	if (dir > 0) {
		if (limit <= s_p10_target[0]) {
			s_p10_target[0] = -limit;
			s_p10_target[2] = (limit / c) * s;
			s_p10_spinOffset = 0;
		} else {
			s_p10_target[0] += step;
			s_p10_target[2] += (step / -c) * s;
		}
	} else {
		if (-limit >= s_p10_target[0]) {
			s_p10_target[0] = limit;
			s_p10_target[2] = (limit / -c) * s;
			s_p10_spinOffset = 0;
		} else {
			s_p10_target[0] -= step;
			s_p10_target[2] -= (step / -c) * s;
		}
	}
}

/* ORIGIN  MenuPage10_BikeSelect_Tick @ 0x0044ed30 */
static void page10_tick(void)
{
	double u = 1.0;
	int    act;
	int    i;
	int    panDir;
	float  camSin, camCos;

	if (!s_p10_ok)
		return;

	/*
	 * THE VIEW IS BUILT FROM THE TARGET AS IT STANDS NOW, before
	 * this frame moves it - see the note on s_p10_targetDrawn. The
	 * draw runs later, out of menu_draw_page_3d, so the snapshot is
	 * how the lag survives our different call order.
	 */
	s_p10_targetDrawn[0] = s_p10_target[0];
	s_p10_targetDrawn[1] = s_p10_target[1];
	s_p10_targetDrawn[2] = s_p10_target[2];

	camSin = (float)sin(s_p10_camAngle);
	camCos = (float)cos(s_p10_camAngle);
	panDir = p10_pan_dir(camCos);

	/*
	 * THE CAMERA ANGLE ADVANCES ONLY WHEN NOTHING IS SLIDING, and
	 * the timestamp only moves with it - so the spin is frozen for
	 * the length of the pan and then resumes from where it stopped
	 * rather than jumping forward by the pan's duration.
	 *
	 * o THE CLAMP IS 1000, NOT 100 - 0x0044ee43 is MOV EDX,0x3e8,
	 *   so a long gap is capped at one second rather than shrunk to
	 *   a tenth of one. 100 is PAGE 14's number, which is what
	 *   makes the confusion easy.
	 */
	if (s_p10_spinDir == 0) {
		double now  = tm_now();
		double dtMs = (now - s_p10_lastFrame) * 1000.0;

		if (dtMs > 1000.0)
			dtMs = 1000.0;
		if (dtMs < 0.0)
			dtMs = 0.0;
		s_p10_lastFrame = now;
		s_p10_camAngle += (float)(dtMs * P10_SPIN_RATE);
	} else if (panDir != 0) {
		p10_pan_step(panDir, camSin, camCos);
	}

	/*
	 * THE DISPLAYED LIVERY, written every frame at 0x0044efa3.
	 *
	 * THE ORIGINAL WRITES THREE INSTANCES AND NOT THE FOURTH:
	 * the three stores at 0x0044efbb land on racer 1 +0x5fc,
	 * +0x2f4 and +0x478 - the RIDER and the two WHEELS. Instance
	 * 0 at +0x170 is left at whatever Racer_Create gave it.
	 * That is not an oversight: instance 3's livery is what
	 * maybe_Racer_Draw dispatches the pose array on, so it IS
	 * the bike you are looking at. See racer.h.
	 *
	 * UNCONDITIONALLY, INCLUDING THE WHEELS. The page writes all
	 * three flat; it does not repeat Racer_Create's dirt-bike
	 * exception, so a cross bike shows coloured wheels here and
	 * plain ones in a race.
	 *
	 * The bank call stays as the fallback for anything on this
	 * bank that draws with no livery of its own.
	 */
	{
		int liv = p10_livery();

		s_p10_racer.liveryRider      = liv;
		s_p10_racer.liveryFrontWheel = liv;
		s_p10_racer.liveryRearWheel  = liv;
		bank_set_palette(P10_BANK, liv);
	}

	/*
	 * AND THE END OF THE PAN, 0x0044f25b. Only once the offset has
	 * been cleared - that is, only on the way back in - and only
	 * when the target has come back through centre. The original
	 * puts this after the draw; it reads nothing the draw writes,
	 * and our draw takes the snapshot above, so here is the same
	 * place.
	 */
	if (s_p10_spinOffset == 0 && panDir != 0 &&
	    ((panDir > 0 && s_p10_target[0] >= 0.0f) ||
	     (panDir < 0 && s_p10_target[0] <= 0.0f))) {
		s_p10_target[0] = 0.0f;
		s_p10_target[1] = 0.0f;
		s_p10_target[2] = 0.0f;
		s_p10_spinDir   = 0;
	}

	/* choosefo is a still backdrop, not a scrolling tile - it is the
	 * only menu page whose background does not move. */
	sprite_draw(&s_p10_backdrop, SPRITE_MODE_USE_KIND, 100, 0);

	sprite_update_move(&s_p10_title);
	sprite_draw(&s_p10_title, SPRITE_MODE_USE_KIND, 100, 0);
	sprite_update_move(&s_p10_features);
	sprite_draw(&s_p10_features, SPRITE_MODE_USE_KIND, 100, 0);

	if (s_p10_tweening) {
		u = (tm_now() - s_p10_tweenStart) / (P10_TWEEN_MS / 1000.0);
		if (u >= 1.0) {
			u = 1.0;
			s_p10_tweening = 0;
		}
	}

	for (i = 0; i < P10_BARS; i++) {
		sprite_update_move(&s_p10_bar[i]);
		sprite_draw(&s_p10_bar[i], SPRITE_MODE_USE_KIND, 100,
		            p10_bar_shrink(i, u));
	}

	/*
	 * THE GEARBOX IS A PAIR OF THREE-STATE ROWS. Each row has a lit
	 * sprite, a shaded one and a flash, and the SETTING picks which
	 * of the first two is the button's idle art:
	 *
	 *     automatic   auto   + autofla     manusha + manufla
	 *     manual      autosha + autofla    manu    + manufla
	 *
	 * [verified, the Enter branch at 0x0044ea66 - CMP ECX,1 on
	 * 0x004dc0a0, with the manual arm at 0x0044eb9c]. The hover
	 * sprite is the flash in both cases, so hovering does not tell
	 * you which is selected - the shading does.
	 *
	 * [open] OUR ENTER DOES NOT BRANCH. It slides in the automatic
	 * pair unconditionally, and the manual pair is loaded already at
	 * its final Y, so picking manual and re-entering shows those two
	 * rows without the slide. Cosmetic, and only in manual.
	 */
	menu_hit_test();
	menu_clear_buttons();

	s_p10_btnLeft  = menu_add_sprite_button(&s_p10_left,  NULL, VK_LEFT);
	s_p10_btnRight = menu_add_sprite_button(&s_p10_right, NULL, VK_RIGHT);
	s_p10_btnStart = menu_add_sprite_button(&s_p10_start, NULL,
	                                       VK_RETURN);
	s_p10_btnBack  = menu_add_sprite_button(&s_p10_back,  NULL,
	                                       VK_ESCAPE);
	s_p10_btnAuto  = menu_add_sprite_button(g_autoGearbox ? &s_p10_auto
	                                                      : &s_p10_autoSha,
	                                       &s_p10_autoFla, 0);
	s_p10_btnManu  = menu_add_sprite_button(g_autoGearbox ? &s_p10_manuSha
	                                                      : &s_p10_manu,
	                                       &s_p10_manuFla, 0);

	menu_draw_buttons();

	if (s_p10_flash) {
		sprite_draw(s_p10_flash, SPRITE_MODE_USE_KIND, 100, 0);
		s_p10_flash = NULL;
	}

	/*
	 * THE HOLD RUNS BEFORE ACTIVATION IS EVEN READ, so the
	 * page keeps drawing and keeps spinning its bike while it
	 * waits, and a second click cannot start anything. That is
	 * the original's shape: it bails out of the tick at
	 * 0x0044f64a, past the draw and short of the dispatch.
	 */
	if (s_p10_holdUntil > 0.0) {
		menu_clear_activated();
		if (tm_real_now() < s_p10_holdUntil)
			return;
		s_p10_holdUntil = 0.0;
		if (s_p10_holdPage == MENU_PAGE_LOADING)
			menu_goto_page(MENU_PAGE_LOADING);
		else
			mrlog("page10: start -> page 8, lobby, not built\n");
		return;
	}

	act = menu_get_activated();
	if (act < 0)
		return;
	menu_clear_activated();

	if (act == s_p10_btnLeft || act == s_p10_btnRight) {
		int  dir     = (act == s_p10_btnLeft) ? -1 : 1;
		int  limit   = (dir < 0) ? 0 : BIKE_COUNT - 1;
		MR_Sprite *arrow = (dir < 0) ? &s_p10_left  : &s_p10_right;
		MR_Sprite *flash = (dir < 0) ? &s_p10_leftL : &s_p10_rightL;

		/* AT THE END IT DOES NOTHING AT ALL - no sound, no flash. */
		if (g_selectedBike == limit)
			return;

		audio_cue(SND_MENU_MOVE, 0);
		g_selectedBike += dir;

		/*
		 * THE SELECTION CHANGES NOW AND THE PICTURE CATCHES UP.
		 * The offset is the opposite sign, so (selected + offset)
		 * is still the bike that is on screen; the pan clears it
		 * at the screen edge and the livery changes there.
		 *
		 * NO bank_set_palette HERE - the tick writes it every
		 * frame from the offset, which is what the original does
		 * with its three stores at 0x0044efbb. Setting it here as
		 * well would swap the livery before the bike had left.
		 */
		s_p10_spinDir    = dir;
		s_p10_spinOffset = -dir;
		p10_start_tween();
		p10_place(flash, arrow->x, arrow->y);
		s_p10_flash = flash;
		return;
	}

	if (act == s_p10_btnAuto || act == s_p10_btnManu) {
		/*
		 * THE ORIGINAL REBUILDS THE BUTTON LIST HERE and then sets
		 * hoverIndex to 2, START - so choosing a gearbox moves the
		 * highlight onto the thing you are most likely to press
		 * next. Ours rebuilds every frame anyway, so only the flag
		 * and the hover need setting.
		 */
		g_autoGearbox = (act == s_p10_btnAuto);
		menu_set_hover(s_p10_btnStart);
		return;
	}

	if (act == s_p10_btnBack) {
		menu_begin_transition(MENU_PAGE_RACE);
		return;
	}

	if (act == s_p10_btnStart) {
		/*
		 * TWO CUES AND THEN A WAIT, and the wait is the point.
		 * The original plays the bike's own engine sample - 0x2b
		 * or 0x2c, chosen by PersoAnim - then letsgo at 0x58, and
		 * STAYS ON THIS PAGE until they have finished. See
		 * s_p10_holdUntil for how, and for why leaving at once
		 * silences them.
		 *
		 * PERSOANIM, CARAC 72 - NOT MotoType, carac 0. The test
		 * is at 0x0044f5d1: [EAX*4 + 0x509398] with EAX =
		 * 73*bike, and 0x509398 - 0x509278 (the g_bikeCarac
		 * base) is 0x120, which is 72 dwords in. The 73*bike is
		 * the LEA pair just above it. Then MOV EAX,0x2c /
		 * CMP ECX,1 / ADC EAX,-1 at 0x0044f5d8..0x0044f5e0
		 * leaves 0x2c when PersoAnim is 0 and 0x2b otherwise.
		 *
		 * MOTOTYPE LOOKS LIKE IT WOULD DO AND SILENTLY WILL NOT:
		 * it is 2 on every Speed bike and 1 on every Motocross
		 * one, NEVER 0, so a == 0 test on it picks goktm for
		 * both disciplines and the road bikes get the
		 * two-stroke. PersoAnim is 0 for Speed and 1 for
		 * Motocross [verified, all 32 CARAC records in SPEED,
		 * SPEEDPKT, KTMCROSS and KTMPKT]. The same field picks
		 * the race sample bank - audio-cues.txt 5.5.
		 */
		const MR_BikeCarac *b = bike_at(g_selectedBike);
		int bikeCue = (b && b->persoAnim == 0)
		                  ? SND_BIKE_START_B : SND_BIKE_START_A;
		int holdMs  = audio_cue_length_ms(bikeCue);
		int voiceMs = audio_cue_length_ms(SND_VOICE_LETSGO);

		audio_cue(bikeCue, 0);
		audio_cue(SND_VOICE_LETSGO, 0);

		/*
		 * SOLO GOES TO PAGE 1 AND A NETWORK GAME TO PAGE 8
		 * [verified, the two arms at 0x0044f900]. Page 1 is
		 * built - it paints the loading screen and then hands
		 * over - so that arm is a real transition. Page 8, the
		 * network lobby, is not, and the hold expiry says so.
		 *
		 * GotoPage, NOT a transition - so no glass. Both arms call
		 * the same GotoPage, but the solo one at 0x0044f922 sets
		 * nothing first, while the network arm at 0x0044f909 does
		 * MOV ESI,1 / MOV EDI,ESI - and EDI is the flag the tail
		 * tests before Menu_BeginTransition. BACK, case 3 at
		 * 0x0044f934, sets EDI too [verified]. So
		 * menu_begin_transition here would shatter on the way into
		 * a race, which retail does not do.
		 */
		if (voiceMs > holdMs)
			holdMs = voiceMs;
		s_p10_holdPage  = g_soloMode ? MENU_PAGE_LOADING : 8;
		s_p10_holdUntil = tm_real_now() + holdMs / 1000.0;
		mrlog("page10: start, holding %d ms (cue 0x%02x %d ms, "
		      "letsgo %d ms)\n", holdMs, bikeCue,
		      audio_cue_length_ms(bikeCue), voiceMs);
		return;
	}
}

/* ================================================================ */
/* Page 9 - results, the TOP 5 screen                               */
/* ================================================================ */

/*
 * The page table record is at 0x004dbbc8.
 *
 * THIS IS WHERE A QUIT RACE LANDS, not the main menu. The button set
 * is what tells the three modes apart, and it is built in Enter -
 * the ORDER of the list is the activated index, so a button that is
 * conditionally absent shifts the ones after it.
 *
 * EVERY BUTTON IS PARKED OFF-SCREEN RIGHT BY ENTER and slid home by
 * Tick. Enter puts them at x 640 or 840 - past the right edge of a
 * 640-wide screen - and only Tick knows where they belong, so
 * reading Enter alone gives a page with no visible buttons at all.
 *
 * WHAT IS NOT HERE YET: the typed initials. The rows, their place
 * numbers and the button destinations are all in - bar SAVE, which
 * wants page 21, and NEXT RACE's page-5 arm, which wants
 * 0x004dc07c. See menu-pages.txt 2.9.
 */

#define P9_RES_BACKDROP    1     /* paralax2.lez, the checkerboard */
#define P9_RES_TOPTITLE   18     /* toptitle.lez                   */
#define P9_RES_BESTLAP    19     /* bestlap.lez                    */
#define P9_RES_BESTRACE   20     /* bestRace.lez                   */
#define P9_RES_CONTINUE   77     /* continue.lez                   */
#define P9_RES_SAVE       78     /* Btsave.lez                     */
#define P9_RES_ABORT      80     /* BtAbort.lez                    */
#define P9_RES_NEXTRACE   82     /* BtNextRa.lez                   */
#define P9_RES_RESETTA   201     /* resetTA.lez                    */

/*
 * Enter's parking spots. THE BUTTONS COME FROM THE RIGHT AND
 * bestRace COMES FROM THE LEFT - it parks at x -150, not 840,
 * which is why it is the one sprite that needs its own.
 */
#define P9_PARK_FAR      840     /* 0x348  */
#define P9_PARK_NEAR     640     /* 0x280  */
#define P9_PARK_LEFT    (-150)   /* 0xffffff6a */

/* The home positions, from Tick's Sprite_StartMove targets. */
#define P9_TOPTITLE_X      0
#define P9_TOPTITLE_Y      0
#define P9_BESTRACE_X    430     /* 0x1ae */
#define P9_BESTRACE_Y    135     /* 0x87  */

/*
 * bestlap IS NEVER MOVED - Enter places it and Tick only draws
 * it [verified, 0x0044d083]. Leaving it at the origin, as a
 * page built from Tick alone would, puts it in the top left
 * corner instead of down beside its time.
 */
#define P9_BESTLAP_X      30     /* 0x1e  */
#define P9_BESTLAP_Y     400     /* 0x190 */

/*
 * The two captions, both drawn rather than blitted.
 *
 * THE TRACK NAME IS PLACED AGAINST toptitle, not against the
 * screen: Tick reads that sprite's position back with
 * Sprite_GetPosition at 0x0044da7e and adds this offset, so
 * the name travels with the banner as it slides in.
 */
#define P9_TRACK_DX      450     /* 0x1c2, ADD EBX at 0x0044db01 */
#define P9_TRACK_DY       56     /* 0x38,  ADD EBP at 0x0044dafa */
#define P9_LAPS_X         33     /* 0x21  */
#define P9_LAPS_Y        103     /* 0x67  */

/*
 * THE FIVE ROWS. y walks 0x2c a row from 0xb6, the initials sit
 * at 0x6e and the time at 0x1e0 [verified, the loop at
 * 0x0044dc8a].
 */
#define P9_ROW_FIRST_Y   182     /* 0xb6  */
#define P9_ROW_STEP       44     /* 0x2c  */
#define P9_ROW_NAME_X    110     /* 0x6e  */
#define P9_ROW_TIME_X    480     /* 0x1e0 */

/*
 * EACH ROW HAS TWO SPRITES UNDER IT, drawn before the text.
 * ligne is the grey rule the row sits on and the number is the
 * stylised place; the rule goes down first, so the number
 * lands on top of it.
 */
#define P9_RES_LIGNE     104     /* ligne.lez    */
#define P9_RES_NUMBER1    96     /* number1..5   */
#define P9_LIGNE_X        30     /* 0x1e, y - 12 */
#define P9_LIGNE_DY     (-12)
#define P9_NUMBER_X       28     /* 0x1c, y - 14 */
#define P9_NUMBER_DY    (-14)

/* The best lap sits on one line with bestlap's banner. */
#define P9_BLAP_TIME_X   180     /* 0xb4  */
#define P9_BLAP_NAME_X   330     /* 0x14a */
#define P9_BLAP_Y        400

/*
 * THREE FONTS, AND THEY ARE NOT INTERCHANGEABLE. The page asks
 * Font_Get for three different ids and structs/MR_Font.txt
 * section 5 names all three:
 *
 *   0x1f  31  DataDiskMenu  Direct             the captions
 *   0x0c  12  Messages      AntiAlias8Transp   the rows
 *   0x09   9  MenuLink      AntiAlias8Transp   the best lap
 *
 * THE CAPTION ONE IS THE BOLD FACE. Drawing the track name in
 * Messages instead makes it thin and, over the yellow half of
 * the banner, nearly unreadable - which is two faults at once
 * and reads as one.
 *
 * DIRECT AND AntiAlias8Transp ARE DIFFERENT CLASSES, so the
 * two cannot share an MR_Font: a Direct face builds its LUT
 * from fore/back and an AntiAlias8Transp face treats its
 * pixels as coverage.
 */
#define P9_FONT_CAPTION  31     /* DataDiskMenu, Direct   */
#define P9_FONT_ROW      12     /* Messages               */
#define P9_FONT_BLAP      9     /* MenuLink               */

/* Three colour pairs, read out of .rdata as 3-byte RGB. */
static const unsigned char kP9TrackFore[3] = {   0,   0,   0 };
static const unsigned char kP9TrackBack[3] = { 255, 255,   0 };
static const unsigned char kP9LapsFore[3]  = { 255, 255,   0 };
static const unsigned char kP9LapsBack[3]  = {   0, 139,  89 };
/* Enter sets the row font white on white at 0x0044d02a. */
static const unsigned char kP9RowFore[3]   = { 255, 255, 255 };
static const unsigned char kP9RowBack[3]   = { 255, 255, 255 };
#define P9_ROW_CHAMP_Y   437     /* 0x1b5 */
#define P9_ROW_SOLO_Y    429     /* 0x1ad */
#define P9_COL_RIGHT     496     /* 0x1f0 */
#define P9_COL_MID       296     /* 0x128 */
#define P9_COL_LEFT      150     /* 0x96  */

/* Enter slides the title over a second; Tick slides the rest in
 * 0x3f333333 of one. */
#define P9_TITLE_SECS   1.0f
#define P9_SLIDE_SECS   0.7f

static MR_Sprite s_p9_backdrop;
static MR_Sprite s_p9_toptitle;
static MR_Sprite s_p9_bestlap;
static MR_Sprite s_p9_bestrace;
static MR_Sprite s_p9_continue;
static MR_Sprite s_p9_save;
static MR_Sprite s_p9_abort;
static MR_Sprite s_p9_nextrace;
static MR_Sprite s_p9_resetta;
static int       s_p9_ok;
static int       s_p9_slid;      /* Tick has started the button slide */
static int       s_p9_btnFirst, s_p9_btnSecond, s_p9_btnThird;
static MR_Font   s_p9_font;      /* 31, the captions */
static MR_Font   s_p9_fontRow;   /* 12, the rows     */
static MR_Font   s_p9_fontBlap;  /*  9, the best lap */
static int       s_p9_fontOk;
static int       s_p9_fontRowOk;
static int       s_p9_fontBlapOk;
static MR_Sprite s_p9_ligne;
static MR_Sprite s_p9_number[RACE_RESULT_ROWS];
static int       s_p9_rowArtOk;

/*
 * ORIGIN  MenuPage09_Results_Enter @ 0x0044cfd0, at 0x0044d2c2
 *
 * PRACTICE SHOWS TWO BUTTONS AND A SINGLE RACE SHOWS ONE, out of the
 * same branch - the mode test is on resetTA alone. The original also
 * gates it on the track's default lap count:
 *
 *     CMP [trackList + track*0x130 + 0x12c], g_nRaceLaps
 *     JL  skip                             0x0044d29d [verified]
 *
 * so RESET TIME ATTACK is offered only while the chosen distance is
 * no longer than the track default - the same test page 6 uses to
 * offer the time-attack toggle at all.
 *
 * +0x12c is MR_Track.laps, which tracks.c carries, so both halves of
 * the original's test are reproduced here.
 *
 * NO TRACK MEANS NO BUTTON. The original indexes the list without a
 * null check; ours can be asked before tracks_load, and offering a
 * time-attack reset for a track we cannot name is worse than hiding
 * it.
 */
static int page9_wants_resetta(void)
{
	const MR_Track *t = track_at(g_selectedTrack);

	return g_selectedMode == 0 && t && t->laps >= g_raceLaps;
}

/*
 * ORIGIN  NONE - ours. The original tests the pair inline in
 *         MenuPage09_Results_Enter @ 0x0044cfd0, at 0x0044d139
 *
 * [open] 0x00509ba8 < 0x005083b4, the championship race index
 * against its count. Neither is reconstructed, so NEXT RACE and SAVE
 * are always offered; on the last race of a championship retail
 * shows ABORT alone. The record those two index is settled - see
 * docs/structs/MR_ChampEntry.txt - so what is missing is the pair of
 * globals, not the layout.
 */
static int page9_wants_nextrace(void)
{
	return 1;
}

/* ORIGIN  MenuPage09_Results_Enter @ 0x0044cfd0 */
static void page9_enter(void)
{
	int champ = (g_selectedMode == 2);
	int i;

	s_p9_ok   = 1;
	s_p9_slid = 0;

	s_p9_ok &= load_res(&s_p9_backdrop, P9_RES_BACKDROP, 0, 0,
	                    SPRITE_ANCHOR_NONE, "page9");
	s_p9_ok &= load_res(&s_p9_bestlap, P9_RES_BESTLAP,
	                    P9_BESTLAP_X, P9_BESTLAP_Y,
	                    SPRITE_ANCHOR_NONE, "page9");
	s_p9_ok &= load_res(&s_p9_bestrace, P9_RES_BESTRACE,
	                    P9_PARK_LEFT, P9_BESTRACE_Y,
	                    SPRITE_ANCHOR_NONE, "page9");

	/*
	 * BY ID. The class and the face both come out of FONTS.BPT,
	 * so neither is named here - and fontdef_open adds EVERY face
	 * a font declares, which a hand-written font_add_face call
	 * cannot know to do.
	 */
	s_p9_fontOk     = fontdef_open(&s_p9_font,     P9_FONT_CAPTION);
	s_p9_fontRowOk  = fontdef_open(&s_p9_fontRow,  P9_FONT_ROW);
	s_p9_fontBlapOk = fontdef_open(&s_p9_fontBlap, P9_FONT_BLAP);
	if (!s_p9_fontOk || !s_p9_fontRowOk || !s_p9_fontBlapOk)
		mrlog_err("page9:  a font did not load (%d %d %d)\n",
	              s_p9_fontOk, s_p9_fontRowOk,
	              s_p9_fontBlapOk);

	/* The rule and the five place numbers. */
	s_p9_rowArtOk = load_res(&s_p9_ligne, P9_RES_LIGNE, 0, 0,
	                         SPRITE_ANCHOR_NONE, "page9");
	for (i = 0; i < RACE_RESULT_ROWS; i++) {
		s_p9_rowArtOk &= load_res(&s_p9_number[i],
		                          P9_RES_NUMBER1 + i, 0, 0,
		                          SPRITE_ANCHOR_NONE,
		                          "page9");
	}

	/*
	 * THE TITLE IS THE ONE THING ENTER ANIMATES. It is parked and
	 * started here rather than in Tick, and Tick then waits for
	 * this move to finish before starting the others - that is
	 * what staggers the page.
	 */
	s_p9_ok &= load_res(&s_p9_toptitle, P9_RES_TOPTITLE,
	                    P9_PARK_FAR, P9_TOPTITLE_Y,
	                    SPRITE_ANCHOR_NONE, "page9");
	if (s_p9_ok) {
		int t[2];

		t[0] = P9_TOPTITLE_X;
		t[1] = P9_TOPTITLE_Y;
		sprite_start_move(&s_p9_toptitle, t, P9_TITLE_SECS, 1,
		                  SPRITE_ANCHOR_NONE);
	}

	if (champ) {
		s_p9_ok &= load_res(&s_p9_nextrace, P9_RES_NEXTRACE,
		                    P9_PARK_FAR, P9_ROW_CHAMP_Y,
		                    SPRITE_ANCHOR_NONE, "page9");
		s_p9_ok &= load_res(&s_p9_abort, P9_RES_ABORT,
		                    P9_PARK_NEAR, P9_ROW_CHAMP_Y,
		                    SPRITE_ANCHOR_NONE, "page9");
		s_p9_ok &= load_res(&s_p9_save, P9_RES_SAVE,
		                    P9_PARK_NEAR, P9_ROW_CHAMP_Y,
		                    SPRITE_ANCHOR_NONE, "page9");
	} else {
		s_p9_ok &= load_res(&s_p9_continue, P9_RES_CONTINUE,
		                    P9_PARK_FAR, P9_ROW_SOLO_Y,
		                    SPRITE_ANCHOR_NONE, "page9");
		s_p9_ok &= load_res(&s_p9_resetta, P9_RES_RESETTA,
		                    P9_PARK_NEAR, P9_ROW_SOLO_Y,
		                    SPRITE_ANCHOR_NONE, "page9");
	}

	/* The menu SOUND bank is restored by menu_set_page, which runs
	   before this - see the note in its body. */

	/* AND THE STANDINGS HAVE NOW BEEN SEEN.  0x0044d4fc, the
	   last thing MenuPage09_Results_Enter does. */
	s_menu_resultsSeen = 1;

	mrlog("page9:  results - %s, mode %d, %s\n",
	      s_p9_ok ? "loaded" : "INCOMPLETE", g_selectedMode,
	      champ ? "championship" : (g_selectedMode == 0 ? "practice"
	                                                    : "single"));
}

/*
 * ORIGIN  MenuPage09_Results_Tick @ 0x0044d540, at 0x0044de01
 *
 * mm'ss''cc - the original's format string at 0x004e95a4 is
 * "%02d'%02d''%02d", so minutes, seconds, HUNDREDTHS. The hundredths
 * come from (ms % 1000) / 10, so the last digit of the
 * millisecond is dropped rather than rounded.
 */
static void page9_format_time(char *out, int max, int ms)
{
	int secs = ms / 1000;

	_snprintf(out, max, "%02d'%02d''%02d",
	          (secs / 60) % 60, secs % 60, (ms % 1000) / 10);
	out[max - 1] = 0;
}

/*
 * ORIGIN  MenuPage09_Results_Tick @ 0x0044d540, at 0x0044d846
 *
 * THE TOP FIVE, and the BEST LAP under them.
 *
 * The rows come out of BESTSCOR.DAT, not out of the race -
 * which is why they survive race_free having already run by
 * the time this page is up. An empty place draws nothing at
 * all rather than a zero time, because the file marks it with
 * 0x7f7f7f7f and race_result_row returns NULL for it.
 *
 * THE ROW SHOWS THE RACE TIME, NOT THE LAP TIME. Both are in
 * the record and the page draws +0xf1; the lap time at +0xf5
 * is what the BEST LAP line below uses.
 *
 * THE RULE AND THE PLACE NUMBERS ARE THE ORIGINAL'S OWN ART.
 * Its sprite 0x00504a20 is resource 104, ligne, and the array
 * at 0x00504a00 starts at resource 96, number1 - both from
 * (addr - g_menuObjects) / 4 [verified].
 */
static void page9_draw_rows(void)
{
	const MR_RaceResultRow *row;
	char                    buf[32];
	int                     i;
	int                     y;

	if (!s_p9_fontRowOk)
		return;

	for (i = 0; i < RACE_RESULT_ROWS; i++) {
		row = race_result_row(i);
		if (!row)
			continue;

		y = P9_ROW_FIRST_Y + i * P9_ROW_STEP;

		if (s_p9_rowArtOk) {
			int pos[2];

			pos[0] = P9_LIGNE_X;
			pos[1] = y + P9_LIGNE_DY;
			sprite_set_position(&s_p9_ligne, pos,
			                    SPRITE_ANCHOR_NONE);
			sprite_draw(&s_p9_ligne, SPRITE_MODE_USE_KIND,
			            100, 0);

			pos[0] = P9_NUMBER_X;
			pos[1] = y + P9_NUMBER_DY;
			sprite_set_position(&s_p9_number[i], pos,
			                    SPRITE_ANCHOR_NONE);
			sprite_draw(&s_p9_number[i],
			            SPRITE_MODE_USE_KIND, 100, 0);
		}

		font_draw_text(&s_p9_fontRow, P9_ROW_NAME_X, y,
		               row->name, 0);
		page9_format_time(buf, (int)sizeof buf, row->raceMs);
		font_draw_text(&s_p9_fontRow, P9_ROW_TIME_X, y, buf,
		               0);
	}

	row = race_best_lap_record();
	if (row && s_p9_fontBlapOk) {
		page9_format_time(buf, (int)sizeof buf, row->lapMs);
		font_draw_text(&s_p9_fontBlap, P9_BLAP_TIME_X,
		               P9_BLAP_Y, buf, 0);
		font_draw_text(&s_p9_fontBlap, P9_BLAP_NAME_X,
		               P9_BLAP_Y, row->name, 0);
	}
}

/*
 * ORIGIN  MenuPage09_Results_Tick @ 0x0044d540, at 0x0044db3a
 *
 * THE TRACK NAME AND THE LAP COUNT - text built into a scratch
 * buffer, not art. The name comes out of the track list with
 * UNDERSCORES TURNED INTO SPACES, which is why SPEED_BAY reads as
 * SPEED BAY on screen [verified, CMP CL,0x5f at 0x0044dad3].
 *
 * [open] The lap caption should also gain REVERSE or POCKET when
 * those are set, and the strings for it are located: "%s_" at
 * 0x004e95b4, "Pocket" at 0x004e95b8, "Reverse" at 0x004e95c4, all
 * read between 0x0044dbf5 and 0x0044dc26. The caption format at
 * 0x004e95cc is "%d %s " - with a trailing space, which ours omits.
 */
static void page9_draw_captions(void)
{
	const MR_Track *t;
	char            buf[64];
	int             i;

	if (!s_p9_fontOk)
		return;

	t = track_at(g_selectedTrack);
	if (t) {
		_snprintf(buf, sizeof buf, "%s", t->name);
		buf[sizeof buf - 1] = 0;
		for (i = 0; buf[i]; i++) {
			if (buf[i] == '_')
				buf[i] = ' ';
			buf[i] = (char)toupper((unsigned char)buf[i]);
		}
		/*
		 * BLACK ON THE YELLOW BANNER. The original pushes
		 * 0x004f0de8 then 0x004f0dd8 at 0x0044daec and
		 * 0x0044daf3 and calls the font's vtable slot 0 -
		 * CALL [EDX] at 0x0044dafd, EDX being [this]. The two
		 * read 00 00 00 and ff ff 00 out of the colour table
		 * at 0x004f0dd0 [verified]. Left at the font's own
		 * white it is all but invisible against the banner it
		 * sits on.
		 */
		font_set_colors(&s_p9_font, kP9TrackFore,
		                kP9TrackBack);
		/* Against the banner, so it slides in with it. */
		font_draw_text(&s_p9_font,
		               s_p9_toptitle.x + P9_TRACK_DX,
		               s_p9_toptitle.y + P9_TRACK_DY, buf, 0);
	}

	/* Fore ff ff 00, back 00 8b 59 - two calls on font 0x1f, at
	   0x0044dc45 and 0x0044dc5e, each pushing its triple. */
	font_set_colors(&s_p9_font, kP9LapsFore, kP9LapsBack);
	_snprintf(buf, sizeof buf, "%d %s", g_raceLaps,
	          (g_raceLaps == 1) ? "LAP" : "LAPS");
	buf[sizeof buf - 1] = 0;
	font_draw_text(&s_p9_font, P9_LAPS_X, P9_LAPS_Y, buf, 0);
}

/* ORIGIN  MenuPage09_Results_Enter @ 0x0044cfd0, the slide-in */
static void page9_slide_in(void)
{
	p6_move(&s_p9_bestrace, P9_BESTRACE_X, P9_BESTRACE_Y,
	        P9_SLIDE_SECS);

	if (g_selectedMode == 2) {
		p6_move(&s_p9_nextrace, P9_COL_RIGHT, P9_ROW_CHAMP_Y,
		        P9_SLIDE_SECS);
		p6_move(&s_p9_abort, P9_COL_MID, P9_ROW_CHAMP_Y,
		        P9_SLIDE_SECS);
		p6_move(&s_p9_save, P9_COL_LEFT, P9_ROW_CHAMP_Y,
		        P9_SLIDE_SECS);
	} else {
		p6_move(&s_p9_continue, P9_COL_RIGHT, P9_ROW_SOLO_Y,
		        P9_SLIDE_SECS);
		p6_move(&s_p9_resetta, P9_COL_MID, P9_ROW_SOLO_Y,
		        P9_SLIDE_SECS);
	}
	s_p9_slid = 1;
}

/*
 * ORIGIN  MenuPage09_Results_Tick @ 0x0044d540, at 0x0044e36f
 *
 * WHERE EACH BUTTON GOES, and g_soloMode picks the pair.
 *
 * CONTINUE AND RESET TIME ATTACK BOTH LAND ON PAGE 6. That is
 * not a simplification: the difference between them is that
 * resetTA DELETES the saved time-attack lap first. The original
 * builds the timatak path at 0x0044e305 and calls crt_remove -
 * DeleteFileA - on it at 0x0044e3db. Same destination, different
 * side effect.
 */
static void page9_activate(int act)
{
	int champ = (g_selectedMode == 2);

	menu_clear_activated();

	/*
	 * 0x0044e36f  NEXT RACE goes to the lobby and ABORT to the
	 * session list, keyed on the activated INDEX rather than on
	 * the sprite drawn. ABORT broadcasts 0x67 first, 0x0044e3a1,
	 * so peers drop our bike.
	 *
	 * [open] the buttons are still the solo set - the network
	 * layout in MenuPage09_Results_Enter is not reconstructed.
	 */
	if (!g_soloMode) {
		if (act == s_p9_btnFirst) {
			mrlog("page9:  net -> page 8, lobby\n");
			menu_begin_transition(MENU_PAGE_LOBBY);
		} else if (act == s_p9_btnSecond) {
			mrlog("page9:  net ABORT -> page 7\n");
			netplay_send_leave();
			netplay_leave_room();
			menu_begin_transition(MENU_PAGE_SESSION);
		}
		return;
	}

	if (champ) {
		if (act == s_p9_btnSecond) {
			/* ABORT - the only arm that reaches the main
			   menu, and only in a championship. */
			mrlog("page9:  ABORT -> page 3\n");
			menu_begin_transition(MENU_PAGE_MAIN);
			return;
		}
		if (act == s_p9_btnThird) {
			/* SAVE - page 0x15, which is not built. */
			mrlog("page9:  SAVE -> page 21, not built\n");
			return;
		}
		if (act == s_p9_btnFirst) {
			/*
			 * NEXT RACE. Page 5 when 0x004dc07c is clear
			 * and page 6 otherwise; that global is not
			 * reconstructed, so this takes the race-select
			 * arm - which is what a championship in progress
			 * does, and what was observed.
			 * [hole - 0x004dc07c]
			 */
			mrlog("page9:  NEXT RACE -> page 6\n");
			menu_begin_transition(MENU_PAGE_RACE);
			return;
		}
		return;
	}

	if (act == s_p9_btnSecond) {
		mrlog("page9:  RESET TIME ATTACK -> page 6\n");
		race_ghost_erase();
		menu_begin_transition(MENU_PAGE_RACE);
		return;
	}
	if (act == s_p9_btnFirst) {
		mrlog("page9:  CONTINUE -> page 6\n");
		menu_begin_transition(MENU_PAGE_RACE);
		return;
	}
}

/* ORIGIN  MenuPage09_Results_Tick @ 0x0044d540 */
static void page9_tick(void)
{
	int champ = (g_selectedMode == 2);

	if (!s_p9_ok)
		return;

	/*
	 * THE STAGGER: Tick waits for the title's move to end before
	 * starting everything else. The original tests the sprite's
	 * moveActive at +0x10 - MOV ECX,[EAX+0x10] / TEST at
	 * 0x0044d55a, on resource 18 - then the one-shot at 0x0044d565,
	 * and sets the one-shot at 0x0044d59b [verified].
	 *
	 * THREE GATES, NOT ONE. g_nSoloMode is tested first at
	 * 0x0044d543, and a network game takes a PARALLEL stagger block
	 * at 0x0044d67c with its own sprites, resources 24 and 23. That
	 * half has no counterpart here because the net pages are not
	 * built.
	 *
	 * AND THE ONE-SHOT IS SHARED, NOT PAGE 9'S. 0x00542380 is the
	 * engine's stagger flag: pages 3, 6, 7, 8, 9, 16, 17, 18, 23 and
	 * 25 all clear it on Enter and set it in Tick. s_p9_slid being
	 * page-local is a deliberate divergence - with one global, two
	 * pages that staggered without an Enter between them would
	 * interfere.
	 */
	if (!s_p9_slid && !s_p9_toptitle.moveActive)
		page9_slide_in();

	sprite_update_wave(&s_p9_backdrop, -1, 1);
	sprite_draw_wave(&s_p9_backdrop);

	sprite_update_move(&s_p9_toptitle);
	sprite_draw(&s_p9_toptitle, SPRITE_MODE_USE_KIND, 100, 0);
	sprite_update_move(&s_p9_bestrace);
	sprite_draw(&s_p9_bestrace, SPRITE_MODE_USE_KIND, 100, 0);
	sprite_update_move(&s_p9_bestlap);
	sprite_draw(&s_p9_bestlap, SPRITE_MODE_USE_KIND, 100, 0);

	page9_draw_captions();
	page9_draw_rows();

	/*
	 * THE LIST ORDER IS THE ACTIVATED INDEX, so the conditional
	 * buttons have to be added in the original's order and skipped
	 * rather than hidden - dropping one shifts every index after it.
	 */
	menu_clear_buttons();
	s_p9_btnFirst = s_p9_btnSecond = s_p9_btnThird = -1;

	if (champ) {
		if (page9_wants_nextrace()) {
			sprite_update_move(&s_p9_nextrace);
			s_p9_btnFirst = menu_add_sprite_button(&s_p9_nextrace,
			                                       NULL, 0);
		}
		sprite_update_move(&s_p9_abort);
		s_p9_btnSecond = menu_add_sprite_button(&s_p9_abort, NULL,
		                                        VK_ESCAPE);
		if (page9_wants_nextrace()) {
			sprite_update_move(&s_p9_save);
			s_p9_btnThird = menu_add_sprite_button(&s_p9_save,
			                                       NULL, 0);
		}
	} else {
		sprite_update_move(&s_p9_continue);
		s_p9_btnFirst = menu_add_sprite_button(&s_p9_continue, NULL,
		                                       VK_RETURN);
		if (page9_wants_resetta()) {
			sprite_update_move(&s_p9_resetta);
			s_p9_btnSecond = menu_add_sprite_button(&s_p9_resetta,
			                                        NULL, 0);
		}
	}

	menu_hit_test();
	menu_draw_buttons();

	if (menu_get_activated() >= 0)
		page9_activate(menu_get_activated());
}

/* ORIGIN  MenuPage09_Results_Leave @ 0x0044d510 */
static void page9_leave(void)
{
	int i;

	sprite_free(&s_p9_backdrop);
	sprite_free(&s_p9_toptitle);
	sprite_free(&s_p9_bestlap);
	sprite_free(&s_p9_bestrace);
	sprite_free(&s_p9_continue);
	sprite_free(&s_p9_save);
	sprite_free(&s_p9_abort);
	sprite_free(&s_p9_nextrace);
	sprite_free(&s_p9_resetta);
	if (s_p9_fontOk)     font_free(&s_p9_font);
	if (s_p9_fontRowOk)  font_free(&s_p9_fontRow);
	if (s_p9_fontBlapOk) font_free(&s_p9_fontBlap);
	s_p9_fontOk = s_p9_fontRowOk = s_p9_fontBlapOk = 0;

	sprite_free(&s_p9_ligne);
	for (i = 0; i < RACE_RESULT_ROWS; i++)
		sprite_free(&s_p9_number[i]);
	s_p9_rowArtOk = 0;
	s_p9_ok   = 0;
	s_p9_slid = 0;
}


/* ================================================================ */
/* Page 11 - options                                                */
/* ================================================================ */

/*
 * The tick is 6316 bytes - 0x00443200 to 0x00444aab - and builds the
 * page as TEXT ROWS through Menu_AddTextButton rather than as
 * sprites. Big, though not the biggest: page 8's net-lobby tick runs
 * to 8406.
 *
 * THE ROW GEOMETRY IS DERIVED, NOT GUESSED. The first row sets its
 * value position to (300, 100) at 0x00443360, and the 16/9 row - the
 * tenth - pushes its LABEL at (50, 280) at 0x00443c5c [verified].
 * Nine gaps over those 180 pixels is 20 a row, and 50 is the label
 * column.
 */
#define P11_RES_BACKDROP   1     /* paralax2.lez, kind 4, the green one */
#define P11_RES_TITLE     60     /* optitre.lez                        */
#define P11_RES_INFO     170     /* btinfo.lez                         */
#define P11_RES_DATADISK 183     /* DATADISK.lez                       */
#define P11_RES_ACCEPT    70     /* accept.lez                         */
#define P11_RES_CONFIG    87     /* BtConfig.lez, beside CONTROLLER    */
#define P11_RES_SLIDER    50     /* featacc.lez - a bike stat bar,     */
                                 /* reused as the volume slider        */

/*
 * THE SLIDER ROWS. Three volumes, and the geometry is read off the
 * master-volume row at 0x00443e85 [verified]:
 *
 *     "-"    text button at x 300, repeatOnHold set
 *     bar    sprite 50 at x 312, EIGHT PIXELS BELOW the text row
 *     "+"    text button at x 400
 *
 * o THE BAR IS DRAWN BY SHRINKING IT, not by scaling it:
 *
 *       shrink = 100 - volume * 100 / max
 *
 *   and Sprite_Draw removes that percentage from the RIGHT. Full
 *   volume is shrink 0, which is the whole sprite. So the slider is
 *   one bitmap clipped, and there is no separate track art.
 */
#define P11_SLIDER_MINUS_X  300
#define P11_SLIDER_BAR_X    312
#define P11_SLIDER_BAR_DY     8
#define P11_SLIDER_PLUS_X   400
#define P11_CONFIG_X        475
#define P11_CONFIG_Y        120

/* Which rows carry a slider: master, wave and CD volume. */
#define P11_ROW_MASTER_VOL 11
#define P11_ROW_WAVE_VOL   12
#define P11_ROW_CD_VOL     13

#define P11_LABEL_X    50
#define P11_VALUE_X   300
#define P11_ROW0_Y    100
#define P11_ROW_STEP   20
#define P11_ROWS       15

/*
 * *** ONE FONT, AND IT IS 12. ***
 *
 * Enter fetches fonts 9 and 12 and colour-sets both, which reads like
 * a label font and a value font. It is not: the tick asks for font 12
 * at EVERY Font_Get - labels and values alike - and never asks for 9
 * [verified: no bare PUSH 0x9 anywhere in the tick's 1851
 * instructions, the three near misses being two packed colours and a
 * 0x94]. Font 9 is set up here and used by something else.
 *
 * o USING 9 FOR THE LABELS MADE THEM BIGGER AND HEAVIER than the
 *   values, which is what it looks like: MenuLink is a chunkier face
 *   than Messages. Two fonts fetched together is not two fonts used.
 *
 * The ids are positional in
 * DATA\FONTS.BPT, and structs/MR_Font.txt section 5 names them:
 *
 *      9  MenuLink   AntiAlias8Transp   FONTES\HIRES\MENULINK.FNT
 *     12  Messages   AntiAlias8Transp   FONTES\HIRES\MSG.FNT
 *
 * The define below is the id; fontdef_open resolves it through
 * FONTS.BPT.
 */
#define P11_FONT_ROW   12    /* Messages */

/*
 * THE ROW LABELS, TRANSCRIBED FROM .data - not observed.
 *
 * They sit at 0x004e0b80..0x004e2118, SIX PER ROW, one per language,
 * English last in each group and in capitals. The runtime table at
 * 0x0053b6xx that the tick reads is filled with pointers into this
 * block, chosen by the language setting - which is the loader that
 * could not be found by chasing writes to the table.
 *
 * o THE SPACE BEFORE THE COLON IS REAL, and THE TRAILING SPACE IS
 *   NOT UNIFORM: most rows end "X : " but the three volume rows end
 *   "X :" with nothing after. Transcribed as found rather than
 *   regularised - the difference is in the data.
 */
static const char *const kP11Label[P11_ROWS] = {
	"DIFFICULTY : ",           /* 0x004e2108 */
	"CONTROLLER : ",           /* 0x004e2018 */
	"DETAIL LEVELS : ",        /* 0x004e1f58 */
	"OPPONENTS : ",            /* 0x004e1ee8 */
	"D3D FILTER : ",           /* ours - 5.2 */
	"VOICES : ",               /* 0x004e1e88 */
	"SOUNDS : ",               /* 0x004e1e48 */
	"MUSIC : ",                /* 0x004e1e08 */
	"SCREEN MODE : ",          /* 0x004e1cf8 */
	"16/9 MODE : ",            /* 0x004e1c8c */
	"WAVE OUTPUT : ",          /* 0x004e1bc8 */
	"MASTER VOLUME :",         /* 0x004e0cb0, no trailing */
	"WAVE VOLUME :",           /* 0x004e0c44, no trailing */
	"CD AUDIO VOLUME :",       /* 0x004e0bd0, no trailing */
	"SESSIONS REFRESH RATE : " /* 0x004e19d8 */
};

/*
 * The value words the page can show - from .rdata where the code has
 * them, and from a recording where it does not - in the same block
 * and the same six-per-language grouping [verified].
 *
 * JOYPAD is a third controller the recording did not show, at
 * 0x004e1f84 right beside JOYSTICK, so the row has three device
 * names and not two.
 */
static const char *const kP11Difficulty[3] = {
	"EASY", "MEDIUM", "HARD"     /* 0x004e20a8/2080/2050 */
};
static const char *const kP11Controller[3] = {
	"KEYBOARD", "JOYSTICK", "JOYPAD" /* 0x004e1fc8/1f78/1f84 */
};
static const char *const kP11Mode[3] = {
	"320*240", "512*384", "640*480"  /* 0x004e92d0/92c8/92c0 */
};
static const char *const kP11OnOff[2] = {
	"OFF", "ON"                  /* 0x004e1dd4, 0x004e1dbc */
};

/*
 * OPPONENTS IS A COUNT, NOT A SWITCH. g_cfgNbComputerCut is 0 or 1
 * and selects between two literals, "24" at 0x004e92e0 and "12" at
 * 0x004e92e4 - the two field sizes, not on and off [verified].
 */
static const char *const kP11Opponents[2] = { "24", "12" };

/* The refresh-rate row: 0 reads as a word, anything else as seconds. */
static const char *const kP11Manual = "manual";   /* 0x004e1958 */
static const char *const kP11Sec    = "sec.";     /* 0x004e1938 */

/*
 * BUTTON INDEX IS ACTIVATION ID, and the order below is the
 * original's. MenuPage11_Options_Tick clears the whole list at
 * 0x00443295 and rebuilds all twenty-three EVERY FRAME, because most
 * of them are text whose content changes with the value they show
 * [verified].
 *
 * o SO THE ORDER OF THE ADDS IS LOAD-BEARING. The activation switch
 *   is a switch on the index. Inserting one button silently changes
 *   the meaning of every case after it.
 *
 * o OUR HIT TEST RUNS ON THE PREVIOUS FRAME'S LIST, because it is
 *   called before the rebuild. That is only safe because the list
 *   is rebuilt identically every frame: the highlight is one frame
 *   stale and nothing can see it. Page 9 calls it the other way
 *   round, after the adds, so the two pages are inconsistent -
 *   harmless for the same reason, but pick one.
 *
 *   [open] Where the ORIGINAL hit-tests is not established. There is
 *   no such call before Menu_ClearButtons at 0x00443295 - only
 *   Present, BeginFrame, the backdrop and the title - so it is
 *   inside Menu_Draw or Menu_GetActivated, and this comment used to
 *   claim an ordering it did not have.
 */
#define P11_BTN_DIFFICULTY   0
#define P11_BTN_CONTROLLER   1
#define P11_BTN_CONFIG       2   /* the BtConfig sprite       */
#define P11_BTN_DETAIL       3
#define P11_BTN_OPPONENTS    4
#define P11_BTN_D3D          5
#define P11_BTN_VOICES       6
#define P11_BTN_SOUNDS       7
#define P11_BTN_MUSIC        8
#define P11_BTN_SCREENMODE   9
#define P11_BTN_WIDE169     10
#define P11_BTN_WAVEOUT     11
#define P11_BTN_MASTER_DOWN 12
#define P11_BTN_MASTER_UP   13
#define P11_BTN_WAVE_DOWN   14
#define P11_BTN_WAVE_UP     15
#define P11_BTN_CD_DOWN     16
#define P11_BTN_CD_UP       17
#define P11_BTN_SESS_DOWN   18
#define P11_BTN_SESS_UP     19
#define P11_BTN_ACCEPT      20
#define P11_BTN_INFO        21   /* btinfo   -> page 13, diagnostics */
#define P11_BTN_DATADISK    22   /* DATADISK -> page 23, data disk   */

#define P11_ROW_SESSIONS    14
#define P11_SESS_VALUE_X   312   /* 0x138, where the value sits */

/*
 * Which button, or pair of buttons, a row LABEL takes its highlight
 * from. The original spells each test out inline - "if (hover == 0)"
 * before the difficulty label, "if (hover == 0xc || hover == 0xd)"
 * before the master volume - and this is the same thing as a table.
 */
static const signed char kP11RowBtn[P11_ROWS][2] = {
	{  0, -1 }, {  1, -1 }, {  3, -1 }, {  4, -1 }, {  5, -1 },
	{  6, -1 }, {  7, -1 }, {  8, -1 }, {  9, -1 }, { 10, -1 },
	{ 11, -1 }, { 12, 13 }, { 14, 15 }, { 16, 17 }, { 18, 19 }
};

/* Master, wave, CD - and the channel numbers are not in row order. */
static const int kP11VolChannel[3] = {
	AUDIO_VOL_MASTER, AUDIO_VOL_WAVE, AUDIO_VOL_CD
};

static MR_Sprite s_p11_backdrop;
static MR_Sprite s_p11_title;
static MR_Sprite s_p11_info;
static MR_Sprite s_p11_datadisk;
static MR_Sprite s_p11_accept;
static MR_Sprite s_p11_config;
static MR_Sprite s_p11_slider;
static MR_Font   s_p11_font;
static int       s_p11_fontOk;
static int       s_p11_sliderOk;
static int       s_p11_ok;

/*
 * THE SETTINGS LIVE IN g_options, not here.
 *
 * The original keeps each on its own global - g_cfgDifficulty,
 * g_bindingColumn, g_cfgDetailLevel and so on - and Settings_LoadAll
 * distributes CONFIG.BPT across them. options.c is that function, and
 * one struct rather than eleven file-scope ints is the only
 * difference. The three VOLUMES are not in it either: they belong to
 * audio.c, which is what will apply them.
 *
 * o SO NOTHING ON THIS PAGE IS RESET BY Enter. The originals are
 *   globals, so leaving the page and coming back keeps what was
 *   chosen - and now so does quitting the game.
 */
static int s_p11_dataDiskCount;    /* DAT_004dc04c               */

/* The original's three static value buffers, kept static for the
 * same reason: a text button holds the pointer, not a copy. */
static char s_p11_detailText[16];  /* DAT_0053f678 */
static char s_p11_modeText[32];    /* DAT_0053f3c0 */
static char s_p11_waveText[64];    /* DAT_0053f2b8 */

/*
 * ORIGIN  MenuPage11_Options_Enter @ 0x00443000, at 0x00443110
 *
 * THE DATA DISK SCAN [verified]. Enter _findfirst()s
 * "Data\List*.bpt" - the string is at 0x004e9290 - atoi()s each name
 * into the u16 table at 0x0051a420 (the store is 0x00443194), and
 * counts them into DAT_004dc04c: zeroed at 0x0044315a, accumulated
 * at 0x0044319d.
 *
 * THE TEST IS IN THE TICK, NOT HERE. CMP ECX,1 / JLE at 0x00444410
 * decides whether the DATADISK button is added, so it is re-decided
 * every frame along with the rest of the list.
 *
 * o A LOOSE-FILE SCAN IS CORRECT HERE. CLAUDE.md rule 10 sends
 *   assets through asset_load, but DATA\*.BPT are loose in a stock
 *   install and this is a directory question, not a file read.
 *
 * o A STOCK INSTALL HAS EXACTLY ONE, LIST.BPT, so the button is
 *   hidden - which is the point. Our decomp/DATA has LIST01.BPT as
 *   well and will show it.
 *
 * NOT REPRODUCED: the atoi table itself. Nothing reads it until
 * page 23 is built.
 */
static int p11_count_datadisks(void)
{
	WIN32_FIND_DATAA fd;
	HANDLE           h;
	char             pattern[MAX_PATH];
	const char      *dir = get_data_dir();
	int              n   = 0;

	if (!dir || !dir[0] || strlen(dir) + 12 >= sizeof(pattern))
		return 0;
	strcpy(pattern, dir);
	strcat(pattern, "List*.bpt");

	h = FindFirstFileA(pattern, &fd);
	if (h == INVALID_HANDLE_VALUE)
		return 0;
	do {
		if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
			n++;
	} while (FindNextFileA(h, &fd));
	FindClose(h);
	return n;
}

/* ORIGIN  MenuPage11_Options_Enter @ 0x00443000 */
static void page11_enter(void)
{
	s_p11_ok = 1;
	s_p11_ok &= load_res(&s_p11_backdrop, P11_RES_BACKDROP, 0, 0,
	                    SPRITE_ANCHOR_NONE, "page11");
	s_p11_ok &= load_res(&s_p11_title,    P11_RES_TITLE,    0,   0,
	                    SPRITE_ANCHOR_NONE, "page11");
	s_p11_ok &= load_res(&s_p11_info,     P11_RES_INFO,     25,  420,
	                    SPRITE_ANCHOR_NONE, "page11");
	s_p11_ok &= load_res(&s_p11_datadisk, P11_RES_DATADISK, 275, 420,
	                    SPRITE_ANCHOR_NONE, "page11");
	s_p11_ok &= load_res(&s_p11_accept,   P11_RES_ACCEPT,   500, 420,
	                    SPRITE_ANCHOR_NONE, "page11");
	s_p11_ok &= load_res(&s_p11_config, P11_RES_CONFIG,
	                    P11_CONFIG_X, P11_CONFIG_Y,
	                    SPRITE_ANCHOR_NONE, "page11");
	s_p11_sliderOk = load_res(&s_p11_slider, P11_RES_SLIDER, 0, 0,
	                          SPRITE_ANCHOR_NONE, "page11");

	s_p11_fontOk = fontdef_open(&s_p11_font, P11_FONT_ROW);
	if (!s_p11_fontOk)
		mrlog_err("page11: row font did not load\n");

	s_p11_dataDiskCount = p11_count_datadisks();

	/*
	 * NOT REPRODUCED, and all of it is Enter:
	 *
	 *   the g_cfgInputDevice (0x004dc11c) -> g_bindingColumn
	 *   (0x004e90d0) map, three spelled-out arms at 0x00443084,
	 *   0x00443096 and 0x004430ac giving 1->0, 4->1 and 2->2
	 *   [verified] - which we cannot do until an input device
	 *   exists;
	 *
	 *   the clamp that forces detail level 0 when the device
	 *   reports no texture support.
	 */
	mrlog("page11: %s, %d List*.bpt\n",
	      s_p11_ok ? "loaded" : "INCOMPLETE", s_p11_dataDiskCount);
}

/* ORIGIN  NONE - ours. Page 11's table record at 0x004dbbe8 reads
 *         {0, 0x00443200, 0x00443000, 0} - tick, enter, and a NULL
 *         fnLeave [verified]. */
static void page11_leave(void)
{
	sprite_free(&s_p11_backdrop);
	sprite_free(&s_p11_title);
	sprite_free(&s_p11_info);
	sprite_free(&s_p11_datadisk);
	sprite_free(&s_p11_accept);
	sprite_free(&s_p11_config);
	if (s_p11_sliderOk) sprite_free(&s_p11_slider);
	if (s_p11_fontOk)   font_free(&s_p11_font);

	/*
	 * AND THIS IS WHERE [OPTIONS] IS WRITTEN. Leave runs on ACCEPT,
	 * on escape and on any other transition off the page, so one
	 * call here covers every way out. The original has no leave
	 * handler at all and writes CONFIG.BPT elsewhere.
	 */
	options_save();
	s_p11_fontOk   = 0;
	s_p11_sliderOk = 0;
	menu_clear_buttons();
	s_p11_ok = 0;
}

/*
 * ORIGIN  MenuPage11_Options_Tick @ 0x00443200, at 0x0044352f
 *
 * The value text for one row. Rows 11 to 13 have a slider instead
 * and row 14 draws its own, so those return NULL and get no button.
 */
static const char *p11_value(int row)
{
	switch (row) {
	case 0:  return kP11Difficulty[g_options.difficulty];
	case 1:  return kP11Controller[g_options.controller];
	case 2:
		_snprintf(s_p11_detailText, sizeof s_p11_detailText,
		          "%d", g_options.detail + 1);  /* 0x004e4ed8 */
		return s_p11_detailText;
	case 3:  return kP11Opponents[g_options.opponents];
	case 4:
		/*
		 * THE ROW IS RELABELLED AND IS TWO-STATE.  [divergence]
		 *
		 * The original labels it "D3D : " at 0x004e1e94. THE
		 * BUTTON CYCLES THREE STATES OVER TWO GLOBALS - 0x004dc070
		 * for on/off and 0x0053be0c for filtered, written in three
		 * arms at 0x0044464f, 0x0044465b and 0x00444671 - while the
		 * ROW CAN PRINT FOUR TEXTS, the fourth being "Not detected."
		 * when there is no device: OFF and ON at 0x004e1dd4 and
		 * 0x004e1dbc, "FILTERED" at 0x004e1d40, "Not detected." at
		 * 0x004e164c [verified].
		 * Three of those four are about the SOFTWARE rasterizer
		 * and whether a device was found, and this port has one
		 * renderer which is always hardware - so the only state
		 * left that means anything is the filtering bit.
		 *
		 * So the row says what it actually does.  The label is
		 * OURS and is the one string in kP11Label with no
		 * original address; the rest of the table cites one.
		 */
		return kP11OnOff[g_options.textureFilter != 0];
	case 5:  return kP11OnOff[g_options.voices != 0];
	case 6:  return kP11OnOff[g_options.sounds != 0];
	case 7:  return kP11OnOff[g_options.music  != 0];
	case 8:
		_snprintf(s_p11_modeText, sizeof s_p11_modeText, "%s",
		          kP11Mode[g_options.screenMode]);
		return s_p11_modeText;
	case 9:  return kP11OnOff[g_options.wide169 != 0];
	case 10:
		{
			/*
			 * "%d %s / %d %s /%s" at 0x004e92ac, and in the original
			 * every field comes from the format the sound device
			 * ACTUALLY OPENED, not from the setting. With no device
			 * to ask, ours reads the stored index back out.
			 *
			 * The rate prints as kHz by INTEGER DIVISION - 22050/1000
			 * is 22, not 22.05 - which is why the retail screen says
			 * 22 and not 22.05 [verified].
			 */
			const MR_WaveFormat *w =
			        options_wave_format(g_options.waveFormat);

			if (!w)
				return "";
			_snprintf(s_p11_waveText, sizeof s_p11_waveText,
			          "%d %s / %d %s /%s", w->bits, "bits",
			          w->rateHz / 1000, "Khz",
			          w->stereo ? " stereo" : " mono");
			return s_p11_waveText;
		}
	default: return NULL;
	}
}

/* ORIGIN  NONE - ours
 *
 * One volume step. The original goes through the Windows mixer; see
 * the note over AUDIO_VOL_MASTER for why we do not. */
static void p11_step_volume(int row, int delta)
{
	int ch = kP11VolChannel[row];

	audio_set_volume(ch, audio_volume(ch) + delta);
}

/* ORIGIN  MenuPage11_Options_Tick @ 0x00443200 */
static void page11_tick(void)
{
	int  row;
	int  hover;
	int  act;
	int  pos[2];

	if (!s_p11_ok)
		return;

	/*
	 * THE BACKDROP IS paralax2 AND IT SCROLLS THE OTHER WAY. Sprite
	 * index 1 at 0x00504884, updated with (-1, -1) where page 3
	 * passes (1, 1) [verified, 0x00443243..0x0044324f]. Decreasing
	 * offsets move the IMAGE down and right.
	 */
	sprite_update_wave(&s_p11_backdrop, -1, -1);
	sprite_draw_wave(&s_p11_backdrop);

	/* On the list this tick is about to replace - see the note over
	 * P11_BTN_DIFFICULTY. */
	menu_hit_test();
	hover = menu_hover_index();

	sprite_update_move(&s_p11_title);
	sprite_draw(&s_p11_title, SPRITE_MODE_USE_KIND, 100, 0);

	menu_clear_buttons();

	/*
	 * NO FONT, NO ROWS - and then the indices below all shift, so the
	 * activation switch would fire the wrong case. Bail out with the
	 * one button that matters and treat it as ACCEPT.
	 */
	if (!s_p11_fontOk) {
		menu_add_sprite_button(&s_p11_accept, NULL, VK_ESCAPE);
		menu_draw_buttons();
		if (menu_get_activated() >= 0) {
			menu_clear_activated();
			menu_begin_transition(MENU_PAGE_MAIN);
		}
		return;
	}

	for (row = 0; row < P11_ROWS; row++) {
		int         y   = P11_ROW0_Y + row * P11_ROW_STEP;
		int         on  = (hover >= 0 &&
		                   (hover == kP11RowBtn[row][0] ||
		                    hover == kP11RowBtn[row][1]));
		const char *val;

		font_set_colors(&s_p11_font, on ? kMenuTextYellow
		                               : kMenuTextWhite, kMenuTextBlack);
		font_draw_text(&s_p11_font, P11_LABEL_X, y, kP11Label[row], 0);

		if (row >= P11_ROW_MASTER_VOL && row <= P11_ROW_CD_VOL) {
			/*
			 * A MINUS, A BAR AND A PLUS. The bar is ONE sprite drawn
			 * three times and SHRUNK rather than scaled - full volume
			 * is shrink 0, which is the whole bitmap. There is no
			 * separate track art.
			 */
			int ch  = kP11VolChannel[row - P11_ROW_MASTER_VOL];
			int vol = audio_volume(ch);

			menu_add_text_button("-", &s_p11_font,
			                     P11_SLIDER_MINUS_X, y, 0, 1);
			menu_add_text_button("+", &s_p11_font,
			                     P11_SLIDER_PLUS_X,  y, 0, 1);

			if (s_p11_sliderOk) {
				pos[0] = P11_SLIDER_BAR_X;
				pos[1] = y + P11_SLIDER_BAR_DY;
				sprite_set_position(&s_p11_slider, pos,
				                    SPRITE_ANCHOR_NONE);
				sprite_draw(&s_p11_slider, SPRITE_MODE_USE_KIND, 100,
				            100 - vol * 100 / AUDIO_VOL_MAX);
			}
			continue;
		}

		if (row == P11_ROW_SESSIONS) {
			char buf[32];

			/* Drawn, not a button - the value sits BETWEEN the two
			 * steppers at x 312, in the label colour. */
			if (g_options.sessionRefresh == 0)
				_snprintf(buf, sizeof buf, "%s", kP11Manual);
			else
				_snprintf(buf, sizeof buf, "%d %s",   /* 0x004e92a0 */
			              g_options.sessionRefresh, kP11Sec);
			font_draw_text(&s_p11_font, P11_SESS_VALUE_X, y, buf, 0);

			menu_add_text_button("-", &s_p11_font,
			                     P11_SLIDER_MINUS_X, y, 0, 1);
			menu_add_text_button("+", &s_p11_font,
			                     P11_SLIDER_PLUS_X,  y, 0, 1);
			continue;
		}

		val = p11_value(row);
		if (val)
			menu_add_text_button(val, &s_p11_font, P11_VALUE_X, y, 0, 0);

		/* BtConfig sits beside the CONTROLLER row and is button 2, so
		 * it has to be added here and not with the other sprites. */
		if (row == 1)
			menu_add_sprite_button(&s_p11_config, NULL, 0);
	}

	/*
	 * THE THREE AT THE BOTTOM, and the third is conditional. The
	 * original adds DATADISK only when more than one Data\List*.bpt
	 * exists, so a stock install shows two buttons and not three.
	 *
	 * o ESCAPE ON ACCEPT IS OURS. Every button on this page in the
	 *   original registers MENUKEY_NONE, which leaves no keyboard
	 *   way off the page at all [verified: no key code is pushed
	 *   anywhere in the tick's 1851 instructions - the only 0xd is
	 *   the page number at 0x0044498a].
	 *
	 *   The three destinations are pushed together at 0x0044497f,
	 *   0x0044498a and 0x00444995: pages 3, 13 and 23.
	 */
	menu_add_sprite_button(&s_p11_accept, NULL, VK_ESCAPE);
	menu_add_sprite_button(&s_p11_info,   NULL, 0);
	if (s_p11_dataDiskCount > 1)
		menu_add_sprite_button(&s_p11_datadisk, NULL, 0);

	menu_draw_buttons();

	act = menu_get_activated();
	if (act < 0)
		return;
	menu_clear_activated();

	switch (act) {
	case P11_BTN_DIFFICULTY:
		if (++g_options.difficulty > 2)
			g_options.difficulty = 0;
		break;

	case P11_BTN_CONTROLLER:
		/* The binding COLUMN, 0..2. It maps to the original's
		 * InputDevice as 0->1, 1->4, 2->2 - see options.h. */
		if (++g_options.controller > 2)
			g_options.controller = 0;
		break;

	case P11_BTN_DETAIL:
		/*
		 * FIVE LEVELS, 0 to 4, shown as 1 to 5. The original also
		 * refuses to leave 0 when the device does not do textures.
		 */
		if (++g_options.detail > 4)
			g_options.detail = 0;
		break;

	case P11_BTN_OPPONENTS:
		g_options.opponents ^= 1;
		break;

	case P11_BTN_VOICES:
		/* [open] FUN_00408f30 also loads or drops the voice bank
		 * and puts a toast on screen. We only move the flag -
		 * audio_load_bank exists, so the bank half is wireable. */
		g_options.voices ^= 1;
		break;

	case P11_BTN_SOUNDS:
		g_options.sounds ^= 1;   /* FUN_00408b30 */
		break;

	case P11_BTN_MUSIC:
		g_options.music ^= 1;    /* FUN_00409410 */
		break;

	case P11_BTN_WIDE169:
		/*
		 * APPLIED AT THE TOP OF THE NEXT FRAME, through the
		 * original's own dirty flag - see options.h and
		 * apply_video_settings in main.cpp.
		 *
		 * RAISING IT FROM THIS PAGE IS OURS. The original's
		 * page 11 edits a saved COPY (0x00504e80) because
		 * Menu_SetPage forces the whole menu to 640x480 4:3 on
		 * entry and only restores the real settings on the way
		 * out - so on retail nothing visible happens until you
		 * leave. We keep no such copy.
		 *
		 * o AND OURS IS NOT THE ORIGINAL'S 16:9. Theirs crops
		 *   the surface to 640x360 and leaves the lens alone;
		 *   ours widens to 854x480 so the field opens sideways
		 *   instead of losing 120 scanlines. [OPTIONS]
		 *   Wide169Crop=1 asks for theirs. See options.h.
		 */
		g_options.wide169 ^= 1;
		g_videoSettingsDirty = 1;
		break;

	case P11_BTN_SCREENMODE:
		/*
		 * THE VALUE CYCLES AND IS STORED; NOTHING APPLIES IT. The
		 * original has two arms here - windowed cycles
		 * g_screenModeCopy 0..2, fullscreen walks the enumerated
		 * g_displayModes - and ours renders at 640x480 and
		 * letterboxes either way.
		 *
		 * o SO THE LOG LINE IS PART OF THE FEATURE, not debug
		 *   noise. A row that changes and does nothing looks
		 *   exactly like a working setting.
		 */
		if (++g_options.screenMode >= OPT_SCREEN_MODES)
			g_options.screenMode = 0;
		mrlog("page11: screen mode %d stored, not applied yet\n",
		      g_options.screenMode);
		break;

	case P11_BTN_WAVEOUT:
		/*
		 * One index over eight formats - see options.h for the
		 * odometer FUN_00409570 walks and why we cannot reproduce
		 * its device gating. Stored, and applied by nothing yet.
		 */
		if (++g_options.waveFormat >= OPT_WAVE_FORMATS)
			g_options.waveFormat = 0;
		mrlog("page11: wave format %d stored, needs an audio back "
		      "end to apply\n", g_options.waveFormat);
		break;

	case P11_BTN_MASTER_DOWN: p11_step_volume(0, -1); break;
	case P11_BTN_MASTER_UP:   p11_step_volume(0, +1); break;
	case P11_BTN_WAVE_DOWN:   p11_step_volume(1, -1); break;
	case P11_BTN_WAVE_UP:     p11_step_volume(1, +1); break;
	case P11_BTN_CD_DOWN:     p11_step_volume(2, -1); break;
	case P11_BTN_CD_UP:       p11_step_volume(2, +1); break;

	case P11_BTN_SESS_DOWN:
		if (--g_options.sessionRefresh < 0)
			g_options.sessionRefresh = 0;
		break;

	case P11_BTN_SESS_UP:
		if (++g_options.sessionRefresh > 30)
			g_options.sessionRefresh = 30;   /* 0x1e */
		break;

	case P11_BTN_ACCEPT:
		/* The original applies g_bindingColumn to g_cfgInputDevice
		 * and calls FUN_004311c0 first. Nothing to apply it to yet. */
		menu_begin_transition(MENU_PAGE_MAIN);
		break;

	/*
	 * THE FOUR THAT GO NOWHERE YET, and they are not skipped
	 * quietly. Three lead to pages that are not built and one
	 * changes a setting we cannot apply, so each says so rather
	 * than looking broken - or worse, looking like it worked.
	 */
	case P11_BTN_CONFIG:
		mrlog("page11: controller config is page 12, not built\n");
		break;

	case P11_BTN_INFO:
		mrlog("page11: diagnostics is page 13, not built\n");
		break;

	case P11_BTN_DATADISK:
		mrlog("page11: data disk is page 23, not built\n");
		break;

	case P11_BTN_D3D:
		/*
		 * BILINEAR ON OR OFF.  The original cycles three states
		 * here over 0x004dc070 and 0x0053be0c, and rebuilds the
		 * glass shards on every change; two of the three concern
		 * the software rasterizer, which we do not have.  See
		 * p11_value case 4 for the arms and the four texts.
		 *
		 * IT TAKES EFFECT ON THE NEXT 3D FRAME and needs no
		 * device reset - drawscene reads the option per
		 * primitive.  Nothing on THIS page changes, because the
		 * options page draws sprites and no scene geometry; the
		 * value text is the only feedback here.
		 */
		g_options.textureFilter = !g_options.textureFilter;
		mrlog("page11: texture filter %s\n",
		      g_options.textureFilter ? "on" : "off");
		break;

	}
}


static int s_menu_quit;

/* ================================================================ */
/* Page 4 - EXIT.  It is not a screen; it closes the game.          */
/* ================================================================ */

/*
 * ORIGIN  MenuPage04_Tick @ 0x0040bc40
 *
 * A TICK HANDLER AND NOTHING ELSE - the page table record at
 * 0x004dbb78 reads {0, 0x0040bc40, 0, 0}, so no enter and no leave
 * and arriving IS the whole behaviour [verified].  The original's
 * tick:
 *
 *     free the menu object resources
 *     clear hover and keyboard nav
 *     run the CURRENT page's leave handler
 *     Menu_SetPage(0)
 *     walk the page table running EVERY page's leave handler
 *     PostMessageA(hwnd, WM_CLOSE, 0, 0)
 *
 * IT IS A DIRECT QUIT.  There is no confirmation page and nothing
 * asks twice - which is why EXIT arrives here with a plain
 * Menu_GotoPage and no glass transition.
 *
 * PAGE 0 IS NOT 'HAND THE MENU TO THE GAME', which the EXIT
 * button invites you to read it as.  Page 0 here is simply 'no
 * page', set so the teardown below has nothing live to walk over;
 * the very next statement closes the window.
 *
 * OURS DOES LESS, DELIBERATELY.  menu_goto_page has already run
 * page 3's leave handler on the way in, and menu_free at shutdown
 * walks the rest - so the explicit teardown would be doing twice
 * what the exit path already does once.  What is left is the
 * request itself.
 */
static void page4_tick(void)
{
	if (!s_menu_quit) {
		s_menu_quit = 1;
		mrlog("page4:  EXIT - closing\n");
	}
}

/* ORIGIN  NONE - ours */
int menu_quit_requested(void)
{
	return s_menu_quit;
}

/* ================================================================ */
/* The page table                                                   */
/* ================================================================ */

/*
 * g_menuPageTable, 0x004dbb38, 28 records of 16 bytes indexed by page
 * number. Records 0 and 27 are empty and are not pages.
 *
 * o THE FIELD ORDER IS {initialised, fnTick, fnEnter, fnLeave} -
 *   TICK BEFORE ENTER [verified, read out of the record at 0x004dbc18].
 *   That is not the order the handlers run in, and menu-pages.txt
 *   section 2 lists its columns the other way round, which is worth
 *   knowing before cross-referencing the two.
 */
static MR_MenuPage s_pages[MENU_PAGE_COUNT] = { { 0, 0, 0, 0 } };


/* ================================================================ */
/* Page 25 - the Delphine Software logo, the cinematic intro        */
/* ================================================================ */

/*
 * From MenuPage25_DsiLogo_Enter (0x004537d0). This is App_Init's FIRST
 * page - the chain is 25 -> 14 -> 15 -> 3 - and it exits to the title
 * screen. [DEBUG] startpage in the ini steps over it; see menu.h.
 *
 * IT USES THE SAME MACHINERY AS PAGE 14 and nothing else: Mesh_LoadPair,
 * Mesh_Scale, ModelInstance_Init, Camera_LookAt, Screen3D_SetProjZoom.
 * It does NOT use MenuTools_CreateSceneObject, so MR_MenuSceneObject is
 * not a prerequisite for the intro - see menu-pages.txt section 4.3.2.
 *
 * FIVE MESHES, and the first is not scenery: camera.3df is a 215-vertex
 * model, the only real geometry in the set. D, S and I are extruded 3D
 * letters and DsInt is a flat billboard.
 */

#define P25_MODELS   5

/*
 * The mesh pair names, in the order Enter loads them. Logical asset
 * names - PAGES\TITLEDSI\... resolves inside PAGES.BKF.
 */
static const char *const kP25Mesh[P25_MODELS][2] = {
	{ "PAGES\\TITLEDSI\\CAMERA.3DF", "PAGES\\TITLEDSI\\CAMERA.MAP" },
	{ "PAGES\\TITLEDSI\\D.3DF",      "PAGES\\TITLEDSI\\D.MAP"      },
	{ "PAGES\\TITLEDSI\\S.3DF",      "PAGES\\TITLEDSI\\S.MAP"      },
	{ "PAGES\\TITLEDSI\\I.3DF",      "PAGES\\TITLEDSI\\I.MAP"      },
	{ "PAGES\\TITLEDSI\\DSINT.3DF",  "PAGES\\TITLEDSI\\DSINT.MAP"  }
};

/*
 * Mesh_Scale per mesh. The camera gets its own; the four letters share
 * one [verified, 0x004538a4 onward].
 */
static const float kP25Scale[P25_MODELS][3] = {
	{ 0.055f, 0.055f, 0.06f },
	{ 0.04f,  0.04f,  0.04f },
	{ 0.04f,  0.04f,  0.04f },
	{ 0.04f,  0.04f,  0.04f },
	{ 0.04f,  0.04f,  0.04f }
};

/*
 * ModelInstance_Init's position and rotation per instance.
 *
 * The camera model is placed off to the left and a long way back; the
 * four letters all start at the origin and are moved by the tick. The
 * rotations are the interesting part:
 *
 *   D and S get -pi/2, I and DsInt get +pi/2. Enter runs the letters
 *   through one loop that writes -pi/2 for all four, then overwrites
 *   rotation[1] of the last two with +pi/2 immediately afterwards, at
 *   0x0053fce0 and 0x0053fe64 [verified]. Two of the four face the
 *   other way.
 */
static const float kP25Pos[P25_MODELS][3] = {
	{ -30.0f, 0.0f, -900.0f },
	{   0.0f, 0.0f,    0.0f },
	{   0.0f, 0.0f,    0.0f },
	{   0.0f, 0.0f,    0.0f },
	{   0.0f, 0.0f,    0.0f }
};
static const float kP25Rot[P25_MODELS][3] = {
	{ 0.0f, -1.701696f,  0.0f },    /* 0xbfd9d12d, about -97.5 deg */
	{ 0.0f, -1.5707964f, 0.0f },
	{ 0.0f, -1.5707964f, 0.0f },
	{ 0.0f,  1.5707964f, 0.0f },
	{ 0.0f,  1.5707964f, 0.0f }
};

/*
 * THE ANIMATION TABLES, from FUN_00453610 (0x00453610), page 25's
 * counterpart of MenuPage14_Title_ResetAnim [verified].
 *
 * SIX ENTRIES, NOT FIVE. The tick loops six times - CMP EAX,0x6 at
 * 0x00454152 - over instances 0x184 apart from 0x0053f850, and the
 * sixth is not a model: it drives the CAMERA EYE and is never drawn.
 *
 *     if (i != 5) ModelInstance_Draw(...)
 *
 * Entry 5 is special-cased twice, at 0x00453dbc and 0x0045407f, both
 * testing the index as a byte offset - CMP ESI,0x14 [verified].
 *
 * so entry 5 writes camera+0x98 instead. The instance slot behind it
 * exists and its rotation[0] is zeroed, but nothing draws it.
 *
 * o ENTRY 0 IS ALSO SPECIAL: its branch writes ROTATION ONLY and never
 *   touches position, so its start and end rows are set by the reset
 *   and then never read. They are kept here because leaving a row out
 *   would silently renumber every one below it.
 *
 * THE INTRO RUNS FOR TWENTY SECONDS - the longest duration is entry
 * 5's. That is worth knowing before wondering whether it has hung.
 */
#define P25_ANIM 6              /* six animated objects            */
#define P25_ANIM_EYE 5          /* the one that moves the camera   */

static const float kP25AnimStart[P25_ANIM][3] = {
	{  50.0f,  80.0f, -1000.0f },   /* unused - entry 0 rotates only */
	{   0.0f,   0.0f, 20000.0f },
	{   0.0f,   0.0f, 20000.0f },
	{   0.0f,   0.0f, 20000.0f },
	{  20.0f,   0.0f, 60000.0f },
	{  50.0f,  80.0f, -1000.0f }
};
static const float kP25AnimEnd[P25_ANIM][3] = {
	{  50.0f,   0.0f,  7000.0f },   /* unused */
	{ 200.0f,   0.0f,   200.0f },
	{ -50.0f,   0.0f,   200.0f },
	{-250.0f,   0.0f,   200.0f },
	{  20.0f, -400.0f, -600.0f },
	{  50.0f,   0.0f,  7000.0f }
};
static const float kP25Duration[P25_ANIM] = {
	15.0f, 14.5f, 14.0f, 13.5f, 16.0f, 20.0f
};

/*
 * Entry 0's rotation curve, the only one fully transcribed:
 *
 *     rotation[0] = 0
 *     rotation[1] = (1 - u) * pi/2 - 1.701696
 *     rotation[2] = (1 - u*u) * pi/2
 *
 * The constants are 0x004d0b5c = 1.0f, 0x004d0c20 = pi/2 as a double
 * and 0x004d0d28 = 1.701696021 as a double [verified]. That last one
 * is exactly the rotation[1] Enter gives this instance, so the curve
 * ENDS where Enter started it - at u = 1 the triple is (0, -1.701696,
 * 0) again.
 */
#define P25_ROT_BIAS 1.701696f

/*
 * The D/S/I constants, all read out of the binary [verified]:
 *
 *   0x004d0d30  -MR_TWO_PI     the spin angle, -2pi over the curve
 *   0x004d0ca8   TRIG_IDX_PER_RAD  radians to table index - the
 *                              TYPED 2*pi, see trig.h
 *   0x004d0d38  25.13274123    8pi - FOUR turns of self-rotation
 *   0x004d0d40  -2500.0f       the radius shrink term, in u*u
 *   0x004d0d44   2500.0f       the radius offset at u = 0
 *   0x004d0d48   800.0f        the z shift applied at the end
 */
#define P25_SPIN_TURNS  25.132741f      /* 8pi */
#define P25_RADIUS_ADD  2500.0f
#define P25_Z_SHIFT      800.0f

/* The two banks, loaded into slots 0 and 1 [verified]. */
#define P25_BANK0 0
#define P25_BANK1 1

#define RES_CONTINUE 77         /* continue.lez, the ESC button       */
#define RES_LOGODSI 178         /* logodsi.lez, Sprite_Load(0xb2)     */

/*
 * The continue button sits OFF SCREEN at (-1, -300) [verified, the
 * -300 is MOV EAX,0xfffffed4 at 0x00453b71]. It is
 * not decoration - it is registered with Menu_AddSpriteButton for
 * VK_ESCAPE, so it is how the player skips the logo. A hit test can
 * never reach it, which is the point: it is a key binding wearing a
 * button's clothes.
 */
#define P25_CONTINUE_X   (-1)
#define P25_CONTINUE_Y (-300)

static MESH             s_p25_mesh[P25_MODELS];
static MR_ModelInstance s_p25_inst[P25_MODELS];
static MR_Primitive    *s_p25_prims[P25_MODELS];
static int              s_p25_mesh_ok[P25_MODELS];
static int              s_p25_models_ok;

static MR_Sprite s_p25_logo;
static MR_Sprite s_p25_continue;
static int       s_p25_logo_ok;
static int       s_p25_continue_ok;

static float  s_p25_view[12];
static double s_p25_enter_time;
static double s_p25_last_frame;
static float  s_p25_t[P25_ANIM];
static float  s_p25_eye[3];
static int    s_p25_phase;

/*
 * Camera_SetEyeAndTarget(0,0,0 / 0,0,1500), then the eye is written
 * directly to camera+0x98 as (0,0,4500) and Camera_LookAt aims at the
 * origin - the same camera page 14 uses [verified].
 */
static const float kP25CamPos[3] = { 0.0f, 0.0f, 4500.0f };
static const float kP25Target[3] = { 0.0f, 0.0f, 0.0f };

/* ORIGIN  MenuPage25_DsiLogo_Enter @ 0x004537d0 */
static void page25_enter(void)
{
	int i;
	int loaded = 0;

	/*
	 * TWO BANKS, 0 AND 1, both requested with wantKey 1 and rgbBits 16
	 * - the immediates to the calls at 0x00453860 and 0x004538cf
	 * [verified] - so both are colour-keyed 16-bit pages, exactly like
	 * page 14 bank 3.
	 *
	 * o THE ORIGINAL GATES BOTH ON g_rendererReady (0x004dc06c), at
	 *   0x00453842 and again for the second bank, and skips the load
	 *   entirely when it is clear. There is nothing to gate on here:
	 *   this port has one renderer and it is always up. Noted so the
	 *   unconditional load reads as a decision.
	 *
	 * Enter also writes flags 0x180 to each bank before allocating the
	 * slot. texture-banks.txt has 0x80|0x100 as "set together by
	 * Bankw_CreateBank, meaning not established", so it is reproduced
	 * as a bank_alloc_texture_slot call and no more - inventing a
	 * meaning for two bits nobody has decoded would be worse.
	 */
	bank_load(P25_BANK0, "PAGES\\TITLEDSI\\BNK00_00.RAW", 1);
	bank_alloc_texture_slot(P25_BANK0, 1, 0, 0, 16);
	bank_load(P25_BANK1, "PAGES\\TITLEDSI\\BNK00_01.RAW", 1);
	bank_alloc_texture_slot(P25_BANK1, 1, 0, 0, 16);

	/*
	 * o BOTH BANKS ARE KEYED, exactly like page 14 bank 3.
	 *
	 *   MEASURE THE SAMPLED RECTANGLES, NOT THE WHOLE PAGE. It is
	 *   easy to conclude keying punches a solid model into holes:
	 *   BNK00_01.RAW is 63 per cent black across all 256x256. But
	 *   the camera's 166 faces sample x[19..241] y[14..223] and are
	 *   1.7 per cent black - 158 of them 0.0, the worst single face
	 *   35.6. The hollow camera was a missing
	 *   Mesh_MirrorXAndFlipWinding, not the keying.
	 *
	 *   The rest of the page is the tell. DSINT's two faces sample
	 *   x[0..240] y[217..255] and are 50.8 per cent black - the same
	 *   flat-quad-on-a-black-background shape as page 14's
	 *   billboards, which is precisely what the key is for.
	 */

	s_p25_models_ok = 1;
	for (i = 0; i < P25_MODELS; i++) {
		int nprims;

		s_p25_mesh_ok[i] = mesh_load_with_map(&s_p25_mesh[i],
		                                      kP25Mesh[i][0],
		                                      kP25Mesh[i][1],
		                                      MESH_SCALE_3DF_MAP);
		if (!s_p25_mesh_ok[i]) {
			mrlog_err("page25: %s did not load\n", kP25Mesh[i][0]);
			s_p25_models_ok = 0;
			continue;
		}
		loaded++;
		/*
		 * RECENTRE BEFORE SCALING, which is the order Enter uses and
		 * the reason the letters land where they should. The three
		 * letter meshes are modelled as one word and each carries its
		 * position in that word; without this they arrive at that
		 * offset PLUS their animation target and stack up. See mesh.h.
		 */
		mesh_center_on_bounds(&s_p25_mesh[i], NULL);
		mesh_scale(&s_p25_mesh[i], kP25Scale[i][0], kP25Scale[i][1],
		           kP25Scale[i][2]);

		nprims = 0;
		if (!prim_build_from_mesh(&s_p25_mesh[i], &s_p25_prims[i],
		                          &nprims) || nprims <= 0) {
			mrlog_err("page25: %s produced no primitives\n",
			          kP25Mesh[i][0]);
			s_p25_models_ok = 0;
			continue;
		}

		/*
		 * NO REBASE HERE, unlike page 14. That page loads its texture
		 * into bank 3 and calls FUN_00422250 - Mesh_RebaseBankIndices
		 * wraps it - to shift every face bank index up by three; it
		 * does so three times, at 0x00446750, 0x00446761 and
		 * 0x0044676d, and page 24's podium calls it once at
		 * 0x00452da0. Page 25's enter makes no such call at all
		 * [verified] - it loads into banks 0 and 1, which is where the
		 * .map records already point, so the indices are used as they
		 * are. Adding a rebase "for symmetry" would send every face at
		 * a bank nobody loaded.
		 */
		model_init(&s_p25_inst[i], kP25Pos[i], kP25Rot[i],
		           &s_p25_mesh[i]);
		model_set_prims(&s_p25_inst[i], s_p25_prims[i], nprims);
	}

	if (!camera_look_at(s_p25_view, kP25CamPos, kP25Target))
		s_p25_models_ok = 0;
	screen3d_set_proj_zoom(1.0f);

	s_p25_logo_ok = load_res(&s_p25_logo, RES_LOGODSI, 0, 0,
	                         SPRITE_ANCHOR_NONE, "page25");
	s_p25_continue_ok = load_res(&s_p25_continue, RES_CONTINUE,
	                             P25_CONTINUE_X, P25_CONTINUE_Y,
	                             SPRITE_ANCHOR_NONE, "page25");

	for (i = 0; i < P25_ANIM; i++)
		s_p25_t[i] = 0.0f;
	s_p25_phase = 0;
	for (i = 0; i < 3; i++)
		s_p25_eye[i] = kP25AnimStart[P25_ANIM_EYE][i];

	s_p25_enter_time = tm_now();
	s_p25_last_frame = s_p25_enter_time;

	/*
	 * THE INTRO MUSIC. Enter calls FUN_00404990(0x0d, 0) at
	 * 0x00453b61 - CdAudio_PlayTrackCue in the DB - so CD track 13,
	 * mode 0, once and no loop [verified; the mode is ESI, and all
	 * three writes to it precede the XOR ESI,ESI at 0x00453ab2].
	 *
	 * It is not the last thing Enter does: ResetAnim runs immediately
	 * before it at 0x00453b59, and Menu_ClearButtons at 0x00453b6c
	 * plus the continue-button add come after.
	 *
	 * o IT IS MUSIC, NOT THE SHATTER. This call was carried as "a
	 *   sound or music trigger [guess]" and read as the glass sound
	 *   waiting to be wired. It is neither: FUN_00404990 goes to
	 *   mciSendCommandA and selects a RED BOOK CD TRACK, and the
	 *   shatter is a sample, 0x2a, fired from the transition arm in
	 *   glass.c. Two different subsystems that looked alike only
	 *   because the function had no name.
	 *
	 * o SO THE INTRO IS SILENT ON A STOCK INSTALL ANYWAY, and not
	 *   because of the stub - the music was on the disc. audio.h
	 *   records that as a real limit of CLAUDE.md rule 10.
	 */
	audio_music_cue(CD_TRACK_INTRO, CD_MODE_ONCE);

	/*
	 * MenuPage25_DsiLogo_ResetAnim (0x00453610) IS REPRODUCED. All
	 * six things it writes have an equivalent above or in the tick
	 * [verified]:
	 *
	 *   6 start vec3s  0x005401d0  -> kP25AnimStart
	 *   6 end vec3s    0x00542100  -> kP25AnimEnd
	 *   6 durations    0x00540ae0  -> kP25Duration
	 *   6 elapsed = 0  0x00540188  -> s_p25_t[i] = 0, just above
	 *   finished = 0   0x0053f788  -> s_p25_phase = 0, just above
	 *   instance[5][0] 0x0053ffe4  -> the camera-eye pseudo-instance
	 *                                 we do not have; nothing draws it
	 *
	 * o WRITING CONSTANTS INTO GLOBALS vs HOLDING const TABLES is
	 *   equivalent HERE, and that had to be checked rather than
	 *   assumed - a reset exists precisely when something else can
	 *   clobber the storage. Nothing does. ResetAnim at 0x0045363c is
	 *   the only WRITER of 0x00542100; the other three references are
	 *   page 25's own vec3 constructor at 0x004535f2, which walks six
	 *   of them 0xc apart, and page 7's Enter and Leave using the
	 *   address as a LOOP TERMINATOR because their own array ends
	 *   where this one begins. See the plate on
	 *   MenuPage25_DsiLogo_ResetAnim.
	 *
	 *   Mesh_CenterOnBounds (FUN_00420250) IS NOT OPTIONAL, and the
	 *   reason it looks optional is that Mesh_Scale recomputes
	 *   normals anyway - so the recentre reads as redundant. It is
	 *   not: the normals are the part that does not matter and the
	 *   VERTEX recentre is the part that does. See mesh.h.
	 */

	mrlog("page25: %d/%d meshes, logo %d, continue %d\n",
	      loaded, P25_MODELS, s_p25_logo_ok, s_p25_continue_ok);
}

/* ORIGIN  MenuPage25_DsiLogo_Leave @ 0x00453bf0 */
static void page25_leave(void)
{
	int i;

	for (i = 0; i < P25_MODELS; i++) {
		if (s_p25_prims[i]) {
			free(s_p25_prims[i]);
			s_p25_prims[i] = NULL;
		}
		if (s_p25_mesh_ok[i]) {
			mesh_free(&s_p25_mesh[i]);
			s_p25_mesh_ok[i] = 0;
		}
	}
	if (s_p25_logo_ok)     sprite_free(&s_p25_logo);
	if (s_p25_continue_ok) sprite_free(&s_p25_continue);
	s_p25_logo_ok     = 0;
	s_p25_continue_ok = 0;
	s_p25_models_ok   = 0;

	/*
	 * STOP THE INTRO MUSIC. Leave calls CdAudio_StopCue at 0x00453bff
	 * [verified], and it is that function's ONLY caller - no other
	 * page stops the CD, they simply ask for their own track.
	 *
	 * NOT the only CD stop in the binary, though: CdAudio_Stop
	 * (0x004b1720) underneath it is also reached from the options
	 * MUSIC toggle at 0x004094dc and from
	 * maybe_Game_FreeRaceResources at 0x0041144c.
	 *
	 * o IT IS NEEDED BECAUSE THE INTRO CAN BE SKIPPED. Track 13 is
	 *   started with mode 0, so it plays once and never loops; left
	 *   alone it would still be playing under the title page when
	 *   page 14 asks for track 3.
	 */
	audio_music_stop();
}

/*
 * ORIGIN  MenuPage25_DsiLogo_Tick @ 0x00453c50, the D/S/I branch
 *
 * The per-object curve. Returns the number of objects that have
 * finished.
 *
 *     t[i] += dtMs / (duration[i] * 1000)
 *     u = clamp(t[i], 0, 1)
 *     if (t[i] > 1) finished++
 *
 * exactly as page 14 does it. What differs is that the six entries take
 * THREE DIFFERENT BRANCHES, and only two of them are reconstructed.
 */
static int page25_animate(double dtMs)
{
	int i, finished = 0;

	for (i = 0; i < P25_ANIM; i++) {
		float u;
		int   k;

		s_p25_t[i] += (float)(dtMs / (kP25Duration[i] * 1000.0));

		u = s_p25_t[i];
		if (u < 0.0f) u = 0.0f;
		if (u > 1.0f) u = 1.0f;
		if (s_p25_t[i] > 1.0f)
			finished++;

		if (i == 0) {
			/* Rotation only; position is never written. */
			s_p25_inst[0].rotation[0] = 0.0f;
			s_p25_inst[0].rotation[1] =
			    (1.0f - u) * 1.5707964f - P25_ROT_BIAS;
			s_p25_inst[0].rotation[2] =
			    (1.0f - u * u) * 1.5707964f;
			continue;
		}

		if (i >= 1 && i <= 3) {
			/*
			 * D, S and I - the letters. Not a lerp: they SPIRAL IN,
			 * orbiting the Y axis on a shrinking radius while spinning
			 * about their own vertical.
			 *
			 * The branch looked impenetrable in the decompiler because
			 * it is written as a general rotation about an arbitrary
			 * axis, and the axis is given by an ANGLE that happens to
			 * be the literal 0. So the two table reads that look like
			 * variables are constants:
			 *
			 *     [0x005fcb80] = g_cosTable[0]      = cos 0     = 1
			 *     [0x00608b80] = g_cosTable[0x3000] = cos 3pi/2 = 0
			 *
			 * The arithmetic is the proof: g_cosTable is float[0x4000]
			 * at 0x005fcb80, and 0x00608b80 - 0x005fcb80 is 0xc000,
			 * which is 0x3000 entries [verified].
			 *
			 * Every term carrying that second one vanishes, and what
			 * survives is a plain rotation about Y. Reading those two
			 * addresses as table INDICES rather than as variables is
			 * the whole trick.
			 *
			 * The end position doubles as the approach DIRECTION: it
			 * is normalised, scaled by a radius that falls from
			 * |end| + 2500 to |end|, rotated, and shifted -800 in z.
			 * At u = 1 the rotation is a full turn (identity) and the
			 * radius is |end|, so the letter lands on
			 * end - (0, 0, 800) - which is z = -600 for all three, the
			 * same plane DsInt ends on. That agreement is what
			 * confirms the reading.
			 */
			const float *e = kP25AnimEnd[i];
			float len = (float)sqrt((double)(e[0] * e[0] +
			                                 e[1] * e[1] +
			                                 e[2] * e[2]));
			float inv, radius, px, py, pz, c, sn;
			int   idx;

			if (len <= 0.0f)
				continue;               /* a zero end has no direction */

			inv    = 1.0f / len;
			radius = u * u * -P25_RADIUS_ADD + len + P25_RADIUS_ADD;

			/* theta = u * -2pi, then to a table index the same way
			 * ModelInstance_Draw does it. */
			idx = (int)(u * -(float)TRIG_TWO_PI *
			            (float)TRIG_IDX_PER_RAD);
			c  = trig_cos(idx);
			sn = trig_sin(idx);

			px = e[0] * inv * radius;
			py = e[1] * inv * radius;
			pz = e[2] * inv * radius;

			s_p25_inst[i].rotation[0] = 0.0f;
			/*
			 * FOUR SELF-TURNS, unwinding to the pose Enter set: I ends
			 * at +pi/2 and D and S at -pi/2, which is exactly the split
			 * Enter makes between the last two letters and the first
			 * two. i == 3 is I.
			 */
			s_p25_inst[i].rotation[1] =
			    (1.0f - u) * P25_SPIN_TURNS +
			    ((i == 3) ? 1.5707964f : -1.5707964f);
			s_p25_inst[i].rotation[2] = 0.0f;

			s_p25_inst[i].position[0] = pz * sn + px * c;
			s_p25_inst[i].position[1] = py;
			s_p25_inst[i].position[2] =
			    (px * -sn + pz * c) - P25_Z_SHIFT;
			continue;
		}

		/*
		 * The plain lerp, which is what entries 4 and 5 take:
		 *
		 *     p = (end - start) * u + start
		 *
		 * Entry 4 is the DsInt billboard and writes the instance's
		 * position. Entry 5 writes the CAMERA EYE instead and is not
		 * drawn.
		 */
		if (i == P25_ANIM_EYE) {
			for (k = 0; k < 3; k++)
				s_p25_eye[k] = (kP25AnimEnd[i][k] -
			                    kP25AnimStart[i][k]) * u +
			                   kP25AnimStart[i][k];
		} else {
			for (k = 0; k < 3; k++)
				s_p25_inst[i].position[k] =
			        (kP25AnimEnd[i][k] -
			         kP25AnimStart[i][k]) * u +
			        kP25AnimStart[i][k];
		}
	}
	return finished;
}

/*
 * ORIGIN  MenuPage25_DsiLogo_Tick @ 0x00453c50
 *
 * ALL THREE BRANCHES ARE RECONSTRUCTED. The D/S/I one is in
 * page25_animate; what made it hard to read is recorded there.
 *
 * THE EXIT, TWO WAYS IN THE ORIGINAL: a button activating - the
 * off-screen ESCAPE button Enter registered - or the phase reaching
 * 6, i.e. all six objects finished. Only the second is here; the
 * first is page25_on_key below, for the reason given there. The
 * phase is the count of finished objects and never goes down, the
 * same shape as page 14.
 */
static void page25_tick(void)
{
	double now = tm_now();
	double dtMs;
	int    finished;

	/*
	 * dt in milliseconds, and over 500 SUBSTITUTES 100 rather than
	 * capping. Both are float constants, 0x004d0bf4 and 0x004d0bf8,
	 * compared with FCOM at 0x00453c81 - which is why no integer CMP
	 * appears in this function [verified].
	 *
	 * PAGE 10 IS THE OTHER WAY ROUND: CMP EDX,0x3e8 / MOV EDX,0x3e8,
	 * a cap at 1000. The two pages genuinely differ, so neither
	 * number can be carried across.
	 */
	dtMs = (now - s_p25_last_frame) * 1000.0;
	if (dtMs > 500.0)
		dtMs = 100.0;
	if (dtMs < 0.0)
		dtMs = 0.0;
	s_p25_last_frame = now;

	if (s_p25_models_ok) {
		finished = page25_animate(dtMs);
		if (finished > s_p25_phase)
			s_p25_phase = finished;
	} else {
		/* Nothing loaded, so nothing can finish. Leave at once
		 * rather than hold a blank page for twenty seconds. */
		s_p25_phase = P25_ANIM;
	}

	/*
	 * THE LOGO SPRITE IS NOT DRAWN HERE, AND IS NEVER DRAWN AT ALL.
	 * Enter loads it with Sprite_Load(0xb2) at 0x004539ce into
	 * 0x005409c0, and that global has exactly two references: the
	 * write in Enter and the free in Leave at 0x00453c2a. The tick
	 * does not touch it [verified]. The tick's only Sprite_Draw is
	 * the cursor, resource 85 at 0x005049d4, and then only on a
	 * full-screen enumerated device - the same sprite under the same
	 * two conditions as page 6.
	 *
	 * DRAWING IT PUTS THE FINISHED 2D LOGO UNDER THE 3D LETTERS still
	 * flying in, which reads as two instances with one already at
	 * rest. It is not a billboard and nothing in the original draws
	 * it.
	 */

	if (s_p25_phase >= P25_ANIM)
		menu_begin_transition(MENU_PAGE_TITLE);
}

/*
 * ORIGIN  NONE - ours. The original makes ESCAPE an off-screen sprite
 *         button in MenuPage25_DsiLogo_Enter @ 0x004537d0
 *
 * The ESCAPE key, which Enter models as an off-screen sprite button -
 * MenuPage25_DsiLogo_Enter registers it with Menu_AddSpriteButton at
 * a position off the left of the screen, so it is a key binding
 * wearing a button's clothes [verified].
 *
 * ENTER IS A HARNESS STAND-IN AND NOT THE GAME'S BINDING. The shell
 * intercepts ESCAPE for "quit" before the menu ever sees it, so the
 * game's own skip is unreachable while that is true. Rather than
 * change what ESCAPE does, RETURN is accepted as a second way in.
 *
 * It is a stand-in in the strict sense: it runs the SAME path the
 * game's ESCAPE runs, so nothing here changes what the game does -
 * it only adds a door to a room that already exists. When the shell
 * stops eating ESCAPE this arm can go, and nothing else has to move.
 */
static int page25_on_key(int vkey)
{
	if (vkey == VK_ESCAPE || vkey == VK_RETURN) {
		menu_begin_transition(MENU_PAGE_TITLE);
		return 1;
	}
	return 0;
}


/* ================================================================== */
/* PAGE 7 - THE GAME LIST.  OURS.                                     */
/*                                                                    */
/* The original's MenuPage07_NetSessionList enumerated DirectPlay      */
/* sessions; this lists what the relay knows about. NO CODES AND NO    */
/* PASSWORDS - a game is listed under whoever is hosting it and        */
/* joining is trusting that name.                                     */
/* ================================================================== */

#define P7_RES_BACKDROP   1
#define P7_FONT          12
#define P7_TITLE_Y      110
#define P7_LIST_Y       160
#define P7_ROW_DY        26
#define P7_BTN_Y        430
#define P7_COL_X         92
#define P7_REFRESH_MS  2000

static MR_Sprite s_p7_backdrop;
static MR_Font   s_p7_font;
static int       s_p7_ok;
static int       s_p7_fontOk;
static DWORD     s_p7_lastList;
static char      s_p7_rows[NET_ROOMS_MAX][48];
static int       s_p7_rowRoom[NET_ROOMS_MAX];
static char      s_p7_status[64];

/* ORIGIN  NONE - ours */
static void p7_set_status(const char *text)
{
	int i;

	for (i = 0; i < (int)sizeof(s_p7_status) - 1 && text[i]; i++)
		s_p7_status[i] = text[i];
	s_p7_status[i] = 0;
}

/* ORIGIN  NONE - ours */
static void page7_enter(void)
{
	s_p7_ok = load_res(&s_p7_backdrop, P7_RES_BACKDROP, 0, 0,
	                   SPRITE_ANCHOR_NONE, "page7");
	s_p7_fontOk = fontdef_open(&s_p7_font, P7_FONT);
	s_p7_status[0] = 0;
	s_p7_lastList  = 0;
	mrlog("page7: game list - %s\n", s_p7_ok ? "loaded" : "INCOMPLETE");
}

/* ORIGIN  NONE - ours */
static void page7_leave(void)
{
	sprite_free(&s_p7_backdrop);
	if (s_p7_fontOk) {
		font_free(&s_p7_font);
		s_p7_fontOk = 0;
	}
}

/* ORIGIN  NONE - ours */
int page7_on_key(int vkey)
{
	return menu_buttons_key(vkey);
}

/* ORIGIN  NONE - ours */
static void page7_tick(void)
{
	int  n, i, created, back, first;
	int  which;
	char line[80];

	if (!s_p7_ok)
		return;

	sprite_update_wave(&s_p7_backdrop, -1, 1);
	sprite_draw_wave(&s_p7_backdrop);

	netplay_poll();
	netplay_pump();

	/* The relay went away; the connect page owns reconnecting. */
	if (netplay_state() != NET_BROWSING &&
	    netplay_state() != NET_JOINED) {
		menu_begin_transition(MENU_PAGE_PROTOCOL);
		return;
	}
	/* In a room now - the participants page takes over. */
	if (netplay_state() == NET_JOINED) {
		menu_begin_transition(MENU_PAGE_LOBBY);
		return;
	}

	if (GetTickCount() - s_p7_lastList > P7_REFRESH_MS) {
		netplay_request_list();
		s_p7_lastList = GetTickCount();
	}

	if (netplay_refusal() == NET_REFUSE_STATE)
		p7_set_status("that game has already started");
	else if (netplay_refusal() == NET_REFUSE_FULL)
		p7_set_status("that game is full");

	/*
	 * THE ROWS ARE BUILT EVERY FRAME from the directory, and their
	 * text has to outlive this function because menu_add_text_button
	 * keeps the pointer rather than a copy.
	 */
	n = netplay_room_count();
	if (n > NET_ROOMS_MAX)
		n = NET_ROOMS_MAX;

	menu_clear_buttons();
	first = -1;
	for (i = 0; i < n; i++) {
		const MR_NetRoom *r = netplay_room_at(i);
		int id;

		if (!r)
			continue;
		_snprintf(s_p7_rows[i], sizeof(s_p7_rows[i]),
		          "%-8s  %d/%d  %s", r->name, r->players,
		          NET_PEER_MAX - 1,
		          (r->state == NET_ROOM_RACING) ? "RACING" : "WAITING");
		s_p7_rowRoom[i] = r->id;
		if (!s_p7_fontOk)
			continue;
		id = menu_add_text_button(s_p7_rows[i], &s_p7_font, P7_COL_X,
		                          P7_LIST_Y + i * P7_ROW_DY, 0, 0);
		if (first < 0)
			first = id;
	}

	created = back = -1;
	if (s_p7_fontOk) {
		created = menu_add_text_button("CREATE A GAME", &s_p7_font,
		                               P7_COL_X, P7_BTN_Y, 0, 0);
		back = menu_add_text_button("DISCONNECT", &s_p7_font,
		                            P7_COL_X + 240, P7_BTN_Y,
		                            VK_ESCAPE, 0);
	}

	menu_hit_test();
	menu_draw_buttons();

	if (s_p7_fontOk) {
		_snprintf(line, sizeof(line), "LINK GAMES        YOU ARE %s",
		          g_playerName[0] ? g_playerName : "PLAYER");
		font_draw_text(&s_p7_font, P7_COL_X, P7_TITLE_Y, line, 0);
		if (n == 0)
			font_draw_text(&s_p7_font, P7_COL_X, P7_LIST_Y,
		                   "no games yet - create one", 0);
		if (s_p7_status[0])
			font_draw_text(&s_p7_font, P7_COL_X, P7_BTN_Y - P7_ROW_DY,
		                   s_p7_status, 0);
	}

	which = menu_get_activated();
	if (which < 0)
		return;
	menu_clear_activated();

	if (which == created) {
		netplay_create_room();
		p7_set_status("creating...");
	} else if (which == back) {
		netplay_close();
		menu_begin_transition(MENU_PAGE_PROTOCOL);
	} else if (first >= 0 && which >= first && which < first + n) {
		netplay_join_room(s_p7_rowRoom[which - first]);
		p7_set_status("joining...");
	}
}


/* ================================================================== */
/* PAGE 8 - THE PARTICIPANTS, AND THE CHAT.  OURS.                    */
/*                                                                    */
/* Plain text. The roster comes from the one idempotent LOBBY message  */
/* the host publishes - netplay-relay.txt 5.2 - and the chat scrolls   */
/* up like an IRC window.                                             */
/* ================================================================== */

#define P8_RES_BACKDROP   1
#define P8_FONT          12
#define P8_TITLE_Y       18
#define P8_ROSTER_Y      44    /* two fit; a third lands on the
                                * photo, which is wanted      */
#define P8_ROW_DY        24
#define P8_CHAT_Y       320
#define P8_CHAT_DY       22
#define P8_INPUT_Y      428
#define P8_BIKE_X       340
#define P8_BIKE_Y       220
#define P8_LAPS_X       180
#define P8_LAPS_Y       300
#define P8_BTN_Y        440
#define P8_COL_X         92
#define P8_CHAT_SHOWN     5

/* menu-pages.txt 2.10.1 for the layout. PHOTO.LEZ is 237x258. */
#define P8_PHOTO_X      120
#define P8_PHOTO_Y       96
#define P8_PHOTO_PCT     55

/*
 * A BUTTON SPRITE IS DRAWN NATIVE, because sprite_hit measures the
 * cursor against s->width and s->height - scale one and the hit
 * rectangle stops matching the art. Only the digits are scaled;
 * they are drawn, not clicked.
 */
#define P8_ARROW_Y      110    /* LEFT/RIGHT are 63x114  */
#define P8_ARROW_L_X     68    /* ~half on the photo     */
#define P8_ARROW_R_X    250
#define P8_LAP_Y        246
#define P8_MOIN_X       100    /* 62x53                  */
#define P8_PLUS_X       234    /* 67x54                  */
#define P8_DIGIT_PCT     60    /* NUM0 is 53x62          */
#define P8_DIGIT_Y      254
#define P8_DIGIT_TENS_X 168
#define P8_DIGIT_UNITS_X 200
#define P8_DIGITS        10

/*
 * THE RIGHT COLUMN. g_viewCentreX/Y move the projection, so the
 * bike draws off to the side with no change to the renderer, and a
 * bigger radius than page 10 IS the shrink.
 */
#define P8_BIKE_CX      455
#define P8_BIKE_CY      150
#define P8_BIKE_RADIUS 2200.0f
#define P8_BIKE_ANGLE    0.6f    /* a fixed three-quarter view  */
#define P8_BARROW_Y      93
#define P8_BARROW_L_X   320
#define P8_BARROW_R_X   528
#define P8_BAR_X        420
#define P8_BAR0_Y       260
#define P8_BAR_STEP      27
#define P8_BARS           4
#define P8_START_DELAY 1500   /* ms of margin on a scheduled start */

static MR_Sprite s_p8_backdrop;
static MR_Sprite s_p8_photo;
static MR_Sprite s_p8_left;
static MR_Sprite s_p8_right;
static MR_Sprite s_p8_moin;
static MR_Sprite s_p8_plus;
static MR_Sprite s_p8_digit[P8_DIGITS];
static int       s_p8_digitOk[P8_DIGITS];
static int       s_p8_artOk;
static MR_Sprite s_p8_bleft;
static MR_Sprite s_p8_bright;
static MR_Sprite s_p8_bar[P8_BARS];
static int       s_p8_barOk[P8_BARS];
static int       s_p8_photoOk;
static int       s_p8_photoTrack = -1;
static int       s_p8_photoPocket = -1;
static MR_Font   s_p8_font;
static int       s_p8_ok;
static int       s_p8_fontOk;
static int       s_p8_typing;

/*
 * THE SCHEDULED START, already translated into OUR clock by
 * netplay.c's offset - netplay-relay.txt 5.5. Set by the host for
 * itself and by everyone else out of START, because a broadcast
 * excludes the sender and the host never receives its own.
 */
static int          s_net_starting;
static unsigned int s_net_startAt;

/* ORIGIN  NONE - ours */
static void page8_enter(void)
{
	s_p8_ok = load_res(&s_p8_backdrop, P8_RES_BACKDROP, 0, 0,
	                   SPRITE_ANCHOR_NONE, "page8");
	s_p8_fontOk = fontdef_open(&s_p8_font, P8_FONT);
	s_p8_typing = 0;

	/* Page 6 owns these ids; a lobby just places them STATIC. */
	s_p8_artOk  = load_res(&s_p8_left,  P6_RES_LEFT,
	                       P8_ARROW_L_X, P8_ARROW_Y,
	                       SPRITE_ANCHOR_NONE, "page8");
	s_p8_artOk &= load_res(&s_p8_right, P6_RES_RIGHT,
	                       P8_ARROW_R_X, P8_ARROW_Y,
	                       SPRITE_ANCHOR_NONE, "page8");
	s_p8_artOk &= load_res(&s_p8_moin,  P6_RES_MOIN,
	                       P8_MOIN_X, P8_LAP_Y,
	                       SPRITE_ANCHOR_NONE, "page8");
	s_p8_artOk &= load_res(&s_p8_plus,  P6_RES_PLUS,
	                       P8_PLUS_X, P8_LAP_Y,
	                       SPRITE_ANCHOR_NONE, "page8");
	{
		int i;

		for (i = 0; i < P8_DIGITS; i++)
			s_p8_digitOk[i] = load_res(&s_p8_digit[i],
		                               P6_RES_NUM0 + i, 0, 0,
		                               SPRITE_ANCHOR_NONE, "page8");
	}

	/* The bike arrows are page 10's, and the bars with them. */
	s_p8_artOk &= load_res(&s_p8_bleft,  P10_RES_LEFT,
	                       P8_BARROW_L_X, P8_BARROW_Y,
	                       SPRITE_ANCHOR_NONE, "page8");
	s_p8_artOk &= load_res(&s_p8_bright, P10_RES_RIGHT,
	                       P8_BARROW_R_X, P8_BARROW_Y,
	                       SPRITE_ANCHOR_NONE, "page8");
	{
		int i;

		for (i = 0; i < P8_BARS; i++)
			s_p8_barOk[i] = load_res(&s_p8_bar[i],
		                                 P10_RES_FEATACC + i,
		                                 P8_BAR_X,
		                                 P8_BAR0_Y + i * P8_BAR_STEP,
		                                 SPRITE_ANCHOR_NONE, "page8");
	}


	/* NOT RACING: BEGIN sets it and nothing cleared it, so one race
	 * made the room unjoinable. netplay-relay.txt 2.5. */
	netplay_set_room_racing(0);

	/*
	 * THE ISLAND LIST, because page6_enter is the only other caller
	 * and a link game never visits page 6 - netplay-relay.txt 4.3.
	 * Both roles need it.
	 */
	if (tracks_count() == 0)
		tracks_load("LIST.BPT");

	/*
	 * AND THE CARAC TABLE, which page 10 is otherwise the only
	 * loader of. PER TRACK, so it reloads on a track change.
	 */
	bikes_load_for_track(track_at(g_selectedTrack), g_pocketEnabled);

	/*
	 * THE HOST PUBLISHES ITSELF, and without this it shows "waiting
	 * for the host" in its own game: the roster only ever got built
	 * when somebody JOINED, so a host alone in a room had no lobby
	 * at all and drew the empty case.
	 */
	if (netplay_is_host() && netplay_lobby() == NULL) {
		MR_NetLobby     lobby;
		int             slot = netplay_self();
		const MR_Track *t;
		int             i;

		/*
		 * THE HOST PICKS THE TRACK, because a link game never visits
		 * page 6 and page 1 has no fallback - netplay-relay.txt 4.3.
		 * An OFF track is fair game here; availability gates SOLO
		 * selection only [verified, 0x00447920].
		 */
		if (g_selectedTrack < 0 || g_selectedTrack >= tracks_count())
			g_selectedTrack = 0;
		t = track_at(g_selectedTrack);
		if (g_raceLaps < 1)
			g_raceLaps = (t && t->laps > 0) ? t->laps : 1;

		/*
		 * MODE 0 IS SINGLE RACE HERE, not practice - the numbering
		 * differs between the two shapes [verified, 0x004478a6].
		 */
		g_selectedMode = 0;

		memset(&lobby, 0, sizeof(lobby));
		lobby.track = g_selectedTrack;
		lobby.laps  = g_raceLaps;
		lobby.mode  = g_selectedMode;
		lobby.pocket     = g_pocketEnabled;
		lobby.reverse    = g_reverseEnabled;
		lobby.difficulty = g_options.difficulty;
		if (slot >= 1 && slot < NET_PEER_MAX) {
			lobby.players[slot].dpId = slot;
		lobby.bikes[slot]       = g_selectedBike;
			for (i = 0; i < (int)sizeof(lobby.players[slot].name)
				 - 1 && g_playerName[i]; i++)
			    lobby.players[slot].name[i] = g_playerName[i];
			lobby.players[slot].name[i] = 0;
		}
		netplay_publish_lobby(&lobby);
	}

	/*
	 * A GUEST ANNOUNCES ITSELF. The host publishes its lobby before
	 * any guest is in the room, and a broadcast only reaches current
	 * members - so without this a guest is in nobody's roster and
	 * gets no lobby, which also makes it ignore START.
	 */
	if (!netplay_is_host())
		netplay_send_join(g_playerName, g_selectedBike);

	mrlog("page8: participants - %s, %s\n",
	      s_p8_ok ? "loaded" : "INCOMPLETE",
	      netplay_is_host() ? "host" : "guest");
}

/*
 * ORIGIN  NONE - ours. Page 6 keeps one photo per track and slides
 *         them; a lobby needs the current one, static.
 *
 * EVERYTHING HOST-AUTHORITATIVE IN ONE GUARD - the TRACK INDEX and
 * the POCKET FLAG rather than a button, because on a guest both
 * arrive from the host's republished lobby and no key is ever
 * pressed. p8_host_set reloads the carac on its own step, which
 * covers the host and nobody else.
 *
 * THE BIKE MESH IS PER TRACK TOO - every bike on a track is the
 * same model and the choice is a palette, so it reloads here and
 * not when the rider steps the bike.
 */
static void p8_load_track_art(void)
{
	const MR_Track *t;
	char            name[TRACK_PATH_MAX];

	if (s_p8_photoTrack == g_selectedTrack &&
	    s_p8_photoPocket == g_pocketEnabled)
	    return;
	if (s_p8_photoOk) {
		sprite_free(&s_p8_photo);
		s_p8_photoOk = 0;
	}
	s_p8_photoTrack  = g_selectedTrack;
	s_p8_photoPocket = g_pocketEnabled;

	t = track_at(g_selectedTrack);
	bikes_load_for_track(t, g_pocketEnabled);
	p10_free_bike();
	p10_load_bike();

	if (!t || !track_asset_name(t, "PHOTO.LEZ", name, (int)sizeof(name)))
		return;
	if (!sprite_load(&s_p8_photo, name, 1)) {
		mrlog_err("page8:  %s did not load\n", name);
		return;
	}
	s_p8_photoOk = 1;
}


/* ORIGIN  p6_draw_digit @ page 6, scaled - see P8_DIGIT_PCT. */
static void p8_draw_digit(int value, int x)
{
	int at[2];

	if (value < 0 || value >= P8_DIGITS || !s_p8_digitOk[value])
		return;
	at[0] = x;
	at[1] = P8_DIGIT_Y;
	sprite_set_position(&s_p8_digit[value], at, SPRITE_ANCHOR_NONE);
	sprite_draw_scaled(&s_p8_digit[value], SPRITE_MODE_USE_KIND,
	                   P8_DIGIT_PCT);
}


static void p8_draw_laps(void)
{
	int laps = g_raceLaps;

	p8_draw_digit(laps / 10, P8_DIGIT_TENS_X);
	p8_draw_digit(laps % 10, P8_DIGIT_UNITS_X);
}


/*
 * ORIGIN  NONE - ours
 *
 * PAGE 10'S DRAW, MOVED AND SHRUNK. g_viewCentreX/Y are the
 * projection centre and s_p10_camRadius the orbit, so both come
 * back exactly as they were - page 10 is entered afterwards and
 * must not inherit a lobby camera.
 *
 * THE ANGLE IS FIXED. Nothing on this page animates, so the orbit
 * is parked at a three-quarter view instead of being advanced.
 */
static void page8_draw_3d(void)
{
	int   cx = g_viewCentreX;
	int   cy = g_viewCentreY;
	float r  = s_p10_camRadius;
	float a  = s_p10_camAngle;

	g_viewCentreX   = P8_BIKE_CX + g_fb2dOriginX;
	g_viewCentreY   = P8_BIKE_CY;
	s_p10_camRadius = P8_BIKE_RADIUS;
	s_p10_camAngle  = P8_BIKE_ANGLE;

	page10_draw_3d();

	g_viewCentreX   = cx;
	g_viewCentreY   = cy;
	s_p10_camRadius = r;
	s_p10_camAngle  = a;
}


static void page8_leave(void)
{
	sprite_free(&s_p8_backdrop);
	if (s_p8_photoOk) {
		sprite_free(&s_p8_photo);
		s_p8_photoOk = 0;
	}
	s_p8_photoTrack = -1;
	if (s_p8_artOk) {
		sprite_free(&s_p8_left);
		sprite_free(&s_p8_right);
		sprite_free(&s_p8_moin);
		sprite_free(&s_p8_plus);
		s_p8_artOk = 0;
	}
	{
		int i;

		for (i = 0; i < P8_DIGITS; i++) {
			if (s_p8_digitOk[i]) {
				sprite_free(&s_p8_digit[i]);
				s_p8_digitOk[i] = 0;
			}
		}
		for (i = 0; i < P8_BARS; i++) {
			if (s_p8_barOk[i]) {
				sprite_free(&s_p8_bar[i]);
				s_p8_barOk[i] = 0;
			}
		}
	}
	sprite_free(&s_p8_bleft);
	sprite_free(&s_p8_bright);
	p10_free_bike();
	if (s_p8_fontOk) {
		font_free(&s_p8_font);
		s_p8_fontOk = 0;
	}
}

/* ORIGIN  NONE - ours */
int page8_on_key(int vkey)
{
	return menu_buttons_key(vkey);
}

/* ORIGIN  NONE - ours */
static void p8_draw_roster(void)
{
	const MR_NetLobby *lobby = netplay_lobby();
	char line[80];
	int  i;
	int  y = P8_ROSTER_Y;

	if (!lobby) {
		font_draw_text(&s_p8_font, P8_COL_X, y,
		               "waiting for the host...", 0);
		return;
	}
	for (i = 1; i < NET_PEER_MAX; i++) {
		const MR_NetPlayer *pl = &lobby->players[i];

		if (pl->dpId == 0)
			continue;
		_snprintf(line, sizeof(line), "%-8s%s", pl->name,
		          (i == lobby->hostSlot) ? "  (hosting)" : "");
		font_draw_text(&s_p8_font, P8_COL_X, y, line, 0);
		y += P8_ROW_DY;
	}
}

/*
 * ORIGIN  NONE - ours
 *
 * OLDEST AT THE TOP, newest at the bottom, like an IRC window - so
 * the last line typed is always the one nearest the input.
 */
static void p8_draw_chat(void)
{
	char line[120];
	int  total = netplay_chat_count();
	int  first = (total > P8_CHAT_SHOWN) ? total - P8_CHAT_SHOWN : 0;
	int  i;
	int  y = P8_CHAT_Y;

	for (i = first; i < total; i++) {
		const MR_NetLobby *lobby = netplay_lobby();
		int                who   = 0;
		const char        *text  = netplay_chat_at(i, &who);
		const char        *name  = "?";

		if (!text)
			continue;
		if (lobby && who >= 1 && who < NET_PEER_MAX &&
		    lobby->players[who].name[0])
		    name = lobby->players[who].name;
		_snprintf(line, sizeof(line), "<%s> %s", name, text);
		font_draw_text(&s_p8_font, P8_COL_X, y, line, 0);
		y += P8_CHAT_DY;
	}
}

/*
 * ORIGIN  MenuPage08_NetRaceLobby_Tick @ 0x0044aee0, the
 *         selection half - netplay-relay.txt 4.3.
 *
 * HOST ONLY. Republishing is how a guest learns; it has no track
 * UI of its own.
 */
static void menu_net_on_ready(int slot, int ready, int bike);

static void p8_host_set(int trackStep, int lapStep)
{
	const MR_NetLobby *cur = netplay_lobby();
	MR_NetLobby        next;
	const MR_Track    *t;
	int                n = tracks_count();

	if (!netplay_is_host() || !cur || n <= 0)
		return;
	next = *cur;

	if (trackStep) {
		next.track += trackStep;
		if (next.track < 0)
			next.track = n - 1;
		if (next.track >= n)
			next.track = 0;
		/*
		 * THE LAPS FOLLOW THE TRACK, as page 6 does - a lap count
		 * carried over from another island is not what the record
		 * says.
		 */
		t = track_at(next.track);
		next.laps = (t && t->laps > 0) ? t->laps : 1;
	}
	if (lapStep) {
		next.laps += lapStep;
		if (next.laps < P6_LAPS_MIN)
			next.laps = P6_LAPS_MIN;
		if (next.laps > P6_LAPS_MAX)
			next.laps = P6_LAPS_MAX;
	}

	g_selectedTrack = next.track;
	g_raceLaps      = next.laps;
	if (trackStep)
		bikes_load_for_track(track_at(next.track), g_pocketEnabled);
	netplay_publish_lobby(&next);
}

/* ORIGIN  NONE - ours */
static void page8_tick(void)
{
	char line[120];
	int  btnChat, btnBegin, btnLeave, which;
	int  btnTrkL, btnTrkR, btnLapL, btnLapR;
	int  btnBikeL, btnBikeR;

	if (!s_p8_ok)
		return;

	sprite_update_wave(&s_p8_backdrop, -1, 1);
	sprite_draw_wave(&s_p8_backdrop);

	/* Over the backdrop, under the text - the roster is allowed to
	   sit on it past two players. */
	p8_load_track_art();
	if (s_p8_photoOk) {
		int at[2];

		at[0] = P8_PHOTO_X;
		at[1] = P8_PHOTO_Y;
		sprite_set_position(&s_p8_photo, at, SPRITE_ANCHOR_NONE);
		sprite_draw_scaled(&s_p8_photo, SPRITE_MODE_USE_KIND,
		                   P8_PHOTO_PCT);
	}

	/*
	 * THE HOST ALONE STEPS THE TRACK AND THE LAPS, so a guest is
	 * shown neither control - it has nothing to press and the
	 * photo is the only thing that tells it where it is racing.
	 */
	if (s_p8_artOk && netplay_is_host()) {
		sprite_draw(&s_p8_left,  SPRITE_MODE_USE_KIND, 100, 0);
		sprite_draw(&s_p8_right, SPRITE_MODE_USE_KIND, 100, 0);
		sprite_draw(&s_p8_moin,  SPRITE_MODE_USE_KIND, 100, 0);
		sprite_draw(&s_p8_plus,  SPRITE_MODE_USE_KIND, 100, 0);
	}
	p8_draw_laps();

	/*
	 * THE BIKE ARROWS AND ITS BARS. Everyone picks their own bike,
	 * so unlike the track pair these are not host-only.
	 *
	 * shrink IS THE BAR LENGTH - page 10 drives the same four
	 * sprites with it. Static here, so the value goes straight in
	 * with no tween.
	 */
	if (s_p8_artOk) {
		int i;

		sprite_draw(&s_p8_bleft,  SPRITE_MODE_USE_KIND, 100, 0);
		sprite_draw(&s_p8_bright, SPRITE_MODE_USE_KIND, 100, 0);
		for (i = 0; i < P8_BARS; i++) {
			if (!s_p8_barOk[i])
				continue;
			/* p10_bar_target IS ALREADY 100 - value, which is what
			   shrink wants - do not invert it again. */
			sprite_draw(&s_p8_bar[i], SPRITE_MODE_USE_KIND, 100,
			            p10_bar_target(i));
		}
	}

	netplay_poll();
	netplay_pump();

	/*
	 * THE ROOM WENT - the host left, or the relay dropped it. The
	 * transport puts us back to browsing, so the list page is where
	 * this belongs rather than sitting on a dead roster.
	 */
	if (netplay_state() != NET_JOINED) {
		menu_begin_transition(netplay_state() == NET_BROWSING
		                      ? MENU_PAGE_SESSION
		                      : MENU_PAGE_PROTOCOL);
		return;
	}

	/* A finished line goes out; an abandoned one just closes. */
	if (s_p8_typing && !menu_capture_active()) {
		if (menu_capture_done() && menu_capture_len() > 0)
			netplay_send_chat(menu_capture_text());
		menu_capture_clear_result();
		s_p8_typing = 0;
	}

	/*
	 * THE INSTANT ARRIVED. Page 1 is the loading screen and its enter
	 * handler is what calls race_load, so a transition there IS
	 * starting the race.
	 *
	 * COMPARED AS A SIGNED DIFFERENCE, because GetTickCount wraps
	 * every 49 days and `now >= then` is wrong across the wrap while
	 * `now - then >= 0` is right.
	 */
	if (s_net_starting &&
	    (int)(GetTickCount() - s_net_startAt) >= 0) {
		s_net_starting = 0;
		mrlog("net: starting the race\n");
		menu_begin_transition(MENU_PAGE_LOADING);
		return;
	}

	menu_clear_buttons();
	btnChat = btnBegin = btnLeave = -1;
	btnTrkL = btnTrkR = btnLapL = btnLapR = -1;
	btnBikeL = btnBikeR = -1;
	if (s_p8_fontOk && !s_p8_typing) {
		if (netplay_is_host() && s_p8_artOk) {
			btnTrkL = menu_add_sprite_button(&s_p8_left,  NULL, 0);
			btnTrkR = menu_add_sprite_button(&s_p8_right, NULL, 0);
			btnLapL = menu_add_sprite_button(&s_p8_moin,  NULL, 0);
			btnLapR = menu_add_sprite_button(&s_p8_plus,  NULL, 0);
		}
		/*
		 * THE BIKE IS PER PLAYER, not the host's to set - the
		 * original sends 0xcf from each machine and enforces no
		 * uniqueness, so two riders may pick the same one.
		 */
		if (s_p8_artOk) {
			btnBikeL = menu_add_sprite_button(&s_p8_bleft,  NULL, 0);
			btnBikeR = menu_add_sprite_button(&s_p8_bright, NULL, 0);
		}
		btnChat = menu_add_text_button("SAY", &s_p8_font, P8_COL_X,
		                               P8_BTN_Y, 0, 0);
		if (netplay_is_host())
			btnBegin = menu_add_text_button("BEGIN", &s_p8_font,
		                                    P8_COL_X + 120, P8_BTN_Y, 0, 0);
		btnLeave = menu_add_text_button("LEAVE", &s_p8_font,
		                                P8_COL_X + 280, P8_BTN_Y,
		                                VK_ESCAPE, 0);
	}

	menu_hit_test();
	menu_draw_buttons();

	if (s_p8_fontOk) {
		font_draw_text(&s_p8_font, P8_COL_X, P8_TITLE_Y,
		               netplay_is_host() ? "YOUR GAME"
		                                 : "WAITING TO RACE", 0);
		_snprintf(line, sizeof(line), "   BIKE %d OF %d",
		          g_selectedBike + 1, bikes_count());
		font_draw_text(&s_p8_font, P8_BIKE_X, P8_BIKE_Y, line, 0);
		font_draw_text(&s_p8_font, P8_LAPS_X, P8_LAPS_Y, "LAPS", 0);
		p8_draw_roster();
		p8_draw_chat();
		if (s_p8_typing) {
			_snprintf(line, sizeof(line), "> %s%s", menu_capture_text(),
			          menu_capture_cursor_on() ? "_" : "");
			font_draw_text(&s_p8_font, P8_COL_X, P8_INPUT_Y, line, 0);
		}
	}

	which = menu_get_activated();
	if (which < 0)
		return;
	menu_clear_activated();

	if (which == btnBikeL || which == btnBikeR) {
		int n = bikes_count();

		if (n > 0) {
			g_selectedBike += (which == btnBikeR) ? 1 : -1;
			if (g_selectedBike < 0)
				g_selectedBike = n - 1;
			if (g_selectedBike >= n)
				g_selectedBike = 0;
			/* READY carries it; the host records and republishes. */
			if (netplay_is_host())
				menu_net_on_ready(netplay_self(), 1, g_selectedBike);
			else
				netplay_send_ready(1, g_selectedBike);
		}
	} else if (which == btnTrkL) {
		p8_host_set(-1, 0);
	} else if (which == btnTrkR) {
		p8_host_set(1, 0);
	} else if (which == btnLapL) {
		p8_host_set(0, -1);
	} else if (which == btnLapR) {
		p8_host_set(0, 1);
	} else if (which == btnChat) {
		s_p8_typing = 1;
		menu_capture_begin(NULL, NET_CHAT_MAX - 1);
	} else if (which == btnBegin) {
		/*
		 * A SCHEDULED INSTANT, not a go-now: every machine starts when
		 * its own clock reaches it - netplay-relay.txt 5.5. The room is
		 * closed to joins first, so nobody arrives mid-countdown.
		 *
		 * AND THE HOST SCHEDULES ITSELF. A broadcast excludes the
		 * sender, so the host never receives its own START and would
		 * sit in the lobby while everybody else raced.
		 */
		netplay_set_room_racing(1);
		netplay_send_start(netplay_host_now() + P8_START_DELAY);
		s_net_startAt  = GetTickCount() + P8_START_DELAY;
		s_net_starting = 1;
	} else if (which == btnLeave) {
		netplay_leave_room();
		menu_begin_transition(MENU_PAGE_SESSION);
	}
}


/* ================================================================== */
/* THE LOBBY HALF OF MR_NetHandlers.  race.c owns the race half.      */
/* ================================================================== */

/* ORIGIN  maybe_Net_Dispatch case 0x64 	 0x004552a0 */
static void menu_net_on_join(int slot, const char *name, int bike)
{
	MR_NetLobby next;
	const MR_NetLobby *cur;
	int i;

	mrlog("net: slot %d joined as \"%s\", bike %d\n", slot, name, bike);
	if (!netplay_is_host() || slot < 1 || slot >= NET_PEER_MAX)
		return;

	/*
	 * READ, MODIFY, PUBLISH - never keep a second roster. netplay.c
	 * holds the only copy and netplay_publish_lobby writes it, so
	 * going through it keeps one source of truth.
	 */
	cur = netplay_lobby();
	if (cur) {
		next = *cur;
	} else {
		memset(&next, 0, sizeof(next));
		next.track = g_selectedTrack;
		next.laps  = g_raceLaps;
		next.mode  = g_selectedMode;
		next.pocket     = g_pocketEnabled;
		next.reverse    = g_reverseEnabled;
		next.difficulty = g_options.difficulty;
		/* The host occupies its own slot the first time through. */
		next.players[netplay_self()].dpId = netplay_self();
		for (i = 0; i < (int)sizeof(next.players[0].name) - 1 &&
		     g_playerName[i]; i++)
		    next.players[netplay_self()].name[i] = g_playerName[i];
		next.players[netplay_self()].name[i] = 0;
	}
	next.players[slot].dpId = slot;
	for (i = 0; i < (int)sizeof(next.players[slot].name) - 1 && name[i];
	     i++)
	    next.players[slot].name[i] = name[i];
	next.players[slot].name[i] = 0;
	next.players[slot].ready = 0;
	next.bikes[slot]         = bike;
	netplay_publish_lobby(&next);
}

/* ORIGIN  maybe_Net_Dispatch case 0x67 	 0x004555c0 */
static void menu_net_on_leave(int slot)
{
	MR_NetLobby next;
	const MR_NetLobby *cur;

	mrlog("net: slot %d left\n", slot);
	cur = netplay_lobby();
	if (!netplay_is_host() || !cur || slot < 1 || slot >= NET_PEER_MAX)
		return;
	next = *cur;
	memset(&next.players[slot], 0, sizeof(next.players[slot]));
	netplay_publish_lobby(&next);
}

/* ORIGIN  maybe_Net_Dispatch case 0xca 	 0x00455c90 - host side */
static void menu_net_on_ready(int slot, int ready, int bike)
{
	MR_NetLobby next;
	const MR_NetLobby *cur;

	cur = netplay_lobby();
	if (!netplay_is_host() || !cur || slot < 1 || slot >= NET_PEER_MAX)
		return;
	next = *cur;
	next.players[slot].ready = ready ? 1 : 0;
	next.bikes[slot]         = bike;
	netplay_publish_lobby(&next);
}

/* ORIGIN  NONE - ours. Replaces 0xc8, 0xcb, 0x6b, 0xd0..0xd5. */
static void menu_net_on_lobby(const MR_NetLobby *lobby)
{
	/*
	 * HOST AUTHORITY OVER THE RACE SETTINGS, which is the original's
	 * shape too - 0x6b writes these three globals outright. A guest
	 * never chooses them, so nothing here is overwriting a choice.
	 */
	g_selectedTrack  = lobby->track;
	g_raceLaps       = lobby->laps;
	g_selectedMode   = lobby->mode;
	g_pocketEnabled  = lobby->pocket ? 1 : 0;
	g_reverseEnabled = lobby->reverse ? 1 : 0;
	mrlog("net: lobby - track %d, %d lap(s), mode %d, pocket %d,"
	      " reverse %d, host slot %d\n",
	      lobby->track, lobby->laps, lobby->mode, lobby->pocket,
	      lobby->reverse, lobby->hostSlot);
}

/* ORIGIN  maybe_Net_Dispatch case 0x6a / 0xce 	 0x004556a0 */
static void menu_net_on_start(unsigned int localStartMs)
{
	/*
	 * ALREADY IN OUR CLOCK - netplay.c subtracts the offset it
	 * measured, so this is a local GetTickCount instant and nothing
	 * here needs to know the host's.
	 */
	s_net_startAt  = localStartMs;
	s_net_starting = 1;
	mrlog("net: start at local %u (now %u)\n", localStartMs,
	      (unsigned)GetTickCount());
}

/* ORIGIN  maybe_Net_Dispatch case 0xd3 	 0x00456140 */
static void menu_net_on_chat(int slot, const char *text)
{
	mrlog("net: chat %d: %s\n", slot, text);
}

/*
 * ORIGIN  NONE - ours
 *
 * REGISTERED ON TOP OF race_net_install, which fills the race half
 * and leaves these five null. Both halves write the SAME struct, so
 * whichever runs second must not clear the other's - which is why
 * this reads the current set back rather than starting from zero.
 */
void menu_net_install(void)
{
	MR_NetHandlers h;

	race_net_install();
	netplay_get_handlers(&h);
	h.onJoin  = menu_net_on_join;
	h.onLeave = menu_net_on_leave;
	h.onLobby = menu_net_on_lobby;
	h.onStart = menu_net_on_start;
	h.onChat  = menu_net_on_chat;
	h.onReady = menu_net_on_ready;
	netplay_set_handlers(&h);
}


/* ORIGIN  NONE - ours */
static void page_table_init(void)
{
	memset(s_pages, 0, sizeof(s_pages));

	s_pages[MENU_PAGE_LOADING].fnTick  = page01_tick;
	s_pages[MENU_PAGE_LOADING].fnEnter = page01_enter;
	s_pages[MENU_PAGE_LOADING].fnLeave = page01_leave;

	s_pages[MENU_PAGE_BIKE].fnTick     = page10_tick;
	s_pages[MENU_PAGE_BIKE].fnEnter    = page10_enter;
	s_pages[MENU_PAGE_BIKE].fnLeave    = page10_leave;

	s_pages[MENU_PAGE_RACE].fnTick     = page6_tick;
	s_pages[MENU_PAGE_RACE].fnEnter    = page6_enter;
	s_pages[MENU_PAGE_RACE].fnLeave    = page6_leave;

	s_pages[MENU_PAGE_MODE].fnTick     = page5_tick;
	s_pages[MENU_PAGE_MODE].fnEnter    = page5_enter;
	s_pages[MENU_PAGE_MODE].fnLeave    = page5_leave;

	s_pages[MENU_PAGE_TITLE].fnTick    = page14_tick;
	s_pages[MENU_PAGE_TITLE].fnEnter   = page14_enter;
	s_pages[MENU_PAGE_TITLE].fnLeave   = page14_leave;

	s_pages[MENU_PAGE_OPTIONS].fnTick  = page11_tick;
	s_pages[MENU_PAGE_OPTIONS].fnEnter = page11_enter;
	s_pages[MENU_PAGE_OPTIONS].fnLeave = page11_leave;

	s_pages[MENU_PAGE_RESULTS].fnTick   = page9_tick;
	s_pages[MENU_PAGE_RESULTS].fnEnter  = page9_enter;
	s_pages[MENU_PAGE_RESULTS].fnLeave  = page9_leave;

	s_pages[MENU_PAGE_PROTOCOL].fnTick  = page2_tick;
	s_pages[MENU_PAGE_PROTOCOL].fnEnter = page2_enter;
	s_pages[MENU_PAGE_PROTOCOL].fnLeave = page2_leave;

	s_pages[MENU_PAGE_QUIT].fnTick     = page4_tick;
	s_pages[MENU_PAGE_MAIN].fnTick     = page3_tick;
	s_pages[MENU_PAGE_MAIN].fnEnter    = page3_enter;
	s_pages[MENU_PAGE_MAIN].fnLeave    = page3_leave;

	s_pages[MENU_PAGE_NAME].fnTick     = page15_tick;
	s_pages[MENU_PAGE_NAME].fnEnter    = page15_enter;
	s_pages[MENU_PAGE_NAME].fnLeave    = page15_leave;

	s_pages[MENU_PAGE_SESSION].fnTick  = page7_tick;
	s_pages[MENU_PAGE_SESSION].fnEnter = page7_enter;
	s_pages[MENU_PAGE_SESSION].fnLeave = page7_leave;

	s_pages[MENU_PAGE_LOBBY].fnTick    = page8_tick;
	s_pages[MENU_PAGE_LOBBY].fnEnter   = page8_enter;
	s_pages[MENU_PAGE_LOBBY].fnLeave   = page8_leave;

	s_pages[MENU_PAGE_DSILOGO].fnTick  = page25_tick;
	s_pages[MENU_PAGE_DSILOGO].fnEnter = page25_enter;
	s_pages[MENU_PAGE_DSILOGO].fnLeave = page25_leave;
}

/*
 * ORIGIN  Menu_SetPage @ 0x00409f70
 *
 * THE MENU RUNS ITS OWN PROJECTION, and it reinstates it on every
 * single page change rather than once at start-up. Read out of
 * 0x00409ffe..0x0040a041 [verified]:
 *
 *     Screen3D_SetProjBaseX(1911.0)      CALL at 0x0040a022
 *     Screen3D_SetProjBaseY(1911.0)      CALL at 0x0040a032, its arg
 *                                        pushed at 0x0040a027 as
 *                                        0x44eee000 - both get 1911
 *     [0x006d1f70] = 40000.0             0x0040a013, was 16000 at open
 *     [0x00625774] = 256.0               0x0040a037, the near plane
 *     [0x004ee9f0] = 1, [0x0062e444] = 1
 *
 * o 1911, NOT 480. Screen3D_Open computes 480 from the resolution -
 *   see docs/subsystems/projection.txt - and that is the value the
 *   RACE uses. The menu overrides it with a much longer lens: a 19
 *   degree horizontal field against the 67 the race gets. That is
 *   what makes menu models read as near-orthographic, and it is why
 *   page 14 can put an object at z = -40000 and still have it be a
 *   recognisable shape rather than a dot.
 *
 * o THE FAR DISTANCE GOES OUT TO EXACTLY 40000, which is also page
 *   14's start z. The menu widens its own depth range far enough for
 *   the title animation.
 *
 *   [verified constants; the causal link is INFERRED and the
 *   argument once offered for it - that 40000 appears nowhere else -
 *   is false. 0x471c4000 is written to 0x006d1f70 at three sites:
 *   here, InGameMenu_OnContinueNo at 0x00436313 and
 *   InGameMenu_OnQuitRace at 0x004379cc. The other two are the paths
 *   from a race back into the menus, so 40000 is the MENU far
 *   distance generally, re-asserted wherever the menu takes over.]
 *
 * o The near plane is 256, not 1. It is documented as
 *   maybe_g_nearPlaneZ in rasterizer-pipeline.txt 13.4, already noted
 *   there as "written by Menu_SetPage".
 *
 * Doing this per page change rather than once is not redundancy: the
 * race and the menu genuinely disagree about the lens, and whichever
 * ran last owns the globals.
 */
void menu_set_page(int page)
{
	if (page < 0 || page >= MENU_PAGE_COUNT)
		return;
	s_page = page;

	/*
	 * THE MENU SOUND BANK COMES BACK HERE, ON EVERY PAGE
	 * CHANGE, and that is the original's own arrangement:
	 * Menu_SetPage calls Menu_LoadResources, which calls
	 * Snd_LoadMenuBank at 0x0040b0c8.  So entering ANY menu
	 * page restores it.
	 *
	 * WITHOUT IT A RACE LEAVES ITS OWN BANK RESIDENT and the
	 * menus run on it - where 0x29 is Divers\select rather
	 * than Menu\select, and every id the race bank does not
	 * carry is simply absent.  That is audible as WRONG, THEN
	 * SILENT rather than as a failure: the hover blip still
	 * makes a noise because Divers happens to have something
	 * at that id, while the glass shatter and every menu voice
	 * line go quiet with nothing logged.
	 *
	 * ONE SEAM COVERS EVERY ROUTE, including the continue menu's
	 * NO, which goes straight to page 6 without passing page 9.
	 *
	 * GUARDED, WHERE THE ORIGINAL RELOADS UNCONDITIONALLY.
	 * Reloading an identical bank costs a free and a re-read of
	 * every menu sample for no change, and this port pays that
	 * out of the archives.  [deliberate divergence]
	 */
	if (audio_current_bank() != AUDIO_BANK_MENU)
		audio_load_bank(AUDIO_BANK_MENU);
	s_pages[page].initialised = 0;

	/* The button list is page-local, so a page change empties it and
	 * drops any hover or activation left over from the old page. */
	s_buttonCount  = 0;
	s_hoverIndex   = -1;
	s_activated    = -1;
	s_repeatButton = -1;

	screen3d_set_proj_base_x(1911.0f);
	screen3d_set_proj_base_y(1911.0f);
	screen3d_set_proj_zoom(1.0f);
	g_nearPlaneZ = 256.0f;

	/*
	 * The far distance, and it is what makes the depth buffer work.
	 *
	 * Menu_SetPage writes 40000.0 to [0x006d1f70], which Screen3D_Open
	 * had set to 16000.0 - MOV EAX,0x467a0000 at 0x00499607
	 * [verified].
	 *
	 * A far/depth range is the reading [inferred], and the WRITER SET
	 * is the argument for it. All five references are writes and the
	 * binary contains no reader at all:
	 *
	 *     0x00499607  Screen3D_Open          16000, the default
	 *     0x0040a013  Menu_SetPage           40000, the menu's
	 *     0x00410e59  Race_SetupRenderState  a computed value
	 *     0x00436313  InGameMenu_OnContinueNo
	 *     0x004379cc  InGameMenu_OnQuitRace  40000, restoring it
	 *
	 * Open sets a default, each mode sets its own, and the two
	 * in-game-menu handlers put the menu's back on the way out of a
	 * race. That is the shape of a mode-owned projection global,
	 * whoever ends up reading it.
	 *
	 * o USING IT FOR THE D3D DEPTH RANGE IS A PORT DECISION, not a
	 *   transcription. g_drawSceneZFar had never been assigned, so it
	 *   sat at its 1.0 initialiser and every vertex clamped to sz = 1:
	 *   the depth buffer was enabled, fed a constant, and did nothing.
	 *   Draw order then fell entirely to the queue's bucket sort, whose
	 *   key is __ftol(camZ) in WORLD UNITS - so a 215-vertex model only
	 *   tens of units deep put most of its faces in one bucket and drew
	 *   them in list order, which on a solid model paints back faces
	 *   over front ones.
	 *
	 *
	 * o IT IS ALSO THE FAR PLANE NOW, and it clips. Scene geometry
	 *   goes to the renderer untransformed so D3D9 can clip it (see
	 *   r_set_projection), which means anything beyond this distance
	 *   is cut rather than flattened onto sz = 1. Every page gets a
	 *   value here, and that is why this assignment is unconditional.
	 */
	g_drawSceneZFar = 40000.0f;
}

/* ORIGIN  Menu_BeginTransition @ 0x0040c4a0 */
void menu_begin_transition(int page)
{
	if (page < 0 || page >= MENU_PAGE_COUNT)
		return;

	/*
	 * The capture is armed now and taken at the end of THIS frame,
	 * with the outgoing page still on screen. The page change waits
	 * for menu_apply_pending_page, which runs after it.
	 */
	glass_request();
	s_pendingPage = page;
}

/* ORIGIN  NONE - ours */
void menu_apply_pending_page(void)
{
	int page = s_pendingPage;

	if (page < 0)
		return;
	s_pendingPage = -1;
	menu_goto_page(page);
}

/* ORIGIN  Menu_GotoPage @ 0x00409eb0 */
void menu_goto_page(int page)
{
	if (s_page >= 0 && s_page < MENU_PAGE_COUNT &&
	    s_pages[s_page].initialised) {
		if (s_pages[s_page].fnLeave)
			s_pages[s_page].fnLeave();
		s_pages[s_page].initialised = 0;
	}
	menu_set_page(page);
}

/* ORIGIN  NONE - ours */
void menu_restart_page(void)
{
	menu_goto_page(s_page);
}

/*
 * ORIGIN  Menu_Tick @ 0x00409c00
 *
 * The lazy enter-then-tick is the original's shape exactly, indexing
 * g_menuPageTable as pageIndex*4 dwords with fnTick at 0x004dbb3c and
 * fnEnter at 0x004dbb40 [verified].
 *
 * MOST OF Menu_Tick IS NOT HERE, and it is spread across our frame
 * loop rather than dropped: the frame timestamp, the pending-request
 * dispatch, the video-settings reapply, the repeat-on-hold
 * auto-activation, and page 14's attract-demo trigger at 30000 ms.
 *
 * o THE TRANSITION IS ARMED HERE IN THE ORIGINAL, on the first tick
 *   after a page change: a 0x4b0 ms window, MenuTools_BuildViewMatrix,
 *   and Snd_PlayCue(0x2a, 0) - which is where the glass shatter comes
 *   from. Ours lives in glass.c, requested by menu_begin_transition.
 *
 * o IT ALSO CLEARS +0x10 ON EVERY g_menuObjects SPRITE, because those
 *   are shared globals and a half-finished slide would survive into
 *   the next page. Ours are per-page statics loaded in Enter, so
 *   there is nothing stale to clear.
 */
void menu_tick(void)
{
	MR_MenuPage *p;

	if (s_page < 0 || s_page >= MENU_PAGE_COUNT)
		return;
	p = &s_pages[s_page];

	if (!p->initialised) {
		if (p->fnEnter)
			p->fnEnter();
		p->initialised = 1;
	}
	if (p->fnTick)
		p->fnTick();
}

/*
 * ORIGIN  NONE - ours
 *
 * Our split, not the original's: its page ticks draw as they go,
 * bracketed by Screen3D_BeginFrame and EndFrame INSIDE the handler.
 * Ours are called from the frame loop's 2D phase, so the tick does the
 * drawing and this is only here to keep the loop's shape honest.
 */
void menu_draw(void)
{
	menu_tick();
}

/*
 * ORIGIN  NONE - ours, the scene-bracket split
 *
 * Inside the scene bracket - see menu_draw_page_3d. The camera eye,
 * entry 5, falls out on its own: this loops to P25_MODELS (5) where
 * page25_animate loops to P25_ANIM (6), which is the structural form
 * of the tick's "if (i != 5) ModelInstance_Draw".
 *
 * THE VIEW MATRIX IS NOT REBUILT PER FRAME even though the eye moves.
 * The tick writes camera+0x98 and never calls Camera_LookAt again, so
 * the camera TRANSLATES without re-aiming; ModelInstance_Draw takes
 * the difference against the eye and multiplies by the view Enter
 * built. Re-running the look-at here would keep the letters centred
 * and quietly destroy the fly-through.
 *
 * o AND PAGE 25 IS THE ONLY 3D MENU PAGE THAT DOES THIS. Of
 *   Camera_LookAt's thirteen callers, pages 10, 14 and 24 each call
 *   it from BOTH their enter and their tick; page 25 calls it only
 *   from Enter, at 0x00453b44 [verified]. Aiming once is what makes
 *   it a fly-through rather than an orbit.
 */
static void page25_draw_3d(void)
{
	int i;

	if (!s_p25_models_ok)
		return;

	queue_reset();
	vertexpool_reset();
	for (i = 0; i < P25_MODELS; i++)
		if (s_p25_mesh_ok[i])
			model_draw(&s_p25_inst[i], s_p25_view, s_p25_eye);
	queue_walk(drawscene_dispatch);
}

/*
 * ORIGIN  NONE - ours
 *
 * THE SCENE BRACKET, AND WHY NO PAGE TICK MAY DRAW GEOMETRY. main.cpp
 * calls this between r_begin_scene and r_end_scene. The frame loop's
 * 2D phase runs BEFORE the bracket opens, so a queue built and walked
 * from a page tick lands outside it and draws nothing - silently.
 * Sprites survive that, being written into the framebuffer in
 * software; the geometry path does not.
 *
 * The original has the same split without needing it stated: its page
 * ticks call Screen3D_BeginFrame and EndFrame THEMSELVES, so the tick
 * is already inside a bracket. Ours is called from the loop, so the
 * bracket has to be respected explicitly.
 */
static void menu_draw_page_3d(void)
{
	int i;

	/*
	 * A PAGE THAT HAS NOT ENTERED MUST NOT DRAW. menu_tick runs Enter
	 * lazily on the first tick after a page change, and a page can be
	 * switched from inside a tick - page 15 does exactly that when a
	 * cheat matches, going to itself. In that frame Leave has already
	 * run and Enter has not, so the state here is stale: the old
	 * page's geometry with whatever the Leave handler changed, which
	 * for page 15 is a projection base 1.87x larger.
	 *
	 * One frame, and easy to dismiss as a flicker - which is what
	 * makes it worth a guard rather than a comment.
	 */
	if (s_page < 0 || s_page >= MENU_PAGE_COUNT ||
	    !s_pages[s_page].initialised)
	    return;


	if (s_page == MENU_PAGE_DSILOGO) {
		page25_draw_3d();
		return;
	}
	if (s_page == MENU_PAGE_NAME) {
		page15_draw_3d();
		return;
	}
	if (s_page == MENU_PAGE_LOADING) {
		race_draw_3d();
		return;
	}
	if (s_page == MENU_PAGE_BIKE) {
		page10_draw_3d();
		return;
	}
	if (s_page == MENU_PAGE_LOBBY) {
		page8_draw_3d();
		return;
	}
	if (s_page != MENU_PAGE_TITLE || !s_p14_models_ok)
		return;

	/* The original rebuilds it every frame from the tick. */
	if (!camera_look_at(s_p14_view, kP14CamPos, kP14Target))
		return;

	queue_reset();
	vertexpool_reset();

	/*
	 * THE PHASE GATES THE DRAWS, and this is the handoff from the 3D
	 * models to the 2D title art. Read out of the disassembly at
	 * 0x00446e00..0x00446e99 [verified] - the phase is 0x005409bc and
	 * the first gate is CMP [0x005409bc],1 / JGE at 0x00446e03:
	 *
	 *     if (phase < 1)  ModelInstance_Draw(0x00540500)
	 *     if (phase < 2)  ModelInstance_Draw(0x00540684)
	 *     if (phase < 3)  ModelInstance_Draw(0x00540808)
	 *
	 * The three instance pointers are 0x184 apart, which is
	 * sizeof(MR_ModelInstance), so they are instances 0, 1 and 2 of
	 * one array. The decompiler renders all three calls identically
	 * because it loses the pointer arithmetic - the ECX loads are the
	 * only thing that distinguishes them.
	 *
	 * o SO A MODEL STOPS BEING DRAWN THE MOMENT IT FINISHES. The
	 *   phase is the count of finished animations, and the durations
	 *   are 1.0, 1.2 and 1.4 - so instance 0 finishes first, phase
	 *   goes to 1, and instance 0 is dropped. Meanwhile the tick has
	 *   switched the title sprite to the next layer. The 3D model
	 *   hands the frame to the 2D art and gets out of the way.
	 *
	 * o WITHOUT THIS GATE THE FINISHED MODEL KEEPS DRAWING ON TOP OF
	 *   the sprite that replaced it, which is what it looked like:
	 *   the landed title with a leftover model still on it. There was
	 *   never a missing "stop drawing" step - it was here and we were
	 *   not implementing it.
	 */
	for (i = 0; i < P14_MODELS; i++)
		if (s_p14_mesh_ok[i] && s_p14_reveal < i + 1)
			model_draw(&s_p14_inst[i], s_p14_view, kP14CamPos);

	queue_walk(drawscene_dispatch);

}

/*
 * ORIGIN  NONE - ours
 *
 * THE ONE LIST OF PAGES THAT USE THE SPRITE BUTTON WIDGET.
 *
 * ADD A NEW BUTTON PAGE HERE AND NOWHERE ELSE. Split across
 * parallel lists in menu_on_click and menu_on_key, a page can be
 * registered in s_pages and missing from both - which fails in a
 * way that reads as a page bug rather than a dispatch bug:
 *
 *   menu_hit_test runs from the PAGE's own tick, so hover and
 *   the hover cue work regardless. The button lights up under
 *   the pointer and clicking it does nothing at all.
 *
 * One list cannot disagree with itself.
 */
static int menu_page_uses_buttons(int page)
{
	return page == MENU_PAGE_TITLE ||
	       page == MENU_PAGE_MODE ||
	       page == MENU_PAGE_RACE ||
	       page == MENU_PAGE_BIKE ||
	       page == MENU_PAGE_MAIN ||
	       page == MENU_PAGE_OPTIONS ||
	       page == MENU_PAGE_PROTOCOL ||
	       page == MENU_PAGE_SESSION ||
	       page == MENU_PAGE_LOBBY ||
	       page == MENU_PAGE_RESULTS;
}

/* ORIGIN  NONE - ours */
int menu_on_click(void)
{
	if (menu_page_uses_buttons(s_page))
		return menu_buttons_click();
	if (s_page != MENU_PAGE_NAME)
		return 0;

	/*
	 * A CLICK IS THE SPACE CASE. The original tests DAT_005083b8 == 2
	 * - CMP EAX,2 at 0x0048848a, an equality and not a mask - then
	 * runs the same three-way branch on the selection that space
	 * does: CMP against 0x1a at 0x00488498, which is the 26 below.
	 * All three arms go through FUN_00487a70, and its only callers
	 * are three sites in this one tick [verified].
	 *
	 * The selection, 0x005f647c, already follows the pointer by
	 * proximity, so clicking a letter and typing it differ only in
	 * how the selection got there.
	 */
	if (s_p15_sel < 26) {
		p15_type_letter(s_p15_sel);
	} else if (s_p15_sel == P15_SEL_BACKSPACE) {
		p15_backspace();
	} else if (s_p15_sel == P15_SEL_VALID && s_p15_nameLen > 0) {
		s_p15_done = 1;
	}
	audio_cue(SND_MENU_HOVER, 0);
	return 1;
}

/* ORIGIN  Menu_OnKey @ 0x0040a0f0 */
int menu_on_key(int vkey)
{
	if (s_page == MENU_PAGE_TITLE)
		return page14_on_key(vkey);
	if (s_page == MENU_PAGE_DSILOGO)
		return page25_on_key(vkey);
	if (s_page == MENU_PAGE_NAME)
		return page15_on_key(vkey);
	if (s_page == MENU_PAGE_SESSION)
		return page7_on_key(vkey);
	if (s_page == MENU_PAGE_LOBBY)
		return page8_on_key(vkey);
	/*
	 * THE THREE ABOVE COME FIRST because they have handlers
	 * of their own; everything else that has buttons goes
	 * through the widget. MENU_PAGE_TITLE appears in both -
	 * page14_on_key wants the key, the click list wants the
	 * widget - which is why the key side cannot simply be
	 * the same test as the click side.
	 */
	if (menu_page_uses_buttons(s_page))
		return menu_buttons_key(vkey);
	return 0;
}

/*
 * ORIGIN  NONE - ours
 *
 * THE PAGE'S OWN 3D, THEN THE TRANSITION OVER IT.
 *
 * Menu_UpdateTransition is called after Menu_Draw in the original's
 * page ticks, so the shards are queued after the page's geometry and
 * land on top of it. We keep the ordering and pay for it with a
 * second queue walk: page25_draw_3d already resets and walks the
 * queue itself, so the shatter cannot simply add to that batch
 * without restructuring both. Two walks a frame is the cost of
 * leaving the page code alone, and it is only paid for 1200 ms.
 */
void menu_draw_3d(void)
{
	menu_draw_page_3d();
	glass_update();
}

/* ORIGIN  NONE - ours. Cf. Menu_Ctor @ 0x004098e0 */
int menu_init(int startPage)
{
	page_table_init();

	get_ini_value("DEBUG", "alltracks", &s_p6_allTracks);
	if (s_p6_allTracks)
		mrlog("menu:   all tracks selectable ([DEBUG] alltracks)\n");

	/*
	 * The menu sound bank. Menu_LoadResources calls Snd_LoadMenuBank
	 * (0x00403f90) at 0x0040b0c8 [verified], which fills the 256-entry
	 * handle table - g_nSndHandles at 0x00519ff8, audio-cues.txt 5 -
	 * from "Menu\" and, when voices are on, "Voices\Menu\".
	 * It is what the cue ids below resolve against - see
	 * audio-cues.txt 5.5 for why the bank and not the id decides
	 * which file a cue plays.
	 */
	audio_load_bank(AUDIO_BANK_MENU);

	/*
	 * Menu_LoadResources builds the glass ONCE per run, and that is
	 * also the only thing that rolls the shards' spin rates - so
	 * where this is called decides how often the tumble changes. Here
	 * means once, which is what the original does.
	 *
	 * A failure is not fatal: glass_update does nothing when the
	 * effect did not build, and a menu that changes page without an
	 * effect still works.
	 */
	glass_init();

	menu_set_page(startPage);
	return 1;
}

/* ORIGIN  NONE - ours. Cf. Menu_Dtor @ 0x00409990 */
void menu_free(void)
{
	int i;
	for (i = 0; i < MENU_PAGE_COUNT; i++) {
		if (s_pages[i].initialised && s_pages[i].fnLeave)
			s_pages[i].fnLeave();
		s_pages[i].initialised = 0;
	}
	if (s_p14_font_ok) {
		font_free(&s_p14_font);
		s_p14_font_ok = 0;
	}
	glass_shutdown();
}
