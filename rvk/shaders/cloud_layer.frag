#version 450
// The cloud layer (rvk/clouds.cpp MarchCloudLayer; RVK_CloudLayer): a shell of cloud between a base and a top height
// over a large sphere round the eye (so it bends down to the horizon), marched along each view ray at half size. What
// is where comes from the weather map (coverage, type, density: the game's puffs and weather); the shape from tiling
// noise - a Perlin-Worley body by the type's height profile, eaten into by coverage, its edges eroded by finer Worley
// (wispy below, billowy above); the light from the sun through the cloud (a few steps towards it, several scattering
// orders, a forward lobe for silver edges, powdery dark edges) and from the sky (lighter at the tops), tinted by the
// game's own cloud colour. Premultiplied; filtered up over the scene (cloud_up.frag).
#include "frame_lights.glsl"                    // FL (binding 4): the sun, the TAA's noise

layout(set = 0, binding = 0, std140) uniform LayerFrame {
    mat4 invViewProj;   // clip -> world (the world camera; D3D row-vector convention, as the scene's)
    vec4 eye;           // xyz: the eye (world); w: time (s)
    vec4 layer;         // x: the base's height, y: the top's (world y), z: the sphere's radius, w: the march's reach
    vec4 weather;       // xy: the weather map's corner (world x z), z: 1 / its size, w: 1 = skip covered pixels
    vec4 shape;         // x: the shape noise's scale (per world unit), y: the detail's, z: erosion, w: extinction
    vec4 wind;          // xyz: the noise's drift (world), w: the pre-pass's clear depth
    vec4 tint;          // rgb: the game's cloud colour, w: brightness
    vec4 ambient;       // rgb: the sky's light; w: rain (0..1)
    vec4 haze;          // rgb: the distance's colour (the game's fog); w: the haze's distance (world)
    vec4 view;          // xy: the half-size target's size, z: steps, w: steps towards the sun
} L;
layout(set = 0, binding = 1) uniform sampler2D weatherMap;
layout(set = 0, binding = 2) uniform sampler3D shapeNoise;
layout(set = 0, binding = 3) uniform sampler2D sceneDepth;  // the pre-pass's (L.weather.w): only where it is clear
layout(set = 0, binding = 5) uniform sampler3D detailNoise;

layout(location = 0) out vec4 outScene;

const float kPi = 3.14159265;

float Remap(float v, float lo, float hi, float a, float b) { return a + (v - lo) * (b - a) / max(hi - lo, 1e-5); }

// Where a ray from o along unit d is inside the sphere at c of radius r: t0 <= t1 (t1 < 0: never).
vec2 Sphere(vec3 o, vec3 d, vec3 c, float r)
{
    vec3 oc = o - c;
    float b = dot(oc, d), q = dot(oc, oc) - r * r, h = b * b - q;
    if (h < 0.0)
        return vec2(1.0, -1.0);
    h = sqrt(h);
    return vec2(-b - h, -b + h);
}

vec3 gCentre;
float gInner, gThick;

// 0 at the base .. 1 at the top.
float Height(vec3 p) { return (length(p - gCentre) - gInner) / gThick; }

// How much of the layer's height the type fills: stratus a low thin band, cumulus a flat base and rounded top half
// way up, storm clouds nearly all of it.
float Profile(float h, float type)
{
    float stratus = smoothstep(0.0, 0.06, h) * (1.0 - smoothstep(0.12, 0.28, h));
    float cumulus = smoothstep(0.0, 0.1, h) * (1.0 - smoothstep(0.35, 0.65, h));
    float storm = smoothstep(0.0, 0.1, h) * (1.0 - smoothstep(0.7, 1.0, h));
    return type < 0.5 ? mix(stratus, cumulus, type * 2.0) : mix(cumulus, storm, type * 2.0 - 1.0);
}

float Density(vec3 p, bool detail)
{
    float h = Height(p);
    if (h <= 0.0 || h >= 1.0)
        return 0.0;
    vec2 uv = (p.xz - L.weather.xy) * L.weather.z;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))))
        return 0.0;
    vec4 w = textureLod(weatherMap, uv, 0.0);
    float cover = w.r;
    if (cover < 0.01)
        return 0.0;
    vec3 q = (p + L.wind.xyz) * L.shape.x;
    q.y *= 1.6;                                  // (cells a little flatter than wide)
    vec4 n = textureLod(shapeNoise, q, 0.0);
    float fbm = n.g * 0.625 + n.b * 0.25 + n.a * 0.125;
    float body = Remap(n.r, fbm - 1.0, 1.0, 0.0, 1.0) * Profile(h, w.g);
    // Coverage eats into the body from its thin parts: little cover, only the cores remain.
    float d = clamp(Remap(body, 1.0 - cover, 1.0, 0.0, 1.0), 0.0, 1.0) * cover;
    if (d <= 0.0 || !detail)
        return d * w.b;
    vec3 e = textureLod(detailNoise, (p + L.wind.xyz * 1.7) * L.shape.y, 0.0).rgb;
    float ef = e.r * 0.625 + e.g * 0.25 + e.b * 0.125;
    ef = mix(ef, 1.0 - ef, clamp(h * 4.0, 0.0, 1.0));   // wispy underneath, billowy above
    d = clamp(Remap(d, ef * L.shape.z, 1.0, 0.0, 1.0), 0.0, 1.0);
    return d * w.b;
}

float HG(float c, float g) { return (1.0 - g * g) / (4.0 * kPi * pow(1.0 + g * g - 2.0 * g * c, 1.5)); }

