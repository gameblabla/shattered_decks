/* FM TOWNS Marty (i386) 3D-renderer span-filler backend.
 *
 * Mirrors the CD32X/PC-FX split: shared geometry/edge walking lives in
 * renderer3d.c, while target-specific span choices are compiled in this
 * separate translation unit. renderer3d_port.h's WAIFU_FM_FMTOWNS branch
 * selects the same portable-C, flat-shaded-only span fillers as CD32X (no
 * V810/SH-2 assembly, byte framebuffer) -- this platform uses the CD32X
 * renderer's approach deliberately, not the PC-FX's textured/perspective
 * one; see src/platform/fmtowns/STATUS.md's milestone 6 notes.
 *
 * NOT YET LINKED INTO A BUILD: Makefile.fmtowns does not compile
 * src/engine/*.c at all yet (see STATUS.md's "the single most important
 * thing to do next" -- the portable game core has not been made to
 * compile freestanding for this target). This file exists so the seam is
 * registered and ready the moment that lands, matching what the task's
 * milestone list calls for structurally, but it has not been build- or
 * boot-verified standalone the way milestones 1-5 were. A separate,
 * boot-verified flat-shaded rasterizer proof of concept (a rotating cube,
 * not wired to this file or the game's vertex data) lives in
 * src/platform/fmtowns/fmtowns_cube_demo.c instead. */
#include "renderer3d_internal.h"
#include "renderer3d_spans.inc"
