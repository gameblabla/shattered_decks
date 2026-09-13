#!/usr/bin/env python3
"""Build the SNES SPC snapshots used by the cartridge audio frontend.

The MIDI2SPC source supplied with the project emits a complete SPC700 player
and a standard SPC snapshot.  This tool keeps that converter as the source of
the music and its BRR data, then adds the cartridge-only runtime:

  * the reload protocol that changes songs without rebooting the APU
    (F5=$FF: 256-byte ARAM block, F5=$FE: restart), and
  * the 2-bit SFX streamer.

MUSIC AND SFX SHARE ONE 64K BUDGET.  The SFX bank is the same nine effects the
PC build plays (tools/gen_sound_assets.py), at the PC-FX's 16 kHz, packed to
2.5 bits a sample (tools/snes/snes_sfx_brr2.py) and resident in every image at
one fixed address under the DSP directory.  Whatever ARAM is left below it is
the music budget of every track, so the converter compacts each song to fit
next to the effects rather than the effects being cut to fit the song.

The S-DSP cannot play packed data, so each of the two SFX voices owns a small
ring of real BRR blocks (24 blocks, END+LOOP on the last) that the voice loops
over; the SPC700 expands packed blocks into the ring ahead of the DSP, five
blocks every 200 Hz tick at 16 kHz, and writes the final block with END alone
so the voice stops by itself.  Trigger latency is one prefill (12 blocks).

The runtime SFX command is the MIDI2SPC one the CPU driver already speaks:
APUIO1/F5 = slot, APUIO2/F6 = left volume, APUIO3/F7 = right volume,
APUIO0/F4 = non-zero command token, echoed back once accepted.
"""

from __future__ import annotations

import argparse
import ast
import json
from pathlib import Path
import re
import struct
import sys

import numpy as np


REPO = Path(__file__).resolve().parents[2]
CONVERTER_TOOLS = (
    REPO / "snes_music" / "MIDI2SPC_v23_altbattle_decrackle_release"
    / "source" / "c_transcriber" / "tools"
)
sys.path.insert(0, str(CONVERTER_TOOLS))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import midi2spc  # noqa: E402  (the supplied converter is intentionally local)
import snes_sfx_brr2 as brr2  # noqa: E402


PROGRAM_ADDRESS = midi2spc.PROGRAM_ADDRESS
PITCH_LOW_TABLE = midi2spc.PITCH_LOW_TABLE
SPC_HEADER_SIZE = 0x100
SPC_RAM_SIZE = 0x10000
DIRECTORY_ADDRESS = midi2spc.ECHO_DIRECTORY_ADDRESS
SFX_SOURCE_BASE = midi2spc.SFX_SOURCE_BASE

# Direct page.  $62-$6F belong to this runtime ($67 is the converter's own
# DP_FINITE), $B0-$CF hold the two 2-bit residual alphabets, $D0-$DF the two
# SFX voices' stream state and $E0-$EF the streamer's scratch.
DP_RELOAD_TOKEN = 0x62
DP_RELOAD_ADDR_LO = 0x63
DP_RELOAD_ADDR_HI = 0x64
DP_RELOAD_RX_TOKEN = 0x65
DP_RELOAD_COUNT = 0x66
DP_FINITE = midi2spc.DP_FINITE
DP_FINITE_PENDING = 0x68
DP_MIN_LEAD0 = 0x69
DP_MIN_LEAD1 = 0x6A

DP_PAIR_TABLE = 0xB0
DP_VOICE = 0xD0            # + 8 * voice
V_SRC_LO, V_SRC_HI, V_REM_LO, V_REM_HI, V_WPOS, V_LEAD, V_ACTIVE, V_UNDERRUN = range(8)
S_SRC = 0xE0               # 2 bytes
S_DST = 0xE2               # 2 bytes
S_TMP = 0xE4
S_TBL = 0xE5
S_FLAGS = 0xE6
S_VX = 0xE7
S_TICKS = 0xE8
S_CONSUMED = 0xE9
S_CURV = 0xEA
S_SLOT = 0xEB
S_MASK = 0xEC
S_LATE_LO = 0xED           # saturating 16-bit count of late timer reads
S_LATE_HI = 0xEE
S_WORST = 0xEF             # worst timer backlog returned by $FD

RELOAD_BLOCK = 0xFF
RELOAD_RESTART = 0xFE
RELOAD_BYTES = 144         # divisible by 3 and aligns $0C10 exactly to $8080

SFX_VOICES = 2
SFX_FIRST_VOICE = 6
RING_BLOCKS = 24
RING_BYTES = RING_BLOCKS * brr2.BRR_BLOCK_BYTES
BLOCKS_PER_TICK = 4        # 12.8 kHz / 200 Hz / 16 samples
LEAD_TARGET = 12           # blocks kept decoded ahead of the DSP
SFX_PITCH = 0x0666         # 12.8 kHz on a 32 kHz DSP

# Slot order mirrors WaifuSoundEffect (src/game/sounds.h) so the game code
# can use the PC's effect names one for one; the sources are the PC build's
# first variant of each (tools/gen_sound_assets.py).  YOU_LOST is a music cue
# on every console port and stays an empty slot.  The two longest effects are
# capped: their tails are what the whole budget would otherwise go to.
SFX_SLOTS = (
    ("SELECT", "random-ui.wav", None),
    ("CONFIRM", "random-ui(1).wav", None),
    ("CONFIRM_ALT", "random-ui(2).wav", None),
    ("CARD_PLACED", "mech-stomp.wav", None),
    ("CARD_DESTROYED", "explosion(1).wav", 1.0),
    ("TURN_PASSED", "random-magic.wav", 1.0),
    ("YOU_LOST", None, None),
    ("LASER_SHOOT", "random-impact.wav", None),
    ("DIRECT_HIT", "multi-hit-rush-3-overkill.wav", None),
    ("CARD_DRAWN", "coin-pickup.wav", None),
)
SFX_DIR = REPO / "sounds" / "new"

TRACKS = (
    ("titlealt", "TitleAlt.midi"),
    ("overworld", "Overworld.midi"),
    ("altbattle", "AltBattle.midi"),
    ("victory", "Victory.midi"),
    ("fail", "Fail.midi"),
)


