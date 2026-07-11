#version 450

/* 2D UI layer: solid-color quads/lines (u < 0) or sprites from the per-frame
   streaming RGBA image atlas, tinted by the vertex color (white = untinted,
   dimmed for the grayed used-card state). Alpha comes from the sprite's
   baked mask/colorkey; alpha blending composites over the 3D scene. */

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 0) out vec4 o_color;

layout(set = 2, binding = 0) uniform sampler2D u_image;

layout(set = 3, binding = 0) uniform UiParams {
    vec4 fade; /* x = fade factor 0..1 */
} u_params;

void main()
{
    vec4 c;
    if (v_uv.x < 0.0) {
        c = v_color;
    } else {
        ivec2 size = textureSize(u_image, 0);
        ivec2 texel = clamp(ivec2(v_uv), ivec2(0), size - ivec2(1));
        c = texelFetch(u_image, texel, 0) * v_color;
    }
    if (c.a < 0.004) discard;
    o_color = vec4(c.rgb * u_params.fade.x, c.a);
}
