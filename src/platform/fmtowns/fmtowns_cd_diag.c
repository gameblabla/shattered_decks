/* FM TOWNS CD-ROM diagnostics and read resilience.  See fmtowns_cd_diag.h
 * for why this is game code and not part of libfmt. */
#include "fmtowns_cd_diag.h"

#include "cdrom.h"
#include "fmt_pixel.h"   /* fmt_vram_singlepage_offset() -- the byte swizzle
                            the 256x240 8bpp single-page mode displays through */
#include "font_menudata.h" /* n2DLib_font, the core's 8x8 font (defined once,
                              in src/main.c) -- no second copy for this */
#include "io.h"
#include "iso9660.h"
#include "libfmt.h"
#include "machine.h"     /* g_fmt_vram0_base */
#include "pad.h"
#include "palette.h"
#include "waifu_assets.h" /* IDX_* indices into the game's common palette */

/* The CDC's master control/status port.  Named again here rather than
 * reached for through libfmt: this file only ever *reads* it, to put a
 * number on the failure report, and a diagnostic that pokes the driver's
 * registers would be a second driver. */
#define DIAG_CDC_MASTER_STATUS  0x4C0
#define DIAG_CDC_DRY            0x01

#define DIAG_W   256
#define DIAG_H   240
#define DIAG_COL 8   /* the font is 8x8, so a column is a character */
#define DIAG_ROW 10  /* 8 tall plus 2 of leading */

/* Attempts per read before the caller is told it failed.  Each one costs a
 * full pass through the driver's poll limits when the drive is not
 * answering -- on the order of ten seconds, since there is no timer to
 * bound them by wall clock in this freestanding environment (see
 * CD_POLL_LIMIT in cdrom.c).  Three is a compromise: enough that a
 * transient fault is absorbed silently, few enough that a dead drive
 * reaches the report screen inside a minute instead of never. */
#define DIAG_READ_TRIES  3

/* Vblanks the failure report waits before retrying on its own. */
#define DIAG_REPORT_FRAMES 1800  /* ~30s at 60Hz */

/* Vblanks a passing self-test stays up.  Long enough to read and to
 * photograph, short enough not to feel like a boot delay. */
#define DIAG_PASS_FRAMES   90

/*----------------------------------------------------------------------
 * Text into VRAM
 *
 * Deliberately not routed through the game's framebuffer.  A framebuffer
 * only reaches the screen when something presents it, and the case this
 * exists for is the case where control never comes back to do that -- so
 * these go straight into VRAM, into every page, and are on the glass the
 * instant the store retires.
 *--------------------------------------------------------------------*/

static uint8_t g_bg   = 0;
static uint8_t g_fg   = 1;   /* body text  */
static uint8_t g_hi   = 4;   /* headings   */
static uint8_t g_ok   = 2;   /* pass       */
static uint8_t g_bad  = 3;   /* fail       */

/* The self-test runs before the game core has loaded its palette, so it
 * brings its own -- five entries at the bottom of the palette, over the
 * zeroed VRAM fmt_set_mode() leaves behind (hence background index 0). */
static void diag_use_boot_palette(void)
{
    set_palette(0,   0,   0,   0);    /* background        */
    set_palette(1, 220, 220, 220);    /* body text         */
    set_palette(2,  60, 210,  90);    /* pass              */
    set_palette(3, 230,  70,  70);    /* fail              */
    set_palette(4, 240, 200,  60);    /* heading           */
    g_bg = 0; g_fg = 1; g_ok = 2; g_bad = 3; g_hi = 4;
}

/* Once the core owns the palette, borrow its indices instead of
 * overwriting entries the game is drawing with. */
static void diag_use_game_palette(void)
{
    g_bg = IDX_BLACK; g_fg = IDX_WHITE; g_ok = IDX_GREEN;
    g_bad = IDX_RED;  g_hi = IDX_GOLD_HI;
}

static void diag_glyph(uint32_t page_off, int x, int y, unsigned char ch,
                       uint8_t fg, uint8_t bg)
{
    const uint8_t *rows = n2DLib_font + ((uint32_t)(ch & 0x7fu) * 8u);
    int gy;

    for (gy = 0; gy < 8; ++gy) {
        uint8_t bits = rows[gy];
        uint32_t line = page_off + (uint32_t)(y + gy) * DIAG_W + (uint32_t)x;
        int gx;
        for (gx = 0; gx < 8; ++gx) {
            uint8_t c = (bits & (0x80u >> gx)) ? fg : bg;
            *(volatile uint8_t *)(g_fmt_vram0_base
                + fmt_vram_singlepage_offset(line + (uint32_t)gx)) = c;
        }
    }
}

