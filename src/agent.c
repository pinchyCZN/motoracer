/*
 * agent.c - the debug agent. See agent.h for the wire protocol.
 *
 * ORIGIN  NONE - ours. Every function in this file is our own work;
 * none of it is transcribed from MOTO.EXE. Modelled on the cdino_pc
 * agent (N:/DEV/CDINO/cdino_pc).
 *
 * The symbol table comes from the linker .map beside the .exe, which
 * means Release|Win32 has to be linked with /MAP. Addresses in the map
 * are Rva+Base against the PREFERRED base; we rebase them onto the
 * module's actual base, so the agent still resolves correctly under
 * ASLR instead of handing back confidently wrong pointers.
 */
#include "agent.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <intrin.h>

#include "log.h"
#include "timing.h"
#include "race.h"

#define AGENT_PIPE_NAME    "\\\\.\\pipe\\mracer_agent"
#define AGENT_INPUT_MAX    512
#define AGENT_MEM_MAX      32768
#define AGENT_REPLY_MAX    (96 * 1024)
#define AGENT_WRITE_CHUNK  16384
#define AGENT_WRITE_TMO_MS 15000

typedef struct {
	char  name[64];
	char  obj[64];
	void *ptr;
} MR_AgentSym;

static MR_AgentSym *g_syms;
static int          g_symCount;
static size_t       g_mapPrefBase;
static size_t       g_modBase;
static int          g_relocated;

static HANDLE g_thread;
static HANDLE g_stopEvent;
static int    g_started;


/* ------------------------------------------------------------------ */
/* the symbol table                                                    */
/* ------------------------------------------------------------------ */

static void agent_add_sym(const char *name, const char *obj, void *ptr)
{
	int          n = g_symCount + 1;
	MR_AgentSym *tmp = (MR_AgentSym *)realloc(g_syms,
	                                          n * sizeof(MR_AgentSym));
	MR_AgentSym *e;

	if (!tmp)
		return;

	/*
	 * DUPLICATE NAMES ARE KEPT. Two file-static symbols may share a
	 * name in different translation units, which is exactly what the
	 * obj= selector is for.
	 */
	e = &tmp[n - 1];
	memset(e, 0, sizeof(*e));
	strncpy(e->name, name, sizeof(e->name) - 1);
	if (obj)
		strncpy(e->obj, obj, sizeof(e->obj) - 1);
	e->ptr = ptr;

	g_syms = tmp;
	g_symCount = n;
}


static void agent_extract_word(const char *line, char *word, int size)
{
	char format[32];

	if (!word || size <= 0)
		return;
	word[0] = 0;
	sprintf(format, "%%%ds", size - 1);
	if (sscanf(line, format, word) != 1)
		word[0] = 0;
}


static int agent_has_any(const char *s, const char *set)
{
	while (*set) {
		if (strchr(s, *set))
			return 1;
		set++;
	}
	return 0;
}


static void agent_strip_leading(char *s, char c)
{
	size_t len;

	if (!s || s[0] != c)
		return;
	len = strlen(s);
	memmove(s, s + 1, len);        /* includes the terminator */
}


/*
 * The last whitespace-delimited token of a "Publics by Value" line,
 * when it ends in ".obj". Library members arrive as "libcmt:file.obj"
 * and are stored whole.
 */
static void agent_tail_obj(const char *line, char *obj, int size)
{
	const char *end;
	const char *start;
	size_t      n;

	obj[0] = 0;
	end = line + strlen(line);
	while (end > line && (end[-1] == '\n' || end[-1] == '\r'
	                      || end[-1] == ' ' || end[-1] == '\t'))
	    end--;

	start = end;
	while (start > line && start[-1] != ' ' && start[-1] != '\t')
		start--;

	n = (size_t)(end - start);
	if (n == 0 || n >= (size_t)size)
		return;
	if (n < 4 || _strnicmp(end - 4, ".obj", 4) != 0)
		return;

	memcpy(obj, start, n);
	obj[n] = 0;
}


static const char *agent_map_path(void)
{
	static char path[MAX_PATH];
	DWORD       n;
	size_t      len;

	path[0] = 0;
	n = GetModuleFileNameA(NULL, path, MAX_PATH);
	if (n == 0 || n >= MAX_PATH) {
		path[0] = 0;
		return path;
	}
	len = strlen(path);
	if (len >= 4 && _stricmp(path + len - 4, ".exe") == 0)
		path[len - 4] = 0;
	if (strlen(path) + 4 >= MAX_PATH) {
		path[0] = 0;
		return path;
	}
	strcat(path, ".map");
	return path;
}


static void agent_parse_map(void)
{
	const char *fname = agent_map_path();
	FILE       *f = fopen(fname, "r");
	int         state = 0;
	static char line[1024];

	g_modBase = (size_t)GetModuleHandleA(NULL);

	if (!f) {
		mrlog("agent:  no map file: %s\n", fname);
		return;
	}

	while (fgets(line, sizeof(line), f)) {
		if (state == 0) {
			if (!g_mapPrefBase) {
				const char *pl = strstr(line,
				                        "Preferred load address is");

				if (pl)
					g_mapPrefBase = (size_t)strtoul(pl + 25, NULL, 16);
			}
			if (strstr(line, " Address ")
				&& strstr(line, " Publics by Value ")
			    && strstr(line, " Rva+Base ")) {
				state = 1;
			}
			continue;
		}

		if (line[5] == ':') {
			MR_AgentSym sym;
			char       *p;

			memset(&sym, 0, sizeof(sym));
			agent_extract_word(line + 15, sym.name, sizeof(sym.name));

			p = strchr(line + 22, ' ');
			if (p && !agent_has_any(sym.name, "?@$")) {
				char hex[12] = { 0 };
				long val;

				agent_extract_word(p, hex, sizeof(hex));
				val = strtol(hex, NULL, 16);
				if (val) {
					agent_strip_leading(sym.name, '_');
					agent_tail_obj(line, sym.obj, sizeof(sym.obj));
					agent_add_sym(sym.name, sym.obj, (void *)val);
				}
			}
		}
	}
	fclose(f);

	g_relocated = (g_mapPrefBase != 0 && g_modBase != g_mapPrefBase);
	mrlog("agent:  %d symbols from %s\n", g_symCount, fname);
	if (g_relocated) {
		mrlog("        image relocated (map %08X, actual %08X) - "
		      "rebasing\n", (unsigned)g_mapPrefBase, (unsigned)g_modBase);
	}
}


/* Map address onto the module's actual base. */
static unsigned char *agent_runtime_addr(const MR_AgentSym *e)
{
	size_t a = (size_t)e->ptr;

	if (g_mapPrefBase && g_modBase)
		a = a - g_mapPrefBase + g_modBase;
	return (unsigned char *)a;
}


/*
 * First symbol matching name, and obj when one is given. obj compares
 * case-insensitively against the whole stored token.
 */
static const MR_AgentSym *agent_find(const char *name, const char *obj)
{
	int i;

	for (i = 0; i < g_symCount; i++) {
		if (strcmp(g_syms[i].name, name) != 0)
			continue;
		if (obj && obj[0] && _stricmp(g_syms[i].obj, obj) != 0)
			continue;
		return &g_syms[i];
	}
	return NULL;
}


static int agent_count_named(const char *name)
{
	int i, n = 0;

	for (i = 0; i < g_symCount; i++) {
		if (strcmp(g_syms[i].name, name) == 0)
			n++;
	}
	return n;
}


/* ------------------------------------------------------------------ */
/* pipe I/O                                                            */
/* ------------------------------------------------------------------ */

static int agent_write(HANDLE pipe, const void *data, DWORD size)
{
	const unsigned char *p = (const unsigned char *)data;
	DWORD                remaining = size;
	DWORD                t0 = GetTickCount();

	while (remaining > 0) {
		DWORD written = 0;
		DWORD chunk;
		DWORD avail;

		if ((GetTickCount() - t0) > AGENT_WRITE_TMO_MS)
			return 0;

		chunk = remaining;
		if (chunk > AGENT_WRITE_CHUNK)
			chunk = AGENT_WRITE_CHUNK;

		if (!WriteFile(pipe, p, chunk, &written, NULL)) {
			DWORD err = GetLastError();

			if (err == ERROR_BROKEN_PIPE
			    || err == ERROR_PIPE_NOT_CONNECTED)
			    return 0;
			if (err == ERROR_NO_DATA) {
				avail = 0;
				if (!PeekNamedPipe(pipe, NULL, 0, NULL, &avail, NULL))
					return 0;
				Sleep(1);
				continue;
			}
			mrlog("agent:  WriteFile failed (%lu)\n",
			      (unsigned long)err);
			return 0;
		}
		if (written == 0) {
			avail = 0;
			if (!PeekNamedPipe(pipe, NULL, 0, NULL, &avail, NULL))
				return 0;
			Sleep(1);
			continue;
		}
		p += written;
		remaining -= written;
	}
	return 1;
}


static int agent_write_text(HANDLE pipe, const char *text)
{
	return agent_write(pipe, text, (DWORD)strlen(text));
}


/* ------------------------------------------------------------------ */
/* function hooks                                                      */
/* ------------------------------------------------------------------ */

/*
 * Ported from cdino_hooks.c with 10 slots instead of 32.
 *
 * A hook overwrites the first FIVE BYTES of a function with a jmp to a
 * per-slot trampoline. That only works because Debug|Win32 is built
 * /hotpatch, which puts a two-byte nop in front of the frame setup:
 *
 *     66 90        xchg ax,ax      the hotpatch pad
 *     55           push ebp
 *     8b ec        mov ebp,esp
 *
 * exactly five bytes, and VERIFIED to be what our own build emits.
 * Anything else is refused rather than patched.
 *
 * THE WHOLE PREAMBLE IS REPLACED, so the target's own push ebp / mov
 * ebp,esp never runs. The trampoline's prologue rebuilds that frame
 * identically, which is what makes the arguments land where a cdecl
 * callee expects them and makes _ReturnAddress() the real call site.
 */

#define AGENT_HOOK_MAX         10
#define AGENT_HOOK_ARGS_MAX    5
#define AGENT_HOOK_LOG_MAX     16
#define AGENT_HOOK_TAG_MAX     16
#define AGENT_HOOK_NAME_MAX    64
#define AGENT_HOOK_TRACE_MAX   256
#define AGENT_HOOK_LINE_MAX    384

/*
 * A hit further than this past a symbol is reported as "?" rather than
 * blamed on the previous one. The map carries no function sizes.
 */
#define AGENT_HOOK_SYM_MAX_OFF 0x4000

#define AGENT_HOOK_F_ARMED     0x01

enum agent_arg {
	AGENT_ARG_NONE = 0,
	AGENT_ARG_0,
	AGENT_ARG_1,
	AGENT_ARG_2,
	AGENT_ARG_3,
	AGENT_ARG_4,
	AGENT_ARG_RA
};

