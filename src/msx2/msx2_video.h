// ─────────────────────────────────────────────────────────────────────────────
//  msx2_video.h — GRAPHIC 7 (SCREEN 8) video layer
//
//  The port lives in GRAPHIC 7 permanently: 256x212, 256 fixed GRB332 colours,
//  two 64 KB pages in 128 KB of VRAM (MSX2_PORT_PLAN.md §0.1).  There is no
//  framebuffer in RAM — nothing here reads back what is on screen.  Drawing is
//  either a VDP command (the command engine costs the Z80 almost nothing) or a
//  direct write through the data port.
//
//  Colour is GRB332: byte = (G<<5) | (R<<2) | B.  There is no palette in this
//  mode (the V9938's 16 palette registers apply to sprites only), so a colour
//  is just arithmetic and every fade has to be faked — see the plan §7.4.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "msxgl.h"

#define MSX2_SCREEN_W   256
#define MSX2_SCREEN_H   212

// The bring-up font: 6 pixels wide inside an 8x8 cell, leftmost pixel in bit 7.
#define MSX2_FONT_W_PX  6
#define MSX2_FONT_H_PX  8

// GRB332 helper.  g, r in 0..7, b in 0..3.
#define MSX2_RGB(r, g, b)  (u8)(((g) << 5) | ((r) << 2) | (b))

#define MSX2_BLACK      MSX2_RGB(0, 0, 0)
#define MSX2_WHITE      MSX2_RGB(7, 7, 3)
#define MSX2_GOLD       MSX2_RGB(7, 5, 0)
#define MSX2_DEEP_BLUE  MSX2_RGB(0, 0, 2)
#define MSX2_SAND       MSX2_RGB(6, 5, 1)
#define MSX2_DARK_SAND  MSX2_RGB(3, 2, 0)
#define MSX2_RED        MSX2_RGB(7, 0, 0)
#define MSX2_TEAL       MSX2_RGB(0, 5, 2)

// Page 0 is VRAM lines 0-255, page 1 is 256-511; only 0-211 of each shows.
#define MSX2_PAGE_0     0
#define MSX2_PAGE_1     1

void Msx2_VideoInit(void);

// ── Presentation ─────────────────────────────────────────────────────────────
//
// NOTHING IS EVER DRAWN ON THE PAGE THE VDP IS SCANNING OUT.  128 KB of VRAM is
// exactly two GRAPHIC 7 pages, so the port always has a spare one: a scene
// draws into the hidden page and asks for a flip, and the flip happens inside
// the next V-blank.  Drawing on the visible page is what made the title flicker
// -- an erase and the redraw over it are two separate command-engine jobs, and
// a scan line passing between them shows the gap.
//
// The consequence, and it is the only thing a scene has to remember: a partial
// repaint lands on ONE page.  The other page still holds the previous picture,
// so anything that changes must be painted on BOTH pages -- that is, over two
// consecutive flips -- before it is settled.  Scenes do that by keeping a dirty
// mask alive for MSX2_VIDEO_PAGES frames.
#define MSX2_VIDEO_PAGES  2

// Show `page` right now and put drawing on the other one.  For composing a
// screen from scratch, not for per-frame updates -- it writes R#2 immediately,
// so call it while the display is blanked or during V-blank.
void Msx2_VideoShowPage(u8 page);
u8   Msx2_VideoGetShowPage(void);

// Ask for the hidden page to become visible at the next V-blank.
void Msx2_VideoFlipRequest(void);

// Perform a pending flip.  Called by the frame loop immediately after HALT, so
// R#2 changes inside the blanking interval and the swap itself never tears.
void Msx2_VideoPresent(void);

// Which page drawing goes to.  Set explicitly only while composing both pages;
// the flip keeps it pointed at the hidden one the rest of the time.
void Msx2_VideoDrawPage(u8 page);
u8   Msx2_VideoGetDrawPage(void);

// Duplicate a whole page onto the other one, offscreen rows included, with a
// single HMMM.  This is how the second buffer is built: re-streaming a scene
// out of the cartridge for it would cost three times as long as letting the
// command engine copy what is already in VRAM.
void Msx2_VideoCopyPage(u8 src, u8 dst);

// Flat fill via the VDP's HMMV command: the command engine does the work, the
// Z80 only writes the command registers.
void Msx2_Fill(u8 x, u8 y, u16 w, u8 h, u8 color);
void Msx2_ClearPage(u8 color);

// A 1-pixel outline, drawn as four fills.
void Msx2_FrameRect(u8 x, u8 y, u16 w, u8 h, u8 color);

// A single pixel line in the current draw page.  Effects use this for the
// short-lived beam and impact rays; the retained board itself only needs the
// quad helper below.
void Msx2_Line(u8 x1, u8 y1, u8 x2, u8 y2, u8 color);
void Msx2_LineXor(u8 x1, u8 y1, u8 x2, u8 y2, u8 color);

// XOR outlines are reversible, which lets a short-lived effect be removed
// from the next retained page without restoring the whole captured arena.
void Msx2_FrameRectXor(u8 x, u8 y, u16 w, u8 h, u8 color);

// A 1-pixel outline around an arbitrary quad, drawn with four VDP LINE
// commands.  The duel board's slots are projected trapezoids rather than
// rectangles (MSX2_PORT_PLAN.md §4.3), so the selection bracket has to follow
// the perspective or it sits visibly beside the card it is selecting.  The
// quad is eight bytes -- x0,y0..x3,y3 -- which is exactly what the generated
// slot table holds.
void Msx2_QuadOutline(const u8* quad, u8 color);
void Msx2_QuadOutlineXor(const u8* quad, u8 color);

// VRAM to VRAM inside the draw page, via HMMM -- the command engine does it all.
// Used to stash and restore strips of a streamed picture in the offscreen rows
// below the visible 212, which is how anything drawn over artwork gets undone
// without re-streaming it.
void Msx2_CopyRect(u8 sx, u8 sy, u8 dx, u8 dy, u16 w, u8 h);

// Text.  x and y are pixels; the writer is in msx2_video.c, not MSXgl.
void Msx2_TextColor(u8 fg, u8 bg);
void Msx2_TextAt(u8 x, u8 y, const c8* text);
void Msx2_TextCenter(u8 y, const c8* text);
void Msx2_NumAt(u8 x, u8 y, i16 value);

// Width in pixels the current font would take for `text` -- used for centring.
u8   Msx2_TextWidth(const c8* text);

// Double-size text.  The bitmap printer has no scaler -- its "character size"
// is only an advance width -- so a 2x glyph is drawn here as runs of fills,
// which also makes it transparent: whatever is already on the page shows
// through, which is what lets the logo sit on streamed artwork.  A hard offset
// shadow keeps it readable over both bright sky and dark rock.  Costs a couple
// of dozen commands per character, so this is composition, never per frame.
void Msx2_TextBigShadow(u8 y, const c8* text, u8 fg, u8 shadow);
u8   Msx2_TextBigWidth(const c8* text);

// Normal-size text with a 1-pixel outline all round, drawn as runs of fills so
// the artwork shows through everywhere the glyph does not.  This is what text
// over a streamed picture uses: an outline reads on both a bright sky and a
// dark rock, where a plain colour reads on neither.
void Msx2_TextOutline(u8 x, u8 y, const c8* text, u8 fg, u8 outline);
void Msx2_TextOutlineCenter(u8 y, const c8* text, u8 fg, u8 outline);
