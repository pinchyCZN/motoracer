/*
 * drawscene.c - see drawscene.h.
 */

#include <windows.h>

#include "drawscene.h"
#include "render.h"
#include "detail.h"
#include "options.h"
#include "bank.h"
#include "shade.h"

float g_drawSceneZFar = 1.0f;

/*
 * g_cfgGouraud - DAT_004dc1d8, the -NoGouraud switch. DEFAULT 1,
 * which is the value in .data and NOT what the one writer stores -
 * see config-globals.txt 9.1 before trusting any flag's default.
 *
 * It decides two things: the SHADEMODE render state, and whether
 * D3D_WriteVertex computes a colour for every corner or only the
 * first.
 *
 *     SHADEMODE = (g_cfgGouraud == 0) ? D3DSHADE_FLAT
 *                                     : 2 - (DAT_0053bdfc == 0);
 *
 * Cd3d_Ctor sets DAT_0053bdfc to 1, so the default resolves to 2,
 * D3DSHADE_GOURAUD. D3D_WriteVertex colours a corner when its index
 * is 0 OR this is 1, so by default EVERY corner gets its own colour.
 */
int g_cfgGouraud = 1;

static int s_tris;
static int s_lines;
static int s_skipped;
static int s_forcePoint;

/* 1 normally; a pass that must sit over the scene clears it. */
static int s_depthTest = 1;

void drawscene_set_depth_test(int on)
{
	s_depthTest = on ? 1 : 0;
}

void drawscene_force_point_filter(int on)
{
	s_forcePoint = on ? 1 : 0;
}

void drawscene_begin(void)
{
	shade_init();
	s_tris    = 0;
	s_lines   = 0;
	s_skipped = 0;
}

void drawscene_stats(int *triangles, int *lines, int *skipped)
{
	if (triangles) *triangles = s_tris;
	if (lines)     *lines     = s_lines;
	if (skipped)   *skipped   = s_skipped;
}

/* ------------------------------------------------------------------ */
/* The two colour routes                                              */
/* ------------------------------------------------------------------ */

/*
 * D3D_DrawScene has TWO ways of colouring a vertex and picks between
 * them per primitive. This is the selector, transcribed [verified]:
 *
 *     untextured = kind == 0x0c || kind == 5 || kind == 0
 *               || bankIndex >= 0x2d
 *               || bank[bankIndex].texSlot[slot] == -1
 *
 * The bank bound is CMP EDX,0x2d / JGE at 0x00419dba in
 * D3D_EmitTriangle and again at 0x00419f3a in D3D_EmitLine, so the
 * last textured bank is 0x2c - which is BANK_MAX_INDEX.
 *
 * and the sense is the one that reads backwards at first glance:
 *
 *     UNTEXTURED  ->  the inline SHADE RAMP loop
 *     TEXTURED    ->  a call to D3D_WriteVertex
 *
 * It makes sense once stated. An untextured face has no texel to take
 * colour from, so it needs its flat colour run through the intensity
 * ramp. A textured face wants a GREY modulator instead, so that the
 * MODULATE stage gives texel times light. Two different jobs, two
 * routines.
 *
 * The last clause is a real D3D texture handle in the original. Here
 * it stands in as "the bank has a usable page", which is the same
 * question one layer down.
 */
static int prim_is_textured(const MR_Primitive *p)
{
	const MR_Bank *b;
	unsigned int   kind = p->effectiveKind;

	if (kind == 0x0c || kind == 5 || kind == 0)
		return 0;
	if (p->bankIndex > BANK_MAX_INDEX)
		return 0;
	b = bank_get((int)p->bankIndex);
	/*
	 * INDEXED PAGES COUNT AS TEXTURED. Excluding them looks right,
	 * because a palette has to be resolved before a texel means
	 * anything - and it is wrong: every face of a paletted bank
	 * then falls through to its flat colour, which for a textured
	 * face is 0xffffffff. A WHITE MODEL.
	 *
	 * The original test has no term that could exclude them. It
	 * asks only whether the bank index is in range and the slot
	 * is allocated.
	 */
	if (!b || !b->loaded || !b->page)
		return 0;
	return 1;
}

