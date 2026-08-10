#ifndef FMTOWNS_CUBE_DEMO_H
#define FMTOWNS_CUBE_DEMO_H

#include <stdint.h>

/* Advances the rotation by one frame's worth and rasterizes the cube (flat
 * shaded, backface-culled, no textures -- the CD32X renderer's approach,
 * see src/platform/fmtowns/STATUS.md's milestone 6 notes) into `frame`, a
 * 256x240 8bpp buffer. Does not touch rows outside [0, 231] so a caller
 * can still overlay its own bottom status strip below that (see
 * fmtowns_main.c's fmtowns_pad_status_loop()). Palette indices 1-6 must be
 * loaded with the six face colours by the caller (fmtowns_cube_demo_palette()
 * fills them); index 0 is the background and is cleared by this call. */
void fmtowns_cube_demo_frame(uint8_t *frame);

/* Fills `palette` (256*3 RGB888 bytes) with the background colour (index 0)
 * and the six flat face colours (indices 1-6). Leaves every other index
 * untouched so the caller can still use them for its own overlays. */
void fmtowns_cube_demo_palette(uint8_t *palette);

#endif /* FMTOWNS_CUBE_DEMO_H */
