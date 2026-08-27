#include "libfmt.h"
#include "palette.h"
#include "io.h"
#if defined(FMTOWNS_MEASURE_DIRTY_BAR)
#include "fmt_pixel.h"
#endif
#include "defs.h"

/*
 * FM TOWNS VRAM0 physical base address. g_fmt_vram0_base (machine.h) holds
 * TOWNSEMU's TOWNSADDR_386SX_VRAM0_BASE (0xA00000) on the narrow 386SX-based
 * UX/Marty memory map, or TOWNSADDR_VRAM0_BASE (0x80000000) on the "full"
 * 386DX/486/Pentium map -- whichever fmt_machine_detect() found this
 * machine to be.
 */
#define TOWNS_VRAM_SIZE           0x80000u

static const fmt_mode_t g_modes[FMT_NUM_MODES] = {
    /*
     * Bubble Bobble-derived 31kHz timing with the normal 2x CRTC zoom
     * (ZOOM=0x1111).  This intentionally keeps the source at 512x480 rather
     * than applying the VING family's 2.5x horizontal expansion.
     *
     * With HST=0x31f, Tsugaru's standard horizontal origin is 138.  Setting
     * HDS=HAJ=0xCA gives an x origin of 64, centering the 512-dot viewport
     * (HDE=0x2CA) in the 640-pixel output.  FO must be zero: it tells the
     * CRTC that the 960-line register span is a 480-line display.  LO is the
     * source row length in eight-byte units: 0x40 for 256 RGB555 pixels.
     */
    [FMT_MODE_256x240_16BPP] = {
        .width = 256, .height = 240, .bpp = 16, .stride = 512,
        .crtc = {
            /* 00 HSW1 */ 0x0060, /* 01 HSW2 */ 0x02C0, /* 02 ---- */      0, /* 03 ---- */      0,
            /* 04 HST  */ 0x031F, /* 05 VST1 */ 0x0000, /* 06 VST2 */ 0x0004, /* 07 EET  */ 0x0000,
            /* 08 VST  */ 0x0419, /* 09 HDS0 */ 0x00CA, /* 0A HDE0 */ 0x02CA, /* 0B HDS1 */ 0x00CA,
            /* 0C HDE1 */ 0x02CA, /* 0D VDS0 */ 0x0046, /* 0E VDE0 */ 0x0406, /* 0F VDS1 */ 0x0046,
            /* 10 VDE1 */ 0x0406, /* 11 FA0  */ 0x0000, /* 12 HAJ0 */ 0x00CA, /* 13 FO0  */ 0x0000,
            /* 14 LO0  */ 0x0040, /* 15 FA1  */ 0x0000, /* 16 HAJ1 */ 0x00CA, /* 17 FO1  */ 0x0000,
            /* 18 LO1  */ 0x0040, /* 19 EHAJ */ 0x0058, /* 1A EVAJ */ 0x0001, /* 1B ZOOM */ 0x1111,
            /* 1C CR0  */ 0x0002, /* 1D CR1  */ 0x0002, /* 1E FR   */ 0x0000, /* 1F CR2  */ 0x0192
        },
        .video = { 0x0A, 0x18 },
    },
    [FMT_MODE_256x240_8BPP] = {
        .width = 256, .height = 240, .bpp = 8, .stride = 256,
        .crtc = {
            /* 00 HSW1 */ 0x0060, /* 01 HSW2 */ 0x02C0, /* 02 ---- */      0, /* 03 ---- */      0,
            /* 04 HST  */ 0x031F, /* 05 VST1 */ 0x0000, /* 06 VST2 */ 0x0004, /* 07 EET  */ 0x0000,
            /* 08 VST  */ 0x0419, /* 09 HDS0 */ 0x00CA, /* 0A HDE0 */ 0x02CA, /* 0B HDS1 */ 0x00CA,
            /* 0C HDE1 */ 0x02CA, /* 0D VDS0 */ 0x0046, /* 0E VDE0 */ 0x0406, /* 0F VDS1 */ 0x0046,
            /* 10 VDE1 */ 0x0406, /* 11 FA0  */ 0x0000, /* 12 HAJ0 */ 0x00CA, /* 13 FO0  */ 0x0000,
            /* 14 LO0  */ 0x0020, /* 15 FA1  */ 0x0000, /* 16 HAJ1 */ 0x00CA, /* 17 FO1  */ 0x0000,
            /* 18 LO1  */ 0x0020, /* 19 EHAJ */ 0x0058, /* 1A EVAJ */ 0x0001, /* 1B ZOOM */ 0x1111,
            /* 1C CR0  */ 0x0003, /* 1D CR1  */ 0x0002, /* 1E FR   */ 0x0000, /* 1F CR2  */ 0x0192
        },
        .video = { 0x0A, 0x18 },
    },
    /*
     * 31kHz single-page 8bpp, displaying a 320x240 source at 2x zoom as
     * 640x480.  The timing and display spans match the verified 640x480
     * register family below; LO0 remains the unzoomed 320-byte source
     * stride (LO0*8 in single-page mode).  At 31kHz ZOOM=0x0011 is the
     * documented ZH0=2, ZV0=2 setting without the 15kHz special case.
     */
    [FMT_MODE_320x240_8BPP] = {
        .width = 320, .height = 240, .bpp = 8, .stride = 320,
        .crtc = {
            /* 00 HSW1 */ 0x003C, /* 01 HSW2 */ 0x02AA, /* 02 ---- */      0, /* 03 ---- */      0,
            /* 04 HST  */ 0x0320, /* 05 VST1 */ 0x0005, /* 06 VST2 */ 0x000B, /* 07 EET  */ 0x0015,
            /* 08 VST  */ 0x020D, /* 09 HDS0 */ 0x0050, /* 0A HDE0 */ 0x02D0, /* 0B HDS1 */ 0x0050,
            /* 0C HDE1 */ 0x02D0, /* 0D VDS0 */ 0x0010, /* 0E VDE0 */ 0x01F0, /* 0F VDS1 */ 0x0010,
            /* 10 VDE1 */ 0x01F0, /* 11 FA0  */ 0x0000, /* 12 HAJ0 */ 0x0050, /* 13 FO0  */ 0x0001,
            /* 14 LO0  */ 0x0028, /* 15 FA1  */ 0x0000, /* 16 HAJ1 */ 0x0050, /* 17 FO1  */ 0x0001,
            /* 18 LO1  */ 0x0028, /* 19 EHAJ */ 0x0056, /* 1A EVAJ */ 0x0007, /* 1B ZOOM */ 0x0011,
            /* 1C CR0  */ 0x002B, /* 1D CR1  */ 0x0002, /* 1E FR   */ 0x0002, /* 1F CR2  */ 0x0188
        },
        .video = { 0x0A, 0x18 },
    },
    /*
     * Separate native 15kHz 320x240 geometry in 16bpp: LO0 doubles
     * (2 bytes/pixel) and
     * CR0 bits[1:0] switch from 3 (8bpp) to 2 (16bpp) per
     * TownsCRTC::GetPageBitsPerPixel. 16bpp pixels are RGB555 - build
     * them with common.h's rgb15() macro.
     */
    [FMT_MODE_320x240_16BPP] = {
        .width = 320, .height = 240, .bpp = 16, .stride = 640,
        .crtc = {
            /* 00 HSW1 */ 0x0074, /* 01 HSW2 */ 0x0530, /* 02 ---- */      0, /* 03 ---- */      0,
            /* 04 HST  */ 0x0617, /* 05 VST1 */ 0x000C, /* 06 VST2 */ 0x0018, /* 07 EET  */ 0x0030,
            /* 08 VST  */ 0x049A, /* 09 HDS0 */ 0x00E7, /* 0A HDE0 */ 0x0367, /* 0B HDS1 */ 0x00E7,
            /* 0C HDE1 */ 0x0367, /* 0D VDS0 */ 0x0046, /* 0E VDE0 */ 0x0136, /* 0F VDS1 */ 0x0046,
            /* 10 VDE1 */ 0x0136, /* 11 FA0  */ 0x0000, /* 12 HAJ0 */ 0x00E7, /* 13 FO0  */ 0x0001,
            /* 14 LO0  */ 0x0050, /* 15 FA1  */ 0x0000, /* 16 HAJ1 */ 0x00E7, /* 17 FO1  */ 0x0001,
            /* 18 LO1  */ 0x0050, /* 19 EHAJ */ 0x0056, /* 1A EVAJ */ 0x0007, /* 1B ZOOM */ 0x0001,
            /* 1C CR0  */ 0x002A, /* 1D CR1  */ 0x0001, /* 1E FR   */ 0x0002, /* 1F CR2  */ 0x0188
        },
        .video = { 0x0A, 0x18 },
    },
    /*
     * 31kHz, 2-page/linear 16bpp. CLKSEL=2 (25175000Hz) with HST=800
     * gives GetHorizontalFrequency()==31 exactly (25175000/800/1000).
     * At 31kHz, GetPageSizeOnMonitor() maps monitor size directly to
     * HDE0-HDS0/VDE0-VDS0 (no 15kHz-only /2,*2 fixups), so HDE0-HDS0=640,
     * VDE0-VDS0=400 give exactly 640x400. video[0] bit 0x10 selects
     * 2-page/linear addressing (LowResCrtcIsInSinglePageMode()==false),
     * which makes bytesPerLine=LO0*4 (not *8 like single-page) and CR0
     * CL=1 mean 16bpp (not CL=2 like single-page) - see
     * TownsCRTC::GetPageBitsPerPixel/GetPageBytesPerLine. FO0 is set
     * to a value distinct from LO0 and nonzero to avoid an unrelated
     * VRAM-coverage halving quirk (GetPageVRAMCoverageSize1X's
     * "FO==0 || FO==LO" check - see the FO0/LO0 comment on the
     * 256x240 mode in the .c file's git history).
     */
    [FMT_MODE_640x400_16BPP_LINEAR] = {
        .width = 640, .height = 400, .bpp = 16, .stride = 1280, .linear = 1,
        .crtc = {
            /* 00 HSW1 */ 0x003C, /* 01 HSW2 */ 0x02AA, /* 02 ---- */      0, /* 03 ---- */      0,
            /* 04 HST  */ 0x0320, /* 05 VST1 */ 0x0005, /* 06 VST2 */ 0x0009, /* 07 EET  */ 0x0012,
            /* 08 VST  */ 0x01C1, /* 09 HDS0 */ 0x0050, /* 0A HDE0 */ 0x02D0, /* 0B HDS1 */ 0x0050,
            /* 0C HDE1 */ 0x02D0, /* 0D VDS0 */ 0x0020, /* 0E VDE0 */ 0x01B0, /* 0F VDS1 */ 0x0020,
            /* 10 VDE1 */ 0x01B0, /* 11 FA0  */ 0x0000, /* 12 HAJ0 */ 0x0050, /* 13 FO0  */ 0x0001,
            /* 14 LO0  */ 0x0140, /* 15 FA1  */ 0x0000, /* 16 HAJ1 */ 0x0050, /* 17 FO1  */ 0x0001,
            /* 18 LO1  */ 0x0140, /* 19 EHAJ */ 0x0056, /* 1A EVAJ */ 0x0007, /* 1B ZOOM */ 0x0000,
            /* 1C CR0  */ 0x0001, /* 1D CR1  */ 0x0002, /* 1E FR   */ 0x0002, /* 1F CR2  */ 0x0188
        },
        .video = { 0x1B, 0x18 },
    },
    /*
     * Same 31kHz timing/derivation as the 640x400 mode above, sized
     * for 512x480 instead: HDE0-HDS0=512, VDE0-VDS0=480, VST widened
     * to fit (525, matching the vertical total of standard VGA
     * 640x480@60Hz at the same 25.175MHz/800 horizontal timing).
     */
    [FMT_MODE_512x480_16BPP_LINEAR] = {
        .width = 512, .height = 480, .bpp = 16, .stride = 1024, .linear = 1,
        .crtc = {
            /* 00 HSW1 */ 0x003C, /* 01 HSW2 */ 0x02AA, /* 02 ---- */      0, /* 03 ---- */      0,
            /* 04 HST  */ 0x0320, /* 05 VST1 */ 0x0005, /* 06 VST2 */ 0x000B, /* 07 EET  */ 0x0015,
            /* 08 VST  */ 0x020D, /* 09 HDS0 */ 0x0060, /* 0A HDE0 */ 0x0260, /* 0B HDS1 */ 0x0060,
            /* 0C HDE1 */ 0x0260, /* 0D VDS0 */ 0x0010, /* 0E VDE0 */ 0x01F0, /* 0F VDS1 */ 0x0010,
            /* 10 VDE1 */ 0x01F0, /* 11 FA0  */ 0x0000, /* 12 HAJ0 */ 0x0060, /* 13 FO0  */ 0x0001,
            /* 14 LO0  */ 0x0100, /* 15 FA1  */ 0x0000, /* 16 HAJ1 */ 0x0060, /* 17 FO1  */ 0x0001,
            /* 18 LO1  */ 0x0100, /* 19 EHAJ */ 0x0056, /* 1A EVAJ */ 0x0007, /* 1B ZOOM */ 0x0000,
            /* 1C CR0  */ 0x0001, /* 1D CR1  */ 0x0002, /* 1E FR   */ 0x0002, /* 1F CR2  */ 0x0188
        },
        .video = { 0x1B, 0x18 },
    },
    /*
     * Same 31kHz horizontal timing/width as the 640x400 mode
     * (HSW/HST/HDS0-HDE0/HAJ0/LO0 identical - both are 640 columns
     * wide), with the vertical block borrowed from the 512x480 mode
     * instead (VST1/VST2/EET/VST/VDS0-VDE0) to get 480 rows: same
     * 525-line vertical total as standard VGA 640x480@60Hz at this
     * 25.175MHz/800 horizontal timing. "15bpp" here means RGB555
     * packed into a 16-bit VRAM word, same as every other 16bpp mode
     * in this table - see common.h's rgb15().
     */
    [FMT_MODE_640x480_16BPP_LINEAR] = {
        .width = 640, .height = 480, .bpp = 16, .stride = 1280, .linear = 1,
        .crtc = {
            /* 00 HSW1 */ 0x003C, /* 01 HSW2 */ 0x02AA, /* 02 ---- */      0, /* 03 ---- */      0,
            /* 04 HST  */ 0x0320, /* 05 VST1 */ 0x0005, /* 06 VST2 */ 0x000B, /* 07 EET  */ 0x0015,
            /* 08 VST  */ 0x020D, /* 09 HDS0 */ 0x0050, /* 0A HDE0 */ 0x02D0, /* 0B HDS1 */ 0x0050,
            /* 0C HDE1 */ 0x02D0, /* 0D VDS0 */ 0x0010, /* 0E VDE0 */ 0x01F0, /* 0F VDS1 */ 0x0010,
            /* 10 VDE1 */ 0x01F0, /* 11 FA0  */ 0x0000, /* 12 HAJ0 */ 0x0050, /* 13 FO0  */ 0x0001,
            /* 14 LO0  */ 0x0140, /* 15 FA1  */ 0x0000, /* 16 HAJ1 */ 0x0050, /* 17 FO1  */ 0x0001,
            /* 18 LO1  */ 0x0140, /* 19 EHAJ */ 0x0056, /* 1A EVAJ */ 0x0007, /* 1B ZOOM */ 0x0000,
            /* 1C CR0  */ 0x0001, /* 1D CR1  */ 0x0002, /* 1E FR   */ 0x0002, /* 1F CR2  */ 0x0188
        },
        .video = { 0x1B, 0x18 },
    },
};

