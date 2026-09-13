; Sparse, transaction-based Mode 3 framebuffer uploader.
.include "hdr.asm"
.include "snes_fb.inc"

.ACCU 16
.INDEX 16
.16BIT
.BASE $00

.RAMSECTION "snes_fb_ring" BANK 0 SLOT 1 ORGA $0400 FORCE
snes_ring dsb FB_RING_SIZE
.ENDS

.RAMSECTION "snes_fb_dp" BANK 0 SLOT 1 ORGA $0310 FORCE
fb_sptr dsb 3
fb_pad  db
fb_dptr dw
.ENDS
.DEFINE FB_DP_BASE $0300
.DEFINE FB_SPTR $10
.DEFINE FB_DPTR $14

.RAMSECTION "snes_frame_fb_ram" BANK $7E SLOT 2 ORGA $7000 FORCE
snes_frame_fb dsb 36864
.ENDS

; The two logical maps, the renderer's per-row spans and dirty masks, and
; the tile allocator (snes_conv_drivers.inc): 6,268 bytes of the 9,216 the
; deleted 128x72 motion frame used to take.
.RAMSECTION "snes_sparse_ram" BANK $7F SLOT 3 ORGA $8000 FORCE KEEP
snes_map_shadow_a dsb 2048
snes_map_shadow_b dsb 2048
snes_conv_rowspan dsb 36          ; 18 x {first, last} occupied pixel column
snes_conv_dirty   dsb 72          ; 18 x 32-bit cell mask
snes_conv_rom     dsb 72          ; 18 x 32-bit mask: cells the ROM floor supplies
snes_tile_ref     dsb 704         ; references per physical tile (both maps)
snes_tile_free    dsb FB_FREE_BYTES   ; the free-tile queue
.ENDS

.RAMSECTION "snes_fb_vars" BANK $7E SLOT 2
ring_wrote dw
job_w dw
frames_queued dw
jp_bytes dw
ring_freed dw
job_r dw
frames_shown dw
fb_presented_generation dw
fb_occupied dw
fb_peak_occupied dw
fb_nmi_skips dw
nm_budget dw
nm_n dw
nm_tmp dw
fb_drain_enable dw
fb_epoch dw
fb_reserve dw
fb_free_head dw
fb_free_tail dw
fb_free_count dw
fb_jobs dsb FB_JOBS * 8
snes_fb_tm dsb 7
wd_piece_bytes dw
wf_value dw

; Converter shared state.
cv_run_src dw
cv_run_bank dw
cv_run_dst dw
cv_run_bytes dw
cv_run_kind dw
cv_tile dw
cv_col dw
cv_row dw
cv_shadow dw
cv_y dw
cv_mask_lo dw
cv_mask_hi dw
cv_pool dw
cv_generation dw
cv_occupied dw
cv_tmp dw
cv_other dw
cv_bits dw
cv_bits_hi dw
cv_entry dw
cv_rom dw
cv_rom_hi dw
cv_rom_src dw
cv_rom_bank dw
.ENDS

.BASE $C0
.SECTION "snes_fb_text" SUPERFREE

snesFbInit:
    php
    rep #$30
    lda #0
    sta.l ring_wrote
    sta.l ring_freed
    sta.l job_w
    sta.l job_r
    sta.l frames_queued
    sta.l frames_shown
    sta.l fb_presented_generation
    sta.l fb_occupied
    sta.l fb_peak_occupied
    sta.l fb_nmi_skips
    sta.l fb_drain_enable
    sta.l cv_run_bytes
    sta.l fb_reserve
    inc a
    sta.l fb_epoch
    ; Both logical maps blank, no dirty cells, no tile referenced.
    ldx #0
    lda #0
-   sta.l snes_map_shadow_a,x
    inx
    inx
    cpx #2048*2+36+72+72+704
    bne -
    ; Every physical tile in the free queue, in order.
    ldx #0
    lda #1
-   sta.l snes_tile_free,x
    inc a
    inx
    inx
    cpx #FB_TILES*2
    bne -
    lda #0
    sta.l fb_free_head
    lda #FB_TILES*2
    sta.l fb_free_tail
    lda #FB_TILES
    sta.l fb_free_count
    ; BG1 above the HUD, OBJ everywhere.
    sep #$20
