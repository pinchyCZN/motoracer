/*
 * race.h - the race: track, grid, racers, camera.
 *
 * THIS IS WHERE GAMEPLAY LIVES - the destination, not a harness to
 * throw away. The real flow is page 1, the loading screen, HANDING
 * OVER to a race; a menu source file is the wrong home for one.
 *
 * The subsystems keep their own files - level.c reads the manifest,
 * trk.c the geometry, grid.c the starting slots, racer.c the racers,
 * horizon.c the sky. What lives here is the orchestration - load them,
 * hold the state, draw a frame - plus the chase camera, which rule 13
 * folded back in.
 *
 * ONE RACE AT A TIME, so the state is file-scope inside race.c rather
 * than a struct threaded through every call. The original does the
 * same; there is no second race to keep separate from the first.
 */

#ifndef MRACER_RACE_H
#define MRACER_RACE_H

#include "racer.h"
#include "trk.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * race_load - read the level manifest, the track, its texture banks
 * and the horizon, then build the starting grid and place the racers.
 *
 * trackIndex is an index into the track list, the same one page 10
 * selects. Returns 1 on success; on failure the race is left unloaded
 * and every draw path below becomes a no-op, so a missing track costs
 * an empty frame rather than a crash.
 */
int race_load(int trackIndex);

/* Release the track and forget the state. Safe on an unloaded race. */
void race_free(void);

/* 1 once a track is loaded and has segments. */
int race_ready(void);

/*
 * The all-time track record, out of Data\bestscor.dat. The BEST
 * LAP block draws it as its own row, which is why retail shows a
 * time there from the moment the race starts.
 */
typedef struct {
	char name[16];
	int  timeMs;
	int  valid;
} MR_TrackRecord;

const MR_TrackRecord *race_track_record(void);

/*
 * THE TOP FIVE, for the results page.
 *
 * BESTSCOR.DAT KEEPS NINE RECORDS PER TRACK, not one. The other
 * eight are the table page 9 shows.
 *
 * RECORD 0 IS NOT A ROW - it is the BEST LAP entry, and it is
 * built differently: its initials are empty and its RACE time
 * is the 0x7f7f7f7f sentinel, leaving only a lap time [verified,
 * decomp/DATA/bestscor.dat]. The rows are records 1 onward, and
 * page 9 draws the first five of them [inferred - the loader
 * FUN_004656c0 fills two separate destinations, 0x00540230 for
 * the lap and 0x005cf500 for the table, which is what that
 * split means].
 *
 * TWO NAMES AND TWO TIMES PER RECORD, which is the thing to
 * get right: +0 is a name the game ships (DSI on every stock
 * row) and +9 is the THREE-LETTER INITIALS the player types -
 * the entry code caps at three. +0xf1 is the race time and
 * +0xf5 the lap time.
 */
#define RACE_RESULT_ROWS 5

typedef struct {
	char name[8];    /* the typed initials, record +9 */
	int  raceMs;     /* record +0xf1                  */
	int  lapMs;      /* record +0xf5                  */
	int  valid;
} MR_RaceResultRow;

/* Row i of the table, or NULL when that place is empty. */
const MR_RaceResultRow *race_result_row(int i);

/* The track's best LAP - record 0. NULL when there is none. */
const MR_RaceResultRow *race_best_lap_record(void);

/* The loaded track, for the map. NULL before a race is ready. */
const MR_Trk *race_trk(void);

/*
 * The lap total for the loaded race - the original's g_nRaceLaps.
 * race_load takes the track table's default unless the menu has
 * already set g_raceLaps.
 */
int race_lap_count(void);

/* One lap time of the viewed racer, ms. 0 for a lap not yet run. */
int race_viewed_lap_time(int lap);

/*
 * The racer the camera is following - g_racers[g_viewedRacerIndex].
 * NULL when no race is loaded.
 *
 * THE HUD WANTS THE VIEWED ONE, NOT THE LOCAL ONE. Hud_DrawCounter
 * reads its speed, gear and tacho through g_viewedRacerIndex, so a
 * replay or a spectator camera shows the bike on screen rather than
 * the one the player owns.
 */
