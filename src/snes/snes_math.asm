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

.ACCU 16
.INDEX 16
.16BIT

.BASE $00
.RAMSECTION "snes_math_vars" BANK $7E SLOT 2
mth_a             dw
mth_b             dw
mth_p             dw
mth_q             dw
; snesQMul's own two words.  It CALLS the leaf multipliers, and they use
; mth_a..mth_q as scratch, so anything it needs to survive a call has to live
; somewhere they do not touch.  Sharing them is what made qmul(1.0, 64.0)
; come back as -64.0: the sign flag was overwritten by the second product.
mth_sgn           dw
mth_acc           dw
mth_ma            dw
mth_mb            dw
.ENDS

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
    lda 5,s
    sta.l mth_a
    lda 7,s
    sta.l mth_b

    sep #$20
.ACCU 8
    ; al * bl -> mth_p, of which only the high byte survives into the column
    lda.l mth_a
    sta.l $4202
    lda.l mth_b
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
    sta.l mth_p

    sep #$20
.ACCU 8
    lda.l mth_a                 ; al * bh
    sta.l $4202
    lda.l mth_b+1
    sta.l $4203
    nop
    nop
    nop
    nop
    rep #$20
.ACCU 16
    lda.l $4216
    clc
    adc.l mth_p
    sta.l mth_p                 ; cannot carry: 255 + 65025 < 65536

    sep #$20
.ACCU 8
    lda.l mth_a+1               ; ah * bl
    sta.l $4202
    lda.l mth_b
    sta.l $4203
    nop
    nop
    nop
    nop
    rep #$20
.ACCU 16
    lda.l $4216
    clc
    adc.l mth_p                 ; this one CAN carry out of sixteen bits
    sta.l mth_p
    lda #0
    rol a                       ; keep the carry as bit 0
    xba                         ; ...worth 256 in the high word
    sta.l mth_q

    sep #$20
.ACCU 8
    lda.l mth_a+1               ; ah * bh
    sta.l $4202
    lda.l mth_b+1
    sta.l $4203
    nop
    nop
    nop
    nop
    rep #$20
.ACCU 16
    lda.l $4216
    clc
    adc.l mth_q
    sta.l mth_q

    lda.l mth_p                 ; + the middle column's own high byte
    xba
    and #$00FF
    clc
    adc.l mth_q
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
    lda 5,s
    sta.l mth_a
    lda 7,s
    sta.l mth_b

    sep #$20
.ACCU 8
    lda.l mth_a
    sta.l $4202
    lda.l mth_b
    sta.l $4203
    nop
    nop
    nop
    nop
    rep #$20
.ACCU 16
    lda.l $4216
    sta.l mth_p                 ; al * bl

    sep #$20
.ACCU 8
    lda.l mth_a
    sta.l $4202
    lda.l mth_b+1
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
    adc.l mth_p
    sta.l mth_p

    sep #$20
.ACCU 8
    lda.l mth_a+1
    sta.l $4202
    lda.l mth_b
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
    adc.l mth_p
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

    lda 5,s
    sta.l mth_ma
    lda 7,s
    sta.l mth_mb
    lda #0
    sta.l mth_sgn                 ; sign accumulator

    lda.l mth_ma
    bpl +
    eor #$FFFF
    inc a
    sta.l mth_ma
    lda #1
    sta.l mth_sgn
+
    lda.l mth_mb
    bpl +
    eor #$FFFF
    inc a
    sta.l mth_mb
    lda.l mth_sgn
    eor #1
    sta.l mth_sgn
+
    ; (a*b) >> 8 = (low16 >> 8) | (high16 << 8)
    lda.l mth_mb
    pha
    lda.l mth_ma
    pha
    jsl snesMulLo
    pla
    pla
    lda.b tcc__r0
    xba
    and #$00FF
    sta.l mth_acc

    lda.l mth_mb
    pha
    lda.l mth_ma
    pha
    jsl snesMulHi
    pla
    pla
    lda.b tcc__r0
    xba
    and #$FF00
    clc
    adc.l mth_acc

    ; The sign is put back last.  LDX has no absolute-long addressing mode --
    ; only A does -- so the product is parked in Y while mth_sgn is read.
    tay
    lda.l mth_sgn
    beq +
    tya
    eor #$FFFF
    inc a
    sta.b tcc__r0
    plp
    rtl
+
    tya
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

    lda 7,s
    sta.w mth_b
    beq _dv_sat                 ; divide by zero saturates rather than hangs
    lda 5,s
    sta.w mth_a
    ; (a << 8) / b overflows sixteen bits exactly when a >= b * 256, which is
    ; (a >> 8) >= b.  Checking it here keeps the loop free of a per-step
    ; saturation test.
    xba
    and #$00FF
    cmp.w mth_b
    bcs _dv_sat

    stz.w mth_p                 ; remainder
    stz.w mth_q                 ; quotient

    ; The numerator is a << 8: bits 23..8 are `a` and bits 7..0 are zero, so
    ; shifting `a` left twenty-four times feeds the loop first `a` and then the
    ; eight zeros, in order.
    ldx #24
_dv_loop:
    asl.w mth_a
    rol.w mth_p                 ; remainder <<= 1; its 17th bit is the carry
    bcs _dv_sub                 ; a set 17th bit is always >= the divisor
    lda.w mth_p
    cmp.w mth_b
    bcc _dv_zero
_dv_sub:
    lda.w mth_p
    sec
    sbc.w mth_b
    sta.w mth_p
    sec
    bra _dv_shift
_dv_zero:
    clc
_dv_shift:
    rol.w mth_q
    dex
    bne _dv_loop

    lda.w mth_q
    sta.b tcc__r0
    plp
    rtl

_dv_sat:
    lda #$FFFF
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

.ENDS
