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
#include "cd32x_title_asset.h"

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
#define CD32X_MD_CMD_SAVE_WRITE  0xCD08
#define CD32X_MD_CMD_SAVE_EXISTS 0xCD09
#define CD32X_CD_STATUS_ERROR   0xCDEE
#define CD32X_PRIV_BLOB_SAVE     0x0600

#define WAIFU_ASSET_BLOB_TITLE_SCREEN            0
#define WAIFU_ASSET_BLOB_ENDING_SCREEN           3
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
#define CD32X_CARD_FACE_CHUNK_BYTES                32768
#define CD32X_CARD_FACE_CHUNK_COUNT                5
#define CD32X_CARD_BIG_ONE_BYTES                   (WAIFU_BIG_W * WAIFU_BIG_H)
#define CD32X_CARD_BIG_ONE_WORDS                   (CD32X_CARD_BIG_ONE_BYTES / 2)
#define CD32X_CD_SECTOR_BYTES                      2048
#define CD32X_CARD_BIG_CD_SLOT_BYTES               (((CD32X_CARD_BIG_ONE_BYTES + CD32X_CD_SECTOR_BYTES - 1) / CD32X_CD_SECTOR_BYTES) * CD32X_CD_SECTOR_BYTES)
/* Cards per big-art chunk file (CBGnn.BIN).  Small chunks keep a B-button card
   check down to one ~56 KiB BIOS read instead of a whole 224 KiB 16-card chunk.
   Must match CARDS_PER_CHUNK in tools/cd32x_assets/split_card_big_art.py. */
#define CD32X_CARD_BIG_CHUNK_CARDS                 4
#define CD32X_STORY_PORTRAIT_COUNT                 WAIFU_STORY_PORTRAIT_COUNT
#define CD32X_STORY_PORTRAIT_CD_STRIDE             WAIFU_STORY_PORTRAIT_CD_STRIDE
#define CD32X_STORY_PORTRAIT_WORDS                 (CD32X_STORY_PORTRAIT_CD_STRIDE / 2)
/* A whole portrait plane (COUNT * ~26 KiB = ~156 KiB) does not fit in the
   128 KiB 1M-mode Word RAM bank load_file targets, so the plane is split into
   small chunk files (SPX%02d.BIN pixels / SPM%02d.BIN mask) that each hold a
   few records.  Must match PORTRAITS_PER_CHUNK in
   tools/cd32x_assets/split_story_portraits.py. */
#define CD32X_STORY_PORTRAIT_CHUNK                 2

typedef struct Cd32xBlobInfo {
    int blob;
    const char *filename;
    unsigned short max_words_per_request;
} Cd32xBlobInfo;

static const Cd32xBlobInfo g_cd32x_blobs[] = {
    { WAIFU_ASSET_BLOB_TITLE_SCREEN,        "TITLE_SCREEN_IMG.BIN",      CD32X_TITLE_SCREEN_BYTES / 2 },
    { WAIFU_ASSET_BLOB_ENDING_SCREEN,       "ENDING_SCREEN_IMG.BIN",     CD32X_TITLE_SCREEN_BYTES / 2 },
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

/* See cd32x_service_card_face_request: one CARD_FACE_n.BIN chunk stays resident
   in the Word-RAM staging window between face requests.  Any other Word-RAM
   write clobbers it, so those paths reset this so the next face reloads only
   the needed 32 KiB chunk instead of the whole 145 KiB face atlas. */
static int g_face_chunk_resident = -1;
static int g_face_chunk_resident_size = 0;
static int8_t g_face_card_scratch[CD32X_CARD_ONE_BYTES];
static void cd32x_invalidate_face_atlas(void)
{
    g_face_chunk_resident = -1;
    g_face_chunk_resident_size = 0;
}

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
    /* Top the RF5C164 wave-RAM ring up before the BIOS owns the CD for a card or
       portrait read.  The INT2 pump continues normal vblank refills, but this
       gives blocking reads the largest possible buffered-audio cushion. */
    cd32x_music_prime_for_cd_read();
}

static void cd32x_card_face_chunk_filename(char *out, int chunk_id)
{
    out[0] = 'C'; out[1] = 'A'; out[2] = 'R'; out[3] = 'D';
    out[4] = '_'; out[5] = 'F'; out[6] = 'A'; out[7] = 'C';
    out[8] = 'E'; out[9] = '_';
    out[10] = (char)('0' + chunk_id);
    out[11] = '.'; out[12] = 'B'; out[13] = 'I'; out[14] = 'N'; out[15] = 0;
}

