#!/usr/bin/env python3
"""Regression test for bundled OpenMSX keyboard/joystick injection."""

from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} openmsx-wrapper", file=sys.stderr)
        return 2
    wrapper = Path(sys.argv[1]).resolve()
    # DI; select MSX keyboard row 8 on PPI C; read PPI B; copy it to L;
    # loop.  SPACE is bit 0 of row 8 and is active-low.
    rom_bytes = bytes.fromhex("f3 3e 08 d3 aa db a9 6f c3 05 40")
    with tempfile.TemporaryDirectory(prefix="openmsx-input-test-") as temp:
        root = Path(temp)
        rom = root / "probe.rom"
        script = root / "probe.input"
        joy_rom = root / "joystick.rom"
        joy2_rom = root / "joystick2.rom"
        joy_script = root / "joystick.input"
        joy2_script = root / "joystick2.input"
        rom.write_bytes(rom_bytes)
        script.write_text("0f:+SPACE\n", encoding="utf-8")
        # Select PSG register 15/port A connector 1, select register 14/port
        # A input, read it into L, and loop on the read.  RIGHT is bit 3.
        joy_code = bytes.fromhex("f3 3e 0f d3 a0 3e 00 d3 a1 3e 0e d3 a0 db a2 6f c3 0d 40")
        joy_rom.write_bytes(joy_code)
        # PSG R#15 bit 6 selects connector 2.  The input schedule exposes
        # the same deterministic state on both connectors.
        joy2_rom.write_bytes(joy_code.replace(bytes.fromhex("3e 00 d3 a1"), bytes.fromhex("3e 4f d3 a1"), 1))
        joy_script.write_text("0f:+RIGHT\n", encoding="utf-8")
        joy2_script.write_text("0f:+RIGHT\n", encoding="utf-8")
        result = subprocess.run(
            [str(wrapper), "input", "run", str(rom), str(script), "1", "--direct-cart"],
            text=True,
            capture_output=True,
        )
        joy_result = subprocess.run(
            [str(wrapper), "input", "run", str(joy_rom), str(joy_script), "1", "--direct-cart"],
            text=True,
            capture_output=True,
        )
        joy2_result = subprocess.run(
            [str(wrapper), "input", "run", str(joy2_rom), str(joy2_script), "1", "--direct-cart"],
            text=True,
            capture_output=True,
        )
    if result.returncode != 0:
        print(result.stdout, end="")
        print(result.stderr, end="", file=sys.stderr)
        return result.returncode
    if "HL=#00FE" not in result.stdout:
        print(result.stdout, end="")
        print("input-test: SPACE was not observed on MSX keyboard row 8", file=sys.stderr)
        return 1
    if joy_result.returncode != 0 or "HL=#0037" not in joy_result.stdout:
        print(joy_result.stdout, end="")
        print("bundled input-test: RIGHT was not observed on joystick port 1", file=sys.stderr)
        return joy_result.returncode or 1
    if joy2_result.returncode != 0 or "HL=#0037" not in joy2_result.stdout:
        print(joy2_result.stdout, end="")
        print("bundled input-test: RIGHT was not observed on joystick port 2", file=sys.stderr)
        return joy2_result.returncode or 1
    print("bundled input injection: ok (keyboard SPACE + joystick ports 1/2 RIGHT)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
