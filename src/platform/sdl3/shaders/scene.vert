#version 450

/* 3D scene vertices arrive as premultiplied clip coordinates
   (ndc_x*w, ndc_y*w, w) — the game's pinhole projection folded in at capture
   time — so no uniforms are needed: the hardware perspective divide by w
   reproduces the software projection exactly, with real clipping and
   perspective-correct UV interpolation. */

layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in float a_tile;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out float v_tile;

const float ZN = 0.05;
const float ZF = 200.0;

void main()
{
    float w = a_pos.z;
    float zclip = ZF * (w - ZN) / (ZF - ZN);
    v_uv = a_uv;
    v_tile = a_tile;
    gl_Position = vec4(a_pos.x, a_pos.y, zclip, w);
}
