# hdClaude independent analysis

Date: 2026-11-15
Revision read: `f5c2e3c` (Version 1.6.0)

## How this was produced

Written from the source alone. I did not read
`docs/repository-analysis-2026-11-15.md` before forming these findings, so the
two can be compared. I did read `docs/code-review-2026-10-26.md`, to avoid
re-reporting what it found and fixed.

**Nothing was built or run.** Every finding below comes from reading the code
at the cited lines. Where a finding rests on arithmetic I checked the
arithmetic; where it rests on a third-party behaviour (Imath's half
conversion) I say so. None is a runtime reproduction, and each should be
reproduced before its fix is accepted.

Read in full: the Hydra render pass, render delegate, scene store, instancer,
render buffer, texture loader, DDS decoder and camera; the Vulkan context,
resources, compute pipeline and GLSL compiler; the shader cache; the frame
path of `path_tracer.cpp` (texture upload, invalidation, `BeginFrame`, `Trace`);
the film kernel; the CMake, dependency, test and build-driver files.

Not read, so nothing here speaks for them: `SetScene` and the acceleration
structures, displacement, the MaterialX generator and material compiler, the
closure GLSL under `mtlx/`, the shade/extend/shadow/fog kernels, lights,
meshes, subdivision, curves, splats, cull volumes, NGX, and the Python
scripts. That is roughly two thirds of the code by line count, and it includes
the transport itself.

## Assessment

The layering is real: `src/gpu` has no `pxr` include and `src/hydra` no Vulkan
one, failures are reported rather than absorbed, and the previous review's
findings were each fixed with a test that fails on the old source. The
weakest areas are the ones furthest from the transport tests: what happens to
values outside the expected range (a texel above half-float's maximum, a
header that lies about its size), what happens after a step fails part-way,
and how much host-side work each frame and each publication repeats.

## Bugs

Ordered by how likely a user is to meet them.

### B1. A float texture above 65504 becomes infinity, and the film keeps the NaN

- [texture_loader.cpp:115-119](../src/hydra/texture_loader.cpp) (`WriteHalf`)
  and [dds_image.cpp:724-727](../src/hydra/dds_image.cpp) convert every float
  texel to a half with no clamp. Imath's conversion returns infinity for
  anything past the largest half, 65504 (by its documented behaviour; I did
  not execute it).
- [path_state.glsl:1712](../shaders/path_state.glsl) returns the dome texel
  times the environment colour, unguarded.
- [environment_distribution.cpp:92](../src/gpu/environment_distribution.cpp)
  and line 157 treat a non-finite texel as zero, so the sampling table gives
  those texels no probability.
- [film.comp.glsl:48-55](../shaders/film.comp.glsl) converts to RGB with a
  matrix that has negative terms and then does `accumulation += rgb`, with no
  finite check.

An outdoor HDRI with an unclipped sun routinely exceeds 65504. Those texels
load as infinity. They are never chosen by light sampling, so the sun
contributes nothing to next-event estimation; a path that reaches them by
BSDF sampling or directly from the camera carries infinity, the film's matrix
turns that into NaN, and the accumulation holds the NaN for the rest of the
render. The expected picture is a sun that lights nothing and pixels that
never converge. `Halve` keeps the infinity through texture-quality reduction.

Fix: clamp at the conversion and report once per image how many texels were
clamped, or store float sources in a 32-bit format; and independently, have
the film drop a non-finite sample and count it in the render stats, so that
any future source of NaN is a statistic rather than a dead pixel.

### B2. A texture upload that fails empties the pool but still remembers it

[path_tracer.cpp:883-907](../src/gpu/path_tracer.cpp). Reused images are
*moved* out of `_texturePool` into a local as the loop runs. If
`UploadTexture` then throws (device memory is the realistic cause), the local
is destroyed with those images in it, while `_uploadedTextures` still records
them as uploaded. The render pass retries the same revision
([render_pass.cpp:1123-1133](../src/hydra/render_pass.cpp)); on the retry each
of those slots matches its remembered pointer, takes the moved-from image, and
binds the placeholder. The scene then renders with textures missing and no
report. This is architecture rule 6.1 (build, then publish) not holding for
the texture pool.

