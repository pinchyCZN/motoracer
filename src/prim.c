/*
 * prim.c - the .map flags to primitive kind decode.
 *
 * Transcribed from Load3dfAndMap (0x00420a50). docs/formats/
 * mesh-3df-map.txt section 5 had recorded the seven flag bits and the
 * nine kind constants they select - 1, 2, 8, 0xc, 0xd, 0x10, 0x15,
 * 0x26 and 0x27 - and called decoding the table "a job of its own".
 * This is that job.
 *
 * IT IS NOT A TABLE. It is a base kind plus three feature bits, and the
 * nine constants are what those combinations happen to be numbered:
 *
 *     base                            kind 1     bits 0x01  TIER1
 *     + map 0x010000                  kind 2     bits 0x09  + NO_FILTER
 *     + tier2 selector                kind 0x10  bits 0x81  + TIER2
 *     + both                          kind 0x26  bits 0x89  + both
 *     + map 0x200000                  kind 0x15  bits 0x85  full ladder
 *     + map 0x200000 and 0x010000     kind 0x27  bits 0x8d  + NO_FILTER
 *
 * Line the kinds up against the feature table in
 * docs/subsystems/detail-levels.txt section 6 and every column agrees:
 * map bit 0x010000 adds exactly DETAIL_NO_FILTER (0x08) in all three
 * pairs 1->2, 0x10->0x26 and 0x15->0x27, and the tier-2 selector adds
 * exactly DETAIL_TIER2 (0x80). The two decodes were made independently
 * from different functions and they agree bit for bit, which is the
 * strongest confirmation either of them has.
 *
 * Two bits then REPLACE the kind outright rather than adding to it, and
 * the order matters because both are unconditional stores:
 *
 *     map 0x040000  ->  kind 0x0d   (bits 0x40, EXEMPT from reduction)
 *     map 0x080000  ->  kind 8      (bits 0x11, TRANSLUCENT | TIER1)
 *
 * 0x080000 is written LAST, so translucent wins over 0x0d when both are
 * set - and five shipped records do set both.
 */

#include <windows.h>
#include <stdlib.h>
#include "prim.h"
#include "mesh.h"
#include "bank.h"

unsigned short prim_flags_from_map(unsigned int mapFlags)
{
	/*
	 * Bit 2 is set on EVERY face unconditionally and read by nothing.
	 * Kept rather than dropped: a port should not repurpose a bit
	 * without checking whether anything writes the face records back
	 * out. MR_Primitive.txt section 7.2.
	 */
	unsigned short f = PRIM_FLAG_WRITTEN_ONLY;

	if (mapFlags & 0x020000) f |= PRIM_FLAG_SECOND_PASS;
	if (mapFlags & 0x400000) f |= PRIM_FLAG_DEPTH_BIAS;
	if (mapFlags & 0x100000) f |= PRIM_FLAG_NO_REDUCE;
	return f;
}