def validate_sfx_contract():
    """Keep the console slots tied to the portable mixer's public contract."""
    sounds_h = (REPO / "src" / "game" / "sounds.h").read_text(encoding="utf-8")
    enum = re.search(r"typedef enum WaifuSoundEffect\s*\{(.*?)\}", sounds_h,
                     re.DOTALL)
    if not enum:
        raise RuntimeError("cannot find WaifuSoundEffect in src/game/sounds.h")
    semantic = []
    for token in enum.group(1).split(","):
        match = re.search(r"WAIFU_SOUND_([A-Z0-9_]+)", token)
        if match and match.group(1) != "EFFECT_COUNT":
            semantic.append(match.group(1))

    source = (REPO / "tools" / "gen_sound_assets.py").read_text(encoding="utf-8")
    tree = ast.parse(source)
    variants = None
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(
                isinstance(target, ast.Name) and target.id == "SOUND_VARIANTS"
                for target in node.targets):
            variants = ast.literal_eval(node.value)
            break
    if variants is None:
        raise RuntimeError("cannot find SOUND_VARIANTS in tools/gen_sound_assets.py")
    pc_names = [row[0] for row in variants]
    snes_names = [row[0] for row in SFX_SLOTS]
    if semantic != pc_names or semantic != snes_names:
        raise RuntimeError("SFX slot order differs: enum=%r PC=%r SNES=%r" %
                           (semantic, pc_names, snes_names))
    for (name, pc_sources), (_snes_name, filename, _cap) in zip(variants, SFX_SLOTS):
        expected = Path(pc_sources[0]).name if pc_sources else None
        if filename != expected:
            raise RuntimeError("%s source differs: PC=%r SNES=%r" %
                               (name, expected, filename))


# ── SPC700 emit helpers ──────────────────────────────────────────────────────

def _load_a_dp(code, dp):
    code.emit(0xE4, dp)              # MOV A,dp


def _store_a_dp(code, dp):
    code.emit(0xC4, dp)              # MOV dp,A


def _load_a_dpx(code, dp):
    code.emit(0xF4, dp)              # MOV A,dp+X


def _store_a_dpx(code, dp):
    code.emit(0xD4, dp)              # MOV dp+X,A


def _imm_dp(code, dp, value):
    code.emit(0x8F, value, dp)       # MOV dp,#imm


def _inc_dp(code, dp):
    code.emit(0xAB, dp)              # INC dp


def _cmp_imm(code, value):
    code.emit(0x68, value)           # CMP A,#imm


def _imm_a(code, value):
    code.emit(0xE8, value)           # MOV A,#imm


def _imm_y(code, value):
    code.emit(0x8D, value)           # MOV Y,#imm


def _imm_x(code, value):
    code.emit(0xCD, value)           # MOV X,#imm


def _load_a_absx(code, address):
    code.emit(0xF5, address & 0xFF, address >> 8)   # MOV A,!abs+X


def _load_a_absy(code, address):
    code.emit(0xF6, address & 0xFF, address >> 8)   # MOV A,!abs+Y


def _call_abs(code, address):
    code.emit(0x3F, address & 0xFF, (address >> 8) & 0xFF)


# ── the SFX bank ─────────────────────────────────────────────────────────────

def build_sfx_bank():
    """Encode every slot; returns (bank bytes, [(offset, blocks)], report)."""
    bank = bytearray()
    entries = []
    report = []
    for name, filename, cap in SFX_SLOTS:
        if filename is None:
            entries.append((0, 0))
            continue
        pcm = brr2.prepare(SFX_DIR / filename, max_seconds=cap)
        packed, decoded = brr2.encode(pcm)
        if not np.array_equal(brr2.decode(packed), decoded):
            raise RuntimeError(f"{name}: encoder and reference decoder disagree")
        blocks = len(packed) // brr2.PACKED_BLOCK_BYTES
        if blocks >= 0x10000:
            raise ValueError(f"{name}: too long for a 16-bit block count")
        entries.append((len(bank), blocks))
        bank += packed
        report.append(f"{name:15s} {len(pcm) / brr2.SAMPLE_RATE:5.2f}s "
                      f"{len(packed):6d} bytes  SNR {brr2.snr_db(pcm, decoded):5.1f} dB")
    return bytes(bank), entries, report


def sfx_layout(bank_size):
    """Fixed ARAM addresses: [rings][bank] directly under the DSP directory."""
    region = (DIRECTORY_ADDRESS - bank_size - SFX_VOICES * RING_BYTES) & ~0x0F
    rings = tuple(region + v * RING_BYTES for v in range(SFX_VOICES))
    bank = region + SFX_VOICES * RING_BYTES
    return region, rings, bank


# ── the cartridge runtime ────────────────────────────────────────────────────

