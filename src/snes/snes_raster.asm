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
.ENDS

.RAMSECTION "snes_board_texture_ram" BANK $7F SLOT 3 ALIGN 256 KEEP
snes_board_texture dsb 32768
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
    beq _st_out
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
; u and v wrap inside the face's 32 texels (v << 5 | u); page is
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
    xba                         ; the integer part into the low byte
    and #$001F
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
    xba
    and #$001F
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

; centre = (world texture v << 8) | u; face is a 16x16 ROM image.
; Stamp dimensions vary towards the native top view's 32/48 cell coverage.
snesBoardTextureCard:
    php
    rep #$30
    lda 7,s
    xba
    and #$FF00
    sta.l rs_page
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
    lda.b <rs_v
    lsr a
    lsr a
    lsr a
    lsr a
    and #$00F0
    ora.b <rs_page
    sta.b <rs_tmp
    lda #0
    sta.b <rs_u
    lda.b <rs_width
    sta.b <rs_col
    lda.b <rs_dst
    tay
_bt_pixel:
    lda.b <rs_u
    xba
    and #$000F
    ora.b <rs_tmp
    tax
    sep #$20
.ACCU 8
    lda.l snes_card_tex,x
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
