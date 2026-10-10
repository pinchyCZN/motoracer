#!/usr/bin/env python3
"""mr_dump_racer.py - lay out a live racer record in full.

    python tools/mr_dump_racer.py              racer 1, everything
    python tools/mr_dump_racer.py --racer 2    another slot
    python tools/mr_dump_racer.py --parts      just the skeleton
    python tools/mr_dump_racer.py --slot 1     parts from block 1
    python tools/mr_dump_racer.py --raw 2ea9 64   hex at an offset

ONE SNAPSHOT, THEN EVERYTHING IS SLICED LOCALLY. The record is 0x3209
bytes, which is four GET_MEM calls at the 4096-byte limit; every
table below comes out of that one read rather than a round trip each.
That also makes the whole dump self-consistent - two fields printed
here were read in the same instant, which hand-slicing across separate
reads never guaranteed.

WHY THIS EXISTS. The offsets below were worked out over several
sessions and re-derived by hand each time, from raw hex, with a fresh
chance of an off-by-one on every pass - and the phase of the part
array was got wrong TWICE that way. See docs/structs/MR_Racer.txt and
MR_RacerPart.txt; this file is those documents made executable, and if
the two disagree the documents win.

THE TAIL IS BYTE-MISALIGNED. Everything after the player name at
+0x2dd4 sits at 1 mod 4 - +0x2e1d, +0x2e55, +0x3085, +0x319d. That is
not a transcription error and it is why struct.unpack_from is used
throughout rather than array indexing.
"""

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bridge_mcp_mrhook as mr          # noqa: E402

G_RACERS = 0x00581218
STRIDE = 0x3209
READ_MAX = 4096

INST_BASE = 0x0024
INST_SIZE = 0x0184
INST_COUNT = 4
INST_NAME = ("body", "front wheel", "rear wheel", "SKELETON")

POSE_BASE = 0x063c
POSE_SLOTS = 5
POSE_PARTS = 20
PART_SIZE = 100

OFF_CAMDIST = 0x0634
OFF_SLOT = 0x0638
OFF_PARTCOUNT = 0x2d4c
OFF_PTR_A = 0x2d60
OFF_PTR_B = 0x2d74

