/* ─────────────────────────────────────────────────────────────────────────────
 *  atarist_video.c — screen ownership: buffers, palettes, the split, the flip.
 *
 *  The game keeps TOS resident, so the only things taken away from it are the
 *  screen registers, the VBL vector (chained, see atarist_isr.S) and Timer B,
 *  which TOS never uses.  Everything is put back by Atarist_VideoShutdown() so
 *  a quit lands on a working desktop rather than a dead machine.
 * ───────────────────────────────────────────────────────────────────────────── */

#include "atarist_video.h"
#include "atarist_hw.h"
#include "atarist_os.h"
#include "atarist_probe.h"

uint8_t g_atarist_is_ste;
uint8_t g_atarist_has_blitter;
volatile uint32_t g_atarist_vbl;

/* Read by atarist_isr.S. */
uint8_t *g_atarist_front;
uint8_t *g_atarist_back;
uint16_t g_atarist_pal_arena[16];
uint16_t g_atarist_pal_card[16];
uint8_t  g_atarist_split_on;
uint8_t  g_atarist_split_line = ATARIST_SPLIT_Y;
uint32_t g_atarist_old_vbl;

static void Atarist_C2PInit(void);
void Atarist_VblHandler(void);
void Atarist_TimerBHandler(void);

static uint8_t *g_video_block;
static uint8_t *g_chunky;
static uint32_t g_old_timb_vec;
static uint8_t  g_old_tbcr;
static uint8_t  g_old_iera;
static uint8_t  g_old_imra;
static uint8_t  g_old_rez;
static uint32_t g_old_screen_base;
static uint32_t g_old_screenpt;
static uint16_t g_saved_palette[16];
static uint32_t g_last_present_vbl;

/* ── Machine detection ───────────────────────────────────────────────────── */

/* The cookie jar is a list of (tag, value) longword pairs terminated by a zero
 * tag; _p_cookies is 0 on a TOS that has none, in which case the machine can
 * only be a plain ST. */
static uint32_t st_cookie(uint32_t tag, uint32_t missing)
{
    const uint32_t *jar = (const uint32_t *)ST_R32(0x5a0);
    if (!jar) return missing;
    while (jar[0]) {
        if (jar[0] == tag) return jar[1];
        jar += 2;
    }
    return missing;
}

/* ── Palette packing ─────────────────────────────────────────────────────── */

uint16_t Atarist_PackRGB(uint8_t r, uint8_t g, uint8_t b)
{
    if (g_atarist_is_ste) {
        /* STE: four bits per channel, but the extra (least significant) bit
         * sits in bit 3 of the field rather than below bit 0 -- the field is
         * rotated right by one.  Getting this wrong is invisible on an ST and
         * turns every colour into a near neighbour on an STE. */
        unsigned rr = r >> 4, gg = g >> 4, bb = b >> 4;
        unsigned rs = ((rr >> 1) | ((rr & 1) << 3)) & 15;
        unsigned gs = ((gg >> 1) | ((gg & 1) << 3)) & 15;
        unsigned bs = ((bb >> 1) | ((bb & 1) << 3)) & 15;
        return (uint16_t)((rs << 8) | (gs << 4) | bs);
    }
    return (uint16_t)(((r >> 5) << 8) | ((g >> 5) << 4) | (b >> 5));
}

void Atarist_SetArenaPalette(const uint16_t *pal16)
{
    int i;
    for (i = 0; i < 16; ++i) g_atarist_pal_arena[i] = pal16[i];
}

void Atarist_SetCardPalette(const uint16_t *pal16)
{
    int i;
    for (i = 0; i < 16; ++i) g_atarist_pal_card[i] = pal16[i];
}

void Atarist_SetWholePalette(const uint16_t *pal16)
{
    Atarist_SetArenaPalette(pal16);
    Atarist_SetCardPalette(pal16);
}

void Atarist_SetSplitEnabled(int on)
{
    g_atarist_split_on = (uint8_t)(on ? 1 : 0);
}

/* ── Buffers ─────────────────────────────────────────────────────────────── */

uint8_t *Atarist_BackBuffer(void) { return g_atarist_back; }
uint8_t *Atarist_Chunky(void)     { return g_chunky; }

void Atarist_VideoWaitVbl(void)
{
    uint32_t t = g_atarist_vbl;
    while (g_atarist_vbl == t) { }
}

