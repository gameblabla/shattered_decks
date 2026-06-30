/* Sega CD supervisor for the CD32X build.
 *
 * This program is the Mega-CD/Mega-Drive-side boot binary.  It initializes the
 * Sega CD BIOS/CD filesystem, uploads the SH-2 program to the 32X, then stays
 * resident.  The copied Raycaster/Chilly Willy MD-side vblank code continues to
 * feed controller/tick state through MARS comm registers for the SH-2 game.
 *
 * CD-ROM service commands stay here so the SH-2-side game remains isolated
 * behind generic platform APIs. */
#include <stdint.h>
#include <string.h>

#include "cd32x_md_iface.h"
#include "cd32x_files.h"
#include "cd32x_pcm.h"
#include "cd32x_music_pcm.h"
#include "waifu_assets.h"

extern void cd32x_bios_cdda_init(void);
extern int cd32x_bios_cdda_play(int track, int loop);
extern void cd32x_bios_cdda_stop(void);

#define CD32X_COMM_READY        0x0001
#define CD32X_CD_CMD_READ_BLOB  0xCD01
#define CD32X_MD_CMD_SET_FADE   0xCD02
#define CD32X_MD_CMD_CDDA_PLAY  0xCD03
#define CD32X_MD_CMD_CDDA_STOP  0xCD04
#define CD32X_MD_CMD_PCM_PLAY   0xCD05
#define CD32X_MD_CMD_PCM_MUSIC  0xCD06
#define CD32X_MD_CMD_PCM_MUSIC_STOP 0xCD07
#define CD32X_CD_STATUS_ERROR   0xCDEE

#define WAIFU_ASSET_BLOB_TITLE_SCREEN            0
#define WAIFU_ASSET_BLOB_STORY_PORTRAITS         4
#define WAIFU_ASSET_BLOB_STORY_PORTRAIT_MASK     5
#define WAIFU_ASSET_BLOB_CARD_FACES              6
#define WAIFU_ASSET_BLOB_CARD_BIG_ART            7
#define WAIFU_ASSET_BLOB_CARD_BIG_ART_CD         8
#define WAIFU_ASSET_BLOB_CARD_BACK               9
#define WAIFU_ASSET_BLOB_SUPPORT_FACE            10
#define WAIFU_ASSET_BLOB_SUPPORT_BIG_ART         11
#define WAIFU_ASSET_BLOB_SUPPORT_BIG_ART_CD      12
#define CD32X_PRIV_BLOB_CARD_FACE_0               0x0100
#define CD32X_PRIV_BLOB_CARD_SINGLE_0             0x0200
#define CD32X_PRIV_BLOB_CARD_BIG_SINGLE_0         0x0300
#define CD32X_PRIV_BLOB_PORTRAIT_PIXELS_0         0x0400
#define CD32X_PRIV_BLOB_PORTRAIT_MASK_0           0x0500
#define CD32X_CARD_SINGLE_COUNT                    WAIFU_CARD_COUNT
#define CD32X_CARD_ONE_BYTES                       (WAIFU_CARD_W * WAIFU_CARD_H)
#define CD32X_CARD_ONE_WORDS                       (CD32X_CARD_ONE_BYTES / 2)
#define CD32X_CARD_BIG_ONE_BYTES                   (WAIFU_BIG_W * WAIFU_BIG_H)
#define CD32X_CARD_BIG_ONE_WORDS                   (CD32X_CARD_BIG_ONE_BYTES / 2)
#define CD32X_CD_SECTOR_BYTES                      2048
#define CD32X_CARD_BIG_CD_SLOT_BYTES               (((CD32X_CARD_BIG_ONE_BYTES + CD32X_CD_SECTOR_BYTES - 1) / CD32X_CD_SECTOR_BYTES) * CD32X_CD_SECTOR_BYTES)
#define CD32X_CARD_BIG_CHUNK_CARDS                 16
#define CD32X_STORY_PORTRAIT_COUNT                 WAIFU_STORY_PORTRAIT_COUNT
#define CD32X_STORY_PORTRAIT_CD_STRIDE             WAIFU_STORY_PORTRAIT_CD_STRIDE
#define CD32X_STORY_PORTRAIT_WORDS                 (CD32X_STORY_PORTRAIT_CD_STRIDE / 2)

