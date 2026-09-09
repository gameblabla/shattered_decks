/* ─────────────────────────────────────────────────────────────────────────────
 *  snes_scene.c — the first real-art Mode 3 scene.
 *
 *  The title deliberately owns only BG1.  It does not initialise the duel's
 *  OBJ sheet or Mode 7 framebuffer; the duel scene will do that under force
 *  blank at the scene boundary.  That keeps the title's 896 tiles in bank C9
 *  and leaves the existing board/HUD memory layout untouched.
 * ───────────────────────────────────────────────────────────────────────────── */
#include <snes.h>

#include "snes_scene.h"
#include "snes_title.h"
#include "snes_scene_data.h"
#include "snes_obj_data.h"
#include "snes_audio.h"

#define SCENE_MAP_WORD       0x7400u
#define SCENE_BG1_MAP_WORD   0x7000u
#define STORY_FONT_WORD      0x5000u
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

static u16 scene_text_map[1024];
static u8 scene_text_dirty;
static u8 story_line;
static u8 story_reveal;
static u8 ending_line;
static u8 ending_reveal;
static u16 scene_blank_tile;

static const char *const story_lines[] = {
    "THE DESERT REMEMBERS.",
    "SERENA, THE FIRST SEAL WAITS.",
    "DRAW A CARD. FACE THE GUARDIAN."
};

static const char *const ending_lines[] = {
    "THE LAST CARD TURNS.",
    "THE SANDS KEEP HER NAME.",
    "PRESS A TO RETURN"
};

static u8 text_length(const char *s)
{
    u8 n = 0;
    while (*s++ && n != 255) ++n;
    return n;
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
    clear_text_rows(18, 14);
    draw_dialog_panel(tile_base);
    draw_text_line(SCENE_PANEL_Y + 1, SCENE_PANEL_TEXT_X,
                   SCENE_PANEL_TEXT_W, speaker, 255, tile_base);
    draw_text_line(SCENE_PANEL_Y + 3, SCENE_PANEL_TEXT_X,
                   SCENE_PANEL_TEXT_W, line, reveal, tile_base);
    if (reveal >= text_length(line))
        draw_text_line(SCENE_PANEL_Y + 8, SCENE_PANEL_TEXT_X,
                       SCENE_PANEL_TEXT_W, "A: NEXT", 255, tile_base);
    scene_text_dirty = 1;
}

static void scene_upload(const u8 *tiles, u16 tile_bytes, const u8 *pal,
                         const u8 *map, u16 font_word, u8 bg12nba)
{
    setScreenOff();
    REG_HDMAEN = 0;
    REG_CGWSEL = 0;
    REG_CGADSUB = 0;
    REG_TMW = 0;
    REG_W12SEL = 0;

    dmaCopyCGram((u8 *)pal, 0, SNES_SCENE_PAL_BYTES);
    dmaCopyVram((u8 *)tiles, 0, tile_bytes);
    dmaCopyVram((u8 *)map, SCENE_BG1_MAP_WORD, SNES_SCENE_MAP_BYTES);
    dmaCopyVram((u8 *)snes_scene_font, font_word, SNES_SCENE_FONT_BYTES);
    dmaCopyVram((u8 *)scene_text_map, SCENE_MAP_WORD,
                SNES_SCENE_MAP_BYTES);

    REG_BG1SC = 0x70;
    REG_BG2SC = 0x74;
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
    dmaCopyVram((u8 *)snes_title_map, 0x7000, SNES_TITLE_MAP_BYTES);

    /* BG1 is 8bpp in Mode 3.  Its character data starts at VRAM $0000 and
     * its 32x32 map at word address $7000, matching the title asset bank. */
    REG_BG1SC = 0x70;
    REG_BG12NBA = 0x00;
    REG_BG1HOFS = 0;
    REG_BG1HOFS = 0;
    REG_BG1VOFS = 0;
    REG_BG1VOFS = 0;
    setMode(BG_MODE3, 0);
    REG_TM = BG1_ENABLE;
    REG_TS = 0;
    setScreenOn();
}

