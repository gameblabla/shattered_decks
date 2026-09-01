#!/bin/bash
# ─────────────────────────────────────────────────────────────────────────────
#  msx2.sh — build and headlessly verify the MSX2 / NEO-16 cartridge
#
#  Mirrors fmtowns.sh for the MSX2 target.
#
#  WHICH EMULATOR, AND WHY IT MATTERS
#  ----------------------------------
#  NOT the deterministic openmsx-headless backend bundled with MSXgl for
#  executing this compiled C ROM.  Its Z80 core does not
#  implement LD r,(IX+d) / LD (IX+d),r for r != A -- the loads and stores are
#  silently skipped.  SDCC uses IX as its frame pointer and emits those two
#  instructions constantly, so *every* C function with stack locals computes
#  garbage there and the ROM crashes within a few hundred instructions.  Proven
#  with a 20-instruction hand-written test ROM; the same ROM gives the correct
#  answer under real openMSX.  The command layer still uses the bundled build
#  for NEO/VDP analysis and now for deterministic keyboard injection, but the
#  compiled C game remains on real openMSX until that CPU defect is fixed.
#
#  So: real openMSX (20.0-rc1+, which knows the NEO-16 mapper), driven with a
#  Tcl script, no window, RAM dumped at the end.  The project command layer in
#  openmsx/ is used for mapper regression, ROM analysis, and bundled scripted
#  input injection;
#  compiled-C verification still uses real openMSX.  The ROM also stamps its
#  state into RAM (src/msx2/msx2_probe.c); tools/msx2/read_probe.py reads it
#  back.
#
#  Usage:
#      ./msx2.sh build                     # build the ROM
#      ./msx2.sh verify [--seconds N|--frames N] # build, run, read probe
#      ./msx2.sh run    [--seconds N|--frames N] # run without rebuilding
#      ./msx2.sh shot   [--seconds N|--frames N] # PNG of the final frame (VRAM decode)
#      ./msx2.sh shot --keys "2.0:space 2.6:down" # ... after driving the menu
#      ./msx2.sh neo-test                 # NEO-1.2 high-bank regression
#      ./msx2.sh header                   # inspect the built ROM header
#      ./msx2.sh asm <source> [output]     # assemble through openmsx/
#      ./msx2.sh input <script> [frames]  # check/compile/run scripted input
#      ./msx2.sh tool <command> ...       # pass through to openmsx/
#      ./msx2.sh ram                       # RAM/code budget from the link map
# ─────────────────────────────────────────────────────────────────────────────
set -u

cd "$(dirname "$0")"

OPENMSX_TOOL="${OPENMSX_TOOL:-$PWD/openmsx/openmsx}"
OPENMSX="${OPENMSX:-/usr/local/bin/openmsx}"
MSXGL_PATH="${MSXGL_PATH:-MSXgl-main}"
MSX2_BIOS="${MSX2_BIOS:-$MSXGL_PATH/msx2.rom}"
MACHINE="${MSX2_MACHINE:-C-BIOS_MSX2}"
OUT_DIR="src/msx2/out"
ROM="$OUT_DIR/waifu_msx2.rom"
MAP="$OUT_DIR/waifu_msx2.map"
RAM="$OUT_DIR/waifu_msx2.ram"
SHOT="$OUT_DIR/waifu_msx2.png"
VRAM="$OUT_DIR/waifu_msx2.vram"
SHOT_PAGE=0

CMD="${1:-verify}"
[ $# -gt 0 ] && shift

# Long enough that the default key sequence has fired and a duel has started.
SECONDS_RUN=15

die() { echo "msx2.sh: $*" >&2; exit 1; }

need_rom() { [ -f "$ROM" ] || die "no ROM at $ROM -- run ./msx2.sh build"; }
need_emu() {
	[ -x "$OPENMSX" ] || die "openMSX not found at $OPENMSX (set OPENMSX=...)"
}
need_tool() {
	[ -x "$OPENMSX_TOOL" ] || die "openMSX command layer not found at $OPENMSX_TOOL (set OPENMSX_TOOL=...)"
}

# ── Scripted key injection ───────────────────────────────────────────────────
#
# Real openMSX drives the keyboard matrix from Tcl, so a headless run can walk
# the title menu exactly as a player would -- no self-play code in the ROM, and
# the real input path is what gets exercised.  A sequence entry is
# `<seconds>:<key>`; the key is held for 150 ms, which is about nine frames and
# so cannot be missed by the once-per-frame latch.
#
# The default sequence dismisses the attract prompt, moves the cursor down one
# row to BATTLE MODE and confirms, which is how a blind run reaches a duel.
# It starts at six seconds because the title is not listening before then: the
# machine boots, and then the title streams its artwork out of the cartridge
# with interrupts off in 16 KB chunks, during which no key is latched.
KEY_SEQ_DEFAULT="6.0:space 6.8:down 7.6:space"
KEY_SEQ=""

key_matrix() {
	case "$1" in
		space)  echo "8 0x01" ;;
		left)   echo "8 0x10" ;;
		up)     echo "8 0x20" ;;
		down)   echo "8 0x40" ;;
		right)  echo "8 0x80" ;;
		return) echo "7 0x80" ;;
		esc)    echo "7 0x04" ;;
		*)      echo "" ;;
	esac
}

