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
;      set to the frame's bank (snesRasterTarget) so the store is a plain
;      absolute-indexed one: Y is the pixel's absolute offset in that bank,
;      which is what SnesViewport.origin carries.  The rest frame is in $7E
;      and the motion frame in $7F.
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
rs_bits           dw          ; the glyph row the text blitter is shifting
rs_ink            dw
rs_dst            dw
rs_width          dw
rs_height         dw
rs_row            dw
rs_col            dw
rs_flip           dw
rs_fbbank         dw          ; the frame the walkers write: $7E or $7F
rs_sheet          dw          ; the stamp's 32x32 sheet: 0 = tex32, else tex32b
.ENDS

.RAMSECTION "snes_board_texture_ram" BANK $7F SLOT 3 ALIGN 256 KEEP
snes_board_texture dsb 32768
.ENDS

; The pitch-only floor mapper's inputs (snesFloorRowsPitch).  Low RAM, so C
; reaches them through the $7E mirror as plain globals.
.RAMSECTION "snes_floor_rows_ram" BANK 0 SLOT 1
fr_half           dw            ; the slab's half width on the row, Q8.8 units
fr_dhalf          dw            ; ...and its change per row
fr_denom16        dw            ; sin(pitch) + sy*cos(pitch), Q4.12
fr_dstep          dw            ; ...its step per row
fr_a16            dw            ; cos(pitch) - sy*sin(pitch), Q4.12
fr_astep          dw
fr_height         dw            ; the camera's height, Q8.8
fr_camz           dw            ; the camera's z, Q8.8
fr_ubase          dw            ; (cam.x << 5) + half a cell, Q8.8 texels
fr_origin         dw            ; the viewport's byte origin
fr_y              dw
fr_yend           dw
fr_denom          dw
fr_a              dw
fr_depth          dw
fr_dtex           dw
fr_x0             dw
fr_x1             dw
fr_tu             dw
fr_tv             dw
fr_tmp2           dw
fr_sub            dw            ; 1: the 256x144 frame, 0: the 128x72 motion frame
fr_halfw          dw            ; half the viewport's width in pixels (128 / 64)
fr_w              dw            ; the viewport's width (256 / 128)
.ENDS

.BASE $C0
.SECTION "snes_raster_text" SUPERFREE

; DB = the frame's bank.  A 16-bit A on entry and exit.
.MACRO RS_FRAME_BANK
    sep #$20
    lda.l rs_fbbank
    pha
    plb
    rep #$20
.ENDM

;-----------------------------------------------------------------------------
; void snesRasterTarget(u16 bank)
;
; Which WRAM bank the span walkers store into.  The viewport's origin is an
; absolute offset in that bank.
;-----------------------------------------------------------------------------
snesRasterTarget:
    php
    rep #$30
    lda 5,s
    and #$00FF
    sta.l rs_fbbank
    plp
    rtl

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
    RS_FRAME_BANK               ; DB = the frame's bank

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
    sta.w $0000,y
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
; void snesSpanFloorTex(u16 fb_index, u16 count, u16 tex_index, u16 u_frac,
;                       u16 u_step)
;
; snesSpanFloor over the WORLD texture (snes_board_texture: the floor with the
; cards stamped on it, 256 x 128) instead of the ROM checker.  A moving camera
; with no yaw has a constant v along every row, so the lift and the overhead
; pose walk at this routine's ~36 cycles a texel rather than the general
; quad walker's ~57.  Same contract as snesSpanFloor; v is the index's high
; byte and must already be masked to the texture's 128 rows.
;-----------------------------------------------------------------------------
snesSpanFloorTex:
    php
    rep #$30

    lda 7,s
    bne +
    plp
    rtl
+   ; ONE TEXEL A PIXEL IS A BLOCK MOVE.  The overhead pose is at 4.0 units,
    ; where 32 texels a unit meet 32 pixels a unit, so every row of it is a
    ; run of consecutive texels: MVN at seven cycles a byte instead of the
    ; walk's thirty-six.  The row must stay inside one texture row, the
    ; step must be exactly 1.0 and the frame must be in $7E, the bank the
    ; opcode names.
    lda 13,s
    cmp #$0100
    bne _st_walk
    lda.l rs_fbbank
    and #$00FF
    cmp #$007E
    bne _st_walk
    ; The board straddles the texture's u wrap (its columns are stamped at
    ; texels 192..255 and 0..95), so a row is at most two moves: up to the
    ; end of the texture row, then from its start.
    lda 9,s                     ; the texel BEFORE the first, as the walker
    inc a                       ; expects: the first is the next one
    sta.l rs_end                ; borrowed: the source index
    and #$00FF
    eor #$FFFF
    sec
    adc #256                    ; texels left in the texture row
    cmp 7,s
    bcc +
    lda 7,s                     ; the whole span fits before the wrap
