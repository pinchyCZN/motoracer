/*
 * agent.h - the debug agent: a named pipe an external tool can drive.
 *
 * ORIGIN  NONE - ours. Nothing in this file corresponds to anything in
 * MOTO.EXE; it exists to let a harness read our own process while it
 * runs. Modelled on the cdino_pc agent.
 *
 * The pipe is \.\pipe\mracer_agent, one client, line oriented, text
 * in and text out. Commands:
 *
 *     STATUS
 *         -> OK symbols=<n> base=0x???????? relocated=<0|1>
 *
 *     GET_MEM name=<sym> [offset=<n>] [obj=<file.obj>] len=<n>
 *     GET_MEM addr=<hex> [offset=<n>] len=<n>
 *         -> OK name=<sym> obj=<obj> addr=0x???????? len=<n> lines=1
 *            <len*2 hex digits>
 *
 *     SET_MEM name=<sym> [offset=<n>] [obj=<file.obj>] value=<hex>
 *     SET_MEM addr=<hex> [offset=<n>] value=<hex>
 *         -> OK SET_MEM name=<sym> addr=0x???????? len=<n>
 *            was=<len*2 hex digits>
 *
 *         DATA ONLY, and up to 128 bytes - the input line is 512.
 *         was= is the undo. It crosses to the game thread so a
 *         multi-field write cannot land across a frame boundary;
 *         see the note on it in agent.c.
 *
 *     SET_HOOK add name=<sym> [nargs=N] [when=...] [cap=N] [tag=NAME]
 *                  [log=tok,tok,...]
 *         -> OK SET_HOOK n=<armed> idx=<slot> name=<sym> addr=0x????????
 *
 *     SET_HOOK clear  |  CLEAR_HOOK
 *         -> OK CLEAR_HOOK
 *
 *     SEND_INPUT [keys=<list>] [frames=N] [tap=<list>]
 *         -> OK SEND_INPUT frames=N word=0x? stunt=? paused
 *
 *     GET_TRACE [keep]
 *         -> OK GET_TRACE lines=N [truncated]
 *            <N lines>
 *
 *     SET_DATABP - see its own block below, with agent_service.
 *
 * A REPLY WITH A PAYLOAD SAYS SO IN ITS HEADER, as lines=N. A header
 * without lines= is the whole reply. That is what lets one client read
 * both GET_MEM and GET_TRACE without knowing which it asked for.
 *
 * SET_HOOK patches the FIVE-BYTE /hotpatch preamble 66 90 55 8B EC with
 * a jmp to one of TEN slots. Anything else at the entry is refused
 * rather than patched.
 *
 *     nargs=  real cdecl args, 0..5. Default 1. a0..a4 at or above it
 *             are the caller's stack and are never read.
 *     when=   always (default) | aN | aNnz | aN=IMM | aN!=IMM
 *     cap=    max hits recorded, 0 = unlimited. DEFAULT 200, because a
 *             per-frame function with no cap fills the ring instantly.
 *     tag=    label in the trace line, [A-Za-z0-9._-], max 15.
 *     log=    a0..a4        the argument word
 *             aNf           that word READ AS A FLOAT - which is what a
 *                           float passed by value is
 *             ra            caller, as ra=ADDR ra_fn=name+0xOFF
 *             aNb+OFF       byte  at *(aN+OFF)
 *             aNw+OFF       word  at *(aN+OFF)
 *             aNl+OFF       dword at *(aN+OFF)
 *             aNf+OFF       float at *(aN+OFF)
 *             Default a0, or ra when nargs=0.
 *
 * SEND_INPUT DRIVES THE GAME. A run unpauses, holds keys= for
 * frames= frames, then PAUSES - so the game is always left stopped
 * and whatever is read afterwards is a still frame.
 *
 *     keys=   held for the run, comma list, no spaces:
 *             z (accelerate) x (brake) left right c (stunt).
 *             They assemble the GAME'S OWN word - 1 accel, 2 brake,
 *             4 left, 8 right - which is the same mask MOTO.EXE
 *             takes, so one description can drive both.
 *     tap=    one keypress each, delivered before the run starts:
 *             up down left right enter esc, and the harness keys
 *             f1 (restart the page), step (F5), pause (toggle).
 *             Menus are edge driven, so this is how to work one.
 *     frames= 0..300, default 1. 0 applies the taps and returns
 *             without touching the pause.
 *
 * pause= on SET_HOOK is parsed and rejected: it needs a pause/resume
 * state machine that does not exist yet.
 *
 * HOOKS RUN ON THE GAME THREAD. The pipe thread posts them to a mailbox
 * and agent_service() executes them, so installation cannot race the
 * thread executing the bytes being overwritten.
 *
 * Symbol names are the C names: the map's leading '_' is stripped, so
 * ask for g_racers, not _g_racers. offset defaults to 0 and takes
 * strtol base 0, so 0x40 and 64 are the same and negatives are
 * allowed. obj disambiguates duplicate statics; without it the first
 * match in map order wins. len is 1..32768.
 *
 * THE CALLER IS TRUSTED. Only the symbol name and the length are
 * checked. A bad offset reads whatever is there.
 *
 * THIS IS A Debug|Win32 TOOL, because that is the only configuration
 * that links a .map. Release writes none, so the agent still starts
 * there and STATUS answers symbols=0 - which is the symptom to
 * recognise, not a parser bug.
 *
 * Release would also HOLE the map even if one were asked for: LTCG
 * leaves no COMDAT for a function inlined into every caller. Measured
 * on a Release map - racer_wheel_contacts and racer_lean_delta
 * absent, race_tick_physics and racer_build_pose present, every file
 * static present. Debug does not enable LTCG.
 */
