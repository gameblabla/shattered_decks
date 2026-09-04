/* SDL3 platform: hardware scene capture (hw3d.h / hw2d seams) + video seams.
 *
 * This TU replaces host_video.c in the SDL3 build. The game core hands every
 * 3D primitive here in world space (Q8.8) BEFORE its software projection, and
 * every 2D primitive in game screen space BEFORE it would touch the 8bpp
 * framebuffer. All palette indices are resolved to RGBA at capture time —
 * nothing 8bpp survives past this point; sdl3_video.c draws the frame as GPU
 * polygons and sprite batches.
 *
 * Ordering: 2D primitives keep their submission order in a run list. The 2D
 * runs recorded before the frame's first 3D primitive are the scene backdrop
 * (the software sky bands paint BEFORE the floor/solids), the rest is UI
 * above the 3D scene — the same painter order the software compositor used.
 * A clear_screen capture wipes the whole frame, exactly like the software
 * clear wiping the framebuffer. */

#include "sdl3_internal.h"
#include "sdl3_text.h"

#include <math.h>
#include <string.h>

#include "platform.h"
#include "hw3d.h"
#include "game_api.h"
#include "assets.h"
#include "sdl3_hires.h"
#include "sdl3_card_paths.h"

static Sdl3SceneFrame g_frame = {
    .bg_rgba = { 0.0f, 0.0f, 0.0f, 1.0f },
    .bg_runs = -1,
};

/* ---- palette → RGBA ------------------------------------------------------- */

static void pal_rgba_f(uint8_t idx, float out[4])
{
    const uint8_t *pal = waifu_fm_palette_rgb();
    out[0] = pal[idx * 3 + 0] / 255.0f;
    out[1] = pal[idx * 3 + 1] / 255.0f;
    out[2] = pal[idx * 3 + 2] / 255.0f;
    out[3] = 1.0f;
}

/* ---- camera transform ------------------------------------------------------ */

typedef struct SceneXform {
    float ex, ey, ez;
    float rx, ry, rz;   /* right */
    float ux, uy, uz;   /* up (orthonormalized) */
    float fx, fy, fz;   /* forward */
    float sx, sy;       /* focal / (W/2), focal / (H/2) */
} SceneXform;

static WaifuHw3DCamera g_xf_cam;
static SceneXform g_xf;
static int g_xf_valid = 0;

static void norm3(float *x, float *y, float *z)
{
    float len = sqrtf(*x * *x + *y * *y + *z * *z);
    if (len < 1e-6f) { *x = 0.0f; *y = 0.0f; *z = 1.0f; return; }
    *x /= len; *y /= len; *z /= len;
}

static const SceneXform *xform_for(const WaifuHw3DCamera *cam)
{
    float fx, fy, fz, rx, ry, rz, ux, uy, uz;
    if (g_xf_valid && memcmp(cam, &g_xf_cam, sizeof(*cam)) == 0) return &g_xf;

    g_xf.ex = cam->eye.x / 256.0f;
    g_xf.ey = cam->eye.y / 256.0f;
    g_xf.ez = cam->eye.z / 256.0f;
    fx = cam->target.x / 256.0f - g_xf.ex;
    fy = cam->target.y / 256.0f - g_xf.ey;
    fz = cam->target.z / 256.0f - g_xf.ez;
    norm3(&fx, &fy, &fz);
    {
        float upx = cam->up.x / 256.0f, upy = cam->up.y / 256.0f, upz = cam->up.z / 256.0f;
        rx = fy * upz - fz * upy;
        ry = fz * upx - fx * upz;
        rz = fx * upy - fy * upx;
        norm3(&rx, &ry, &rz);
    }
    ux = ry * fz - rz * fy;
    uy = rz * fx - rx * fz;
    uz = rx * fy - ry * fx;

    g_xf.fx = fx; g_xf.fy = fy; g_xf.fz = fz;
    g_xf.rx = rx; g_xf.ry = ry; g_xf.rz = rz;
    g_xf.ux = ux; g_xf.uy = uy; g_xf.uz = uz;
    g_xf.sx = (cam->focal / 256.0f) / (WAIFU_FM_WIDTH / 2.0f);
    g_xf.sy = (cam->focal / 256.0f) / (WAIFU_FM_HEIGHT / 2.0f);
    g_xf_cam = *cam;
    g_xf_valid = 1;
    return &g_xf;
}

/* Premultiplied clip position: (ndc_x*w, ndc_y*w, w). */
static void xform_point(const SceneXform *xf, const WaifuHw3DVec3 *p, float out[3])
{
    float px = p->x / 256.0f - xf->ex;
    float py = p->y / 256.0f - xf->ey;
    float pz = p->z / 256.0f - xf->ez;
    float cx = px * xf->rx + py * xf->ry + pz * xf->rz;
    float cy = px * xf->ux + py * xf->uy + pz * xf->uz;
    float cz = px * xf->fx + py * xf->fy + pz * xf->fz;
    out[0] = cx * xf->sx;
    out[1] = cy * xf->sy;
    out[2] = cz;
}

/* ---- 2D run management ----------------------------------------------------- */

static int g_run_open_kind = -1;
static int g_ui_hud = 0;        /* widescreen HUD capture bracket (full-width) */
static int g_ui_extra_w = 0;    /* set by the video layer from the canvas aspect */

static void ui_close_run(void)
{
    g_run_open_kind = -1;
}

static void ui_append(int kind, int count_added)
{
    Sdl3UiRun *run;
    int first = (kind == SDL3_UI_RUN_TRIS)  ? g_frame.ui_vert_count
              : (kind == SDL3_UI_RUN_GLYPH) ? g_frame.ui_glyph_vert_count
                                            : g_frame.ui_line_vert_count;
    if (g_run_open_kind == kind && g_frame.ui_run_count > 0 &&
        g_frame.ui_runs[g_frame.ui_run_count - 1].hud == g_ui_hud) {
        g_frame.ui_runs[g_frame.ui_run_count - 1].count += count_added;
        return;
    }
    if (g_frame.ui_run_count >= SDL3_UI_MAX_RUNS) { g_sdl3_drop_run++; return; }
    run = &g_frame.ui_runs[g_frame.ui_run_count++];
    run->kind = kind;
    run->first = first - count_added;
    run->count = count_added;
    run->hud = g_ui_hud;
    g_run_open_kind = kind;
}

/* Insert a hi-res card as its own run in the UI list so its draw order relative
   to surrounding 2D (card frame, cursor, stat text) is preserved. */
static void ui_append_hires(int draw_index)
{
    Sdl3UiRun *run;
    if (draw_index < 0) return;
    if (g_frame.ui_run_count >= SDL3_UI_MAX_RUNS) { g_sdl3_drop_run++; return; }
    run = &g_frame.ui_runs[g_frame.ui_run_count++];
    run->kind = SDL3_UI_RUN_HIRES;
    run->first = draw_index;
    run->count = 0;
    run->hud = g_ui_hud;
    g_run_open_kind = -1;   /* hi-res runs never coalesce with adjacent tris */
}

/* Same idea for a procedural impact burst: its own run, never coalesced, so
   the shader pass lands exactly where the game issued it in the 2D order. */
static void ui_append_impact(int fx_index)
{
    Sdl3UiRun *run;
    if (fx_index < 0) return;
    if (g_frame.ui_run_count >= SDL3_UI_MAX_RUNS) { g_sdl3_drop_run++; return; }
    run = &g_frame.ui_runs[g_frame.ui_run_count++];
    run->kind = SDL3_UI_RUN_IMPACT;
    run->first = fx_index;
    run->count = 0;
    run->hud = g_ui_hud;
    g_run_open_kind = -1;
}

static void ui_push_quad(float x0, float y0, float x1, float y1,
                         float u0, float v0, float u1, float v1,
                         const float rgba[4])
{
    Sdl3UiVertex *vt;
    static const int cx[6] = { 0, 1, 1, 0, 1, 0 };
    static const int cy[6] = { 0, 0, 1, 0, 1, 1 };
    int i;
    if (g_frame.ui_vert_count + 6 > SDL3_UI_MAX_VERTS) { g_sdl3_drop_vert++; return; }
    vt = &g_frame.ui_verts[g_frame.ui_vert_count];
    for (i = 0; i < 6; ++i) {
        vt[i].x = cx[i] ? x1 : x0;
        vt[i].y = cy[i] ? y1 : y0;
        vt[i].u = cx[i] ? u1 : u0;
        vt[i].v = cy[i] ? v1 : v0;
        vt[i].r = rgba[0]; vt[i].g = rgba[1]; vt[i].b = rgba[2]; vt[i].a = rgba[3];
    }
    g_frame.ui_vert_count += 6;
    ui_append(SDL3_UI_RUN_TRIS, 6);
    g_frame.has_content = 1;
}

/* Vertically graded solid quad: same run/vertex path as ui_push_quad, but the
   top and bottom edges carry different colours so a backdrop can be a smooth
   gradient instead of banded fill_rows. */
static void ui_push_quad_grad(float x0, float y0, float x1, float y1,
                              const float top_rgba[4], const float bot_rgba[4])
{
    Sdl3UiVertex *vt;
    static const int cx[6] = { 0, 1, 1, 0, 1, 0 };
    static const int cy[6] = { 0, 0, 1, 0, 1, 1 };
    int i;
    if (g_frame.ui_vert_count + 6 > SDL3_UI_MAX_VERTS) return;
    vt = &g_frame.ui_verts[g_frame.ui_vert_count];
    for (i = 0; i < 6; ++i) {
        const float *c = cy[i] ? bot_rgba : top_rgba;
        vt[i].x = cx[i] ? x1 : x0;
        vt[i].y = cy[i] ? y1 : y0;
        vt[i].u = -1.0f;
        vt[i].v = 0.0f;
        vt[i].r = c[0]; vt[i].g = c[1]; vt[i].b = c[2]; vt[i].a = c[3];
    }
    g_frame.ui_vert_count += 6;
    ui_append(SDL3_UI_RUN_TRIS, 6);
    g_frame.has_content = 1;
}

/* Horizontally graded solid quad (arena side vignette). */
static void ui_push_quad_hgrad(float x0, float y0, float x1, float y1,
                               const float left_rgba[4], const float right_rgba[4])
{
    Sdl3UiVertex *vt;
    static const int cx[6] = { 0, 1, 1, 0, 1, 0 };
    static const int cy[6] = { 0, 0, 1, 0, 1, 1 };
    int i;
    if (g_frame.ui_vert_count + 6 > SDL3_UI_MAX_VERTS) return;
    vt = &g_frame.ui_verts[g_frame.ui_vert_count];
    for (i = 0; i < 6; ++i) {
        const float *c = cx[i] ? right_rgba : left_rgba;
        vt[i].x = cx[i] ? x1 : x0;
        vt[i].y = cy[i] ? y1 : y0;
        vt[i].u = -1.0f;
        vt[i].v = 0.0f;
        vt[i].r = c[0]; vt[i].g = c[1]; vt[i].b = c[2]; vt[i].a = c[3];
    }
    g_frame.ui_vert_count += 6;
    ui_append(SDL3_UI_RUN_TRIS, 6);
    g_frame.has_content = 1;
}