/* Draws into every page the mode has, not just the one being drawn to.  A
 * hang stops the flip, so the page that would have shown this is not
 * necessarily the page on screen. */
static void diag_text_at(int x, int y, const char *s, uint8_t fg, uint8_t bg)
{
    uint32_t fb = fmt_frame_buffer_size();
    int pages = fmt_page_flipping_available() ? 2 : 1;
    int p;

    if (y < 0 || y + 8 > DIAG_H) {
        return;
    }
    /* This writes VRAM directly, behind the per-frame present.  Tell it so,
     * or fmt_put_image_dirty() keeps believing the pixels this just
     * overwrote are still what it last wrote there, and a breadcrumb from a
     * mid-game asset load stays burned into the bottom row of the screen
     * until something in the game happens to change those exact bytes. */
    fmt_invalidate_dirty_present();
    for (p = 0; p < pages; ++p) {
        uint32_t off = (uint32_t)p * fb;
        const char *c = s;
        int cx = x;
        for (; *c && cx + DIAG_COL <= DIAG_W; ++c, cx += DIAG_COL) {
            diag_glyph(off, cx, y, (unsigned char)*c, fg, bg);
        }
    }
}

static void diag_text(int col, int row, const char *s, uint8_t fg)
{
    diag_text_at(col * DIAG_COL, row * DIAG_ROW, s, fg, g_bg);
}

static void diag_clear(void)
{
    uint32_t fb = fmt_frame_buffer_size();
    int pages = fmt_page_flipping_available() ? 2 : 1;
    int p;

    for (p = 0; p < pages; ++p) {
        uint32_t off = (uint32_t)p * fb;
        uint32_t i;
        for (i = 0; i < (uint32_t)DIAG_W * DIAG_H; ++i) {
            *(volatile uint8_t *)(g_fmt_vram0_base
                + fmt_vram_singlepage_offset(off + i)) = g_bg;
        }
    }
}

/*----------------------------------------------------------------------
 * Formatting.  Freestanding: no snprintf worth the payload bytes here,
 * and every number this prints is a fixed-width hex or decimal field.
 *--------------------------------------------------------------------*/

static char *put_str(char *p, const char *end, const char *s)
{
    while (*s && p < end) {
        *p++ = *s++;
    }
    return p;
}

static char *put_hex(char *p, const char *end, uint32_t v, int digits)
{
    static const char hexdig[] = "0123456789ABCDEF";
    int i;
    for (i = digits - 1; i >= 0; --i) {
        if (p >= end) break;
        *p++ = hexdig[(v >> (i * 4)) & 0xfu];
    }
    return p;
}

static char *put_dec(char *p, const char *end, uint32_t v)
{
    char tmp[10];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v && n < (int)sizeof tmp);
    while (n > 0 && p < end) {
        *p++ = tmp[--n];
    }
    return p;
}

/*----------------------------------------------------------------------
 * What happened
 *--------------------------------------------------------------------*/

static struct {
    const char *stage;      /* what the game was loading */
    uint32_t last_lba;
    uint16_t last_count;
    uint8_t  last_tries;    /* attempts spent on the last failure */
    uint8_t  last_cdc_reg;  /* 0x4C0 sampled right after it failed */
    uint32_t reads;         /* fmt_cdrom_read() calls that succeeded */
    uint32_t sectors;
    uint32_t retries;       /* attempts beyond the first that were needed */
    uint32_t failures;      /* reads that used every attempt and still failed */
} g_cd = { "BOOT", 0, 0, 0, 0, 0, 0, 0, 0 };

static int g_breadcrumbs;

void fmtowns_cd_diag_stage(const char *what)
{
    g_cd.stage = what ? what : "?";
}

static void diag_breadcrumb_clear(void);

void fmtowns_cd_diag_breadcrumbs(int on)
{
    if (!on) {
        /* Wipe the last one on the way out, or it sits under the game's
         * first drawn frames until something happens to overwrite it. */
        diag_breadcrumb_clear();
    }
    g_breadcrumbs = on ? 1 : 0;
}

