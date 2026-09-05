// ─────────────────────────────────────────────────────────────────────────────
//  msx2_screens.c — the card check screen and the fusion cut-in
// ─────────────────────────────────────────────────────────────────────────────

#include "msx2_screens.h"
#include "msx2_video.h"
#include "msx2_sprite.h"
#include "msx2_stream.h"
#include "msx2_duel.h"
#include "msx2_scenes.h"

static c8 g_name[MSX2_NAME_STRIDE];

static void Msx2_ScreenName(u8 card, u8 y, u8 color)
{
	Msx2_RomRead(MSX2_TEXT_SEGMENT, (u16)card * MSX2_NAME_STRIDE,
	             (u8*)g_name, MSX2_NAME_STRIDE);
	g_name[MSX2_NAME_STRIDE - 1] = 0;
	Msx2_TextColor(color, MSX2_BLACK);
	Msx2_TextCenter(y, g_name);
}

// The big 88x120 art the battle cut-in uses.  Only the 72 monsters have it;
// a support card falls back to its 40x48 hand thumbnail, doubled in place by
// nothing at all -- it is simply drawn at its own size, centred.
static void Msx2_ScreenBigCard(u8 card, u8 x, u8 y)
{
	if(card < MSX2_BATTLE_CARD_COUNT)
	{
		Msx2_StreamRect((u16)(MSX2_BATTLE_CARD_SEGMENT + card), 0, x, y,
		                MSX2_BATTLE_CARD_W, MSX2_BATTLE_CARD_H);
		return;
	}
	Msx2_StreamRect((u16)(MSX2_CARD_ART_SEGMENT + card / MSX2_CARD_ART_PER_SEG),
	                (u16)((card % MSX2_CARD_ART_PER_SEG) * MSX2_CARD_ART_STRIDE),
	                (u8)(x + (MSX2_BATTLE_CARD_W - MSX2_CARD_W) / 2),
	                (u8)(y + (MSX2_BATTLE_CARD_H - MSX2_CARD_H) / 2),
	                MSX2_CARD_W, MSX2_CARD_H);
}

static void Msx2_ScreenSmallCard(u8 card, u8 x, u8 y)
{
	Msx2_StreamRect((u16)(MSX2_CARD_ART_SEGMENT + card / MSX2_CARD_ART_PER_SEG),
	                (u16)((card % MSX2_CARD_ART_PER_SEG) * MSX2_CARD_ART_STRIDE),
	                x, y, MSX2_CARD_W, MSX2_CARD_H);
}

// ── The card check screen ───────────────────────────────────────────────────
//
// Art on the left, everything the card says on the right: its name, its two
// figures, and the sentence describing it.  The sentence is the point -- the
// screen used to be a big picture with a name under it, which the bottom panel
// of the duel already tells you, so there was no reason to leave the board.
//
// The prose lives in the cartridge with the names (MSX2_DESC_OFFSET) and is
// wrapped here rather than offline, because one column width is one number and
// a wrapper is cheaper than eighty bytes a card of pre-broken lines.

#define CHECK_ART_X    14
#define CHECK_ART_Y    44
#define CHECK_COL_X    116          // the right column, and its width in
#define CHECK_COL_COLS 22           // ... six-pixel characters
#define CHECK_NAME_Y   44
#define CHECK_STAT_Y   76
#define CHECK_DESC_Y   102
#define CHECK_LINE_H   11

#define CHECK_COL_MAX  40           // the widest column the line buffer holds

static c8 g_desc[MSX2_DESC_STRIDE];
static c8 g_line[CHECK_COL_MAX + 1];

// One paragraph, broken on spaces into a column `cols` characters wide and
// drawn from `y` down.  The column is a parameter because the effect screen
// below runs the same prose across the whole width instead of beside the art.
static void Msx2_ScreenParagraph(const c8* text, u8 x, u8 cols, u8 y, u8 color)
{
	u8 at = 0;

	if(cols > CHECK_COL_MAX)
		cols = CHECK_COL_MAX;
	Msx2_TextColor(color, MSX2_BLACK);
	while(text[at] != 0)
	{
		u8 take = 0;
		u8 fits = 0;
		u8 i;

		// How much of what is left fits, and where the last space inside it is.
		while((take < cols) && (text[at + take] != 0))
		{
			if(text[at + take] == ' ')
				fits = take;
			++take;
		}
		// Break on that space unless the whole remainder fits, or there is no
		// space at all -- a single word longer than the column is simply cut.
		if((text[at + take] != 0) && (fits != 0))
			take = fits;

		for(i = 0; i < take; ++i)
			g_line[i] = text[at + i];
		g_line[take] = 0;
		Msx2_TextAt(x, y, g_line);
		y = (u8)(y + CHECK_LINE_H);

		at = (u8)(at + take);
		while(text[at] == ' ')
			++at;
	}
}