void main()
{
    // The four scene pixels under this one: none of them sky - an opaque surface is drawn there later.
    if (L.weather.w > 0.5) {
        ivec2 at = ivec2(gl_FragCoord.xy) * 2, last = textureSize(sceneDepth, 0) - 1;
        bool open = false;
        for (int k = 0; k < 4; ++k)
            open = open || texelFetch(sceneDepth, min(at + ivec2(k & 1, k >> 1), last), 0).r == L.wind.w;
        if (!open)
            discard;
    }
    // The view ray (the scene's flipped viewport; the TAA's jitter taken out as the scene's draws put it in).
    vec2 ndc = vec2(gl_FragCoord.x / L.view.x * 2.0 - 1.0, 1.0 - gl_FragCoord.y / L.view.y * 2.0) - FL.taa.xy;
    vec4 a = L.invViewProj * vec4(ndc, 0.0, 1.0), b = L.invViewProj * vec4(ndc, 1.0, 1.0);
    vec3 eye = L.eye.xyz;
    vec3 dir = normalize(b.xyz / b.w - a.xyz / a.w);
    // The shell: the ground's sphere through the eye's height (the layer's heights above it).
    float R = L.layer.z;
    gCentre = vec3(eye.x, eye.y - R, eye.z);
    gInner = R + (L.layer.x - eye.y);
    float outer = R + (L.layer.y - eye.y);
    gThick = outer - gInner;
    vec2 si = Sphere(eye, dir, gCentre, gInner), so = Sphere(eye, dir, gCentre, outer);
    float r0 = length(eye - gCentre);
    float t0, t1;
    if (r0 < gInner) {                           // under the layer: from leaving the inner sphere to the outer
        t0 = si.y;
        t1 = so.y;
    } else if (r0 < outer) {                     // in it: to the outer sphere, or down into the inner one
        t0 = 0.0;
        t1 = si.y > 0.0 && si.x > 0.0 ? si.x : so.y;
    } else {                                     // above it
        if (so.y < 0.0 || so.x > so.y) { outScene = vec4(0.0); return; }
        t0 = max(so.x, 0.0);
        t1 = si.x > 0.0 && si.y > si.x ? si.x : so.y;
    }
    t1 = min(t1, L.layer.w);
    if (t1 <= t0) {
        outScene = vec4(0.0);
        return;
    }
    int steps = int(L.view.z);
    float dt = (t1 - t0) / float(steps);
    float jitter = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))) + FL.taa.z);
    vec3 sunL = -normalize(FL.sunDir.xyz + vec3(0.0, -1e-6, 0.0));
    float sunUp = smoothstep(-0.08, 0.12, sunL.y);
    float cosT = dot(dir, sunL);
    vec3 sunColour = FL.sunColor.rgb * sunUp;
    float sigma = L.shape.w;
    float transmit = 1.0;
    vec3 light = vec3(0.0);
    float depthSum = 0.0, weightSum = 0.0;      // (where the cloud is along the ray: its haze)
    for (int i = 0; i < steps && transmit > 0.01; ++i) {
        float t = t0 + (float(i) + jitter) * dt;
        vec3 p = eye + dir * t;
        float d = Density(p, true);
        if (d <= 0.001)
            continue;
        // The sun through the cloud: a few steps towards it, longer each.
        float od = 0.0;
        float ls = gThick * 0.06;
        vec3 lp = p;
        for (int k = 0; k < int(L.view.w); ++k) {
            lp += sunL * ls;
            od += Density(lp, false) * ls;
            ls *= 1.6;
        }
        od *= sigma;
        // Several scattering orders (each dimmer, reaching deeper, less forward): light inside thick cloud.
        float sun = 0.0;
        float aa = 1.0, bb = 1.0, cc = 1.0;
        for (int o = 0; o < 3; ++o) {
            float phase = mix(HG(cosT, 0.6 * cc), HG(cosT, -0.25 * cc), 0.3) * 4.0 * kPi;
            sun += aa * exp(-od * bb) * phase;
            aa *= 0.5; bb *= 0.4; cc *= 0.5;
        }
        // Powder: an edge facing the sun is darker than the inside it lights (light enters, doesn't come back out).
        float powder = 1.0 - 0.5 * exp(-d * sigma * gThick * 0.08);
        float h = Height(p);
        // The sky's light: lighter at the tops, and dimmed by the cloud over the point (the sun's way through it
        // stands in for up: thick storm clouds get dark bases).
        vec3 sky = L.ambient.rgb * mix(0.55, 1.15, h) * mix(0.35, 1.0, exp(-od * 0.35));
        vec3 S = (sunColour * sun * powder + sky) * L.tint.rgb * L.tint.w;
        float stepT = exp(-d * sigma * dt);
        light += transmit * (1.0 - stepT) * S;
        depthSum += transmit * (1.0 - stepT) * t;
        weightSum += transmit * (1.0 - stepT);
        transmit *= stepT;
    }
    float alpha = 1.0 - transmit;
    if (alpha <= 0.0) {
        outScene = vec4(0.0);
        return;
    }
    // Haze: the cloud's distance (where along the ray it is) blends it into the game's distance colour; the very
    // far fades out (the march's reach is no horizon).
    float dist = depthSum / max(weightSum, 1e-6);
    float haze = 1.0 - exp(-dist / L.haze.w);
    light = mix(light, L.haze.rgb * alpha, haze * 0.85);
    float fade = 1.0 - smoothstep(0.7 * L.layer.w, L.layer.w, dist);
    outScene = vec4(light, alpha) * fade;
}
