/* ─────────────────────────────────────────────────────────────────────────────
 *  snes_m7fb.c — Mode 7 as a chunky framebuffer, and its presenter.
 *
 *  The bitmap itself is described in snes_fb.asm.  What lives here is the PPU
 *  state around it: the matrix, the per-band HDMA table that gives the board
 *  and the HUD different scales in the same frame, and the staged upload.
 *
 *  Why HDMA at all.  With B = C = 0 and the centre at the origin the Mode 7
 *  matrix is a pure scale: source x = A * screen_x, source y = D * screen_y +
 *  VOFS.  One A for the whole screen would mean that when the board drops to
 *  the 64x40 moving resolution the HUD text under it quadruples in size and
 *  stops fitting.  Three HDMA channels -- A, D and VOFS, two entries each --
 *  buy a different scale per band for the cost of a 12-byte table, and HDMA
 *  writes during active display are the one form of mid-frame PPU write the
 *  hardware is built for.
 * ───────────────────────────────────────────────────────────────────────────── */
#include <snes.h>
#include "snes_video.h"
#include "snes_stamp.h"

/* HDMA tables, mode 2 (two bytes into one write-twice register).  Each entry
 * is a line count then the 16-bit value; a count byte is 1..127, so the
 * 160-line board band needs two entries. */
static u8 hdma_a[10];
static u8 hdma_d[10];

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

/* The deck editor has no painted panel behind its text.  Its backdrop is a
 * purple ramp written once per scanline, so the screen stays a clean field of
 * colour while BG2 carries the labels and OBJ carries the cards and cursor. */
#define DECK_GRAD_LINES       224
#define DECK_GRAD_TOP_R        18
#define DECK_GRAD_TOP_B        28
#define DECK_GRAD_BOT_R         3
#define DECK_GRAD_BOT_B         8
#define DECK_GRAD_TABLE_BYTES (4 + (DECK_GRAD_LINES * 3))
#define DECK_COL_R             0x20
#define DECK_COL_B             0x80
static u8 hdma_deck_col[DECK_GRAD_TABLE_BYTES];

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

static u8 board_res = SNES_RES_STILL;
static u8 view = SNES_VIEW_BOARD;
static u8 view_pending = SNES_VIEW_BOARD;
static u8 present_row = 0;          /* next framebuffer row to upload */
static u8 present_done = 0;
/* The HUD band is uploaded ON REQUEST, not with every board frame.  It is
 * static between the events that change it, and at 4096 bytes it is two
 * thirds of a vblank's DMA budget -- paying that on every frame of a moving
 * board is what left the band showing the previous resolution's picture. */
static u8 hud_pending = 0;

/* Rows per vblank for the still upload.  NTSC vblank is 38 lines, about
 * 51,800 master cycles, and a DMA moves roughly one byte per 8, so the window
 * carries something under 6 KB in total and the whole of it is shared.
 *
 * THE SPRITE LAYER IS PAID FIRST AND THE BOARD GETS WHAT IS LEFT.  OAM is 544
 * bytes whenever the list changed and a card face is 512, so 24 rows -- 3072
 * bytes -- keeps the worst frame at about 4.1 KB and leaves room for the C
 * that drives all three.  Overspend and the rows past the end of the window
 * are simply not written: a black board under a framebuffer that is perfectly
 * correct in WRAM, which reads as a renderer bug and is not one. */
#define STILL_ROWS_PER_VBL  24

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

static void hdma_entry(u8 *t, u16 lines, u16 value)
{
    t[0] = (u8)lines;
    t[1] = (u8)(value & 0xFF);
    t[2] = (u8)(value >> 8);
}

/* Build the two-band table for the current resolution.
 *
 * Board band, lines 0..159: scale S (4.0 still, 2.0 moving), source row 0 at
 * screen line 0.
 * HUD band, lines 160..223: always 4.0, which maps those lines onto rows
 * 80..111 on its own.  M7VOFS stays zero for the whole screen -- see
 * SNES_HUD_ROW: a per-band VOFS written by HDMA applies to the entire frame,
 * not to its band, and shifts the board off the top of the screen. */
