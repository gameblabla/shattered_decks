#ifndef WAIFU_HW3D_H
#define WAIFU_HW3D_H

/* ---------------------------------------------------------------------------
 * Hardware 3D scene-capture seam.
 *
 * Platforms with a real polygon renderer (SDL3 GPU) capture the game's 3D
 * scenes as world-space geometry BEFORE the software projection/rasterization
 * runs, and present them on a hardware layer *below* the 8bpp framebuffer
 * (which then acts as the 2D/UI overlay, keyed on palette index 0 exactly
 * like the PC-FX RAINBOW / CD32X MD-plane see-through convention).
 *
 * Seam pattern (same as platform.h): every capture call returns 1 when the
 * hardware took the primitive — the call site must then skip its software
 * draw — or 0 when there is no hardware 3D layer, in which case the SAME
 * call site runs the existing software path unchanged.
 *
 * Builds without a hardware 3D backend (headless, SDL 1.2, PC-FX, CD32X)
 * compile the static-inline stubs below: they return 0, evaluate nothing,
 * and optimize out entirely, so the common code stays byte-identical.
 * A backend build defines WAIFU_PLATFORM_HW3D and links a translation unit
 * providing the real functions.
 *
 * Coordinates are the game's own Q8.8 fixed point (256 units = 1.0); the
 * camera mirrors main.c's Camera layout (eye/target/up + focal). Colors are
 * palette indices into the game's active 256-entry palette, so hardware
 * output follows palette swaps and fades automatically.
 * ------------------------------------------------------------------------- */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WaifuHw3DVec3 {
    int32_t x, y, z;                    /* Q8.8 world space */
} WaifuHw3DVec3;

typedef struct WaifuHw3DCamera {
    WaifuHw3DVec3 eye, target, up;
    int32_t focal;                      /* Q8.8, isotropic pinhole focal */
} WaifuHw3DCamera;

/* Card front frame classes, for waifu_hw2d_card_frame_hint(). */
enum {
    WAIFU_CARD_FRAME_MONSTER = 0,
    WAIFU_CARD_FRAME_SPELL = 1,
    WAIFU_CARD_FRAME_TRAP = 2
};

/* What a support card's front frame writes in the band where a monster writes
   its ATK/DEF, also named through waifu_hw2d_card_frame_hint(). */
enum {
    WAIFU_CARD_LABEL_EQUIP = 0,
    WAIFU_CARD_LABEL_SUPPORT = 1,
    WAIFU_CARD_LABEL_TRAP = 2
};

#if defined(WAIFU_PLATFORM_HW3D)

/* Constant per platform: 1 when a hardware 3D layer exists. Call sites use it
 * to pick the transparent-key clear (index 0) over the opaque software clear. */
int waifu_hw3d_present(void);

/* Registers the shared 32x32 8bpp texture atlas (tile_count vertical tiles).
 * Called once from waifu_fm_init alongside cfx_renderer3d_set_texture_atlas. */
void waifu_hw3d_set_texture_atlas(const void *atlas, int tile_count);

/* One 32x32 atlas tile mapped across a world-space quad (v[0]..v[3] wound like
 * the software draw_quad3d: UV (0,0),(1,0),(1,1),(0,1)). */
int waifu_hw3d_quad(const WaifuHw3DCamera *cam, const WaifuHw3DVec3 v[4], int tile);

/* Pyramid/cone face triangle (base0, base1, apex) tiled rows x cols with the
 * software's UV layout (base v=0, apex v=rows; u flipped by flip_u), plus its
 * gold outline edges in edge_color. */
int waifu_hw3d_tri(const WaifuHw3DCamera *cam, const WaifuHw3DVec3 v[3], int tile,
                   int flip_u, int rows, int cols, uint8_t edge_color);

/* Infinite checkered floor plane at world y = floor_y, alternating tile_a and
 * tile_b every tile_size world units (the software raycast floor's pattern). */
int waifu_hw3d_floor(const WaifuHw3DCamera *cam, int32_t floor_y,
                     int tile_a, int tile_b, int32_t tile_size);

/* A raw 8bpp image (card face/back/support, w x h) mapped across a world quad
 * with UV (0,0),(1,0),(1,1),(0,1), plus its rim outline in edge_color; gray
 * draws the image dimmed with the crossed-out diagonals (used-card state). */
int waifu_hw3d_image_quad(const WaifuHw3DCamera *cam, const WaifuHw3DVec3 v[4],
                          const uint8_t *pixels, int w, int h,
                          int gray, uint8_t edge_color);

/* World-space line (grid lines, void crystals). shadow_px replicates the
 * software +N-pixel-lower duplicate line (0 = single line). */
