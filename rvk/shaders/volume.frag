#version 450
// Volumetric light (hdr.cpp), at half resolution: the sunlight the air between the camera and each surface scatters
// towards the camera, marched along the ray through the sun shadow cascades, so shadowed air stays dark and light
// coming through gaps forms shafts, brightest looking towards the sun (forward scattering). No lamp glow: the game's
// point lights sit away from their lamps (half a metre under them), so a glow around them is misplaced; the bloom
// gives lamps their halo. Output: the scattered light and view depth, for the depth-aware blur and upsampling.
#include "frame_lights.glsl"
layout(set = 0, binding = 0) uniform sampler2D depthTex;
layout(set = 0, binding = 1) uniform sampler2DArrayShadow shadowMap;
layout(set = 0, binding = 2) uniform sampler2D cloudShadow;   // the clouds' (rvk clouds.cpp; FL.clouds: where, if any)
layout(push_constant) uniform Push {
    mat4 invViewProj;   // the world camera's clip -> world (raw D3DMATRIX memory)
    vec4 eye;           // camera position; w = longest ray (world units)
    vec4 params;        // haze density, sun strength, this frame's noise offset, D3D projection m[2][2]
    vec4 size;          // full target width, height, steps, D3D projection m[3][2]
    vec4 shafts;        // x: contrast of the shafts (power on the ray's lit fraction - 1); y: the rays through the
                        // clouds' gaps, x (0: none); z: their reach (world)
} P;
layout(location = 0) out vec4 outVolume;

// The ray in each cascade's coordinates: the cascades are orthographic, so a point t along the ray is at
// gRayStart[c] + gRayDir[c] * t - two transforms per cascade for the whole ray instead of one per step.
vec3 gRayStart[4], gRayDir[4];

// Sunlit (1) or in shadow (0) at distance t along the ray: one comparison tap in the sharpest cascade holding it.
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

// The sunlight through the clouds (light shafts through the gaps between them).
#include "cloud_shadow_common.glsl"