const MR_Racer *race_viewed_racer(void);

/* Its index - g_viewedRacerIndex. Not 1 on a guest. */
int race_viewed_index(void);

/*
 * race_find_named_mesh - a decor TEMPLATE of the loaded track, by
 * name prefix. NULL when there is no race or no such template.
 *
 * ORIGIN  the strncmp walk of maybe_Hud_BindCornerSigns @ 0x00473370
 *
 * THE HUD's ART IS TRACK DECOR. The counter plate and the six corner
 * warning signs are ordinary decor templates that the original picks
 * out of the global mesh array by name once per race, exactly the
 * way maybe_Trk_CountCameras picks out the CAMERAs. Ours looks them
 * up on demand instead of building a table, because the walk is
 * forty strncmps and a race changes tracks rarely.
 */
const MR_TrkMesh *race_find_named_mesh(const char *prefix, int n);

/* The roster, for readers that are not the race itself. NULL or 0
 * when there is no race. */
const MR_Racer *race_racer_at(int index);
int             race_racer_count(void);

/*
 * THE RUNNING ORDER. race_compute_standings rebuilds it;
 * race_finishing_position answers "where is racer N", 0 for the
 * leader and -1 when it is not in the list.
 *
 * ORIGIN  Race_ComputeStandings @ 0x0047e1e0 and
 *         maybe_Race_FinishingPosition @ 0x00459f50
 *
 * See docs/subsystems/race-standings.txt.
 */
void race_compute_standings(void);
int  race_finishing_position(int racerIndex);
int  race_standings_count(void);

/*
 * g_extendedTimeMs - "how long ago the last checkpoint extension
 * was", 5000 down to 0.
 *
 * ORIGIN  the global at 0x00519fec. maybe_Race_UpdateProgress sets
 *         it to 5000 in the same breath as adding the bonus, and
 *         maybe_Game_Tick counts it down by the frame delta.
 *
 * The countdown turns yellow and stops flashing while it is above
 * 4000; the EXTENDED TIME banner shows for the whole five seconds.
 */
extern int g_extendedTimeMs;


/*
 * race_update - advance one frame. The camera steps HERE and nowhere
 * else: the horizon and the 3D pass both read it, and while both of
 * them also stepped it every lerp ran at double rate. See
 * docs/subsystems/camera.txt 4.8.
 */
void race_update(void);

/*
 * race_tick_count - simulation steps since start, monotonic.
 *
 * Counts race_update calls that INTEGRATED, not frames rendered:
 * a paused frame takes a zero step and is not counted. Read it
 * either side of a scripted run to learn what actually happened.
 */
unsigned int race_tick_count(void);

/*
 * race_walls_toggle - rides through the guard rails,
 * for putting the port in the same state as MOTO.EXE under
 * tools/mr_cheats.py --off nowalls. Switches BOTH the wall clip and
 * the rail containment, and PERSISTS across a restart - it is the
 * only writer of the flag.
 */
void race_walls_toggle(void);

void race_camera_cycle_view(void);

void race_camera_toggle_first_person(void);

/*
 * race_camera_look_back - RETAIL'S TAB, "REAR VIEW". HELD, not
 * toggled: down selects look slot 1, up returns to 0, exactly as
 * Input_OnKeyDown case 9 and maybe_Input_OnKeyUp do.
 *
 * It pre-empts BOTH cameras - the original's mode 6 arm
 * falls through to the chase on a non-zero slot at 0x004701b5 - and it
 * suppresses the cockpit meshes, whose gate tests the same field.
 */
void race_camera_look_back(int down);

/* The keyboard is read with GetAsyncKeyState, which ignores focus;
 * this is what stops a background window driving the bike.
 * [DEBUG] inputunfocused=1 restores the old behaviour. */
void race_set_input_focus(int active);

/*
 * race_camera_toggle_side - swing the chase camera 90 degrees to
 * the bike's left and back. OURS, not the original's: retail has a
 * view key but it only swaps high and low from behind. It exists
 * because the fork and the wheels cannot be seen from a chase cam.
 */
