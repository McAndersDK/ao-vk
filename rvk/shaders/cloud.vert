#version 450
// Clouds (rvk/clouds.cpp): a puff's box (36 vertices, no buffers) in its mesh space; cloud.frag keeps only the far faces,
// so each pixel the box covers is marched once.
#include "clouds_common.glsl"

layout(location = 0) out vec3 vLocal;

void main()
{
    const vec3 corners[8] = vec3[](vec3(0, 0, 0), vec3(1, 0, 0), vec3(1, 1, 0), vec3(0, 1, 0), vec3(0, 0, 1), vec3(1, 0, 1),
                                   vec3(1, 1, 1), vec3(0, 1, 1));
    const int faces[36] = int[](0, 2, 1, 0, 3, 2,  4, 5, 6, 4, 6, 7,  0, 1, 5, 0, 5, 4,  3, 7, 6, 3, 6, 2,
                                0, 4, 7, 0, 7, 3,  1, 2, 6, 1, 6, 5);
    vec3 p = mix(C.low.xyz, C.high.xyz, corners[faces[gl_VertexIndex]]);
    vLocal = p;
    gl_Position = C.viewProj * (C.world * vec4(p, 1.0));
    // (Its far faces kept inside the depth range: the sky's puffs lie near its far plane.)
    gl_Position.z = clamp(gl_Position.z, 0.0, gl_Position.w * 0.999999);
    if (C.eye.w > 0.5)
        gl_Position.xy += FL.taa.xy * gl_Position.w;
}
