/*
 * mrhook.c - a capture harness for the original MOTO.EXE.
 *
 * Deployed as dplayx.dll beside the game. It changes NO BYTES IN THE
 * EXE ON DISK: the loader maps it because MOTO.EXE statically imports
 * DirectPlay, DllMain runs before the game's entry point, and
 * everything after that happens in memory.
 *
 * WHY dplayx AND NOT ddraw. Two reasons, and the second is the one
 * that matters on a modern machine:
 *
 *   o Nothing on the path to the bike-select page needs DirectPlay, so
 *     stubs that fail both calls cost nothing. A ddraw proxy would
 *     have to return a working IDirectDraw or the game dies.
 *
 *   o DIRECTPLAY IS AN OPTIONAL WINDOWS FEATURE AND IS OFF BY DEFAULT
 *     on Windows 10. Without dplayx.dll the loader refuses to start
 *     the process at all, before any of our code runs. This file is
 *     therefore also the fix for that.
 *
 * WHAT IT IS FOR. docs/runtime-capture.txt has the argument in full.
 * The short version: page 10's remaining errors are small and
 * systematic, several causes each fit, and the alternative to reading
 * the original's own numbers is nudging constants until the picture
 * matches - which CLAUDE.md rule 5a exists to stop.
 *
 * WHAT IT AVOIDS WHERE IT CAN: detouring. Every value we want persists
 * in a global for as long as the page is up, so a snapshot on a hotkey
 * gets all of it with no trampolines and no risk of breaking the frame
 * loop.
 *
 * TWO THINGS DO PATCH CODE, both deliberate. The registry helpers are
 * replaced outright at startup - see mr_reg_get_string. And HOOK_FUNC
 * replaces a MOTO function with an export of a module we load, so our
 * reconstruction can be run inside the original and compared against
 * it directly. That one does not return to the game code at all; the
 * five entry bytes are kept so it can be undone. This file used to say
 * it detoured nothing, and that stopped being true with HOOK_FUNC.
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <intrin.h>
#include <mmsystem.h>

#pragma comment(lib, "winmm.lib")

/*
 * ADDRESSES ARE FOR THE PREFERRED BASE, 0x00400000, and every one is
 * put through rebase() before use. A 1997 binary carries no
 * DYNAMIC_BASE flag so it will load there, but the image does have a
 * .reloc section and costing one subtraction to be certain is
 * cheaper than debugging a wild pointer.
 */
#define IMAGE_PREFERRED_BASE 0x00400000u

/* The two registry readers, both __stdcall with three arguments. */
/*
 * WHAT -NoD3D SETS. Main_InitAndParseCommandLine compares argv
 * against "-NoD3D" at 0x004dc4a4 and, on a match, stores 1 here
 * (0x004148ac). Settings_LoadAll at 0x0042bc40 reads it back.
 *
 * THOSE ARE THE ONLY TWO REFERENCES IN THE IMAGE - one write, one
 * read - which is what makes setting it from DllMain safe. Our
 * DllMain runs during process init, before the EXE's entry point
 * and so before both the parser and Settings_LoadAll; and because
 * the parser only writes when the flag is actually present, an
 * absent -NoD3D cannot overwrite what we put here.
 */
#define ADDR_NO_D3D          0x004dc198u   /* -NoD3D, 1 = software  */

#define ADDR_REG_GET_STRING  0x0045a330u   /* (key, value, out)      */
#define ADDR_REG_GET_DWORD   0x0045a3f0u   /* (key, value, outDword) */

/*
 * THE MENU RACER, THE POSE RECORDS, THE MESHES AND THE CARAC TABLE
 * USED TO BE DECLARED HERE - about sixty constants that existed only
 * so the F11 dump could print them. They went with it.
 *
 * They are not lost, they were only ever a copy: the 100-byte part
 * record is in docs/structs/MR_RacerPart.txt, the g_pilotRot table in
 * docs/runtime-capture.txt section 9.2, the carac layout in
 * docs/formats/carac-bpt.txt. An address that is defined here and
 * used nowhere is a claim nothing checks, which is how two of them
 * had already drifted one field out.
 *
 * GET_MEM takes an address, so the tools in tools/ carry these now -
 * mr_dump_racer.py holds the whole racer map in one table that four
 * scripts share.
 */

#define ADDR_MENU            0x004dc0c4u   /* MR_Menu*, +0x3c page   */
#define MENU_OFF_PAGE        0x3cu

#define ADDR_SELECTED_BIKE   0x00584499u   /* STATUS reports it     */

#define CAPTURE_KEY          VK_F11

/*
 * RESTART_KEY is F1 on our port and "2" here, because MOTO binds
 * F1 itself. It is the same idea: crash, press one key, test
 * again - without walking the pause menu by hand every time.
 *
 * It posts ESC then ENTER, which is what tools/mr_reset.py does,
 * and nothing more. THE COUNTDOWN IS A SEPARATE PROBLEM: this
 * gets you to the grid, and mr_cheats.py --on nocountdown is what
 * makes the grid start immediately. Without that patch the six
 * seconds still run.
 */
#define RESTART_KEY          0x32u        /* the "2" key         */

/*
 * SIDE_CAM_KEY is "1", the same key our port puts it on, and it
 * TOGGLES rather than holds.
 *
 * THE SIDE VIEW IS ONE THE GAME ALREADY HAS. nLookBackKey is not a
 * flag - Camera_UpdateChase (0x0046f840) uses it as an INDEX into
 * two tables in the camera record, pLookOffset[i*3] for where the
 * eye sits and pLookAngles[i*3] for where it points, and slot 0 is
 * the ordinary chase. Read live:
 *
 *     slot   offset x,y,z            yaw
 *       0      0.00 -160.00 1680.00    0     the chase
 *       1      0.00  160.00  -20.00  180     TAB, look back
 *       2   -250.00  150.00    0.00   90     the LEFT side
 *       3    250.00  150.00    0.00  -90     the RIGHT side
 *       6      0.00  400.00  400.00  180     raised, pitched -45
 *
 * so a side view is nLookBackKey = 2 and nothing else.
 *
 * flLookYaw IS NOT THE ANGLE and writing it does nothing here. It
 * is read only on the nLookBackKey == 0 path, where it feeds the
 * boom length; the look branch never touches it. Setting it to pi/2
 * with nLookBackKey = 1 - which is what this used to do - therefore
 * gave slot 1, the full 180, and the pi/2 was ignored.
 *
 * IT IS RE-ASSERTED WHILE ON, every poll. TAB's key-up handler
 * writes nLookBackKey = 0, and the game clears the record on the
 * race restart the 2 key causes, so a one-shot write would
 * disappear the first time either happened. That is also why the
 * toggle keeps its own flag rather than reading the field back: a
 * cleared field would read as "off" and the next press would be a
 * no-op.
 *
 * PRESSING TAB WHILE IT IS ON FIGHTS IT, same field. Look back or
 * look sideways, not both.
 */
#define SIDE_CAM_KEY         0x31u        /* the "1" key         */
#define CAM_RECORDS          0x005161c0u  /* g_abCameraRecords   */
#define CAM_INDEX            0x004dc0f8u  /* which one is live   */
#define CAM_STRIDE           0x238u       /* sizeof MR_Camera    */
#define CAM_OFF_LOOKYAW      0x03cu
#define CAM_OFF_LOOKSLOT     0x21cu       /* nLookBackKey        */
#define CAM_SLOT_CHASE       0u
#define CAM_SLOT_LEFT        2u           /* 3 is the right side */
/*
 * THE SIDE VIEW ROLLS WITH THE BIKE, and one immediate fixes it.
 * Camera_UpdateChase's look-slot path builds the view from the
 * racer matrix it loads at 0x0047000f:
 *
 *     0047000f  ADD EAX, 0x0058406d      g_racers + 0x2e55
 *
 * racer+0x2e55 is matBikeRoot and it CARRIES the wheelie pitch -
 * measured at wheelieAngle pi/2 it reads 0.70711 in m[7] while
 * matOrientation at +0x00bc reads 0.00000. Pointing the ADD at
 * +0x00bc gives a level view and changes nothing else. Measured
 * either way through the same wheelie: view m[4] holds 1.0000
 * patched and falls to 0.7071 unpatched.
 *
 * DO NOT LEVEL racer+0x2e55 ITSELF. maybe_Racer_Draw copies it at
 * 0x0045f2e5 and maybe_Racer_ChassisAttitude reads it, so the bike
 * and the suspension would tilt with the camera. Patch the CAMERA'S
 * read, not the field.
 */
#define CAM_SRC_IMM          0x00470010u  /* the imm32 of that ADD */
#define CAM_SRC_PITCHED      0x0058406du  /* g_racers + 0x2e55     */
#define CAM_SRC_LEVEL        0x005812d4u  /* g_racers + 0x00bc     */

/*
 * THE COUNTDOWN IS ONE IMMEDIATE.
 *
 *     00411293  MOV [0x004dc0b4],1        the countdown flag on
 *     0041129d  MOV [0x004dc0a8],0x1770   6000 ms on the clock
 *
 * so the six seconds are the four bytes at 0041129d+6. Zero them
 * and maybe_Race_TickClock takes the timer negative on its first
 * tick and starts the race itself - the lap state, the racer
 * reset and the input gate all run exactly as they would have
 * six seconds later. Nothing here reimplements the start.
 *
 * IT IS THE COUNTDOWN, NOT THE TIME LIMIT, even though both live
 * in 0x004dc0a8. The limit is written separately once the grid
 * clears, so a patched race still has its full allowance.
 *
 * PATCHING IT AT LOAD IS SAFE. DllMain runs before the EXE entry
 * point, so nothing is executing this code yet and hook_patch
 * needs no thread suspend - which it could not have here anyway,
 * since the thread it would suspend is this one.
 */
#define ADDR_COUNTDOWN_MS    0x004112a3u
#define COUNTDOWN_LEN        4u

static unsigned int s_delta;
static char         s_outPath[MAX_PATH];
static char         s_dataPath[MAX_PATH];
static char         s_savingPath[MAX_PATH];
static char         s_language[64];
static int          s_noD3D;
static int          s_restartKey = 1;
static int          s_sideCamKey = 1;
static int          s_noCountdown;
static int          s_captureIndex;

static void *rebase(unsigned int va)
{
	return (void *)(va + s_delta);
}

/* ------------------------------------------------------------------ */
/* The DirectPlay stubs                                               */
/* ------------------------------------------------------------------ */

/*
 * DPERR_UNAVAILABLE. The game checks the HRESULT and gives up on
 * networking, which is what we want: the menus we care about never
 * touch it, and pretending it works would only get us further into
 * code that needs a real service.
 */
#define MR_DPERR_UNAVAILABLE ((HRESULT)0x80004005L)

HRESULT WINAPI DirectPlayCreate(void *guid, void **out, void *outer)
{
	(void)guid;
	(void)outer;
	if (out)
		*out = NULL;
	return MR_DPERR_UNAVAILABLE;
}

HRESULT WINAPI DirectPlayEnumerateA(void *callback, void *context)
{
	(void)callback;
	(void)context;
	return MR_DPERR_UNAVAILABLE;
}

/* ------------------------------------------------------------------ */
/* Logging                                                            */
/* ------------------------------------------------------------------ */

static void logf(const char *fmt, ...)
{
	FILE   *f;
	va_list ap;

	f = fopen(s_outPath, "a");
	if (!f)
		return;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fclose(f);
}

/* ------------------------------------------------------------------ */
/* The two registry helpers, replaced outright                        */
/* ------------------------------------------------------------------ */

/*
 * The originals read HKEY_LOCAL_MACHINE\SOFTWARE\DelphineSoft\
 * MotoRacer\CurrentVersion\Moto and insist the value be REG_SZ
 * [verified, RegOpenKeyExA((HKEY)0x80000002, ...) at 0x0045a330].
 *
 * o WE REPLACE THEM RATHER THAN CREATE THE KEY. Writing HKLM needs
 *   administrator, and from a 32-bit process it lands in
 *   WOW6432Node, which is one more thing to be wrong about. Answering
 *   in-process needs neither and leaves nothing behind on the machine.
 *
 * o AND THEY ARE REPLACED, NOT HOOKED. Nothing here wants the real
 *   behaviour for any value, so there is no trampoline to get wrong -
 *   just a jump at the entry.
 */
static int __stdcall mr_reg_get_string(const char *key, const char *value,
                                       char *out)
{
	const char *answer = NULL;

	(void)key;
	if (!value)
		return 0;

	if (_stricmp(value, "DataPath") == 0)
		answer = s_dataPath;
	else if (_stricmp(value, "SavingPath") == 0)
		answer = s_savingPath;
	else if (_stricmp(value, "Language") == 0)
		answer = s_language;

	if (!answer || !answer[0])
		return 0;               /* as the original does when absent */
	if (out)
		strcpy(out, answer);
	return 1;
}

static int __stdcall mr_reg_get_dword(const char *key, const char *value,
                                      unsigned int *out)
{
	(void)key;
	if (!value || !out)
		return 0;

	/*
	 * Version 1 and InstallType 3 are what the callers default to
	 * when the read fails, so answering with them changes nothing
	 * except that the read succeeds.
	 */
	if (_stricmp(value, "Version") == 0) {
		*out = 1;
		return 1;
	}
	if (_stricmp(value, "InstallType") == 0) {
		*out = 3;
		return 1;
	}
	return 0;
}

/* A five-byte relative jump at `at`, to `to`. */
static int write_jump(unsigned int at, void *to)
{
	unsigned char *p = (unsigned char *)rebase(at);
	DWORD          old;
	int            rel;

	if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old))
		return 0;
	rel = (int)((unsigned char *)to - (p + 5));
	p[0] = 0xe9;
	memcpy(p + 1, &rel, 4);
	VirtualProtect(p, 5, old, &old);
	FlushInstructionCache(GetCurrentProcess(), p, 5);
	return 1;
}

/* ------------------------------------------------------------------ */
/* The F11 trigger - deliberately a stub                              */
/* ------------------------------------------------------------------ */

/*
 * WHAT USED TO BE HERE, AND WHY IT IS NOT.
 *
 * F11 wrote a FIXED report into mrcapture.txt: the selected bike and
 * track, the whole 73-value carac record, three model instances, the
 * camera record, two mesh bounding boxes, the pose part table, the
 * pilot rotation table and up to 512 skeleton vertices. Around three
 * hundred lines of C, and every read in it is now one GET_MEM away.
 *
 * THE DUMP EARNED ITS REMOVAL BY BEING SLOWER TO CHANGE THAN TO USE.
 * Asking it a new question meant editing this file, rebuilding,
 * redeploying and restarting the game. GET_MEM answers a new question
 * in one command against a game that is already running, and the
 * Python in tools/ decodes whatever comes back - mr_dump_racer.py
 * alone now covers more of the racer than this ever printed.
 *
 * THE HOTKEY PLUMBING IS KEPT ON PURPOSE, because there is exactly
 * one thing GET_MEM cannot do: run code AT AN INSTANT THE PLAYER
 * CHOOSES, on the game thread, with no pipe round trip in the way.
 * A structure that is only coherent mid-frame, or a state a 25 ms
 * poll would step over, needs that. If such a question turns up, the
 * body goes in capture_trigger() and nothing else changes.
 *
 * Note the counter is still reported by STATUS as captures=N, so a
 * press is visible from the agent without opening the log.
 */
static void capture_trigger(void)
{
	s_captureIndex++;
	logf("F11 #%d - no action bound; see the comment above "
	     "capture_trigger() in mrhook.c.\n", s_captureIndex);
}

/* ------------------------------------------------------------------ */
/* A guarded read, shared by the hooks and by GET_MEM            */
/* ------------------------------------------------------------------ */

/*
 * THE FAULT GUARD. The range check in agent_get_mem is the real guard;
 * this only catches an unmapped page inside a range that passed it.
 * Without it a single bad address ends the session, which is the one
 * cost this whole harness exists to avoid.
 *
 * A torn read is possible - the game is running and nothing is
 * synchronised - and is accepted. Every value this is used for is
 * either static or stable for as long as a menu page is up.
 */
static int agent_copy(void *dst, const void *src, unsigned int n)
{
	__try {
		memcpy(dst, src, n);
		return 1;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return 0;
	}
}

/* ------------------------------------------------------------------ */
/* Trace hooks - a detour that reads and returns                      */
/* ------------------------------------------------------------------ */

/*
 * WHY THIS EXISTS. GET_MEM reads an address. The values the front
 * wheel work needs - A at [ESP+0x180] and B at [ESP+0xac] inside
 * maybe_Racer_Draw - are stack slots, and a stack slot means nothing
 * once the frame has returned. They have to be read WHILE that frame
 * is live, which means executing code at a chosen instruction.
 *
 * THE SHAPE. Overwrite the instruction with a 5-byte E9 to a naked
 * stub; the stub saves the registers, calls plain C to do the reading,
 * restores, then runs the stolen bytes out of a trampoline and jumps
 * back. Nothing about the game's behaviour changes: the same
 * instructions execute in the same order with the same register and
 * FPU state.
 *
 * FIVE THINGS THAT WOULD MAKE THIS SILENTLY WRONG, and what is done
 * about each:
 *
 *   o INSTRUCTION LENGTH. Whole instructions must be relocated, and
 *     the length cannot be known without decoding. THE CALLER SUPPLIES
 *     IT - SET_HOOK takes the length, read off the Ghidra listing we
 *     are working from anyway. A length disassembler would be more
 *     code and one more thing to be wrong about.
 *
 *   o RELATIVE BRANCHES. E8/E9/EB/Jcc in the stolen bytes would point
 *     somewhere else once moved. hook_arm REFUSES a site that starts
 *     with one. It cannot see a branch in the middle of a longer
 *     stolen run - pick sites off the listing, not by guessing.
 *
 *   o THE ESP ARITHMETIC. Entry is by JMP, not CALL, so no return
 *     address is pushed and ESP at the stub is ESP at the site. Then
 *     pushfd takes 4 and pushad stores the ESP it saw, which is
 *     already 4 low. So the site's ESP is regs->esp + 4, and that +4
 *     is the single easiest thing here to get wrong. It is applied in
 *     one place, hook_dispatch, and the trace prints the corrected
 *     value.
 *
 *   o THE x87 STACK. pushad does not save it, and this function is
 *     full of FPU work - at 0x0045fdb3 the game has a live value in
 *     ST0. NOTHING HERE TOUCHES THE FPU: captures are memory reads
 *     only. A hook that wanted ST0 would need fnsave/frstor and 108
 *     bytes of state, and is a different tool.
 *
 *   o WHERE THE OUTPUT GOES. A hook on a draw function fires every
 *     frame. logf() opens and closes the file per call, so writing
 *     from the hook would wreck the frame rate and race the capture
 *     thread. Hits land in a ring buffer and are drained over the pipe
 *     by GET_TRACE, and a hook auto-disarms after its hit budget so a
 *     forgotten one cannot fill the ring forever.
 */

