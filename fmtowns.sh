#!/bin/sh
# Compile and test the FM TOWNS Marty port.
#
# One entry point for everything this port gets built for, so the flags that
# matter (which make target, which emulator switches, which debug define)
# live here instead of being retyped:
#
#   ./fmtowns.sh build            build the mixed-mode CD-DA disc (default)
#   ./fmtowns.sh build --iso      build the data-only disc, no music tracks
#   ./fmtowns.sh run              build, then boot it in Tsugaru on your display
#   ./fmtowns.sh test             build a debug disc, boot it headlessly, and
#                                 report what each screenshot caught: game
#                                 frame, CD-DA track, and the frame time split
#                                 into game step vs present
#   ./fmtowns.sh profile [hand|board]
#                                 the same capture, but parked on ONE scene --
#                                 this is the mode to use when comparing two
#                                 builds; see the notes below.  `hand' (the
#                                 default) is the duel's usual view; `board'
#                                 is the top-down view the software 3-D
#                                 rasterizer dominates
#   ./fmtowns.sh clean            remove build/fmtowns
#
# Extra arguments after the subcommand are passed to make, so e.g.
#   ./fmtowns.sh build GAME_OPT=-O2
#   ./fmtowns.sh profile MUSIC_SECS=20
#   ./fmtowns.sh profile board EXTRA_CORE_DEFINES=-DWAIFU_BATTLE_BASE_CACHE_DISABLE
# work.
#
# `test` and `profile` need no display and no controller.  Tsugaru cannot
# inject pad input, so a debug build presses its own buttons from a script
# (src/platform/fmtowns/debug_input.txt) and stamps its state -- frame
# counter, CD-DA track, frame timings -- into the top-left pixels of every
# frame, which tools/fmtowns/read_frame_stamp.py reads back out of the
# screenshots.  The timings come from the machine's own 1us counter, so
# -NOWAIT does not distort them.
#
# WHY `profile` PARKS ON ONE SCENE.  Two builds never reach the same place at
# the same second: CD load times are wall-clock, so a faster build arrives
# everywhere earlier and the runs desynchronise within seconds.  Comparing a
# shot from each build then compares two different scenes and says nothing --
# that mistake has already produced one confident, completely wrong "3x
# regression" on this port.  `profile` uses a script that walks into the duel
# and then stops pressing buttons, so both builds sit rendering the same
# frame and their numbers mean the same thing.  Compare the `step` figure;
# `present` is dominated by a vsync wait that is not stable run to run.
set -eu

ROOT=$(cd "$(dirname "$0")" && pwd)
cd "$ROOT"

MAKE="make -f Makefile.fmtowns"
TSUGARU=FMTOWNSCD_EXAMPLE_Cube/Tsugaru_CUI.elf
MARTY_ROM=FMTOWNSCD_EXAMPLE_Cube/MARTY_ROM

# Where test/profile drop their screenshots.  Overridable so two runs can be
# kept side by side (before/after a change).
SHOTS=${FMTOWNS_SHOTS:-$ROOT/build/fmtowns/shots}

usage() {
	sed -n '2,/^set -eu$/{/^set -eu$/d;s/^# \{0,1\}//;p;}' "$0"
	exit "${1:-0}"
}

# Parse "--iso" out of the argument list; everything else goes to make.
target=all
image=build/fmtowns/output.cue
cmd=${1:-build}
[ $# -gt 0 ] && shift
scene=hand
# Everything that is not ours is re-collected into the positional list and
# forwarded to make as separate words.  It used to be accumulated into one
# string, which split a multi-define measurement build --
#   ./fmtowns.sh profile hand 'EXTRA_CORE_DEFINES=-DA -DB'
# -- into a make variable plus a make *target* named -DB, and make died in a
# usage dump.  Combining two measurement knobs is exactly what attributing a
# frame on this target takes, so that has to work.
makeargs=""
for arg in "$@"; do
	case $arg in
	--iso)        target=iso; image=build/fmtowns/output.iso ;;
	hand|board)   scene=$arg ;;
	*)            makeargs="$makeargs '$(printf '%s' "$arg" | sed "s/'/'\\\\''/g")'" ;;
	esac
