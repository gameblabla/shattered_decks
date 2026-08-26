#version 450

/* Fullscreen triangle for the story-map environment pass (smooth sky gradient +
   infinite ray-cast floor). Passes clip-space NDC (x,y in [-1,1]) to the
   fragment shader, matching the scene projection's convention (y up = +1 = top
   of screen), so a per-pixel view ray can be reconstructed. */

layout(location = 0) out vec2 v_ndc;

void main()
{
    vec2 pos = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2)) * 2.0 - 1.0;
    v_ndc = pos;
    gl_Position = vec4(pos, 0.0, 1.0);
}
