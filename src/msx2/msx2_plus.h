// ─────────────────────────────────────────────────────────────────────────────
//  msx2_plus.h — what a SCREEN 10 translation unit says instead
//
//  Included, AFTER msx2_video.h and msx2_scenes.h, by exactly the translation
//  units whose screens are SCREEN 10 in the MSX2+ build: the title and the
//  whole of the story.  It is empty in the MSX2 build, so the same source
//  compiles for both cartridges with no #ifdef in the drawing code itself.
//
//  WHY A COLOUR HAS TO BE SAID TWICE.
//  A GRAPHIC 7 byte IS a colour (GRB332).  A SCREEN 10 byte is a brightness
//  and a share of its group's hue -- unless bit 3 is set, in which case it is
//  palette entry bits 7..4 at full accuracy and owes its neighbours nothing.
//  Interface has to be the second kind: a word drawn as YJK would take its hue
//  from whatever painting the three pixels beside it belong to, and change
//  colour as the picture under it changed.  So in a SCREEN 10 unit every ink
//  name below becomes a YAE pixel naming an entry of the palette
//  tools/msx2/msx2_yjk.py reserves for the interface (UI_PALETTE, entries
//  0..7; the picture fits the other eight to itself).
//
//  The low three bits of a YAE byte are still read as its group's J or K, so
//  they are left at zero here and the four-pixel group a piece of interface
//  lands on loses its hue -- which is exactly what is wanted inside a panel,
//  and is why the panels themselves are stamped flat in the baked picture.
// ─────────────────────────────────────────────────────────────────────────────

#pragma once

#ifdef MSX2_PLUS

// (entry << 4) | YAE.  The entries are msx2_yjk.py's UI_PALETTE, in order.
#define MSX2_YAE(i)     (u8)(((i) << 4) | 0x08)

#undef  MSX2_BLACK
#undef  MSX2_WHITE
#undef  MSX2_GOLD
#undef  MSX2_DEEP_BLUE
#undef  MSX2_SAND
#undef  MSX2_DARK_SAND
#undef  MSX2_RED
#undef  MSX2_TEAL
#undef  MSX2_ORANGE
#undef  MSX2_FLAME
#undef  MSX2_PANEL_COLOR

#define MSX2_BLACK      MSX2_YAE(0)
#define MSX2_PANEL_COLOR MSX2_YAE(1)
#define MSX2_GOLD       MSX2_YAE(2)
#define MSX2_SAND       MSX2_YAE(3)
#define MSX2_DARK_SAND  MSX2_YAE(4)
#define MSX2_TEAL       MSX2_YAE(5)
#define MSX2_RED        MSX2_YAE(6)
#define MSX2_WHITE      MSX2_YAE(7)
#define MSX2_DEEP_BLUE  MSX2_YAE(1)
#define MSX2_ORANGE     MSX2_YAE(11)
#define MSX2_FLAME      MSX2_YAE(13)

// THE BUSTS STAND ON MULTIPLES OF FOUR.
// A chroma group is four pixels wide and starts at a multiple of four, and a
// bust is baked with its own left edge as group zero (tools/msx2/gen_msx_plus.py
// rounds every opaque run inward to a group for the same reason).  Two pixels
// to the left, as the MSX2 build has it, would put every group of the figure
// across a boundary and hand its hue to the picture beside it.
#undef  MSX2_PORTRAIT_LEFT_X
#undef  MSX2_PORTRAIT_RIGHT_X
#define MSX2_PORTRAIT_LEFT_X   0
#define MSX2_PORTRAIT_RIGHT_X  128

#endif // MSX2_PLUS
