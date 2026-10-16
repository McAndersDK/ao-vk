#version 450
// Clouds at half size (rvk/clouds.cpp DrawClouds, RVK_CloudHalf): over the scene's targets as cloud.frag would have
// drawn them, filtered up. (Only the sky is there yet - no depth to keep them off anything.) A lightning flash's light
// in the cloud layer added here, not in its march: the layer's history then stays (cloud_resolve.frag) - its noise
// averaged out through the flash - and the flash is there the frame it strikes (user: the clouds fuzzy at each strike).
layout(set = 0, binding = 0) uniform sampler2D halfClouds;   // premultiplied
layout(set = 0, binding = 1, std140) uniform UpFrame {
    mat4 invViewProj;   // clip -> world (the layer's camera, as its march's)
    vec4 eye;           // xyz: the eye (world); w: the layer's base (world y)
    vec4 tint;          // rgb: the layer's cloud colour; w: its haze's distance (world)
    vec4 flash;         // rgb: the flash's light in the cloud (0: none); w: the march's reach
    vec4 flashWorld;    // xyz: the strike (world)
} U;

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
    if (U.flash.r + U.flash.g + U.flash.b > 0.0 && c.a > 0.001) {
        // Where the cloud is: where the ray meets the layer's base (a storm's deck is seen by its underside) - lit by
        // the strike by how far across from it, as the march lit it, through as much of the haze as it was.
        vec2 ndc = vec2(gl_FragCoord.x / P.size.x * 2.0 - 1.0, 1.0 - gl_FragCoord.y / P.size.y * 2.0);
        vec4 a = U.invViewProj * vec4(ndc, 0.0, 1.0), b = U.invViewProj * vec4(ndc, 1.0, 1.0);
        vec3 dir = normalize(b.xyz / b.w - a.xyz / a.w);
        float t = abs(dir.y) > 1e-3 ? (U.eye.w - U.eye.y) / dir.y : -1.0;
        t = t > 0.0 ? min(t, U.flash.w) : 3000.0;
        vec2 off = U.eye.xz + dir.xz * t - U.flashWorld.xz;
        vec3 light = U.flash.rgb / (1.0 + dot(off, off) / (1500.0 * 1500.0));
        c.rgb += c.a * light * U.tint.rgb * exp(-t / max(U.tint.w, 1.0));
    }
    outScene = c;
    outLocal = vec4(c.a, 0.0, 0.0, c.a);       // (cloud.frag's local fraction)
}
