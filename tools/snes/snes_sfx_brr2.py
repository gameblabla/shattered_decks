#!/usr/bin/env python3
"""2-bit BRR ("BRR2-5") encoder for the SNES one-shot SFX bank.

The S-DSP only plays ordinary 9-byte BRR blocks, so a 2-bit stream cannot be
handed to it directly: the SPC700 expands each packed block into a small ring
of real BRR blocks that the voice loops over while the CPU keeps writing ahead
(tools/snes/gen_snes_audio.py).  The packed format is deliberately the
simplest thing that decoder can expand quickly:

    byte 0      shift << 4 | filter << 2 | table          (a BRR header with
                                                           the 2-bit alphabet
                                                           in the flag bits)
    bytes 1..4  four 2-bit residual codes each, MSB first (samples 0..15)

Five bytes per 16 samples, 2.5 bits a sample, against BRR's 4.5.  Each code
selects one of four residual levels from one of two alphabets; the SPC700
maps a nibble (two codes) to one BRR residual byte through a 16-entry table,
so the expansion is a lookup per two samples.

Encoding is a greedy nearest-level walk with exact S-DSP predictor feedback,
tried over every filter, both alphabets and a window of shifts; the lowest
squared error wins.  The decoder here is the reference the generator checks
its own packed streams against.
"""

from __future__ import annotations

import math
import wave
from pathlib import Path

import numpy as np

SAMPLE_RATE = 12800
BLOCK_SAMPLES = 16
PACKED_BLOCK_BYTES = 5
BRR_BLOCK_BYTES = 9

# Two residual alphabets in units of (1 << shift) / 2, the same scale as a BRR
# nibble.  Table 0 covers steady material (the zero level rides out a tail
# without dither); table 1 is the wider alphabet for transients.
LEVEL_TABLES = ((-3, -1, 1, 3), (-5, -1, 1, 5))


def pair_table_bytes():
    """The 32 bytes the SPC700 keeps at $B0: nibble (two codes) -> BRR byte."""
    out = bytearray()
    for levels in LEVEL_TABLES:
        for index in range(16):
            out.append(((levels[index >> 2] & 15) << 4) | (levels[index & 3] & 15))
    return bytes(out)


def _predict(p1, p2, brr_filter):
    """Exact S-DSP filter contribution for 15-bit history samples (numpy)."""
    if brr_filter == 0:
        return np.zeros_like(p1)
    if brr_filter == 1:
        return p1 + ((-p1) >> 4)
    if brr_filter == 2:
        return p1 * 2 + ((-p1 * 3) >> 5) - p2 + (p2 >> 4)
    return p1 * 2 + ((-p1 * 13) >> 6) - p2 + ((p2 * 3) >> 4)


def _wrap15(s):
    """Clamp to 16 bits, then wrap to 15 as the S-DSP does after every sample."""
    s = np.clip(s, -32768, 32767)
    return (((s * 2 + 32768) & 0xFFFF) - 32768) >> 1