enum agent_log_op {
	AGENT_LOG_ARG = 0,
	AGENT_LOG_ARG_F,
	AGENT_LOG_MEM_B,
	AGENT_LOG_MEM_W,
	AGENT_LOG_MEM_L,
	AGENT_LOG_MEM_F,
	AGENT_LOG_RA
};

enum agent_when {
	AGENT_WHEN_ALWAYS = 0,
	AGENT_WHEN_ARG_NZ,
	AGENT_WHEN_ARG_EQ,
	AGENT_WHEN_ARG_NE
};

typedef struct {
	unsigned char op;    /* agent_log_op                        */
	unsigned char arg;   /* agent_arg - value or memory base    */
	short         off;   /* byte offset for the MEM_* forms     */
} MR_AgentLogItem;

typedef struct {
	char            name[AGENT_HOOK_NAME_MAX];
	void           *target;
	void           *ret;        /* target+5, where the trampoline jumps */
	unsigned char   saved[5];   /* the original preamble, for restore   */
	unsigned char   flags;
	unsigned char   slot;
	unsigned char   nargs;      /* how many cdecl args are real, 0..5   */
	void           *lastRa;     /* transient: this hit's caller         */
	unsigned char   whenKind;
	unsigned char   whenArg;
	unsigned        whenImm;
	unsigned char   nlog;
	MR_AgentLogItem log[AGENT_HOOK_LOG_MAX];
	unsigned short  hitCap;
	unsigned short  hitCount;
	char            tag[AGENT_HOOK_TAG_MAX];
} MR_AgentHook;

static MR_AgentHook s_hooks[AGENT_HOOK_MAX];

/*
 * The trace ring. Written by the game thread inside a trampoline, read
 * by the pipe thread - but every reader goes through the mailbox, so
 * the drain is serialised against the writer.
 */
static char s_trace[AGENT_HOOK_TRACE_MAX][AGENT_HOOK_LINE_MAX];
static int  s_traceHead;
static int  s_traceCount;

/* Re-entry guard. The game loop is single threaded, so a plain int. */
static int  s_inRecord;


/*
 * Greatest symbol at or below addr, formatted "name+0xOFF".
 *
 * A LINEAR SCAN ON PURPOSE. Sorting g_syms by address would change
 * which duplicate GET_MEM resolves to, and "first match in map order"
 * is documented behaviour. Roughly 2500 comparisons once per recorded
 * hit is nothing beside the pipe round trip that reads the result.
 */
static void agent_format_code_addr(void *addr, char *out, int cap)
{
	const MR_AgentSym *best = NULL;
	size_t             key = (size_t)addr;
	size_t             off;
	int                i;

	if (!out || cap <= 0)
		return;
	out[0] = 0;

	for (i = 0; i < g_symCount; i++) {
		size_t p = (size_t)agent_runtime_addr(&g_syms[i]);

		if (p <= key && (!best
		                 || p > (size_t)agent_runtime_addr(best)))
		    best = &g_syms[i];
	}

	if (!best) {
		_snprintf(out, cap, "?");
		out[cap - 1] = 0;
		return;
	}

	off = key - (size_t)agent_runtime_addr(best);
	if (off > AGENT_HOOK_SYM_MAX_OFF)
		_snprintf(out, cap, "?");
	else if (off)
		_snprintf(out, cap, "%s+0x%X", best->name, (unsigned)off);
	else
		_snprintf(out, cap, "%s", best->name);
	out[cap - 1] = 0;
}


/* Write the 5-byte jmp. Returns the resume address, or NULL. */
static void *agent_hook_install(void *target, void *hook)
{
	unsigned char *bytes = (unsigned char *)target;
	DWORD          old;

	if (!VirtualProtect(bytes, 5, PAGE_EXECUTE_READWRITE, &old))
		return NULL;

	bytes[0] = 0xe9;
	*(int *)(bytes + 1) = (int)((size_t)hook - ((size_t)bytes + 5));
	VirtualProtect(bytes, 5, old, &old);
	FlushInstructionCache(GetCurrentProcess(), bytes, 5);
	return bytes + 5;
}


static void agent_hook_uninstall(void *target, const unsigned char saved[5])
{
	DWORD old;

	if (!target)
		return;
	if (!VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &old))
		return;
	memcpy(target, saved, 5);
	VirtualProtect(target, 5, old, &old);
	FlushInstructionCache(GetCurrentProcess(), target, 5);
}


static void agent_hook_on_entry(int slot, void *ra, void *a0, void *a1,
                                void *a2, void *a3, void *a4);

/*
 * Per-slot trampoline.
 *
 * resume MUST BE A LOCAL - MSVC inline asm cannot take s_hooks[i].ret
 * as a mov operand.
 *
 * _ReturnAddress() MUST EXPAND HERE and not in a callee, where it would
 * only report this logging code. pushad moves esp and never ebp, so
 * HOOK_ENTRY does not disturb the frame the epilogue unwinds to.
 */
#define HOOK_ENTRY()    __asm { pushad }
#define HOOK_EXIT(addr) \
    __asm { popad } \
    __asm { mov esp, ebp } \
    __asm { mov eax, addr } \
    __asm { jmp eax }

/*
 * C4740 - "flow in or out of inline asm suppresses global
 * optimization" - is CORRECT AND INTENDED: the whole point of the
 * trampoline is to jmp out to the resume address instead of
 * returning. Ten slots emit it ten times, which would drown every
 * other warning in the build.
 */
#pragma warning(push)
#pragma warning(disable: 4740)

#define AGENT_HOOK_SLOT(i) \
static void agent_hook_slot_##i(void *a0, void *a1, void *a2, void *a3, \
                                void *a4) \
{ \
	void *resume; \
	void *ra; \
	HOOK_ENTRY(); \
	ra = _ReturnAddress(); \
	agent_hook_on_entry(i, ra, a0, a1, a2, a3, a4); \
	resume = s_hooks[i].ret; \
	HOOK_EXIT(resume); \
}

AGENT_HOOK_SLOT(0)
AGENT_HOOK_SLOT(1)
AGENT_HOOK_SLOT(2)
AGENT_HOOK_SLOT(3)
AGENT_HOOK_SLOT(4)
AGENT_HOOK_SLOT(5)
AGENT_HOOK_SLOT(6)
AGENT_HOOK_SLOT(7)
AGENT_HOOK_SLOT(8)
AGENT_HOOK_SLOT(9)

#pragma warning(pop)

#undef AGENT_HOOK_SLOT

typedef void (*MR_AgentSlotFn)(void *, void *, void *, void *, void *);

static const MR_AgentSlotFn s_slotFn[AGENT_HOOK_MAX] = {
	agent_hook_slot_0, agent_hook_slot_1, agent_hook_slot_2,
	agent_hook_slot_3, agent_hook_slot_4, agent_hook_slot_5,
	agent_hook_slot_6, agent_hook_slot_7, agent_hook_slot_8,
	agent_hook_slot_9
};


/*
 * a0..a4 are only real parameters below nargs. Above it they are the
 * caller's stack, so they are never printed and never dereferenced.
 */
static int agent_arg_live(const MR_AgentHook *h, unsigned char arg)
{
	if (!h || arg < AGENT_ARG_0 || arg > AGENT_ARG_4)
		return 0;
	return (arg - AGENT_ARG_0) < (int)h->nargs;
}


static const char *agent_arg_name(unsigned char arg)
{
	switch (arg) {
	case AGENT_ARG_0:  return "a0";
	case AGENT_ARG_1:  return "a1";
	case AGENT_ARG_2:  return "a2";
	case AGENT_ARG_3:  return "a3";
	case AGENT_ARG_4:  return "a4";
	case AGENT_ARG_RA: return "ra";
	default:           return "?";
	}
}


/* The bits of a by-value float argument, as a float. */
static float agent_bits_to_float(void *v)
{
	union { unsigned u; float f; } c;

	c.u = (unsigned)(size_t)v;
	return c.f;
}


static int agent_when_ok(const MR_AgentHook *h, void *const args[])
{
	void *v;

	if (!h)
		return 0;
	if (h->whenKind == AGENT_WHEN_ALWAYS)
		return 1;
	if (!agent_arg_live(h, h->whenArg))
		return 1;

	v = args[h->whenArg - AGENT_ARG_0];
	switch (h->whenKind) {
	case AGENT_WHEN_ARG_NZ: return v != 0;
	case AGENT_WHEN_ARG_EQ: return (unsigned)(size_t)v == h->whenImm;
	case AGENT_WHEN_ARG_NE: return (unsigned)(size_t)v != h->whenImm;
	default:                return 1;
	}
}


static void agent_append_field(MR_AgentHook *h, void *const args[],
                               const MR_AgentLogItem *item,
                               char *dst, int cap)
{
	int            used = (int)strlen(dst);
	char          *p = dst + used;
	int            rem = cap - used;
	void          *base;
	unsigned char *addr;

	if (rem < 32)
		return;

	if (item->op == AGENT_LOG_RA) {
		char sym[AGENT_HOOK_NAME_MAX + 16];

		if (rem < (int)sizeof(sym) + 16) {
			_snprintf(p, rem, " ra=%p", h->lastRa);
			return;
		}
		agent_format_code_addr(h->lastRa, sym, (int)sizeof(sym));
		_snprintf(p, rem, " ra=%p ra_fn=%s", h->lastRa, sym);
		return;
	}

	if (!agent_arg_live(h, item->arg))
		return;

	if (item->op == AGENT_LOG_ARG) {
		_snprintf(p, rem, " %s=%p", agent_arg_name(item->arg),
		          args[item->arg - AGENT_ARG_0]);
		return;
	}
	if (item->op == AGENT_LOG_ARG_F) {
		_snprintf(p, rem, " %sf=%g", agent_arg_name(item->arg),
		          (double)agent_bits_to_float(
		              args[item->arg - AGENT_ARG_0]));
		return;
	}

	base = args[item->arg - AGENT_ARG_0];
	if (!base)
		return;
	addr = (unsigned char *)base + item->off;

	switch (item->op) {
	case AGENT_LOG_MEM_B:
		_snprintf(p, rem, " %sb+%X=%02X", agent_arg_name(item->arg),
		          (unsigned)item->off, (unsigned)addr[0]);
		break;
	case AGENT_LOG_MEM_W:
		_snprintf(p, rem, " %sw+%X=%04X", agent_arg_name(item->arg),
		          (unsigned)item->off,
		          (unsigned)*(unsigned short *)addr);
		break;
	case AGENT_LOG_MEM_L:
		_snprintf(p, rem, " %sl+%X=%08X", agent_arg_name(item->arg),
		          (unsigned)item->off, (unsigned)*(unsigned *)addr);
		break;
	case AGENT_LOG_MEM_F:
		_snprintf(p, rem, " %sf+%X=%g", agent_arg_name(item->arg),
		          (unsigned)item->off, (double)*(float *)addr);
		break;
	default:
		break;
	}
}


