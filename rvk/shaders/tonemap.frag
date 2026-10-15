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
    // By night (RVK_NightTint) colour fades and leans blue as eyes see in the dark - most in the darks, little in what
    // is bright (lamps, windows keep their colour).
    float nightTint = P.grade2.x * P.grade2.y;
    float darks = 1.0 - smoothstep(0.08, 0.45, dot(c, kLuma));
    c = mix(c, dot(c, kLuma) * vec3(0.78, 0.9, 1.18), min(nightTint * mix(0.3, 1.2, darks), 1.0));
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

// The air between the eye and a surface z (view depth) away (RVK_Atmos, docs/scattering-plan.md): thicker low down -
// valleys below the eye hazier, peaks above clearer (exponential with height, integrated along the ray) - its
// extinction taking red first (the far land bluer), the light it scatters the game's fog colour (the zone's air) and,
// towards the sun, its glow (a forward lobe: hazy air round a low sun lights up), the sun's light reddened as it sets.
vec3 Air(vec3 c, float z)
{
    vec2 uv = gl_FragCoord.xy / vec2(textureSize(scene, 0));
    vec3 V = vec3((uv.x * 2.0 - 1.0) / P.air0.x, (1.0 - uv.y * 2.0) / P.air0.y, 1.0) * z;
    float dist = length(V);
    vec3 dir = V / max(dist, 1e-4);
    float x = dot(V, P.air1.xyz) / P.air0.w;      // (height over the eye, in scale heights)
    float f = abs(x) < 1e-3 ? 1.0 : (1.0 - exp(-x)) / x;
    float tau = P.air0.z * dist * clamp(f, 0.0, 4.0);
    vec3 through = exp(-tau * vec3(0.8, 1.0, 1.3));
    float g = P.air2.w, cosT = dot(dir, P.air2.xyz);
    float phase = min((1.0 - g * g) / pow(1.0 + g * g - 2.0 * g * cosT, 1.5), 5.0);
    vec3 scattered = (1.0 - through) * P.haze.rgb + (1.0 - exp(-tau)) * P.air3.rgb * (phase * P.air1.w);
    c = c * through + scattered;
    // The valley mist (rvk mist.cpp): a layer up to P.fog.z over the eye, fading over ~8 units above. Its depth along
    // the ray: the density's integral over the heights the ray crosses, over how fast it climbs. Lit as the air - the
    // zone's colour, paler - and the sun's (moon's) glow.
    if (P.fog.w > 0.0) {
        float h0 = 0.0, h1 = dot(V, P.air1.xyz), top = P.fog.z;
        float tauM;
        if (abs(h1 - h0) < 0.5) {
            float h = 0.5 * (h0 + h1);
            tauM = dist * (h < top ? 1.0 : exp(-(h - top) / 8.0));
        } else {
            // (G: the density's integral from -inf: full below the top, then 8 units more fading.)
            #define MIST_G(y) ((y) < top ? (y) : top + 8.0 * (1.0 - exp(-((y) - top) / 8.0)))
            tauM = dist * (MIST_G(h1) - MIST_G(h0)) / (h1 - h0);
            #undef MIST_G
        }
        tauM *= P.fog.w;
        float m = 1.0 - exp(-tauM);
        float l = dot(P.haze.rgb, vec3(0.3, 0.59, 0.11));
        vec3 mist = mix(P.haze.rgb, vec3(l), 0.45) * 1.2 + P.air3.rgb * (phase * P.air1.w * 0.5);
        c = mix(c, mist, m);
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
            float gameFog = clamp((z - P.fog.x) / (P.fog.y - P.fog.x), 0.0, 1.0);
            if (P.air0.z > 0.0) {
                c = mix(c, Air(c, z), min(P.haze.w * 2.5, 1.0) * (1.0 - gameFog));
            } else {
                float air = 1.0 - exp(-z / (0.4 * P.fog.y));
                c = mix(c, P.haze.rgb, P.haze.w * air * (1.0 - gameFog));
            }
        }
    }
    // A sand or ash storm (rvk wind.cpp): the air thick with sand to the horizon - the sky wholly, the land through a
    // veil that thickens with distance, surging with the wind.
    if (P.dust.w > 0.0) {
        float dist = d < 1.0 ? P.proj.y / min(d - P.proj.x, -1e-9) : 1e5;
        vec2 uv = gl_FragCoord.xy / vec2(textureSize(scene, 0));
        float surge = 1.0 + P.dust2.y * (0.6 * sin(P.dust2.x * 0.9 + uv.x * 5.0) + 0.4 * sin(P.dust2.x * 2.3 - uv.y * 7.0 + uv.x * 3.0));
        // (Sand in the air right in front too: a veil at every distance, P.dust2.z, under the haze that thickens
        // with it.)
        float through = exp(-P.dust.w * dist * max(surge, 0.2)) * (1.0 - P.dust2.z * clamp(surge, 0.5, 1.5));
        through = clamp(through, 0.0, 1.0);
        // Smoke and blowing sand (drawn over the scene before this, without depth) are near: the pixel taken apart
        // again - the smoke's own light S and cover a (the half-size smoke image, premultiplied), what is behind it
        // c - S - and each veiled by its own distance: the smoke by a few units' air, what is behind by its own. (A
        // single veil by the depth behind gave the sand over the sky the sky's full haze and over near leaves next to
        // none - patchy; and with it set by the cover, thin sand looking up cleared the sky's haze - the user.)
        if (P.dust2.w > 0.5) {
            vec4 smoke = texture(smokeCover, uv);
            float a = clamp(smoke.a, 0.0, 1.0);
            vec3 S = min(smoke.rgb, c);
            float near_ = clamp(exp(-P.dust.w * 6.0 * max(surge, 0.2)) * (1.0 - P.dust2.z * clamp(surge, 0.5, 1.5)), 0.0, 1.0);
            c = S * near_ + P.dust.rgb * a * (1.0 - near_) + mix(P.dust.rgb * (1.0 - a), c - S, through);
            through = mix(through, near_, a);
        } else {
            c = mix(P.dust.rgb, c, through);
        }
        // What the sand hides is no longer sky: the night's and the grading's sky (by far) only as much as shows
        // through - else a darker, greyer band of "sky" over a storm that covers it.
        far *= through;
    }
    c = max((c + Volumetric() + glow) * P.params.y, vec3(0.0));
    // Night (RVK_NightDark x how much it is night): darker - the darks and middle tones down, the sky more; what is
    // bright kept (lamps, windows, the moon, stars, moonlit cloud edges), so it stands out of the dark.
    float nightDark = P.grade2.x * P.grade2.w;
    if (nightDark > 0.0) {
        // (Nearer, from a lower brightness: lamp-lit ground keeps most of its light; the sky and clouds as tuned.)
        float sky = smoothstep(0.7, 1.0, far);
        float bright = smoothstep(mix(0.25, 0.35, sky), mix(0.8, 0.9, sky), dot(c, vec3(0.3, 0.59, 0.11)));
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
    // Alpha: with TAA (flag 16), how much of this pixel additive effects made - their glow, as shown (taa.frag gives
    // those pixels more of the new frame); else the scene's alpha.
    float alpha = clamp(s.a, 0.0, 1.0);
    if ((int(P.proj.w) & 16) != 0) {
        vec3 effect = texelFetch(glowRaw, ivec2(gl_FragCoord.xy), 0).rgb * P.params.y;
        alpha = smoothstep(0.01, 0.25, dot(effect, vec3(0.3, 0.59, 0.11)));
    }
    outColor = vec4(Grade(min(c, vec3(1.0)), far), alpha);
}
