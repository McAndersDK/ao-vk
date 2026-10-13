#version 450
// The sun's disc (rvk/sun.cpp DrawSun): drawn into the sky where the game draws its sun's ray sprites, before its cloud
// sheets and the cloud layer - which then cover it as they are thick (premultiplied over: a thin cloud lets a bright
// smear through, a thick one hides it). About twice the real sun's size; darker towards its edge (limb darkening); a
// soft halo; its rays (the game's flare made anew); a faint starburst. Added to the scene (HDR: the bloom its glare).
layout(push_constant) uniform Params {
    mat4 invViewProj;   // clip -> world, the sky's camera (raw D3DMATRIX memory, as cloud_layer.frag)
    vec4 toSun;         // xyz: towards the sun (world), w: the disc's angular radius (radians)
    vec4 colour;        // rgb: the disc's light (HDR), w: the halo's strength
    vec4 view;          // xy: the target's size, z: the starburst's strength, w: the halo's angular width
    vec4 rays;          // the rays (the game's flare made anew): x the clock (s), y strength, z this sun's seed, w reach
} P;

float Hash(float n) { return fract(sin(n * 12.9898 + 4.1414) * 43758.5453); }

// The rays round the sun at angle phi (radians) about it: streaks in many directions, each its own brightness and
// length, two layers turning slowly opposite ways, each streak's brightness drifting (shimmer). 0..~1.
float Rays(float phi, float angle)
{
    float t = P.rays.x, sum = 0.0;
    for (int layer = 0; layer < 2; ++layer) {
        float bins = layer == 0 ? 28.0 : 61.0;
        float turn = (layer == 0 ? 0.012 : -0.007) * t;
        float a = fract((phi + turn) / 6.2831853) * bins;
        float i = floor(a), f = fract(a);
        float seed = P.rays.z + float(layer) * 101.0;
        float s0 = Hash(i + seed), s1 = Hash(mod(i + 1.0, bins) + seed);
        // (each streak: thin - a peak in its bin - its brightness shimmering with time, its length its own)
        float width = layer == 0 ? 0.18 : 0.12;
        float peak = exp(-pow((f - 0.5) / width, 2.0) * 2.0);
        float shimmer = 0.6 + 0.4 * sin(t * (0.7 + s1 * 1.3) + s0 * 6.2831853);
        float len = P.rays.w * (0.35 + 0.9 * s1) * (layer == 0 ? 1.0 : 0.6);
        sum += peak * pow(s0, 2.0) * shimmer * exp(-angle / len) * (layer == 0 ? 1.0 : 0.6);
    }
    return sum;
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
    // The rays (outside the disc, falling off from its rim).
    light += P.colour.rgb * (P.rays.y * Rays(phi, max(angle - P.toSun.w, 0.0)) * 0.035 * smoothstep(0.8, 1.2, r));
    outScene = vec4(light, 0.0);                 // (added: over with alpha 0)
}
