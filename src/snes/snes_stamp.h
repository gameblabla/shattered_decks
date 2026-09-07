/* ─────────────────────────────────────────────────────────────────────────────
 *  snes_stamp.h — the frame-stamp channel the headless harness reads.
 *
 *  snesfaust's `script` mode dumps WRAM at the end of a run, so a fixed record
 *  in a known place is how a scripted run reports what the game actually did:
 *  which scene it reached, how many frames it drew, and what the last render
 *  cost.  tools/snes/verify.py reads it out of wram.bin.
 *
 *  This is the SNES form of the FM TOWNS frame stamp, and it exists for the
 *  same reason: a screenshot that is black proves nothing, and a perf number
 *  read off the source proves less.
 * ───────────────────────────────────────────────────────────────────────────── */
#ifndef WAIFU_SNES_STAMP_H
#define WAIFU_SNES_STAMP_H

#include "snes_types.h"

#define SNES_STAMP_MAGIC  0x5744u          /* 'WD' */

typedef struct SnesStamp {
    u16 magic;
    u16 scene;          /* enum SnesScene */
    u16 frames;         /* frames the scene machine has run */
    u16 render_lines;   /* scanlines the last board render took (see snes_perf.c) */
    u16 board_res;      /* enum SnesBoardRes in force */
    u16 duel_turn;      /* rules turn counter */
    u16 lp_player;
    u16 lp_com;
    u16 duel_result;    /* 0 running, 1 player won, 0xFFFF player lost */
    u16 ui;             /* enum SnesDuelUi: which question the screen is asking */
    u16 cursor;         /* the hand or field slot the cursor is on */
    u16 field_cards;    /* monsters standing on the player's own row */
    u16 phase;          /* enum Msx2Phase */
    u16 turn_owner;     /* 0 the player, 1 the COM */
    u16 checksum;       /* sum of the words above, so a torn dump is detectable */
} SnesStamp;

extern SnesStamp g_stamp;

void snesStampInit(void);
void snesStampCommit(void);

#endif /* WAIFU_SNES_STAMP_H */
