# Volumetric clouds, second version: a cloud layer

Status: planned (2026-10-13). Replaces the per-puff volumes of `rvk/clouds.cpp` (v1, branch `clouds`).

## Why

v1 turns each of the game's cloud puffs (VisualSingleCloud_t) into a box marched through: the puff's texture
silhouette given a body, frayed by noise. In game it reads as soft grey blobs: no cloud structure (flat bases,
billowing tops, wispy edges), little variation in light, and the storm "puffs" are flat sheets hundreds of units
across near the eye's height in the sky's space, which no amount of shaping makes cloud-like.

## Decisions (user, 2026-10-13)

- A real volumetric **cloud layer** at altitude over the whole sky (the Horizon Zero Dawn / Nubis technique).
- The game still decides **where and how cloudy**: its puffs, weather and time of day drive the layer, so zones keep
  their art direction (v1's decision "follow the game's clouds" stands).
- Look: **stylised AO, richer** - the game's cloud colours and moods, with depth, structure and light. Not photoreal.
- Budget: **~1 ms at 1440p** (half resolution or less, temporal reuse).
- Game's puffs and flat cloud layers stay hidden; sky dome, stars, sun and moons stay.

## Design

**The layer.** A slab between a base and a top height above the ground (world units; to tune, start ~600..1600 above
the terrain), bent down towards the horizon (an earth curvature term) so it meets the horizon as a sky does. Marched
from the eye along each view ray through the slab; only sky pixels (the depth pre-pass's depth at its clear value,
as v1's march already skips covered ones).

**The weather map.** A 2D top-down image round the camera (~256x256 over ~20 km, scrolling with the wind), made each
frame on the GPU: R coverage, G cloud type (0 stratus .. 0.5 cumulus .. 1 cumulonimbus), B density, A precipitation.
Filled from
- the game's puffs: each projected out to the layer along the eye's line to it (as v1's shadow map does) and stamped
  as a coverage blob of its size and opacity; flat storm sheets as wide overcast stamps;
- the game's weather: rain (rvk rain.cpp's `m_rain`) -> overcast, darker, type towards cumulonimbus;
- a procedural field (drifting low-frequency noise) so coverage between and beyond the puffs looks natural, scaled by
  how cloudy the game's sky is (v1's `cover` measure).

**Shape.** Density = height profile for the type (flat-bottomed cumulus, thin stratus, tall anvils) x coverage, from a
tiling 3D base noise (Perlin-Worley, 128^3) eroded at the edges by a detail noise (Worley, 32^3) for wisps and
billows. Wind drifts both; a slow evolution offset keeps shapes changing.

**Light (stylised).** Beer-Lambert with a powder term (dark edges facing the sun, bright interiors), an approximation of
multiple scattering (a few attenuated octaves), a dual-lobe phase function (silver lining towards the sun), a few
steps towards the sun for self-shadowing. Ambient from a bottom-to-top gradient between the game's sky/fog colours.
Final colours tinted towards the game's own cloud colour (its puffs' lit colour, v1's capture) so moods match the
zone. Rain darkens and greys.

**Horizon and fog.** Distant clouds blend into the game's fog/sky colour by distance, as the game's sky does.

**Performance.** March at quarter resolution with a blue-noise jitter, temporal reprojection (each frame updates a
quarter of the pixels and reuses the rest through the camera's motion), depth-aware upsampling. Fewer steps for
far rays and where coverage is empty (cheap skipping using the weather map). Recorded in v1's own pass
(Frame::clouds, between the depth pre-pass and the scene) - the pre-pass stays intact.

**Shadows and light shafts.** v1's cloud shadow map and the volumetric light's shafts read the weather map and layer
instead of the per-puff footprints: ground shadows then match the visible clouds.

**Settings** (names <= 15 chars; the existing ones kept): RVK_CloudsOn, RVK_CloudThick (density), RVK_CloudBright,
RVK_CloudDetail (erosion), RVK_CloudShadow, RVK_CloudSteps; new RVK_CloudCover (coverage bias), RVK_CloudHeight
(layer base). RVK_CloudHalf becomes the resolution choice.

## Phases

1. **Layer + weather map + shape**: slab march at half resolution (no temporal yet), weather map from puffs and rain,
   base/detail noise, simple light. Demo scenes: clear, scattered cumulus, overcast, storm, sunset, night.
2. **Light**: powder, multiple scattering, phase, sun steps, ambient gradient, tint to the game's colours; horizon fog.
3. **Performance**: quarter resolution + temporal reprojection + upsample; timing at 1440p (only when the game isn't
   running) to the ~1 ms budget.
4. **Shadows and shafts** from the layer; retire v1's per-puff shadow footprints.
5. **In game**: tuning against the user's frame dumps (stock vs new, same spot), zone moods, weather transitions.

## Open questions

- Layer heights in AO units across zones (sky's scale differs from the world's; v1 used 350 above the eye).
- Zones with special skies (VisualGloomySky_t and friends, Shadowlands, the "vortex" skies): keep the game's clouds
  there at first.
- Indoors / no sky: off (no puffs captured).
- v1 bug seen in a dump (2026-10-13): a few puffs show in front of the far hills - check whether the far terrain is
  in the depth pre-pass; the layer's march must respect the same.