void Msx2_CardCheckCompose_In(u8 card, i16 atk, i16 def)
{
	u8 page = (u8)(Msx2_VideoGetShowPage() ^ 1);

	Msx2_VideoDisplayBlank();
	Msx2_VideoDrawPage(page);
	Msx2_VideoModeG7();
	Msx2_StreamSceneBlanked(MSX2_SCENE_BATTLE_SEGMENT, page);
	Msx2_VideoDrawPage(page);

	Msx2_TextColor(MSX2_GOLD, MSX2_BLACK);
	Msx2_TextCenter(8, Msx2_UiText(MSX2_S_CARD_CHECK));
	Msx2_ScreenBigCard(card, CHECK_ART_X, CHECK_ART_Y);

	// The name goes through the wrapper too: several of them are longer than
	// the column, and a name cut in half is worse than a name on two lines.
	Msx2_RomRead(MSX2_TEXT_SEGMENT, (u16)card * MSX2_NAME_STRIDE,
	             (u8*)g_name, MSX2_NAME_STRIDE);
	g_name[MSX2_NAME_STRIDE - 1] = 0;
	Msx2_ScreenParagraph(g_name, CHECK_COL_X, CHECK_COL_COLS,
	                     CHECK_NAME_Y, MSX2_WHITE);

	if(Msx2_IsMonster(card))
	{
		Msx2_TextColor(MSX2_GOLD, MSX2_BLACK);
		Msx2_TextAt(CHECK_COL_X, CHECK_STAT_Y, "ATK");
		Msx2_TextAt((u8)(CHECK_COL_X + 72), CHECK_STAT_Y, "DEF");
		Msx2_TextColor(MSX2_WHITE, MSX2_BLACK);
		Msx2_NumAt((u8)(CHECK_COL_X + 26), CHECK_STAT_Y, atk);
		Msx2_NumAt((u8)(CHECK_COL_X + 98), CHECK_STAT_Y, def);
	}
	else
	{
		Msx2_TextColor(MSX2_TEAL, MSX2_BLACK);
		Msx2_TextAt(CHECK_COL_X, CHECK_STAT_Y, Msx2_UiText(MSX2_S_SUPPORT_CARD));
	}

	Msx2_RomRead(MSX2_TEXT_SEGMENT,
	             (u16)(MSX2_DESC_OFFSET + (u16)card * MSX2_DESC_STRIDE),
	             (u8*)g_desc, MSX2_DESC_STRIDE);
	g_desc[MSX2_DESC_STRIDE - 1] = 0;
	Msx2_ScreenParagraph(g_desc, CHECK_COL_X, CHECK_COL_COLS,
	                     CHECK_DESC_Y, MSX2_SAND);

	Msx2_TextColor(MSX2_SAND, MSX2_BLACK);
	Msx2_TextCenter(196, Msx2_UiText(MSX2_S_SPACE_RETURNS_TO_THE_DUEL));

	Msx2_VideoCopyPage(page, (u8)(page ^ 1));
	Msx2_VideoShowPage(page);
	Msx2_VideoDisplayRestore();
}

// ── The fusion cut-in ───────────────────────────────────────────────────────
//
// Three beats, and the middle one is why the sprite layer exists.  The
// materials stand on the black stage; a burst opens over them, eight frames of
// it, and takes them away one at a time; what is left standing is the monster
// they became, at the size the battle cut-in shows a card.  None of the burst
// touches the bitmap, so none of it has to be repaired on two pages.

#define FUSE_MAT_Y     52
#define FUSE_BURST_N   MSX2_SPR_BURST_N
#define FUSE_HOLD      64

static u8 g_fuse_mat[MSX2_FUSION_MATS];
static u8 g_fuse_n;
static u8 g_fuse_result;
static u8 g_fuse_phase;
static u8 g_fuse_step;