void race_camera_toggle_side(void);

/*
 * The 2D backdrop - the sky. Call BEFORE the 3D bracket, because the
 * horizon writes every pixel it covers and the world draws over it.
 */
void race_draw_2d(void);

/* The 3D frame: queue reset, the racers, then the track. */
void race_draw_3d(void);


/* ------------------------------------------------------------------ */
/*
 * THE RACE CHASE CAMERA. Rule 13 folded camera.c and camera.h back
 * in here; this is that section of race.h, not a separate file.
 *
 * Reconstructed from Camera_UpdateChase (0x0046f840), which is what
 * camera mode 7 runs and what a live race actually uses. The full
 * derivation and the live probes behind every constant are in
 * docs/subsystems/camera.txt.
 *
 * THE MODEL, all of it read rather than fitted:
 *
 *     boom   = boomTable[index] + base                  = -484
 *     M      = Ry(-Racer_GetYaw(racer matrix))
 *
 *     raw    = racer.position + (0, 64, boom) * M
 *     eye    = lerp(prevEye, raw, 0.2)
 *     target = racer.position + (0, -160, 1680) * M
 *              lerped 0.5 on x and z, 0.333 on y
 *
 *     look-at from eye to target            <- BEFORE the boom
 *
 *     eye    = racer.position - normalise(eye - racer.position) * boom
 *     eye.y += 191
 *
 * o THE BOOM RUNS AFTER THE LERP, which is the whole reason the
 *   camera holds a constant DISTANCE from the bike while its
 *   direction lags. Reverse the two and the maths reads as broken
 *   rather than as misordered - camera.txt 4.5.
 *
 * o AND THE LOOK-AT RUNS BEFORE THE BOOM. So the original builds its
 *   view matrix from an eye it then moves, and the rendered position
 *   and the view matrix disagree slightly. That is the binary's
 *   behaviour and it is reproduced rather than tidied.
 */

/*
 * The four in-race camera distances the view key cycles through,
 * from the camera record at +0x224. Added to CAM_BOOM_BASE.
 */
#define CAM_BOOM_SLOTS 4

typedef struct {
	/* Where the camera ends up. eye is what the renderer positions
	 * from; viewEye is the pre-boom point the look-at was built at. */
	float eye[3];
	float viewEye[3];
	float target[3];

	/* +0xc8 in the original: the post-lerp, PRE-boom eye. */
	float prevEye[3];

	/* 0 until the first update, which snaps instead of lerping.
	 * The original has no equivalent because maybe_Game_InitRace
	 * seeds the record; ours has nowhere else to get a first eye. */
	int   primed;

	/* Index into the boom table. 0 is the default chase distance. */
	int   boomSlot;

	/*
	 * THE FINISH CAMERA, camera +0x34 and +0x3c.
	 *
	 * selectFlag is set by the caller when the viewed racer has
	 * finished - Camera_SelectView 0x004583e0 - and is THE ONLY
	 * DIFFERENCE between the racing camera and the finished one.
	 * Both run the same mode 7.
	 *
	 * lookYaw IS NOT AN ANGLE. It is a one-way ramp the boom and
	 * the eye drop share; Input_OnKeyDown writes pi into the same
	 * field beside the look-back key, which is what makes it look
	 * like a view angle. See camera.txt 1.14.
	 */
	int   selectFlag;
	float lookYaw;

	/*
	 * THE TRACKSIDE DIRECTOR'S STATE.
	 *
	 * shot and shotTimer are g_nCameraShot (0x004ea5a8) and
	 * g_nCameraShotTimer (0x004dc13c) - globals in the original,
	 * fields here because they belong to the camera and nothing
	 * else reads them.
	 *
	 * directorEye says the director placed the eye itself this
	 * frame, so the chase must not overwrite it. Shot 1 clears it
	 * and hands the frame back to camera_update, which is exactly
	 * what the original does by selecting mode 7 again.
	 */
	int   shot;
	int   shotTimer;
	float orbitYaw;
	int   lookSlot;
	int   directorEye;

	/* Mode 6's view roll, trig-table units. Ours - camera.txt 3.2. */
	int   roll;

	/*
	 * THE LOOK BRANCH WRITES THE VIEW MATRIX ITSELF, so the record
	 * has to carry one. The original always has - camera+0x68 - and
	 * only the port built it in the draw; viewValid says which of the
	 * two produced this frame's.
	 */
	float view[12];
	int   viewValid;
} MR_Camera;

