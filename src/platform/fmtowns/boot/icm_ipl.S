/*
 *	icm_ipl.S - FM TOWNS "IPL4" boot sector for an IC Memory Card.
 *
 * The IC-card counterpart of bootsect.S+setup.S, and much shorter than
 * the pair of them, because a memory card is not a disk.
 *
 * The boot ROM finds this sector the same way it finds a CD or floppy
 * IPL - "IPL4" at offset 0, then `call far <seg>:4` in real mode with
 * BL = device type (4 = IC memory card) and BH = unit - and it preloads
 * the sector to linear 0xB0000 before doing so. From there the CD path
 * has to talk to the disk BIOS to fetch its payload. This one does not:
 * the card is memory-mapped (0xD00000 on the 386SX/Marty, 0xC0000000 on
 * a 386DX-class machine - see icm_defs.h), so all that is needed is
 *
 *	real mode -> 32-bit flat protected mode
 *	copy the payload image out of the card into RAM at 0x10000
 *	jump to it
 *
 * which is why setup.S has no equivalent here: its whole job was the
 * mode switch, and doing the copy needs us to be in 32-bit mode anyway
 * (the card lives far above the 1MB a real-mode segment can reach).
 * The GDT installed below is the same flat 4GB code/data pair at
 * selectors 0x10/0x18 that setup.S installs and head.S expects, so the
 * payload cannot tell which of the two boot paths started it.
 *
 * Returning (lretw) hands the machine back to the boot ROM, which moves
 * on to the next bootable device - the documented way for an FM TOWNS
 * IPL to decline, and what we do if the card turns out not to be the
 * one we were built for.
 */

#include "defs.h"
#include "icm_defs.h"

/* Linear address the boot ROM preloads this sector to. Needed because
 * protected-mode code has to name its own labels by absolute address:
 * the linker lays this file out from 0, and 0xB0000 is where those
 * offsets actually end up. */
#define IPL_LINEAR	0x000B0000

	.code16
	.section ".iplsect", "ax", @progbits
_boot:

.globl	_main
_main:
	.ascii	"IPL4"			/* FM TOWNS IPL identifier */

.org 4
	jmp	real_start

.org 8
	.ascii	"FMTOWNS IC CARD"

/* UX/Marty ROM chain-load compatibility, exactly as in bootsect.S: the
 * ROM disk reads these as the boot device's IO.SYS extent, fails to
 * find "FBIOS" magic in what it gets, and falls through to running this
 * IPL. The extent has to be readable by the BIOS, so it points at
 * sector 0 - ourselves - with a length of one sector, never zero. */
.org 0x20
	.long	0			/* IO.SYS start sector: ourselves */
.org 0x24
	.long	1			/* IO.SYS length: 1 sector (must be >= 1) */

/* Patched by tools/mkicm.py once the payload has been linked and the
 * card image laid out. Nothing here is a build-time constant of this
 * file: the payload's size and entry point come out of the linked ELF,
 * so hard-coding them here would just be a second copy to get wrong. */
.org FMT_ICM_HDR_PAYLOAD_OFF
hdr_payload_off:
	.long	0
.org FMT_ICM_HDR_PAYLOAD_BYTES
hdr_payload_bytes:
	.long	0
.org FMT_ICM_HDR_LOAD_ADDR
hdr_load_addr:
	.long	FMT_ICM_LOAD_ADDR
.org FMT_ICM_HDR_ENTRY
hdr_entry:
	.long	FMT_ICM_LOAD_ADDR
.org FMT_ICM_HDR_TOC_OFF
hdr_toc_off:
	.long	0
.org FMT_ICM_HDR_CARD_BYTES
hdr_card_bytes:
	.long	0
.org FMT_ICM_HDR_CARD_BASE
hdr_card_base:
	.long	FMT_ICM_CARD_BASE

.org FMT_ICM_HDR_END
real_start:
	/* Only interested in being booted from the IC card. Any other
	 * device and this is not our image; decline politely. */
	cmpb	$4, %bl
	je	1f
	lretw
