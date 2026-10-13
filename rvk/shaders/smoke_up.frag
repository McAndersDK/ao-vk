#version 450
// Smoke at half size (rvk/smoke.cpp RenderSmoke, RVK_SmokeHalf): over the scene's targets as smoke.frag would have
// drawn it - each pixel from the four half-size pixels round it, bilinear weights times how near each one's marched
// depth is to this pixel's (relative), so smoke in front of an edge doesn't bleed over what is in front of it.
layout(set = 0, binding = 0) uniform sampler2D halfSmoke;  // premultiplied
layout(set = 0, binding = 1) uniform sampler2D halfDepth;  // R: the view depth each was marched to
layout(set = 0, binding = 2) uniform sampler2D sceneDepth;

layout(push_constant) uniform Params {
    vec4 proj;          // D3D projection m[2][2], m[3][2]; the half-size target's width, height
} P;

layout(location = 0) out vec4 outScene;
layout(location = 1) out vec4 outGlow;
layout(location = 2) out vec4 outLocal;
layout(location = 3) out vec4 outMotion;
layout(location = 4) out vec4 outAlbedo;

void main()
{
    outGlow = outMotion = outAlbedo = vec4(0.0);
    float d = texelFetch(sceneDepth, ivec2(gl_FragCoord.xy), 0).r;
    float z = d < 1.0 ? P.proj.y / (min(d, 0.9999999) - P.proj.x) : 60000.0;
    vec2 at = gl_FragCoord.xy * 0.5 - 0.5;
    ivec2 base = ivec2(floor(at));
    vec2 f = at - vec2(base);
    ivec2 last = ivec2(P.proj.zw) - 1;
    vec4 sum = vec4(0.0);
    float weights = 0.0;
    for (int k = 0; k < 4; ++k) {
        ivec2 o = ivec2(k & 1, k >> 1);
        ivec2 t = clamp(base + o, ivec2(0), last);
        float bilinear = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y);
        float tz = texelFetch(halfDepth, t, 0).r;
        float w = bilinear / (1e-3 + abs(tz - z) / max(z, 1e-3)) + 1e-6;
        sum += w * texelFetch(halfSmoke, t, 0);
        weights += w;
    }
    vec4 smoke = sum / weights;
    outScene = smoke;
    outLocal = vec4(smoke.a, 0.0, 0.0, smoke.a);   // (smoke.frag's local fraction)
}
