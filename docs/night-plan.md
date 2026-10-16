# Nights: darker, moonlit, the land matching the sky

Status: planned (2026-10-13). Builds on the cloud layer and sheets (`docs/clouds-sheets-plan.md`), the grading in
`tonemap.frag`, the sun shadows (`rvk/shadow.cpp`).

## Why

At night the sky and clouds read as moonlit - a bright teal sky, near-white clouds (user screenshot 2026-10-13
14:08, RVK_CloudBright 0.7) - while the land is dark, warm and flat: no light from the moon's side, no cool cast, no
shadows. The two halves of the picture don't belong to the same night, and the whole is brighter than a night should be.

What the game gives at night (user dump rvk-frame-134854, night): its one directional light points *up* (dir
-0.50 0.59 0.63: the sun under the ground) in dim blues - diffuse (0.13 0.22 0.30) / (0.24 0.39 0.55) per object
class - with a grey global ambient (0x3A3A3A). Our renderer drops the sun once it is below the horizon
(`RenderShadowMap`: sunDir.y > -0.05), so the land is left with that ambient and the game's dim, upward light. The
sky draws two lit 584-vertex meshes at eye + (0 400 800) and eye + (0 400 0) (draws 9, 10: 256^2 texture, blend
SRCALPHA/INVSRCALPHA) - most likely the moons, lit by the same light (their phases).

## Decisions (user, 2026-10-13)

- **Moonlit blue**: clearly darker than now, a cool silver-blue cast, shapes still readable ("day for night").
- **Moonlight + shadows**: a dim cool light from the moon's direction on the land, characters and clouds, with soft
  moon shadows (the sun's shadow maps, free at night as the sun has none).
- **Sky darker, clouds moon-side lit**: a darker sky (stars show more), clouds brightest towards the moon with silver
  edges, darker away from it.
- Extras, all three: **night vision shift** (darks lose colour and lean blue), **brighter lamps/windows**, a
  **night setting**.

## Design

**The moon.** Found from the sky's draws: the moon meshes (render list 0, 584 v, lit, at eye + 400 up) - the
direction from the eye to the brighter/larger one's centre (world matrix x mesh bounds), eased over frames. Fallback
when none is seen (zones without moons, indoors): opposite the sun, raised to at least 25 degrees. Its brightness:
night only - `Night()` (the sun's light gone) x the moon's height (none below 5 degrees, full above 30) x the game's
own night light's strength (so zones and weather that make nights pitch dark stay so); rain/overcast dims it by the
cloud layer's cover.

**Moonlight on the land.** The moon becomes the frame's "sun" once the sun is gone: `m_frameSunDir/Color` take the
moon's direction and a cool silver colour (~0.55 0.65 0.85 x 0.12-0.18 of full sun), so everything that already
follows the sun follows it - per-pixel lighting, leaves, contact shadows, the cloud shadow map, light shafts (weak).
The cross-fade at dusk: the sun fades out (its light on flat ground -> 0), the moon fades in over the same span - no
frame with both, no pop. The game's upward night light: kept for the sky draws it lights (moons' phases, sheets),
dropped from the scene (it lights from below).

**Moon shadows.** The sun's cascades, drawn from the moon's direction. Softer (wider filter: the moon is the same size
as the sun but the light is dim - wider penumbrae read better), strength ~0.6, shorter distance (the far cascades
skipped: cheaper at night).

**The land's ambient.** The grey global ambient becomes the night sky's colour: the ambient taken from the sky's air
colour (`AirColour()` at the zenith) at the game's ambient's brightness - the land picks up the same blue as the sky
over it. Faces up get more of it than faces down (a sky/ground split, cheap: by the normal's y).

**Darker sky and clouds.** The night sky lowered in the tonemap's grade by depth (the far plane: sky) - not in the
sky's own colours, so the stars dim less than the sky round them. The clouds' night boost (`NightSky()` 3.4, the 25%
air floor) eased down to fit the darker sky; the moon lights them as the sun does (SunLight() takes the moon at
night: forward scattering round it - silver edges towards the moon; the far side dim). `RVK_CloudBright` stays the
user's overall control.

**Night vision shift.** In the grade, by `night`: below a luminance knee colours lose saturation and shift towards
blue (0.78 0.9 1.18 now - stronger, and weighted to the darks so lamps keep their colour). Replaces/extends
`RVK_NightTint`.

**Lamps and windows.** At night the point lights (lamps) and emissive texels (`RVK_NightGlow`) gain relative to the
darker surroundings - their strength held (not darkened with the rest) and the bloom threshold lowered by night, so
they glow.

**Setting.** `RVK_NightDark` (0..2, default 1): how dark the night gets - 0 = as now (no darkening; moonlight still
on), 1 = moonlit blue, 2 = dark and moody. Scales the sky/land darkening and the vision shift together. Optionally
`RVK_MoonLight` (0..2, default 1) for the moon's strength. (Names <= 15 characters.)

## Phases

1. The moon found (log its direction, which draw, brightness; demo `--moon x y z`); moonlight on the land as the
   frame's sun at night, the dusk cross-fade, no shadows yet. Sky-coloured ambient.
2. Moon shadows (cascades from the moon, softer, fewer).
3. Darker sky and clouds; clouds lit by the moon (silver edges); `RVK_NightDark`.
4. Night vision shift; lamps and windows held bright, bloom by night.
5. In-game tuning against user screenshots: clear night, moonless/rainy night, dusk to night, a town at night.

## Open questions

- Are draws 9/10 the moons, and does their world matrix carry the orbit (the translation looks fixed at eye + 400
  up)? A night dump with the moons on screen decides it (their rect/position as the night goes on).
- How the game's night light varies by zone and weather (does a dark zone set a darker night colour?) - decides
  whether the moon's strength follows it.
- Shadow cost at night with the moon low (long shadows: the cascades' depth ranges).