typedef struct Cd32xBlobInfo {
    int blob;
    const char *filename;
    unsigned short max_words_per_request;
} Cd32xBlobInfo;

static const Cd32xBlobInfo g_cd32x_blobs[] = {
    { WAIFU_ASSET_BLOB_TITLE_SCREEN,        "TITLE_SCREEN_IMG.BIN",      38400 },
    { WAIFU_ASSET_BLOB_STORY_PORTRAITS,     "STORY_PORTRAITS.BIN",       0xFFFF },
    { WAIFU_ASSET_BLOB_STORY_PORTRAIT_MASK, "STORY_PORTRAIT_MASK.BIN",   0xFFFF },
    { WAIFU_ASSET_BLOB_CARD_FACES,          "CARD_FACES.BIN",            0xFFFF },
    { CD32X_PRIV_BLOB_CARD_FACE_0 + 0,       "CARD_FACE_0.BIN",           16384 },
    { CD32X_PRIV_BLOB_CARD_FACE_0 + 1,       "CARD_FACE_1.BIN",           16384 },
    { CD32X_PRIV_BLOB_CARD_FACE_0 + 2,       "CARD_FACE_2.BIN",           16384 },
    { CD32X_PRIV_BLOB_CARD_FACE_0 + 3,       "CARD_FACE_3.BIN",           16384 },
    { CD32X_PRIV_BLOB_CARD_FACE_0 + 4,       "CARD_FACE_4.BIN",           9216 },
    { WAIFU_ASSET_BLOB_CARD_BIG_ART,        "CARD_BIG_ART.BIN",          0xFFFF },
    { WAIFU_ASSET_BLOB_CARD_BIG_ART_CD,     "CARD_BIG_ART_CD.BIN",       0xFFFF },
    { WAIFU_ASSET_BLOB_CARD_BACK,           "CARD_BACK.BIN",             1026 },
    { WAIFU_ASSET_BLOB_SUPPORT_FACE,        "SUPPORT_FACE.BIN",          1026 },
    { WAIFU_ASSET_BLOB_SUPPORT_BIG_ART,     "SUPPORT_BIG_ART.BIN",       6272 },
    { WAIFU_ASSET_BLOB_SUPPORT_BIG_ART_CD,  "SUPPORT_BIG_ART_CD.BIN",    7168 }
};

static const Cd32xBlobInfo *cd32x_blob_info(int blob)
{
    unsigned i;
    for (i = 0; i < sizeof(g_cd32x_blobs) / sizeof(g_cd32x_blobs[0]); ++i) {
        if (g_cd32x_blobs[i].blob == blob) return &g_cd32x_blobs[i];
    }
    return 0;
}

/* See cd32x_service_card_face_request: the CARD_FACES.BIN atlas stays resident
   in the Word-RAM staging window between face requests.  Any other Word-RAM
   write clobbers it, so those paths reset this so the next face reloads it. */
static int g_face_atlas_resident = 0;
static void cd32x_invalidate_face_atlas(void) { g_face_atlas_resident = 0; }

static int g_cd32x_cwd = -1;

static int cd32x_set_asset_cwd(void)
{
    if (g_cd32x_cwd == 0) return 0;
    if (set_cwd("/ASSETS") < 0) return -1;
    g_cd32x_cwd = 0;
    return 0;
}

static int cd32x_set_music_cwd(void)
{
    if (g_cd32x_cwd == 1) return 0;
    if (set_cwd("/MUSIC") < 0) return -1;
    g_cd32x_cwd = 1;
    return 0;
}


static void cd32x_force_md_h40(void)
{
    volatile unsigned short *vdp_ctrl = (volatile unsigned short *)0xC00004;
    /* 32X 320-wide bitmap output still depends on the MD VDP being in H40.
       The Raycaster/Chilly Willy MD init path sets this, but the CD BIOS/32X
       handoff can leave blastem's capture in H32 unless we restate it after
       SH-2 upload. */
    *vdp_ctrl = 0x8C81;
}

static void cd32x_delay(int frames)
{
    unsigned int target = GET_TICKS + (unsigned int)frames;
    while (target > GET_TICKS) {
    }
}

static void cd32x_put_status(const char *text, int color, int x, int y)
{
    char *word_ram = (char *)0x0C0000;
    cd32x_invalidate_face_atlas();
    strncpy(word_ram, text, 255);
    word_ram[255] = 0;
    switch_banks();
    do_md_cmd4(MD_CMD_PUT_STR, 0x200000, color, x, y);
}

