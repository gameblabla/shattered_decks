/* ─────────────────────────────────────────────────────────────────────────────
 *  atarist_assets.c — loading the art, and holding it.
 *
 *  One read per file at boot, straight into a Malloc'd block, and no fix-up
 *  pass afterwards: the converter already stores chunky texels multiplied by
 *  four and planar words in shifter order, because an 8 MHz 68000 walking
 *  130 KB to shift it would cost more than the floppy read did.
 *
 *  A missing or unloadable file is not fatal.  The port has to survive on a
 *  512 KB machine, and a duel drawn in flat colours is still a duel; the probe
 *  says which happened.
 * ───────────────────────────────────────────────────────────────────────────── */
#include <stddef.h>

#include "atarist_assets.h"
#include "atarist_video.h"
#include "atarist_disk.h"
#include "atarist_probe.h"
#include "atarist_os.h"

#define ARENA_TEX_W  ATARIST_ART_ARENA_W
#define ARENA_BYTES  (ARENA_TEX_W * ARENA_TEX_W)
#define FIELD_W      ATARIST_ART_FIELD_W
#define FIELD_H      ATARIST_ART_FIELD_H
#define FIELD_BYTES  (FIELD_W * FIELD_H)
#define HAND_W       ATARIST_ART_HAND_W
#define HAND_H       ATARIST_ART_HAND_H
/* Planar: four bitplanes of (w/16) words per row, no mask -- a card's art
 * window is a solid rectangle. */
#define HAND_WORDS   ((HAND_W / 16) * HAND_H * 4)
#define HAND_BYTES   (HAND_WORDS * 2)

static uint16_t g_arena_pal[16];
static uint16_t g_card_pal[16];

static uint8_t *g_arena_tex;
static uint8_t *g_field;               /* ATARIST_ART_FACES textures */
static uint16_t *g_hand;               /* ATARIST_ART_FACES planar images */
static AtaristTexture g_arena;
static AtaristTexture g_faces[ATARIST_ART_FACES];
static AtaristImage   g_hand_img[ATARIST_ART_FACES];
static uint8_t g_have_art;

/* What the board shows when the art did not load: flat sand with the slot
 * outline the geometry needs to stay readable. */
static uint8_t g_fallback_tex[FIELD_BYTES];

static void build_fallback(void)
{
    int x, y;
    for (y = 0; y < FIELD_H; ++y) {
        for (x = 0; x < FIELD_W; ++x) {
            int edge = (x == 0 || y == 0 || x == FIELD_W - 1 || y == FIELD_H - 1);
            g_fallback_tex[y * FIELD_W + x] =
                (uint8_t)((edge ? ARENA_SLOT : ARENA_SAND_MID) << 2);
        }
    }
}

static int load_art(void)
{
    uint8_t *block;
    int i;

    /* One allocation for all three files: three Malloc()s of this size on a
     * TPA that already carries two screens and a music buffer is three chances
     * to fail on a 512 KB machine, and the sizes are fixed anyway. */
    block = (uint8_t *)st_malloc(ARENA_BYTES +
                                 (int32_t)ATARIST_ART_FACES * FIELD_BYTES +
                                 (int32_t)ATARIST_ART_FACES * HAND_BYTES);
    if (!block) {
        g_atarist_probe.status = ATARIST_PROBE_NOMEM;
        ATARIST_MARK(20);
        return 0;
    }
    g_arena_tex = block;
    g_field = block + ARENA_BYTES;
    g_hand = (uint16_t *)(g_field + (size_t)ATARIST_ART_FACES * FIELD_BYTES);

    /* The marks are the blind run's only account of a boot that spends about
     * nineteen seconds reading: 21 before the first file, 24 when all three
     * are in.  A run that comes back NOFILE says which read failed. */
    ATARIST_MARK(21);
    if (Atarist_DiskLoad("DAT\\ARENA.TEX", g_arena_tex, ARENA_BYTES) != ARENA_BYTES)
        return 0;
    ATARIST_MARK(22);
    if (Atarist_DiskLoad("DAT\\FIELD.CRD", g_field,
                         (int32_t)ATARIST_ART_FACES * FIELD_BYTES) !=
        (int32_t)ATARIST_ART_FACES * FIELD_BYTES)
        return 0;
    ATARIST_MARK(23);
    if (Atarist_DiskLoad("DAT\\HAND.CRD", g_hand,
                         (int32_t)ATARIST_ART_FACES * HAND_BYTES) !=
        (int32_t)ATARIST_ART_FACES * HAND_BYTES)
        return 0;
    ATARIST_MARK(24);

    for (i = 0; i < ATARIST_ART_FACES; ++i) {
        g_faces[i].texels = g_field + (size_t)i * FIELD_BYTES;
        g_faces[i].w_log2 = ATARIST_CARD_TEX_LOG2;
        g_faces[i].h_log2 = ATARIST_CARD_TEX_LOG2;
        g_hand_img[i].w = HAND_W;
        g_hand_img[i].h = HAND_H;
        g_hand_img[i].has_mask = 0;
        g_hand_img[i].data = g_hand + (size_t)i * HAND_WORDS;
    }
    g_arena.texels = g_arena_tex;
    g_arena.w_log2 = ATARIST_ARENA_TEX_LOG2;
    g_arena.h_log2 = ATARIST_ARENA_TEX_LOG2;
    return 1;
}