unsigned short prim_kind_from_map(unsigned int mapFlags, int mapRecordN,
                                  int faceKindSelect,
                                  unsigned int bankIndex)
{
	unsigned short kind;
	int            tier2;

	if (mapRecordN < 3) {
		/*
		 * A record whose vertex count is below three does not describe
		 * a polygon, and the loader takes a shorter path for it: the
		 * kind is SET TO 0 first - through the setter, so +0x3a is
		 * cleared too - and only three bits are then consulted. There
		 * is no tier-2 or tier-3 variant on this path, and 0x080000
		 * gives kind 0x0c rather than kind 8.
		 *
		 * THIS PATH IS NOT RARE, and the note that used to be here said
		 * it was: "one record in the whole shipped game takes it". That
		 * survey missed data\menu\font3d entirely. EVERY record of all
		 * 30 3D-font .map files has n = 0 - 26 in LA.MAP alone - so the
		 * whole 3D alphabet on page 15 comes through here, with no flag
		 * bits, as kind 0: untextured, flat-shaded. Which is exactly
		 * what the letters look like.
		 *
		 * o KIND 0x0C IS STILL UNREACHABLE ON SHIPPED DATA, since that
		 *   needs map flag 0x080000 and none of these records has any
		 *   flags. Reproduced anyway, because an authored .map could
		 *   reach it.
		 *
		 * o A "ONE RECORD IN THE WHOLE GAME" CLAIM IS A SURVEY, and a
		 *   survey is only as good as the files it walked. This one
		 *   was drawn from the track and bike meshes and then written
		 *   as though it covered everything.
		 */
		kind = 0;
		if (mapFlags & 0x010000) kind = 2;
		if (mapFlags & 0x040000) kind = 0x0d;
		if (mapFlags & 0x080000) kind = 0x0c;
		return kind;
	}

	/*
	 * THE TIER-2 SELECTOR. faceKindSelect is Load3dfAndMap's fourth
	 * argument, and it only bites when the face's own bank index is
	 * non-zero - so it upgrades faces on the SECOND texture page and
	 * leaves the first alone. For a bike, relative bank 0 is
	 * bnk00_00.raw and relative 1 is bnk00_01.raw.
	 *
	 * Who passes what, read off the call sites:
	 *
	 *     MenuTools_CreateSceneObject   0        the 3D menu scenes
	 *     maybe_Game_LoadBikeMeshes     0, and a runtime test at two
	 *                                   sites: SETZ on
	 *                                   DAT_00509398 == EBP
	 *     FUN_0047b600                  1
	 *
	 * So menu geometry never sees the upgrade, and bike geometry sees
	 * it conditionally. What that runtime test compares has not been
	 * traced.
	 */
	tier2 = (faceKindSelect != 0 && bankIndex != 0);

	if (!(mapFlags & 0x010000)) {
		kind = tier2 ? 0x10 : 1;
		if (mapFlags & 0x200000) kind = 0x15;
	} else {
		kind = tier2 ? 0x26 : 2;
		if (mapFlags & 0x200000) kind = 0x27;
	}

	/*
	 * Note 0x200000 discards the tier-2 selector rather than composing
	 * with it: 0x15 is the full quality ladder either way. So a face
	 * with the tier-3 bit is at maximum quality whatever the caller
	 * asked for.
	 */

	if (mapFlags & 0x040000) kind = 0x0d;
	if (mapFlags & 0x080000) kind = 8;

	return kind;
}


/* ------------------------------------------------------------------ */
/* MESH_FACE -> MR_Primitive                                           */
/* ------------------------------------------------------------------ */

/*
 * A 565 texel widened for display. REPLICATES the high bits, unlike
 * the original's 5-to-8 flat-colour expansion, which places each
 * channel at the top of its byte with no replicate and so never
 * reaches full white - MR_Primitive.txt section 6.1.
 */
static unsigned int expand565(unsigned short p)
{
	unsigned int r = (p >> 11) & 0x1f;
	unsigned int g = (p >> 5)  & 0x3f;
	unsigned int b = p & 0x1f;

	r = (r << 3) | (r >> 2);
	g = (g << 2) | (g >> 4);
	b = (b << 3) | (b >> 2);
	return 0xff000000u | (r << 16) | (g << 8) | b;
}

/*
 * A face whose UV pairs are all equal is FLAT COLOURED, and the colour
 * is one texel read out of the bank page at [v * 0x100 + u] - the
 * loader's own rule, from docs/formats/mesh-3df-map.txt section 6.
 *
 * IT RETURNS ONE OF TWO THINGS, exactly as the original's two arms
 * store one of two things into colorIndex: a widened ARGB when the
 * page is truecolour, and a bare PALETTE SLOT when it is indexed.
 * isPalIndex says which. The caller must not treat them alike -
 * resolving a slot here is what bakes one livery in for good.
 *
 * DIVERGENCE on the truecolour arm. The original keeps the 16-bit
 * value and derives flatColor with the NO-REPLICATE 5-to-8
 * expansion; this widens WITH a replicate, so a flat white face is
 * 0xffffff here and 0xf8f8f8 there.
 */
