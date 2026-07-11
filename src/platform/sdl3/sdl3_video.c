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

#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "sdl3_internal.h"
#include "sdl3_shaders.h"

#define RENDER_SCALE 5
#define RENDER_W (WAIFU_FM_WIDTH * RENDER_SCALE)
#define RENDER_H (WAIFU_FM_HEIGHT * RENDER_SCALE)

/* Vertex-buffer layout: one GPU buffer, fixed segment offsets. */
#define SEG_SCENE_OFF   0
#define SEG_SCENE_SIZE  (SDL3_SCENE_MAX_SCENE_VERTS * (int)sizeof(Sdl3SceneVertex))
#define SEG_FLOOR_OFF   (SEG_SCENE_OFF + SEG_SCENE_SIZE)
#define SEG_FLOOR_SIZE  (SDL3_SCENE_MAX_FLOORS * SDL3_FLOOR_VERTS_PER_DRAW * (int)sizeof(Sdl3FloorVertex))
#define SEG_IMAGE_OFF   (SEG_FLOOR_OFF + SEG_FLOOR_SIZE)
#define SEG_IMAGE_SIZE  (SDL3_SCENE_MAX_IMAGE_VERTS * (int)sizeof(Sdl3ImageVertex))
#define SEG_LINE_OFF    (SEG_IMAGE_OFF + SEG_IMAGE_SIZE)
#define SEG_LINE_SIZE   (SDL3_SCENE_MAX_LINE_VERTS * (int)sizeof(Sdl3LineVertex))
#define SEG_UI_OFF      (SEG_LINE_OFF + SEG_LINE_SIZE)
#define SEG_UI_SIZE     (SDL3_UI_MAX_VERTS * (int)sizeof(Sdl3UiVertex))
#define SEG_UILINE_OFF  (SEG_UI_OFF + SEG_UI_SIZE)
#define SEG_UILINE_SIZE (SDL3_UI_MAX_LINE_VERTS * (int)sizeof(Sdl3UiVertex))
#define SEG_TOTAL       (SEG_UILINE_OFF + SEG_UILINE_SIZE)

struct WaifuSdl3Video {
    SDL_Window *window;
    SDL_GPUDevice *dev;

    SDL_GPUTexture *canvas;      /* persistent RENDER_W x RENDER_H RGBA8 */
    SDL_GPUTexture *depth;
    SDL_GPUTextureFormat depth_format;
    SDL_GPUTextureFormat swap_format;
    SDL_GPUTexture *tile_tex;    /* 32x32 RGBA8 array */
    SDL_GPUTexture *image_tex;   /* streaming RGBA atlas */
    SDL_GPUSampler *sampler;     /* nearest (texelFetch paths) */
    SDL_GPUSampler *sampler_lin; /* linear (present blit) */

