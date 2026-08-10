#include "icm.h"
#include "io.h"

/*
 * See icm.h. Everything here is a copy out of the card's window in the
 * physical address space; the only state worth having is which bank is
 * currently in that window.
 */

/* Does the card have to be paged into view at all? On a 386DX-class
 * machine the window is 16MB and nothing this project ships comes close
 * to filling it, so the bank register is never touched (and its
 * encoding there differs - bits 4-5, 4MB granularity - which is exactly
 * the sort of difference not worth carrying code for until a card needs
 * it). tools/mkicm.py refuses to build an image bigger than the window
 * when banking is off. */
#if FMT_ICM_WINDOW < 0x01000000
#define ICM_BANKED  1
#else
#define ICM_BANKED  0
#endif

static uint32_t g_bank = 0xFFFFFFFFu;   /* no bank known to be selected yet */
static uint32_t g_card_bytes;
static uint32_t g_toc_off;
static uint32_t g_toc_count;

static void icm_bank(uint32_t bank)
{
#if ICM_BANKED
    if (bank != g_bank) {
        outb((uint8_t)bank, FMT_ICM_IO_BANK);
        g_bank = bank;
    }
#else
    (void)bank;
#endif
}

static const uint8_t *icm_map(uint32_t off)
{
    icm_bank(off >> FMT_ICM_BANK_SHIFT);
    return (const uint8_t *)(FMT_ICM_CARD_BASE + (off & (FMT_ICM_WINDOW - 1u)));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/*
 * Bank 0 is the resting state of the window, and this is not a detail:
 * the payload's own read-only data is *in* bank 0 (src/boot/icm.lds.S
 * links .rodata into card space), so for as long as another bank is
 * selected the program's constants are not there. Every string, every
 * lookup table, every embedded asset silently reads as whatever that
 * other bank happens to hold.
 *
 * So a bank other than 0 may only be selected inside a read, between
 * whose start and end nothing touches a const - the copy loop below
 * uses only registers and its caller's buffer - and the window is put
 * back before returning to code that does.
 *
 * That is exactly the failure this cost an afternoon to find: with an
 * asset early on the card everything worked, and with the same asset
 * placed past the first megabyte the MP2 decoder ran for a few seconds
 * and then decoded silence forever - it was reading its own synthesis
 * tables out of the middle of the song.
 */
static void icm_rest(void)
{
    icm_bank(0);
}

int fmt_icm_read(uint32_t off, uint32_t bytes, void *buf)
{
    uint8_t *dst = (uint8_t *)buf;

    /* g_card_bytes is 0 until fmt_icm_init() has read the header, which
     * is itself a read - so the check is skipped for exactly that one. */
    if (g_card_bytes && (off > g_card_bytes || bytes > g_card_bytes - off)) {
        return -1;
    }

    while (bytes) {
        uint32_t in_window = FMT_ICM_WINDOW - (off & (FMT_ICM_WINDOW - 1u));
        uint32_t n = bytes < in_window ? bytes : in_window;
        const uint8_t *src = icm_map(off);

        off += n;
        bytes -= n;

        /* 32-bit copies where both ends allow it. The card is a 16-bit
         * device, so the bus cycles happen either way; this is about the
         * loop overhead, which the video stream copies megabytes through. */
        if ((((uint32_t)dst | (uint32_t)src) & 3u) == 0) {
            uint32_t words = n >> 2;
            const uint32_t *s32 = (const uint32_t *)src;
            uint32_t *d32 = (uint32_t *)dst;
            while (words--) {
                *d32++ = *s32++;
            }
            src = (const uint8_t *)s32;
            dst = (uint8_t *)d32;
            n &= 3u;
        }
        while (n--) {
            *dst++ = *src++;
        }
    }
    icm_rest();
    return 0;
}

int fmt_icm_init(void)
{
    uint8_t hdr[FMT_ICM_HDR_END];
    uint8_t toc[FMT_ICM_TOC_HDR_BYTES];

    g_card_bytes = 0;
    g_toc_count = 0;

    /* The IPL sector is still sitting at card offset 0, and it is the
     * only description of the image's layout there is. Its "IPL4" magic
     * doubles as proof that the window is showing the card at all. */
    if (fmt_icm_read(0, sizeof hdr, hdr) != 0) {
        return -1;
    }
    if (hdr[0] != 'I' || hdr[1] != 'P' || hdr[2] != 'L' || hdr[3] != '4') {
        return -1;
    }
    /* A card built for a different window base would have handed the
     * payload a pile of const pointers into nothing; the IPL checks this
     * too, but a mismatch here means something even stranger happened. */
    if (rd32(hdr + FMT_ICM_HDR_CARD_BASE) != (uint32_t)FMT_ICM_CARD_BASE) {
        return -1;
    }

    g_card_bytes = rd32(hdr + FMT_ICM_HDR_CARD_BYTES);
    g_toc_off = rd32(hdr + FMT_ICM_HDR_TOC_OFF);
    if (!g_card_bytes || !g_toc_off) {
        return -1;
    }
    if (fmt_icm_read(g_toc_off, sizeof toc, toc) != 0 ||
        rd32(toc) != FMT_ICM_TOC_MAGIC) {
        g_card_bytes = 0;
        return -1;
    }
    g_toc_count = rd32(toc + 4);
    return 0;
}

static int chr_upper(int c)
{
    return (c >= 'a' && c <= 'z') ? (c - 'a' + 'A') : c;
}

/* Same matching rule as iso9660.c's, so a name that finds a file on the
 * CD build finds the same asset here: case-insensitive, and a trailing
 * ";<version>" on either side is ignored. */
static int name_matches(const char *want, const uint8_t *entry)
{
    int i;

    for (i = 0; i < FMT_ICM_TOC_NAME_LEN; i++) {
        if (entry[i] == '\0') {
            break;
        }
        if (want[i] == '\0' || want[i] == ';') {
            return 0;
        }
        if (chr_upper(want[i]) != chr_upper(entry[i])) {
            return 0;
        }
    }
    return (want[i] == '\0' || want[i] == ';');
}

int fmt_icm_find(const char *name, uint32_t *off, uint32_t *size)
{
    uint8_t entry[FMT_ICM_TOC_ENTRY_BYTES];
    uint32_t i;

    for (i = 0; i < g_toc_count; i++) {
        uint32_t at = g_toc_off + FMT_ICM_TOC_HDR_BYTES + i * FMT_ICM_TOC_ENTRY_BYTES;
        if (fmt_icm_read(at, sizeof entry, entry) != 0) {
            return -1;
        }
        if (name_matches(name, entry)) {
            *off = rd32(entry + FMT_ICM_TOC_NAME_LEN);
            *size = rd32(entry + FMT_ICM_TOC_NAME_LEN + 4);
            return 0;
        }
    }
    return -1;
}

int32_t fmt_icm_load(const char *name, void *buf, uint32_t bufsize)
{
    uint32_t off, size;

    if (fmt_icm_find(name, &off, &size) != 0 || size > bufsize) {
        return -1;
    }
    if (fmt_icm_read(off, size, buf) != 0) {
        return -1;
    }
    return (int32_t)size;
}

void fmt_icm_stream_init(fmt_icm_stream *st, uint32_t off, uint32_t size,
                         uint8_t *ring, uint32_t ring_size, int loop)
{
    st->ring = ring;
    st->ring_mask = ring_size - 1u;
    st->fill_pos = 0;
    st->off = off;
    st->size = size;
    st->pos = 0;
    st->loop = (uint8_t)(loop ? 1 : 0);
    st->state = (uint8_t)(size ? FMT_ICM_STREAM_RUN : FMT_ICM_STREAM_ERROR);
}

unsigned fmt_icm_stream_step(fmt_icm_stream *st, uint32_t play_pos,
                             unsigned budget)
{
    uint32_t ring_size = st->ring_mask + 1u;
    unsigned added = 0;

    while (budget-- && st->state == FMT_ICM_STREAM_RUN) {
        uint32_t used = st->fill_pos - play_pos;
        uint32_t free = ring_size - used;
        uint32_t n = FMT_ICM_STEP_BYTES;
        uint32_t to_end;

        if (used > ring_size) {          /* caller fell behind its own ring */
            break;
        }
        if (n > free) {
            n = free;
        }
        if (!n) {
            break;
        }
        /* One copy may not cross either the end of the asset or the end
         * of the ring's linear buffer. */
        to_end = st->size - st->pos;
        if (n > to_end) {
            n = to_end;
        }
        to_end = ring_size - (st->fill_pos & st->ring_mask);
        if (n > to_end) {
            n = to_end;
        }

        if (fmt_icm_read(st->off + st->pos, n,
                         st->ring + (st->fill_pos & st->ring_mask)) != 0) {
            st->state = FMT_ICM_STREAM_ERROR;
            break;
        }
        st->fill_pos += n;
        st->pos += n;
        added += n;

        if (st->pos >= st->size) {
            if (st->loop) {
                st->pos = 0;
            } else {
                st->state = FMT_ICM_STREAM_DONE;
            }
        }
    }
    return added;
}
