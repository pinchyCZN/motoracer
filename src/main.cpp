/*
 * mracer - MotoRacer (PC, 1997) reconstruction
 * main.cpp - the Win32 shell: console, window, back buffer, message
 *            pump, and the window-placement save/restore.
 *
 * Deliberately plain Win32. The original links MFC, but only at its
 * outermost shell - it registers its own window class and runs its own
 * window procedure, and nothing below that touches MFC. See
 * docs/subsystems/app-init.txt section 2.1.1.1 and
 * docs/subsystems/window-proc.txt.
 *
 * No precompiled header, by project convention. Every translation unit
 * includes what it needs.
 */

#include <windows.h>
#include <mmsystem.h>
#include <windowsx.h>   /* GET_X_LPARAM / GET_Y_LPARAM */
#include <stdio.h>

#include "config.h"
#include "wave.h"
#include "options.h"
#include "framebuffer.h"
#include "lez.h"
#include "sprite.h"
#include "menu.h"
#include "hud.h"
#include "glass.h"
#include "queue.h"
#include "cull.h"
#include "transform.h"
#include "mesh.h"
#include "transform.h"
#include "bank.h"
#include "render.h"
#include "assets.h"
#include "trig.h"
#include "timing.h"
#include "autopilot.h"
#include "race.h"
#include "audio.h"
#include "fontdef.h"
#include "font.h"
#include "log.h"
#include "agent.h"

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

static const char kWindowClass[]  = "MracerWindow";
static const char kWindowTitle[]  = "MotoRacer";
static const char kConsoleTitle[] = "MotoRacer - debug";

/*
 * The original supports 320x240, 512x384 and 640x480. 640x480 is the
 * design resolution: Sprite_StartMove centres a sprite with
 * (640 - width) / 2, so the menu art is authored against it. See
 * docs/structs/MR_Sprite.txt section 2.
 */
/*
 * Hold-TAB fast forward. 4x by default, overridable with
 * [DEBUG] fastforward=N so a long intro can be skimmed without a
 * rebuild. Held rather than toggled: releasing the key cannot leave
 * the clock in a strange state, and there is no mode to forget about.
 */
static double s_fastForward = 4.0;

/*
 * THE BACK BUFFER, AND THE WINDOW'S FIRST CLIENT SIZE.
 *
 * Not constants any more: options_frame_size decides, so 16:9 is
 * 854x480 and 4:3 is 640x480. They are read once, between
 * options_load and CreateMainWindow, and nothing changes them
 * afterwards - the original defers every display setting to a
 * restart and so do we. See options.h.
 *
 * WHAT DOES NOT MOVE WITH THEM is the 640x480 the 2D art is authored
 * against; that is FB_DESIGN_W/H, and g_fb2dOriginX is what puts the
 * block of art in the middle of a wider frame.
 */
static int kClientWidth  = FB_DESIGN_W;
static int kClientHeight = FB_DESIGN_H;

/* Console scrollback, in lines. printf debugging wants more than the
 * 300-line default. */
static const SHORT kConsoleScrollback = 9999;

/* ------------------------------------------------------------------ */
/* Debug console                                                       */
/* ------------------------------------------------------------------ */

/*
 * console_open - attach a console and point stdout/stderr at it.
 *
 * A /SUBSYSTEM:WINDOWS process has no console, so printf goes nowhere
 * until one is allocated and the CRT streams are reopened onto it.
 */
static void console_open(void)
{
	HANDLE out;
	HWND   hwnd;
	HMENU  menu;
	CONSOLE_SCREEN_BUFFER_INFO csbi;
	FILE  *f;

	if (!AllocConsole())
		return;

	f = NULL;
	freopen_s(&f, "CONOUT$", "w", stdout);
	f = NULL;
	freopen_s(&f, "CONOUT$", "w", stderr);
	f = NULL;
	freopen_s(&f, "CONIN$", "r", stdin);

	/* Unbuffered, so a printf before a crash still reaches the screen. */
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);

	SetConsoleTitleA(kConsoleTitle);

	out = GetStdHandle(STD_OUTPUT_HANDLE);
	if (out != INVALID_HANDLE_VALUE &&
	    GetConsoleScreenBufferInfo(out, &csbi)) {
		COORD size = csbi.dwSize;
		if (size.Y < kConsoleScrollback) {
			size.Y = kConsoleScrollback;
			SetConsoleScreenBufferSize(out, size);
		}
	}

	/*
	 * Closing the console window kills the whole process without ever
	 * reaching WM_CLOSE, which would lose the saved layout. Remove the
	 * close item so the game window is the only way out.
	 */
	hwnd = GetConsoleWindow();
	if (hwnd) {
		menu = GetSystemMenu(hwnd, FALSE);
		if (menu)
			DeleteMenu(menu, SC_CLOSE, MF_BYCOMMAND);
	}
}

/* ------------------------------------------------------------------ */
/* Window placement                                                    */
/* ------------------------------------------------------------------ */

/*
 * normal_rect_screen - the restore-me-to-here rect, in screen coords.
 *
 * WINDOWPLACEMENT.rcNormalPosition is the restore rect for every state:
 * normal, arranged (Win+Arrow snap), maximized and minimized. Using it
 * means a snapped or maximized window saves its pre-snap size rather
 * than the half-screen it happens to occupy.
 *
 * rcNormalPosition is in workspace coordinates, i.e. relative to the
 * work area of the PRIMARY monitor - not the window's own monitor. The
 * primary work origin is rcWork.left/top, usually (0,0) but nonzero
 * when the taskbar is docked top or left, so offset by that to reach
 * screen coordinates. Offsetting by the NEAREST monitor instead adds a
 * second monitor's desktop origin a second time, which is a bug the
 * cdino version of this function carries a note about.
 */
static int normal_rect_screen(HWND hwnd, RECT *out)
{
	WINDOWPLACEMENT wp;
	MONITORINFO mi;
	POINT origin;

	if (!hwnd || !out)
		return 0;

	ZeroMemory(&wp, sizeof(wp));
	wp.length = sizeof(wp);
	if (!GetWindowPlacement(hwnd, &wp))
		return 0;

	*out = wp.rcNormalPosition;

	origin.x = 0;
	origin.y = 0;
	ZeroMemory(&mi, sizeof(mi));
	mi.cbSize = sizeof(mi);
	if (GetMonitorInfoA(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY),
	                    &mi)) {
		OffsetRect(out, mi.rcWork.left, mi.rcWork.top);
	}
	return 1;
}

/* ------------------------------------------------------------------ */
/* BORDERLESS FULLSCREEN - ours, the original has no such thing.        */
/*                                                                      */
/* IT IS A WINDOW, NOT A DISPLAY MODE. Nothing calls
 * ChangeDisplaySettings and nothing goes exclusive, so the desktop
 * resolution never changes - which is the whole point: an exclusive
 * mode switch reshuffles every other window on the desktop and they
 * do not all come back where they were. This drops the frame, sets
 * WS_POPUP and sizes the window to the monitor it is already on.
 *
 * NOT TOPMOST either, deliberately. HWND_TOP leaves it an ordinary
 * window, so alt-tab, the console and anything else behave normally
 * - a topmost fullscreen window is the other half of what makes
 * fullscreen games unpleasant to develop against.
 */
static int  s_fsActive;
static LONG s_fsStyle;
static LONG s_fsExStyle;
static RECT s_fsRestore;   /* screen coords, pre-fullscreen */