int Atarist_VideoPresent(void)
{
    uint8_t *finished = g_atarist_back;
    uint32_t now;
    int elapsed;

    /* The swap is two words wide on a 68000, so it is done with interrupts
     * masked: a VBL landing between them would program the shifter with half
     * of one buffer address and half of the other. */
    __asm__ volatile("move.w #0x2700,%%sr" : : : "cc");
    g_atarist_back  = g_atarist_front;
    g_atarist_front = finished;
    __asm__ volatile("move.w #0x2300,%%sr" : : : "cc");

    /* Wait for the VBL that actually latches it, so the game never draws into
     * a buffer the shifter is still reading. */
    Atarist_VideoWaitVbl();

    now = g_atarist_vbl;
    elapsed = (int)(now - g_last_present_vbl);
    g_last_present_vbl = now;
    if (elapsed < 1) elapsed = 1;
    return elapsed;
}

/* ── Clears ──────────────────────────────────────────────────────────────── */

void Atarist_ClearPlanarBand(int y0, int y1, uint8_t colour)
{
    uint16_t *p = (uint16_t *)(g_atarist_back + y0 * ATARIST_SCREEN_STRIDE);
    int rows = y1 - y0;
    int i;
    uint16_t w[4];
    w[0] = (colour & 1) ? 0xffffu : 0u;
    w[1] = (colour & 2) ? 0xffffu : 0u;
    w[2] = (colour & 4) ? 0xffffu : 0u;
    w[3] = (colour & 8) ? 0xffffu : 0u;
    if (rows <= 0) return;
    if (!colour) {
        uint32_t *q = (uint32_t *)p;
        int longs = rows * (ATARIST_SCREEN_STRIDE / 4);
        while (longs--) *q++ = 0u;
        return;
    }
    for (i = 0; i < rows * (ATARIST_SCREEN_W / 16); ++i) {
        *p++ = w[0]; *p++ = w[1]; *p++ = w[2]; *p++ = w[3];
    }
}

void Atarist_ClearPlanar(uint8_t colour)
{
    Atarist_ClearPlanarBand(0, ATARIST_SCREEN_H, colour);
}

/* ── Bring-up / tear-down ────────────────────────────────────────────────── */

int Atarist_VideoInit(void)
{
    uint32_t mch;
    uint8_t *block;
    uint32_t addr;
    int i;

    ATARIST_MARK(1);
    Atarist_C2PInit();
    ATARIST_MARK(2);

    mch = st_cookie(0x5f4d4348u /* '_MCH' */, 0u);
    g_atarist_is_ste = (uint8_t)(((mch >> 16) == 1u) ? 1 : 0);
    /* Blitmode(-1) bit 1 is "Blitter hardware present"; a TOS without XBIOS 64
     * returns a negative value, which is also "no blitter" as far as we care. */
    {
        int16_t bm = st_blitmode(-1);
        ATARIST_MARK(3);
        g_atarist_has_blitter = (uint8_t)((bm > 0 && (bm & 2)) ? 1 : 0);
    }

    /* Two 256-byte-aligned screens plus the chunky board buffer, out of one
     * allocation so a 512 KB machine fragments its TPA once, not three times. */
    block = (uint8_t *)st_malloc(ATARIST_SCREEN_BYTES * 2 + 256 +
                                 ATARIST_CHUNKY_BYTES);
    ATARIST_MARK(4);
    if (!block) return 0;
    addr = ((uint32_t)block + 255u) & ~255u;
    g_video_block = (uint8_t *)addr;
    g_atarist_back  = g_video_block;
    g_atarist_front = g_video_block + ATARIST_SCREEN_BYTES;
    g_chunky = g_video_block + ATARIST_SCREEN_BYTES * 2;

    for (i = 0; i < ATARIST_SCREEN_BYTES * 2; ++i) g_video_block[i] = 0;

    ATARIST_MARK(5);

    for (i = 0; i < 16; ++i) g_saved_palette[i] = ST_PALETTE[i];
    g_old_rez = ST_RESOLUTION;
    g_old_screen_base = ((uint32_t)ST_VIDBASE_HI << 16) |
                        ((uint32_t)ST_VIDBASE_MID << 8);
    g_old_screenpt = ST_SCREENPT;

    /* TOS's VBL rewrites the shifter base from _screenpt when it is non-zero,
     * which would undo every flip.  Zeroing it hands the register to us and
     * leaves the rest of the TOS VBL (the 200 Hz clock, floppy timeouts, the
     * VBL queue) running, which the disk loader still needs. */
    ST_SCREENPT = 0;

    ATARIST_MARK(7);
    ST_RESOLUTION = 0;             /* 320x200, 16 colours */
    if (g_atarist_is_ste) ST_LINEWIDTH = 0;

    /* A default palette so a frame drawn before the game sets one is visible
     * rather than sixteen shades of black. */
    for (i = 0; i < 16; ++i) {
        uint8_t v = (uint8_t)(i * 17);
        g_atarist_pal_arena[i] = g_atarist_pal_card[i] =
            Atarist_PackRGB(v, v, v);
    }

    g_atarist_split_on = 1;
    g_atarist_split_line = ATARIST_SPLIT_Y;

    __asm__ volatile("move.w #0x2700,%%sr" : : : "cc");
    g_old_timb_vec = ST_VEC_MFP_TIMB;
    g_old_tbcr = ST_MFP_TBCR;
    g_old_iera = ST_MFP_IERA;
    g_old_imra = ST_MFP_IMRA;
    g_atarist_old_vbl = ST_VEC_VBL;

    ST_VEC_MFP_TIMB = (uint32_t)Atarist_TimerBHandler;
    ST_MFP_TBCR = 0;
    ST_MFP_TBDR = ATARIST_SPLIT_Y;
    ST_MFP_IERA = (uint8_t)(g_old_iera | ST_MFP_IERA_TIMB);
    ST_MFP_IMRA = (uint8_t)(g_old_imra | ST_MFP_IERA_TIMB);
    ST_MFP_TBCR = 8;               /* event count mode */

    ST_VEC_VBL = (uint32_t)Atarist_VblHandler;
    __asm__ volatile("move.w #0x2300,%%sr" : : : "cc");

    ATARIST_MARK(8);
    g_last_present_vbl = g_atarist_vbl;
    return 1;
}

