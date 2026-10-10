#!/usr/bin/env python3
"""mr_cheats.py - toggle the game's unlockables in the running MOTO.EXE.

    python tools/mr_cheats.py                 report, change nothing
    python tools/mr_cheats.py --list          what can be toggled
    python tools/mr_cheats.py --on pocket     turn one on
    python tools/mr_cheats.py --on pocket reverse tracks
    python tools/mr_cheats.py --off tracks    put it back as shipped
    python tools/mr_cheats.py --all-on

WHY A SCRIPT AND NOT AN INI SETTING IN THE DLL. Every one of these is
a handful of 4-byte writes to addresses we already know, and SET_MEM
already exists. Adding an ini key would mean editing mrhook.c,
rebuilding, redeploying and restarting the game to change a value -
and the whole point of the agent is not having to. The same
composition rule mr_freeze.py follows: if GET_MEM and SET_MEM can
express it, it belongs in tools/, not in the DLL.

nowalls AND nocountdown ARE CODE PATCHES, NOT DATA POKES, and they
are the only two here that are. They need SET_MEM's CODE flag, which
lifts the .text refusal and routes the write through VirtualProtect
with the game thread suspended - so they need an mrhook new enough to
have it.

nocountdown IS HALF OF THE RESTART LOOP. The other half is the "2"
key, which mrhook posts ESC+ENTER for - press 2, the race restarts,
and with this on it is running before you let go. Together they are
what F1 is on our port. See tools/mrhook/README.txt.

cam90 IS THE SIDE VIEW and it is one the game already has - slot 2
of the camera's own look table, where TAB is slot 1. It is a DATA
poke and it survives neither TAB nor a race restart, both of which
write the same field; the "1" hotkey in mrhook re-asserts it and
does.

It writes five bytes over the entry of
maybe_Trk_ClipMoveAgainstWalls so the routine returns 0 - "neither
wall was crossed" - and the caller neither reflects the move nor
pushes the bike out. --off puts the original five bytes back.

IT LASTS UNTIL THE PROCESS EXITS. A level reset, a restart of the
race, even returning to the menus all leave it in place, because
nothing reloads .text. Only quitting MOTO.EXE clears it. That is
different from every other toggle here, which the game itself
re-reads from LIST.BPT on a fresh launch.

IT DOES NOT TOUCH THE FILE. The write goes to the running process
through SET_MEM, so decomp/MOTO.EXE on disk is unchanged and
tools/diff_original.py still reports the same seven patched sites.

THE OTHER TOGGLES ARE RUNTIME POKES AND THEY DO NOT PERSIST. Tracks_LoadList
(0x00428d60) re-reads PocketAvailable and ReverseAvailable out of
DATA\\LIST.BPT on every run, and the shipped file has both at 0, so a
fresh launch is locked again. Nothing here writes a file, patches an
executable, or touches the registry. See CLAUDE.md rule 5a: a debug
override is fine provided it is off unless you ask for it.

WHAT THE GAME DOES WITH THE UNLOCKS. Each unlockable is TWO flags - it
is UNLOCKED (the option exists at all) and it is ENABLED (it is
switched on right now). Page 3 draws an indicator per feature and
tests unlocked OR cheated. Turning ENABLED on is self-sustaining
inside a run, because 0x00455224 raises UNLOCKED whenever the matching
ENABLED flag is set - so the game re-asserts what this script sets.

onelap EXISTS TO MAKE A REPLAY TESTABLE, and it writes the TRACK's
default lap count rather than g_nRaceLaps. Poking g_nRaceLaps
(0x004dc074) does not hold: MenuPage06_RaceSelect_Tick re-forces it
from the track default on EVERY FRAME while g_nSoloMode is set and
the mode is not practice - the test is at 0x0044815c - so the write
is gone the next time page 6 ticks. Writing the default instead means
the forcing copies the 1 for us, and single race offers no + and - to
fight over.

  RETAIL REPLAY IS SINGLE RACE ONLY. The .Sav header stores
  g_nSelectedMode at +0x10 and the loader demands it be 1, so a
  practice or championship recording is refused with REPLAY
  COMPATIBILITY ERROR. The loader also requires the same TRACK and
  the same reverse and pocket flags as the recording, and fewer than
  24 opponents - so load the replay from the same page-6 selection it
  was made on. See journal 383.

TRACK AVAILABILITY LIVES ON THE HEAP. g_pTrackList (0x004dc088) points
at g_nTrackCount records of 0x130 bytes, availability at record+0x128.
The pointer is re-read on every invocation because the block comes
from operator_new and moves between runs - never cache it.

Addresses come from docs/formats/list-bpt.txt,
docs/subsystems/config-file.txt and docs/formats/bpt-carac.txt, and
every layout was re-verified live before this file was written.
"""
import json
import os
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bridge_mcp_mrhook as mr          # noqa: E402

