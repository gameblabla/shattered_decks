| ─────────────────────────────────────────────────────────────────────────────
|  atarist_raster.S — the innermost loops of the 3D board.
|
|  These were C first, and they were measured: the compiler's ground span ran
|  at about 86 cycles a pixel, which put a moving 160x56 board at four vblanks
|  and the at-rest 320x112 one at nineteen.  The cost was not the texture
|  fetch, it was everything around it -- `u >> 16` on a 68000 is a variable
|  shift at two cycles a bit unless it is spelled SWAP, the texture mask had
|  been widened to a long, and the loop bound was re-read out of the viewport
|  struct on every iteration.
|
|  Spelled by hand the pixel is five instructions and about 38 cycles:
|
|      move.l  d0,d1     | u
|      swap    d1        | the integer part is now the low word, no shifting
|      and.w   d3,d1     | wrap inside the texture row
|      move.b  (a0,d1.w),(a1)+
|      add.l   d2,d0     | u += du
|
|  Texels are stored pre-multiplied by four (the chunky buffer's convention,
|  see atarist_c2p.S), so the byte that comes out is written straight down with
|  no scaling.
| ─────────────────────────────────────────────────────────────────────────────

	.text

	.macro	GPIXEL
	move.l	d0,d1
	swap	d1
	and.w	d3,d1
	move.b	(a0,d1.w),(a1)+
	add.l	d2,d0
	.endm

| void Atarist_GroundSpan(u8 *dst, const u8 *tex_row, u32 u, u32 du,
|                         int count, int mask)
|
| One scanline of the perspective ground.  The texture row and the step are
| solved by the caller, because a plane through the camera has exactly one
| depth per screen row: this routine never divides and never multiplies.
	.globl	_Atarist_GroundSpan
_Atarist_GroundSpan:
	movem.l	d2-d5,-(sp)
	move.l	20(sp),a1		| dst
	move.l	24(sp),a0		| tex_row
	move.l	28(sp),d0		| u
	move.l	32(sp),d2		| du
	move.l	36(sp),d5		| count
	move.l	40(sp),d3		| mask (low word is all that is used)
	tst.w	d5
	ble	9f

	move.w	d5,d4
	and.w	#3,d4			| count % 4, peeled first
	lsr.w	#2,d5			| then groups of four, which dbra counts
	tst.w	d4
	beq	2f
	subq.w	#1,d4
1:
	GPIXEL
	dbra	d4,1b
2:
	tst.w	d5
	beq	9f
	subq.w	#1,d5
3:
	GPIXEL
	GPIXEL
	GPIXEL
	GPIXEL
	dbra	d5,3b
9:
	movem.l	(sp)+,d2-d5
	rts

| void Atarist_ChunkyFill(u8 *dst, int count, int value)
|
| A solid run in the chunky buffer -- the sky band above the horizon, and the
| rows past the ground's far plane.  Chunky rows are four-byte aligned (the
| buffer follows two 256-aligned screens and the stride is 320), so the body
| moves longs.
	.globl	_Atarist_ChunkyFill
_Atarist_ChunkyFill:
	movem.l	d2-d3,-(sp)
	move.l	12(sp),a0		| dst
	move.l	16(sp),d0		| count
	move.l	20(sp),d1		| value
	tst.w	d0
	ble	9f

	| Replicate the byte across a long once, then store longs.
	and.l	#0xff,d1
	move.l	d1,d2
	lsl.l	#8,d2
	or.l	d2,d1
	move.l	d1,d2
	swap	d2
	or.l	d2,d1

	| ALIGN FIRST, THEN LONGS, THEN THE TAIL.  A flat quad span starts at an
	| arbitrary x, and `move.l` to an odd address on a 68000 is an address
	| error -- not a slow access -- which froze the duel outright the first
	| time this routine was used for anything but a whole scanline.
	move.l	a0,d2
	and.w	#3,d2
	beq	2f
	move.w	#4,d3
	sub.w	d2,d3			| bytes needed to reach a long boundary
	cmp.w	d0,d3
	ble	1f
	move.w	d0,d3			| ... or the whole run, if it is shorter
1:
	sub.w	d3,d0
	subq.w	#1,d3
11:
	move.b	d1,(a0)+
	dbra	d3,11b
2:
	move.w	d0,d3
	and.w	#3,d3			| the tail, stored after the longs
	lsr.w	#2,d0
	tst.w	d0
	beq	4f
	subq.w	#1,d0
3:
	move.l	d1,(a0)+
	dbra	d0,3b
4:
	tst.w	d3
	beq	9f
	subq.w	#1,d3
5:
	move.b	d1,(a0)+
	dbra	d3,5b
9:
	movem.l	(sp)+,d2-d3
	rts

| void Atarist_ChunkyCopy(u8 *dst, const u8 *src, int bytes)
|
| A flat block move in the chunky buffer.  The duel camera never moves, so the
| textured ground is the same picture every frame: it is rasterised once into a
| cache and copied in from there, which turns the most expensive thing on the
| screen into about a fifth of a vblank.  Both ends are long aligned (the cache
| and the chunky buffer both start on a 256-byte boundary and the widths are
| multiples of four), so this only has to handle the tail.
	.globl	_Atarist_ChunkyCopy
_Atarist_ChunkyCopy:
	movem.l	d2,-(sp)
	move.l	8(sp),a1		| dst
	move.l	12(sp),a0		| src
	move.l	16(sp),d0		| bytes
	tst.l	d0
	ble	9f
	move.l	d0,d2
	and.w	#3,d2
	lsr.l	#2,d0
	tst.l	d0
	beq	2f
	subq.l	#1,d0
1:
	move.l	(a0)+,(a1)+
	dbra	d0,1b
	clr.w	d0
	subq.l	#1,d0
	bcc	1b			| dbra only counts a word; carry on past 65536
2:
	tst.w	d2
	beq	9f
	subq.w	#1,d2
3:
	move.b	(a0)+,(a1)+
	dbra	d2,3b
9:
	movem.l	(sp)+,d2
	rts
