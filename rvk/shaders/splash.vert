#version 450
// The rain's splashes (rvk/rain.cpp RenderRain, after the streaks): R.time.w slots, six vertices each and no buffers.
// Each slot, every kLife seconds, picks a point of the screen and finds the surface there in the scene's depth (the
// ground, a roof, the water): a splash if that surface is near (kRange), faces up and is not on an edge, and with a
// chance growing with the square of its distance - each pixel further away covers that much more ground, so the
// splashes come out even over the ground, not over the screen. Drawn as a small upright crown there (splash.frag).
#include "rain_common.glsl"

layout(set = 0, binding = 1) uniform sampler2D sceneDepth;   // the scene's depth (readable for this pass)

layout(location = 0) out vec2 vCorner;      // x: -1..1 across; y: -0.25 (just under the surface) .. 1 up
layout(location = 1) out vec3 vLight;
layout(location = 2) out float vAlpha;      // 0: not drawn
layout(location = 3) out float vAge;        // 0..1 through the splash
layout(location = 4) out vec2 vSeed;        // its droplets' own pattern
layout(location = 5) out float vViewZ;      // this vertex's view depth (the surface hides what is under it)
layout(location = 6) out vec4 vClip;
layout(location = 7) out vec4 vPrevClip;

const float kLife = 0.24;                   // seconds a splash lasts
const float kRange = 25.0;                  // splashes this near only (further they are under a pixel)

// The world point on the surface at a pixel (and its view depth).
vec3 SurfaceAt(ivec2 px, out float z)
{
    float d = texelFetch(sceneDepth, px, 0).r;
    z = d >= 1.0 ? 1e5 : RainViewZ(d);
    vec2 ndc = vec2((float(px.x) + 0.5) * R.viewport.z * 2.0 - 1.0, 1.0 - (float(px.y) + 0.5) * R.viewport.w * 2.0);
    vec3 q = vec3(ndc.x * z / R.proj.z, ndc.y * z / R.proj.w, z);
    vec3 right = vec3(R.view[0][0], R.view[1][0], R.view[2][0]);
    vec3 up = vec3(R.view[0][1], R.view[1][1], R.view[2][1]);
    vec3 forward = vec3(R.view[0][2], R.view[1][2], R.view[2][2]);
    return R.camera.xyz + right * q.x + up * q.y + forward * q.z;
}

void hide()
{
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
    vAlpha = 0.0;
    vCorner = vec2(0.0);
    vLight = vec3(0.0);
    vAge = 0.0;
    vSeed = vec2(0.0);
    vViewZ = 0.0;
    vClip = vPrevClip = vec4(0.0, 0.0, 0.0, 1.0);
}

void main()
{
    uint vi = uint(gl_VertexIndex), slot = vi / 6u, corner = vi % 6u;
    uint seed = RainHash(slot * 9781u + 11u);
    float phase = R.time.x / kLife + float(seed >> 8u) * (1.0 / 16777216.0);
    float age = fract(phase);
    uint s = RainHash(seed ^ (uint(floor(phase)) * 2654435761u));
    ivec2 px = ivec2(vec2(RainRand(s), RainRand(s)) * R.viewport.xy);
    float z;
    vec3 p = SurfaceAt(px, z);
    if (z > kRange || z < 0.5 || RainRand(s) > (z / kRange) * (z / kRange) * 4.0) {
        hide();
        return;
    }
    // Facing up, not on an edge (neighbours a few pixels away on the same surface).
    float zx, zy;
    vec3 px1 = SurfaceAt(px + ivec2(3, 0), zx), py1 = SurfaceAt(px + ivec2(0, 3), zy);
    vec3 n = cross(px1 - p, py1 - p);
    float nl = length(n);
    if (nl < 1e-8 || abs(n.y) / nl < 0.7 || abs(zx - z) > 0.03 * z + 0.2 || abs(zy - z) > 0.03 * z + 0.2) {
        hide();
        return;
    }

    // An upright quad turned to the camera about the vertical, standing on the surface.
    float size = 0.12 + 0.14 * RainRand(s);
    vec3 right = vec3(R.view[0][0], 0.0, R.view[2][0]);
    right = dot(right, right) > 1e-6 ? normalize(right) : vec3(1.0, 0.0, 0.0);
    const vec2 corners[6] = vec2[](vec2(-1.0, -0.25), vec2(1.0, -0.25), vec2(1.0, 1.0), vec2(-1.0, -0.25), vec2(1.0, 1.0),
                                   vec2(-1.0, 1.0));
    vec2 c = corners[corner];
    vec3 wp = p + right * (c.x * size) + vec3(0.0, c.y * size * 1.4, 0.0);
    float dist = distance(p, R.camera.xyz);
    float fog = RainFog(dist);
    vLight = mix(R.fogColour.rgb, R.colour.rgb * RainLight(p), fog);
    vAlpha = 0.85 * min(R.time.z * 1.5, 1.0) * mix(0.3, 1.0, fog);
    vCorner = c;
    vAge = age;
    vSeed = vec2(RainRand(s), RainRand(s));
    vViewZ = (R.view * vec4(wp, 1.0)).z;
    vClip = R.viewProj * vec4(p, 1.0);
    vPrevClip = R.prevViewProj * vec4(p, 1.0);
    gl_Position = R.viewProj * vec4(wp, 1.0);
    if (R.camera.w > 0.5)
        gl_Position.xy += FL.taa.xy * gl_Position.w;
}
