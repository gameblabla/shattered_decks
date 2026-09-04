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
// Slots 0..19 are the field, in the order the capture emitted them: 0-4 the COM
// monster row, 5-9 the player's, 10-14 the COM's SUPPORT row and 15-19 the
// player's.  Slots 20..24 are the player's hand, which is a flat HUD strip
// rather than board geometry.  The field quads and their boxes come out of the
// capture (src/generated/msx2_scenes.h) -- there is no second definition of
// where a card goes, which is the whole point of §4.3.
//
// THE SUPPORT ROWS ARE ROWS OF THE BOARD, NOT A LIST BESIDE IT.
// An equip or a set trap lives at ENEMY_CARD_ROW-1 / PLAYER_CARD_ROW+1 on every
// other target, and it is drawn there in perspective like any other card.  This
// port used to have nowhere to put one: an equip flew a card across the board
// and then vanished, so a duel could be decided by cards that were never on the
// screen.  The capture now projects all four rows, so they are ordinary slots
// with ordinary quads, tiles and span programs.
//
// They are display-only.  Nothing the player does targets a support slot -- an
// equip is aimed at the MONSTER it attaches to -- so the cursor still walks
// COM -> FIELD -> HAND and never stops on one.  That is why the two support
// zones sit BETWEEN the field and the hand in the numbering: the numbering is
// the slot layout the capture emitted, and the cursor's three rows are picked
// out of it by name rather than by counting.
#define ZONE_COM      0
#define ZONE_FIELD    1
#define ZONE_COM_SUP  2
#define ZONE_SUP      3
#define ZONE_HAND     4
#define FIELD_ROW    (MSX2_FIELD_SLOTS / 4)
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
// A value no real flag word can hold (the queue lives in the high nibble and
// only bits 0 and 1 are used below it), written into g_shown_flag[] to mean
// "this page is showing something that is no longer right, whatever it thinks".
#define F_STALE      0x08
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
// left to right so a card never crosses one that has already landed.
// g_deal_target_mask is the logical hand occupancy captured at turn start;
// g_deal_visible_mask is the subset the retained painter may show while the
// deal is in flight, and each page has its own completion mask.  A positional
// reveal frontier cannot describe a hand with a hole, or prove that both
// buffers have received the same settled card.
// A deal step is a black fill, up to two settled cards put
// back where the erase box crossed them and one 40x48 card streamed out of the
// cartridge -- four kilobytes of VDP, which is several video frames.  Four
// poses a card is what keeps a five-card deal about a second; six was nearly
// four seconds of watching cards crawl in from the right.
#define DEAL_STEPS   3
#define DEAL_START_X 216
static u8  g_deal_slot;
static u8  g_deal_step;
static u8  g_deal_reveal;
static u8  g_deal_px[MSX2_VIDEO_PAGES];
static u8  g_deal_target_mask;
static u8  g_deal_visible_mask;
static u8  g_deal_landed_mask[MSX2_VIDEO_PAGES];

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
static u8  g_refuse_text;   // which refusal the prompt line is showing

// What the board should look like, and what each page is actually showing.
static u8  g_want[SLOT_COUNT];
static u8  g_flag[SLOT_COUNT];
static u8  g_shown[MSX2_VIDEO_PAGES][SLOT_COUNT];
static u8  g_shown_flag[MSX2_VIDEO_PAGES][SLOT_COUNT];
// The selector's animation phase.  It is a sprite, so it is not per page and
// it is not journalled: it floats over both, changes with one attribute write,
// and nothing under it is ever disturbed.
static u8  g_gem_tick;
// THE PANEL'S DIRTY STATE IS A PAGE MASK, NOT A COUNT.
// It used to be a countdown of "pages still owing a repaint", decremented by
// every Msx2_BoardPaint().  Two of the callers below run the painter several
// times in a row on the SAME page, which spent both credits there and left the
// other page holding the previous panel for the rest of the duel: that is why
// the turn number read 2 on one flip and 3 on the next, for a whole turn.  A
// bit per page cannot be spent twice on one page.
//
// It is also three masks rather than one.  Walking the hand changes the card
// the panel names and nothing else -- not the life points, not the prompt --
// and repainting all three regions is what made moving the selector answer a
// couple of frames late.
#define PAGES_ALL      ((u8)((1u << MSX2_VIDEO_PAGES) - 1u))
#define PAGE_BIT(p)    ((u8)(1u << (p)))
#define PANEL_ALL()    (g_hud_left = g_card_left = g_prompt_left = PAGES_ALL)

static u8  g_hud_left;           // pages still owing the top strip
static u8  g_card_left;          // pages still owing the name/ATK/DEF lines
static u8  g_prompt_left;        // pages still owing the bottom prompt line
static u8  g_hand_left;          // pages still owing the whole hand strip

static c8  g_name[MSX2_NAME_STRIDE];

// ── The result banner ───────────────────────────────────────────────────────
// The word is held here rather than being read out of the cartridge each frame:
// Msx2_UiText() answers out of ONE shared buffer that every panel painter
// overwrites, so a pointer into it does not survive the frame it was taken in.
#define OVER_SLIDE_STEPS 30
// AND THEN IT STAYS.  The word arriving is not the beat -- the beat is the
// board it arrived over, with everything the last attack left standing on it,
// and a slide that ends the instant it lands gives a player no time to look at
// either.  The duel loop runs at about thirty-five iterations a second, so this
// is a little over two seconds before a button is worth anything.
#define OVER_HOLD_STEPS  76
// The band of bare table between the HUD and the opponent's row.  The two card
// rows overhead are flush -- 54..96 and 96..138 -- so there is no gap between
// them to put a thirty-two row word in, and the middle of the board is the one
// place a result banner must NOT be: the board it is announcing is the whole
// point of the screen, and every card has to stay readable under it.
#define MSX2_OVER_WORD_Y 18
static c8  g_over_text[MSX2_SPR_LETTER_N + 1];
static u8  g_over_n;
static u8  g_over_color;
static u8  g_over_step;

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
// (there is no FX for a position change: see Msx2_BoardStartFx)
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
// The hand position the opponent's wind-up beat is pointing at.  The rules
// have already taken that card out of the hand, so the snapshot reports the
// position empty -- and the beat exists to say WHICH card came out of it, so
// the cover is held in place for as long as the selector stands on it.
static u8 g_hold_hand;
static u8 g_fx_page_frame[MSX2_VIDEO_PAGES];

// A card landing on the field takes the hand off the screen, slides the real
// 2-D thumbnail across the emptied band, bends the board under it and then
// holds the bare top view while the new card is read.  These carry that.
// One per page: a bend step is allowed to touch the hidden page only, so it
// takes two of them to put the landing on both.
#define FX_BEND_POSES  2
// Every landing flight is the same length, so one easing curve serves them all.
// Eight poses, not fourteen: a pose is a 40x48 stream out of the cartridge plus
// a save and a restore of what was under it, which is nearly two video frames
// of VDP on its own.  Fourteen of those, then a bend, then twenty-four frames
// of hold, was the best part of two seconds to put one card down.
#define FX_LANDING_FRAMES 8
#define FX_HOLD_FRAMES 6
// The equip banner, which has to stand on its own now that no card flies.
#define FX_EQUIP_FRAMES 26
static u8 g_hand_hidden;
// Did this landing take the hand off the screen?  Only then is the strip owed
// back when the hold ends: in the top view those rows are board, and putting
// five hand positions on them punches the table full of holes.
static u8 g_fx_hand_back;
static u8 g_fx_bend;
static u8 g_fx_hold;
// These are the actual rectangle origins used by the flight.  They are
// snapped when the effect starts; they must not be inferred later from the
// hand-visibility flag, because walking from the hand to the field hides the
// hand before the rules action is presented.
static u8 g_fx_start_x;
static u8 g_fx_start_y;
static u8 g_fx_dest_x;
static u8 g_fx_dest_y;

// THE 2-D BATTLE CUT-IN: THE CARDS STAND STILL AND THE ATTACK IS THE ANIMATION.
//
// The cards used to close the gap between them -- five 88x120 blits forward and
// five back, on each of two pages, before anything happened.  That is about six
// V-blanks a step for a beat that says nothing the lanes do not already say, and
// it cost the strike itself the frames it needed.  So the attacker is parked in
// the left lane and what it is striking in the right one, neither moves, and the
// whole of the animation is the blade sweep and the burst landing on the right
// lane -- with the damage climbing under it from -0 to the real figure.  This is
// what the PC build does (draw_direct_attack_slash() plus its damage readout);
// the only thing the console version was missing was the blade.
#define BATT_HOLD    56
// The struck lane's centre, which is where every strike lands: a direct attack
// has nothing in the right lane and drives at it anyway, and a trap turns the
// strike back onto the attacker's own lane instead.
#define BATT_LANE_L  12
#define BATT_LANE_R  156
#define BATT_HIT_X(lane)  (u8)((lane) + MSX2_BATTLE_CARD_W / 2)
static u8 g_batt_phase;
static u8 g_batt_step;
static u8 g_batt_ax;
static u8 g_batt_dx;
static u8 g_batt_fx_x;      // where the blade, the burst and the figure land
static u8 g_batt_direct;
static u8 g_batt_trap;
static i16 g_batt_damage;   // 0 when there is no figure to count

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

