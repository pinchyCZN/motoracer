#!/usr/bin/env python3
"""
bridge_mcp_mrhook.py - an MCP stdio server for BOTH agents.

    python tools/bridge_mcp_mrhook.py                serve MCP on stdio
    python tools/bridge_mcp_mrhook.py --cli CMD         one MOTO command
    python tools/bridge_mcp_mrhook.py --cli-mracer CMD  one port command

TWO TARGETS, TWO PIPES, ONE SERVER. Every tool is named for the
process it reaches, so there is no way to read the wrong one by
accident:

    moto_*      the original, MOTO.EXE, via mrhook
    mracer_*    our port, mracer.exe, via src/agent.c

The names use an underscore rather than the "moto:get_mem" form
because MCP tool names are composed into mcp__<server>__<tool> by the
client, and a colon does not survive that.

mrhook (tools/mrhook/mrhook.c) runs inside MOTO.EXE and listens on
\\\\.\\pipe\\mrhook. One line in, one line out:

    STATUS                  -> OK STATUS base=.. delta=.. page=.. ...
    GET_MEM <hexaddr> <n>   -> OK GET_MEM addr=.. size=.. <hex bytes>

The port agent (src/agent.h has the protocol) listens on
\\\\.\\pipe\\mracer_agent and answers GET_MEM with TWO lines, a
header and then the hex. It reads symbols BY NAME out of the linker
.map, which only Debug|Win32 links - under Release it reports
symbols=0.

Either way, any failure comes back as a single ERR line.

This file is the bridge from that pipe to MCP tools. It has NO
dependencies - the JSON-RPC is hand-rolled and the pipe is reached
through ctypes - so it cannot break because a package moved.

WHY OVERLAPPED I/O AND NOT open(). A plain open() on a named pipe
works, but there is no way to bound a read on it. If the game wedges,
a blocking readline() parks this process, MCP stops answering, and the
failure presents as "the server died" rather than "the game is busy".
Overlapped I/O plus CancelIoEx is the only way to get a timeout, and a
timeout is the whole difference between a diagnosable and an opaque
failure.

--cli EXISTS SO THE PIPE CAN BE TESTED WITHOUT MCP AT ALL. Debug the
transport first, then the protocol; the two failure modes look alike
from inside Claude Code and nothing else separates them.
"""

import ctypes
import json
import os
import struct
import sys
import time
from ctypes import wintypes

PIPE_NAME = r"\\.\pipe\mrhook"
MRACER_PIPE_NAME = r"\\.\pipe\mracer_agent"
PIPE_TIMEOUT_MS = 2000
MAX_LINE = 1 << 20
MAX_SIZE = 4096
SET_MEM_MAX = 64
MRACER_MAX_SIZE = 32768
TEXT_LO = 0x00401000
TEXT_HI = 0x004cf5ff

SERVER_NAME = "mrhook"
SERVER_VERSION = "1.0.0"

# From mcp/types.py SUPPORTED_PROTOCOL_VERSIONS. The client's version is
# echoed when we recognise it, which is what the reference server does;
# hardcoding one is how a bridge stops working after a client update.
KNOWN_VERSIONS = ("2024-11-05", "2025-03-26", "2025-06-18", "2025-11-25")
FALLBACK_VERSION = "2024-11-05"

PARSE_ERROR = -32700
INVALID_REQUEST = -32600
METHOD_NOT_FOUND = -32601
INVALID_PARAMS = -32602
INTERNAL_ERROR = -32603

_OUT = None


def log(fmt, *args):
    """Diagnostics go to stderr. Claude Code captures it; stdout is
    reserved for JSON-RPC and one stray byte desynchronises it."""
    try:
        sys.stderr.write("mrhook-mcp: " + (fmt % args) + "\n")
        sys.stderr.flush()
    except Exception:
        pass


class PipeDown(Exception):
    """The game is not running, or mrhook is not loaded. Expected, and
    reported to the model rather than raised as a protocol fault."""


# ------------------------------------------------------------------ #
# The named pipe, through ctypes                                      #
# ------------------------------------------------------------------ #

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
OPEN_EXISTING = 3
FILE_FLAG_OVERLAPPED = 0x40000000
ERROR_FILE_NOT_FOUND = 2
ERROR_BROKEN_PIPE = 109
ERROR_PIPE_BUSY = 231
ERROR_IO_PENDING = 997
WAIT_OBJECT_0 = 0
WAIT_TIMEOUT = 258
INFINITE = 0xFFFFFFFF
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value


class OVERLAPPED(ctypes.Structure):
    _fields_ = [
        ("Internal", ctypes.c_size_t),
        ("InternalHigh", ctypes.c_size_t),
        ("Offset", wintypes.DWORD),
        ("OffsetHigh", wintypes.DWORD),
        ("hEvent", wintypes.HANDLE),
    ]


# Every argtype is declared on purpose: on 64-bit Python an undeclared
# HANDLE return is truncated to 32 bits and everything downstream fails
# in a way that looks like a pipe bug.
kernel32.CreateFileW.argtypes = [
    wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
    wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE,
]
kernel32.CreateFileW.restype = wintypes.HANDLE
kernel32.WaitNamedPipeW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD]
kernel32.WaitNamedPipeW.restype = wintypes.BOOL
kernel32.CreateEventW.argtypes = [
    ctypes.c_void_p, wintypes.BOOL, wintypes.BOOL, wintypes.LPCWSTR,
]
kernel32.CreateEventW.restype = wintypes.HANDLE
kernel32.WriteFile.argtypes = [
    wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD,
    ctypes.POINTER(wintypes.DWORD), ctypes.POINTER(OVERLAPPED),
]
kernel32.WriteFile.restype = wintypes.BOOL
kernel32.ReadFile.argtypes = kernel32.WriteFile.argtypes
kernel32.ReadFile.restype = wintypes.BOOL
kernel32.GetOverlappedResult.argtypes = [
    wintypes.HANDLE, ctypes.POINTER(OVERLAPPED),
    ctypes.POINTER(wintypes.DWORD), wintypes.BOOL,
]
kernel32.GetOverlappedResult.restype = wintypes.BOOL
kernel32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
kernel32.WaitForSingleObject.restype = wintypes.DWORD
kernel32.CancelIoEx.argtypes = [
    wintypes.HANDLE, ctypes.POINTER(OVERLAPPED),
]
kernel32.CancelIoEx.restype = wintypes.BOOL
kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
kernel32.CloseHandle.restype = wintypes.BOOL


def win_error(prefix):
    err = ctypes.get_last_error()
    return OSError("%s failed (%d): %s"
                   % (prefix, err, ctypes.FormatError(err)))


# Everything that differs between the two agents, in one place.
TARGETS = {
    "moto": {
        "pipe": PIPE_NAME,
        "hint": "the game is not running, or mrhook is not deployed "
                "as dplayx.dll beside MOTO.EXE",
    },
    "mracer": {
        "pipe": MRACER_PIPE_NAME,
        "hint": "our port is not running, or it was linked without a "
                ".map beside the .exe - both Debug|Win32 and "
                "Release|Win32 write one",
    },
}


