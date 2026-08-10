/*
 * Linker script for the IC Memory Card IPL sector (icm_ipl.S).
 *
 * One flat section starting at 0, which tools/mkicm.py drops at card
 * offset 0 and the boot ROM preloads to 0xB0000. It has to fit in one
 * 1024-byte IC card sector: that is all the ROM is known to have in
 * memory when it calls us (FM/TOWNS/IPL's ICM_IPLM.ASM puts its
 * second-stage loader at offset 1024 and BIOS-reads it back, which only
 * makes sense if the rest of the card is not preloaded). mkicm.py
 * enforces the limit.
 *
 * Unlike the CD build there is no separate "setup" stage to link in
 * after this one - see the head comment of icm_ipl.S.
 */

OUTPUT_FORMAT("binary")
OUTPUT_ARCH("i386")

ENTRY(_main);
SECTIONS {
	. = 0;
	.iplsect : { *(.iplsect) }

	/DISCARD/ : { *(.note*) *(.comment) }
}