+   sta.l rs_ufrac              ; borrowed: the first move's length
    lda.l rs_end
    tax                         ; source: snes_board_texture + index
    lda 5,s
    tay                         ; destination: the frame row
    lda.l rs_ufrac
    dec a                       ; MVN moves A + 1 bytes
    phb
    mvn $7F, $7E                ; WLA order: source bank, destination bank
    plb
    lda 7,s
    sec
    sbc.l rs_ufrac
    beq +                       ; nothing wrapped
    dec a
    pha
    lda.l rs_end
    and #$FF00
    tax                         ; the texture row's first texel; Y carried on
    pla
    phb
    mvn $7F, $7E
    plb
+   plp
    rtl
_st_walk:
    lda 7,s
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
    RS_FRAME_BANK

    txa
    xba
    sep #$20
.ACCU 8
    xba

_st_loop:
    lda.b <rs_ufrac
    clc
    adc.b <rs_dufrac
    sta.b <rs_ufrac
    txa
    adc.b <rs_duint
    tax
    lda.l snes_board_texture,x
    sta.w $0000,y
    iny
    cpy.b <rs_end
    bne _st_loop

    rep #$20
.ACCU 16
    pld
    plb
_st_out:
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
    RS_FRAME_BANK

    lda.l rs_end
    lsr a
    beq _fl_tail
    tax
    lda.l rs_colour
_fl_pair:
    sta.w $0000,y
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
    sta.w $0000,y
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
    RS_FRAME_BANK               ; DB = the frame's bank

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
    sta.w $0000,y
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
; void snesSpanCard32(u16 fb_index, u16 count, u16 tex_index, u16 u_frac,
;                     u16 u_step)
;
; THE 1:1 RESTING CARD, off the 32x32 sheet.  Same walk as snesSpanCard; the
; index is `face << 10 | v << 5 | u` and the eight-bit add still steps u
; inside the row because the caller clips the span to the face's 32 texels.
; Faces 64.. live in a second sheet (a long-indexed read spans one bank), so
; there are two copies of the loop; bit 15 of u_frac (whose high byte is
; otherwise unused) picks the second: the caller passes (face & 63) << 10 in
; tex_index and $8000 | frac for face >= 64.
;-----------------------------------------------------------------------------
snesSpanCard32:
    php
    rep #$30

    lda 7,s
    beq _s3_out
    clc
    adc 5,s
    sta.l rs_end
    lda 11,s
    sta.l rs_page               ; bit 15: which sheet
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
    RS_FRAME_BANK

    txa
    xba
    sep #$20
.ACCU 8
    xba                         ; A8 = the texel byte, B = the page
    lda.b <rs_page+1
    bmi _s3_hi

_s3_loop:
    lda.b <rs_ufrac
    clc
    adc.b <rs_dufrac
    sta.b <rs_ufrac
    txa
    adc.b <rs_duint
    tax
    lda.l snes_card_tex32,x
    sta.w $0000,y
    iny
    cpy.b <rs_end
    bne _s3_loop
    bra _s3_done

_s3_hi:
    lda.b <rs_ufrac
    clc
    adc.b <rs_dufrac
    sta.b <rs_ufrac
    txa
    adc.b <rs_duint
    tax
    lda.l snes_card_tex32b,x
    sta.w $0000,y
    iny
    cpy.b <rs_end
    bne _s3_hi

_s3_done:
    rep #$20
.ACCU 16
    pld
    plb
_s3_out:
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
; while a card is in the air, which is the moving board cadence.  The source
; remains full detail; the presenter paces complete board updates instead.
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
    RS_FRAME_BANK               ; DB = the frame's bank

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
    sta.w $0000,y
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