def open_pipe(timeout_ms=PIPE_TIMEOUT_MS, target="moto"):
    """Connect, waiting out another client rather than giving up.

    THE PIPE HAS ONE INSTANCE, so two tools cannot be connected at
    once - and tools/mr_freeze.py is designed to be left running while
    other commands are issued. This used to try three times and raise,
    which lost roughly one command in four under that load and killed
    a 4-call snapshot outright.

    WaitNamedPipe ALONE IS NOT ENOUGH. The agent runs a PIPE_NOWAIT
    loop and only calls ConnectNamedPipe on its 25 ms poll, so between
    one client disconnecting and the server re-listening there is a
    window with no instance queued for Wait to hand over. Retrying
    around a deadline covers that window; a fixed attempt count does
    not.
    """
    spec = TARGETS[target]
    pipe_name = spec["pipe"]
    deadline = time.time() + timeout_ms / 1000.0
    while True:
        handle = kernel32.CreateFileW(
            pipe_name, GENERIC_READ | GENERIC_WRITE, 0, None,
            OPEN_EXISTING, FILE_FLAG_OVERLAPPED, None)
        if handle and handle != INVALID_HANDLE_VALUE:
            return handle
        err = ctypes.get_last_error()
        if err == ERROR_FILE_NOT_FOUND:
            raise PipeDown(
                "no pipe at %s - %s" % (pipe_name, spec["hint"]))
        if err != ERROR_PIPE_BUSY:
            raise win_error("CreateFileW")
        if time.time() >= deadline:
            raise PipeDown(
                "pipe busy: another client held it for %d ms" % timeout_ms)
        kernel32.WaitNamedPipeW(pipe_name, 50)
        time.sleep(0.02)


def _wait_io(handle, ov, got, op, timeout_ms):
    """Wait for an overlapped operation, cancelling it on timeout.

    THE CANCEL PATH MATTERS. Returning without CancelIoEx plus a wait
    would leave the kernel writing into a buffer Python is about to
    collect."""
    rc = kernel32.WaitForSingleObject(ov.hEvent, timeout_ms)
    if rc == WAIT_TIMEOUT:
        kernel32.CancelIoEx(handle, ctypes.byref(ov))
        kernel32.WaitForSingleObject(ov.hEvent, INFINITE)
        raise PipeDown("%s timed out after %d ms - the game may be "
                       "paused or minimised" % (op, timeout_ms))
    if rc != WAIT_OBJECT_0:
        raise win_error("WaitForSingleObject")
    if not kernel32.GetOverlappedResult(handle, ctypes.byref(ov),
                                        ctypes.byref(got), False):
        if ctypes.get_last_error() == ERROR_BROKEN_PIPE:
            got.value = 0
            return
        raise win_error("GetOverlappedResult")


def _overlapped():
    ov = OVERLAPPED()
    ov.hEvent = kernel32.CreateEventW(None, True, False, None)
    if not ov.hEvent:
        raise win_error("CreateEventW")
    return ov


def write_all(handle, data, timeout_ms):
    """One WriteFile for the whole command INCLUDING its newline - if
    the server ever becomes a message-mode pipe, splitting the two
    would deliver a command with no terminator."""
    ov = _overlapped()
    try:
        got = wintypes.DWORD(0)
        ok = kernel32.WriteFile(handle, data, len(data),
                                ctypes.byref(got), ctypes.byref(ov))
        if not ok:
            if ctypes.get_last_error() != ERROR_IO_PENDING:
                raise win_error("WriteFile")
            _wait_io(handle, ov, got, "write", timeout_ms)
        if got.value != len(data):
            raise PipeDown("short write: %d of %d bytes"
                           % (got.value, len(data)))
    finally:
        kernel32.CloseHandle(ov.hEvent)


def read_some(handle, count, timeout_ms):
    ov = _overlapped()
    try:
        buf = ctypes.create_string_buffer(count)
        got = wintypes.DWORD(0)
        ok = kernel32.ReadFile(handle, buf, count,
                               ctypes.byref(got), ctypes.byref(ov))
        if not ok:
            err = ctypes.get_last_error()
            if err == ERROR_BROKEN_PIPE:
                return b""
            if err != ERROR_IO_PENDING:
                raise win_error("ReadFile")
            _wait_io(handle, ov, got, "read", timeout_ms)
        return buf.raw[:got.value]
    finally:
        kernel32.CloseHandle(ov.hEvent)


def read_line(handle, timeout_ms):
    """Read until the first newline. Chunked, not byte at a time - a
    4096-byte GET_MEM reply is 8 KB of hex."""
    buf = bytearray()
    while b"\n" not in buf:
        chunk = read_some(handle, 4096, timeout_ms)
        if not chunk:
            raise PipeDown("pipe closed before the reply ended")
        buf += chunk
        if len(buf) > MAX_LINE:
            raise PipeDown("reply exceeded %d bytes" % MAX_LINE)
    line = bytes(buf).split(b"\n", 1)[0]
    return line.decode("ascii", "replace").rstrip("\r")


def _payload_lines(header):
    """lines=N from a reply header, or 0 when it does not say."""
    for tok in header.split():
        if tok.startswith("lines="):
            try:
                return max(0, int(tok[6:]))
            except ValueError:
                return 0
    return 0


def read_reply(handle, timeout_ms):
    """Read one reply into ONE buffer, sized by its own header.

    THE HEADER DECLARES ITS PAYLOAD as lines=N, so this does not
    have to know which command was sent - GET_MEM says lines=1,
    GET_TRACE says lines=N, and an ERR or a bare OK says nothing
    and is complete at its first newline.

    read_line cannot do this job: it builds a fresh buffer and
    discards whatever followed the first newline in the chunk it
    happened to read, and the agent sends header and payload in a
    single write.
    """
    buf = bytearray()
    want = None
    while True:
        if want is None:
            nl = buf.find(b"\n")
            if nl >= 0:
                head = bytes(buf[:nl]).decode("ascii", "replace")
                want = _payload_lines(head)
        if want is not None and buf.count(b"\n") >= want + 1:
            break
        chunk = read_some(handle, 4096, timeout_ms)
        if not chunk:
            if want is not None or buf:
                break        # short reply, take what arrived
            raise PipeDown("pipe closed before the reply ended")
        buf += chunk
        if len(buf) > MAX_LINE:
            raise PipeDown("reply exceeded %d bytes" % MAX_LINE)
    parts = bytes(buf).decode("ascii", "replace").split("\n")
    while parts and parts[-1] == "":
        parts.pop()
    if want is not None:
        parts = parts[:want + 1]
    return "\n".join(x.rstrip("\r") for x in parts)

def pipe_command(command, timeout_ms=PIPE_TIMEOUT_MS,
                 target="moto"):
    """One connection per command. Stateless, so a game restart needs
    nothing on this side.

    """
    handle = open_pipe(timeout_ms, target)
    try:
        write_all(handle, (command + "\n").encode("ascii"), timeout_ms)
        return read_reply(handle, timeout_ms)
    finally:
        kernel32.CloseHandle(handle)