emit_key_script() {
	local entry at key rm
	for entry in $KEY_SEQ; do
		at="${entry%%:*}"
		key="${entry##*:}"
		rm="$(key_matrix "$key")"
		[ -n "$rm" ] || die "unknown key '$key' in key sequence"
		printf 'after time %s { keymatrixdown %s ; after time 0.15 { keymatrixup %s } }\n' \
			"$at" "$rm" "$rm"
	done
}

do_build() { make -f Makefile.msx2 rom || die "build failed"; }
do_mapper_test() { need_tool; "$OPENMSX_TOOL" neo-test || die "NEO mapper regression failed"; }

# Only the original build/run/shot commands consume these wrapper options.
# Analysis and input commands receive their arguments unchanged by the
# openmsx/ command layer.
KEY_SEQ_SET=0
case "$CMD" in
	build|verify|run|shot)
		while [ $# -gt 0 ]; do
			case "$1" in
				--seconds) [ $# -ge 2 ] || die "--seconds requires a value"; SECONDS_RUN="$2"; shift 2 ;;
				--frames)
					[ $# -ge 2 ] || die "--frames requires a value"
					case "$2" in *[!0-9]*|'') die "--frames must be a non-negative integer" ;; esac
					SECONDS_RUN=$(( (10#$2 + 59) / 60 )); shift 2 ;;
				--out)     [ $# -ge 2 ] || die "--out requires a path"; SHOT="$2";        shift 2 ;;
				--page)    [ $# -ge 2 ] || die "--page requires 0 or 1"; SHOT_PAGE="$2";  shift 2 ;;
				--keys)    [ $# -ge 2 ] || die "--keys requires a sequence like '2.0:space 2.6:down'"
				           KEY_SEQ="$2"; KEY_SEQ_SET=1; shift 2 ;;
				--no-keys) KEY_SEQ=""; KEY_SEQ_SET=1; shift ;;
				*) echo "unknown option: $1" >&2; exit 2 ;;
			esac
		done
		# A run wants to reach a duel, so it presses through the menu; a shot
		# photographs whatever screen it lands on and presses nothing.
		if [ "$KEY_SEQ_SET" = "0" ] && [ "$CMD" != "shot" ]; then
			KEY_SEQ="$KEY_SEQ_DEFAULT"
		fi
		;;
esac

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
$(emit_key_script)
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
	neo-test|mapper-test)
		do_mapper_test
		;;

	header|rominfo)
		need_rom; need_tool
		"$OPENMSX_TOOL" header "$ROM"
		;;

	asm|assemble)
		need_tool
		"$OPENMSX_TOOL" asm "$@"
		;;

	input)
		[ $# -gt 0 ] || die "usage: ./msx2.sh input <script> [frames] [options]"
		need_tool
		case "$1" in
			check|validate|compile|build)
				"$OPENMSX_TOOL" input "$@"
				;;
			*)
				need_rom
				# input run is handled by the bundled deterministic backend.
				# Direct-cart keeps this path independent of the CBIOS startup.
				"$OPENMSX_TOOL" input run "$ROM" "$@" --direct-cart
				;;
		esac
		;;

	cov)
		need_tool
		if [ "${1:-}" = "summary" ] || [ "${1:-}" = "report" ]; then
			shift
			"$OPENMSX_TOOL" cov summary "$@"
			exit $?
		fi
		need_rom
		COV_FILE="${1:-$OUT_DIR/waifu_msx2.cov}"
		[ $# -eq 0 ] || shift
		COV_FRAMES="${1:-600}"
		[ $# -eq 0 ] || shift
		# This backend is mapper/VDP analysis only; STATUS.md still requires
		# real openMSX for executing the compiled C game.
		"$OPENMSX_TOOL" cov "$ROM" "$COV_FILE" "$COV_FRAMES" \
			--bios "$MSX2_BIOS" --direct-cart "$@" || exit $?
		"$OPENMSX_TOOL" cov summary "$COV_FILE"
		;;

	tool)
		need_tool
		[ $# -gt 0 ] || die "usage: ./msx2.sh tool <openmsx-command> ..."
		"$OPENMSX_TOOL" "$@"
		;;

	build)
		do_build
		;;

	verify)
		do_build
		do_mapper_test
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
		# openMSX's own screenshot needs a renderer, and its GL renderer hands
		# back an empty frame under Xvfb -- so the target photographs itself the
		# way the other consoles do: dump VRAM, decode it here.  No display
		# server, and it can show the hidden page as well as the visible one.
		local_script="$OUT_DIR/shot.tcl"
		mkdir -p "$OUT_DIR"
		cat > "$local_script" <<EOF
set renderer none
set throttle off
$(emit_key_script)
after time $SECONDS_RUN {
    set f [open "$PWD/$VRAM" w]
    fconfigure \$f -translation binary
    puts -nonewline \$f [debug read_block {physical VRAM} 0 131072]
    close \$f
    exit 0
}
EOF
		rm -f "$VRAM" "$SHOT"
		SDL_VIDEODRIVER=dummy "$OPENMSX" -machine "$MACHINE" \
			-cart "$ROM" -romtype NEO-16 -script "$local_script" 2>&1 | head -10
		[ -f "$VRAM" ] || die "no VRAM dump written -- the emulator never reached the timer"
		python3 tools/msx2/vram_png.py "$VRAM" "$SHOT" --page "$SHOT_PAGE" --scale 2
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
