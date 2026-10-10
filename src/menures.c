/*
 * menures.c - the menu resource manifest, transcribed from the static
 * table at 0x004e3e20.
 *
 * Emitted from the binary rather than typed, and sorted by ID. The
 * table in the executable is NOT in id order - menu-resources.txt
 * section 2 records eight places where the id decreases from one
 * physical record to the next, which is what ids appended by hand over
 * time look like. Everything indexes by id at run time, so the
 * physical order carries no meaning and is not preserved here.
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "menures.h"
#include "config.h"
#include "options.h"

/* id, name, preload, localised, kind */
static const MENURES_ENTRY s_res[MENURES_COUNT] = {
	{   0, "paralax1.lez",  1, 0, 4 },
	{   1, "paralax2.lez",  1, 0, 4 },
	{   2, "paralax3.lez",  1, 0, 4 },
	{   3, "maintit.lez",   1, 1, 1 },
	{   4, "SOLO_D.lez",    1, 1, 1 },
	{   5, "SOLO_OK.lez",   1, 1, 1 },
	{   6, "LINK_D.lez",    1, 1, 1 },
	{   7, "LINK_OK.lez",   1, 1, 1 },
	{   8, "selmode.lez",   1, 1, 1 },
	{   9, "practic2.lez",  1, 1, 1 },
	{  10, "practice.lez",  1, 1, 1 },
	{  11, "single2.lez",   1, 1, 1 },
	{  12, "single.lez",    1, 1, 1 },
	{  13, "champio2.lez",  0, 1, 1 },
	{  14, "champion.lez",  0, 1, 1 },
	{  15, "Grdprix2.lez",  0, 1, 1 },
	{  16, "Grdprix.lez",   0, 1, 1 },
	{  17, "newgame.lez",   1, 1, 1 },
	{  18, "toptitle.lez",  1, 1, 1 },
	{  19, "bestlap.lez",   1, 1, 1 },
	{  20, "bestRace.lez",  1, 1, 1 },
	{  21, "resultsG.lez",  0, 1, 1 },
	{  22, "results.lez",   0, 1, 1 },
	{  23, "Totalpts.lez",  1, 1, 1 },
	{  24, "Coupe.lez",     1, 0, 1 },
	{  25, "selrace.lez",   1, 1, 1 },
	{  26, "left.lez",      1, 0, 1 },
	{  27, "left_l.lez",    1, 0, 1 },
	{  28, "right.lez",     1, 0, 1 },
	{  29, "right_l.lez",   1, 0, 1 },
	{  30, "num0.lez",      1, 0, 1 },
	{  31, "num1.lez",      1, 0, 1 },
	{  32, "num2.lez",      1, 0, 1 },
	{  33, "num3.lez",      1, 0, 1 },
	{  34, "num4.lez",      1, 0, 1 },
	{  35, "num5.lez",      1, 0, 1 },
	{  36, "num6.lez",      1, 0, 1 },
	{  37, "num7.lez",      1, 0, 1 },
	{  38, "num8.lez",      1, 0, 1 },
	{  39, "num9.lez",      1, 0, 1 },
	{  40, "moin.lez",      1, 0, 1 },
	{  41, "moin_l.lez",    1, 0, 1 },
	{  42, "moiflach.lez",  1, 0, 1 },
	{  43, "plus.lez",      1, 0, 1 },
	{  44, "plus_l.lez",    1, 0, 1 },
	{  45, "pluflach.lez",  1, 0, 1 },
	{  46, "laps.lez",      1, 1, 1 },
	{  47, "choosefo.lez",  1, 0, 1 },
	{  48, "choosemo.lez",  1, 1, 1 },
	{  49, "features.lez",  1, 1, 1 },
	{  50, "featacc.lez",   1, 0, 1 },
	{  51, "featspe.lez",   1, 0, 1 },
	{  52, "featgri.lez",   1, 0, 1 },
	{  53, "featbra.lez",   1, 0, 1 },
	{  54, "auto.lez",      1, 1, 1 },
	{  55, "autofla.lez",   1, 1, 1 },
	{  56, "autosha.lez",   1, 1, 1 },
	{  57, "manu.lez",      1, 1, 1 },
	{  58, "manufla.lez",   1, 1, 1 },
	{  59, "manusha.lez",   1, 1, 1 },
	{  60, "optitre.lez",   1, 1, 1 },
	{  61, "bouthaut.lez",  1, 0, 1 },
	{  62, "FOCUShau.lez",  1, 0, 1 },
	{  63, "bouthau2.lez",  1, 0, 1 },
	{  64, "boutbas.lez",   1, 0, 1 },
	{  65, "FOCUSbas.lez",  1, 0, 1 },
	{  66, "boutbas2.lez",  1, 0, 1 },
	{  67, "titre.lez",     1, 1, 1 },
	{  68, "stitre.lez",    1, 1, 1 },
	{  69, "tableau.lez",   1, 0, 2 },
	{  70, "accept.lez",    1, 1, 1 },
	{  71, "cancel.lez",    1, 1, 1 },
	{  72, "disconet.lez",  1, 1, 1 },
	{  73, "exit.lez",      1, 1, 1 },
	{  74, "join.lez",      1, 1, 1 },
	{  75, "options.lez",   1, 1, 1 },
	{  76, "start.lez",     1, 1, 1 },
	{  77, "continue.lez",  1, 1, 1 },
	{  78, "Btsave.lez",    1, 1, 1 },
	{  79, "BtLoad.lez",    1, 1, 1 },
	{  80, "BtAbort.lez",   1, 1, 1 },
	{  81, "Btback.lez",    1, 1, 1 },
	{  82, "BtNextRa.lez",  1, 1, 1 },
	{  83, "BtVali.lez",    1, 1, 1 },
	{  84, "BtUnvali.lez",  1, 1, 1 },
	{  85, "Pointeur.lez",  1, 0, 1 },
	{  86, "BtHiscor.lez",  1, 1, 1 },
	{  87, "BtConfig.lez",  1, 1, 1 },
	{  88, "poc_L.lez",     1, 0, 1 },
	{  89, "poc_D.lez",     1, 0, 1 },
	{  90, "rev_L.lez",     1, 0, 1 },
	{  91, "rev_D.lez",     1, 0, 1 },
	{  92, "rev_on_D.lez",  1, 1, 0 },
	{  93, "rev_on_L.lez",  1, 1, 0 },
	{  94, "rev_offD.lez",  1, 1, 0 },
	{  95, "rev_offL.lez",  1, 1, 0 },
	{  96, "number1.lez",   1, 0, 1 },
	{  97, "number2.lez",   1, 0, 1 },
	{  98, "number3.lez",   1, 0, 1 },
	{  99, "number4.lez",   1, 0, 1 },
	{ 100, "number5.lez",   1, 0, 1 },
	{ 101, "number6.lez",   1, 0, 1 },
	{ 102, "number7.lez",   1, 0, 1 },
	{ 103, "number8.lez",   1, 0, 1 },
	{ 104, "ligne.lez",     1, 0, 3 },
	{ 105, "enternam.lez",  1, 1, 1 },
	{ 106, "padcard.lez",   1, 1, 1 },
	{ 107, "clavcard.lez",  1, 1, 1 },
	{ 108, "joycard.lez",   1, 1, 1 },
	{ 109, "choset.lez",    1, 1, 1 },
	{ 110, "setting.lez",   1, 1, 1 },
	{ 111, "set.lez",       1, 0, 1 },
	{ 112, "set2.lez",      1, 0, 1 },
	{ 113, "CHFOND.lez",    1, 0, 4 },
	{ 114, "CHTITREG.lez",  0, 1, 1 },
	{ 115, "CHTITRE.lez",   0, 1, 1 },
	{ 116, "CHBARRE1.lez",  1, 0, 1 },
	{ 117, "CHBARRE2.lez",  1, 0, 1 },
	{ 118, "CHLOADA.lez",   1, 1, 1 },
	{ 119, "CHLOADB.lez",   1, 1, 1 },
	{ 120, "CHSAVEA.lez",   1, 1, 1 },
	{ 121, "CHSAVEB.lez",   1, 1, 1 },
	{ 122, "CHLIGNE1.lez",  1, 0, 1 },
	{ 123, "CHLIGNE2.lez",  1, 0, 1 },
	{ 124, "CHLIGNE3.lez",  1, 0, 1 },
	{ 125, "LKBARRE1.lez",  1, 1, 1 },
	{ 126, "LKLIGNE1.lez",  1, 0, 0 },
	{ 127, "LKLIGNE2.lez",  1, 0, 0 },
	{ 128, "LKLIGNE3.lez",  1, 0, 2 },
	{ 129, "LKLIGNE4.lez",  1, 0, 2 },
	{ 130, "LKLED1.lez",    1, 0, 0 },
	{ 131, "LKLED2.lez",    1, 0, 0 },
	{ 132, "LKLED3.lez",    1, 0, 1 },
	{ 133, "LKLED4.lez",    1, 0, 1 },
	{ 134, "LKCOURSE.lez",  1, 0, 1 },
	{ 135, "LKBARRE2.lez",  1, 0, 1 },
	{ 136, "LKFEATUR.lez",  1, 1, 1 },
	{ 137, "LKAUTO.lez",    1, 1, 0 },
	{ 138, "LKPOCON.lez",   1, 0, 0 },
	{ 139, "LKPOCOFF.lez",  1, 0, 0 },
	{ 140, "LKLAND.lez",    1, 0, 1 },
	{ 141, "LKREVON.lez",   1, 0, 0 },
	{ 142, "LKREVOFF.lez",  1, 0, 0 },
	{ 143, "LKFLG1.lez",    1, 0, 1 },
	{ 144, "LKFLD1.lez",    1, 0, 1 },
	{ 145, "LKFLG2.lez",    1, 0, 1 },
	{ 146, "LKFLD2.lez",    1, 0, 1 },
	{ 147, "LKBARRE3.lez",  1, 1, 1 },
	{ 148, "LKFEATU1.lez",  1, 1, 1 },
	{ 149, "LKFEATU2.lez",  1, 1, 1 },
	{ 150, "LKMANUEL.lez",  1, 1, 0 },
	{ 151, "LKLOCK1.lez",   1, 0, 0 },
	{ 152, "LKLOCK2.lez",   1, 0, 0 },
	{ 153, "LKCOMPU1.lez",  1, 0, 0 },
	{ 154, "LKCOMPU2.lez",  1, 0, 0 },
	{ 155, "SSBARRE1.lez",  1, 0, 0 },
	{ 156, "SSBARRE2.lez",  1, 0, 0 },
	{ 157, "SSBARRE3.lez",  1, 0, 0 },
	{ 158, "SSBARRE4.lez",  1, 0, 0 },
	{ 159, "SSSCROL1.lez",  1, 0, 0 },
	{ 160, "SSFOND1.lez",   1, 0, 0 },
	{ 161, "SSOMBRE1.lez",  1, 0, 0 },
	{ 162, "SSTITRE1.lez",  1, 1, 1 },
	{ 163, "SSTITRE2.lez",  1, 1, 1 },
	{ 164, "SSCURSE1.lez",  1, 0, 1 },
	{ 165, "SSCURSE2.lez",  1, 0, 1 },
	{ 166, "SSFLECH1.lez",  1, 0, 1 },
	{ 167, "SSFLECH2.lez",  1, 0, 1 },
	{ 168, "SSFLECH3.lez",  1, 0, 1 },
	{ 169, "SSFLECH4.lez",  1, 0, 1 },
	{ 170, "btinfo.lez",    1, 0, 1 },
	{ 171, "timeatak.lez",  1, 0, 1 },
	{ 172, "attackon.lez",  1, 1, 1 },
	{ 173, "attackof.lez",  1, 1, 1 },
	{ 174, "attakfla.lez",  1, 0, 1 },
	{ 175, "lodanim.lez",   0, 0, 1 },
	{ 176, "loadtxt.lez",   0, 1, 1 },
	{ 177, "load.lez",      0, 0, 0 },
	{ 178, "logodsi.lez",   0, 0, 0 },
	{ 179, "proc.lez",      1, 0, 0 },
	{ 180, "video.lez",     1, 0, 0 },
	{ 181, "son.lez",       1, 0, 0 },
	{ 182, "joy.lez",       1, 0, 0 },
	{ 183, "DATADISK.lez",  1, 1, 1 },
	{ 184, "DATTITLE.lez",  1, 1, 1 },
	{ 185, "smallrev.lez",  1, 0, 1 },
	{ 186, "smallpok.lez",  1, 0, 1 },
	{ 187, "maptxt.lez",    1, 1, 1 },
	{ 188, "title.lez",     0, 0, 0 },
	{ 189, "title2.lez",    0, 0, 0 },
	{ 190, "title3.lez",    0, 0, 0 },
	{ 191, "title4.lez",    0, 0, 0 },
	{ 192, "titleGP.lez",   0, 0, 0 },
	{ 193, "title2GP.lez",  0, 0, 0 },
	{ 194, "title3GP.lez",  0, 0, 0 },
	{ 195, "title4GP.lez",  0, 0, 0 },
	{ 196, "session.lez",   1, 1, 1 },
	{ 197, "diagnos.lez",   1, 1, 1 },
	{ 198, "Modpock.lez",   0, 1, 0 },
	{ 199, "Modrever.lez",  0, 1, 0 },
	{ 200, "podium.lez",    0, 1, 0 },
	{ 201, "resetTA.lez",   1, 1, 1 },
	{ 202, "bmglogo.lez",   0, 0, 0 },
	{ 203, "ealogo.lez",    0, 0, 0 },
	{ 204, "jtop.lez",      1, 0, 1 },
	{ 205, "jleft.lez",     1, 0, 1 },
	{ 206, "jright.lez",    1, 0, 1 },
	{ 207, "jbas.lez",      1, 0, 1 },
	{ 208, "Btupdate.lez",  1, 1, 1 }
};

