/* icm_defs.h - geometry of the bootable IC Memory Card image.
 *
 * Shared by three things that all have to agree byte-for-byte:
 *
 *   - src/boot/icm_ipl.S, the IPL4 sector the boot ROM runs;
 *   - src/common/icm.c, the payload's card reader;
 *   - tools/mkicm.py, which lays the card image out.
 *
 * The card is memory, not a disk. Unlike the CD path (bootsect.S, which
 * asks the boot ROM's disk BIOS to read sectors) nothing here does any
 * I/O to fetch the game: the whole card is visible in the physical
 * address space, so the IPL just copies the payload out of it and the
 * payload reads its assets straight from it.
 *
 * Where the card appears depends on the CPU, not on the card:
 *
 *   386SX models (FM TOWNS UX, and the Marty this project targets) have
 *   a 24-bit physical address bus, so the card gets a 1MB window at
 *   0x00D00000 (TOWNSADDR_386SX_MEMCARD_BASE in TOWNSEMU's townsdef.h),
 *   and anything past the first megabyte is reached by writing a 1MB
 *   bank number to I/O 0x490.
 *
 *   386DX and later models map the card's first 16MB flat at
 *   0xC0000000 (TOWNSADDR_MEMCARD_OLD_BASE), with no banking needed for
 *   a card this size.
 *
 * A build picks one: the payload's read-only data is *linked* into card
 * space (src/boot/icm.lds), so the base address is baked into every
 * const pointer in the program and cannot be decided at run time.
 * Build with ICM_CARD_BASE=0xC0000000 for a 386DX-class machine.
 */

#ifndef FMT_ICM_DEFS_H
#define FMT_ICM_DEFS_H

#ifndef FMT_ICM_CARD_BASE
#define FMT_ICM_CARD_BASE	0x00D00000	/* 386SX/Marty window */
#endif

/* Size of the window the card is seen through, i.e. how much of the
 * card is addressable without touching the bank register. */
#if FMT_ICM_CARD_BASE >= 0xC0000000
#define FMT_ICM_WINDOW		0x01000000	/* 16MB, no banking */
#define FMT_ICM_BANK_SHIFT	24
#else
#define FMT_ICM_WINDOW		0x00100000	/* 1MB, banked */
#define FMT_ICM_BANK_SHIFT	20
#endif

/* I/O ports: [2] pp.794-795. 0x490 selects which slice of the card the
 * window shows; 0x491 bit 0 switches the window between the card's
 * attribute (CIS) memory and its common memory, and the boot ROM reads
 * the CIS on its way in, so the IPL puts it back to common memory
 * before trusting anything it reads. */
#define FMT_ICM_IO_BANK		0x490
#define FMT_ICM_IO_ATTRIB	0x491

/* Card image layout. Sector 0 is the IPL, exactly as on any other FM
 * TOWNS boot device; the payload image starts at the next sector
 * boundary. IC card sectors are 1024 bytes (see FM/TOWNS/IPL's
 * ICM_IPLM.ASM, which puts its second-stage loader at offset 1024), and
 * the boot ROM only guarantees that first sector is in memory when it
 * calls us - so the IPL has to fit in it. */
#define FMT_ICM_SECTOR_BYTES	1024
#define FMT_ICM_PAYLOAD_OFF	1024

/* Where the payload's RAM image is copied to and run from: the same
 * address the CD path loads to (TSTLOAD<<4 in defs.h), so head.S and
 * everything above it see an identical machine either way. */
#define FMT_ICM_LOAD_ADDR	0x00010000

/* Fields the packer patches into the IPL sector. Offsets 0x20/0x24 are
 * fixed by the UX/Marty boot ROM (it reads them as the disc's IO.SYS
 * extent - see docs/HOWFMTOWNS_BOOTS_FROM_CD.txt), so ours start after
 * them, mirroring bootsect.S's use of 0x28/0x2C. */
#define FMT_ICM_HDR_PAYLOAD_OFF		0x28	/* card offset of the payload image */
#define FMT_ICM_HDR_PAYLOAD_BYTES	0x2C	/* bytes of it to copy into RAM */
#define FMT_ICM_HDR_LOAD_ADDR		0x30	/* where to copy them */
#define FMT_ICM_HDR_ENTRY		0x34	/* payload entry point (startup_32) */
#define FMT_ICM_HDR_TOC_OFF		0x38	/* card offset of the asset directory */
#define FMT_ICM_HDR_CARD_BYTES		0x3C	/* total size of the card image */
#define FMT_ICM_HDR_CARD_BASE		0x40	/* card base this image was linked for */
#define FMT_ICM_HDR_END			0x44

/* Asset directory ("table of contents"): what the CD build gets from
 * ISO9660, in the smallest form that does the same job. Written by
 * tools/mkicm.py, read by fmt_icm_find(). Always inside the first
 * window's worth of the card, so it can be read without banking. */
#define FMT_ICM_TOC_MAGIC	0x544D4349u	/* "ICMT" */
#define FMT_ICM_TOC_NAME_LEN	12
#define FMT_ICM_TOC_ENTRY_BYTES	20		/* name[12] + offset + size */
#define FMT_ICM_TOC_HDR_BYTES	8		/* magic + count */

#endif
