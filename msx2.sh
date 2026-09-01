#!/bin/bash
# ─────────────────────────────────────────────────────────────────────────────
#  msx2.sh — build and headlessly verify the MSX2 / NEO-16 cartridge
#
#  Mirrors fmtowns.sh for the MSX2 target.
#
#  WHICH EMULATOR, AND WHY IT MATTERS
#  ----------------------------------
#  NOT the openmsx-headless build bundled with MSXgl.  Its Z80 core does not
#  implement LD r,(IX+d) / LD (IX+d),r for r != A -- the loads and stores are
#  silently skipped.  SDCC uses IX as its frame pointer and emits those two
#  instructions constantly, so *every* C function with stack locals computes
#  garbage there and the ROM crashes within a few hundred instructions.  Proven
#  with a 20-instruction hand-written test ROM; the same ROM gives the correct
#  answer under real openMSX.  Use that bundled build only for its NEO mapper
#  and SCREEN 7/8 capture experiments, never to run this game.
#
#  So: real openMSX (20.0-rc1+, which knows the NEO-16 mapper), driven with a
#  Tcl script, no window, RAM dumped at the end.  Since it cannot inject input
#  either, the ROM drives itself and stamps its state into RAM
#  (src/msx2/msx2_probe.c); tools/msx2/read_probe.py reads it back.
#
#  Usage:
#      ./msx2.sh build                     # build the ROM
#      ./msx2.sh verify [--seconds N]      # build, run blind, read the probe
#      ./msx2.sh run    [--seconds N]      # run without rebuilding
#      ./msx2.sh shot   [--seconds N]      # PNG of the final frame
#      ./msx2.sh ram                       # RAM/code budget from the link map
# ─────────────────────────────────────────────────────────────────────────────
set -u

cd "$(dirname "$0")"

OPENMSX="${OPENMSX:-/usr/local/bin/openmsx}"
MACHINE="${MSX2_MACHINE:-C-BIOS_MSX2}"
OUT_DIR="src/msx2/out"
ROM="$OUT_DIR/waifu_msx2.rom"
MAP="$OUT_DIR/waifu_msx2.map"
RAM="$OUT_DIR/waifu_msx2.ram"
SHOT="$OUT_DIR/waifu_msx2.png"

CMD="${1:-verify}"
[ $# -gt 0 ] && shift

SECONDS_RUN=10
while [ $# -gt 0 ]; do
	case "$1" in
		--seconds) SECONDS_RUN="$2"; shift 2 ;;
		--out)     SHOT="$2";        shift 2 ;;
		*) echo "unknown option: $1" >&2; exit 2 ;;
	esac
done

die() { echo "msx2.sh: $*" >&2; exit 1; }

need_rom() { [ -f "$ROM" ] || die "no ROM at $ROM -- run ./msx2.sh build"; }
need_emu() {
	[ -x "$OPENMSX" ] || die "openMSX not found at $OPENMSX (set OPENMSX=...)"
}

do_build() { make -f Makefile.msx2 rom || die "build failed"; }

# Run the cartridge blind for N emulated seconds, then dump all 64 KiB of
# CPU-visible memory.  `set renderer none` keeps it windowless; SDL_VIDEODRIVER
# is set as well so it works over a bare ssh session.
do_run() {
	need_rom; need_emu
	local script="$OUT_DIR/run.tcl"
	mkdir -p "$OUT_DIR"
	cat > "$script" <<EOF
set renderer none
set throttle off
after time $SECONDS_RUN {
    set f [open "$PWD/$RAM" w]
    fconfigure \$f -translation binary
    puts -nonewline \$f [debug read_block memory 0 65536]
    close \$f
    exit 0
}
EOF
	SDL_VIDEODRIVER=dummy "$OPENMSX" -machine "$MACHINE" \
		-cart "$ROM" -romtype NEO-16 -script "$script" 2>&1 |
		grep -vE "^$" | head -20
	[ -f "$RAM" ] || die "no RAM dump written -- the emulator never reached the timer"
}

case "$CMD" in
	build)
		do_build
		;;

	verify)
		do_build
		do_run
		echo
		echo "── blind-play probe after ${SECONDS_RUN}s of emulated time ──────────"
		python3 tools/msx2/read_probe.py "$RAM" || die "probe check failed"
		;;

	run)
		do_run
		python3 tools/msx2/read_probe.py "$RAM"
		;;

	shot)
		need_rom; need_emu
		local_script="$OUT_DIR/shot.tcl"
		cat > "$local_script" <<EOF
after time $SECONDS_RUN {
    screenshot -raw "$PWD/$SHOT"
    exit 0
}
EOF
		SDL_VIDEODRIVER=dummy "$OPENMSX" -machine "$MACHINE" \
			-cart "$ROM" -romtype NEO-16 -script "$local_script" 2>&1 | head -10
		[ -f "$SHOT" ] && echo "wrote $SHOT" || die "no screenshot written"
		;;

	ram)
		[ -f "$MAP" ] || die "no map at $MAP -- run ./msx2.sh build"
		python3 tools/msx2/report_budget.py "$MAP"
		;;

	*)
		sed -n '2,32p' "$0"
		exit 2
		;;
esac