static void ui_push_corner_quad(const float xy[8], const float uv[8], const float rgba[4])
{
    Sdl3UiVertex *vt;
    static const int order[6] = { 0, 1, 2, 0, 2, 3 };
    int i;
    if (g_frame.ui_vert_count + 6 > SDL3_UI_MAX_VERTS) return;
    vt = &g_frame.ui_verts[g_frame.ui_vert_count];
    for (i = 0; i < 6; ++i) {
        int c = order[i];
        vt[i].x = xy[c * 2 + 0];
        vt[i].y = xy[c * 2 + 1];
        vt[i].u = uv[c * 2 + 0];
        vt[i].v = uv[c * 2 + 1];
        vt[i].r = rgba[0]; vt[i].g = rgba[1]; vt[i].b = rgba[2]; vt[i].a = rgba[3];
    }
    g_frame.ui_vert_count += 6;
    ui_append(SDL3_UI_RUN_TRIS, 6);
    g_frame.has_content = 1;
}

static void ui_push_line(float x0, float y0, float x1, float y1, const float rgba[4])
{
    Sdl3UiVertex *vt;
    if (g_frame.ui_line_vert_count + 2 > SDL3_UI_MAX_LINE_VERTS) return;
    vt = &g_frame.ui_line_verts[g_frame.ui_line_vert_count];
    vt[0].x = x0; vt[0].y = y0; vt[0].u = -1.0f; vt[0].v = 0.0f;
    vt[1].x = x1; vt[1].y = y1; vt[1].u = -1.0f; vt[1].v = 0.0f;
    vt[0].r = vt[1].r = rgba[0];
    vt[0].g = vt[1].g = rgba[1];
    vt[0].b = vt[1].b = rgba[2];
    vt[0].a = vt[1].a = rgba[3];
    g_frame.ui_line_vert_count += 2;
    ui_append(SDL3_UI_RUN_LINES, 2);
    g_frame.has_content = 1;
}

/* One text glyph quad into the glyph vertex stream (uv normalized into the
   persistent atlas). Coalesced into the current glyph run so a whole string is
   one draw. */
static void ui_push_glyph_quad(float x0, float y0, float x1, float y1,
                               float u0, float v0, float u1, float v1,
                               const float rgba[4])
{
    Sdl3UiVertex *vt;
    static const int cx[6] = { 0, 1, 1, 0, 1, 0 };
    static const int cy[6] = { 0, 0, 1, 0, 1, 1 };
    int i;
    if (g_frame.ui_glyph_vert_count + 6 > SDL3_UI_MAX_GLYPH_VERTS) return;
    vt = &g_frame.ui_glyph_verts[g_frame.ui_glyph_vert_count];
    for (i = 0; i < 6; ++i) {
        vt[i].x = cx[i] ? x1 : x0;
        vt[i].y = cy[i] ? y1 : y0;
        vt[i].u = cx[i] ? u1 : u0;
        vt[i].v = cy[i] ? v1 : v0;
        vt[i].r = rgba[0]; vt[i].g = rgba[1]; vt[i].b = rgba[2]; vt[i].a = rgba[3];
    }
    g_frame.ui_glyph_vert_count += 6;
    ui_append(SDL3_UI_RUN_GLYPH, 6);
    g_frame.has_content = 1;
}

/* Per-character text seam (src/engine/platform.h). SDL3 renders each glyph from
   the FreeType atlas as a crisp quad (drop shadow + fill, matching the bitmap
   font's (x+1,y+1)/(x,y) two-pass look). Returns 1 when handled so the common
   text primitives skip the 8x8 bitmap blit; returns 0 only when the font could
   not be loaded, so the caller falls back to the bitmap font. */
static float g_glyph_scale = 1.0f;

void waifu_hw2d_text_scale(int percent)
{
    if (percent < 10) percent = 10;
    if (percent > 400) percent = 400;
    g_glyph_scale = (float)percent / 100.0f;
}

int waifu_platform_glyph(int x, int y, int cell_w, unsigned char ch, uint8_t fg, uint8_t shadow)
{
    WaifuGlyphInfo gi;
    float fg_rgba[4], sh_rgba[4];
    float sx, sy, ox, x0, y0, x1, y1, sh;
    if (!waifu_sdl3_text_ready()) return 0;
    if (!waifu_sdl3_glyph_info(ch, &gi)) return 1;   /* space/blank: advance only */

    /* Glyph metrics are baked for the 8 px cell; the compact HUD face steps 7.
       Condense to the caller's cell so both keep the same fit, then centre the
       advance box in it — a glyph left at its own bearing leaves the slack at
       the right of every cell and the line reads ragged. */
    sy = g_glyph_scale;
    sx = (float)cell_w / 8.0f * sy;
    ox = ((float)cell_w - gi.adv * sx) * 0.5f;

    x0 = (float)x + ox + gi.dx * sx;
    x1 = x0 + gi.dw * sx;
    /* A scaled glyph shrinks about the cell's own middle, so a shrunk label
       stays centred on the plate the layout drew for it. */
    y0 = (float)y + 4.0f + (gi.dy - 4.0f) * sy;
    y1 = y0 + gi.dh * sy;
    /* Drop shadow offset scales with the glyph so it stays a shadow and not an
       outline at large canvas scales. */
    sh = 1.0f;

    pal_rgba_f(shadow, sh_rgba);
    pal_rgba_f(fg, fg_rgba);
    ui_push_glyph_quad(x0 + sh, y0 + sh, x1 + sh, y1 + sh,
                       gi.u0, gi.v0, gi.u1, gi.v1, sh_rgba);
    ui_push_glyph_quad(x0, y0, x1, y1, gi.u0, gi.v0, gi.u1, gi.v1, fg_rgba);
    return 1;
}

/* ---------------------------------------------------------------------------
 * Direct-attack impact burst (src/engine/hw3d.h).
 *
 * The burst itself is not geometry: it is one whole-viewport shader pass
 * (shaders/impact.frag) queued as its own UI run, so the attacker card
 * captured before it stays underneath and the damage readout captured after it
 * stays on top. The choreography's progress and damage tier cross the seam;
 * the look lives entirely in the shader.
 * ------------------------------------------------------------------------- */
int waifu_hw2d_impact_fx(int cx, int cy, int t_q8, int dir, int intensity_q8)
{
    Sdl3ImpactFx *fx;
    if (g_frame.impact_fx_count >= SDL3_MAX_IMPACT_FX) return 1;  /* drawn once already */
    fx = &g_frame.impact_fx[g_frame.impact_fx_count];
    fx->cx = (float)cx;
    fx->cy = (float)cy;
    fx->t = (float)t_q8 / 256.0f;
    fx->dir = (dir < 0) ? -1.0f : 1.0f;
    fx->intensity = (float)(intensity_q8 < 0 ? 0 :
                            intensity_q8 > 256 ? 256 : intensity_q8) / 256.0f;
    ui_append_impact(g_frame.impact_fx_count);
    g_frame.impact_fx_count++;
    g_frame.has_content = 1;
    return 1;
}

/* The damage readout over that burst. The glyph atlas is scalable, so this is
 * laid out directly in game pixels rather than through the 8 px text cell:
 * a warm outer glow (offset copies at low alpha), a dark outline, then the
 * fill. Cap height drives the size, so `cap_px` reads as the number's actual
 * height on screen. */
int waifu_hw2d_impact_text(int cx, int cy, int cap_px, const char *s,
                           int glow_q8, int alpha_q8)
{
    /* Ring of unit offsets for the glow and the outline passes. */
    static const float ring[8][2] = {
        { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 },
        { 0.7f, 0.7f }, { -0.7f, 0.7f }, { 0.7f, -0.7f }, { -0.7f, -0.7f }
    };
    const char *p;
    float scale, adv, total, pen, top;
    float glow, alpha;
    int i;

    if (!s || !*s) return 1;
    if (!waifu_sdl3_text_ready()) return 0;   /* no scalable glyphs: caller falls back */

    /* The atlas metrics are baked for the 8 px cell, in which a capital is
       CELL_CAP tall; scale so the caller's cap height is what it asks for. */
    scale = (float)cap_px / 6.6f;
    adv = waifu_sdl3_glyph_advance() * scale;
    if (adv <= 0.0f) adv = 8.0f * scale;

    total = 0.0f;
    for (p = s; *p; ++p) total += adv;
    pen = (float)cx - total * 0.5f;
    top = (float)cy - 4.0f * scale;   /* the 8 px cell's middle on the centre */

    glow = (float)glow_q8 / 256.0f;
    alpha = (float)alpha_q8 / 256.0f;
    if (alpha <= 0.0f) return 1;
    if (alpha > 1.0f) alpha = 1.0f;

    /* Three passes over the whole string so the outline of one glyph never
       lands on top of the fill of the one before it. */
    for (i = 0; i < 3; ++i) {
        float x = pen;
        for (p = s; *p; ++p, x += adv) {
            WaifuGlyphInfo gi;
            float x0, y0, x1, y1;
            int k;
            if (!waifu_sdl3_glyph_info((unsigned char)*p, &gi)) continue;
            x0 = x + (adv - gi.adv * scale) * 0.5f + gi.dx * scale;
            x1 = x0 + gi.dw * scale;
            y0 = top + gi.dy * scale;
            y1 = y0 + gi.dh * scale;
            if (i == 0) {
                /* Outer glow: wide, warm, additive-looking low-alpha copies. */
                float c[4] = { 1.0f, 0.42f, 0.06f, 0.0f };
                float spread = (float)cap_px * 0.16f;
                c[3] = 0.13f * glow * alpha;
                if (c[3] <= 0.0f) continue;
                for (k = 0; k < 8; ++k)
                    ui_push_glyph_quad(x0 + ring[k][0] * spread, y0 + ring[k][1] * spread,
                                       x1 + ring[k][0] * spread, y1 + ring[k][1] * spread,
                                       gi.u0, gi.v0, gi.u1, gi.v1, c);
            } else if (i == 1) {
                /* Dark outline: the readout sits on the still-hot core, and a
                   warm outline vanished into it. */
                float c[4] = { 0.06f, 0.01f, 0.02f, alpha };
                float w = (float)cap_px * 0.075f;
                if (w < 1.0f) w = 1.0f;
                for (k = 0; k < 8; ++k)
                    ui_push_glyph_quad(x0 + ring[k][0] * w, y0 + ring[k][1] * w,
                                       x1 + ring[k][0] * w, y1 + ring[k][1] * w,
                                       gi.u0, gi.v0, gi.u1, gi.v1, c);
            } else {
                /* Fill: pale gold at the top falling to amber, so the number
                   reads as lit from above rather than as flat paint. */
                float hi[4] = { 1.00f, 0.97f, 0.78f, alpha };
                float lo[4] = { 1.00f, 0.72f, 0.13f, alpha };
                float mid = (y0 + y1) * 0.5f;
                float vm = (gi.v0 + gi.v1) * 0.5f;
                ui_push_glyph_quad(x0, y0, x1, mid, gi.u0, gi.v0, gi.u1, vm, hi);
                ui_push_glyph_quad(x0, mid, x1, y1, gi.u0, vm, gi.u1, gi.v1, lo);
            }
        }
    }
    return 1;
}

/* First 3D primitive of the frame: everything 2D so far is scene backdrop. */
static void mark_3d(void)
{
    if (g_frame.bg_runs < 0) {
        ui_close_run();
        g_frame.bg_runs = g_frame.ui_run_count;
    }
    g_frame.has_content = 1;
}

