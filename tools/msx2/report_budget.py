#!/usr/bin/env python3
"""Report the MSX2 port's code and RAM footprint against its hard budgets.

The port lives inside several ceilings that are easy to blow through silently:

  * Resident code.  SDCC links _CODE contiguously from 0x4000.  Page 1
    (0x4000-0x7FFF) is NEO segment 0 and is always mapped; page 2
    (0x8000-0xBFFF) is the *switched* window that the asset streamer needs, and
    MSXgl's crt0 only happens to map segment 1 there at boot.  So any code that
    spills past 0x8000 is living in the streaming window on borrowed time: the
    first bank switch under it is a crash.  16 KB is the honest resident-code
    budget on paper; in practice _CODE uses the whole 32 KB and the streamer is
    kept below the line, which pack_msx_rom.py checks on every build.

  * The page-0 code banks.  Page 0 (0x0000-0x3FFF) is a switched window over
    cartridge segments 2 (the duel screen), 3 (the modal screens) and 4 (the
    story) -- see src/msx2/msx2_bank.h.  Each is a hard 16 KB of its own and
    they are where the two biggest screens in the port actually live, so these
    are usually the budget that binds first.

  * Working RAM.  _DATA starts at 0xC000 and the stack starts at HIMEM
    (0xF380).  Everything statically allocated plus the deepest stack the game
    ever reaches has to fit in between.

Usage:  tools/msx2/report_budget.py src/msx2/out/waifu_msx2.map
"""

import re
import sys

PAGE1_END = 0x8000
PAGE2_END = 0xC000
RAM_BASE = 0xC000
HIMEM = 0xF380


def areas(text):
    """Every '<NAME>  <addr>  <size> =  N. bytes' line in the map."""
    out = {}
    for m in re.finditer(r"^(\S+)\s+([0-9A-F]{8})\s+([0-9A-F]{8})\s+=\s+(\d+)\. bytes",
                         text, re.M):
        name, addr, _, size = m.group(1), int(m.group(2), 16), m.group(3), int(m.group(4))
        prev = out.get(name)
        if prev is None or size > prev[1]:
            out[name] = (addr, size)
    return out


def bar(used, total, width=40):
    filled = min(width, int(width * used / total)) if total else 0
    return "[" + "#" * filled + "." * (width - filled) + "]"


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    text = open(sys.argv[1], encoding="utf-8", errors="replace").read()
    a = areas(text)

    code_addr, code_size = a.get("_CODE", (0x4000, 0))
    code_end = code_addr + code_size
    home = a.get("_HOME", (0, 0))[1]
    data_addr, data_size = a.get("_DATA", (RAM_BASE, 0))
    bss = a.get("_BSS", (0, 0))[1]
    initialized = a.get("_INITIALIZED", (0, 0))[1]

    ram_end = max(data_addr + data_size, RAM_BASE) + bss + initialized
    ram_used = ram_end - RAM_BASE
    ram_free = HIMEM - ram_end

    print("MSX2 footprint  (from %s)" % sys.argv[1])
    print()
    print("  code   0x%04X-0x%04X  %6d bytes  %s" % (code_addr, code_end, code_size,
                                                     bar(code_size, PAGE1_END - 0x4000)))
    if code_end > PAGE1_END:
        over = code_end - PAGE1_END
        print("         NOTE: %d bytes sit past 0x8000, inside the NEO streaming"
              % over)
        print("         window (segment 1 at boot).  None of it exists while a scene")
        print("         is streaming, which is why the streamer is linked below")
        print("         0x8000 and runs with interrupts off; pack_msx_rom.py fails")
        print("         the build if that stops being true.  Anything that must run")
        print("         *during* a stream has to live below 0x8000 too.")
    if code_end > PAGE2_END:
        print("         FATAL: code runs past 0xBFFF, into RAM.")
    print("  home                    %6d bytes" % home)
    print()
    # The page-0 banks.  All three are mapped at 0x0000 and each is its own
    # 16 KB; msx2_bank.h says which screen is in which.
    for seg, what in ((2, "duel screen"), (3, "modal screens"), (4, "story")):
        name = "_SEG%d" % seg
        if name not in a:
            continue
        size = a[name][1]
        print("  seg %d  %-14s %6d bytes  %s" % (seg, what, size, bar(size, 0x4000)))
        if size > 0x4000:
            print("         FATAL: bank %d does not fit in its 16 KB window." % seg)
    print()
    print("  RAM    0x%04X-0x%04X  %6d bytes used, %d free to HIMEM (0x%04X)"
          % (RAM_BASE, ram_end, ram_used, ram_free, HIMEM))
    print("         %s" % bar(ram_used, HIMEM - RAM_BASE))
    print()
    print("  Stack and every C local live in that free space; the running figure"
          " the")
    print("  ROM itself measures is in the blind-play probe (./msx2.sh run).")

    banks_over = any(a.get("_SEG%d" % seg, (0, 0))[1] > 0x4000 for seg in (2, 3, 4))
    if code_end > PAGE2_END or ram_free <= 0 or banks_over:
        sys.exit(1)


if __name__ == "__main__":
    main()
