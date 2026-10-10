#!/usr/bin/env python3
"""mr_reset.py - restart the race on either game, or both.

    python tools/mr_reset.py            restart BOTH
    python tools/mr_reset.py --to moto        MOTO only
    python tools/mr_reset.py --to mracer      the port only
    python tools/mr_reset.py --state    say where they are, change
                                        nothing

BOTH IS THE DEFAULT BECAUSE A ONE-SIDED RESET IS WORSE THAN
NONE. Resetting MOTO alone and then copying its racers over
with mr_place --grid leaves the PORT's race phase untouched -
the bikes line up, the drive gate does not, and the same
throttle produces a different answer every run. That cost a
day: engineSpeed after ten frames came back 1.2843 twice and
then 6.5771, and two confident conclusions were drawn off the
first pair before the third showed them up.

COMPOSED FROM THE COMMANDS THAT ALREADY EXIST - SEND_INPUT, GET_MEM
and SET_MEM. Nothing is added to the DLL, so changing the sequence
costs an edit rather than a rebuild and a redeploy. It talks to the
pipe through bridge_mcp_mrhook rather than reimplementing it.

THE COUNTDOWN IS TWO GLOBALS AND NEEDS NO PATCH [verified]:

    0x00411293  MOV [0x004dc0b4], 1       countdown flag on
    0x0041129d  MOV [0x004dc0a8], 0x1770  6000 ms on the clock

and FUN_0047dae0, the race tick, does

    DAT_004dc0a8 -= DAT_004e9810;         by the frame delta
    if (DAT_004dc0a8 < 0 && DAT_004dc0b4)
        DAT_004dc0b4 = 0;                 ... and the race begins

SO DRIVING THE TIMER NEGATIVE MAKES THE GAME START THE RACE ITSELF.
Clearing lap state, resetting the racers, arming the input gate - all
of it runs exactly as it would have six seconds later. Nothing here
reimplements the start, which is the reason to do it this way rather
than by poking the flag directly.

The countdown is 6000 ms, not the three it looks like: the rider
settle keys off (2000 - DAT_004dc0a8), so only the last two seconds
carry the visible animation.
"""

import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bridge_mcp_mrhook as mr          # noqa: E402

ADDR_TIMER = 0x004dc0a8                 # ms, counts down
ADDR_COUNTDOWN = 0x004dc0b4             # non-zero while it runs
ADDR_FRAMES = 0x004e981c                # ++ every rendered frame
ADDR_PAUSE = 0x004dc038                 # the pause toggle's state

#
# THE COUNTDOWN CAN BE PATCHED OUT FROM UNDER THIS SCRIPT, and it
# changes what a successful restart looks like. mrhook zeroes the
# 0x1770 immediate at 0x004112a3 when NoCountdown=1 is set in its
# ini, so the countdown flag goes up and comes down inside a
# single tick.
#
# WAITING FOR THAT FLAG THEN NEVER SEES IT. The wait polls every
# 10 ms against a frame that lasts about 33, so the window is
# missed more often than not and a restart that worked perfectly
# is reported as "ESC+ENTER did not restart it".
#
# So the patch is DETECTED rather than worked around: read the
# four bytes, and if they are zero skip both the wait and the
# timer drive, because the game has already done the job.
#
ADDR_COUNTDOWN_MS = 0x004112a3

VK_ESCAPE = "1b"
VK_RETURN = "0d"

WAIT_S = 6.0                            # max per phase
POLL_S = 0.01


def _dword(addr):
    """One dword, or None if the read did not come back as expected."""
    reply = mr.pipe_command("GET_MEM %08x 4" % addr)
    if not reply.startswith("OK"):
        return None
    hexbytes = reply.split()[-1]
    if len(hexbytes) != 8:
        return None
    return int.from_bytes(bytes.fromhex(hexbytes), "little")


def _countdown_patched():
    """True when mrhook has zeroed the countdown immediate."""
    reply = mr.pipe_command("GET_MEM %08x 4" % ADDR_COUNTDOWN_MS)
    if not reply.startswith("OK"):
        return False
    return reply.split()[-1] == "00000000"


