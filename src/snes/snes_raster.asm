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
; The affine quad walker's own accumulators.  It steps u AND v, so it cannot
; use the one-byte trick the two constant-v walkers share.
rs_u              dw
rs_v              dw
rs_du             dw
rs_dv             dw
rs_page           dw          ; the face's page, already in the high byte
rs_tmp            dw
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

;-----------------------------------------------------------------------------
; void snesSpanCard(u16 fb_index, u16 count, u16 tex_index, u16 u_frac,
;                   u16 u_step)
;
; A RESTING CARD IS THE FLOOR WITH A DIFFERENT TEXTURE.  A card lies flat on
; the board and the camera never rolls, so a screen row of a card is a texture
; row of constant v exactly as the ground is, and it walks with the same two
; adds a texel -- no quad, no perspective divide, no edge chains.
;
; tex_index is `page << 8 | (v << 4) | u`: the high byte selects the face's
; 256-byte page in the card sheet and the low byte is the texel inside it.  The
; eight-bit `adc` therefore steps u within the card's own page, and C is
; responsible for handing over a span whose u stays inside its sixteen texels
; (snesDrawCardFlat) -- a carry out of the low nibble would step v, which is
; what the span's own clipping is there to make impossible.
;-----------------------------------------------------------------------------
snesSpanCard:
    php
    rep #$30

    lda 7,s
    beq _sc_out
    clc
    adc 5,s
    sta.l rs_end
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
    tay
    lda 9,s
    tax

    phb
    phd
    lda #rs_ufrac
    tad
    pea $7F7F
    plb
    plb

    txa
    xba
    sep #$20
.ACCU 8
    xba                         ; A8 = the texel byte, B = the page

_sc_loop:
    lda.b <rs_ufrac
    clc
    adc.b <rs_dufrac
    sta.b <rs_ufrac
    txa
    adc.b <rs_duint
    tax
    lda.l snes_card_tex,x
    sta.w snes_fb,y
    iny
    cpy.b <rs_end
    bne _sc_loop

    rep #$20
.ACCU 16
    pld
    plb
_sc_out:
    plp
    rtl

;-----------------------------------------------------------------------------
; void snesSpanCardQuad(u16 fb_index, u16 count, u16 u, u16 v, u16 du, u16 dv,
;                       u16 page)
;
; THE GENERAL AFFINE WALKER, for a card that is NOT lying flat: one that lifts
; off its slot, tilts and flies during a play or a battle.  Both u and v vary
; along the span, so the one-byte trick the three constant-v walkers share is
; not available and the index has to be assembled from two Q8.8 accumulators on
; every texel.
;
; u and v are Q8.8 texels; the integer parts are masked to four bits, so the
; texture wraps inside the face's page and a span clipped a fraction wide of
; the quad cannot walk into the next card's picture.  page is already shifted
; into the high byte.
;
; It costs about twice a floor texel, and that is the right trade: it runs only
; while a card is in the air, which is the moving resolution, which is a
; quarter of the pixels.
;-----------------------------------------------------------------------------
snesSpanCardQuad:
    php
    rep #$30

    lda 7,s
    beq _sq_out
    clc
    adc 5,s
    sta.l rs_end
    lda 9,s
    sta.l rs_u
    lda 11,s
    sta.l rs_v
    lda 13,s
    sta.l rs_du
    lda 15,s
    sta.l rs_dv
    lda 17,s
    sta.l rs_page

    lda 5,s
    tay

    phb
    phd
    lda #rs_ufrac
    tad
    pea $7F7F
    plb
    plb

_sq_loop:
    lda.b <rs_u
    clc
    adc.b <rs_du
    sta.b <rs_u
    xba                         ; the integer part into the low byte
    and #$000F
    ora.b <rs_page
    sta.b <rs_tmp
    lda.b <rs_v
    clc
    adc.b <rs_dv
    sta.b <rs_v
    xba
    asl a
    asl a
    asl a
    asl a
    and #$00F0                  ; v.int << 4
    ora.b <rs_tmp
    tax
    sep #$20
.ACCU 8
    lda.l snes_card_tex,x
    sta.w snes_fb,y
    rep #$20
.ACCU 16
    iny
    cpy.b <rs_end
    bne _sq_loop

    pld
    plb
_sq_out:
    plp
    rtl

.ENDS
