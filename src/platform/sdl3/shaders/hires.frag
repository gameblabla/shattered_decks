#version 450

/* Full-resolution card art (PC): unlike image.frag's texelFetch, this samples
   with texture() so the per-card mipmapped texture is filtered (linear + mip)
   when the card is drawn much smaller than the source — crisp thumbnails and
   big art with no aliasing. Pairs with image.vert (clip-space + horizontal
   widescreen scale) and a per-card 2D texture bound at draw time. */

layout(location = 0) in vec2 v_uv;
layout(location = 1) in float v_gray;
layout(location = 0) out vec4 o_color;

layout(set = 2, binding = 0) uniform sampler2D u_image;

layout(set = 3, binding = 0) uniform ImageParams {
    vec4 fade; /* x = fade factor 0..1 */
} u_params;

void main()
{
    vec4 c = texture(u_image, v_uv);
    if (v_gray > 0.5) c.rgb *= 0.55;    /* dimmed used-card state */
    if (c.a < 0.004) discard;
    o_color = vec4(c.rgb * u_params.fade.x, c.a);
}
