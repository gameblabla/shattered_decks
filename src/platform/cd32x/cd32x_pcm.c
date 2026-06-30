/* Sega CD PCM service for the CD32X resident supervisor.
 *
 * The low-level sample loader/register helpers follow RaycastDemo's pcm.c
 * model: samples are copied to PCM wave RAM as sign/magnitude bytes and the
 * channel start/loop registers point at 256-byte blocks.  The game-specific
 * layer below preloads a compact 11.025 kHz SFX bank and plays one-shots on
 * four PCM channels under SH-2 command control. */
#include <stdint.h>
#include "cd32x_pcm.h"
#include "cd32x_sfx_pcm.h"
#include "cd32x_music_pcm.h"
#include "cd32x_md_iface.h"

#define PCM_ENV   *((volatile uint8_t *)0xFF0001)
#define PCM_PAN   *((volatile uint8_t *)0xFF0003)
#define PCM_FDL   *((volatile uint8_t *)0xFF0005)
#define PCM_FDH   *((volatile uint8_t *)0xFF0007)
#define PCM_LSL   *((volatile uint8_t *)0xFF0009)
#define PCM_LSH   *((volatile uint8_t *)0xFF000B)
#define PCM_START *((volatile uint8_t *)0xFF000D)
#define PCM_CTRL  *((volatile uint8_t *)0xFF000F)
#define PCM_ONOFF *((volatile uint8_t *)0xFF0011)
#define PCM_WAVE  *((volatile uint8_t *)0xFF2001)

#define BLK_PAD   (32u + 255u)
#define BLK_SHIFT 8u
#define CD32X_PCM_SFX_CHANNELS 4u

static const uint8_t loop_markers[32] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

static uint8_t ChanOff = 0xFF;
static uint8_t g_cd32x_pcm_ready;
static uint8_t g_cd32x_pcm_next_channel;

void pcm_delay(void)
{
    volatile uint8_t *ctrl = (volatile uint8_t *)0xFF000F;
    (void)*ctrl;
    __asm__ volatile ("nop\n\tnop\n\tnop\n\tnop" ::: "memory");
}

uint8_t pcm_lcf(uint8_t pan)
{
    /* Raycaster's helper applies a constant-power-ish pan curve.  CD32X SFX
       are mono UI/game one-shots, so keep centre as full L/R and retain sane
       edge behaviour for future callers. */
    if (pan <= 8u) return 0x0F;       /* left only on this register layout */
    if (pan >= 247u) return 0xF0;     /* right only */
    return 0xFF;                      /* centre/full stereo */
}

void pcm_set_period(uint32_t period)
{
    (void)period;
    pcm_set_freq(WAIFU_CD32X_SFX_PCM_FREQ_DELTA);
}

void pcm_set_freq(uint32_t freq_delta)
{
    uint16_t f = (uint16_t)(freq_delta & 0x0FFFu);
    PCM_FDL = (uint8_t)(f & 0xFFu);
    pcm_delay();
    PCM_FDH = (uint8_t)((f >> 8) & 0x0Fu);
    pcm_delay();
}

void pcm_set_timer(uint16_t bpm) { (void)bpm; }
void pcm_stop_timer(void) { }
void pcm_start_timer(void (*callback)(void)) { (void)callback; }

static void pcm_cpy(uint16_t doff, const void *src, uint16_t len, uint16_t conv)
{
    const uint8_t *sptr = (const uint8_t *)src;

    while (len > 0) {
        uint8_t *wptr = (uint8_t *)&PCM_WAVE;
        uint16_t woff = doff & 0x0FFFu;
        uint16_t wblen = (uint16_t)(0x1000u - woff);
        wptr += (woff << 1);

        PCM_CTRL = (uint8_t)(0x80u + (doff >> 12));
        pcm_delay();

        if (wblen > len) wblen = len;
        doff = (uint16_t)(doff + wblen);
        len = (uint16_t)(len - wblen);
        while (wblen > 0) {
            int16_t s = (int8_t)*sptr++;
            if (conv) {
                if (s < 0) {
                    s = (int16_t)-s;
                    if (s > 127) s = 127;
                } else {
                    if (s > 126) s = 126;
                    s |= 128;
                }
            }
            *wptr++ = (uint8_t)(s & 255);
            ++wptr;
            --wblen;
        }
    }
}

void pcm_load_samples(uint8_t start, const int8_t *samples, uint16_t length)
{
    PCM_ONOFF = 0xFF;
    ChanOff = 0xFF;
    pcm_cpy((uint16_t)start << BLK_SHIFT, samples, length, 1);
    pcm_cpy((uint16_t)(((uint16_t)start << BLK_SHIFT) + length), loop_markers, sizeof(loop_markers), 0);
}