static void cd32x_set_comm_word(int offset, int value)
{
    do_md_cmd3(MD_CMD_SET_COMM32X, offset, 2, value);
}

static void cd32x_fail_cd_request(int code)
{
    cd32x_set_comm_word(4, CD32X_CD_STATUS_ERROR);
    cd32x_set_comm_word(6, code);
    cd32x_set_comm_word(0, 0);
}


static unsigned short cd32x_md_fade_channel(unsigned value, unsigned fade_q8)
{
    value = (value * fade_q8 + 128u) >> 8;
    return (unsigned short)(value > 15u ? 15u : value);
}

static unsigned short cd32x_md_rgb444(unsigned r, unsigned g, unsigned b, unsigned fade_q8)
{
    r = cd32x_md_fade_channel(r, fade_q8);
    g = cd32x_md_fade_channel(g, fade_q8);
    b = cd32x_md_fade_channel(b, fade_q8);
    /* Mega Drive CRAM uses 0x0BGR nibble ordering in the Chilly Willy
       helpers used by this port: red is the low nibble, green the middle
       nibble, and blue the high nibble. */
    return (unsigned short)((b << 8) | (g << 4) | r);
}

static void cd32x_finish_md_request(int rc)
{
    cd32x_set_comm_word(4, rc < 0 ? CD32X_CD_STATUS_ERROR : 0);
    cd32x_set_comm_word(6, rc);
    cd32x_set_comm_word(0, 0);
}

static void cd32x_service_md_fade_request(void)
{
    char *word_ram = (char *)0x0C0000;
    unsigned short *pal = (unsigned short *)word_ram;
    unsigned fade_q8 = (unsigned)do_md_cmd2(MD_CMD_GET_COMM32X, 2, 2);
    cd32x_invalidate_face_atlas();
    if (fade_q8 > 256u) fade_q8 = 256u;

    /* Palette groups used by the resident MD text plane:
       palette 0: black / white, palette 1: black / green, palette 2: black / red. */
    pal[0] = 0x0000;
    pal[1] = cd32x_md_rgb444(12u, 12u, 12u, fade_q8);
    switch_banks();
    do_md_cmd3(MD_CMD_SET_PALETTE, 0x200000, 0, 2);

    pal = (unsigned short *)word_ram;
    pal[0] = 0x0000;
    pal[1] = cd32x_md_rgb444(0u, 10u, 0u, fade_q8);
    switch_banks();
    do_md_cmd3(MD_CMD_SET_PALETTE, 0x200000, 16, 2);

    pal = (unsigned short *)word_ram;
    pal[0] = 0x0000;
    pal[1] = cd32x_md_rgb444(10u, 0u, 0u, fade_q8);
    switch_banks();
    do_md_cmd3(MD_CMD_SET_PALETTE, 0x200000, 32, 2);

    cd32x_finish_md_request(0);
}

static int g_cd32x_cdda_initialized;
/* Track the active CD-DA selection so it can be re-asserted after a data read.
   On Sega CD a BIOS data read (load_file) stops CD-DA, and the SH-2 side only
   re-issues music on a track *change*, so the supervisor must resume the
   current track itself after every card/portrait/blob read. */
static int g_cd32x_cdda_track = -1;
static int g_cd32x_cdda_loop = 0;
static int g_cd32x_cdda_playing = 0;
static const char *g_cd32x_pcm_music_stem = 0;
static int g_cd32x_pcm_music_chunks = 0;
static int g_cd32x_pcm_music_next_chunk = 0;

static void cd32x_ensure_cdda_initialized(void)
{
    if (!g_cd32x_cdda_initialized) {
        cd32x_bios_cdda_init();
        g_cd32x_cdda_initialized = 1;
    }
}