# Fields that live INSIDE instance 0 - see MR_Racer.txt 3.1. They are
# racer offsets, not instance offsets, and that overlap is real.
SCALARS = (
    (0x0000, "i", "drawn this frame"),
    (0x0004, "i", "solo mode"),
    (0x0008, "i", "human player"),
    (0x0030, "v", "position"),
    (0x0054, "f", "FRAME DELTA at tick time - Racer_SavePrevFrame"
                  " copies g_frameDelta here"),
    (0x0068, "i", "active"),
    (0x0078, "i", "bike index into g_bikeCarac"),
    (0x007c, "i", "GEAR, 1..NbSpeed"),
    (0x0080, "i", "shift timer, ms"),
    (0x0084, "i", "shift state: 0 idle, 1 up, 2 down"),
    (0x0088, "i", "GATES THE 1.1x TORQUE MULTIPLIER."
                  " Engine_TorqueAt applies it only when"
                  " g_codeCheckPassed is set AND this is 0. READ 1"
                  " LIVE with the bike stationary in gear 1 and the"
                  " shift state idle, while g_codeCheckPassed read"
                  " 1 - so the multiplier was NOT being applied, and"
                  " thirteen captured acceleration frames confirm"
                  " it: every one matches the curve without the"
                  " 1.1x. Sits inside the gearbox block (+0x7c gear,"
                  " +0x80 timer, +0x84 state, +0x8c pending) but is"
                  " not a shift-in-progress flag - the state was"
                  " idle. See racers-convergence.txt 9.20"
                  " [open: what writes it]"),
    (0x008c, "i", "gears the pending downshift wants"),
    (0x00b4, "f", "RealSpeedMax, m/s - carac field 18"),
    (0x00ac, "i", "TURN-AND-BRAKE FLAG. IT GATES BRAKING - "
                  "maybe_Physics_TickDrivetrain reads it at"
                  " 0x0048f915 and skips the whole deceleration"
                  " block when it is 0. maybe_Racer_ApplySteering"
                  " sets it to 1 UNCONDITIONALLY at 0x00466504,"
                  " every racer every frame; only a HUMAN on a"
                  " MotoType 1 bike gets past the two guards at"
                  " 0x0046651d and 0x00466532 that can clear it."
                  " So a street bike and every AI always have it"
                  " set, and ONLY THE PLAYER ON A DIRT BIKE cannot"
                  " brake in a corner. Cleared while braking and"
                  " turning, or with the steer axis outside"
                  " 0x4000..0xc000; forced back to 1 below speed"
                  " 2.0. See racers.txt 9.21.4"),
    (0x00b8, "f", "ENGINE TORQUE MULTIPLIER. maybe_Racer_UpdateWheelie"
                  " sets 1.0 when the wheelie angle is zero, and"
                  " while it is not: 2.0 if g_codeCheckPassed is"
                  " set (0x004661da) or 1.2 if not (0x004661e8)."
                  " Engine_TorqueAt returns curve * this, so a"
                  " wheelie DOUBLES torque on a stock copy and is"
                  " worth only 20 per cent on a cracked one."
                  " CORRECTED: this said 1.6 for a long time, but"
                  " the immediate is 0x3f99999a = 1.2 - 1.6 would"
                  " be 0x3fcccccd. See racers.txt 9.23"),
    (0x00ec, "f", "engine-side speed (see MR_Racer.txt 9.10)"),
    (0x00f0, "f", "AIR TIME. += 2.592 per frame while +0x2db8"
                  " (onGround) is 0, reset to 0 the moment it is"
                  " not - the two arms of one if in"
                  " maybe_Racer_UpdateAirborne, 0x00466682 and"
                  " 0x0046668d. maybe_Physics_TickDrivetrain tests"
                  " it as (bits & 0x7fffffff) != 0 at 0x0048ef70,"
                  " which selects the 1.6 drag coefficient over 0.3"
                  " and gates the slip path. Ladder verified live"
                  " over 8 airborne frames - racers.txt 9.18"),
    (0x00f8, "f", "scales the steering self-centring rate -"
                  " maybe_Racer_ApplySteering multiplies it by"
                  " RoueVitesseRetour, then floors the result at"
                  " RoueVitesseRetour [open: what writes it]"),
    (0x0108, "x", "a POINTER - maybe_Physics_TickDrivetrain reads"
                  " ptr[2] and tests it against 0 [open]"),
    (0x0110, "f", "steering, pi/4096 units"),
    (0x010c, "f", "STEERING ANGULAR RATE - integrated by"
                  " maybe_Racer_ApplySteering at"
                  " RoueAccelerationAngulaire * dt while a turn is"
                  " held, zeroed when neither turn button is down."
                  " Sits immediately below steering at +0x110"),
    (0x0114, "i", "speed, raw km/h"),
    (0x0118, "i", "speed, needle km/h"),
    (0x011c, "i", "tachometer rpm"),
    (0x0120, "i", "NEEDLE RATE - the speedo needle's rate of"
                  " change, clamped to [-50, +40]"),
    (0x0124, "f", "speed, m/s"),
    (0x0128, "f", "PREVIOUS SPEED - maybe_Physics_TickDrivetrain"
                  " saves +0x124 on entry and stores it here as its"
                  " last statement"),
    (0x012c, "f", "GEARBOX road speed - the rev limiter writes here"),
    (0x0130, "v", "VELOCITY DIRECTION - normalised, then scaled by"
                  " speed and rotated into body space by the inverse"
                  " of matOrientation, six times over in"
                  " maybe_Physics_TickDrivetrain"),
    (0x013c, "i", "DRIVETRAIN DISENGAGED - cleared every call, set to"
                  " 1 only for MotoType 1 while coasting with no"
                  " brake. While set, the gearbox is not resynced"
                  " from road speed"),
    (0x018c, "v", "movement direction, unit - slid along walls"),
    (0x0180, "f", "maybe_Racer_Tick clears +0x000c and +0x0010 when this"
                  " is +-0.0 [open: what writes it]"),
    (0x0184, "f", "previous |lateral| - maybe_Racer_EmitWheelFx compares"
                  " this frame's against it to decide whether the skid"
                  " is widening or narrowing, then stores the new one"),
    (0x0188, "f", "SKID WIDTH, clamped to 1.0..12.0. Grows by a"
                  " constant while |lateral| exceeds +0x0184 and shrinks"
                  " by 1.0 otherwise. It is the half-width of the two"
                  " skid ribbons, and it is fixed at 5.0 for an AI"),
    (0x01b4, "v", "FRONT wheel contact point"),
    (0x0338, "v", "REAR wheel contact point"),
    (0x0634, "f", "distance to the camera / projection zoom"),
    (0x0638, "i", "skeleton slot / LOD index"),
    (0x2da8, "f", "speed, m/s (tail copy)"),
    (0x2dac, "x", "rider state bits"),
    (0x2db8, "i", "on the ground"),
    (0x2dbc, "i", "ON THE GROUND, previous frame - Racer_SavePrevFrame"),
    (0x003c, "v", "position, PREVIOUS frame"),
    (0x0048, "v", "position, TWO frames ago"),
    (0x01c0, "v", "front wheel contact, previous frame"),
    (0x0344, "v", "rear wheel contact, previous frame"),
    (0x2da4, "i", "previous +0x2da0"),
    (0x2e1d, "f", "LEAN, radians"),
    (0x2e21, "f", "lean, previous frame"),
    (0x2e31, "f", "decayed toward zero, fed to a euler"),
    (0x2e39, "f", "fork dive/lift, CLAMPED +-11.25 deg - NOT the"
                  " wheelie"),
    (0x2e85, "i", "stunting or overspeeding, from +0x31f9"),
    (0x2e8d, "f", "SUSPENSION ANGLE - clamped +-AngleSuspension"),
    (0x2e91, "f", "vertical bounce = sin(+0x2e99) * +0x2e9d"),
    (0x2e95, "f", "spring compression, forced <= 0 each frame"),
    (0x2e99, "f", "spring phase, snapped to pi/2 on overrun"),
    (0x2e9d, "f", "spring amplitude, decays to RessorAmplitudeMin"),
    (0x2ea1, "f", "smoothed chassis heading, lags by RetardCadre"),
    (0x2ea5, "f", "heading reference for +0x2ea1"),
    (0x30b5, "v", "rider position - centre of the 2nd shadow quad"),
    (0x30c1, "i", "CRASH LATCH - rider off the bike"),
    (0x30c5, "f", "400.0"),
    (0x30c9, "v", "crash launch direction"),
    (0x30d5, "f", "crash timer, speed * 0.15"),
    (0x30dd, "f", "which side he falls, +-pi/2"),
    (0x2ddd, "i", "RACE CLOCK, ms - the same in every racer"),
    (0x2de1, "i", "BEST LAP, ms. 0x7f7f7f7f = none yet"),
    (0x2f71, "i", "Fx POOL SLOT, 0..15 - maybe_Fx_AllocSlot"),
    (0x2f79, "h", "checkpoint index, 16 bit"),
    (0x3065, "i", "STUNT POSE ID - g_anStuntPoseIndex[+0x3061]"),
    (0x2dc0, "i", "on the ground, pair 1 of 2 - cleared at"
                  " UpdateGroundContact entry, raised with +0x2db8"),
    (0x2dc4, "i", "on the ground, contact pair 2 of 2"),
    (0x2f75, "i", "Fx POOL SLOT 2 - the second AllocSlot call"),
    (0x3051, "f", "smoothed pose blend, a += (t-a)*k; its SIGN"
                  " selects how g_pilotRot[7] is applied"),
    (0x3055, "i", "copy of 0x0050bbc8 taken when a stunt starts"),
    (0x305d, "x", "STUNT POSE - &g_pilotRot[id] or 0 for none"),
    (0x3061, "i", "the NEXT stunt, rand() % g_nStuntPoseCount"),
    (0x3205, "i", "stunt pressed PREVIOUS frame - the edge gate"),
    (0x2e89, "i", "DEAD - zero rw hits over a complete race"),
    (0x31e1, "i", "DEAD - zero rw hits over a complete race"),
    (0x31e5, "i", "beside +0x31e1, same dead run"),
    (0x2e0d, "i", "DEAD - zero, airborne and grounded"),
    (0x000c, "i", "a FLAG - read at 0x0045d28d, cleared with"
                  " +0x0010 when +0x180 is +-0.0"),
    (0x0010, "i", "cleared beside +0x000c at 0x0045d788"),
    (0x0014, "i", "zeroed during a race, writer unknown [open]"),
    (0x2da0, "i", "a flag maybe_Racer_Tick sets 1/0; gates Fx"
                  " allocation, copied to +0x2da4 as previous"),
    (0x2e29, "i", "LEAN MODE - 1 on the ANALOG steering path, 0 on"
                  " both DIGITAL paths, all three in"
                  " maybe_Racer_ApplySteering. Consumed by"
                  " maybe_Racer_UpdateLean, which uses 20.0f when"
                  " it is clear"),
    (0x31d5, "f", "CHARGE/DECAY METER - ramps to a ceiling of"
                  " 240.0, bleeds off by dt*k. FUN_0046cd20"),
    (0x2db4, "i", "reads 0 at the grid, airborne, and at the"
                  " finish for all 12 racers [open]"),
    (0x306d, "x", "the other racer last encountered - a pointer"),
    (0x3079, "f", "encounter timer, set to 400.0"),
    (0x319d, "i", "HAND SKELETON - selects pose block 1"),
    (0x31b1, "i", "progress - reads as a FLOAT, see the note"),
    (0x31b5, "i", "TRACK TOTAL PROGRESS - 1432 and 3153 on two"
                  " tracks, identical in every racer"),
    (0x31b9, "i", "previous segment"),
    (0x31bd, "i", "current segment"),
    (0x31c1, "i", "segment assigned at the start"),
    (0x31c5, "v", "WALL NORMAL of the last collision, unit"),
    (0x31d9, "i", "LANE, 0..2 across the track, -1 = off it"),
    (0x31dd, "i", "camera index"),
    (0x31f9, "i", "STUNT HELD - the wheelie button, level"),
    (0x31fd, "i", "action 6 down this frame, raw"),
    (0x2db0, "f", "a float, moving - read 84.095 [open]"),
    (0x2dc8, "i", "THE BUTTON WORD THIS TICK RAN WITH."
                  " maybe_Racer_Tick's last statement is"
                  " this->tickInputWord = buttons. maybe_Game_Tick"
                  " also preloads it for the slot it is about to"
                  " broadcast, then sends net message 0x66"),
    (0x2dcc, "i", "written by Racer_Create and FUN_00455280 [open]"),
    (0x2dd0, "i", "THIS RACER'S OWN SLOT INDEX [inferred] -"
                  " maybe_Racer_EmitWheelFx tests it against"
                  " g_viewedRacerIndex to ask 'am I the camera's"
                  " racer', which only works if it holds this"
                  " racer's index. Also referenced by App_Init and"
                  " read by the net"),
    (0x2de5, "i", "RACE STATE. 0 and 1 both count as STILL RACING -"
                  " maybe_Game_Tick's finish test accepts either."
                  " 2 = finished. 3 = TIMED OUT, written by"
                  " maybe_Race_TickClock when g_raceTimer runs 500"
                  " past zero, and set only on the LOCAL racer"),
    (0x2df5, "v", "PlaceWheels arg copy 1 of 3 - x reads -235.4465"),
    (0x2e01, "v", "PlaceWheels arg copy 2 of 3"),
    (0x2e11, "v", "PlaceWheels arg copy 3 of 3"),
    (0x2e3d, "v", "a vec3 Racer_Ctor zeroes [open]"),
    (0x2e49, "v", "a vec3 Racer_Ctor zeroes [open]"),
    (0x3049, "f", "a float, NOT speed - read 543.12/534.74 [open]"),
    (0x304d, "x", "&g_pilotRot[4] staticPied - rewritten every frame"),
    (0x3069, "x", "OVERTAKE GLANCE - &g_pilotRot[29]/[30] or 0"),
    (0x2f7b, "h", "LAP COUNTER, 16 bit - 2 on a two-lap race and 4"
                  " on a four-lap one, every finisher"),
    (0x2f7d, "i", "written by maybe_Game_InitRace with +0x2f81 [open]"),
    (0x2f81, "i", "written by maybe_Game_InitRace with +0x2f7d [open]"),
    (0x2f85, "i", "FINISHING POSITION - written once per human"
                  " racer in maybe_Game_Tick's race-finish block,"
                  " from maybe_Race_FinishingPosition(slot)."
                  " 0 = did not finish"),
    (0x307d, "f", "WHEELIE ANGLE, radians - maybe_Racer_UpdateWheelie"
                  " ramps it to pi/2 while stuntOrOverspeed is set and"
                  " decays it otherwise. Crossing zero from below sets"
                  " springCompression to -32.0, the landing thump."
                  " Read by Camera_Update and CollideRacers"),
    (0x3081, "f", "WHEELIE HOLD TIMER - accumulates dt while the"
                  " wheelie ramps. Past 0x4347ffff (about 200.0) the"
                  " ramp stops and the angle decays, so this is the"
                  " maximum wheelie duration"),
    (0x3059, "f", "read as a speed by camera mode 6"),
    (0x3071, "i", "SIGNED, carried in from the caller stack [open]"),
    (0x3075, "i", "a copy of the global at 0x0050bbc8 [open]"),
    (0x30d9, "f", "second copy of the fall side"),
    (0x30e1, "v", "euler triple (0, +0x30dd, +0x30dd)"),
    (0x31a1, "v", "second copy of the impact vector"),
    (0x31ad, "i", "one-shot consumed by maybe_Ghost_RecordFrame -"
                  " goes into ghost record +0x24, then cleared"
                  " to 0"),
    (0x31d1, "f", "set to 200.0 in several places, then decays"),
    (0x31e9, "x", "INPUT WORD - 1 accel 2 brake 4 left 8 right"
                  " 0x10 up 0x20 down"),
    (0x31ed, "x", "a second button word"),
    (0x31f1, "i", "an axis, centred at 0x8000"),
    (0x31f5, "i", "an axis, centred at 0x8000"),
    (0x3201, "i", "stunt PRESSED - edge, cleared by the pose blend"),
)