def _emit_reload(code, poll_sfx_unused):
    """Song reload protocol: aligned 144-byte blocks, then restart."""
    code.label("reload_block")
    _load_a_dp(code, 0xF4)
    _store_a_dp(code, DP_RELOAD_TOKEN)
    _store_a_dp(code, DP_RELOAD_RX_TOKEN)
    _load_a_dp(code, 0xF6)
    _store_a_dp(code, DP_RELOAD_ADDR_LO)
    _load_a_dp(code, 0xF7)
    _store_a_dp(code, DP_RELOAD_ADDR_HI)
    _imm_dp(code, 0xF1, 0)            # stop timer while ARAM is replaced
    _imm_dp(code, 0xF2, 0x5C)         # S-DSP KOFF
    _imm_dp(code, 0xF3, 0xFF)         # key off all voices
    _imm_dp(code, 0xF2, 0x6C)
    _imm_dp(code, 0xF3, 0x60)         # mute and disable echo writes
    _load_a_dp(code, DP_RELOAD_TOKEN)
    _store_a_dp(code, 0xF4)           # command acknowledgement
    _imm_dp(code, DP_RELOAD_COUNT, 0)
    code.call("reload_receive")
    # Stay here with the timer stopped until ALL blocks have arrived. Never
    # interpret a sequence or play BRR samples while they are being replaced.
    code.label("reload_next_command")
    _load_a_dp(code, 0xF4)
    code.emit(0x64, DP_RELOAD_RX_TOKEN)
    code.branch(0xF0, "reload_next_command")
    _store_a_dp(code, DP_RELOAD_TOKEN)
    _load_a_dp(code, 0xF5)
    _cmp_imm(code, RELOAD_BLOCK)
    code.branch(0xF0, "reload_block")
    _cmp_imm(code, RELOAD_RESTART)
    code.branch(0xD0, "reload_next_command")

    code.label("reload_restart")
    _load_a_dp(code, 0xF6)
    _store_a_dp(code, DP_FINITE_PENDING)
    _load_a_dp(code, 0xF4)
    _store_a_dp(code, DP_RELOAD_TOKEN)
    _store_a_dp(code, 0xF4)
    # The reload call stack is intentionally discarded before entry restarts
    # the player.  This also makes repeated scene changes safe indefinitely.
    code.jump("cartridge_restart")

    code.label("reload_receive")
    code.label("reload_receive_wait")
    _load_a_dp(code, 0xF4)
    code.emit(0x64, DP_RELOAD_RX_TOKEN)
    code.branch(0xF0, "reload_receive_wait")
    _load_a_dp(code, 0xF4)
    _store_a_dp(code, DP_RELOAD_RX_TOKEN)
    _imm_y(code, 0)
    _load_a_dp(code, 0xF5)              # three data bytes from the CPU
    code.emit(0xD7, DP_RELOAD_ADDR_LO)  # MOV [addr]+Y,A
    code.emit(0x3A, DP_RELOAD_ADDR_LO)  # INCW addr
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
    _load_a_dp(code, DP_RELOAD_COUNT)
    _cmp_imm(code, RELOAD_BYTES)
    code.branch(0xF0, "reload_receive_done")
    code.branch(0x2F, "reload_receive_wait")  # BRA

    code.label("reload_receive_done")
    code.emit(0x6F)