static const fmt_mode_t *g_cur = 0;
uint32_t g_fmt_draw_buffer_offset = 0;
static uint32_t g_frame_buffer_size = 0;
static uint8_t g_display_page = 0;
static uint8_t g_draw_page = 0;
static uint8_t g_can_flip = 0;

static inline uint32_t vram_singlepage_trans(uint32_t off)
{
    return ((off & 4u) << 16) | ((off & 0x7fff8u) >> 1) | (off & 3u);
}

void fmt_set_mode(fmt_mode_id_t id)
{
    const fmt_mode_t *m = &g_modes[id];

    /* Make sure GVRAM (not main RAM) is mapped at the VRAM window. */
    outb(0, IO_FMR_VRAM_OR_MAINRAM);

    stop_display();
    set_crtc(m->crtc);
    set_video(m->video);

    fmt_invalidate_dirty_present();
    g_frame_buffer_size = (uint32_t)m->stride * m->height;
    g_can_flip = (!m->linear && g_frame_buffer_size * 2u <= TOWNS_VRAM_SIZE);
    g_display_page = 0;
    g_draw_page = g_can_flip ? 1 : 0;
    g_fmt_draw_buffer_offset = (uint32_t)g_draw_page * g_frame_buffer_size;

    /* FA0 is expressed in groups of eight bytes in all supported
     * single-page 8/16bpp modes.  Begin on the first, cleared page.
     * FA1 must track it - see fmt_flip_page_poll(). */
    crtc_out16(FA0, 0);
    crtc_out16(FA1, 0);

    /* A mode change must never expose stale VRAM.  Clear all buffers that
     * belong to the mode while display output is stopped. */
    volatile uint8_t *vram = (volatile uint8_t *)g_fmt_vram0_base;
    uint32_t clear_size = g_can_flip ? g_frame_buffer_size * 2u
                                     : g_frame_buffer_size;
    if (clear_size > TOWNS_VRAM_SIZE) {
        clear_size = TOWNS_VRAM_SIZE;
    }
    for (uint32_t off = 0; off < clear_size; ++off) {
        vram[m->linear ? off : vram_singlepage_trans(off)] = 0;
    }
    start_display();

    g_cur = m;
}

