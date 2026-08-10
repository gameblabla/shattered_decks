#include "scsi.h"
#include "io.h"

#define IO_1US_WAIT              0x06C

#define IO_SCSI_DATA              0xC30
#define IO_SCSI_STATUS            0xC32
#define IO_SCSI_COMMAND           0xC32

#define IO_DMA_INITIALIZE         0x0A0
#define IO_DMA_CHANNEL            0x0A1
#define IO_DMA_COUNT_LOW          0x0A2  /* word port */
#define IO_DMA_ADDR_LOW           0x0A4  /* word port */
#define IO_DMA_ADDR_MID_HIGH      0x0A6
#define IO_DMA_ADDR_HIGH          0x0A7
#define IO_DMA_DEVICE_CTRL_LOW    0x0A8
#define IO_DMA_MODE_CONTROL       0x0AA
#define IO_DMA_STATUS             0x0AB
#define IO_DMA_MASK               0x0AF

#define SCSI_STATUS_BUSY_BIT      0x08
#define SCSI_STATUS_REQ_BIT       0x80
#define SCSI_STATUS_PHASE_MASK    0x70
#define SCSI_PHASE_DATA_OUT       0x00
#define SCSI_PHASE_COMMAND        0x10
#define SCSI_PHASE_DATA_IN        0x40
#define SCSI_PHASE_STATUS         0x50
#define SCSI_PHASE_MESSAGE        0x70

#define SCSI_CMD_INQUIRY          0x12
#define SCSI_CMD_INQUIRY_LENGTH   8

#define SCSI_WAIT_LOOPS           3000

static inline void bus_1us_wait(void)
{
    outb(0, IO_1US_WAIT);
}

/* Returns 0 once BUSY (bit3) clears, -1 on timeout. */
static int scsi_wait_ready(void)
{
    for (int i = 0; i < SCSI_WAIT_LOOPS; i++) {
        if (0 == (inb(IO_SCSI_STATUS) & SCSI_STATUS_BUSY_BIT)) {
            return 0;
        }
        bus_1us_wait();
    }
    return -1;
}

/* Returns 0 once BUSY (bit3) sets (selection acknowledged), -1 on timeout. */
static int scsi_wait_busy(void)
{
    for (int i = 0; i < SCSI_WAIT_LOOPS; i++) {
        if (0 != (inb(IO_SCSI_STATUS) & SCSI_STATUS_BUSY_BIT)) {
            return 0;
        }
        bus_1us_wait();
    }
    return -1;
}

static void scsi_dma_initialize(void)
{
    outb(3, IO_DMA_INITIALIZE);       /* Reset DMA controller */
    outb(1, IO_DMA_CHANNEL);          /* Channel 1 = SCSI */
    outb(0x20, IO_DMA_DEVICE_CTRL_LOW); /* DMA enable */
}

static void scsi_select(uint8_t id)
{
    outb((uint8_t)(0x80 | (1u << id)), IO_SCSI_DATA);
    outb(0x86, IO_SCSI_COMMAND); /* WEN, SEL */
}

static void scsi_start_command_sequence(void)
{
    outb(0x82, IO_SCSI_COMMAND); /* WEN, DMAE - clears SEL, starts the sequence */
}

/*
 * Runs DMA bursts (each capped at the 64KB boundary the ISA DMA
 * controller can't cross) for as long as the target keeps driving the
 * DATA IN/OUT phase it entered with, advancing *addr as it goes.
 * `dirMode` is the DMA mode-control byte: 0x44 = I/O->memory (DATA IN),
 * 0x48 = memory->I/O (DATA OUT).
 */
