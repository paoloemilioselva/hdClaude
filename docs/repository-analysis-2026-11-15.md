# hdClaude repository analysis

Review date: 15 November 2026  
Revision: `f5c2e3c18875c49423161efe57720f3715487122` — version 1.6.0  
Scope: repository state, implementation, tests, known defects, and roadmap. No application code, tests, settings, or baselines were edited for this review.

## Assessment

hdClaude has a working renderer, a clear architecture, and substantial numerical and integration testing. It is well beyond an initial prototype. Its strongest evidence concerns MaterialX generation, spectral calculations, geometry traversal, and transport in controlled scenes. The project also has a useful practice of testing equivalent USD scenes and including negative controls that demonstrate that a test can detect the intended defect.

The remaining work is concentrated in interactive behavior, completeness across scene types, reconstruction portability, and closing physical correctness gaps. Version 1.6.0 should be understood as a usable renderer with explicit limits. The repository does not yet establish that every supported scene is physically accurate, that all interactive transitions work, or that the claimed real-time architecture overlaps frames.

The immediate priorities I recommend are failure handling, cache invalidation, and trustworthy status documentation. These would make the existing implementation more dependable before adding more rendering features.

## Where the project stands

The [roadmap](roadmap.md) contains 21 numbered phases: seven complete, eleven in progress, two not started, and one deferred. Those counts describe the tracker, not a percentage of the engineering effort completed.

| Area | Implemented today | Material remaining work |
| --- | --- | --- |
| Build and core | CMake presets; Windows and Linux driver; dependency-free core; hashes, shader cache, spectral tables, image metrics | Repeatable clean-machine build jobs and a visible automated platform matrix |
| GPU backend | Vulkan ray queries, allocation, acceleration structures, device-loss checks, validation tests | Broader hardware validation; actual frame overlap |
| Geometry | Meshes, instancing, subdivision, implicit surfaces, curves, displacement | Limit-surface normals; displacement residency and further acceptance evidence |
| Materials | Generated MaterialX target, closure evaluation/sampling/density, combinators, texture pooling and UDIM handling | Additional UV sets and geometry properties; authored sampler behavior; upstream USD/MaterialX type compatibility |
| Transport | Spectral wavefront tracing, analytic emitters, environment sampling, linking, absorption and scattering, fog, splat paths | Rough-interface residuals; hero-wavelength MIS; light selection, IES, filters and geometry lights |
| Hydra integration | Render settings, incremental edits, selection, depth and picking, reconstruction guides | Host-level guide verification; robust failure and invalidation transitions |
| Reconstruction | Optional NVIDIA SR, DLAA and Ray Reconstruction | Native fallback is not started; deformation motion is incomplete; upscaling withholds picking/data AOVs |
| Volumes and splats | Graph-evaluated mesh fog; splat coverage and volume transport | Surface plus volume terminals, field volumes, lit splats, splat NEE and guides |
| Gallery | Versioned baselines and comparison tooling; independent Cornell-box check | Resolve reproducibility findings and assess the remaining scenes against independent expectations |

The separation between `src/core`, `src/gpu`, `src/materialx`, and `src/hydra` is a major strength: the GPU layer receives renderer-owned scene data instead of depending on USD. Optional NGX integration stays behind a renderer-facing interface. The principal maintenance concern is concentration: orchestration and material translation live in several large files, and the main render test is itself large. Splitting them is useful when a specific change requires it; it is less urgent than closing the behavioral gaps below.

## Findings

Priority **P2** means a concrete correctness, integration, or release-readiness issue worth addressing soon. **P3** means documentation or maintenance work. Evidence is identified separately: source tracing establishes a code path, while an integration reproduction establishes its observed behavior in a host.

### F1 — P2: the upscaling failure fallback is rejected by the Hydra render pass

**Evidence: source-verified; no injected backend-failure reproduction performed.**

`PathTracer::BeginFrame` chooses a smaller trace extent for upscaling. If `Reconstruct` returns no image, it keeps the traced image and changes the frame's output dimensions to that smaller extent. This is explicitly described in the code as a usable fallback preview.

The Hydra pass subsequently rejects any frame whose dimensions differ from the bound output extent. Consequently, a failure after upscaling sizing succeeds—for example, backend feature creation failing—does not deliver that preview to the host. The pass returns before its reconstruction-unavailable warning and before writing the image. Repeated failures eventually mark the render complete through the bounded failure mechanism.

**Impact:** the viewport can keep an old image or its cleared allocation instead of receiving the intended fallback. This concerns a late failure in an active upscaling mode; a build without DLSS and native-resolution DLAA are different cases.