# ------------------------------------------------------------------ #
# The tools                                                           #
# ------------------------------------------------------------------ #

TOOLS = [
    {
        "name": "moto_status",
        "description":
            "Ask the running MOTO.EXE what state it is in. Returns the "
            "image base, the rebase delta, the current menu page, the "
            "selected bike and the number of F11 captures taken. Use it "
            "first: it says whether the game is on the page you are "
            "about to read memory for.",
        "inputSchema": {
            "type": "object",
            "properties": {},
            "additionalProperties": False,
        },
    },
    {
        "name": "moto_load_mod",
        "description":
            "Load mrmod.dll into the running MOTO.EXE so its exports "
            "can stand in for MOTO functions. The name is a BARE "
            "FILENAME sitting beside MOTO.EXE - a path is refused. "
            "Loading is one-way: to swap a rebuilt DLL, restart "
            "MOTO.EXE. Call this before moto_hook_func.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "name": {
                    "type": "string",
                    "description": "Filename beside MOTO.EXE, "
                                   "e.g. mrmod.dll",
                },
            },
            "required": ["name"],
            "additionalProperties": False,
        },
    },
    {
        "name": "moto_hook_func",
        "description":
            "REPLACE a MOTO.EXE function with one of ours, or put the "
            "original back. Patches a five-byte JMP at the function "
            "entry to an export of the loaded module; the original "
            "never runs. This is the strongest A/B available - our "
            "code is called by MOTO, with MOTO arguments, in MOTO "
            "state, on MOTO frame - where the two-process comparison "
            "can only match the SHAPE of a track. "
            "The address is a preferred-base VA in hex, as in Ghidra. "
            "Refused outside .text, and refused where the five bytes "
            "would overlap the watched copy-protection bytes at "
            "00442761 or 004b1830..004b18b6 - a jump there makes "
            "championship mode unfinishable hours later. "
            "The reply echoes the original bytes, so a hook that did "
            "not land is visible.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "address": {
                    "type": "string",
                    "description": "Function entry, preferred-base "
                                   "VA in hex, e.g. 004b10c0",
                },
                "export": {
                    "type": "string",
                    "description": "Export name in the loaded module, "
                                   "e.g. mrmod_plane_distance",
                },
                "on": {
                    "type": "boolean",
                    "description": "true to install, false to restore "
                                   "the original bytes",
                },
            },
            "required": ["address", "export", "on"],
            "additionalProperties": False,
        },
    },
    {
        "name": "moto_unload_mod",
        "description":
            "Restore every replaced function and free the loaded "
            "module, so it can be rebuilt and loaded again WITHOUT "
            "restarting MOTO.EXE - the game keeps its race, its "
            "position and its state across the whole cycle, which is "
            "the point of testing in the live process. "
            "Unhooking first is done for you and is not optional: "
            "freeing a DLL while a jump still points into it kills "
            "the game at the next call. The reply says how many were "
            "put back.",
        "inputSchema": {
            "type": "object",
            "properties": {},
            "additionalProperties": False,
        },
    },
    {
        "name": "moto_list_func",
        "description":
            "List the loaded module and every function currently "
            "replaced by one of its exports. Use it to check what is "
            "live before believing a measurement - a hook left "
            "installed from an earlier experiment is the quiet way to "
            "misread a whole session.",
        "inputSchema": {
            "type": "object",
            "properties": {},
            "additionalProperties": False,
        },
    },
    {
        "name": "moto_get_mem",
        "description":
            "Read bytes out of the running MOTO.EXE and return them as "
            "hex. The address is a PREFERRED-BASE virtual address, the "
            "same form used in Ghidra and throughout docs/ - it is "
            "rebased inside the game, so paste addresses unchanged. "
            "Reads are bounded to 0x00400000..0x7FFFFFFF and guarded "
            "against faults.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "address": {
                    "type": "string",
                    "pattern": "^(0[xX])?[0-9a-fA-F]{1,8}$",
                    "description":
                        "Hex virtual address, with or without 0x, e.g. "
                        "004d102c. Image base is 0x00400000.",
                },
                "size": {
                    "type": "integer",
                    "minimum": 1,
                    "maximum": MAX_SIZE,
                    "description": "Bytes to read, 1..%d." % MAX_SIZE,
                },
            },
            "required": ["address", "size"],
            "additionalProperties": False,
        },
    },
    {
        "name": "moto_set_hook",
        "description":
            "Arm a trace hook. Overwrites the instruction at an "
            "address with a jump to a stub that records the registers "
            "and any requested memory, then runs the original "
            "instruction and returns - so the game behaves the same. "
            "Reads only; nothing is written to the game. The hook "
            "disarms itself after the hit budget, and get_trace "
            "collects what it saw. LENGTH MUST BE THE FULL BYTE "
            "LENGTH of the instruction being replaced, taken from a "
            "disassembly listing, and must be at least 5.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "slot": {
                    "type": "integer", "minimum": 0, "maximum": 3,
                    "description": "Hook slot, 0..3.",
                },
                "address": {
                    "type": "string",
                    "pattern": "^(0[xX])?[0-9a-fA-F]{1,8}$",
                    "description":
                        "Preferred-base VA of the instruction to hook.",
                },
                "length": {
                    "type": "integer", "minimum": 5, "maximum": 27,
                    "description":
                        "Full length in bytes of that instruction. "
                        "Getting this wrong corrupts the game.",
                },
                "hits": {
                    "type": "integer", "minimum": 1, "maximum": 8,
                    "description":
                        "Stop after this many hits, then disarm.",
                },
                "captures": {
                    "type": "string",
                    "description":
                        "Optional memory to record, comma separated, "
                        "each REG+hexoffset:size - for example "
                        "'ESP+180:4,ESP+ac:4'. Registers are EAX ECX "
                        "EDX EBX ESP EBP ESI EDI; ESP is the value at "
                        "the hooked instruction. Up to 4 items, 32 "
                        "bytes each.",
                },
            },
            "required": ["slot", "address", "length", "hits"],
            "additionalProperties": False,
        },
    },
    {
        "name": "moto_clear_hook",
        "description":
            "Disarm a hook and restore the original instruction bytes. "
            "With no slot, disarms all four and empties the trace ring.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "slot": {
                    "type": "integer", "minimum": 0, "maximum": 3,
                    "description": "Slot to clear; omit for all.",
                },
            },
            "additionalProperties": False,
        },
    },
    {
        "name": "moto_get_trace",
        "description":
            "Collect and clear what the hooks have recorded. One line: "
            "per hit, the slot, hit number, the eight general "
            "registers, and each capture as hex. ESP is corrected to "
            "the value at the hooked instruction.",
        "inputSchema": {
            "type": "object",
            "properties": {},
            "additionalProperties": False,
        },
    },
    {
        "name": "moto_set_databp",
        "description":
            "Arm an x86 HARDWARE DATA WATCHPOINT on an address, using "
            "DR0-DR3. This answers 'which instruction wrote this', "
            "which set_hook cannot: a hook traps a site you already "
            "suspect, this traps the DATA and so finds writers you "
            "have not thought of - including stores through a pointer, "
            "which a Ghidra displacement search structurally cannot "
            "find. Four slots, hardware limit. get_databp collects the "
            "hits. THE REPORTED EIP IS THE INSTRUCTION AFTER THE "
            "STORE: a data trap is raised once the storing instruction "
            "has retired, so a hit reading eip=0045e2c3 was written by "
            "the instruction ENDING there. Only the game thread is "
            "watched.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "action": {
                    "type": "string",
                    "enum": ["add", "clear"],
                    "description":
                        "'add' arms a watch, 'clear' disarms all four "
                        "slots. There is no separate clear command.",
                },
                "address": {
                    "type": "string",
                    "pattern": "^(0[xX])?[0-9a-fA-F]{1,8}$",
                    "description":
                        "Preferred-base VA to watch, required for "
                        "'add'. MUST BE NATURALLY ALIGNED TO length - "
                        "x86 leaves an unaligned data breakpoint "
                        "undefined and it is refused. The racer tail is "
                        "byte-misaligned throughout, so use length 1 "
                        "there; a 1-byte watch works at any address and "
                        "catches any store touching that byte.",
                },
                "length": {
                    "type": "integer",
                    "enum": [1, 2, 4],
                    "description":
                        "Bytes to watch, default 1. 8 is x64 only.",
                },
                "mode": {
                    "type": "string",
                    "enum": ["w", "rw"],
                    "description":
                        "'w' (default) traps writes. 'rw' traps read OR "
                        "write - x86 HAS NO READ-ONLY ENCODING, so a "
                        "read watch is an access watch and a read has "
                        "to be inferred from the value not changing.",
                },
                "cap": {
                    "type": "integer", "minimum": 0, "maximum": 64,
                    "description":
                        "Auto-disarm after this many hits, default 8, "
                        "0 for unlimited. A watch on a hot address "
                        "costs a kernel round trip per access and will "
                        "visibly slow the game, so raise this "
                        "deliberately.",
                },
                "changed": {
                    "type": "boolean",
                    "description":
                        "Only record when the value actually differs "
                        "from the previous hit.",
                },
                "value_eq": {
                    "type": "string",
                    "pattern": "^(0[xX])?[0-9a-fA-F]{1,8}$",
                    "description":
                        "Only record when the value after the access "
                        "equals this hex immediate.",
                },
                "value_ne": {
                    "type": "string",
                    "pattern": "^(0[xX])?[0-9a-fA-F]{1,8}$",
                    "description":
                        "Only record when the value after the access "
                        "differs from this hex immediate.",
                },
                "tag": {
                    "type": "string",
                    "description": "Short label echoed by get_databp.",
                },
            },
            "required": ["action"],
            "additionalProperties": False,
        },
    },
    {
        "name": "moto_get_databp",
        "description":
            "Report the watchpoint state and collect what it has "
            "recorded. The state line comes FIRST and is not padding: "
            "it reads DR0-DR3 and DR7 back off the game thread next to "
            "what was asked for, because Windows sanitizes DR7 on the "
            "way in and a silently dropped arm is otherwise "
            "indistinguishable from 'nothing writes this'. Then one "
            "entry per hit: the slot, hit number, eip AFTER the storing "
            "instruction, all eight registers, and the watched value "
            "before and after. Draining clears the hits.",
        "inputSchema": {
            "type": "object",
            "properties": {},
            "additionalProperties": False,
        },
    },
    {
        "name": "moto_send_input",
        "description":
            "Drive the game: pause, hold a driving input for a bounded "
            "number of frames, then pause again - so the game is always "
            "left stopped and safe to probe. The hold mask writes the "
            "player input word directly (bit 0 accelerate, 1 brake, "
            "2 left, 3 right, 4 higher gear, 5 lower gear, 6 STUNT - the "
            "wheelie button, which is not in the input word at all and "
            "goes to action6Down instead); accelerate with brake, or "
            "left with right, is refused because the game cannot reach "
            "that state from a keyboard. Turbo is not a bit and is not "
            "supported. Taps are virtual-key codes "
            "posted to the window for hotkeys - \"74\" for F5, "
            "\"11+56\" for CTRL+V - and go in once the run starts.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "hold": {
                    "type": "string",
                    "description": "Driving bitmask in hex, bits 0..6. "
                                   "0 for none.",
                    "pattern": "^[0-9a-fA-F]{1,8}$",
                },
                "frames": {
                    "type": "integer",
                    "description": "Frames to run, 1..300 - five seconds "
                                   "at 60 fps. Chain calls for longer.",
                    "minimum": 1,
                    "maximum": 300,
                },
                "taps": {
                    "type": "array",
                    "description": "Optional hex virtual-key taps, each "
                                   "\"vk\" or \"mod+vk\".",
                    "items": {
                        "type": "string",
                        "pattern":
                            "^[0-9a-fA-F]{1,2}([+][0-9a-fA-F]{1,2})?$",
                    },
                    "maxItems": 4,
                },
            },
            "required": ["hold", "frames"],
            "additionalProperties": False,
        },
    },
    {
        "name": "moto_set_mem",
        "description":
            "WRITE bytes into the running MOTO.EXE - the only command "
            "here that writes. The address is a PREFERRED-BASE virtual "
            "address, same as get_mem. Pass a raw hex byte string, or "
            "a number with type f32/i32/u32 and it is packed "
            "little-endian for you. The reply carries \"was\" (the "
            "bytes clobbered, so the undo is in the reply) and "
            "\"now\" (re-read after the write, so a write that did "
            "not land is visible rather than a false success). Writes "
            "into .text are refused outright. Most physics fields are "
            "recomputed every frame, so a bare write is gone before "
            "anything draws it - use hold_frames to rewrite the value "
            "every poll for N frames, which pauses, runs, and pauses "
            "again exactly as send_input does. code=true "
            "LIFTS THE .text REFUSAL for deliberate patching: the "
            "write goes through VirtualProtect with the game thread "
            "suspended, and it lasts until MOTO.EXE exits - a level "
            "reset, a race restart and the menus all leave it in "
            "place. It never touches the file on disk. Do not write "
            "00442761-2 or 004b1830..004b18b6; the copy protection "
            "reads those as data and altering them makes championship "
            "mode unfinishable much later.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "address": {
                    "type": "string",
                    "pattern": "^(0[xX])?[0-9a-fA-F]{1,8}$",
                    "description":
                        "Hex virtual address, with or without 0x. "
                        "Image base is 0x00400000.",
                },
                "value": {
                    "type": "string",
                    "description":
                        "With type hex: an even-length hex byte "
                        "string, 1..%d bytes, little-endian as stored. "
                        "With f32/i32/u32: a decimal number, e.g. "
                        "\"0.5\" or \"-3\"." % SET_MEM_MAX,
                },
                "type": {
                    "type": "string",
                    "enum": ["hex", "f32", "i32", "u32"],
                    "description":
                        "How to read value. Default hex. f32 packs an "
                        "IEEE754 float - use it rather than packing by "
                        "hand.",
                },
                "code": {
                    "type": "boolean",
                    "description":
                        "Allow writing .text. Off by default. "
                        "Mutually exclusive with hold_frames.",
                },
                "hold_frames": {
                    "type": "integer",
                    "minimum": 1,
                    "maximum": 300,
                    "description":
                        "Rewrite the value every poll for this many "
                        "frames. Needed for anything the game "
                        "recomputes per frame.",
                },
            },
            "required": ["address", "value"],
            "additionalProperties": False,
        },
    },
    {
        "name": "mracer_status",
        "description":
            "Ask OUR PORT (mracer.exe) whether its debug agent is up. "
            "Returns the symbol count read from the linker .map, the "
            "module base, and whether the image was relocated. Use it "
            "first: symbols=0 means the .map was missing, and every "
            "mracer_get_mem will fail until that is fixed.",
        "inputSchema": {
            "type": "object",
            "properties": {},
            "additionalProperties": False,
        },
    },
    {
        "name": "mracer_get_mem",
        "description":
            "Read a NAMED symbol out of our running port and return it "
            "as hex. There is no address: the agent resolves the name "
            "against the .map and rebases it. Use the C name - the "
            "map's leading underscore is stripped, so s_race_racers, "
            "not _s_race_racers. NEEDS THE .map BESIDE THE .exe; "
            "without it mracer_status reports symbols=0 and every "
            "read fails. Release|Win32 links one, so the build "
            "CLAUDE.md rule 9 allows is enough. The caller is "
            "trusted for the size: only the name and the length are "
            "checked.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "name": {
                    "type": "string",
                    "description":
                        "Symbol name, e.g. s_race_racers. No spaces.",
                },
                "size": {
                    "type": "integer",
                    "minimum": 1,
                    "maximum": MRACER_MAX_SIZE,
                    "description": "Bytes to read, 1..%d."
                                   % MRACER_MAX_SIZE,
                },
                "offset": {
                    "type": "integer",
                    "description":
                        "Bytes past the symbol. Defaults to 0, and may "
                        "be negative.",
                },
                "obj": {
                    "type": "string",
                    "description":
                        "Object file, to pick between duplicate "
                        "statics, e.g. race.obj or libucrt:87disp.obj. "
                        "Without it the first match in map order wins.",
                },
            },
            "required": ["name", "size"],
            "additionalProperties": False,
        },
    },
    {
        "name": "mracer_set_mem",
        "description":
            "Write bytes to a NAMED symbol in our running port - the "
            "mirror of mracer_get_mem, and the way to PLACE THE BIKE "
            "for a repeatable test. Wall and crash behaviour depends "
            "on the angle and the speed of the impact, so driving to "
            "a position by hand gives a different event every run; "
            "writing the position and orientation directly makes the "
            "two sides comparable. tools/mr_place.py does the same "
            "to MOTO.EXE. The reply echoes was= so the write can be "
            "undone. DATA ONLY, up to 128 bytes; it crosses to the "
            "game thread so a multi-field write cannot land across a "
            "frame boundary. Needs the .map, which Release links.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "name": {
                    "type": "string",
                    "description":
                        "Symbol name, e.g. s_race_racers. No spaces.",
                },
                "value": {
                    "type": "string",
                    "description":
                        "Bytes as hex, no 0x and no spaces, even "
                        "length, 1..128 bytes. Little-endian is the "
                        "caller's business.",
                },
                "offset": {
                    "type": "integer",
                    "description":
                        "Bytes past the symbol. Defaults to 0, and "
                        "may be negative.",
                },
                "obj": {
                    "type": "string",
                    "description":
                        "Object file, to pick between duplicate "
                        "statics, e.g. race.obj.",
                },
            },
            "required": ["name", "value"],
            "additionalProperties": False,
        },
    },
    {
        "name": "mracer_set_hook",
        "description":
            "Patch a function entry in OUR PORT so every call is "
            "recorded, then read the hits with mracer_get_trace. Ten "
            "slots. Needs a Debug|Win32 build: the entry must carry "
            "the /hotpatch preamble 66 90 55 8B EC or it is refused "
            "rather than patched. Installation happens on the game "
            "thread, so it cannot corrupt code the game is running.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "name": {
                    "type": "string",
                    "description":
                        "Function symbol, e.g. racer_lean_delta.",
                },
                "nargs": {
                    "type": "integer",
                    "minimum": 0,
                    "maximum": 5,
                    "description":
                        "How many cdecl args are real (default 1). "
                        "a0..a4 at or above this are the caller's "
                        "stack and are never read.",
                },
                "when": {
                    "type": "string",
                    "description":
                        "Gate: always (default), aN, aNnz, aN=IMM, "
                        "aN!=IMM.",
                },
                "cap": {
                    "type": "integer",
                    "minimum": 0,
                    "maximum": 65535,
                    "description":
                        "Max hits recorded, 0 = unlimited. Defaults "
                        "to 200 - a per-frame function with no cap "
                        "fills the 256-line ring instantly.",
                },
                "tag": {
                    "type": "string",
                    "description":
                        "Label in the trace line, max 15 chars of "
                        "[A-Za-z0-9._-].",
                },
                "log": {
                    "type": "string",
                    "description":
                        "Comma list, no spaces. a0..a4 the argument "
                        "word; aNf that word AS A FLOAT (what a float "
                        "passed by value is - use this for our "
                        "physics); ra the caller as name+0xOFF; "
                        "aNb+OFF / aNw+OFF / aNl+OFF / aNf+OFF to "
                        "dereference. Default a0, or ra when nargs=0.",
                },
            },
            "required": ["name"],
            "additionalProperties": False,
        },
    },
    {
        "name": "mracer_get_trace",
        "description":
            "Drain the port's hook trace ring and return the recorded "
            "hits, newest last. Clears the ring unless keep is true. "
            "The ring holds 256 lines; a hook that overran its cap "
            "says so with a truncated marker.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "keep": {
                    "type": "boolean",
                    "description":
                        "Read without clearing. Default false.",
                },
            },
            "additionalProperties": False,
        },
    },
    {
        "name": "mracer_clear_hook",
        "description":
            "Remove every hook from our port, restoring the original "
            "function prologues, and empty the trace ring. Safe to "
            "call when nothing is hooked.",
        "inputSchema": {
            "type": "object",
            "properties": {},
            "additionalProperties": False,
        },
    },
    {
        "name": "mracer_send_input",
        "description":
            "Drive OUR PORT: unpause, hold keys for a bounded number "
            "of frames, then pause - so the game is always left "
            "stopped and safe to read with mracer_get_mem. keys are "
            "HELD for the run and assemble the game's own input word "
            "(the same mask MOTO.EXE takes). tap is a single "
            "keypress delivered before the run, posted to the window "
            "as a real key - menus are edge driven, so that is how "
            "to work one, and it is also how to reach the harness "
            "keys f1, step and pause.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "keys": {
                    "type": "string",
                    "description":
                        "Held for the run, comma list, no spaces: "
                        "z (accelerate), x (brake), left, right, "
                        "c (stunt). Omit to hold nothing.",
                },
                "frames": {
                    "type": "integer",
                    "minimum": 0,
                    "maximum": 300,
                    "description":
                        "Frames to run, default 1. The port targets "
                        "30 fps, so 300 is ten seconds. 0 applies "
                        "the taps and returns without touching the "
                        "pause. Chain calls for longer runs.",
                },
                "tap": {
                    "type": "string",
                    "description":
                        "One keypress each, before the run, comma "
                        "list, no spaces: up, down, left, right, "
                        "enter, esc, f1 (restart page), step (F5 "
                        "single frame), pause (toggle).",
                },
            },
            "additionalProperties": False,
        },
    },
]


