;-----------------------------------------------------------------------------
;  snes_conv_drivers.inc — the sparse converter's driver.
;
;  Included at the end of the generated snes_conv_gen.asm so the jsr (table,x)
;  dispatch into the four phase copies of the tile converter stays inside one
;  bank.  The ring, the jobs, the two map shadows, the tile allocator and
;  every persistent cv_* word are declared in snes_fb.asm; the words a call
;  churns -- the cell, its tile and map entry, the run being booked -- are
;  on the fast direct page (snes_fastdp.inc, D = FASTDP for the call), six
;  cycles an access instead of WRAM's eight.
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

; A >>= 1 unless the frame is the doubled one: pixel columns to cells are
; three shifts at 1:1 and two doubled, and the row's bytes the other way.
; Long-addressed, for the entries where DB is the caller's (ldx has no long
; form).  Clobbers X.
.MACRO CV_CELL_LSR
    tax
    lda.l cv_half
    bne +
    txa
    lsr a
    bra ++
+   txa
++
.ENDM

;-----------------------------------------------------------------------------
; Push the pending run as a job.  Clobbers A, X, Y.
;-----------------------------------------------------------------------------
cv_flush_run:
    lda.b <FD_CV_RUN_BYTES
    beq _cfr_none
    sta.l jp_bytes
    lda.l cv_run_bank
    xba
    and #$FF00
    ora.l cv_run_kind
    tax
    lda.b <FD_CV_RUN_DST
    tay
    lda.b <FD_CV_RUN_SRC
    jsl snesFbJobPushRaw
    lda #0
    sta.b <FD_CV_RUN_BYTES
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
    sta.b <FD_CV_TMP
    rts

;-----------------------------------------------------------------------------
; Book the converted tile at ring slot Y into the run bound for VRAM tile
; cv_tile.  Breaks the run first if the tile would not continue it, and after
; it if the slot was the ring's last.
;-----------------------------------------------------------------------------
cv_ring_commit:
    lda.b <FD_CV_RUN_BYTES
    beq _crc_start
    lda.l cv_run_kind
    cmp #1
    bne _crc_break              ; a ROM run is pending: it cannot continue
    lda.b <FD_CV_RUN_SRC
    clc
    adc.b <FD_CV_RUN_BYTES
    sta.b <FD_CV_TMP
    tya
    cmp.b <FD_CV_TMP
    bne _crc_break
    lda.b <FD_CV_RUN_BYTES
    lsr a
    clc
    adc.b <FD_CV_RUN_DST
    sta.b <FD_CV_TMP
    lda.b <FD_CV_TILE
    asl a
    asl a
    asl a
    asl a
    asl a
    cmp.b <FD_CV_TMP
    beq _crc_extend
_crc_break:
    phy
    jsr cv_flush_run
    ply
_crc_start:
    tya
    sta.b <FD_CV_RUN_SRC
    lda #$007E
    sta.l cv_run_bank
    lda #1
    sta.l cv_run_kind
    lda.b <FD_CV_TILE
    asl a
    asl a
    asl a
    asl a
    asl a
    sta.b <FD_CV_RUN_DST
    lda #0
    sta.b <FD_CV_RUN_BYTES
_crc_extend:
    lda.b <FD_CV_RUN_BYTES
    clc
    adc #64
    sta.b <FD_CV_RUN_BYTES
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
    sta.b <FD_CV_ENTRY              ; the tile's ROM address (cv_entry is free here)
    lda.b <FD_CV_RUN_BYTES
    beq _rom_start
    lda.l cv_run_kind
    bne _rom_break
    lda.l cv_run_bank
    cmp.l cv_rom_bank
    bne _rom_break
    lda.b <FD_CV_RUN_SRC
    clc
    adc.b <FD_CV_RUN_BYTES
    cmp.b <FD_CV_ENTRY
    bne _rom_break
    lda.b <FD_CV_RUN_BYTES
    lsr a
    clc
    adc.b <FD_CV_RUN_DST
    sta.b <FD_CV_TMP
    lda.b <FD_CV_TILE
    asl a
    asl a
    asl a
    asl a
    asl a
    cmp.b <FD_CV_TMP
    beq _rom_extend
