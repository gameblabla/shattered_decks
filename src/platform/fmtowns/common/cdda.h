#ifndef FMT_CDDA_H
#define FMT_CDDA_H

#include <stdint.h>

/*
 * CD-DA (Red Book audio track) playback on the FM TOWNS internal CD-ROM
 * controller.
 *
 * This is the third and by far the cheapest of the three ways this
 * library makes sound, and the only one that costs no CPU time at all:
 *
 *   - sound.[ch]     RF5C68 wave-table playback out of the PCM chip's
 *                    own 64KB wave RAM. Short samples only.
 *   - pcmstream.[ch] a raw 8-bit PCM *file* on the data track, streamed
 *                    off the disc and bit-banged into the YM2612's
 *                    channel-6 DAC one sample at a time. The CPU does
 *                    nothing else for the whole song.
 *   - cdda.[ch]      this: 44.1kHz 16-bit stereo audio *tracks*, played
 *                    by the drive itself. One command starts a track;
 *                    the CPU is then free, and the audio keeps playing
 *                    while the game runs.
 *
 * The catch, and the reason the other two still exist: the drive has one
 * head. While it is playing an audio track it is not reading data
 * sectors, so on real hardware nothing can be loaded off the disc during
 * CD-DA playback - not fmt_cdrom_read(), not fmt_iso9660_load(), not
 * pcmstream's ring refills. (TOWNSEMU is more forgiving: it slurps the
 * whole track into host RAM when the play command is issued, so a
 * mixture that would starve or seek-thrash on a Marty appears to work
 * there. Don't take that as permission.) Load first, then start the
 * music, and treat the two streaming players as alternatives rather
 * than layers.
 *
 * Even in TOWNSEMU the two do not mix: a MODE1READ arriving while a
 * track is playing silently stops it (TownsCDROM::DelayedCommand-
 * Execution sets CDDAState=CDDA_IDLE for the read commands, with the
 * comment "CDDA needs to stop when MODE1READ is sent while playing"),
 * so the first sector cdrom.[ch] fetches kills the music.
 *
 * Command codes, parameter layout and status replies are not in the FM
 * TOWNS Technical Databook - section 6.3 says outright that "the formats
 * of the commands to the CDC, status replies from the CDC, and
 * parameters have not been publicly released. When developing programs
 * using the CD-ROM, please use the CD-ROM BIOS", which is not an option
 * in this bare-metal boot path (there is no BIOS INT 93H here). They
 * come instead from TOWNSEMU's CDC model, TOWNSEMU/src/towns/cdrom/
 * cdrom.cpp - PrepareCDDAPlay()/DelayedCommandExecution() for CDCMD_
 * CDDAPLAY(0x04)/TOCREAD(0x05)/SETSTATE(0x80)/CDDASTOP(0x84)/
 * CDDAPAUSE(0x85)/CDDARESUME(0x87), SetStatusQueueForTOC() for the TOC
 * reply format, and StatusSecondByte() for the play-state encoding - all
 * of which that file documents as reverse-engineered from the real
 * CD-ROM BIOS and from commercial games. The register-level handshake
 * (0x4C0-0x4C6, four reads per status entry, SIRQ/DEI acknowledgement)
 * is the same one cdrom.[ch] already uses for data sectors and *is* in
 * the databook; only the command payloads are inferred.
 *
 * Everything here polls. Nothing in this boot path is interrupt-driven
 * (see pcmstream.c's disable_interrupts() for why), so no command sets
 * the IRQ flag, and playback progress is checked by asking the drive.
 *
 * Addressing is BCD MSF (minute:second:frame) throughout, matching what
 * the drive reports in its TOC and expects in its play command: frame 0
 * of the disc is MSF 00:02:00 because of the 150-frame Red Book pregap,
 * so lba = M*60*75 + S*75 + F - 150 (fmt_cdda_msf_to_lba() below).
 * Track start times come straight out of the TOC and go straight into
 * the play command without conversion.
 */

/*----------------------------------------------------------------------
 * Mixer setup
 *--------------------------------------------------------------------*/

/* Routes CD audio to the speakers: sets the audio register's MUTE bit
 * (0x4EC bit 6, Table I-5-42 - "at reset time, this bit is zero, so in
 * order for audio output to occur, it must be set to 1") and opens the
 * CD channels of electronic volume 2 (0x4E2/0x4E3, Table I-5-1) all the
 * way up.
 *
 * Without this the drive plays and reports playing, and not a sound
 * comes out - the same trap pcmstream.c documents for the DAC path.
 * Call once before fmt_cdda_play_*(). Harmless to call alongside
 * pcmstream (they share the 0x4EC bit and agree on its value).
 */
