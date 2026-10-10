#!/usr/bin/env python3
"""mr_freeze.py - hold the race clock so you can ride at your own pace.

    python tools/mr_freeze.py             hold it, Ctrl-C to stop
    python tools/mr_freeze.py --status    report, change nothing
    python tools/mr_freeze.py --set 1500  set it once and exit
    python tools/mr_freeze.py --value 120000 --floor 90000

THE CHECKPOINT TIMER AND THE START COUNTDOWN ARE THE SAME WORD.
0x004dc0a8 counts down in milliseconds; while 0x004dc0b4 is set it is
the six-second countdown, and the moment that clears the game reloads
the same word with the race limit - 120000 in a network game, a
per-difficulty value from 0x00515fe0 otherwise. See
docs/subsystems/game-object.txt, RACE COUNTDOWN.

So holding the clock up is all it takes to stop the checkpoint
timeout, and it needs nothing in the DLL: this is a loop over GET_MEM
and SET_MEM, the same composition rule tools/mr_reset.py follows.

IT WILL NOT TOUCH THE COUNTDOWN. While 0x004dc0b4 is set the script
waits, because writing there would stop the race ever starting - the
race BEGINS when this word goes negative. That is also why it tops up
rather than pinning: a value that never moves is a value the game can
never use to make a transition.

o THIS IS A DIAGNOSTIC OVERRIDE, and it is off unless you run it.
  Nothing in the DLL changes, nothing persists, and the moment you
  stop the script the clock drains normally again. It exists so a lap
  can be ridden slowly while something else is being measured - not
  to make a race easier to win.
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bridge_mcp_mrhook as mr          # noqa: E402

ADDR_CLOCK = 0x004dc0a8                 # ms, counts down
ADDR_COUNTDOWN = 0x004dc0b4             # non-zero while the countdown runs
ADDR_PAUSE = 0x004dc038

DEFAULT_VALUE = 120000                  # what to top up to
DEFAULT_FLOOR = 60000                   # top up once it falls below this
# POLITE ON PURPOSE. This script is meant to be left running while
# other tools use the same one-instance pipe, so it holds it as little
# of the time as it can get away with. The clock only has to be topped
# up before it drains, which is a matter of tens of seconds.
POLL_S = 0.5


def _i32(addr):
    reply = mr.pipe_command("GET_MEM %08x 4" % addr)
    if not reply.startswith("OK"):
        return None
    raw = bytes.fromhex(reply.split()[-1])
    return int.from_bytes(raw, "little", signed=True)


def _write(addr, value):
    data = int(value).to_bytes(4, "little", signed=True).hex()
    return mr.pipe_command("SET_MEM %08x %s" % (addr, data)).startswith("OK")


def status():
    clock = _i32(ADDR_CLOCK)
    if clock is None:
        return "the agent did not answer - is MOTO.EXE running?"
    down = _i32(ADDR_COUNTDOWN)
    paused = _i32(ADDR_PAUSE)
    where = ("COUNTDOWN" if down else "racing") + (", PAUSED" if paused else "")
    return "clock %d ms (%.1f s), %s" % (clock, clock / 1000.0, where)


def freeze(value, floor):
    print("holding the race clock at %d ms once it drops below %d."
          % (value, floor))
    print("Ctrl-C to stop. The countdown is left alone.\n", flush=True)
    topped = 0
    last = None
    try:
        while True:
            down = _i32(ADDR_COUNTDOWN)
            clock = _i32(ADDR_CLOCK)
            if clock is None:
                print("\nlost the agent - stopping.")
                return 1
            if down:
                state = "countdown, waiting"
            elif clock < floor:
                _write(ADDR_CLOCK, value)
                topped += 1
                state = "topped up (%d so far)" % topped
            else:
                state = "ok"
            line = "  clock %7d ms  %-28s" % (clock, state)
            if line != last:
                sys.stdout.write("\r" + line)
                sys.stdout.flush()
                last = line
            time.sleep(POLL_S)
    except KeyboardInterrupt:
        print("\n\nstopped. The clock drains normally from here.")
        print("  %s" % status())
        return 0


def main():
    args = sys.argv[1:]
    value, floor = DEFAULT_VALUE, DEFAULT_FLOOR
    while args:
        a = args.pop(0)
        if a == "--status":
            print(status())
            return 0
        if a == "--set":
            #
            # SET IT ONCE AND LEAVE. For reaching the end of the
            # clock without waiting out a race - the timeout line
            # lives in the 500 ms between the clock passing zero
            # and game over, so it is not reachable by watching.
            #
            #   --set 1500   a second and a half left
            #   --set -200   INSIDE the timeout window
            #   --set -600   past it: game over on the next tick
            #
            # It refuses while the grid countdown is up, for the
            # same reason freeze() waits: the race BEGINS when
            # this word goes negative, so writing it there stops
            # the race ever starting.
            #
            v = int(args.pop(0))
            if _i32(ADDR_COUNTDOWN) != 0:
                print("the grid countdown is up - not touching it")
                return 2
            before = _i32(ADDR_CLOCK)
            _write(ADDR_CLOCK, v)
            print("  clock %d -> %d ms" % (before, _i32(ADDR_CLOCK)))
            return 0
        if a == "--value":
            value = int(args.pop(0))
        elif a == "--floor":
            floor = int(args.pop(0))
        else:
            print(__doc__.strip().split("\n\n")[1])
            return 2
    if floor >= value:
        print("--floor must be below --value")
        return 2
    print(status())
    return freeze(value, floor)


if __name__ == "__main__":
    sys.exit(main())
