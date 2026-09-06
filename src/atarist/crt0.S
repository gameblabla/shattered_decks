| ─────────────────────────────────────────────────────────────────────────────
|  Shattered Decks — Atari ST/STE port
|  crt0.S — the whole C runtime startup.
|
|  The port links -nostdlib: MiNTlib's crt0 drags in the full stdio/heap
|  runtime, which costs a quarter of a megabyte on a machine that has half of
|  one.  This does the three things a TOS program actually has to do:
|
|    1. give itself a stack (the basepage hands us none we can rely on),
|    2. Mshrink the TPA down to text+data+bss so Malloc() has the rest of the
|       machine to hand out (on a 512 KB ST that is the difference between
|       having a chunky buffer and not),
|    3. call the C entry and Pterm0 if it ever comes back.
|
|  The entry symbol is `_start`; the MiNT a.out loader stub that BFD places at
|  the head of TEXT jumps to it, so this need not be the literal first byte.
| ─────────────────────────────────────────────────────────────────────────────

	.text
	.globl	_start
_start:
	move.l	4(sp),a5		| basepage
	move.l	a5,_atarist_basepage
	move.l	12(a5),d0		| text length
	add.l	20(a5),d0		| + data length
	add.l	28(a5),d0		| + bss length
	add.l	#256,d0			| + the basepage itself
	lea	_atarist_stack_top,sp

	move.l	d0,-(sp)		| Mshrink(0, basepage, keep)
	move.l	a5,-(sp)
	clr.w	-(sp)
	move.w	#0x4a,-(sp)
	trap	#1
	lea	12(sp),sp

	jsr	_atarist_main

	clr.w	-(sp)			| Pterm0
	trap	#1

| ── Supervisor helpers ──────────────────────────────────────────────────────
| THE MODE IS QUERIED FIRST, AND THAT IS NOT OPTIONAL.
| Super(0) *toggles*: called from user mode it enters supervisor mode, but
| called from supervisor mode it drops to user.  A program in the AUTO folder
| is already started in supervisor mode, so the obvious unconditional Super(0)
| takes the game OUT of it, and the first write to a shifter register is then a
| privilege violation — which is exactly the silent hang this port booted with.
| Super(1) inquires instead: it returns -1 when already supervisor.

	.globl	_atarist_supervisor_enter
_atarist_supervisor_enter:
	pea	1.w
	move.w	#0x20,-(sp)
	trap	#1
	addq.l	#6,sp
	tst.l	d0
	bmi.s	1f			| -1: already supervisor, nothing to do
	clr.l	-(sp)
	move.w	#0x20,-(sp)
	trap	#1
	addq.l	#6,sp
1:
	rts

	.globl	_atarist_supervisor_leave
_atarist_supervisor_leave:
	move.l	4(sp),-(sp)
	move.w	#0x20,-(sp)
	trap	#1
	addq.l	#6,sp
	rts

	.bss
	.even
	.globl	_atarist_basepage
_atarist_basepage:
	.space	4
	.space	16384
	.globl	_atarist_stack_top
_atarist_stack_top:
