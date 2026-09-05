# hdClaude roadmap and phase tracker

Status: live. Last revised 2026-09-05.

This is the durable progress record. Update the tracker, the evidence column,
and the decision log **in the same commit** as the work they describe. A phase
is not complete because its API exists; it is complete when its exit gate passes
and the evidence is recorded here.

## How to resume

1. Read the decision log and the phase tracker below.
2. Inspect the worktree and recent commits. **Never assume this document is
   newer than the code.**
3. Run `compile.bat`, the full test suite, and `validate_usd.bat` before
   changing renderer behaviour.
4. Work one reviewable phase boundary at a time.
5. Re-render and commit the full gallery after every render-affecting change.
6. Record timing, image, validation, and hardware evidence in the phase entry.

## Phase tracker

Allowed states: `Not started`, `In progress`, `Blocked`, `Complete`, `Deferred`.
A blocked phase must name what blocks it.

| # | Phase | State | Evidence |
| --- | --- | --- | --- |
| 0 | Repository, build system, environment, documentation | Complete | core-only preset builds and tests from clean; env script verified; 10 gallery stages open |
| 1 | Core library: hashing, shader cache, spectral tables, display transform | In progress | hashing, shader cache, spectral sampling and sensor; 1222 checks pass |
| 2 | Vulkan context, memory, resource rules, validation gate | In progress | context, allocator, buffers, images, generations, device-lost latch, compute pipelines and dispatch; validation clean on RTX 5060 Ti |
| 3 | Geometry pipeline: meshes, BLAS/TLAS, instancing, subdivision | In progress | prototypes, fingerprinted BLAS reuse, TLAS instancing, ray-query traversal verified by hit pattern on GPU. per-triangle materials from GeomSubsets; deformation through ExtComputation; uniform OpenSubdiv refinement carrying subsets through the parent-face chain. Remaining: adaptive refinement, limit-surface normals |
| 4 | `genglsl_pt` MaterialX target: eval, sample, pdf, combinators | In progress | all 22 overrides; `standard_surface` and `open_pbr_surface` compile; energy and probability-mass validated on GPU for 10 closures and combinators. Remaining: chi-squared agreement |
| 5 | Wavefront integrator: queues, sort, per-material dispatch | In progress | raygen, extend, environment, per-material shade, shadow, film; compaction between bounces; images verified for direct lighting, per-material dispatch and cast shadows. Remaining: the material sort, indirect dispatch |
| 6 | Spectral transport, lights, MIS, film | In progress | the analytic UsdLux set -- rect, disk, sphere, cylinder, distant -- sampled by NEE, with cone and focus shaping and colour temperature; a dome light supplies a constant or textured environment; exposure control; film accumulates progressively. Remaining: hero-wavelength transport, MIS, light-power selection, dome importance sampling, IES profiles, light filters, geometry lights |
| 7 | Hydra delegate: adapters, render pass, AOVs, settings | In progress | plugin discovered as "Claude GPU Path Tracer"; mesh, material, light, camera and render-buffer adapters; progressive render pass; all three `gallery/shader_ball_*.usda` scenes render lit and materially distinct; textures load through Hio and bind bindlessly. Remaining: face-varying UVs, per-texture wrap and filter modes, depth and id AOVs |
| 8 | Gallery parity with hdCodex baselines | Not started | - |
| 9 | Interactive contract: frame identity, per-slot resources, real overlap | Not started | - |
| 10 | Temporal foundation: jitter, motion vectors, guides, history reset | Not started | - |
| 11 | Renderer-native reconstruction fallback (any Vulkan device) | Not started | - |
| 12 | NVIDIA NGX bootstrap and runtime support query | Not started | - |
| 13 | DLSS Super Resolution and DLAA | Blocked | Requires 10, 12 |
| 14 | DLSS Ray Reconstruction | Blocked | Requires 4, 10, 12, 13 |
| 15 | ~~SER in `extend`~~ | Deferred | Not possible: SER builtins do not exist on the compute stage, and the per-material sort already provides the coherence. See [implementation-notes.md](implementation-notes.md) |
| 16 | GPU displacement through the generated MaterialX program | Not started | - |
| 17 | External-memory AOV interop for Hgi hosts | Not started | - |

Phases 13 and 14 are the two hdCodex never reached. They are deliberately late:
each depends on guide buffers that are only meaningful once phases 4-10 are
correct, and feeding a reconstructor from a broken estimator is explicitly a
non-goal.

## Exit gates

Each gate is a claim that can fail. "The code exists" is never a gate.

**Phase 0.** `compile.bat` configures and builds the core-only preset from a
clean clone with no manual dependency steps. `setup_usd_env.bat` reports a clear,
actionable failure for each missing dependency rather than failing later inside a
tool. All committed documents cross-reference consistently.

**Phase 1.** Unit tests pass with no GPU and no OpenUSD present. Spectral round
trip: sRGB primaries and a set of Munsell reflectances upsample and re-integrate
to within 1e-3 dE2000. The shader cache is proven to miss on every component of
its key.

