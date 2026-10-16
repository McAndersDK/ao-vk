// Clouds (rvk/clouds.cpp DrawClouds): the per-puff block, shared by cloud.vert and cloud.frag. A puff's own "mesh space"
// is the game's: its mesh's coordinates, before its world matrix.
#include "frame_lights.glsl"                    // FL (binding 4): the TAA's jitter and noise, the sun

layout(set = 0, binding = 0) uniform CloudFrame {
    mat4 viewProj;      // world -> clip (the sky's view; D3D row-vector convention, as the scene's)
    mat4 world;         // mesh -> world
    mat4 invWorld;      // world -> mesh
    vec4 eye;           // xyz: the eye in mesh space; w: 1 = apply the TAA jitter
    vec4 low;           // xyz: its box's lowest corner (mesh space); w: the thin axis (0..2)
    vec4 high;          // xyz: the highest; w: the march's steps
    vec4 uvU, uvV;      // the texture's coordinates: dot(xyz, p) + w
    vec4 colour;        // rgb: its colour (as the game lit it); a: its opacity
    vec4 sun;           // xyz: towards the sun, mesh space (not unit: normalise); w: unused
    vec4 sunColour;     // rgb: the sun's colour; w: the density (RVK_CloudThick)
    vec4 ambient;       // rgb: the sky's light; w: time (s)
    vec4 noise;         // xyz: the noise's drift; w: how much it frays the puff (RVK_CloudDetail)
    vec4 fogColour;     // rgb; w: 1 = the game drew it fogged
    vec4 fogParams;     // start, end, density, table mode (D3DFOG_*)
    vec4 distance;      // x: its middle's view depth; y: the pre-pass's clear depth; z: 1 = skip where the pre-pass's
                        // depth isn't that (half size: an opaque surface is drawn there later)
} C;
