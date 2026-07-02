/* CD32X audio routing.
 *
 * Music is CD-DA.  SFX are routed to the Sega CD PCM service on the resident
 * Mega-CD supervisor side; this SH-2 module deliberately keeps only the generic
 * game -> platform mapping.  The low-level supervisor command bridge is the next
 * step; these calls are isolated here so no common game/audio code changes. */
#include "waifu_cd32x_audio.h"
#include "waifu_cd32x_cdrom.h"
#include "cd32x_32x.h"
#include "cd32x_music_pcm.h"

#include <string.h>
#include <stdint.h>

#define CD32X_TRACK_NONE        0
#define CD32X_TRACK_TITLE       2
#define CD32X_TRACK_OVERWORLD   3
#define CD32X_TRACK_BATTLE      4
#define CD32X_TRACK_BOSS        5
#define CD32X_TRACK_FINAL_BOSS  6
#define CD32X_TRACK_VICTORY     7
#define CD32X_TRACK_FAIL        8

#define CD32X_MD_CMD_PCM_MUSIC      0xCD06u
#define CD32X_MD_CMD_PCM_MUSIC_STOP 0xCD07u
#define CD32X_MD_CMD_PCM_PLAY       0xCD05u
#define CD32X_PCM_THEME_NONE        (-1)
#define CD32X_MD_STATUS_ERROR       0xCDEEu

struct WaifuCd32xAudio {
    WaifuFmMusicTrack current_music;
    uint8_t active_cdda_track;
    uint8_t active_loop;
    int active_pcm_theme;
    int pcm_command_theme;
    uint8_t pcm_command_pending;
    int pending_sfx;
};

static WaifuCd32xAudio g_audio;

static uint8_t cdda_track_for_music(WaifuFmMusicTrack track)
{
    switch (track) {
    case WAIFU_FM_MUSIC_TITLE: return CD32X_TRACK_TITLE;
    case WAIFU_FM_MUSIC_OPENING_DREAM: return CD32X_TRACK_OVERWORLD;
    /* Deck-editor and in-duel themes are served by the RF5C164 PCM stream, not
       CD-DA: those screens read card data off the disc, and CD-DA would force a
       stop/resume around every read.  CD-DA stays for title / opening / results
       where no card data is streamed. */
    case WAIFU_FM_MUSIC_DECK_EDITOR:
    case WAIFU_FM_MUSIC_RANDOM_BATTLE:
    case WAIFU_FM_MUSIC_BOSS:
    case WAIFU_FM_MUSIC_FINAL_BOSS: return CD32X_TRACK_NONE;
    case WAIFU_FM_MUSIC_RESULTS: return CD32X_TRACK_VICTORY;
    case WAIFU_FM_MUSIC_LOST: return CD32X_TRACK_FAIL;
    case WAIFU_FM_MUSIC_NONE:
    default: return CD32X_TRACK_NONE;
    }
}

/* Which RF5C164 PCM theme (if any) backs a music track. */
static int pcm_theme_for_music(WaifuFmMusicTrack track)
{
    switch (track) {
    case WAIFU_FM_MUSIC_DECK_EDITOR:   return WAIFU_CD32X_MUSIC_DECK_EDITOR_ID;
    case WAIFU_FM_MUSIC_RANDOM_BATTLE: return WAIFU_CD32X_MUSIC_BATTLE_ID;
    case WAIFU_FM_MUSIC_BOSS:          return WAIFU_CD32X_MUSIC_BOSS_ID;
    case WAIFU_FM_MUSIC_FINAL_BOSS:    return WAIFU_CD32X_MUSIC_FINAL_BOSS_ID;
    default:                           return CD32X_PCM_THEME_NONE;
    }
}

static int cd32x_audio_supervisor_idle(void)
{
    return MARS_SYS_COMM0 == 0u;
}

static void cd32x_audio_poll_pcm_command(WaifuCd32xAudio *audio)
{
    if (!audio || !audio->pcm_command_pending) return;
    if (!cd32x_audio_supervisor_idle()) return;
    if (MARS_SYS_COMM4 != CD32X_MD_STATUS_ERROR) {
        audio->active_pcm_theme = audio->pcm_command_theme;
    }
    audio->pcm_command_pending = 0;
    audio->pcm_command_theme = CD32X_PCM_THEME_NONE;
}

