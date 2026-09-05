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
#      ./msx2.sh verify [--seconds N|--frames N] # build the SOAK rom, run, read probe
#      ./msx2.sh run    [--seconds N|--frames N] # run without rebuilding
#      ./msx2.sh shot   [--seconds N|--frames N] # PNG of the final frame (VRAM decode)
#      ./msx2.sh shot --keys "2.0:space 2.6:down" # ... after driving the menu
#      ./msx2.sh neo-test                 # NEO-1.2 high-bank regression
#      ./msx2.sh header                   # inspect the built ROM header
#      ./msx2.sh asm <source> [output]     # assemble through openmsx/
#      ./msx2.sh input <script> [frames]  # check/compile/run scripted input
#      ./msx2.sh tool <command> ...       # pass through to openmsx/
#      ./msx2.sh ram                       # RAM/code budget from the link map
#      ./msx2.sh trace --times "6.0 7.0" # capture a transient sequence
# ─────────────────────────────────────────────────────────────────────────────
set -u

cd "$(dirname "$0")"

OPENMSX_TOOL="${OPENMSX_TOOL:-$PWD/openmsx/openmsx}"
OPENMSX="${OPENMSX:-/usr/local/bin/openmsx}"
MSXGL_PATH="${MSXGL_PATH:-MSXgl-main}"
MSX2_BIOS="${MSX2_BIOS:-$MSXGL_PATH/msx2.rom}"
MACHINE="${MSX2_MACHINE:-C-BIOS_MSX2}"
# Extra hardware to plug in, as openMSX extension names separated by spaces.
# The sound-chip probe is the reason this exists: MSX2_EXT=fmpac exercises the
# MSX-MUSIC path and MSX2_EXT=audio the MSX-AUDIO one, neither of which the
# bare machine has.  Deliberately unquoted where it is used, so it can be empty.
EXTENSIONS=""
for ext in ${MSX2_EXT:-}; do EXTENSIONS="$EXTENSIONS -ext $ext"; done
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
# How long a scripted key is held, in emulated seconds.  The V-blank handler
# latches the matrix and accumulates edges (src/msx2/msx2_input.c), so a press
# only has to outlast one frame -- but a script that needs a longer one can say
# so: KEY_HOLD=0.4 ./msx2.sh shot ...
KEY_HOLD="${KEY_HOLD:-0.15}"

key_matrix() {
	case "$1" in
		space)  echo "8 0x01" ;;
		left)   echo "8 0x10" ;;
		up)     echo "8 0x20" ;;
		down)   echo "8 0x40" ;;
		right)  echo "8 0x80" ;;
		return) echo "7 0x80" ;;
		esc)    echo "7 0x04" ;;
		# Every printable key the game reads, in the order the matrix reports
		# each row -- so a scripted run can type a name or a continue code.
		[0-7])  echo "0 $(printf '0x%02x' $((1 << $1)))" ;;
		8)      echo "1 0x01" ;;
		9)      echo "1 0x02" ;;
		[ab])   echo "2 $(printf '0x%02x' $((1 << (6 + $(printf '%d' \'$1) - 97))))" ;;
		[c-j])  echo "3 $(printf '0x%02x' $((1 << ($(printf '%d' \'$1) - 99))))" ;;
		[k-r])  echo "4 $(printf '0x%02x' $((1 << ($(printf '%d' \'$1) - 107))))" ;;
		[s-z])  echo "5 $(printf '0x%02x' $((1 << ($(printf '%d' \'$1) - 115))))" ;;
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
		printf 'after time %s { keymatrixdown %s ; after time %s { keymatrixup %s } }\n' \
			"$at" "$rm" "$KEY_HOLD" "$rm"
	done
}

do_build() { make -f Makefile.msx2 rom || die "build failed"; }

