/* FM TOWNS music: the game's soundtrack as Red Book CD-DA tracks.
 *
 * The game core does not command music; it just reports which track it
 * *wants* playing (waifu_fm_audio_music_track()) and expects the platform to
 * make that true, the same contract waifu_pcfx_audio.c and waifu_cd32x_audio.c
 * implement.  fmtowns_audio_update() is that reconciliation step: call it
 * every frame with the core's wish, and it starts, switches, stops and
 * repairs playback as needed.
 *
 * "Repairs" is not defensive padding -- it is the whole reason this is a
 * state machine rather than a play() call.  The drive has one head, so any
 * data-sector read kills whatever CD-DA is playing (see cdda.h's block
 * comment: TOWNSEMU's CDC model stops CDDA outright on MODE1READ, and real
 * hardware physically cannot do both).  This port streams card and portrait
 * art off that same disc all game long, so music dying mid-scene is normal
 * traffic, not an error.  waifu_fmtowns_cdrom.c therefore calls
 * fmtowns_audio_note_data_read() around its reads, and the next update()
 * restarts the track.
 *
 * Restarts necessarily begin the track over rather than resuming -- the drive
 * takes an MSF span to play, and nothing reports how far in it got.  Keeping
 * the asset cache large enough that reads are rare (Makefile.fmtowns's
 * WAIFU_ASSET_BIG_CACHE_SLOTS) is what keeps that from being audible.
 */
#include "fmtowns_audio.h"
#include "cdda.h"
#include "fmtowns_cdda_tracks.h"

/* How often to ask the drive what it is doing, in frames.  cdda.h warns that
 * each state request is a real command to the drive's sub-MPU and that
 * hammering it can stall a transfer, so this is a slow background check for
 * the one case nothing else reports -- a looping track that stopped for a
 * reason we did not cause.  The common case (our own CD reads) is handled
 * immediately by fmtowns_audio_note_data_read() instead of by polling. */
#define STATE_POLL_INTERVAL 60

static fmt_cdda_toc g_toc;
static int g_toc_valid;          /* 0 = not read yet / no disc */
static int g_mixer_ready;

static int g_wanted_track;       /* CD track number, 0 = silence */
static int g_wanted_loop;
static int g_started_track;      /* what we last successfully started */
static int g_needs_start;
static int g_poll_countdown;
static int g_last_state = FMT_CDDA_STOPPED;  /* last value the poll below saw */

/* Maps the core's music enum onto this disc's track numbers.  The
 * FMTOWNS_CDDA_TRACK_* constants are generated from Makefile.fmtowns's
 * MUSIC_STEMS order, so this cannot drift from the disc layout.  The
 * enum -> song assignment itself matches waifu_pcfx_audio.c's
 * music_to_cdda_track(), so every platform plays the same music in the same
 * place. */
static int music_to_track(WaifuFmMusicTrack track)
{
    switch (track) {
    case WAIFU_FM_MUSIC_TITLE:         return FMTOWNS_CDDA_TRACK_TITLESCREEN_MOONLITCIPHER;
    case WAIFU_FM_MUSIC_OPENING_DREAM: return FMTOWNS_CDDA_TRACK_OVERWORLD;
    case WAIFU_FM_MUSIC_DECK_EDITOR:   return FMTOWNS_CDDA_TRACK_OVERWORLD;
    case WAIFU_FM_MUSIC_BOSS:          return FMTOWNS_CDDA_TRACK_BOSS;
    case WAIFU_FM_MUSIC_FINAL_BOSS:    return FMTOWNS_CDDA_TRACK_FINALBOSS;
    case WAIFU_FM_MUSIC_RANDOM_BATTLE: return FMTOWNS_CDDA_TRACK_BATTLE;
    case WAIFU_FM_MUSIC_RESULTS:       return FMTOWNS_CDDA_TRACK_VICTORY;
    case WAIFU_FM_MUSIC_LOST:          return FMTOWNS_CDDA_TRACK_FAIL;
    default:                           return 0;
    }
}

