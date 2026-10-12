// The rain (rvk/rain.cpp): the per-draw block, shared by rain.vert and rain.frag.
#include "frame_lights.glsl"                    // FL (binding 4): the sun, the lamps, the TAA jitter

layout(set = 0, binding = 0) uniform RainFrame {
    mat4 viewProj;      // world -> clip (D3D row-vector convention, as the scene's)
    mat4 prevViewProj;  // ... last frame (motion vectors)
    mat4 view;          // world -> view
    vec4 camera;        // xyz: the eye; w: 1 = apply the TAA jitter
    vec4 time;          // x: the rain's clock (s); y: last frame's; z: how hard it rains (0..1); w: how many drops
    vec4 fall;          // xyz: the drops' velocity (world, units / s); w: the box's half size round the camera
    vec4 colour;        // rgb: the drops' tint (x RVK_RainBright); w: their opacity
    vec4 ambient;       // rgb: the terrain's ambient light
    vec4 fogColour;     // rgb; w: 1 = fog on
    vec4 fogParams;     // start, end, density, table mode (D3DFOG_*: 1 exp, 2 exp2, 3 linear; 0 = none)
    vec4 viewport;      // width, height, 1 / width, 1 / height
    vec4 proj;          // D3D projection m[2][2], m[3][2], m[0][0], m[1][1] (the splashes: depth -> view)
} R;

// The linear view depth of a depth buffer value.
float RainViewZ(float d) { return R.proj.y / (min(d, 0.9999999) - R.proj.x); }

// The light on a drop: the ambient, a little of the sun, much more looking towards it through the rain; the lamps
// near likewise.
vec3 RainLight(vec3 p)
{
    vec3 V = normalize(R.camera.xyz - p);
    vec3 L = -normalize(FL.sunDir.xyz + vec3(0.0, -1e-6, 0.0));
    vec3 light = R.ambient.rgb * 0.55 + FL.sunColor.rgb * (0.15 + 1.2 * pow(max(dot(-V, L), 0.0), 8.0));
    uint count = min(FL.info.x, 64u);
    for (uint i = 0u; i < count; ++i) {
        Light li = FL.lights[i];
        uint type = uint(li.position.w + 0.5);
        if (type != 1u && type != 2u)
            continue;
        vec3 dl = li.position.xyz - p;
        float d = length(dl);
        if (d >= li.direction.w || d < 1e-3)
            continue;
        float att = 1.0 / max(li.atten.x + li.atten.y * d + li.atten.z * d * d, 1e-3);
        att *= 1.0 - smoothstep(0.75, 1.0, d / li.direction.w);
        light += li.diffuse.rgb * (att * (0.3 + 2.5 * pow(max(dot(-V, dl / d), 0.0), 6.0)));
    }
    return light;
}

uint RainHash(uint x)
{
    x ^= x >> 16u;
    x *= 0x7FEB352Du;
    x ^= x >> 15u;
    x *= 0x846CA68Bu;
    x ^= x >> 16u;
    return x;
}
float RainRand(inout uint s)
{
    s = RainHash(s);
    return float(s >> 8u) * (1.0 / 16777216.0);
}

// The game's fog at a view distance (1 = none).
float RainFog(float d)
{
    if (R.fogColour.w < 0.5)
        return 1.0;
    uint mode = uint(R.fogParams.w);
    if (mode == 3u) return clamp((R.fogParams.y - d) / max(R.fogParams.y - R.fogParams.x, 1e-3), 0.0, 1.0);
    if (mode == 1u) return clamp(exp(-R.fogParams.z * d), 0.0, 1.0);
    if (mode == 2u) { float x = R.fogParams.z * d; return clamp(exp(-x * x), 0.0, 1.0); }
    return 1.0;
}
