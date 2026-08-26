/*
 * Linker script for the CD payload -- a plain static link at a fixed
 * address.
 *
 * This used to be a -shared -Bsymbolic PIC object that relocated itself at
 * startup through boot/reloc.c, on the theory that the payload should not
 * care where the loader put it.  It always cared: bootsect.S loads it at
 * TSTLOAD (0x1000 << 4 = 0x10000) and nowhere else, so the whole mechanism
 * bought nothing and cost a lot on a 386SX --
 *
 *   * -fPIC reserves %ebx as the GOT pointer.  A 386 has eight registers,
 *     and the rasterizer's inner loops (renderer3d_spans.inc) are written
 *     against every one of them.
 *   * every access to a global goes through a GOT indirection.
 *   * -Bsymbolic makes every global an export, which makes it a
 *     --gc-sections root, which is why 190 KB of unreferenced PCM stayed in
 *     the image (see src/game/sounds.c).
 *
 * boot/icm.lds.S has linked the IC-card build this way from the start; this
 * is the same arrangement for the disc.  head.S skips the self-relocation
 * call when FMT_NO_RELOC is defined, which Makefile.fmtowns now does.
 */

OUTPUT_FORMAT("elf32-i386");
OUTPUT_ARCH(i386);

ENTRY(startup_32);
SECTIONS {
	/* Must match TSTLOAD<<4 in boot/defs.h and PAYLOAD_LOAD in
	 * Makefile.fmtowns: the image is loaded here and never moved. */
	. = 0x10000;
	.text : {
		_start = .;
		*(.text)
		*(.text.*)
		*(.plt)
		_etext = . ;
	} = 0x9090
	.rodata : {
		*(.rodata)
		*(.rodata.*)
	}

	. = ALIGN(4);
	.data : {
		 _data = .;
		*(.data)
		*(.data.*)
		*(.data.rel)
		*(.data.rel.*)
	}
	.got : {
		*(.got.plt)
		*(.got)
		_edata = . ;
	}
	/*
	 * .bss lives in extended RAM, not in the loaded image.
	 *
	 * The payload is loaded at 0x10000 (boot/defs.h TSTLOAD) and its
	 * loaded part -- .text/.rodata/.data/.got -- has to stay below
	 * 0x90000, where bootsect.S relocated itself and its stack.  .bss
	 * does not: it is NOBITS (nothing is read off CD for it, head.S
	 * just zeroes it), so it can be parked anywhere there is RAM.
	 * 0x100000 is the start of extended memory, which on a 2 MB Marty
	 * gives it the whole 0x100000-0x200000 megabyte to grow into
	 * instead of fighting the image for what is left under 0xC0000.
	 * The gap costs nothing in the binary (objcopy drops NOBITS).
	 *
	 * Makefile.fmtowns checks both ceilings after the link.
	 */
	. = 0x100000;
	.bss (NOLOAD) : {
		_bss = .;
		*(.dynbss)
		*(.bss)
		*(.bss.*)
		*(COMMON)
		/* _end must be at least 256 byte aligned */
		. = ALIGN(256);
		_end = .;
	}
	/DISCARD/ : { *(*) }
}
