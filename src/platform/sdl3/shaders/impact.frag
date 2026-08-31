#version 450

/* Direct-attack impact burst (PC only).
 *
 * The console builds draw this hit as palette-index discs, rings and Bresenham
 * spokes, which is all an 8bpp framebuffer can express -- hard-edged blocks of
 * one of 256 colours. Here the whole beat is one procedural pass instead:
 * every layer is a smooth analytic falloff evaluated per fragment at the
 * canvas resolution and composited with premultiplied alpha, so the effect is
 * soft, blown-out and resolution-independent rather than pixelated.
 *
 * Composited with src = ONE, dst = ONE_MINUS_SRC_ALPHA, so one pass does both
 * jobs: rgb is additive light on top of the arena, alpha darkens what is
 * behind it (result = light + arena * (1 - dim)).
 *
 * Layers, in order of arrival:
 *   - blade      a swept oriented capsule with a white core and a warm bloom
 *   - flash      a brief full-frame whiteout at the moment of contact
 *   - core       a blooming hot centre that swells and dies
 *   - ring       an expanding shockwave annulus, narrowing as it grows
 *   - rays       two hashed angular ray fans with per-ray lengths
 *   - embers     a handful of sparks thrown outward
 *   - dim        a vignetted darkening that sinks the arena behind the burst
 *
 * All radii/distances are in game-space pixels. u_fx.p0 carries the viewport's
 * game-space extent so NDC can be mapped back into that space. */

layout(location = 0) in vec2 v_ndc;
layout(location = 0) out vec4 o_color;

layout(set = 3, binding = 0) uniform ImpactParams {
    vec4 p0;    /* xy = viewport extent in game units, zw = burst centre */
    vec4 p1;    /* x = beat progress 0..1(+), y = slash direction, zw = unused */
} u_fx;

const float TAU = 6.28318530718;

/* Slash occupies the first fifth of the beat; the burst owns the rest. */
const float SLASH_END  = 0.19;
const float FLASH_AT   = 0.115;
const float BURST_FROM = 0.11;

float hash1(float n) { return fract(sin(n * 127.1) * 43758.5453); }

/* White -> pale yellow -> yellow -> orange -> red -> ember, as a continuous
   ramp instead of the console build's eight discrete palette entries. */
vec3 heat(float h)
{
    h = clamp(h, 0.0, 1.0);
    vec3 c = mix(vec3(1.00, 0.99, 0.96), vec3(1.00, 0.94, 0.70), smoothstep(0.00, 0.20, h));
    c = mix(c, vec3(1.00, 0.84, 0.28), smoothstep(0.17, 0.40, h));
    c = mix(c, vec3(1.00, 0.45, 0.07), smoothstep(0.38, 0.63, h));
    c = mix(c, vec3(0.92, 0.09, 0.06), smoothstep(0.60, 0.85, h));
    c = mix(c, vec3(0.34, 0.03, 0.03), smoothstep(0.85, 1.00, h));
    return c;
}

float gauss(float x, float w) { float t = x / max(w, 0.0001); return exp(-t * t); }

/* One ray fan: `n` spokes around the centre, each with its own hashed length,
   windowed so they start outside the core and taper off past their tip. */
float rayfan(float ang, float r, float R, float n, float phase, float sharp)
{
    float ai = (ang / TAU + 1.5 + phase) * n;
    float cell = floor(ai);
    float f = fract(ai) * 2.0 - 1.0;
    float spoke = pow(max(0.0, 1.0 - abs(f)), sharp);
    float len = R * (0.55 + 0.80 * hash1(cell + n));
    float inner = R * 0.10;
    float body = smoothstep(inner, inner + R * 0.16, r) *
                 (1.0 - smoothstep(len * 0.60, len, r));
    return spoke * body;
}

