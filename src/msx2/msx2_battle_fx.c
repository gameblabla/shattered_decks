// Generic full-screen battle-cut-in primitives.  They live in the ordinary
// resident code area rather than the nearly-full page-0 bank; all cartridge
// reads have completed before these VDP-only routines run.

#include "msx2_battle_fx.h"
#include "msx2_video.h"
#include "msx2_duel.h"
#include "msx2_scenes.h"
#include "msx2_stream.h"
#include "msx2_sprite.h"

void Msx2_BattleFxImpact(u8 x, bool trap)
{
	u8 y = 83;
	Msx2_Line((u8)(x - 24), y, (u8)(x + 24), y, MSX2_WHITE);
	Msx2_Line(x, (u8)(y - 24), x, (u8)(y + 24), MSX2_WHITE);
	Msx2_Line((u8)(x - 17), (u8)(y - 17),
	          (u8)(x + 17), (u8)(y + 17), MSX2_GOLD);
	Msx2_Line((u8)(x + 17), (u8)(y - 17),
	          (u8)(x - 17), (u8)(y + 17), MSX2_GOLD);
	Msx2_FrameRect((u8)(x - 10), (u8)(y - 10), 21, 21,
	               trap ? MSX2_RED : MSX2_WHITE);
}

// THE IMPACT, AS SPRITES.
// The bitmap starburst above is one pose held for six flips, because putting
// eight of them into the page would mean saving and restoring the arena under
// each one, twice.  The V9938 draws sprites over both pages from an absolute
// table, so the same explosion animated costs one attribute write a frame --
// which is the whole reason the sprite layer exists.  Three of them, a frame
// apart and in three colours, read as one blast rather than three.
void Msx2_BattleFxBurst(u8 x, u8 y, u8 step)
{
	if(step >= MSX2_SPR_BURST_N)
	{
		Msx2_SpriteHide(0);
		Msx2_SpriteHide(1);
		Msx2_SpriteHide(2);
		return;
	}
	Msx2_SpriteAt(0, (u8)(x - 16), (u8)(y - 16),
	              (u8)(MSX2_SPR_BURST0 + step), MSX2_SPR_WHITE);
	if(step > 0)
		Msx2_SpriteAt(1, (u8)(x - 44), (u8)(y - 4),
		              (u8)(MSX2_SPR_BURST0 + step - 1), MSX2_SPR_GOLD);
	if(step > 1)
		Msx2_SpriteAt(2, (u8)(x + 12), (u8)(y + 2),
		              (u8)(MSX2_SPR_BURST0 + step - 2), MSX2_SPR_RED);
}

void Msx2_BattleFxResult(bool trap)
{
	Msx2_Fill(0, 181, MSX2_SCREEN_W, 31, MSX2_BLACK);
	Msx2_TextColor(trap ? MSX2_RED : MSX2_GOLD, MSX2_BLACK);
	if(trap)
		Msx2_TextCenter(190, Msx2_UiText(MSX2_S_ATTACKER_DESTROYED));
	else if(g_duel.last_battle.damage > 0)
	{
		Msx2_TextAt(82, 190, Msx2_UiText(MSX2_S_DAMAGE));
		Msx2_NumAt(132, 190, g_duel.last_battle.damage);
	}
	else
		Msx2_TextCenter(190, Msx2_UiText(MSX2_S_NO_BATTLE_DAMAGE));
}

void Msx2_BattleFxBurnCard(u8 x, u8 h, u8 step)
{
	Msx2_Fill(x, 23, MSX2_BATTLE_CARD_W, h, MSX2_BLACK);
	if(step < MSX2_BATTLE_BURN_STEPS)
		Msx2_Line(x, (u8)(23 + h), (u8)(x + MSX2_BATTLE_CARD_W - 1),
		          (u8)(23 + h), (step & 1) ? MSX2_GOLD : MSX2_RED);
	else
		Msx2_Fill(x, 149, MSX2_BATTLE_CARD_W, 8, MSX2_BLACK);
}