    SDL_GPUGraphicsPipeline *pl_scene;
    SDL_GPUGraphicsPipeline *pl_floor;
    SDL_GPUGraphicsPipeline *pl_image;
    SDL_GPUGraphicsPipeline *pl_line3d;
    SDL_GPUGraphicsPipeline *pl_ui_tris;
    SDL_GPUGraphicsPipeline *pl_ui_lines;
    SDL_GPUGraphicsPipeline *pl_blit;

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
    static const SDL_GPUVertexAttribute floor_attrs[] = {
        { 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0 },
        { 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 12 },
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

    SDL_GPUShader *scene_vs = load_shader(v->dev, waifu_sdl3_scene_vert_spv, sizeof(waifu_sdl3_scene_vert_spv), SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
    SDL_GPUShader *scene_fs = load_shader(v->dev, waifu_sdl3_scene_frag_spv, sizeof(waifu_sdl3_scene_frag_spv), SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
    SDL_GPUShader *floor_vs = load_shader(v->dev, waifu_sdl3_floor_vert_spv, sizeof(waifu_sdl3_floor_vert_spv), SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
    SDL_GPUShader *floor_fs = load_shader(v->dev, waifu_sdl3_floor_frag_spv, sizeof(waifu_sdl3_floor_frag_spv), SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
    SDL_GPUShader *image_vs = load_shader(v->dev, waifu_sdl3_image_vert_spv, sizeof(waifu_sdl3_image_vert_spv), SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
    SDL_GPUShader *image_fs = load_shader(v->dev, waifu_sdl3_image_frag_spv, sizeof(waifu_sdl3_image_frag_spv), SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
    SDL_GPUShader *line_vs = load_shader(v->dev, waifu_sdl3_line_vert_spv, sizeof(waifu_sdl3_line_vert_spv), SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
    SDL_GPUShader *line_fs = load_shader(v->dev, waifu_sdl3_line_frag_spv, sizeof(waifu_sdl3_line_frag_spv), SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 1);
    SDL_GPUShader *ui_vs = load_shader(v->dev, waifu_sdl3_ui_vert_spv, sizeof(waifu_sdl3_ui_vert_spv), SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
    SDL_GPUShader *ui_fs = load_shader(v->dev, waifu_sdl3_ui_frag_spv, sizeof(waifu_sdl3_ui_frag_spv), SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
    SDL_GPUShader *blit_vs = load_shader(v->dev, waifu_sdl3_blit_vert_spv, sizeof(waifu_sdl3_blit_vert_spv), SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
    SDL_GPUShader *blit_fs = load_shader(v->dev, waifu_sdl3_blit_frag_spv, sizeof(waifu_sdl3_blit_frag_spv), SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
    int ok = scene_vs && scene_fs && floor_vs && floor_fs && image_vs && image_fs &&
             line_vs && line_fs && ui_vs && ui_fs && blit_vs && blit_fs;

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

        d.vs = floor_vs; d.fs = floor_fs;
        d.attrs = floor_attrs; d.num_attrs = 2; d.pitch = (int)sizeof(Sdl3FloorVertex);
        v->pl_floor = make_pipeline(v, &d);

        d.vs = image_vs; d.fs = image_fs;
        d.attrs = scene_attrs; d.num_attrs = 3; d.pitch = (int)sizeof(Sdl3ImageVertex);
        v->pl_image = make_pipeline(v, &d);

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

        d.vs = blit_vs; d.fs = blit_fs;
        d.attrs = NULL; d.num_attrs = 0; d.pitch = 0;
        d.primitive = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        d.depth_test = 0; d.depth_write = 0; d.blend = 0;
        d.color_format = v->swap_format;
        d.has_depth = 0;
        v->pl_blit = make_pipeline(v, &d);

        ok = v->pl_scene && v->pl_floor && v->pl_image && v->pl_line3d &&
             v->pl_ui_tris && v->pl_ui_lines && v->pl_blit;
    }

    if (scene_vs) SDL_ReleaseGPUShader(v->dev, scene_vs);
    if (scene_fs) SDL_ReleaseGPUShader(v->dev, scene_fs);
    if (floor_vs) SDL_ReleaseGPUShader(v->dev, floor_vs);
    if (floor_fs) SDL_ReleaseGPUShader(v->dev, floor_fs);
    if (image_vs) SDL_ReleaseGPUShader(v->dev, image_vs);
    if (image_fs) SDL_ReleaseGPUShader(v->dev, image_fs);
    if (line_vs) SDL_ReleaseGPUShader(v->dev, line_vs);
    if (line_fs) SDL_ReleaseGPUShader(v->dev, line_fs);
    if (ui_vs) SDL_ReleaseGPUShader(v->dev, ui_vs);
    if (ui_fs) SDL_ReleaseGPUShader(v->dev, ui_fs);
    if (blit_vs) SDL_ReleaseGPUShader(v->dev, blit_vs);
    if (blit_fs) SDL_ReleaseGPUShader(v->dev, blit_fs);
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

WaifuSdl3Video *waifu_sdl3_video_create(const char *title, int scale,
                                        int fullscreen, int vsync)
{
    WaifuSdl3Video *v = (WaifuSdl3Video *)calloc(1, sizeof(*v));
    if (!v) return NULL;
    if (scale < 1) scale = 1;

    v->debug_counts = SDL_getenv("WAIFU_SDL3_DEBUG") != NULL;

    v->dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, false, NULL);
    if (!v->dev) goto fail;

    v->window = SDL_CreateWindow(title, WAIFU_FM_WIDTH * scale, WAIFU_FM_HEIGHT * scale,
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

    v->canvas = create_texture(v->dev, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                               RENDER_W, RENDER_H, 1,
                               SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
    v->depth = create_texture(v->dev, SDL_GPU_TEXTURETYPE_2D, v->depth_format,
                              RENDER_W, RENDER_H, 1, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET);
    v->tile_tex = create_texture(v->dev, SDL_GPU_TEXTURETYPE_2D_ARRAY, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                                 32, 32, SDL3_TILE_MAX_COUNT, SDL_GPU_TEXTUREUSAGE_SAMPLER);
    v->image_tex = create_texture(v->dev, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                                  SDL3_IMAGE_ATLAS_W, SDL3_IMAGE_ATLAS_H, 1, SDL_GPU_TEXTUREUSAGE_SAMPLER);
    if (!v->canvas || !v->depth || !v->tile_tex || !v->image_tex) goto fail;

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
        if (!v->sampler || !v->sampler_lin) goto fail;
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

/* ---- present ----------------------------------------------------------------- */

static void upload_segment(SDL_GPUCopyPass *cp, SDL_GPUTransferBuffer *tbuf,
                           SDL_GPUBuffer *buf, int offset, int bytes)
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
    SDL_UploadToGPUBuffer(cp, &src, &dst, true);
}

static void bind_frag_texture(SDL_GPURenderPass *rp, SDL_GPUTexture *tex, SDL_GPUSampler *sampler)
{
    SDL_GPUTextureSamplerBinding tsb;
    SDL_zero(tsb);
    tsb.texture = tex;
    tsb.sampler = sampler;
    SDL_BindGPUFragmentSamplers(rp, 0, &tsb, 1);
}

static void draw_ui_runs(WaifuSdl3Video *v, SDL_GPURenderPass *rp,
                         const Sdl3SceneFrame *frame, int first_run, int run_end)
{
    SDL_GPUGraphicsPipeline *bound = NULL;
    int i;
    for (i = first_run; i < run_end; ++i) {
        const Sdl3UiRun *run = &frame->ui_runs[i];
        SDL_GPUBufferBinding vb;
        SDL_GPUGraphicsPipeline *want = (run->kind == SDL3_UI_RUN_TRIS) ? v->pl_ui_tris : v->pl_ui_lines;
        if (run->count <= 0) continue;
        if (want != bound) {
            SDL_BindGPUGraphicsPipeline(rp, want);
            bound = want;
            bind_frag_texture(rp, v->image_tex, v->sampler);
        }
        SDL_zero(vb);
        vb.buffer = v->vtx_buf;
        vb.offset = (Uint32)((run->kind == SDL3_UI_RUN_TRIS ? SEG_UI_OFF : SEG_UILINE_OFF) +
                             run->first * (int)sizeof(Sdl3UiVertex));
        SDL_BindGPUVertexBuffers(rp, 0, &vb, 1);
        SDL_DrawGPUPrimitives(rp, (Uint32)run->count, 1, 0, 0);
    }
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

    if (v->debug_counts && frame->has_content) {
        SDL_Log("sdl3 frame: cleared=%d scene=%d floors=%d img=%d line3d=%d ui=%d uiline=%d runs=%d bg_runs=%d atlas_h=%d",
                frame->cleared, frame->scene_vert_count, frame->floor_count,
                frame->image_vert_count, frame->line_vert_count,
                frame->ui_vert_count, frame->ui_line_vert_count,
                frame->ui_run_count, frame->bg_runs, frame->image_atlas_used_h);
    }

    cmd = SDL_AcquireGPUCommandBuffer(v->dev);
    if (!cmd) return 0;

    if (frame->has_content) {
        /* --- uploads --- */
        Uint8 *map = (Uint8 *)SDL_MapGPUTransferBuffer(v->dev, v->vtx_tbuf, true);
        if (!map) { SDL_CancelGPUCommandBuffer(cmd); return 0; }
        memcpy(map + SEG_SCENE_OFF, frame->scene_verts, (size_t)frame->scene_vert_count * sizeof(Sdl3SceneVertex));
        memcpy(map + SEG_FLOOR_OFF, frame->floor_verts, (size_t)frame->floor_count * SDL3_FLOOR_VERTS_PER_DRAW * sizeof(Sdl3FloorVertex));
        memcpy(map + SEG_IMAGE_OFF, frame->image_verts, (size_t)frame->image_vert_count * sizeof(Sdl3ImageVertex));
        memcpy(map + SEG_LINE_OFF, frame->line_verts, (size_t)frame->line_vert_count * sizeof(Sdl3LineVertex));
        memcpy(map + SEG_UI_OFF, frame->ui_verts, (size_t)frame->ui_vert_count * sizeof(Sdl3UiVertex));
        memcpy(map + SEG_UILINE_OFF, frame->ui_line_verts, (size_t)frame->ui_line_vert_count * sizeof(Sdl3UiVertex));
        SDL_UnmapGPUTransferBuffer(v->dev, v->vtx_tbuf);

        {
            int upload_tiles = (frame->tile_atlas_serial != v->tile_serial_uploaded) && frame->tile_count > 0;
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
            upload_segment(cp, v->vtx_tbuf, v->vtx_buf, SEG_SCENE_OFF, frame->scene_vert_count * (int)sizeof(Sdl3SceneVertex));
            upload_segment(cp, v->vtx_tbuf, v->vtx_buf, SEG_FLOOR_OFF, frame->floor_count * SDL3_FLOOR_VERTS_PER_DRAW * (int)sizeof(Sdl3FloorVertex));
            upload_segment(cp, v->vtx_tbuf, v->vtx_buf, SEG_IMAGE_OFF, frame->image_vert_count * (int)sizeof(Sdl3ImageVertex));
            upload_segment(cp, v->vtx_tbuf, v->vtx_buf, SEG_LINE_OFF, frame->line_vert_count * (int)sizeof(Sdl3LineVertex));
            upload_segment(cp, v->vtx_tbuf, v->vtx_buf, SEG_UI_OFF, frame->ui_vert_count * (int)sizeof(Sdl3UiVertex));
            upload_segment(cp, v->vtx_tbuf, v->vtx_buf, SEG_UILINE_OFF, frame->ui_line_vert_count * (int)sizeof(Sdl3UiVertex));
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
                    SDL_UploadToGPUTexture(cp, &src, &dst, true);
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

            /* 1. backdrop (2D captured before the first 3D primitive) */
            draw_ui_runs(v, rp, frame, 0, bg_runs);

            /* 2. 3D scene */
            if (frame->floor_count > 0) {
                int i;
                SDL_BindGPUGraphicsPipeline(rp, v->pl_floor);
                bind_frag_texture(rp, v->tile_tex, v->sampler);
                for (i = 0; i < frame->floor_count; ++i) {
                    const Sdl3FloorDraw *fd = &frame->floors[i];
                    float params[4] = { fd->texels_per_unit, fd->tile_a, fd->tile_b, 1.0f };
                    SDL_GPUBufferBinding vb;
                    SDL_PushGPUFragmentUniformData(cmd, 0, params, sizeof(params));
                    SDL_zero(vb);
                    vb.buffer = v->vtx_buf;
                    vb.offset = (Uint32)(SEG_FLOOR_OFF + fd->first_vertex * (int)sizeof(Sdl3FloorVertex));
                    SDL_BindGPUVertexBuffers(rp, 0, &vb, 1);
                    SDL_DrawGPUPrimitives(rp, (Uint32)fd->vertex_count, 1, 0, 0);
                }
                SDL_PushGPUFragmentUniformData(cmd, 0, one4, sizeof(one4));
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
            if (frame->line_vert_count > 0) {
                SDL_GPUBufferBinding vb;
                SDL_BindGPUGraphicsPipeline(rp, v->pl_line3d);
                SDL_zero(vb);
                vb.buffer = v->vtx_buf;
                vb.offset = SEG_LINE_OFF;
                SDL_BindGPUVertexBuffers(rp, 0, &vb, 1);
                SDL_DrawGPUPrimitives(rp, (Uint32)frame->line_vert_count, 1, 0, 0);
            }

            /* 3. UI above the scene */
            draw_ui_runs(v, rp, frame, bg_runs, frame->ui_run_count);

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
        if (swap && sw > 0 && sh > 0 && v->canvas_initialized) {
            SDL_GPUColorTargetInfo color;
            SDL_GPURenderPass *rp;
            float fade4[4] = { fade, 0.0f, 0.0f, 0.0f };
            float scale_x = (float)sw / (float)RENDER_W;
            float scale_y = (float)sh / (float)RENDER_H;
            float s = scale_x < scale_y ? scale_x : scale_y;
            SDL_GPUViewport vp;

            SDL_zero(color);
            color.texture = swap;
            color.load_op = SDL_GPU_LOADOP_CLEAR;
            color.store_op = SDL_GPU_STOREOP_STORE;
            color.clear_color.a = 1.0f;

            rp = SDL_BeginGPURenderPass(cmd, &color, 1, NULL);
            if (rp) {
                vp.w = RENDER_W * s;
                vp.h = RENDER_H * s;
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
    waifu_sdl3_scene_frame_reset();
    return 1;
}

/* ---- readback ----------------------------------------------------------------- */

int waifu_sdl3_video_render_width(const WaifuSdl3Video *v) { (void)v; return RENDER_W; }
int waifu_sdl3_video_render_height(const WaifuSdl3Video *v) { (void)v; return RENDER_H; }

int waifu_sdl3_video_read_frame(WaifuSdl3Video *v, uint8_t *rgba)
{
    SDL_GPUCommandBuffer *cmd;
    SDL_GPUCopyPass *cp;
    SDL_GPUFence *fence;
    void *map;

    if (!v || !rgba) return 0;
    if (!v->read_tbuf) {
        SDL_GPUTransferBufferCreateInfo ti;
        SDL_zero(ti);
        ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
        ti.size = RENDER_W * RENDER_H * 4;
        v->read_tbuf = SDL_CreateGPUTransferBuffer(v->dev, &ti);
        if (!v->read_tbuf) return 0;
    }

    cmd = SDL_AcquireGPUCommandBuffer(v->dev);
    if (!cmd) return 0;
    cp = SDL_BeginGPUCopyPass(cmd);
    {
        SDL_GPUTextureRegion src;
        SDL_GPUTextureTransferInfo dst;
        SDL_zero(src);
        src.texture = v->canvas;
        src.w = RENDER_W;
        src.h = RENDER_H;
        src.d = 1;
        SDL_zero(dst);
        dst.transfer_buffer = v->read_tbuf;
        dst.pixels_per_row = RENDER_W;
        dst.rows_per_layer = RENDER_H;
        SDL_DownloadFromGPUTexture(cp, &src, &dst);
    }
    SDL_EndGPUCopyPass(cp);
    fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (!fence) return 0;
    SDL_WaitForGPUFences(v->dev, true, &fence, 1);
    SDL_ReleaseGPUFence(v->dev, fence);

    map = SDL_MapGPUTransferBuffer(v->dev, v->read_tbuf, false);
    if (!map) return 0;
    memcpy(rgba, map, RENDER_W * RENDER_H * 4);
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
        if (v->pl_floor) SDL_ReleaseGPUGraphicsPipeline(v->dev, v->pl_floor);
        if (v->pl_image) SDL_ReleaseGPUGraphicsPipeline(v->dev, v->pl_image);
        if (v->pl_line3d) SDL_ReleaseGPUGraphicsPipeline(v->dev, v->pl_line3d);
        if (v->pl_ui_tris) SDL_ReleaseGPUGraphicsPipeline(v->dev, v->pl_ui_tris);
        if (v->pl_ui_lines) SDL_ReleaseGPUGraphicsPipeline(v->dev, v->pl_ui_lines);
        if (v->pl_blit) SDL_ReleaseGPUGraphicsPipeline(v->dev, v->pl_blit);
        if (v->canvas) SDL_ReleaseGPUTexture(v->dev, v->canvas);
        if (v->depth) SDL_ReleaseGPUTexture(v->dev, v->depth);
        if (v->tile_tex) SDL_ReleaseGPUTexture(v->dev, v->tile_tex);
        if (v->image_tex) SDL_ReleaseGPUTexture(v->dev, v->image_tex);
        if (v->sampler) SDL_ReleaseGPUSampler(v->dev, v->sampler);
        if (v->sampler_lin) SDL_ReleaseGPUSampler(v->dev, v->sampler_lin);
        if (v->vtx_tbuf) SDL_ReleaseGPUTransferBuffer(v->dev, v->vtx_tbuf);
        if (v->tex_tbuf) SDL_ReleaseGPUTransferBuffer(v->dev, v->tex_tbuf);
        if (v->read_tbuf) SDL_ReleaseGPUTransferBuffer(v->dev, v->read_tbuf);
        if (v->vtx_buf) SDL_ReleaseGPUBuffer(v->dev, v->vtx_buf);
        if (v->window) SDL_ReleaseWindowFromGPUDevice(v->dev, v->window);
        SDL_DestroyGPUDevice(v->dev);
    }
    if (v->window) SDL_DestroyWindow(v->window);
    free(v);
}
