# hdClaude code review and follow-up plan

Review date: 2026-10-26  
Reviewed revision: `dab68b723f89ffed8ede66baf133e172eb5cc35f`

## Assessment

The project has a clear separation between Hydra ingestion, MaterialX compilation,
GPU transport, and dependency-free core code. Its numerical, shader, and Vulkan
validation tests are substantial. The most useful next work is in interactive
state transitions and host integration: these are less directly exercised by the
existing transport tests and static gallery renders.

This is a targeted source review, not an exhaustive audit or a certification of
rendering correctness. No renderer source was changed. Findings below distinguish
defects established by tracing the code from issues that still need an integration
reproduction. Proposed acceptance tests are work to do, not tests already passed.

Priority meanings: **P1** should be addressed first because it can invalidate an
interactive result; **P2** is a concrete correctness or robustness fix;
**P3** is lower-impact hardening or maintainability work.

## Second reading

Every finding below was re-read against the cited source at the reviewed
revision. Each cited line range points at the code described, and none of the
findings misreads it. Adjustments to scope and priority:

- **R1** is confirmed: `previousObjectToWorld` is written only by `SetScene`,
  while the camera's history advances every frame. It matters wherever the
  motion guide is consumed -- DLSS, or a host reading the motion AOV.
- **R2** is confirmed, and `Clear` lacks a Float16 case as well as `Write`. The
  delegate's default colour AOV is Float32Vec4, so it reaches only a host that
  requests half-float colour explicitly (Houdini and husk can).
- **R3** is confirmed. The loop visits only the descriptors still present, so a
  primvar that disappears is never looked at, not only one that resolves empty.
- **R4** is confirmed. Low impact on a machine that has OpenUSD, but the README
  claim is untrue until it is fixed.
- **R5** is confirmed, and lowered to **P3**: only hdClaude writes cache entries,
  so a damaged count needs a damaged disk or a foreign file. `wordCount` is
  32-bit, so the worst request is 16 GB and an uncaught `bad_alloc`.
- **I1** is well-founded: `_Execute` ignores `renderTags`, meshes record no
  purpose, and an rprim excluded by a later toggle is no longer synced, so
  whatever it published stays published. Batch renders with `--purposes render`
  are unaffected because excluded prims are never synced to begin with.
- **Performance P1 and P2** are both confirmed and are promoted ahead of I1:
  every publication re-uploads every texture synchronously, and
  `HdClaudeMesh::Sync` consults no dirty bit, so a transform edit regenerates
  the mesh. Between them they set the cost of an interactive edit.
- The P3 and P4 checklists are follow-up work rather than findings; their
  references (the installed-plugin note, roadmap questions 8 and 10) are
  accurate.

Revised order: R2, R3, R1, texture reuse, incremental mesh sync, R4, R5, then
the I1 reproduction. Each item is ticked here in the commit that fixes it.

## Validation performed

The existing `build/dev` binaries passed these CTest suites:

| Suite | Result | Approximate duration |
| --- | --- | --- |
| `core` | Passed | 1.27 s |
| `glsl_compiler` | Passed | 0.29 s |
| `geometry` | Passed | 1.18 s |
| `gpu` | Passed | 1.90 s |
| `materialx` | Passed | 1.16 s |
| `closure_validation` | Passed | 33.04 s |

Commands, with CTest invoked from the installed Visual Studio CMake directory:

```text
ctest --test-dir build/dev -R "^(core|glsl_compiler|geometry|gpu|materialx)$" --output-on-failure
ctest --test-dir build/dev -R "^closure_validation$" --output-on-failure --timeout 90
```

These were runs of existing binaries, not a fresh rebuild. The full `render`
suite, USD integration suites, gallery, Linux build, and DLSS image behavior were
not verified to completion during this review. An earlier combined closure/render
run was interrupted by the tool/server restart; only the subsequently completed
closure run is counted above.

A targeted render-buffer executable was prepared, but compilation was blocked by
the unavailable Python development header `pyconfig.h` required by this OpenUSD
distribution. The AOV findings therefore remain source-verified, not runtime
reproductions. Temporary probes are under the gitignored
`build/review-2026-10-26/` directory.

One suspected shader-cache issue was explicitly ruled out: storing over a corrupt
entry recovered successfully on this Windows toolchain. A small executable built
from the current core sources reported
`miss_before=1 store_success=1 hit_after=1`. Do not treat replacement of an existing
cache file as a confirmed Windows bug.

## Findings and required fixes

### R1 — P1: object motion remains nonzero after an object stops

**Evidence:** source-verified lifecycle defect; visual DLSS impact not measured.

Relevant code:

