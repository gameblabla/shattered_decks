#ifndef FMT_CDROM_H
#define FMT_CDROM_H

#include <stdint.h>

/*
 * FM TOWNS CD-ROM sector reader for the *internal* CD-ROM controller
 * ("CDC"), addressed through I/O ports 0x4C0-0x4C8 (TOWNSIO_CDROM_*).
 *
 * This project boots via TOWNSEMU's "-CD image.iso" option (see
 * run.sh), which attaches the disc image to the internal CD-ROM device
 * (TownsCDROM/cdImgFName in TOWNSEMU/src/main_cui/argv/townsargv.cpp) -
 * a different, separate device from the general SCSI bus (which
 * FM/TOWNS/SCSILIB + FM/TOWNS/YSSCSICD's real DOS driver source talks
 * to, and which TOWNSEMU only populates via its debugger's SCSICDxLOAD
 * command, not by -CD). An initial SCSI-bus-based implementation here
 * was tested against this project's actual boot setup and never found
 * a device - IOMON showed the SCSI status port permanently reporting
 * "not busy", confirming nothing is listening on that bus in this
 * configuration. See scsi.[ch] for that driver, kept for a real
 * external/desktop-FM-TOWNS SCSI CD-ROM setup, but *not* exercised or
 * verified by this project's boot path - fmt_cdrom_read() here is the
 * one actually confirmed working (see below).
 *
 * Port map and CPU/PIO transfer command protocol reverse-engineered
 * from TOWNSEMU's CDC model (TOWNSEMU/src/towns/cdrom/cdrom.cpp,
 * TownsCDROM::IOWriteByte/IOReadByte/ExecuteCDROMCommand/
 * DelayedCommandExecution), which implements the FM TOWNS Technical
 * Databook pp.224-227 CDC command set:
 *
 *   0x4C0 Master control/status (read: DRY/STSF/DTSF/SIRQ/DEI bits)
 *   0x4C2 Command/status FIFO   (write: command byte; also status readback)
 *   0x4C4 Parameter/data        (write: command parameters; read: PIO
 *                                sector data once STSF is set)
 *   0x4C6 Transfer control      (write: 0x08 arms one sector of CPU/PIO
 *                                transfer, setting STSF once the CDC has
 *                                it ready)
 *
 * Protocol for a Mode 1 (2048 byte/sector) read of `count` sectors
 * starting at LBA `lba`, confirmed working end-to-end (booted, loaded
 * IMAGE.RAW+PALETTE.BIN via iso9660.c, rendered pixel-correct in
 * TOWNSEMU - see the fmt_iso9660_load() call site in main.c):
 *   1. Send the CD-ROM BIOS's undocumented setup command (0xA0 with the
 *      fixed parameters 08 01 00 00 00 00 00 00) and read its 4-byte
 *      status reply. Real hardware requires this before every read; see
 *      fmt_cdc_setup_read() below.
 *   2. Write 8 parameter bytes to 0x4C4: BCD-encoded start MSF (3 bytes),
 *      BCD-encoded end MSF (3 bytes, *inclusive* - the last sector to
 *      read), then 2 unused/zero filler bytes to reach the CDC's fixed
 *      8-byte parameter queue (PARAM_QUEUE_LEN in cdrom.cpp).
 *   3. Write the command byte (CDCMD_MODE1READ) to 0x4C2. That fires the
 *      already-parameterized request; the parameters must be in place
 *      first, which is how the CD-ROM BIOS and the hardware reference
 *      FM/TOWNS/EXPERIMENTS/CDREAD/CDREAD.ASM order it. Both this and the
 *      parameter writes are preceded by a wait for DRY (0x4C0 bit 0).
 *   4. Per sector: write 0x08 to 0x4C6 to arm CPU-transfer mode for that
 *      sector, poll 0x4C0 until the STSF bit (0x20) is set, then read
 *      2048 bytes one at a time from 0x4C4. The CDC clears STSF and
 *      CPUTransfer after each sector, so this arm-poll-read cycle repeats
 *      once per sector rather than once per whole request.
 *
 * MSF (minute:second:frame) addressing follows Red Book convention:
 * frame 0 of track 1's data starts at MSF 00:02:00 (150-frame/2-second
 * pregap), so LBA and MSF convert via lba = M*60*75+S*75+F-150.
 */

/* Reads `count` Mode-1 2048-byte sectors starting at LBA `lba` into
 * `buf` (must hold count*2048 bytes). Returns 0 on success, -1 if the
 * CDC never became ready (timeout - e.g. no disc). Blocking. */
int fmt_cdrom_read(uint32_t lba, uint16_t count, void *buf);

/*----------------------------------------------------------------------
 * Raw CDC command interface
 *
 * The sector reader above is one user of the CDC; CD-DA playback
 * (cdda.[ch]) is another, and it drives the same command/status
 * handshake with a completely different set of commands and no data
 * transfer at all. These are the shared primitives, so the port numbers
 * and the four-reads-per-status-entry rule live in exactly one place.
 *
 * Command bytes carry flags in their top bits (Table I-6-3): bit 7
 * TYPE (0 = PLAY command, 1 = STATE control command - already part of
 * the command codes as written, e.g. 0x84 CDDASTOP), bit 6 IRQ, bit 5
 * STATUS. Everything here polls, so pass FMT_CDC_FLAG_STATUS (to get a
 * status reply at all) and never FMT_CDC_FLAG_IRQ.
 *--------------------------------------------------------------------*/

#define FMT_CDC_FLAG_STATUS   0x20  /* Command status request */
#define FMT_CDC_FLAG_IRQ      0x40  /* Raise IRQ on status - unused here */

