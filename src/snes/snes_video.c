/* ─────────────────────────────────────────────────────────────────────────────
 *  snes_video.c — the PPU state around the Mode 3 Direct Colour board.
 *
 *  The frame, the ring, the tile store and the upload live in snes_fb.asm;
 *  what lives here is the register model: Mode 3 with BG1 in direct colour,
 *  the two HDMA channels the duel runs (the HUD plate's COLDATA ramp and the
 *  TM write that keeps BG1 off the HUD band), the presentation owner, and the
 *  board's frame generations.  The other scenes' HDMA gradients are here as
 *  well because they share the fixed-colour idea.
 * ───────────────────────────────────────────────────────────────────────────── */
#include <snes.h>
#include "snes_video.h"
#include "snes_stamp.h"

/* The Mode 3 scenes use the same fixed-colour HDMA idea as the duel, but the
 * dialogue window owns the whole bottom eighty scanlines rather than only the
 * duel's text rows.  It gets its own table so the board's backdrop remains
 * black above line 197. */
#define SCENE_GRAD_LEAD       144
#define SCENE_GRAD_LINES       80
#define SCENE_GRAD_TOP_RG       5
#define SCENE_GRAD_TOP_B       27
#define SCENE_GRAD_BOT_RG       1
#define SCENE_GRAD_BOT_B         9
#define SCENE_GRAD_TABLE_BYTES (6 + 1 + (SCENE_GRAD_LINES * 2) + 1)
static u8 hdma_scene_col[SCENE_GRAD_TABLE_BYTES];

/* ── The HUD plate ───────────────────────────────────────────────────────── */

/* THE BLUE PLATE UNDER THE TEXT ROWS IS THE BACKDROP, TINTED PER SCANLINE.
 *
 * It used to be painted into the bitmap, and the bitmap is the one surface on
 * this screen that cannot hold a gradient: direct colour is BBGGGRRR, so blue
 * has FOUR levels and the band samples 2x2, so a "ramp" was three blues over
 * thirteen fat two-line steps.  It banded visibly.
 *
 * So the plate is not drawn at all any more.  The bitmap's HUD rows are left
 * transparent, which shows CGRAM 0 -- and colour math adds the FIXED COLOUR to
 * the backdrop, at five bits a channel, from a register HDMA rewrites every
 * scanline.  That is one line per line and 32768 colours to choose from
 * instead of two-line steps out of a 256-entry cube, for a 63-byte table and a
 * third HDMA channel.  Nothing is drawn, so it also gives the band's thirteen
 * span fills back.
 *
 * $2132 is COLDATA, written as %BGRiiiii: the top three bits say WHICH
 * channels take the five-bit intensity, so a colour needs two writes and the
 * channel is transfer mode 2 (two bytes into one write-twice register), the
 * same shape as the two matrix channels.  Red and green are set TOGETHER by
 * the first byte -- they are equal all the way down the plate, which is what
 * makes it a blue that pales rather than a blue that turns cyan -- and blue by
 * the second.
 *
 * The text on the plate is sprites and colour math is enabled for the backdrop
 * ONLY ($2131 = $20), so the words, the icons and the hand above them are
 * untouched by it; and outside the plate's lines the fixed colour is black, so
 * adding it to the backdrop is what the rest of the screen already was. */
#define GRAD_LINES   26                 /* screen lines 197..222 */
#define GRAD_LEAD    197                /* black above it */
#define GRAD_TAIL    8                  /* black under it, line 223 included */
#define COL_BLACK    0xE0               /* R, G and B all to intensity zero */
#define COL_RG       0x60               /* this byte sets red and green */
#define COL_G        0x40               /* this byte sets green */
#define COL_B        0x80               /* this byte sets blue */

/* The rule along the top edge, then the ramp under it.  The rule is a separate
 * colour and not the ramp's first step because an edge is what makes the plate
 * read as a panel rather than as a fog the words are sinking into. */
#define GRAD_RULE_RG   13
#define GRAD_RULE_B    31
#define GRAD_TOP_RG    7
#define GRAD_TOP_B     27
#define GRAD_BOT_B     9

/* 3 + 3 for the lead, 1 + 2*26 for the ramp, 3 for the tail, 1 terminator. */
static u8 hdma_col[63];