static int cd32x_audio_try_send_pcm_music(WaifuCd32xAudio *audio, int theme)
{
    if (!audio) return 0;
    cd32x_audio_poll_pcm_command(audio);
    if (audio->pcm_command_pending) return 0;
    if (theme == audio->active_pcm_theme) return 1;
    if (!cd32x_audio_supervisor_idle()) return 0;
    if (theme >= 0) {
        MARS_SYS_COMM2 = (uint16_t)theme;
        MARS_SYS_COMM4 = CD32X_MD_CMD_PCM_MUSIC;
    } else {
        MARS_SYS_COMM4 = CD32X_MD_CMD_PCM_MUSIC_STOP;
    }
    MARS_SYS_COMM6 = 0;
    MARS_SYS_COMM0 = 1;
    audio->pcm_command_theme = theme;
    audio->pcm_command_pending = 1;
    return 1;
}

static uint8_t cdda_loop_for_music(WaifuFmMusicTrack track)
{
    switch (track) {
    case WAIFU_FM_MUSIC_RESULTS:
    case WAIFU_FM_MUSIC_LOST:
        return 0;
    default:
        return 1;
    }
}

WaifuCd32xAudio *waifu_cd32x_audio_create(void)
{
    memset(&g_audio, 0, sizeof(g_audio));
    g_audio.active_pcm_theme = CD32X_PCM_THEME_NONE;
    g_audio.pcm_command_theme = CD32X_PCM_THEME_NONE;
    g_audio.pending_sfx = -1;
    (void)waifu_cd32x_cdda_stop();
    return &g_audio;
}

void waifu_cd32x_audio_destroy(WaifuCd32xAudio *audio)
{
    (void)audio;
    waifu_cd32x_cdda_stop();
}

static void cd32x_audio_try_apply_music(WaifuCd32xAudio *audio)
{
    uint8_t cdda_track;
    uint8_t loop;
    int pcm_theme;
    if (!audio) return;

    cd32x_audio_poll_pcm_command(audio);
    pcm_theme = pcm_theme_for_music(audio->current_music);
    /* A PCM theme and CD-DA are mutually exclusive.  Start/stop the RF5C164
       stream on change; a theme implies CD-DA off. */
    if (pcm_theme != audio->active_pcm_theme) {
        (void)cd32x_audio_try_send_pcm_music(audio, pcm_theme);
    }
    if (audio->pcm_command_pending) return;
    if (pcm_theme != CD32X_PCM_THEME_NONE) {
        if (audio->active_cdda_track != CD32X_TRACK_NONE) {
            if (waifu_cd32x_cdda_stop()) {
                audio->active_cdda_track = CD32X_TRACK_NONE;
                audio->active_loop = 0;
            }
            if (audio->active_cdda_track != CD32X_TRACK_NONE) return;
        }
        return;
    }

    cdda_track = cdda_track_for_music(audio->current_music);
    loop = cdda_loop_for_music(audio->current_music);
    if (cdda_track == audio->active_cdda_track && loop == audio->active_loop) return;

    if (cdda_track == CD32X_TRACK_NONE) {
        if (waifu_cd32x_cdda_stop()) {
            audio->active_cdda_track = CD32X_TRACK_NONE;
            audio->active_loop = 0;
        }
    } else if (waifu_cd32x_cdda_play(cdda_track, loop)) {
        audio->active_cdda_track = cdda_track;
        audio->active_loop = loop;
    }
}

void waifu_cd32x_audio_set_music(WaifuCd32xAudio *audio, WaifuFmMusicTrack track)
{
    if (!audio) return;
    audio->current_music = track;
    cd32x_audio_try_apply_music(audio);
}

static void __attribute__((noinline)) cd32x_audio_send_sfx(int sfx_id)
{
    MARS_SYS_COMM2 = (uint16_t)sfx_id;
    MARS_SYS_COMM6 = 0;
    MARS_SYS_COMM4 = CD32X_MD_CMD_PCM_PLAY;
    MARS_SYS_COMM0 = 1;
}

void waifu_cd32x_audio_pump(WaifuCd32xAudio *audio)
{
    /* CD-ROM reads and direct-title transfers can temporarily occupy the
       resident M68K supervisor.  Do not declare CD-DA active after a failed
       command; retry here until the supervisor accepts the request. */
    cd32x_audio_try_apply_music(audio);
    if (audio && audio->pending_sfx >= 0 && !audio->pcm_command_pending &&
        cd32x_audio_supervisor_idle()) {
        cd32x_audio_send_sfx(audio->pending_sfx);
        audio->pending_sfx = -1;
    }
}

void waifu_cd32x_audio_play_sfx(int sfx_id)
{
    if (sfx_id < 0) return;
    if (!cd32x_audio_supervisor_idle()) {
        g_audio.pending_sfx = sfx_id;
        return;
    }
    cd32x_audio_send_sfx(sfx_id);
    g_audio.pending_sfx = -1;
    /* Do not block the game on SFX.  If the supervisor is busy with a music/CD
       request the latest one-shot is retried from the audio pump. */
}
