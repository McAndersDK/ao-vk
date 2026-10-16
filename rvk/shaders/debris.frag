#version 450
// A storm's blown debris (debris.vert): an opaque bit - a leaf's oval, a twig's or a straw's thin stick - written as
// a surface is (depth: the haze and the storm's fog veil it by its own distance), its motion for the temporal filter.
#include "rain_common.glsl"

layout(location = 0) in vec2 vCorner;
layout(location = 1) in vec3 vColour;
layout(location = 2) in float vShape;
layout(location = 3) in vec4 vClip;
layout(location = 4) in vec4 vPrevClip;

layout(location = 0) out vec4 outScene;
layout(location = 1) out vec4 outGlow;
layout(location = 2) out vec4 outLocal;
layout(location = 3) out vec4 outMotion;
layout(location = 4) out vec4 outAlbedo;

void main()
{
    vec2 c = vCorner;
    float inside = vShape > 0.5 ? 1.0 - abs(c.y) * 0.2 : 1.0 - dot(c * vec2(1.0, 1.0), c);
    if (inside <= 0.0)
        discard;
    outScene = vec4(vColour, 1.0);
    vec2 now = vClip.xy / vClip.w, before = vPrevClip.xy / max(vPrevClip.w, 1e-6);
    vec2 motion = vPrevClip.w > 1e-6 ? (now - before) * 0.5 * R.viewport.xy * vec2(1.0, -1.0) : vec2(0.0);
    outMotion = vec4(motion, 0.0, 1.0);
    outGlow = vec4(0.0, 0.0, 0.0, 1.0);
    outLocal = vec4(0.0, 0.0, 0.0, 1.0);
    outAlbedo = vec4(vColour, 1.0);
}
