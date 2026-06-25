#!/usr/bin/env python3
"""Generate PC-FX ADPCM SFX bank.

The PC-FX ADPCM decoder used by KING/SoundBox consumes 4-bit nibbles from
16-bit KRAM halfwords in nibble order 0,4,8,12.  This encoder is deliberately
simple and deterministic: it resamples each unsigned 8-bit mono WAV to 16 kHz,
then greedily chooses the nibble that minimizes predictor error using the same
step table/index deltas as pcfxemu's SoundBox_ADPCMUpdate().
"""
from __future__ import annotations
import os
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOUND_DIRS = [ROOT / "sounds", ROOT / "assets" / "sounds"]
OUT_BIN = ROOT / "assets" / "generated" / "sfx_adpcm.bin"
OUT_H = ROOT / "src" / "generated" / "pcfx_sfx_adpcm.h"

SRC_RATE = 44100
DST_RATE = 16000
ALIGN_WORDS = 256
ALIGN_BYTES = ALIGN_WORDS * 2

# Must match WaifuSoundEffect enum order.  YOU_LOST is intentionally omitted:
# result-loss audio is CD-DA, not an in-game SFX sample.
SOUNDS = [
    ("SELECT", "Select.wav", 44),
    ("CONFIRM", "Confirm.wav", 54),
    ("CONFIRM_ALT", "ConfirmAlt.wav", 48),
    ("CARD_PLACED", "CardPlaced.wav", 54),
    ("CARD_DESTROYED", "CardDestroyed.wav", 58),
    ("TURN_PASSED", "TurnPassed.wav", 50),
    ("YOU_LOST", None, 0),
    ("LASER_SHOOT", "laserShoot.wav", 56),
]

STEP_SIZES = [
    16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50,
    55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157,
    173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449,
    494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552,
]
STEP_DELTAS = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]


def clamp(v: int, lo: int, hi: int) -> int:
    return lo if v < lo else hi if v > hi else v


def find_sound_file(name: str) -> Path:
    for d in SOUND_DIRS:
        p = d / name
        if p.exists():
            return p
    raise FileNotFoundError(name)


def read_u8_mono_wav(path: Path) -> bytes:
    with wave.open(str(path), "rb") as w:
        if w.getnchannels() != 1 or w.getsampwidth() != 1 or w.getframerate() != SRC_RATE:
            raise SystemExit(f"{path}: expected unsigned 8-bit mono {SRC_RATE} Hz WAV")
        return w.readframes(w.getnframes())


def resample_u8_to_s14(src: bytes, dst_rate: int = DST_RATE) -> list[int]:
    # Linear interpolation.  Output matches the PC-FX ADPCM predictor range
    # (-0x4000..0x3fff) but stays below hard rails to reduce clicks.
    if not src:
        return []
    out_len = max(1, int(round(len(src) * dst_rate / SRC_RATE)))
    out: list[int] = []
    for i in range(out_len):
        pos_num = i * SRC_RATE
        ip = pos_num // dst_rate
        frac = pos_num % dst_rate
        if ip >= len(src) - 1:
            sample = src[-1]
        else:
            a = src[ip]
            b = src[ip + 1]
            sample = (a * (dst_rate - frac) + b * frac + dst_rate // 2) // dst_rate
        # unsigned 8-bit center 128 -> signed; scale to about +/-0x3000
        out.append(clamp((int(sample) - 128) * 96, -0x3000, 0x2fff))
    # Add a tiny silence tail so one-shot playback ends cleanly.
    out.extend([0] * (dst_rate // 100))
    return out


def encode_adpcm(samples: list[int]) -> bytes:
    pred = 0
    idx = 0
    nibbles: list[int] = []
    for target in samples:
        step = STEP_SIZES[idx]
        best_nib = 0
        best_err = 1 << 62
        best_pred = pred
        best_idx = idx
        for mag in range(8):
            delta = step * (mag + 1)
            for sign in (0, 8):
                cand = pred - delta if sign else pred + delta
                cand = clamp(cand, -0x4000, 0x3fff)
                err = target - cand
                err = err * err
                if err < best_err:
                    nib = mag | sign
                    ni = clamp(idx + STEP_DELTAS[nib], 0, 48)
                    best_err = err
                    best_nib = nib
                    best_pred = cand
                    best_idx = ni
        nibbles.append(best_nib)
        pred = best_pred
        idx = best_idx
    while len(nibbles) % 4:
        nibbles.append(0)
    out = bytearray()
    for i in range(0, len(nibbles), 4):
        hw = (nibbles[i] & 15) | ((nibbles[i+1] & 15) << 4) | ((nibbles[i+2] & 15) << 8) | ((nibbles[i+3] & 15) << 12)
        out.append(hw & 0xff)
        out.append((hw >> 8) & 0xff)
    return bytes(out)


def main() -> int:
    OUT_BIN.parent.mkdir(parents=True, exist_ok=True)
    OUT_H.parent.mkdir(parents=True, exist_ok=True)
    bank = bytearray()
    metas = []
    for enum_index, (name, filename, volume) in enumerate(SOUNDS):
        if filename is None:
            metas.append((name, enum_index, 0, 0, volume))
            continue
        while len(bank) % ALIGN_BYTES:
            bank.append(0)
        start_word = len(bank) // 2
        src = read_u8_mono_wav(find_sound_file(filename))
        adpcm = encode_adpcm(resample_u8_to_s14(src))
        bank.extend(adpcm)
        word_count = len(adpcm) // 2
        metas.append((name, enum_index, start_word, word_count, volume))
    while len(bank) % 2048:
        bank.append(0)
    OUT_BIN.write_bytes(bank)
    with OUT_H.open("w", newline="\n") as f:
        f.write("/* Generated by tools/gen_pcfx_sfx_adpcm.py. */\n")
        f.write("#ifndef WAIFU_PCFX_SFX_ADPCM_H\n#define WAIFU_PCFX_SFX_ADPCM_H\n\n")
        f.write("#include <stdint.h>\n\n")
        f.write(f"#define WAIFU_PCFX_SFX_ADPCM_RATE {DST_RATE}\n")
        f.write(f"#define WAIFU_PCFX_SFX_ADPCM_BANK_BYTES {len(bank)}u\n")
        f.write(f"#define WAIFU_PCFX_SFX_ADPCM_BANK_WORDS {len(bank)//2}u\n")
        f.write("#define WAIFU_PCFX_SFX_ADPCM_KRAM_BASE_WORD 0u\n\n")
        f.write("typedef struct WaifuPcfxSfxAdpcmMeta {\n")
        f.write("    uint32_t start_word;\n")
        f.write("    uint32_t word_count;\n")
        f.write("    uint8_t volume;\n")
        f.write("} WaifuPcfxSfxAdpcmMeta;\n\n")
        f.write("static const WaifuPcfxSfxAdpcmMeta waifu_pcfx_sfx_adpcm_meta[] = {\n")
        for name, enum_index, start_word, word_count, volume in metas:
            f.write(f"    /* {enum_index}: {name} */ {{ {start_word}u, {word_count}u, {volume}u }},\n")
        f.write("};\n\n")
        f.write("#endif /* WAIFU_PCFX_SFX_ADPCM_H */\n")
    print(f"wrote {OUT_BIN} ({len(bank)} bytes), {OUT_H}")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
