# Trees: our own canopies in place of the game's cards

Status: plan (2026-10-15), branch `storms` (user: this work goes there). Trees only - shrubs and palms keep today's
leaves (leaves.cpp). New leaf art per tree type (user), not cut from the game's textures. Under RVK_LeafOn
(Experimental) until tuned; the game's canopy comes back with it off, and beyond the leaves' distance.

## Why

An AO tree is two meshes: a bark trunk and a canopy of a few big alpha-tested cards with a painted cluster of leaves on
each (a 20-unit jungle crown is 54 triangles). Today's leaves (leaves.cpp, 0.4) scatter sprigs cut from that texture
over the cards and keep a thinned core of the cards. Up close the crown still reads as flat cards, the painted leaves
are blurry, and in wind the crown slides as a block over a trunk that doesn't move (in a storm, several units: it
detaches - the user's "glitchy against the wind").

## What there is to build on

- Canopy detection (leaves.cpp CanopyKind): lit, static, depth-writing, mostly-holes texture, upright, tree <= 45 units.
- Its trunk (NoteSolidDraw / LeafGround): the solid draw standing under it, remembered per instance.
- The leaf pool (LeafRecord, 32 B a sprig), bake per mesh + texture, a shuffled order so any first part is an even
  thinning (distance LOD), drawn through the canopy's own vertex path (leaf.vert = ffp.vert -DRVK_LEAF): its lights,
  fog, shadows, PBR. Leaf sun shadows per cascade (leaf_shadow.vert). Falling leaves.
- Tree identity: every texture's RDB id (Texture::m_rdbId) and mesh ids (meshids.cpp: geometry hash -> rdb mesh and
  name, e.g. 'jungle_talltree_20m.abiff').
- One wind (wind.cpp): direction, strength, gust fronts (wind.glsl Gust), shared with the grass.
- ao-assets: rdb.db access, generators (`ao-assets generate`), the workbench viewer.

## Design

### A tree type = its canopy texture

Many tree meshes share a leaf texture (336 meshes are named *tree*, many of them dead, fallen or stumps; the living
canopies use far fewer textures). The type is keyed by the canopy texture's RDB id; the mesh name only refines it (a
"leaning" or "small" variant: size). A profile per type (randy-vk/trees/types.ini, hot-reloaded like materials):

- leaf: shape (oval, long and narrow, round, lobed (oak-like), compound (fern-like fronds of leaflets), needles,
  broad tropical), size (world), colour (from the game texture's leaf texels - keeps the zone's palette - with a
  spread: yellower young tips, darker old leaves), gloss, translucency.
- sprig: leaves per twig, their arrangement (alternate, opposite, whorled, a fan), twig colour and length.
- crown: density, how far out leaves stand from the cards, droop.
- Types without a profile: a default built from the texture's colour and the crown's size.

### The leaf art (generated, ao-assets)