_rom_break:
    jsr cv_flush_run
_rom_start:
    lda #0
    sta.b <FD_CV_NEXT           ; (the whole-frame loop: no tile continues
                                ; a ROM run)
    lda.b <FD_CV_ENTRY
    sta.b <FD_CV_RUN_SRC
    lda.l cv_rom_bank
    sta.l cv_run_bank
    lda #0
    sta.l cv_run_kind
    sta.b <FD_CV_RUN_BYTES
    lda.b <FD_CV_TILE
    asl a
    asl a
    asl a
    asl a
    asl a
    sta.b <FD_CV_RUN_DST
_rom_extend:
    lda.b <FD_CV_RUN_BYTES
    clc
    adc #64
    sta.b <FD_CV_RUN_BYTES
    rts

;-----------------------------------------------------------------------------
; A = cell row -> the row's first chunky byte: row * 2048 at 1:1 (eight
; 256-byte lines), row * 512 in the doubled motion frame (four 128-byte
; lines, each shown twice).  Clobbers X.
;-----------------------------------------------------------------------------
cv_row_origin:
    xba
    asl a
    and #$FE00                  ; row * 512: four 128-byte lines, doubled
    tax
    lda.l cv_half
    bne +
    txa
    asl a
    asl a                       ; row * 2048 at 1:1: eight 256-byte lines
    rts
+   txa
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
    phd
    lda #FASTDP
    tcd                         ; the per-call words on the fast direct page
    lda 7,s
    and #1
    sta.l cv_pool
    lda 9,s
    sta.l cv_generation
    lda #0
    sta.l cv_occupied
    sta.b <FD_CV_ROW
_cf_count_row:
    lda.b <FD_CV_ROW
    asl a
    tax
    lda.l snes_conv_rowspan,x
    sta.b <FD_CV_TMP
    and #$00FF
    cmp #$00FF
    beq _cf_count_next
    lsr a
    lsr a
    CV_CELL_LSR                 ; eight pixels a cell at 1:1, four doubled
    sta.l cv_mask_lo
    lda.b <FD_CV_TMP
    xba
    and #$00FF
    lsr a
    lsr a
    CV_CELL_LSR
    sec
    sbc.l cv_mask_lo
    inc a
    clc
    adc.l cv_occupied
    sta.l cv_occupied
_cf_count_next:
    lda.b <FD_CV_ROW
    inc a
    sta.b <FD_CV_ROW
    cmp #18
    bne _cf_count_row
    lda.l cv_occupied
    cmp #FB_POOL_TILES+1
    bcc +
    pld
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
    lda #0
    sta.l cv_whole
    ldx #0
_cf_all_dirty:
    lda.l snes_conv_dirty,x
    cmp #$FFFF
    bne _cf_copy_needed
    inx
    inx
    cpx #18*4
    bne _cf_all_dirty
;    ; A WHOLE FRAME EMPTIES THE MAP FIRST.  Every entry of the map being
    ; built is released and zeroed here in one tight pass, and the row loop
    ; then walks only each row's span (_cw_row), allocating and converting
    ; without a mask, a release or a blank store per cell: the column loop
    ; below was doing forty instructions for each of the 400-odd cells a
    ; camera frame leaves blank.
    ;
    ; The release is inline -- cv_tile_release through a jsr with X saved
    ; round it was thirty-five instructions an entry -- with the queue's
    ; tail and count on the fast page for the pass and DB = $7F so the map
    ; reads as `lda abs,y` and X is free for the reference and the queue.
    lda #1
    sta.l cv_whole
    lda.l fb_free_tail
    sta.b <FD_CV_FREE_TAIL
    lda.l fb_free_count
    sta.b <FD_CV_FREE_CNT
    lda.l cv_shadow
    clc
    adc #576*2
    sta.b <FD_CV_Y              ; one past the last entry
    phb
    pea $7F7F
    plb
    plb
    lda.l cv_shadow
    tay
