#!/usr/bin/env python3
"""Convert the MSX2 music sources to bankable MSXgl lVGM streams.

The cartridge carries THREE renditions of the soundtrack, because an MSX2 may
have any of three sound chips and the port plays the best one it finds:

    msx_music/*.vgm             AY-3-8910 / YM2149     (the PSG, always there)
    msx_music/MSX OPLL/*.vgm    YM2413                 (MSX-MUSIC / FM-PAC)
    msx_music/MSX-AUDIO/*.vgm   Y8950                  (MSX-AUDIO)

MSXzip is the MSXgl tool that owns the lVGM conversion, simplification and 16K
split format, so this wrapper validates the input and output around that tool
and leaves a reproducible manifest for the scene packer.  The FM sets are not
complete -- there is no FM rendition of the title theme -- and that is
expected: a public track with no recording in the selected set is emitted with
a zero segment count, and msx2_audio.c falls back to the PSG recording.

The generated files are deliberately not checked in: they are cartridge
outputs, just like the scene binaries.  The VGM sources and this script are
the inputs that make them.

Source VGMs are shrunk in place by tools/msx2/optimize_vgm_sources.py; the
vgm_cmp pass here is what keeps that true for a recording added later.
"""

import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SOURCE_DIR = ROOT / "msx_music"
ASSET_DIR = ROOT / "src" / "msx2" / "assets"
META = ASSET_DIR / "music.json"
MSXZIP = ROOT / "MSXgl-main" / "tools" / "MSXtk" / "bin" / "MSXzip"
VGMTOOLS = ROOT / "FMTOWNSCD_EXAMPLE_Cube" / "vgmtools"
VGM_CMP_BUILD = ROOT / "build" / "msx2_vgmtools"
SEGMENT_BYTES = 16 * 1024

# VGM register-write opcode and header clock offset per supported chip, keyed
# by the lVGM device bit the converted stream must end up declaring.
LVGM_CHIP_PSG = 0x01
LVGM_CHIP_MSXMUSIC = 0x02
LVGM_CHIP_MSXAUDIO = 0x04
CHIP_WRITE_OP = {LVGM_CHIP_PSG: 0xA0, LVGM_CHIP_MSXMUSIC: 0x51,
                 LVGM_CHIP_MSXAUDIO: 0x5C}
CHIP_CLOCK_OFFSET = {LVGM_CHIP_PSG: 0x74, LVGM_CHIP_MSXMUSIC: 0x10,
                     LVGM_CHIP_MSXAUDIO: 0x58}
CHIP_NAME = {LVGM_CHIP_PSG: "PSG", LVGM_CHIP_MSXMUSIC: "MSX-MUSIC",
             LVGM_CHIP_MSXAUDIO: "MSX-AUDIO"}

# The three sets, in the order msx2_audio.c's Msx2AudioChip enum names them.
# The prefix is what makes an asset id unique; the PSG set keeps the bare
# names it has always had, so nothing downstream has to be renamed.
CHIP_SETS = (
    ("", "", LVGM_CHIP_PSG),
    ("opll_", "MSX OPLL", LVGM_CHIP_MSXMUSIC),
    ("msxaudio_", "MSX-AUDIO", LVGM_CHIP_MSXAUDIO),
)

# These names are the stable interface used by msx2_audio.c.  A track can be
# an alias of another source; the scene generator places each output once and
# emits one record for every public id.  A set that has no file for a track
# simply does not contribute one.
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


