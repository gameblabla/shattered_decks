; snes_fastdp.inc -- the 3.5 MHz scratch page.
;
; EVERY BYTE OF WRAM IS AN EIGHT-CYCLE ACCESS, the direct page and the stack
; included; FastROM, the B-bus ports and the CPU's own registers are six.
; The one writable memory on the fast side is the DMA controller: each
; channel's twelve registers ($43x0-$43xB) are plain read/write latches that
; a transfer only reads, so a channel that never runs is twelve bytes of RAM
; at ROM speed.  Channels 1-3 never move anything here (0 is the main
; thread's general DMA, 4-6 carry the HDMA tables, 7 is the NMI drain's),
; which makes $4310-$433B thirty-six bytes of it.
;
; With D = $4300 they are direct-page bytes: `lda.b $13` is four fast cycles
; against the same load's five (one of them slow) from a WRAM direct page --
; a quarter off every accumulator step in the span walkers, which is where
; the port's frames go.  D's low byte is zero, so no access pays the
; unaligned-D cycle.
;
; $43x0 is left alone: DMAPx has a bit the hardware may not latch, and the
; emulator cannot tell.  $43x1-$43xB are whole registers (A-bus address,
; byte count, bank, HDMA table address, line counter, and the spare $43xB).
;
; THE WHOLE PAGE IS LEAF SCRATCH.  Nothing that lives across a call may be
; put here: the span walkers, the motion mapper, the sprite store and the
; converter driver each own it for the duration of one call and never nest.
; (The walkers' bounded-run dispatcher also uses the CPU multiplier and
; divider, $4202-$4206 / $4214-$4216: nothing that could be in flight
; during a walker -- the NMI included -- touches them.)
; The NMI touches none of it (pvsneslib's handler sets its own D, and the
; drain only names channel 7's registers).
.DEFINE FASTDP          $4300

; Channel 1: the span walkers and the motion mapper (snes_raster.asm), the
; sprite store (snes_oam.asm).
.DEFINE FD_UFRAC        $11     ; byte: the walkers' u fraction
.DEFINE FD_DUFRAC       $12     ; byte: du's fraction
.DEFINE FD_DUINT        $13     ; byte: du's whole texels
.DEFINE FD_U            $11     ; word: the quad walkers' Q8.8 u
.DEFINE FD_V            $13     ; word: ...and v
.DEFINE FD_DU           $15     ; word
.DEFINE FD_DV           $17     ; word
.DEFINE FD_PAGE         $19     ; word: the face's page, in the high byte
.DEFINE FD_QTMP         $21     ; word: the quad walkers' u half of the index
                                ; (channel 2's, free: no converter runs
                                ; inside a walker)
.DEFINE FD_BT_SHEET     $23     ; the board texture stamp: which 32x32 sheet,
.DEFINE FD_BT_WIDTH     $25     ; its footprint, and the row's first world
.DEFINE FD_BT_HEIGHT    $27     ; texel
.DEFINE FD_BT_DST       $29
.DEFINE FD_BT_V         $31     ; the stamp's Q4.12 row over the face and its
.DEFINE FD_BT_DV        $33     ; step (channel 3's: FD_V would overlap the
                                ; walk's FD_DUINT byte)
.DEFINE FD_BT_ENTRY     $35     ; the row run's entry - 1 (0: the walk) and
.DEFINE FD_BT_RUN       $37     ; its length, width - 1
.DEFINE FD_MAP_U        $11     ; the motion mapper: u, v, du, dv, then the
.DEFINE FD_MAP_V        $13     ; 24-bit texture pointer
.DEFINE FD_MAP_DU       $15
.DEFINE FD_MAP_DV       $17
.DEFINE FD_MAP_PTR      $19     ; three bytes: $19-$1B
; (The sprite store's own FD_OAM_* / FD_TXT_* / FD_NUM_* bytes are declared
; in snes_oam.asm: channel 1 for the entry, channel 2 for the emitters.)

; Channels 2-3: the converter driver's per-call words (snes_conv_drivers.inc).
.DEFINE FD_CV_COL       $21
.DEFINE FD_CV_ROW       $23
.DEFINE FD_CV_TILE      $25
.DEFINE FD_CV_ENTRY     $27
.DEFINE FD_CV_Y         $29
.DEFINE FD_CV_TMP       $31
.DEFINE FD_CV_RUN_BYTES $33
.DEFINE FD_CV_RUN_SRC   $35
.DEFINE FD_CV_RUN_DST   $37
.DEFINE FD_CV_COL_END   $39

; Channel 3 again: the bounded run's dispatcher (snes_raster.asm, RS_RUN),
; live only inside a walker -- the converter never runs inside one, and the
; stamp, which has its own run, does not use them.
.DEFINE FD_RUN_CNT      $35     ; word: texels still to walk
.DEFINE FD_RUN_N        $37     ; word: the piece being walked
.DEFINE FD_RUN_TMP      $39     ; word: X parked across the entry lookup
