#include "game_api.h"
#include "assets.h"
#include "waifu_cd32x_audio.h"
#include "waifu_cd32x_cdrom.h"
#include "waifu_cd32x_input.h"
#include "waifu_cd32x_memory.h"
#include "waifu_cd32x_video.h"

enum {
    CD32X_SLAVE_JOB_IDLE = 0,
    CD32X_SLAVE_JOB_FILL_U8 = 1
};

typedef struct Cd32xSlaveJob {
    volatile uint32_t command;
    volatile uint32_t dst;
    volatile uint32_t count;
    volatile uint32_t color;
} Cd32xSlaveJob;

static Cd32xSlaveJob g_cd32x_slave_job __attribute__((aligned(16)));

static volatile Cd32xSlaveJob *cd32x_slave_job_uncached(void)
{
    uintptr_t addr = (uintptr_t)&g_cd32x_slave_job;
    if (addr >= WAIFU_CD32X_SDRAM_CACHED_BASE && addr < WAIFU_CD32X_SDRAM_CACHED_LIMIT) {
        addr = (addr - WAIFU_CD32X_SDRAM_CACHED_BASE) + WAIFU_CD32X_SDRAM_UNCACHED_BASE;
    }
    return (volatile Cd32xSlaveJob *)addr;
}

static void cd32x_fill_u8_local(uint8_t *dst, uint32_t count, uint8_t color)
{
    uint16_t pair = (uint16_t)(((uint16_t)color << 8) | color);
    volatile uint8_t *d8 = (volatile uint8_t *)dst;

    if (((uintptr_t)d8 & 1u) && count > 0u) {
        *d8++ = color;
        --count;
    }

    volatile uint16_t *d16 = (volatile uint16_t *)(volatile void *)d8;
    while (count >= 8u) {
        d16[0] = pair;
        d16[1] = pair;
        d16[2] = pair;
        d16[3] = pair;
        d16 += 4;
        count -= 8u;
    }
    while (count >= 2u) {
        *d16++ = pair;
        count -= 2u;
    }

    d8 = (volatile uint8_t *)(volatile void *)d16;
    if (count) *d8 = color;
}

void waifu_cd32x_fill_u8_parallel(uint8_t *dst, int count, uint8_t color)
{
    volatile Cd32xSlaveJob *job = cd32x_slave_job_uncached();
    uint32_t total;
    uint32_t first;

    if (count <= 0) return;
    total = (uint32_t)count;
    if (total < 4096u) {
        cd32x_fill_u8_local(dst, total, color);
        return;
    }

    while (job->command != CD32X_SLAVE_JOB_IDLE) {
    }

    first = (total >> 1) & ~1u;
    if (first == 0u || first >= total) {
        cd32x_fill_u8_local(dst, total, color);
        return;
    }

    job->dst = (uint32_t)(uintptr_t)(dst + first);
    job->count = total - first;
    job->color = color;
    job->command = CD32X_SLAVE_JOB_FILL_U8;

    cd32x_fill_u8_local(dst, first, color);

    while (job->command != CD32X_SLAVE_JOB_IDLE) {
    }
}

int main(void)
{
    WaifuCd32xCdrom *cdrom = waifu_cd32x_cdrom_create();
    WaifuCd32xVideo *video = waifu_cd32x_video_create();
    WaifuCd32xInput *input = waifu_cd32x_input_create();
    WaifuCd32xAudio *audio = waifu_cd32x_audio_create();
    WaifuFmMusicTrack last_music = WAIFU_FM_MUSIC_NONE;

    (void)cdrom;
    waifu_fm_init();
    waifu_fm_reset_interactive();

    for (;;) {
        WaifuFmInput in;
        WaifuFmMusicTrack music;

        waifu_cd32x_input_poll(input, &in);
        waifu_fm_step(&in);

        waifu_cd32x_video_present_8bpp(video, waifu_fm_framebuffer(), waifu_fm_palette_rgb(), waifu_fm_palette_id(), waifu_fm_video_fade_q8());
        waifu_cd32x_video_wait_vblank(video);

        music = waifu_fm_audio_music_track();
        if (music == WAIFU_FM_MUSIC_TITLE &&
            (!waifu_assets_title_ready() ||
             waifu_fm_palette_id() != WAIFU_FM_PALETTE_TITLE ||
             waifu_fm_video_fade_q8() < 256)) {
            /* Match the PC-FX CD-DA gate: do not start title music while the
               title image is still loading, while the direct 32X title page is
               not active, or while palette fade-in is still underway.  This
               avoids repeatedly interrupting the drive under the black/loading
               frames.  If title CD-DA is already active, keep it alive during
               title/menu fade-out instead of issuing a stop command. */
            if (last_music != WAIFU_FM_MUSIC_TITLE) music = WAIFU_FM_MUSIC_NONE;
        }
        if (music != last_music) {
            waifu_cd32x_audio_set_music(audio, music);
            last_music = music;
        }
        waifu_cd32x_audio_pump(audio);
    }

    waifu_cd32x_audio_destroy(audio);
    waifu_cd32x_input_destroy(input);
    waifu_cd32x_video_destroy(video);
    waifu_cd32x_cdrom_destroy(cdrom);
    return 0;
}

/* Renderer3D direct-framebuffer compatibility hooks.  CD32X presents the common
   CPU framebuffer through the video backend after rendering. */
int cfx_pcfx_current_kram_page_word_offset = 0;
uint8_t *cfx_game_framebuffer(void)
{
    return waifu_fm_framebuffer();
}

void slave(void)
{
    volatile Cd32xSlaveJob *job = cd32x_slave_job_uncached();
    for (;;) {
        if (job->command == CD32X_SLAVE_JOB_FILL_U8) {
            cd32x_fill_u8_local((uint8_t *)(uintptr_t)job->dst, job->count, (uint8_t)job->color);
            job->command = CD32X_SLAVE_JOB_IDLE;
        } else {
            __asm__ volatile ("nop");
        }
    }
}
