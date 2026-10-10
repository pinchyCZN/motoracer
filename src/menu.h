/*
 * menu.h - the menu driver, the page table, and the page handlers.
 *
 * ONE FILE FOR THE WHOLE MENU, and the name is the original's own:
 * the leaked assert paths put menu.cpp under D:\Dsed\mr320\menu\.
 *
 * The original split further - pageMainMenu.cpp and pageProtocol.cpp
 * are two more paths that leaked, so it had a file per page or per
 * group alongside the driver. We are not doing that yet. page 1 used
 * to live in its own page_loading.c and has been folded in here,
 * because 28 pages at a file each is 28 files and the driver is the
 * thing they all share.
 *
 * WHEN TO SPLIT, so it is a decision and not a drift: when the page
 * handlers dominate this file, move them to menu_pages.c and leave the
 * driver here. That mirrors the original's menu.cpp/pageXxx.cpp split
 * without going to one file per page.
 *
 *
 * THERE IS NO GIANT SWITCH IN THE ORIGINAL, and it is worth saying
 * because that is the shape people expect. Three levels, and none of
 * them is a switch on a mode enum [verified]:
 *
 *   1  THE GAME OBJECT. One global instance with a nine-slot vtable at
 *      0x004d04c8; slot +0x14 (maybe_Game_Tick) drives the others. See
 *      docs/subsystems/game-object.txt.
 *
 *   2  MENU OR RACE IS A BOOLEAN. maybe_Game_Tick calls FUN_004126a0 -
 *      InGameMenu_ServiceAll in the DB - at 0x0047d750 and does
 *      TEST EAX,EAX / JZ: zero runs the menus, non-zero runs physics,
 *      AI, sound, collision and the frame. TWO branches, not N.
 *
 *   3  THE PAGE IS A TABLE. g_menuPageTable at 0x004dbb38 is 28
 *      records of four dwords indexed by page number, and Menu_Tick
 *      indexes it. No switch anywhere.
 *
 * So the frame loop in main.cpp does not grow a mode switch as the
 * game fills in. It grows one call - game_tick() - and everything
 * below that is table dispatch.
 *
 * See docs/subsystems/menu-pages.txt.
 */

#ifndef MRACER_MENU_H
#define MRACER_MENU_H

/* For MR_Sprite, which the button list takes by pointer. */
#include "sprite.h"
#include "font.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 28 records at 0x004dbb38, 16 bytes each. Records 0 and 27 are empty
 * and are not pages [verified].
 */
#define MENU_PAGE_COUNT 28

#define MENU_PAGE_LOADING 1
#define MENU_PAGE_QUIT    4    /* EXIT - tears down and closes */
#define MENU_PAGE_PROTOCOL 2   /* where the main menu's LINK goes  */
#define MENU_PAGE_TITLE   14
#define MENU_PAGE_NAME    15    /* where the title screen goes next */
#define MENU_PAGE_MAIN     3    /* where name entry goes next       */
#define MENU_PAGE_MODE     5    /* where SOLO and LINK both go      */
#define MENU_PAGE_RACE     6    /* race select, all three modes     */
#define MENU_PAGE_RESULTS  9    /* TOP 5 - where a quit race lands  */
#define MENU_PAGE_BIKE    10    /* bike select                      */
#define MENU_PAGE_OPTIONS 11    /* the main menu OPTIONS button     */
#define MENU_PAGE_SESSION  7    /* host or join by code; OURS  */
#define MENU_PAGE_LOBBY    8    /* roster, chat, start; OURS   */
#define MENU_PAGE_DSILOGO 25    /* App_Init's first page            */

/*
 * MENU_START_PAGE_DEFAULT - where the reconstruction enters the menu
 * chain, and the compile-time half of the intro-skip switch.
 *
 * The original's chain is 25 -> 14 -> 15 -> 3 (Delphine logo, title,
 * name entry, main menu). So skipping an intro is not a mode: it is
 * entering the chain further along.
 *
 * THE ATTRACT DEMO IS PAGE 1, NOT PAGE 0. Page 14's 30-second idle
 * calls maybe_Menu_StartAttractDemo (0x00409a30), which picks a demo
 * script, runs the current page's leave handler, sets g_nSoloMode and
 * ends with Menu_SetPage(1) - the loading page, which then starts the
 * race [verified]. Page 0 is the no-page sentinel and nothing enters
 * it deliberately; see page4_tick in menu.c.
 *
 *     25   the DSI logo intro. THE DEFAULT, because it is the
 *          original's own first page - App_Init enters there. Enter,
 *          tick, leave and its own 3D pass are all reconstructed,
 *          and it hands over to 14 when the animation finishes or a
 *          key is pressed.
 *     14   the title screen. [DEBUG] startpage=14 SKIPS THE INTRO,
 *          which is what an iteration on a later page wants.
 *
 * SKIPPING IS THE INI'S JOB, NOT THE DEFAULT'S. The default used to
 * be 14 with a comment saying page 25 was not reconstructed yet -
 * true when written, false once page25_* landed, and it cost
 * somebody an afternoon looking for an intro-skip switch that never
 * existed. The game now starts where the game starts, and the ini is
 * what shortens it.
 *
 * [DEBUG] startpage=N in the ini overrides this without a rebuild,
 * which is the form worth using while working on a later page - see
 * main.cpp. Change the constant only to move the DEFAULT.
 */
