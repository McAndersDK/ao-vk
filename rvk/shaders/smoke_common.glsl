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
    vec4 detail;        // the wisps finer than the grid, carried by the flow: x how strong; y their cycle (s); z how
                        // fine (per world unit); w a seed
    vec4 look;          // x: how opaque it is drawn (RVK_SmokeOpaque: its finished colour and cover, x); y: how far
                        // from the state a step before to the latest (the simulation steps less often than frames)
    uvec4 info;         // x: its slot in the bounds; y: lamps; z: cells a side
    uvec4 lamps[2];     // the lamps reaching the box (indices in FL.lights)
} S;

// The cells with smoke (smoke_sim.comp stage 7): [2 slot] the lowest, [2 slot + 1] the highest; empty: lowest > highest.
layout(set = 0, binding = 6, std430) readonly buffer SmokeBounds { uvec4 bounds[]; } SB;

const float kSmokeFlowMax = 4.0;                // the flow image's range (world / s; rvk/smoke.cpp kFlowMax)

float SmokeViewZ(float d) { return S.proj.y / (min(d, 0.9999999) - S.proj.x); }
