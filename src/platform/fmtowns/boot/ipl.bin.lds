/*
 * Linker script for IPL.BIN: the FM TOWNS CD boot sector (bootsect.S)
 * plus setup.S, and nothing else.
 *
 * This is the whole of what gets injected into the ISO9660 System Area
 * (LBA 0-15) by tools/inject_ipl.py, and the boot ROM only ever preloads
 * the first IPL_PRELOAD_BYTES (4 sectors = 8192 bytes) of the disc, so
 * the image must stay under that. tools/inject_ipl.py enforces it.
 *
 * No padding to 8192 and no appended copy of the payload: the payload
 * (mygame_shared.bin) travels as an ordinary ISO9660 file, CD/SYSTEM.BIN,
 * and bootsect.S reads it from the LBA patched into its own header. The
 * old combined BOOT.BIN concatenated a second copy of the payload after
 * an 8192-byte pad and handed the lot to `mkisofs -G`, which silently
 * truncates its boot image to the 32768-byte System Area - so ~91KB of
 * that file never reached the disc at all, and the part that did was a
 * duplicate nothing read.
 */

OUTPUT_FORMAT("binary")
OUTPUT_ARCH("i386")

ENTRY(_main);
SECTIONS {
	. = 0;
	.bootsect : { *(.bootsect) }
	.setup : { *(.setup) }

	/DISCARD/ : { *(.note*) *(.comment) }
}