/* Forget all state. The next update snaps rather than eases in. */
void camera_reset(MR_Camera *c);

/*
 * camera_side - the rigid side view, ours. It is the shape of
 * Camera_UpdateChase's look-slot branch: a bike-local offset
 * placed outright, with no eye lerp and no boom.
 *
 * DO NOT ROUTE IT THROUGH camera_update. That was the first
 * version and the lerp dragged the eye round behind the bike as
 * speed rose - see the comment on the definition.
 *
 * right selects the side. It hangs off matOrientation rather
 * than matBikeRoot so the lean and the wheelie stay visible.
 */
void camera_side(MR_Camera *c, const float racerPos[3],
                 const float racerMatrix[12], int right);

/*
 * MODE 6, retail's F6 - camera.txt 3.2. It takes the racer record
 * rather than a position and a matrix because it needs four fields
 * plus the bike's PersoAnim.
 *
 * DO NOT ROUTE IT THROUGH camera_update: no boom, no settle.
 */
void camera_first_person(MR_Camera *c, const MR_Racer *r);

/* view = view * Rz(roll), roll in trig-table units. */
void camera_apply_roll(float view[12], int roll);

/*
 * THE EIGHT LOOK PLACEMENTS, Camera_UpdateChase's nLookBackKey path.
 * Slot 0 is the ordinary chase; 1 is the rear view the original puts
 * on TAB; 2 and 3 are side views nothing in the game selects.
 * camera.txt 1.2 and 6.
 */
#define CAM_LOOK_SLOTS   8
#define CAM_LOOK_CHASE   0
#define CAM_LOOK_BACK    1

/*
 * camera_look_slot - the look placement, and it is NOT the chase.
 *
 * ORIGIN  Camera_UpdateChase @ 0x0046f840, at 0x0046fbfe
 *
 * It writes eye, viewEye AND view, and DELIBERATELY LEAVES target
 * ALONE - the original does, and that is what produces the pan when
 * the key is released in cockpit view. See camera.txt 6.
 */
void camera_look_slot(MR_Camera *c, const float racerPos[3],
                      const float racerMatrix[12], int slot);

/*
 * camera_update_trackside - the shot director.
 *
 * ORIGIN  Camera_UpdateTrackside @ 0x0046e9f0
 *
 * Cuts between six shots on a frame timer, choosing by whether the
 * track placed a camera near the bike. See camera.txt 5.2-5.3.
 */
void camera_update_trackside(MR_Camera *c, float dt, const MR_Racer *r,
                             const MR_Trk *t);

/*
 * camera_update - one frame of the chase.
 *
 * racerMatrix is the racer's 3x4 at +0x98 in the original - rows
 * then translation, row 2 being forward. Only its yaw is used,
 * exactly as Racer_GetYaw takes it: atan2(m02, m22).
 *
 * dt is g_frameDelta, and the finish-camera ramp is the only thing
 * that uses it.
 */
void camera_update(MR_Camera *c, const float racerPos[3],
                   const float racerMatrix[12], float dt);

