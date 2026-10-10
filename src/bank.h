/*
 * bank.h - texture banks.
 *
 * A bank owns one 256 x 256 16-bit page plus up to sixteen palettes,
 * and it is what a .map face record's bank index resolves to. From
 * Bankw_CreateBank, Bankw_LoadBank and Bankw_LoadRawTexture in
 * screen3d/bankw.Cpp; the write-up is
 * docs/subsystems/texture-banks.txt.
 *
 * SCOPE. This is the SOFTWARE half only. The original's bank record
 * also carries DirectDraw surfaces, D3D texture slots and the
 * reserved/published slot arrays, all of which are on the replace-list
 * and none of which mean anything until a back end exists. What is
 * here is the part a rewrite keeps: the decoded page, the palettes,
 * and the flags that say how to read them.
 */

#ifndef MRACER_BANK_H
#define MRACER_BANK_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 0x2e banks in the original, of which 0x00..0x18 are static and
 * 0x19..0x2c a dynamic pool. Bankw_LoadBank refuses an index above
 * 0x2c, so 45 are usable.
 */
#define BANK_COUNT       0x2e
#define BANK_MAX_INDEX   0x2c
#define BANK_MAX_PALETTES 16

/* The page is always allocated 256 x 256 x 2 = 0x20000 bytes, whatever
 * the file turns out to hold. A paletted page writes only 256 bytes
 * per row into a 512-byte row, so an 8-bit page occupies half of what
 * it reserves. Not a bug, but a port that sizes the allocation from
 * the texel format will differ from the original. */
#define BANK_PAGE_W      256
#define BANK_PAGE_H      256
#define BANK_PAGE_BYTES  0x20000

/*
 * MR_Bank.flags, the word at bank + 0x254 (bank 0 is 0x004f954c).
 * D3D_AllocTextureSlot ORs these in from its arguments; only the two
 * the blend ladder reads are reconstructed so far.
 * See docs/subsystems/texture-banks.txt section 5.2.
 */
#define BANK_FLAG_PAL8      0x0001  /* source is 8-bit paletted       */
#define BANK_FLAG_SRC16     0x0002  /* source is 16-bit               */
#define BANK_FLAG_SMALL     0x0008  /* surface is 128x128, not 256    */
#define BANK_FLAG_KEYED     0x0010  /* a colour key was applied       */
#define BANK_FLAG_PAL4      0x0020  /* source is 4-bit paletted       */
#define BANK_FLAG_SRC8      0x0040  /* source is 8-bit non-paletted   */
#define BANK_FLAG_KEYBLACK  0x0400  /* maybe_D3D_RegisterTexture's arg */

/*
 * The colour key is BLACK, and it is not configurable: the DDCOLORKEY
 * D3D_AllocTextureSlot builds at 0x0041b54b is {0, 0}, passed to
 * SetColorKey with DDCKEY_SRCBLT [verified].
 */
#define BANK_COLOUR_KEY     0x0000

typedef struct {
	unsigned short  flags;        /* +0x254 in the original       */
	int             loaded;
	int             width;        /* from the file header */
	int             height;
	int             isIndexed;    /* the page holds 8-bit indices */
	unsigned short *page;         /* BANK_PAGE_BYTES, RGB565 or indices */
	int             ownsPage;     /* the field a port needs most        */
	unsigned short *palette[BANK_MAX_PALETTES];  /* 256 entries each    */
	int             paletteCount;
	int             activePalette;  /* which one bank_texture uses */
	char            name[MAX_PATH];
} MR_Bank;

/*
 * bank_load - read a .RAW into bank `index`.
 *
 * mode is passed through from the caller exactly as the original does.
 * It matters in two ways, both surprising:
 *
 *   o MODE 0 KEEPS A PALETTE. That is what sets isIndexed.
 *
 *   o MODE 0 ON A 256x256 TRUECOLOUR FILE READS NO PIXELS AT ALL. The
 *     page is allocated and the loader returns success with it
 *     untouched. See bank.c and texture-banks.txt section 4.2 - this
 *     is reproduced deliberately.
 *
 * Returns 1 on success.
 */
int bank_load(int index, const char *path, int mode);

/*
 * bank_alloc_texture_slot - the FLAG half of D3D_AllocTextureSlot
 * (0x0041b0f0), which is all of that function the reconstruction needs.
 *
 * The original allocates a D3D texture slot and, on the way, records
 * how the source was formatted so that D3D_DrawScene can pick a blend
 * for it later. Those ORs are the part that matters here [verified,
 * 0x0041b24a..0x0041b56b]:
 *
 *     rgbBits == 16          flags |= 0x02
 *     paletted               flags |= 0x01 or 0x20 by palBits
 *     wantKey                flags |= 0x10, else flags &= ~0x10
 *
 * and when wantKey is set it also applies a colour key of BLACK to the
 * surface - unless -Alpha is on, in which case the key is carried as
 * per-texel alpha in a 4444 texture instead and SetColorKey is
 * skipped. Our back end always uploads with alpha, so it takes the
 * second path unconditionally, and bank_texture keys off flag 0x10.
 *
 * WHY THIS MATTERS: the blend ladder's one conditional row needs
 * 0x10 AND 0x02 together - "a colour-keyed 16-bit texture" - and
 * without it a keyed page draws its key colour as opaque black.
 */