/*
 * The device capability that gates the kind-0x44 arm of the flat-alpha
 * stamp. It reported a device that could not blend a flat alpha and
 * faked it with a stipple; no D3D9 device does. Kept as a named
 * constant so the transcribed condition reads like the original rather
 * than quietly losing a term.
 */
#define DEV_DOES_ALPHA_FLAT_STIPPLED 0

/*
 * The three kinds that draw at half brightness. D3D_DrawScene sets
 * DAT_0053bb38 for exactly these and clears it otherwise, and
 * D3D_WriteVertex is the only reader - so the halving applies on the
 * TEXTURED route only.
 */
static int kind_is_halved(unsigned int kind)
{
	return kind == 0x15 || kind == 0x27 || kind == 0x20;
}

/*
 * D3D_WriteVertex's colour computation, transcribed [verified].
 *
 * The eight bits are replicated identically into R, G and B - a GREY,
 * which is what a MODULATE stage wants. Note this is a WIDER window of
 * the shade word than the ramp route uses: eight bits here against the
 * ramp's top five. Same number at two precisions.
 */
static unsigned int writevertex_colour(int shade, unsigned int flatColor,
                                       int halve)
{
	unsigned int s = (unsigned int)shade;
	unsigned int g;

	if (s & 0x80000000u) {
		/*
		 * Bit 31: the low 24 bits ARE the colour, not an intensity.
		 * The original leaves the alpha byte at zero here and forces
		 * the specular to opaque white. Reproduced as written - a zero
		 * alpha would matter on a blended primitive, but nothing in
		 * the data sets this bit, so the case is untested either way.
		 */
		return s & 0x00ffffffu;
	}
	if (s & 0x40000000u) {
		/*
		 * Bit 30: full bright, unshaded white. The original picks
		 * 0xffffffff instead when EITHER of DAT_004dc1bc or
		 * DAT_004dc1c0 is set - 0x004dc1c0 is -Alpha, the 4444
		 * texture format.
		 *
		 * [open] .data HOLDS 0 FOR BOTH AND THAT IS NOT THE VALUE
		 * AT DRAW TIME. D3D_InitDevice writes 1 into 0x004dc1c0 at
		 * 0x0041ecb9 when 0x004dc1cc is set, and that in turn is
		 * written by the command line and five times inside
		 * D3D_InitDevice - so which branch runs depends on the
		 * device. 0x00ffffff is the pre-init default, not a
		 * settled answer.
		 *
		 * It also skips the specular write on this branch alone,
		 * which costs nothing here: SPECULARENABLE is off in both
		 * back ends. See d3d9rt.cpp around the state block.
		 */
		return 0x00ffffffu;
	}

	g = (s & 0x0003fc00u) >> 10;
	if (halve)
		g /= 2;
	g = (g << 16) | (g << 8) | g;

	/* The primitive's own alpha byte survives, which is what carries
	 * the 0x80 the half blend depends on. */
	return (flatColor & 0xff000000u) | g;
}

/* ------------------------------------------------------------------ */

static void fill_vertex(R_Vertex *dst, const MR_Vertex *src,
                        float u, float v, unsigned int colour)
{
	/*
	 * CAMERA SPACE, NOT SCREEN SPACE. The renderer projects and clips
	 * these; see r_set_projection. camX and camY already carry
	 * g_projScaleX/Y, which transform.c folded into the matrix, so the
	 * projection only has to divide and map to the viewport.
	 *
	 * o USING camX/camY/camZ IS ALSO SAFER THAN screenX/screenY.
	 *   transform.c writes the screen pair ONLY when camZ passes the
	 *   near plane and leaves the previous occupant of the pool slot
	 *   there otherwise (MR_Vertex.txt section 7.2); the camera triple
	 *   is written unconditionally.
	 */
	dst->sx  = src->camX;
	dst->sy  = src->camY;
	dst->sz  = src->camZ;
	dst->rhw = src->invZ;    /* dropped downstream - the projection
	                            recomputes it. Kept so the record is
	                            whole. */

	dst->color    = colour;
	dst->specular = 0;

	/*
	 * UVs are TEXEL INDICES over a 256x256 page, not normalised - and
	 * an index has to be moved to the middle of its texel.
	 *
	 * A texel i covers [i/N, (i+1)/N) and its CENTRE is (i+0.5)/N.
	 * Sampling at i/N lands on the boundary between texels i-1 and
	 * i, so a linear filter returns a 50/50 blend of two texels at
	 * every corner of every face - worst where the art is smallest,
	 * and 62 of the bike body's 196 faces span one texel or less.
	 *
	 * A PORT CORRECTION, NOT A TRANSCRIPTION. The original had no
	 * such problem: its software rasterizer indexes texels directly
	 * and its D3D path fed a different API.
	 */
	dst->tu = (u + 0.5f) * (1.0f / (float)BANK_PAGE_W);
	dst->tv = (v + 0.5f) * (1.0f / (float)BANK_PAGE_H);
}

