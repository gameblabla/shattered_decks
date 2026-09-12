/* ─────────────────────────────────────────────────────────────────────────────
 *  snes_scene.c — the Mode 3 scenes: title, story dialogue, ending.
 *
 *  THE TITLE is the whole painting on BG1 (920 tiles: the picture plus the
 *  two lettered rows of PRESS START), blinking by swapping between its two
 *  maps.  The menu that replaces the prompt is BG2 text over a colour-math
 *  window (snesVideoTitleMenuPlate), so it owns no tiles of its own.
 *
 *  THE STORY DIALOGUE is a sky, a ground and two speakers:
 *    - the sky is CGRAM 0 rewritten every scanline by HDMA (a ramp built here
 *      from the painting's own colours, see build_sky_table);
 *    - the ground is three rows of 4bpp BG2 tiles in BG2 palette 1, under
 *      the horizon on line 120 (the void's stars cover every row instead);
 *    - the speakers are 8bpp BG1 tile blocks, 112 colours each, which slide
 *      in from the screen's edges by rewriting the BG1 map a column a field.
 *  The dialogue window is BG2 text with the priority bit, over both.
 *
 *  Neither scene initialises the duel's OBJ sheet or Mode 7 framebuffer; the
 *  duel scene does that under force blank at the scene boundary.
 * ───────────────────────────────────────────────────────────────────────────── */
#include <snes.h>

#include "snes_scene.h"
#include "snes_title.h"
#include "snes_scene_data.h"
#include "snes_obj_data.h"
#include "snes_audio.h"
#include "snes_save.h"
#include "snes_duel.h"
#include "snes_video.h"
#include "msx2_duel.h"

#define SCENE_MAP_WORD       0x7400u
#define SCENE_BG1_MAP_WORD   0x7000u
#define ENDING_FONT_WORD     0x7800u
#define ENDING_FONT_TILE     128u
#define FONT_FIRST            SNES_SPR_GLYPH_FIRST
#define FONT_COUNT            SNES_SPR_GLYPH_COUNT
#define SCENE_NONE            SNES_SCENE_COUNT
#define SCENE_TEXT_PRIORITY   0x2000u
#define SCENE_PANEL_Y         18
#define SCENE_PANEL_H         10
#define SCENE_PANEL_W         32
#define SCENE_PANEL_TEXT_X    1
#define SCENE_PANEL_TEXT_W    30

/* The story's VRAM: two speakers, a blank, the font and the ground, the maps.
 * BG1 characters at $0000, BG2's at $6000 -- the same BG12NBA the ending
 * uses, so only the map bases differ between the two. */
#define STORY_PORTRAIT_L_WORD 0x0000u
#define STORY_PORTRAIT_R_WORD (SNES_PORTRAIT_TILES * 32u)
#define STORY_BLANK_TILE      (SNES_PORTRAIT_TILES * 2u)
#define STORY_BLANK_WORD      (STORY_BLANK_TILE * 32u)
#define STORY_FONT_WORD       0x6000u
#define STORY_GROUND_TILE     (SNES_SCENE_BORDER_TILE + SNES_SCENE_BORDER_COUNT)
#define STORY_GROUND_WORD     (STORY_FONT_WORD + STORY_GROUND_TILE * 16u)
#define STORY_MAP_WORD        0x7000u
#define STORY_TEXT_MAP_WORD   0x7400u
#define STORY_PORTRAIT_ROW    1              /* lines 8..143 */
#define STORY_SLIDE_FRAMES    24
/* The sky tables: one repeat entry of SNES_STORY_SKY_LINES lines -- two
 * COLDATA bytes a line for red and green, one for blue -- then the
 * terminator, so the dialogue plate's own ramp owns the lines below. */
#define STORY_SKY_RG_BYTES    (1 + SNES_STORY_SKY_LINES * 2 + 1)
#define STORY_SKY_B_BYTES     (1 + SNES_STORY_SKY_LINES + 1)

/* The title's VRAM: the painting's 920 tiles, then the two maps and the
 * font's 64 glyphs in what is left.  BG2's character base is $7000, so the
 * glyphs at $7C00 are its tiles 192 onwards. */
