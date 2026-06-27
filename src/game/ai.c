#include "ai.h"

static WaifuAiAction ai_none(void)
{
    WaifuAiAction a;
    a.kind = WAIFU_AI_ACTION_NONE;
    a.hand_slot = -1;
    a.field_slot = -1;
    a.attacker_slot = -1;
    a.defender_slot = -1;
    a.defense_position = 0;
    return a;
}

static int is_monster(const WaifuAiState *s, int card_id)
{
    return card_id >= 0 && card_id < s->card_count;
}

static int is_support(const WaifuAiState *s, int card_id)
{
    return card_id >= s->card_count;
}

static int support_kind(const WaifuAiState *s, int card_id)
{
    if (!is_support(s, card_id)) return -1;
    return card_id - s->card_count;
}

static int is_thunder_support(const WaifuAiState *s, int card_id)
{
    return support_kind(s, card_id) == 4;
}

static int is_equip_support(const WaifuAiState *s, int card_id)
{
    int kind = support_kind(s, card_id);
    return kind == 0 || kind == 1;
}

static int player_has_monsters(const WaifuAiState *s)
{
    int i;
    for (i = 0; i < WAIFU_AI_FIELD; ++i) {
        if (is_monster(s, s->player_field[i].card_id)) return 1;
    }
    return 0;
}

static int player_field_warrants_thunder(const WaifuAiState *s)
{
    int i;
    int live = 0;
    for (i = 0; i < WAIFU_AI_FIELD; ++i) {
        const WaifuAiCardView *c = &s->player_field[i];
        if (!is_monster(s, c->card_id)) continue;
        ++live;
        if (c->atk > 2000) return 1;
    }
    return live >= 2;
}

static int strongest_player_attack_atk(const WaifuAiState *s)
{
    int i;
    int best = 0;
    for (i = 0; i < WAIFU_AI_FIELD; ++i) {
        const WaifuAiCardView *c = &s->player_field[i];
        if (is_monster(s, c->card_id) && c->faceup && !c->defense && c->atk > best) best = c->atk;
    }
    return best;
}

static int direct_attack_lethal_retaliation_risk(const WaifuAiState *s, int com_slot)
{
    int strongest;
    const WaifuAiCardView *c;
    if (com_slot < 0 || com_slot >= WAIFU_AI_FIELD) return 1;
    c = &s->com_field[com_slot];
    if (!is_monster(s, c->card_id)) return 1;
    strongest = strongest_player_attack_atk(s);
    return strongest > c->atk && (strongest - c->atk) >= s->com_lp;
}

static int attack_mode_lp_risk(const WaifuAiState *s, int com_slot)
{
    int strongest;
    const WaifuAiCardView *c;
    if (com_slot < 0 || com_slot >= WAIFU_AI_FIELD) return 0;
    c = &s->com_field[com_slot];
    if (!is_monster(s, c->card_id)) return 0;
    strongest = strongest_player_attack_atk(s);
    return strongest > c->atk;
}

static int defender_passive(const WaifuAiCardView *defender)
{
    return defender->defense;
}

static int can_attack_known_target_without_loss(const WaifuAiCardView *attacker, const WaifuAiCardView *defender)
{
    int value = defender_passive(defender) ? defender->def : defender->atk;
    return attacker->atk >= value;
}

static int can_attack_face_down_attack_target(const WaifuAiState *s, const WaifuAiCardView *attacker, const WaifuAiCardView *defender)
{
    (void)defender;
    return attacker->atk >= s->deck_attack_threshold;
}

static int target_score(const WaifuAiCardView *attacker, const WaifuAiCardView *defender)
{
    int value = defender_passive(defender) ? defender->def : defender->atk;
    int score = value;
    int delta = attacker->atk - value;
    if (delta > 0 && !defender_passive(defender)) score += delta;
    if (!defender->faceup && !defender->defense) score += 400;
    if (defender->defense) score -= 100;
    return score;
}

static int choose_attack_target(const WaifuAiState *s, int attacker_slot)
{
    const WaifuAiCardView *attacker;
    int i;
    int best_slot = -1;
    int best_score = -999999;
    if (attacker_slot < 0 || attacker_slot >= WAIFU_AI_FIELD) return -1;
    attacker = &s->com_field[attacker_slot];
    if (!is_monster(s, attacker->card_id) || attacker->defense || attacker->attacked) return -1;

    for (i = 0; i < WAIFU_AI_FIELD; ++i) {
        const WaifuAiCardView *defender = &s->player_field[i];
        int legal = 0;
        int score;
        if (!is_monster(s, defender->card_id)) continue;
        if (!defender->faceup && !defender->defense) {
            legal = can_attack_face_down_attack_target(s, attacker, defender);
        } else {
            legal = can_attack_known_target_without_loss(attacker, defender);
        }
        if (!legal) continue;
        score = target_score(attacker, defender);
        if (score > best_score) {
            best_score = score;
            best_slot = i;
        }
    }
    return best_slot;
}