/* ------------------------------------------------------------------ */
/*
 * THE RACE TICK - the clock, the per-racer loop and the human/AI
 * fork. Rule 13 folded racetick.c and racetick.h back in here; this
 * is that section of race.h, not a separate file.
 *
 * It is the control-flow spine that physics.c hangs off. Unlike the
 * drivetrain it needed no measurement - it is branches, a countdown
 * and a loop, and every claim here is read straight out of the
 * decompiled bodies of maybe_Race_TickClock (0x0047dae0),
 * Race_TickAllRacers (0x0047df40) and Racer_TickDispatch (0x0045d540).
 *
 *
 * WHAT IS TRANSCRIBED: THE SOLO RACE TICK, and nothing else.
 *
 * The three originals carry a great deal that a single-player race
 * never reaches, and writing it here would be transcribing code
 * nobody can exercise. Each exclusion is marked at the point it
 * belongs in race.c:
 *
 *   o THE DirectPlay START HANDSHAKE in maybe_Race_TickClock - marking
 *     our net slot 4, waiting for every peer to reach 4, pinging each,
 *     qsorting by latency and busy-waiting on timeGetTime so all
 *     machines leave the grid together. Network only.
 *   o THE TIME-ATTACK AND GHOST BRANCH in Race_TickAllRacers, which
 *     ticks racer 1, plays back the ghost in slot 2 and appends to the
 *     ghost stream. IT RETURNS EARLY - no AI runs in time attack at
 *     all, the main loop is not entered.
 *   o THE REMOTE-PEER PATH, maybe_Racer_TickRemote, for slots somebody
 *     else owns.
 *   o ALL OF maybe_Game_Tick's outer shell - the menu fork, the vtable
 *     dispatch to collision and render, the font reloads, the
 *     race-finish jingles. That is a game-loop interface, not a race
 *     tick, and it belongs with whatever drives the port's main loop.
 *
 *
 * THE ONE TRAP: TWO TIMERS IN ONE VARIABLE
 *
 * g_raceTimer (0x004dc0a8) counts down by the frame delta and is the
 * GRID COUNTDOWN first and the RACE TIME LIMIT afterwards. Nothing in
 * the variable says which; g_bPreRace (0x004dc0b4) is what
 * disambiguates it, and the changeover happens in one place - the
 * moment the timer goes negative while still on the grid. A reader
 * that assumes it means one thing is wrong for the other phase, so
 * MR_RaceClock keeps the flag beside the counter rather than leaving
 * a caller to remember.
 */

/*
 * The race-level state the tick owns.
 *
 * timerMs is signed and is MEANT to go negative - that is the
 * transition trigger, tested as < 0 in the original, not == 0.
 */
typedef struct {
	int enabled;      /* DAT_004dc0ac 0x004dc0ac - see below       */
	int preRace;      /* g_bPreRace   0x004dc0b4, non-zero on grid */
	int timerMs;      /* g_raceTimer  0x004dc0a8, counts DOWN      */
	int started;      /* set on the frame the grid clears          */
} MR_RaceClock;

/*
 * Non-zero while the grid countdown is running - g_bPreRace. The
 * drive and the AI tick both have to see it or the field jumps
 * the start.
 */
int race_pre_race(void);

/* ------------------------------------------------------------------ */
/*
 * THE LOOK-AHEAD SIMULATOR - a helper for the autopilot, and the
 * only thing in this header that exists for it.
 *
 * race_autopilot_probe runs the REAL physics forward from the
 * current state with one button word and one STEERING ANGLE held,
 * reports where the bike ends up, and restores every byte it
 * touched.
 *
 * axis1 IS THE GAME'S ANALOG STEERING AXIS, 0x8000 centred, the
 * same value the autopilot writes live. Passing it is what makes a
 * steering candidate mean anything: without it a rollout inherits
 * whatever angle the real frame left behind, so every candidate
 * steers identically and only the throttle varies. See race.c for what
 * has to be put back and why the function lives there rather than
 * in autopilot.c.
 *
 * Returns 0 and writes nothing if the race is not running, or if
 * called from inside a probe.
 */
typedef struct {
	float position[3];
	float speed;
	int   segment;
	int   crashed;
} MR_AutoProbe;

int race_autopilot_probe(unsigned int buttons, int axis1, int ticks,
                         float dtScaled, MR_AutoProbe *out);

/*
 * race_clock - the race's own MR_RaceClock, or NULL before a race
 * is loaded. The HUD countdown reads it through here, so it draws
 * nothing rather than a confidently wrong number when there is no
 * clock to read.
 */
