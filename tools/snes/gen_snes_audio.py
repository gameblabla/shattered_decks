#!/usr/bin/env python3
"""Build the SNES SPC snapshots used by the cartridge audio frontend.

The MIDI2SPC source supplied with the project emits a complete SPC700 player
and a standard SPC snapshot.  This tool keeps that converter as the source of
the music and BRR data, then adds the small cartridge-only reload protocol
needed to change songs without rebooting the APU.

The runtime uses the existing MIDI2SPC SFX protocol unchanged:

    APUIO1/F5 = logical SFX slot, APUIO2/F6 = left volume,
    APUIO3/F7 = right volume, APUIO0/F4 = non-zero command token.

The reload protocol reserves F5=$FF for a 256-byte ARAM block and F5=$FE for
restart.  Each block transfers three bytes per token using F5/F6/F7, keeping
scene changes short enough for a cartridge transition.  The main CPU streams
the new snapshot's music/sample regions while force-blanked; the shared player
code at $0200-$0C0F is kept in place.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import struct
import sys


REPO = Path(__file__).resolve().parents[2]
CONVERTER_TOOLS = (
    REPO / "snes_music" / "MIDI2SPC_v23_altbattle_decrackle_release"
    / "source" / "c_transcriber" / "tools"
)
sys.path.insert(0, str(CONVERTER_TOOLS))

import midi2spc  # noqa: E402  (the supplied converter is intentionally local)


PROGRAM_ADDRESS = midi2spc.PROGRAM_ADDRESS
PITCH_LOW_TABLE = midi2spc.PITCH_LOW_TABLE
SPC_HEADER_SIZE = 0x100
SPC_RAM_SIZE = 0x10000
SFX_SOURCE_BASE = midi2spc.SFX_SOURCE_BASE
SFX_SLOT_COUNT = 4
SFX_FIXED_ADDRESS = 0xB400

DP_RELOAD_TOKEN = 0x62
DP_RELOAD_ADDR_LO = 0x63
DP_RELOAD_ADDR_HI = 0x64
DP_RELOAD_RX_TOKEN = 0x65
DP_RELOAD_COUNT = 0x66
DP_RELOAD_HANDLED = 0x67

RELOAD_BLOCK = 0xFF
RELOAD_RESTART = 0xFE

SFX_SAMPLES = (
    REPO / "sounds" / "CardPlaced.wav",
    REPO / "sounds" / "ConfirmAlt.wav",
    REPO / "sounds" / "laserShoot.wav",
    REPO / "sounds" / "Select.wav",
)

TRACKS = (
    ("titlealt", "TitleAlt.midi", "full", True),
    ("overworld", "Overworld.midi", "56k", False),
    ("altbattle", "AltBattle.midi", "32k", False),
    ("victory", "Victory.midi", "56k", False),
    ("fail", "Fail.midi", "56k", False),
)


def _load_a_dp(code, dp):
    code.emit(0xE4, dp)              # MOV A,dp


def _store_a_dp(code, dp):
    code.emit(0xC4, dp)              # MOV dp,A


def _imm_dp(code, dp, value):
    code.emit(0x8F, value, dp)       # MOV dp,#imm


def _inc_dp(code, dp):
    code.emit(0xAB, dp)               # INC dp


def _cmp_imm(code, value):
    code.emit(0x68, value)           # CMP A,#imm


def _call_abs(code, address):
    code.emit(0x3F, address & 0xFF, (address >> 8) & 0xFF)


def _build_reload_runtime(original_labels, original_length):
    """Return a wrapper plus reload handler for the supplied player build."""
    poll_sfx = original_labels["poll_sfx"]
    runtime_address = PROGRAM_ADDRESS + original_length
    code = midi2spc.Code(runtime_address)

    # tick_done originally calls poll_sfx.  Redirect that existing CALL to
    # this wrapper so no existing instruction has to move.  Reload runs first
    # because its FF/FE commands are outside the SFX slot range; ordinary SFX
    # commands then go through the converter's original handler.
    code.label("audio_tick_wrapper")
    code.call("reload_poll")
    _load_a_dp(code, DP_RELOAD_HANDLED)
    _cmp_imm(code, 0)
    code.branch(0xD0, "audio_tick_sync")  # BNE: reload consumed the tick
    _call_abs(code, poll_sfx)
    code.label("audio_tick_sync")
    _load_a_dp(code, 0xF4)
    _store_a_dp(code, DP_RELOAD_TOKEN)
    code.emit(0x6F)                   # RET

    code.label("reload_poll")
    _imm_dp(code, DP_RELOAD_HANDLED, 0)
    _load_a_dp(code, 0xF4)
    code.emit(0x64, DP_RELOAD_TOKEN)  # CMP A,token
    code.branch(0xF0, "reload_poll_done")
    _load_a_dp(code, 0xF5)
    _cmp_imm(code, RELOAD_BLOCK)
    code.branch(0xF0, "reload_block")
    _load_a_dp(code, 0xF5)
    _cmp_imm(code, RELOAD_RESTART)
    code.branch(0xF0, "reload_restart")
    code.emit(0x6F)

    code.label("reload_block")
    _imm_dp(code, DP_RELOAD_HANDLED, 1)
    _load_a_dp(code, 0xF4)
    _store_a_dp(code, DP_RELOAD_TOKEN)
    _store_a_dp(code, DP_RELOAD_RX_TOKEN)
    _load_a_dp(code, 0xF6)
    _store_a_dp(code, DP_RELOAD_ADDR_LO)
    _load_a_dp(code, 0xF7)
    _store_a_dp(code, DP_RELOAD_ADDR_HI)
    _imm_dp(code, 0xF1, 0)            # stop timer while ARAM is replaced
    _imm_dp(code, 0xF2, 0x5C)         # S-DSP KOFF
    _imm_dp(code, 0xF3, 0)
    _load_a_dp(code, DP_RELOAD_TOKEN)
    _store_a_dp(code, 0xF4)           # command acknowledgement
    _imm_dp(code, DP_RELOAD_COUNT, 0)
    code.call("reload_receive")
    _imm_dp(code, 0xF1, 1)
    code.emit(0x6F)

    code.label("reload_restart")
    _imm_dp(code, DP_RELOAD_HANDLED, 1)
    _load_a_dp(code, 0xF4)
    _store_a_dp(code, DP_RELOAD_TOKEN)
    _store_a_dp(code, 0xF4)
    # The reload call stack is intentionally discarded before entry restarts
    # the player.  This also makes repeated scene changes safe indefinitely.
    code.emit(0xCD, 0xFF, 0xBD)       # MOV X,#$FF / MOV SP,X
    code.emit(0x5F, PROGRAM_ADDRESS & 0xFF, PROGRAM_ADDRESS >> 8)

    code.label("reload_poll_done")
    code.emit(0x6F)

    code.label("reload_receive")
    code.label("reload_receive_wait")
    _load_a_dp(code, 0xF4)
    code.emit(0x64, DP_RELOAD_RX_TOKEN)
    code.branch(0xF0, "reload_receive_wait")
    _load_a_dp(code, DP_RELOAD_COUNT)
    _cmp_imm(code, 0xFF)
    code.branch(0xF0, "reload_receive_last")
    _load_a_dp(code, 0xF4)
    _store_a_dp(code, DP_RELOAD_RX_TOKEN)
    code.emit(0x8D, 0)                  # Y=0
    _load_a_dp(code, 0xF5)              # three data bytes from the CPU
    code.emit(0xD7, DP_RELOAD_ADDR_LO) # MOV [addr]+Y,A
    code.emit(0x3A, DP_RELOAD_ADDR_LO) # INCW addr
    _load_a_dp(code, 0xF6)
    code.emit(0xD7, DP_RELOAD_ADDR_LO)
    code.emit(0x3A, DP_RELOAD_ADDR_LO)
    _load_a_dp(code, 0xF7)
    code.emit(0xD7, DP_RELOAD_ADDR_LO)
    code.emit(0x3A, DP_RELOAD_ADDR_LO)
    _load_a_dp(code, 0xF4)              # echo the token after the ARAM writes
    _store_a_dp(code, DP_RELOAD_RX_TOKEN)
    _store_a_dp(code, 0xF4)
    _inc_dp(code, DP_RELOAD_COUNT)
    _inc_dp(code, DP_RELOAD_COUNT)
    _inc_dp(code, DP_RELOAD_COUNT)
    code.branch(0x2F, "reload_receive_wait")  # BRA

    code.label("reload_receive_last")
    _load_a_dp(code, 0xF4)
    _store_a_dp(code, DP_RELOAD_RX_TOKEN)
    code.emit(0x8D, 0)
    _load_a_dp(code, 0xF5)
    code.emit(0xD7, DP_RELOAD_ADDR_LO)
    code.emit(0x3A, DP_RELOAD_ADDR_LO)
    _load_a_dp(code, 0xF4)
    _store_a_dp(code, DP_RELOAD_RX_TOKEN)
    _store_a_dp(code, 0xF4)
    _inc_dp(code, DP_RELOAD_COUNT)
    code.emit(0x6F)

    runtime = bytearray(code.finalize())
    if len(runtime) + runtime_address >= PITCH_LOW_TABLE:
        raise ValueError(
            f"reload runtime ends at ${runtime_address + len(runtime):04X}, "
            f"past the pitch table at ${PITCH_LOW_TABLE:04X}")
    return runtime, runtime_address


def _render_with_labels(*args, **kwargs):
    """Render once and retain the generated program labels for patching."""
    captured = {}
    original_finalize = midi2spc.Code.finalize

    def capture_finalize(code):
        result = original_finalize(code)
        if code.address == PROGRAM_ADDRESS:
            captured["labels"] = dict(code.labels)
            captured["length"] = len(result)
        return result

    midi2spc.Code.finalize = capture_finalize
    try:
        stats = midi2spc.render(*args, **kwargs)
    finally:
        midi2spc.Code.finalize = original_finalize
    if "labels" not in captured:
        raise RuntimeError("MIDI2SPC did not expose its SPC700 program")
    return stats, captured


def _patch_runtime(output_path, captured):
    data = bytearray(output_path.read_bytes())
    if len(data) != midi2spc.SPC_SIZE:
        raise ValueError(f"unexpected SPC size for {output_path}: {len(data)}")
    ram = data[SPC_HEADER_SIZE:SPC_HEADER_SIZE + SPC_RAM_SIZE]
    original_length = captured["length"]
    labels = captured["labels"]
    tick_offset = labels["tick_done"] - PROGRAM_ADDRESS
    if ram[PROGRAM_ADDRESS + tick_offset] != 0x3F:
        raise ValueError("MIDI2SPC tick hook no longer has its expected CALL")

    runtime, runtime_address = _build_reload_runtime(labels, original_length)
    wrapper_offset = runtime_address - PROGRAM_ADDRESS
    ram[runtime_address:runtime_address + len(runtime)] = runtime
    # The original tick hook is CALL poll_sfx; redirect its operand to the
    # wrapper.  Its three-byte footprint is unchanged.
    ram[PROGRAM_ADDRESS + tick_offset + 1] = runtime_address & 0xFF
    ram[PROGRAM_ADDRESS + tick_offset + 2] = runtime_address >> 8

    data[SPC_HEADER_SIZE:SPC_HEADER_SIZE + SPC_RAM_SIZE] = ram
    output_path.write_bytes(data)
    return runtime_address, len(runtime), wrapper_offset


def _strip_title_sfx(output_path, stats):
    data = bytearray(output_path.read_bytes())
    ram = data[SPC_HEADER_SIZE:SPC_HEADER_SIZE + SPC_RAM_SIZE]
    sfx_address = int(stats["sfx_address"], 16)
    sfx_bytes = int(stats["sfx_sample_bytes"])
    ram[sfx_address:sfx_address + sfx_bytes] = bytes(sfx_bytes)
    directory = midi2spc.ECHO_DIRECTORY_ADDRESS
    for slot in range(SFX_SLOT_COUNT):
        start = directory + (SFX_SOURCE_BASE + slot) * 4
        ram[start:start + 4] = bytes(4)
    data[SPC_HEADER_SIZE:SPC_HEADER_SIZE + SPC_RAM_SIZE] = ram
    output_path.write_bytes(data)


def _relocate_sfx(output_path, stats):
    """Put the shared direct-SFX bank at one address in every music image."""
    data = bytearray(output_path.read_bytes())
    ram = data[SPC_HEADER_SIZE:SPC_HEADER_SIZE + SPC_RAM_SIZE]
    old_address = int(stats["sfx_address"], 16)
    sfx_bytes = int(stats["sfx_sample_bytes"])
    if SFX_FIXED_ADDRESS + sfx_bytes > midi2spc.ECHO_DIRECTORY_ADDRESS:
        raise ValueError("fixed SFX bank would overlap the echo directory")
    bank = bytes(ram[old_address:old_address + sfx_bytes])
    ram[SFX_FIXED_ADDRESS:SFX_FIXED_ADDRESS + sfx_bytes] = bank
    if old_address != SFX_FIXED_ADDRESS:
        ram[old_address:old_address + sfx_bytes] = bytes(sfx_bytes)

    directory = midi2spc.ECHO_DIRECTORY_ADDRESS
    for slot in range(SFX_SLOT_COUNT):
        entry = directory + (SFX_SOURCE_BASE + slot) * 4
        address, loop = struct.unpack_from("<HH", ram, entry)
        if address == 0 and loop == 0:
            continue
        struct.pack_into(
            "<HH", ram, entry,
            SFX_FIXED_ADDRESS + address - old_address,
            SFX_FIXED_ADDRESS + loop - old_address)
    data[SPC_HEADER_SIZE:SPC_HEADER_SIZE + SPC_RAM_SIZE] = ram
    output_path.write_bytes(data)


def _write_layout_header(layouts):
    path = REPO / "src" / "snes" / "snes_audio_layout.h"
    lines = [
        "/* Generated by tools/snes/gen_snes_audio.py. */",
        "#ifndef WAIFU_SNES_AUDIO_LAYOUT_H",
        "#define WAIFU_SNES_AUDIO_LAYOUT_H",
        "",
        f"#define SNES_AUDIO_MUSIC_START 0x{PITCH_LOW_TABLE:04X}u",
        f"#define SNES_AUDIO_SFX_ADDRESS 0x{SFX_FIXED_ADDRESS:04X}u",
    ]
    for name, music_end in layouts:
        lines.append(f"#define SNES_AUDIO_{name.upper()}_END 0x{music_end:04X}u")
    lines += ["", "#endif /* WAIFU_SNES_AUDIO_LAYOUT_H */", ""]
    path.write_text("\n".join(lines), encoding="ascii")


def build(output_dir):
    output_dir.mkdir(parents=True, exist_ok=True)
    common = {
        "title": "Shattered Decks",
        "game": "Shattered Decks",
        "author": "Waifu Card Game",
        "voices": 8,
        "loop": True,
        "soundfont": REPO / "snes_music" / "MMX Revised" / "MMX1.sf2",
        "capcom_echo": True,
        "native_sf2": True,
        "brr_donor": REPO / "snes_music" / "MMX_spc",
        "sequence_opt": True,
        "sfx_samples": SFX_SAMPLES,
        "sfx_voices": 2,
        "sfx_compression": "off",
        "music_compression": "off",
        "brr2_policy": "adaptive",
        "nesdev_profile": "off",
    }
    layouts = []
    for name, midi_name, budget, strip_sfx in TRACKS:
        midi_path = REPO / "snes_music" / "midi" / midi_name
        output_path = output_dir / f"{name}.spc"
        stats, captured = _render_with_labels(
            midi_path, output_path, aram_budget=budget, **common)
        runtime_address, runtime_size, _ = _patch_runtime(output_path, captured)
        if strip_sfx:
            _strip_title_sfx(output_path, stats)
        else:
            _relocate_sfx(output_path, stats)
        layouts.append((name, int(stats["music_aram_end"], 16)))
        print(
            f"{name}: {budget}, music end {stats['music_aram_end']}, "
            f"SFX {stats['sfx_sample_bytes']} bytes, "
            f"runtime ${runtime_address:04X}+{runtime_size}")
    _write_layout_header(layouts)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--output-dir", type=Path,
        default=REPO / "src" / "snes" / "assets" / "audio",
        help="directory for generated SPC snapshots")
    args = parser.parse_args(argv)
    build(args.output_dir.resolve())


if __name__ == "__main__":
    main()