ADDR_POCKET_UNLOCKED = 0x004dc03c
ADDR_REVERSE_UNLOCKED = 0x004dc040
ADDR_REVERSE_ENABLED = 0x004dc068
ADDR_POCKET_ENABLED = 0x004dc078

ADDR_TRACKLIST = 0x004dc088             # pointer to the record block
ADDR_TRACKCOUNT = 0x0051d298
TRACK_STRIDE = 0x130
TRACK_OFF_NAME = 0x004
TRACK_OFF_AVAIL = 0x128
TRACK_OFF_LAPS = 0x12c

# g_bikeCarac - eight records of 73 values, filled by Carac_Load
# (0x0042a580) from the CARAC1..CARAC8.BPT of whichever bike class is
# loaded. docs/formats/bpt-carac.txt section 4.
ADDR_CARAC = 0x00509278
CARAC_STRIDE = 0x124
CARAC_COUNT = 8

# The eight values somebody edited in decomp/DATA/SPEED.BKF's
# CARAC2.BPT. Seven raised and one lowered; releases.txt section 4.3
# established that our SPEED.BKF is NOT stock and this is the edit.
# Bytes were read back out of the archive through bkf.py and bpt.py
# rather than retyped from the table.
#
#   idx  name                cheat      stock CARAC2 on the retail disc
#    1   Gfx_acc             100        50
#    2   Gfx_spe             100        75
#   18   RealSpeedMax        110.0      80.0
#   19   VitesseMaxBoite     116.0      100.0
#   20   RotationTransfert   0.0026     0.003     <- the one LOWERED
#   36   RegimeCoupleMax     10500      10000
#   37   CoupleMax           60.0       15.0
#   39   CoupleRegimeMax     20.0       2.0
#
# Gfx_acc and Gfx_spe are the bike-select stat BARS, not physics - so
# a maxed bike also looks maxed on the page that chooses it.
CARAC2_HACK = (
    (1, "Gfx_acc", "64000000"),
    (2, "Gfx_spe", "64000000"),
    (18, "RealSpeedMax", "0000dc42"),
    (19, "VitesseMaxBoite", "0000e842"),
    (20, "RotationTransfert", "c3642a3b"),
    (36, "RegimeCoupleMax", "00102446"),
    (37, "CoupleMax", "00007042"),
    (39, "CoupleRegimeMax", "0000a041"),
)

# The availability column of the shipped DATA\LIST.BPT, in file order.
# --off restores THIS rather than zeroing, so putting a cheat back
# leaves the game as the disc left it. docs/formats/list-bpt.txt 3.
SHIPPED_AVAIL = (1, 1, 0, 0, 1, 0, 0, 1)

# Undo state for the carac cheat. The eight edited words are read back
# before the first write and kept here, because the values differ per
# bike CLASS and cannot be reconstructed from a constant.
SAVED = os.path.join(tempfile.gettempdir(), "mr_cheats_carac.json")

# Undo state for onelap. Snapshotted rather than tabulated because a
# data disk brings its own LIST*.BPT with its own tracks and its own
# lap counts, and there is no constant that covers those.
SAVED_LAPS = os.path.join(tempfile.gettempdir(), "mr_cheats_laps.json")


def _read(addr, size):
    reply = mr.pipe_command("GET_MEM %08x %d" % (addr, size))
    if not reply.startswith("OK"):
        return None
    return bytes.fromhex(reply.split()[-1])


def _i32(addr):
    raw = _read(addr, 4)
    if raw is None:
        return None
    return struct.unpack("<i", raw)[0]


def _hex32(addr):
    raw = _read(addr, 4)
    return None if raw is None else raw.hex()


