#include "fmtowns_audio.h"
#include "cdda.h"

int fmtowns_audio_start_music(void)
{
    fmt_cdda_toc toc;
    int track_idx;

    fmt_cdda_init();

    if (fmt_cdda_read_toc(&toc) != 0) {
        return 0;
    }

    track_idx = fmt_cdda_first_audio_track(&toc);
    if (track_idx < 0) {
        return 0;   /* data-only disc, e.g. the plain output.iso build */
    }

    if (fmt_cdda_play_track(&toc, toc.track[track_idx].number, 1) != 0) {
        return 0;
    }

    return 1;
}

int fmtowns_audio_state(void)
{
    return fmt_cdda_state();
}
