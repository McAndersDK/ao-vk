# Render-thread batching: per-object caching, retained state, instanced statics

Status: P0, P1 done (2026-10-14), branch `render-batching` (from `statels` 5875a16). Continues docs/device-on-rvk.md:
its phase 3 (retained meshes: indices, mesh handles) is done; this is its phase 4 (retained state) taken further, with
the per-draw derivations cached and identical static meshes drawn instanced. The user wants it "all in".

## Why now

West Bank's city, 2026-10-14 (playfield 790, eye about 1377 2 3026, the profile lines of `logs/randy-vk.log`):
95 fps, about 4,000 draws a frame. The **render thread is the limit**: 10.6 ms a frame; the GPU 6.3 ms; the game thread
does ~4.8 ms of work and waits ~5.7 ms for the render thread. Of the render thread (avg of 600 frames, per-draw
sections sampled one frame in 16):

| section (rvk profile name) | ms | what it is |
|---|---|---|
| draw: geometry + casters | 3.50 | ring copies of non-static vertices / indices; `RecordShadowCaster` (of it: "shadow casters" 2.26 before 5875a16 skipped the statel pieces) |
| draw: state + descriptors + draw | 1.93 | binds, descriptor pushes, the record and indirect command, `FlushGroup` |
| sun shadows | 1.25 | collect 0.64 (CollectShadowItems: keys, sort), cull 0.33, draw 0.17 |
| draw: constants | 0.78 | `DrawConstants` built and compared (the block a state shares) |
| draw: setup (sky, terrain notes, grass tiles) | 0.76 | includes grass tile building (budget 0.6 ms) |
| draw: motion vectors | 0.72 | `prevWorld` lookups, motion flags |
| draw: setup (pick, grass cover) | 0.65 | `CaptureCover`: a 16-float hash + map lookup per static draw |
| draw: setup | 0.57 | ring reservation, arenas, frame lights |
| draw: setup (mesh info) | 0.50 | `DrawMeshInfo`: the mesh cache (by handle for most) |
| draw: light mask | 0.46 | which of the frame's 64 lights reach the draw's box |
| draw: setup (mesh ids) | 0.37 | after af199a8 (was 2.0) |
| draw: setup (canopy, plants) | 0.36 | `CanopyParams`, `SwayParams` / `SubdividePlant` |
| draw: sway | 0.34 | |

Batching already exists: GPU-driven M2/M3 (draw.cpp) - one `DrawRecord` per draw (constants index + `DrawTransform`),
consecutive draws with the same batch key one `vkCmdDrawIndexedIndirect` (`FlushGroup`): 4,330 draws -> 794 indirect
calls in the dump. So *command count* isn't the cost; the **per-draw CPU work re-deriving what didn't change** is.
The dump's "instancing" line: only ~40 draws a frame are identical *neighbours* - instancing must regroup across the
frame to pay.

Ceiling: the GPU at 6.3 ms here -> ~150 fps once the render thread drops under it (the game thread then ~5 ms).

## Facts to build on

- Mesh handles (`RANDYVK_MESH_HANDLES`, rvk `MeshSlot`): a static mesh range arrives as `DrawMesh(id)` - a stable id,
  its static-arena vertices and indices, its mesh info. `m_drawMeshKey`, `m_drawMesh` (MeshInfo: bounds, index hash,
  geo hash), `m_drawMeshStatic`.
