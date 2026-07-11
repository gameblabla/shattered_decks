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

#include <math.h>
#include <string.h>

#include "platform.h"
#include "hw3d.h"
#include "game_api.h"

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

static void ui_close_run(void)
{
    g_run_open_kind = -1;
}

static void ui_append(int kind, int count_added)
{
    Sdl3UiRun *run;
    int first = (kind == SDL3_UI_RUN_TRIS) ? g_frame.ui_vert_count
                                           : g_frame.ui_line_vert_count;
    if (g_run_open_kind == kind && g_frame.ui_run_count > 0) {
        g_frame.ui_runs[g_frame.ui_run_count - 1].count += count_added;
        return;
    }
    if (g_frame.ui_run_count >= SDL3_UI_MAX_RUNS) return;
    run = &g_frame.ui_runs[g_frame.ui_run_count++];
    run->kind = kind;
    run->first = first - count_added;
    run->count = count_added;
    g_run_open_kind = kind;
}

static void ui_push_quad(float x0, float y0, float x1, float y1,
                         float u0, float v0, float u1, float v1,
                         const float rgba[4])
{
    Sdl3UiVertex *vt;
    static const int cx[6] = { 0, 1, 1, 0, 1, 0 };
    static const int cy[6] = { 0, 0, 1, 0, 1, 1 };
    int i;
    if (g_frame.ui_vert_count + 6 > SDL3_UI_MAX_VERTS) return;
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
    if (g_image_entry_count >= SDL3_IMAGE_MAX_ENTRIES) return 0;
    if (w > SDL3_IMAGE_ATLAS_W || h > SDL3_IMAGE_ATLAS_H) return 0;
    if (g_shelf_x + w > SDL3_IMAGE_ATLAS_W) {
        g_shelf_y += g_shelf_h;
        g_shelf_x = 0;
        g_shelf_h = 0;
    }
    if (g_shelf_y + h > SDL3_IMAGE_ATLAS_H) return 0;
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

int waifu_hw3d_quad(const WaifuHw3DCamera *cam, const WaifuHw3DVec3 v[4], int tile)
{
    const SceneXform *xf = xform_for(cam);
    float p[4][3];
    float t;
    int i;
    mark_3d();
    ensure_tiles_converted();
    if (tile < 0) tile = 0;
    if (g_frame.tile_count > 0 && tile >= g_frame.tile_count) tile = g_frame.tile_count - 1;
    t = (float)tile;
    for (i = 0; i < 4; ++i) xform_point(xf, &v[i], p[i]);
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
    /* One large rectangle on the plane y = floor_y centered under the camera;
       the checkerboard tile pattern is evaluated per fragment from world XZ.
       The rectangle is clipped against the camera near plane HERE, in world
       space: a whole-plane quad has corners behind the eye, and triangles
       crossing w = 0 do not rasterize reliably, so only forward geometry is
       ever submitted. The far plane (200 world units) trims the horizon; the
       sky's ground band sits behind it, matching the software look where the
       raycast floor recedes into the distant-ground sky color. */
    const float R = 180.0f;
    const float NEAR_CZ = 0.06f;
    const SceneXform *xf;
    Sdl3FloorDraw *fd;
    float fy;
    float poly[8][2];   /* clipped polygon, world XZ */
    float cz[8];
    int poly_n = 4;
    float clipped[8][2];
    int n, i;

    mark_3d();
    ensure_tiles_converted();
    if (g_frame.floor_count >= SDL3_SCENE_MAX_FLOORS) return 1;
    xf = xform_for(cam);
    fy = floor_y / 256.0f;

    poly[0][0] = xf->ex - R; poly[0][1] = xf->ez - R;
    poly[1][0] = xf->ex + R; poly[1][1] = xf->ez - R;
    poly[2][0] = xf->ex + R; poly[2][1] = xf->ez + R;
    poly[3][0] = xf->ex - R; poly[3][1] = xf->ez + R;

    /* Camera-space forward distance of a floor point (y fixed at fy). */
    for (i = 0; i < poly_n; ++i) {
        float px = poly[i][0] - xf->ex;
        float py = fy - xf->ey;
        float pz = poly[i][1] - xf->ez;
        cz[i] = px * xf->fx + py * xf->fy + pz * xf->fz;
    }

    /* Sutherland-Hodgman against cz >= NEAR_CZ (cz is affine in world XZ, so
       edge interpolation is exact). */
    n = 0;
    for (i = 0; i < poly_n; ++i) {
        int j = (i + 1) % poly_n;
        int in_i = cz[i] >= NEAR_CZ;
        int in_j = cz[j] >= NEAR_CZ;
        if (in_i) {
            clipped[n][0] = poly[i][0];
            clipped[n][1] = poly[i][1];
            ++n;
        }
        if (in_i != in_j) {
            float t = (NEAR_CZ - cz[i]) / (cz[j] - cz[i]);
            clipped[n][0] = poly[i][0] + t * (poly[j][0] - poly[i][0]);
            clipped[n][1] = poly[i][1] + t * (poly[j][1] - poly[i][1]);
            ++n;
        }
    }
    if (n < 3) return 1; /* plane entirely behind the camera */

    fd = &g_frame.floors[g_frame.floor_count];
    fd->first_vertex = g_frame.floor_count * SDL3_FLOOR_VERTS_PER_DRAW;
    fd->tile_a = (float)(tile_a < 0 ? 0 : tile_a);
    fd->tile_b = (float)(tile_b < 0 ? 0 : tile_b);
    fd->texels_per_unit = 32.0f / (tile_size > 0 ? tile_size / 256.0f : 1.0f);

    /* Fan-triangulate the (convex) clipped polygon. */
    fd->vertex_count = 0;
    for (i = 1; i + 1 < n && fd->vertex_count + 3 <= SDL3_FLOOR_VERTS_PER_DRAW; ++i) {
        int tri[3] = { 0, i, i + 1 };
        int k;
        for (k = 0; k < 3; ++k) {
            WaifuHw3DVec3 wp;
            float pos[3];
            Sdl3FloorVertex *vt = &g_frame.floor_verts[fd->first_vertex + fd->vertex_count++];
            wp.x = (int32_t)(clipped[tri[k]][0] * 256.0f);
            wp.y = floor_y;
            wp.z = (int32_t)(clipped[tri[k]][1] * 256.0f);
            xform_point(xf, &wp, pos);
            vt->x = pos[0]; vt->y = pos[1]; vt->w = pos[2];
            vt->wx = clipped[tri[k]][0];
            vt->wz = clipped[tri[k]][1];
        }
    }
    g_frame.floor_count++;
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

    /* Card rim outline (software draws it after the two triangles), plus the
       crossed diagonals for the grayed used-card state. */
    push_line3d_raw(p[0], p[1], edge_color);
    push_line3d_raw(p[1], p[2], edge_color);
    push_line3d_raw(p[2], p[3], edge_color);
    push_line3d_raw(p[3], p[0], edge_color);
    if (gray) {
        push_line3d_raw(p[0], p[2], edge_color);
        push_line3d_raw(p[1], p[3], edge_color);
    }
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
    e = image_atlas_add(pix, mask, sw, sh, colorkey0);
    if (!e) return 1; /* atlas full: drop rather than corrupt */
    ui_push_quad((float)dx, (float)dy, (float)(dx + dw), (float)(dy + dh),
                 (float)e->x, (float)e->y,
                 (float)(e->x + sw), (float)(e->y + sh),
                 gray ? dim : white);
    return 1;
}

int waifu_hw2d_image_quad(const uint8_t *pix, int sw, int sh,
                          const int xy[8], int gray)
{
    static const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    static const float dim[4] = { 0.55f, 0.55f, 0.55f, 1.0f };
    const ImageEntry *e;
    float fxy[8], uv[8];
    int i;
    if (!pix || sw <= 0 || sh <= 0) return 1;
    e = image_atlas_add(pix, 0, sw, sh, 0);
    if (!e) return 1;
    for (i = 0; i < 4; ++i) {
        fxy[i * 2 + 0] = (float)xy[i * 2 + 0];
        fxy[i * 2 + 1] = (float)xy[i * 2 + 1];
    }
    uv[0] = (float)e->x;        uv[1] = (float)e->y;
    uv[2] = (float)(e->x + sw); uv[3] = (float)e->y;
    uv[4] = (float)(e->x + sw); uv[5] = (float)(e->y + sh);
    uv[6] = (float)e->x;        uv[7] = (float)(e->y + sh);
    ui_push_corner_quad(fxy, uv, gray ? dim : white);
    return 1;
}

/* ---- platform video seams (replacing host_video.c) --------------------------- */

/* No dedicated background layer: the software sky paints before the frame's
   first 3D primitive and is captured into the backdrop runs, pixel-exact. */
int waifu_platform_background_request(WaifuBackgroundKind kind, int hscroll)
{
    (void)kind;
    (void)hscroll;
    return 0;
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

/* ---- frame lifecycle ---------------------------------------------------------- */

Sdl3SceneFrame *waifu_sdl3_scene_frame(void)
{
    return &g_frame;
}

void waifu_sdl3_scene_frame_reset(void)
{
    g_frame.has_content = 0;
    g_frame.cleared = 0;
    g_frame.scene_vert_count = 0;
    g_frame.floor_count = 0;
    g_frame.image_vert_count = 0;
    g_frame.line_vert_count = 0;
    g_frame.ui_vert_count = 0;
    g_frame.ui_line_vert_count = 0;
    g_frame.ui_run_count = 0;
    g_frame.bg_runs = -1;
    g_frame.image_atlas_used_h = 0;
    g_image_entry_count = 0;
    g_shelf_x = g_shelf_y = g_shelf_h = 0;
    ui_close_run();
}