#define HOOK_MAX        4
#define HOOK_CAP_MAX    4       /* memory captures per hook           */
/*
 * RAISED FROM 32 AND 8. A frame is worth dumping whole rather than
 * guessing which slot holds what: locating a 3x4 by picking offsets
 * and reading the result already produced one false answer, because
 * two capture windows that OVERLAP concatenate into something that
 * can look orthonormal and is not.
 *
 * 24 hits reaches the deep parts of a skeleton walk - seventeen
 * parts over two trees would not fit in eight - and 128 bytes takes
 * a whole small frame in one capture, which cannot overlap itself.
 *
 * GET_TRACE's reply is bounded by AGENT_REPLY_MAX and _snprintf
 * truncates rather than overruns, so asking for 24 hits of four
 * 128-byte captures loses the tail instead of corrupting anything.
 * Use ONE wide capture when the hit count is high.
 */
#define HOOK_CAP_BYTES  128     /* per capture                        */
#define HOOK_TRAMP_MAX  32      /* stolen bytes plus the jump back    */
#define HOOK_HITS_MAX   24      /* PER-SLOT pool, and the budget ceiling */
#define HOOK_JMP_LEN    5

/* The pushad frame, low address first, with pushfd's EFLAGS above it. */
typedef struct {
	unsigned int edi, esi, ebp, esp, ebx, edx, ecx, eax;
	unsigned int eflags;
} HOOK_REGS;

typedef struct {
	int          reg;           /* index into HOOK_REGS, see hook_reg */
	int          offset;
	unsigned int size;
} HOOK_CAP;

typedef struct {
	int           slot;
	unsigned int  hit;
	HOOK_REGS     regs;
	unsigned int  siteEsp;
	int           capCount;
	unsigned int  capSize[HOOK_CAP_MAX];
	int           capOk[HOOK_CAP_MAX];
	unsigned char capData[HOOK_CAP_MAX][HOOK_CAP_BYTES];
} HOOK_HIT;

typedef struct {
	int           armed;
	unsigned int  addr;         /* preferred-base VA                  */
	unsigned int  origLen;
	unsigned int  budget;       /* hits still to take                 */
	unsigned int  hits;         /* hits taken                         */
	int           capCount;
	HOOK_CAP      cap[HOOK_CAP_MAX];
	unsigned char orig[HOOK_TRAMP_MAX];
} HOOK_SLOT;

static HOOK_SLOT     s_hook[HOOK_MAX];
static unsigned char s_hookTramp[HOOK_MAX][HOOK_TRAMP_MAX];
static void         *s_trampPtr[HOOK_MAX];
/*
 * ONE POOL PER SLOT, NOT ONE POOL.
 *
 * This used to be a single HOOK_HITS_MAX buffer claimed by whichever
 * hook fired first, and it cost a whole race of evidence: three hooks
 * were armed, maybe_Game_CollideRacers fired several times a frame and
 * took all eight entries, and the crash and checkpoint hooks reported
 * NOTHING. That is indistinguishable from "the event never happened",
 * which is the expensive way to be wrong.
 *
 * It was worse than crowding. On overflow the old code also did
 * h->budget = 0, so the starved hook DISARMED ITSELF having recorded
 * nothing. A rare hook could be killed by an unrelated hot one.
 *
 * Per-slot pools make a slot's budget and its storage the same size,
 * so a slot can only ever fill its own. s_hookDrained is the consumer
 * cursor: GET_TRACE prints from it and only resets a slot once that
 * slot is fully drained, so a reply that runs out of room continues on
 * the next call instead of throwing hits away.
 */
static HOOK_HIT      s_hookRing[HOOK_MAX][HOOK_HITS_MAX];
static volatile LONG s_hookCount[HOOK_MAX];    /* written by the game  */
static LONG          s_hookDrained[HOOK_MAX];  /* read by the pipe     */
static volatile LONG s_hookDropped[HOOK_MAX];  /* defensive; see above */
static DWORD         s_mainThreadId;

/* ------------------------------------------------------------------ */
/* Function replacement - running our code inside MOTO                */
/* ------------------------------------------------------------------ */

/*
 * THIS IS THE ONE THING THE HEADER SAID WE DID NOT DO.
 *
 * The trace hooks above detour, read and RETURN - the stolen bytes run
 * from a trampoline and the game carries on. These do not return: the
 * entry of a MOTO function is overwritten with a jump into a
 * replacement compiled from src/, and the original never executes.
 *
 * WHY IT IS WORTH THE RISK. The two-process A/B compares racer fields
 * at frame boundaries, and tools/mr_ab.py states its own ceiling: our
 * frames and MOTO frames are not the same thing, so only the SHAPE of
 * a track can be compared. Every difference is also the accumulation
 * of every divergence upstream, and once the worlds differ they drift.
 * A replacement is called by MOTO, with MOTO arguments, in MOTO state,
 * on MOTO frame N. The question stops being "do the two games end up
 * in the same place" and becomes "given these bytes in, does our
 * function produce the same bytes out".
 *
 * THE MECHANISM IS ALREADY PROVEN HERE. write_jump() plus an
 * ABI-matched replacement is exactly how mr_reg_get_string and
 * mr_reg_get_dword stand in for the registry readers at startup - see
 * the comment above them. The only thing that was missing is UNDO:
 * write_jump saves nothing, so this keeps the five original bytes.
 *
 * NO NAKED STUBS, so HOOK_MAX does not apply. That limit exists
 * because each trace hook needs a hand-written stub with its slot
 * baked in; a replacement is a plain jump to an export.
 */

#define FUNC_HOOK_MAX  16
#define FUNC_JMP_LEN   5

typedef struct {
	int           active;
	unsigned int  addr;              /* preferred-base VA            */
	void         *dst;
	char          name[64];
	unsigned char orig[FUNC_JMP_LEN];
} FUNC_HOOK;

static FUNC_HOOK s_funcHook[FUNC_HOOK_MAX];
static HMODULE   s_mod;
static char      s_modName[64];

static const char *const k_hookRegName[8] = {
	"EDI", "ESI", "EBP", "ESP", "EBX", "EDX", "ECX", "EAX"
};

/*
 * THE CAPTURE, IN PLAIN C. Called from the stub with interrupts and
 * registers already saved, so it may do anything except disturb the
 * FPU or block.
 */
static void __cdecl hook_dispatch(int slot, HOOK_REGS *regs)
{
	HOOK_SLOT *h;
	HOOK_HIT  *hit;
	LONG       index;
	int        i;

	if (slot < 0 || slot >= HOOK_MAX)
		return;
	h = &s_hook[slot];
	if (!h->armed || h->budget == 0)
		return;

	h->budget--;
	h->hits++;

	/*
	 * ONE SLOT PER HIT, CLAIMED WITH AN INTERLOCK. The game thread is
	 * the only writer today, but the pipe thread reads the ring and a
	 * second racer would be a second writer; claiming the index is
	 * cheaper than reasoning about whether that can happen.
	 */
	index = InterlockedIncrement(&s_hookCount[slot]) - 1;
	if (index >= HOOK_HITS_MAX) {
		/*
		 * CANNOT HAPPEN while the budget ceiling equals the pool
		 * depth, which hook_cmd_set enforces. Counted rather than
		 * asserted, and NOT allowed to zero the budget - doing that
		 * is what used to make a starved hook disarm itself.
		 */
		InterlockedDecrement(&s_hookCount[slot]);
		InterlockedIncrement(&s_hookDropped[slot]);
		return;
	}
	hit = &s_hookRing[slot][index];

	hit->slot = slot;
	hit->hit  = h->hits;
	hit->regs = *regs;

	/* SEE THE COMMENT ABOVE ON THE ESP ARITHMETIC. pushfd ran first. */
	hit->siteEsp = regs->esp + 4;

	hit->capCount = h->capCount;
	for (i = 0; i < h->capCount; i++) {
		const unsigned int *frame = &regs->edi;
		unsigned int        base;
		unsigned int        at;

		base = (h->cap[i].reg == 3) ? hit->siteEsp
		                            : frame[h->cap[i].reg];
		at = base + (unsigned int)h->cap[i].offset;
		hit->capSize[i] = h->cap[i].size;
		hit->capOk[i] = agent_copy(hit->capData[i], (const void *)at,
		                           h->cap[i].size);
	}

	if (h->budget == 0)
		h->armed = 2;           /* spent; hook_service disarms it     */
}

/*
 * FOUR STUBS, ONE PER SLOT, WITH THE INDEX BAKED IN.
 *
 * A single shared stub would have to work out which site it came
 * from, and entry is by JMP so there is no return address to tell it.
 * Pushing an index would move ESP and break the capture arithmetic
 * above. Four near-identical functions is the cost of not having that
 * problem, and MSVC's naked plus inline asm is x86 only - which this
 * is, and always will be.
 */
static __declspec(naked) void hook_stub_0(void)
{
	__asm {
		pushfd
		pushad
		push esp
		push 0
		call hook_dispatch
		add  esp, 8
		popad
		popfd
		jmp  dword ptr [s_trampPtr + 0 * 4]
	}
}

static __declspec(naked) void hook_stub_1(void)
{
	__asm {
		pushfd
		pushad
		push esp
		push 1
		call hook_dispatch
		add  esp, 8
		popad
		popfd
		jmp  dword ptr [s_trampPtr + 1 * 4]
	}
}

static __declspec(naked) void hook_stub_2(void)
{
	__asm {
		pushfd
		pushad
		push esp
		push 2
		call hook_dispatch
		add  esp, 8
		popad
		popfd
		jmp  dword ptr [s_trampPtr + 2 * 4]
	}
}

static __declspec(naked) void hook_stub_3(void)
{
	__asm {
		pushfd
		pushad
		push esp
		push 3
		call hook_dispatch
		add  esp, 8
		popad
		popfd
		jmp  dword ptr [s_trampPtr + 3 * 4]
	}
}

static void *const k_hookStub[HOOK_MAX] = {
	(void *)hook_stub_0, (void *)hook_stub_1,
	(void *)hook_stub_2, (void *)hook_stub_3
};

/*
 * PATCHING LIVE CODE. write_jump() gets away with no synchronisation
 * because it runs before the game's entry point; this does not, so the
 * game thread is suspended across the write. Its id is recorded in
 * DllMain, which runs on that thread.
 */
static HANDLE hook_suspend_game(void)
{
	HANDLE thread;

	if (!s_mainThreadId || s_mainThreadId == GetCurrentThreadId())
		return NULL;
	thread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, s_mainThreadId);
	if (thread)
		SuspendThread(thread);
	return thread;
}

static void hook_resume_game(HANDLE thread)
{
	if (thread) {
		ResumeThread(thread);
		CloseHandle(thread);
	}
}

static int hook_patch(unsigned char *p, const unsigned char *bytes,
                      unsigned int len)
{
	DWORD old;

	if (!VirtualProtect(p, len, PAGE_EXECUTE_READWRITE, &old))
		return 0;
	memcpy(p, bytes, len);
	VirtualProtect(p, len, old, &old);
	FlushInstructionCache(GetCurrentProcess(), p, len);
	return 1;
}

static int hook_is_relative(unsigned char op)
{
	return op == 0xe8 || op == 0xe9 || op == 0xeb ||
	       (op >= 0x70 && op <= 0x7f) || op == 0x0f;
}

static int hook_arm(int slot, unsigned int addr, unsigned int origLen,
                    unsigned int budget)
{
	HOOK_SLOT     *h = &s_hook[slot];
	unsigned char *p = (unsigned char *)rebase(addr);
	unsigned char  patch[HOOK_TRAMP_MAX];
	HANDLE         thread;
	int            rel;
	unsigned int   i;
	DWORD          old;

	if (origLen < HOOK_JMP_LEN ||
	    origLen > HOOK_TRAMP_MAX - HOOK_JMP_LEN)
	    return 0;
	if (IsBadReadPtr(p, origLen))
		return 0;
	if (hook_is_relative(p[0]))
		return 0;

	memcpy(h->orig, p, origLen);

	/*
	 * THE TRAMPOLINE: the stolen bytes, then a jump back to the
	 * instruction after them. It lives in a static array, which is not
	 * executable until asked.
	 */
	memcpy(s_hookTramp[slot], p, origLen);
	rel = (int)((p + origLen) - (s_hookTramp[slot] + origLen +
	                             HOOK_JMP_LEN));
	s_hookTramp[slot][origLen] = 0xe9;
	memcpy(s_hookTramp[slot] + origLen + 1, &rel, 4);
	if (!VirtualProtect(s_hookTramp[slot], HOOK_TRAMP_MAX,
	                    PAGE_EXECUTE_READWRITE, &old))
	    return 0;
	FlushInstructionCache(GetCurrentProcess(), s_hookTramp[slot],
	                      HOOK_TRAMP_MAX);
	s_trampPtr[slot] = s_hookTramp[slot];

	/* The detour, with the tail padded so no half instruction is left. */
	rel = (int)((unsigned char *)k_hookStub[slot] - (p + HOOK_JMP_LEN));
	patch[0] = 0xe9;
	memcpy(patch + 1, &rel, 4);
	for (i = HOOK_JMP_LEN; i < origLen; i++)
		patch[i] = 0x90;

	h->addr = addr;
	h->origLen = origLen;
	h->budget = budget;
	h->hits = 0;

	/* Re-arming a slot empties ITS pool and nobody else's. */
	InterlockedExchange(&s_hookCount[slot], 0);
	InterlockedExchange(&s_hookDropped[slot], 0);
	s_hookDrained[slot] = 0;

	thread = hook_suspend_game();
	if (!hook_patch(p, patch, origLen)) {
		hook_resume_game(thread);
		return 0;
	}
	h->armed = 1;
	hook_resume_game(thread);
	return 1;
}

static void hook_disarm(int slot)
{
	HOOK_SLOT *h = &s_hook[slot];
	HANDLE     thread;

	if (!h->armed)
		return;
	thread = hook_suspend_game();
	hook_patch((unsigned char *)rebase(h->addr), h->orig, h->origLen);
	hook_resume_game(thread);
	h->armed = 0;
}

/*
 * A SPENT HOOK IS DISARMED FROM THE PIPE THREAD, not from inside the
 * stub. Restoring the bytes means suspending the game thread, and the
 * game thread cannot suspend itself.
 */
static void hook_service(void)
{
	int i;

	for (i = 0; i < HOOK_MAX; i++) {
		if (s_hook[i].armed == 2)
			hook_disarm(i);
	}
}

/* One capture item: REG[+-]hexoffset:size, e.g. ESP+180:4 */
static int hook_parse_cap(const char *text, HOOK_CAP *out)
{
	char  name[8];
	int   i;
	int   sign = 1;
	char *end;

	for (i = 0; i < 3 && text[i] && text[i] != '+' && text[i] != '-';
	     i++)
	    name[i] = (char)toupper((unsigned char)text[i]);
	name[i] = 0;
	text += i;

	out->reg = -1;
	for (i = 0; i < 8; i++) {
		if (strcmp(name, k_hookRegName[i]) == 0)
			out->reg = i;
	}
	if (out->reg < 0)
		return 0;

	if (*text == '-')
		sign = -1;
	else if (*text != '+')
		return 0;
	text++;

	out->offset = sign * (int)strtoul(text, &end, 16);
	if (end == text || *end != ':')
		return 0;
	text = end + 1;

	out->size = (unsigned int)strtoul(text, &end, 10);
	if (end == text || out->size < 1 || out->size > HOOK_CAP_BYTES)
		return 0;
	return 1;
}

/* SET_HOOK <slot> <hexaddr> <len> <hits> [cap[,cap...]] */
static void hook_cmd_set(const char *args, char *out, size_t max)
{
	HOOK_SLOT   *h;
	char        *end;
	unsigned long slot;
	unsigned long addr;
	unsigned long len;
	unsigned long hits;
	HOOK_CAP     cap[HOOK_CAP_MAX];
	int          capCount = 0;

	slot = strtoul(args, &end, 10);
	if (end == args || slot >= HOOK_MAX) {
		_snprintf(out, max, "ERR SET_HOOK bad slot, 0..%d\n",
		          HOOK_MAX - 1);
		out[max - 1] = 0;
		return;
	}
	args = end;
	addr = strtoul(args, &end, 16);
	if (end == args) {
		_snprintf(out, max, "ERR SET_HOOK bad address\n");
		out[max - 1] = 0;
		return;
	}
	args = end;
	len = strtoul(args, &end, 10);
	if (end == args || len < HOOK_JMP_LEN ||
	    len > HOOK_TRAMP_MAX - HOOK_JMP_LEN) {
		_snprintf(out, max, "ERR SET_HOOK bad length, %d..%d\n",
		          HOOK_JMP_LEN, HOOK_TRAMP_MAX - HOOK_JMP_LEN);
		out[max - 1] = 0;
		return;
	}
	args = end;
	hits = strtoul(args, &end, 10);
	if (end == args || hits < 1 || hits > HOOK_HITS_MAX) {
		_snprintf(out, max, "ERR SET_HOOK bad hit budget, 1..%d\n",
		          HOOK_HITS_MAX);
		out[max - 1] = 0;
		return;
	}
	args = end;

	while (*args == ' ' || *args == '\t')
		args++;
	while (*args && capCount < HOOK_CAP_MAX) {
		if (!hook_parse_cap(args, &cap[capCount])) {
			_snprintf(out, max, "ERR SET_HOOK bad capture near '%s', "
			          "want REG+hexoff:size\n", args);
			out[max - 1] = 0;
			return;
		}
		capCount++;
		while (*args && *args != ',')
			args++;
		if (*args == ',')
			args++;
	}

	hook_disarm((int)slot);
	h = &s_hook[slot];
	memset(h, 0, sizeof(*h));
	h->capCount = capCount;
	memcpy(h->cap, cap, sizeof(cap));

	if (!hook_arm((int)slot, (unsigned int)addr, (unsigned int)len,
	              (unsigned int)hits)) {
		_snprintf(out, max, "ERR SET_HOOK could not arm %08x\n",
		          (unsigned int)addr);
		out[max - 1] = 0;
		return;
	}
	_snprintf(out, max,
	          "OK SET_HOOK slot=%lu addr=%08x len=%lu hits=%lu caps=%d\n",
	          slot, (unsigned int)addr, len, hits, capCount);
	out[max - 1] = 0;
}

