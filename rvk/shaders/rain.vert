#version 450
// The rain (rvk/rain.cpp RenderRain): R.time.w drops in a box round the camera, six vertices each and no buffers.
// A drop's place is a function of its index and the time alone: a random start in the box, moved by its velocity
// (the fall, the wind, a little its own) and wrapped into the box - the box anchored to the world, so the drops stay
// where they are as the camera moves and only those crossing its walls wrap. Drawn as a streak from where it is back
// to where it was a moment ago (the eye sees a falling drop as one), at least about a pixel wide (thinner: fainter
// instead, or it would flicker), lit by the ambient light, the sun and the lamps near - brightest looking towards a
// light through the rain.
#include "rain_common.glsl"
#include "wind.glsl"

layout(location = 0) out vec2 vCorner;      // x: 0 the tail .. 1 the head; y: -1..1 across
layout(location = 1) out vec3 vLight;       // its colour
layout(location = 2) out float vAlpha;      // its opacity (0: not drawn)
layout(location = 3) out vec4 vClip;
layout(location = 4) out vec4 vPrevClip;

const float kNearShare = 0.4;               // of the drops, in a box of kNearSize x the box round the camera (the
const float kNearSize = 0.38;               //   near ones make the rain: there it is denser)
const float kWidth = 0.012;                 // a drop's width (units)

void main()
{
    uint vi = uint(gl_VertexIndex), drop = vi / 6u, corner = vi % 6u;
    uint s = RainHash(drop * 7919u + 3u);
    vec3 seed = vec3(RainRand(s), RainRand(s), RainRand(s));
    float speed = 0.85 + 0.3 * RainRand(s);
    vec3 own = vec3(RainRand(s) - 0.5, 0.0, RainRand(s) - 0.5) * 0.6;
    vec3 v = R.fall.xyz * speed + own;
    bool nearBox = float(drop) < kNearShare * R.time.w;
    vec3 size = vec3(2.0 * R.fall.w * (nearBox ? kNearSize : 1.0));
    vec3 lo = R.camera.xyz - size * 0.5;
    float t = R.time.x;
    // (Its way so far: the rain's, added up as the wind changed, x its own speed; its own drift on the clock.)
    vec3 head = lo + mod(seed * size + R.drift.xyz * speed + own * t - lo, size);
    vec3 prevHead = head - v * max(t - R.time.y, 0.0);
    // A gust front passing leans it further (the streak: the eye reads the slant) and gathers the rain there into a
    // curtain, thinner between (about as much rain on the whole).
    float gust = Gust(WindAxes(head.xz, FL.wind.xy, FL.windFrame.xy), FL.wind.z);
    vec3 tail = head - (v + vec3(FL.wind.x, 0.0, FL.wind.y) * (R.drift.w * gust)) * R.curtains.y;

    // Faded out towards the box's walls (where the drops wrap) and right at the camera.
    vec3 rel = (head - R.camera.xyz) / (size * 0.5);
    float edge = 1.0 - smoothstep(0.65, 1.0, max(abs(rel.x), max(abs(rel.y), abs(rel.z))));
    float dist = distance(head, R.camera.xyz);
    float alpha = R.colour.w * edge * smoothstep(0.3, 1.2, dist) * mix(1.0, 0.6 + 2.6 * gust, R.curtains.x);

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

    vec3 light = RainLight(head);
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
