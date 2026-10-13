#version 450
// The cloud layer (rvk/clouds.cpp MarchCloudLayer; RVK_CloudLayer): a shell of cloud between a base and a top height
// over a large sphere round the eye (so it bends down to the horizon), marched along each view ray at half size. What
// is where comes from the weather map (coverage, type, density: the game's puffs and weather); the shape from tiling
// noise - a Perlin-Worley body by the type's height profile, eaten into by coverage, its edges eroded by finer Worley
// (wispy below, billowy above); the light from the sun through the cloud (a few steps towards it, several scattering
// orders, a forward lobe for silver edges, powdery dark edges) and from the sky (lighter at the tops), tinted by the
// game's own cloud colour. Premultiplied; filtered up over the scene (cloud_up.frag).
#include "frame_lights.glsl"                    // FL (binding 4): the sun, the TAA's noise

layout(set = 0, binding = 0, std140) uniform LayerFrame {
    mat4 invViewProj;   // clip -> world (the world camera; D3D row-vector convention, as the scene's)
    vec4 eye;           // xyz: the eye (world); w: time (s)
    vec4 layer;         // x: the base's height, y: the top's (world y), z: the sphere's radius, w: the march's reach
    vec4 weather;       // xy: the weather map's corner (world x z), z: 1 / its size, w: 1 = skip covered pixels
    vec4 shape;         // x: the shape noise's scale (per world unit), y: the detail's, z: erosion, w: extinction
    vec4 wind;          // xyz: the noise's drift (world), w: the pre-pass's clear depth
    vec4 tint;          // rgb: the game's cloud colour, w: brightness
    vec4 ambient;       // rgb: the sky's light; w: rain (0..1)
    vec4 haze;          // rgb: the distance's colour (the fog the game draws its clouds in); w: the haze's distance
                        // (world)
    vec4 view;          // xy: the half-size target's size, z: steps, w: steps towards the sun
    vec4 sub;           // reused over frames (cloud_resolve.frag): xy this frame's pixel in each 2x2, z: 2 (one of each
                        // 2x2 marched, into a quarter-size target) or 1 (every pixel)
    vec4 horizon;       // x: how much the game fogs its clouds (0..1: towards the horizon they become the fog's colour,
                        // as the game's do - the fogged ground meets them seamlessly); y: contrast (RVK_CloudShade)
    vec4 shadowArea;    // the shadow map (RVK_CLOUD_SHADOW): its corner x, z (world), its size, the plane it lies in
                        // (world y)
    vec4 deck;          // the low deck (stratus: the game's flat sheets and dull overcast): x how much (0..1), y its
                        // base, z its top (world y)
} L;
layout(set = 0, binding = 1) uniform sampler2D weatherMap;
layout(set = 0, binding = 2) uniform sampler3D shapeNoise;
layout(set = 0, binding = 3) uniform sampler2D sceneDepth;  // the pre-pass's (L.weather.w): only where it is clear
layout(set = 0, binding = 5) uniform sampler3D detailNoise;

layout(location = 0) out vec4 outScene;

const float kPi = 3.14159265;

float Remap(float v, float lo, float hi, float a, float b) { return a + (v - lo) * (b - a) / max(hi - lo, 1e-5); }

// The sunlight reaching the clouds: reddened low in the sky (its long way through the air - orange, then pink near
// the horizon), gone a little after it sets (the clouds stay lit a while: they're high).
vec3 SunLight(vec3 sunL)
{
    float low = 1.0 - smoothstep(0.0, 0.35, sunL.y);
    low *= low;                                  // (golden first; pink only in its last few degrees)
    vec3 tint = mix(vec3(1.0), mix(vec3(1.0, 0.8, 0.58), vec3(1.0, 0.6, 0.5), smoothstep(0.75, 1.0, low)), low);
    return FL.sunColor.rgb * tint * smoothstep(-0.12, 0.1, sunL.y);
}

