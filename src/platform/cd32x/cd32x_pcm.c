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
#define CD32X_SFX_LOOP_SILENCE_BYTES WAIFU_CD32X_SFX_PCM_LOOP_SILENCE_BYTES

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
    pcm_set_loop((uint16_t)(((uint16_t)m->start_block << BLK_SHIFT) +
                            m->length -
                            (m->length > CD32X_SFX_LOOP_SILENCE_BYTES ? CD32X_SFX_LOOP_SILENCE_BYTES : 0u)));
    pcm_set_freq(WAIFU_CD32X_SFX_PCM_FREQ_DELTA);
    pcm_set_env(m->volume);
    pcm_set_pan(128u);
    pcm_set_on(ch);
    return 0;
}

/* ---- RF5C164 streamed music ------------------------------------------------
   SFX occupies the low generated block range of wave RAM; music uses the
   remaining high RAM on channel 4 with a 0xFF loop-end marker at 0xFF00 so the
   chip wraps the ring continuously.

   Streaming is CLOSED-LOOP: the RF5C164 exposes the live per-channel playback
   address through its read registers (0x10-0x1F; see BlastEm rf5c164_read), so
   each pump reads channel 4's play head out of wave RAM and refills the ring up
   to a near-full target ahead of it.  This is immune to the open-loop failure
   that silenced music during card loads: a blocking BIOS CD read starves the
   Sub-CPU INT2 so GET_TICKS freezes while the chip keeps draining the ring, and
   the old tick-paced refill then wrote almost nothing and underran.  Reading the
   actual play head removes any dependence on GET_TICKS and on the assumed
   sample rate, and keeps the ring maximally buffered (~2.4s) right up to each
   CD read so a single read never drains it.  When a source chunk has been fully
   queued into the ring, cd32x_music_needs_chunk() asks the supervisor to load
   the next CD chunk into the PRG-RAM source buffer. */
#define CD32X_MUSIC_CHANNEL    4u
#define CD32X_MUSIC_RING_BLOCK WAIFU_CD32X_SFX_PCM_USED_BLOCKS
#define CD32X_MUSIC_RING_BASE  (CD32X_MUSIC_RING_BLOCK << 8)
#define CD32X_MUSIC_MARKER_OFF 0xFF00u
#define CD32X_MUSIC_RING_BYTES (CD32X_MUSIC_MARKER_OFF - CD32X_MUSIC_RING_BASE)
#define CD32X_MUSIC_VOLUME     0xC0u
#define CD32X_MUSIC_LEAD_GUARD 1024u

/* RF5C164 channel-4 playback-address read registers.  Register N sits at
   0xFF0001 + 2*N; the address registers for channel C are 0x10|(C<<1) (low byte
   of the wave-RAM byte address, cur_ptr>>11) and 0x11|(C<<1) (high byte,
   cur_ptr>>19).  Channel 4 -> 0x18 / 0x19 -> 0xFF0031 / 0xFF0033. */
#define PCM_PLAY_LO *((volatile uint8_t *)0xFF0031)
#define PCM_PLAY_HI *((volatile uint8_t *)0xFF0033)

static int8_t  g_music_clip[WAIFU_CD32X_MUSIC_PCM_MAX_CHUNK_BYTES];
static uint32_t g_music_clip_len;
static uint32_t g_music_clip_pos;
static uint16_t g_music_write_off;
static uint8_t  g_music_playing;
static uint8_t  g_music_need_chunk;

int8_t *cd32x_music_clip_buffer(void) { return g_music_clip; }
uint32_t cd32x_music_clip_capacity(void) { return (uint32_t)sizeof(g_music_clip); }

/* Live channel-4 play head as a ring-relative byte offset in [0, RING_BYTES).
   Returns 0 before the channel has started (cur_ptr still below the ring).

   The RF5C164 only exposes the channel address read registers while the control
   register is in register/channel-select mode (D6=1) with the wanted channel
   selected; in wave-RAM access mode (D6=0, left by the previous pcm_cpy) the
   reads return RAM/garbage on real hardware.  BlastEm decodes the channel from
   the address alone and ignores this, which is why the old read worked in the
   emulator but the stream silently stalled after one ring (~2.6s) on hardware.
   Select channel 4 first, then read 0x18/0x19. */