void snesTitleInit(void)
{
    title_upload();
}

u8 snesTitleFrame(void)
{
    const u16 down = padsDown(0);
    /* B is the compact story entry while the menu scene is still being
     * filled out.  X opens the ending gallery, which makes the real ending
     * art reachable without a sound or SRAM dependency. */
    if (down & KEY_B) return SNES_SCENE_STORY_TALK;
    if (down & KEY_X) return SNES_SCENE_ENDING;
    if (down & KEY_Y) return SNES_SCENE_DECK;
    /* A/START is the playable path; the timeout keeps unattended regression
     * captures moving into the duel. */
    if ((down & (KEY_A | KEY_START)) || snesSceneFrames() >= 30)
        return SNES_SCENE_DUEL;
    return SCENE_NONE;
}

void snesStoryInit(void)
{
    scene_blank_tile = 0;
    clear_text_rows(0, 32);
    story_line = 0;
    story_reveal = 0;
    draw_dialogue("SERENA", story_lines[story_line], story_reveal, 0);
    scene_upload(snes_story_tiles, SNES_STORY_TILE_BYTES, snes_story_pal,
                 snes_story_map, STORY_FONT_WORD, 0x50);
}

u8 snesStoryFrame(void)
{
    const u16 down = padsDown(0);
    const u8 length = text_length(story_lines[story_line]);

    if (story_reveal < length) {
        if (down & (KEY_A | KEY_START)) story_reveal = length;
        else if ((snesSceneFrames() & 1) == 0) ++story_reveal;
        draw_dialogue("SERENA", story_lines[story_line], story_reveal, 0);
    } else if (down & (KEY_A | KEY_START)) {
        snesAudioSfx(SNES_SFX_CONFIRM_ALT);
        ++story_line;
        if (story_line >= sizeof(story_lines) / sizeof(story_lines[0]))
            return SNES_SCENE_DUEL;
        story_reveal = 0;
        draw_dialogue("SERENA", story_lines[story_line], story_reveal, 0);
    }
    return SCENE_NONE;
}

void snesEndingInit(void)
{
    scene_blank_tile = ENDING_FONT_TILE;
    clear_text_rows(0, 32);
    ending_line = 0;
    ending_reveal = 0;
    draw_dialogue("SERENA", ending_lines[ending_line], ending_reveal,
                  ENDING_FONT_TILE);
    scene_upload(snes_ending_tiles, SNES_ENDING_TILE_BYTES, snes_ending_pal,
                 snes_ending_map, ENDING_FONT_WORD, 0x70);
}

u8 snesEndingFrame(void)
{
    const u16 down = padsDown(0);
    const u8 length = text_length(ending_lines[ending_line]);

    if (ending_reveal < length) {
        if (down & (KEY_A | KEY_START)) ending_reveal = length;
        else if ((snesSceneFrames() & 1) == 0) ++ending_reveal;
        draw_dialogue("SERENA", ending_lines[ending_line], ending_reveal,
                      ENDING_FONT_TILE);
    } else if (down & (KEY_A | KEY_START)) {
        snesAudioSfx(SNES_SFX_CONFIRM_ALT);
        ++ending_line;
        if (ending_line >= sizeof(ending_lines) / sizeof(ending_lines[0]))
            return SNES_SCENE_TITLE;
        ending_reveal = 0;
        draw_dialogue("SERENA", ending_lines[ending_line], ending_reveal,
                      ENDING_FONT_TILE);
    }
    return SCENE_NONE;
}

void snesSceneVblank(void)
{
    if (!scene_text_dirty) return;
    dmaCopyVram((u8 *)scene_text_map, SCENE_MAP_WORD,
                SNES_SCENE_MAP_BYTES);
    scene_text_dirty = 0;
}
