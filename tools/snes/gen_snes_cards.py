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


def face_bytes(tag, path):
    src = Image.open(path) if path else None
    img = ga.face_image(tag, src, (CARD_W * BUILD, CARD_H * BUILD))
    if src is not None:
        src.close()
    # AREA AVERAGE, NOT LANCZOS.  A sharp kernel over a painting that carries
    # far more detail than sixteen pixels can hold rings, and the ringing lands
    # in the direct-colour cube as speckle that reads as static on the board;
    # box-filtering leaves flat regions the cube can actually hold, so the
    # monster keeps a silhouette.
    small = img.convert("RGB").resize((CARD_W, CARD_H), Image.BOX)
    # Then lifted and pushed, because the card is seen lying on sunlit
    # sandstone: a painting that keeps its own gallery-dark midtones quantises
    # into the cube's bottom levels and reads as a hole cut in the board rather
    # than as a card on it.  Blue is two bits, so saturation has to come back
    # up as well or every face converges on the same brown.
    small = ImageEnhance.Brightness(small).enhance(1.35)
    small = ImageEnhance.Contrast(small).enhance(1.2)
    small = ImageEnhance.Color(small).enhance(1.35)
    return snes_dc.quantize(small, (CARD_W, CARD_H))


def sheet_preview(path, blob, count):
    """The whole sheet as one picture, decoded back through the direct-colour
    cube.  A card that came out as a dark smear is invisible in a hex dump and
    obvious here, and this is the only way to judge the conversion without a
    console."""
    cols = 10
    rows = (count + cols - 1) // cols
    out = Image.new("RGB", (cols * (CARD_W + 1), rows * (CARD_H + 1)),
                    (24, 24, 24))
    for i in range(count):
        face = snes_dc.preview(blob[i * 256:(i + 1) * 256], (CARD_W, CARD_H))
        out.paste(face, ((i % cols) * (CARD_W + 1), (i // cols) * (CARD_H + 1)))
    out.resize((out.width * 3, out.height * 3), Image.NEAREST).save(path)


def main():
    os.makedirs(ASSETS, exist_ok=True)
    fs = faces()
    blob = b""
    for tag, path in fs:
        data = face_bytes(tag, path)
        assert len(data) == 256, "%s is %d bytes, not one page" % (tag, len(data))
        blob += data
    assert len(blob) <= 0x10000, "the card sheet is %d bytes, past one bank" % len(blob)

    with open(os.path.join(ASSETS, "snes_card_tex.bin"), "wb") as fh:
        fh.write(blob)
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
            ".ENDS",
            "",
        ]))
    sheet_preview(os.path.join(ASSETS, "cards_preview.png"), blob, len(fs))

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

extern const u8 snes_card_tex[];

#endif
""" % (CARD_W, len(fs), len(fs) - 1))

    print("%s: bank $%02X, %d faces, %d bytes"
          % (os.path.relpath(os.path.join(ASSETS, "snes_cards.asm"), ROOT),
             0xC0 + BANK, len(fs), len(blob)))
    print("%s written" % os.path.relpath(header, ROOT))


if __name__ == "__main__":
    main()