_cf_empty:
    lda.w $0000,y
    and #$03FF
    beq _cf_empty_next
    tax
    sep #$20
.ACCU 8
    lda.l snes_tile_ref,x
    beq _cf_empty_zero          ; already free: a bug elsewhere, but harmless
    dec a
    sta.l snes_tile_ref,x
    bne _cf_empty_zero
    rep #$20
.ACCU 16
    txa
    ldx.b <FD_CV_FREE_TAIL
    sta.l snes_tile_free,x
    inx
    inx
    cpx #FB_FREE_BYTES
    bne +
    ldx #0
+   stx.b <FD_CV_FREE_TAIL
    inc.b <FD_CV_FREE_CNT
    bra _cf_empty_clear
_cf_empty_zero:
    rep #$20
.ACCU 16
_cf_empty_clear:
    lda #0
    sta.w $0000,y
_cf_empty_next:
    iny
    iny
    cpy.b <FD_CV_Y
    bne _cf_empty
    plb
    lda.b <FD_CV_FREE_TAIL
    sta.l fb_free_tail
    lda.b <FD_CV_FREE_CNT
    sta.l fb_free_count
    bra _cf_copy_done
_cf_copy_needed:
    lda #0
    sta.b <FD_CV_Y                  ; byte index of the cell in a map
_cf_copy:
    lda.b <FD_CV_Y
    clc
    adc.l cv_shadow
    tax
    lda.l $7F0000,x
    sta.b <FD_CV_TMP                ; what the new map held
    lda.b <FD_CV_Y
    clc
    adc.l cv_other
    tax
    lda.l $7F0000,x
    sta.b <FD_CV_COL                ; what the shown map holds
    cmp.b <FD_CV_TMP
    beq _cf_copy_next
    lda.b <FD_CV_TMP
    jsr cv_tile_release
    lda.b <FD_CV_COL
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
+   lda.b <FD_CV_Y
    clc
    adc.l cv_shadow
    tax
    lda.b <FD_CV_COL
    sta.l $7F0000,x
_cf_copy_next:
    lda.b <FD_CV_Y
    inc a
    inc a
    sta.b <FD_CV_Y
    cmp #576*2
    bne _cf_copy
_cf_copy_done:
    lda.l cv_occupied
    pld
    sta.b tcc__r0
    plp
    rtl

; void snesConvRows(u16 row0, u16 row1)
;
; ── Step 2: the dirty cells of rows [row0, row1) ─────────────────────────
snesConvRows:
    php
    rep #$30
    phb
    phd
    lda #FASTDP
    tcd                         ; the per-call words on the fast direct page
    pea $7E7E
    plb
    plb
    lda #32
    sta.b <FD_CV_COL_END        ; whole rows (see _cf_row_next)
    lda 10,s
    sta.l cv_row_end
    lda 8,s
    sta.b <FD_CV_ROW
    ; The cell row's first chunky byte: row * 2048 (512 doubled).
    jsr cv_row_origin
    sta.b <FD_CV_Y
    lda #0
    sta.b <FD_CV_RUN_BYTES      ; no run pending: every call flushes its own
    sta.l cv_partial
    lda.b <FD_CV_ROW
    cmp.l cv_row_end
    bcc +
    pld
    plb
    plp
    rtl
+
    lda.l cv_whole
    beq _cf_row
    jmp _cw_row
_cf_row:
    lda.b <FD_CV_ROW
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
    lda.b <FD_CV_ROW
    asl a
    tax
    lda.l snes_conv_rowspan,x
    sta.b <FD_CV_TMP
    and #$00FF
    cmp #$00FF
    bne +
    lda #$FFFF                  ; an empty row: no column is in the span
    sta.l cv_mask_lo
    sta.l cv_mask_hi
    bra _cf_col_start