Fix: build the new pool without touching the old one (share or defer the
moves until every upload has succeeded), or clear `_uploadedTextures` first.

### B3. A DDS header can overflow the size check and write out of bounds

[dds_image.cpp:405-418](../src/hydra/dds_image.cpp) computes the top level's
byte count in 64 bits from two 32-bit dimensions with no bound on either. At
32 bits a pixel, width = height = 0x80000000 gives exactly 2^64, which wraps
to 0 (checked). The test at line 676 passes, `DecodeMasked` resizes its output
to a size that also wraps to 0 (line 442), and the loop then reads and writes
far past both buffers. For block formats, 0xFFFFFFFF squared wraps the same
way and ends in an allocation of nearly 2^64 bytes thrown inside
`WorkParallelForN`. The comment at line 673 says a header cannot cause this.

It needs a damaged or hostile file, so it is unlikely, but it is memory
corruption inside the host. Fix: reject dimensions above a stated maximum
(65536 is beyond any device limit) before any arithmetic.

### B4. The kernel-profile query pool is sized once

[path_tracer.cpp:3175-3184](../src/gpu/path_tracer.cpp) creates the timestamp
pool for the first frame's `maxBounces * 12 + 2` queries and never again;
lines 3207 and 3386 then reset and read `stampCount` queries computed from the
*current* bounce count. Raising Max bounces in a session with
`HDCLAUDE_PROFILE_KERNELS` or `HDCLAUDE_FRAME_LOG` set addresses queries the
pool does not have, which is invalid usage. Fix: remember the pool's size and
recreate it when more are needed.

### B5. Settings paired with environment variables are applied only at construction

[render_delegate.cpp:450-463](../src/hydra/render_delegate.cpp) copies each
setting into the override the lower layers read, once, in `Initialize`. There
is no `SetRenderSetting` override anywhere in the tree, so a later change to
Trace, Profile kernels, Frame log, Stats report, Dump shaders or Republish at
changes nothing those layers see. The comments at lines 91-97 and 1036-1040
say these exist so they can be flipped from a viewport. Device and validation
do need a new delegate; the others need not.

Related, in the same mechanism: every setting change increments the settings
version, which is part of `Framing`
([render_pass.cpp:354, 867](../src/hydra/render_pass.cpp)), so it restarts
accumulation. That includes Exposure, which is applied to the resolved image
after the film (lines 1411-1421) and cannot change a sample, and raising
Samples per pixel, which could continue from the samples already taken.

### B6. Lesser defects

- **Device probe disagrees with device selection.**
  [vulkan_context.cpp:316-341](../src/gpu/vulkan_context.cpp) takes the first
  capable device, or the last whose name matches; the constructor (lines
  599-694) also requires Vulkan 1.3 and prefers discrete, then memory. The
  comment says they agree. On a machine with an integrated and a discrete GPU
  the probe can name one and the renderer use the other.
- **A fence leaks on the device-lost path.** `SubmitImmediate` (lines
  1008-1016) skips destroying the fence when the device is lost, which is
  where a lost device is most likely to be discovered. The destructor's own
  comment (lines 938-949) says destruction must not be skipped, and that
  skipping it crashed `vkDestroyDevice` once already.
- **Only the colour AOV gets its clear value.** When the backend never
  started ([render_pass.cpp:535-547](../src/hydra/render_pass.cpp)) colour is
  cleared and every other bound AOV keeps its zero allocation; the same holds
  for depth and the picking AOVs while reconstruction upscales (lines
  282-293). The file's own warning text says what a zero depth means to a
  host: a surface at the near plane under every pixel.
- **`HDCLAUDE_REPUBLISH_AT` silently needs `HDCLAUDE_FRAME_LOG`.** It compares
  against `_frameLogIndex` (lines 981-986), which only advances inside the
  frame-log block (line 1527).
- **Instancer primvars of another type are dropped without a word.**
  [instancer.cpp:148-178](../src/hydra/instancer.cpp) accepts translations and
  scales as `VtVec3fArray` only. A `Vec3d` or `Vec3h` array places every
  instance at the instancer's origin, unscaled, and nothing is reported.

### Structural risk, not confirmed

