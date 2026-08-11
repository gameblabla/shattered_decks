#ifndef FMTOWNS_AUDIO_H
#define FMTOWNS_AUDIO_H

#include "game_api.h"

/* CD-DA music, the port's music path (sound effects go through the PCM chip
 * instead -- see fmtowns_sfx.h).  Thin manager over
 * src/platform/fmtowns/common/cdda.c; see fmtowns_audio.c's header for why
 * this is a per-frame state machine rather than a play() call. */

/* Sets the CD audio mixer up and reads the disc TOC.  Call once at boot,
 * before the first fmtowns_audio_update(). */
void fmtowns_audio_init(void);

/* Reconciles what is playing with what the game core wants playing
 * (waifu_fm_audio_music_track()).  Call once a frame; it is cheap on the
 * frames where nothing needs to change. */
void fmtowns_audio_update(WaifuFmMusicTrack wanted);

/* Tell the music layer that a CD data read is about to happen (or just
 * happened).  A read stops CD-DA on this hardware, so this is how a looping
 * track gets restarted afterwards instead of silently staying dead. */
void fmtowns_audio_note_data_read(void);

/* What the drive is doing now: FMT_CDDA_PLAYING, FMT_CDDA_STOPPED, etc
 * (cdda.h).  Diagnostics only -- fmtowns_audio_update() does its own,
 * rate-limited polling. */
int fmtowns_audio_state(void);

/* CD track number this layer last successfully started (0 = silence), and the
 * most recent playback state it observed, without asking the drive again.
 * Cheap enough to read every frame, unlike fmtowns_audio_state(). */
int fmtowns_audio_started_track(void);
int fmtowns_audio_last_state(void);

/* Starts the title track and leaves it looping.  Only fmtowns_main.c's
 * pre-milestone-7 demo loops use this; the real game loop drives
 * fmtowns_audio_update() from the core's music state instead. */
int fmtowns_audio_start_music(void);

#endif /* FMTOWNS_AUDIO_H */
