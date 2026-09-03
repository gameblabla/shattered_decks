// ─────────────────────────────────────────────────────────────────────────────
//  msx2_board.c — the duel screen
// ─────────────────────────────────────────────────────────────────────────────

#include "msx2_board.h"
#include "msx2_video.h"
#include "msx2_input.h"
#include "msx2_stream.h"
#include "msx2_audio.h"
#include "msx2_duel.h"
#include "msx2_cards.h"
#include "msx2_raster.h"
#include "msx2_scenes.h"
#include "msx2_battle_fx.h"
#include "msx2_sprite.h"
#include "msx2_screens.h"

// ── Geometry ─────────────────────────────────────────────────────────────────
//
// Slots 0..9 are the field, in the order the capture emitted them: 0-4 the COM
// row, 5-9 the player's.  Slots 10..14 are the player's hand, which is a flat
// HUD strip rather than board geometry.  The field quads and their boxes come
// out of the capture (src/generated/msx2_scenes.h) -- there is no second
// definition of where a card goes, which is the whole point of §4.3.

#define ZONE_COM     0
#define ZONE_FIELD   1
#define ZONE_HAND    2
#define FIELD_ROW    (MSX2_FIELD_SLOTS / 2)
#define SLOT_COUNT   (MSX2_FIELD_SLOTS + MSX2_HAND_SLOTS)

#define SLOT_OF(z, i)     (u8)((u8)(z) * FIELD_ROW + (u8)(i))
#define SLOT_ZONE(s)      (u8)((s) / FIELD_ROW)
#define SLOT_INDEX(s)     (u8)((s) % FIELD_ROW)
#define IS_HAND(s)        ((s) >= MSX2_FIELD_SLOTS)

#define HAND_X(i)         (u8)(MSX2_HAND_X0 + (u8)(i) * MSX2_HAND_PITCH)

// Slot flags, tracked alongside the card id so a repaint knows a face-down card,
// a face-up one and the third card in a fusion chain are different pictures.
// The queue position lives in the high nibble, 0 meaning "not queued".
#define F_FACEUP     0x01
#define F_DEFENSE    0x02
#define F_QUEUE(n)   (u8)((n) << 4)
#define F_QUEUE_OF(f) (u8)((f) >> 4)

// ── Interaction ──────────────────────────────────────────────────────────────
#define M_IDLE       0   // walking the board, nothing chosen
#define M_PLACE      1   // a monster is chosen, picking the field slot
#define M_EQUIP      2   // an equip is chosen, picking the monster
#define M_TARGET     3   // an attacker is chosen, picking the defender
#define M_COM        4   // the COM turn, one rules step at a time
#define M_OVER       5   // the duel is decided
#define M_TURN       6   // the table is swinging to the other player's chair
#define M_OPENING    7   // the shared opening camera path is being streamed
#define M_DEAL       8   // the UI has just appeared and the hand is flying in
#define M_CHECK      9   // the card check screen is up over the duel

#define BOARD_VIEW_PLAYER  MSX2_VIEW_TOP
#define BOARD_VIEW_COM     MSX2_VIEW_COM

static u8  g_stage;
static u8  g_mode;
static u8  g_view;
static u8  g_zone;
static u8  g_sel;
static u8  g_hand_pick;
static u8  g_atk_pick;
static u8  g_place_def;
static u8  g_move_pose;
static u8  g_move_target;
static u8  g_move_forward;

// The opening deal.  g_deal_slot is the hand position currently in flight,
// left to right so a card never crosses one that has already landed;
// g_deal_reveal is how many of them the retained painter is allowed to see.
#define DEAL_STEPS   6
#define DEAL_START_X 216
static u8  g_deal_slot;
static u8  g_deal_step;
static u8  g_deal_reveal;
static u8  g_deal_px[MSX2_VIDEO_PAGES];

// The fusion chain the player is building, in the order they chose it -- the
// order is part of the rule, because the materials fold left to right.
static u8  g_queue[MSX2_HAND];
static u8  g_queue_n;

// The cards a fusion consumed, kept because the cut-in shows them and the
// rules have already taken them out of the hand by the time it runs.
static u8  g_fuse_mat[MSX2_FUSION_MATS];
static u8  g_fuse_mat_n;
// Frames left on the "those two do not fuse" line.  A refused chain used to be
// completely silent, which reads as the button not working.
static u8  g_refuse;

// What the board should look like, and what each page is actually showing.
static u8  g_want[SLOT_COUNT];
static u8  g_flag[SLOT_COUNT];
static u8  g_shown[MSX2_VIDEO_PAGES][SLOT_COUNT];
static u8  g_shown_flag[MSX2_VIDEO_PAGES][SLOT_COUNT];
static u8  g_cursor_at[MSX2_VIDEO_PAGES];
static u8  g_cursor_col_at[MSX2_VIDEO_PAGES];
static u8  g_panel_left;         // pages still owing a HUD/info repaint
static u8  g_hand_left;          // pages still owing the whole hand strip

static c8  g_name[MSX2_NAME_STRIDE];

// ── Presentation events ─────────────────────────────────────────────────────
// Rules are committed immediately, but the screen holds the result back for a
// few frames so a player can read what happened.  The effect is painted on the
// hidden page; the other page is clean, so alternating flips erase it without a
// framebuffer or a second copy of the arena.
#define FX_NONE       0
#define FX_SUMMON     1
#define FX_FUSION     2
#define FX_EQUIP      3
#define FX_SUPPORT    4
#define FX_ATTACK     5
#define FX_POSITION   6
#define FX_COM_CHOOSE 7
#define FX_COM_PLACE  8
#define FX_COM_ATTACK 9
#define FX_FRAME_NONE 0xFFu

static u8 g_fx_kind;
static u8 g_fx_followup;
static u8 g_fx_frames;
static u8 g_fx_cleanup;
static u8 g_fx_card;
static u8 g_fx_hand;
static u8 g_fx_field;
static u8 g_fx_owner;
static u8 g_suppress_slot;
static u8 g_fx_page_frame[MSX2_VIDEO_PAGES];

// A card landing on the field takes the hand off the screen, slides the real
// 2-D thumbnail across the emptied band, bends the board under it and then
// holds the bare top view while the new card is read.  These carry that.
#define FX_BEND_POSES  1
// Every landing flight is the same length, so one easing curve serves them all.
#define FX_LANDING_FRAMES 14
#define FX_HOLD_FRAMES 24
static u8 g_hand_hidden;
static u8 g_fx_bend;
static u8 g_fx_hold;
static u8 g_fx_dest_x;
static u8 g_fx_dest_y;

// The 2-D battle cut-in.  The cards do not just sit on the black stage and
// flash at each other: the attacker closes the gap in visible steps, the
// contact beat lands where they meet, and the result is held long enough to
// read.  A cut-in card is 88x120, which is six frames of streaming, so the
// lunge is five long steps rather than a smooth slide.  The same steps then
// run backwards after contact: leaving the attacker parked in the middle of
// the cut-in made the result appear to teleport it back onto the 3-D field.
#define BATT_STEPS   5
#define BATT_DX      8
#define BATT_BURN_DY 20
#define BATT_HOLD    56
static u8 g_batt_phase;
static u8 g_batt_step;
static u8 g_batt_ax;
static u8 g_batt_dx;
static u8 g_batt_dir;
static u8 g_batt_direct;
static u8 g_batt_trap;
static u8 g_batt_counter;
static u8 g_batt_px[MSX2_VIDEO_PAGES];
static u8 g_batt_dpx[MSX2_VIDEO_PAGES];

// ─────────────────────────────────────────────────────────────────────────────
//  Reading the rules
// ─────────────────────────────────────────────────────────────────────────────

u8 Msx2_BoardStageForStory_In(u8 story_duel_index)
{
	// story_scene_for_progress() in src/main.c, by another name.
	if(story_duel_index == MSX2_STORY_NONE)
		return 0;
	if(story_duel_index >= 4) return 3;
	if(story_duel_index >= 3) return 2;
	if(story_duel_index >= 2) return 1;
	return 0;
}

// Where a hand slot sits in the fusion chain, 1-based, or 0 if it is not in it.
static u8 Msx2_BoardQueueOrder(u8 hand_slot)
{
	u8 i;
	for(i = 0; i < g_queue_n; ++i)
		if(g_queue[i] == hand_slot)
			return (u8)(i + 1);
	return 0;
}

static void Msx2_BoardQueueToggle(u8 hand_slot)
{
	u8 order = Msx2_BoardQueueOrder(hand_slot);
	u8 i;

	if(order != 0)
	{
		// Taking a card out closes the gap, so the remaining order is still
		// the order the player chose.
		for(i = (u8)(order - 1); i + 1 < g_queue_n; ++i)
			g_queue[i] = g_queue[i + 1];
		--g_queue_n;
	}
	else if((g_queue_n < MSX2_HAND) &&
	        (g_duel.side[MSX2_OWNER_PLAYER].hand[hand_slot] != MSX2_CARD_NONE))
	{
		g_queue[g_queue_n++] = hand_slot;
	}
}

// Rebuild the wanted picture from the duel state.  Called after every action;
// the paint pass works out what that actually costs.
static void Msx2_BoardSnapshot(void)
{
	u8 i;
	u8 hand_owner = (g_view == BOARD_VIEW_COM) ? MSX2_OWNER_COM : MSX2_OWNER_PLAYER;
	for(i = 0; i < FIELD_ROW; ++i)
	{
		const Msx2Side* com = &g_duel.side[MSX2_OWNER_COM];
		const Msx2Side* you = &g_duel.side[MSX2_OWNER_PLAYER];

		g_want[SLOT_OF(ZONE_COM, i)] = com->field[i];
		g_flag[SLOT_OF(ZONE_COM, i)] = (u8)((com->faceup[i] ? F_FACEUP : 0)
		                                  | (com->defense[i] ? F_DEFENSE : 0));

		g_want[SLOT_OF(ZONE_FIELD, i)] = you->field[i];
		g_flag[SLOT_OF(ZONE_FIELD, i)] = (u8)(F_FACEUP
		                                  | (you->defense[i] ? F_DEFENSE : 0));

		// A hand position the opening deal has not delivered yet is empty as
		// far as the retained painter is concerned.
		g_want[SLOT_OF(ZONE_HAND, i)] = ((i < g_deal_reveal) && !g_hand_hidden)
		       ? g_duel.side[hand_owner].hand[i] : MSX2_CARD_NONE;
		// The COM chair is a presentation view, not permission to look at the
		// opponent's hand.  Keep the real id in the model for placement, but draw
		// the common cover and expose no metadata to the player.
		g_flag[SLOT_OF(ZONE_HAND, i)] = (hand_owner == MSX2_OWNER_PLAYER)
		       ? (u8)(F_FACEUP | F_QUEUE(Msx2_BoardQueueOrder(i))) : 0;
	}
	if(g_suppress_slot != MSX2_SLOT_NONE)
	{
		g_want[g_suppress_slot] = MSX2_CARD_NONE;
		g_flag[g_suppress_slot] = 0;
	}
}

// The card the cursor is over, for the info panel.
static u8 Msx2_BoardHovered(void)
{
	u8 slot = SLOT_OF(g_zone, g_sel);
	if((g_zone == ZONE_HAND) && (g_view == BOARD_VIEW_COM))
		return MSX2_CARD_NONE;
	if((g_zone == ZONE_COM) && !(g_flag[slot] & F_FACEUP))
		return MSX2_CARD_NONE;          // a set monster gives nothing away
	return g_want[slot];
}

