| ─────────────────────────────────────────────────────────────────────────────
|  atarist_unzx0.S — ZX0 depacker, from AtariST/unzx0_68000/unzx0_68000.S.
|
|  Emmanuel Marty's ZX0 depacker with Chris Hodges' 68000 optimisations,
|  transcribed into gas syntax and given a C entry point.  The algorithm is
|  untouched; what is here that was not there is the register save (the
|  original documents that it trashes d2/a2, and those are callee-saved in the
|  m68k C ABI) and the argument fetch.
|
|  IT READS THE CURRENT ZX0 STREAM, NOT THE CLASSIC ONE.  `zx0 -c` output walks
|  off the end of the buffer here; plain `zx0` output is exact.  That was
|  settled by transliterating this file into Python and round-tripping both --
|  see tools/atarist/zx0pack.py, which still round-trips every blob it writes
|  through the same transliteration, so a stream this cannot depack fails the
|  build rather than the boot.
|
|  DECOMPRESSION IS IN PLACE.  The caller loads the packed bytes into the tail
|  of the destination buffer and decompresses over them; the depacker's writer
|  runs ahead of its reader, and the header the packer writes says by how much.
|
|  Original copyright (C) 2021 Emmanuel Marty, (C) 2023 Emmanuel Marty and
|  Chris Hodges; zlib licence, see AtariST/unzx0_68000/LICENSE.md.
| ─────────────────────────────────────────────────────────────────────────────

	.text
	.even
	.globl	_Atarist_Unzx0

| void Atarist_Unzx0(const void *src, void *dst);
_Atarist_Unzx0:
	movem.l	d2/a2,-(sp)
	move.l	12(sp),a0		| src: the packed stream
	move.l	16(sp),a1		| dst: where it decompresses to

	moveq.l	#-128,d1		| initialize empty bit queue
					| plus bit to roll into carry
	moveq.l	#-1,d2			| initialize rep-offset to 1
	bra.s	L_literals

L_do_copy_offs2:
	move.b	(a1,d2.l),(a1)+
	move.b	(a1,d2.l),(a1)+

	add.b	d1,d1			| read 'literal or match' bit
	bcs.s	L_get_offset		| if 0: go copy literals

L_literals:
	| read number of literals to copy
	moveq.l	#1,d0			| initialize value to 1
L_elias_loop1:
	add.b	d1,d1			| shift bit queue, high bit into carry
	bne.s	L_got_bit1		| queue not empty, bits remain
	move.b	(a0)+,d1		| read 8 new bits
	addx.b	d1,d1			| shift bit queue, high bit into carry
					| and shift 1 from carry into bit queue
L_got_bit1:
	bcs.s	L_got_elias1		| done if control bit is 1
	add.b	d1,d1			| read data bit
	addx.w	d0,d0			| shift data bit into value in d0
	bra.s	L_elias_loop1		| keep reading

L_got_elias1:
	subq.w	#1,d0			| dbf will loop until d0 is -1, not 0
L_copy_lits:
	move.b	(a0)+,(a1)+		| copy literal byte
	dbra	d0,L_copy_lits		| loop for all literal bytes

	add.b	d1,d1			| read 'match or rep-match' bit
	bcs.s	L_get_offset		| if 1: read offset, if 0: rep-match

L_rep_match:
	| read match length (starts at 1)
	moveq.l	#1,d0			| initialize value to 1
L_elias_loop2:
	add.b	d1,d1			| shift bit queue, high bit into carry
	bne.s	L_got_bit2		| queue not empty, bits remain
	move.b	(a0)+,d1		| read 8 new bits
	addx.b	d1,d1			| shift bit queue, high bit into carry
					| and shift 1 from carry into bit queue
L_got_bit2:
	bcs.s	L_got_elias2		| done if control bit is 1
	add.b	d1,d1			| read data bit
	addx.w	d0,d0			| shift data bit into value in d0
	bra.s	L_elias_loop2		| keep reading

L_got_elias2:
	subq.w	#1,d0			| dbra will loop until d0 is -1, not 0
L_do_copy_offs:
	move.l	a1,a2			| calculate backreference address
	add.l	d2,a2			| (dest + negative match offset)
L_copy_match:
	move.b	(a2)+,(a1)+		| copy matched byte
	dbra	d0,L_copy_match		| loop for all matched bytes

	add.b	d1,d1			| read 'literal or match' bit
	bcc.s	L_literals		| if 0: go copy literals

L_get_offset:
	moveq.l	#-2,d0			| initialize value to $fe

	| read high byte of match offset
L_elias_loop3:
	add.b	d1,d1			| shift bit queue, high bit into carry
	bne.s	L_got_bit3		| queue not empty, bits remain
	move.b	(a0)+,d1		| read 8 new bits
	addx.b	d1,d1			| shift bit queue, high bit into carry
					| and shift 1 from carry into bit queue
L_got_bit3:
	bcs.s	L_got_elias3		| done if control bit is 1
	add.b	d1,d1			| read data bit
	addx.w	d0,d0			| shift data bit into value in d0
	bra.s	L_elias_loop3		| keep reading

L_got_elias3:
	addq.b	#1,d0			| obtain negative offset high byte
	beq.s	L_done			| exit if EOD marker
	move.b	d0,-(sp)		| transfer negative high byte to stack
	move.w	(sp)+,d2		| shift it to make room for low byte

	move.b	(a0)+,d2		| read low byte of offset + 1 bit of len
	asr.l	#1,d2			| shift len bit into carry/offset in place
	bcs.s	L_do_copy_offs2		| if len bit is set, no need for more
	moveq.l	#1,d0			| initialize length value to 1
	| read rest of elias-encoded match length
	add.b	d1,d1			| read data bit
	addx.w	d0,d0			| shift data bit into value in d0

L_elias_loop4:
	add.b	d1,d1			| shift bit queue, high bit into carry
	bne.s	L_got_bit4		| queue not empty, bits remain
	move.b	(a0)+,d1		| read 8 new bits
	addx.b	d1,d1			| shift bit queue, high bit into carry
					| and shift 1 from carry into bit queue
L_got_bit4:
	bcs.s	L_do_copy_offs		| done if control bit is 1
	add.b	d1,d1			| read data bit
	addx.w	d0,d0			| shift data bit into value in d0
	bra.s	L_elias_loop4		| keep reading

L_done:
	movem.l	(sp)+,d2/a2
	rts
