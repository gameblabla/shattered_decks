#!/usr/bin/env python3
"""Convert the MSX2 music sources to bankable MSXgl lVGM streams.

The source recordings are VGM files containing AY/PSG writes only.  MSXzip is
the MSXgl tool that owns the lVGM conversion, simplification and 16K split
format, so this wrapper validates the input and output around that tool and
leaves a reproducible manifest for the scene packer.

The generated files are deliberately not checked in: they are cartridge
outputs, just like the scene binaries.  The VGM sources and this script are
the inputs that make them.
"""

import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path
import wave


ROOT = Path(__file__).resolve().parents[2]
SOURCE_DIR = ROOT / "msx_music"
ASSET_DIR = ROOT / "src" / "msx2" / "assets"
META = ASSET_DIR / "music.json"
MSXZIP = ROOT / "MSXgl-main" / "tools" / "MSXtk" / "bin" / "MSXzip"
VGMTOOLS = ROOT / "FMTOWNSCD_EXAMPLE_Cube" / "vgmtools"
VGM_CMP_BUILD = ROOT / "build" / "msx2_vgmtools"
SEGMENT_BYTES = 16 * 1024

# These names are the stable interface used by msx2_audio.c.  A track can be
# an alias of another source; the scene generator places each output once and
# emits one record for every public id.
TRACKS = (
    ("title", "titlescreen.vgm", True),
    ("overworld", "Overworld.vgm", True),
    ("battle", "Battle.vgm", True),
    ("boss", "Boss.vgm", True),
    ("final_boss", "FinalBoss.vgm", True),
    ("result", "Victory.vgm", False),
    ("lost", "Fail.vgm", False),
)


def fail(path, message):
    raise SystemExit("%s: %s" % (path, message))


def validate_vgm(path):
    """Check that *path* is a bounded, AY-only VGM command stream."""
    data = path.read_bytes()
    if len(data) < 0x40 or data[:4] != b"Vgm ":
        fail(path, "not a VGM file")
    version = int.from_bytes(data[0x08:0x0C], "little")
    if version < 0x00000150:
        fail(path, "VGM version is too old for the declared data offset")
    data_offset = int.from_bytes(data[0x34:0x38], "little")
    cursor = 0x40 if data_offset == 0 else 0x34 + data_offset
    if cursor >= len(data):
        fail(path, "data offset is outside the file")

    saw_end = False
    while cursor < len(data):
        op = data[cursor]
        if op == 0x66:
            saw_end = True
            break
        if op == 0x61:
            size = 3
        elif op in (0x62, 0x63):
            size = 1
        elif 0x70 <= op <= 0x7F:
            size = 1
        elif op == 0xA0:  # AY-3-8910 / YM2149 register write
            size = 3
            if cursor + size > len(data):
                fail(path, "truncated AY write")
            if data[cursor + 1] > 15:
                fail(path, "AY register is outside 0..15")
        else:
            fail(path, "unsupported VGM opcode 0x%02X at 0x%X" % (op, cursor))
        cursor += size
        if cursor > len(data):
            fail(path, "truncated opcode 0x%02X" % op)
    if not saw_end:
        fail(path, "command stream has no end marker")
    return data


def vgm_events(path):
    """Return the timed AY writes that *path* presents to the PSG.

    ``vgm_cmp -justtmr`` is intentionally used before MSXzip, but its output
    is accepted only if this event stream is byte-for-byte equivalent to the
    source.  Comparing decoded register events also catches an optimizer that
    preserves file headers while changing a note, effect, or wait.
    """
    data = validate_vgm(path)
    version = int.from_bytes(data[0x08:0x0C], "little")
    data_offset = int.from_bytes(data[0x34:0x38], "little")
    cursor = 0x40 if data_offset == 0 else 0x34 + data_offset
    ticks = 0
    events = []
    while cursor < len(data):
        op = data[cursor]
        if op == 0x66:
            return events, ticks
        if op == 0x61:
            ticks += int.from_bytes(data[cursor + 1:cursor + 3], "little")
            cursor += 3
        elif op == 0x62:
            ticks += 735
            cursor += 1
        elif op == 0x63:
            ticks += 882
            cursor += 1
        elif 0x70 <= op <= 0x7F:
            ticks += (op & 0x0F) + 1
            cursor += 1
        elif op == 0xA0:
            events.append((ticks, data[cursor + 1], data[cursor + 2]))
            cursor += 3
        else:
            # validate_vgm() has already rejected this.  Keep the branch
            # explicit so a future supported command cannot be silently
            # omitted from the equivalence check.
            fail(path, "unsupported event opcode 0x%02X at 0x%X" %
                 (op, cursor))
    fail(path, "command stream ended before 0x66")


