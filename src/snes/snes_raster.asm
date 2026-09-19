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
;      only with X.  The framebuffer is written through WMDATA, whose address
;      counter ($2181-$2183) is loaded with the span's first pixel -- the
;      pixel's absolute offset in the frame's bank (snesRasterTarget), which
;      is what SnesViewport.origin carries -- and Y is the count.  The rest
;      frame is in $7E and the motion frame in $7F.
;    * The accumulators sit on the FAST direct page -- DMA channel 1's
;      registers, snes_fastdp.inc -- so each of the DDA's three accesses is
;      three fast cycles rather than two fast and a slow one, and the frame
;      is written through WMDATA ($2180, a fast port with its own address
;      counter) rather than an indexed store into WRAM: the walkers' one
;      unavoidable WRAM access a texel is gone from the CPU's side of the
;      bus.  It is safe because pvsneslib's NMI handler saves and sets D
;      and DB itself and touches neither the port nor those registers.
;
;  Textures are read straight out of ROM.  FastROM is six master cycles and ALL
;  WRAM is eight, so a texture copied into RAM "for speed" would be slower; see
;  SNES_PORT_PLAN.md section 11.
;-----------------------------------------------------------------------------
.include "hdr.asm"
.include "snes_fastdp.inc"

.ACCU 16
.INDEX 16
.16BIT

.BASE $00

; The walkers' accumulators are NOT here any more: they are on the fast
; direct page (snes_fastdp.inc), DMA-register bytes at six cycles an access.
; What is left in WRAM is the state that outlives a call.
.RAMSECTION "snes_raster_ram" BANK 0 SLOT 1
rs_ufrac          dw          ; the MVN floor path's first move length
rs_end            dw          ; ...and its source index; snesSpanFill's count
rs_colour         dw
rs_fbbank         dw          ; the frame the walkers write: $7E or $7F
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
; The yawed camera's terms (snesFloorYawSetup / snesFloorRowYaw).
fr_cy             dw            ; cos(yaw), sin(yaw), Q8.8
fr_sn             dw
fr_eyecy          dw            ; cam.x * cos(yaw), cam.x * sin(yaw)
fr_eyesn          dw
fr_z              dw
fr_du             dw
fr_dv             dw
.ENDS


; The resting card rows' inputs (snesCardRows): snesDrawCardRow's per-row
; setup, handed over as plain bank-0 globals the way snesFloorRowsPitch's
; are.  C fills the first block; the rest is the loop's own.
.RAMSECTION "snes_card_rows_ram" BANK 0 SLOT 1
cr_y              dw            ; the first pixel row, and one past the last
cr_yend           dw
cr_rows           dw            ; ...its rows below the horizon
cr_base           dw            ; ...its byte offset in the frame, and a row
cr_stride         dw
cr_acc_l          dw            ; the centre card's edges and the slot pitch,
cr_acc_r          dw            ; Q8.8 units, seeded for the row before cr_y
cr_acc_p          dw
cr_step_l         dw            ; ...and their steps per row
cr_step_r         dw
cr_step_p         dw
cr_l_off          dw            ; 1 once an accumulator has latched
cr_r_off          dw
cr_p_off          dw
cr_limit          dw            ; the side of the viewport in Q8.8 units
cr_hf             dw            ; height * focal, Q8.8
cr_du_k           dw            ; the card's texel step per pixel at unit depth
cr_d_far          dw            ; the row's far and near edges from the camera
cr_d_near         dw
cr_clip_y0        dw            ; rows above this are stepped but not drawn
cr_sub            dw            ; 1: the 256x144 frame, 0: the 128x72 one
cr_halfw          dw
cr_w              dw
cr_flip           dw            ; the mask that turns a face to the far side
cr_faces          dsb 6         ; the five faces in COLUMN order (mirror applied)
cr_depth          dw
cr_du             dw
cr_texv           dw
cr_l              dw            ; the card being spanned
cr_r              dw
cr_face           dw
cr_xl             dw
cr_x0             dw
cr_x1             dw
cr_u              dw
cr_tmp            dw
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

; ── The constant-v walk ───────────────────────────────────────────────────
;
; THE STORE GOES THROUGH WMDATA.  A WRAM byte is an eight-cycle access from
; the CPU whichever way it is addressed, but the B-bus port at $2180 is a
; six-cycle one and auto-increments, so the frame is written as a stream:
; $2181-$2183 are pointed at the span's first pixel once and the loop's
; store is `sta.w $2180` -- four fast cycles against the indexed store's
; four plus a slow one -- and the framebuffer index register is freed to be
; the count, which retires the `cpy` against a memory word as well.  The
; accumulators are on the fast direct page (snes_fastdp.inc).  Two texels a
; pass halve the loop overhead; an odd count walks its first texel alone.
;
; Entered with the caller's five arguments at 8,s (php, phb and phd have
; been pushed under the three-byte return address), A/X/Y 16-bit.  DB is set to 0 so the port is a plain
; absolute; every texture read is long anyway.
.MACRO RS_WALK_SETUP
    lda #FASTDP
    tcd
    pea $0000
    plb
    plb                         ; DB = 0: $2180-$2183 by absolute address
    lda 8,s
    sta.w $2181                 ; WMADDL/M = the span's first frame byte
    sep #$20
.ACCU 8
    lda.l rs_fbbank
    and #$01
    sta.w $2183                 ; ...in $7E (0) or $7F (1)
    lda 14,s
    sta.b <FD_UFRAC
    lda 16,s
    sta.b <FD_DUFRAC
    lda 17,s
    sta.b <FD_DUINT
    rep #$20
.ACCU 16
    lda 12,s
    tax                         ; X = the texel index
.ENDM

; One texel: step the Q8.8 u, reassemble the index, read, stream out.
.MACRO RS_WALK_TEXEL ARGS TEX
    lda.b <FD_UFRAC
    clc
    adc.b <FD_DUFRAC
    sta.b <FD_UFRAC
    txa
    adc.b <FD_DUINT
    tax
    lda.l TEX,x
    sta.w $2180
.ENDM