int waifu_hw3d_line(const WaifuHw3DCamera *cam, const WaifuHw3DVec3 *a,
                    const WaifuHw3DVec3 *b, uint8_t color, int shadow_px);

/* ---- 2D layer -------------------------------------------------------------
 * On a hardware platform the 8bpp framebuffer is never presented: every 2D
 * primitive is captured here instead (in the game's WAIFU_FM_WIDTH x
 * WAIFU_FM_HEIGHT screen space) and rendered as GPU sprites/quads, with
 * palette indices resolved to full color at capture time. Submission order is
 * preserved; primitives captured before the frame's first 3D primitive form
 * the scene background (sky bands behind the floor), the rest draw above the
 * 3D scene — exactly the painter order the software compositor used. */

/* Constant per platform; lets bespoke effect blitters pre-check capture. */
int waifu_hw2d_active(void);

/* Whole-screen clear: resets everything captured so far this frame (2D and
 * 3D) and sets the frame background color, like the software clear wiping the
 * framebuffer. */
int waifu_hw2d_clear(uint8_t color);

int waifu_hw2d_rect(int x, int y, int w, int h, uint8_t color);
int waifu_hw2d_px(int x, int y, uint8_t color);
int waifu_hw2d_line(int x0, int y0, int x1, int y1, uint8_t color);

/* Nearest-scaled 8bpp image blit (card sprites, portraits, title/ending
 * screens, effect buffers). mask: nonzero = opaque texel; NULL mask keys
 * palette index 0 when colorkey0 is set, else the image is fully opaque.
 * gray renders the dimmed used-card look. */
int waifu_hw2d_image(const uint8_t *pix, const uint8_t *mask, int sw, int sh,
                     int dx, int dy, int dw, int dh, int gray, int colorkey0);

/* Names the class of the card whose 8bpp face is about to be captured, so the
 * frontend can pick the matching high-resolution front frame. Only support
 * cards need it -- every Spell and Trap shares one face buffer, so the pointer
 * alone cannot tell them apart -- and it applies to the next support-face draw.
 * `label` (WAIFU_CARD_LABEL_*) is the word that frame's bottom band carries.
 * Purely cosmetic: a platform without hi-res frames ignores it. */
void waifu_hw2d_card_frame_hint(int frame, int label);

/* Screen-space textured quad (projected card animations whose corners exist
 * only as screen points). xy = 4 corner pairs, UV (0,0),(1,0),(1,1),(0,1). */
int waifu_hw2d_image_quad(const uint8_t *pix, int sw, int sh,
                          const int xy[8], int gray);

/* Screen-space flat-coloured quad with an alpha (0..255), for overlays a
 * palette framebuffer cannot express -- the 3D zone cursor's translucent ring
 * and tint. xy = 4 corner pairs, in the same winding as the textured quad.
 * Returns 0 when the platform has no alpha layer, so the caller falls back to
 * its opaque software draw. */
int waifu_hw2d_quad_alpha(const int xy[8], uint8_t color, int alpha);

/* The same quad in true colour rather than a palette index, for shading a
 * platform's palette cannot name -- the lit faces of the spinning 3-D hand
 * cursor, each a different brightness of the same red. Corner pairs may repeat
 * to submit a triangle. Returns 0 where there is no true-colour layer. */
int waifu_hw2d_quad_rgba(const int xy[8], uint8_t r, uint8_t g, uint8_t b, uint8_t a);

/* Scale of the glyph INK inside its text cell, in percent (100 = the shipped
 * face). Cell advances are untouched, so every authored layout still holds --
 * only the drawn glyph shrinks about the cell's centre. Set it, draw, set it
 * back to 100. Lets a label sized for the 8x8 bitmap font (the COM / YOU LP
 * plates) keep its authored plate on a build whose TTF ink runs taller and
 * wider than the cell. */
void waifu_hw2d_text_scale(int percent);

/* Procedural impact burst for the direct attack, drawn by the frontend as one
 * shader pass over the whole viewport: a swept blade, a contact flash, a
 * blooming core, an expanding shockwave ring, hashed ray fans and embers, all
 * smooth analytic falloffs composited with premultiplied alpha (the pass adds
 * light AND darkens the arena behind it). None of that is expressible in a
 * palette framebuffer -- an 8bpp target can only stamp hard-edged discs of one
 * of 256 colours -- so this returns 0 everywhere else and the caller keeps its
 * software slash. `t_q8` is the beat progress in Q8 (256 = one full beat;
 * larger values hold the final dim through the event's settle frames), `dir`
 * is +1/-1 for the slash direction. (cx, cy) is the burst centre in game
 * screen space. */
int waifu_hw2d_impact_fx(int cx, int cy, int t_q8, int dir);