static void hook_cmd_clear(const char *args, char *out, size_t max)
{
	char         *end;
	unsigned long slot;
	int           i;

	while (*args == ' ' || *args == '\t')
		args++;
	if (*args == 0) {
		for (i = 0; i < HOOK_MAX; i++) {
			hook_disarm(i);
			InterlockedExchange(&s_hookCount[i], 0);
			InterlockedExchange(&s_hookDropped[i], 0);
			s_hookDrained[i] = 0;
		}
		_snprintf(out, max, "OK CLEAR_HOOK all\n");
		out[max - 1] = 0;
		return;
	}
	slot = strtoul(args, &end, 10);
	if (end == args || slot >= HOOK_MAX) {
		_snprintf(out, max, "ERR CLEAR_HOOK bad slot, 0..%d\n",
		          HOOK_MAX - 1);
		out[max - 1] = 0;
		return;
	}
	hook_disarm((int)slot);
	InterlockedExchange(&s_hookCount[slot], 0);
	InterlockedExchange(&s_hookDropped[slot], 0);
	s_hookDrained[slot] = 0;
	_snprintf(out, max, "OK CLEAR_HOOK slot=%lu\n", slot);
	out[max - 1] = 0;
}

/* Hits recorded for one slot that GET_TRACE has not sent yet. */
static LONG hook_slot_pending(int slot)
{
	LONG count = s_hookCount[slot];

	if (count > HOOK_HITS_MAX)
		count = HOOK_HITS_MAX;
	return count - s_hookDrained[slot];
}

/*
 * GET_TRACE - every hit on ONE line, records separated by '|'.
 *
 * The one-command-one-line rule is what the pipe and the MCP bridge
 * both rest on, and a hit budget of eight keeps the line inside a few
 * kilobytes. cdino's multi-line "lines=N" form is the answer if that
 * ever stops being true; it is not worth the protocol change yet.
 */
static void hook_cmd_trace(char *out, size_t max)
{
	size_t at;
	LONG   pending = 0;
	LONG   left = 0;
	int    slot;

	hook_service();

	for (slot = 0; slot < HOOK_MAX; slot++)
		pending += hook_slot_pending(slot);

	at = (size_t)_snprintf(out, max, "OK GET_TRACE hits=%ld", pending);

	for (slot = 0; slot < HOOK_MAX; slot++) {
		LONG count = s_hookCount[slot];
		LONG i;

		if (count > HOOK_HITS_MAX)
			count = HOOK_HITS_MAX;

		for (i = s_hookDrained[slot]; i < count && at + 384 < max; i++) {
			const HOOK_HIT *hit = &s_hookRing[slot][i];
			int c;

			at += (size_t)_snprintf(out + at, max - at,
			                        " |h%d#%u esp=%08x eax=%08x ecx=%08x "
			                        "edx=%08x ebx=%08x ebp=%08x esi=%08x "
			                        "edi=%08x",
			                        hit->slot, hit->hit, hit->siteEsp,
			                        hit->regs.eax, hit->regs.ecx,
			                        hit->regs.edx, hit->regs.ebx,
			                        hit->regs.ebp, hit->regs.esi,
			                        hit->regs.edi);

			for (c = 0; c < hit->capCount && at + 96 < max; c++) {
				static const char hex[] = "0123456789abcdef";
				unsigned int b;

				at += (size_t)_snprintf(out + at, max - at, " c%d=", c);
				if (!hit->capOk[c]) {
					at += (size_t)_snprintf(out + at, max - at, "FAULT");
					continue;
				}
				for (b = 0; b < hit->capSize[c] && at + 4 < max; b++) {
					out[at++] = hex[(hit->capData[c][b] >> 4) & 0xf];
					out[at++] = hex[hit->capData[c][b] & 0xf];
				}
			}
			s_hookDrained[slot] = i + 1;
		}

		if (s_hookDropped[slot] && at + 96 < max) {
			at += (size_t)_snprintf(out + at, max - at,
			                        " |WARN h%d dropped %ld", slot,
			                        s_hookDropped[slot]);
			InterlockedExchange(&s_hookDropped[slot], 0);
		}

		/*
		 * RECYCLE WITH A COMPARE-EXCHANGE, NOT A STORE. The game
		 * thread can append to this slot between the last _snprintf
		 * and here; a plain store would drop that hit AND rewind the
		 * write index onto it. If the count moved, leave the cursor
		 * alone and let the next GET_TRACE carry it.
		 */
		if (count > 0 && s_hookDrained[slot] >= count) {
			if (InterlockedCompareExchange(&s_hookCount[slot], 0,
			                               count) == count)
			    s_hookDrained[slot] = 0;
		}
	}

	for (slot = 0; slot < HOOK_MAX; slot++)
		left += hook_slot_pending(slot);
	if (left && at + 96 < max)
		at += (size_t)_snprintf(out + at, max - at,
	                            " |MORE %ld hit(s) not shown, call"
	                            " GET_TRACE again", left);

	if (at + 2 < max) {
		out[at++] = '\n';
		out[at] = 0;
	} else {
		out[max - 2] = '\n';
		out[max - 1] = 0;
	}
}

/* ------------------------------------------------------------------ */
/* The agent - a named pipe that answers questions                    */
/* ------------------------------------------------------------------ */

/*
 * A SECOND THREAD, AND IT ONLY READS.
 *
 * The pattern is lifted from N:\DEV\CDINO\cdino_pc\cdino_pc.c: a
 * duplex byte-mode pipe in PIPE_NOWAIT, one client, newline-terminated
 * text in and one line out, and a poll loop whose sleep is also its
 * shutdown wake. Same shape, so anyone who knows that file knows this
 * one.
 *
 * WHAT IS DELIBERATELY LEFT OUT. cdino carries a single-slot mailbox -
 * a CRITICAL_SECTION, an InterlockedCompareExchange on a pending flag,
 * and a reply event - because ADVANCE and RESET have to run on the
 * emulator thread. STATUS and GET_MEM are pure reads of process
 * memory, so this thread does them itself and none of that machinery
 * is needed.
 *
 * THAT IS A SEAM, NOT A SIMPLIFICATION. The first command that has to
 * be synchronised with a frame - stepping, or reading a stack slot
 * while a particular function is executing - needs cdino's mailbox
 * brought over wholesale. Do not bolt such a command onto this loop.
 *
 * WHY A PIPE AND NOT A SOCKET. Named pipes are kernel32, which is
 * already linked; a socket would add ws2_32 and a WSAStartup whose
 * failure mode is a game that will not start.
 */

#define AGENT_PIPE_NAME   "\\\\.\\pipe\\mrhook"
#define AGENT_INPUT_MAX   512
#define AGENT_REPLY_MAX   16384
#define AGENT_GET_MEM_MAX 4096
#define AGENT_POLL_MS     25
#define AGENT_WRITE_MS    5000
#define AGENT_ADDR_MIN    0x00400000u
#define AGENT_ADDR_MAX    0x7fffffffu

/*
 * THE READ CAP IS BOUNDED BY THE REPLY BUFFER, NOT BY TASTE. A reply
 * is hex, so it costs two bytes per byte read, plus a header of about
 * 34. 4096 bytes is 8226 of the 16384 available; 8192 would be 16418
 * and would not fit.
 *
 * That matters more than it looks. agent_get_mem's hex loop stops at
 * the buffer end rather than failing, so an over-large cap would not
 * crash - it would return a SHORT reply that still says OK, and the
 * caller would decode fewer bytes than it asked for. This is a
 * compile-time check so the two constants cannot drift apart:
 * the array is negative-sized if they ever do.
 */
typedef char agent_reply_fits_the_read_cap
    [(AGENT_GET_MEM_MAX * 2 + 64 <= AGENT_REPLY_MAX) ? 1 : -1];

/*
 * PIPE_NOWAIT MEANS A WRITE CAN COME BACK SHORT. ERROR_NO_DATA is the
 * client not having drained yet, not the client being gone; a real
 * disconnect is ERROR_BROKEN_PIPE. Retry on the first and give up on
 * the second, with a wall-clock deadline so a wedged client cannot
 * park this thread forever.
 */
static int agent_write(HANDLE pipe, const char *text)
{
	DWORD total = (DWORD)strlen(text);
	DWORD sent  = 0;
	DWORD start = GetTickCount();

	while (sent < total) {
		DWORD wrote = 0;

		if (WriteFile(pipe, text + sent, total - sent, &wrote, NULL)) {
			sent += wrote;
			if (wrote == 0)
				Sleep(1);
			continue;
		}
		if (GetLastError() != ERROR_NO_DATA)
			return 0;
		if (GetTickCount() - start > AGENT_WRITE_MS)
			return 0;
		Sleep(1);
	}
	return 1;
}

static void agent_status(char *out, size_t max)
{
	unsigned int menu;
	int          page = -1;
	int          bike = -1;
	int          armed = 0;
	LONG         tracePending = 0;
	int          i;

	/*
	 * READ THE MENU POINTER DEFENSIVELY, the same way capture() does:
	 * it is null before the menus are built, and dereferencing it then
	 * would take the game down over a status request.
	 */
	menu = *(const unsigned int *)rebase(ADDR_MENU);
	if (menu && !IsBadReadPtr((void *)(menu + MENU_OFF_PAGE), 4))
		page = *(const int *)(menu + MENU_OFF_PAGE);

	if (!IsBadReadPtr(rebase(ADDR_SELECTED_BIKE), 4))
		bike = *(const int *)rebase(ADDR_SELECTED_BIKE);

	for (i = 0; i < HOOK_MAX; i++) {
		if (s_hook[i].armed)
			armed |= 1 << i;
		tracePending += hook_slot_pending(i);
	}

	_snprintf(out, max,
	          "OK STATUS base=%08x delta=%08x page=%d bike=%d "
	          "captures=%d hooks=%x trace=%ld\n",
	          IMAGE_PREFERRED_BASE + s_delta, s_delta, page, bike,
	          s_captureIndex, armed, tracePending);
	out[max - 1] = 0;
}

/*
 * GET_MEM <hexaddr> <size>
 *
 * THE ADDRESS IS A PREFERRED-BASE VA and goes through rebase(), so an
 * address copied out of Ghidra or out of docs/ works unchanged. That
 * is the whole point: the addresses in this project are all written
 * against 0x00400000, and adjusting them by hand is how a
 * transcription error gets in.
 */
static void agent_get_mem(const char *args, char *out, size_t max)
{
	unsigned char  buf[AGENT_GET_MEM_MAX];
	char          *end;
	unsigned long  addr;
	unsigned long  size;
	unsigned int   phys;
	unsigned int   i;
	size_t         at;

	while (*args == ' ' || *args == '\t')
		args++;
	if (args[0] == '0' && (args[1] == 'x' || args[1] == 'X'))
		args += 2;

	addr = strtoul(args, &end, 16);
	if (end == args) {
		_snprintf(out, max, "ERR GET_MEM bad address\n");
		out[max - 1] = 0;
		return;
	}

	size = strtoul(end, &end, 10);
	if (size < 1 || size > AGENT_GET_MEM_MAX) {
		_snprintf(out, max, "ERR GET_MEM bad size, 1..%d\n",
		          AGENT_GET_MEM_MAX);
		out[max - 1] = 0;
		return;
	}

	/*
	 * BOTH ENDS OF THE RUN ARE CHECKED, and against the REBASED
	 * address, because that is the one about to be dereferenced.
	 * Checking only the start would let a 4096-byte read walk off the
	 * top of the range.
	 */
	phys = (unsigned int)addr + s_delta;
	if (phys < AGENT_ADDR_MIN || phys > AGENT_ADDR_MAX ||
	    phys + size - 1 > AGENT_ADDR_MAX || phys + size - 1 < phys) {
		_snprintf(out, max, "ERR GET_MEM address out of range\n");
		out[max - 1] = 0;
		return;
	}

	if (!agent_copy(buf, (const void *)phys, (unsigned int)size)) {
		_snprintf(out, max, "ERR GET_MEM read faulted at %08x\n", phys);
		out[max - 1] = 0;
		return;
	}

	at = (size_t)_snprintf(out, max, "OK GET_MEM addr=%08x size=%lu ",
	                       (unsigned int)addr, size);
	for (i = 0; i < (unsigned int)size && at + 3 < max; i++) {
		static const char hex[] = "0123456789abcdef";

		out[at++] = hex[(buf[i] >> 4) & 0xf];
		out[at++] = hex[buf[i] & 0xf];
	}
	out[at++] = '\n';
	out[at] = 0;
}

/* ------------------------------------------------------------------ */
/* Data watchpoints - DR0-DR3 and a vectored handler                  */
/* ------------------------------------------------------------------ */

/*
 * WHAT THIS ANSWERS THAT SET_HOOK CANNOT: "which instruction wrote
 * this address". A hook traps a site you already suspect. This traps
 * the DATA, so it finds writers you have not thought of - including
 * the ones a Ghidra displacement search structurally cannot see,
 * because a store through a pointer never names the offset.
 *
 * That hole is not hypothetical here. racer+0x2ea9, the shadow vertex
 * array, has ZERO instructions referencing it in the whole image; it
 * was found only because Racer_Ctor happens to construct it by name.
 *
 * LIFTED FROM N:\DEV\CDINO\cdino_pc\cdino_hooks.c, which implements
 * the same facility with its failure modes documented in place. Three
 * things differ, all because mrhook is simpler:
 *
 *   o ONE ADDRESS SPACE. cdino has a5=/lo=/abs= because it emulates;
 *     here an address is a preferred-base VA through rebase(), the
 *     same as GET_MEM.
 *
 *   o HITS ARE FORMATTED ON THE PIPE THREAD. cdino stages raw hits and
 *     drains them from its game loop. mrhook has no game-thread poll,
 *     and the pipe thread is where every other reply is built anyway.
 *
 *   o EVERY HIT CARRIES ALL EIGHT REGISTERS. The handler is handed a
 *     CONTEXT, so the register file is free. cdino could only report
 *     eip plus a list of RAM fields named in the command; this needs
 *     no capture syntax at all.
 *
 * THE REPORTED EIP IS THE INSTRUCTION AFTER THE STORE. A data #DB is a
 * TRAP, not a fault: it is raised once the storing instruction has
 * retired. So a hit reading eip=0045e2c3 means the write was made by
 * the instruction ENDING at 0045e2c3, not the one starting there. A
 * write performed by the CRT - memset and memcpy compile to rep stos -
 * reports inside the CRT, not in the caller.
 *
 * ONLY THE GAME THREAD IS WATCHED. Debug registers are per-thread.
 * MOTO.EXE runs its frame loop, physics and draw on the thread DllMain
 * loaded on, so this covers everything the racer struct sees, but a
 * write from another thread is invisible and would look like "nothing
 * writes this".
 */

#define DBP_MAX        4        /* hardware limit, DR0-DR3            */
#define DBP_STAGE      64       /* raw hits between drains            */
#define DBP_CAP_DEF    8        /* matches the SET_HOOK hit budget    */
#define DBP_CAP_MAX    64
#define DBP_TAG_MAX    24

#define DBP_F_CHANGED  0x01
#define DBP_F_VAL_EQ   0x02
#define DBP_F_VAL_NE   0x04

typedef struct {
	int           used;
	unsigned int  va;           /* preferred-base VA, as typed        */
	void         *addr;         /* rebased host address               */
	unsigned int  len;          /* 1, 2 or 4                          */
	unsigned int  mode;         /* 0 = write, 1 = read or write       */
	unsigned int  flags;
	unsigned int  valImm;
	unsigned int  shadow;       /* last observed value                */
	unsigned int  cap;          /* 0 = unlimited                      */
	unsigned int  hits;
	char          tag[DBP_TAG_MAX];
} DBP_SLOT;

typedef struct {
	unsigned int  eip;
	unsigned int  eax, ecx, edx, ebx, esp, ebp, esi, edi;
	unsigned int  oldVal;
	unsigned int  newVal;
	unsigned int  hit;
	int           slot;
} DBP_HIT;

static DBP_SLOT      s_dbp[DBP_MAX];
/*
 * PER-SLOT STAGING, for the same reason the hooks have it. This one
 * was already a proper head/tail ring that reported its drops, and
 * with the default cap of 8 the four slots could not between them
 * overflow 64 - but raise two caps to 64 and they can, and then a hot
 * watch eats a rare one's evidence exactly as the hooks used to.
 */
static DBP_HIT       s_dbpStage[DBP_MAX][DBP_STAGE];
static volatile LONG s_dbpHead[DBP_MAX];
static volatile LONG s_dbpTail[DBP_MAX];
static volatile LONG s_dbpDropped[DBP_MAX];

static HANDLE        s_dbpThread;     /* duplicated GAME thread handle */
static PVOID         s_dbpVeh;
static unsigned int  s_dbpLiveDr7;    /* read back after the last arm  */