; ── The bounded run ──────────────────────────────────────────────────────
;
; THE FRACTION MOVES INTO B.  The walk above keeps B for the index's high
; byte because `tax` rebuilds X after every eight-bit add, and so pays a
; load and a store of the fraction on every texel.  Over a piece of the
; span that provably stays inside its texture row, X can hold the WHOLE
; index instead: the fraction lives in B, is stepped with `xba; adc; xba`
; (XBA keeps C), and its carry is an `inx`:
;
;     xba / clc / adc.b <FD_DUFRAC / xba / bcc + / inx / + lda.l TEX-o,x / sta.w $2180
;
; Twenty-two CPU cycles a texel (plus one on a carry) against the walk's
; twenty-seven, and no fraction traffic at all.
;
; THE WHOLE TEXELS OF du ARE IN THE ADDRESS.  The block is RS_RUN_LEN
; copies of the body, entered 15 bytes a texel from its end; there is no
; loop, and a longer piece is simply entered twice.  Body i reads
; TEX - (LEN-1 - i) * k, so a step's k whole texels are the difference
; between one body's baked offset and the next's, and X only ever moves
; for a carry.  The entry adds n * k to X so that the last body reads the
; last texel exactly and X leaves the block as the true index.  A k of
; 0..3 costs the same.  (128 bodies: four k by five textures of 256 would
; not fit the code's bank.)
;
; THE DISPATCHER PROVES THE PIECE.  The walk's texel sequence is u_i =
; u_0 + i*du with u wrapping inside its 256-texel row (the floor's, or a
; card page's).  A piece can take the block only while no step carries out
; of the low byte:
;
;   * index.low + count*(k+1) <= 255 proves the whole rest of the span
;     fits (a step moves at most k+1 texels): no arithmetic.
;   * Otherwise the exact end (index.low << 8 | frac) + count*f + count*k*256
;     is formed with the CPU multiplier; no carry out of sixteen bits is
;     the same proof.
;   * A k = 0 span that does wrap is cut: the texels left in the row are
;     rem = $FFFF - u, the safe count rem / f through the CPU divider, at
;     most count; the wrap step itself is walked (eight-bit add, tax) and
;     the rest dispatched again.  A k >= 1 span that wraps is walked whole:
;     the run saves that class a tenth a texel, which two pieces and a
;     wrap step would spend.
;
; Steps of four or more whole texels, negative steps and spans shorter than
; RS_RUN_MIN keep the walk.  The block sees the same NMI as the walk:
; pvsneslib's handler saves the 16-bit A (so both XBA halves), X, Y, D, DB.
;
; Entered with A8, X16, D = FASTDP, DB = 0, WMADD at the span's first byte,
; X = the texel BEFORE the first, FD_UFRAC/FD_DUFRAC/FD_DUINT, du < $0400,
; and FD_RUN_CNT > 0 the texels to walk.  Leaves A8; X, Y and B clobbered.
.DEFINE RS_RUN_MIN  24
; The world texture's u origin, snes_video.h's SNES_WORLD_U_CENTRE (the
; C passes it to the pitch mapper in ubase; the yaw mapper adds it here).
.DEFINE FR_U_CENTRE 144
.DEFINE RS_RUN_BODY 15          ; bytes: one texel of the block
.DEFINE RS_RUN_LEN  128         ; texels: the longest piece

; The 256-body block over TEX with K whole texels a step baked in.
.MACRO RS_RUN_BLOCK ARGS TEX, K
.REPT RS_RUN_LEN INDEX i
    xba
    clc
    adc.b <FD_DUFRAC
    xba
    bcc +
    inx
+   lda.l TEX-(RS_RUN_LEN-1-i)*K,x
    sta.w $2180
.ENDR
.ENDM

; The piece (FD_RUN_N, 1..256, already off the count) is entered: the
; stack = its entry - 1 (RTS adds one) from the instance's table, X moved
; on by n * K, B = the fraction.  X is parked in a fast word rather than
; on the (WRAM) stack while the table is read.
.MACRO RS_RUN_ENTER ARGS K, ATAB
.ACCU 16
    lda.b <FD_RUN_N
    asl a
    stx.b <FD_RUN_TMP
    tax
    lda.l ATAB,x
    pha
.IF K == 0
    ldx.b <FD_RUN_TMP
.ELSE
    lda.b <FD_RUN_N
.IF K == 2
    asl a
.ENDIF
.IF K == 3
    sta.b <FD_RUN_N
    asl a
    clc
    adc.b <FD_RUN_N
.ENDIF
    clc
    adc.b <FD_RUN_TMP
    tax
.ENDIF
    sep #$20
.ACCU 8
    lda.b <FD_UFRAC
    xba                         ; B = the fraction
    rts                         ; into the block
.ENDM

.MACRO RS_RUN ARGS TEX
.ACCU 8
_rn_loop\@:
    lda.b <FD_DUINT
    beq +
    cmp #2
    bcs ++
    jmp _rn_k1\@
++  beq +++
    jmp _rn_k3\@
+++ jmp _rn_k2\@
    ; k = 0.  The cheap test first: index.low + count <= 255 means no
    ; step can carry out of the row.
+   rep #$20
.ACCU 16
    txa
    and #$00FF
    clc
    adc.b <FD_RUN_CNT
    cmp #$0100
    bcc _rn_fit0\@
    ; The exact test: (index.low << 8 | frac) + count * f overflowing
    ; sixteen bits is the wrap.  count * f from the CPU multiplier, the
    ; current u assembled while it works; a count of 256 (only a full
    ; rest row) goes straight to the divider.
    lda.b <FD_RUN_CNT
    xba
    and #$00FF
    bne _rn_div0\@
    sep #$20
.ACCU 8
    lda.b <FD_RUN_CNT
    sta.w $4202
    lda.b <FD_DUFRAC
    sta.w $4203                 ; eight cycles to the product
    rep #$20
.ACCU 16
    txa
    xba
    and #$FF00
    sep #$20
.ACCU 8
    ora.b <FD_UFRAC
    rep #$20
.ACCU 16
    clc
    adc.w $4216
    bcs _rn_div0\@
_rn_fit0\@:
    lda.b <FD_RUN_CNT           ; the rest of the span, a block at a time
    cmp #RS_RUN_LEN+1
    bcc +
    lda #RS_RUN_LEN
+   sta.b <FD_RUN_N
    bra _rn_go0\@
_rn_div0\@:
    ; It wraps: the texels left in the row are rem = $FFFF - u, and the
    ; safe count rem / f from the divider, at most count.  Zero means the
    ; next step is the wrap itself.
    txa
    xba
    and #$FF00
    sep #$20
.ACCU 8
    ora.b <FD_UFRAC
    rep #$20
.ACCU 16
    eor #$FFFF
    sta.w $4204
    sep #$20
.ACCU 8
    lda.b <FD_DUFRAC
    sta.w $4206                 ; sixteen cycles to the quotient
    rep #$20
.ACCU 16
    lda.b <FD_RUN_CNT
    nop
    nop
    nop
    nop
    cmp.w $4214                 ; (3 + 4 + 8 + 3 cycles to the read)
    bcc +                       ; the count is the smaller
    lda.w $4214
+   cmp #RS_RUN_LEN+1
    bcc +
    lda #RS_RUN_LEN             ; ...and a block at a time
+   sta.b <FD_RUN_N
    bne _rn_go0\@
    ; A piece of zero: the next step wraps the row.  Fall into _rn_one.

_rn_one\@:
    ; One texel the walk's way: the eight-bit u wraps inside its row.
    sep #$20
.ACCU 8
    lda.b <FD_UFRAC
    clc
    adc.b <FD_DUFRAC
    sta.b <FD_UFRAC
    rep #$20
.ACCU 16
    txa
    sep #$20
.ACCU 8
    adc.b <FD_DUINT             ; A8 = index.low stepped, B = its high byte
    tax
    lda.l TEX,x
    sta.w $2180
    rep #$20
.ACCU 16
    dec.b <FD_RUN_CNT
    beq _rn_exit\@
    sep #$20
.ACCU 8
    jmp _rn_loop\@

_rn_piece\@:
.ACCU 8
    xba
    sta.b <FD_UFRAC             ; the fraction back out of B
    rep #$20
.ACCU 16
    lda.b <FD_RUN_CNT
    beq _rn_exit\@
    sep #$20
.ACCU 8
    jmp _rn_loop\@              ; the next block, or the wrap
_rn_exit\@:
    sep #$20
.ACCU 8
    jmp _rn_done\@

_rn_go0\@:
.ACCU 16
    lda.b <FD_RUN_CNT
    sec
    sbc.b <FD_RUN_N
    sta.b <FD_RUN_CNT
    RS_RUN_ENTER 0, _rn_tab0\@
_rn_b0\@:
    RS_RUN_BLOCK TEX, 0
    jmp _rn_piece\@

_rn_k1\@:
    ; k = 1: a step moves 1 or 2 texels.  The same two tests; a span
    ; that wraps is WALKED WHOLE instead of cut.
    rep #$20
.ACCU 16
    txa
    and #$00FF
    clc
    adc.b <FD_RUN_CNT
    adc.b <FD_RUN_CNT
    cmp #$0100
    bcc _rn_fit1\@
    lda.b <FD_RUN_CNT
    xba
    and #$00FF
    bne _rn_w1\@
    sep #$20
.ACCU 8
    lda.b <FD_RUN_CNT
    sta.w $4202
    lda.b <FD_DUFRAC
    sta.w $4203
    rep #$20
.ACCU 16
    txa
    xba
    and #$FF00
    sep #$20
.ACCU 8
    ora.b <FD_UFRAC
    rep #$20
.ACCU 16
    clc
    adc.w $4216                 ; + count * f
    bcs _rn_w1\@
    sta.b <FD_RUN_TMP
    lda.b <FD_RUN_CNT
    xba                         ; count << 8 (count is under 256 here)
    clc
    adc.b <FD_RUN_TMP           ; + count whole texels
    bcc _rn_fit1\@
_rn_w1\@:
    jmp _rn_walk\@
_rn_fit1\@:
    lda.b <FD_RUN_CNT
    cmp #RS_RUN_LEN+1
    bcc +
    lda #RS_RUN_LEN
+   sta.b <FD_RUN_N
    lda.b <FD_RUN_CNT
    sec
    sbc.b <FD_RUN_N
    sta.b <FD_RUN_CNT
    RS_RUN_ENTER 1, _rn_tab1\@
_rn_b1\@:
    RS_RUN_BLOCK TEX, 1
    jmp _rn_piece\@

_rn_k2\@:
    ; k = 2: a step moves 2 or 3 texels.  The same two tests; a span
    ; that wraps is WALKED WHOLE instead of cut.
    rep #$20
.ACCU 16
    txa
    and #$00FF
    clc
    adc.b <FD_RUN_CNT
    adc.b <FD_RUN_CNT
    adc.b <FD_RUN_CNT
    cmp #$0100
    bcc _rn_fit2\@
    lda.b <FD_RUN_CNT
    xba
    and #$00FF
    bne _rn_w2\@
    sep #$20
.ACCU 8
    lda.b <FD_RUN_CNT
    sta.w $4202
    lda.b <FD_DUFRAC
    sta.w $4203
    rep #$20
.ACCU 16
    txa
    xba
    and #$FF00
    sep #$20
.ACCU 8
    ora.b <FD_UFRAC
    rep #$20
.ACCU 16
    clc
    adc.w $4216                 ; + count * f
    bcs _rn_w2\@
    sta.b <FD_RUN_TMP
    lda.b <FD_RUN_CNT
    xba                         ; count << 8 (count is under 256 here)
    clc
    adc.b <FD_RUN_TMP           ; + count whole texels
    bcs _rn_w2\@
    sta.b <FD_RUN_TMP
    lda.b <FD_RUN_CNT
    xba                         ; count << 8 (count is under 256 here)
    clc
    adc.b <FD_RUN_TMP           ; + count whole texels
    bcc _rn_fit2\@
_rn_w2\@:
    jmp _rn_walk\@
_rn_fit2\@:
    lda.b <FD_RUN_CNT
    cmp #RS_RUN_LEN+1
    bcc +
    lda #RS_RUN_LEN
+   sta.b <FD_RUN_N
    lda.b <FD_RUN_CNT
    sec
    sbc.b <FD_RUN_N
    sta.b <FD_RUN_CNT
    RS_RUN_ENTER 2, _rn_tab2\@
_rn_b2\@:
    RS_RUN_BLOCK TEX, 2
    jmp _rn_piece\@

_rn_k3\@:
    ; k = 3: a step moves 3 or 4 texels.  The same two tests; a span
    ; that wraps is WALKED WHOLE instead of cut.
    rep #$20
.ACCU 16
    txa
    and #$00FF
    clc
    adc.b <FD_RUN_CNT
    adc.b <FD_RUN_CNT
    adc.b <FD_RUN_CNT
    adc.b <FD_RUN_CNT
    cmp #$0100
    bcc _rn_fit3\@
    lda.b <FD_RUN_CNT
    xba
    and #$00FF
    bne _rn_w3\@
    sep #$20
.ACCU 8
    lda.b <FD_RUN_CNT
    sta.w $4202
    lda.b <FD_DUFRAC
    sta.w $4203
    rep #$20
.ACCU 16
    txa
    xba
    and #$FF00
    sep #$20
.ACCU 8
    ora.b <FD_UFRAC
    rep #$20
.ACCU 16
    clc
    adc.w $4216                 ; + count * f
    bcs _rn_w3\@
    sta.b <FD_RUN_TMP
    lda.b <FD_RUN_CNT
    xba                         ; count << 8 (count is under 256 here)
    clc
    adc.b <FD_RUN_TMP           ; + count whole texels
    bcs _rn_w3\@
    sta.b <FD_RUN_TMP
    lda.b <FD_RUN_CNT
    xba                         ; count << 8 (count is under 256 here)
    clc
    adc.b <FD_RUN_TMP           ; + count whole texels
    bcs _rn_w3\@
    sta.b <FD_RUN_TMP
    lda.b <FD_RUN_CNT
    xba                         ; count << 8 (count is under 256 here)
    clc
    adc.b <FD_RUN_TMP           ; + count whole texels
    bcc _rn_fit3\@
_rn_w3\@:
    jmp _rn_walk\@
_rn_fit3\@:
    lda.b <FD_RUN_CNT
    cmp #RS_RUN_LEN+1
    bcc +
    lda #RS_RUN_LEN
+   sta.b <FD_RUN_N
    lda.b <FD_RUN_CNT
    sec
    sbc.b <FD_RUN_N
    sta.b <FD_RUN_CNT
    RS_RUN_ENTER 3, _rn_tab3\@
_rn_b3\@:
    RS_RUN_BLOCK TEX, 3
    jmp _rn_piece\@

_rn_walk\@:
    ; The rest of the span the walk's way (RS_WALK's loop), from the
    ; dispatcher's state.
.ACCU 16
    lda.b <FD_RUN_CNT
    lsr a
    tay                         ; Y = pairs, C = an odd texel first
    txa
    xba
    sep #$20
.ACCU 8
    xba                         ; A8 = u, B = v
    bcc _rn_wpair\@
    RS_WALK_TEXEL TEX
    cpy #0
    beq _rn_wdone\@
_rn_wpair\@:
    RS_WALK_TEXEL TEX
    RS_WALK_TEXEL TEX
    dey
    bne _rn_wpair\@
_rn_wdone\@:
    jmp _rn_done\@
; This instance's entry addresses by piece length n: the last n bodies of
; each block, minus one for the RTS.
_rn_tab0\@:
.REPT RS_RUN_LEN+1 INDEX n
    .dw _rn_b0\@ + (RS_RUN_LEN - n) * RS_RUN_BODY - 1
.ENDR
_rn_tab1\@:
.REPT RS_RUN_LEN+1 INDEX n
    .dw _rn_b1\@ + (RS_RUN_LEN - n) * RS_RUN_BODY - 1
.ENDR
_rn_tab2\@:
.REPT RS_RUN_LEN+1 INDEX n
    .dw _rn_b2\@ + (RS_RUN_LEN - n) * RS_RUN_BODY - 1
.ENDR
_rn_tab3\@:
.REPT RS_RUN_LEN+1 INDEX n
    .dw _rn_b3\@ + (RS_RUN_LEN - n) * RS_RUN_BODY - 1
.ENDR
_rn_done\@:
.ACCU 8
.ENDM

; The whole walk over texture TEX: the bounded run where it pays, the
; two-texel walk otherwise.
.MACRO RS_WALK ARGS TEX
    RS_WALK_SETUP
    lda 16,s                    ; du: up to three whole texels, not negative
    cmp #$0400
    bcs _rw_far\@
    lda 10,s
    cmp #RS_RUN_MIN
    bcs _rw_run\@
_rw_far\@:
    jmp _rw_generic\@
_rw_run\@:
    sta.b <FD_RUN_CNT
    sep #$20
.ACCU 8
    RS_RUN TEX
    jmp _rw_done\@
_rw_generic\@:
.ACCU 16
    lda 10,s
    lsr a
    tay                         ; Y = pairs, C = an odd texel first
    txa
    xba
    sep #$20
.ACCU 8
    xba                         ; A8 = u, B = v: the loop's invariant
    bcc _rw_pair\@
    RS_WALK_TEXEL TEX
    cpy #0
    beq _rw_done\@
_rw_pair\@:
    RS_WALK_TEXEL TEX
    RS_WALK_TEXEL TEX
    dey
    bne _rw_pair\@
_rw_done\@:
    rep #$20
.ACCU 16
.ENDM

; The affine walkers' setup: the four Q8.8 accumulators and the page on the
; fast direct page, WMDATA pointed at the span, Y the count.  Arguments at
; 8,s as above.
.MACRO RS_QUAD_SETUP
    lda #FASTDP
    tcd
    pea $0000
    plb
    plb
    lda 8,s
    sta.w $2181
    sep #$20
.ACCU 8
    lda.l rs_fbbank
    and #$01
    sta.w $2183
    rep #$20
.ACCU 16
    lda 12,s
    sta.b <FD_U
    lda 14,s
    sta.b <FD_V
    lda 16,s
    sta.b <FD_DU
    lda 18,s
    sta.b <FD_DV
    lda 20,s
    sta.b <FD_PAGE
    lda 10,s
    tay
.ENDM

snesSpanFloor:
    php
    rep #$30
    lda 7,s
    bne +
    jmp _sf_out                 ; empty spans are common at the board's edges
+   phb
    phd
    RS_WALK snes_floor_tex
    pld
    plb
_sf_out:
    plp
    rtl

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
    ; walk's thirty-odd.  The row must stay inside one texture row, the
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
    phb
    phd
    RS_WALK snes_board_texture
    pld
    plb
    plp
    rtl

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
    bne +
    jmp _sc_out
+   phb
    phd
    RS_WALK snes_card_tex
    pld
    plb
_sc_out:
    plp
    rtl

;-----------------------------------------------------------------------------
snesSpanCard32:
    php
    rep #$30
    lda 7,s
    bne +
    jmp _s3_out
+   phb
    phd
    lda 14,s                    ; bit 15: which sheet
    bpl +
    jmp _s3_hi
+   RS_WALK snes_card_tex32
    jmp _s3_done
_s3_hi:
    RS_WALK snes_card_tex32b
_s3_done:
    pld
    plb
_s3_out:
    plp
    rtl

;-----------------------------------------------------------------------------
snesSpanCardQuad:
    php
    rep #$30

    lda 7,s
    beq _sq_out
    phb
    phd
    RS_QUAD_SETUP

_sq_loop:
    lda.b <FD_U
    clc
    adc.b <FD_DU
    sta.b <FD_U
    xba                         ; the integer part into the low byte
    and #$000F
    ora.b <FD_PAGE
    sta.b <FD_QTMP
    lda.b <FD_V
    clc
    adc.b <FD_DV
    sta.b <FD_V
    xba
    asl a
    asl a
    asl a
    asl a
    and #$00F0                  ; v.int << 4
    ora.b <FD_QTMP
    tax
    sep #$20
.ACCU 8
    lda.l snes_card_tex,x
    sta.w $2180
    rep #$20
.ACCU 16
    dey
    bne _sq_loop

    pld
    plb
_sq_out:
    plp
    rtl

;-----------------------------------------------------------------------------
snesSpanCardQuad32:
    php
    rep #$30

    lda 7,s
    bne +
    jmp _q3_out
+   phb
    phd
    RS_QUAD_SETUP
    lda 22,s                    ; the sheet
    bne _q3_hi
_q3_loop:
    lda.b <FD_U
    clc
    adc.b <FD_DU
    sta.b <FD_U
    bpl +
    lda #$0000
    sta.b <FD_U
+   cmp #$2000
    bcc +
    lda #$1FFF
    sta.b <FD_U
+
    xba                         ; the integer part into the low byte
    and #$001F
    ora.b <FD_PAGE
    sta.b <FD_QTMP
    lda.b <FD_V
    clc
    adc.b <FD_DV
    sta.b <FD_V
    bpl +
    lda #$0000
    sta.b <FD_V
+   cmp #$2000
    bcc +
    lda #$1FFF
    sta.b <FD_V
+
    xba
    asl a
    asl a
    asl a
    asl a
    asl a
    and #$03E0                  ; v.int << 5
    ora.b <FD_QTMP
    tax
    sep #$20
.ACCU 8
    lda.l snes_card_tex32,x
    sta.w $2180
    rep #$20
.ACCU 16
    dey
    bne _q3_loop
    bra _q3_done

_q3_hi:
    lda.b <FD_U
    clc
    adc.b <FD_DU
    sta.b <FD_U
    bpl +
    lda #$0000
    sta.b <FD_U
+   cmp #$2000
    bcc +
    lda #$1FFF
    sta.b <FD_U
+
    xba                         ; the integer part into the low byte
    and #$001F
    ora.b <FD_PAGE
    sta.b <FD_QTMP
    lda.b <FD_V
    clc
    adc.b <FD_DV
    sta.b <FD_V
    bpl +
    lda #$0000
    sta.b <FD_V
+   cmp #$2000
    bcc +
    lda #$1FFF
    sta.b <FD_V
+
    xba
    asl a
    asl a
    asl a
    asl a
    asl a
    and #$03E0                  ; v.int << 5
    ora.b <FD_QTMP
    tax
    sep #$20
.ACCU 8
    lda.l snes_card_tex32b,x
    sta.w $2180
    rep #$20
.ACCU 16
    dey
    bne _q3_hi

_q3_done:
    pld
    plb
_q3_out:
    plp
    rtl

; Motion mapper.  D = the fast direct page (snes_fastdp.inc), the four
; Q8.8 accumulators and the 24-bit texture pointer at FD_MAP_*; the pointer
; wraps at 256 columns / 128 rows, and WMDATA increments the destination
; for free.  A full Q8.8 carry DDA handles increments of either sign,
; including >1 texel.
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
    lda #FASTDP
    tcd
    ; PHP + PHD add three bytes to the caller's stack arguments.
    lda 11,s
    sta.b <FD_MAP_U             ; u fraction/integer
    lda 13,s
    sta.b <FD_MAP_V             ; v fraction/integer
    lda 15,s
    sta.b <FD_MAP_DU            ; du
    lda 17,s
    sta.b <FD_MAP_DV            ; dv
    ; Pointer base must be page-aligned.  The texture has its own bank's
    ; lower 32 KB (asserted by the link/verification).
    sep #$20
.ACCU 8
    lda.b <FD_MAP_U+1
    sta.b <FD_MAP_PTR
    lda.b <FD_MAP_V+1
    and #$7F
    sta.b <FD_MAP_PTR+1
    lda #$7F
    sta.b <FD_MAP_PTR+2
_fq_loop:
    lda.b <FD_MAP_U
    clc
    adc.b <FD_MAP_DU
    sta.b <FD_MAP_U
    lda.b <FD_MAP_PTR
    adc.b <FD_MAP_DU+1
    sta.b <FD_MAP_PTR
    lda.b <FD_MAP_V
    clc
    adc.b <FD_MAP_DV
    sta.b <FD_MAP_V
    lda.b <FD_MAP_PTR+1
    adc.b <FD_MAP_DV+1
    and #$7F
    sta.b <FD_MAP_PTR+1
    lda.b [FD_MAP_PTR]
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
; THE ROW'S STATE IS ON THE FAST PAGE.  The first version kept every term
; in fr_* and read each with a long load -- four bytes of FastROM fetch
; and two slow WRAM bytes, forty master cycles an access, a hundred-odd
; accesses a row -- and called snesQMul twice a row through the stack.
; Here what changes a row lives in DMA-register words (snes_fastdp.inc):
; what the walker call must not clobber on channel 2, the row's scratch
; on channels 1 and 3 (the walker owns those inside the call, and nothing
; of the row's is needed after it).  The two Q8.8 products are the CPU
; multiplier's partials inline (FR_UMUL), the sign of `a` handled around
; it.  The frame's constants stay in fr_* and are read `.w` under DB = 0.
;
; Persistent across the walker (channel 2):
.DEFINE FD_FR_HALF      $21     ; the slab's half width on the row, Q8.8
.DEFINE FD_FR_DENOM16   $23     ; sin(pitch) + sy*cos(pitch), Q4.12
.DEFINE FD_FR_A16       $25     ; cos(pitch) - sy*sin(pitch), Q4.12
.DEFINE FD_FR_Y         $27
.DEFINE FD_FR_HEIGHT    $29     ; the camera's height, Q8.8 (a constant, parked)
; The row's scratch (channel 1 -- the walker's own words -- and channel 3):
.DEFINE FD_FR_TMP       $11     ; the row's half width, then recip[denom]
.DEFINE FD_FR_X0        $13
.DEFINE FD_FR_X1        $15
.DEFINE FD_FR_DEPTH     $17
.DEFINE FD_FR_DTEX      $19
.DEFINE FD_FR_TU        $31
.DEFINE FD_FR_TV        $33
.DEFINE FD_FR_DENOM     $35
.DEFINE FD_FR_A         $37     ; |a|, the sign in FD_FR_SGN
.DEFINE FD_FR_SGN       $39

; R = (A * B) >> 8, A and B unsigned Q8.8 fast words (R distinct from
; both): the four CPU partials, each started and then left alone for its
; eight cycles while the next operand is fetched -- snesQMul's schedule
; without its stack frame and sign work.  Enters and leaves A16.
.MACRO FR_UMUL ARGS A, B, R
    sep #$20
.ACCU 8
    lda.b <A
    sta.w $4202
    lda.b <B
    sta.w $4203                 ; al * bl            (cycles from the write)
    lda.b <B+1                  ; bh                  3
    xba                         ; B = bh              3
    stz.b <R+1                  ;                     3
    nop                         ;                     2
    lda.w $4217                 ; (al * bl) >> 8      read at 11 + 4
    sta.b <R
    xba                         ; A = bh
    sta.w $4203                 ; al * bh
    rep #$20                    ;                     3
.ACCU 16
    lda.b <R                    ;                     4
    clc                         ;                     2
    nop                         ;                     2
    adc.w $4216                 ; read at 11 + 4
    sta.b <R
    sep #$20
.ACCU 8
    lda.b <A+1                  ; ah
    sta.w $4202
    lda.b <B                    ; bl
    sta.w $4203                 ; ah * bl
    rep #$20                    ;                     3
.ACCU 16
    lda.b <R                    ;                     4
    clc                         ;                     2
    nop                         ;                     2
    adc.w $4216                 ; read at 11 + 4
    sta.b <R
    sep #$20
.ACCU 8
    lda.b <B+1                  ; bh
    sta.w $4203                 ; ah * bh
    rep #$20                    ;                     3
.ACCU 16
    nop                         ;                     2
    nop                         ;                     2
    nop                         ;                     2
    lda.w $4216                 ; read at 9 + 4
    xba
    and #$FF00
    clc
    adc.b <R
    sta.b <R
.ENDM

snesFloorRowsPitch:
    php
    rep #$30
    phb
    phd
    pea $0000
    plb
    plb                         ; DB = 0: the fr_* constants by .w
    lda #FASTDP
    tcd
    lda 8,s
    sta.b <FD_FR_Y
    lda 10,s
    sta.w fr_yend
    lda.w fr_half
    sta.b <FD_FR_HALF
    lda.w fr_denom16
    sta.b <FD_FR_DENOM16
    lda.w fr_a16
    sta.b <FD_FR_A16
    lda.w fr_height
    sta.b <FD_FR_HEIGHT
_fr_row:
    lda.b <FD_FR_Y
    cmp.w fr_yend
    bcc +
    jmp _fr_done
+
    ; The half width for this row, then the step for the next.
    lda.b <FD_FR_HALF
    sta.b <FD_FR_TMP
    clc
    adc.w fr_dhalf
    sta.b <FD_FR_HALF
    ; The camera terms for this row, then the step for the next.
    lda.b <FD_FR_DENOM16
    sta.b <FD_FR_DENOM
    clc
    adc.w fr_dstep
    sta.b <FD_FR_DENOM16
    lda.b <FD_FR_A16
    sta.b <FD_FR_A
    clc
    adc.w fr_astep
    sta.b <FD_FR_A16
    lda.b <FD_FR_DENOM
    cmp #$8000
    ror a
    cmp #$8000
    ror a
    cmp #$8000
    ror a
    cmp #$8000
    ror a
    sta.b <FD_FR_DENOM
    ; Above the horizon, or too far below it for the table: nothing.
    cmp #3
    bcc _fr_skip0
    cmp #1024
    bcs _fr_skip0
    ; The row's span: 128 -/+ half, in pixels, clipped to the viewport.
    lda.b <FD_FR_TMP
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
+   sta.b <FD_FR_TMP
    lda.w fr_halfw
    sec
    sbc.b <FD_FR_TMP
    bpl +
    lda #0
+   sta.b <FD_FR_X0
    lda.w fr_halfw
    clc
    adc.b <FD_FR_TMP
    cmp.w fr_w
    bcc +
    beq +
    lda.w fr_w
+   sta.b <FD_FR_X1
    cmp.b <FD_FR_X0
    bcc _fr_skip
    beq _fr_skip
    ; Note the span for the converter: per cell row (eight lines of either
    ; frame), the first and last pixel columns touched.
    lda.b <FD_FR_Y
    lsr a
    lsr a
    lsr a
    asl a
    tax
    sep #$20
.ACCU 8
    lda.b <FD_FR_X0
    cmp.l snes_conv_rowspan,x
    bcs +
    sta.l snes_conv_rowspan,x
+   lda.b <FD_FR_X1
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
    ; depth = height * recip[denom]: both positive.
    lda.b <FD_FR_DENOM
    asl a
    tax
    lda.l snes_recip_plane,x
    sta.b <FD_FR_TMP
    FR_UMUL FD_FR_HEIGHT, FD_FR_TMP, FD_FR_DEPTH
    ; z = camz + depth * a: a is Q4.12 >> 4, signed; the product of its
    ; magnitude, negated back.
    lda.b <FD_FR_A
    cmp #$8000
    ror a
    cmp #$8000
    ror a
    cmp #$8000
    ror a
    cmp #$8000
    ror a
    sta.b <FD_FR_SGN
    bpl +
    eor #$FFFF
    inc a
+   sta.b <FD_FR_A
    FR_UMUL FD_FR_DEPTH, FD_FR_A, FD_FR_TU
    lda.b <FD_FR_SGN
    bpl +
    lda.b <FD_FR_TU
    eor #$FFFF
    inc a
    sta.b <FD_FR_TU
+   lda.b <FD_FR_TU
    clc
    adc.w fr_camz
    ; tv = z << 5, kept to the texture's 128 rows
    asl a
    asl a
    asl a
    asl a
    asl a
    and #$7F00
    sta.b <FD_FR_TV
    ; dtex = (depth + 2) >> 2 at 1:1, (depth + 1) >> 1 on the motion frame
    lda.b <FD_FR_DEPTH
    ldx.w fr_sub
    beq +
    inc a
    lsr a
+   inc a
    lsr a
    sta.b <FD_FR_DTEX
    ; tu = ubase + dtex * (x0 - 128) + dtex / 2: dtex is a positive word,
    ; x0 - 128 a signed byte, and the low sixteen bits of their product
    ; are two CPU partials with the byte taken unsigned, less dtex * 256
    ; when it is negative (snesMul16x8's arithmetic; not the PPU
    ; multiplier -- its latch is BG1VOFS's, which the motion frame's
    ; doubling HDMA writes every other line).
    sep #$20
.ACCU 8
    lda.b <FD_FR_X0
    sec
    sbc.w fr_halfw
    xba                         ; B = the offset
    lda.b <FD_FR_DTEX
    sta.w $4202
    xba
    sta.w $4203                 ; dtex.lo * offset   (cycles from the write)
    xba                         ;                     3
    lda.b <FD_FR_DTEX+1         ;                     3
    xba                         ; A = offset, B = hi  3
    rep #$20                    ;                     3
.ACCU 16
    tax                         ;                     2
    nop                         ;                     2
    lda.w $4216                 ; the low partial, read at 16 + 4
    sta.b <FD_FR_TU
    txa
    sep #$20
.ACCU 8
    xba
    sta.w $4202                 ; dtex.hi
    xba
    sta.w $4203                 ; dtex.hi * offset
    cmp #$80                    ; C = the offset is negative   2
    rep #$20                    ;                              3
.ACCU 16
    bcc +
    lda.b <FD_FR_DTEX
    xba
    and #$FF00
    eor #$FFFF
    sec
    adc.b <FD_FR_TU             ; less dtex.lo * 256
    sta.b <FD_FR_TU
    bra ++
+   nop                         ; the other arm is longer than eight cycles
    nop
++  lda.w $4216                 ; read at 9 + 4 at the soonest
    xba
    and #$FF00
    clc
    adc.b <FD_FR_TU
    clc
    adc.w fr_ubase
    sta.b <FD_FR_TU
    lda.b <FD_FR_DTEX
    lsr a
    clc
    adc.b <FD_FR_TU
    ; The walker steps before it reads: hand it the texel before the first.
    sec
    sbc.b <FD_FR_DTEX
    sta.b <FD_FR_TU
    ; snesSpanFloorTex(origin + y * 256 + x0, x1 - x0,
    ;                  tv | (tu >> 8), tu & 255, dtex)
    lda.b <FD_FR_DTEX
    pha
    lda.b <FD_FR_TU
    and #$00FF
    pha
    lda.b <FD_FR_TU
    xba
    and #$00FF
    ora.b <FD_FR_TV
    pha
    lda.b <FD_FR_X1
    sec
    sbc.b <FD_FR_X0
    pha
    lda.b <FD_FR_Y
    xba
    and #$FF00                  ; y * 256
    ldx.w fr_sub
    bne +
    lsr a                       ; y * 128 on the motion frame
+   clc
    adc.b <FD_FR_X0
    clc
    adc.w fr_origin
    pha
    jsl snesSpanFloorTex
    tsa
    clc
    adc #10
    tas
_fr_next:
    inc.b <FD_FR_Y
    jmp _fr_row
_fr_done:
    ; The stepped terms back for the caller's next call (the job's rows
    ; come a few at a time).
    lda.b <FD_FR_HALF
    sta.w fr_half
    lda.b <FD_FR_DENOM16
    sta.w fr_denom16
    lda.b <FD_FR_A16
    sta.w fr_a16
    pld
    plb
    plp
    rtl