def encode(samples15):
    """Pack 15-bit mono samples (multiple of 16) into BRR2-5 blocks.

    Returns (packed bytes, decoded 15-bit samples as produced by the S-DSP).
    """
    samples15 = np.asarray(samples15, dtype=np.int64)
    if len(samples15) % BLOCK_SAMPLES:
        raise ValueError("sample count must be a multiple of 16")
    tables = np.array(LEVEL_TABLES, dtype=np.int64)
    packed = bytearray()
    decoded = np.zeros_like(samples15)
    p1 = p2 = 0
    for block in range(len(samples15) // BLOCK_SAMPLES):
        target = samples15[block * BLOCK_SAMPLES:(block + 1) * BLOCK_SAMPLES]
        # Candidate grid: a shift window around what the block's swing needs,
        # every filter (only filter 0 for the first block: the DSP's history is
        # zero there and a predictive filter would only amplify a click), both
        # alphabets.
        peak = int(np.max(np.abs(target)))
        residual_peak = max(peak, int(np.max(np.abs(np.diff(target, prepend=p1)))))
        centre = 1 if residual_peak < 4 else min(12, int(math.log2(residual_peak)) - 1)
        shifts = sorted({max(1, min(12, centre + d)) for d in (-2, -1, 0, 1, 2, 3)})
        filters = (0,) if block == 0 else (0, 1, 2, 3)
        cand = [(s, f, t) for s in shifts for f in filters for t in range(2)]
        n = len(cand)
        c_shift = np.array([c[0] for c in cand], dtype=np.int64)
        c_filter = np.array([c[1] for c in cand], dtype=np.int64)
        c_levels = tables[[c[2] for c in cand]]                # (n, 4)
        # level * (1 << shift) >> 1 with the hardware's arithmetic shift
        step = (c_levels << c_shift[:, None]) >> 1               # (n, 4)
        cp1 = np.full(n, p1, dtype=np.int64)
        cp2 = np.full(n, p2, dtype=np.int64)
        err = np.zeros(n, dtype=np.int64)
        codes = np.zeros((n, BLOCK_SAMPLES), dtype=np.int64)
        for i in range(BLOCK_SAMPLES):
            pred = np.zeros(n, dtype=np.int64)
            for f in (1, 2, 3):
                mask = c_filter == f
                if mask.any():
                    pred[mask] = _predict(cp1[mask], cp2[mask], f)
            values = _wrap15(pred[:, None] + step)               # (n, 4)
            diff = np.abs(values - target[i])
            pick = np.argmin(diff, axis=1)
            chosen = values[np.arange(n), pick]
            codes[:, i] = pick
            err += (chosen - target[i]) ** 2
            cp2 = cp1
            cp1 = chosen
        best = int(np.argmin(err))
        shift, brr_filter, table = cand[best]
        packed.append((shift << 4) | (brr_filter << 2) | table)
        for k in range(0, BLOCK_SAMPLES, 4):
            c = codes[best, k:k + 4]
            packed.append(int((c[0] << 6) | (c[1] << 4) | (c[2] << 2) | c[3]))
        # Replay the winner to get its decoded history (cheap, exact).
        out = decode_block(packed[-PACKED_BLOCK_BYTES:], p1, p2)
        decoded[block * BLOCK_SAMPLES:(block + 1) * BLOCK_SAMPLES] = out
        p2, p1 = int(out[-2]) if len(out) > 1 else p1, int(out[-1])
    return bytes(packed), decoded


def expand_block(packed_block, flags=0):
    """The SPC700's job: one packed block -> one 9-byte BRR block."""
    hdr = packed_block[0]
    levels = LEVEL_TABLES[hdr & 1]
    out = bytearray([(hdr & 0xFC) | flags])
    for b in packed_block[1:5]:
        codes = ((b >> 6) & 3, (b >> 4) & 3, (b >> 2) & 3, b & 3)
        out.append(((levels[codes[0]] & 15) << 4) | (levels[codes[1]] & 15))
        out.append(((levels[codes[2]] & 15) << 4) | (levels[codes[3]] & 15))
    return bytes(out)


def decode_brr_block(brr_block, p1, p2):
    """Exact S-DSP decode of one 9-byte BRR block from 15-bit history."""
    hdr = brr_block[0]
    shift = hdr >> 4
    brr_filter = (hdr >> 2) & 3
    out = []
    for b in brr_block[1:9]:
        for nib in (b >> 4, b & 15):
            nib = nib - 16 if nib >= 8 else nib
            if shift <= 12:
                s = (nib << shift) >> 1
            else:
                s = (-1 if nib < 0 else 0) << 11
            if brr_filter == 1:
                s += p1 + ((-p1) >> 4)
            elif brr_filter == 2:
                s += p1 * 2 + ((-p1 * 3) >> 5) - p2 + (p2 >> 4)
            elif brr_filter == 3:
                s += p1 * 2 + ((-p1 * 13) >> 6) - p2 + ((p2 * 3) >> 4)
            s = max(-32768, min(32767, s))
            s = (((s * 2 + 32768) & 0xFFFF) - 32768) >> 1
            out.append(s)
            p2, p1 = p1, s
    return out


def decode_block(packed_block, p1, p2):
    return decode_brr_block(expand_block(packed_block), p1, p2)


def decode(packed):
    """Reference decode of a whole stream to 15-bit samples."""
    out = []
    p1 = p2 = 0
    for off in range(0, len(packed), PACKED_BLOCK_BYTES):
        block = decode_block(packed[off:off + PACKED_BLOCK_BYTES], p1, p2)
        out.extend(block)
        p2, p1 = block[-2], block[-1]
    return np.array(out, dtype=np.int64)


# ── source preparation ───────────────────────────────────────────────────────

def load_wav_mono(path):
    """Read a PCM WAV as float32 mono in [-1, 1] plus its sample rate."""
    with wave.open(str(path), "rb") as w:
        channels, width, rate = w.getnchannels(), w.getsampwidth(), w.getframerate()
        raw = w.readframes(w.getnframes())
    if width == 1:
        data = (np.frombuffer(raw, dtype=np.uint8).astype(np.float32) - 128.0) / 128.0
    elif width == 2:
        data = np.frombuffer(raw, dtype="<i2").astype(np.float32) / 32768.0
    else:
        raise ValueError(f"{path}: unsupported sample width {width}")
    if channels > 1:
        data = data.reshape(-1, channels).mean(axis=1)
    return data, rate


def prepare(path, rate=SAMPLE_RATE, trim_db=-40.0, max_seconds=None,
            fade_seconds=0.08, peak=0.95):
    """WAV -> trimmed, resampled, peak-normalised 15-bit samples (x16 count).

    The lead-in and tail below ``trim_db`` of the peak are cut (10 ms RMS
    windows), the clip is optionally capped at ``max_seconds`` with a fade,
    resampled through scipy's polyphase filter and normalised so the loudest
    sample sits at ``peak`` of full scale.
    """
    from scipy.signal import resample_poly

    data, src_rate = load_wav_mono(path)
    if not len(data):
        raise ValueError(f"{path}: empty")
    g = math.gcd(src_rate, rate)
    data = resample_poly(data, rate // g, src_rate // g).astype(np.float32)
    win = rate // 100
    n = len(data) // win
    rms = np.sqrt(np.mean(data[:n * win].reshape(n, win) ** 2, axis=1))
    thr = float(np.max(np.abs(data))) * (10.0 ** (trim_db / 20.0))
    loud = np.nonzero(rms > thr)[0]
    if len(loud):
        data = data[loud[0] * win:(loud[-1] + 1) * win]
    if max_seconds is not None and len(data) > int(max_seconds * rate):
        data = data[:int(max_seconds * rate)].copy()
        fade = min(len(data), int(fade_seconds * rate))
        data[-fade:] *= np.linspace(1.0, 0.0, fade, dtype=np.float32)
    # A short fade at each end so a hard cut never starts or ends on a click.
    edge = min(len(data) // 4, rate // 200)
    if edge:
        data[:edge] *= np.linspace(0.0, 1.0, edge, dtype=np.float32)
        data[-edge:] *= np.linspace(1.0, 0.0, edge, dtype=np.float32)
    top = float(np.max(np.abs(data))) or 1.0
    data = data * (peak / top)
    pad = (-len(data)) % BLOCK_SAMPLES
    if pad:
        data = np.concatenate([data, np.zeros(pad, dtype=np.float32)])
    return np.clip(np.round(data * 16383.0), -16384, 16383).astype(np.int64)


def snr_db(reference, decoded):
    reference = np.asarray(reference, dtype=np.float64)
    noise = reference - np.asarray(decoded, dtype=np.float64)
    power = float(np.sum(reference ** 2)) or 1.0
    return 10.0 * math.log10(power / (float(np.sum(noise ** 2)) or 1e-9))


if __name__ == "__main__":
    import sys
    for arg in sys.argv[1:]:
        pcm = prepare(Path(arg))
        packed, decoded = encode(pcm)
        assert np.array_equal(decode(packed), decoded)
        print(f"{arg}: {len(pcm) / SAMPLE_RATE:.2f}s {len(packed)} bytes "
              f"SNR {snr_db(pcm, decoded):.1f} dB")
