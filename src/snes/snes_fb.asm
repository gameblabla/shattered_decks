;-----------------------------------------------------------------------------
;  snes_fb.asm — the Mode 7 chunky framebuffer, and the routine that presents it
;
;  Mode 7's two VRAM halves are interleaved in one 16384-word window: the LOW
;  byte of word W is tilemap entry W, and the HIGH byte is character data.  If
;  tile n is made 64 bytes of the constant n -- which is exactly what
;  "high byte of word W = W >> 6" spells -- then a tilemap entry IS a pixel
;  colour, and the 128x128 tilemap is a 256-colour chunky framebuffer.  Both
;  halves live in words $0000-$3FFF, so the whole bitmap costs half of VRAM and
;  words $4000-$7FFF stay free for OBJ character data.
;
;  With VMAIN = $00 (increment by one word after a write to $2118) a DMA in
;  mode 0 to $2118 streams one framebuffer byte per VRAM word, so presenting is
;  a straight memcpy from WRAM into the tilemap's low bytes.
;
;  Rows are uploaded one at a time rather than as one block because the moving
;  board is 64 texels wide inside a 128-texel stride: only the left half of
;  each row is live, and uploading the dead half would triple the DMA for
;  nothing.  Per row this costs four register writes; the DMA itself is the
;  same hardware either way.
;-----------------------------------------------------------------------------
.include "hdr.asm"

.ACCU 16
.INDEX 16
.16BIT

;-----------------------------------------------------------------------------
; The framebuffer.  128x128 bytes in bank $7F, which SLOT 3 maps whole; the
; displayed board window is always 128x80, including full-detail camera
; motion.  Rows 80..111 hold the transparent HUD band.
;
; ALIGN 256 is not cosmetic: the rasteriser addresses a row as a 16-bit base
; plus an 8-bit X, so a row must never straddle a page boundary in a way the
; caller has to know about, and a 128-byte stride from a page-aligned base
; guarantees it.
;-----------------------------------------------------------------------------
.BASE $00
.RAMSECTION "snes_fb_ram" BANK $7F SLOT 3 ALIGN 256 KEEP
snes_fb_pad       dsb 256
snes_fb           dsb 16384
.ENDS

.RAMSECTION "snes_fb_vars" BANK $7E SLOT 2
fb_word           dw
fb_rows           dw
fb_width          dw
.ENDS

.BASE $C0
.SECTION "snes_fb_text" SUPERFREE

;-----------------------------------------------------------------------------
; void snesFbPresentRows(u16 first_row, u16 rows, u16 width)
;
; Uploads `rows` rows of `width` bytes each, starting at framebuffer row
; `first_row`, into the Mode 7 tilemap's low bytes.  MUST fit in what is left
; of vblank: a transfer that runs past the end of vblank does not merely get
; dropped, it walks the VRAM address on into the rows already on screen and
; smears one framebuffer row across the rest of the bitmap.  That is a black
; screen with a correct-looking framebuffer, and it is why the caller sizes
; each pass from a measured budget rather than from the theoretical 6.4 KB a
; 38-line vblank could carry.
;
; When `width` is the full stride the rows are contiguous in both WRAM and
; VRAM, so the whole block is one DMA and the per-row register writes vanish.
; The board always uses the full stride, so the rows are contiguous.
;
; 816-tcc pushes arguments right to left and returns with rtl, so after php the
; stack holds P (1) + the return long (3) and the first argument sits at 5,s.
; Scratch is three words of its own rather than tcc__r0..r2, which the compiler
; assumes a called routine leaves alone.
;-----------------------------------------------------------------------------
snesFbPresentRows:
    php
    rep #$30

    lda 5,s                     ; first_row
    asl a
    asl a
    asl a
    asl a
    asl a
    asl a
    asl a                       ; row * 128 -> both the VRAM word and the
    sta.l fb_word               ; framebuffer offset, which are the same number
    lda 7,s
    sta.l fb_rows               ; rows remaining
    lda 9,s
    sta.l fb_width

    sep #$20
.ACCU 8
    lda #$00                    ; VMAIN: +1 word after a write to $2118
    sta.l $2115
    lda #$00                    ; DMA mode 0: one byte, one register
    sta.l $4300
    lda #$18                    ; ...which is $2118
    sta.l $4301
    lda #:snes_fb               ; source bank
    sta.l $4304
    rep #$20
.ACCU 16

    lda.l fb_width
    cmp #128
    bne _fbrow

    ; ── Contiguous: one DMA for the whole block ─────────────────────────────
    lda.l fb_word
    sta.l $2116
    clc
    adc #snes_fb
    sta.l $4302
    lda.l fb_rows               ; rows * 128 bytes
    asl a
    asl a
    asl a
    asl a
    asl a
    asl a
    asl a
    sta.l $4305
    bra _fbgo

    ; ── Windowed: one DMA a row ─────────────────────────────────────────────
_fbrow:
    lda.l fb_rows
    beq _fbdone
    dec a
    sta.l fb_rows

    lda.l fb_word
    sta.l $2116                 ; VRAM word address = row * 128
    clc
    adc #snes_fb                ; ...and the source offset is the same value
    sta.l $4302
    lda.l fb_width
    sta.l $4305                 ; byte count

    sep #$20
.ACCU 8
    lda #$01
    sta.l $420B                 ; go
    rep #$20
.ACCU 16

    lda.l fb_word
    clc
    adc #128
    sta.l fb_word
    bra _fbrow

_fbgo:
    sep #$20
.ACCU 8
    lda #$01
    sta.l $420B
    rep #$20
.ACCU 16

_fbdone:
    plp
    rtl

;-----------------------------------------------------------------------------
; void snesFbWriteChars(void)
;
; Writes the Mode 7 character half once, under force blank: the high byte of
; word W becomes W >> 6, which makes tile n a solid block of colour n.  It
; clears the low byte at the same time, because a previous Mode 3 scene may
; have left tile data in the otherwise-unused rows below the HUD.  16384 word
; writes, so this is a scene-load cost and never a per-frame one.
;
; It cannot be a DMA from a table: the table would be 16 KB of ROM to say
; something a six-instruction loop already says.
;-----------------------------------------------------------------------------
snesFbWriteChars:
    php
    rep #$30

    sep #$20
.ACCU 8
    lda #$80                    ; VMAIN: +1 word after a write to $2119
    sta.l $2115
    rep #$20
.ACCU 16
    lda #$0000
    sta.l $2116

    ldy #0                      ; Y = the tile number, and X >> 6
_chr_outer:
    tya                         ; A's low byte is the tile number
    sep #$20
.ACCU 8
    ldx #64                     ; 64 bytes of this tile
_chr_inner:
    pha
    lda #$00
    sta.l $2118                    ; clear the Mode 7 tilemap byte
    pla
    sta.l $2119
    dex
    bne _chr_inner
    rep #$20
.ACCU 16
    iny
    cpy #256
    bne _chr_outer

    plp
    rtl

.ENDS
