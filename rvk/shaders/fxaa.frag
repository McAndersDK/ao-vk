#version 450
// FXAA (hdr.cpp, RVK_Fxaa: the anti-aliasing when TAA is off): on the tone mapped image, a pixel on an edge -
// a brightness step among its diagonal neighbours - is replaced by samples taken along the edge, so stair steps
// soften without the image going soft where there are none. One pass, no history: nothing smears.
layout(set = 0, binding = 0) uniform sampler2D src;     // tone mapped (display) colours, linear sampler
layout(push_constant) uniform Push {
    vec4 size;          // 1 / width, 1 / height, unused, unused
} P;
layout(location = 0) out vec4 outColor;

const float kReduceMul = 1.0 / 8.0;     // how much a bright neighbourhood shortens the search
const float kReduceMin = 1.0 / 128.0;
const float kSpanMax = 8.0;             // the furthest along an edge it samples, pixels

float Luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

void main()
{
    vec2 rcp = P.size.xy, uv = gl_FragCoord.xy * rcp;
    vec3 m = texture(src, uv).rgb;
    float lNW = Luma(texture(src, uv + vec2(-0.5, -0.5) * rcp).rgb), lNE = Luma(texture(src, uv + vec2(0.5, -0.5) * rcp).rgb);
    float lSW = Luma(texture(src, uv + vec2(-0.5, 0.5) * rcp).rgb), lSE = Luma(texture(src, uv + vec2(0.5, 0.5) * rcp).rgb);
    float lM = Luma(m);
    float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE))), lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));
    if (lMax - lMin < max(0.0312, lMax * 0.125)) { outColor = vec4(m, 1.0); return; }   // no edge here
    // The edge's direction: across the brightness gradient.
    vec2 dir = vec2(-((lNW + lNE) - (lSW + lSE)), (lNW + lSW) - (lNE + lSE));
    float reduce = max((lNW + lNE + lSW + lSE) * 0.25 * kReduceMul, kReduceMin);
    float scale = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);
    dir = clamp(dir * scale, vec2(-kSpanMax), vec2(kSpanMax)) * rcp;
    // Two samples near the pixel along the edge, and two further out; the wider pair only if it stays within the
    // neighbourhood's range (else it crossed into something else).
    vec3 a = 0.5 * (texture(src, uv + dir * (1.0 / 3.0 - 0.5)).rgb + texture(src, uv + dir * (2.0 / 3.0 - 0.5)).rgb);
    vec3 b = a * 0.5 + 0.25 * (texture(src, uv - dir * 0.5).rgb + texture(src, uv + dir * 0.5).rgb);
    float lB = Luma(b);
    outColor = vec4((lB < lMin || lB > lMax) ? a : b, 1.0);
}