void main()
{
    vec2 extent = u_fx.p0.xy;
    vec2 centre = u_fx.p0.zw;
    float t = max(u_fx.p1.x, 0.0);
    float dir = u_fx.p1.y;

    /* NDC -> game screen space (y down, matching the 2D UI transform). */
    vec2 g = vec2((v_ndc.x * 0.5 + 0.5) * extent.x, (0.5 - v_ndc.y * 0.5) * extent.y);
    vec2 d = g - centre;
    float r = length(d);
    float ang = atan(d.y, d.x);

    /* Burst sub-progress, and its late pull-back so the beat resolves. */
    float b = clamp((t - BURST_FROM) / (1.0 - BURST_FROM), 0.0, 1.0);
    float settle = smoothstep(0.72, 1.0, b);
    float R = (12.0 + 118.0 * (1.0 - pow(1.0 - b, 3.0))) * (1.0 - 0.26 * settle);
    float ringw = mix(9.0, 2.4, b);
    float core = mix(9.0, 46.0, smoothstep(0.0, 0.22, b)) * (1.0 - smoothstep(0.22, 0.74, b));

    vec3 light = vec3(0.0);

    /* --- core ---------------------------------------------------------- */
    float hot = 1.0 - smoothstep(0.30, 0.70, b);
    light += heat(b * 0.55) * gauss(r, max(core, 1.0)) * mix(0.9, 2.6, hot);
    light += vec3(1.0, 0.99, 0.97) * gauss(r, max(core * 0.42, 1.0)) * 1.9 * hot;

    /* --- shockwave ring ------------------------------------------------
       Two falloffs (a tight crest over a wide skirt) and a little angular
       wobble in the radius, so the wave reads as a blown-out shock front
       rather than a drawn circle. The crest is sampled a step cooler on its
       outside than its inside, which gives the edge a subtle warm-to-cool
       separation instead of one flat hue. */
    float wob = 1.0 + 0.012 * sin(ang * 3.0 + 1.1) + 0.007 * sin(ang * 7.0 - 2.3);
    float dr = r - R * wob;
    float edge = clamp(0.5 + dr / (ringw * 3.0), 0.0, 1.0);
    light += heat(clamp(b * 0.95 + edge * 0.10, 0.0, 1.0)) * gauss(dr, ringw) * mix(2.3, 0.8, b);
    light += heat(clamp(b * 0.95 + 0.14, 0.0, 1.0)) * gauss(dr, ringw * 3.2) * mix(0.7, 0.30, b);

    /* --- ray fans ------------------------------------------------------ */
    float rays = rayfan(ang, r, R, 30.0, 0.00, 2.2) * 0.85 +
                 rayfan(ang, r, R, 17.0, 0.31, 3.2) * 0.55;
    light += heat(clamp(b * 1.12, 0.0, 1.0)) * rays * mix(1.7, 0.45, b);

    /* --- broad halo ---------------------------------------------------- */
    light += heat(min(1.0, 0.55 + b * 0.45)) * gauss(r, R * 0.85 + 1.0) * 0.38;

    /* --- embers -------------------------------------------------------- */
    float ember_life = smoothstep(0.02, 0.18, b) * (1.0 - smoothstep(0.55, 0.95, b));
    if (ember_life > 0.001) {
        for (int i = 0; i < 14; ++i) {
            float fi = float(i);
            float a = hash1(fi * 1.7) * TAU;
            float sp = R * (0.75 + 0.55 * hash1(fi * 3.1)) * smoothstep(0.0, 0.75, b);
            vec2 pp = vec2(cos(a), sin(a)) * sp;
            light += heat(clamp(b * 1.2, 0.0, 1.0)) *
                     gauss(length(d - pp), 2.4) * 1.6 * ember_life;
        }
    }

    /* --- blade sweep --------------------------------------------------- */
    if (t < SLASH_END) {
        float st = clamp(t / SLASH_END, 0.0, 1.0);
        vec2 axis = normalize(vec2(dir, 0.86));
        vec2 nrm = vec2(-axis.y, axis.x);
        float along = dot(d, axis);
        float across = dot(d, nrm);
        float reach = 210.0;
        float head = mix(-reach, reach, smoothstep(0.00, 0.45, st));
        float tail = mix(-reach, reach, smoothstep(0.28, 1.00, st));
        float seg = smoothstep(tail - 6.0, tail + 6.0, along) *
                    (1.0 - smoothstep(head - 6.0, head + 6.0, along));
        float w = mix(4.5, 0.9, st);
        light += vec3(1.00, 0.98, 0.93) * seg * gauss(across, w) * 3.2;
        light += heat(0.30) * seg * gauss(across, w * 3.0) * 0.75;
    }

    /* --- contact flash -------------------------------------------------
       Kept deliberately short: an amplitude/width that saturates for more
       than two or three frames stops reading as a hit and starts reading as
       a white screen. */
    light += vec3(1.00, 0.98, 0.95) * gauss(t - FLASH_AT, 0.026) * 1.6;

    /* --- arena dim -----------------------------------------------------
       The relief around the centre only exists so the hottest part of the
       burst is not sitting on pure black; it has to stay tight, because any
       wider and it is the attacker's card that shows through it. */
    float dim = 0.97 * smoothstep(0.09, 0.24, t);
    dim *= 1.0 - 0.35 * gauss(r, 40.0);

    /* Everything releases over the last tenth of the beat; past t = 1 the
       caller is holding the settle frames, so only the dim remains. */
    light *= 1.0 - smoothstep(0.90, 1.02, t);

    o_color = vec4(light, clamp(dim, 0.0, 1.0));
}
