# Cull volumes in hdClaude

Status: implemented, except pruning (section 6), and the GPU unit tests and
the interactive measurement that section 7 lists as not done. Written and
built 2026-10-22.

The schema, the use cases, and the rules every renderer must follow are in
[`extensions/cullVolume/README.md`](../extensions/cullVolume/README.md). This
document covers how hdClaude implements them.

Related documents:

- [Architecture](architecture.md) -- the wavefront kernels this changes.
- [Wavefront integrator](wavefront-integrator.md) -- queues and bounces.

## 1. Requirements

1. **Exact.** Every rule in the README is implemented as written: ray kinds,
   invert, how volumes combine, and linking.
2. **Real time when a volume moves.** Moving, scaling or animating a volume
   must not touch any acceleration structure. Only a small buffer of volume
   transforms changes, and accumulation restarts. Look development relies on
   dragging a region around and seeing the result at once.
3. **Free when unused.** A ray that touches no volume is traced exactly as it
   is today: one opaque ray query.
4. **Faster for inverted regions.** A ray inside an inverted region must not
   traverse the parts of the scene's BVH outside it.
5. **Memory is not a goal of the default path.** The BVH of a large set stays
   resident, culled or not. Dropping culled geometry from memory is a separate,
   opt-in setting (section 6) and gives up requirement 2.

## 2. The method: cull in ray space, not in the BVH

The test is done on the *ray*, not on each hit. Each cube, sphere and plane is
convex, so a ray is inside it for at most one interval of `t`. That interval
comes from a slab test, a quadratic, or a single division, done in the volume's
local frame.

For a ray of kind `k` over `[tMin, tMax]`, and geometry linked to every active
volume affecting `k`, README rule 4 becomes interval arithmetic:

```text
kept = (inverted volumes exist ? union of their intervals : [tMin, tMax])
       minus the union of the non-inverted volumes' intervals
```

`kept` is a short sorted list of disjoint intervals. The ray is traced as one
ordinary opaque query per kept interval, in order, and the first hit wins:

| Ray | Kept intervals | Queries | Effect on traversal |
| --- | --- | --- | --- |
| touches no volume | `[tMin, tMax]` | 1 | none: today's fast path |
| crosses one culling volume | before it, after it | 2 | two shorter queries |
| inside an inverted region | entry to exit of the region | 1 | the BVH outside the region is never descended |
| misses every inverted region | none | 0 | the ray escapes at once |

This is the "if you hit the volume, go through it" behaviour, applied exactly:

- Geometry in a culled interval is never reported, because no query covers it.
- No candidate is handed to the shader, so the opaque fast path holds
  everywhere.
- Nothing in it depends on the BVH. A volume's position only enters through
  the per-ray interval computation, which reads a uniform buffer. That is what
  makes moving a volume real time (requirement 2).

For the look-development case, an inverted cube around a character's hand
with all rays culled, every camera ray starts where it enters the cube, and
every scattered ray ends where it leaves. The body and the rest of the set
stay in memory, but rays never descend into their BVH nodes away from the
cube. This is still true when the hand and the body are one mesh, which no
instance-level pruning could separate.

### 2.1 Linking exceptions

The interval method is exact only for geometry linked to *every* active volume
of the ray's kind. Geometry left out of some volume's `cullLink` (the excluded
pillar, the kept backdrop) has a different culled region, which a ray-wide
interval cannot express. These are rare, and are handled apart using the TLAS
instance mask, which today is only `visible ? 0xFF : 0x00`:

| Mask bit | Meaning |
| --- | --- |
| `0x01` | primary rays, regular: linked to every volume affecting primary rays |
| `0x02` | primary rays, exception |
| `0x04` | secondary rays, regular |
| `0x08` | secondary rays, exception |

- The kept-interval queries use the ray kind's *regular* bit only.
- If the frame has any exception instances for that kind, one more query
  covers the whole `[tMin, tBest]` with the kind's *exception* bit and
  `gl_RayFlagsNoOpaqueEXT`. Each candidate is tested against the instance's
  own link mask and the ray's precomputed per-volume intervals, which is a
  comparison of `t`, not a transform.
- The closer hit of the two wins.

Masks depend on collections and on which kinds each volume affects, never on
where a volume is. Moving a volume never rebuilds the TLAS. Changing a
collection, or a volume's ray flags, rebuilds the TLAS instance list (no BLAS
is touched), which hdClaude already does on every scene publication.