def validate_vgm(path, chip):
    """Check that *path* is a bounded command stream for exactly *chip*."""
    data = path.read_bytes()
    if len(data) < 0x80 or data[:4] != b"Vgm ":
        fail(path, "not a VGM file")
    version = int.from_bytes(data[0x08:0x0C], "little")
    if version < 0x00000150:
        fail(path, "VGM version is too old for the declared data offset")
    for bit, offset in CHIP_CLOCK_OFFSET.items():
        clock = int.from_bytes(data[offset:offset + 4], "little")
        if bool(clock) != (bit == chip):
            fail(path, "expected a %s recording, but the %s clock is %d"
                 % (CHIP_NAME[chip], CHIP_NAME[bit], clock))
    write_op = CHIP_WRITE_OP[chip]

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
        elif op == write_op:
            size = 3
            if cursor + size > len(data):
                fail(path, "truncated %s write" % CHIP_NAME[chip])
            if chip == LVGM_CHIP_PSG and data[cursor + 1] > 15:
                fail(path, "AY register is outside 0..15")
        else:
            fail(path, "unsupported VGM opcode 0x%02X at 0x%X" % (op, cursor))
        cursor += size
        if cursor > len(data):
            fail(path, "truncated opcode 0x%02X" % op)
    if not saw_end:
        fail(path, "command stream has no end marker")
    return data


def vgm_events(path, chip):
    """Return the timed register writes that *path* presents to the chip.

    This is the equivalence the optimizer is held to.  Comparing decoded
    register events -- not file bytes, and not rendered PCM -- is what catches
    an optimizer that preserves headers while changing a note, effect or wait,
    and it is the only comparison that is exactly as strict as the hardware:
    a renderer resamples at its own command boundaries, so re-spelling one
    wait as two shifts a few samples by an LSB while the chip sees no
    difference at all.
    """
    data = validate_vgm(path, chip)
    write_op = CHIP_WRITE_OP[chip]
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
        elif op == write_op:
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


# -----------------------------------------------------------------------------
#  Dead air at the head of a recording
#
#  These are machine transcriptions, and several of them open with the chip
#  setup at sample 0 and then simply nothing: the MSX-MUSIC and MSX-AUDIO
#  battle themes wait FIFTEEN SECONDS before their first note.  On a cartridge
#  that is not merely rude, it is fifteen seconds of a duel with no music and
#  several hundred bytes of encoded silence, and it comes back every time the
#  track loops.
#
#  What is removed is only the wait commands that run from the last of the
#  opening register writes to the first write that happens after any time has
#  passed.  No register write lives inside that span -- that is the definition
#  of it -- so the chip is handed exactly the same setup in exactly the same
#  order, just without the gap.  A track whose first note is genuinely at
#  sample 0, or that interleaves writes with its lead-in, loses nothing.
# -----------------------------------------------------------------------------
def trim_leadin(source, output, chip):
    """Write *source* to *output* without its silent lead-in; return its ticks."""
    data = source.read_bytes()
    write_op = CHIP_WRITE_OP[chip]
    start = 0x34 + int.from_bytes(data[0x34:0x38], "little")

    cursor = start
    ticks = 0
    waits = []          # (offset, size) of every wait before the first note
    while cursor < len(data):
        op = data[cursor]
        if op == 0x66:
            break
        if op == 0x61:
            ticks += int.from_bytes(data[cursor + 1:cursor + 3], "little")
            waits.append((cursor, 3))
            cursor += 3
        elif op in (0x62, 0x63):
            ticks += 735 if op == 0x62 else 882
            waits.append((cursor, 1))
            cursor += 1
        elif 0x70 <= op <= 0x7F:
            ticks += (op & 0x0F) + 1
            waits.append((cursor, 1))
            cursor += 1
        elif op == write_op:
            if ticks:
                break   # the first write after time has passed: stop here
            cursor += 3
        else:
            fail(source, "unsupported VGM opcode 0x%02X at 0x%X" % (op, cursor))

    if not ticks:
        shutil.copyfile(source, output)
        return 0

    kept = bytearray(data[:start])
    at = start
    for offset, size in waits:
        kept += data[at:offset]
        at = offset + size
    kept += data[at:]
    dropped = len(data) - len(kept)

    # Every offset in the header past the command stream moved, and the song
    # is shorter by exactly the silence that was taken out of it.
    def shift(field, base):
        value = int.from_bytes(kept[field:field + 4], "little")
        if value:
            kept[field:field + 4] = (value - dropped).to_bytes(4, "little")
    shift(0x04, 0x04)   # EOF offset
    shift(0x14, 0x14)   # GD3 offset
    total = int.from_bytes(kept[0x18:0x1C], "little")
    kept[0x18:0x1C] = max(0, total - ticks).to_bytes(4, "little")
    # These recordings carry no loop point (the game loops on lVGM's own FE
    # marker), so there is no loop offset to move.  Refuse rather than corrupt
    # one if that ever changes.
    if int.from_bytes(kept[0x1C:0x20], "little"):
        fail(source, "has a VGM loop point; trim_leadin would have to move it")
    output.write_bytes(bytes(kept))
    return ticks


