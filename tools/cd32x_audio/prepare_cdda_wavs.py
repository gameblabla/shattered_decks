#!/usr/bin/env python3
"""Prepare Sega CD CD-DA WAV tracks for CD32X CUE files.

The source music files remain untouched.  Build-local output is normalized to
16-bit stereo PCM at 44100 Hz, which is what Sega CD CD-DA tracks and Blastem's
WAVE-track loader expect.
"""
from __future__ import annotations

import audioop
import shutil
import sys
import wave
from pathlib import Path

TARGET_RATE = 44100
TARGET_CHANNELS = 2
TARGET_WIDTH = 2


def normalize_wav(src: Path, dst: Path) -> str:
    with wave.open(str(src), "rb") as w:
        channels = w.getnchannels()
        width = w.getsampwidth()
        rate = w.getframerate()
        frames = w.getnframes()
        comptype = w.getcomptype()
        raw = w.readframes(frames)
    if comptype != "NONE":
        raise SystemExit(f"{src}: compressed WAV is not supported ({comptype})")
    if width != TARGET_WIDTH:
        raise SystemExit(f"{src}: expected 16-bit PCM, got {width * 8}-bit")
    if channels not in (1, 2):
        raise SystemExit(f"{src}: expected mono/stereo PCM, got {channels} channels")

    if channels == 1:
        raw = audioop.tostereo(raw, TARGET_WIDTH, 1.0, 1.0)
        channels = 2
    if rate != TARGET_RATE:
        raw, _ = audioop.ratecv(raw, TARGET_WIDTH, channels, rate, TARGET_RATE, None)
        rate = TARGET_RATE

    dst.parent.mkdir(parents=True, exist_ok=True)
    if channels == TARGET_CHANNELS and rate == TARGET_RATE:
        # Re-write rather than symlink so the CUE stays self-contained under the
        # build directory, and so headers are guaranteed canonical PCM headers.
        with wave.open(str(dst), "wb") as out:
            out.setnchannels(TARGET_CHANNELS)
            out.setsampwidth(TARGET_WIDTH)
            out.setframerate(TARGET_RATE)
            out.writeframes(raw)
    return f"{src} -> {dst} ({frames} source frames, {TARGET_RATE} Hz CD-DA)"


def main(argv: list[str]) -> int:
    if len(argv) < 3:
        print("usage: prepare_cdda_wavs.py OUT_DIR INPUT.wav...", file=sys.stderr)
        return 2
    out_dir = Path(argv[1])
    manifest = []
    for idx, src_name in enumerate(argv[2:], start=2):
        src = Path(src_name)
        stem = src.stem.upper()[:32]
        dst = out_dir / f"TRACK{idx:02d}_{stem}.WAV"
        print(normalize_wav(src, dst))
        manifest.append(str(dst))
    (out_dir / "tracks.txt").write_text("\n".join(manifest) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