Sample count, uploaded revision and uploaded selection live on each
`HdClaudeRenderPass`; the scene, film and history live on the one `PathTracer`
the delegate owns. Two passes on one delegate with different collections or
cameras would each believe the tracer holds *their* scene and *their*
accumulated samples: pass A does not re-upload after pass B replaced the
scene, and each would count samples the other's camera change had reset. I
did not establish that any supported host creates two passes on one delegate,
so this is a hazard to test rather than a defect.

## Improvements

### Performance

1. **Read back only what is asked for.**
   [path_tracer.cpp:3425-3506](../src/gpu/path_tracer.cpp) copies the film and
   all five guide buffers to the host on every call, 116 bytes a pixel, then
   splits them into separate vectors on the CPU, whether or not any guide AOV
   is bound or reconstruction is on. At 3840x2160 that is about 960 MB a
   frame. The division by sample count (lines 3559-3569) is a CPU pass that
   could be the film's.
2. **Compile materials in parallel.**
   [material.cpp:197-202](../src/hydra/material.cpp) generates and compiles
   each bound material one after another on the render thread, inside the
   first `_Execute`. Texture decoding beside it is already parallel. This
   needs the generator's thread-safety checked first, which I did not do.
3. **Stop deep-copying geometry on every snapshot.**
   [scene_store.cpp:502](../src/hydra/scene_store.cpp) copies each mesh's
   whole `MeshPrototype` (vectors of positions, indices, normals, UVs) under
   the store mutex for every revision, a light edit included, and lines
   873-880 copy every instance transform again for motion history whether or
   not anything consumes motion. Textures were moved to
   `shared_ptr<const ...>` for exactly this reason in the last review;
   prototypes are the same case.
4. **Reload textures in parallel on a quality change.** `SetMaxEdge`
   ([texture_loader.cpp:635-650](../src/hydra/texture_loader.cpp)) re-decodes
   every image serially while holding the pool mutex.
5. **One submit and one wait per sample.** `Trace` submits and blocks once a
   sample (lines 3189-3195), and `_Execute` runs the whole frame on the host's
   thread, so a viewport is unresponsive for `samplesPerFrame` samples at a
   time.
6. **Hoist the primvar lookups out of the per-instance loop**
   ([instancer.cpp:144-190](../src/hydra/instancer.cpp)): four hash lookups
   and type tests per instance, on stages the comments measure in millions.

### Robustness

1. **Query device features before requesting them.**
   [vulkan_context.cpp:778-811](../src/gpu/vulkan_context.cpp) enables
   `shaderInt64`, `scalarBlockLayout`, `computeFullSubgroups`, the
   descriptor-indexing set and others unconditionally; selection checked only
   extension names and the API version. A device lacking one fails at
   `vkCreateDevice` with a generic error, after selection could have chosen
   another GPU or named the missing feature.
2. **Give the caches a home.** With `HDCLAUDE_SHADER_CACHE` unset both caches
   go to `<current directory>/shader-cache`
   ([shader_cache.cpp:189-201](../src/core/shader_cache.cpp)), so every launch
   directory grows its own and a read-only one caches nothing. A per-user
   cache directory is the usual default. Neither cache has a size limit or
   eviction. The Houdini package points the cache inside the package root
   ([hdClaude.json.in](../resources/houdini/hdClaude.json.in)), which stops
   working silently if that root is installed read-only.
3. **A texture too large for the device should cost that texture, not the
   scene.** `UploadTexture` never compares against `maxImageDimension2D`; the
   failure throws out of `SetScene` and the publication is abandoned after
   three tries.
4. **Keep 16-bit sources at more than 8 bits.**
   [texture_loader.cpp:33-35](../src/hydra/texture_loader.cpp) quantises every
   integer source to RGBA8, as its comment admits. A 16-bit height map
   driving displacement is reduced to 256 levels.
5. **Publish stage metrics somewhere other than on lights.** The stand-in sun
   is aimed from the Up axis setting alone
   ([render_pass.cpp:1191-1200](../src/hydra/render_pass.cpp)). It is used
   only when a stage has no lights, which is exactly when the adapter that
   publishes the stage's up axis has no prim to publish it on, so a Z-up stage
   with no lights gets a sideways sun unless told.
6. **Environment overrides are process-wide**
   ([environment.h:43-70](../include/hdclaude/core/environment.h)). Two
   delegates in one process share whichever was constructed last.