; void snesFloorYawSetup(s16 cy, s16 sn, s16 eye_cy, s16 eye_sn,
;                        s16 height, s16 camz, u16 origin, u16 sub)
;
; The per-frame terms of a yawed camera's floor rows: the yaw's cosine and
; sine, the camera's x through both, its height and z, the frame.
snesFloorYawSetup:
    php
    rep #$30
    lda 19,s
    and #$0001
    sta.l fr_sub
    bne +
    lda #64
    sta.l fr_halfw
    bra ++
+   lda #128
    sta.l fr_halfw
++
    lda 5,s
    sta.l fr_cy
    lda 7,s
    sta.l fr_sn
    lda 9,s
    sta.l fr_eyecy
    lda 11,s
    sta.l fr_eyesn
    lda 13,s
    sta.l fr_height
    lda 15,s
    sta.l fr_camz
    lda 17,s
    sta.l fr_origin
    plp
    rtl

; void snesFloorRowYaw(s16 y, s16 x0, s16 x1, s16 denom, s16 a)
;
; ONE ROW OF A YAWED CAMERA'S FLOOR: the turn animation's frames.  The C
; row loop (snes_board3d.c texture_quad) walks the projected slab's two
; edge chains and clips the row to the viewport, and hands the row here
; with its two camera terms; everything from the camera maths to the
; walker was four hundred instructions of 816-tcc a row, through the stack
; and six calls, and a turn frame's mapping was twelve fields.
;
;   depth = height * recip_plane[denom]              (Q8.8)
;   z     = camz + depth * a
;   dtex  = (depth + 2) >> 2 at 1:1, (depth + 1) >> 1 on the motion frame
;   du    = dtex * cos(yaw),  dv = -(dtex * sin(yaw))    (texels a pixel)
;   tu    = ((eye_cy + z * sin(yaw)) << 5) + U_CENTRE   (the row's middle)
;   tv    = (z * cos(yaw) - eye_sn) << 5
;   tu   += du * (x0 - halfw) + du / 2,  tv likewise    (the first pixel)
;   note the span; walk x1 - x0 texels from (tu - du, tv - dv)
;
; Every product and shift is the C's: the frames are byte for byte what
; texture_quad's loop produced.
snesFloorRowYaw:
    php
    rep #$30
    phb
    pea $0000
    plb
    plb
    lda 12,s                    ; denom: nothing above the horizon or past
    cmp #3                      ; the reciprocal table
    bcc +
    cmp #1024
    bcc ++
+   jmp _fy_out
++  sta.l fr_denom
    lda 14,s
    sta.l fr_a
    lda 6,s
    sta.l fr_y
    lda 8,s
    sta.l fr_x0
    lda 10,s
    sta.l fr_x1
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
    sta.l fr_z
    ; dtex
    lda.l fr_depth
    ldx.w fr_sub
    beq +
    inc a
    inc a
    lsr a
    lsr a
    bra ++
+   inc a
    lsr a
++  sta.l fr_dtex
    ; du = dtex * cy
    lda.l fr_cy
    pha
    lda.l fr_dtex
    pha
    jsl snesQMul
    tsa
    clc
    adc #4
    tas
    lda.b tcc__r0
    sta.l fr_du
    ; dv = -(dtex * sn)
    lda.l fr_sn
    pha
    lda.l fr_dtex
    pha
    jsl snesQMul
    tsa
    clc
    adc #4
    tas
    lda.b tcc__r0
    eor #$FFFF
    inc a
    sta.l fr_dv
    ; tu = ((eye_cy + z * sn) << 5) + U_CENTRE
    lda.l fr_sn
    pha
    lda.l fr_z
    pha
    jsl snesQMul
    tsa
    clc
    adc #4
    tas
    lda.b tcc__r0
    clc
    adc.l fr_eyecy
    asl a
    asl a
    asl a
    asl a
    asl a
    clc
    adc #FR_U_CENTRE << 8
    sta.l fr_tu
    ; tv = (z * cy - eye_sn) << 5
    lda.l fr_cy
    pha
    lda.l fr_z
    pha
    jsl snesQMul
    tsa
    clc
    adc #4
    tas
    lda.b tcc__r0
    sec
    sbc.l fr_eyesn
    asl a
    asl a
    asl a
    asl a
    asl a
    sta.l fr_tv
    ; The offset from the row's middle to the first pixel's centre, in
    ; steps: du * (x0 - halfw) + du / 2, and the same for dv.
    lda.l fr_x0
    sec
    sbc.l fr_halfw
    sta.l fr_tmp2
    pha
    lda.l fr_du
    pha
    jsl snesMul16x8
    tsa
    clc
    adc #4
    tas
    lda.l fr_du
    cmp #$8000
    ror a                       ; du >> 1, arithmetic
    clc
    adc.b tcc__r0
    clc
    adc.l fr_tu
    sec
    sbc.l fr_du                 ; the walker steps before it reads
    sta.l fr_tu
    lda.l fr_tmp2
    pha
    lda.l fr_dv
    pha
    jsl snesMul16x8
    tsa
    clc
    adc #4
    tas
    lda.l fr_dv
    cmp #$8000
    ror a
    clc
    adc.b tcc__r0
    clc
    adc.l fr_tv
    sec
    sbc.l fr_dv
    sta.l fr_tv
    ; Note the span for the converter: per cell row (eight lines), the
    ; first and last pixel columns touched.
    lda.l fr_y
    lsr a
    lsr a
    lsr a
    asl a
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
    ; snesSpanFloorQuad(origin + y * stride + x0, x1 - x0, tu, tv, du, dv)
    lda.l fr_dv
    pha
    lda.l fr_du
    pha
    lda.l fr_tv
    pha
    lda.l fr_tu
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
    jsl snesSpanFloorQuad
    tsa
    clc
    adc #12
    tas
_fy_out:
    plb
    plp
    rtl

; A world-space board image, shared by every camera pose.  The second half
; repeats the 64-row floor pattern; card stamps then cover their own slots.
snesBoardTextureClear:
    ; TWO DMAs, ROM TO WMDATA: the 16 KB floor pattern from ROM into each
    ; half of the 32 KB world image at eight master cycles a byte.  The
    ; 16-bit copy loop this replaces read WRAM back into WRAM at twenty-six
    ; a byte, a field and a half every time the board changed.  Channel 0,
    ; the main thread's.
    php
    rep #$30
    lda #snes_board_texture
    sta.l $2181
    sep #$20
.ACCU 8
    lda #$01
    sta.l $2183                 ; bank $7F
    lda #$00
    sta.l $4300                 ; CPU -> PPU, one register, auto increment
    lda #$80
    sta.l $4301                 ; $2180
    lda #:snes_floor_tex
    sta.l $4304
    rep #$20
.ACCU 16
    lda #snes_floor_tex
    sta.l $4302
    lda #16384
    sta.l $4305
    sep #$20
.ACCU 8
    lda #$01
    sta.l $420B
    rep #$20
.ACCU 16
    lda #snes_floor_tex         ; the second half: WMADD carried on
    sta.l $4302
    lda #16384
    sta.l $4305
    sep #$20
.ACCU 8
    lda #$01
    sta.l $420B
    rep #$20
.ACCU 16
    plp
    rtl

; centre = (world texture v << 8) | u; face is a 32x32 ROM image off the
; same two 1:1 sheets the resting board draws from, so a card looks the same
; from every camera pose.  width x height is the stamp's footprint in world
; texels; the face is resampled to it.
;
; A ROW OF THE STAMP IS A CONSTANT-v WALK, the same one the floor and the
; resting cards use (RS_WALK_TEXEL): the face is 32 texels wide and the
; stamp at most 32, so u never leaves its row and the index's low byte
; steps it -- u in Q8.8 texels, du = 32 / width -- while the world image is
; written through WMDATA.
snesBoardTextureCard:
    php
    rep #$30
    phb
    phd
    lda #FASTDP
    tcd
    pea $0000
    plb
    plb                         ; DB = 0: the ports by absolute address
    ; Arguments at 8,s: centre, face, flip, width, height.
    lda 10,s
    and #$003F                  ; (face & 63) << 10: the page in its sheet
    xba
    asl a
    asl a
    sta.b <FD_PAGE
    lda 10,s
    and #$0040                  ; faces from 64 up are on the second sheet
    sta.b <FD_BT_SHEET
    lda 14,s
    sta.b <FD_BT_WIDTH
    lsr a
    sta.b <FD_QTMP
    lda 8,s
    sec
    sbc.b <FD_QTMP
    and #$00FF
    sta.b <FD_BT_DST
    lda 16,s
    sta.b <FD_BT_HEIGHT
    lsr a
    xba
    sta.b <FD_QTMP
    lda 8,s
    sec
    sbc.b <FD_QTMP
    and #$7F00
    ora.b <FD_BT_DST
    sta.b <FD_BT_DST
    ; du = 32 texels / width in Q8.8: 8192 / width.
    lda #8192
    sta.w $4204
    sep #$20
.ACCU 8
    lda.b <FD_BT_WIDTH
    sta.w $4206
    lda #$01
    sta.w $2183                 ; the world image's bank, $7F
    nop
    nop
    nop
    nop
    nop
    nop
    rep #$20
.ACCU 16
    lda.w $4214
    sta.b <FD_DU                ; fraction low, whole texels high
    ; dv = 32 rows / height in Q4.12 (4096 / height), signed by the flip.
    lda #4096
    sta.w $4204
    sep #$20
.ACCU 8
    lda.b <FD_BT_HEIGHT
    sta.w $4206
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
    lda.w $4214
    sta.b <FD_BT_DV
    lda #0
    sta.b <FD_BT_V
    lda 12,s                    ; flip
    bne +
    lda #4095
    sta.b <FD_BT_V
    lda.b <FD_BT_DV
    eor #$FFFF
    inc a
    sta.b <FD_BT_DV
+   sep #$20
.ACCU 8
    lda.b <FD_DU+1
    sta.b <FD_DUINT
    lda.b <FD_DU
    sta.b <FD_DUFRAC
    rep #$20
.ACCU 16
    ; A ROW OF THE STAMP IS ONE BOUNDED RUN, entered the same way every
    ; row.  u_i = i*du for i < width and (width-1)*floor(8192/width) is
    ; under 8192, so u never reaches texel 32 and the low byte never wraps:
    ; no dispatcher.  The run is a 32-body block of the RS_RUN kind with
    ; k = 1 baked in, entered (width-1) bodies from its end; the entry is
    ; fixed for the stamp.  Texel 0 itself is emitted straight from the
    ; row start and the run steps the other width-1 from u = 0.  An entry
    ; of zero means the walk instead: two whole texels a step (a footprint
    ; under seventeen), or a footprint of one.
    stz.b <FD_BT_ENTRY
    lda.b <FD_DU
    cmp #$0200
    bcs _bt_row
    lda.b <FD_BT_WIDTH
    dec a
    beq _bt_row
    sta.b <FD_BT_RUN            ; width - 1: the run's length, X's advance
    asl a
    asl a
    asl a
    asl a                       ; * 16
    sec
    sbc.b <FD_BT_RUN            ; * 15: bytes of block before the entry
    eor #$FFFF
    sec
    adc #_bt_blk_lo+32*RS_RUN_BODY-1
    ldy.b <FD_BT_SHEET
    beq +
    clc
    adc #_bt_blk_hi-_bt_blk_lo
+   sta.b <FD_BT_ENTRY
_bt_row:
    ; v is Q4.12 over the face: texel row = v >> 7, and a row is 32 bytes.
    lda.b <FD_BT_V
    lsr a
    lsr a
    and #$03E0
    ora.b <FD_PAGE
    tax                         ; X = page | row << 5 | u = 0
    lda.b <FD_BT_DST
    sta.w $2181                 ; the row's first world texel
    ldy.b <FD_BT_WIDTH
    lda.b <FD_BT_ENTRY
    bne +
    jmp _bt_walk
+   pha
    sep #$20
.ACCU 8
    lda.b <FD_BT_SHEET
    bne +
    lda.l snes_card_tex32,x     ; texel 0 as it is
    sta.w $2180
    bra ++
+   lda.l snes_card_tex32b,x
    sta.w $2180
++  rep #$20
.ACCU 16
    txa
    clc
    adc.b <FD_BT_RUN            ; X on by the run's whole texels
    tax
    sep #$20
.ACCU 8
    lda #0
    xba                         ; B = the fraction, 0
    rts                         ; into the sheet's block
_bt_blk_lo:
.REPT 32 INDEX i
    xba
    clc
    adc.b <FD_DUFRAC
    xba
    bcc +
    inx
+   lda.l snes_card_tex32-(31-i),x
    sta.w $2180
.ENDR
    jmp _bt_row_done
_bt_blk_hi:
.REPT 32 INDEX i
    xba
    clc
    adc.b <FD_DUFRAC
    xba
    bcc +
    inx
+   lda.l snes_card_tex32b-(31-i),x
    sta.w $2180
.ENDR
    jmp _bt_row_done
_bt_walk:
.ACCU 16
    ; The walker steps before it reads: start one du before texel 0.  The
    ; low byte alone is stepped, so the borrow stays out of the page.
    txa
    xba
    sep #$20
.ACCU 8
    xba                         ; A8 = the index's low byte, B = its page
    sta.b <FD_QTMP
    lda #0
    sec
    sbc.b <FD_DUFRAC
    sta.b <FD_UFRAC
    lda.b <FD_QTMP
    sbc.b <FD_DUINT             ; C from the fraction's borrow
    tax
    lda.b <FD_BT_SHEET
    bne _bt_pixel_hi
_bt_pixel:
    RS_WALK_TEXEL snes_card_tex32
    dey
    bne _bt_pixel
    bra _bt_row_done
_bt_pixel_hi:
    RS_WALK_TEXEL snes_card_tex32b
    dey
    bne _bt_pixel_hi
_bt_row_done:
    rep #$20
.ACCU 16
    lda.b <FD_BT_V
    clc
    adc.b <FD_BT_DV
    sta.b <FD_BT_V
    lda.b <FD_BT_DST
    clc
    adc #256
    and #$7FFF
    sta.b <FD_BT_DST
    dec.b <FD_BT_HEIGHT
    beq +
    jmp _bt_row
+   pld
    plb
    plp
    rtl

;-----------------------------------------------------------------------------
; void snesCardRows(void)
;
; THE RESTING CARDS, ROW BY ROW, with no C between the rows or the spans.
; snesDrawCardRow keeps the per-row-of-slots setup -- two divides and three
; edge slopes -- and hands the cr_* words over; from there every screen row
; is: three edge accumulators stepped and latched, one reciprocal lookup,
; one Q8.8 product for the depth and one for the texel step, the card's v
; from the depth, and then up to five spans walked outwards from the
; centre column, each of them a clip to the viewport, at most one product
; (a card cut by the side of the frame), and the constant-v walker.  The C
; this replaces was ~4,500 master cycles a span and ~2,500 a row on top,
; four times the walker's own cost over a full board.
;
; Signed comparisons are (a - b) with the sign corrected by V, so an edge
; sitting exactly at the limit compares the way the C did.
;-----------------------------------------------------------------------------

; A >>= N arithmetic.
.MACRO CR_SAR ARGS N
.REPT N
    cmp #$8000
    ror a
.ENDR
.ENDM

; A = a signed 16-bit difference already in A; leaves N (and Z) describing
; the true sign, overflow included.
.MACRO CR_FIXSIGN
    bvc +
    eor #$8000
+
.ENDM

snesCardRows:
    php
    rep #$30
    phb
    pea $0000
    plb
    plb                         ; DB = 0: the cr_* words by absolute address
_cr_row:
    lda.w cr_y
    cmp.w cr_yend
    bcc +
    jmp _cr_done
+
    ; The left edge: stepped until it latches at the side of the viewport.
    lda.w cr_l_off
    bne _cr_l_done
    lda.w cr_acc_l
    clc
    adc.w cr_step_l
    sta.w cr_acc_l
    sec
    sbc.w cr_limit
    CR_FIXSIGN
    bpl _cr_l_latch             ; acc >= limit
    lda.w cr_acc_l
    clc
    adc.w cr_limit
    CR_FIXSIGN
    beq _cr_l_latch             ; acc <= -limit
    bpl _cr_l_done
_cr_l_latch:
    lda #1
    sta.w cr_l_off
_cr_l_done:
    ; The right edge, the same.
    lda.w cr_r_off
    bne _cr_r_done
    lda.w cr_acc_r
    clc
    adc.w cr_step_r
    sta.w cr_acc_r
    sec
    sbc.w cr_limit
    CR_FIXSIGN
    bpl _cr_r_latch
    lda.w cr_acc_r
    clc
    adc.w cr_limit
    CR_FIXSIGN
    beq _cr_r_latch
    bpl _cr_r_done
_cr_r_latch:
    lda #1
    sta.w cr_r_off
_cr_r_done:
    ; The pitch only grows, so it latches on one side.
    lda.w cr_p_off
    bne _cr_p_done
    lda.w cr_acc_p
    clc
    adc.w cr_step_p
    sta.w cr_acc_p
    sec
    sbc.w cr_limit
    CR_FIXSIGN
    bmi _cr_p_done
    lda #1
    sta.w cr_p_off
_cr_p_done:
    ; Rows above the patch are stepped but not drawn.
    lda.w cr_y
    cmp.w cr_clip_y0
    bcs +
    jmp _cr_next
+
    ; depth = hf * recip[rows_below], the table for the frame's row pitch.
    lda.w cr_rows
    asl a
    tax
    lda.w cr_sub
    beq +
    lda.l snes_recip_row2,x
    bra ++
+   lda.l snes_recip_row,x
++  pha
    lda.w cr_hf
    pha
    jsl snesMulHi
    tsa
    clc
    adc #4
    tas
    lda.b tcc__r0
    sta.w cr_depth
    ; Only the rows between the slot's far and near edges carry the card.
    ; Both edges are positive, so a depth past a signed word (a row right
    ; under the horizon) fails the far test exactly as the C's signed one.
    cmp.w cr_d_far
    beq +
    bcs _cr_skip
+   cmp.w cr_d_near
    bcs +
_cr_skip:
    jmp _cr_next
+
    ; du = depth * du_k, Q8.8
    lda.w cr_du_k
    pha
    lda.w cr_depth
    pha
    jsl snesQMul
    tsa
    clc
    adc #4
    tas
    lda.b tcc__r0
    sta.w cr_du
    ; v: the card is one unit deep and 32 (or 16) texels tall, so the row
    ; of texels is the depth into the card, Q8.8, taken down to a byte.
    lda.w cr_d_far
    sec
    sbc.w cr_depth
    cmp #256
    bcc +
    lda #255
+   ldx.w cr_sub
    beq +
    and #$00F8                  ; (v << 5 >> 3) & $03E0 on the 32x32 sheet
    asl a
    asl a
    bra ++
+   and #$00F0                  ; (v << 4 >> 4) & $00F0 on the 16x16
++  eor.w cr_flip
    sta.w cr_texv

    ; Outwards from the centre: right first, then left.
    lda.w cr_acc_l
    sta.w cr_l
    lda.w cr_acc_r
    sta.w cr_r
    ldx #2
_cr_right:
    lda.w cr_l
    sec
    sbc.w cr_limit
    CR_FIXSIGN
    bpl _cr_right_done          ; l >= limit: off the side
    jsr _cr_span
    lda.w cr_limit
    sec
    sbc.w cr_acc_p
    sta.w cr_tmp
    lda.w cr_l
    sec
    sbc.w cr_tmp
    CR_FIXSIGN
    beq +
    bpl _cr_right_done          ; l > limit - pitch: the next would wrap
+   lda.w cr_l
    clc
    adc.w cr_acc_p
    sta.w cr_l
    lda.w cr_r
    clc
    adc.w cr_acc_p
    sta.w cr_r
    inx
    cpx #5
    bcc _cr_right
_cr_right_done:
    lda.w cr_acc_l
    sec
    sbc.w cr_acc_p
    sta.w cr_l
    lda.w cr_acc_r
    sec
    sbc.w cr_acc_p
    sta.w cr_r
    ldx #1
_cr_left:
    lda.w cr_r
    clc
    adc.w cr_limit
    CR_FIXSIGN
    bmi _cr_left_done           ; r <= -limit
    beq _cr_left_done
    jsr _cr_span
    lda.w cr_acc_p
    sec
    sbc.w cr_limit
    sta.w cr_tmp
    lda.w cr_r
    sec
    sbc.w cr_tmp
    CR_FIXSIGN
    bmi _cr_left_done           ; r < pitch - limit
    lda.w cr_l
    sec
    sbc.w cr_acc_p
    sta.w cr_l
    lda.w cr_r
    sec
    sbc.w cr_acc_p
    sta.w cr_r
    dex
    bpl _cr_left
_cr_left_done:
_cr_next:
    inc.w cr_y
    inc.w cr_rows
    lda.w cr_base
    clc
    adc.w cr_stride
    sta.w cr_base
    jmp _cr_row
_cr_done:
    plb
    plp
    rtl

; One card's span on the row: column X of cr_faces between the Q8.8 edges
; cr_l and cr_r.  The row has already produced the depth, the texel step
; and v.  Preserves X.
_cr_span:
    lda.w cr_faces,x
    and #$00FF
    cmp #$00FF
    bne +
    rts                         ; an empty slot
+   sta.w cr_face
    phx
    ; The edges in pixels: half_w + (edge >> 7) at 1:1, >> 8 on the motion
    ; frame.
    lda.w cr_l
    ldx.w cr_sub
    beq +
    CR_SAR 7
    bra ++
+   CR_SAR 8
++  clc
    adc.w cr_halfw
    sta.w cr_xl
    bpl +
    lda #0
+   sta.w cr_x0
    lda.w cr_r
    ldx.w cr_sub
    beq +
    CR_SAR 7
    bra ++
+   CR_SAR 8
++  clc
    adc.w cr_halfw
    cmp.w cr_w
    bcc +
    lda.w cr_w
+   sta.w cr_x1
    cmp.w cr_x0
    beq +
    bcs ++
+   jmp _cs_done                ; nothing of it inside the viewport
++
    ; A span that starts on the card's own left edge starts at texel zero;
    ; one cut by the side of the viewport pays a product for where it
    ; starts.  Either way the walker steps before it reads, so it is
    ; handed the texel BEFORE the first: without that a card loses its
    ; left keyline column.
    lda.w cr_xl
    bpl _cs_u_zero
    eor #$FFFF
    inc a                       ; x0 - xl, x0 being 0
    pha
    lda.w cr_du
    pha
    jsl snesMulLo
    tsa
    clc
    adc #4
    tas
    lda.b tcc__r0
    bmi _cs_u_zero              ; past the card: clamp to its first texel
    cmp.w cr_du
    bcc _cs_u_zero
    sbc.w cr_du                 ; C is set
    bra _cs_u
_cs_u_zero:
    lda #0
_cs_u:
    ldx.w cr_sub
    beq _cs_16
    ; The 32x32 sheets: the face's 1 KB page in one of two sheets.
    cmp #$2000
    bcc +
    lda #$1FFF
+   sta.w cr_u
    ; snesSpanCard32(base + x0, x1 - x0,
    ;                ((face & 63) << 10) | texv | (u >> 8),
    ;                (u & 255) | (face >= 64 ? $8000 : 0), du)
    lda.w cr_du
    pha
    lda.w cr_u
    and #$00FF
    ldx.w cr_face
    cpx #64
    bcc +
    ora #$8000
+   pha
    lda.w cr_face
    and #$003F
    xba
    asl a
    asl a                       ; (face & 63) << 10
    ora.w cr_texv
    sta.w cr_tmp
    lda.w cr_u
    xba
    and #$00FF                  ; u >> 8, at most $1F
    ora.w cr_tmp
    pha
    lda.w cr_x1
    sec
    sbc.w cr_x0
    pha
    lda.w cr_base
    clc
    adc.w cr_x0
    pha
    jsl snesSpanCard32
    tsa
    clc
    adc #10
    tas
    bra _cs_done
_cs_16:
    ; The 16x16 sheet: the face's 256-byte page.
    cmp #$1000
    bcc +
    lda #$0FFF
+   sta.w cr_u
    ; snesSpanCard(base + x0, x1 - x0, (face << 8) | texv | (u >> 8),
    ;              u & 255, du)
    lda.w cr_du
    pha
    lda.w cr_u
    and #$00FF
    pha
    lda.w cr_face
    xba                         ; face << 8 (the high byte was clear)
    ora.w cr_texv
    sta.w cr_tmp
    lda.w cr_u
    xba
    and #$00FF                  ; u >> 8, at most $F
    ora.w cr_tmp
    pha
    lda.w cr_x1
    sec
    sbc.w cr_x0
    pha
    lda.w cr_base
    clc
    adc.w cr_x0
    pha
    jsl snesSpanCard
    tsa
    clc
    adc #10
    tas
_cs_done:
    plx
    rts

.ENDS