const fmt_mode_t *fmt_current_mode(void)
{
    return g_cur;
}

/* `rgb888` is always the base of the WHOLE palette; `first` selects where in
 * it to start.  Uploading a sub-range matters because palette RAM may only be
 * written during vertical blanking - the CRTC reads it on every displayed
 * pixel and the ports latch immediately, so a write during active display
 * changes colours mid-frame - and the blanking window only fits so many port
 * writes.  Sending just the entries that actually changed is what keeps a
 * 256-entry fade inside it. */
void fmt_load_palette_range(const uint8_t *rgb888, int first, int count)
{
    for (int i = first; i < first + count; i++) {
        uint8_t r = rgb888[i * 3];
        uint8_t g = rgb888[i * 3 + 1];
        uint8_t b = rgb888[i * 3 + 2];
        set_palette((uint8_t)i, r, g, b);
    }
}

void fmt_load_palette(const uint8_t *rgb888, int count)
{
    fmt_load_palette_range(rgb888, 0, count);
}

/*
 * Low-res single-page VRAM is NOT a simple linear/row-major byte array
 * on real FM TOWNS hardware (and TOWNSEMU faithfully reproduces this):
 * the renderer reads single-page mode through a VRAM1Trans byte-address
 * swizzle before reading VRAM, so a plain row-major write disagrees
 * with what gets displayed (see git history b2020fc for the original
 * diagnosis, from the 8bpp 256x240 case). Apply the same forward
 * transform on write, one VRAM byte at a time, regardless of bpp - the
 * transform operates on VRAM byte offsets, not pixel values.
 *
 * 2-page/linear mode (fmt_mode_t.linear) reads through VRAM0Trans
 * instead, which is a no-op - so those modes get a plain row-major
 * write, with no per-byte transform.
 */