/*
 * TWO MASKS, AND THEY DIVERGE ON PURPOSE.
 *
 * s_dbpHwMask is what is committed to DR7. s_dbpArmedMask is what we
 * still want to RECORD. The cap auto-disarm drops a slot from the
 * recording mask FROM INSIDE THE HANDLER, but MOV DR7 is CPL-0 and the
 * handler cannot rewrite it - so the slot keeps trapping until the
 * pipe thread can.
 *
 * THE HANDLER MUST GATE ON THE HARDWARE MASK. While DR7 is live every
 * #DB it raises is ours to consume, including the ones we no longer
 * want. Declining one is not a no-op: nobody else claims it, it
 * reaches the unhandled filter, and the process dies.
 */
static volatile unsigned int s_dbpHwMask;
static volatile unsigned int s_dbpArmedMask;

/*
 * A commit the game thread could not make for itself. Set from the
 * handler on a cap auto-disarm; the pipe thread settles it on its next
 * poll, within AGENT_POLL_MS. Deferring is safe because the recording
 * side is already gated in software - a stale DR7 costs a few traps
 * that record nothing.
 */
static volatile LONG s_dbpReapply;

/*
 * RE-ENTRY GUARD. Under mode=rw the handler's own read of the watched
 * address re-traps. There is no way to disarm mid-handler, so the
 * nested path must do nothing at all.
 *
 * A plain static is enough where cdino needed thread-local storage:
 * the handler has already rejected every thread but the game thread by
 * the time this is touched, so only one thread can ever be inside it.
 */
static volatile LONG s_dbpInHandler;

/*
 * Whether the exception CONTEXT carries the debug-register block is
 * not guaranteed, and is doubly uncertain under WoW64 - this is a
 * 32-bit build on a 64-bit OS. If it does not, Dr6 is stack garbage
 * and the per-slot filter cannot be trusted. Capture the first hit's
 * raw values once and surface them through GET_DATABP, because a
 * degraded read is indistinguishable from a correct one otherwise.
 */
static volatile LONG s_dbpDiagDone;
static unsigned int  s_dbpDiagFlags;
static unsigned int  s_dbpDiagDr6;

static unsigned int dbp_read(const void *p, unsigned int len)
{
	const unsigned char *b = (const unsigned char *)p;

	if (len == 1)
		return b[0];
	if (len == 2)
		return (unsigned int)b[0] | ((unsigned int)b[1] << 8);
	return (unsigned int)b[0] | ((unsigned int)b[1] << 8) |
	       ((unsigned int)b[2] << 16) | ((unsigned int)b[3] << 24);
}

/*
 * DR7 from the slot table. LOCAL enable bits, not global: Windows
 * never performs a hardware task switch, so the CPU never auto-clears
 * them, and the whole DR set is saved per thread anyway.
 *
 * Bit 8 is LE. Per slot i:
 * local enable at bit i*2, the R/W field at 16+i*4, the LEN field at
 * 18+i*4. x86 has NO read-only encoding: 01 is write, 11 is read or
 * write, and 10 is I/O. So a "read watch" is an access watch, and a
 * read has to be inferred from the value not changing.
 */
static unsigned int dbp_build_dr7(void)
{
	/*
	 * BIT 10 IS DELIBERATELY NOT SET. The SDM calls it reserved and
	 * read-as-one, and cdino seeds it, but SetThreadContext strips
	 * it on the way in - so seeding it makes dr7_want and dr7_live
	 * differ forever in GET_DATABP. That readback is the one
	 * diagnostic that says whether an arm actually landed, and a
	 * permanent cosmetic mismatch teaches you to ignore it. The arm
	 * demonstrably works without it: a watch armed as 0x00010101
	 * fired and reported the right eip.
	 */
	unsigned int dr7 = 0x100;
	int i;

	for (i = 0; i < DBP_MAX; i++) {
		unsigned int rw, lenBits;

		if (!s_dbp[i].used)
			continue;
		dr7 |= 1u << (i * 2);
		rw = (s_dbp[i].mode == 1) ? 3u : 1u;
		lenBits = (s_dbp[i].len == 1) ? 0u :
		          (s_dbp[i].len == 2) ? 1u : 3u;
		dr7 |= rw << (16 + i * 4);
		dr7 |= lenBits << (18 + i * 4);
	}
	return dr7;
}

/*
 * Commit the slot table to the game thread's debug registers. NOTHING
 * BUT CONTEXT CALLS MAY HAPPEN INSIDE THE SUSPEND WINDOW - no
 * allocation, no logging, no pipe IO - or a suspended lock owner
 * deadlocks us.
 */
static int dbp_apply(char *err, size_t max)
{
	CONTEXT ctx;
	CONTEXT vfy;
	unsigned int mask = 0;
	int i, ok = 0;

	if (!s_dbpThread) {
		_snprintf(err, max, "no game thread handle");
		return 0;
	}
	for (i = 0; i < DBP_MAX; i++) {
		if (s_dbp[i].used)
			mask |= 1u << i;
	}
	/*
	 * Never suspend ourselves - that is an unrecoverable hang, not a
	 * failed arm. Take the software mask now and leave DR7 to the
	 * pipe thread.
	 */
	if (GetCurrentThreadId() == s_mainThreadId) {
		s_dbpArmedMask = mask;
		s_dbpReapply = 1;
		return 1;
	}

	if (SuspendThread(s_dbpThread) == (DWORD)-1) {
		_snprintf(err, max, "SuspendThread failed (%lu)",
		          (unsigned long)GetLastError());
		return 0;
	}
	memset(&ctx, 0, sizeof(ctx));
	ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
	/* GetThreadContext blocks until the suspend has actually landed. */
	if (GetThreadContext(s_dbpThread, &ctx)) {
		/*
		 * Write ALL SIX. If the kernel did not populate the block,
		 * a field we skipped is stack garbage we just armed.
		 */
		ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
		ctx.Dr0 = (DWORD)(size_t)s_dbp[0].addr;
		ctx.Dr1 = (DWORD)(size_t)s_dbp[1].addr;
		ctx.Dr2 = (DWORD)(size_t)s_dbp[2].addr;
		ctx.Dr3 = (DWORD)(size_t)s_dbp[3].addr;
		ctx.Dr6 = 0;
		ctx.Dr7 = dbp_build_dr7();
		if (SetThreadContext(s_dbpThread, &ctx)) {
			/*
			 * Still inside the suspend window, so the game thread
			 * cannot trap between the DR7 write and this store.
			 */
			s_dbpHwMask = mask;
			memset(&vfy, 0, sizeof(vfy));
			vfy.ContextFlags = CONTEXT_DEBUG_REGISTERS;
			if (GetThreadContext(s_dbpThread, &vfy)) {
				s_dbpLiveDr7 = (unsigned int)vfy.Dr7;
				ok = 1;
			} else {
				_snprintf(err, max, "verify GetThreadContext failed");
			}
		} else {
			_snprintf(err, max, "SetThreadContext failed (%lu)",
			          (unsigned long)GetLastError());
		}
	} else {
		_snprintf(err, max, "GetThreadContext failed (%lu)",
		          (unsigned long)GetLastError());
	}
	s_dbpArmedMask = ok ? mask : 0;
	ResumeThread(s_dbpThread);
	s_dbpReapply = 0;
	return ok;
}

/*
 * Pipe thread only. Settle a DR7 change the handler could not make.
 * Silent: the slot is already out of the recording mask, so a failure
 * leaks traps but never a wrong record.
 */
static void dbp_flush_pending(void)
{
	char err[96];

	if (!s_dbpReapply || !s_dbpThread)
		return;
	err[0] = 0;
	dbp_apply(err, sizeof(err));
}

/* Producer - game thread, inside the handler. */
static void dbp_stage_push(const DBP_HIT *hit)
{
	int  slot = hit->slot;
	LONG head;
	LONG next;

	if (slot < 0 || slot >= DBP_MAX)
		return;
	head = s_dbpHead[slot];
	next = (head + 1) % DBP_STAGE;

	if (next == s_dbpTail[slot]) {
		s_dbpDropped[slot]++;
		return;
	}
	s_dbpStage[slot][head] = *hit;
	_ReadWriteBarrier();
	s_dbpHead[slot] = next;
}

static LONG CALLBACK dbp_veh(EXCEPTION_POINTERS *ep)
{
	DBP_HIT      hit;
	unsigned int fired;
	unsigned int flags;
	int          i;

	/* Cheapest rejections first: this runs for every exception in
	 * the process, on every thread. */
	if (!ep || !ep->ExceptionRecord || !ep->ContextRecord)
		return EXCEPTION_CONTINUE_SEARCH;
	if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
		return EXCEPTION_CONTINUE_SEARCH;
	if (s_dbpHwMask == 0)
		return EXCEPTION_CONTINUE_SEARCH;
	if (GetCurrentThreadId() != s_mainThreadId)
		return EXCEPTION_CONTINUE_SEARCH;

	if (s_dbpInHandler) {
		ep->ContextRecord->Dr6 = 0;
		return EXCEPTION_CONTINUE_EXECUTION;
	}
	s_dbpInHandler = 1;

	flags = (unsigned int)ep->ContextRecord->ContextFlags;
	if (InterlockedCompareExchange(&s_dbpDiagDone, 1, 0) == 0) {
		s_dbpDiagFlags = flags;
		s_dbpDiagDr6   = (unsigned int)ep->ContextRecord->Dr6;
	}

	if ((flags & CONTEXT_DEBUG_REGISTERS) == CONTEXT_DEBUG_REGISTERS)
		fired = (unsigned int)ep->ContextRecord->Dr6 & 0xf;
	else
		fired = s_dbpArmedMask;   /* degraded: cannot tell which */
	fired &= s_dbpArmedMask;

	for (i = 0; i < DBP_MAX; i++) {
		DBP_SLOT    *d = &s_dbp[i];
		unsigned int now;

		if (!(fired & (1u << i)) || !d->used)
			continue;
		now = dbp_read(d->addr, d->len);

		if ((d->flags & DBP_F_CHANGED) && now == d->shadow)
			continue;
		if ((d->flags & DBP_F_VAL_EQ) && now != d->valImm)
			continue;
		if ((d->flags & DBP_F_VAL_NE) && now == d->valImm)
			continue;

		hit.eip    = (unsigned int)ep->ContextRecord->Eip;
		hit.eax    = (unsigned int)ep->ContextRecord->Eax;
		hit.ecx    = (unsigned int)ep->ContextRecord->Ecx;
		hit.edx    = (unsigned int)ep->ContextRecord->Edx;
		hit.ebx    = (unsigned int)ep->ContextRecord->Ebx;
		hit.esp    = (unsigned int)ep->ContextRecord->Esp;
		hit.ebp    = (unsigned int)ep->ContextRecord->Ebp;
		hit.esi    = (unsigned int)ep->ContextRecord->Esi;
		hit.edi    = (unsigned int)ep->ContextRecord->Edi;
		hit.oldVal = d->shadow;
		hit.newVal = now;
		hit.slot   = i;
		hit.hit    = d->hits + 1;
		dbp_stage_push(&hit);

		d->shadow = now;
		d->hits++;
		if (d->cap != 0 && d->hits >= d->cap) {
			/*
			 * Auto-disarm the RECORDING side only. DR7 cannot be
			 * touched from here, so the slot keeps trapping until
			 * the pipe thread rewrites it - which is exactly what
			 * s_dbpHwMask keeps us claiming it for.
			 */
			d->used = 0;
			s_dbpArmedMask &= ~(1u << i);
			s_dbpReapply = 1;
		}
	}

	ep->ContextRecord->Dr6 = 0;
	s_dbpInHandler = 0;
	return EXCEPTION_CONTINUE_EXECUTION;
}

static void dbp_disarm_all(void)
{
	char err[96];
	int  i;

	memset(s_dbp, 0, sizeof(s_dbp));
	s_dbpArmedMask = 0;
	for (i = 0; i < DBP_MAX; i++) {
		s_dbpHead[i] = 0;
		s_dbpTail[i] = 0;
		s_dbpDropped[i] = 0;
	}
	err[0] = 0;
	if (s_dbpThread)
		dbp_apply(err, sizeof(err));
}

/*
 * Runs from startup(), on the game thread. Duplicating the pseudo
 * handle here is what gets THREAD_GET_CONTEXT and THREAD_SET_CONTEXT -
 * hook_suspend_game's OpenThread(THREAD_SUSPEND_RESUME) is not enough
 * for the context calls.
 */
static void dbp_init(void)
{
	if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
	                     GetCurrentProcess(), &s_dbpThread, 0, FALSE,
	                     DUPLICATE_SAME_ACCESS)) {
		s_dbpThread = NULL;
		logf("  WARN: SET_DATABP off (DuplicateHandle failed)\n");
		return;
	}
	s_dbpVeh = AddVectoredExceptionHandler(1, dbp_veh);
	if (!s_dbpVeh)
		logf("  WARN: SET_DATABP off (AddVectoredExceptionHandler"
	         " failed)\n");
}

/* SET_DATABP add <hexaddr> [len=] [mode=] [cap=] [changed] [val=]
 *                          [val!=] [tag=] */
static void dbp_cmd_add(const char *args, char *out, size_t max)
{
	DBP_SLOT d;
	char     err[96];
	char     buf[AGENT_INPUT_MAX];
	char    *save = NULL;
	char    *tok;
	int      slot = -1;
	int      i;
	int      gotAddr = 0;
	unsigned int probe;

	memset(&d, 0, sizeof(d));
	d.len  = 1;
	d.mode = 0;
	d.cap  = DBP_CAP_DEF;

	_snprintf(buf, sizeof(buf), "%s", args);
	buf[sizeof(buf) - 1] = 0;

	for (tok = strtok_s(buf, " \t", &save); tok;
	     tok = strtok_s(NULL, " \t", &save)) {
		if (strncmp(tok, "len=", 4) == 0) {
			d.len = (unsigned int)strtoul(tok + 4, NULL, 0);
			if (d.len != 1 && d.len != 2 && d.len != 4) {
				_snprintf(out, max, "ERR SET_DATABP len must be"
				          " 1, 2 or 4 (8 is x64 only)\n");
				return;
			}
		} else if (strncmp(tok, "mode=", 5) == 0) {
			if (strcmp(tok + 5, "w") == 0)
				d.mode = 0;
			else if (strcmp(tok + 5, "rw") == 0)
				d.mode = 1;
			else {
				_snprintf(out, max, "ERR SET_DATABP mode is w or"
				          " rw (x86 has no read-only)\n");
				return;
			}
		} else if (strncmp(tok, "cap=", 4) == 0) {
			d.cap = (unsigned int)strtoul(tok + 4, NULL, 0);
			if (d.cap > DBP_CAP_MAX) {
				_snprintf(out, max, "ERR SET_DATABP cap must be"
				          " 0..%d\n", DBP_CAP_MAX);
				return;
			}
		} else if (strcmp(tok, "changed") == 0) {
			d.flags |= DBP_F_CHANGED;
		} else if (strncmp(tok, "val!=", 5) == 0) {
			d.flags |= DBP_F_VAL_NE;
			d.valImm = (unsigned int)strtoul(tok + 5, NULL, 16);
		} else if (strncmp(tok, "val=", 4) == 0) {
			d.flags |= DBP_F_VAL_EQ;
			d.valImm = (unsigned int)strtoul(tok + 4, NULL, 16);
		} else if (strncmp(tok, "tag=", 4) == 0) {
			_snprintf(d.tag, sizeof(d.tag), "%s", tok + 4);
			d.tag[sizeof(d.tag) - 1] = 0;
		} else if (!gotAddr) {
			char *end = NULL;

			if (tok[0] == '0' && (tok[1] == 'x' || tok[1] == 'X'))
				tok += 2;
			d.va = (unsigned int)strtoul(tok, &end, 16);
			if (!end || *end) {
				_snprintf(out, max, "ERR SET_DATABP bad"
				          " address\n");
				return;
			}
			gotAddr = 1;
		} else {
			_snprintf(out, max, "ERR SET_DATABP unknown token\n");
			return;
		}
	}

	if (!gotAddr) {
		_snprintf(out, max, "ERR SET_DATABP need an address\n");
		return;
	}
	if (d.va < AGENT_ADDR_MIN ||
	    (unsigned int)d.va + d.len - 1 > AGENT_ADDR_MAX) {
		_snprintf(out, max, "ERR SET_DATABP address out of range\n");
		return;
	}
	d.addr = rebase(d.va);
	/*
	 * NATURAL ALIGNMENT IS A HARDWARE RULE, not a preference: x86
	 * leaves an unaligned data breakpoint undefined. Refusing is the
	 * only honest answer, and it matters here because the racer tail
	 * is byte-misaligned throughout - len=1 works at any address.
	 */
	if (((unsigned int)(size_t)d.addr & (d.len - 1)) != 0) {
		_snprintf(out, max, "ERR SET_DATABP %08x is not aligned to"
		          " len=%u - use len=1\n", d.va, d.len);
		return;
	}
	if (!agent_copy(&probe, d.addr, d.len)) {
		_snprintf(out, max, "ERR SET_DATABP address faulted on a test"
		          " read\n");
		return;
	}
	if (!s_dbpThread || !s_dbpVeh) {
		_snprintf(out, max, "ERR SET_DATABP unavailable (init"
		          " failed)\n");
		return;
	}
	/*
	 * A debugger owns DR0-DR3 and re-applies its own set on every
	 * resume. Half working is worse than refusing outright: the
	 * caller could not tell "nothing wrote it" from "the debugger ate
	 * the breakpoint".
	 */
	if (IsDebuggerPresent()) {
		_snprintf(out, max, "ERR SET_DATABP refused: a debugger is"
		          " attached and owns DR0-DR3\n");
		return;
	}

	for (i = 0; i < DBP_MAX; i++) {
		if (!s_dbp[i].used) {
			slot = i;
			break;
		}
	}
	if (slot < 0) {
		_snprintf(out, max, "ERR SET_DATABP full (4 max, hardware"
		          " limit)\n");
		return;
	}

	d.used   = 1;
	d.shadow = dbp_read(d.addr, d.len);
	s_dbp[slot] = d;

	err[0] = 0;
	if (!dbp_apply(err, sizeof(err))) {
		memset(&s_dbp[slot], 0, sizeof(s_dbp[slot]));
		_snprintf(out, max, "ERR SET_DATABP arm failed: %s\n",
		          err[0] ? err : "unknown");
		return;
	}
	_snprintf(out, max, "OK SET_DATABP slot=%d addr=%08x len=%u mode=%s"
	          " cap=%u dr7=%08x init=%0*x\n",
	          slot, d.va, d.len, (d.mode == 1) ? "rw" : "w", d.cap,
	          s_dbpLiveDr7, (int)d.len * 2, d.shadow);
	out[max - 1] = 0;
}