- `SetDrawVisual` gives the game's visual (pointer) and kind per draw; native `mesh.cpp` owns `RTriMesh_t` rendering.
- Order-dependent features inside `Device::Draw` (must keep the game's order, or be handled): two-pass foliage
  edges (`FlushEdges`), canopy leaves drawn just before their canopy (`DrawLeaves`), ground grass inserted at the
  game's first blended draw (`DrawGrassTiles`), the terrain's multiply pass (depth EQUAL), the sky (list 0), water
  (`DrawWaterInstead`), decals / coplanar statics (z-fighting decided by order), the depth pre-pass (`PrepassDraw`),
  occlusion queries for the profile (`ShadeQuery*`), picking (`PickDraw`), the frame dump's D lines, render target
  changes (scene / interface layer / offscreen), mid-frame ring flushes (`FlushDrawArenas`).
- The shadow passes are separate (shadow items: own records, M4 batching) - unaffected by main-pass regrouping.
- Tests: `tests/rvk_demo.cpp` - `--shadow-test`, `--cache-test 100`, `--statel-test`, `--bench N` (2,450 draws, direct
  Device API), `--grass-bench`; baselines in the session scratchpad (re-make them at the branch start). The demo has no
  native scene: retained / handle paths need a new bench.

## Phases

### P0. A city bench and baselines

- Demo `--city-bench`: through `ThreadedDevice` with `RegisterMesh` / `DrawMesh` - a few hundred meshes (some
  with cut-out textures, a few blended), ~4,000 draws a frame of which ~70% repeat meshes, 7 point lights, sun
  shadows, the camera turning slowly. Prints the render-thread frame and the per-draw sections; `--shot` for pixel
  comparisons (fixed frame, fixed camera at the end).
- Record baselines (time and images) before any change. Every later phase: pixel-identical where order is unchanged.

**Done (2026-10-14).** `rvk_demo --city-bench` (tests/rvk_demo.cpp `RunCityBench`), shaped after the game's city
frame dump (rvk-frame-085542: 3,029 static draws of 2,067 objects, FVF 0x112, ~37 vertices, 133 distinct sizes -
one 20/30 box alone 909 draws; 85% blended SRCALPHA/INVSRCALPHA writing depth; the ground 320 draws indexing big
static vertex buffers): 2,000 statics of 1-2 pieces from 160 mesh kinds (the first kinds drawn most), 85% blended with
depth writes, 170 fence pieces with holes (two-pass foliage), 40 additive panes; 160 ground chunks in two passes from 4
shared vertex buffers, with the ground grass; sun shadows, 7 point lights; **3,498 draws, all from shared buffers
(3,178 by mesh handle)**; HDR, pixel lighting. The clock is fixed: a run is pixel-identical to the next.
`--city-warm N` (default 60), `--city-no-grass`, `--dump`. `tools/city-bench.sh OUT [BASE]` runs the set (city picture
+ dump, two 700-frame timing runs at 2560x1440, shadow / cache / statel pictures, `--bench 300 --threaded`);
`tools/city-compare.py BASE NEW` diffs pictures (writes *-diff.png) and the render-thread sections.

Baseline `build/rb-base` (ms a frame; the game's West Bank profile beside it - it also has characters, effects and
~560 copied draws the bench hasn't, so its frame is longer):

| section | bench | game |
|---|---|---|
| render thread frame | 6.41 (GPU 3.24: the render thread is the limit) | 10.6 |
| draw: geometry + casters (shadow casters of it) | 2.51 (1.47) | 3.50 (2.26) |
| draw: state + descriptors + draw | 1.79 | 1.93 |
| draw: motion vectors | 0.76 | 0.72 |
| draw: constants | 0.69 | 0.78 |
| draw: setup (pick, grass cover) | 0.56 | 0.65 |
| sun shadows | 0.51 | 1.25 |
| draw: setup / mesh info | 0.38 / 0.36 | 0.57 / 0.50 |
| draw: sway / canopy, plants / light mask / mesh ids | 0.23 / 0.21 / 0.18 / 0.15 | 0.34 / 0.36 / 0.46 / 0.37 |
| draw batching | 3,498 draws in 938 indirect calls, 938 constant blocks | 3,600 in 735, 599 blocks |

(The per-draw sections are sampled one frame in 16 and scaled: their sum overshoots the frame a little.)

### P1. Per-object frame cache (biggest, lowest risk)

Key: (mesh handle, world matrix bits, the state block - P2's id, until then the constants index's content hash).
Kept from the frame before, per object:
- the light mask (valid while the frame lights are the same set at the same places: a per-frame lights hash),
- motion vectors' `prevWorld` (unchanged world: prev = world, no lookup),
- the shadow caster record: key, bounds, cut-out (skip `CasterKey`'s hashing; the caster cache's books by key),
- `CaptureCover`'s lookup result (processed / tile epoch) and the canopy / plant analysis (by mesh handle: once),
- the mesh info (by handle: already).
Invalidated by: world or state change, a settings change (`m_constantsDirty`), a new device / zone (clear).
Check: the city bench's sections; in game the same profile lines. Expect ~2-3 ms off the render thread.

**Done (2026-10-14), staged as randy31-batch1.dll.** rvk/objects.cpp: `ObjectEntry` per (m_drawMeshKey, exact world
matrix) in an open-addressing table (keys probed in a compact array; each lookup first tries the last frame's entry
at its place in the draw order, the next one prefetched - 3,475 of 3,481 lookups a frame in the bench). Kept per
object: the caster key and box (while the cut-out kind / sway stay), the remembered caster (pointer, valid while no
caster was forgotten: m_casterCacheEpoch), the placed-object lookup (m_statelLookupGen), the grass cover record, its
world boxes (DrawWorldBox, FrameLightMask: their own sums), and whether it was drawn there last frame - then its
motion vector is its matrix, no search, nothing pushed for the next frame's search (a still object starting to move:
no motion vector for that first frame). `RANDYVK_OBJECT_CACHE=0` off, `RANDYVK_OBJECT_VERIFY=1` recompute + log.
With it: camera checks by a generation (m_cameraGen: WorldCamera, AoCamera, caster views), the orthographic cascade
cull by box centre and extents, ProfileAdd by name address.
Fixed on the way: caster streaks starting on frame 1 never remembered; TerrainChunkKey made chunks indexing one static
buffer with equal counts one chunk (the ground grass only under the first; one base texture) - now with the snapshot
and an index sample; the object table's growth dropped every entry.
Result (city bench, render thread, all test pictures identical cache on/off): 6.27 -> 5.08 ms (motion vectors
0.58 -> 0.11, shadow casters 1.35 -> 0.73, grass cover 0.45 -> 0.27, cull 0.15 -> 0.06; the lookup costs 0.17).
Not done (small in the bench): the light mask by a lights generation, canopy / sway analysis by mesh. Tools:
`tools/profile-demo.sh` (perf by function, rvk_demo.map) - the per-draw sections include the profiler's own clock
reads (the casters' nested timer especially): judge by the whole frame and perf.

### P2. Retained state blocks (device-on-rvk phase 4b)

- A draw's `DrawConstants` minus the per-frame parts (view / proj / eye / fog / ambient: move those to the frame
  block where shaders allow) is the material's: build once per native `RMaterial_t` / `RDeltaState` (proxy native
  material.cpp) and hand rvk a block id with the draw (`SetDrawState(id)`), rebuilt on change.