MATRICES = (
    (0x00bc, "orientation - yaw and pitch, NO ROLL"),
    (0x0140, "orientation, PREVIOUS frame - Racer_SavePrevFrame"),
    (0x2e55, "BIKE root - orientation with lean"),
    (0x3085, "RIDER root - built by maybe_Racer_BeginCrash"),
)


# ------------------------------------------------------------------ #
# Reading                                                             #
# ------------------------------------------------------------------ #

def read_range(addr, size):
    """Any span of process memory, as bytes, in READ_MAX chunks. None
    if the pipe did not answer. Shared with tools/mr_diff.py."""
    out = bytearray()
    while len(out) < size:
        want = min(READ_MAX, size - len(out))
        reply = mr.pipe_command("GET_MEM %08x %d" % (addr + len(out), want))
        if not reply.startswith("OK"):
            sys.stderr.write("read failed at %08x: %s\n"
                             % (addr + len(out), reply.strip()))
            return None
        out += bytes.fromhex(reply.split()[-1])
    return bytes(out)


def snapshot(index):
    """The whole record, as bytes. None if the pipe did not answer."""
    base = G_RACERS + index * STRIDE
    return read_range(base, STRIDE), base


def f32(b, o):
    return struct.unpack_from("<f", b, o)[0]


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def i32(b, o):
    return struct.unpack_from("<i", b, o)[0]


