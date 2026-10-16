// Dithering a colour written to an 8-bit target (the tone mapping and the passes after it): a triangular noise of
// one step either way, so a smooth gradient - a dark storm sky, ~16 steps across half the screen - comes out as fine
// grain instead of bands. t moves the pattern on each frame (0: fixed).
vec3 Dither8(vec3 c, vec2 fragCoord, float t)
{
    uvec2 p = uvec2(fragCoord) + uvec2(uint(t * 4096.0) * 1973u, uint(t * 4096.0) * 9277u);
    uint h = p.x * 1664525u + p.y * 22695477u + 1013904223u;   // (a pcg-style hash: two values from it)
    h = (h ^ (h >> 16)) * 2246822519u;
    h = (h ^ (h >> 13)) * 3266489917u;
    h ^= h >> 16;
    float a = float(h & 0xFFFFu) / 65535.0, b = float(h >> 16) / 65535.0;
    // (None at black and white: pure black stays black.)
    vec3 room = clamp(min(c, 1.0 - c) * 255.0, 0.0, 1.0);
    return clamp(c + room * (a + b - 1.0) / 255.0, 0.0, 1.0);
}
