;-----------------------------------------------------------------------------
;  snes_math.asm — Q8.8 arithmetic on a CPU with no multiplier.
;
;  The 65816 has neither a multiply nor a divide.  The PPU has an unsigned
;  8x8 -> 16 multiplier ($4202 x $4203 -> $4216) with an eight-cycle latency,
;  and that is what every product here is built from: four partial products for
;  a 16x16, about forty-five cycles.  That cost is why the renderer never
;  multiplies per pixel -- only per vertex, per scanline or per span.
;
;  Division does not appear at all.  The one division the geometry needs is the
;  perspective divide, and it is a ROM lookup (snes_recip_row) followed by one
;  of these multiplies; see snes_math.h.
;-----------------------------------------------------------------------------
.include "hdr.asm"
.include "snes_fastdp.inc"

.ACCU 16
.INDEX 16
.16BIT

; The operands and partial products live on the fast direct page
; (snes_fastdp.inc) for the call: forty-odd accesses a product at six cycles
; instead of eight.  snesQMul has words of its own because it once called
; the leaf multipliers, which use FD_M_A..FD_M_Q as scratch; sharing them is
; what made qmul(1.0, 64.0) come back as -64.0.
.DEFINE FD_M_A      $11
.DEFINE FD_M_B      $13
.DEFINE FD_M_P      $15
.DEFINE FD_M_Q      $17
.DEFINE FD_M_SGN    $19
.DEFINE FD_M_ACC    $21
.DEFINE FD_M_MA     $23
.DEFINE FD_M_MB     $25

.BASE $C0
.SECTION "snes_math_text" SUPERFREE

;-----------------------------------------------------------------------------
; u16 snesMulHi(u16 a, u16 b)   -- the high 16 bits of a * b
;
;   a*b = al*bl + (al*bh + ah*bl) << 8 + ah*bh << 16
;
; so the high word is ah*bh plus the carry out of the middle column.  The
; middle sum can reach 17 bits, which is why it is accumulated with the carry
; flag rather than in one 16-bit add.
;-----------------------------------------------------------------------------
snesMulHi:
    php
    rep #$30
    phd
    lda #FASTDP
    tcd
    lda 7,s
    sta.b <FD_M_A
    lda 9,s
    sta.b <FD_M_B

    sep #$20
.ACCU 8
    ; al * bl -> mth_p, of which only the high byte survives into the column
    lda.b <FD_M_A
    sta.l $4202
    lda.b <FD_M_B
    sta.l $4203
    nop                         ; the product needs eight cycles to settle
    nop
    nop
    nop
    rep #$20
.ACCU 16
    lda.l $4216
    xba
    and #$00FF                  ; (al*bl) >> 8
    sta.b <FD_M_P

    sep #$20
.ACCU 8
    lda.b <FD_M_A                 ; al * bh
    sta.l $4202
    lda.b <FD_M_B+1
    sta.l $4203
    nop
    nop
    nop
    nop
    rep #$20
.ACCU 16
    lda.l $4216
    clc
    adc.b <FD_M_P
    sta.b <FD_M_P                 ; cannot carry: 255 + 65025 < 65536

    sep #$20
.ACCU 8
    lda.b <FD_M_A+1               ; ah * bl
    sta.l $4202
    lda.b <FD_M_B
    sta.l $4203
    nop
    nop
    nop
    nop
    rep #$20
.ACCU 16
    lda.l $4216
    clc
    adc.b <FD_M_P                 ; this one CAN carry out of sixteen bits
    sta.b <FD_M_P
    lda #0
    rol a                       ; keep the carry as bit 0
    xba                         ; ...worth 256 in the high word
    sta.b <FD_M_Q

    sep #$20
.ACCU 8
    lda.b <FD_M_A+1               ; ah * bh
    sta.l $4202
    lda.b <FD_M_B+1
    sta.l $4203
    nop
    nop
    nop
    nop
    rep #$20
.ACCU 16
    lda.l $4216
    clc
    adc.b <FD_M_Q
    sta.b <FD_M_Q

    lda.b <FD_M_P                 ; + the middle column's own high byte
    xba
    and #$00FF
    clc
    adc.b <FD_M_Q
    pld
    sta.b tcc__r0               ; 816-tcc returns in tcc__r0

    plp
    rtl