#define TITLE_MAP_WORD        0x7400u
#define TITLE_TEXT_MAP_WORD   0x7800u
#define TITLE_FONT_WORD       0x7C00u
#define TITLE_FONT_TILE       192u
#define TITLE_ATTRACT         0
#define TITLE_MENU            1
#define TITLE_ROWS            3
#define TITLE_MENU_ROW        21             /* lines 168, 184, 200 */
#define TITLE_MENU_MARK_COL   8
#define TITLE_MENU_TEXT_COL   10

static u16 scene_text_map[1024];
static u8 scene_text_dirty;
static u8 story_line;
static u8 story_page;
static u8 story_reveal;
/* The current page's length, so the typewriter does not re-extract the page
 * every frame: at ~100 cycles a far byte on this compiler, that and a full
 * window redraw made a dialogue frame five vblanks long. */
static u8 page_length;
static char story_page_text[96];
static u8 story_progress;
static u8 ending_line;
static u8 ending_reveal;
static u16 scene_blank_tile;
static u8 title_phase;
static u8 title_cursor;
static u8 title_blink;
static u8 title_dirty;
static u8 story_portrait_frame;
static u16 scene_text_word;
static u8 story_kind;
static u16 story_bg1_map[1024];
static u8 story_bg1_dirty;
static u8 story_sky_rg[STORY_SKY_RG_BYTES];
static u8 story_sky_b[STORY_SKY_B_BYTES];
static u8 title_prompt_shown;
static u8 title_map_dirty;
static u8 title_plate_pending;
static u8 title_plate_on;
/* The menu's rows of the text map: only these are ever written on the
 * title, so only these go up -- a whole map beside a whole map is more than
 * one vblank carries once the NMI handler has had its share. */
#define TITLE_TEXT_FIRST_ROW  20
#define TITLE_TEXT_ROWS       8

static void story_update_portraits(void);

static const char *const title_rows[TITLE_ROWS] = {
    "STORY MODE", "RANDOM BATTLE", "LOAD SAVE (SRAM)"
};

static const char *const opponent_names[MSX2_STORY_MAX_DUELS] = {
    "KASEM", "ANPU", "RAHOTEP", "NADIRA", "ISYRA"
};

#include "snes_dialogue_data.h"
#define story_lines snes_story_lines
#define story_line_count snes_story_line_count
#define story_speaker snes_story_speaker
#define ending_lines snes_ending_lines

static u8 text_length(const char *s)
{
    u8 n = 0;
    while (*s++ && n != 255) ++n;
    return n;
}

/* The canonical prose is longer than the three-line SNES window.  Select one
 * word-wrapped page at a time; no words are discarded and the speaker remains
 * unchanged until the complete sentence has been shown. */
static u8 story_page_extract(const char *line, u8 page)
{
    u16 pos = 0;
    u8 current = 0;
    while (line[pos]) {
        u8 row, used = 0;
        for (row = 0; row < 3 && line[pos]; ++row) {
            u8 n = 0, cut, i;
            while (n < SCENE_PANEL_TEXT_W && line[pos + n]) ++n;
            cut = n;
            if (n == SCENE_PANEL_TEXT_W && line[pos + n] &&
                line[pos + n] != ' ') {
                while (cut && line[pos + cut] != ' ') --cut;
                if (!cut) cut = n;
            }
            if (current == page) {
                for (i = 0; i < cut; ++i)
                    story_page_text[used + i] = line[pos + i];
            }
            pos = (u16)(pos + cut);
            while (line[pos] == ' ') ++pos;
            used = (u8)(used + cut);
            if (line[pos] && row < 2) {
                while (used < (u8)((row + 1) * SCENE_PANEL_TEXT_W)) {
                    if (current == page) story_page_text[used] = ' ';
                    ++used;
                }
            }
        }
        if (current == page) {
            story_page_text[used] = 0;
            return used;
        }
        ++current;
    }
    story_page_text[0] = 0;
    return 0;
}

static void clear_text_rows(u8 first, u8 count)
{
    u16 i;
    for (i = (u16)first * 32; i < (u16)(first + count) * 32; ++i)
        scene_text_map[i] = scene_blank_tile;
}

static void draw_text_line(u8 row, u8 first_col, u8 width, const char *s,
                           u8 reveal, u8 tile_base)
{
    u8 col = 0;
    while (*s && col < width) {
        const u8 c = (u8)*s++;
        if (col < reveal && c >= FONT_FIRST &&
            c < FONT_FIRST + FONT_COUNT && c != ' ')
            scene_text_map[(u16)row * 32 + first_col + col] =
                (u16)(SCENE_TEXT_PRIORITY + tile_base + c - FONT_FIRST);
        ++col;
    }
}