def vec3(b, o):
    return struct.unpack_from("<3f", b, o)


def cstr(b, o, n):
    return b[o:o + n].split(b"\x00")[0].decode("ascii", "replace")


def denorm(x):
    """A near-identity matrix prints as denormals, which read as
    pointers. MR_RacerPart.txt 4 records that trap; this removes it."""
    return 0.0 if abs(x) < 1e-20 else x


# ------------------------------------------------------------------ #
# Sections                                                            #
# ------------------------------------------------------------------ #

def show_header(b, base, index):
    print("racer %d at %08x, %d bytes" % (index, base, len(b)))
    name = cstr(b, 0x2dd4, 20)
    if name:
        print("  name %r  (+0x2dd4)" % name)


def show_scalars(b):
    print("\nSCALARS")
    for off, kind, what in SCALARS:
        if kind == "f":
            val = "%14.5f" % f32(b, off)
        elif kind == "i":
            val = "%14d" % i32(b, off)
            # AN INT THAT LOOKS LIKE A FLOAT USUALLY IS ONE. A field
            # typed by guesswork shows up here as a huge integer with a
            # clean float reading beside it - which is how +0x31b1 was
            # caught being a float that racer.h declares as an int.
            if abs(i32(b, off)) > 1000000:
                f = f32(b, off)
                if f == f and abs(f) < 1e9:
                    val += "   = %.4ff" % f
        elif kind == "h":
            val = "%14d" % struct.unpack_from("<h", b, off)[0]
        elif kind == "x":
            val = "    0x%08x" % u32(b, off)
        else:
            val = "%9.2f %9.2f %9.2f" % vec3(b, off)
        print("  +0x%04x  %-30s %s" % (off, val, what))


