#include "cdda.h"
#include "cdrom.h"
#include "io.h"

/* See cdda.h for the design and for where these command/status codes
 * come from (TOWNSEMU's CDC model - the databook does not publish
 * them). */

/*----------------------------------------------------------------------
 * CDC commands (written to 0x4C2, ORed with FMT_CDC_FLAG_STATUS so the
 * drive replies at all). Bit 7 distinguishes the two command groups per
 * Table I-6-3: PLAY commands (0x0x) versus STATE control commands
 * (0x8x), which is why the codes below are not contiguous.
 *--------------------------------------------------------------------*/
#define CDCMD_CDDAPLAY            0x04
#define CDCMD_TOCREAD             0x05
#define CDCMD_GETSTATE            0x80
#define CDCMD_CDDASTOP            0x84
#define CDCMD_CDDAPAUSE           0x85
#define CDCMD_CDDARESUME          0x87

/*----------------------------------------------------------------------
 * First byte of a 4-byte status FIFO reply.
 *--------------------------------------------------------------------*/
#define CDSTAT_NO_ERROR           0x00  /* Command accepted; 2nd byte carries the drive state */
#define CDSTAT_TOC_POINT          0x16  /* TOC entry, part 1: what this entry describes */
#define CDSTAT_TOC_TIME           0x17  /* TOC entry, part 2: its MSF (or track count) */
#define CDSTAT_ERROR              0x21  /* Command failed; 2nd byte says why */

/* Second byte of a 0x21 error reply. */
#define CDERR_MEDIA_CHANGED       0x08

/* Second byte of a 0x00 reply - the drive's CD-DA state. From
 * TownsCDROM::StatusSecondByte(), which cites the disassembled BIOS and
 * Shadow of the Beast 2 ("checks the 2nd byte to be 03 for verifying
 * that the CDDA started playing"). */
#define CDPLAY_STOPPED            0x00
#define CDPLAY_PAUSED             0x01
#define CDPLAY_PLAYING            0x03
#define CDPLAY_NOT_READY          0x09

/* Third byte of a TOC "point" entry (0x16). Values below 0xA0 are BCD
 * track numbers; these three are the standard Red Book lead-in
 * descriptors. */
#define TOC_POINT_FIRST_TRACK     0xA0
#define TOC_POINT_NUM_TRACKS      0xA1
#define TOC_POINT_LEADOUT         0xA2

/* Second byte of a TOC track entry: bit 6 is the data/audio flag (the
 * Red Book control field's "data track" bit). */
#define TOC_TRACK_IS_DATA         0x40

/*----------------------------------------------------------------------
 * Mixer registers - FM TOWNS Technical Databook Tables I-5-1 and
 * I-5-42.
 *--------------------------------------------------------------------*/
#define SND_ELEVOL2_DATA          0x4E2  /* Volume 2 DATA: attenuation, 0-63 */
#define SND_ELEVOL2_COM           0x4E3  /* Volume 2 COM: channel select + EN/C0/C32 */
#define SND_AUDIO                 0x4EC  /* Audio register: LOFF/MUTE */

#define ELEVOL_CH_CD_LEFT         0x00   /* Volume 2, CH1:CH0 = 00 */
#define ELEVOL_CH_CD_RIGHT        0x01   /* Volume 2, CH1:CH0 = 01 */
#define ELEVOL_EN                 0x04   /* Output enabled; 0 means -infinity dB */
#define ELEVOL_MAX                63u    /* D0-5 all set = 0dB */

/* MUTE(bit6)=1=output. Bit 7 (LOFF) left clear so the front-panel level
 * indicator still works; the low bits are written 1 per the databook's
 * write-format row. Same value pcmstream.c writes - the two agree, so
 * either may run first. */
#define SND_AUDIO_ON              0x7F

#define CD_MSF_PREGAP_FRAMES      150u   /* 00:02:00 */
#define CD_FRAMES_PER_SEC          75u
#define CD_SECS_PER_MIN            60u

static inline unsigned from_bcd(uint8_t v)
{
    return (unsigned)(v >> 4) * 10u + (unsigned)(v & 0x0Fu);
}

static inline uint8_t to_bcd(unsigned v)
{
    return (uint8_t)(((v / 10u) << 4) | (v % 10u));
}

