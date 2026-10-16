#version 450
// A lightning bolt's ribbon (bolt.vert): a white-hot core and a bluish glow round it, added over the picture.
layout(location = 0) in vec2 vCorner;
layout(location = 1) in float vBright;
layout(location = 2) in float vCore;

layout(location = 0) out vec4 outScene;

void main()
{
    float y = abs(vCorner.y);
    float core = exp(-pow(y / max(vCore, 1e-3), 2.0) * 2.0);
    float glow = exp(-y * y * 6.0) * 0.18 + exp(-y * 3.0) * 0.06;
    vec3 c = vec3(1.0, 1.0, 1.0) * core + vec3(0.6, 0.7, 1.0) * glow;
    outScene = vec4(c * vBright, 0.0);       // (additive: alpha 0 over the premultiplied blend)
}
