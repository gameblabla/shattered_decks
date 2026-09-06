#include <stddef.h>

#include "atarist_assets.h"
#include "atarist_video.h"
#include "atarist_os.h"

#include "msxgl.h"
#include "msx2_cards.h"

#define ARENA_TEX_W (1 << ATARIST_ARENA_TEX_LOG2)
#define CARD_TEX_W  (1 << ATARIST_CARD_TEX_LOG2)

/* 8-bit RGB, packed to the running machine's palette word at boot so one asset
 * set serves both the 3-bit ST and the 4-bit STE. */
static const uint8_t g_arena_rgb[16][3] = {
    {  0,   0,   0},   /* black */
    { 32,  40,  80},   /* sky dark */
    { 72,  96, 152},   /* sky mid */
    {136, 168, 208},   /* sky light */
    { 96,  72,  40},   /* sand dark */
    {160, 128,  80},   /* sand mid */
    {216, 192, 136},   /* sand light */
    { 64,  48,  32},   /* grid line */
    {224, 176,  56},   /* slot outline */
    {152,  40,  40},   /* card back */
    {232, 216, 184},   /* card face */
    { 80, 112, 216},   /* attribute A (water/wind) */
    { 72, 168,  88},   /* attribute B (earth/dark) */
    {184,  80, 200},   /* attribute C (fire/light) */
    {248, 248, 248},   /* white */
    {248, 232,  96}    /* highlight */
};

static const uint8_t g_card_rgb[16][3] = {
    {  0,   0,   0},
    { 24,  24,  40},
    { 56,  56,  88},
    { 96,  96, 136},
    {224, 176,  56},
    {208,  64,  64},
    { 72, 176,  88},
    { 72, 112, 216},
    {160,  72, 200},
    { 88, 200, 208},
    {232, 144,  56},
    {128,  88,  48},
    {112, 112, 112},
    {176, 176, 176},
    {248, 248, 248},
    {248, 232,  96}
};

static uint16_t g_arena_pal[16];
static uint16_t g_card_pal[16];

static uint8_t *g_arena_tex;
static uint8_t *g_card_tex;            /* ATARIST_CARD_FACES faces, back to back */
static AtaristTexture g_arena;
static AtaristTexture g_faces[ATARIST_CARD_FACES];

/* Deterministic value noise, so the sand has grain without a texture file.
 * The real sandstone conversion (tools/atarist/gen_atarist_assets.py) replaces
 * the fill, not the layout: the grid and slot markings below are the part the
 * board's geometry depends on. */
static uint16_t noise(uint16_t x, uint16_t y)
{
    uint16_t n = (uint16_t)(x * 374u + y * 1597u);
    n = (uint16_t)((n << 13) ^ n);
    return (uint16_t)((n * (n * n * 15731u + 789221u) + 1376312589u) >> 8);
}

/* ── Arena texture ───────────────────────────────────────────────────────── */

/* One world unit is ATARIST_TEXELS_PER_UNIT texels, so the 128x128 texture
 * tiles every 16 world units.  The board is 5 columns by 4 rows inside that, and the grid lines are
 * baked at the slot boundaries: the ground rasteriser then draws the board's
 * markings for free, with no second pass and no per-slot geometry. */
#define TEXELS_PER_UNIT ATARIST_TEXELS_PER_UNIT

static void build_arena_texture(void)
{
    int x, y;
    for (y = 0; y < ARENA_TEX_W; ++y) {
        for (x = 0; x < ARENA_TEX_W; ++x) {
            uint8_t c;
            /* THE BOARD IS CENTRED ON TEXEL (0,0), NOT ON THE TEXTURE'S
             * MIDDLE.  Atarist_DrawGround samples u = world_x * texels and
             * v = world_z * texels with no origin offset, so world (0,0) is
             * texel (0,0); building the markings around the texture's centre
             * instead put the whole board eight world units away, out by the
             * horizon, and left the playfield blank sand. */
            int cx = (x < ARENA_TEX_W / 2) ? x : x - ARENA_TEX_W;
            int cy = (y < ARENA_TEX_W / 2) ? y : y - ARENA_TEX_W;
            int in_board = (cx >= -TEXELS_PER_UNIT * 5 / 2) &&
                           (cx <   TEXELS_PER_UNIT * 5 / 2) &&
                           (cy >= -TEXELS_PER_UNIT * 5 / 2) &&
                           (cy <   TEXELS_PER_UNIT * 5 / 2);
            uint16_t n = noise((uint16_t)x, (uint16_t)y);

            c = (n & 0x80) ? ARENA_SAND_MID
              : (n & 0x40) ? ARENA_SAND_LIGHT : ARENA_SAND_DARK;

            if (in_board) {
                int gx = ((cx + TEXELS_PER_UNIT * 8) % TEXELS_PER_UNIT);
                int gy = ((cy + TEXELS_PER_UNIT * 8) % TEXELS_PER_UNIT);
                /* Slot outline on the cell border, grid line one texel in. */
                if (gx == 0 || gy == 0)
                    c = ARENA_GRID;
                else if (gx == 1 || gy == 1 ||
                         gx == TEXELS_PER_UNIT - 1 || gy == TEXELS_PER_UNIT - 1)
                    c = ARENA_SLOT;
                else if ((n & 0xc0) == 0xc0)
                    c = ARENA_SAND_LIGHT;
                else
                    c = ARENA_SAND_MID;
            }
            g_arena_tex[y * ARENA_TEX_W + x] = (uint8_t)(c << 2);
        }
    }
}

