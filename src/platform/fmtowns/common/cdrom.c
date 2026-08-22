#include "cdrom.h"
#include "io.h"

#define CD_MASTER_CTRL_STATUS   0x4C0
#define CD_COMMAND_STATUS       0x4C2
#define CD_PARAMETER_DATA       0x4C4
#define CD_TRANSFER_CTRL        0x4C6

/* Mask status register (read 0x4C0) - FM TOWNS Technical Databook,
 * Table I-6-2. Note what DRY actually means: "SUB MPU READY - 0: sub-MPU
 * state cannot accept commands, 1: sub-MPU state can accept commands".
 * It is *not* a data-ready signal, and it is deliberately low for the
 * whole duration of a read command (the drive is busy). Waiting for it
 * after issuing MODE1READ - which this file used to do - can therefore
 * only ever be satisfied by the drive aborting the command on its
 * lost-data timeout (~100ms per sector), which is exactly what was
 * happening: every sector cost a full timeout/abort cycle. The real
 * per-sector handshake is SRQ + the status FIFO below. */
#define CD_STATUS_DRY            0x01  /* Sub-MPU ready to accept a command byte / parameter */
#define CD_STATUS_SRQ            0x02  /* Status read request: 4 status bytes waiting in the FIFO */
#define CD_STATUS_STSF           0x20  /* Software (PIO) transfer in progress */

/* Write-only "wait about one microsecond" register (I/O 0x6C, present
 * since the TOWNS 20F, so on every model this port targets).  The CD-ROM
 * BIOS - and CaptainYS's hardware-verified FM/TOWNS/EXPERIMENTS/CDREAD/
 * CDREAD.ASM, which this driver's sequence is modelled on - spaces the
 * parameter-FIFO writes with it.  Back-to-back OUTs on a fast machine put
 * them ~450ns apart, well under what the reference code ever produces. */
#define CD_TIMER_1US_WAIT        0x6C

/* Written to CD_MASTER_CTRL_STATUS to acknowledge/clear SIRQ (bit7,
 * SMIC) and DEI (bit6, DEIC) after handling a sector's transfer
 * completion - see TOWNSEMU's TownsCDROM::IOWriteByte for 0x4C0. Real
 * drivers always do this; leaving DEI/SIRQ set after the first sector
 * left the CD-ROM device's internal command state machine stuck waiting
 * on an unconsumed IRQ (see the "Initial State" gate: `if(IRR && DEI)
 * reschedule, don't post data-ready`) before it would post readiness for
 * the next sector. The low nibble is 0, which also leaves SMIM/DEIM
 * (the interrupt enables) off - this driver polls. */
#define CD_ACK_SIRQ_DEI          0xC0

/* Command register (write 0x4C2), Table I-6-3: bit7 TYPE, bit6 IRQ,
 * bit5 STATUS (command status request), bits 0-4 command code. We want
 * status replies but no interrupts, so STATUS is set and IRQ is not. */
#define CDCMD_MODE1READ           0x02
#define CDCMD_FLAG_STATUS         0x20

/* The undocumented setup command the CD-ROM BIOS issues immediately before
 * every sector read, with the fixed parameters 08 01 00 00 00 00 00 00.
 * Its effect is not documented anywhere - the Technical Databook declines
 * to publish the CDC command set at all ("please use the CD-ROM BIOS") -
 * but tracing a Marty boot ROM's own CDC traffic shows 36 of them, one
 * paired with each read, and no driver known to work on real hardware
 * skips it.  A bare MODE1READ is accepted (status 00H) and then never
 * delivers a sector, which is exactly the hang this port had on hardware
 * while running fine under emulators that do not model the requirement.
 * 0xA0 already carries the STATUS flag (bit 5); the IRQ flag stays clear. */
#define CDCMD_SETUP               0xA0