/* ------------------------------------------------------------------
 * Dirty-region present.
 *
 * fmt_put_image() pushes all 61440 bytes into VRAM every frame, whether or
 * not a single pixel moved.  Under the 386SX bus model that is the single
 * most expensive thing this port does outside the rasterizer -- a VRAM bus
 * cycle costs several times a main-RAM one, and a 16-bit bus splits every
 * 32-bit store into two of them -- and most frames of this game change a
 * fraction of the screen: a story dialogue frame types one character, a
 * parked duel frame moves nothing at all.
 *
 * So: hash each aligned 32-byte group of the source, keep the hashes, and
 * store only the groups whose hash changed.
 *
 * A hash rather than a shadow copy of the framebuffer, for two reasons.  It
 * is 7.7 KB of .bss instead of 61 KB, on a machine whose whole .bss budget
 * is the 1 MB between 0x100000 and a 2 MB Marty's ceiling; and it is
 * *faster*, because the comparison then reads the framebuffer once instead
 * of reading both it and a shadow.  Reads are what this costs, so halving
 * them halves the price of finding out that nothing changed.
 *
 * The hash is a rotate-and-XOR fold, which is position-sensitive (a plain
 * XOR would not notice two dwords swapping places) and cheap enough on a
 * 386 to stay well under what the stores cost.  A collision would leave one
 * 32-byte block stale until the next time that block changed; at 2^-32 per
 * changed group that is a self-healing once-in-many-hours flicker, which is
 * the right trade against 53 KB of RAM this machine does not have.
 *
 * PAGE FLIPPING is why g_dirty_prev exists.  The two VRAM pages alternate,
 * so the page being written now was last written two frames ago and holds
 * frame N-2, not N-1.  Writing only what changed since N-1 would leave it
 * missing everything that changed between N-2 and N-1.  The union of the
 * last two frames' changed sets is a superset of "changed since N-2", so
 * each group is written if it changed this frame OR changed last frame.
 */
