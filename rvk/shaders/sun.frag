#version 450
// The sun's disc (rvk/sun.cpp DrawSun): drawn into the sky where the game draws its sun's ray sprites, before its cloud
// sheets and the cloud layer - which then cover it as they are thick (premultiplied over: a thin cloud lets a bright
// smear through, a thick one hides it). About twice the real sun's size; darker towards its edge (limb darkening); a
// soft halo; its corona (the game's flare made anew: soft, flowing); a faint starburst. Added to the scene (HDR: the
// bloom its glare).
layout(push_constant) uniform Params {
    mat4 invViewProj;   // clip -> world, the sky's camera (raw D3DMATRIX memory, as cloud_layer.frag)
    vec4 toSun;         // xyz: towards the sun (world), w: the disc's angular radius (radians)
    vec4 colour;        // rgb: the disc's light (HDR), w: the halo's strength
    vec4 view;          // xy: the target's size, z: the starburst's strength, w: the halo's angular width
    vec4 rays;          // the corona (the game's flare made anew): x the clock (s), y strength, z this sun's seed, w reach
} P;

float Hash(float n) { return fract(sin(n * 12.9898 + 4.1414) * 43758.5453); }

// Smooth value noise on a cylinder: x wraps every 'period' cells (round the sun), y runs outward.
float Noise(vec2 p, float period, float seed)
{
    vec2 i = floor(p), f = p - i;
    f = f * f * (3.0 - 2.0 * f);
    float x0 = mod(i.x, period), x1 = mod(i.x + 1.0, period);
    float a = Hash(x0 + i.y * 57.0 + seed), b = Hash(x1 + i.y * 57.0 + seed);
    float c = Hash(x0 + (i.y + 1.0) * 57.0 + seed), d = Hash(x1 + (i.y + 1.0) * 57.0 + seed);
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

// The corona round the sun at angle phi (radians) about it, 'angle' out from its rim: a soft glow whose reach swells
// and shrinks round it in broad lobes - smooth noise flowing slowly outward and round, the whole breathing gently.
// No streaks (user, 2026-10-13: the rays "look strange"). 0..~1.
float Corona(float phi, float angle)
{
    float t = P.rays.x, seed = P.rays.z;
    float a = fract(phi / 6.2831853);
    float out1 = angle / P.rays.w;
    // (two scales: broad lobes (7 round), finer wisps (17 round); each drifting round and flowing outward)
    float n1 = Noise(vec2(a * 7.0 + t * 0.010, out1 * 1.2 - t * 0.05), 7.0, seed);
    float n2 = Noise(vec2(a * 17.0 - t * 0.006, out1 * 2.5 - t * 0.09), 17.0, seed + 31.0);
    float n = n1 * 0.7 + n2 * 0.3;
    float reach = P.rays.w * (0.45 + 0.9 * n);
    float breathe = 0.85 + 0.15 * sin(t * 0.21 + seed) * sin(t * 0.13 + seed * 0.7);
    return exp(-angle / reach) * (0.55 + 0.45 * n) * breathe;
}

layout(location = 0) out vec4 outScene;
layout(location = 1) out vec4 outGlow;
layout(location = 2) out vec4 outLocal;
layout(location = 3) out vec4 outMotion;
layout(location = 4) out vec4 outAlbedo;

void main()
{
    outGlow = outLocal = outMotion = outAlbedo = vec4(0.0);
    vec2 ndc = vec2(gl_FragCoord.x / P.view.x * 2.0 - 1.0, 1.0 - gl_FragCoord.y / P.view.y * 2.0);
    vec4 a = P.invViewProj * vec4(ndc, 0.0, 1.0), b = P.invViewProj * vec4(ndc, 1.0, 1.0);
    vec3 dir = normalize(b.xyz / b.w - a.xyz / a.w);
    float cosA = clamp(dot(dir, P.toSun.xyz), -1.0, 1.0);
    float angle = acos(cosA);
    if (angle > 0.6) {
        outScene = vec4(0.0);
        return;
    }
    float r = angle / P.toSun.w;
    // The disc: edge smoothed over a pixel or so; darker towards the rim (limb darkening, the sun's u ~ 0.6).
    float edge = 1.0 - smoothstep(0.92, 1.0, r);
    float mu = sqrt(max(1.0 - r * r, 0.0));
    vec3 light = P.colour.rgb * (edge * (0.4 + 0.6 * mu));
    // The halo: the light scattered round it, falling off with the angle (a tight core, a wide skirt).
    float halo = exp(-angle / (0.15 * P.view.w)) * 0.6 + exp(-angle / P.view.w) * 0.4;
    light += P.colour.rgb * (P.colour.w * halo * 0.02);
    // The starburst: six thin streaks round it, fading out quickly (glare, faint).
    vec3 up = abs(P.toSun.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 u = normalize(cross(up, P.toSun.xyz)), v = cross(P.toSun.xyz, u);
    float phi = atan(dot(dir, v), dot(dir, u));
    float streak = pow(abs(cos(3.0 * phi)), 60.0) * exp(-angle / (4.0 * P.toSun.w));
    light += P.colour.rgb * (P.view.z * streak * 0.05);
    // The corona (outside the disc, falling off from its rim).
    light += P.colour.rgb * (P.rays.y * Corona(phi, max(angle - P.toSun.w, 0.0)) * 0.03 * smoothstep(0.8, 1.2, r));
    outScene = vec4(light, 0.0);                 // (added: over with alpha 0)
}
