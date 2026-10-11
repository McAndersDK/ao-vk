#version 450
// The rain (rvk/rain.cpp RenderRain): R.time.w drops in a box round the camera, six vertices each and no buffers.
// A drop's place is a function of its index and the time alone: a random start in the box, moved by its velocity
// (the fall, the wind, a little its own) and wrapped into the box - the box anchored to the world, so the drops stay
// where they are as the camera moves and only those crossing its walls wrap. Drawn as a streak from where it is back
// to where it was a moment ago (the eye sees a falling drop as one), at least about a pixel wide (thinner: fainter
// instead, or it would flicker), lit by the ambient light, the sun and the lamps near - brightest looking towards a
// light through the rain.
#include "rain_common.glsl"

layout(location = 0) out vec2 vCorner;      // x: 0 the tail .. 1 the head; y: -1..1 across
layout(location = 1) out vec3 vLight;       // its colour
layout(location = 2) out float vAlpha;      // its opacity (0: not drawn)
layout(location = 3) out vec4 vClip;
layout(location = 4) out vec4 vPrevClip;

const float kStreak = 0.075;                // seconds of the fall a streak shows
const float kNearShare = 0.4;               // of the drops, in a box of kNearSize x the box round the camera (the
const float kNearSize = 0.38;               //   near ones make the rain: there it is denser)
const float kWidth = 0.012;                 // a drop's width (units)

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
    uint vi = uint(gl_VertexIndex), drop = vi / 6u, corner = vi % 6u;
    uint s = Hash(drop * 7919u + 3u);
    vec3 seed = vec3(Rand(s), Rand(s), Rand(s));
    float speed = 0.85 + 0.3 * Rand(s);
    vec3 v = R.fall.xyz * speed + vec3(Rand(s) - 0.5, 0.0, Rand(s) - 0.5) * 0.6;
    bool nearBox = float(drop) < kNearShare * R.time.w;
    vec3 size = vec3(2.0 * R.fall.w * (nearBox ? kNearSize : 1.0));
    vec3 lo = R.camera.xyz - size * 0.5;
    float t = R.time.x;
    vec3 head = lo + mod(seed * size + v * t - lo, size);
    vec3 tail = head - v * kStreak;
    vec3 prevHead = head - v * max(t - R.time.y, 0.0);

    // Faded out towards the box's walls (where the drops wrap) and right at the camera.
    vec3 rel = (head - R.camera.xyz) / (size * 0.5);
    float edge = 1.0 - smoothstep(0.65, 1.0, max(abs(rel.x), max(abs(rel.y), abs(rel.z))));
    float dist = distance(head, R.camera.xyz);
    float alpha = R.colour.w * edge * smoothstep(0.3, 1.2, dist);

    vec4 cHead = R.viewProj * vec4(head, 1.0), cTail = R.viewProj * vec4(tail, 1.0);
    if (cHead.w < 0.05 || cTail.w < 0.05 || alpha <= 0.0) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);  // (behind the camera or faded out: not drawn)
        vAlpha = 0.0;
        vCorner = vec2(0.0);
        vLight = vec3(0.0);
        vClip = vPrevClip = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    // Across the streak on the screen, so many pixels wide: its own width there, or a pixel (then fainter).
    vec2 halfPx = R.viewport.xy * 0.5;
    vec2 along = (cHead.xy / cHead.w - cTail.xy / cTail.w) * halfPx;
    along = dot(along, along) > 0.25 ? normalize(along) : vec2(0.0, 1.0);
    vec3 upW = vec3(R.view[0][1], R.view[1][1], R.view[2][1]);
    vec4 cUp = R.viewProj * vec4(head + upW * kWidth, 1.0);
    float px = length((cUp.xy / cUp.w - cHead.xy / cHead.w) * halfPx);
    alpha *= min(px / 1.2, 1.0);
    float halfWidth = max(px, 1.2) * 0.5;
    const vec2 corners[6] = vec2[](vec2(0.0, -1.0), vec2(0.0, 1.0), vec2(1.0, 1.0), vec2(0.0, -1.0), vec2(1.0, 1.0),
                                   vec2(1.0, -1.0));
    vec2 c = corners[corner];
    vec4 clip = mix(cTail, cHead, c.x);
    clip.xy += vec2(-along.y, along.x) * (c.y * halfWidth) / halfPx * clip.w;

    // Its light: the ambient, a little of the sun, much more looking towards it through the rain; the lamps likewise.
    vec3 V = normalize(R.camera.xyz - head);
    vec3 L = -normalize(FL.sunDir.xyz + vec3(0.0, -1e-6, 0.0));
    vec3 light = R.ambient.rgb * 1.2 + FL.sunColor.rgb * (0.5 + 1.5 * pow(max(dot(-V, L), 0.0), 8.0));
    uint count = min(FL.info.x, 64u);
    for (uint i = 0u; i < count; ++i) {
        Light li = FL.lights[i];
        uint type = uint(li.position.w + 0.5);
        if (type != 1u && type != 2u)
            continue;
        vec3 dl = li.position.xyz - head;
        float d = length(dl);
        if (d >= li.direction.w || d < 1e-3)
            continue;
        float att = 1.0 / max(li.atten.x + li.atten.y * d + li.atten.z * d * d, 1e-3);
        att *= 1.0 - smoothstep(0.75, 1.0, d / li.direction.w);
        light += li.diffuse.rgb * (att * (0.6 + 3.0 * pow(max(dot(-V, dl / d), 0.0), 6.0)));
    }
    float fog = RainFog(dist);
    vLight = mix(R.fogColour.rgb, R.colour.rgb * light, fog);
    vAlpha = alpha * mix(0.3, 1.0, fog);
    vCorner = c;
    vClip = cHead;
    vPrevClip = R.prevViewProj * vec4(prevHead, 1.0);
    gl_Position = clip;
    if (R.camera.w > 0.5)
        gl_Position.xy += FL.taa.xy * gl_Position.w;
}