- `draw: constants` becomes a lookup; the batch key uses the block id.
- Careful: what the shaders read from `DrawConstants` per draw today (lights, fog per draw, tfactor, stage states);
  the game changes some state outside materials (blend for effects, fog) - those draws keep the loose path.

### P3. Instanced opaque statics

- During the opaque phase, an eligible draw isn't recorded at once: it is appended to a bucket keyed by (mesh
  handle, block id, pipeline state, texture set). At the end of the opaque phase - the game's first blended draw,
  where the grass tiles go in - each bucket is one instanced indexed draw (`instanceCount` = objects; per-instance
  `DrawRecord`s contiguous; `firstInstance` = the first record).
- Eligible: static (handle), opaque (no blend), depth-writing, depth test LESS/LESSEQUAL, no alpha test or a
  cut-out the pipeline handles per instance, world camera target (the scene), not terrain / sky / water / leaves'
  canopies / two-pass foliage / decals (coplanar: z order), not picked, not during a frame dump's D lines (or dump
  them in bucket order).
- The depth pre-pass: buckets drawn into it too (instanced), before the colour pass as now.
- Light masks per instance (in the record), motion vectors per instance (prevWorld in the record).
- A setting `RVK_Instancing` (Experimental, default off until in-game A/B shows no difference); the frame dump says
  how many draws went into how many instanced calls.
- Pixel comparison: order changes, so equal depths may resolve differently (z-fighting) - compare with a tolerance and
  look at differences; coplanar pairs must stay out (detect: same mesh box plane, or simply exclude tiny-thickness
  meshes).

### P4. The game thread (optional, after P3)

- Native `TriMeshRender` hands a static mesh as (handle, world, state id) in one small record, skipping
  DeviceState and the per-draw hand-off of states. Only if the game thread becomes the limit after P1-P3.

### P5. GPU culling (device-on-rvk phase 5; optional)

- With buckets retained across frames, frustum (and last frame's depth) culling in a compute pass writing the
  indirect commands. A setting, off by default.

## Risks and how to see them

- Order-dependent draws regrouped: z-fighting, blending wrong, missing grass or leaves -> strict eligibility, A/B
  setting, demo pixel tests, frame dumps before / after at the same spot.
- Stale cache entries: state changed but the key didn't -> the key includes everything the cached values read;
  `m_constantsDirty` clears all; a debug setting that verifies (recompute and compare, log mismatches).
- Mid-frame ring flushes (`FlushDrawArenas`) with buckets pending -> flush buckets first.
- Render target switches inside the opaque phase (offscreen renders, water's scene copy) -> flush buckets before.

## Measuring

The render thread's sections (profile lines every 600 frames: "cpu ms (frame recording)"), "draw batching" (draws,
indirect calls), "presented ... fps, draws/frame". Same spot each time: West Bank, eye ~(1377, 2, 3026) looking at the
city; and the forest at (1372, 3, 3010).