- [Scene snapshots](../src/hydra/scene_store.cpp), lines 420–437 and 574–580.
- [Instance-table upload](../src/gpu/path_tracer.cpp), lines 1175–1180 and 1213–1218.
- [Scene-upload condition](../src/hydra/render_pass.cpp), lines 873–881.
- [Motion guide](../shaders/guides.comp.glsl), lines 91–115.

Previous object transforms are captured between **scene snapshots**, but the guide
uses them as the previous **rendered frame's** transforms. After an object moves
from A to B, the uploaded instance table contains current B and previous A. Later
progressive frames reuse that table until another scene publication. Even with a
stationary camera and object, the shader keeps reporting the A-to-B displacement.
Camera history advances independently, so it does not correct this stale object
history. The same issue applies to splat instances.

This supplies incorrect motion vectors to temporal reconstruction after a single
interactive transform edit. Ghosting or history rejection is a likely consequence,
but requires a reconstruction-enabled reproduction to quantify.

**To do:**

- [x] Make previous placement state correspond to the last successfully rendered
  frame, rather than the last scene publication.
- [x] Advance or disable the object-motion contribution after its first consumed
  frame without rebuilding BLAS/TLAS or reuploading all textures. One
  device-to-device copy of the moved entries' placements, when the camera
  history advances.
- [x] Define behavior for a failed frame and for a scene publication that is never
  rendered, so neither accidentally advances history. The store stages a
  snapshot's placements and commits them only after a frame completes
  (`HdClaudeSceneStore::CommitHistory`); the tracer advances only after its
  readback. Not covered by a test, as it needs a publication that throws.
- [x] Add a fixed-camera, fixed-jitter three-frame test: A, move to B, remain at B.
  Require the expected displacement on the moving frame and approximately zero
  motion on the stationary frame. Cover meshes and splats. In `render`: -3.200 px
  moving (-3.200 expected), 9.4e-07 at rest, and 3.20 px at rest with the copy
  disabled. Meshes only; splats share the table and the copy, and are not
  tested separately.

### R2 — P2: half-float color buffers are accepted but never written

**Evidence:** source-verified; targeted runtime probe blocked as described above.

Relevant code: [render_buffer.cpp](../src/hydra/render_buffer.cpp), lines 19–41,
195–238; [render_pass.cpp](../src/hydra/render_pass.cpp), lines 179–188.

`Allocate` accepts a valid `HdFormatFloat16Vec4` buffer. `Write`, the color output
path, handles Float32, UNorm8, and Int32, but has no Float16 case. It marks the
buffer `_written = true` and returns through the default branch without storing
the color. A host selecting half-float color therefore receives the zero-filled
allocation, and `HoldsNoFrame()` incorrectly reports that a frame was supplied.
The scalar and vector-data writers already support Float16, making the color path
inconsistent with the rest of this class.

**To do:**

- [x] Add half-float conversion to the color writer and supported clear paths.
- [x] Make unsupported output formats fail explicitly or emit a diagnostic.
  `Write` returns false and the render pass warns once, as for depth.
- [x] Set `_written` only after a supported write actually succeeds (`Clear`
  too).
- [x] Test Float32Vec4 and Float16Vec4 with fractional values and values above 1.
- [x] Test clear, write, reallocation, and late AOV binding behavior directly.
  The new `hydra` suite (`tests/hydra_tests.cpp`) covers clear, write and
  reallocation, and fails 47 of its 101 checks against the old source. Late
  binding is the render pass's, and stays covered by `usd_depth`'s
  buffers-replaced case.

Related **P3** issue: the UNorm8 branch applies the sRGB transfer function to all
components, including alpha (lines 209–228). An input alpha of 0.5 would become
approximately 188/255 rather than 128/255. Encode RGB only. This is currently
latent for ordinary frames because the film accumulates opaque alpha; do not
describe it as an observed transparency regression. **Fixed with R2.**

### R3 — P2: removed instancer primvars retain their previous values

**Evidence:** source-verified cache invalidation defect.

Relevant code: [instancer.cpp](../src/hydra/instancer.cpp), lines 39–52 and 115–153.

`_SyncPrimvars` only adds or replaces nonempty values for descriptors returned by
the scene delegate. It never erases an old entry when a value becomes empty or
its descriptor disappears. `ComputeInstanceTransforms` then continues using the
cached scale, rotation, translation, or transform.

For example, removing an authored instance scale during an interactive edit can
leave instances scaled until the instancer is recreated. Static instancing tests
do not exercise this transition.

**To do:**

- [x] Remove cached entries whose descriptors have disappeared during a relevant
  primvar update.
- [x] Erase an existing entry when a dirty primvar resolves to an empty value.
- [x] Preserve unchanged entries during incremental updates.
- [x] Test author → render → remove/block → render against a newly created stage
  with the same final authored state. Cover scale, rotation, and transforms.
  Done at the adapter, in the `hydra` suite: an instancer synced, edited and
  synced again must compute what a new instancer computes from the final
  primvars, for each of the three removed and blocked. The old source fails
  all six.

