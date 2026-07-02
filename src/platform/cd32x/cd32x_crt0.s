! SEGA CD32X support code for SH2
! by Chilly Willy
! Rom header and SH2 init/exception code - must be first in object list

    .text
    .global _start
_start:

    /* first two longs are for section alignment */
    .long   0x00000000
    .long   0x00000000

    /* actual start of header */
    .ascii  "MARS Waifu FM   "          /* module name */
    .long   0x00000000                  /* version */

    .long   0x00000000                  /* src = unused */
    .long   0x06000000                  /* dest = SH2 SDRAM */
    .long   __data_end-0x06000000       /* length */

    .long   master_start                /* Master SH2 entry point */
    .long   slave_start                 /* Slave SH2 entry point */

    .long   master_vbr                  /* Master SH2 VBR */
    .long   slave_vbr                   /* Slave SH2 VBR */

    .long   0x00000000                  /* reserved */
    .long   0x00000000                  /* reserved */

!-----------------------------------------------------------------------
! Master Vector Base Table
!-----------------------------------------------------------------------

master_vbr:
        .long   master_start    /* Cold Start PC */
        .long   0x0603F000      /* Cold Start SP */
        .long   master_start    /* Manual Reset PC */
        .long   0x0603F000      /* Manual Reset SP */
        .long   master_err      /* Illegal instruction */
        .long   0x00000000      /* reserved */
        .long   master_err      /* Invalid slot instruction */
        .long   0x00000000      /* reserved */
        .long   0x00000000      /* reserved */
        .long   master_err      /* CPU address error */
        .long   master_err      /* DMA address error */
        .long   master_err      /* NMI vector */
        .long   master_err      /* User break vector */
        .space  76              /* reserved */
        .long   master_err      /* TRAPA #32 */
        .long   master_err      /* TRAPA #33 */
        .long   master_err      /* TRAPA #34 */
        .long   master_err      /* TRAPA #35 */
        .long   master_err      /* TRAPA #36 */
        .long   master_err      /* TRAPA #37 */
        .long   master_err      /* TRAPA #38 */
        .long   master_err      /* TRAPA #39 */
        .long   master_err      /* TRAPA #40 */
        .long   master_err      /* TRAPA #41 */
        .long   master_err      /* TRAPA #42 */
        .long   master_err      /* TRAPA #43 */
        .long   master_err      /* TRAPA #44 */
        .long   master_err      /* TRAPA #45 */
        .long   master_err      /* TRAPA #46 */
        .long   master_err      /* TRAPA #47 */
        .long   master_err      /* TRAPA #48 */
        .long   master_err      /* TRAPA #49 */
        .long   master_err      /* TRAPA #50 */
        .long   master_err      /* TRAPA #51 */
        .long   master_err      /* TRAPA #52 */
        .long   master_err      /* TRAPA #53 */
        .long   master_err      /* TRAPA #54 */
        .long   master_err      /* TRAPA #55 */
        .long   master_err      /* TRAPA #56 */
        .long   master_err      /* TRAPA #57 */
        .long   master_err      /* TRAPA #58 */
        .long   master_err      /* TRAPA #59 */
        .long   master_err      /* TRAPA #60 */
        .long   master_err      /* TRAPA #61 */
        .long   master_err      /* TRAPA #62 */
        .long   master_err      /* TRAPA #63 */
        .long   master_lvl1     /* Level 1 IRQ */
        .long   master_lvl2_3   /* Level 2 & 3 IRQ's */
        .long   master_lvl4_5   /* Level 4 & 5 IRQ's */
        .long   master_pwm      /* PWM interupt */
        .long   master_cmd      /* Command interupt */
        .long   master_hbi      /* H Blank interupt */
        .long   master_vbi      /* V Blank interupt */
        .long   master_rst      /* Reset Button */

!-----------------------------------------------------------------------
! Slave Vector Base Table
!-----------------------------------------------------------------------