const MR_RaceClock *race_clock(void);

/*
 * race_scale_dt - the first thing Race_TickAllRacers does, and it
 * rewrites the global in place.
 *
 *     dt *= 0.1f;                 FLT_TICK_DT_SCALE, 0x004d1970
 *     if (dt < 0.0f) dt = 1.0f;   the constant at 0x004d1900 is 0.0f
 *
 * THE SECOND LINE IS A NEGATIVE GUARD, NOT A FLOOR. It compares
 * against ZERO and substitutes 1.0, which is two orders of magnitude
 * above any step the game sees - so it is a "this should never
 * happen" branch, not clamping. Reading it as a minimum frame time
 * would make every stalled frame enormous.
 *
 * The result is the SCALED unit - 3.3 at 33 ms - which is what the
 * whole tick chain runs on, and which physics.c then multiplies by
 * ten again to get back to milliseconds for the drivetrain alone.
 */
float race_scale_dt(float dtScaled);

/*
 * race_clock_tick - one frame of the clock. Returns non-zero on the
 * frame the race starts.
 *
 * frameDeltaMs is the INTEGER millisecond delta maybe_Game_Tick
 * produces with __ftol - a separate quantity from the scaled float
 * step, and the clock uses only this one.
 *
 * lapTimes is int[count][RACE_LAP_SLOTS], flat.
 *
 * ORDER MATTERS AND IT IS NOT THE OBVIOUS ONE: the timer is
 * decremented BEFORE the per-racer clocks advance, and the start
 * transition is tested AFTER both. So the frame the grid clears also
 * advances no lap time, because preRace was still set when the clocks
 * ran.
 *
 * THE WHOLE CLOCK IS GATED on DAT_004dc0ac - maybe_Race_TickClock
 * opens with `if (DAT_004dc0ac != 0)` and its entire body is inside
 * that test, so the clock can be suspended wholesale. What writes
 * the flag is unread, so it is a field the caller sets rather than
 * something assumed true. [open]
 *
 * THE THREE LOOPS DO NOT AGREE ON WHERE TO START, and that is not a
 * transcription slip:
 *
 *     the clock loop        racers 0..24    25 entries
 *     the start-reset loop  racers 0..24    25 entries
 *     the tick loop         racers 1..24    24 entries
 *
 * Slot 0 is unused, so the difference costs only a wasted iteration
 * on an inactive record - but a port that "tidies" all three to
 * start at the same index has changed one of them.
 */
int race_clock_tick(MR_RaceClock *c, MR_Racer *racers, int count,
                    int localIndex, int frameDeltaMs,
                    int *lapTimes);

/*
 * race_clock_start - the grid-clears transition, called by
 * race_clock_tick and exposed because it is testable on its own.
 *
 * Reloads timerMs as the TIME LIMIT - track+0xda plus a difficulty
 * bonus in solo, plus 120000 on a network game - zeroes every
 * eligible racer's clock and all eight of its lap times, and assigns
 * raceState 1 by comparing startSegment against half the segment
 * count.
 *
 * THE HALF-TRACK TEST IS INVERTED BY THE REVERSE FLAG,
 * g_nRaceReverse at 0x004dc144. It flips which side of the
 * comparison gets raceState 1, which is what a grid straddling the
 * start line needs when the field is pointed the other way.
 * PASSING 0 IN ITS PLACE IS RIGHT FORWARDS AND WRONG IN REVERSE.
 *
 * ONE FLAG, THREE CONSUMERS: maybe_Game_LoadRace writes it at
 * 0x0047b460, and maybe_Game_InitRace reads the same address at
 * 0x0047c0ee and 0x0047c281 for the reverse-only grid step and the
 * reverse orientation.
 *
 * It stays a parameter rather than becoming a global read, because
 * the clock module owns no race state - the same reason the time
 * limit is passed in.
 *
 * hasTimeLimit IS THE START SEGMENT'S gridShort >= 0 - at
 * 0x0047dd6c a signed 16-bit compare, and a NEGATIVE one skips
 * the reload so the timer carries its small negative value on
 * into the race. Only the reload is skipped: the clocks are
 * still zeroed and raceState is still assigned either way.
 */
