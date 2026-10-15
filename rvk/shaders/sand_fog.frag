#version 450
// A sand or ash storm's air as a lit volume (hdr.cpp RenderSandFog, RVK_SandFog; docs/sand-fog-plan.md), at half
// resolution: marched from the eye to the surface (or out of the dust, for the sky) - at each step the dust's density
// (thick near the ground, thinning upwards; the gusts' billows drifting with the wind) and the light reaching it: the
// sun through the dust above, the shadow cascades near, the terrain's shade from a low sun, the clouds' shadows; the
// sky's light and the moon's evenly. Out: the light the dust scatters towards the eye (rgb) and how much of what is
// behind still shows (a). The tone mapping lays it over the picture: c x a + rgb.
#include "frame_lights.glsl"
layout(set = 0, binding = 0) uniform sampler2D depthTex;
layout(set = 0, binding = 1) uniform sampler2DArrayShadow shadowMap;
layout(set = 0, binding = 2) uniform sampler2D cloudShadow;   // the clouds' (rvk clouds.cpp; FL.clouds: where, if any)
layout(set = 0, binding = 3) uniform sampler2D terrainMap;    // terrain_sun.cpp: R the height to see a low sun, G the ground
layout(push_constant) uniform Push {
    mat4 invViewProj;   // the world camera's clip -> world (raw D3DMATRIX memory)
    vec4 eye;           // camera position; w: the march's reach (world units)
    vec4 proj;          // D3D projection m[2][2], m[3][2]; the full target's width, height
    vec4 dens;          // x: extinction per unit at the ground, y: scale height, z: the ground under the eye, w: clear
                        // round the eye (RVK_SandClear)
    vec4 albedo;        // the dust's colour (rgb); w: this frame's noise offset
    vec4 sky;           // the sky's light on the dust (rgb); w: steps
    vec4 terrain;       // the terrain map: its corner x, z, 1 / its size; w: 1 = there, 2 = and its sun shade usable
    vec4 wind;          // the wind's direction x, z; its frame's offsets along, across (the gusts carried with it)
    vec4 gusts;         // x: RVK_SandGusts, y: the wind's time (s), z: the eye's shelter (0 .. 1), w: ash (1: no glow)
} P;
layout(location = 0) out vec4 outFog;

#include "cloud_shadow_common.glsl"

// The ray in each cascade's coordinates (as volume.frag): a point t along it at gRayStart[c] + gRayDir[c] t.
vec3 gRayStart[4], gRayDir[4];
float SunVisible(float t)
{
    int count = int(FL.shadowParams.z);
    for (int c = 0; c < count; ++c) {
        vec3 ndc = gRayStart[c] + gRayDir[c] * t;
        if (max(abs(ndc.x), abs(ndc.y)) >= 0.98 || ndc.z <= 0.0 || ndc.z >= 1.0) continue;
        return texture(shadowMap, vec4(ndc.xy * 0.5 + 0.5, float(c), ndc.z));
    }
    return 1.0;
}

// The terrain map at a point: (height to see a low sun, the ground); the ground under the eye where it isn't known.
vec2 TerrainAt(vec2 xz)
{
    if (P.terrain.w < 0.5)
        return vec2(-60000.0, P.dens.z);
    vec2 uv = (xz - P.terrain.xy) * P.terrain.z;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))))
        return vec2(-60000.0, P.dens.z);
    vec2 m = textureLod(terrainMap, uv, 0.0).rg;
    return vec2(m.r, m.g < -10000.0 ? P.dens.z : m.g);
}

float GustHash(vec3 p)
{
    p = fract(p * 0.3183099 + 0.1);
    p *= 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}
