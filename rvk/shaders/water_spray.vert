#version 450
// The spray and mist at the falls' feet (rvk/water.cpp DrawWater, after the water): camera-facing quads, kPerFoot per
// foot segment, six vertices each and no buffers of their own. Each particle's life is a function of its index and
// the time alone - born at a random point along its segment, thrown out from the cliff, gone, born again elsewhere -
// so nothing is simulated or kept. The first kMist of a segment are mist (big, slow, long-lived: drifting out and up,
// held back by the air, carried by the wind), the rest drops (small, quick: thrown out and up, falling back).
#include "water_common.glsl"

layout(set = 0, binding = 2, std430) readonly buffer Feet { FootSegment feet[]; };

layout(location = 0) out vec2 vCorner;      // -1..1 across the quad
layout(location = 1) out vec3 vPos;         // world position
layout(location = 2) out float vAlpha;      // its opacity now (0: not drawn)
layout(location = 3) out float vMist;       // 1: mist, 0: a drop
layout(location = 4) out float vFootY;      // the pool's surface under it (where its foot is)
layout(location = 5) out vec2 vSeed;        // its own place in the noise (mist's shape)

const uint kPerFoot = 48u;                  // (rvk/water.cpp kSprayPerFoot)
const uint kMist = 16u;

uint Hash(uint x)
{
    x ^= x >> 16u;
    x *= 0x7FEB352Du;
    x ^= x >> 15u;
    x *= 0x846CA68Bu;
    x ^= x >> 16u;
    return x;
}
float Rand(inout uint s)
{
    s = Hash(s);
    return float(s >> 8u) * (1.0 / 16777216.0);
}

void main()
{
    uint vi = uint(gl_VertexIndex), particle = vi / 6u, corner = vi % 6u;
    uint segment = particle / kPerFoot, k = particle % kPerFoot;
    FootSegment fs = feet[segment];
    vec3 p0 = fs.a.xyz, p1 = vec3(fs.a.w, fs.b.x, fs.b.y);
    vec3 outward = vec3(fs.b.z, 0.0, fs.b.w);
    vec3 along = p1 - p0;
    along = dot(along, along) > 1e-6 ? normalize(along) : vec3(1.0, 0.0, 0.0);
    bool mist = k < kMist;
    float life = mist ? 4.0 : 1.1;

    // Its cycle: a phase of its own; each new cycle a new start and throw.
    uint seed = Hash(particle * 7919u + 17u);
    float cycle = W.time.x / life + float(seed >> 8u) * (1.0 / 16777216.0);
    float age01 = fract(cycle), age = age01 * life;
    uint s = Hash(seed ^ (uint(floor(cycle)) * 2654435761u));
    vec3 start = mix(p0, p1, Rand(s));
    vec3 pos;
    float size, alpha;
    if (mist) {
        vec3 v = outward * (1.0 + 2.5 * Rand(s)) + vec3(0.0, 0.6 + 1.6 * Rand(s), 0.0) + along * ((Rand(s) - 0.5) * 1.5);
        const float drag = 0.6;
        pos = start + outward * 0.4 + v * ((1.0 - exp(-drag * age)) / drag) +
              vec3(W.wind.x, 0.0, W.wind.y) * (0.4 * age) + vec3(0.0, 0.15 * age, 0.0);
        size = mix(1.2, 5.0, sqrt(age01)) * (0.7 + 0.6 * Rand(s));
        alpha = smoothstep(0.0, 0.15, age01) * (1.0 - smoothstep(0.5, 1.0, age01)) * 0.16;
    } else {
        vec3 v = outward * (1.5 + 3.5 * Rand(s)) + vec3(0.0, 2.0 + 4.0 * Rand(s), 0.0) + along * ((Rand(s) - 0.5) * 2.0);
        pos = start + v * age - vec3(0.0, 4.9 * age * age, 0.0);
        size = 0.12 + 0.18 * Rand(s);
        alpha = (1.0 - smoothstep(0.6, 1.0, age01)) * 0.6 * (pos.y > start.y - 0.3 ? 1.0 : 0.0);
    }
    // (Faded out close to the camera: a quad filling the screen.)
    alpha *= smoothstep(1.0, 4.0, distance(pos, W.camera.xyz)) * W.fall2.x;

    const vec2 corners[6] = vec2[](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, -1.0), vec2(1.0, 1.0),
                                   vec2(-1.0, 1.0));
    vec2 c = corners[corner];
    vec3 right = vec3(W.view[0][0], W.view[1][0], W.view[2][0]);
    vec3 up = vec3(W.view[0][1], W.view[1][1], W.view[2][1]);
    vec3 wp = pos + (right * c.x + up * c.y) * size;
    vCorner = c;
    vPos = wp;
    vAlpha = alpha;
    vMist = mist ? 1.0 : 0.0;
    vFootY = start.y;
    vSeed = vec2(Rand(s), Rand(s)) * 7.0;
    gl_Position = alpha > 0.0 ? W.viewProj * vec4(wp, 1.0) : vec4(2.0, 2.0, 2.0, 1.0);   // (off screen: not drawn)
    if (W.camera.w > 0.5)
        gl_Position.xy += FL.taa.xy * gl_Position.w;
}