+   lsr a
    lsr a
    ldx.w cv_half
    bne +
    lsr a
+   sta.l cv_mask_lo            ; first occupied cell column
    lda.b <FD_CV_TMP
    xba
    and #$00FF
    lsr a
    lsr a
    ldx.w cv_half
    bne +
    lsr a
+   sta.l cv_mask_hi            ; last occupied cell column
_cf_col_start:
    lda #0
    sta.b <FD_CV_COL
    lda #32
    sta.b <FD_CV_COL_END
_cf_col:
    lda.b <FD_CV_COL
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
    sta.b <FD_CV_TMP
    ; The cell's map entry: release what the new map named there.
    lda.b <FD_CV_ROW
    asl a
    asl a
    asl a
    asl a
    asl a
    clc
    adc.b <FD_CV_COL
    asl a
    clc
    adc.l cv_shadow
    sta.b <FD_CV_ENTRY
    tax
    lda.l $7F0000,x
    jsr cv_tile_release
    ; In the row's span?  Otherwise it is the blank tile.
    lda.b <FD_CV_COL
    cmp.l cv_mask_lo
    bcc _cf_blank
    cmp.l cv_mask_hi
    beq +
    bcs _cf_blank
+   lda.b <FD_CV_TMP
    pha                         ; the ROM flag, across the allocation
    jsr cv_tile_alloc
    sta.b <FD_CV_TILE
    bne +
    ; No tile left (cannot happen inside the bound): show it blank.
    pla
    bra _cf_blank
+   ora #FB_MAP_ATTR
    pha
    lda.b <FD_CV_ENTRY
    tax
    pla
    sta.l $7F0000,x
    pla
    beq +
    ; The ROM floor's own tile: a DMA from ROM, no conversion at all.
    lda.b <FD_CV_ROW
    asl a
    asl a
    asl a
    asl a
    asl a
    clc
    adc.b <FD_CV_COL
    jsr cv_rom_commit
    jmp _cf_col_next
+
    ; Convert the cell into a ring slot and book it for tile cv_tile.
    jsr cv_ring_reserve         ; Y = the slot, FD_CV_TMP = its phase * 2
    phy                         ; the slot, for the commit
    tya
    and #$FF00
    pha                         ; the slot's page: the tile routine's D
    lda.b <FD_CV_COL
    asl a
    asl a
    ldx.w cv_half
    bne +
    asl a                       ; eight texels a cell at 1:1, four doubled
+   clc
    adc.b <FD_CV_Y
    ldx.b <FD_CV_TMP
    tay                         ; Y = the cell's first chunky byte
    pld                         ; D = the ring page (the fast page is gone
                                ; until the tile is done)
    lda.l cv_half
    bne +
    jsr (snesConvFrameTileTab,x)
    bra ++
+   jsr (snesConvHalfTileTab,x)
++  lda #FASTDP
    tcd
    ply
    jsr cv_ring_commit
    bra _cf_col_next
_cf_blank:
    lda.b <FD_CV_ENTRY
    tax
    lda #0
    sta.l $7F0000,x
_cf_col_next:
    lda.b <FD_CV_COL
    inc a
    sta.b <FD_CV_COL
    cmp.b <FD_CV_COL_END
    beq _cf_row_next
    jmp _cf_col
_cf_row_next:
    lda.b <FD_CV_COL_END
    cmp #32
    beq +
    ; A partial row (snesConvCells): done after its columns.
    jsr cv_flush_run
    pld
    plb
    plp
    rtl