static u8 presentation_owner = SNES_OWNER_BOARD;
static u16 requested_generation = 0;

/* Fill the plate's table once.  The ramp is walked in 8.8 rather than divided
 * per line: 816-tcc has no divide worth spending here, and both steps happen
 * to be exact enough that the ends land on the values above. */
static void build_gradient(void)
{
    u16 rg = (u16)GRAD_TOP_RG << 8;
    u16 b  = (u16)GRAD_TOP_B << 8;
    /* Over the twenty-four intervals between the ramp's twenty-five lines. */
    const u16 rg_step = (u16)(((u16)GRAD_TOP_RG << 8) / (GRAD_LINES - 2));
    const u16 b_step  = (u16)((((u16)(GRAD_TOP_B - GRAD_BOT_B)) << 8)
                              / (GRAD_LINES - 2));
    u8 *t = hdma_col;
    u8 i;

    /* Lines 0..196: black.  A line count tops out at 127. */
    *t++ = 127;         *t++ = COL_BLACK; *t++ = COL_BLACK;
    *t++ = GRAD_LEAD - 127; *t++ = COL_BLACK; *t++ = COL_BLACK;

    /* Lines 197..222, a line at a time: the repeat flag says the entry carries
     * its own data for each of the lines it counts. */
    *t++ = (u8)(0x80 | GRAD_LINES);
    *t++ = (u8)(COL_RG | GRAD_RULE_RG);
    *t++ = (u8)(COL_B  | GRAD_RULE_B);
    for (i = 1; i < GRAD_LINES; ++i) {
        *t++ = (u8)(COL_RG | (rg >> 8));
        *t++ = (u8)(COL_B  | (b >> 8));
        rg = (rg > rg_step) ? (u16)(rg - rg_step) : 0;
        b -= b_step;
    }

    /* Line 223 and the lines past the visible screen.  Without this the
     * register simply keeps the ramp's last colour, and the bottom line of the
     * screen -- which the band's rows cannot reach -- comes out dark blue. */
    *t++ = GRAD_TAIL;   *t++ = COL_BLACK; *t++ = COL_BLACK;
    *t   = 0;
}

/* The duel's two channels.
 *
 * Channel 4: COLDATA, the HUD plate (above).
 * Channel 6: TM, BG1 + OBJ over the board's 144 lines and OBJ alone under
 *            them.
 * Channels 1-3 are never used: their registers are reserved as fast scratch
 * for the renderer (SNES_MODE3_PLAN.md 2.3).  Channel 0 is the main thread's
 * general DMA and 7 the NMI drain's. */
static void arm_hdma(void)
{
    REG_HDMAEN = 0;

    build_gradient();
    *(vuint8 *)0x4340 = 0x02;  *(vuint8 *)0x4341 = 0x32;
    *(vuint16 *)0x4342 = (u16)(u16)&hdma_col[0];
    *(vuint8 *)0x4344 = 0x7E;

    *(vuint8 *)0x4360 = 0x00;  *(vuint8 *)0x4361 = 0x2C;
    *(vuint16 *)0x4362 = (u16)(u16)snes_fb_tm;
    *(vuint8 *)0x4364 = 0x7E;

    REG_HDMAEN = 0x50;         /* channels 4 and 6 */
}

static void build_scene_gradient(void)
{
    u16 rg = (u16)SCENE_GRAD_TOP_RG << 8;
    u16 b  = (u16)SCENE_GRAD_TOP_B << 8;
    const u16 rg_step = (u16)((((u16)(SCENE_GRAD_TOP_RG -
                                      SCENE_GRAD_BOT_RG)) << 8) /
                              (SCENE_GRAD_LINES - 1));
    const u16 b_step = (u16)((((u16)(SCENE_GRAD_TOP_B -
                                     SCENE_GRAD_BOT_B)) << 8) /
                             (SCENE_GRAD_LINES - 1));
    u8 *t = hdma_scene_col;
    u8 i;

    /* Leave the painted scene untouched above the dialogue window. */
    *t++ = 127;                   *t++ = COL_BLACK; *t++ = COL_BLACK;
    *t++ = SCENE_GRAD_LEAD - 127; *t++ = COL_BLACK; *t++ = COL_BLACK;

    /* One fixed-colour pair per scanline gives the same smooth ramp as the
     * duel, without the Mode 7 framebuffer's four-level blue limitation. */
    *t++ = (u8)(0x80 | SCENE_GRAD_LINES);
    *t++ = (u8)(COL_RG | (rg >> 8));
    *t++ = (u8)(COL_B  | (b >> 8));
    for (i = 1; i < SCENE_GRAD_LINES; ++i) {
        rg = (rg > rg_step) ? (u16)(rg - rg_step) : 0;
        b  = (b > b_step) ? (u16)(b - b_step) : 0;
        *t++ = (u8)(COL_RG | (rg >> 8));
        *t++ = (u8)(COL_B  | (b >> 8));
    }
    *t = 0;
}

