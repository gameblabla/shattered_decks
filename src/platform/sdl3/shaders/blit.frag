#version 450

/* Present blit: sample the persistent canvas and apply the global fade here,
   so fades never bake into the canvas (which persists across frames like the
   game's framebuffer). */

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

layout(set = 2, binding = 0) uniform sampler2D u_canvas;

layout(set = 3, binding = 0) uniform BlitParams {
    vec4 fade; /* x = fade factor 0..1 */
} u_params;

void main()
{
    o_color = vec4(texture(u_canvas, v_uv).rgb * u_params.fade.x, 1.0);
}
