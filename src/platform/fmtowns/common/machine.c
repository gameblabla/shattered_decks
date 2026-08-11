#include "machine.h"
#include "io.h"

/* TOWNSIO_MACHINE_ID_LOW in TOWNSEMU's townsdef.h. */
#define FMT_IO_MACHINE_ID_LOW   0x0030u

/* FMTownsCommon::MachineID()'s CPU-class field, low byte, genuine FM TOWNS
 * (non-FMR) models: the only value the UX and the Marty produce. */
#define FMT_MACHINE_ID_CPU_80386SX  3u

#define FMT_VRAM0_BASE_NARROW  0xA00000u   /* 386SX map: Marty, UX */
#define FMT_VRAM0_BASE_WIDE    0x80000000u /* everyone else */

/* Default to the narrow (Marty) map until detected -- this port's original,
 * verified-only-on-Marty behaviour, preserved as the safe fallback. */
uint32_t g_fmt_vram0_base = FMT_VRAM0_BASE_NARROW;

static int g_fmt_narrow_map = 1;

void fmt_machine_detect(void)
{
    uint8_t cpu_class = inb(FMT_IO_MACHINE_ID_LOW);

    if (cpu_class == FMT_MACHINE_ID_CPU_80386SX) {
        g_fmt_narrow_map = 1;
        g_fmt_vram0_base = FMT_VRAM0_BASE_NARROW;
    } else {
        g_fmt_narrow_map = 0;
        g_fmt_vram0_base = FMT_VRAM0_BASE_WIDE;
    }
}

int fmt_machine_is_narrow_map(void)
{
    return g_fmt_narrow_map;
}
