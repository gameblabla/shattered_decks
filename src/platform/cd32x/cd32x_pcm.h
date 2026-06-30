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

/* RF5C164 streamed music.  One already-converted sign/magnitude chunk at a time
   lives in Sub-CPU PRG RAM; the supervisor refills a wave-RAM ring from it each
   vblank tick.  When the PRG chunk has been fully queued into wave RAM,
   cd32x_music_needs_chunk() asks the supervisor to load the next CD chunk. */
int8_t *cd32x_music_clip_buffer(void);
uint32_t cd32x_music_clip_capacity(void);
void cd32x_music_start(uint32_t chunk_bytes);
void cd32x_music_supply_chunk(uint32_t chunk_bytes);
int cd32x_music_needs_chunk(void);
void cd32x_music_stop(void);
void cd32x_music_pump(void);

#ifdef __cplusplus
}
#endif

#endif /* WAIFU_CD32X_PCM_H */