.ACCU 8
    lda #127
    sta.l snes_fb_tm+0
    lda #$11
    sta.l snes_fb_tm+1
    lda #144-127
    sta.l snes_fb_tm+2
    lda #$11
    sta.l snes_fb_tm+3
    lda #224-144
    sta.l snes_fb_tm+4
    lda #$10
    sta.l snes_fb_tm+5
    lda #0
    sta.l snes_fb_tm+6
    rep #$20
.ACCU 16
    plp
    rtl

snesFbDrain:
    php
    rep #$30
    lda 5,s
    sta.l fb_drain_enable
    plp
    rtl

; Stop the ISR and atomically discard all work owned by the old scene.
snesFbCancel:
    php
    sei
    rep #$30
    lda #0
    sta.l fb_drain_enable
    lda.l job_w
    sta.l job_r
    lda.l ring_wrote
    sta.l ring_freed
    lda.l frames_queued
    sta.l frames_shown
    lda.l fb_epoch
    inc a
    sta.l fb_epoch
    plp
    rtl

snesFbJobsPending:
    rep #$30
    lda.l job_w
    sec
    sbc.l job_r
    and #$00FF
    sta.b tcc__r0
    rtl

snesFbFramesPending:
    rep #$30
    lda.l frames_queued
    sec
    sbc.l frames_shown
    sta.b tcc__r0
    rtl

snesFbPresentedGeneration:
    rep #$30
    lda.l fb_presented_generation
    sta.b tcc__r0
    rtl

snesFbOccupied:
    rep #$30
    lda.l fb_occupied
    sta.b tcc__r0
    rtl

snesFbPeakOccupied:
    rep #$30
    lda.l fb_peak_occupied
    sta.b tcc__r0
    rtl

snesFbNmiSkips:
    rep #$30
    lda.l fb_nmi_skips
    sta.b tcc__r0
    rtl

snesFbJobPush:
    php
    rep #$30
    lda 7,s
    xba
    and #$FF00
    sta.l cv_tmp
    lda 13,s
    and #$00FF
    ora.l cv_tmp
    tax
    lda 11,s
    sta.l jp_bytes
    lda 9,s
    tay
    lda 5,s
    jsl snesFbJobPushRaw
    plp
    rtl

; A=source, X=kind|bank<<8, Y=VRAM destination, jp_bytes=count.
snesFbJobPushRaw:
    sta.l cv_tmp
_jp_wait:
    lda.l job_w
    sec
    sbc.l job_r
    and #$00FF
    cmp #(FB_JOBS - 1) * 8
    bcs _jp_wait
    lda.l job_w
    phx
    tax
    lda.l cv_tmp
    sta.l fb_jobs + JOB_SRC,x
    pla
    sep #$20
.ACCU 8
    sta.l fb_jobs + JOB_KIND,x
    xba
    sta.l fb_jobs + JOB_BANK,x
    rep #$20
.ACCU 16
    tya
    sta.l fb_jobs + JOB_DST,x
    lda.l jp_bytes
    sta.l fb_jobs + JOB_BYTES,x
    txa
    clc
    adc #8
    and #$00FF
    sta.l job_w
    rtl

; Exact byte rectangle copy. Zero width/height are no-ops and odd widths are
; valid, which keeps cancellation/restoration from turning 0 into 65536.
snesFbRectCopy:
    php
    rep #$30
    lda 13,s
    beq _rc_done_early
    lda 15,s
    beq _rc_done_early
    phb
    phd
    lda #FB_DP_BASE
    tcd
    lda 8,s
    sta.b FB_SPTR
    lda 10,s
    sep #$20
.ACCU 8
    sta.b FB_SPTR+2
    lda 14,s
    pha
    plb
    rep #$20
.ACCU 16
    lda 12,s
    sta.b FB_DPTR
    lda 18,s
    sta.l cv_row
_rc_row:
    lda 16,s
    tax
    ldy #0
    sep #$20