static void dbp_cmd_set(const char *args, char *out, size_t max)
{
	while (*args == ' ' || *args == '\t')
		args++;

	dbp_flush_pending();

	if (strcmp(args, "clear") == 0) {
		dbp_disarm_all();
		_snprintf(out, max, "OK SET_DATABP clear\n");
		return;
	}
	if (strncmp(args, "add", 3) == 0 &&
	    (args[3] == ' ' || args[3] == '\t')) {
		dbp_cmd_add(args + 4, out, max);
		return;
	}
	_snprintf(out, max, "ERR SET_DATABP need clear | add <addr> ...\n");
}

/*
 * GET_DATABP reports the ARM STATE FIRST and then drains the hits.
 *
 * The state line is not padding. Windows sanitizes DR7 on the way in,
 * a debugger steals the registers, and a silently dropped arm is
 * indistinguishable from "nothing writes this" unless you look at what
 * the hardware actually holds - so it is read back OFF THE THREAD next
 * to what we asked for.
 */
static void dbp_cmd_get(char *out, size_t max)
{
	CONTEXT      ctx;
	unsigned int live[DBP_MAX];
	unsigned int dr7 = 0;
	size_t       at;
	int          got = 0;
	int          i;

	dbp_flush_pending();
	memset(live, 0, sizeof(live));

	/* Same self-suspend hazard as dbp_apply: degrade, never hang. */
	if (s_dbpThread && GetCurrentThreadId() != s_mainThreadId &&
	    SuspendThread(s_dbpThread) != (DWORD)-1) {
		memset(&ctx, 0, sizeof(ctx));
		ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
		if (GetThreadContext(s_dbpThread, &ctx)) {
			live[0] = (unsigned int)ctx.Dr0;
			live[1] = (unsigned int)ctx.Dr1;
			live[2] = (unsigned int)ctx.Dr2;
			live[3] = (unsigned int)ctx.Dr3;
			dr7     = (unsigned int)ctx.Dr7;
			got = 1;
		}
		ResumeThread(s_dbpThread);
	}

	at = (size_t)_snprintf(out, max,
	                       "OK GET_DATABP tid=%lu hw=%x armed=%x"
	                       " dr7_live=%08x dr7_want=%08x readback=%s",
	                       (unsigned long)s_mainThreadId,
	                       s_dbpHwMask, s_dbpArmedMask,
	                       got ? dr7 : 0u, dbp_build_dr7(),
	                       got ? "yes" : "NO");
	if (s_dbpDiagDone &&
	    (s_dbpDiagFlags & CONTEXT_DEBUG_REGISTERS)
	        != CONTEXT_DEBUG_REGISTERS)
	    at += (size_t)_snprintf(out + at, max - at,
	                            " DEGRADED-no-debug-regs dr6=%08x",
	                            s_dbpDiagDr6);

	for (i = 0; i < DBP_MAX; i++) {
		if (!s_dbp[i].used && live[i] == 0)
			continue;
		at += (size_t)_snprintf(out + at, max - at,
		                        " |s%d %s addr=%08x live=%08x len=%u"
		                        " %s hits=%u/%u",
		                        i, s_dbp[i].used ? "on " : "off",
		                        s_dbp[i].va, live[i], s_dbp[i].len,
		                        (s_dbp[i].mode == 1) ? "rw" : "w",
		                        s_dbp[i].hits, s_dbp[i].cap);
		if (s_dbp[i].tag[0])
			at += (size_t)_snprintf(out + at, max - at, " tag=%s",
		                            s_dbp[i].tag);
	}

	for (i = 0; i < DBP_MAX; i++) {
		while (s_dbpTail[i] != s_dbpHead[i] && at + 256 < max) {
			const DBP_HIT *h = &s_dbpStage[i][s_dbpTail[i]];

			at += (size_t)_snprintf(out + at, max - at,
			                        " |d%d#%u eip=%08x eax=%08x"
			                        " ecx=%08x edx=%08x ebx=%08x"
			                        " esp=%08x ebp=%08x esi=%08x"
			                        " edi=%08x old=%08x new=%08x",
			                        h->slot, h->hit, h->eip, h->eax,
			                        h->ecx, h->edx, h->ebx, h->esp,
			                        h->ebp, h->esi, h->edi,
			                        h->oldVal, h->newVal);
			_ReadWriteBarrier();
			s_dbpTail[i] = (s_dbpTail[i] + 1) % DBP_STAGE;
		}
		if (s_dbpDropped[i] && at + 96 < max) {
			at += (size_t)_snprintf(out + at, max - at,
			                        " |WARN d%d dropped %ld (that"
			                        " slot's staging full)", i,
			                        s_dbpDropped[i]);
			s_dbpDropped[i] = 0;
		}
	}

	{
		LONG more = 0;

		for (i = 0; i < DBP_MAX; i++)
			more += (s_dbpHead[i] - s_dbpTail[i] + DBP_STAGE)
		            % DBP_STAGE;
		if (more && at + 96 < max)
			at += (size_t)_snprintf(out + at, max - at,
		                            " |MORE %ld hit(s) not shown,"
		                            " call GET_DATABP again", more);
	}

	if (at + 2 < max) {
		out[at++] = '\n';
		out[at] = 0;
	} else {
		out[max - 2] = '\n';
		out[max - 1] = 0;
	}
}

/* ------------------------------------------------------------------ */
/* SEND_INPUT - driving the game from the agent                       */
/* ------------------------------------------------------------------ */

/*
 * THE GAME KEEPS A PLAYER INPUT WORD AND THE KEY HANDLER ONLY SETS
 * BITS IN IT, so nothing here has to fake a keyboard. Input_OnKeyDown
 * (0x00431250) writes &DAT_00584401 + racer * 0x3209, and this writes
 * the same word.
 *
 * That matters for more than convenience. The alternative was hooking
 * GetKeyState, which is process-wide: a stuck override would take the
 * real keyboard with it, and a crash mid-run would leave the bike
 * accelerating with no way to stop it from the keys.
 *
 * Bits, read out of the handler's switch [verified]:
 *
 *     0 accelerate   sets 1, and CLEARS bit 1
 *     1 brake        sets 2, and CLEARS bit 0
 *     2 turn left    sets 4, and CLEARS bit 3
 *     3 turn right   sets 8, and CLEARS bit 2
 *     4 higher gear  sets 0x10
 *     5 lower gear   sets 0x20
 *
 * o ACCEL/BRAKE AND LEFT/RIGHT ARE MUTUALLY EXCLUSIVE. The handler
 *   clears the opposite bit rather than letting both stand, so a mask
 *   with both is not a state the game can reach from a keyboard and
 *   is refused here rather than written.
 *
 * o TURBO IS NOT A BIT. Action 6 writes two separate dwords, at
 *   racer+0x31f9 and racer+0x3201. Out of scope; the mask is 0..5.
 */

#define ADDR_INPUT_WORD    0x00584401u  /* + racer * 0x3209           */
#define ADDR_RACER_STRIDE  0x3209u
#define ADDR_INPUT_RACER   0x004dc0fcu  /* index Input_OnKeyDown uses */
#define ADDR_INPUT_GATE    0x004e9834u  /* stage 2 runs only when 0   */
#define ADDR_PAUSE_STATE   0x004dc038u  /* the pause toggle's state   */
#define ADDR_DT_SMOOTHED   0x004dc150u  /* smoothed frame delta, ms   */
#define ADDR_PREV_STAMP    0x0050bbc8u  /* last sample, FUN_004120e0  */
#define ADDR_CLOCK_SAMPLE  0x00459fa0u  /* Clock_Sample               */
#define ADDR_CLOCK_MS      0x004e9cfcu  /* g_clockMs                  */
#define ADDR_FPS_MAX       0x004dc200u  /* g_frameRateMax             */
#define SI_DT_MS_DEFAULT   33u          /* 1000/30, the game's own    */
#define ADDR_FRAME_COUNT   0x004e981cu  /* ++ every rendered frame    */

#define SI_FRAME_MAX   0x12cu           /* 300, five seconds at 60fps */
#define SI_MASK        0x7fu            /* bits 0..6                  */

/*
 * BIT 6 IS THE STUNT BUTTON, and it is not a bit of the input word.
 *
 * Bits 0..5 go straight into racer+0x31e9, the word Input_OnKeyDown
 * fills for actions 0..5. ACTION 6 IS DIFFERENT: its case at
 * 0x00431250 does not touch the word at all - it writes stuntHeld
 * (+0x31f9) and stuntPressed (+0x3201) directly, and the key-up
 * handler at 0x00432700 clears stuntHeld alone. So the harness
 * reproduces the two writes rather than setting a bit.
 *
 * stuntHeld IS A LEVEL and stuntPressed IS AN EDGE. The press flag
 * is set only on the 0 -> 1 transition and is consumed elsewhere,
 * so writing it every poll would be a key pressed eight times a
 * frame. It is written only when stuntHeld was found clear.
 *
 * NOT action6Down (+0x31fd). That field is the JOYSTICK path -
 * maybe_Input_ReadDevice at 0x00411b30 derives the same two fields
 * from it - and it is called only for device types 2 and 4. On a
 * keyboard nothing reads or writes it, so an earlier version of
 * this that drove action6Down moved the wheelie not at all: a
 * watchpoint on it recorded zero writes across a run.
 */
#define SI_STUNT       0x40u
#define SI_HELD_OFF    0x10u            /* 0x31f9 - 0x31e9            */
#define SI_PRESS_OFF   0x18u            /* 0x3201 - 0x31e9            */
#define SI_PAUSE_SETTLE  60u          /* ms to let the in-flight
                                       * frame finish, holding   */
#define SI_PAUSE_WAIT  1000u            /* ms to see the toggle land  */
#define SI_RUN_GUARD   20000u           /* ms before giving up on it  */

static HWND s_gameWnd;

static BOOL CALLBACK si_pick_window(HWND wnd, LPARAM param)
{
	(void)param;
	if (IsWindowVisible(wnd)) {
		s_gameWnd = wnd;
		return FALSE;
	}
	return TRUE;
}

/*
 * The game's own window, found by walking the thread DllMain ran on.
 * Not FindWindow by title - the title changes with the build, and a
 * wrong match would post keys into somebody else's application.
 */
static HWND si_window(void)
{
	if (!s_gameWnd && s_mainThreadId)
		EnumThreadWindows(s_mainThreadId, si_pick_window, 0);
	return s_gameWnd;
}

static int si_read(unsigned int va, unsigned int *out)
{
	return agent_copy(out, rebase(va), 4);
}

static int si_write(unsigned int va, unsigned int val)
{
	__try {
		*(unsigned int *)rebase(va) = val;
		return 1;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return 0;
	}
}

/*
 * Write the driving bits into the input word, and drive the stunt
 * button the way Input_OnKeyDown/OnKeyUp do - level in stuntHeld,
 * edge in stuntPressed.
 */
static void si_write_input(unsigned int va, unsigned int hold)
{
	unsigned int held = 0;

	si_write(va, hold & (SI_MASK & ~SI_STUNT));
	if (hold & SI_STUNT) {
		if (si_read(va + SI_HELD_OFF, &held) && held == 0)
			si_write(va + SI_PRESS_OFF, 1u);
		si_write(va + SI_HELD_OFF, 1u);
	} else {
		si_write(va + SI_HELD_OFF, 0u);
	}
}


static unsigned int si_word_va(void)
{
	unsigned int idx = 0;

	if (!si_read(ADDR_INPUT_RACER, &idx) || idx > 32)
		idx = 0;
	return ADDR_INPUT_WORD + idx * ADDR_RACER_STRIDE;
}

static int si_paused(void)
{
	unsigned int v = 0;

	si_read(ADDR_PAUSE_STATE, &v);
	return v != 0;
}

static void si_key(unsigned int vk, int down)
{
	HWND w = si_window();

	if (!w)
		return;
	if (down)
		PostMessageA(w, WM_KEYDOWN, (WPARAM)vk, 0x00000001);
	else
		PostMessageA(w, WM_KEYUP, (WPARAM)vk, 0xc0000001);
}

/*
 * A tap, optionally with one modifier: "74" or "11+56".
 *
 * ORDER MATTERS AND IS NOT DECORATION. g_shiftDown and g_ctrlDown are
 * LATCHES - Input_OnKeyDown sets them, maybe_Input_OnKeyUp clears
 * them - so the modifier goes down first and comes up last. Get it
 * wrong and the latch stays stuck, after which every bare key the
 * player presses is read as a CTRL combination.
 */
static void si_tap(const char *text)
{
	char        *end;
	unsigned int a = strtoul(text, &end, 16);
	unsigned int b;

	if (*end == '+') {
		b = strtoul(end + 1, NULL, 16);
		si_key(a, 1);
		si_key(b, 1);
		si_key(b, 0);
		si_key(a, 0);
	} else {
		si_key(a, 1);
		si_key(a, 0);
	}
}

/*
 * PAUSE BY WRITING THE FLAG, not by posting the key.
 *
 * 0x004dc038 is what the game itself reads to decide whether to
 * advance, and writing it stops and starts the simulation on the
 * instruction - measured at 36 frames a second running against 4
 * paused. Posting VK_PAUSE instead costs a message round trip plus
 * a 5 ms poll before the state settles, and every one of those
 * frames runs with whatever the input word happens to hold. That
 * latency is what put a throttle blip at the head of every burst.
 *
 * CTRL+P STILL WORKS. This writes the same flag the key handler
 * toggles, so a human can pause and unpause normally afterwards -
 * nothing here latches anything or takes the game off its own
 * path. Whatever else the handler does - the overlay, the engine
 * sound - is skipped, which is cosmetic for a measurement run.
 *
 * THE KEY REMAINS AS A FALLBACK. If the write does not take, the
 * old path runs, so this can only be faster and never worse.
 */
/*
 * THE VIRTUAL CLOCK - game time as a function of the frame count.
 *
 * Every frame delta in the game descends from one global, g_clockMs
 * at 0x004e9cfc, written by Clock_Sample @ 0x00459fa0 from
 * timeGetTime. Three call sites read it, all of the form
 * Clock_Sample() then Clock_Now(); the frame limiter is NOT among
 * them - it calls timeGetTime directly - so replacing Clock_Sample
 * changes what the simulation believes about time without touching
 * pacing.
 *
 *     g_clockMs += vclock_step_ms() for every frame the
 *                 counter advances, and never goes backwards
 *
 * It is seeded at engagement so the clock does not jump.
 *
 * WHY THIS AND NOT A CORRECTION AT THE PAUSE BOUNDARY. The boundary
 * fix worked - it took the lean loss on the first resumed frame from
 * 63 per cent down to 0.3 - but it did not survive repetition:
 * twelve one-frame bursts drifted 22 per cent away from one
 * twelve-frame burst, because g_framePrevMs has three writers and
 * the last one before the resumed frame is the tail of
 * FUN_00410ee0, which sets it to now. Correcting one writer at one
 * moment is a race against the other two.
 *
 * Frame-locking the clock removes the question. Stepped and
 * continuous runs see an identical time history by construction,
 * whatever order those three writers run in, because none of them
 * can observe real time any more. It also stops the race clock
 * advancing while the game sits paused.
 *
 * A PAUSE COSTS NOTHING because frameCount does not advance while
 * the game is frozen, so the delta across a resume is exactly one
 * frame - the same as any other frame.
 */
static int          s_vclockOn;
static unsigned int s_vclockMs;
static unsigned int s_vclockFrames;
static unsigned int s_vclockStepMs = SI_DT_MS_DEFAULT;

/*
 * THE STEP FOLLOWS -FrameRateMax, AND IT USED TO BE A HARDCODED
 * 33 - 'one frame at the original', which is only true at the
 * game's default 30.
 *
 * AT -FrameRateMax60 THAT RAN THE GAME AT DOUBLE SPEED. The
 * renderer ticks twice as often and the clock still advanced 33
 * a frame, so game time moved at 2x real time and the physics
 * stepped 33 ms where an unhooked game steps 16. Both are
 * visible from outside: the bike covers ground twice as fast,
 * and g_frameDeltaScaled reads 3.3 against the 1.6 the same
 * game reports free-running.
 *
 * IT ALSO POISONED EVERY A/B. mracer steps its own cap period,
 * so a driven comparison at 60 fps was 33 ms against 17 and
 * every drivetrain quantity came back about 1.97x apart - which
 * reads exactly like a port defect. A long session went into
 * chasing that ratio through the torque curve, the carac table
 * and the gain block before the clock turned out to be the
 * cause. See docs/subsystems/frame-loop.txt 1.4.
 *
 * THE GAME'S OWN LIMITER SPENDS AN INTEGER 1000/n, so 30 buys
 * 33 ms and 60 buys 16 - and 16 is exactly what a free-running
 * 60 fps game reports. Matching that arithmetic rather than
 * rounding to 16.67 keeps the clock honest against the GAME
 * rather than against the wall.
 *
 * 30 STILL YIELDS 33, so every measurement taken before this
 * and every 33 ms case in tools/check_physics.py stands.
 */
static unsigned int vclock_step_ms(void)
{
	unsigned int fps = *(unsigned int *)rebase(ADDR_FPS_MAX);

	/*
	 * 0 IS 'NO LIMIT' and has no frame time to speak of, so
	 * there is nothing honest to derive - the game's own
	 * default is the least surprising fallback. The upper
	 * guard is against a garbage read, not against a real
	 * setting; the parser only stores 20 < n < 99.
	 */
	if (fps == 0u || fps > 1000u)
		return SI_DT_MS_DEFAULT;
	return 1000u / fps;
}

/* The same number as a float's bits, for g_frameDeltaSmoothed. */
static unsigned int vclock_step_bits(void)
{
	float        f = (float)s_vclockStepMs;
	unsigned int bits;

	memcpy(&bits, &f, sizeof bits);
	return bits;
}