slave_vbr:
        .long   slave_start     /* Cold Start PC */
        .long   0x06040000      /* Cold Start SP */
        .long   slave_start     /* Manual Reset PC */
        .long   0x06040000      /* Manual Reset SP */
        .long   slave_err       /* Illegal instruction */
        .long   0x00000000      /* reserved */
        .long   slave_err       /* Invalid slot instruction */
        .long   0x00000000      /* reserved */
        .long   0x00000000      /* reserved */
        .long   slave_err       /* CPU address error */
        .long   slave_err       /* DMA address error */
        .long   slave_err       /* NMI vector */
        .long   slave_err       /* User break vector */
        .space  76              /* reserved */
        .long   slave_err       /* TRAPA #32 */
        .long   slave_err       /* TRAPA #33 */
        .long   slave_err       /* TRAPA #34 */
        .long   slave_err       /* TRAPA #35 */
        .long   slave_err       /* TRAPA #36 */
        .long   slave_err       /* TRAPA #37 */
        .long   slave_err       /* TRAPA #38 */
        .long   slave_err       /* TRAPA #39 */
        .long   slave_err       /* TRAPA #40 */
        .long   slave_err       /* TRAPA #41 */
        .long   slave_err       /* TRAPA #42 */
        .long   slave_err       /* TRAPA #43 */
        .long   slave_err       /* TRAPA #44 */
        .long   slave_err       /* TRAPA #45 */
        .long   slave_err       /* TRAPA #46 */
        .long   slave_err       /* TRAPA #47 */
        .long   slave_err       /* TRAPA #48 */
        .long   slave_err       /* TRAPA #49 */
        .long   slave_err       /* TRAPA #50 */
        .long   slave_err       /* TRAPA #51 */
        .long   slave_err       /* TRAPA #52 */
        .long   slave_err       /* TRAPA #53 */
        .long   slave_err       /* TRAPA #54 */
        .long   slave_err       /* TRAPA #55 */
        .long   slave_err       /* TRAPA #56 */
        .long   slave_err       /* TRAPA #57 */
        .long   slave_err       /* TRAPA #58 */
        .long   slave_err       /* TRAPA #59 */
        .long   slave_err       /* TRAPA #60 */
        .long   slave_err       /* TRAPA #61 */
        .long   slave_err       /* TRAPA #62 */
        .long   slave_err       /* TRAPA #63 */
        .long   slave_lvl1      /* Level 1 IRQ */
        .long   slave_lvl2_3    /* Level 2 & 3 IRQ's */
        .long   slave_lvl4_5    /* Level 4 & 5 IRQ's */
        .long   slave_pwm       /* PWM interupt (Level 6 & 7) */
        .long   slave_cmd       /* Command interupt (Level 8 & 9) */
        .long   slave_hbi       /* H Blank interupt (Level 10 & 11 */
        .long   slave_vbi       /* V Blank interupt (Level 12 & 13) */
        .long   slave_rst       /* Reset Button (Level 14 & 15) */

!-----------------------------------------------------------------------
! The Master SH2 starts here
!-----------------------------------------------------------------------

master_start:
        ! clear interrupt flags
        mov.l   _master_int_clr,r1
        mov.w   r0,@-r1                 /* PWM INT clear */
        mov.w   r0,@r1
        mov.w   r0,@-r1                 /* CMD INT clear */
        mov.w   r0,@r1
        mov.w   r0,@-r1                 /* H INT clear */
        mov.w   r0,@r1
        mov.w   r0,@-r1                 /* V INT clear */
        mov.w   r0,@r1
        mov.w   r0,@-r1                 /* VRES INT clear */
        mov.w   r0,@r1

        mov.l   _master_stk,r15
        ! purge cache and turn it off
        mov.l   _master_cctl,r0
        mov     #0x10,r1
        mov.b   r1,@r0

        ! clear bss
        mov     #0,r0
        mov.l   _bss_dst,r1
        mov.l   _bss_end,r2
0:
        mov.b   r0,@r1
        add     #1,r1
        cmp/eq  r1,r2
        bf      0b

        ! wait for 68000 to finish init
        mov.l   _master_sts,r0
        mov.l   _master_ok,r1
1:
        mov.l   @r0,r2
        nop
        nop
        cmp/eq  r1,r2
        bt      1b

        ! let Slave SH2 run
        mov     #0,r1
        mov.l   r1,@(4,r0)              /* clear slave status */

        mov     #0x80,r0
        mov.l   _master_adapter,r1
        mov.b   r0,@r1                  /* set FM */
        mov     #0x00,r0
        mov.b   r0,@(1,r1)              /* set int enables */
        mov     #0x20,r0
        ldc     r0,sr                   /* allow ints */

        ! purge cache, turn it on, and run main()
        mov.l   _master_cctl,r0
        mov     #0x11,r1
        mov.b   r1,@r0
        mov.l   _master_go,r0
        jmp     @r0
        nop

        .align   2