static void cd32x_service_cdda_request(int cmd)
{
    int rc = 0;
    if (cmd == CD32X_MD_CMD_CDDA_PLAY) {
        int track = do_md_cmd2(MD_CMD_GET_COMM32X, 2, 2);
        int loop = do_md_cmd2(MD_CMD_GET_COMM32X, 6, 2);
        if (track < 2 || track > 99) rc = -1;
        else {
            cd32x_ensure_cdda_initialized();
            rc = cd32x_bios_cdda_play(track, loop != 0);
            g_cd32x_cdda_track = track;
            g_cd32x_cdda_loop = loop != 0;
            g_cd32x_cdda_playing = 1;
        }
    } else if (cmd == CD32X_MD_CMD_CDDA_STOP) {
        cd32x_bios_cdda_stop();
        g_cd32x_cdda_playing = 0;
    } else {
        rc = -1;
    }
    cd32x_finish_md_request(rc);
}

/* Re-assert the current CD-DA track after a BIOS data read stopped it.  Called
   after each card/portrait/blob read so battle/menu music survives card loads. */
static void cd32x_cdda_resume_after_read(void)
{
    if (g_cd32x_cdda_playing && g_cd32x_cdda_track >= 2) {
        cd32x_ensure_cdda_initialized();
        cd32x_bios_cdda_play(g_cd32x_cdda_track, g_cd32x_cdda_loop);
    }
}

static void cd32x_before_cd_read(void)
{
    cd32x_music_pump();
}

/* The whole CARD_FACES.BIN atlas (~144 KiB) is loaded into the Word-RAM staging
   window once and kept resident, so each face is served by a pure Word-RAM slide
   + CPY with no CD access.  The old behaviour re-loaded the entire atlas through
   the BIOS for every single face (a ~144 KiB read each time), which made every
   first-time face render do a full-atlas CD load — the deck editor and the board
   could spend many seconds loading faces one at a time.  Any other Word-RAM user
   (big art, portraits, fade palette, status text, generic blob, SFX) overwrites
   the staging window, so each of those calls cd32x_invalidate_face_atlas() and
   the next face reloads the atlas once.  The resident path re-acquires Sub-CPU
   ownership of the Word-RAM bank before the slide, mirroring the bank state a
   fresh load_file would have left. */
static void cd32x_service_card_face_request(int card_id, int words, char *word_ram)
{
    int byte_offset;
    int rc;

    if (card_id < 0 || card_id >= CD32X_CARD_SINGLE_COUNT || words != CD32X_CARD_ONE_WORDS) {
        cd32x_fail_cd_request(-1);
        return;
    }

    if (!g_face_atlas_resident) {
        if (cd32x_set_asset_cwd() < 0) {
            cd32x_fail_cd_request(-1);
            return;
        }
        cd32x_before_cd_read();
        rc = load_file((char *)"CARD_FACES.BIN", word_ram);
        if (rc < 0) {
            cd32x_fail_cd_request(rc);
            return;
        }
        g_face_atlas_resident = 1;     /* load_file leaves the bank Sub-CPU owned */
    } else {
        /* The previous face request ended with the Main-CPU owning the bank for
           its CPY; hand it back to the Sub-CPU so the slide can read the atlas. */
        switch_banks();
    }

    byte_offset = card_id * CD32X_CARD_ONE_BYTES;
    memcpy(word_ram, word_ram + byte_offset, CD32X_CARD_ONE_BYTES);
    switch_banks();
    rc = do_md_cmd2(MD_CMD_CPY_TO_32X, 0x200000, words);
    if (rc < 0) cd32x_fail_cd_request(rc);
    cd32x_cdda_resume_after_read();
}

static void cd32x_transfer_word_ram_to_32x(int words)
{
    int rc;
    switch_banks();
    rc = do_md_cmd2(MD_CMD_CPY_TO_32X, 0x200000, words);
    if (rc < 0) cd32x_fail_cd_request(rc);
    else cd32x_cdda_resume_after_read();
}

