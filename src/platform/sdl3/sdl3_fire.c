/* PSX-DOOM fire for the PC build.
 *
 * The console targets run a small propagation fire in the game's own 8bpp
 * palette (main.c draw_oldschool_fire): a 128x100 intensity buffer with six
 * flame colours, blitted 2x.  On a desktop GPU there is no reason to be that
 * coarse, so the PC frontend answers waifu_platform_fire() with the original
 * PlayStation DOOM algorithm at its own resolution and its own 37-step colour
 * ramp (the palette from the FM TOWNS reference port in
 * PSX-DOOM-FIRE-FOR-FM-TOWNS/), uploaded as a true-colour image.
 *
 * SPEED.  The console fire climbs one cell per frame and a cell is two screen
 * pixels tall, so at 60 Hz the flames raced up the screen.  Here a cell is
 * roughly one screen pixel and the decay is probabilistic (one step in four,
 * against the reference's one in two), which both slows the climb and lets the
 * flames stand tall enough to fill the frame -- the two things that made the
 * story fire read as "too fast" on PC. */

#include "sdl3_internal.h"
#include "platform.h"
#include "defines.h"

#include <SDL3/SDL.h>

#define FIRE_W 512
#define FIRE_H 168
#define FIRE_TOP 36            /* hottest palette index */

/* The reference port's 37-entry ramp, black through red/orange/yellow to
   white. Stored as the frontend's packed RGBA (ABGR little-endian). */
static const uint8_t k_fire_rgb[FIRE_TOP + 1][3] = {
    { 0x07,0x07,0x07 }, { 0x1F,0x07,0x07 }, { 0x2F,0x0F,0x07 }, { 0x47,0x0F,0x07 },
    { 0x57,0x17,0x07 }, { 0x67,0x1F,0x07 }, { 0x77,0x1F,0x07 }, { 0x8F,0x27,0x07 },
    { 0x9F,0x2F,0x07 }, { 0xAF,0x3F,0x07 }, { 0xBF,0x47,0x07 }, { 0xC7,0x47,0x07 },
    { 0xDF,0x4F,0x07 }, { 0xDF,0x57,0x07 }, { 0xDF,0x57,0x07 }, { 0xD7,0x5F,0x07 },
    { 0xD7,0x5F,0x07 }, { 0xD7,0x67,0x0F }, { 0xCF,0x6F,0x0F }, { 0xCF,0x77,0x0F },
    { 0xCF,0x7F,0x0F }, { 0xCF,0x87,0x17 }, { 0xC7,0x87,0x17 }, { 0xC7,0x8F,0x17 },
    { 0xC7,0x97,0x1F }, { 0xBF,0x9F,0x1F }, { 0xBF,0x9F,0x1F }, { 0xBF,0xA7,0x27 },
    { 0xBF,0xA7,0x27 }, { 0xBF,0xAF,0x2F }, { 0xB7,0xAF,0x2F }, { 0xB7,0xB7,0x2F },
    { 0xB7,0xB7,0x37 }, { 0xCF,0xCF,0x6F }, { 0xDF,0xDF,0x9F }, { 0xEF,0xEF,0xC7 },
    { 0xFF,0xFF,0xFF }
};

static uint8_t g_cells[FIRE_W * FIRE_H];
static uint32_t g_rgba[FIRE_W * FIRE_H];
static uint32_t g_lut[FIRE_TOP + 1];
static uint32_t g_rng = 0x2545f491u;
static int g_ready;

static uint32_t rng_next(void)
{
    uint32_t r = g_rng;
    r ^= r << 13; r ^= r >> 17; r ^= r << 5;
    g_rng = r;
    return r;
}

static void fire_init(void)
{
    int i;
    for (i = 0; i <= FIRE_TOP; ++i) {
        /* Index 0 is the ramp's near-black; the frontend keys it fully
           transparent instead so the flames sit over the scene rather than
           over a grey slab. */
        g_lut[i] = (i == 0) ? 0u
                            : ((uint32_t)k_fire_rgb[i][0] |
                               ((uint32_t)k_fire_rgb[i][1] << 8) |
                               ((uint32_t)k_fire_rgb[i][2] << 16) |
                               0xFF000000u);
    }
    SDL_memset(g_cells, 0, sizeof(g_cells));
    /* Light the bottom row. */
    SDL_memset(g_cells + (size_t)(FIRE_H - 1) * FIRE_W, FIRE_TOP, FIRE_W);
    g_ready = 1;
}

/* One spread step, straight from the PSX algorithm: every cell takes the cell
   below it, loses a little, and drifts one column left or right. */
static void fire_spread(void)
{
    int y, x;
    for (y = FIRE_H - 1; y > 0; --y) {
        const uint8_t *below = g_cells + (size_t)y * FIRE_W;
        uint8_t *row = g_cells + (size_t)(y - 1) * FIRE_W;
        uint32_t bits = 0;
        int have = 0;
        for (x = 0; x < FIRE_W; ++x) {
            int v = below[x];
            int dst;
            if (have == 0) { bits = rng_next(); have = 8; }
            if (v == 0) {
                /* Cold cell: put the zero straight above, exactly as the
                   reference does. Drifting it would leave the cell overhead
                   holding last frame's value and the flame would never die. */
                row[x] = 0;
                bits >>= 4; --have;
                continue;
            }
            /* One bit each for the sideways drift; two more for the decay,
               which fires one step in four so the flames stand tall. */
            dst = x + (int)(bits & 1u) - (int)((bits >> 1) & 1u);
            v -= (int)(((bits >> 2) & 3u) == 0u);
            bits >>= 4; --have;
            if (dst < 0) dst = 0;
            else if (dst >= FIRE_W) dst = FIRE_W - 1;
            row[dst] = (uint8_t)v;
        }
    }
}

int waifu_platform_fire(int x, int y, int w, int h)
{
    int i, n = FIRE_W * FIRE_H;
    if (w <= 0 || h <= 0) return 1;
    if (!g_ready) fire_init();
    fire_spread();
    for (i = 0; i < n; ++i) g_rgba[i] = g_lut[g_cells[i]];
    return waifu_sdl3_push_rgba_image(g_rgba, FIRE_W, FIRE_H, x, y, w, h);
}