**Recommended acceptance check:** make a backend return valid reduced sizing and then fail feature creation. Require a current image at the host's requested extent, an actionable diagnostic, and consistent convergence behavior.

Sources: [path_tracer.cpp](../src/gpu/path_tracer.cpp), `EnsureReconstructionImages`, `Reconstruct`, and `BeginFrame` (approximately lines 2559–2589 and 2869–2886); [render_pass.cpp](../src/hydra/render_pass.cpp), dimension rejection at approximately lines 1293–1311 and later unavailable-backend reporting.

### F2 — P2: overwriting a texture at the same path does not invalidate its cached image

**Evidence: source-verified cache behavior; visual reproduction not performed.**

`HdClaudeTexturePool::AcquireAll` keys entries by asset path and color-space mode. If the key already exists, it returns the old slot without checking file content or retrying a previous decode failure. Material recompilation still acquires through that pool. Reloading existing slots is implemented for an actual texture-quality change, but there is no corresponding file-content invalidation path in the reviewed Hydra code.

**Impact:** saving a revised image over an existing texture can leave a live session showing old pixels. A missing texture that later becomes available can keep its failed slot. Changing to a different filename is covered by the material-edit test, but that does not exercise an overwrite at the same filename.

**Recommended acceptance check:** render a texture, overwrite it, explicitly request a refresh, and compare the live session with a fresh delegate. Include missing-file → file-created recovery. An explicit reload operation would be sufficient; continuous filesystem watching is not required.

Sources: [texture_loader.cpp](../src/hydra/texture_loader.cpp), approximately lines 522–551 and 621–652; [material.cpp](../src/hydra/material.cpp), `AcquireTextures`; [material_edits.py](../tests/usd/material_edits.py), approximately lines 299–307.

### F3 — P2: changing stage units or up axis in a live session leaves retained light metadata stale

**Evidence: source-verified and already acknowledged in implementation notes; not independently reproduced here.**

The new stage-metrics adapter reads `metersPerUnit` and `upAxis` when each light is imaged, storing retained values. It has no path to refresh those values when layer metadata changes. The code and [implementation notes](implementation-notes.md) explicitly record that limitation.

**Impact:** physical power and illuminance normalization can keep using the previous stage scale or axis until lights are reimaged. For a fixed authored area, changing the length unit by a factor of 100 changes its physical area by 10,000, so this can be a substantial brightness error.

**Recommended acceptance check:** render a physically authored light, change stage metadata in the same engine, and compare with a fresh engine. Provide an explicit reimage/reset operation if automatic metadata invalidation is unavailable.

Sources: [stageMetricsAdapter.cpp](../src/usdImaging/stageMetricsAdapter.cpp), approximately lines 47–71; [light.cpp](../src/hydra/light.cpp), approximately lines 666–672; [scene_store.cpp](../src/hydra/scene_store.cpp), physical brightness resolution.

### F4 — P2: fog transport silently exceeds its correctness envelope

**Evidence: source-verified, documented algorithmic limits; no new stress-scene render performed.**

Three limits deserve explicit treatment as supported-scene constraints:

- Boundary collection stops after eight crossings across the ray. The initial inside/outside state is computed from the parity of the crossings collected. With additional overlapping or concave boundaries, truncation can therefore affect both containment and subsequent attenuation.
- The extinction majorant is estimated on a fixed 32³ sampling pattern with a 1.5 margin. An arbitrary graph can contain narrow features that those samples miss. The walk clamps invalid negative null-collision rates; it does not establish a valid bound or report the violation.
- Both fog walking and shadow transmittance stop examining tentative collisions at 1,024 steps and continue with the accumulated result, leaving the remaining interval untreated.

These limits do not invalidate the existing smooth, controlled fog tests. They do mean that an apparently plausible result is insufficient evidence for arbitrary procedural density or complicated overlapping volumes.

**Recommended acceptance checks:** more than eight boundary crossings, a narrow density feature between majorant samples, and a highly varying field that exhausts the collision budget. Expose overflow or bound violations through a diagnostic/counter and define a deliberate fallback.

Sources: [fog.glsl](../shaders/fog.glsl), constants near lines 42–57, boundary collection near 125–169, walk near 285–390, transmittance near 395–466, and majorant construction near 468–519; [fog-volumes.md](fog-volumes.md).

### F5 — P2: passing transport tests still permits known physical residuals

**Evidence: reproduced by this review's passing render suite, alongside source inspection; no additional parameter sweep performed.**

The rough-dielectric sphere tests intentionally allow deviations from unity of 3%, 8%, and 20% at alpha 0.1, 0.3, and 0.6. RT-versus-layered agreement is not asserted for alpha 0.6. These checks protect the fitted compensation against regression; they do not prove energy conservation to those materials' physical target.