def tool_status(args):
    return pipe_command("STATUS")


def tool_load_mod(args):
    name = str(args.get("name", "")).strip()
    if not name:
        return "ERR moto_load_mod needs a filename"
    return pipe_command("LOAD_MOD " + name)


def tool_hook_func(args):
    addr = str(args.get("address", "")).strip()
    export = str(args.get("export", "")).strip()
    if addr.lower().startswith("0x"):
        addr = addr[2:]
    if not addr or not export:
        return "ERR moto_hook_func needs address and export"
    on = 1 if args.get("on") else 0
    return pipe_command("HOOK_FUNC %s %s %d" % (addr, export, on))


def tool_unload_mod(args):
    return pipe_command("UNLOAD_MOD")


def tool_list_func(args):
    return pipe_command("LIST_FUNC")


def tool_get_mem(args):
    """Re-validate rather than trusting the schema - a client is not
    obliged to enforce it, and the game is on the other end."""
    address = args.get("address")
    size = args.get("size")

    if not isinstance(address, str):
        raise ValueError("address must be a hex string")
    text = address.strip()
    if text[:2].lower() == "0x":
        text = text[2:]
    if not text or len(text) > 8:
        raise ValueError("address must be 1 to 8 hex digits")
    try:
        value = int(text, 16)
    except ValueError:
        raise ValueError("address is not hex: %s" % address)

    if isinstance(size, bool) or not isinstance(size, int):
        raise ValueError("size must be an integer")
    if size < 1 or size > MAX_SIZE:
        raise ValueError("size must be 1..%d" % MAX_SIZE)

    return pipe_command("GET_MEM %08x %d" % (value, size))