#define FMT_DIRTY_GROUP_DWORDS 16u                    /* 64 source bytes */
#define FMT_DIRTY_MAX_GROUPS   (256u * 240u / 64u)     /* the 8bpp game mode */

static uint32_t g_dirty_hash[FMT_DIRTY_MAX_GROUPS];
static uint8_t  g_dirty_prev[FMT_DIRTY_MAX_GROUPS];

/* WHAT THE CALLER DECLARED LAST FRAME.
 *
 * The hash above answers "did this group change?" but it has to read the
 * group to do it, and reading all 61440 bytes is ~13 ms on the 386SX model
 * -- the entire `present` cost of a screen where the game moved nothing.
 * So the game also reports which 64-pixel groups it wrote
 * (waifu_fm_frame_damage(), src/main.c) and the scan skips the rest.
 *
 * The scan set has to be the union of THIS frame's and LAST frame's
 * declarations, for the same page-flip reason g_dirty_prev exists: a group
 * written last frame still owes this frame's page a copy.  Everything
 * outside that union is unchanged in both pages, so not looking at it is
 * exactly as correct as looking and finding nothing. */
/* g_dirty_prev value meaning "written, but the stored hash was not updated
 * and no longer describes the group" -- see the force_mask path below.  It is
 * also non-zero, so it keeps the page-flip rule (write again next frame). */
#define FMT_DIRTY_NO_HASH 2u
#define FMT_DIRTY_MAX_ROWS 240u
static uint8_t g_dirty_prev_rows[FMT_DIRTY_MAX_ROWS];
/* Cleared by fmt_set_mode(): the hashes describe VRAM contents, and after a
 * mode set (which also clears both pages) they describe nothing. */
static int g_dirty_valid = 0;

/* One group's hash.  Every operation takes its operand straight out of the
 * framebuffer, so this is sixteen instructions for sixty-four bytes -- which
 * is the whole point.  An earlier version loaded into a register, rotated,
 * and XORed (three instructions per dword) and cost as much as the stores it
 * was there to avoid: 25.8 ms to hash a screen it then decided not to write
 * a single byte of, against 23 ms to blit the screen unconditionally.  On a
 * machine with no cache and a 2-3x clock penalty per instruction, the
 * instruction count of a comparison matters as much as the bus traffic.
 *
 * XOR and add alternate so the fold is order-sensitive: with one operation
 * throughout, two dwords swapping places inside a group would hash the same.
 * A missed change leaves one 64-byte block stale until that block next
 * changes, so it self-heals; at 2^-32 per changed group that is the right
 * trade against the 61 KB of .bss a byte-exact shadow copy would need on a
 * machine whose entire .bss budget is one megabyte. */
static inline uint32_t fmt_dirty_fold(const uint32_t *s)
{
    uint32_t h = 0x811c9dc5u;
    h ^= s[0];  h += s[1];
    h ^= s[2];  h += s[3];
    h ^= s[4];  h += s[5];
    h ^= s[6];  h += s[7];
    h ^= s[8];  h += s[9];
    h ^= s[10]; h += s[11];
    h ^= s[12]; h += s[13];
    h ^= s[14]; h += s[15];
    return h;
}

/* Write complete 64-byte source groups to the two physical banks.  A group
 * contains sixteen source dwords: even dwords belong to the low bank and odd
 * dwords to the high bank.  The default path finishes one bank pass before
 * touching the other, so each destination pointer advances monotonically.
 * FMTOWNS_VRAM_INTERLEAVED_REFERENCE is retained for pixel/reference and
 * hardware A/B measurements. */
static inline void fmt_write_low_group(volatile uint32_t *d, const uint32_t *s)
{
    d[0] = s[0];  d[1] = s[2];  d[2] = s[4];  d[3] = s[6];
    d[4] = s[8];  d[5] = s[10]; d[6] = s[12]; d[7] = s[14];
}

static inline void fmt_write_high_group(volatile uint32_t *d, const uint32_t *s)
{
    d[0] = s[1];  d[1] = s[3];  d[2] = s[5];  d[3] = s[7];
    d[4] = s[9];  d[5] = s[11]; d[6] = s[13]; d[7] = s[15];
}

