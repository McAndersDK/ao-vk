#version 450
// The rain (rain.vert): a streak, soft across and fading towards its tail, premultiplied over the scene; the motion
// target gets the drop's motion (premultiplied too: the temporal filter follows the drops), the others nothing.
#include "rain_common.glsl"

layout(location = 0) in vec2 vCorner;
layout(location = 1) in vec3 vLight;
layout(location = 2) in float vAlpha;
layout(location = 3) in vec4 vClip;
layout(location = 4) in vec4 vPrevClip;

layout(location = 0) out vec4 outScene;
layout(location = 1) out vec4 outGlow;
layout(location = 2) out vec4 outLocal;
layout(location = 3) out vec4 outMotion;
layout(location = 4) out vec4 outAlbedo;

void main()
{
    float a = vAlpha * (1.0 - smoothstep(0.35, 1.0, abs(vCorner.y))) * smoothstep(0.0, 0.6, vCorner.x);
    if (a <= 1e-3)
        discard;
    outScene = vec4(vLight * a, a);
    vec2 now = vClip.xy / vClip.w, before = vPrevClip.xy / max(vPrevClip.w, 1e-6);
    vec2 motion = vPrevClip.w > 1e-6 ? (now - before) * 0.5 * R.viewport.xy * vec2(1.0, -1.0) : vec2(0.0);
    outMotion = vec4(motion * a, 0.0, a);
    outGlow = outLocal = outAlbedo = vec4(0.0);
}
