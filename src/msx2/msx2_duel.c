// ─────────────────────────────────────────────────────────────────────────────
//  msx2_duel.c — duel rules for the MSX2 port
//
//  Ported from the rules interleaved through src/main.c (calc_battle_state,
//  prepare_battle, resolve_battle, finish_thunder, finish_equip, the support
//  card effects and the IB_* turn order).  Everything here is render-free: the
//  framebuffer targets' phase machine exists mostly to time animations, and
//  none of that timing is reproduced.  What is reproduced exactly is the
//  arithmetic, because a card game that computes damage differently from the
//  other five ports is a different game.
//
//  Deliberate divergences from src/main.c, all noted where they occur:
//    * traps are owner-generic here rather than player-only (the AI never sets
//      one, so the two are behaviourally identical);
//    * fusion is the pairwise chain without the animation bookkeeping;
//    * the player side is currently driven by the same AI as the COM, so a
//      duel plays out with no input at all (see Msx2_DuelStep).
// ─────────────────────────────────────────────────────────────────────────────

#include "msx2_duel.h"
#include "msx2_cards.h"
#include "ai.h"
#include "card_ids.h"

Msx2Duel g_duel;

// ─────────────────────────────────────────────────────────────────────────────
//  Card queries
// ─────────────────────────────────────────────────────────────────────────────

bool Msx2_IsMonster(u8 card)
{
	return card < MSX2_CARD_COUNT;
}

bool Msx2_IsSupport(u8 card)
{
	return (card >= MSX2_CARD_COUNT) && (card < MSX2_TOTAL_CARDS);
}

u8 Msx2_SupportKind(u8 card)
{
	if(card < MSX2_CARD_COUNT)
		return MSX2_SUP_EQUIP;
	return (u8)((card - MSX2_CARD_COUNT) % MSX2_SUPPORT_VARIANTS);
}

static bool Msx2_IsEquipSupport(u8 card)
{
	if(!Msx2_IsSupport(card))
		return FALSE;
	u8 kind = Msx2_SupportKind(card);
	return (kind == MSX2_SUP_EQUIP) || (kind == MSX2_SUP_GUARD);
}

static bool Msx2_IsKind(u8 card, u8 kind)
{
	return Msx2_IsSupport(card) && (Msx2_SupportKind(card) == kind);
}

u16 Msx2_CardAtk(u8 card)
{
	return Msx2_IsMonster(card) ? g_msx2_card_atk[card] : 0;
}

u16 Msx2_CardDef(u8 card)
{
	return Msx2_IsMonster(card) ? g_msx2_card_def[card] : 0;
}

// Equip bonuses, from equip_atk_bonus()/equip_def_bonus() in src/main.c.
static i16 Msx2_EquipAtkBonus(u8 card)
{
	return Msx2_IsKind(card, MSX2_SUP_GUARD) ? 250 : 500;
}

