#!/usr/bin/env python3
"""gen_mr_racer.py - emit the Ghidra MR_Racer struct from the field map.

    python tools/gen_mr_racer.py            report coverage and conflicts
    python tools/gen_mr_racer.py --json     the create_struct fields body

WHY GENERATE IT. mr_dump_racer.py's SCALARS and MATRICES are the
authority four tools already share. Hand-typing 119 fields into Ghidra
would make a second authority that can disagree with the first, and
CLAUDE.md rule 5b is exactly about not letting that happen. Emitting
the struct from the same table means a wrong field is wrong in one
place, and re-shaping the struct later is a re-run rather than a
rewrite.

THE STRUCT IS FLAT, AND THAT IS A DECISION. The racer also contains
four MR_ModelInstance records at +0x24, +0x1a8, +0x32c and +0x4b0 -
but structs/MR_Racer.txt section 3.1 shows the racer's own named
fields land INSIDE them: position at +0x30 is instance 0 + 0x0c, and
the wheel contacts at +0x1c0 and +0x344 sit inside instances 1 and 2.
Ghidra cannot express two overlapping views of the same bytes, so one
of them has to win.

The named scalars win, because they are what gameplay code reads and
what the documents describe. The instance view is recorded in the
plate comment instead. A reconstruction that wants MR_ModelInstance
semantics can cast; a decompiler that shows racer->lean cannot be
given that back by casting.

GAPS ARE undefined1[] ON PURPOSE. Leaving them out would let Ghidra
invent fields; naming them would assert more than is known. 52 of
them survived three poison sweeps and are dead space - see
structs/MR_Racer.txt section 21.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mr_dump_racer as dump            # noqa: E402

KIND = {"i": ("int", 4), "x": ("uint", 4), "f": ("float", 4),
        "h": ("short", 2), "v": ("float[3]", 12)}

# Short field names for the offsets whose meaning is settled. Every one
# is taken from that field's own description in mr_dump_racer.SCALARS;
# anything not listed here keeps f_XXXX, which is not a placeholder but
# a deliberate link to the offset the documents are keyed by.
SHORT = {
    0x0000: "drawnThisFrame", 0x0004: "soloMode", 0x0008: "humanPlayer",
    0x0030: "position", 0x003c: "positionPrev", 0x0048: "positionPrev2",
    0x0054: "frameDelta", 0x0068: "active", 0x0078: "bikeIndex",
    0x007c: "gear",
    0x0080: "shiftTimerMs", 0x0084: "shiftState",
    0x008c: "pendingDownshift", 0x00b4: "realSpeedMax",
    0x00ac: "turnBrakeFlag", 0x00b8: "wheelieScale",
    0x00bc: "matOrientation", 0x00ec: "engineSpeed",
    0x00f0: "airTime",
    0x00f8: "steerReturnScale", 0x010c: "steerRate",
    0x0110: "steering", 0x0120: "needleRate",
    0x0114: "speedKmhRaw", 0x0118: "speedKmhNeedle", 0x011c: "tachoRpm",
    0x0124: "speed", 0x0128: "speedPrev",
    0x012c: "gearboxRoadSpeed", 0x0130: "velDir",
    0x013c: "driveDisengaged",
    0x0140: "matOrientationPrev", 0x0184: "skidWidthPrev",
    0x0188: "skidWidth", 0x018c: "moveDir",
    0x01b4: "frontWheelContact", 0x01c0: "frontWheelContactPrev",
    0x0338: "rearWheelContact", 0x0344: "rearWheelContactPrev",
    0x0634: "cameraDist", 0x0638: "skeletonSlot",
    0x2da0: "fxGate", 0x2da4: "fxGatePrev", 0x2da8: "speedTail",
    0x2dac: "riderStateBits", 0x2db8: "onGround", 0x2dbc: "onGroundPrev",
    0x2dc0: "onGroundPair1", 0x2dc4: "onGroundPair2",
    0x2dc8: "tickInputWord",
    0x2dd0: "slotIndex",
    0x2ddd: "raceClockMs", 0x2de1: "bestLapMs", 0x2de5: "raceState",
    0x2df5: "placeWheels0", 0x2e01: "placeWheels1",
    0x2e0d: "dead_2e0d", 0x2e11: "placeWheels2",
    0x2e1d: "lean", 0x2e21: "leanPrev", 0x2e29: "leanMode",
    0x2e39: "forkDive", 0x2e55: "matBikeRoot",
    0x2e85: "stuntOrOverspeed", 0x2e89: "dead_2e89",
    0x2e8d: "suspensionAngle", 0x2e91: "springBounce",
    0x2e95: "springCompression", 0x2e99: "springPhase",
    0x2e9d: "springAmplitude", 0x2ea1: "headingSmoothed",
    0x2ea5: "headingRef", 0x2f71: "fxSlot0", 0x2f75: "fxSlot1",
    0x2f79: "checkpointIndex", 0x2f7b: "lapCounter",
    0x2f85: "finishPosition",
    0x307d: "wheelieAngle", 0x3081: "wheelieTimer",
    0x304d: "poseStaticPied", 0x3051: "poseBlend",
    0x3055: "stuntStartStamp", 0x305d: "stuntPose", 0x3061: "stuntNext",
    0x3065: "stuntPoseId", 0x3069: "glancePose", 0x306d: "glanceOther",
    0x3079: "glanceTimer", 0x3085: "matRiderRoot",
    0x30b5: "riderPosition", 0x30c1: "crashLatch",
    0x30c9: "crashLaunchDir", 0x30d5: "crashTimer",
    0x30dd: "crashFallSide", 0x319d: "handSkeleton",
    0x31ad: "ghostEvent", 0x31b1: "progress",
    0x31b5: "trackTotalProgress",
    0x31b9: "segmentPrev", 0x31bd: "segment", 0x31c1: "segmentStart",
    0x31c5: "wallNormal", 0x31d5: "chargeMeter", 0x31d9: "lane",
    0x31dd: "cameraIndex", 0x31e1: "dead_31e1", 0x31e5: "dead_31e5",
    0x31e9: "inputWord", 0x31ed: "inputWord2", 0x31f1: "axis0",
    0x31f5: "axis1", 0x31f9: "stuntHeld", 0x31fd: "action6Down",
    0x3201: "stuntPressed", 0x3205: "stuntPressedPrev",
}


# Named blocks that are not in SCALARS or MATRICES. Each is
# (offset, size, type, name) and every one is sourced from a document.
BLOCKS = (
    (0x0018, 8, "float[2]", "turnRadius"),       # [0] is a RADIUS:
    # maybe_Racer_ApplyCornering fits it through three successive
    # positions and divides v*v by it. FLT_MAX means "straight",
    # which is why it read as a clip minimum before anything wrote it.
    (0x063c, 10000, "MR_RacerPart[100]", "pose"),  # 5 slots x 20 parts
    (0x2d4c, 60, "undefined1[60]", "perSlotArrays"),
    (0x2dd4, 9, "char[9]", "playerName"),
    (0x2ea9, 0x2f71 - 0x2ea9, "undefined1[200]", "bikeShadow"),
    (0x2f89, 192, "int[48]", "checkpointSplits"),
    (0x30ed, 0x319d - 0x30ed, "undefined1[176]", "riderShadow"),
)


def collect():
    """[(offset, size, type, name, note)] sorted, conflicts reported."""
    out = []
    for off, kind, note in dump.SCALARS:
        ty, size = KIND[kind]
        out.append((off, size, ty, SHORT.get(off, "f_%04x" % off), note))
    for off, note in dump.MATRICES:
        out.append((off, 48, "float[12]",
                    SHORT.get(off, "m_%04x" % off), note))
    for off, size, ty, name in BLOCKS:
        out.append((off, size, ty, name, "block"))
    out.sort()
    return out


def build():
    fields, gaps, conflicts = [], [], []
    cur = 0
    for off, size, ty, name, note in collect():
        if off < cur:
            conflicts.append((off, size, name, cur))
            continue
        if off > cur:
            n = off - cur
            fields.append((cur, "undefined1[%d]" % n, "unk_%04x" % cur))
            gaps.append((cur, n))
            cur = off
        fields.append((off, ty, name))
        cur = off + size
    if cur < dump.STRIDE:
        n = dump.STRIDE - cur
        fields.append((cur, "undefined1[%d]" % n, "unk_%04x" % cur))
        gaps.append((cur, n))
        cur = dump.STRIDE
    return fields, gaps, conflicts, cur


def main():
    fields, gaps, conflicts, total = build()
    if "--json" in sys.argv:
        parts = ['{"name":"%s","type":"%s","offset":%d}' % (n, t, o)
                 for o, t, n in fields]
        sys.stdout.write("[" + ",".join(parts) + "]\n")
        return 0
    named = sum(1 for o, t, n in fields if not n.startswith("unk_"))
    gapb = sum(n for _o, n in gaps)
    print("MR_Racer: %d fields, %d named, %d gap runs (%d bytes)"
          % (len(fields), named, len(gaps), gapb))
    print("total size %d (0x%x), STRIDE %d (0x%x)  %s"
          % (total, total, dump.STRIDE, dump.STRIDE,
             "OK" if total == dump.STRIDE else "*** MISMATCH ***"))
    if conflicts:
        print("\n%d CONFLICTS - dropped, they overlap an earlier field:"
              % len(conflicts))
        for off, size, name, cur in conflicts:
            print("  +0x%04x %-18s size %-5d starts before 0x%04x"
                  % (off, name, size, cur))
    else:
        print("no overlaps")
    print("\ngap runs:")
    for o, n in gaps:
        print("  +0x%04x  %d bytes" % (o, n))
    return 0 if total == dump.STRIDE and not conflicts else 1


sys.exit(main())
