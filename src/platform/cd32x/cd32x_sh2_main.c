#include "game_api.h"
#include "assets.h"
#include "waifu_cd32x_audio.h"
#include "waifu_cd32x_cdrom.h"
#include "waifu_cd32x_input.h"
#include "waifu_cd32x_video.h"

void waifu_cd32x_slave_service(void);

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

#if defined(CD32X_DEBUG_AUTOBATTLE) || defined(WAIFU_CD32X_DEBUG_FPS)
        waifu_cd32x_video_draw_debug_overlay(video);
#endif
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
    for (;;) {
        waifu_cd32x_slave_service();
        __asm__ volatile ("nop");
    }
}