static int cd32x_load_card_face_chunk(int chunk_id, char *word_ram)
{
    char filename[16];
    int rc;
    if (chunk_id < 0 || chunk_id >= CD32X_CARD_FACE_CHUNK_COUNT) return -1;
    if (g_face_chunk_resident == chunk_id) {
        /* The previous CPY handed this Word-RAM bank to the MD/32X side; switch
           it back so the Sub-CPU can read the resident chunk. */
        switch_banks();
        return g_face_chunk_resident_size;
    }
    if (g_face_chunk_resident >= 0) {
        /* Same reason as above, but a new chunk must be loaded over it. */
        switch_banks();
    }
    if (cd32x_set_asset_cwd() < 0) return -1;
    cd32x_card_face_chunk_filename(filename, chunk_id);
    cd32x_before_cd_read();
    rc = load_file(filename, word_ram);
    if (rc < 0) {
        cd32x_invalidate_face_atlas();
        return rc;
    }
    g_face_chunk_resident = chunk_id;     /* load_file leaves the bank Sub-CPU owned */
    g_face_chunk_resident_size = rc;
    return rc;
}

/* Single-card face requests used to load CARD_FACES.BIN (~145 KiB) the first
   time a hand/field card rendered.  That looked like a hang right before the
   opening cards appeared.  Serve the card from the 32 KiB split chunk files
   instead, with a scratch copy because the 2052-byte card records are not
   aligned to the 32 KiB file boundary. */
