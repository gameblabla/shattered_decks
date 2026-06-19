#ifndef WAIFU_FM_AI_H
#define WAIFU_FM_AI_H

#ifdef __cplusplus
extern "C" {
#endif

#define WAIFU_AI_HAND 5
#define WAIFU_AI_FIELD 5
#define WAIFU_AI_CARD_NONE (-1)

typedef enum WaifuAiActionKind {
    WAIFU_AI_ACTION_NONE = 0,
    WAIFU_AI_ACTION_PLAY_SUPPORT,
    WAIFU_AI_ACTION_PLACE_MONSTER,
    WAIFU_AI_ACTION_SET_DEFENSE,
    WAIFU_AI_ACTION_SET_ATTACK,
    WAIFU_AI_ACTION_ATTACK_MONSTER,
    WAIFU_AI_ACTION_ATTACK_DIRECT
} WaifuAiActionKind;

typedef struct WaifuAiCardView {
    int card_id;
    int used;
    int faceup;
    int defense;
    int attacked;
    int atk;
    int def;
} WaifuAiCardView;

typedef struct WaifuAiState {
    int card_count;
    int you_lp;
    int com_lp;
    int com_can_place_monster;
    int free_com_slot;
    int free_com_equip_slot;
    int first_com_equip_target;
    int deck_attack_threshold;
    WaifuAiCardView player_field[WAIFU_AI_FIELD];
    WaifuAiCardView com_field[WAIFU_AI_FIELD];
    WaifuAiCardView com_hand[WAIFU_AI_HAND];
} WaifuAiState;

typedef struct WaifuAiAction {
    WaifuAiActionKind kind;
    int hand_slot;
    int field_slot;
    int attacker_slot;
    int defender_slot;
    int defense_position;
} WaifuAiAction;

WaifuAiAction waifu_ai_choose_com_select(const WaifuAiState *state);
WaifuAiAction waifu_ai_choose_com_battle(const WaifuAiState *state);

#ifdef __cplusplus
}
#endif

#endif /* WAIFU_FM_AI_H */