### Build, tests and repository

1. **There is no CI.** No workflow directory exists. `core` and
   `glsl_compiler` need neither a GPU nor OpenUSD, and a Linux GCC job running
   them would protect the build that is used in production. The last review's
   open item for a clean-machine core-only job is still open.
2. **Pin dependencies by commit and plan for no network.**
   [Dependencies.cmake](../cmake/Dependencies.cmake) pins four of five by tag,
   which can be moved, and a clean clone cannot configure without reaching
   GitHub.
3. **The clone is 274 MiB, of which gallery baselines are 291 MiB of blobs**
   (506 versions; measured with `git rev-list --objects --all`). Every
   re-rendered baseline adds to it permanently. A shallow clone in the
   packaging recipe avoids the cost today; LFS or a separate baselines
   repository stops it growing.
4. **Machine-specific paths.** [CMakeLists.txt:37](../CMakeLists.txt) falls
   back to `C:/dev/usd-26.03`; [compile.py:41](../compile.py) knows only
   Visual Studio 2022 Community, where `vswhere` would find any edition.
5. **Test suites cannot be run in part.** `render_tests.cpp` is 6,704 lines
   behind one ctest name, and `test_support.h` takes no filter argument.
6. **Warnings-as-errors is off by default** and nothing turns it on.

## Documentation that no longer matches the code

- [architecture.md](architecture.md) section 8 states that readback is
  double-buffered and overlaps the next frame's execution. It does not: every
  submit waits, and
  [render_pass.cpp:1299-1304](../src/hydra/render_pass.cpp) says "nothing
  overlaps yet". The roadmap lists this as in progress; the architecture
  document states it as done.
- `architecture.md` and `roadmap.md` both say "Last revised 2026-09-22" and
  have been revised since.
- [render_delegate.cpp:579-583](../src/hydra/render_delegate.cpp) says the
  refinement level is read once and changing it means a new delegate; the
  render pass has applied it live since. Lines 1016-1035 hold the comments for
  Light geometry and Splat transport, detached from their entries further
  down.
- The README says "Status: under construction" for a 1.6.0 that is in
  production use on Linux.
- `roadmap.md` is about 330 KB with single table cells thousands of
  characters long, which makes its diffs unreadable. Moving evidence out of
  the tracker table into per-phase sections would keep the table a table.

## Capability gaps seen while reading

Stated only where I checked for them. The camera passes matrices and exposure
and nothing else: there is no depth of field and no motion blur (no use of
f-stop, focus distance or shutter in `src/gpu` or `shaders`). `elementId` is
written as a constant -1 ([render_pass.cpp:237-243](../src/hydra/render_pass.cpp)).
Textures have one mip level and a linear sampler. SPIR-V is emitted
unoptimised ([glsl_compiler.cpp:131](../src/gpu/glsl_compiler.cpp)); whether
that costs pipeline build time is worth one measurement. Phases 11 (native
reconstruction) and 17 (external-memory AOVs) are not started, as the roadmap
says.

## Against the other report of the same date

Read afterwards. The two overlap on two points only: no CI in the repository,
and frame overlap described but not implemented. Everything under Bugs and
most of Improvements here is absent there; its F1 to F6 (the rejected
upscaling fallback, same-path texture overwrites, stale stage metrics, fog
limits, transport residuals, upscaling's host limits) are absent here, mostly
because they sit in code this pass did not read. Its F1 agrees with what I
read in `BeginFrame` and the render pass's extent check. That report ran the
test suite; this one ran nothing. They are complements, not duplicates.

## Suggested order

1. B1: clamp or widen float textures, and add the film's non-finite guard with
   a counter. Reproduce first with a one-texel dome above 65504.
2. B2 and B3: both are small, contained and testable without a GPU (B3) or
   with an injected upload failure (B2).
3. B4 and B5: the query pool, then a `SetRenderSetting` override that
   forwards live settings and a framing key that excludes Exposure.
4. Performance 1 and 3, measured on Sponza or ALab with
   `HDCLAUDE_FRAME_LOG`, since both scale with the scenes that are already
   slow.
5. A CI job for the two GPU-free suites on Windows and Linux.
6. The architecture document's section 8, in the same commit as whichever
   change next touches the frame loop.