/*
 * bank_is_bike_atlas - is this page the bike's PARTICLE AND EFFECTS
 * atlas?
 *
 * ONE QUESTION, TWO CALLERS, and they must agree: the FX pool draws
 * its smoke and tyre specks out of this page, and the texture upload
 * gives its two rectangles their special alpha. Both used to answer
 * it separately and one of them answered it for a single bike.
 *
 * THE ORIGINAL NEVER ASKS. maybe_Fx_InitParticle stores a LITERAL
 * BANK INDEX OF 1, and slot 1 holds whichever bike's BNK00_01.RAW
 * the level loaded - so a name match is the port's way of reaching
 * the same page, not the original's. See fx-particles.txt 7.6 and
 * the note there about the port's bank slots differing.
 *
 * maybe_D3D_LoadRawToTexture strcmps four literals for the rectangle
 * treatment [verified, 0x0041c2f0..0x0041c3b5]:
 *
 *     data\\SpeedPkt\\bnk00_01.raw     data\\speed\\bnk00_01.raw
 *     data\\ktmcross\\bnk00_01.raw     data\\KTMpkt\\bnk00_01.raw
 *
 * and those four are the same four pages the FX pool wants, because
 * they are the same page. Matched here on directory and file,
 * case-insensitively, ignoring any prefix - our logical names are
 * archive-relative where the original's are ini paths, and the
 * original needs four spellings only because strcmp is case
 * sensitive.
 *
 * A BARE MATCH ON THE FILE NAME IS WRONG: the title screen loads
 * PAGES\\TITLEDSI\\BNK00_01.RAW.
 *
 * Returns 1 if name is one of the four, 0 otherwise. NULL is 0.
 */
int bank_is_bike_atlas(const char *name);

/*
 * bank_set_palette - choose which of a bank's palettes resolves it.
 *
 * AN INDEXED PAGE PLUS EIGHT PALETTES IS HOW THE GAME RECOLOURS
 * ONE IMAGE. SPEED.BKF carries BNK00_00.RAW and BNK00_00.1 through
 * .11 - one 256x256 page of indices and eleven alternate palettes
 * for it, which bank_load already reads as siblings. The bike
 * liveries on the select page are palettes of a single page, not
 * twelve pages, which is why the tick masks its selector with 7.
 *
 * Invalidates the cached texture, so a change is picked up on the
 * next draw.
 */
void bank_set_palette(int index, int palette);

void bank_alloc_texture_slot(int index, int wantKey, int paletted,
                             int palBits, int rgbBits);

/*
 * THERE IS NO SEPARATE bank_set_colour_key. wantKey above is the only
 * switch, and the back end reads flag 0x10 when it builds the texture.
 *
 * THE TWO SWITCHES ARE STILL NOT THE SAME SWITCH - that part of the
 * old note was right - but the OTHER one is not a colour key at all.
 * maybe_D3D_RegisterTexture's keyBlack sets flag 0x400 and does two
 * things inside maybe_D3D_LoadRawToTexture [verified, 0x0041c050]:
 *
 *   o maps (0, 255, 0) - pure green - onto black, so green keys out;
 *   o RAISES near-black texels to the g_cfgOpaqueBlackLevel floor, so
 *     dark artwork stays OPAQUE.
 *
 * So keyBlack = 1 keys LESS black, not more, and its name reads
 * backwards. Every caller reaching these banks passes 0.
 *
 * EXACTLY ONE CALLER IN THE BINARY PASSES 1: maybe_Cdd_LoadRawToSurface
 * at 0x00478844, which serves the HORIZON BACKDROP (banks 4/5/6),
 * App_Init and Cdd_Init [verified, all seven call sites read]. A
 * full-screen sky-and-ground image is exactly what must not have its
 * dark half keyed away. D3D_InitDevice's eighth site passes
 * bank.flags & 0x400, replaying the bit rather than deciding it.
 *
 * o SO -OpaqueBlackLevel DOES NOTHING to a truecolour page other than
 *   the backdrop. Nothing in the reconstruction needs it yet; when the
 *   horizon lands, it does. See docs/subsystems/d3d-textures.txt
 *   section 3.1.
 */

MR_Bank *bank_get(int index);
/*
 * bank_create_empty - a bank with a blank page and no file behind it.
 * From Bankw_CreateBank (0x00401e40), whose single caller is
 * Menu_LoadResources at 0x0040aad1 with index 2. [verified]
 *
 * Bank 2 exists ONLY to hold the screen capture the glass shatter is
 * textured with. Nothing loads it from disk, so a reader looking for
 * its asset finds nothing and concludes the bank is unused.
 */
int bank_create_empty(int index, int width, int height);

/*
 * bank_capture_screen - fill a bank's page with the rendered frame.
 * From MenuTools_CaptureScreenToBank (0x00479260), called by
 * Menu_BeginTransition with bank 2 while the OUTGOING page is still on
 * screen. The 640x480 frame becomes 256x192 by the original's
 * 2-of-every-5 decimation - see r_capture_screen, which does the
 * sampling.
 *
 * o TIMING IS A PORT CONSTRAINT, NOT THE GAME'S. It must run after the
 *   scene is drawn and before the present, because our back buffer is
 *   DISCARD and holds nothing once presented. The original reads its
 *   own persistent software framebuffer and can do it whenever.
 *
 * Returns 1 on success. On failure the page keeps whatever it held,
 * which is a stale capture rather than garbage.
 */
int bank_capture_screen(int index);

void bank_free(int index);
void bank_free_all(void);

#ifdef __cplusplus
}
#endif

#endif /* MRACER_BANK_H */