static void fmt_write_groups(volatile uint32_t *lo, volatile uint32_t *hi,
                             const uint32_t *s, uint32_t groups)
{
#if defined(FMTOWNS_VRAM_INTERLEAVED_REFERENCE)
    while (groups--) {
        lo[0] = s[0];  hi[0] = s[1];
        lo[1] = s[2];  hi[1] = s[3];
        lo[2] = s[4];  hi[2] = s[5];
        lo[3] = s[6];  hi[3] = s[7];
        lo[4] = s[8];  hi[4] = s[9];
        lo[5] = s[10]; hi[5] = s[11];
        lo[6] = s[12]; hi[6] = s[13];
        lo[7] = s[14]; hi[7] = s[15];
        lo += 8; hi += 8; s += 16;
    }
#else
    /* Four groups per setup amortises the loop branch without needing more
       than the i386's general registers. */
    {
        const uint32_t *source = s;
        uint32_t n = groups;
        while (n >= 4u) {
            fmt_write_low_group(lo,      s);
            fmt_write_low_group(lo + 8,  s + 16);
            fmt_write_low_group(lo + 16, s + 32);
            fmt_write_low_group(lo + 24, s + 48);
            lo += 32; s += 64; n -= 4u;
        }
        while (n--) {
            fmt_write_low_group(lo, s);
            lo += 8; s += 16;
        }

        s = source;
        n = groups;
        while (n >= 4u) {
            fmt_write_high_group(hi,      s);
            fmt_write_high_group(hi + 8,  s + 16);
            fmt_write_high_group(hi + 16, s + 32);
            fmt_write_high_group(hi + 24, s + 48);
            hi += 32; s += 64; n -= 4u;
        }
        while (n--) {
            fmt_write_high_group(hi, s);
            hi += 8; s += 16;
        }
    }
#endif
}

void fmt_invalidate_dirty_present(void)
{
    g_dirty_valid = 0;
}

int fmt_put_image_dirty(const void *src)
{
    return fmt_put_image_dirty_rows(src, 0, 0);
}

