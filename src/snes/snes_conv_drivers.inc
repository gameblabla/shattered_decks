;-----------------------------------------------------------------------------
;  snes_conv_drivers.inc — the sparse converter's driver.
;
;  Included at the end of the generated snes_conv_gen.asm so the jsr (table,x)
;  dispatch into the four phase copies of the tile converter stays inside one
;  bank.  The ring, the jobs, the two map shadows, the tile allocator and
;  every cv_* word are declared in snes_fb.asm.
;
;  ONE TRANSACTION = ONE NEW LOGICAL MAP.  The board is 18 rows of 32 cells;
;  a cell is either the permanent blank tile 0 or one of 702 physical 8bpp
;  tiles.  Two logical maps exist (A at VRAM $5800, B at $5C00): one is on
;  screen, the other is rebuilt here and switched to in a single vblank once
;  every tile it names has reached VRAM.  Cells the caller did not mark dirty
;  KEEP THE TILE THE SHOWN MAP USES -- copy-on-write, with a reference count
;  per physical tile -- so a cursor move converts a handful of cells, not the
;  board.  A camera move marks every cell dirty and converts every occupied
;  one; the worst case (two disjoint full frames) is 2 x 351 = 702 tiles,
;  which is the whole tile store.
;
;  Inputs, set up by the renderer before the call:
;     snes_conv_rowspan[row*2]   first occupied pixel column of cell row
;                                (255 = the row is empty)
;     snes_conv_rowspan[row*2+1] last occupied pixel column
;     snes_conv_dirty[row*4]     32-bit mask of the cells to reconvert
;     snes_conv_rom[row*4]       32-bit mask of the cells whose tile is the
;                                planar ROM floor's (snesConvSetFloor): those
;                                are DMA'd from ROM and never converted
;
;  THE RUN ACCUMULATOR.  Converted tiles are booked into a run of contiguous
;  ring bytes bound for contiguous VRAM and a run becomes one job when it
;  breaks (a non-consecutive tile id, the ring wrapping) or ends.  The free
;  tile QUEUE hands out ids in the order they were released, which for a
;  full frame is cell order, so runs stay long.
;-----------------------------------------------------------------------------

;-----------------------------------------------------------------------------
; Push the pending run as a job.  Clobbers A, X, Y.
;-----------------------------------------------------------------------------
cv_flush_run:
    lda.l cv_run_bytes
    beq _cfr_none
    sta.l jp_bytes
    lda.l cv_run_bank
    xba
    and #$FF00
    ora.l cv_run_kind
    tax
    lda.l cv_run_dst
    tay
    lda.l cv_run_src
    jsl snesFbJobPushRaw
    lda #0
    sta.l cv_run_bytes
_cfr_none:
    rts

;-----------------------------------------------------------------------------
; Reserve one ring slot: waits until 64 bytes are free (flushing the pending
; run first, or the ISR could never free them), then returns
;   Y = the slot's absolute low-RAM address (snes_ring + offset)
; and leaves the slot's phase*2 in cv_tmp for the jsr dispatch.
;-----------------------------------------------------------------------------
cv_ring_reserve:
    lda.l ring_wrote
    sec
    sbc.l ring_freed
    cmp #FB_RING_SIZE - 64 + 1
    bcc _rr_room
    jsr cv_flush_run
_rr_wait:
    lda.l ring_wrote
    sec
    sbc.l ring_freed
    cmp #FB_RING_SIZE - 64 + 1
    bcs _rr_wait
_rr_room:
    lda.l ring_wrote
    and #FB_RING_MASK
    clc
    adc #snes_ring
    tay
    and #$00C0
    lsr a
    lsr a
    lsr a
    lsr a
    lsr a
    sta.l cv_tmp
    rts

;-----------------------------------------------------------------------------
; Book the converted tile at ring slot Y into the run bound for VRAM tile
; cv_tile.  Breaks the run first if the tile would not continue it, and after
; it if the slot was the ring's last.
;-----------------------------------------------------------------------------
cv_ring_commit:
    lda.l cv_run_bytes
    beq _crc_start
    lda.l cv_run_kind
    cmp #1
    bne _crc_break              ; a ROM run is pending: it cannot continue
    lda.l cv_run_src
    clc
    adc.l cv_run_bytes
    sta.l cv_tmp
    tya
    cmp.l cv_tmp
    bne _crc_break
    lda.l cv_run_bytes
    lsr a
    clc
    adc.l cv_run_dst
    sta.l cv_tmp
    lda.l cv_tile
    asl a
    asl a
    asl a
    asl a
    asl a
    cmp.l cv_tmp
    beq _crc_extend
