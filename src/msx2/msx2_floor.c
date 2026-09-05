// ─────────────────────────────────────────────────────────────────────────────
//  msx2_floor.c — the textured board, one scanline span at a time
//
//  Included in the duel bank.  Visible spans replace all polygon edge walking:
//  tools/msx2/gen_msx_floor.py bakes, per authored camera pose, what each of
//  the band's 114 rows shows, and this walks that list.
//
//  THE PIXEL RATE IS THE WHOLE DESIGN.  A GRAPHIC 7 byte written through the
//  data port needs about 29 T-states of spacing while the display is on, so a
//  13,500-pixel arena cannot cost less than ~110 ms however clever the mapper
//  is, and everything else in the frame has to fit in what is left of 200 ms.
//  Three things follow from that, and they are why this file is mostly Z80:
//
//   1. The two samplers land just over the VDP's floor.  A constant-v span
//      (`Msx2_FloorFast`, 33 T a pixel) has had v's texture row folded into the
//      address by the generator, so the accumulator's high byte IS the texel's
//      low address byte: `ld c,h / ld a,(bc) / out / add hl,de` is the whole
//      mapper, unrolled eight deep and entered part-way through for the
//      remainder so the loop control disappears.  A moving-v span
//      (`Msx2_FloorSlow`, ~44 T a pixel) walks v in IY scaled by sixteen -- its
//      texture row is then already the top nibble of IYH -- with the step for
//      IY in SP, because `add iy,rr` has no other free pair.  It writes each
//      sample twice: the second write is what fills the VDP's own spacing, so
//      the doubling is free, and only rotating poses use it.
//   2. The record walk is assembly (`Msx2_FloorRow`).  In C it cost about 2800
//      T-states a record -- SDCC keeps that many locals in an IX frame -- which
//      was over half the frame, more than every pixel of the board put
//      together.
//   3. Flat material is the command engine's job and black is not painted per
//      row at all: see `Msx2_FloorBackdrop`.
//
//  `ld a,iyh` and `add iy,sp` are assembled as bytes: sdasz80 rejects the
//  index-half mnemonics, and every real Z80 (and openMSX) implements them.
// ─────────────────────────────────────────────────────────────────────────────
#include "msx2_floor_data.h"

// The NEO mapper, as msx2_stream.c uses it: the row records are read straight
// out of the cartridge window rather than through Msx2_RomRead, whose offset
// normalisation and straddle check cost more than the copy itself.
#define MSX2_FLOOR_BANK_REG   0x7000
#define MSX2_FLOOR_WINDOW     0x8000
#define MSX2_FLOOR_CODE_SEG   1

// One 256-byte-aligned block: the two 16x16 mips first (a mip row must be
// reachable by changing one address byte), then the two fine 32x32 materials.
static u8 g_floor_mem[MSX2_FLOOR_TEX_BYTES + 256];
static u8 g_floor_page;                 // high byte of the aligned base
u8 g_msx2_arena_top;                    // see msx2_arena.h
static u8 g_floor_ready;

// One header per pose: 115 row offsets (the last closes row 113), then the
// backdrop rectangle table -- count, then x/y/w(0=256)/h quads.
static u8 g_floor_head[MSX2_FLOOR_SPLIT_AT + 1];
#define g_floor_rows  ((u16*)g_floor_head)
#define g_floor_rects (g_floor_head + 115 * 2)
// How many screen rows one drawn row stands for while the camera moves: baked
// per pose (2 or 3), since the band is 114 rows and both divide it.
#define g_floor_stride (g_floor_head[MSX2_FLOOR_STRIDE_AT])
// The band rows the arena reaches.  Outside them there is nothing to fetch and
// nothing to walk: the row only has to go back to black where this page's last
// pose left something, and the sliver pass is already exactly that.
#define g_floor_row0   (g_floor_head[MSX2_FLOOR_ROW0_AT])
#define g_floor_row1   (g_floor_head[MSX2_FLOOR_ROW1_AT])
// The band row at which a two-row pose starts trebling its rows instead.
#define g_floor_split  (g_floor_head[MSX2_FLOOR_SPLIT_AT])
static u8 g_floor_row[MSX2_FLOOR_ROW_BYTES];

