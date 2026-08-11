#ifndef FMT_MACHINE_H
#define FMT_MACHINE_H

#include <stdint.h>

/*
 * Runtime FM TOWNS model detection.
 *
 * This payload was originally written and verified only against the FM
 * TOWNS Marty. Marty (and the UX) are 80386SX-class machines with only a
 * 24-bit physical address space, so Fujitsu gave them a completely
 * different physical memory map from every other (386DX/486/Pentium-class)
 * model -- VRAM, ROM and CMOS windows all sit at different physical
 * addresses. See TOWNSEMU's towns/memory/physmem.cpp,
 * TownsPhysicalMemory::SetUpMemoryAccess(): it branches VRAM/ROM/CMOS
 * addresses on `cpuType==TOWNSCPU_80386SX`, one full set of
 * TOWNSADDR_386SX_* constants for Marty/UX and a completely different
 * TOWNSADDR_* set for everyone else (VRAM0 at 0xA00000 vs 0x80000000, for
 * example -- see TOWNSADDR_386SX_VRAM0_BASE / TOWNSADDR_VRAM0_BASE in
 * towns/townsdef/townsdef.h).
 *
 * Because this is a bare-metal payload -- no TOWNS OS, no BIOS call graph
 * to lean on -- it has to make that branch itself, at runtime, before
 * anything touches VRAM. Real FM TOWNS software does this off a genuine
 * hardware register: the "machine ID" I/O ports 0x30 (low byte) / 0x31
 * (high byte) (TOWNSIO_MACHINE_ID_LOW / _HIGH in TOWNSEMU's townsdef.h).
 * TOWNSEMU's FMTownsCommon::MachineID() (towns/towns.cpp) shows the low
 * byte is a small CPU-class field for genuine FM TOWNS models (as opposed
 * to the legacy FMR line, which ORs it into a different high-nibble
 * pattern): 0 = 80286-class (never a real FM TOWNS), 1 = 80386(DX)-class,
 * 2 = 80486/Pentium-class, 3 = 80386SX-class -- and TOWNSTYPE_2_UX and
 * TOWNSTYPE_MARTY are the *only* two model cases in that switch that
 * produce 3. That is exactly the narrow/wide memory-map split this port
 * needs, read straight off the low byte with no model-name table to keep
 * in sync.
 *
 * (Real TBIOS.SYS is documented, per TOWNSEMU's author from disassembly,
 * to test the *high* byte's 0x40 bit to detect Marty specifically --
 * that identifies Marty alone, not the shared 386SX narrow-map class UX
 * also belongs to, so it is the wrong test for this port's purposes even
 * though it is the "real" BIOS-internal Marty check.)
 */

/* Probes the machine ID I/O ports once and records the result. Must run
 * before fmt_set_mode()/fmt_put_image()/anything else that touches VRAM.
 * Safe to call more than once (idempotent). */
void fmt_machine_detect(void);

/* Non-zero on Marty/UX-class 80386SX machines (24-bit narrow physical
 * memory map); zero on every 386DX/486/Pentium-class model (wide map).
 * Valid only after fmt_machine_detect() has run at least once -- until
 * then it reports the narrow-map (Marty) default this port shipped with
 * originally, which keeps every existing Marty-only code path correct by
 * construction if some future call site forgets to detect first. */
int fmt_machine_is_narrow_map(void);

/* VRAM0's physical base address for the machine fmt_machine_detect() found:
 * 0xA00000 on the narrow (386SX) map, 0x80000000 on the wide map. This is
 * what the old FMT_VRAM0_BASE compile-time constant used to be; every VRAM
 * pointer in this library now reads it here instead so the same binary
 * works on both memory maps. */
extern uint32_t g_fmt_vram0_base;

#endif
