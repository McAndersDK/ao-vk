#version 450
// The cloud layer (rvk/clouds.cpp MarchCloudLayer; RVK_CloudLayer): a shell of cloud between a base and a top height
// over a large sphere round the eye (so it bends down to the horizon), marched along each view ray at half size. What
// is where comes from the weather map (coverage, type, density: the game's puffs and weather); the shape from tiling
// noise - a Perlin-Worley body by the type's height profile, eaten into by coverage, its edges eroded by finer Worley
// (wispy below, billowy above); the light from the sun through the cloud (a few steps towards it, several scattering
// orders, a forward lobe for silver edges, powdery dark edges) and from the sky (lighter at the tops), tinted by the
// game's own cloud colour. Premultiplied; filtered up over the scene (cloud_up.frag).
#include "frame_lights.glsl"                    // FL (binding 4): the sun, the TAA's noise
#include "cloud_light.glsl"                     // the sun, the afterglow, the night's sky on clouds

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
                        // 2x2 marched, into a quarter-size target) or 1 (every pixel); w: the shadow map's - how
                        // cloudy the sky is
    vec4 horizon;       // x: how much the game fogs its clouds (0..1: towards the horizon they become the fog's colour,
                        // as the game's do - the fogged ground meets them seamlessly); y: contrast (RVK_CloudShade)
    vec4 shadowArea;    // the shadow map (RVK_CLOUD_SHADOW): its corner x, z (world), its size, the plane it lies in
                        // (world y)
    vec4 deck;          // the low deck (stratus: the game's flat sheets and dull overcast): x how much (0..1), y its
                        // base, z its top (world y); w: the game's sheets' faint shadow (RVK_CloudSheet)
    vec4 scud;          // a thunderstorm's scud: xy the low deck's own drift (racing ahead of the layer), z how much
                        // (0..1: torn into ragged fragments)
    vec4 morph;         // the churn (rvk clouds.cpp UpdateCloudDrift): x its field's phase (tiles, wrapping), y how far
                        // it pushes the cells (body tiles), z the detail's extra rise (world)
} L;
layout(set = 0, binding = 1) uniform sampler2D weatherMap;
layout(set = 0, binding = 2) uniform sampler3D shapeNoise;
layout(set = 0, binding = 3) uniform sampler2D sceneDepth;  // the pre-pass's (L.weather.w): only where it is clear
layout(set = 0, binding = 5) uniform sampler3D detailNoise;
layout(set = 0, binding = 6) uniform sampler2D blueNoise;   // 64 x 64 (rvk clouds.cpp CreateBlueNoise): the march's offsets

layout(location = 0) out vec4 outScene;

const float kPi = 3.14159265;

float Remap(float v, float lo, float hi, float a, float b) { return a + (v - lo) * (b - a) / max(hi - lo, 1e-5); }

// The air between the eye and a cloud dist away (premultiplied light, alpha): its colour goes towards the game's
// distance colour (the more the farther - wholly far off - and low in the sky the more the game fogs its clouds). The
// cloud stays as opaque as it is: the air in front of it hides what's behind as well (thinning far clouds instead
// showed the game's moon through them - user); only at the march's reach do they fade out (a band at the horizon).
vec4 Aerial(vec3 light, float alpha, float dist, vec3 dir)
{
    float air = 1.0 - exp(-dist / L.haze.w);
    float low = HorizonBand(dir, L.horizon.x);
    // (By night at least a quarter of the air's colour, as the game's sheets have: the heaps grey among them - user.)
    float haze = max(max(min(air * 1.1, 1.0), low), 0.25 * Night());
    light = mix(light, AirColour(L.haze.rgb, low) * alpha, haze);
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
    // (A thunderstorm's layer is taller - rvk clouds.cpp - for its towers, type 1: the rest keep to the height they
    // had, the lower 1300 units; the towers fill it all.)
    float tall = smoothstep(0.97, 1.0, type);
    h = h / mix(min(1300.0 / max(gThick, 1.0), 1.0), 1.0, tall);
    float stratus = smoothstep(0.0, 0.06, h) * (1.0 - smoothstep(0.12, 0.28, h));
    float cumulus = smoothstep(0.0, 0.1, h) * (1.0 - smoothstep(0.35, 0.65, h));
    float storm = smoothstep(0.0, 0.1, h) * (1.0 - smoothstep(0.7, 1.0, h));
    return type < 0.5 ? mix(stratus, cumulus, type * 2.0) : mix(cumulus, storm, type * 2.0 - 1.0);
}