1:
	cli
	cld
	movw	%cs, %ax
	movw	%ax, %ds
	movw	%ax, %es
	movw	%ax, %ss
	movw	$0xF000, %sp		/* 0xBF000: above us, below the 0xC0000 VRAM window */

	/* The boot ROM read the card's CIS (attribute memory) to identify
	 * it. Put the window back to common memory before reading a byte
	 * of the image. */
	movw	$FMT_ICM_IO_ATTRIB, %dx
	xorb	%al, %al
	outb	%al, %dx

	/* Bank 0: the payload image, and everything the payload links
	 * against, is inside the first window's worth of the card by
	 * construction (tools/mkicm.py enforces it). */
	movw	$FMT_ICM_IO_BANK, %dx
	xorb	%al, %al
	outb	%al, %dx

	call	INITGRAPHICS
	call	VRAMMODE
	call	CLS

	movw	$(0x01+0x01*(80*16)), %di
	movw	$msg_boot, %si
	call	PRINT_STR

	lidt	idt_48
	lgdt	gdt_48

	movw	$0x0001, %ax		/* protected mode (PE) */
	lmsw	%ax
	jmp	flush_instr
flush_instr:
	movw	$KERNEL_DS, %ax
	movw	%ax, %ds
	movw	%ax, %es
	movw	%ax, %ss
	movw	%ax, %fs
	movw	%ax, %gs

	data32 ljmp $KERNEL_CS, $(IPL_LINEAR + pm_start)

	.code32
pm_start:
	movl	$0x0009F000, %esp

	/* Is the card actually there, and is it the card this image was
	 * built for? Its first four bytes are our own "IPL4" - we are
	 * looking at a copy of ourselves - so this both proves the window
	 * reads and proves the base address the payload was linked for is
	 * the one this machine uses. Getting that wrong is the one
	 * mistake that would otherwise fail silently and far from here:
	 * every const in the payload would point into nothing. */
	movl	(IPL_LINEAR + hdr_card_base), %ebp
	cmpl	$0x344C5049, (%ebp)		/* "IPL4", little-endian */
	jne	no_card

	movl	(IPL_LINEAR + hdr_payload_off), %esi
	addl	%ebp, %esi
	movl	(IPL_LINEAR + hdr_load_addr), %edi
	movl	(IPL_LINEAR + hdr_payload_bytes), %ecx
	testl	%ecx, %ecx
	jz	no_card				/* unpatched IPL: nothing to run */
	addl	$3, %ecx
	shrl	$2, %ecx
	rep
	movsl

	movl	(IPL_LINEAR + hdr_entry), %eax
	xorl	%ebx, %ebx			/* head.S: ESP is already set, EBX unused */
	jmp	*%eax

no_card:
	movl	$(0x01+0x03*(80*16)), %edi
	movl	$(IPL_LINEAR + msg_nocard), %esi
	call	PM_PRINT_STR
1:	hlt
	jmp	1b

	.code16

msg_boot:
	.asciz	"IC MEMORY CARD BOOT"
msg_nocard:
	.asciz	"IC CARD NOT READABLE AT LINKED BASE"

/*----------------------------------------------------------------------
 * FM-R-compatible text output, lifted from bootsect.S (which took it
 * from captainys's FD_IPL): the boot ROM leaves the machine in a mode
 * where 0xC0000 is the FMR VRAM window and the ANK font ROM can be
 * paged in over it at 0xCA000 with I/O 0xFF99. 80 bytes per scanline,
 * and each 8x8 glyph is drawn twice as tall so it is legible on a TV.
 *--------------------------------------------------------------------*/
.intel_syntax noprefix
INITGRAPHICS:
	MOV	DX,IO_FMR_GVRAMDISPMODE
	MOV	AL,0x27			# Show all planes, write to page 0
	OUT	DX,AL
	MOV	DX,IO_FMR_GVRAMMASK
	MOV	AL,0x0F			# Write to all planes
	OUT	DX,AL
	RET

VRAMMODE:
	PUSH	DX
	PUSH	AX
	MOV	AL,0
	MOV	DX,IO_FMR_VRAM_OR_MAINRAM
	OUT	DX,AL
	POP	AX
	POP	DX
	RET

CLS:
	PUSH	ES
	PUSH	DI
	PUSH	AX
	PUSH	CX
	MOV	AX,0x0C000
	MOV	ES,AX
	MOV	DI,0
	MOV	CX,80*400/2
	MOV	AX,0
	REP STOSW
	POP	CX
	POP	AX
	POP	DI
	POP	ES
	RET