**Phase 2.** A validation-enabled run of the full suite reports zero validation
errors **and the test fails if the count is nonzero** (rule R8). Device-loss
simulation leaves the latch set, every subsequent entry point refuses, and the
destructor completes without issuing a wait.

**Phase 3.** Prototype BLAS reuse, in-place TLAS update, and in-place BLAS update
under stable-topology deformation are each asserted by reading back GPU state,
not a CPU shadow (rule R7). Subdivision preserves creases, corners, holes,
orientation, face-varying seams, and material subsets against a committed
baseline.

**Phase 4.** All 22 pbrlib overrides present (the set is all-or-nothing; see
[materialx-codegen.md](materialx-codegen.md) §3), and the five acceptance items
in §8 of that document: every gallery material compiles with no fallback, white
furnace within 0.5%, chi-squared agreement between sampling and the reported
density, a finite nonzero density at every sampled direction, and combinator
mixture densities that integrate to one — for every closure **and every
combinator**. The last item is the one that catches a combinator reporting the
selected child's density instead of the mixture.

**Phase 5.** Per-material dispatch is observed to scale: adding a large
procedural nodegraph to one object changes that object's shading cost and not
the frame's. Queue counters are GPU-written and never read back by the CPU
during a frame.

**Phase 6.** Furnace test at scene scale. Analytic light comparisons for each
`UsdLux` type. Spectral: a dispersion scene matches an analytic reference; a
metamer pair renders as distinct under two illuminants.

**Phase 7.** A render-pass-level test drives move -> move -> stop -> move against
a stub scene and asserts a frame reaches the AOV on every moving frame (rule R7;
this is the test whose absence hid hdCodex A2/A1/N1/N8). Failure paths assert
`IsConverged()` stays false and the revision is retried.

**Phase 8.** Every gallery scene renders, and the image gate in the gallery
script passes against a committed baseline (rule R9). Timings, GPU, driver, and
TDR values recorded (rule R11).

**Phase 9.** GPU timestamps prove frame N+1's trace overlaps frame N's readback —
measured across a *queue*, not within one command buffer, since a whole-buffer
barrier's first synchronisation scope includes everything already submitted to
the queue. This is the exact claim hdCodex could not make (finding N3); it is
achievable here only because of Rule 1.

**Phase 10.** Jitter sequence, motion vectors, and history reset are validated
against a synthetic scene with known motion. Motion vectors are correct under
instancing and deformation, or the phase does not close.

**Phase 11.** The native fallback improves a fixed-sample interactive image
measurably (SSIM against the converged reference) on a device with no DLSS.

**Phase 12.** Runtime support query returns a correct answer on a supported and
an unsupported device, and the renderer runs unchanged when DLSS is absent.

**Phase 13.** DLAA at native resolution is compared against the converged
reference by SSIM and by a temporal-stability metric. Reference mode output is
bit-identical with DLSS present and absent.

**Phase 14.** Ray Reconstruction fed only by guides produced by MaterialX
closures (never by a surface-model name). Reference mode remains unaffected.

## Decision log

Decisions are recorded when made, with the reason, and are not silently
reversed.

