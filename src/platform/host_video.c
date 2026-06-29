/* Host (SDL 1.2 / headless) implementation of the platform video seams.
 *
 * The host renders everything into the CPU framebuffer that the frontend then
 * blits, so it has no dedicated hardware background layer the way the PC-FX
 * RAINBOW unit does. waifu_platform_background_request() therefore returns 0,
 * which tells the common scene code to composite the sky into the framebuffer
 * itself (the software sky path).
 *
 * The seam is deliberately in place so a future host background layer (e.g. an
 * SDL surface drawn behind the game framebuffer) can present `kind`/`hscroll`
 * here and return 1, without any change to the common game code. */

#include "platform.h"

int waifu_platform_background_request(WaifuBackgroundKind kind, int hscroll)
{
    (void)kind;
    (void)hscroll;
    return 0;
}