;-----------------------------------------------------------------------------
; u16 snesMulLo(u16 a, u16 b)   -- the low 16 bits, i.e. a plain 16-bit product
;
; Only three partial products are needed: ah*bh contributes nothing below bit
; sixteen.
;-----------------------------------------------------------------------------
snesMulLo:
    php
    rep #$30
    phd
    lda #FASTDP
    tcd
    lda 7,s
    sta.b <FD_M_A
    lda 9,s
    sta.b <FD_M_B

    sep #$20
.ACCU 8
    lda.b <FD_M_A
    sta.l $4202
    lda.b <FD_M_B
    sta.l $4203
    nop
    nop
    nop
    nop
    rep #$20
.ACCU 16
    lda.l $4216
    sta.b <FD_M_P                 ; al * bl

    sep #$20
.ACCU 8
    lda.b <FD_M_A
    sta.l $4202
    lda.b <FD_M_B+1
    sta.l $4203
    nop
    nop
    nop
    nop
    lda.l $4216                 ; only the low byte of al*bh reaches bit 15
    xba                         ; ...and it belongs in the high half
    lda #0
    rep #$20
.ACCU 16
    clc
    adc.b <FD_M_P
    sta.b <FD_M_P

    sep #$20
.ACCU 8
    lda.b <FD_M_A+1
    sta.l $4202
    lda.b <FD_M_B
    sta.l $4203
    nop
    nop
    nop
    nop
    lda.l $4216
    xba
    lda #0
    rep #$20
.ACCU 16
    clc
    adc.b <FD_M_P
    pld
    sta.b tcc__r0

    plp
    rtl

;-----------------------------------------------------------------------------
; s16 snesQMul(s16 a, s16 b)    -- (a * b) >> 8, Q8.8
;
; The hardware multiplier is unsigned, so the signs are taken off the operands
; and put back on the result.  |a| and |b| are bounded by the board being five
; units across, so the unsigned product cannot reach the top of 32 bits and the
; shift is a straight recombination of the two halves.
;-----------------------------------------------------------------------------
snesQMul:
    php
    rep #$30
    phd
    lda #FASTDP
    tcd
    lda 7,s
    sta.b <FD_M_MA
    eor 9,s
    sta.b <FD_M_SGN
    ; Signed 16 x signed 8, twice.  M7A is a write-twice register: a
    ; 16-bit store would write M7B instead of its high byte.
    sep #$20
.ACCU 8
    lda 7,s
    sta.l $211B
    lda 8,s
    sta.l $211B
    lda 9,s
    sta.l $211C
    lda.l $2134
    sta.b <FD_M_MB                ; fractional byte, for truncation toward zero
    rep #$20
.ACCU 16
    lda.l $2135
    sta.b <FD_M_ACC
    ; The low byte of b is unsigned in the partial-product expansion.
    lda 9,s
    and #$0080
    beq +
    lda.b <FD_M_ACC
    clc
    adc.b <FD_M_MA
    sta.b <FD_M_ACC
+   sep #$20
.ACCU 8
    lda 10,s
    sta.l $211C
    rep #$20
.ACCU 16
    lda.l $2134
    clc
    adc.b <FD_M_ACC
    sta.b <FD_M_ACC
    lda.b <FD_M_SGN
    bpl +
    lda.b <FD_M_MB
    and #$00FF
    beq +
    lda.b <FD_M_ACC
    inc a
    sta.b <FD_M_ACC
+   lda.b <FD_M_ACC
    pld
    sta.b tcc__r0
    plp
    rtl

