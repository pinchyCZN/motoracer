/*
 * mrmod.c - OUR RECONSTRUCTION, RUNNING INSIDE MOTO.EXE.
 *
 * mrhook loads this beside MOTO.EXE on a LOAD_MOD command and then
 * patches a MOTO function entry to jump at one of the exports below
 * on HOOK_FUNC. The original never runs; ours answers in its place,
 * with MOTO arguments, in MOTO state, on MOTO frame.
 *
 * WHY THIS IS BETTER EVIDENCE THAN THE TWO-PROCESS A/B. tools/mr_ab.py
 * states its own ceiling: MOTO frames and ours are not the same thing,
 * so only the SHAPE of a track can be compared, never a value. Every
 * difference is also the sum of every divergence upstream, and once
 * the two worlds differ they drift apart. There is only one world
 * here, so a function either matches on that call or it does not.
 *
 *
 * THE SOURCE IS NOT COPIED HERE. mrmod.vcxproj compiles ..\..\src\trk.c
 * directly, so a change to the port is picked up by the next build and
 * there is no second copy to drift. mrmod_stubs.c supplies the eight
 * externals and one global that translation unit references but this
 * DLL never reaches.
 *
 *
 * THE CALLING CONVENTION, AND WHY THERE IS NO ASSEMBLY.
 *
 * The originals are mostly __thiscall: ECX carries the implicit this,
 * the rest go on the stack, and the callee purges them. MSVC will not
 * let a free function be __thiscall - but __fastcall lands its first
 * two arguments in ECX and EDX and pushes the rest, and it purges the
 * stack ones. So
 *
 *     float __fastcall f(this, unused, a, b, ...)
 *
 * is byte-for-byte the same contract as __thiscall f(a, b, ...) with
 * an implicit this. The dead second parameter absorbs EDX, which the
 * original never set. Float returns come back in ST0 on both sides.
 *
 * mrhook already relies on this trick in the other direction, to CALL
 * into MOTO - see the volume code there.
 *
 *
 * THE EXPORT NAMES ARE DECORATED and mrmod.def maps them back. A
 * __fastcall C function is @name@N where N is the total size of every
 * parameter, register ones included - so twelve for three pointers. If
 * a signature changes, the link fails with the expected name in the
 * error, which is the right way to find out.
 */

#include "trk.h"


/*
 * THE CALL COUNTER, AND WHY IT HAD TO BE OURS.
 *
 * The obvious proof that a replacement is live was MOTO own
 * instrumentation: Plane_Distance opens with INC [0x00709378], so a
 * frozen counter should mean our code answered instead. IT DOES NOT.
 * That dword is shared - Plane_IntersectSegment (0x004b103a)
 * increments the same one - so it keeps climbing however well the
 * hook works, and the test says nothing.
 *
 * This one is incremented ONLY by the replacements below, so it is
 * unambiguous: if it moves, our code ran inside MOTO.
 *
 * IT IS A DATA EXPORT so its address can be found without a new
 * mrhook command - dumpbin gives the RVA, LOAD_MOD reports the base,
 * and GET_MEM reads the sum.
 */
__declspec(dllexport) unsigned int mrmod_call_count = 0;


/*
 * ORIGIN  Plane_Distance @ 0x004b10c0 - __thiscall(plane, point),
 *         38 bytes, one stack argument, RET 4.
 *
 * THE FIRST ONE ON PURPOSE. Two const float pointers in, one float
 * out; no globals, no structs, no allocation, and a real standalone
 * entry point rather than a block inside a larger function. It cannot
 * itself be the source of a bug, so if this goes wrong the mechanism
 * is wrong - which is exactly what a proof of concept should isolate.
 */
float __fastcall mrmod_plane_distance(const float *plane, void *unused,
                                      const float *p)
{
	(void)unused;
	mrmod_call_count++;
	return plane_distance(plane, p);
}


/*
 * THE SAME, DELIBERATELY WRONG, and it earns its place.
 *
 * A faithful replacement and a hook that never fired look identical on
 * screen, so a POC that only installs the correct answer proves
 * nothing you can see. This one shifts every plane by 64 units.
 * Plane_Distance feeds the ground height and the cull, so the picture
 * should visibly break the moment it is hooked and come back the
 * moment it is not.
 *
 * Hook this first, then the real one. Two exports also prove the
 * GetProcAddress path rather than just the byte patch.
 */
float __fastcall mrmod_plane_distance_bad(const float *plane, void *unused,
                                          const float *p)
{
	(void)unused;
	mrmod_call_count++;
	return plane_distance(plane, p) + 64.0f;
}


/*
 * THE LOUD ONE. +64 turned out to be nearly invisible, and the reason
 * is worth writing down rather than just turning the number up.
 *
 * MOST CONSUMERS OF Plane_Distance USE ONLY ITS SIGN. Trk_GroundHeightAt
 * tests the SELECTOR plane to choose which of a segment two ground
 * triangles a point is over - and on smooth track those two triangles
 * are nearly coplanar, so picking the wrong one moves the ground by
 * almost nothing. The frustum tests in maybe_Racer_CullSphere compare
 * against radius * -4, which is -2048 for a bike: a 64-unit shift is
 * three per cent of the threshold. Only the segment walk moves at all,
 * and one segment either way is hard to see.
 *
 * SO NEGATE IT INSTEAD OF NUDGING IT. Every inside/outside test in the
 * game inverts at once - ground triangle selection, wall sides, the
 * frustum, the segment walk. If THIS is invisible the hook is not
 * firing, and that is the point of having it.
 */
float __fastcall mrmod_plane_distance_flip(const float *plane, void *unused,
                                           const float *p)
{
	(void)unused;
	mrmod_call_count++;
	return -plane_distance(plane, p);
}