// The glow under the clouds round sunset: their undersides lit pink by the light low in the sky (most with the sun
// at the horizon and a little after). Scaled as the sky's light (about 1).
vec3 Afterglow(vec3 sunL)
{
    float g = smoothstep(-0.14, -0.02, sunL.y) * (1.0 - smoothstep(0.02, 0.12, sunL.y));
    return vec3(1.0, 0.5, 0.55) * g;
}

// The air between the eye and a cloud dist away (premultiplied light, alpha): its colour goes towards the game's
// distance colour (the more the farther - wholly far off - and low in the sky the more the game fogs its clouds). The
// cloud stays as opaque as it is: the air in front of it hides what's behind as well (thinning far clouds instead
// showed the game's moon through them - user); only at the march's reach do they fade out (a band at the horizon).
vec4 Aerial(vec3 light, float alpha, float dist, vec3 dir)
{
    float air = 1.0 - exp(-dist / L.haze.w);
    float low = 1.0 - smoothstep(0.0, 0.5, dir.y);
    float haze = max(min(air * 1.2, 1.0), L.horizon.x * low * low * (3.0 - 2.0 * low));
    light = mix(light, L.haze.rgb * alpha, haze);
    return vec4(light, alpha) * (1.0 - smoothstep(0.6 * L.layer.w, L.layer.w, dist));
}

// Where a ray from o along unit d is inside the sphere at c of radius r: t0 <= t1 (t1 < 0: never).
vec2 Sphere(vec3 o, vec3 d, vec3 c, float r)
{
    vec3 oc = o - c;
    float b = dot(oc, d), q = dot(oc, oc) - r * r, h = b * b - q;
    if (h < 0.0)
        return vec2(1.0, -1.0);
    h = sqrt(h);
    return vec2(-b - h, -b + h);
}

vec3 gCentre;
float gInner, gThick;

// 0 at the base .. 1 at the top.
float Height(vec3 p) { return (length(p - gCentre) - gInner) / gThick; }

// How much of the layer's height the type fills: stratus a low thin band, cumulus a flat base and rounded top half
// way up, storm clouds nearly all of it.
float Profile(float h, float type)
{
    float stratus = smoothstep(0.0, 0.06, h) * (1.0 - smoothstep(0.12, 0.28, h));
    float cumulus = smoothstep(0.0, 0.1, h) * (1.0 - smoothstep(0.35, 0.65, h));
    float storm = smoothstep(0.0, 0.1, h) * (1.0 - smoothstep(0.7, 1.0, h));
    return type < 0.5 ? mix(stratus, cumulus, type * 2.0) : mix(cumulus, storm, type * 2.0 - 1.0);
}

