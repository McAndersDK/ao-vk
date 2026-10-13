#version 450
// A sun sprite (rvk/sun.cpp DrawSuns), the game's VisualSunRays_t made anew: its texture (newsun_frame01.png) drawn
// procedurally - measured from it: white to ~0.13 of the sprite's radius, that edge serrated (~14 teeth, +-10%), a
// pinkish glow falling off as exp(-(r - 0.12) / 0.2), gone by ~0.8, faint thin rays - in the sprite's tint, added. In
// HDR its core glares (the bloom); alive: the teeth swell and shrink each at its own pace, the rays shimmer and turn
// slowly. Low in the sky: bigger (sun.cpp), flattened, its glow spread along the horizon. Its glare follows how much
// of its core is seen (sun_vis.comp): gone behind thick cloud or the land, swelling as it comes out. Drawn into the sky before the game's cloud sheets and the cloud layer - which cover it as they are thick -
// and a share of its glow again over the clouds at the sky's end (RVK_SunBleed).
layout(push_constant) uniform Params {
    mat4 invViewProj;   // clip -> world, the sky's camera (raw D3DMATRIX memory, as cloud_layer.frag)
    vec4 toSun;         // xyz: towards the sprite's middle (world), w: its radius (radians)
    vec4 colour;        // rgb: its tint (premultiplied, HDR), w: its rim's tint over its middle's (Gouraud)
    vec4 view;          // xy: the target's size, z: 1 over the clouds (the bleed alone), 0 under, w: the bleed's share
    vec4 anim;          // x: the clock (s), y: this sprite's seed, z: how low in the sky (0..1), w: how much of its
                        // core is seen (0..1, eased: its glare)
} P;

float Hash(float n) { return fract(sin(n * 12.9898 + 4.1414) * 43758.5453); }

// The core's serrated edge at angle a (0..1 round): 14 teeth, each its own height, swelling and shrinking. ~+-1.
float Teeth(float a)
{
    float t = P.anim.x, seed = P.anim.y;
    float x = a * 14.0, i = floor(x), f = x - i;
    float h = Hash(mod(i, 14.0) + seed);
    float height = (0.55 + 0.45 * h) * (1.0 + 0.3 * sin(t * (0.4 + 0.6 * h) + h * 40.0));
    return (1.0 - 2.0 * abs(f - 0.5)) * height * 2.0 - 1.0;
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
    // The core: white-hot to its serrated edge.
    float edge = 0.135 * (1.0 + 0.1 * Teeth(at));
    float core = smoothstep(edge * 1.12, edge * 0.88, r);
    // The glow: white near the core, pinkish further out; its rays.
    float glow = r < 0.9 ? min(exp(-(r - 0.12) / 0.2), 1.0) * smoothstep(0.85, 0.6, r) : 0.0;
    vec3 hue = mix(vec3(1.0), vec3(1.0, 0.78, 0.86), smoothstep(0.1, 0.25, r));
    float rays = r < 0.9 ? 0.18 * Rays(at, r) * step(0.1, r) : 0.0;
    float fade = mix(1.0, P.colour.w, clamp(r, 0.0, 1.0));   // (the vertex tint across the fan)
    vec3 outer = (hue * glow * (1.0 - core) + vec3(rays) + vec3(band)) * 1.2 * fade;
    // (the core whiter than the tint: the game's sprites, added in 8 bits, clip to white where they're hot)
    vec3 hot = P.colour.rgb + 0.6 * max(P.colour.r, max(P.colour.g, P.colour.b));
    float bleed = clamp(P.view.w, 0.0, 1.0);
    vec3 light;
    if (P.view.z > 0.5) {
        // Over the clouds: the glow's bleed - less behind thick cloud or the land (seen: of its core) - and the glare
        // of a sun seen, swelling in as it comes out: a wide soft white bloom round the core, the rays brighter.
        float glare = seen * (exp(-r / 0.16) * 0.45 + exp(-r / 0.5) * 0.12 + rays * 1.5);
        // (the glow whole - not cut where the core is: under the clouds the core is hidden with it)
        vec3 whole = (hue * glow + vec3(rays) + vec3(band)) * 1.2 * fade;
        light = P.colour.rgb * whole * bleed * (0.35 + 0.65 * seen) + hot * glare * 0.6;
    } else {
        light = hot * core * 4.0 + P.colour.rgb * outer * (1.0 - bleed);
    }
    outScene = vec4(light, 0.0);                 // (added: over with alpha 0)
}