def _hexn(addr, nbytes):
    """Read exactly as many bytes as the site writes.

    _hex32 reads four whatever the site is, which showed the
    five-byte nowalls patch as 83ec1489 -> 83ec1489 and called a
    correctly refused write a FAILURE.
    """
    raw = _read(addr, nbytes)
    return None if raw is None else raw.hex()


def _write(addr, hexstr, code=False):
    cmd = "SET_MEM %08x %s" % (addr, hexstr)
    if code:
        cmd += " CODE"
    return mr.pipe_command(cmd).startswith("OK")


def _i32hex(value):
    return struct.pack("<i", int(value)).hex()


def _cstr(raw):
    return raw.split(b"\x00")[0].decode("ascii", "replace")


def track_records():
    """[(index, name, availAddr, avail, laps)], or [] if not loaded."""
    base = _i32(ADDR_TRACKLIST)
    count = _i32(ADDR_TRACKCOUNT)
    if not base or not count or count < 1 or count > 64:
        return []
    out = []
    for i in range(count):
        rec = base + i * TRACK_STRIDE
        name = _cstr(_read(rec + TRACK_OFF_NAME, 32) or b"")
        avail = _i32(rec + TRACK_OFF_AVAIL)
        laps = _i32(rec + TRACK_OFF_LAPS)
        out.append((i, name, rec + TRACK_OFF_AVAIL, avail, laps))
    return out


def carac_addr(record, index):
    return ADDR_CARAC + record * CARAC_STRIDE + index * 4


def carac_save(records):
    """Snapshot the words a carac cheat is about to overwrite."""
    if os.path.exists(SAVED):
        return                          # keep the FIRST snapshot
    state = {}
    for rec in records:
        for index, _name, _hexv in CARAC2_HACK:
            addr = carac_addr(rec, index)
            was = _hex32(addr)
            if was is not None:
                state["%08x" % addr] = was
    if state:
        with open(SAVED, "w") as fh:
            json.dump(state, fh, indent=1)


def carac_restore_value(addr):
    """The saved word for addr, or None if nothing was snapshotted."""
    if not os.path.exists(SAVED):
        return None
    try:
        with open(SAVED) as fh:
            return json.load(fh).get("%08x" % addr)
    except (ValueError, OSError):
        return None


def laps_addr(index):
    base = _i32(ADDR_TRACKLIST)
    if not base:
        return None
    return base + index * TRACK_STRIDE + TRACK_OFF_LAPS


def laps_save(indices):
    """Snapshot the lap defaults before the first one-lap write."""
    if os.path.exists(SAVED_LAPS):
        return                          # keep the FIRST snapshot
    state = {}
    for i in indices:
        addr = laps_addr(i)
        was = _hex32(addr) if addr else None
        if was is not None:
            state["%08x" % addr] = was
    if state:
        with open(SAVED_LAPS, "w") as fh:
            json.dump(state, fh, indent=1)


def laps_restore_value(addr):
    """The saved lap count for addr, or None if none was snapshotted."""
    if not os.path.exists(SAVED_LAPS):
        return None
    try:
        with open(SAVED_LAPS) as fh:
            return json.load(fh).get("%08x" % addr)
    except (ValueError, OSError):
        return None


def sites_pocket():
    return [(ADDR_POCKET_UNLOCKED, _i32hex(1), _i32hex(0), "pocket unlocked"),
            (ADDR_POCKET_ENABLED, _i32hex(1), _i32hex(0), "pocket enabled")]


def sites_reverse():
    return [(ADDR_REVERSE_UNLOCKED, _i32hex(1), _i32hex(0),
             "reverse unlocked"),
            (ADDR_REVERSE_ENABLED, _i32hex(1), _i32hex(0),
             "reverse enabled")]


def sites_tracks():
    out = []
    for i, name, addr, _avail, _laps in track_records():
        shipped = SHIPPED_AVAIL[i] if i < len(SHIPPED_AVAIL) else 0
        out.append((addr, _i32hex(1), _i32hex(shipped), "track %s" % name))
    return out


def sites_onelap():
    """Every track's DEFAULT lap count to 1, not g_nRaceLaps."""
    recs = track_records()
    if not recs:
        return []
    laps_save([r[0] for r in recs])
    out = []
    for i, name, _availAddr, _avail, _laps in recs:
        addr = laps_addr(i)
        if addr is None:
            continue
        out.append((addr, _i32hex(1), laps_restore_value(addr),
                    "laps %s" % name))
    return out