static void scsi_exec_dma(uint8_t dirMode, uint32_t *addr)
{
    outb(dirMode, IO_DMA_MODE_CONTROL);

    for (;;) {
        uint32_t a = *addr;
        uint16_t burst = (uint16_t)(0xFFFFu - (a & 0xFFFFu));

        outw((uint16_t)(a & 0xFFFFu), IO_DMA_ADDR_LOW);
        outb((uint8_t)((a >> 16) & 0xFF), IO_DMA_ADDR_MID_HIGH);
        outb((uint8_t)((a >> 24) & 0xFF), IO_DMA_ADDR_HIGH);
        outw(burst, IO_DMA_COUNT_LOW);

        /* Unmask DMA channel 1 (clear bit1, keep the rest). */
        outb((uint8_t)(inb(IO_DMA_MASK) & 0x0D), IO_DMA_MASK);

        for (;;) {
            if (0 != (inb(IO_DMA_STATUS) & 0x02)) {
                break; /* Terminal count / DMA end for channel 1 */
            }
            uint8_t st = inb(IO_SCSI_STATUS);
            /* Stop polling once the target leaves Data In/Out phase
             * (or drops BUSY, i.e. the transfer ended early). */
            if (0 != ((st ^ SCSI_STATUS_BUSY_BIT) & 0x38)) {
                break;
            }
        }

        /* Mask DMA channel 1 back off. */
        outb((uint8_t)((inb(IO_DMA_MASK) & 0x0F) | 0x02), IO_DMA_MASK);

        a = (a + 0x10000u) & 0xFFFF0000u; /* advance to the next 64KB segment */
        *addr = a;

        uint8_t st = inb(IO_SCSI_STATUS);
        if (0 != ((st ^ SCSI_STATUS_BUSY_BIT) & 0x38)) {
            break; /* No longer in a data phase (or bus went idle) - done. */
        }
    }
}

int scsi_command(uint8_t id, const uint8_t cdb[10], void *buf,
                  uint8_t *status, uint8_t *message)
{
    uint32_t addr = (uint32_t)(uintptr_t)buf;
    int cmdIndex = 0;
    uint8_t st = 0, msg = 0;

    if (scsi_wait_ready() != 0) {
        return -1;
    }

    scsi_dma_initialize();
    scsi_select(id);

    if (scsi_wait_busy() != 0) {
        return -1;
    }

    scsi_start_command_sequence();

    for (;;) {
        uint8_t sr = inb(IO_SCSI_STATUS);
        if (0 == (sr & SCSI_STATUS_BUSY_BIT)) {
            break; /* BUSFREE - target finished the sequence. */
        }
        if (0 == (sr & SCSI_STATUS_REQ_BIT)) {
            continue;
        }

        switch (sr & SCSI_STATUS_PHASE_MASK) {
        case SCSI_PHASE_COMMAND:
            outb(cmdIndex < 10 ? cdb[cmdIndex] : 0, IO_SCSI_DATA);
            cmdIndex++;
            break;
        case SCSI_PHASE_DATA_IN:
            scsi_exec_dma(0x44, &addr);
            break;
        case SCSI_PHASE_DATA_OUT:
            scsi_exec_dma(0x48, &addr);
            break;
        case SCSI_PHASE_STATUS:
            st = (uint8_t)inb(IO_SCSI_DATA);
            break;
        case SCSI_PHASE_MESSAGE:
            msg = (uint8_t)inb(IO_SCSI_DATA);
            break;
        default:
            break;
        }
    }

    if (status) {
        *status = st;
    }
    if (message) {
        *message = msg;
    }
    return 0;
}

int scsi_find_cdrom(void)
{
    static uint8_t inquiry_buf[SCSI_CMD_INQUIRY_LENGTH];
    uint8_t cdb[10] = { SCSI_CMD_INQUIRY, 0, 0, 0, SCSI_CMD_INQUIRY_LENGTH, 0, 0, 0, 0, 0 };

    for (int id = 7; id >= 0; id--) {
        uint8_t status = 0, message = 0;

        for (int i = 0; i < SCSI_CMD_INQUIRY_LENGTH; i++) {
            inquiry_buf[i] = 0;
        }

        if (scsi_command((uint8_t)id, cdb, inquiry_buf, &status, &message) != 0) {
            continue;
        }
        if (status != 0) {
            continue;
        }
        if (inquiry_buf[0] == 0x05 || inquiry_buf[0] == 0x04) {
            return id;
        }
    }
    return -1;
}
