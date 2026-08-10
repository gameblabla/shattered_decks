/*
 * Linker script for the IC Memory Card payload. Preprocessed into
 * build/icm.lds so it can share icm_defs.h with the IPL and the packer.
 *
 * The point of this file is the split between what lives in RAM and
 * what lives on the card:
 *
 *   .text .data .got	VMA in main RAM at 0x10000, LMA in card space.
 *			The IPL copies this range - and only this range -
 *			out of the card before jumping to it, exactly as
 *			the CD build's IPL copies it off the disc.
 *
 *   .rodata		VMA *and* LMA in card space. Never copied, never
 *			relocated: every `const` in the program resolves
 *			to an address inside the IC card's window, and
 *			reading it reads the card. That is the whole
 *			reason this build exists - a 64KB const table or
 *			a megabyte of embedded artwork costs no RAM at
 *			all, and the card physically cannot be written
 *			through these addresses without going out of the
 *			way to do so (and not at all with the card's
 *			write-protect switch on, or with a mask-ROM or
 *			FRAM card).
 *
 *   .bss		RAM, after .data. Not in the image.
 *
 * Everything on the card is addressed through a window (1MB on the
 * 386SX/Marty), so the whole loaded image plus .rodata has to fit in
 * the first window's worth of it. tools/mkicm.py checks that, and
 * checks .bss still ends below the 0xC0000 VRAM window the same way the
 * CD build's Makefile rule does.
 *
 * Unlike src/boot/mygame_shared.lds this is a plain static link at a
 * fixed address rather than a -shared/-Bsymbolic PIC object that
 * relocates itself at startup (src/boot/reloc.c). It can be, because
 * the load address here is not merely known but forced: the IPL copies
 * to it. Skipping the self-relocation also means .rodata's addresses
 * survive - a relocating loader would happily "fix up" every card
 * pointer to point at RAM instead.
 */

#include "icm_defs.h"

/* Distance from a section's run-time address in RAM to where it sits on
 * the card. The payload image starts at card offset FMT_ICM_PAYLOAD_OFF
 * (the sector after the IPL) and runs at FMT_ICM_LOAD_ADDR. */
#define ICM_LMA_OFF	(FMT_ICM_CARD_BASE + FMT_ICM_PAYLOAD_OFF - FMT_ICM_LOAD_ADDR)

OUTPUT_FORMAT("elf32-i386")
OUTPUT_ARCH(i386)

ENTRY(startup_32)

SECTIONS {
	. = FMT_ICM_LOAD_ADDR;
	_icm_load_addr = .;

	.text : AT(ADDR(.text) + ICM_LMA_OFF) {
		_start = .;
		*(.text)
		*(.text.*)
		*(.plt)
		_etext = .;
	} = 0x9090

	. = ALIGN(4);
	.data : AT(ADDR(.data) + ICM_LMA_OFF) {
		_data = .;
		*(.data)
		*(.data.*)
		/* Const data holding relocated pointers: resolved at link
		 * time here, but kept in RAM rather than on the card so
		 * that nothing that ld may want to write to ends up behind
		 * a write-protect switch. */
		*(.data.rel)
		*(.data.rel.*)
	}

	.got : AT(ADDR(.got) + ICM_LMA_OFF) {
		*(.got.plt)
		*(.got)
		_edata = .;
	}

	/* What the IPL copies into RAM: text, data and GOT, contiguous. */
	_icm_load_bytes = _edata - _icm_load_addr;

	/* .bss is next in RAM, and is not in the image at all - so it is
	 * placed here, before the jump into card space, purely to keep the
	 * addresses ld hands out moving in one direction. */
	. = ALIGN(4);
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
	_icm_bss_end = _end;

	/* And now into card space, immediately after the loaded image, so
	 * that the two are contiguous in the file the packer writes out.
	 *
	 * The explicit AT() matters: without it ld carries the load-address
	 * offset over from .data and puts .rodata's LMA another 12MB up,
	 * which objcopy -O binary then faithfully zero-fills the gap to.
	 * Here VMA and LMA are the same thing - the section is not loaded
	 * anywhere, it is already where it runs. */
	. = FMT_ICM_CARD_BASE + FMT_ICM_PAYLOAD_OFF + _icm_load_bytes;
	. = ALIGN(16);
	_icm_rodata_card = .;

	.rodata : AT(ADDR(.rodata)) {
		*(.rodata)
		*(.rodata.*)
	}

	_icm_rodata_end = .;
	_icm_rodata_bytes = _icm_rodata_end - _icm_rodata_card;

	/DISCARD/ : {
		*(.note*)
		*(.comment)
		*(.eh_frame)
		*(.rel.*)
		*(.dynamic)
		*(.hash)
		*(.gnu.hash)
		*(.dynsym)
		*(.dynstr)
	}
}