static void agent_hook_record(MR_AgentHook *h, void *const args[])
{
	char line[AGENT_HOOK_LINE_MAX];
	int  i;

	_snprintf(line, sizeof(line), "[%s] name=%s hits=%u",
	          h->tag[0] ? h->tag : "hit", h->name,
	          (unsigned)(h->hitCount + 1));
	line[sizeof(line) - 1] = 0;

	for (i = 0; i < h->nlog; i++)
		agent_append_field(h, args, &h->log[i], line,
	                       (int)sizeof(line));

	_snprintf(s_trace[s_traceHead], AGENT_HOOK_LINE_MAX, "%s", line);
	s_trace[s_traceHead][AGENT_HOOK_LINE_MAX - 1] = 0;
	s_traceHead = (s_traceHead + 1) % AGENT_HOOK_TRACE_MAX;
	if (s_traceCount < AGENT_HOOK_TRACE_MAX)
		s_traceCount++;
}


static void agent_hook_on_entry(int slot, void *ra, void *a0, void *a1,
                                void *a2, void *a3, void *a4)
{
	MR_AgentHook *h;
	void         *args[AGENT_HOOK_ARGS_MAX];

	if (slot < 0 || slot >= AGENT_HOOK_MAX)
		return;
	h = &s_hooks[slot];
	if (!(h->flags & AGENT_HOOK_F_ARMED))
		return;

	/*
	 * A hooked routine reached from inside the recorder would recurse
	 * until the stack ran out. Cheap insurance on top of refusing to
	 * hook our own names at install.
	 */
	if (s_inRecord)
		return;

	/* Consumed synchronously below, so re-entry cannot leave it stale. */
	h->lastRa = ra;

	args[0] = a0;
	args[1] = a1;
	args[2] = a2;
	args[3] = a3;
	args[4] = a4;

	if (!agent_when_ok(h, args))
		return;
	if (h->hitCap != 0 && h->hitCount >= h->hitCap)
		return;

	s_inRecord = 1;
	agent_hook_record(h, args);
	s_inRecord = 0;
	h->hitCount++;
}


/* ---- parsing ----------------------------------------------------- */

static int agent_parse_u32(const char *text, unsigned *out)
{
	char         *end;
	unsigned long value;

	if (!text || !text[0] || !out)
		return 0;
	if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
		value = strtoul(text, &end, 16);
	else
		value = strtoul(text, &end, 0);
	if (end == text || *end != 0)
		return 0;
	*out = (unsigned)value;
	return 1;
}


static int agent_parse_off(const char *text, short *out)
{
	unsigned u;

	if (!agent_parse_u32(text, &u) || u > 0x7fff)
		return 0;
	*out = (short)u;
	return 1;
}


/* "a0".."a4" or "ra". len is given because the token may not end here. */
static int agent_parse_arg_n(const char *name, int len, unsigned char *out)
{
	if (!name || len != 2 || !out)
		return 0;
	if ((name[0] == 'a' || name[0] == 'A')
		&& name[1] >= '0' && name[1] <= '4') {
		*out = (unsigned char)(AGENT_ARG_0 + (name[1] - '0'));
		return 1;
	}
	if ((name[0] == 'r' || name[0] == 'R')
		&& (name[1] == 'a' || name[1] == 'A')) {
		*out = AGENT_ARG_RA;
		return 1;
	}
	return 0;
}


/*
 * ra | aN | aNf | aNb+OFF | aNw+OFF | aNl+OFF | aNf+OFF
 *
 * THE TWO f FORMS ARE DIFFERENT and the '+' is what separates them.
 * aNf is the ARGUMENT's own bits read as a float, which is what a
 * float passed by value actually is - our code does that everywhere.
 * aNf+OFF dereferences, like the b/w/l forms.
 */
static int agent_parse_log_token(const char *tok, MR_AgentLogItem *item,
                                 const char **err)
{
	int           len;
	unsigned char arg;
	char          sizech;

	if (!tok || !tok[0] || !item) {
		if (err)
			*err = "empty log token";
		return 0;
	}
	memset(item, 0, sizeof(*item));
	len = (int)strlen(tok);

	if (len == 2 && (tok[0] == 'r' || tok[0] == 'R')
		&& (tok[1] == 'a' || tok[1] == 'A')) {
		item->op = AGENT_LOG_RA;
		item->arg = AGENT_ARG_RA;
		return 1;
	}

	/* aNb+OFF and friends */
	if (len >= 5) {
		sizech = (char)tolower((unsigned char)tok[2]);
		if ((sizech == 'b' || sizech == 'w' || sizech == 'l'
		     || sizech == 'f') && tok[3] == '+') {
			if (!agent_parse_arg_n(tok, 2, &arg)
				|| arg < AGENT_ARG_0 || arg > AGENT_ARG_4) {
				if (err)
					*err = "bad mem base arg";
				return 0;
			}
			if (!agent_parse_off(tok + 4, &item->off)) {
				if (err)
					*err = "bad mem offset";
				return 0;
			}
			item->arg = arg;
			if (sizech == 'b')
				item->op = AGENT_LOG_MEM_B;
			else if (sizech == 'w')
				item->op = AGENT_LOG_MEM_W;
			else if (sizech == 'l')
				item->op = AGENT_LOG_MEM_L;
			else
				item->op = AGENT_LOG_MEM_F;
			return 1;
		}
	}

	/* aNf - the argument itself, as a float */
	if (len == 3 && (tolower((unsigned char)tok[2]) == 'f')
		&& agent_parse_arg_n(tok, 2, &arg)
	    && arg >= AGENT_ARG_0 && arg <= AGENT_ARG_4) {
		item->op = AGENT_LOG_ARG_F;
		item->arg = arg;
		return 1;
	}

	/* aN */
	if (agent_parse_arg_n(tok, len, &arg)
		&& arg >= AGENT_ARG_0 && arg <= AGENT_ARG_4) {
		item->op = AGENT_LOG_ARG;
		item->arg = arg;
		return 1;
	}

	if (err)
		*err = "bad log token";
	return 0;
}


static int agent_parse_log_list(const char *list, MR_AgentHook *h,
                                const char **err)
{
	char  buf[512];
	char *save;
	char *tok;

	h->nlog = 0;
	if (!list || !list[0]) {
		h->nlog = 1;
		/* a0 is not a parameter when nargs=0; log the call site. */
		if (h->nargs == 0) {
			h->log[0].op = AGENT_LOG_RA;
			h->log[0].arg = AGENT_ARG_RA;
		} else {
			h->log[0].op = AGENT_LOG_ARG;
			h->log[0].arg = AGENT_ARG_0;
		}
		h->log[0].off = 0;
		return 1;
	}

	_snprintf(buf, sizeof(buf), "%s", list);
	buf[sizeof(buf) - 1] = 0;
	save = NULL;
	for (tok = strtok_s(buf, ",", &save); tok;
	     tok = strtok_s(NULL, ",", &save)) {
		while (*tok == ' ' || *tok == '\t')
			tok++;
		if (!*tok)
			continue;
		if (h->nlog >= AGENT_HOOK_LOG_MAX) {
			if (err)
				*err = "too many log fields";
			return 0;
		}
		if (!agent_parse_log_token(tok, &h->log[h->nlog], err))
			return 0;
		h->nlog++;
	}
	if (h->nlog == 0) {
		if (err)
			*err = "empty log list";
		return 0;
	}
	return 1;
}


static int agent_parse_when(const char *text, MR_AgentHook *h,
                            const char **err)
{
	const char   *ne;
	const char   *eq;
	int           len;
	unsigned char arg;
	unsigned      imm;

	h->whenKind = AGENT_WHEN_ALWAYS;
	h->whenArg = AGENT_ARG_NONE;
	h->whenImm = 0;

	if (!text || !text[0] || strcmp(text, "always") == 0)
		return 1;

	ne = strstr(text, "!=");
	if (ne) {
		if (!agent_parse_arg_n(text, (int)(ne - text), &arg)
			|| arg < AGENT_ARG_0 || arg > AGENT_ARG_4) {
			if (err)
				*err = "bad when arg";
			return 0;
		}
		if (!agent_parse_u32(ne + 2, &imm)) {
			if (err)
				*err = "bad when imm";
			return 0;
		}
		h->whenKind = AGENT_WHEN_ARG_NE;
		h->whenArg = arg;
		h->whenImm = imm;
		return 1;
	}

	eq = strchr(text, '=');
	if (eq) {
		if (!agent_parse_arg_n(text, (int)(eq - text), &arg)
			|| arg < AGENT_ARG_0 || arg > AGENT_ARG_4) {
			if (err)
				*err = "bad when arg";
			return 0;
		}
		if (!agent_parse_u32(eq + 1, &imm)) {
			if (err)
				*err = "bad when imm";
			return 0;
		}
		h->whenKind = AGENT_WHEN_ARG_EQ;
		h->whenArg = arg;
		h->whenImm = imm;
		return 1;
	}

	/* aNnz */
	len = (int)strlen(text);
	if (len == 4 && tolower((unsigned char)text[2]) == 'n'
	    && tolower((unsigned char)text[3]) == 'z'
	    && agent_parse_arg_n(text, 2, &arg)
	    && arg >= AGENT_ARG_0 && arg <= AGENT_ARG_4) {
		h->whenKind = AGENT_WHEN_ARG_NZ;
		h->whenArg = arg;
		return 1;
	}

	/* bare aN means the same as aNnz */
	if (agent_parse_arg_n(text, len, &arg)
		&& arg >= AGENT_ARG_0 && arg <= AGENT_ARG_4) {
		h->whenKind = AGENT_WHEN_ARG_NZ;
		h->whenArg = arg;
		return 1;
	}

	if (err)
		*err = "bad when";
	return 0;
}


static int agent_parse_tag(const char *text, MR_AgentHook *h,
                           const char **err)
{
	int n = 0;

	h->tag[0] = 0;
	if (!text || !text[0])
		return 1;

	while (*text) {
		char c = *text++;
		int  ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
		          || (c >= '0' && c <= '9') || c == '_' || c == '.'
		          || c == '-';

		if (!ok) {
			if (err)
				*err = "bad tag char";
			return 0;
		}
		if (n >= AGENT_HOOK_TAG_MAX - 1) {
			if (err)
				*err = "tag too long";
			return 0;
		}
		h->tag[n++] = c;
	}
	h->tag[n] = 0;
	return 1;
}