/* The line that is on screen while a read is in flight.  Bottom row, so it
 * cannot collide with the debug build's frame stamp (top-left pixels, see
 * fmtowns_main.c) or with the core's centred LOADING text. */
static void diag_breadcrumb(uint32_t lba, uint16_t count, int attempt)
{
    char line[32];
    char *p = line;
    const char *end = line + sizeof line - 1;

    if (!g_breadcrumbs) {
        return;
    }
    p = put_str(p, end, "RD ");
    p = put_hex(p, end, lba, 6);
    p = put_str(p, end, " N");
    p = put_hex(p, end, count, 2);
    p = put_str(p, end, " T");
    p = put_dec(p, end, (uint32_t)attempt + 1u);
    p = put_str(p, end, " ");
    p = put_str(p, end, g_cd.stage);
    *p = 0;
    diag_text_at(0, DIAG_H - 8, line, IDX_WHITE, IDX_BLACK);
}

static void diag_breadcrumb_clear(void)
{
    if (g_breadcrumbs) {
        diag_text_at(0, DIAG_H - 8, "                                ",
                     IDX_BLACK, IDX_BLACK);
    }
}

/*----------------------------------------------------------------------
 * The read wrapper
 *--------------------------------------------------------------------*/

/* Test hook.  Built with -DFMTOWNS_CD_DIAG_FORCE_FAIL=n (Makefile.fmtowns's
 * FMTOWNS_CD_FORCE_FAIL=n), the first n reads fail without the drive being
 * asked, and everything after them behaves normally.  It exists because the
 * failure this whole file is for cannot be produced on demand: the emulator
 * reads the disc perfectly, so the report screen and the recovery path would
 * otherwise ship having never been run.  n=1 is enough to see both. */
#ifndef FMTOWNS_CD_DIAG_FORCE_FAIL
#define FMTOWNS_CD_DIAG_FORCE_FAIL 0
#endif

int fmtowns_cd_diag_read(uint32_t lba, uint16_t count, void *buf)
{
    int attempt;

    g_cd.last_lba = lba;
    g_cd.last_count = count;

#if FMTOWNS_CD_DIAG_FORCE_FAIL
    {
        static unsigned forced = FMTOWNS_CD_DIAG_FORCE_FAIL;
        if (forced) {
            forced--;
            diag_breadcrumb(lba, count, 0);
            g_cd.last_tries = DIAG_READ_TRIES;
            g_cd.last_cdc_reg = (uint8_t)inb(DIAG_CDC_MASTER_STATUS);
            g_cd.retries += DIAG_READ_TRIES;
            g_cd.failures++;
            return -1;
        }
    }
#endif

    for (attempt = 0; attempt < DIAG_READ_TRIES; ++attempt) {
        diag_breadcrumb(lba, count, attempt);
        if (fmt_cdrom_read(lba, count, buf) == 0) {
            g_cd.reads++;
            g_cd.sectors += count;
            g_cd.last_tries = (uint8_t)(attempt + 1);
            return 0;
        }
        g_cd.retries++;
        /* Whatever the drive did or did not post, the next command has to
         * start from an empty status FIFO with the interrupt flags cleared,
         * or it will match on the wreckage of this one.  This is the whole
         * reason a bare retry is worth anything. */
        fmt_cdc_drain_status();
    }

    g_cd.last_tries = DIAG_READ_TRIES;
    g_cd.last_cdc_reg = (uint8_t)inb(DIAG_CDC_MASTER_STATUS);
    g_cd.failures++;
    return -1;
}

/*----------------------------------------------------------------------
 * Input for the report screens
 *--------------------------------------------------------------------*/

/* Pad bits are active low (see pad.h). */
static int diag_pressed(unsigned int mask)
{
    return (fmt_pad_read(0) & mask) != mask;
}

/* Waits for a fresh press, a frame at a time, so a button still held from
 * the previous screen does not count.  Returns the mask that was pressed,
 * or 0 if `frames` vblanks went by first. */
static unsigned int diag_wait_press(unsigned int mask, int frames,
                                    int countdown_col, int countdown_row)
{
    int i;
    int armed = 0;

    for (i = 0; i < frames; ++i) {
        fmt_wait_vsync();
        if (!diag_pressed(mask)) {
            armed = 1;
        } else if (armed) {
            unsigned int held = ~fmt_pad_read(0) & mask;
            return held ? held : mask;
        }
        if (countdown_row >= 0 && (i % 60) == 0) {
            char line[16];
            char *p = line;
            const char *end = line + sizeof line - 1;
            p = put_dec(p, end, (uint32_t)((frames - i) / 60));
            p = put_str(p, end, "S  ");
            *p = 0;
            diag_text(countdown_col, countdown_row, line, g_fg);
        }
    }
    return 0;
}