// Where material i stands, for up to three of them.
static u8 Msx2_FuseMatX(u8 i)
{
	u8 span = (u8)(g_fuse_n * (MSX2_CARD_W + 14) - 14);
	return (u8)((MSX2_SCREEN_W - span) / 2 + i * (MSX2_CARD_W + 14));
}

void Msx2_FusionBegin_In(const u8* materials, u8 count, u8 result, u8 fused)
{
	u8 page = (u8)(Msx2_VideoGetShowPage() ^ 1);
	u8 i;

	g_fuse_n = (count > MSX2_FUSION_MATS) ? MSX2_FUSION_MATS : count;
	for(i = 0; i < g_fuse_n; ++i)
		g_fuse_mat[i] = materials[i];
	g_fuse_result = result;
	g_fuse_phase = 0;
	g_fuse_step = 0;

	Msx2_VideoDisplayBlank();
	Msx2_VideoDrawPage(page);
	Msx2_VideoModeG7();
	Msx2_StreamSceneBlanked(MSX2_SCENE_BATTLE_SEGMENT, page);
	Msx2_VideoDrawPage(page);
	Msx2_TextColor(MSX2_TEAL, MSX2_BLACK);
	// SAY WHICH OF THE THREE THINGS JUST HAPPENED, the way the other targets
	// do: a recipe fired, or the chain simply spent its materials and left the
	// last card standing, or there was no card left to stand at all.
	Msx2_TextCenter(20, (result == MSX2_CARD_NONE)
	                        ? "CARDS DISCARDED"
	                        : Msx2_UiText(fused ? MSX2_S_FUSION_SUMMON
	                                            : MSX2_S_FUSION_FAILED));
	for(i = 0; i < g_fuse_n; ++i)
		Msx2_ScreenSmallCard(g_fuse_mat[i], Msx2_FuseMatX(i), FUSE_MAT_Y);
	Msx2_VideoCopyPage(page, (u8)(page ^ 1));
	Msx2_VideoShowPage(page);
	Msx2_VideoDisplayRestore();
}

bool Msx2_FusionStep_In(void)
{
	u8 page = (u8)(Msx2_VideoGetShowPage() ^ 1);

	if(g_fuse_phase == 0)
	{
		// The burst, over the materials.  Three sprites a frame apart make one
		// blast rather than three, and the middle of the row is where the
		// result is about to stand.
		u8 cx = (u8)(MSX2_SCREEN_W / 2);
		u8 cy = (u8)(FUSE_MAT_Y + MSX2_CARD_H / 2);
		Msx2_SpriteAt(0, (u8)(cx - 16), (u8)(cy - 16),
		              (u8)(MSX2_SPR_BURST0 + g_fuse_step), MSX2_SPR_WHITE);
		if(g_fuse_step > 0)
			Msx2_SpriteAt(1, (u8)(cx - 46), (u8)(cy - 8),
			              (u8)(MSX2_SPR_BURST0 + g_fuse_step - 1), MSX2_SPR_GOLD);
		if(g_fuse_step > 1)
			Msx2_SpriteAt(2, (u8)(cx + 14), (u8)(cy - 6),
			              (u8)(MSX2_SPR_BURST0 + g_fuse_step - 2), MSX2_SPR_TEAL);

		// One material leaves the stage per burst frame, on both pages, so the
		// flip cannot bring a taken card back.
		if((g_fuse_step < g_fuse_n) && (g_fuse_step < FUSE_BURST_N))
		{
			u8 p;
			for(p = 0; p < MSX2_VIDEO_PAGES; ++p)
			{
				Msx2_VideoDrawPage(p);
				Msx2_Fill(Msx2_FuseMatX(g_fuse_step), FUSE_MAT_Y, MSX2_CARD_W,
				          MSX2_CARD_H, MSX2_BLACK);
			}
			Msx2_VideoDrawPage(page);
		}

		if(++g_fuse_step >= FUSE_BURST_N)
		{
			u8 p;
			// The monster, on both pages under the last of the burst.
			for(p = 0; p < MSX2_VIDEO_PAGES; ++p)
			{
				Msx2_VideoDrawPage(p);
				Msx2_Fill(0, 40, MSX2_SCREEN_W, 120, MSX2_BLACK);
				// A discarded support-only chain has no result art or name.
				if(g_fuse_result == MSX2_CARD_NONE)
					continue;
				Msx2_ScreenBigCard(g_fuse_result,
				                   (u8)((MSX2_SCREEN_W - MSX2_BATTLE_CARD_W) / 2),
				                   38);
				Msx2_ScreenName(g_fuse_result, 166, MSX2_GOLD);
				Msx2_TextColor(MSX2_GOLD, MSX2_BLACK);
				Msx2_TextAt(56, 182, "ATK");
				Msx2_TextAt(140, 182, "DEF");
				Msx2_TextColor(MSX2_WHITE, MSX2_BLACK);
				Msx2_NumAt(82, 182, (i16)Msx2_CardAtk(g_fuse_result));
				Msx2_NumAt(166, 182, (i16)Msx2_CardDef(g_fuse_result));
			}
			Msx2_VideoDrawPage(page);
			g_fuse_phase = 1;
			g_fuse_step = 0;
		}
		Msx2_VideoFlipRequest();
		return TRUE;
	}

	// The hold.  The burst fades away over the first frames of it, one sprite
	// at a time, and then the monster stands alone until the beat is over.
	if(g_fuse_step == 0)
		Msx2_SpriteHide(2);
	else if(g_fuse_step == 4)
		Msx2_SpriteHide(1);
	else if(g_fuse_step == 8)
		Msx2_SpriteHide(0);
	if(++g_fuse_step < FUSE_HOLD)
		return TRUE;

	Msx2_SpriteClear();
	return FALSE;
}