def _emit_sfx_streamer(code, rings, bank_address, entries):
    """The 2-bit SFX streamer: trigger, per-tick pump, ring writer, expander."""
    voice_dp = [DP_VOICE + v * 8 for v in range(SFX_VOICES)]
    assert SFX_VOICES == 2 and voice_dp == [0xD0, 0xD8]

    # ── sfx_trigger: a play command sits in F5/F6/F7 ────────────────────────
    code.label("sfx_trigger")
    _load_a_dp(code, 0xF5)
    code.emit(0x28, 0x7F)                       # AND #$7F (preload bit is moot)
    _cmp_imm(code, len(entries))
    code.branch(0x90, "sfx_slot_in_range")      # BCC
    code.jump("sfx_ack")                        # slot out of range
    code.label("sfx_slot_in_range")
    code.emit(0x1C, 0x1C)                       # ASL A x2: slot * 4
    _store_a_dp(code, S_SLOT)
    code.emit(0x5D)                             # MOV X,A
    _load_a_absx(code, 0)                       # blocks lo (patched below)
    slot_tab_refs = [len(code.data) - 2]
    code.emit(0x05, 0, 0)                       # OR A,!abs (blocks hi)
    slot_tab_refs.append(len(code.data) - 2)
    code.branch(0xD0, "sfx_slot_playable")
    code.jump("sfx_ack")                        # empty slot
    code.label("sfx_slot_playable")
    # Voice choice: the one not used last if it is idle, else the last one
    # if idle, else steal the older (the one not used last).
    _load_a_dp(code, S_CURV)
    code.emit(0x48, 0x08)                       # EOR #8
    code.emit(0x5D)                             # MOV X,A
    _load_a_dpx(code, DP_VOICE + V_ACTIVE)
    code.branch(0xF0, "sfx_voice_chosen")
    code.emit(0xF8, S_CURV)                     # MOV X,dp
    _load_a_dpx(code, DP_VOICE + V_ACTIVE)
    code.branch(0xF0, "sfx_voice_chosen")
    _load_a_dp(code, S_CURV)
    code.emit(0x48, 0x08)
    code.emit(0x5D)
    code.label("sfx_voice_chosen")
    code.emit(0xD8, S_CURV)                     # MOV dp,X
    code.emit(0xD8, S_VX)
    # Stream state from the slot table.
    code.emit(0xEB, S_SLOT)                     # MOV Y,dp
    for field in range(4):
        _load_a_absy(code, 0)
        slot_tab_refs.append((len(code.data) - 2, field))
        _store_a_dpx(code, DP_VOICE + field)
    _imm_a(code, 0)
    _store_a_dpx(code, DP_VOICE + V_WPOS)
    _store_a_dpx(code, DP_VOICE + V_LEAD)
    _imm_a(code, 1)
    _store_a_dpx(code, DP_VOICE + V_ACTIVE)
    # Voice mask and DSP register base for voice 6 (X=0) or 7 (X=8).
    _imm_a(code, 1 << SFX_FIRST_VOICE)
    code.emit(0xC8, 0x00)                       # CMP X,#0
    code.branch(0xF0, "sfx_mask_ready")
    _imm_a(code, 1 << (SFX_FIRST_VOICE + 1))
    code.label("sfx_mask_ready")
    _store_a_dp(code, S_MASK)
    _imm_a(code, SFX_FIRST_VOICE << 4)
    code.emit(0xC8, 0x00)
    code.branch(0xF0, "sfx_base_ready")
    _imm_a(code, (SFX_FIRST_VOICE + 1) << 4)
    code.label("sfx_base_ready")
    _store_a_dp(code, midi2spc.DP_BASE)
    # Key the voice off (it may be mid-effect), fill the ring, program, key on.
    _imm_a(code, 0x5C)
    code.emit(0xEB, S_MASK)
    code.call("write_global")
    _imm_a(code, 0x5C)
    _imm_y(code, 0)
    code.call("write_global")
    code.call("sfx_fill")
    _load_a_dp(code, S_MASK)
    code.emit(0x48, 0xFF)                       # EOR #$FF
    code.emit(0x24, midi2spc.DP_NON)            # AND A,dp
    _store_a_dp(code, midi2spc.DP_NON)
    _load_a_dp(code, S_MASK)
    code.emit(0x48, 0xFF)
    code.emit(0x24, midi2spc.DP_EON)
    _store_a_dp(code, midi2spc.DP_EON)
    _imm_a(code, 0x4D)
    code.emit(0xEB, midi2spc.DP_EON)
    code.call("write_global")
    _load_a_dp(code, 0xF6)
    code.emit(0x28, 0x7F)
    code.emit(0xFD)                             # MOV Y,A
    _imm_a(code, 0x00)
    code.call("write_dynamic")                  # VOL(L)
    _load_a_dp(code, 0xF7)
    code.emit(0x28, 0x7F)
    code.emit(0xFD)
    _imm_a(code, 0x01)
    code.call("write_dynamic")                  # VOL(R)
    _imm_a(code, 0x02)
    _imm_y(code, SFX_PITCH & 0xFF)
    code.call("write_dynamic")
    _imm_a(code, 0x03)
    _imm_y(code, SFX_PITCH >> 8)
    code.call("write_dynamic")
    _imm_y(code, SFX_SOURCE_BASE)
    code.emit(0xF8, S_VX)                       # MOV X,dp
    code.emit(0xC8, 0x00)
    code.branch(0xF0, "sfx_srcn_ready")
    _imm_y(code, SFX_SOURCE_BASE + 1)
    code.label("sfx_srcn_ready")
    _imm_a(code, 0x04)
    code.call("write_dynamic")                  # SRCN = ring
    _imm_a(code, 0x05)
    _imm_y(code, 0x00)
    code.call("write_dynamic")                  # ADSR off: GAIN mode
    _imm_a(code, 0x06)
    _imm_y(code, 0x00)
    code.call("write_dynamic")
    _imm_a(code, 0x07)
    _imm_y(code, 0x7F)
    code.call("write_dynamic")                  # direct GAIN, full
    _imm_a(code, 0x4C)
    code.emit(0xEB, S_MASK)
    code.call("write_global")                   # KON
    code.label("sfx_ack")
    _load_a_dp(code, DP_RELOAD_TOKEN)
    _store_a_dp(code, 0xF4)
    code.emit(0x6F)

    # ── sfx_pump: once per sequencer tick, from the tick hook ───────────────
    code.label("sfx_pump")
    _load_a_dp(code, S_TICKS)
    _imm_y(code, BLOCKS_PER_TICK)
    code.emit(0xCF)                             # MUL YA
    _store_a_dp(code, S_CONSUMED)
    _imm_dp(code, S_TICKS, 0)
    _imm_x(code, 0)
    code.call("sfx_pump_voice")
    _imm_x(code, 8)
    code.call("sfx_pump_voice")
    code.emit(0x6F)

    code.label("sfx_pump_voice")
    _load_a_dpx(code, DP_VOICE + V_ACTIVE)
    code.branch(0xD0, "sfx_pump_active")
    code.emit(0x6F)
    code.label("sfx_pump_active")
    _load_a_dpx(code, DP_VOICE + V_REM_LO)
    code.emit(0x14, DP_VOICE + V_REM_HI)        # OR A,dp+X
    code.branch(0xD0, "sfx_pump_remaining")
    # Fully written: idle once the DSP has played past the END block.
    _load_a_dpx(code, DP_VOICE + V_LEAD)
    code.emit(0x80)                             # SETC
    code.emit(0xA4, S_CONSUMED)                 # SBC A,dp
    _store_a_dpx(code, DP_VOICE + V_LEAD)
    code.branch(0x30, "sfx_pump_drained")       # BMI
    code.emit(0x6F)
    code.label("sfx_pump_drained")
    _imm_a(code, 0)
    _store_a_dpx(code, DP_VOICE + V_ACTIVE)
    code.emit(0x6F)
    code.label("sfx_pump_remaining")
    _load_a_dpx(code, DP_VOICE + V_LEAD)
    code.emit(0x80)                             # SETC
    code.emit(0xA4, S_CONSUMED)                 # SBC A,dp
    code.branch(0x10, "sfx_lead_nonnegative")   # BPL
    _imm_a(code, 0)
    _store_a_dpx(code, DP_VOICE + V_LEAD)
    _load_a_dpx(code, DP_VOICE + V_UNDERRUN)
    _cmp_imm(code, 0xFF)
    code.branch(0xF0, "sfx_lead_measure")
    code.emit(0xBB, DP_VOICE + V_UNDERRUN)      # INC dp+X, saturating
    code.branch(0x2F, "sfx_lead_measure")
    code.label("sfx_lead_nonnegative")
    _store_a_dpx(code, DP_VOICE + V_LEAD)
    code.label("sfx_lead_measure")
    _load_a_dpx(code, DP_VOICE + V_LEAD)
    code.emit(0xC8, 0x00)                       # CMP X,#0
    code.branch(0xD0, "sfx_lead_voice1")
    code.emit(0x64, DP_MIN_LEAD0)               # CMP A,dp
    code.branch(0xB0, "sfx_pump_fill")          # BCS: no new minimum
    _store_a_dp(code, DP_MIN_LEAD0)
    code.branch(0x2F, "sfx_pump_fill")
    code.label("sfx_lead_voice1")
    code.emit(0x64, DP_MIN_LEAD1)
    code.branch(0xB0, "sfx_pump_fill")
    _store_a_dp(code, DP_MIN_LEAD1)
    code.label("sfx_pump_fill")
    code.emit(0xD8, S_VX)                       # MOV dp,X
    # fall into sfx_fill

    # ── sfx_fill: write blocks until the lead is back at target ─────────────
    code.label("sfx_fill")
    code.emit(0xF8, S_VX)                       # MOV X,dp
    _load_a_dpx(code, DP_VOICE + V_REM_LO)
    code.emit(0x14, DP_VOICE + V_REM_HI)
    code.branch(0xF0, "sfx_fill_done")
    _load_a_dpx(code, DP_VOICE + V_LEAD)
    _cmp_imm(code, LEAD_TARGET)
    code.branch(0x10, "sfx_fill_done")          # BPL: lead >= target
    code.call("sfx_write_block")
    code.branch(0x2F, "sfx_fill")
    code.label("sfx_fill_done")
    code.emit(0x6F)

    # ── sfx_write_block: one packed block -> ring[WPOS] ─────────────────────
    code.label("sfx_write_block")
    _load_a_dpx(code, DP_VOICE + V_SRC_LO)
    _store_a_dp(code, S_SRC)
    _load_a_dpx(code, DP_VOICE + V_SRC_HI)
    _store_a_dp(code, S_SRC + 1)
    _load_a_dpx(code, DP_VOICE + V_WPOS)
    _imm_y(code, brr2.BRR_BLOCK_BYTES)
    code.emit(0xCF)                             # MUL YA: A = wpos * 9
    code.emit(0x60)                             # CLRC
    code.emit(0x95, 0, 0)                       # ADC A,!abs+X (ring lo table)
    ring_tab_refs = [(len(code.data) - 2, 0)]
    _store_a_dp(code, S_DST)
    _imm_a(code, 0)
    code.emit(0x95, 0, 0)                       # ADC A,!abs+X (ring hi table)
    ring_tab_refs.append((len(code.data) - 2, 1))
    _store_a_dp(code, S_DST + 1)
    # REM -= 1; the last block gets END alone, ring block 23 END+LOOP.
    _load_a_dpx(code, DP_VOICE + V_REM_LO)
    code.emit(0x80)                             # SETC
    code.emit(0xA8, 0x01)                       # SBC A,#1
    _store_a_dpx(code, DP_VOICE + V_REM_LO)
    _load_a_dpx(code, DP_VOICE + V_REM_HI)
    code.emit(0xA8, 0x00)
    _store_a_dpx(code, DP_VOICE + V_REM_HI)
    _load_a_dpx(code, DP_VOICE + V_REM_LO)
    code.emit(0x14, DP_VOICE + V_REM_HI)
    code.branch(0xD0, "sfx_flags_not_last")
    _imm_dp(code, S_FLAGS, 0x01)
    code.branch(0x2F, "sfx_flags_ready")
    code.label("sfx_flags_not_last")
    _imm_dp(code, S_FLAGS, 0x00)
    _load_a_dpx(code, DP_VOICE + V_WPOS)
    _cmp_imm(code, RING_BLOCKS - 1)
    code.branch(0xD0, "sfx_flags_ready")
    _imm_dp(code, S_FLAGS, 0x03)
    code.label("sfx_flags_ready")
    code.call("sfx_expand_block")
    code.emit(0xF8, S_VX)                       # MOV X,dp
    _load_a_dp(code, S_SRC)
    _store_a_dpx(code, DP_VOICE + V_SRC_LO)
    _load_a_dp(code, S_SRC + 1)
    _store_a_dpx(code, DP_VOICE + V_SRC_HI)
    _load_a_dpx(code, DP_VOICE + V_WPOS)
    code.emit(0xBC)                             # INC A
    _cmp_imm(code, RING_BLOCKS)
    code.branch(0xD0, "sfx_wpos_ready")
    _imm_a(code, 0)
    code.label("sfx_wpos_ready")
    _store_a_dpx(code, DP_VOICE + V_WPOS)
    code.emit(0xBB, DP_VOICE + V_LEAD)          # INC dp+X
    code.emit(0x6F)

    # ── sfx_expand_block: 5 packed bytes at [S_SRC] -> 9 BRR bytes at [S_DST]
    # The header keeps its shift/filter and takes the ring flags; every packed
    # byte is two nibbles, each a pair of 2-bit codes that the direct-page
    # alphabet table turns into one BRR residual byte.  Clobbers X and Y.
    code.label("sfx_expand_block")
    _imm_y(code, 0)
    code.emit(0xF7, S_SRC)                      # MOV A,[dp]+Y
    _store_a_dp(code, S_TMP)
    code.emit(0x28, 0x01)                       # AND #1: alphabet
    code.emit(0x9F)                             # XCN: $00 / $10
    code.emit(0x60)                             # CLRC
    code.emit(0x88, DP_PAIR_TABLE)              # ADC #$B0 ($B0 has bit 4 set: no OR)
    _store_a_dp(code, S_TBL)
    _load_a_dp(code, S_TMP)
    code.emit(0x28, 0xFC)
    code.emit(0x04, S_FLAGS)                    # OR A,dp
    code.emit(0xD7, S_DST)                      # MOV [dp]+Y,A: header
    for k in range(1, 5):
        _imm_y(code, k)
        code.emit(0xF7, S_SRC)
        _store_a_dp(code, S_TMP)
        code.emit(0x9F)                         # XCN
        code.emit(0x28, 0x0F)
        code.emit(0x04, S_TBL)
        code.emit(0x5D)                         # MOV X,A
        code.emit(0xE6)                         # MOV A,(X)
        _imm_y(code, 2 * k - 1)
        code.emit(0xD7, S_DST)
        _load_a_dp(code, S_TMP)
        code.emit(0x28, 0x0F)
        code.emit(0x04, S_TBL)
        code.emit(0x5D)
        code.emit(0xE6)
        _imm_y(code, 2 * k)
        code.emit(0xD7, S_DST)
    _load_a_dp(code, S_SRC)
    code.emit(0x60)
    code.emit(0x88, brr2.PACKED_BLOCK_BYTES)    # ADC A,#5
    _store_a_dp(code, S_SRC)
    code.branch(0x90, "sfx_src_advanced")       # BCC
    _inc_dp(code, S_SRC + 1)
    code.label("sfx_src_advanced")
    _load_a_dp(code, S_DST)
    code.emit(0x60)
    code.emit(0x88, brr2.BRR_BLOCK_BYTES)
    _store_a_dp(code, S_DST)
    code.branch(0x90, "sfx_dst_advanced")
    _inc_dp(code, S_DST + 1)
    code.label("sfx_dst_advanced")
    code.emit(0x6F)

    # ── sfx_reset: both voices idle, alphabets in direct page ───────────────
    code.label("sfx_reset")
    _imm_dp(code, DP_VOICE + V_ACTIVE, 0)
    _imm_dp(code, DP_VOICE + 8 + V_ACTIVE, 0)
    _imm_dp(code, DP_VOICE + V_UNDERRUN, 0)
    _imm_dp(code, DP_VOICE + 8 + V_UNDERRUN, 0)
    _imm_dp(code, DP_MIN_LEAD0, 0xFF)
    _imm_dp(code, DP_MIN_LEAD1, 0xFF)
    _imm_dp(code, S_TICKS, 0)
    _imm_dp(code, S_CURV, 0)
    _imm_dp(code, S_LATE_LO, 0)
    _imm_dp(code, S_LATE_HI, 0)
    _imm_dp(code, S_WORST, 0)
    _imm_x(code, 31)
    code.label("sfx_reset_table")
    _load_a_absx(code, 0)
    pair_tab_ref = len(code.data) - 2
    _store_a_dpx(code, DP_PAIR_TABLE)
    code.emit(0x1D)                             # DEC X
    code.branch(0x10, "sfx_reset_table")        # BPL
    code.emit(0x6F)

    # ── data: slot table, ring address tables, alphabet pairs ───────────────
    code.label("sfx_slot_table")
    for offset, blocks in entries:
        address = bank_address + offset if blocks else 0
        code.emit(address & 0xFF, address >> 8, blocks & 0xFF, blocks >> 8)
    code.label("sfx_ring_lo")
    lo = [0] * 9
    hi = [0] * 9
    for v, ring in enumerate(rings):
        lo[v * 8] = ring & 0xFF
        hi[v * 8] = ring >> 8
    code.emit(*lo)
    code.label("sfx_ring_hi")
    code.emit(*hi)
    code.label("sfx_pair_table")
    code.emit(*brr2.pair_table_bytes())

    # Fix up the absolute data references now the labels exist.
    def patch(position, address):
        code.data[position] = address & 0xFF
        code.data[position + 1] = address >> 8

    slot_table = code.labels["sfx_slot_table"]
    patch(slot_tab_refs[0], slot_table + 2)
    patch(slot_tab_refs[1], slot_table + 3)
    for position, field in slot_tab_refs[2:]:
        patch(position, slot_table + field)
    patch(ring_tab_refs[0][0], code.labels["sfx_ring_lo"])
    patch(ring_tab_refs[1][0], code.labels["sfx_ring_hi"])
    patch(pair_tab_ref, code.labels["sfx_pair_table"])