.ACCU 8
_rc_byte:
    lda [FB_SPTR],y
    sta (FB_DPTR),y
    iny
    dex
    bne _rc_byte
    rep #$20
.ACCU 16
    lda.b FB_SPTR
    clc
    adc 20,s
    sta.b FB_SPTR
    lda.b FB_DPTR
    clc
    adc 22,s
    sta.b FB_DPTR
    lda.l cv_row
    dec a
    sta.l cv_row
    bne _rc_row
    pld
    plb
_rc_done_early:
    plp
    rtl

snesFbWramDma:
    php
    rep #$30
    lda 13,s
    beq _wd_done
    lda 9,s
    sta.l $2181
    lda 11,s
    and #$0001
    sep #$20
.ACCU 8
    sta.l $2183
    lda #0
    sta.l $4300
    lda #$80
    sta.l $4301
    lda 7,s
    sta.l $4304
    rep #$20
.ACCU 16
    lda 5,s
    sta.l $4302
    lda 13,s
    sta.l cv_tmp
_wd_piece:
    lda.l cv_tmp
    beq _wd_done
    cmp #8192
    bcc +
    lda #8192
+   sta.l $4305
    sta.l wd_piece_bytes
    sep #$20
.ACCU 8
    lda #1
    sta.l $420B
    rep #$20
.ACCU 16
    lda.l cv_tmp
    sec
    sbc.l wd_piece_bytes
    sta.l cv_tmp
    bra _wd_piece
_wd_done:
    plp
    rtl

; void snesFbWramFill(u16 dst, u16 dst_bank, u16 bytes, u16 value)
;
; A DMA memset through WMDATA: one fixed source byte, `bytes` times.  Eight
; master cycles a byte, which is what makes clearing the 36 KB frame cost a
; fifth of a field rather than the four and a half the 16-bit store loop did.
snesFbWramFill:
    php
    rep #$30
    lda 9,s
    beq _wf_done
    lda 5,s
    sta.l $2181
    lda 7,s
    and #$0001
    sep #$20
.ACCU 8
    sta.l $2183
    lda 11,s
    sta.l wf_value
    lda #$08                    ; fixed A-bus address, CPU -> PPU, 1 byte
    sta.l $4300
    lda #$80                    ; $2180
    sta.l $4301
    lda #$7E
    sta.l $4304
    rep #$20
.ACCU 16
    lda #wf_value
    sta.l $4302
    lda 9,s
    sta.l cv_tmp
_wf_piece:
    lda.l cv_tmp
    beq _wf_done
    cmp #8192
    bcc +
    lda #8192
+   sta.l $4305
    sta.l wd_piece_bytes
    sep #$20
.ACCU 8
    lda #1
    sta.l $420B
    rep #$20
.ACCU 16
    lda.l cv_tmp
    sec
    sbc.l wd_piece_bytes
    sta.l cv_tmp
    bra _wf_piece
_wf_done:
    plp
    rtl

; NMI transfer drain.  Channel 7 is private to this routine.
;
; Called from pvsneslib's NMI with A/X/Y 16-bit and DB = $7E.  It moves at
; most nm_budget bytes of queued jobs into VRAM, splitting a job across
; vblanks on whole 64-byte tiles, and runs a completion job (kind 2/3) only
; once every job queued before it has gone up: that is what makes the map
; switch atomic -- no tile of a generation is ever visible before all of it.
;
; The main thread names, in fb_reserve, the bytes IT will DMA after
; WaitForVBlank returns (OAM, card rows, palettes); the drain leaves that much
; of the window alone so the two never add up to more than one vblank.
snesFbNmi:
    lda.l fb_drain_enable
    beq _nm_idle
    lda.l job_w
    cmp.l job_r
    bne _nm_go
_nm_idle:
    rtl
_nm_go:
    sep #$20
.ACCU 8
    lda.l $4212
    bmi +
    jmp _nm_late8
+   lda.l $213F                 ; clear the V counter's byte toggle
    lda.l $2137
    lda.l $213D
    xba
    lda.l $213D
    xba
    rep #$20