float Density(vec3 p, bool detail)
{
    float h = Height(p);
    if (h <= 0.0 || h >= 1.0)
        return 0.0;
    vec2 uv = (p.xz - L.weather.xy) * L.weather.z;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))))
        return 0.0;
    vec4 w = textureLod(weatherMap, uv, 0.0);
    if (w.r < 0.005)                             // (clear: nothing - the warp below reaches only a few texels)
        return 0.0;
    vec3 q = (p + L.wind.xyz) * L.shape.x;
    q.y *= 1.6;                                  // (cells a little flatter than wide)
    vec4 n = textureLod(shapeNoise, q, 0.0);
    // The weather read a little off where the body's noise says, differently at each height: coverage's edges then
    // lean and wander instead of standing as straight walls (seen from below, walls line up into streaks).
    w = textureLod(weatherMap, uv + (n.gb - 0.5) * (500.0 * L.weather.z), 0.0);
    // (... and a little more or less with the body's noise: in 3D, so no edge of the map's runs straight up.)
    float cover = clamp(w.r + (n.a - 0.5) * 0.3, 0.0, 1.0);
    if (cover < 0.01)
        return 0.0;
    float fbm = n.g * 0.625 + n.b * 0.25 + n.a * 0.125;
    float lift = w.a * 0.07;                     // (the uneven underside: the clouds start this much higher here -
                                                 // gently: more, and an overcast hangs in waves)
    float body = Remap(n.r, fbm - 1.0, 1.0, 0.0, 1.0) * Profile(clamp((h - lift) / (1.0 - lift), 0.0, 1.0), w.g);
    // Coverage eats into the body from its thin parts: little cover, only the cores remain.
    float d = clamp(Remap(body, 1.0 - cover, 1.0, 0.0, 1.0), 0.0, 1.0) * cover;
    if (d <= 0.0 || !detail)                     // (the detail below only takes away)
        return d * w.b;
    // Billows: the body bumped by its own cells three times finer (cauliflower tops rather than smooth blobs) - more
    // towards the top, where cumulus heap up.
    float billow = textureLod(shapeNoise, q * 3.1 + 0.17, 0.0).g;
    // (As much as the edges' erosion: the weather's detail - storms smoother.)
    body = Remap(body, (1.0 - billow) * mix(0.15, 0.35, clamp(h * 2.0, 0.0, 1.0)) * clamp(L.shape.z / 0.65, 0.3, 1.5), 1.0, 0.0, 1.0);
    d = clamp(Remap(body, 1.0 - cover, 1.0, 0.0, 1.0), 0.0, 1.0) * cover;
    if (d <= 0.0)
        return 0.0;
    // The edges eroded by finer cells, swirled by the body's own noise (wisps curl rather than tile), at two scales.
    vec3 swirl = (n.gba - 0.5) * 0.35;
    vec3 dq = (p + L.wind.xyz * 1.7) * L.shape.y + swirl;
    vec3 e = textureLod(detailNoise, dq, 0.0).rgb;
    float ef = e.r * 0.625 + e.g * 0.25 + e.b * 0.125;
    ef = mix(ef, textureLod(detailNoise, dq.zxy * 1.37 + 0.31, 0.0).g, 0.35);   // (another scale, axes turned: no
                                                                               // repeat lines up)
    ef = mix(ef, 1.0 - ef, clamp(h * 4.0, 0.0, 1.0));   // wispy underneath, billowy above
    d = clamp(Remap(d, ef * L.shape.z, 1.0, 0.0, 1.0), 0.0, 1.0);
    return d * w.b;
}

float HG(float c, float g) { return (1.0 - g * g) / (4.0 * kPi * pow(1.0 + g * g - 2.0 * g * c, 1.5)); }

// The low deck (stratus): a thin, nearly even sheet below the layer where the game's sky is flat sheets and overcast -
// soft, fibrous (its noise drawn out along the wind ~3:1, as the game's fibrous puff textures on its sheets), a little
// thicker in the middle of its height, broken here and there (fewer breaks the more of it).
float DeckDensity(vec3 p)
{
    float r = length(p - gCentre) - (L.layer.z - L.eye.y);   // (its height, world y, over the curved ground)
    float h = (r - L.deck.y) / max(L.deck.z - L.deck.y, 1.0);
    if (h <= 0.0 || h >= 1.0)
        return 0.0;
    vec2 q = p.xz + L.wind.xz;
    float broad = clamp((textureLod(shapeNoise, vec3(q / 14000.0, 0.47), 0.0).r - 0.54) / 0.26, 0.0, 1.0);
    float cover = smoothstep(1.0 - L.deck.x, 1.25 - L.deck.x * 0.6, broad * 0.5 + 0.5 * L.deck.x);
    if (cover <= 0.0)
        return 0.0;
    vec2 along = normalize(L.wind.xz + vec2(1e-3, 0.0)), across = vec2(-along.y, along.x);
    vec2 f = vec2(dot(q, along), dot(q, across));
    // (Worley fbm: values bunched in ~0.4..0.85 - spread, or the texture disappears.)
    float fib = clamp((textureLod(detailNoise, vec3(f.x / 3000.0, h * 0.35, f.y / 1000.0), 0.0).r - 0.42) / 0.4, 0.0, 1.0);
    float fine = clamp((textureLod(detailNoise, vec3(f.x / 1100.0, 0.5 + h * 0.5, f.y / 380.0), 0.0).g - 0.42) / 0.4, 0.0, 1.0);
    float body = smoothstep(0.0, 0.35, h) * (1.0 - smoothstep(0.55, 1.0, h));
    float d = cover * body * (fib * 0.85 + fine * 0.45) - 0.2;
    return clamp(d, 0.0, 1.0);
}

