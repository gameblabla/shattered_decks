#include "game_api.h"
#include "assets.h"
#include "waifu_pcfx_audio.h"
#include "waifu_pcfx_cdrom.h"
#include "waifu_pcfx_input.h"
#include "waifu_pcfx_video.h"
#include "pcfx.h"
#include <pcfx/v810.h>

int main(void)
{
    /* Real-hardware bring-up: run with NO interrupts at all.  On real PC-FX
       hardware any armed IRQ (the BIOS vsync handler, an interval timer, the
       KING VD-status vblank IRQ) can starve the single highest-priority source
       the controller delivers, stall the render loop and leave the KING BG
       layer BLACK -- while pcfxemu tolerates it and looks fine.  Masking every
       source at the controller AND disabling at the CPU (PSW) means no ISR ever
       runs, so nothing corrupts a KING register write or the VDC control
       register.  Field timing is done by polling the TETSU raster in
       waifu_pcfx_video_wait_vblank instead (matches the maka/doom-pcfx hardware
       path). */
    irq_set_mask(0x7F);   /* mask every maskable source at the controller */
    irq_disable();        /* and disable at the CPU (PSW) -- no ISR ever runs */

    /* KING REG.61 = 4-Mbit KRAM, before any other KING access (C6272_1 2.1 step
       3, and doom-pcfx/wolf-pcfx keep it first for the same reason).  The retail
       BIOS hands off in 1-Mbit mode, in which most of this port's KRAM map --
       affine pages 1 and 2, the CD DMA bounce window, and the RAINBOW/ADPCM page
       1 -- does not exist.  That is the real-hardware black boot; the full
       derivation is on waifu_pcfx_video_init_kram_mode(). */
    waifu_pcfx_video_init_kram_mode();

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
               cannot start a short burst under loading/black frames.

               If title CD-DA is already playing, keep it alive during title/menu
               fade-out.  Stopping CD-DA issues SCSI commands, and doing that
               during the visible fade makes Battle Mode selection visibly stall
               before loading has even started. */
            if (last_music != WAIFU_FM_MUSIC_TITLE) music = WAIFU_FM_MUSIC_NONE;
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