+
    lda.b <FD_CV_ROW
    inc a
    sta.b <FD_CV_ROW
    jsr cv_row_origin           ; the next row's first chunky byte
    sta.b <FD_CV_Y
    lda.b <FD_CV_ROW
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
    phb
    phd
    lda #FASTDP
    tcd
    pea $7E7E
    plb
    plb
    lda 8,s
    sta.b <FD_CV_ROW
    inc a
    sta.l cv_row_end
    lda 8,s
    jsr cv_row_origin
    sta.b <FD_CV_Y
    lda 12,s
    sta.b <FD_CV_COL_END
    lda 10,s
    sta.b <FD_CV_COL
    lda #0
    sta.b <FD_CV_RUN_BYTES
    lda #1
    sta.l cv_partial
    lda.b <FD_CV_COL
    cmp.b <FD_CV_COL_END
    bcc +
    pld
    plb
    plp
    rtl
+
    lda.l cv_whole
    beq +
    jmp _cw_cells
+
    lda.b <FD_CV_ROW
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
    lda.b <FD_CV_ROW
    asl a
    tax
    lda.l snes_conv_rowspan,x
    sta.b <FD_CV_TMP
    and #$00FF
    cmp #$00FF
    bne +
    lda #$FFFF
    sta.l cv_mask_lo
    sta.l cv_mask_hi
    bra _cc_shift
+   lsr a
    lsr a
    ldx.w cv_half
    bne +
    lsr a
+   sta.l cv_mask_lo
    lda.b <FD_CV_TMP
    xba
    and #$00FF
    lsr a
    lsr a
    ldx.w cv_half
    bne +
    lsr a
+   sta.l cv_mask_hi
_cc_shift:
    ; Shift the masks to col0, swapping in the high words at column 16
    ; exactly as the column loop does.
    lda #0
    sta.b <FD_CV_TMP
_cc_shift_loop:
    lda.b <FD_CV_TMP
    cmp.b <FD_CV_COL
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
    lda.b <FD_CV_TMP
    inc a
    sta.b <FD_CV_TMP
    bra _cc_shift_loop
_cc_go:
    jmp _cf_col

;-----------------------------------------------------------------------------
; THE WHOLE-FRAME ROW LOOP (cv_whole): the map is empty, every cell in the
; row's span gets a fresh tile -- the ROM floor's by DMA where the ROM mask
; says so, converted otherwise -- and nothing else is touched.  Enters with
; DB = $7E, D pushed, cv_row/cv_row_end (and, from snesConvCells, cv_col/
; cv_col_end for one row) set.
;
; ONE STRAIGHT LINE A CELL.  The allocator, the ring reservation and the
; run commit used to be the shared subroutines below, each recomputing the
; cell's map entry, chunky offset and cell index from the row and column
; and reaching the allocator's and ring's state through long WRAM stores:
; about 170 instructions a converted tile, more than the tile conversion
; itself.  Here the per-cell terms are RUNNING COUNTERS on the fast page
; (the map entry pointer, the chunky offset, the cell index, the ROM mask
; shifted a bit a column), the free queue's head and the ring's write index
; are cached there for the row and written back at its end, and the run
; continues when the fresh tile is THE ONE THE RUN EXPECTS (FD_CV_NEXT,
; kept as a tile id: ring slots are handed out in order, so a run whose
; tile ids are consecutive is contiguous at both ends; a ROM run in between
; sets the expected id to 0, which no tile has).  About sixty instructions.
;
; The state the general path (_cf_col) and the subroutines keep in WRAM --
; fb_free_head/count, ring_wrote -- is loaded at each row's start and stored
; at its end, so the two paths never see each other's copy stale: the ISR
; reads none of it (the NMI only ever advances ring_freed and the jobs).
;-----------------------------------------------------------------------------

; The column loop, once for the 1:1 frame (SHIFT 3: eight texels a cell,
; snesConvFrameTileTab64) and once for the doubled one (SHIFT 2, four).
; Fast-page words for the row: FD_CV_COL becomes the cell count, FD_CV_Y
; the running chunky offset, FD_CV_ENTRY the running map entry, FD_CV_TMP
; the cell's ring slot across its tile routine.
.MACRO CW_LOOP ARGS SHIFT, TAB
    ; The row's running terms from its clipped first column.
    lda.b <FD_CV_COL_END
    sec
    sbc.b <FD_CV_COL
    beq _cw_empty\@
    bcs +
