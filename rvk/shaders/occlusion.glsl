// The tone mapping's inputs, ambient occlusion and indirect light (hdr.cpp), shared with the depth of field composite.
layout(set = 0, binding = 0) uniform sampler2D scene;
layout(set = 0, binding = 1) uniform sampler2D bloom;   // half resolution, light above the bloom threshold, blurred
layout(set = 0, binding = 2) uniform sampler2D ao;      // half resolution: ambient occlusion (1 = open), view depth
layout(set = 0, binding = 3) uniform sampler2D depthTex; // full resolution scene depth (for the AO upsampling)
layout(set = 0, binding = 4) uniform sampler2D localFraction;   // how much of each pixel local lights lit
layout(set = 0, binding = 5) uniform sampler2D gi;      // half resolution: indirect light, view depth
layout(set = 0, binding = 6) uniform sampler2D albedo;  // the surfaces' own colour (fogged)
layout(set = 0, binding = 7) uniform sampler2D volume;  // half resolution: light scattered by the air, view depth
layout(set = 0, binding = 8) uniform sampler2D ssr;     // reflected colour, how much of it shows
layout(set = 0, binding = 9) uniform sampler2D contact; // half resolution: contact shadow (1 = lit), view depth
layout(set = 0, binding = 10) uniform sampler3D lutDay;  // colour grading lookup tables (identity unless loaded)
layout(set = 0, binding = 11) uniform sampler3D lutNight;
layout(set = 0, binding = 12) uniform sampler2D glowRaw; // full resolution: what additive effects added (the scene's glow)
layout(set = 0, binding = 13) uniform sampler2D smokeCover;   // half resolution: the smoke drawn this frame (A: its
                                                              // cover; P.dust2.w bit 1 only)
layout(set = 0, binding = 14) uniform sampler2D sandFog;      // half resolution: a storm's air (sand_fog.frag) - the
                                                              // light it scatters, its transmittance (dust2.w bit 2)
layout(push_constant) uniform Push {
    vec4 params;        // knee, exposure, bloom strength, bloom left on objects nearer than its light (-1: no depth)
    vec4 proj;          // D3D projection m[2][2], m[3][2] (view depth from the depth buffer), indirect light strength,
                        // volumetric light on (bit 1), reflections on (bit 2), ambient occlusion on (bit 4),
                        // contact shadows on (bit 8), the depth is the scene's (bit 16)
    vec4 grade;         // saturation, contrast, warmth (-1 cool .. 1 warm), lookup table amount
    vec4 grade2;        // night (0 day .. 1 night), night tint, vignette, unused
    vec4 haze;          // the haze over the distant land (bit 16 only): rgb its colour, w how much (0: none)
    vec4 fog;           // x, y: the game's fog start and end (view depth); z: the valley mist's top over the eye,
                        // w: its density (per world unit; 0: none - rvk mist.cpp)
    // The air (RVK_Atmos; rvk hdr.cpp LandHaze): light scattered by the air between the eye and the land.
    vec4 air0;          // x, y: the projection's m[0][0], m[1][1] (view rays), z: the air's density at the eye (per
                        // world unit; 0: the plain haze instead), w: its scale height (world units)
    vec4 air1;          // xyz: up (view space), w: the sun's glow in the haze, x
    vec4 air2;          // xyz: towards the sun - by night the moon (view space), w: the glow's forward lobe (HG g)
    vec4 air3;          // rgb: the sun's (moon's) light through the air - golden, then red low
    // A sand or ash storm (rvk wind.cpp SandHaze): the air thick with it, from the game's own storm tint.
    vec4 dust;          // rgb: its colour (as the scene's light); w: its density (per world unit; 0: none)
    vec4 dust2;         // x: the wind's clock; y: how much it surges with the gusts; z: the veil at every distance
                        // (with the fog - w bit 2 -: how much the land's light takes the dust's colour);
                        // w: + 1 = smokeCover holds this frame's smoke (blowing sand: near, not behind the sky's
                        // veil), + 2 = sandFog holds the storm's air (RVK_SandFog: in place of the flat haze), + 4 x the
                        // rain mist's share of it in 15ths (a thunderstorm's: grey, not reddening)
    // The storm's distant gusts (rvk wind.cpp SandGusts; tonemap.frag): beyond the simulated sand round the camera,
    // billows of thicker and thinner dust drifting with the wind through the haze.
    vec4 gust0;         // xyz: the eye (world); w: the ground's height under it
    vec4 gust1;         // xyz: the view's forward (world); w: the projection's m00
    vec4 gust2;         // x: the projection's m11; y: where the simulated sand ends (world units from the eye); z: how
                        // strong the gusts (RVK_SandGusts; 0 none); w: the wind's clock
    vec4 gust3;         // xy: the wind's direction (x z); zw: along and across the wind, what to add to a place's
                        // dot products (its frame, carried with the wind)
} P;