# The blind probe only means anything on a ROM that plays itself: the shipping
# build waits for a hand on the joystick, so a `verify` against it would sit on
# turn 1 for the whole run and report a hang that is really an empty chair.
# `make soak` is the same ROM with the player's turn handed to the COM's AI.
do_soak_build() { make -f Makefile.msx2 soak || die "soak build failed"; }
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
	trace)
		# Trace uses the real openMSX executable and records the exact bytes the
		# transient regressions care about: full VRAM, the sprite attribute table,
		# and the RAM probe at each requested emulated time.  The PNG conversion
		# happens after the machine exits so a missing frame is a hard error.
		need_rom; need_emu
		TRACE_TIMES=""
		while [ $# -gt 0 ]; do
			case "$1" in
				--times) [ $# -ge 2 ] || die "--times requires space-separated seconds"; TRACE_TIMES="$2"; shift 2 ;;
				--out) [ $# -ge 2 ] || die "--out requires a directory"; TRACE_OUT="$2"; shift 2 ;;
				--keys) [ $# -ge 2 ] || die "--keys requires a sequence"; KEY_SEQ="$2"; KEY_SEQ_SET=1; shift 2 ;;
				*) die "unknown trace option: $1" ;;
			esac
		done
		[ -n "$TRACE_TIMES" ] || die "usage: ./msx2.sh trace --times '6.0 7.0' [--out dir]"
		TRACE_OUT="${TRACE_OUT:-$OUT_DIR/trace}"
		mkdir -p "$TRACE_OUT"
		TRACE_SCRIPT="$TRACE_OUT/trace.tcl"
		TRACE_INDEX=0
		TRACE_TCL="set renderer none\nset throttle off\n"
		[ "$KEY_SEQ_SET" = "0" ] && KEY_SEQ="$KEY_SEQ_DEFAULT"
		TRACE_TCL="$TRACE_TCL$(emit_key_script)\n"
		for TRACE_TIME in $TRACE_TIMES; do
			TRACE_TAG=$(printf '%s' "$TRACE_TIME" | tr '.-' '__')
			TRACE_TCL="$TRACE_TCL$(cat <<EOF
after time $TRACE_TIME {
    set f [open "$PWD/$TRACE_OUT/frame_${TRACE_TAG}.vram" w]
    fconfigure \$f -translation binary
    puts -nonewline \$f [debug read_block {physical VRAM} 0 131072]
    close \$f
    set f [open "$PWD/$TRACE_OUT/frame_${TRACE_TAG}.sat" w]
    fconfigure \$f -translation binary
    puts -nonewline \$f [debug read_block {physical VRAM} 64000 128]
    close \$f
    set f [open "$PWD/$TRACE_OUT/frame_${TRACE_TAG}.ram" w]
    fconfigure \$f -translation binary
    puts -nonewline \$f [debug read_block memory 0 65536]
    close \$f
    set f [open "$PWD/$TRACE_OUT/frame_${TRACE_TAG}.reg" w]
    fconfigure \$f -translation binary
    puts -nonewline \$f [debug read_block {VDP regs} 0 64]
    close \$f
    set f [open "$PWD/$TRACE_OUT/frame_${TRACE_TAG}.pal" w]
    fconfigure \$f -translation binary
    puts -nonewline \$f [debug read_block {VDP palette} 0 32]
    close \$f
}
EOF
)\n"
			TRACE_INDEX=$((TRACE_INDEX + 1))
		done
		TRACE_TCL="$TRACE_TCL$(cat <<EOF
after time [expr {[lindex [lsort -real [list $TRACE_TIMES]] end] + 0.25}] { exit 0 }
EOF
)"
		# shellcheck disable=SC2059
		printf '%b' "$TRACE_TCL" > "$TRACE_SCRIPT"
		SDL_VIDEODRIVER=dummy "$OPENMSX" -machine "$MACHINE" $EXTENSIONS \
			-cart "$ROM" -romtype NEO-16 -script "$TRACE_SCRIPT" 2>&1 | head -20
		python3 - "$TRACE_OUT" "$TRACE_TIMES" <<'PY'