`ao-assets generate tree-leaves`: per profile, an atlas of sprigs - 8 to 16 variants, 256 px each - painted
procedurally: the leaf outline (from the shape), a midrib and veins, a colour gradient (base to tip, edge darkening),
serration for lobed and oval leaves, the twig; plus a normal map (the veins and the leaf's fold) and a translucency
mask. Files: randy-vk/trees/<type>.png (+ _n.png). Mipmapped with alpha kept (coverage-preserving mips, so far leaves
don't thin out). Viewed and tuned in the workbench before the game. Art style: realistic-leaning, but colours from the
game's own textures so it sits in AO's zones; this is the first thing to look at with the user (phase 1).

### The crown (baked per canopy mesh)

- Where the foliage is: points over the game's cards where its texture is leaf (alpha), as today, giving the crown's
  shape the artist made. From them: the crown's hull (an ellipsoid fit, plus the points' density), its centre, and
  clusters (k-means, 20 - 200 by size) = branch ends.
- Sprigs: placed mostly on an outer shell of each cluster (where leaves get light), sparser inside; turned to face out
  from the cluster and up, with a random twist; droop by profile. Each sprig records its cluster (a branch: sways
  together) and how deep in the crown it is (shading).
- Counts: a 20-unit tree ~2,000 - 5,000 sprigs at full density, shuffled for the distance thinning (as today).
- The game's cards are not drawn where ours are (the canopy draw skipped while its leaves are drawn); a crossfade
  (dithered, both drawn for ~10% of the leaf distance) where ours end.

### Shading

- Normals: each sprig's own (normal map) bent towards the direction out from the crown's centre - the crown shades as
  one rounded mass (lit side, shadow side), not card by card.
- Depth in the crown: inner sprigs darker (ambient), a cheap stand-in for the crown shadowing itself; the sun shadow
  map (leaf shadows, existing) does the rest.
- Translucency: sun behind the crown -> the leaves glow at its edges (RVK_LeafLight's backlight, per leaf).
- Colour spread per sprig and per tree (no two trees identical).

### Wind (the trees' part of the storm work)

- The trunk bends: the trunk draw sways with the crown (height-squared bend from its base, the same function for the
  trunk, the canopy's crossfade and our sprigs) - nothing detaches.
- In a storm the tree leans downwind and the lean levels off (as the grass, wind.glsl GrassGusts); swings around the
  lean, never far upwind; gust fronts (Gust, shared) push it further as they pass.
- Three scales: trunk (slow), branches (per cluster, own phase), leaves (fast flutter, per sprig); all livelier with
  the storm.
- Falling leaves: in our art, more in gusts, carried along the wind in a storm.

## Phase 0 result (2026-10-15): ao-assets `python -m aoassets trees` (work/trees: inventory.tsv, types.md, sheet.png)

2.13 M placed objects; tree-sized canopies (3..45 units) placed 380 k times, in 143 textures - most not foliage (wreck
debris, gratings, crystals, glass: 100 textures, 9 k placed). Foliage: leaves 33 textures (295 k placed), bare
branches 5 (64 k: the "spooky" leafless trees), hanging moss 5 (12 k). The leaves are concentrated:

| # | texture | placed | playfields | tree heights | look |
|---|---|---|---|---|---|
| 1 | leaves_hires (1010004:6269) | 132 k | 64 | 7.5 - 31 | small olive-yellow leaves on twigs |
| 2 | applestyletrees_newleaves (42116) | 94 k | 49 | 3 - 44 | small dark green leaves, dense |
| 4 | mcclean_leaves (42065) | 17 k | 50 | 8 - 18 | small dark green |
| 6 | frazettatreeleaves (24590) | 11 k | 33 | 14 - 19 | broad rounded leaves, mottled green |
| 7 | enigmatreeleaves3 (42840) | 8 k | 4 | 3 - 44 | teal alien leaves |
| 9 | curvedtrees_leave1_opac (20983) | 7.6 k | 39 | 7 - 10 | broad, oak-like |
| 10 | deserttree_smallestleaves (26301) | 5.7 k | 18 | 4.3 | dry, brown-orange, small |
| 11 | penumbra_frozen_jungletrees_leaves (207223) | 4.8 k | 4 | 25 - 43 | frosted white |
| 12 | dryleaves (26302) | 4.7 k | 30 | 4 - 9 | autumn brown |
| 13 | new_leaves_cluster2 (206288) | 3.8 k | 57 | 12 - 43 | small yellow-green, airy |

The first two are 77% of the leafy trees placed, these ten ~98%. The tail (23 textures, ~5 k placed) takes the default
profile. Name-sort errors to remember: potteplant_branch1 is a leafy plant, smushroom_texture_deadleaf a giant
mushroom's leaf (not a crown).

## Phase 2 (2026-10-15): our own canopy drawn

- Art: rvk/material_maps.h + proxy rvk_materials.cpp - <type>_<id>_leaves.png / _leaves_n.png in randy-vk\materials\
  (ao-assets `leaf-art --install`), decoded off-thread, Device::SetLeafArt on the canopy texture (hot reload as the
  other maps).
- leaves.cpp BakeOwnCanopy: a Tree-kind canopy whose texture has art (RVK_LeafOwn, default on): sprigs where the
  game's cards have leaves (alpha >= 0.5), size 0.11 x the crown's height (1 - 3.2 world, x RVK_LeafSize), 4.5 x
  RVK_LeafDensity / (0.45 size^2) per card area; twig base on the card, reaching out of the crown's ellipsoid and
  lying 15-45 degrees off its surface, drooping; a random atlas cell.
- draw.cpp: a second record for the leaf pass (texIdx = atlas, its normal; no PBR maps); leafSet[1] bit 31 = own.
  ffp.vert: the cards thinned by LeafKeep (none near; a dithered crossfade out to RVK_LeafDist), the sprigs' flutter
  at their tip, no round mask. Shadows: the cards' shadow thinned alike (origin.w), the sprigs cast with the atlas.
- Fixed on the way: the cut-out depth pre-pass ignored the canopy thinning mask - dropped card pixels hid what was
  behind (sky-coloured blocks through the trunk; the old core thinning had a few too).
- Demo: --leaf-art ATLAS[,NORMAL], --leaf-no-own. 25 trees (jungle 6273 + 13147, 1751 sprigs each): scene GPU
  0.33 -> 0.66 ms. Falling leaves off for own canopies (a whole sprig would fall) - later.

## Phases

0. Inventory: a log line per tree canopy type met in game (texture id, mesh name, size, count) and an ao-assets scan of
   the tree meshes' canopy textures -> the type list, with thumbnails in the workbench. Pick the first 6 - 10 types
   (the common zones' trees) for profiles.
1. Leaf art generator + workbench preview; the user picks the look on a few types.
2. Crown bake + replacement drawing for trees (the game's cards off near, crossfade far); default profile for the
   rest. Demo: tools/extract-tree.py trees, --leaf-tree.
3. Shading: crown normals, depth darkening, translucency, colour spread.
4. Wind: trunk bend, storm lean, three scales; falling leaves in the new art.
5. Profiles for the remaining types; tuning in game; costs (target: a forest view <= 1.5 ms GPU at default density).

Each phase: the demo (pixel-identical where trees aren't involved), the harness, crash and hang reports, validation.

## Risks

- Canopies the classifier gets wrong (a tree's dead branches, vines, signs): the inventory shows them; per-type
  "leave alone" in types.ini.
- Trees whose leaf texture is also used by shrubs: keyed by texture + size class.
- Cost of many sprigs in forests: the distance thinning and the crossfade to the game's cards bound it; shadows draw
  fewer.
- Trunks the pairing misses (the crown then can't bend with it): fall back to swaying the crown only, gently.
