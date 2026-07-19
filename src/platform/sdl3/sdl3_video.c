/* SDL3 GPU presenter: consumes the frame captured by sdl3_scene3d.c.
 *
 * The canvas (a fixed 5x-resolution RGBA offscreen target) is PERSISTENT: it
 * behaves exactly like the game's framebuffer. Each present renders only this
 * frame's captured commands into it, in capture order:
 *   1. a captured clear_screen restarts the canvas from the clear color
 *      (otherwise prior content is kept — the game redraws menus and dialogs
 *      incrementally and relies on persistence);
 *   2. backdrop UI runs (sky bands captured before the first 3D primitive);
 *   3. the 3D scene with a real depth buffer — checkered floor plane,
 *      atlas-tile polygons, card-face quads, 3D lines;
 *   4. the remaining UI runs (panels, text, sprites), alpha-blended.
 * The canvas is then drawn to the swapchain through the blit pipeline, which
 * applies the global fade — fades never bake into persistent content. */

#include "sdl3_video.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "sdl3_internal.h"
#include "sdl3_shaders.h"
#include "sdl3_hires.h"
#include "sdl3_text.h"

/* The offscreen canvas tracks the drawable so the renderer is both resolution-
   agnostic (HD/4K) and widescreen-aware:
     - Vertical: the canvas is an integer scale N of WAIFU_FM_HEIGHT (N = ceil of
       the fit), so the 2D pixel art stays a uniform integer scale (no shimmer)
       and the 3D rasterizes at >= native resolution, then the present blit
       linear-downscales the supersampled result.
     - Horizontal: the canvas WIDTH follows the display aspect (>= the game's
       WAIFU_FM_WIDTH*N column). The story environment (sky gradient + infinite
       floor) fills the full width with a widened horizontal FOV, while the 2D UI
       and the 3D solids render into a CENTERED WAIFU_FM_WIDTH*N column
       (undistorted) — i.e. non-widescreen assets are centered, exactly as a
       real display would letterbox them, but the sides are filled by the
       environment instead of black bars where one exists.
   A single factor hs = column_width / canvas_width aligns the two (the floor
   ray uses sx*hs so it lines up under the centered solids). */
#define CANVAS_MIN_SCALE 2
#define CANVAS_MAX_SCALE 16   /* N=9 is native 4K; 16 covers ~5K/6K drawables */
#define CANVAS_MAX_ASPECT 3.0f /* clamp beyond ~27:9 so ultrawide stays sane */

/* Vertex-buffer layout: one GPU buffer, fixed segment offsets. */
#define SEG_SCENE_OFF   0
#define SEG_SCENE_SIZE  (SDL3_SCENE_MAX_SCENE_VERTS * (int)sizeof(Sdl3SceneVertex))
#define SEG_IMAGE_OFF   (SEG_SCENE_OFF + SEG_SCENE_SIZE)
#define SEG_IMAGE_SIZE  (SDL3_SCENE_MAX_IMAGE_VERTS * (int)sizeof(Sdl3ImageVertex))
#define SEG_LINE_OFF    (SEG_IMAGE_OFF + SEG_IMAGE_SIZE)
#define SEG_LINE_SIZE   (SDL3_SCENE_MAX_LINE_VERTS * (int)sizeof(Sdl3LineVertex))
#define SEG_UI_OFF      (SEG_LINE_OFF + SEG_LINE_SIZE)
#define SEG_UI_SIZE     (SDL3_UI_MAX_VERTS * (int)sizeof(Sdl3UiVertex))
#define SEG_UILINE_OFF  (SEG_UI_OFF + SEG_UI_SIZE)
#define SEG_UILINE_SIZE (SDL3_UI_MAX_LINE_VERTS * (int)sizeof(Sdl3UiVertex))
#define SEG_HIRES_OFF   (SEG_UILINE_OFF + SEG_UILINE_SIZE)
#define SEG_HIRES_SIZE  (SDL3_HIRES_MAX_DRAWS * 6 * (int)sizeof(Sdl3ImageVertex))
#define SEG_GLYPH_OFF   (SEG_HIRES_OFF + SEG_HIRES_SIZE)
#define SEG_GLYPH_SIZE  (SDL3_UI_MAX_GLYPH_VERTS * (int)sizeof(Sdl3UiVertex))
#define SEG_TOTAL       (SEG_GLYPH_OFF + SEG_GLYPH_SIZE)

struct WaifuSdl3Video {
    SDL_Window *window;
    SDL_GPUDevice *dev;

    SDL_GPUTexture *canvas;      /* persistent canvas_w x canvas_h RGBA8 */
    SDL_GPUTexture *depth;
    int canvas_w, canvas_h;      /* current canvas size (display aspect) */
    int canvas_scale;            /* current integer N (= canvas_h / WAIFU_FM_HEIGHT) */
    int pending_w, pending_h;    /* canvas size for the next frame, from the last drawable */
    int read_tbuf_bytes;         /* size the readback transfer buffer was made at */
    SDL_GPUTextureFormat depth_format;
    SDL_GPUTextureFormat swap_format;
    SDL_GPUTexture *tile_tex;    /* 32x32 RGBA8 array */
    SDL_GPUTexture *image_tex;   /* streaming RGBA atlas */
    SDL_GPUSampler *sampler;     /* nearest (texelFetch paths) */
    SDL_GPUSampler *sampler_lin; /* linear (present blit) */
    SDL_GPUSampler *sampler_mip; /* linear + mipmap (hi-res card art) */

    /* Full-resolution card art (PC): per-card mipmapped textures, decoded from
       PNG/WebP on first use. Parallel arrays of hires_count entries; a *_tried
       flag prevents re-decoding a card that failed. */
    SDL_GPUTexture **hires_face;
    SDL_GPUTexture **hires_big;
    unsigned char *hires_face_tried;
    unsigned char *hires_big_tried;
    int hires_count;

    /* 16:9 title / ending source textures (PC), decoded on first use. */
    SDL_GPUTexture *title_tex;
    SDL_GPUTexture *ending_tex;
    unsigned char title_tried, ending_tried;

    SDL_GPUGraphicsPipeline *pl_scene;
    SDL_GPUGraphicsPipeline *pl_env;
    SDL_GPUGraphicsPipeline *pl_hires_scene; /* hi-res card, depth, no blend (3D) */
    SDL_GPUGraphicsPipeline *pl_hires_ui;    /* hi-res card, blend, no depth (2D) */
    SDL_GPUGraphicsPipeline *pl_fullimage;   /* 16:9 title/ending, fullscreen */
    SDL_GPUGraphicsPipeline *pl_image;
    SDL_GPUGraphicsPipeline *pl_line3d;
    SDL_GPUGraphicsPipeline *pl_ui_tris;
    SDL_GPUGraphicsPipeline *pl_ui_lines;
    SDL_GPUGraphicsPipeline *pl_glyph;       /* FreeType text glyphs (linear atlas) */
    SDL_GPUGraphicsPipeline *pl_blit;

    SDL_GPUTexture *glyph_tex;               /* persistent FreeType glyph atlas */
    unsigned char glyph_tried;

    SDL_GPUTransferBuffer *vtx_tbuf;
    SDL_GPUBuffer *vtx_buf;
    SDL_GPUTransferBuffer *tex_tbuf;   /* image atlas + tile atlas staging */
    SDL_GPUTransferBuffer *read_tbuf;  /* lazy, for read_frame */

    uint32_t tile_serial_uploaded;
    int canvas_initialized;
    int debug_counts;
    int fullscreen;
};

/* ---- shader / pipeline helpers -------------------------------------------- */

static SDL_GPUShader *load_shader(SDL_GPUDevice *dev, const unsigned char *code,
                                  size_t code_size, SDL_GPUShaderStage stage,
                                  int num_samplers, int num_uniform_buffers)
{
    SDL_GPUShaderCreateInfo info;
    SDL_zero(info);
    info.code = code;
    info.code_size = code_size;
    info.entrypoint = "main";
    info.format = SDL_GPU_SHADERFORMAT_SPIRV;
    info.stage = stage;
    info.num_samplers = (Uint32)num_samplers;
    info.num_uniform_buffers = (Uint32)num_uniform_buffers;
    return SDL_CreateGPUShader(dev, &info);
}

typedef struct PipelineDesc {
    SDL_GPUShader *vs;
    SDL_GPUShader *fs;
    const SDL_GPUVertexAttribute *attrs;
    int num_attrs;
    int pitch;
    SDL_GPUPrimitiveType primitive;
    int depth_test;
    int depth_write;
    int blend;
    SDL_GPUTextureFormat color_format;
    int has_depth;
} PipelineDesc;

