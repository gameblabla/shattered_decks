| ─────────────────────────────────────────────────────────────────────────────
|  atarist_isr.S — the two interrupt handlers the in-game screen needs.
|
|  VBL      : flips the shifter to the finished buffer, loads the ARENA palette
|             for the top half of the coming frame, rearms Timer B, ticks the
|             clock and the music, then chains to whatever TOS had installed so
|             _hz_200 / _frclock / the floppy timeouts keep running.
|
|  Timer B  : the raster splits.  Timer B counts display-enable pulses -- one
|             per displayed scanline -- so arming it with a line count in
|             event-count mode fires it exactly that many lines later.  The
|             handler loads the next palette in the list and re-arms itself for
|             the gap to the one after, so a screen may carry as many palettes
|             as it has splits, not two.
|
|             The list is precomputed as GAPS, not absolute lines: the handler
|             runs on the line it fired at and has to arm the distance to the
|             next one, and that subtraction inside a level 6 interrupt is
|             cycles the board render wants back.
|
|  Timer B is *stopped and reloaded from the VBL* rather than left free
|  running.  An MFP timer reloads its counter from the data register the moment
|  it expires, so a free-running split would fire again 112 lines later — which
|  is 40 lines into the next frame, and the split would walk up the screen.
| ─────────────────────────────────────────────────────────────────────────────

	.text

	.globl	_Atarist_VblHandler
	.globl	_Atarist_TimerBHandler

| ── VBL ─────────────────────────────────────────────────────────────────────
_Atarist_VblHandler:
	movem.l	d0-d2/a0-a2,-(sp)

	| Shifter base = the buffer the game just finished.
	move.l	_g_atarist_front,d0
	lsr.l	#8,d0
	move.b	d0,0xffff8203
	lsr.l	#8,d0
	move.b	d0,0xffff8201

	| Top-of-screen palette: entry 0 of the split list.
	lea	_g_atarist_split_pal,a0
	lea	0xffff8240,a1
	move.l	(a0)+,(a1)+
	move.l	(a0)+,(a1)+
	move.l	(a0)+,(a1)+
	move.l	(a0)+,(a1)+
	move.l	(a0)+,(a1)+
	move.l	(a0)+,(a1)+
	move.l	(a0)+,(a1)+
	move.l	(a0)+,(a1)+

	| Rearm the split list for this frame, or leave it off.
	clr.b	0xfffffa1b			| TBCR = stop
	clr.w	_g_atarist_split_index
	tst.b	_g_atarist_split_on
	beq	1f
	cmp.w	#2,_g_atarist_split_count
	blt	1f
	move.b	_g_atarist_split_gap+1,0xfffffa21 | TBDR = lines to split 1
	move.b	#8,0xfffffa1b			| TBCR = event count mode
1:
	addq.l	#1,_g_atarist_vbl

	| Music runs here: it is the one thing that must not miss a frame even
	| when a board render overruns its budget.
	jsr	_Atarist_AudioTick

	movem.l	(sp)+,d0-d2/a0-a2
	move.l	_g_atarist_old_vbl,-(sp)
	rts					| ... i.e. jump to the TOS handler

| ── Timer B: the raster split ───────────────────────────────────────────────
_Atarist_TimerBHandler:
	movem.l	d0-d1/a0-a1,-(sp)
	move.w	_g_atarist_split_index,d0
	addq.w	#1,d0
	move.w	d0,_g_atarist_split_index

	| Palette for this split: entry `index` of the list, 32 bytes each.
	move.w	d0,d1
	lsl.w	#5,d1
	lea	_g_atarist_split_pal,a0
	adda.w	d1,a0
	lea	0xffff8240,a1
	move.l	(a0)+,(a1)+
	move.l	(a0)+,(a1)+
	move.l	(a0)+,(a1)+
	move.l	(a0)+,(a1)+
	move.l	(a0)+,(a1)+
	move.l	(a0)+,(a1)+
	move.l	(a0)+,(a1)+
	move.l	(a0)+,(a1)+

	| Arm the gap to the next split, or stop for the rest of the frame.
	addq.w	#1,d0
	cmp.w	_g_atarist_split_count,d0
	bge	1f
	lea	_g_atarist_split_gap,a0
	move.b	0(a0,d0.w),0xfffffa21
	move.b	#8,0xfffffa1b
	bra	2f
1:
	clr.b	0xfffffa1b
2:
	bclr	#0,0xfffffa0f			| clear Timer B in-service
	movem.l	(sp)+,d0-d1/a0-a1
	rte

| ── IKBD ACIA ───────────────────────────────────────────────────────────────
| The decode itself is C (atarist_input.c): keyboard bytes arrive a few dozen
| times a second, so the wrapper only has to save what the m68k ABI lets a C
| function clobber and acknowledge the MFP.
	.globl	_Atarist_AciaWrapper
_Atarist_AciaWrapper:
	movem.l	d0-d1/a0-a1,-(sp)
	jsr	_Atarist_AciaPoll
	movem.l	(sp)+,d0-d1/a0-a1
	bclr	#6,0xfffffa11			| clear ACIA in-service
	rte
