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
extern void snesAudioRestart(void);
extern void snesAudioPlaySfx(u8 slot);

static u8 current_track = 0xFF;
static u8 sfx_loaded;

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
    /* This is the only full 64K upload.  The title profile intentionally has
     * no SFX payload, so it leaves the maximum ARAM room for its music. */
    snesAudioLoadSnapshot(snes_audio_titlealt);
    current_track = SNES_AUDIO_TITLEALT;
    sfx_loaded = 0;
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

    /* The converter's common player and direct-page state remain resident.
     * Stream only the new song's music region.  The generated non-title
     * snapshots share one fixed direct-SFX bank, so it is uploaded once on
     * the first transition away from the title and retained thereafter. */
    for (destination = SNES_AUDIO_MUSIC_START;
         destination < track_music_end(track);
         destination += 0x0100)
        snesAudioStreamBlock(snapshot + destination, destination);
    if (!sfx_loaded && track != SNES_AUDIO_TITLEALT) {
        for (destination = SNES_AUDIO_SFX_ADDRESS;
             destination < 0xE700; destination += 0x0100)
            snesAudioStreamBlock(snapshot + destination, destination);
        sfx_loaded = 1;
    }
    snesAudioStreamBlock(snapshot + 0xE700, 0xE700);
    snesAudioRestart();
    current_track = track;
}

void snesAudioSfx(u8 effect)
{
    /* The 64K title deliberately has no SFX directory entries. */
    if (current_track == SNES_AUDIO_TITLEALT) return;
    if (effect >= SNES_SFX_COUNT) return;
    snesAudioPlaySfx(effect);
}