/* Naming an arg at or above nargs would read the caller's stack. */
static int agent_check_nargs(const MR_AgentHook *h, const char **err)
{
	int i;

	if (h->whenKind != AGENT_WHEN_ALWAYS
	    && h->whenArg >= AGENT_ARG_0 && h->whenArg <= AGENT_ARG_4
	    && !agent_arg_live(h, h->whenArg)) {
		if (err)
			*err = "when arg >= nargs";
		return 0;
	}
	for (i = 0; i < h->nlog; i++) {
		unsigned char a = h->log[i].arg;

		if (h->log[i].op == AGENT_LOG_RA)
			continue;
		if (a >= AGENT_ARG_0 && a <= AGENT_ARG_4
		    && !agent_arg_live(h, a)) {
			if (err)
				*err = "log arg >= nargs";
			return 0;
		}
	}
	return 1;
}


/* ---- arming ------------------------------------------------------ */

static int agent_find_free_slot(void)
{
	int i;

	for (i = 0; i < AGENT_HOOK_MAX; i++) {
		if (!(s_hooks[i].flags & AGENT_HOOK_F_ARMED))
			return i;
	}
	return -1;
}


static int agent_name_in_use(const char *name)
{
	int i;

	for (i = 0; i < AGENT_HOOK_MAX; i++) {
		if ((s_hooks[i].flags & AGENT_HOOK_F_ARMED)
			&& strcmp(s_hooks[i].name, name) == 0)
		    return 1;
	}
	return 0;
}


/*
 * Hooking the agent's own code, or the logger it calls, would re-enter
 * the recorder from inside itself.
 */
static int agent_name_reserved(const char *name)
{
	return strncmp(name, "agent_", 6) == 0
	       || strncmp(name, "mrlog", 5) == 0;
}


static int agent_hook_arm(MR_AgentHook *h, int slot, char *err,
                          size_t errMax)
{
	static const unsigned char expect[5] = {
		0x66, 0x90, 0x55, 0x8b, 0xec
	};
	const MR_AgentSym *sym;
	unsigned char     *target;
	void              *resume;

	sym = agent_find(h->name, NULL);
	if (!sym) {
		_snprintf(err, errMax, "ERR SET_HOOK not in map: %s\n",
		          h->name);
		return 0;
	}

	target = agent_runtime_addr(sym);
	if (memcmp(target, expect, 5) != 0) {
		_snprintf(err, errMax,
		          "ERR SET_HOOK bad preamble at %s: %02X %02X %02X "
		          "%02X %02X (want 66 90 55 8B EC - needs a "
		          "/hotpatch Debug build)\n",
		          h->name, target[0], target[1], target[2],
		          target[3], target[4]);
		return 0;
	}

	memcpy(h->saved, target, 5);
	resume = agent_hook_install(target, (void *)s_slotFn[slot]);
	if (!resume) {
		_snprintf(err, errMax, "ERR SET_HOOK install failed: %s\n",
		          h->name);
		return 0;
	}

	h->target = target;
	h->ret = resume;
	h->slot = (unsigned char)slot;
	h->flags = (unsigned char)(h->flags | AGENT_HOOK_F_ARMED);
	return 1;
}


static void agent_hook_clear_all(void)
{
	int i;

	for (i = 0; i < AGENT_HOOK_MAX; i++) {
		MR_AgentHook *h = &s_hooks[i];

		if (h->flags & AGENT_HOOK_F_ARMED)
			agent_hook_uninstall(h->target, h->saved);
		memset(h, 0, sizeof(*h));
	}
	s_traceHead = 0;
	s_traceCount = 0;
	memset(s_trace, 0, sizeof(s_trace));
}


/* ---- the commands ------------------------------------------------ */

static void agent_reply_set(char *reply, size_t max, const char *text)
{
	if (!reply || max == 0)
		return;
	_snprintf(reply, max, "%s", text ? text : "ERR HOOK\n");
	reply[max - 1] = 0;
}


static void agent_hook_add(const char *args, char *reply, size_t max)
{
	MR_AgentHook w;
	char         logBuf[512];
	char         whenBuf[64];
	char         tagBuf[AGENT_HOOK_TAG_MAX];
	char         line[768];
	char         armErr[192];
	char        *save;
	char        *tok;
	const char  *err = NULL;
	int          haveName = 0;
	int          slot;

	if (g_symCount == 0) {
		agent_reply_set(reply, max,
		                "ERR SET_HOOK no symbols - Debug|Win32 links "
		                "the .map, Release does not\n");
		return;
	}
	if (agent_find_free_slot() < 0) {
		agent_reply_set(reply, max, "ERR SET_HOOK full\n");
		return;
	}

	memset(&w, 0, sizeof(w));
	w.whenKind = AGENT_WHEN_ALWAYS;
	w.nargs = 1;
	w.hitCap = 200;              /* bounded by default - see agent.h */
	logBuf[0] = 0;
	whenBuf[0] = 0;
	tagBuf[0] = 0;

	_snprintf(line, sizeof(line), "%s", args ? args : "");
	line[sizeof(line) - 1] = 0;
	save = NULL;
	for (tok = strtok_s(line, " \t", &save); tok;
	     tok = strtok_s(NULL, " \t", &save)) {
		char       *eq = strchr(tok, '=');
		const char *key;
		const char *val;

		if (!eq) {
			agent_reply_set(reply, max, "ERR SET_HOOK bad token\n");
			return;
		}
		*eq = 0;
		key = tok;
		val = eq + 1;

		if (strcmp(key, "name") == 0) {
			if (!val[0] || strlen(val) >= AGENT_HOOK_NAME_MAX) {
				agent_reply_set(reply, max,
				                "ERR SET_HOOK bad name\n");
				return;
			}
			_snprintf(w.name, sizeof(w.name), "%s", val);
			haveName = 1;
		} else if (strcmp(key, "nargs") == 0) {
			unsigned v;

			if (!agent_parse_u32(val, &v) || v > AGENT_HOOK_ARGS_MAX) {
				agent_reply_set(reply, max,
				                "ERR SET_HOOK bad nargs\n");
				return;
			}
			w.nargs = (unsigned char)v;
		} else if (strcmp(key, "when") == 0) {
			_snprintf(whenBuf, sizeof(whenBuf), "%s", val);
		} else if (strcmp(key, "cap") == 0) {
			unsigned v;

			if (!agent_parse_u32(val, &v) || v > 0xffff) {
				agent_reply_set(reply, max,
				                "ERR SET_HOOK bad cap\n");
				return;
			}
			w.hitCap = (unsigned short)v;
		} else if (strcmp(key, "tag") == 0) {
			_snprintf(tagBuf, sizeof(tagBuf), "%s", val);
		} else if (strcmp(key, "log") == 0) {
			_snprintf(logBuf, sizeof(logBuf), "%s", val);
		} else if (strcmp(key, "pause") == 0) {
			agent_reply_set(reply, max,
			                "ERR SET_HOOK pause not implemented\n");
			return;
		} else {
			agent_reply_set(reply, max, "ERR SET_HOOK unknown key\n");
			return;
		}
	}

	if (!haveName) {
		agent_reply_set(reply, max, "ERR SET_HOOK need name\n");
		return;
	}
	if (agent_name_reserved(w.name)) {
		agent_reply_set(reply, max,
		                "ERR SET_HOOK refusing to hook the agent's "
		                "own code\n");
		return;
	}
	if (agent_name_in_use(w.name)) {
		agent_reply_set(reply, max, "ERR SET_HOOK already hooked\n");
		return;
	}

	if (!agent_parse_when(whenBuf[0] ? whenBuf : "always", &w, &err)) {
		_snprintf(reply, max, "ERR SET_HOOK %s\n", err ? err : "when");
		reply[max - 1] = 0;
		return;
	}
	if (!agent_parse_tag(tagBuf, &w, &err)) {
		_snprintf(reply, max, "ERR SET_HOOK %s\n", err ? err : "tag");
		reply[max - 1] = 0;
		return;
	}
	if (!agent_parse_log_list(logBuf, &w, &err)) {
		_snprintf(reply, max, "ERR SET_HOOK %s\n", err ? err : "log");
		reply[max - 1] = 0;
		return;
	}
	if (!agent_check_nargs(&w, &err)) {
		_snprintf(reply, max, "ERR SET_HOOK %s\n", err ? err : "nargs");
		reply[max - 1] = 0;
		return;
	}

	slot = agent_find_free_slot();
	armErr[0] = 0;
	if (!agent_hook_arm(&w, slot, armErr, sizeof(armErr))) {
		agent_reply_set(reply, max, armErr);
		return;
	}

	s_hooks[slot] = w;
	{
		int n = 0;
		int i;

		for (i = 0; i < AGENT_HOOK_MAX; i++) {
			if (s_hooks[i].flags & AGENT_HOOK_F_ARMED)
				n++;
		}
		_snprintf(reply, max,
		          "OK SET_HOOK n=%d idx=%d name=%s addr=0x%08X\n",
		          n, slot, w.name, (unsigned)(size_t)w.target);
	}
	reply[max - 1] = 0;
}


/*
 * Body first, with room reserved at the front for the header and at the
 * end for the truncation warning, then the header is memmove'd in.
 */
static void agent_hook_format_trace(char *reply, size_t max, int clearAfter)
{
	enum { HDR_MAX = 64 };
	static const char truncTag[] = "WARN GET_TRACE truncated\n";
	size_t truncNeed = sizeof(truncTag) - 1;
	size_t used = 0;
	size_t hlen;
	char   hdr[HDR_MAX];
	int    n;
	int    idx;
	int    i;
	int    emitted = 0;
	int    truncated = 0;

	if (!reply || max < HDR_MAX + truncNeed + 8)
		return;

	n = s_traceCount;
	idx = (s_traceHead - n + AGENT_HOOK_TRACE_MAX) % AGENT_HOOK_TRACE_MAX;
	reply[0] = 0;

	for (i = 0; i < n; i++) {
		size_t len = strlen(s_trace[idx]);

		if (HDR_MAX + used + len + 1 + truncNeed >= max) {
			truncated = 1;
			break;
		}
		memcpy(reply + used, s_trace[idx], len);
		used += len;
		reply[used++] = '\n';
		reply[used] = 0;
		emitted++;
		idx = (idx + 1) % AGENT_HOOK_TRACE_MAX;
	}

	if (truncated) {
		memcpy(reply + used, truncTag, truncNeed);
		used += truncNeed;
		reply[used] = 0;
	}

	/*
	 * lines= is what the client must read after the header: the trace
	 * rows emitted, plus the WARN when there is one.
	 */
	_snprintf(hdr, sizeof(hdr), "OK GET_TRACE lines=%d%s\n",
	          emitted + (truncated ? 1 : 0),
	          truncated ? " truncated" : "");
	hdr[sizeof(hdr) - 1] = 0;
	hlen = strlen(hdr);
	memmove(reply + hlen, reply, used + 1);
	memcpy(reply, hdr, hlen);

	if (clearAfter) {
		if (truncated && emitted < n) {
			s_traceCount = n - emitted;   /* keep the remainder */
		} else {
			s_traceHead = 0;
			s_traceCount = 0;
			memset(s_trace, 0, sizeof(s_trace));
		}
	}
}


