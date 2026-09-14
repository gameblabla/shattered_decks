;-----------------------------------------------------------------------------
;  snes_oam.asm — the sprite list's store, and the emitters that feed it.
;
;  THE HUD IS REBUILT EVERY GAME FRAME (snes_duel.c's build_objects), and
;  measured with the cycle profiler it was the frame: a hundred sprites of
;  text, digits, plates, bars and cards through 816-tcc cost 1.3 fields at
;  rest -- a resting board ran at thirty frames a second for its HUD alone.
;  Everything per sprite now lives here:
;
;    oam_put              the store: one OAM entry from the fast direct page
;    snesObjSpriteFlip    the C entry (x, y, tile, pal, big, flip)
;    snesObjSprite        ...without a flip
;    snesObjText          a string of 8x8 glyphs
;    snesObjNum           a right-aligned decimal
;    snesObjPatchY        move one entry's y in place
;    snesObjEnd           hide last frame's tail, pick the next card upload
;
;  D IS THE FAST DIRECT PAGE (snes_fastdp.inc): the entry being built and the
;  emitters' cursors are DMA-register bytes at six cycles an access, not WRAM
;  at eight; the shadow itself is WRAM and is written once per byte.  Every
;  sprite is priority 3, so OAM order decides what is in front (snes_obj.h).
;
;  The shadow, the count, the dirty flag and the card residency arrays are
;  snes_obj.c's globals.
;-----------------------------------------------------------------------------
.include "hdr.asm"
.include "snes_fastdp.inc"
.include "snes_obj_data.inc"

.ACCU 16
.INDEX 16
.16BIT

; snes_obj.c's sheet map: the glyphs start after the twenty cards' 16 tiles
; each, then the corners, the bars, the plates, the icons.
.DEFINE OBJ_FONT_TILE   320
.DEFINE OBJ_CORNER_TILE (OBJ_FONT_TILE + SPR_GLYPH_COUNT)
.DEFINE OBJ_BAR_TILE    (OBJ_CORNER_TILE + SPR_CORNER_COUNT)
.DEFINE OBJ_PLATE_TILE  (OBJ_BAR_TILE + SPR_BAR_COUNT)
.DEFINE OBJ_ICON_TILE   (OBJ_PLATE_TILE + SPR_PLATE_COUNT)
.DEFINE OBJ_CARDS       20
.DEFINE OBJ_NO_FACE     $FF
; The life panel, in pixels from its left edge (snes_obj.c).
.DEFINE LIFE_PLATE_W    24
.DEFINE LIFE_BAR_X      28
.DEFINE LIFE_BAR_W      32
.DEFINE LIFE_NUM_X      64

; The entry being built (channel 1's window).
.DEFINE FD_OAM_X        $11     ; word
.DEFINE FD_OAM_Y        $13     ; word
.DEFINE FD_OAM_TILE     $15     ; word
.DEFINE FD_OAM_ATTR     $17     ; byte: pal << 1 | flip | $30
.DEFINE FD_OAM_BIG      $18     ; byte
.DEFINE FD_OAM_HI       $19     ; word: the high-table bits, then bits | mask << 8
; The emitters' own (channel 2's window, and 3's for the life panel).
.DEFINE FD_TXT_PTR      $21     ; three bytes
.DEFINE FD_NUM_VAL      $25
.DEFINE FD_NUM_I        $27
.DEFINE FD_NUM_POW      $29
.DEFINE FD_LP_X         $31     ; the panel's left edge
.DEFINE FD_LP_Y         $33
.DEFINE FD_LP_BASE      $35     ; the side's first bar tile
.DEFINE FD_LP_PX        $37     ; the gauge's lit pixels
.DEFINE FD_LP_I         $39
.DEFINE FD_CARD_SLOT    $25     ; the card emitter (no digits inside it)
.DEFINE FD_CARD_FACE    $27
.DEFINE FD_CARD_FLIP    $29

.BASE $C0
.SECTION "snes_oam_text" SUPERFREE

; The high table's two bits an entry: for (entry & 3) * 4 + bits, the bits in
; place in the low byte and the mask that clears the entry's pair in the high.
oam_hi_tab:
.REPT 4 INDEX sh
.REPT 4 INDEX b
    .dw (b << (sh * 2)) | (($FF - ($03 << (sh * 2))) << 8)
.ENDR
.ENDR

; The digit emitter's powers of ten.
oam_pow10:
    .dw 1000, 100, 10, 1
oam_you:
    .db "YOU", 0
oam_com:
    .db "COM", 0

;-----------------------------------------------------------------------------
; oam_put: append FD_OAM_* as the next entry.  D = FASTDP, A/X/Y 16-bit;
; clobbers A, X, Y and FD_OAM_HI.  Full lists drop the sprite.
;-----------------------------------------------------------------------------
oam_put:
    lda.l snes_obj_n
    and #$00FF
    cmp #128
    bcs _put_full
    asl a
    asl a
    tax                         ; X = entry * 4
    sep #$20
.ACCU 8
    lda.b <FD_OAM_X
    sta.l snes_oam_shadow+0,x
    lda.b <FD_OAM_Y
    sta.l snes_oam_shadow+1,x
    lda.b <FD_OAM_TILE
    sta.l snes_oam_shadow+2,x
    lda.b <FD_OAM_TILE+1
    and #$01
    ora.b <FD_OAM_ATTR
    sta.l snes_oam_shadow+3,x
    ; The high table: x's ninth bit and the size, two bits at 512 + entry/4.
    lda.b <FD_OAM_X+1
    and #$01
    sta.b <FD_OAM_HI
    stz.b <FD_OAM_HI+1
    lda.b <FD_OAM_BIG
    beq +
    lda #$02
    ora.b <FD_OAM_HI
    sta.b <FD_OAM_HI
+   rep #$20
.ACCU 16
    txy                         ; Y = entry * 4, kept
    txa
    and #$000C                  ; (entry & 3) * 4
    ora.b <FD_OAM_HI
    asl a
    tax
    lda.l oam_hi_tab,x
    sta.b <FD_OAM_HI            ; bits, mask
    tya
    lsr a
    lsr a
    lsr a
    lsr a                       ; entry / 4
    clc
    adc #512
    tax
    sep #$20
.ACCU 8
    lda.b <FD_OAM_HI+1
    and.l snes_oam_shadow,x
    ora.b <FD_OAM_HI
    sta.l snes_oam_shadow,x
    lda.l snes_obj_n
    inc a
    sta.l snes_obj_n
    lda #1
    sta.l snes_oam_dirty
    rep #$20
.ACCU 16
_put_full:
    rts

; The C entries below push php and phd: the caller's arguments start at 7,s.
.MACRO OAM_ENTER
    php
    rep #$30
    phd
    lda #FASTDP
    tcd
.ENDM
.MACRO OAM_LEAVE
    pld
    plp
    rtl
.ENDM

;-----------------------------------------------------------------------------
; void snesObjSpriteFlip(s16 x, s16 y, u16 tile, u8 pal, u8 big, u8 flip)
; void snesObjSprite(s16 x, s16 y, u16 tile, u8 pal, u8 big)
;
; 816-tcc pushes a u8 as ONE byte: x, y and tile are words, pal, big and
; flip single bytes.
;-----------------------------------------------------------------------------
snesObjSpriteFlip:
    OAM_ENTER
    lda 7,s
    sta.b <FD_OAM_X
    lda 9,s
    sta.b <FD_OAM_Y
    lda 11,s
    sta.b <FD_OAM_TILE
    sep #$20
.ACCU 8
    lda 13,s                    ; palette
    and #$07
    asl a
    ora #$30                    ; priority 3
    sta.b <FD_OAM_ATTR
    lda 15,s                    ; flip bits
    and #$C0
    ora.b <FD_OAM_ATTR
    sta.b <FD_OAM_ATTR
    lda 14,s                    ; big
    sta.b <FD_OAM_BIG
    rep #$20
.ACCU 16
    jsr oam_put
    OAM_LEAVE

snesObjSprite:
    OAM_ENTER
    lda 7,s
    sta.b <FD_OAM_X
    lda 9,s
    sta.b <FD_OAM_Y
    lda 11,s
    sta.b <FD_OAM_TILE
    sep #$20
.ACCU 8
    lda 13,s
    and #$07
    asl a
    ora #$30
    sta.b <FD_OAM_ATTR
    lda 14,s
    sta.b <FD_OAM_BIG
    rep #$20
.ACCU 16
    jsr oam_put
    OAM_LEAVE

;-----------------------------------------------------------------------------
; void snesObjText(s16 x, s16 y, const char *s)
;
; One 8x8 glyph sprite per character in the sheet's range, spaces and
; anything outside it advancing the pen only.  The pointer is 816-tcc's
; four-byte far pointer at 11,s.
;-----------------------------------------------------------------------------
snesObjText:
    OAM_ENTER
    lda 7,s
    sta.b <FD_OAM_X
    lda 9,s
    sta.b <FD_OAM_Y
    lda 11,s
    sta.b <FD_TXT_PTR
    sep #$20
.ACCU 8
    lda 13,s
    sta.b <FD_TXT_PTR+2
    rep #$20
.ACCU 16
    jsr oam_text
    OAM_LEAVE

; The glyph loop: FD_OAM_X/Y the pen, FD_TXT_PTR the string.  D = FASTDP,
; 16-bit A/X/Y; clobbers A, X, Y and the pen.
oam_text:
    sep #$20
.ACCU 8
    lda #(SPR_HUD_PAL << 1) | $30
    sta.b <FD_OAM_ATTR
    stz.b <FD_OAM_BIG
    ldy #0
_tx_loop:
    lda.b [FD_TXT_PTR],y
    beq _tx_done
    cmp #32                     ; a space advances the pen only
    beq _tx_next
    cmp #SPR_GLYPH_FIRST
    bcc _tx_next
    cmp #SPR_GLYPH_FIRST + SPR_GLYPH_COUNT
    bcs _tx_next
    rep #$20
.ACCU 16
    and #$00FF
    clc
    adc #OBJ_FONT_TILE - SPR_GLYPH_FIRST
    sta.b <FD_OAM_TILE
    phy
    jsr oam_put
    ply
    sep #$20
.ACCU 8
_tx_next:
    rep #$20
.ACCU 16
    lda.b <FD_OAM_X
    clc
    adc #8
    sta.b <FD_OAM_X
    sep #$20
.ACCU 8
    iny
    bra _tx_loop
_tx_done:
    rep #$20
.ACCU 16
    rts

;-----------------------------------------------------------------------------
; void snesObjNum(s16 x, s16 y, u16 value, u8 digits)
;
; Right to left in a fixed field of `digits` places (at most four are
; drawn, a wider field is padded on the left), the digits peeled by
; subtraction from the powers of ten.
;-----------------------------------------------------------------------------
snesObjNum:
    OAM_ENTER
    lda 7,s
    sta.b <FD_OAM_X
    lda 9,s
    sta.b <FD_OAM_Y
    lda 11,s
    sta.b <FD_NUM_VAL
    lda 13,s
    and #$00FF
    jsr oam_num
    OAM_LEAVE

; A = the field width, FD_NUM_VAL the value, FD_OAM_X/Y the pen.
oam_num:
    pha
    sep #$20
.ACCU 8
    lda #(SPR_HUD_PAL << 1) | $30
    sta.b <FD_OAM_ATTR
    stz.b <FD_OAM_BIG
    rep #$20
.ACCU 16
    pla
    cmp #5
    bcc +
    ; More than four places: the pen starts (digits - 4) cells in.
    sec
    sbc #4
    asl a
    asl a
    asl a
    clc
    adc.b <FD_OAM_X
    sta.b <FD_OAM_X
    lda #0
    bra ++
+   eor #$FFFF
    sec
    adc #4                      ; 4 - digits: the first power's index
++  asl a
    sta.b <FD_NUM_I
_nm_loop:
    ldx.b <FD_NUM_I
    cpx #8
    bcs _nm_done
    lda.l oam_pow10,x
    sta.b <FD_NUM_POW
    ldy #0
    lda.b <FD_NUM_VAL
-   cmp.b <FD_NUM_POW
    bcc +
    sbc.b <FD_NUM_POW           ; C is set by the compare
    iny
    bra -
+   sta.b <FD_NUM_VAL
    tya
    clc
    adc #OBJ_FONT_TILE + 48 - SPR_GLYPH_FIRST    ; '0'
    sta.b <FD_OAM_TILE
    jsr oam_put
    lda.b <FD_OAM_X
    clc
    adc #8
    sta.b <FD_OAM_X
    lda.b <FD_NUM_I
    inc a
    inc a
    sta.b <FD_NUM_I
    bra _nm_loop
_nm_done:
    rts

;-----------------------------------------------------------------------------
; void snesObjLifePanel(s16 x, s16 y, u8 side, u16 lp, u16 lp_max)
;
; THE LABEL IS EMITTED BEFORE THE PLATE IT SITS ON, and that ordering is
; the whole of the drawing here: every sprite is priority 3, so what
; decides which of two overlapping ones is seen is the OAM index, and a
; lower index wins.  Text first, then the plate under it, then the gauge --
; which overlaps nothing.  The gauge is lp * 32 / lp_max with both sides
; taken down by eight first (the honest product overflows sixteen bits at
; the 8000 the duel starts on); one pixel of it is 250 life points.
;-----------------------------------------------------------------------------
snesObjLifePanel:
    OAM_ENTER
    lda 7,s
    sta.b <FD_LP_X
    lda 9,s
    sta.b <FD_LP_Y
    sta.b <FD_OAM_Y
    ; The label.
    lda 7,s
    clc
    adc #2
    sta.b <FD_OAM_X
    lda 11,s
    and #$00FF
    beq +
    lda #oam_com
    bra ++
+   lda #oam_you
++  sta.b <FD_TXT_PTR
    sep #$20
.ACCU 8
    lda #:oam_you
    sta.b <FD_TXT_PTR+2
    rep #$20
.ACCU 16
    jsr oam_text
    ; The number.
    lda.b <FD_LP_X
    clc
    adc #LIFE_NUM_X
    sta.b <FD_OAM_X
    lda 12,s
    sta.b <FD_NUM_VAL
    lda #4
    jsr oam_num
    ; The plate: three cells of the side's plate tile.  (oam_num left the
    ; HUD attribute and the small size in place.)
    lda 11,s
    and #$00FF
    clc
    adc #OBJ_PLATE_TILE
    sta.b <FD_OAM_TILE
    lda.b <FD_LP_X
    sta.b <FD_OAM_X
    lda #LIFE_PLATE_W / 8
    sta.b <FD_LP_I
-   jsr oam_put
    lda.b <FD_OAM_X
    clc
    adc #8
    sta.b <FD_OAM_X
    dec.b <FD_LP_I
    bne -
    ; The gauge: px = (lp >> 3) * 32 / (lp_max >> 3), lp clamped to lp_max
    ; and lp_max to at least 8, by a sixteen-step shift-and-subtract.
    lda 14,s
    cmp #8
    bcs +
    lda #8
+   lsr a
    lsr a
    lsr a
    sta.b <FD_LP_BASE           ; borrowed: the divisor
    lda 12,s
    cmp 14,s
    bcc +
    beq +
    lda 14,s
+   cmp #8
    bcs +
    lda #8
+   lsr a
    lsr a
    lsr a
    asl a
    asl a
    asl a
    asl a
    asl a                       ; the dividend, (lp >> 3) * 32
    sta.b <FD_LP_PX
    lda #0                      ; the remainder
    ldx #16
_lp_div:
    asl.b <FD_LP_PX             ; the quotient grows in from the right
    rol a
    cmp.b <FD_LP_BASE
    bcc +
    sbc.b <FD_LP_BASE
    inc.b <FD_LP_PX
+   dex
    bne _lp_div
    ; Four cells, each lit for the pixels of the gauge it holds.
    lda 11,s
    and #$00FF
    beq +
    lda #SPR_BAR_STEPS
+   clc
    adc #OBJ_BAR_TILE
    sta.b <FD_LP_BASE
    lda.b <FD_LP_X
    clc
    adc #LIFE_BAR_X
    sta.b <FD_OAM_X
    lda #0
    sta.b <FD_LP_I              ; i * 8
_lp_cell:
    lda.b <FD_LP_PX
    sec
    sbc.b <FD_LP_I
    bcs +
    lda #0                      ; px <= i * 8: nothing lit
    bra ++
+   cmp #8
    bcc ++
    lda #8
++  clc
    adc.b <FD_LP_BASE
    sta.b <FD_OAM_TILE
    jsr oam_put
    lda.b <FD_OAM_X
    clc
    adc #8
    sta.b <FD_OAM_X
    lda.b <FD_LP_I
    clc
    adc #8
    sta.b <FD_LP_I
    cmp #LIFE_BAR_W
    bne _lp_cell
    OAM_LEAVE

;-----------------------------------------------------------------------------
; void snesObjCardFlip(s16 x, s16 y, u8 slot, u8 face, u8 flip)
; void snesObjCard(s16 x, s16 y, u8 slot, u8 face)
;
; A 32x32 card sprite from OBJ slot `slot`, asked for as `face` -- drawn only
; once the slot HOLDS that face out of the sheet the mode asks for (the
; vblank pump uploads it otherwise).  A slot outside the card palettes
; cannot own one, so it falls back to the clustered sheet however the
; caller asked.  Grey is a palette selection, part of what is wanted but not
; of residency, so the cursor moving does not hide an unchanged card.
;-----------------------------------------------------------------------------
snesObjCard:
    OAM_ENTER
    lda 7,s
    sta.b <FD_OAM_X
    lda 9,s
    sta.b <FD_OAM_Y
    lda 11,s
    and #$00FF
    sta.b <FD_CARD_SLOT
    lda 12,s
    and #$00FF
    sta.b <FD_CARD_FACE
    lda #0
    sta.b <FD_CARD_FLIP
    jsr oam_card
    OAM_LEAVE

snesObjCardFlip:
    OAM_ENTER
    lda 7,s
    sta.b <FD_OAM_X
    lda 9,s
    sta.b <FD_OAM_Y
    lda 11,s
    and #$00FF
    sta.b <FD_CARD_SLOT
    lda 12,s
    and #$00FF
    sta.b <FD_CARD_FACE
    lda 13,s
    and #$00C0
    sta.b <FD_CARD_FLIP
    jsr oam_card
    OAM_LEAVE

oam_card:
    ldx.b <FD_CARD_SLOT
    cpx #OBJ_CARDS
    bcs _oc_none
    lda.b <FD_CARD_FACE
    cmp #OBJ_NO_FACE
    bne +
_oc_none:
    rts
+
    sep #$20
.ACCU 8
    sta.l snes_card_want,x
    ; hi = the per-face sheet is on and the slot has a palette of its own;
    ; grey only with hi.
    lda.l snes_card_hi_mode
    and #$01
    beq +
    cpx #SPR_CARD_PALS
    bcc ++
+   lda #0
    sta.l snes_card_want_hi,x
    sta.l snes_card_want_grey,x
    bra +++
++  sta.l snes_card_want_hi,x
    lda.l snes_card_hi_mode
    and #$02
    beq +
    lda #1
+   sta.l snes_card_want_grey,x
+++
    ; Resident?  Face and sheet must both agree.
    lda.l snes_card_have,x
    cmp.l snes_card_want,x
    bne _oc_out8
    lda.l snes_card_have_hi,x
    cmp.l snes_card_want_hi,x
    bne _oc_out8
    ; The palette: the slot's own on the per-face sheet, the face's group
    ; on the clustered one.
    lda.l snes_card_have_hi,x
    beq +
    txa
    bra ++
+   ldx.b <FD_CARD_FACE
    lda.l snes_spr_group,x
++  and #$07
    asl a
    ora #$30
    ora.b <FD_CARD_FLIP
    sta.b <FD_OAM_ATTR
    lda #1
    sta.b <FD_OAM_BIG
    rep #$20
.ACCU 16
    ; CARD_TILE(slot) = (slot >> 2) << 6 | (slot & 3) << 2
    lda.b <FD_CARD_SLOT
    and #$000C
    asl a
    asl a
    asl a
    asl a
    sta.b <FD_OAM_TILE
    lda.b <FD_CARD_SLOT
    and #$0003
    asl a
    asl a
    ora.b <FD_OAM_TILE
    sta.b <FD_OAM_TILE
    jsr oam_put
    rts
_oc_out8:
    rep #$20
.ACCU 16
_oc_out:
    rts

;-----------------------------------------------------------------------------
; void snesObjPatchY(u8 index, s16 y, u8 big)
;
; OBJ y is eight bits over a 256-line space and a sprite wraps round it: a
; 32-pixel card at 226 shows its last row on line 1.  Anything at or past
; the bottom is parked where nothing of it is seen.
;-----------------------------------------------------------------------------
snesObjPatchY:
    php
    rep #$30
    lda 5,s                     ; index (a byte; the word's high byte is y's low)
    and #$00FF
    cmp #128
    bcs _py_out
    asl a
    asl a
    tax
    lda 6,s                     ; y
    cmp #224
    bcc _py_store               ; 0..223
    cmp #$FFE1
    bcs _py_store               ; -31..-1
    lda 8,s                     ; big (a byte; the word's high byte is junk)
    and #$00FF
    beq +
    lda #224
    bra _py_store
+   lda #240
_py_store:
    sep #$20
.ACCU 8
    sta.l snes_oam_shadow+1,x
    lda #1
    sta.l snes_oam_dirty
    rep #$20
.ACCU 16
_py_out:
    plp
    rtl

;-----------------------------------------------------------------------------
; void snesObjEnd(void)
;
; Hides the entries past this frame's count that last frame used, then
; picks what the vblank pump uploads next: the slot in flight is kept ahead
; of the others (restarted from row zero if its face changed under it),
; otherwise the first slot whose face or sheet disagrees with what it
; holds; and one hand slot whose grey palette disagrees.  All of it a byte
; loop over snes_obj.c's residency arrays.
;-----------------------------------------------------------------------------
snesObjEnd:
    php
    rep #$30
    ; for i = obj_n .. obj_prev - 1: y = 240, the high-table pair cleared.
    lda.l snes_obj_prev
    and #$00FF
    asl a
    asl a
    sta.l snes_oam_tmp          ; obj_prev * 4
    lda.l snes_obj_n
    and #$00FF
    asl a
    asl a
    tax                         ; X = i * 4
    cmp.l snes_oam_tmp
    bcs _oe_hidden
_oe_hide:
    sep #$20
.ACCU 8
    lda #240
    sta.l snes_oam_shadow+1,x
    rep #$20
.ACCU 16
    txa
    sta.l snes_oam_tmp2
    and #$000C
    asl a
    tax
    lda.l oam_hi_tab,x
    xba                         ; the pair's mask, low
    sta.l snes_oam_tmp3
    lda.l snes_oam_tmp2
    lsr a
    lsr a
    lsr a
    lsr a
    clc
    adc #512
    tax
    sep #$20
.ACCU 8
    lda.l snes_oam_tmp3
    and.l snes_oam_shadow,x
    sta.l snes_oam_shadow,x
    rep #$20
.ACCU 16
    lda.l snes_oam_tmp2
    clc
    adc #4
    tax
    cmp.l snes_oam_tmp
    bcc _oe_hide
_oe_hidden:
    lda.l snes_obj_n
    and #$00FF
    sep #$20
.ACCU 8
    sta.l snes_obj_prev
    lda #1
    sta.l snes_oam_dirty

    ; A slot in flight stays in flight.
    lda.l snes_next_card
    tax
    cpx #OBJ_CARDS
    bcs _oe_scan
    lda.l snes_card_have,x
    cmp.l snes_card_want,x
    bne +
    lda.l snes_card_have_hi,x
    cmp.l snes_card_want_hi,x
    beq _oe_scan
+   lda.l snes_next_card_face
    cmp.l snes_card_want,x
    bne +
    lda.l snes_next_card_hi
    cmp.l snes_card_want_hi,x
    beq _oe_keep
+   lda.l snes_card_want,x
    sta.l snes_next_card_face
    lda.l snes_card_want_hi,x
    sta.l snes_next_card_hi
    lda #0
    sta.l snes_next_card_row
_oe_keep:
    jmp _oe_out

_oe_scan:
    lda #OBJ_CARDS
    sta.l snes_next_card
    ldx #0
-   lda.l snes_card_have,x
    cmp.l snes_card_want,x
    bne +
    lda.l snes_card_have_hi,x
    cmp.l snes_card_want_hi,x
    bne +
    inx
    cpx #OBJ_CARDS
    bne -
    bra _oe_palette
+   txa
    sta.l snes_next_card
    lda #0
    sta.l snes_next_card_row
    lda.l snes_card_want,x
    sta.l snes_next_card_face
    lda.l snes_card_want_hi,x
    sta.l snes_next_card_hi

_oe_palette:
    ; One resident hand slot whose grey state disagrees, for the vblank
    ; pump's single CGRAM update.
    lda #OBJ_CARDS
    sta.l snes_next_palette
    ldx #0
-   lda.l snes_card_have,x
    cmp #OBJ_NO_FACE
    beq +
    lda.l snes_card_have_hi,x
    beq +
    lda.l snes_card_have,x
    cmp.l snes_card_want,x
    bne +
    lda.l snes_card_have_grey,x
    cmp.l snes_card_want_grey,x
    beq +
    txa
    sta.l snes_next_palette
    bra _oe_out
+   inx
    cpx #SPR_CARD_PALS
    bne -
_oe_out:
    rep #$20
.ACCU 16
    plp
    rtl

.ENDS

.BASE $00
.RAMSECTION "snes_oam_vars" BANK $7E SLOT 2
snes_oam_tmp  dw
snes_oam_tmp2 dw
snes_oam_tmp3 dw
.ENDS