/* THE STORY SKY IS THE BACKDROP, TINTED PER SCANLINE -- the HUD plate's trick.
 *
 * The dialogue scene's picture has no sky tiles at all: above the ground the
 * map is transparent, what shows is the backdrop, and the backdrop is black
 * plus the fixed colour.  Two more channels rewrite COLDATA every line of the
 * sky: one in mode 2 for red and green, one in mode 0 for blue, so each line
 * gets all three components (channel 4's own lead entries write black to
 * those lines first; the higher channels run after it in the same HBlank).
 * Both tables end after the sky, so the dialogue plate's ramp below is
 * channel 4's alone.  snes_scene.c builds the tables; NULL means no sky. */
static u16 scene_sky_rg = 0;
static u8  scene_sky_rg_bank = 0;
static u16 scene_sky_b = 0;
static u8  scene_sky_b_bank = 0;

void snesVideoSetSkyTables(u16 rg, u8 rg_bank, u16 b, u8 b_bank)
{
    scene_sky_rg = rg;
    scene_sky_rg_bank = rg_bank;
    scene_sky_b = b;
    scene_sky_b_bank = b_bank;
}

static void arm_scene_hdma(void)
{
    u8 channels = 0x10;
    build_scene_gradient();
    REG_HDMAEN = 0;

    /* Channel 4: COLDATA ($2132), two bytes per scanline. */
    *(vuint8 *)0x4340 = 0x02;
    *(vuint8 *)0x4341 = 0x32;
    *(vuint16 *)0x4342 = (u16)(u16)&hdma_scene_col[0];
    *(vuint8 *)0x4344 = 0x7E;

    if (scene_sky_rg && scene_sky_b) {
        /* Channels 5 and 6: the sky's red/green pair and its blue.  The
         * caller names each table's bank: the scene's statics are not all
         * in $7E, and a pointer variable does not carry its bank through
         * this compiler reliably. */
        *(vuint8 *)0x4350 = 0x02;
        *(vuint8 *)0x4351 = 0x32;
        *(vuint16 *)0x4352 = scene_sky_rg;
        *(vuint8 *)0x4354 = scene_sky_rg_bank;
        *(vuint8 *)0x4360 = 0x00;
        *(vuint8 *)0x4361 = 0x32;
        *(vuint16 *)0x4362 = scene_sky_b;
        *(vuint8 *)0x4364 = scene_sky_b_bank;
        channels |= 0x60;
    }

    /* Both scene assets leave the dialogue-box area transparent in BG1, so the
     * backdrop is the panel and BG2 supplies its border and text. */
    REG_CGWSEL = 0;
    REG_CGADSUB = 0x20;
    REG_HDMAEN = channels;
}

/* THE TITLE MENU'S PLATE IS COLOUR MATH INSIDE A WINDOW.
 *
 * There is no VRAM left on the title for plate tiles -- the whole painting
 * is resident -- and none is needed: colour window 1 covers the menu's
 * rectangle, BG1 is averaged with a fixed blue inside it, and the painting
 * becomes a translucent blue plate under the BG2 text.  The window's left
 * and right edges are HDMA'd so the box has a top and a bottom. */
#define TITLE_BOX_X0     56
#define TITLE_BOX_X1     215
#define TITLE_BOX_Y0     160
#define TITLE_BOX_Y1     216
static u8 hdma_title_win[16];