_crc_break:
    phy
    jsr cv_flush_run
    ply
_crc_start:
    tya
    sta.l cv_run_src
    lda #$007E
    sta.l cv_run_bank
    lda #1
    sta.l cv_run_kind
    lda.l cv_tile
    asl a
    asl a
    asl a
    asl a
    asl a
    sta.l cv_run_dst
    lda #0
    sta.l cv_run_bytes
_crc_extend:
    lda.l cv_run_bytes
    clc
    adc #64
    sta.l cv_run_bytes
    lda.l ring_wrote
    clc
    adc #64
    sta.l ring_wrote
    and #FB_RING_MASK
    bne _crc_done
    jsr cv_flush_run
_crc_done:
    rts

;-----------------------------------------------------------------------------
; Book ROM floor cell A (row*32 + col) for VRAM tile cv_tile: a kind-0 job
; from cv_rom_src + cell*64 in bank cv_rom_bank, extending the pending run
; when both source and destination continue it.
;-----------------------------------------------------------------------------
cv_rom_commit:
    asl a
    asl a
    asl a
    asl a
    asl a
    asl a
    clc
    adc.l cv_rom_src
    sta.l cv_entry              ; the tile's ROM address (cv_entry is free here)
    lda.l cv_run_bytes
    beq _rom_start
    lda.l cv_run_kind
    bne _rom_break
    lda.l cv_run_bank
    cmp.l cv_rom_bank
    bne _rom_break
    lda.l cv_run_src
    clc
    adc.l cv_run_bytes
    cmp.l cv_entry
    bne _rom_break
    lda.l cv_run_bytes
    lsr a
    clc
    adc.l cv_run_dst
    sta.l cv_tmp
    lda.l cv_tile
    asl a
    asl a
    asl a
    asl a
    asl a
    cmp.l cv_tmp
    beq _rom_extend
_rom_break:
    jsr cv_flush_run
_rom_start:
    lda.l cv_entry
    sta.l cv_run_src
    lda.l cv_rom_bank
    sta.l cv_run_bank
    lda #0
    sta.l cv_run_kind
    sta.l cv_run_bytes
    lda.l cv_tile
    asl a
    asl a
    asl a
    asl a
    asl a
    sta.l cv_run_dst
_rom_extend:
    lda.l cv_run_bytes
    clc
    adc #64
    sta.l cv_run_bytes
    rts

;-----------------------------------------------------------------------------
; The tile allocator.  Reference counts are bytes in snes_tile_ref; the free
; queue is 704 words in snes_tile_free, head/tail/count in bank $7E.
;-----------------------------------------------------------------------------

; Release the tile named by map entry A (0 is never counted).  Clobbers A, X.
cv_tile_release:
    and #$03FF
    beq _rel_none
    tax
    sep #$20
.ACCU 8
    lda.l snes_tile_ref,x
    beq _rel_zero               ; already free: a bug elsewhere, but harmless
    dec a
    sta.l snes_tile_ref,x
    bne _rel_zero
    rep #$20
.ACCU 16
    txa
    pha
    lda.l fb_free_tail
    tax
    pla
    sta.l snes_tile_free,x
    inx
    inx
    cpx #FB_FREE_BYTES
    bne +
    ldx #0
+   txa
    sta.l fb_free_tail
    lda.l fb_free_count
    inc a
    sta.l fb_free_count
    rts
_rel_zero:
    rep #$20
.ACCU 16
_rel_none:
    rts

; Take a tile: A = its id with a reference of one, or 0 if the store is
; empty (which the occupancy bound rules out).  Clobbers X.
cv_tile_alloc:
    lda.l fb_free_count
    beq _alloc_none
    dec a
    sta.l fb_free_count
    lda.l fb_free_head
    tax
    lda.l snes_tile_free,x
    pha
    inx
    inx
    cpx #FB_FREE_BYTES
    bne +
    ldx #0
+   txa
    sta.l fb_free_head
    pla
    tax
    sep #$20
.ACCU 8
    lda #1
    sta.l snes_tile_ref,x
    rep #$20
.ACCU 16
    txa
    rts
_alloc_none:
    lda #0
    rts

