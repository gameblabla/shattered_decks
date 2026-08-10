/*
 * fmtowns_title_asset.S - milestone 2: link the game's real 256x240 8bpp
 * title screen + palette directly into the payload binary, so the video
 * present pipeline (fmtowns_video.c) can be proven against real production
 * art before CD-ROM/ISO9660 loading is wired up (that's milestone 3 --
 * see src/platform/fmtowns/fmtowns_main.c and STATUS.md).
 *
 * build/fmtowns/title_pixels.bin / title_palette.bin are produced by
 * tools/fmtowns/extract_title_asset.py from the same committed
 * src/generated/title_asset.h every other platform's title screen comes
 * from -- not separately authored art.
 */

.section ".rodata"

.globl g_fmtowns_title_pixels
.balign 4
g_fmtowns_title_pixels:
	.incbin "build/fmtowns/title_pixels.bin"
.globl g_fmtowns_title_pixels_end
g_fmtowns_title_pixels_end:

.globl g_fmtowns_title_palette
.balign 4
g_fmtowns_title_palette:
	.incbin "build/fmtowns/title_palette.bin"
.globl g_fmtowns_title_palette_end
g_fmtowns_title_palette_end:
