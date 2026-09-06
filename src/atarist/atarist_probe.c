#include "atarist_probe.h"
#include "atarist_video.h"
#include "atarist_hw.h"

/* Not static, and not const: the harness locates it by scanning RAM for the
 * magic and writes the script queue straight into it. */
AtaristProbe g_atarist_probe;

void Atarist_ProbeInit(void)
{
    int i;
    g_atarist_probe.magic = ATARIST_PROBE_MAGIC;
    g_atarist_probe.version = ATARIST_PROBE_VERSION;
    g_atarist_probe.stage = ATARIST_STAGE_ENTRY;
    g_atarist_probe.status = ATARIST_PROBE_OK;
    g_atarist_probe.frame = 0;
    g_atarist_probe.frame_vbls = 1;
    g_atarist_probe.scene = 0;
    g_atarist_probe.menu_cursor = 0xffffu;
    g_atarist_probe.duels = 0;
    g_atarist_probe.wins_player = 0;
    g_atarist_probe.wins_com = 0;
    g_atarist_probe.lp_player = 0;
    g_atarist_probe.lp_com = 0;
    g_atarist_probe.turns = 0;
    g_atarist_probe.phase = 0;
    g_atarist_probe.script_len = 0;
    g_atarist_probe.script_pos = 0;
    g_atarist_probe.script_hold = 0;
    g_atarist_probe.mark = 0;
    for (i = 0; i < ATARIST_PROBE_SCRIPT; ++i) g_atarist_probe.script[i] = 0;
    g_atarist_probe.magic_end = ATARIST_PROBE_MAGIC_END;
}

void Atarist_ProbeUpdate(void)
{
    g_atarist_probe.vbl = g_atarist_vbl;
    g_atarist_probe.is_ste = g_atarist_is_ste;
    g_atarist_probe.has_blitter = g_atarist_has_blitter;
    g_atarist_probe.machine_ram_kb = (uint16_t)(ST_PHYSTOP >> 10);
    ++g_atarist_probe.frame;
}

uint16_t Atarist_ProbeScriptedButtons(void)
{
    uint32_t entry;

    if (!g_atarist_probe.script_len) return 0;
    if (g_atarist_probe.script_pos >= g_atarist_probe.script_len) return 0;

    entry = g_atarist_probe.script[g_atarist_probe.script_pos];
    if (!g_atarist_probe.script_hold) {
        uint16_t frames = (uint16_t)(entry >> 16);
        g_atarist_probe.script_hold = frames ? frames : 1;
    }
    if (--g_atarist_probe.script_hold == 0) ++g_atarist_probe.script_pos;
    return (uint16_t)(entry & 0xffffu);
}