float GustNoise(vec3 x)
{
    vec3 i = floor(x), f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(GustHash(i), GustHash(i + vec3(1, 0, 0)), f.x), mix(GustHash(i + vec3(0, 1, 0)), GustHash(i + vec3(1, 1, 0)), f.x), f.y),
               mix(mix(GustHash(i + vec3(0, 0, 1)), GustHash(i + vec3(1, 0, 1)), f.x), mix(GustHash(i + vec3(0, 1, 1)), GustHash(i + 1.0), f.x), f.y),
               f.z);
}
// The gusts' billows at a point (h over the ground): about 1 on average, 0.45 .. 2.45 / 1.45 (as the haze's: never
// thin enough to show the far land through).
float Billow(vec3 q, float h)
{
    if (P.gusts.x <= 0.0)
        return 1.0;
    vec2 side = vec2(-P.wind.y, P.wind.x);
    float along = dot(q.xz, P.wind.xy) + P.wind.z, across = dot(q.xz, side) + P.wind.w;
    // (One scale - the haze's larger: the finer one cost a quarter of the fog and is lost in its blur.)
    float n = GustNoise(vec3(along / 70.0, h / 22.0 + P.gusts.y * 0.015, across / 40.0));
    float b = (0.45 + 2.0 * smoothstep(0.34, 0.68, n)) / 1.45;
    return mix(1.0, b, clamp(P.gusts.x, 0.0, 1.0));
}