;-----------------------------------------------------------------------------
; void snesSpanCardQuad32(u16 fb_index, u16 count, u16 u, u16 v, u16 du,
;                         u16 dv, u16 page, u16 sheet)
;
; The affine walker off the 32x32 sheet, for the held card on the 1:1 board.
; u and v are clamped inside the face's 32 texels (v << 5 | u); page is
; (face & 63) << 10 and sheet is non-zero for faces 64 and up.
;-----------------------------------------------------------------------------
snesSpanCardQuad32:
    php
    rep #$30

    lda 7,s
    bne +
    jmp _q3_out
+
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
    lda 19,s
    sta.l rs_flip               ; the sheet

    lda 5,s
    tay

    phb
    phd
    lda #rs_ufrac
    tad
    RS_FRAME_BANK

    lda.b <rs_flip
    bne _q3_hi
_q3_loop:
    lda.b <rs_u
    clc
    adc.b <rs_du
    sta.b <rs_u
    bpl +
    lda #$0000
    sta.b <rs_u
+   cmp #$2000
    bcc +
    lda #$1FFF
    sta.b <rs_u
+
    xba                         ; the integer part into the low byte
    and #$001F
    ora.b <rs_page
    sta.b <rs_tmp
    lda.b <rs_v
    clc
    adc.b <rs_dv
    sta.b <rs_v
    bpl +
    lda #$0000
    sta.b <rs_v
+   cmp #$2000
    bcc +
    lda #$1FFF
    sta.b <rs_v
+
    xba
    asl a
    asl a
    asl a
    asl a
    asl a
    and #$03E0                  ; v.int << 5
    ora.b <rs_tmp
    tax
    sep #$20
.ACCU 8
    lda.l snes_card_tex32,x
    sta.w $0000,y
    rep #$20
.ACCU 16
    iny
    cpy.b <rs_end
    bne _q3_loop
    bra _q3_done

_q3_hi:
    lda.b <rs_u
    clc
    adc.b <rs_du
    sta.b <rs_u
    bpl +
    lda #$0000
    sta.b <rs_u
+   cmp #$2000
    bcc +
    lda #$1FFF
    sta.b <rs_u
+
    xba
    and #$001F
    ora.b <rs_page
    sta.b <rs_tmp
    lda.b <rs_v
    clc
    adc.b <rs_dv
    sta.b <rs_v
    bpl +
    lda #$0000
    sta.b <rs_v
+   cmp #$2000
    bcc +
    lda #$1FFF
    sta.b <rs_v
+
    xba
    asl a
    asl a
    asl a
    asl a
    asl a
    and #$03E0
    ora.b <rs_tmp
    tax
    sep #$20
.ACCU 8
    lda.l snes_card_tex32b,x
    sta.w $0000,y
    rep #$20
.ACCU 16
    iny
    cpy.b <rs_end
    bne _q3_hi

_q3_done:
    pld
    plb
_q3_out:
    plp
    rtl

; Motion mapper. D stays page-aligned at $4300; $4310-$433F are reserved
; DMA-register scratch (channels 1-3 never run).  The texture pointer wraps
; at 256 columns / 128 rows, and WMDATA increments the destination for free.
; A full Q8.8 carry DDA handles increments of either sign, including >1 texel.
snesSpanFloorQuad:
    php
    rep #$30
    lda 7,s
    bne +
    plp
    rtl
+   tax
    lda 5,s
    sta.l $2181
    sep #$20
.ACCU 8
    lda.l rs_fbbank             ; the frame's bank: $7E -> 0, $7F -> 1
    and #$01
    sta.l $2183
    rep #$20
.ACCU 16
    phd
    lda #$4300
    tcd
    ; PHP + PHD add three bytes to the caller's stack arguments.
    lda 11,s
    sta.b $10                   ; u fraction/integer
    lda 13,s
    sta.b $12                   ; v fraction/integer
    lda 15,s
    sta.b $14                   ; du
    lda 17,s
    sta.b $16                   ; dv
    ; Pointer base must be page-aligned.  The texture has its own bank's
    ; lower 32 KB (asserted by the link/verification).
    sep #$20
.ACCU 8
    lda.b $11
    sta.b $18
    lda.b $13
    and #$7F
    sta.b $19
    lda #$7F
    sta.b $1A
_fq_loop:
    lda.b $10
    clc
    adc.b $14
    sta.b $10
    lda.b $18
    adc.b $15
    sta.b $18
    lda.b $12
    clc
    adc.b $16
    sta.b $12
    lda.b $19
    adc.b $17
    and #$7F
    sta.b $19
    lda.b [$18]
    sta.l $2180
    dex
    bne _fq_loop
    rep #$20
