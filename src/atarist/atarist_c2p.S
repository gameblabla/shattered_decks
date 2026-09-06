| ─────────────────────────────────────────────────────────────────────────────
|  atarist_c2p.S — chunky to planar for the 3D board half of the screen.
|
|  THE CHUNKY BUFFER DOES NOT HOLD PALETTE INDICES.  It holds index * 4.
|  That one convention is what makes the inner loop two instructions per pair
|  of pixels instead of five: a word read of two adjacent chunky bytes is then
|  *already* the byte offset of the pair's entry in a table of longs
|
|      offset = (va*4 << 8) | (vb*4) = 4 * ((va << 8) | vb)
|
|  so the loop is `move.w (a0)+,d2 / move.l (a2,d2.w),d4` with no masking, no
|  scaling and no zero extension.  The table is sparse (15 KB spanned, 256
|  entries live) and is built at boot by Atarist_C2PInit(); the floppy carries
|  none of it.  Texture and span data are emitted pre-multiplied, so the
|  rasteriser pays nothing for the convention.
|
|  Table entry layout, chosen so one MOVEP.L stores a whole 16-pixel group:
|
|      byte 3 (bits 31-24) : plane 0 bits, in the LOW nibble
|      byte 2             : plane 1
|      byte 1             : plane 2
|      byte 0             : plane 3
|
|  Two entries fold into one accumulator with `lsl.l #4 / or.l`, giving eight
|  planar bits per plane in each byte, and MOVEP.L then scatters those four
|  bytes to offsets 0/2/4/6 — exactly the ST's interleaved plane words.
|  A second MOVEP.L at offset 1 fills the low bytes with the next eight pixels.
|
|  _Double is the moving-camera path: 160x56 source, doubled in x by the table
|  and in y by storing each group twice, filling 320x112.
|  _Direct is the at-rest path: 320x112 source, 1:1.
| ─────────────────────────────────────────────────────────────────────────────

	.text

| void Atarist_C2P_Double(const u8 *src, u8 *dst, int rows, int src_stride)
	.globl	_Atarist_C2P_Double
_Atarist_C2P_Double:
	movem.l	d2-d5/a2,-(sp)
	move.l	24(sp),a0		| src
	move.l	28(sp),a1		| dst
	move.l	32(sp),d0		| source rows
	move.l	36(sp),d1		| source stride
	tst.w	d0
	ble	9f
	lea	_g_atarist_c2p_tab_x2,a2
	sub.w	#160,d1			| stride left over after 160 source bytes
	subq.w	#1,d0
1:
	moveq	#19,d3			| 20 groups of 16 output pixels per row
2:
	move.w	(a0)+,d2
	move.l	(a2,d2.w),d4
	move.w	(a0)+,d2
	lsl.l	#4,d4
	or.l	(a2,d2.w),d4
	move.w	(a0)+,d2
	move.l	(a2,d2.w),d5
	move.w	(a0)+,d2
	lsl.l	#4,d5
	or.l	(a2,d2.w),d5
	movep.l	d4,0(a1)
	movep.l	d5,1(a1)
	movep.l	d4,160(a1)		| the doubled scanline
	movep.l	d5,161(a1)
	addq.l	#8,a1
	dbra	d3,2b
	adda.w	d1,a0
	lea	160(a1),a1		| skip the row we just duplicated
	dbra	d0,1b
9:
	movem.l	(sp)+,d2-d5/a2
	rts

| void Atarist_C2P_Direct(const u8 *src, u8 *dst, int rows, int src_stride)
	.globl	_Atarist_C2P_Direct
_Atarist_C2P_Direct:
	movem.l	d2-d5/a2,-(sp)
	move.l	24(sp),a0
	move.l	28(sp),a1
	move.l	32(sp),d0
	move.l	36(sp),d1
	tst.w	d0
	ble	9f
	lea	_g_atarist_c2p_tab_x1,a2
	sub.w	#320,d1
	subq.w	#1,d0
1:
	moveq	#19,d3
2:
	move.w	(a0)+,d2
	move.l	(a2,d2.w),d4
	move.w	(a0)+,d2
	lsl.l	#2,d4
	or.l	(a2,d2.w),d4
	move.w	(a0)+,d2
	lsl.l	#2,d4
	or.l	(a2,d2.w),d4
	move.w	(a0)+,d2
	lsl.l	#2,d4
	or.l	(a2,d2.w),d4
	move.w	(a0)+,d2
	move.l	(a2,d2.w),d5
	move.w	(a0)+,d2
	lsl.l	#2,d5
	or.l	(a2,d2.w),d5
	move.w	(a0)+,d2
	lsl.l	#2,d5
	or.l	(a2,d2.w),d5
	move.w	(a0)+,d2
	lsl.l	#2,d5
	or.l	(a2,d2.w),d5
	movep.l	d4,0(a1)
	movep.l	d5,1(a1)
	addq.l	#8,a1
	dbra	d3,2b
	adda.w	d1,a0
	dbra	d0,1b
9:
	movem.l	(sp)+,d2-d5/a2
	rts