static void fullscreen_set(HWND hwnd, int on)
{
	MONITORINFO mi;

	if (!hwnd || (on != 0) == (s_fsActive != 0))
		return;

	if (on) {
		/* Remember where to come back to BEFORE the window is
		 * resized - see save_window_positions. */
		if (!normal_rect_screen(hwnd, &s_fsRestore))
			GetWindowRect(hwnd, &s_fsRestore);

		s_fsStyle   = GetWindowLongA(hwnd, GWL_STYLE);
		s_fsExStyle = GetWindowLongA(hwnd, GWL_EXSTYLE);

		mi.cbSize = sizeof mi;
		if (!GetMonitorInfoA(MonitorFromWindow(hwnd,
		                     MONITOR_DEFAULTTONEAREST), &mi))
		    return;

		SetWindowLongA(hwnd, GWL_STYLE,
		               (s_fsStyle & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
		SetWindowLongA(hwnd, GWL_EXSTYLE,
		               s_fsExStyle & ~(WS_EX_DLGMODALFRAME
		                               | WS_EX_WINDOWEDGE
		                               | WS_EX_CLIENTEDGE
		                               | WS_EX_STATICEDGE));
		SetWindowPos(hwnd, HWND_TOP,
		             mi.rcMonitor.left, mi.rcMonitor.top,
		             mi.rcMonitor.right - mi.rcMonitor.left,
		             mi.rcMonitor.bottom - mi.rcMonitor.top,
		             SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
		s_fsActive = 1;
	} else {
		SetWindowLongA(hwnd, GWL_STYLE,   s_fsStyle);
		SetWindowLongA(hwnd, GWL_EXSTYLE, s_fsExStyle);
		SetWindowPos(hwnd, NULL,
		             s_fsRestore.left, s_fsRestore.top,
		             s_fsRestore.right - s_fsRestore.left,
		             s_fsRestore.bottom - s_fsRestore.top,
		             SWP_NOZORDER | SWP_NOOWNERZORDER
		             | SWP_FRAMECHANGED);
		s_fsActive = 0;
	}
	mrlog("main:   fullscreen %s (F)\n", on ? "on" : "off");
}

/* Save both windows' restore rects to the ini. */
static void save_window_positions(HWND hwnd)
{
	HWND console;
	RECT rc;

	console = GetConsoleWindow();
	if (console && normal_rect_screen(console, &rc)) {
		save_console_pos_ini(rc.left, rc.top,
		                     rc.right - rc.left,
		                     rc.bottom - rc.top);
	}

	/*
	 * A FULLSCREEN WINDOW SAVES WHERE IT CAME FROM, not where it
	 * is. rcNormalPosition would hold the monitor rect here -
	 * borderless fullscreen IS the normal state as far as Windows
	 * is concerned, unlike maximised - so quitting fullscreen
	 * once would pin the window to the whole screen for ever.
	 */
	if (hwnd && s_fsActive) {
		save_win_pos_ini(s_fsRestore.left, s_fsRestore.top,
		                 s_fsRestore.right - s_fsRestore.left,
		                 s_fsRestore.bottom - s_fsRestore.top);
	} else if (hwnd && normal_rect_screen(hwnd, &rc)) {
		save_win_pos_ini(rc.left, rc.top,
		                 rc.right - rc.left,
		                 rc.bottom - rc.top);
	}
}

/* Move the console window to a screen rect. w/h <= 0 keep the current
 * size. */
static void move_console_window(int x, int y, int w, int h)
{
	HWND hwnd;
	RECT rc;
	int  width;
	int  height;

	hwnd = GetConsoleWindow();
	if (!hwnd)
		return;
	if (!GetWindowRect(hwnd, &rc))
		return;

	width  = (w > 0) ? w : (rc.right - rc.left);
	height = (h > 0) ? h : (rc.bottom - rc.top);
	MoveWindow(hwnd, x, y, width, height, TRUE);
}

/* Put both windows back where they were last run. */
static void restore_windows(HWND hwnd)
{
	int x;
	int y;
	int w;
	int h;

	/* Console: default to the left of the game window, tall. */
	x = 40;
	y = 40;
	w = 700;
	h = 800;
	if (get_console_pos_ini(&x, &y, &w, &h)) {
		clamp_win_rect_to_monitor(&x, &y, &w, &h);
		move_console_window(x, y, w, h);
	} else {
		clamp_win_rect_to_monitor(&x, &y, &w, &h);
		move_console_window(x, y, w, h);
	}

	/* Game window: default is whatever CreateMainWindow chose. */
	{
		RECT rc;
		if (!GetWindowRect(hwnd, &rc))
			return;
		x = rc.left;
		y = rc.top;
		w = rc.right - rc.left;
		h = rc.bottom - rc.top;
	}
	if (get_win_pos_ini(&x, &y, &w, &h)) {
		clamp_win_rect_to_monitor(&x, &y, &w, &h);
		MoveWindow(hwnd, x, y, w, h, TRUE);
	}
}

/* ------------------------------------------------------------------ */
/* Frame loop state                                                    */
/* ------------------------------------------------------------------ */

static MR_FramePacer s_pacer;
static MR_Font       s_debugFont;
static int           s_have_font;
static double        s_simTime = 0.0;
static int           s_have_page = 0;

/*
 * Primitives the queue dispatched last frame. Only the debug primitive
 * limit uses it: pressing '[' with no limit set has to start
 * SOMEWHERE, and starting at "everything that was drawn" means the
 * first press removes the last primitive rather than jumping to an
 * arbitrary number and blanking the screen.
 */
static int s_lastDispatched = 0;

static int s_escapeQuits = 0;

/*
 * THE CLIPBOARD READ LIVES HERE, not in menu.c: this is the Win32
 * shell and OpenClipboard wants a window. menu.c gets the text.
 *
 * CF_TEXT, SO ANSI. A relay address is ASCII and the capture drops
 * anything outside 0x20..0x7e anyway, so CF_UNICODETEXT would buy
 * a conversion and nothing else.
 */
static void paste_into_capture(HWND hwnd)
{
	HANDLE      h;
	const char *s;

	if (!OpenClipboard(hwnd))
		return;
	h = GetClipboardData(CF_TEXT);
	if (h) {
		s = (const char *)GlobalLock(h);
		if (s) {
			menu_capture_paste(s);
			GlobalUnlock(h);
		}
	}
	CloseClipboard();
}

/*
 * STOPPING THE CLOCK IN A NET RACE DOES NOT STOP THE RACE - the
 * peers keep simulating and sending, and the pump keeps running,
 * so a local freeze only desynchronises this machine. The
 * original has no pause message to synchronise one with.
 * Menus and solo races are unaffected.
 */
static int net_race_live(void)
{
	return !g_soloMode && race_ready();
}

/* ------------------------------------------------------------------ */
/* Window procedure                                                   */
/* ------------------------------------------------------------------ */
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg,
                                WPARAM wparam, LPARAM lparam)
{
	switch (msg) {
	case WM_CLOSE:
		/* Last point at which both windows still exist. */
		save_window_positions(hwnd);
		/*
		 * AND THE SETTINGS, which until now were only written by
		 * the options PAGE - so anything changed by a key and
		 * never confirmed there was lost on exit. The map mode is
		 * the one that shows: F9 cycles it, MapMode has been in
		 * the table all along, and nothing ever saved it.
		 *
		 * options_save writes the whole table, so it is also what
		 * persists a 16:9 toggled with F4.
		 */
		options_save();
		DestroyWindow(hwnd);
		return 0;

	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;

	case WM_ERASEBKGND:
		/* We will own every pixel once the back buffer exists. */
		return 1;

	case WM_SIZE:
		/*
		 * The GDI blit letterboxes into whatever the client area is,
		 * so on that path a resize only needs a repaint. The D3D9
		 * device owns a swap chain sized to the client area, so it
		 * needs a Reset - r_resize does nothing if the size is
		 * unchanged, and nothing at all while minimised.
		 */
		r_resize((int)LOWORD(lparam), (int)HIWORD(lparam));
		InvalidateRect(hwnd, NULL, FALSE);
		return 0;

	case WM_PAINT: {
		PAINTSTRUCT ps;
		RECT rc;
		HDC hdc = BeginPaint(hwnd, &ps);
		/* Only the GDI path paints from here. When the D3D9 device is
		 * up it owns every pixel of the client area and a GDI blit
		 * underneath it would only flicker; the frame loop presents. */
		if (!r_ready() && GetClientRect(hwnd, &rc)) {
			fb_blit(hdc, rc.right - rc.left, rc.bottom - rc.top);
		}
		EndPaint(hwnd, &ps);
		return 0;
	}

	case WM_LBUTTONDOWN:
		/* The cursor position is already current from WM_MOUSEMOVE, so
		 * the click needs nothing but the fact that it happened.
		 *
		 * The HELD state is separate, and the menu needs it: a button
		 * with repeatOnHold set re-activates on every frame the mouse
		 * stays down over it, which is how a volume slider moves. */
		menu_set_mouse_down(1);
		menu_on_click();
		return 0;

	case WM_LBUTTONUP:
		menu_set_mouse_down(0);
		return 0;


	case WM_MOUSEMOVE: {
		/*
		 * The pointer, mapped back into FRAME space.
		 *
		 * Menu_HitTest compares the cursor against button rectangles
		 * that are authored against the design resolution, so a raw
		 * client coordinate is only correct when the window happens to
		 * be exactly the frame size. Both of our present paths
		 * letterbox with the same rule - uniform scale, centred, no
		 * crop - so undoing it here is one calculation rather than one
		 * per back end:
		 *
		 *     d3d9rt.cpp   compute_letterbox / r_vertex
		 *     framebuffer.c fb_blit
		 *
		 * A pointer in the letterbox bars maps outside the frame and
		 * simply fails every hit test, which is what should happen.
		 * THE AUTHORED-SPACE STEP IS menu_set_cursor's, not ours: at
		 * 16:9 the art block sits g_fb2dOriginX in from the left and
		 * that function takes the offset back off.
		 */
		RECT  rc;
		float scale;
		float sx;
		float sy;

		if (GetClientRect(hwnd, &rc) &&
			rc.right > rc.left && rc.bottom > rc.top) {
			sx = (float)(rc.right - rc.left) / (float)kClientWidth;
			sy = (float)(rc.bottom - rc.top) / (float)kClientHeight;
			scale = (sx < sy) ? sx : sy;
			if (scale > 0.0f) {
				float offX = ((float)(rc.right - rc.left) -
					          (float)kClientWidth * scale) * 0.5f;
				float offY = ((float)(rc.bottom - rc.top) -
					          (float)kClientHeight * scale) * 0.5f;
				menu_set_cursor(
					(int)(((float)GET_X_LPARAM(lparam) - offX) / scale),
					(int)(((float)GET_Y_LPARAM(lparam) - offY) / scale));
			}
		}
		return 0;
	}

	case WM_KEYDOWN:
		/*
		 * A TEXT FIELD OWNS THE KEYBOARD WHILE IT IS OPEN, and this has
		 * to be the FIRST thing in here. Everything below - ESCAPE, the
		 * pause, the camera digits - would otherwise fire while
		 * somebody types a room code, and ESCAPE must leave the field
		 * rather than reach the page under it. See menu.h.
		 */
		if (menu_capture_active()) {
			if (wparam == 'V' && (GetKeyState(VK_CONTROL) & 0x8000))
				paste_into_capture(hwnd);
			else
				menu_capture_on_key((int)wparam);
			return 0;
		}

		if (wparam == VK_ESCAPE && s_escapeQuits) {
			PostMessage(hwnd, WM_CLOSE, 0, 0);
			return 0;
		}

		/*
		 * EXCEPT ON A PAGE THAT TYPES - menu.h on menu_page_modal.
		 * FUNCTION KEYS STILL PASS: none of them can be a name, and
		 * F5 stepping a frame is worth keeping everywhere.
		 */
		if (menu_page_modal() &&
		    !(wparam >= VK_F1 && wparam <= VK_F24)) {
			menu_on_key((int)wparam);
			return 0;
		}

		/*
		 * THE DEBUG KEYS, ahead of the menu so a page cannot swallow
		 * them. Mostly harness rather than game, and none of it
		 * changes what the game does when it is not being driven.
		 *
		 *   SPACE   pause / resume
		 *   F5      step one frame, pausing first if running
		 *   M       mute / unmute - OURS
		 *   1       side view - OURS, retail has no such camera
		 *   2       restart the current page - MOTO.EXE's reset key
		 *   3       guard rails on / off - ride through the walls
		 *   5       CHANGE VIEW - retail's F5, four chase distances
		 *   6       1ST PERSON VIEW - retail's F6, camera mode 6
		 *   7       REAR VIEW - retail's TAB, HELD not toggled
		 *   [ ]     lower / raise the primitive limit
		 *   \       clear the primitive limit
		 *
		 * The pause is a clock, not a flag - see timing.h. Drawing and
		 * presenting carry on every frame, so the image stays up and
		 * the window keeps repainting with no special case anywhere.
		 */
		if (wparam == VK_SPACE) {
			/*
			 * AUTO-REPEAT IGNORED, bit 30 of lparam. Holding a toggle
			 * down would otherwise flap it thirty times a second.
			 */
			if (!(lparam & (1 << 30)) && !net_race_live())
				tm_pause_toggle();
			return 0;
		}
		if (wparam == 'M') {
			/* Auto-repeat ignored, as for the other toggles. */
			if (!(lparam & (1 << 30)))
				audio_set_muted(!audio_muted());
			return 0;
		}
		if (wparam == '1') {
			/* Auto-repeat ignored, as for the pause toggle. */
			if (!(lparam & (1 << 30)))
				race_camera_toggle_side();
			return 0;
		}
		if (wparam == 'Q' || (wparam == VK_ESCAPE && race_ready())) {
			/*
			 * THE IN-RACE ESCAPE MENU - ESCAPE is the original's
			 * key, Q is kept. race_ready() is what stops this
			 * starving the page bindings; the menu swallows every
			 * key while up, so this must precede it to close it.
			 * Auto-repeat ignored.
			 */
			if (!(lparam & (1 << 30)))
				hud_menu_toggle();
			return 0;
		}
		if (hud_menu_active()) {
			hud_menu_on_key((int)wparam);
			return 0;
		}
		if (wparam == '3') {
			/* Auto-repeat ignored, as for the pause toggle. */
			if (!(lparam & (1 << 30)))
				race_walls_toggle();
			return 0;
		}
		if (wparam == '5') {
			if (!(lparam & (1 << 30)))
				race_camera_cycle_view();
			return 0;
		}
		if (wparam == '6') {
			if (!(lparam & (1 << 30)))
				race_camera_toggle_first_person();
			return 0;
		}
		if (wparam == '7') {
			race_camera_look_back(1);
			return 0;
		}
		if (wparam == 'F') {
			/*
			 * BORDERLESS FULLSCREEN, ours. Auto-repeat ignored or
			 * holding it would thrash the style.
			 *
			 * NOTHING IS PERSISTED. There is no setting behind this
			 * and the window position saved on exit is the one the
			 * window had BEFORE going fullscreen.
			 */
			if (!(lparam & (1 << 30)))
				fullscreen_set(hwnd, !s_fsActive);
			return 0;
		}
		if (wparam == VK_F4) {
			/*
			 * 16:9, the key retail uses. It raises the dirty flag
			 * and flips the widescreen setting inline at
			 * 0x00431fd2 - the same two steps
			 * Menu_ToggleWidescreen (0x0043a370) takes, and the
			 * same two the options page takes.
			 *
			 * Mode_169 IS a stored setting, so this is saved with
			 * the rest on exit - the key and the page are two ways
			 * to the same switch, which is what the original does
			 * too.
			 */
			if (!(lparam & (1 << 30))) {
				g_options.wide169 = !g_options.wide169;
				g_videoSettingsDirty = 1;
				mrlog("main:   16:9 %s (F4)\n",
				      g_options.wide169 ? "on" : "off");
			}
			return 0;
		}
		if (wparam == VK_F8) {
			/*
			 * THE AUTOPILOT, toggled - see autopilot.h. Ours
			 * entirely; the original has no such thing, so there
			 * is no key to match and F8 is simply free.
			 *
			 * Auto-repeat ignored, as for the map below, or
			 * holding it would flicker the flag rather than turn
			 * it on.
			 */
			if (!(lparam & (1 << 30))) {
				g_autopilot = !g_autopilot;
				mrlog("auto:   autopilot %s (F8)\n",
				      g_autopilot ? "on" : "off");
			}
			return 0;
		}
		if (wparam == VK_F9) {
			/*
			 * THE MAP, cycled. Five values and 4 is off, exactly as
			 * Input_OnKeyDown does it at 0x004321d7:
			 *
			 *     g_cfgMapMode = (g_cfgMapMode + 1) % 5
			 *
			 * The KEY is ours for now - the original reads it from a
			 * remappable table the F1 help page lists, and F9 is what
			 * that page shows. Auto-repeat ignored so holding it does
			 * not spin through the modes.
			 */
			if (!(lparam & (1 << 30))) {
				g_options.mapMode = (g_options.mapMode + 1) % 5;
				mrlog("map:    mode %d\n", g_options.mapMode);
			}
			return 0;
		}
		if (wparam == VK_F6) {
			/*
			 * BILINEAR FILTERING, toggled.  The original has a
			 * key for this too - Input_OnKeyDown at 0x00431a65
			 * flips DAT_0053be0c - but which key reaches it is
			 * not identified, so F6 is OURS.  The original's is
			 * also guarded by three globals and silently
			 * refuses in some states; ours always works.
			 *
			 * SESSION ONLY, ON PURPOSE.  options_save runs when
			 * page 11 is left, so the options row is the
			 * durable setting and this is for comparing the two
			 * looks in the moment.  Writing the ini from a key
			 * press mid-race would be a side effect nobody
			 * asked for.
			 *
			 * Auto-repeat ignored, as for every other toggle.
			 */
			if (!(lparam & (1 << 30))) {
				g_options.textureFilter =
				    !g_options.textureFilter;
				mrlog("filter: texture filter %s (session)\n",
				      g_options.textureFilter ? "on" : "off");
			}
			return 0;
		}
		if (wparam == VK_F5) {
			/*
			 * AUTO-REPEAT DELIBERATELY NOT IGNORED. Each repeat queues
			 * another single frame, so holding the key plays the game
			 * forward at the keyboard repeat rate - slow motion, for
			 * free, with no separate mechanism.
			 *
			 * tm_step_frames pauses first, so this behaves the same
			 * whether or not the game was already paused. One key does
			 * both jobs and there is no state to remember.
			 */
			if (!net_race_live())
				tm_step_frames(1);
			return 0;
		}
		if (wparam == VK_OEM_4) {            /* [ */
			if (g_queuePrimLimit < 0)
				g_queuePrimLimit = s_lastDispatched;
			if (g_queuePrimLimit > 0)
				g_queuePrimLimit--;
			return 0;
		}
		if (wparam == VK_OEM_6) {            /* ] */
			if (g_queuePrimLimit < 0)
				g_queuePrimLimit = 0;
			g_queuePrimLimit++;
			return 0;
		}
		if (wparam == VK_OEM_5) {            /* backslash */
			g_queuePrimLimit = -1;
			return 0;
		}
		if (wparam == VK_TAB) {
			/*
			 * FAST FORWARD WHILE HELD. Auto-repeat is welcome here -
			 * setting the same speed again is a no-op, and it means a
			 * missed key-up is corrected by the next repeat rather
			 * than leaving the clock fast.
			 *
			 * It scales the VIRTUAL clock only, so the window still
			 * presents at the real rate and the fps readout stays
			 * honest. Nothing happens while paused; SPACE and A own
			 * that direction.
			 */
			tm_set_speed(s_fastForward);
			return 0;
		}
		if (wparam == '2') {
			/*
			 * RESTART THE CURRENT PAGE. Re-runs its leave and enter
			 * without relaunching, which is what makes an intro or an
			 * animation worth stepping through more than once.
			 *
			 * '2' RATHER THAN F1, to match MOTO.EXE, where 2 is the
			 * reset. One key resets either game, which is what makes a
			 * two-sided comparison quick to set up by hand.
			 *
			 * IT IS SAFE TO TAKE A DIGIT HERE, and only a digit: this
			 * block runs ahead of menu_on_key, so a key a page wants is
			 * swallowed. No page reads digits. LETTERS ARE NOT SAFE -
			 * page 15 types A-Z - which is what menu_page_modal above
			 * exists for.
			 *
			 * Auto-repeat is ignored: holding it would re-enter the
			 * page thirty times a second, and page enters load meshes
			 * and banks.
			 *
			 * It does NOT touch the pause. Restarting while paused
			 * leaves you on the new page's first frame, ready to step
			 * into it with F5, which is the reason to want the key.
			 */
			if (!(lparam & (1 << 30)))
				menu_restart_page();
			return 0;
		}
		/*
		 * The menu gets first refusal. The original routes input
		 * through the menu object's button list, which a page
		 * registers in its enter handler; we have no button object, so
		 * menu_on_key stands in as the one entry point.
		 */
		if (menu_on_key((int)wparam))
			return 0;
		return 0;

	case WM_CHAR:
		/*
		 * THE ONLY SOURCE OF TEXT. A VK code equals an ASCII character
		 * for A-Z and 0-9 and nothing else, so punctuation and space
		 * have to come from here. Nothing else in the game wants
		 * WM_CHAR, so the whole message belongs to the field.
		 */
		if (menu_capture_active()) {
			menu_capture_on_char((int)wparam);
			return 0;
		}
		return 0;

	case WM_KEYUP:
		if (wparam == VK_TAB) {
			tm_set_speed(1.0);
			return 0;
		}
		if (wparam == '7') {
			race_camera_look_back(0);
			return 0;
		}
		return 0;

	/*
	 * ALT-TAB IS THE REASON THIS EXISTS. Switching away sends the
	 * key-up to whoever took focus, not to us, so without this the
	 * clock would still be at 4x when the window came back - and the
	 * symptom, a game that runs fast until you press and release TAB,
	 * reads as a timing bug rather than a stuck key.
	 */
	case WM_SETFOCUS:
		race_set_input_focus(1);
		audio_set_focus(1);
		return 0;

	case WM_KILLFOCUS:
		tm_set_speed(1.0);
		race_set_input_focus(0);
		audio_set_focus(0);
		race_camera_look_back(0);
		/* And the same argument for the mouse: a release that arrives
		 * while another window has it never reaches us, so a held
		 * button would latch on and keep stepping a volume. */
		menu_set_mouse_down(0);
		return 0;

	default:
		break;
	}

	return DefWindowProc(hwnd, msg, wparam, lparam);
}

/* ------------------------------------------------------------------ */
/* Window creation                                                     */
/* ------------------------------------------------------------------ */

/*
 * apply_video_settings - re-derive the frame from the settings.
 *
 * ORIGIN  maybe_Game_RenderFrame @ 0x0047ff90, at 0x00480496
 *
 *             if (g_videoSettingsDirty)
 *                 maybe_Screen3D_UpdateScreen(g_cfgScreenMode,
 *                                             g_fullscreenResolved,
 *                                             g_cfgWidescreen169,
 *                                             DAT_004dc028);
 *
 * which ends in Screen3D_SetViewport (0x00499ba0) re-deriving the
 * viewport, the clip rect and the scanline table from the new size.
 * Ours is the same shape one layer down: the framebuffer IS the
 * surface, so re-deriving it means reallocating it.
 *
 * AT THE TOP OF THE FRAME, NOT AT THE BOTTOM. The original runs it
 * after the present, which is the same instant; ours goes first
 * because fb_create frees g_frameBuffer and nothing may be holding a
 * row pointer across the call. Between frames, nothing is.
 *
 * THE WINDOW IS NOT TOUCHED. Only the back buffer changes shape; both
 * present paths scale it into whatever client area exists, so a 16:9
 * buffer in a 16:9 window simply stops having bars.
 *
 * DIVERGENCE: THE PROJECTION BASES ARE LEFT ALONE. The original's
 * UpdateScreen rewrites them to 394*W/640 and 394*H/480, which would
 * stamp on the 1911 a menu page set on entry and not put it back
 * until the next page change. Ours are owned by whoever set them -
 * race_draw_3d every frame, menu_set_page per page - and they do not
 * depend on the frame size anyway, because both are always set to the
 * same number. See docs/subsystems/projection.txt 7.1.
 */
static void apply_video_settings(void)
{
	int w;
	int h;

	g_videoSettingsDirty = 0;

	options_frame_size(&w, &h);
	if (w == g_frameWidth && h == g_frameHeight)
		return;

	if (!fb_create(w, h)) {
		/*
		 * Put the old size back rather than run on with no
		 * framebuffer at all. If that fails too there is nothing
		 * left to do about it.
		 */
		mrlog_err("display: %dx%d allocation failed - staying at "
		          "%dx%d\n", w, h, kClientWidth, kClientHeight);
		fb_create(kClientWidth, kClientHeight);
		return;
	}

	kClientWidth  = w;
	kClientHeight = h;
	r_set_design_size(w, h);

	g_viewCentreX = g_frameWidth / 2;
	g_viewCentreY = g_frameHeight / 2;
	g_viewMaxX    = g_frameWidth  - 1;
	g_viewMaxY    = g_frameHeight - 1;

	mrlog("display: now %dx%d, 2D origin %d\n",
	      g_frameWidth, g_frameHeight, g_fb2dOriginX);
}

static HWND CreateMainWindow(HINSTANCE hinst)
{
	WNDCLASSEXA wc;
	DWORD       style;
	RECT        rc;

	ZeroMemory(&wc, sizeof(wc));
	wc.cbSize        = sizeof(wc);
	wc.style         = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
	wc.lpfnWndProc   = WndProc;
	wc.hInstance     = hinst;
	wc.hIcon         = LoadIcon(NULL, IDI_APPLICATION);
	wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
	wc.hbrBackground = NULL;    /* no background erase */
	wc.lpszClassName = kWindowClass;

	if (!RegisterClassExA(&wc))
		return NULL;

	/*
	 * Resizable and maximizable. A fixed-size window cannot be snapped
	 * or maximized at all, which would make the rcNormalPosition
	 * handling above untestable. The back buffer keeps whatever size
	 * options_frame_size chose and is stretched to the client area,
	 * aspect preserved, with black bars on whichever axis is left
	 * over - so a 16:9 buffer maximized onto a 16:9 monitor has none.
	 */
	style = WS_OVERLAPPEDWINDOW;

	/* Size so the CLIENT area is exactly kClientWidth x kClientHeight
	 * on first run, before any saved rect is applied. */
	rc.left   = 0;
	rc.top    = 0;
	rc.right  = kClientWidth;
	rc.bottom = kClientHeight;
	AdjustWindowRect(&rc, style, FALSE);

	return CreateWindowExA(0,
	                       kWindowClass,
	                       kWindowTitle,
	                       style,
	                       CW_USEDEFAULT, CW_USEDEFAULT,
	                       rc.right - rc.left,
	                       rc.bottom - rc.top,
	                       NULL, NULL, hinst, NULL);
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

int APIENTRY WinMain(HINSTANCE hinst, HINSTANCE hprev,
                     LPSTR cmdline, int nCmdShow)
{
	HWND hwnd;
	MSG  msg = { 0 };
	BOOL running = TRUE;

	(void)hprev;
	(void)cmdline;

	console_open();
	/*
	 * The log, before anything logs. Failure is not fatal: mrlog
	 * still writes to the console with no file open.
	 */
	if (mrlog_init())
		mrlog("log:    %s\n", mrlog_path());

	mrlog("mracer - MotoRacer reconstruction\n");

	/*
	 * THE DEBUG AGENT, before anything else runs, so a harness can
	 * attach from the first frame. Failure is not fatal - the game
	 * runs exactly the same without it.
	 */
	if (!agent_start())
		mrlog("agent:  failed to start\n");
	mrlog("ini:  %s\n", get_ini_file());
	mrlog("data: %s\n", get_data_dir());

	/*
	 * The asset layer. CLAUDE.md rule 10: a stock install keeps almost
	 * everything inside .BKF archives, so this opens them and every
	 * loader resolves through assets.c. Zero archives is not fatal -
	 * lookups fall through to loose files.
	 */
	asset_init();

	/*
	 * The cosine table, which App_Init builds at the same point in the
	 * original: before anything can rotate. Nothing checks whether it
	 * has been built, there and here alike - an unbuilt table is all
	 * zeros and every rotation collapses, which is loud enough.
	 */
	trig_build_table();

	/*
	 * The clock, before anything measures anything. Also raises the
	 * system timer resolution, so it is paired with tm_shutdown at the
	 * bottom of this function rather than left to process exit.
	 */
	tm_init();

	/*
	 * [OPTIONS], before anything reads a setting. The three volumes
	 * land in audio.c on the way through, so this has to be ahead of
	 * audio_init even though nothing plays yet.
	 */
	options_load();

	/*
	 * THE FRAME SIZE, before the window exists - CreateMainWindow
	 * sizes the client area to it and fb_create allocates it.
	 */
	options_frame_size(&kClientWidth, &kClientHeight);
	mrlog("display: %dx%d%s\n", kClientWidth, kClientHeight,
	      g_options.wide169
	          ? (g_options.wide169Crop ? "  16:9 (the original's crop)"
	                                   : "  16:9")
	          : "");
	audio_init();

	/*
	 * THE FONT TABLE, before any page asks for an id. Font ids are
	 * POSITIONAL AND LIVE IN DATA - font 31 is DataDiskMenu because
	 * it is the 31st entry of FONTS.BPT, not because anything here
	 * says so. Without this a caller has to name a face file, which
	 * is a lookup done by hand and got wrong at least twice.
	 */
	fontdef_load();

	/*
	 * The debug face: 94 characters from 0x20, 12 tall, no palette.
	 * Font id 8 in FONTS.BPT is "InfosDebug", type Direct - so this is
	 * the font the developers printed diagnostics with, and it is the
	 * simplest possible exercise of the whole font path.
	 */
	s_have_font = font_init(&s_debugFont, "Direct") &&
	              font_add_face(&s_debugFont, "FONTES\\DELPHINE\\DEBUG.FNT");
	if (s_have_font) {
		/*
		 * WHITE ON BLACK. The default is already white-on-black, but
		 * saying it here is what exercises slot 0 -> slot 1, which is
		 * the path that rebuilds the colour LUT.
		 *
		 * DEBUG.FNT holds exactly two pixel values, 29 and 128, and no
		 * zeros at all. Through the LUT those become ramp[29 & 0x1f],
		 * near the foreground, and ramp[128 & 0x1f] = ramp[0], the
		 * background - so the glyph comes out white and its box black.
		 * Reading the byte as an intensity instead, which is what this
		 * file did before the blitters were read, gives 29 and 128 as
		 * near-black and mid-grey: black text on a grey box.
		 */
		static const unsigned char white[3] = { 0xff, 0xff, 0xff };
		static const unsigned char black[3] = { 0x00, 0x00, 0x00 };
		font_set_colors(&s_debugFont, white, black);

		/*
		 * The shipped faces are PROPORTIONAL - in DEBUG.FNT the ten
		 * digits are not all the same width - so a changing number
		 * shifts everything after it and a running counter shuffles.
		 * Tabular digits are ours, not the original's.
		 */
		font_set_tabular_digits(&s_debugFont, 1);

		mrlog("font:   DEBUG.FNT loaded, %d tall, digit cell %d\n",
		       font_height(&s_debugFont), s_debugFont.digitCell);
	} else {
		mrlog_err("font:   DEBUG.FNT did not load\n");
	}
	hwnd = CreateMainWindow(hinst);
	if (hwnd == NULL) {
		MessageBoxA(NULL, "Failed to create the main window.",
		            kWindowTitle, MB_OK | MB_ICONERROR);
		return 1;
	}

	if (!fb_create(kClientWidth, kClientHeight)) {
		MessageBoxA(NULL, "Failed to allocate the back buffer.",
		            kWindowTitle, MB_OK | MB_ICONERROR);
		DestroyWindow(hwnd);
		return 1;
	}
	mrlog("back buffer: %dx%d, stride %d px, RGB565\n",
	       g_frameWidth, g_frameHeight, g_frameStridePx);

	/*
	 * The back end. Not fatal if it fails - r_init prints why and
	 * returns 0, and everything below falls back to the GDI present.
	 * The original has the same shape: it loads DDRAW.DLL by name and
	 * complains rather than failing to start.
	 */
	if (!r_init(hwnd, kClientWidth, kClientHeight))
		mrlog("render: no D3D9 device - using the GDI present\n");

	/*
	 * A LOGICAL NAME, not a path: first component MENU picks MENU.BKF,
	 * the rest is the member. These two used to pass
	 * "extracted\\MENU\\..." through data_path(), which handed
	 * asset_load a fully-qualified path it cannot split - so they had
	 * been silently failing since the archive reader landed, printing
	 * "cannot find" and returning. See src/README.txt section 30.
	 */

	/* Menu page 1, the loading screen. Falls back to the synthetic
	 * pattern if the data directory is not where the ini says. */
	/*
	 * THE GEOMETRY PIPELINE IS OWNED HERE, not by whoever happens to
	 * draw first.
	 *
	 * queue_create and vertexpool_create used to live in the geometry
	 * test harness. When that harness was removed, the queue
	 * stopped being created - and queue_primitive drops every
	 * primitive when its buckets are NULL, silently and with no
	 * return value to check. Page 14's models were transformed
	 * correctly and then thrown away.
	 *
	 * A lifetime that belongs to the frame loop should be owned by the
	 * frame loop. Any page that draws 3D needs these, so they are not
	 * a demo's to allocate.
	 */
	if (!queue_create())
		mrlog_err("queue:  create FAILED - no 3D will draw\n");
	if (!vertexpool_create(20480))
		mrlog_err("verts:  pool create FAILED - no 3D will draw\n");

	/*
	 * AND THE PIPELINE'S CONFIGURATION, for the same reason. All of
	 * this used to be set by the test harness as well:
	 *
	 *   g_projScaleX/Y   zero means every vertex projects to a point
	 *   g_nearPlaneZ     zero makes the near test degenerate
	 *   g_viewMaxX/Y     zero makes the cull reject everything as
	 *                    being off screen
	 *
	 * so with the demo off, the geometry path was configured to draw
	 * nothing, and did - correctly, silently, and looking exactly like
	 * a bug in whatever was being worked on.
	 *
	 * o THE PATTERN IS THE LESSON. A test harness that also happens to
	 *   initialise the thing it tests hides the initialisation, and it
	 *   only shows up when the harness is switched off - by which time
	 *   the suspicion is on the new code.
	 *
	 * The original sets these from the display mode; ours follow the
	 * framebuffer, which is the same thing one layer down.
	 */
	g_viewCentreX    = g_frameWidth / 2;
	g_viewCentreY    = g_frameHeight / 2;
	screen3d_open_projection(g_frameWidth, g_frameHeight);
	g_nearPlaneZ     = 1.0f;
	g_viewLeftMargin = 0;
	g_viewTopMargin  = 0;
	g_viewMaxX       = g_frameWidth  - 1;
	g_viewMaxY       = g_frameHeight - 1;
	mrlog("pipe:   %dx%d, centre %d,%d, projScale %.0f, near %.1f\n",
	       g_frameWidth, g_frameHeight, g_viewCentreX, g_viewCentreY,
	       g_projScaleX, g_nearPlaneZ);

	/*
	 * The menu, and the switch for skipping intro sequences.
	 *
	 * The original's startup chain is 25 -> 14 -> 15 -> 3: the Delphine
	 * logo, the title screen, name entry, then the main menu. Each is a
	 * page, so "skip the intro" is not a special case to build - it is
	 * just entering the chain further along.
	 *
	 *     MENU_START_PAGE_DEFAULT   the compile-time switch, menu.h
	 *     [DEBUG] startpage=N       the same thing without a rebuild
	 *
	 * o ONE KNOB, NOT ONE PER SEQUENCE. A boolean per intro would need
	 *   a new flag every time another one is reconstructed, and they
	 *   would interact: "skip the logo but not the title" and "skip
	 *   both" are two settings expressing one number. Naming the page
	 *   to start on covers every combination that exists and every one
	 *   that will, including the attract demo at page 0.
	 *
	 * o THE INI OVERRIDE IS THE ONE THAT MATTERS for iteration speed.
	 *   A compile-time switch still costs a rebuild per experiment;
	 *   this way checking a later page is an edit and a relaunch.
	 */
	{
		int startPage = MENU_START_PAGE_DEFAULT;

		get_ini_value("DEBUG", "startpage", &startPage);
		if (startPage < 0 || startPage >= MENU_PAGE_COUNT) {
			mrlog_err("menu:   [DEBUG] startpage %d is out of range, "
			          "using %d\n", startPage, MENU_START_PAGE_DEFAULT);
			startPage = MENU_START_PAGE_DEFAULT;
		}
		if (startPage != MENU_START_PAGE_DEFAULT)
			mrlog("menu:   starting on page %d, not %d "
		          "([DEBUG] startpage)\n",
		          startPage, MENU_START_PAGE_DEFAULT);

		/* The unlock flags, from the .ini rather than the registry. */
		menu_load_unlocks();
		s_have_page = menu_init(startPage);
	}

	/*
	 * [DEBUG] fastforward=N - how much TAB multiplies the clock by
	 * while it is held. Out of range says so rather than being
	 * clamped silently, so a typo is visible instead of quietly doing
	 * something else.
	 */
	{
		int cfgFast = 0;

		get_ini_value("DEBUG", "fastforward", &cfgFast);
		if (cfgFast > 0 && cfgFast <= 64) {
			s_fastForward = (double)cfgFast;
			mrlog("clock:  TAB fast-forwards at %dx "
			      "([DEBUG] fastforward)\n", cfgFast);
		} else if (cfgFast != 0) {
			mrlog_err("clock:  [DEBUG] fastforward %d is out of range "
			          "1..64, using %.0fx\n", cfgFast, s_fastForward);
		}
	}


	/*
	 * [DEBUG] wavecheck=1 - decode the menu samples and log each
	 * header, for the known-answer diff against
	 *
	 *     python tools/rmnr.py --table decomp\\DATA\\SFX.BKF
	 *                          --match "16_BITS\\\\MENU"
	 *
	 * It loads and frees and nothing else - see wave.h.
	 */
	{
		int cfgWaveCheck = 0;

		get_ini_value("DEBUG", "wavecheck", &cfgWaveCheck);
		if (cfgWaveCheck)
			wave_selftest();
	}

	get_ini_value("DEBUG", "escapequits", &s_escapeQuits);
	if (s_escapeQuits)
		mrlog("main:   ESCAPE quits ([DEBUG] escapequits)\n");

	{
		int cfgAutoLog = 0;

		get_ini_value("DEBUG", "autopilotlog", &cfgAutoLog);
		if (cfgAutoLog) {
			g_autopilotLog = 1;
			mrlog("auto:   autopilot logging on ([DEBUG] autopilotlog)\n");
		}
	}


	if (!s_have_page)
		mrlog_err("menu:   no page came up - the window will be blank\n");

	restore_windows(hwnd);

	/* SEND_INPUT taps keys into this window. */
	agent_set_window(hwnd);

	ShowWindow(hwnd, nCmdShow);
	UpdateWindow(hwnd);

	/*
	 * ---------------------------------------------------------------
	 * THE FRAME LOOP.
	 * ---------------------------------------------------------------
	 *
	 * A game pump, not GetMessage: drain what is pending, then run a
	 * frame whether or not a message arrived. The original does the
	 * same, pacing off its own clock rather than the message queue.
	 *
	 * WHAT THIS REPLACES, and why the old one was choppy. It ended in
	 * Sleep(1) and drove the animation from
	 *
	 *     draw((float)timeGetTime() * 0.001f)
	 *
	 * and timeGetTime advances in ~15.6 ms steps unless somebody has
	 * called timeBeginPeriod. So the logo's angle was quantised to
	 * 15.6 ms while the display presented every 16.67 ms, and the two
	 * beat against each other - every frame arrived on time and the
	 * motion still stuttered. That is why vsync alone did not fix it.
	 *
	 * Now: the clock is QueryPerformanceCounter under tm_real_now, and
	 * the pacer holds the budget. tm_now sits on top of it as the
	 * virtual clock the game reads, which is what the debug pause
	 * freezes. See timing.h and docs/subsystems/frame-loop.txt.
	 */
	{
		/*
		 * THE ORIGINAL STEPS A WHOLE 33 ms, not 1000/30. DAT_004e9810,
		 * the delta its race tick subtracts, is the INTEGER 33, and
		 * g_frameRateMax at 0x004dc200 holds 30. Those disagree by one
		 * part in a hundred and it is measurable: at 1000/30 the port
		 * ran every dt-driven quantity 1.01x hot - the steer ramp read
		 * -111.12 against MOTO.EXE's -108.90, and the heading and slip
		 * both sat at 101%.
		 *
		 * Matching the rate is not cosmetic either way:
		 * racer_lean_response DIVIDES BY THE FRAME DELTA, so an
		 * uncapped build leans twice as hard as retail - 16.6 ms
		 * measured against 33.0. Running at the same period also makes
		 * a frame count mean the same thing on both sides, which is
		 * what differential testing needs.
		 *
		 * [VIDEO] maxfps=N in the ini overrides it; 0 uncaps,
		 * -1 asks for the 33 ms step above.
		 *
		 * BUT THE DEFAULT IS 60, NOT THE ORIGINAL'S 30.  [divergence]
		 *
		 * This is a real behaviour change and not only a smoother
		 * picture: frame_step takes 1/targetFps as the simulation
		 * step, so the physics advances 16 ms at a time instead of
		 * 33, and racer_lean_response - which DIVIDES by the frame
		 * delta - leans about twice as hard.  frame-loop.txt 7
		 * has the warning in full.
		 */
		const double kOriginalStepMs = 33.0;
		int    cfgMaxFps = 60;      /* -1 would be the 33 ms step     */
		int    cfgSimHz  = 30;      /* the original's own rate        */
		double targetFps;

		get_ini_value("VIDEO", "maxfps", &cfgMaxFps);
		get_ini_value("VIDEO", "simhz",  &cfgSimHz);
		if (cfgSimHz <= 0)
			cfgSimHz = 30;

		/*
		 * AN EXPLICIT maxfps SPENDS AN INTEGER MILLISECOND TOO.
		 *
		 * The game's limiter budgets an integer 1000/n ms, so 30
		 * buys 33 and 60 buys 16 - and mrhook's virtual clock now
		 * steps the same amount. Taking cfgMaxFps as a frame RATE
		 * instead gave 1/60 = 16.67, and race_update quantises dt
		 * to whole ms, so the step alternated 16/17/17 against
		 * MOTO's flat 16. Measured: every drivetrain quantity came
		 * back 0.965 of MOTO's over ten frames, which is exactly
		 * 16.0/16.67 and not a physics difference at all.
		 *
		 * The DEFAULT arm already did this - kOriginalStepMs is a
		 * step, not a rate - which is why 30 fps always matched and
		 * only an explicit maxfps drifted.
		 */
		if (cfgMaxFps < 0) {
			targetFps = 1000.0 / kOriginalStepMs;
		} else if (cfgMaxFps == 0) {
			targetFps = 0.0;            /* uncapped - no step */
		} else {
			int stepMs = 1000 / cfgMaxFps;

			/* Past 1000 fps the integer step collapses to 0. */
			if (stepMs < 1)
				stepMs = 1;
			targetFps = 1000.0 / (double)stepMs;
		}

		frame_init(&s_pacer, targetFps, (double)cfgSimHz);

		mrlog("timing: sim %d Hz fixed, render %s, %s timer\n",
		       cfgSimHz,
		       (cfgMaxFps != 0) ? "capped" : "uncapped (vsync paces it)",
		       tm_have_highres_timer() ? "high-resolution"
		                               : "coarse - expect more spin");
		mrlog("        spin margin %.2f ms", tm_wait_margin() * 1000.0);
		if (cfgMaxFps != 0) {
			mrlog(", cap %.2f fps -> %.2f ms budget",
			       targetFps, 1000.0 / targetFps);
		}
		mrlog("\n");
		mrlog("        [VIDEO] maxfps / simhz in %s\n", get_ini_file());
	}

	while (running) {
		double dt;      /* used by the on-screen counter below */

		while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
			if (msg.message == WM_QUIT) {
				running = FALSE;
				break;
			}
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}

		if (!running)
			break;

		/*
		 * A DISPLAY SETTING MOVED. Before anything draws, and
		 * before the pacer's step loop, because this can free
		 * and reallocate the framebuffer.
		 */
		if (g_videoSettingsDirty)
			apply_video_settings();

		dt = frame_begin(&s_pacer);

		/*
		 * THE DEBUG AGENT PATCHES CODE, so it must do it here and
		 * not on its pipe thread - this thread cannot be inside a
		 * function entry while it is at this line. No-op when idle.
		 */
		agent_service();

		/*
		 * FIXED-RATE SIMULATION, AND IT IS STILL A STUB. This loop
		 * advances s_simTime and nothing else; game_tick is commented
		 * out below and was never wired up.
		 *
		 * THE RACE DOES SIMULATE - just not from here. race_update is
		 * called once per RENDERED frame from page01_tick (menu.c)
		 * and takes its dt from the real frame time, not from
		 * s_pacer.fixedStep. So the decoupling timing.h describes is
		 * scaffolding rather than something in force.
		 *
		 * BUT race_update SUB-STEPS THAT DELTA ITSELF, in slices of
		 * at most RACE_MAX_SUBSTEP - so the dt the physics actually
		 * sees is about 16 ms at 30 fps, NOT the 33 ms this comment
		 * used to imply. Do not reason about the step size from here
		 * alone; see frame-loop.txt 1.3.9.
		 *
		 * o WHICH IS WHY THE RENDER CAP IS LOAD-BEARING. The scaled
		 *   delta the physics reads is 3.3 only because a frame is
		 *   held at 33.0 ms; racer_lean_response divides by it, so an
		 *   uncapped build leans about twice as hard. Measured -
		 *   racers-convergence.txt 9.61.2. Moving the race into
		 *   this loop is the fix: see frame-loop.txt 9.
		 *
		 * Input is polled once per frame, here, NOT on a thread. The
		 * original has no CreateThread at all, and nothing in a modern
		 * input API needs one either - see
		 * docs/subsystems/frame-loop.txt section 5.
		 */
		while (frame_next_step(&s_pacer)) {
			s_simTime += s_pacer.fixedStep;
			/* input_poll(); game_tick(s_pacer.fixedStep); */
		}

		audio_update();

		/*
		 * The frame bracket. Screen3D_BeginFrame / EndFrame in the
		 * original; here the clear and the present. Page 1 paints
		 * every pixel with its background, so the clear is only
		 * insurance.
		 */
		if (s_have_page) {
			fb_clear(0);
			menu_draw();
		}

		/*
		 * An overlay in the 2D layer, which is the same place the
		 * original's text goes: Font_DrawTextInternal writes 16-bit
		 * pixels straight into the locked surface rather than blitting
		 * through DirectDraw. Our framebuffer is the same shape - a
		 * bits pointer, a stride in pixels, two bytes each.
		 */
		/*
		 * The overlay's own rectangle, measured where it is drawn
		 * and redrawn on top after the geometry - see below.
		 */
		int ovW = 0;
		int ovH = 0;
		int ovX = 8;

		/* [OPTIONS_CUSTOM] ShowFps - off unless asked for. */
		if (g_options.showFps && s_have_font && s_have_page) {
			char line[96];
			int  n;

			/*
			 * The fps is REAL time - frame_fps averages the real ring -
			 * so it keeps reading the true present rate while paused.
			 * dt is the VIRTUAL step, which is 0 while paused and one
			 * frame on a step, and showing both is the point: they
			 * disagree exactly when the clock is being driven by hand.
			 */
			n = sprintf(line, "%.1f FPS  %.2f MS", frame_fps(&s_pacer),
			            dt * 1000.0);
			if (tm_paused())
				n += sprintf(line + n, "  PAUSED");
			/*
			 * The multiplier is worth showing for the same reason
			 * PAUSED is: the fps stays at the real rate while the game
			 * moves at four times it, and without a label on screen
			 * that combination reads as the fps counter lying.
			 */
			if (tm_speed() != 1.0)
				n += sprintf(line + n, "  FF x%.0f", tm_speed());
			if (g_queuePrimLimit >= 0)
				sprintf(line + n, "  PRIM %d/%d", g_queuePrimLimit,
			            s_lastDispatched);
			/*
			 * TOP CENTRE, NOT TOP LEFT. The upper left is the lap
			 * list and the upper right is BEST LAP - both of them
			 * game HUD that draws after this - so a counter in
			 * either corner ends up underneath. The top middle is
			 * the one strip the race does not use.
			 *
			 * Ours, so there is no original to match.
			 */
			/*
			 * IN FRAME COORDINATES, WITH THE 2D ORIGIN OFF.
			 *
			 * font_draw_text adds g_fb2dOriginX - the authored art
			 * is 640 wide and sits centred in a wider frame - but
			 * this counter is not authored art. It is centred
			 * against g_clipWidth, which is the FRAME, so the
			 * origin pushed it 107 pixels right of centre in 16:9
			 * and left the re-composite rect behind at the centre -
			 * the counter back under the geometry with a strip of
			 * bare background lifted over where it used to be.
			 *
			 * hud.c does the same thing for the whole in-race
			 * overlay, and for the same reason.
			 */
			{
				int origin = g_fb2dOriginX;

				fb_set_2d_origin(0);
				ovW = font_text_width(&s_debugFont, line);
				ovH = font_height(&s_debugFont);
				ovX = (g_clipWidth - ovW) / 2;
				if (ovX < 0)
					ovX = 0;
				font_draw_text(&s_debugFont, ovX, 8, line, 0);
				fb_set_2d_origin(origin);
			}
		}

		/*
		 * The frame skeleton of docs/subsystems/renderer.txt section
		 * 3.1, as far as it exists here:
		 *
		 *     Screen3D_BeginFrame     r_begin_scene
		 *     ...queue primitives     menu_draw_3d
		 *     Screen3D_DrawScene      r_end_scene, which flushes
		 *     Screen3D_Present        r_present
		 *
		 * The 2D layer goes down first. In the original both layers
		 * land in the same DirectDraw surface, and menu pages draw
		 * their sprites during the page tick and only then call
		 * Screen3D_DrawScene - so 2D under 3D is the original's order
		 * as well as the convenient one.
		 */
		if (r_ready()) {
			if (r_begin_scene()) {
				r_draw_framebuffer();
				/*
				 * The menu's 3D layer goes here, not in menu_draw -
				 * the 2D phase above runs outside this bracket.
				 */
				if (s_have_page)
					menu_draw_3d();
				/*
				 * AND THE OVERLAY AGAIN, ON TOP. It already went
				 * into the 2D layer above, which is the
				 * BACKGROUND - so a building or a palm in front
				 * of it hides the counter. Ours, not the
				 * original's, so there is no draw order to
				 * match; render.h r_draw_overlay_rect.
				 */
				if (ovW > 0)
					r_draw_overlay_rect(ovX, 8, ovW, ovH);
				/*
				 * AND THE IN-RACE MENU, for a different reason. The
				 * fps counter is ours and only wants to be legible;
				 * this is GAME content that the original genuinely
				 * draws after the scene. Our 2D layer goes down
				 * before the geometry, which is right for a menu
				 * page and wrong for an overlay on a running race -
				 * a palm tree near the camera was covering the words.
				 */
				{
					int hx, hy, hw, hh;

					/*
					 * THE COUNTER FIRST AND WITH NO PANEL. Its
					 * digits sit over the dial faces, which are
					 * scene geometry, so darkening behind them
					 * would put a grey box on the track.
					 */
					if (hud_counter_overlay_rect(&hx, &hy, &hw, &hh))
						r_draw_hud_rect(hx, hy, hw, hh);

					if (hud_time_overlay_rect(&hx, &hy, &hw, &hh))
						r_draw_hud_rect(hx, hy, hw, hh);

					if (hud_bestlap_overlay_rect(&hx, &hy, &hw, &hh))
						r_draw_hud_rect(hx, hy, hw, hh);

					if (hud_place_overlay_rect(&hx, &hy, &hw, &hh))
						r_draw_hud_rect(hx, hy, hw, hh);

					if (hud_pos_overlay_rect(&hx, &hy, &hw, &hh))
						r_draw_hud_rect(hx, hy, hw, hh);

					if (hud_countdown_overlay_rect(&hx, &hy, &hw, &hh))
						r_draw_hud_rect(hx, hy, hw, hh);

					if (hud_banner_overlay_rect(&hx, &hy, &hw, &hh))
						r_draw_hud_rect(hx, hy, hw, hh);

					if (hud_laplist_overlay_rect(&hx, &hy, &hw, &hh))
						r_draw_hud_rect(hx, hy, hw, hh);

					/* The racer callouts, wherever they landed
					   this frame - hud_callout_overlay_rect. */
					if (hud_callout_overlay_rect(&hx, &hy, &hw, &hh))
						r_draw_hud_rect(hx, hy, hw, hh);

					if (hud_menu_overlay_rect(&hx, &hy, &hw, &hh)) {
						/*
						 * THE PANEL FIRST, THEN THE TEXT. The original
						 * darkens the same box before it draws the
						 * widgets on the software path; on hardware it
						 * defers the fade to after them, which our
						 * separate composite makes moot.
						 */
						r_darken_rect(hx, hy, hw, hh);
						r_draw_hud_rect(hx, hy, hw, hh);
					}
				}
				r_end_scene();
				/*
				 * THE SCREEN CAPTURE THE SHATTER IS TEXTURED WITH,
				 * and this is the only place it can go. The back
				 * buffer is D3DSWAPEFFECT_DISCARD, so it holds
				 * nothing defined once r_present has run, and it is
				 * not finished until r_end_scene has flushed. The
				 * original captures its own persistent software
				 * framebuffer and has no such window. See
				 * glass_capture_if_pending.
				 */
				glass_capture_if_pending();

				/*
				 * AND ONLY NOW DOES THE PAGE CHANGE. The capture above
				 * had to see the outgoing page fully drawn; switching
				 * any earlier hands the shards a picture of the page
				 * being arrived at. See menu_begin_transition.
				 */
				menu_apply_pending_page();

				/*
				 * AND THE EXIT BUTTON.  Page 4 is the original's
				 * quit page and its tick posts WM_CLOSE itself;
				 * menu.c owns no window, so it raises a flag and
				 * the message goes from here.  Same path the
				 * ESCAPE key takes.
				 */
				if (menu_quit_requested())
					PostMessage(hwnd, WM_CLOSE, 0, 0);
				r_present();
			}
		} else {
			fb_present(hwnd);
		}

		/*
		 * What the queue actually dispatched, for the primitive limit
		 * to count down from. Read after the flush, since queue_walk
		 * is what sets it.
		 */
		queue_stats(NULL, &s_lastDispatched, NULL, NULL);

		/*
		 * Wait out whatever is left of the budget. A no-op when
		 * uncapped or when Present already blocked on vsync for longer
		 * than the budget - so having both on is safe, the tighter of
		 * the two simply wins.
		 */
		frame_end(&s_pacer);
	}

	{
		int fromArchive = 0, fromLoose = 0, fromExtracted = 0, missing = 0;
		asset_stats(&fromArchive, &fromLoose, &fromExtracted, &missing);
		mrlog("assets: %d from archives, %d loose, %d from the "
		       "pre-extracted tree, %d missing\n",
		       fromArchive, fromLoose, fromExtracted, missing);
		if (fromExtracted == 0 && missing == 0)
			mrlog("        nothing needed the extracted tree - this "
		           "run would work on a stock install\n");
	}

	if (s_have_font)
		font_free(&s_debugFont);
	hud_free();
	queue_destroy();
	vertexpool_destroy();
	audio_shutdown();
	r_shutdown();
	menu_free();
	bank_free_all();
	asset_shutdown();
	fb_destroy();
	tm_shutdown();
	agent_stop();
	mrlog_shutdown();
	mrlog("exiting\n");
	return (int)msg.wParam;
}
