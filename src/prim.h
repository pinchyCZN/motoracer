/*
 * prim.h - MR_Primitive, the face record.
 *
 * 0x40 bytes. The most-touched struct in the renderer: what a mesh
 * stores on disk, what the transform and cull stages pass around, what
 * the queue links, and what both back ends finally draw. Every byte is
 * accounted for in docs/structs/MR_Primitive.txt, which is the
 * authority for this file.
 *
 * This is NOT mesh.h's MESH_FACE. That one is a loader convenience -
 * three or four indices and their UVs in whatever shape was easiest to
 * parse. This is the engine's own record, and the queue, the detail
 * system and both rasterizers all read it by offset. prim_from_face
 * converts.
 */

#ifndef MRACER_PRIM_H
#define MRACER_PRIM_H

#include "mesh.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * FIVE SLOTS, NOT FOUR, in both arrays. A face has at most four
 * corners, so four is the natural guess and it is wrong: the fifth is
 * a WRAP entry repeating the first, which lets edge-walking code run
 * i and i+1 to the end of the array with no modulo and no special last
 * case. The face constructors state it outright - base +0x04, stride
 * 8, count 5.
 *
 * vertexIndex[4] == 0xffff is how every "is this a quad" test in the
 * engine is spelled. It means the wrap slot is unset, so the polygon
 * does not close through a fourth vertex. MR_Primitive.txt section 2.1
 * records the earlier reading that called it wQuadMarker: naming it
 * after its use rather than its position hid an array boundary.
 */
#define PRIM_SLOTS      5
#define PRIM_NO_WRAP    0xffff

typedef struct {
	unsigned short kind;              /* +0x00 the AUTHORED kind      */
	unsigned short colorIndex;        /* +0x02 5-5-5, or a pal index  */
	float          uv[PRIM_SLOTS * 2];/* +0x04 five (u,v) pairs       */
	unsigned short vertexIndex[PRIM_SLOTS]; /* +0x2c [4] is the wrap  */
	unsigned short bankIndex;         /* +0x36 texture bank, rebased  */
	unsigned short flags;             /* +0x38 five bits used         */
	unsigned short effectiveKind;     /* +0x3a the kind actually DRAWN*/
	unsigned int   flatColor;         /* +0x3c 0x3f is its alpha byte */
} MR_Primitive;

typedef char mr_primitive_is_0x40[(sizeof(MR_Primitive) == 0x40) ? 1 : -1];

/*
 * TWO KINDS, AND WHY. kind is what the artist or loader asked for.
 * effectiveKind is what actually gets drawn, and they differ whenever
 * detail reduction fires - Screen3D_QueuePrimitive writes +0x3a, and
 * Screen3D_DispatchPrimitive and D3D_DrawScene both read +0x3a and
 * never +0x00. The whole level-of-detail system is that one
 * substitution. See detail.h.
 */

/*
 * The flags word. A search of all 239,429 instructions for tests
 * against +0x38 returns eleven sites and no more, so the readers are
 * fully enumerated - MR_Primitive.txt section 7.
 */
#define PRIM_FLAG_SECOND_PASS  0x0001  /* grouped second pass         */
#define PRIM_FLAG_DEPTH_BIAS   0x0002  /* sorts as if 1024 farther    */
#define PRIM_FLAG_WRITTEN_ONLY 0x0004  /* set always, read by NOTHING */
#define PRIM_FLAG_NO_REDUCE    0x0008  /* opt out of detail level 2   */
#define PRIM_FLAG_LINE         0x0010  /* a LINE, not a triangle      */

/*
 * OURS - THE ORIGINAL HAS NO SUCH BIT, and does not need one.
 *
 * On an INDEXED bank every primitive Load3dfAndMap produces
 * carries a PALETTE INDEX in colorIndex, not a colour: the flat
 * arm at 0x00421242 samples the page byte and masks it with
 * 0xffff00ff, and the textured arm searches the bank's 256-entry
 * palette for the .map's texture id and stores the slot it finds.
 * So the original can decide at QUEUE time by asking the bank -
 * Screen3D_CullAndQueueFaces at 0x0049c516 reads
 * bank[prim->bankIndex] + 8 and sends the prim to
 * Screen3D_QueuePrimitivePaletted when it is set.
 *
 * WE CANNOT USE THAT TEST, because the port hand-builds
 * primitives the original's mesh path never produces - the bike
 * shadow, the HUD map, the particle quads - and some of those sit
 * on an indexed bank while holding a real colour. Asking the bank
 * would recolour them. This bit marks the ones that really did
 * come from a sampled texel.
 */
#define PRIM_FLAG_PAL_INDEX    0x0100  /* colorIndex is a pal slot    */

/*
 * The .map flags to kind decode - see prim.c. The .map's flags dword
 * carries the bank index in its low 16 bits and seven feature bits
 * above; these turn the top half into a kind and a flags word.
 *
 * mapRecordN is the record's own vertex count, which selects a
 * different and much shorter path when it is below three.
 * faceKindSelect is Load3dfAndMap's fourth argument.
 */
unsigned short prim_kind_from_map(unsigned int mapFlags, int mapRecordN,
                                  int faceKindSelect,
                                  unsigned int bankIndex);
