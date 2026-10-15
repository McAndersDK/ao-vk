# Sand fog: the storm's air as a lit volume, in place of the flat haze

Status (2026-10-15): phase 1 done (prototype, RVK_SandFog, off by default), branch `storms`. The user: instead of a haze, a volumetric fog. Phase 1 behind
RVK_SandFog (0 = today's haze) until it wins in game.

## Why

The storm's air today (tonemap.frag, the `dust` block) is a veil over the finished picture: a flat colour, mixed in by
an optical depth along the view ray (RVK_SandGusts: billows in it). It lights nothing and nothing lights it. What
followed from that, one patch each so far:

- the sim sand (smoke.frag, lit) over the hazy distance laid a film of another colour (storms40: its colour tied to
  the haze's);
- between gust billows the haze thinned and the far land showed through in its own dusk colours (storms41);
- a low sun lit the dunes' crests bright through the storm (storms42/43: the sun spread on surfaces);
- no shade in the dust: a dune or a wall between the sun and the air changes nothing; the dust as bright facing the sun
  as away from it; at dusk and night tuned colours rather than the light there is.

## The idea

The air marched like the volumetric light (volume.frag, half resolution, ~0.5 ms): per pixel a ray from the eye to the
surface (or the sky), at each step the dust's density and the light reaching it; out come the light the dust scatters
towards the eye and how much of what is behind still shows (transmittance). The tone mapping: c = c x T + L (the sim
sand, near, kept in front: its own light and cover taken apart as today).

### Density

- Height over the ground: thick near it, thinning upwards (scale height ~40 units: a storm wall; the sky overhead
  brighter, the horizon buried). The ground: the terrain grid terrain_sun.cpp keeps (16 units a cell, 4 km; its map's
  G channel), so valleys fill with dust and a ridge stands in thinner air; without it, the ground under the eye.
- The storm's strength (m_sand) x RVK_SandHaze: today's haze's optical depth, so the far distance is as buried.
- Gusts (RVK_SandGusts): today's billow noise in 3D drifting along the wind - clouds of dust going by.
- Near the eye: clear within RVK_SandClear (as the sim sand), the shelter (the lee of a hill or wall clears the air:
  the eye's shelter now, the sand map's per point later).

### Light at each step

- The sun: through the dust above towards it (optical depth of the layer along the sun's direction - a low sun
  reddened and dimmed through the storm), the shadow cascades near, the terrain's shade from a low sun beyond (the
  terrain map's R: dunes cast shade into the dust at dusk), the clouds' shadow map. Phase: forward (g ~0.5) - the dust
  glows towards the sun.
- The sky: the ambient light, evenly, x the dust's colour (the storm's colour: its albedo).
- The dust's colour: the storm's (m_sandColour) - the haze's today, so the look stays the game's.

### Beyond the march

Steps out to ~400 units (closer together near: t = R u^2); past it, the rest of the ray to the surface (or for the
sky, out of the layer) in closed form - an exponential layer's optical depth along the ray - lit as the march's last
steps. So a far ridge sits deep in the dust and the horizon meets the sky with no seam.

## Phases

1. **Prototype** (RVK_SandFog): sand_fog.frag (density by height over the terrain grid, the gusts; sun through the
   layer with cascades + terrain shade + phase; sky light; the closed-form tail), a depth-aware blur, the tone
   mapping's composite (binding 14) in place of the dust block with the sim sand kept in front. terrain_sun.cpp: the
   map made while a storm blows (not only a low sun), G = each cell's ground. Demo: --sand-fog; compare day, dusk,
   night with the haze.
2. **Tuning in game**: density and scale height against today's haze, colours at dusk and night, the sun's glow; the
   sim sand's colour against the fog's (storms40's tie to the haze -> to the fog's light); cost.
3. **Shelter in the fog**: the sand map's shelter per point near the eye (the lee of walls and hills clear).
4. **The haze retired** (the fog the default), RVK_SandGusts / RVK_SandHaze meaning the fog's; ash storms alike
   (dark grey, no sun glow).

Each phase: the demo (identical without a storm), the harness, validation, cost (target: fog <= 0.5 ms at 1440p).

## Risks

- Noise at half resolution (jittered steps + blur + TAA, as the volumetric light); thin things against bright dust.
- Shadow cascades end at RVK_SunDist: beyond, only the terrain shades the dust (far buildings cast no shafts).
- The terrain grid knows only terrain drawn so far (holes far off: the eye's ground stands in).
- The sim sand and the fog lit differently: tie the sim's colour to the fog's light (phase 2).

## Done (2026-10-15)

- Phase 1: rvk/shaders/sand_fog.frag + sand_fog_blur.frag, hdr.cpp RenderSandFog (m_sandFogTex, the volume pass's
  layout), occlusion.glsl binding 14 SandFogAt (depth-aware upsampling from the depth buffer), tonemap.frag's fog
  branch (dust2.w bit 2; the sim sand kept in front; the sun's shafts x the transmittance). terrain_sun: the map made
  while the fog is on, G = each cell's ground. Settings RVK_SandFog, demo --sand-fog.
- Tuning on the way: scale height 120 (40 left the sky showing overhead); the sun's light the dust above took comes
  down spread (0.8 x the sun's height) - single scattering alone went near black; the sky's pixels wholly hidden (as
  the haze: RVK_SandHaze's default 0.1 is a thin storm); the dust's colour calibrated so that as typically lit it is
  the haze's (night identical to the haze; day: the horizon as the haze, overhead darker - lit from behind and above).
- Cost: 24 steps, one noise scale: 0.48 ms at 2560x1440 in the demo (40 steps and two scales: 1.06).
