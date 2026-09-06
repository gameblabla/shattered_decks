/* ─────────────────────────────────────────────────────────────────────────────
 *  atarist_video.h — screen, palettes and the raster split.
 *
 *  The in-game screen is ST low resolution: 320x200, 16 colours, four
 *  interleaved bitplanes, 32000 bytes, double buffered.  It is cut in two by a
 *  Timer B raster split so the halves have independent 16-colour palettes:
 *
 *      rows 0 .. ATARIST_SPLIT_Y-1   ARENA  — the 3D board
 *      rows ATARIST_SPLIT_Y .. 199   CARD   — hand, card art, HUD text
 *
 *  The board half is produced by rasterising into an 8-bit chunky buffer and
 *  converting (atarist_c2p.S).  The card half is drawn straight into the
 *  planar back buffer at full 320x200 resolution, because the requirement is
 *  that card art is never the thing that pays for the 3D downscale.
 * ───────────────────────────────────────────────────────────────────────────── */
#ifndef WAIFU_ATARIST_VIDEO_H
#define WAIFU_ATARIST_VIDEO_H

#include <stdint.h>

#define ATARIST_SCREEN_W      320
#define ATARIST_SCREEN_H      200
#define ATARIST_SCREEN_BYTES  (ATARIST_SCREEN_W / 2 * ATARIST_SCREEN_H)
#define ATARIST_SCREEN_STRIDE (ATARIST_SCREEN_W / 2)   /* 160 bytes per row */

/* The raster split.  112 rows of board leaves 88 for the hand row and the two
 * HUD text lines, which is what the duel layout needs at 8x8 glyphs. */
#define ATARIST_SPLIT_Y       112

/* Chunky board buffer.  Full resolution is 320x112; a moving camera renders
 * 160x56 into the same buffer (same stride) and the C2P doubles it. */
#define ATARIST_CHUNKY_W      320
#define ATARIST_CHUNKY_H      ATARIST_SPLIT_Y
#define ATARIST_CHUNKY_STRIDE ATARIST_CHUNKY_W
#define ATARIST_CHUNKY_BYTES  (ATARIST_CHUNKY_STRIDE * ATARIST_CHUNKY_H)

/* Machine detection, filled in by Atarist_VideoInit(). */
extern uint8_t g_atarist_is_ste;      /* STE-class shifter present */
extern uint8_t g_atarist_has_blitter; /* Blitter chip present */

/* Frames counted by our VBL handler.  This is the port's only clock: nothing
 * reads TOS's _frclock, because a frame that overruns must still advance the
 * game by the number of vblanks it actually took. */
extern volatile uint32_t g_atarist_vbl;

int  Atarist_VideoInit(void);
void Atarist_VideoShutdown(void);

/* The planar buffer the game draws into this frame. */
uint8_t *Atarist_BackBuffer(void);
/* The chunky buffer the 3D board rasterises into. */
uint8_t *Atarist_Chunky(void);

/* Hand the back buffer to the shifter at the next vblank and take the other
 * one.  Returns the number of vblanks the finished frame occupied (>= 1), so
 * the caller can advance its animation clocks by real time. */
int  Atarist_VideoPresent(void);
void Atarist_VideoWaitVbl(void);

/* ── Raster splits ─────────────────────────────────────────────────────────
 *  The screen carries a LIST of palettes, each taking effect at a scanline:
 *  entry 0 from the top of the frame, entry i from its own line down.  Timer B
 *  counts display-enable pulses, so the handler simply re-arms itself with the
 *  gap to the next entry (atarist_isr.S).
 *
 *  The duel uses two -- board and cards -- but a static screen may use every
 *  slot, which is how a title or a portrait shows far more than sixteen
 *  colours at once. */
#define ATARIST_MAX_SPLITS  16

typedef struct AtaristSplit {
    uint16_t line;          /* first scanline this palette applies to */
    uint16_t pal[16];
} AtaristSplit;

/* Install a whole list.  `list[0].line` is ignored (it is always the top of
 * the frame) and entries must be in increasing line order.  Takes effect at
 * the next vblank, so a mid-frame change never tears. */
void Atarist_SetSplits(const AtaristSplit *list, int count);

/* The duel's two halves, kept as named helpers because that is what every call
 * site means.  They edit the same two-entry list. */
void Atarist_SetArenaPalette(const uint16_t *pal16);
void Atarist_SetCardPalette(const uint16_t *pal16);
/* One palette for the whole screen, for scenes that have no split. */
void Atarist_SetWholePalette(const uint16_t *pal16);
/* Enable/disable the split.  Disabled, entry 0 covers the screen. */
void Atarist_SetSplitEnabled(int on);

/* Pack an 8-bit-per-channel RGB triple into this machine's palette word.  The
 * STE has four bits per channel with the extra bit at the bottom of a rotated
 * field; the ST has three.  Converters emit 8-bit RGB and let the running
 * machine decide, so one asset set serves both builds. */
uint16_t Atarist_PackRGB(uint8_t r, uint8_t g, uint8_t b);

/* Clear the planar back buffer (whole screen, or one row band). */
void Atarist_ClearPlanar(uint8_t colour);
void Atarist_ClearPlanarBand(int y0, int y1, uint8_t colour);

/* ── Chunky -> planar (atarist_c2p.S) ──────────────────────────────────────
 *  Both convert `rows` source rows into the top of the planar destination.
 *  _Double doubles in x and y (160x56 -> 320x112); _Direct is 1:1.
 *  `src` is 8-bit palette indices 0..15, `src_stride` bytes per source row. */
void Atarist_C2P_Double(const uint8_t *src, uint8_t *dst, int rows, int src_stride);
void Atarist_C2P_Direct(const uint8_t *src, uint8_t *dst, int rows, int src_stride);

#endif /* WAIFU_ATARIST_VIDEO_H */