def optimize(source, output, optimizer, chip):
    """Run vgm_cmp and accept its output only if the chip cannot tell."""
    output.unlink(missing_ok=True)
    try:
        subprocess.run([
            str(optimizer), str(source), str(output),
        ], check=True, stdout=subprocess.DEVNULL)
    except subprocess.CalledProcessError as exc:
        fail(source, "vgm_cmp failed with exit status %d" % exc.returncode)
    # vgm_cmp does not write a second file when no compression is possible,
    # which is the normal case now that the sources are optimized in place.
    if not output.exists():
        shutil.copyfile(source, output)
        return source, True, ""
    if vgm_events(source, chip) != vgm_events(output, chip):
        return source, False, "changed timed register events"
    return output, True, ""


def lvgm_command_size(data, cursor, chip, path):
    """Bytes consumed by one lVGM chip command, mirroring msx2_lvgm.c."""
    op = data[cursor]
    if chip == LVGM_CHIP_MSXMUSIC:
        high = op >> 4
        if high <= 0x3:
            return 2                                     # R#op = nn
        if high == 0x4:
            return 1 + (8, 9, 9, 9, 3, 3, 3)[op & 0x0F]  # 4x nn[] copy a run
        if high == 0x5:
            return 2                                     # 5x nn fill a run
        if high == 0x6:
            return 3                                     # 6n rr vv
        # 7n rr vv[]: the opcode, the first register, and n+3 values.
        if high == 0x7:
            return 5 + (op & 0x0F)
        if 0x8 <= high <= 0xB:
            return 1                                     # R#(op & 7F) = 0
        fail(path, "unsupported lVGM OPLL opcode 0x%02X at 0x%X" % (op, cursor))
    if chip == LVGM_CHIP_MSXAUDIO:
        return 2                                         # rr vv
    # PSG direct writes are either the two-byte "R#0n = nn" form (00..0f, in
    # which the WHOLE byte is the register number), a compact register/value
    # opcode (10..cf), or a common-value write (d0..df).  The two-byte form is
    # the entire 0x low nibble range, not just 00: msx2_lvgm.c switches on
    # `*ptr & 0xF0`, so 02 F2 is "R#2 = F2" and not two commands.  Reading it
    # the narrow way desynchronised this walker on almost every recording --
    # it then resynchronised by chance and still found the end marker, which
    # is why the mistake survived.
    return 2 if (op & 0xF0) == 0x00 else 1