static uint32_t msf_to_frames(const uint8_t msf[3])
{
    return (from_bcd(msf[0]) * CD_SECS_PER_MIN + from_bcd(msf[1])) * CD_FRAMES_PER_SEC
           + from_bcd(msf[2]);
}

static void frames_to_msf(uint32_t frames, uint8_t msf[3])
{
    msf[0] = to_bcd(frames / (CD_SECS_PER_MIN * CD_FRAMES_PER_SEC));
    msf[1] = to_bcd((frames / CD_FRAMES_PER_SEC) % CD_SECS_PER_MIN);
    msf[2] = to_bcd(frames % CD_FRAMES_PER_SEC);
}

uint32_t fmt_cdda_msf_to_lba(const uint8_t msf[3])
{
    uint32_t frames = msf_to_frames(msf);
    return frames > CD_MSF_PREGAP_FRAMES ? frames - CD_MSF_PREGAP_FRAMES : 0;
}

/*----------------------------------------------------------------------
 * Command plumbing
 *--------------------------------------------------------------------*/

/* Issues `cmd` with `param` (8 bytes) and reads back its first status
 * entry into `status`. Returns 0 if that entry arrived, -1 if the drive
 * never replied.
 *
 * The retry exists because of a "media changed" reply (0x21 0x08),
 * which the drive posts once for the first command that asks about the
 * disc after it was inserted - and, because -CD hands TOWNSEMU the image
 * at power-on, that flag is still pending here no matter how much data
 * has already been read: the MODE1READ path in cdrom.c never consumes
 * it (TownsCDROM::DelayedCommandExecution only checks DiscLoadedAnd-
 * LidClosed() for reads, not discChanged). It is a status, not a
 * failure - the disc is fine, the drive just wanted to say so - and the
 * flag is cleared by the act of reporting it, so re-issuing the command
 * gets the real answer. Every command here goes through this, so
 * whichever one happens to run first absorbs it. */
static int cdc_command(uint8_t cmd, const uint8_t param[8], uint8_t status[4])
{
    for (int attempt = 0; attempt < 2; attempt++) {
        fmt_cdc_drain_status();
        fmt_cdc_issue((uint8_t)(cmd | FMT_CDC_FLAG_STATUS), param);

        if (fmt_cdc_read_status(status) != 0) {
            return -1;
        }
        if (!(status[0] == CDSTAT_ERROR && status[1] == CDERR_MEDIA_CHANGED)) {
            return 0;
        }
    }
    return 0; /* Still "media changed" twice over - report it as-is. */
}

/* Consumes whatever else the drive has to say and clears its interrupt
 * flags, leaving the FIFO empty for the next command. Several of these
 * commands reply with two or three entries (CDDASTOP posts a state, a
 * "stop done" 0x11, and a trailing 00 0D) and none of the extra ones
 * tell us anything the first entry didn't. */
static void cdc_finish(void)
{
    for (int i = 0; i < 64 && fmt_cdc_status_pending(); i++) {
        uint8_t status[4];
        fmt_cdc_read_status(status);
    }
    fmt_cdc_ack();
}

