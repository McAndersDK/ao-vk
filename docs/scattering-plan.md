# Light scattering in the air (atmosphere)

Status: planned (2026-10-13). Replaces the separate, hand-tuned air effects - the land haze (RVK_VolLand,
tonemap.frag), the clouds' Aerial() / AirColour() / HorizonBand() (cloud_light.glsl, cloud_layer.frag), the sunset
tints (SunLight, Afterglow) - with one model of the air everything is lit through. Builds on the volumetric light
(volume.frag), the cloud layer, the nights (docs/night-plan.md).

## Why

Each air effect was tuned on its own, so land, sky and clouds drift apart: bright clouds over dark land at night,
sunset colours on the clouds but not on the land, the land's ambient grey under a teal sky. One model of the air
gives them all the same light: distance fades into the colour the air scatters there, the sun's light reddens as it
sets on everything, the sky's light on the land is the sky's colour.

## Decisions (user, 2026-10-13)

- Go ahead, in phases, with a strength setting to compare against the current look.
- Stay near the game's look: the model is fitted to each zone's own fog and sky colours, not physical blue air.

## Design (Hillaire 2020, "A Scalable and Production Ready Sky and Atmosphere Rendering Technique")

**Lookup tables, per frame (compute, tiny).** Transmittance (256x64: the light's way through the air from any
height and angle), multiple scattering (32x32), sky view (192x108: the air's light in every direction from the
eye). The air's makeup: Rayleigh (fine, blue-scattering), Mie (haze, forward-scattering: the sun's glow), ozone.

**Fitted to the game.** The Rayleigh colour and Mie amount solved per frame so the model's horizon colour comes out as
the game's fog colour (the terrain base pass's, SceneFog) and its zenith the game's sky dome's (sampled from the sky's
draws); eased over seconds. Mie strength from the weather (rain, fog density). Zones keep their teal / orange / grey.

**Aerial perspective (froxels).** 32x32x32 over the view out to the far clip: the in-scattered light and the
transmittance for each cell (from the sky-view and transmittance tables, the sun's and the moon's light). Applied in
the tonemap to every surface by its depth (replaces RVK_VolLand's haze) and to the clouds by where along the ray they
are (replaces Aerial / HorizonBand). Near the eye almost nothing; the game's own fog takes over where it starts (no
seam with its sky).

**Sun glow.** The Mie forward lobe round the sun in the aerial perspective: a soft halo in the haze, stronger in hazy
and wet weather. The existing shafts (volume.frag) take the same phase and the multiple-scattering term.

**Sunlight's colour.** The sun's light on everything (the frame's sun colour: lit draws, grass, water, clouds) times
the transmittance at its height: golden, then red as it sets - on the land as on the clouds (replaces SunLight's
hand tints). The moon's the same at night.

**Sky light on the land.** The sky-view table summed to a few spherical-harmonic terms: the land's ambient (the
game's global ambient, the ground's lightmap pass, grass) tinted towards it - day and night the land under the sky's
colour (replaces the night ambient tint idea in the night plan).

**Setting.** `RVK_Atmos` (0..1, default 1 when done; 0 = the current look). Names <= 15 characters.

## Phases

1. Tables + fitting to the game's fog / sky colours (log the fit; demo `--atmos`); aerial perspective on the land
   (RVK_VolLand's place) and clouds. Cost check (~0.1-0.2 ms at 1440p).
2. Sun glow; shafts with the same phase and multiple scattering.
3. Sunlight's colour by transmittance (land and clouds); moonlight the same.
4. Sky light on the land (ambient from the sky view).
5. In-game tuning: several zones, dawn / day / sunset / night, rain.

## Open questions

- The game's sky dome colours: read from its texture under the sky's draws, or from the frame (pixels at the zenith
  and horizon before our clouds)? The latter is simplest.
- How the fitted air behaves in zones whose fog is far from any physical sky (deep orange, dark grey) - clamp the fit.
- Units: AO world units ~ metres; the air's scale heights (8 km Rayleigh, 1.2 km Mie) as real, or squeezed so the
  short far clip (fog end ~1-3 km) shows the effect.