// The noise's drift (main sets them): the body with the wind (and rising: shapes change), the detail faster. The
// shadow map's: all with the wind, none rising - its shadows slide over the land as one, no edge creeping against
// the rest (user: shadows going opposite ways).
vec3 gWind = vec3(0.0), gDetailWind = vec3(0.0);
vec3 gPush = vec3(0.0);                          // the churn's push at the last sample (Density)
bool gPushKept = false;                          // ... kept for the next ones (the sun's march)

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
    // (Under a storm's raised floor: nothing - out early, before the noise: a ray passed up to ~2.5 km of the layer's
    // empty bottom there, every sample paying for the shape noise.)
    if (L.ambient.w > 0.5 && w.g > 0.93 && h < (w.a * 900.0 - 160.0) / gThick)
        return 0.0;
    vec3 q = (p + gWind) * L.shape.x;
    q.y *= 1.6;                                  // (cells a little flatter than wide)
    // The churn: the cells pushed about by a slow field that itself moves on (its phase through the noise) - shapes
    // keep changing as they drift, never stretched out (the push stays bounded); the tops more than the bases. (User:
    // the clouds were static.) In the sun's march too, or the light would stand still while the shapes move.
    // (The sun's march takes its sample's push - gPushKept: the field is ~7 km across, its steps within ~1 km.)
    if (!gPushKept)
        gPush = textureLod(shapeNoise, (p + gWind * 0.85) * vec3(1.0, 1.6, 1.0) / 7000.0 + vec3(0.0, L.morph.x, 0.0), 0.0).gba - 0.5;
    q += gPush * (L.morph.y * (0.6 + 0.8 * h));
    // A storm (its sealed deck: type ~0.95, by the storm sky): its cells drawn out a little along the wind (rolls), pushed
    // further by the churn - its lumps had all one size on one plane: a wave pattern (user).
    float storm = L.ambient.w * smoothstep(0.85, 0.93, w.g);
    vec4 n;
    if (storm > 0.0) {                           // (its push: the churn's, larger in a storm - L.morph.y)
        vec2 along = normalize(L.wind.xz + vec2(1e-3, 0.0));
        float a = dot(q.xz, along), c = dot(q.xz, vec2(-along.y, along.x));
        vec2 st = along * (a * mix(1.0, 0.67, storm)) + vec2(-along.y, along.x) * c;
        n = textureLod(shapeNoise, vec3(st.x, q.y, st.y), 0.0);
    } else {
        n = textureLod(shapeNoise, q, 0.0);
    }
    // The weather read a little off where the body's noise says, differently at each height: coverage's edges then
    // lean and wander instead of standing as straight walls (seen from below, walls line up into streaks).
    w = textureLod(weatherMap, uv + (n.gb - 0.5) * (500.0 * L.weather.z), 0.0);
    // (... and a little more or less with the body's noise: in 3D, so no edge of the map's runs straight up.)
    float cover = clamp(w.r + (n.a - 0.5) * 0.3, 0.0, 1.0);
    // (A tower's anvil: spreading at its top, wider than the column below.)
    cover = clamp(cover + 0.4 * smoothstep(0.97, 1.0, w.g) * smoothstep(0.72, 0.88, h) * (1.0 - smoothstep(0.94, 1.0, h)), 0.0, 1.0);
    if (cover < 0.01)
        return 0.0;
    float fbm = n.g * 0.625 + n.b * 0.25 + n.a * 0.125;
    float lift = w.a * (0.07 + 0.08 * L.ambient.w);   // (in a storm - L.ambient.w - a lumpier underside)
    lift = mix(lift, w.a * 900.0 / gThick, storm);   // (a storm's: its base up to 900 units higher, by the map's swells)                     // (the uneven underside: the clouds start this much higher here -
                                                 // gently: more, and an overcast hangs in waves)
    // (A storm's floor ragged: its height wanders with the 3D noise, ~+-90 units, solid above it - not a sharp floor of
    // round bulbs. Not by thinning it: a see-through underside marched deep cost ~1.7 ms in game.)
    lift += (n.b - 0.5) * (180.0 / gThick) * storm;
    float hb = clamp((h - lift) / (1.0 - lift), 0.0, 1.0);
    float body = Remap(n.r, fbm - 1.0, 1.0, 0.0, 1.0) * Profile(hb, w.g);
    // Coverage eats into the body from its thin parts: little cover, only the cores remain.
    float d = clamp(Remap(body, 1.0 - cover, 1.0, 0.0, 1.0), 0.0, 1.0) * cover;
    if (d <= 0.0 || !detail)                     // (the detail below only takes away)
        return d * w.b;
    // Billows: the body bumped by its own cells three times finer (cauliflower tops rather than smooth blobs) - more
    // towards the top, where cumulus heap up.
    float billow = textureLod(shapeNoise, q * 3.1 + 0.17, 0.0).g;
    // (As much as the edges' erosion: the weather's detail - storms smoother.)
    // (A storm's underside without them: they made its even bulbs.)
    float billowAmount = mix(0.15, 0.35, clamp(h * 2.0, 0.0, 1.0)) * mix(1.0, 0.35 + 0.65 * smoothstep(0.05, 0.3, hb), storm);
    body = Remap(body, (1.0 - billow) * billowAmount * clamp(L.shape.z / 0.65, 0.3, 1.5), 1.0, 0.0, 1.0);
    d = clamp(Remap(body, 1.0 - cover, 1.0, 0.0, 1.0), 0.0, 1.0) * cover;
    if (d <= 0.0)
        return 0.0;
    // The edges eroded by finer cells, swirled by the body's own noise (wisps curl rather than tile), at two scales.
    vec3 swirl = (n.gba - 0.5) * 0.35;
    vec3 dq = (p + gDetailWind) * L.shape.y + swirl;
    vec3 e = textureLod(detailNoise, dq, 0.0).rgb;
    float ef = e.r * 0.625 + e.g * 0.25 + e.b * 0.125;
    ef = mix(ef, textureLod(detailNoise, dq.zxy * 1.37 + 0.31, 0.0).g, 0.35);   // (another scale, axes turned: no
                                                                               // repeat lines up)
    ef = mix(ef, 1.0 - ef, clamp(h * 4.0, 0.0, 1.0));   // wispy underneath, billowy above
    d = clamp(Remap(d, ef * L.shape.z, 1.0, 0.0, 1.0), 0.0, 1.0);
    return d * w.b;
}

