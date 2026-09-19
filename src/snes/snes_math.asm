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
; u16 snesMul16x8(u16 a, s16 k)  -- the low 16 bits of a * k, k in -128..127
;
; snesMulLo for a small signed multiplier.  Two CPU partial products, al*k
; and ah*k with k taken unsigned, and when k is negative the product is
; short of a*256 -- of which only al*256 survives in sixteen bits.  Its low
; sixteen bits are the plain product's whatever the operands' signs, which
; is all the callers keep (a texture step times a pixel offset from the
; row's middle).  No fast-page words: the operands are read off the stack
; and the partials are held in X and the caller's tcc__r0.
;
; NOT THE PPU MULTIPLIER.  M7A/M7B share the write-twice latch of BG1's
; scroll registers, and the motion frame's doubling is an HDMA on BG1VOFS
; (snes_video.c): an HDMA write landing between the two halves of M7A
; corrupted the product.  Every foreground product is a CPU one for that
; reason -- snesQMul and the floor rows' too.
;-----------------------------------------------------------------------------
snesMul16x8:
    sep #$20
.ACCU 8
    lda 4,s
    sta.l $4202
    lda 6,s
    sta.l $4203                 ; al * k             (cycles from the write)
    lda 5,s                     ; ah                  4
    xba                         ;                     3
    lda 6,s                     ; A = k, B = ah       4
    rep #$20                    ;                     3
.ACCU 16
    tax                         ;                     2
    lda.l $4216                 ; al * k, read at 16 + 6
    sta.b tcc__r0
    txa
    sep #$20
.ACCU 8
    xba
    sta.l $4202                 ; ah
    xba
    sta.l $4203                 ; ah * k
    cmp #$80                    ; C = k < 0           2
    rep #$20                    ;                     3
.ACCU 16
    bcc +
    ; k negative: the unsigned partials are a * (k + 256) -- take al*256
    ; off (ah*256*256 is beyond the word).
    lda 4,s
    xba
    and #$FF00
    eor #$FFFF
    sec
    adc.b tcc__r0
    sta.b tcc__r0
+   lda.l $4216                 ; ah * k, read at 8 + 6 at the soonest
    xba
    and #$FF00
    clc
    adc.b tcc__r0
    sta.b tcc__r0
    rtl

;-----------------------------------------------------------------------------
; s16 snesQMul(s16 a, s16 b)    -- (a * b) >> 8, Q8.8, truncated toward zero
;
; The CPU multiplier is unsigned, so the signs are taken off the operands
; and put back on the result: |a| * |b| is four eight-bit partials, of
; which bits 8..23 are the truncated magnitude,
;     (al*bl >> 8) + al*bh + ah*bl + (ah*bh << 8)
; and negating that is exactly truncation toward zero.  Each partial's
; operands are written while the previous one settles (eight cycles from
; the write of $4203 to the read of $4216, counted in the margins).
;-----------------------------------------------------------------------------
snesQMul:
    php
    rep #$30
    phd
    lda #FASTDP
    tcd
    lda 7,s
    eor 9,s
    sta.b <FD_M_SGN
    lda 7,s
    bpl +
    eor #$FFFF
    inc a
+   sta.b <FD_M_A               ; |a|
    lda 9,s
    bpl +
    eor #$FFFF
    inc a
+   sta.b <FD_M_B               ; |b|
    sep #$20
.ACCU 8
    lda.b <FD_M_A               ; al
    sta.l $4202
    lda.b <FD_M_B               ; bl
    sta.l $4203                 ; al * bl            (cycles from the write)
    lda.b <FD_M_B+1             ; bh                  3
    xba                         ; B = bh              3
    stz.b <FD_M_ACC+1           ;                     3
    nop                         ;                     2
    lda.l $4217                 ; (al * bl) >> 8      read at 11 + 5
    sta.b <FD_M_ACC
    xba                         ; A = bh
    sta.l $4203                 ; al * bh
    rep #$20                    ;                     3
.ACCU 16
    lda.b <FD_M_ACC             ;                     4
    clc                         ;                     2
    nop                         ;                     2
    adc.l $4216                 ;                     read at 11 + 6
    sta.b <FD_M_ACC
    sep #$20
.ACCU 8
    lda.b <FD_M_A+1             ; ah
    sta.l $4202
    lda.b <FD_M_B               ; bl
    sta.l $4203                 ; ah * bl
    rep #$20                    ;                     3
.ACCU 16
    lda.b <FD_M_ACC             ;                     4
    clc                         ;                     2
    nop                         ;                     2
    adc.l $4216                 ;                     read at 11 + 6
    sta.b <FD_M_ACC
    sep #$20
.ACCU 8
    lda.b <FD_M_B+1             ; bh
    sta.l $4203                 ; ah * bh
    rep #$20                    ;                     3
.ACCU 16
    lda.b <FD_M_SGN             ;                     4
    php                         ;                     3
    lda.l $4216                 ;                     read at 10 + 6
    xba
    and #$FF00
    clc
    adc.b <FD_M_ACC
    plp
    bpl +
    eor #$FFFF
    inc a
+   pld
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
; Restoring division over a twenty-four bit numerator, with the steps that
; cannot produce a quotient bit LEFT OUT.  The numerator is a << 8, so a
; step-by-step divide would shift `a` in a bit at a time and then eight
; zeros; but the saturation test above has proved (a >> 8) < b, so the
; first eight steps -- the ones that shift a's high byte in -- all compare
; a partial remainder below b and yield zero bits.  They are skipped by
; STARTING with the remainder a >> 8 and the low byte still to come:
; sixteen steps.  When a < b the same holds for all of a's sixteen bits,
; and the divide starts with the remainder a and only the eight zero steps.
; Bit for bit the same quotient as the full twenty-four; a third or two
; thirds of the time.  The steps are unrolled; the remainder needs
; seventeen bits, which is the carry flag plus a word.
.MACRO DV_STEP
    asl.b <FD_M_A
    rol a                       ; remainder <<= 1; its 17th bit is the carry
    bcs _dv_sub\@              ; a set 17th bit is always >= the divisor
    cmp.b <FD_M_B
    bcc _dv_shift\@            ; C clear: a zero quotient bit
_dv_sub\@:
    sbc.b <FD_M_B               ; C is set on both ways in
    sec                         ; (a 17-bit remainder may borrow in 16)
_dv_shift\@:
    rol.b <FD_M_Q
.ENDM

snesUQDiv:
    php
    rep #$30
    phd
    lda #FASTDP
    tcd

    lda 9,s
    sta.b <FD_M_B
    bne +
    jmp _dv_sat                 ; divide by zero saturates rather than hangs
+   lda 7,s
    sta.b <FD_M_A
    ; (a << 8) / b overflows sixteen bits exactly when a >= b * 256, which is
    ; (a >> 8) >= b.  Checking it here keeps the loop free of a per-step
    ; saturation test.
    xba
    and #$00FF
    cmp.b <FD_M_B
    bcc +
    jmp _dv_sat
+
    stz.b <FD_M_Q                 ; quotient
    lda.b <FD_M_A
    cmp.b <FD_M_B
    bcs +
    jmp _dv_small
+
    ; a >= b: the remainder starts as a's high byte, and a's low byte is
    ; shifted in over the next eight steps.  The bits shifted out of FD_M_A
    ; must be exactly a << 8 -- the low byte in the high position -- so the
    ; word is turned round and the zeros follow on their own.
    xba
    and #$00FF
    pha
    lda.b <FD_M_A
    xba
    and #$FF00
    sta.b <FD_M_A
    pla
    DV_STEP
    DV_STEP
    DV_STEP
    DV_STEP
    DV_STEP
    DV_STEP
    DV_STEP
    DV_STEP
    bra _dv_tail
_dv_small:
    ; a < b: the whole of a is the remainder and the eight zero bits are
    ; what is left to divide.
    lda.b <FD_M_A
    stz.b <FD_M_A
_dv_tail:
    DV_STEP
    DV_STEP
    DV_STEP
    DV_STEP
    DV_STEP
    DV_STEP
    DV_STEP
    DV_STEP

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
