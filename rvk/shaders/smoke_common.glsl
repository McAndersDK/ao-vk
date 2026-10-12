// Smoke (rvk/smoke.cpp RenderSmoke): the per-draw block, shared by smoke.vert and smoke.frag.
#include "frame_lights.glsl"                    // FL (binding 4): the sun, the lamps, the TAA's jitter and noise

layout(set = 0, binding = 0) uniform SmokeFrame {
    mat4 viewProj;      // world -> clip (D3D row-vector convention, as the scene's)
    mat4 view;          // world -> view
    vec4 camera;        // xyz: the eye; w: 1 = apply the TAA jitter
    vec4 box;           // xyz: the box's lowest corner (world); w: its size (world)
    vec4 colour;        // rgb: the smoke's colour (albedo); w: its extinction (per world unit at density 1)
    vec4 ambient;       // rgb: the terrain's ambient light; w: steps of the march
    vec4 proj;          // D3D projection m[2][2], m[3][2], m[0][0], m[1][1]
    vec4 viewport;      // width, height, 1 / width, 1 / height
    vec4 fogColour;     // rgb; w: 1 = fog on
    vec4 fogParams;     // start, end, density, table mode (D3DFOG_*)
} S;

float SmokeViewZ(float d) { return S.proj.y / (min(d, 0.9999999) - S.proj.x); }