#define MENU_START_PAGE_DEFAULT MENU_PAGE_DSILOGO

/*
 * The record. The initialised flag lives in the RECORD, not in the
 * menu object, so it is per page - which is what makes the enter
 * handler run again on every visit [verified].
 */
typedef struct MR_MenuPage {
	int   initialised;
	void (*fnTick)(void);
	void (*fnEnter)(void);
	void (*fnLeave)(void);
} MR_MenuPage;

/*
 * menu_tick - Menu_Tick (0x00409c00), once per frame.
 *
 *     if (!page.initialised) {
 *         reset the menu object list
 *         if (page.fnEnter) page.fnEnter();
 *         page.initialised = 1;
 *     }
 *     if (page.fnTick) page.fnTick();
 */
void menu_tick(void);

/*
 * menu_goto_page - Menu_GotoPage (0x00409eb0).
 *
 *     if (page.initialised) {
 *         if (page.fnLeave) page.fnLeave();
 *         page.initialised = 0;
 *     }
 *     menu_set_page(newIndex);
 *
 * THE LEAVE HANDLER IS GATED BY THE SAME FLAG AS THE ENTER HANDLER, so
 * leave runs if and only if enter ran. That invariant is the whole
 * point of putting the flag in the record, and it is enforced in both
 * places the flag is cleared.
 */
void menu_goto_page(int page);

/* Menu_SetPage (0x00409f70): set the index and clear the target's
 * flag. No leave handler - that is menu_goto_page's job. */
void menu_set_page(int page);

/*
 * menu_restart_page - leave the current page and enter it again.
 *
 * A debug facility with no counterpart in the original, but built out
 * of the original's own machinery rather than a special path: a page
 * is restarted by transitioning to itself. Menu_GotoPage already runs
 * the leave handler, clears the initialised flag and re-applies the
 * menu projection, and the next tick then runs the enter handler - so
 * a restart is a normal page change that happens to have the same
 * destination.
 *
 * That matters for correctness as much as for brevity: anything a page
 * sets up in enter and tears down in leave is handled by the code that
 * already knows about it, including any page added later.
 */
void menu_restart_page(void);

int  menu_current_page(void);

/* Bring the menu up on a starting page. Returns 0 if nothing loaded. */
int  menu_init(int startPage);
void menu_free(void);

/*
 * Draw the current page. Separate from the tick because our frame loop
 * comes back - the original's page 1 never does, so it never had to
 * separate construction from drawing.
 */
void menu_draw(void);

/*
 * menu_draw_3d - the page's 3D layer, and it MUST be called inside the
 * renderer's scene bracket.
 *
 * Separate from menu_draw because the frame loop's 2D phase runs
 * BEFORE r_begin_scene: sprites go into the framebuffer, which is
 * software and does not care, but anything reaching the renderer does.
 * Queueing in the 2D phase and walking the queue there put every
 * primitive outside the bracket, where it went nowhere.
 */
void menu_draw_3d(void);

/*
 * menu_set_cursor - where the pointer is, in 640x480 DESIGN space.
 *
 * The original keeps the cursor in two int16s at 0x00519fe8 and
 * 0x00519fea - two bytes apart, so not ints. ONE WRITER AND ABOUT
 * THIRTY READERS: the only writes are in caseD_200, which is the
 * WM_MOUSEMOVE arm of the window procedure, and the reads are
 * Menu_HitTest plus the tick of very nearly every page, each feeding
 * its own cursor sprite [verified].
 *
 * DESIGN space, not client space: every rectangle in the menu is
 * authored against 640x480, so the window's letterbox has to be
 * undone before the test means anything.
 */
void menu_set_cursor(int x, int y);