import json, sys
from pathlib import Path
out = Path(sys.argv[1])
frames = []
for time in sys.argv[2].split():
    tag = time.replace('.', '_').replace('-', '__')
    vram = out / ('frame_%s.vram' % tag)
    sat = out / ('frame_%s.sat' % tag)
    ram = out / ('frame_%s.ram' % tag)
    if not vram.exists() or not sat.exists() or not ram.exists():
        raise SystemExit('trace frame %s was not written' % time)
    frames.append({'time': float(time), 'vram': str(vram), 'sat': str(sat), 'ram': str(ram),
                   'png': str(out / ('frame_%s.png' % tag))})
(out / 'manifest.json').write_text(json.dumps({'frames': frames}, indent=2))
PY
		# SCREEN 10 IS THE SAME BYTES READ DIFFERENTLY.
		# The MSX2+ cartridge puts its picture screens in YJK+YAE, and a VRAM
		# dump cannot say so on its own -- decoding one as GRAPHIC 7 gives a
		# fully-formed picture in wrong colours, which reads as a rendering bug
		# that is not there.  R#25 bit 3 is the machine's own answer, and the
		# palette beside it is what the YAE half of every byte means.
		for TRACE_TIME in $TRACE_TIMES; do
			TRACE_TAG=$(printf '%s' "$TRACE_TIME" | tr '.-' '__')
			TRACE_MODE=""
			if [ -f "$TRACE_OUT/frame_${TRACE_TAG}.reg" ] &&
			   [ "$(python3 -c "import sys;d=open(sys.argv[1],'rb').read();print((d[25]>>3)&1)" \
			        "$TRACE_OUT/frame_${TRACE_TAG}.reg")" = "1" ]; then
				TRACE_MODE="--yjk --palette $TRACE_OUT/frame_${TRACE_TAG}.pal"
			fi
			python3 tools/msx2/vram_png.py "$TRACE_OUT/frame_${TRACE_TAG}.vram" \
				"$TRACE_OUT/frame_${TRACE_TAG}.png" --page 0 --scale 1 --sprites \
				$TRACE_MODE
		done
		exit 0
		;;
esac

# Run the cartridge blind for N emulated seconds, then dump all 64 KiB of
# CPU-visible memory.  `set renderer none` keeps it windowless; SDL_VIDEODRIVER
# is set as well so it works over a bare ssh session.
do_run() {
	need_rom; need_emu
	local script="$OUT_DIR/run.tcl"
	mkdir -p "$OUT_DIR"
	rm -f "$RAM"
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
	SDL_VIDEODRIVER=dummy "$OPENMSX" -machine "$MACHINE" $EXTENSIONS \
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
		do_soak_build
		do_mapper_test
		do_run
		echo
		echo "── blind-play probe after ${SECONDS_RUN}s of emulated time ──────────"
		python3 tools/msx2/read_probe.py "$RAM" || die "probe check failed"
		# WHAT IS LEFT IN out/ IS THE SOAK ROM, NOT THE GAME.  It plays itself,
		# which means a `shot` or a `run` straight after a `verify` photographs a
		# build that boots into a duel and ignores the keyboard -- and the
		# screenshot looks exactly like a game that has lost its title screen.
		# Rebuild before photographing anything.
		echo
		echo "note: src/msx2/out holds the SOAK rom now; run './msx2.sh build'"
		echo "      before 'shot' or 'run', or you will photograph the self-play build."
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
		SDL_VIDEODRIVER=dummy "$OPENMSX" -machine "$MACHINE" $EXTENSIONS \
			-cart "$ROM" -romtype NEO-16 -script "$local_script" 2>&1 | head -10
		[ -f "$VRAM" ] || die "no VRAM dump written -- the emulator never reached the timer"
		python3 tools/msx2/vram_png.py "$VRAM" "$SHOT" --page "$SHOT_PAGE" --scale 2 --sprites
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