static void vclock_sample(void)
{
	unsigned int frames = *(unsigned int *)rebase(ADDR_FRAME_COUNT);

	if (frames != s_vclockFrames) {
		unsigned int step = frames - s_vclockFrames;

		/*
		 * THE FRAME COUNTER IS NOT MONOTONIC. A race restart puts
		 * it back to zero, and a clock that follows it backwards
		 * hands the game a NEGATIVE frame delta - which the lean
		 * then divides by.
		 *
		 * Measured before this guard: g_frameDeltaSmoothed read
		 * -159.534 on the first frame after mr_reset and took some
		 * fifty frames to climb back to 33, so every run started
		 * from a fresh reset began inside a corrupted window.
		 *
		 * Accumulate instead of deriving, and treat any backward or
		 * absurd jump as a single frame.
		 */
		if (frames < s_vclockFrames || step > SI_FRAME_MAX)
			step = 1;

		s_vclockMs += step * s_vclockStepMs;
		s_vclockFrames = frames;
	}

	*(unsigned int *)rebase(ADDR_CLOCK_MS) = s_vclockMs;
}

static int vclock_engage(void)
{
	unsigned char  jmp[5];
	unsigned char *p;
	unsigned int   frames;
	unsigned int   clk;
	HANDLE         thread;
	int            ok;

	if (s_vclockOn)
		return 1;

	frames = *(unsigned int *)rebase(ADDR_FRAME_COUNT);
	clk    = *(unsigned int *)rebase(ADDR_CLOCK_MS);
	s_vclockFrames = frames;
	s_vclockMs     = clk;      /* no jump at engagement */

	/*
	 * READ THE RATE ONCE, HERE. -FrameRateMax is parsed at
	 * startup and nothing moves it afterwards, and engagement
	 * is the first moment the harness drives - so this is both
	 * late enough to see the real value and early enough that
	 * no frame has been stepped with the wrong one.
	 */
	s_vclockStepMs = vclock_step_ms();

	p = (unsigned char *)rebase(ADDR_CLOCK_SAMPLE);
	jmp[0] = 0xe9;
	*(int *)(jmp + 1) = (int)((unsigned char *)vclock_sample - (p + 5));

	thread = hook_suspend_game();
	ok = hook_patch(p, jmp, 5);
	hook_resume_game(thread);

	s_vclockOn = ok;
	return ok;
}

/*
 * Engaged the first time the harness drives, and seeded once so the
 * smoothed delta starts where a running game would have it rather
 * than wherever the last real-time frame left it.
 */
static void si_bridge_clock(void)
{
	if (!s_vclockOn && vclock_engage())
		si_write(ADDR_DT_SMOOTHED, vclock_step_bits());
}
static int si_set_pause(int want)
{
	unsigned int waited = 0;

	if (si_paused() == want)
		return 1;

	/* Still frozen at this point - see si_bridge_clock. */
	if (!want)
		si_bridge_clock();

	if (si_write(ADDR_PAUSE_STATE, want ? 1u : 0u)
	    && si_paused() == want) {
		return 1;
	}

	/* The flag would not take - go through the handler after all. */
	si_tap("13");
	while (waited < SI_PAUSE_WAIT) {
		Sleep(5);
		waited += 5;
		if (si_paused() == want)
			return 1;
	}
	return 0;
}

/*
 * Hold the mask for N frames, counted off the game's own frame
 * counter - maybe_Game_RenderFrame increments it, so no hook is
 * needed for timing.
 *
 * The word is REWRITTEN every poll rather than set once, because
 * nothing here knows whether the game clears it after consuming it.
 * Writing repeatedly is correct either way; writing once is only
 * correct in one of the two cases.
 */
static int si_run(unsigned int frames, unsigned int hold,
                  unsigned int *ran)
{
	unsigned int wordVa = si_word_va();
	unsigned int start = 0;
	unsigned int now = 0;
	unsigned int guard = 0;
	unsigned int settle;
	/*
	 * THE FRAME COUNT IS THE TARGET, MEASURED NOT REASONED. Writing
	 * the pause flag stops the game where it is - there is no
	 * in-flight frame that still lands, which is what arming a frame
	 * early assumed. With target = frames - 1 a run of 20 advanced
	 * 19 and a run of 5 advanced 4; with target = 0 for a
	 * single-frame run it advanced nothing at all and the counter
	 * sat at 35 across four bursts.
	 *
	 * advanced == target for every request of TWO OR MORE.
	 *
	 * A REQUEST OF ONE ADVANCES TWO, and cannot be made to do
	 * otherwise from here: the game finishes the frame it was
	 * frozen in and starts the next before the pause flag lands,
	 * so two frames is the minimum a resume can cost. Measured
	 * across 1,2,3,5,10,20,40 - only the request of one is over,
	 * and it reports ran=2 honestly.
	 *
	 * SO DO NOT BUILD A COMPARISON OUT OF ONE-FRAME BURSTS. Twelve
	 * of them advance 24 frames, which reads as a physics
	 * divergence and is not one.
	 */
	unsigned int target = frames;

	*ran = 0;
	si_read(ADDR_FRAME_COUNT, &start);

	/*
	 * ARM THE PAUSE ONE FRAME EARLY - the fence post goes INSIDE the
	 * run. Writing the flag stops the NEXT frame, not the one in
	 * flight, so arming at frames-1 lands the stop on frames.
	 */
	/*
	 * NOTHING PINS THE FRAME DELTA HERE. si_set_pause corrects the
	 * clock at the resume boundary instead, where the game is
	 * frozen and there is no race to lose.
	 */
	while (target > 0) {
		si_write_input(wordVa, hold);
		Sleep(2);
		guard += 2;
		if (!si_read(ADDR_FRAME_COUNT, &now))
			break;
		*ran = now - start;
		if (*ran >= target)
			break;
		if (guard > SI_RUN_GUARD)
			break;
	}

	si_write(ADDR_PAUSE_STATE, 1u);

	/*
	 * AND KEEP HOLDING WHILE IT LANDS. The game thread finishes the
	 * frame it is in; across that frame nothing else is rewriting
	 * the word, and the game rebuilds it from the real keyboard, so
	 * letting go here is what put a released frame at the end of
	 * every burst. In a circle it shows as the bike standing up and
	 * lying back down between segments.
	 *
	 * The pause is already set, so this cannot overrun: no frame
	 * starts after it, and the one in flight finishes with the input
	 * still held.
	 */
	for (settle = 0; settle < SI_PAUSE_SETTLE; settle += 2) {
		si_write_input(wordVa, hold);
		Sleep(2);
	}

	if (si_read(ADDR_FRAME_COUNT, &now))
		*ran = now - start;
	return *ran >= frames || frames <= 1;
}

/* SEND_INPUT <hold> <frames> [tap ...] */
static void agent_send_input(char *args, char *out, size_t max)
{
	char        *end;
	unsigned int hold;
	unsigned int frames;
	unsigned int gate = 0;
	unsigned int ran = 0;
	int          complete;

	hold = strtoul(args, &end, 16);
	if (end == args) {
		_snprintf(out, max, "ERR SEND_INPUT bad hold mask\n");
		return;
	}
	if (hold & ~SI_MASK) {
		_snprintf(out, max, "ERR SEND_INPUT mask %08x has bits outside "
		          "0..6; turbo is not a bit\n", hold);
		return;
	}
	/*
	 * NO COMBINATION IS REFUSED. There used to be two guards here -
	 * accelerate with brake, and left with right - on the reasoning
	 * that a keyboard cannot produce them and the game would not
	 * expect them.
	 *
	 * BOTH WERE WRONG, AND THE FIRST HID A REAL BUG. Gas and brake
	 * and a direction held together is how a dirt bike does endless
	 * donuts in the retail game - a genuine manoeuvre with its own
	 * code path, the turnBrakeFlag gate at 0x0048f915 - and the
	 * guard made that path impossible to drive from here, so the
	 * port's version of it could not be compared against the
	 * original at all.
	 *
	 * A HARNESS THAT REFUSES AN INPUT THE GAME ACCEPTS IS NOT A
	 * SAFETY FEATURE. The mask is already bounded to bits 0..6
	 * above; what the game does with a combination is the game's
	 * business and is what we are here to find out.
	 */

	args = end;
	frames = strtoul(args, &end, 16);
	if (end == args || frames < 1 || frames > SI_FRAME_MAX) {
		_snprintf(out, max, "ERR SEND_INPUT frames must be 1..%x\n",
		          SI_FRAME_MAX);
		return;
	}

	if (!si_window()) {
		_snprintf(out, max, "ERR SEND_INPUT no game window found\n");
		return;
	}
	si_read(ADDR_INPUT_GATE, &gate);
	if (gate != 0) {
		_snprintf(out, max, "ERR SEND_INPUT driving input is gated off "
		          "(DAT_004e9834=%u)\n", gate);
		return;
	}

	if (!si_set_pause(1)) {
		_snprintf(out, max, "ERR SEND_INPUT could not pause - solo mode "
		          "only, and the menu must be down\n");
		return;
	}
	/*
	 * SET THE INPUT BEFORE LETTING THE GAME GO.
	 *
	 * si_run does not write the word until its first poll, so
	 * unpausing first leaves the game running with NO BUTTONS for
	 * however long the unpause and the taps take - a message round
	 * trip plus a 5 ms poll. On the throttle that reads as a blip
	 * at the start of every burst, and the tail of the bike bounces
	 * as it would if someone had feathered the gas.
	 *
	 * This is the same bug as the trailing release in si_run, at
	 * the other end of the run: the input has to be in place across
	 * BOTH pause boundaries, not just inside them.
	 */
	si_write_input(si_word_va(), hold);

	if (!si_set_pause(0)) {
		_snprintf(out, max, "ERR SEND_INPUT could not unpause\n");
		si_write_input(si_word_va(), 0);
		return;
	}

	/* Taps go in once the game is running again. */
	for (args = end; *args; ) {
		while (*args == ' ')
			args++;
		if (!*args)
			break;
		si_tap(args);
		while (*args && *args != ' ')
			args++;
	}

	complete = si_run(frames, hold, &ran);

	/* si_run armed the pause and held the word across it landing. */
	si_set_pause(1);
	si_write_input(si_word_va(), 0);

	_snprintf(out, max,
	          "%s SEND_INPUT held=%08x frames=%x ran=%x paused=%d\n",
	          complete ? "OK" : "ERR", hold, frames, ran, si_paused());
}

/* ------------------------------------------------------------------ */
/* SET_MEM - the one command that writes                              */
/* ------------------------------------------------------------------ */

/*
 * EVERYTHING ELSE IN THIS FILE READS. This one writes, so it is
 * built to be reversible and to refuse the cases that end a session.
 *
 * o IT ECHOES WHAT IT CLOBBERED. "was=" is read immediately before
 *   the write, so the undo is in the reply and no experiment has to
 *   have remembered to snapshot first.
 *
 * o IT REPORTS REALITY, NOT INTENT. "now=" is re-read AFTER the
 *   write. A write that did not land - a read-only page, or a value
 *   the game recomputes in the same instant - shows up as a "now"
 *   that is not what was asked for, rather than a false success.
 *
 * o IT REFUSES .text UNLESS YOU ASK FOR IT. Struct work never needs
 *   to write code and a mistyped address does the most damage there,
 *   so the refusal is the default. CODE lifts it, routes the write
 *   through hook_patch() - VirtualProtect, memcpy, protect back,
 *   FlushInstructionCache - and suspends the game thread across it,
 *   which is how SET_HOOK has always patched.
 *
 *   THERE IS NO WATCHED-BYTE REFUSAL, DELIBERATELY. 0x00442761-2 and
 *   0x004b1830..0x004b18b6 are read by the copy protection as DATA;
 *   altering them leaves practice and single races working and makes
 *   championship mode unfinishable hours later, in a different
 *   subsystem. CODE will write them. See CLAUDE.md.
 *
 * o 64 BYTES MAX, against GET_MEM's 4096. A write should be
 *   surgical; 64 is exactly a 4x4 matrix and far too small to smear
 *   a struct by accident.
 *
 * ALIGNMENT IS DELIBERATELY NOT ENFORCED. The racer's tail fields
 * sit at 4k+1 - +0x2e1d and +0x2e39 among them - so an alignment
 * check would refuse precisely the fields this exists for. x86
 * writes unaligned without complaint.
 */

#define AGENT_SET_MEM_MAX  64
#define AGENT_TEXT_LO      0x00401000u
#define AGENT_TEXT_HI      0x004cf5ffu

static int agent_poke(void *dst, const void *src, unsigned int n)
{
	__try {
		memcpy(dst, src, n);
		return 1;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return 0;
	}
}

static size_t agent_hex(char *out, size_t at, size_t max,
                        const unsigned char *b, unsigned int n)
{
	static const char hex[] = "0123456789abcdef";
	unsigned int i;

	for (i = 0; i < n && at + 3 < max; i++) {
		out[at++] = hex[(b[i] >> 4) & 0xf];
		out[at++] = hex[b[i] & 0xf];
	}
	return at;
}

/*
 * REWRITE THE VALUE EVERY POLL FOR N FRAMES, counted off the game's
 * own frame counter.
 *
 * WITHOUT THIS THE COMMAND CANNOT TEST A PHYSICS FIELD AT ALL.
 * maybe_Racer_UpdateLean recomputes racer+0x2e1d every frame, so a
 * one-shot write is gone before anything draws it. This is the same
 * trick si_run uses to defeat the key handler clearing the input
 * word, reused rather than reinvented.
 */
static int agent_hold(unsigned int phys, const unsigned char *buf,
                      unsigned int size, unsigned int frames,
                      unsigned int *ran)
{
	unsigned int start = 0;
	unsigned int now = 0;
	unsigned int guard = 0;

	*ran = 0;
	si_read(ADDR_FRAME_COUNT, &start);
	for (;;) {
		if (!agent_poke((void *)phys, buf, size))
			return 0;
		Sleep(2);
		guard += 2;
		if (!si_read(ADDR_FRAME_COUNT, &now))
			break;
		*ran = now - start;
		if (*ran >= frames)
			break;
		if (guard > SI_RUN_GUARD)
			break;
	}
	return *ran >= frames;
}

/* SET_MEM <hexaddr> <hexbytes> [CODE] [HOLD <frames>] */
static void agent_set_mem(const char *args, char *out, size_t max)
{
	unsigned char  buf[AGENT_SET_MEM_MAX];
	unsigned char  was[AGENT_SET_MEM_MAX];
	unsigned char  now[AGENT_SET_MEM_MAX];
	char          *end;
	const char    *p;
	unsigned long  addr;
	unsigned long  frames = 0;
	unsigned int   phys;
	unsigned int   size;
	unsigned int   digits;
	unsigned int   i;
	unsigned int   ran = 0;
	int            held = 0;
	int            code = 0;
	size_t         at;

	while (*args == ' ' || *args == '\t')
		args++;
	if (args[0] == '0' && (args[1] == 'x' || args[1] == 'X'))
		args += 2;

	addr = strtoul(args, &end, 16);
	if (end == args) {
		_snprintf(out, max, "ERR SET_MEM bad address\n");
		out[max - 1] = 0;
		return;
	}

	p = end;
	while (*p == ' ' || *p == '\t')
		p++;

	/*
	 * THE VALUE IS RAW LITTLE-ENDIAN HEX, exactly the encoding
	 * GET_MEM returns, so a read pastes straight back as a write.
	 * Typing lives in the Python bridge, which keeps float packing
	 * out of this side entirely.
	 */
	digits = 0;
	while (p[digits] && p[digits] != ' ' && p[digits] != '\t')
		digits++;
	if (digits == 0 || (digits & 1) || digits > AGENT_SET_MEM_MAX * 2) {
		_snprintf(out, max, "ERR SET_MEM bad value, whole bytes, "
		          "1..%d\n", AGENT_SET_MEM_MAX);
		out[max - 1] = 0;
		return;
	}
	size = digits / 2;
	for (i = 0; i < digits; i++) {
		char         c = p[i];
		unsigned int v;

		if (c >= '0' && c <= '9')
			v = (unsigned int)(c - '0');
		else if (c >= 'a' && c <= 'f')
			v = (unsigned int)(c - 'a') + 10;
		else if (c >= 'A' && c <= 'F')
			v = (unsigned int)(c - 'A') + 10;
		else {
			_snprintf(out, max, "ERR SET_MEM value is not hex\n");
			out[max - 1] = 0;
			return;
		}
		if ((i & 1) == 0)
			buf[i / 2] = (unsigned char)(v << 4);
		else
			buf[i / 2] |= (unsigned char)v;
	}

	p += digits;

	/*
	 * THE TRAILING KEYWORDS, in any order. Both are bare and
	 * uppercase like the rest of this protocol.
	 */
	for (;;) {
		while (*p == ' ' || *p == '\t')
			p++;

		if (strncmp(p, "CODE", 4) == 0) {
			code = 1;
			p += 4;
			continue;
		}
		if (strncmp(p, "HOLD", 4) == 0) {
			held = 1;
			frames = strtoul(p + 4, &end, 16);
			if (end == p + 4 || frames < 1 ||
			    frames > SI_FRAME_MAX) {
				_snprintf(out, max,
				          "ERR SET_MEM frames must be 1..%x\n",
				          SI_FRAME_MAX);
				out[max - 1] = 0;
				return;
			}
			p = end;
			continue;
		}
		break;
	}

	/*
	 * A CODE PATCH PERSISTS UNTIL SOMETHING WRITES IT BACK, so
	 * re-patching it every 2 ms buys nothing and adds a pause
	 * dance plus a window where the bytes are half written.
	 */
	if (code && held) {
		_snprintf(out, max, "ERR SET_MEM CODE and HOLD are exclusive"
		          " - a code patch persists until it is written back\n");
		out[max - 1] = 0;
		return;
	}

	/*
	 * .text IS CHECKED AGAINST THE ADDRESS AS TYPED, not the
	 * rebased one, because the section bounds in docs/ and in
	 * Ghidra are preferred-base. It is an OVERLAP test, not
	 * containment - a run that merely clips the end of .text is
	 * still a write into code.
	 */
	if (!code && addr <= AGENT_TEXT_HI &&
	    (unsigned int)addr + size - 1 >= AGENT_TEXT_LO) {
		_snprintf(out, max, "ERR SET_MEM refuses to write code "
		          "(.text %08x..%08x) - add CODE to mean it, and "
		          "do not touch the watched protection bytes at "
		          "00442761 or 004b1830..004b18b6\n",
		          AGENT_TEXT_LO, AGENT_TEXT_HI);
		out[max - 1] = 0;
		return;
	}

	phys = (unsigned int)addr + s_delta;
	if (phys < AGENT_ADDR_MIN || phys > AGENT_ADDR_MAX ||
	    phys + size - 1 > AGENT_ADDR_MAX || phys + size - 1 < phys) {
		_snprintf(out, max, "ERR SET_MEM address out of range\n");
		out[max - 1] = 0;
		return;
	}

	if (!agent_copy(was, (const void *)phys, size)) {
		_snprintf(out, max, "ERR SET_MEM read faulted at %08x\n", phys);
		out[max - 1] = 0;
		return;
	}

	if (held) {
		/*
		 * FRAMES ONLY ADVANCE WHILE THE GAME RUNS, so this does the
		 * same pause dance SEND_INPUT does: prove it is pausable,
		 * unpause, hold, re-pause. A bare SET_MEM never touches the
		 * pause state.
		 */
		if (!si_window()) {
			_snprintf(out, max, "ERR SET_MEM no game window found\n");
			out[max - 1] = 0;
			return;
		}
		if (!si_set_pause(1) || !si_set_pause(0)) {
			_snprintf(out, max, "ERR SET_MEM could not pause - solo "
			          "mode only, and the menu must be down\n");
			out[max - 1] = 0;
			return;
		}
		agent_hold(phys, buf, size, (unsigned int)frames, &ran);
		si_set_pause(1);
	} else if (code) {
		/*
		 * SUSPEND THE GAME THREAD ACROSS IT, the way SET_HOOK
		 * does. This runs on the pipe thread, so without it the
		 * patch can land in the middle of an instruction the
		 * game is executing.
		 */
		HANDLE thread = hook_suspend_game();
		int    ok = hook_patch((unsigned char *)phys, buf, size);

		hook_resume_game(thread);
		if (!ok) {
			_snprintf(out, max,
			          "ERR SET_MEM could not unprotect %08x\n",
			          phys);
			out[max - 1] = 0;
			return;
		}
	} else if (!agent_poke((void *)phys, buf, size)) {
		_snprintf(out, max, "ERR SET_MEM write faulted at %08x\n", phys);
		out[max - 1] = 0;
		return;
	}

	if (!agent_copy(now, (const void *)phys, size))
		memset(now, 0, size);

	at = (size_t)_snprintf(out, max, "OK SET_MEM addr=%08x size=%u was=",
	                       (unsigned int)addr, size);
	at = agent_hex(out, at, max, was, size);
	at += (size_t)_snprintf(out + at, max - at, " now=");
	at = agent_hex(out, at, max, now, size);
	if (code)
		at += (size_t)_snprintf(out + at, max - at, " code=1");
	if (held)
		at += (size_t)_snprintf(out + at, max - at,
	                            " frames=%x ran=%x paused=%d",
	                            (unsigned int)frames, ran, si_paused());
	if (at + 2 < max) {
		out[at++] = '\n';
		out[at] = 0;
	}
	out[max - 1] = 0;
}

