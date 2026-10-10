/*
 * font.h - the bitmap font family.
 *
 * THE ONE REAL CLASS HIERARCHY IN THE GAME, and it is small. src/
 * README.txt section 24.3 sized it before any of it was written; this
 * is that estimate cashed in.
 *
 * Six classes plus two vtable-swap variants share a SEVEN-SLOT vtable,
 * and only three slots ever differ [verified, all eight vtables read]:
 *
 *   +----+------------------------+----------+----------------------+
 *   |Slot| Role                   | Distinct | In C                 |
 *   +====+========================+==========+======================+
 *   | 0  | set fore/back colour   | 2        | function pointer     |
 *   +----+------------------------+----------+----------------------+
 *   | 1  | fill the colour LUT    | 5        | function pointer     |
 *   +----+------------------------+----------+----------------------+
 *   | 2  | load face, char range  | 1        | plain function       |
 *   +----+------------------------+----------+----------------------+
 *   | 3  | load face, .FNT        | 1        | plain function       |
 *   +----+------------------------+----------+----------------------+
 *   | 4  | load face, char range  | 1        | plain function       |
 *   +----+------------------------+----------+----------------------+
 *   | 5  | load face, other ext   | 1        | plain function       |
 *   +----+------------------------+----------+----------------------+
 *   | 6  | draw one glyph         | 8        | function pointer     |
 *   +----+------------------------+----------+----------------------+
 *
 * So: THREE FUNCTION POINTERS AND FOUR ORDINARY FUNCTIONS. Slots 2..5
 * are the same function in all eight vtables - virtual only because
 * the base declared them so, with nothing overriding.
 *
 *
 * THE COLOUR LUT IS THE WHOLE DESIGN, and it took reading two blitters
 * and slot 1 to see it. MR_Font+0x418 is 0x200 bytes that MR_Font.txt
 * had as "never touched by anything read so far". It is a 256-ENTRY
 * RGB565 TABLE INDEXED BY THE GLYPH'S PIXEL BYTE [verified].
 *
 * So a glyph pixel is not an intensity and not a palette index. It is
 * an index into a table the FONT owns and the face does not, which is
 * why the same face can be drawn in different colours without touching
 * its pixels, and why slot 0 triggers a slot 1 rebuild.
 *
 * The base fills it with a 32-step gradient from backColor to
 * foreColor, repeated eight times - entry i gets ramp32[i & 0x1f]. So
 * the byte splits:
 *
 *     bits 0..4   which of 32 gradient steps        (b & 0x1f)
 *     bits 5..7   the blend level, 0 = opaque       (b >> 5)
 *
 * ONE BYTE CARRYING BOTH COLOUR AND ALPHA is what makes AntiAlias8
 * work, and it explains the shipped data exactly: MSG.FNT's pixel
 * values are 0, 31, 63, 95, ... 255, which is 31 | (level << 5) -
 * always full foreground, varying only in alpha.
 *
 * See docs/structs/MR_Font.txt and docs/formats/fnt.txt.
 */

#ifndef MRACER_FONT_H
#define MRACER_FONT_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Four faces per font, and the bound is not a guess: the face path
 * array in MR_Font is 0x204 bytes of 0x81-byte records, which is
 * exactly four, and the count field is only ever 0..4 [verified].
 */
#define FONT_MAX_FACES 4
#define FONT_CHARS     256

/* The gradient the base slot 1 builds, and the mask that selects it. */
#define FONT_RAMP_STEPS 32
#define FONT_RAMP_MASK  0x1f

/* Blend levels, from the top three bits. 0 is opaque. */
#define FONT_LEVELS     8

/*
 * A loaded .FNT face. See docs/formats/fnt.txt - the format is decoded
 * byte-exact and tools/fnt.py is the oracle.
 */
typedef struct MR_FontFace {
	int             firstChar;
	int             charCount;
	int             glyphHeight;
	int             paletteEntries;     /* 0, or 256 for a Pal face   */
	const unsigned char *palette;       /* 3 bytes per entry, or NULL */
	const unsigned int  *widths;        /* charCount entries          */
	const unsigned char *pixels;
	unsigned int   *glyphOffset;        /* charCount, into pixels     */
	void           *blob;               /* the asset_load buffer      */
} MR_FontFace;

struct MR_Font;

/*
 * The three slots that vary. Everything else is a plain function
 * below, because the binary makes it one.
 */
typedef struct MR_FontClass {
	const char  *name;                  /* the config type name       */
	int          kind;                  /* the value the ctor stores  */
	unsigned int vtable;                /* the original's, for tracing*/

	/* slot 0 */
	void (*set_colors)(struct MR_Font *f,
	                   const unsigned char fore[3],
	                   const unsigned char back[3]);
	/* slot 1 - fill colorLut over an inclusive index range */
	void (*build_lut)(struct MR_Font *f, int firstIndex, int lastIndex);
	/* slot 6 */
	void (*draw_glyph)(struct MR_Font *f, int x, int y, int ch, int level);
} MR_FontClass;