### R4 — P2: the core-only build driver still requires OpenUSD setup

**Evidence:** source-verified build-flow defect; not tested on a clean machine.

Relevant code: [compile.py](../compile.py), lines 135–158;
[setup_usd_env.bat](../setup_usd_env.bat), lines 39–50 and 67–103;
[CMakePresets.json](../CMakePresets.json), `core-only` preset.

The README advertises `compile.bat core-only` as dependency-free. However, every
non-Houdini preset calls `usd_environment()` before configuration, including
`core-only`. On Windows, that setup exits when OpenUSD is absent and also enforces
the distribution's Python version. The later configure arguments require
`USDROOT` and `USDEXTRA` from that setup even though core-only disables the USD,
MaterialX, and Vulkan subsystems.

**To do:**

- [ ] Give core-only a toolchain-only environment path.
- [ ] Pass USD and plugin-install configuration only to presets that need it.
- [ ] Test the documented core-only command with a supported C++ toolchain and
  build-driver Python, but no OpenUSD installation or USD environment variables.
- [ ] Verify the same behavior through `python3 compile.py core-only` on Linux.

### R5 — P3 (was P2): a corrupt cache header controls allocation before validation

**Evidence:** source-verified robustness issue; no oversized allocation attempted.

Relevant code: [shader_cache.cpp](../src/core/shader_cache.cpp), lines 96–109;
[shader_cache.h](../include/hdclaude/core/shader_cache.h), lines 51–54.

After checking the header's magic, version, and nonzero count, `Find` immediately
allocates `vector<uint32_t>(header.wordCount)`. It has not checked whether the file
contains that many words. A tiny damaged entry with a large count can therefore
request many gigabytes of memory before being rejected as truncated. That
contradicts the cache's stated contract that corrupt entries degrade to a miss
rather than a rendering failure.

**To do:**

- [x] Validate payload length and checked size arithmetic before allocating. The
  file must be exactly its header and `wordCount` words, computed in 64 bits,
  before the vector is made.
- [x] Establish a reasonable maximum cached module size, or another explicit
  allocation limit consistent with supported materials. The entry's own size on
  disk is the limit: nothing larger than the file is allocated, so no separate
  cap is needed.
- [x] Treat malformed entries as misses and allow recompilation.
- [x] Add a small-file/huge-count regression case that proves rejection occurs
  without a large allocation; retain existing truncated-entry coverage. There
  was no truncated-entry test; `core` now damages a stored entry three ways
  (the largest count over one word, one word short, one word long) and then
  stores over it to recover. On this Windows machine the old code did not
  throw: it committed the 16 GB, which took the suite from about 1.3 s to
  8.5 s, and then missed. The check it fails is the lengthened entry, which
  it read as a hit.

## Integration issue requiring reproduction

### I1 — Potential P1: render-purpose and collection changes may leave stale geometry

**Status:** suspected integration defect, not a confirmed end-to-end failure.

Relevant code:

- [render_pass.cpp](../src/hydra/render_pass.cpp), lines 318–320 and 288–304.
- [mesh.cpp](../src/hydra/mesh.cpp), lines 537–541.
- [scene_store.cpp](../src/hydra/scene_store.cpp), lines 341–345.
- [cull_volume.cpp](../src/hydra/cull_volume.cpp), lines 110–120.

The render pass ignores its `renderTags` argument, and the framing comparison does
not include active tags or collection membership. Mesh publication records
visibility, but no render purpose. Snapshots select the store's visible entries.
Cull volumes separately hard-code exclusion of guide and proxy purposes.

Hydra may avoid syncing excluded prims initially; that alone does not establish
what happens to entries already stored after a purpose or collection change.
The interaction with the host's scene-index filtering must be verified before
selecting the implementation fix.

**To do:**

- [ ] Render default, render, proxy, and guide objects in one engine, then toggle
  the included purposes without recreating the delegate.
- [ ] Compare each result and picking AOV against a fresh engine configured with
  that final purpose selection.
- [ ] Change collection roots/exclusions in the same session and test similarly.
- [ ] If the issue reproduces, carry active selection into scene filtering and
  frame invalidation; align cull-volume activation with the same selection.

## Performance and maintenance improvements

### P1 — Retain unchanged GPU textures across scene publications

[PathTracer::SetScene](../src/gpu/path_tracer.cpp), line 1030, calls
`UploadTextures` for every scene publication. That function clears and recreates
the entire GPU texture pool (lines 760–773). A geometry transform or light edit
can therefore upload unchanged texture assets, with a staging allocation and
synchronous submission for each texture. The CPU scene snapshot also copies
`pool->Images()` in [render_pass.cpp](../src/hydra/render_pass.cpp), line 888.

