#ifndef WAIFU_CD32X_PCM_H
#define WAIFU_CD32X_PCM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void pcm_delay(void);
uint8_t pcm_lcf(uint8_t pan);
void pcm_set_period(uint32_t period);
void pcm_set_freq(uint32_t freq_delta);
void pcm_set_timer(uint16_t bpm);
void pcm_stop_timer(void);
void pcm_start_timer(void (*callback)(void));

void pcm_load_samples(uint8_t start, const int8_t *samples, uint16_t length);
uint16_t pcm_next_block(uint8_t start, uint16_t length);
void pcm_reset(void);
void pcm_set_ctrl(uint8_t val);
void pcm_set_off(uint8_t index);
void pcm_set_on(uint8_t index);
void pcm_set_start(uint8_t start, uint16_t offset);
void pcm_set_loop(uint16_t loopstart);
void pcm_set_env(uint8_t vol);
void pcm_set_pan(uint8_t pan);

void cd32x_pcm_sfx_init_from_bank(const int8_t *bank, uint32_t bank_bytes);
int cd32x_pcm_sfx_ready(void);
int cd32x_pcm_sfx_play(int effect);

/* RF5C164 streamed music.  The clip (already RF5C164 sign/magnitude bytes) lives
   in Sub-CPU PRG RAM; the supervisor refills a small wave-RAM ring from it each
   vblank tick so deck-editor / in-duel music plays from PCM with the CD free for
   card streaming.  cd32x_music_clip_buffer() is the PRG-RAM staging the loader
   reads the clip file into. */
int8_t *cd32x_music_clip_buffer(void);
uint32_t cd32x_music_clip_capacity(void);
void cd32x_music_start(uint32_t clip_bytes);
void cd32x_music_stop(void);
void cd32x_music_pump(void);

#ifdef __cplusplus
}
#endif

#endif /* WAIFU_CD32X_PCM_H */
