;-----------------------------------------------------------------------------
;  snes_raster.asm — the texture mapper's inner loops.
;
;  C sets up per-frame and per-row geometry; every pixel that reaches the
;  chunky framebuffer is written from here.
;
;  THE FLOOR SPAN.  The camera never rolls and the board is axis aligned in
;  world space, so a screen row of the floor maps to a texture row of CONSTANT
;  v.  One reciprocal lookup and two multiplies give the row its starting u and
;  its du (snes_board3d.c), and the inner loop is a one-dimensional DDA.
;
;  Three register choices carry the whole loop:
;
;    * The texture index lives in X as (v << 8) | u, with v parked in the
;      hidden half of the accumulator.  `txa` reads back only u, the eight-bit
;      `adc` drops its carry so u wraps inside its own 256-byte row for free,
;      and `tax` reassembles the index from B.  That is why the floor texture
;      is 256 texels wide: the wrap costs no mask and no branch.
;    * X and not Y, because absolute-long-indexed -- the one addressing mode
;      that reads ROM outside the data bank in a single instruction -- exists
;      only with X.  The framebuffer therefore takes Y, and the data bank is
;      set to $7F so the store is a plain absolute-indexed one.
;    * The accumulators sit in a page-aligned block of low RAM and the direct
;      page register is pointed at it, which turns each of the three DDA
;      accesses from a five-cycle long into a three-cycle direct page one --
;      about a quarter of the loop.  It is safe because pvsneslib's NMI handler
;      saves and sets D and DB itself.
;
;  Textures are read straight out of ROM.  FastROM is six master cycles and ALL
;  WRAM is eight, so a texture copied into RAM "for speed" would be slower; see
;  SNES_PORT_PLAN.md section 11.
;-----------------------------------------------------------------------------
.include "hdr.asm"

.ACCU 16
.INDEX 16
.16BIT

.BASE $00

; Low RAM, page aligned: the direct page register is pointed here for the span
; loops, and a direct-page access costs an extra cycle when D's low byte is not
; zero.
.RAMSECTION "snes_raster_dp" BANK 0 SLOT 1 ALIGN 256
; rs_ufrac is the block's base -- the direct page register is pointed at it,
; and ALIGN 256 is what keeps D's low byte zero so a direct-page access does
; not pay its extra cycle.
rs_ufrac          db
rs_pad0           db
rs_dufrac         db
rs_pad1           db
rs_duint          db
rs_pad2           db
rs_end            dw          ; the framebuffer index one past the span
rs_colour         dw
.ENDS

.BASE $C0
.SECTION "snes_raster_text" SUPERFREE

;-----------------------------------------------------------------------------
; void snesSpanFloor(u16 fb_index, u16 count, u16 tex_index, u16 u_frac,
;                    u16 u_step)
;
;   fb_index   byte offset into snes_fb of the first pixel
;   count      pixels
;   tex_index  (v << 8) | u, the texel this span starts on
;   u_frac     the fractional part of u, 0..255
;   u_step     du in Q8.8: fraction in the low byte, whole texels in the high
;-----------------------------------------------------------------------------
snesSpanFloor:
    php
    rep #$30

    lda 7,s
    beq _sf_out                 ; empty spans are common at the board's edges
    clc
    adc 5,s
    sta.l rs_end                ; one past the last framebuffer byte
    lda 11,s
    and #$00FF
    sta.l rs_ufrac
    lda 13,s
    and #$00FF
    sta.l rs_dufrac
    lda 13,s
    xba
    and #$00FF
    sta.l rs_duint

    lda 5,s
    tay                         ; Y = framebuffer index
    lda 9,s
    tax                         ; X = (v << 8) | u

    phb
    phd
    lda #rs_ufrac
    tad
    pea $7F7F
    plb
    plb                         ; DB = $7F, the framebuffer's bank

    ; Park v in B, where `tax` picks it up again on every texel.
    txa                         ; A16 = (v << 8) | u
    xba                         ; A16 = (u << 8) | v
    sep #$20