static void title_clear_map(void)
{
    u16 i;
    for (i = 0; i < 1024; ++i) scene_text_map[i] = 0;
}

static void title_draw_center(u8 row, const char *s)
{
    u8 n = text_length(s);
    draw_text_line(row, (u8)((32 - n) >> 1), n, s, 255,
                   TITLE_FONT_TILE);
}

static void title_draw_menu(void)
{
    u8 i;
    title_clear_map();
    for (i = 0; i < TITLE_ROWS; ++i) {
        char marker[2];
        marker[0] = (i == title_cursor) ? '>' : ' ';
        marker[1] = 0;
        draw_text_line((u8)(TITLE_MENU_ROW + i * 2), TITLE_MENU_MARK_COL, 1,
                       marker, 255, TITLE_FONT_TILE);
        draw_text_line((u8)(TITLE_MENU_ROW + i * 2), TITLE_MENU_TEXT_COL, 16,
                       title_rows[i], 255, TITLE_FONT_TILE);
    }
    title_dirty = 1;
    if (title_prompt_shown) title_map_dirty = 1;
    title_prompt_shown = 0;
    title_plate_on = 1;
    title_plate_pending = 1;
}

/* The attract screen is the painting alone; PRESS START is a second map
 * whose two rows point at lettered tiles, swapped in and out at vblank. */
static void title_draw_attract(void)
{
    title_clear_map();
    title_dirty = 1;
    if (title_prompt_shown != title_blink) title_map_dirty = 1;
    title_prompt_shown = title_blink;
    title_plate_on = 0;
    title_plate_pending = 1;
}

static void draw_dialog_panel(u8 tile_base)
{
    u8 row, col;
    u16 tile;

    for (row = 0; row < SCENE_PANEL_H; ++row) {
        for (col = 0; col < SCENE_PANEL_W; ++col) {
            tile = 0xFFFFu;
            if (row == 0) {
                tile = (col == 0) ? 0 : (col == SCENE_PANEL_W - 1) ? 2 : 1;
            } else if (row == SCENE_PANEL_H - 1) {
                tile = (col == 0) ? 5 : (col == SCENE_PANEL_W - 1) ? 7 : 6;
            } else if (col == 0) {
                tile = 3;
            } else if (col == SCENE_PANEL_W - 1) {
                tile = 4;
            }
            if (tile != 0xFFFFu)
                scene_text_map[(u16)(SCENE_PANEL_Y + row) * 32 + col] =
                    (u16)(SCENE_TEXT_PRIORITY + tile_base +
                          SNES_SCENE_BORDER_TILE + tile);
        }
    }
}

static void draw_dialogue(const char *speaker, const char *line,
                          u8 reveal, u8 tile_base)
{
    u8 offset = 0;
    u8 row;
    const u8 length = text_length(line);
    clear_text_rows(18, 14);
    draw_dialog_panel(tile_base);
    draw_text_line(SCENE_PANEL_Y + 1, SCENE_PANEL_TEXT_X,
                   SCENE_PANEL_TEXT_W, speaker, 255, tile_base);
    for (row = 0; row < 3 && offset < length; ++row) {
        draw_text_line((u8)(SCENE_PANEL_Y + 3 + row), SCENE_PANEL_TEXT_X,
                       SCENE_PANEL_TEXT_W, line + offset,
                       reveal > offset ? (u8)(reveal - offset) : 0,
                       tile_base);
        offset = (u8)(offset + SCENE_PANEL_TEXT_W);
    }
    if (reveal >= length)
        draw_text_line(SCENE_PANEL_Y + 8, SCENE_PANEL_TEXT_X,
                       SCENE_PANEL_TEXT_W, "A: NEXT", 255, tile_base);
    scene_text_dirty = 1;
}

/* One more character of the page: the typewriter's per-frame work is one
 * cell of the map, plus the prompt once the page is complete. */
static void dialogue_reveal(u8 reveal, u8 tile_base)
{
    if (reveal && reveal <= page_length) {
        const u8 i = (u8)(reveal - 1);
        const u8 c = (u8)story_page_text[i];
        if (c >= FONT_FIRST && c < FONT_FIRST + FONT_COUNT && c != ' ')
            scene_text_map[(u16)(SCENE_PANEL_Y + 3 + i / SCENE_PANEL_TEXT_W) * 32
                           + SCENE_PANEL_TEXT_X + i % SCENE_PANEL_TEXT_W] =
                (u16)(SCENE_TEXT_PRIORITY + tile_base + c - FONT_FIRST);
    }
    if (reveal >= page_length)
        draw_text_line(SCENE_PANEL_Y + 8, SCENE_PANEL_TEXT_X,
                       SCENE_PANEL_TEXT_W, "A: NEXT", 255, tile_base);
    scene_text_dirty = 1;
}