def _slot(args):
    slot = args.get("slot")
    if isinstance(slot, bool) or not isinstance(slot, int):
        raise ValueError("slot must be an integer")
    if slot < 0 or slot > 3:
        raise ValueError("slot must be 0..3")
    return slot


def tool_set_hook(args):
    slot = _slot(args)
    address = args.get("address")
    length = args.get("length")
    hits = args.get("hits")
    caps = args.get("captures") or ""

    if not isinstance(address, str):
        raise ValueError("address must be a hex string")
    text = address.strip()
    if text[:2].lower() == "0x":
        text = text[2:]
    try:
        value = int(text, 16)
    except ValueError:
        raise ValueError("address is not hex: %s" % address)

    for name, val, lo, hi in (("length", length, 5, 27),
                              ("hits", hits, 1, 8)):
        if isinstance(val, bool) or not isinstance(val, int):
            raise ValueError("%s must be an integer" % name)
        if val < lo or val > hi:
            raise ValueError("%s must be %d..%d" % (name, lo, hi))

    caps = "".join(caps.split())
    command = "SET_HOOK %d %08x %d %d" % (slot, value, length, hits)
    if caps:
        command += " " + caps
    return pipe_command(command)


def tool_clear_hook(args):
    if "slot" in args and args.get("slot") is not None:
        return pipe_command("CLEAR_HOOK %d" % _slot(args))
    return pipe_command("CLEAR_HOOK")