static unsigned int resolve_flat_colour(const MESH_FACE *f, int bank,
                                        int *isPalIndex)
{
	MR_Bank *b = bank_get(bank);
	int      u;
	int      v;

	*isPalIndex = 0;
	if (!b || !b->loaded || !b->page)
		return 0xffffffffu;

	u = (int)f->u[0];
	v = (int)f->v[0];
	if (u < 0) u = 0;
	if (v < 0) v = 0;
	if (u >= BANK_PAGE_W) u = BANK_PAGE_W - 1;
	if (v >= BANK_PAGE_H) v = BANK_PAGE_H - 1;

	/*
	 * IT IS v * 256 + u. V is the row. Both arms of the original
	 * scale the SECOND uv float, and the two saved pointers are what
	 * say which that is:
	 *
	 *     004211ea  LEA ECX,[EBX + 0x4]   &u0 -> [EBP-0x1c]
	 *     00421211  LEA EAX,[EBX + 0x8]   &v0 -> [EBP-0x14]
	 *     ...
	 *     00421251  MOV EAX,[EBP-0x14]    v0
	 *     0042125b  SHL EAX,0x8           <- V is the one scaled
	 *     0042125e  MOV ECX,[EBP-0x1c]    u0
	 *     0042126a  MOV AL,[EDI + EAX]    <- plus U
	 *
	 * uv[] is (u0,v0,u1,v1,...), so prim+0x4 is u0 and prim+0x8 is
	 * v0 - which the four equality tests above confirm from the
	 * other side, pairing +0x4/+0xc/+0x14 against each other and
	 * +0x8/+0x10/+0x18 against each other.
	 *
	 * LANDMINE, AND IT HAS BEEN STEPPED ON TWICE. A transposed read
	 * lands on a REAL texel rather than out of bounds, so it returns
	 * a plausible wrong colour and nothing looks broken. What gives
	 * it away is that the wrong texels cluster in the page's grey
	 * ramp, which every livery palette holds in common - so the
	 * bike's flat faces come out grey and, worse, stop changing with
	 * the rider.
	 */
	if (b->isIndexed) {
		/*
		 * THE PAGE HOLDS ONE BYTE PER PIXEL at a 512-byte row stride,
		 * and the palette resolves it - 0x00421242's arm, which reads
		 * the byte and keeps only the low eight bits. Returning white
		 * here is what made the whole first-person cockpit white: the
		 * bike's first page is paletted.
		 */
		const unsigned char *idx = (const unsigned char *)b->page;

		if (b->paletteCount < 1)
			return 0xffffffffu;
		/*
		 * THE SLOT, NOT THE COLOUR - prim_flat_colour resolves it
		 * against whichever palette is live when the face is drawn.
		 * Baking it here is what made every flat face wear bike 0's
		 * livery whoever was selected.
		 */
		*isPalIndex = 1;
		return idx[(size_t)v * (BANK_PAGE_W * 2) + u];
	}

	return expand565(b->page[(size_t)v * BANK_PAGE_W + u]);
}

/*
 * THE KIND IS DECODED. prim_kind_from_map above is the transcription
 * of that table, so this converter does select a real draw mode.
 *
 * o THE COMMENT THAT USED TO BE HERE SAID OTHERWISE, and it was
 *   stale. It was written in the geometry test harness, before
 *   prim.c existed, and moved across with the function; reading it
 *   led to telling the user that
 *   the kind decode was still outstanding when the code one screen up
 *   does it. A stale comment outranks the code it sits next to, in
 *   the reader every time.
 *
 * faceKindSelect is 0 because MenuTools_CreateSceneObject - the
 * function that builds the 3D menu scene objects - passes 0. Correct
 * for menu geometry, wrong for a bike.
 */
int prim_build_from_mesh(const MESH *mesh, MR_Primitive **out,
                         int *count)
{
	return prim_build_from_mesh_sel(mesh, out, count, 0);
}

int prim_build_from_mesh_sel(const MESH *mesh, MR_Primitive **out,
                             int *count, int faceKindSelect)
{
	return prim_build_from_mesh_bank(mesh, out, count, faceKindSelect, 0);
}

