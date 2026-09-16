#!/bin/sh
# Build every shipping port and package each one as a zip in release/.
#
#   ./build_all.sh                  build + package all five ports
#   ./build_all.sh pcfx msx2        build + package only the named ports
#
# What lands in release/:
#   ShatteredDecks_PCFX_gbb_2026.zip  PC-FX CD image   (make -f Makefile.pcfx zip)
#   waifucd32x.zip                    SEGA CD 32X image (make -f Makefile.cd32x zip)
#   waifu_fmt.zip                     FM TOWNS image    (make -f Makefile.fmtowns zip)
#   waifu_msx2.zip                    MSX2 + MSX2+ ROMs, both mappers
#                                                     (make -f Makefile.msx2 cartridges)
#   shattered_decks_win64.zip         Windows PC build  (make -f Makefile.win64 dist)
#
# A failure aborts the script (set -eu) so a release/ zip always matches the
# current tree. release/*.zip stays gitignored (see .gitignore *.zip): the
# zips are reproducible from the committed tree, so only this script is
# committed, not the binaries.
set -eu

ROOT=$(cd "$(dirname "$0")" && pwd)
cd "$ROOT"

RELEASE=${RELEASE:-$ROOT/release}
mkdir -p "$RELEASE"

for arg in "$@"; do
	case $arg in
	pcfx|cd32x|fmtowns|msx2|win64) ;;
	*) echo "unknown port: $arg (want pcfx|cd32x|fmtowns|msx2|win64)" >&2; exit 1 ;;
	esac
done

want() {
	port=$1; shift
	[ $# -eq 0 ] && return 0
	for p in "$@"; do
		[ "$p" = "$port" ] && return 0
	done
	return 1
}

if want pcfx "$@"; then
	echo "=== PC-FX ==="
	make -f Makefile.pcfx zip
	cp -f ShatteredDecks_PCFX_gbb_2026.zip "$RELEASE/"
	echo "wrote $RELEASE/ShatteredDecks_PCFX_gbb_2026.zip"
fi

if want cd32x "$@"; then
	echo "=== CD32X ==="
	make -f Makefile.cd32x zip
	cp -f waifucd32x.zip "$RELEASE/"
	echo "wrote $RELEASE/waifucd32x.zip"
fi

if want fmtowns "$@"; then
	echo "=== FM TOWNS ==="
	make -f Makefile.fmtowns zip
	cp -f build/fmtowns/waifu_fmt.zip "$RELEASE/"
	echo "wrote $RELEASE/waifu_fmt.zip"
fi

if want msx2 "$@"; then
	echo "=== MSX2 / MSX2+ ==="
	make -f Makefile.msx2 cartridges
	rm -f "$RELEASE/waifu_msx2.zip"
	zip -j -q "$RELEASE/waifu_msx2.zip" \
		REDME_MSX2.txt \
		src/msx2/out/waifu_msx2.rom \
		src/msx2/out/waifu_msx2p.rom \
		src/msx2/out/waifu_msx2_ascii16x.rom \
		src/msx2/out/waifu_msx2p_ascii16x.rom
	echo "wrote $RELEASE/waifu_msx2.zip"
fi

if want win64 "$@"; then
	echo "=== Windows PC ==="
	make -f Makefile.win64 dist
	rm -f "$RELEASE/shattered_decks_win64.zip"
	(cd dist && zip -qr "$RELEASE/shattered_decks_win64.zip" win64)
	echo "wrote $RELEASE/shattered_decks_win64.zip"
fi

echo "release/ contents:"
ls -l "$RELEASE"