def _build_runtime(original_labels, original_length, rings, bank_address, entries):
    """Return the cartridge runtime appended to the supplied player build."""
    runtime_address = PROGRAM_ADDRESS + original_length
    code = midi2spc.Code(runtime_address)
    # The converter's own routines the runtime calls into.
    for name in ("write_global", "write_dynamic", "timer_ready", "finish_loop"):
        code.labels[name] = original_labels[name]

    # Snapshot players restore direct page from the SPC file; the cartridge
    # IPL upload starts at $0200 instead. Recreate the required state both on
    # boot and restart, without clearing the CPU's incoming command latch.
    code.label("cartridge_entry")
    _imm_dp(code, DP_RELOAD_TOKEN, 0)
    _imm_dp(code, DP_FINITE, 0)
    _imm_dp(code, DP_FINITE_PENDING, 0)
    code.label("cartridge_restart")
    code.emit(0x20)                   # CLRP: direct page is $0000
    code.emit(0xCD, 0xFF, 0xBD)       # reset stack (discard reload calls)
    _imm_dp(code, 0x20, midi2spc.EVENT_ADDRESS & 0xFF)
    _imm_dp(code, 0x21, midi2spc.EVENT_ADDRESS >> 8)
    for n in range(8):
        _imm_dp(code, midi2spc.DP_MASK_TABLE + n, 1 << n)
    code.call("sfx_reset")
    # Replay the first MOV dp,#0 displaced by the entry JMP.
    _imm_dp(code, midi2spc.DP_DELAY_LO, 0)
    _load_a_dp(code, DP_FINITE_PENDING)
    _store_a_dp(code, DP_FINITE)
    code.emit(0x5F, (PROGRAM_ADDRESS + 3) & 0xFF,
              (PROGRAM_ADDRESS + 3) >> 8)

    # Every sequencer tick, after the music: feed the SFX rings, then look at
    # the command latch.  Save the accepted token BEFORE any acknowledgement:
    # reading F4 again after acknowledging can consume the CPU's next command
    # without handling it.
    code.label("audio_tick_wrapper")
    code.call("sfx_pump")
    _load_a_dp(code, 0xF4)
    code.emit(0x64, DP_RELOAD_TOKEN)
    code.branch(0xF0, "reload_poll_done")
    _store_a_dp(code, DP_RELOAD_TOKEN)
    _load_a_dp(code, 0xF5)
    _cmp_imm(code, RELOAD_BLOCK)
    code.branch(0xD0, "audio_tick_not_block")
    code.jump("reload_block")
    code.label("audio_tick_not_block")
    _cmp_imm(code, RELOAD_RESTART)
    code.branch(0xD0, "audio_tick_sfx")
    code.jump("reload_restart")
    code.label("audio_tick_sfx")
    code.jump("sfx_trigger")
    code.label("reload_poll_done")
    code.emit(0x6F)

    # The player's timer read.  Every fired tick is counted here so the SFX
    # pump knows how many the DSP consumed even when the sequencer merges
    # late ticks into one.  Called in place of MOV A,$FD / BNE timer_ready.
    code.label("timer_read")
    _load_a_dp(code, 0xFD)
    code.branch(0xF0, "timer_read_idle")
    code.call("timer_account")
    code.emit(0xAE, 0xAE)             # POP A x2: drop the return address
    code.jump("timer_ready")
    code.label("timer_read_idle")
    code.emit(0x6F)

    # A is a non-zero timer backlog. Preserve elapsed ticks for the SFX pump,
    # and keep diagnostics that cannot silently wrap during a soak test.
    code.label("timer_account")
    _store_a_dp(code, S_TMP)
    _load_a_dp(code, S_WORST)
    code.emit(0x64, S_TMP)                       # CMP A,dp
    code.branch(0xB0, "timer_worst_ready")       # BCS: worst >= current
    _load_a_dp(code, S_TMP)
    _store_a_dp(code, S_WORST)
    code.label("timer_worst_ready")
    _load_a_dp(code, S_TMP)
    _cmp_imm(code, 2)
    code.branch(0x90, "timer_read_on_time")     # BCC: one tick, as it should be
    _inc_dp(code, S_LATE_LO)
    _load_a_dp(code, S_LATE_LO)
    code.branch(0xD0, "timer_read_on_time")
    _inc_dp(code, S_LATE_HI)
    _load_a_dp(code, S_LATE_HI)
    code.branch(0xD0, "timer_read_on_time")
    _imm_dp(code, S_LATE_LO, 0xFF)
    _imm_dp(code, S_LATE_HI, 0xFF)
    code.label("timer_read_on_time")
    _load_a_dp(code, S_TMP)
    code.emit(0x60)
    code.emit(0x84, S_TICKS)          # ADC A,dp
    _store_a_dp(code, S_TICKS)
    code.emit(0x6F)

    # A finite cue used to STOP the SPC700 after its last release, which also
    # stopped the SFX and the reload protocol.  Idle on the timer instead.
    code.label("finite_finish")
    code.branch(0xD0, "finite_idle")            # A = DP_FINITE
    code.jump("finish_loop")
    code.label("finite_idle")
    _load_a_dp(code, 0xFD)
    code.branch(0xF0, "finite_idle")
    code.call("timer_account")
    code.call("audio_tick_wrapper")
    code.branch(0x2F, "finite_idle")

    _emit_reload(code, None)
    _emit_sfx_streamer(code, rings, bank_address, entries)

    runtime = bytearray(code.finalize())
    if len(runtime) + runtime_address >= PITCH_LOW_TABLE:
        raise ValueError(
            f"cartridge runtime ends at ${runtime_address + len(runtime):04X}, "
            f"past the pitch table at ${PITCH_LOW_TABLE:04X}")
    return runtime, runtime_address, code.labels


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


