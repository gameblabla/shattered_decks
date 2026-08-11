#!/bin/sh
# Boot build/fmtowns/output.cue in Tsugaru_CUI headlessly and capture frames.
#
# Tsugaru_CUI needs a GL-capable display, so this runs under xvfb and drives
# the emulator's interactive machine monitor over stdin (see STATUS.md's
# "Headless emulator verification" section, and note the caveat there: a
# black screenshot alone does not prove a boot failure in this environment).
#
# Usage: headless_shot.sh OUT_PREFIX [SCRIPT_FILE]
#
#   OUT_PREFIX    frames are written to $OUT_PREFIX0.png, ...1.png etc, in
#                 whatever order SCRIPT_FILE's SS commands appear.
#   SCRIPT_FILE   lines fed to the monitor, one per line, in order.  A line
#                 of the form "SLEEP n" is consumed by this script (it waits
#                 n seconds of wall time instead of forwarding the line), so
#                 a script can interleave waits with monitor commands.  With
#                 no SCRIPT_FILE the default is "boot for 12s, take one
#                 shot, quit".
#
# Every run is timeout -s KILL guarded: plain SIGTERM does not reliably stop
# a wedged Tsugaru_CUI here.
set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
TSUGARU=$ROOT/FMTOWNSCD_EXAMPLE_Cube/Tsugaru_CUI.elf
# Overridable so a build can be profiled/tested against a non-Marty model
# too (see STATUS.md's "Runtime Marty / standard-model detection" and the
# UX/MX-vs-Marty perf-parity investigation): FMTOWNS_TOWNSTYPE must be a
# real Tsugaru model string ("FMTOWNS" is NOT one -- it silently falls back
# to TOWNSTYPE_UNKNOWN, see STATUS.md), and FMTOWNS_ROM must point at a ROM
# directory matching that model class (MARTY_ROM only works for -TOWNSTYPE
# MARTY; a plain FM TOWNS ROM dump is needed for MX/UX/etc).
ROM=${FMTOWNS_ROM:-$ROOT/FMTOWNSCD_EXAMPLE_Cube/MARTY_ROM/}
TOWNSTYPE=${FMTOWNS_TOWNSTYPE:-MARTY}
MEMSIZE=${FMTOWNS_MEMSIZE:-2}
CD=${FMTOWNS_CD:-$ROOT/build/fmtowns/output.cue}

OUT=${1:?usage: headless_shot.sh OUT_PREFIX [SCRIPT_FILE]}
SCRIPT=${2:-}
EXTRA_CMOS=${EXTRA_CMOS:-}
EXTRA_TSUGARU=${EXTRA_TSUGARU:-}
RUN_TIMEOUT=${RUN_TIMEOUT:-180}

# Default script: let it boot, grab one frame, quit.
if [ -z "$SCRIPT" ]; then
	SCRIPT=$(mktemp)
	printf 'SLEEP 12\nSS %s0.png\nSLEEP 1\nQUIT\n' "$OUT" > "$SCRIPT"
fi

feed() {
	while IFS= read -r line; do
		case $line in
		SLEEP\ *) sleep "${line#SLEEP }" ;;
		''|\#*)   ;;
		*)        printf '%s\n' "$line" ;;
		esac
	done < "$SCRIPT"
	# Give the VM a moment to flush the last command before stdin closes.
	sleep 2
}

feed | xvfb-run -a timeout -s KILL "$RUN_TIMEOUT" \
	"$TSUGARU" "$ROM" \
	-TOWNSTYPE "$TOWNSTYPE" -MEMSIZE "$MEMSIZE" -CD "$CD" $EXTRA_CMOS \
	-NORMALFD -DONTUSEFPU -NOWAITBOOT $EXTRA_TSUGARU \
	-GAMEPORT0 KEY -KEYBOARD DIRECT
