#version 450
// The terrain's shade from a low sun (rvk/terrain_sun.cpp): for each cell of a height grid round the eye (the game's
// terrain as drawn), how high a point there must be to see the sun over the terrain - the highest of each cell
// towards the sun, less the sun's climb over the way to it. A point lower is in the terrain's shade. Shadow maps can't
// give it low in the sky (their shadows hundreds of metres long; faded out under ~10 degrees): there a valley lay
// lit gold under the mountains hiding the sun (user, 2026-10-13). G: each cell's own ground (the sand fog).
layout(push_constant) uniform Params {
    vec4 sun;           // xy: towards the sun along the ground (unit, grid x / y = world x / z), z: its climb per
                        // cell (world y), w: steps (cells)
    vec4 grid;          // x: cells a side
} P;
layout(std430, binding = 0) readonly buffer Heights { float h[]; } H;   // (world y; < -1e8: never drawn)

layout(location = 0) out vec4 outScene;

void main()
{
    int n = int(P.grid.x);
    vec2 at = floor(gl_FragCoord.xy) + 0.5;
    float need = -60000.0;
    int steps = int(P.sun.w);
    for (int k = 1; k <= steps; ++k) {
        ivec2 c = ivec2(floor(at + P.sun.xy * float(k)));
        if (c.x < 0 || c.y < 0 || c.x >= n || c.y >= n)
            break;
        float y = H.h[c.y * n + c.x];
        if (y > -1e8)
            need = max(need, y - P.sun.z * float(k));
    }
    // (G: the cell's own ground - the sand fog's height over the ground; -60000 never drawn.)
    ivec2 own = ivec2(at);
    float ground = H.h[own.y * n + own.x];
    outScene = vec4(clamp(need, -60000.0, 60000.0), ground > -1e8 ? ground : -60000.0, 0.0, 1.0);
}