/*
 * menu_begin_transition - leave for another page WITH the glass
 * shatter, which means leaving one frame later than you asked.
 *
 * Menu_BeginTransition (0x0040c4a0) captures the screen and then the
 * page changes in the same breath, because the original reads its own
 * SOFTWARE framebuffer and that still holds the outgoing page's last
 * completed frame. We have no such buffer - our capture comes off the
 * render target and is only valid once the frame is drawn - so the
 * page change has to wait until after the capture, or the shards
 * carry a picture of the page being arrived at.
 *
 * o THAT BUG LOOKS LIKE TWO DIFFERENT BUGS, which is why it is worth
 *   the extra entry point. Ask during the message pump and the whole
 *   frame renders the NEW page, so the glass wears the next menu's
 *   background. Ask at the end of a page tick and the 2D layer is
 *   still the old page but the 3D is skipped, so the glass comes out
 *   black - which looks plausible enough to be believed.
 *
 * So: this only RECORDS the destination. menu_apply_pending_page
 * performs it, after the capture.
 */
void menu_begin_transition(int page);

/*
 * Non-zero once page 4 has run - the EXIT button was pressed and
 * the game should close.
 *
 * THE WINDOW MESSAGE IS NOT SENT FROM HERE.  The original's page
 * 4 posts WM_CLOSE itself, but menu.c is C and owns no window;
 * rule 11 keeps the Win32 call in the shell, so the frame loop
 * polls this and posts the message.  Same effect, one level up.
 */
int  menu_quit_requested(void);

/*
 * Perform a page change asked for by menu_begin_transition. Call it
 * once a frame, AFTER the capture and before the next frame's tick.
 * Does nothing when none is pending.
 */
void menu_apply_pending_page(void);

/*
 * THE PLAYER NAME, g_playerName in the original. Page 15 writes it on
 * the way out and pre-fills itself from it on the way in.
 *
 * Eight characters plus a terminator: the name is clamped to eight by
 * the entry page, and the original folds every character to upper case
 * and replaces anything outside A-Z with 'A'.
 */
#define MENU_NAME_MAX 8
extern char g_playerName[MENU_NAME_MAX + 1];

/*
 * The three unlock flags the name-entry cheats set, at 0x004dc050,
 * 0x004dc054 and 0x004dc058 - three consecutive dwords. Nothing reads
 * them yet; page 3's indicator sprites will.
 */
extern int g_cheatReverse;
extern int g_cheatPocket;
extern int g_cheatIsland;

/*
 * THE TWO UNLOCKABLE FEATURES, and each is TWO flags, not one:
 *
 *   g_reverseUnlocked  0x004dc040  the option EXISTS at all
 *   g_reverseEnabled   0x004dc068  it is currently switched on
 *
 * and the same pair for the pocket bike at 0x004dc03c / 0x004dc078.
 * Page 3 shows the indicator when unlocked OR cheated, and clicking
 * it flips the enabled flag.
 *
 * o UNLOCKED IS EARNED AND STICKY. Two sites set it: a race-result
 *   path at 0x004581a5 reads a record field and unlocks on it, and
 *   0x00455224 sets it whenever the ENABLED flag is on - so once the
 *   mode has been used it stays available. That second one is why
 *   the option cannot be lost by toggling it off.
 *
 * o THE CHEAT DOES NOT SET UNLOCKED. It is tested alongside it,
 *   which is why entering the code makes the button appear without
 *   writing anything into the save.
 *
 * All four live in the .ini under [PLAYER], never the registry.
 */
/*
 * g_soloMode (0x004dbffc) and g_selectedMode (0x004dc094).
 *
 * Page 3 writes the first - 1 for SOLO, 0 for LINK - and page 5
 * reads it to choose which of its two shapes to build. Page 5 writes
 * the second, 0..2, and what those number MEANS depends on the first;
 * see the note over page5_tick.
 */
extern int g_soloMode;
extern int g_selectedMode;

/*
 * The race setup page 6 builds: which island, how many laps, and
 * whether it is a time attack. 0x004dc08c, 0x004dc074, 0x004dc148.
 */
extern int g_selectedTrack;
extern int g_raceLaps;
extern int g_timeAttack;

/*
 * And what page 10 chooses: which of the eight bikes, and whether
 * the gearbox is automatic. 0x00584499 and 0x004dc0a0.
 */
extern int g_selectedBike;
extern int g_autoGearbox;

extern int g_reverseUnlocked;
extern int g_reverseEnabled;
extern int g_pocketUnlocked;
extern int g_pocketEnabled;

/* Read them at startup and write them back when one changes. */
void menu_load_unlocks(void);
void menu_save_unlocks(void);

/*
 * THE SPRITE BUTTON LIST. Menu_AddSpriteButton and friends, reduced
 * to what the pages use. lit may be NULL, which asks for the stipple
 * treatment instead of a second sprite - see menu.c.
 */
void menu_clear_buttons(void);
int  menu_add_sprite_button(MR_Sprite *base, MR_Sprite *lit, int key);

