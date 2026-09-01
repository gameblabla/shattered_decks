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
#include "msx2_scenes.h"

// ── Geometry ─────────────────────────────────────────────────────────────────
//
// The three rows are one array so a slot is a single number 0..14 everywhere
// below: 0-4 the COM monsters, 5-9 the player's, 10-14 the player's hand.  The
// coordinates themselves are generated (src/generated/msx2_scenes.h) from the
// same constants the backdrop was baked with.

#define ZONE_COM     0
#define ZONE_FIELD   1
#define ZONE_HAND    2
#define SLOT_COUNT   (3 * MSX2_SLOTS)

#define SLOT_X(i)         (u8)(MSX2_SLOT_X0 + (u8)(i) * MSX2_SLOT_PITCH)
#define SLOT_OF(z, i)     (u8)((u8)(z) * MSX2_SLOTS + (u8)(i))
#define SLOT_ZONE(s)      (u8)((s) / MSX2_SLOTS)
#define SLOT_INDEX(s)     (u8)((s) % MSX2_SLOTS)

static const u8 g_row_y[3] = { MSX2_ROW_COM_Y, MSX2_ROW_PLAYER_Y, MSX2_ROW_HAND_Y };

// Slot flags, tracked alongside the card id so a repaint knows a face-down
// card and a face-up one are different pictures.
#define F_FACEUP     0x01
#define F_DEFENSE    0x02

// ── Interaction ──────────────────────────────────────────────────────────────
#define M_IDLE       0   // walking the board, nothing chosen
#define M_PLACE      1   // a monster is chosen, picking the field slot
#define M_EQUIP      2   // an equip is chosen, picking the monster
#define M_TARGET     3   // an attacker is chosen, picking the defender
#define M_COM        4   // the COM turn, one rules step at a time
#define M_OVER       5   // the duel is decided

static u8  g_stage;
static u8  g_mode;
static u8  g_zone;
static u8  g_sel;
static u8  g_hand_pick;
static u8  g_atk_pick;
static u8  g_place_def;

// What the board should look like, and what each page is actually showing.
static u8  g_want[SLOT_COUNT];
static u8  g_flag[SLOT_COUNT];
static u8  g_shown[MSX2_VIDEO_PAGES][SLOT_COUNT];
static u8  g_shown_flag[MSX2_VIDEO_PAGES][SLOT_COUNT];
static u8  g_cursor_at[MSX2_VIDEO_PAGES];
static u8  g_cursor_col_at[MSX2_VIDEO_PAGES];
static u8  g_panel_left;         // pages still owing a HUD/info repaint

static c8  g_name[MSX2_NAME_STRIDE];

// ─────────────────────────────────────────────────────────────────────────────
//  Reading the rules
// ─────────────────────────────────────────────────────────────────────────────

u8 Msx2_BoardStageForStory(u8 story_duel_index)
{
	// story_scene_for_progress() in src/main.c, by another name.
	if(story_duel_index == MSX2_STORY_NONE)
		return 0;
	if(story_duel_index >= 4) return 3;
	if(story_duel_index >= 3) return 2;
	if(story_duel_index >= 2) return 1;
	return 0;
}

// Rebuild the wanted picture from the duel state.  Called after every action;
// the paint pass works out what that actually costs.
static void Msx2_BoardSnapshot(void)
{
	u8 i;
	for(i = 0; i < MSX2_SLOTS; ++i)
	{
		const Msx2Side* com = &g_duel.side[MSX2_OWNER_COM];
		const Msx2Side* you = &g_duel.side[MSX2_OWNER_PLAYER];

		g_want[SLOT_OF(ZONE_COM, i)] = com->field[i];
		g_flag[SLOT_OF(ZONE_COM, i)] = (u8)((com->faceup[i] ? F_FACEUP : 0)
		                                  | (com->defense[i] ? F_DEFENSE : 0));

		g_want[SLOT_OF(ZONE_FIELD, i)] = you->field[i];
		g_flag[SLOT_OF(ZONE_FIELD, i)] = (u8)(F_FACEUP
		                                  | (you->defense[i] ? F_DEFENSE : 0));

		g_want[SLOT_OF(ZONE_HAND, i)] = you->hand[i];
		g_flag[SLOT_OF(ZONE_HAND, i)] = F_FACEUP;
	}
}

// The card the cursor is over, for the info panel.
static u8 Msx2_BoardHovered(void)
{
	u8 slot = SLOT_OF(g_zone, g_sel);
	if((g_zone == ZONE_COM) && !(g_flag[slot] & F_FACEUP))
		return MSX2_CARD_NONE;          // a set monster gives nothing away
	return g_want[slot];
}

