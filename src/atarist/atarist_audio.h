/* ─────────────────────────────────────────────────────────────────────────────
 *  atarist_audio.h — YM2149 music.
 *
 *  The ST's sound chip is the same AY-3-8910 family the MSX PSG belongs to, so
 *  the soundtrack comes from the very same VGM files the MSX2 port uses
 *  (msx_music, the AY-3-8910 VGM set).  tools/atarist/gen_atarist_audio.py flattens each one
 *  into a 50 Hz register-delta stream and retunes the period registers from the
 *  MSX clock (1.7897 MHz) to the ST's (2.0 MHz), which is the whole reason the
 *  conversion is offline: doing it live would cost a divide per note.
 *
 *  Stream format (little detail, because the converter is its only author):
 *
 *      'YMS1'      u32   magic
 *      frames      u16   total frames
 *      flags       u16   bit 0 = loops
 *      loop_off    u32   byte offset of the loop frame within `data`
 *      data_size   u32
 *      data        [ u8 count, count * (u8 reg, u8 value) ] per frame
 *
 *  The player runs inside the VBL handler so music keeps its tempo through a
 *  board render that overruns its frame.
 * ───────────────────────────────────────────────────────────────────────────── */
#ifndef WAIFU_ATARIST_AUDIO_H
#define WAIFU_ATARIST_AUDIO_H

#include <stdint.h>

enum AtaristMusicTrack {
    ATARIST_MUSIC_NONE = 0,
    ATARIST_MUSIC_TITLE,
    ATARIST_MUSIC_OVERWORLD,
    ATARIST_MUSIC_BATTLE,
    ATARIST_MUSIC_BOSS,
    ATARIST_MUSIC_FINAL_BOSS,
    ATARIST_MUSIC_RESULT,
    ATARIST_MUSIC_LOST,
    ATARIST_MUSIC_COUNT
};

void Atarist_AudioInit(void);
void Atarist_AudioShutdown(void);
/* Called from the VBL handler (atarist_isr.S).  Must stay short. */
void Atarist_AudioTick(void);

/* Point the player at a stream already resident in RAM.  The disk layer owns
 * the buffer; the player only reads it. */
void Atarist_MusicSetStream(int track, const uint8_t *stream, int32_t len);
void Atarist_MusicPlay(int track);
void Atarist_MusicStop(void);
int  Atarist_MusicTrack(void);

#endif /* WAIFU_ATARIST_AUDIO_H */