void drawscene_dispatch(const MR_QueueNode *node)
{
	const MR_Primitive *prim;
	const MR_Vertex    *verts;
	R_Vertex            rv[4];
	int                 idx[4];
	unsigned int        kind;
	int                 keyed16;
	unsigned int        base;
	unsigned int        corner0 = 0;
	int                 corners;
	int                 textured;
	int                 halve;
	int                 blend;
	int                 i;

	if (!node || !node->prim || !node->vertices)
		return;

	prim  = node->prim;
	verts = node->vertices;

	/*
	 * The frustum these vertices are projected and CLIPPED with. It is
	 * per-primitive only because there is no per-frame hook here; the
	 * call compares its four arguments and returns immediately when
	 * nothing moved, so a whole page costs four float compares a face.
	 *
	 * nearZ is HALF g_nearPlaneZ on purpose. The software clipper puts
	 * rebuilt corners at exactly g_nearPlaneZ, and a vertex sitting
	 * exactly on the near plane is a coin toss once D3D9 does the
	 * comparison in its own precision. Half is a free margin.
	 */
	r_set_projection((float)g_viewCentreX, (float)g_viewCentreY,
	                 g_nearPlaneZ * 0.5f, g_drawSceneZFar);

	/*
	 * THE EFFECTIVE KIND, NEVER THE AUTHORED ONE. Both back ends read
	 * +0x3a and never +0x00, because detail reduction has already
	 * substituted one kind for another and neither of them knows it
	 * happened. Reading prim->kind here would silently undo the whole
	 * level-of-detail system.
	 */
	kind = prim->effectiveKind;

	corners  = (prim->vertexIndex[4] != PRIM_NO_WRAP) ? 4 : 3;
	textured = prim_is_textured(prim);
	halve    = kind_is_halved(kind);

	/*
	 * The blend ladder's "textured" is NOT the route selector, even
	 * though they overlap. It is the state mask's bit 0, and
	 * D3D_DrawScene builds that from the KIND alone - set for anything
	 * that is not kind 0, 5 or 0x0c - without asking whether a texture
	 * is actually bound.
	 */
	/*
	 * keyed16 IS THE BANK'S OWN 0x10|0x02, never a constant. It
	 * selects the ladder's one conditional row, so a fixed 0 makes
	 * that row unreachable and every colour-keyed page draws its key
	 * colour - black - fully opaque.
	 */
	{
		const MR_Bank *bank = bank_get((int)prim->bankIndex);
		const unsigned short want = BANK_FLAG_KEYED | BANK_FLAG_SRC16;

		keyed16 = (bank && (bank->flags & want) == want);
	}

	blend = r_blend_for_kind(kind,
	                         (kind != 0 && kind != 5 && kind != 0x0c),
	                         keyed16);

	/*
	 * A COLOUR-KEYED TEXTURE IS NOT TRANSLUCENT. Its texels are either
	 * fully opaque or fully absent - bank_texture writes alpha 0 or
	 * 255 and nothing between - so the geometry is opaque geometry
	 * with holes in it, and opaque geometry must WRITE DEPTH.
	 *
	 * o "WRITE DEPTH UNLESS BLENDING" IS THE WRONG RULE HERE. It is
	 *   right for real translucency, and for keying it turns depth
	 *   writes off on every keyed surface - which, since the menu
	 *   pages texture almost entirely from keyed banks, leaves the
	 *   depth buffer enabled, cleared every frame and never stamped.
	 *   All that remains is the queue's bucket sort, and that orders
	 *   OBJECTS while doing nothing for the faces inside one, so a
	 *   solid model paints its far faces over its near ones.
	 *
	 * The alpha test is what makes writing depth safe here - without
	 * it the keyed-away texels would stamp the quad's whole rectangle
	 * into the depth buffer. Blending stays on regardless, so the
	 * ladder is still the ladder.
	 *
	 * THE ALPHA TEST FOLLOWS THE TEXTURE, NOT THE BLEND, and that is
	 * an emulation decision rather than a transcription. The original
	 * keys at the DirectDraw level - D3D_AllocTextureSlot sets a
	 * DDCOLORKEY of black with DDCKEY_SRCBLT - so a keyed texel is
	 * ABSENT whatever blend mode is in force. We express the key as
	 * alpha 0 in the texture, which only removes the texel if the
	 * alpha test is on.
	 *
	 * o DO NOT COUPLE IT TO blend == R_BLEND_SRCALPHA. That holds
	 *   for every kind reaching the ladder's conditional row and
	 *   fails for the four that return earlier - 0x2c, 0x3e, 0x44
	 *   and 0x45 all take the half blend, so a keyed texture on one
	 *   of them blends its key colour in at fifty per cent. Kind
	 *   0x44 is the smoke particles, on a keyed bank whose atlas is
	 *   nearly black except where the key cuts it away.
	 *
	 * o THE DEPTH RULE IS DELIBERATELY LEFT ALONE. A half-blended
	 *   surface is genuinely translucent and must not write depth,
	 *   which is a different question from whether its texels are
	 *   keyed - so that test still asks about the BLEND.
	 */
	{
		int keyedBlend = (blend == R_BLEND_SRCALPHA);

		r_set_blend(blend);
		r_set_alpha_test(textured && keyed16);
		/*
		 * THE DEPTH TEST IS PER PRIMITIVE, so a caller cannot turn
		 * it off around a queue_walk - the first primitive puts it
		 * straight back. s_depthTest is how a pass says otherwise.
		 *
		 * It stands in for the original's own mechanism, which is
		 * per VERTEX: Hud_DrawMap pins invZ to 1.0 on every map
		 * vertex so it always wins. Our vertices go to the renderer
		 * in CAMERA SPACE and the depth comes from camZ, so invZ
		 * has nothing to feed and the pinning has no effect. Same
		 * intent, one level up.
		 */
		r_set_depth(s_depthTest,
		            s_depthTest &&
		            (blend == R_BLEND_NONE || keyedBlend));
	}
	/*
	 * AND WHICH PALETTE. node->palette is the livery the cull
	 * stage stamped on - -1 for everything that is not a racer,
	 * which leaves the bank's own active palette in charge.
	 */
	r_set_texture(textured ? (int)prim->bankIndex : -1,
	              node->palette);

	/*
	 * THE MASTER SWITCH FIRST, THEN THE PER-KIND BIT.
	 *
	 * ORIGIN  D3D_DrawScene @ 0x004afa5b
	 *
	 *     filter = DAT_0053be0c && ((featureTable[kind] & 8) == 0)
	 *
	 * Bit 3 of the feature table is INVERTED - it means
	 * "do NOT filter this kind" - and the original ANDs it with a
	 * global filtering switch that D3D_SetTextureFilter also
	 * forces to 0 whenever the switch is clear.
	 *
	 * BOTH TERMS ARE NEEDED. 58 of the 79 kinds lack bit 3, so
	 * reading the bit without the master switch filters all 58
	 * where a stock game point samples every one of them. See
	 * d3d-render-state.txt 5.
	 *
	 * s_forcePoint stays an override on top - see drawscene.h
	 * for its one caller.
	 */
	r_set_filter((g_options.textureFilter && !s_forcePoint &&
	              !(detail_bits(kind) & DETAIL_NO_FILTER))
	             ? 1 : 0);

	/*
	 * THE FLAT ALPHA IS SELECTED BY KIND, NOT BY "IS IT BLENDED".
	 * D3D_DrawScene stamps the top byte of the primitive colour
	 * immediately before the state selection [verified,
	 * d3d-render-state.txt section 3.2]:
	 *
	 *     prim[0x3f] = 0xff;
	 *     if (kind == 0x2c || kind == 0x3e || kind == 0x0c ||
	 *         (kind == 0x44 && devDoesAlphaFlatStippled))
	 *         alpha = 0x80;
	 *
	 * o DO NOT SUBSTITUTE blend != R_BLEND_NONE FOR THE KIND LIST.
	 *   The two agree only while the half blend and the additive one
	 *   are the only reachable blends; the colour-keyed SRCALPHA row
	 *   is a blend that is NOT in the list, and the proxy extends the
	 *   50 per cent to it, halving the art as well as the key. The
	 *   keyed row exists precisely BECAUSE a keyed texture must not
	 *   take the half blend - its transparency is per-texel.
	 *
	 *   A PROXY FOR A CONDITION IS A BUG WITH A DELAY: it fails the
	 *   moment somebody widens one of the two, which is exactly when
	 *   nobody is reading this line.
	 *
	 * The 0x44 arm is kept for shape. THE SMOKE PARTICLES DO produce
	 * kind 0x44 - maybe_Prim_InitQuad takes it at both call sites in
	 * the particle constructor and tick. 0x45 is unproduced, and
	 * DoesAlphaFlatStippled is a capability for devices that faked
	 * flat alpha with a stipple, which no D3D9 device reports. So the
	 * arm is unreachable here and the flag is a constant 0; see
	 * d3d-render-state.txt section 6.
	 */
	/* NOT prim->flatColor - a paletted face resolves per frame, so
	 * the livery follows the bike. prim.h. */
	base = prim_flat_colour(prim) | 0xff000000u;
	if (kind == 0x2c || kind == 0x3e || kind == 0x0c ||
	    (kind == 0x44 && DEV_DOES_ALPHA_FLAT_STIPPLED)) {
		base = (base & 0x00ffffffu) |
		       ((unsigned int)R_HALF_BLEND_ALPHA << 24);
	}

	for (i = 0; i < corners; i++) {
		const MR_Vertex *mv = &verts[prim->vertexIndex[i]];
		unsigned int     colour;

		/* screenX and screenY are written only when camZ passes the
		 * near plane. The cull and the clipper between them should
		 * have removed these, so reaching here means something
		 * upstream let one through. */
		if (mv->camZ < g_nearPlaneZ) {
			s_skipped++;
			return;
		}

		if (textured) {
			/*
			 * D3D_WriteVertex computes the colour for corner 0 always
			 * and for the others only when g_cfgGouraud is set - which
			 * it is by default. With -NoGouraud, D3DSHADE_FLAT takes
			 * the first vertex's colour for the whole triangle, so
			 * colouring the rest would be wasted work and the original
			 * skips it.
			 *
			 * We replicate corner 0's colour in that case rather than
			 * leaving the others unwritten. Identical under flat
			 * shading, and it means the back end can keep SHADEMODE at
			 * Gouraud rather than depending on which vertex D3D9
			 * considers first.
			 */
			if (i == 0 || g_cfgGouraud)
				colour = writevertex_colour(mv->shade, base, halve);
			else
				colour = corner0;
		} else {
			/* The inline route: the primitive's flat colour through
			 * the intensity ramp, per corner. */
			colour = shade_apply(base, mv->shade);
		}
		if (i == 0)
			corner0 = colour;

		fill_vertex(&rv[i], mv, prim->uv[i * 2], prim->uv[i * 2 + 1],
		            colour);
	}

	for (i = 0; i < corners; i++) {
		idx[i] = r_write_vertex(&rv[i]);
		if (idx[i] < 0) {
			s_skipped++;
			return;
		}
	}

	if (prim->flags & PRIM_FLAG_LINE) {
		r_emit_line(idx[0], idx[1]);
		s_lines++;
		return;
	}

	/*
	 * A quad becomes two triangles sharing an edge - D3D_EmitTriangle's
	 * mode 2 then mode 1, which name v,v+1,v+2 and then v,v+2,v+3.
	 */
	r_emit_triangle(idx[0], idx[1], idx[2]);
	s_tris++;
	if (corners == 4) {
		r_emit_triangle(idx[0], idx[2], idx[3]);
		s_tris++;
	}
}
