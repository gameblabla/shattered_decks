#include "snes_stamp.h"

SnesStamp g_stamp;

void snesStampInit(void)
{
    u16 i;
    u8 *p = (u8 *)&g_stamp;
    for (i = 0; i < sizeof(SnesStamp); ++i) p[i] = 0;
    g_stamp.magic = SNES_STAMP_MAGIC;
    snesStampCommit();
}

void snesStampCommit(void)
{
    u16 sum = 0;
    /* Spell the record out instead of walking it through a u16 pointer.  The
     * 816-tcc backend has to switch accumulator widths for byte pointers, and
     * an earlier compact loop could leave the duel's phase out of the sum
     * when the record grew.  These fields are the on-disk verifier contract;
     * keeping them explicit makes a stamp failure loud rather than letting a
     * stale checksum masquerade as a stalled game. */
    sum += g_stamp.magic;
    sum += g_stamp.scene;
    sum += g_stamp.frames;
    sum += g_stamp.render_lines;
    sum += g_stamp.frame_gen;
    sum += g_stamp.duel_turn;
    sum += g_stamp.lp_player;
    sum += g_stamp.lp_com;
    sum += g_stamp.duel_result;
    sum += g_stamp.ui;
    sum += g_stamp.cursor;
    sum += g_stamp.field_cards;
    sum += g_stamp.phase;
    sum += g_stamp.turn_owner;
    sum += g_stamp.deck_slot;
    sum += g_stamp.deck_count;
    sum += g_stamp.deck_head;
    sum += g_stamp.storage_count;
    sum += g_stamp.save_valid;
    sum += g_stamp.map_lines;
    sum += g_stamp.conv_lines;
    sum += g_stamp.nmi_skips;
    sum += g_stamp.turn_max_lines;
    sum += g_stamp.rest_max_lines;
    sum += g_stamp.patch_max_lines;
    sum += g_stamp.held_max_lines;
    sum += g_stamp.occupied;
    sum += g_stamp.view;
    sum += g_stamp.battle_phase;
    sum += g_stamp.battle_field;
    sum += g_stamp.battle_damage;
    g_stamp.checksum = sum;
}
