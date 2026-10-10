/*
 * hud.h - what the game draws OVER a running race.
 *
 * The escape menu first; the lap counter, the tachometer, the map and
 * the messages are the same layer and belong here as they arrive.
 *
 * IT IS NOT A MENU PAGE, and that is the reason for a separate file.
 * The original has two menu frameworks: menu\menu.cpp drives the
 * out-of-race PAGES, and a second, smaller one drives four IN-RACE
 * menus that float over the track with their own widgets, their own
 * mouse cursors and their own per-frame service. This is the second.
 * See docs/subsystems/in-race-menu.txt.
 */

#ifndef MRACER_HUD_H
#define MRACER_HUD_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * THE ESCAPE MENU. In the original this is InGameMenu_OpenEscape
 * (0x00436420), raised from Input_OnKeyDown case 0x1b and gated on
 * the pause flag - ESCAPE does nothing at all while paused.
 *
 * ESCAPE RAISES IT, and Q as well. The pause gate is the original's,
 * and in a NET race it is an overlay rather than a freeze - race.c
 * stops the simulation only in solo, netplay-relay.txt 3.4.
 */
void hud_menu_open(void);
void hud_menu_close(void);
void hud_menu_toggle(void);

/*
 * hud_prompt_open - the post-race question, [REPLAY] YES/NO.
 *
 * ORIGIN  FUN_00437b70 @ 0x00437b70, which FUN_00412a50 reaches
 *         when its timer runs out. race.c raises it.
 */
void hud_prompt_open(void);

/*
 * hud_continue_open - the CONTINUE menu, [CONTINUE] over YES
 * and NO, raised when the clock has run out and the GAME OVER
 * sequence has finished.
 *
 * ORIGIN  FUN_004358b0 @ 0x004358b0, which the game reaches
 *         through g_menuPendingRequest bit 0x40.
 *
 * IT PLAYS TRY AGAIN ITSELF, after a Snd_StopAll - so the
 * caller must not queue the line separately.
 */
void hud_continue_open(void);

/*
 * Non-zero while the menu is up.
 *
 * THE RACE MUST NOT STEP WHILE THIS IS SET. The original does not
 * use a pause flag for it: the menu object carries +0x10, and
 * InGameMenu_ServiceAll returns 0 to the frame loop as soon as it
 * finds a menu with that set, which the caller reads as "do not tick
 * the game". Ours is the same rule with one less indirection.
 */
int  hud_menu_active(void);

/*
 * hud_menu_on_key - returns 1 when the key was consumed.
 *
 * Up/down move the selection, RETURN activates, Q closes. The
 * original also derives a one-letter accelerator per row from the
 * label text (InGameMenu_BuildAccelerators, 0x00434960), so the
 * hotkeys change with the language; not reproduced yet.
 */
int  hud_menu_on_key(int vkey);

/*
 * hud_draw - build the whole in-race overlay. Call from the race's
 * 2D pass, after race_draw_2d.
 *
 * ONE FUNCTION FOR ALL OF IT because the overlay is one buffer and
 * fb_overlay_begin clears it: the escape menu and the counter have
 * to be drawn inside the same two passes or the second would erase
 * the first.
 */
/*
 * hud_banner_timeout - the blinking TIME OUT banner.
 *
 * Raised by the race clock in its 500 ms grace window, and
 * idempotent: calling it while the banner is up does nothing,
 * which is what keeps the blink from restarting every frame.
 */
void hud_banner_timeout(void);

/*
 * hud_banner_gameover - the GAME OVER banner, which is SPRITE
 * ART rather than a glyph. Idempotent the same way.
 */
void hud_banner_gameover(void);

/*
 * The rectangle the two end-of-race banners occupy, so the
 * caller can composite them back over the 3D. Same contract as
 * hud_counter_overlay_rect.
 *
 * WITHOUT THIS NEITHER BANNER IS VISIBLE. They draw into the
 * overlay like every other message widget, and the overlay is
 * put back one rectangle at a time.
 */
