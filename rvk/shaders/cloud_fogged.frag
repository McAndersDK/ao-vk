#version 450
// Clouds over what the game fogs (rvk/clouds.cpp ComposeCloudsOverFog): after the scene's draws, the half-size clouds
// filtered up over the surfaces the fog has taken, as much as it has. The game blends a fogged surface towards the
// fog's colour - the colour of its sky low down, so far scenery and the ships high over the cities fade into the sky;
// with the clouds over the sky, that blend has to go towards them, or fogged shapes stand out flat against the clouds.
layout(set = 0, binding = 0) uniform sampler2D halfClouds;   // premultiplied
layout(set = 0, binding = 1) uniform sampler2D sceneDepth;

layout(push_constant) uniform Params {
    vec4 size;          // xy: the scene's width, height; z: the depth's clear value; w: the fog's mode (D3DFOG_*)
    vec4 fog;           // start, end, density; w: the depth from which any of this pass applies
    vec4 proj;          // the scene's projection: m22, m32, m23, m33 (a depth back to a view distance)
} P;

layout(location = 0) out vec4 outScene;
layout(location = 1) out vec4 outGlow;
layout(location = 2) out vec4 outLocal;
layout(location = 3) out vec4 outMotion;
layout(location = 4) out vec4 outAlbedo;

void main()
{
    float d = texelFetch(sceneDepth, ivec2(gl_FragCoord.xy), 0).r;
    if (d == P.size.z || d < P.fog.w)            // the sky (its clouds are there already), or too near to be fogged much
        discard;
    float dist = (P.proj.y - d * P.proj.w) / max(d * P.proj.z - P.proj.x, 1e-6);
    uint mode = uint(P.size.w);
    float left = 1.0;                            // (as the game's fog: how much of the surface's own colour is left)
    if (mode == 3u) left = clamp((P.fog.y - dist) / max(P.fog.y - P.fog.x, 1e-3), 0.0, 1.0);
    else if (mode == 1u) left = exp(-P.fog.z * dist);
    else if (mode == 2u) { float x = P.fog.z * dist; left = exp(-x * x); }
    // (From a quarter fogged up: the march only reaches that far.)
    float amount = smoothstep(0.25, 1.0, 1.0 - left);
    if (amount <= 0.003)
        discard;
    outGlow = outMotion = outAlbedo = vec4(0.0);
    vec4 c = texture(halfClouds, gl_FragCoord.xy / P.size.xy) * amount;
    outScene = c;
    outLocal = vec4(c.a, 0.0, 0.0, c.a);
}
