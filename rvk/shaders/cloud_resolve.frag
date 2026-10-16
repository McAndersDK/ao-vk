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
    // The fresh pixels round it - those marched (alpha >= 0; -1: covered, not marched, unknown): their range, their
    // mean.
    vec4 lo = vec4(1e9), hi = vec4(-1e9), sum = vec4(0.0);
    float known = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            vec4 c = texelFetch(quarter, clamp(q + ivec2(x, y), ivec2(0), last), 0);
            if (c.a < 0.0)
                continue;
            lo = min(lo, c);
            hi = max(hi, c);
            sum += c;
            known += 1.0;
        }
    // This frame's value here: the fresh pixel's own; else the known of the four fresh ones round it, weighted as a
    // bilinear filter would; else the mean round it; else none.
    vec4 now = vec4(0.0);
    bool haveNow = false;
    vec4 own = texelFetch(quarter, clamp(q, ivec2(0), last), 0);
    if (fresh && own.a >= 0.0) {
        now = own;
        haveNow = true;
    } else {
        vec2 at = (vec2(hp - sub) * 0.5 + 0.5) - 0.5;      // (in quarter texels)
        ivec2 base = ivec2(floor(at));
        vec2 f = at - vec2(base);
        vec4 acc = vec4(0.0);
        float wsum = 0.0;
        for (int k = 0; k < 4; ++k) {
            ivec2 o = ivec2(k & 1, k >> 1);
            vec4 c = texelFetch(quarter, clamp(base + o, ivec2(0), last), 0);
            float w = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y);
            if (c.a >= 0.0) { acc += c * w; wsum += w; }
        }
        if (wsum > 1e-4) { now = acc / wsum; haveNow = true; }
        else if (known > 0.0) { now = sum / known; haveNow = true; }
    }
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
                // (Kept within what was marched round it - where nothing round it was, as it was: hidden there.)
                vec4 before = texture(history, uv);
                if (known > 0.0) before = clamp(before, lo, hi);
                result = fresh && own.a >= 0.0 ? mix(before, now, R.sub.w) : before;
            }
        }
    }
    // (Never unknown in the history: a covered spot holds the clouds round it, ready when it is uncovered.)
    outClouds = max(result, vec4(0.0));
}