#ifndef MRACER_AGENT_H
#define MRACER_AGENT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Parses the .map and starts the pipe thread. 0 on failure. */
int  agent_start(void);

/* Stops the thread, restores every patched prologue, frees the table. */
void agent_stop(void);

/*
 * agent_service - run any pending hook command. CALL ONCE PER FRAME
 * FROM THE GAME THREAD; it is a no-op when nothing is pending.
 *
 * Without it SET_HOOK, CLEAR_HOOK, GET_TRACE and SET_DATABP answer
 * "hook timed out" - they are deliberately never executed on the pipe
 * thread.
 */
void agent_service(void);

/*
 * SET_DATABP - a HARDWARE DATA WATCHPOINT, four slots.
 *
 *     SET_DATABP addr=584545 [len=1|2|4] [cap=N] [changed] [tag=X]
 *     SET_DATABP clear
 *
 * It answers the question a hook cannot: WHICH INSTRUCTION WROTE THIS
 * FIELD. A hook traps a function entry, so it only ever sees writes
 * that happen to be behind a call; the stores that matter in the
 * physics are inline and have no entry to trap.
 *
 * Hits land in the same ring GET_TRACE drains, one line each with the
 * EIP, the resolved function+offset, and the value before and after.
 *
 * THE EIP IS THE INSTRUCTION AFTER THE STORE. The trap is raised once
 * the storing instruction has retired, so a hit reading eip=X was
 * written by the instruction ENDING at X - the same convention MOTO's
 * side reports.
 *
 * ADDRESSES MUST BE NATURALLY ALIGNED to len, because x86 leaves an
 * unaligned data breakpoint undefined; a misaligned request is
 * refused rather than quietly rounded. MR_Racer is byte-packed, so
 * most fields in it want len=1, which is legal at any address.
 *
 * FOUR IS A HARDWARE LIMIT, not an implementation one - DR0..DR3.
 */

/* The window SEND_INPUT taps keys into. Call once, after create. */
void agent_set_window(void *hwnd);

/*
 * agent_input_get - the driving word SEND_INPUT is holding, if any.
 *
 * Returns 0 when the agent is not driving, and the caller should
 * read the keyboard as usual. Bits are the game's: 1 accelerate,
 * 2 brake, 4 left, 8 right.
 */
int agent_input_get(unsigned int *word, int *stunt);

#ifdef __cplusplus
}
#endif

#endif /* MRACER_AGENT_H */