const MENURES_ENTRY *menures_get(int id)
{
	if (id < 0 || id >= MENURES_COUNT)
		return NULL;
	/* Ids are dense and sorted, so the index is the id. Assert that
	 * rather than trusting it silently. */
	if (s_res[id].id != id)
		return NULL;
	return &s_res[id];
}

/*
 * ORIGIN  Sprite_Load @ 0x0043e900, the path build at 0x0043e8f6..0x0043e94b
 *
 * THE SHARED PREFIX IS A LITERAL, NOT A SETTING. The original holds
 * "data\\menu\\bitmap\\" at 0x004e8ca0 and uses it for every resource
 * whose descriptor +0x04 is zero. Ours drops the "data\\", because
 * that is the archive root rather than part of the member name -
 * "MENU\\BITMAP\\LODANIM.LEZ" resolves as member "BITMAP\\LODANIM.LEZ"
 * of MENU.BKF. CLAUDE.md rule 10.
 */
#define MENURES_SHARED_DIR "MENU\\BITMAP\\"

/*
 * THE SIX LANGUAGES, in the order of the pointer table at 0x004e8b60
 * that Sprite_Load indexes with g_language (0x004dbce8) [verified].
 * The identical table at 0x004e4e88 is the same six in the same
 * order, and Lang_GetMenuDir (0x00415a10) bounds the index as
 * 0 <= n < 6, which is where the count of six is pinned.
 *
 * The original's strings are "data\\MenuUs\\" and so on; ours are the
 * archive-relative form - the "data\\" is the archive root, so
 * each entry is the stem of its own .BKF.
 *
 * THE SHIPPED DISC ONLY CARRIES ONE OF THEM. A USA install has
 * MENUUS.BKF and nothing else, so every other index depends on a
 * localised release we do not have - which is exactly why the
 * per-file fallback below is not optional.
 */