/* ── Card faces ──────────────────────────────────────────────────────────── */

static void build_card_face(uint8_t *dst, uint8_t border, uint8_t body,
                            uint8_t detail)
{
    int x, y;
    for (y = 0; y < CARD_TEX_W; ++y) {
        for (x = 0; x < CARD_TEX_W; ++x) {
            uint8_t c = body;
            if (x < 2 || y < 2 || x >= CARD_TEX_W - 2 || y >= CARD_TEX_W - 2)
                c = border;
            else if (x >= 5 && x < CARD_TEX_W - 5 && y >= 5 && y < 20)
                c = detail;          /* the art window */
            else if (y >= 22 && y < 27 && x >= 5 && x < CARD_TEX_W - 5)
                c = ARENA_BLACK;     /* the stat strip */
            dst[y * CARD_TEX_W + x] = (uint8_t)(c << 2);
        }
    }
}

static void build_card_faces(void)
{
    static const uint8_t body[ATARIST_CARD_FACES] = {
        ARENA_ATTR_B, ARENA_FACE, ARENA_SAND_LIGHT, ARENA_ATTR_A,
        ARENA_ATTR_C, ARENA_SKY_MID, ARENA_BACK, ARENA_SAND_DARK
    };
    static const uint8_t detail[ATARIST_CARD_FACES] = {
        ARENA_SKY_DARK, ARENA_HILIGHT, ARENA_SAND_MID, ARENA_SKY_LIGHT,
        ARENA_WHITE, ARENA_ATTR_A, ARENA_SAND_DARK, ARENA_GRID
    };
    int i;
    for (i = 0; i < ATARIST_CARD_FACES; ++i) {
        uint8_t *dst = g_card_tex + (size_t)i * CARD_TEX_W * CARD_TEX_W;
        uint8_t border = (i == ATARIST_CARD_FACE_BACK) ? ARENA_GRID : ARENA_SLOT;
        build_card_face(dst, border, body[i], detail[i]);
        g_faces[i].texels = dst;
        g_faces[i].w_log2 = ATARIST_CARD_TEX_LOG2;
        g_faces[i].h_log2 = ATARIST_CARD_TEX_LOG2;
    }
}

int Atarist_CardFaceForCard(int card_id, int face_up)
{
    if (!face_up) return ATARIST_CARD_FACE_SET;
    if (card_id < 0 || card_id >= MSX2_CARD_COUNT) return ATARIST_CARD_FACE_BACK;
    /* One face per attribute: the six attribute ids map straight onto the six
     * generated fronts. */
    return g_msx2_card_attr[card_id] % 6;
}

/* ── Bring-up ────────────────────────────────────────────────────────────── */

int Atarist_AssetsInit(void)
{
    int i;
    uint8_t *block;

    for (i = 0; i < 16; ++i) {
        g_arena_pal[i] = Atarist_PackRGB(g_arena_rgb[i][0], g_arena_rgb[i][1],
                                         g_arena_rgb[i][2]);
        g_card_pal[i] = Atarist_PackRGB(g_card_rgb[i][0], g_card_rgb[i][1],
                                        g_card_rgb[i][2]);
    }

    block = (uint8_t *)st_malloc(ARENA_TEX_W * ARENA_TEX_W +
                                 ATARIST_CARD_FACES * CARD_TEX_W * CARD_TEX_W);
    if (!block) return 0;
    g_arena_tex = block;
    g_card_tex = block + ARENA_TEX_W * ARENA_TEX_W;

    build_arena_texture();
    g_arena.texels = g_arena_tex;
    g_arena.w_log2 = ATARIST_ARENA_TEX_LOG2;
    g_arena.h_log2 = ATARIST_ARENA_TEX_LOG2;
    build_card_faces();
    return 1;
}

void Atarist_ApplyArenaPalette(void) { Atarist_SetArenaPalette(g_arena_pal); }
void Atarist_ApplyCardPalette(void)  { Atarist_SetCardPalette(g_card_pal); }
void Atarist_ApplyCardPaletteEverywhere(void)
{
    Atarist_SetWholePalette(g_card_pal);
}

const AtaristTexture *Atarist_ArenaTexture(void) { return &g_arena; }
const AtaristTexture *Atarist_CardFace(int face)
{
    if (face < 0 || face >= ATARIST_CARD_FACES) face = ATARIST_CARD_FACE_BACK;
    return &g_faces[face];
}
