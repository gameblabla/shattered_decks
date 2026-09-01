// ─────────────────────────────────────────────────────────────────────────────
//  Shattered Decks — MSX2 port
//  msx2_duel.h — the duel rules model
//
//  This is the Z80 fork of the duel rules that live inside src/main.c on the
//  framebuffer targets.  It is render-free and animation-free: it owns board
//  state, life points, decks, battle resolution, support cards and the turn
//  order, and nothing else.  Presentation asks it questions; it never draws.
//
//  Card ids are u8 here (72 monsters + 6 support variants = 78 ids), unlike the
//  shared `int` model, because the whole point of this file is to fit the state
//  in a couple of hundred bytes of a ~12 KB RAM budget.  MSX2_CARD_NONE is the
//  empty marker; the shared code's -1 does not survive the narrowing.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "msxgl.h"
#include "deck.h"

#define MSX2_HAND           5
#define MSX2_FIELD          5
#define MSX2_CARD_NONE      0xFFu
#define MSX2_SLOT_NONE      0xFFu

#define MSX2_START_LP       8000
#define MSX2_HEAL_AMOUNT    1000

#define MSX2_OWNER_PLAYER   0
#define MSX2_OWNER_COM      1

// Story mode: five duels, the last of which is fought on a water field.
#define MSX2_STORY_MAX_DUELS   5
#define MSX2_STORY_FINAL_DUEL  (MSX2_STORY_MAX_DUELS - 1)
#define MSX2_STORY_NONE        0xFFu

// Support card kinds, matching support_card_kind() in src/main.c.
#define MSX2_SUP_EQUIP      0
#define MSX2_SUP_GUARD      1
#define MSX2_SUP_DRAW       2
#define MSX2_SUP_HEAL       3
#define MSX2_SUP_THUNDER    4
#define MSX2_SUP_TRAP       5

// Battle outcomes, matching BattleOutcome in src/main.c.
enum Msx2BattleOutcome
{
	MSX2_BATTLE_NONE = 0,
	MSX2_BATTLE_DESTROY_DEFENDER,
	MSX2_BATTLE_DESTROY_ATTACKER,
	MSX2_BATTLE_DESTROY_BOTH,
	MSX2_BATTLE_NO_DESTROY,
	MSX2_BATTLE_DIRECT,
};

// A rules action is retained for one presentation pass.  The Z80 rules model
// applies an action atomically, then the board uses this small record to show
// the card choosing/landing or battle effect without putting rendering back in
// the rules module.
enum Msx2ActionEvent
{
	MSX2_ACTION_NONE = 0,
	MSX2_ACTION_PLACE,
	MSX2_ACTION_FUSION,
	MSX2_ACTION_EQUIP,
	MSX2_ACTION_SUPPORT,
	MSX2_ACTION_ATTACK,
	MSX2_ACTION_POSITION
};

// One side of the board.  Two of these plus a handful of scalars is the whole
// duel state.
typedef struct Msx2Side
{
	u8  hand[MSX2_HAND];          // card id or MSX2_CARD_NONE
	u8  used[MSX2_HAND];          // hand slot spent this turn
	u8  field[MSX2_FIELD];        // monster row
	u8  faceup[MSX2_FIELD];
	u8  defense[MSX2_FIELD];      // 1 = defence position
	u8  attacked[MSX2_FIELD];     // already declared an attack this turn
	i16 atk_bonus[MSX2_FIELD];    // from equips
	i16 def_bonus[MSX2_FIELD];
	u8  equip_field[MSX2_FIELD];  // support row: equips and set traps
	i8  equip_target[MSX2_FIELD]; // monster slot an equip is attached to, or -1
	i16 lp;
	WaifuDeck deck;
	u8  monster_played;           // one monster placement per turn
} Msx2Side;

// The outcome of one attack, computed before it is applied (the port's
// BattleCalc).
typedef struct Msx2BattleCalc
{
	i16 attacker_atk;
	i16 defender_value;
	i16 delta;
	i16 damage;
	i8  damage_owner;             // 0/1, or -1 for none
	u8  defender_passive;
	u8  outcome;                  // enum Msx2BattleOutcome
} Msx2BattleCalc;