/*
 * Every hook command runs HERE, on the game thread, reached through the
 * mailbox - never on the pipe thread. Installing patches live code, and
 * draining the ring races the trampoline that writes it.
 */
static void agent_hook_command(const char *line, char *reply, size_t max)
{
	if (_strnicmp(line, "CLEAR_HOOK", 10) == 0
	    || _stricmp(line, "SET_HOOK clear") == 0) {
		agent_hook_clear_all();
		agent_reply_set(reply, max, "OK CLEAR_HOOK\n");
		return;
	}
	if (_strnicmp(line, "GET_TRACE", 9) == 0) {
		const char *p = line + 9;

		while (*p == ' ' || *p == '\t')
			p++;
		agent_hook_format_trace(reply, max,
		                        _stricmp(p, "keep") == 0 ? 0 : 1);
		return;
	}
	if (_strnicmp(line, "SET_HOOK add", 12) == 0) {
		agent_hook_add(line + 12, reply, max);
		return;
	}
	agent_reply_set(reply, max, "ERR SET_HOOK bad form\n");
}


/* ------------------------------------------------------------------ */
/* the mailbox: hook commands cross to the game thread                 */
/* ------------------------------------------------------------------ */

/*
 * THE PIPE THREAD MUST NOT PATCH CODE. If the game thread happens to be
 * executing inside the five bytes being overwritten, the process dies
 * somewhere unrelated and the cause is invisible. So SET_HOOK,
 * CLEAR_HOOK and GET_TRACE are posted here and executed by
 * agent_service() on the game thread, which by definition is not inside
 * a function entry while it is in the frame loop.
 *
 * GET_TRACE goes the same way even though it only reads: draining the
 * ring rewrites head and count, and the trampoline writes them.
 *
 * STATUS and GET_MEM stay on the pipe thread. They only read, and
 * routing them through here would stall them behind a frame.
 */

#define AGENT_MB_WAIT_MS 1500    /* under the client's 2000 ms read */

static CRITICAL_SECTION s_mbLock;
static HANDLE           s_mbDone;          /* manual reset */
static volatile LONG    s_mbPending;
static char             s_mbCmd[AGENT_INPUT_MAX];
static char             s_mbReply[AGENT_REPLY_MAX];
static int              s_mbReady;


/* Pipe thread: post one command and wait for the game thread. */
/*
 * A SEND_INPUT RUN BLOCKS FOR ITS WHOLE LENGTH, unlike every other
 * command here, so the fixed wait would fire before a long hold had
 * finished. 45 ms a frame leaves headroom over the 30 fps target.
 */
static DWORD agent_mailbox_wait_ms(const char *line)
{
	const char *f;

	if (_strnicmp(line, "SEND_INPUT", 10) != 0)
		return AGENT_MB_WAIT_MS;

	f = strstr(line, "frames=");
	if (f) {
		long n = strtol(f + 7, NULL, 0);

		if (n > 0)
			return AGENT_MB_WAIT_MS + (DWORD)n * 45;
	}
	return AGENT_MB_WAIT_MS;
}

static int agent_mailbox_call(const char *line, HANDLE pipe)
{
	DWORD wait;

	if (!s_mbReady)
		return agent_write_text(pipe,
	                            "ERR agent service not running\n");

	EnterCriticalSection(&s_mbLock);
	if (s_mbPending) {
		LeaveCriticalSection(&s_mbLock);
		return agent_write_text(pipe, "ERR hook busy\n");
	}
	ResetEvent(s_mbDone);
	_snprintf(s_mbCmd, sizeof(s_mbCmd), "%s", line);
	s_mbCmd[sizeof(s_mbCmd) - 1] = 0;
	s_mbReply[0] = 0;
	InterlockedExchange(&s_mbPending, 1);
	LeaveCriticalSection(&s_mbLock);

	wait = WaitForSingleObject(s_mbDone,
	                          agent_mailbox_wait_ms(line));
	if (wait != WAIT_OBJECT_0) {
		/*
		 * Leave pending set. The next command answers "busy" until
		 * the game thread drains it, which is self-healing; clearing
		 * it here would race a service already in progress.
		 */
		return agent_write_text(pipe,
		                        "ERR hook timed out - is the game "
		                        "in its frame loop?\n");
	}

	return agent_write_text(pipe, s_mbReply);
}


/* ------------------------------------------------------------------ */
/* driving the game: SEND_INPUT                                        */
/* ------------------------------------------------------------------ */

/*
 * There are TWO input paths in this program and SEND_INPUT covers both,
 * because they are not interchangeable:
 *
 *   HELD    race_poll_input samples GetAsyncKeyState once a frame for
 *           the driving word. keys= overrides that word for the length
 *           of a run.
 *   EDGE    the menu, and the harness keys, are WM_KEYDOWN events.
 *           tap= posts real key messages instead of duplicating any of
 *           that logic - agent_service runs on the thread that owns the
 *           window, so SendMessage lands straight in the WndProc and
 *           behaves exactly as a keypress does.
 *
 * A RUN IS: unpause, hold keys= for frames= frames, then PAUSE. The
 * game is always left stopped, so whatever is read afterwards is a
 * still frame rather than a moving target. That is the same contract
 * moto_send_input offers for the original.
 *
 * THE HELD BITS ARE THE GAME'S OWN - 1 accelerate, 2 brake, 4 left,
 * 8 right, from racer+0x31e9 - so a mask assembled here is the same
 * mask MOTO.EXE takes, which is what a future differential harness
 * needs. The names are just a readable way to spell it.
 */

#define AGENT_RUN_FRAMES_MAX 300

typedef struct {
	const char  *name;
	unsigned int bit;
} MR_AgentHeldKey;

/*
 * c is the stunt button and is not part of the word - see below.
 *
 * A BIT WITH NO NAME IN THIS TABLE IS INVISIBLE TO THE PORT SIDE,
 * and an A/B then reads as the port ignoring the input rather than
 * as the harness never sending it. MOTO takes a raw word from its
 * DLL; we take key NAMES and assemble the word here, so the two
 * sides are only comparable for bits this table knows. b and v -
 * the gear buttons, 0x10 and 0x20 - were the ones that caught it.
 */
static const MR_AgentHeldKey s_heldKeys[] = {
	{ "z",     1u },
	{ "x",     2u },
	{ "left",  4u },
	{ "right", 8u },
	{ "b",     0x10u },
	{ "v",     0x20u },
	{ "c",     0u }
};

typedef struct {
	const char *name;
	int         vk;
} MR_AgentTapKey;

static const MR_AgentTapKey s_tapKeys[] = {
	{ "up",    VK_UP },
	{ "down",  VK_DOWN },
	{ "left",  VK_LEFT },
	{ "right", VK_RIGHT },
	{ "enter", VK_RETURN },
	{ "esc",   VK_ESCAPE },
	{ "2",     '2' },           /* restart the page - MOTO uses 2 too */
	{ "f1",    '2' },           /* the old name, kept so scripts run  */
	{ "step",  VK_F5 },        /* one frame, pausing first           */
	{ "pause", VK_SPACE }      /* toggle                             */
};

static HWND         s_hwnd;
static int          s_inputActive;   /* keys= is overriding the keyboard */
static unsigned int s_inputWord;
static int          s_inputStunt;
static int          s_runActive;
static int          s_runFrames;
static int          s_runTotal;
static unsigned int s_runTickStart;


void agent_set_window(void *hwnd)
{
	s_hwnd = (HWND)hwnd;
}


int agent_input_get(unsigned int *word, int *stunt)
{
	if (!s_inputActive)
		return 0;
	if (word)
		*word = s_inputWord;
	if (stunt)
		*stunt = s_inputStunt;
	return 1;
}


/* One real keypress, through the window procedure. */
static void agent_tap(int vk)
{
	if (!s_hwnd)
		return;
	SendMessageA(s_hwnd, WM_KEYDOWN, (WPARAM)vk, 0);
	SendMessageA(s_hwnd, WM_KEYUP, (WPARAM)vk, (LPARAM)0xc0000000);
}


/*
 * Returns 1 when a run has STARTED and the reply must wait for it,
 * 0 when the reply is already written.
 */
