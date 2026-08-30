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

/* No widescreen room on the host framebuffer: the HUD keeps its normal layout. */
int waifu_platform_ui_extra_w(void) { return 0; }
int waifu_platform_arena_backdrop(void) { return 0; }
void waifu_platform_ui_hud(int on) { (void)on; }
int waifu_platform_performance_tier(void) { return 2; }
int waifu_platform_glyph(int x, int y, int cell_w, unsigned char ch, unsigned char fg, unsigned char shadow)
{ (void)x; (void)y; (void)cell_w; (void)ch; (void)fg; (void)shadow; return 0; }
void waifu_platform_prewarm_ending(void) {}

/* The host has no hardware text layer, so every UI panel is rendered with the
   software text API by the caller. Returning 0 selects that software path. */
int waifu_platform_text_overlay(WaifuTextOverlayKind kind, const WaifuTextOverlayParams *params)
{
    (void)kind;
    (void)params;
    return 0;
}

void waifu_platform_text_overlay_clear(void)
{
}

int waifu_platform_text_overlay_is_hardware(void)
{
    return 0;
}

void waifu_platform_story_layers_begin(void) {}
int waifu_platform_story_portrait(int portrait_id, int x, int y)
{ (void)portrait_id; (void)x; (void)y; return 0; }