.ACCU 8
    xba                         ; A8 = u, B = v -- the loop's invariant

_sf_loop:
    lda.b <rs_ufrac
    clc
    adc.b <rs_dufrac
    sta.b <rs_ufrac
    txa
    adc.b <rs_duint
    tax
    lda.l snes_floor_tex,x
    sta.w snes_fb,y
    iny
    cpy.b <rs_end
    bne _sf_loop

    rep #$20
.ACCU 16
    pld
    plb
_sf_out:
    plp
    rtl

;-----------------------------------------------------------------------------
; void snesSpanHorizon(u16 fb_index, u16 count, u16 tex_index, u16 u_frac,
;                      u16 u_step)
;
; The sky band above the board's far edge, same arguments and same walk.
;
; It is a COPY of the loop above rather than a shared one taking a texture
; pointer, and the reason is the addressing mode: `lda.l snes_floor_tex,x`
; needs its bank and base at assembly time, and the runtime alternative
; (`lda [dp],y`) costs a cycle on every texel of the floor, which is the hot
; path.  Two textures is where this stops -- the animating cards go through
; the affine quad walker, not this one.
;-----------------------------------------------------------------------------
snesSpanHorizon:
    php
    rep #$30

    lda 7,s
    beq _sh_out                 ; empty spans are common at the board's edges
    clc
    adc 5,s
    sta.l rs_end                ; one past the last framebuffer byte
    lda 11,s
    and #$00FF
    sta.l rs_ufrac
    lda 13,s
    and #$00FF
    sta.l rs_dufrac
    lda 13,s
    xba
    and #$00FF
    sta.l rs_duint

    lda 5,s
    tay                         ; Y = framebuffer index
    lda 9,s
    tax                         ; X = (v << 8) | u

    phb
    phd
    lda #rs_ufrac
    tad
    pea $7F7F
    plb
    plb                         ; DB = $7F, the framebuffer's bank

    ; Park v in B, where `tax` picks it up again on every texel.
    txa                         ; A16 = (v << 8) | u
    xba                         ; A16 = (u << 8) | v
    sep #$20
.ACCU 8
    xba                         ; A8 = u, B = v -- the loop's invariant

_sh_loop:
    lda.b <rs_ufrac
    clc
    adc.b <rs_dufrac
    sta.b <rs_ufrac
    txa
    adc.b <rs_duint
    tax
    lda.l snes_horizon_tex,x
    sta.w snes_fb,y
    iny
    cpy.b <rs_end
    bne _sh_loop

    rep #$20
.ACCU 16
    pld
    plb
_sh_out:
    plp
    rtl

;-----------------------------------------------------------------------------
; void snesSpanFill(u16 fb_index, u16 count, u8 colour)
;
; The backdrop either side of the board and above its far edge.  A 16-bit store
; covers two pixels a pass, so the caller may hand it an odd count; the tail is
; written singly.
;-----------------------------------------------------------------------------
snesSpanFill:
    php
    rep #$30

    lda 7,s
    beq _fl_out
    sta.l rs_end                ; the count, borrowing the span's slot
    lda 9,s
    and #$00FF
    sta.l rs_colour
    xba
    ora.l rs_colour
    sta.l rs_colour             ; the colour in both halves of the word

    lda 5,s
    tay

    phb
    pea $7F7F
    plb
    plb

    lda.l rs_end
    lsr a
    beq _fl_tail
    tax
    lda.l rs_colour
_fl_pair:
    sta.w snes_fb,y
    iny
    iny
    dex
    bne _fl_pair

_fl_tail:
    lda.l rs_end
    and #$0001
    beq _fl_pop
    sep #$20
.ACCU 8
    lda.l rs_colour
    sta.w snes_fb,y
    rep #$20
.ACCU 16

_fl_pop:
    plb
_fl_out:
    plp
    rtl

.ENDS
