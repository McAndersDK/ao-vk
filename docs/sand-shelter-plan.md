# Sand that knows the land: shelter behind hills and objects, flow round them

Status: plan (2026-10-15), branch `storms`. The user: the blowing sand and the wind ignore terrain and objects -
standing by a hill there should be no sand on its lee side, and sand should wrap round a tree. Moving off the smoke
simulation to something procedural is fine.

## Today

- The sand volume (smoke.cpp UpdateSandVolume, smoke_sim.comp sand mode): a 32-unit box of a 3D fluid solver round
  the camera, 48-128 cells a side. It knows the ground under each column (the grass's terrain capture, flat without
  grass) as a floor, and characters as small obstacles. The wind comes in over every wall alike; a hill upwind or a
  tree beside you changes nothing.
- Drawn by smoke.frag (a march through its density image, lit by the sun's light image), the grain (SandGrain) laid on
  top along the wind.
- The haze (tonemap.frag dust): distance only.
- Cost ~0.5 ms (80 cells), up to ~4.6 ms (128). Characters are a cell wide: the grid can't hold a tree trunk.

## The idea

Drop the 3D solver for the sand. Instead, each frame:

1. **A shelter map** (2D, round the camera): for every ground point, how sheltered it is from the wind by what stands
   upwind - hills, rocks, walls, buildings, trees - and up to what height.
2. **A wind field** (2D, same map): the storm's wind steered round obstacles (a cheap 2D flow solve on the map: it
   goes round a trunk, through a gap, over a ridge), slowed in the lee.
3. **The sand, procedural**: density at a point = streak noise carried along the steered wind (the grain we have, on
   a flow map) x the height falloff over the ground x (1 - shelter at that height); written into the volume's density
   image by a compute pass, so smoke.frag draws and lights it as now.

What stays: smoke.frag's drawing and lighting, SandGrain, the haze, the dust devils (rewritten as procedural vortices
on the wind field), the feet kicking sand up (puffs), RVK_SandBlow / RVK_SandRes / RVK_SandForce.

## 1. The obstacle height map

A 2D grid round the camera aligned to the world: 128 x 128 cells of 0.5 units (64 units) near, plus the terrain
alone further out for hills. Per cell: the ground's height and the top of whatever stands there. Built from:

- **The ground**: the grass's terrain capture near (GroundAt, ~0.5 unit); beyond and without grass, terrain_sun.cpp's
  height grid (16 units a cell, 4 km). (A third source if the grass is off near: the terrain draws' own vertices, as
  terrain_sun captures them.)
- **Objects**: the statel packs (statels.cpp: every placed rock, wall, building, tree with its mesh's shadow
  pieces) - rasterised top-down into the grid: for each cell the highest point of anything over it, and how solid
  (a trunk's footprint solid; a crown's cut-out canopy part porous - wind and sand go through foliage, slowed). Done
  on the GPU from the pieces already in the statel buffer (a top-down depth render of the pieces within the map,
  max height, with a "porous" flag from cut-out pieces), only when the map moves a cell or the playfield changes.
- **Characters**: as now (small, moving), stamped each frame.

## 2. Shelter

Per cell, along the wind: how high the upwind obstacles reach above this cell, falling off with distance - the lee
of an obstacle h tall is sheltered out to ~6-10 h behind it (wind tunnel figures for walls and hills), less behind
porous ones (trees ~3-5 h, 50% at most). A march upwind over the height map (the terrain_sun "horizon" march, along
the wind instead of the sun): shelter(cell) = the height under which the cell is sheltered. Sand density at a height y
above the ground is cut by smoothstep over that height. Hills far upwind come from the coarse terrain grid (a ridge
200 units off shelters a valley). Recomputed when the wind turns more than a few degrees or the map moves; smoothed in
time so a turning wind moves the lee slowly.

## 3. The wind field round obstacles

A 2D incompressible flow on the map (the solver's projection, in 2D: 128 x 128, ~20 Jacobi steps - a fraction of the
3D solver's cost), solids blocking, porous cells slowing it. Gives the deflection round a trunk or a building corner,
the speed-up past it and the gusty lee. The sand noise is advected along it as a flow map (two phases, as smoke.frag
Detail does), so streaks bend round a tree and stream past its sides. The grass, plants and trees could sample it too
(sheltered grass still in the lee of a wall) - a later step.

## 4. The sand volume, procedural

A compute pass fills the existing density image (the drawing unchanged): per cell, its height over the ground, the
shelter there, the flow-mapped streak noise, a thick layer in the bottom unit and dust above; the dust devils as
vortices in the wind field with their columns; footfalls as puffs fading over a second or two. No solver state: the
box moving with the camera has nothing to carry across, any cell count costs about the same, and the grid only needs
to be as fine as the noise (a coarse grid gets the grain from SandGrain as now).

The haze: the near veil (dust2.z) cut by the shelter at the camera, so standing in a hill's lee the air close by
clears while the distance stays thick.

## Phases

1. **Obstacle map**: ground from the two terrain sources, objects from the statel packs (top-down GPU raster), debug
   view (Ctrl+Shift+K-style overlay or a frame dump image). Check against hills, walls, trees in game with the free
   camera.
2. **Shelter**: the upwind march, applied to the current sand (multiply the sim's inflow and ground pick-up by it) -
   the quickest visible win: no sand behind hills and walls.
3. **Procedural sand**: the compute fill in place of the sand solver (behind a setting until it matches), the dust
   devils and footfalls redone, the solver kept for smoke.
4. **Flow round obstacles**: the 2D wind field and the flow-mapped noise; the near haze by shelter.
5. **Others reading it**: grass and plants calmer in the lee, rain slanting less behind walls (optional).

Each phase: demo scenes (a wall and a tree in a sandstorm: tests/rvk_demo --sand-storm with obstacles), the harness,
validation, cost (target: the whole sand under 0.6 ms at any RVK_SandRes).

## Risks

- Statel packs don't cover everything (animated meshes, the city's dynamic objects): the depth of what the game draws
  near could fill gaps later.
- Terrain capture holes far from grass: the coarse grid covers hills; small dunes may be missed.
- Indoors / under roofs: a roof is an obstacle from above - the height map's "top" makes a covered cell fully
  sheltered below it (as wanted).