// The sampler's parameter block.  It is one array because the assembly walker
// copies a span record straight into its first eight bytes -- the record is
// laid out in exactly this order -- and because every one of these has to be
// in memory: neither loop has a register to spare.
static u8 g_fp[14];
#define FP_ACC     0            // u accumulator: H is the texel address byte
#define FP_DU      2
#define FP_SDV     0            // the moving-v record, in pop order
#define FP_SACC    2
#define FP_SDU     4
#define FP_SVACC   6
#define FP_PTR     8            // C scratch, B texture page
#define FP_BLOCKS 10
#define FP_REM    11
#define FP_ODD    12
#define FP_N      13            // span length, 0 meaning 256

static u16 g_fp_sp;             // the real stack, while SP carries dV
static u16 g_fp_rec;            // the span record the sampler pops its state from
static u16 g_fp_p;              // record cursor
static u8 g_fp_y;
static u8 g_fp_x;
static u8 g_fp_w;
static u8 g_fp_mat;
static u8 g_fp_busy;            // a fill is in flight; the port must wait
static u8 g_fp_r14;             // this row's VRAM address, less the column
static u8 g_fp_hi;
static u8 g_fp_color;
static u8 g_fp_wide;
static u8 g_fp_skip;            // 8 if this span walks v as well, else 4
static u8 g_fp_h = 1;           // rows a flat span covers: a doubled group
static u16 g_bd_at, g_bd_el, g_bd_er;   // the band loop's cursors
static u8 g_bd_y, g_bd_rows, g_bd_stride, g_bd_delta, g_bd_r14;
static u8 g_bd_extra, g_bd_page, g_bd_y0, g_bd_y1, g_bd_stride_1, g_bd_split;
static u8 g_fp_ol, g_fp_or;     // what this page's arena covered last time
static u8 g_fp_nl, g_fp_nr;     // and what it covers now
static u8 g_fp_atok;            // the data port is already on the next column

// What each page's arena covered when it was last drawn, so the next draw of
// that page knows the only black worth repainting.  Valid only while nothing
// but the arena and its cards has touched the band -- Msx2_ArenaMoveStart
// clears it at the top of every camera move, which is the one stretch where
// that holds and also the one where the frame rate is spent.
static u8 g_ext_l[2][MSX2_BAND_H];
static u8 g_ext_r[2][MSX2_BAND_H];
static u8 g_ext_ok[2];
static u16 g_fp_src;            // window address of the row's records
static u16 g_fp_seg;
static u8 g_fp_len;
static u8 g_fp_end;

static const u8 g_floor_colors[3] =
    { MSX2_BLACK, MSX2_BOARD_WALL_Z, MSX2_BOARD_WALL_X };

static void Msx2_FloorLoad(void)
{
    if(!g_floor_ready)
    {
        u16 base = (u16)(((u16)g_floor_mem + 255) & 0xFF00);
        g_floor_page = (u8)(base >> 8);
        Msx2_RomReadLong(MSX2_FLOOR_TEXTURE_SEG, 0, (u8*)base,
                         MSX2_FLOOR_TEX_BYTES);
        g_floor_ready = 1;
    }
}

// ── The two samplers ─────────────────────────────────────────────────────────
//
// Both own the VRAM pointer for the length of a span and both borrow the
// alternate set for their loop counter, so it is saved and restored before EI:
// the PSG interrupt is allowed to use it.