// ─────────────────────────────────────────────────────────────────────────────
//  Drawing
// ─────────────────────────────────────────────────────────────────────────────

// One card face, straight out of the cartridge.  A face-down monster is the
// card back, which is simply the last texture in the blob.
static void Msx2_BoardBlitSlot(u8 slot)
{
	u8 x = SLOT_X(SLOT_INDEX(slot));
	u8 y = g_row_y[SLOT_ZONE(slot)];
	u8 card = g_want[slot];
	u16 index;

	if(card == MSX2_CARD_NONE)
	{
		// Empty: the backdrop's own pixels for this slot on this arena.  A fill
		// would flatten the floor the board is standing on.
		index = (u16)g_stage * MSX2_SLOT_ART_PER_STAGE + slot;
		Msx2_StreamRect((u16)(MSX2_SLOT_ART_SEGMENT + index / MSX2_SLOT_ART_PER_SEG),
		                (u16)((index % MSX2_SLOT_ART_PER_SEG) * MSX2_SLOT_ART_STRIDE),
		                x, y, MSX2_CARD_W, MSX2_CARD_H);
		return;
	}

	index = (g_flag[slot] & F_FACEUP) ? card : MSX2_CARD_BACK_INDEX;
	Msx2_StreamRect((u16)(MSX2_CARD_ART_SEGMENT + index / MSX2_CARD_ART_PER_SEG),
	                (u16)((index % MSX2_CARD_ART_PER_SEG) * MSX2_CARD_ART_STRIDE),
	                x, y, MSX2_CARD_W, MSX2_CARD_H);

	// Defence position.  The other targets turn the card sideways; a Z80 cannot
	// rotate a bitmap for free, so the card says so instead -- written over the
	// stat band, which carries no information at this size anyway.
	if(g_flag[slot] & F_DEFENSE)
	{
		Msx2_Fill((u8)(x + 4), (u8)(y + 39), MSX2_CARD_W - 8, 7, MSX2_DEEP_BLUE);
		Msx2_TextColor(MSX2_WHITE, MSX2_DEEP_BLUE);
		Msx2_TextAt((u8)(x + 11), (u8)(y + 39), "DEF");
	}
}

// The selection bracket, drawn into the flat ring the backdrop bakes around
// every slot.  Erasing it is the same four fills in MSX2_RING_COLOR, so no
// artwork ever has to be restored.
static void Msx2_BoardCursor(u8 slot, u8 color)
{
	u8 x = (u8)(SLOT_X(SLOT_INDEX(slot)) - MSX2_SLOT_RING);
	u8 y = (u8)(g_row_y[SLOT_ZONE(slot)] - MSX2_SLOT_RING);
	u8 w = MSX2_CARD_W + 2 * MSX2_SLOT_RING;
	u8 h = MSX2_CARD_H + 2 * MSX2_SLOT_RING;

	Msx2_Fill(x, y, w, MSX2_SLOT_RING, color);
	Msx2_Fill(x, (u8)(y + h - MSX2_SLOT_RING), w, MSX2_SLOT_RING, color);
	Msx2_Fill(x, y, MSX2_SLOT_RING, h, color);
	Msx2_Fill((u8)(x + w - MSX2_SLOT_RING), y, MSX2_SLOT_RING, h, color);
}

// Gold to choose with, red to attack with: the cursor colour is the only place
// the mode is stated without words.
static u8 Msx2_BoardCursorColor(void)
{
	if(g_mode == M_TARGET)
		return MSX2_RED;
	if((g_mode == M_PLACE) || (g_mode == M_EQUIP))
		return MSX2_TEAL;
	// White, not gold: every card already has a gold frame, and a gold cursor
	// on top of one is invisible.
	return MSX2_WHITE;
}

static void Msx2_BoardHud(void)
{
	Msx2_Fill(1, 1, MSX2_SCREEN_W - 2, MSX2_HUD_H - 2, MSX2_BLACK);

	Msx2_TextColor(MSX2_RED, MSX2_BLACK);
	Msx2_TextAt(4, 3, "COM");
	Msx2_NumAt(28, 3, g_duel.side[MSX2_OWNER_COM].lp);

	Msx2_TextColor(MSX2_GOLD, MSX2_BLACK);
	Msx2_TextAt(100, 3, "TURN");
	Msx2_NumAt(130, 3, (i16)g_duel.turns);

	Msx2_TextColor(MSX2_TEAL, MSX2_BLACK);
	Msx2_TextAt(196, 3, "YOU");
	Msx2_NumAt(220, 3, g_duel.side[MSX2_OWNER_PLAYER].lp);
}

