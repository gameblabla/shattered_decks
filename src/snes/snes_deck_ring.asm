; ─────────────────────────────────────────────────────────────────────────────
;  snes_deck_ring.asm — the deck editor's stages and its cursor ring.
;
;  The two WRAM stages a gallery block is composed in, and the routine that
;  paints the PC-FX cursor into one: rect_outline(x-2, y-2, 30, 38) inside
;  rect_outline(x-3, y-3, 32, 40), both IDX_RED, over the 40x40 block --
;  a two-pixel ring covering x 3..34 on every row and rows 0..1 and 38..39
;  across.  The block is 25 planar 8bpp tiles, five by five; the red is
;  direct colour $07, so a ring pixel SETS its bit in planes 0..2 and CLEARS
;  it in 3..7, and one tile row's eight plane bytes sit at
;  py*2 + {0, 1, 16, 17, 32, 33, 48, 49}.
;
;  In C this was 736 far-pointer byte operations at some 230 cycles apiece --
;  731 scanlines, nearly three fields, for one cursor move (816-tcc turns
;  every b[16] |= m into a 32-bit pointer add and an indirect long access).
;  Here a row is four 16-bit read-modify-writes on adjacent plane pairs,
;  long-indexed off a fixed-bank buffer, with the masks in a ROM table.
; ─────────────────────────────────────────────────────────────────────────────
.include "hdr.asm"

.DEFINE RING_BLOCK_BYTES 1600
.DEFINE RING_TILE_ROW    320      ; five tiles of 64 bytes

; RAM labels take the current .BASE (snes_fb.asm does the same dance).
.BASE $00
.RAMSECTION "snes_deck_stage_ram" BANK $7E SLOT 2
snes_deck_stage dsb RING_BLOCK_BYTES * 2
.ENDS

.RAMSECTION "snes_deck_ring_vars" BANK $7E SLOT 2
rg_row  dw                        ; 0..39
.ENDS

.BASE $C0
.SECTION "snes_deck_ring_text" SUPERFREE

; One mask set per ring column family, as the four words ring_apply needs:
;   +0  m | m<<8            OR  into planes 0,1
;   +2  m                   OR  into plane 2 (low byte of the 16,17 pair)
;   +4  ~(m<<8) | $00FF     AND over plane 3 (high byte of that pair)
;   +6  ~(m | m<<8)         AND over planes 4..7
.MACRO RING_SET ARGS m
    .dw (m | (m << 8)), m, ((~(m << 8)) & $FF00) | $00FF, (~(m | (m << 8))) & $FFFF
.ENDM
ring_masks:
    RING_SET $18                  ;  0: column 0, a side row  (x 3..4)
    RING_SET $60                  ;  8: column 4, a side row  (x 33..34)
    RING_SET $1F                  ; 16: column 0, a full row  (x 3..7)
    RING_SET $E0                  ; 24: column 4, a full row  (x 32..34)
    RING_SET $FF                  ; 32: columns 1..3, a full row
.DEFINE SET_SIDE0 0
.DEFINE SET_SIDE4 8
.DEFINE SET_FULL0 16
.DEFINE SET_FULL4 24
.DEFINE SET_FULL  32

; void snesDeckPaintRing(u16 stage): the ring over stage block 0 or 1.
; 16-bit throughout: X is the byte index of a tile row inside the stages,
; Y the mask set.  About a hundred ring_apply calls; the byte-at-a-time
; version cost 120 scanlines, this one a quarter of that.
snesDeckPaintRing:
    php
    phb
    rep #$30
    ; The masks are read absolute-indexed by Y (there is no long,Y mode),
    ; so the data bank is this section's for the duration.
    sep #$20
    lda #:ring_masks
    pha
    plb
    rep #$20
    lda 6,s
    beq +
    lda #RING_BLOCK_BYTES
+   tax
    ; The four horizontal rows: 0 and 1 in tile row 0, 38 and 39 in tile
    ; row 4 (byte 1280 + 6*2).
    jsr ring_full_row
    inx
    inx
    jsr ring_full_row
    txa
    clc
    adc #RING_TILE_ROW * 4 + 12 - 2
    tax
    jsr ring_full_row
    inx
    inx
    jsr ring_full_row
    ; The sides, rows 2..37: back to the block's start, then row by row.
    txa
    sec
    sbc #RING_TILE_ROW * 4 + 14
    tax
    lda #0
    sta.l rg_row
_rg_side:
    lda.l rg_row
    cmp #2
    bcc _rg_skip
    cmp #38
    bcs _rg_skip
    ldy #SET_SIDE0
    jsr ring_apply
    txa
    clc
    adc #4 * 64
    tax
    ldy #SET_SIDE4
    jsr ring_apply
    txa
    sec
    sbc #4 * 64
    tax
_rg_skip:
    lda.l rg_row
    inc a
    sta.l rg_row
    cmp #40
    bcs _rg_done
    and #7
    bne +
    txa
    clc
    adc #RING_TILE_ROW - 14
    tax
    bra _rg_side
+   inx
    inx
    bra _rg_side
_rg_done:
    plb
    plp
    rtl

; X = a tile row's byte index in tile column 0: the ring across all five
; tiles of that row.  Preserves X.
ring_full_row:
    ldy #SET_FULL0
    jsr ring_apply
    ldy #SET_FULL
    txa
    clc
    adc #64
    tax
    jsr ring_apply
    txa
    clc
    adc #64
    tax
    jsr ring_apply
    txa
    clc
    adc #64
    tax
    jsr ring_apply
    txa
    clc
    adc #64
    tax
    ldy #SET_FULL4
    jsr ring_apply
    txa
    sec
    sbc #4 * 64
    tax
    rts

; X = the tile row's byte index, Y = the mask set: set the ring bits in
; planes 0..2, clear them in 3..7, a plane pair at a time.  Preserves X, Y.
ring_apply:
    lda.l snes_deck_stage + 0,x
    ora.w ring_masks + 0,y
    sta.l snes_deck_stage + 0,x
    lda.l snes_deck_stage + 16,x
    ora.w ring_masks + 2,y
    and.w ring_masks + 4,y
    sta.l snes_deck_stage + 16,x
    lda.l snes_deck_stage + 32,x
    and.w ring_masks + 6,y
    sta.l snes_deck_stage + 32,x
    lda.l snes_deck_stage + 48,x
    and.w ring_masks + 6,y
    sta.l snes_deck_stage + 48,x
    rts

.ENDS
