/*
 * assets.S - link the 256x240 8bpp indexed background image and its
 * palette directly into the payload binary.
 *
 * The old RUN386/.EXP path loaded these at runtime from files on the CD's
 * ISO9660 filesystem (IMAGE.RAW / PALETTE.BIN, via DOS int 0x21 handle
 * I/O). The bare-metal boot path has no filesystem driver and no DOS, so
 * instead the same two files (produced by tools/png2fmt from its 320x240 PNG)
 * are embedded directly into the ELF as data and linked in.
 */

.section ".rodata"

.globl g_image_data
.balign 4
g_image_data:
	.incbin "CD/IMAGE.RAW"
.globl g_image_data_end
g_image_data_end:

.globl g_palette_data
.balign 4
g_palette_data:
	.incbin "CD/PALETTE.BIN"
.globl g_palette_data_end
g_palette_data_end:

.globl g_sprite_pisga0
.balign 4
g_sprite_pisga0:
	.incbin "build/PISGA0.spr"
.globl g_sprite_pisga0_end
g_sprite_pisga0_end:

.globl g_sprite_shtgb0
.balign 4
g_sprite_shtgb0:
	.incbin "build/SHTGB0.spr"
.globl g_sprite_shtgb0_end
g_sprite_shtgb0_end:
