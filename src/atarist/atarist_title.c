/* ─────────────────────────────────────────────────────────────────────────────
 *  atarist_title.c — the title screen.
 *
 *  The backdrop is the game's own title painting, converted by
 *  tools/atarist/gen_atarist_assets.py into 320x200 planar plus ONE PALETTE
 *  PER EIGHT SCANLINES.  The screen shows sixteen colours at a time and about
 *  four hundred over its height, which is what makes a photographic backdrop
 *  possible in ST low resolution at all.
 *
 *  Three palette entries mean the same thing in every band -- black, the
 *  highlight and white -- so the menu can be drawn anywhere over the picture
 *  without knowing which band a glyph landed in.  Everything else varies per
 *  band, and does.
 *
 *  The picture is a whole screen, so it is not blitted: it is copied over the
 *  back buffer, which is byte-for-byte the same layout.
 * ───────────────────────────────────────────────────────────────────────────── */

#include <stdint.h>

#include "atarist_title.h"
#include "atarist_video.h"
#include "atarist_draw.h"
#include "atarist_board3d.h"
#include "atarist_blitter.h"
#include "atarist_input.h"
#include "atarist_audio.h"
#include "atarist_disk.h"
#include "atarist_assets.h"
#include "atarist_probe.h"
#include "atarist_os.h"

/* Palette slots that are the same in every band. */
#define T_BLACK     0
#define T_HILITE   14
#define T_WHITE    15

/* The file: a header, one 8-bit RGB palette per band, then the bitmap.  RGB
 * rather than packed palette words because the same floppy boots an ST and an
 * STE, and only the running machine knows how many bits a channel has. */
#define TITLE_MAGIC  0x53545343u    /* 'STSC' */
#define TITLE_HDR    12
#define TITLE_BAND_RGB (2 + 16 * 3)
#define TITLE_MAX_BANDS (ATARIST_MAX_SPLITS - 1)
#define TITLE_FILE_MAX (TITLE_HDR + TITLE_MAX_BANDS * TITLE_BAND_RGB + \
                        ATARIST_SCREEN_BYTES)
/* The file is ZX0 packed and depacked in place, so the buffer carries the
 * depacker's slack past the picture -- see ATARIST_ZX0_SLACK. */
#define TITLE_BUFFER (TITLE_FILE_MAX + ATARIST_ZX0_SLACK)

/* The fallback gradient, for a floppy with no DAT\TITLE.SCR on it. */
#define FALLBACK_BANDS 12

#define MENU_ITEMS   2
#define MENU_Y      120
#define MENU_PITCH   16

/* The menu plate, in pixels.  Kept on 16-pixel boundaries so restoring it is
 * whole words out of the picture, which is what lets a cursor move repaint 44
 * rows instead of the whole screen. */
#define PLATE_X      64
#define PLATE_Y     (MENU_Y - 8)
#define PLATE_W     192
#define PLATE_H     (MENU_ITEMS * MENU_PITCH + 12)

/* The key legend gets a plate of its own.  Without one the two lines sat
 * straight on the painting, which is busy and light exactly there, and white
 * eight-pixel text with a one-pixel shadow does not survive that.  It never
 * changes, so it is painted with the backdrop and never restored. */
#define HELP_X       32
#define HELP_Y      168
#define HELP_W      256
#define HELP_H       28

static uint8_t  g_choice;
static uint8_t  g_quit;        /* Escape: the only way out of the game */
static uint8_t  g_cursor;
static uint8_t  g_redraw;      /* whole screen, both buffers */
static uint8_t  g_plate;       /* just the menu plate, both buffers */
static uint16_t g_anim;

static uint8_t *g_file;             /* the loaded TITLE.SCR, or null */
static const uint8_t *g_picture;    /* into g_file: the 32000-byte bitmap */

static const char *const g_menu[MENU_ITEMS] = {
    "FREE BATTLE",
    "STORY DUEL"
};

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

/* ── The picture ─────────────────────────────────────────────────────────── */

