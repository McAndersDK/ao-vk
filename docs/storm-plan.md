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
- Storm sky + storm fog (storms65): a thunderstorm (the rain: the game's or RVK_StormForce) seals the sky - the
  cloud cover at least full overcast whatever the game's puffs say, the weather map's cover 0.9..1 (cloud_weather.frag
  W.storm), the low deck on (ragged low cloud), heavier (density x1.6) and darker. The sand fog generalised to the
  storm's air (wind.cpp StormAirNow): a sand storm's dust, a thunderstorm's rain mist (RVK_RainFog: 0.004 a unit in
  full rain, scale height 250, little high up), or both; the mist grey, not reddening (tonemap: dust2.w + 4 x its
  share in 15ths), its billows rain shafts, the sun held back under the storm; lightning lights the fog round the
  strike (FL.flashWorld). RVK_StormGreen (0.6): the storm's clouds and mist cast green-grey (the user: the game's storm
  air is green). Cumulonimbus towers on the horizon: next, its own plan.

## Storm enhancements (2026-10-16, the user: wet ground + puddles, rain shelter, bolts, cumulonimbus, scud)

1. Rain shelter (storms75): the sand map (sand_map.cpp) built while it rains too, with the rain's own cover per cell -
   the highest solid thing over it, kept whether it reaches the ground or not (roofs, overhangs) - and the
   see-through cover (canopies); RainCoverMap uploads it (RGBA16F, binding 2 of the rain's set; bindless for the
   scene: FL.cover). rain.vert / splash.vert: no drops or splashes below a roof, few under a canopy.
2. Wet ground (storms75, RVK_RainWet): m_wetness (rises ~25 s with rain, dries ~4 min) and m_puddles (~90 s / 10
   min); ffp_main: the world's own surfaces (CPU flag: C.vtx.w + 2) darker and reflecting (SSR reflectivity) where
   open, puddles on the level in hollows (the map's A: the ground under the mean round it) and noise - darker,
   mirror-like, rain rings. Dry under roofs and canopies. Demo: --wet W, --bare-ground.
3. Lightning bolts of our own (storms76, RVK_StormBolt): each strike a bolt from the cloud base to the ground
   (rain.cpp MakeBolt: midpoint displacement, 3-6 branches), drawn into the rain layer (bolt.vert/frag: a white-hot
   core, a bluish glow), its strokes as the flash, the fog between taken off; the game's VisualLightning_t left out.
4. Cumulonimbus towers (storms77): in a thunderstorm the layer three times taller; the sealed deck within 6.5 km of the
   eye keeps to the lower 1300 (type 0.95), beyond it storm cells (type 1) fill the full height with an anvil spreading
   at the top (cloud_weather.frag W.storm.y, cloud_layer.frag Profile). Seen from under the deck in the band over the
   horizon. Unverified in game (the demo shows no sky).
5. Scud (storms77): in a thunderstorm the low deck sinks lower, races along 2.5x the layer's wind (m_scudDrift,
   added up) and is torn into ragged fragments a few hundred units across (cloud_layer.frag DeckDensity, L.scud).

## Phase 4 done (2026-10-16, storms79)

- Torn smoke: the storm's own wind on the smoke (smoke.cpp StormSmokeWind: up to ~4 units/s, gusting), the sim drags it
  faster and gusty eddies tear it (smoke_sim.comp, a wind past 0.8 = a storm); its box leans no further.
- Rough water: waves up to 1.8x, ripples rougher and much rougher in passing gust fronts (dark ruffled patches racing
  across), crests breaking white, spray streaks on the gusts (water.frag W.storm).
- Blown debris (RVK_StormDebris): leaves, straw and twigs skimming and hopping over the ground round the camera
  (debris.vert/frag: stateless as the rain, the cover map's B now the ground), opaque with depth and motion; trees
  shed more leaves in a storm, carried far on the wind (ffp.vert FallingLeaf, FL.rainNear.z).