/* The damage readout for that burst: `s` centred on (cx, cy) with a cap height
 * of `cap_px` game pixels, drawn from the scalable glyph atlas as a warm outer
 * glow, a dark outline and a graded gold fill. `glow_q8` scales the glow and
 * `alpha_q8` the whole readout (both Q8). Returns 0 without a scalable glyph
 * layer, so the caller keeps its integer-scaled bitmap number. */
int waifu_hw2d_impact_text(int cx, int cy, int cap_px, const char *s,
                           int glow_q8, int alpha_q8);

#else /* !WAIFU_PLATFORM_HW3D: inert stubs, compile out entirely */

static inline int waifu_hw3d_present(void) { return 0; }
static inline void waifu_hw3d_set_texture_atlas(const void *atlas, int tile_count)
{ (void)atlas; (void)tile_count; }
static inline int waifu_hw3d_quad(const WaifuHw3DCamera *cam, const WaifuHw3DVec3 v[4], int tile)
{ (void)cam; (void)v; (void)tile; return 0; }
static inline int waifu_hw3d_tri(const WaifuHw3DCamera *cam, const WaifuHw3DVec3 v[3], int tile,
                                 int flip_u, int rows, int cols, uint8_t edge_color)
{ (void)cam; (void)v; (void)tile; (void)flip_u; (void)rows; (void)cols; (void)edge_color; return 0; }
static inline int waifu_hw3d_floor(const WaifuHw3DCamera *cam, int32_t floor_y,
                                   int tile_a, int tile_b, int32_t tile_size)
{ (void)cam; (void)floor_y; (void)tile_a; (void)tile_b; (void)tile_size; return 0; }
static inline int waifu_hw3d_image_quad(const WaifuHw3DCamera *cam, const WaifuHw3DVec3 v[4],
                                        const uint8_t *pixels, int w, int h,
                                        int gray, uint8_t edge_color)
{ (void)cam; (void)v; (void)pixels; (void)w; (void)h; (void)gray; (void)edge_color; return 0; }
static inline int waifu_hw3d_line(const WaifuHw3DCamera *cam, const WaifuHw3DVec3 *a,
                                  const WaifuHw3DVec3 *b, uint8_t color, int shadow_px)
{ (void)cam; (void)a; (void)b; (void)color; (void)shadow_px; return 0; }
static inline int waifu_hw2d_active(void) { return 0; }
static inline int waifu_hw2d_clear(uint8_t color) { (void)color; return 0; }
static inline int waifu_hw2d_rect(int x, int y, int w, int h, uint8_t color)
{ (void)x; (void)y; (void)w; (void)h; (void)color; return 0; }
static inline int waifu_hw2d_px(int x, int y, uint8_t color)
{ (void)x; (void)y; (void)color; return 0; }
static inline int waifu_hw2d_line(int x0, int y0, int x1, int y1, uint8_t color)
{ (void)x0; (void)y0; (void)x1; (void)y1; (void)color; return 0; }
static inline int waifu_hw2d_image(const uint8_t *pix, const uint8_t *mask, int sw, int sh,
                                   int dx, int dy, int dw, int dh, int gray, int colorkey0)
{ (void)pix; (void)mask; (void)sw; (void)sh; (void)dx; (void)dy; (void)dw; (void)dh;
  (void)gray; (void)colorkey0; return 0; }
static inline void waifu_hw2d_card_frame_hint(int frame, int label)
{ (void)frame; (void)label; }
static inline int waifu_hw2d_image_quad(const uint8_t *pix, int sw, int sh,
                                        const int xy[8], int gray)
{ (void)pix; (void)sw; (void)sh; (void)xy; (void)gray; return 0; }
static inline int waifu_hw2d_quad_alpha(const int xy[8], uint8_t color, int alpha)
{ (void)xy; (void)color; (void)alpha; return 0; }
static inline int waifu_hw2d_quad_rgba(const int xy[8], uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{ (void)xy; (void)r; (void)g; (void)b; (void)a; return 0; }
static inline void waifu_hw2d_text_scale(int percent) { (void)percent; }
static inline int waifu_hw2d_impact_fx(int cx, int cy, int t_q8, int dir)
{ (void)cx; (void)cy; (void)t_q8; (void)dir; return 0; }
static inline int waifu_hw2d_impact_text(int cx, int cy, int cap_px, const char *s,
                                         int glow_q8, int alpha_q8)
{ (void)cx; (void)cy; (void)cap_px; (void)s; (void)glow_q8; (void)alpha_q8; return 0; }

#endif /* WAIFU_PLATFORM_HW3D */

#ifdef __cplusplus
}
#endif

#endif /* WAIFU_HW3D_H */