void race_clock_start(MR_RaceClock *c, MR_Racer *racers, int count,
                      int localIndex, int segmentCount,
                      int hasTimeLimit, int trackTimeMs,
                      int difficultyBonusMs, int invertHalfTest,
                      int *lapTimes);

/*
 * race_tick_all - the per-racer loop. Walks slots 1..count-1, skips
 * anything with active == 0, and dispatches.
 *
 * THE LOOP STARTS AT 1. Slot 0 is unused - g_racers is 0x00581218 and
 * the local player is index 1 at 0x00584421 - and the original's
 * bound comes from walking g_netPlayerStates, 24 slots of 36 bytes,
 * which is where RACER_MAX comes from.
 */
void race_tick_all(MR_Racer *racers, int count, float dtScaled);

/* The per-racer engine note. See racer_audio_tick in racer.c. */
void race_audio_tick(void);
void race_audio_stop(void);

/*
 * race_collide_racers - maybe_Game_CollideRacers (0x0047ecd0), whole.
 *
 * Racer against racer, once a frame, run between the tick and the
 * draw. Every pair at most once; the gates are both active, within
 * four segments, and a swept sphere test at radius 48 each.
 *
 * *** IT DOES NOTHING IN PRACTICE MODE *** - the original's whole
 * body is under g_nSelectedMode != 0 - and a qualifying impact only
 * CRASHES one time in four. Both matter when testing it; see
 * docs/subsystems/racer-collision.txt section 7.
 */
void race_collide_racers(MR_Racer *racers, int count, float dtScaled);

/*
 * racer_tick_dispatch - the human/AI fork, Racer_TickDispatch
 * (0x0045d540).
 *
 *     g_frameDelta = dt;
 *     if (!humanPlayer)  maybe_Racer_TickAI(this, dt);
 *     else               maybe_Racer_Tick(this, buttons, a0, a1, dt);
 *
 * IT PUBLISHES THE STEP TO A GLOBAL before forking, which is how
 * functions further down read it without being passed it - and is
 * worth knowing before wondering where maybe_Racer_UpdateAirborne
 * gets its dt from.
 *
 * BOTH ARMS ARE REAL: maybe_Racer_TickAI is racer_tick_ai and
 * maybe_Racer_Tick is race_tick_physics. The AI arm gets only the
 * delta, as the original's does - no buttons, no axes - so an
 * opponent does not drive through a synthetic controller.
 */
void racer_tick_dispatch(MR_Racer *r, unsigned int buttons,
                         int axis0, int axis1, float dtScaled);

/* The step the dispatch published, g_frameDelta. */
float race_frame_delta(void);

/*
 * race_final_place - the finishing position the 3D plaque is
 * showing, or -1 when no plaque is due.
 *
 * ORIGIN  Race_DrawFinalPlaque @ 0x0047b7a0, its place test
 *
 * hud.c asks through this so the small text readout
 * (hud_draw_finish_place, FUN_00481970) stands down whenever the
 * plaque is up. The original gets the same exclusivity by
 * repeating the four terms in the HUD dispatcher's podium test;
 * one predicate in one place is the port's way of guaranteeing
 * the two cannot drift apart.
 */
int race_final_place(void);

/* g_nRaceOverHud - the post-race HUD gate. See race.c. */
int race_over_hud(void);

/*
 * The post-race sequence - the timer that runs from the line to the
 * menus, and the auto-ride it can arm on the way.
 *
 * ORIGIN  FUN_00412a50 @ 0x00412a50, FUN_00410120 @ 0x00410120
 *
 * race_auto_ride is what switches the trackside director on, the
 * way DAT_00504ea0 does in Camera_SelectView.
 */
void race_post_race_tick(int frameDeltaMs);
/*
 * Non-zero while the post-race auto-ride is hiding the counter.
 *
 * THE CONFIG VALUE IS NOT TOUCHED, which is the point - see the
 * note on s_race_counterHidden in race.c.  hud_counter_type
 * reads this as well as g_options.showCounter.
 */
