/*
 * setup.s is responsible for getting the system data from the BIOS,
 * and putting them into the appropriate places in system memory.
 * both setup.s and system has been loaded by the bootblock.
 *
 * 1-Jan-96 Modified by Chris Brady for use as a boot/loader for memtest-86.
 */

#define __ASSEMBLY__
#include "defs.h"

.code16
.section ".setup", "ax", @progbits
.globl start
start:
# ok, the read went well
# now we want to move to protected mode ...


	cli			# no interrupts allowed #
	
	# disable NMI for the bootup sequence
	inb	$0x28, %al
	and	$0xFE, %al	
	outb	%al, $0x28

# The system will move itself to its rightful place.

	push	%cs
	pop	%ds
	/*
	 * NOTE: these use bare `idt_48`/`gdt_48` (not `idt_48 - start` /
	 * `gdt_48 - start` as in the classic floppy-boot code this was
	 * adapted from). The old "- start" form produced a *section-local*
	 * offset (correct only under the fixed-SETUPSEG floppy scheme,
	 * where DS was reloaded to a segment aliased exactly to `start`,
	 * i.e. offset 0). Here DS is just CS = INITSEG (see `push %cs; pop
	 * %ds` above), and `start` itself lives well past INITSEG:0 (right
	 * after bootsect.S's now-variable-sized code - see bootsect.S's
	 * comment on `ljmp $INITSEG, $start`). Bare symbols get a proper
	 * link-time relocation to their true final offset from INITSEG:0,
	 * same mechanism bootsect.S already relies on for that jump.
	 */
	lidt	idt_48	# load idt with 0,0
	lgdt	gdt_48	# load gdt with whatever appropriate


/*
 * Note that the short jump isn't strictly needed, althought there are
 * reasons why it might be a good idea. It won't hurt in any case.
 */
	movw	$0x0001, %ax	# protected mode (PE) bit
	lmsw	%ax		# This is it#
	jmp	flush_instr
flush_instr:
	movw	$KERNEL_DS, %ax
	movw	%ax, %ds
	movw	%ax, %es
	movw	%ax, %ss
	movw	%ax, %fs
	movw	%ax, %gs

data32	ljmp	$KERNEL_CS, $(TSTLOAD <<4)	# jmp offset 2000 of segment 0x10 (cs)


#
# Delay is needed after doing i/o
#
delay:
	.word	0x00eb			# jmp $+2
	ret

gdt:
	.word	0,0,0,0		# dummy

	.word	0,0,0,0		# unused

	.word	0x7FFF		# limit 128mb
	.word	0x0000		# base address=0
	.word	0x9A00		# code read/exec
	.word	0x00C0		# granularity=4096, 386

	.word	0x7FFF		# limit 128mb
	.word	0x0000		# base address=0
	.word	0x9200		# data read/write
	.word	0x00C0		# granularity=4096, 386

idt_48:
	.word	0			# idt limit=0
	.long	0			# idt base=0L

gdt_48:
	.word	0x800		# gdt limit=2048, 256 GDT entries
	/*
	 * gdt base = 0x9xxxx (INITSEG segment, physical 0x90000 + offset).
	 * The low word must be `gdt`'s actual final linked offset from
	 * INITSEG:0 in the combined bootsect+setup image. The old
	 * "512 + gdt - start" formula assumed the classic floppy-boot
	 * convention of a fixed 512-byte bootsect (so setup always starts
	 * at offset 512); that's stale here since bootsect.S and setup.S are
	 * linked+relocated together as one blob (see bootsect.S) and our
	 * custom CD bootsect is nowhere near 512 bytes. Using the bare
	 * symbol `gdt` lets the linker fix up the real absolute offset
	 * (same mechanism bootsect.S already relies on for
	 * `ljmp $INITSEG, $start`).
	 */
	.word	gdt,0x9

msg1:
	.asciz "Setup.S\r\n"

	/* Pad setup to the proper size */
	.org	(SETUPSECS*1024)

