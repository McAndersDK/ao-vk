# Cloud types (clouds v2, next feature)

Status: planned (2026-10-13). Builds on the cloud layer (`docs/clouds-layer-plan.md`, `rvk/clouds.cpp`,
`cloud_layer.frag`, `cloud_weather.frag`).

## Why

The layer draws one kind of cloud - cumulus heaped to stratocumulus to overcast, by a single "type" value. The
stock sky is different in kind: its puff textures are fibrous (`single_cloud01-03.png`) or streaky wisps
(`single_cloud04.png`: cirrus), and the game stretches them over big flat sheets - a wispy, high-cloud look the
layer replaced with soft, rounded cloud.

## Decisions (user, 2026-10-13)

- Types: **cirrus** (high wisps), **altocumulus** (mid-level cloudlets in rows), **storm towers** (cumulonimbus,
  anvils), **stratus / fog banks** (low grey sheets, mist near the ground) - besides the current cumulus/overcast.
- Which appear: **follow the game** - its puff textures, its flat sheets, its weather, time of day; zones and
  weather keep their character.
- Cirrus: a **thin high layer** (cheap, ~0.1 ms), not a second volume.

## Design

**Reading the game's sky.** Per puff, beyond what is captured now (size, shape, opacity, lit and fogged colour):
which texture - the proxy knows each texture's RDB id (texture picking's `DescribeTexture`); the device gets it
(a `Texture` RDB id) so a puff can be told wispy (04: cirrus) from fibrous (01-03). Shares per frame, eased:
wispy, fibrous, flat sheets, small-and-many (altocumulus-like), opacity; with rain, wind, the sun's height. Logged.

**Weather map, second image.** Coverage per level: low (stratus / fog banks), mid (altocumulus), high (cirrus), and
storm cells (where towers stand: a few discrete cells, drifting, in rain/storm weather). The current map keeps the
main layer's coverage, type, density, base.

**Cirrus.** A thin shell high above (~8 km), hit once per view ray (no march): fibrous streaks - detail noise
stretched along the wind, two scales, edges frayed - coverage from the high channel; lit cheaply (forward
scattering: bright towards the sun, silver at sunset; the sky's colour), hazed like the layer. Composited behind
the main layer in the same pass (the main layer's transmittance over it).

**Altocumulus.** A thin mid band (~3.5-4.5 km): small Worley cells in loose rows (stretched across the wind),
marched in a few steps in the same shader after the main layer (thin: cheap), lit like it.

**Storm towers.** Where the storm cells are, the main layer's top rises (to ~6-7 km): cumulonimbus - a tall column
by the storm profile, an anvil spreading at the top (wider with height near the top, flattened), dark bases;
seen from far off over the horizon. The shell's top becomes per-cell (the march's upper bound from the map).

**Stratus / fog banks.** Low sheets: a flat grey layer just above the ground on dull days (the main layer's base
lowered, its profile thin and even), and fog banks hanging near the ground - those stand in front of the scenery,
so the march must go to the scene's depth there (not only the sky's pixels): the last and biggest step.

**Shadows (the old phase 4).** The cloud shadow map from the layers (main, towers, altocumulus; cirrus faintly), so
ground shadows match the clouds seen - after the types, or alongside the first.

## Phases

1. Reading the game's sky (texture ids, shares, log) + weather map 2 + cirrus layer. Demo: a cirrus sky.
2. Altocumulus band.
3. Storm towers (per-cell tops, anvils).
4. Stratus and fog banks (marching to the scene's depth near the ground).
5. Cloud shadows from the layers; in-game tuning against stock dumps (same spot, each weather).

## Open questions

- The game's mapping of weather to its puff textures (needs a few game sessions' logs: which textures per weather).
- Altitudes in AO units (the main layer sits at the eye + 900).
- Fog banks and the game's own fog (the ground fog already hazes; banks must not double it).
