#version 450
// The clouds' shadow map (rvk/clouds.cpp MakeCloudShadow): top down, in the cloud layer's plane round the camera - the
// sunlight through it (1 clear). Each puff a round footprint on the layer, its edge broken up by the clouds' noise
// (drifting), as opaque as the puff.
layout(set = 0, binding = 0) uniform ShadowFrame {
    vec4 area;          // the map's corner x, z (world), its size (world), the puffs
    vec4 info;          // x: time (s); y: the map's pixels a side; z: how cloudy the sky is (0..1); w: the field's scale
                        // (world units a noise tile)
    vec4 drift;         // xy: how far the field has drifted (world), zw: unused
    vec4 puff[64];      // x z: the footprint's middle (world), its radius, its opacity
} S;
layout(set = 0, binding = 1) uniform sampler3D noiseTexture;

layout(location = 0) out vec4 outLight;

void main()
{
    vec2 p = S.area.xy + gl_FragCoord.xy / S.info.y * S.area.z;
    float light = 1.0;
    // The sky's clouds overall: a drifting field of shadows, as much of the ground under them as the sky is cloudy
    // (the game's puffs are not where their shadows would be - only those right overhead are placed, below).
    float cloudy = S.info.z;
    if (cloudy > 0.0) {
        vec2 q = (p + S.drift.xy) / S.info.w;
        float n = texture(noiseTexture, vec3(q, S.info.x * 0.0015)).r * 0.65 + texture(noiseTexture, vec3(q * 2.7 + 0.31, 0.5)).g * 0.35;
        float cut = 1.0 - cloudy * 0.75;
        light *= 1.0 - smoothstep(cut - 0.06, cut + 0.12, n) * min(0.4 + cloudy, 0.9);
    }
    int count = int(S.area.w);
    for (int i = 0; i < count; ++i) {
        vec4 f = S.puff[i];
        float d = length(p - f.xy) / f.z;
        if (d >= 1.3)
            continue;
        float n = texture(noiseTexture, vec3(p * (1.5 / f.z) + vec2(float(i) * 0.37, S.info.x * 0.004), float(i) * 0.13)).r;
        float shape = 1.0 - smoothstep(0.5, 1.1, d + (n - 0.5) * 0.6);
        light *= 1.0 - f.w * shape;
    }
    outLight = vec4(light, light, light, 1.0);
}