static void cd32x_service_card_big_request(int card_id, int words, char *word_ram)
{
    char filename[] = "CARD_BG0.BIN";
    int chunk_id;
    int chunk_card;
    int byte_offset;
    int rc;

    if (card_id < 0 || card_id >= CD32X_CARD_SINGLE_COUNT || words != CD32X_CARD_BIG_ONE_WORDS) {
        cd32x_fail_cd_request(-1);
        return;
    }
    cd32x_invalidate_face_atlas();

    chunk_id = card_id / CD32X_CARD_BIG_CHUNK_CARDS;
    chunk_card = card_id % CD32X_CARD_BIG_CHUNK_CARDS;
    byte_offset = chunk_card * CD32X_CARD_BIG_CD_SLOT_BYTES;
    filename[7] = (char)('0' + chunk_id);
    if (cd32x_set_asset_cwd() < 0) {
        cd32x_fail_cd_request(-1);
        return;
    }
    cd32x_before_cd_read();
    rc = load_file(filename, word_ram);
    if (rc < 0) {
        cd32x_fail_cd_request(rc);
        return;
    }
    if (byte_offset < 0 || byte_offset + CD32X_CARD_BIG_ONE_BYTES > rc) {
        cd32x_fail_cd_request(-1);
        return;
    }
    if (byte_offset > 0) {
        int i;
        for (i = 0; i < CD32X_CARD_BIG_ONE_BYTES; ++i) {
            word_ram[i] = word_ram[byte_offset + i];
        }
    }
    cd32x_transfer_word_ram_to_32x(words);
}

static void cd32x_service_portrait_request(int portrait_id, int words, int mask, char *word_ram)
{
    const char *filename = mask ? "STORY_PORTRAIT_MASK.BIN" : "STORY_PORTRAITS.BIN";
    int byte_offset;
    int rc;

    if (portrait_id < 0 || portrait_id >= CD32X_STORY_PORTRAIT_COUNT || words != CD32X_STORY_PORTRAIT_WORDS) {
        cd32x_fail_cd_request(-1);
        return;
    }
    cd32x_invalidate_face_atlas();

    /* Portrait planes are only ~156 KiB each, so load the whole plane through
       the proven BIOS path and slide the requested record to the transfer
       window.  Raw read_cd slices can hang if story-map CD-DA is still active. */
    if (cd32x_set_asset_cwd() < 0) {
        cd32x_fail_cd_request(-1);
        return;
    }
    cd32x_before_cd_read();
    rc = load_file((char *)filename, word_ram);
    if (rc < 0) {
        cd32x_fail_cd_request(rc);
        return;
    }
    byte_offset = portrait_id * CD32X_STORY_PORTRAIT_CD_STRIDE;
    if (byte_offset < 0 || byte_offset + CD32X_STORY_PORTRAIT_CD_STRIDE > rc) {
        cd32x_fail_cd_request(-1);
        return;
    }
    memcpy(word_ram, word_ram + byte_offset, CD32X_STORY_PORTRAIT_CD_STRIDE);
    cd32x_transfer_word_ram_to_32x(words);
}

static void cd32x_music_chunk_filename(char *out, const char *stem, int chunk)
{
    int i = 0;
    while (stem && *stem && i < 6) out[i++] = *stem++;
    out[i++] = (char)('0' + ((chunk / 10) % 10));
    out[i++] = (char)('0' + (chunk % 10));
    out[i++] = '.';
    out[i++] = 'B';
    out[i++] = 'I';
    out[i++] = 'N';
    out[i] = 0;
}

static int cd32x_music_theme_info(int theme_id, const char **stem, int *chunks)
{
    switch (theme_id) {
    case WAIFU_CD32X_MUSIC_DECK_EDITOR_ID:
        *stem = WAIFU_CD32X_MUSIC_DECK_EDITOR_STEM;
        *chunks = WAIFU_CD32X_MUSIC_DECK_EDITOR_CHUNKS;
        return 1;
    case WAIFU_CD32X_MUSIC_BATTLE_ID:
        *stem = WAIFU_CD32X_MUSIC_BATTLE_STEM;
        *chunks = WAIFU_CD32X_MUSIC_BATTLE_CHUNKS;
        return 1;
    case WAIFU_CD32X_MUSIC_BOSS_ID:
        *stem = WAIFU_CD32X_MUSIC_BOSS_STEM;
        *chunks = WAIFU_CD32X_MUSIC_BOSS_CHUNKS;
        return 1;
    case WAIFU_CD32X_MUSIC_FINAL_BOSS_ID:
        *stem = WAIFU_CD32X_MUSIC_FINAL_BOSS_STEM;
        *chunks = WAIFU_CD32X_MUSIC_FINAL_BOSS_CHUNKS;
        return 1;
    default:
        *stem = 0;
        *chunks = 0;
        return 0;
    }
}