;-----------------------------------------------------------------------------
; u16 snesUQDiv(u16 a, u16 b)   -- (a << 8) / b, saturating at $FFFF
;
; The only division in the port, and it is deliberately confined to per-frame
; and per-polygon setup: the floor's two edge slopes and a card's projected
; corners, a few dozen calls a frame.  Nothing per row and nothing per pixel
; reaches it -- those go through snes_recip_row instead.
;
; Plain restoring division, twenty-four steps for a twenty-four bit numerator.
; The remainder needs seventeen bits, which is the carry flag plus a word.
;-----------------------------------------------------------------------------
snesUQDiv:
    php
    rep #$30
    phd
    lda #FASTDP
    tcd

    lda 9,s
    sta.b <FD_M_B
    beq _dv_sat                 ; divide by zero saturates rather than hangs
    lda 7,s
    sta.b <FD_M_A
    ; (a << 8) / b overflows sixteen bits exactly when a >= b * 256, which is
    ; (a >> 8) >= b.  Checking it here keeps the loop free of a per-step
    ; saturation test.
    xba
    and #$00FF
    cmp.b <FD_M_B
    bcs _dv_sat

    stz.b <FD_M_Q                 ; quotient
    lda #0                        ; the remainder stays in A

    ; The numerator is a << 8: bits 23..8 are `a` and bits 7..0 are zero, so
    ; shifting `a` left twenty-four times feeds the loop first `a` and then the
    ; eight zeros, in order.
    ldx #24
_dv_loop:
    asl.b <FD_M_A
    rol a                         ; remainder <<= 1; its 17th bit is the carry
    bcs _dv_sub                   ; a set 17th bit is always >= the divisor
    cmp.b <FD_M_B
    bcc _dv_shift                 ; C clear: a zero quotient bit
_dv_sub:
    sbc.b <FD_M_B                 ; C is set on both ways in
    sec                           ; (a 17-bit remainder may borrow in 16)
_dv_shift:
    rol.b <FD_M_Q
    dex
    bne _dv_loop

    lda.b <FD_M_Q
    pld
    sta.b tcc__r0
    plp
    rtl

_dv_sat:
    lda #$FFFF
    pld
    sta.b tcc__r0
    plp
    rtl

;-----------------------------------------------------------------------------
; u16 snesVCounter(void)
;
; Reading $2137 latches the H/V counters; $213D then returns the vertical one,
; low byte first, and the second read supplies the ninth bit.  The low/high
; toggle is per-register and is cleared by reading $213F, which has to happen
; first or a stale toggle swaps the two bytes.  This is the
; port's stopwatch, and the only source a performance claim here may come from.
;-----------------------------------------------------------------------------
snesVCounter:
    php
    rep #$30
    sep #$20
.ACCU 8
    lda.l $213F                 ; STAT78: resets the counter read's high/low
                                ; toggle, without which the first $213D read
                                ; can return the HIGH byte and the stopwatch
                                ; reports nonsense
    lda.l $2137                 ; latch
    lda.l $213D
    sta.b tcc__r0
    lda.l $213D
    and #$01
    sta.b tcc__r0+1
    rep #$20
.ACCU 16
    plp
    rtl

; Monotonic scanline timestamp modulo 65536.  The library field count
; increments at NMI, not at scanline zero: phase the V counter at line 225.
; Retry if NMI interrupted the latch/read pair (it also latches the counters).
snesClock:
    php
    rep #$30
_ck_retry:
    lda.l snes_vblank_count
    pha
    jsl snesVCounter
    lda.l snes_vblank_count
    cmp 1,s
    beq +
    pla
    bra _ck_retry
+   pla
    ; A * 262 = A * 256 + A * 4 + A * 2 (modulo 16 bits), in registers.
    tax                         ; X = fields
    asl a
    tay                         ; Y = fields * 2
    asl a
    sta.l snes_clock_tmp        ; fields * 4
    tya
    clc
    adc.l snes_clock_tmp
    sta.l snes_clock_tmp        ; fields * 6
    txa
    xba
    and #$FF00
    clc
    adc.l snes_clock_tmp
    sta.l snes_clock_tmp        ; fields * 262
    lda.b tcc__r0
    cmp #225
    bcc +
    sec
    sbc #262
+   clc
    adc #37
    clc
    adc.l snes_clock_tmp
    sta.b tcc__r0
    plp
    rtl

.ENDS

.BASE $00
.RAMSECTION "snes_math_vars" BANK $7E SLOT 2
snes_clock_tmp    dw
.ENDS
