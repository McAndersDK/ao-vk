# Storms: one wind, gust fronts, sandstorms, rain that moves

Status: plan (2026-10-14), branch `storms` (from main 2346b5b). After 0.5.0 - not in that release (user). Under the
Experimental "New weather" switch (RVK_WeatherOn) until tuned.

## Why

The user wants a storm rework, and sand blowing in the deserts. Today the renderer has no single wind:

| what | its wind |
|---|---|
| grass, shrubs, trees (sway, gusts) | fixed direction (0.56, 0.35), RVK_Sway / RVK_GrassGusts - no weather |
| rain (rain.cpp) | the game's weather wind (VisualRain_t::process), smoothed: m_rainWind |
| clouds' drift (clouds.cpp) | m_rainWind |
| smoke (smoke.cpp) | the game's effect wind (_EffectHandler_t +0x30), x0.1, at most 0.8 |
| sky wind on dry days (sky.cpp SkyWind) | a direction turning over hours, from the clock |

In a storm the rain slants one way while the grass waves gently another.

## Phase 1 - one wind, storm strength, gust fronts

- `Device::Wind` (new, wind.cpp): direction, strength and a gust field, once a frame. Direction from the game's weather
  wind when it has one, else SkyWind; strength from the weather (m_rain, the game's precipitation and wind scale) and
  the sky's cloudiness - calm when clear, strong in storms, ramping up as the rain arrives and down after.
- Gust fronts: bands of stronger wind travelling downwind across the land (a few per minute in a storm), as a 2D
  field the shaders sample by world position.
- Everything reads it: grass (grass_common.glsl wind/gusts: flattened in the band, springing back), sway of plants and
  trees (sway.glsl), falling leaves (torn off in gusts), rain's slant, clouds' drift, smoke's carry, GPU particles.
  RVK_Sway / RVK_GrassGusts stay as multipliers.
- Settings: RVK_WindStorm (how strong storms get, x), names <= 15 characters.

## Phase 2 - desert sandstorms

- Where: sand ground (grass.cpp's ground classification: not grass, sandy colour) and the zone's climate (sky.cpp
  m_skyZoneLean: deserts). Check first whether the game has a sandstorm weather of its own (its storm object plays
  `SM_Sandy_Env_NearLightning`; FUN_100b9850): follow it if so.
- Near: one simulated volume around the camera (smoke.cpp's solver; the box travels with the camera, its contents stay
  in the world, as a source's box). Sand fed in off dune crests and windward slopes (the terrain heights grass.cpp
  captures), along the box's upwind side, and at characters' feet (running, landing). Sand colour, heavier (settles),
  no heat. Quality setting as RVK_SmokeQual.
- Dust devils: now and then a column with a rotating force added to the solver (done, then removed in storms61 - the
  user).
- Far: sand-coloured haze in the air pass (tonemap.frag Air / RVK_Atmos), thickening with the wind, moving with the gust
  fronts - a wall of dust at a distance, a few dozen metres' visibility in a bad storm.
- Along the ground: sand streaks skimming low, GPU particles (particles.cpp), everywhere on sand.
- Budget: ~1-2 ms in a full sandstorm (mostly the near volume), next to nothing when calm.

## Phase 3 - rain that moves, lightning

- Rain's angle following each gust; rain curtains (denser sheets) sweeping through; spray where it hits.
- Lightning: flashes lighting the clouds from inside and the scene for a moment; the game's thunder.
- Scud: ragged low cloud racing under the storm deck (clouds.cpp), faster than the layer.

## Phase 4 - debris, rough water, smoke

- Blown leaves, grass bits, twigs in forests and fields; spindrift near water (GPU particles, carried by the wind).
- Water (water.cpp / water_sim.comp): waves along the wind, whitecaps and spray in storms, dark wind-ripple patches
  racing ahead of gusts.
- Smoke and fire torn sideways and broken up by gusts (smoke.cpp's wind cap lifted with the storm).

Each phase playable on its own; demo scenes for each (the grass bench with a storm, a desert terrain for sand).

## Status (2026-10-14)

- Phase 1 done (524e014): wind.cpp shared wind; grass/plants/leaves/water/rain/clouds follow it; RVK_WindStorm; the
  game's storm tint left out with the new weather (RVK_StormTint).
- Phase 2a done: the sand/ash storm's air from the game's tint (its colour, alpha = strength): tone mapping dust haze,
  RVK_SandHaze. Staged randy31-storms1.dll (not in ao-wine.sh: 0.5.0's candidate is under test).
- Phase 2b (2026-10-15): the sand volume (smoke.cpp UpdateSandVolume, smoke_sim.comp's sand mode; RVK_SandBlow): one
  more smoke volume, 32 units a side round the camera (48/64/80 cells by RVK_SmokeQual), moving a whole cell at a time,
  its floor under the lowest ground in it. The storm's wind (wind.cpp: a sandstorm now raises the storm's strength
  too) blows through it and in over its walls with sand in streaks; the ground under each column (the ground grass
  captures - flat under the camera's character with grass off) is solid and gives sand off where it is bare, skimming
  along it; characters' feet kick it up. (Dust devils - now and then one carried through by the wind - were removed in
  storms61: the user.) The tone mapping's sand haze leaves what smoke covers at a near veil (it read the
  sky's full veil before: a devil against the sky was painted over) - the half-size smoke image's cover, binding 13.
  Demo: --sand-storm (the grass bench on sand, the game's tint, the new weather without rain), --sand-blow X;
  ~0.6 ms in the demo (960 x 960).
- Next: an in-game look at 2a + 2b in a real sandstorm (the game's tint colour and alpha, the wind's speed, how
  thick); ground streaks finer than the grid (GPU particles) if the volume's skimming sand is too coarse; then phase 3.
- Phase 3 (2026-10-15, storms63): the rain's way added up frame by frame (rain.cpp m_rainDrift: a change of lean moves
  the drops on, never jumps them); gust fronts (wind.glsl Gust) lean it harder as they pass and gather it into
  curtains, thinner between (rain.vert). Lightning (RVK_Lightning, x; 0 none): each of the game's strikes - found by
  its glow over the clouds (VisualCloudlight_t, still left out) flaring up: when and where - gives a flash of our
  own (rain.cpp: a stroke, one or two return strokes, an afterglow over ~1.5 s): its light on everything by the
  surface's colour (the albedo target), falling off over ~450 units from the strike, and the clouds round it glowing
  (tonemap.frag; the frame lights at binding 15). Demo: --lightning-at N (the grass bench). Not yet: scud.