.ACCU 16
    pld
    plp
    rtl

; void snesFloorRowsSetup(s16 half, s16 dhalf, s16 denom16, s16 dstep,
;                         s16 a16, s16 astep, s16 height, s16 camz,
;                         u16 ubase, u16 origin, u16 sub)
;
; `sub` is the viewport's pixels-per-unit shift: 1 for the 256x144 frame, 0
; for the 128x72 motion frame, where a pixel row is a whole unit row (the
; caller steps the camera terms accordingly), a pixel is depth/64 units
; across -- half a texel step at unit depth -- a row is 128 bytes and a
; converter cell four rows.
snesFloorRowsSetup:
    php
    rep #$30
    lda 25,s
    and #$0001
    sta.l fr_sub
    bne +
    lda #64
    sta.l fr_halfw
    lda #128
    sta.l fr_w
    bra ++
+   lda #128
    sta.l fr_halfw
    lda #256
    sta.l fr_w
++
    lda 5,s
    sta.l fr_half
    lda 7,s
    sta.l fr_dhalf
    lda 9,s
    sta.l fr_denom16
    lda 11,s
    sta.l fr_dstep
    lda 13,s
    sta.l fr_a16
    lda 15,s
    sta.l fr_astep
    lda 17,s
    sta.l fr_height
    lda 19,s
    sta.l fr_camz
    lda 21,s
    sta.l fr_ubase
    lda 23,s
    sta.l fr_origin
    plp
    rtl

