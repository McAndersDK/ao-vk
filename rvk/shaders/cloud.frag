#version 450
// Clouds (rvk/clouds.cpp): a puff of the game's as a volume. The view ray through its box (mesh space), marched: the
// density is the puff's texture silhouette given a body across its thin axis - thickest where the texture is most
// opaque - frayed by drifting noise; each step lit by the sky (more towards the puff's top) and by the sun through the
// puff (a few steps towards it: dark bellies, bright sunlit sides; brighter still looking towards the sun). The colour
// is the game's for the puff (its texture times its lit colour): the clouds keep their look and gain depth.
#include "clouds_common.glsl"

layout(set = 0, binding = 1) uniform sampler2D puffTexture;
layout(set = 0, binding = 2) uniform sampler3D noiseTexture; // r: billowy (value x worley), g, b: worley, a: value
#ifdef RVK_CLOUD_HALF
layout(set = 0, binding = 3) uniform sampler2D sceneDepth;  // the pre-pass's (C.distance.z): only where it is clear
#endif

layout(location = 0) in vec3 vLocal;

layout(location = 0) out vec4 outScene;
#ifndef RVK_CLOUD_HALF                          // (half size, RVK_CloudHalf: the smoke only; cloud_up.frag the rest)
layout(location = 1) out vec4 outGlow;
layout(location = 2) out vec4 outLocal;
layout(location = 3) out vec4 outMotion;
layout(location = 4) out vec4 outAlbedo;
#endif

int gThin;
float gMid, gHalf, gNoiseScale;

vec2 Uv(vec3 p) { return vec2(dot(C.uvU.xyz, p) + C.uvU.w, dot(C.uvV.xyz, p) + C.uvV.w); }

float Density(vec3 p)
{
    float a = textureLod(puffTexture, Uv(p), 1.0).a;
    if (a < 0.03)
        return 0.0;
    float across = (p[gThin] - gMid) / gHalf;                      // -1..1 through the puff
    float body = sqrt(a);
    float shape = clamp(1.0 - across * across / (body * body + 1e-3), 0.0, 1.0);
    float d = a * sqrt(shape);
    vec3 q = p * gNoiseScale + C.noise.xyz * 0.01;
    vec4 n = texture(noiseTexture, q);
    float fray = C.noise.w;
    d = d * (1.0 + 0.4 * fray) - (1.0 - n.r) * 0.55 * fray;          // billows: the edges eaten into
    d -= (1.0 - texture(noiseTexture, q * 3.1 + 0.37).g) * 0.12 * fray * (1.0 - d);   // finer wisps at the edges
    return clamp(d, 0.0, 1.0);
}

