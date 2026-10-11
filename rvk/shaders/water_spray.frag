#version 450
// The spray and mist at the falls' feet (water_spray.vert): soft round particles blended over the scene's colour
// (premultiplied alpha), faded where they meet what is behind them (the scene's depth before the water) and the pool's
// surface. Lit by the ambient light, the sun - far brighter looking towards it through the mist (it scatters forwards)
// - and the lamps near.
#include "water_common.glsl"

layout(set = 0, binding = 10) uniform sampler2D detail;   // B: noise (the mist's billows)

layout(location = 0) in vec2 vCorner;
layout(location = 1) in vec3 vPos;
layout(location = 2) in float vAlpha;
layout(location = 3) in float vMist;
layout(location = 4) in float vFootY;
layout(location = 5) in vec2 vSeed;

layout(location = 0) out vec4 outScene;
layout(location = 1) out vec4 outGlow;      // (the other targets blend as the scene's colour does: 0 leaves them be)
layout(location = 2) out vec4 outLocal;
layout(location = 3) out vec4 outMotion;
layout(location = 4) out vec4 outAlbedo;

void main()
{
    float r2 = dot(vCorner, vCorner);
    if (r2 >= 1.0 || vAlpha <= 0.0)
        discard;
    bool mist = vMist > 0.5;
    float t = W.time.x;
    float shape = mist ? (1.0 - r2) * (1.0 - r2) * (0.4 + 0.9 * texture(detail, vSeed + vCorner * 0.22 + vec2(t * 0.015, 0.0)).b)
                       : 1.0 - smoothstep(0.35, 1.0, sqrt(r2));
    // Soft against what is behind (rock, the cliff) and the pool's surface.
    float sd = texelFetch(sceneDepth, ivec2(gl_FragCoord.xy), 0).r;
    float zScene = sd >= 1.0 ? 1e5 : ViewZ(sd);
    float zHere = (W.view * vec4(vPos, 1.0)).z;
    float soft = clamp((zScene - zHere) / (mist ? 1.5 : 0.2), 0.0, 1.0);
    float above = clamp((vPos.y - vFootY + 0.1) / (mist ? 0.8 : 0.15), 0.0, 1.0);
    float a = clamp(vAlpha * shape * soft * above, 0.0, 1.0);
    if (a <= 1e-3)
        discard;

    vec3 L = -normalize(FL.sunDir.xyz + vec3(0.0, -1e-6, 0.0));
    vec3 V = normalize(W.camera.xyz - vPos);
    float forward = pow(max(dot(-V, L), 0.0), 4.0);
    vec3 light = W.ambient.rgb * 1.1 + FL.sunColor.rgb * (0.35 + 1.5 * forward);
    uint count = min(FL.info.x, 64u);
    for (uint i = 0u; i < count; ++i) {
        vec3 Ll, lc;
        if (LocalLight(i, vPos, Ll, lc))
            light += lc * 0.6;
    }
    vec3 colour = vec3(0.9, 0.93, 0.95) * light;
    colour = mix(W.fogColour.rgb, colour, W.fogColour.w > 0.5 ? FogFactor(length(W.camera.xyz - vPos)) : 1.0);
    outScene = vec4(colour * a, a);
    outGlow = outLocal = outMotion = outAlbedo = vec4(0.0);
}