The fresh run confirms that this is a current image-correctness issue. At alpha 0.6, the off-center layered sphere's green channel reads **0.8290 against 1.0**, while the RT encoding reads **0.9141**. The composed OpenPBR subsurface-sphere probe (`subsurface_weight = 0.2`, radius 0.1) still prints **0.8710 at 32 bounces**, without an assertion against unity. Its thin-walled counterpart reaches 0.9998 at 32 bounces, so the old claim that both readings stay flat across bounce counts is out of date. The separate lossless bare-subsurface sphere passes at approximately (1.0018, 0.9984, 0.9983); that test does not close the composed-material deficit.

The interior scattering walk also has a 256-collision cap. On the final step it proceeds to the boundary instead of continuing the random walk, which can bias the exit location. The roadmap records this limitation, and the subsurface tests deliberately choose cases that avoid confusing the cap with the phenomenon under test.

**Recommended action:** keep the regression bounds, report the physical error separately, and prioritize a better compensation model and a deliberate treatment of dense interior walks. A green suite should not close either item by itself.

Sources: [render_tests.cpp](../tests/render_tests.cpp), approximately lines 2735–2799, 2885–2900, and 3410 onward; [extend.comp.glsl](../shaders/extend.comp.glsl), lines 38 and 534–600; [roadmap](roadmap.md), phase 6 and open questions 8 and 10; this run's CTest log.

### F6 — P2: upscaling and temporal features have important host limitations

**Evidence: source-verified, mostly already documented; usability effects were not measured in a viewer here.**

The render pass withholds depth, prim/instance IDs, normals, roughness and albedo guides when trace and output extents differ. This avoids copying data at the wrong stride, but means upscaling does not provide the picking/data behavior a host normally expects. DLAA avoids that size mismatch. Face element IDs remain the explicit no-face value.

Motion guides reconstruct previous positions from previous object transforms and current geometry. The shader explicitly supports rigid motion only; it does not account for point deformation. The roadmap's phase-10 gate requires deformation correctness, so that phase needs a more explicit remaining-work entry than host guide measurement alone.

**Recommended action:** decide and document the host contract for picking under upscaling, and make deformation motion or deliberate history rejection a separately testable deliverable.

Sources: [render_pass.cpp](../src/hydra/render_pass.cpp), `WriteFrame`, approximately lines 206–304; [guides.comp.glsl](../shaders/guides.comp.glsl), approximately lines 82–98; [roadmap](roadmap.md), phase-10 exit gate.

### F7 — P3: status documentation contains conflicting current claims

**Evidence: directly compared repository text and current implementation.**

| Claim | Current evidence | Needed correction |
| --- | --- | --- |
| README says a native spatiotemporal filter works on any Vulkan 1.3 ray-query device | Roadmap phase 11 is not started; landing page also says planned | Describe it as planned |
| Phase 7 lists depth and ID AOVs as remaining | Implemented in `WriteFrame`; `usd_depth` and `usd_picking` exist | Replace with the actual limitations, including upscaling and face IDs |
| Phase 6 contains an earlier statement that interior scattering is achromatic | Current walk carries per-lane scattering densities and tests chromatic media | Mark the earlier statement historical |
| Subsurface question 8 describes readings flat across bounce counts | The original composed-material case is still unasserted, but the fresh run changes with bounce count, and thin-walled reaches 0.9998 at 32 | Update its measurements and distinguish it from the newer asserted bare-subsurface tests |
| Render-test commentary says the scattering-sphere gate is not asserted | The loop immediately below asserts unity for vacuum, isotropic, forward, dense and chromatic cases | Update or remove the obsolete commentary |
| Phase 7 says failed frames keep `IsConverged()` false | Current code eventually marks repeated failures complete to avoid an infinite host loop | Document transient retry and terminal failure separately |

The old OpenChessSet nondeterminism/baseline failure also remains in phase 8. I did not rerun the full gallery, so this review neither closes that issue nor claims to have freshly reproduced it.

Sources: [README](../README.md), approximately lines 51–54; [roadmap](roadmap.md), phases 6, 7, 8 and open question 8; [render_tests.cpp](../tests/render_tests.cpp), approximately lines 2807–2900; [render_pass.cpp](../src/hydra/render_pass.cpp), approximately lines 450–469.

## Test coverage and release confidence

The test structure is unusually useful for a renderer: core numerical checks, GLSL compilation, GPU/geometry tests, statistical closure validation, image transport tests, adapter tests, and USD integration tests. GPU tests request validation and make validation failures meaningful. USD integration tests are configured to load the build-tree plugin instead of silently testing an older installed renderer.