typedef struct MR_Font {
	const MR_FontClass *cls;
	int          kind;

	MR_FontFace  faces[FONT_MAX_FACES];
	int          faceCount;

	/*
	 * MR_Font+0x418. Indexed by the glyph's pixel byte, NOT by
	 * character code. See the header comment.
	 */
	unsigned short colorLut[FONT_CHARS];

	/* Which face supplies each character, and its advance. */
	signed char  charFace[FONT_CHARS];  /* -1 = absent                */
	int          charWidth[FONT_CHARS]; /* 0 = absent                 */

	int          glyphHeight;
	int          charSpacing;           /* MR_Font+0x0a20             */

	unsigned char foreColor[3];         /* MR_Font+0x5494             */
	unsigned char backColor[3];         /* MR_Font+0x5497             */

	/* Ours, not the original's - see font_set_tabular_digits. */
	int          tabularDigits;
	int          digitCell;

	int          loaded;
} MR_Font;


/* ---------------------------------------------------------------- */
/* Faces                                                            */

/*
 * font_face_load - read one .FNT by LOGICAL NAME.
 *
 * "FONTES\\HIRES\\CHRONO.FNT" - it resolves through asset_load, so it
 * comes out of FONTES.BKF on a stock install. CLAUDE.md rule 10.
 */
int  font_face_load(MR_FontFace *face, const char *logical);
void font_face_free(MR_FontFace *face);

/* Pixels of one glyph, or NULL. Row major, width*height bytes. */
const unsigned char *font_face_glyph(const MR_FontFace *face, int ch,
                                     int *widthOut);


/* ---------------------------------------------------------------- */
/* Fonts                                                            */

/*
 * font_init - set a font up as an instance of a named class.
 *
 * typeName is the config name: "Direct", "DirectTrame", "CustomColor",
 * "CustomColorTrame", "AntiAlias8", "AntiAlias8Transp", "Pal", "Moto".
 * Returns 0 on an unknown name.
 *
 * THE ORIGINAL THROWS A C++ EXCEPTION on an unknown type name rather
 * than falling back. Returning 0 is the C equivalent.
 */
int  font_init(MR_Font *f, const char *typeName);
void font_free(MR_Font *f);

/* Attach a face. Up to FONT_MAX_FACES; returns 0 when full or on a
 * load failure. */
int  font_add_face(MR_Font *f, const char *logical);

/* slot 1. Rebuild colorLut over an inclusive index range. */
void font_build_lut(MR_Font *f, int firstIndex, int lastIndex);

/* slot 0. Either pointer may be NULL to leave that colour alone. */
void font_set_colors(MR_Font *f, const unsigned char fore[3],
                     const unsigned char back[3]);

/* Width of a string in pixels, charSpacing included between glyphs. */
int  font_text_width(const MR_Font *f, const char *s);
int  font_height(const MR_Font *f);

/*
 * TABULAR DIGITS. Off by default, because the original has no such
 * thing - it is a readability choice for our own overlays.
 *
 * The shipped faces are PROPORTIONAL: in DEBUG.FNT the ten digits are
 * not all the same width, so a number that changes value changes the
 * position of everything after it and a running counter visibly
 * shuffles. With this on, every digit is advanced by the widest digit
 * in the font and centred in that cell, so only the digits themselves
 * change and the text after them holds still.
 */
void font_set_tabular_digits(MR_Font *f, int on);

/*
 * font_draw_text - draw into the framebuffer at x, y.
 *
 * x, y is the TOP LEFT of the first glyph. level is the blend/fade
 * level the original passes as its last argument: 0 is normal, and
 * higher values fade for AntiAlias8 or dissolve for Trame.
 */
void font_draw_text(MR_Font *f, int x, int y, const char *s, int level);

/*
 * font_draw_text_aligned - the same, with the original's alignment
 * flags applied first.
 *
 * ORIGIN  Font_ApplyAlignment @ 0x00492c20, whole
 *
 *     h = flags & 0x1f            v = flags & 0xe0
 *
 *     h == 2 or 4     x = (g_clipWidth - textW) / 2      centred
 *     h == 8 or 0x10  x = (g_clipWidth - x) - textW      RIGHT, x
 *                                                        being an
 *                                                        inset from
 *                                                        the right
 *     otherwise       x unchanged                        left
 *
 *     v == 0x40       y = (g_clipHeight - textH) / 2     centred
 *     v == 0x80       y = (g_clipHeight - y) - textH     bottom
 *     otherwise       y unchanged                        top
 *
 * NOT Text_ApplyAlignment (0x00417900), which serves the menu
 * widgets and decodes a different bit layout. Two schemes.
 *
 * FLAG 8 IS THE RACE HUD'S and flag 0x21 is the counter's, and 0x21
 * means no alignment at all - the one combination the original skips
 * the text measure for. See docs/subsystems/hud-race.txt 4.
 */
void font_draw_text_aligned(MR_Font *f, int x, int y, const char *s,
                            unsigned int flags, int level);

/*
 * font_layout_divisor - 2 when the HUD's authored coordinates are
 * halved, 1 otherwise.
 *
 * ORIGIN  Font_DrawTextInternal @ 0x00493820, the test at 0x00493a5c
 *
 * font_draw_text_aligned applies this itself. It is exposed because
 * a caller that needs to know WHERE its text landed - to composite
 * the rectangle it occupies, say - has to apply the same transform,
 * and two copies of the condition would drift.
 */
int font_layout_divisor(void);

/* The class table, for tooling and tests. Terminated by a NULL name. */
const MR_FontClass *font_classes(void);
const MR_FontClass *font_class_by_name(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* MRACER_FONT_H */
