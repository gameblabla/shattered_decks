#ifndef FMT_ICM_H
#define FMT_ICM_H

#include <stdint.h>
#include "icm_defs.h"

/*
 * Asset access for the bootable IC Memory Card build - the card-side
 * counterpart of cdrom.[ch] + iso9660.[ch], and deliberately the same
 * shape so src/common/media.h can present one API over either.
 *
 * There is no drive, no controller and no filesystem here. The card is
 * memory: an asset "read" is a copy out of the card's window in the
 * physical address space, and the only thing standing between a byte on
 * the card and the CPU is the bank register (I/O 0x490) on machines
 * whose window is smaller than the card - which on the 386SX/Marty, at
 * 1MB, it usually is. See icm_defs.h for the geometry.
 *
 * That makes the streaming reader below trivial compared to the CD one:
 * it cannot fail, cannot be late, and has no drive state machine. It
 * exists only so the players (mp2stream.c, mbvplay.c, pcmstream.c) can
 * be written once against a ring buffer that fills a bounded amount per
 * call, and run unchanged off either medium.
 *
 * Note what does *not* go through here: anything the linker put in
 * .rodata is already at a card address (src/boot/icm.lds.S) and is read
 * by simply dereferencing it. This interface is for the bulk assets
 * that live past the payload - the ones tools/mkicm.py appends and
 * lists in the card's directory.
 */

/* Reads the card's directory, which tools/mkicm.py wrote and the IPL
 * sector points at. Returns 0 on success, -1 if the card does not look
 * like one of ours. Call once before anything else here. */
int fmt_icm_init(void);

/* Looks up `name` (8.3-style, case-insensitive, e.g. "MUSIC.MP2") in
 * the card directory. On success returns 0 and sets `off`/`size` to the
 * asset's byte offset into the card image and its length. */
int fmt_icm_find(const char *name, uint32_t *off, uint32_t *size);

/* Copies `bytes` bytes from card offset `off` into `buf`, crossing bank
 * boundaries as needed. Returns 0, or -1 if the range runs off the end
 * of the card. */
int fmt_icm_read(uint32_t off, uint32_t bytes, void *buf);

/* Looks up `name` and copies all of it into `buf`. Returns its length,
 * or -1 if it is missing or does not fit. */
int32_t fmt_icm_load(const char *name, void *buf, uint32_t bufsize);

/*----------------------------------------------------------------------
 * Streaming reader - see the fmt_cd_stream comment in cdrom.h for the
 * ring-buffer contract, which is identical: the caller owns the read
 * cursor and passes it in, this owns the write cursor (`fill_pos`), and
 * both count bytes since the start so `fill_pos - play_pos` is what is
 * available. The ring size must be a power of two.
 *--------------------------------------------------------------------*/

enum {
    FMT_ICM_STREAM_RUN,     /* more to deliver */
    FMT_ICM_STREAM_DONE,    /* end of a non-looping asset */
    FMT_ICM_STREAM_ERROR    /* the asset does not fit on the card */
};

/* Bytes copied per unit of step budget. The players hand out a budget
 * of 1 from inside sample-paced loops, where the DAC needs servicing
 * every 62.5us, so one unit has to be small enough to disappear into
 * that gap - a 64-byte copy is a couple of microseconds - while still
 * being enough, at the ~16000 calls a second those loops make, to feed
 * a video stream several times over. */
#define FMT_ICM_STEP_BYTES  64u

typedef struct {
    uint8_t *ring;
    uint32_t ring_mask;     /* ring size - 1 */
    uint32_t fill_pos;      /* bytes deposited since the start */
    uint32_t off;           /* card offset of the asset */
    uint32_t size;          /* asset length in bytes */
    uint32_t pos;           /* bytes taken from the asset so far */
    uint8_t  state;
    uint8_t  loop;
} fmt_icm_stream;

/* Prepares `st` to stream `size` bytes from card offset `off` into
 * `ring`. With `loop` set, the end of the asset wraps to its start. */
void fmt_icm_stream_init(fmt_icm_stream *st, uint32_t off, uint32_t size,
                         uint8_t *ring, uint32_t ring_size, int loop);

/* Copies at most `budget` * FMT_ICM_STEP_BYTES bytes into the ring,
 * stopping early if it would overrun the caller's read cursor
 * `play_pos`. Returns how many bytes it added. Never blocks. */
unsigned fmt_icm_stream_step(fmt_icm_stream *st, uint32_t play_pos,
                             unsigned budget);

#endif