There are 27 tests registered in the reviewed `dev` configuration. The Houdini preset disables testing, so passing standalone USD tests does not establish Houdini behavior. The reviewed configuration also has DLSS disabled, so it cannot establish current NVIDIA reconstruction image quality or reproduce F1 through NGX.

No tracked GitHub Actions, GitLab CI, or Azure Pipelines configuration was found. External automation may exist, but it is not visible in this repository. A production Linux build is documented; this review did not run Linux and does not dispute that usage. The missing repository evidence is a repeatable automated matrix, particularly for core-only, supported USD versions, platform builds, and optional DLSS.

### Validation performed in this review

- Confirmed a clean tracked worktree before the review.
- Successfully regenerated/built the current `dev` configuration with MSVC, OpenUSD 26.03, MaterialX 1.39.3, Vulkan enabled, and DLSS disabled. No install target was run. This reused the existing build and dependency trees; it was not a clean-clone build.
- Successfully rebuilt `core-only` and passed its CTest suite with its OpenUSD root deliberately pointing at a nonexistent directory. This confirms that configuration's independence from USD on this machine, not a clean-machine certification.
- The first full-suite attempts were interrupted and are not counted as complete validation.
- The final full `dev` CTest run passed **27/27 tests, zero failures**, in **995.44 seconds** on an NVIDIA GeForce RTX 5060 Ti. The separate core-only test passed in 1.69 seconds. These are local observations, not performance baselines.

| Validation group | Result |
| --- | --- |
| Core, GLSL compiler, geometry, GPU, MaterialX, closure validation, render, Hydra | All eight suites passed |
| USD instancing, light linking, culling, ray visibility, fog and implicit surfaces | All six passed |
| USD texture quality, picking, incremental sync, material edits, purposes and unusable materials | All six passed |
| USD ribbon curves, depth, generated Houdini parameters and landing page | All four passed |
| USD Cornell box, physical illuminant and physical brightness | All three passed |

The fresh Cornell-box run's largest reported window error was **0.57%**, against a 1.5% acceptance limit. Its RGB-only negative control missed by up to **5.39%**, supporting the claim that the comparison detects use of the measured spectra. The same passing run reproduced the rough-transmission and composed-subsurface residuals in F5.

The core suite reported 507,006 checks, closure validation 793, the render suite 16,990, and Hydra 290. Check counts include repeated numerical cases and do not imply exhaustive feature coverage. Optional DLSS image evaluations were skipped inside the suites because this build has no DLSS SDK.

Reproduction commands, from the configured Windows developer/OpenUSD environment:

```text
cmake --build --preset dev
ctest --test-dir build/dev --output-on-failure --timeout 300 --output-log build/review-2026-11-15-ctest.log
cmake --build --preset core-only
ctest --preset core-only
```

The raw local test transcript is [build/review-2026-11-15-ctest.log](../build/review-2026-11-15-ctest.log), under the gitignored build tree. Only this report was added to the tracked-source area; no application source, existing documentation, tests, or gallery baselines were changed.

## Recommended next milestones

1. **Make failure and refresh behavior dependable.** Address F1 and define explicit texture and stage-metadata refresh behavior. Test live-session results against a fresh delegate.
2. **Reconcile the status documents with the tests.** Correct the native-filter claim and obsolete open questions. Separate shipped behavior, known residuals, and future plans in each phase.
3. **Establish repeatable release evidence.** Add visible build/test automation, retain validation logs and exact feature configurations, and reproduce the outstanding gallery determinism issue before accepting a new baseline.
4. **Close the existing interactive contract.** Finish host guide checks, deformation motion/history behavior, and the picking contract under upscaling. Measure frame overlap only after GPU timestamps demonstrate it; `BeginFrame` currently performs the trace synchronously and `EndFrame` retrieves the completed result.
5. **Choose the next feature milestone deliberately.** For broad interactive usability, native reconstruction is the largest missing portable feature. For image correctness, prioritize rough transmission and volume limits. Lit splats, field volumes and further lighting features can follow those acceptance gates.

The October review's main findings—half-float AOV writes, removed instancer primvars, stale rigid-object motion, corrupt-cache allocation, purpose filtering, redundant texture uploads, and placement-only mesh regeneration—have implementation changes and regression coverage in the current tree. They should not be carried forward as unresolved defects merely because the historical report still contains their original descriptions.

This is a targeted repository review. Source-verified findings identify paths worth fixing or reproducing; they are not claims that every user encounters those paths. Gallery image quality, Linux/Houdini execution, multi-vendor GPU behavior and DLSS output were not independently revalidated here.
