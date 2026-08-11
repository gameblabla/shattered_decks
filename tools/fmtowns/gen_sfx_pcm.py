#!/usr/bin/env python3
"""Pack the game's sound effects into RF5C68 wave-table samples for FM TOWNS.

The FM TOWNS PCM chip is a Ricoh RF5C68 -- the same family as the Sega CD's
RF5C164, so this is the direct counterpart of tools/gen_cd32x_sfx_pcm.py, and
it maps WaifuSoundEffect onto the same source WAVs that generator uses.

Two things differ from the Sega CD version, both from the chip's own rules
(FM TOWNS Technical Databook 5.3, and src/platform/fmtowns/common/sound.h):

  * Sample data is 8-bit *sign-magnitude*, not two's complement: bit 7 is the
    sign (1 = positive) and bits 6-0 the magnitude, which is why 0x80 is
    silence.  0xFF is the chip's end/loop marker and 0x00 is not valid sample
    data, so magnitudes are clamped to 1..126 and neither byte can occur.

  * Each channel gets one 8 KiB wave-RAM slot, of which 7935 bytes are
    usable.  Rather than truncate the long effects, each one is resampled to
    the highest rate at which it still fits whole (capped at RATE_MAX), and
    the player programs that rate per channel when it triggers the sample --
    so short effects keep their fidelity and only the long ones lose any.

  * Each effect is loudness-normalised after resampling.  With only 7 bits of
    magnitude there is no headroom to waste, and the effects that get
    resampled hardest are exactly the ones the box filter quietens most --
    which is why the two longest ones were inaudible before.  See
    encode_sign_magnitude() for the measurements.

Usage: gen_sfx_pcm.py sounds_dir out.h
"""
import os
import sys
import wave

# Effect order must match WaifuSoundEffect in src/game/sounds.h.
SOUNDS = [
    ("SELECT", "Select.wav"),
    ("CONFIRM", "Confirm.wav"),
    ("CONFIRM_ALT", "ConfirmAlt.wav"),
    ("CARD_PLACED", "CardPlaced.wav"),
    ("CARD_DESTROYED", "CardDestroyed.wav"),
    ("TURN_PASSED", "TurnPassed.wav"),
    # No sample, matching gen_cd32x_sfx_pcm.py: the loss *music* covers this
    # moment (WAIFU_FM_MUSIC_LOST -> the Fail CD-DA track).
    ("YOU_LOST", None),
    ("LASER_SHOOT", "SlashAttack.wav"),
    ("DIRECT_HIT", "SlashAttack.wav"),
    ("CARD_DRAWN", "CardDrawn.wav"),
]

RATE_MAX = 8000
MAX_SAMPLE_BYTES = 7935   # FMT_SOUND_MAX_SAMPLE_SIZE
SRC_RATE = 44100
MAG_MAX = 126             # 0x00 and 0xFF are reserved, so 1..126 is the range
NORM_PERCENTILE = 0.995   # loudness reference; see encode_sign_magnitude()
GAIN_MAX = 12             # ceiling on how far a near-silent effect is lifted


def read_wav_u8_mono(path):
    with wave.open(path, "rb") as w:
        if w.getnchannels() != 1 or w.getsampwidth() != 1 or w.getframerate() != SRC_RATE:
            raise SystemExit(f"{path}: expected unsigned 8-bit mono {SRC_RATE} Hz WAV")
        return w.readframes(w.getnframes())