static void scene_upload(const u8 *tiles, u16 tile_bytes, const u8 *pal,
                         const u8 *map, u16 font_word, u8 bg12nba,
                         u16 bg1_map_word, u16 text_map_word)
{
    setScreenOff();
    REG_HDMAEN = 0;
    REG_CGWSEL = 0;
    REG_CGADSUB = 0;
    REG_TMW = 0;
    REG_W12SEL = 0;

    dmaCopyCGram((u8 *)pal, 0, SNES_SCENE_PAL_BYTES);
    dmaCopyVram((u8 *)tiles, 0, tile_bytes);
    dmaCopyVram((u8 *)map, bg1_map_word, SNES_SCENE_MAP_BYTES);
    dmaCopyVram((u8 *)snes_scene_font, font_word, SNES_SCENE_FONT_BYTES);
    dmaCopyVram((u8 *)scene_text_map, text_map_word,
                SNES_SCENE_MAP_BYTES);

    REG_BG1SC = (u8)(bg1_map_word >> 8);
    REG_BG2SC = (u8)(text_map_word >> 8);
    REG_BG12NBA = bg12nba;
    REG_BG1HOFS = 0;
    REG_BG1HOFS = 0;
    REG_BG1VOFS = 0;
    REG_BG1VOFS = 0;
    REG_BG2HOFS = 0;
    REG_BG2HOFS = 0;
    REG_BG2VOFS = 0;
    REG_BG2VOFS = 0;
    setMode(BG_MODE3, 0);
    REG_TM = BG1_ENABLE | BG2_ENABLE;
    REG_TS = 0;
    scene_text_dirty = 0;
    scene_text_word = text_map_word;
}

static void title_upload(void)
{
    setScreenOff();

    /* Nothing from the duel's HDMA or sprite layer may remain active while
     * BG1 is being repainted. */
    REG_HDMAEN = 0;
    REG_CGWSEL = 0;
    REG_CGADSUB = 0;
    REG_TMW = 0;
    REG_W12SEL = 0;

    dmaCopyCGram((u8 *)snes_title_pal, 0, SNES_TITLE_PAL_BYTES);
    dmaCopyVram((u8 *)snes_title_tiles, 0, SNES_TITLE_TILE_BYTES);
    dmaCopyVram((u8 *)(title_prompt_shown ? snes_title_prompt_map
                                          : snes_title_map),
                TITLE_MAP_WORD, SNES_TITLE_MAP_BYTES);
    dmaCopyVram((u8 *)snes_scene_font, TITLE_FONT_WORD,
                SNES_SCENE_GLYPH_BYTES);
    dmaCopyVram((u8 *)scene_text_map, TITLE_TEXT_MAP_WORD,
                SNES_SCENE_MAP_BYTES);

    /* BG1 is 8bpp in Mode 3.  Its character data starts at VRAM $0000 and
     * fills it to $7300; the maps and the glyphs take the rest. */
    REG_BG1SC = (u8)(TITLE_MAP_WORD >> 8);
    REG_BG2SC = (u8)(TITLE_TEXT_MAP_WORD >> 8);
    REG_BG12NBA = 0x70;
    REG_BG1HOFS = 0;
    REG_BG1HOFS = 0;
    REG_BG1VOFS = 0;
    REG_BG1VOFS = 0;
    REG_BG2HOFS = 0;
    REG_BG2HOFS = 0;
    REG_BG2VOFS = 0;
    REG_BG2VOFS = 0;
    setMode(BG_MODE3, 0);
    REG_TM = BG1_ENABLE | BG2_ENABLE;
    REG_TS = 0;
    snesVideoTitleMenuPlate(0);
    title_plate_pending = 0;
    title_map_dirty = 0;
    setScreenOn();
}