static int cd32x_load_music_chunk(const char *stem, int chunk, char *word_ram)
{
    char filename[16];
    int rc;
    cd32x_music_chunk_filename(filename, stem, chunk);
    if (cd32x_set_music_cwd() < 0) return -1;
    cd32x_before_cd_read();
    rc = load_file(filename, word_ram);
    if (rc < 0) return rc;
    if ((uint32_t)rc > cd32x_music_clip_capacity()) rc = (int)cd32x_music_clip_capacity();
    if (rc <= 0) return -1;
    memcpy(cd32x_music_clip_buffer(), word_ram, rc);
    return rc;
}

static void cd32x_clear_music_stream(void)
{
    cd32x_music_stop();
    g_cd32x_pcm_music_stem = 0;
    g_cd32x_pcm_music_chunks = 0;
    g_cd32x_pcm_music_next_chunk = 0;
}

/* Load the first in-duel / deck-editor theme chunk from CD into the Sub-CPU
   PRG-RAM music source buffer and start the RF5C164 ring.  Later chunks are
   loaded by cd32x_music_stream_pump() when the current source chunk has been
   queued into wave RAM.  The chunk load uses Word RAM only as a temporary CD
   staging buffer, so card art keeps its existing chunked transfer semantics. */
static int cd32x_start_music(int theme_id)
{
    char *word_ram = (char *)0x0C0000;
    const char *stem;
    int chunks;
    int rc;

    if (!cd32x_music_theme_info(theme_id, &stem, &chunks)) {
        cd32x_clear_music_stream();
        return 0;
    }
    if (chunks <= 0) return -1;

    cd32x_clear_music_stream();
    cd32x_invalidate_face_atlas();
    rc = cd32x_load_music_chunk(stem, 0, word_ram);
    if (rc < 0) return rc;
    cd32x_music_start((uint32_t)rc);
    g_cd32x_pcm_music_stem = stem;
    g_cd32x_pcm_music_chunks = chunks;
    g_cd32x_pcm_music_next_chunk = chunks > 1 ? 1 : 0;
    return 0;
}

static void cd32x_music_stream_pump(void)
{
    char *word_ram = (char *)0x0C0000;
    int rc;
    if (!g_cd32x_pcm_music_stem || g_cd32x_pcm_music_chunks <= 0) return;
    if (!cd32x_music_needs_chunk()) return;
    cd32x_invalidate_face_atlas();
    rc = cd32x_load_music_chunk(g_cd32x_pcm_music_stem, g_cd32x_pcm_music_next_chunk, word_ram);
    if (rc <= 0) return;
    cd32x_music_supply_chunk((uint32_t)rc);
    ++g_cd32x_pcm_music_next_chunk;
    if (g_cd32x_pcm_music_next_chunk >= g_cd32x_pcm_music_chunks) {
        g_cd32x_pcm_music_next_chunk = 0;
    }
}

