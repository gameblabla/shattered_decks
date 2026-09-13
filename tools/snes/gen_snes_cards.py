#!/usr/bin/env python3
"""The card faces, as Mode 7 direct-colour board textures.

Every face is the same object the other ports show -- the PC card template
with the monster's painting composited into its art window -- converted here
by the ST generator's own crop, contrast and framing rules
(tools/atarist/gen_atarist_assets.py) so a card is framed identically on both
machines and the crop is not re-derived in a second place.

**A FACE IS 16x16 TEXELS AND THAT IS A RASTERISER CONSTRAINT, NOT AN ART ONE.**
The span walker keeps its texture index in one register as `page << 8 | texel`,
where the low byte is `(v << 4) | u`; the eight-bit add that steps u therefore
stays inside the card's own 256-byte page and never disturbs v or the page,
exactly as the floor's 256-byte row does.  So a face is one page, a page is
16x16, and the whole sheet is an array of pages a card id indexes directly.

A face covers ONE BOARD TILE -- one world unit square, 16 texels across it --
and that is what makes a resting card cost the floor's arithmetic and no more:
the floor is 32 texels a unit, so the card's texture step is the floor's
shifted right by one, and its v is the row's depth shifted, with no extra
multiply and no divide anywhere in the row.  The card's own dark keyline is
what separates one from the next; there is no gap in world space to leave.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "atarist"))

from PIL import Image, ImageEnhance  # noqa: E402
import snes_dc  # noqa: E402
import gen_atarist_assets as ga  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ASSETS = os.path.join(ROOT, "src", "snes", "assets")

CARD_W = CARD_H = 16            # one page, and one board tile
BANK = 8                        # $C8; the floor is $C6 and the horizon $C7

# THE 1:1 RESTING BOARD HAS ITS OWN SHEET.  The Mode 3 board shows a resting
# card at the screen's own resolution -- twenty to thirty pixels across --
# and sixteen texels stretched over that is a mosaic.  So every face is baked
# a second time at 32x32 (one kilobyte, indexed (v << 5) | u inside a
# 1024-byte page) for snesSpanCard32.  Seventy-nine of those is 79 KB, which is
# more than the 64 KB an X-indexed long read can span from one base, so faces
# 0..63 are one sheet and 64..78 another; the walker picks the base by face.
CARD32 = 32
CARD32_SPLIT = 64
BANK32_LO = 59                  # $FB: faces 0..63, a whole bank
BANK32_HI = 60                  # $FC: faces 64..78, under the rest texel LUT

# The name the HUD prints under the board.  Fixed width and NOT a table of
# pointers: a pointer table in a data bank costs a far indirection per lookup on
# this CPU and the name is read every frame, so sixteen bytes a face indexed by
# the card id is both smaller and one shift away.  Fifteen characters is what
# the HUD row has room for beside nothing else, and it is the same cut the MSX2
# tables take -- the part before the comma, which is the character's name.
NAME_LEN = 16
SUPPORT_NAMES = ("BRONZE EQUIP", "DESERT GUARD", "ANCIENT DRAW",
                 "OASIS LIGHT", "THUNDER", "MIRROR VEIL")

# The face is composited at four times its final size and resampled down.  A
# card front built directly at sixteen pixels loses the frame's gold rules to
# the nearest-pixel grid; built at sixty-four and filtered down they survive as
# a lighter edge, which is what tells a card from a tile at this scale.
BUILD = 4


def faces():
    """Every face in card-id order: 72 monsters, the six supports, the back.

    The order IS the mapping -- src/msx2/msx2_duel.c's card ids are monster
    index, then support kind, and the back is the id past the end -- so the
    renderer indexes the sheet with the card id and needs no table."""
    out = []
    for asset_id in ga.parse_cards():
        out.append(("m:" + asset_id, ga.find_card_image(asset_id)))
    for kind in range(ga.SUPPORT_VARIANTS):
        out.append(("s:%d" % kind, None))
    out.append(("back", None))
    return out


def card_names():
    """Every face's name in card-id order, to match the sheet exactly.

    The names come out of the same generated asset header every other port
    prints, so a card is called the same thing on the SNES as on the PC; the
    HUD font is upper case only, so they are folded here rather than at the
    twenty-odd draw sites that would otherwise each need to."""
    import re
    src = os.path.join(ROOT, "src", "generated", "waifu_assets.h")
    text = open(src, encoding="utf-8", errors="replace").read()
    m = re.search(r'waifu_card_names\[WAIFU_CARD_COUNT\]\s*=\s*\{(.*?)\n\};',
                  text, re.S)
    if not m:
        sys.exit("gen_snes_cards: waifu_card_names not found in %s" % src)
    names = [n.split(",")[0].strip()
             for n in re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1))]
    names += list(SUPPORT_NAMES)
    names.append("FACE DOWN")
    blob = bytearray()
    for n in names:
        cut = n.upper().encode("ascii", "replace")[:NAME_LEN - 1]
        blob += cut + bytes(NAME_LEN - len(cut))
    return names, bytes(blob)


def face_bytes(tag, path, size=CARD_W):
    src = Image.open(path) if path else None
    img = ga.face_image(tag, src, (size * BUILD, size * BUILD))
    if src is not None:
        src.close()
    # AREA AVERAGE, NOT LANCZOS.  A sharp kernel over a painting that carries
    # far more detail than sixteen pixels can hold rings, and the ringing lands
    # in the direct-colour cube as speckle that reads as static on the board;
    # box-filtering leaves flat regions the cube can actually hold, so the
    # monster keeps a silhouette.
    small = img.convert("RGB").resize((size, size), Image.BOX)
    # Then lifted and pushed, because the card is seen lying on sunlit
    # sandstone: a painting that keeps its own gallery-dark midtones quantises
    # into the cube's bottom levels and reads as a hole cut in the board rather
    # than as a card on it.  Blue is two bits, so saturation has to come back
    # up as well or every face converges on the same brown.
    small = ImageEnhance.Brightness(small).enhance(1.35)
    small = ImageEnhance.Contrast(small).enhance(1.2)
    small = ImageEnhance.Color(small).enhance(1.35)
    return snes_dc.quantize(small, (size, size))


def sheet_preview(path, blob, count, size=CARD_W):
    """The whole sheet as one picture, decoded back through the direct-colour
    cube.  A card that came out as a dark smear is invisible in a hex dump and
    obvious here, and this is the only way to judge the conversion without a
    console."""
    cols = 10
    rows = (count + cols - 1) // cols
    page = size * size
    out = Image.new("RGB", (cols * (size + 1), rows * (size + 1)),
                    (24, 24, 24))
    for i in range(count):
        face = snes_dc.preview(blob[i * page:(i + 1) * page], (size, size))
        out.paste(face, ((i % cols) * (size + 1), (i // cols) * (size + 1)))
    out.resize((out.width * 3, out.height * 3), Image.NEAREST).save(path)


def emit_sheet32(fs):
    blob = b""
    for tag, path in fs:
        data = face_bytes(tag, path, CARD32)
        assert len(data) == CARD32 * CARD32
        blob += data
    lo = blob[:CARD32_SPLIT * CARD32 * CARD32]
    hi = blob[CARD32_SPLIT * CARD32 * CARD32:]
    assert len(lo) <= 0x10000 and len(hi) <= 0x4000, "the 32x32 sheets outgrew their banks"
    for name, bank, label, data in (("snes_cards32", BANK32_LO, "snes_card_tex32", lo),
                                    ("snes_cards32b", BANK32_HI, "snes_card_tex32b", hi)):
        with open(os.path.join(ASSETS, label + ".bin"), "wb") as fh:
            fh.write(data)
        with open(os.path.join(ASSETS, name + ".asm"), "w") as fh:
            fh.write("\n".join([
                "; Generated by tools/snes/gen_snes_cards.py; do not edit.",
                '.include "hdr.asm"',
                ".BASE $C0",
                '.SECTION "%s" BANK %d SLOT 0 ORG $0000 FORCE' % (name, bank),
                "",
                "%s:" % label,
                '    .INCBIN "%s.bin"' % label,
                "",
                ".ENDS",
                "",
            ]))
        print("%s: bank $%02X, %d bytes" % (name, 0xC0 + bank, len(data)))
    sheet_preview(os.path.join(ASSETS, "cards32_preview.png"), blob, len(fs), CARD32)


def main():
    os.makedirs(ASSETS, exist_ok=True)
    fs = faces()
    blob = b""
    for tag, path in fs:
        data = face_bytes(tag, path)
        assert len(data) == 256, "%s is %d bytes, not one page" % (tag, len(data))
        blob += data
    assert len(blob) <= 0x10000, "the card sheet is %d bytes, past one bank" % len(blob)

    names, name_blob = card_names()
    if len(names) != len(fs):
        sys.exit("gen_snes_cards: %d names against %d faces"
                 % (len(names), len(fs)))

    with open(os.path.join(ASSETS, "snes_card_tex.bin"), "wb") as fh:
        fh.write(blob)
    with open(os.path.join(ASSETS, "snes_card_names.bin"), "wb") as fh:
        fh.write(name_blob)
    with open(os.path.join(ASSETS, "snes_cards.asm"), "w") as fh:
        fh.write("\n".join([
            "; Generated by tools/snes/gen_snes_cards.py; do not edit.",
            '.include "hdr.asm"',
            ".BASE $C0",
            '.SECTION "snes_cards" BANK %d SLOT 0 ORG $0000 FORCE' % BANK,
            "",
            "snes_card_tex:",
            '    .INCBIN "snes_card_tex.bin"',
            "",
            "snes_card_names:",
            '    .INCBIN "snes_card_names.bin"',
            "",
            ".ENDS",
            "",
        ]))
    sheet_preview(os.path.join(ASSETS, "cards_preview.png"), blob, len(fs))
    emit_sheet32(fs)

    header = os.path.join(ROOT, "src", "snes", "snes_cards.h")
    with open(header, "w") as fh:
        fh.write("""/* Generated by tools/snes/gen_snes_cards.py; do not edit. */
