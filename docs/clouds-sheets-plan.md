# The game's flat cloud sheets, back (clouds v2)

Status: planned (2026-10-13). Builds on the cloud layer (`docs/clouds-layer-plan.md`, `rvk/clouds.cpp`,
`cloud_layer.frag`).

## Why

The game's sky is ~30 flat sheets (VisualSingleCloud_t: hundreds of units across, within ~1.5 km of the eye, at
about its height, kept round it) textured with its fibrous puffs (single_cloud01-03) and streaky wisps (04). Together
they make a broad soft veil over much of the sky - brightest overhead, greying to the horizon (user dumps
rvk-frame-134853 stock / 134854 ours). The layer replaced them with heaped cloud only; the veil is gone, and with it
much of the game's look.

## Decisions (user, 2026-10-13)

- **Redraw the game's sheets** - its geometry, textures and placement, as the stock sky - behind the layer's heaped
  clouds, coloured by our lighting (white by day, the sunset colours and afterglow, the night's brightness, the air's
  haze). Not a procedural high layer. Kept round the eye as the game keeps them.
- **Both, fewer heaps**: the sheets always there as the game draws them; the heaped clouds a little sparser.
- **Faint shadows** from the sheets.

## Design

**Capture (exists).** `CaptureCloudPuff` already keeps every sheet each frame (`CloudPuff`: world/view/proj, mesh
extent, thin axis, texture and its UV planes, the game's lit colour and opacity, its fog) and skips the game's draw.
Keep the vertices/indices too (or rebuild the quad from extent + UV planes: the sheets are flat 100-vertex grids -
check whether they bend; if so keep the mesh).

**Draw (new, `cloud_sheet.vert/.frag`).** In the clouds' own pass, before the layer is composited
(`DrawClouds` / cloud_up.frag): each sheet drawn as the game drew it (its matrices, depth tested against the
pre-pass and clamped inside the far plane as kMatSky: only where the sky shows), blended premultiplied over the sky.
- Alpha: the texture's alpha x the game's opacity x `RVK_CloudSheet` (setting, 0..2, default 1).
- Colour, per pixel, as the layer lights its clouds (shared functions moved to a `cloud_light.glsl`):
  the whitened game tint (30% hue, the day floor), the sky's light (x NightSky by night), the sun (SunLight: golden
  then pink low) through a thin sheet - forward scattering towards the sun (bright, silvery round it), thicker
  (more alpha) darker beneath; the Afterglow on them round sunset.
- Air: Aerial() by the view direction's height (low: the fog's colour, the horizon band), opaque as they are.
- Under the layer: the layer's heaped clouds composite over them (its alpha hides them) - the sheets the high veil.

**Fewer heaps.** With sheets drawn, the layer's dry-day cloudiness 0.42-0.47 -> ~0.32-0.36 (tuned against the stock
dumps: how much open sky).

**Faint shadows.** The sheets are kept round the eye - a shadow straight from them would slide with the player. So:
in the cloud shadow map, the sheets' texture (single_cloud01, tiled ~2 km, fixed in the world, drifting with the
layer's wind) x how much sheet the game shows (its puffs' mean alpha) x 0.25 - multiplied with the layer's shadow.

**Settings.** `RVK_CloudSheet` (0 = none, as now; 1 = as the game; up to 2). Vanilla: unchanged (clouds off there).

## Phases

1. Sheets drawn with our lighting (day, night, sunset), the air's haze; demo `--cloud-puffs` sheets like the game's
   (flat, textured); `RVK_CloudSheet`. Cost check (~0.1 ms at 1440p).
2. Fewer heaps under them; tune against the stock dumps (same spot: stock vs ours).
3. Faint world-fixed shadows.

## Open questions

- Do the sheets bend (100 vertices, 486 indices each - a grid, maybe curved)? Decides quad vs mesh.
- Their texture's alpha holds the shape; the colour channels - plain white or shading? (Read single_cloud01-04.)
- The game fades sheets in/out by distance (vertex alpha?) - keep it.