/* Drops any status left over from a previous command and clears its
 * interrupt flags. Call before issuing a command whose reply you intend
 * to match on. */
void fmt_cdc_drain_status(void);

/* Loads the 8-byte parameter FIFO and then writes the command byte, which
 * is what starts the command. Waits for DRY (the sub-MPU's "I can accept a
 * command" flag) before the FIFO and again before the command register,
 * because bytes written while it is clear are dropped. Does not wait for
 * the command to finish - poll for that with fmt_cdc_read_status(). */
void fmt_cdc_issue(uint8_t cmd, const uint8_t param[8]);

/* Issues the undocumented setup command the CD-ROM BIOS sends immediately
 * before every sector read (0xA0 with parameters 08 01 00 00 00 00 00 00)
 * and consumes its status reply. Returns 0, or -1 if no reply arrived.
 *
 * fmt_cdrom_read() and the streaming reader below do this for themselves;
 * it is exposed for anything else that drives a read command directly.
 * A read that skips it is accepted by the sub-MPU and then never delivers
 * a sector - a hang that emulators which do not model the requirement will
 * not show (Tsugaru only under -CDCSTRICT). */
int fmt_cdc_setup_read(void);

/* 1 if a status entry is waiting (SRQ set), 0 if not. Never blocks. */
int fmt_cdc_status_pending(void);

/* Reads one 4-byte status FIFO entry into `status`, waiting for it to
 * arrive. Returns 0, or -1 if it never did. status[0] is the status
 * code (see the CDSTAT/FMT_CDSTAT values used by cdrom.c/cdda.c). */
int fmt_cdc_read_status(uint8_t status[4]);

/* Acknowledges/clears SIRQ+DEI - see CD_ACK_SIRQ_DEI in cdrom.c for why
 * leaving them set wedges the drive's command state machine. */
void fmt_cdc_ack(void);

/*----------------------------------------------------------------------
 * Streaming reader
 *
 * fmt_cdrom_read() above blocks for the whole transfer, which is fine
 * when nothing else is going on but useless for playing audio off the
 * disc: the CD handshake takes milliseconds per sector, and any
 * millisecond not spent feeding the DAC is an audible hole. This is the
 * same protocol driven as a state machine that never waits for
 * anything - every call does a bounded amount of I/O and returns,
 * whatever the drive is or isn't ready for - so it can be advanced from
 * inside a sample-paced playback loop (see pcmstream.c) using time that
 * loop would otherwise spend idling on the YM2612 busy flag.
 *
 * It fills a caller-owned ring buffer, whose size must be a power of
 * two and a multiple of 2048, at least 2*FMT_CD_STREAM_RUN_SECTORS
 * sectors so a run can always be started while the previous data is
 * still being consumed. The caller owns the read cursor and passes it
 * to each step; the machine owns the write cursor (`fill_pos`). Both
 * count bytes since the start and only ever increase, so the number of
 * bytes available is `fill_pos - play_pos` and the byte at `play_pos`
 * lives at `ring[play_pos & ring_mask]`.
 *--------------------------------------------------------------------*/

/* Sectors per MODE1READ command. Bigger amortises the command handshake
 * over more data; the ring has to be able to hold a whole run twice
 * over, and the run must be drainable well inside the drive's ~100ms
 * lost-data timeout. */
#define FMT_CD_STREAM_RUN_SECTORS   8u

enum {
    FMT_CD_STREAM_IDLE,   /* between commands - will start a run when the ring has room */
    FMT_CD_STREAM_SETUP,  /* writing out the 9 bytes of the BIOS setup command */
    FMT_CD_STREAM_SETUP_WAIT, /* waiting on the setup command's status reply */
    FMT_CD_STREAM_ISSUE,  /* writing out the 9 command/parameter bytes */
    FMT_CD_STREAM_WAIT,   /* command in flight, waiting on the status FIFO */
    FMT_CD_STREAM_XFER,   /* a sector is ready, draining it into the ring */
    FMT_CD_STREAM_DONE,   /* end of a non-looping file */
    FMT_CD_STREAM_ERROR   /* the drive reported a failure */
};

typedef struct {
    uint8_t *ring;
    uint32_t ring_mask;      /* ring size - 1 */
    uint32_t fill_pos;       /* bytes deposited since the start */
    uint32_t lba;            /* first sector of the file */
    uint32_t size;           /* file length in bytes */
    uint32_t total_sectors;  /* sectors the file occupies */
    uint32_t sector;         /* next sector of the file to fetch */
    uint16_t byte_idx;       /* bytes taken so far from the sector in flight */
    uint8_t  cmd[9];         /* command byte + 8 params; params issue first */
                             /* (cmd_idx walks it as params 1..8 then byte 0) */
    uint8_t  cmd_idx;
    uint8_t  state;
    uint8_t  loop;
} fmt_cd_stream;

/* Prepares `st` to stream `size` bytes starting at LBA `lba` into
 * `ring`. With `loop` set, reaching the end of the file wraps back to
 * its start instead of finishing. Issues no I/O beyond clearing any
 * leftover drive status. */
void fmt_cdrom_stream_init(fmt_cd_stream *st, uint32_t lba, uint32_t size,
                           uint8_t *ring, uint32_t ring_size, int loop);

/* Advances the transfer by at most `budget` I/O operations and returns
 * how many payload bytes that added to the ring (0 whenever the step
 * went into command issue, status polling, or found the ring full).
 * `play_pos` is the caller's current read cursor. Never blocks; call it
 * as often as there is time for. */
unsigned fmt_cdrom_stream_step(fmt_cd_stream *st, uint32_t play_pos,
                               unsigned budget);

#endif
