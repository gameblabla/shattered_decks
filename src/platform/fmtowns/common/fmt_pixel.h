#ifndef FMT_PIXEL_H
#define FMT_PIXEL_H

#include <stdint.h>
#include "io.h"
#include "libfmt.h"

/*
 * Packed-pixel drawing for the native FM TOWNS low-resolution VRAM window.
 *
 * In single-page mode, four consecutive logical bytes alternate between the
 * VRAM's two 16-bit banks.  TOWNSEMU models the same mapping with
 * TownsSinglePageVRAMAddressTransform::SinglePageOffsetToLinearOffset().
 * Transforming the byte offset here lets one aligned 32-bit CPU store update
 * four 8bpp pixels (or two RGB555 pixels) in one packed-pixel transfer.
 *
 * The one-pixel 8bpp path uses the hardware packed-pixel mask registers.  It
 * performs a byte store with only the requested byte lane enabled,
 * then restores the all-writable mask expected by normal VRAM code.
 *
 * Coordinates are intentionally unchecked.  The 4-pixel routine requires x
 * divisible by 4; the 2-pixel routines require x divisible by 2.  All pixels
 * in a call must fit on the scanline.
 */
#define FMT_VRAM0_BASE                 0xA00000u
#define FMT_VRAM_MASK_ADDRESS_PORT     0x0458u
#define FMT_VRAM_MASK_DATA_LOW_PORT    0x045Au
#define FMT_VRAM_MASK_DATA_HIGH_PORT   0x045Bu

static inline uint32_t fmt_vram_singlepage_offset(uint32_t logical_offset)
{
    return ((logical_offset & 4u) << 16)
         | ((logical_offset & 0x7fff8u) >> 1)
         |  (logical_offset & 3u);
}

static inline void fmt_vram_set_packed_mask(uint32_t mask)
{
    outb(0, FMT_VRAM_MASK_ADDRESS_PORT);
    outb((uint8_t)mask, FMT_VRAM_MASK_DATA_LOW_PORT);
    outb((uint8_t)(mask >> 8), FMT_VRAM_MASK_DATA_HIGH_PORT);
    outb(1, FMT_VRAM_MASK_ADDRESS_PORT);
    outb((uint8_t)(mask >> 16), FMT_VRAM_MASK_DATA_LOW_PORT);
    outb((uint8_t)(mask >> 24), FMT_VRAM_MASK_DATA_HIGH_PORT);
}

static inline void fmt_vram_store32(uint32_t logical_offset, uint32_t pixels)
{
    volatile uint32_t *dst = (volatile uint32_t *)(FMT_VRAM0_BASE
        + fmt_vram_singlepage_offset(g_fmt_draw_buffer_offset + logical_offset));
    __asm__ volatile ("movl %1,(%0)" : : "r" (dst), "r" (pixels) : "memory");
}

static inline void fmt_vram_store16(uint32_t logical_offset, uint16_t pixels)
{
    volatile uint16_t *dst = (volatile uint16_t *)(FMT_VRAM0_BASE
        + fmt_vram_singlepage_offset(g_fmt_draw_buffer_offset + logical_offset));
    __asm__ volatile ("movw %1,(%0)" : : "r" (dst), "r" (pixels) : "memory");
}

static inline void fmt_vram_store8(uint32_t logical_offset, uint8_t pixel)
{
    volatile uint8_t *dst = (volatile uint8_t *)(FMT_VRAM0_BASE
        + fmt_vram_singlepage_offset(g_fmt_draw_buffer_offset + logical_offset));
    __asm__ volatile ("movb %1,(%0)" : : "r" (dst), "q" (pixel) : "memory");
}

static inline void Set_4Pixels_320x240_8bpp(
    uint16_t x, uint16_t y, uint32_t packed_pixels)
{
    fmt_vram_store32((uint32_t)y * 320u + x, packed_pixels);
}

static inline void Set_2Pixels_320x240_8bpp(
    uint16_t x, uint16_t y, uint8_t color0, uint8_t color1)
{
    fmt_vram_store16((uint32_t)y * 320u + x,
        (uint16_t)color0 | ((uint16_t)color1 << 8));
}

static inline void Set_Pixel_320x240_8bpp(
    uint16_t x, uint16_t y, uint8_t color)
{
    uint32_t logical = (uint32_t)y * 320u + x;
    uint32_t lane = logical & 3u;
    fmt_vram_set_packed_mask(0xffu << (lane * 8u));
    fmt_vram_store8(logical, color);
    fmt_vram_set_packed_mask(0xffffffffu);
}

static inline void Set_2Pixels_320x240_15bpp(
    uint16_t x, uint16_t y, uint16_t color0, uint16_t color1)
{
    fmt_vram_store32(((uint32_t)y * 320u + x) * 2u,
        (uint32_t)color0 | ((uint32_t)color1 << 16));
}

static inline void Set_Pixel_320x240_15bpp(
    uint16_t x, uint16_t y, uint16_t color)
{
    fmt_vram_store16(((uint32_t)y * 320u + x) * 2u, color);
}

/* Existing lowercase spelling retained for source compatibility. */
static inline void set_pixel_320_8(uint16_t x, uint16_t y, uint8_t color)
{
    Set_Pixel_320x240_8bpp(x, y, color);
}

static inline void set_pixel_320_16(uint16_t x, uint16_t y, uint16_t color)
{
    Set_Pixel_320x240_15bpp(x, y, color);
}

#endif
