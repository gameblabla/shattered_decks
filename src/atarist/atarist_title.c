/* ─────────────────────────────────────────────────────────────────────────────
 *  atarist_title.c — the title screen.
 *
 *  Everything here is drawn planar at the full 320x200; there is no 3D and no
 *  C2P.  The colour comes from the raster split list instead: TITLE_BANDS
 *  palettes down the screen, each one a step of the same gradient, which puts
 *  roughly forty distinct colours on a sixteen-colour screen.
 *
 *  Only three palette entries mean the same thing in every band -- black, the
 *  text white and the highlight -- so text and the menu can be drawn anywhere
 *  without caring which band they land in.  Everything else is free to vary,
 *  and does.
 * ───────────────────────────────────────────────────────────────────────────── */

#include <stdint.h>

#include "atarist_title.h"
#include "atarist_video.h"
#include "atarist_draw.h"
#include "atarist_input.h"
#include "atarist_audio.h"
#include "atarist_disk.h"
#include "atarist_probe.h"

/* Palette slots that are the same in every band. */
#define T_BLACK     0
#define T_BACK      1      /* the gradient step -- different in every band */
#define T_BACK2     2      /* its darker companion, for the banner */
#define T_ACCENT    3
#define T_WHITE    14
#define T_HILITE   15

#define TITLE_BANDS      12
#define TITLE_BAND_ROWS  (ATARIST_SCREEN_H / TITLE_BANDS)

#define MENU_ITEMS   2
#define MENU_Y      120
#define MENU_PITCH   16

static uint8_t  g_choice;
static uint8_t  g_cursor;
static uint8_t  g_redraw;
static uint16_t g_anim;

static const char *const g_menu[MENU_ITEMS] = {
    "FREE BATTLE",
    "STORY DUEL"
};

/* One gradient step.  `t` runs 0..TITLE_BANDS-1 from the top of the screen:
 * a deep blue night at the top falling to a warm horizon at the bottom, which
 * is the same palette family the arena uses so the cut to the duel does not
 * look like a different game. */
static void band_palette(int t, uint16_t *pal)
{
    int i;
    int r = 24 + t * 16;
    int g = 24 + t * 12;
    int b = 96 - t * 4;
    if (r > 248) r = 248;
    if (g > 248) g = 248;
    if (b < 32) b = 32;

    for (i = 0; i < 16; ++i) pal[i] = Atarist_PackRGB(0, 0, 0);
    pal[T_BACK]   = Atarist_PackRGB((uint8_t)r, (uint8_t)g, (uint8_t)b);
    pal[T_BACK2]  = Atarist_PackRGB((uint8_t)(r / 2), (uint8_t)(g / 2),
                                    (uint8_t)(b / 2));
    pal[T_ACCENT] = Atarist_PackRGB((uint8_t)((r + 248) / 2),
                                    (uint8_t)((g + 200) / 2),
                                    (uint8_t)((b + 96) / 2));
    pal[T_WHITE]  = Atarist_PackRGB(248, 248, 248);
    pal[T_HILITE] = Atarist_PackRGB(248, 232, 96);
    return;
}

static void title_palettes(void)
{
    static AtaristSplit splits[TITLE_BANDS];
    int t;
    for (t = 0; t < TITLE_BANDS; ++t) {
        splits[t].line = (uint16_t)(t * TITLE_BAND_ROWS);
        band_palette(t, splits[t].pal);
    }
    Atarist_SetSplits(splits, TITLE_BANDS);
}

/* The name, drawn twice the size the 8x8 font gives, by stamping each glyph
 * into a scratch band and then doubling it.  Doing it with the font rather
 * than with a bitmap keeps the floppy free of a logo the game does not have
 * yet, and the whole screen still costs one repaint. */
static void draw_banner(const char *text, int cy, uint8_t colour, uint8_t shadow)
{
    int w = Atarist_TextWidth(text);
    int x = (160 - w / 2) & ~7;
    /* Doubling in x would need a second buffer; doubling in y is free, because
     * two passes one pixel apart read as a heavier face at this size. */
    Atarist_DrawText(x, cy, text, colour, shadow);
    Atarist_DrawText(x, cy + 1, text, colour, shadow);
}

static void title_draw(void)
{
    int t, i;

    /* The gradient itself: one solid band per palette.  Index T_BACK is a
     * different colour in each, which is the whole trick. */
    for (t = 0; t < TITLE_BANDS; ++t)
        Atarist_FillRect(0, t * TITLE_BAND_ROWS, ATARIST_SCREEN_W,
                         TITLE_BAND_ROWS, T_BACK);

    /* A darker plate behind the name and the menu, so the text keeps its
     * contrast where the gradient is brightest. */
    Atarist_FillRect(32, 40, 256, 40, T_BACK2);
    Atarist_FillRect(64, MENU_Y - 8, 192, MENU_ITEMS * MENU_PITCH + 12, T_BACK2);

    draw_banner("SHATTERED DECKS", 48, T_WHITE, T_BLACK);
    Atarist_DrawTextCentred(160, 64, "ATARI ST", T_ACCENT, T_ACCENT);

    for (i = 0; i < MENU_ITEMS; ++i) {
        int y = MENU_Y + i * MENU_PITCH;
        uint8_t c = (i == g_cursor) ? T_HILITE : T_WHITE;
        Atarist_DrawTextCentred(160, y, g_menu[i], c, c);
        if (i == g_cursor) {
            /* The caret blinks off the frame clock, so it keeps time even when
             * a frame runs long. */
            if ((g_anim >> 4) & 1)
                Atarist_DrawText(72, y, ">", T_HILITE, T_HILITE);
        }
    }

    Atarist_DrawTextCentred(160, 176, "CURSOR KEYS  RETURN", T_ACCENT, T_ACCENT);
    Atarist_DrawTextCentred(160, 188, "ESC QUITS TO DESKTOP", T_ACCENT, T_ACCENT);
}

void Atarist_TitleEnter(void)
{
    g_choice = ATARIST_TITLE_NONE;
    g_cursor = 0;
    g_anim = 0;
    g_redraw = 2;
    title_palettes();
    Atarist_InputFlush();
    Atarist_ClearPlanar(T_BLACK);
    Atarist_MusicLoadTrack(ATARIST_MUSIC_TITLE);
    ATARIST_STAGE(ATARIST_STAGE_TITLE);
}

void Atarist_TitleStep(int vblanks)
{
    uint16_t before = g_anim;

    g_anim = (uint16_t)(g_anim + vblanks);
    /* The caret is the only thing that changes on its own, so the screen is
     * repainted when it flips and not otherwise -- the same rule the duel
     * board follows, for the same reason. */
    if (((before >> 4) & 1) != ((g_anim >> 4) & 1)) g_redraw = 2;

    if (Atarist_InputRepeat(ATARIST_BTN_UP, 14)) {
        g_cursor = (uint8_t)((g_cursor + MENU_ITEMS - 1) % MENU_ITEMS);
        g_redraw = 2;
    }
    if (Atarist_InputRepeat(ATARIST_BTN_DOWN, 14)) {
        g_cursor = (uint8_t)((g_cursor + 1) % MENU_ITEMS);
        g_redraw = 2;
    }
    if (g_atarist_input.pressed & (ATARIST_BTN_A | ATARIST_BTN_START))
        g_choice = (uint8_t)(ATARIST_TITLE_FREE_BATTLE + g_cursor);

    if (g_redraw) {
        title_draw();
        --g_redraw;
    }
    g_atarist_probe.menu_cursor = g_cursor;
}

int Atarist_TitleChoice(void) { return g_choice; }