def tool_get_trace(args):
    return pipe_command("GET_TRACE")


def tool_send_input(args):
    hold = str(args.get("hold", "0")).lower()
    frames = int(args.get("frames", 1))
    taps = args.get("taps") or []
    cmd = "SEND_INPUT %s %x" % (hold, frames)
    for t in taps:
        cmd += " " + str(t).lower()
    # This command BLOCKS for the length of the run, unlike every
    # other one here - so the default 2 s read timeout would fire
    # before a 60-frame hold had finished. Budget the run itself at
    # 60 fps, then add room for the two pause round trips, and stay
    # above the DLL's own 20 s guard so its error line arrives
    # rather than being cut off as a timeout.
    budget = frames * 1000 // 60 + 8000
    return pipe_command(cmd, min(budget, 30000))


def tool_set_mem(args):
    """Re-validate rather than trusting the schema, same stance as
    tool_get_mem - a client is not obliged to enforce it, and this one
    writes."""
    address = args.get("address")
    value = args.get("value")
    kind = args.get("type", "hex")
    hold = args.get("hold_frames")
    code = bool(args.get("code"))

    if not isinstance(address, str):
        raise ValueError("address must be a hex string")
    text = address.strip()
    if text[:2].lower() == "0x":
        text = text[2:]
    if not text or len(text) > 8:
        raise ValueError("address must be 1 to 8 hex digits")
    try:
        addr = int(text, 16)
    except ValueError:
        raise ValueError("address is not hex: %s" % address)

    if not isinstance(value, str):
        raise ValueError("value must be a string")
    value = value.strip()

    if kind == "hex":
        payload = value.lower()
        if payload[:2] == "0x":
            payload = payload[2:]
        if not payload or len(payload) % 2:
            raise ValueError("hex value needs whole bytes")
        try:
            int(payload, 16)
        except ValueError:
            raise ValueError("value is not hex: %s" % value)
    elif kind == "f32":
        payload = struct.pack("<f", float(value)).hex()
    elif kind == "i32":
        payload = struct.pack("<i", int(value, 0)).hex()
    elif kind == "u32":
        payload = struct.pack("<I", int(value, 0)).hex()
    else:
        raise ValueError("type must be hex, f32, i32 or u32")

    if len(payload) // 2 > SET_MEM_MAX:
        raise ValueError("value must be 1..%d bytes" % SET_MEM_MAX)

    # .text is refused in the DLL too - this is the early, friendlier
    # copy of the same rule, so a typo does not have to round trip.
    # code=true lifts it on BOTH sides; leaving it here would reject
    # the command before the wire ever saw the flag.
    size = len(payload) // 2
    if not code and addr <= TEXT_HI and addr + size - 1 >= TEXT_LO:
        raise ValueError(
            "refusing to write .text (%08x..%08x) - pass code=true "
            "to mean it" % (TEXT_LO, TEXT_HI))

    cmd = "SET_MEM %08x %s" % (addr, payload)
    if code:
        cmd += " CODE"
    if hold is None:
        return pipe_command(cmd)

    if code:
        # The DLL refuses this too; saying so here costs no round
        # trip and explains why.
        raise ValueError(
            "code and hold_frames are exclusive - a code patch "
            "persists until it is written back")

    if isinstance(hold, bool) or not isinstance(hold, int):
        raise ValueError("hold_frames must be an integer")
    if hold < 1 or hold > 300:
        raise ValueError("hold_frames must be 1..300")
    # Same budgeting as send_input: this blocks for the length of the
    # run, so the default read timeout would fire mid-hold.
    budget = hold * 1000 // 60 + 8000
    return pipe_command(cmd + " HOLD %x" % hold, min(budget, 30000))


