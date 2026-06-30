#!/usr/bin/env python3
"""Generate RF5C164 PCM music chunks for the CD32X in-duel / deck-editor themes.

The full CD-DA themes are minutes long, but on CD32X the deck-editor and battle
screens stream card data off the disc, so their music is served by the Sega-CD
RF5C164 PCM chip instead of CD-DA (which would force a stop/resume around every
read).  Each full source theme is resampled to a low-rate RF5C164 sign/magnitude
stream and split into short CD chunks.  The supervisor loads one chunk at a time
through Word RAM into Sub-CPU PRG RAM, then refills a RF5C164 wave-RAM ring from
that buffer.  The deliberately modest sample rate buys more autonomous playback
time in wave RAM while the CD drive is busy loading card/portrait assets.  Keep
each source chunk large enough to survive battle-entry asset traffic after the
initial ring prime, but still small enough for a short CD read: tiny chunks can
starve before the next music load gets CD time, while huge chunks can take longer
to load than the ring can play unattended.

Output: assets/generated/<STEM><NN>.BIN per theme chunk +
src/generated/cd32x_music_pcm.h with per-theme chunk counts and the shared
sample rate / freq delta.
"""
import os
import wave
import struct
import glob

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, 'assets', 'generated')
OUT_H = os.path.join(ROOT, 'src', 'generated', 'cd32x_music_pcm.h')
OUT_STAMP = os.path.join(OUT_DIR, '.cd32x_music_pcm.stamp')

RATE = 8000
CHUNK_BYTES = 64 * 1024
# RF5C164 frequency delta for RATE.  BlastEm models Sega CD PCM at
# 50 MHz / (4 * 384), with cur_ptr advancing by delta / 2048 per output sample.
SCD_MASTER_CLOCK = 50000000
RF5C164_DIVIDER = 4
RF5C164_FRAC_SCALE = 1 << 11
FREQ_DELTA = (RATE * RF5C164_FRAC_SCALE * RF5C164_DIVIDER * 384 + SCD_MASTER_CLOCK // 2) // SCD_MASTER_CLOCK

# (macro name, CD filename stem, source wav)
THEMES = [
    ('DECK_EDITOR', 'DECK', 'Music/Overworld.wav'),
    ('BATTLE',      'BTL',  'Music/Battle.wav'),
    ('BOSS',        'BOSS', 'Music/Boss.wav'),
    ('FINAL_BOSS',  'FINL', 'Music/FinalBoss.wav'),
]


def read_wav(path):
    with wave.open(path, 'rb') as w:
        ch = w.getnchannels()
        width = w.getsampwidth()
        rate = w.getframerate()
        raw = w.readframes(w.getnframes())
    return ch, width, rate, raw


def to_mono_s16(ch, width, raw):
    """Return a list of 16-bit signed mono samples."""
    out = []
    if width == 2:
        n = len(raw) // 2
        vals = struct.unpack('<%dh' % n, raw[:n * 2])
        if ch == 2:
            for i in range(0, n - 1, 2):
                out.append((vals[i] + vals[i + 1]) // 2)
        else:
            out = list(vals)
    elif width == 1:
        # unsigned 8-bit
        if ch == 2:
            for i in range(0, len(raw) - 1, 2):
                a = raw[i] - 128
                b = raw[i + 1] - 128
                out.append(((a + b) // 2) << 8)
        else:
            out = [(b - 128) << 8 for b in raw]
    else:
        raise SystemExit('unsupported sample width %d' % width)
    return out


def resample(samples, src_rate, dst_rate, count):
    """Nearest-neighbour resample to dst_rate, exactly `count` output samples."""
    out = [0] * count
    n = len(samples)
    if n == 0:
        return out
    for i in range(count):
        src = (i * src_rate) // dst_rate
        if src >= n:
            src = n - 1
        out[i] = samples[src]
    return out


def to_rf5c164(samples):
    """s16 -> RF5C164 sign/magnitude bytes (bit7 = sign, 0=negative)."""
    out = bytearray(len(samples))
    for i, s in enumerate(samples):
        v = s >> 8  # to 8-bit signed
        if v < 0:
            m = -v
            if m > 127:
                m = 127
            out[i] = m & 0x7F           # sign bit 0 => negative magnitude
        else:
            if v > 126:
                v = 126
            out[i] = 0x80 | v           # sign bit 1 => positive magnitude
        if out[i] == 0xFF:
            out[i] = 0xFE               # never emit the loop-end marker as data
    return out


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    entries = []
    for _, stem, _ in THEMES:
        for old in glob.glob(os.path.join(OUT_DIR, '%s[0-9][0-9].BIN' % stem)):
            os.remove(old)
        old_clip = os.path.join(OUT_DIR, '%s_PCM.BIN' % stem)
        if os.path.exists(old_clip):
            os.remove(old_clip)

    for macro, stem, wav in THEMES:
        path = os.path.join(ROOT, wav)
        ch, width, rate, raw = read_wav(path)
        mono = to_mono_s16(ch, width, raw)
        count = max(1, (len(mono) * RATE + rate // 2) // rate)
        clip = resample(mono, rate, RATE, count)
        pcm = to_rf5c164(clip)
        chunks = 0
        for off in range(0, len(pcm), CHUNK_BYTES):
            chunk = pcm[off:off + CHUNK_BYTES]
            out_bin = os.path.join(OUT_DIR, '%s%02d.BIN' % (stem, chunks))
            with open(out_bin, 'wb') as f:
                f.write(chunk)
            chunks += 1
        entries.append((macro, stem, len(pcm), chunks))
        print('%-20s %-10s %8d bytes %2d chunks (%.1fs)' %
              (macro, stem, len(pcm), chunks, len(pcm) / float(RATE)))

    with open(OUT_H, 'w') as f:
        f.write('/* Generated by tools/gen_cd32x_music_pcm.py - do not edit. */\n')
        f.write('#ifndef WAIFU_CD32X_MUSIC_PCM_H\n#define WAIFU_CD32X_MUSIC_PCM_H\n\n')
        f.write('#define WAIFU_CD32X_MUSIC_PCM_RATE %u\n' % RATE)
        f.write('#define WAIFU_CD32X_MUSIC_PCM_FREQ_DELTA 0x%04Xu\n' % FREQ_DELTA)
        f.write('#define WAIFU_CD32X_MUSIC_PCM_CHUNK_BYTES %u\n' % CHUNK_BYTES)
        f.write('#define WAIFU_CD32X_MUSIC_PCM_MAX_CHUNK_BYTES %u\n\n' % CHUNK_BYTES)
        for i, (macro, stem, n, chunks) in enumerate(entries):
            f.write('#define WAIFU_CD32X_MUSIC_%s_ID %d\n' % (macro, i))
            f.write('#define WAIFU_CD32X_MUSIC_%s_STEM "%s"\n' % (macro, stem))
            f.write('#define WAIFU_CD32X_MUSIC_%s_BYTES %u\n' % (macro, n))
            f.write('#define WAIFU_CD32X_MUSIC_%s_CHUNKS %u\n' % (macro, chunks))
        f.write('#define WAIFU_CD32X_MUSIC_COUNT %d\n' % len(entries))
        f.write('\n#endif\n')
    with open(OUT_STAMP, 'w') as f:
        f.write('ok\n')
    print('wrote', OUT_H)


if __name__ == '__main__':
    main()