static const c8* Msx2_BoardPrompt(void)
{
	switch(g_mode)
	{
	case M_PLACE:  return g_place_def ? "PLACE IN DEFENCE - UP/DOWN ATK"
	                                  : "PLACE IN ATTACK - UP/DOWN DEF";
	case M_EQUIP:  return "PICK A MONSTER TO EQUIP";
	case M_TARGET: return "PICK THE TARGET - ESC CANCELS";
	case M_COM:    return "THE OPPONENT IS THINKING";
	case M_OVER:   return "SPACE RETURNS TO THE TITLE";
	default:
		if(g_zone == ZONE_HAND)  return "SPACE PLAYS  -  ESC ENDS TURN";
		if(g_zone == ZONE_FIELD) return "SPACE ATTACKS  -  ESC ENDS TURN";
		return "OPPONENT ROW - DOWN TO GO BACK";
	}
}

static void Msx2_BoardInfo(void)
{
	u8 card = Msx2_BoardHovered();

	Msx2_Fill(1, (u8)(MSX2_INFO_Y + 1), MSX2_SCREEN_W - 2,
	          MSX2_SCREEN_H - MSX2_INFO_Y - 2, MSX2_BLACK);

	if(g_mode == M_OVER)
	{
		Msx2_TextColor((g_duel.result > 0) ? MSX2_GOLD : MSX2_RED, MSX2_BLACK);
		Msx2_TextCenter((u8)(MSX2_INFO_Y + 4),
		                (g_duel.result > 0) ? "YOU WIN THE DUEL" : "YOU HAVE LOST");
	}
	else if(card != MSX2_CARD_NONE)
	{
		// Names live in the cartridge, not in the 32 KB of code: 78 of them at
		// a fixed stride is 1.8 KB the link cannot spare.
		Msx2_RomRead(MSX2_TEXT_SEGMENT, (u16)card * MSX2_NAME_STRIDE,
		             (u8*)g_name, MSX2_NAME_STRIDE);
		g_name[MSX2_NAME_STRIDE - 1] = 0;
		Msx2_TextColor(MSX2_WHITE, MSX2_BLACK);
		Msx2_TextCenter((u8)(MSX2_INFO_Y + 4), g_name);

		if(Msx2_IsMonster(card))
		{
			u8 slot = SLOT_OF(g_zone, g_sel);
			Msx2_TextColor(MSX2_GOLD, MSX2_BLACK);
			Msx2_TextAt(56, (u8)(MSX2_INFO_Y + 14), "ATK");
			Msx2_TextAt(140, (u8)(MSX2_INFO_Y + 14), "DEF");
			Msx2_TextColor(MSX2_WHITE, MSX2_BLACK);
			// On the board the equips count; in the hand there is nothing to
			// equip yet, so the printed figure is the card's own.
			if(g_zone == ZONE_HAND)
			{
				Msx2_NumAt(82, (u8)(MSX2_INFO_Y + 14), (i16)Msx2_CardAtk(card));
				Msx2_NumAt(166, (u8)(MSX2_INFO_Y + 14), (i16)Msx2_CardDef(card));
			}
			else
			{
				u8 owner = (g_zone == ZONE_COM) ? MSX2_OWNER_COM : MSX2_OWNER_PLAYER;
				Msx2_NumAt(82, (u8)(MSX2_INFO_Y + 14),
				           Msx2_FieldAtk(owner, SLOT_INDEX(slot)));
				Msx2_NumAt(166, (u8)(MSX2_INFO_Y + 14),
				           Msx2_FieldDef(owner, SLOT_INDEX(slot)));
			}
		}
		else
		{
			Msx2_TextColor(MSX2_TEAL, MSX2_BLACK);
			Msx2_TextCenter((u8)(MSX2_INFO_Y + 14), "SUPPORT CARD");
		}
	}

	Msx2_TextColor(MSX2_SAND, MSX2_BLACK);
	Msx2_TextCenter((u8)(MSX2_INFO_Y + 24), Msx2_BoardPrompt());
}