_master_int_clr:
        .long   0x2000401E              /* one word passed last int clr reg */
_master_stk:
        .long   0x0603F000              /* Cold Start SP */
_master_sts:
        .long   0x20004020
_master_ok:
        .ascii  "M_OK"
_master_adapter:
        .long   0x20004000
_master_cctl:
        .long   0xFFFFFE92
_master_go:
        .long   _main

_bss_dst:
        .long   __bss_start
_bss_end:
        .long   __bss_end

!-----------------------------------------------------------------------
! Each section of code below has its own data table so that the code
! can be extended without worrying about the offsets becoming too big.
! This results in duplicate entries, but not so many that we care. :)
!-----------------------------------------------------------------------

!-----------------------------------------------------------------------
! Master exception handler
!-----------------------------------------------------------------------

master_err:
        rte
        nop

!-----------------------------------------------------------------------
! Master Level 1 IRQ handler
!-----------------------------------------------------------------------

master_lvl1:
        rte
        nop

!-----------------------------------------------------------------------
! Master Level 2/3 IRQ handler
!-----------------------------------------------------------------------

master_lvl2_3:
        rte
        nop

!-----------------------------------------------------------------------
! Master Level 4/5 IRQ handler
!-----------------------------------------------------------------------

master_lvl4_5:
        rte
        nop

!-----------------------------------------------------------------------
! Master V Blank IRQ handler
!-----------------------------------------------------------------------

master_vbi:
        mov.l   r0,@-r15
        mov.l   r1,@-r15

        mov.l   mvi_mars_adapter,r1
        mov.w   r0,@(0x16,r1)           /* clear V IRQ */
        nop
        nop
        nop
        nop

        ! handle V IRQ

        mov.l   @r15+,r1
        mov.l   @r15+,r0
        rte
        nop

        .align  2

mvi_mars_adapter:
        .long   0x20004000

!-----------------------------------------------------------------------
! Master H Blank IRQ handler
!-----------------------------------------------------------------------

master_hbi:
        mov.l   r0,@-r15
        mov.l   r1,@-r15

        mov.l   mhi_mars_adapter,r1
        mov.w   r0,@(0x18,r1)           /* clear H IRQ */
        nop
        nop
        nop
        nop

        ! handle H IRQ

        mov.l   @r15+,r1
        mov.l   @r15+,r0
        rte
        nop

        .align  2

mhi_mars_adapter:
        .long   0x20004000

!-----------------------------------------------------------------------
! Master Command IRQ handler
!-----------------------------------------------------------------------

master_cmd:
        mov.l   r0,@-r15
        mov.l   r1,@-r15

        mov.l   mci_mars_adapter,r1
        mov.w   r0,@(0x1A,r1)           /* clear CMD IRQ */
        nop
        nop
        nop
        nop

        ! handle CMD IRQ

        mov.l   @r15+,r1
        mov.l   @r15+,r0
        rte
        nop

        .align  2

mci_mars_adapter:
        .long   0x20004000

!-----------------------------------------------------------------------
! Master PWM IRQ handler
!-----------------------------------------------------------------------

master_pwm:
        mov.l   r0,@-r15
        mov.l   r1,@-r15

        mov.l   mpi_mars_adapter,r1
        mov.w   r0,@(0x1C,r1)           /* clear PWM IRQ */
        nop
        nop
        nop
        nop

        ! handle PWM IRQ

        mov.l   @r15+,r1
        mov.l   @r15+,r0
        rte
        nop

        .align  2

mpi_mars_adapter:
        .long   0x20004000

!-----------------------------------------------------------------------
! Master RESET IRQ handler
!-----------------------------------------------------------------------

master_rst:
        mov.l   mvri_mars_adapter,r1
        mov.w   r0,@(0x14,r1)           /* clear VRES IRQ */
        nop
        nop
        nop
        nop

        mov     #0x0F,r0
        shll2   r0
        shll2   r0
        ldc     r0,sr                   /* disallow ints */

        mov.l   mvri_master_stk,r15
        mov.l   mvri_master_vres,r0
        jmp     @r0
        nop

        .align  2

mvri_mars_adapter:
        .long   0x20004000
mvri_master_stk:
        .long   0x0603F000              /* Cold Start SP */
mvri_master_vres:
        .long   master_reset