.ACCU 16
    and #$01FF
    cmp #231
    bcc +
    jmp _nm_late
+   lda #FB_BUDGET
    sec
    sbc.l fb_reserve
    bcc +
    bne ++
+   jmp _nm_out
++  sta.l nm_budget
    sep #$20
.ACCU 8
    lda #$80
    sta.l $2115
    lda #$01
    sta.l $4370
    lda #$18
    sta.l $4371
    rep #$20
.ACCU 16
_nm_job:
    lda.l job_w
    cmp.l job_r
    bne +
    jmp _nm_out
+   lda.l job_r
    tax
    lda.l fb_jobs + JOB_KIND,x
    and #$00FF
    cmp #2
    bcc +
    jmp _nm_commit
+   lda.l fb_jobs + JOB_BYTES,x
    bne +
    jsr nm_advance              ; an empty transfer: nothing to spend
    bra _nm_job
+   cmp.l nm_budget
    bcc +
    lda.l nm_budget
+   and #$FFC0                  ; split only on complete 64-byte tiles
    bne +
    jmp _nm_out
+   sta.l nm_n
    lda.l fb_jobs + JOB_DST,x
    sta.l $2116
    lda.l fb_jobs + JOB_SRC,x
    sta.l $4372
    sep #$20
.ACCU 8
    lda.l fb_jobs + JOB_BANK,x
    sta.l $4374
    rep #$20
.ACCU 16
    lda.l nm_n
    sta.l $4375
    sep #$20
.ACCU 8
    lda #$80
    sta.l $420B
    rep #$20
.ACCU 16
    lda.l fb_jobs + JOB_SRC,x
    clc
    adc.l nm_n
    sta.l fb_jobs + JOB_SRC,x
    lda.l nm_n
    lsr a
    clc
    adc.l fb_jobs + JOB_DST,x
    sta.l fb_jobs + JOB_DST,x
    lda.l fb_jobs + JOB_BYTES,x
    sec
    sbc.l nm_n
    sta.l fb_jobs + JOB_BYTES,x
    pha
    lda.l fb_jobs + JOB_KIND,x
    and #$00FF
    cmp #1
    bne +
    lda.l ring_freed            ; a ring job frees its slots as it goes
    clc
    adc.l nm_n
    sta.l ring_freed
+   pla
    bne +
    jsr nm_advance
+   lda.l nm_budget
    sec
    sbc.l nm_n
    sta.l nm_budget
    beq _nm_out
    jmp _nm_job
_nm_commit:
    ; Completion jobs are queued after the complete map.  Switch the map and
    ; publish the generation in this same vblank; nothing is left to wait for.
    cmp #2
    bne _nm_commit_b
    sep #$20
.ACCU 8
    lda #$58
    sta.l $2107
    rep #$20
.ACCU 16
    bra _nm_publish
_nm_commit_b:
    sep #$20
.ACCU 8
    lda #$5C
    sta.l $2107
    rep #$20
.ACCU 16
_nm_publish:
    lda.l fb_jobs + JOB_SRC,x
    sta.l fb_presented_generation
    lda.l frames_shown
    inc a
    sta.l frames_shown
    jsr nm_advance
    jmp _nm_job
_nm_late8:
    rep #$20
.ACCU 16
_nm_late:
    lda.l fb_nmi_skips
    inc a
    sta.l fb_nmi_skips
_nm_out:
    rep #$30
    rtl

nm_advance:
    lda.l job_r
    clc
    adc #8
    and #$00FF
    sta.l job_r
    rts

; void snesConvSetFloor(u16 src, u16 bank): the planar ROM floor the
; converter takes ROM-masked cells from, tile `cell` at src + cell * 64.
snesConvSetFloor:
    php
    rep #$30
    lda 5,s
    sta.l cv_rom_src
    lda 7,s
    and #$00FF
    sta.l cv_rom_bank
    plp
    rtl

; void snesFbReserve(u16 bytes): what the main thread will transfer itself
; in the coming vblank, so the drain leaves room for it.
snesFbReserve:
    php
    rep #$30
    lda 5,s
    sta.l fb_reserve
    plp
    rtl

.ENDS
