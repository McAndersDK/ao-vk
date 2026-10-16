#version 450
// A storm's blown debris (rvk/rain.cpp RenderDebris): R.time.w bits - leaves, straw, twigs - in a box round the camera
// anchored to the world (as the rain's drops), skimming and hopping over the ground along the wind, tumbling; more of
// them airborne where a gust front is passing. Each bit's place a function of its index and the time alone: carried by
// the wind's way so far (R.drift), new each time round the box. Six vertices each, opaque (debris.frag).
#include "rain_common.glsl"
#include "wind.glsl"

layout(location = 0) out vec2 vCorner;      // -1..1 across the bit
layout(location = 1) out vec3 vColour;      // lit
layout(location = 2) out float vShape;      // 0: a leaf (oval), 1: a twig / straw (long, thin)
layout(location = 3) out vec4 vClip;
layout(location = 4) out vec4 vPrevClip;

// The ground's height at a point (the rain's cover map, B: over FL.wet.w), else the ground under the eye (R.drift.w).
float GroundAt(vec2 xz)
{
    if (FL.cover.z > 0.0) {
        vec2 uv = (xz - FL.cover.xy) * FL.cover.z;
        if (all(greaterThanEqual(uv, vec2(0.0))) && all(lessThan(uv, vec2(1.0)))) {
            float g = texelFetch(rainCover, ivec2(uv * vec2(textureSize(rainCover, 0))), 0).b;
            if (g > -8000.0) return g + FL.wet.w;
        }
    }
    return R.drift.w;
}

vec3 Place(uint drop, float t, vec3 drift, out float alpha, out uint fresh)
{
    uint s = RainHash(drop * 7919u + 77u);
    vec3 seed = vec3(RainRand(s), RainRand(s), RainRand(s));
    float speed = 0.5 + 1.0 * RainRand(s);
    vec3 size = vec3(2.0 * R.fall.w);
    vec3 lo = R.camera.xyz - size * 0.5;
    // (New each time round the box along the wind: elsewhere across, its own kind and hop.)
    vec2 moved = seed.xz * size.xz + drift.xz * speed - lo.xz;
    vec2 cycle = floor(moved / size.xz);
    fresh = RainHash(drop * 2654435761u + uint(int(cycle.x)) * 40503u + uint(int(cycle.y)) * 9973u + 5u);
    vec2 across = vec2(RainRand(fresh), RainRand(fresh));
    vec2 wd = FL.wind.xy, side = vec2(-wd.y, wd.x);
    vec2 xz = lo.xz + mod(moved, size.xz);
    xz += side * (across.x - 0.5) * 3.0;          // (spread across the wind a little more)
    float ground = GroundAt(xz);
    float gust = Gust(WindAxes(xz, wd, FL.windFrame.xy), FL.wind.z) * min(FL.leaves.z, 3.0);
    // Hopping: arcs a fraction of a unit to a couple high, higher in a gust; now and then skimming the ground.
    float hopRate = 1.2 + 1.8 * across.y, hop = abs(sin(t * hopRate + seed.x * 6.28));
    float height = 0.03 + hop * (0.08 + 0.6 * gust + 0.15 * R.curtains.x) * (0.3 + 0.7 * RainRand(fresh));
    vec3 rel = (vec3(xz.x, ground, xz.y) - R.camera.xyz) / (size * 0.5);
    float edge = 1.0 - smoothstep(0.6, 1.0, max(abs(rel.x), abs(rel.z)));
    alpha = edge * R.curtains.x * clamp(0.35 + 0.9 * gust, 0.0, 1.0) * step(RainRand(fresh), 0.25 + 0.35 * R.curtains.x);
    alpha *= 1.0 - RainSheltered(vec3(xz.x, ground + 0.5, xz.y));
    return vec3(xz.x, ground + height, xz.y);
}

void main()
{
    uint vi = uint(gl_VertexIndex), drop = vi / 6u, corner = vi % 6u;
    float alpha, alphaPrev;
    uint fresh, freshPrev;
    float t = R.time.x, tPrev = R.time.y;
    vec3 p = Place(drop, t, R.drift.xyz, alpha, fresh);
    vec3 pPrev = Place(drop, tPrev, R.drift.xyz - R.fall.xyz * max(t - tPrev, 0.0), alphaPrev, freshPrev);
    if (alpha < 0.05 || distance(p, R.camera.xyz) < 0.4) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        vCorner = vec2(0.0); vColour = vec3(0.0); vShape = 0.0;
        vClip = vPrevClip = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    uint k = fresh ^ 0x9E37u;
    float kind = RainRand(k);
    bool twig = kind > 0.7;
    float len = (twig ? 0.11 : 0.05) * (0.6 + 0.8 * RainRand(k)) * alpha;
    float wid = twig ? len * 0.08 : len * 0.6;
    // Tumbling: its two axes turned about a slowly turning axis.
    vec3 axis = normalize(vec3(RainRand(k) - 0.5, RainRand(k) - 0.5, RainRand(k) - 0.5) + 1e-3);
    float ang = t * (4.0 + 8.0 * RainRand(k));
    vec3 u0 = normalize(cross(axis, vec3(0.31, 0.83, 0.47))), v0 = cross(axis, u0);
    vec3 u = u0 * cos(ang) + v0 * sin(ang), v = cross(axis, u);
    const vec2 corners[6] = vec2[](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, -1.0), vec2(1.0, 1.0),
                                   vec2(-1.0, 1.0));
    vec2 c = corners[corner];
    vec3 world = p + u * (c.x * len) + v * (c.y * wid);
    // Its colour: a dry leaf, a green one, straw, a dark twig - lit by the sky and what sun the storm lets by.
    vec3 palette[4] = vec3[](vec3(0.36, 0.24, 0.11), vec3(0.22, 0.32, 0.10), vec3(0.55, 0.47, 0.28), vec3(0.16, 0.12, 0.08));
    vec3 col = palette[twig ? 3 : int(RainRand(k) * 2.99)];
    vec3 L = -normalize(FL.sunDir.xyz + vec3(0.0, -1e-6, 0.0));
    vec3 n = normalize(cross(u, v));
    vec3 light = R.ambient.rgb * 0.9 + FL.sunColor.rgb * (0.25 + 0.5 * abs(dot(n, L)));
    float dist = distance(world, R.camera.xyz);
    vColour = mix(R.fogColour.rgb, col * light, RainFog(dist));
    vShape = twig ? 1.0 : 0.0;
    vCorner = c;
    vClip = R.viewProj * vec4(world, 1.0);
    vPrevClip = R.prevViewProj * vec4(pPrev + u * (c.x * len) + v * (c.y * wid), 1.0);
    gl_Position = vClip;
    if (R.camera.w > 0.5)
        gl_Position.xy += FL.taa.xy * gl_Position.w;
}
