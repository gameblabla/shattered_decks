#version 450

/* 16:9 title / ending image (PC): sample the source across the full canvas,
   linear-filtered. Paired with blit.vert's fullscreen triangle. Drawn into the
   persistent canvas (the global fade is applied later at the present blit). */

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

layout(set = 2, binding = 0) uniform sampler2D u_image;

/* Sub-rectangle of the source to frame, as uv (x0,y0,x1,y1). The whole image is
   (0,0,1,1); the title's attract sequence walks a smaller window across it. */
layout(set = 3, binding = 0) uniform FullImageParams {
    vec4 rect;
} u_params;

void main()
{
    vec2 uv = mix(u_params.rect.xy, u_params.rect.zw, v_uv);
    o_color = vec4(texture(u_image, uv).rgb, 1.0);
}
