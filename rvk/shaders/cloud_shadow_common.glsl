// The clouds' shadow (rvk/clouds.cpp: its map; the frame lights' FL.clouds / clouds2 say where, if any): the sunlight
// through the cloud layer at a point, 1 = none in the way. The includer declares `cloudShadow` (sampler2D) and the frame
// lights. (ffp_main.glsl reads the same map through the bindless set: CloudShadow.)
float CloudShadowMap(vec3 q)
{
    vec3 L = -FL.sunDir.xyz;
    if (floatBitsToUint(FL.clouds.w) == 0u || L.y < 0.05 || q.y >= FL.clouds2.x)
        return 1.0;
    vec2 uv = (q.xz + (FL.clouds2.x - q.y) * L.xz / L.y - FL.clouds.xy) * FL.clouds.z;
    vec2 edge = min(uv, 1.0 - uv);
    float inside = smoothstep(0.0, 0.08, min(edge.x, edge.y));
    if (inside <= 0.0)
        return 1.0;
    return mix(1.0, textureLod(cloudShadow, uv, 0.0).r, FL.clouds2.y * inside);
}