// Lightning (rvk rain.cpp Lightning): the cloud round a strike lit from inside - by how far across from it (the
// strike under the storm, the flash in its cloud above), bluish white.
vec3 FlashInCloud(vec3 p)
{
    if (FL.flash.w <= 0.0)
        return vec3(0.0);
    vec2 off = p.xz - FL.flashWorld.xz;
    return vec3(0.8, 0.85, 1.0) * (FL.flash.w * 1.2 / (1.0 + dot(off, off) / (1500.0 * 1500.0)));
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
    vec2 q = p.xz + L.wind.xz + L.scud.xy;
    float broad = clamp((textureLod(shapeNoise, vec3(q / 14000.0, 0.47), 0.0).r - 0.54) / 0.26, 0.0, 1.0);
    float cover = smoothstep(1.0 - L.deck.x, 1.25 - L.deck.x * 0.6, broad * 0.5 + 0.5 * L.deck.x);
    // Scud (a thunderstorm): the deck torn into ragged fragments a few hundred units across, gaps between them.
    if (L.scud.z > 0.0) {
        float rag = textureLod(shapeNoise, vec3(q / 1800.0, 0.21 + h * 0.2), 0.0).r * 0.7 +
                    textureLod(shapeNoise, vec3(q / 600.0 + 0.3, 0.63), 0.0).g * 0.3;
        cover *= mix(1.0, smoothstep(0.5, 0.66, rag), L.scud.z);
    }
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
    vec3 sunL = normalize(FL.sunTrue.xyz + vec3(0.0, 1e-6, 0.0));   // (the sun's: below the horizon too)
    vec3 sun = SunLight(sunL) * 0.35 + Afterglow(sunL) * 0.45 + MoonLight() * 0.35;   // (its underside: the afterglow)
    vec3 light = vec3(0.0);
    for (int i = 0; i < kSteps && transmit > 0.02; ++i) {
        vec3 p = eye + dir * (t0 + (float(i) + jitter) * dt);
        float d = DeckDensity(p);
        if (d <= 0.001)
            continue;
        float h = (float(i) + 0.5) / float(kSteps);
        // (Seen from below: mostly its underside - darker than the layer's sunlit tops; thick parts darker still.)
        vec3 S = (L.ambient.rgb * mix(0.38, 0.8, h) * mix(0.55, 1.0, exp(-d * 2.0)) * NightSky() * (1.0 + Night()) *
                  mix(1.0, 0.7, MoonShare()) + sun) * L.tint.rgb * L.tint.w;
        const vec3 luma = vec3(0.3, 0.59, 0.11);   // (contrast, as the layer: RVK_CloudShade)
        S *= clamp(pow(max(dot(S, luma) / max(dot(L.tint.rgb, luma) * L.tint.w, 1e-3), 1e-3), L.horizon.y - 1.0), 0.15, 3.0);
        S += FlashInCloud(p) * L.tint.rgb;
        float st = exp(-d * sigma * dt);
        light += transmit * (1.0 - st) * S;
        transmit *= st;
    }
    // (Thick cloud wholly opaque: the march stops with 1-2% let through, enough of the sun's bright disc behind it to
    // show - user. What is left under 4% is none.)
    float alpha = 1.0 - max(transmit - 0.04, 0.0) / 0.96;
    if (alpha <= 0.0)
        return vec4(0.0);
    return Aerial(light, alpha, t0, dir);
}