int prim_build_from_mesh_bank(const MESH *mesh, MR_Primitive **out,
                              int *count, int faceKindSelect,
                              int bankDelta)
{
	MR_Primitive *prims;
	int i, f, n;

	if (out)   *out = NULL;
	if (count) *count = 0;
	if (!mesh || !out || !count || mesh->faceCount <= 0)
		return 0;

	prims = (MR_Primitive *)calloc((size_t)mesh->faceCount,
	                               sizeof(MR_Primitive));
	if (!prims)
		return 0;

	for (f = 0; f < mesh->faceCount; f++) {
		const MESH_FACE *src = &mesh->faces[f];
		MR_Primitive    *p   = &prims[f];
		int              flat;

		n = (src->count > 4) ? 4 : src->count;
		p->bankIndex = (unsigned short)(src->hasMap ? src->bank : 0);
		p->kind = src->hasMap
		    ? prim_kind_from_map(src->mapFlags, src->count,
		                         faceKindSelect, p->bankIndex)
		    : 0;
		p->effectiveKind = p->kind;
		p->flags = src->hasMap ? prim_flags_from_map(src->mapFlags)
		                       : PRIM_FLAG_WRITTEN_ONLY;

		/*
		 * A STRAIGHT COPY, because the winding is settled in the LOADER
		 * rather than here. mesh_mirror_and_flip in mesh.c reproduces
		 * FUN_00422070, the last step of both paired loaders, which
		 * negates x on every vertex and reverses every face. By the time
		 * a MESH reaches this function its faces are already in the order
		 * the game holds them.
		 *
		 * DO NOT ADD A REVERSAL HERE. Two earlier sessions did, to fix
		 * page 14 logos that read mirrored, and it worked for the wrong
		 * reason - it supplied half of FUN_00422070 while leaving the
		 * geometry unmirrored. On a billboard those two halves cancel, so
		 * page 14 looked correct and page 25 stayed broken in a way no
		 * amount of winding could fix. See mesh.c.
		 */
		for (i = 0; i < n; i++) {
			p->vertexIndex[i] = (unsigned short)src->index[i];
			p->uv[i * 2]      = src->hasMap ? src->u[i] : 0.0f;
			p->uv[i * 2 + 1]  = src->hasMap ? src->v[i] : 0.0f;
		}

		/* The wrap slots, exactly as the face constructors write them. */
		if (n == 4) {
			p->vertexIndex[4] = p->vertexIndex[0];
			p->uv[8] = p->uv[0];
			p->uv[9] = p->uv[1];
		} else {
			p->vertexIndex[3] = p->vertexIndex[0];
			p->vertexIndex[4] = PRIM_NO_WRAP;
			p->uv[6] = p->uv[0];
			p->uv[7] = p->uv[1];
			p->uv[8] = p->uv[0];
			p->uv[9] = p->uv[1];
		}

		flat = 1;
		for (i = 1; i < n; i++) {
			if (src->u[i] != src->u[0] || src->v[i] != src->v[0]) {
				flat = 0;
				break;
			}
		}
		if (!src->hasMap)
			flat = 1;

		/*
		 * TWO DIFFERENT FLAT COLOURS, and picking the wrong one is how
		 * the 3D alphabet came out white.
		 *
		 * An n < 3 record has no UVs, so there is nothing to sample a
		 * bank page with; its colour is the record's own +0x02, a
		 * 5-5-5 value. An n >= 3 face whose UVs happen to be all equal
		 * IS sampled from the bank, at [v * 0x100 + u].
		 *
		 * o THE BANK ROUTE SILENTLY YIELDS WHITE WHEN NO BANK IS
		 *   LOADED, which is exactly what page 15 does - it loads no
		 *   banks at all, because its glyphs are flat-shaded and never
		 *   needed one. So the wrong branch does not fail, it just
		 *   quietly paints everything 0xffffffff.
		 *
		 * o AND +0x02 IS 5-6-5, NOT 5-5-5, which is the same format
		 *   the bank pages are in. It was read as 555 first, on the
		 *   strength of the "5-5-5" in mesh-3df-map.txt section 5.3 -
		 *   but that sentence is about the TEXTURE ID being converted
		 *   as 555 on a different branch, and it does not describe
		 *   this field.
		 *
		 *   THE DATA SETTLES IT. Across all 26 letter .map files there
		 *   are 18 distinct values. Read as 5-6-5 every one is a
		 *   blue-purple differing only in brightness - 0x829f is
		 *   (132,165,255) and occurs 306 times, 0x2149 is (33,41,74),
		 *   0xe55f is (231,170,255). Read as 5-5-5 the same 18 values
		 *   scatter into cyan, magenta and blue with no relationship
		 *   at all. One reading is a lit palette and the other is
		 *   noise.
		 */
		/*
		 * *** REBASE BEFORE THE SAMPLE, NOT AFTER ***
		 *
		 * prim_kind_from_map above needs the MESH-LOCAL bank - its
		 * tier-2 test is "bank != 0" and every mesh-local bank for
		 * a tier-1 face is 0. resolve_flat_colour below needs the
		 * FINAL one, because it reads a texel out of that page.
		 *
		 * Doing the rebase in a separate pass afterwards, which is
		 * what prim_rebase_banks is for, fixes the texturing and
		 * leaves every flat face coloured from global bank 0.
		 */
		p->bankIndex = (unsigned short)(p->bankIndex + bankDelta);

		if (src->mapRecordN >= 0 && src->mapRecordN < 3) {
			p->flatColor = expand565(src->mapColor);
		} else if (flat) {
			int          isPal = 0;
			unsigned int c     = resolve_flat_colour(src,
			                                         p->bankIndex,
			                                         &isPal);

			if (isPal) {
				p->colorIndex = (unsigned short)c;
				p->flags     |= PRIM_FLAG_PAL_INDEX;
				p->flatColor  = 0xffffffffu;
			} else {
				p->flatColor = c;
			}
		} else {
			p->flatColor = 0xffffffffu;
		}

		/*
		 * AND A FLAT FACE IS KIND 0 - Prim_SetKind(prim, 0), pushed
		 * as a literal at 0x004212b4 on BOTH arms of the flat branch,
		 * after colorIndex has been set from the sampled texel. So
		 * the kind table above applies ONLY to a face with real UV
		 * spans; a face whose UVs are all equal is untextured
		 * flat-shaded geometry however its .map flags read.
		 *
		 * THIS WAS THE WHOLE FIRST-PERSON COCKPIT. GUIDON.MAP's
		 * records are n = 3 and 4 with flags 0x00000007 and
		 * 0x00200007, so the table gave them kind 1 and 0x15 -
		 * textured, on a bank index of 7 - where MOTO reads kind 0
		 * with a sampled colorIndex. Read live: MOTO's guidon prim 0
		 * is kind 0, colorIndex 0xc8c8, flatColor 0xffc81840.
		 *
		 * It is not cosmetic anywhere else either: every one-texel
		 * detail patch in the game takes this path - 62 of the 196
		 * faces in SPEED\\MEDIUM\\CORPS.MAP alone, per
		 * formats/mesh-3df-map.txt 2.
		 */
		if (flat && src->hasMap && src->mapRecordN >= 3) {
			p->kind          = 0;
			p->effectiveKind = 0;
		}
	}

	*out   = prims;
	*count = mesh->faceCount;
	return 1;
}