// The deck along the view ray (below it, looking up through it): a few steps, lit mostly by the sky - lighter at its
// top, darker beneath - a little sun through it; hazed as the layer. Premultiplied.
vec4 Deck(vec3 eye, vec3 dir, float jitter)
{
    if (L.deck.x <= 0.0 || eye.y >= L.deck.y)
        return vec4(0.0);
    float R = L.layer.z;
    vec2 a = Sphere(eye, dir, gCentre, R + (L.deck.y - eye.y)), b = Sphere(eye, dir, gCentre, R + (L.deck.z - eye.y));
    float t0 = a.y, t1 = min(b.y, L.layer.w);
    if (t1 <= t0 || t0 <= 0.0)
        return vec4(0.0);
    const int kSteps = 8;
    float dt = (t1 - t0) / float(kSteps);
    float sigma = L.shape.w * 0.9, transmit = 1.0;
    vec3 sunL = -normalize(FL.sunDir.xyz + vec3(0.0, -1e-6, 0.0));
    vec3 sun = SunLight(sunL) * 0.35 + Afterglow(sunL) * 0.45;   // (its underside: the afterglow on it)
    vec3 light = vec3(0.0);
    for (int i = 0; i < kSteps && transmit > 0.02; ++i) {
        vec3 p = eye + dir * (t0 + (float(i) + jitter) * dt);
        float d = DeckDensity(p);
        if (d <= 0.001)
            continue;
        float h = (float(i) + 0.5) / float(kSteps);
        // (Seen from below: mostly its underside - darker than the layer's sunlit tops; thick parts darker still.)
        vec3 S = (L.ambient.rgb * mix(0.38, 0.8, h) * mix(0.55, 1.0, exp(-d * 2.0)) + sun) * L.tint.rgb * L.tint.w;
        const vec3 luma = vec3(0.3, 0.59, 0.11);   // (contrast, as the layer: RVK_CloudShade)
        S *= clamp(pow(max(dot(S, luma) / max(dot(L.tint.rgb, luma) * L.tint.w, 1e-3), 1e-3), L.horizon.y - 1.0), 0.15, 3.0);
        float st = exp(-d * sigma * dt);
        light += transmit * (1.0 - st) * S;
        transmit *= st;
    }
    float alpha = 1.0 - transmit;
    if (alpha <= 0.0)
        return vec4(0.0);
    return Aerial(light, alpha, t0, dir);
}