uint16_t pcm_next_block(uint8_t start, uint16_t length)
{
    return (uint16_t)(start + ((length + BLK_PAD) >> BLK_SHIFT));
}

void pcm_reset(void)
{
    uint16_t i;

    ChanOff = 0xFF;
    PCM_ONOFF = 0xFF;
    pcm_delay();

    for (i = 0; i < 8; ++i) {
        PCM_CTRL = (uint8_t)(0xC0u + i);
        pcm_delay();
        PCM_ENV = 0x00;
        pcm_delay();
        PCM_PAN = 0x00;
        pcm_delay();
        PCM_FDL = 0x00;
        pcm_delay();
        PCM_FDH = 0x00;
        pcm_delay();
        PCM_LSL = 0x00;
        pcm_delay();
        PCM_LSH = 0x00;
        pcm_delay();
        PCM_START = 0x00;
        pcm_delay();
    }
}

void pcm_set_ctrl(uint8_t val)
{
    PCM_CTRL = val;
    pcm_delay();
}

void pcm_set_off(uint8_t index)
{
    ChanOff |= (uint8_t)(1u << index);
    PCM_ONOFF = ChanOff;
    pcm_delay();
}

void pcm_set_on(uint8_t index)
{
    ChanOff &= (uint8_t)~(1u << index);
    PCM_ONOFF = ChanOff;
    pcm_delay();
}

void pcm_set_start(uint8_t start, uint16_t offset)
{
    PCM_START = (uint8_t)(start + (offset >> BLK_SHIFT));
    pcm_delay();
}

void pcm_set_loop(uint16_t loopstart)
{
    PCM_LSL = (uint8_t)(loopstart & 0x00FFu);
    pcm_delay();
    PCM_LSH = (uint8_t)((loopstart >> 8) & 0x00FFu);
    pcm_delay();
}

void pcm_set_env(uint8_t vol)
{
    PCM_ENV = vol;
    pcm_delay();
}

void pcm_set_pan(uint8_t pan)
{
    PCM_PAN = pcm_lcf(pan);
    pcm_delay();
}

void cd32x_pcm_sfx_init_from_bank(const int8_t *bank, uint32_t bank_bytes)
{
    unsigned i;
    if (g_cd32x_pcm_ready) return;
    if (!bank || bank_bytes < WAIFU_CD32X_SFX_PCM_BANK_BYTES) return;

    pcm_reset();
    for (i = 0; i < WAIFU_CD32X_SFX_PCM_META_COUNT; ++i) {
        const WaifuCd32xSfxPcmMeta *m = &waifu_cd32x_sfx_pcm_meta[i];
        if (m->length) {
            pcm_load_samples(m->start_block, bank + m->offset, m->length);
        }
    }

    g_cd32x_pcm_next_channel = 0;
    g_cd32x_pcm_ready = 1;
}

int cd32x_pcm_sfx_ready(void)
{
    return g_cd32x_pcm_ready != 0;
}

int cd32x_pcm_sfx_play(int effect)
{
    uint8_t ch;
    const WaifuCd32xSfxPcmMeta *m;
    if (!g_cd32x_pcm_ready) return -1;
    if (effect < 0 || effect >= (int)WAIFU_CD32X_SFX_PCM_META_COUNT) return -1;
    m = &waifu_cd32x_sfx_pcm_meta[effect];
    if (!m->length) return -1;

    ch = g_cd32x_pcm_next_channel;
    g_cd32x_pcm_next_channel = (uint8_t)((g_cd32x_pcm_next_channel + 1u) % CD32X_PCM_SFX_CHANNELS);

    pcm_set_ctrl((uint8_t)(0xC0u + ch));
    pcm_set_off(ch);
    pcm_set_start(m->start_block, 0);
    pcm_set_loop((uint16_t)(((uint16_t)m->start_block << BLK_SHIFT) + m->length));
    pcm_set_freq(WAIFU_CD32X_SFX_PCM_FREQ_DELTA);
    pcm_set_env(m->volume);
    pcm_set_pan(128u);
    pcm_set_on(ch);
    return 0;
}

/* ---- RF5C164 streamed music ------------------------------------------------
   SFX occupies the low ~47 KiB of wave RAM; music uses a 16128-byte ring above
   it (byte 0xC000 = block 192) on channel 4, with a 0xFF loop-end marker at
   0xFF00 so the chip wraps the ring continuously.  Each vblank tick the
   supervisor copies the next clip bytes into the ring ahead of the play head
   (open-loop: play and write both advance ~RATE/60 bytes/tick).  The clip is
   already RF5C164 sign/magnitude bytes in PRG RAM, so the refill is a raw copy.
   Writing wave RAM briefly suspends RF5C164 output, so refills are kept to one
   small burst per vblank. */
