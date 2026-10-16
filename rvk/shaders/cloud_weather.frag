#version 450
// The cloud layer's weather map (rvk/clouds.cpp MakeCloudWeather): top down round the camera, what the layer holds
// where - R coverage (0 clear .. 1 overcast), G cloud type (0 flat stratus .. 0.5 cumulus .. 1 towering storm cloud),
// B density, A the clouds' base (above the layer's). A drifting field of clouds fixed in the world, as much of the sky covered as the game's sky is cloudy.
layout(set = 0, binding = 0) uniform WeatherFrame {
    vec4 area;          // the map's corner x, z (world), its size (world), its size (pixels)
    vec4 info;          // x: time (s), y: how cloudy the sky is (0..1), z: the type, w: the density
    vec4 drift;         // xy: how far the field has drifted (world), z: the field's scale (world units a noise tile),
                        // w: RVK_CloudCover (a bias, -1..1)
    vec4 storm;         // x: a thunderstorm (0..1): the cover sealed - one heavy mass, no gaps - out to y (world units
                        // from the eye), beyond: its towering cells (cumulonimbus, type 1)
} W;
layout(set = 0, binding = 1) uniform sampler3D noiseTexture;   // the layer's shape noise (cloud_layer_noise.comp: r
                                                                // Perlin-Worley, g b a Worley fbm 4, 8, 16 cells)

layout(location = 0) out vec4 outWeather;

// The noise's values (bunched: 80% of them between lo and hi) spread over 0.1 .. 0.9.
float Spread(float v, float lo, float hi) { return clamp(0.1 + 0.8 * (v - lo) / (hi - lo), 0.0, 1.0); }

void main()
{
    vec2 p = W.area.xy + gl_FragCoord.xy / W.area.w * W.area.z;
    float cloudy = clamp(W.info.y + W.drift.w, 0.0, 1.0);
    // The field: two scales of drifting noise; the more cloudy, the lower its cut (overcast leaves few gaps).
    vec2 q = (p + W.drift.xy) / W.drift.z;
    float n = texture(noiseTexture, vec3(q, W.info.x * 0.0004)).r * 0.65 +
              texture(noiseTexture, vec3(q * 2.9 + 0.37, 0.5 + W.info.x * 0.0007)).g * 0.35;
    n = Spread(n, 0.49, 0.70);
    float cut = 1.0 - cloudy;
    float cover = smoothstep(cut - 0.15, cut + 0.25, n) * smoothstep(0.0, 0.15, cloudy);
    // Overcast: covered everywhere - but unevenly, thick dark masses and thinner lighter stretches (an even cover is one
    // flat grey ceiling from below), never wholly (the shape noise still breaks it into cloud).
    float overcast = smoothstep(0.7, 1.0, cloudy);
    float soft = Spread(texture(noiseTexture, vec3(q * 1.7 + 0.53, 0.61 + W.info.x * 0.0003)).r, 0.54, 0.80);   // (smooth: no sharp
    cover = mix(cover, 0.62 + 0.36 * smoothstep(0.2, 0.8, soft), overcast);                // ridges to stand as walls)
    // A thunderstorm: sealed over the eye - heavy masses and thinner stretches between - and beyond, its towering
    // cells standing apart (cumulonimbus: type 1, the layer's full height, an anvil spreading at the top - the deck
    // over the eye keeps to its lower part: type 0.95).
    float fromEye = length(p - (W.area.xy + 0.5 * W.area.z));
    float edge = smoothstep(W.storm.y, W.storm.y * 1.35, fromEye);
    float sealed = W.storm.x * (1.0 - edge), cells = W.storm.x * edge;
    float type = W.info.z;
    cover = mix(cover, 0.78 + 0.22 * smoothstep(0.2, 0.8, soft), sealed);
    if (cells > 0.0) {
        float c = Spread(texture(noiseTexture, vec3(q * 0.55 + 1.7, 0.33 + W.info.x * 0.0002)).r, 0.5, 0.75);
        float tower = smoothstep(0.5, 0.72, c);
        cover = mix(cover, max(cover * 0.35, tower), cells);
        type = mix(type, 1.0, cells * step(0.05, tower));
    }
    if (sealed > 0.0) type = mix(type, min(type, 0.95), sealed / max(W.storm.x, 1e-3));
    // A: how far above the layer's base the clouds start here - an uneven underside (seen from below, an overcast
    // is lumps and folds at different heights, lit differently, not one flat ceiling).
    float base = Spread(texture(noiseTexture, vec3(q * 3.3 + 0.71, 0.27 + W.info.x * 0.0003)).r, 0.54, 0.80);
    outWeather = vec4(clamp(cover, 0.0, 1.0), type, W.info.w, base);
}
