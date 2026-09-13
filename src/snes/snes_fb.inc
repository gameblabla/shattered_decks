; Constants shared by the sparse converter and NMI uploader.
.DEFINE FB_RING_SIZE       4096
.DEFINE FB_RING_MASK       (FB_RING_SIZE - 1)
.DEFINE FB_JOBS            32
.DEFINE FB_BUDGET          3584   ; the whole vblank's transfer allowance, in
                                  ; bytes; the main thread's own DMAs (OAM,
                                  ; card rows, palettes) are reserved out of it
.DEFINE FB_MAP_A_WORD      $5800
.DEFINE FB_MAP_B_WORD      $5C00
.DEFINE FB_MAP_ATTR        $1C00
.DEFINE FB_POOL_TILES      351
.DEFINE FB_POOL_B_TILE     352
.DEFINE FB_TILES           702    ; physical tiles 1..702; 0 is the blank
.DEFINE FB_FREE_BYTES      1408   ; the free queue: 704 words

.DEFINE JOB_SRC            0
.DEFINE JOB_BANK           2
.DEFINE JOB_KIND           3
.DEFINE JOB_DST            4
.DEFINE JOB_BYTES          6