static int title_load(void)
{
    int32_t got;
    uint16_t w, h, bands, rows;
    static AtaristSplit splits[TITLE_MAX_BANDS];
    int b, i;
    const uint8_t *p;

    if (g_picture) return 1;
    if (!g_file) {
        g_file = (uint8_t *)st_malloc(TITLE_BUFFER);
        if (!g_file) return 0;
    }
    got = Atarist_DiskLoadPacked("DAT\\TITLE.SCR", g_file, TITLE_BUFFER);
    if (got < (int32_t)(TITLE_HDR + ATARIST_SCREEN_BYTES)) return 0;
    if (be32(g_file) != TITLE_MAGIC) return 0;

    w = be16(g_file + 4);
    h = be16(g_file + 6);
    bands = be16(g_file + 8);
    rows = be16(g_file + 10);
    if (w != ATARIST_SCREEN_W || h != ATARIST_SCREEN_H) return 0;
    if (bands == 0 || bands > TITLE_MAX_BANDS || rows == 0) return 0;
    if (got != (int32_t)(TITLE_HDR + bands * TITLE_BAND_RGB +
                         ATARIST_SCREEN_BYTES))
        return 0;

    p = g_file + TITLE_HDR;
    for (b = 0; b < bands; ++b) {
        splits[b].line = be16(p);
        p += 2;
        for (i = 0; i < 16; ++i, p += 3)
            splits[b].pal[i] = Atarist_PackRGB(p[0], p[1], p[2]);
    }
    Atarist_SetSplits(splits, bands);
    g_picture = p;
    return 1;
}

/* One step of the gradient the port drew before it had a title painting.  It
 * is still here because a floppy is allowed to ship without every file, and a
 * blind run that comes up black cannot say whether the game hung. */
static void fallback_palettes(void)
{
    static AtaristSplit splits[FALLBACK_BANDS];
    int t, i;
    for (t = 0; t < FALLBACK_BANDS; ++t) {
        int r = 24 + t * 16, g = 24 + t * 12, b = 96 - t * 4;
        if (r > 248) r = 248;
        if (g > 248) g = 248;
        if (b < 32) b = 32;
        splits[t].line = (uint16_t)(t * (ATARIST_SCREEN_H / FALLBACK_BANDS));
        for (i = 0; i < 16; ++i) splits[t].pal[i] = Atarist_PackRGB(0, 0, 0);
        splits[t].pal[1] = Atarist_PackRGB((uint8_t)r, (uint8_t)g, (uint8_t)b);
        splits[t].pal[T_HILITE] = Atarist_PackRGB(248, 232, 96);
        splits[t].pal[T_WHITE] = Atarist_PackRGB(248, 248, 248);
    }
    Atarist_SetSplits(splits, FALLBACK_BANDS);
}

static void paint_backdrop(void)
{
    uint8_t *dst = Atarist_BackBuffer();
    if (!g_picture) {
        int t;
        for (t = 0; t < FALLBACK_BANDS; ++t)
            Atarist_FillRect(0, t * (ATARIST_SCREEN_H / FALLBACK_BANDS),
                             ATARIST_SCREEN_W,
                             ATARIST_SCREEN_H / FALLBACK_BANDS, 1);
        return;
    }
    /* A whole screen of planar data in the shifter's own layout: one block
     * move, and the Blitter takes it when there is one. */
    if (Atarist_BlitterUsable(dst, g_picture, ATARIST_SCREEN_BYTES))
        Atarist_BlitterCopy(dst, g_picture, ATARIST_SCREEN_BYTES);
    else
        Atarist_ChunkyCopy(dst, g_picture, ATARIST_SCREEN_BYTES);
}

/* ── The screen ──────────────────────────────────────────────────────────── */

/* One horizontal band of the picture, back into the buffer.  The picture is
 * 32,000 bytes; repainting all of it to move a one-glyph caret is a vblank and
 * a half of copying. */
static void restore_plate(void)
{
    int row;
    const int groups = PLATE_W / 16;
    const int stride = ATARIST_SCREEN_W / 16 * 4;      /* words per row */
    uint16_t *dst = (uint16_t *)Atarist_BackBuffer() +
                    (size_t)PLATE_Y * stride + (PLATE_X / 16) * 4;
    const uint16_t *src = (const uint16_t *)g_picture +
                          (size_t)PLATE_Y * stride + (PLATE_X / 16) * 4;
    for (row = 0; row < PLATE_H; ++row) {
        int i;
        for (i = 0; i < groups * 4; ++i) dst[i] = src[i];
        dst += stride;
        src += stride;
    }
}

