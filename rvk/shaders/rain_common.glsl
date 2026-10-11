// The rain (rvk/rain.cpp): the per-draw block, shared by rain.vert and rain.frag.
#include "frame_lights.glsl"                    // FL (binding 4): the sun, the lamps, the TAA jitter

layout(set = 0, binding = 0) uniform RainFrame {
    mat4 viewProj;      // world -> clip (D3D row-vector convention, as the scene's)
    mat4 prevViewProj;  // ... last frame (motion vectors)
    mat4 view;          // world -> view
    vec4 camera;        // xyz: the eye; w: 1 = apply the TAA jitter
    vec4 time;          // x: the rain's clock (s); y: last frame's; z: how hard it rains (0..1); w: how many drops
    vec4 fall;          // xyz: the drops' velocity (world, units / s); w: the box's half size round the camera
    vec4 colour;        // rgb: the drops' tint; w: their opacity
    vec4 ambient;       // rgb: the terrain's ambient light
    vec4 fogColour;     // rgb; w: 1 = fog on
    vec4 fogParams;     // start, end, density, table mode (D3DFOG_*: 1 exp, 2 exp2, 3 linear; 0 = none)
    vec4 viewport;      // width, height, 1 / width, 1 / height
} R;

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