void main()
{
    vec2 pix = floor(gl_FragCoord.xy) * 2.0;
    float d = texelFetch(depthTex, ivec2(pix), 0).r;
    float dc = min(d, 0.999999);
    vec2 ndc = vec2((pix.x + 0.5) / P.size.x * 2.0 - 1.0, 1.0 - (pix.y + 0.5) / P.size.y * 2.0);
    vec4 w = P.invViewProj * vec4(ndc, dc, 1.0);
    vec3 toPoint = w.xyz / w.w - P.eye.xyz;
    float dist = length(toPoint);
    vec3 dir = toPoint / max(dist, 1e-4);
    float len = min(dist, P.eye.w);
    float viewZ = P.size.w / (dc - P.params.w);
    vec3 light = vec3(0.0);

    // Sun: march with a per-pixel offset that changes every frame (the blur and the temporal anti-aliasing average
    // it), the steps closer together near the camera, where shafts past nearby objects are big on screen:
    // t = len * u^2, each step weighted by its length. Air farther away counts less (extinction): shafts near the
    // camera stand out against the evenly lit air beyond instead of being averaged into it.
    if (FL.shadowParams.x > 0.5 && P.params.y > 0.0 && dot(FL.sunColor.rgb, FL.sunColor.rgb) > 0.0) {
        int steps = int(P.size.z);
        float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))) + P.params.z);
        const float kExtinction = 0.012;         // per world unit
        const float kHazeHeight = 15.0;           // world units
        for (int c = 0; c < int(FL.shadowParams.z); ++c) {
            gRayStart[c] = (FL.shadowViewProj[c] * vec4(P.eye.xyz, 1.0)).xyz;
            gRayDir[c] = (FL.shadowViewProj[c] * vec4(dir, 0.0)).xyz;
        }
        // (Things' shadows fade out as the sun nears the horizon, as on the ground - rvk draw.cpp: full from ~10 degrees
        // up, none at ~3. Unfaded, shafts of shadows no longer seen came from invisible things - user, 2026-10-13.)
        float casterFade = smoothstep(0.05, 0.17, -normalize(FL.sunDir.xyz).y);
        float lit = 0.0, open = 0.0;
        for (int i = 0; i < steps; ++i) {
            float u = (float(i) + noise) / float(steps), t = len * u * u;
            vec3 q = P.eye.xyz + dir * t;
            // Haze thins with height above a couple of units under the camera (scale height kHazeHeight): rays into
            // the sky leave it soon (the sky isn't washed out), air near the ground - where shadows fall - keeps it.
            float density = exp(-max(q.y - (P.eye.y - 2.0), 0.0) / kHazeHeight);
            float w = (2.0 * u) * exp(-kExtinction * t) * density;
            lit += mix(1.0, SunVisible(t), casterFade) * CloudShadowMap(q) * w;
            open += w;
        }
        // Shafts are contrast: a ray partly in shadow is darkened by a power of its lit fraction, a fully lit one
        // keeps its brightness - beams through gaps stand out from the shadowed air around them.
        float fraction = open > 1e-6 ? lit / open : 0.0;
        lit = open * pow(fraction, 1.0 + P.shafts.x);
        float stepLen = len / float(steps);
        // Henyey-Greenstein phase (g = 0.6: forward), scaled so light scattered evenly would be 1; its peak capped,
        // so looking straight into the sun glows without washing out.
        const float g = 0.6;
        float cosAngle = dot(dir, -normalize(FL.sunDir.xyz));
        float phase = min((1.0 - g * g) / pow(1.0 + g * g - 2.0 * g * cosAngle, 1.5), 4.0);
        light += FL.sunColor.rgb * (lit * stepLen * P.params.x * P.params.y * phase);
    }

    // Rays through the gaps in the clouds: the air out to kilometres (thin, thinning with height up to the clouds'
    // shadow plane), lit where the clouds let the sun through (their shadow map) - beams fanning from the gaps,
    // brightest looking towards the sun. A few long steps: the clouds' shadows are kilometres across.
    if (P.shafts.y > 0.0 && floatBitsToUint(FL.clouds.w) != 0u && dot(FL.sunColor.rgb, FL.sunColor.rgb) > 0.0) {
        const int kSteps = 16;
        float reach = min(d >= 1.0 ? P.shafts.z : dist, P.shafts.z);
        float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.0583715, 0.06711056))) + P.params.z);
        float lit = 0.0, open = 0.0, stepLen = reach / float(kSteps);
        for (int i = 0; i < kSteps; ++i) {
            float t = (float(i) + noise) * stepLen;
            vec3 q = P.eye.xyz + dir * t;
            if (q.y >= FL.clouds2.x) break;      // (into the clouds)
            float w = exp(-max(q.y - P.eye.y, 0.0) / 500.0) * exp(-t / 2500.0);
            lit += CloudShadowMap(q) * w;
            open += w;
        }
        if (open > 1e-6) {
            // Contrast as the near shafts': the shadowed air dark, the beams through the gaps standing out - and only
            // where the ray crosses both lit and shaded air (4 f (1 - f)): air lit all along is the haze (tonemap.frag
            // Air) already. With the clouds' shadows mostly lit (finer patches) the old open * f^4 lit kilometres of
            // air towards the sun: the morning washed out (user).
            float fraction = lit / open;
            float beams = open * fraction * fraction * 4.0 * fraction * (1.0 - fraction);
            const float g = 0.7;
            float cosAngle = dot(dir, -normalize(FL.sunDir.xyz));
            float phase = min((1.0 - g * g) / pow(1.0 + g * g - 2.0 * g * cosAngle, 1.5), 6.0);
            light += FL.sunColor.rgb * (beams * stepLen * 0.0001 * P.shafts.y * phase);
        }
    }

    outVolume = vec4(light, viewZ);
}