static const struct {
	const char *dir;      /* archive-relative prefix       */
	const char *name;     /* the original's own short name */
} s_lang[MENURES_LANG_COUNT] = {
	{ "MENUUS\\", "MenuUs" },   /* 0  English  -us */
	{ "MENUFR\\", "MenuFr" },   /* 1  French   -fr */
	{ "MENUGR\\", "MenuGr" },   /* 2  German   -gr */
	{ "MENUIT\\", "MenuIt" },   /* 3  Italian  -it */
	{ "MENUSP\\", "MenuSp" },   /* 4  Spanish  -sp */
	{ "MENUSW\\", "MenuSw" }    /* 5  Swedish  -sw */
};

const char *menures_lang_name(int lang)
{
	if (lang < 0 || lang >= MENURES_LANG_COUNT)
		return "?";
	return s_lang[lang].name;
}

static int build(const MENURES_ENTRY *e, const char *dir, char *out,
                 int size)
{
	if ((int)(strlen(dir) + strlen(e->name)) >= size)
		return 0;
	strcpy(out, dir);
	strcat(out, e->name);
	return 1;
}

int menures_path(int id, char *out, int size)
{
	const MENURES_ENTRY *e;
	int lang;

	if (!out || size <= 0)
		return 0;
	out[0] = 0;

	e = menures_get(id);
	if (!e)
		return 0;

	if (!e->localised)
		return build(e, MENURES_SHARED_DIR, out, size);

	/* Clamped rather than rejected, as Lang_GetMenuDir bounds it. */
	lang = g_options.language;
	if (lang < 0 || lang >= MENURES_LANG_COUNT)
		lang = 0;

	/*
	 * The result is a LOGICAL ASSET NAME, not a filesystem path -
	 * "MENU\\BITMAP\\LODANIM.LEZ". assets.c splits the archive stem
	 * off the front. See CLAUDE.md rule 10.
	 */
	return build(e, s_lang[lang].dir, out, size);
}

/*
 * ORIGIN  Sprite_Load @ 0x0043e900, at 0x0043e9a1
 *
 * THE LOCALISED FALLBACK IS PER FILE, AND IT IS ALWAYS US. When the
 * localised sprite will not open, the original rebuilds the path
 * against index 0 - it loads PTR_s_data_MenuUs directly at
 * 0x0043e9a5 rather than indexing by g_language - and loads that
 * instead. So a partly translated release degrades ONE SPRITE AT A
 * TIME rather than all or nothing.
 *
 * Returns 0 when there is nothing to fall back to: a shared
 * resource, or a language that is already US.
 */
int menures_path_us(int id, char *out, int size)
{
	const MENURES_ENTRY *e;
	int lang;

	if (!out || size <= 0)
		return 0;
	out[0] = 0;

	e = menures_get(id);
	if (!e || !e->localised)
		return 0;
	lang = g_options.language;
	if (lang <= 0 || lang >= MENURES_LANG_COUNT)
		return 0;

	return build(e, s_lang[0].dir, out, size);
}