def _expect(ram, address, expected, what):
    if bytes(ram[address:address + len(expected)]) != bytes(expected):
        raise ValueError(f"MIDI2SPC {what} no longer looks as expected at ${address:04X}")


def _patch_runtime(output_path, captured, rings, bank_address, bank, entries):
    data = bytearray(output_path.read_bytes())
    if len(data) != midi2spc.SPC_SIZE:
        raise ValueError(f"unexpected SPC size for {output_path}: {len(data)}")
    ram = data[SPC_HEADER_SIZE:SPC_HEADER_SIZE + SPC_RAM_SIZE]
    original_length = captured["length"]
    labels = captured["labels"]

    runtime, runtime_address, rt_labels = _build_runtime(
        labels, original_length, rings, bank_address, entries)
    ram[runtime_address:runtime_address + len(runtime)] = runtime

    # The SPC snapshot initializes finite mode to zero on cold boot. Restarts
    # enter below cartridge_entry, so remove its redundant initializer and let
    # the mode received in F6 survive.
    finite_pos = rt_labels["cartridge_entry"] + 3
    _expect(ram, finite_pos, (0x8F, 0x00, DP_FINITE), "finite-mode initializer")
    ram[finite_pos:finite_pos + 3] = bytes(3)

    # This local converter revision names zero as looping and nonzero as
    # finite, but its two conditional branches were emitted backwards.  The
    # event_end branch is corrected in place; the finish one is replaced
    # whole so a finite cue idles on the timer instead of executing STOP.
    start, stop = labels["event_end"], labels["event_loop"]
    branch = ram.find(bytes((0xE4, DP_FINITE, 0xD0)), start, stop)
    if branch < 0:
        raise ValueError("MIDI2SPC lost the event_end finite-mode branch")
    ram[branch + 2] = 0xF0
    start, stop = labels["finish"], labels["finish_loop"]
    branch = ram.find(bytes((0xE4, DP_FINITE, 0xD0)), start, stop)
    if branch < 0 or ram[branch + 4] != 0xFF:
        raise ValueError("MIDI2SPC lost the finish finite-mode branch/STOP")
    ff = rt_labels["finite_finish"]
    ram[branch + 2:branch + 5] = bytes((0x5F, ff & 0xFF, ff >> 8))

    # The tick hook is CALL poll_sfx; redirect its operand to the wrapper.
    tick = labels["tick_done"]
    _expect(ram, tick, (0x3F,), "tick hook")
    wrapper = rt_labels["audio_tick_wrapper"]
    ram[tick + 1], ram[tick + 2] = wrapper & 0xFF, wrapper >> 8

    # The timer read: MOV A,$FD / BNE timer_ready -> CALL timer_read / NOP.
    wait = labels["wait_timer"]
    _expect(ram, wait, (0xE4, 0xFD, 0xD0), "timer read")
    tr = rt_labels["timer_read"]
    ram[wait:wait + 4] = bytes((0x3F, tr & 0xFF, tr >> 8, 0x00))

    # The entry: MOV delay,#0 -> JMP cartridge_entry (the MOV is replayed).
    _expect(ram, PROGRAM_ADDRESS, (0x8F, 0, midi2spc.DP_DELAY_LO), "entry")
    ram[PROGRAM_ADDRESS:PROGRAM_ADDRESS + 3] = bytes(
        (0x5F, runtime_address & 0xFF, runtime_address >> 8))

    # The SFX region: rings (silence, END+LOOP on the last block), the bank,
    # and the two ring sources in the DSP directory.
    for ring in rings:
        ram[ring:ring + RING_BYTES] = bytes(RING_BYTES)
        ram[ring + RING_BYTES - 9] = 0x03
    ram[bank_address:bank_address + len(bank)] = bank
    for v, ring in enumerate(rings):
        entry = DIRECTORY_ADDRESS + (SFX_SOURCE_BASE + v) * 4
        struct.pack_into("<HH", ram, entry, ring, ring)

    data[SPC_HEADER_SIZE:SPC_HEADER_SIZE + SPC_RAM_SIZE] = ram
    output_path.write_bytes(data)
    return runtime_address, len(runtime)