/*----------------------------------------------------------------------
 * Failure report
 *--------------------------------------------------------------------*/

static void diag_counters(int row)
{
    char line[34];
    char *p, *end = line + sizeof line - 1;

    p = put_str(line, end, "READS "); p = put_dec(p, end, g_cd.reads);
    p = put_str(p, end, "  SECTORS "); p = put_dec(p, end, g_cd.sectors);
    *p = 0;
    diag_text(1, row, line, g_fg);

    p = put_str(line, end, "RETRY "); p = put_dec(p, end, g_cd.retries);
    p = put_str(p, end, "  FAILED  "); p = put_dec(p, end, g_cd.failures);
    *p = 0;
    diag_text(1, row + 1, line, g_cd.failures ? g_bad : g_fg);
}

int fmtowns_cd_diag_report_failure(void)
{
    char line[34];
    char *p, *end = line + sizeof line - 1;
    unsigned int hit;

    diag_use_game_palette();
    diag_clear();

    diag_text(4, 2, "CD-ROM READ FAILED", g_bad);

    p = put_str(line, end, "LOADING  "); p = put_str(p, end, g_cd.stage);
    *p = 0;
    diag_text(1, 5, line, g_fg);

    p = put_str(line, end, "SECTOR   "); p = put_hex(p, end, g_cd.last_lba, 6);
    p = put_str(p, end, "  COUNT "); p = put_dec(p, end, g_cd.last_count);
    *p = 0;
    diag_text(1, 6, line, g_fg);

    p = put_str(line, end, "ATTEMPTS "); p = put_dec(p, end, g_cd.last_tries);
    p = put_str(p, end, "  REG4C0 "); p = put_hex(p, end, g_cd.last_cdc_reg, 2);
    *p = 0;
    diag_text(1, 7, line, g_fg);

    /* Bit 0 of 4C0 is the one worth naming in words: it is the drive
     * saying whether it can take a command at all, so it separates "the
     * disc did not read" from "nothing is answering on the bus". */
    diag_text(1, 8,
        (g_cd.last_cdc_reg == 0xff) ? "NO DRIVE RESPONDING"
      : (g_cd.last_cdc_reg & DIAG_CDC_DRY) ? "DRIVE READY, READ REFUSED"
                                           : "DRIVE BUSY OR STALLED",
        g_bad);

    diag_counters(10);

    diag_text(1, 13, "CHECK THE DISC AND THE LENS.", g_fg);
    diag_text(1, 15, "RUN   RETRY THIS READ", g_hi);
    diag_text(1, 16, "A     CONTINUE WITHOUT IT", g_hi);
    diag_text(1, 18, "RETRYING IN", g_fg);

    hit = diag_wait_press(FMT_PAD_RUN | FMT_PAD_A, DIAG_REPORT_FRAMES, 13, 18);
    if (hit & FMT_PAD_A) {
        return 0;
    }
    return 1;   /* RUN, or the countdown ran out: retry */
}

/*----------------------------------------------------------------------
 * Boot self-test
 *
 * The ordering is the point.  Each step is drawn, then run, then stamped,
 * so the screen always names the step that is executing -- if a step never
 * returns, its label is the diagnosis.  That is the whole answer to the
 * Marty freeze that reported nothing but "LOADING... TITLE 0%".
 *--------------------------------------------------------------------*/

static void step_label(int row, const char *text)
{
    diag_text(1, row, text, g_fg);
    diag_text(24, row, "....", g_fg);
}

static void step_result(int row, int ok)
{
    diag_text(24, row, ok ? "PASS" : "FAIL", ok ? g_ok : g_bad);
}

/* Row the failing self-test parked its "RETRYING IN" label on, so the
 * countdown lands next to it however long the report above it grew. */
static int g_selftest_countdown_row;