/* First byte of a 4-byte status FIFO reply. */
#define CDSTAT_NO_ERROR           0x00  /* Command accepted */
#define CDSTAT_READ_DONE          0x06  /* All requested sectors transferred */
/* 0x22 is the generic/DMA data-ready status.  A PIO transfer can report
 * 0x21 instead; MAME does so for every sector after the first, while
 * TOWNSEMU reports 0x22.  Both mean the same thing to this polled reader. */
#define CDSTAT_DATA_READY_DMA     0x22
#define CDSTAT_DATA_READY_PIO     0x21

#define CD_SECTOR_SIZE            2048u
#define CD_MSF_PREGAP_FRAMES       150u /* 00:02:00 */
#define CD_FRAMES_PER_SEC           75u
#define CD_SECS_PER_MIN             60u

/* Generous spin-count timeouts - there's no reliable timer interrupt to
 * bound these by wall-clock time in this freestanding environment, so we
 * just cap the number of polls to avoid hanging forever on a missing/
 * stuck drive. */
#define CD_POLL_LIMIT         10000000

static inline uint8_t bcd8(unsigned v)
{
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

static void lba_to_bcd_msf(uint32_t lba, uint8_t *m, uint8_t *s, uint8_t *f)
{
    uint32_t frames = lba + CD_MSF_PREGAP_FRAMES;
    unsigned min = frames / (CD_SECS_PER_MIN * CD_FRAMES_PER_SEC);
    unsigned sec = (frames / CD_FRAMES_PER_SEC) % CD_SECS_PER_MIN;
    unsigned frm = frames % CD_FRAMES_PER_SEC;
    *m = bcd8(min);
    *s = bcd8(sec);
    *f = bcd8(frm);
}

static int wait_status_bit(uint8_t bit)
{
    for (int i = 0; i < CD_POLL_LIMIT; i++) {
        if (inb(CD_MASTER_CTRL_STATUS) & bit) {
            return 0;
        }
    }
    return -1;
}

/* Read one 4-byte status FIFO entry (Table I-6-5: "the same address must
 * be read four times"), returning its first byte - the status code - or
 * -1 if none arrived. */
static int read_status(void)
{
    uint8_t status[4];

    if (fmt_cdc_read_status(status) != 0) {
        return -1;
    }
    return (int)status[0];
}

/*----------------------------------------------------------------------
 * Raw CDC command interface - see cdrom.h.
 *--------------------------------------------------------------------*/

int fmt_cdc_status_pending(void)
{
    return (inb(CD_MASTER_CTRL_STATUS) & CD_STATUS_SRQ) != 0;
}

int fmt_cdc_read_status(uint8_t status[4])
{
    if (wait_status_bit(CD_STATUS_SRQ) != 0) {
        return -1;
    }
    for (int i = 0; i < 4; i++) {
        status[i] = (uint8_t)inb(CD_COMMAND_STATUS);
    }
    return 0;
}

void fmt_cdc_ack(void)
{
    outb(CD_ACK_SIRQ_DEI, CD_MASTER_CTRL_STATUS);
}

/* Drop any status left over from a previous command and clear its
 * interrupt flags, so the status codes we match on below are certain to
 * belong to the command we are about to issue. */
void fmt_cdc_drain_status(void)
{
    for (int i = 0; i < 64 && (inb(CD_MASTER_CTRL_STATUS) & CD_STATUS_SRQ); i++) {
        (void)inb(CD_COMMAND_STATUS);
    }
    fmt_cdc_ack();
}

void fmt_cdc_issue(uint8_t cmd, const uint8_t param[8])
{
    /* The CDC parameter FIFO is loaded before the command is fired.  This is
     * the order used by the TBIOS-compatible hardware reference: the command
     * register starts execution using the eight parameters already queued.
     * TOWNSEMU also models this pairing, and MAME executes immediately on the
     * command-register write, so writing the command first makes it consume
     * stale parameters there.
     *
     * DRY (4C0H bit 0) means "the sub-MPU can accept a command"; a byte
     * written while it is clear is simply dropped.  CDREAD.ASM waits for it
     * once before loading the FIFO and again before the command register,
     * and so do we.  Note this is the only place waiting on DRY is correct:
     * it stays clear for the whole duration of a read, so polling it after
     * MODE1READ - which this file used to do - can only be satisfied by the
     * drive giving up on its lost-data timeout. */
    (void)wait_status_bit(CD_STATUS_DRY);
    for (int i = 0; i < 8; i++) {
        outb(param[i], CD_PARAMETER_DATA);
        outb(0, CD_TIMER_1US_WAIT);
    }
    (void)wait_status_bit(CD_STATUS_DRY);
    outb(cmd, CD_COMMAND_STATUS);
}

/* CDCMD_SETUP and the fixed parameters the CD-ROM BIOS pairs with it - see
 * its definition.  Laid out command-byte-first like fmt_cd_stream::cmd, so
 * the streaming issuer can dribble either one out with the same code. */
static const uint8_t g_setup_cmd[9] = { CDCMD_SETUP, 0x08, 0x01, 0, 0, 0, 0, 0, 0 };

int fmt_cdc_setup_read(void)
{
    uint8_t status[4];

    fmt_cdc_issue(g_setup_cmd[0], &g_setup_cmd[1]);
    /* The command carries STATUS but not IRQ, so it posts a status entry
     * without raising SIRQ: read the entry and the drive is ready for the
     * read command, with nothing left over for the read's own handshake to
     * trip over.  No acknowledge is needed (and CDREAD.ASM issues none).
     *
     * The reply itself is waited for but not judged: the setup command
     * reports drive state, so the first one after a disc change answers
     * "media changed" (21 08 00 00) followed by a clear 00 - an answer, not
     * a failure.  Drain whatever it queued so the read that follows sees
     * only its own status codes. */
    if (fmt_cdc_read_status(status) != 0) {
        return -1;
    }
    for (int i = 0; i < 8 && fmt_cdc_status_pending(); i++) {
        (void)fmt_cdc_read_status(status);
    }
    return 0;
}

/* Fills in the 9 bytes a MODE1READ takes: the command byte followed by
 * the 8-byte parameter FIFO. The end MSF is the last sector to read,
 * *inclusive* (the drive transfers sectors while readingSector <=
 * endSector). Passing lba + count made the drive expect one more sector
 * than we then drained, leaving the command half-finished and its
 * lost-data timer armed across the next call. */
static void build_read_command(uint32_t lba, uint16_t count, uint8_t cmd[9])
{
    cmd[0] = CDCMD_MODE1READ | CDCMD_FLAG_STATUS;
    lba_to_bcd_msf(lba, &cmd[1], &cmd[2], &cmd[3]);
    lba_to_bcd_msf(lba + count - 1u, &cmd[4], &cmd[5], &cmd[6]);
    cmd[7] = 0;
    cmd[8] = 0;
}

int fmt_cdrom_read(uint32_t lba, uint16_t count, void *buf)
{
    uint8_t *dst = (uint8_t *)buf;
    uint8_t cmd[9];

    if (count == 0) {
        return 0;
    }

    fmt_cdc_drain_status();
    /* Every read is preceded by the CD-ROM BIOS's setup command; without it
     * real hardware accepts MODE1READ and then hands over nothing. */
    if (fmt_cdc_setup_read() != 0) {
        return -1;
    }
    build_read_command(lba, count, cmd);
    fmt_cdc_issue(cmd[0], &cmd[1]);

    /* Per-sector handshake: the drive posts 0x22 (data ready) into the
     * status FIFO when a sector is available, we enable software (CPU)
     * transfer and drain 2048 bytes, then acknowledge - after which it
     * posts the next 0x22, and finally 0x06 (read done). 0x00 is just
     * "command accepted", emitted once up front, and is skipped here;
     * anything else is an error (0x21 = abnormal termination). */
    for (uint16_t sec = 0; sec < count; ) {
        int status = read_status();
        if (status == CDSTAT_NO_ERROR) {
            continue;
        }
        if (status != CDSTAT_DATA_READY_DMA && status != CDSTAT_DATA_READY_PIO) {
            return -1;
        }

        outb(0x08, CD_TRANSFER_CTRL); /* STS=1, DTS=0: software transfer. */
        if (wait_status_bit(CD_STATUS_STSF) != 0) {
            return -1;
        }
        for (uint32_t i = 0; i < CD_SECTOR_SIZE; i++) {
            *dst++ = (uint8_t)inb(CD_PARAMETER_DATA);
        }
        outb(CD_ACK_SIRQ_DEI, CD_MASTER_CTRL_STATUS);
        sec++;
    }

    /* Consume the closing "read done" status so the drive is back to
     * accepting commands with an empty FIFO for the next call. */
    if (read_status() != CDSTAT_READ_DONE) {
        return -1;
    }
    outb(CD_ACK_SIRQ_DEI, CD_MASTER_CTRL_STATUS);

    return 0;
}

/*----------------------------------------------------------------------
 * Non-blocking streaming reader - see cdrom.h for the contract.
 *--------------------------------------------------------------------*/

void fmt_cdrom_stream_init(fmt_cd_stream *st, uint32_t lba, uint32_t size,
                           uint8_t *ring, uint32_t ring_size, int loop)
{
    st->ring = ring;
    st->ring_mask = ring_size - 1u;
    st->fill_pos = 0;
    st->lba = lba;
    st->size = size;
    st->total_sectors = (size + CD_SECTOR_SIZE - 1u) / CD_SECTOR_SIZE;
    st->sector = 0;
    st->byte_idx = 0;
    st->cmd_idx = sizeof st->cmd; /* nothing to issue yet */
    st->state = FMT_CD_STREAM_IDLE;
    st->loop = (uint8_t)(loop ? 1 : 0);
    fmt_cdc_drain_status();
}

unsigned fmt_cdrom_stream_step(fmt_cd_stream *st, uint32_t play_pos,
                               unsigned budget)
{
    if (budget == 0) {
        return 0;
    }

    switch (st->state) {
    case FMT_CD_STREAM_IDLE: {
        uint32_t pending = st->fill_pos - play_pos;
        uint32_t freebytes = (st->ring_mask + 1u) - pending;
        uint32_t run;

        if (st->sector >= st->total_sectors) {
            if (!st->loop) {
                st->state = FMT_CD_STREAM_DONE;
                return 0;
            }
            st->sector = 0;
        }

        /* A run never crosses the end of the file, so the drive's
         * "read done" always lines up with a file boundary and the
         * wrap above is the only place looping happens. */
        run = st->total_sectors - st->sector;
        if (run > FMT_CD_STREAM_RUN_SECTORS) {
            run = FMT_CD_STREAM_RUN_SECTORS;
        }

        /* Only start a run once the whole of it is guaranteed to fit.
         * Once the drive posts a sector as ready it arms a lost-data
         * timeout, so there must never be a point where we have to
         * stop draining and wait for the player to catch up. */
        if (freebytes < run * CD_SECTOR_SIZE) {
            return 0;
        }

        build_read_command(st->lba + st->sector, (uint16_t)run, st->cmd);
        st->cmd_idx = 0;
        st->state = FMT_CD_STREAM_SETUP;
        return 0;
    }

    case FMT_CD_STREAM_SETUP:
    case FMT_CD_STREAM_ISSUE: {
        /* Dribbled out a few bytes per call rather than all nine at
         * once: this runs inside the caller's sample-pacing loop, and
         * nine back-to-back I/O writes is enough work to push a sample
         * late.
         *
         * Both the CD-ROM BIOS setup command (SETUP) and the read itself
         * (ISSUE) go out this way: eight parameter bytes, then the command
         * byte that fires them.  A byte written while DRY is clear would be
         * dropped, so stop and come back rather than losing it - the state
         * machine is allowed to make no progress on any given call. */
        const uint8_t *cmd = (st->state == FMT_CD_STREAM_SETUP)
                             ? g_setup_cmd : st->cmd;
        unsigned n = 9u - st->cmd_idx;

        if (n > budget) {
            n = budget;
        }
        for (unsigned i = 0; i < n; i++) {
            if (!(inb(CD_MASTER_CTRL_STATUS) & CD_STATUS_DRY)) {
                return 0;
            }
            if (st->cmd_idx < 8u) {
                outb(cmd[st->cmd_idx + 1u], CD_PARAMETER_DATA);
                outb(0, CD_TIMER_1US_WAIT);
            } else {
                outb(cmd[0], CD_COMMAND_STATUS);
            }
            st->cmd_idx++;
        }
        if (st->cmd_idx >= 9u) {
            st->state = (st->state == FMT_CD_STREAM_SETUP)
                        ? FMT_CD_STREAM_SETUP_WAIT : FMT_CD_STREAM_WAIT;
        }
        return 0;
    }

    case FMT_CD_STREAM_SETUP_WAIT: {
        /* The setup command's reply is consumed, not judged - see
         * fmt_cdc_setup_read(). */
        if (!(inb(CD_MASTER_CTRL_STATUS) & CD_STATUS_SRQ)) {
            return 0;
        }
        for (int i = 0; i < 4; i++) {
            (void)inb(CD_COMMAND_STATUS);
        }
        st->cmd_idx = 0;
        st->state = FMT_CD_STREAM_ISSUE;
        return 0;
    }

    case FMT_CD_STREAM_WAIT: {
        int code;
        if (!(inb(CD_MASTER_CTRL_STATUS) & CD_STATUS_SRQ)) {
            return 0; /* nothing yet - come back next call */
        }
        code = (int)inb(CD_COMMAND_STATUS);
        (void)inb(CD_COMMAND_STATUS);
        (void)inb(CD_COMMAND_STATUS);
        (void)inb(CD_COMMAND_STATUS);
        if (code == CDSTAT_DATA_READY_DMA || code == CDSTAT_DATA_READY_PIO) {
            outb(0x08, CD_TRANSFER_CTRL); /* STS=1, DTS=0: software transfer. */
            st->byte_idx = 0;
            st->state = FMT_CD_STREAM_XFER;
        } else if (code == CDSTAT_READ_DONE) {
            outb(CD_ACK_SIRQ_DEI, CD_MASTER_CTRL_STATUS);
            st->state = FMT_CD_STREAM_IDLE;
        } else if (code != CDSTAT_NO_ERROR) {
            st->state = FMT_CD_STREAM_ERROR;
        }
        return 0;
    }

    case FMT_CD_STREAM_XFER: {
        uint32_t base = st->sector * CD_SECTOR_SIZE;
        unsigned n = CD_SECTOR_SIZE - st->byte_idx;
        unsigned i;

        if (n > budget) {
            n = budget;
        }
        for (i = 0; i < n; i++) {
            uint8_t b = (uint8_t)inb(CD_PARAMETER_DATA);
            /* Every byte of the sector has to be pulled out of the CDC
             * to finish the transfer, but a final short sector carries
             * padding past the end of the file - keep the ring holding
             * nothing but real audio so looping wraps seamlessly. */
            if (base + st->byte_idx + i < st->size) {
                st->ring[st->fill_pos & st->ring_mask] = b;
                st->fill_pos++;
            }
        }
        st->byte_idx = (uint16_t)(st->byte_idx + n);
        if (st->byte_idx >= CD_SECTOR_SIZE) {
            outb(CD_ACK_SIRQ_DEI, CD_MASTER_CTRL_STATUS);
            st->sector++;
            st->state = FMT_CD_STREAM_WAIT;
        }
        return n;
    }

    default: /* DONE / ERROR */
        return 0;
    }
}
