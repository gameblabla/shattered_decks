#include "libfmt.h"
#include "palette.h"
#include "io.h"
#include "defs.h"

/*
 * FM TOWNS VRAM0 physical base address for the 386SX-based UX/Marty
 * memory map (TOWNSEMU's TOWNSADDR_386SX_VRAM0_BASE). The "full"
 * 386DX/486 FM TOWNS map uses 0x80000000 instead - this library
 * currently only targets Marty.
 */
#define TOWNS_VRAM0_BASE_MARTY   FMT_VRAM0_BASE
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

    g_frame_buffer_size = (uint32_t)m->stride * m->height;
    g_can_flip = (!m->linear && g_frame_buffer_size * 2u <= TOWNS_VRAM_SIZE);
    g_display_page = 0;
    g_draw_page = g_can_flip ? 1 : 0;
    g_fmt_draw_buffer_offset = (uint32_t)g_draw_page * g_frame_buffer_size;

    /* FA0 is expressed in groups of eight bytes in all supported
     * single-page 8/16bpp modes.  Begin on the first, cleared page. */
    crtc_out16(FA0, 0);

    /* A mode change must never expose stale VRAM.  Clear all buffers that
     * belong to the mode while display output is stopped. */
    volatile uint8_t *vram = (volatile uint8_t *)TOWNS_VRAM0_BASE_MARTY;
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

void fmt_load_palette(const uint8_t *rgb888, int count)
{
    for (int i = 0; i < count; i++) {
        uint8_t r = rgb888[i * 3];
        uint8_t g = rgb888[i * 3 + 1];
        uint8_t b = rgb888[i * 3 + 2];
        set_palette((uint8_t)i, r, g, b);
    }
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
void fmt_put_image(const void *src, int width, int height, int stride)
{
    volatile uint8_t *vram = (volatile uint8_t *)TOWNS_VRAM0_BASE_MARTY;
    const uint8_t *src8 = (const uint8_t *)src;
    int bytes_per_pixel = (g_cur->bpp == 16) ? 2 : 1;

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

    g_display_page = g_draw_page;
    g_draw_page ^= 1u;

    /* FA0 increments by eight bytes in single-page 8bpp and 16bpp modes
     * (TOWNSEMU TownsCRTC::GetPageVRAMAddressOffset()). */
    crtc_out16(FA0,
        (uint16_t)(((uint32_t)g_display_page * g_frame_buffer_size) / 8u));
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
