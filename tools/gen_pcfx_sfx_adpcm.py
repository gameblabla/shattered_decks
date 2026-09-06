#!/usr/bin/env python3
"""Generate PC-FX ADPCM SFX bank.

The PC-FX ADPCM decoder used by KING/SoundBox consumes 4-bit nibbles from
16-bit KRAM halfwords in nibble order 0,4,8,12.  This encoder is deliberately
simple and deterministic: it resamples each WAV to 16 kHz, then greedily
chooses the nibble that minimizes predictor error using the same step
table/index deltas as pcfxemu's SoundBox_ADPCMUpdate().

Accepted source format: uncompressed PCM WAV, 8- or 16-bit, mono or stereo,
any sample rate (the classic unsigned 8-bit mono 44100 Hz assets keep their
exact legacy encoding path).  Stereo is downmixed to mono by averaging
channels before resampling, matching tools/gen_sound_assets.py.
"""
from __future__ import annotations
import math
import os
import struct
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOUND_DIRS = [ROOT / "sounds", ROOT / "assets" / "sounds"]
OUT_BIN = ROOT / "assets" / "generated" / "sfx_adpcm.bin"
OUT_H = ROOT / "src" / "generated" / "pcfx_sfx_adpcm.h"

SRC_RATE = 44100
DST_RATE = 16000
SILENCE_TAIL = DST_RATE // 100  # appended zeros; excluded from loudness stats

# Loudness: the legacy 8-bit masters were mastered hot (old Confirm RMS
# ~3400 in s14 units) while newer 16-bit recordings peak near full scale but
# carry ~6 dB less RMS (high crest factor).  A bare peak-normalize would only
# buy +1..3 dB on those, so quiet SFX get a bounded makeup gain through a
# soft-knee limiter that asymptotically approaches the rails: the ADPCM
# predictor range is never exceeded, so nothing clips on the ADPCM side.
TARGET_RMS = 3400.0  # matches the legacy hot masters (old Confirm RMS)
GAIN_CAP = 2.0  # +6 dB max: covers the measured deficit, won't blast 8-bit hiss
LIMIT_KNEE = 8600.0
LIMIT_RAIL = float(0x2fff)  # 12287, matches the resampler clamp
CEIL_PEAK = 12000  # at/above this the file already uses the full swing
ALIGN_WORDS = 256
ALIGN_BYTES = ALIGN_WORDS * 2
# RAINBOW stills and the 8bpp/16M frame surfaces also live in KING KRAM page 1.
# Keep ADPCM above those regions and aligned to the hardware start register's
# 256-word granularity so video/RAINBOW reloads cannot overwrite resident SFX.
KRAM_BASE_WORD = 0x20000

