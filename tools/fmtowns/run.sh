#!/bin/sh
# Build (if needed) and boot the FM TOWNS Marty port in Tsugaru_CUI.elf.
#
# Usage:
#   tools/fmtowns/run.sh          # build+run the mixed-mode CD-DA disc (default)
#   tools/fmtowns/run.sh --iso    # build+run the plain data-only disc (no music)
#   tools/fmtowns/run.sh --no-build   # skip the make step, just boot what's there
#
# Needs a real GL-capable display (X11/Wayland) -- this is the same
# Tsugaru_CUI.elf FMTOWNSCD_EXAMPLE_Cube/run.sh uses. For headless
# screenshot-only verification (no display available), see the xvfb-run
# recipe in src/platform/fmtowns/STATUS.md instead.

set -e

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

TSUGARU="FMTOWNSCD_EXAMPLE_Cube/Tsugaru_CUI.elf"
MARTY_ROM="FMTOWNSCD_EXAMPLE_Cube/MARTY_ROM"

TARGET=build/fmtowns/output.cue
MAKE_TARGET=all
DO_BUILD=1

for arg in "$@"; do
    case "$arg" in
        --iso)       TARGET=build/fmtowns/output.iso; MAKE_TARGET=iso ;;
        --no-build)  DO_BUILD=0 ;;
        *) echo "unknown option: $arg" >&2; exit 1 ;;
    esac
done

if [ "$DO_BUILD" = "1" ]; then
    make -f Makefile.fmtowns "$MAKE_TARGET"
fi

exec "$TSUGARU" "$MARTY_ROM/" -TOWNSTYPE MARTY -CD "$TARGET" -NORMALFD -DONTUSEFPU -AUTOSCALE