;-----------------------------------------------------------------------------
; u16 snesConvFrame(u16 pool, u16 generation)
;
; Builds logical map `pool` (0 = A, 1 = B) from the other map plus the dirty
; cells, converts those into freshly allocated tiles, and queues the map
; and its completion job.  Returns the occupied cell count, or zero when the
; frame's span exceeds the 351-cell budget -- in which case nothing is
; queued and no map or reference count has changed.
;
; IT IS THREE CALLS, so a camera frame can be converted a few cell rows a
; display field with the sprite layer serviced in between (snes_duel.c's
; resumable camera job): snesConvBegin counts the span and copies the map,
; snesConvRows(r0, r1) converts the dirty cells of rows [r0, r1) and flushes
; their run, snesConvEnd queues the map and the completion marker.
; snesConvFrame itself is the three in a row.
;-----------------------------------------------------------------------------
snesConvFrame:
    php
    rep #$30
    lda 7,s
    pha
    lda 7,s
    pha
    jsl snesConvBegin
    pla
    pla
    lda.b tcc__r0
    bne +
    plp
    rtl
+   pea 18
    pea 0
    jsl snesConvRows
    pla
    pla
    jsl snesConvEnd
    lda.l cv_occupied
    sta.b tcc__r0
    plp
    rtl

; u16 snesConvBegin(u16 pool, u16 generation)
snesConvBegin:
    php
    rep #$30
    lda 5,s
    and #1
    sta.l cv_pool
    lda 7,s
    sta.l cv_generation
    lda #0
    sta.l cv_occupied
    sta.l cv_row
_cf_count_row:
    lda.l cv_row
    asl a
    tax
    lda.l snes_conv_rowspan,x
    sta.l cv_tmp
    and #$00FF
    cmp #$00FF
    beq _cf_count_next
    lsr a
    lsr a
    lsr a
    sta.l cv_mask_lo
    lda.l cv_tmp
    xba
    and #$00FF
    lsr a
    lsr a
    lsr a
    sec
    sbc.l cv_mask_lo
    inc a
    clc
    adc.l cv_occupied
    sta.l cv_occupied
_cf_count_next:
    lda.l cv_row
    inc a
    sta.l cv_row
    cmp #18
    bne _cf_count_row
    lda.l cv_occupied
    cmp #FB_POOL_TILES+1
    bcc +
    jmp _cf_overflow
+   sta.l fb_occupied
    cmp.l fb_peak_occupied
    bcc +
    sta.l fb_peak_occupied
+
    ; cv_shadow = the map being built, cv_other = the map on screen.
    lda.l cv_pool
    beq _cf_map_a
    lda #snes_map_shadow_b
    sta.l cv_shadow
    lda #snes_map_shadow_a
    sta.l cv_other
    bra _cf_map_ready
_cf_map_a:
    lda #snes_map_shadow_a
    sta.l cv_shadow
    lda #snes_map_shadow_b
    sta.l cv_other
_cf_map_ready:

    ; ── Step 1: the new map starts as a copy of the shown one ──────────────
    ; Only the 576 board cells; rows 18..31 of both maps are blank for ever.
    ; A CAMERA FRAME SKIPS IT: with every cell dirty, the column loop
    ; releases each cell's old tile and names a new one (or the blank) for
    ; all 576, which is everything the copy would have done and undone.
    ldx #0
_cf_all_dirty:
    lda.l snes_conv_dirty,x
    cmp #$FFFF
    bne _cf_copy_needed
    inx
    inx
    cpx #18*4
    bne _cf_all_dirty
    bra _cf_copy_done
_cf_copy_needed:
    lda #0
    sta.l cv_y                  ; byte index of the cell in a map
_cf_copy:
    lda.l cv_y
    clc
    adc.l cv_shadow
    tax
    lda.l $7F0000,x
    sta.l cv_tmp                ; what the new map held
    lda.l cv_y
    clc
    adc.l cv_other
    tax
    lda.l $7F0000,x
    sta.l cv_col                ; what the shown map holds
    cmp.l cv_tmp
    beq _cf_copy_next
    lda.l cv_tmp
    jsr cv_tile_release
    lda.l cv_col
    and #$03FF
    beq +
    tax
    sep #$20
.ACCU 8
    lda.l snes_tile_ref,x
    inc a
    sta.l snes_tile_ref,x
    rep #$20
.ACCU 16
+   lda.l cv_y
    clc
    adc.l cv_shadow
    tax
    lda.l cv_col
    sta.l $7F0000,x
