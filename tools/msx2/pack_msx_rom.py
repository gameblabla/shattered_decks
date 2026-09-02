#!/usr/bin/env python3
"""Write the baked scene binaries into the built NEO cartridge image.

MSXgl links code into segments 0-2 and pads the rest of the cartridge; the
picture data is not part of the link at all (it could never fit in the 32 KB the
Z80 addresses), so it is placed here, at the exact segments
tools/msx2/gen_msx_scenes.py assigned and src/generated/msx2_scenes.h names.

The pass also enforces the one invariant the streamer depends on: the routine
that swaps the 0x8000 window must itself live *below* 0x8000, because while the
window holds picture data none of the code up there exists.  A link that drifts
past that line is a crash on the first stream, so it fails the build instead.
It also rejects either resident code area crossing its physical 16/32 KB bank
boundary; SDCC can otherwise emit an apparently successful, wrapped image.

Usage:
    tools/msx2/pack_msx_rom.py src/msx2/out/waifu_msx2.rom
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ASSET_DIR = os.path.join(ROOT, "src", "msx2", "assets")
MANIFEST = os.path.join(ASSET_DIR, "manifest.txt")
SEGMENT_BYTES = 16 * 1024
WINDOW = 0x8000
CODE_END = 0xC000
SEG2_END = 0x4000

# Symbols that must be resident while the streaming window is swapped out.
# They live in the page-0 code bank (src/msx2/waifu_msx2_s2_b0.c), which the
# mapper never switches; this check is what notices if one ever stops.
RESIDENT_SYMBOLS = [
    "_Msx2_StreamChunk", "_Msx2_StreamSetVramChunk", "_Msx2_StreamScene",
    "_Msx2_BlitRow", "_Msx2_StreamRect", "_Msx2_RomRead",
]


def assets():
    """(name, first segment, binary) triples, in segment order."""
    out = []
    for raw in open(MANIFEST):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        name, seg, binary = line.split()
        out.append((name, int(seg), binary))
    return sorted(out, key=lambda entry: entry[1])


def check_resident(mapfile):
    """Every streamer symbol must sit below 0x8000 in the Z80's address space.

    Banked symbols are printed by the linker as SSSSAAAA -- the segment number
    above the address it is linked at -- so only the low 16 bits are the
    address the CPU will see."""
    if not os.path.exists(mapfile):
        return []
    addresses = {}
    for line in open(mapfile):
        for addr, name in re.findall(r"([0-9A-F]{4,8})\s+(_[A-Za-z0-9_]+)", line):
            addresses.setdefault(name, int(addr, 16) & 0xFFFF)
    bad = []
    for name in RESIDENT_SYMBOLS:
        at = addresses.get(name)
        if at is not None and at >= WINDOW:
            bad.append((name, at))
    return bad


# main() wipes _DATA above the bytes crt0 has already filled in, because SDCC
# puts uninitialised statics there and MSXgl's ROM crt0 never clears them.  The
# size of that reserved prefix is a constant in msx2_main.c, so the link has to
# be checked against it: a game variable that lands inside the prefix would keep
# its boot garbage, and a crt0 that grew past it would have its own state wiped.
CRT0_DATA_BYTES = 15
CRT0_MODULES = ("crt0",)


def check_data_prefix(mapfile):
    """Return (name, address) for any non-crt0 _DATA symbol below the prefix,
    and the crt0 symbols that sit at or above it."""
    if not os.path.exists(mapfile):
        return [], []
    in_data = False
    start = None
    intruders = []
    overrun = []
    for line in open(mapfile):
        head = re.match(r"^(\S+)\s+([0-9A-F]{8})\s+([0-9A-F]{8})\s+=", line)
        if head:
            in_data = head.group(1) == "_DATA"
            if in_data:
                start = int(head.group(2), 16)
            continue
        if not in_data or start is None:
            continue
        row = re.match(r"^\s+([0-9A-F]{8})\s+(_\S+)\s+(\S+)\s*$", line)
        if not row:
            continue
        at, name, module = int(row.group(1), 16), row.group(2), row.group(3)
        if module in CRT0_MODULES:
            if at >= start + CRT0_DATA_BYTES:
                overrun.append((name, at))
        elif at < start + CRT0_DATA_BYTES:
            intruders.append((name, at))
    return intruders, overrun


def check_code_banks(mapfile):
    """Return linker areas whose low-16-bit end crosses their mapped bank."""
    if not os.path.exists(mapfile):
        return []
    limits = {"_CODE": CODE_END, "_SEG2": SEG2_END}
    found = {}
    pattern = re.compile(r"^\s*(_CODE|_SEG2)\s+([0-9A-F]{8})\s+([0-9A-F]{8})\s+=")
    for line in open(mapfile):
        match = pattern.match(line)
        if match:
            found[match.group(1)] = (int(match.group(2), 16),
                                     int(match.group(3), 16))
    bad = []
    for name, limit in limits.items():
        if name in found:
            start, size = found[name]
            end = (start & 0xFFFF) + size
            if end > limit:
                bad.append((name, end, limit))
    return bad


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    rompath = sys.argv[1]
    rom = bytearray(open(rompath, "rb").read())
    mapfile = os.path.splitext(rompath)[0] + ".map"

    intruders, overrun = check_data_prefix(mapfile)
    if intruders or overrun:
        for name, at in intruders:
            print("%s is at 0x%04X, inside the _DATA prefix main() does not wipe"
                  % (name, at), file=sys.stderr)
        for name, at in overrun:
            print("crt0's %s is at 0x%04X, past the %d-byte prefix main() keeps"
                  % (name, at, CRT0_DATA_BYTES), file=sys.stderr)
        sys.exit("MSX2_CRT0_DATA_BYTES in msx2_main.c no longer matches the link")

    overflow = check_code_banks(mapfile)
    if overflow:
        for name, end, limit in overflow:
            print("%s ends at 0x%04X, past its 0x%04X bank boundary"
                  % (name, end, limit), file=sys.stderr)
        sys.exit("resident code crossed a mapper bank boundary")

    bad = check_resident(mapfile)
    if bad:
        for name, at in bad:
            print("%s is linked at 0x%04X, inside the streaming window" % (name, at),
                  file=sys.stderr)
        sys.exit("the streamer must be linked below 0x%04X -- reorder ProjModules "
                 "or move code out of bank 2" % WINDOW)

    placed = 0
    total = 0
    for name, segment, binary in assets():
        binpath = os.path.join(ASSET_DIR, binary)
        if not os.path.exists(binpath):
            sys.exit("%s is missing: run tools/msx2/gen_msx_scenes.py" % binpath)
        data = open(binpath, "rb").read()
        at = segment * SEGMENT_BYTES
        if at + len(data) > len(rom):
            sys.exit("%s does not fit: ROM is %d KB, needs %d KB -- raise ROM_SIZE_KB"
                     % (name, len(rom) // 1024,
                        (at + len(data) + 1023) // 1024))
        # Refuse to land on anything the linker emitted.
        occupied = rom[at:at + len(data)]
        if any(b not in (0x00, 0xFF) for b in occupied):
            sys.exit("segment %d is not empty: %s would overwrite the link"
                     % (segment, name))
        rom[at:at + len(data)] = data
        placed += 1
        total += len(data)
        print("packed %-13s %6d bytes at segment %2d (0x%06X)"
              % (name, len(data), segment, at))

    open(rompath, "wb").write(bytes(rom))
    print("%s: %d assets packed, %d KB of cartridge used"
          % (os.path.relpath(rompath, ROOT), placed, total // 1024))


if __name__ == "__main__":
    main()
