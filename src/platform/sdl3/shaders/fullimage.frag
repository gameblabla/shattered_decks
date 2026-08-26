#version 450

/* 16:9 title / ending image (PC): sample the source across the full canvas,
   linear-filtered. Paired with blit.vert's fullscreen triangle. Drawn into the
   persistent canvas (the global fade is applied later at the present blit). */

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

layout(set = 2, binding = 0) uniform sampler2D u_image;

void main()
{
    o_color = vec4(texture(u_image, v_uv).rgb, 1.0);
}