/*
 * THE WATCHED COPY-PROTECTION BYTES, AND WHY THIS VERB REFUSES THEM
 * WHERE SET_MEM DOES NOT.
 *
 * SET_MEM writes them as DATA and deliberately allows it - the
 * protection reads its own code as data and a test may need to change
 * what it sees. A five-byte JMP is different in kind: it would destroy
 * the check outright, and the symptom is not a crash, it is a
 * championship that quietly becomes unfinishable hours later. Two
 * bytes at 00442761 and the eight-entry table over 004b1830..004b18b6.
 * See docs/subsystems/copy-protection.txt 5.1-5.2.
 */
static int fh_hits_watched(unsigned int addr)
{
	unsigned int lo = addr;
	unsigned int hi = addr + FUNC_JMP_LEN - 1;

	if (lo <= 0x00442762u && hi >= 0x00442761u)
		return 1;
	if (lo <= 0x004b18b6u && hi >= 0x004b1830u)
		return 1;
	return 0;
}

static FUNC_HOOK *fh_find(unsigned int addr)
{
	int i;

	for (i = 0; i < FUNC_HOOK_MAX; i++)
		if (s_funcHook[i].active && s_funcHook[i].addr == addr)
			return &s_funcHook[i];
	return NULL;
}

/*
 * LOAD_MOD <filename>
 *
 * IT MUST NOT RUN IN DllMain. LoadLibrary from under the loader lock
 * deadlocks or worse, which is why this is a command rather than
 * something startup() does - commands arrive on agent_thread, so the
 * timing is right for free.
 *
 * THE NAME IS A BARE FILENAME, resolved against MOTO own directory.
 * A path separator is refused so this cannot be talked into loading
 * something from elsewhere on the machine.
 */
static void fh_cmd_load(const char *args, char *out, unsigned int max)
{
	char  path[MAX_PATH];
	char  dir[MAX_PATH];
	char  name[64];
	char *slash;
	unsigned int i = 0;

	while (*args == 32 || *args == 9)
		args++;
	while (args[i] && args[i] != 32 && args[i] != 9 &&
	       i < sizeof(name) - 1) {
		name[i] = args[i];
		i++;
	}
	name[i] = 0;

	if (name[0] == 0) {
		_snprintf(out, max, "ERR LOAD_MOD needs a filename\n");
		out[max - 1] = 0;
		return;
	}
	if (strchr(name, 92) || strchr(name, 47) || strchr(name, 58)) {
		_snprintf(out, max, "ERR LOAD_MOD wants a bare filename beside "
		          "MOTO.EXE, not a path\n");
		out[max - 1] = 0;
		return;
	}

	GetModuleFileNameA(NULL, dir, MAX_PATH);
	slash = strrchr(dir, 92);
	if (slash)
		slash[1] = 0;
	else
		dir[0] = 0;

	strcpy(path, dir);
	strcat(path, name);

	if (s_mod) {
		_snprintf(out, max, "ERR LOAD_MOD %s is already loaded - "
		          "restart MOTO to swap it\n", s_modName);
		out[max - 1] = 0;
		return;
	}

	s_mod = LoadLibraryA(path);
	if (!s_mod) {
		_snprintf(out, max, "ERR LOAD_MOD %s did not load (%lu)\n",
		          path, (unsigned long)GetLastError());
		out[max - 1] = 0;
		return;
	}
	strcpy(s_modName, name);

	_snprintf(out, max, "OK LOAD_MOD name=%s base=%08x\n",
	          s_modName, (unsigned int)(ULONG_PTR)s_mod);
	out[max - 1] = 0;
}

/*
 * HOOK_FUNC <hexaddr> <export> <0|1>
 *
 * on=1  save the five entry bytes and write E9 rel32 to the export;
 * on=0  put the five bytes back.
 *
 * THE GAME THREAD IS SUSPENDED ACROSS BOTH. write_jump gets away
 * without it because it runs before the entry point; this does not.
 */
static void fh_cmd_hook(const char *args, char *out, unsigned int max)
{
	char           name[64];
	unsigned char *p;
	unsigned char  patch[FUNC_JMP_LEN];
	unsigned int   addr;
	unsigned int   i = 0;
	int            on;
	int            rel;
	FUNC_HOOK     *fh;
	HANDLE         thread;
	char          *end;

	addr = (unsigned int)strtoul(args, &end, 16);
	if (end == args) {
		_snprintf(out, max, "ERR HOOK_FUNC needs <hexaddr> <export> "
		          "<0|1>\n");
		out[max - 1] = 0;
		return;
	}
	while (*end == 32 || *end == 9)
		end++;
	while (end[i] && end[i] != 32 && end[i] != 9 &&
	       i < sizeof(name) - 1) {
		name[i] = end[i];
		i++;
	}
	name[i] = 0;
	on = (int)strtoul(end + i, NULL, 10);

	if (name[0] == 0) {
		_snprintf(out, max, "ERR HOOK_FUNC needs an export name\n");
		out[max - 1] = 0;
		return;
	}

	/* --- off: restore, and nothing else matters --- */
	if (!on) {
		fh = fh_find(addr);
		if (!fh) {
			_snprintf(out, max, "ERR HOOK_FUNC %08x is not hooked\n",
			          addr);
			out[max - 1] = 0;
			return;
		}
		thread = hook_suspend_game();
		i = (unsigned int)hook_patch((unsigned char *)rebase(fh->addr),
		                             fh->orig, FUNC_JMP_LEN);
		hook_resume_game(thread);
		if (!i) {
			_snprintf(out, max, "ERR HOOK_FUNC %08x restore failed\n",
			          addr);
			out[max - 1] = 0;
			return;
		}
		fh->active = 0;
		_snprintf(out, max, "OK HOOK_FUNC addr=%08x dst=%s on=0\n",
		          addr, fh->name);
		out[max - 1] = 0;
		return;
	}

	/* --- on --- */
	if (!s_mod) {
		_snprintf(out, max, "ERR HOOK_FUNC no module - LOAD_MOD "
		          "first\n");
		out[max - 1] = 0;
		return;
	}
	if (addr < AGENT_TEXT_LO || addr + FUNC_JMP_LEN - 1 > AGENT_TEXT_HI) {
		_snprintf(out, max, "ERR HOOK_FUNC %08x is outside .text "
		          "(%08x..%08x)\n", addr, AGENT_TEXT_LO, AGENT_TEXT_HI);
		out[max - 1] = 0;
		return;
	}
	if (fh_hits_watched(addr)) {
		_snprintf(out, max, "ERR HOOK_FUNC %08x overlaps the watched "
		          "protection bytes - a jump there makes championship "
		          "mode unfinishable much later\n", addr);
		out[max - 1] = 0;
		return;
	}
	if (fh_find(addr)) {
		_snprintf(out, max, "ERR HOOK_FUNC %08x is already hooked\n",
		          addr);
		out[max - 1] = 0;
		return;
	}

	for (i = 0; i < FUNC_HOOK_MAX; i++)
		if (!s_funcHook[i].active)
			break;
	if (i == FUNC_HOOK_MAX) {
		_snprintf(out, max, "ERR HOOK_FUNC all %d slots are in use\n",
		          FUNC_HOOK_MAX);
		out[max - 1] = 0;
		return;
	}
	fh = &s_funcHook[i];

	fh->dst = (void *)GetProcAddress(s_mod, name);
	if (!fh->dst) {
		_snprintf(out, max, "ERR HOOK_FUNC %s is not exported by %s\n",
		          name, s_modName);
		out[max - 1] = 0;
		return;
	}

	p = (unsigned char *)rebase(addr);
	if (IsBadReadPtr(p, FUNC_JMP_LEN)) {
		_snprintf(out, max, "ERR HOOK_FUNC %08x is not readable\n", addr);
		out[max - 1] = 0;
		return;
	}
	memcpy(fh->orig, p, FUNC_JMP_LEN);

	rel = (int)((unsigned char *)fh->dst - (p + FUNC_JMP_LEN));
	patch[0] = 0xe9;
	memcpy(patch + 1, &rel, 4);

	thread = hook_suspend_game();
	i = (unsigned int)hook_patch(p, patch, FUNC_JMP_LEN);
	hook_resume_game(thread);
	if (!i) {
		_snprintf(out, max, "ERR HOOK_FUNC %08x could not be written\n",
		          addr);
		out[max - 1] = 0;
		return;
	}

	fh->addr   = addr;
	fh->active = 1;
	strcpy(fh->name, name);

	_snprintf(out, max, "OK HOOK_FUNC addr=%08x dst=%s at=%08x on=1 "
	          "was=%02x%02x%02x%02x%02x\n", addr, fh->name,
	          (unsigned int)(ULONG_PTR)fh->dst,
	          fh->orig[0], fh->orig[1], fh->orig[2], fh->orig[3],
	          fh->orig[4]);
	out[max - 1] = 0;
}

/*
 * UNLOAD_MOD - put every hook back, then free the module.
 *
 * THE UNHOOK IS NOT OPTIONAL AND IT IS NOT THE CALLER JOB. Freeing a
 * DLL while a MOTO function still jumps into it leaves an E9 pointing
 * at unmapped memory, and the game dies at the next call - which for
 * Plane_Distance is within one frame. So this restores every active
 * hook first and reports how many, rather than refusing and making the
 * caller remember which ones were live.
 *
 * WITHOUT THIS, ITERATING MEANS RESTARTING MOTO. Rebuild the module,
 * UNLOAD_MOD, LOAD_MOD, hook again - the game keeps its race, its
 * position and its state across the whole cycle, which is the point of
 * testing in the live process at all.
 *
 * FreeLibrary DECREMENTS A REFCOUNT. One LOAD_MOD is one reference, so
 * one call here drops it to zero and the module really goes; s_mod is
 * cleared either way so a failure cannot strand us holding a handle we
 * will not use.
 */
static void fh_cmd_unload(char *out, unsigned int max)
{
	HANDLE thread;
	int    freed = 0;
	int    ok = 1;
	int    i;

	if (!s_mod) {
		_snprintf(out, max, "ERR UNLOAD_MOD nothing is loaded\n");
		out[max - 1] = 0;
		return;
	}

	thread = hook_suspend_game();
	for (i = 0; i < FUNC_HOOK_MAX; i++) {
		if (!s_funcHook[i].active)
			continue;
		if (hook_patch((unsigned char *)rebase(s_funcHook[i].addr),
		               s_funcHook[i].orig, FUNC_JMP_LEN)) {
			s_funcHook[i].active = 0;
			freed++;
		} else {
			ok = 0;
		}
	}
	hook_resume_game(thread);

	/*
	 * A HOOK THAT WOULD NOT RESTORE KEEPS THE MODULE. Freeing it now
	 * would leave that jump pointing into nothing, so the safer half
	 * of a bad situation is to stay loaded and say so.
	 */
	if (!ok) {
		_snprintf(out, max, "ERR UNLOAD_MOD could not restore every "
		          "hook - %s is still loaded and %d were put back\n",
		          s_modName, freed);
		out[max - 1] = 0;
		return;
	}

	FreeLibrary(s_mod);
	s_mod = NULL;
	_snprintf(out, max, "OK UNLOAD_MOD name=%s unhooked=%d\n",
	          s_modName, freed);
	s_modName[0] = 0;
	out[max - 1] = 0;
}


static void fh_cmd_list(char *out, unsigned int max)
{
	unsigned int n = 0;
	int          i;

	n += (unsigned int)_snprintf(out + n, max - n, "OK LIST_FUNC mod=%s",
	                             s_mod ? s_modName : "-");
	for (i = 0; i < FUNC_HOOK_MAX && n < max - 64; i++) {
		if (!s_funcHook[i].active)
			continue;
		n += (unsigned int)_snprintf(out + n, max - n, " %08x=%s",
		                             s_funcHook[i].addr,
		                             s_funcHook[i].name);
	}
	_snprintf(out + n, max - n, "\n");
	out[max - 1] = 0;
}


static int agent_process_line(HANDLE pipe, char *line)
{
	char   reply[AGENT_REPLY_MAX];
	size_t len = strlen(line);

	if (len > 0 && line[len - 1] == '\r')
		line[len - 1] = 0;

	if (strcmp(line, "STATUS") == 0) {
		agent_status(reply, sizeof(reply));
		return agent_write(pipe, reply);
	}
	if (strncmp(line, "GET_MEM ", 8) == 0) {
		agent_get_mem(line + 8, reply, sizeof(reply));
		return agent_write(pipe, reply);
	}
	if (strncmp(line, "SET_HOOK ", 9) == 0) {
		hook_cmd_set(line + 9, reply, sizeof(reply));
		return agent_write(pipe, reply);
	}
	if (strncmp(line, "CLEAR_HOOK", 10) == 0) {
		hook_cmd_clear(line + 10, reply, sizeof(reply));
		return agent_write(pipe, reply);
	}
	if (strcmp(line, "GET_TRACE") == 0) {
		hook_cmd_trace(reply, sizeof(reply));
		return agent_write(pipe, reply);
	}
	if (strncmp(line, "SEND_INPUT ", 11) == 0) {
		agent_send_input(line + 11, reply, sizeof(reply));
		return agent_write(pipe, reply);
	}
	if (strncmp(line, "SET_MEM ", 8) == 0) {
		agent_set_mem(line + 8, reply, sizeof(reply));
		return agent_write(pipe, reply);
	}
	if (strncmp(line, "LOAD_MOD ", 9) == 0) {
		fh_cmd_load(line + 9, reply, sizeof(reply));
		return agent_write(pipe, reply);
	}
	if (strncmp(line, "HOOK_FUNC ", 10) == 0) {
		fh_cmd_hook(line + 10, reply, sizeof(reply));
		return agent_write(pipe, reply);
	}
	if (strcmp(line, "UNLOAD_MOD") == 0) {
		fh_cmd_unload(reply, sizeof(reply));
		return agent_write(pipe, reply);
	}
	if (strcmp(line, "LIST_FUNC") == 0) {
		fh_cmd_list(reply, sizeof(reply));
		return agent_write(pipe, reply);
	}
	if (strncmp(line, "SET_DATABP", 10) == 0) {
		dbp_cmd_set(line + 10, reply, sizeof(reply));
		return agent_write(pipe, reply);
	}
	if (strcmp(line, "GET_DATABP") == 0) {
		dbp_cmd_get(reply, sizeof(reply));
		return agent_write(pipe, reply);
	}
	if (line[0] == 0)
		return 1;

	return agent_write(pipe, "ERR unknown command\n");
}

/* ------------------------------------------------------------------ */
/* MasterVolume - turning the game down                               */
/* ------------------------------------------------------------------ */