!-----------------------------------------------------------------------
! The Slave SH2 starts here
!-----------------------------------------------------------------------

slave_start:
        ! clear interrupt flags
        mov.l   _slave_int_clr,r1
        mov.w   r0,@-r1                 /* PWM INT clear */
        mov.w   r0,@r1
        mov.w   r0,@-r1                 /* CMD INT clear */
        mov.w   r0,@r1
        mov.w   r0,@-r1                 /* H INT clear */
        mov.w   r0,@r1
        mov.w   r0,@-r1                 /* V INT clear */
        mov.w   r0,@r1
        mov.w   r0,@-r1                 /* VRES INT clear */
        mov.w   r0,@r1

        mov.l   _slave_stk,r15
        ! wait for Master SH2 and 68000 to finish init
        mov.l   _slave_sts,r0
        mov.l   _slave_ok,r1
1:
        mov.l   @r0,r2
        nop
        nop
        cmp/eq  r1,r2
        bt      1b

        mov.l   _slave_adapter,r1
        mov     #0x00,r0
        mov.b   r0,@(1,r1)              /* set int enables (different from master despite same address!) */
        mov     #0x0F,r0
        shll2   r0
        shll2   r0
        ldc     r0,sr                   /* disallow ints */

! purge cache, turn it on, and run slave()
        mov.l   _slave_cctl,r0
        mov     #0x11,r1
        mov.b   r1,@r0
        mov.l   _slave_go,r0
        jmp     @r0
        nop

        .align   2

_slave_int_clr:
        .long   0x2000401E              /* one word passed last int clr reg */
_slave_stk:
        .long   0x06040000              /* Cold Start SP */
_slave_sts:
        .long   0x20004024
_slave_ok:
        .ascii  "S_OK"
_slave_adapter:
        .long   0x20004000
_slave_cctl:
        .long   0xFFFFFE92
_slave_go:
        .long   _slave

!-----------------------------------------------------------------------
! Each section of code below has its own data table so that the code
! can be extended without worrying about the offsets becoming too big.
! This results in duplicate entries, but not so many that we care. :)
!-----------------------------------------------------------------------

!-----------------------------------------------------------------------
! Slave exception handler
!-----------------------------------------------------------------------

slave_err:
        rte
        nop

!-----------------------------------------------------------------------
! Slave Level 1 IRQ handler
!-----------------------------------------------------------------------

slave_lvl1:
        rte
        nop

!-----------------------------------------------------------------------
! Slave Level 2/3 IRQ handler
!-----------------------------------------------------------------------

slave_lvl2_3:
        rte
        nop

!-----------------------------------------------------------------------
! Slave Level 4/5 IRQ handler
!-----------------------------------------------------------------------

slave_lvl4_5:
        rte
        nop

!-----------------------------------------------------------------------
! Slave V Blank IRQ handler
!-----------------------------------------------------------------------

slave_vbi:
        mov.l   r0,@-r15
        mov.l   r1,@-r15

        mov.l   svi_mars_adapter,r1
        mov.w   r0,@(0x16,r1)           /* clear V IRQ */
        nop
        nop
        nop
        nop

        ! handle V IRQ

        mov.l   @r15+,r1
        mov.l   @r15+,r0
        rte
        nop

        .align  2

svi_mars_adapter:
        .long   0x20004000

!-----------------------------------------------------------------------
! Slave H Blank IRQ handler
!-----------------------------------------------------------------------

slave_hbi:
        mov.l   r0,@-r15
        mov.l   r1,@-r15

        mov.l   shi_mars_adapter,r1
        mov.w   r0,@(0x18,r1)           /* clear H IRQ */
        nop
        nop
        nop
        nop

        ! handle H IRQ

        mov.l   @r15+,r1
        mov.l   @r15+,r0
        rte
        nop

        .align  2

shi_mars_adapter:
        .long   0x20004000

!-----------------------------------------------------------------------
! Slave Command IRQ handler
!-----------------------------------------------------------------------

slave_cmd:
        mov.l   r0,@-r15
        mov.l   r1,@-r15

        mov.l   sci_mars_adapter,r1
        mov.w   r0,@(0x1A,r1)           /* clear CMD IRQ */
        nop
        nop
        nop
        nop

        ! handle CMD IRQ

        mov.l   @r15+,r1
        mov.l   @r15+,r0
        rte
        nop

        .align  2

