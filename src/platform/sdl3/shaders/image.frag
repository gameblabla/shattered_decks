#version 450

/* 3D card-face quads sampling the per-frame streaming RGBA image atlas. */

layout(location = 0) in vec2 v_uv;
layout(location = 1) in float v_gray;
layout(location = 0) out vec4 o_color;

layout(set = 2, binding = 0) uniform sampler2D u_image;

layout(set = 3, binding = 0) uniform ImageParams {
    vec4 fade; /* x = fade factor 0..1 */
} u_params;

void main()
{
    ivec2 size = textureSize(u_image, 0);
    ivec2 texel = clamp(ivec2(v_uv), ivec2(0), size - ivec2(1));
    vec4 c = texelFetch(u_image, texel, 0);
    /* Grayed (used) cards: the software dithers toward a dim tone; dim
       uniformly here, which reads the same at GPU resolution. */
    if (v_gray > 0.5) c.rgb *= 0.55;
    o_color = vec4(c.rgb * u_params.fade.x, 1.0);
}
