/* PC-FX (V810 / KING) 3D-renderer span-filler backend.
 *
 * Compiled only in the PC-FX build, where renderer3d_port.h enables the V810
 * assembly scanline paths (and, when CFX_RENDERER_DIRECT_KRAM is on, the KING
 * KRAM write paths). This is where the renderer's hardware-specific assembly
 * lives; the host build compiles renderer3d_generic.c instead. Shared
 * geometry/edge walking lives in renderer3d.c. */
#include "renderer3d_internal.h"
#include "renderer3d_spans.inc"
