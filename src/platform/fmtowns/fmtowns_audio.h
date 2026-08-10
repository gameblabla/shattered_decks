#ifndef FMTOWNS_AUDIO_H
#define FMTOWNS_AUDIO_H

/* CD-DA music, the primary audio path for this port (see STATUS.md) --
 * thin wrapper over src/platform/fmtowns/common/cdda.c.
 *
 * Sets the mixer up, reads the disc's table of contents, and starts the
 * first audio track (track 2 on this port's mixed-mode disc -- track 1 is
 * the ISO9660 data track the game itself boots from) looping. Returns 1 if
 * playback was started, 0 if the disc has no audio track (a data-only
 * .iso build) or the drive reported an error. */
int fmtowns_audio_start_music(void);

/* What the drive is doing now: FMT_CDDA_PLAYING, FMT_CDDA_STOPPED, etc
 * (cdda.h). Safe to poll once a frame. */
int fmtowns_audio_state(void);

#endif /* FMTOWNS_AUDIO_H */