unsigned short prim_flags_from_map(unsigned int mapFlags);

/* Kind 0x0d is skipped by both general submitters - unconditionally by
 * the Ex one, and only with flag bit 0 by the plain one. */
#define PRIM_KIND_SKIPPED      0x0d

/*
 * The two kinds Screen3D_CullAndQueueTranslucent forces every
 * non-zero kind into - 0x10 flat/filtered/translucent/affine and
 * 0x2c its textured colour-keyed sibling. Both are in the bit-4
 * blended family; detail-levels.txt 2.6 and 5.
 */
#define PRIM_KIND_TRANSLUCENT      0x10
#define PRIM_KIND_TRANSLUCENT_TEX  0x2c

/*
 * prim_build_from_mesh - MESH_FACE -> MR_Primitive.
 *
 * THIS CONVERTER SHOULD NOT EXIST. Load3dfAndMap builds MR_Primitive
 * directly; mesh.c produces its own simpler MESH_FACE because it was
 * written before MR_Primitive was in src/. When mesh.c is rewritten to
 * emit MR_Primitive this whole function goes away.
 *
 * It lived in the geometry test harness until page 14 needed it. Two callers is
 * where a private helper becomes a shared one - and prim.c is the
 * right home, because building MR_Primitive is what this file is for.
 *
 * Allocates *out with calloc; the caller frees it. Returns 0 on
 * failure, leaving *out NULL and *count 0.
 */
int prim_build_from_mesh(const MESH *m, MR_Primitive **outPrims,
                         int *outCount);

/*
 * prim_build_from_mesh_sel - the same, with Load3dfAndMap's
 * faceKindSelect exposed. prim_build_from_mesh passes 0.
 *
 * ONLY ONE CALLER PASSES 1: Race_LoadFinalMeshes, for the
 * end-of-race plaque. With a non-zero face bank - and the plaque
 * .maps all name bank 19 - that upgrades the face kinds to the
 * quality-tier-2 variants 0x10, 0x26 and 0x27.
 */
int prim_build_from_mesh_sel(const MESH *m, MR_Primitive **outPrims,
                             int *outCount, int faceKindSelect);

/*
 * prim_build_from_mesh_bank - the same, with the bank REBASE APPLIED
 * BEFORE the flat-colour sample. Use this wherever the caller would
 * otherwise follow the build with prim_rebase_banks.
 *
 * *** THE ORDER IS THE WHOLE POINT ***
 *
 * A flat face takes its colour from one texel of its own bank page,
 * and the bank it names in the .map is a MESH-LOCAL index - almost
 * always 0. Rebasing afterwards fixes which page the face is TEXTURED
 * from and cannot fix the colour, because the texel has already been
 * read out of global bank 0.
 *
 * Bank 0 is a loaded 256x256 TRUECOLOUR menu page, so the sample
 * succeeds and returns a plausible wrong colour rather than failing:
 * the bike's flat faces come out as whatever sits at those
 * coordinates in a menu background. That is how a yellow mudguard
 * draws white and a red helmet vent draws grey.
 *
 * Passing bankDelta 0 is exactly prim_build_from_mesh_sel.
 */
int prim_build_from_mesh_bank(const MESH *m, MR_Primitive **outPrims,
                              int *outCount, int faceKindSelect,
                              int bankDelta);

/*
 * prim_rebase_banks - Mesh_RebaseFaceBanks (0x00422250), transcribed.
 *
 *     for (i = 0; i < faceCount; i++)
 *         faces[i].bankIndex += delta;
 *
 * IT ADDS, IT DOES NOT ASSIGN. A .map stores bank numbers that are
 * relative to its own page, and the caller shifts them to wherever it
 * actually loaded the texture. Page 14 loads its raw into bank 3 and
 * then calls this with 3, once per mesh [verified].
 *
 * o WITHOUT IT A FACE POINTS AT A BANK NOBODY LOADED, drawscene's
 *   prim_is_textured returns 0, and the face draws as its flatColor -
 *   which for a textured face is 0xffffffff. A WHITE QUAD. That is
 *   not a missing feature, it looks exactly like one.
 *
 * That harness never needed it: it loaded banks 0 and 1 and its
 * .maps already say 0 and 1, so its delta would be zero.
 */
void prim_rebase_banks(MR_Primitive *prims, int count, int delta);

/*
 * prim_flat_colour - the flat ARGB to draw this primitive with,
 * resolved against the palette that is live NOW.
 *
 * For everything but a PRIM_FLAG_PAL_INDEX face this is just
 * flatColor. For one of those it is the bank's active palette
 * entry, and it changes when the livery changes - which is the
 * whole point, and is why the draw calls this instead of reading
 * the field.
 *
 * THE LIVERY IS A PALETTE, NOT A PAGE: BNK00_00.RAW is one
 * 256-colour page and BNK00_00.1 through .11 are alternate
 * palettes for it, so the mudguard, the seat and the helmet vent
 * are the SAME texels under a different palette. Resolving at
 * load time picks one and keeps it.
 */
unsigned int prim_flat_colour(const MR_Primitive *p);

#ifdef __cplusplus
}
#endif

#endif /* MRACER_PRIM_H */
