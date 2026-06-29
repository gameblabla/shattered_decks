/* Portable (host/headless/exotic) 3D-renderer span-filler backend.
 *
 * Compiled in every non-PC-FX build. renderer3d_port.h selects the portable C
 * paths (no V810/SH1 assembly, byte framebuffer), so this TU holds the
 * platform-agnostic software rasterizer leaves. The PC-FX build compiles
 * renderer3d_pcfx.c instead. Shared geometry/edge walking lives in
 * renderer3d.c; shared inline helpers and the filler implementation are
 * included below. */
#include "renderer3d_internal.h"
#include "renderer3d_spans.inc"
