#ifndef FMT_MEDIA_H
#define FMT_MEDIA_H

#include <stdint.h>

/*
 * Where the game's bulk assets come from, with the medium named once.
 *
 * The players (mp2stream.c, mbvplay.c, pcmstream.c, vgmplay.c) all want
 * the same three things - look a file up by name, read its first block,
 * and trickle it into a ring buffer without ever blocking - and none of
 * them care whether that is a CD-ROM sector read over the CDC's
 * command/status handshake or a copy out of an IC card's memory window.
 * This header is that shared vocabulary; cdrom.[ch] + iso9660.[ch] and
 * icm.[ch] are the two implementations behind it.
 *
 * The CD build is the default. Building with FMT_TARGET_ICCARD selects
 * the IC card, and then the CD sources are not compiled at all: no CDC
 * driver, no ISO9660 reader, no CD-DA (see the Makefile's ICCARD=1
 * target). An IC card machine may well have no drive, and code for a
 * device that is not there is code that can only mislead.
 *
 * "Offset" below means whatever the medium uses to name a position:
 * a 2048-byte LBA on CD, a byte offset on the card. Callers only ever
 * get one from fmt_media_find() and hand it straight back, so the
 * difference never leaks out.
 */

#if defined(FMT_TARGET_ICCARD) && FMT_TARGET_ICCARD

#include "icm.h"

typedef fmt_icm_stream fmt_media_stream;

#define FMT_MEDIA_STREAM_DONE   FMT_ICM_STREAM_DONE
#define FMT_MEDIA_STREAM_ERROR  FMT_ICM_STREAM_ERROR

/* One block of a file's start, for players that need to see a header
 * before committing to playback. 2048 bytes either way: on CD that is
 * one sector, which is the smallest a drive will hand over. */
#define FMT_MEDIA_BLOCK_BYTES   2048u

static inline int fmt_media_init(void)
{
    return fmt_icm_init();
}

static inline int fmt_media_find(const char *name, uint32_t *off, uint32_t *size)
{
    return fmt_icm_find(name, off, size);
}

static inline int32_t fmt_media_load(const char *name, void *buf, uint32_t bufsize)
{
    return fmt_icm_load(name, buf, bufsize);
}

static inline int fmt_media_read_block(uint32_t off, void *buf)
{
    return fmt_icm_read(off, FMT_MEDIA_BLOCK_BYTES, buf);
}

static inline void fmt_media_stream_init(fmt_media_stream *st, uint32_t off,
                                         uint32_t size, uint8_t *ring,
                                         uint32_t ring_size, int loop)
{
    fmt_icm_stream_init(st, off, size, ring, ring_size, loop);
}

static inline unsigned fmt_media_stream_step(fmt_media_stream *st,
                                             uint32_t play_pos, unsigned budget)
{
    return fmt_icm_stream_step(st, play_pos, budget);
}

#else /* CD-ROM */

#include "cdrom.h"
#include "iso9660.h"

typedef fmt_cd_stream fmt_media_stream;

#define FMT_MEDIA_STREAM_DONE   FMT_CD_STREAM_DONE
#define FMT_MEDIA_STREAM_ERROR  FMT_CD_STREAM_ERROR
#define FMT_MEDIA_BLOCK_BYTES   2048u

static inline int fmt_media_init(void)
{
    return 0;   /* the CDC needs no setting up before a read */
}

static inline int fmt_media_find(const char *name, uint32_t *off, uint32_t *size)
{
    return fmt_iso9660_find(name, off, size);
}

static inline int32_t fmt_media_load(const char *name, void *buf, uint32_t bufsize)
{
    return fmt_iso9660_load(name, buf, bufsize);
}

static inline int fmt_media_read_block(uint32_t off, void *buf)
{
    return fmt_cdrom_read(off, 1, buf);
}

static inline void fmt_media_stream_init(fmt_media_stream *st, uint32_t off,
                                         uint32_t size, uint8_t *ring,
                                         uint32_t ring_size, int loop)
{
    fmt_cdrom_stream_init(st, off, size, ring, ring_size, loop);
}

static inline unsigned fmt_media_stream_step(fmt_media_stream *st,
                                             uint32_t play_pos, unsigned budget)
{
    return fmt_cdrom_stream_step(st, play_pos, budget);
}

#endif

#endif