static int agent_input_command(const char *args, char *reply, size_t max)
{
	char         line[AGENT_INPUT_MAX];
	char         keysBuf[128];
	char         tapBuf[128];
	char        *save;
	char        *tok;
	unsigned int word = 0;
	int          noClaim = 0;
	int          stunt = 0;
	int          frames = 1;
	size_t       i;

	keysBuf[0] = 0;
	tapBuf[0] = 0;

	_snprintf(line, sizeof(line), "%s", args ? args : "");
	line[sizeof(line) - 1] = 0;
	save = NULL;
	for (tok = strtok_s(line, " \t", &save); tok;
	     tok = strtok_s(NULL, " \t", &save)) {
		char       *eq = strchr(tok, '=');
		const char *key;
		const char *val;

		if (!eq) {
			agent_reply_set(reply, max, "ERR SEND_INPUT bad token\n");
			return 0;
		}
		*eq = 0;
		key = tok;
		val = eq + 1;

		if (strcmp(key, "keys") == 0) {
			_snprintf(keysBuf, sizeof(keysBuf), "%s", val);
		} else if (strcmp(key, "tap") == 0) {
			_snprintf(tapBuf, sizeof(tapBuf), "%s", val);
		} else if (strcmp(key, "frames") == 0) {
			unsigned v;

			if (!agent_parse_u32(val, &v) || v > AGENT_RUN_FRAMES_MAX) {
				_snprintf(reply, max,
				          "ERR SEND_INPUT frames must be 0..%d\n",
				          AGENT_RUN_FRAMES_MAX);
				reply[max - 1] = 0;
				return 0;
			}
			frames = (int)v;
		} else {
			agent_reply_set(reply, max,
			                "ERR SEND_INPUT unknown key\n");
			return 0;
		}
	}

	/*
	 * keys= into the driving word.
	 *
	 * keys=auto IS NOT A KEY. It means DO NOT CLAIM THE INPUT
	 * AT ALL - step the frames and let whatever normally
	 * drives the bike drive it. It exists because a third
	 * input source turned up after this file was written: the
	 * autopilot sits behind agent_input_get in
	 * race_poll_input, so a run that owns the keyboard
	 * silently SUPPRESSES it and any attempt to observe it by
	 * driving frames observes nothing at all.
	 *
	 * Everything else about a run is unchanged - the stepped
	 * clock, the bounded frame count, the pause on the way
	 * out - so this is still deterministic 33 ms a frame and
	 * is the fast way to advance a lap for inspection.
	 */
	if (keysBuf[0]) {
		char *ksave = NULL;
		char *k;

		for (k = strtok_s(keysBuf, ",", &ksave); k;
		     k = strtok_s(NULL, ",", &ksave)) {
			int found = 0;

			while (*k == ' ' || *k == '\t')
				k++;
			if (!*k)
				continue;
			if (_stricmp(k, "auto") == 0) {
				noClaim = 1;
				continue;
			}
			for (i = 0; i < sizeof(s_heldKeys) / sizeof(s_heldKeys[0]);
			     i++) {
				if (_stricmp(k, s_heldKeys[i].name) == 0) {
					if (s_heldKeys[i].bit)
						word |= s_heldKeys[i].bit;
					else
						stunt = 1;
					found = 1;
					break;
				}
			}
			if (!found) {
				_snprintf(reply, max,
				          "ERR SEND_INPUT bad key %s (z x c left "
				          "right)\n", k);
				reply[max - 1] = 0;
				return 0;
			}
		}
	}

	/* tap= applied before the run, so a menu key lands first */
	if (tapBuf[0]) {
		char *tsave = NULL;
		char *t;

		for (t = strtok_s(tapBuf, ",", &tsave); t;
		     t = strtok_s(NULL, ",", &tsave)) {
			int vk = 0;

			while (*t == ' ' || *t == '\t')
				t++;
			if (!*t)
				continue;
			for (i = 0; i < sizeof(s_tapKeys) / sizeof(s_tapKeys[0]);
			     i++) {
				if (_stricmp(t, s_tapKeys[i].name) == 0) {
					vk = s_tapKeys[i].vk;
					break;
				}
			}
			if (!vk) {
				_snprintf(reply, max,
				          "ERR SEND_INPUT bad tap %s (up down left "
				          "right enter esc f1 step pause)\n", t);
				reply[max - 1] = 0;
				return 0;
			}
			agent_tap(vk);
		}
	}

	if (frames <= 0) {
		/* Taps only. Leave the pause exactly as the taps left it. */
		_snprintf(reply, max, "OK SEND_INPUT frames=0 taps applied\n");
		reply[max - 1] = 0;
		return 0;
	}

	s_inputWord = word;
	s_inputStunt = stunt;
	/*
	 * A RUN ALWAYS OWNS THE KEYBOARD, even with no keys= - then it
	 * holds nothing. Letting the real keyboard through would mean a
	 * scripted run recorded whatever the user was leaning on, which
	 * is exactly the non-determinism this exists to remove.
	 */
	/* keys=auto: step the frames, claim nothing - see above. */
	s_inputActive = noClaim ? 0 : 1;
	s_runFrames = frames;
	s_runTotal = frames;
	s_runTickStart = race_tick_count();
	s_runActive = 1;
	/*
	 * STEP THE CLOCK, DO NOT FREE-RUN IT.
	 *
	 * tm_set_paused(0) here let the run integrate REAL elapsed time,
	 * so every scripted frame took whatever the frame happened to
	 * cost and no two runs of the same script integrated the same
	 * amount. MOTO's harness does not do that: with its virtual
	 * clock engaged the raw delta is exactly 33 ms on every frame,
	 * which is why MOTO reproduces a sixteen-frame launch to six
	 * figures and we did not - 11.4516 one run against 11.4555 the
	 * next, from an identical placement.
	 *
	 * tm_step_frames pauses and hands out ONE FIXED STEP PER FRAME
	 * from the same branch of clock_advance, and the default step is
	 * 1/(1000/33) = 33 ms - the original's, exactly. So the two sides
	 * now integrate the same time per frame as well as the same
	 * number of frames.
	 *
	 * See frame-loop.txt 1.6.
	 */
	tm_step_frames(frames);
	return 1;
}


/* One frame of an in-flight run. Ends it, and the wait, at zero. */
static void agent_run_tick(void)
{
	unsigned int word;
	int          stunt;

	if (s_runFrames > 0) {
		if (--s_runFrames > 0)
			return;

		/*
		 * PAUSE NOW, BUT KEEP HOLDING THE KEYS FOR ONE MORE TICK.
		 *
		 * agent_service runs at the TOP of a frame, so the frame in
		 * which the pause is set still ticks once: race_update reads
		 * the time elapsed since the previous frame, which is real
		 * even though the clock is now frozen. Releasing the input
		 * here would leave that last tick with NO buttons held, and
		 * for steering that is not neutral - it is a frame of active
		 * self-centring running backwards through the measurement.
		 *
		 * MEASURED: an 8-frame left hold read steer -499.994 instead
		 * of the -600.0 lock stop, and a 3-frame hold read -233.36
		 * instead of -333.36 - both exactly one centring step of
		 * roueVitesseRetour * dt = 100 away from the right answer.
		 */
		tm_set_paused(1);
		return;
	}

	/* The trailing tick has now happened; let go and answer. */
	word = s_inputWord;
	stunt = s_inputStunt;

	s_runActive = 0;
	s_inputActive = 0;
	s_inputWord = 0;
	s_inputStunt = 0;

	/*
	 * ran= IS MEASURED, frames= IS WHAT WAS ASKED FOR. Compare
	 * them: if they differ the run is not what the caller thinks
	 * it is, and any physics number taken from it is suspect.
	 */
	_snprintf(s_mbReply, sizeof(s_mbReply),
	          "OK SEND_INPUT frames=%d ran=%u word=0x%X stunt=%d paused\n",
	          s_runTotal, race_tick_count() - s_runTickStart,
	          word, stunt);
	s_mbReply[sizeof(s_mbReply) - 1] = 0;
	InterlockedExchange(&s_mbPending, 0);
	SetEvent(s_mbDone);
}



/* Defined below, with the other address-resolving commands. */
static void agent_set_mem(const char *args, char *reply, size_t max);

/* ------------------------------------------------------------------ */
/* data watchpoints                                                    */
/* ------------------------------------------------------------------ */
/*
 * WHY THIS EXISTS. A hook traps a function ENTRY, which answers "what
 * did this call see". It cannot answer "which instruction wrote this
 * field", because the writes that matter are inline stores with no
 * call to trap. MOTO has had that answer available for a long time
 * through the debugger's hardware watchpoints; the port has not, and
 * the difference cost five wrong hypotheses in a row chasing a single
 * +1.7 a frame on the airborne speed - see racers-convergence.txt 9.46.10.
 *
 * x86 GIVES FOUR DEBUG REGISTERS AND THEY ARE PER-THREAD. DR0..DR3
 * hold the addresses, DR7 the enable and length/type bits, DR6 says
 * which one fired. Arming therefore has to happen ON THE GAME THREAD,
 * which is why the commands go through the mailbox like SET_HOOK: a
 * SetThreadContext from the agent thread would arm the wrong thread
 * and report success.
 *
 * THE LENGTH FIELD IS ENCODED, NOT COUNTED: 0 means 1 byte, 1 means
 * 2, 3 means 4. There is no 3-byte watch, and an address must be
 * NATURALLY ALIGNED to its length or the processor's behaviour is
 * undefined - so a misaligned request is refused rather than rounded.
 * MR_Racer is byte-packed, so most fields in it need length 1.
 *
 * THE TRAP FIRES AFTER THE STORE RETIRES, so the recorded EIP is the
 * instruction AFTER the one that wrote. That is the same convention
 * MOTO's side reports and the reason a hit reading eip=X was written
 * by the instruction ENDING at X.
 */
#define AGENT_DBP_MAX 4

typedef struct {
	int           armed;
	unsigned char *addr;
	int           len;       /* 1, 2 or 4                          */
	unsigned int  cap;       /* auto-disarm after this many hits   */
	unsigned int  hits;
	int           changed;   /* only record when the value moved   */
	unsigned int  last;      /* previous value, for `changed`      */
	char          tag[16];
} MR_AgentDbp;

static MR_AgentDbp s_dbp[AGENT_DBP_MAX];
static void       *s_dbpVeh;

static unsigned int agent_dbp_read(const MR_AgentDbp *d)
{
	unsigned int v = 0;

	if (IsBadReadPtr(d->addr, (UINT_PTR)d->len))
		return 0;
	memcpy(&v, d->addr, (size_t)d->len);
	return v;
}


/*
 * Push DR0..DR3 and DR7 onto the CALLING thread. Rebuilt from the
 * slot table every time rather than edited in place, so a disarm
 * cannot leave a stale enable bit behind.
 */
static void agent_dbp_apply(void)
{
	CONTEXT ctx;
	DWORD   dr7 = 0;
	int     i;

	memset(&ctx, 0, sizeof(ctx));
	ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
	if (!GetThreadContext(GetCurrentThread(), &ctx))
		return;

	ctx.Dr0 = ctx.Dr1 = ctx.Dr2 = ctx.Dr3 = 0;

	for (i = 0; i < AGENT_DBP_MAX; i++) {
		DWORD lenBits;

		if (!s_dbp[i].armed)
			continue;

		switch (i) {
		case 0: ctx.Dr0 = (DWORD_PTR)s_dbp[i].addr; break;
		case 1: ctx.Dr1 = (DWORD_PTR)s_dbp[i].addr; break;
		case 2: ctx.Dr2 = (DWORD_PTR)s_dbp[i].addr; break;
		default: ctx.Dr3 = (DWORD_PTR)s_dbp[i].addr; break;
		}

		lenBits = (s_dbp[i].len == 4) ? 3u
		        : (s_dbp[i].len == 2) ? 1u : 0u;

		/* local enable for the slot */
		dr7 |= (DWORD)(1u << (i * 2));
		/* RW = 01, a write watch; LEN in the next two bits */
		dr7 |= (DWORD)(1u << (16 + i * 4));
		dr7 |= (DWORD)(lenBits << (18 + i * 4));
	}

	ctx.Dr6 = 0;
	ctx.Dr7 = dr7;
	ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
	SetThreadContext(GetCurrentThread(), &ctx);
}


