#version 450
// The suns (rvk/sun.cpp DrawSuns), as AO's rather than real ones: drawn into the sky where the game draws its sun's
// ray sprites, before its cloud sheets and the cloud layer - which then cover them as they are thick (premultiplied
// over: a thin cloud lets a bright smear through, a thick one hides it) - and a share of their glow again over the
// clouds at the sky's end (RVK_SunBleed). Each: a big spiky star of the game's own sprite outline (its biggest
// sprite's), soft towards its tips; a white-hot core of its small sprite's outline; a few broad soft beams; a faint
// wide haze. Alive: the spikes swell and shrink each at its own pace, the star turns very slowly, the beams turn and
// breathe. Added to the scene (HDR: the bloom its glare).
layout(push_constant) uniform Params {
    mat4 invViewProj;   // clip -> world, the sky's camera (raw D3DMATRIX memory, as cloud_layer.frag)
    vec4 toSun;         // xyz: towards the sun (world), w: the glow's reach (radians: its outline's longest spike)
    vec4 colour;        // rgb: its light, w: the core's reach over the glow's
    vec4 view;          // xy: the target's size, z: 1 over the clouds (the bleed alone), 0 under, w: the bleed's share
    vec4 anim;          // x: the clock (s), y: this sun's seed
    uvec4 glowShape[2]; // the glow's outline: 32 reaches (over toSun.w) round the sun, 8 bits each, 4 to a word
    uvec4 coreShape[2]; // the core's
} P;

float Hash(float n) { return fract(sin(n * 12.9898 + 4.1414) * 43758.5453); }

// Smooth value noise on a cylinder: x wraps every 'period' cells, y runs on.
float Noise(vec2 p, float period, float seed)
{
    vec2 i = floor(p), f = p - i;
    f = f * f * (3.0 - 2.0 * f);
    float x0 = mod(i.x, period), x1 = mod(i.x + 1.0, period);
    float a = Hash(x0 + i.y * 57.0 + seed), b = Hash(x1 + i.y * 57.0 + seed);
    float c = Hash(x0 + (i.y + 1.0) * 57.0 + seed), d = Hash(x1 + (i.y + 1.0) * 57.0 + seed);
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

// One spike's reach (0..1 of the glow's), swelling and shrinking (+-15%), each at its own pace.
float Spike(int i, bool core)
{
    i = (i + 32) & 31;
    uint w = core ? P.coreShape[i >> 4][(i >> 2) & 3] : P.glowShape[i >> 4][(i >> 2) & 3];
    float r = float((w >> (8u * uint(i & 3))) & 255u) / 255.0;
    float h = Hash(float(i) * 3.1 + P.anim.y + (core ? 50.0 : 0.0));
    return r * (1.0 + 0.15 * sin(P.anim.x * (0.3 + 0.5 * h) + h * 31.0));
}

// The outline's reach at angle phi (radians) round the sun: between two spikes a straight edge, near enough.
float Outline(float phi, bool core)
{
    float b = fract(phi / 6.2831853 + 0.5) * 32.0 - 0.5;
    float i = floor(b);
    return mix(Spike(int(i), core), Spike(int(i) + 1, core), b - i);
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
    float angle = acos(clamp(dot(dir, P.toSun.xyz), -1.0, 1.0));
    float reach = P.toSun.w;
    if (angle > reach * 6.0)
        return;
    float outer = smoothstep(6.0, 3.5, angle / reach);   // (the haze and beams gone smoothly by 6 reaches)
    // (phi: as sun.cpp's outline - u = up x toSun, v = toSun x u)
    vec3 up = abs(P.toSun.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 u = normalize(cross(up, P.toSun.xyz)), v = cross(P.toSun.xyz, u);
    float phi = atan(dot(dir, v), dot(dir, u));
    float t = P.anim.x, seed = P.anim.y;
    float turned = phi + t * 0.006;              // (the star turning, very slowly)
    // The star: the glow's outline, full and bright (its middle over white), fading evenly out to its tips.
    float s = angle / max(reach * Outline(turned, false), 1e-5);
    float star = clamp(1.0 - s, 0.0, 1.0);
    // The core: the small sprite's outline, white-hot (well over the tone map's white).
    float sc = angle / max(reach * Outline(turned, true), 1e-5);
    float core = pow(clamp(1.0 - sc, 0.0, 1.0), 0.8);
    // The beams: a few broad soft ones past the star's tips, two sets turning opposite ways, breathing.
    float at = fract(phi / 6.2831853 + 0.5);
    float b1 = smoothstep(0.4, 0.95, Noise(vec2(at * 6.0 + t * 0.004, t * 0.03), 6.0, seed + 7.0));
    float b2 = smoothstep(0.45, 0.95, Noise(vec2(at * 9.0 - t * 0.003, t * 0.04), 9.0, seed + 19.0));
    float beams = (b1 * 0.5 + b2 * 0.3) * exp(-angle / (reach * 0.9)) * outer;
    // A faint wide haze round it all.
    float haze = 0.15 * exp(-angle / (reach * 1.2)) * outer;
    float glow = star * 2.4 + beams + haze;
    float bleed = clamp(P.view.w, 0.0, 1.0);
    float light = P.view.z > 0.5 ? glow * bleed : core * 5.0 + glow * (1.0 - bleed);
    outScene = vec4(P.colour.rgb * light, 0.0);  // (added: over with alpha 0)
}
