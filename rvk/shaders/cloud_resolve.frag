#version 450
// The cloud layer reused over frames (rvk/clouds.cpp ResolveCloudLayer): each frame marches one half-size pixel of
// every 2x2 (cloud_layer.frag into a quarter-size target, the pixel taking turns); this rebuilds the half-size clouds
// from those and last frame's, moved to where they are now. The clouds are kilometres away: only the camera's turning
// moves them on screen - last frame's pixel is found by this pixel's view direction through last frame's rotation.
// What comes from last frame is kept within the range of the fresh pixels round it (no smears where clouds came or
// went); a fresh pixel blends in with what was there (the march's noise averaged out over frames).
layout(set = 0, binding = 0, std140) uniform ResolveFrame {
    mat4 invViewProj;   // clip -> direction, the camera's rotation only (this frame)
    mat4 prevViewProj;  // direction -> clip, last frame's rotation
    vec4 size;          // xy: the half-size target's, zw: the quarter-size's
    vec4 sub;           // xy: this frame's pixel in each 2x2, z: 1 = last frame's clouds usable, w: a fresh pixel's weight
                        // (1: a lightning flash - every pixel the fresh ones round it, filtered)
} R;
layout(set = 0, binding = 1) uniform sampler2D quarter;      // this frame's marched pixels
layout(set = 0, binding = 2) uniform sampler2D history;      // last frame's half-size clouds

layout(location = 0) out vec4 outClouds;

void main()
{
    ivec2 hp = ivec2(gl_FragCoord.xy);
    ivec2 sub = ivec2(R.sub.xy);
    ivec2 q = (hp - sub) / 2;                                // (the fresh pixel of this 2x2 block)
    ivec2 last = ivec2(R.size.zw) - 1;
    bool fresh = all(equal(hp & 1, sub));
    // The fresh pixels round it: their range, and their filtered value here.
    vec4 lo = vec4(1e9), hi = vec4(-1e9), tent = vec4(0.0);
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            vec4 c = texelFetch(quarter, clamp(q + ivec2(x, y), ivec2(0), last), 0);
            lo = min(lo, c);
            hi = max(hi, c);
            tent += c * float((2 - abs(x)) * (2 - abs(y)));
        }
    // A lightning flash: the clouds' light changes from frame to frame - no history to average the march's noise in, and
    // three of each four pixels would keep last frame's, unlit (user: noise in the clouds at each strike). Every pixel
    // the fresh ones round it, tent-filtered: softer for those few frames, no noise.
    if (R.sub.w > 0.99) {
        outClouds = tent / 16.0;
        return;
    }
    vec4 now = fresh ? texelFetch(quarter, clamp(q, ivec2(0), last), 0)
                     : texture(quarter, (vec2(hp - sub) * 0.5 + 0.5) / R.size.zw);
    vec4 result = now;
    if (R.sub.z > 0.5) {
        // This pixel's direction, and where it was on screen last frame.
        vec2 ndc = vec2((float(hp.x) + 0.5) / R.size.x * 2.0 - 1.0, 1.0 - (float(hp.y) + 0.5) / R.size.y * 2.0);
        vec4 far = R.invViewProj * vec4(ndc, 1.0, 1.0);
        vec3 dir = normalize(far.xyz / far.w);
        vec4 clip = R.prevViewProj * vec4(dir, 0.0);
        if (clip.w > 1e-4) {
            vec2 p = clip.xy / clip.w;
            vec2 uv = vec2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
            if (all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0)))) {
                vec4 before = clamp(texture(history, uv), lo, hi);
                result = fresh ? mix(before, now, R.sub.w) : before;
            }
        }
    }
    outClouds = result;
}