#define CD32X_MUSIC_CHANNEL    4u
#define CD32X_MUSIC_RING_BASE  0xC000u
#define CD32X_MUSIC_RING_BYTES 0x3F00u                                  /* 16128 */
#define CD32X_MUSIC_RING_BLOCK (CD32X_MUSIC_RING_BASE >> 8)             /* 192 */
#define CD32X_MUSIC_MARKER_OFF (CD32X_MUSIC_RING_BASE + CD32X_MUSIC_RING_BYTES)
#define CD32X_MUSIC_TICK_BYTES 184u                                     /* 11025/60 */
#define CD32X_MUSIC_VOLUME     0xC0u

static int8_t  g_music_clip[WAIFU_CD32X_MUSIC_PCM_MAX_BYTES];
static uint32_t g_music_clip_len;
static uint32_t g_music_clip_pos;
static uint16_t g_music_write_off;
static uint32_t g_music_last_tick;
static uint8_t  g_music_playing;

int8_t *cd32x_music_clip_buffer(void) { return g_music_clip; }
uint32_t cd32x_music_clip_capacity(void) { return (uint32_t)sizeof(g_music_clip); }

static void music_write_ring(uint32_t n)
{
    uint16_t off = g_music_write_off;
    if (g_music_clip_len == 0) return;
    if (n > CD32X_MUSIC_RING_BYTES) n = CD32X_MUSIC_RING_BYTES;
    while (n > 0u) {
        uint32_t ring_room = (uint32_t)(CD32X_MUSIC_RING_BYTES - off);
        uint32_t clip_room = g_music_clip_len - g_music_clip_pos;
        uint32_t chunk = n;
        if (chunk > ring_room) chunk = ring_room;
        if (chunk > clip_room) chunk = clip_room;
        pcm_cpy((uint16_t)(CD32X_MUSIC_RING_BASE + off),
                g_music_clip + g_music_clip_pos, (uint16_t)chunk, 0u);
        off = (uint16_t)(off + chunk);
        if (off >= CD32X_MUSIC_RING_BYTES) off = 0u;
        g_music_clip_pos += chunk;
        if (g_music_clip_pos >= g_music_clip_len) g_music_clip_pos = 0u;
        n -= chunk;
    }
    g_music_write_off = off;
}

void cd32x_music_start(uint32_t clip_bytes)
{
    static const uint8_t marker = 0xFFu;
    if (clip_bytes < 2u) return;

    cd32x_music_stop();

    g_music_clip_len = clip_bytes - 1u;     /* drop the file's trailing marker */
    g_music_clip_pos = 0u;
    g_music_write_off = 0u;
    music_write_ring(CD32X_MUSIC_RING_BYTES);                 /* prime whole ring */
    pcm_cpy(CD32X_MUSIC_MARKER_OFF, &marker, 1u, 0u);         /* loop-end marker */

    pcm_set_ctrl((uint8_t)(0xC0u + CD32X_MUSIC_CHANNEL));
    pcm_set_off(CD32X_MUSIC_CHANNEL);
    pcm_set_start(CD32X_MUSIC_RING_BLOCK, 0u);
    pcm_set_loop(CD32X_MUSIC_RING_BASE);
    pcm_set_freq(WAIFU_CD32X_MUSIC_PCM_FREQ_DELTA);
    pcm_set_env(CD32X_MUSIC_VOLUME);
    pcm_set_pan(128u);

    /* Lead the play head (which starts at ring base) by half the ring so a card
       read that stalls the refill for a few frames does not underrun.  The ring
       is already fully primed, so the next writes begin halfway through it and
       replay the same source offset until the writer wraps to fresh samples. */
    g_music_write_off = CD32X_MUSIC_RING_BYTES / 2u;
    g_music_clip_pos = CD32X_MUSIC_RING_BYTES / 2u;
    g_music_last_tick = GET_TICKS;
    g_music_playing = 1u;
    pcm_set_on(CD32X_MUSIC_CHANNEL);
}

void cd32x_music_stop(void)
{
    if (!g_music_playing) return;
    pcm_set_off(CD32X_MUSIC_CHANNEL);
    g_music_playing = 0u;
    g_music_clip_len = 0u;
}

void cd32x_music_pump(void)
{
    uint32_t now, delta;
    if (!g_music_playing) return;
    now = GET_TICKS;
    delta = now - g_music_last_tick;
    if (delta == 0u) return;
    g_music_last_tick = now;
    music_write_ring(delta * CD32X_MUSIC_TICK_BYTES);
}