def _write_layout(layouts, region, bank_address, bank_size, layout_dir):
    layout_dir.mkdir(parents=True, exist_ok=True)
    path = layout_dir / "snes_audio_layout.h"
    lines = [
        "/* Generated by tools/snes/gen_snes_audio.py. */",
        "#ifndef WAIFU_SNES_AUDIO_LAYOUT_H",
        "#define WAIFU_SNES_AUDIO_LAYOUT_H",
        "",
        f"#define SNES_AUDIO_MUSIC_START 0x{PITCH_LOW_TABLE:04X}u",
        f"#define SNES_AUDIO_RELOAD_BYTES {RELOAD_BYTES}u",
        f"#define SNES_AUDIO_SFX_REGION 0x{region:04X}u",
        f"#define SNES_AUDIO_SFX_BANK 0x{bank_address:04X}u",
        f"#define SNES_AUDIO_SFX_BANK_BYTES {bank_size}u",
        f"#define SNES_AUDIO_SFX_SLOTS {len(SFX_SLOTS)}u",
    ]
    for name, music_end in layouts:
        lines.append(f"#define SNES_AUDIO_{name.upper()}_END 0x{music_end:04X}u")
    lines += ["", "#endif /* WAIFU_SNES_AUDIO_LAYOUT_H */", ""]
    path.write_text("\n".join(lines), encoding="ascii")
    inc = layout_dir / "snes_audio_layout.inc"
    inc.write_text(
        "; Generated by tools/snes/gen_snes_audio.py.\n"
        f".DEFINE SNES_AUDIO_SFX_COUNT {len(SFX_SLOTS)}\n"
        f".DEFINE SNES_AUDIO_RELOAD_BYTES {RELOAD_BYTES}\n", encoding="ascii")


