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
// The two heats between gold and red.  A blade sweep that is one flat red reads
// as a scratch; the ramp white -> gold -> orange -> flame -> red is what makes
// it read as a cut.
#define MSX2_ORANGE     MSX2_RGB(7, 3, 0)
#define MSX2_FLAME      MSX2_RGB(6, 1, 0)

// Page 0 is VRAM lines 0-255, page 1 is 256-511; only 0-211 of each shows.
#define MSX2_PAGE_0     0
#define MSX2_PAGE_1     1

void Msx2_VideoInit(void);

// ── SCREEN 10, in the MSX2+ build only ───────────────────────────────────────
//
// A V9958 reads the same 256-byte lines and the same two pages as GRAPHIC 7,
// so nothing about the streamer, the page flip, the command engine or the
// sprite plane changes: the ONLY difference is R#25's YJK/YAE pair, which says
// whether a byte is a GRB332 colour or a brightness plus a share of its group's
// hue.  A screen therefore declares which it is as it takes over, and the
// picture it streams is baked to match (tools/msx2/gen_msx_plus.py).
//
// The two calls are nothing at all in the MSX2 build, which is what lets the
// title and story sources carry them unconditionally.
#ifdef MSX2_PLUS
void Msx2_VideoModeYjk(void);
void Msx2_VideoModeG7(void);
bool Msx2_VideoIsYjk(void);
// The sixteen palette entries a streamed SCREEN 10 picture carries in the tail
// of its last segment.  Called by the streamer itself, so no screen has to.
void Msx2_VideoScenePalette(u16 segment);
#else
#define Msx2_VideoModeYjk()        ((void)0)
#define Msx2_VideoModeG7()         ((void)0)
#define Msx2_VideoIsYjk()          FALSE
#define Msx2_VideoScenePalette(s)  ((void)(s))
#endif

// Pull the bitmap font out of the cartridge into RAM.  Call it once, before
// anything prints; the glyphs are cartridge data, not a C array (msx2_video.c).
void Msx2_VideoLoadFont(void);
extern u8 g_msx2_font[];

// ── The font mask ────────────────────────────────────────────────────────────
//
// A string is drawn by the command engine out of a mask baked into VRAM once
// (msx2_video.c explains the arithmetic).  It lives in page 1's lines 240..255,
// the one part of VRAM nothing else can want: page 0's same lines are the
// sprite tables, Msx2_VideoCopyPage stops at 240, and so does Msx2_ClearPage.
#define MSX2_TEXT_MAX_CHARS   42
#define MSX2_FONT_MASK_LINE   496   // page 1 (256) + MSX2_SPRITE_VRAM_ROW
#define MSX2_FONT_MASK_COLS   MSX2_TEXT_MAX_CHARS   // glyphs a band, 42*6 = 252
#define MSX2_FONT_GLYPHS      64    // MSX2_FONT_BYTES / MSX2_FONT_H_PX

// Expand the glyphs into those lines.  Msx2_VideoInit() calls it; nothing may
// print before it has run.  The body is banked (msx2_video_bank.c) because
// _CODE has no room for a routine that runs once.
void Msx2_VideoBakeFont(void);

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

// Pair these around a composition that must not be seen.  The regression
// build counts the boundaries; the shipping build is the same VDP operation.
#ifdef MSX2_DEBUG_REGRESSION
void Msx2_VideoDisplayBlank(void);
void Msx2_VideoDisplayRestore(void);
#else
#define Msx2_VideoDisplayBlank()   VDP_EnableDisplay(FALSE)
#define Msx2_VideoDisplayRestore() VDP_EnableDisplay(TRUE)
#endif

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
// from the next retained page without redrawing the live arena.
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

// The title's logo, prompt and copyright are painted into the picture by the
// scene generator, not drawn at runtime -- see msx2_video.c and msx2_title.c.