With no exception instances, which is the default since `cullLink` includes the
whole stage, the extra query is never issued.

### 2.2 Ray kinds in hdClaude

| Traversal site | Kind |
| --- | --- |
| `extend`, bounce 0 | primary |
| `extend`, later bounces | secondary |
| `extend`, subsurface random-walk steps | secondary |
| `shadow` | secondary |
| `specular_hit` (reflection guide probes for DLSS) | secondary, so the guides match the image, where a mirror shows a wall that is culled for primary rays only |

Cut-out opacity is decided inside the same candidate loops
(`shaders/cutout.glsl`), not by continuing a path from the shading kernel, so a
camera ray that passes through a leaf is still the one camera ray and stays
primary (README rule 3) without a flag on the path. A cut-out candidate is
asked about its opacity only after its cull links have kept it.

### 2.3 Where it goes in the kernels

All three sites already run a `rayQueryProceedEXT` loop that also intersects
curve and splat boxes. That loop body becomes one function taking
`(tMin, tMax, cullMask, flags)`. A shared helper in `path_state.glsl` computes
the per-volume intervals and the kept list, and calls the function once per
kept interval, then once for exceptions. The shadow kernel's existing
shadow-link candidate test stays inside that function, unchanged.

## 3. Hydra

### 3.1 The adapter

`extensions/cullVolume/usdImagingCullVolume/` is a small library separate from
hdClaude, so it can be proposed with the schema. It contains a
`UsdImagingSceneIndexPrimAdapter` for the three concrete types. It publishes
one Hydra prim type, `cullVolume`, with:

- a `cullVolume` container: `shape`, `size`, `radius`, `axis`, `invert`,
  `affectsPrimaryRays`, `affectsSecondaryRays`;
- the standard `xform`, `visibility` and `purpose` data sources.

UsdImaging's own CollectionAPI adapter adds `collections/cullLink`, with no
code from us. The Hydra schema class is generated with `hdGenSchema`, as
OpenUSD's own are. The adapter maps property invalidations to these
locators.

### 3.2 The sprim

hdClaude lists `cullVolume` in its supported sprim types. Through Hydra's
scene-delegate emulation, `HdSceneDelegate::Get` returns nothing for a custom
prim's own data. OpenUSD's `HdSceneIndexAdapterSceneDelegate::_GetImpl` falls
through to an empty `VtValue` for keys it does not know. So `Sync` reads the
prim's data source from `GetRenderIndex().GetTerminalSceneIndex()`, and a
translator registered with
`HdDirtyBitsTranslator::RegisterTranslatorsForCustomSprimType` turns
`cullVolume`, `xform`, `visibility` and `collections` locators into dirty bits.

What `Sync` changes decides the cost of an edit:

| Change | Work |
| --- | --- |
| transform, size, radius, invert | rewrite the volume buffer, restart accumulation |
| visibility, ray flags | the above, plus recompute instance masks and rebuild the TLAS instance list |
| `cullLink` membership | recompute link masks and instance masks, rebuild the TLAS instance list |
| volume added or removed | the above |

None of these republishes geometry or touches a BLAS. Toggling `invert` does
not change which instances are exceptions, so it stays in the first row.

### 3.3 Linking

Each volume's `collections/cullLink/membershipExpression` is evaluated with
`HdCollectionExpressionEvaluator` over the terminal scene index, against each
geometry rprim's path, at every scene snapshot, and not cached. The default
collection resolves to `//`, which matches everything and skips evaluation. A
collection that includes nothing resolves to an *empty* expression, which in
USD matches nothing, not everything. Only a volume with no published
collection at all falls back to the schema's default of the whole stage.

Membership is decided per Hydra prim, and every placement of that prim
shares it. Prototypes of a native instance or a point instancer are matched
by their own paths, not their instances' paths, which is the limit
`HdsiLightLinkingSceneIndex` has for light links.

Each instance carries a 32-bit link mask, one bit per active volume. A scene
with more than 32 active volumes is reported, and the volumes beyond 32 are
ignored and named. They are not merged or approximated.

### 3.4 Invalid input

A negative `size` or `radius`, an unknown `axis` or shape, or a singular
transform is reported once per prim with its path, whenever the report
changes. That volume is then ignored, as README rule 5 requires: its shape is
`CullShape::None`, and it drops out of the per-kind volume masks the kernels
read.

