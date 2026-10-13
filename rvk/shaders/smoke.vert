#version 450
// Smoke (rvk/smoke.cpp RenderSmoke): a box's faces (36 vertices, no buffers); smoke.frag keeps only the far ones, so
// each pixel the box covers is marched once - whether the camera is outside the box or in it. The box drawn is the part
// of the volume with smoke in it (its bounds, a cell more round them for the filtering); none: nothing drawn.
#include "smoke_common.glsl"

layout(location = 0) out vec3 vWorld;
layout(location = 1) flat out vec3 vLow;
layout(location = 2) flat out vec3 vHigh;

void main()
{
    const vec3 corners[8] = vec3[](vec3(0, 0, 0), vec3(1, 0, 0), vec3(1, 1, 0), vec3(0, 1, 0), vec3(0, 0, 1), vec3(1, 0, 1),
                                   vec3(1, 1, 1), vec3(0, 1, 1));
    const int faces[36] = int[](0, 2, 1, 0, 3, 2,  4, 5, 6, 4, 6, 7,  0, 1, 5, 0, 5, 4,  3, 7, 6, 3, 6, 2,
                                0, 4, 7, 0, 7, 3,  1, 2, 6, 1, 6, 5);
    uint slot = S.info.x * 2u;
    uvec3 lowCell = SB.bounds[slot].xyz, highCell = SB.bounds[slot + 1u].xyz;
    float n = float(S.info.z);
    vec3 a = vec3(0.0), b = vec3(0.0);
    if (lowCell.x <= highCell.x) {
        a = clamp((vec3(lowCell) - 1.0) / n, 0.0, 1.0);
        b = clamp((vec3(highCell) + 2.0) / n, 0.0, 1.0);
    }
    vLow = S.box.xyz + a * S.box.w;
    vHigh = S.box.xyz + b * S.box.w;
    vec3 p = mix(vLow, vHigh, corners[faces[gl_VertexIndex]]);
    vWorld = p;
    gl_Position = S.viewProj * vec4(p, 1.0);
    // (A box round the camera: its far faces stay in front of the near plane.)
    gl_Position.z = min(gl_Position.z, gl_Position.w * 0.999999);
    if (S.camera.w > 0.5)
        gl_Position.xy += FL.taa.xy * gl_Position.w;
}