static void cd32x_service_card_face_request(int card_id, int words, char *word_ram)
{
    int global_offset;
    int chunk_id;
    int chunk_off;
    int remaining;
    int copied;
    int rc;

    if (card_id < 0 || card_id >= CD32X_CARD_SINGLE_COUNT || words != CD32X_CARD_ONE_WORDS) {
        cd32x_fail_cd_request(-1);
        return;
    }

    global_offset = card_id * CD32X_CARD_ONE_BYTES;
    chunk_id = global_offset / CD32X_CARD_FACE_CHUNK_BYTES;
    chunk_off = global_offset % CD32X_CARD_FACE_CHUNK_BYTES;
    remaining = CD32X_CARD_ONE_BYTES;
    copied = 0;

    while (remaining > 0) {
        int available = CD32X_CARD_FACE_CHUNK_BYTES - chunk_off;
        int n = remaining < available ? remaining : available;
        rc = cd32x_load_card_face_chunk(chunk_id, word_ram);
        if (rc < 0) {
            cd32x_fail_cd_request(rc);
            return;
        }
        if (chunk_off < 0 || chunk_off + n > rc) {
            cd32x_fail_cd_request(-1);
            return;
        }
        memcpy(g_face_card_scratch + copied, word_ram + chunk_off, n);
        copied += n;
        remaining -= n;
        ++chunk_id;
        chunk_off = 0;
        if (remaining > 0) cd32x_invalidate_face_atlas();
    }

    memcpy(word_ram, g_face_card_scratch, CD32X_CARD_ONE_BYTES);
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
    char filename[] = "CBG00.BIN";
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
    filename[3] = (char)('0' + (chunk_id / 10) % 10);
    filename[4] = (char)('0' + chunk_id % 10);
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
    char filename[] = "SPX00.BIN";
    int chunk_id;
    int chunk_portrait;
    int byte_offset;
    int rc;

    if (portrait_id < 0 || portrait_id >= CD32X_STORY_PORTRAIT_COUNT || words != CD32X_STORY_PORTRAIT_WORDS) {
        cd32x_fail_cd_request(-1);
        return;
    }
    cd32x_invalidate_face_atlas();

    /* Load only the small chunk file holding this portrait (a few 26 KiB
       records) so the BIOS read fits the 128 KiB Word RAM bank, then slide the
       requested record to the transfer window.  Loading the whole ~156 KiB
       plane overran the bank and hung/crashed the first-opponent plaza load. */
    chunk_id = portrait_id / CD32X_STORY_PORTRAIT_CHUNK;
    chunk_portrait = portrait_id % CD32X_STORY_PORTRAIT_CHUNK;
    byte_offset = chunk_portrait * CD32X_STORY_PORTRAIT_CD_STRIDE;
    filename[2] = mask ? 'M' : 'X';
    filename[3] = (char)('0' + (chunk_id / 10) % 10);
    filename[4] = (char)('0' + chunk_id % 10);
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
    if (byte_offset < 0 || byte_offset + CD32X_STORY_PORTRAIT_CD_STRIDE > rc) {
        cd32x_fail_cd_request(-1);
        return;
    }
    /* Slide the requested record to the transfer window with a byte loop, not
       memcpy: this is a Word RAM -> Word RAM copy and, as in the big-art path,
       the BIOS memcpy's wide/backward moves are unreliable on the shared Word
       RAM bank -- using memcpy here crashed the second-portrait plaza load. */
    if (byte_offset > 0) {
        int i;
        for (i = 0; i < CD32X_STORY_PORTRAIT_CD_STRIDE; ++i) {
            word_ram[i] = word_ram[byte_offset + i];
        }
    }
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

/* -----------------------------------------------------------------------
 * Save persistence: Sega CD internal Backup RAM via the _BURAM BIOS.
 *
 * On a Sega CD 32X the cartridge slot holds the 32X, so the internal 8 KiB
 * Backup RAM is the only save device.  The _BURAM vector (0x5F16) is part of
 * the Sub-CPU BIOS jump table (already used via SPNull at 0x5F3A during init).
 * A single fixed-size record "WAIFUFM" holds the story save blob; the common
 * game code owns its format (a 2-byte length header + payload), so here it is
 * an opaque CD32X_SAVE_RECORD_BYTES byte record.  Save data crosses from the
 * SH-2 to Word RAM through the mirror-of-cpy_to_32x MD routine (CPY_FROM_32X).
 * ----------------------------------------------------------------------- */
#define CD32X_BRM_INIT           0
#define CD32X_BRM_SERCH          2
#define CD32X_BRM_READ           3
#define CD32X_BRM_WRITE          4
#define CD32X_BRM_FORMAT         6
#define CD32X_SAVE_RECORD_BYTES  1024
#define CD32X_SAVE_RECORD_WORDS  (CD32X_SAVE_RECORD_BYTES / 2)
/* Normal-mode Backup RAM blocks are 0x40 bytes each. */
#define CD32X_SAVE_RECORD_BLOCKS (CD32X_SAVE_RECORD_BYTES / 0x40)

static unsigned char g_bram_work[0x640];
static unsigned char g_bram_str[12];
static unsigned char g_bram_buf[CD32X_SAVE_RECORD_BYTES];
static unsigned char g_bram_wparams[14];
/* 11-char Backup RAM filename + terminator. */
static const char g_bram_save_name[12] = { 'W','A','I','F','U','F','M','_','_','_','_', 0 };

/* BRMINIT: 1 = Sega-formatted RAM present, 0 = absent/unformatted (carry set). */
static int buram_init(void)
{
    register unsigned int d0 asm("d0") = CD32X_BRM_INIT;
    register void *a0 asm("a0") = g_bram_work;
    register void *a1 asm("a1") = g_bram_str;
    int ok;
    asm volatile(
        "jsr    0x5F16\n\t"
        "moveq  #1,%0\n\t"
        "bcc    1f\n\t"
        "moveq  #0,%0\n\t"
        "1:\n\t"
        : "=d"(ok), "+d"(d0), "+a"(a0), "+a"(a1)
        :
        : "d1", "cc", "memory");
    return ok;
}

/* BRMFORMAT: 1 = ok.  BRMINIT must have been called first. */
static int buram_format(void)
{
    register unsigned int d0 asm("d0") = CD32X_BRM_FORMAT;
    int ok;
    asm volatile(
        "jsr    0x5F16\n\t"
        "moveq  #1,%0\n\t"
        "bcc    1f\n\t"
        "moveq  #0,%0\n\t"
        "1:\n\t"
        : "=d"(ok), "+d"(d0)
        :
        : "d1", "a0", "a1", "cc", "memory");
    return ok;
}

/* BRMSERCH: 1 = the save file exists. */
static int buram_exists(void)
{
    register unsigned int d0 asm("d0") = CD32X_BRM_SERCH;
    register const void *a0 asm("a0") = g_bram_save_name;
    int found;
    asm volatile(
        "jsr    0x5F16\n\t"
        "moveq  #1,%0\n\t"
        "bcc    1f\n\t"
        "moveq  #0,%0\n\t"
        "1:\n\t"
        : "=d"(found), "+d"(d0), "+a"(a0)
        :
        : "d1", "a1", "cc", "memory");
    return found;
}

/* BRMREAD the save record into g_bram_buf: 1 = ok. */
static int buram_read(void)
{
    register unsigned int d0 asm("d0") = CD32X_BRM_READ;
    register const void *a0 asm("a0") = g_bram_save_name;
    register void *a1 asm("a1") = g_bram_buf;
    int ok;
    asm volatile(
        "jsr    0x5F16\n\t"
        "moveq  #1,%0\n\t"
        "bcc    1f\n\t"
        "moveq  #0,%0\n\t"
        "1:\n\t"
        : "=d"(ok), "+d"(d0), "+a"(a0), "+a"(a1)
        :
        : "d1", "cc", "memory");
    return ok;
}

/* BRMWRITE g_bram_buf to the save record: 1 = ok. */
static int buram_write(void)
{
    register unsigned int d0 asm("d0") = CD32X_BRM_WRITE;
    register const void *a0 asm("a0") = g_bram_wparams;
    register const void *a1 asm("a1") = g_bram_buf;
    register unsigned int d1 asm("d1") = 0;
    int ok;
    asm volatile(
        "jsr    0x5F16\n\t"
        "moveq  #1,%0\n\t"
        "bcc    1f\n\t"
        "moveq  #0,%0\n\t"
        "1:\n\t"
        : "=d"(ok), "+d"(d0), "+d"(d1), "+a"(a0), "+a"(a1)
        :
        : "cc", "memory");
    return ok;
}

static void buram_prepare_wparams(void)
{
    int i;
    for (i = 0; i < 11; ++i) g_bram_wparams[i] = (unsigned char)g_bram_save_name[i];
    g_bram_wparams[11] = 0;  /* normal mode */
    g_bram_wparams[12] = (unsigned char)((CD32X_SAVE_RECORD_BLOCKS >> 8) & 0xFF);
    g_bram_wparams[13] = (unsigned char)(CD32X_SAVE_RECORD_BLOCKS & 0xFF);
}

/* SAVE_EXISTS: BRMINIT then BRMSERCH.  Reports found via COMM4/rc. */
static void cd32x_service_save_exists(void)
{
    int found;
    cd32x_invalidate_face_atlas();
    buram_init();
    found = buram_exists();
    cd32x_finish_md_request(found ? 0 : -1);
}

/* SAVE_WRITE: receive the fixed record from the SH-2 into Word RAM, then
   BRMWRITE it.  Formats the Backup RAM first if it is unformatted. */
static void cd32x_service_save_write(char *word_ram)
{
    int words;
    int rc;

    cd32x_invalidate_face_atlas();
    words = do_md_cmd2(MD_CMD_GET_COMM32X, 6, 2);
    if (words != CD32X_SAVE_RECORD_WORDS) {
        cd32x_finish_md_request(-1);
        return;
    }
    switch_banks();  /* hand Word RAM to the Main CPU for the SH-2 transfer */
    do_md_cmd2(MD_CMD_CPY_FROM_32X, 0x200000, words);
    switch_banks();  /* take Word RAM back to read it */
    memcpy(g_bram_buf, word_ram, CD32X_SAVE_RECORD_BYTES);

    if (!buram_init()) buram_format();
    buram_prepare_wparams();
    rc = buram_write() ? 0 : -1;
    cd32x_finish_md_request(rc);
}

/* SAVE_READ (served through the READ_BLOB path): BRMREAD into Word RAM then
   stream to the SH-2 like any other blob. */
static void cd32x_service_save_read(int words, char *word_ram)
{
    cd32x_invalidate_face_atlas();
    if (words != CD32X_SAVE_RECORD_WORDS) {
        cd32x_fail_cd_request(-1);
        return;
    }
    buram_init();
    if (!buram_exists() || !buram_read()) {
        cd32x_fail_cd_request(-1);
        return;
    }
    memcpy(word_ram, g_bram_buf, CD32X_SAVE_RECORD_BYTES);
    cd32x_transfer_word_ram_to_32x(words);
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
    if (cmd == CD32X_MD_CMD_SAVE_EXISTS) {
        cd32x_service_save_exists();
        return;
    }
    if (cmd == CD32X_MD_CMD_SAVE_WRITE) {
        cd32x_service_save_write(word_ram);
        return;
    }
    if (cmd != CD32X_CD_CMD_READ_BLOB) return;

    words = do_md_cmd2(MD_CMD_GET_COMM32X, 2, 2);
    blob = do_md_cmd2(MD_CMD_GET_COMM32X, 6, 2);
    if (blob == CD32X_PRIV_BLOB_SAVE) {
        cd32x_service_save_read(words, word_ram);
        return;
    }
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
        /* The RF5C164 ring is refilled from the INT2/vblank handler; here we only
           keep the CD chunk source topped up for it. */
        cd32x_music_stream_pump();
    }

    return 0;
}
