/* ─────────────────────────────────────────────────────────────────────────────
 * snes_audio_driver.c — track selection and the SPC snapshot stream schedule.
 * ───────────────────────────────────────────────────────────────────────────── */
#include "snes_audio.h"
#include "snes_audio_layout.h"

extern const u8 snes_audio_titlealt[];
extern const u8 snes_audio_overworld[];
extern const u8 snes_audio_altbattle[];
extern const u8 snes_audio_victory[];
extern const u8 snes_audio_fail[];

extern void snesAudioLoadSnapshot(const u8 *snapshot);
extern void snesAudioStreamBlock(const u8 *snapshot, u16 destination);
extern void snesAudioRestart(u8 finite);
extern void snesAudioPlaySfx(u8 slot);

static u8 current_track = 0xFF;

static const u8 *track_snapshot(u8 track)
{
    switch (track) {
    case SNES_AUDIO_OVERWORLD: return snes_audio_overworld;
    case SNES_AUDIO_ALTBATTLE: return snes_audio_altbattle;
    case SNES_AUDIO_VICTORY: return snes_audio_victory;
    case SNES_AUDIO_FAIL: return snes_audio_fail;
    default: return snes_audio_titlealt;
    }
}

void snesAudioInit(void)
{
    /* Boot uploads the resident code, the title music and the SFX bank
     * every image carries at the same address above its music. */
    snesAudioLoadSnapshot(snes_audio_titlealt);
    current_track = SNES_AUDIO_TITLEALT;
}

static u16 track_music_end(u8 track)
{
    switch (track) {
    case SNES_AUDIO_OVERWORLD: return SNES_AUDIO_OVERWORLD_END;
    case SNES_AUDIO_ALTBATTLE: return SNES_AUDIO_ALTBATTLE_END;
    case SNES_AUDIO_VICTORY: return SNES_AUDIO_VICTORY_END;
    case SNES_AUDIO_FAIL: return SNES_AUDIO_FAIL_END;
    default: return SNES_AUDIO_TITLEALT_END;
    }
}

void snesAudioPlay(u8 track)
{
    const u8 *snapshot;
    u16 destination;

    if (track >= SNES_AUDIO_TRACK_COUNT) track = SNES_AUDIO_TITLEALT;
    if (track == current_track) return;
    snapshot = track_snapshot(track);

    /* The converter's common player, its direct-page state and the SFX
     * region stay resident.  Stream only the new song's music region and
     * the DSP directory page that describes its samples. */
    for (destination = SNES_AUDIO_MUSIC_START;
         destination < track_music_end(track);
         destination += SNES_AUDIO_RELOAD_BYTES)
        snesAudioStreamBlock(snapshot + destination, destination);
    snesAudioStreamBlock(snapshot + 0xE700, 0xE700);
    snesAudioStreamBlock(snapshot + 0xE700 + SNES_AUDIO_RELOAD_BYTES,
                         0xE700 + SNES_AUDIO_RELOAD_BYTES);
    snesAudioRestart((u8)(track == SNES_AUDIO_VICTORY ||
                         track == SNES_AUDIO_FAIL));
    current_track = track;
}

void snesAudioSfx(u8 effect)
{
    if (effect >= SNES_SFX_COUNT) return;
    snesAudioPlaySfx(effect);
}
