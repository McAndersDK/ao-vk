#version 450
// A lightning bolt of our own (rvk/rain.cpp: RenderRain, into the rain layer - not veiled as far as what is behind it):
// its segments (binding 3: the channel from the cloud base to the ground, its branches), six vertices each, a ribbon
// turned to the camera - a few pixels across at least, wider for its glow (bolt.frag).
#include "rain_common.glsl"

layout(set = 0, binding = 3, std140) uniform BoltSegments {
    vec4 info;              // x: segments, y: its brightness now (HDR, the fog between taken off), zw: unused
    vec4 seg[2 * 256];      // per segment: a (xyz, w: width - world units), b (xyz, w: its own brightness 0..1)
} B;

layout(location = 0) out vec2 vCorner;      // x: 0..1 along; y: -1..1 across (the glow's full width)
layout(location = 1) out float vBright;
layout(location = 2) out float vCore;       // the core's share of the ribbon's half width (the rest: glow)

void main()
{
    uint vi = uint(gl_VertexIndex), s = vi / 6u, corner = vi % 6u;
    if (s >= uint(B.info.x)) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        vCorner = vec2(0.0); vBright = 0.0; vCore = 1.0;
        return;
    }
    vec4 a = B.seg[2u * s], b = B.seg[2u * s + 1u];
    vec4 ca = R.viewProj * vec4(a.xyz, 1.0), cb = R.viewProj * vec4(b.xyz, 1.0);
    if (ca.w < 0.5 || cb.w < 0.5) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        vCorner = vec2(0.0); vBright = 0.0; vCore = 1.0;
        return;
    }
    vec2 halfPx = R.viewport.xy * 0.5;
    vec2 along = (cb.xy / cb.w - ca.xy / ca.w) * halfPx;
    along = dot(along, along) > 1e-4 ? normalize(along) : vec2(0.0, 1.0);
    // Its core's width on the screen (at least 1.5 pixels) and its glow round it (4x).
    float px = a.w / ca.w * R.proj.w * halfPx.y;
    float core = max(px, 1.5) * 0.5, glow = core * 4.0 + 3.0;
    const vec2 corners[6] = vec2[](vec2(0.0, -1.0), vec2(0.0, 1.0), vec2(1.0, 1.0), vec2(0.0, -1.0), vec2(1.0, 1.0),
                                   vec2(1.0, -1.0));
    vec2 c = corners[corner];
    vec4 clip = mix(ca, cb, c.x);
    clip.xy += vec2(-along.y, along.x) * (c.y * glow) / halfPx * clip.w;
    gl_Position = clip;
    vCorner = c;
    vCore = core / glow;
    vBright = B.info.y * b.w;
}