/*
 * Menu_AddTextButton (0x0040bd60). The hit rect is the text
 * extent in the given font, so the caller supplies no size.
 *
 * repeatOnHold is MR_MenuButton +0x54: while the mouse is held down
 * over such a button Menu_Tick re-activates it EVERY FRAME, with no
 * initial delay [verified, 0x00409c00]. That is how a volume steps
 * from 0 to 32 without thirty-two clicks - and it is also why we do
 * NOT transcribe the rate. See MENU_REPEAT_DELAY in menu.c.
 */
int  menu_add_text_button(const char *text, MR_Font *font, int x, int y,
                          int key, int repeatOnHold);

/* The held state of the left button, for repeatOnHold. Set from the
 * window procedure, not polled. */
void menu_set_mouse_down(int down);
void menu_hit_test(void);
void menu_draw_buttons(void);
int  menu_hover_index(void);
int  menu_get_activated(void);
void menu_clear_activated(void);

/* Move the highlight without a mouse. Page 10 does this when the
 * gearbox is chosen, so the next press lands on START. */
void menu_set_hover(int index);

/*
 * menu_on_click - a left button press, at the CURRENT cursor.
 *
 * It takes no coordinates on purpose. The original does the same: it
 * tests a flag the mouse handler set and then acts on whatever the
 * proximity search already selected, so a click is exactly the SPACE
 * case with the selection set by the pointer instead of the arrows.
 * Passing coordinates here would invite a second, different hit test.
 */
int  menu_on_click(void);

/*
 * Feed a key to the menu. Returns 1 if the menu consumed it.
 *
 * The original reaches input through the menu object's button list;
 * we have no button object yet, so pages test keys directly and this
 * exists to give them one entry point rather than several.
 */
int  menu_on_key(int vkey);

/* This page TYPES, so the shortcuts must not. NOT the capture
 * above - that is for a FIELD. */
int  menu_page_modal(void);

/* ================================================================== */
/* MODAL TEXT CAPTURE.  OURS.                                         */
/*                                                                    */
/* WHILE THIS IS ACTIVE EVERY KEY BELONGS TO THE FIELD, and main.cpp  */
/* routes to it from the very top of WM_KEYDOWN - ahead of ESCAPE,    */
/* ahead of the debug keys. Without that, typing a room code hits the */
/* side camera on 1, the wall toggle on 3, the chase view on 5 and    */
/* the pause on space, and ESCAPE closes the game outright.           */
/*                                                                    */
/* TEXT COMES FROM WM_CHAR, NOT WM_KEYDOWN. A VK code only equals an  */
/* ASCII character for A-Z and 0-9; punctuation, space and anything   */
/* layout-dependent do not survive the shortcut. The control keys go  */
/* the other way, through WM_KEYDOWN, because WM_CHAR does not see    */
/* them reliably.                                                    */
/* ================================================================== */

#define MENU_CAPTURE_MAX 96

/*
 * allowed - a set of acceptable characters, or NULL for any printable.
 * Lower case is folded to upper when the set contains only upper.
 */
void menu_capture_begin(const char *allowed, int maxLen);
void menu_capture_end(void);
int  menu_capture_active(void);

int  menu_capture_on_key(int vkey);    /* WM_KEYDOWN; 1 = consumed */
int  menu_capture_on_char(int ch);     /* WM_CHAR;    1 = consumed */

const char *menu_capture_text(void);
int         menu_capture_len(void);

/*
 * ENTER sets done, ESCAPE sets cancelled, and BOTH end the capture.
 * A page reads whichever it cares about and then clears it; the flags
 * do not clear themselves, or a page that misses a frame misses the
 * result.
 */
int  menu_capture_done(void);
int  menu_capture_cancelled(void);
void menu_capture_clear_result(void);

/*
 * menu_capture_paste - REPLACES the field with one line of text.
 *
 * Every character goes through menu_capture_on_char, so the
 * printable range, the length cap and any allowed set apply to a
 * paste exactly as they do to typing - one filter, not two.
 *
 * Stops at the first CR or LF rather than skipping them, or two
 * copied lines would arrive as one run-together address.
 */
void menu_capture_paste(const char *text);

/* The blink phase, for drawing a cursor. 1 while it should show. */
int  menu_capture_cursor_on(void);


/* Registers both halves of MR_NetHandlers - see menu_net_install. */
void menu_net_install(void);

/*
 * BMG or EA branding. The original picks between two whole sets of
 * title art on g_useBmgBranding, which App_Init sets from which .dem
 * file it found. Default 0 = MotoRacer, 1 = the Grand Prix art.
 */
void menu_set_bmg_branding(int on);

#ifdef __cplusplus
}
#endif

#endif /* MRACER_MENU_H */