static void cd32x_service_cd_request(void)
{
    const Cd32xBlobInfo *info;
    char *word_ram = (char *)0x0C0000;
    int state;
    int cmd;
    int blob;
    int words;
    int rc;

    state = do_md_cmd2(MD_CMD_GET_COMM32X, 0, 2);
    if (state != CD32X_COMM_READY) return;

    cmd = do_md_cmd2(MD_CMD_GET_COMM32X, 4, 2);
    if (cmd == CD32X_MD_CMD_SET_FADE) {
        cd32x_service_md_fade_request();
        return;
    }
    if (cmd == CD32X_MD_CMD_CDDA_PLAY || cmd == CD32X_MD_CMD_CDDA_STOP) {
        cd32x_service_cdda_request(cmd);
        return;
    }
    if (cmd == CD32X_MD_CMD_PCM_MUSIC) {
        int theme = do_md_cmd2(MD_CMD_GET_COMM32X, 2, 2);
        cd32x_finish_md_request(cd32x_start_music(theme));
        return;
    }
    if (cmd == CD32X_MD_CMD_PCM_MUSIC_STOP) {
        cd32x_clear_music_stream();
        cd32x_finish_md_request(0);
        return;
    }
    if (cmd == CD32X_MD_CMD_PCM_PLAY) {
        int effect = do_md_cmd2(MD_CMD_GET_COMM32X, 2, 2);
        cd32x_finish_md_request(cd32x_pcm_sfx_play(effect));
        return;
    }
    if (cmd != CD32X_CD_CMD_READ_BLOB) return;

    words = do_md_cmd2(MD_CMD_GET_COMM32X, 2, 2);
    blob = do_md_cmd2(MD_CMD_GET_COMM32X, 6, 2);
    if (blob >= CD32X_PRIV_BLOB_CARD_SINGLE_0 &&
        blob < CD32X_PRIV_BLOB_CARD_SINGLE_0 + CD32X_CARD_SINGLE_COUNT) {
        cd32x_service_card_face_request(blob - CD32X_PRIV_BLOB_CARD_SINGLE_0, words, word_ram);
        return;
    }
    if (blob >= CD32X_PRIV_BLOB_CARD_BIG_SINGLE_0 &&
        blob < CD32X_PRIV_BLOB_CARD_BIG_SINGLE_0 + CD32X_CARD_SINGLE_COUNT) {
        cd32x_service_card_big_request(blob - CD32X_PRIV_BLOB_CARD_BIG_SINGLE_0, words, word_ram);
        return;
    }
    if (blob >= CD32X_PRIV_BLOB_PORTRAIT_PIXELS_0 &&
        blob < CD32X_PRIV_BLOB_PORTRAIT_PIXELS_0 + CD32X_STORY_PORTRAIT_COUNT) {
        cd32x_service_portrait_request(blob - CD32X_PRIV_BLOB_PORTRAIT_PIXELS_0, words, 0, word_ram);
        return;
    }
    if (blob >= CD32X_PRIV_BLOB_PORTRAIT_MASK_0 &&
        blob < CD32X_PRIV_BLOB_PORTRAIT_MASK_0 + CD32X_STORY_PORTRAIT_COUNT) {
        cd32x_service_portrait_request(blob - CD32X_PRIV_BLOB_PORTRAIT_MASK_0, words, 1, word_ram);
        return;
    }
    info = cd32x_blob_info(blob);
    if (!info || words <= 0 || (unsigned)words > info->max_words_per_request) {
        cd32x_fail_cd_request(-1);
        return;
    }
    cd32x_invalidate_face_atlas();

    if (cd32x_set_asset_cwd() < 0) {
        cd32x_fail_cd_request(-1);
        return;
    }
    cd32x_before_cd_read();
    rc = load_file((char *)info->filename, word_ram);
    if (rc < 0) {
        cd32x_fail_cd_request(rc);
        return;
    }

    switch_banks();
    rc = do_md_cmd2(MD_CMD_CPY_TO_32X, 0x200000, words);
    if (rc < 0) {
        cd32x_fail_cd_request(rc);
        return;
    }
    cd32x_cdda_resume_after_read();
}

int main(void)
{
    char *word_ram = (char *)0x0C0000;
    int rc;

    cd32x_put_status("Waifu FM CD32X", TEXT_WHITE, 12, 2);
    cd32x_put_status("Initializing CD...", TEXT_GREEN, 10, 4);
    init_cd();
    cd32x_set_asset_cwd();

    cd32x_put_status("Loading PCM SFX...", TEXT_GREEN, 9, 5);
    cd32x_set_asset_cwd();
    rc = load_file((char *)"SFX_PCM.BIN", word_ram);
    if (rc > 0) {
        cd32x_pcm_sfx_init_from_bank((const int8_t *)word_ram, (uint32_t)rc);
#ifdef CD32X_PCM_BOOT_PROBE
        cd32x_pcm_sfx_play(1);
#endif
    }

    cd32x_put_status("Uploading SH2 app...", TEXT_GREEN, 9, 6);
    memcpy(word_ram, &sh2_app_start[8], sh2_app_length - 8);
    switch_banks();
    rc = do_md_cmd2(MD_CMD_INIT_32X, 0x200000, sh2_app_length);

    if (rc < 0) {
        if (rc == -2) cd32x_put_status("32X not detected", TEXT_RED, 12, 10);
        else if (rc == -3) cd32x_put_status("32X SDRAM error", TEXT_RED, 11, 10);
        else cd32x_put_status("32X init failed", TEXT_RED, 12, 10);
        for (;;) {
        }
    }

    cd32x_force_md_h40();

    cd32x_put_status("SH2 running", TEXT_GREEN, 14, 8);
    cd32x_delay(60);
    cd32x_put_status("           ", TEXT_GREEN, 14, 8);

    for (;;) {
        cd32x_service_cd_request();
        cd32x_music_pump();
        cd32x_music_stream_pump();
    }

    return 0;
}