def _carac_sites(records):
    carac_save(records)
    out = []
    for rec in records:
        for index, name, hexv in CARAC2_HACK:
            addr = carac_addr(rec, index)
            back = carac_restore_value(addr)
            out.append((addr, hexv, back, "bike %d %s" % (rec, name)))
    return out


# maybe_Trk_ClipMoveAgainstWalls @ 0x0040ca80, int __thiscall with
# six stack arguments - it ends RET 0x18 at 0x0040cd7e. Returning 0
# means "neither wall was crossed", so the caller does not reflect
# the move and does not push the bike out along the normal.
#
#     33 c0        xor eax, eax
#     c2 18 00     ret 0x18
#
ADDR_WALLCLIP = 0x0040ca80
WALLCLIP_OFF = "33c0c21800"
WALLCLIP_ON = "83ec14894c"      # sub esp,0x14 / mov [esp],ecx

# AND THE CONTAINMENT, WHICH IS A SECOND FUNCTION. FUN_0040cd90 tests
# the point against the segment's two wall planes and, when it is on
# the wrong side, PROJECTS IT BACK onto the plane with
# Plane_IntersectSegment. Patching only the reflecting clip above
# leaves this one holding the bike against the rail - it pinned x at
# exactly -1280.0 and slid the bike along it.
#
# void __thiscall, two stack args, ends RET 8 at 0x0040ce56.
#
#     c2 08 00     ret 8
#
ADDR_WALLPUSH = 0x0040cd90
WALLPUSH_OFF = "c20800"
WALLPUSH_ON = "8b4424"          # mov eax,[esp+..]


def sites_nowalls():
    """The one CODE site - a fifth element marks it."""
    return [(ADDR_WALLCLIP, WALLCLIP_OFF, WALLCLIP_ON,
             "guard rail reflect", True),
            (ADDR_WALLPUSH, WALLPUSH_OFF, WALLPUSH_ON,
             "guard rail containment", True)]


#
# THE COUNTDOWN IS ONE IMMEDIATE, and it is a CODE site.
#
#     00411293  MOV [0x004dc0b4],1        the countdown flag on
#     0041129d  MOV [0x004dc0a8],0x1770   6000 ms on the clock
#
# so the six seconds are the four bytes at 0041129d+6. Zero them and
# maybe_Race_TickClock takes the timer negative on its first tick and
# starts the race itself - the lap state, the racer reset and the
# input gate all run exactly as they would have six seconds later.
# Nothing here reimplements the start.
#
# IT IS THE COUNTDOWN, NOT THE TIME LIMIT, even though both live in
# 0x004dc0a8. The limit is written separately once the grid clears,
# so a race patched this way still has its full time allowance.
#
# THE 2 KEY DOES NOT NEED THIS AND IS BETTER WITH IT. mrhook posts
# ESC+ENTER to restart; that gets you to the grid, and this is what
# makes the grid go immediately.
#
ADDR_COUNTDOWN_MS = 0x004112a3
COUNTDOWN_OFF = "70170000"      # 6000 ms, as shipped
COUNTDOWN_ON = "00000000"       # none


def sites_nocountdown():
    return [(ADDR_COUNTDOWN_MS, COUNTDOWN_ON, COUNTDOWN_OFF,
             "race countdown ms", True)]


