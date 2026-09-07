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

static u8 board_res = SNES_RES_STILL;
static u8 present_row;          /* next framebuffer row to upload */
static u8 present_done;
/* The HUD band is uploaded ON REQUEST, not with every board frame.  It is
 * static between the events that change it, and at 4096 bytes it is two
 * thirds of a vblank's DMA budget -- paying that on every frame of a moving
 * board is what left the band showing the previous resolution's picture. */
static u8 hud_pending;

/* Rows per vblank for the still upload.  NTSC vblank is 38 lines, about
 * 51,800 master cycles, and a DMA moves roughly one byte per 8; 38 rows of 128
 * bytes is 4864 bytes, which leaves room for the OAM upload and the register
 * writes that share the window.  Three passes cover 112 rows. */
#define STILL_ROWS_PER_VBL  38

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

    REG_HDMAEN = 0x60;         /* channels 5 and 6 */
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

    /* Direct colour: the 8-bit texel IS the colour, BBGGGRRR, and CGRAM is
     * left entirely to the sprites. */
    REG_CGWSEL = CM_DIRCOLOR;
    REG_CGADSUB = 0x00;

    arm_hdma();
    snesVideoClear(0);
    present_row = 0;
    present_done = 0;
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