void fmt_cdda_init(void);

/* Electronic volume 2, CD audio left/right, 0-63 where 63 is 0dB and
 * each step down is -0.5dB (Table I-5-2); 0 is -31.5dB, not silence.
 * Note this attenuates the analog CD audio line, so it takes effect
 * mid-track and applies equally to whatever the drive is already
 * playing. fmt_cdda_init() sets both to 63. */
void fmt_cdda_set_volume(unsigned left, unsigned right);

/*----------------------------------------------------------------------
 * Table of contents
 *--------------------------------------------------------------------*/

#define FMT_CDDA_MAX_TRACKS   99

typedef struct {
    uint8_t number;    /* Track number, 1-99 (binary, not BCD) */
    uint8_t audio;     /* 1 = audio track (playable), 0 = data track */
    uint8_t msf[3];    /* Absolute start time, BCD minute/second/frame */
} fmt_cdda_track;

typedef struct {
    uint8_t first;         /* First track number on the disc */
    uint8_t count;         /* Number of entries in track[] */
    uint8_t leadout[3];    /* BCD MSF of the lead-out, i.e. one past the
                            * end of the last track */
    fmt_cdda_track track[FMT_CDDA_MAX_TRACKS];
} fmt_cdda_toc;

/* Reads the disc's table of contents into `toc`. Returns 0 on success,
 * -1 if the drive reported an error or stopped replying (no disc, lid
 * open). A data-only disc reads back fine - it just has no track with
 * `audio` set. */
int fmt_cdda_read_toc(fmt_cdda_toc *toc);

/* Index into toc->track[] of track number `number`, or -1 if the disc
 * has no such track. */
int fmt_cdda_find_track(const fmt_cdda_toc *toc, unsigned number);

/* Index into toc->track[] of the first audio track, or -1 on a
 * data-only disc. On a typical mixed-mode game disc this is track 2,
 * track 1 being the ISO9660 data track. */
int fmt_cdda_first_audio_track(const fmt_cdda_toc *toc);

/* LBA of a BCD MSF address (subtracting the 150-frame pregap), for
 * lining TOC entries up with the sector addresses cdrom.[ch] uses. */
uint32_t fmt_cdda_msf_to_lba(const uint8_t msf[3]);

/*----------------------------------------------------------------------
 * Playback
 *
 * All of these return 0 if the drive accepted the command and -1 if it
 * reported an error or stopped replying. They return as soon as the
 * drive has acknowledged - playback itself continues in the background,
 * so a play call returning 0 means "started", not "finished".
 *--------------------------------------------------------------------*/

/* Plays the whole of `toc`'s track number `number`. With `loop` set the
 * drive repeats it indefinitely; otherwise it plays once and stops.
 * Fails (-1, nothing issued) if the disc has no such track or it isn't
 * an audio track. */
int fmt_cdda_play_track(const fmt_cdda_toc *toc, unsigned number, int loop);

/* Plays an arbitrary span, `start` to `end` inclusive, both BCD MSF.
 * The building block behind fmt_cdda_play_track() - useful for playing
 * part of a track (a loop point, a stinger) or several tracks back to
 * back as one span. */
int fmt_cdda_play_msf(const uint8_t start[3], const uint8_t end[3], int loop);

/* Stops playback. Safe to call when nothing is playing. */
int fmt_cdda_stop(void);

/* Pauses/resumes, holding the head where it is. Resuming something that
 * was stopped rather than paused does not restart it. */
int fmt_cdda_pause(void);
int fmt_cdda_resume(void);

/* What the drive is doing now, from a state-request command. Playback
 * ending on its own is reported as FMT_CDDA_STOPPED, so polling this is
 * how a non-looping track's end is detected - there is no interrupt and
 * no other completion signal in this polling-only setup. */
/* Poll it at a human pace - once a frame is plenty. Each call is a real
 * command to the drive's sub-MPU, and in TOWNSEMU the state-request
 * command also cancels the drive's pending scheduled callback, so
 * hammering it while a data transfer is in flight can stall the
 * transfer. */
enum {
    FMT_CDDA_STOPPED = 0,
    FMT_CDDA_PLAYING,
    FMT_CDDA_PAUSED,
    FMT_CDDA_NOT_READY,  /* No disc, or lid open */
    FMT_CDDA_ERROR       /* Drive reported an error, or stopped replying */
};
int fmt_cdda_state(void);

#endif