A volume of size or radius zero is valid. It contains nothing, so it culls
nothing; inverted, it keeps nothing.

Purpose follows the rprims': hdClaude renders the `default` and `render`
purposes, so a cull volume whose purpose is `guide` or `proxy` is inactive.

## 4. Settings

Following the other renderer settings, each has a Hydra setting and an
environment variable.

| Setting | Environment variable | Default | Effect |
| --- | --- | --- | --- |
| `cullVolumes` | `HDCLAUDE_CULL_VOLUMES` | on | Off ignores every volume, for A/B comparison without editing the stage. |
| `cullVolumePruning` | `HDCLAUDE_CULL_VOLUME_PRUNING` | off | Section 6. Not implemented; there is no such setting yet. |

## 5. Validation

Equivalence renders, in the style of `tests/usd/light_linking_equivalence.py`,
gate correctness. Each pair must match within noise, using the same seeds:

1. A volume affecting all rays over the whole of an object, and the object
   deactivated.
2. An inverted volume that encloses the whole scene, and no volume.
3. A volume that contains no geometry, and no volume.
4. A volume whose `cullLink` excludes everything, and no volume. This
   exercises the exception query.
5. Two inverted volumes, and the same scene with only what lies inside
   either of them kept by hand.

GPU unit tests, beside `tests/traversal.comp.glsl`, check the interval
functions and the kept-list construction against a CPU reference. This
includes rays that start inside a volume, rays parallel to a plane, and
grazing hits on a box.

Performance is measured, not assumed: `lookdev_region.usda` with and without
its volume, and the kernel profile (`HDCLAUDE_PROFILE_KERNELS`) for `extend`
and `shadow`.

### 5.1 What was run (2026-10-22)

`tests/usd/cull_volume_equivalence.py` (ctest `usd_cull_volumes`) runs
checks 1 to 5 above with a cube, a sphere and a plane. It adds:

- a mixed case, where the block is excluded and the post beside it is culled
  in the same ray;
- the ray kinds, seen from above with the block out of frame: primary-only
  culling must leave the shadow, and secondary-only culling must remove it;
- three pairs that must *not* match.

All 13 pass. Most matching pairs are identical to the pixel (z = 0), not just
equal within noise. The first run caught one defect: a collection that
includes nothing was read as the whole stage (section 3.3). The other 11
suites passed with the change, so scenes without cull volumes are unaffected.

The GPU unit tests for the interval functions were not written. The
equivalence renders cover those functions end to end, but not edge cases such
as grazing rays and rays parallel to a plane.

Timing, on the gallery kitchen (1,788 instances, 8.7 M triangles; 640 px,
128 spp, 8 bounces), with an inverted cube around the stove that affects all
rays, and a camera framing it:

| | Trace time |
| --- | --- |
| `HDCLAUDE_CULL_VOLUMES=0` | 815 ms |
| on | 620 ms |

24% less trace time. Memory and BVH build time (about 3.3 s) are unchanged,
as section 1 says. A frame that showed mostly empty sky behind the region
took 763 ms without it and 550 ms with it. Those figures say little, because
sky pixels are cheap whether or not they are culled.

## 6. Optional: pruning for memory (`cullVolumePruning`)

Off by default. When on, hdClaude also changes the acceleration structure:

- An instance whose world bounds lie entirely in the culled region for both
  ray kinds is left out of the TLAS. A BLAS no remaining instance uses is
  released.
- An instance only partly in the culled region of an inverted volume may get
  a clipped BLAS that keeps only the triangles overlapping the kept region.

This saves memory and build time for very large sets. Moving a volume then
means rebuilding structures, so it is not real time. That is the trade-off of
turning it on, and the README of the extension says the same.

## 7. Order of work

1. Done. The adapter, the Hydra schema, and the sprim, with linking, the
   setting, and invalid-input reporting. Checked through renders, not by
   printing the scene index, because UsdImaging has no Python bindings in
   this install.
2. Done, without the GPU unit tests. GLSL intervals and kept lists.
3. Done. The traversal refactor in `extend`, `shadow` and `specular_hit`,
   the instance masks, and equivalence renders (5.1).
4. Built, not yet measured. The update path: moving a volume bumps only the
   store's cull revision, which uploads the volume table and restarts the
   film without a scene publication. The measurement on a volume animated in
   usdview is still to do.
5. Performance on `lookdev_region.usda`.
6. Pruning (section 6), only if a real scene needs the memory.