int fmt_put_image_dirty_rows(const void *src, const unsigned char *row_mask,
                             const unsigned char *force_mask)
{
    volatile uint8_t *vram = (volatile uint8_t *)g_fmt_vram0_base;
    uint32_t total = (uint32_t)g_cur->stride * (uint32_t)g_cur->height;
    uint32_t groups = total / (FMT_DIRTY_GROUP_DWORDS * 4u);
    uint32_t base;
    volatile uint32_t *lo;
    volatile uint32_t *hi;
    const uint32_t *s = (const uint32_t *)src;
    uint32_t *hash = g_dirty_hash;
    uint8_t *prev = g_dirty_prev;
    uint32_t written = 0;
    uint32_t g;

    /* Same preconditions as fmt_put_image()'s fast path, plus a size the
     * hash table covers.  Anything else is not the game's present. */
    uint32_t row_groups = (uint32_t)g_cur->stride / (FMT_DIRTY_GROUP_DWORDS * 4u);

    /* Row masks are one byte per scanline with one bit per group, so they only
     * describe a mode whose row divides into at most 8 groups and whose height
     * fits the table.  Anything else falls back to scanning everything. */
    if (row_mask && ((uint32_t)g_cur->stride % (FMT_DIRTY_GROUP_DWORDS * 4u) != 0
                     || row_groups == 0 || row_groups > 8u
                     || (uint32_t)g_cur->height > FMT_DIRTY_MAX_ROWS)) {
        row_mask = 0;
    }

    if (g_cur->linear || g_cur->bpp != 8 || groups > FMT_DIRTY_MAX_GROUPS
        || row_groups > 32u
        || total % (FMT_DIRTY_GROUP_DWORDS * 4u) != 0
        || (g_fmt_draw_buffer_offset & 7u) != 0 || (g_cur->stride & 7u) != 0) {
        fmt_put_image(src, g_cur->width, g_cur->height, g_cur->stride);
        g_dirty_valid = 0;
        return 1;
    }

    base = g_fmt_draw_buffer_offset >> 1;
    lo = (volatile uint32_t *)(vram + base);
    hi = (volatile uint32_t *)(vram + 0x40000u + base);

    if (!g_dirty_valid) {
        /* Nothing is known about either page: write everything, seed the
         * hashes, and mark every group dirty so the *other* page gets a
         * full write on the next present too. */
        fmt_put_image(src, g_cur->width, g_cur->height, g_cur->stride);
        for (g = 0; g < groups; ++g) {
            hash[g] = fmt_dirty_fold(s + g * FMT_DIRTY_GROUP_DWORDS);
            prev[g] = 1;
        }
        for (g = 0; g < FMT_DIRTY_MAX_ROWS; ++g) g_dirty_prev_rows[g] = 0xffu;
        g_dirty_valid = 1;
        return 1;
    }

    if (row_mask) {
        uint32_t all = (uint32_t)((1u << row_groups) - 1u);
        uint32_t y;
        g = 0;
        for (y = 0; y < (uint32_t)g_cur->height; ++y) {
            uint32_t owed = 0;
            uint32_t m;
            uint32_t f = force_mask ? ((uint32_t)force_mask[y] & all) : 0u;
            uint32_t write_mask = 0;
            const uint32_t *row_s = s;
            volatile uint32_t *row_lo = lo;
            volatile uint32_t *row_hi = hi;
            uint32_t bit;

            /* A comparison made because of LAST frame's declaration can
             * discover a change that THIS frame did not declare.  Partial
             * composite restoration deliberately works that way: restoring
             * the old cursor is covered by the old cursor's mask, while this
             * frame declares only its new position.
             *
             * That discovered change was written to one VRAM page and
             * prev[] records that the other page is still owed the write.
             * Include those per-group debts in the scan gate.  Using only
             * the two declaration masks dropped the group one frame too
             * early, leaving alternating pages with different cursor/card/UI
             * pixels (and the same stale-page artifact after the fire scene). */
            for (bit = 0; bit < row_groups; ++bit) {
                if (prev[g + bit]) owed |= 1u << bit;
            }
            m = ((uint32_t)row_mask[y]
                 | (uint32_t)g_dirty_prev_rows[y]
                 | owed) & all;
            g_dirty_prev_rows[y] = (uint8_t)(row_mask[y] & all);
            if (!m) {
                /* Whole scanline untouched in both frames: step over it
                 * without reading one byte of it, which is the point. */
                lo += 8u * row_groups;
                hi += 8u * row_groups;
                s  += FMT_DIRTY_GROUP_DWORDS * row_groups;
                g  += row_groups;
                continue;
            }
            for (bit = 0; bit < row_groups; ++bit) {
                if (f & (1u << bit)) {
                    /* The caller has already said this group changed, so
                     * reading it back and folding it would only confirm what
                     * is known.  Leave the stored hash alone and record
                     * FMT_DIRTY_NO_HASH, which makes the next comparison of
                     * this group unconditional rather than a comparison
                     * against a value that no longer describes it. */
                    write_mask |= 1u << bit;
                    prev[g + bit] = FMT_DIRTY_NO_HASH;
                } else if (m & (1u << bit)) {
                    const uint32_t *group_s = row_s + bit * FMT_DIRTY_GROUP_DWORDS;
                    uint32_t group = g + bit;
                    uint32_t h = fmt_dirty_fold(group_s);
                    unsigned int changed = (prev[group] == FMT_DIRTY_NO_HASH) | (h != hash[group]);
                    hash[group] = h;
                    if (changed | prev[group]) write_mask |= 1u << bit;
                    prev[group] = (uint8_t)changed;
                }
            }
            bit = 0;
            while (bit < row_groups) {
                uint32_t run;
                if (!(write_mask & (1u << bit))) { ++bit; continue; }
                run = bit;
                while (run < row_groups && (write_mask & (1u << run))) ++run;
                fmt_write_groups(row_lo + bit * 8u, row_hi + bit * 8u,
                                 row_s + bit * FMT_DIRTY_GROUP_DWORDS, run - bit);
                written += run - bit;
                bit = run;
            }
            lo += 8u * row_groups;
            hi += 8u * row_groups;
            s += FMT_DIRTY_GROUP_DWORDS * row_groups;
            g += row_groups;
        }
        return written != 0;
    }

    for (g = 0; g < FMT_DIRTY_MAX_ROWS; ++g) g_dirty_prev_rows[g] = 0xffu;
    {
        uint32_t y;
        for (y = 0; y < (uint32_t)g_cur->height; ++y) {
            const uint32_t *row_s = (const uint32_t *)((const uint8_t *)src + y * g_cur->stride);
            volatile uint32_t *row_lo = (volatile uint32_t *)(vram + base + y * row_groups * 32u);
            volatile uint32_t *row_hi = (volatile uint32_t *)(vram + 0x40000u + base + y * row_groups * 32u);
            uint32_t write_mask = 0;
            uint32_t bit;

            for (bit = 0; bit < row_groups; ++bit) {
                uint32_t group = y * row_groups + bit;
                uint32_t h = fmt_dirty_fold(row_s + bit * FMT_DIRTY_GROUP_DWORDS);
                unsigned int changed = (h != hash[group]);
                if (changed) hash[group] = h;
                if (changed | prev[group]) write_mask |= 1u << bit;
                prev[group] = (uint8_t)changed;
            }

            bit = 0;
            while (bit < row_groups) {
                uint32_t run;
                if (!(write_mask & (1u << bit))) { ++bit; continue; }
                run = bit;
                while (run < row_groups && (write_mask & (1u << run))) ++run;
                fmt_write_groups(row_lo + bit * 8u, row_hi + bit * 8u,
                                 row_s + bit * FMT_DIRTY_GROUP_DWORDS, run - bit);
                written += run - bit;
                bit = run;
            }
        }
    }

#if defined(FMTOWNS_MEASURE_DIRTY_BAR)
    /* Attribution knob (EXTRA_CORE_DEFINES=-DFMTOWNS_MEASURE_DIRTY_BAR):
     * paint the number of groups this present actually wrote as a bar
     * across row 1 of VRAM, full scale = all of them.  It is the only way
     * to tell "the dirty present is skipping nothing" apart from "the
     * comparison itself is what costs" -- and those two have opposite
     * fixes. */
    {
        uint32_t n = (written * 256u) / (groups ? groups : 1u), x;
        for (x = 0; x < 256u; ++x) {
            uint32_t off = g_fmt_draw_buffer_offset + 256u + x;
            ((volatile uint8_t *)g_fmt_vram0_base)[fmt_vram_singlepage_offset(off)] =
                (x < n) ? 3u : 255u;
        }
    }
#endif
    return written != 0;
}

