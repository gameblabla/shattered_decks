#version 450

/* High-resolution text glyphs (PC/SDL3 only). Pairs with ui.vert (same
   game-space vertex transform and attributes as the solid/sprite UI), but
   samples a persistent FreeType glyph atlas with texture() (linear + mipmap)
   so each glyph stays crisp and anti-aliased at any canvas resolution. The
   atlas stores white RGBA with the glyph coverage in the alpha channel; the
   vertex color carries the text colour (fg or drop-shadow). */

layout(location = 0) in vec2 v_uv;      /* normalized atlas coords */
layout(location = 1) in vec4 v_color;   /* text colour (rgb) */
layout(location = 0) out vec4 o_color;

layout(set = 2, binding = 0) uniform sampler2D u_glyph;

layout(set = 3, binding = 0) uniform UiParams {
    vec4 fade; /* x = fade factor 0..1 */
} u_params;

void main()
{
    float cov = texture(u_glyph, v_uv).a;
    float a = v_color.a * cov;
    if (a < 0.004) discard;
    o_color = vec4(v_color.rgb * u_params.fade.x, a);
}