static SDL_GPUGraphicsPipeline *make_pipeline(WaifuSdl3Video *v, const PipelineDesc *d)
{
    SDL_GPUColorTargetDescription color;
    SDL_GPUVertexBufferDescription vb;
    SDL_GPUGraphicsPipelineCreateInfo info;

    SDL_zero(color);
    color.format = d->color_format;
    if (d->blend) {
        color.blend_state.enable_blend = true;
        color.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        color.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        color.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        color.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        color.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        color.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    }

    SDL_zero(vb);
    vb.slot = 0;
    vb.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    vb.pitch = (Uint32)d->pitch;

    SDL_zero(info);
    info.vertex_shader = d->vs;
    info.fragment_shader = d->fs;
    info.primitive_type = d->primitive;
    if (d->num_attrs > 0) {
        info.vertex_input_state.num_vertex_buffers = 1;
        info.vertex_input_state.vertex_buffer_descriptions = &vb;
        info.vertex_input_state.num_vertex_attributes = (Uint32)d->num_attrs;
        info.vertex_input_state.vertex_attributes = d->attrs;
    }
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.depth_stencil_state.enable_depth_test = d->depth_test ? true : false;
    info.depth_stencil_state.enable_depth_write = d->depth_write ? true : false;
    info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    info.target_info.num_color_targets = 1;
    info.target_info.color_target_descriptions = &color;
    info.target_info.has_depth_stencil_target = d->has_depth ? true : false;
    if (d->has_depth) info.target_info.depth_stencil_format = v->depth_format;
    return SDL_CreateGPUGraphicsPipeline(v->dev, &info);
}