void fmt_put_image(const void *src, int width, int height, int stride)
{
    volatile uint8_t *vram = (volatile uint8_t *)g_fmt_vram0_base;
    const uint8_t *src8 = (const uint8_t *)src;
    int bytes_per_pixel = (g_cur->bpp == 16) ? 2 : 1;
    uint32_t span = (uint32_t)width * (uint32_t)bytes_per_pixel;

    /*
     * Fast path for a full-width single-page blit -- what a game's
     * per-frame present always is, and far too slow the byte-at-a-time way
     * on a 386SX (61440 stores per frame, each with the swizzle recomputed).
     *
     * The swizzle is not as scattered as it looks.  Writing off = 8k + j:
     *
     *     trans(8k+j) = ((j&4) << 16) | (4k) | (j&3)
     *
     * so each aligned group of 8 source bytes lands as two *contiguous*
     * 4-byte runs: bytes 0-3 at 4k, bytes 4-7 at 0x40000 + 4k.  Every 8
     * source bytes is therefore one dword store into each of two banks,
     * with the destination pointers just marching forward -- no per-byte
     * address arithmetic at all, and 4 bytes moved per store instead of 1.
     *
     * Only taken when the copy is contiguous in `off` space (src stride ==
     * VRAM stride, full width) and both the draw-page offset and the stride
     * are multiples of 8, which is exactly the 256x240 8bpp case
     * (stride 256, page size 61440).  Anything else falls through to the
     * general loop below.
     */
    if (!g_cur->linear && g_cur->bpp == 8 && span == (uint32_t)stride && span == g_cur->stride
        && (g_fmt_draw_buffer_offset & 7u) == 0 && (g_cur->stride & 7u) == 0) {
        uint32_t total = (uint32_t)height * (uint32_t)g_cur->stride;
        uint32_t base = g_fmt_draw_buffer_offset >> 1;
        volatile uint32_t *lo = (volatile uint32_t *)(vram + base);
        volatile uint32_t *hi = (volatile uint32_t *)(vram + 0x40000u + base);
        /* Dword loads off a byte-aligned source are legal on x86 (they only
         * cost an extra bus cycle when they straddle a dword boundary), and
         * a game framebuffer is aligned in practice anyway. */
        /* The helper keeps four-pixel dword transfers while making the bank
           ordering an explicit, shared measurement switch. */
        fmt_write_groups(lo, hi, (const uint32_t *)src, total >> 6);
        return;
    }

    for (int y = 0; y < height; y++) {
        const uint8_t *srow = src8 + y * stride;
        for (int x = 0; x < width * bytes_per_pixel; x++) {
            uint32_t off = g_fmt_draw_buffer_offset
                + (uint32_t)y * (uint32_t)g_cur->stride + (uint32_t)x;
            vram[g_cur->linear ? off : vram_singlepage_trans(off)] = srow[x];
        }
    }
}

int fmt_page_flipping_available(void)
{
    return g_can_flip;
}

uint8_t fmt_display_page(void)
{
    return g_display_page;
}

uint8_t fmt_draw_page(void)
{
    return g_draw_page;
}

uint32_t fmt_frame_buffer_size(void)
{
    return g_frame_buffer_size;
}

int fmt_flip_page(void)
{
    return fmt_flip_page_poll(0);
}

int fmt_flip_page_poll(void (*poll)(void))
{
    if (!g_can_flip) {
        return -1;
    }

    fmt_wait_vsync_poll(poll);
    return fmt_flip_page_now();
}

/* The page swap on its own, with no vsync wait of its own.  For callers that
 * have already parked themselves inside blanking to do other blanking-only
 * work (palette RAM, above all) and must not spend a second field waiting. */

int fmt_flip_page_now(void)
{
    if (!g_can_flip) {
        return -1;
    }

    g_display_page = g_draw_page;
    g_draw_page ^= 1u;

    /* FA0 increments by eight bytes in single-page 8bpp and 16bpp modes
     * (TOWNSEMU TownsCRTC::GetPageVRAMAddressOffset()).
     *
     * FA1 has to be given the same address.  Single-page mode is not "one
     * layer using one register set": the CRTC still fetches the picture
     * from both VRAM banks, alternating between them every 16 bits, and
     * each bank is addressed by its own register set (bank 0 by
     * FA0/HAJ0/FO0/LO0, bank 1 by FA1/HAJ1/FO1/LO1).  Flipping FA0 alone
     * leaves every other pair of 8bpp pixels being fetched from the page
     * that is not being displayed: on hardware that showed up as a
     * 4-pixel-period stripe pattern over the whole screen, half of it the
     * palette-index-0 cream of the never-drawn page.  Emulators that model
     * only one register set in this mode show nothing wrong. */
    uint16_t page_addr =
        (uint16_t)(((uint32_t)g_display_page * g_frame_buffer_size) / 8u);
    crtc_out16(FA0, page_addr);
    crtc_out16(FA1, page_addr);
    g_fmt_draw_buffer_offset = (uint32_t)g_draw_page * g_frame_buffer_size;
    return 0;
}

void fmt_wait_vsync(void)
{
    fmt_wait_vsync_poll(0);
}

void fmt_wait_vsync_poll(void (*poll)(void))
{
    outb(30, 0x0440);
    while (inb(0x443) & 4) {
        if (poll) {
            poll();
        }
    }
    while (!(inb(0x443) & 4)) {
        if (poll) {
            poll();
        }
    }
}
