#version 450
// A sun sprite (rvk/sun.cpp DrawSuns), the game's VisualSunRays_t made anew: its texture (newsun_frame01.png) drawn
// procedurally - measured from it: white to ~0.12 of the sprite's radius with ~20 short soft spikes round it, a
// pinkish glow falling off as exp(-(r - 0.12) / 0.2), gone by ~0.8, faint thin rays - in the sprite's tint, added. A
// sun's smallest sprite is its body: in HDR its core glares (the bloom). Alive: the spikes swell and shrink each at its
// own pace, the rays shimmer and turn slowly. Low in the sky: bigger (sun.cpp), flattened, its glow spread along the horizon. Its glare follows how much
// of its core is seen (sun_vis.comp): gone behind thick cloud or the land, swelling as it comes out. Drawn into the sky before the game's cloud sheets and the cloud layer - which cover it as they are thick -
// and a share of its glow again over the clouds at the sky's end (RVK_SunBleed).
layout(push_constant) uniform Params {
    mat4 invViewProj;   // clip -> world, the sky's camera (raw D3DMATRIX memory, as cloud_layer.frag)
    vec4 toSun;         // xyz: towards the sprite's middle (world), w: its radius (radians)
    vec4 colour;        // rgb: its tint (premultiplied, HDR), w: its rim's tint over its middle's (Gouraud)
    vec4 view;          // xy: the target's size, z: 1 over the clouds (the bleed alone), 0 under, w: the bleed's share
                        // (fraction) + 1000 x the sprite's core share (whole part: 1 a sun's body, less its glows)
    vec4 anim;          // x: the clock (s), y: this sprite's seed, z: how low in the sky (0..1), w: how much of its
                        // core is seen (0..1, eased: its glare)
} P;

float Hash(float n) { return fract(sin(n * 12.9898 + 4.1414) * 43758.5453); }

// The short soft spikes round the core at angle a (0..1 round), r out (the texture's ~20, blurred - not a serrated
// edge: drawn hard that read as a polygon, user 2026-10-13): each its own length, swelling and shrinking. 0..1.
float Spikes(float a, float r)
{
    float t = P.anim.x, seed = P.anim.y + 37.0;
    float x = a * 20.0, i = floor(x), f = x - i;
    float h = Hash(mod(i, 20.0) + seed);
    float len = (0.15 + 0.1 * h) * (1.0 + 0.25 * sin(t * (0.35 + 0.5 * h) + h * 40.0));
    float peak = exp(-pow((f - 0.5) / 0.2, 2.0));
    return peak * smoothstep(len, 0.06, r);
}

// The faint rays at angle a (0..1 round), r out: 48 thin streaks, each its own brightness and length, shimmering.
float Rays(float a, float r)
{
    float t = P.anim.x, seed = P.anim.y + 91.0;
    float x = a * 48.0, i = floor(x), f = x - i;
    float h = Hash(mod(i, 48.0) + seed), g = Hash(mod(i, 48.0) + seed + 7.0);
    float peak = exp(-pow((f - 0.5) / 0.12, 2.0));
    float len = 0.3 + 0.6 * g * g;
    float shimmer = 0.65 + 0.35 * sin(t * (0.5 + h) + g * 30.0);
    return peak * h * h * shimmer * smoothstep(len, 0.12, r);
}

layout(location = 0) out vec4 outScene;
layout(location = 1) out vec4 outGlow;
layout(location = 2) out vec4 outLocal;
layout(location = 3) out vec4 outMotion;
layout(location = 4) out vec4 outAlbedo;

void main()
{
    outGlow = outLocal = outMotion = outAlbedo = vec4(0.0);
    outScene = vec4(0.0);
    vec2 ndc = vec2(gl_FragCoord.x / P.view.x * 2.0 - 1.0, 1.0 - gl_FragCoord.y / P.view.y * 2.0);
    vec4 a = P.invViewProj * vec4(ndc, 0.0, 1.0), b = P.invViewProj * vec4(ndc, 1.0, 1.0);
    vec3 dir = normalize(b.xyz / b.w - a.xyz / a.w);
    float ahead = dot(dir, P.toSun.xyz);
    if (ahead < 0.3)
        return;
    // Where round the sprite: on the sky's plane at its middle (x along the horizon, y up), in its radii.
    vec3 up = abs(P.toSun.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 u = normalize(cross(up, P.toSun.xyz)), v = cross(P.toSun.xyz, u);
    vec2 q = vec2(dot(dir, u), dot(dir, v)) / ahead / P.toSun.w;
    // Low in the sky: flattened (squashed upright by the air, as a setting sun), its glow spread along the horizon.
    float low = P.anim.z, seen = P.anim.w;
    float r = length(vec2(q.x, q.y / (1.0 - 0.2 * low)));   // (0 the middle, 1 the rim)
    float band = low * exp(-abs(q.x) / 1.8) * exp(-abs(q.y) / 0.22) * 0.35;
    if (r > 0.9 && band < 0.002 && (P.view.z < 0.5 || r > 2.5))
        return;
    float phi = atan(q.y, q.x);
    float turn = P.anim.x * 0.004 * (fract(P.anim.y * 0.37) < 0.5 ? 1.0 : -1.0);   // (turning slowly, either way)
    float at = fract(phi / 6.2831853 + 0.5 + turn);
    float coreShare = floor(P.view.w) / 1000.0, bleed = fract(P.view.w);
    // The glow (the texture's measured fall: white to ~0.12, pinkish further out), its short soft spikes round the
    // core, its faint long rays.
    float glow = r < 0.9 ? min(exp(-(r - 0.12) / 0.2), 1.0) * smoothstep(0.85, 0.6, r) : 0.0;
    vec3 hue = mix(vec3(1.0), vec3(1.0, 0.78, 0.86), smoothstep(0.1, 0.25, r));
    float spikes = Spikes(at, r);
    float rays = r < 0.9 ? 0.18 * Rays(at, r) * step(0.1, r) : 0.0;
    float fade = mix(1.0, P.colour.w, clamp(r, 0.0, 1.0));   // (the vertex tint across the fan)
    vec3 whole = (hue * glow + vec3(0.35 * spikes + rays + band)) * 1.2 * fade;
    // The body (a sun's smallest sprite: coreShare 1; its big ones' cores, magnified, stay soft glows in their tint):
    // a soft white-hot middle and its spikes, whiter than the tint (the game's sprites, added in 8 bits, clip to white).
    vec3 hot = P.colour.rgb + 0.6 * max(P.colour.r, max(P.colour.g, P.colour.b));
    float body = coreShare * (exp(-pow(r / 0.09, 2.0)) * 3.0 + spikes * 0.8);
    vec3 light;
    if (P.view.z > 0.5) {
        // Over the clouds: the glow's bleed - less behind thick cloud or the land (seen: of its core) - and the glare
        // of a sun seen, swelling in as it comes out: a soft white bloom round the body, the rays brighter.
        float glare = coreShare * seen * (exp(-r / 0.16) * 0.45 + exp(-r / 0.5) * 0.12 + rays * 1.5);
        light = P.colour.rgb * whole * bleed * (0.35 + 0.65 * seen) + hot * glare * 0.6;
    } else {
        light = hot * body + P.colour.rgb * whole * (1.0 - bleed);
    }
    outScene = vec4(light, 0.0);                 // (added: over with alpha 0)
}