void main()
{
#ifndef RVK_CLOUD_HALF
    outGlow = outLocal = outMotion = outAlbedo = vec4(0.0);
#else
    // The four scene pixels under this one: none of them sky (the pre-pass's depth isn't its clear value) - an opaque
    // surface is drawn over all of them later; nothing to march.
    if (C.distance.z > 0.5) {
        ivec2 at = ivec2(gl_FragCoord.xy) * 2, last = textureSize(sceneDepth, 0) - 1;
        bool open = false;
        for (int k = 0; k < 4; ++k)
            open = open || texelFetch(sceneDepth, min(at + ivec2(k & 1, k >> 1), last), 0).r == C.distance.y;
        if (!open)
            discard;
    }
#endif
    vec3 lo = C.low.xyz, hi = C.high.xyz;
    vec3 eye = C.eye.xyz;
    vec3 dir = vLocal - eye;                                         // t = 1 at this face
    // The far faces only: the face this point is on, its outward normal away from the eye.
    vec3 f = (vLocal - lo) / max(hi - lo, vec3(1e-6));
    vec3 n = vec3(0.0);
    float best = 1e9;
    for (int k = 0; k < 3; ++k) {
        if (f[k] < best) { best = f[k]; n = vec3(0.0); n[k] = -1.0; }
        if (1.0 - f[k] < best) { best = 1.0 - f[k]; n = vec3(0.0); n[k] = 1.0; }
    }
    if (dot(n, dir) <= 0.0)
        discard;
    vec3 inv = 1.0 / mix(dir, vec3(1e-6), lessThan(abs(dir), vec3(1e-6)));
    vec3 t0 = (lo - eye) * inv, t1 = (hi - eye) * inv;
    vec3 tmin = min(t0, t1), tmax = max(t0, t1);
    float enter = max(max(max(tmin.x, tmin.y), tmin.z), 0.0);
    float leave = min(min(tmax.x, tmax.y), tmax.z);
    if (leave <= enter)
        discard;
    gThin = int(C.low.w);
    gMid = 0.5 * (lo[gThin] + hi[gThin]);
    gHalf = max(0.5 * (hi[gThin] - lo[gThin]), 1e-4);
    vec3 size = hi - lo;
    gNoiseScale = 2.2 / max(max(size.x, size.y), size.z);           // a few billows across the puff
    // Optical depth: per unit of the puff's thickness (mesh space), so a puff is as dense however big it is drawn.
    float sigma = 1.6 * C.sunColour.w / (2.0 * gHalf);              // (its core about as opaque as the game drew it)
    int steps = int(C.high.w);
    float dt = (leave - enter) / float(steps);
    float ds = dt * length(dir);                                     // mesh units a step
    float jitter = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))) + FL.taa.z);
    vec3 towardSun = normalize(C.sun.xyz);
    float sunStep = 0.5 * gHalf;
    vec3 viewW = normalize(mat3(C.world) * dir);
    vec3 sunW = normalize(FL.sunTrue.xyz + vec3(0.0, 1e-6, 0.0));   // (the sun's: below the horizon too)
    float cosT = dot(viewW, sunW);
    float phase = 0.85 + 0.9 * pow(max(cosT, 0.0), 8.0);           // the lit side; a silver rim looking at the sun
    vec3 sunColour = C.sunColour.rgb;
    float sunUp = smoothstep(-0.1, 0.25, sunW.y);                    // (by night, no sun through them)
    float transmit = 1.0;
    vec3 light = vec3(0.0);
    for (int i = 0; i < steps && transmit > 0.02; ++i) {
        vec3 p = eye + dir * (enter + (float(i) + jitter) * dt);
        float d = Density(p);
        if (d < 0.01)
            continue;
        // The sun through the puff.
        // (Two samples, the second further: what lies towards the sun, near and deeper.)
        float toSun = Density(p + towardSun * (sunStep * 1.2)) * 1.6 + Density(p + towardSun * (sunStep * 3.2)) * 2.4;
        float sunT = exp(-toSun * sigma * sunStep * 2.5);              // (the sun's way through it: shadowed inside)
        // The sky's light, more towards the puff's top (its belly darker), and the sun's.
        vec2 uv = Uv(p);
        vec3 albedo = C.colour.rgb * textureLod(puffTexture, uv, 0.0).rgb;
        float up = clamp(0.5 + 0.5 * (p[gThin] - gMid) / gHalf, 0.0, 1.0);
        // (Around the game's colour - lit as the game lit it, sun included: its middle about as the game drew it, its
        // top and sunlit parts brighter, its belly and shadowed parts darker.)
        vec3 lit = albedo * (mix(0.75, 1.0, up) + (sunT - 0.5) * phase * 0.3 * sunUp * (0.5 + 0.5 * dot(sunColour, vec3(0.33))));
        float a = 1.0 - exp(-d * sigma * ds);
        light += transmit * a * lit;
        transmit *= 1.0 - a;
    }
    float alpha = (1.0 - transmit) * clamp(C.colour.a, 0.0, 1.0);
    light *= clamp(C.colour.a, 0.0, 1.0);
    // The fog the game drew the puff with, by its distance (as the game's per-vertex fog, about: the whole puff alike).
    if (C.fogColour.w > 0.5) {
        float d = C.distance.x, fog = 1.0;
        uint mode = uint(C.fogParams.w);
        if (mode == 3u) fog = clamp((C.fogParams.y - d) / max(C.fogParams.y - C.fogParams.x, 1e-3), 0.0, 1.0);
        else if (mode == 1u) fog = clamp(exp(-C.fogParams.z * d), 0.0, 1.0);
        else if (mode == 2u) { float x = C.fogParams.z * d; fog = clamp(exp(-x * x), 0.0, 1.0); }
        light = mix(C.fogColour.rgb * alpha, light, fog);
    }
    outScene = vec4(light, alpha);
#ifndef RVK_CLOUD_HALF
    outLocal = vec4(alpha, 0.0, 0.0, alpha);                         // (the ambient occlusion spares it, as smoke)
#endif
}
