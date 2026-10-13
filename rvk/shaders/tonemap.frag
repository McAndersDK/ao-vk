#version 450
// HDR scene -> 8-bit main target (hdr.cpp). Colours up to the knee pass unchanged, so the game's own look is kept;
// brighter ones roll off smoothly towards 1 by their brightest channel, which keeps their hue, and lose saturation
// the more they are overexposed, as bright light does.
#include "occlusion.glsl"
layout(location = 0) out vec4 outColor;

// Colour grading of the tone mapped colour (0..1): white balance, a cooler, paler night, contrast (in a perceptual
// square-root space, around its middle), saturation, the day / night lookup tables, vignette. far: 0 near .. 1 at
// the far clip and the sky - the warmth is for what's near; the distance and the sky lean cool, as the air makes
// them (a warmed teal sky turned green-grey: user).
vec3 Grade(vec3 c, float far)
{
    const vec3 kLuma = vec3(0.3, 0.59, 0.11);
    float warmth = P.grade.z * mix(1.0, -0.5, smoothstep(0.3, 1.0, far));
    c *= vec3(1.0 + 0.12 * warmth, 1.0 + 0.02 * warmth, 1.0 - 0.12 * warmth);
    float nightTint = P.grade2.x * P.grade2.y;
    c = mix(c, dot(c, kLuma) * vec3(0.78, 0.9, 1.18), 0.6 * nightTint);
    vec3 s = sqrt(max(c, vec3(0.0)));
    s = (s - 0.5) * P.grade.y + 0.5;
    c = max(s, vec3(0.0)) * max(s, vec3(0.0));
    c = mix(vec3(dot(c, kLuma)), c, P.grade.x);
    c = clamp(c, 0.0, 1.0);
    if (P.grade.w > 0.0) {
        vec3 size = vec3(textureSize(lutDay, 0));
        vec3 day = texture(lutDay, c * (size - 1.0) / size + 0.5 / size).rgb;
        size = vec3(textureSize(lutNight, 0));
        vec3 night = texture(lutNight, c * (size - 1.0) / size + 0.5 / size).rgb;
        c = mix(c, mix(day, night, P.grade2.x), P.grade.w);
    }
    if (P.grade2.z > 0.0) {
        vec2 uv = gl_FragCoord.xy / vec2(textureSize(scene, 0)) - 0.5;
        c *= 1.0 - P.grade2.z * smoothstep(0.35, 0.9, length(uv * vec2(1.0, 0.75)) * 1.4);
    }
    return c;
}

void main()
{
    vec4 s = texelFetch(scene, ivec2(gl_FragCoord.xy), 0);
    vec3 glow = P.params.z > 0.0 ? Bloom() * P.params.z : vec3(0.0);
    vec3 c = Reflected(s.rgb * AmbientFactor() + Indirect());
    // How far the pixel is: its view depth over the far clip's (the sky: wholly).
    float d = texelFetch(depthTex, ivec2(gl_FragCoord.xy), 0).r;
    float far = 1.0;
    if ((int(P.proj.w) & 16) == 0) {
        far = 0.0;                               // (no depth readable: warmth everywhere)
    } else if (d < 1.0) {
        // (Over the far clip - the game's fog end; with no far clip to speak of, over 3 km.)
        // (D3D: m[2][2] = f / (f - n) > 1, m[3][2] = -n m[2][2] < 0 - both terms of the view depth negative.)
        float z = P.proj.y / min(d - P.proj.x, -1e-9);
        float zFar = P.proj.x > 1.000001 ? P.proj.y / (1.0 - P.proj.x) : 3000.0;
        far = clamp(z / zFar, 0.0, 1.0);
        // The haze over the distant land (RVK_VolLand): the air between the eye and the land, thickening from the
        // eye out - each ridge farther paler and bluer - and giving way to the game's own fog where that starts
        // (its fog end the far land's colour: no seam with the sky).
        if (P.haze.w > 0.0 && P.fog.y > P.fog.x) {
            float air = 1.0 - exp(-z / (0.4 * P.fog.y));
            float gameFog = clamp((z - P.fog.x) / (P.fog.y - P.fog.x), 0.0, 1.0);
            c = mix(c, P.haze.rgb, P.haze.w * air * (1.0 - gameFog));
        }
    }
    c = max((c + Volumetric() + glow) * P.params.y, vec3(0.0));
    // Night (RVK_NightDark x how much it is night): darker - the darks and middle tones down, the sky more; what is
    // bright kept (lamps, windows, the moon, stars, moonlit cloud edges), so it stands out of the dark.
    float nightDark = P.grade2.x * P.grade2.w;
    if (nightDark > 0.0) {
        float bright = smoothstep(0.35, 0.9, dot(c, vec3(0.3, 0.59, 0.11)));
        float down = exp2(-0.7 * nightDark) * mix(1.0, 0.8, smoothstep(0.7, 1.0, far) * min(nightDark, 1.0));
        c *= mix(down, 1.0, bright);
    }
    float m = max(c.r, max(c.g, c.b)), k = P.params.x;
    if (m > k) {
        float room = 1.0 - k;
        float shown = room > 0.0 ? k + room * (1.0 - exp(-(m - k) / room)) : 1.0;
        float white = m > 1.0 ? 1.0 - 1.0 / (1.0 + 0.25 * (m - 1.0)) : 0.0;   // 0 at 1, 0.2 at 2, 0.5 at 5
        c = mix(c * (shown / m), vec3(shown), white);
    }
    outColor = vec4(Grade(min(c, vec3(1.0)), far), clamp(s.a, 0.0, 1.0));
}