_cf_copy_next:
    lda.l cv_y
    inc a
    inc a
    sta.l cv_y
    cmp #576*2
    bne _cf_copy
_cf_copy_done:
    lda #0
    sta.l cv_run_bytes
    lda.l cv_occupied
    sta.b tcc__r0
    plp
    rtl

; void snesConvRows(u16 row0, u16 row1)
;
; ── Step 2: the dirty cells of rows [row0, row1) ─────────────────────────
snesConvRows:
    php
    rep #$30
    lda #32
    sta.l cv_col_end            ; whole rows (see _cf_row_next)
    lda 7,s
    sta.l cv_row_end
    lda 5,s
    sta.l cv_row
    ; The cell row's first chunky byte: row * 2048.
    xba
    asl a
    asl a
    asl a
    and #$F800
    sta.l cv_y
    lda.l cv_row
    cmp.l cv_row_end
    bcc +
    plp
    rtl
+   phb
    phd
    pea $7E7E
    plb
    plb
_cf_row:
    lda.l cv_row
    asl a
    asl a
    tax
    lda.l snes_conv_dirty,x
    sta.l cv_bits
    lda.l snes_conv_dirty+2,x
    sta.l cv_bits_hi
    ora.l cv_bits
    bne +
    jmp _cf_row_next            ; nothing dirty on this row
+   lda.l snes_conv_rom,x
    sta.l cv_rom
    lda.l snes_conv_rom+2,x
    sta.l cv_rom_hi
    lda.l cv_row
    asl a
    tax
    lda.l snes_conv_rowspan,x
    sta.l cv_tmp
    and #$00FF
    cmp #$00FF
    bne +
    lda #$FFFF                  ; an empty row: no column is in the span
    sta.l cv_mask_lo
    sta.l cv_mask_hi
    bra _cf_col_start
+   lsr a
    lsr a
    lsr a
    sta.l cv_mask_lo            ; first occupied cell column
    lda.l cv_tmp
    xba
    and #$00FF
    lsr a
    lsr a
    lsr a
    sta.l cv_mask_hi            ; last occupied cell column
_cf_col_start:
    lda #0
    sta.l cv_col
    lda #32
    sta.l cv_col_end
_cf_col:
    lda.l cv_col
    cmp #16
    bne +
    lda.l cv_bits_hi            ; the second words of the masks
    sta.l cv_bits
    lda.l cv_rom_hi
    sta.l cv_rom
+   lda.l cv_rom
    lsr a
    sta.l cv_rom                ; C = this cell may come from the ROM floor
    php
    lda.l cv_bits
    lsr a
    sta.l cv_bits
    bcs +
    plp
    jmp _cf_col_next            ; clean cell: keeps the shown map's tile
+   plp
    rol a                       ; A bit 0 = ROM
    and #$0001
    sta.l cv_tmp
    ; The cell's map entry: release what the new map named there.
    lda.l cv_row
    asl a
    asl a
    asl a
    asl a
    asl a
    clc
    adc.l cv_col
    asl a
    clc
    adc.l cv_shadow
    sta.l cv_entry
    tax
    lda.l $7F0000,x
    jsr cv_tile_release
    ; In the row's span?  Otherwise it is the blank tile.
    lda.l cv_col
    cmp.l cv_mask_lo
    bcc _cf_blank
    cmp.l cv_mask_hi
    beq +
    bcs _cf_blank
+   lda.l cv_tmp
    pha                         ; the ROM flag, across the allocation
    jsr cv_tile_alloc
    sta.l cv_tile
    bne +
    ; No tile left (cannot happen inside the bound): show it blank.
    pla
    bra _cf_blank
+   ora #FB_MAP_ATTR
    pha
    lda.l cv_entry
    tax
    pla
    sta.l $7F0000,x
    pla
    beq +
    ; The ROM floor's own tile: a DMA from ROM, no conversion at all.
    lda.l cv_row
    asl a
    asl a
    asl a
    asl a
    asl a
    clc
    adc.l cv_col
    jsr cv_rom_commit
    jmp _cf_col_next
+
    ; Convert the cell into a ring slot and book it for tile cv_tile.
    jsr cv_ring_reserve
    tya
    and #$FF00
    tcd
    lda.l cv_col
    asl a
    asl a
    asl a
    clc
    adc.l cv_y
    phy
    tay
    lda.l cv_tmp
    tax
    jsr (snesConvFrameTileTab,x)
    ply
    lda #0
    tcd
    jsr cv_ring_commit
    bra _cf_col_next
