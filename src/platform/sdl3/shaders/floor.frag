#version 450

/* Infinite checkered floor: world XZ -> texel coordinates (32 texels per
   tile_size world units), alternating tile_a / tile_b per tile cell — the
   same pattern the software raycast floor samples. */

layout(location = 0) in vec2 v_world;
layout(location = 0) out vec4 o_color;

layout(set = 2, binding = 0) uniform sampler2DArray u_atlas;

layout(set = 3, binding = 0) uniform FloorParams {
    vec4 params; /* x = texels_per_unit, y = tile_a, z = tile_b, w = fade */
} u_floor;

void main()
{
    float tpu = u_floor.params.x;
    int itx = int(floor(v_world.x * tpu));
    int itz = int(floor(v_world.y * tpu));
    int checker = ((itx ^ itz) >> 5) & 1;
    float tile = (checker == 1) ? u_floor.params.z : u_floor.params.y;
    ivec2 texel = ivec2(itx & 31, itz & 31);
    vec4 c = texelFetch(u_atlas, ivec3(texel, int(tile + 0.5)), 0);
    o_color = vec4(c.rgb * u_floor.params.w, 1.0);
}