static u8 Msx2_BoardFieldSlot(u8 owner, u8 field_slot)
{
	return SLOT_OF((owner == MSX2_OWNER_COM) ? ZONE_COM : ZONE_FIELD, field_slot);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Drawing
// ─────────────────────────────────────────────────────────────────────────────

// One card, drawn where the arena says it goes.
//
// A field slot is a projected quad, so the card is mapped into it by the §8
// span rasterizer -- that is the whole of §0.3.1 in one call.  A hand slot is a
// flat strip, so it is an ordinary rectangle straight out of the cartridge,
// which also keeps the cards a player is choosing between at a readable size.
static void Msx2_BoardBlitSlot(u8 slot)
{
	u8  card = g_want[slot];
	u8  index;
	u16 tile;

	if(IS_HAND(slot))
	{
		u8 x = HAND_X(slot - MSX2_FIELD_SLOTS);

		if(card == MSX2_CARD_NONE)
		{
			// An empty hand position keeps its baked frame: the band under it is
			// black, so putting it back is a fill and an outline rather than
			// anything read from the cartridge.
			Msx2_Fill(x, MSX2_HAND_Y, MSX2_CARD_W, MSX2_CARD_H, MSX2_BLACK);
			if(!g_hand_hidden)
				Msx2_FrameRect((u8)(x - 1), (u8)(MSX2_HAND_Y - 1),
				               MSX2_CARD_W + 2, MSX2_CARD_H + 2,
				               MSX2_GOLD_COLOR);
			return;
		}

		index = (g_flag[slot] & F_FACEUP) ? card : MSX2_CARD_BACK_INDEX;
		Msx2_StreamRect((u16)(MSX2_CARD_ART_SEGMENT + index / MSX2_CARD_ART_PER_SEG),
		                (u16)((index % MSX2_CARD_ART_PER_SEG) * MSX2_CARD_ART_STRIDE),
		                x, MSX2_HAND_Y, MSX2_CARD_W, MSX2_CARD_H);

		if(F_QUEUE_OF(g_flag[slot]) != 0)
		{
			// A fusion material, numbered: the fold is left to right in the
			// order the player picked, so the order has to be visible.
			Msx2_Fill((u8)(x + 4), (u8)(MSX2_HAND_Y + 39), MSX2_CARD_W - 8, 7,
			          MSX2_TEAL);
			Msx2_TextColor(MSX2_BLACK, MSX2_TEAL);
			Msx2_TextAt((u8)(x + 12), (u8)(MSX2_HAND_Y + 39), "F");
			Msx2_NumAt((u8)(x + 20), (u8)(MSX2_HAND_Y + 39),
			           (i16)F_QUEUE_OF(g_flag[slot]));
		}
		return;
	}

	if(card == MSX2_CARD_NONE)
	{
		// Empty: the arena's own pixels for this slot on this stage, cut out of
		// the quantised capture.  A fill would flatten the floor the board is
		// standing on, and the ring around the slot with it.
		const u8* box = g_msx2_slot_box[g_view][slot];
		tile = (u16)(((u16)g_stage * MSX2_BOARD_VIEWS + g_view)
		             * MSX2_SLOT_ART_PER_VIEW + slot);
		Msx2_StreamRect((u16)(MSX2_SLOT_ART_SEGMENT + tile / MSX2_SLOT_ART_PER_SEG),
		                (u16)((tile % MSX2_SLOT_ART_PER_SEG) * MSX2_SLOT_ART_STRIDE),
		                box[0], box[1], box[2], box[3]);
		return;
	}

	index = (g_flag[slot] & F_FACEUP) ? card : MSX2_CARD_BACK_INDEX;
	Msx2_RasterSetView(g_view);
	Msx2_RasterCard(index, slot);

	// Defence position: the other targets turn the card sideways on the board
	// plane, and warping a second quad for that would double what the cartridge
	// carries for a state a word states more clearly at this size.
	if(g_flag[slot] & F_DEFENSE)
	{
		const u8* box = g_msx2_slot_box[g_view][slot];
		u8 x = (u8)(box[0] + (box[2] >> 1) - 11);
		u8 y = (u8)(box[1] + box[3] - 12);
		Msx2_Fill(x, y, 24, 8, MSX2_DEEP_BLUE);
		Msx2_TextColor(MSX2_WHITE, MSX2_DEEP_BLUE);
		Msx2_TextAt((u8)(x + 3), y, "DEF");
	}
}

// The selection bracket.  A field slot's follows its projected quad -- a
// rectangle around a trapezoid sits visibly beside the card it is selecting --
// and it is drawn INTO the flat ring the generator baked just outside every
// quad, so erasing it is the same four lines in MSX2_RING_COLOR and no artwork
// underneath is ever repaired.  A hand slot's is the baked gold frame, redrawn.
static void Msx2_BoardCursor(u8 slot, u8 color)
{
	if(IS_HAND(slot))
	{
		u8 x;
		// The hand is off the screen for a landing, and a bracket around a
		// position that is not there reads as a stray white box.
		if(g_hand_hidden)
			return;
		x = HAND_X(slot - MSX2_FIELD_SLOTS);
		Msx2_FrameRect((u8)(x - 1), (u8)(MSX2_HAND_Y - 1), MSX2_CARD_W + 2,
		               MSX2_CARD_H + 2, color);
		return;
	}
	Msx2_QuadOutline(g_msx2_slot_quad[g_view][slot], color);
}

// What "erase the cursor" means depends on which strip the slot is in: the
// board's baked ring, or the hand band's baked gold frame.
static u8 Msx2_BoardRestColor(u8 slot)
{
	return IS_HAND(slot) ? MSX2_GOLD_COLOR : MSX2_RING_COLOR;
}

// Gold to choose with, red to attack with: the cursor colour is the only place
// the mode is stated without words.
static u8 Msx2_BoardCursorColor(void)
{
	if(g_mode == M_TARGET)
		return MSX2_RED;
	if(g_mode == M_COM)
		return MSX2_RED;
	if((g_mode == M_PLACE) || (g_mode == M_EQUIP))
		return MSX2_TEAL;
	// White, not gold: every card already has a gold frame, and a gold cursor
	// on top of one is invisible.
	return MSX2_WHITE;
}

static void Msx2_BoardHud(void)
{
	Msx2_Fill(1, 1, MSX2_SCREEN_W - 2, MSX2_HUD_H - 2, MSX2_PANEL_COLOR);

	Msx2_TextColor(MSX2_RED, MSX2_PANEL_COLOR);
	Msx2_TextAt(4, 3, "COM");
	Msx2_NumAt(28, 3, g_duel.side[MSX2_OWNER_COM].lp);

	Msx2_TextColor(MSX2_GOLD, MSX2_PANEL_COLOR);
	Msx2_TextAt(100, 3, Msx2_UiText(MSX2_S_TURN));
	Msx2_NumAt(130, 3, (i16)g_duel.turns);

	Msx2_TextColor(MSX2_TEAL, MSX2_PANEL_COLOR);
	Msx2_TextAt(196, 3, "YOU");
	Msx2_NumAt(220, 3, g_duel.side[MSX2_OWNER_PLAYER].lp);
}

static const c8* Msx2_BoardPrompt(void)
{
	if(g_refuse != 0)
		return Msx2_UiText(MSX2_S_THOSE_CARDS_DO_NOT_FUSE);
	switch(g_mode)
	{
	case M_PLACE:  return g_place_def ? Msx2_UiText(MSX2_S_PLACE_IN_DEFENCE_UP_DOWN_ATK)
	                                  : Msx2_UiText(MSX2_S_PLACE_IN_ATTACK_UP_DOWN_DEF);
	case M_EQUIP:  return Msx2_UiText(MSX2_S_PICK_A_MONSTER_TO_EQUIP);
	case M_TARGET: return Msx2_UiText(MSX2_S_PICK_THE_TARGET_ESC_CANCELS);
	case M_COM:    return Msx2_UiText(MSX2_S_THE_OPPONENT_IS_THINKING);
	case M_CHECK:  return Msx2_UiText(MSX2_S_SPACE_RETURNS_TO_THE_DUEL);
	case M_OVER:   return Msx2_UiText(MSX2_S_SPACE_RETURNS_TO_THE_TITLE);
	default:
		if(g_queue_n != 0)
		{
			if(g_zone == ZONE_HAND) return Msx2_UiText(MSX2_S_DOWN_PICKS_MATERIALS_ESC_CLE);
			return Msx2_UiText(MSX2_S_SPACE_FUSES_HERE_ESC_CLEARS);
		}
		if(g_zone == ZONE_HAND)  return Msx2_UiText(MSX2_S_SPACE_PLAYS_DOWN_FUSES_C_CHE);
		if(g_zone == ZONE_FIELD) return Msx2_UiText(MSX2_S_SPACE_ATTACKS_ESC_ENDS_TURN);
		return Msx2_UiText(MSX2_S_OPPONENT_ROW_SPACE_CHECKS);
	}
}

static void Msx2_BoardInfo(void)
{
	u8 card = Msx2_BoardHovered();

	Msx2_Fill(1, (u8)(MSX2_INFO_Y + 1), MSX2_SCREEN_W - 2,
	          MSX2_SCREEN_H - MSX2_INFO_Y - 2, MSX2_PANEL_COLOR);

	if(g_mode == M_OVER)
	{
		Msx2_TextColor((g_duel.result > 0) ? MSX2_GOLD : MSX2_RED, MSX2_PANEL_COLOR);
		Msx2_TextCenter((u8)(MSX2_INFO_Y + 3),
		                (g_duel.result > 0) ? Msx2_UiText(MSX2_S_YOU_WIN_THE_DUEL) : Msx2_UiText(MSX2_S_YOU_HAVE_LOST));
	}
	else if(card != MSX2_CARD_NONE)
	{
		// Names live in the cartridge, not in the 32 KB of code: 78 of them at
		// a fixed stride is 1.8 KB the link cannot spare.
		Msx2_RomRead(MSX2_TEXT_SEGMENT, (u16)card * MSX2_NAME_STRIDE,
		             (u8*)g_name, MSX2_NAME_STRIDE);
		g_name[MSX2_NAME_STRIDE - 1] = 0;
		Msx2_TextColor(MSX2_WHITE, MSX2_PANEL_COLOR);
		Msx2_TextCenter((u8)(MSX2_INFO_Y + 3), g_name);

		if(Msx2_IsMonster(card))
		{
			u8 slot = SLOT_OF(g_zone, g_sel);
			Msx2_TextColor(MSX2_GOLD, MSX2_PANEL_COLOR);
			Msx2_TextAt(56, (u8)(MSX2_INFO_Y + 12), "ATK");
			Msx2_TextAt(140, (u8)(MSX2_INFO_Y + 12), "DEF");
			Msx2_TextColor(MSX2_WHITE, MSX2_PANEL_COLOR);
			// On the board the equips count; in the hand there is nothing to
			// equip yet, so the printed figure is the card's own.
			if(g_zone == ZONE_HAND)
			{
				Msx2_NumAt(82, (u8)(MSX2_INFO_Y + 12), (i16)Msx2_CardAtk(card));
				Msx2_NumAt(166, (u8)(MSX2_INFO_Y + 12), (i16)Msx2_CardDef(card));
			}
			else
			{
				u8 owner = (g_zone == ZONE_COM) ? MSX2_OWNER_COM : MSX2_OWNER_PLAYER;
				Msx2_NumAt(82, (u8)(MSX2_INFO_Y + 12),
				           Msx2_FieldAtk(owner, SLOT_INDEX(slot)));
				Msx2_NumAt(166, (u8)(MSX2_INFO_Y + 12),
				           Msx2_FieldDef(owner, SLOT_INDEX(slot)));
			}
		}
		else
		{
			Msx2_TextColor(MSX2_TEAL, MSX2_PANEL_COLOR);
			Msx2_TextCenter((u8)(MSX2_INFO_Y + 12), Msx2_UiText(MSX2_S_SUPPORT_CARD));
		}
	}

	Msx2_TextColor(MSX2_SAND, MSX2_PANEL_COLOR);
	Msx2_TextCenter((u8)(MSX2_INFO_Y + 21), Msx2_BoardPrompt());
}

static bool Msx2_BoardPaint(void);

static void Msx2_BoardFxBanner(const c8* text, u8 color)
{
	Msx2_Fill(1, (u8)(MSX2_INFO_Y + 1), MSX2_SCREEN_W - 2,
	          MSX2_SCREEN_H - MSX2_INFO_Y - 2, MSX2_PANEL_COLOR);
	Msx2_TextColor(color, MSX2_PANEL_COLOR);
	Msx2_TextCenter((u8)(MSX2_INFO_Y + 3), text);
}

// The five hand frames, put back after the band has been blacked out.
static void Msx2_BoardHandFrames(void)
{
	u8 i;
	for(i = 0; i < MSX2_HAND_SLOTS; ++i)
		Msx2_FrameRect((u8)(HAND_X(i) - 1), (u8)(MSX2_HAND_Y - 1),
		               MSX2_CARD_W + 2, MSX2_CARD_H + 2, MSX2_GOLD_COLOR);
}

// The effects that put a card down on the board, and so get the hand-off-screen
// flight, the board bend and the bare top-view hold.
static bool Msx2_BoardFxIsLanding(void)
{
	return (g_fx_kind == FX_SUMMON) || (g_fx_kind == FX_EQUIP)
	    || (g_fx_kind == FX_COM_PLACE);
}

static bool Msx2_BoardFxIsBattle(void)
{
	return (g_fx_kind == FX_ATTACK) || (g_fx_kind == FX_COM_ATTACK);
}

static void Msx2_BoardBattleCard(u8 card, u8 x, u8 y)
{
	if(card >= MSX2_BATTLE_CARD_COUNT)
		return;
	Msx2_StreamRect((u16)(MSX2_BATTLE_CARD_SEGMENT + card), 0,
	                x, y, MSX2_BATTLE_CARD_W, MSX2_BATTLE_CARD_H);
}

static void Msx2_BoardBattleName(u8 card, u8 y)
{
	if(card >= MSX2_BATTLE_CARD_COUNT)
		return;
	Msx2_RomRead(MSX2_TEXT_SEGMENT, (u16)card * MSX2_NAME_STRIDE,
	             (u8*)g_name, MSX2_NAME_STRIDE);
	g_name[MSX2_NAME_STRIDE - 1] = 0;
	Msx2_TextColor(MSX2_WHITE, MSX2_BLACK);
	Msx2_TextAt(4, y, g_name);
}

static void Msx2_BoardDrawBattleBase(u8 direct, u8 trap, u8 ax, u8 dx)
{
	Msx2_BoardBattleName(g_duel.last_attacker_card, 3);
	Msx2_BoardBattleCard(g_duel.last_attacker_card, ax, 23);
	if(!direct && !trap)
	{
		Msx2_BoardBattleCard(g_duel.last_defender_card, dx, 23);
		Msx2_BoardBattleName(g_duel.last_defender_card, 171);
	}
	else
	{
		Msx2_TextColor(MSX2_GOLD, MSX2_BLACK);
		Msx2_TextAt(4, 171, trap ? Msx2_UiText(MSX2_S_TRAP_COUNTER) : Msx2_UiText(MSX2_S_DIRECT_ATTACK));
	}

	Msx2_TextColor(MSX2_WHITE, MSX2_BLACK);
	if(!trap)
	{
		Msx2_TextAt((u8)(ax + 10), 149, "ATK");
		Msx2_NumAt((u8)(ax + 38), 149, g_duel.last_battle.attacker_atk);
		if(!direct)
		{
			Msx2_TextAt((u8)(dx + 8), 149,
			             g_duel.last_battle.defender_passive ? "DEF" : "ATK");
			Msx2_NumAt((u8)(dx + 36), 149, g_duel.last_battle.defender_value);
		}
	}
}

// Attacks leave the arena completely.  This is the MSX2 version of the shared
// full-screen battle cut-in: large source-resolution card art on the same black
// stage as PC-FX, with live field stats and outcome text. No board pixel remains on
// screen until the result has been read.
static void Msx2_BoardShowBattleCutin(void)
{
	u8 page = (u8)(Msx2_VideoGetShowPage() ^ 1);

	g_batt_direct = (g_duel.last_battle.outcome == MSX2_BATTLE_DIRECT);
	g_batt_trap = g_duel.last_trap_fired;
	g_batt_ax = g_batt_direct ? 84 : (g_fx_owner == MSX2_OWNER_PLAYER ? 12 : 156);
	g_batt_dx = (g_fx_owner == MSX2_OWNER_PLAYER) ? 156 : 12;
	// Which way the attacker has to move to reach what it is attacking.  A
	// direct hit has nothing in front of it, so it drives at the opponent's
	// side of the screen instead.
	g_batt_dir = g_batt_direct ? (g_fx_owner == MSX2_OWNER_PLAYER)
	                           : (g_batt_dx > g_batt_ax);
	g_batt_phase = 0;
	g_batt_step = 0;
	g_batt_counter = FALSE;
	g_batt_px[0] = g_batt_px[1] = g_batt_ax;
	g_batt_dpx[0] = g_batt_dpx[1] = g_batt_dx;

	/* Compose the clean card page while output is blank.  The contact beat's
	   second page is built later, once the two cards have actually met; no
	   effect command ever touches the page being scanned. */
	VDP_EnableDisplay(FALSE);
	Msx2_VideoDrawPage(page);
	Msx2_StreamSceneBlanked(MSX2_SCENE_BATTLE_SEGMENT, page);
	Msx2_VideoDrawPage(page);
	Msx2_BoardDrawBattleBase(g_batt_direct, g_batt_trap, g_batt_ax, g_batt_dx);
	Msx2_VideoCopyPage(page, (u8)(page ^ 1));
	Msx2_VideoShowPage(page);
	VDP_EnableDisplay(TRUE);
}

// One step of the attacker closing on its target: erase where this page last
// had the card, put it down further along, and carry its ATK figure with it.
static void Msx2_BoardBattleMoveAttacker(void)
{
	u8 page = (u8)(Msx2_VideoGetShowPage() ^ 1);
	u8 travel = (u8)(g_batt_step * BATT_DX);
	u8 x = g_batt_dir ? (u8)(g_batt_ax + travel) : (u8)(g_batt_ax - travel);

	Msx2_VideoDrawPage(page);
	Msx2_Fill(g_batt_px[page], 23, MSX2_BATTLE_CARD_W, MSX2_BATTLE_CARD_H,
	          MSX2_BLACK);
	Msx2_Fill(g_batt_px[page], 149, MSX2_BATTLE_CARD_W, 8, MSX2_BLACK);
	Msx2_BoardBattleCard(g_duel.last_attacker_card, x, 23);
	if(!g_batt_trap)
	{
		Msx2_TextColor(MSX2_WHITE, MSX2_BLACK);
		Msx2_TextAt((u8)(x + 10), 149, "ATK");
		Msx2_NumAt((u8)(x + 38), 149, g_duel.last_battle.attacker_atk);
	}
	g_batt_px[page] = x;
}

// When the defender wins an attack-position clash, PC-FX gives it the answer:
// it drives back into the attacker, then returns before the losing card burns.
// Keep separate per-page positions so neither half of that round trip can
// expose the previous pose when the pages alternate.
static void Msx2_BoardBattleMoveDefender(void)
{
	u8 page = (u8)(Msx2_VideoGetShowPage() ^ 1);
	u8 travel = (u8)(g_batt_step * BATT_DX);
	u8 x = g_batt_dir ? (u8)(g_batt_dx - travel)
	                  : (u8)(g_batt_dx + travel);

	Msx2_VideoDrawPage(page);
	Msx2_Fill(g_batt_dpx[page], 23, MSX2_BATTLE_CARD_W,
	          MSX2_BATTLE_CARD_H, MSX2_BLACK);
	Msx2_Fill(g_batt_dpx[page], 149, MSX2_BATTLE_CARD_W, 8, MSX2_BLACK);
	Msx2_BoardBattleCard(g_duel.last_defender_card, x, 23);
	Msx2_TextColor(MSX2_WHITE, MSX2_BLACK);
	Msx2_TextAt((u8)(x + 8), 149,
	            g_duel.last_battle.defender_passive ? "DEF" : "ATK");
	Msx2_NumAt((u8)(x + 36), 149, g_duel.last_battle.defender_value);
	g_batt_dpx[page] = x;
}

// The shared/PC-FX cut-in burns a destroyed card away instead of leaving it
// whole until the board cut.  SCREEN 8 has no alpha or palette fade, so the
// MSX2 equivalent is a six-band wipe with a hot edge, composed on the hidden
// page and revealed only after the command engine has finished it.
static void Msx2_BoardBattleBurnStep(void)
{
	u8 page = (u8)(Msx2_VideoGetShowPage() ^ 1);
	u8 h = (u8)(g_batt_step * BATT_BURN_DY);
	u8 outcome = g_duel.last_battle.outcome;

	// Carry the preceding wipe stage forward, then extend it.  No page can
	// reveal an older band or a half-issued fill while the other is scanned.
	Msx2_VideoCopyPage((u8)(page ^ 1), page);
	Msx2_VideoDrawPage(page);
	if(g_batt_trap || outcome == MSX2_BATTLE_DESTROY_ATTACKER ||
	   outcome == MSX2_BATTLE_DESTROY_BOTH)
		Msx2_BattleFxBurnCard(g_batt_ax, h, g_batt_step);
	if(outcome == MSX2_BATTLE_DESTROY_DEFENDER ||
	   outcome == MSX2_BATTLE_DESTROY_BOTH)
		Msx2_BattleFxBurnCard(g_batt_dx, h, g_batt_step);
}

static void Msx2_BoardRestoreFromCutin(void)
{
	u8 page = (u8)(Msx2_VideoGetShowPage() ^ 1);
	u8 i;

	g_suppress_slot = MSX2_SLOT_NONE;
	Msx2_BoardSnapshot();
	VDP_EnableDisplay(FALSE);
	Msx2_VideoDrawPage(page);
	Msx2_StreamSceneBlanked(MSX2_VIEW_SEGMENT(g_stage, g_view), page);
	Msx2_VideoDrawPage(page);
	for(i = 0; i < SLOT_COUNT; ++i)
		if(g_want[i] != MSX2_CARD_NONE)
			Msx2_BoardBlitSlot(i);
	Msx2_BoardHud();
	Msx2_BoardInfo();
	Msx2_BoardCursor(SLOT_OF(g_zone, g_sel), Msx2_BoardCursorColor());
	Msx2_VideoCopyPage(page, (u8)(page ^ 1));
	Msx2_VideoShowPage(page);
	VDP_EnableDisplay(TRUE);

	for(i = 0; i < SLOT_COUNT; ++i)
	{
		g_shown[0][i] = g_shown[1][i] = g_want[i];
		g_shown_flag[0][i] = g_shown_flag[1][i] = g_flag[i];
	}
	g_cursor_at[0] = g_cursor_at[1] = SLOT_OF(g_zone, g_sel);
	g_cursor_col_at[0] = g_cursor_col_at[1] = Msx2_BoardCursorColor();
	g_panel_left = 0;
	g_fx_kind = FX_NONE;
	g_fx_followup = FX_NONE;
}

// A 40x48 face does not fit in the 44 offscreen rows below GRAPHIC 7's
// visible page.  Split its backing store into two 40x24 tiles side by side.
// Each retained page owns its own copy automatically because Msx2_CopyRect()
// addresses the current draw page.
#define FX_STASH_Y       216
#define FX_STASH_HALF_H  (MSX2_CARD_H / 2)

static void Msx2_BoardFxCardBacking(bool restore, u8 x, u8 y)
{
	u8 i;
	for(i = 0; i < 2; ++i)
	{
		u8 sx = (u8)(i * MSX2_CARD_W);
		u8 cy = (u8)(y + i * FX_STASH_HALF_H);
		if(restore)
			Msx2_CopyRect(sx, FX_STASH_Y, x, cy, MSX2_CARD_W, FX_STASH_HALF_H);
		else
			Msx2_CopyRect(x, cy, sx, FX_STASH_Y, MSX2_CARD_W, FX_STASH_HALF_H);
	}
}


// The card that is being played, drawn as itself.
//
// It leaves its hand position and travels in both axes to the projected slot.
// Before each opaque pose, the hidden page saves the 40x48 arena underneath in
// its offscreen rows; when that page comes round again the exact pixels are put
// back first.  This keeps the real card face instead of falling back to an XOR
// outline, and scan-out only ever sees completed poses.
static void Msx2_BoardFxCardFlight(bool erase)
{
	i16 sx = (g_fx_hand == MSX2_SLOT_NONE) ? 108 : HAND_X(g_fx_hand);
	// A flight is FX_LANDING_FRAMES long and g_fx_frames counts it down to 1,
	// so progress runs 0..13.  progress + progress/4 maps that onto 0..16
	// exactly, which turns the interpolation into a shift and keeps a signed
	// divider out of the nearly-full resident bank.
	u8 progress = (u8)(FX_LANDING_FRAMES - g_fx_frames);
	u8 ease = (u8)(progress + (progress >> 2));
	i16 ex = g_fx_dest_x;
	i16 ey = g_fx_dest_y;
	u8 x, y;
	u8 card = (g_fx_owner == MSX2_OWNER_COM) ? MSX2_CARD_BACK_INDEX : g_fx_card;

	x = (u8)(sx + (((ex - sx) * ease) >> 4));
	y = (u8)(MSX2_HAND_Y + (((ey - MSX2_HAND_Y) * ease) >> 4));

	if(erase)
	{
		Msx2_BoardFxCardBacking(TRUE, x, y);
		return;
	}
	Msx2_BoardFxCardBacking(FALSE, x, y);
	Msx2_StreamRect((u16)(MSX2_CARD_ART_SEGMENT + card / MSX2_CARD_ART_PER_SEG),
	                (u16)((card % MSX2_CARD_ART_PER_SEG) * MSX2_CARD_ART_STRIDE),
	                x, y, MSX2_CARD_W, MSX2_CARD_H);
}

static void Msx2_BoardFxDraw(bool erase)
{
	u8 flash = (g_fx_frames & 2) ? MSX2_GOLD : MSX2_WHITE;

	switch(g_fx_kind)
	{
	case FX_COM_CHOOSE:
		if(!erase)
			Msx2_BoardFxBanner(Msx2_UiText(MSX2_S_OPPONENT_CHOOSES_A_CARD), MSX2_RED);
		if(g_fx_hand != MSX2_SLOT_NONE)
		{
			u8 x = HAND_X(g_fx_hand);
			if(!erase && (g_fx_card < MSX2_CARD_ART_COUNT))
			{
				Msx2_StreamRect((u16)(MSX2_CARD_ART_SEGMENT + MSX2_CARD_BACK_INDEX / MSX2_CARD_ART_PER_SEG),
				                (u16)((MSX2_CARD_BACK_INDEX % MSX2_CARD_ART_PER_SEG) * MSX2_CARD_ART_STRIDE),
				                x, MSX2_HAND_Y, MSX2_CARD_W, MSX2_CARD_H);
			}
			Msx2_FrameRectXor((u8)(x - 2), (u8)(MSX2_HAND_Y - 2),
			                  MSX2_CARD_W + 4, MSX2_CARD_H + 4, flash);
		}
		else
		{
			// Attacks have no hand card left to point at; show the attacker as a
			// compact 2-D cut-in while the battle banner changes on the next beat.
			if(g_fx_field != MSX2_SLOT_NONE)
				Msx2_QuadOutlineXor(g_msx2_slot_quad[g_view][g_fx_field], flash);
		}
		break;

	// The three plain landings differ only in what the panel says.
	case FX_COM_PLACE:
	case FX_SUMMON:
	case FX_EQUIP:
		if(!erase)
		{
			if(g_fx_kind == FX_COM_PLACE)
				Msx2_BoardFxBanner(Msx2_UiText(MSX2_S_OPPONENT_PLACES_THE_CARD), MSX2_RED);
			else if(g_fx_kind == FX_SUMMON)
				Msx2_BoardFxBanner(Msx2_UiText(MSX2_S_SUMMON), MSX2_TEAL);
			else
				Msx2_BoardFxBanner(Msx2_UiText(MSX2_S_EQUIP_POWER), MSX2_GOLD);
		}
		if(g_fx_field != MSX2_SLOT_NONE)
			Msx2_BoardFxCardFlight(erase);
		break;

	case FX_SUPPORT:
		if(!erase)
		{
			Msx2_BoardFxBanner(Msx2_UiText(MSX2_S_SUPPORT_ACTIVATED), MSX2_TEAL);
			Msx2_TextColor(MSX2_WHITE, MSX2_PANEL_COLOR);
			Msx2_TextCenter(82, Msx2_UiText(MSX2_S_EFFECT));
		}
		Msx2_FrameRectXor(84, 70, 88, 34, flash);
		break;

	case FX_POSITION:
		if(!erase)
			Msx2_BoardFxBanner(g_fx_owner == MSX2_OWNER_COM ? Msx2_UiText(MSX2_S_OPPONENT_CHANGES_POSITION)
			                                                : Msx2_UiText(MSX2_S_CHANGE_POSITION), MSX2_GOLD);
		if(g_fx_field != MSX2_SLOT_NONE)
			Msx2_QuadOutlineXor(g_msx2_slot_quad[g_view][g_fx_field], flash);
		break;
	}
}

static void Msx2_BoardFxErase(u8 frame)
{
	u8 page = Msx2_VideoGetDrawPage();

	// The chosen COM card is the only opaque effect layer.  It is painted over
	// an empty hand slot, so restoring that slot is enough; all other effect
	// geometry is XOR and is removed by replaying the same commands.
	if((g_fx_kind == FX_COM_CHOOSE) && (g_fx_hand != MSX2_SLOT_NONE))
	{
		u8 slot = SLOT_OF(ZONE_HAND, g_fx_hand);
		Msx2_BoardBlitSlot(slot);
		g_shown[page][slot] = g_want[slot];
		g_shown_flag[page][slot] = g_flag[slot];
	}
	Msx2_BoardInfo();
	g_fx_frames = frame;
	Msx2_BoardFxDraw(TRUE);
}

// Take the hand off the screen for a landing.  The band goes flat black on
// both pages at once -- it is nearly black already, so the cards simply stop
// being there -- which is what makes the opaque flight above erasable, and is
// also the bare top view the card is meant to land into.
static void Msx2_BoardHideHand(void)
{
	u8 show = Msx2_VideoGetShowPage();
	u8 i;

	if(g_hand_hidden || !Msx2_BoardFxIsLanding())
		return;
	g_hand_hidden = TRUE;
	// Every landing action has a field destination; StartFx filters all other
	// effects before this path.
	{
		const u8* box = g_msx2_slot_box[g_view][g_fx_field];
		g_fx_dest_x = (u8)(box[0] + (box[2] >> 1) - MSX2_CARD_W / 2);
		g_fx_dest_y = (u8)(box[1] + (box[3] >> 1) - MSX2_CARD_H / 2);
	}
	g_fx_bend = FX_BEND_POSES;
	g_fx_hold = 0;
	Msx2_BoardSnapshot();

	for(i = 0; i < MSX2_VIDEO_PAGES; ++i)
	{
		Msx2_VideoDrawPage(i);
		if(g_cursor_at[i] != MSX2_SLOT_NONE)
		{
			Msx2_BoardCursor(g_cursor_at[i], Msx2_BoardRestColor(g_cursor_at[i]));
			g_cursor_at[i] = MSX2_SLOT_NONE;
		}
		Msx2_Fill(0, MSX2_HAND_BAND_Y, MSX2_SCREEN_W, MSX2_HAND_BAND_H,
		          MSX2_BLACK);
	}
	Msx2_VideoDrawPage((u8)(show ^ 1));

	for(i = MSX2_FIELD_SLOTS; i < SLOT_COUNT; ++i)
	{
		g_shown[0][i] = g_shown[1][i] = MSX2_CARD_NONE;
		g_shown_flag[0][i] = g_shown_flag[1][i] = 0;
	}
	if(IS_HAND(g_cursor_at[0])) g_cursor_at[0] = MSX2_SLOT_NONE;
	if(IS_HAND(g_cursor_at[1])) g_cursor_at[1] = MSX2_SLOT_NONE;
}

// The board settling under the card that just landed on it.
//
// This used to borrow the turn strip's neighbour of the resting pose as a
// "bend".  That strip is a half orbit sampled five times, so its neighbour is a
// quarter turn away: what played was a one-frame jump-cut to a completely
// different camera, and nothing on any other target lurches when a card is
// placed.  What is left is the part that was actually doing work -- putting the
// resting board band back from the cartridge so the cleanup repaint can draw
// every card into its own projected quad -- and the landing costs one 29 KB
// stream instead of two.
static void Msx2_BoardStepBend(void)
{
	u8 pose = (g_view == BOARD_VIEW_COM) ? (u8)(MSX2_MOVE_TURN_POSES - 1) : 0;
	u8 i;

	Msx2_Fill(0, MSX2_HAND_BAND_Y, MSX2_SCREEN_W, MSX2_HAND_BAND_H, MSX2_BLACK);
	Msx2_StreamBand((u16)(MSX2_MOVE_TURN_SEGMENT(g_stage)
	                      + (u16)pose * MSX2_MOVE_POSE_SEGS),
	                MSX2_BAND_Y, MSX2_BAND_H);
	// The pose is a picture of the empty board, so every live card on this page
	// is gone and the cleanup repaint below has to put all of them back.
	for(i = 0; i < MSX2_FIELD_SLOTS; ++i)
	{
		g_shown[0][i] = g_shown[1][i] = MSX2_CARD_NONE;
		g_shown_flag[0][i] = g_shown_flag[1][i] = 0;
	}
	g_cursor_at[0] = g_cursor_at[1] = MSX2_SLOT_NONE;
	--g_fx_bend;
}

static void Msx2_BoardStartFx(void)
{
	u8 action = g_duel.last_action;
	u8 owner;
	if(action == MSX2_ACTION_NONE || g_fx_kind != FX_NONE)
		return;

	owner = g_duel.last_action_owner;
	g_fx_owner = owner;
	g_fx_card = g_duel.last_action_card;
	g_fx_hand = g_duel.last_action_hand_slot;
	g_fx_field = (g_duel.last_action_field_slot == MSX2_SLOT_NONE)
	           ? MSX2_SLOT_NONE
	           : Msx2_BoardFieldSlot(owner, g_duel.last_action_field_slot);
	g_fx_followup = FX_NONE;
	g_suppress_slot = MSX2_SLOT_NONE;

	if((action == MSX2_ACTION_PLACE) || (action == MSX2_ACTION_FUSION))
	{
		g_suppress_slot = g_fx_field;
		if(owner == MSX2_OWNER_COM)
		{
			// The opponent's fusion keeps the retained board beat: the cut-in
			// below shows the materials the PLAYER chose, and there is no
			// record of the opponent's.
			g_fx_kind = FX_COM_CHOOSE;
			g_fx_followup = FX_COM_PLACE;
		}
		else if(action == MSX2_ACTION_FUSION)
		{
			// A fusion leaves the arena the way an attack does: it is the one
			// beat where the player is owed a look at what they just made.
			g_fx_kind = FX_FUSION;
			g_suppress_slot = MSX2_SLOT_NONE;
		}
		else
			g_fx_kind = FX_SUMMON;
	}
	else if(action == MSX2_ACTION_ATTACK)
	{
		// Attack presentation is exclusively the board-free 2-D cut-in.  Do
		// not prepend the COM's retained-board "choice" beat: even the wind-up
		// belongs to the 2-D screen under the owner's final presentation rule.
		g_fx_kind = (owner == MSX2_OWNER_COM) ? FX_COM_ATTACK : FX_ATTACK;
	}
	else if(action == MSX2_ACTION_EQUIP)
	{
		g_fx_kind = (owner == MSX2_OWNER_COM) ? FX_COM_CHOOSE : FX_EQUIP;
		if(owner == MSX2_OWNER_COM)
			g_fx_followup = FX_EQUIP;
	}
	else if(action == MSX2_ACTION_POSITION)
		g_fx_kind = FX_POSITION;
	else
		g_fx_kind = (owner == MSX2_OWNER_COM) ? FX_COM_CHOOSE : FX_SUPPORT;

	Msx2_ClearActionEvent();
	Msx2_BoardSnapshot();
	g_panel_left = MSX2_VIDEO_PAGES;
	g_fx_frames = (g_fx_kind == FX_COM_CHOOSE) ? 12 : FX_LANDING_FRAMES;
	g_fx_cleanup = FALSE;
	g_fx_page_frame[0] = g_fx_page_frame[1] = FX_FRAME_NONE;
	if(Msx2_BoardFxIsBattle())
	{
		g_fx_frames = 60;
		Msx2_BoardShowBattleCutin();
	}
	else if(g_fx_kind == FX_FUSION)
		Msx2_FusionBegin(g_fuse_mat, g_fuse_mat_n, g_duel.last_action_card);
	Msx2_BoardHideHand();
}

static void Msx2_BoardFinishFx(void)
{
	u8 next = g_fx_followup;
	if(next == FX_COM_PLACE)
	{
		// Keep the newly chosen card off the field while its outline travels
		// from the COM hand to the destination.  The settled projected card is
		// revealed by the final cleanup pass.
		g_suppress_slot = g_fx_field;
		Msx2_BoardSnapshot();
		g_panel_left = MSX2_VIDEO_PAGES;
		g_fx_kind = FX_COM_PLACE;
		g_fx_followup = FX_NONE;
		g_fx_frames = FX_LANDING_FRAMES;
		g_fx_page_frame[0] = g_fx_page_frame[1] = FX_FRAME_NONE;
		Msx2_BoardHideHand();
		return;
	}
	if(next == FX_EQUIP)
	{
		g_fx_kind = FX_EQUIP;
		g_fx_followup = FX_NONE;
		g_fx_frames = FX_LANDING_FRAMES;
		g_fx_page_frame[0] = g_fx_page_frame[1] = FX_FRAME_NONE;
		Msx2_BoardHideHand();
		return;
	}

	g_suppress_slot = MSX2_SLOT_NONE;
	g_hand_hidden = FALSE;
	g_fx_bend = 0;
	g_fx_hold = 0;
	Msx2_BoardSnapshot();
	g_panel_left = MSX2_VIDEO_PAGES;
	g_fx_kind = FX_NONE;
	g_fx_page_frame[0] = g_fx_page_frame[1] = FX_FRAME_NONE;
}

static bool Msx2_BoardRunFx(void)
{
	if(g_fx_kind == FX_NONE)
		return FALSE;
	if(g_fx_kind == FX_FUSION)
	{
		if(!Msx2_FusionStep())
			Msx2_BoardRestoreFromCutin();
		return TRUE;
	}
	if(Msx2_BoardFxIsBattle())
	{
		u8 page;
		switch(g_batt_phase)
		{
		case 0:
			/* The attacker closes, or the stronger defender answers.  One
			   88x120 blit is about six V-blanks, so five steps is roughly a
			   second of visible approach. */
			++g_batt_step;
			if(g_batt_counter) Msx2_BoardBattleMoveDefender();
			else               Msx2_BoardBattleMoveAttacker();
			Msx2_VideoFlipRequest();
			if(g_batt_step >= BATT_STEPS)
			{
				g_batt_phase = 1;
				g_batt_step = 0;
			}
			return TRUE;

		case 1:
			/* Level the pages at the meeting position, then build the contact
			   beat's second page on the one that is not being scanned. */
			page = (u8)(Msx2_VideoGetShowPage() ^ 1);
			Msx2_VideoCopyPage((u8)(page ^ 1), page);
			if(g_batt_counter) g_batt_dpx[page] = g_batt_dpx[page ^ 1];
			else               g_batt_px[page] = g_batt_px[page ^ 1];
			Msx2_VideoDrawPage(page);
			Msx2_BattleFxImpact(g_batt_trap
			                     ? (u8)(g_batt_px[page] + MSX2_BATTLE_CARD_W / 2)
			                     : g_batt_direct
			                       ? (g_fx_owner == MSX2_OWNER_PLAYER ? 220 : 36)
			                       : (u8)((g_batt_counter ? g_batt_ax : g_batt_dx)
			                              + MSX2_BATTLE_CARD_W / 2),
			                     g_batt_trap);
			g_batt_phase = 2;
			g_batt_step = 0;
			return TRUE;

		case 2:
			/* The flash is a page flip, so nothing is ever drawn over live
			   scan-out -- and the explosion over it is sprites, which cost one
			   attribute write a frame and need no repair at all.  That is what
			   buys eight frames of it where the bitmap could afford one held
			   pose. */
			Msx2_BattleFxBurst(g_batt_trap
			                   ? (u8)(g_batt_px[Msx2_VideoGetShowPage()]
			                          + MSX2_BATTLE_CARD_W / 2)
			                   : g_batt_direct
			                     ? (g_fx_owner == MSX2_OWNER_PLAYER ? 220 : 36)
			                     : (u8)((g_batt_counter ? g_batt_ax : g_batt_dx)
			                            + MSX2_BATTLE_CARD_W / 2),
			                   83, g_batt_step);
			Msx2_VideoFlipRequest();
			if(++g_batt_step >= MSX2_SPR_BURST_N)
			{
				Msx2_BattleFxBurst(0, 0, MSX2_SPR_BURST_N);
				g_batt_phase = 3;
				g_batt_step = BATT_STEPS;
			}
			return TRUE;

		case 3:
			/* Remove the impact marks from the other page before the retreat.
			   Both pages now contain the moving card at full extension, so each
			   backwards step can be composed off-screen and revealed in V-blank. */
			page = (u8)(Msx2_VideoGetShowPage() ^ 1);
			Msx2_VideoCopyPage((u8)(page ^ 1), page);
			if(g_batt_counter) g_batt_dpx[page] = g_batt_dpx[page ^ 1];
			else               g_batt_px[page] = g_batt_px[page ^ 1];
			g_batt_phase = 4;
			return TRUE;

		case 4:
			--g_batt_step;
			if(g_batt_counter) Msx2_BoardBattleMoveDefender();
			else               Msx2_BoardBattleMoveAttacker();
			if(g_batt_step == 0)
			{
				if(!g_batt_counter && !g_batt_trap &&
				   g_duel.last_battle.outcome == MSX2_BATTLE_DESTROY_ATTACKER)
				{
					// First reveal the attacker home; case 5 levels the other
					// page before the defender starts its own round trip.
					g_batt_phase = 5;
				}
				else if(g_batt_trap ||
				   g_duel.last_battle.outcome == MSX2_BATTLE_DESTROY_ATTACKER ||
				   g_duel.last_battle.outcome == MSX2_BATTLE_DESTROY_DEFENDER ||
				   g_duel.last_battle.outcome == MSX2_BATTLE_DESTROY_BOTH)
				{
					g_batt_step = 0;
					g_batt_phase = 6;
				}
				else
				{
					Msx2_BattleFxResult(FALSE);
					g_fx_frames = BATT_HOLD;
					g_batt_phase = 7;
				}
			}
			Msx2_VideoFlipRequest();
			return TRUE;

		case 5:
			page = (u8)(Msx2_VideoGetShowPage() ^ 1);
			Msx2_VideoCopyPage((u8)(page ^ 1), page);
			g_batt_counter = TRUE;
			g_batt_dpx[0] = g_batt_dpx[1] = g_batt_dx;
			g_batt_step = 0;
			g_batt_phase = 0;
			return TRUE;

		case 6:
			++g_batt_step;
			Msx2_BoardBattleBurnStep();
			if(g_batt_step >= MSX2_BATTLE_BURN_STEPS)
			{
				Msx2_BattleFxResult(g_batt_trap);
				g_fx_frames = BATT_HOLD;
				g_batt_phase = 7;
			}
			Msx2_VideoFlipRequest();
			return TRUE;

		default:
			if(g_fx_frames != 0)
			{
				--g_fx_frames;
				return TRUE;
			}
			Msx2_BoardRestoreFromCutin();
			return TRUE;
		}
	}

	if(g_fx_cleanup && (g_fx_bend != 0))
	{
		Msx2_VideoDrawPage(Msx2_VideoGetDrawPage());
		g_fx_page_frame[0] = g_fx_page_frame[1] = FX_FRAME_NONE;
		Msx2_BoardStepBend();
		Msx2_VideoFlipRequest();
		if(g_fx_bend == 0)
		{
			// The board has accepted the card.  Reveal it now, while the hand is
			// still absent, so the landing hold shows the actual new field state
			// rather than an empty destination until the normal HUD returns.
			g_suppress_slot = MSX2_SLOT_NONE;
			Msx2_BoardSnapshot();
			g_fx_hold = FX_HOLD_FRAMES;
		}
		return TRUE;
	}

	if(g_fx_hold != 0)
	{
		// The bare top view: the card is on the board, the hand is still off
		// the screen and only the bottom panel is under it.  Repaint into it
		// for the first frames, then simply hold it.
		u8 page = Msx2_VideoGetDrawPage();
		u8 i;
		if(g_fx_hold == FX_HOLD_FRAMES)
		{
			// The bend left its last pose on the page that is now on screen and
			// its previous pose on the other.  Level them, or the hold would
			// flicker between two camera positions.
			Msx2_VideoCopyPage((u8)(page ^ 1), page);
			--g_fx_hold;
			return TRUE;
		}
		for(i = 0; i < 4; ++i)
			if(!Msx2_BoardPaint())
				break;
		if(i != 0)
			Msx2_VideoFlipRequest();
		--g_fx_hold;
		if(g_fx_hold == 0)
		{
			g_hand_hidden = FALSE;
			Msx2_BoardSnapshot();
			Msx2_VideoDrawPage(page);
			Msx2_BoardHandFrames();
			g_cursor_at[0] = g_cursor_at[1] = MSX2_SLOT_NONE;
			g_panel_left = MSX2_VIDEO_PAGES;
		}
		return TRUE;
	}

	if(g_fx_cleanup)
	{
		// The page we draw now did not receive the previous effect, so it is the
		// clean copy.  Finish all retained repairs there, then copy that clean
		// page over the page that held the last flash.  Without the copy, the
		// next turn would eventually flip back to a page with a stale card
		// flight because the effect overlay is intentionally not in g_shown[].
		u8 page = Msx2_VideoGetDrawPage();
		u8 i;
		if(g_fx_page_frame[page] != FX_FRAME_NONE)
			Msx2_BoardFxErase(g_fx_page_frame[page]);
		for(i = 0; i < 32; ++i)
			if(!Msx2_BoardPaint())
				break;
		Msx2_VideoCopyPage(page, (u8)(page ^ 1));
		for(i = 0; i < SLOT_COUNT; ++i)
		{
			g_shown[0][i] = g_shown[1][i] = g_want[i];
			g_shown_flag[0][i] = g_shown_flag[1][i] = g_flag[i];
		}
		g_cursor_at[0] = g_cursor_at[1] = SLOT_OF(g_zone, g_sel);
		g_cursor_col_at[0] = g_cursor_col_at[1] = Msx2_BoardCursorColor();
		g_fx_cleanup = FALSE;
		Msx2_VideoFlipRequest();
		Msx2_BoardFinishFx();
		return TRUE;
	}

	{
		u8 page = Msx2_VideoGetDrawPage();
		u8 frame = g_fx_frames;
		if(g_fx_page_frame[page] != FX_FRAME_NONE)
		{
			Msx2_BoardFxErase(g_fx_page_frame[page]);
			g_fx_frames = frame;
		}
		Msx2_BoardPaint();
		Msx2_BoardFxDraw(FALSE);
		g_fx_page_frame[page] = g_fx_frames;
	}
	Msx2_VideoFlipRequest();
	--g_fx_frames;
	if(g_fx_frames == 0)
		g_fx_cleanup = TRUE;
	return TRUE;
}

// Repaint whatever the page in front of us is not showing.  Bounded: a card is
// about a frame of VDP time, so at most two per frame, and the rest waits.
static bool Msx2_BoardPaint(void)
{
	u8 page = Msx2_VideoGetDrawPage();
	u8 cursor = SLOT_OF(g_zone, g_sel);
	u8 color = Msx2_BoardCursorColor();

	// With the strip off the screen a bracket around a hand position is a
	// stray white box; on the board it is still the cursor, and hiding it
	// while the player is choosing a slot is what made the top view unusable.
	// An effect owns the whole screen, so nothing is bracketed under one.
	if((g_hand_hidden && IS_HAND(cursor)) || (g_fx_kind != FX_NONE))
		cursor = MSX2_SLOT_NONE;
	bool painted = FALSE;
	u8 cards = 0;
	u8 i;

	if(g_panel_left != 0)
	{
		Msx2_BoardHud();
		Msx2_BoardInfo();
		--g_panel_left;
		painted = TRUE;
	}

	// THE WHOLE HAND STRIP AT ONCE.
	// The loop below repaints one card a frame, which is the right budget for
	// a card arriving on the board -- but when the strip changes hands, all
	// five change together, and the opponent's first effect owns the screen
	// long before a one-a-frame painter has finished.  What stayed on the
	// screen through the opponent's turn was the player's own hand.
	if(g_hand_left != 0)
	{
		u8 j;
		// Overhead, those rows are board: blacking them out and framing five
		// empty hand positions would punch the table full of holes.  The strip
		// is still marked painted so the retained loop below leaves it alone.
		if(g_view != MSX2_VIEW_OVER)
		{
			Msx2_Fill(0, MSX2_HAND_BAND_Y, MSX2_SCREEN_W, MSX2_HAND_BAND_H,
			          MSX2_BLACK);
			if(!g_hand_hidden)
				Msx2_BoardHandFrames();
		}
		for(j = MSX2_FIELD_SLOTS; j < SLOT_COUNT; ++j)
		{
			if(g_view != MSX2_VIEW_OVER)
				Msx2_BoardBlitSlot(j);
			g_shown[page][j] = g_want[j];
			g_shown_flag[page][j] = g_flag[j];
		}
		--g_hand_left;
		painted = TRUE;
	}

	for(i = 0; (i < SLOT_COUNT) && (cards < 1); ++i)
	{
		if((g_shown[page][i] == g_want[i]) && (g_shown_flag[page][i] == g_flag[i]))
			continue;
		Msx2_BoardBlitSlot(i);
		g_shown[page][i] = g_want[i];
		g_shown_flag[page][i] = g_flag[i];
		++cards;
		painted = TRUE;
	}

	if((g_cursor_at[page] != cursor) || (g_cursor_col_at[page] != color))
	{
		if(g_cursor_at[page] != MSX2_SLOT_NONE)
			Msx2_BoardCursor(g_cursor_at[page],
			                 Msx2_BoardRestColor(g_cursor_at[page]));
		Msx2_BoardCursor(cursor, color);
		g_cursor_at[page] = cursor;
		g_cursor_col_at[page] = color;
		painted = TRUE;
	}

	return painted;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Entry
// ─────────────────────────────────────────────────────────────────────────────

// A duel opens on nothing: no HUD, no hand row, no info panel, and a black
// board band for the camera arc to fill.  The interface arrives only once the
// camera has settled into the player's chair, the way the other ports open.
static void Msx2_BoardBlankAll(void)
{
	Msx2_Fill(0, 0, MSX2_SCREEN_W, MSX2_SCREEN_H, MSX2_BLACK);
}

// The baked panel grounds, put back by hand.  Restreaming the whole resting
// view would do it too, but that needs the display blanked for 54 KB and the
// flash would land in the middle of the opening.
static void Msx2_BoardRevealPanels(void)
{
	Msx2_Fill(0, 0, MSX2_SCREEN_W, MSX2_HUD_H, MSX2_PANEL_COLOR);
	Msx2_FrameRect(0, 0, MSX2_SCREEN_W, MSX2_HUD_H, MSX2_GOLD_COLOR);
	Msx2_Fill(0, MSX2_INFO_Y, MSX2_SCREEN_W,
	          (u8)(MSX2_SCREEN_H - MSX2_INFO_Y), MSX2_PANEL_COLOR);
	Msx2_FrameRect(0, MSX2_INFO_Y, MSX2_SCREEN_W,
	               (u8)(MSX2_SCREEN_H - MSX2_INFO_Y), MSX2_GOLD_COLOR);
	Msx2_BoardHandFrames();
}

// One frame of the opening deal: the card in flight is erased from this page
// at the position it last had here, redrawn one step further left, and the
// hand's gold frames are put back where it crossed them.
static void Msx2_BoardStepDeal(void)
{
	u8 page = (u8)(Msx2_VideoGetShowPage() ^ 1);
	u8 card;
	u8 x;

	Msx2_VideoDrawPage(page);

	// THE LAST CARD'S LEFTOVER.
	// A flight alternates pages, and each pass erases the copy THIS page was
	// left holding before drawing the next one.  The final pose therefore
	// cleans only the page it lands on: the other one still carries the card a
	// couple of pixels short of the slot, and the two columns that stick out
	// past the settled card flickered on every flip for the rest of the duel.
	// Every slot but the last was cleaned up by the following card's first
	// pass, which is exactly why it was only ever the last one.  So the deal
	// ends with one more pass, on that page, doing nothing but the erase.
	if(g_deal_slot >= MSX2_HAND_SLOTS)
	{
		u8 i;
		if(g_deal_px[page] != MSX2_SLOT_NONE)
		{
			Msx2_Fill(g_deal_px[page], MSX2_HAND_Y, MSX2_CARD_W, MSX2_CARD_H,
			          MSX2_BLACK);
			Msx2_BoardHandFrames();
			g_deal_px[page] = MSX2_SLOT_NONE;
			for(i = MSX2_FIELD_SLOTS; i < SLOT_COUNT; ++i)
				g_shown[page][i] = MSX2_CARD_NONE;
		}
		for(i = 0; i < SLOT_COUNT; ++i)
			if(!Msx2_BoardPaint())
				break;
		Msx2_VideoFlipRequest();
		g_mode = M_IDLE;
		g_panel_left = MSX2_VIDEO_PAGES;
		return;
	}

	if(g_deal_px[page] != MSX2_SLOT_NONE)
	{
		Msx2_Fill(g_deal_px[page], MSX2_HAND_Y, MSX2_CARD_W, MSX2_CARD_H,
		          MSX2_BLACK);
		Msx2_BoardHandFrames();
		g_deal_px[page] = MSX2_SLOT_NONE;
	}

	card = g_duel.side[MSX2_OWNER_PLAYER].hand[g_deal_slot];
	if((card != MSX2_CARD_NONE) && (card < MSX2_CARD_ART_COUNT))
	{
		u8 target = HAND_X(g_deal_slot);
		x = (u8)(DEAL_START_X
		         - (u16)(DEAL_START_X - target) * g_deal_step / DEAL_STEPS);
		Msx2_StreamRect((u16)(MSX2_CARD_ART_SEGMENT + card / MSX2_CARD_ART_PER_SEG),
		                (u16)((card % MSX2_CARD_ART_PER_SEG) * MSX2_CARD_ART_STRIDE),
		                x, MSX2_HAND_Y, MSX2_CARD_W, MSX2_CARD_H);
		g_deal_px[page] = x;
	}

	Msx2_BoardPaint();
	Msx2_VideoFlipRequest();

	if(g_deal_step < DEAL_STEPS)
	{
		++g_deal_step;
		return;
	}

	// Landed.  The retained model owns this position from here, so the copy the
	// flight left on this page is exactly what the painter would have drawn.
	if(card != MSX2_CARD_NONE)
	{
		u8 slot = SLOT_OF(ZONE_HAND, g_deal_slot);
		g_deal_reveal = (u8)(g_deal_slot + 1);
		Msx2_BoardSnapshot();
		g_shown[page][slot] = g_want[slot];
		g_shown_flag[page][slot] = g_flag[slot];
	}
	g_deal_px[page] = MSX2_SLOT_NONE;
	g_deal_step = 0;
	++g_deal_slot;
	if(g_deal_slot >= MSX2_HAND_SLOTS)
	{
		// The hand is dealt, but the other page still owes the erase above.
		g_deal_reveal = MSX2_HAND_SLOTS;
		Msx2_BoardSnapshot();
		g_panel_left = MSX2_VIDEO_PAGES;
	}
}

void Msx2_BoardEnter_In(u8 stage)
{
	u8 i;
	u8 page;

	g_stage = stage;
	g_mode = M_OPENING;
	g_view = BOARD_VIEW_PLAYER;
	g_zone = ZONE_HAND;
	g_sel = 0;
	g_hand_pick = MSX2_SLOT_NONE;
	g_atk_pick = MSX2_SLOT_NONE;
	g_place_def = FALSE;
	g_queue_n = 0;
	g_refuse = 0;
	g_fx_kind = FX_NONE;
	g_fx_followup = FX_NONE;
	g_fx_frames = 0;
	g_fx_cleanup = FALSE;
	// The landing beats are their own little state machine, and a duel that is
	// entered a second time has to start it from rest: a stale g_hand_hidden
	// makes the next summon skip taking the hand off the screen, and a stale
	// g_fx_bend makes the cleanup pass stream that many camera poses.
	g_hand_hidden = FALSE;
	g_fx_bend = 0;
	g_fx_hold = 0;
	g_suppress_slot = MSX2_SLOT_NONE;
	g_fx_page_frame[0] = g_fx_page_frame[1] = FX_FRAME_NONE;
	g_move_pose = 0;
	g_move_target = BOARD_VIEW_PLAYER;
	g_move_forward = TRUE;
	g_deal_slot = 0;
	g_deal_step = 0;
	g_deal_reveal = 0;
	g_deal_px[0] = g_deal_px[1] = MSX2_SLOT_NONE;
	Msx2_BoardSnapshot();

	Msx2_RasterInit();
	Msx2_SpriteClear();

	/* Build both retained pages while the display is blank.  Their board bands
	   start black; Msx2_BoardStep() then streams one complete pose into the
	   hidden page and requests a V-blank flip.  The old loop wrote all sixteen
	   poses directly into the scanned page, which made every partially copied
	   band visible as a horizontal tear despite the rest of the game being
	   double-buffered. */
	page = (u8)(Msx2_VideoGetShowPage() ^ 1);
	VDP_EnableDisplay(FALSE);
	Msx2_VideoDrawPage(page);
	Msx2_StreamSceneBlanked(MSX2_VIEW_SEGMENT(stage, g_view), page);
	Msx2_VideoCopyPage(page, (u8)(page ^ 1));
	Msx2_VideoDrawPage(page);
	Msx2_BoardBlankAll();
	Msx2_VideoDrawPage((u8)(page ^ 1));
	Msx2_BoardBlankAll();
	Msx2_VideoShowPage(page);
	VDP_EnableDisplay(TRUE);

	// No live card or cursor has been painted yet.  The final opening pose is
	// the retained player view; after it lands the ordinary bounded painter
	// deals the hand and HUD onto the two pages.
	for(i = 0; i < SLOT_COUNT; ++i)
	{
		g_shown[0][i] = g_shown[1][i] = MSX2_CARD_NONE;
		g_shown_flag[0][i] = g_shown_flag[1][i] = 0;
	}
	g_cursor_at[0] = g_cursor_at[1] = MSX2_SLOT_NONE;
	g_cursor_col_at[0] = g_cursor_col_at[1] = 0;
	g_panel_left = 0;
	g_hand_left = 0;
}

// Begin a turn handoff.  Playback itself is one pose per BoardStep on the
// hidden page, followed by a V-blank flip; this function never touches the
// scanned page.
// ── The overhead view ───────────────────────────────────────────────────────
//
// Walking up out of the hand puts the camera over the table, the way it does
// on PC-FX and on the PC build.  It is a cut, not a move: there is no baked
// camera path between a duellist's chair and straight down, and the two are
// not the same picture in any case -- the overhead board takes the hand's rows
// as well as the board band, because in this view there is no hand.
//
// A cut costs a blanked stream of the whole view into both pages, about a
// quarter of a second of black.  That is affordable HERE and nowhere else: it
// happens when the player deliberately walks off the hand strip or back onto
// it, not on every press, and the port already spends more than that on a
// chair change.
static void Msx2_BoardCutTo(u8 view)
{
	u8 i;

	if(view == g_view)
		return;

	g_view = view;
	Msx2_RasterSetView(view);
	VDP_EnableDisplay(FALSE);
	Msx2_StreamSceneBlanked(MSX2_VIEW_SEGMENT(g_stage, view), MSX2_PAGE_0);
	Msx2_StreamSceneBlanked(MSX2_VIEW_SEGMENT(g_stage, view), MSX2_PAGE_1);
	VDP_EnableDisplay(TRUE);

	// The picture underneath every retained card is a different picture now,
	// so the painter owes both pages everything.
	for(i = 0; i < SLOT_COUNT; ++i)
	{
		g_shown[0][i] = g_shown[1][i] = MSX2_CARD_NONE;
		g_shown_flag[0][i] = g_shown_flag[1][i] = 0;
	}
	g_cursor_at[0] = g_cursor_at[1] = MSX2_SLOT_NONE;
	g_cursor_col_at[0] = g_cursor_col_at[1] = 0;
	g_panel_left = MSX2_VIDEO_PAGES;
	g_hand_left = MSX2_VIDEO_PAGES;
	Msx2_BoardSnapshot();
}

static void Msx2_BoardSwitchView(u8 view, bool forward)
{
	// The baked camera path runs between the two chairs and nowhere else, and
	// it only redraws the board band -- so it cannot start from overhead,
	// where the picture goes on down through the hand's rows.  Come back down
	// to the player's chair first.
	if(g_view == MSX2_VIEW_OVER)
		Msx2_BoardCutTo(BOARD_VIEW_PLAYER);
	if(view == g_view)
		return;

	g_mode = M_TURN;
	g_move_pose = 0;
	g_move_target = view;
	g_move_forward = forward;
}

static void Msx2_BoardStepCameraMove(void)
{
	u8 show = Msx2_VideoGetShowPage();
	u8 page = (u8)(show ^ 1);
	u8 i;

	if(g_mode == M_OPENING)
	{
		if(g_move_pose < MSX2_MOVE_OPENING_POSES)
		{
			Msx2_VideoDrawPage(page);
			Msx2_StreamBand((u16)(MSX2_MOVE_OPENING_SEGMENT(g_stage)
			                      + (u16)g_move_pose * MSX2_MOVE_POSE_SEGS),
			                MSX2_BAND_Y, MSX2_BAND_H);
			++g_move_pose;
			Msx2_VideoFlipRequest();
			return;
		}
	}
	else if(g_move_pose < MSX2_MOVE_TURN_POSES)
	{
		u8 pose = g_move_forward ? g_move_pose
		          : (u8)(MSX2_MOVE_TURN_POSES - 1 - g_move_pose);
		Msx2_VideoDrawPage(page);
		Msx2_StreamBand((u16)(MSX2_MOVE_TURN_SEGMENT(g_stage)
		                      + (u16)pose * MSX2_MOVE_POSE_SEGS),
		                MSX2_BAND_Y, MSX2_BAND_H);
		++g_move_pose;
		Msx2_VideoFlipRequest();
		return;
	}

	/* The final pose became visible at the V-blank immediately before this
	   step.  Copy that completed page to the hidden buffer so a later retained
	   flip cannot reveal the penultimate camera pose. */
	Msx2_VideoCopyPage(show, page);
	Msx2_VideoDrawPage(page);
	if(g_mode == M_OPENING)
	{
		// The camera has arrived.  Put the interface on both pages now, so the
		// hand can be dealt into a screen that already has somewhere to put it.
		Msx2_BoardRevealPanels();
		Msx2_VideoDrawPage(show);
		Msx2_BoardRevealPanels();
		Msx2_VideoDrawPage(page);
	}
	g_view = g_move_target;
	Msx2_RasterSetView(g_view);
	g_suppress_slot = MSX2_SLOT_NONE;
	Msx2_BoardSnapshot();
	for(i = 0; i < SLOT_COUNT; ++i)
	{
		g_shown[0][i] = g_shown[1][i] = MSX2_CARD_NONE;
		g_shown_flag[0][i] = g_shown_flag[1][i] = 0;
	}
	g_cursor_at[0] = g_cursor_at[1] = MSX2_SLOT_NONE;
	g_cursor_col_at[0] = g_cursor_col_at[1] = 0;
	g_panel_left = MSX2_VIDEO_PAGES;
	// A chair change hands the strip to the other player: five cards at once,
	// not one a frame.  The opening deals its own hand and owes nothing here.
	g_hand_left = (g_mode == M_OPENING) ? 0 : MSX2_VIDEO_PAGES;
	if(g_mode == M_OPENING)
	{
		g_mode = M_DEAL;
		g_deal_slot = 0;
		g_deal_step = 0;
		g_deal_px[0] = g_deal_px[1] = MSX2_SLOT_NONE;
		return;
	}
	g_mode = (g_view == BOARD_VIEW_COM) ? M_COM : M_IDLE;
}

// ─────────────────────────────────────────────────────────────────────────────
//  The player's turn
// ─────────────────────────────────────────────────────────────────────────────

// THE TOP VIEW (the other targets' up-from-the-hand).
// Walking up out of the hand takes the strip off the screen: the board is what
// the player is reading now, and the bottom panel keeps naming whatever the
// cursor is over, with its attack and defence.  Walking back down brings the
// five cards back.  Nothing about the board picture changes -- there is one
// captured arena per chair -- so this is the strip, the frames and the cards.
static void Msx2_BoardHandVisible(bool on)
{
	if(g_hand_hidden == (u8)!on)
		return;
	g_hand_hidden = (u8)!on;
	Msx2_BoardSnapshot();
	g_hand_left = MSX2_VIDEO_PAGES;
	g_panel_left = MSX2_VIDEO_PAGES;
}

static void Msx2_BoardTouch(void)
{
	Msx2_BoardSnapshot();
	g_panel_left = MSX2_VIDEO_PAGES;
}

static void Msx2_BoardCancel(void)
{
	g_mode = M_IDLE;
	if(g_hand_pick != MSX2_SLOT_NONE)
	{
		g_zone = ZONE_HAND;
		g_sel = g_hand_pick;
	}
	else if(g_atk_pick != MSX2_SLOT_NONE)
	{
		g_zone = ZONE_FIELD;
		g_sel = g_atk_pick;
	}
	g_hand_pick = MSX2_SLOT_NONE;
	g_atk_pick = MSX2_SLOT_NONE;
	g_panel_left = MSX2_VIDEO_PAGES;
}

// The first free field slot, or wherever the cursor already is if the row is
// full -- so choosing a monster lands the cursor somewhere it can be played.
static u8 Msx2_BoardFirstFree(void)
{
	u8 slot = Msx2_FirstFreeSlot(MSX2_OWNER_PLAYER);
	return (slot == MSX2_SLOT_NONE) ? 0 : slot;
}

// THE CARD CHECK SCREEN.
// A duel is played on cards the size of a postage stamp, so there has to be a
// way to look at one properly.  The cursor is already on a card, so the screen
// asks nothing more of the player: SPACE on the opponent's row -- which meant
// nothing at all before -- opens it, and so does C on the keyboard anywhere.
// A set card gives nothing away; Msx2_BoardHovered() has already refused it.
static void Msx2_BoardCheck(void)
{
	u8 card = Msx2_BoardHovered();
	u8 slot;
	u8 owner;

	if(card == MSX2_CARD_NONE)
		return;
	slot = SLOT_OF(g_zone, g_sel);
	owner = (g_zone == ZONE_COM) ? MSX2_OWNER_COM : MSX2_OWNER_PLAYER;
	Msx2_SfxPlay(MSX2_SFX_CONFIRM);
	g_mode = M_CHECK;
	if(g_zone == ZONE_HAND)
		Msx2_CardCheckCompose(card, (i16)Msx2_CardAtk(card),
		                      (i16)Msx2_CardDef(card));
	else
		Msx2_CardCheckCompose(card, Msx2_FieldAtk(owner, SLOT_INDEX(slot)),
		                      Msx2_FieldDef(owner, SLOT_INDEX(slot)));
}

static void Msx2_BoardConfirm(void)
{
	u8 card;

	switch(g_mode)
	{
	case M_IDLE:
		if(g_zone == ZONE_HAND)
		{
			card = g_duel.side[MSX2_OWNER_PLAYER].hand[g_sel];
			if(card == MSX2_CARD_NONE)
				return;
			if(Msx2_IsMonster(card))
			{
				g_hand_pick = g_sel;
				g_mode = M_PLACE;
				g_zone = ZONE_FIELD;
				g_sel = Msx2_BoardFirstFree();
				g_place_def = FALSE;
			}
			else if((Msx2_SupportKind(card) == MSX2_SUP_EQUIP) ||
			        (Msx2_SupportKind(card) == MSX2_SUP_GUARD))
			{
				g_hand_pick = g_sel;
				g_mode = M_EQUIP;
				g_zone = ZONE_FIELD;
				g_sel = Msx2_FirstLiveSlot(MSX2_OWNER_PLAYER);
				if(g_sel == MSX2_SLOT_NONE)
				{
					// Nothing to equip: refuse rather than strand the cursor.
					Msx2_BoardCancel();
					return;
				}
			}
			else
			{
				// Draw, heal, thunder and trap need no target.
				if(Msx2_PlaySupport(MSX2_OWNER_PLAYER, g_sel, 0))
					Msx2_SfxPlay(MSX2_SFX_CONFIRM);
			}
		}
		else if(g_zone == ZONE_COM)
		{
			Msx2_BoardCheck();
			return;
		}
		else if(g_zone == ZONE_FIELD)
		{
			if(g_queue_n != 0)
			{
				// A chain is waiting: this row picks where it lands, and the
				// slot may be empty or hold the monster the chain folds into.
				u8 m;
				g_fuse_mat_n = (g_queue_n > MSX2_FUSION_MATS)
				             ? MSX2_FUSION_MATS : g_queue_n;
				for(m = 0; m < g_fuse_mat_n; ++m)
					g_fuse_mat[m] =
						g_duel.side[MSX2_OWNER_PLAYER].hand[g_queue[m]];
				if(Msx2_PlaceFusion(MSX2_OWNER_PLAYER, g_queue, g_queue_n,
				                    g_sel, FALSE))
				{
					Msx2_SfxPlay(MSX2_SFX_CONFIRM);
					g_queue_n = 0;
				}
				else
					g_refuse = 96;      // no recipe, or no summon left this turn
				break;
			}
			if(!Msx2_IsMonster(g_duel.side[MSX2_OWNER_PLAYER].field[g_sel]))
				return;
			g_atk_pick = g_sel;
			if(Msx2_LiveMonsterCount(MSX2_OWNER_COM) == 0)
			{
				// No monsters to go through: this is a direct attack, and there
				// is nothing to aim at.
				if(Msx2_Attack(MSX2_OWNER_PLAYER, g_atk_pick, MSX2_SLOT_NONE))
					Msx2_SfxPlay(MSX2_SFX_CONFIRM);
				g_atk_pick = MSX2_SLOT_NONE;
			}
			else
			{
				g_mode = M_TARGET;
				g_zone = ZONE_COM;
				g_sel = Msx2_FirstLiveSlot(MSX2_OWNER_COM);
			}
		}
		break;

	case M_PLACE:
		if(Msx2_PlaceMonster(MSX2_OWNER_PLAYER, g_hand_pick, g_sel, g_place_def))
		{
			Msx2_SfxPlay(MSX2_SFX_CONFIRM);
			g_hand_pick = MSX2_SLOT_NONE;
			g_mode = M_IDLE;
		}
		break;

	case M_EQUIP:
		if(Msx2_PlaySupport(MSX2_OWNER_PLAYER, g_hand_pick, g_sel))
		{
			Msx2_SfxPlay(MSX2_SFX_CONFIRM);
			g_hand_pick = MSX2_SLOT_NONE;
			g_mode = M_IDLE;
		}
		break;

	case M_TARGET:
		if(Msx2_Attack(MSX2_OWNER_PLAYER, g_atk_pick, g_sel))
		{
			Msx2_SfxPlay(MSX2_SFX_LASER_SHOOT);
			g_atk_pick = MSX2_SLOT_NONE;
			g_mode = M_IDLE;
			g_zone = ZONE_FIELD;
		}
		break;

	default:
		break;
	}

	Msx2_BoardTouch();
}

// Which rows the cursor may visit right now.  In the middle of a placement or
// an attack the answer is one row, which is what makes those modes readable
// without a word of explanation.
static void Msx2_BoardMove(u8 pressed)
{
	u8 low = ZONE_COM, high = ZONE_HAND;

	if((g_mode == M_PLACE) || (g_mode == M_EQUIP))
		low = high = ZONE_FIELD;
	else if(g_mode == M_TARGET)
		low = high = ZONE_COM;

	if(pressed & MSX2_BTN_LEFT)
		g_sel = (u8)((g_sel == 0) ? (FIELD_ROW - 1) : (g_sel - 1));
	if(pressed & MSX2_BTN_RIGHT)
		g_sel = (u8)((g_sel + 1) % FIELD_ROW);

	if(g_mode == M_PLACE)
	{
		// The row is fixed, so up and down are free to mean the thing a player
		// actually has to decide here.
		if(pressed & (MSX2_BTN_UP | MSX2_BTN_DOWN))
		{
			g_place_def = !g_place_def;
			g_panel_left = MSX2_VIDEO_PAGES;
		}
		return;
	}

	if((pressed & MSX2_BTN_UP) && (g_zone > low))
		--g_zone;
	if(pressed & MSX2_BTN_DOWN)
	{
		if(g_zone < high)
			++g_zone;
		else if(g_zone == ZONE_HAND)
		{
			// Already on the bottom row, so down is free to mean the other
			// thing a hand card can be: material for a fusion chain.
			Msx2_BoardQueueToggle(g_sel);
			Msx2_SfxPlay(MSX2_SFX_SELECT);
			Msx2_BoardSnapshot();
			g_panel_left = MSX2_VIDEO_PAGES;
		}
	}
}

// ─────────────────────────────────────────────────────────────────────────────
//  The frame
// ─────────────────────────────────────────────────────────────────────────────

u8 Msx2_BoardStep_In(void)
{
	u8 pressed = Msx2_InputPressed();
	u8 before_zone = g_zone;
	u8 before_sel = g_sel;

	if((g_mode == M_OPENING) || (g_mode == M_TURN))
	{
		Msx2_BoardStepCameraMove();
		return MSX2_BOARD_BUSY;
	}

	if(g_mode == M_DEAL)
	{
		Msx2_BoardStepDeal();
		return MSX2_BOARD_BUSY;
	}

	if(g_mode == M_CHECK)
	{
		// Nothing else happens while a card is being read.  The rules have not
		// moved, so putting the board back is the cut-in's own restore.
		if(pressed & (MSX2_BTN_A | MSX2_BTN_B))
		{
			g_mode = M_IDLE;
			Msx2_BoardRestoreFromCutin();
		}
		return MSX2_BOARD_BUSY;
	}

	// Effects own the frame while they are on screen.  Input is intentionally
	// ignored, so a held button cannot skip the COM's card choice or a battle
	// result.
	if(Msx2_BoardRunFx())
		return MSX2_BOARD_BUSY;

	if(g_mode == M_OVER)
	{
#ifdef MSX2_DEBUG_AUTOPLAY
		// Nothing is going to press anything, and a soak that stops on the
		// first result screen measures one duel.
		pressed |= MSX2_BTN_A;
#endif
		if(pressed & (MSX2_BTN_A | MSX2_BTN_B))
		{
			Msx2_SpriteClear();
			return (g_duel.result > 0) ? MSX2_BOARD_WIN : MSX2_BOARD_LOSE;
		}
	}
	else if(g_mode == M_COM)
	{
		// One rules step per frame.  A step is already tens of milliseconds of
		// Z80 -- rebuilding WaifuAiState is most of it -- so this reads as the
		// opponent thinking rather than as a stall, and the board keeps
		// repainting underneath it.
		if((g_duel.turn_owner == MSX2_OWNER_COM) ||
		   (g_duel.phase == MSX2_PHASE_TURN_START))
		{
			Msx2_DuelStep();
			Msx2_BoardTouch();
			if((g_duel.turn_owner == MSX2_OWNER_PLAYER) &&
			   (g_duel.phase != MSX2_PHASE_TURN_START) &&
			   (g_duel.result == 0))
			{
				// The COM has finished its rules work AND the player's own turn
				// start has run.  Both halves matter: the step that flips the
				// owner over is the COM's end-of-turn, and the player's draw is
				// the NEXT one.  Handing the camera back on the first of the two
				// left the rules parked in TURN_START for the whole turn --
				// nothing else steps them once the board is idle -- so a hand
				// that had spent a card stayed one card short, and the turn
				// after that two.  It is one extra frame in the COM chair.
				g_zone = ZONE_HAND;
				g_sel = 0;
				Msx2_BoardSwitchView(BOARD_VIEW_PLAYER, FALSE);
			}
		}
		else
		{
			if(g_duel.result == 0)
			{
				g_zone = ZONE_HAND;
				g_sel = 0;
				Msx2_BoardSwitchView(BOARD_VIEW_PLAYER, FALSE);
			}
			else
			{
				g_mode = M_IDLE;
				g_panel_left = MSX2_VIDEO_PAGES;
			}
		}
	}
	else
	{
#ifdef MSX2_DEBUG_AUTOPLAY
		// The blind soak (./msx2.sh verify) has no hands.  With this defined the
		// player's turn is played by the same AI the COM uses, which keeps the
		// M1a evidence -- hundreds of complete duels with no input -- available
		// on a build that also draws the board.
		Msx2_DuelStep();
		Msx2_BoardTouch();
		if(g_duel.turn_owner == MSX2_OWNER_COM)
		{
			g_mode = M_COM;
			Msx2_BoardSwitchView(BOARD_VIEW_COM, TRUE);
		}
#else
		if(pressed & (MSX2_BTN_LEFT | MSX2_BTN_RIGHT | MSX2_BTN_UP | MSX2_BTN_DOWN))
			Msx2_BoardMove(pressed);

		if((Msx2_InputTyped() == 'C') && (g_mode == M_IDLE))
		{
			Msx2_BoardCheck();
			return MSX2_BOARD_BUSY;
		}

		if(pressed & MSX2_BTN_A)
			Msx2_BoardConfirm();
		else if(pressed & MSX2_BTN_B)
		{
			if(g_mode != M_IDLE)
				Msx2_BoardCancel();
			else if(g_queue_n != 0)
			{
				g_queue_n = 0;
				Msx2_BoardTouch();
			}
			else
			{
				Msx2_EndTurn();
				g_queue_n = 0;
				g_mode = M_COM;
				Msx2_BoardSwitchView(BOARD_VIEW_COM, TRUE);
			}
		}
#endif
	}

	if(g_refuse != 0)
	{
		--g_refuse;
		if((g_refuse == 0) || (g_refuse == 95))
			g_panel_left = MSX2_VIDEO_PAGES;
	}
	if((g_zone != before_zone) || (g_sel != before_sel))
	{
		Msx2_SfxPlay(MSX2_SFX_SELECT);
		g_panel_left = MSX2_VIDEO_PAGES;
	}
	if((g_fx_kind == FX_NONE) && (g_mode != M_TURN) && (g_mode != M_COM) &&
	   (g_mode != M_DEAL) && (g_mode != M_OVER) && (g_view != BOARD_VIEW_COM))
	{
		bool in_hand = (g_zone == ZONE_HAND);
		Msx2_BoardHandVisible(in_hand);
		Msx2_BoardCutTo(in_hand ? BOARD_VIEW_PLAYER : MSX2_VIEW_OVER);
	}
	if(g_mode == M_TURN)
	{
		Msx2_BoardStepCameraMove();
		return MSX2_BOARD_BUSY;
	}

	if((g_duel.result != 0) && (g_mode != M_OVER))
	{
		g_mode = M_OVER;
		Msx2_BoardTouch();
		// The result, in letters the size of the cards.  A word built out of
		// sprites floats over both pages and over the arena without a pixel of
		// it being drawn into either, which is the only way this screen can
		// say it big and still put the board back underneath.
		if(g_duel.result > 0)
			Msx2_SpriteShowWord(Msx2_UiText(MSX2_S_YOU_WIN), 70, MSX2_SPR_GOLD);
		else
			Msx2_SpriteShowWord(Msx2_UiText(MSX2_S_YOU_LOSE), 70, MSX2_SPR_RED);
	}

	Msx2_BoardStartFx();
	if(Msx2_BoardRunFx())
		return MSX2_BOARD_BUSY;

	if(Msx2_BoardPaint())
		Msx2_VideoFlipRequest();

	return MSX2_BOARD_BUSY;
}