def _wait_countdown(want, deadline):
    """Wait for the countdown flag to match. Returns (ok, seconds)."""
    start = time.time()
    while True:
        flag = _dword(ADDR_COUNTDOWN)
        if flag is None:
            return False, time.time() - start
        if (flag != 0) == want:
            return True, time.time() - start
        if time.time() - start >= deadline:
            return False, time.time() - start
        time.sleep(POLL_S)


def state():
    """Where is the game? Frames advancing with the race timer frozen
    means a menu, which is the case RESET cannot dig itself out of."""
    a = _dword(ADDR_FRAMES)
    time.sleep(0.25)
    b = _dword(ADDR_FRAMES)
    if a is None or b is None:
        return "no answer from the pipe - is MOTO.EXE up with the "\
               "hook deployed?"

    timer = _dword(ADDR_TIMER)
    flag = _dword(ADDR_COUNTDOWN)
    running = (b != a)
    if not running:
        # A PAUSED GAME RENDERS NOTHING, so a frozen counter is the
        # normal case rather than a fault - and every reading taken
        # here is taken while paused. Say which it is.
        if _dword(ADDR_PAUSE):
            # SAY WHEN THE COUNTDOWN CANNOT HAPPEN. Without this the
            # paused line looks the same patched or not, and a race
            # that starts instantly reads as a race that has already
            # started - two different things to anyone debugging.
            if flag:
                why = ", in the countdown"
            elif _countdown_patched():
                why = ", countdown patched out"
            else:
                why = ""
            return "PAUSED at frame %d, %d ms on the clock%s" % (
                b, timer, why)
        return "frames are not advancing (%d) and it is not paused - "\
               "the game is stopped or minimised" % b

    t0 = timer
    time.sleep(0.25)
    t1 = _dword(ADDR_TIMER)
    if flag:
        return "in the COUNTDOWN, %d ms left" % timer
    if _countdown_patched():
        return "RACING, %d ms on the clock (countdown patched out)" % timer
    if t1 == t0:
        return "IN A MENU - the race timer is frozen at %d while "\
               "frames advance. Nothing here can restart it; the "\
               "race has to be re-entered by hand." % timer
    return "RACING, %d ms on the clock" % timer


def reset(log=print):
    """ESC, ENTER, then hold the timer negative. True if the race
    restarted."""
    # BOTH TAPS GO IN ONE SEND_INPUT. After ESC the menu is up, and
    # SEND_INPUT cannot pause with a menu up - so a second call would
    # be refused before its tap was ever posted. Posting is ordered,
    # so the game digests ESC then ENTER over the frames that follow.
    reply = mr.pipe_command("SEND_INPUT 0 18 %s %s"
                            % (VK_ESCAPE, VK_RETURN), 10000)
    log("  keys: %s" % reply.strip())

    if _countdown_patched():
        # NOTHING TO SKIP. Hand it a few frames so the restart
        # lands, then say what it is doing rather than waiting for
        # a flag that is already gone.
        mr.pipe_command("SEND_INPUT 0 6", 10000)
        log("  countdown patched out - racing")
        return True

    ok, secs = _wait_countdown(True, WAIT_S)
    if not ok:
        log("  no countdown after %.2fs - ESC+ENTER did not restart "
            "it. %s" % (secs, state()))
        return False
    log("  countdown armed after %.2fs" % secs)

    # THE GAME HAS TO BE RUNNING FOR THE TICK TO CONSUME THE VALUE,
    # and SEND_INPUT re-pauses as soon as its run ends. Writing the
    # timer in a sleep loop therefore writes it over and over into a
    # frozen game and nothing ever happens - which is exactly what
    # the first version of this did. So: write, then hand the game
    # some frames, then look.
    start = time.time()
    for _ in range(8):
        mr.pipe_command("SET_MEM %08x ffffffff" % ADDR_TIMER)
        mr.pipe_command("SEND_INPUT 0 6", 10000)
        if _dword(ADDR_COUNTDOWN) == 0:
            log("  countdown skipped in %.2fs - racing"
                % (time.time() - start))
            return True

    log("  timer would not go negative. %s" % state())
    return False


