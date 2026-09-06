/* ─────────────────────────────────────────────────────────────────────────────
 *  atarist_probe.h — the blind-run instrument.
 *
 *  The headless Hatari MCP build can read memory, single-step and take
 *  screenshots, but it has no way to press a key.  So the port carries the
 *  same instrument the other blind targets do: one magic-tagged struct that
 *  says what the game is doing, and a scripted-input queue the harness pokes
 *  through the MCP `reasm` tool to stand in for the player.
 *
 *  The struct is found by scanning a RAM dump for ATARIST_PROBE_MAGIC, because
 *  a TOS program's load address depends on how much of the TPA the AUTO folder
 *  has already used and is not worth predicting.  A trailing magic makes a
 *  false positive on a random 32-bit pattern effectively impossible.
 * ───────────────────────────────────────────────────────────────────────────── */
#ifndef WAIFU_ATARIST_PROBE_H
#define WAIFU_ATARIST_PROBE_H

#include <stdint.h>

#define ATARIST_PROBE_MAGIC     0x57465354u   /* 'WFST' */
#define ATARIST_PROBE_MAGIC_END 0x454e4421u   /* 'END!' */
#define ATARIST_PROBE_VERSION   1
#define ATARIST_PROBE_SCRIPT    128

/* Coarse "how far did boot get" markers, so a machine that dies before it can
 * draw anything still says where. */
enum AtaristStage {
    ATARIST_STAGE_ENTRY = 0,
    ATARIST_STAGE_VIDEO,
    ATARIST_STAGE_INPUT,
    ATARIST_STAGE_AUDIO,
    ATARIST_STAGE_ASSETS,
    ATARIST_STAGE_LOOP,
    ATARIST_STAGE_TITLE,
    ATARIST_STAGE_DUEL,
    ATARIST_STAGE_STORY,
    ATARIST_STAGE_EXIT
};

enum AtaristProbeStatus {
    ATARIST_PROBE_OK = 0,
    ATARIST_PROBE_BADSTATE,
    ATARIST_PROBE_STUCK,
    ATARIST_PROBE_NOMEM,
    ATARIST_PROBE_NOFILE
};

typedef struct AtaristProbe {
    uint32_t magic;
    uint16_t version;
    uint16_t stage;

    uint32_t frame;          /* game steps taken */
    uint32_t vbl;            /* hardware vblanks since boot */
    uint16_t frame_vbls;     /* vblanks the previous frame occupied */
    uint16_t status;         /* enum AtaristProbeStatus */

    uint16_t scene;
    uint16_t menu_cursor;
    uint16_t duels;
    uint16_t wins_player;
    uint16_t wins_com;
    int16_t  lp_player;
    int16_t  lp_com;
    uint16_t turns;
    uint16_t phase;

    uint16_t is_ste;
    uint16_t has_blitter;
    uint16_t machine_ram_kb;

    /* Scripted input.  Each entry is (frames << 16) | button mask; the queue is
     * consumed one entry at a time, holding the mask for `frames` game frames.
     * script_len == 0 means "a person is playing". */
    uint16_t script_len;
    uint16_t script_pos;
    uint16_t script_hold;
    /* Free-form progress marker.  Boot is a long stretch with nothing on
     * screen and no frame counter yet, so each step stamps this and a blind
     * run can say which one died. */
    uint16_t mark;
    /* Worst frame since boot, in vblanks.  A duel screen that is quiet most of
     * the time hides its cost from a two-sample average, and the frames that
     * matter are exactly the ones that move; this is the number to read when
     * judging whether the renderer fits its budget.  `spare` keeps the script
     * array on a four-byte boundary. */
    uint16_t worst_vbls;
    uint16_t spare;
    uint32_t script[ATARIST_PROBE_SCRIPT];

    uint32_t magic_end;
} AtaristProbe;

extern AtaristProbe g_atarist_probe;

void Atarist_ProbeInit(void);
void Atarist_ProbeUpdate(void);
/* The button mask the script wants held this frame (0 when there is none).
   Consumes one frame of the current entry. */
uint16_t Atarist_ProbeScriptedButtons(void);

#define ATARIST_STAGE(s) (g_atarist_probe.stage = (uint16_t)(s))
#define ATARIST_MARK(m)  (g_atarist_probe.mark = (uint16_t)(m))

#endif /* WAIFU_ATARIST_PROBE_H */