This is a concrete unnecessary-work path, not a measured speedup estimate.

- [x] Give texture slots content/generation identities and reuse unchanged GPU
  images and descriptors. The identity is the image itself: the pool holds
  `shared_ptr<const TextureImage>` and re-decodes into a new one, and the
  tracer keeps the GPU copy of each slot whose pointer it uploaded last time.
  The dome map and its sampling distribution are kept the same way.
- [x] Keep texture-quality changes and failed/reloaded assets correctly invalidating.
  `SetMaxEdge` puts a new image in every slot, failed ones included. In
  `render`, a half-sky dome republished unchanged uploads nothing and still
  reads 0.40; replaced by a whole sky it uploads two images and reads 0.80.
  `usd_texture_quality` passes.
- [x] Measure a transform-only edit in a texture-heavy scene: unchanged textures
  should upload zero bytes after initial publication. Intel Sponza republished
  at frame 2 (`HDCLAUDE_REPUBLISH_AT`): 72 textures and 4,831,838,208 bytes on
  the first publication, 0 bytes and 72 kept on the second, which took 497 ms
  against 3,580 ms. `texturesUploaded`, `textureBytesUploaded` and
  `texturesReused` are now render stats.
- [x] Consider immutable/shared CPU image ownership to avoid copying the whole
  decoded texture pool on each snapshot. Done with the item above: a snapshot
  copies pointers.

### P2 — Separate placement edits from mesh regeneration

[HdClaudeMesh::Sync](../src/hydra/mesh.cpp), starting at line 478, reads topology,
points, primvars, and enters the refinement/triangulation path without a fast path
for a transform-only or visibility-only edit. GPU fingerprint reuse does not
avoid this CPU work.

- [x] Cache the generated prototype and update only placements/visibility when
  those are the only dirty inputs. The store already holds the prototype;
  `HdClaudeMesh::_SyncPlacementOnly` replaces only the placements, visibility,
  binding, categories and prim id through `UpdateMeshPlacement` when nothing
  outside transform, visibility, instancer, instance index, categories,
  material id, prim id and extent is dirty.
- [x] Preserve rebuilds when adaptive tessellation genuinely depends on the
  changed placement or when a geometry-affecting setting changes. A
  subdivision surface refined adaptively or per face takes the full sync; a
  setting change dirties topology and points, which do too.
- [x] Benchmark repeated transform edits on a subdivided mesh and compare the
  resulting geometry and image against the current path. A 200x200-quad
  Catmull-Clark grid at level 2: 73.8 ms a transform edit, refining nothing,
  against 274.6 ms for an edit that re-sets the points and takes the full
  sync a transform edit used to take. The new `usd_incremental_sync` compares
  the geometry, by picking 160 probes against a fresh engine, after a move,
  a hide, a show and a reshape; the old plugin fails its move check by
  refining again.

### P3 — Add tests for state transitions and host buffer formats

Keep the existing numerical and validation suites. Complement them with focused
tests around the boundaries identified here:

- [x] Direct render-buffer format and clear/write tests.
- [x] Instancer attribute removal and reauthoring in one engine (at the
  adapter, R3).
- [x] Motion followed by several stationary frames (R1).
- [ ] Purpose/collection changes and comparison with a fresh engine.
- [ ] A documented clean-machine core-only build job.
- [ ] Ensure USD integration jobs load the exact plugin just built; the current
  tests intentionally use the installed plugin, as documented in
  [tests/CMakeLists.txt](../tests/CMakeLists.txt), lines 130–134.

### P4 — Revalidate the already documented transport deficits

These are existing roadmap items, not newly reproduced findings:

- [Roadmap question 8](roadmap.md): a subsurface furnace deficit, with recorded
  readings around 0.83 solid and 0.90 thin-walled (lines 447–453).
- [Roadmap question 10](roadmap.md): residual rough-dielectric energy loss and
  disagreement between RT and layered encodings (lines 477–598).

- [ ] Rerun these cases against a fresh build and record the commit and settings.
- [ ] Turn remaining print-only cases into explicit regression gates using
  justified bounds, while retaining the physical target separately.
- [ ] Track improvements to the dielectric compensation fit/table independently
  from Hydra integration fixes so image changes have a clear cause.

## Suggested implementation order

1. Add focused regression cases and fix R2, R3, and R4; these are relatively
   contained changes with clear acceptance criteria.
2. Fix R1 with frame-lifecycle tests, then validate temporal reconstruction.
3. Reproduce I1 before committing to a purpose-filtering design.
4. Harden cache parsing in R5.
5. Measure and implement texture reuse, followed by incremental mesh sync.
6. Revalidate the known transport deficits and tighten their gates.

For render-affecting fixes, follow the repository's existing build, validation,
and gallery comparison workflow. Update this document's checkboxes only when the
fix and its stated verification have actually completed.