/*
 * THE GAME'S "MASTER VOLUME" IS THE WINDOWS MIXER, not an internal
 * software gain. Settings_LoadAll (0x0042b2a0) reads three registry
 * values and hands each to Mixer_SetVolume (0x00470cf0), which ends
 * in mixerSetControlDetails [verified]:
 *
 *     line 0   "MasterVolume"     0x004e7ee4
 *     line 1   "CDVolume"         0x004e7ed8
 *     line 2   "WaveVolume"       0x004e7ecc
 *
 * So the game moves the OPERATING SYSTEM's volume on startup, which
 * is why it can be so loud - it restores whatever was saved,
 * regardless of what the machine is set to now.
 *
 * THERE IS NO COMMAND-LINE FLAG FOR IT. The binary documents its own
 * flags at 0x004dc664 and the list ends at -FrameRateMax; -NoSound is
 * the only audio switch and it disables DirectSound entirely.
 *
 * AND THE SETTINGS ARE IN THE REGISTRY, under
 * SOFTWARE\DelphineSoft\MotoRacer\CurrentVersion. Nothing here writes
 * there - the value is applied by calling the game's own setter, so
 * the change lasts for this run and nothing on the machine is edited.
 *
 * THE RANGE IS 0..32 and comes from the game, not from us: it is
 * mixer[line + 0x119], read live rather than assumed, because it is
 * filled in during mixer init and a different machine could differ.
 */

#define ADDR_MIXER_OBJ     0x005d2958u  /* the mixer object          */
#define ADDR_MIXER_SETVOL  0x00470cf0u  /* Mixer_SetVolume           */
#define MIXER_LINE_MASTER  0
#define MIXER_MAX_INDEX    0x119        /* mixer[line + this] = max  */


/*
 * __thiscall FROM C. MSVC will not declare a __thiscall pointer in a
 * C file, so this uses the standard __fastcall stand-in: ECX takes
 * the object, EDX takes a dummy the callee never reads, and the two
 * real arguments go on the stack in the same order and are cleaned
 * by the callee. Same machine code, no assembly.
 */
typedef void (__fastcall *MIXER_SETVOL_FN)(void *self, int unused,
                                           int line, int value);

static const char *const k_volumeName[3] = {
	"MasterVolume", "CDVolume", "WaveVolume"
};
static int s_volume[3]      = { -1, -1, -1 };
static int s_volumeLevel[3];
static int s_volumeDone[3];
static int s_volumeHeld[3];

/*
 * APPLIED FROM THE AGENT THREAD, AND HELD - not set once.
 *
 * IT CANNOT BE DONE AT DLL LOAD: this runs during process init,
 * before the EXE's entry point, and the mixer object is still null.
 *
 * AND ONE SHOT IS NOT ENOUGH, which was learnt the hard way. The
 * first version applied as soon as the mixer object went live, which
 * is EARLIER than Settings_LoadAll's own call, so the game set the
 * saved level afterwards and won. The log said "MasterVolume set to
 * 1 of 32" while the control read 65535 - FULL - because the line
 * reported our call rather than the mixer. A report that describes
 * the request instead of the result is worse than no report.
 *
 * So this reads the control BACK, remembers what the mixer actually
 * holds while we are the last writer, and re-applies whenever it
 * changes to something else. One compare per line per poll, and a
 * call into the game only when the value has drifted.
 *
 * THE TRADE, STATED PLAINLY: while a line is set here, the game's
 * own sound page cannot move that slider - the setting wins back
 * within 25 ms. That is what an override is for, and every line is
 * off unless asked for.
 *
 * ALL THREE LINES ARE SUPPORTED because MASTER MAY NOT BE THE ONE
 * THAT MATTERS. On a modern Windows the legacy mixer's master line
 * does not necessarily attenuate an application's own wave output,
 * and the game's effects - engine, voices, the glass crash - are
 * waves. WaveVolume is the line to reach for if MasterVolume alone
 * does not quieten it.
 */
static void volume_service(void)
{
	int *mixer;
	int  line;

	mixer = (int *)rebase(ADDR_MIXER_OBJ);
	if (IsBadReadPtr(mixer, 8) || mixer[0] == 0 || mixer[1] == 0)
		return;

	for (line = 0; line < 3; line++) {
		int maxValue = 0;
		int cur = 0;
		int value;

		if (s_volume[line] < 0)
			continue;
		if (!agent_copy(&maxValue, mixer + line + MIXER_MAX_INDEX, 4) ||
		    maxValue <= 0)
		    continue;
		if (!agent_copy(&cur, mixer + line * 0x5d + 2 + 0x5c, 4))
			continue;

		/*
		     * NOTHING TO DO WHILE THE CONTROL STILL HOLDS WHAT WE PUT
		     * THERE. s_volumeLevel is the value the GAME computed from
		     * ours and is on the mixer's 0..65535 scale, not the ini's.
		     */
		if (s_volumeDone[line] && cur == s_volumeLevel[line])
			continue;

		value = s_volume[line];
		if (value > maxValue)
			value = maxValue;

		__try {
			((MIXER_SETVOL_FN)rebase(ADDR_MIXER_SETVOL))(mixer, 0, line,
			                                             value);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			s_volume[line] = -1;   /* stop retrying every 25 ms */
			logf("  %s: setter faulted, left alone\n",
			     k_volumeName[line]);
			continue;
		}

		if (!agent_copy(&s_volumeLevel[line],
		                mixer + line * 0x5d + 2 + 0x5c, 4))
		    continue;

		if (!s_volumeDone[line]) {
			s_volumeDone[line] = 1;
			logf("  %s %d of %d - mixer level %d of 65535\n",
			     k_volumeName[line], value, maxValue,
			     s_volumeLevel[line]);
		} else if (!s_volumeHeld[line]) {
			s_volumeHeld[line] = 1;
			logf("  %s: the game reset it, holding at %d\n",
			     k_volumeName[line], s_volumeLevel[line]);
		}
	}
}

static DWORD WINAPI agent_thread(LPVOID param)
{
	char   input[AGENT_INPUT_MAX];
	HANDLE pipe;
	int    connected = 0;
	DWORD  inputLen = 0;

	(void)param;

	pipe = CreateNamedPipeA(AGENT_PIPE_NAME, PIPE_ACCESS_DUPLEX,
	                        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE |
	                        PIPE_NOWAIT | PIPE_REJECT_REMOTE_CLIENTS,
	                        1, 64 * 1024, 64 * 1024, 0, NULL);
	if (pipe == INVALID_HANDLE_VALUE) {
		logf("  agent: CreateNamedPipe failed (%lu)\n",
		     (unsigned long)GetLastError());
		return 1;
	}

	input[0] = 0;
	for (;;) {
		DWORD available = 0;

		volume_service();
		dbp_flush_pending();

		if (!connected) {
			DWORD err;

			if (ConnectNamedPipe(pipe, NULL)) {
				connected = 1;
				inputLen = 0;
				continue;
			}
			err = GetLastError();
			if (err == ERROR_PIPE_CONNECTED) {
				connected = 1;
				inputLen = 0;
				continue;
			}
			if (err != ERROR_PIPE_LISTENING && err != ERROR_NO_DATA) {
				logf("  agent: ConnectNamedPipe failed (%lu)\n",
				     (unsigned long)err);
				break;
			}
			Sleep(AGENT_POLL_MS);
			continue;
		}

		if (!PeekNamedPipe(pipe, NULL, 0, NULL, &available, NULL)) {
			DisconnectNamedPipe(pipe);
			connected = 0;
			inputLen = 0;
			continue;
		}
		if (available == 0) {
			Sleep(AGENT_POLL_MS);
			continue;
		}

		{
			char  chunk[128];
			DWORD want;
			DWORD got = 0;
			char *nl;

			want = available < (DWORD)sizeof(chunk) ? available
			                                        : (DWORD)sizeof(chunk);
			if (!ReadFile(pipe, chunk, want, &got, NULL)) {
				DisconnectNamedPipe(pipe);
				connected = 0;
				inputLen = 0;
				continue;
			}
			if (inputLen + got >= sizeof(input)) {
				inputLen = 0;
				input[0] = 0;
				if (!agent_write(pipe, "ERR command too long\n")) {
					DisconnectNamedPipe(pipe);
					connected = 0;
				}
				continue;
			}
			memcpy(input + inputLen, chunk, got);
			inputLen += got;
			input[inputLen] = 0;

			/*
			 * DRAIN EVERY COMPLETE LINE. One read can carry more than
			 * one command, and leaving the second in the buffer would
			 * make it look like the client had gone quiet.
			 */
			while ((nl = strchr(input, '\n')) != NULL) {
				DWORD used;

				*nl = 0;
				if (!agent_process_line(pipe, input)) {
					DisconnectNamedPipe(pipe);
					connected = 0;
					inputLen = 0;
					break;
				}
				used = (DWORD)(nl - input) + 1;
				inputLen -= used;
				memmove(input, input + used, inputLen);
				input[inputLen] = 0;
			}
		}
	}

	if (connected)
		DisconnectNamedPipe(pipe);
	CloseHandle(pipe);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Startup                                                            */
/* ------------------------------------------------------------------ */

static void read_settings(HMODULE self)
{
	char ini[MAX_PATH];
	char dir[MAX_PATH];
	char *slash;

	GetModuleFileNameA(self, ini, MAX_PATH);
	strcpy(dir, ini);
	slash = strrchr(dir, '\\');
	if (slash)
		slash[1] = 0;
	else
		dir[0] = 0;

	strcpy(ini, dir);
	strcat(ini, "mrhook.ini");

	sprintf(s_outPath, "%smrcapture.txt", dir);

	/*
	 * THE PATHS ARE NOT COMPILED IN. They default to the directory the
	 * DLL sits in, which is beside MOTO.EXE and so is already the
	 * install root - but mrhook.ini overrides both, because the
	 * capture machine and the analysis tree are not always the same
	 * folder.
	 */
	GetPrivateProfileStringA("mrhook", "DataPath", dir,
	                         s_dataPath, MAX_PATH, ini);
	GetPrivateProfileStringA("mrhook", "SavingPath", dir,
	                         s_savingPath, MAX_PATH, ini);
	s_noD3D = GetPrivateProfileIntA("mrhook", "NoD3D", 0, ini);
	s_restartKey = GetPrivateProfileIntA("mrhook", "RestartKey", 1,
	                                     ini);
	s_sideCamKey = GetPrivateProfileIntA("mrhook", "SideCamKey", 1,
	                                     ini);
	s_noCountdown = GetPrivateProfileIntA("mrhook", "NoCountdown", 0,
	                                      ini);
	{
		int v;

		for (v = 0; v < 3; v++)
			s_volume[v] = GetPrivateProfileIntA("mrhook",
		                                        k_volumeName[v],
		                                        -1, ini);
	}

	GetPrivateProfileStringA("mrhook", "Language", "ENGLISH",
	                         s_language, sizeof(s_language), ini);
}

/*
 * ESC then ENTER, posted to the game window - the pause menu
 * restarts the race on the default item. The two go in on separate
 * polls rather than back to back: the menu needs a frame to come up,
 * and an ENTER that arrives before it does is swallowed.
 */
/*
 * Select a look slot on whichever camera record is live. The index
 * is read every time rather than cached - the game picks the record
 * and a stale one would aim the write at another camera.
 *
 * flLookYaw is cleared alongside because TAB leaves pi in it, and on
 * the chase path that value shortens the boom.
 */
static void side_cam_write(unsigned int slot)
{
	unsigned int idx = 0;
	unsigned int rec;

	if (!si_read(CAM_INDEX, &idx) || idx > 8)
		return;

	rec = CAM_RECORDS + idx * CAM_STRIDE;
	si_write(rec + CAM_OFF_LOOKYAW, 0u);
	si_write(rec + CAM_OFF_LOOKSLOT, slot);
}


/*
 * Point the look-slot camera at the level matrix, or back at the
 * pitched one. The game thread is suspended across the write
 * because this runs long after the entry point.
 */
static void side_cam_level(int on)
{
	unsigned int   want = on ? CAM_SRC_LEVEL : CAM_SRC_PITCHED;
	unsigned char *p = (unsigned char *)rebase(CAM_SRC_IMM);
	HANDLE         thread;

	thread = hook_suspend_game();
	hook_patch(p, (const unsigned char *)&want, 4);
	hook_resume_game(thread);
}


static void restart_trigger(void)
{
	si_key(0x1bu, 1);
	si_key(0x1bu, 0);
	Sleep(120);
	si_key(0x0du, 1);
	si_key(0x0du, 0);
	logf("restart: ESC+ENTER posted\n");
}


/*
 * GetAsyncKeyState IS NOT FOCUS AWARE, and these three hotkeys are
 * polled from a background thread - so without a foreground test they
 * fire from WHATEVER WINDOW the key was pressed in.
 *
 * TWO OF THE THREE COLLIDE WITH OUR OWN PORT, exactly:
 *
 *     "1"   mrhook side camera   vs  mracer race_camera_toggle_side
 *     "2"   mrhook race restart  vs  mracer restart-the-page
 *     F11   mrhook screen capture
 *
 * so driving the port toggled MOTO's side camera and restarted MOTO's
 * race at the same time. The side camera is the one that showed,
 * because side_cam_write RE-ASSERTS SLOT 2 EVERY TEN MILLISECONDS
 * while it is on: nothing the game or the agent does can clear it for
 * longer than one poll, so it reads as "the agent keeps switching the
 * camera". It also silently disables the first-person cockpit, whose
 * gate at 0x0046213e rejects any non-zero look slot - which cost
 * several probes that came back with no hits and no explanation.
 *
 * THE EDGES ARE STILL TRACKED WHILE UNFOCUSED, and only the ACTION is
 * gated. Skipping the sample instead would make the first poll after
 * focus returns an edge for any key still held, and alt-tabbing back
 * with a finger on "2" would restart the race.
 */
static DWORD WINAPI capture_thread(LPVOID param)
{
	int wasDown = 0;
	int wasRestart = 0;
	int wasSideCam = 0;
	int sideCamOn = 0;

	(void)param;
	for (;;) {
		HWND w       = si_window();
		int  focused = (w != NULL && GetForegroundWindow() == w);
		int  down    = (GetAsyncKeyState(CAPTURE_KEY) & 0x8000) != 0;
		int  rst;

		if (down && !wasDown && focused)
			capture_trigger();
		wasDown = down;

		rst = s_restartKey
		      && (GetAsyncKeyState(RESTART_KEY) & 0x8000) != 0;
		if (rst && !wasRestart && focused)
			restart_trigger();
		wasRestart = rst;

		if (s_sideCamKey) {
			int cam = (GetAsyncKeyState(SIDE_CAM_KEY) & 0x8000) != 0;

			if (cam && !wasSideCam && focused) {
				sideCamOn = !sideCamOn;
				side_cam_level(sideCamOn);
				if (!sideCamOn)
					side_cam_write(CAM_SLOT_CHASE);
				logf("side camera %s\n",
				     sideCamOn ? "on" : "off");
			}
			wasSideCam = cam;

			if (sideCamOn)
				side_cam_write(CAM_SLOT_LEFT);
		}

		/*
		 * TEN MILLISECONDS, NOT THIRTY. The side camera is
		 * re-asserted from this loop and the game runs a frame in
		 * about thirty, so a thirty-millisecond poll drops one
		 * here and there and the view flickers back.
		 */
		Sleep(10);
	}
}

static void startup(HMODULE self)
{
	HMODULE exe = GetModuleHandleA(NULL);

	s_delta = (unsigned int)exe - IMAGE_PREFERRED_BASE;

	/*
	 * DllMain RUNS ON THE GAME THREAD, so this is the only place
	 * the id can be had without enumerating. hook_arm suspends it
	 * before patching live code.
	 */
	s_mainThreadId = GetCurrentThreadId();

	read_settings(self);

	if (s_noCountdown) {
		static const unsigned char zero[COUNTDOWN_LEN] = { 0, 0, 0, 0 };

		if (hook_patch((unsigned char *)rebase(ADDR_COUNTDOWN_MS),
		               zero, COUNTDOWN_LEN))
			logf("  NoCountdown patched at %08x - races start "
			     "at once\n", ADDR_COUNTDOWN_MS);
		else
			logf("  NoCountdown FAILED to patch %08x\n",
			     ADDR_COUNTDOWN_MS);
	}

	logf("\nmrhook loaded.  MOTO.EXE at %p, delta %08x\n",
	     (void *)exe, s_delta);
	logf("  DataPath   %s\n", s_dataPath);
	logf("  SavingPath %s\n", s_savingPath);
	logf("  Language   %s\n", s_language);

	/*
	 * NoD3D=1 in mrhook.ini is the same switch as -NoD3D on the
	 * command line, without needing a command line. It forces the
	 * SOFTWARE renderer, which on a machine where the 16-bit
	 * colour gate leaves the hardware path unusable is the
	 * difference between a visible game and a blank one.
	 *
	 * Written here rather than from the agent thread because this
	 * runs before the parser and before Settings_LoadAll reads it;
	 * doing it later would be a race against both.
	 */
	if (s_noD3D) {
		*(int *)rebase(ADDR_NO_D3D) = 1;
		logf("  NoD3D      forced - software renderer\n");
	}

	if (s_delta != 0) {
		logf("  NOTE: the image did not load at its preferred base.\n"
		     "        Every address below is rebased by the delta.\n");
	}

	if (!write_jump(ADDR_REG_GET_STRING, (void *)mr_reg_get_string) ||
	    !write_jump(ADDR_REG_GET_DWORD,  (void *)mr_reg_get_dword)) {
		logf("  FAILED to patch the registry helpers - the install\n"
		     "  check will still fire.\n");
	} else {
		logf("  registry helpers answered in-process; nothing is\n"
		     "  written to HKLM.\n");
	}

	logf("  F11 is a stub - see capture_trigger() in mrhook.c.\n");
	logf("  agent listening on %s\n\n", AGENT_PIPE_NAME);

	dbp_init();

	CreateThread(NULL, 0, capture_thread, NULL, 0, NULL);
	CreateThread(NULL, 0, agent_thread, NULL, 0, NULL);
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID reserved)
{
	(void)reserved;
	if (reason == DLL_PROCESS_ATTACH) {
		DisableThreadLibraryCalls(self);
		startup((HMODULE)self);
	}
	return TRUE;
}