/*
 * The per-frame half of the flat-colour path. See prim.h.
 *
 * ORIGIN is a SHAPE rather than a line-for-line transcription, and
 * the difference is worth stating. The original resolves a paletted
 * primitive by sending it down a separate queue -
 * Screen3D_CullAndQueueFaces tests bank[prim->bankIndex] + 8 at
 * 0x0049c516 and calls Screen3D_QueuePrimitivePaletted, which stores
 * bank[i].palette[k] into the queue node at 0x0049cdf7 for the
 * rasterizer to index. Our draw has one path and no queue node, so
 * the same lookup happens here, at the point the colour is needed.
 *
 * The selection differs too. The original picks the palette with a
 * queue-time argument k; ours reads the bank's activePalette, which
 * bank_set_palette writes from (g_selectedBike + spinOffset) & 7.
 * Same eight palettes, same result, chosen one level up.
 */
/* ORIGIN  Screen3D_QueuePrimitivePaletted @ 0x0049ca70, at 0x0049cdf7 */
unsigned int prim_flat_colour(const MR_Primitive *p)
{
	MR_Bank              *b;
	const unsigned short *pal;

	if (!p || !(p->flags & PRIM_FLAG_PAL_INDEX))
		return p ? p->flatColor : 0xffffffffu;

	b = bank_get(p->bankIndex);
	if (!b || !b->loaded || b->paletteCount < 1)
		return 0xffffffffu;

	pal = b->palette[b->activePalette];
	if (!pal)
		return 0xffffffffu;

	return expand565(pal[p->colorIndex & 0xff]);
}

/*
 * Mesh_RebaseFaceBanks (0x00422250). See prim.h.
 *
 * The original walks the mesh's face array directly and adds to the
 * short at +0x36; ours walks the converted primitives, which is the
 * same field by name.
 */
/* ORIGIN  Mesh_RebaseFaceBanks @ 0x00422250 */
void prim_rebase_banks(MR_Primitive *prims, int count, int delta)
{
	int i;

	if (!prims)
		return;
	for (i = 0; i < count; i++)
		prims[i].bankIndex = (unsigned short)(prims[i].bankIndex + delta);
}