def validate_lvgm(path, chip):
    """Validate MSXzip's lVGM stream and its 16K notification seams."""
    data = path.read_bytes()
    if len(data) < 7 or data[:4] != b"lVGM":
        fail(path, "MSXzip did not produce an lVGM stream")
    option = data[4]
    cursor = 5
    if option & 0x04:
        devices = data[cursor]
        cursor += 1
    else:
        devices = LVGM_CHIP_PSG
    if devices != chip:
        fail(path, "lVGM declares devices 0x%02X, expected 0x%02X"
             % (devices, chip))
    if devices & LVGM_CHIP_PSG:
        # MSXzip emits the common PSG value even when it is zero.
        cursor += 1

    # Chunk markers select the chip; a stream that emits none is PSG data.
    current = LVGM_CHIP_PSG
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
        if op == 0xFE:
            cursor += 1
            continue
        if op in (0xF0, 0xF1, 0xF2):
            current = (LVGM_CHIP_PSG, LVGM_CHIP_MSXMUSIC,
                       LVGM_CHIP_MSXAUDIO)[op & 0x0F]
            cursor += 1
            continue
        if 0xF3 <= op <= 0xFC:
            fail(path, "lVGM selects an unsupported chip (0x%02X)" % op)
        if (op & 0xF0) == 0xE0:
            cursor += 1
            continue
        cursor += lvgm_command_size(data, cursor, current, path)
        if cursor > len(data):
            fail(path, "truncated lVGM command")
    if not saw_end:
        fail(path, "lVGM stream has no end marker")

    # Every 16K seam must be reached through an FD 00 notification, which is
    # what hands msx2_audio.c the chance to map the next cartridge segment
    # before the decoder reads another byte.  MSXzip emits the marker as soon
    # as the NEXT command would not fit and then pads the segment out with
    # zeroes, so the marker sits at the end of the segment but not necessarily
    # in its last two bytes -- a command that is eighteen bytes long pushes it
    # further back.  The padding is never decoded: the callback retargets the
    # pointer at 0x8000 and the parser resumes there.
    segment_count = (len(data) + SEGMENT_BYTES - 1) // SEGMENT_BYTES
    for segment in range(1, segment_count):
        seam = segment * SEGMENT_BYTES
        tail = seam
        while tail > 2 and data[tail - 1] == 0x00 and data[tail - 2] != 0xFD:
            tail -= 1
        if data[tail - 2:tail] != b"\xFD\x00":
            fail(path, "missing FD 00 segment notification before 0x%X" % seam)
    return data, segment_count


def convert(source, output, chip):
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
    return validate_lvgm(output, chip)


def main():
    quiet = "--quiet" in sys.argv
    ASSET_DIR.mkdir(parents=True, exist_ok=True)
    VGM_CMP_BUILD.mkdir(parents=True, exist_ok=True)
    optimizer = find_vgm_cmp()
    assets = []
    for prefix, subdir, chip in CHIP_SETS:
        directory = SOURCE_DIR / subdir if subdir else SOURCE_DIR
        for name, filename, loop in TRACKS:
            source = directory / filename
            if not source.exists():
                if prefix == "":
                    fail(source, "music source is missing")
                # An FM set that has no rendition of this track is expected;
                # msx2_audio.c plays the PSG recording for it.
                continue
            validate_vgm(source, chip)
            trimmed = VGM_CMP_BUILD / (prefix + filename + ".trimmed.vgm")
            lead = trim_leadin(source, trimmed, chip)
            candidate = VGM_CMP_BUILD / (prefix + filename + ".optimized.vgm")
            input_path, accepted, rejection = optimize(
                trimmed, candidate, optimizer, chip)
            asset_id = prefix + name
            output = ASSET_DIR / ("music_" + asset_id + ".bin")
            data, segments = convert(input_path, output, chip)
            assets.append({
                "id": asset_id,
                "chip": CHIP_NAME[chip],
                "chip_bit": chip,
                "track": name,
                "source": str(source.relative_to(ROOT)),
                "file": output.name,
                "bytes": len(data),
                "segments": segments,
                "loop": bool(loop),
                "sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                "lead_in_samples": lead,
                "optimized_bytes": candidate.stat().st_size,
                "optimizer_accepted": accepted,
                "optimizer_rejection": rejection,
                "input": str(input_path.relative_to(ROOT)),
            })
            if not quiet:
                print("%-11s %-20s %6d bytes, %d segments%s <- %s"
                      % (CHIP_NAME[chip], asset_id, len(data), segments,
                         "" if not lead else
                         ", trimmed %.2fs of lead-in" % (lead / 44100.0),
                         source.relative_to(ROOT)))

    payload = {
        "format": 2,
        "converter": "MSXzip --simplify --split 16K",
        "optimizer": "vgm_cmp",
        "optimizer_source": str(VGMTOOLS.relative_to(ROOT)),
        "segment_bytes": SEGMENT_BYTES,
        "assets": assets,
    }
    META.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")
    if not quiet:
        print("wrote %s" % META.relative_to(ROOT))


if __name__ == "__main__":
    main()