void snesTitleInit(void)
{
    /* This runtime does not clear uninitialized RAM.  The shared scene-text
     * uploader must be idle even on first boot, or its stale dirty flag and
     * destination can overwrite title tiles on the first vblank. */
    scene_text_dirty = 0;
    scene_text_word = TITLE_TEXT_MAP_WORD;
    title_phase = TITLE_ATTRACT;
    title_cursor = 0;
    title_blink = 1;
    title_dirty = 0;
    title_draw_attract();
    title_upload();
    title_dirty = 0;
    title_plate_pending = 0;
}

void snesStoryBegin(u8 progress)
{
    story_progress = (progress < MSX2_STORY_MAX_DUELS) ? progress : 0;
}

u8 snesTitleFrame(void)
{
    const u16 down = padsDown(0);
    if (title_phase == TITLE_ATTRACT) {
        u8 blink = (u8)(((snesSceneFrames() / 24) & 1) == 0);
        if (blink != title_blink) {
            title_blink = blink;
            title_draw_attract();
        }
        if (down & (KEY_START | KEY_A)) {
            title_phase = TITLE_MENU;
            title_cursor = 0;
            snesAudioSfx(SNES_SFX_CONFIRM_ALT);
            title_draw_menu();
        }
        /* Non-menu verification/service shortcuts retained from the port's
         * bring-up harness.  They do not appear in or alter the three-option
         * player-facing menu. */
        if (down & KEY_Y) return SNES_SCENE_DECK;
        if (down & KEY_B) {
            snesStoryBegin(0);
            return SNES_SCENE_STORY_TALK;
        }
        if (down & KEY_X) return SNES_SCENE_ENDING;
        return SCENE_NONE;
    }

    if (down & KEY_UP) {
        title_cursor = (u8)((title_cursor + TITLE_ROWS - 1) % TITLE_ROWS);
        snesAudioSfx(SNES_SFX_SELECT);
        title_draw_menu();
    } else if (down & KEY_DOWN) {
        title_cursor = (u8)((title_cursor + 1) % TITLE_ROWS);
        snesAudioSfx(SNES_SFX_SELECT);
        title_draw_menu();
    }
    if (down & KEY_B) {
        title_phase = TITLE_ATTRACT;
        title_blink = 1;
        title_draw_attract();
    } else if (down & (KEY_A | KEY_START)) {
        if (title_cursor == 0) {
            snesStoryBegin(0);
            return SNES_SCENE_STORY_TALK;
        }
        if (title_cursor == 1) {
            snesDuelConfigure(MSX2_STORY_NONE, 0);
            return SNES_SCENE_DUEL;
        }
        if (snesSaveIsValid()) {
            /* A completed run is a real checkpoint too: progress 5 must
             * resume on the ending instead of wrapping to the first duel. */
            if (snesSaveStoryProgress() >= MSX2_STORY_MAX_DUELS)
                return SNES_SCENE_ENDING;
            snesStoryBegin(snesSaveStoryProgress());
            return SNES_SCENE_STORY_TALK;
        }
        title_draw_menu();
        draw_text_line(27, 7, 18, "NO SRAM SAVE FOUND", 255,
                       TITLE_FONT_TILE);
        title_dirty = 1;
        snesAudioSfx(SNES_SFX_CONFIRM_ALT);
    }
    return SCENE_NONE;
}

/* Which painting: src/main.c's story_scene_kind() by the frontier. */
static u8 story_kind_of(u8 progress)
{
    if (progress >= 4) return 3;          /* sky: the void */
    if (progress >= 3) return 2;          /* ember: the volcano */
    if (progress >= 2) return 1;          /* stone: the temple */
    return 0;                             /* desert */
}

#define STORY_BG_SWITCH(kind, EXPR) \
    switch (kind) { \
    case 1:  EXPR(stone); break; \
    case 2:  EXPR(ember); break; \
    case 3:  EXPR(sky); break; \
    default: EXPR(desert); break; }

/* The far ROM symbols must stay in the DMA expressions -- see the portrait
 * switch below -- so every per-kind upload is a macro over the kind. */
