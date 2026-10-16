#version 450
// Clouds at half size (rvk/clouds.cpp DrawClouds, RVK_CloudHalf): over the scene's targets as cloud.frag would have
// drawn them, filtered up. (Only the sky is there yet - no depth to keep them off anything.)
layout(set = 0, binding = 0) uniform sampler2D halfClouds;   // premultiplied

layout(push_constant) uniform Params {
    vec4 size;          // the scene's width, height
} P;

layout(location = 0) out vec4 outScene;
layout(location = 1) out vec4 outGlow;
layout(location = 2) out vec4 outLocal;
layout(location = 3) out vec4 outMotion;
layout(location = 4) out vec4 outAlbedo;

void main()
{
    outGlow = outMotion = outAlbedo = vec4(0.0);
    vec4 c = texture(halfClouds, gl_FragCoord.xy / P.size.xy);
    outScene = c;
    outLocal = vec4(c.a, 0.0, 0.0, c.a);       // (cloud.frag's local fraction)
}