static void build_hdma(void)
{
    u16 scale;

    scale = (board_res == SNES_RES_STILL) ? SNES_M7_SCALE_STILL
                                          : SNES_M7_SCALE_MOVING;

    /* The board band is 160 lines and an HDMA line count tops out at 127. */
    hdma_entry(&hdma_a[0], 127, scale);
    hdma_entry(&hdma_a[3], SNES_BOARD_LINES - 127, scale);
    hdma_entry(&hdma_a[6], SNES_HUD_LINES, SNES_M7_SCALE_STILL);
    hdma_a[9] = 0;

    hdma_entry(&hdma_d[0], 127, scale);
    hdma_entry(&hdma_d[3], SNES_BOARD_LINES - 127, scale);
    hdma_entry(&hdma_d[6], SNES_HUD_LINES, SNES_M7_SCALE_STILL);
    hdma_d[9] = 0;

}

static void arm_hdma(void)
{
    build_hdma();

    REG_HDMAEN = 0;

    /* Channel 5: M7A ($211B).  Channel 6: M7D ($211E).  Both are write-twice
     * registers, hence transfer mode 2. */
    *(vuint8 *)0x4350 = 0x02;  *(vuint8 *)0x4351 = 0x1B;
    *(vuint16 *)0x4352 = (u16)(u16)&hdma_a[0];
    *(vuint8 *)0x4354 = 0x7E;

    *(vuint8 *)0x4360 = 0x02;  *(vuint8 *)0x4361 = 0x1E;
    *(vuint16 *)0x4362 = (u16)(u16)&hdma_d[0];
    *(vuint8 *)0x4364 = 0x7E;

    /* Channel 4: COLDATA ($2132), also write-twice, also mode 2. */
    build_gradient();
    *(vuint8 *)0x4340 = 0x02;  *(vuint8 *)0x4341 = 0x32;
    *(vuint16 *)0x4342 = (u16)(u16)&hdma_col[0];
    *(vuint8 *)0x4344 = 0x7E;

    REG_HDMAEN = 0x70;         /* channels 4, 5 and 6 */
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

static void arm_scene_hdma(void)
{
    build_scene_gradient();
    REG_HDMAEN = 0;

    /* Channel 4: COLDATA ($2132), two bytes per scanline. */
    *(vuint8 *)0x4340 = 0x02;
    *(vuint8 *)0x4341 = 0x32;
    *(vuint16 *)0x4342 = (u16)(u16)&hdma_scene_col[0];
    *(vuint8 *)0x4344 = 0x7E;

    /* Both scene assets leave the dialogue-box area transparent in BG1, so the
     * backdrop is the panel and BG2 supplies its border and text. */
    REG_CGWSEL = 0;
    REG_CGADSUB = 0x20;
    REG_HDMAEN = 0x10;           /* channel 4 only */
}

static void build_deck_gradient(void)
{
    u16 r = (u16)DECK_GRAD_TOP_R << 8;
    u16 b = (u16)DECK_GRAD_TOP_B << 8;
    const u16 r_step = (u16)((((u16)(DECK_GRAD_TOP_R - DECK_GRAD_BOT_R)) << 8) /
                             (DECK_GRAD_LINES - 1));
    const u16 b_step = (u16)((((u16)(DECK_GRAD_TOP_B - DECK_GRAD_BOT_B)) << 8) /
                             (DECK_GRAD_LINES - 1));
    u8 *t = hdma_deck_col;
    u16 i;

    /* Establish green explicitly on the first (non-visible) line.  The deck
     * can be entered from any scene, so the table must not depend on the
     * previous scene's fixed-colour state. */
    *t++ = 0x81;
    *t++ = COL_G;
    *t++ = COL_G;

    /* One line and two COLDATA bytes per entry: red and blue only, so green
     * is never introduced into the visible backdrop. */
    for (i = 0; i < DECK_GRAD_LINES; ++i) {
        *t++ = 0x81;
        *t++ = (u8)(DECK_COL_R | (r >> 8));
        *t++ = (u8)(DECK_COL_B | (b >> 8));
        if (i + 2 == DECK_GRAD_LINES) {
            r = (u16)DECK_GRAD_BOT_R << 8;
            b = (u16)DECK_GRAD_BOT_B << 8;
        } else {
            r = (r > r_step) ? (u16)(r - r_step) : 0;
            b = (b > b_step) ? (u16)(b - b_step) : 0;
        }
    }
    *t = 0;
}

static void arm_deck_hdma(void)
{
    build_deck_gradient();
    REG_HDMAEN = 0;

    /* Channel 4: COLDATA ($2132), two bytes per scanline. */
    *(vuint8 *)0x4340 = 0x02;
    *(vuint8 *)0x4341 = 0x32;
    *(vuint16 *)0x4342 = (u16)(u16)&hdma_deck_col[0];
    *(vuint8 *)0x4344 = 0x7E;

    REG_CGWSEL = 0;
    REG_CGADSUB = 0x20;           /* fixed colour on the backdrop only */
    REG_COLDATA = COL_BLACK;
    REG_COLDATA = COL_G;          /* explicitly clear green before the ramp */
    REG_COLDATA = (u8)(DECK_COL_R | DECK_GRAD_TOP_R);
    REG_COLDATA = (u8)(DECK_COL_B | DECK_GRAD_TOP_B);
    REG_HDMAEN = 0x10;
}

void snesVideoInitDuel(void)
{
    setScreenOff();                    /* force blank: $2100 = $8F */

    snesFbWriteChars();

    /* B = C = 0, centre at the origin, no scroll: the matrix is a pure scale
     * and HDMA supplies A, D and the vertical offset per band. */
    REG_M7SEL  = 0x00;                 /* wrap inside the 128x128 tile area */
    /* A and D are the per-band scale and HDMA rewrites them every scanline;
     * these writes are the state the first scanline inherits, and the state
     * the picture falls back to if a band table is ever not armed.
     *
     * $0400 is 4.0, not 0.5, and the factor of eight is the whole reason this
     * works: Mode 7 samples a 1024x1024 space of PIXELS, and a tilemap entry
     * covers 8x8 of them.  A = 4.0 therefore advances the tilemap by one entry
     * every two screen pixels, which is the 2x2 still texel. */
    REG_M7A = 0x00; REG_M7A = 0x04;      /* 0.5 in 8.8 */
    REG_M7B = 0x00; REG_M7B = 0x00;
    REG_M7C = 0x00; REG_M7C = 0x00;
    REG_M7D = 0x00; REG_M7D = 0x04;
    REG_M7X = 0x00; REG_M7X = 0x00;
    REG_M7Y = 0x00; REG_M7Y = 0x00;
    REG_M7HOFS = 0x00; REG_M7HOFS = 0x00;
    REG_M7VOFS = 0x00; REG_M7VOFS = 0x00;

    setMode(BG_MODE7, 0);
    REG_TM = 0x11;                     /* BG1 + OBJ on the main screen */
    REG_TMW = 0;
    REG_W12SEL = 0;

    /* The top view's background, set once and then left alone: Mode 7 ignores
     * both of these, so they are resident before the view change and never has
     * to touch them.  BG1 characters at word $6000 (BG12NBA counts in 4096
     * words) and its tilemap at $7000 (BG1SC counts in 1024). */
    REG_BG1SC   = 0x70;                /* $7000, 32x32 entries */
    REG_BG12NBA = 0x06;                /* $6000 */

    /* Direct colour: the 8-bit texel IS the colour, BBGGGRRR, and CGRAM is
     * left entirely to the sprites. */
    REG_CGWSEL = CM_DIRCOLOR;
    /* Colour math on the BACKDROP alone, adding the fixed colour: that is the
     * whole of the HUD plate.  Neither half-intensity nor subtract; the
     * backdrop is black, so the fixed colour arrives unmodified. */
    REG_CGADSUB = 0x20;

    snesVideoClear(0);
    present_row = 0;
    present_done = 0;
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

void snesVideoRestartDeckHdma(void)
{
    arm_deck_hdma();
}

void snesVideoSetView(u8 v)
{
    view_pending = v;
}

u8 snesVideoView(void) { return view; }

/* The whole mode change, and there is nothing else to it.
 *
 * $2105 picks the mode; $2130 takes direct colour off, because it applies to
 * ANY 256-colour background and Mode 3's BG1 is one -- left on, the top view's
 * table comes out as the raw palette indices read as BBGGGRRR; and $420C stops
 * the two HDMA channels that rewrite the Mode 7 matrix every scanline, which
 * in Mode 3 would be a hundred and sixty pointless writes a field.
 *
 * BG1SC, BG12NBA, TM and the scroll registers do NOT change: Mode 7 ignores
 * the first two, TM is BG1 + OBJ either way, and both scrolls are zero.  So
 * this runs inside vblank with room to spare and never blanks the screen. */
static void apply_view(void)
{
    if (view_pending == SNES_VIEW_TOP) {
        REG_HDMAEN = 0;
        REG_BGMODE = 0x03;
        REG_CGWSEL = 0x00;
    } else {
        REG_BGMODE = BG_MODE7;
        REG_CGWSEL = CM_DIRCOLOR;
        REG_HDMAEN = 0x70;
    }
    view = view_pending;
}

void snesVideoSetBoardRes(u8 res)
{
    if (res == board_res) return;
    board_res = res;
    arm_hdma();
    snesVideoPresentRestart();
}

u8 snesVideoBoardRes(void) { return board_res; }

/* Whether the frame in the buffer is entirely on screen.
 *
 * A still frame takes three vblanks to upload and every render restarts that
 * upload at the top, so a scene that redraws on every game frame would show
 * only the first thirty-eight rows -- for ever.  Presentation asks this before
 * it animates in the still resolution.
 *
 * A pending HUD band counts as not done, and that is not pedantry: the board's
 * last pass sets present_done and leaves the HUD for the vblank after it, so a
 * caller that redrew on present_done alone restarted the upload in between and
 * the panel never reached the screen at all. */
u8 snesVideoPresentDone(void) { return present_done && !hud_pending; }

void snesVideoPresentRestart(void)
{
    present_row = 0;
    present_done = 0;
}

void snesVideoHudDirty(void)
{
    hud_pending = 1;
}

/* One vblank's worth of upload.  Returns non-zero once the whole frame is on
 * screen.
 *
 * Mode 7 has no second tilemap base, so there is no page flip: this staged
 * upload IS the double-buffering story, and it is why the moving frame is
 * sized to fit a single vblank.  A still frame is only produced when the board
 * has settled, so refreshing an unchanging image over three vblanks is
 * invisible. */
u8 snesVideoPresent(void)
{
    u16 rows, width, total;

    if (view != view_pending) apply_view();

    /* THE TOP VIEW DOES NOT PRESENT THE BITMAP.  Its picture is a resident
     * tilemap and its cards are sprites, so the bitmap in VRAM is simply the
     * board the player walked up from -- still there, untouched, and back on
     * screen the instant they walk down again.  The vblank goes to the card
     * sprites instead, which is why entering the top view fills with cards in
     * a handful of fields. */
    if (view == SNES_VIEW_TOP) return 1;

    if (present_done) {
        /* The board is up; a requested HUD refresh gets the next vblank to
         * itself rather than sharing one with 2560 bytes of board. */
        if (hud_pending) {
            snesVideoPresentHud();
            hud_pending = 0;
        }
        return 1;
    }

    if (board_res == SNES_RES_STILL) {
        /* Board rows 0..79, full stride: 10240 bytes over three vblanks. */
        total = SNES_STILL_H;
        width = SNES_FB_STRIDE;
        rows = STILL_ROWS_PER_VBL;
    } else {
        /* The moving board is 64 texels inside a 128 stride, so it is a
         * windowed upload: 40 short rows, 2560 bytes, one vblank. */
        total = SNES_MOVING_H;
        width = SNES_MOVING_W;
        rows = SNES_MOVING_H;
    }

    if (present_row + rows > total) rows = (u8)(total - present_row);
    snesFbPresentRows(present_row, rows, width);
    present_row += (u8)rows;

    if (present_row >= total) {
        present_done = 1;
        return hud_pending ? 0 : 1;  /* one more pass if the HUD changed */
    }
    return 0;
}

void snesVideoPresentHud(void)
{
    snesFbPresentRows(SNES_HUD_ROW, SNES_HUD_H, SNES_HUD_W);
}

void snesVideoClear(u8 colour)
{
    /* One span, not a C loop: the same 16384 bytes cost 12.3 million master
     * cycles through 816-tcc's indexed store (measured, 9056 scanlines) and
     * about a hundred thousand through the span filler's 16-bit stores. */
    snesSpanFill(0, SNES_FB_STRIDE * SNES_FB_ROWS, colour);
}