| Date | Decision | Reason |
| --- | --- | --- |
| 2026-09-05 | Wavefront integrator, not a megakernel | A megakernel must contain the union of all material code; with real compiled MaterialX programs that destroys occupancy frame-wide ([architecture.md](architecture.md) §2) |
| 2026-09-05 | `VK_KHR_ray_query` in compute, not `VK_KHR_ray_tracing_pipeline` | In a wavefront design the SBT buys nothing already gained by separating shading from traversal, and costs portability plus a second shader-compilation path |
| 2026-09-05 | MaterialX `genglsl_pt` derived target, not a lowered closure ABI | Full MaterialX fidelity including arbitrary pattern and procedural graphs; the documented extension mechanism (`targetdef inherit`) |
| 2026-09-05 | Four hero wavelengths, not three | `vec4` is the natural GPU register width; better hero MIS; and the interactive preview must not change estimator (hdCodex N9) |
| 2026-09-05 | Separate reference and interactive storage | Sharing one buffer caused four hdCodex findings at once ([lessons](lessons-from-hdcodex.md) C1) |
| 2026-09-05 | Single `BeginFrame(FrameDescription)` entry point | Independent setters caused five hdCodex findings ([lessons](lessons-from-hdcodex.md) C2) |
| 2026-09-05 | CPU readback is the Hydra AOV adapter; interop is a later phase | No public OpenUSD API crosses a device boundary for a `VkImage`. Decided now because phases 11 and 17 both depend on the answer (hdCodex D1, which was never decided) |
| 2026-09-05 | DLSS via NGX directly, not Streamline | A delegate does not own presentation, so Frame Generation and Reflex — Streamline's reason to exist — are out of scope |
| 2026-09-05 | Film and shadow accumulation use no atomics, by invariant | Paths map one-to-one onto pixels, so exactly one invocation writes each entry. Tracing several paths per pixel would need `VK_EXT_shader_atomic_float`; the invariant is stated in the shader so the requirement is not discovered by a race |
| 2026-09-05 | Acceleration-structure reuse is keyed on a geometry fingerprint | Not on prototype index or name: a publication that reorders or renames prototypes must reuse everything, and one that changes a vertex must rebuild only that prototype. The opacity class is part of the fingerprint because it changes the build flags |
| 2026-09-05 | Traversal is asserted by hit pattern, not hit count | A structure built at the wrong scale, or with a transposed instance transform, still produces hits. Coverage fractions and per-pixel instance identity are what distinguish those from correct traversal |
| 2026-09-05 | The density invariant includes discarded probability mass | A visible-normal sampler can produce directions below the horizon, which the closure cannot scatter into. The reported density integrates to one *minus* that mass, and asserting a bare integral of one would have driven a real bug into correct closures ([implementation-notes.md](implementation-notes.md)) |
| 2026-09-05 | Shader and surface nodes thread `closureData` too | Replacing the surface node's calling convention means a shader *nodegraph* must pass it through, or its generated function references an undeclared identifier ([implementation-notes.md](implementation-notes.md)) |
| 2026-09-05 | Subsurface and volume closures publish parameters; the integrator transports | A random walk and volumetric absorption happen along paths inside a medium, not at a surface. Mirrors MaterialX's own OSL target, which emits a closure and leaves transport to the renderer |
| 2026-09-05 | MaterialX 1.39.3 is the version of record | It is what OpenUSD 26.03 ships and what hdClaude links; 1.39.6 differs in headers, closure signatures, throughput semantics, and helpers ([implementation-notes.md](implementation-notes.md)) |
| 2026-09-05 | `ClosureData` stays byte-compatible; extra state in globals | 1.39.3 constructs it inline with a fixed argument list, so adding fields breaks every construction site hdClaude does not replace. A global is per-invocation in a compute shader, so it costs nothing |
| 2026-09-05 | The implementation declarations are generated from the stock ones | Nodedef names do not follow file names, and a wrong one fails silently by falling back to the stock implementation. Deriving them removes the class of error |
| 2026-09-05 | Screen-space derivatives are never used | Neighbouring lanes in a `shade` dispatch are unrelated paths, so `fwidth` measures nothing. Footprints come from ray differentials |
| 2026-09-05 | The validation layer is built from source, not assumed installed | Validation is a gate the GPU tests fail without, so it is a dependency rather than an optional local install. Pinned to the same SDK tag as the headers, loader, and compiler ([implementation-notes.md](implementation-notes.md)) |
| 2026-09-05 | Dependencies by pinned CMake `FetchContent`, never committed | A fresh clone reproduces the tree; the repository stays small and carries no third-party or proprietary code |
| 2026-09-05 | Closure sampling returns a direction only; densities are written by the evaluation closure types | MaterialX evaluates a combinator's children *before* the combinator, so a density returned by sampling belongs to that child's own direction and cannot be mixed. Moving densities to evaluation lets every combinator mix them with the weights it already mixes responses with ([materialx-codegen.md](materialx-codegen.md) 2) |
| 2026-09-05 | The `surface` node gets its own genglsl_pt implementation | `HwSurfaceNode` constructs `ClosureData` inside a rasteriser light loop and an environment lookup. A path tracer supplies its own direction and light sample, so the calling convention has to be replaced alongside the closures ([implementation-notes.md](implementation-notes.md)) |
| 2026-09-05 | SER is not used | Its builtins exist only on ray-tracing-pipeline stages, not compute, and the per-material sort already delivers the coherence SER recovers ([implementation-notes.md](implementation-notes.md)) |
| 2026-09-05 | On device loss, skip the wait but still destroy objects | `vkDestroyDevice` requires its children to be destroyed first, and destruction stays valid after loss; skipping it faults. Only the unchecked wait contaminates diagnostics |
| 2026-09-05 | The pbrlib override set is all-or-nothing | MaterialX resolves `#include` relative to the including file, so mixing one upstream closure with one hdClaude closure emits `struct ClosureData` twice. The set is exactly the 22 pbrlib files that include `mx_closure_type.glsl`; no stdlib file does |

## Open questions

Tracked here rather than decided prematurely.

1. **Sort granularity.** Counting sort per bounce over all active paths, or a
   partial sort that only separates the few most expensive materials? Decide
   with a measurement in phase 5, not before.
2. **Shadow-ray batching depth.** One shadow queue flushed per bounce, or a
   deeper queue flushed per frame? Interacts with memory footprint at 4K.
3. **Displacement residency.** GPU displacement (phase 16) multiplies vertex
   memory by the refinement level. Whether to cache displaced positions or
   re-evaluate per BLAS build is a memory/time trade to measure.
4. **Volume rendering.** MaterialX VDFs are declared and generated; hdClaude
   currently plans homogeneous interior media only. Heterogeneous volumes
   (`UsdVol`) are not scheduled.