#define STORY_UPLOAD_GROUND(k) \
    do { \
        dmaCopyVram((u8 *)snes_story_##k##_tiles, STORY_GROUND_WORD, \
                    SNES_STORY_##k##_TILE_BYTES_); \
        dmaCopyCGram((u8 *)snes_story_##k##_pal, SNES_STORY_GROUND_PAL, 32); \
    } while (0)
#define SNES_STORY_desert_TILE_BYTES_ SNES_STORY_DESERT_TILE_BYTES
#define SNES_STORY_stone_TILE_BYTES_  SNES_STORY_STONE_TILE_BYTES
#define SNES_STORY_ember_TILE_BYTES_  SNES_STORY_EMBER_TILE_BYTES
#define SNES_STORY_sky_TILE_BYTES_    SNES_STORY_SKY_TILE_BYTES
#define STORY_GROUND_MAP(k) ground = snes_story_##k##_map
#define STORY_SKY(k)        sky = snes_story_##k##_sky

/* The sky: the fixed colour per line, from the painting's ramp.  The asset
 * holds BGR555 words; COLDATA takes five bits a component behind a select
 * bit ($20 red, $40 green, $80 blue). */
static void build_sky_table(void)
{
    const u8 *sky = 0;
    u8 *rg = story_sky_rg;
    u8 *b = story_sky_b;
    u8 i;
    STORY_BG_SWITCH(story_kind, STORY_SKY);
    *rg++ = (u8)(0x80 | SNES_STORY_SKY_LINES);
    *b++ = (u8)(0x80 | SNES_STORY_SKY_LINES);
    for (i = 0; i < SNES_STORY_SKY_LINES; ++i) {
        const u16 word = (u16)(sky[(u16)i * 2] | ((u16)sky[(u16)i * 2 + 1] << 8));
        *rg++ = (u8)(0x20 | (word & 31));
        *rg++ = (u8)(0x40 | ((word >> 5) & 31));
        *b++ = (u8)(0x80 | ((word >> 10) & 31));
    }
    *rg = 0;
    *b = 0;
}

/* The ground map into BG2's rows 0..17.  The asset's cells carry BG2
 * palette 1 already; its tile indices count from the first ground tile. */
static void story_ground_map(void)
{
    const u8 *ground = 0;
    u16 i;
    STORY_BG_SWITCH(story_kind, STORY_GROUND_MAP);
    for (i = 0; i < (u16)SNES_STORY_SCENE_ROWS * 32; ++i) {
        const u8 tile = ground[i * 2];
        scene_text_map[i] = (u16)(((u16)ground[i * 2 + 1] << 8) |
                                  (tile ? (u8)(tile + STORY_GROUND_TILE) : 0));
    }
    scene_text_dirty = 1;
}

/* Both speakers on the BG1 map, `cols` columns of each in from the edge.
 * Serena stands on the left and walks in from x = -128; the opponent walks
 * in from x = 256 on the right.  Everything else is the blank tile. */
static void story_place_portraits(u8 cols)
{
    u8 row, col;
    for (row = 0; row < SNES_PORTRAIT_ROWS; ++row) {
        const u16 map_row = (u16)(STORY_PORTRAIT_ROW + row) * 32;
        /* Only the rows the figures stand on are rewritten; the rest of the
         * map was blanked once when the scene was set up. */
        for (col = 0; col < 32; ++col) story_bg1_map[map_row + col] = STORY_BLANK_TILE;
        for (col = 0; col < cols && col < SNES_PORTRAIT_COLS; ++col) {
            /* The leftmost visible column of Serena is her column
             * (16 - cols); the opponent's rightmost visible column is his
             * (cols - 1). */
            const u8 lcol = (u8)(SNES_PORTRAIT_COLS - cols + col);
            story_bg1_map[map_row + col] =
                (u16)(row * SNES_PORTRAIT_COLS + lcol);
            story_bg1_map[map_row + 31 - col] =
                (u16)(SNES_PORTRAIT_TILES + row * SNES_PORTRAIT_COLS +
                      (cols - 1 - col));
        }
    }
    story_bg1_dirty = 1;
}

static void story_update_portraits(void)
{
    u8 frame = story_portrait_frame;
    if (frame > STORY_SLIDE_FRAMES) frame = STORY_SLIDE_FRAMES;
    story_place_portraits((u8)(((u16)frame * SNES_PORTRAIT_COLS) /
                               STORY_SLIDE_FRAMES));
}

#define STORY_UPLOAD_PORTRAIT(n, word, cg) \
    do { \
        dmaCopyVram((u8 *)snes_portrait_##n, word, SNES_PORTRAIT_BYTES); \
        dmaCopyCGram((u8 *)snes_portrait_##n##_pal, cg, \
                     SNES_PORTRAIT_PAL_BYTES); \
    } while (0)

void snesStoryInit(void)
{
    static const u8 blank[64] = { 0 };
    scene_blank_tile = 0;
    story_kind = story_kind_of(story_progress);
    story_line = 0;
    story_page = 0;
    story_reveal = 0;
    story_portrait_frame = 0;
    page_length = 0;

    setScreenOff();
    REG_HDMAEN = 0;
    REG_CGWSEL = 0;
    REG_CGADSUB = 0;
    REG_TMW = 0;
    REG_W12SEL = 0;
    REG_WOBJSEL = 0;

    /* CGRAM: the text palette, the ground's, then the two speakers'. */
    dmaCopyCGram((u8 *)snes_scene_text_pal, 0, 32);
    STORY_BG_SWITCH(story_kind, STORY_UPLOAD_GROUND);
    /* Keep the far ROM symbol in the DMA expression.  All six generated
     * sections start at offset zero in different banks; storing one in a
     * temporary pointer lets this 816 compiler retain the offset but lose the
     * bank, which made the opponent redraw Serena through palette 1. */
    STORY_UPLOAD_PORTRAIT(0, STORY_PORTRAIT_L_WORD, SNES_PORTRAIT_FIRST_L);
    switch (story_progress) {
    default:
    case 0: STORY_UPLOAD_PORTRAIT(1, STORY_PORTRAIT_R_WORD, SNES_PORTRAIT_FIRST_R); break;
    case 1: STORY_UPLOAD_PORTRAIT(2, STORY_PORTRAIT_R_WORD, SNES_PORTRAIT_FIRST_R); break;
    case 2: STORY_UPLOAD_PORTRAIT(3, STORY_PORTRAIT_R_WORD, SNES_PORTRAIT_FIRST_R); break;
    case 3: STORY_UPLOAD_PORTRAIT(4, STORY_PORTRAIT_R_WORD, SNES_PORTRAIT_FIRST_R); break;
    case 4: STORY_UPLOAD_PORTRAIT(5, STORY_PORTRAIT_R_WORD, SNES_PORTRAIT_FIRST_R); break;
    }
    dmaCopyVram((u8 *)blank, STORY_BLANK_WORD, 64);
    dmaCopyVram((u8 *)snes_scene_font, STORY_FONT_WORD, SNES_SCENE_FONT_BYTES);

    clear_text_rows(0, 32);
    story_ground_map();
    {
        u16 i;
        for (i = 0; i < 1024; ++i) story_bg1_map[i] = STORY_BLANK_TILE;
    }
    page_length = story_page_extract(story_lines[story_progress][story_line],
                                     story_page);
    draw_dialogue(story_speaker[story_progress][story_line]
                  ? opponent_names[story_progress] : "SERENA",
                  story_page_text, story_reveal, 0);
    story_update_portraits();
    dmaCopyVram((u8 *)story_bg1_map, STORY_MAP_WORD, SNES_SCENE_MAP_BYTES);
    dmaCopyVram((u8 *)scene_text_map, STORY_TEXT_MAP_WORD, SNES_SCENE_MAP_BYTES);
    story_bg1_dirty = 0;
    scene_text_dirty = 0;
    scene_text_word = STORY_TEXT_MAP_WORD;

    REG_BG1SC = (u8)(STORY_MAP_WORD >> 8);
    REG_BG2SC = (u8)(STORY_TEXT_MAP_WORD >> 8);
    REG_BG12NBA = 0x60;
    REG_BG1HOFS = 0; REG_BG1HOFS = 0;
    REG_BG1VOFS = 0; REG_BG1VOFS = 0;
    REG_BG2HOFS = 0; REG_BG2HOFS = 0;
    REG_BG2VOFS = 0; REG_BG2VOFS = 0;
    setMode(BG_MODE3, 0);
    REG_TM = BG1_ENABLE | BG2_ENABLE;
    REG_TS = 0;
    oamClear(0, 0);

    build_sky_table();
    {
        /* The tables' banks, read out of the pointers' own bytes: a pointer
         * here is four bytes with the bank third, and shifting a u32 by
         * sixteen is not something this compiler gets right. */
        const u8 *rg = story_sky_rg;
        const u8 *b = story_sky_b;
        snesVideoSetSkyTables((u16)rg, ((const u8 *)&rg)[2],
                              (u16)b, ((const u8 *)&b)[2]);
    }
}

u8 snesStoryFrame(void)
{
    const u16 down = padsDown(0);
    const char *line = story_lines[story_progress][story_line];

    if (story_portrait_frame <= STORY_SLIDE_FRAMES) {
        ++story_portrait_frame;
        story_update_portraits();
    }

    if (story_reveal < page_length) {
        if (down & (KEY_A | KEY_START)) {
            story_reveal = page_length;
            draw_dialogue(story_speaker[story_progress][story_line]
                          ? opponent_names[story_progress] : "SERENA",
                          story_page_text, story_reveal, 0);
        } else if ((snesSceneFrames() & 1) == 0) {
            ++story_reveal;
            dialogue_reveal(story_reveal, 0);
        }
    } else if (down & (KEY_A | KEY_START)) {
        snesAudioSfx(SNES_SFX_CONFIRM_ALT);
        if (story_page_extract(line, (u8)(story_page + 1)) != 0) {
            ++story_page;
        } else {
            ++story_line;
            story_page = 0;
        }
        if (story_line >= story_line_count[story_progress]) {
            snesDuelConfigure(story_progress, 1);
            return SNES_SCENE_DUEL;
        }
        story_reveal = 0;
        page_length = story_page_extract(story_lines[story_progress][story_line],
                                         story_page);
        draw_dialogue(story_speaker[story_progress][story_line]
                      ? opponent_names[story_progress] : "SERENA",
                      story_page_text, story_reveal, 0);
    }
    return SCENE_NONE;
}

void snesEndingInit(void)
{
    oamClear(0, 0);
    snesVideoSetSkyTables(0, 0, 0, 0);
    scene_blank_tile = ENDING_FONT_TILE;
    clear_text_rows(0, 32);
    ending_line = 0;
    ending_reveal = 0;
    page_length = story_page_extract(ending_lines[ending_line], 0);
    draw_dialogue("SERENA", story_page_text, ending_reveal,
                  ENDING_FONT_TILE);
    scene_upload(snes_ending_tiles, SNES_ENDING_TILE_BYTES, snes_ending_pal,
                 snes_ending_map, ENDING_FONT_WORD, 0x70,
                 SCENE_BG1_MAP_WORD, SCENE_MAP_WORD);
}

u8 snesEndingFrame(void)
{
    const u16 down = padsDown(0);

    if (ending_reveal < page_length) {
        if (down & (KEY_A | KEY_START)) {
            ending_reveal = page_length;
            draw_dialogue("SERENA", story_page_text, ending_reveal,
                          ENDING_FONT_TILE);
        } else if ((snesSceneFrames() & 1) == 0) {
            ++ending_reveal;
            dialogue_reveal(ending_reveal, ENDING_FONT_TILE);
        }
    } else if (down & (KEY_A | KEY_START)) {
        snesAudioSfx(SNES_SFX_CONFIRM_ALT);
        ++ending_line;
        if (ending_line >= sizeof(ending_lines) / sizeof(ending_lines[0]))
            return SNES_SCENE_TITLE;
        ending_reveal = 0;
        page_length = story_page_extract(ending_lines[ending_line], 0);
        draw_dialogue("SERENA", story_page_text, ending_reveal,
                      ENDING_FONT_TILE);
    }
    return SCENE_NONE;
}

void snesSceneVblank(void)
{
    if (snesSceneCurrent() == SNES_SCENE_TITLE) {
        if (title_plate_pending) {
            snesVideoTitleMenuPlate(title_plate_on);
            title_plate_pending = 0;
        }
        if (title_map_dirty) {
            dmaCopyVram((u8 *)(title_prompt_shown ? snes_title_prompt_map
                                                  : snes_title_map),
                        TITLE_MAP_WORD, SNES_TITLE_MAP_BYTES);
            title_map_dirty = 0;
        }
        if (title_dirty) {
            dmaCopyVram((u8 *)&scene_text_map[TITLE_TEXT_FIRST_ROW * 32],
                        (u16)(TITLE_TEXT_MAP_WORD + TITLE_TEXT_FIRST_ROW * 32),
                        TITLE_TEXT_ROWS * 64);
            title_dirty = 0;
        }
        return;
    }
    if (story_bg1_dirty) {
        dmaCopyVram((u8 *)story_bg1_map, STORY_MAP_WORD, SNES_SCENE_MAP_BYTES);
        story_bg1_dirty = 0;
    }
    if (!scene_text_dirty) return;
    dmaCopyVram((u8 *)scene_text_map, scene_text_word,
                SNES_SCENE_MAP_BYTES);
    scene_text_dirty = 0;
}
