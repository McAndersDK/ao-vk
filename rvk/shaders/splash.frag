#version 450
// A splash (splash.vert): a crown of water thrown up where a drop lands - a thin sheet rising round the impact and
// falling back, and a few droplets flung out and up on arcs - fading as it ends. Hidden where the surface is in front
// of it (its lower part, under the ground or the water). Premultiplied over the scene, as the streaks.
#include "rain_common.glsl"

layout(set = 0, binding = 1) uniform sampler2D sceneDepth;

layout(location = 0) in vec2 vCorner;
layout(location = 1) in vec3 vLight;
layout(location = 2) in float vAlpha;
layout(location = 3) in float vAge;
layout(location = 4) in vec2 vSeed;
layout(location = 5) in float vViewZ;
layout(location = 6) in vec4 vClip;
layout(location = 7) in vec4 vPrevClip;

layout(location = 0) out vec4 outScene;
layout(location = 1) out vec4 outGlow;
layout(location = 2) out vec4 outLocal;
layout(location = 3) out vec4 outMotion;
layout(location = 4) out vec4 outAlbedo;

void main()
{
    if (vAlpha <= 0.0)
        discard;
    float sd = texelFetch(sceneDepth, ivec2(gl_FragCoord.xy), 0).r;
    if (sd < 1.0 && vViewZ > RainViewZ(sd) + 0.02)
        discard;                                 // behind the surface (under it)
    float a = vAge;
    vec2 p = vCorner;
    // The crown: a thin wall rising round the impact (seen side-on: two edges spreading apart), up and back down.
    float spread = 0.15 + 0.75 * sqrt(a);
    float height = 0.4 * sin(3.14159 * min(a * 1.6, 1.0));
    float wall = exp(-pow((abs(p.x) - spread) / 0.09, 2.0)) * step(0.0, p.y) * (1.0 - smoothstep(height * 0.6, height, p.y));
    // The droplets: flung out and up, falling back.
    float drops = 0.0;
    for (int k = 0; k < 4; ++k) {
        float h1 = fract(vSeed.x * 7.13 + float(k) * 0.371), h2 = fract(vSeed.y * 5.71 + float(k) * 0.619);
        float side = (float(k & 1) * 2.0 - 1.0) * (0.35 + 0.5 * h1);
        float up = 2.2 + 1.6 * h2;
        vec2 at = vec2(side * a * 1.2, up * a * (1.0 - a));
        drops = max(drops, 1.0 - smoothstep(0.05, 0.11, length((p - at) * vec2(1.0, 0.8))));
    }
    float shape = max(wall * 0.4, drops) * (1.0 - a) * (1.0 - a);
    float alpha = clamp(vAlpha * shape, 0.0, 1.0);
    if (alpha <= 1e-3)
        discard;
    outScene = vec4(vLight * alpha, alpha);
    vec2 now = vClip.xy / vClip.w, before = vPrevClip.xy / max(vPrevClip.w, 1e-6);
    vec2 motion = vPrevClip.w > 1e-6 ? (now - before) * 0.5 * R.viewport.xy * vec2(1.0, -1.0) : vec2(0.0);
    outMotion = vec4(motion * alpha, 0.0, alpha);
    outGlow = outLocal = outAlbedo = vec4(0.0);
}