done

build() {
	eval "$MAKE \"\$target\" $makeargs \"\$@\""
}

# The parked-scene input scripts `profile` compiles in: title -> menu ->
# BATTLE MODE -> ... -> a duel, then nothing.  Frames, not seconds, so they
# replay identically however fast the emulator runs.
#
# Two scenes, because they stress completely different code:
#
#   hand   the duel's default view.  The board is mostly behind the cards,
#          and the cards themselves are 2-D blits, so this measures the
#          blitter and the game step far more than the rasterizer.
#   board  UP from the hand view goes to the top-down board.  The software
#          3-D board fills nearly the whole screen here, so this is the
#          scene to use when changing the rasterizer -- pair it with
#          EXTRA_CORE_DEFINES=-DWAIFU_BATTLE_BASE_CACHE_DISABLE, or the
#          composite cache serves the parked frame from a memcpy and the
#          rasterizer never runs at all.
write_parked_script() {
	cat > "$1" <<-'EOF'
	# Generated by fmtowns.sh -- walk into the duel, then stop pressing
	# buttons so the scene stays put and two builds can be compared on
	# identical pixels.
	 90 START
	300 DOWN
	400 A
	EOF
	if [ "$2" = board ]; then
		cat >> "$1" <<-'EOF'
		# UP leaves the hand for the top-down board view (main.c's
		# IB_PLAYER_HAND case), which waits for input, so the scene holds.
		1000 UP
		EOF
	fi
}

# Boot the debug disc under xvfb and take timed screenshots.  The sleeps are
# wall clock, not emulated frames, so they are deliberately generous: what
# each shot actually caught is read back from the stamp rather than assumed.
capture() {
	mkdir -p "$SHOTS"
	rm -f "$SHOTS"/shot*.png
	script=$SHOTS/monitor.txt
	{
		i=0
		for wait in $1; do
			printf 'SLEEP %s\nSS %s/shot%s.png\n' "$wait" "$SHOTS" "$i"
			i=$((i + 1))
		done
		printf 'SLEEP 2\nQUIT\n'
	} > "$script"

	EXTRA_TSUGARU="-NOWAIT" RUN_TIMEOUT=${RUN_TIMEOUT:-300} \
		tools/fmtowns/headless_shot.sh "$SHOTS/shot" "$script" \
		> "$SHOTS/emulator.log" 2>&1
	shift
	python3 tools/fmtowns/read_frame_stamp.py "$@" "$SHOTS"/shot*.png
}

case $cmd in
build)
	build
	echo "built $image"
	;;
run)
	build
	[ -x "$TSUGARU" ] || { echo "missing $TSUGARU" >&2; exit 1; }
	# Tsugaru's default for game port 0 is a physical gamepad
	# (TOWNS_GAMEPORTEMU_PHYSICAL0), so without -GAMEPORT0 KEY it silently
	# ignores the keyboard and nothing responds at the title screen.
	exec "$TSUGARU" "$MARTY_ROM/" -TOWNSTYPE MARTY -CD "$image" \
		-NORMALFD -DONTUSEFPU -AUTOSCALE -GAMEPORT0 KEY
	;;
test)
	build FMTOWNS_DEBUG_INPUT=1
	capture "40 12 12 12 12 12 12 12 12 12 12 12 12" --summary
	echo
	echo "screenshots in $SHOTS"
	;;
profile)
	mkdir -p "$SHOTS"
	write_parked_script "$SHOTS/parked_input.txt" "$scene"
	build FMTOWNS_DEBUG_INPUT=1 INPUT_SCRIPT="$SHOTS/parked_input.txt"
	# Three shots well after the scene has settled; they should agree.
	capture "35 15 15"
	echo
	echo "screenshots in $SHOTS -- compare the 'step' figure between builds"
	;;
clean)
	$MAKE clean
	;;
-h|--help|help)
	usage
	;;
*)
	echo "unknown command: $cmd" >&2
	usage 1
	;;
esac