#
# THE 90 DEGREE CAMERA IS A VIEW THE GAME ALREADY HAS.
#
# nLookBackKey is not a flag. Camera_UpdateChase (0x0046f840) uses it
# as an INDEX into two tables in the camera record - pLookOffset[i*3]
# for where the eye sits, pLookAngles[i*3] for where it points - and
# slot 0 is the ordinary chase. Read live:
#
#     slot   offset x,y,z              yaw
#       0      0.00 -160.00 1680.00      0    the chase
#       1      0.00  160.00  -20.00    180    TAB, look back
#       2   -250.00  150.00    0.00     90    the LEFT side
#       3    250.00  150.00    0.00    -90    the RIGHT side
#       4/5  -+70.00 120.00  200.00    180    a pair, mirrors
#       6      0.00  400.00  400.00    180    raised, pitched -45
#       7      0.00  500.00  500.00    180    higher still
#
# so cam90 is slot 2 and cam90r is slot 3. It is the same view our
# port puts on the "1" key, and the only angle the fork steering and
# the wheelie pivot are visible from.
#
# flLookYaw IS NOT THE ANGLE. It is read only on the slot-0 path,
# where it feeds the boom length; the look branch never touches it.
# Writing pi/2 into it with slot 1 selected - which this used to do -
# gave the full 180 and ignored the pi/2 completely.
#
# THE RECORD IS PICKED AT RUN TIME. g_abCameraRecords is an array of
# MR_Camera (0x238 each) and 0x004dc0f8 says which one is live, so
# the index is read rather than assumed.
#
# IT DOES NOT SURVIVE TAB OR A RACE RESTART - both write the same
# field. The "1" hotkey in mrhook re-asserts it and does survive
# them; this is the one-shot version.
#
ADDR_CAMERA_RECORDS = 0x005161c0
ADDR_CAMERA_INDEX = 0x004dc0f8
CAMERA_STRIDE = 0x238
CAM_OFF_LOOKYAW = 0x03c
CAM_OFF_LOOKSLOT = 0x21c        # nLookBackKey

CAM_SLOT_LEFT = "02000000"
CAM_SLOT_RIGHT = "03000000"
CAM_SLOT_CHASE = "00000000"


def _cam_sites(slotHex):
    idx = _i32(ADDR_CAMERA_INDEX)
    if idx is None or idx < 0 or idx > 8:
        return []
    rec = ADDR_CAMERA_RECORDS + idx * CAMERA_STRIDE
    return [(rec + CAM_OFF_LOOKSLOT, slotHex, CAM_SLOT_CHASE,
             "camera look slot"),
            (rec + CAM_OFF_LOOKYAW, "00000000", "00000000",
             "camera look yaw - cleared, TAB leaves pi here")]


def sites_cam90():
    return _cam_sites(CAM_SLOT_LEFT)


def sites_cam90r():
    return _cam_sites(CAM_SLOT_RIGHT)

def sites_carac():
    return _carac_sites([0])


def sites_carac_all():
    return _carac_sites(list(range(CARAC_COUNT)))


PATCHES = (
    ("pocket", "the pocket bike - a second bike per track",
     sites_pocket),
    ("reverse", "reverse mode - every track ridden backwards",
     sites_reverse),
    ("tracks", "all eight islands selectable, not the shipped three",
     sites_tracks),
    ("onelap", "every track's lap default to 1 - so a single race is "
               "one lap and can be replayed",
     sites_onelap),
    ("carac", "bike 0 gets CARAC2's hacked performance figures",
     sites_carac),
    ("carac-all", "ALL EIGHT bikes get them - the AI too",
     sites_carac_all),
    ("nowalls", "ride through the guard rails - for comparing turns "
                "the track will not normally allow",
     sites_nowalls),
    ("nocountdown", "races start at once - no six-second grid wait",
     sites_nocountdown),
    ("cam90", "look 90 degrees left - the side view, for watching "
              "the fork and the wheelie pivot",
     sites_cam90),
    ("cam90r", "the same, 90 degrees right",
     sites_cam90r),
)


def find(name):
    for entry in PATCHES:
        if entry[0] == name:
            return entry
    return None


def cmd_list():
    print("toggles:")
    for name, summary, _fn in PATCHES:
        print("  %-10s %s" % (name, summary))
    print("\n  --on NAME / --off NAME, several at a time.")
    print("  --off restores the SHIPPED value, not zero.")
    print("\n  carac / carac-all apply to whichever bike class is")
    print("  LOADED NOW, so run them once the bike is chosen - road,")
    print("  dirt and pocket each load their own CARAC1..8.BPT, and")
    print("  Game_LoadBikeAndBanks calls Carac_Load which overwrites")
    print("  the table again on every bike change.")
    return 0