static i16 Msx2_EquipDefBonus(u8 card)
{
	if(Msx2_IsKind(card, MSX2_SUP_GUARD))
		return 800;
	return (Msx2_SupportKind(card) == MSX2_SUP_EQUIP) ? 300 : 0;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Fusion
// ─────────────────────────────────────────────────────────────────────────────

typedef struct Msx2FusionRule { u8 a, b, result; } Msx2FusionRule;

// The same twelve recipes as g_fusion_rules in src/main.c, by card id.
static const Msx2FusionRule g_fusion_rules[] =
{
	{ WAIFU_CARD_ID_COCINELLE,    WAIFU_CARD_ID_BEE_WOMAN,      WAIFU_CARD_ID_INSECT_QUEEN },
	{ WAIFU_CARD_ID_COCINELLE,    WAIFU_CARD_ID_REPTILE,        WAIFU_CARD_ID_INSECT_QUEEN },
	{ WAIFU_CARD_ID_PARROT,       WAIFU_CARD_ID_PENGUIN,        WAIFU_CARD_ID_OWL_WOMAN },
	{ WAIFU_CARD_ID_RAT,          WAIFU_CARD_ID_SLIME,          WAIFU_CARD_ID_SQUID_TENTACLES },
	{ WAIFU_CARD_ID_SNAKE,        WAIFU_CARD_ID_CROW,           WAIFU_CARD_ID_COBRA },
	{ WAIFU_CARD_ID_GOLEM_IDOL,   WAIFU_CARD_ID_ELECTRIC,       WAIFU_CARD_ID_STONE_DRAGON },
	{ WAIFU_CARD_ID_JESTER,       WAIFU_CARD_ID_PRIESTESS,      WAIFU_CARD_ID_MAGE_SKELETON },
	{ WAIFU_CARD_ID_TURTLE,       WAIFU_CARD_ID_REPTILE,        WAIFU_CARD_ID_SHARK },
	{ WAIFU_CARD_ID_WITCH,        WAIFU_CARD_ID_PENGUIN,        WAIFU_CARD_ID_PUMPKIN },
	{ WAIFU_CARD_ID_INSECT_BOMB,  WAIFU_CARD_ID_INSECT_SOLDIER, WAIFU_CARD_ID_INSECT_QUEEN_WOMAN },
	{ WAIFU_CARD_ID_ELEC_WOLF,    WAIFU_CARD_ID_CROW,           WAIFU_CARD_ID_YOKAI },
	{ WAIFU_CARD_ID_STREET,       WAIFU_CARD_ID_BEE_WOMAN,      WAIFU_CARD_ID_WARRIOR },
};

#define MSX2_FUSION_RULE_COUNT (sizeof(g_fusion_rules) / sizeof(g_fusion_rules[0]))

u8 Msx2_FusionResult(u8 a, u8 b)
{
	u8 i;
	for(i = 0; i < MSX2_FUSION_RULE_COUNT; ++i)
	{
		if(((g_fusion_rules[i].a == a) && (g_fusion_rules[i].b == b)) ||
		   ((g_fusion_rules[i].a == b) && (g_fusion_rules[i].b == a)))
			return g_fusion_rules[i].result;
	}
	// The generic Water recipe: two Water monsters fuse by strength band.
	if(Msx2_IsMonster(a) && Msx2_IsMonster(b) &&
	   (g_msx2_card_attr[a] == MSX2_ATTR_WATER) && (g_msx2_card_attr[b] == MSX2_ATTR_WATER))
	{
		u16 atk_a = g_msx2_card_atk[a];
		u16 atk_b = g_msx2_card_atk[b];
		if((atk_a >= 2000) && (atk_b >= 2000))
			return WAIFU_CARD_ID_ANGEL_FISHWOMAN;
		if((atk_a < 2000) && (atk_b < 2000))
			return WAIFU_CARD_ID_SEA_SERPENT;
	}
	return MSX2_CARD_NONE;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Board queries
// ─────────────────────────────────────────────────────────────────────────────

// The final story duel is fought on a water field: Water/Fish/Aqua gain 500 ATK
// and DEF, Fire/Insect/Machine lose 500 (story_water_field_bonus in main.c).
static i16 Msx2_WaterFieldBonus(u8 card)
{
	if(!g_duel.story_active || !Msx2_IsMonster(card))
		return 0;
	if(g_duel.story_duel_index != MSX2_STORY_FINAL_DUEL)
		return 0;
	if((g_msx2_card_attr[card] == MSX2_ATTR_WATER) ||
	   (g_msx2_card_tribe[card] == MSX2_TRIBE_FISH) ||
	   (g_msx2_card_tribe[card] == MSX2_TRIBE_AQUA))
		return 500;
	if((g_msx2_card_attr[card] == MSX2_ATTR_FIRE) ||
	   (g_msx2_card_tribe[card] == MSX2_TRIBE_INSECT) ||
	   (g_msx2_card_tribe[card] == MSX2_TRIBE_MACHINE))
		return -500;
	return 0;
}

i16 Msx2_FieldAtk(u8 owner, u8 slot)
{
	if(slot >= MSX2_FIELD)
		return 0;
	u8 card = g_duel.side[owner].field[slot];
	if(!Msx2_IsMonster(card))
		return 0;
	return (i16)g_msx2_card_atk[card] + Msx2_WaterFieldBonus(card) +
	       g_duel.side[owner].atk_bonus[slot];
}

i16 Msx2_FieldDef(u8 owner, u8 slot)
{
	if(slot >= MSX2_FIELD)
		return 0;
	u8 card = g_duel.side[owner].field[slot];
	if(!Msx2_IsMonster(card))
		return 0;
	return (i16)g_msx2_card_def[card] + Msx2_WaterFieldBonus(card) +
	       g_duel.side[owner].def_bonus[slot];
}

u8 Msx2_LiveMonsterCount(u8 owner)
{
	u8 i, n = 0;
	for(i = 0; i < MSX2_FIELD; ++i)
		if(Msx2_IsMonster(g_duel.side[owner].field[i]))
			++n;
	return n;
}

u8 Msx2_FirstLiveSlot(u8 owner)
{
	u8 i;
	for(i = 0; i < MSX2_FIELD; ++i)
		if(Msx2_IsMonster(g_duel.side[owner].field[i]))
			return i;
	return MSX2_SLOT_NONE;
}

u8 Msx2_FirstFreeSlot(u8 owner)
{
	u8 i;
	for(i = 0; i < MSX2_FIELD; ++i)
		if(!Msx2_IsMonster(g_duel.side[owner].field[i]))
			return i;
	return MSX2_SLOT_NONE;
}

u8 Msx2_FirstFreeEquipSlot(u8 owner)
{
	u8 i;
	for(i = 0; i < MSX2_FIELD; ++i)
		if(g_duel.side[owner].equip_field[i] == MSX2_CARD_NONE)
			return i;
	return MSX2_SLOT_NONE;
}

// Yu-Gi-Oh-style opener rule: whoever opens the duel cannot attack on their
// first turn (player_first_turn_attack_locked in src/main.c).
bool Msx2_FirstTurnAttackLocked(void)
{
	return (g_duel.turn_owner == MSX2_OWNER_PLAYER) && (g_duel.turns <= 1);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Slot bookkeeping
// ─────────────────────────────────────────────────────────────────────────────

static void Msx2_ClearMonsterSlot(u8 owner, u8 slot)
{
	if(slot >= MSX2_FIELD)
		return;
	Msx2Side* s = &g_duel.side[owner];
	s->field[slot] = MSX2_CARD_NONE;
	s->faceup[slot] = TRUE;
	s->defense[slot] = FALSE;
	s->attacked[slot] = FALSE;
	s->atk_bonus[slot] = 0;
	s->def_bonus[slot] = 0;
	// Equips attached to the destroyed monster go with it.
	u8 i;
	for(i = 0; i < MSX2_FIELD; ++i)
	{
		if((s->equip_target[i] == (i8)slot) && Msx2_IsEquipSupport(s->equip_field[i]))
		{
			s->equip_field[i] = MSX2_CARD_NONE;
			s->equip_target[i] = -1;
		}
	}
}

// ─────────────────────────────────────────────────────────────────────────────
//  Battle
// ─────────────────────────────────────────────────────────────────────────────

static bool Msx2_DefenderPassive(u8 owner, u8 slot)
{
	if(slot >= MSX2_FIELD)
		return TRUE;
	if(!Msx2_IsMonster(g_duel.side[owner].field[slot]))
		return TRUE;
	return g_duel.side[owner].defense[slot] ? TRUE : FALSE;
}

static i16 Msx2_DefenderBattleValue(u8 owner, u8 slot)
{
	if(Msx2_DefenderPassive(owner, slot))
		return Msx2_FieldDef(owner, slot);
	return Msx2_FieldAtk(owner, slot);
}

// calc_battle_state() in src/main.c, unchanged arithmetic.
static void Msx2_CalcBattle(Msx2BattleCalc* bc, u8 atk_owner, u8 atk_slot, u8 def_owner, u8 def_slot)
{
	bc->damage = 0;
	bc->damage_owner = -1;
	bc->outcome = MSX2_BATTLE_NONE;
	bc->attacker_atk = Msx2_FieldAtk(atk_owner, atk_slot);
	bc->defender_value = Msx2_DefenderBattleValue(def_owner, def_slot);
	bc->defender_passive = Msx2_DefenderPassive(def_owner, def_slot);
	bc->delta = bc->attacker_atk - bc->defender_value;

	if(bc->defender_passive)
	{
		if(bc->delta > 0)
		{
			bc->outcome = MSX2_BATTLE_DESTROY_DEFENDER;
		}
		else if(bc->delta < 0)
		{
			// A defence-position defender never counter-destroys; the attacker
			// only takes the passive value gap as damage.
			bc->outcome = MSX2_BATTLE_NO_DESTROY;
			bc->damage = -bc->delta;
			bc->damage_owner = (i8)atk_owner;
		}
		else
		{
			bc->outcome = MSX2_BATTLE_NO_DESTROY;
		}
	}
	else
	{
		if(bc->delta > 0)
		{
			bc->outcome = MSX2_BATTLE_DESTROY_DEFENDER;
			bc->damage = bc->delta;
			bc->damage_owner = (i8)def_owner;
		}
		else if(bc->delta < 0)
		{
			bc->outcome = MSX2_BATTLE_DESTROY_ATTACKER;
			bc->damage = -bc->delta;
			bc->damage_owner = (i8)atk_owner;
		}
		else
		{
			bc->outcome = MSX2_BATTLE_DESTROY_BOTH;
		}
	}
}

static void Msx2_CheckResult(void)
{
	if(g_duel.side[MSX2_OWNER_PLAYER].lp <= 0)
	{
		g_duel.side[MSX2_OWNER_PLAYER].lp = 0;
		g_duel.result = -1;
		g_duel.phase = MSX2_PHASE_RESULT;
	}
	else if(g_duel.side[MSX2_OWNER_COM].lp <= 0)
	{
		g_duel.side[MSX2_OWNER_COM].lp = 0;
		g_duel.result = 1;
		g_duel.phase = MSX2_PHASE_RESULT;
	}
}

// The defending side's set trap auto-fires against a declared attack: it
// destroys the attacker and cancels the attack before the battle step.
// try_trigger_player_trap() in src/main.c is player-only; the AI never sets a
// trap, so making it owner-generic here changes no observable behaviour.
static bool Msx2_TryTriggerTrap(u8 defending_owner, u8 attacker_owner, u8 attacker_slot)
{
	u8 i;
	if(attacker_slot >= MSX2_FIELD)
		return FALSE;
	if(!Msx2_IsMonster(g_duel.side[attacker_owner].field[attacker_slot]))
		return FALSE;
	for(i = 0; i < MSX2_FIELD; ++i)
	{
		if(Msx2_IsKind(g_duel.side[defending_owner].equip_field[i], MSX2_SUP_TRAP))
		{
			g_duel.side[defending_owner].equip_field[i] = MSX2_CARD_NONE;
			g_duel.side[defending_owner].equip_target[i] = -1;
			Msx2_ClearMonsterSlot(attacker_owner, attacker_slot);
			g_duel.last_trap_fired = TRUE;
			return TRUE;
		}
	}
	return FALSE;
}

// prepare_battle() + resolve_battle() collapsed: with no animation to schedule
// between them there is no reason to hold the calculation across frames.
bool Msx2_Attack(u8 owner, u8 attacker_slot, u8 defender_slot)
{
	u8 def_owner = owner ? MSX2_OWNER_PLAYER : MSX2_OWNER_COM;
	Msx2Side* atk_side = &g_duel.side[owner];
	Msx2BattleCalc bc;

	if(attacker_slot >= MSX2_FIELD)
		return FALSE;
	if(!Msx2_IsMonster(atk_side->field[attacker_slot]))
		return FALSE;
	if(atk_side->defense[attacker_slot])         // defence position cannot attack
		return FALSE;
	if(atk_side->attacked[attacker_slot])
		return FALSE;
	if((owner == MSX2_OWNER_PLAYER) && Msx2_FirstTurnAttackLocked())
		return FALSE;

	g_duel.last_trap_fired = FALSE;
	if(Msx2_TryTriggerTrap(def_owner, owner, attacker_slot))
		return TRUE;                              // attack cancelled by the trap

	// Hard rule: a direct attack is illegal while the opponent holds a monster.
	if((defender_slot == MSX2_SLOT_NONE) && (Msx2_LiveMonsterCount(def_owner) > 0))
		defender_slot = Msx2_FirstLiveSlot(def_owner);

	atk_side->attacked[attacker_slot] = TRUE;
	atk_side->faceup[attacker_slot] = TRUE;
	g_duel.last_attacker_owner = owner;
	g_duel.last_attacker_slot = attacker_slot;
	g_duel.last_defender_slot = defender_slot;
	g_duel.last_attacker_card = atk_side->field[attacker_slot];

	if(defender_slot == MSX2_SLOT_NONE)
	{
		i16 dmg = Msx2_FieldAtk(owner, attacker_slot);
		bc.attacker_atk = dmg;
		bc.defender_value = 0;
		bc.delta = dmg;
		bc.damage = dmg;
		bc.damage_owner = (i8)def_owner;
		bc.defender_passive = FALSE;
		bc.outcome = MSX2_BATTLE_DIRECT;
		g_duel.last_defender_card = MSX2_CARD_NONE;
		g_duel.side[def_owner].lp -= dmg;
	}
	else
	{
		if(!Msx2_IsMonster(g_duel.side[def_owner].field[defender_slot]))
			return FALSE;
		Msx2_CalcBattle(&bc, owner, attacker_slot, def_owner, defender_slot);
		g_duel.last_defender_card = g_duel.side[def_owner].field[defender_slot];
		g_duel.side[def_owner].faceup[defender_slot] = TRUE;

		if(bc.outcome == MSX2_BATTLE_DESTROY_DEFENDER)
		{
			Msx2_ClearMonsterSlot(def_owner, defender_slot);
		}
		else if(bc.outcome == MSX2_BATTLE_DESTROY_ATTACKER)
		{
			Msx2_ClearMonsterSlot(owner, attacker_slot);
		}
		else if(bc.outcome == MSX2_BATTLE_DESTROY_BOTH)
		{
			Msx2_ClearMonsterSlot(owner, attacker_slot);
			Msx2_ClearMonsterSlot(def_owner, defender_slot);
		}

		if(bc.damage > 0)
			g_duel.side[bc.damage_owner].lp -= bc.damage;
	}

	g_duel.last_battle = bc;
	Msx2_CheckResult();
	return TRUE;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Placement and support cards
// ─────────────────────────────────────────────────────────────────────────────

static u8 Msx2_Draw(u8 owner)
{
	int card = waifu_deck_draw(&g_duel.side[owner].deck);
	if(card < 0)
		return MSX2_CARD_NONE;
	return (u8)card;
}

bool Msx2_PlaceMonster(u8 owner, u8 hand_slot, u8 field_slot, bool defense)
{
	Msx2Side* s = &g_duel.side[owner];
	if((hand_slot >= MSX2_HAND) || (field_slot >= MSX2_FIELD))
		return FALSE;
	if(s->used[hand_slot])
		return FALSE;
	if(s->monster_played)
		return FALSE;

	u8 card = s->hand[hand_slot];
	if(!Msx2_IsMonster(card))
		return FALSE;

	// Placing onto an occupied slot is a fusion attempt (the pairwise recipe of
	// fusion_result_for_cards); a failed pair simply replaces nothing and the
	// placement is refused, as the shared code refuses it.
	u8 occupant = s->field[field_slot];
	if(Msx2_IsMonster(occupant))
	{
		u8 fused = Msx2_FusionResult(occupant, card);
		if(!Msx2_IsMonster(fused))
			return FALSE;
		card = fused;
		// The fused monster inherits the field slot's equip bonuses.
	}
	else
	{
		s->atk_bonus[field_slot] = 0;
		s->def_bonus[field_slot] = 0;
	}

	s->field[field_slot] = card;
	s->faceup[field_slot] = (owner == MSX2_OWNER_PLAYER) ? TRUE : FALSE;
	s->defense[field_slot] = defense ? TRUE : FALSE;
	s->attacked[field_slot] = FALSE;
	s->used[hand_slot] = TRUE;
	s->hand[hand_slot] = MSX2_CARD_NONE;
	s->monster_played = TRUE;
	return TRUE;
}

// Thunder destroys every monster on the opposing field; equip/guard attach to a
// target monster; draw and heal are one-shots; a trap is set face down on the
// support row and stays inert until an attack is declared.
bool Msx2_PlaySupport(u8 owner, u8 hand_slot, u8 target_slot)
{
	Msx2Side* s = &g_duel.side[owner];
	u8 other = owner ? MSX2_OWNER_PLAYER : MSX2_OWNER_COM;
	u8 i;

	if(hand_slot >= MSX2_HAND)
		return FALSE;
	if(s->used[hand_slot])
		return FALSE;

	u8 card = s->hand[hand_slot];
	if(!Msx2_IsSupport(card))
		return FALSE;

	switch(Msx2_SupportKind(card))
	{
	case MSX2_SUP_EQUIP:
	case MSX2_SUP_GUARD:
	{
		u8 zone = Msx2_FirstFreeEquipSlot(owner);
		if((target_slot >= MSX2_FIELD) || (zone == MSX2_SLOT_NONE))
			return FALSE;
		if(!Msx2_IsMonster(s->field[target_slot]))
			return FALSE;
		s->equip_field[zone] = card;
		s->equip_target[zone] = (i8)target_slot;
		s->atk_bonus[target_slot] += Msx2_EquipAtkBonus(card);
		s->def_bonus[target_slot] += Msx2_EquipDefBonus(card);
		break;
	}

	case MSX2_SUP_DRAW:
	{
		u8 drawn = Msx2_Draw(owner);
		if(drawn == MSX2_CARD_NONE)
			return FALSE;
		// The drawn card lands back in the slot the support card just left.
		s->hand[hand_slot] = drawn;
		s->used[hand_slot] = FALSE;
		return TRUE;
	}

	case MSX2_SUP_HEAL:
		s->lp += MSX2_HEAL_AMOUNT;
		break;

	case MSX2_SUP_THUNDER:
		for(i = 0; i < MSX2_FIELD; ++i)
			if(Msx2_IsMonster(g_duel.side[other].field[i]))
				Msx2_ClearMonsterSlot(other, i);
		break;

	case MSX2_SUP_TRAP:
	{
		u8 zone = Msx2_FirstFreeEquipSlot(owner);
		if(zone == MSX2_SLOT_NONE)
			return FALSE;
		s->equip_field[zone] = card;
		s->equip_target[zone] = -1;
		break;
	}

	default:
		return FALSE;
	}

	s->used[hand_slot] = TRUE;
	s->hand[hand_slot] = MSX2_CARD_NONE;
	return TRUE;
}

// draw_replacement_cards_to_hand(): refill every spent slot, and if nothing was
// spent, replace slot 0 anyway (the anti-stall discard) -- but never on the
// opening turn, where a full hand just means the deal has happened.
static void Msx2_DrawReplacements(u8 owner)
{
	Msx2Side* s = &g_duel.side[owner];
	bool drew = FALSE;
	u8 i;

	if(waifu_deck_remaining(&s->deck) <= 0)
		return;
	for(i = 0; i < MSX2_HAND; ++i)
	{
		if(s->used[i] || (s->hand[i] == MSX2_CARD_NONE))
		{
			if(waifu_deck_remaining(&s->deck) <= 0)
				break;
			s->hand[i] = Msx2_Draw(owner);
			s->used[i] = FALSE;
			drew = TRUE;
		}
	}
	if(!drew && (g_duel.turns > 1) && (waifu_deck_remaining(&s->deck) > 0))
	{
		s->hand[0] = Msx2_Draw(owner);
		s->used[0] = FALSE;
	}
}

// ─────────────────────────────────────────────────────────────────────────────
//  AI glue
// ─────────────────────────────────────────────────────────────────────────────

static void Msx2_FillAiCard(WaifuAiCardView* dst, u8 owner, u8 slot, bool from_hand)
{
	const Msx2Side* s = &g_duel.side[owner];
	if(from_hand)
	{
		u8 card = s->hand[slot];
		dst->card_id = (card == MSX2_CARD_NONE) ? WAIFU_AI_CARD_NONE : (int)card;
		dst->used = s->used[slot];
		dst->faceup = 0;
		dst->defense = 0;
		dst->attacked = 0;
		dst->atk = (int)Msx2_CardAtk(card);
		dst->def = (int)Msx2_CardDef(card);
	}
	else
	{
		u8 card = s->field[slot];
		dst->card_id = (card == MSX2_CARD_NONE) ? WAIFU_AI_CARD_NONE : (int)card;
		dst->used = 0;
		dst->faceup = s->faceup[slot];
		dst->defense = s->defense[slot];
		dst->attacked = s->attacked[slot];
		dst->atk = Msx2_FieldAtk(owner, slot);
		dst->def = Msx2_FieldDef(owner, slot);
	}
}

// com_deck_face_down_attack_threshold(): roughly the upper third of the acting
// side's remaining ATK values, used by the AI to decide whether gambling into a
// face-down monster is worth it.  Insertion sort over at most 40 entries; this
// is the one AI input that costs real Z80 time, so it is computed once per turn
// rather than once per AI query.
static int g_attack_threshold = 1800;

static void Msx2_ComputeAttackThreshold(u8 owner)
{
	const Msx2Side* s = &g_duel.side[owner];
	static i16 vals[WAIFU_DECK_SIZE];
	u8 count = 0;
	u8 i;
	i8 j;

	for(i = 0; (i < (u8)s->deck.count) && (count < WAIFU_DECK_SIZE); ++i)
	{
		int id = s->deck.cards[i];
		if((id >= 0) && Msx2_IsMonster((u8)id))
			vals[count++] = (i16)g_msx2_card_atk[id];
	}
	for(i = 0; (i < MSX2_FIELD) && (count < WAIFU_DECK_SIZE); ++i)
		if(Msx2_IsMonster(s->field[i]))
			vals[count++] = (i16)g_msx2_card_atk[s->field[i]];
	for(i = 0; (i < MSX2_HAND) && (count < WAIFU_DECK_SIZE); ++i)
		if(!s->used[i] && Msx2_IsMonster(s->hand[i]))
			vals[count++] = (i16)g_msx2_card_atk[s->hand[i]];

	if(count == 0)
	{
		g_attack_threshold = 1800;
		return;
	}
	for(i = 1; i < count; ++i)
	{
		i16 v = vals[i];
		j = (i8)(i - 1);
		while((j >= 0) && (vals[j] > v)) { vals[j + 1] = vals[j]; --j; }
		vals[j + 1] = v;
	}
	g_attack_threshold = vals[(count * 2) / 3];
}

// The AI module is written from the COM's point of view.  Handing it a state
// with the two sides swapped lets the same code drive either side, which is
// what makes a blind, input-free duel possible.
static void Msx2_BuildAiState(WaifuAiState* st, u8 owner)
{
	u8 other = owner ? MSX2_OWNER_PLAYER : MSX2_OWNER_COM;
	u8 i;

	st->card_count = MSX2_CARD_COUNT;
	st->you_lp = g_duel.side[other].lp;
	st->com_lp = g_duel.side[owner].lp;
	st->com_can_place_monster = g_duel.side[owner].monster_played ? 0 : 1;
	{
		u8 slot = Msx2_FirstFreeSlot(owner);
		st->free_com_slot = (slot == MSX2_SLOT_NONE) ? -1 : (int)slot;
		slot = Msx2_FirstFreeEquipSlot(owner);
		st->free_com_equip_slot = (slot == MSX2_SLOT_NONE) ? -1 : (int)slot;
		slot = Msx2_FirstLiveSlot(owner);
		st->first_com_equip_target = (slot == MSX2_SLOT_NONE) ? -1 : (int)slot;
	}
	st->deck_attack_threshold = g_attack_threshold;

	for(i = 0; i < MSX2_FIELD; ++i)
	{
		Msx2_FillAiCard(&st->player_field[i], other, i, FALSE);
		Msx2_FillAiCard(&st->com_field[i], owner, i, FALSE);
	}
	for(i = 0; i < MSX2_HAND; ++i)
		Msx2_FillAiCard(&st->com_hand[i], owner, i, TRUE);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Lifecycle and the turn machine
// ─────────────────────────────────────────────────────────────────────────────

static void Msx2_ResetSide(Msx2Side* s)
{
	u8 i;
	for(i = 0; i < MSX2_FIELD; ++i)
	{
		s->field[i] = MSX2_CARD_NONE;
		s->faceup[i] = TRUE;
		s->defense[i] = FALSE;
		s->attacked[i] = FALSE;
		s->atk_bonus[i] = 0;
		s->def_bonus[i] = 0;
		s->equip_field[i] = MSX2_CARD_NONE;
		s->equip_target[i] = -1;
	}
	for(i = 0; i < MSX2_HAND; ++i)
	{
		s->hand[i] = MSX2_CARD_NONE;
		s->used[i] = FALSE;
	}
	s->lp = MSX2_START_LP;
	s->monster_played = FALSE;
}

void Msx2_DuelInit(u32 seed, u8 story_duel_index)
{
	u8 i, owner;

	waifu_deck_rng_seed(&g_duel.rng, seed);
	g_duel.story_duel_index = story_duel_index;
	g_duel.story_active = (story_duel_index != 0xFF) ? TRUE : FALSE;

	for(owner = 0; owner < 2; ++owner)
		Msx2_ResetSide(&g_duel.side[owner]);

	if(g_duel.story_active)
	{
		waifu_deck_build_random(&g_duel.side[MSX2_OWNER_PLAYER].deck, &g_duel.rng, 0);
		waifu_deck_build_opponent_story(&g_duel.side[MSX2_OWNER_COM].deck,
		                                story_duel_index, &g_duel.rng, 1);
	}
	else
	{
		waifu_deck_build_random(&g_duel.side[MSX2_OWNER_PLAYER].deck, &g_duel.rng, 0);
		waifu_deck_build_random(&g_duel.side[MSX2_OWNER_COM].deck, &g_duel.rng, 1);
	}

	for(owner = 0; owner < 2; ++owner)
		for(i = 0; i < MSX2_HAND; ++i)
			g_duel.side[owner].hand[i] = Msx2_Draw(owner);

	g_duel.turns = 1;
	g_duel.main_actions = 0;
	g_duel.turn_owner = MSX2_OWNER_PLAYER;
	g_duel.phase = MSX2_PHASE_TURN_START;
	g_duel.result = 0;
	g_duel.last_trap_fired = FALSE;
	g_duel.last_battle.outcome = MSX2_BATTLE_NONE;
	g_duel.last_attacker_card = MSX2_CARD_NONE;
	g_duel.last_defender_card = MSX2_CARD_NONE;
}

void Msx2_EndTurn(void)
{
	u8 owner = g_duel.turn_owner;
	u8 i;
	for(i = 0; i < MSX2_FIELD; ++i)
		g_duel.side[owner].attacked[i] = FALSE;
	g_duel.side[owner].monster_played = FALSE;
	g_duel.main_actions = 0;
	g_duel.turn_owner = owner ? MSX2_OWNER_PLAYER : MSX2_OWNER_COM;
	++g_duel.turns;
	g_duel.phase = MSX2_PHASE_TURN_START;
}

// One discrete rules action per call.  The caller paces this (one step per
// frame is plenty), so a long AI turn never blocks music or input -- the
// coroutine shape the port plan asks for in its scheduling section.
bool Msx2_DuelStep(void)
{
	WaifuAiState st;
	WaifuAiAction act;
	u8 owner = g_duel.turn_owner;

	if(g_duel.result != 0)
		return FALSE;

	switch(g_duel.phase)
	{
	case MSX2_PHASE_TURN_START:
		// Deck-out: a side that cannot refill its hand loses.
		if(waifu_deck_remaining(&g_duel.side[owner].deck) <= 0)
		{
			bool has_play = FALSE;
			u8 i;
			for(i = 0; i < MSX2_HAND; ++i)
				if(g_duel.side[owner].hand[i] != MSX2_CARD_NONE)
					has_play = TRUE;
			if(!has_play)
			{
				g_duel.result = (owner == MSX2_OWNER_PLAYER) ? -1 : 1;
				g_duel.phase = MSX2_PHASE_RESULT;
				return TRUE;
			}
		}
		Msx2_DrawReplacements(owner);
		Msx2_ComputeAttackThreshold(owner);
		g_duel.main_actions = 0;
		g_duel.phase = MSX2_PHASE_MAIN;
		return TRUE;

	case MSX2_PHASE_MAIN:
		// The COM turn in src/main.c allows at most a placement followed by one
		// support play (IB_COM_SELECT -> IB_COM_PLACE -> IB_COM_EQUIP_SELECT ->
		// IB_COM_BATTLE), so the main phase is capped at two actions here.  The
		// cap is also what guarantees this step machine terminates: an action
		// the rules refuse must not leave the AI free to propose it forever.
		if(g_duel.main_actions >= 2)
		{
			g_duel.phase = MSX2_PHASE_BATTLE;
			return TRUE;
		}
		Msx2_BuildAiState(&st, owner);
		act = waifu_ai_choose_com_select(&st);
		++g_duel.main_actions;
		if(act.kind == WAIFU_AI_ACTION_PLAY_SUPPORT)
		{
			if(!Msx2_PlaySupport(owner, (u8)act.hand_slot, (u8)act.field_slot))
				g_duel.phase = MSX2_PHASE_BATTLE;
			return TRUE;
		}
		if(act.kind == WAIFU_AI_ACTION_PLACE_MONSTER)
		{
			if(!Msx2_PlaceMonster(owner, (u8)act.hand_slot, (u8)act.field_slot,
			                      act.defense_position ? TRUE : FALSE))
				g_duel.phase = MSX2_PHASE_BATTLE;
			return TRUE;
		}
		g_duel.phase = MSX2_PHASE_BATTLE;
		return TRUE;

	case MSX2_PHASE_BATTLE:
		Msx2_BuildAiState(&st, owner);
		act = waifu_ai_choose_com_battle(&st);
		switch(act.kind)
		{
		// Position switches are pure bookkeeping and the AI may ask for several
		// in a row; src/main.c bounds that loop at I_FIELD * 2 and so do we,
		// reusing the main-phase action counter as the guard.
		case WAIFU_AI_ACTION_SET_DEFENSE:
		case WAIFU_AI_ACTION_SET_ATTACK:
			if(g_duel.main_actions >= (2 + MSX2_FIELD * 2))
			{
				g_duel.phase = MSX2_PHASE_TURN_END;
				return TRUE;
			}
			++g_duel.main_actions;
			if(act.field_slot >= 0)
				g_duel.side[owner].defense[act.field_slot] =
					(act.kind == WAIFU_AI_ACTION_SET_DEFENSE) ? TRUE : FALSE;
			return TRUE;
		case WAIFU_AI_ACTION_ATTACK_MONSTER:
			if(!Msx2_Attack(owner, (u8)act.attacker_slot, (u8)act.defender_slot))
				g_duel.phase = MSX2_PHASE_TURN_END;
			return TRUE;
		case WAIFU_AI_ACTION_ATTACK_DIRECT:
			if(!Msx2_Attack(owner, (u8)act.attacker_slot, MSX2_SLOT_NONE))
				g_duel.phase = MSX2_PHASE_TURN_END;
			return TRUE;
		default:
			g_duel.phase = MSX2_PHASE_TURN_END;
			return TRUE;
		}

	case MSX2_PHASE_TURN_END:
		Msx2_EndTurn();
		return TRUE;

	default:
		return FALSE;
	}
}
