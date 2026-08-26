#version 450

/* Story-map environment: a smooth vertical sky gradient with an infinite
   ray-cast checkerboard floor, drawn as one fullscreen pass beneath the 3D
   solids. Replaces the software's hard fill_rows sky bands and the fragile
   clipped-quad floor (which produced grazing near-plane slivers that never
   rasterized). For every pixel we reconstruct the world-space view ray from the
   camera basis, intersect it with the plane y = floor_y, and either sample the
   checker at the hit point (below the horizon) or shade the sky gradient
   (above). */

layout(location = 0) in vec2 v_ndc;
layout(location = 0) out vec4 o_color;

layout(set = 2, binding = 0) uniform sampler2DArray u_atlas;

layout(set = 3, binding = 0) uniform EnvParams {
    vec4 eye;      /* xyz eye, w = has_floor */
    vec4 right;    /* xyz right,   w = sx (focal / (W/2)) */
    vec4 up;       /* xyz up,      w = sy (focal / (H/2)) */
    vec4 fwd;      /* xyz forward, w = floor_y */
    vec4 floorp;   /* x = texels/unit, y = tile_a, z = tile_b, w = has_sky */
    vec4 stop0;    /* sky stop: rgb + normalized screen-y (top -> horizon) */
    vec4 stop1;
    vec4 stop2;
    vec4 stop3;
    vec4 horizon;  /* rgb horizon/fog colour, w = far fade distance */
    vec4 misc;     /* x = sky_kind, y = hscroll phase */
} u;

float hash21(vec2 p)
{
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

vec3 sky_gradient(float t)
{
    vec3 c = u.stop0.rgb;
    c = mix(c, u.stop1.rgb, clamp((t - u.stop0.a) / max(u.stop1.a - u.stop0.a, 1e-4), 0.0, 1.0));
    c = mix(c, u.stop2.rgb, clamp((t - u.stop1.a) / max(u.stop2.a - u.stop1.a, 1e-4), 0.0, 1.0));
    c = mix(c, u.stop3.rgb, clamp((t - u.stop2.a) / max(u.stop3.a - u.stop2.a, 1e-4), 0.0, 1.0));
    return c;
}

void main()
{
    vec2 uv = v_ndc * 0.5 + 0.5;          /* 0..1, y up */
    float t_sky = clamp(1.0 - uv.y, 0.0, 1.0);
    vec3 col = sky_gradient(t_sky);

    int kind = int(u.misc.x + 0.5);
    float phase = u.misc.y;

    /* Drifting stars (void) / rising embers (volcano) in the sky band. */
    if (kind == 4) {                       /* WAIFU_BACKGROUND_SKY (void) */
        vec2 g = uv * vec2(64.0, 42.0) + vec2(phase * 0.03, 0.0);
        vec2 cell = floor(g);
        float h = hash21(cell);
        if (h > 0.90) {
            float d = length(fract(g) - 0.5);
            float b = smoothstep(0.16, 0.0, d) * (0.35 + 0.65 * hash21(cell + 7.3));
            col += vec3(b);
        }
    } else if (kind == 3) {                /* WAIFU_BACKGROUND_EMBER (volcano) */
        vec2 g = uv * vec2(48.0, 60.0);
        g.y += phase * 0.01 + hash21(floor(g * 0.3)) * 3.0;  /* rise */
        vec2 cell = floor(g);
        float h = hash21(cell);
        if (h > 0.94 && uv.y > 0.45) {
            float d = length(fract(g) - 0.5);
            float b = smoothstep(0.18, 0.0, d);
            col = mix(col, vec3(1.0, 0.55, 0.15), b);
        }
    }

    if (u.eye.w > 0.5) {
        /* World-space view ray: camera-space (ndc.x/sx, ndc.y/sy, 1) rotated by
           the basis. The forward coefficient is 1, so the intersection parameter
           t equals the world forward distance (basis is orthonormal). */
        vec3 dir = (v_ndc.x / u.right.w) * u.right.xyz
                 + (v_ndc.y / u.up.w)    * u.up.xyz
                 +                          u.fwd.xyz;
        float floor_y = u.fwd.w;
        if (dir.y < -1e-4) {
            float t = (floor_y - u.eye.y) / dir.y;
            if (t > 0.0) {
                vec3 hit = u.eye.xyz + t * dir;
                float tpu = u.floorp.x;
                int itx = int(floor(hit.x * tpu));
                int itz = int(floor(hit.z * tpu));
                int checker = ((itx ^ itz) >> 5) & 1;
                float tile = (checker == 1) ? u.floorp.z : u.floorp.y;
                ivec2 texel = ivec2(itx & 31, itz & 31);
                vec3 fc = texelFetch(u_atlas, ivec3(texel, int(tile + 0.5)), 0).rgb;
                /* Fade to the horizon colour with distance so the checker does
                   not alias into noise as it recedes to the horizon line. */
                float fog = clamp(t / max(u.horizon.a, 1e-3), 0.0, 1.0);
                col = mix(fc, u.horizon.rgb, fog * fog);
            }
        }
    }

    o_color = vec4(col, 1.0);
}