static int choose_hand_monster(const WaifuAiState *s)
{
    int i;
    int best_slot = -1;
    int best_atk = -1;
    for (i = 0; i < WAIFU_AI_HAND; ++i) {
        const WaifuAiCardView *c = &s->com_hand[i];
        if (c->used || !is_monster(s, c->card_id)) continue;
        if (c->atk > best_atk) {
            best_atk = c->atk;
            best_slot = i;
        }
    }
    return best_slot;
}

static int should_place_in_defense(const WaifuAiState *s, const WaifuAiCardView *card)
{
    int i;
    int has_face_down_attack = 0;
    int can_pressure = 0;
    if (!card) return 0;

    for (i = 0; i < WAIFU_AI_FIELD; ++i) {
        const WaifuAiCardView *p = &s->player_field[i];
        if (!is_monster(s, p->card_id)) continue;
        if (!p->faceup && !p->defense) has_face_down_attack = 1;
        if (p->defense) {
            if (card->atk >= p->def) can_pressure = 1;
        } else if (p->faceup) {
            if (card->atk >= p->atk) can_pressure = 1;
        }
    }

    if (has_face_down_attack && card->atk < s->deck_attack_threshold) return 1;
    if (!can_pressure && attack_mode_lp_risk(s, -1)) return 1;
    if (strongest_player_attack_atk(s) > card->atk) return 1;
    return 0;
}

WaifuAiAction waifu_ai_choose_com_select(const WaifuAiState *s)
{
    WaifuAiAction a = ai_none();
    int i;
    int h;
    if (!s) return a;

    if (player_field_warrants_thunder(s)) {
        for (i = 0; i < WAIFU_AI_HAND; ++i) {
            if (!s->com_hand[i].used && is_thunder_support(s, s->com_hand[i].card_id)) {
                a.kind = WAIFU_AI_ACTION_PLAY_SUPPORT;
                a.hand_slot = i;
                a.field_slot = -1;
                return a;
            }
        }
    }

    if (s->free_com_equip_slot >= 0 && s->first_com_equip_target >= 0) {
        for (i = 0; i < WAIFU_AI_HAND; ++i) {
            if (!s->com_hand[i].used && is_equip_support(s, s->com_hand[i].card_id)) {
                a.kind = WAIFU_AI_ACTION_PLAY_SUPPORT;
                a.hand_slot = i;
                a.field_slot = s->first_com_equip_target;
                return a;
            }
        }
    }

    if (!s->com_can_place_monster || s->free_com_slot < 0) return a;
    h = choose_hand_monster(s);
    if (h < 0) return a;

    a.kind = WAIFU_AI_ACTION_PLACE_MONSTER;
    a.hand_slot = h;
    a.field_slot = s->free_com_slot;
    a.defense_position = should_place_in_defense(s, &s->com_hand[h]);
    return a;
}

WaifuAiAction waifu_ai_choose_com_battle(const WaifuAiState *s)
{
    WaifuAiAction a = ai_none();
    int i;
    if (!s) return a;

    if (!player_has_monsters(s)) {
        for (i = 0; i < WAIFU_AI_FIELD; ++i) {
            const WaifuAiCardView *c = &s->com_field[i];
            if (!is_monster(s, c->card_id) || c->attacked) continue;
            if (!c->defense) {
                a.kind = WAIFU_AI_ACTION_ATTACK_DIRECT;
                a.attacker_slot = i;
                return a;
            }
            if (c->atk > 0 && !direct_attack_lethal_retaliation_risk(s, i)) {
                a.kind = WAIFU_AI_ACTION_SET_ATTACK;
                a.field_slot = i;
                return a;
            }
        }
        return a;
    }

    for (i = 0; i < WAIFU_AI_FIELD; ++i) {
        const WaifuAiCardView *c = &s->com_field[i];
        int target;
        if (!is_monster(s, c->card_id) || c->attacked || c->defense) continue;
        target = choose_attack_target(s, i);
        if (target >= 0) {
            a.kind = WAIFU_AI_ACTION_ATTACK_MONSTER;
            a.attacker_slot = i;
            a.defender_slot = target;
            return a;
        }
        if (attack_mode_lp_risk(s, i) || s->deck_attack_threshold > c->atk) {
            a.kind = WAIFU_AI_ACTION_SET_DEFENSE;
            a.field_slot = i;
            return a;
        }
    }

    return a;
}
