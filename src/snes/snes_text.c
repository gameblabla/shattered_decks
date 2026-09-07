/* ─────────────────────────────────────────────────────────────────────────────
 *  snes_text.c — HUD text, drawn into the chunky framebuffer.
 *
 *  The duel's text lives in the framebuffer rather than on a BG layer because
 *  there is no spare BG layer: Mode 7 has exactly one, and it is the board.
 *  So the HUD band's texels are written the same way the board's are, by the
 *  glyph blitter in snes_raster.asm, out of the same 1bpp font the PC, CD32X
 *  and Atari ST builds draw with (src/engine/font_menudata.h).
 *
 *  EVERY GLYPH IS DRAWN TWICE, once a pixel down and right in a dark colour
 *  and once in its own.  The band is arena sandstone, and light text on stone
 *  without a shadow is a smear at this size -- the same conclusion the PC-FX
 *  port's drop-shadow font reached.
 * ───────────────────────────────────────────────────────────────────────────── */
#include "snes_text.h"
#include "snes_video.h"

void snesTextAt(u8 x, u8 y, const char *s, u8 ink, u8 shadow)
{
    u16 base = (u16)((u16)y * SNES_FB_STRIDE + x);

    while (*s) {
        const u16 ch = (u16)(u8)*s++;
        if (ch != ' ') {
            snesTextGlyph((u16)(base + SNES_FB_STRIDE + 1), ch, shadow,
                          SNES_FB_STRIDE);
            snesTextGlyph(base, ch, ink, SNES_FB_STRIDE);
        }
        base += SNES_TEXT_W;
    }
}

/* A right-aligned unsigned number in a fixed field, because life points are
 * read as a column and a left-aligned 800 under an 8000 looks like a different
 * number.  No division: 816-tcc's is a software routine and this is called for
 * both sides on every HUD repaint. */
void snesTextNum(u8 x, u8 y, u16 v, u8 digits, u8 ink, u8 shadow)
{
    static const u16 pow10[5] = { 1, 10, 100, 1000, 10000 };
    u8 i = digits;
    char buf[6];

    if (digits > 5) digits = 5;
    buf[digits] = 0;
    for (i = 0; i < digits; ++i) {
        const u16 unit = pow10[digits - 1 - i];
        u8 d = 0;
        while (v >= unit) { v -= unit; ++d; }
        buf[i] = (char)('0' + d);
    }
    snesTextAt(x, y, buf, ink, shadow);
}