def build(output_dir, layout_dir=None):
    output_dir.mkdir(parents=True, exist_ok=True)
    if layout_dir is None:
        layout_dir = REPO / "src" / "snes"
    validate_sfx_contract()
    bank, entries, report = build_sfx_bank()
    region, rings, bank_address = sfx_layout(len(bank))
    print("\n".join(report))
    print(f"SFX bank {len(bank)} bytes at ${bank_address:04X}, rings at "
          + ", ".join(f"${r:04X}" for r in rings)
          + f"; music budget ${PITCH_LOW_TABLE:04X}-${region:04X} "
          f"({region - midi2spc.EVENT_ADDRESS} bytes after the tables)")
    # Everything under the SFX region is the music's.  One dummy direct SFX
    # keeps the converter reserving voices 6/7 and emitting the tick hook the
    # runtime replaces; its bank and directory entry are overwritten.
    midi2spc.ARAM_BUDGETS["game"] = region
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
        "sfx_samples": (REPO / "sounds" / "Select.wav",),
        "sfx_voices": SFX_VOICES,
        "sfx_compression": "off",
        "music_compression": "auto-lossy",
        "brr2_policy": "adaptive",
        "nesdev_profile": "auto",
        "aram_budget": "game",
    }
    layouts = []
    song_reports = []
    for name, midi_name in TRACKS:
        midi_path = REPO / "snes_music" / "midi" / midi_name
        output_path = output_dir / f"{name}.spc"
        stats, captured = _render_with_labels(midi_path, output_path, **common)
        music_end = int(stats["music_aram_end"], 16)
        if music_end > region:
            raise ValueError(f"{name}: music ends at ${music_end:04X}, "
                             f"inside the SFX region at ${region:04X}")
        runtime_address, runtime_size = _patch_runtime(
            output_path, captured, rings, bank_address, bank, entries)
        layouts.append((name, music_end))
        song_reports.append({
            "track": name,
            "music_start": PITCH_LOW_TABLE,
            "music_end": music_end,
            "music_bytes": music_end - PITCH_LOW_TABLE,
            "music_codec": stats.get("music_compression", "unknown"),
            "music_paging_codecs": stats.get("music_paging_codecs", ""),
            "music_paging_packed_bytes": stats.get("music_paging_packed_bytes", 0),
            "sample_quality_tier": stats.get("sample_quality_tier", "unknown"),
            "runtime_address": runtime_address,
            "runtime_bytes": runtime_size,
            "free_before_sfx": region - music_end,
        })
        print(f"{name}: music end {stats['music_aram_end']} "
              f"({stats.get('sample_quality_tier', '?')}), "
              f"runtime ${runtime_address:04X}+{runtime_size}")
    _write_layout(layouts, region, bank_address, len(bank), layout_dir)
    memory_report = {
        "format": 1,
        "aram_bytes": SPC_RAM_SIZE,
        "resident_player_start": PROGRAM_ADDRESS,
        "music_start": PITCH_LOW_TABLE,
        "reload_block_bytes": RELOAD_BYTES,
        "sfx_codec": "2-bit residual, 5 bytes per 16 samples",
        "sfx_sample_rate": brr2.SAMPLE_RATE,
        "sfx_voices": SFX_VOICES,
        "rings": [{"address": address, "bytes": RING_BYTES}
                  for address in rings],
        "sfx_bank": {"address": bank_address, "bytes": len(bank)},
        "dsp_directory": {"address": DIRECTORY_ADDRESS, "bytes": 0x100},
        "upper_free": {"address": DIRECTORY_ADDRESS + 0x100,
                       "bytes": SPC_RAM_SIZE - DIRECTORY_ADDRESS - 0x100},
        "tracks": song_reports,
        "slots": [{"slot": index, "name": spec[0], "source": spec[1],
                   "cap_seconds": spec[2], "packed_offset": entries[index][0],
                   "blocks": entries[index][1]}
                  for index, spec in enumerate(SFX_SLOTS)],
    }
    (output_dir / "audio-layout-report.json").write_text(
        json.dumps(memory_report, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--output-dir", type=Path,
        default=REPO / "src" / "snes" / "assets" / "audio",
        help="directory for generated SPC snapshots")
    parser.add_argument(
        "--layout-dir", type=Path,
        default=REPO / "src" / "snes",
        help="directory for the generated C and assembler layout files")
    args = parser.parse_args(argv)
    build(args.output_dir.resolve(), args.layout_dir.resolve())


if __name__ == "__main__":
    main()