static int selftest_once(void)
{
    static uint8_t sector[2048] __attribute__((aligned(4)));
    uint32_t lba = 0, size = 0;
    uint8_t reg;
    int row = 4;
    int ok = 1;
    int step_ok;
    char line[34];
    char *p, *end = line + sizeof line - 1;

    diag_clear();
    diag_text(6, 1, "CD-ROM SELF TEST", g_hi);

    /* 1. Is anything there at all?  An empty ISA-style port reads back as
     *    0xFF, which is a different failure from a drive that answers and
     *    then refuses. */
    step_label(row, "CDC STATUS PORT");
    reg = (uint8_t)inb(DIAG_CDC_MASTER_STATUS);
    step_ok = (reg != 0xff);
    step_result(row, step_ok);
    p = put_str(line, end, "REG 4C0 = "); p = put_hex(p, end, reg, 2);
    p = put_str(p, end, (reg & DIAG_CDC_DRY) ? "  READY" : "  BUSY");
    *p = 0;
    diag_text(3, row + 1, line, g_fg);
    ok &= step_ok;
    row += 2;

    /* 2. The undocumented setup command on its own.  If the A0 theory is
     *    wrong, this is where it shows: the command that every read now
     *    depends on either answers or it does not. */
    step_label(row, "SETUP COMMAND A0");
    fmt_cdc_drain_status();
    step_ok = (fmt_cdc_setup_read() == 0);
    step_result(row, step_ok);
    ok &= step_ok;
    row += 1;

    /* 3. A real sector read, of the one sector every ISO9660 disc has. */
    step_label(row, "READ SECTOR 16");
    step_ok = (fmtowns_cd_diag_read(16, 1, sector) == 0);
    step_result(row, step_ok);
    ok &= step_ok;
    row += 1;

    /* 4. ...and did the bytes that came back mean anything?  A read that
     *    reports success but delivers garbage is its own failure mode, and
     *    the volume descriptor is a free checksum on it. */
    step_label(row, "VOLUME DESCRIPTOR");
    step_ok = step_ok && sector[1] == 'C' && sector[2] == 'D' &&
              sector[3] == '0' && sector[4] == '0' && sector[5] == '1';
    step_result(row, step_ok);
    ok &= step_ok;
    row += 1;

    /* 5. The directory walk the asset loader does for every blob. */
    step_label(row, "FIND TITLE.BIN");
    step_ok = (fmt_iso9660_find("TITLE.BIN", &lba, &size) == 0);
    step_result(row, step_ok);
    ok &= step_ok;
    row += 1;
    if (step_ok) {
        p = put_str(line, end, "LBA "); p = put_hex(p, end, lba, 6);
        p = put_str(p, end, "  BYTES "); p = put_dec(p, end, size);
        *p = 0;
        diag_text(3, row, line, g_fg);
    }
    row += 1;

    /* 6. And an asset read at a real LBA, which is what actually froze. */
    step_label(row, "READ TITLE.BIN");
    step_ok = step_ok && (fmtowns_cd_diag_read(lba, 1, sector) == 0);
    step_result(row, step_ok);
    ok &= step_ok;
    row += 2;

    diag_counters(row);
    row += 3;

    if (ok) {
        diag_text(1, row, "CD-ROM PATH OK", g_ok);
    } else {
        diag_text(1, row, "CD-ROM PATH FAILED", g_bad);
        diag_text(1, row + 2, "RUN   RUN THE TEST AGAIN", g_hi);
        diag_text(1, row + 3, "A     START THE GAME ANYWAY", g_hi);
        diag_text(1, row + 5, "RETRYING IN", g_fg);
        g_selftest_countdown_row = row + 5;
    }
    return ok;
}

int fmtowns_cd_diag_selftest(void)
{
    if (!fmt_current_mode()) {
        fmt_set_mode(FMT_MODE_256x240_8BPP);
    }
    diag_use_boot_palette();
    fmtowns_cd_diag_stage("SELF TEST");

    for (;;) {
        if (selftest_once()) {
            (void)diag_wait_press(FMT_PAD_RUN | FMT_PAD_A, DIAG_PASS_FRAMES,
                                  0, -1);
            return 1;
        }
        /* Failed.  Hold the report, then run the whole test again rather
         * than booting into a game whose assets are not there -- an
         * unattended machine keeps trying, and a present one can skip. */
        if (diag_wait_press(FMT_PAD_RUN | FMT_PAD_A, DIAG_REPORT_FRAMES,
                            13, g_selftest_countdown_row) & FMT_PAD_A) {
            return 0;
        }
    }
}