// Turn phases.  Deliberately coarser than the framebuffer targets' 25-entry
// WaifuBattlePhase: everything there that exists to time an animation collapses
// into the presentation layer here, and the rules only care about who acts.
enum Msx2Phase
{
	MSX2_PHASE_TURN_START = 0,
	MSX2_PHASE_MAIN,              // place a monster / play a support card
	MSX2_PHASE_BATTLE,            // declare attacks
	MSX2_PHASE_TURN_END,
	MSX2_PHASE_RESULT,
};

typedef struct Msx2Duel
{
	Msx2Side side[2];
	WaifuDeckRng rng;
	u16 turns;                    // turns taken, 1-based once the duel opens
	u8  phase;                    // enum Msx2Phase
	u8  turn_owner;               // 0 = player, 1 = COM
	i8  result;                   // 0 = running, 1 = player won, -1 = player lost
	u8  story_duel_index;         // MSX2_STORY_NONE = free battle
	u8  main_actions;             // actions spent in the main phase this turn
	u8  story_active;
	// Last resolved battle, kept so the presentation layer can animate it after
	// the rules have already moved on.
	Msx2BattleCalc last_battle;
	u8  last_attacker_owner;
	u8  last_attacker_slot;
	u8  last_defender_slot;
	u8  last_attacker_card;
	u8  last_defender_card;
	u8  last_trap_fired;
	u8  last_action;                // enum Msx2ActionEvent
	u8  last_action_card;
	u8  last_action_hand_slot;
	u8  last_action_field_slot;
	u8  last_action_owner;
	u8  last_action_defense;
} Msx2Duel;

extern Msx2Duel g_duel;

// ── Card queries ────────────────────────────────────────────────────────────
bool Msx2_IsMonster(u8 card);
bool Msx2_IsSupport(u8 card);
u8   Msx2_SupportKind(u8 card);
u16  Msx2_CardAtk(u8 card);
u16  Msx2_CardDef(u8 card);
u8   Msx2_FusionResult(u8 a, u8 b);

// The most materials one fusion can consume: five hand cards plus the monster
// already standing in the target slot (FUSION_MAX_MATERIALS in src/main.c).
#define MSX2_FUSION_MAX     6

// The chain recipes, which consume every monster material at once rather than
// folding them in pairs.  Returns MSX2_CARD_NONE when no rule matches.
u8   Msx2_FusionChainResult(const u8* cards, u8 count);

// ── Board queries ───────────────────────────────────────────────────────────
i16  Msx2_FieldAtk(u8 owner, u8 slot);
i16  Msx2_FieldDef(u8 owner, u8 slot);
u8   Msx2_LiveMonsterCount(u8 owner);
u8   Msx2_FirstLiveSlot(u8 owner);
u8   Msx2_FirstFreeSlot(u8 owner);
u8   Msx2_FirstFreeEquipSlot(u8 owner);
bool Msx2_FirstTurnAttackLocked(void);

// ── Duel lifecycle ──────────────────────────────────────────────────────────
// story_duel_index is 0xFF for a free battle, otherwise the story opponent
// index, which selects the scripted COM deck and the final duel's water field.
void Msx2_DuelInit(u32 seed, u8 story_duel_index);

// One rules step: performs at most one discrete action (a draw, a placement, an
// attack, a phase change) and returns TRUE while the duel is still running.
// The COM side is driven from here, and so is the player's in the soak build
// (-DMSX2_DEBUG_AUTOPLAY), where a duel plays to completion with no input.
bool Msx2_DuelStep(void);

// ── Actions (the presentation layer calls these for the human player) ────────
bool Msx2_PlaceMonster(u8 owner, u8 hand_slot, u8 field_slot, bool defense);

// A fusion summon from several hand cards at once, optionally onto the monster
// already in `field_slot`.  `hand_slots` is the order the player chose them in,
// which is part of the rule: the materials fold left to right.
bool Msx2_PlaceFusion(u8 owner, const u8* hand_slots, u8 count, u8 field_slot,
                      bool defense);
bool Msx2_PlaySupport(u8 owner, u8 hand_slot, u8 target_slot);
bool Msx2_Attack(u8 owner, u8 attacker_slot, u8 defender_slot); // MSX2_SLOT_NONE = direct
void Msx2_EndTurn(void);

// Consume the most recent rules action.  It is deliberately separate from the
// action itself: a presentation can spend several frames showing it.
void Msx2_ClearActionEvent(void);
