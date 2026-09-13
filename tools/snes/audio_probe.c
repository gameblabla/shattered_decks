/* Test cartridge for the real SNES/SPC transport.  A advances through every
 * track, including a return to title; B plays the current SFX slot through
 * the resident 2-bit streamer and steps to the next, RIGHT steps without
 * playing and SELECT returns to slot 0.  tools/snes/verify_audio.py drives
 * and checks it. */
#include "snes_audio.h"

static const u8 tracks[] = {0, 2, 1, 3, 4, 0, 2, 1};
volatile u16 audio_probe_stamp[4];

int main(void)
{
    u8 phase = 0;
    u8 slot = 0;
    u16 previous = 0;
    u16 keys;
    consoleInit();
    snesAudioInit();
    audio_probe_stamp[0] = 0x4155;
    audio_probe_stamp[1] = 0;
    audio_probe_stamp[2] = 0;
    audio_probe_stamp[3] = 0;
    setScreenOn();
    while (1) {
        WaitForVBlank();
        keys = padsCurrent(0);
        if ((keys & KEY_A) && !(previous & KEY_A) && phase < 7) {
            ++phase;
            /* Invalid IDs must fall back to title, and a repeated request
             * must leave playback running rather than start another upload. */
            snesAudioPlay(phase == 5 ? 0xFF : tracks[phase]);
            snesAudioPlay(tracks[phase]);
            snesAudioSfx(0xFF);
            audio_probe_stamp[1] = phase;
            audio_probe_stamp[2] = 0;
            audio_probe_stamp[3] = tracks[phase];
        }
        if ((keys & KEY_RIGHT) && !(previous & KEY_RIGHT))
            slot = (u8)((slot + 1) % SNES_SFX_COUNT);
        if ((keys & KEY_SELECT) && !(previous & KEY_SELECT)) slot = 0;
        if ((keys & KEY_B) && !(previous & KEY_B)) {
            snesAudioSfx(slot);
            slot = (u8)((slot + 1) % SNES_SFX_COUNT);
        }
        previous = keys;
        ++audio_probe_stamp[2];
        setPaletteColor(0, RGB5(phase * 4, 8, 16));
    }
}
