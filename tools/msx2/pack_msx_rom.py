#!/usr/bin/env python3
"""Pack generated MSX2 assets into a checked NEO-16 or ASCII16-X image.

MSXgl links the executable code into mapper segments, while the large scene,
card, music and floor files are placed here after linking.  The two mapper
formats use the same 16 KiB physical layout for those files; only the way the
CPU selects the page-2 window and the resident-code ceiling differs.

Usage:
    tools/msx2/pack_msx_rom.py src/msx2/out/waifu_msx2.rom
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ASSET_DIR = os.path.join(ROOT, "src", "msx2", "assets")
MANIFEST = os.path.join(ASSET_DIR, "manifest.txt")
PLUS_MANIFEST = os.path.join(ASSET_DIR, "plus", "manifest_plus.txt")
SEGMENT_BYTES = 16 * 1024
WINDOW = 0x8000

# These are CPU-address ceilings of linked areas.  NEO-16 has a fixed page-0
# half at 0x0000-0x3FFF and a fixed page-2 half at 0x8000-0xBFFF.  ASCII16-X
# mirrors its page-2 window into page 0, so the resident image must fit below
# 0x8000.
NEO_LIMITS = {
    "_CODE": 0xC000, "_HOME": 0xC000, "_RODATA": 0xC000,
    "_INITIALIZER": 0xC000, "_GSINIT": 0xC000, "_GSFINAL": 0xC000,
    "_SEG2": 0x4000, "_SEG3": 0x8000, "_SEG4": 0x8000, "_SEG5": 0xC000,
}
ASCII16X_LIMITS = {
    "_CODE": 0x8000, "_HOME": 0x8000, "_RODATA": 0x8000,
    "_INITIALIZER": 0x8000, "_GSINIT": 0x8000, "_GSFINAL": 0x8000,
    "_SEG3": 0xC000, "_SEG4": 0xC000, "_SEG5": 0xC000,
    "_SEG6": 0xC000, "_SEG7": 0xC000,
}

RESIDENT_SYMBOLS = [
    "_Msx2_StreamChunk", "_Msx2_StreamSetVramChunk", "_Msx2_StreamScene",
    "_Msx2_BlitRow", "_Msx2_StreamRect", "_Msx2_RomRead",
    "_Msx2_AudioTick", "_Msx2_LvgmNotify", "_Msx2_Bank2Enter",
    "_Msx2_Bank2Leave", "_LVGM_Play", "_LVGM_Stop", "_LVGM_Decode",
    "_LVGM_DecodePSG", "_PSG_Apply", "_PSG_Mute",
    "_Msx2_ClearActionEvent", "_Msx2_IsMonster", "_Msx2_IsSupport",
    "_Msx2_SupportKind", "_Msx2_LiveMonsterCount", "_Msx2_FirstLiveSlot",
    "_Msx2_FirstFreeSlot", "_Msx2_FirstFreeEquipSlot",
    "_Msx2_FirstTurnAttackLocked",
]

BOOT_BANK_ASM = {
    "neo16": "waifu_msx2_s5_b2.asm",
    "ascii16x": "waifu_msx2_ascii_s6_b1.asm",
}
ISR_WINDOW_ASM = "msx2_lvgm.asm"
CRT0_MODULES = ("crt0",)


def mapper_name():
    mapper = os.environ.get("MSX2_MAPPER", "neo16").lower()
    if mapper not in ("neo16", "ascii16x"):
        sys.exit("MSX2_MAPPER must be neo16 or ascii16x")
    return mapper


def read_manifest(path):
    entries = []
    with open(path) as handle:
        for raw in handle:
            line = raw.split("#", 1)[0].strip()
            if line:
                name, segment, binary = line.split()
                entries.append((name, int(segment), binary))
    return entries


def assets():
    """Return (name, first segment, binary) triples in segment order."""
    out = read_manifest(MANIFEST) + read_manifest(
        os.path.join(ASSET_DIR, "floor_manifest.txt"))
    if os.environ.get("MSX2_PLUS"):
        replacements = {name: (name, segment, binary)
                        for name, segment, binary in read_manifest(PLUS_MANIFEST)}
        by_name = {name: index for index, (name, _s, _b) in enumerate(out)}
        for name, entry in replacements.items():
            if name in by_name:
                out[by_name[name]] = entry
            else:
                out.append(entry)
    return sorted(out, key=lambda entry: entry[1])


def map_areas(mapfile):
    areas = {}
    pattern = re.compile(r"^(\S+)\s+([0-9A-F]{8})\s+([0-9A-F]{8})\s+=")
    with open(mapfile) as handle:
        for line in handle:
            match = pattern.match(line)
            if match:
                areas[match.group(1)] = (int(match.group(2), 16),
                                         int(match.group(3), 16))
    return areas


def map_symbols(mapfile):
    symbols = {}
    with open(mapfile) as handle:
        for line in handle:
            for address, name in re.findall(r"([0-9A-F]{8})\s+(_[A-Za-z0-9_]+)",
                                            line):
                symbols.setdefault(name, int(address, 16) & 0xFFFF)
    return symbols


def check_resident(mapfile):
    """Return resident symbols that are not below the switched window."""
    symbols = map_symbols(mapfile)
    return [(name, symbols[name]) for name in RESIDENT_SYMBOLS
            if name in symbols and symbols[name] >= WINDOW]


def check_code_banks(mapfile, mapper):
    """Return linked areas that cross the CPU bank they are mapped into."""
    limits = ASCII16X_LIMITS if mapper == "ascii16x" else NEO_LIMITS
    areas = map_areas(mapfile)
    bad = []
    for name, limit in limits.items():
        if name not in areas:
            continue
        start, size = areas[name]
        end = (start & 0xFFFF) + size
        if end > limit:
            bad.append((name, end, limit))
    return bad


def calls_outside_bank(mapfile, asm_name, allow_window=False):
    """Return calls from an active bank to code that cannot be present.

    The boot bank has a strict self-contained contract.  The ASCII16-X rules
    bank may call the fixed resident image and itself, so its external calls
    are checked against the map instead of being rejected.
    """
    asmfile = os.path.join(os.path.dirname(mapfile), asm_name)
    if not os.path.exists(asmfile):
        return []
    text = open(asmfile).read()
    defined = set(re.findall(r"^(_[A-Za-z0-9_]+)::?", text, re.M))
    called = set(re.findall(r"^\s+(?:call|jp)\s+(_[A-Za-z0-9_]+)", text, re.M))
    external = sorted(called - defined)
    if allow_window:
        symbols = map_symbols(mapfile)
        return [(name, symbols.get(name)) for name in external
                if name in symbols and symbols[name] >= WINDOW]
    return [(name, None) for name in external]


def check_isr_window(mapfile):
    """Return SDCC helpers called by lVGM code while the window is swapped."""
    asmfile = os.path.join(os.path.dirname(mapfile), ISR_WINDOW_ASM)
    if not os.path.exists(asmfile):
        return []
    text = open(asmfile).read()
    return sorted(set(re.findall(r"^\s+(?:call|jp)\s+(___[A-Za-z0-9_]+)",
                                 text, re.M)))


def check_data_prefix(mapfile, mapper):
    """Return _DATA symbols that disagree with the crt0 reserved prefix."""
    crt0_bytes = 11 if mapper == "ascii16x" else 15
    in_data = False
    start = None
    intruders = []
    overrun = []
    row_pattern = re.compile(r"^\s+([0-9A-F]{8})\s+(_\S+)\s+(\S+)\s*$")
    with open(mapfile) as handle:
        for line in handle:
            head = re.match(r"^(\S+)\s+([0-9A-F]{8})\s+([0-9A-F]{8})\s+=", line)
            if head:
                in_data = head.group(1) == "_DATA"
                start = int(head.group(2), 16) if in_data else None
                continue
            if not in_data or start is None:
                continue
            row = row_pattern.match(line)
            if not row:
                continue
            at, name, module = int(row.group(1), 16), row.group(2), row.group(3)
            if module in CRT0_MODULES:
                if at >= start + crt0_bytes:
                    overrun.append((name, at))
            elif at < start + crt0_bytes:
                intruders.append((name, at))
    return intruders, overrun, crt0_bytes


def check_assets(rom, mapper):
    """Validate asset ranges and return the files ready to write."""
    entries = assets()
    capacity = len(rom) // SEGMENT_BYTES
    max_segment = 0x0FFF if mapper == "ascii16x" else 0xFFFF
    placed = []
    previous_end = -1
    for name, segment, binary in entries:
        binpath = os.path.join(ASSET_DIR, binary)
        if not os.path.exists(binpath):
            sys.exit("%s is missing: run tools/msx2/gen_msx_scenes.py" % binpath)
        if segment > max_segment:
            sys.exit("%s uses segment %d, beyond the %s mapper range"
                     % (name, segment, mapper))
        data = open(binpath, "rb").read()
        start = segment * SEGMENT_BYTES
        end = start + len(data)
        if segment >= capacity or end > len(rom):
            sys.exit("%s does not fit: ROM is %d KB, needs %d KB -- raise ROM_SIZE_KB"
                     % (name, len(rom) // 1024, (end + 1023) // 1024))
        if start < previous_end:
            sys.exit("asset %s overlaps an earlier manifest entry" % name)
        previous_end = end
        placed.append((name, segment, binary, data, start, end))
    return placed


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    mapper = mapper_name()
    rompath = sys.argv[1]
    rom = bytearray(open(rompath, "rb").read())
    mapfile = os.path.splitext(rompath)[0] + ".map"

    if len(rom) % SEGMENT_BYTES:
        sys.exit("ROM size must be a multiple of 16 KiB")
    requested_kb = int(os.environ.get("MSX2_ROM_SIZE_KB", len(rom) // 1024))
    if len(rom) != requested_kb * 1024:
        sys.exit("ROM is %d KB but MSX2_ROM_SIZE_KB is %d KB"
                 % (len(rom) // 1024, requested_kb))
    signature = b"ASCII16X" if mapper == "ascii16x" else b"ROM_NE16"
    if bytes(rom[0:2]) != b"AB" or bytes(rom[0x10:0x18]) != signature:
        sys.exit("ROM header does not identify the requested %s mapper" % mapper)
    if not os.path.exists(mapfile):
        sys.exit("missing link map: %s" % mapfile)

    intruders, overrun, crt0_bytes = check_data_prefix(mapfile, mapper)
    if intruders or overrun:
        for name, at in intruders:
            print("%s is at 0x%04X, inside the _DATA prefix main() does not wipe"
                  % (name, at), file=sys.stderr)
        for name, at in overrun:
            print("crt0's %s is at 0x%04X, past the %d-byte prefix main() keeps"
                  % (name, at, crt0_bytes), file=sys.stderr)
        sys.exit("MSX2_CRT0_DATA_BYTES in msx2_main.c no longer matches the link")

    overflow = check_code_banks(mapfile, mapper)
    if overflow:
        for name, end, limit in overflow:
            print("%s ends at 0x%04X, past its 0x%04X bank boundary"
                  % (name, end, limit), file=sys.stderr)
        sys.exit("resident code crossed a mapper bank boundary")

    bad = check_resident(mapfile)
    if bad:
        for name, at in bad:
            print("%s is linked at 0x%04X, inside the streaming window"
                  % (name, at), file=sys.stderr)
        sys.exit("the streamer must be linked below 0x%04X" % WINDOW)

    boot_asm = BOOT_BANK_ASM[mapper]
    escapes = calls_outside_bank(mapfile, boot_asm)
    if escapes:
        for name, _at in escapes:
            print("the boot bank calls %s, which is not in the boot bank" % name,
                  file=sys.stderr)
        sys.exit("code in %s may only call itself while its window is active"
                 % boot_asm)

    if mapper == "ascii16x":
        rules_asm = "waifu_msx2_ascii_s7_b1.asm"
        escapes = calls_outside_bank(mapfile, rules_asm, allow_window=True)
        if escapes:
            for name, at in escapes:
                print("the ASCII16-X rules bank calls %s at 0x%04X, outside its"
                      " bank and the fixed resident image" % (name, at),
                      file=sys.stderr)
            sys.exit("ASCII16-X rules bank has an unsafe cross-bank call")

    helpers = check_isr_window(mapfile)
    if helpers:
        for name in helpers:
            print("%s calls %s, an SDCC helper linked above 0x%04X"
                  % (ISR_WINDOW_ASM, name, WINDOW), file=sys.stderr)
        sys.exit("the lVGM decoder may not call a helper above 0x%04X"
                 % WINDOW)

    placed = check_assets(rom, mapper)
    total = 0
    for name, segment, _binary, data, start, end in placed:
        # The manifest starts at segment 8 in both layouts.  Still inspect the
        # exact range so a future manifest cannot hide a linker overlap behind
        # an all-zero or all-FF hole.
        occupied = rom[start:end]
        if any(byte not in (0x00, 0xFF) for byte in occupied):
            sys.exit("segment %d is not empty: %s would overwrite the link"
                     % (segment, name))
        rom[start:end] = data
        total += len(data)
        print("packed %-18s %7d bytes at segment %3d (0x%06X)"
              % (name, len(data), segment, start))

    open(rompath, "wb").write(bytes(rom))
    last_end = max(end for _name, _segment, _binary, _data, _start, end in placed)
    power = 1024
    while power * 1024 < last_end:
        power *= 2
    print("%s: %d assets packed, %d KB of data, last byte at %d KB"
          % (os.path.relpath(rompath, ROOT), len(placed), total // 1024,
             (last_end + 1023) // 1024))
    print("%s cartridge %d KB, smallest that fits %d KB, %d KB of tail padding"
          % (mapper, len(rom) // 1024, power, (len(rom) - last_end) // 1024))


if __name__ == "__main__":
    main()