#ifdef RVK_CLOUD_SHADOW
// The clouds' shadow map (rvk/clouds.cpp MakeCloudLayerShadow): top down, in a plane through the layer - for each
// texel, the sunlight through the layer along the sun's way (a few samples through its height): 1 clear. Ground
// shadows (ffp_main, grass, water) and the light shafts read it where the sun's ray through a point meets the plane.
void main()
{
    vec3 eye = L.eye.xyz;
    float R = L.layer.z;
    gCentre = vec3(eye.x, eye.y - R, eye.z);
    gInner = R + (L.layer.x - eye.y);
    gThick = (R + (L.layer.y - eye.y)) - gInner;
    vec3 sunL = -normalize(FL.sunDir.xyz + vec3(0.0, -1e-6, 0.0));
    if (sunL.y < 0.05) {
        outScene = vec4(1.0);
        return;
    }
    vec3 p = vec3(L.shadowArea.x + gl_FragCoord.x / 256.0 * L.shadowArea.z, L.shadowArea.w,
                  L.shadowArea.y + gl_FragCoord.y / 256.0 * L.shadowArea.z);
    const int kSamples = 8;
    float od = 0.0, span = (L.layer.y - L.layer.x) / float(kSamples);
    for (int k = 0; k < kSamples; ++k) {
        float y = L.layer.x + span * (float(k) + 0.5);
        od += Density(p + sunL * ((y - p.y) / sunL.y), false);
    }
    od *= span / sunL.y * L.shape.w;
    if (L.deck.x > 0.0) {                         // (and the low deck's)
        float ds = 0.0, dspan = (L.deck.z - L.deck.y) / 4.0;
        for (int k = 0; k < 4; ++k) {
            float y = L.deck.y + dspan * (float(k) + 0.5);
            ds += DeckDensity(p + sunL * ((y - p.y) / sunL.y));
        }
        od += ds * dspan / sunL.y * L.shape.w * 0.9;
    }
    float light = exp(-od * 0.25);                // (softened: thin cloud a light shade, thick cloud dark)
    outScene = vec4(light, light, light, 1.0);
}
#else
void main()
{
    // The half-size pixel marched (one of each 2x2 when reused over frames).
    vec2 hp = L.sub.z > 1.5 ? floor(gl_FragCoord.xy) * 2.0 + L.sub.xy + 0.5 : gl_FragCoord.xy;
    // The four scene pixels under this one: none of them sky - an opaque surface is drawn there later.
    if (L.weather.w > 0.5) {
        ivec2 at = ivec2(hp) * 2, last = textureSize(sceneDepth, 0) - 1;
        bool open = false;
        for (int k = 0; k < 4; ++k)
            open = open || texelFetch(sceneDepth, min(at + ivec2(k & 1, k >> 1), last), 0).r == L.wind.w;
        if (!open)
            discard;
    }
    // The view ray (the scene's flipped viewport; the TAA's jitter taken out as the scene's draws put it in).
    vec2 ndc = vec2(hp.x / L.view.x * 2.0 - 1.0, 1.0 - hp.y / L.view.y * 2.0) - FL.taa.xy;
    vec4 a = L.invViewProj * vec4(ndc, 0.0, 1.0), b = L.invViewProj * vec4(ndc, 1.0, 1.0);
    vec3 eye = L.eye.xyz;
    vec3 dir = normalize(b.xyz / b.w - a.xyz / a.w);
    // The shell: the ground's sphere through the eye's height (the layer's heights above it).
    float R = L.layer.z;
    gCentre = vec3(eye.x, eye.y - R, eye.z);
    gInner = R + (L.layer.x - eye.y);
    float outer = R + (L.layer.y - eye.y);
    gThick = outer - gInner;
    float jitter = fract(52.9829189 * fract(dot(hp, vec2(0.06711056, 0.00583715))) + FL.taa.z);
    vec4 deck = Deck(eye, dir, jitter);          // (below the layer: in front of it, looking up)
    vec2 si = Sphere(eye, dir, gCentre, gInner), so = Sphere(eye, dir, gCentre, outer);
    float r0 = length(eye - gCentre);
    float t0, t1;
    if (r0 < gInner) {                           // under the layer: from leaving the inner sphere to the outer
        t0 = si.y;
        t1 = so.y;
    } else if (r0 < outer) {                     // in it: to the outer sphere, or down into the inner one
        t0 = 0.0;
        t1 = si.y > 0.0 && si.x > 0.0 ? si.x : so.y;
    } else {                                     // above it
        if (so.y < 0.0 || so.x > so.y) { outScene = deck; return; }
        t0 = max(so.x, 0.0);
        t1 = si.x > 0.0 && si.y > si.x ? si.x : so.y;
    }
    t1 = min(t1, L.layer.w);
    if (t1 <= t0) {
        outScene = deck;
        return;
    }
    // Steps growing with the distance (short near the eye: the near clouds' detail; long far off), at most L.view.z.
    int steps = int(L.view.z);
    vec3 sunL = -normalize(FL.sunDir.xyz + vec3(0.0, -1e-6, 0.0));
    float cosT = dot(dir, sunL);
    vec3 sunColour = SunLight(sunL);
    vec3 glow = Afterglow(sunL);
    float sigma = L.shape.w;
    float transmit = 1.0;
    vec3 light = vec3(0.0);
    float depthSum = 0.0, weightSum = 0.0;      // (where the cloud is along the ray: its haze)
    float minStep = gThick * 0.012, along = (t1 - t0) / float(steps);
    float t = t0 + jitter * clamp(t0 * 0.012, minStep, along);
    for (int i = 0; i < steps && transmit > 0.01 && t < t1; ++i) {
        float dt = clamp(t * 0.012, minStep, max(along, minStep));
        vec3 p = eye + dir * t;
        t += dt;
        float d = Density(p, true);
        if (d <= 0.001) {
            t += dt;                             // (clear air: on twice as far)
            continue;
        }
        float tHere = t - dt;
        // The sun through the cloud: a few steps towards it, longer each (far off, fewer and longer).
        float od = 0.0;
        bool far = tHere > 8000.0;
        int lightSteps = far ? 2 : int(L.view.w);
        float ls = gThick * (far ? 0.1 : 0.045), grow = far ? 2.2 : 1.5;
        vec3 lp = p;
        for (int k = 0; k < lightSteps; ++k) {
            lp += sunL * ls;
            od += Density(lp, false) * ls;
            ls *= grow;
        }
        od *= sigma;
        // Several scattering orders (each dimmer, reaching deeper, less forward): light inside thick cloud; a strong
        // forward lobe - the silver edges looking towards the sun.
        float sun = 0.0;
        float aa = 1.0, bb = 1.0, cc = 1.0;
        for (int o = 0; o < 3; ++o) {
            // (Half even - light bounced about inside the cloud comes out every way: sunlit sides are bright with the
            // sun behind the eye too - half the two lobes.)
            float phase = mix(1.0, mix(HG(cosT, 0.75 * cc), HG(cosT, -0.25 * cc), 0.35) * 4.0 * kPi, 0.5);
            sun += aa * exp(-od * bb) * phase;
            aa *= 0.5; bb *= 0.4; cc *= 0.5;
        }
        // Powder: an edge facing the sun is darker than the inside it lights (light enters, doesn't come back out).
        float powder = 1.0 - 0.5 * exp(-d * sigma * gThick * 0.08);
        float h = Height(p);
        // The sky's light: the sky's colour from above (lighter at the tops), less and warmer from below (the ground
        // lit by it), dimmed by the cloud over the point (the sun's way through it stands in for up: dark storm bases).
        vec3 skyTop = L.ambient.rgb * 1.15, skyLow = L.ambient.rgb * vec3(0.66, 0.62, 0.58);
        // (Dimmed by the cloud towards the sun only over a short way - from far off, the cloud's shadows lie through
        // it as long streaks all one way.)
        vec3 sky = mix(skyLow * mix(1.0, 0.75, L.ambient.w), skyTop, smoothstep(0.0, 0.8, h)) * mix(0.4, 1.0, exp(-od * 0.3));
        sky += glow * (1.0 - smoothstep(0.0, 0.7, h)) * 0.9;   // (round sunset: the pink undersides)
        // (The game's colour - its puffs as it shows them, fog and all - wholly in the sky's light, half in the sun's:
        // a storm takes the game's colour, sunlit tops stay bright.)
        vec3 S = (sunColour * sun * powder * 1.5 * mix(L.tint.rgb, vec3(dot(L.tint.rgb, vec3(0.3, 0.59, 0.11))), 0.5) +
                  sky * L.tint.rgb) * L.tint.w;
        // Contrast (RVK_CloudShade): how far from the cloud's middle light (its colour as the game shows it) a part
        // is, made more - shaded parts darker, lit parts lighter - its hue kept.
        const vec3 luma = vec3(0.3, 0.59, 0.11);
        float middle = max(dot(L.tint.rgb, luma) * L.tint.w, 1e-3);
        S *= clamp(pow(max(dot(S, luma) / middle, 1e-3), L.horizon.y - 1.0), 0.15, 3.0);
        float stepT = exp(-d * sigma * dt);
        light += transmit * (1.0 - stepT) * S;
        depthSum += transmit * (1.0 - stepT) * tHere;
        weightSum += transmit * (1.0 - stepT);
        transmit *= stepT;
    }
    float alpha = 1.0 - transmit;
    if (alpha <= 0.0) {
        outScene = deck;
        return;
    }
    // The air: by where along the ray the cloud is.
    vec4 layer = Aerial(light, alpha, depthSum / max(weightSum, 1e-6), dir);
    outScene = deck + (1.0 - deck.a) * layer;
}
#endif
