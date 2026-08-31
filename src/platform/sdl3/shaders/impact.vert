#version 450

/* Fullscreen triangle for the direct-attack impact pass. Passes clip-space NDC
   (x,y in [-1,1], y up) to the fragment shader, which converts it back to the
   game's screen-space units so the burst can be authored in game pixels and
   stay identical at every canvas resolution. */

layout(location = 0) out vec2 v_ndc;

void main()
{
    vec2 pos = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2)) * 2.0 - 1.0;
    v_ndc = pos;
    gl_Position = vec4(pos, 0.0, 1.0);
}
