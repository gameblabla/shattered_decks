#version 450

/* 3D lines (board grid, card rims, pyramid edges, void crystals): RGBA color
   resolved at capture time. */

layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec4 a_color;

layout(location = 0) out vec4 v_color;

layout(set = 1, binding = 0) uniform Xform { vec4 p; } u_xf; /* x = horizontal scale */

const float ZN = 0.05;
const float ZF = 200.0;

void main()
{
    float w = a_pos.z;
    float zclip = ZF * (w - ZN) / (ZF - ZN);
    v_color = a_color;
    gl_Position = vec4(a_pos.x * u_xf.p.x, a_pos.y, zclip, w);
}
