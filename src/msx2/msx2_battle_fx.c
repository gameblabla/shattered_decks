// Generic full-screen battle-cut-in primitives.  They live in the ordinary
// resident code area rather than the nearly-full page-0 bank; all cartridge
// reads have completed before these VDP-only routines run.

#include "msx2_battle_fx.h"
#include "msx2_video.h"
#include "msx2_duel.h"
#include "msx2_scenes.h"
#include "msx2_stream.h"
#include "msx2_sprite.h"

// THE IMPACT, AS SPRITES.
// There used to be a bitmap starburst under this, one pose held for six flips,
// because putting eight of them into the page would have meant saving and
// restoring the arena under each one, twice.  Nothing in the strike is bitmap
// any more, so it is gone.  The V9938 draws sprites over both pages from an
// absolute table, so an animated explosion costs one attribute write a frame --
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

// THE BLADE SWEEP, AS SPRITES.
//
// It was seven V9938 LINE commands a pose drawn into the bitmap, and the pose
// before it was still there -- so every frame began by levelling the hidden page
// from the other one, a 256x240 HMMM.  Sixty thousand pixels a frame for sixteen
// frames is what made an attack crawl, and levelling from a page one pose behind
// is what made it flicker.  The figure is the same; the layer is not.  Sprites
// float over both pages from an absolute table, so nothing under the cut is ever
// saved, restored, or copied, and a pose is twenty bytes a segment.
//
// The animation is which segments are up.  Stroke A opens the cut from the
// middle out, stroke B crosses it, and both then withdraw from the ends inwards
// while the sprite explosion takes the crossing point -- so the blade is gone by
// the time the burst is at its widest, rather than sitting under it.
void Msx2_BattleFxSlash(u8 x, u8 step)
{
	u8 a, b, core;

	if(step < MSX2_BATTLE_SLASH_STEPS)
	{
		// Opening.  Two segments of the first stroke, then all four, then the
		// second stroke the same way; from there both are whole.
		a = (step >= 1) ? MSX2_SPR_SLASH_SEGS : 2;
		b = (step >= 3) ? MSX2_SPR_SLASH_SEGS : (step >= 2) ? 2 : 0;
	}
	else
	{
		// Withdrawing, a segment every two frames, never quite to nothing until
		// the caller takes it off.
		u8 gone = (u8)((step - MSX2_BATTLE_SLASH_STEPS) >> 1);
		a = (gone >= MSX2_SPR_SLASH_SEGS - 1)
		  ? 1 : (u8)(MSX2_SPR_SLASH_SEGS - gone);
		b = a;
	}
	core = (step & 1) ? MSX2_SPR_WHITE : MSX2_SPR_ORANGE;
	Msx2_SpriteSlash(x, 83, a, b, core);
}

// THE DAMAGE READOUT, COUNTING.
//
// PC punches the figure over the dying rays in one piece; on an 8-bit screen a
// number that arrives whole reads as a caption, and a number that CLIMBS reads
// as a hit.  So it is drawn every pose of the sweep and the burst, from -0 up
// to the real figure, on a plate of its own under the struck lane: the digit
// count shrinks as well as grows on the way (-1000 to -900), and without the
// plate the wider figure would leave its tail behind.
#define MSX2_BATTLE_DMG_W  110
#define MSX2_BATTLE_DMG_Y  160

void Msx2_BattleFxDamageCount(u8 x, i16 value)
{
	// Six pixels a glyph, and one more glyph for the minus sign.
	u8 digits = 1;
	i16 v = value;
	u8 w;

	while(v >= 10) { v /= 10; ++digits; }
	w = (u8)((digits + 1) * MSX2_FONT_W_PX);

	Msx2_Fill((u8)(x - MSX2_BATTLE_DMG_W / 2), MSX2_BATTLE_DMG_Y,
	          MSX2_BATTLE_DMG_W, 10, MSX2_BLACK);
	Msx2_TextColor(MSX2_GOLD, MSX2_BLACK);
	Msx2_NumAt((u8)(x - (w >> 1)), (u8)(MSX2_BATTLE_DMG_Y + 1), (i16)(-value));
}