def cmd_status():
    if _i32(ADDR_POCKET_UNLOCKED) is None:
        print("the agent did not answer - is MOTO.EXE running?")
        return 1
    print("unlockables            unlocked  enabled")
    print("  pocket bike             %d         %d"
          % (_i32(ADDR_POCKET_UNLOCKED), _i32(ADDR_POCKET_ENABLED)))
    print("  reverse mode            %d         %d"
          % (_i32(ADDR_REVERSE_UNLOCKED), _i32(ADDR_REVERSE_ENABLED)))
    recs = track_records()
    if recs:
        print("\ntracks (%d)          avail  laps   shipped" % len(recs))
        for i, name, _addr, avail, laps in recs:
            shipped = SHIPPED_AVAIL[i] if i < len(SHIPPED_AVAIL) else 0
            was = laps_restore_value(laps_addr(i) or 0)
            flag = "" if avail == shipped else "   <- changed"
            if was is not None and _i32hex(laps) != was:
                flag += "   <- laps were %d" % struct.unpack(
                    "<i", bytes.fromhex(was))[0]
            print("  [%d] %-13s   %d     %d       %d%s"
                  % (i, name, avail, laps, shipped, flag))
    else:
        print("\ntrack list not loaded yet.")
    print("\nbikes, as loaded now   Gfx_acc Gfx_spe RealSpeedMax CoupleMax")
    for rec in range(CARAC_COUNT):
        acc = _i32(carac_addr(rec, 1))
        spe = _i32(carac_addr(rec, 2))
        smax = _read(carac_addr(rec, 18), 4)
        cmax = _read(carac_addr(rec, 37), 4)
        if smax is None or cmax is None:
            break
        smax = struct.unpack("<f", smax)[0]
        cmax = struct.unpack("<f", cmax)[0]
        hacked = (acc == 100 and spe == 100
                  and abs(smax - 110.0) < 0.01 and abs(cmax - 60.0) < 0.01)
        print("  [%d]                    %4d    %4d   %8.2f   %8.2f%s"
              % (rec, acc, spe, smax, cmax, "   <- CARAC2" if hacked else ""))
    if os.path.exists(SAVED):
        print("\ncarac undo snapshot: %s" % SAVED)
    if os.path.exists(SAVED_LAPS):
        print("laps  undo snapshot: %s" % SAVED_LAPS)
    return 0


def apply(names, turn_on):
    bad = [n for n in names if find(n) is None]
    if bad:
        sys.stderr.write("unknown toggle(s): %s\n" % ", ".join(bad))
        sys.stderr.write("try --list\n")
        return 2
    total = fails = skipped = 0
    for name in names:
        _n, _summary, fn = find(name)
        sites = fn()
        if not sites:
            print("%-10s no sites - is it loaded yet?" % name)
            continue
        print("%s %s:" % ("enabling" if turn_on else "restoring", name))
        for site in sites:
            addr, on_hex, off_hex, label = site[:4]
            code = len(site) > 4 and site[4]
            value = on_hex if turn_on else off_hex
            if value is None:
                print("  %08x  %-22s no saved value - skipped" % (addr, label))
                skipped += 1
                continue
            nbytes = len(value) // 2
            was = _hexn(addr, nbytes)
            ok = _write(addr, value, code)
            now = _hexn(addr, nbytes)
            total += 1
            if not ok or now != value:
                fails += 1
            print("  %08x  %-22s %s -> %s%s"
                  % (addr, label, was, now,
                     "" if ok and now == value else "   FAILED"))
    print("\n%d write(s), %d failed%s."
          % (total, fails, ", %d skipped" % skipped if skipped else ""))
    if turn_on:
        print("Runtime only - a fresh launch reads the .bpt files again.")
    return 1 if fails else 0


def main():
    args = sys.argv[1:]
    if not args:
        return cmd_status()
    if args[0] in ("--list", "-l"):
        return cmd_list()
    if args[0] in ("--status", "-s"):
        return cmd_status()
    if args[0] == "--all-on":
        return apply([p[0] for p in PATCHES if p[0] != "carac-all"], True)
    if args[0] == "--all-off":
        return apply([p[0] for p in PATCHES if p[0] != "carac-all"], False)
    if args[0] in ("--on", "--off"):
        names = args[1:]
        if not names:
            sys.stderr.write("%s needs at least one name; try --list\n"
                             % args[0])
            return 2
        return apply(names, args[0] == "--on")
    sys.stderr.write("unknown option %r; try --list\n" % args[0])
    return 2


sys.exit(main())