def show_matrices(b):
    print("\nMATRICES")
    for off, what in MATRICES:
        m = [denorm(x) for x in struct.unpack_from("<12f", b, off)]
        print("  +0x%04x  %s" % (off, what))
        for r in range(3):
            row = m[r * 3:r * 3 + 3]
            norm = sum(x * x for x in row) ** 0.5
            print("            [%9.5f %9.5f %9.5f]  |r|=%.5f"
                  % (row[0], row[1], row[2], norm))
        print("            translation %9.2f %9.2f %9.2f"
              % (m[9], m[10], m[11]))


def show_instances(b):
    print("\nMODEL INSTANCES - four, 0x184 apart")
    print("  #  offset  mesh      position                     "
          "prev                        detail")
    for i in range(INST_COUNT):
        o = INST_BASE + i * INST_SIZE
        mesh = u32(b, o + 0x34)
        print("  %d  +0x%04x %-9s (%8.1f %7.1f %8.1f)  "
              "(%8.1f %7.1f %8.1f)  %6d   %s"
              % (i, o, ("%08x" % mesh) if mesh else "NONE",
                 vec3(b, o + 0x0c)[0], vec3(b, o + 0x0c)[1],
                 vec3(b, o + 0x0c)[2],
                 vec3(b, o + 0x18)[0], vec3(b, o + 0x18)[1],
                 vec3(b, o + 0x18)[2],
                 i32(b, o + 0x14c), INST_NAME[i]))
    print("  +0x0c current, +0x18 previous, +0x24 the frame before -"
          " a history, not copies")