/* Runs a command that has nothing to report but success or failure. */
static int cdc_simple_command(uint8_t cmd)
{
    static const uint8_t no_param[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    uint8_t status[4];
    int ok;

    ok = cdc_command(cmd, no_param, status);
    cdc_finish();

    return (ok == 0 && status[0] == CDSTAT_NO_ERROR) ? 0 : -1;
}

/*----------------------------------------------------------------------
 * Mixer setup
 *--------------------------------------------------------------------*/

/* One channel of electronic volume 2. The COM register both selects the
 * channel and carries its EN/C0/C32 bits, while the DATA register sets
 * the attenuation of whichever channel COM last selected - so COM has to
 * go first. It is written again afterwards because a real electronic-
 * volume chip latches on the COM strobe; TOWNSEMU applies DATA
 * immediately and doesn't care, and the repeat is harmless there. */
static void set_channel_volume(uint8_t channel, unsigned vol)
{
    uint8_t com = (uint8_t)(channel | ELEVOL_EN);

    if (vol > ELEVOL_MAX) {
        vol = ELEVOL_MAX;
    }
    outb(com, SND_ELEVOL2_COM);
    outb((uint8_t)vol, SND_ELEVOL2_DATA);
    outb(com, SND_ELEVOL2_COM);
}

void fmt_cdda_set_volume(unsigned left, unsigned right)
{
    set_channel_volume(ELEVOL_CH_CD_LEFT, left);
    set_channel_volume(ELEVOL_CH_CD_RIGHT, right);
}

void fmt_cdda_init(void)
{
    outb(SND_AUDIO_ON, SND_AUDIO);
    fmt_cdda_set_volume(ELEVOL_MAX, ELEVOL_MAX);
}

/*----------------------------------------------------------------------
 * Table of contents
 *--------------------------------------------------------------------*/

/* The reply to TOCREAD is a run of status entries in fixed pairs: a
 * 0x16 "point" entry saying what is being described, then a 0x17 entry
 * carrying its time (or, for the lead-in points, a track number). Their
 * order is lead-in first - first track (0xA0), track count (0xA1),
 * lead-out time (0xA2) - then one pair per track, in track order. See
 * TownsCDROM::SetStatusQueueForTOC(), which notes each field was
 * recovered by reverse-engineering Shadow of the Beast.
 *
 * How many pairs there are is only knowable from the 0xA1 entry, so the
 * loop reads until it has the lead-out plus that many tracks. Track
 * times come back already including the 150-frame pregap - the same
 * absolute MSF the play command takes - so they are stored raw. */
int fmt_cdda_read_toc(fmt_cdda_toc *toc)
{
    static const uint8_t no_param[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    uint8_t status[4];
    unsigned expect_tracks = 0;
    int have_count = 0;
    int have_leadout = 0;

    toc->first = 0;
    toc->count = 0;
    toc->leadout[0] = toc->leadout[1] = toc->leadout[2] = 0;

    if (cdc_command(CDCMD_TOCREAD, no_param, status) != 0) {
        return -1;
    }

    /* Bounded by the largest reply a 99-track disc can produce (3
     * lead-in pairs + 99 track pairs, two entries each) plus a little
     * slack for the leading "command accepted" entry. */
    for (int i = 0; i < 2 * 2 * (3 + FMT_CDDA_MAX_TRACKS); i++) {
        uint8_t time[4];

        if (status[0] == CDSTAT_ERROR) {
            cdc_finish();
            return -1;
        }
        if (status[0] == CDSTAT_TOC_POINT) {
            /* The 0x17 half of the pair lands in its own buffer, so the
             * 0x16 half is still in `status` below: status[1] is the
             * track's data/audio flag and status[2] the point. */
            if (fmt_cdc_read_status(time) != 0 || time[0] != CDSTAT_TOC_TIME) {
                break;
            }

            switch (status[2]) {
            case TOC_POINT_FIRST_TRACK:
                toc->first = (uint8_t)from_bcd(time[1]);
                break;
            case TOC_POINT_NUM_TRACKS:
                expect_tracks = from_bcd(time[1]);
                if (expect_tracks > FMT_CDDA_MAX_TRACKS) {
                    expect_tracks = FMT_CDDA_MAX_TRACKS;
                }
                have_count = 1;
                break;
            case TOC_POINT_LEADOUT:
                toc->leadout[0] = time[1];
                toc->leadout[1] = time[2];
                toc->leadout[2] = time[3];
                have_leadout = 1;
                break;
            default:
                if (toc->count < FMT_CDDA_MAX_TRACKS) {
                    fmt_cdda_track *t = &toc->track[toc->count++];
                    t->number = (uint8_t)from_bcd(status[2]);
                    t->audio = (uint8_t)((status[1] & TOC_TRACK_IS_DATA) ? 0 : 1);
                    t->msf[0] = time[1];
                    t->msf[1] = time[2];
                    t->msf[2] = time[3];
                }
                break;
            }
        }

        /* Everything the disc had to say. Stopping on the expected
         * count rather than on an empty FIFO matters: the drive fills
         * the FIFO as it reads the lead-in, so "nothing waiting right
         * now" is not "nothing more coming", and the only alternative
         * to counting is to sit out a full read timeout at the end of
         * every TOC read. */
        if (have_leadout && have_count && toc->count >= expect_tracks) {
            break;
        }
        if (fmt_cdc_read_status(status) != 0) {
            break;
        }
    }

    cdc_finish();

    if (toc->count == 0) {
        return -1;
    }
    if (toc->first == 0) {
        toc->first = toc->track[0].number;
    }
    if (!have_leadout) {
        return -1; /* Without it the last track has no end time. */
    }
    return 0;
}

int fmt_cdda_find_track(const fmt_cdda_toc *toc, unsigned number)
{
    for (unsigned i = 0; i < toc->count; i++) {
        if (toc->track[i].number == number) {
            return (int)i;
        }
    }
    return -1;
}

int fmt_cdda_first_audio_track(const fmt_cdda_toc *toc)
{
    for (unsigned i = 0; i < toc->count; i++) {
        if (toc->track[i].audio) {
            return (int)i;
        }
    }
    return -1;
}

/*----------------------------------------------------------------------
 * Playback
 *--------------------------------------------------------------------*/

int fmt_cdda_play_msf(const uint8_t start[3], const uint8_t end[3], int loop)
{
    uint8_t param[8];
    uint8_t status[4];
    int ok;

    param[0] = start[0];
    param[1] = start[1];
    param[2] = start[2];
    param[3] = end[0];
    param[4] = end[1];
    param[5] = end[2];
    /* Repeat flag. TOWNSEMU reads it as "1 means loop forever"
     * (`bool repeat=(1==state.paramQueue[6])`); the BIOS entry point it
     * was inferred from takes a repeat count in CH (0 or 0xFF in the
     * two sample programs under TOWNSEMU/testc/cdrom/), so a real drive
     * may well accept counts here. Only 0 and 1 are known-good. */
    param[6] = (uint8_t)(loop ? 1 : 0);
    param[7] = 0;

    ok = cdc_command(CDCMD_CDDAPLAY, param, status);
    cdc_finish();

    return (ok == 0 && status[0] == CDSTAT_NO_ERROR) ? 0 : -1;
}

int fmt_cdda_play_track(const fmt_cdda_toc *toc, unsigned number, int loop)
{
    int idx = fmt_cdda_find_track(toc, number);
    uint32_t end_frames;
    uint8_t end[3];

    if (idx < 0 || !toc->track[idx].audio) {
        return -1;
    }

    /* The track runs up to wherever the next one starts - or the
     * lead-out, for the last track - and the end address is inclusive,
     * matching the MODE1READ command in cdrom.c (whose end MSF is the
     * last sector read, not one past it). The frame given up to that
     * "minus 1" is 1/75s. */
    if ((unsigned)idx + 1u < toc->count) {
        end_frames = msf_to_frames(toc->track[idx + 1].msf);
    } else {
        end_frames = msf_to_frames(toc->leadout);
    }
    if (end_frames <= msf_to_frames(toc->track[idx].msf)) {
        return -1; /* Zero-length or out-of-order TOC. */
    }
    frames_to_msf(end_frames - 1u, end);

    return fmt_cdda_play_msf(toc->track[idx].msf, end, loop);
}

int fmt_cdda_stop(void)
{
    return cdc_simple_command(CDCMD_CDDASTOP);
}

int fmt_cdda_pause(void)
{
    return cdc_simple_command(CDCMD_CDDAPAUSE);
}

int fmt_cdda_resume(void)
{
    return cdc_simple_command(CDCMD_CDDARESUME);
}

int fmt_cdda_state(void)
{
    static const uint8_t no_param[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    uint8_t status[4];
    int ok;

    ok = cdc_command(CDCMD_GETSTATE, no_param, status);
    cdc_finish();

    if (ok != 0 || status[0] != CDSTAT_NO_ERROR) {
        return FMT_CDDA_ERROR;
    }
    switch (status[1]) {
    case CDPLAY_PLAYING:   return FMT_CDDA_PLAYING;
    case CDPLAY_PAUSED:    return FMT_CDDA_PAUSED;
    case CDPLAY_NOT_READY: return FMT_CDDA_NOT_READY;
    default:               return FMT_CDDA_STOPPED;
    }
}