# DS:SI	NUL-terminated string
# DI	VRAM address of the first glyph
PRINT_STR:
	PUSH	AX
	PUSH	SI
	PUSH	DI
print_str_loop:
	MOV	AH,[SI]
	INC	SI
	CMP	AH,0
	JZ	print_str_done
	PUSH	DI
	CALL	DRAWFONT_TALL
	POP	DI
	INC	DI
	JMP	SHORT print_str_loop
print_str_done:
	POP	DI
	POP	SI
	POP	AX
	RET

# DI:  VRAM ADDRESS
# AH:  ASCII Code
# C0000H must be in FM-R VRAM mode.
DRAWFONT_TALL:
	PUSH	DS
	PUSH	SI
	PUSH	CX
	PUSH	AX
	PUSH	DX

	MOV	DX,0x0C000
	MOV	DS,DX

	MOV	DX,IO_KVRAM_OR_ANKFONT
	MOV	AL,1
	OUT	DX,AL

	# ANK Font ROM CA000H-
	MOV	AL,8
	MUL	AH
	ADD	AX,0x0A000

	MOV	CX,8
	MOV	SI,AX
drawfont_tall_loop:
	MOV	DH,DS:[SI]
	MOV	[DI],DH
	MOV	[DI+0x50],DH
	INC	SI
	ADD	DI,0x0A0
	LOOP	drawfont_tall_loop

	MOV	DX,IO_KVRAM_OR_ANKFONT
	XOR	AL,AL
	OUT	DX,AL

	POP	DX
	POP	AX
	POP	CX
	POP	SI
	POP	DS
	RET
.att_syntax prefix

/* The same two routines again for 32-bit flat mode, where segments are
 * gone and the VRAM window and font ROM are just absolute addresses.
 * Only the failure path uses them, but a boot that dies silently is a
 * boot nobody can debug. */
	.code32
PM_PRINT_STR:
	pushl	%eax
	pushl	%esi
	pushl	%edi
1:	movb	(%esi), %ah
	incl	%esi
	testb	%ah, %ah
	jz	2f
	pushl	%edi
	call	PM_DRAWFONT_TALL
	popl	%edi
	incl	%edi
	jmp	1b
2:	popl	%edi
	popl	%esi
	popl	%eax
	ret

/* EDI: VRAM offset within the 0xC0000 window, AH: character */
PM_DRAWFONT_TALL:
	pushl	%eax
	pushl	%ecx
	pushl	%edx
	pushl	%esi

	movw	$IO_KVRAM_OR_ANKFONT, %dx
	movb	$1, %al
	outb	%al, %dx

	movzbl	%ah, %esi
	shll	$3, %esi
	addl	$0x000CA000, %esi	/* ANK font ROM, paged in over the window */
	addl	$0x000C0000, %edi
	movl	$8, %ecx
1:	movb	(%esi), %dh
	movb	%dh, (%edi)
	movb	%dh, 0x50(%edi)
	incl	%esi
	addl	$0xA0, %edi
	loop	1b

	movw	$IO_KVRAM_OR_ANKFONT, %dx
	xorb	%al, %al
	outb	%al, %dx

	popl	%esi
	popl	%edx
	popl	%ecx
	popl	%eax
	ret

	.code16

/*----------------------------------------------------------------------
 * Descriptor tables. Same shape as setup.S's, but with a full 4GB limit
 * rather than that file's 128MB: the card window at 0xC0000000 on a
 * 386DX-class machine is outside 128MB, and a limit fault there would
 * be a triple fault with nothing on screen to say why.
 *--------------------------------------------------------------------*/
	.balign 8
gdt:
	.quad	0x0000000000000000	/* NULL */
	.quad	0x0000000000000000	/* unused */
	.quad	0x00CF9A000000FFFF	/* 0x10: 4GB code, base 0, read/exec */
	.quad	0x00CF92000000FFFF	/* 0x18: 4GB data, base 0, read/write */
gdt_end:

idt_48:
	.word	0			/* idt limit = 0 */
	.long	0			/* idt base = 0 */

gdt_48:
	.word	gdt_end - gdt - 1
	.long	IPL_LINEAR + gdt