void main()
{
    vec2 pix = floor(gl_FragCoord.xy) * 2.0;
    float d = texelFetch(depthTex, ivec2(pix), 0).r;
    float dc = min(d, 0.999999);
    vec2 ndc = vec2((pix.x + 0.5) / P.proj.z * 2.0 - 1.0, 1.0 - (pix.y + 0.5) / P.proj.w * 2.0);
    vec4 w = P.invViewProj * vec4(ndc, dc, 1.0);
    vec3 toPoint = w.xyz / w.w - P.eye.xyz;
    vec3 dir = normalize(toPoint);
    float dist = d >= 1.0 ? 1e9 : length(toPoint);
    float reach = min(dist, P.eye.w);

    // The sun (or none): its light, the way to it, the phase towards the eye (Henyey-Greenstein, g 0.5; scaled so
    // light scattered evenly is 1, its peak capped - the dust round the sun glows without washing out).
    vec3 toSun = -normalize(FL.sunDir.xyz);
    bool sun = dot(FL.sunColor.rgb, FL.sunColor.rgb) > 0.0 && toSun.y > -0.02;
    const float g = 0.5;
    float phase = min((1.0 - g * g) / pow(1.0 + g * g - 2.0 * g * dot(dir, toSun), 1.5), 2.5);
    phase = mix(phase, 1.0, P.gusts.w);          // (ash: dull, no glow towards the sun)
    float sunUp = max(toSun.y, 0.02);
    // (The dust reddens what crosses it: blue taken a little more than red.)
    const vec3 kRedden = vec3(0.85, 1.0, 1.25);
    float casterFade = smoothstep(0.05, 0.17, toSun.y);   // (things' shadows fade as the sun nears the horizon)
    if (sun)
        for (int c = 0; c < int(FL.shadowParams.z); ++c) {
            gRayStart[c] = (FL.shadowViewProj[c] * vec4(P.eye.xyz, 1.0)).xyz;
            gRayDir[c] = (FL.shadowViewProj[c] * vec4(dir, 0.0)).xyz;
        }
    float shadows = FL.shadowParams.x > 0.5 ? 1.0 : 0.0;
    vec3 moon = FL.moon.w > 0.5 ? vec3(0.0) : FL.moon.rgb * 0.6;   // (the moon's own shadow map: its light is the "sun" above)

    // The storm's dust: a thick layer near the ground (scale height P.dens.y) and a tall thin column above it (the
    // storm's upper dust, kUpper of it at the ground, scale height kUpperHeight) - the same for the land, things high
    // up (the ships in the sky) and the sky itself, so none stands clear of what hides the others.
    const float kUpper = 0.3, kUpperHeight = 1500.0;
    // Its light in this direction: the sky's and the moon's; the sun through half the thick layer above (reddened),
    // its glow towards the eye softened (the light in a storm has scattered many times), and what the dust took from
    // it spread round (else a thick storm went near black). One light for the whole ray - shaded only where the sun
    // is shut out (shadows, the terrain at a low sun, clouds) - so the far land fades into the sky behind it with no
    // other colour (the user: the hills differed from the sky; the sky blazed orange lit as the thin dust high up).
    vec3 sunDirect = vec3(0.0), base = P.sky.rgb + moon;
    if (sun) {
        vec3 mid = exp(-kRedden * (P.dens.x * (1.0 - kUpper) * P.dens.y * 0.5 / sunUp));
        sunDirect = FL.sunColor.rgb * mid * mix(1.0, phase, 0.35);   // (gentle: away from the sun as dark as the sim sand)
        base += FL.sunColor.rgb * (0.8 * max(toSun.y, 0.0)) * (1.0 - mid);
    }
    float transmit = 1.0;
    vec3 light = vec3(0.0);
    int steps = int(P.sky.w);
    float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))) + P.albedo.w);
    float tPrev = 0.0, hLast = 0.0, shadeLast = 1.0;
    for (int i = 0; i < steps; ++i) {
        float u = (float(i) + noise) / float(steps), t = reach * u * u;
        float dt = t - tPrev;
        tPrev = t;
        vec3 q = P.eye.xyz + dir * t;
        vec2 tm = TerrainAt(q.xz);
        float h = max(q.y - tm.y, 0.0);
        hLast = h;
        float sigma = P.dens.x * ((1.0 - kUpper) * exp(-h / P.dens.y) * Billow(q, h) + kUpper * exp(-h / kUpperHeight));
        // (Clear round the eye - RVK_SandClear, fading in over as many again; held off further in a lee.)
        sigma *= smoothstep(0.35 * P.dens.w, 1.6 * P.dens.w, t) * (1.0 - 0.75 * P.gusts.z * (1.0 - smoothstep(10.0, 40.0, t)));
        float shade = 1.0;
        if (sun) {
            if (shadows > 0.0)
                shade = mix(1.0, SunVisible(t), casterFade);
            if (P.terrain.w > 1.5)               // (the terrain's shade from a low sun: under the height to see it)
                shade *= smoothstep(tm.x - 1.0, tm.x + 3.0, q.y);
            shade *= CloudShadowMap(q);
        }
        shadeLast = P.terrain.w > 1.5 ? smoothstep(tm.x - 1.0, tm.x + 3.0, q.y) : 1.0;
        float a = 1.0 - exp(-sigma * dt);
        light += transmit * a * (P.albedo.rgb * (base + sunDirect * shade));
        transmit *= 1.0 - a;
    }

    // Past the march: the rest of the ray - to the surface, or for the sky out of the dust - through both layers
    // in closed form (from the last step's height, the ground taken as level), lit as the march without the near
    // shadows.
    if (dist > reach && transmit > 1e-5) {
        float len = min(dist - reach, 50000.0);
        float k1 = dir.y / P.dens.y, k2 = dir.y / kUpperHeight;
        float d1 = abs(k1 * len) < 1e-3 ? len : (1.0 - exp(-k1 * len)) / k1;
        float d2 = abs(k2 * len) < 1e-3 ? len : (1.0 - exp(-k2 * len)) / k2;
        // (Never thicker than at the ground.)
        float tau = P.dens.x * ((1.0 - kUpper) * min(exp(-hLast / P.dens.y) * d1, len) +
                                kUpper * min(exp(-hLast / kUpperHeight) * d2, len));
        float a = 1.0 - exp(-tau);
        light += transmit * a * (P.albedo.rgb * (base + sunDirect * shadeLast));
        transmit *= 1.0 - a;
    }
    // The sun's disc and the sky round it behind the fog: seen through it as the land's sunlight comes through it
    // (rvk wind.cpp: kSunPath of the column's depth) - dim and red, not gone (the user). (The disc is far brighter
    // than the sky: through the dust it still shows, as the sun through a storm does.)
    if (d >= 1.0 && sun) {
        float near_ = smoothstep(0.978, 0.9997, dot(dir, toSun));   // (~12 degrees out to the disc)
        transmit = pow(max(transmit, 1e-12), mix(1.0, 0.3, near_));
    }
    outFog = vec4(light, transmit);
}
