#version 450

layout(location = 0) in vec4 v_color;
layout(location = 0) out vec4 o_color;

layout(set = 3, binding = 0) uniform LineParams {
    vec4 fade; /* x = fade factor 0..1 */
} u_params;

void main()
{
    o_color = vec4(v_color.rgb * u_params.fade.x, v_color.a);
}
