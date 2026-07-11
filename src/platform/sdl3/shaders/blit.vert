#version 450

/* Fullscreen triangle for the canvas -> swapchain present blit. */

layout(location = 0) out vec2 v_uv;

void main()
{
    vec2 pos = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    v_uv = vec2(pos.x, 1.0 - pos.y);
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