static int create_pipelines(WaifuSdl3Video *v)
{
    static const SDL_GPUVertexAttribute scene_attrs[] = {
        { 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0 },
        { 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 12 },
        { 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, 20 },
    };
    static const SDL_GPUVertexAttribute line_attrs[] = {
        { 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0 },
        { 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 12 },
    };
    static const SDL_GPUVertexAttribute ui_attrs[] = {
        { 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 0 },
        { 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 8 },
        { 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 16 },
    };

    SDL_GPUShader *scene_vs = load_shader(v->dev, waifu_sdl3_scene_vert_spv, sizeof(waifu_sdl3_scene_vert_spv), SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
    SDL_GPUShader *scene_fs = load_shader(v->dev, waifu_sdl3_scene_frag_spv, sizeof(waifu_sdl3_scene_frag_spv), SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
    SDL_GPUShader *env_vs = load_shader(v->dev, waifu_sdl3_env_vert_spv, sizeof(waifu_sdl3_env_vert_spv), SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
    SDL_GPUShader *env_fs = load_shader(v->dev, waifu_sdl3_env_frag_spv, sizeof(waifu_sdl3_env_frag_spv), SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
    SDL_GPUShader *image_vs = load_shader(v->dev, waifu_sdl3_image_vert_spv, sizeof(waifu_sdl3_image_vert_spv), SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
    SDL_GPUShader *image_fs = load_shader(v->dev, waifu_sdl3_image_frag_spv, sizeof(waifu_sdl3_image_frag_spv), SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
    SDL_GPUShader *hires_fs = load_shader(v->dev, waifu_sdl3_hires_frag_spv, sizeof(waifu_sdl3_hires_frag_spv), SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
    SDL_GPUShader *line_vs = load_shader(v->dev, waifu_sdl3_line_vert_spv, sizeof(waifu_sdl3_line_vert_spv), SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
    SDL_GPUShader *line_fs = load_shader(v->dev, waifu_sdl3_line_frag_spv, sizeof(waifu_sdl3_line_frag_spv), SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 1);
    SDL_GPUShader *ui_vs = load_shader(v->dev, waifu_sdl3_ui_vert_spv, sizeof(waifu_sdl3_ui_vert_spv), SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
    SDL_GPUShader *ui_fs = load_shader(v->dev, waifu_sdl3_ui_frag_spv, sizeof(waifu_sdl3_ui_frag_spv), SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
    SDL_GPUShader *glyph_fs = load_shader(v->dev, waifu_sdl3_glyph_frag_spv, sizeof(waifu_sdl3_glyph_frag_spv), SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
    SDL_GPUShader *blit_vs = load_shader(v->dev, waifu_sdl3_blit_vert_spv, sizeof(waifu_sdl3_blit_vert_spv), SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
    SDL_GPUShader *blit_fs = load_shader(v->dev, waifu_sdl3_blit_frag_spv, sizeof(waifu_sdl3_blit_frag_spv), SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
    SDL_GPUShader *fullimg_fs = load_shader(v->dev, waifu_sdl3_fullimage_frag_spv, sizeof(waifu_sdl3_fullimage_frag_spv), SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0);
    int ok = scene_vs && scene_fs && env_vs && env_fs && image_vs && image_fs &&
             hires_fs && line_vs && line_fs && ui_vs && ui_fs && glyph_fs && blit_vs && blit_fs && fullimg_fs;

    if (ok) {
        PipelineDesc d;
        SDL_zero(d);
        d.color_format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        d.has_depth = 1;

        d.vs = scene_vs; d.fs = scene_fs;
        d.attrs = scene_attrs; d.num_attrs = 3; d.pitch = (int)sizeof(Sdl3SceneVertex);
        d.primitive = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        d.depth_test = 1; d.depth_write = 1; d.blend = 0;
        v->pl_scene = make_pipeline(v, &d);

        /* Environment (sky gradient + infinite floor): fullscreen triangle, no
           vertex buffer, drawn first as an opaque backdrop with no depth
           interaction (the 3D solids draw over it against the cleared depth). */
        d.vs = env_vs; d.fs = env_fs;
        d.attrs = NULL; d.num_attrs = 0; d.pitch = 0;
        d.depth_test = 0; d.depth_write = 0; d.blend = 0;
        v->pl_env = make_pipeline(v, &d);

        d.vs = image_vs; d.fs = image_fs;
        d.attrs = scene_attrs; d.num_attrs = 3; d.pitch = (int)sizeof(Sdl3ImageVertex);
        d.primitive = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        d.depth_test = 1; d.depth_write = 1; d.blend = 0;
        v->pl_image = make_pipeline(v, &d);

        /* Hi-res card art: same clip-space vertex path as pl_image, but the
           fragment samples a per-card mipmapped texture (linear). Scene variant
           = depth+opaque for board cards; UI variant = blended, no depth, for
           hand/detail cards drawn over the 2D layer. */
        d.vs = image_vs; d.fs = hires_fs;
        d.attrs = scene_attrs; d.num_attrs = 3; d.pitch = (int)sizeof(Sdl3ImageVertex);
        d.depth_test = 1; d.depth_write = 1; d.blend = 0;
        v->pl_hires_scene = make_pipeline(v, &d);
        d.depth_test = 0; d.depth_write = 0; d.blend = 1;
        v->pl_hires_ui = make_pipeline(v, &d);

        d.vs = line_vs; d.fs = line_fs;
        d.attrs = line_attrs; d.num_attrs = 2; d.pitch = (int)sizeof(Sdl3LineVertex);
        d.primitive = SDL_GPU_PRIMITIVETYPE_LINELIST;
        d.depth_test = 1; d.depth_write = 0;
        v->pl_line3d = make_pipeline(v, &d);

        d.vs = ui_vs; d.fs = ui_fs;
        d.attrs = ui_attrs; d.num_attrs = 3; d.pitch = (int)sizeof(Sdl3UiVertex);
        d.primitive = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        d.depth_test = 0; d.depth_write = 0; d.blend = 1;
        v->pl_ui_tris = make_pipeline(v, &d);

        d.primitive = SDL_GPU_PRIMITIVETYPE_LINELIST;
        v->pl_ui_lines = make_pipeline(v, &d);

        /* Text glyphs: same game-space vertex transform as the UI (ui.vert), but
           the fragment samples the persistent FreeType glyph atlas linearly. */
        d.vs = ui_vs; d.fs = glyph_fs;
        d.attrs = ui_attrs; d.num_attrs = 3; d.pitch = (int)sizeof(Sdl3UiVertex);
        d.primitive = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        d.depth_test = 0; d.depth_write = 0; d.blend = 1;
        v->pl_glyph = make_pipeline(v, &d);

        /* 16:9 title/ending: fullscreen triangle into the canvas (opaque). */
        d.vs = blit_vs; d.fs = fullimg_fs;
        d.attrs = NULL; d.num_attrs = 0; d.pitch = 0;
        d.primitive = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        d.depth_test = 0; d.depth_write = 0; d.blend = 0;
        d.color_format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        d.has_depth = 0;
        v->pl_fullimage = make_pipeline(v, &d);

        d.vs = blit_vs; d.fs = blit_fs;
        d.color_format = v->swap_format;
        v->pl_blit = make_pipeline(v, &d);

        ok = v->pl_scene && v->pl_env && v->pl_image && v->pl_hires_scene &&
             v->pl_hires_ui && v->pl_fullimage && v->pl_line3d && v->pl_ui_tris &&
             v->pl_ui_lines && v->pl_glyph && v->pl_blit;
    }

    if (scene_vs) SDL_ReleaseGPUShader(v->dev, scene_vs);
    if (scene_fs) SDL_ReleaseGPUShader(v->dev, scene_fs);
    if (env_vs) SDL_ReleaseGPUShader(v->dev, env_vs);
    if (env_fs) SDL_ReleaseGPUShader(v->dev, env_fs);
    if (image_vs) SDL_ReleaseGPUShader(v->dev, image_vs);
    if (image_fs) SDL_ReleaseGPUShader(v->dev, image_fs);
    if (hires_fs) SDL_ReleaseGPUShader(v->dev, hires_fs);
    if (line_vs) SDL_ReleaseGPUShader(v->dev, line_vs);
    if (line_fs) SDL_ReleaseGPUShader(v->dev, line_fs);
    if (ui_vs) SDL_ReleaseGPUShader(v->dev, ui_vs);
    if (ui_fs) SDL_ReleaseGPUShader(v->dev, ui_fs);
    if (glyph_fs) SDL_ReleaseGPUShader(v->dev, glyph_fs);
    if (blit_vs) SDL_ReleaseGPUShader(v->dev, blit_vs);
    if (blit_fs) SDL_ReleaseGPUShader(v->dev, blit_fs);
    if (fullimg_fs) SDL_ReleaseGPUShader(v->dev, fullimg_fs);
    return ok;
}

/* ---- creation --------------------------------------------------------------- */

static SDL_GPUTexture *create_texture(SDL_GPUDevice *dev, SDL_GPUTextureType type,
                                      SDL_GPUTextureFormat format, Uint32 w, Uint32 h,
                                      Uint32 layers, SDL_GPUTextureUsageFlags usage)
{
    SDL_GPUTextureCreateInfo info;
    SDL_zero(info);
    info.type = type;
    info.format = format;
    info.width = w;
    info.height = h;
    info.layer_count_or_depth = layers;
    info.num_levels = 1;
    info.usage = usage;
    return SDL_CreateGPUTexture(dev, &info);
}

/* Canvas dimensions for a drawable: height is an integer scale N of the game
   height (ceil of the fit, clamped — a mild supersample the present downscale
   antialiases); width follows the drawable's aspect but never narrower than the
   game column (WAIFU_FM_WIDTH*N) and never wider than CANVAS_MAX_ASPECT. */
static void canvas_dims_for(int sw, int sh, int *out_w, int *out_h)
{
    float fit;
    int n, ch, cw, minw, maxw;
    if (sw < 1) sw = 1;
    if (sh < 1) sh = 1;
    fit = (float)sw / (float)WAIFU_FM_WIDTH;
    { float fh = (float)sh / (float)WAIFU_FM_HEIGHT; if (fh < fit) fit = fh; }
    n = (int)ceilf(fit);
    if (n < CANVAS_MIN_SCALE) n = CANVAS_MIN_SCALE;
    if (n > CANVAS_MAX_SCALE) n = CANVAS_MAX_SCALE;
    ch = WAIFU_FM_HEIGHT * n;
    minw = WAIFU_FM_WIDTH * n;
    maxw = (int)(ch * CANVAS_MAX_ASPECT + 0.5f);
    cw = (int)((float)ch * (float)sw / (float)sh + 0.5f);
    if (cw < minw) cw = minw;
    if (cw > maxw) cw = maxw;
    cw &= ~1;                    /* even width keeps the centered column integral */
    if (cw < minw) cw = minw;
    *out_w = cw;
    *out_h = ch;
}

/* (Re)create the offscreen canvas + depth target at the given size. SDL defers
   releasing the old textures until the GPU is done with them, so this is safe
   between frames; the persistent canvas content is dropped
   (canvas_initialized = 0) so the next frame starts from a clean clear. */
static int canvas_ensure(WaifuSdl3Video *v, int w, int h)
{
    if (v->canvas && v->canvas_w == w && v->canvas_h == h) return 1;
    if (v->canvas) SDL_ReleaseGPUTexture(v->dev, v->canvas);
    if (v->depth) SDL_ReleaseGPUTexture(v->dev, v->depth);
    v->canvas = NULL;
    v->depth = NULL;
    v->canvas_w = w;
    v->canvas_h = h;
    v->canvas_scale = h / WAIFU_FM_HEIGHT;
    v->canvas = create_texture(v->dev, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                               (Uint32)v->canvas_w, (Uint32)v->canvas_h, 1,
                               SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
    v->depth = create_texture(v->dev, SDL_GPU_TEXTURETYPE_2D, v->depth_format,
                              (Uint32)v->canvas_w, (Uint32)v->canvas_h, 1,
                              SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET);
    v->canvas_initialized = 0;
    return v->canvas && v->depth;
}

WaifuSdl3Video *waifu_sdl3_video_create(const char *title, int scale,
                                        int fullscreen, int vsync)
{
    WaifuSdl3Video *v = (WaifuSdl3Video *)calloc(1, sizeof(*v));
    int win_w, win_h;
    const char *win_env;
    if (!v) return NULL;
    if (scale < 1) scale = 1;

    v->debug_counts = SDL_getenv("WAIFU_SDL3_DEBUG") != NULL;

    /* Initial window size: default to 16:9 on PC (the game view fills widescreen;
       fixed-layout assets center). Height is the game height at the requested
       scale; width is 16:9 of that. WAIFU_SDL3_WIN=WxH overrides; the canvas
       tracks the drawable regardless, so this only sets the opening size. */
    win_h = WAIFU_FM_HEIGHT * scale;
    win_w = (win_h * 16 + 4) / 9;
    win_env = SDL_getenv("WAIFU_SDL3_WIN");
    if (win_env) {
        int ww = 0, wh = 0;
        if (SDL_sscanf(win_env, "%dx%d", &ww, &wh) == 2 && ww > 0 && wh > 0) {
            win_w = ww;
            win_h = wh;
        }
    }

    v->dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, false, NULL);
    if (!v->dev) goto fail;

    v->window = SDL_CreateWindow(title, win_w, win_h,
                                 SDL_WINDOW_RESIZABLE | (fullscreen ? SDL_WINDOW_FULLSCREEN : 0));
    if (!v->window) goto fail;
    v->fullscreen = fullscreen;

    if (!SDL_ClaimWindowForGPUDevice(v->dev, v->window)) goto fail;
    SDL_SetGPUSwapchainParameters(v->dev, v->window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
                                  vsync ? SDL_GPU_PRESENTMODE_VSYNC : SDL_GPU_PRESENTMODE_IMMEDIATE);
    v->swap_format = SDL_GetGPUSwapchainTextureFormat(v->dev, v->window);

    if (SDL_GPUTextureSupportsFormat(v->dev, SDL_GPU_TEXTUREFORMAT_D24_UNORM,
                                     SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET))
        v->depth_format = SDL_GPU_TEXTUREFORMAT_D24_UNORM;
    else if (SDL_GPUTextureSupportsFormat(v->dev, SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
                                          SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET))
        v->depth_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    else
        v->depth_format = SDL_GPU_TEXTUREFORMAT_D16_UNORM;

    /* Canvas follows the window; start from the requested window size and
       re-evaluate against the actual drawable each present. */
    canvas_dims_for(win_w, win_h, &v->pending_w, &v->pending_h);
    if (!canvas_ensure(v, v->pending_w, v->pending_h)) goto fail;
    v->tile_tex = create_texture(v->dev, SDL_GPU_TEXTURETYPE_2D_ARRAY, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                                 32, 32, SDL3_TILE_MAX_COUNT, SDL_GPU_TEXTUREUSAGE_SAMPLER);
    v->image_tex = create_texture(v->dev, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                                  SDL3_IMAGE_ATLAS_W, SDL3_IMAGE_ATLAS_H, 1, SDL_GPU_TEXTUREUSAGE_SAMPLER);
    if (!v->tile_tex || !v->image_tex) goto fail;

    {
        SDL_GPUSamplerCreateInfo si;
        SDL_zero(si);
        si.min_filter = SDL_GPU_FILTER_NEAREST;
        si.mag_filter = SDL_GPU_FILTER_NEAREST;
        si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        si.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        si.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        v->sampler = SDL_CreateGPUSampler(v->dev, &si);
        si.min_filter = SDL_GPU_FILTER_LINEAR;
        si.mag_filter = SDL_GPU_FILTER_LINEAR;
        v->sampler_lin = SDL_CreateGPUSampler(v->dev, &si);
        si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
        si.max_lod = 1000.0f;
        v->sampler_mip = SDL_CreateGPUSampler(v->dev, &si);
        if (!v->sampler || !v->sampler_lin || !v->sampler_mip) goto fail;
    }

    /* Per-card hi-res texture slots (decoded lazily on first draw). */
    v->hires_count = waifu_sdl3_hires_card_count();
    if (v->hires_count > 0) {
        v->hires_face = (SDL_GPUTexture **)calloc((size_t)v->hires_count, sizeof(*v->hires_face));
        v->hires_big = (SDL_GPUTexture **)calloc((size_t)v->hires_count, sizeof(*v->hires_big));
        v->hires_face_tried = (unsigned char *)calloc((size_t)v->hires_count, 1);
        v->hires_big_tried = (unsigned char *)calloc((size_t)v->hires_count, 1);
        if (!v->hires_face || !v->hires_big || !v->hires_face_tried || !v->hires_big_tried) goto fail;
    }

    if (!create_pipelines(v)) goto fail;

    {
        SDL_GPUTransferBufferCreateInfo ti;
        SDL_GPUBufferCreateInfo bi;
        SDL_zero(ti);
        ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        ti.size = (Uint32)SEG_TOTAL;
        v->vtx_tbuf = SDL_CreateGPUTransferBuffer(v->dev, &ti);
        ti.size = (Uint32)(SDL3_IMAGE_ATLAS_W * SDL3_IMAGE_ATLAS_H * 4 +
                           SDL3_TILE_MAX_COUNT * 32 * 32 * 4);
        v->tex_tbuf = SDL_CreateGPUTransferBuffer(v->dev, &ti);
        SDL_zero(bi);
        bi.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        bi.size = (Uint32)SEG_TOTAL;
        v->vtx_buf = SDL_CreateGPUBuffer(v->dev, &bi);
        if (!v->vtx_tbuf || !v->tex_tbuf || !v->vtx_buf) goto fail;
    }

    return v;

fail:
    waifu_sdl3_video_destroy(v);
    return NULL;
}

/* ---- hi-res textures (PC): cards + 16:9 title/ending ------------------------ */

/* Create a mipmapped RGBA texture from a CPU buffer and upload it synchronously
   (a one-time hitch, like texture streaming). Does not free rgba. NULL on fail. */
static SDL_GPUTexture *make_mipped_texture(WaifuSdl3Video *v, const uint8_t *rgba, int w, int h)
{
    int levels = 1, bytes = w * h * 4;
    SDL_GPUTexture *tex;
    SDL_GPUTransferBuffer *tb;
    SDL_GPUCommandBuffer *cmd;
    SDL_GPUCopyPass *cp;
    SDL_GPUFence *fence;
    void *map;
    { int m = w > h ? w : h; while (m > 1) { m >>= 1; ++levels; } }
    {
        SDL_GPUTextureCreateInfo ti;
        SDL_zero(ti);
        ti.type = SDL_GPU_TEXTURETYPE_2D;
        ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        ti.width = (Uint32)w;
        ti.height = (Uint32)h;
        ti.layer_count_or_depth = 1;
        ti.num_levels = (Uint32)levels;
        ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
        tex = SDL_CreateGPUTexture(v->dev, &ti);
    }
    if (!tex) return NULL;
    {
        SDL_GPUTransferBufferCreateInfo tbi;
        SDL_zero(tbi);
        tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tbi.size = (Uint32)bytes;
        tb = SDL_CreateGPUTransferBuffer(v->dev, &tbi);
    }
    if (!tb) { SDL_ReleaseGPUTexture(v->dev, tex); return NULL; }
    map = SDL_MapGPUTransferBuffer(v->dev, tb, false);
    if (map) { memcpy(map, rgba, (size_t)bytes); SDL_UnmapGPUTransferBuffer(v->dev, tb); }
    if (!map) { SDL_ReleaseGPUTransferBuffer(v->dev, tb); SDL_ReleaseGPUTexture(v->dev, tex); return NULL; }

    cmd = SDL_AcquireGPUCommandBuffer(v->dev);
    if (!cmd) { SDL_ReleaseGPUTransferBuffer(v->dev, tb); SDL_ReleaseGPUTexture(v->dev, tex); return NULL; }
    cp = SDL_BeginGPUCopyPass(cmd);
    {
        SDL_GPUTextureTransferInfo src;
        SDL_GPUTextureRegion dst;
        SDL_zero(src);
        src.transfer_buffer = tb;
        src.pixels_per_row = (Uint32)w;
        src.rows_per_layer = (Uint32)h;
        SDL_zero(dst);
        dst.texture = tex;
        dst.w = (Uint32)w;
        dst.h = (Uint32)h;
        dst.d = 1;
        SDL_UploadToGPUTexture(cp, &src, &dst, false);
    }
    SDL_EndGPUCopyPass(cp);
    if (levels > 1) SDL_GenerateMipmapsForGPUTexture(cmd, tex);
    fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (fence) { SDL_WaitForGPUFences(v->dev, true, &fence, 1); SDL_ReleaseGPUFence(v->dev, fence); }
    SDL_ReleaseGPUTransferBuffer(v->dev, tb);
    return tex;
}

/* Decode + upload card_id's face/big art on first use; cached, NULL if none. */
static SDL_GPUTexture *hires_ensure(WaifuSdl3Video *v, int card_id, int kind)
{
    SDL_GPUTexture **slot;
    unsigned char *tried;
    uint8_t *rgba;
    int w = 0, hh = 0;

    if (card_id < 0 || card_id >= v->hires_count) return NULL;
    if (kind == WAIFU_HIRES_BIG) { slot = &v->hires_big[card_id]; tried = &v->hires_big_tried[card_id]; }
    else                        { slot = &v->hires_face[card_id]; tried = &v->hires_face_tried[card_id]; }
    if (*slot) return *slot;
    if (*tried) return NULL;
    *tried = 1;
    rgba = waifu_sdl3_hires_card_decode(card_id, kind, &w, &hh);
    if (!rgba) return NULL;
    *slot = make_mipped_texture(v, rgba, w, hh);
    free(rgba);
    return *slot;
}

/* Decode + upload the 16:9 title (kind 1) / ending (kind 2) on first use. */
static SDL_GPUTexture *fullimage_ensure(WaifuSdl3Video *v, int kind)
{
    SDL_GPUTexture **slot;
    unsigned char *tried;
    uint8_t *rgba;
    int w = 0, h = 0;
    if (kind == 2) { slot = &v->ending_tex; tried = &v->ending_tried; }
    else           { slot = &v->title_tex;  tried = &v->title_tried; }
    if (*slot) return *slot;
    if (*tried) return NULL;
    *tried = 1;
    rgba = (kind == 2) ? waifu_sdl3_hires_ending_decode(&w, &h)
                       : waifu_sdl3_hires_title_decode(&w, &h);
    if (!rgba) return NULL;
    *slot = make_mipped_texture(v, rgba, w, h);
    free(rgba);
    return *slot;
}

/* Upload the persistent FreeType glyph atlas once (mipmapped, linear). Returns
   the atlas texture, or NULL if the font is unavailable (text then falls back to
   the bitmap font at capture time, so no glyph runs are emitted). */
static SDL_GPUTexture *glyph_ensure(WaifuSdl3Video *v)
{
    if (v->glyph_tex) return v->glyph_tex;
    if (v->glyph_tried) return NULL;
    v->glyph_tried = 1;
    if (waifu_sdl3_text_ready()) {
        int w = 0, h = 0;
        const uint32_t *atlas = waifu_sdl3_glyph_atlas(&w, &h);
        if (atlas && w > 0 && h > 0)
            v->glyph_tex = make_mipped_texture(v, (const uint8_t *)atlas, w, h);
    }
    return v->glyph_tex;
}

/* ---- present ----------------------------------------------------------------- */

/* Upload one segment of the shared vertex buffer. *cycled tracks whether the
   buffer has already been cycled this frame: only the FIRST upload may cycle
   (reallocate the backing) — cycling on every segment scatters the segments
   across discarded allocations so all but the last read back as garbage (the
   3D scene vanished without a fence keeping the buffer idle). Every later
   segment must target that same cycled backing. */
static void upload_segment(SDL_GPUCopyPass *cp, SDL_GPUTransferBuffer *tbuf,
                           SDL_GPUBuffer *buf, int offset, int bytes, int *cycled)
{
    SDL_GPUTransferBufferLocation src;
    SDL_GPUBufferRegion dst;
    if (bytes <= 0) return;
    SDL_zero(src);
    src.transfer_buffer = tbuf;
    src.offset = (Uint32)offset;
    SDL_zero(dst);
    dst.buffer = buf;
    dst.offset = (Uint32)offset;
    dst.size = (Uint32)bytes;
    SDL_UploadToGPUBuffer(cp, &src, &dst, *cycled ? false : true);
    *cycled = 1;
}

static void bind_frag_texture(SDL_GPURenderPass *rp, SDL_GPUTexture *tex, SDL_GPUSampler *sampler)
{
    SDL_GPUTextureSamplerBinding tsb;
    SDL_zero(tsb);
    tsb.texture = tex;
    tsb.sampler = sampler;
    SDL_BindGPUFragmentSamplers(rp, 0, &tsb, 1);
}

/* Draw the UI run list [first_run, run_end). Runs may switch between the
   centered column (normal 2D) and the full canvas (widescreen HUD runs), and
   between the UI transform, the widescreen-HUD transform, and the hi-res card
   transform — tracked as state and re-pushed only on change. Hi-res card runs
   (PC) are interleaved here to preserve draw order. */
static void draw_ui_runs(WaifuSdl3Video *v, SDL_GPUCommandBuffer *cmd,
                         SDL_GPURenderPass *rp, Sdl3SceneFrame *frame,
                         const float ui_screen[4], const float hud_screen[4],
                         const SDL_GPUViewport *vp_col, const SDL_GPUViewport *vp_full,
                         int first_run, int run_end)
{
    static const float xf_one[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
    SDL_GPUGraphicsPipeline *bound = NULL;
    int cur_uni = -1;   /* 0 ui_screen, 1 hud_screen, 2 xf_one */
    int cur_vp = -1;    /* 0 column, 1 full */
    int i;
    for (i = first_run; i < run_end; ++i) {
        const Sdl3UiRun *run = &frame->ui_runs[i];
        SDL_GPUBufferBinding vb;
        int want_vp = run->hud ? 1 : 0;
        if (want_vp != cur_vp) {
            SDL_SetGPUViewport(rp, want_vp ? vp_full : vp_col);
            cur_vp = want_vp;
        }
        if (run->kind == SDL3_UI_RUN_HIRES) {
            const Sdl3HiresDraw *hd = &frame->hires_draws[run->first];
            SDL_GPUTexture *tex = hires_ensure(v, hd->card_id, hd->kind);
            if (!tex) continue;   /* decode failed: skip (frame/outline stay) */
            if (bound != v->pl_hires_ui) { SDL_BindGPUGraphicsPipeline(rp, v->pl_hires_ui); bound = v->pl_hires_ui; }
            if (cur_uni != 2) { SDL_PushGPUVertexUniformData(cmd, 0, xf_one, 4 * sizeof(float)); cur_uni = 2; }
            bind_frag_texture(rp, tex, v->sampler_mip);
            SDL_zero(vb);
            vb.buffer = v->vtx_buf;
            vb.offset = (Uint32)(SEG_HIRES_OFF + hd->first_vertex * (int)sizeof(Sdl3ImageVertex));
            SDL_BindGPUVertexBuffers(rp, 0, &vb, 1);
            SDL_DrawGPUPrimitives(rp, 6, 1, 0, 0);
            continue;
        }
        if (run->count <= 0) continue;
        if (run->kind == SDL3_UI_RUN_GLYPH) {
            if (!v->glyph_tex) continue;   /* font unavailable: skip (never emitted then) */
            if (bound != v->pl_glyph) {
                SDL_BindGPUGraphicsPipeline(rp, v->pl_glyph);
                bound = v->pl_glyph;
                bind_frag_texture(rp, v->glyph_tex, v->sampler_mip);
            }
            {
                int want_uni = run->hud ? 1 : 0;
                if (want_uni != cur_uni) {
                    SDL_PushGPUVertexUniformData(cmd, 0, run->hud ? hud_screen : ui_screen, 4 * sizeof(float));
                    cur_uni = want_uni;
                }
            }
            SDL_zero(vb);
            vb.buffer = v->vtx_buf;
            vb.offset = (Uint32)(SEG_GLYPH_OFF + run->first * (int)sizeof(Sdl3UiVertex));
            SDL_BindGPUVertexBuffers(rp, 0, &vb, 1);
            SDL_DrawGPUPrimitives(rp, (Uint32)run->count, 1, 0, 0);
            continue;
        }
        {
            SDL_GPUGraphicsPipeline *want = (run->kind == SDL3_UI_RUN_TRIS) ? v->pl_ui_tris : v->pl_ui_lines;
            if (want != bound) {
                SDL_BindGPUGraphicsPipeline(rp, want);
                bound = want;
                bind_frag_texture(rp, v->image_tex, v->sampler);
            }
        }
        {
            int want_uni = run->hud ? 1 : 0;
            if (want_uni != cur_uni) {
                SDL_PushGPUVertexUniformData(cmd, 0, run->hud ? hud_screen : ui_screen, 4 * sizeof(float));
                cur_uni = want_uni;
            }
        }
        SDL_zero(vb);
        vb.buffer = v->vtx_buf;
        vb.offset = (Uint32)((run->kind == SDL3_UI_RUN_TRIS ? SEG_UI_OFF : SEG_UILINE_OFF) +
                             run->first * (int)sizeof(Sdl3UiVertex));
        SDL_BindGPUVertexBuffers(rp, 0, &vb, 1);
        SDL_DrawGPUPrimitives(rp, (Uint32)run->count, 1, 0, 0);
    }
    /* Leave the column viewport active for whatever draws next. */
    if (cur_vp == 1) SDL_SetGPUViewport(rp, vp_col);
}

int waifu_sdl3_video_present(WaifuSdl3Video *v, int fade_q8)
{
    Sdl3SceneFrame *frame = waifu_sdl3_scene_frame();
    SDL_GPUCommandBuffer *cmd;
    float fade = fade_q8 < 0 ? 0.0f : (fade_q8 > 256 ? 1.0f : fade_q8 / 256.0f);
    /* Content shaders keep a fade slot for flexibility; the global fade is
       applied at the present blit so it never bakes into the canvas. */
    float one4[4] = { 1.0f, 0.0f, 0.0f, 0.0f };

    if (!v) return 0;

    /* Match the canvas to the drawable seen last frame (resolution-agnostic +
       widescreen). Recreating drops the persistent canvas, so the next frame
       re-clears — imperceptible since resizes are rare. */
    if (v->pending_w != v->canvas_w || v->pending_h != v->canvas_h)
        canvas_ensure(v, v->pending_w, v->pending_h);

    /* Publish the widescreen HUD room (extra game-x beyond WAIFU_FM_WIDTH) so
       the game's duel HUD can reach the screen edges next frame. */
    {
        int extra = v->canvas_w / v->canvas_scale - WAIFU_FM_WIDTH;
        if (extra < 0) extra = 0;
        extra &= ~1;
        waifu_sdl3_set_ui_extra_w(extra);
    }

    if (v->debug_counts && frame->has_content) {
        SDL_Log("sdl3 frame: cleared=%d scene=%d env(sky=%d floor=%d) img=%d hires=%d line3d=%d ui=%d uiline=%d runs=%d bg_runs=%d atlas_h=%d",
                frame->cleared, frame->scene_vert_count, frame->env.has_sky, frame->env.has_floor,
                frame->image_vert_count, frame->hires_draw_count, frame->line_vert_count,
                frame->ui_vert_count, frame->ui_line_vert_count,
                frame->ui_run_count, frame->bg_runs, frame->image_atlas_used_h);
    }

    /* Pre-load every hi-res texture this frame needs BEFORE opening the present
       command buffer. hires_ensure/fullimage_ensure each submit their own copy
       command buffer and wait on a fence; doing that mid-render-pass raced the
       present and produced frame-timing-dependent output. Loading here leaves
       the render pass with cached, ready textures only. */
    if (frame->has_content) {
        int hi;
        for (hi = 0; hi < frame->hires_draw_count; ++hi)
            hires_ensure(v, frame->hires_draws[hi].card_id, frame->hires_draws[hi].kind);
        if (frame->full_image) fullimage_ensure(v, frame->full_image);
        if (frame->ui_glyph_vert_count > 0) glyph_ensure(v);
    }

    cmd = SDL_AcquireGPUCommandBuffer(v->dev);
    if (!cmd) return 0;

    if (frame->has_content) {
        /* --- uploads --- */
        Uint8 *map = (Uint8 *)SDL_MapGPUTransferBuffer(v->dev, v->vtx_tbuf, true);
        if (!map) { SDL_CancelGPUCommandBuffer(cmd); return 0; }
        memcpy(map + SEG_SCENE_OFF, frame->scene_verts, (size_t)frame->scene_vert_count * sizeof(Sdl3SceneVertex));
        memcpy(map + SEG_IMAGE_OFF, frame->image_verts, (size_t)frame->image_vert_count * sizeof(Sdl3ImageVertex));
        memcpy(map + SEG_LINE_OFF, frame->line_verts, (size_t)frame->line_vert_count * sizeof(Sdl3LineVertex));
        memcpy(map + SEG_UI_OFF, frame->ui_verts, (size_t)frame->ui_vert_count * sizeof(Sdl3UiVertex));
        memcpy(map + SEG_UILINE_OFF, frame->ui_line_verts, (size_t)frame->ui_line_vert_count * sizeof(Sdl3UiVertex));
        memcpy(map + SEG_HIRES_OFF, frame->hires_verts, (size_t)frame->hires_draw_count * 6 * sizeof(Sdl3ImageVertex));
        memcpy(map + SEG_GLYPH_OFF, frame->ui_glyph_verts, (size_t)frame->ui_glyph_vert_count * sizeof(Sdl3UiVertex));
        SDL_UnmapGPUTransferBuffer(v->dev, v->vtx_tbuf);

        {
            /* Upload the 32x32 tile array every frame it is used, exactly like
               the per-frame image atlas below. A once-only, serial-gated upload
               is not reliably visible to later frames' render passes without an
               intervening fence (the readback path happened to provide one); the
               live game, which never reads back, showed the whole tile atlas as
               black intermittently — the story-scene "glitch". Re-uploading in
               the same command buffer as the consuming render pass makes the
               intra-buffer barrier guarantee visibility. It is only ~36 KB. */
            int upload_tiles = frame->tile_count > 0;
            int atlas_h = frame->image_atlas_used_h;
            SDL_GPUCopyPass *cp;
            if (upload_tiles || atlas_h > 0) {
                Uint8 *tmap = (Uint8 *)SDL_MapGPUTransferBuffer(v->dev, v->tex_tbuf, true);
                if (!tmap) { SDL_CancelGPUCommandBuffer(cmd); return 0; }
                if (atlas_h > 0)
                    memcpy(tmap, frame->image_atlas, (size_t)atlas_h * SDL3_IMAGE_ATLAS_W * 4);
                if (upload_tiles)
                    memcpy(tmap + SDL3_IMAGE_ATLAS_W * SDL3_IMAGE_ATLAS_H * 4,
                           frame->tile_atlas_rgba, (size_t)frame->tile_count * 32 * 32 * 4);
                SDL_UnmapGPUTransferBuffer(v->dev, v->tex_tbuf);
            }
            cp = SDL_BeginGPUCopyPass(cmd);
            {
                int vtx_cycled = 0;
                upload_segment(cp, v->vtx_tbuf, v->vtx_buf, SEG_SCENE_OFF, frame->scene_vert_count * (int)sizeof(Sdl3SceneVertex), &vtx_cycled);
                upload_segment(cp, v->vtx_tbuf, v->vtx_buf, SEG_IMAGE_OFF, frame->image_vert_count * (int)sizeof(Sdl3ImageVertex), &vtx_cycled);
                upload_segment(cp, v->vtx_tbuf, v->vtx_buf, SEG_LINE_OFF, frame->line_vert_count * (int)sizeof(Sdl3LineVertex), &vtx_cycled);
                upload_segment(cp, v->vtx_tbuf, v->vtx_buf, SEG_UI_OFF, frame->ui_vert_count * (int)sizeof(Sdl3UiVertex), &vtx_cycled);
                upload_segment(cp, v->vtx_tbuf, v->vtx_buf, SEG_UILINE_OFF, frame->ui_line_vert_count * (int)sizeof(Sdl3UiVertex), &vtx_cycled);
                upload_segment(cp, v->vtx_tbuf, v->vtx_buf, SEG_HIRES_OFF, frame->hires_draw_count * 6 * (int)sizeof(Sdl3ImageVertex), &vtx_cycled);
                upload_segment(cp, v->vtx_tbuf, v->vtx_buf, SEG_GLYPH_OFF, frame->ui_glyph_vert_count * (int)sizeof(Sdl3UiVertex), &vtx_cycled);
            }
            if (atlas_h > 0) {
                SDL_GPUTextureTransferInfo src;
                SDL_GPUTextureRegion dst;
                SDL_zero(src);
                src.transfer_buffer = v->tex_tbuf;
                src.offset = 0;
                src.pixels_per_row = SDL3_IMAGE_ATLAS_W;
                src.rows_per_layer = (Uint32)atlas_h;
                SDL_zero(dst);
                dst.texture = v->image_tex;
                dst.w = SDL3_IMAGE_ATLAS_W;
                dst.h = (Uint32)atlas_h;
                dst.d = 1;
                SDL_UploadToGPUTexture(cp, &src, &dst, true);
            }
            if (upload_tiles) {
                int layer;
                for (layer = 0; layer < frame->tile_count; ++layer) {
                    SDL_GPUTextureTransferInfo src;
                    SDL_GPUTextureRegion dst;
                    SDL_zero(src);
                    src.transfer_buffer = v->tex_tbuf;
                    src.offset = (Uint32)(SDL3_IMAGE_ATLAS_W * SDL3_IMAGE_ATLAS_H * 4 + layer * 32 * 32 * 4);
                    src.pixels_per_row = 32;
                    src.rows_per_layer = 32;
                    SDL_zero(dst);
                    dst.texture = v->tile_tex;
                    dst.layer = (Uint32)layer;
                    dst.w = 32;
                    dst.h = 32;
                    dst.d = 1;
                    /* Cycle only on the first layer: cycling reallocates the
                       texture's backing, so cycling on every layer scatters the
                       9 layers across discarded allocations and all but the last
                       come back black (the story-scene tile "glitch", visible
                       whenever no fence kept the texture idle). The remaining
                       layers must target that same cycled backing. */
                    SDL_UploadToGPUTexture(cp, &src, &dst, layer == 0);
                }
                v->tile_serial_uploaded = frame->tile_atlas_serial;
            }
            SDL_EndGPUCopyPass(cp);
        }

        /* --- canvas render pass (persistent: LOAD unless cleared) --- */
        {
            SDL_GPUColorTargetInfo color;
            SDL_GPUDepthStencilTargetInfo depth;
            SDL_GPURenderPass *rp;
            float ui_screen[4] = { (float)WAIFU_FM_WIDTH, (float)WAIFU_FM_HEIGHT, 0.0f, 0.0f };
            int bg_runs = frame->bg_runs >= 0 ? frame->bg_runs : frame->ui_run_count;

            SDL_zero(color);
            color.texture = v->canvas;
            if (frame->cleared || !v->canvas_initialized) {
                color.load_op = SDL_GPU_LOADOP_CLEAR;
                color.clear_color.r = frame->bg_rgba[0];
                color.clear_color.g = frame->bg_rgba[1];
                color.clear_color.b = frame->bg_rgba[2];
                color.clear_color.a = 1.0f;
                v->canvas_initialized = 1;
            } else {
                color.load_op = SDL_GPU_LOADOP_LOAD;
            }
            color.store_op = SDL_GPU_STOREOP_STORE;

            SDL_zero(depth);
            depth.texture = v->depth;
            depth.load_op = SDL_GPU_LOADOP_CLEAR;
            depth.store_op = SDL_GPU_STOREOP_DONT_CARE;
            depth.clear_depth = 1.0f;

            rp = SDL_BeginGPURenderPass(cmd, &color, 1, &depth);
            if (!rp) { SDL_CancelGPUCommandBuffer(cmd); return 0; }

            SDL_PushGPUVertexUniformData(cmd, 0, ui_screen, sizeof(ui_screen));
            SDL_PushGPUFragmentUniformData(cmd, 0, one4, sizeof(one4));

            /* Widescreen: the 3D view (environment + solids) fills the full
               canvas width with a widened horizontal FOV (x *= hs), while the 2D
               UI/pixel-art assets render into a centered WAIFU_FM_WIDTH*N column
               so non-widescreen assets stay undistorted and centered. hs =
               column / canvas ties the two together (the floor ray and the
               solids both use *hs, so they stay aligned). On a game-aspect
               drawable col_w == canvas_w and hs == 1 (no change). */
            {
                int col_w = WAIFU_FM_WIDTH * v->canvas_scale;
                float hs;
                float xf4[4];
                float hud_screen[4];   /* widescreen HUD: game-x [0, canvas_w/N] */
                SDL_GPUViewport vp_full, vp_col;
                if (col_w > v->canvas_w) col_w = v->canvas_w;
                hs = (float)col_w / (float)v->canvas_w;
                xf4[0] = hs; xf4[1] = 0.0f; xf4[2] = 0.0f; xf4[3] = 0.0f;
                hud_screen[0] = (float)v->canvas_w / (float)v->canvas_scale;
                hud_screen[1] = (float)WAIFU_FM_HEIGHT; hud_screen[2] = 0.0f; hud_screen[3] = 0.0f;
                SDL_zero(vp_full);
                vp_full.w = (float)v->canvas_w; vp_full.h = (float)v->canvas_h; vp_full.max_depth = 1.0f;
                SDL_zero(vp_col);
                vp_col.x = (float)((v->canvas_w - col_w) / 2);
                vp_col.w = (float)col_w; vp_col.h = (float)v->canvas_h; vp_col.max_depth = 1.0f;
            /* 0. 16:9 title/ending (PC): fills the full canvas, under everything
               (the logo/menu text overlays in the centered column below). */
            if (frame->full_image) {
                SDL_GPUTexture *fit = fullimage_ensure(v, frame->full_image);
                if (fit) {
                    SDL_SetGPUViewport(rp, &vp_full);
                    SDL_BindGPUGraphicsPipeline(rp, v->pl_fullimage);
                    bind_frag_texture(rp, fit, v->sampler_lin);
                    SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
                }
            }
                SDL_SetGPUViewport(rp, &vp_col);

            /* 1. backdrop (2D captured before the first 3D primitive) */
            draw_ui_runs(v, cmd, rp, frame, ui_screen, hud_screen, &vp_col, &vp_full, 0, bg_runs);

            /* 2-3. the 3D view fills the full width. */
            SDL_SetGPUViewport(rp, &vp_full);
            SDL_PushGPUVertexUniformData(cmd, 0, xf4, sizeof(xf4));

            /* 2. environment: smooth sky gradient + infinite ray-cast floor,
               one fullscreen pass beneath the 3D solids (opaque, no depth). */
            {
                const Sdl3Env *env = &frame->env;
                if (env->has_sky || env->has_floor) {
                    float ep[11 * 4];
                    ep[0] = env->eye[0];   ep[1] = env->eye[1];   ep[2] = env->eye[2];   ep[3]  = env->has_floor ? 1.0f : 0.0f;
                    /* sx*hs widens the horizontal FOV to the full canvas so the
                       floor/sky fill the width while staying aligned with the
                       centered solids; sy (vertical FOV) is unchanged. */
                    ep[4] = env->right[0]; ep[5] = env->right[1]; ep[6] = env->right[2]; ep[7]  = env->sx * hs;
                    ep[8] = env->up[0];    ep[9] = env->up[1];    ep[10] = env->up[2];   ep[11] = env->sy;
                    ep[12] = env->fwd[0];  ep[13] = env->fwd[1];  ep[14] = env->fwd[2];  ep[15] = env->floor_y;
                    ep[16] = env->texels_per_unit; ep[17] = env->tile_a; ep[18] = env->tile_b; ep[19] = env->has_sky ? 1.0f : 0.0f;
                    memcpy(ep + 20, env->sky_stops[0], 4 * sizeof(float));
                    memcpy(ep + 24, env->sky_stops[1], 4 * sizeof(float));
                    memcpy(ep + 28, env->sky_stops[2], 4 * sizeof(float));
                    memcpy(ep + 32, env->sky_stops[3], 4 * sizeof(float));
                    memcpy(ep + 36, env->horizon,      4 * sizeof(float));
                    ep[40] = (float)env->sky_kind; ep[41] = (float)env->sky_hscroll; ep[42] = 0.0f; ep[43] = 0.0f;
                    SDL_BindGPUGraphicsPipeline(rp, v->pl_env);
                    bind_frag_texture(rp, v->tile_tex, v->sampler);
                    SDL_PushGPUFragmentUniformData(cmd, 0, ep, sizeof(ep));
                    SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
                    SDL_PushGPUFragmentUniformData(cmd, 0, one4, sizeof(one4));
                }
            }
            if (frame->scene_vert_count > 0) {
                SDL_GPUBufferBinding vb;
                SDL_BindGPUGraphicsPipeline(rp, v->pl_scene);
                bind_frag_texture(rp, v->tile_tex, v->sampler);
                SDL_zero(vb);
                vb.buffer = v->vtx_buf;
                vb.offset = SEG_SCENE_OFF;
                SDL_BindGPUVertexBuffers(rp, 0, &vb, 1);
                SDL_DrawGPUPrimitives(rp, (Uint32)frame->scene_vert_count, 1, 0, 0);
            }
            if (frame->image_vert_count > 0) {
                SDL_GPUBufferBinding vb;
                SDL_BindGPUGraphicsPipeline(rp, v->pl_image);
                bind_frag_texture(rp, v->image_tex, v->sampler);
                SDL_zero(vb);
                vb.buffer = v->vtx_buf;
                vb.offset = SEG_IMAGE_OFF;
                SDL_BindGPUVertexBuffers(rp, 0, &vb, 1);
                SDL_DrawGPUPrimitives(rp, (Uint32)frame->image_vert_count, 1, 0, 0);
            }
            /* Hi-res board cards (group 0): full-width 3D view, depth-tested, one
               draw per card (its own mipmapped texture). The gold rim outline is
               in line_verts and draws just after, on top. */
            if (frame->hires_draw_count > 0) {
                int hi;
                int bound_hires = 0;
                for (hi = 0; hi < frame->hires_draw_count; ++hi) {
                    const Sdl3HiresDraw *hd = &frame->hires_draws[hi];
                    SDL_GPUTexture *tex;
                    SDL_GPUBufferBinding vb;
                    if (hd->group != 0) continue;
                    tex = hires_ensure(v, hd->card_id, hd->kind);
                    if (!tex) continue;
                    if (!bound_hires) { SDL_BindGPUGraphicsPipeline(rp, v->pl_hires_scene); bound_hires = 1; }
                    bind_frag_texture(rp, tex, v->sampler_mip);
                    SDL_zero(vb);
                    vb.buffer = v->vtx_buf;
                    vb.offset = (Uint32)(SEG_HIRES_OFF + hd->first_vertex * (int)sizeof(Sdl3ImageVertex));
                    SDL_BindGPUVertexBuffers(rp, 0, &vb, 1);
                    SDL_DrawGPUPrimitives(rp, 6, 1, 0, 0);
                }
            }
            if (frame->line_vert_count > 0) {
                SDL_GPUBufferBinding vb;
                SDL_BindGPUGraphicsPipeline(rp, v->pl_line3d);
                SDL_zero(vb);
                vb.buffer = v->vtx_buf;
                vb.offset = SEG_LINE_OFF;
                SDL_BindGPUVertexBuffers(rp, 0, &vb, 1);
                SDL_DrawGPUPrimitives(rp, (Uint32)frame->line_vert_count, 1, 0, 0);
            }

            /* 4. UI above the scene: back to the centered column. */
            SDL_SetGPUViewport(rp, &vp_col);
            SDL_PushGPUVertexUniformData(cmd, 0, ui_screen, sizeof(ui_screen));
            draw_ui_runs(v, cmd, rp, frame, ui_screen, hud_screen, &vp_col, &vp_full, bg_runs, frame->ui_run_count);
            }

            SDL_EndGPURenderPass(rp);
        }
    }

    /* --- present blit (canvas -> swapchain, letterboxed, fade applied) --- */
    {
        SDL_GPUTexture *swap = NULL;
        Uint32 sw = 0, sh = 0;
        if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, v->window, &swap, &sw, &sh)) {
            SDL_CancelGPUCommandBuffer(cmd);
            return 0;
        }
        /* Size next frame's canvas to the drawable we just observed. */
        if (sw > 0 && sh > 0) canvas_dims_for((int)sw, (int)sh, &v->pending_w, &v->pending_h);
        if (swap && sw > 0 && sh > 0 && v->canvas_initialized) {
            SDL_GPUColorTargetInfo color;
            SDL_GPURenderPass *rp;
            float fade4[4] = { fade, 0.0f, 0.0f, 0.0f };
            float scale_x = (float)sw / (float)v->canvas_w;
            float scale_y = (float)sh / (float)v->canvas_h;
            float s = scale_x < scale_y ? scale_x : scale_y;
            SDL_GPUViewport vp;

            SDL_zero(color);
            color.texture = swap;
            color.load_op = SDL_GPU_LOADOP_CLEAR;
            color.store_op = SDL_GPU_STOREOP_STORE;
            color.clear_color.a = 1.0f;

            rp = SDL_BeginGPURenderPass(cmd, &color, 1, NULL);
            if (rp) {
                vp.w = v->canvas_w * s;
                vp.h = v->canvas_h * s;
                vp.x = ((float)sw - vp.w) * 0.5f;
                vp.y = ((float)sh - vp.h) * 0.5f;
                vp.min_depth = 0.0f;
                vp.max_depth = 1.0f;
                SDL_SetGPUViewport(rp, &vp);
                SDL_BindGPUGraphicsPipeline(rp, v->pl_blit);
                bind_frag_texture(rp, v->canvas, v->sampler_lin);
                SDL_PushGPUFragmentUniformData(cmd, 0, fade4, sizeof(fade4));
                SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
                SDL_EndGPURenderPass(rp);
            }
        }
    }

    if (!SDL_SubmitGPUCommandBuffer(cmd)) return 0;
    waifu_sdl3_env_consume_sky();   /* sky persists across the mid-frame clear; drop it here */
    waifu_sdl3_scene_frame_reset();
    return 1;
}

/* ---- readback ----------------------------------------------------------------- */

int waifu_sdl3_video_render_width(const WaifuSdl3Video *v) { return v->canvas_w; }
int waifu_sdl3_video_render_height(const WaifuSdl3Video *v) { return v->canvas_h; }

int waifu_sdl3_video_read_frame(WaifuSdl3Video *v, uint8_t *rgba)
{
    SDL_GPUCommandBuffer *cmd;
    SDL_GPUCopyPass *cp;
    SDL_GPUFence *fence;
    void *map;
    int need = v->canvas_w * v->canvas_h * 4;

    if (!v || !rgba) return 0;
    /* The canvas size can change with the window, so size the download buffer
       to the current canvas (recreate when it grew). */
    if (!v->read_tbuf || v->read_tbuf_bytes < need) {
        SDL_GPUTransferBufferCreateInfo ti;
        if (v->read_tbuf) SDL_ReleaseGPUTransferBuffer(v->dev, v->read_tbuf);
        SDL_zero(ti);
        ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
        ti.size = (Uint32)need;
        v->read_tbuf = SDL_CreateGPUTransferBuffer(v->dev, &ti);
        if (!v->read_tbuf) { v->read_tbuf_bytes = 0; return 0; }
        v->read_tbuf_bytes = need;
    }

    cmd = SDL_AcquireGPUCommandBuffer(v->dev);
    if (!cmd) return 0;
    cp = SDL_BeginGPUCopyPass(cmd);
    {
        SDL_GPUTextureRegion src;
        SDL_GPUTextureTransferInfo dst;
        SDL_zero(src);
        src.texture = v->canvas;
        src.w = (Uint32)v->canvas_w;
        src.h = (Uint32)v->canvas_h;
        src.d = 1;
        SDL_zero(dst);
        dst.transfer_buffer = v->read_tbuf;
        dst.pixels_per_row = (Uint32)v->canvas_w;
        dst.rows_per_layer = (Uint32)v->canvas_h;
        SDL_DownloadFromGPUTexture(cp, &src, &dst);
    }
    SDL_EndGPUCopyPass(cp);
    fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (!fence) return 0;
    SDL_WaitForGPUFences(v->dev, true, &fence, 1);
    SDL_ReleaseGPUFence(v->dev, fence);

    map = SDL_MapGPUTransferBuffer(v->dev, v->read_tbuf, false);
    if (!map) return 0;
    memcpy(rgba, map, (size_t)need);
    SDL_UnmapGPUTransferBuffer(v->dev, v->read_tbuf);
    return 1;
}

void waifu_sdl3_video_toggle_fullscreen(WaifuSdl3Video *v)
{
    if (!v) return;
    v->fullscreen = !v->fullscreen;
    SDL_SetWindowFullscreen(v->window, v->fullscreen);
}

void waifu_sdl3_video_destroy(WaifuSdl3Video *v)
{
    if (!v) return;
    if (v->dev) {
        SDL_WaitForGPUIdle(v->dev);
        if (v->pl_scene) SDL_ReleaseGPUGraphicsPipeline(v->dev, v->pl_scene);
        if (v->pl_env) SDL_ReleaseGPUGraphicsPipeline(v->dev, v->pl_env);
        if (v->pl_image) SDL_ReleaseGPUGraphicsPipeline(v->dev, v->pl_image);
        if (v->pl_hires_scene) SDL_ReleaseGPUGraphicsPipeline(v->dev, v->pl_hires_scene);
        if (v->pl_hires_ui) SDL_ReleaseGPUGraphicsPipeline(v->dev, v->pl_hires_ui);
        if (v->pl_fullimage) SDL_ReleaseGPUGraphicsPipeline(v->dev, v->pl_fullimage);
        if (v->pl_line3d) SDL_ReleaseGPUGraphicsPipeline(v->dev, v->pl_line3d);
        if (v->pl_ui_tris) SDL_ReleaseGPUGraphicsPipeline(v->dev, v->pl_ui_tris);
        if (v->pl_ui_lines) SDL_ReleaseGPUGraphicsPipeline(v->dev, v->pl_ui_lines);
        if (v->pl_glyph) SDL_ReleaseGPUGraphicsPipeline(v->dev, v->pl_glyph);
        if (v->pl_blit) SDL_ReleaseGPUGraphicsPipeline(v->dev, v->pl_blit);
        if (v->canvas) SDL_ReleaseGPUTexture(v->dev, v->canvas);
        if (v->depth) SDL_ReleaseGPUTexture(v->dev, v->depth);
        if (v->tile_tex) SDL_ReleaseGPUTexture(v->dev, v->tile_tex);
        if (v->image_tex) SDL_ReleaseGPUTexture(v->dev, v->image_tex);
        {
            int c;
            for (c = 0; c < v->hires_count; ++c) {
                if (v->hires_face && v->hires_face[c]) SDL_ReleaseGPUTexture(v->dev, v->hires_face[c]);
                if (v->hires_big && v->hires_big[c]) SDL_ReleaseGPUTexture(v->dev, v->hires_big[c]);
            }
        }
        if (v->title_tex) SDL_ReleaseGPUTexture(v->dev, v->title_tex);
        if (v->ending_tex) SDL_ReleaseGPUTexture(v->dev, v->ending_tex);
        if (v->glyph_tex) SDL_ReleaseGPUTexture(v->dev, v->glyph_tex);
        if (v->sampler) SDL_ReleaseGPUSampler(v->dev, v->sampler);
        if (v->sampler_lin) SDL_ReleaseGPUSampler(v->dev, v->sampler_lin);
        if (v->sampler_mip) SDL_ReleaseGPUSampler(v->dev, v->sampler_mip);
        if (v->vtx_tbuf) SDL_ReleaseGPUTransferBuffer(v->dev, v->vtx_tbuf);
        if (v->tex_tbuf) SDL_ReleaseGPUTransferBuffer(v->dev, v->tex_tbuf);
        if (v->read_tbuf) SDL_ReleaseGPUTransferBuffer(v->dev, v->read_tbuf);
        if (v->vtx_buf) SDL_ReleaseGPUBuffer(v->dev, v->vtx_buf);
        if (v->window) SDL_ReleaseWindowFromGPUDevice(v->dev, v->window);
        SDL_DestroyGPUDevice(v->dev);
    }
    free(v->hires_face);
    free(v->hires_big);
    free(v->hires_face_tried);
    free(v->hires_big_tried);
    if (v->window) SDL_DestroyWindow(v->window);
    free(v);
}
