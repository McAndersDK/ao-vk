#version 450
// Clouds over what the game fogs away (rvk/clouds.cpp ComposeCloudsOverFog): after the scene's draws, the half-size
// clouds filtered up over the pixels whose surface lies past the fog's end. The game draws those wholly in the fog's
// colour - the colour of its sky low down, so they vanish into it (far scenery, the ships high over the cities); with
// the clouds over the sky they would stand out as flat fog-coloured shapes. Covered by the clouds, they are sky again.
layout(set = 0, binding = 0) uniform sampler2D halfClouds;   // premultiplied
layout(set = 0, binding = 1) uniform sampler2D sceneDepth;

layout(push_constant) uniform Params {
    vec4 size;          // xy: the scene's width, height; z: the fog's end as a depth; w: the depth's clear value
} P;

layout(location = 0) out vec4 outScene;
layout(location = 1) out vec4 outGlow;
layout(location = 2) out vec4 outLocal;
layout(location = 3) out vec4 outMotion;
layout(location = 4) out vec4 outAlbedo;

void main()
{
    float d = texelFetch(sceneDepth, ivec2(gl_FragCoord.xy), 0).r;
    if (d == P.size.w || d < P.size.z)           // the sky (its clouds are there already), or nearer than the fog's end
        discard;
    outGlow = outMotion = outAlbedo = vec4(0.0);
    vec4 c = texture(halfClouds, gl_FragCoord.xy / P.size.xy);
    outScene = c;
    outLocal = vec4(c.a, 0.0, 0.0, c.a);
}
