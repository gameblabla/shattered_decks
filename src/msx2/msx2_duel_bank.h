// ─────────────────────────────────────────────────────────────────────────────
//  msx2_duel_bank.h — ASCII16-X fixed-page rules entry points
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "msx2_duel.h"

#ifdef MSX2_ASCII16X
u16  Msx2_CardAtk_In(u8 card);
u16  Msx2_CardDef_In(u8 card);
u8   Msx2_FusionResult_In(u8 a, u8 b);
u8   Msx2_FusionChainResult_In(const u8* cards, u8 count);
i16  Msx2_FieldAtk_In(u8 owner, u8 slot);
i16  Msx2_FieldDef_In(u8 owner, u8 slot);
bool Msx2_Attack_In(u8 owner, u8 attacker_slot, u8 defender_slot);
bool Msx2_PlaceMonster_In(u8 owner, u8 hand_slot, u8 field_slot, bool defense);
u8   Msx2_FusionPreview_In(u8 owner, const u8* hand_slots, u8 count, u8 field_slot);
bool Msx2_PlaceFusion_In(u8 owner, const u8* hand_slots, u8 count, u8 field_slot,
                         bool defense);
bool Msx2_FusionSucceeded_In(void);
bool Msx2_PlaySupport_In(u8 owner, u8 hand_slot, u8 target_slot);
bool Msx2_ChangePosition_In(u8 owner, u8 field_slot);
void Msx2_DuelInit_In(u32 seed, u8 story_duel_index);
void Msx2_DuelSetPlayerDeck_In(const u8* cards, u8 count);
void Msx2_EndTurn_In(void);
bool Msx2_DuelStep_In(void);

void waifu_deck_rng_seed_In(WaifuDeckRng* rng, u32 seed);
int  waifu_deck_remaining_In(const WaifuDeck* deck);
void waifu_deck_build_random_In(WaifuDeck* deck, WaifuDeckRng* rng,
                                int strength_bias);
#endif