def _hexarg(args, key):
    """Hex string to int, or None when absent. Re-validated rather than
    trusted from the schema, the same stance as tool_set_mem."""
    text = args.get(key)
    if text is None:
        return None
    if not isinstance(text, str):
        raise ValueError("%s must be a hex string" % key)
    text = text.strip()
    if text[:2].lower() == "0x":
        text = text[2:]
    try:
        return int(text, 16)
    except ValueError:
        raise ValueError("%s is not hex: %s" % (key, args.get(key)))


def tool_set_databp(args):
    action = args.get("action")
    if action == "clear":
        return pipe_command("SET_DATABP clear")
    if action != "add":
        raise ValueError("action must be add or clear")

    address = _hexarg(args, "address")
    if address is None:
        raise ValueError("add needs an address")

    length = args.get("length", 1)
    if length not in (1, 2, 4):
        raise ValueError("length must be 1, 2 or 4")
    # Refuse here as well as in the DLL. x86 leaves an unaligned data
    # breakpoint undefined, and saying so with the offending number is
    # more use than the game refusing it a round trip later.
    if address % length:
        raise ValueError(
            "%08x is not aligned to length=%d - x86 requires natural "
            "alignment; use length=1, which works at any address"
            % (address, length))

    cmd = "SET_DATABP add %08x len=%d" % (address, length)

    mode = args.get("mode")
    if mode is not None:
        if mode not in ("w", "rw"):
            raise ValueError("mode must be w or rw")
        cmd += " mode=%s" % mode

    cap = args.get("cap")
    if cap is not None:
        if isinstance(cap, bool) or not isinstance(cap, int):
            raise ValueError("cap must be an integer")
        if cap < 0 or cap > 64:
            raise ValueError("cap must be 0..64")
        cmd += " cap=%d" % cap

    if args.get("changed"):
        cmd += " changed"

    eq = _hexarg(args, "value_eq")
    ne = _hexarg(args, "value_ne")
    if eq is not None and ne is not None:
        raise ValueError("value_eq and value_ne are mutually exclusive")
    if eq is not None:
        cmd += " val=%x" % eq
    if ne is not None:
        cmd += " val!=%x" % ne

    tag = args.get("tag")
    if tag:
        tag = "".join(str(tag).split())
        if len(tag) > 23:
            raise ValueError("tag must be 23 characters or fewer")
        cmd += " tag=%s" % tag

    return pipe_command(cmd)


def tool_get_databp(args):
    return pipe_command("GET_DATABP")


def tool_mracer_status(args):
    return pipe_command("STATUS", target="mracer")


def _word(args, key, required=False):
    """A whitespace-free argument value, or None."""
    v = args.get(key)
    if v is None:
        if required:
            raise ValueError("%s is required" % key)
        return None
    if not isinstance(v, str) or len(v.split()) != 1:
        raise ValueError("%s must be one word, no spaces" % key)
    return v.strip()


def tool_mracer_set_hook(args):
    """Patch a function entry in our port and record its calls."""
    cmd = "SET_HOOK add name=%s" % _word(args, "name", True)

    nargs = args.get("nargs")
    if nargs is not None:
        if isinstance(nargs, bool) or not isinstance(nargs, int):
            raise ValueError("nargs must be an integer")
        if nargs < 0 or nargs > 5:
            raise ValueError("nargs must be 0..5")
        cmd += " nargs=%d" % nargs

    cap = args.get("cap")
    if cap is not None:
        if isinstance(cap, bool) or not isinstance(cap, int):
            raise ValueError("cap must be an integer")
        if cap < 0 or cap > 0xffff:
            raise ValueError("cap must be 0..65535")
        cmd += " cap=%d" % cap

    for key in ("when", "tag", "log"):
        v = _word(args, key)
        if v is not None:
            cmd += " %s=%s" % (key, v)

    return pipe_command(cmd, target="mracer")


def tool_mracer_get_trace(args):
    keep = args.get("keep")
    return pipe_command("GET_TRACE keep" if keep else "GET_TRACE",
                        target="mracer")


def tool_mracer_clear_hook(args):
    return pipe_command("CLEAR_HOOK", target="mracer")


def tool_mracer_send_input(args):
    """Hold keys for N frames, then leave the game paused."""
    cmd = "SEND_INPUT"

    keys = _word(args, "keys")
    if keys:
        cmd += " keys=%s" % keys

    tap = _word(args, "tap")
    if tap:
        cmd += " tap=%s" % tap

    frames = args.get("frames")
    if frames is None:
        frames = 1
    if isinstance(frames, bool) or not isinstance(frames, int):
        raise ValueError("frames must be an integer")
    if frames < 0 or frames > 300:
        raise ValueError("frames must be 0..300")
    cmd += " frames=%d" % frames

    if not keys and not tap and frames == 0:
        raise ValueError("nothing to do - give keys, tap or frames")

    # THIS ONE BLOCKS FOR THE WHOLE RUN, unlike every other command
    # here, so the default 2 s read timeout would fire before a long
    # hold finished. Budget the run at 45 ms a frame - the port
    # targets 30 fps - and add room for the round trip.
    budget = frames * 45 + 8000
    return pipe_command(cmd, min(budget, 30000), target="mracer")


def tool_mracer_get_mem(args):
    """Read a NAMED symbol out of our own running port.

    Unlike moto_get_mem there is no address: the agent resolves the
    name against the linker .map and rebases it, so nothing here has
    to know where the image landed.
    """
    name = args.get("name")
    if not isinstance(name, str) or not name.strip():
        raise ValueError("name must be a symbol name")
    name = name.strip()
    if len(name.split()) != 1:
        raise ValueError("name must not contain spaces")

    size = args.get("size")
    if isinstance(size, bool) or not isinstance(size, int):
        raise ValueError("size must be an integer")
    if size < 1 or size > MRACER_MAX_SIZE:
        raise ValueError("size must be 1..%d" % MRACER_MAX_SIZE)

    cmd = "GET_MEM name=%s len=%d" % (name, size)

    offset = args.get("offset")
    if offset is not None:
        if isinstance(offset, bool) or not isinstance(offset, int):
            raise ValueError("offset must be an integer")
        cmd += " offset=%d" % offset

    obj = args.get("obj")
    if obj is not None:
        if not isinstance(obj, str) or len(obj.split()) != 1:
            raise ValueError("obj must be one object file name")
        cmd += " obj=%s" % obj.strip()

    return pipe_command(cmd, target="mracer")


MRACER_SET_MAX = 128


