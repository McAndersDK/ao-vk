#version 450
// The sand fog (sand_fog.frag; hdr.cpp): separable blur at half resolution of its light and transmittance - smooth, so
// neighbours mix in unless they lie at a very different depth (a near thing's edge against the far dust).
layout(set = 0, binding = 0) uniform sampler2D src;       // the fog: light, transmittance
layout(set = 0, binding = 1) uniform sampler2D depthTex;  // full resolution scene depth (the fog's texel x 2)
layout(push_constant) uniform Push {
    vec4 params;        // direction xy (texels), tap spacing; w unused
    vec4 proj;          // D3D projection m[2][2], m[3][2]
} P;
layout(location = 0) out vec4 outFog;

float ViewZ(ivec2 c)
{
    float d = min(texelFetch(depthTex, c * 2, 0).r, 0.999999);
    return P.proj.y / min(d - P.proj.x, -1e-9);
}

void main()
{
    ivec2 c = ivec2(gl_FragCoord.xy), size = textureSize(src, 0);
    vec4 sum = texelFetch(src, c, 0);
    float z = ViewZ(c), weight = 1.0;
    for (int i = -4; i <= 4; ++i) {
        if (i == 0) continue;
        ivec2 q = clamp(c + ivec2(P.params.xy * P.params.z) * i, ivec2(0), size - 1);
        float zq = ViewZ(q);
        float w = exp(-float(i * i) / 8.0) * exp(-abs(zq - z) * 2.0 / max(min(zq, z), 1e-3));
        sum += texelFetch(src, q, 0) * w;
        weight += w;
    }
    outFog = sum / weight;
}