static LONG CALLBACK agent_dbp_veh(EXCEPTION_POINTERS *ep)
{
	DWORD dr6;
	int   i;

	if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
		return EXCEPTION_CONTINUE_SEARCH;

	dr6 = (DWORD)ep->ContextRecord->Dr6;
	if ((dr6 & 0xfu) == 0)
		return EXCEPTION_CONTINUE_SEARCH;

	for (i = 0; i < AGENT_DBP_MAX; i++) {
		MR_AgentDbp *d = &s_dbp[i];
		unsigned int now;
		char         line[AGENT_HOOK_LINE_MAX];
		char         sym[AGENT_HOOK_NAME_MAX + 16];

		if (!(dr6 & (1u << i)) || !d->armed)
			continue;

		now = agent_dbp_read(d);
		if (d->changed && d->hits && now == d->last) {
			d->last = now;
			continue;
		}

		agent_format_code_addr((void *)ep->ContextRecord->Eip,
		                       sym, (int)sizeof(sym));
		_snprintf(line, sizeof(line),
		          "[%s] databp s%d hits=%u eip=%08X fn=%s "
		          "old=%08X new=%08X",
		          d->tag[0] ? d->tag : "dbp", i, d->hits + 1,
		          (unsigned)ep->ContextRecord->Eip, sym,
		          d->last, now);
		line[sizeof(line) - 1] = 0;

		_snprintf(s_trace[s_traceHead], AGENT_HOOK_LINE_MAX,
		          "%s", line);
		s_trace[s_traceHead][AGENT_HOOK_LINE_MAX - 1] = 0;
		s_traceHead = (s_traceHead + 1) % AGENT_HOOK_TRACE_MAX;
		if (s_traceCount < AGENT_HOOK_TRACE_MAX)
			s_traceCount++;

		d->last = now;
		d->hits++;
		if (d->cap && d->hits >= d->cap) {
			d->armed = 0;
			agent_dbp_apply();
		}
	}

	/* DR6 is sticky - the processor never clears it for us. */
	ep->ContextRecord->Dr6 = 0;
	return EXCEPTION_CONTINUE_EXECUTION;
}


static void agent_dbp_command(const char *args, char *reply, int max)
{
	unsigned int  addr = 0;
	int           len = 1;
	unsigned int  cap = 8;
	int           changed = 0;
	int           clear = 0;
	char          tag[16];
	const char   *p = args;
	int           slot;

	tag[0] = 0;

	while (*p) {
		while (*p == ' ' || *p == '\t')
			p++;
		if (!*p)
			break;
		if (_strnicmp(p, "clear", 5) == 0) {
			clear = 1;
			p += 5;
		} else if (_strnicmp(p, "addr=", 5) == 0) {
			addr = (unsigned int)strtoul(p + 5, (char **)&p, 16);
		} else if (_strnicmp(p, "len=", 4) == 0) {
			len = (int)strtol(p + 4, (char **)&p, 10);
		} else if (_strnicmp(p, "cap=", 4) == 0) {
			cap = (unsigned int)strtoul(p + 4, (char **)&p, 10);
		} else if (_strnicmp(p, "changed", 7) == 0) {
			changed = 1;
			p += 7;
		} else if (_strnicmp(p, "tag=", 4) == 0) {
			int i = 0;

			p += 4;
			while (*p && *p != ' ' && i < (int)sizeof(tag) - 1)
				tag[i++] = *p++;
			tag[i] = 0;
		} else {
			agent_reply_set(reply, max,
			                "ERR SET_DATABP unknown key\n");
			return;
		}
	}

	if (clear) {
		memset(s_dbp, 0, sizeof(s_dbp));
		agent_dbp_apply();
		agent_reply_set(reply, max, "OK SET_DATABP clear\n");
		return;
	}

	if (!addr) {
		agent_reply_set(reply, max, "ERR SET_DATABP need addr=\n");
		return;
	}
	if (len != 1 && len != 2 && len != 4) {
		agent_reply_set(reply, max, "ERR SET_DATABP len must be "
		                            "1, 2 or 4\n");
		return;
	}
	if (addr % (unsigned)len) {
		agent_reply_set(reply, max, "ERR SET_DATABP addr must be "
		                            "aligned to len - x86 leaves an "
		                            "unaligned watch undefined\n");
		return;
	}

	for (slot = 0; slot < AGENT_DBP_MAX; slot++)
		if (!s_dbp[slot].armed)
			break;
	if (slot == AGENT_DBP_MAX) {
		agent_reply_set(reply, max, "ERR SET_DATABP all four slots "
		                            "in use\n");
		return;
	}

	if (!s_dbpVeh) {
		s_dbpVeh = AddVectoredExceptionHandler(1, agent_dbp_veh);
		if (!s_dbpVeh) {
			agent_reply_set(reply, max, "ERR SET_DATABP no "
			                            "vectored handler\n");
			return;
		}
	}

	memset(&s_dbp[slot], 0, sizeof(s_dbp[slot]));
	s_dbp[slot].addr    = (unsigned char *)(UINT_PTR)addr;
	s_dbp[slot].len     = len;
	s_dbp[slot].cap     = cap;
	s_dbp[slot].changed = changed;
	s_dbp[slot].armed   = 1;
	_snprintf(s_dbp[slot].tag, sizeof(s_dbp[slot].tag), "%s", tag);
	s_dbp[slot].last    = agent_dbp_read(&s_dbp[slot]);

	agent_dbp_apply();

	_snprintf(reply, max,
	          "OK SET_DATABP slot=%d addr=0x%08X len=%d cap=%u "
	          "init=%08X\n",
	          slot, addr, len, cap, s_dbp[slot].last);
}


void agent_service(void)
{
	if (!s_mbReady)
		return;

	/* A run in flight owns the mailbox until its last frame. */
	if (s_runActive) {
		agent_run_tick();
		return;
	}

	if (!s_mbPending)
		return;

	if (_strnicmp(s_mbCmd, "SEND_INPUT", 10) == 0) {
		if (agent_input_command(s_mbCmd + 10, s_mbReply,
		                        sizeof(s_mbReply)))
		    return;      /* started; it replies when it ends */
	} else if (_strnicmp(s_mbCmd, "SET_MEM", 7) == 0) {
		agent_set_mem(s_mbCmd + 7, s_mbReply, sizeof(s_mbReply));
	} else if (_strnicmp(s_mbCmd, "SET_DATABP", 10) == 0) {
		/*
		 * ON THE GAME THREAD, and that is the whole reason this
		 * goes through the mailbox: DR0..DR7 are per-thread, so
		 * arming from the agent thread would watch the wrong one
		 * and report success.
		 */
		agent_dbp_command(s_mbCmd + 10, s_mbReply,
		                  sizeof(s_mbReply));
	} else {
		agent_hook_command(s_mbCmd, s_mbReply, sizeof(s_mbReply));
	}

	InterlockedExchange(&s_mbPending, 0);
	SetEvent(s_mbDone);
}


/* ------------------------------------------------------------------ */
/* commands                                                            */
/* ------------------------------------------------------------------ */

/*
 * Copy under SEH. The caller is trusted to know the size of what it
 * asked for (agent.h), but turning a wild offset into one error line
 * beats losing the process and the session with it.
 */
static int agent_copy_guarded(void *dst, const void *src, size_t n)
{
	__try {
		memcpy(dst, src, n);
		return 1;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return 0;
	}
}


/* Value of key= in a "k=v ..." line, or NULL. Written into out. */
static int agent_arg(const char *line, const char *key, char *out, int size)
{
	size_t      klen = strlen(key);
	const char *p = line;

	out[0] = 0;
	while (*p) {
		while (*p == ' ' || *p == '\t')
			p++;
		if (!*p)
			break;
		if (_strnicmp(p, key, klen) == 0 && p[klen] == '=') {
			const char *v = p + klen + 1;
			int         i = 0;

			while (*v && *v != ' ' && *v != '\t' && i < size - 1)
				out[i++] = *v++;
			out[i] = 0;
			return 1;
		}
		while (*p && *p != ' ' && *p != '\t')
			p++;
	}
	return 0;
}


static int agent_cmd_get_mem(HANDLE pipe, const char *args)
{
	static char        reply[AGENT_REPLY_MAX];
	static unsigned char buf[AGENT_MEM_MAX];
	char               name[64];
	char               obj[64];
	char               num[32];
	const MR_AgentSym *sym = NULL;
	unsigned char     *addr;
	long               offset = 0;
	long               len;
	long               i;
	int                pos;
	int                hasAddr;
	unsigned long      raw = 0;

	/*
	 * addr= READS A RAW ADDRESS, and it exists because half the
	 * interesting data is not a symbol. A mesh keeps its vertices on
	 * the heap behind a pointer, so name= can reach the MESH header
	 * and never the vertices - which blocked comparing our bike
	 * geometry against the original's until this was added.
	 *
	 * IT IS AS UNCHECKED AS name= IS, deliberately. The read is
	 * guarded the same way and a bad address returns "unreadable"
	 * rather than crashing; beyond that it is the caller's business,
	 * which is the same bargain the rest of this command makes.
	 *
	 * offset= still applies, so a pointer read out of one struct can
	 * be indexed directly.
	 */
	hasAddr = agent_arg(args, "addr", num, sizeof(num)) && num[0];
	if (hasAddr)
		raw = (unsigned long)strtoul(num, NULL, 16);
	else if (!agent_arg(args, "name", name, sizeof(name)) || !name[0])
		return agent_write_text(pipe,
	                            "ERR GET_MEM needs name= or addr=\n");

	if (!agent_arg(args, "len", num, sizeof(num)) || !num[0])
		return agent_write_text(pipe, "ERR GET_MEM needs len=\n");
	len = strtol(num, NULL, 0);
	if (len < 1 || len > AGENT_MEM_MAX) {
		_snprintf(reply, sizeof(reply),
		          "ERR len out of range (1..%d)\n", AGENT_MEM_MAX);
		return agent_write_text(pipe, reply);
	}

	if (agent_arg(args, "offset", num, sizeof(num)) && num[0])
		offset = strtol(num, NULL, 0);

	if (hasAddr) {
		addr = (unsigned char *)(size_t)raw + offset;
	} else {
		agent_arg(args, "obj", obj, sizeof(obj));

		sym = agent_find(name, obj);
	if (!sym) {
		if (obj[0] && agent_count_named(name) > 0) {
			_snprintf(reply, sizeof(reply),
				      "ERR no symbol %s in %s (%d other match(es))\n",
				      name, obj, agent_count_named(name));
		} else {
			_snprintf(reply, sizeof(reply),
				      "ERR no symbol %s\n", name);
		}
		return agent_write_text(pipe, reply);
	}

		addr = agent_runtime_addr(sym) + offset;
	}
	if (!agent_copy_guarded(buf, addr, (size_t)len)) {
		_snprintf(reply, sizeof(reply),
		          "ERR unreadable at 0x%08X len %ld\n",
		          (unsigned)(size_t)addr, len);
		return agent_write_text(pipe, reply);
	}

	pos = _snprintf(reply, sizeof(reply),
	                "OK name=%s obj=%s addr=0x%08X len=%ld lines=1\n",
	                sym ? sym->name : "-",
	            (sym && sym->obj[0]) ? sym->obj : "-",
	                (unsigned)(size_t)addr, len);

	for (i = 0; i < len; i++) {
		static const char hex[] = "0123456789abcdef";

		reply[pos++] = hex[buf[i] >> 4];
		reply[pos++] = hex[buf[i] & 0x0f];
	}
	reply[pos++] = '\n';

	return agent_write(pipe, reply, (DWORD)pos);
}


