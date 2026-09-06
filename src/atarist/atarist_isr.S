| ─────────────────────────────────────────────────────────────────────────────
|  atarist_isr.S — the two interrupt handlers the in-game screen needs.
|
|  VBL      : flips the shifter to the finished buffer, loads the ARENA palette
|             for the top half of the coming frame, rearms Timer B, ticks the
|             clock and the music, then chains to whatever TOS had installed so
|             _hz_200 / _frclock / the floppy timeouts keep running.
|
|  Timer B  : the raster split.  Timer B counts display-enable pulses, so with
|             TBDR = split line and event-count mode it fires once, on that
|             line's horizontal blank, and loads the CARD palette for the rest
|             of the screen.
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

	| Top-of-screen palette.
	lea	_g_atarist_pal_arena,a0
	lea	0xffff8240,a1
	move.l	(a0)+,(a1)+
	move.l	(a0)+,(a1)+
	move.l	(a0)+,(a1)+
	move.l	(a0)+,(a1)+
	move.l	(a0)+,(a1)+
	move.l	(a0)+,(a1)+
	move.l	(a0)+,(a1)+
	move.l	(a0)+,(a1)+

	| Rearm the split for this frame, or leave it off.
	clr.b	0xfffffa1b			| TBCR = stop
	tst.b	_g_atarist_split_on
	beq	1f
	move.b	_g_atarist_split_line,0xfffffa21	| TBDR = line count
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
	movem.l	d0/a0-a1,-(sp)
	lea	_g_atarist_pal_card,a0
	lea	0xffff8240,a1
	move.l	(a0)+,(a1)+
	move.l	(a0)+,(a1)+
	move.l	(a0)+,(a1)+
	move.l	(a0)+,(a1)+
	move.l	(a0)+,(a1)+
	move.l	(a0)+,(a1)+
	move.l	(a0)+,(a1)+
	move.l	(a0)+,(a1)+
	bclr	#0,0xfffffa0f			| clear Timer B in-service
	movem.l	(sp)+,d0/a0-a1
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