# Must match WaifuSoundEffect enum order in src/game/sounds.h.
SOUNDS = [
    ("SELECT", "Select.wav", 44),
    ("CONFIRM", "Confirm.wav", 54),
    ("CONFIRM_ALT", "ConfirmAlt.wav", 48),
    ("CARD_PLACED", "CardPlaced.wav", 54),
    ("CARD_DESTROYED", "CardDestroyed.wav", 58),
    ("TURN_PASSED", "TurnPassed.wav", 50),
    ("YOU_LOST", None, 0),
    ("LASER_SHOOT", "SlashAttack.wav", 56),
    ("DIRECT_HIT", "SlashAttack.wav", 56),
    ("CARD_DRAWN", "CardDrawn.wav", 54),
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


def read_wav_mono(path: Path) -> tuple[list[int], int, int]:
    """Decode a PCM WAV to mono samples.

    Returns (mono, src_rate, sampwidth) where mono holds unsigned 8-bit
    values (0..255) for 8-bit sources or signed 16-bit values
    (-32768..32767) for 16-bit sources.  Stereo is downmixed by averaging
    channels (sum // channels), matching tools/gen_sound_assets.py.
    """
    with wave.open(str(path), "rb") as w:
        channels = w.getnchannels()
        sampwidth = w.getsampwidth()
        rate = w.getframerate()
        comptype = w.getcomptype()
        nframes = w.getnframes()
        if comptype != "NONE" or channels not in (1, 2) or sampwidth not in (1, 2) or rate <= 0:
            raise SystemExit(
                f"{path}: expected uncompressed 8/16-bit mono/stereo WAV "
                f"(got channels={channels} width={sampwidth} rate={rate} comptype={comptype})"
            )
        raw = w.readframes(nframes)
    if sampwidth == 1:
        if channels == 1:
            mono = list(raw)
        else:
            mono = [(raw[i] + raw[i + 1]) // 2 for i in range(0, len(raw) - 1, 2)]
        return mono, rate, 1
    count = len(raw) // 2
    samples = list(struct.unpack("<%dh" % count, raw[: count * 2])) if count else []
    if channels == 1:
        return samples, rate, 2
    return [(samples[i] + samples[i + 1]) // 2 for i in range(0, len(samples) - 1, 2)], rate, 2


def resample_u8_to_s14(src: bytes | list[int], src_rate: int = SRC_RATE,
                       dst_rate: int = DST_RATE) -> list[int]:
    # Linear interpolation.  Output matches the PC-FX ADPCM predictor range
    # (-0x4000..0x3fff) but stays below hard rails to reduce clicks.
    if not src:
        return []
    out_len = max(1, int(round(len(src) * dst_rate / src_rate)))
    out: list[int] = []
    for i in range(out_len):
        pos_num = i * src_rate
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


def resample_s16_to_s14(src: list[int], src_rate: int = SRC_RATE,
                        dst_rate: int = DST_RATE) -> list[int]:
    # Linear interpolation in the signed 16-bit domain, then scale to about
    # +/-0x3000 (s16 * 96 / 256 == s16 * 0.375).  Rounds to nearest so full
    # scale +/-32768 lands on the +/-0x3000 rails instead of clipping by a
    # floor-division bias for negative values.
    if not src:
        return []
    out_len = max(1, int(round(len(src) * dst_rate / src_rate)))
    out: list[int] = []
    for i in range(out_len):
        pos_num = i * src_rate
        ip = pos_num // dst_rate
        frac = pos_num % dst_rate
        if ip >= len(src) - 1:
            interp = src[-1]
        else:
            a = src[ip]
            b = src[ip + 1]
            interp = (a * (dst_rate - frac) + b * frac + dst_rate // 2) // dst_rate
        out.append(clamp(int(round(interp * 96 / 256)), -0x3000, 0x2fff))
    out.extend([0] * (dst_rate // 100))
    return out


def read_and_resample(path: Path) -> list[int]:
    mono, src_rate, sampwidth = read_wav_mono(path)
    if sampwidth == 1:
        return resample_u8_to_s14(mono, src_rate)
    return resample_s16_to_s14(mono, src_rate)


def apply_makeup_limit(samples: list[int]) -> list[int]:
    """Bounded makeup gain + soft-knee limiter (no ADPCM-side clipping).

    Returns the input unchanged (bit-identical) when the SFX already uses
    the full swing or already meets the target loudness; otherwise applies
    up to GAIN_CAP through a limiter that keeps every sample within the
    resampler rails.  Stats are measured on the body, excluding the
    appended silence tail (gain leaves those zeros at zero anyway).
    """
    if not samples:
        return samples
    body = samples[:-SILENCE_TAIL] if len(samples) > SILENCE_TAIL else samples
    peak = max(abs(v) for v in body) if body else 0
    if peak >= CEIL_PEAK:
        return samples  # already mastered to the ceiling; more gain = distortion
    rms = (sum(v * v for v in body) / len(body)) ** 0.5 if body else 0.0
    if rms <= 0:
        return samples
    gain = min(TARGET_RMS / rms, GAIN_CAP)
    if gain <= 1.0:
        return samples
    span = LIMIT_RAIL - LIMIT_KNEE
    out: list[int] = []
    for v in samples:
        x = v * gain
        ax = abs(x)
        if ax <= LIMIT_KNEE:
            y = x
        else:
            y = math.copysign(LIMIT_KNEE + span * math.tanh((ax - LIMIT_KNEE) / span), x)
        out.append(clamp(int(round(y)), -0x3000, 0x2fff))
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
        samples = apply_makeup_limit(read_and_resample(find_sound_file(filename)))
        adpcm = encode_adpcm(samples)
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
        f.write(f"#define WAIFU_PCFX_SFX_ADPCM_KRAM_BASE_WORD 0x{KRAM_BASE_WORD:04X}u\n")
        f.write(f"#define WAIFU_PCFX_SFX_ADPCM_META_COUNT {len(SOUNDS)}u\n\n")
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