#ifdef RVK_CLOUD_SUNRAY
// How much sunlight the clouds let through to the eye (rvk/clouds.cpp MakeCloudSunRay): 16 rays from the eye towards
// the sun (spread over ~2 degrees round it: a patch of sky, not one point), each marched through the layer and the
// low deck as the scene's are - in the world, whatever the camera looks at or what land is in front of it (measured
// on the screen, a wall before the sun left the clouds' cover unknown - user). Low, the frame's sun is dimmed by it.
void main()
{
    gWind = L.wind.xyz;
    gDetailWind = L.wind.xyz * 1.7 + vec3(0.0, L.morph.z, 0.0);   // (a storm's detail boiling up)
    vec3 eye = L.eye.xyz;
    float R = L.layer.z;
    gCentre = vec3(eye.x, eye.y - R, eye.z);
    gInner = R + (L.layer.x - eye.y);
    float outer = R + (L.layer.y - eye.y);
    gThick = outer - gInner;
    vec3 sunL = normalize(FL.sunTrue.xyz + vec3(0.0, 1e-6, 0.0));
    vec3 up = abs(sunL.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 u = normalize(cross(up, sunL)), v = cross(sunL, u);
    float i = floor(gl_FragCoord.x), rr = 0.018 * sqrt((i + 0.5) / 16.0), a = i * 2.3999632;
    vec3 dir = normalize(sunL + rr * (cos(a) * u + sin(a) * v));
    dir.y = max(dir.y, 0.005);                   // (a sun at or under the horizon: along it)
    dir = normalize(dir);
    float od = 0.0;
    // The layer: from leaving its inner sphere to leaving its outer one (the eye under it), or from the eye (in it).
    vec2 si = Sphere(eye, dir, gCentre, gInner), so = Sphere(eye, dir, gCentre, outer);
    float r0 = length(eye - gCentre);
    float t0 = r0 < gInner ? si.y : 0.0, t1 = min(so.y, L.layer.w);
    if (t1 > t0) {
        const int kSteps = 32;
        float dt = (t1 - t0) / float(kSteps);
        for (int k = 0; k < kSteps; ++k)
            od += Density(eye + dir * (t0 + dt * (float(k) + 0.5)), true) * dt;   // (as seen: the edges eroded)
    }
    // The low deck (below the layer).
    if (L.deck.x > 0.0) {
        float b = (L.deck.y - eye.y) / dir.y, c = (L.deck.z - eye.y) / dir.y;
        b = max(b, 0.0);
        c = min(c, L.layer.w);
        if (c > b) {
            float dt = (c - b) / 8.0;
            for (int k = 0; k < 8; ++k)
                od += DeckDensity(eye + dir * (b + dt * (float(k) + 0.5))) * dt * 0.9;
        }
    }
    float through = exp(-od * L.shape.w);
    outScene = vec4(through, through, through, 1.0);
}
#elif defined(RVK_CLOUD_SHADOW)
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
    const float kShadowMapSize = 1024.0;          // (rvk clouds.cpp kShadowSize)
    vec3 p = vec3(L.shadowArea.x + gl_FragCoord.x / kShadowMapSize * L.shadowArea.z, L.shadowArea.w,
                  L.shadowArea.y + gl_FragCoord.y / kShadowMapSize * L.shadowArea.z);
    // The clouds' own shadow, and finer (user's choice, 2026-10-13): their cells are about a kilometre - the land in
    // view lay wholly in one cloud's shadow or none, and the shadows weren't seen. Two things, both fixed in shape (a
    // threshold sweeping through noise as a cloud came near grew rings up a mountain, like slicing 3D noise - user):
    // - the clouds' shadow read at a point pushed about by noise (~60-150 units) moving with the clouds: ragged edges
    //   that travel with them;
    // - in the open, small shadows of their own (as many as the sky is cloudy overall, L.sub.w - steady), drifting
    //   with the clouds.
    gWind = vec3(L.wind.x, 0.0, L.wind.z);
    gDetailWind = gWind;
    vec3 c = p + sunL * ((L.layer.x + 0.35 * (L.layer.y - L.layer.x) - p.y) / sunL.y);
    vec4 warp = textureLod(shapeNoise, vec3((c.x + L.wind.x) * L.shape.x * 14.0, 0.21, (c.z + L.wind.z) * L.shape.x * 14.0), 0.0);
    vec3 pw = p + vec3(warp.g - 0.5, 0.0, warp.b - 0.5) * 260.0;
    const int kSamples = 8;
    float od = 0.0, span = (L.layer.y - L.layer.x) / float(kSamples);
    for (int k = 0; k < kSamples; ++k) {
        float y = L.layer.x + span * (float(k) + 0.5);
        od += Density(pw + sunL * ((y - pw.y) / sunL.y), true);   // (with the billows: the clouds' own edges)
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
    float shade = 1.0 - exp(-od * 0.25);          // (softened: thin cloud a light shade, thick cloud dark)
    {
        vec3 q = vec3((c.x + L.wind.x) * L.shape.x * 14.0, 0.43, (c.z + L.wind.z) * L.shape.x * 14.0);
        vec4 n = textureLod(shapeNoise, q, 0.0);
        // (Worley fbm at 4 and 8 cells a tile: values bunched in ~0.45..0.63 - the threshold that leaves `cover` of
        // the ground above it, fitted to their spread.)
        float v = n.g * 0.7 + n.b * 0.3;
        float t = 0.61 - 0.27 * (0.3 * L.sub.w);
        shade = max(shade, 0.6 * smoothstep(t - 0.012, t + 0.018, v));
    }
    float light = 1.0 - shade;
    // The game's sheets (RVK_CloudSheet, L.deck.w): a faint veil's shadow - fixed in the world, drifting with the wind
    // (the sheets themselves are kept round the eye: their own shadow would slide with the player), high up, fibrous.
    if (L.deck.w > 0.0) {
        vec3 q = p + sunL * ((L.layer.y + 1500.0 - p.y) / sunL.y) + gWind;   // (with the rest)
        float n = textureLod(detailNoise, vec3(q.x / 4000.0, 0.37, q.z / 1500.0), 0.0).r * 0.7 +
                  textureLod(detailNoise, vec3(q.z / 1700.0 + 0.3, 0.71, q.x / 900.0), 0.0).g * 0.3;
        light *= 1.0 - L.deck.w * smoothstep(0.38, 0.7, n);
    }
    outScene = vec4(light, light, light, 1.0);
}
#else
void main()
{
    gWind = L.wind.xyz;
    gDetailWind = L.wind.xyz * 1.7 + vec3(0.0, L.morph.z, 0.0);   // (a storm's detail boiling up)
    // The half-size pixel marched (one of each 2x2 when reused over frames).
    vec2 hp = L.sub.z > 1.5 ? floor(gl_FragCoord.xy) * 2.0 + L.sub.xy + 0.5 : gl_FragCoord.xy;
    // The four scene pixels under this one: none of them sky - an opaque surface is drawn there later.
    // Reused over frames, this pixel stands for its whole 2x2 (the others filtered from it): skipped only when no sky
    // is near that block - its 4 x 4 scene pixels and 2 round them, every other one. (Its own four alone: beside a
    // thin pole its pixel fell on the pole while the rest of the block was sky - no cloud filtered into that sky, a
    // light stepped outline down the pole's side; the user's dump.)
    if (L.weather.w > 0.5) {
        ivec2 last = textureSize(sceneDepth, 0) - 1;
        bool open = false;
        if (L.sub.z > 1.5) {
            ivec2 block = ivec2(floor(gl_FragCoord.xy)) * 4;
            for (int k = 0; k < 16 && !open; ++k) {
                ivec2 o = ivec2(k & 3, k >> 2) * 2 - 2;
                open = texelFetch(sceneDepth, clamp(block + o, ivec2(0), last), 0).r == L.wind.w;
            }
            for (int k = 0; k < 16 && !open; ++k) {
                ivec2 o = ivec2(k & 3, k >> 2) * 2 - 1;
                open = texelFetch(sceneDepth, clamp(block + o, ivec2(0), last), 0).r == L.wind.w;
            }
        } else {
            ivec2 at = ivec2(hp) * 2;
            for (int k = 0; k < 4; ++k)
                open = open || texelFetch(sceneDepth, min(at + ivec2(k & 1, k >> 1), last), 0).r == L.wind.w;
        }
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
    // (Blue noise, moved on each frame by the golden ratio - FL.taa.z - for the history to average: neighbours start
    // their marches as differently as can be, no lines of it in the clouds' edges.)
    vec2 blue = texelFetch(blueNoise, ivec2(hp) & 63, 0).rg;
    float jitter = fract(blue.r + FL.taa.z);
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
    vec3 sunL = normalize(FL.sunTrue.xyz + vec3(0.0, 1e-6, 0.0));   // (the sun's: below the horizon too)
    vec3 sunColour = SunLight(sunL);
    vec3 glow = Afterglow(sunL);
    // By night the moon in the sun's place (when its light is the greater): the march towards it, its forward lobe.
    vec3 moonColour = MoonLight();
    const vec3 kLuma = vec3(0.3, 0.59, 0.11);
    if (dot(moonColour, kLuma) > dot(sunColour, kLuma)) {
        sunL = MoonDir();
        sunColour = moonColour;
    }
    float cosT = dot(dir, sunL);
    float day = 1.0 - Night();
    // (By night the heaps' sky light twice the sheets': cloud dimmed inside, they came out greyer and darker than
    // the game's sheets round them - user. Under the moon less: the side away from it darker.)
    float nightSky = NightSky() * (1.0 + Night()) * mix(1.0, 0.7, MoonShare());
    float sigma = L.shape.w;
    float transmit = 1.0;
    vec3 light = vec3(0.0);
    float depthSum = 0.0, weightSum = 0.0;      // (where the cloud is along the ray: its haze)
    // (Step lengths by the layer's usual thickness, not a thunderstorm's three times taller: its steps three times as
    // long left its cloud in terraces - user's dump.)
    float baseThick = min(gThick, 1300.0);
    // (A storm's steps longer: its varied underside hangs closer, where steps are short - the cost grew with it; the
    // blue noise and the edge's small steps keep it smooth.)
    float growth = mix(0.012, 0.018, L.ambient.w);
    float minStep = baseThick * growth, along = (t1 - t0) / float(steps);
    float t = t0 + jitter * clamp(t0 * growth, minStep, along);
    float lightJitter = fract(blue.g + FL.taa.z);   // (the sun's steps moved too: no shells of light)
    // Adaptive steps (vertexfragment.com "volumetric cloud banding", after Horizon Zero Dawn): long steps through clear
    // air; at the first cloud a quarter of the step, three of them back (the edge lies before the sample), and small
    // steps on until kFine samples without cloud. Only the ray's first cloud - the outline seen: refining every gap
    // inside ragged storm cloud cost ~+0.9 ms in game.
    // (Inside the cloud back to long steps after kFineMax small ones: its edge is what bands, and thick cloud soon
    // stops the march anyway.)
    const int kFine = 3, kFineMax = 8;
    int fineLeft = 0, fineMax = 0;               // (small steps: samples left without cloud; small steps left at all)
    bool wasClear = true;                        // (the last sample clear air: the next cloud is an edge)
    bool refined = false;                        // (the first edge found)
    float odLast = -1.0;                         // (the last sample's way to the sun)
    for (int i = 0; i < steps + 8 && transmit > 0.01 && t < t1; ++i) {
        float big = clamp(t * growth, minStep, max(along, minStep));
        bool fine = fineLeft > 0 && fineMax > 0;
        float dt = fine ? big * 0.25 : big;
        vec3 p = eye + dir * t;
        float d = Density(p, t < 8000.0);        // (far off no detail: too small to see, half the cost)
        if (fine) --fineMax;
        if (d <= 0.001) {
            if (fine) --fineLeft;
            wasClear = true;
            t += fine ? dt : 2.0 * dt;           // (clear air: on twice as far)
            continue;
        }
        if (wasClear && !fine && !refined) {
            // The edge: back three small steps (still after the last, clear, sample) and look again from there.
            wasClear = false;
            refined = true;
            fineLeft = kFine;
            fineMax = kFineMax;
            t = max(t - 3.0 * big * 0.25, t0);
            continue;
        }
        wasClear = false;
        if (fine) fineLeft = kFine;
        t += dt;
        float tHere = t - dt;
        // The sun through the cloud: a few steps towards it, longer each (far off, fewer and longer). (Small steps at
        // an edge: every other one keeps the last - a quarter step away, the light hardly differs.)
        bool reuse = fine && (fineMax & 1) == 1 && odLast >= 0.0;
        float od = reuse ? odLast : 0.0;
        bool far = tHere > 8000.0;
        int lightSteps = far ? 2 : int(L.view.w) - (L.ambient.w > 0.5 ? 1 : 0);   // (a storm hides the sun: one less)
        float ls = baseThick * (far ? 0.1 : 0.045), grow = far ? 2.2 : (lightSteps < int(L.view.w) ? 1.75 : 1.5);
        vec3 lp = p + sunL * (ls * (lightJitter - 0.5));
        gPushKept = true;
        for (int k = 0; k < lightSteps && !reuse; ++k) {
            lp += sunL * ls;
            od += Density(lp, false) * ls;
            ls *= grow;
        }
        gPushKept = false;
        if (!reuse) od *= sigma;
        odLast = od;
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
        // By day the light bounced about deep inside (more orders, all ways): undersides and the side away from the sun
        // not dark grey against a bright sky (user: lighter by day).
        sun += 0.3 * exp(-od * 0.06) * day;
        // Powder: an edge facing the sun is darker than the inside it lights (light enters, doesn't come back out).
        float powder = 1.0 - 0.5 * exp(-d * sigma * gThick * 0.08);
        float h = Height(p);
        // The sky's light: the sky's colour from above (lighter at the tops), less and warmer from below (the ground
        // lit by it), dimmed by the cloud over the point (the sun's way through it stands in for up: dark storm bases).
        vec3 skyTop = L.ambient.rgb * 1.15, skyLow = L.ambient.rgb * vec3(0.66, 0.62, 0.58);
        // (Dimmed by the cloud towards the sun only over a short way - from far off, the cloud's shadows lie through
        // it as long streaks all one way.)
        vec3 sky = mix(skyLow * mix(1.0, 0.75, L.ambient.w), skyTop, smoothstep(0.0, 0.8, h)) *
                   mix(mix(0.4, 0.6, day), 1.0, exp(-od * 0.3));
        sky += glow * (1.0 - smoothstep(0.0, 0.7, h)) * 0.9;   // (round sunset: the pink undersides)
        sky *= nightSky;
        // (The game's colour - its puffs as it shows them, fog and all - wholly in the sky's light, half in the sun's:
        // a storm takes the game's colour, sunlit tops stay bright.)
        vec3 S = (sunColour * sun * powder * 1.5 * mix(L.tint.rgb, vec3(dot(L.tint.rgb, vec3(0.3, 0.59, 0.11))), 0.5) +
                  sky * L.tint.rgb) * L.tint.w;
        // Contrast (RVK_CloudShade): how far from the cloud's middle light (its colour as the game shows it) a part
        // is, made more - shaded parts darker, lit parts lighter - its hue kept.
        const vec3 luma = vec3(0.3, 0.59, 0.11);
        float middle = max(dot(L.tint.rgb, luma) * L.tint.w, 1e-3);
        S *= clamp(pow(max(dot(S, luma) / middle, 1e-3), (L.horizon.y - 1.0) / nightSky), 0.15, 3.0);
        S += FlashInCloud(p) * L.tint.rgb;       // (lightning in the cloud)
        float stepT = exp(-d * sigma * dt);
        light += transmit * (1.0 - stepT) * S;
        depthSum += transmit * (1.0 - stepT) * tHere;
        weightSum += transmit * (1.0 - stepT);
        transmit *= stepT;
    }
    // (Thick cloud wholly opaque: the march stops with 1-2% let through, enough of the sun's bright disc behind it to
    // show - user. What is left under 4% is none.)
    float alpha = 1.0 - max(transmit - 0.04, 0.0) / 0.96;
    if (alpha <= 0.0) {
        outScene = deck;
        return;
    }
    // The air: by where along the ray the cloud is.
    vec4 layer = Aerial(light, alpha, depthSum / max(weightSum, 1e-6), dir);
    outScene = deck + (1.0 - deck.a) * layer;
}
#endif
