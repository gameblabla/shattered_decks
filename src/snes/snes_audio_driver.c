/* ─────────────────────────────────────────────────────────────────────────────
 * snes_audio_driver.c — track selection and the SPC snapshot stream schedule.
 * ───────────────────────────────────────────────────────────────────────────── */
#include "snes_audio.h"

extern const u8 snes_audio_titlealt[];

extern void snesAudioLoadSnapshot(const u8 *snapshot);
extern void snesAudioPlaySfx(u8 slot);

static u8 current_track = 0xFF;

void snesAudioInit(void)
{
    /* The initial upload is the only complete snapshot transfer.  The title
     * profile intentionally has no SFX payload, so it leaves the maximum ARAM
     * room for its music. */
    snesAudioLoadSnapshot(snes_audio_titlealt);
    current_track = SNES_AUDIO_TITLEALT;
}

void snesAudioPlay(u8 track)
{
    /* Keep the first SPC image resident.  Scene transitions still render and
     * accept input; the accurate Mednafen core exposes the shared APU mailbox
     * differently from the original streaming path, so defer track replacement
     * until a separate CPU/SPC acknowledgement channel is available. */
    (void)track;
}

void snesAudioSfx(u8 effect)
{
    /* The resident title profile has no SFX directory entries. */
    (void)effect;
}