def find_vgm_cmp():
    """Find or build the repository's lossless VGM optimizer."""
    configured = os.environ.get("VGM_CMP")
    if configured:
        executable = Path(configured)
        if not executable.exists():
            fail(executable, "VGM_CMP does not point to an executable")
        return executable

    candidates = (VGM_CMP_BUILD / "vgm_cmp", VGM_CMP_BUILD / "vgm_cmp.exe")
    for executable in candidates:
        if executable.exists():
            return executable

    cmake = shutil.which("cmake")
    if cmake is None:
        fail(VGMTOOLS, "cmake is required to build vgm_cmp; set VGM_CMP")
    if not VGMTOOLS.exists():
        fail(VGMTOOLS, "repository VGM optimizer sources are missing")
    try:
        subprocess.run([
            cmake, "-S", str(VGMTOOLS), "-B", str(VGM_CMP_BUILD),
            "-DCMAKE_BUILD_TYPE=Release",
        ], check=True)
        subprocess.run([
            cmake, "--build", str(VGM_CMP_BUILD), "--target", "vgm_cmp",
        ], check=True)
    except subprocess.CalledProcessError as exc:
        fail(VGMTOOLS, "could not build vgm_cmp (exit status %d)" %
             exc.returncode)
    for executable in candidates:
        if executable.exists():
            return executable
    fail(VGM_CMP_BUILD, "vgm_cmp build produced no executable")


def find_vgm_renderer():
    configured = os.environ.get("VGM2WAV")
    executable = Path(configured) if configured else shutil.which("vgm2wav")
    if executable is None or not Path(executable).exists():
        fail(ROOT / "vgm2wav",
             "VGM2WAV or vgm2wav is required for the rendered-audio check")
    return Path(executable)


def normalize_header(source, output):
    """Keep non-data metadata stable after vgm_cmp rewrites its header."""
    original = source.read_bytes()
    optimized = bytearray(output.read_bytes())
    if len(original) < 0x80 or len(optimized) < 0x80:
        fail(source, "VGM header is shorter than 0x80 bytes")
    original_start = 0x40 if int.from_bytes(original[0x34:0x38], "little") == 0 \
        else 0x34 + int.from_bytes(original[0x34:0x38], "little")
    optimized_start = 0x40 if int.from_bytes(optimized[0x34:0x38], "little") == 0 \
        else 0x34 + int.from_bytes(optimized[0x34:0x38], "little")
    if original_start != optimized_start:
        fail(source, "vgm_cmp moved the command stream data offset")
    optimized[:0x80] = original[:0x80]
    optimized[0x04:0x08] = (len(optimized) - 4).to_bytes(4, "little")
    output.write_bytes(optimized)