int  hud_banner_overlay_rect(int *x, int *y, int *w, int *h);

void hud_draw(void);

/* ------------------------------------------------------------------ */
/* THE COUNTER - the speedometer and rev counter in the lower right.   */
/* See docs/subsystems/hud-counter.txt.                                */
/* ------------------------------------------------------------------ */

/*
 * The two needles and their drop shadows, as QUEUE PRIMITIVES.
 *
 * CALL IT FROM THE 3D PASS, before the queue walk - that is where
 * the original puts it, and it is why the needles sit over the track
 * instead of under it. Nothing is drawn unless CounterType is 1.
 */
/*
 * The dial FACE - the plate the needles turn over. Queue it from the
 * 3D pass BEFORE the needles; it goes in at depth 15 against their 4
 * and 8, so the queue puts it behind either way.
 *
 * It is the track's own decor template named "counter" - one
 * double-sided textured quad. Nothing draws when the track has no
 * such template.
 */
void hud_counter_draw_face(void);

void hud_counter_draw_needles(void);

/*
 * The rectangle the digital readouts occupy, so the caller can put
 * them back on top of the geometry. Returns 0 when the counter is
 * off. Same contract as hud_menu_overlay_rect.
 */
int  hud_counter_overlay_rect(int *x, int *y, int *w, int *h);

/*
 * THE UPPER-RIGHT RACE HUD - the TIME banner and its countdown.
 *
 * Drawn into the overlay by hud_draw; this is the rectangle it
 * occupies, on the same contract as hud_counter_overlay_rect. See
 * docs/subsystems/hud-race.txt.
 */
int  hud_time_overlay_rect(int *x, int *y, int *w, int *h);

/* The BEST LAP block at the very top right. Same contract. */
int  hud_bestlap_overlay_rect(int *x, int *y, int *w, int *h);

/*
 * The finishing-position readout that replaces the in-race HUD once
 * the flag is out - "1st", "2nd" and so on. See hud.c,
 * hud_draw_finish_place. Returns 0 while the race is still running.
 */
int  hud_place_overlay_rect(int *x, int *y, int *w, int *h);

/* The POS block under the countdown. Same contract. */
int  hud_pos_overlay_rect(int *x, int *y, int *w, int *h);

/*
 * The grid countdown, drawn INSTEAD of the race HUD while the
 * clock is still in its pre-race phase - see hud-race.txt 1.
 */
int  hud_countdown_overlay_rect(int *x, int *y, int *w, int *h);

/*
 * THE RACER CALLOUTS MOVE, so unlike every other element this
 * rectangle changes every frame - it is the union of the captions
 * the last hud_draw drew. Without it they composite only where they
 * overlap another element's rectangle, which reads as a timer.
 */
int  hud_callout_overlay_rect(int *x, int *y, int *w, int *h);
void hud_countdown_reset(void);

/* The lap list, upper left: the LAP banner, "n of N", and the
 * lap times stacked below it. */
int  hud_laplist_overlay_rect(int *x, int *y, int *w, int *h);

/* The in-race map. See docs/subsystems/hud-map.txt. */
void hud_draw_map(void);
void hud_map_reset(void);

/*
 * hud_menu_overlay_rect - the rectangle the menu occupies, so the
 * caller can put it back on top of the 3D.
 *
 * OUR 2D LAYER IS UNDER THE GEOMETRY and the original's is not: it
 * draws both into one surface and lets the call order decide, and
 * the in-race menus draw after the scene. Without this the words sit
 * behind whatever is near the camera.
 *
 * Returns 0 when the menu is not up. See r_draw_overlay_rect.
 */
int  hud_menu_overlay_rect(int *x, int *y, int *w, int *h);

/* Release the font. Call once at shutdown. */
void hud_free(void);

#ifdef __cplusplus
}
#endif

#endif /* MRACER_HUD_H */