/* ------------------------------------------------------------------ */
/*
 * SET_MEM - write bytes. The mirror of GET_MEM, same address forms.
 *
 *     SET_MEM name=<sym> [offset=<n>] [obj=<file.obj>] value=<hex>
 *     SET_MEM addr=<hex> [offset=<n>] value=<hex>
 *
 * IT EXISTS TO PUT THE BIKE SOMEWHERE. Comparing our wall response
 * against MOTO.EXE means striking the same wall at the same angle
 * and the same speed on both sides, and driving there by hand
 * arrives somewhere slightly different every run. tools/mr_place.py
 * already does this to MOTO through the hook DLL's own SET_MEM;
 * this is the other half, so one script can place both.
 *
 * IT RUNS ON THE GAME THREAD, unlike GET_MEM. A position is twelve
 * bytes and an orientation forty-eight, and a write landing either
 * side of a frame boundary leaves the bike half moved. The mailbox
 * makes it atomic with respect to the tick.
 *
 * IT IS AS UNCHECKED AS GET_MEM IS, and for the same reason: the
 * write is guarded, so a wild address costs one error line rather
 * than the session, and what the bytes MEAN is the caller's
 * business. There is no code-patching case here - this is our own
 * build, and a function that needs to behave differently gets an
 * ini setting instead.
 */
#define AGENT_SET_MAX 128        /* the 512-byte input line bounds it */

static int agent_hex_digit(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}


static void agent_set_mem(const char *args, char *reply, size_t max)
{
	static const char  digits[] = "0123456789abcdef";
	unsigned char      val[AGENT_SET_MAX];
	unsigned char      was[AGENT_SET_MAX];
	char               hex[AGENT_SET_MAX * 2 + 8];
	char               name[128];
	char               obj[64];
	char               num[32];
	const MR_AgentSym *sym = NULL;
	unsigned char     *addr;
	long               offset = 0;
	long               len;
	long               i;
	int                pos;
	int                hasAddr;
	unsigned long      raw = 0;

	name[0] = 0;
	obj[0]  = 0;

	hasAddr = agent_arg(args, "addr", num, sizeof(num))
	          && num[0];
	if (hasAddr) {
		raw = (unsigned long)strtoul(num, NULL, 16);
	} else if (!agent_arg(args, "name", name, sizeof(name))
	           || !name[0]) {
		agent_reply_set(reply, max,
		                "ERR SET_MEM needs name= or addr=\n");
		return;
	}

	if (!agent_arg(args, "value", hex, sizeof(hex)) || !hex[0]) {
		agent_reply_set(reply, max,
		                "ERR SET_MEM needs value=\n");
		return;
	}

	len = (long)strlen(hex);
	if ((len & 1) != 0 || len < 2 || len > AGENT_SET_MAX * 2) {
		agent_reply_set(reply, max,
		                "ERR value must be an even 2..256 hex digits\n");
		return;
	}
	len /= 2;

	for (i = 0; i < len; i++) {
		int hi = agent_hex_digit(hex[i * 2]);
		int lo = agent_hex_digit(hex[i * 2 + 1]);

		if (hi < 0 || lo < 0) {
			agent_reply_set(reply, max,
			                "ERR value is not hex\n");
			return;
		}
		val[i] = (unsigned char)((hi << 4) | lo);
	}

	if (agent_arg(args, "offset", num, sizeof(num)) && num[0])
		offset = strtol(num, NULL, 0);

	if (hasAddr) {
		addr = (unsigned char *)(size_t)raw + offset;
	} else {
		agent_arg(args, "obj", obj, sizeof(obj));

		sym = agent_find(name, obj);
		if (!sym) {
			char err[192];

			_snprintf(err, sizeof(err),
			          "ERR no symbol %s\n", name);
			agent_reply_set(reply, max, err);
			return;
		}
		addr = agent_runtime_addr(sym) + offset;
	}

	/* Read first, so the reply carries the undo. */
	if (!agent_copy_guarded(was, addr, (size_t)len)
		|| !agent_copy_guarded(addr, val, (size_t)len)) {
		char err[128];

		_snprintf(err, sizeof(err),
		          "ERR unwritable at 0x%08X len %ld\n",
		          (unsigned)(size_t)addr, len);
		agent_reply_set(reply, max, err);
		return;
	}

	pos = _snprintf(reply, max,
	                "OK SET_MEM name=%s addr=0x%08X len=%ld was=",
	                sym ? sym->name : "-",
	                (unsigned)(size_t)addr, len);

	for (i = 0; i < len; i++) {
		reply[pos++] = digits[was[i] >> 4];
		reply[pos++] = digits[was[i] & 0x0f];
	}
	reply[pos++] = '\n';
	reply[pos]   = 0;
}

static int agent_cmd_status(HANDLE pipe)
{
	char reply[128];

	_snprintf(reply, sizeof(reply),
	          "OK symbols=%d base=0x%08X relocated=%d\n",
	          g_symCount, (unsigned)g_modBase, g_relocated);
	return agent_write_text(pipe, reply);
}


static int agent_process_line(HANDLE pipe, char *line)
{
	char *p = line;

	while (*p == ' ' || *p == '\t')
		p++;
	if (!*p)
		return 1;
	if (p[strlen(p) - 1] == '\r')
		p[strlen(p) - 1] = 0;

	/* Hook commands cross to the game thread; reads do not. */
	if (_strnicmp(p, "SET_HOOK", 8) == 0
	    || _strnicmp(p, "CLEAR_HOOK", 10) == 0
	    || _strnicmp(p, "GET_TRACE", 9) == 0
	    || _strnicmp(p, "SET_MEM", 7) == 0
	    || _strnicmp(p, "SEND_INPUT", 10) == 0
	    || _strnicmp(p, "SET_DATABP", 10) == 0)
	    return agent_mailbox_call(p, pipe);

	if (_strnicmp(p, "GET_MEM", 7) == 0)
		return agent_cmd_get_mem(pipe, p + 7);
	if (_strnicmp(p, "STATUS", 6) == 0)
		return agent_cmd_status(pipe);

	return agent_write_text(pipe, "ERR unknown command\n");
}


/* ------------------------------------------------------------------ */
/* the thread                                                          */
/* ------------------------------------------------------------------ */

static DWORD WINAPI agent_thread(void *param)
{
	HANDLE pipe;
	char   input[AGENT_INPUT_MAX];
	DWORD  inputLen = 0;
	int    connected = 0;

	(void)param;

	pipe = CreateNamedPipeA(AGENT_PIPE_NAME, PIPE_ACCESS_DUPLEX,
	                        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE
	                        | PIPE_NOWAIT | PIPE_REJECT_REMOTE_CLIENTS,
	                        1, 128 * 1024, 128 * 1024, 0, NULL);
	if (pipe == INVALID_HANDLE_VALUE) {
		mrlog("agent:  CreateNamedPipe failed (%lu): %s\n",
		      (unsigned long)GetLastError(), AGENT_PIPE_NAME);
		return 1;
	}

	mrlog("agent:  listening on %s\n", AGENT_PIPE_NAME);

	while (WaitForSingleObject(g_stopEvent, 0) != WAIT_OBJECT_0) {
		DWORD available = 0;

		if (!connected) {
			if (ConnectNamedPipe(pipe, NULL)) {
				connected = 1;
				inputLen = 0;
			} else {
				DWORD err = GetLastError();

				if (err == ERROR_PIPE_CONNECTED) {
					connected = 1;
					inputLen = 0;
				} else if (err != ERROR_PIPE_LISTENING
				           && err != ERROR_NO_DATA) {
					mrlog("agent:  ConnectNamedPipe failed (%lu)\n",
					      (unsigned long)err);
					break;
				}
			}
			if (connected)
				mrlog("agent:  client connected\n");
			WaitForSingleObject(g_stopEvent, 25);
			continue;
		}

		if (!PeekNamedPipe(pipe, NULL, 0, NULL, &available, NULL)) {
			DisconnectNamedPipe(pipe);
			connected = 0;
			inputLen = 0;
			mrlog("agent:  client disconnected\n");
		} else if (available > 0) {
			char  buf[256];
			DWORD want;
			DWORD got = 0;
			char *nl;

			want = available < (DWORD)sizeof(buf)
			       ? available : (DWORD)sizeof(buf);
			if (!ReadFile(pipe, buf, want, &got, NULL)) {
				DisconnectNamedPipe(pipe);
				connected = 0;
				inputLen = 0;
			} else if (inputLen + got >= sizeof(input)) {
				inputLen = 0;
				if (!agent_write_text(pipe, "ERR command too long\n")) {
					DisconnectNamedPipe(pipe);
					connected = 0;
				}
			} else {
				memcpy(input + inputLen, buf, got);
				inputLen += got;
				input[inputLen] = 0;

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

		WaitForSingleObject(g_stopEvent, 25);
	}

	if (connected)
		DisconnectNamedPipe(pipe);
	CloseHandle(pipe);
	mrlog("agent:  stopped\n");
	return 0;
}


int agent_start(void)
{
	if (g_started)
		return 1;

	agent_parse_map();

	/* The mailbox must exist before the pipe thread can post. */
	InitializeCriticalSection(&s_mbLock);
	s_mbDone = CreateEventA(NULL, TRUE, FALSE, NULL);
	g_stopEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
	if (!g_stopEvent || !s_mbDone) {
		if (s_mbDone)
			CloseHandle(s_mbDone);
		if (g_stopEvent)
			CloseHandle(g_stopEvent);
		DeleteCriticalSection(&s_mbLock);
		s_mbDone = NULL;
		g_stopEvent = NULL;
		return 0;
	}
	s_mbReady = 1;

	g_thread = CreateThread(NULL, 0, agent_thread, NULL, 0, NULL);
	if (!g_thread) {
		s_mbReady = 0;
		CloseHandle(s_mbDone);
		CloseHandle(g_stopEvent);
		DeleteCriticalSection(&s_mbLock);
		s_mbDone = NULL;
		g_stopEvent = NULL;
		return 0;
	}

	g_started = 1;
	return 1;
}


void agent_stop(void)
{
	if (!g_started)
		return;

	SetEvent(g_stopEvent);
	WaitForSingleObject(g_thread, INFINITE);
	CloseHandle(g_thread);

	/*
	 * RESTORE EVERY PATCHED PROLOGUE BEFORE THE SYMBOLS GO.
	 * Leaving a hook armed past here would jump into a
	 * trampoline whose table has been freed. Safe to do on this
	 * thread: agent_stop runs from WinMain after the frame loop
	 * has ended, so nothing is executing a hooked function.
	 */
	s_mbReady = 0;
	s_runActive = 0;
	s_inputActive = 0;
	agent_hook_clear_all();
	DeleteCriticalSection(&s_mbLock);
	CloseHandle(s_mbDone);
	s_mbDone = NULL;
	CloseHandle(g_stopEvent);
	g_thread = NULL;
	g_stopEvent = NULL;
	g_started = 0;

	free(g_syms);
	g_syms = NULL;
	g_symCount = 0;
}
