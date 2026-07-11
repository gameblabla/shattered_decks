#version 450

/* Checkered story floor plane: position premultiplied like scene.vert, plus
   the world-space XZ carried through for the per-fragment tile pattern. */

layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec2 a_world;

layout(location = 0) out vec2 v_world;

const float ZN = 0.05;
const float ZF = 200.0;

void main()
{
    float w = a_pos.z;
    float zclip = ZF * (w - ZN) / (ZF - ZN);
    v_world = a_world;
    gl_Position = vec4(a_pos.x, a_pos.y, zclip, w);
}