_cf_blank:
    lda.l cv_entry
    tax
    lda #0
    sta.l $7F0000,x
_cf_col_next:
    lda.l cv_col
    inc a
    sta.l cv_col
    cmp.l cv_col_end
    beq _cf_row_next
    jmp _cf_col
_cf_row_next:
    lda.l cv_col_end
    cmp #32
    beq +
    ; A partial row (snesConvCells): done after its columns.
    jsr cv_flush_run
    pld
    plb
    plp
    rtl
+
    lda.l cv_y
    clc
    adc #2048
    sta.l cv_y
    lda.l cv_row
    inc a
    sta.l cv_row
    cmp.l cv_row_end
    beq +
    jmp _cf_row
+   jsr cv_flush_run
    pld
    plb
    plp
    rtl

; void snesConvCells(u16 row, u16 col0, u16 col1)
;
; The dirty cells of ONE row, columns [col0, col1), for a converter step
; that has to fit in a fraction of a display field: the row's masks are
; pre-shifted to col0 the way the column loop would have shifted them.
snesConvCells:
    php
    rep #$30
    lda 5,s
    sta.l cv_row
    inc a
    sta.l cv_row_end
    lda 5,s
    xba
    asl a
    asl a
    asl a
    and #$F800
    sta.l cv_y
    lda 9,s
    sta.l cv_col_end
    lda 7,s
    sta.l cv_col
    cmp.l cv_col_end
    bcc +
    plp
    rtl
+   phb
    phd
    pea $7E7E
    plb
    plb
    lda.l cv_row
    asl a
    asl a
    tax
    lda.l snes_conv_dirty,x
    sta.l cv_bits
    lda.l snes_conv_dirty+2,x
    sta.l cv_bits_hi
    lda.l snes_conv_rom,x
    sta.l cv_rom
    lda.l snes_conv_rom+2,x
    sta.l cv_rom_hi
    lda.l cv_row
    asl a
    tax
    lda.l snes_conv_rowspan,x
    sta.l cv_tmp
    and #$00FF
    cmp #$00FF
    bne +
    lda #$FFFF
    sta.l cv_mask_lo
    sta.l cv_mask_hi
    bra _cc_shift
+   lsr a
    lsr a
    lsr a
    sta.l cv_mask_lo
    lda.l cv_tmp
    xba
    and #$00FF
    lsr a
    lsr a
    lsr a
    sta.l cv_mask_hi
_cc_shift:
    ; Shift the masks to col0, swapping in the high words at column 16
    ; exactly as the column loop does.
    lda #0
    sta.l cv_tmp
_cc_shift_loop:
    lda.l cv_tmp
    cmp.l cv_col
    beq _cc_go
    cmp #16
    bne +
    lda.l cv_bits_hi
    sta.l cv_bits
    lda.l cv_rom_hi
    sta.l cv_rom
+   lda.l cv_rom
    lsr a
    sta.l cv_rom
    lda.l cv_bits
    lsr a
    sta.l cv_bits
    lda.l cv_tmp
    inc a
    sta.l cv_tmp
    bra _cc_shift_loop
_cc_go:
    jmp _cf_col

; void snesConvEnd(void)
;
; ── Step 3: the map, then the zero-byte completion marker ──────────────
snesConvEnd:
    php
    rep #$30
    lda #2048
    sta.l jp_bytes
    lda.l cv_pool
    beq _cf_queue_a
    ldx #$7F00
    ldy #FB_MAP_B_WORD
    lda #snes_map_shadow_b
    jsl snesFbJobPushRaw
    lda #0
    sta.l jp_bytes
    ldx #3
    ldy #0
    lda.l cv_generation
    jsl snesFbJobPushRaw
    bra _cf_queued
_cf_queue_a:
    ldx #$7F00
    ldy #FB_MAP_A_WORD
    lda #snes_map_shadow_a
    jsl snesFbJobPushRaw
    lda #0
    sta.l jp_bytes
    ldx #2
    ldy #0
    lda.l cv_generation
    jsl snesFbJobPushRaw
_cf_queued:
    lda.l frames_queued
    inc a
    sta.l frames_queued
    plp
    rtl
_cf_overflow:
    ; NOT SILENT: the frame is dropped, and the stamp says so.
    lda.l fb_overflows
    inc a
    sta.l fb_overflows
    lda #0
    sta.l fb_occupied
    sta.b tcc__r0
    plp
    rtl