void Atarist_VideoShutdown(void)
{
    int i;
    __asm__ volatile("move.w #0x2700,%%sr" : : : "cc");
    ST_VEC_VBL = g_atarist_old_vbl;
    ST_MFP_TBCR = 0;
    ST_VEC_MFP_TIMB = g_old_timb_vec;
    ST_MFP_IERA = g_old_iera;
    ST_MFP_IMRA = g_old_imra;
    ST_MFP_TBCR = g_old_tbcr;
    ST_SCREENPT = g_old_screenpt;
    ST_VIDBASE_HI  = (uint8_t)(g_old_screen_base >> 16);
    ST_VIDBASE_MID = (uint8_t)(g_old_screen_base >> 8);
    ST_RESOLUTION = g_old_rez;
    for (i = 0; i < 16; ++i) ST_PALETTE[i] = g_saved_palette[i];
    __asm__ volatile("move.w #0x2300,%%sr" : : : "cc");
    if (g_video_block) st_mfree(g_video_block);
    g_video_block = 0;
}

/* ── C2P tables ───────────────────────────────────────────────────────────
 *  See atarist_c2p.S for the layout and for why the chunky buffer stores
 *  index*4.  The tables are sparse: 15 KB spanned, 256 entries live.  They are
 *  built here rather than shipped because 30 KB of floppy is worth more than
 *  the two milliseconds this costs at boot. */

#define ATARIST_C2P_TAB_BYTES (((60 << 8) | 60) + 4)

uint8_t g_atarist_c2p_tab_x2[ATARIST_C2P_TAB_BYTES];
uint8_t g_atarist_c2p_tab_x1[ATARIST_C2P_TAB_BYTES];

static void Atarist_C2PInit(void)
{
    int i, va, vb, p;

    for (i = 0; i < ATARIST_C2P_TAB_BYTES; ++i) {
        g_atarist_c2p_tab_x2[i] = 0;
        g_atarist_c2p_tab_x1[i] = 0;
    }
    for (va = 0; va < 16; ++va) {
        for (vb = 0; vb < 16; ++vb) {
            uint32_t e2 = 0u, e1 = 0u;
            int off = ((va * 4) << 8) | (vb * 4);
            for (p = 0; p < 4; ++p) {
                uint32_t a = (uint32_t)((va >> p) & 1);
                uint32_t b = (uint32_t)((vb >> p) & 1);
                /* Plane p lives in byte (3-p): plane 0 in the top byte, which
                 * is where MOVEP.L sends it -- offset 0, the plane-0 word. */
                e2 |= ((a ? 0xcu : 0u) | (b ? 0x3u : 0u)) << (8 * (3 - p));
                e1 |= ((a << 1) | b) << (8 * (3 - p));
            }
            *(uint32_t *)(g_atarist_c2p_tab_x2 + off) = e2;
            *(uint32_t *)(g_atarist_c2p_tab_x1 + off) = e1;
        }
    }
}
