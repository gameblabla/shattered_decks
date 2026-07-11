#version 450

/* 2D UI layer: vertices in game screen space (WAIFU_FM_WIDTH x
   WAIFU_FM_HEIGHT units), mapped to the viewport by an orthographic
   transform. u < 0 marks solid-color geometry (no texture). */

layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_color;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;

layout(set = 1, binding = 0) uniform UiTransform {
    vec4 screen; /* x = screen width, y = screen height */
} u_ui;

void main()
{
    float ndc_x = a_pos.x / (u_ui.screen.x * 0.5) - 1.0;
    float ndc_y = 1.0 - a_pos.y / (u_ui.screen.y * 0.5);
    v_uv = a_uv;
    v_color = a_color;
    gl_Position = vec4(ndc_x, ndc_y, 0.0, 1.0);
}