int Atarist_CardFaceForCard(int card_id, int face_up)
{
    if (!face_up) return ATARIST_CARD_FACE_BACK;
    if (card_id < 0 || card_id >= ATARIST_CARD_FACE_BACK)
        return ATARIST_CARD_FACE_BACK;
    return card_id;
}

int Atarist_AssetsInit(void)
{
    int i;

    for (i = 0; i < 16; ++i) {
        g_arena_pal[i] = Atarist_PackRGB(g_atarist_arena_rgb[i][0],
                                         g_atarist_arena_rgb[i][1],
                                         g_atarist_arena_rgb[i][2]);
        g_card_pal[i] = Atarist_PackRGB(g_atarist_card_rgb[i][0],
                                        g_atarist_card_rgb[i][1],
                                        g_atarist_card_rgb[i][2]);
    }
    /* Say so before the reads start.  This is 130 KB off a floppy and it is
     * the longest the machine ever sits with nothing on screen; a blind run
     * (and a player) cannot tell that from a hang. */
    Atarist_ApplyCardPaletteEverywhere();
    Atarist_SetSplitEnabled(0);
    Atarist_ClearPlanar(CARD_BLACK);
    Atarist_DrawTextCentred(ATARIST_SCREEN_W / 2, ATARIST_SCREEN_H / 2 - 4,
                            "LOADING", CARD_WHITE, CARD_BLACK);
    Atarist_VideoPresent();

    build_fallback();
    g_arena.texels = g_fallback_tex;
    g_arena.w_log2 = ATARIST_CARD_TEX_LOG2;
    g_arena.h_log2 = ATARIST_CARD_TEX_LOG2;

    g_have_art = (uint8_t)load_art();
    if (!g_have_art) {
        for (i = 0; i < ATARIST_ART_FACES; ++i) {
            g_faces[i].texels = g_fallback_tex;
            g_faces[i].w_log2 = ATARIST_CARD_TEX_LOG2;
            g_faces[i].h_log2 = ATARIST_CARD_TEX_LOG2;
        }
        if (g_atarist_probe.status == ATARIST_PROBE_OK)
            g_atarist_probe.status = ATARIST_PROBE_NOFILE;
    }
    /* The port itself is fine either way: the caller only fails boot on a
     * machine that could not even make the screen. */
    return 1;
}

int Atarist_AssetsHaveArt(void) { return g_have_art; }

void Atarist_ApplyArenaPalette(void) { Atarist_SetArenaPalette(g_arena_pal); }
void Atarist_ApplyCardPalette(void)  { Atarist_SetCardPalette(g_card_pal); }
void Atarist_ApplyCardPaletteEverywhere(void)
{
    Atarist_SetWholePalette(g_card_pal);
}

const AtaristTexture *Atarist_ArenaTexture(void) { return &g_arena; }

const AtaristTexture *Atarist_CardFace(int face)
{
    if (face < 0 || face >= ATARIST_ART_FACES) face = ATARIST_CARD_FACE_BACK;
    return &g_faces[face];
}

const AtaristImage *Atarist_CardHandImage(int face)
{
    if (!g_have_art) return 0;
    if (face < 0 || face >= ATARIST_ART_FACES) face = ATARIST_CARD_FACE_BACK;
    return &g_hand_img[face];
}