static uint16_t music_play_off(void)
{
    uint16_t addr;
    PCM_CTRL = (uint8_t)(0xC0u | CD32X_MUSIC_CHANNEL);
    pcm_delay();
    addr = (uint16_t)(((uint16_t)PCM_PLAY_HI << 8) | (uint16_t)PCM_PLAY_LO);
    if (addr < (uint16_t)CD32X_MUSIC_RING_BASE) return 0u;
    addr = (uint16_t)(addr - (uint16_t)CD32X_MUSIC_RING_BASE);
    if (addr >= (uint16_t)CD32X_MUSIC_RING_BYTES) addr = (uint16_t)(CD32X_MUSIC_RING_BYTES - 1u);
    return addr;
}

static void music_write_ring(uint32_t n)
{
    uint16_t off = g_music_write_off;
    if (g_music_clip_len == 0 || g_music_clip_pos >= g_music_clip_len) {
        g_music_need_chunk = g_music_playing != 0u;
        return;
    }
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
        n -= chunk;
        if (g_music_clip_pos >= g_music_clip_len) {
            g_music_need_chunk = 1u;
            break;
        }
    }
    g_music_write_off = off;
}

void cd32x_music_start(uint32_t chunk_bytes)
{
    static const uint8_t marker = 0xFFu;
    uint32_t lead;
    if (chunk_bytes < 2u) return;
    if (chunk_bytes > (uint32_t)sizeof(g_music_clip)) chunk_bytes = (uint32_t)sizeof(g_music_clip);

    cd32x_music_stop();

    g_music_clip_len = chunk_bytes;
    g_music_clip_pos = 0u;
    g_music_need_chunk = 0u;
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

    /* The ring is already fully primed with clip[0..RING_BYTES).  Position the
       write head a near-full target ahead of the play head (which starts at the
       ring base) so the closed-loop pump sees the ring as full and only refills
       as the chip drains it. */
    lead = CD32X_MUSIC_RING_BYTES > CD32X_MUSIC_LEAD_GUARD
        ? CD32X_MUSIC_RING_BYTES - CD32X_MUSIC_LEAD_GUARD
        : CD32X_MUSIC_RING_BYTES / 2u;
    if (lead >= g_music_clip_len) lead = g_music_clip_len - 1u;
    g_music_write_off = (uint16_t)lead;
    g_music_clip_pos = lead;
    g_music_playing = 1u;
    pcm_set_on(CD32X_MUSIC_CHANNEL);
}

void cd32x_music_supply_chunk(uint32_t chunk_bytes)
{
    if (!g_music_playing) return;
    if (chunk_bytes < 1u) return;
    if (chunk_bytes > (uint32_t)sizeof(g_music_clip)) chunk_bytes = (uint32_t)sizeof(g_music_clip);
    g_music_clip_len = chunk_bytes;
    g_music_clip_pos = 0u;
    g_music_need_chunk = 0u;
}

int cd32x_music_needs_chunk(void)
{
    return g_music_playing && g_music_need_chunk;
}

void cd32x_music_stop(void)
{
    if (!g_music_playing) return;
    pcm_set_off(CD32X_MUSIC_CHANNEL);
    g_music_playing = 0u;
    g_music_clip_len = 0u;
    g_music_need_chunk = 0u;
}

void cd32x_music_pump(void)
{
    uint16_t play, write, fill, target;
    if (!g_music_playing) return;
    if (g_music_clip_len == 0u || g_music_clip_pos >= g_music_clip_len) {
        g_music_need_chunk = 1u;
        return;
    }

    /* Refill the ring up to TARGET valid bytes ahead of the live play head.
       fill = (write - play) mod RING_BYTES is the amount of queued, not-yet
       played audio; top it back up to the near-full target each pump. */
    play = music_play_off();
    write = g_music_write_off;
    fill = (write >= play)
        ? (uint16_t)(write - play)
        : (uint16_t)(CD32X_MUSIC_RING_BYTES - (uint16_t)(play - write));
    target = (uint16_t)(CD32X_MUSIC_RING_BYTES - CD32X_MUSIC_LEAD_GUARD);
    if (fill >= target) return;
    music_write_ring((uint32_t)(target - fill));
}