/* Victory and loss are jingles: they play once and leave silence behind.
 * Everything else is background music and loops. */
static int music_loops(WaifuFmMusicTrack track)
{
    switch (track) {
    case WAIFU_FM_MUSIC_RESULTS:
    case WAIFU_FM_MUSIC_LOST:
        return 0;
    default:
        return 1;
    }
}

static int ensure_toc(void)
{
    if (g_toc_valid) {
        return 1;
    }
    if (!g_mixer_ready) {
        /* Routes CD audio to the speakers -- without it the drive plays and
         * reports playing and nothing comes out (cdda.h). */
        fmt_cdda_init();
        g_mixer_ready = 1;
    }
    if (fmt_cdda_read_toc(&g_toc) != 0) {
        return 0;
    }
    g_toc_valid = 1;
    return 1;
}

void fmtowns_audio_init(void)
{
    g_toc_valid = 0;
    g_mixer_ready = 0;
    g_wanted_track = 0;
    g_wanted_loop = 1;
    g_started_track = 0;
    g_needs_start = 0;
    g_poll_countdown = STATE_POLL_INTERVAL;
    (void)ensure_toc();
}

void fmtowns_audio_update(WaifuFmMusicTrack wanted)
{
    int track = music_to_track(wanted);
    int loop = music_loops(wanted);

    if (track != g_wanted_track || loop != g_wanted_loop) {
        g_wanted_track = track;
        g_wanted_loop = loop;
        g_needs_start = 1;
    }

    if (!g_needs_start) {
        /* Nothing to change; just make sure a track that should be looping
         * still is.  Anything that stopped it (our own CD reads, a drive
         * hiccup) shows up here as STOPPED. */
        if (g_started_track != 0 && g_wanted_loop) {
            if (--g_poll_countdown <= 0) {
                g_poll_countdown = STATE_POLL_INTERVAL;
                g_last_state = fmt_cdda_state();
                if (g_last_state != FMT_CDDA_PLAYING) {
                    g_needs_start = 1;
                }
            }
        }
        if (!g_needs_start) {
            return;
        }
    }

    g_needs_start = 0;
    g_poll_countdown = STATE_POLL_INTERVAL;

    if (g_wanted_track == 0) {
        if (g_started_track != 0) {
            (void)fmt_cdda_stop();
            g_started_track = 0;
        }
        return;
    }

    if (!ensure_toc()) {
        /* No disc / drive not answering.  Leave g_started_track alone and
         * retry on the next poll rather than spinning on the drive here. */
        g_needs_start = 1;
        return;
    }

    if (fmt_cdda_play_track(&g_toc, (unsigned)g_wanted_track, g_wanted_loop) != 0) {
        g_started_track = 0;
        g_needs_start = 1;    /* e.g. a data-only disc build; harmless to retry */
        return;
    }
    g_started_track = g_wanted_track;
    g_last_state = FMT_CDDA_PLAYING;
}

int fmtowns_audio_started_track(void)
{
    return g_started_track;
}

int fmtowns_audio_last_state(void)
{
    return g_last_state;
}

void fmtowns_audio_note_data_read(void)
{
    /* The read is about to take the head away from the audio track, so
     * whatever was playing is already dead as far as the drive is concerned.
     * Flag a restart for the next update() instead of restarting here: the
     * caller is usually part-way through a burst of reads, and starting
     * playback between two of them would only get killed again. */
    if (g_started_track != 0 && g_wanted_loop) {
        g_needs_start = 1;
    }
}

int fmtowns_audio_state(void)
{
    return fmt_cdda_state();
}

int fmtowns_audio_start_music(void)
{
    /* Kept for fmtowns_main.c's pre-milestone-7 demo loops (FMTOWNS_MILESTONE
     * 5 and 6), which predate the core-driven music manager above and only
     * ever wanted "start the title track and leave it". */
    fmtowns_audio_init();
    fmtowns_audio_update(WAIFU_FM_MUSIC_TITLE);
    return g_started_track != 0;
}