def rendered_audio_signature(path, renderer, output):
    """Render one deterministic pass and return its PCM signature."""
    output.unlink(missing_ok=True)
    try:
        subprocess.run([str(renderer), str(path), str(output)], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except subprocess.CalledProcessError as exc:
        fail(path, "VGM2WAV failed with exit status %d" % exc.returncode)
    try:
        with wave.open(str(output), "rb") as wav:
            params = (wav.getnchannels(), wav.getsampwidth(),
                      wav.getframerate(), wav.getnframes())
            pcm_hash = hashlib.sha256(wav.readframes(wav.getnframes())).hexdigest()
    finally:
        output.unlink(missing_ok=True)
    return params, pcm_hash


def optimize(source, output, optimizer, renderer):
    """Run vgm_cmp and accept its output only after both lossless checks."""
    output.unlink(missing_ok=True)
    try:
        subprocess.run([
            str(optimizer), "-justtmr", str(source), str(output),
        ], check=True, stdout=subprocess.DEVNULL)
    except subprocess.CalledProcessError as exc:
        fail(source, "vgm_cmp failed with exit status %d" % exc.returncode)
    # vgm_cmp does not write a second file when no compression is possible.
    if not output.exists():
        shutil.copyfile(source, output)
    normalize_header(source, output)
    original_events = vgm_events(source)
    optimized_events = vgm_events(output)
    if original_events != optimized_events:
        fail(source, "vgm_cmp -justtmr changed timed AY register events")
    original_audio = rendered_audio_signature(
        source, renderer, VGM_CMP_BUILD / (source.name + ".original.wav"))
    optimized_audio = rendered_audio_signature(
        output, renderer, VGM_CMP_BUILD / (source.name + ".optimized.wav"))
    if original_audio != optimized_audio:
        # vgm_cmp preserves the timed AY event list, but some VGM renderers
        # treat its alternate long-wait encoding differently.  Keep the
        # optimized candidate for diagnostics and feed the original to MSXzip:
        # the final asset must never trade audible equivalence for bytes.
        return source, False, "rendered PCM mismatch"
    return output, True, ""


def validate_lvgm(path):
    """Validate MSXzip's PSG-only lVGM and its 16K notification seams."""
    data = path.read_bytes()
    if len(data) < 7 or data[:4] != b"lVGM":
        fail(path, "MSXzip did not produce an lVGM stream")
    option = data[4]
    if option & 0x04:
        fail(path, "device-list lVGM output is not expected")
    # With no device list, byte five is the common PSG value and byte six is
    # the first command.  MSXzip emits a common value even when it is zero.
    cursor = 6
    saw_end = False
    while cursor < len(data):
        op = data[cursor]
        if op == 0xFF:
            saw_end = True
            break
        if op == 0xFD:
            if cursor + 1 >= len(data):
                fail(path, "truncated lVGM notification")
            cursor += 2
            continue
        if op == 0xFE or op == 0xF0:
            cursor += 1
            continue
        if op == 0xE0:
            cursor += 1
            continue
        # PSG direct writes are either the register-0/value pair (00), a
        # compact register/value opcode (10..cf, plus 01..0f), or a
        # common-value write (d0..df).  MSXgl's lVGM format reserves only 00
        # as the two-byte form; 07, for example, is compact register 0.
        if op == 0x00:
            cursor += 2
        else:
            cursor += 1
        if cursor > len(data):
            fail(path, "truncated lVGM command")
    if not saw_end:
        fail(path, "lVGM stream has no end marker")

    segment_count = (len(data) + SEGMENT_BYTES - 1) // SEGMENT_BYTES
    for segment in range(1, segment_count):
        seam = segment * SEGMENT_BYTES
        if data[seam - 2:seam] != b"\xFD\x00":
            fail(path, "missing FD 00 segment notification before 0x%X" % seam)
    return data, segment_count


def convert(source, output):
    output.unlink(missing_ok=True)
    executable = Path(os.environ.get("MSXZIP", str(MSXZIP)))
    if not executable.exists():
        fail(executable, "MSXzip is missing; set MSXZIP to the MSXgl converter")
    command = [
        str(executable), str(source), "-o", str(output), "-bin", "-lVGM",
        "--simplify", "--split", "16K", "-nodate", "-nodeco",
    ]
    try:
        subprocess.run(command, check=True)
    except subprocess.CalledProcessError as exc:
        fail(source, "MSXzip failed with exit status %d" % exc.returncode)
    return validate_lvgm(output)


def main():
    quiet = "--quiet" in sys.argv
    ASSET_DIR.mkdir(parents=True, exist_ok=True)
    optimizer = find_vgm_cmp()
    renderer = find_vgm_renderer()
    assets = []
    for name, filename, loop in TRACKS:
        source = SOURCE_DIR / filename
        if not source.exists():
            fail(source, "music source is missing")
        validate_vgm(source)
        candidate = VGM_CMP_BUILD / (filename + ".optimized.vgm")
        input_path, optimizer_accepted, optimizer_rejection = optimize(
            source, candidate, optimizer, renderer)
        output = ASSET_DIR / ("music_" + name + ".bin")
        data, segments = convert(input_path, output)
        source_hash = hashlib.sha256(source.read_bytes()).hexdigest()
        candidate_hash = hashlib.sha256(candidate.read_bytes()).hexdigest()
        assets.append({
            "id": name,
            "source": str(source.relative_to(ROOT)),
            "file": output.name,
            "bytes": len(data),
            "segments": segments,
            "loop": bool(loop),
            "sha256": source_hash,
            "optimized_bytes": candidate.stat().st_size,
            "optimized_sha256": candidate_hash,
            "optimizer_accepted": optimizer_accepted,
            "optimizer_rejection": optimizer_rejection,
            "input": str(input_path.relative_to(ROOT)),
        })
        if not quiet:
            print("%-10s %6d bytes, %d segments <- %s" %
                  (name, len(data), segments, source.relative_to(ROOT)))

    payload = {
        "format": 1,
        "converter": "MSXzip --simplify --split 16K",
        "optimizer": "vgm_cmp -justtmr",
        "optimizer_source": str(VGMTOOLS.relative_to(ROOT)),
        "segment_bytes": SEGMENT_BYTES,
        "assets": assets,
    }
    META.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")
    if not quiet:
        print("wrote %s" % META.relative_to(ROOT))


if __name__ == "__main__":
    main()