; void snesFloorRowsPitch(u16 y0, u16 y1)
;
; THE FLOOR OF A PITCHING CAMERA, ROW BY ROW, with no C between the rows.
; The camera never yaws on the lift, so every screen row is one texture row
; of the world image at constant v and a constant step in u, and the whole
; of the per-row work is: two Q4.12 accumulators stepped, one reciprocal
; lookup, two Q8.8 products, one 16x8 product for the row's origin, and the
; walker.  C did exactly this and spent two thousand cycles a row doing it
; through the stack; this is a few hundred.
;
; The slab seen without yaw is a trapezoid symmetric about the middle of
; the viewport (the camera is always over x = 0), so its edges are one half
; width per row, stepped: x0 = 128 - half, x1 = 128 + half, clipped, and
; the converter's row spans are noted from the same numbers.
;
; Per row y in [y0, y1):
;   half += dhalf;  skip unless half > 0
;   denom = denom16 >> 4;  skip unless 2 < denom < 1024
;   x0, x1 from half;  skip unless x1 > x0
;   depth = height * recip_plane[denom]           (Q8.8)
;   z = camz + depth * a                          (Q8.8)
;   dtex = (depth + 2) >> 2                       (texels a pixel, Q8.8)
;   tu = ubase + dtex * (x0 - 128) + dtex / 2     (the first pixel's centre)
;   tv = z << 5
;   walk x1 - x0 texels from (tv, tu) stepping dtex
snesFloorRowsPitch:
    php
    rep #$30
    phb
    pea $0000
    plb
    plb                         ; DB = 0: the fr_* words by ldx.w (no long form)
    lda 6,s
    sta.l fr_y
    lda 8,s
    sta.l fr_yend
_fr_row:
    lda.l fr_y
    cmp.l fr_yend
    bcc +
    jmp _fr_done
+
    ; The half width for this row, then the step for the next.
    lda.l fr_half
    sta.l fr_tmp2
    clc
    adc.l fr_dhalf
    sta.l fr_half
    ; The camera terms for this row, then the step for the next.
    lda.l fr_denom16
    cmp #$8000
    ror a
    cmp #$8000
    ror a
    cmp #$8000
    ror a
    cmp #$8000
    ror a
    sta.l fr_denom
    lda.l fr_a16
    cmp #$8000
    ror a
    cmp #$8000
    ror a
    cmp #$8000
    ror a
    cmp #$8000
    ror a
    sta.l fr_a
    lda.l fr_denom16
    clc
    adc.l fr_dstep
    sta.l fr_denom16
    lda.l fr_a16
    clc
    adc.l fr_astep
    sta.l fr_a16
    ; Above the horizon, or too far below it for the table: nothing.
    lda.l fr_denom
    cmp #3
    bcc _fr_skip0
    cmp #1024
    bcs _fr_skip0
    ; The row's span: 128 -/+ half, in pixels, clipped to the viewport.
    lda.l fr_tmp2
    beq _fr_skip0
    bmi _fr_skip0
    bra +
_fr_skip0:
    jmp _fr_next
+
    cmp #$8000
    ror a
    lsr a
    lsr a
    lsr a
    lsr a
    lsr a
    lsr a                       ; half >> 7: Q8.8 units to pixels at 1:1
    ldx.w fr_sub
    bne +
    lsr a                       ; ...>> 8 on the motion frame
+   sta.l fr_tmp2
    lda.l fr_halfw
    sec
    sbc.l fr_tmp2
    bpl +
    lda #0
+   sta.l fr_x0
    lda.l fr_halfw
    clc
    adc.l fr_tmp2
    cmp.l fr_w
    bcc +
    beq +
    lda.l fr_w
+   sta.l fr_x1
    cmp.l fr_x0
    bcc _fr_skip
    beq _fr_skip
    ; Note the span for the converter: per cell row (eight lines at 1:1,
    ; four on the motion frame), the first and last pixel columns touched.
    lda.l fr_y
    lsr a
    lsr a
    ldx.w fr_sub
    beq +
    lsr a
+   asl a
    tax
    sep #$20
.ACCU 8
    lda.l fr_x0
    cmp.l snes_conv_rowspan,x
    bcs +
    sta.l snes_conv_rowspan,x
+   lda.l fr_x1
    dec a
    cmp.l snes_conv_rowspan+1,x
    bcc +
    sta.l snes_conv_rowspan+1,x
+   rep #$20
.ACCU 16
    bra _fr_map
_fr_skip:
    jmp _fr_next
_fr_map:
    ; depth = height * recip[denom]
    lda.l fr_denom
    asl a
    tax
    lda.l snes_recip_plane,x
    pha
    lda.l fr_height
    pha
    jsl snesQMul
    tsa
    clc
    adc #4
    tas
    lda.b tcc__r0
    sta.l fr_depth
    ; z = camz + depth * a
    lda.l fr_a
    pha
    lda.l fr_depth
    pha
    jsl snesQMul
    tsa
    clc
    adc #4
    tas
    lda.b tcc__r0
    clc
    adc.l fr_camz
    ; tv = z << 5, kept to the texture's 128 rows
    asl a
    asl a
    asl a
    asl a
    asl a
    and #$7F00
    sta.l fr_tv
    ; dtex = (depth + 2) >> 2 at 1:1, (depth + 1) >> 1 on the motion frame
    lda.l fr_depth
    ldx.w fr_sub
    beq +
    inc a
    lsr a
+   inc a
    lsr a
    sta.l fr_dtex
    ; tu = ubase + dtex * (x0 - 128) + dtex / 2, through the 16x8 signed
    ; multiplier: dtex is a positive word, x0 - 128 a signed byte.
    sep #$20
.ACCU 8
    lda.l fr_dtex
    sta.l $211B
    lda.l fr_dtex+1
    sta.l $211B
    lda.l fr_x0
    sec
    sbc.l fr_halfw
    sta.l $211C
    rep #$20
.ACCU 16
    lda.l $2134
    clc
    adc.l fr_ubase
    sta.l fr_tu
    lda.l fr_dtex
    lsr a
    clc
    adc.l fr_tu
    ; The walker steps before it reads: hand it the texel before the first.
    sec
    sbc.l fr_dtex
    sta.l fr_tu
    ; snesSpanFloorTex(origin + y * 256 + x0, x1 - x0,
    ;                  tv | (tu >> 8), tu & 255, dtex)
    lda.l fr_dtex
    pha
    lda.l fr_tu
    and #$00FF
    pha
    lda.l fr_tu
    xba
    and #$00FF
    ora.l fr_tv
    pha
    lda.l fr_x1
    sec
    sbc.l fr_x0
    pha
    lda.l fr_y
    xba
    and #$FF00                  ; y * 256
    ldx.w fr_sub
    bne +
    lsr a                       ; y * 128 on the motion frame
+   clc
    adc.l fr_x0
    clc
    adc.l fr_origin
    pha
    jsl snesSpanFloorTex
    tsa
    clc
    adc #10
    tas
_fr_next:
    lda.l fr_y
    inc a
    sta.l fr_y
    jmp _fr_row
_fr_done:
    plb
    plp
    rtl

; A world-space board image, shared by every camera pose.  The second half
; repeats the 64-row floor pattern; card stamps then cover their own slots.
snesBoardTextureClear:
    php
    rep #$30
    ldx #0
_bt_clear:
    lda.l snes_floor_tex,x
    sta.l snes_board_texture,x
    sta.l snes_board_texture+16384,x
    inx
    inx
    cpx #16384
    bne _bt_clear
    plp
    rtl

; centre = (world texture v << 8) | u; face is a 32x32 ROM image off the
; same two 1:1 sheets the resting board draws from, so a card looks the same
; from every camera pose.  width x height is the stamp's footprint in world
; texels; the face is resampled to it.
snesBoardTextureCard:
    php
    rep #$30
    lda 7,s
    and #$003F                  ; (face & 63) << 10: the page in its sheet
    xba
    asl a
    asl a
    sta.l rs_page
    lda 7,s
    and #$0040                  ; faces from 64 up are on the second sheet
    sta.l rs_sheet
    lda 9,s
    sta.l rs_flip
    lda 11,s
    sta.l rs_width
    lsr a
    sta.l rs_tmp
    lda 5,s
    sec
    sbc.l rs_tmp
    and #$00FF
    sta.l rs_dst
    lda 13,s
    sta.l rs_height
    lsr a
    xba
    sta.l rs_tmp
    lda 5,s
    sec
    sbc.l rs_tmp
    and #$7F00
    ora.l rs_dst
    sta.l rs_dst

    lda #4096
    sta.l $4204
    sep #$20
.ACCU 8
    lda.l rs_width
    sta.l $4206
    nop
    nop
    nop
    nop
    nop
    nop
    nop
    nop
    rep #$20
.ACCU 16
    lda.l $4214
    sta.l rs_du
    lda #4096
    sta.l $4204
    sep #$20
.ACCU 8
    lda.l rs_height
    sta.l $4206
    nop
    nop
    nop
    nop
    nop
    nop
    nop
    nop
    rep #$20
.ACCU 16
    lda.l $4214
    sta.l rs_dv
    lda #0
    sta.l rs_v
    lda.l rs_flip
    bne _bt_setup
    lda #4095
    sta.l rs_v
    lda.l rs_dv
    eor #$FFFF
    inc a
    sta.l rs_dv
_bt_setup:
    phb
    phd
    lda #rs_ufrac
    tad
    pea $7F7F
    plb
    plb
_bt_row:
    ; v is Q4.12 over the face: texel row = v >> 7, and a row is 32 bytes.
    lda.b <rs_v
    lsr a
    lsr a
    and #$03E0
    ora.b <rs_page
    sta.b <rs_tmp
    lda #0
    sta.b <rs_u
    lda.b <rs_width
    sta.b <rs_col
    lda.b <rs_dst
    tay
    lda.b <rs_sheet
    bne _bt_pixel_hi
_bt_pixel:
    lda.b <rs_u
    asl a                       ; texel column = u >> 7
    xba
    and #$001F
    ora.b <rs_tmp
    tax
    sep #$20
.ACCU 8
    lda.l snes_card_tex32,x
    sta.w snes_board_texture,y
    rep #$20
.ACCU 16
    iny
    lda.b <rs_u
    clc
    adc.b <rs_du
    sta.b <rs_u
    dec.b <rs_col
    bne _bt_pixel
    bra _bt_row_done
_bt_pixel_hi:
    lda.b <rs_u
    asl a
    xba
    and #$001F
    ora.b <rs_tmp
    tax
    sep #$20
.ACCU 8
    lda.l snes_card_tex32b,x
    sta.w snes_board_texture,y
    rep #$20
.ACCU 16
    iny
    lda.b <rs_u
    clc
    adc.b <rs_du
    sta.b <rs_u
    dec.b <rs_col
    bne _bt_pixel_hi
_bt_row_done:
    lda.b <rs_v
    clc
    adc.b <rs_dv
    sta.b <rs_v
    lda.b <rs_dst
    clc
    adc #256
    and #$7FFF
    sta.b <rs_dst
    dec.b <rs_height
    bne _bt_row
    pld
    plb
    plp
    rtl

.ENDS
