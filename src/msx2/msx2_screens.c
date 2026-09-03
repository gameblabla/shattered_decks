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

void Msx2_CardCheckCompose(u8 card, i16 atk, i16 def)
{
	u8 page = (u8)(Msx2_VideoGetShowPage() ^ 1);

	VDP_EnableDisplay(FALSE);
	Msx2_VideoDrawPage(page);
	Msx2_StreamSceneBlanked(MSX2_SCENE_BATTLE_SEGMENT, page);
	Msx2_VideoDrawPage(page);

	Msx2_TextColor(MSX2_GOLD, MSX2_BLACK);
	Msx2_TextCenter(6, Msx2_UiText(MSX2_S_CARD_CHECK));
	Msx2_ScreenBigCard(card, (u8)((MSX2_SCREEN_W - MSX2_BATTLE_CARD_W) / 2), 26);
	Msx2_ScreenName(card, 156, MSX2_WHITE);

	if(Msx2_IsMonster(card))
	{
		Msx2_TextColor(MSX2_GOLD, MSX2_BLACK);
		Msx2_TextAt(56, 172, "ATK");
		Msx2_TextAt(140, 172, "DEF");
		Msx2_TextColor(MSX2_WHITE, MSX2_BLACK);
		Msx2_NumAt(82, 172, atk);
		Msx2_NumAt(166, 172, def);
	}
	else
	{
		Msx2_TextColor(MSX2_TEAL, MSX2_BLACK);
		Msx2_TextCenter(172, Msx2_UiText(MSX2_S_SUPPORT_CARD));
	}

	Msx2_TextColor(MSX2_SAND, MSX2_BLACK);
	Msx2_TextCenter(194, Msx2_UiText(MSX2_S_SPACE_RETURNS_TO_THE_DUEL));

	Msx2_VideoCopyPage(page, (u8)(page ^ 1));
	Msx2_VideoShowPage(page);
	VDP_EnableDisplay(TRUE);
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

void Msx2_FusionBegin(const u8* materials, u8 count, u8 result)
{
	u8 page = (u8)(Msx2_VideoGetShowPage() ^ 1);
	u8 i;

	g_fuse_n = (count > MSX2_FUSION_MATS) ? MSX2_FUSION_MATS : count;
	for(i = 0; i < g_fuse_n; ++i)
		g_fuse_mat[i] = materials[i];
	g_fuse_result = result;
	g_fuse_phase = 0;
	g_fuse_step = 0;

	VDP_EnableDisplay(FALSE);
	Msx2_VideoDrawPage(page);
	Msx2_StreamSceneBlanked(MSX2_SCENE_BATTLE_SEGMENT, page);
	Msx2_VideoDrawPage(page);
	Msx2_TextColor(MSX2_TEAL, MSX2_BLACK);
	Msx2_TextCenter(20, Msx2_UiText(MSX2_S_FUSION_SUMMON));
	for(i = 0; i < g_fuse_n; ++i)
		Msx2_ScreenSmallCard(g_fuse_mat[i], Msx2_FuseMatX(i), FUSE_MAT_Y);
	Msx2_VideoCopyPage(page, (u8)(page ^ 1));
	Msx2_VideoShowPage(page);
	VDP_EnableDisplay(TRUE);
}

bool Msx2_FusionStep(void)
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
