#include "machine.h"
#include "io.h"

/* TOWNSIO_MACHINE_ID_LOW in TOWNSEMU's townsdef.h. */
#define FMT_IO_MACHINE_ID_LOW   0x0030u

/* Memory wait-state control.  The firmware leaves this model-dependent:
 * standard TOWNS BIOSes can retain six main-RAM/VRAM wait states (the
 * "SLOW"/FM-R compatibility setting, which exists to hold old software to a
 * compatible speed, not because the DRAM needs it) while Marty's boot path
 * usually arrives already fast.  This bare-metal game owns its execution
 * environment, so select the fast setting once at startup rather than
 * inheriting a firmware choice that makes identical game code several times
 * slower.
 *
 * 5ECH is the FAST/SLOW switch, and writing 1 to it clears both wait
 * counters -- but the Technical Databook puts it on 3rd-generation (CX and
 * later) machines only, so on a 2nd-generation 386SX like the Marty it is
 * very likely a no-op.  The wait registers themselves are what those
 * machines have: 5E0H on 1st generation, 5E2H from the 2nd, and 5E6H for
 * VRAM.  All four are written, cheapest-first, because there is no way to
 * ask the machine which of them it implements and a write to a port that
 * does not exist costs nothing.  0 is the fast setting in every one of them
 * (TOWNSEMU's FMTownsCommon::FASTModeLamp() reads exactly that). */
#define FMT_IO_FAST_MODE         0x05ecu
#define FMT_FAST_MODE_ENABLE     0x01u
#define FMT_IO_MAINRAM_WAIT_1STGEN 0x05e0u
#define FMT_IO_MAINRAM_WAIT      0x05e2u
#define FMT_IO_VRAM_WAIT         0x05e6u

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

    /* These apply equally to the narrow Marty/UX and wide standard maps;
     * they change bus wait policy, never the VRAM address chosen above. */
    outb(FMT_FAST_MODE_ENABLE, FMT_IO_FAST_MODE);
    outb(0, FMT_IO_MAINRAM_WAIT_1STGEN);
    outb(0, FMT_IO_MAINRAM_WAIT);
    outb(0, FMT_IO_VRAM_WAIT);
}

int fmt_machine_is_narrow_map(void)
{
    return g_fmt_narrow_map;
}
