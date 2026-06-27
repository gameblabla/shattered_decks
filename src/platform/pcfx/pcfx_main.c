#include "game_api.h"
#include "assets.h"
#include "waifu_pcfx_audio.h"
#include "waifu_pcfx_cdrom.h"
#include "waifu_pcfx_input.h"
#include "waifu_pcfx_video.h"
#include "pcfx.h"

int main(void)
{
    WaifuPcfxCdrom *cdrom = waifu_pcfx_cdrom_create();
    WaifuPcfxVideo *video = waifu_pcfx_video_create();
    WaifuPcfxInput *input = waifu_pcfx_input_create();
    WaifuPcfxAudio *audio = waifu_pcfx_audio_create();
    WaifuFmMusicTrack last_music = WAIFU_FM_MUSIC_NONE;

    (void)cdrom;
    waifu_fm_init();
    waifu_fm_reset_interactive();

    for (;;) {
        WaifuFmInput in;
        waifu_pcfx_input_poll(input, &in);
        waifu_fm_step(&in);

        waifu_pcfx_video_present_8bpp(video, waifu_fm_framebuffer(), waifu_fm_palette_rgb(), waifu_fm_palette_id());
        waifu_pcfx_video_wait_vblank(video);

        WaifuFmMusicTrack music = waifu_fm_audio_music_track();
        if (music == WAIFU_FM_MUSIC_TITLE &&
            (!waifu_assets_title_ready() ||
             waifu_fm_palette_id() != WAIFU_FM_PALETTE_TITLE ||
             waifu_fm_video_fade_q8() < 256)) {
            /* Returning to the title evicts battle/story assets and reloads title
               data from CD.  Keep CD-DA silent until the title is loaded,
               presented through the title path, and fully faded in so the drive
               cannot start a short burst under loading/black frames. */
            music = WAIFU_FM_MUSIC_NONE;
        }
        if (music != last_music) {
            waifu_pcfx_audio_set_music(audio, music);
            last_music = music;
        }
        waifu_pcfx_audio_pump(audio);
    }

    /* Not reached on console. */
    waifu_pcfx_cdrom_destroy(cdrom);
    waifu_pcfx_audio_destroy(audio);
    waifu_pcfx_input_destroy(input);
    waifu_pcfx_video_destroy(video);
    return 0;
}

/* Renderer3D PC-FX direct-KRAM compatibility hooks.  The first PC-FX port
   presents a complete 8bpp page through the video module after rendering, but
   renderer3d.c still references these hooks when built with PC-FX flags. */
int cfx_pcfx_current_kram_page_word_offset = 0;
uint8_t *cfx_game_framebuffer(void)
{
    return waifu_fm_framebuffer();
}
