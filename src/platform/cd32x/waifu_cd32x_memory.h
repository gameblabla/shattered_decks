#ifndef WAIFU_CD32X_MEMORY_H
#define WAIFU_CD32X_MEMORY_H

#include <stdint.h>
#include <stddef.h>

/*
 * 32X/CD32X RAM budget notes:
 * - SH-2 program/data/stack must fit in the 256 KiB 32X SDRAM link region.
 * - The game's host/PC-FX asset staging cache is intentionally large; CD32X
 *   uses explicit memory tiers instead of linking that cache into .bss.
 * - Do not use the 32X framebuffer pages as general cache.  The title is a
 *   display asset and is streamed there directly; gameplay assets stay in SDRAM.
 * - Transient CD32X asset staging starts immediately after linked .bss instead
 *   of at a fixed low SDRAM address.  This keeps the first streamed title blob
 *   from overwriting SH-2 code/data while leaving PC-FX/headless untouched.
 */
#define WAIFU_CD32X_SDRAM_CACHED_BASE  0x06000000u
#define WAIFU_CD32X_SDRAM_CACHED_LIMIT 0x06040000u
#define WAIFU_CD32X_SDRAM_UNCACHED_BASE 0x26000000u

/* Logical staging budget for compile-time common asset layout.  The live CD32X
 * backend must stream later card/story blobs instead of assuming all of this is
 * resident in SDRAM at once. */
#define WAIFU_CD32X_ASSET_ARENA_BYTES   (224u * 1024u)

extern uint8_t waifu_cd32x_bss_end[] __asm__("__bss_end");

static inline uintptr_t waifu_cd32x_align_up(uintptr_t v, uintptr_t align)
{
    return (v + (align - 1u)) & ~(align - 1u);
}

static inline uint8_t *waifu_cd32x_transient_arena(void)
{
    return (uint8_t *)waifu_cd32x_align_up((uintptr_t)waifu_cd32x_bss_end, 16u);
}

static inline size_t waifu_cd32x_transient_arena_bytes(void)
{
    uintptr_t base = (uintptr_t)waifu_cd32x_transient_arena();
    return (base < WAIFU_CD32X_SDRAM_CACHED_LIMIT) ?
        (size_t)(WAIFU_CD32X_SDRAM_CACHED_LIMIT - base) : 0u;
}

static inline uint8_t *waifu_cd32x_asset_arena(void)
{
    return waifu_cd32x_transient_arena();
}


#endif /* WAIFU_CD32X_MEMORY_H */
