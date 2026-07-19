#version 450

/* Card face/back/support quads: UVs are texel coordinates into the per-frame
   streaming image atlas; gray = 1 renders the dimmed used-card state. */

layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in float a_gray;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out float v_gray;

layout(set = 1, binding = 0) uniform Xform { vec4 p; } u_xf; /* x = horizontal scale */

const float ZN = 0.05;
const float ZF = 200.0;

void main()
{
    float w = a_pos.z;
    float zclip = ZF * (w - ZN) / (ZF - ZN);
    v_uv = a_uv;
    v_gray = a_gray;
    gl_Position = vec4(a_pos.x * u_xf.p.x, a_pos.y, zclip, w);
}