def show_pose_map(b):
    live = i32(b, OFF_SLOT)
    print("\nPOSE ARRAY - %d slots x %d parts x %d bytes at +0x%04x"
          % (POSE_SLOTS, POSE_PARTS, PART_SIZE, POSE_BASE))
    print("  camera distance %.1f (+0x%04x), live slot %d (+0x%04x)"
          % (f32(b, OFF_CAMDIST), OFF_CAMDIST, live, OFF_SLOT))
    print("  slot  offset  parts  posed   ptrA      ptrB")
    for s in range(POSE_SLOTS):
        o = POSE_BASE + s * POSE_PARTS * PART_SIZE
        n = i32(b, OFF_PARTCOUNT + s * 4)
        posed = any(u32(b, o + p * PART_SIZE + 0x0c) for p in
                    range(POSE_PARTS))
        print("   %d   +0x%04x   %3d   %-5s  %08x  %08x%s"
              % (s, o, n, "yes" if posed else "no",
                 u32(b, OFF_PTR_A + s * 4), u32(b, OFF_PTR_B + s * 4),
                 "   <- live" if s == live else ""))


def show_parts(b, slot):
    n = i32(b, OFF_PARTCOUNT + slot * 4)
    if n <= 0:
        print("\nPARTS, slot %d: empty" % slot)
        return
    origin = POSE_BASE + slot * POSE_PARTS * PART_SIZE

    print("\nPARTS - slot %d, %d used of %d" % (slot, n, POSE_PARTS))
    print("  part  sib  par  kid |   angles x/y/z, degrees   |"
          "  first  verts faces | faces ptr")
    for i in range(n):
        o = origin + i * PART_SIZE
        links = []
        for k in range(3):
            v = u32(b, o + k * 4)
            # A link is an ABSOLUTE address; turn it back into a part
            # index by removing the record base this snapshot came from.
            links.append("-" if v == 0 else "p%d"
                         % (((v - g_base) - origin) // PART_SIZE))
        ang = [x * 180.0 / 3.14159265358979 for x in vec3(b, o + 0x0c)]
        print("   %2d  %-4s %-4s %-4s | %7.1f %7.1f %7.1f |"
              "  %5d  %5d %5d | %08x"
              % (i, links[0], links[1], links[2], ang[0], ang[1], ang[2],
                 i32(b, o + 0x54), i32(b, o + 0x58), i32(b, o + 0x5c),
                 u32(b, o + 0x60)))
    print("  +0x00 next sibling, +0x04 parent, +0x08 first child."
          " Roots have no parent.")


def show_shadow(b):
    print("\nSHADOW - the racer owns the geometry")
    print("  bike, 6 vertices at +0x2ea9, 2 faces at +0x2ef1")
    for i in range(6):
        x, y, z = vec3(b, 0x2ea9 + i * 12)
        lift = "midpoint, lifted 8.0" if i in (2, 5) else "corner, lifted 4.0"
        print("    v%d (%9.2f %7.2f %9.2f)   %s" % (i, x, y, z, lift))
    if i32(b, 0x30c1):
        print("  rider, 4 vertices at +0x30ed, 2 faces at +0x311d"
              " - DRAWN, the crash latch is set")
        for i in range(4):
            print("    v%d (%9.2f %7.2f %9.2f)"
                  % ((i,) + vec3(b, 0x30ed + i * 12)))
    else:
        print("  rider quad at +0x30ed not drawn (+0x30c1 is 0)")


def show_raw(b, off, n):
    print("\nRAW +0x%04x, %d bytes" % (off, n))
    for row in range(0, n, 16):
        chunk = b[off + row:off + row + 16]
        hexs = " ".join("%02x" % c for c in chunk)
        text = "".join(chr(c) if 32 <= c < 127 else "." for c in chunk)
        print("  +0x%04x  %-47s  %s" % (off + row, hexs, text))


# ------------------------------------------------------------------ #

g_base = 0


def main():
    global g_base
    args = sys.argv[1:]
    index = 1
    slot = None
    only = None
    raw = None

    while args:
        a = args.pop(0)
        if a == "--racer":
            index = int(args.pop(0))
        elif a == "--slot":
            slot = int(args.pop(0))
        elif a in ("--parts", "--tail", "--instances", "--shadow"):
            only = a
        elif a == "--raw":
            raw = (int(args.pop(0), 16), int(args.pop(0)))
        else:
            print(__doc__.strip().split("\n\n")[1])
            return 2

    b, g_base = snapshot(index)
    if b is None:
        return 1

    show_header(b, g_base, index)
    if raw:
        show_raw(b, raw[0], raw[1])
        return 0
    if slot is None:
        slot = i32(b, OFF_SLOT)

    if only in (None, "--instances"):
        show_instances(b)
    if only in (None, "--tail"):
        show_scalars(b)
        show_matrices(b)
    if only in (None, "--parts"):
        show_pose_map(b)
        show_parts(b, slot)
    if only in (None, "--shadow"):
        show_shadow(b)
    return 0


if __name__ == "__main__":
    sys.exit(main())