static void Msx2_FloorFast(void) __naked
{
    __asm
        di
        push iy
        // The alternate set needs no saving: every MSXgl ISR variant pushes
        // BC'/DE'/HL' and IX/IY itself, and it cannot run inside the DI anyway.
        // blocks = (n + 7) >> 3, with n = 0 meaning a full 256, and the first
        // block entered five bytes per pixel it does not owe.
        ld a, (_g_fp + FP_N)
        or a
        jr nz, 00902$
        ld a, #32
        ld c, #0
        jr 00903$
    00902$:
        ld c, a
        add a, #7
        rra                     // nine bits: the carry is bit eight
        srl a
        srl a
    00903$:
        exx
        ld b, a
        exx
        ld a, c
        neg
        and #7
        ld c, a
        add a, a
        add a, a
        add a, c                // five bytes a pixel
        ld c, a
        ld b, #0
        ld hl, #00901$
        add hl, bc
        push hl
        pop iy
        ld (_g_fp_sp), sp
        ld sp, (_g_fp_rec)
        pop hl                  // accumulator
        pop de                  // step
        ld sp, (_g_fp_sp)
        ld bc, (_g_fp + FP_PTR)
        jp (iy)
    00901$:
        ld c, h
        ld a, (bc)
        out (#0x98), a
        add hl, de
        ld c, h
        ld a, (bc)
        out (#0x98), a
        add hl, de
        ld c, h
        ld a, (bc)
        out (#0x98), a
        add hl, de
        ld c, h
        ld a, (bc)
        out (#0x98), a
        add hl, de
        ld c, h
        ld a, (bc)
        out (#0x98), a
        add hl, de
        ld c, h
        ld a, (bc)
        out (#0x98), a
        add hl, de
        ld c, h
        ld a, (bc)
        out (#0x98), a
        add hl, de
        ld c, h
        ld a, (bc)
        out (#0x98), a
        add hl, de
        exx
        dec b
        exx
        jr nz, 00901$
        pop iy
        ei
        ret
    __endasm;
}

// Pairs of pixels: the odd pairs first, then blocks of four, then the odd
// pixel if the span has one -- by then the accumulators are exactly where it
// wants them, because the doubled step has advanced u by du*(n-1).
static void Msx2_FloorSlow(void) __naked
{
    __asm
        di
        push iy
        ld a, (_g_fp + FP_N)    // pairs = n >> 1 (n is never 256 here)
        srl a
        ld c, a
        and #1
        ld (_g_fp + FP_REM), a
        ld a, c
        rra
        and #0x7F
        ld (_g_fp + FP_BLOCKS), a
        ld (_g_fp_sp), sp
        ld sp, (_g_fp_rec)
        pop hl                  // dV, which SP itself will carry
        pop bc                  // accumulator, via BC: HL is the pop target
        pop de                  // step
        pop iy                  // v accumulator
        ld sp, hl
        ld h, b
        ld l, c
        ld bc, (_g_fp + FP_PTR)
        ld a, (_g_fp + FP_REM)
        or a
        jr z, 00911$
        exx
        ld b, a
        exx
    00910$:
        .db 0xFD, 0x7C          // ld a, iyh
        and #0xF0
        or h
        ld c, a
        ld a, (bc)
        out (#0x98), a
        add hl, de
        .db 0xFD, 0x39          // add iy, sp
        nop
        out (#0x98), a
        exx
        dec b
        exx
        jr nz, 00910$
    00911$:
        ld a, (_g_fp + FP_BLOCKS)
        or a
        jr z, 00913$
        exx
        ld b, a
        exx
    00912$:
        .db 0xFD, 0x7C
        and #0xF0
        or h
        ld c, a
        ld a, (bc)
        out (#0x98), a
        add hl, de
        .db 0xFD, 0x39          // twenty-nine T-states since the last write
        out (#0x98), a
        .db 0xFD, 0x7C
        and #0xF0
        or h
        ld c, a
        ld a, (bc)
        out (#0x98), a
        add hl, de
        .db 0xFD, 0x39          // twenty-nine T-states since the last write
        out (#0x98), a
        exx
        dec b
        exx
        jr nz, 00912$
    00913$:
        ld a, (_g_fp + FP_ODD)
        or a
        jr z, 00914$
        .db 0xFD, 0x7C
        and #0xF0
        or h
        ld c, a
        ld a, (bc)
        out (#0x98), a
    00914$:
        ld sp, (_g_fp_sp)
        pop iy
        ei
        ret
    __endasm;
}

// ── Flat material ────────────────────────────────────────────────────────────
//
// A V9938 command is about three hundred T-states of set-up and then paints
// GRAPHIC 7 at ~2.75 us a pixel; these writes cost 32 T-states each.  Below a
// dozen pixels the CPU wins, and it also leaves the command engine idle, which
// is worth more than it looks: every span after a fill has to wait for it.
#define MSX2_FLOOR_FILL_MIN 12

static void Msx2_FloorSolidRun(void) __naked
{
    __asm
        di
        ld a, (_g_fp + FP_N)
        ld b, a
        ld a, (_g_fp_color)
    00921$:
        out (#0x98), a
        nop
        djnz 00921$
        ei
        ret
    __endasm;
}

static void Msx2_FloorPoke(void);

// Everything about the write address except the column: the walker sets it
// once a row, and a clipped repair once a row too.
static void Msx2_FloorRowAddr(u8 y)
{
    u16 line = (u16)y + ((u16)Msx2_VideoGetDrawPage() << 8);

    g_fp_y = y;
    g_fp_r14 = (u8)(line >> 6);
    g_fp_hi = (u8)((u8)(line & 0x3F) | 0x40);
}

// Called from the walker, which has already put x, width and the material in
// place.  Width zero means the whole 256.
static void Msx2_FloorFlatSpan(void)
{
    u8 color = g_floor_colors[g_fp_mat];

    if(g_fp_w == 0 || g_fp_h != 1 || g_fp_w >= MSX2_FLOOR_FILL_MIN)
    {
        Msx2_Fill(g_fp_x, g_fp_y, g_fp_w ? g_fp_w : 256, g_fp_h, color);
        // A command moves the VDP's own address counter, so the next span
        // owes a fresh set-up whatever column it starts at.
        g_fp_busy = 1;
        g_fp_atok = 0;
        return;
    }
    g_fp[FP_N] = g_fp_w;
    g_fp_color = color;
    if(!g_fp_atok)
        Msx2_FloorPoke();
    Msx2_FloorSolidRun();
    g_fp_atok = 1;
}

// Every black pixel of the pose, in a dozen rectangles instead of two hundred
// per-row spans.  They may overlap the arena: the spans are painted after.
// Nothing may be left running when the caller goes on to its own drawing.
static void Msx2_FloorFinish(void)
{
    if(g_fp_busy)
    {
        VDP_CommandWait();
        g_fp_busy = 0;
    }
}

static void Msx2_FloorBackdrop(void)
{
    const u8* r = g_floor_rects;
    u8 n = *r++;

    while(n--)
    {
        Msx2_Fill(r[0], r[1], r[2] ? r[2] : 256, r[3], MSX2_BLACK);
        r += 4;
    }
    g_fp_busy = 1;
}

// ── The walk ─────────────────────────────────────────────────────────────────

// The row's VRAM write address, column g_fp_x.  Inline rather than
// Msx2_PokeAtLine because the row part is constant and the command-engine wait
// is only owed when something actually left a command running.
static void Msx2_FloorPoke(void) __naked
{
    __asm
        ld a, (_g_fp_busy)
        or a
        jr z, 00951$
        xor a
        ld (_g_fp_busy), a
        call _VDP_CommandWait
    00951$:
        di
        ld a, (_g_fp_r14)
        out (#0x99), a
        ld a, #(14 | 0x80)
        out (#0x99), a
        ld a, (_g_fp_x)
        out (#0x99), a
        ld a, (_g_fp_hi)
        out (#0x99), a
        ei
        ret
    __endasm;
}

// ── THE ONLY BLACK A DELTA BAND PAINTS ──────────────────────────────────────
//
// No row of any pose has black inside its arena -- the generator would have to
// say so, and none does -- so what has to go back to black is exactly the two
// ends of the old silhouette (g_fp_ol/or) that the new one (g_fp_nl/nr) no
// longer covers.  Two spans a row instead of two hundred records, and during a
// camera move both are a few pixels wide.  A doubled row calls this a second
// time, with its own old silhouette and the drawn row's new one.
static void Msx2_FloorSlivers(void) __naked
{
    __asm
        // Slivers are not contiguous with the last textured span.  Even a
        // short CPU fill must set its own VRAM address.
        xor a
        ld (_g_fp_atok), a
        ld a, (_g_fp_ol)
        ld c, a
        ld a, (_g_fp_or)
        ld b, a
        cp c
        ret c                   // the old silhouette was empty
        xor a
        ld (_g_fp_mat), a
        ld a, (_g_fp_nl)
        ld d, a
        ld a, (_g_fp_nr)
        ld e, a
        ld a, d
        cp e
        jr z, 00936$
        jr c, 00936$
        ld a, c                 // nothing here now: black the whole of it
        ld (_g_fp_x), a
        ld a, b
        sub c
        inc a
        ld (_g_fp_w), a
        jp _Msx2_FloorFlatSpan
    00936$:
        ld a, c                 // left sliver: [ol, nl)
        cp d
        jr nc, 00938$
        ld (_g_fp_x), a
        ld a, d
        sub c
        ld (_g_fp_w), a
        push bc
        push de
        call _Msx2_FloorFlatSpan
        pop de
        pop bc
        xor a
        ld (_g_fp_mat), a
    00938$:
        xor a
        ld (_g_fp_atok), a
        ld a, e                 // right sliver: (nr, or]
        cp b
        ret nc
        inc a
        ld (_g_fp_x), a
        ld c, a
        ld a, b
        sub c
        inc a
        ld (_g_fp_w), a
        jp _Msx2_FloorFlatSpan
    __endasm;
}

// One row of records.  B is the record's end column (0 meaning 256), C the
// current one, HL the cursor; all three are reloaded around anything this
// calls, and the cursor lives in memory for that reason.
//
// BLACK IS THE INTERESTING CASE.  On a full band it is not painted at all --
// Msx2_FloorBackdrop already owns every black pixel.  On a delta band it is
// clipped to what this page's arena covered last time (`g_fp_ol/or`): black
// outside that is black already, and during a camera move the two silhouettes
// overlap so nearly that what is left is a sliver a few pixels wide.  Either
// way the row's own silhouette is recorded on the way past, for the next draw
// of this page to clip against.
static void Msx2_FloorRow(void) __naked
{
    __asm
        ld hl, (_g_fp_p)
        ld c, #0
    00930$:
        ld a, (hl)              // end column, 0 meaning 256
        inc hl
        ld b, a
        ld a, (hl)              // material
        inc hl
        cp #3
        jp nc, 00940$
        or a
        jr nz, 00932$           // black is never painted per span: see below
        xor a                   // and it leaves the port somewhere else
        ld (_g_fp_atok), a
        jr 00935$
    00932$:
        ld (_g_fp_mat), a
        call 00960$             // a wall is arena: widen the silhouette
        ld a, c
        ld (_g_fp_x), a
        ld a, b
        sub c
        ld (_g_fp_w), a
        ld (_g_fp_p), hl
        push bc
        call _Msx2_FloorFlatSpan
        pop bc
        ld hl, (_g_fp_p)
    00935$:
        ld c, b
        ld a, b
        or a
        jp nz, 00930$
        ret

    00940$:                     // textured: the record is the sampler's state
        ld (_g_fp_mat), a
        call 00960$
        ld a, (hl)              // texture page, relative to the resident base
        inc hl
        ld e, a
        ld a, (_g_floor_page)
        add a, e
        ld (_g_fp + FP_PTR + 1), a
        ld a, c
        ld (_g_fp_x), a
        ld a, b
        sub c
        ld (_g_fp + FP_N), a
        ld a, b
        ld (_g_fp_end), a
        ld (_g_fp_rec), hl      // the sampler pops its state straight off it
        ld de, #4
        ld a, (_g_fp_mat)
        cp #5
        jr c, 00941$
        ld e, #8
    00941$:
        ld a, e
        ld (_g_fp_wide), a
        add hl, de
        ld (_g_fp_p), hl
        ld a, (_g_fp_atok)      // spans are contiguous: the port is usually
        or a                    // already exactly where this one starts
        jr nz, 00943$
        call _Msx2_FloorPoke
    00943$:
        ld a, #1
        ld (_g_fp_atok), a
        ld a, (_g_fp_wide)
        cp #8
        jr z, 00944$
        call _Msx2_FloorFast
        jr 00945$
    00944$:
        ld a, (_g_fp + FP_N)
        and #1
        ld (_g_fp + FP_ODD), a
        call _Msx2_FloorSlow
    00945$:
        ld hl, (_g_fp_p)
        ld a, (_g_fp_end)
        ld b, a
        ld c, a
        or a
        jp nz, 00930$
        ret

    00960$:                     // the row's silhouette, widened by [c, b)
        ld a, (_g_fp_nl)
        inc a
        jr nz, 00961$           // 255: nothing yet, so this span is the left edge
        ld a, c
        ld (_g_fp_nl), a
    00961$:
        ld a, b
        dec a
        ld (_g_fp_nr), a
        ret
    __endasm;
}

// The row's records, straight out of the cartridge window.  A whole pose lives
// in one segment, so no offset can straddle one.
static void Msx2_FloorFetch(void) __naked
{
    __asm
        .globl _g_bank2
        di
        ld hl, (_g_fp_seg)
        ld (#MSX2_FLOOR_BANK_REG), hl
        ld hl, (_g_fp_src)
        ld de, #_g_floor_row
        ld a, (_g_fp_len)
        ld c, a
        ld b, #0
        ldir
        ld hl, #MSX2_FLOOR_CODE_SEG
        ld (#MSX2_FLOOR_BANK_REG), hl
        ld (_g_bank2), hl       // keep the ISR's shadow coherent
        ei
        ret
    __endasm;
}

static void Msx2_FloorBand(u8 allow_delta, u8 stride);

// One row of the arena copied onto the next by the command engine.  A camera
// move draws every second row and has the VDP fill in the other: the copy is
// about four microseconds a pixel against thirteen for a CPU-sampled one, and
// it halves the record walk, the row fetch and the span set-up as well, which
// together are more of a moving frame than the texels are.  The resting pose
// at the end of a move is redrawn line by line, so nothing the player sits and
// looks at is doubled.
// ONE command, however many rows this one stands for.  A copy one row down
// overlaps itself, and the engine walks a rectangle top to bottom: the second
// output row reads back the first one it just wrote, so the drawn row
// propagates down the whole group.
//
// The command block is filled here rather than through VDP_CommandHMMM
// because that inline is twelve C stores of sixteen-bit fields, and at thirty
// eight copies a frame it cost more than the copies did.
static void Msx2_FloorDouble(void) __naked
{
    __asm
        ld a, (_g_fp_nl)
        ld l, a
        ld h, #0
        ld (#_g_VDP_Command + 0), hl    // SX
        ld (#_g_VDP_Command + 4), hl    // DX
        ld a, (_g_bd_page)
        ld h, a
        ld a, (_g_bd_y)
        ld l, a
        ld (#_g_VDP_Command + 2), hl    // SY
        inc hl
        ld (#_g_VDP_Command + 6), hl    // DY, one row down
        ld a, (_g_fp_nl)
        ld b, a
        ld a, (_g_fp_nr)
        sub b
        inc a
        ld l, a
        ld h, #0
        jr nz, 00965$
        inc h                         // full-width silhouette: NX = 256
    00965$:
        ld (#_g_VDP_Command + 8), hl    // NX
        ld a, (_g_bd_extra)
        ld l, a
        ld h, #0
        ld (#_g_VDP_Command + 10), hl   // NY
        xor a
        ld (#_g_VDP_Command + 13), a    // ARG
        ld a, #0xD0                     // VDP_CMD_HMMM
        ld (#_g_VDP_Command + 14), a
        ld a, #1
        ld (_g_fp_busy), a
        ld (_g_fp_atok), a
        dec a
        ld (_g_fp_atok), a
        jp _VDP_CommandSetupR32         // which waits for the engine itself
    __endasm;
}

static void Msx2_FloorBandRows(void) __naked
{
    __asm
    00970$:
        ld a, (_g_bd_stride_1)  // is any of this group inside the arena?
        ld b, a
        ld a, (_g_bd_y)
        add a, b
        ld b, a                 // the group's last row
        ld a, (_g_bd_y0)
        cp b
        jr z, 00968$
        jr nc, 00969$           // the whole group is above the arena
    00968$:
        ld a, (_g_bd_y1)
        ld b, a
        ld a, (_g_bd_y)
        cp b
        jr c, 00967$            // below it, then, unless this row is inside
    00969$:                     // no: only its black is owed
        ld a, #255
        ld (_g_fp_nl), a
        xor a
        ld (_g_fp_nr), a
        ld a, (_g_bd_delta)
        or a
        jr z, 00966$
        ld a, (_g_bd_stride)
        ld (_g_fp_h), a
        ld a, (_g_bd_y)
        call 00980$
        call 00981$
        call _Msx2_FloorSlivers
    00966$:
        call 00982$
        jp 00975$
    00967$:
        ld a, #1
        ld (_g_fp_h), a
        ld hl, (_g_bd_at)       // the row's records, exactly as many as it has
        ld e, (hl)
        inc hl
        ld d, (hl)
        inc hl
        ld a, (hl)
        sub e
        ld (_g_fp_len), a
        ld a, d
        or #>MSX2_FLOOR_WINDOW
        ld d, a
        ld (_g_fp_src), de
        ld hl, #_g_floor_row
        ld (_g_fp_p), hl
        call _Msx2_FloorFetch
        ld a, (_g_bd_y)
        call 00980$
        ld a, #255
        ld (_g_fp_nl), a
        xor a
        ld (_g_fp_nr), a
        ld (_g_fp_atok), a
        call _Msx2_FloorRow
        ld a, (_g_bd_stride)
        ld (_g_fp_h), a         // the group's black goes down in one command
        ld a, (_g_bd_delta)
        or a
        jr z, 00971$
        call 00981$
        call _Msx2_FloorSlivers
    00971$:
        call 00982$             // this group's silhouette, for the next draw
        ld a, (_g_bd_stride)
        dec a
        jr z, 00975$
        ld (_g_bd_extra), a
        ld a, (_g_fp_nl)        // then the command engine repeats the row
        ld c, a
        ld a, (_g_fp_nr)
        cp c
        jr c, 00975$
        call _Msx2_FloorDouble
    00975$:
        ld a, (_g_bd_stride)
        ld c, a
        ld b, #0
        ld hl, (_g_bd_el)
        add hl, bc
        ld (_g_bd_el), hl
        ld hl, (_g_bd_er)
        add hl, bc
        ld (_g_bd_er), hl
        ld hl, (_g_bd_at)
        add hl, bc
        add hl, bc
        ld (_g_bd_at), hl
        ld a, (_g_bd_y)
        add a, c
        ld (_g_bd_y), a
        ld b, a                 // past the split the rows treble instead
        ld a, (_g_bd_split)
        dec a
        cp b
        jr nc, 00979$
        ld a, #3
        ld (_g_bd_stride), a
        dec a
        ld (_g_bd_stride_1), a
    00979$:
        ld a, (_g_bd_rows)
        sub c
        ld (_g_bd_rows), a
        jp nz, 00970$
        ret

    00980$:                     // GRAPHIC 7 write address for line A
        ld (_g_fp_y), a
        ld e, a
        and #0x3F
        or #0x40
        ld (_g_fp_hi), a
        ld a, e
        and #0x40
        rlca
        rlca
        ld e, a
        ld a, (_g_bd_r14)
        or e
        ld (_g_fp_r14), a
        ret

    00981$:                     // what this group of rows held last time: the
        ld a, (_g_bd_stride)    // widest of them, since one span now blacks all
        ld b, a
        ld hl, (_g_bd_el)
        ld a, #255
    00983$:
        cp (hl)
        jr c, 00984$
        ld a, (hl)
    00984$:
        inc hl
        djnz 00983$
        ld (_g_fp_ol), a
        ld a, (_g_bd_stride)
        ld b, a
        ld hl, (_g_bd_er)
        xor a
    00985$:
        cp (hl)
        jr nc, 00986$
        ld a, (hl)
    00986$:
        inc hl
        djnz 00985$
        ld (_g_fp_or), a
        ret

    00982$:                     // every row of the group now holds the same
        ld a, (_g_bd_stride)
        ld b, a
        ld hl, (_g_bd_el)
        ld a, (_g_fp_nl)
    00987$:
        ld (hl), a
        inc hl
        djnz 00987$
        ld a, (_g_bd_stride)
        ld b, a
        ld hl, (_g_bd_er)
        ld a, (_g_fp_nr)
    00988$:
        ld (hl), a
        inc hl
        djnz 00988$
        ret
    __endasm;
}

static void Msx2_FloorBand(u8 allow_delta, u8 stride)
{
    u8 page = Msx2_VideoGetDrawPage();
    u8 delta = (u8)(allow_delta && g_ext_ok[page]);

    Msx2_FloorLoad();
    if(!delta)
    {
        // The two frames that open a move cannot trust the page and pay for a
        // whole backdrop; they are also the fastest part of the motion, so
        // they take the coarsest rows whatever the pose asked for.
        if(allow_delta)
        {
            stride = 3;
            // The baked backdrop follows the exact rows, not the repeated
            // silhouette.  Clear the initial page so narrower repeated rows
            // cannot leave pixels from the previous pose outside the arena.
            Msx2_Fill(0, MSX2_BAND_Y, MSX2_SCREEN_W, MSX2_BAND_H, MSX2_BLACK);
            g_fp_busy = 1;
        }
        else
            Msx2_FloorBackdrop();
    }
    g_fp_seg = (u16)(MSX2_FLOOR_SPAN_SEG + g_floor_segments[g_msx2_arena_pose]);
    g_bd_at = (u16)g_floor_rows;
    g_bd_el = (u16)&g_ext_l[page][0];
    g_bd_er = (u16)&g_ext_r[page][0];
    g_bd_y = MSX2_BAND_Y;
    g_bd_rows = MSX2_BAND_H;
    g_bd_stride = stride;
    g_bd_delta = delta;
    g_bd_r14 = (u8)(page << 2);
    g_bd_page = page;
    g_bd_y0 = (u8)(MSX2_BAND_Y + g_floor_row0);
    g_bd_y1 = (u8)(MSX2_BAND_Y + g_floor_row1);
    g_bd_stride_1 = (u8)(stride - 1);
    g_bd_split = (stride == 2) ? (u8)(MSX2_BAND_Y + g_floor_split) : 255;
    // The row a group of `stride` leads on is fixed to the band, not to the
    // pose, so the group straddling the arena's top row leads above it and
    // draws nothing at all -- the board starts one whole group down.  Cards
    // clip to that, or they hang over the rim and are never erased again; see
    // msx2_arena.h.
    g_msx2_arena_top = (u8)(MSX2_BAND_Y +
        (u8)((u8)((g_floor_row0 + stride - 1) / stride) * stride));
    Msx2_FloorBandRows();
    g_ext_ok[page] = 1;
    Msx2_FloorFinish();
}

// A clipped repair starts a span part way along it, and the sampler's state
// has to be walked forward to there.  In C that was two signed multiplies and
// a couple of hundred bytes of the duel bank, which it does not have.
static void Msx2_FloorSkipAdvance(void) __naked
{
    __asm
        ld hl, (_g_fp_rec)      // the record, into the sampler's own bytes
        ld de, #_g_fp
        ld a, (_g_fp_wide)
        ld c, a
        ld b, #0
        ldir
        ld hl, #_g_fp
        ld (_g_fp_rec), hl
        ld a, (_g_fp_skip)
        or a
        ret z
        ld a, (_g_fp_wide)
        cp #8
        jr z, 00991$
        ld de, (_g_fp + FP_DU)
        call 00994$
        ld de, (_g_fp + FP_ACC)
        add hl, de
        ld (_g_fp + FP_ACC), hl
        ret
    00991$:                     // the moving-v steps are baked doubled
        ld hl, (_g_fp + FP_SDU)
        call 00993$
        ex de, hl
        call 00994$
        ld de, (_g_fp + FP_SACC)
        add hl, de
        ld (_g_fp + FP_SACC), hl
        ld hl, (_g_fp + FP_SDV)
        call 00993$
        ex de, hl
        call 00994$
        ld de, (_g_fp + FP_SVACC)
        add hl, de
        ld (_g_fp + FP_SVACC), hl
        ret
    00993$:
        sra h
        rr l
        ret
    00994$:                     // hl = de * g_fp_skip
        ld a, (_g_fp_skip)
        ld hl, #0
        ld b, #8
    00995$:
        add hl, hl
        rlca
        jr nc, 00996$
        add hl, de
    00996$:
        djnz 00995$
        ret
    __endasm;
}

// The clipped repair of one rectangle -- an emptied slot, a card that moved.
// It paints its own black (no backdrop ran, and no silhouette is recorded for
// it) and walks a clipped span's sampler state forward, which the band never
// has to.  Every coordinate here is a column, 0..255 inclusive: a repair is
// never the full width, and eight-bit arithmetic is a third of the bank space
// that the same code in sixteen bits was.
static void Msx2_FloorDraw(u8 x0, u8 y0, u8 x1, u8 y1)
{
    u8 y;
    u16 segment = MSX2_FLOOR_SPAN_SEG + g_floor_segments[g_msx2_arena_pose];

    Msx2_FloorLoad();
    // A previous camera step can leave a two/three-row flat-fill height.
    // Rectangle repairs walk every row and must never inherit that height.
    g_fp_h = 1;
    if(y0 < MSX2_BAND_Y) y0 = MSX2_BAND_Y;
    if(y1 > MSX2_BAND_Y + MSX2_BAND_H) y1 = MSX2_BAND_Y + MSX2_BAND_H;
    for(y = y0; y < y1; ++y)
    {
        const u8* p = g_floor_row;
        u8 row = (u8)(y - MSX2_BAND_Y);
        u16 at = g_floor_rows[row];
        u8 x = 0;

        Msx2_RomRead(segment, at, g_floor_row,
                     (u8)(g_floor_rows[row + 1] - at));
        Msx2_FloorRowAddr(y);
        g_fp_atok = 0;
        for(;;)
        {
            u8 end = *p++;
            u8 mat = *p++;
            u8 last = (u8)(end - 1);       // end 0 is column 255
            u8 left = x < x0 ? x0 : x;
            u8 right = last > x1 ? x1 : last;
            u8 wide = (u8)(mat >= 5 ? 8 : 4);

            if(mat >= 3)
            {
                if(left <= right)
                {
                    g_fp_wide = wide;
                    g_fp_skip = (u8)(left - x);
                    g_fp_rec = (u16)(p + 1);
                    Msx2_FloorSkipAdvance();
                    g_fp[FP_PTR + 1] = (u8)(g_floor_page + p[0]);
                    g_fp[FP_N] = (u8)(right - left + 1);
                    g_fp[FP_ODD] = (u8)((right - left + 1) & 1);
                    g_fp_x = left;
                    Msx2_FloorPoke();
                    if(wide == 8)
                        Msx2_FloorSlow();
                    else
                        Msx2_FloorFast();
                    g_fp_atok = 0;
                }
                p += wide + 1;
            }
            else if(left <= right)
            {
                g_fp_mat = mat;
                g_fp_x = left;
                g_fp_w = (u8)(right - left + 1);
                Msx2_FloorFlatSpan();
            }
            x = end;
            if(!end)
                break;
        }
    }
    Msx2_FloorFinish();
}