// ── The effect cut-in ───────────────────────────────────────────────────────
//
// A support card that needs no target -- draw, heal, thunder, a trap -- used to
// resolve as a word in the bottom panel and a rectangle flashing on the board,
// which says nothing about what the card just did.  It is a beat of its own
// now: the card alone on the black stage at the size the battle cut-in shows
// one, its name under it, and the sentence out of the cartridge under that.
//
// The old version also left "EFFECT" written into the arena.  It was drawn
// opaquely at y=82 and only ever "erased" by replaying the XOR frame beside it,
// so the word stayed on the board for the rest of the duel.  Nothing here
// touches the board at all: the whole screen is composed on the hidden page and
// the duel's own Msx2_BoardRestoreFromCutin() streams the arena back.

#define EFFECT_CARD_Y  30
#define EFFECT_NAME_Y  156
#define EFFECT_DESC_Y  174
#define EFFECT_COLS    38
#define EFFECT_HOLD    96

static u8 g_effect_left;

void Msx2_EffectBegin_In(u8 card, u8 by_com)
{
	u8 page = (u8)(Msx2_VideoGetShowPage() ^ 1);

	g_effect_left = EFFECT_HOLD;

	Msx2_VideoDisplayBlank();
	Msx2_VideoDrawPage(page);
	Msx2_VideoModeG7();
	Msx2_StreamSceneBlanked(MSX2_SCENE_BATTLE_SEGMENT, page);
	Msx2_VideoDrawPage(page);

	Msx2_TextColor(by_com ? MSX2_RED : MSX2_TEAL, MSX2_BLACK);
	Msx2_TextCenter(12, Msx2_UiText(by_com ? MSX2_S_OPPONENT_SUPPORT_ACTIVATED
	                                       : MSX2_S_SUPPORT_ACTIVATED));
	Msx2_ScreenBigCard(card, (u8)((MSX2_SCREEN_W - MSX2_BATTLE_CARD_W) / 2),
	                   EFFECT_CARD_Y);
	Msx2_ScreenName(card, EFFECT_NAME_Y, MSX2_GOLD);

	Msx2_RomRead(MSX2_TEXT_SEGMENT,
	             (u16)(MSX2_DESC_OFFSET + (u16)card * MSX2_DESC_STRIDE),
	             (u8*)g_desc, MSX2_DESC_STRIDE);
	g_desc[MSX2_DESC_STRIDE - 1] = 0;
	Msx2_ScreenParagraph(g_desc,
	                     (u8)((MSX2_SCREEN_W - EFFECT_COLS * MSX2_FONT_W_PX) / 2),
	                     EFFECT_COLS, EFFECT_DESC_Y, MSX2_SAND);

	Msx2_VideoCopyPage(page, (u8)(page ^ 1));
	Msx2_VideoShowPage(page);
	Msx2_VideoDisplayRestore();
}

bool Msx2_EffectStep_In(void)
{
	// Both pages already hold the finished picture, so the hold is a countdown
	// and nothing else -- no repaint, no flip, no work for the VDP at all.
	if(g_effect_left == 0)
		return FALSE;
	--g_effect_left;
	return TRUE;
}