def tool_mracer_set_mem(args):
    """Write a NAMED symbol in our own running port.

    The mirror of tool_mracer_get_mem, and it exists for the same
    reason mr_place.py does on the MOTO side: an impact test is only
    a measurement if the impact can be repeated, and that means
    writing the position and the heading rather than driving to them.
    """
    name = args.get("name")
    if not isinstance(name, str) or not name.strip():
        raise ValueError("name must be a symbol name")
    name = name.strip()
    if len(name.split()) != 1:
        raise ValueError("name must not contain spaces")

    value = args.get("value")
    if not isinstance(value, str) or not value.strip():
        raise ValueError("value must be hex bytes")
    value = value.strip().lower()
    if value.startswith("0x"):
        value = value[2:]
    if len(value) % 2 or not value:
        raise ValueError("value must be an even number of hex digits")
    if len(value) > MRACER_SET_MAX * 2:
        raise ValueError("value must be at most %d bytes"
                         % MRACER_SET_MAX)
    if any(c not in "0123456789abcdef" for c in value):
        raise ValueError("value must be hex")

    cmd = "SET_MEM name=%s value=%s" % (name, value)

    offset = args.get("offset")
    if offset is not None:
        if isinstance(offset, bool) or not isinstance(offset, int):
            raise ValueError("offset must be an integer")
        cmd += " offset=%d" % offset

    obj = args.get("obj")
    if obj is not None:
        if not isinstance(obj, str) or len(obj.split()) != 1:
            raise ValueError("obj must be one object file name")
        cmd += " obj=%s" % obj.strip()

    return pipe_command(cmd, target="mracer")

TOOL_FUNCS = {
    "moto_status": tool_status,
    "moto_load_mod": tool_load_mod,
    "moto_hook_func": tool_hook_func,
    "moto_unload_mod": tool_unload_mod,
    "moto_list_func": tool_list_func,
    "moto_get_mem": tool_get_mem,
    "moto_set_hook": tool_set_hook,
    "moto_clear_hook": tool_clear_hook,
    "moto_get_trace": tool_get_trace,
    "moto_send_input": tool_send_input,
    "moto_set_mem": tool_set_mem,
    "moto_set_databp": tool_set_databp,
    "moto_get_databp": tool_get_databp,
    "mracer_status": tool_mracer_status,
    "mracer_get_mem": tool_mracer_get_mem,
    "mracer_set_mem": tool_mracer_set_mem,
    "mracer_set_hook": tool_mracer_set_hook,
    "mracer_get_trace": tool_mracer_get_trace,
    "mracer_clear_hook": tool_mracer_clear_hook,
    "mracer_send_input": tool_mracer_send_input,
}


# ------------------------------------------------------------------ #
# JSON-RPC over stdio                                                 #
# ------------------------------------------------------------------ #

def send(msg):
    data = json.dumps(msg, separators=(",", ":"), ensure_ascii=True)
    _OUT.write(data.encode("utf-8") + b"\n")
    _OUT.flush()


def reply(mid, result):
    send({"jsonrpc": "2.0", "id": mid, "result": result})


def fail(mid, code, message):
    send({"jsonrpc": "2.0", "id": mid,
          "error": {"code": code, "message": message}})


def tool_result(text, is_error=False):
    return {"content": [{"type": "text", "text": text}],
            "isError": bool(is_error)}


def handle_initialize(params):
    asked = params.get("protocolVersion")
    version = asked if asked in KNOWN_VERSIONS else FALLBACK_VERSION
    return {
        "protocolVersion": version,
        "capabilities": {"tools": {"listChanged": False}},
        "serverInfo": {"name": SERVER_NAME, "version": SERVER_VERSION},
    }


def handle_tools_call(params):
    """Anything the model should see - the game being down, a bad
    address, an ERR line - is a SUCCESSFUL result with isError set.
    Only protocol faults become JSON-RPC errors, because those are far
    less visible in the transcript."""
    name = params.get("name")
    args = params.get("arguments") or {}
    func = TOOL_FUNCS.get(name)
    if func is None:
        return None
    try:
        return tool_result(func(args))
    except PipeDown as exc:
        return tool_result(str(exc), True)
    except ValueError as exc:
        return tool_result("bad argument: %s" % exc, True)
    except Exception as exc:
        log("tool %s failed: %r", name, exc)
        return tool_result("%s failed: %s" % (name, exc), True)


def handle(msg):
    """Return a response dict, or None when nothing may be sent."""
    method = msg.get("method")
    params = msg.get("params") or {}
    has_id = "id" in msg          # an id of 0 is legal and common
    mid = msg.get("id")

    if method is None:
        if has_id:
            fail(mid, INVALID_REQUEST, "no method")
        return

    if not has_id:
        return                    # a notification is never answered

    if method == "initialize":
        reply(mid, handle_initialize(params))
    elif method == "tools/list":
        reply(mid, {"tools": TOOLS})
    elif method == "tools/call":
        result = handle_tools_call(params)
        if result is None:
            fail(mid, INVALID_PARAMS,
                 "unknown tool: %s" % params.get("name"))
        else:
            reply(mid, result)
    elif method == "ping":
        reply(mid, {})
    elif method == "resources/list":
        reply(mid, {"resources": []})
    elif method == "resources/templates/list":
        reply(mid, {"resourceTemplates": []})
    elif method == "prompts/list":
        reply(mid, {"prompts": []})
    elif method == "logging/setLevel":
        reply(mid, {})
    else:
        fail(mid, METHOD_NOT_FOUND, "method not found: %s" % method)


def serve():
    stdin = sys.stdin.buffer
    while True:
        line = stdin.readline()
        if not line:
            return 0              # EOF: the client closed us down
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line.decode("utf-8"))
        except Exception as exc:
            log("parse error: %r", exc)
            send({"jsonrpc": "2.0", "id": None,
                  "error": {"code": PARSE_ERROR, "message": "bad JSON"}})
            continue
        try:
            handle(msg)
        except Exception as exc:
            log("handler crashed: %r", exc)
            if "id" in msg:
                fail(msg.get("id"), INTERNAL_ERROR, str(exc))


def main(argv):
    global _OUT

    if argv and argv[0] in ("--cli", "--cli-mracer"):
        target = "moto" if argv[0] == "--cli" else "mracer"
        if len(argv) < 2:
            sys.stderr.write(
                "usage: --cli|--cli-mracer <command...>\n")
            return 2
        try:
            print(pipe_command(" ".join(argv[1:]),
                               target=target))
        except PipeDown as exc:
            sys.stderr.write("%s\n" % exc)
            return 1
        return 0

    # TAKE A PRIVATE COPY OF STDOUT AND POINT fd 1 AT STDERR. After
    # this an accidental print() anywhere is harmless instead of
    # corrupting the protocol stream.
    _OUT = os.fdopen(os.dup(1), "wb", 0)
    os.dup2(2, 1)

    log("serving on stdio, pipes %s and %s",
        PIPE_NAME, MRACER_PIPE_NAME)
    return serve()


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
