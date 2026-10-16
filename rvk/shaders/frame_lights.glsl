// The per-frame light block (binding 4), shared by the scene shaders (constants.glsl) and the volumetric light.
// Must match FrameLights in rvk/internal.h (std140).
struct Light {
    vec4 diffuse, specular, ambient;
    vec4 position;      // xyz world space, w = D3DLIGHTTYPE
    vec4 direction;     // xyz world space, w = range
    vec4 atten;         // attenuation0, attenuation1, attenuation2, falloff
    vec4 spot;          // cos(theta/2), cos(phi/2), frame lights: point shadow cube + 1 (0 = none), its fade-in (0..1)
};

// The frame's active point / spot lights nearest the camera (Device::SetLightOverride). Replaces the game's
// per-object choice of up to 8 lights, which drops lights on big objects such as the ground.
#ifndef FL_BINDING
#define FL_BINDING 4            // (the tone mapping reads it at 15)
#endif
layout(set = 0, binding = FL_BINDING, std140) uniform FrameLights {
    uvec4 info;                 // light count, pusher count
    mat4 shadowViewProj[4];     // world -> each sun shadow cascade (raw D3DMATRIX memory)
    vec4 cascadeTexel;          // world size of a texel of each cascade
    vec4 cascadeDepth;          // world units per unit of each cascade's depth (soft shadows)
    vec4 effects;               // light through leaves, night glow (x darkness), sun shadow softness, plant push
    vec4 wind;                  // plants' sway: direction x, z, time (s), strength
    vec4 taa;                   // temporal anti-aliasing: this frame's jitter (clip x, y per w), noise offset (0..1), the wind's time last frame
    vec4 shadowParams;          // enabled, strength, cascade count, point light shadow strength
    vec4 sunDir;                // direction the sunlight travels; w = light headroom (F_OVERBRIGHT)
    vec4 sunColor;              // the sun's colour (shadow-casting sun; 0 = none)
    vec4 pbr;                   // PBR materials: direct specular, ambient specular, reflections, occlusion map strength
    vec4 pbr2;                  // x: debug view (0 = off), y: 1 = PBR on, z: emissive maps' brightness, w: the
                                // environment probe (its bits: atlas slot + 1, sampler slot << 20, strength / 15 << 28)
    mat4 prevView, prevProj;    // the world camera last frame (motion vectors; raw D3DMATRIX memory)
    vec4 pushers[16];           // what plants bend away from (characters and their trails): world xyz, seconds since
    vec4 pusherBorn[4];         // ... and seconds since each point was made (4 a vector)
    vec4 leaves;                // leaves (leaves.cpp): branch sway, sprig flutter, gusts, canopy core cut
    vec4 leafView;              // ... their distance (none beyond; thinning out from half of it), on, falling leaves
    vec4 clouds;                // the clouds' shadow map (rvk clouds.cpp): corner x, z, 1 / size, its bindless image + 1 |
                                // sampler << 20 (as the float's bits; 0 = none)
    vec4 clouds2;               // ... the cloud layer's height (world y), the shadow's strength
    vec4 cloudTint;             // the game's cloud sheets with the layer's light (cloud_light.glsl): their colour (w
    vec4 cloudSky;              // brightness); the sky's light (w: the sheets' strength, 0 none); the air's colour (w:
    vec4 cloudAir;              // the horizon's haze)
    vec4 moon;                  // the moonlight (rvk night.cpp): its colour on the ground and grass (0 = none); w: 1 =
                                // the shadow map is the moon's (sunDir: the moon's light), not the sun's
    vec4 sunTrue;               // towards the sun, below the horizon too (the clouds' light); w: how much it is night
    vec4 terrainSun;            // the terrain's shade from a low sun (rvk terrain_sun.cpp): map corner x, z, 1 / size,
                                // bindless image + 1 | sampler << 20 (the float's bits; 0 = none)
    vec4 terrainSun2;           // ... x: the eye's own (0 shaded .. 1), y: its weight (0 high sun .. 1 low)
    vec4 windFrame;             // xy: the wind's patterns' frame (wind.glsl WindAxes, rvk Device::WindFrame); z: a sandstorm's spreading of the sun's light (0 .. 0.75)
    // Lightning (rvk rain.cpp Lightning): this frame's flash - its light on the land (rgb; 0 none), w: the glow in the
    // clouds round the strike, x; towards the strike in view space (xyz), w: how far (world units); the projection's
    // m[0][0], m[1][1], m[2][2], m[3][2] (the tone mapping's view rays).
    vec4 flash;
    vec4 flashView;
    vec4 flashProj;
    vec4 flashWorld;            // ... the strike (world xyz: the storm's fog lit round it)
    Light lights[64];
} FL;