def resample_to_signed(data, dst_rate):
    """Box-filter down to dst_rate, returning samples centred on zero."""
    out_len = max(1, (len(data) * dst_rate + SRC_RATE // 2) // SRC_RATE)
    out = []
    for i in range(out_len):
        a = (i * SRC_RATE) // dst_rate
        b = ((i + 1) * SRC_RATE) // dst_rate
        b = max(b, a + 1)
        a = min(a, len(data) - 1)
        b = min(b, len(data))
        out.append(sum(data[a:b]) // (b - a) - 128)
    return out


def encode_sign_magnitude(samples):
    """Normalise to the chip's magnitude range and encode for the RF5C68.

    The normalisation is not cosmetic, it is what makes the long effects
    audible at all.  Two things conspired to bury them:

      * The source WAVs are mixed at wildly different levels -- SlashAttack
        arrives at an RMS of 73 out of a possible 127, while TurnPassed
        manages 9.7 and CardPlaced 0.9.

      * Every effect too long to fit a 7935-byte channel slot at 8 kHz is
        resampled *down* until it does, and the box filter that does the
        resampling is a brutal low-pass: the wider the box, the more of the
        signal it averages away.  TurnPassed takes the widest box of the set
        (44100/5368, over eight input samples per output one) and loses half
        its remaining RMS to it; CardDestroyed loses two thirds.

    So the two longest effects reached the chip at about a sixth of the
    amplitude of the short ones.  That is measured, not guessed: an FM/PCM
    recording of them playing back to back put TurnPassed's peak at 1275
    against SlashAttack's 7968, matching their encoded peaks of 20 and 125.
    Nothing was wrong with the wave-RAM upload or the chip programming -- a
    read-back audit confirmed every byte, including both bank crossings and
    the end markers.  The data itself was simply too quiet to hear over the
    CD-DA music.

    Normalising on the *peak* barely helps, because these are percussive
    sounds with one loud transient over a quiet body -- CardDestroyed's crest
    factor is 6.9, so peak-normalising buys it under 4 dB.  The reference is
    therefore the 99.5th-percentile magnitude, with anything above it clipped:
    that lifts CardDestroyed by 8.6 dB and TurnPassed by 18 dB while clipping
    at most 0.56% of samples, which on a 7-bit chip playing 8-bit source
    material is inaudible next to the gain.

    GAIN_MAX exists for CardPlaced and CardDrawn, which are so close to
    silence (RMS 1.1 and 2.2) that their 99.5th percentile is 1 and the
    formula would otherwise ask for 126x, amplifying nothing but the noise
    floor of a quiet recording.
    """
    ranked = sorted(abs(s) for s in samples)
    if not ranked or ranked[-1] == 0:
        return bytearray(0x80 | 1 for _ in samples)

    ref = ranked[min(len(ranked) - 1, int(len(ranked) * NORM_PERCENTILE))] or 1
    # Fixed point so the encoding is exactly reproducible across Python
    # versions; 1.0x is 256.
    gain = min((MAG_MAX * 256) // ref, GAIN_MAX * 256)

    out = bytearray()
    for s in samples:
        mag = min((abs(s) * gain + 128) >> 8, MAG_MAX)
        if mag == 0:
            mag = 1        # 0x00 is not valid sample data, 0x80 is +0
        out.append(0x80 | mag if s >= 0 else mag)
    return out


def fit_rate(frame_count):
    """Highest rate <= RATE_MAX at which this many source frames still fit."""
    seconds = frame_count / SRC_RATE
    if seconds <= 0:
        return RATE_MAX
    return max(1, min(RATE_MAX, int(MAX_SAMPLE_BYTES / seconds)))


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src_dir, out_path = sys.argv[1], sys.argv[2]

    bank = bytearray()
    entries = []          # (name, offset, length, rate)
    by_file = {}          # dedupe: SlashAttack backs two effects

    for name, filename in SOUNDS:
        if filename is None:
            entries.append((name, 0, 0, RATE_MAX))
            continue
        if filename in by_file:
            offset, length, rate = by_file[filename]
            entries.append((name, offset, length, rate))
            continue

        raw = read_wav_u8_mono(os.path.join(src_dir, filename))
        rate = fit_rate(len(raw))
        pcm = encode_sign_magnitude(resample_to_signed(raw, rate))
        if len(pcm) > MAX_SAMPLE_BYTES:
            pcm = pcm[:MAX_SAMPLE_BYTES]
        offset = len(bank)
        bank.extend(pcm)
        by_file[filename] = (offset, len(pcm), rate)
        entries.append((name, offset, len(pcm), rate))

    with open(out_path, "w") as out:
        out.write(
            "/* Generated by tools/fmtowns/gen_sfx_pcm.py. Do not edit. */\n"
            "#ifndef FMTOWNS_SFX_PCM_H\n"
            "#define FMTOWNS_SFX_PCM_H\n\n"
            "#include <stdint.h>\n\n"
            "typedef struct FmtownsSfxSample {\n"
            "    uint32_t offset;    /* into g_fmtowns_sfx_pcm */\n"
            "    uint16_t length;    /* 0 = this effect has no sample */\n"
            "    uint16_t rate;      /* Hz to program the channel at */\n"
            "} FmtownsSfxSample;\n\n"
            f"#define FMTOWNS_SFX_COUNT {len(entries)}\n"
            f"#define FMTOWNS_SFX_PCM_BYTES {len(bank)}\n\n"
            "static const FmtownsSfxSample g_fmtowns_sfx_samples[FMTOWNS_SFX_COUNT] = {\n")
        for name, offset, length, rate in entries:
            out.write("    { %6d, %5d, %5d },   /* %s */\n" % (offset, length, rate, name))
        out.write("};\n\n"
                  "static const uint8_t g_fmtowns_sfx_pcm[FMTOWNS_SFX_PCM_BYTES] = {\n")
        for i in range(0, len(bank), 16):
            out.write("    " + ",".join("0x%02x" % b for b in bank[i:i + 16]) + ",\n")
        out.write("};\n\n#endif /* FMTOWNS_SFX_PCM_H */\n")

    print("sfx pcm: %d effects, %d bytes of wave data" % (len(entries), len(bank)))
    for name, offset, length, rate in entries:
        if length:
            print("  %-16s %5d bytes @ %d Hz (%.2fs)" % (name, length, rate, length / rate))


if __name__ == "__main__":
    main()
