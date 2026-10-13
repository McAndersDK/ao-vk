// Light on clouds, shared by the cloud layer (cloud_layer.frag) and the game's cloud sheets drawn with it
// (ffp_main.glsl, MAT_CLOUDSHEET). The includer declares the frame lights (FL).

// The sunlight reaching the clouds: reddened low in the sky (its long way through the air - orange, then pink near
// the horizon), gone a little after it sets (the clouds stay lit a while: they're high).
vec3 SunLight(vec3 sunL)
{
    float low = 1.0 - smoothstep(0.0, 0.35, sunL.y);
    low *= low;                                  // (golden first; pink only in its last few degrees)
    vec3 tint = mix(vec3(1.0), mix(vec3(1.0, 0.8, 0.58), vec3(1.0, 0.6, 0.5), smoothstep(0.75, 1.0, low)), low);
    vec3 sun = FL.sunColor.rgb * tint * smoothstep(-0.12, 0.1, sunL.y);
    // At sunrise and sunset the game's light is still level and the renderer's sun off (under ~3 degrees): a warm
    // light of their own for the clouds, from just under the horizon to ~10 degrees up (user: the night sky didn't
    // light up with the sunrise).
    float dawn = smoothstep(-0.1, 0.0, sunL.y) * (1.0 - smoothstep(0.08, 0.25, sunL.y));
    return max(sun, vec3(1.0, 0.72, 0.55) * tint * 0.8 * dawn);
}

// The glow under the clouds round sunset: their undersides lit pink by the light low in the sky (most with the sun
// at the horizon and a little after). Scaled as the sky's light (about 1).
vec3 Afterglow(vec3 sunL)
{
    float g = smoothstep(-0.14, -0.02, sunL.y) * (1.0 - smoothstep(0.02, 0.12, sunL.y));
    return vec3(1.0, 0.5, 0.55) * g;
}

// How much it is night for the clouds' light: 0 with the sun up .. 1 with it ~11 degrees down - by the sun's true
// height (FL.sunTrue), not its light: that goes out with the sun still 3 degrees up (where its shadows end), and the
// night's brightening then came on at once under a dusk sky still near daylight - clouds blown out white (user).
float Night()
{
    return 1.0 - smoothstep(-0.2, 0.03, normalize(FL.sunTrue.xyz + vec3(0.0, 1e-6, 0.0)).y);
}

// How much the sky's light is raised at night (1 by day): lit only by the sky, cloud dimmed inside to 0.4 came out a
// third as light as the game's own night clouds (user: brightness 3 to look right) - the sky's light then carries the
// game's colour on its own.
float NightSky()
{
    return mix(1.0, 3.4, Night());               // (user: 1.7 x the first 2.0 in game)
}

// Low in the sky, the horizon's band (up to ~12 degrees), as much as the game fogs its clouds (amount): 0 .. amount.
float HorizonBand(vec3 dir, float amount)
{
    float low = 1.0 - smoothstep(0.0, 0.22, dir.y);
    return amount * low * low * (3.0 - 2.0 * low);
}

// The air's colour before clouds: the game's fog colour (haze) at the horizon (low: HorizonBand), where the clouds
// meet its sky; above, paler and whiter - the game's teal fog made the whole sky's clouds teal (user: whiter).
vec3 AirColour(vec3 haze, float low)
{
    float grey = dot(haze, vec3(0.3, 0.59, 0.11));
    return mix(mix(vec3(grey), haze, 0.35) * 1.25, haze, low);
}

// The moon's light on the clouds (rvk night.cpp: FL.moon, at night FL.sunDir its direction): cool and dim, a light
// the clouds take as they take the sun's - brightest towards it, silver edges looking at it.
vec3 MoonLight()
{
    return FL.moon.rgb * 1.2;
}

vec3 MoonDir()
{
    return -normalize(FL.sunDir.xyz + vec3(0.0, -1e-6, 0.0));
}

// How much the moon lights (0 none .. 1 a high moon at night): the sky's light on the clouds eased down by it, so
// those away from the moon are darker than those it lights.
float MoonShare()
{
    return clamp(dot(FL.moon.rgb, vec3(0.3, 0.59, 0.11)) / 0.25, 0.0, 1.0);
}
