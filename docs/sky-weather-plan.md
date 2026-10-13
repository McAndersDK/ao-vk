# Sky weather: clear days to overcast

Status: planned (2026-10-13). Builds on the cloud layer (clouds.cpp: the weather map, `cloudy`), the game's sheets
(docs/clouds-sheets-plan.md), rain (docs: ao-weather memory, rain.cpp). Before the light scattering
(docs/scattering-plan.md), which takes its haze from this.

## Why

The game draws the same ~30 flat sheets in every sky - fair days and storms alike (count, size, opacity, fog share
don't tell them apart: user dumps). So the layer shows scattered cloud whenever the game draws its puffs, overcast only
with rain: a clear sky never happens (user, 2026-10-13).

## Decisions (user, 2026-10-13)

- **The game's sheets fade with the weather**: thin or gone on clear days, as the game draws them on cloudy ones.
- **Rain only when the game rains**: our weather varies clear .. overcast between; the game's rain takes over.
- **From the game clock**: the same time gives the same weather on every login (AO's time runs from real time: the
  wall clock, UTC, stands in for it).
- **Before the scattering.**

## Design

**The sky's state.** Cloudiness 0 (clear) .. 1 (overcast) from the clock: smooth noise over time, knots ~90 minutes
apart, a lesser wander over 25 (a sky lasts about half an hour - clear about an hour; changes take minutes), mapped
so clear, fair, scattered, broken and overcast all come up (clear 19% of the time, overcast 14%: 60 days simulated). The zone later (its playfield: native occluder's
+0x94 when on; else a guess from the fog colour): desert clearer, swamp / coast cloudier - phase 3.

**What it drives.**
- The layer's cloudiness (now 0.42-0.47 whenever puffs are drawn) and type (broken / overcast: flatter, layered).
- The game's sheets' strength (FrameLights cloudSky.w; their faint shadow): none when clear, as the game when cloudy.
- Through those: the clouds' shadows, the light shafts through gaps.
- Later: the haze (scattering's Mie), wind strength.
- The game's rain: overcast to rain as now, easing back after.

**Settings.** `RVK_SkyVary` (0..1, default 1): how much the sky changes (0: as before - always scattered).
`RVK_CloudCover` stays an offset on top. Logged: the state, its cloudiness, the next change.

## Phases

1. The clock's cloudiness -> the layer and the sheets; RVK_SkyVary; log; demo `--sky-time T` (seconds) to see each sky.
2. Cloud type by state (broken, overcast: layered, flatter); wind by state.
3. Zones' character (playfield id).
4. Hooks for the scattering (haze by state).
