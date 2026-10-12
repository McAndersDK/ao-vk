#version 450
// Smoke (rvk/smoke.cpp RenderSmoke): a box's faces (36 vertices, no buffers); smoke.frag keeps only the far ones, so
// each pixel the box covers is marched once - whether the camera is outside the box or in it.
#include "smoke_common.glsl"

layout(location = 0) out vec3 vWorld;

void main()
{
    const vec3 corners[8] = vec3[](vec3(0, 0, 0), vec3(1, 0, 0), vec3(1, 1, 0), vec3(0, 1, 0), vec3(0, 0, 1), vec3(1, 0, 1),
                                   vec3(1, 1, 1), vec3(0, 1, 1));
    const int faces[36] = int[](0, 2, 1, 0, 3, 2,  4, 5, 6, 4, 6, 7,  0, 1, 5, 0, 5, 4,  3, 7, 6, 3, 6, 2,
                                0, 4, 7, 0, 7, 3,  1, 2, 6, 1, 6, 5);
    vec3 p = S.box.xyz + corners[faces[gl_VertexIndex]] * S.box.w;
    vWorld = p;
    gl_Position = S.viewProj * vec4(p, 1.0);
    // (A box round the camera: its far faces stay in front of the near plane.)
    gl_Position.z = min(gl_Position.z, gl_Position.w * 0.999999);
    if (S.camera.w > 0.5)
        gl_Position.xy += FL.taa.xy * gl_Position.w;
}
