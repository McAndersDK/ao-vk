#version 450
// Smoke (smoke.vert): the view ray through the box, from where it enters (or the camera) to where it leaves or meets
// the scene, marched through the simulated density: each step's smoke takes light away from what is behind and adds
// its own - the ambient light, the sun (as far as the smoke towards it lets it through: the simulation's light
// image; scattered forwards most, so smoke glows looking towards the sun) and the lamps near. Premultiplied over the
// scene; the other targets get nothing (they blend alike: 0 leaves them be).
#include "smoke_common.glsl"

layout(set = 0, binding = 1) uniform sampler3D density;     // R density
layout(set = 0, binding = 2) uniform sampler3D sunLight;    // R the sunlight reaching the cell (0..1)
layout(set = 0, binding = 3) uniform sampler2D sceneDepth;
layout(set = 0, binding = 5) uniform sampler3D velocity;    // xyz the flow (world / s)
layout(set = 0, binding = 6) uniform sampler3D wispNoise;   // R tiling noise, 8 lattice cells across (smoke_sim.comp 10)

layout(location = 0) in vec3 vWorld;

layout(location = 0) out vec4 outScene;
layout(location = 1) out vec4 outGlow;
layout(location = 2) out vec4 outLocal;
layout(location = 3) out vec4 outMotion;
layout(location = 4) out vec4 outAlbedo;

const float kPi = 3.14159265;

float Wisps(vec3 q) { return texture(wispNoise, q * 0.125).r; }

// The wisps at a point: noise carried along by the flow there, for one cycle, then started again - two cycles half a
// cycle apart, each faded out as it restarts, so nothing jumps (a flow map).
float Detail(vec3 p, vec3 vel)
{
    float cycle = S.detail.y, time = FL.wind.z / cycle + S.detail.w;
    float ph1 = fract(time), ph2 = fract(time + 0.5);
    vec3 q = p * S.detail.z, drift = vel * (cycle * S.detail.z);
    float n1 = Wisps(q - drift * ph1), n2 = Wisps(q - drift * ph2 + 31.7);
    return mix(n1, n2, abs(1.0 - 2.0 * ph1));
}

float Fog(float d)
{
    if (S.fogColour.w < 0.5)
        return 1.0;
    uint mode = uint(S.fogParams.w);
    if (mode == 3u) return clamp((S.fogParams.y - d) / max(S.fogParams.y - S.fogParams.x, 1e-3), 0.0, 1.0);
    if (mode == 1u) return clamp(exp(-S.fogParams.z * d), 0.0, 1.0);
    if (mode == 2u) { float x = S.fogParams.z * d; return clamp(exp(-x * x), 0.0, 1.0); }
    return 1.0;
}

void main()
{
    outGlow = outLocal = outMotion = outAlbedo = vec4(0.0);
    vec3 eye = S.camera.xyz;
    vec3 dir = normalize(vWorld - eye);
    // The far faces only (outward normal away from the camera): the face this point is on is the nearest wall.
    vec3 f = (vWorld - S.box.xyz) / S.box.w;
    vec3 near0 = f, near1 = 1.0 - f;
    vec3 n = vec3(0.0);
    float best = 1e9;
    for (int k = 0; k < 3; ++k) {
        if (near0[k] < best) { best = near0[k]; n = vec3(0.0); n[k] = -1.0; }
        if (near1[k] < best) { best = near1[k]; n = vec3(0.0); n[k] = 1.0; }
    }
    if (dot(n, dir) <= 0.0)
        discard;
    // Where the ray is in the box.
    vec3 inv = 1.0 / mix(dir, vec3(1e-6), lessThan(abs(dir), vec3(1e-6)));
    vec3 t0 = (S.box.xyz - eye) * inv, t1 = (S.box.xyz + S.box.w - eye) * inv;
    vec3 tmin = min(t0, t1), tmax = max(t0, t1);
    float enter = max(max(max(tmin.x, tmin.y), tmin.z), 0.0);
    float leave = min(min(tmax.x, tmax.y), tmax.z);
    // ... and no further than the scene there.
    float sd = texelFetch(sceneDepth, ivec2(gl_FragCoord.xy), 0).r;
    if (sd < 1.0) {
        vec3 forward = vec3(S.view[0][2], S.view[1][2], S.view[2][2]);
        leave = min(leave, SmokeViewZ(sd) / max(dot(dir, forward), 1e-3));
    }
    if (leave <= enter) {
        outScene = vec4(0.0);
        return;
    }
    int steps = int(S.ambient.w);
    float ds = (leave - enter) / float(steps);
    float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))) + FL.taa.z);
    vec3 L = -normalize(FL.sunDir.xyz + vec3(0.0, -1e-6, 0.0));
    float g = 0.35, cosT = dot(dir, L);
    float phase = (1.0 - g * g) / (4.0 * kPi * pow(1.0 + g * g - 2.0 * g * cosT, 1.5)) * 4.0 * kPi;   // 1 = even
    vec3 sun = FL.sunColor.rgb * mix(0.35, 1.0, phase);
    float transmit = 1.0;
    vec3 light = vec3(0.0);
    uint lamps = min(FL.info.x, 64u);
    for (int i = 0; i < steps && transmit > 0.01; ++i) {
        vec3 p = eye + dir * (enter + (float(i) + noise) * ds);
        vec3 uvw = (p - S.box.xyz) / S.box.w;
        float dens = texture(density, uvw).r;
        if (dens < 1e-4)
            continue;
        // Wisps: thin smoke frays right through, thick keeps more of its body (the average kept: 2 x the noise's 0.5).
        float fray = S.detail.x * mix(0.55, 1.0, 1.0 - smoothstep(0.0, 0.6, dens));
        if (fray > 0.0)
            dens *= mix(1.0, 2.0 * smoothstep(0.3, 0.7, Detail(p, texture(velocity, uvw).xyz)), fray);
        float a = 1.0 - exp(-dens * S.colour.w * ds);
        // The sky's light, too, less inside the smoke (as far in as the sun's is shadowed, about).
        float sunIn = texture(sunLight, uvw).r;
        vec3 lit = S.ambient.rgb * mix(0.45, 1.0, sunIn) + sun * sunIn;
        for (uint k = 0u; k < lamps; ++k) {
            Light li = FL.lights[k];
            uint type = uint(li.position.w + 0.5);
            if (type != 1u && type != 2u)
                continue;
            float d = distance(li.position.xyz, p);
            if (d >= li.direction.w)
                continue;
            float att = 1.0 / max(li.atten.x + li.atten.y * d + li.atten.z * d * d, 1e-3);
            lit += li.diffuse.rgb * (att * (1.0 - smoothstep(0.75, 1.0, d / li.direction.w)));
        }
        light += transmit * a * lit * S.colour.rgb;
        transmit *= 1.0 - a;
    }
    float alpha = 1.0 - transmit;
    float fog = Fog(enter + 0.5 * (leave - enter));
    light = mix(S.fogColour.rgb * alpha, light, fog);
    outScene = vec4(light, alpha);
}