static void draw_menu(void)
{
    int i;

    /* A black plate behind the menu.  The painting is busy exactly where the
     * menu sits, and eight-pixel text with a one-pixel shadow does not survive
     * that on its own. */
    Atarist_FillRect(PLATE_X, PLATE_Y, PLATE_W, PLATE_H, T_BLACK);
    Atarist_FrameRect(PLATE_X, PLATE_Y, PLATE_W, PLATE_H, T_HILITE);

    for (i = 0; i < MENU_ITEMS; ++i) {
        int y = MENU_Y + i * MENU_PITCH;
        uint8_t c = (i == g_cursor) ? T_HILITE : T_WHITE;
        Atarist_DrawTextCentred(160, y, g_menu[i], c, T_BLACK);
        /* THE CARET DOES NOT BLINK.  It used to flip twice a second, and each
         * flip repainted the plate into both buffers -- so the menu box
         * flickered continuously, and walking the cursor through it landed in
         * the middle of that.  The highlighted row is already coloured
         * differently; a steady caret says the same thing and costs nothing. */
        if (i == g_cursor)
            Atarist_DrawText(72, y, ">", T_HILITE, T_BLACK);
    }
}

static void draw_help(void)
{
    Atarist_FillRect(HELP_X, HELP_Y, HELP_W, HELP_H, T_BLACK);
    Atarist_FrameRect(HELP_X, HELP_Y, HELP_W, HELP_H, T_HILITE);
    Atarist_DrawTextCentred(160, HELP_Y + 4, "CURSOR KEYS   SPACE SELECTS",
                            T_WHITE, T_BLACK);
    Atarist_DrawTextCentred(160, HELP_Y + 16, "ESC QUITS TO DESKTOP",
                            T_WHITE, T_BLACK);
}

static void title_draw(void)
{
    paint_backdrop();
    draw_menu();
    draw_help();
}

void Atarist_TitleEnter(void)
{
    g_choice = ATARIST_TITLE_NONE;
    g_quit = 0;
    g_cursor = 0;
    g_anim = 0;
    g_redraw = 2;
    g_plate = 0;
    if (!title_load()) {
        g_picture = 0;
        fallback_palettes();
    }
    Atarist_InputFlush();
    Atarist_ClearPlanar(T_BLACK);
    Atarist_MusicLoadTrack(ATARIST_MUSIC_TITLE);
    ATARIST_STAGE(ATARIST_STAGE_TITLE);
}

void Atarist_TitleStep(int vblanks)
{
    g_anim = (uint16_t)(g_anim + vblanks);
    /* NOTHING ON THIS SCREEN CHANGES BY ITSELF, so nothing is repainted until
     * the player moves the cursor -- and then only the menu plate. */
    if (Atarist_InputRepeat(ATARIST_BTN_UP, 14)) {
        g_cursor = (uint8_t)((g_cursor + MENU_ITEMS - 1) % MENU_ITEMS);
        g_plate = 2;
    }
    if (Atarist_InputRepeat(ATARIST_BTN_DOWN, 14)) {
        g_cursor = (uint8_t)((g_cursor + 1) % MENU_ITEMS);
        g_plate = 2;
    }
    if (g_atarist_input.pressed & (ATARIST_BTN_A | ATARIST_BTN_START))
        g_choice = (uint8_t)(ATARIST_TITLE_FREE_BATTLE + g_cursor);
    if (g_atarist_input.pressed & ATARIST_BTN_QUIT) g_quit = 1;

    /* Both counters run to two, because the screen is double buffered and a
     * single repaint leaves the other buffer one present behind. */
    if (g_redraw) {
        title_draw();
        --g_redraw;
    } else if (g_plate) {
        if (g_picture) restore_plate();
        draw_menu();
        --g_plate;
    }
    g_atarist_probe.menu_cursor = g_cursor;
}

int Atarist_TitleChoice(void) { return g_choice; }
int Atarist_TitleQuit(void)   { return g_quit; }
