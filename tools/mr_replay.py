#!/usr/bin/env python3
"""mr_replay.py - read and decode the in-memory replay recording.

    python tools/mr_replay.py                 summary + the first 40 frames
    python tools/mr_replay.py --all           every frame
    python tools/mr_replay.py --at 112963     the frames around a race time
    python tools/mr_replay.py --csv FILE      write the whole track out

The game records EVERY race into a static array whether or not you ask
it to keep one, so this reads out the race just ridden without the
player doing anything. See docs/subsystems/replay.txt.

    0x00558aa8  g_abReplayBuffer   8 bytes per frame
    0x004e9838  the record COUNT
    0x004e983c  g_replayFrame, the playback position
    0x004e9834  g_dwReplayMode, 0 live / 1 playback / 2 demo

THE RACE CLOCK IS RECONSTRUCTED, NOT STORED. Each record carries only
its own frame delta in milliseconds, so a time is the running sum of
every delta before it. That is also exactly how the game re-times a
playback, which is why the reconstruction lines up with the race clock
captured from a hook to the millisecond.
"""
import argparse
import os
import struct
import sys
import time

sys.path.insert(0, os.path.join('N:', os.sep, 'DEV', 'MOTORACER', 'tools'))
import bridge_mcp_mrhook as mr          # noqa: E402
import mr_dump_racer as dump            # noqa: E402

ADDR_BUFFER = 0x00558aa8
ADDR_COUNT = 0x004e9838
ADDR_FRAME = 0x004e983c
ADDR_MODE = 0x004e9834
ADDR_TOTALMS = 0x004e9814

REC = 8
MAX_RECORDS = 0x4fb0                    # 20400 - the cap the writer tests

# THE LAST TWO BITS ARE NOT KEYS. Input_OnKeyDown's seven actions only
# reach bits 0..5: its case 6, Turbo/Stunts, sets no bit at all, it
# writes racer+0x31f9 and +0x3201 directly. maybe_Replay_WriteInput
# then folds those two fields back in as bits 7 and 6 so the recording
# can reproduce a wheelie [verified]:
#
#     racer[+0x31f9] ? (w | 0x80) : (w & ~0x80)    held
#     racer[+0x3201] ? (w | 0x40) : (w & ~0x40)    the press edge
BUTTONS = ((0x01, "accel"), (0x02, "brake"), (0x04, "left"),
           (0x08, "right"), (0x10, "gear up"), (0x20, "gear down"),
           (0x40, "STUNT-edge"), (0x80, "STUNT"))


def buttons(word):
    on = [n for b, n in BUTTONS if word & b]
    rest = word & ~0xff                  # anything above bit 7 is unknown
    if rest:
        on.append("0x%x" % rest)
    return "+".join(on) if on else "-"


def read_state():
    b = dump.read_range(ADDR_MODE, 12)
    if b is None:
        raise SystemExit("could not read the replay globals - is the game up?")
    mode, count, frame = struct.unpack_from("<III", b, 0)
    return mode, count, frame


def read_records(count):
    """Pull count records out in READ_MAX-sized chunks."""
    want = count * REC
    out = bytearray()
    t0 = time.time()
    while len(out) < want:
        n = min(dump.READ_MAX, want - len(out))
        blk = dump.read_range(ADDR_BUFFER + len(out), n)
        if blk is None:
            raise SystemExit("read failed at +%d" % len(out))
        out += blk
    return bytes(out), time.time() - t0


def decode(raw, count):
    """[(frame, ms_at_start, delta, input, axis0, axis1)]"""
    rows = []
    t = 0
    for i in range(count):
        w, a0, a1 = struct.unpack_from("<IHH", raw, i * REC)
        delta = (w >> 24) & 0xff
        rows.append((i, t, delta, w & 0xffffff, a0, a1))
        t += delta
    return rows


def show(rows, lo, hi):
    print("  frame     ms   dt  input     buttons")
    for f, t, d, w, a0, a1 in rows[lo:hi]:
        ax = ("" if (a0, a1) == (0x8000, 0x8000)
              else "  axes %04x %04x" % (a0, a1))
        print("  %5d  %6d  %3d  %06x  %-28s%s" % (f, t, d, w, buttons(w), ax))


def transitions(rows):
    """Only the frames where the button word CHANGES - the whole ride in
    a page or two, because a held key repeats for thirty frames."""
    out = []
    prev = None
    for r in rows:
        if r[3] != prev:
            out.append(r)
            prev = r[3]
    return out


def main():
    ap = argparse.ArgumentParser(description="Decode the replay recording.")
    ap.add_argument("--all", action="store_true", help="every frame")
    ap.add_argument("--changes", action="store_true",
                    help="only frames where the buttons change")
    ap.add_argument("--at", type=int, metavar="MS",
                    help="the frames around a race-clock time")
    ap.add_argument("--window", type=int, default=20,
                    help="frames either side of --at (default 20)")
    ap.add_argument("--csv", metavar="FILE", help="write the whole track")
    args = ap.parse_args()

    mode, count, frame = read_state()
    names = {0: "live (recording)", 1: "playback", 2: "demo?"}
    print("replay: mode %d - %s" % (mode, names.get(mode, "?")))
    if not 0 < count <= MAX_RECORDS:
        raise SystemExit("record count %d is out of range - nothing recorded?"
                         % count)
    raw, secs = read_records(count)
    rows = decode(raw, count)
    total = rows[-1][1] + rows[-1][2]
    print("records: %d of %d capacity   playback at frame %d"
          % (count, MAX_RECORDS, frame))
    print("length:  %d ms (%d:%05.2f)   mean dt %.1f ms = %.1f fps"
          % (total, total // 60000, (total % 60000) / 1000.0,
             float(total) / count, 1000.0 * count / total))
    print("read:    %d bytes in %.1f s (%.0f KB/s)\n"
          % (len(raw), secs, len(raw) / 1024.0 / max(secs, 1e-6)))

    if args.csv:
        with open(args.csv, "w", encoding="ascii", newline="") as fh:
            fh.write("frame,ms,dt,input,buttons,axis0,axis1\n")
            for f, t, d, w, a0, a1 in rows:
                fh.write("%d,%d,%d,%06x,%s,%d,%d\n"
                         % (f, t, d, w, buttons(w), a0, a1))
        print("wrote %s" % args.csv)
        return

    if args.at is not None:
        i = min(range(len(rows)), key=lambda k: abs(rows[k][1] - args.at))
        print("nearest frame to %d ms is %d (%d ms)"
              % (args.at, i, rows[i][1]))
        show(rows, max(0, i - args.window), i + args.window)
    elif args.changes:
        ch = transitions(rows)
        print("%d button changes in %d frames\n" % (len(ch), len(rows)))
        show(ch, 0, len(ch))
    elif args.all:
        show(rows, 0, len(rows))
    else:
        show(rows, 0, 40)
        print("  ... %d more; --all, --changes, --at MS or --csv FILE"
              % (len(rows) - 40))


main()
