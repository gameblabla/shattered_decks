#ifndef FMT_VGMPLAY_H
#define FMT_VGMPLAY_H

/* Read and validate a converted YM2612 stream from the ISO9660 data track.
 * Loading does not change the sound hardware.  Only one file can be loaded at
 * a time; loading another one replaces the previous file. */
int fmt_vgm_load_file(const char *name);

/* Play the loaded stream once.  This is synchronous: it returns 0 at the end
 * of the song, or -1 if no valid file is loaded or the stream is malformed. */
int fmt_vgm_play(void);

/* Silence VGM output and request that an active fmt_vgm_play() return.  The
 * request is useful when playback is driven from a future interrupt or task;
 * in the current synchronous player it also provides explicit cleanup after
 * fmt_vgm_play() returns. */
void fmt_vgm_stop(void);

#endif