/* ---- tile atlas (RGBA conversion) ------------------------------------------ */

static const uint8_t *g_tile_atlas_src = 0;
static uint8_t g_tile_pal_cache[768];
static int g_tiles_converted = 0;

static void ensure_tiles_converted(void)
{
    const uint8_t *pal = waifu_fm_palette_rgb();
    int t, i;
    if (!g_tile_atlas_src || g_frame.tile_count <= 0) return;
    if (g_tiles_converted && memcmp(pal, g_tile_pal_cache, 768) == 0) return;
    for (t = 0; t < g_frame.tile_count; ++t) {
        const uint8_t *src = g_tile_atlas_src + (size_t)t * 32 * 32;
        uint32_t *dst = g_frame.tile_atlas_rgba + (size_t)t * 32 * 32;
        for (i = 0; i < 32 * 32; ++i) {
            uint8_t idx = src[i];
            dst[i] = (uint32_t)pal[idx * 3 + 0] |
                     ((uint32_t)pal[idx * 3 + 1] << 8) |
                     ((uint32_t)pal[idx * 3 + 2] << 16) |
                     0xFF000000u;
        }
    }
    memcpy(g_tile_pal_cache, pal, 768);
    g_tiles_converted = 1;
    g_frame.tile_atlas_serial++;
}

/* ---- image atlas (per-frame streaming, RGBA) -------------------------------- */

typedef struct ImageEntry {
    const uint8_t *pixels;
    const uint8_t *mask;
    int w, h;
    int colorkey0;
    int x, y;
} ImageEntry;

/* Capture-limit diagnostics: any of these dropping a primitive shows up as a
   piece of the frame silently missing (the classic symptom is a card whose
   drop shadow draws but whose art does not, because the shadow is a rect and
   the art is an atlas blit). WAIFU_SDL3_DEBUG reports them. */
int g_sdl3_drop_atlas, g_sdl3_drop_run, g_sdl3_drop_vert, g_sdl3_drop_hires;

static ImageEntry g_image_entries[SDL3_IMAGE_MAX_ENTRIES];
static int g_image_entry_count = 0;
static int g_shelf_x = 0, g_shelf_y = 0, g_shelf_h = 0;

static const ImageEntry *image_atlas_add(const uint8_t *pixels, const uint8_t *mask,
                                         int w, int h, int colorkey0)
{
    const uint8_t *pal = waifu_fm_palette_rgb();
    ImageEntry *e;
    int i, row, col;
    for (i = 0; i < g_image_entry_count; ++i) {
        e = &g_image_entries[i];
        if (e->pixels == pixels && e->mask == mask && e->w == w && e->h == h &&
            e->colorkey0 == colorkey0)
            return e;
    }
    if (g_image_entry_count >= SDL3_IMAGE_MAX_ENTRIES) { g_sdl3_drop_atlas++; return 0; }
    if (w > SDL3_IMAGE_ATLAS_W || h > SDL3_IMAGE_ATLAS_H) { g_sdl3_drop_atlas++; return 0; }
    if (g_shelf_x + w > SDL3_IMAGE_ATLAS_W) {
        g_shelf_y += g_shelf_h;
        g_shelf_x = 0;
        g_shelf_h = 0;
    }
    if (g_shelf_y + h > SDL3_IMAGE_ATLAS_H) { g_sdl3_drop_atlas++; return 0; }
    e = &g_image_entries[g_image_entry_count++];
    e->pixels = pixels;
    e->mask = mask;
    e->w = w;
    e->h = h;
    e->colorkey0 = colorkey0;
    e->x = g_shelf_x;
    e->y = g_shelf_y;
    for (row = 0; row < h; ++row) {
        const uint8_t *srow = pixels + (size_t)row * w;
        const uint8_t *mrow = mask ? mask + (size_t)row * w : 0;
        uint32_t *drow = g_frame.image_atlas + (size_t)(e->y + row) * SDL3_IMAGE_ATLAS_W + e->x;
        for (col = 0; col < w; ++col) {
            uint8_t idx = srow[col];
            int opaque = mrow ? (mrow[col] != 0) : (!colorkey0 || idx != 0);
            drow[col] = opaque
                ? ((uint32_t)pal[idx * 3 + 0] |
                   ((uint32_t)pal[idx * 3 + 1] << 8) |
                   ((uint32_t)pal[idx * 3 + 2] << 16) |
                   0xFF000000u)
                : 0u;
        }
    }
    g_shelf_x += w;
    if (h > g_shelf_h) g_shelf_h = h;
    if (g_shelf_y + g_shelf_h > g_frame.image_atlas_used_h)
        g_frame.image_atlas_used_h = g_shelf_y + g_shelf_h;
    return e;
}

/* Same shelf allocator, but for an image that is already true colour (the
   effects that have no palette form). The content changes every frame, so
   unlike image_atlas_add there is nothing to look up: each call takes fresh
   shelf space in that frame's atlas. */
static const ImageEntry *image_atlas_add_rgba(const uint32_t *rgba, int w, int h)
{
    ImageEntry *e;
    int row;
    if (g_image_entry_count >= SDL3_IMAGE_MAX_ENTRIES) { g_sdl3_drop_atlas++; return 0; }
    if (w > SDL3_IMAGE_ATLAS_W || h > SDL3_IMAGE_ATLAS_H) { g_sdl3_drop_atlas++; return 0; }
    if (g_shelf_x + w > SDL3_IMAGE_ATLAS_W) {
        g_shelf_y += g_shelf_h;
        g_shelf_x = 0;
        g_shelf_h = 0;
    }
    if (g_shelf_y + h > SDL3_IMAGE_ATLAS_H) { g_sdl3_drop_atlas++; return 0; }
    e = &g_image_entries[g_image_entry_count++];
    e->pixels = NULL;
    e->mask = NULL;
    e->w = w;
    e->h = h;
    e->colorkey0 = 0;
    e->x = g_shelf_x;
    e->y = g_shelf_y;
    for (row = 0; row < h; ++row) {
        memcpy(g_frame.image_atlas + (size_t)(e->y + row) * SDL3_IMAGE_ATLAS_W + e->x,
               rgba + (size_t)row * w, (size_t)w * sizeof(uint32_t));
    }
    g_shelf_x += w;
    if (h > g_shelf_h) g_shelf_h = h;
    if (g_shelf_y + g_shelf_h > g_frame.image_atlas_used_h)
        g_frame.image_atlas_used_h = g_shelf_y + g_shelf_h;
    return e;
}