sci_mars_adapter:
        .long   0x20004000

!-----------------------------------------------------------------------
! Slave PWM IRQ handler
!-----------------------------------------------------------------------

slave_pwm:
        mov.l   r0,@-r15
        mov.l   r1,@-r15

        mov.l   spi_mars_adapter,r1
        mov.w   r0,@(0x1C,r1)           /* clear PWM IRQ */
        nop
        nop
        nop
        nop

        ! handle PWM IRQ

        mov.l   @r15+,r1
        mov.l   @r15+,r0
        rte
        nop

        .align  2

spi_mars_adapter:
        .long   0x20004000

!-----------------------------------------------------------------------
! Slave RESET IRQ handler
!-----------------------------------------------------------------------

slave_rst:
        mov.l   svri_mars_adapter,r1
        mov.w   r0,@(0x14,r1)           /* clear VRES IRQ */
        nop
        nop
        nop
        nop

        mov     #0x0F,r0
        shll2   r0
        shll2   r0
        ldc     r0,sr                   /* disallow ints */

        mov.l   svri_slave_stk,r15
        mov.l   svri_slave_vres,r0
        jmp     @r0
        nop

        .align  2

svri_mars_adapter:
        .long   0x20004000
svri_slave_stk:
        .long   0x06040000              /* Cold Start SP */
svri_slave_vres:
        .long   slave_reset

!-----------------------------------------------------------------------

master_reset:
        ! do any master SH2 specific reset code here

        mov.l   slave_st,r0
        mov.l   slave_ok,r1
0:
        mov.l   @r0,r2
        nop
        nop
        cmp/eq  r1,r2
        bf      0b                      /* wait for slave */

        mov.l   master_st,r0
        mov.l   master_ok,r1
        mov.l   r1,@r0                  /* tell everyone reset complete */

        mov.l   master_go,r0
        jmp     @r0
        nop

slave_reset:
        ! do any slave SH2 specific reset code here

        mov.l   slave_st,r0
        mov.l   slave_ok,r1
        mov.l   r1,@r0                  /* tell master to start reset */

        mov.l   master_st,r0
        mov.l   master_ok,r1
0:
        mov.l   @r0,r2
        nop
        nop
        cmp/eq  r1,r2
        bf      0b                      /* wait for master to do the work */

        mov.l   slave_go,r0
        jmp     @r0
        nop

        .align  2

master_st:
        .long   0x20004020
master_ok:
        .ascii  "M_OK"
master_go:
        .long   master_start
rom_header:
        .long   0x220003D4
rom_start:
        .long   0x22000000
sdram_start:
        .long   0x26000000

slave_st:
        .long   0x20004024
slave_ok:
        .ascii  "S_OK"
slave_go:
        .long   slave_start

!-----------------------------------------------------------------------
! Fast memcpy function - copies longs
! On entry: r4 = dst, r5 = src, r6 = len (in longs)

        .align  4

        .global _fast_memcpy
_fast_memcpy:
        mov.l   @r5+,r3
        mov.l   r3,@r4
        dt      r6
        bf/s    _fast_memcpy
        add     #4,r4
        rts
        nop

! Cache clear line function
! On entry: r4 = ptr - should be 16 byte aligned

        .align  4

        .global _CacheClearLine
_CacheClearLine:
        mov.l   _cache_flush,r0
        or      r0,r4
        mov     #0,r0
        mov.l   r0,@r4
        rts
        nop

        .align  2

_cache_flush:
        .long   0x40000000

! Cache control function
! On entry: r4 = cache mode => 0x10 = CP, 0x08 = TW, 0x01 = CE

        .align  4

        .global _CacheControl
_CacheControl:
        mov.l   _sh2_cctl,r0
        mov.b   r4,@r0
        rts
        nop

        .align  2

_sh2_cctl:
        .long   0xFFFFFE92

! int SetSH2SR(int level);
! On entry: r4 = new irq level
! On exit:  r0 = old irq level
        .align  4

        .global _SetSH2SR
_SetSH2SR:
        stc     sr,r1
        mov     #0x0F,r0
        shll2   r0
        shll2   r0
        and     r0,r1                   /* just the irq mask */
        shlr2   r1
        shlr2   r1
        not     r0,r0
        stc     sr,r2
        and     r0,r2
        shll2   r4
        shll2   r4
        or      r4,r2
        ldc     r2,sr
        rts
        mov     r1,r0