// Repaint whatever the page in front of us is not showing.  Bounded: a card is
// about a frame of VDP time, so at most two per frame, and the rest waits.
static bool Msx2_BoardPaint(void)
{
	u8 page = Msx2_VideoGetDrawPage();
	u8 cursor = SLOT_OF(g_zone, g_sel);
	u8 color = Msx2_BoardCursorColor();
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

	for(i = 0; (i < SLOT_COUNT) && (cards < 2); ++i)
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
			Msx2_BoardCursor(g_cursor_at[page], MSX2_RING_COLOR);
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

void Msx2_BoardEnter(u8 stage)
{
	u8 i;

	g_stage = stage;
	g_mode = M_IDLE;
	g_zone = ZONE_HAND;
	g_sel = 0;
	g_hand_pick = MSX2_SLOT_NONE;
	g_atk_pick = MSX2_SLOT_NONE;
	g_place_def = FALSE;
	Msx2_BoardSnapshot();

	// Compose page 1 whole -- the backdrop stream alone is far longer than a
	// frame -- then hand page 0 a copy of it with one HMMM rather than doing
	// all of it twice.
	Msx2_VideoDrawPage(MSX2_PAGE_1);
	Msx2_StreamScene(MSX2_BOARD_SEGMENT(stage), MSX2_PAGE_1);
	for(i = 0; i < SLOT_COUNT; ++i)
		if(g_want[i] != MSX2_CARD_NONE)
			Msx2_BoardBlitSlot(i);
	Msx2_BoardHud();
	Msx2_BoardInfo();
	Msx2_BoardCursor(SLOT_OF(g_zone, g_sel), Msx2_BoardCursorColor());
	Msx2_VideoCopyPage(MSX2_PAGE_1, MSX2_PAGE_0);
	Msx2_VideoShowPage(MSX2_PAGE_1);

	// Both pages now hold the identical picture, so from here a repaint only
	// has to touch the hidden one.
	for(i = 0; i < SLOT_COUNT; ++i)
	{
		g_shown[0][i] = g_shown[1][i] = g_want[i];
		g_shown_flag[0][i] = g_shown_flag[1][i] = g_flag[i];
	}
	g_cursor_at[0] = g_cursor_at[1] = SLOT_OF(g_zone, g_sel);
	g_cursor_col_at[0] = g_cursor_col_at[1] = Msx2_BoardCursorColor();
	g_panel_left = 0;
}

// ─────────────────────────────────────────────────────────────────────────────
//  The player's turn
// ─────────────────────────────────────────────────────────────────────────────

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
		else if(g_zone == ZONE_FIELD)
		{
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
		g_sel = (u8)((g_sel == 0) ? (MSX2_SLOTS - 1) : (g_sel - 1));
	if(pressed & MSX2_BTN_RIGHT)
		g_sel = (u8)((g_sel + 1) % MSX2_SLOTS);

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
	if((pressed & MSX2_BTN_DOWN) && (g_zone < high))
		++g_zone;
}

// ─────────────────────────────────────────────────────────────────────────────
//  The frame
// ─────────────────────────────────────────────────────────────────────────────

u8 Msx2_BoardStep(void)
{
	u8 pressed = Msx2_InputPressed();
	u8 before_zone = g_zone;
	u8 before_sel = g_sel;

	if(g_mode == M_OVER)
	{
#ifdef MSX2_DEBUG_AUTOPLAY
		// Nothing is going to press anything, and a soak that stops on the
		// first result screen measures one duel.
		pressed |= MSX2_BTN_A;
#endif
		if(pressed & (MSX2_BTN_A | MSX2_BTN_B))
			return (g_duel.result > 0) ? MSX2_BOARD_WIN : MSX2_BOARD_LOSE;
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
		}
		else
		{
			g_mode = M_IDLE;
			g_zone = ZONE_HAND;
			g_sel = 0;
			g_panel_left = MSX2_VIDEO_PAGES;
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
			g_mode = M_COM;
#else
		if(pressed & (MSX2_BTN_LEFT | MSX2_BTN_RIGHT | MSX2_BTN_UP | MSX2_BTN_DOWN))
			Msx2_BoardMove(pressed);

		if(pressed & MSX2_BTN_A)
			Msx2_BoardConfirm();
		else if(pressed & MSX2_BTN_B)
		{
			if(g_mode != M_IDLE)
				Msx2_BoardCancel();
			else
			{
				Msx2_EndTurn();
				g_mode = M_COM;
				Msx2_BoardTouch();
			}
		}
#endif
	}

	if((g_zone != before_zone) || (g_sel != before_sel))
	{
		Msx2_SfxPlay(MSX2_SFX_SELECT);
		g_panel_left = MSX2_VIDEO_PAGES;
	}

	if((g_duel.result != 0) && (g_mode != M_OVER))
	{
		g_mode = M_OVER;
		Msx2_BoardTouch();
	}

	if(Msx2_BoardPaint())
		Msx2_VideoFlipRequest();

	return MSX2_BOARD_BUSY;
}