static void build_title_window(void)
{
    u8 *t = hdma_title_win;
    /* Above the box: an empty window (left past right). */
    *t++ = 127;                   *t++ = 255; *t++ = 0;
    *t++ = TITLE_BOX_Y0 - 127;    *t++ = 255; *t++ = 0;
    *t++ = TITLE_BOX_Y1 - TITLE_BOX_Y0; *t++ = TITLE_BOX_X0; *t++ = TITLE_BOX_X1;
    *t++ = 224 - TITLE_BOX_Y1;    *t++ = 255; *t++ = 0;
    *t = 0;
}

void snesVideoTitleMenuPlate(u8 on)
{
    REG_HDMAEN = 0;
    if (!on) {
        REG_CGWSEL = 0;
        REG_CGADSUB = 0;
        REG_WOBJSEL = 0;
        return;
    }
    build_title_window();
    /* Channel 4, mode 1: WH0 and WH1 ($2126, $2127), one byte each. */
    {
        const u8 *src = hdma_title_win;
        *(vuint8 *)0x4340 = 0x01;
        *(vuint8 *)0x4341 = 0x26;
        *(vuint16 *)0x4342 = (u16)src;
        *(vuint8 *)0x4344 = ((const u8 *)&src)[2];
    }
    REG_WOBJSEL = 0x20;           /* colour window 1 on, not inverted */
    REG_CGWSEL = 0x10;            /* colour math inside the window only */
    REG_COLDATA = 0x20 | 3;       /* the plate's blue: averaged with the */
    REG_COLDATA = 0x40 | 8;       /* painting, so it reads as a translucent */
    REG_COLDATA = 0x80 | 26;      /* blue pane over it, not a black hole */
    REG_CGADSUB = 0x41;           /* (BG1 + fixed colour) / 2 */
    REG_HDMAEN = 0x10;
}

void snesVideoInitDuel(void)
{
    static const u16 blank[32] = { 0 };
    u16 row;

    setScreenOff();                    /* force blank: $2100 = $8F */

    snesFbInit();

    setMode(BG_MODE3, 0);
    presentation_owner = SNES_OWNER_BOARD;
    requested_generation = 0;

    /* Permanent blank tile and two blank logical maps. */
    dmaCopyVram((u8 *)blank, SNES_VRAM_BOARD_CHARS, 64);
    for (row = 0; row < 32; ++row) {
        dmaCopyVram((u8 *)blank, (u16)(SNES_VRAM_BOARD_MAP_A + row * 32), 64);
        dmaCopyVram((u8 *)blank, (u16)(SNES_VRAM_BOARD_MAP_B + row * 32), 64);
    }

    REG_TM = 0x11;                     /* BG1 + OBJ on the main screen */
    REG_TMW = 0;
    REG_W12SEL = 0;
    REG_BG1SC   = 0x58;
    REG_BG12NBA = (u8)(SNES_VRAM_BOARD_CHARS >> 12);        /* 0 */
    REG_BG1HOFS = 0; REG_BG1HOFS = 0;
    REG_BG1VOFS = 0xFF; REG_BG1VOFS = 0x03;

    /* Direct colour: the 8-bit texel IS the colour, BBGGGRRR, and CGRAM is
     * left entirely to the sprites. */
    REG_CGWSEL = CM_DIRCOLOR;
    /* Colour math on the BACKDROP alone, adding the fixed colour: that is the
     * whole of the HUD plate.  Neither half-intensity nor subtract; the
     * backdrop is black, so the fixed colour arrives unmodified. */
    REG_CGADSUB = 0x20;

}

void snesVideoRestartHdma(void)
{
    REG_TMW = 0;
    REG_W12SEL = 0;
    REG_COLDATA = COL_BLACK;
    arm_hdma();
}

void snesVideoRestartSceneHdma(void)
{
    REG_COLDATA = COL_BLACK;
    arm_scene_hdma();
}

void snesVideoSetOwner(u8 owner) { presentation_owner = owner; }
u8 snesVideoOwner(void) { return presentation_owner; }

u16 snesVideoRequestGeneration(void)
{
    ++requested_generation;
    if (!requested_generation) ++requested_generation;
    return requested_generation;
}

u16 snesVideoPresentedGeneration(void) { return snesFbPresentedGeneration(); }

/* Whether everything queued for VRAM has gone up. */
u8 snesVideoPresentDone(void)
{
    return snesFbJobsPending() == 0 && snesFbFramesPending() == 0;
}

u8 snesVideoPresent(void)
{
    return snesVideoPresentDone();
}
