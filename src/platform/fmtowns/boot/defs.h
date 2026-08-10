/* defs.h - assembler/compiler definitions for the bare-metal CD boot path.
 *
 * Adapted from fmtowns_playground/defs.h (itself derived from MemTest-86's
 * defs.h). The floppy-specific SETUPSECS/track-geometry machinery is gone;
 * this build boots straight from an El Torito-less, FM TOWNS "IPL4" boot
 * sector embedded at the front of the CD image (see src/boot/bootsect.S).
 */

/*
 * Final (protected-mode) load address used as an ESP fallback in head.S.
 * Not otherwise load-bearing since our loader always loads the "system"
 * payload at a fixed, known address (TSTLOAD below).
 */
#define LOW_TEST_ADR	0x00002000

#define BOOTSEG		0xB000			/* Segment the boot ROM preloads the IPL to */
#define INITSEG		0x9000			/* Segment the IPL relocates itself to */
#define SETUPSEG	(INITSEG+0x20)		/* Segment of setup.S, right after bootsect.S */
#define TSTLOAD		0x1000			/* Segment the 32-bit payload (mygame_shared.bin) is loaded to */

#define KERNEL_CS	0x10			/* 32 bit flat code selector */
#define KERNEL_DS	0x18			/* 32 bit flat data selector */
#define REAL_CS		0x20			/* 16 bit code selector (unused by this boot path, kept for setup.S compat) */
#define REAL_DS		0x28			/* 16 bit data selector (unused by this boot path, kept for setup.S compat) */

/* Number of bytes the FM TOWNS boot ROM preloads at BOOTSEG (0xB0000)
 * before transferring control to the IPL: 4 CD-ROM sectors * 2048 bytes.
 * Our combined bootsect+setup code must fit within this, since it runs
 * with no further disk I/O until it explicitly reads the "system" payload.
 */
#define IPL_PRELOAD_BYTES	8192

/* setup.S pads itself to this many 1024-byte units; kept only for that
 * padding math (mirrors fmtowns_playground/defs.h). */
#define SETUPSECS	2

/* FM TOWNS I/O ports (GVRAM control, from captainys's FD_IPL / defs.h) */
#define	IO_FMR_GVRAMMASK		0x0FF81
#define	IO_FMR_GVRAMDISPMODE		0x0FF82
#define	IO_FMR_GVRAMPAGESEL		0x0FF83
#define	IO_KVRAM_OR_ANKFONT		0x0FF99
#define	IO_FMR_VRAM_OR_MAINRAM		0x0404
