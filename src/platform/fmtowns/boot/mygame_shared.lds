OUTPUT_FORMAT("elf32-i386");
OUTPUT_ARCH(i386);

ENTRY(startup_32); 
SECTIONS {
	. = 0;
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
	.dynsym     : { *(.dynsym) }
	.dynstr     : { *(.dynstr) }
	.hash       : { *(.hash) }
	.gnu.hash   : { *(.gnu.hash) }
	.dynamic    : { *(.dynamic) }

	.rel.text    : { *(.rel.text   .rel.text.*) }
	.rel.rodata  : { *(.rel.rodata .rel.rodata.*) }
	.rel.data    : { *(.rel.data   .rel.data.*) }
	.rel.got     : { *(.rel.got    .rel.got.*) }
	.rel.plt     : { *(.rel.plt    .rel.plt.*) }

	. = ALIGN(4);
	.data : {
		 _data = .; 
		*(.data) 
		*(.data.*) 
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
	 * 0xC0000, where the FMR VRAM window starts.  .bss does not: it is
	 * NOBITS (nothing is read off CD for it, head.S just zeroes it), so
	 * it can be parked anywhere there is RAM.  Skipping to link address
	 * 0xF0000 puts it at 0xF0000 + 0x10000 = 0x100000 at run time, i.e.
	 * the start of extended memory, which on a 2 MB Marty gives it the
	 * whole 0x100000-0x200000 megabyte to grow into instead of fighting
	 * the image for what is left under 0xC0000.  The gap costs nothing
	 * in the binary (objcopy drops NOBITS) and nothing at relocation
	 * time (reloc.c applies one uniform load bias to every address).
	 *
	 * Makefile.fmtowns checks both ceilings after the link.
	 */
	. = 0xF0000;
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
