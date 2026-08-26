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
#   ./fmtowns.sh profile [hand|board|story]
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

# Overridable so `run`/`test`/`profile` can target a non-Marty model -- e.g.
#   FMTOWNS_TOWNSTYPE=MX FMTOWNS_ROM=/path/to/plain/ROMS ./fmtowns.sh profile
# "FMTOWNS" is NOT a real Tsugaru model string (see STATUS.md); use a real
# one (MX, UX, CX, ...). Defaults reproduce the exact Marty invocation this
# script always used.
FMTOWNS_TOWNSTYPE=${FMTOWNS_TOWNSTYPE:-MARTY}
FMTOWNS_ROM=${FMTOWNS_ROM:-$MARTY_ROM}
FMTOWNS_MEMSIZE=${FMTOWNS_MEMSIZE:-2}

# Marty timing model.  Tsugaru's CPU core is a 486 with zero-wait memory
# whatever -TOWNSTYPE says, so out of the box it reports this game at 60 fps
# on a machine that really runs it in single digits -- every profile taken
# before 2026-08-26 describes a 25 MHz 386 with perfect memory.  These
# switches (added to TOWNSEMU in the same change as this line) scale the
# core's clock count to 386 microcode and charge wait states per data bus
# cycle, VRAM more than main RAM, so that moving bytes out of VRAM shows up
# as the win it is on hardware.
#
# THE NUMBERS ARE A CALIBRATION, NOT A MEASUREMENT.  Nobody has profiled the
# real machine yet.  They are set so a parked duel frame lands in the range
# the phone capture of a real Marty shows, and they should be re-fitted the
# moment someone reads a frame stamp off real hardware
# (tools/fmtowns/read_frame_stamp.py on a photo of the screen).  Until then
# treat absolute figures as indicative and compare builds, not runs.
#
# FMTOWNS_TIMING= (empty) restores the old zero-wait behaviour.
FMTOWNS_TIMING=${FMTOWNS_TIMING--FREQ 16 -CPUCLOCKSCALE 220 -BUSWAIT 2 -VRAMBUSWAIT 6 -DATABUSWIDTH 16}

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
#
# EXTRA_CORE_DEFINES is pulled out of that list instead of being forwarded
# verbatim, because `profile` has to add -DWAIFU_DEBUG_AUTODUEL to it, and
# two command-line assignments of the same make variable would make the last
# one win -- silently dropping whichever measurement knob the caller asked
# for.  build() puts it back, with its own defines prepended.
makeargs=""
core_defines=""
for arg in "$@"; do
	case $arg in
	--iso)                target=iso; image=build/fmtowns/output.iso ;;
	hand|board|story)     scene=$arg ;;
	EXTRA_CORE_DEFINES=*) core_defines="${arg#EXTRA_CORE_DEFINES=}" ;;
	*)            makeargs="$makeargs '$(printf '%s' "$arg" | sed "s/'/'\\\\''/g")'" ;;
	esac
done

# $1 is prepended to whatever EXTRA_CORE_DEFINES the caller passed;
# everything after it goes to make unchanged.
build() {
	own=$1; shift
	eval "$MAKE \"\$target\" $makeargs \
		\"EXTRA_CORE_DEFINES=\$own \$core_defines\" \"\$@\""
}

# The parked-scene input scripts `profile` compiles in: title -> menu ->
# BATTLE MODE -> ... -> a duel, then nothing.  Frames, not seconds, so they
# replay identically however fast the emulator runs.
#
# `profile` does NOT walk in through the menu any more.  A script step fires
# on a GAME frame, but the CD reads between the title screen and the duel
# take a fixed amount of wall time -- so a build with a faster frame has
# loaded LESS by any given frame number, and a press tuned to a slow build
# lands before the menu exists in a fast one.  That is not hypothetical: at
# "400 A" the -Os build entered the duel and the -O2 build sat on the menu
# for the entire capture, and -O2 was written down as a 8% regression when
# it is really a 20% win.  WAIFU_DEBUG_AUTODUEL (src/main.c) boots straight
# into a free duel instead, so there is no navigation left to desynchronise.
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
#   story  the first story plaza dialogue: sky gradient, raycast floor,
#          temple solids, two large portraits and a typewriter text box.
#          Nothing in it moves once the portraits have slid in, so it is
#          the scene that shows what caching and the dirty present are
#          worth -- and the one the "story text is sluggish" report is
#          about.
write_parked_script() {
	# The build boots straight into the duel (WAIFU_DEBUG_AUTODUEL below),
	# so there is nothing to press to get there -- only the one button
	# that picks which parked view to sit on.
	: > "$1"
	if [ "$2" = board ]; then
		cat >> "$1" <<-'EOF'
		# UP leaves the hand for the top-down board view (main.c's
		# IB_PLAYER_HAND case), which waits for input, so the scene holds.
		600 UP
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

	EXTRA_TSUGARU="-NOWAIT $FMTOWNS_TIMING" RUN_TIMEOUT=${RUN_TIMEOUT:-300} \
		FMTOWNS_TOWNSTYPE="$FMTOWNS_TOWNSTYPE" FMTOWNS_ROM="$FMTOWNS_ROM/" \
		FMTOWNS_MEMSIZE="$FMTOWNS_MEMSIZE" \
		tools/fmtowns/headless_shot.sh "$SHOTS/shot" "$script" \
		> "$SHOTS/emulator.log" 2>&1
	shift
	python3 tools/fmtowns/read_frame_stamp.py "$@" "$SHOTS"/shot*.png
}

case $cmd in
build)
	build ""
	echo "built $image"
	;;
run)
	build ""
	[ -x "$TSUGARU" ] || { echo "missing $TSUGARU" >&2; exit 1; }
	# Tsugaru's default for game port 0 is a physical gamepad
	# (TOWNS_GAMEPORTEMU_PHYSICAL0), so without -GAMEPORT0 KEY it silently
	# ignores the keyboard and nothing responds at the title screen.
	exec "$TSUGARU" "$FMTOWNS_ROM/" -TOWNSTYPE "$FMTOWNS_TOWNSTYPE" -MEMSIZE "$FMTOWNS_MEMSIZE" -CD "$image" \
		-NORMALFD -DONTUSEFPU -AUTOSCALE -GAMEPORT0 KEY $FMTOWNS_TIMING
	;;
test)
	build "" FMTOWNS_DEBUG_INPUT=1
	capture "40 12 12 12 12 12 12 12 12 12 12 12 12" --summary
	echo
	echo "screenshots in $SHOTS"
	;;
profile)
	mkdir -p "$SHOTS"
	write_parked_script "$SHOTS/parked_input.txt" "$scene"
	case $scene in
	story) parked_define=-DWAIFU_DEBUG_AUTOSTORY ;;
	*)     parked_define=-DWAIFU_DEBUG_AUTODUEL ;;
	esac
	build "$parked_define" \
		FMTOWNS_DEBUG_INPUT=1 INPUT_SCRIPT="$SHOTS/parked_input.txt"
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
