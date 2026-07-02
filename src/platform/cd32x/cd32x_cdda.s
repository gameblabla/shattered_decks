| CD-DA BIOS wrappers for the CD32X resident Sega-CD supervisor.
| Kept separate from the Raycaster-derived MD command bridge so the SH-2 side
| can request music through a narrow CD32X-only protocol.

        .text
        .align  2

        .equ    CDBIOS,          0x005F22
        .equ    CDSTAT,          0x005E80
        .equ    BIOS_MSC_STOP,   0x0002
        .equ    BIOS_ROM_PAUSEON,0x0008
        .equ    BIOS_DRV_INIT,   0x0010
        .equ    BIOS_MSC_PLAY1,  0x0012
        .equ    BIOS_MSC_PLAYR,  0x0013
        .equ    BIOS_CDB_STAT,   0x0081
        .equ    BIOS_FDR_SET,    0x0085
        .equ    BIOS_CDC_STOP,   0x0089
        .equ    GA_REG_CDFADER,   0xFF8034

| void cd32x_bios_cdda_init(void);
| Re-read the TOC after the boot loader has handed control to our Sub-CPU code.
| Megadev's examples do this explicitly; without it, CD-DA can fail silently on
| some BIOS/emulator paths after an IP/BIOS boot handoff.
        .global cd32x_bios_cdda_init
cd32x_bios_cdda_init:
        movem.l d2-d7/a2-a6,-(sp)
        lea     cd32x_drv_init_tracklist(pc),a0
        move.w  #BIOS_DRV_INIT,d0
        jsr     CDBIOS.w
        move.l  #0x00200000,d2          | bounded TOC wait; never strand boot
1:
        move.w  #BIOS_CDB_STAT,d0
        jsr     CDBIOS.w
        move.b  CDSTAT.w,d1
        andi.b  #0xF0,d1
        beq.b   2f
        subq.l  #1,d2
        bne.b   1b
2:
        movem.l (sp)+,d2-d7/a2-a6
        rts

| int cd32x_bios_cdda_play(int track, int loop);
| track is the absolute CD track number from the mixed-mode CUE. loop selects
| BIOS_MSC_PLAYR, otherwise BIOS_MSC_PLAY1.
        .global cd32x_bios_cdda_play
cd32x_bios_cdda_play:
        move.l  4(sp),d0
        move.w  d0,cd32x_cdda_track
        movem.l d2-d7/a2-a6,-(sp)
        move.w  #BIOS_CDC_STOP,d0
        jsr     CDBIOS.w
        move.w  #BIOS_ROM_PAUSEON,d0
        jsr     CDBIOS.w
        move.w  #0x0400,d1
        move.w  #BIOS_FDR_SET,d0
        jsr     CDBIOS.w
        move.w  #0x4000,GA_REG_CDFADER.l
        lea     cd32x_cdda_track(pc),a0
        tst.l   52(sp)                  | original second arg after movem push
        beq.b   1f
        move.w  #BIOS_MSC_PLAYR,d0
        bra.b   2f
1:
        move.w  #BIOS_MSC_PLAY1,d0
2:
        jsr     CDBIOS.w
        movem.l (sp)+,d2-d7/a2-a6
        moveq   #0,d0
        rts

| void cd32x_bios_cdda_stop(void);
        .global cd32x_bios_cdda_stop
cd32x_bios_cdda_stop:
        movem.l d2-d7/a2-a6,-(sp)
        move.w  #BIOS_MSC_STOP,d0
        jsr     CDBIOS.w
        movem.l (sp)+,d2-d7/a2-a6
        rts

        .align  2
cd32x_drv_init_tracklist:
        .byte   1,0xFF
        .align  2
cd32x_cdda_track:
        .word   2