int waifu_sdl3_push_rgba_image(const uint32_t *rgba, int sw, int sh,
                               int dx, int dy, int dw, int dh)
{
    static const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    const ImageEntry *e;
    if (!rgba || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return 1;
    e = image_atlas_add_rgba(rgba, sw, sh);
    if (!e) return 0;
    ui_push_quad((float)dx, (float)dy, (float)(dx + dw), (float)(dy + dh),
                 (float)e->x, (float)e->y,
                 (float)(e->x + sw), (float)(e->y + sh), white);
    g_frame.has_content = 1;
    return 1;
}

/* ---- hw3d seam (3D scene) --------------------------------------------------- */

int waifu_hw3d_present(void)
{
    return 1;
}

void waifu_hw3d_set_texture_atlas(const void *atlas, int tile_count)
{
    g_tile_atlas_src = (const uint8_t *)atlas;
    if (tile_count > SDL3_TILE_MAX_COUNT) tile_count = SDL3_TILE_MAX_COUNT;
    g_frame.tile_count = tile_count;
    g_tiles_converted = 0;
}

/* ---- hi-res card art (PC) --------------------------------------------------
 * Board/hand/detail card draws arrive here as 8bpp pixel pointers. On the SDL3
 * build we map the game's stable per-card face/big-art pointers to a card id and
 * substitute the full-resolution source art (sdl3_hires + per-card GPU texture
 * in the video layer), keeping the console 8bpp path for everything else. No
 * common-code change: the map is keyed on the pointers the game already hands
 * us. */

typedef struct CardPtr { const uint8_t *ptr; int card_id; int kind; } CardPtr;
static CardPtr g_card_ptrs[256 * 2];
static int g_card_ptr_count = 0;
static int g_card_ptrs_built = 0;

static void build_card_ptr_map(void)
{
    int n, id;
    g_card_ptr_count = 0;
    g_card_ptrs_built = 1;
    n = waifu_sdl3_hires_card_count();
    for (id = 0; id < n && g_card_ptr_count + 2 <= (int)(sizeof(g_card_ptrs)/sizeof(g_card_ptrs[0])); ++id) {
        const uint8_t *f, *b;
        if (!waifu_sdl3_hires_has_card(id)) continue;
        f = waifu_assets_card_face(id);
        b = waifu_assets_card_big_art(id);
        if (f) { g_card_ptrs[g_card_ptr_count].ptr = f; g_card_ptrs[g_card_ptr_count].card_id = id; g_card_ptrs[g_card_ptr_count].kind = WAIFU_HIRES_FACE; ++g_card_ptr_count; }
        if (b) { g_card_ptrs[g_card_ptr_count].ptr = b; g_card_ptrs[g_card_ptr_count].card_id = id; g_card_ptrs[g_card_ptr_count].kind = WAIFU_HIRES_BIG; ++g_card_ptr_count; }
    }
}

/* If `pix` is the full-screen title/ending image, return 1 (title) / 2 (ending)
   so it can be drawn from the 16:9 source across the full canvas. */
static int fullimage_lookup(const uint8_t *pix)
{
    if (!pix) return 0;
    if (pix == waifu_assets_title_screen_img()) return 1;
    if (pix == waifu_assets_ending_screen_img()) return 2;
    return 0;
}

/* If `pix` is a known card face/big-art buffer, return 1 and set the card id + kind. */
static int card_lookup(const uint8_t *pix, int *card_id, int *kind)
{
    int i;
    if (!pix) return 0;
    if (!g_card_ptrs_built) build_card_ptr_map();
    for (i = 0; i < g_card_ptr_count; ++i) {
        if (g_card_ptrs[i].ptr == pix) {
            *card_id = g_card_ptrs[i].card_id;
            *kind = g_card_ptrs[i].kind;
            return 1;
        }
    }
    return 0;
}

/* Emit one hi-res card quad (6 verts, two triangles) from clip-space corners
   wound (0,0),(1,0),(1,1),(0,1). Returns the hires_draws index, or -1 if full. */
static int push_hires_quad(const float p[4][3], int card_id, int kind, int group, float gray)
{
    static const int order[6] = { 0, 1, 2, 0, 2, 3 };
    static const float uv[4][2] = { {0,0}, {1,0}, {1,1}, {0,1} };
    Sdl3HiresDraw *d;
    int i;
    if (g_frame.hires_draw_count >= SDL3_HIRES_MAX_DRAWS) { g_sdl3_drop_hires++; return -1; }
    d = &g_frame.hires_draws[g_frame.hires_draw_count];
    d->first_vertex = g_frame.hires_draw_count * 6;
    d->card_id = card_id;
    d->kind = kind;
    d->group = group;
    for (i = 0; i < 6; ++i) {
        int c = order[i];
        Sdl3ImageVertex *vt = &g_frame.hires_verts[d->first_vertex + i];
        vt->x = p[c][0]; vt->y = p[c][1]; vt->w = p[c][2];
        vt->u = uv[c][0]; vt->v = uv[c][1];
        vt->gray = gray;
    }
    return g_frame.hires_draw_count++;
}

/* Pull a clip-space quad a hair toward the eye. Scaling x, y and w together
   leaves the projected position identical (x and y are premultiplied by w) but
   shortens the depth, which is what decides a coplanar decal: the art window
   drawn over a card front frame is exactly coplanar with it, and an exact depth
   tie under LESS_OR_EQUAL is not reliably won by the later draw -- on the board
   the art vanished behind the frame entirely. */
#define CLIP_DECAL_BIAS 0.999f

static void clip_bias_toward_eye(float p[4][3])
{
    int i;
    for (i = 0; i < 4; ++i) {
        p[i][0] *= CLIP_DECAL_BIAS;
        p[i][1] *= CLIP_DECAL_BIAS;
        p[i][2] *= CLIP_DECAL_BIAS;
    }
}

/* Clip-space corners of a game-screen rect, wound like push_hires_quad expects
   ((0,0),(1,0),(1,1),(0,1)). hx/hy are the half-extents of the UI transform's
   screen space (widened in the HUD bracket). */
static void ui_rect_clip(float x0, float y0, float x1, float y1,
                         float hx, float hy, float p[4][3])
{
    p[0][0] = x0 / hx - 1.0f; p[0][1] = 1.0f - y0 / hy; p[0][2] = 1.0f;
    p[1][0] = x1 / hx - 1.0f; p[1][1] = 1.0f - y0 / hy; p[1][2] = 1.0f;
    p[2][0] = x1 / hx - 1.0f; p[2][1] = 1.0f - y1 / hy; p[2][2] = 1.0f;
    p[3][0] = x0 / hx - 1.0f; p[3][1] = 1.0f - y1 / hy; p[3][2] = 1.0f;
}

/* The art window inside the full-resolution card FRONT frame, from the
   template's own reference file (see tools/gen_sdl3_hires_paths.py). The frame
   texture covers the whole card and its window is empty; this is where the
   card's art goes. */
#define CARD_ART_U0 WAIFU_SDL3_CARD_ART_U0
#define CARD_ART_U1 WAIFU_SDL3_CARD_ART_U1
#define CARD_ART_V0 WAIFU_SDL3_CARD_ART_V0
#define CARD_ART_V1 WAIFU_SDL3_CARD_ART_V1

/* The same window in the baked 8bpp face (gen_assets pastes the 30x30 art at
   (4,9) of the 38x54 card). Only the support sigil is still read from there --
   support cards have no high-resolution source art of their own. */
#define FACE_ART_U0 (4.0f / 38.0f)
#define FACE_ART_U1 (34.0f / 38.0f)
#define FACE_ART_V0 (9.0f / 54.0f)
#define FACE_ART_V1 (39.0f / 54.0f)

/* The frame's other two windows: the top band that carries the level ankhs and
   the bottom band that carries ATK/DEF (or a support card's class word). The
   console bakes both into the 8bpp face, which the full-resolution frame covers
   over, so each comes back as its own generated strip texture. */
#define CARD_STAR_U0 WAIFU_SDL3_CARD_STAR_U0
#define CARD_STAR_U1 WAIFU_SDL3_CARD_STAR_U1
#define CARD_STAR_V0 WAIFU_SDL3_CARD_STAR_V0
#define CARD_STAR_V1 WAIFU_SDL3_CARD_STAR_V1
#define CARD_STAT_U0 WAIFU_SDL3_CARD_STAT_U0
#define CARD_STAT_U1 WAIFU_SDL3_CARD_STAT_U1
#define CARD_STAT_V0 WAIFU_SDL3_CARD_STAT_V0
#define CARD_STAT_V1 WAIFU_SDL3_CARD_STAT_V1

/* Which front frame the next support-card face draw wants; monsters are known
   from the card id, but every Spell and Trap shares ONE 8bpp face buffer, so the
   game names the class through the hw2d seam just before drawing it. */
static int g_card_frame_hint = WAIFU_CARD_FRAME_SPELL;
static int g_card_label_hint = WAIFU_CARD_LABEL_SUPPORT;

void waifu_hw2d_card_frame_hint(int frame, int label)
{
    if (frame < 0 || frame >= WAIFU_SDL3_CARD_FRAME_SRC_COUNT) frame = WAIFU_CARD_FRAME_MONSTER;
    if (label < 0 || label > 2) label = WAIFU_CARD_LABEL_SUPPORT;
    g_card_frame_hint = frame;
    g_card_label_hint = label;
}

/* Bilinear point on the card quad (corners wound (0,0),(1,0),(1,1),(0,1)) at
   fractional (u, w); used to carve the art-window sub-quad from a board card. */
static WaifuHw3DVec3 card_bilerp(const WaifuHw3DVec3 v[4], float u, float w)
{
    WaifuHw3DVec3 r;
    float tx = (float)v[0].x + ((float)v[1].x - (float)v[0].x) * u;
    float ty = (float)v[0].y + ((float)v[1].y - (float)v[0].y) * u;
    float tz = (float)v[0].z + ((float)v[1].z - (float)v[0].z) * u;
    float bx = (float)v[3].x + ((float)v[2].x - (float)v[3].x) * u;
    float by = (float)v[3].y + ((float)v[2].y - (float)v[3].y) * u;
    float bz = (float)v[3].z + ((float)v[2].z - (float)v[3].z) * u;
    r.x = (int32_t)(tx + (bx - tx) * w);
    r.y = (int32_t)(ty + (by - ty) * w);
    r.z = (int32_t)(tz + (bz - tz) * w);
    return r;
}

static void push_scene_vertex(const float pos[3], float u, float v, float tile)
{
    Sdl3SceneVertex *vt;
    if (g_frame.scene_vert_count >= SDL3_SCENE_MAX_SCENE_VERTS) return;
    vt = &g_frame.scene_verts[g_frame.scene_vert_count++];
    vt->x = pos[0]; vt->y = pos[1]; vt->w = pos[2];
    vt->u = u; vt->v = v; vt->tile = tile;
}

static void push_line3d_raw(const float a[3], const float b[3], uint8_t color)
{
    /* Bias lines fractionally toward the camera so outlines on a polygon's
       own plane (card rims, pyramid edges) win the depth test the way the
       software painter order made them win. Scaling all premultiplied
       components keeps the screen position identical. */
    const float bias = 0.998f;
    float rgba[4];
    Sdl3LineVertex *vt;
    if (g_frame.line_vert_count + 2 > SDL3_SCENE_MAX_LINE_VERTS) return;
    pal_rgba_f(color, rgba);
    vt = &g_frame.line_verts[g_frame.line_vert_count];
    vt[0].x = a[0] * bias; vt[0].y = a[1] * bias; vt[0].w = a[2] * bias;
    vt[1].x = b[0] * bias; vt[1].y = b[1] * bias; vt[1].w = b[2] * bias;
    vt[0].r = vt[1].r = rgba[0];
    vt[0].g = vt[1].g = rgba[1];
    vt[0].b = vt[1].b = rgba[2];
    vt[0].a = vt[1].a = rgba[3];
    g_frame.line_vert_count += 2;
}

/* The duel board's two checker squares (atlas tiles 1 and 5). On the PC build
   they are not the 32x32 atlas downscale at all: each cell maps one whole
   full-resolution sandstone source (sdl3_hires board-tile textures, mipmapped),
   which is why they leave the scene pipeline for the hi-res pass. Any other
   tile index is ordinary atlas geometry. */
#define BOARD_TILE_LIGHT 1
#define BOARD_TILE_DARK  5

static int board_tile_id(int tile)
{
    if (tile == BOARD_TILE_LIGHT) return 0;
    if (tile == BOARD_TILE_DARK) return 1;
    return -1;
}

/* The two generated band strips over one card. `card_id` >= 0 gives a monster
   its level ankhs and its ATK/DEF; otherwise `label` names the support word the
   bottom band carries and there is no ankh row. Shared by the 2D and the 3D
   card paths through a caller-supplied quad emitter, so a hand card and a board
   card place the bands identically. */
static int card_band_level(int card_id)
{
    return (card_id >= 0) ? waifu_sdl3_hires_card_level(card_id) : 0;
}

static void ui_push_card_bands(float dx, float dy, float dw, float dh,
                               float hx, float hy, int card_id, int label, float g)
{
    float p[4][3];
    int level = card_band_level(card_id);
    int idx;
    if (level > 0) {
        ui_rect_clip(dx + dw * CARD_STAR_U0, dy + dh * CARD_STAR_V0,
                     dx + dw * CARD_STAR_U1, dy + dh * CARD_STAR_V1, hx, hy, p);
        idx = push_hires_quad(p, level - 1, WAIFU_HIRES_STARS, 1 /* 2D */, g);
        if (idx >= 0) ui_append_hires(idx);
    }
    ui_rect_clip(dx + dw * CARD_STAT_U0, dy + dh * CARD_STAT_V0,
                 dx + dw * CARD_STAT_U1, dy + dh * CARD_STAT_V1, hx, hy, p);
    idx = (card_id >= 0) ? push_hires_quad(p, card_id, WAIFU_HIRES_STATS, 1, g)
                         : push_hires_quad(p, label, WAIFU_HIRES_LABEL, 1, g);
    if (idx >= 0) ui_append_hires(idx);
}

static void scene_push_card_bands(const SceneXform *xf, const WaifuHw3DVec3 v[4],
                                  int card_id, int label, float g)
{
    WaifuHw3DVec3 q[4];
    float p[4][3];
    int level = card_band_level(card_id);
    int i;
    if (level > 0) {
        q[0] = card_bilerp(v, CARD_STAR_U0, CARD_STAR_V0);
        q[1] = card_bilerp(v, CARD_STAR_U1, CARD_STAR_V0);
        q[2] = card_bilerp(v, CARD_STAR_U1, CARD_STAR_V1);
        q[3] = card_bilerp(v, CARD_STAR_U0, CARD_STAR_V1);
        for (i = 0; i < 4; ++i) xform_point(xf, &q[i], p[i]);
        clip_bias_toward_eye(p);
        push_hires_quad(p, level - 1, WAIFU_HIRES_STARS, 0 /* 3D */, g);
    }
    q[0] = card_bilerp(v, CARD_STAT_U0, CARD_STAT_V0);
    q[1] = card_bilerp(v, CARD_STAT_U1, CARD_STAT_V0);
    q[2] = card_bilerp(v, CARD_STAT_U1, CARD_STAT_V1);
    q[3] = card_bilerp(v, CARD_STAT_U0, CARD_STAT_V1);
    for (i = 0; i < 4; ++i) xform_point(xf, &q[i], p[i]);
    clip_bias_toward_eye(p);
    if (card_id >= 0) push_hires_quad(p, card_id, WAIFU_HIRES_STATS, 0, g);
    else push_hires_quad(p, label, WAIFU_HIRES_LABEL, 0, g);
}

int waifu_hw3d_quad(const WaifuHw3DCamera *cam, const WaifuHw3DVec3 v[4], int tile)
{
    const SceneXform *xf = xform_for(cam);
    float p[4][3];
    float t;
    int i;
    int board_id;
    mark_3d();
    ensure_tiles_converted();
    if (tile < 0) tile = 0;
    if (g_frame.tile_count > 0 && tile >= g_frame.tile_count) tile = g_frame.tile_count - 1;
    t = (float)tile;
    for (i = 0; i < 4; ++i) xform_point(xf, &v[i], p[i]);
    board_id = board_tile_id(tile);
    if (board_id >= 0 &&
        push_hires_quad(p, board_id, WAIFU_HIRES_BOARD_TILE, 0 /* 3D */, 0.0f) >= 0) {
        return 1;
    }
    push_scene_vertex(p[0], 0.0f, 0.0f, t);
    push_scene_vertex(p[1], 1.0f, 0.0f, t);
    push_scene_vertex(p[2], 1.0f, 1.0f, t);
    push_scene_vertex(p[0], 0.0f, 0.0f, t);
    push_scene_vertex(p[2], 1.0f, 1.0f, t);
    push_scene_vertex(p[3], 0.0f, 1.0f, t);
    return 1;
}

int waifu_hw3d_tri(const WaifuHw3DCamera *cam, const WaifuHw3DVec3 v[3], int tile,
                   int flip_u, int rows, int cols, uint8_t edge_color)
{
    const SceneXform *xf = xform_for(cam);
    float p[3][3];
    float u0, u1, uapex, t;
    int i;
    mark_3d();
    ensure_tiles_converted();
    if (tile < 0) tile = 0;
    if (g_frame.tile_count > 0 && tile >= g_frame.tile_count) tile = g_frame.tile_count - 1;
    t = (float)tile;
    for (i = 0; i < 3; ++i) xform_point(xf, &v[i], p[i]);
    /* Software UV layout: base corners at v=0 (u 0 / cols, swapped by flip_u),
       apex at (cols/2, rows). */
    u0 = flip_u ? (float)cols : 0.0f;
    u1 = flip_u ? 0.0f : (float)cols;
    uapex = (float)cols * 0.5f;
    push_scene_vertex(p[0], u0, 0.0f, t);
    push_scene_vertex(p[1], u1, 0.0f, t);
    push_scene_vertex(p[2], uapex, (float)rows, t);
    /* The software renderer outlines each face. */
    push_line3d_raw(p[0], p[1], edge_color);
    push_line3d_raw(p[1], p[2], edge_color);
    push_line3d_raw(p[2], p[0], edge_color);
    return 1;
}

int waifu_hw3d_floor(const WaifuHw3DCamera *cam, int32_t floor_y,
                     int tile_a, int tile_b, int32_t tile_size)
{
    /* Record the floor plane as an environment parameter set for the fullscreen
       ray-plane pass (sdl3_video.c). Instead of triangulating a giant
       camera-centred quad — which produces degenerate grazing vertices at the
       near plane and never rasterizes cleanly — the GPU casts a ray per pixel
       from the eye through that pixel, intersects the plane y = floor_y, and
       samples the checkerboard at the hit point. That yields a truly infinite,
       glitch-free floor that recedes exactly to the horizon. */
    const SceneXform *xf;
    Sdl3Env *env = &g_frame.env;

    mark_3d();
    ensure_tiles_converted();
    xf = xform_for(cam);

    env->has_floor = 1;
    env->floor_y = floor_y / 256.0f;
    env->tile_a = (float)(tile_a < 0 ? 0 : tile_a);
    env->tile_b = (float)(tile_b < 0 ? 0 : tile_b);
    env->texels_per_unit = 32.0f / (tile_size > 0 ? tile_size / 256.0f : 1.0f);
    env->eye[0] = xf->ex; env->eye[1] = xf->ey; env->eye[2] = xf->ez;
    env->right[0] = xf->rx; env->right[1] = xf->ry; env->right[2] = xf->rz;
    env->up[0] = xf->ux; env->up[1] = xf->uy; env->up[2] = xf->uz;
    env->fwd[0] = xf->fx; env->fwd[1] = xf->fy; env->fwd[2] = xf->fz;
    env->sx = xf->sx;
    env->sy = xf->sy;
    return 1;
}

int waifu_hw3d_image_quad(const WaifuHw3DCamera *cam, const WaifuHw3DVec3 v[4],
                          const uint8_t *pixels, int w, int h,
                          int gray, uint8_t edge_color)
{
    const SceneXform *xf;
    const ImageEntry *e;
    float p[4][3];
    float uv[4][2];
    float g = gray ? 1.0f : 0.0f;
    int i;

    if (!pixels || w <= 0 || h <= 0) return 0;
    mark_3d();

    /* PC card back: one full-resolution texture covers the WHOLE card (the art
       carries its own gold frame), so it replaces the face rather than
       overlaying an art window. It is coplanar with the 8bpp card drawn below
       and the depth test is LESS_OR_EQUAL, so the later hi-res pass wins; if
       the source image is missing the console art simply stays. */
    if (pixels == waifu_assets_card_back()) {
        float bp[4][3];
        xf = xform_for(cam);
        for (i = 0; i < 4; ++i) xform_point(xf, &v[i], bp[i]);
        push_hires_quad(bp, 0, WAIFU_HIRES_BACK, 0 /* 3D */, g);
    }

    /* Full-resolution card front (PC): the board card gets the monster front
       frame across its whole face and the hi-res art inside that frame's art
       window. Both go through the group-0 hi-res pass, which draws after the
       8bpp faces (image_verts) below -- so the baked 8bpp face is what shows if
       either source image is missing. Support cards keep their 8bpp face here:
       their only art is the baked sigil, which cannot draw over the frame in
       this pass, and a support card only reaches a monster zone through stale
       state anyway. */
    {
        int card_id, kind;
        if (card_lookup(pixels, &card_id, &kind)) {
            WaifuHw3DVec3 aw[4];
            float ap[4][3];
            xf = xform_for(cam);
            for (i = 0; i < 4; ++i) xform_point(xf, &v[i], ap[i]);
            push_hires_quad(ap, WAIFU_CARD_FRAME_MONSTER, WAIFU_HIRES_FRAME, 0 /* 3D */, g);
            aw[0] = card_bilerp(v, CARD_ART_U0, CARD_ART_V0);
            aw[1] = card_bilerp(v, CARD_ART_U1, CARD_ART_V0);
            aw[2] = card_bilerp(v, CARD_ART_U1, CARD_ART_V1);
            aw[3] = card_bilerp(v, CARD_ART_U0, CARD_ART_V1);
            for (i = 0; i < 4; ++i) xform_point(xf, &aw[i], ap[i]);
            clip_bias_toward_eye(ap);
            push_hires_quad(ap, card_id, WAIFU_HIRES_FACE, 0 /* 3D */, g);
            scene_push_card_bands(xf, v, card_id, WAIFU_CARD_LABEL_SUPPORT, g);
            (void)kind;
        }
    }

    if (g_frame.image_vert_count + 6 > SDL3_SCENE_MAX_IMAGE_VERTS) return 1;
    e = image_atlas_add(pixels, 0, w, h, 0);
    if (!e) return 1; /* atlas full: drop rather than corrupt */

    xf = xform_for(cam);
    for (i = 0; i < 4; ++i) xform_point(xf, &v[i], p[i]);
    uv[0][0] = (float)e->x;       uv[0][1] = (float)e->y;
    uv[1][0] = (float)(e->x + w); uv[1][1] = (float)e->y;
    uv[2][0] = (float)(e->x + w); uv[2][1] = (float)(e->y + h);
    uv[3][0] = (float)e->x;       uv[3][1] = (float)(e->y + h);

    {
        static const int order[6] = { 0, 1, 2, 0, 2, 3 };
        for (i = 0; i < 6; ++i) {
            int c = order[i];
            Sdl3ImageVertex *vt = &g_frame.image_verts[g_frame.image_vert_count++];
            vt->x = p[c][0]; vt->y = p[c][1]; vt->w = p[c][2];
            vt->u = uv[c][0]; vt->v = uv[c][1];
            vt->gray = g;
        }
    }

    /* Card rim outline (software draws it after the two triangles). The
       console also stamps two crossed diagonals over a used card, because an
       8bpp framebuffer cannot express "the same card, dimmed". Here it can --
       image.frag / hires.frag already multiply a grayed card's texel by 0.55,
       which is exactly a translucent black plate over it -- so the X is left
       off and the dimming alone carries the used state. */
    push_line3d_raw(p[0], p[1], edge_color);
    push_line3d_raw(p[1], p[2], edge_color);
    push_line3d_raw(p[2], p[3], edge_color);
    push_line3d_raw(p[3], p[0], edge_color);
    return 1;
}

int waifu_hw3d_line(const WaifuHw3DCamera *cam, const WaifuHw3DVec3 *a,
                    const WaifuHw3DVec3 *b, uint8_t color, int shadow_px)
{
    const SceneXform *xf = xform_for(cam);
    float pa[3], pb[3];
    mark_3d();
    xform_point(xf, a, pa);
    xform_point(xf, b, pb);
    push_line3d_raw(pa, pb, color);
    if (shadow_px > 0) {
        /* +N screen pixels downward: ndc_y decreases by 2N/H, premultiplied
           by each vertex's own w. */
        float dy = 2.0f * (float)shadow_px / (float)WAIFU_FM_HEIGHT;
        float sa[3] = { pa[0], pa[1] - dy * pa[2], pa[2] };
        float sb[3] = { pb[0], pb[1] - dy * pb[2], pb[2] };
        push_line3d_raw(sa, sb, color);
    }
    return 1;
}

/* ---- hw2d seam (UI layer) ---------------------------------------------------- */

int waifu_hw2d_active(void)
{
    return 1;
}

int waifu_hw2d_clear(uint8_t color)
{
    /* The software clear wipes the whole framebuffer: drop everything captured
       so far this frame and restart from a solid background. */
    float rgba[4];
    pal_rgba_f(color, rgba);
    waifu_sdl3_scene_frame_reset();
    g_frame.bg_rgba[0] = rgba[0];
    g_frame.bg_rgba[1] = rgba[1];
    g_frame.bg_rgba[2] = rgba[2];
    g_frame.bg_rgba[3] = 1.0f;
    g_frame.cleared = 1;
    g_frame.has_content = 1;
    return 1;
}

int waifu_hw2d_rect(int x, int y, int w, int h, uint8_t color)
{
    float rgba[4];
    if (w <= 0 || h <= 0) return 1;
    pal_rgba_f(color, rgba);
    ui_push_quad((float)x, (float)y, (float)(x + w), (float)(y + h),
                 -1.0f, 0.0f, -1.0f, 0.0f, rgba);
    return 1;
}

int waifu_hw2d_px(int x, int y, uint8_t color)
{
    return waifu_hw2d_rect(x, y, 1, 1, color);
}

int waifu_hw2d_line(int x0, int y0, int x1, int y1, uint8_t color)
{
    float rgba[4];
    pal_rgba_f(color, rgba);
    /* Center-of-pixel endpoints so the GPU raster matches Bresenham. */
    ui_push_line(x0 + 0.5f, y0 + 0.5f, x1 + 0.5f, y1 + 0.5f, rgba);
    return 1;
}

int waifu_hw2d_image(const uint8_t *pix, const uint8_t *mask, int sw, int sh,
                     int dx, int dy, int dw, int dh, int gray, int colorkey0)
{
    static const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    static const float dim[4] = { 0.55f, 0.55f, 0.55f, 1.0f };
    const ImageEntry *e;
    if (!pix || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return 1;

    /* 16:9 title / ending (PC): the full-screen source fills the whole canvas
       (the menu text / logo still overlays in the centered column). */
    {
        int fi = fullimage_lookup(pix);
        if (fi) { g_frame.full_image = fi; g_frame.has_content = 1; return 1; }
    }

    /* PC card back (hand, deck stacks, face-down thumbnails): the hi-res back
       covers the whole card rect, drawn over the 8bpp back so a missing source
       image degrades to the console art. */
    if (!mask && pix == waifu_assets_card_back()) {
        float hx = (float)(g_ui_hud ? WAIFU_FM_WIDTH + g_ui_extra_w : WAIFU_FM_WIDTH) * 0.5f;
        float hy = (float)WAIFU_FM_HEIGHT * 0.5f;
        float p[4][3];
        int idx;
        e = image_atlas_add(pix, mask, sw, sh, colorkey0);
        if (e) ui_push_quad((float)dx, (float)dy, (float)(dx + dw), (float)(dy + dh),
                            (float)e->x, (float)e->y, (float)(e->x + sw), (float)(e->y + sh),
                            gray ? dim : white);
        p[0][0] = (float)dx / hx - 1.0f;        p[0][1] = 1.0f - (float)dy / hy;        p[0][2] = 1.0f;
        p[1][0] = (float)(dx + dw) / hx - 1.0f; p[1][1] = 1.0f - (float)dy / hy;        p[1][2] = 1.0f;
        p[2][0] = (float)(dx + dw) / hx - 1.0f; p[2][1] = 1.0f - (float)(dy + dh) / hy; p[2][2] = 1.0f;
        p[3][0] = (float)dx / hx - 1.0f;        p[3][1] = 1.0f - (float)(dy + dh) / hy; p[3][2] = 1.0f;
        idx = push_hires_quad(p, 0, WAIFU_HIRES_BACK, 1 /* 2D */, gray ? 1.0f : 0.0f);
        if (idx >= 0) ui_append_hires(idx);
        g_frame.has_content = 1;
        return 1;
    }

    /* PC support cards (Spell / Trap): the full-resolution front frame covers
       the whole card, and the sigil is lifted out of the baked 8bpp face's art
       window and re-blitted into the frame's window -- support cards have no
       high-resolution source art of their own. The 8bpp face still goes down
       first, so a missing template simply leaves the console look (and the two
       art windows very nearly coincide, so the sigil lands right either way). */
    if (!mask && waifu_assets_support_face() && pix == waifu_assets_support_face()) {
        float hx = (float)(g_ui_hud ? WAIFU_FM_WIDTH + g_ui_extra_w : WAIFU_FM_WIDTH) * 0.5f;
        float hy = (float)WAIFU_FM_HEIGHT * 0.5f;
        float p[4][3];
        int idx;
        e = image_atlas_add(pix, mask, sw, sh, colorkey0);
        if (e) ui_push_quad((float)dx, (float)dy, (float)(dx + dw), (float)(dy + dh),
                            (float)e->x, (float)e->y, (float)(e->x + sw), (float)(e->y + sh),
                            gray ? dim : white);
        ui_rect_clip((float)dx, (float)dy, (float)(dx + dw), (float)(dy + dh), hx, hy, p);
        idx = push_hires_quad(p, g_card_frame_hint, WAIFU_HIRES_FRAME, 1 /* 2D */,
                              gray ? 1.0f : 0.0f);
        if (idx >= 0) ui_append_hires(idx);
        if (e)
            ui_push_quad((float)dx + (float)dw * CARD_ART_U0, (float)dy + (float)dh * CARD_ART_V0,
                         (float)dx + (float)dw * CARD_ART_U1, (float)dy + (float)dh * CARD_ART_V1,
                         (float)e->x + (float)sw * FACE_ART_U0, (float)e->y + (float)sh * FACE_ART_V0,
                         (float)e->x + (float)sw * FACE_ART_U1, (float)e->y + (float)sh * FACE_ART_V1,
                         gray ? dim : white);
        ui_push_card_bands((float)dx, (float)dy, (float)dw, (float)dh, hx, hy,
                           -1, g_card_label_hint, gray ? 1.0f : 0.0f);
        g_frame.has_content = 1;
        return 1;
    }

    /* Full-resolution card art (PC): a hand thumbnail or detail big-art blit
       becomes a hi-res quad, inserted into the UI run list at this exact point
       so any frame/cursor/stat text drawn afterwards still lands on top. The
       screen rect maps to clip space the same way the UI transform does. */
    {
        int card_id, kind;
        if (!mask && card_lookup(pix, &card_id, &kind)) {
            /* In the widescreen HUD bracket the enclosing quad maps game-x over
               [0, WAIFU_FM_WIDTH+extra] (matching the 8bpp frame's ui.vert
               transform), so a card sliding in from the true screen edge stays
               aligned with its frame. */
            float hx = (float)(g_ui_hud ? WAIFU_FM_WIDTH + g_ui_extra_w : WAIFU_FM_WIDTH) * 0.5f;
            float hy = (float)WAIFU_FM_HEIGHT * 0.5f;
            if (kind == WAIFU_HIRES_BIG) {
                /* Detail big art is frameless (the panel frame is drawn
                   separately): replace the whole rect with hi-res. */
                float p[4][3];
                int idx;
                p[0][0] = (float)dx / hx - 1.0f;        p[0][1] = 1.0f - (float)dy / hy;        p[0][2] = 1.0f;
                p[1][0] = (float)(dx + dw) / hx - 1.0f; p[1][1] = 1.0f - (float)dy / hy;        p[1][2] = 1.0f;
                p[2][0] = (float)(dx + dw) / hx - 1.0f; p[2][1] = 1.0f - (float)(dy + dh) / hy; p[2][2] = 1.0f;
                p[3][0] = (float)dx / hx - 1.0f;        p[3][1] = 1.0f - (float)(dy + dh) / hy; p[3][2] = 1.0f;
                idx = push_hires_quad(p, card_id, kind, 1 /* 2D */, gray ? 1.0f : 0.0f);
                if (idx >= 0) { ui_append_hires(idx); g_frame.has_content = 1; return 1; }
            } else {
                /* Card face: the 8bpp framed thumbnail goes down first as the
                   fallback layer, the full-resolution monster front frame covers
                   it, and the hi-res art fills that frame's art window. */
                float ax0 = (float)dx + (float)dw * CARD_ART_U0, ax1 = (float)dx + (float)dw * CARD_ART_U1;
                float ay0 = (float)dy + (float)dh * CARD_ART_V0, ay1 = (float)dy + (float)dh * CARD_ART_V1;
                float p[4][3];
                int idx;
                e = image_atlas_add(pix, mask, sw, sh, colorkey0);
                if (e) ui_push_quad((float)dx, (float)dy, (float)(dx + dw), (float)(dy + dh),
                                    (float)e->x, (float)e->y, (float)(e->x + sw), (float)(e->y + sh),
                                    gray ? dim : white);
                ui_rect_clip((float)dx, (float)dy, (float)(dx + dw), (float)(dy + dh), hx, hy, p);
                idx = push_hires_quad(p, WAIFU_CARD_FRAME_MONSTER, WAIFU_HIRES_FRAME,
                                      1 /* 2D */, gray ? 1.0f : 0.0f);
                if (idx >= 0) ui_append_hires(idx);
                ui_rect_clip(ax0, ay0, ax1, ay1, hx, hy, p);
                idx = push_hires_quad(p, card_id, kind, 1 /* 2D */, gray ? 1.0f : 0.0f);
                if (idx >= 0) ui_append_hires(idx);
                ui_push_card_bands((float)dx, (float)dy, (float)dw, (float)dh, hx, hy,
                                   card_id, WAIFU_CARD_LABEL_SUPPORT, gray ? 1.0f : 0.0f);
                g_frame.has_content = 1;
                return 1;
            }
        }
    }

    e = image_atlas_add(pix, mask, sw, sh, colorkey0);
    if (!e) return 1; /* atlas full: drop rather than corrupt */
    ui_push_quad((float)dx, (float)dy, (float)(dx + dw), (float)(dy + dh),
                 (float)e->x, (float)e->y,
                 (float)(e->x + sw), (float)(e->y + sh),
                 gray ? dim : white);
    return 1;
}

int waifu_hw2d_quad_alpha(const int xy[8], uint8_t color, int alpha)
{
    static const float uv[8] = { -1.0f, 0.0f, -1.0f, 0.0f, -1.0f, 0.0f, -1.0f, 0.0f };
    float rgba[4], fxy[8];
    int i;
    if (alpha <= 0) return 1;
    pal_rgba_f(color, rgba);
    rgba[3] = (alpha >= 255) ? 1.0f : (float)alpha / 255.0f;
    for (i = 0; i < 8; ++i) fxy[i] = (float)xy[i];
    ui_push_corner_quad(fxy, uv, rgba);
    return 1;
}

int waifu_hw2d_quad_rgba(const int xy[8], uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    static const float uv[8] = { -1.0f, 0.0f, -1.0f, 0.0f, -1.0f, 0.0f, -1.0f, 0.0f };
    float rgba[4], fxy[8];
    int i;
    if (a <= 0) return 1;
    rgba[0] = (float)r / 255.0f;
    rgba[1] = (float)g / 255.0f;
    rgba[2] = (float)b / 255.0f;
    rgba[3] = (float)a / 255.0f;
    for (i = 0; i < 8; ++i) fxy[i] = (float)xy[i];
    ui_push_corner_quad(fxy, uv, rgba);
    return 1;
}

/* Bilinear point on a screen-space quad wound (0,0),(1,0),(1,1),(0,1), then
   straight into the UI clip space push_hires_quad expects. */
static void quad_corner_clip(const float xy[8], float u, float v,
                             float hx, float hy, float out[3])
{
    float tx = xy[0] + (xy[2] - xy[0]) * u, ty = xy[1] + (xy[3] - xy[1]) * u;
    float bx = xy[6] + (xy[4] - xy[6]) * u, by = xy[7] + (xy[5] - xy[7]) * u;
    out[0] = (tx + (bx - tx) * v) / hx - 1.0f;
    out[1] = 1.0f - (ty + (by - ty) * v) / hy;
    out[2] = 1.0f;
}

static void quad_sub(const float xy[8], float u0, float v0, float u1, float v1,
                     float hx, float hy, float p[4][3])
{
    quad_corner_clip(xy, u0, v0, hx, hy, p[0]);
    quad_corner_clip(xy, u1, v0, hx, hy, p[1]);
    quad_corner_clip(xy, u1, v1, hx, hy, p[2]);
    quad_corner_clip(xy, u0, v1, hx, hy, p[3]);
}

int waifu_hw2d_image_quad(const uint8_t *pix, int sw, int sh,
                          const int xy[8], int gray)
{
    static const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    static const float dim[4] = { 0.55f, 0.55f, 0.55f, 1.0f };
    const ImageEntry *e;
    float fxy[8], uv[8];
    float hx = (float)(g_ui_hud ? WAIFU_FM_WIDTH + g_ui_extra_w : WAIFU_FM_WIDTH) * 0.5f;
    float hy = (float)WAIFU_FM_HEIGHT * 0.5f;
    float g = gray ? 1.0f : 0.0f;
    int card_id, kind, i;
    if (!pix || sw <= 0 || sh <= 0) return 1;
    for (i = 0; i < 4; ++i) {
        fxy[i * 2 + 0] = (float)xy[i * 2 + 0];
        fxy[i * 2 + 1] = (float)xy[i * 2 + 1];
    }
    e = image_atlas_add(pix, 0, sw, sh, 0);
    if (e) {
        uv[0] = (float)e->x;        uv[1] = (float)e->y;
        uv[2] = (float)(e->x + sw); uv[3] = (float)e->y;
        uv[4] = (float)(e->x + sw); uv[5] = (float)(e->y + sh);
        uv[6] = (float)e->x;        uv[7] = (float)(e->y + sh);
        ui_push_corner_quad(fxy, uv, gray ? dim : white);
    }

    /* Same full-resolution substitution the axis-aligned blit does, so a card
       that is turning or tumbling (the play-to-board flight, the fusion whirl)
       keeps its real front frame instead of dropping to the 8bpp thumbnail for
       the length of the animation. The 8bpp quad above stays underneath as the
       fallback layer. */
    {
        float p[4][3];
        int idx;
        if (pix == waifu_assets_card_back()) {
            quad_sub(fxy, 0.0f, 0.0f, 1.0f, 1.0f, hx, hy, p);
            idx = push_hires_quad(p, 0, WAIFU_HIRES_BACK, 1 /* 2D */, g);
            if (idx >= 0) ui_append_hires(idx);
            g_frame.has_content = 1;
            return 1;
        }
        if (card_lookup(pix, &card_id, &kind) && kind == WAIFU_HIRES_FACE) {
            quad_sub(fxy, 0.0f, 0.0f, 1.0f, 1.0f, hx, hy, p);
            idx = push_hires_quad(p, WAIFU_CARD_FRAME_MONSTER, WAIFU_HIRES_FRAME, 1, g);
            if (idx >= 0) ui_append_hires(idx);
            quad_sub(fxy, CARD_ART_U0, CARD_ART_V0, CARD_ART_U1, CARD_ART_V1, hx, hy, p);
            idx = push_hires_quad(p, card_id, kind, 1, g);
            if (idx >= 0) ui_append_hires(idx);
            if (waifu_sdl3_hires_card_level(card_id) > 0) {
                quad_sub(fxy, CARD_STAR_U0, CARD_STAR_V0, CARD_STAR_U1, CARD_STAR_V1, hx, hy, p);
                idx = push_hires_quad(p, waifu_sdl3_hires_card_level(card_id) - 1,
                                      WAIFU_HIRES_STARS, 1, g);
                if (idx >= 0) ui_append_hires(idx);
            }
            quad_sub(fxy, CARD_STAT_U0, CARD_STAT_V0, CARD_STAT_U1, CARD_STAT_V1, hx, hy, p);
            idx = push_hires_quad(p, card_id, WAIFU_HIRES_STATS, 1, g);
            if (idx >= 0) ui_append_hires(idx);
        }
    }
    g_frame.has_content = 1;
    return 1;
}

/* ---- platform video seams (replacing host_video.c) --------------------------- */

/* Sky band palette slots (stable semantic indices from
   src/generated/waifu_assets.h; IDX_* names given so this mirrors the software
   draw_*_sky() colours while following palette swaps/fades). */
#define ENV_IDX_BLACK      255  /* IDX_BLACK */
#define ENV_IDX_DIM        133  /* IDX_DIM */
#define ENV_IDX_UI_BLUE    198  /* IDX_UI_BLUE */
#define ENV_IDX_UI_TEAL    205  /* IDX_UI_TEAL */
#define ENV_IDX_GOLD_DARK  100  /* IDX_GOLD_DARK */
#define ENV_IDX_DARK_BROWN 243  /* IDX_DARK_BROWN */
#define ENV_IDX_RED        135  /* IDX_RED */
#define ENV_IDX_FLAME3     138  /* IDX_FLAME3 */

static void env_stop(float dst[4], float y, uint8_t idx)
{
    float rgba[4];
    pal_rgba_f(idx, rgba);
    dst[0] = rgba[0]; dst[1] = rgba[1]; dst[2] = rgba[2];
    dst[3] = y;
}

/* Resolve the story-scene sky into a smooth 4-stop vertical gradient. The stops
   correspond to draw_*_sky()'s bands (top -> horizon), rendered interpolated on
   the GPU instead of as hard rows. Requested through the same seam the PC-FX
   RAINBOW and CD32X MD sky plane use, so the software band fill is skipped. */
/* ---- widescreen HUD seam ---------------------------------------------------- */

void waifu_sdl3_set_ui_extra_w(int extra)
{
    g_ui_extra_w = extra < 0 ? 0 : extra;
}

int waifu_platform_ui_extra_w(void)
{
    return g_ui_extra_w;
}

int waifu_platform_performance_tier(void) { return 2; }

void waifu_platform_ui_hud(int on)
{
    int want = on ? 1 : 0;
    if (want != g_ui_hud) ui_close_run();   /* don't coalesce across the boundary */
    g_ui_hud = want;
}

/* Arena backdrop (platform.h): the duel board is cleared to black on every
   console, which on a widescreen display leaves the board floating in a void.
   Here the frame opens with a graded vault instead — a deep indigo sky falling
   to near-black, with a warm floor haze under the board's horizon — pushed as
   the first full-width UI runs of the frame, so they are captured before any
   3D primitive and composite underneath the board. */
static const float k_arena_top[4]   = { 0.050f, 0.054f, 0.140f, 1.0f };
static const float k_arena_mid[4]   = { 0.105f, 0.085f, 0.195f, 1.0f };
static const float k_arena_low[4]   = { 0.028f, 0.024f, 0.058f, 1.0f };
static const float k_arena_haze0[4] = { 0.85f, 0.66f, 0.32f, 0.00f };
static const float k_arena_haze1[4] = { 0.85f, 0.66f, 0.32f, 0.15f };
static const float k_arena_haze2[4] = { 0.85f, 0.66f, 0.32f, 0.00f };

static void arena_grad_at(const float a[4], const float b[4], float t, float out[4])
{
    int i;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    for (i = 0; i < 4; ++i) out[i] = a[i] + (b[i] - a[i]) * t;
}

/* One backdrop layer spanning [ly0, ly1), clipped to the caller's rectangle and
   with its stop colours re-evaluated at the clipped edges -- so a band of the
   backdrop is exactly the pixels the full-screen paint would have put there. */
static void arena_layer(float ly0, float ly1, const float a[4], const float b[4],
                        float rx0, float ry0, float rx1, float ry1)
{
    float y0 = ry0 > ly0 ? ry0 : ly0;
    float y1 = ry1 < ly1 ? ry1 : ly1;
    float c0[4], c1[4];
    if (y1 <= y0 || ly1 <= ly0) return;
    arena_grad_at(a, b, (y0 - ly0) / (ly1 - ly0), c0);
    arena_grad_at(a, b, (y1 - ly0) / (ly1 - ly0), c1);
    ui_push_quad_grad(rx0, y0, rx1, y1, c0, c1);
}

int waifu_platform_arena_backdrop_band(int x, int y, int w, int h)
{
    const float fh = (float)WAIFU_FM_HEIGHT;
    const float x0 = (float)x, y0 = (float)y;
    const float x1 = (float)(x + w), y1 = (float)(y + h);
    const int was_hud = g_ui_hud;
    if (w <= 0 || h <= 0) return 1;
    waifu_platform_ui_hud(1);
    arena_layer(0.0f, fh * 0.55f, k_arena_top, k_arena_mid, x0, y0, x1, y1);
    arena_layer(fh * 0.55f, fh, k_arena_mid, k_arena_low, x0, y0, x1, y1);
    arena_layer(fh * 0.38f, fh * 0.62f, k_arena_haze0, k_arena_haze1, x0, y0, x1, y1);
    arena_layer(fh * 0.62f, fh * 0.92f, k_arena_haze1, k_arena_haze2, x0, y0, x1, y1);
    /* The side vignette is deliberately NOT repainted: it is transparent
       everywhere except the outer flanks, and nothing that uses this seam sits
       out there. */
    waifu_platform_ui_hud(was_hud);
    return 1;
}

int waifu_platform_arena_backdrop(void)
{
    static const float dark[4]  = { 0.0f, 0.0f, 0.0f, 0.62f };
    static const float clear[4] = { 0.0f, 0.0f, 0.0f, 0.00f };
    const float w = (float)(WAIFU_FM_WIDTH + g_ui_extra_w);
    const float h = (float)WAIFU_FM_HEIGHT;
    const int was_hud = g_ui_hud;

    waifu_platform_ui_hud(1);
    ui_push_quad_grad(0.0f, 0.0f, w, h * 0.55f, k_arena_top, k_arena_mid);
    ui_push_quad_grad(0.0f, h * 0.55f, w, h, k_arena_mid, k_arena_low);
    /* Horizon haze: a soft warm band around the board's horizon line, so the
       flanks read as an arena rather than empty space. */
    ui_push_quad_grad(0.0f, h * 0.38f, w, h * 0.62f, k_arena_haze0, k_arena_haze1);
    ui_push_quad_grad(0.0f, h * 0.62f, w, h * 0.92f, k_arena_haze1, k_arena_haze2);
    /* Side vignette: only exists when the display is wider than the game
       column, and it falls off exactly at the column edge, so the board keeps
       the eye and the flanks frame it instead of glowing. */
    if (g_ui_extra_w > 0) {
        const float flank = (float)g_ui_extra_w * 0.5f;
        ui_push_quad_hgrad(0.0f, 0.0f, flank, h, dark, clear);
        ui_push_quad_hgrad(w - flank, 0.0f, w, h, clear, dark);
    }
    waifu_platform_ui_hud(was_hud);
    return 1;
}

/* SDL3 decodes the 16:9 ending image in the present pre-pass (fullimage_ensure),
   not lazily mid-frame, so there is nothing to prewarm. */
void waifu_platform_prewarm_ending(void) {}

int waifu_platform_background_request(WaifuBackgroundKind kind, int hscroll)
{
    Sdl3Env *env = &g_frame.env;
    env->has_sky = 1;
    env->sky_kind = (int)kind;
    env->sky_hscroll = hscroll;

    switch (kind) {
    case WAIFU_BACKGROUND_STONE:  /* temple: blue -> teal -> dim -> brown */
        env_stop(env->sky_stops[0], 0.00f, ENV_IDX_UI_BLUE);
        env_stop(env->sky_stops[1], 0.28f, ENV_IDX_UI_TEAL);
        env_stop(env->sky_stops[2], 0.52f, ENV_IDX_DIM);
        env_stop(env->sky_stops[3], 0.80f, ENV_IDX_DARK_BROWN);
        env_stop(env->horizon,      0.00f, ENV_IDX_DARK_BROWN);
        break;
    case WAIFU_BACKGROUND_EMBER:  /* volcano: black -> red -> flame -> brown */
        env_stop(env->sky_stops[0], 0.00f, ENV_IDX_BLACK);
        env_stop(env->sky_stops[1], 0.28f, ENV_IDX_RED);
        env_stop(env->sky_stops[2], 0.52f, ENV_IDX_FLAME3);
        env_stop(env->sky_stops[3], 0.80f, ENV_IDX_DARK_BROWN);
        env_stop(env->horizon,      0.00f, ENV_IDX_DARK_BROWN);
        break;
    case WAIFU_BACKGROUND_SKY:    /* void: deep black with drifting stars */
        env_stop(env->sky_stops[0], 0.00f, ENV_IDX_BLACK);
        env_stop(env->sky_stops[1], 0.50f, ENV_IDX_BLACK);
        env_stop(env->sky_stops[2], 0.85f, ENV_IDX_BLACK);
        env_stop(env->sky_stops[3], 1.00f, ENV_IDX_BLACK);
        env_stop(env->horizon,      0.00f, ENV_IDX_BLACK);
        break;
    default:                      /* desert: blue -> teal -> gold -> brown */
        env_stop(env->sky_stops[0], 0.00f, ENV_IDX_UI_BLUE);
        env_stop(env->sky_stops[1], 0.33f, ENV_IDX_UI_TEAL);
        env_stop(env->sky_stops[2], 0.60f, ENV_IDX_GOLD_DARK);
        env_stop(env->sky_stops[3], 0.85f, ENV_IDX_DARK_BROWN);
        env_stop(env->horizon,      0.00f, ENV_IDX_DARK_BROWN);
        break;
    }
    /* Far distance (world units) over which the floor fades into the horizon
       colour, hiding checker aliasing at the horizon line. */
    env->horizon[3] = 26.0f;
    return 1;
}

/* No hardware text layer: UI panels stay on the (captured) software path. */
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
/* Story dialogue portraits at full resolution (PC).  The decoded portrait is
   framed to the same 124x200 cell the 8bpp one occupies (sdl3_hires.c does the
   fitting), so this is a straight substitution: the quad is exactly the rect the
   software blit would have covered, and the upper-body framing / bottom anchor
   come out of the shared recipe rather than being re-guessed here.
   Returns 0 when there is no source, and the caller blits the 8bpp portrait. */
int waifu_platform_story_portrait(int portrait_id, int x, int y)
{
    float hx, hy, p[4][3];
    int idx;
    if (portrait_id < 0 || portrait_id >= WAIFU_SDL3_PORTRAIT_SRC_COUNT) return 0;
    if (!waifu_sdl3_portrait_src[portrait_id] || !waifu_sdl3_portrait_src[portrait_id][0]) return 0;
    hx = (float)(g_ui_hud ? WAIFU_FM_WIDTH + g_ui_extra_w : WAIFU_FM_WIDTH) * 0.5f;
    hy = (float)WAIFU_FM_HEIGHT * 0.5f;
    {
        float x0 = (float)x, y0 = (float)y;
        /* WAIFU_STORY_PORTRAIT_W/H from src/generated/waifu_assets.h, spelled
           out rather than pulling that 86k-line header into the capture TU. */
        float x1 = x0 + 124.0f;
        float y1 = y0 + 200.0f;
        p[0][0] = x0 / hx - 1.0f; p[0][1] = 1.0f - y0 / hy; p[0][2] = 1.0f;
        p[1][0] = x1 / hx - 1.0f; p[1][1] = 1.0f - y0 / hy; p[1][2] = 1.0f;
        p[2][0] = x1 / hx - 1.0f; p[2][1] = 1.0f - y1 / hy; p[2][2] = 1.0f;
        p[3][0] = x0 / hx - 1.0f; p[3][1] = 1.0f - y1 / hy; p[3][2] = 1.0f;
    }
    idx = push_hires_quad(p, portrait_id, WAIFU_HIRES_PORTRAIT, 1 /* 2D */, 0.0f);
    if (idx < 0) return 0;
    ui_append_hires(idx);
    g_frame.has_content = 1;
    return 1;
}

/* ---- title-screen presentation (PC) ------------------------------------------
 * The title is a 16:9 photograph with the logo over it, which on a desktop
 * screen leaves the tagline and prompt sitting on whatever the artwork happens
 * to be doing behind them, and the jump into the menu was an instant cut. Both
 * are fixed here rather than in the core: they are pure presentation, they want
 * alpha and diagonals the 8bpp software layer has no way to express, and no
 * console should pay for them.
 * --------------------------------------------------------------------------- */

static float title_full_w(void)
{
    return (float)(g_ui_hud ? WAIFU_FM_WIDTH + g_ui_extra_w : WAIFU_FM_WIDTH);
}

/* Set once at start-up from the command line; see platform.h. */
static int g_title_attract_on = 1;

void waifu_sdl3_set_title_attract(int on)
{
    g_title_attract_on = on ? 1 : 0;
}

int waifu_platform_title_attract(void)
{
    return g_title_attract_on;
}

void waifu_platform_title_view(int32_t u0, int32_t v0, int32_t u1, int32_t v1)
{
    /* Q16 (see platform.h): the pan needs sub-source-pixel steps to drift
       rather than tick along. */
    g_frame.full_image_uv[0] = (float)u0 / (float)WAIFU_TITLE_VIEW_ONE;
    g_frame.full_image_uv[1] = (float)v0 / (float)WAIFU_TITLE_VIEW_ONE;
    g_frame.full_image_uv[2] = (float)u1 / (float)WAIFU_TITLE_VIEW_ONE;
    g_frame.full_image_uv[3] = (float)v1 / (float)WAIFU_TITLE_VIEW_ONE;
}

int waifu_platform_title_scrim(int32_t reveal_q8)
{
    float a = (float)reveal_q8 / 256.0f;
    float w = title_full_w();
    const float clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    float dark[4] = { 0.02f, 0.02f, 0.05f, 0.0f };
    float rule[4] = { 0.93f, 0.78f, 0.36f, 0.0f };
    float half;

    if (a < 0.0f) a = 0.0f;
    if (a > 1.0f) a = 1.0f;
    dark[3] = 0.82f * a;
    rule[3] = 0.85f * a;

    /* A gradient into the bottom third, then a solid tail: the tagline, the
       prompt and the copyright all sit on it, so none of them depends on the
       photograph being dark in that spot. */
    ui_push_quad_grad(0.0f, 118.0f, w, 174.0f, clear, dark);
    ui_push_quad(0.0f, 174.0f, w, (float)WAIFU_FM_HEIGHT, -1.0f, 0.0f, -1.0f, 0.0f, dark);

    /* A gold rule that opens from the centre as the tagline types itself in. */
    half = w * 0.44f * a;
    ui_push_quad(w * 0.5f - half, 152.0f, w * 0.5f + half, 153.0f,
                 -1.0f, 0.0f, -1.0f, 0.0f, rule);
    return 1;
}

/* The wipe into the menu. The screen is cut into vertical shards that sweep
   through it in alternating directions, each with a slanted leading edge lit
   gold, over a white impact flash and a black beat at the crossing point. The
   deck is called Shattered; the transition may as well say so. */
int waifu_platform_title_shatter(int32_t t_q8)
{
    enum { SHARDS = 9 };
    float t = (float)t_q8 / 256.0f;
    float w = title_full_w();
    float h = (float)WAIFU_FM_HEIGHT;
    const float slant = 30.0f;
    float band = h + 2.0f * slant;
    const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    float rim[4] = { 1.0f, 0.86f, 0.42f, 0.9f };
    int i;

    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    for (i = 0; i < SHARDS; ++i) {
        float x0 = w * (float)i / (float)SHARDS;
        float x1 = w * (float)(i + 1) / (float)SHARDS;
        /* A small per-shard lead so they do not move as one slab. The black
           veil below covers the gaps this opens at the crossing point. */
        float lead = ((i * 37) % 11) / 11.0f * 0.10f;
        float tt = (t - lead) / (1.0f - lead);
        float top, s = (i & 1) ? slant : -slant;
        float xy[8], uv[8];
        int c;
        if (tt <= 0.0f) tt = 0.0f;
        if (tt >= 1.0f) tt = 1.0f;
        /* Odd shards sweep up, even ones down; both start clear of the screen
           and finish clear of the far side. */
        top = (i & 1) ? (h - tt * (h + band)) : (tt * (h + band) - band);
        for (c = 0; c < 4; ++c) { uv[c * 2] = -1.0f; uv[c * 2 + 1] = 0.0f; }
        xy[0] = x0; xy[1] = top;
        xy[2] = x1; xy[3] = top + s;
        xy[4] = x1; xy[5] = top + band + s;
        xy[6] = x0; xy[7] = top + band;
        ui_push_corner_quad(xy, uv, black);

        /* The lit edge, on whichever side is leading. */
        if (tt > 0.0f && tt < 1.0f) {
            float e = (i & 1) ? top : top + band;
            float xy2[8];
            xy2[0] = x0; xy2[1] = e;
            xy2[2] = x1; xy2[3] = e + s;
            xy2[4] = x1; xy2[5] = e + s + ((i & 1) ? -2.0f : 2.0f);
            xy2[6] = x0; xy2[7] = e + ((i & 1) ? -2.0f : 2.0f);
            ui_push_corner_quad(xy2, uv, rim);
        }
    }

    /* A solid black beat where the shards cross, so the menu is never revealed
       through the seams between them. */
    {
        float veil = 1.0f - fabsf(t - 0.52f) / 0.20f;
        if (veil > 0.0f) {
            float c[4] = { 0.0f, 0.0f, 0.0f, veil > 1.0f ? 1.0f : veil };
            ui_push_quad(0.0f, 0.0f, w, h, -1.0f, 0.0f, -1.0f, 0.0f, c);
        }
    }
    /* The impact: a hard white flash on the first few frames. */
    if (t < 0.16f) {
        float c[4] = { 1.0f, 0.98f, 0.90f, (0.16f - t) / 0.16f * 0.75f };
        ui_push_quad(0.0f, 0.0f, w, h, -1.0f, 0.0f, -1.0f, 0.0f, c);
    }
    return 1;
}

/* ---- frame lifecycle ---------------------------------------------------------- */

Sdl3SceneFrame *waifu_sdl3_scene_frame(void)
{
    return &g_frame;
}

/* Consumed once by the present after the environment pass has been rendered:
   the sky request is set BEFORE the frame's clear_screen (which runs
   frame_reset), so it must persist across the reset and only be dropped here so
   a following non-story frame does not inherit a stale sky. */
void waifu_sdl3_env_consume_sky(void)
{
    g_frame.env.has_sky = 0;
}

void waifu_sdl3_scene_frame_reset(void)
{
    g_frame.has_content = 0;
    g_frame.cleared = 0;
    g_frame.scene_vert_count = 0;
    g_frame.env.has_floor = 0;      /* sky fields persist: cleared at present */
    g_frame.image_vert_count = 0;
    g_frame.line_vert_count = 0;
    g_frame.hires_draw_count = 0;
    g_frame.full_image = 0;
    g_frame.full_image_uv[0] = 0.0f; g_frame.full_image_uv[1] = 0.0f;
    g_frame.full_image_uv[2] = 1.0f; g_frame.full_image_uv[3] = 1.0f;
    g_ui_hud = 0;
    g_frame.ui_vert_count = 0;
    g_frame.ui_line_vert_count = 0;
    g_frame.ui_glyph_vert_count = 0;
    g_frame.ui_run_count = 0;
    g_frame.impact_fx_count = 0;
    g_frame.bg_runs = -1;
    g_frame.image_atlas_used_h = 0;
    g_image_entry_count = 0;
    g_shelf_x = g_shelf_y = g_shelf_h = 0;
    ui_close_run();
}