// A support card on the row: face up unless it is a trap, which is set.
// Msx2_PlaySupport() puts nothing else on the row, and the rules take a trap
// off it on the frame it fires, so a face-down card here is always a trap that
// has not gone off yet.
static bool Msx2_BoardSupportFaceUp(u8 card)
{
	return Msx2_IsSupport(card) &&
	       (Msx2_SupportKind(card) != MSX2_SUP_TRAP);
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

		// The player's own row reads its face-up flag now, exactly as the
		// opponent's does.  It used to be hard-wired face up, which is why a
		// monster the player had just set still showed its face on the board
		// while the rules -- and every other port -- had it hidden.  Reading
		// what is under it is still the player's privilege: the info panel
		// redacts a set card in ZONE_COM and nowhere else.
		g_want[SLOT_OF(ZONE_FIELD, i)] = you->field[i];
		g_flag[SLOT_OF(ZONE_FIELD, i)] = (u8)((you->faceup[i] ? F_FACEUP : 0)
		                                  | (you->defense[i] ? F_DEFENSE : 0));

		// The support rows.  An equip is played openly and shows its face; a
		// trap is SET, so it shows the common back until it fires -- and the
		// rules take it off the row on the frame it does, so a face-down card
		// on this row is always a trap that has not gone off yet.  Neither is
		// ever in defence position: the row has no position to be in.
		g_want[SLOT_OF(ZONE_COM_SUP, i)] = com->equip_field[i];
		g_flag[SLOT_OF(ZONE_COM_SUP, i)] =
		    Msx2_BoardSupportFaceUp(com->equip_field[i]) ? F_FACEUP : 0;
		g_want[SLOT_OF(ZONE_SUP, i)] = you->equip_field[i];
		g_flag[SLOT_OF(ZONE_SUP, i)] =
		    Msx2_BoardSupportFaceUp(you->equip_field[i]) ? F_FACEUP : 0;

		// During a deal, only cards whose destination has completed are visible.
		// The mask is occupancy, not a left-to-right frontier, so a sparse hand
		// cannot make later cards appear early or make an earlier card vanish.
		g_want[SLOT_OF(ZONE_HAND, i)] =
		       (((g_mode == M_DEAL) ? (g_deal_visible_mask & (u8)(1u << i))
		                            : (u8)!g_hand_hidden) != 0)
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
	if(g_hold_hand != MSX2_SLOT_NONE)
	{
		g_want[g_hold_hand] = MSX2_CARD_BACK_INDEX;
		g_flag[g_hold_hand] = 0;
	}

	// AN EMPTY SLOT HAS NO FLAGS.
	// Face-up and defence describe a card, and the loop above sets them from
	// the duel's parallel arrays whether or not there is one in the slot -- so
	// an empty player field slot carried F_FACEUP, and an empty hand position
	// carried F_FACEUP too.  Every place that has just put a whole arena on the
	// screen tells the painter so by writing CARD_NONE and flag 0 into
	// g_shown[]; a stale flag on an empty slot made that comparison fail, and
	// the painter answered by stamping ten empty-slot tiles over a board that
	// was already showing exactly them.  Normalising here is what lets "this
	// slot is empty" be one value on both sides of the comparison.
	for(i = 0; i < SLOT_COUNT; ++i)
		if(g_want[i] == MSX2_CARD_NONE)
			g_flag[i] = 0;
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
// In a chair view a field slot is a projected quad, so the card is mapped into
// it by the §8 span rasterizer -- that is the whole of §0.3.1 in one call.
// Overhead the slot is a rectangle and so is the card: its own 32x42 texture,
// copied straight out of the cartridge.  A hand slot is a flat strip, and the
// same kind of copy at 40x48, which keeps the cards a player is choosing
// between at a readable size.
// The bare slot: the arena's own pixels for it on this stage, cut out of the
// quantised capture.  A fill would flatten the floor the board is standing on.
// It is what an empty slot shows, and what a card lying down leaves uncovered.
static void Msx2_BoardSlotGround(u8 slot)
{
	const u8* box = g_msx2_slot_box[g_view][slot];
	u16 tile = (u16)(((u16)g_stage * MSX2_BOARD_VIEWS + g_view)
	                 * MSX2_SLOT_ART_PER_VIEW + slot);
	Msx2_StreamRect((u16)(MSX2_SLOT_ART_SEGMENT + tile / MSX2_SLOT_ART_PER_SEG),
	                (u16)((tile % MSX2_SLOT_ART_PER_SEG) * MSX2_SLOT_ART_STRIDE),
	                box[0], box[1], box[2], box[3]);
}

// A slot's restore tile is its quad plus a four-pixel margin, and in a chair
// view the four board rows recede close enough together that the margin reaches
// into the same column of the row in front of it and the row behind.  Emptying
// a slot therefore takes a bite out of whatever is standing in those, and the
// retained painter would never put it back -- as far as it is concerned those
// slots still show exactly what they showed.  So tell it they do not.  It costs
// at most three extra card draws, and only on the frame a slot is emptied.
static void Msx2_BoardSlotGroundNeighbours(u8 slot)
{
	u8 page = Msx2_VideoGetDrawPage();
	u8 col = SLOT_INDEX(slot);
	u8 i;

	for(i = col; i < MSX2_FIELD_SLOTS; i = (u8)(i + FIELD_ROW))
		if((i != slot) && (g_want[i] != MSX2_CARD_NONE))
			g_shown_flag[page][i] |= F_STALE;
}

static void Msx2_BoardBlitSlot(u8 slot)
{
	u8  card = g_want[slot];
	u8  index;

	if(IS_HAND(slot))
	{
		u8 x = HAND_X(slot - MSX2_FIELD_SLOTS);

		if(card == MSX2_CARD_NONE)
		{
			// An empty hand position is just black: the band under it is black
			// and the positions carry no outline of their own.
			Msx2_Fill(x, MSX2_HAND_Y, MSX2_CARD_W, MSX2_CARD_H, MSX2_BLACK);
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
		Msx2_BoardSlotGround(slot);
		Msx2_BoardSlotGroundNeighbours(slot);
		return;
	}

	index = (g_flag[slot] & F_FACEUP) ? card : MSX2_CARD_BACK_INDEX;

	// A CARD LYING DOWN IS WIDER THAN THE SLOT IT STANDS IN.
	// The defence draw already puts the table back before it draws, because it
	// covers ground the upright card does not.  Coming back the other way was
	// never given the same treatment: the upright card was simply stamped over
	// the middle of the turned one and left its two ends on the board.  Turning
	// twice therefore left a card with wings.  Whichever way the position moves
	// the ground goes back first.
	if(((g_shown_flag[Msx2_VideoGetDrawPage()][slot] ^ g_flag[slot])
	    & F_DEFENSE) != 0)
	{
		Msx2_BoardSlotGround(slot);
		Msx2_BoardSlotGroundNeighbours(slot);
	}

	if(g_view == MSX2_VIEW_OVER)
	{
		// Overhead the slots are axis-aligned, so a card is a rectangle copy of
		// a texture baked at that size -- sharper than minifying the 40x48
		// board master through a span program, and a great deal quicker.  The
		// opponent's row reads the half-turned set: seen from above their cards
		// face their own chair.
		const u8* at = g_msx2_over_card_xy[slot];
		bool def = (g_flag[slot] & F_DEFENSE) != 0;
		// Both of the opponent's rows read the half-turned set: seen from
		// above, its monsters and its supports alike face its own chair.
		bool com_row = (SLOT_ZONE(slot) == ZONE_COM) ||
		               (SLOT_ZONE(slot) == ZONE_COM_SUP);
		u16 base = com_row
		         ? (def ? MSX2_OVER_DEF_MIRROR_SEGMENT : MSX2_OVER_CARD_MIRROR_SEGMENT)
		         : (def ? MSX2_OVER_DEF_SEGMENT : MSX2_OVER_CARD_SEGMENT);
		if(def)
		{
			// A card lying down is wider and shorter than the slot's upright
			// footprint, so the strip of table it uncovers above and below has
			// to be put back before it is drawn -- the slot's own restore tile
			// is cut wide enough for exactly this.
			const u8* box = g_msx2_slot_box[g_view][slot];
			Msx2_BoardSlotGround(slot);
			Msx2_BoardSlotGroundNeighbours(slot);
			Msx2_StreamRect((u16)(base + index / MSX2_OVER_CARD_PER_SEG),
			                (u16)((index % MSX2_OVER_CARD_PER_SEG)
			                      * MSX2_OVER_CARD_STRIDE),
			                box[0],
			                (u8)(at[1] + (MSX2_OVER_CARD_H - MSX2_OVER_CARD_W) / 2),
			                MSX2_OVER_CARD_H, MSX2_OVER_CARD_W);
			return;
		}
		Msx2_StreamRect((u16)(base + index / MSX2_OVER_CARD_PER_SEG),
		                (u16)((index % MSX2_OVER_CARD_PER_SEG)
		                      * MSX2_OVER_CARD_STRIDE),
		                at[0], at[1], MSX2_OVER_CARD_W, MSX2_OVER_CARD_H);
		return;
	}

	// Defence position in a chair view: the same quarter turn, done by the span
	// program and the pre-turned texture set rather than by printing "DEF" over
	// the card, which is what used to say it.
	Msx2_RasterSetView(g_view);
	Msx2_RasterCard(index, slot, (g_flag[slot] & F_DEFENSE) ? 1 : 0);
}

// ── The selector ────────────────────────────────────────────────────────────
//
// It used to be a rectangle drawn INTO the bitmap: a white frame round the
// chosen hand card, an outline round the chosen quad.  Both had to be erased
// again, on each page separately, by redrawing the exact colour that was under
// them -- which is why the hand row is drawn plainly, and why half a
// dozen places in this file had to remember where the cursor was on which page.
// (Every slot also carried a flat brown ring for the same reason.  It was paint
// over the arena that said nothing once the cursor stopped being drawn into it,
// so the generator does not bake one any more.)
//
// It is a sprite now: the spinning red gem the PC build draws beside the chosen
// card (draw_spin_cursor in src/main.c), projected offline into eight sprite
// patterns by tools/msx2/gen_msx_scenes.py.  A sprite floats over both GRAPHIC
// 7 pages, so there is one cursor rather than two, it moves for the cost of one
// attribute write, and nothing underneath it is ever touched -- which is what
// lets it animate at all on a Z80.
//
// Teal to place with, red otherwise: the colour is still the only place the
// mode is stated without words.
static u8 Msx2_BoardCursorColor(void)
{
	if((g_mode == M_PLACE) || (g_mode == M_EQUIP))
		return MSX2_SPR_TEAL;
	return MSX2_SPR_RED;
}

// The gem stands on the left edge of whatever the cursor is over, half on and
// half off it -- there are seven pixels between two hand cards, which is not
// room for a twenty-pixel gem beside one.
static void Msx2_BoardGemOnSlot(u8 slot, u8 color)
{
	u8 x, y;

	if(IS_HAND(slot))
	{
		x = HAND_X(slot - MSX2_FIELD_SLOTS);
		y = (u8)(MSX2_HAND_Y + (MSX2_CARD_H - MSX2_GEM_SCREEN) / 2);
	}
	else if(g_view == MSX2_VIEW_OVER)
	{
		// Not the slot's restore box overhead: that is cut wide enough for a
		// card lying down, and the gem would stand five pixels out in the
		// gutter instead of on the edge of the card it is selecting.
		const u8* at = g_msx2_over_card_xy[slot];
		u8 mid = (u8)(at[1] + MSX2_OVER_CARD_H / 2);
		x = at[0];
		y = (mid > MSX2_GEM_SCREEN / 2) ? (u8)(mid - MSX2_GEM_SCREEN / 2) : 0;
	}
	else
	{
		const u8* box = g_msx2_slot_box[g_view][slot];
		u8 mid = (u8)(box[1] + box[3] / 2);
		x = box[0];
		y = (mid > MSX2_GEM_SCREEN / 2) ? (u8)(mid - MSX2_GEM_SCREEN / 2) : 0;
	}
	x = (x > MSX2_GEM_SCREEN / 2) ? (u8)(x - MSX2_GEM_SCREEN / 2) : 0;

	++g_gem_tick;
	Msx2_SpriteGem(x, y, (u8)(g_gem_tick >> 2), color);
}

static void Msx2_BoardShowCursor(void)
{
	u8 slot = SLOT_OF(g_zone, g_sel);

	// THE OPPONENT POINTS WITH THE SAME CURSOR THE PLAYER DOES.
	// Its wind-up beat used to draw its own thing into the bitmap -- the card
	// cover restamped over the hand position plus an XOR frame flashing round
	// it -- which is the drawn-in selector this port stopped using everywhere
	// else, and had to be unpicked again on both pages afterwards.  The gem is
	// a sprite over both pages, so the beat is one attribute write and there is
	// nothing to erase.
	if(g_fx_kind == FX_COM_CHOOSE)
	{
		u8 at = (g_hold_hand != MSX2_SLOT_NONE) ? g_hold_hand : g_fx_field;
		if(at == MSX2_SLOT_NONE)
			Msx2_SpriteHideGem();
		else
			Msx2_BoardGemOnSlot(at, MSX2_SPR_RED);
		return;
	}

	if((g_fx_kind != FX_NONE) || (g_mode == M_COM) || (g_mode == M_TURN) ||
	   (g_mode == M_OPENING) || (g_mode == M_DEAL) || (g_mode == M_CHECK) ||
	   (g_mode == M_OVER) || (g_hand_hidden && IS_HAND(slot)))
	{
		Msx2_SpriteHideGem();
		return;
	}

	Msx2_BoardGemOnSlot(slot, Msx2_BoardCursorColor());
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
		return Msx2_UiText(g_refuse_text);
	switch(g_mode)
	{
	case M_PLACE:  return g_place_def ? Msx2_UiText(MSX2_S_PLACE_IN_DEFENCE_UP_DOWN_ATK)
	                                  : Msx2_UiText(MSX2_S_PLACE_IN_ATTACK_UP_DOWN_DEF);
	case M_EQUIP:  return Msx2_UiText(MSX2_S_PICK_A_MONSTER_TO_EQUIP);
	case M_TARGET: return Msx2_UiText(MSX2_S_PICK_THE_TARGET_ESC_CANCELS);
	case M_COM:    return Msx2_UiText(MSX2_S_THE_OPPONENT_IS_THINKING);
	case M_CHECK:  return Msx2_UiText(MSX2_S_SPACE_RETURNS_TO_THE_DUEL);
	case M_OVER:   return "";
	default:
		if(g_queue_n != 0)
		{
			if(g_zone == ZONE_HAND) return Msx2_UiText(MSX2_S_DOWN_PICKS_MATERIALS_ESC_CLE);
			return Msx2_UiText(MSX2_S_SPACE_FUSES_HERE_ESC_CLEARS);
		}
		if(g_zone == ZONE_HAND)  return Msx2_UiText(MSX2_S_SPACE_PLAYS_DOWN_FUSES_C_CHE);
		if(g_zone == ZONE_FIELD) return Msx2_UiText(MSX2_S_SPACE_ATK_X_TURN_ESC_PASS);
		return Msx2_UiText(MSX2_S_OPPONENT_ROW_SPACE_CHECKS);
	}
}

// The bottom panel is two regions, repainted independently: the card the
// cursor is over (its name and its figures) and the one-line prompt under it.
// Walking the hand changes only the first.
#define INFO_CARD_Y   (u8)(MSX2_INFO_Y + 1)
#define INFO_CARD_H   20
#define INFO_PROMPT_Y (u8)(MSX2_INFO_Y + 21)
#define INFO_PROMPT_H (u8)(MSX2_SCREEN_H - MSX2_INFO_Y - 22)

static void Msx2_BoardPromptLine(void)
{
	Msx2_Fill(1, INFO_PROMPT_Y, MSX2_SCREEN_W - 2, INFO_PROMPT_H,
	          MSX2_PANEL_COLOR);
	// Across a chair change there is no cursor and no prompt to give, except
	// the one line that says whose turn it is.
	if((g_mode == M_TURN) || (g_mode == M_DEAL) ||
	   ((g_view == BOARD_VIEW_COM) && (g_mode != M_COM)))
		return;
	Msx2_TextColor(MSX2_SAND, MSX2_PANEL_COLOR);
	Msx2_TextCenter(INFO_PROMPT_Y, Msx2_BoardPrompt());
}

static void Msx2_BoardCardLines(void)
{
	u8 card = Msx2_BoardHovered();

	Msx2_Fill(1, INFO_CARD_Y, MSX2_SCREEN_W - 2, INFO_CARD_H, MSX2_PANEL_COLOR);

	// THE TURN IS NOT THE PLAYER'S.
	// The panel names whatever the cursor is over, and across a chair change
	// there is no cursor: what stayed on it was the last card the player had
	// been reading, with its attack and defence, for the whole of the
	// opponent's turn.  It goes blank -- with one line, while the camera is
	// still, saying whose turn it now is.
	if((g_mode == M_TURN) || (g_mode == M_COM) || (g_mode == M_DEAL) ||
	   (g_view == BOARD_VIEW_COM))
		return;

	if(g_mode == M_OVER)
		return;
	if(card != MSX2_CARD_NONE)
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
}

// The whole panel below the board, for the paths that have just streamed a
// picture over it and owe every part of it back.
static void Msx2_BoardInfo(void)
{
	Msx2_BoardCardLines();
	Msx2_BoardPromptLine();
}

static bool Msx2_BoardPaint(void);

static void Msx2_BoardFxBanner(const c8* text, u8 color)
{
	Msx2_Fill(1, (u8)(MSX2_INFO_Y + 1), MSX2_SCREEN_W - 2,
	          MSX2_SCREEN_H - MSX2_INFO_Y - 2, MSX2_PANEL_COLOR);
	Msx2_TextColor(color, MSX2_PANEL_COLOR);
	Msx2_TextCenter((u8)(MSX2_INFO_Y + 3), text);
}

// The effects that put a card down on the board, and so get the hand-off-screen
// flight, the board bend and the bare top-view hold.
static bool Msx2_BoardFxIsLanding(void)
{
	// AN EQUIP IS NOT FLOWN ONTO THE BOARD ANY MORE.
	// It used to travel from the hand to the MONSTER it attaches to and then
	// stop existing, because there was nowhere on the board for it to land.
	// Its own support row is there now, so the rules put it down and the board
	// simply has it -- in perspective, in the second row, on whichever side
	// played it.  What is left of the beat is the banner naming it.
	return (g_fx_kind == FX_SUMMON) || (g_fx_kind == FX_COM_PLACE);
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
	// The attacker is in the left lane whoever owns it, and whatever it is
	// striking is in the right one.  Which chair the blow came from is already
	// said twice over -- by the two name lines and by the board the cut-in
	// returns to -- and a fixed pair of lanes is what lets the strike always
	// play in the same place.
	g_batt_ax = BATT_LANE_L;
	g_batt_dx = BATT_LANE_R;
	// A trap turns the attack back on the card that declared it, so that is the
	// lane the blade falls on.
	g_batt_fx_x = g_batt_trap ? BATT_HIT_X(g_batt_ax) : BATT_HIT_X(g_batt_dx);
	g_batt_damage = g_batt_trap ? 0 : (i16)g_duel.last_battle.damage;
	g_batt_phase = 0;
	g_batt_step = 0;

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

// One pose of the strike: the blade sweep, the explosion over it, and the damage
// figure climbing under the lane the blade fell on.
//
// The first two are sprites and cost the picture nothing, which is why this no
// longer begins by levelling the hidden page from the other one -- sixty
// thousand pixels a frame, sixteen frames, for a beat that never changed the
// stage.  The figure is the one bitmap part, so it is written on the draw page
// and only allowed to change on every SECOND step: each value then lands on
// both pages before the next one, and the number stops flickering between two
// readings.
static void Msx2_BoardBattleStrike(u8 step)
{
	Msx2_BattleFxSlash(g_batt_fx_x, step);
	if(step >= MSX2_BATTLE_SLASH_STEPS)
		Msx2_BattleFxBurst(g_batt_fx_x, 83,
		                   (u8)(step - MSX2_BATTLE_SLASH_STEPS));
	if(g_batt_damage > 0)
	{
		// -0 on the frame the blade lands, the real figure on the last pair.
		// The count is (damage / 8) * t rather than (damage * t) / 8: the
		// second form reaches 64000 on the way and the Z80 multiplies in
		// sixteen bits.  The last pair IS the figure, so the eighths never have
		// to add back up to it.
		u8 t = (u8)((step >> 1) + 1);
		Msx2_VideoDrawPage((u8)(Msx2_VideoGetShowPage() ^ 1));
		Msx2_BattleFxDamageCount(g_batt_fx_x,
		    (t >= (MSX2_BATTLE_COUNT_STEPS / 2))
		        ? g_batt_damage
		        : (i16)((g_batt_damage / (MSX2_BATTLE_COUNT_STEPS / 2)) * t));
	}
}

// The shared/PC-FX cut-in burns a destroyed card away instead of leaving it
// whole until the board cut.  SCREEN 8 has no alpha or palette fade, so the
// MSX2 equivalent is a six-band wipe with a hot edge, composed on the hidden
// page and revealed only after the command engine has finished it.
static void Msx2_BoardBattleBurnStep(void)
{
	u8 page = (u8)(Msx2_VideoGetShowPage() ^ 1);
	u8 outcome = g_duel.last_battle.outcome;

	// The wipe itself is a sprite overlay shared by both bitmap pages.  Only the
	// final stat-line cleanup needs a bitmap copy; copying 60,000 pixels for
	// every band was the slow part of the old black wipe.
	Msx2_VideoDrawPage(page);
	if(g_batt_trap || outcome == MSX2_BATTLE_DESTROY_ATTACKER ||
	   outcome == MSX2_BATTLE_DESTROY_BOTH)
		Msx2_BattleFxBurnCard(g_batt_ax, MSX2_BATTLE_CARD_H, g_batt_step);
	if(outcome == MSX2_BATTLE_DESTROY_DEFENDER ||
	   outcome == MSX2_BATTLE_DESTROY_BOTH)
		Msx2_BattleFxBurnCard(g_batt_dx, MSX2_BATTLE_CARD_H, g_batt_step);
	if(g_batt_step >= MSX2_BATTLE_BURN_STEPS)
		Msx2_VideoCopyPage(page, (u8)(page ^ 1));
}

static void Msx2_BoardRestoreFromCutin(void)
{
	u8 page = (u8)(Msx2_VideoGetShowPage() ^ 1);
	u8 i;

	g_suppress_slot = MSX2_SLOT_NONE;
	g_hold_hand = MSX2_SLOT_NONE;
	// A DECIDED DUEL COMES BACK STRAIGHT INTO THE VIEW IT IS ANNOUNCED IN.
	// The blow that ends a duel is an attack, so its cut-in restored the chair
	// board -- a blanked stream of the whole view on both pages plus ten slot
	// blits -- and Msx2_BoardOverBegin() then cut from that to the overhead
	// view, which is the same work again.  The player saw the board rebuild
	// itself twice, through two black gaps, before the word arrived, and the
	// first of the two was a picture nothing was going to be shown on.  There
	// is only one board worth restoring here, so restore that one.
	if(g_duel.result != 0)
	{
		g_view = MSX2_VIEW_OVER;
		Msx2_RasterSetView(g_view);
	}
	Msx2_BoardSnapshot();
	VDP_EnableDisplay(FALSE);
	// Nothing of the cut-in may survive onto the board, and the strike's layer
	// is sprites, which no repaint of the bitmap can reach.
	//
	// THE OUTPUT IS OFF BEFORE THEY GO.  The wipe is what is standing where a
	// destroyed card still is: the cut-in's own picture is underneath it,
	// whole, until this call composes the new board.  Taking the sprites off
	// first handed the scanner one frame of the card the burn had just put out
	// -- the destroyed monster flashing back into existence on its way off the
	// screen.  Blank first, then hide, and there is no frame to show it in.
	Msx2_BattleFxBurst(0, 0, MSX2_SPR_BURST_N);
	Msx2_SpriteSlashHide();
	Msx2_SpriteBurnHide();
	Msx2_VideoDrawPage(page);
	Msx2_StreamSceneBlanked(MSX2_VIEW_SEGMENT(g_stage, g_view), page);
	Msx2_VideoDrawPage(page);
	for(i = 0; i < SLOT_COUNT; ++i)
		if((g_want[i] != MSX2_CARD_NONE) &&
		   (!IS_HAND(i) || (g_view != MSX2_VIEW_OVER)))
			Msx2_BoardBlitSlot(i);
	Msx2_BoardHud();
	Msx2_BoardInfo();
	Msx2_VideoCopyPage(page, (u8)(page ^ 1));
	Msx2_VideoShowPage(page);
	VDP_EnableDisplay(TRUE);

	for(i = 0; i < SLOT_COUNT; ++i)
	{
		g_shown[0][i] = g_shown[1][i] = g_want[i];
		g_shown_flag[0][i] = g_shown_flag[1][i] = g_flag[i];
	}
	g_hud_left = g_card_left = g_prompt_left = 0;
	g_fx_kind = FX_NONE;
	g_fx_followup = FX_NONE;
}

// A 40x48 face does not fit in the 44 offscreen rows below GRAPHIC 7's
// visible page.  Split its backing store into two 40x24 tiles side by side.
// Each retained page owns its own copy automatically because Msx2_CopyRect()
// addresses the current draw page.
#define FX_STASH_Y       216
#define FX_STASH_HALF_H  (MSX2_CARD_H / 2)
// How far apart the two backing tiles sit.  It used to be MSX2_CARD_W, which
// was enough while the only thing ever saved was a 40-wide card; the arrival
// pose saves a whole SLOT BOX, and a near slot's box is wider than that -- at
// forty the two tiles would have overlapped and each would have eaten the
// other's rows back.
#define FX_STASH_PITCH   64

// THE FLYING CARD IS CACHED IN VRAM, NOT RE-READ EVERY POSE.
// A pose used to be Msx2_StreamRect() out of the cartridge: the Z80 maps a NEO
// segment in and pushes 1,344 bytes (32x42, overhead) or 1,920 (40x48) through
// the VDP data port at the padded 32 T-states a byte GRAPHIC 7 needs while it
// is scanning out -- roughly a frame of the CPU, per pose, per page, and a
// landing is eight poses on each of two pages.
//
// But every pose of a flight shows the SAME picture; only its position moves.
// So it is read once, into offscreen rows of both pages, and each pose is two
// Msx2_CopyRect() calls -- VDP-to-VDP block copies the command engine does on
// its own while the CPU goes back to the rest of the frame.  Sixteen cartridge
// streams become two.
//
// It sits beside the flight's own backing store, which owns x 0..80 of the same
// rows.  Split in half for the same reason the backing is: a 48-row card does
// not fit in the 44 rows GRAPHIC 7 leaves below the visible 212.
#define FX_CACHE_X       128

// Pages that still owe the landing's banner.  The words never change for the
// length of a flight, so writing them once a page is the whole of it.
static u8 g_fx_banner_left;

static void Msx2_BoardFxCardBacking(bool restore, u8 x, u8 y, u8 w,
                                    u8 ha, u8 hb)
{
	if(restore)
	{
		Msx2_CopyRect(0, FX_STASH_Y, x, y, w, ha);
		Msx2_CopyRect(FX_STASH_PITCH, FX_STASH_Y, x, (u8)(y + ha), w, hb);
	}
	else
	{
		Msx2_CopyRect(x, y, 0, FX_STASH_Y, w, ha);
		Msx2_CopyRect(x, (u8)(y + ha), FX_STASH_PITCH, FX_STASH_Y, w, hb);
	}
}

// What a landing carries.
//
// A monster put into a field slot is SET: face down, whoever played it -- the
// rules say so for both sides now -- so what travels is the one back cover, and
// the same cached picture serves the player's summon and the opponent's.  An
// equip is not set; it is played openly, and the player's own equip flies its
// own face.
static u8 Msx2_BoardFxFlightCard(void)
{
	if((g_fx_kind == FX_SUMMON) || (g_fx_kind == FX_COM_PLACE))
		return MSX2_CARD_BACK_INDEX;
	return (g_fx_owner == MSX2_OWNER_COM) ? MSX2_CARD_BACK_INDEX : g_fx_card;
}

// Read the flight's card into the offscreen cache of both pages.  Called once,
// when a landing begins.
static void Msx2_BoardFxCacheCard(void)
{
	bool over = (g_view == MSX2_VIEW_OVER);
	u8   card = Msx2_BoardFxFlightCard();
	u8   cw   = over ? MSX2_OVER_CARD_W : MSX2_CARD_W;
	u8   half = (u8)((over ? MSX2_OVER_CARD_H : MSX2_CARD_H) >> 1);
	u16  seg, off;
	u8   keep = Msx2_VideoGetDrawPage();
	u8   p;

	// Every caller is "a presentation beat has just started"; only some of them
	// are flights, so the test lives here rather than at three call sites.
	if(!Msx2_BoardFxIsLanding() || (g_fx_field == MSX2_SLOT_NONE))
		return;
	g_fx_banner_left = PAGES_ALL;

	if(over)
	{
		// The opponent's row is the half-turned set, exactly as the settled
		// card will be drawn -- otherwise the card spins as it lands.
		u16 base = ((SLOT_ZONE(g_fx_field) == ZONE_COM) ||
		            (SLOT_ZONE(g_fx_field) == ZONE_COM_SUP))
		         ? MSX2_OVER_CARD_MIRROR_SEGMENT : MSX2_OVER_CARD_SEGMENT;
		seg = (u16)(base + card / MSX2_OVER_CARD_PER_SEG);
		off = (u16)((card % MSX2_OVER_CARD_PER_SEG) * MSX2_OVER_CARD_STRIDE);
	}
	else
	{
		seg = (u16)(MSX2_CARD_ART_SEGMENT + card / MSX2_CARD_ART_PER_SEG);
		off = (u16)((card % MSX2_CARD_ART_PER_SEG) * MSX2_CARD_ART_STRIDE);
	}

	// The blob is row-major, so the bottom half of a card starts exactly half
	// its rows in.  Msx2_StreamRect() normalises an offset that runs past the
	// 16 KB window, so the sum needs no care here.
	for(p = 0; p < MSX2_VIDEO_PAGES; ++p)
	{
		Msx2_VideoDrawPage(p);
		Msx2_StreamRect(seg, off, FX_CACHE_X, FX_STASH_Y, cw, half);
		Msx2_StreamRect(seg, (u16)(off + (u16)half * cw),
		                (u8)(FX_CACHE_X + MSX2_CARD_W), FX_STASH_Y, cw, half);
	}
	Msx2_VideoDrawPage(keep);
}

// Take the destination from the same table that draws the settled card.  This
// is deliberately called from StartFx/FinishFx, not from HideHand(): the
// latter is also called after the cursor has already hidden the hand.
static void Msx2_BoardFxSetDestination(void)
{
	u8 i;

	if(!Msx2_BoardFxIsLanding() || (g_fx_field == MSX2_SLOT_NONE))
		return;

	g_fx_start_x = (g_fx_hand == MSX2_SLOT_NONE) ? 108 : HAND_X(g_fx_hand);
	g_fx_start_y = MSX2_HAND_Y;

	if(g_view == MSX2_VIEW_OVER)
	{
		// The overhead card is copied at exactly this origin by
		// Msx2_BoardBlitSlot().  In particular, slot 5 is (17, 98), so the
		// flight must end at (17, 98), not at a box centre or a stale default.
		const u8* at = g_msx2_over_card_xy[g_fx_field];
		g_fx_dest_x = at[0];
		g_fx_dest_y = at[1];
		return;
	}

	// A chair-view card is drawn into a projected quad.  The flight still uses
	// the readable 40x48 thumbnail, so aim its centre at the quad's centre.
	// Compute the quad bounds in signed temporaries before narrowing anything;
	// a wrapped unsigned intermediate here was the source of the off-screen
	// Y path seen when a player placed a card by hand.
	{
		const u8* q = g_msx2_slot_quad[g_view][g_fx_field];
		u8 x0 = q[0], x1 = q[0], y0 = q[1], y1 = q[1];
		for(i = 1; i < 4; ++i)
		{
			u8 qx = q[i * 2], qy = q[i * 2 + 1];
			if(qx < x0) x0 = qx;
			if(qx > x1) x1 = qx;
			if(qy < y0) y0 = qy;
			if(qy > y1) y1 = qy;
		}
		{
			i16 x = (i16)x0 + (i16)(x1 - x0 + 1) / 2
			       - MSX2_CARD_W / 2;
			i16 y = (i16)y0 + (i16)(y1 - y0 + 1) / 2
			       - MSX2_CARD_H / 2;
			if(x < 0) x = 0;
			if(y < MSX2_BAND_Y) y = MSX2_BAND_Y;
			if(x > (i16)(MSX2_SCREEN_W - MSX2_CARD_W))
				x = (i16)(MSX2_SCREEN_W - MSX2_CARD_W);
			if(y > (i16)(MSX2_SCREEN_H - MSX2_CARD_H))
				y = (i16)(MSX2_SCREEN_H - MSX2_CARD_H);
			g_fx_dest_x = (u8)x;
			g_fx_dest_y = (u8)y;
		}
	}
}

// Interpolate only values that have already been clamped to the screen.  The
// old expression mixed signed deltas with u8 coordinates at the call site;
// on SDCC a negative delta could be narrowed before the add and wrap a valid
// destination toward the top of the screen.  This form keeps the magnitude
// positive and therefore cannot overshoot either endpoint.
static u8 Msx2_BoardFxLerp(u8 start, u8 dest, u8 weight)
{
	i16 delta = (i16)dest - (i16)start;
	i16 value;

	if(delta < 0)
		value = (i16)start - ((i16)(-delta) * (i16)weight) / 16;
	else
		value = (i16)start + (delta * (i16)weight) / 16;
	if(value < 0) value = 0;
	if(value > 255) value = 255;
	return (u8)value;
}

// THE EASING, AND WHY IT IS A TABLE.
// The interpolation is a 4.4 fixed-point weight, so the last pose has to reach
// exactly 16 or the card stops short of the slot and then jumps into it when
// the board takes it.  The weight used to be computed as progress + progress/4,
// which reached 16 only while a flight was fourteen poses long; at the eight it
// is now that arithmetic tops out at 8, so every card landed on the halfway
// point of its own flight.  Eight numbers cost eight bytes and cannot drift out
// of step with FX_LANDING_FRAMES again -- and being a table, they are also an
// ease-in-out rather than a constant speed.
static const u8 g_fx_ease[FX_LANDING_FRAMES] = { 0, 1, 4, 7, 10, 13, 15, 16 };


// The card that is being played, as a picture rather than an outline.
//
// It leaves its hand position and travels in both axes to the projected slot.
// Before each opaque pose, the hidden page saves the 40x48 arena underneath in
// its offscreen rows; when that page comes round again the exact pixels are put
// back first.  So this is a real card moving over the board, not an XOR frame,
// and scan-out only ever sees completed poses.
static void Msx2_BoardFxCardFlight(bool erase)
{
	// Overhead the card that is going to sit in the slot is the 32x42 one baked
	// for that view and the slot is 42x42, so the whole flight is one rectangle
	// moving: no size ever changes and the last pose IS the settled card.
	//
	// A CHAIR VIEW CANNOT DO THAT, AND MUST NOT PRETEND TO.
	// A projected slot is about 30x15 and the thumbnail is 40x48, and the
	// command engine copies rectangles -- it does not scale.  Shrinking the
	// thumbnail into the slot by taking a smaller and smaller window out of it
	// arrives in the right place with the wrong picture: thirty by fifteen out
	// of the middle of a card back is a smear, not a card.  So the flight
	// carries the WHOLE card, centred on where the card is going, and the
	// arrival pose is the settled card itself -- drawn into its quad by the
	// same span program the board is about to use, which is the only thing on
	// this machine that can put a card into a projected slot at all.  The
	// picture is then continuous from the flight into the board taking it.
	bool over = (g_view == MSX2_VIEW_OVER);
	u8 cw   = over ? MSX2_OVER_CARD_W : MSX2_CARD_W;
	u8 ch   = over ? MSX2_OVER_CARD_H : MSX2_CARD_H;
	u8 pose = (g_fx_frames == 0) || (g_fx_frames > FX_LANDING_FRAMES)
	        ? (u8)(FX_LANDING_FRAMES - 1)
	        : (u8)(FX_LANDING_FRAMES - g_fx_frames);
	u8 ease = g_fx_ease[pose];
	u8 x, y, w, ha, hb;

	if(!over && (ease == 16))
	{
		// The arrival.  What is saved and put back is the slot's own restore
		// box -- the quad plus its margin -- because that is the rectangle a
		// span program is allowed to round a texel into.
		const u8* box = g_msx2_slot_box[g_view][g_fx_field];
		hb = (u8)(box[3] >> 1);
		ha = (u8)(box[3] - hb);
		if(erase)
		{
			Msx2_BoardFxCardBacking(TRUE, box[0], box[1], box[2], ha, hb);
			return;
		}
		Msx2_BoardFxCardBacking(FALSE, box[0], box[1], box[2], ha, hb);
		Msx2_RasterSetView(g_view);
		Msx2_RasterCard(Msx2_BoardFxFlightCard(), g_fx_field,
		                g_duel.side[g_fx_owner].defense[SLOT_INDEX(g_fx_field)]
		                    ? 1 : 0);
		return;
	}

	// Everything before it: the whole card, easing from the snapped hand
	// origin to the snapped destination origin.  For overhead the last pose is
	// therefore exactly the same (x,y) that the board painter uses.
	x = Msx2_BoardFxLerp(g_fx_start_x, g_fx_dest_x, ease);
	y = Msx2_BoardFxLerp(g_fx_start_y, g_fx_dest_y, ease);
	w  = cw;
	ha = (u8)(ch >> 1);
	hb = (u8)(ch - ha);

	if(erase)
	{
		Msx2_BoardFxCardBacking(TRUE, x, y, w, ha, hb);
		return;
	}
	Msx2_BoardFxCardBacking(FALSE, x, y, w, ha, hb);
	// Out of the offscreen cache Msx2_BoardFxCacheCard() filled, in the same
	// two halves it was stored in: two command-engine copies, and not one byte
	// through the data port.
	Msx2_CopyRect(FX_CACHE_X, FX_STASH_Y, x, y, w, ha);
	Msx2_CopyRect((u8)(FX_CACHE_X + MSX2_CARD_W), FX_STASH_Y,
	              x, (u8)(y + ha), w, hb);
}

static void Msx2_BoardFxDraw(bool erase)
{
	u8 flash = (g_fx_frames & 2) ? MSX2_GOLD : MSX2_WHITE;

	switch(g_fx_kind)
	{
	case FX_COM_CHOOSE:
		// The banner and nothing else.  What the beat POINTS at -- the held
		// cover in the opponent's hand, or the attacker on its row when there
		// is no hand card left -- is the sprite selector, placed by
		// Msx2_BoardShowCursor() exactly as it is for the player.  Nothing is
		// drawn into the bitmap, so there is nothing here to erase either.
		if(!erase)
			Msx2_BoardFxBanner(Msx2_UiText(MSX2_S_OPPONENT_CHOOSES_A_CARD), MSX2_RED);
		break;

	case FX_EQUIP:
		// The board already has the card, in its own row, so the beat is the
		// banner naming it and an outline round the MONSTER it attached to --
		// which is the one thing the row itself cannot say.
		if(!erase)
			Msx2_BoardFxBanner(Msx2_UiText(MSX2_S_EQUIP_POWER), MSX2_GOLD);
		if(g_fx_field != MSX2_SLOT_NONE)
			Msx2_QuadOutlineXor(g_msx2_slot_quad[g_view][g_fx_field], flash);
		break;

	// The two plain landings differ only in what the panel says.
	case FX_COM_PLACE:
	case FX_SUMMON:
		if(!erase)
		{
			// Once a page.  Msx2_BoardPaint() puts the real panel back on a
			// page the first time it draws there -- PANEL_ALL() is set when the
			// effect starts -- and it runs before this, so the banner always
			// lands on top of a panel that has just been repainted.  After that
			// nothing disturbs it, so nothing has to rewrite it.
			u8 bit = PAGE_BIT(Msx2_VideoGetDrawPage());
			if(g_fx_banner_left & bit)
			{
				if(g_fx_kind == FX_COM_PLACE)
					Msx2_BoardFxBanner(Msx2_UiText(MSX2_S_OPPONENT_PLACES_THE_CARD), MSX2_RED);
				else
				{
					// The landing itself is the player's summon cue.  Keeping the
					// panel blank here avoids a redundant fill and text pass on
					// every page while the card is in flight.
					g_fx_banner_left &= (u8)~bit;
				}
				g_fx_banner_left &= (u8)~bit;
			}
		}
		if(g_fx_field != MSX2_SLOT_NONE)
			Msx2_BoardFxCardFlight(erase);
		break;

	}
}

// Prime both pages with the first flight pose while output is blank.  A
// landing used to blacken the hand, then rely on the next visible-page pass to
// put the card back; a busy VDP could show that empty hand for a frame before
// the cached card arrived.  The source pose now exists on both pages before
// the display is re-enabled, so the flight is continuous from the old hand.
static void Msx2_BoardHideHand(void);

static void Msx2_BoardPrimeLanding(void)
{
	u8 keep = Msx2_VideoGetDrawPage();
	u8 p;

	if(!Msx2_BoardFxIsLanding())
		return;
	for(p = 0; p < MSX2_VIDEO_PAGES; ++p)
	{
		Msx2_VideoDrawPage(p);
		Msx2_BoardFxDraw(FALSE);
		g_fx_page_frame[p] = g_fx_frames;
	}
	Msx2_VideoDrawPage(keep);
}

static void Msx2_BoardPrepareLanding(void)
{
	if(!Msx2_BoardFxIsLanding())
		return;
	// THE BEND BELONGS TO THE LANDING, NOT TO THE HAND.
	// It used to be armed inside Msx2_BoardHideHand(), which returns at once
	// when the hand is already off the screen -- and it always is when a card
	// is placed, because choosing the destination slot walks the cursor onto
	// the field and the camera cuts to the top view before the flight starts.
	// So the one case the player actually plays had no bend: the flight ended,
	// the cleanup pass restored the arena under the last pose on both pages,
	// and the settled card was only put back afterwards by the ordinary
	// painter, one page per frame.  The card the player had just played
	// vanished for about a second and then came back.  Arming it here means a
	// landing always gets its two commit poses, whatever the hand was doing.
	g_fx_bend = FX_BEND_POSES;
	g_fx_hold = 0;
	// Only a landing that actually took the strip off the screen owes it back.
	g_fx_hand_back = (u8)!g_hand_hidden;
	// Hand removal, cartridge caching and the first pose are one composition.
	// Keep output blank for all of it; otherwise the visible page can show the
	// blackened hand before the cached card has reached its source position.
	Msx2_SpriteTransitionBegin();
	VDP_EnableDisplay(FALSE);
	Msx2_BoardHideHand();
	Msx2_BoardFxCacheCard();
	Msx2_BoardPrimeLanding();
	VDP_CommandWait();
	VDP_EnableDisplay(TRUE);
}

static void Msx2_BoardFxErase(u8 frame)
{
	// A LANDING'S PANEL IS THE BANNER, AND THE BANNER IS CONSTANT.
	// Repainting the info panel here wiped it every pose, so every pose had to
	// write it again -- and Msx2_BoardInfo() is not cheap: the card lines read
	// the hovered card's NAME out of the cartridge and then push three strings
	// through the data port, all to produce the picture that was already on the
	// screen.  For a flight, neither half of that repetition is needed.
	if(!Msx2_BoardFxIsLanding())
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

	if(!Msx2_BoardFxIsLanding() || g_hand_hidden)
		return;
	// The destination was already snapped by StartFx/FinishFx.  Hand
	// visibility is only a drawing concern; it is not allowed to alter the
	// flight path or leave it using the previous action's coordinates -- nor,
	// since Msx2_BoardPrepareLanding() arms them, the landing's bend poses.
	Msx2_BoardSnapshot();

	g_hand_hidden = TRUE;

	// ONLY THE CARD THAT IS BEING PLAYED LEAVES THE HAND.
	// The whole band used to go flat black on both pages, which is right for
	// the player -- the camera is overhead by then and the strip is not on the
	// screen at all -- but the opponent plays from its own chair, with its five
	// covers in front of it, and every card it played wiped all five off and
	// dealt them back afterwards.  The card that flies is the only one that has
	// left the hand, so it is the only one erased: a fill and the position's
	// own baked frame, on both pages, and the other four are never touched.
	if(g_view != MSX2_VIEW_OVER)
	{
		u8 hand_slot = (g_fx_hand < MSX2_HAND_SLOTS)
		             ? SLOT_OF(ZONE_HAND, g_fx_hand) : MSX2_SLOT_NONE;
		for(i = 0; i < MSX2_VIDEO_PAGES; ++i)
		{
			Msx2_VideoDrawPage(i);
			if(hand_slot == MSX2_SLOT_NONE)
				Msx2_Fill(0, MSX2_HAND_BAND_Y, MSX2_SCREEN_W,
				          MSX2_HAND_BAND_H, MSX2_BLACK);
			else
			{
				u8 x = HAND_X(g_fx_hand);
				Msx2_Fill(x, MSX2_HAND_Y, MSX2_CARD_W, MSX2_CARD_H,
				          MSX2_BLACK);
			}
		}
		Msx2_VideoDrawPage((u8)(show ^ 1));
		if(hand_slot != MSX2_SLOT_NONE)
		{
			// The strip is still on the screen, so the snapshot must keep
			// reporting the other four: g_hand_hidden stays clear, and only the
			// emptied position is marked as drawn.
			g_hand_hidden = FALSE;
			g_shown[0][hand_slot] = g_shown[1][hand_slot] = MSX2_CARD_NONE;
			g_shown_flag[0][hand_slot] = g_shown_flag[1][hand_slot] = 0;
			return;
		}
	}
	else
		Msx2_VideoDrawPage((u8)(show ^ 1));

	for(i = MSX2_FIELD_SLOTS; i < SLOT_COUNT; ++i)
	{
		g_shown[0][i] = g_shown[1][i] = MSX2_CARD_NONE;
		g_shown_flag[0][i] = g_shown_flag[1][i] = 0;
	}
}

// Taking the flight back off the board.
//
// This used to re-stream the whole resting board band out of the cartridge --
// twenty-nine kilobytes, blanked -- and then repaint all ten slots, because the
// picture that came back was empty.  It was a third of a second on its own, and
// it was wrong in the overhead view, where the board goes on down through the
// hand's rows and the band is only part of the picture.
//
// The flight already saves the 40x48 of arena it is about to cover, on each
// page separately, and already knows which pose each page is still holding.  So
// putting the board back is that restore, twice -- one command pair per page --
// and the ten slots the repaint would have had to redraw were never disturbed.
static void Msx2_BoardFxCommitLanding(void);

// ONE PAGE PER FRAME, AND THE CARD GOES BACK BEFORE THE PAGE IS SHOWN.
// This used to erase the flight from BOTH pages and only then draw the settled
// card into the slot -- and one of those two pages was the one the VDP was
// scanning out.  Between the restore and the card, that page showed an empty
// destination, and the card is not cheap to put back: a chair-view slot is a
// 1,920-byte cartridge read and a span program.  What the player saw was the
// card they had just played blinking out of existence and then reappearing.
//
// So a bend step now touches the hidden page only: it takes the flight off it,
// puts the board's own card into the slot, and asks for the flip.  The page
// that was visible still holds the last flight pose -- which, by construction,
// is the same picture -- and gets the same treatment on the next frame, once
// the flip has made it the hidden one.
static void Msx2_BoardStepBend(void)
{
	u8 page = Msx2_VideoGetDrawPage();
	u8 keep = g_fx_frames;

	if(g_fx_page_frame[page] != FX_FRAME_NONE)
	{
		Msx2_BoardFxErase(g_fx_page_frame[page]);
		g_fx_page_frame[page] = FX_FRAME_NONE;
		g_fx_frames = keep;
	}
	// The rules have already taken the card; the board is only now allowed to
	// say so.  Snapshot once, on the first of the two pages.
	if(g_suppress_slot != MSX2_SLOT_NONE)
	{
		g_suppress_slot = MSX2_SLOT_NONE;
		Msx2_BoardSnapshot();
	}
	Msx2_BoardFxCommitLanding();
	--g_fx_bend;
}

// Replace the flight with the retained board card before the flight layer is
// allowed to disappear.  Both pages are updated in this same command batch, so
// the V-blank can never expose the gap between the last flight pose and the
// card painter catching up on the next frame.
// The settled card, into the slot, on the page being drawn -- which is always
// the hidden one.  See Msx2_BoardStepBend for why it is not both at once.
static void Msx2_BoardFxCommitLanding(void)
{
	u8 page = Msx2_VideoGetDrawPage();

	if(g_fx_field == MSX2_SLOT_NONE)
		return;
	Msx2_BoardBlitSlot(g_fx_field);
	g_shown[page][g_fx_field] = g_want[g_fx_field];
	g_shown_flag[page][g_fx_field] = g_flag[g_fx_field];
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
	g_hold_hand = MSX2_SLOT_NONE;

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
		{
			// THE PLAYER'S OWN SUMMON IS A SET, AND IT IS ANIMATED LIKE ONE.
			// A monster put into a field slot goes down face down for both
			// sides, so the card that lands is not the one the player picked
			// out of their hand -- it is the back cover, and the flight is what
			// says which of five slots it went into and that it is now hidden.
			// It was dropped once as a second and a half of watching per
			// summon, and most of that was the cartridge: eight poses on each
			// of two pages, each one 1,344 bytes pushed through the data port.
			// Msx2_BoardFxCacheCard() reads it twice for the whole flight now,
			// and the poses themselves are command-engine copies.
			// The cursor still ends on the slot, which is what the overhead
			// view is for.
			g_fx_kind = FX_SUMMON;
			g_zone = ZONE_FIELD;
			g_sel = g_duel.last_action_field_slot;
		}
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
	{
		// TURNING A CARD IS NOT AN EVENT, IT IS A REDRAW.
		// It used to be a presentation beat like a summon: eight frames of an
		// XOR outline flashing round the quad, a banner, a whole-panel repaint
		// and a two-page cleanup copy -- the best part of a second to answer a
		// keypress, and the outline was itself the thing the owner asked not to
		// be drawn.  The card's own art already says which way it is lying, so
		// the answer is simply to let the retained painter put the other art in
		// the slot.  Only the card lines move with it; the life points and the
		// prompt are the words they already were.
		Msx2_ClearActionEvent();
		Msx2_BoardSnapshot();
		g_card_left = PAGES_ALL;
		g_fx_kind = FX_NONE;
		return;
	}
	else
	{
		// A support card is the same card whoever plays it, and the 2-D cut-in
		// is the only place the port ever says what one DID.  The opponent's
		// used to get the retained-board "chooses a card" flash and nothing
		// else, so THUNDER took a monster off the player's row with no screen
		// naming it.  The opponent keeps the wind-up beat -- it is what says
		// the card came out of the opponent's hand -- and the cut-in follows it.
		g_fx_kind = (owner == MSX2_OWNER_COM) ? FX_COM_CHOOSE : FX_SUPPORT;
		if(owner == MSX2_OWNER_COM)
			g_fx_followup = FX_SUPPORT;
	}

	// The wind-up beat says which card came out of the opponent's hand, so the
	// cover stays in the position the selector is standing on until the beat is
	// over.  The rules emptied it before any of this ran.
	if((g_fx_kind == FX_COM_CHOOSE) && (g_fx_hand < MSX2_HAND_SLOTS))
		g_hold_hand = SLOT_OF(ZONE_HAND, g_fx_hand);

	Msx2_BoardFxSetDestination();
	Msx2_ClearActionEvent();
	// The selector goes now, not on the next frame's Msx2_BoardShowCursor():
	// a cut-in composes inside this call, and the frame it appears on would
	// otherwise still carry the gem sitting on the card that was just played.
	Msx2_SpriteHideGem();
	// THE BOARD MUST NOT SHOW THE CONSEQUENCE BEFORE THE SCREEN SHOWS THE CAUSE.
	// The rules commit the instant the opponent acts, and Msx2_BoardTouchRules()
	// has already snapshotted the result -- so for THUNDER the player's whole
	// row was empty in g_want before this ran.  The opponent's wind-up beat is
	// played ON the board, and the cut-in that NAMES the card is two beats
	// after it, so the row swept itself clear and the screen explained why
	// afterwards.  A wind-up that hands over to a full-screen cut-in therefore
	// puts back the board that is actually on the screen and leaves it there;
	// the cut-in owns the display from the frame it starts, and
	// Msx2_BoardRestoreFromCutin() is what shows the new state, once the card
	// has been named.
	if(g_fx_followup == FX_SUPPORT)
	{
		u8 show = Msx2_VideoGetShowPage();
		u8 i;
		for(i = 0; i < SLOT_COUNT; ++i)
		{
			g_want[i] = g_shown[show][i];
			g_flag[i] = (u8)(g_shown_flag[show][i] & (u8)~F_STALE);
		}
	}
	else
		Msx2_BoardSnapshot();
	PANEL_ALL();
	// A flight IS the beat for a landing, so eight poses is the beat.  An equip
	// no longer has one -- the card is simply on its row -- so its banner is
	// given long enough to be read instead.
	g_fx_frames = (g_fx_kind == FX_COM_CHOOSE) ? 12
	            : (g_fx_kind == FX_EQUIP)      ? FX_EQUIP_FRAMES
	            :                                FX_LANDING_FRAMES;
	g_fx_cleanup = FALSE;
	g_fx_page_frame[0] = g_fx_page_frame[1] = FX_FRAME_NONE;
	if(Msx2_BoardFxIsBattle())
	{
		g_fx_frames = 60;
		Msx2_BoardShowBattleCutin();
	}
	else if(g_fx_kind == FX_FUSION)
		Msx2_FusionBegin(g_fuse_mat, g_fuse_mat_n, g_duel.last_action_card);
	else if(g_fx_kind == FX_SUPPORT)
		Msx2_EffectBegin(g_fx_card, FALSE);
	Msx2_BoardPrepareLanding();
}

static void Msx2_BoardFinishFx(void)
{
	u8 next = g_fx_followup;

	// The wind-up is over: the held cover leaves the hand now, which is what
	// the flight that follows is carrying.
	g_hold_hand = MSX2_SLOT_NONE;
	if(next == FX_COM_PLACE)
	{
		// Keep the newly chosen card off the field while its outline travels
		// from the COM hand to the destination.  The settled projected card is
		// revealed by the final cleanup pass.
		g_suppress_slot = g_fx_field;
		Msx2_BoardSnapshot();
		PANEL_ALL();
		g_fx_kind = FX_COM_PLACE;
		g_fx_followup = FX_NONE;
		g_fx_frames = FX_LANDING_FRAMES;
		g_fx_page_frame[0] = g_fx_page_frame[1] = FX_FRAME_NONE;
		Msx2_BoardFxSetDestination();
		Msx2_BoardPrepareLanding();
		return;
	}
	if(next == FX_SUPPORT)
	{
		// The opponent's support card, now that its wind-up beat has run: the
		// same full-screen cut-in the player's own gets.
		g_fx_kind = FX_SUPPORT;
		g_fx_followup = FX_NONE;
		g_fx_frames = FX_LANDING_FRAMES;
		g_fx_page_frame[0] = g_fx_page_frame[1] = FX_FRAME_NONE;
		Msx2_EffectBegin(g_fx_card, TRUE);
		return;
	}
	if(next == FX_EQUIP)
	{
		g_fx_kind = FX_EQUIP;
		g_fx_followup = FX_NONE;
		g_fx_frames = FX_EQUIP_FRAMES;
		g_fx_page_frame[0] = g_fx_page_frame[1] = FX_FRAME_NONE;
		return;
	}

	g_suppress_slot = MSX2_SLOT_NONE;
	g_hand_hidden = FALSE;
	g_fx_bend = 0;
	g_fx_hold = 0;
	Msx2_BoardSnapshot();
	PANEL_ALL();
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
	if(g_fx_kind == FX_SUPPORT)
	{
		// Space or Escape cuts the hold short -- but not for the first few
		// frames.  The press that PLAYED the card is still in the latch on the
		// frame the cut-in is composed (StartFx runs at the end of the same
		// step), so an unguarded test skipped the screen with the button that
		// asked for it and nobody ever saw it.
		if(g_fx_frames != 0)
			--g_fx_frames;
		if(Msx2_EffectStep() &&
		   ((g_fx_frames != 0) ||
		    !(Msx2_InputPressed() & (MSX2_BTN_A | MSX2_BTN_B))))
			return TRUE;
		// The cut-in has already said what the card did, in a full-screen
		// picture with the text under it.  Sending the camera overhead
		// afterwards is a second of blanked streaming to show a board the
		// player was not asking about -- so the screen it comes back to is the
		// hand it was played from, which is where the cursor still is.
		Msx2_BoardRestoreFromCutin();
		PANEL_ALL();
		return TRUE;
	}
	if(Msx2_BoardFxIsBattle())
	{
		switch(g_batt_phase)
		{
		case 0:
			/* The whole strike: the blade opening and crossing, the explosion
			   taking the crossing point, and the figure climbing under it.
			   Sprites, so a pose is a handful of attribute bytes and the stage
			   under them is never touched -- there is no page copy, no save and
			   no restore anywhere in these sixteen frames. */
			Msx2_BoardBattleStrike(g_batt_step);
			Msx2_VideoFlipRequest();
			if(++g_batt_step >= MSX2_BATTLE_COUNT_STEPS)
			{
				Msx2_BattleFxBurst(0, 0, MSX2_SPR_BURST_N);
				Msx2_SpriteSlashHide();
				g_batt_step = 0;
				if(g_batt_trap ||
				   g_duel.last_battle.outcome == MSX2_BATTLE_DESTROY_ATTACKER ||
				   g_duel.last_battle.outcome == MSX2_BATTLE_DESTROY_DEFENDER ||
				   g_duel.last_battle.outcome == MSX2_BATTLE_DESTROY_BOTH)
					g_batt_phase = 6;
				else
				{
					/* Level the other page once, here, so the result line and
					   the settled figure are on both of them for the hold. */
					u8 page = (u8)(Msx2_VideoGetShowPage() ^ 1);
					Msx2_VideoCopyPage((u8)(page ^ 1), page);
					Msx2_VideoDrawPage(page);
					Msx2_BattleFxResult(FALSE);
					g_fx_frames = BATT_HOLD;
					g_batt_phase = 7;
				}
			}
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
		Msx2_BoardStepBend();
		if(g_fx_bend == 0)
			g_fx_hold = FX_HOLD_FRAMES;
		// Request the flip only after the restore and settled card have both
		// been queued.  Asking first let V-blank reveal the erased destination
		// before the landing card was committed.
		Msx2_VideoFlipRequest();
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
			// ... and only if it was this landing that took it away.  A card
			// placed from the top view never had a strip on the screen, and
			// giving it one here draws its cards over the middle of the table.
			if(g_fx_hand_back)
			{
				g_hand_hidden = FALSE;
				Msx2_BoardSnapshot();
				Msx2_VideoDrawPage(page);
			}
			PANEL_ALL();
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
	bool painted = FALSE;
	u8 cards = 0;
	u8 i;

	{
		u8 bit = PAGE_BIT(page);
		if(g_hud_left & bit)
		{
			Msx2_BoardHud();
			g_hud_left &= (u8)~bit;
			painted = TRUE;
		}
		if(g_card_left & bit)
		{
			Msx2_BoardCardLines();
			g_card_left &= (u8)~bit;
			painted = TRUE;
		}
		if(g_prompt_left & bit)
		{
			Msx2_BoardPromptLine();
			g_prompt_left &= (u8)~bit;
			painted = TRUE;
		}
	}
	// Interactive panel updates are the latency-sensitive part of a cursor
	// move.  Do not queue a field-card stream behind them in the same frame;
	// the card can catch up on the next hidden-page pass without delaying the
	// name/prompt flip the player is waiting to see.
	//
	// ONLY WHILE THE BOARD ITSELF IS UP TO DATE HERE, THOUGH.  Returning early
	// asks for a flip, and a flip to a page that is still showing the old art
	// of a slot that has changed puts that art back on the screen: a card
	// turned face-down flickered between its two positions for exactly that
	// reason.  A cursor move leaves nothing stale, so it still takes this path.
	if(painted)
	{
		u8 j;
		for(j = 0; j < SLOT_COUNT; ++j)
			if((g_shown[page][j] != g_want[j]) ||
			   (g_shown_flag[page][j] != g_flag[j]))
				break;
		if(j == SLOT_COUNT)
			return TRUE;
	}

	// THE WHOLE HAND STRIP AT ONCE.
	// The loop below repaints one card a frame, which is the right budget for
	// a card arriving on the board -- but when the strip changes hands, all
	// five change together, and the opponent's first effect owns the screen
	// long before a one-a-frame painter has finished.  What stayed on the
	// screen through the opponent's turn was the player's own hand.
	if(g_hand_left & PAGE_BIT(page))
	{
		u8 j;
		// Overhead, those rows are board: blacking them out would punch the
		// table full of holes.  The strip
		// is still marked painted so the retained loop below leaves it alone.
		if(g_view != MSX2_VIEW_OVER)
		{
			Msx2_Fill(0, MSX2_HAND_BAND_Y, MSX2_SCREEN_W, MSX2_HAND_BAND_H,
			          MSX2_BLACK);
		}
		for(j = MSX2_FIELD_SLOTS; j < SLOT_COUNT; ++j)
		{
			if(g_view != MSX2_VIEW_OVER)
				Msx2_BoardBlitSlot(j);
			g_shown[page][j] = g_want[j];
			g_shown_flag[page][j] = g_flag[j];
		}
		g_hand_left &= (u8)~PAGE_BIT(page);
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
}

// One frame of the opening deal: the card in flight is erased from this page
// at the position it last had here and redrawn one step further left.
// THE FLICKER IN THE DEAL.
//
// A card in flight is erased by blacking out the forty-eight rows it was on.  That is right for the band and wrong for
// anything already settled in it: the flight comes in from the right and lands
// on the left, so on the step after a landing the black rectangle sits exactly
// on top of the card that just arrived -- and the retained painter was never
// told, because g_shown[] still said that slot was drawn.
//
// The two pages hold different flights, so each blacked out a different card
// and each kept it blacked out.  What the player saw was the whole hand
// blinking, one card at a time, for the length of the deal.
//
// So the erase repairs what it damaged, here rather than a frame later: at most
// two settled cards can overlap a forty-pixel rectangle, and only on the step
// after a landing.
static void Msx2_BoardDealErase(u8 page)
{
	u8 px = g_deal_px[page];
	u8 i;

	if(px == MSX2_SLOT_NONE)
		return;
	Msx2_Fill(px, MSX2_HAND_Y, MSX2_CARD_W, MSX2_CARD_H, MSX2_BLACK);
	g_deal_px[page] = MSX2_SLOT_NONE;

	for(i = 0; i < MSX2_HAND_SLOTS; ++i)
	{
		if(!(g_deal_visible_mask & (u8)(1u << i)))
			continue;
		u8 hx = HAND_X(i);
		if(((u8)(hx + MSX2_CARD_W) > px) && (hx < (u8)(px + MSX2_CARD_W)))
			Msx2_BoardBlitSlot(SLOT_OF(ZONE_HAND, i));
	}
}

static void Msx2_BoardStepDeal(void)
{
	u8 page = (u8)(Msx2_VideoGetShowPage() ^ 1);
	u8 card;
	u8 x;
	u8 bit;

	Msx2_VideoDrawPage(page);
	while((g_deal_slot < MSX2_HAND_SLOTS) &&
	      !(g_deal_target_mask & (u8)(1u << g_deal_slot)))
		++g_deal_slot;

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
		if(g_deal_px[page] != MSX2_SLOT_NONE)
			Msx2_BoardDealErase(page);
		if((g_deal_landed_mask[0] == g_deal_target_mask) &&
		   (g_deal_landed_mask[1] == g_deal_target_mask))
		{
			g_deal_visible_mask = g_deal_target_mask;
			Msx2_BoardSnapshot();
		}
		g_mode = (g_view == BOARD_VIEW_COM) ? M_COM : M_IDLE;
		PANEL_ALL();
		return;
	}

	Msx2_BoardDealErase(page);

	// THE HAND BELONGS TO WHOEVER'S CHAIR THIS IS.
	// The opponent's five arrive the same way the player's do and from the same
	// side of the screen -- as the common back, because the face of a card in
	// the opponent's hand is not the player's to see.  The strip used to simply
	// appear, all five at once, which read as a redraw rather than as a deal.
	{
		u8 owner = (g_view == BOARD_VIEW_COM) ? MSX2_OWNER_COM : MSX2_OWNER_PLAYER;
		card = g_duel.side[owner].hand[g_deal_slot];
		if(card != MSX2_CARD_NONE)
			card = (owner == MSX2_OWNER_COM) ? MSX2_CARD_BACK_INDEX : card;
	}
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

	// LANDED -- ON BOTH PAGES, NOW.
	// The copy the flight left on THIS page is exactly what the painter would
	// have drawn, so this page is simply marked up to date.  The other page is
	// not: it owes the card, and the retained painter hands out one card a
	// frame while the deal is spending its frames on the next card's flight.
	// So the second page ran several cards behind the first, and the flip
	// alternated between two different hands -- which is the opponent's hand
	// appearing and disappearing card by card as it was dealt.  Drawing it on
	// both pages here costs one extra 40x48 copy per card, five in a deal.
	if(card != MSX2_CARD_NONE)
	{
		u8 slot = SLOT_OF(ZONE_HAND, g_deal_slot);
		u8 p;
		bit = (u8)(1u << g_deal_slot);
		g_deal_visible_mask |= bit;
		++g_deal_reveal;
		Msx2_BoardSnapshot();
		for(p = 0; p < MSX2_VIDEO_PAGES; ++p)
		{
			Msx2_VideoDrawPage(p);
			Msx2_BoardBlitSlot(slot);
			g_shown[p][slot] = g_want[slot];
			g_shown_flag[p][slot] = g_flag[slot];
			g_deal_landed_mask[p] |= bit;
		}
		Msx2_VideoDrawPage(page);
	}
	g_deal_px[page] = MSX2_SLOT_NONE;
	g_deal_step = 0;
	++g_deal_slot;
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
	g_refuse_text = MSX2_S_THOSE_CARDS_DO_NOT_FUSE;
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
	g_fx_start_x = g_fx_start_y = 0;
	g_fx_dest_x = g_fx_dest_y = 0;
	g_suppress_slot = MSX2_SLOT_NONE;
	g_hold_hand = MSX2_SLOT_NONE;
	g_fx_page_frame[0] = g_fx_page_frame[1] = FX_FRAME_NONE;
	g_move_pose = 0;
	g_move_target = BOARD_VIEW_PLAYER;
	g_move_forward = TRUE;
	g_deal_slot = 0;
	g_deal_step = 0;
	g_deal_reveal = 0;
	g_deal_px[0] = g_deal_px[1] = MSX2_SLOT_NONE;
	g_deal_target_mask = 0;
	g_deal_visible_mask = 0;
	g_deal_landed_mask[0] = g_deal_landed_mask[1] = 0;
	Msx2_BoardSnapshot();

	Msx2_RasterInit();
	Msx2_SpriteTransitionBegin();

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
	g_hud_left = g_card_left = g_prompt_left = 0;
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
	u8 page;

	if(view == g_view)
		return;
	page = Msx2_VideoGetDrawPage();

	g_view = view;
	Msx2_RasterSetView(view);
	Msx2_SpriteTransitionBegin();
	VDP_EnableDisplay(FALSE);
	Msx2_StreamSceneBlanked(MSX2_VIEW_SEGMENT(g_stage, view), MSX2_PAGE_0);
	Msx2_StreamSceneBlanked(MSX2_VIEW_SEGMENT(g_stage, view), MSX2_PAGE_1);
	VDP_EnableDisplay(TRUE);

	// THE VIEW ARRIVES FINISHED.
	// The picture underneath every retained card is a different picture now, so
	// the cut owes both pages everything -- and it used to hand that debt to
	// the ordinary painter, which pays it back at one card per page per frame.
	// A game step on this machine is a fifth of a second, so the board the
	// camera cut to spent the best part of two seconds rebuilding itself in
	// front of the player, and the card that had just been played was the last
	// thing to appear on it.  The display is already blanked here for the two
	// arena streams; the whole board goes into both pages inside the same
	// blank, and what comes back is complete.
	//
	// Only occupied slots are drawn: an empty one is exactly what the arena
	// stream has just put down.
	Msx2_BoardSnapshot();
	for(i = 0; i < MSX2_VIDEO_PAGES; ++i)
	{
		u8 j;
		Msx2_VideoDrawPage(i);
		if(view != MSX2_VIEW_OVER)
		{
			Msx2_Fill(0, MSX2_HAND_BAND_Y, MSX2_SCREEN_W, MSX2_HAND_BAND_H,
			          MSX2_BLACK);
		}
		for(j = 0; j < SLOT_COUNT; ++j)
		{
			// Recorded before it is drawn: the slot draw compares against this
			// to decide whether the ground has to go back first, and after an
			// arena stream it never does.
			g_shown[i][j] = g_want[j];
			g_shown_flag[i][j] = g_flag[j];
			if((g_want[j] != MSX2_CARD_NONE) &&
			   (!IS_HAND(j) || (view != MSX2_VIEW_OVER)))
				Msx2_BoardBlitSlot(j);
		}
		// A slot draw can mark its neighbours stale; on this path they have
		// just been drawn too, so nothing is owed.
		for(j = 0; j < SLOT_COUNT; ++j)
			g_shown_flag[i][j] = g_flag[j];
		Msx2_BoardHud();
		Msx2_BoardCardLines();
		Msx2_BoardPromptLine();
	}
	Msx2_VideoDrawPage(page);
	g_hud_left = g_card_left = g_prompt_left = 0;
	g_hand_left = 0;
}

// THE HAND GOES WITH THE TURN, NOT WITH THE CAMERA.
// A chair change plays a baked path that restreams the BOARD BAND only, and
// the hand is dealt after it arrives -- so for the whole of the swing the strip
// still held five cards belonging to the player whose turn had just ended: the
// player's own hand on the way out, the opponent's five covers on the way back.
// Blacking the band here is one fill a page, and it leaves the empty gold
// frames the deal is about to fly cards into.
static void Msx2_BoardClearHandBand(void)
{
	u8 draw = Msx2_VideoGetDrawPage();
	u8 i;

	// The incoming chair always has a strip.  Walking up into the board before
	// ending the turn used to leave this set across the handoff, and the
	// snapshot then reported every hand position empty -- so the deal drew five
	// cards the retained painter immediately wiped off again.
	g_hand_hidden = FALSE;
	g_deal_reveal = 0;
	g_deal_slot = 0;
	g_deal_step = 0;
	g_deal_px[0] = g_deal_px[1] = MSX2_SLOT_NONE;
	g_deal_target_mask = 0;
	g_deal_visible_mask = 0;
	g_deal_landed_mask[0] = g_deal_landed_mask[1] = 0;
	Msx2_BoardSnapshot();

	for(i = 0; i < MSX2_VIDEO_PAGES; ++i)
	{
		Msx2_VideoDrawPage(i);
		Msx2_Fill(0, MSX2_HAND_BAND_Y, MSX2_SCREEN_W, MSX2_HAND_BAND_H,
		          MSX2_BLACK);
	}
	Msx2_VideoDrawPage(draw);

	// Both pages now show exactly what the snapshot asks for -- nothing -- so
	// the retained painter has no repair to make and cannot put the outgoing
	// hand back a card at a time under the swing.
	for(i = MSX2_FIELD_SLOTS; i < SLOT_COUNT; ++i)
	{
		g_shown[0][i] = g_shown[1][i] = g_want[i];
		g_shown_flag[0][i] = g_shown_flag[1][i] = g_flag[i];
	}
	g_hand_left = 0;
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

	Msx2_BoardClearHandBand();
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
	g_hold_hand = MSX2_SLOT_NONE;
	for(i = 0; i < SLOT_COUNT; ++i)
	{
		g_shown[0][i] = g_shown[1][i] = MSX2_CARD_NONE;
		g_shown_flag[0][i] = g_shown_flag[1][i] = 0;
	}
	PANEL_ALL();
	// EVERY chair change deals, not only the opening.  The strip used to be
	// handed over all five at once, which is a redraw and not a deal; and on
	// the way back it is also how the player sees the cards drawn to replace
	// what they spent last turn.
	g_hand_left = 0;
	g_mode = M_DEAL;
	g_deal_slot = 0;
	g_deal_step = 0;
	g_deal_reveal = 0;
	g_deal_px[0] = g_deal_px[1] = MSX2_SLOT_NONE;
	g_deal_visible_mask = 0;
	g_deal_landed_mask[0] = g_deal_landed_mask[1] = 0;
	{
		u8 owner = (g_view == BOARD_VIEW_COM) ? MSX2_OWNER_COM : MSX2_OWNER_PLAYER;
		g_deal_target_mask = 0;
		for(i = 0; i < MSX2_HAND_SLOTS; ++i)
			if(g_duel.side[owner].hand[i] != MSX2_CARD_NONE)
				g_deal_target_mask |= (u8)(1u << i);
	}
	Msx2_BoardSnapshot();
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
	g_hand_left = PAGES_ALL;
	PANEL_ALL();
}

static void Msx2_BoardTouch(void)
{
	Msx2_BoardSnapshot();
	PANEL_ALL();
}

// The same thing for the opponent's turn, which runs one rules step a frame for
// as long as the opponent takes.  A full panel repaint is a fill and eight
// strings on each of two pages, and asking for one every frame was most of what
// made the opponent's turn crawl -- the flip could only come round once the
// painter had finished, so the board itself was updating a few times a second.
// Nothing in the panel changes unless a figure in it does, so that is what is
// tested.
static i16 g_hud_com_lp;
static i16 g_hud_you_lp;
static u8  g_hud_turns;

static void Msx2_BoardTouchRules(void)
{
	Msx2_BoardSnapshot();
	if((g_duel.side[MSX2_OWNER_COM].lp != g_hud_com_lp) ||
	   (g_duel.side[MSX2_OWNER_PLAYER].lp != g_hud_you_lp) ||
	   (g_duel.turns != g_hud_turns))
	{
		g_hud_com_lp = g_duel.side[MSX2_OWNER_COM].lp;
		g_hud_you_lp = g_duel.side[MSX2_OWNER_PLAYER].lp;
		g_hud_turns = g_duel.turns;
		g_hud_left = PAGES_ALL;
	}
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
	PANEL_ALL();
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
	// C is a keyboard-only card check.  Hide the gem before composing the
	// check page so the selector cannot flash over the enlarged card for one
	// frame while the hidden-page copy is being prepared.
	Msx2_SpriteHideGem();
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
				// ONE MONSTER A TURN.
				// The rules already know (Msx2Side::monster_played), but the
				// only place that asked was Msx2_PlaceMonster -- so a second
				// monster walked the cursor into the field row, offered the
				// attack/defence choice, and then silently did nothing, with
				// M_PLACE stuck until ESC.  Refuse here instead, on the card.
				if(g_duel.side[MSX2_OWNER_PLAYER].monster_played)
				{
					g_refuse_text = MSX2_S_ONE_MONSTER_A_TURN;
					g_refuse = 96;
					Msx2_BoardTouch();
					return;
				}
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
				{
					// no recipe, or no summon left this turn
					g_refuse_text = MSX2_S_THOSE_CARDS_DO_NOT_FUSE;
					g_refuse = 96;
				}
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
			PANEL_ALL();
		}
		return;
	}

	// The cursor's rows are COM, FIELD and HAND.  The two support zones lie
	// between FIELD and HAND in the slot numbering and are display-only, so a
	// step names the next row rather than counting to it; the low/high clamps
	// above still work, because the numbering keeps the three in order.
	if((pressed & MSX2_BTN_UP) && (g_zone > low))
		g_zone = (g_zone == ZONE_HAND) ? ZONE_FIELD : ZONE_COM;
	if(pressed & MSX2_BTN_DOWN)
	{
		if(g_zone < high)
			g_zone = (g_zone == ZONE_COM) ? ZONE_FIELD : ZONE_HAND;
		else if(g_zone == ZONE_HAND)
		{
			// Already on the bottom row, so down is free to mean the other
			// thing a hand card can be: material for a fusion chain.
			Msx2_BoardQueueToggle(g_sel);
			Msx2_SfxPlay(MSX2_SFX_SELECT);
			Msx2_BoardSnapshot();
			PANEL_ALL();
		}
	}
}

// ─────────────────────────────────────────────────────────────────────────────
//  The result
// ─────────────────────────────────────────────────────────────────────────────

// The duel is decided.  What the player is owed here is the board that decided
// it: the tactical view from above, with the last exchange already settled into
// it -- whatever died in it gone, whatever survived still standing where it was
// -- and only then the word.
//
// Both pages are brought all the way up to the snapshot before anything is
// announced.  The retained painter is deliberately bounded to one card a frame,
// which is right while a duel is being played and wrong here: it would deal the
// final board back onto the screen a card at a time under a banner that had
// already arrived.  The display is off for it, the way it is for a camera cut,
// so none of that repaint is seen.
// Does either page still owe a CARD?  The panel regions are excluded on
// purpose: they are the three small regions the ordinary painter writes live
// every frame of a duel, and they are never worth blanking the screen for.
static bool Msx2_BoardOwesSlots(void)
{
	u8 p, i;

	for(p = 0; p < MSX2_VIDEO_PAGES; ++p)
		for(i = 0; i < SLOT_COUNT; ++i)
			if((g_shown[p][i] != g_want[i]) ||
			   (g_shown_flag[p][i] != g_flag[i]))
				return TRUE;
	return FALSE;
}

static void Msx2_BoardOverBegin(void)
{
	const c8* word;
	u8 p, i;

	g_mode = M_OVER;
	Msx2_BoardTouch();
	// The banner's board is the overhead one whichever chair the last blow was
	// struck from: it is the only view that shows both rows whole.
	Msx2_BoardCutTo(MSX2_VIEW_OVER);

	// ... AND ONLY IF ANYTHING IS ACTUALLY OWED.
	// The restore that brought the final board back has already put every slot
	// on both pages, so the queue is usually empty here -- and blanking the
	// output to run a loop that paints nothing is a black frame the player is
	// shown for no reason at all.  The panel is the one thing the result
	// changes, and it is three regions, not a board.
	if(Msx2_BoardOwesSlots())
	{
		VDP_EnableDisplay(FALSE);
		for(p = 0; p < MSX2_VIDEO_PAGES; ++p)
		{
			Msx2_VideoDrawPage(p);
			// One card and the three panel regions per pass, so a slot count of
			// passes clears the whole queue with room to spare.
			for(i = 0; i < SLOT_COUNT + 4; ++i)
				if(!Msx2_BoardPaint())
					break;
		}
		Msx2_VideoDrawPage((u8)(Msx2_VideoGetShowPage() ^ 1));
		VDP_EnableDisplay(TRUE);
	}
	else
	{
		// Only the words under the board have changed.  They go on with the
		// output up: three regions a page, which is less than the painter does
		// on an ordinary frame.
		for(p = 0; p < MSX2_VIDEO_PAGES; ++p)
		{
			Msx2_VideoDrawPage(p);
			for(i = 0; i < 4; ++i)
				if(!Msx2_BoardPaint())
					break;
		}
		Msx2_VideoDrawPage((u8)(Msx2_VideoGetShowPage() ^ 1));
	}

	word = (g_duel.result > 0) ? Msx2_UiText(MSX2_S_YOU_WIN)
	                           : Msx2_UiText(MSX2_S_YOU_LOSE);
	for(i = 0; (i < MSX2_SPR_LETTER_N) && (word[i] != 0); ++i)
		g_over_text[i] = word[i];
	g_over_text[i] = 0;
	g_over_color = (g_duel.result > 0) ? MSX2_SPR_GOLD : MSX2_SPR_RED;
	g_over_n = Msx2_SpriteWord(g_over_text);
	g_over_step = 0;
}

// One frame of the word coming in.  It starts wholly off the left edge and
// eases to the middle: a quadratic on the REMAINING distance, so it arrives
// slowing down rather than stopping dead.
//
// The arithmetic is deliberately two divisions rather than one.  The word is up
// to eight letters at a 26-pixel pitch, so the distance travelled is about 230,
// and 230 * 30 * 30 does not fit in the 16 bits a Z80 multiplies in.  Dividing
// between the two multiplications keeps every intermediate under four thousand.
static void Msx2_BoardStepOverWord(void)
{
	i16 span, home, rem, x;

	if(g_over_step > OVER_SLIDE_STEPS + OVER_HOLD_STEPS)
		return;
	if(g_over_step >= OVER_SLIDE_STEPS)
	{
		// Landed.  The sprites are already where they belong and nothing under
		// them moves, so the hold costs nothing but the count.
		++g_over_step;
		return;
	}

	span = (i16)((u16)g_over_n * MSX2_SPR_WORD_PITCH);
	home = (i16)((MSX2_SCREEN_W - span) / 2);
	rem  = (i16)(OVER_SLIDE_STEPS - g_over_step);
	x    = (i16)(home - ((((home + span) * rem) / OVER_SLIDE_STEPS) * rem)
	                    / OVER_SLIDE_STEPS);

	Msx2_SpriteWordAt(g_over_text, g_over_n, x, MSX2_OVER_WORD_Y, g_over_color);
	++g_over_step;
}

// The word has arrived and the player has had a moment with the board.  Only
// then is a button worth anything: without this, a held A -- or the soak's
// permanently held one -- ended the duel on the frame the banner was created
// and the screen this whole path exists to show was never on the screen.
static bool Msx2_BoardOverIsSettled(void)
{
	return g_over_step > OVER_SLIDE_STEPS + OVER_HOLD_STEPS;
}

// ─────────────────────────────────────────────────────────────────────────────
//  The frame
// ─────────────────────────────────────────────────────────────────────────────

u8 Msx2_BoardStep_In(void)
{
	u8 pressed = Msx2_InputPressed();
	u8 before_zone = g_zone;
	u8 before_sel = g_sel;
	bool cursor_moved = FALSE;

	// The selector is a sprite: it has to be taken off the screen by something,
	// and every branch below can return before the paint.  Doing it here, once,
	// covers all of them -- at the cost of the cursor being one frame behind a
	// press, which at 20 pixels and sixty hertz is not a thing anyone can see.
	Msx2_BoardShowCursor();

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
		if((pressed & (MSX2_BTN_A | MSX2_BTN_B)) && Msx2_BoardOverIsSettled())
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
			Msx2_BoardTouchRules();
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
				PANEL_ALL();
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
		{
			// Remove the old location before doing any page/card work.  The next
			// location is shown again below in this same frame, eliminating the
			// one-frame lag that made hand card 1 -> 2 feel sticky.
			Msx2_SpriteHideGem();
			cursor_moved = TRUE;
			Msx2_BoardMove(pressed);
		}

		if((Msx2_InputTyped() == 'C') && (g_mode == M_IDLE))
		{
			Msx2_BoardCheck();
			return MSX2_BOARD_BUSY;
		}

		if(Msx2_InputXKey() && (g_mode == M_IDLE) && (g_zone == ZONE_FIELD) &&
		   (Msx2_BoardHovered() != MSX2_CARD_NONE) &&
		   Msx2_ChangePosition(MSX2_OWNER_PLAYER, g_sel))
		{
			Msx2_SfxPlay(MSX2_SFX_CONFIRM);
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
			PANEL_ALL();
	}
	if((g_zone != before_zone) || (g_sel != before_sel))
	{
		Msx2_SfxPlay(MSX2_SFX_SELECT);
		// Only what actually changed.  Sliding along a row renames the card;
		// the life points and the prompt are the same words they were, and
		// repainting them is what the move used to wait for.
		g_card_left = PAGES_ALL;
		if(g_zone != before_zone)
			g_prompt_left = PAGES_ALL;
	}
	// A decided duel is not walked around any more.  Without the result test
	// here the camera answered the cursor's row one last time -- a blanked
	// stream of a whole view -- and Msx2_BoardOverBegin() then cut straight
	// back out of it: half a second of black on the way to a screen that was
	// always going to be the overhead one.
	if((g_fx_kind == FX_NONE) && (g_mode != M_TURN) && (g_mode != M_COM) &&
	   (g_mode != M_DEAL) && (g_mode != M_OVER) && (g_duel.result == 0) &&
	   (g_view != BOARD_VIEW_COM))
	{
		bool in_hand = (g_zone == ZONE_HAND);
		Msx2_BoardHandVisible(in_hand);
		Msx2_BoardCutTo(in_hand ? BOARD_VIEW_PLAYER : MSX2_VIEW_OVER);
	}
	if(cursor_moved && (g_fx_kind == FX_NONE) && (g_mode != M_TURN) &&
	   (g_mode != M_COM) && (g_mode != M_DEAL) && (g_mode != M_CHECK) &&
	   (g_mode != M_OVER))
		Msx2_BoardShowCursor();
	if(g_mode == M_TURN)
	{
		Msx2_BoardStepCameraMove();
		return MSX2_BOARD_BUSY;
	}

	Msx2_BoardStartFx();
	if(Msx2_BoardRunFx())
		return MSX2_BOARD_BUSY;

	// THE RESULT COMES LAST.
	// This used to be tested before Msx2_BoardStartFx(), on the frame the rules
	// set it -- which is the very frame the winning attack is still owed its
	// damage screen.  The banner went up, and then the full-screen battle
	// cut-in played over the top of it and put the board back underneath a word
	// nobody had been given a chance to read.  The test belongs here, past
	// StartFx and past a running effect: by the time it is reached the last
	// action has had its screen, the bend has put the arena back, and the
	// snapshot below is of a field the destroyed monster has already left.
	if((g_duel.result != 0) && (g_mode != M_OVER))
		Msx2_BoardOverBegin();

	if(Msx2_BoardPaint())
		Msx2_VideoFlipRequest();

	if(g_mode == M_OVER)
		Msx2_BoardStepOverWord();

	return MSX2_BOARD_BUSY;
}
