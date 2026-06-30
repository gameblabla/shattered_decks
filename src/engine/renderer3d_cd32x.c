/* Sega CD 32X (SH-2 / 32X VDP) 3D-renderer span-filler backend.
 *
 * This mirrors the PC-FX split: shared geometry/edge walking lives in
 * renderer3d.c, while target-specific span choices and optional SH assembly are
 * compiled in this separate translation unit. */
#include "renderer3d_internal.h"
#include "renderer3d_spans.inc"
