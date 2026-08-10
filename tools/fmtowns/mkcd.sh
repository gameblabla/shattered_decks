#!/bin/sh
# Builds a mixed-mode CD image: the ISO9660 data track this project boots
# from, followed by one Red Book audio track per file given on the command
# line, as output.bin + output.cue.
#
#   ./tools/mkcd.sh song.wav [song2.wav ...]
#
# With no arguments it builds the plain data-only output.iso instead, which
# is what run.sh used before CD-DA support existed - the payload's music
# then falls back to streaming CD/MUSIC.PCM through the YM2612 DAC (see
# src/common/pcmstream.h and the player selection in src/main.c).
#
# Why a .bin/.cue rather than an .iso: an ISO file is by definition a
# single data track, so there is nowhere for an audio track to live.
# TOWNSEMU reads the layout from the .cue (TOWNSEMU/src/discimg/
# discimg.cpp, OpenCUE) and reports it to the guest as the disc's table of
# contents, which is what fmt_cdda_read_toc() then reads back.
#
# Layout notes, all of which TOWNSEMU's CUE parser is picky about:
#   - Track 1 must start at 00:00:00 (OpenCUEPostProcess() rejects
#     anything else outright).
#   - Every track lives in the one binary, concatenated in track order at
#     its own sector size: 2048 bytes/sector for the MODE1 data track,
#     2352 for audio. TryAnalyzeTracksWithProbablyCorrectInterpretation()
#     works out each track's byte offset from the INDEX times and those
#     sector sizes, so the INDEX time of each track has to be exactly the
#     running sector total of everything before it.
#   - The last track's byte length must come out to a whole number of its
#     own sectors, hence the padding below.
#   - No PREGAP: gap sectors are not stored in the binary, and adding one
#     would shift every INDEX time without changing the file.
# CUE INDEX times count from the first sector of the disc (LBA 0 =
# 00:00:00), i.e. they do NOT include the 150-frame Red Book pregap - that
# offset appears only later, in the MSF addresses the drive reports and
# accepts (see fmt_cdda_msf_to_lba()).
set -e

# Resolve alongside this script regardless of the caller's cwd -- unlike
# FMTOWNSCD_EXAMPLE_Cube's original (invoked from its own repo root), this
# copy is invoked with cwd = build/fmtowns (see Makefile.fmtowns), so
# "tools/..." relative paths would not resolve.
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)

SECTOR_DATA=2048
SECTOR_AUDIO=2352

# tools/iso_sort.txt gives CD/SYSTEM.BIN a high -sort weight so mkisofs
# places it at the front of the disc. That file is the payload the IPL
# loads by raw LBA before anything else runs, and the boot ROM's disk
# read takes a 16-bit sector number, so it has to sit below LBA 65536;
# left to mkisofs's default (directory order) it lands after
# CD/MUSIC.PCM, where a large enough soundtrack would push it out of
# reach. The sort file's format has no room for comments, hence this one.
ISO_OPTS="-iso-level 1 -pad -N -V FROG_FEAST -D -input-charset ASCII -sysid Win32
          -sort $SCRIPT_DIR/iso_sort.txt"

# Build the ISO9660 filesystem, then write the IPL into its System Area
# and patch the payload's real location into it (tools/inject_ipl.py).
#
# Note there is no `mkisofs -G IPL.BIN` here. Letting mkisofs place the
# boot image is a build-order trap: the IPL has to know the LBA of
# CD/SYSTEM.BIN, which only exists once the filesystem has been laid
# out, so the location either gets guessed from a probe ISO built
# separately (and goes stale the moment the two disagree) or - as here -
# gets patched in afterwards, from the finished image itself. mkisofs -G
# also silently truncates its boot image to the 32768-byte System Area.
build_iso() {
    # shellcheck disable=SC2086
    mkisofs $ISO_OPTS -o "$1" CD
    "$SCRIPT_DIR/inject_ipl.py" "$1" IPL.BIN SYSTEM.BIN
}

if [ $# -eq 0 ]; then
    build_iso output.iso
    echo "Built output.iso (data only, no CD-DA)"
    exit 0
fi

for f in "$@"; do
    if [ ! -f "$f" ]; then
        echo "mkcd.sh: no such audio file: $f" >&2
        exit 1
    fi
done

rm -f output.bin output.cue
build_iso output.iso

iso_bytes=$(stat -c%s output.iso)
if [ $((iso_bytes % SECTOR_DATA)) -ne 0 ]; then
    echo "mkcd.sh: output.iso is not a whole number of $SECTOR_DATA-byte sectors" >&2
    exit 1
fi
sectors=$((iso_bytes / SECTOR_DATA))

cp output.iso output.bin

{
    echo "FILE \"output.bin\" BINARY"
    echo "  TRACK 01 MODE1/$SECTOR_DATA"
    echo "    INDEX 01 00:00:00"
} > output.cue

track=1
for f in "$@"; do
    track=$((track + 1))

    # 44.1kHz 16-bit signed little-endian stereo, which is the only thing
    # a Red Book audio track can hold - the drive's own DAC plays it, so
    # unlike the PCM path there is no choice of rate to make here.
    tmp=$(mktemp -t mkcd-track-XXXXXX.raw)
    sox "$f" -r 44100 -c 2 -b 16 -e signed-integer -t raw "$tmp"

    # Pad the tail out to a whole sector with silence.
    bytes=$(stat -c%s "$tmp")
    pad=$(( (SECTOR_AUDIO - bytes % SECTOR_AUDIO) % SECTOR_AUDIO ))
    if [ "$pad" -ne 0 ]; then
        head -c "$pad" /dev/zero >> "$tmp"
        bytes=$((bytes + pad))
    fi

    msf=$(printf '%02d:%02d:%02d' \
        $((sectors / 75 / 60)) $((sectors / 75 % 60)) $((sectors % 75)))
    {
        printf '  TRACK %02d AUDIO\n' "$track"
        echo "    INDEX 01 $msf"
    } >> output.cue

    cat "$tmp" >> output.bin
    rm -f "$tmp"

    echo "Track $track: $f -> $((bytes / SECTOR_AUDIO)) sectors at $msf"
    sectors=$((sectors + bytes / SECTOR_AUDIO))
done

rm -f output.iso
echo "Built output.bin + output.cue ($sectors sectors total)"
