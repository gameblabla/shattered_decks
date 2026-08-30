#!/bin/sh
# Headless CD32X capture through the bundled BlastEm build.
#
#   scripts/cd32x/blastem_headless_capture.sh <cue> <out.png> <frames> [input-script]
#
# Runs CD32X/blastem-headless on a Sega CD + 32X disc image for <frames>
# video frames and writes the final frame to <out.png>.
#
# Environment:
#   BLASTEM               emulator binary (default: CD32X/blastem-headless)
#   BLASTEM_BIOS_DIR      BIOS directory (default: the emulator's directory)
#   BLASTEM_HOME          config HOME for the run (default: build/cd32x/blastem-home)
#   BLASTEM_TIMEOUT       wall-clock limit in seconds (default: 900)
#   BLASTEM_LOG           emulator stdout/stderr log (default: <out.png>.log)
#   BLASTEM_WAV           if set, also write mixed audio to this WAVE file
#   BLASTEM_MKV           if set, also write a ZMBV/PCM Matroska capture
#   CD32X_BIOS_START_END  last frame of the auto-generated Sega CD BIOS-window
#                         START presses (default: 5400).  A START tapped after
#                         the BIOS window lands in the game's own menu, so lower
#                         this when capturing an early boot scene.
#   CD32X_NO_BIOS_SKIP    set to 1 to suppress the auto-generated START presses
#                         (only when the supplied input script has its own).
set -eu

usage() {
    echo "usage: $0 <cue> <out.png> <frames> [input-script]" >&2
    exit 2
}

[ $# -ge 3 ] || usage

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/../.." && pwd)

cue=$1
out=$2
frames=$3
user_input=${4-}

case $cue in /*) ;; *) cue=$repo_root/$cue ;; esac
case $out in /*) ;; *) out=$PWD/$out ;; esac
[ -n "$user_input" ] && case $user_input in /*) ;; *) user_input=$PWD/$user_input ;; esac

blastem=${BLASTEM:-$repo_root/CD32X/blastem-headless}
case $blastem in /*) ;; *) blastem=$repo_root/$blastem ;; esac
bios_dir=${BLASTEM_BIOS_DIR:-$(dirname -- "$blastem")}
home_dir=${BLASTEM_HOME:-$repo_root/build/cd32x/blastem-home}
timeout_s=${BLASTEM_TIMEOUT:-900}
log=${BLASTEM_LOG:-$out.log}
bios_start_end=${CD32X_BIOS_START_END:-5400}

[ -x "$blastem" ] || { echo "error: emulator not found or not executable: $blastem" >&2; exit 1; }
[ -f "$cue" ] || { echo "error: disc image not found: $cue (build it: make -f Makefile.cd32x)" >&2; exit 1; }
[ -n "$user_input" ] && [ ! -f "$user_input" ] && { echo "error: input script not found: $user_input" >&2; exit 1; }

# BIOS images must sit next to the emulator (or in BLASTEM_BIOS_DIR); the
# config below is written with absolute paths, so the run never depends on the
# user's own ~/.config/blastem.
missing=
for f in bios_CD_U.bin bios_CD_E.bin bios_CD_J.bin 32X_M_BIOS.bin 32X_S_BIOS.bin 32X_G_BIOS.bin; do
    [ -f "$bios_dir/$f" ] || missing="$missing $f"
done
[ -z "$missing" ] || { echo "error: missing BIOS file(s) in $bios_dir:$missing" >&2; exit 1; }

mkdir -p "$home_dir/.config/blastem" "$(dirname -- "$out")"
cat > "$home_dir/.config/blastem/blastem.cfg" <<CFG
system {
  sync_source audio
  ram_init zero
  default_region U
  force_region off
  megawifi off
  model md1va3
  scd_bios_us $bios_dir/bios_CD_U.bin
  scd_bios_eu $bios_dir/bios_CD_E.bin
  scd_bios_jp $bios_dir/bios_CD_J.bin
  s32x_main_bios $bios_dir/32X_M_BIOS.bin
  s32x_sub_bios $bios_dir/32X_S_BIOS.bin
  s32x_68k_bios $bios_dir/32X_G_BIOS.bin
}
CFG

# Build the effective input script: BIOS-window STARTs first, then the caller's.
input=$home_dir/effective.input
: > "$input"
if [ "${CD32X_NO_BIOS_SKIP:-0}" != "1" ]; then
    f=600
    while [ "$f" -le "$bios_start_end" ]; do
        echo "${f}f:P" >> "$input"
        f=$((f + 600))
    done
fi
[ -n "$user_input" ] && cat "$user_input" >> "$input"

set -- -m 32xcd -b "$frames" -i "$input" -p "$out"
[ -n "${BLASTEM_WAV:-}" ] && set -- "$@" -w "$BLASTEM_WAV"
[ -n "${BLASTEM_MKV:-}" ] && set -- "$@" --video "$BLASTEM_MKV"

echo "capture: $(basename -- "$cue") -> $out  (${frames} frames, BIOS STARTs <= ${bios_start_end})" >&2
rm -f "$out"
set +e
HOME=$home_dir timeout "$timeout_s" "$blastem" "$@" "$cue" > "$log" 2>&1
status=$?
set -e

if [ ! -f "$out" ]; then
    echo "error: no screenshot produced (exit $status); tail of $log:" >&2
    tail -n 20 "$log" >&2
    exit 1
fi
[ "$status" -eq 124 ] && echo "warning: emulator hit the ${timeout_s}s timeout" >&2
echo "wrote $out (log: $log)" >&2
