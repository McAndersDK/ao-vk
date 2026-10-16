#version 450
// The rain's splashes (rvk/rain.cpp RenderRain, after the streaks): R.time.w slots, six vertices each and no buffers.
// Each slot, every kLife seconds, picks a place on the ground plan - in a box round the camera anchored to the world,
// as the drops' (half the slots in a smaller box: denser near) - so it stays where it is while the camera moves. The
// surface there is found in the scene's depth: down a vertical line through the place, the first point that is
// behind what the screen shows is where the line meets it (the ground, a roof, the water). A splash if the line
// really meets it there (not just passes behind something nearer), the surface faces up and is not on an edge. Drawn
// as a small upright crown there (splash.frag).
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
const float kRange = 22.0;                  // half the box (further they are under a pixel)
const float kNearRange = 7.0;               // ... the near box's

// The view depth of a world point and the scene's depth at its pixel (false: off the screen or behind the camera).
bool Probe(vec3 p, out float pointZ, out float sceneZ)
{
    vec4 c = R.viewProj * vec4(p, 1.0);
    pointZ = c.w;
    sceneZ = 1e5;
    if (c.w < 0.3)
        return false;
    vec2 ndc = c.xy / c.w;
    if (any(greaterThan(abs(ndc), vec2(0.999))))
        return false;
    ivec2 px = ivec2((ndc.x * 0.5 + 0.5) * R.viewport.x, (0.5 - ndc.y * 0.5) * R.viewport.y);
    float d = texelFetch(sceneDepth, px, 0).r;
    sceneZ = d >= 1.0 ? 1e5 : RainViewZ(d);
    return true;
}

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
    // Its place on the ground plan, anchored to the world (wrapped into the box round the camera).
    float range = slot % 2u == 0u ? kNearRange : kRange;
    vec2 lo = R.camera.xz - range;
    vec2 xz = lo + mod(vec2(RainRand(s), RainRand(s)) * 4096.0 - lo, vec2(2.0 * range));
    // Down the vertical line through it: in front of the surface, then behind it - the meeting refined between.
    float y = R.camera.y + 12.0, step_ = 2.0, yFront = y;
    float pz, sz;
    bool found = false, front = false;
    for (int k = 0; k < 48; ++k) {
        if (!Probe(vec3(xz.x, y, xz.y), pz, sz)) {
            if (front) {                         // ran off the screen past the last point seen: closer, in smaller steps
                step_ *= 0.5;
                if (step_ < 0.03)
                    break;
                y = yFront - step_;
            } else {
                y -= step_;
            }
            continue;
        }
        if (pz > sz) {                           // behind what the screen shows
            if (!front) break;                   // (behind from the first point seen: under a roof, out of sight)
            float a = yFront, b = y;
            for (int i = 0; i < 10; ++i) {
                float m = 0.5 * (a + b);
                if (Probe(vec3(xz.x, m, xz.y), pz, sz) && pz > sz) b = m; else a = m;
            }
            yFront = a;
            found = true;
            break;
        }
        yFront = y;
        front = true;
        y -= step_;
    }
    vec3 p = vec3(xz.x, yFront, xz.y);
    float z;
    // Really on the surface there: the depth at its pixel is its own (not something nearer in front of it).
    if (!found || !Probe(p, pz, sz) || abs(sz - pz) > 0.15 + 0.02 * pz || pz > 2.0 * kRange) {
        hide();
        return;
    }
    vec4 pc = R.viewProj * vec4(p, 1.0);
    ivec2 px = ivec2((pc.x / pc.w * 0.5 + 0.5) * R.viewport.x, (0.5 - pc.y / pc.w * 0.5) * R.viewport.y);
    SurfaceAt(px, z);
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
    vAlpha = 0.6 * min(R.time.z * 1.5, 1.0) * mix(0.3, 1.0, fog) * (1.0 - RainSheltered(p + vec3(0.0, 0.4, 0.0)));   // (none under a roof)
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