// A reduced-resolution image at this pixel: of the 4 nearest texels, those at this pixel's depth (the same surface,
// view depth in channel depthChannel), so an object's occlusion or light doesn't spill onto the background around
// its outline.
vec4 Upsample(sampler2D t, int depthChannel, float scale)
{
    ivec2 pix = ivec2(gl_FragCoord.xy), size = textureSize(t, 0);
    float z = P.proj.y / (min(texelFetch(depthTex, pix, 0).r, 0.999999) - P.proj.x);
    vec2 h = (gl_FragCoord.xy - 0.5) / scale;    // texel i stands for full-resolution pixel scale * i
    ivec2 base = ivec2(floor(h));
    vec2 f = h - vec2(base);
    vec4 sum = vec4(0.0);
    float weight = 0.0;
    for (int j = 0; j < 4; ++j) {
        ivec2 o = ivec2(j & 1, j >> 1);
        vec4 s = texelFetch(t, clamp(base + o, ivec2(0), size - 1), 0);
        float bilinear = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y) + 1e-3;
        float w = bilinear / (1e-3 + abs(s[depthChannel] - z) / z * 40.0);
        sum += s * w;
        weight += w;
    }
    return sum / weight;
}

// The sand fog at this pixel (its texel i marched from full-resolution pixel 2 i): of the 4 nearest texels, those
// at this pixel's depth (their depth read back from the depth buffer where they were marched).
vec4 SandFogAt()
{
    ivec2 pix = ivec2(gl_FragCoord.xy), size = textureSize(sandFog, 0);
    float z = P.proj.y / (min(texelFetch(depthTex, pix, 0).r, 0.999999) - P.proj.x);
    vec2 h = (gl_FragCoord.xy - 0.5) / 2.0;
    ivec2 base = ivec2(floor(h));
    vec2 f = h - vec2(base);
    vec4 sum = vec4(0.0);
    float weight = 0.0;
    for (int j = 0; j < 4; ++j) {
        ivec2 o = ivec2(j & 1, j >> 1);
        ivec2 q = clamp(base + o, ivec2(0), size - 1);
        float zq = P.proj.y / (min(texelFetch(depthTex, q * 2, 0).r, 0.999999) - P.proj.x);
        float bilinear = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y) + 1e-3;
        float w = bilinear / (1e-3 + abs(zq - z) / z * 40.0);
        sum += texelFetch(sandFog, q, 0) * w;
        weight += w;
    }
    return sum / weight;
}

float Occlusion()
{
    if (texelFetch(depthTex, ivec2(gl_FragCoord.xy), 0).r >= 1.0) return 1.0;
    return Upsample(ao, 1, 2.0).r;
}

// The light surfaces reflect from the lit scene around them (indirect light x their own colour); 0 when off.
vec3 Indirect()
{
    if (P.proj.z <= 0.0 || texelFetch(depthTex, ivec2(gl_FragCoord.xy), 0).r >= 1.0) return vec3(0.0);
    return Upsample(gi, 3, 4.0).rgb * texelFetch(albedo, ivec2(gl_FragCoord.xy), 0).rgb * P.proj.z;
}

// What the scene's colour is multiplied by for ambient occlusion - which takes ambient light away, not the light
// local lights shine into the corner - and contact shadows.
float AmbientFactor()
{
    float factor = 1.0;
    if ((int(P.proj.w) & 4) != 0) {
        float local = texelFetch(localFraction, ivec2(gl_FragCoord.xy), 0).r;
        factor = mix(Occlusion(), 1.0, local);
    }
    // Contact shadows take away the direct sunlight's share of the colour (the local-fraction target's B).
    if ((int(P.proj.w) & 8) != 0 && texelFetch(depthTex, ivec2(gl_FragCoord.xy), 0).r < 1.0) {
        float sunShare = texelFetch(localFraction, ivec2(gl_FragCoord.xy), 0).b;
        factor *= 1.0 - (1.0 - Upsample(contact, 1, 2.0).r) * sunShare;
    }
    return factor;
}

// Light scattered towards the camera by the air in front of this pixel (volumetric light); 0 when off.
vec3 Volumetric()
{
    if ((int(P.proj.w) & 1) == 0) return vec3(0.0);
    return Upsample(volume, 3, 2.0).rgb;
}

// The surface's colour with its reflection over it (screen-space reflections).
vec3 Reflected(vec3 c)
{
    if ((int(P.proj.w) & 2) == 0) return c;
    vec4 r = texelFetch(ssr, ivec2(gl_FragCoord.xy), 0);
    return mix(c, r.rgb, r.a);
}

// The bloom at this pixel, held back where the pixel is well in front of the light the bloom spreads (the levels carry
// the light's brightness / view depth): foliage before a lamp doesn't glow with the lamp's halo.
vec3 Bloom()
{
    vec4 b = texture(bloom, gl_FragCoord.xy / vec2(textureSize(scene, 0)));
    if (P.params.w < 0.0) return b.rgb;
    float luma = dot(b.rgb, vec3(0.3, 0.59, 0.11));
    float d = texelFetch(depthTex, ivec2(gl_FragCoord.xy), 0).r;
    if (luma <= 1e-5 || d >= 1.0) return b.rgb;
    float lightInvZ = b.a / luma;                       // how far the light is (1 / view depth, brightness-weighted)
    float z = P.proj.y / (min(d, 0.999999) - P.proj.x);
    float ratio = 1.0 / max(z * lightInvZ, 1e-4);      // light depth / this pixel's depth: > 1 = the pixel is in front
    return b.rgb * mix(1.0, P.params.w, smoothstep(1.15, 1.6, ratio));
}