#ifndef WAIFU_SNES_CARDS_H
#define WAIFU_SNES_CARDS_H

#include "snes_types.h"

/* One face is one 256-byte page: 16x16 texels, indexed (v << 4) | u, which is
 * what lets the span walker step u with an eight-bit add that cannot reach v.
 * A face covers one board tile, so 16 texels is one world unit. */
#define SNES_CARD_TEXELS      %d
#define SNES_CARD_TEXELS_LOG2 4
#define SNES_CARD_FACES       %d
#define SNES_CARD_BACK        %d      /* also what a set monster shows */

/* The high half of the span walker's texture index: the face's page. */
#define snesCardPage(face)    ((u16)((u16)(face) << 8))

/* The name the HUD prints, sixteen bytes a face and NUL padded, indexed by the
 * same card id the sheet is.  A fixed stride rather than a pointer table: the
 * name is read every frame and a shift beats a far indirection here. */
#define SNES_CARD_NAME_LEN    %d
#define snesCardName(face)    (&snes_card_names[(u16)(face) * SNES_CARD_NAME_LEN])

extern const u8 snes_card_tex[];
extern const char snes_card_names[];

/* The 1:1 resting sheet: 32x32 texels a face, one kilobyte, indexed
 * (v << 5) | u inside the page.  Faces below SNES_CARD32_SPLIT are in
 * snes_card_tex32, the rest in snes_card_tex32b (a long-indexed read spans one
 * bank); snesSpanCard32 picks the sheet from the face. */
#define SNES_CARD32_TEXELS    %d
#define SNES_CARD32_SPLIT     %d
extern const u8 snes_card_tex32[];
extern const u8 snes_card_tex32b[];

#endif
""" % (CARD_W, len(fs), len(fs) - 1, NAME_LEN, CARD32, CARD32_SPLIT))

    print("%s: bank $%02X, %d faces, %d bytes"
          % (os.path.relpath(os.path.join(ASSETS, "snes_cards.asm"), ROOT),
             0xC0 + BANK, len(fs), len(blob)))
    print("%s written" % os.path.relpath(header, ROOT))


if __name__ == "__main__":
    main()
