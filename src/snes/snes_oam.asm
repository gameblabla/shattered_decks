;-----------------------------------------------------------------------------
;  snes_oam.asm — the sprite list's inner store.
;
;  void snesObjSpriteFlip(s16 x, s16 y, u16 tile, u8 pal, u8 big, u8 flip)
;
;  Appends one entry to snes_obj.c's OAM shadow: the four bytes of the low
;  table and the two bits of the high table (x's ninth bit and the size).
;  It is assembly because the battle's burst is forty sprites a field and
;  the HUD another sixty, and the C form -- a dozen shifted array stores
;  through 816-tcc -- cost more than a thousand cycles a sprite.  Every
;  sprite is priority 3, so OAM order decides what is in front (snes_obj.h).
;
;  The shadow, the count and the dirty flag are snes_obj.c's globals.
;-----------------------------------------------------------------------------
.include "hdr.asm"

.ACCU 16
.INDEX 16
.16BIT

.BASE $C0
.SECTION "snes_oam_text" SUPERFREE

snesObjSpriteFlip:
    php
    rep #$30
    lda.l snes_obj_n
    and #$00FF
    cmp #128
    bcc +
    plp
    rtl
+   pha                         ; the entry number, for the high table
    asl a
    asl a
    tax                         ; X = entry * 4
    sep #$20
.ACCU 8
    ; The arguments start at 4,s on entry, and 816-tcc pushes a u8 as ONE
    ; byte: x, y and tile are words, pal, big and flip single bytes.  PHP +
    ; PHA move them up by three.
    lda 7,s                     ; x, low byte
    sta.l snes_oam_shadow+0,x
    lda 9,s                     ; y
    sta.l snes_oam_shadow+1,x
    lda 11,s                    ; tile, low byte
    sta.l snes_oam_shadow+2,x
    lda 12,s                    ; tile bit 8
    and #$01
    sta.l snes_oam_tmp
    lda 13,s                    ; palette
    and #$07
    asl a
    ora.l snes_oam_tmp
    ora #$30                    ; priority 3
    sta.l snes_oam_tmp
    lda 15,s                    ; flip bits
    and #$C0
    ora.l snes_oam_tmp
    sta.l snes_oam_shadow+3,x
    ; The high table: two bits an entry, at 512 + entry / 4.
    lda 8,s                     ; x, high byte
    and #$01
    sta.l snes_oam_tmp
    lda 14,s                    ; big
    beq +
    lda #$02
    ora.l snes_oam_tmp
    sta.l snes_oam_tmp
+   rep #$20
.ACCU 16
    pla                         ; the entry number
    pha
    and #$0003
    asl a
    tay                         ; Y = shift, 0/2/4/6
    pla
    lsr a
    lsr a
    clc
    adc #512
    tax                         ; X = the high table byte's index
    sep #$20
.ACCU 8
    ; bits << shift and mask << shift, the shift in Y (0/2/4/6).  Keep both
    ; values explicit: coupling the mask to XBA's hidden B byte made one
    ; entry's update clear size bits already packed for its neighbours.
    lda #$03
    sta.l snes_oam_mask
    cpy #0
    beq _os_shifted
-   lda.l snes_oam_tmp
    asl a
    sta.l snes_oam_tmp
    lda.l snes_oam_mask
    asl a
    sta.l snes_oam_mask
    dey
    bne -
_os_shifted:
    lda.l snes_oam_mask
    eor #$FF
    and.l snes_oam_shadow,x
    ora.l snes_oam_tmp
    sta.l snes_oam_shadow,x
    lda.l snes_obj_n
    inc a
    sta.l snes_obj_n
    lda #1
    sta.l snes_oam_dirty
    rep #$20
.ACCU 16
    plp
    rtl

.ENDS

.BASE $00
.RAMSECTION "snes_oam_vars" BANK $7E SLOT 2
snes_oam_tmp  db
snes_oam_mask db
.ENDS
