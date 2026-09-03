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

// THE BLADE SWEEP.
//
// The PC build's draw_direct_attack_slash() is a thick diagonal stroke through
// the struck card, a second stroke across it, and a hot burst on its far side,
// all in the white -> gold -> orange -> flame -> red ramp.  This is the same
// figure in seven V9938 LINE commands a pose: the port has no framebuffer to
// draw a blade into, and a line is the one primitive the command engine draws
// as fast as it fills.
//
// It only grows.  Every pose is drawn onto a page that has just been levelled
// from the other one, so the previous pose is already there and nothing has to
// be taken back off -- which is what lets the whole sweep run without a single
// save-and-restore of the stage under it.
void Msx2_BattleFxSlash(u8 x, u8 step)
{
	u8 y = 83;
	u8 len = (u8)(18 + step * 6);
	u8 i;

	if(len > 56) len = 56;
	// The core stroke, seven pixels thick, hottest in the middle.  It runs
	// upper-left to lower-right because the attacker is always in the left
	// lane and what it is cutting is always in the right one.
	for(i = 0; i < 7; ++i)
	{
		u8 c = (i == 3) ? ((step & 1) ? MSX2_WHITE : MSX2_ORANGE)
		     : ((i >= 2) && (i <= 4)) ? MSX2_ORANGE
		     : ((i == 1) || (i == 5)) ? MSX2_FLAME : MSX2_RED;
		Msx2_Line((u8)(x - (len >> 1) + i - 3), (u8)(y - len),
		          (u8)(x + (len >> 1) + i - 3), (u8)(y + len), c);
	}
	if(step >= 2)
	{
		// The counter-stroke, the other way across.  One cut is a scratch; two
		// crossing is a strike.  It is given the same steep angle as the first
		// so the two read as blades rather than as a grid.
		u8 h = (u8)(len - (len >> 2));
		for(i = 0; i < 5; ++i)
		{
			u8 c = (i == 2) ? MSX2_ORANGE
			     : ((i == 1) || (i == 3)) ? MSX2_FLAME : MSX2_RED;
			Msx2_Line((u8)(x + (h >> 1) + i - 2), (u8)(y - h),
			          (u8)(x - (h >> 1) + i - 2), (u8)(y + h), c);
		}
	}
	if(step >= 4)
	{
		// And the sparks thrown off the crossing point, opening as the sweep
		// finishes.  The sprite explosion lands on the same point on the next
		// beat, so these are what carry the eye into it.
		u8 r = (u8)(14 + (step - 4) * 6);
		u8 q = (u8)(r >> 1);
		Msx2_Line((u8)(x - r), (u8)(y - q), (u8)(x + r), (u8)(y + q),
		          MSX2_ORANGE);
		Msx2_Line((u8)(x - r), (u8)(y + q), (u8)(x + r), (u8)(y - q),
		          MSX2_ORANGE);
		Msx2_Line((u8)(x - r), y, (u8)(x + r), y, MSX2_FLAME);
		Msx2_Line(x, (u8)(y - r), x, (u8)(y + r), MSX2_FLAME);
	}
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