_cw_empty\@:
    jmp _cw_row_done            ; nothing of the span in [col, col_end)
+   tay                         ; the cell count, parked
    lda.b <FD_CV_ROW
    asl a
    asl a
    asl a
    asl a
    asl a
    clc
    adc.b <FD_CV_COL
    asl a
    clc
    adc.l cv_shadow
    sta.b <FD_CV_ENTRY          ; the first cell's map entry
    lda.b <FD_CV_COL
    .REPT SHIFT
    asl a
    .ENDR
    clc
    adc.b <FD_CV_Y
    sta.b <FD_CV_Y              ; ...and its first chunky byte
    ; The ROM mask, shifted so its low bit is this column's.
    lda.b <FD_CV_ROW
    asl a
    asl a
    tax
    lda.l snes_conv_rom,x
    sta.b <FD_CV_ROM_LO
    lda.l snes_conv_rom+2,x
    sta.b <FD_CV_ROM_HI
    ldx.b <FD_CV_COL
    beq ++
-   lsr.b <FD_CV_ROM_HI
    ror.b <FD_CV_ROM_LO
    dex
    bne -
++  sty.b <FD_CV_COL            ; cells to go
    lda.l fb_free_head
    sta.b <FD_CV_FREE_HEAD
    lda.l fb_free_count
    sta.b <FD_CV_FREE_CNT
_cw_col\@:
    ; A tile off the free queue, with a reference of one.
    lda.b <FD_CV_FREE_CNT
    bne +
    jmp _cw_none\@              ; none left (outside the bound): blank
+   dec a
    sta.b <FD_CV_FREE_CNT
    ldx.b <FD_CV_FREE_HEAD
    lda.l snes_tile_free,x
    sta.b <FD_CV_TILE
    inx
    inx
    cpx #FB_FREE_BYTES
    bne +
    ldx #0
+   stx.b <FD_CV_FREE_HEAD
    tax
    sep #$20
.ACCU 8
    lda #1
    sta.l snes_tile_ref,x
    rep #$20