int  race_counter_hidden(void);

void race_post_race_reset(void);
int  race_auto_ride(void);

/*
 * THE REPLAY - inputs, not positions, played back through the real
 * physics. See docs/subsystems/replay.txt and race.c.
 *
 * ORIGIN  maybe_Replay_WriteInput @ 0x004577f0
 *         maybe_Replay_ReadInput  @ 0x004578a0
 *
 * race_replay_start rewinds and switches to playback; it returns 0
 * when there is nothing recorded.
 */
void race_replay_reset(void);
int  race_replay_mode(void);
int  race_replay_count(void);
int  race_replay_start(void);
void race_replay_stop(void);


/*
 * race_reset_crashes - end a crash, and possibly the whole field's.
 *
 * The gate-and-cascade half of FUN_004681d0 (0x004681d0). The
 * per-racer work is racer_reset_crash; this decides WHO gets it, and
 * that decision is the interesting part of the function.
 *
 * THE GATE:
 *
 *     index == viewedIndex  ||  racer[viewedIndex].crashLatch == 0
 *
 * so a racer only gets back on the bike if he is the one being
 * WATCHED, or if the watched racer is not himself down. While the
 * camera's racer is crashed, everybody else stays crashed with him.
 *
 * THE CASCADE, which only the viewed racer triggers:
 *
 *     for i in 1 .. racerCount-1
 *         if i != index and racer[i].active and racer[i].crashLatch
 *             reset racer[i]
 *
 * Put together, those two produce a restart that happens to the whole
 * field AT ONCE rather than racer by racer. Nobody remounts early and
 * nobody is left down. [inferred - the mechanism is verified, the
 * intent behind it is not]
 *
 * IT TERMINATES IN ONE LEVEL. The recursion looks unbounded but is
 * not: a cascaded racer has index != viewedIndex, so it fails the
 * cascade test and only ever does its own reset. It reaches the work
 * at all because the viewed racer's crashLatch was cleared one line
 * earlier, which is what opens the gate for everybody else.
 *
 * THE LOOP STARTS AT 1, so racer 0 is never cascaded into - the
 * original walks from 0x00584489, which is racer[1]+0x68 [verified
 * from the disassembly at 0x00468257].
 *
 * Returns how many racers were reset.
 */
int race_reset_crashes(int index, int viewedIndex);

/*
 * THE TIME-ATTACK GHOST. The stream pair, the two tick halves and
 * the disk I/O are static in race.c; these are the only parts
 * anything else needs.
 *
 * race_ghost_race_done promotes the run just taped to the one being
 * raced and WRITES IT TO DISK, once, at race end - the dispatch
 * calls it, not the lap counter. race_ghost_has_lap says whether
 * slot 2 has anything to play; the grid build reads it to decide
 * whether the ghost is active at all.
 */
void race_ghost_race_done(const MR_Racer *r);
int  race_ghost_has_lap(void);

/*
 * RESET TIME ATTACK on the results page. Deletes the saved lap for
 * the current track, direction and lap count, and empties the
 * stream so the button takes effect without waiting for a reload.
 */
void race_ghost_erase(void);

/*
 * THE CAMERA THE LAST 3D FRAME USED - the view matrix and the
 * post-boom eye. The HUD needs both to project a world point, and it
 * runs after race_draw_3d has returned and dropped its locals.
 *
 * The original reads them off the camera record instead; ours has no
 * record, so race_draw_3d keeps a copy. Valid only after a frame has
 * been drawn.
 */
const float *race_camera_view(void);
const float *race_camera_eye(void);


/*
 * THE NETWORK SIDE. race_net_install registers the race half of
 * MR_NetHandlers; menu.c owns the lobby half. race_net_send_state
 * goes out once a tick while g_soloMode is clear.
 */
void race_net_install(void);
void race_net_send_state(void);

#ifdef __cplusplus
}
#endif

#endif /* MRACER_RACE_H */