# ------------------------------------------------------------------
# OUR PORT
#
# '2' IS THE RESTART KEY ON BOTH GAMES. MOTO takes it too; this
# file drives MOTO through ESC+ENTER only because that path was
# written first and is known to work through its menu.
#
# THERE IS NO COUNTDOWN TO SKIP ON THE PORT. MR_RaceClock exists
# and nothing ticks it - s_race_clock.preRace reads 0 on the grid
# - so the restart is racing the moment the key lands, and the
# timer drive MOTO needs has no counterpart here. If the clock is
# ever wired, this is the function that has to learn about it.
# ------------------------------------------------------------------

MRACER_LAYOUT = 'g_race_layout'
MRACER_RACERS = 's_race_racers'
MRACER_VIEWED = 1                       # race_viewed_racer()


def _mracer(cmd, timeout_ms=None):
    if timeout_ms is None:
        return mr.pipe_command(cmd, target='mracer')
    return mr.pipe_command(cmd, timeout_ms, target='mracer')


def _mracer_speed():
    """The viewed racer's speed, or None if the port will not say.

    THE OFFSETS COME OUT OF THE PORT, not out of racer.h. Reading
    g_race_layout is what keeps this file working when MR_Racer
    grows a field - which it has, twice.
    """
    try:
        reply = _mracer('GET_MEM name=%s len=24' % MRACER_LAYOUT)
        if not reply.startswith('OK'):
            return None
        raw = bytes.fromhex(reply.split(chr(10))[1].strip())
        if len(raw) < 24:
            return None
        stride, _posOff, _mv, _seg, speedOff, _gear = \
            struct.unpack('<6i', raw)
        off = MRACER_VIEWED * stride + speedOff
        reply = _mracer('GET_MEM name=%s len=4 offset=%d'
                        % (MRACER_RACERS, off))
        if not reply.startswith('OK'):
            return None
        raw = bytes.fromhex(reply.split(chr(10))[1].strip())
        return struct.unpack('<f', raw)[0]
    except Exception:
        return None


def mracer_state():
    """One line on where the port is."""
    reply = _mracer('STATUS')
    if not reply.startswith('OK'):
        return 'no answer from the port pipe - is mracer.exe up?'
    speed = _mracer_speed()
    if speed is None:
        return ('up, but the racer layout would not read - is this a'
                ' Debug build with its .map?')
    return 'up, viewed racer at %.4f m/s' % speed


def reset_mracer(log=print):
    """Tap 2. True when the viewed racer comes back to rest."""
    reply = _mracer('SEND_INPUT tap=2 frames=8', 10000)
    log('  mracer keys: %s' % reply.strip())
    if not reply.startswith('OK'):
        return False

    speed = _mracer_speed()
    if speed is None:
        log('  mracer restarted, but its speed would not read')
        return False

    # A GRID BIKE IS STOPPED. Anything else means the key did not
    # reach the race - a menu swallowed it, or the page was not the
    # race at all.
    if abs(speed) > 0.5:
        log('  mracer did NOT restart - still rolling at %.4f m/s'
            % speed)
        return False
    log('  mracer on the grid, %.4f m/s' % speed)
    return True


def main():
    args = sys.argv[1:]
    target = "both"

    if args[:1] == ["--to"] and len(args) > 1:
        target = args[1]
        args = args[2:]

    if args[:1] == ["--state"]:
        if target in ("both", "moto"):
            print("moto  : %s" % state())
        if target in ("both", "mracer"):
            print("mracer: %s" % mracer_state())
        return 0

    if args or target not in ("both", "moto", "mracer"):
        print(__doc__.strip().splitlines()[0])
        print("usage: mr_reset.py [--to moto|mracer|both] [--state]")
        return 2

    print("resetting:")
    ok = True
    if target in ("both", "moto"):
        ok = reset() and ok
        print("  moto  : %s" % state())
    if target in ("both", "mracer"):
        ok = reset_mracer() and ok
        print("  mracer: %s" % mracer_state())
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
