#ifndef FMT_SCSI_H
#define FMT_SCSI_H

#include <stdint.h>

/*
 * FM TOWNS internal SCSI bus driver: this is what the real machine's
 * CD-ROM drive sits on (there is no separate "CDC" controller on real
 * hardware - that's TOWNSEMU's internal simplification). Ported from
 * FM/TOWNS/SCSILIB/SCSIIO.ASM (Soji Yamakawa / YSSCSICD, the real DOS
 * CD-ROM driver's SCSI layer) into C, onto this project's port I/O
 * macros (src/boot/io.h) instead of raw ASM IN/OUT.
 *
 * Bus protocol (SCSIIO.ASM's own summary, kept verbatim since it's the
 * whole picture in a few lines):
 *   1. Wait for the controller to be ready (not BUSY).
 *   2. Initialize DMA channel 1 (the SCSI channel).
 *   3. Select the target: write (0x80 | (1<<id)) to the data port, then
 *      WEN|SEL (0x86) to the command port.
 *   4. Wait for the controller to raise BUSY (selection acknowledged).
 *   5. Clear SEL (write WEN|DMAE = 0x82) to start the command sequence.
 *   6. Loop while BUSY: on REQ, act on the current bus phase - COMMAND
 *      writes one CDB byte, DATA IN/OUT runs one DMA burst capped at a
 *      64KB boundary since the ISA DMA controller can't cross one,
 *      MESSAGE/STATUS latches one byte; phase encoding is bits[6:4] of
 *      the status port (0x1=COMMAND, 0x4=DATA_IN, 0x0=DATA_OUT,
 *      0x5=STATUS, 0x7=MESSAGE).
 *   7. BUSY clears (BUSFREE) -> done; report the latched STATUS byte.
 *
 * `buf` for scsi_command() must be a *physical* address reachable by
 * the (24-bit-addressed-in-practice) ISA DMA controller - true of any
 * ordinary pointer in this project's flat, identity-mapped 32-bit
 * unreal/protected-mode environment (see fmt_pixel.h's comment on the
 * same property).
 */

/* Probes SCSI IDs 7 down to 0 with INQUIRY, looking for a device
 * reporting peripheral type 0x05 (CD-ROM) or 0x04 (WORM, also used by
 * some Towns CD-ROM drives) - same order/acceptance test as YSSCSICD's
 * SCAN_SCSI_CDROM. Returns the SCSI ID (0-7) on success, -1 if none
 * found. */
int scsi_find_cdrom(void);

/* Issues a 10-byte CDB to target `id`, transferring data to/from `buf`
 * via DMA as directed by the bus phase the target drives (works for
 * both DATA IN and DATA OUT commands - the target picks the direction,
 * this just follows it). Returns 0 and sets `status`/`message` from the
 * target's STATUS/MESSAGE phase bytes on a clean bus sequence, -1 on a
 * bus-level timeout (target never went BUSY, etc). A 0 return with a
 * nonzero *status still means the command itself failed (SCSI check
 * condition etc.) - callers should check *status too. */
int scsi_command(uint8_t id, const uint8_t cdb[10], void *buf,
                  uint8_t *status, uint8_t *message);

#endif
