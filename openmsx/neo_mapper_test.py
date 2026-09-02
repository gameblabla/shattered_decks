#!/usr/bin/env python3
"""Exercise NEO Mapper 1.2 register bits above the old 12-bit ceiling."""

from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path


ROM_SIZE = 80 * 1024 * 1024


def build(path: Path, neo16: bool, bank: int, marker: int, alias_marker: int) -> None:
    data = bytearray([0xFF]) * ROM_SIZE
    data[:16] = bytes([0x41, 0x42, 0x40, 0x40]) + bytes(12)
    if neo16:
        data[16:24] = b"ROM_NE16"
        register, read_address, bank_size = 0x7000, 0x8000, 0x4000
    else:
        data[16:24] = b"ROM_NEO8"
        register, read_address, bank_size = 0x6800, 0x6000, 0x2000
    code = bytes([
        0x3E, bank & 0xFF,
        0x32, register & 0xFF, register >> 8,
        0x3E, (bank >> 8) & 0x3F,
        0x32, (register + 1) & 0xFF, (register + 1) >> 8,
        0x3A, read_address & 0xFF, read_address >> 8,
        0x47, 0x76,
    ])
    data[0x40:0x40 + len(code)] = code
    # The former 0x0F high-byte mask aliases both selected banks to segment 1.
    data[bank_size] = alias_marker
    data[bank * bank_size] = marker
    path.write_bytes(data)


def check(executable: Path, rom: Path, mapper: str, marker: int) -> None:
    header = subprocess.check_output(
        [str(executable), "header", str(rom)], text=True
    )
    if f"mapper: {mapper}" not in header:
        raise AssertionError(header)
    state = subprocess.check_output(
        [str(executable), "step", str(rom), "7", "--direct-cart"], text=True
    )
    if f"BC=#{marker:02X}00" not in state:
        raise AssertionError(state)


def main(argv: list[str]) -> int:
    executable = Path(argv[0]) if argv else Path(__file__).resolve().parent / "openmsx"
    if not executable.is_file():
        print(f"NEO mapper test: executable not found: {executable}", file=sys.stderr)
        return 2
    with tempfile.TemporaryDirectory(prefix="openmsx-neo-") as directory:
        root = Path(directory)
        neo8 = root / "neo8_80mb_bank2001.rom"
        neo16 = root / "neo16_80mb_bank1001.rom"
        build(neo8, False, 0x2001, 0xA5, 0x11)
        build(neo16, True, 0x1001, 0x5A, 0x22)
        check(executable, neo8, "NEO-8", 0xA5)
        check(executable, neo16, "NEO-16", 0x5A)
    print("NEO Mapper 1.2 14-bit large-ROM tests: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
