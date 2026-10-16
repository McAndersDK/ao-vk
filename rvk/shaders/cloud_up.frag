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
    // (Bilinear over the known texels only: the reused cloud layer marks a covered one unknown, alpha -1 - none.)
    vec2 hs = vec2(textureSize(halfClouds, 0));
    vec2 at = gl_FragCoord.xy / P.size.xy * hs - 0.5;
    ivec2 base = ivec2(floor(at)), hl = ivec2(hs) - 1;
    vec2 f = at - vec2(base);
    vec4 acc = vec4(0.0);
    float wsum = 0.0;
    for (int k = 0; k < 4; ++k) {
        ivec2 o = ivec2(k & 1, k >> 1);
        vec4 t = texelFetch(halfClouds, clamp(base + o, ivec2(0), hl), 0);
        float w = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y);
        if (t.a >= 0.0) { acc += t * w; wsum += w; }
    }
    vec4 c = wsum > 1e-4 ? acc / wsum : vec4(0.0);
    outScene = c;
    outLocal = vec4(c.a, 0.0, 0.0, c.a);       // (cloud.frag's local fraction)
}
