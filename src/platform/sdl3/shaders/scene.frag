#version 450

/* Atlas-tile surfaces: sample the RGBA-converted 32x32 tile (layer = tile
   index in the texture array). UV integer part = repeats across the face
   (pyramid brick courses). fade scales all layers uniformly (palette-
   intensity fade, like the console targets). */

layout(location = 0) in vec2 v_uv;
layout(location = 1) in float v_tile;
layout(location = 0) out vec4 o_color;

layout(set = 2, binding = 0) uniform sampler2DArray u_atlas;

layout(set = 3, binding = 0) uniform SceneParams {
    vec4 fade; /* x = fade factor 0..1 */
} u_params;

void main()
{
    ivec2 texel = ivec2(clamp(fract(v_uv) * 32.0, 0.0, 31.0));
    vec4 c = texelFetch(u_atlas, ivec3(texel, int(v_tile + 0.5)), 0);
    o_color = vec4(c.rgb * u_params.fade.x, 1.0);
}