.ACCU 16
    txa
    ora #FB_MAP_ATTR
    ldx.b <FD_CV_ENTRY
    sta.l $7F0000,x
    ; From the ROM floor?
    lsr.b <FD_CV_ROM_HI
    ror.b <FD_CV_ROM_LO
    bcc +
    lda.b <FD_CV_ENTRY
    pha                         ; (cv_rom_commit's scratch)
    sec
    sbc.l cv_shadow
    lsr a                       ; the cell index
    jsr cv_rom_commit
    pla
    sta.b <FD_CV_ENTRY
    jmp _cw_next\@
+   ; A ring slot: wait for one if the ring is full (flushing the pending
    ; run first, or the ISR could never free the slots it holds).
    lda.l ring_wrote
    sec
    sbc.l ring_freed
    cmp #FB_RING_SIZE - 64 + 1
    bcc _cw_room\@
    jsr cv_flush_run
-   lda.l ring_wrote
    sec
    sbc.l ring_freed
    cmp #FB_RING_SIZE - 64 + 1
    bcs -
_cw_room\@:
    lda.l ring_wrote
    and #FB_RING_MASK
    clc
    adc #snes_ring
    sta.b <FD_CV_TMP            ; the slot
    and #$00C0
    tax                         ; its phase, as the 64-byte-spaced table's
                                ; index
    ldy.b <FD_CV_Y
    lda.b <FD_CV_TMP
    and #$FF00
    pha
    pld                         ; D = the ring page for the tile routine
    jsr (TAB,x)
    lda #FASTDP
    tcd
    ; Book it: the run continues when this is the tile it expects.
    lda.b <FD_CV_RUN_BYTES
    beq _cw_start\@
    lda.b <FD_CV_TILE
    cmp.b <FD_CV_NEXT
    beq _cw_extend\@
    jsr cv_flush_run
_cw_start\@:
    lda.b <FD_CV_TMP
    sta.b <FD_CV_RUN_SRC
    lda #$007E
    sta.l cv_run_bank
    lda #1
    sta.l cv_run_kind
    lda.b <FD_CV_TILE
    sta.b <FD_CV_NEXT           ; (+1 below)
    asl a
    asl a
    asl a
    asl a
    asl a
    sta.b <FD_CV_RUN_DST
    lda #0
    sta.b <FD_CV_RUN_BYTES
_cw_extend\@:
    lda.b <FD_CV_RUN_BYTES
    clc
    adc #64
    sta.b <FD_CV_RUN_BYTES
    inc.b <FD_CV_NEXT
    lda.l ring_wrote
    clc
    adc #64
    sta.l ring_wrote
    and #FB_RING_MASK
    bne _cw_next\@
    jsr cv_flush_run            ; a run never crosses the ring's end
    bra _cw_next\@
_cw_none\@:
    lda #0
    ldx.b <FD_CV_ENTRY
    sta.l $7F0000,x
_cw_next\@:
    inc.b <FD_CV_ENTRY
    inc.b <FD_CV_ENTRY
    lda.b <FD_CV_Y
    clc
    adc #1 << SHIFT
    sta.b <FD_CV_Y
    dec.b <FD_CV_COL
    beq +
    jmp _cw_col\@
+   lda.b <FD_CV_FREE_HEAD
    sta.l fb_free_head
    lda.b <FD_CV_FREE_CNT
    sta.l fb_free_count
    jmp _cw_row_done
.ENDM

_cw_row:
    lda #0
    sta.b <FD_CV_COL
    lda #32
    sta.b <FD_CV_COL_END
_cw_cells:
    ; The row's span in cells; an empty row has none.
    lda.b <FD_CV_ROW
    asl a
    tax
    lda.l snes_conv_rowspan,x
    sta.b <FD_CV_TMP
    and #$00FF
    cmp #$00FF
    bne +
    jmp _cw_row_done
+   lsr a
    lsr a
    ldx.w cv_half
    bne +
    lsr a
+   sta.l cv_span_lo
    lda.b <FD_CV_TMP
    xba
    and #$00FF
    lsr a
    lsr a
    ldx.w cv_half
    bne +
    lsr a
+   sta.l cv_span_hi
    ; Clip [cv_col, cv_col_end) to [span_lo, span_hi].
    lda.b <FD_CV_COL
    cmp.l cv_span_lo
    bcs +
    lda.l cv_span_lo
    sta.b <FD_CV_COL
+   lda.l cv_span_hi
    inc a
    cmp.b <FD_CV_COL_END
    bcs +
    sta.b <FD_CV_COL_END
+   lda.l cv_half
    beq +
    jmp _cw_half
+   CW_LOOP 3, snesConvFrameTileTab64
_cw_half:
    CW_LOOP 2, snesConvHalfTileTab64
_cw_row_done:
    lda.l cv_partial
    beq +
    ; A partial row (snesConvCells): done after its columns.
    jsr cv_flush_run
    pld
    plb
    plp
    rtl
+   lda.b <FD_CV_ROW
    inc a
    sta.b <FD_CV_ROW
    jsr cv_row_origin
    sta.b <FD_CV_Y
    lda.b <FD_CV_ROW
    cmp.l cv_row_end
    beq +
    jmp _cw_row
+   jsr cv_flush_run
    pld
    plb
    plp
    rtl

snes_conv_bit16:
    .dw $0001, $0002, $0004, $0008, $0010, $0020, $0040, $0080
    .dw $0100, $0200, $0400, $0800, $1000, $2000, $4000, $8000

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
