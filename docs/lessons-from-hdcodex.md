# Lessons inherited from hdCodex

Status: binding. Last revised 2026-09-05.

hdClaude is a second implementation. Its predecessor, `hdCodex`, is a working
spectral Vulkan path-tracing Hydra delegate whose Phase 2 was reopened after
three independent reviews found thirteen defects. Those defects were not random:
most trace to four structural decisions. This document records them as **rules
hdClaude adopts from day one**, with the finding each one comes from, so the
cost of relearning them is not paid twice.

Source: `../hdCodex/docs/phase2-verification-review.md` (2026-09-05), which
verified `phase2-bug-analysis.md` and `phase2-independent-review.md`.

## The four structural causes

### C1. One buffer serving two contracts (findings A1, N3, N5, D2)

hdCodex path-traced interactive frames into the same RGBA32F buffer the
reference accumulator used. Four separate defects followed from that single
decision:

- resizing between half-res interactive and full-res reference destroyed and
  recreated the buffer on **every** interaction transition (N5), causing two
  device stalls and ~40 MB of allocation churn per camera drag;
- the recreated buffer left a resolve descriptor naming freed device memory
  (A1) — a shader read of freed memory;
- because every command buffer opened with a whole-buffer barrier on that
  shared buffer, the two-slot submission ring **could not overlap GPU work at
  all** (N3). Two slots' worth of command buffers, fences, uniforms, descriptor
  sets, and timestamp ranges were added; the resource that actually serialised
  them was not duplicated. Phase 2's headline deliverable was structurally
  unachievable.

> **Rule 1.** Reference accumulation and interactive frames have separate,
> independently sized storage. Interaction never resizes reference storage.
> Anything a submission slot writes is owned per slot.

Recorded in [architecture.md](architecture.md) §5.

### C2. Lifecycle as independent setters (findings A1, A2, N1, N8, D3)

`SetScene()`, `SetShadingMode()`, and an implicit resize inside `Trace()` were
three entry points, each guessing what invalidation the others implied.
`SetShadingMode()` guessed "always drain" — which, in the real Hydra call
sequence, meant a continuously moving camera delivered **zero** frames to the
AOV while still paying a full blocking fence wait and a full readback memcpy
every frame (A2). `Trace()` computed the correct invalidation predicate and
threw it away (A1). `RenderInteractive()` returned a previous frame's pixels
that the caller then rescaled using the *current* frame's extents (N8).

> **Rule 2.** One frame-scoped entry point takes everything that could
> invalidate anything, and makes the invalidation decision once. Every result
> carries the identity and extents of the frame that produced it.

Recorded in [architecture.md](architecture.md) §4.

### C3. Failure reported through the success channel (findings A3, N1, N4, D4)

On device loss, hdCodex cleared the colour buffer, set the sample index to the
target, and reported convergence. `usdrecord` exited 0 and wrote a black image.
A scene-build exception reached the same result with no GPU failure at all. And
because `_lastRevision = revision` executed unconditionally, a scene build that
failed for a transient reason was **never retried** — the delegate rendered the
previous scene forever while reporting success. Four `vkDeviceWaitIdle` calls
were unchecked and nothing latched device loss, so every diagnostic gathered
after a fault was contaminated by API calls issued against a dead device.

> **Rule 3.** `IsConverged()` stays false on failure. Errors raise
> `TF_RUNTIME_ERROR`, not `TF_WARN`. A failed revision is retried, not
> committed. Every Vulkan result is checked, `vkDeviceWaitIdle` included, and a
> sticky `deviceLost` latch gates every entry point and the destructor.

Recorded in [architecture.md](architecture.md) §6, rules 3 and 4.

### C4. Non-transactional resource replacement (findings A4, N2)

`EnsureOutput()` destroyed four buffers before allocating replacements, and
updated descriptors only at the end; any throw in between left every descriptor
set naming destroyed buffers, with no state flag recording it. `SetShadingMode()`
destroyed the compute pipeline before creating its replacement, so a MaterialX
compilation failure left a null pipeline that the render pass would then bind
and dispatch.

> **Rule 4.** Build replacements into locals, publish on success, update
> descriptors last. Never destroy before the replacement exists.

Recorded in [architecture.md](architecture.md) §6, rule 1.

## Additional rules adopted

### R5. Invalidate by identity, not by shape (A1, N5)

hdCodex invalidated a descriptor set by comparing render extents, but the
descriptor named a *buffer*. Extents matching did not imply the buffer had
survived. Resources carry generation counters; descriptor sets record the
generation they were written against.

### R6. The preview is a cheaper estimator, not a different one (N9)

hdCodex dropped from three spectral lanes to one while the camera moved. That
does not add zero-mean noise — it adds **chromatic** noise that is not zero-mean
over a short temporal window, in exactly the image a user navigates with, for a
renderer whose stated differentiator is spectral transport. It is also the input
DLSS Ray Reconstruction would be asked to lock in.

hdClaude holds four lanes in every mode and reduces samples and path length
instead. Recorded in [architecture.md](architecture.md) §5.

### R7. Test the call sequence the host actually produces (A2, A5, D5)

Phase 2 was accepted against tests calling the renderer directly in a sequence
`HdCodexRenderPass` never produces. A single render-pass-level test driving
move -> move -> stop -> move against a stub scene would have caught four
findings at once. Layout assertions tested a CPU shadow variable, which is
tautological.

hdClaude's test suite includes render-pass-level tests from the first phase that
has a render pass, and GPU state is asserted by reading back GPU state.

### R8. Validation must be able to fail the build (N13)

A debug callback that prints and returns `VK_FALSE` with no counter makes "VVL
clean" an assertion about someone having read the console. hdClaude's context
keeps an atomic error counter, exposes it, and validation-enabled test runs fail
on a nonzero count.

### R9. The image gate is part of the gallery script (A3, D4)

hdCodex contained an image-diff tool, wired into one subdivision check but not
into the gallery script, which gated on exit code alone — the exact reason a
black image was accepted. hdClaude's gallery script compares every render
against its committed baseline with RMS, per-pixel, and failed-pixel thresholds
before replacing it.

### R10. Derive pool sizes from the binding table (N12)

hdCodex's descriptor pool restated its sizes as literals that exactly matched
the layout with zero headroom; adding one binding would fail allocation at
startup with no compile-time signal. hdClaude derives pool sizes from the
binding arrays.

### R11. Record the machine, not just the timing (Part C)

The hdCodex OpenPBR device loss was investigated for days against a "Windows TDR
timeout" hypothesis. The gallery's own known-good row already refuted it: the
same workload at the same settings had completed in 381 s three days earlier,
and 12 s per dispatch would have tripped a 2 s watchdog on update 1.

**Measured on this workstation, 2026-09-05:** `TdrDelay = 60`, `TdrLevel` and
`TdrDdiDelay` unset. The default-2 s reasoning in all three hdCodex documents
does not apply to this machine, and the TDR hypothesis is falsified in the form
it was stated.

hdClaude's gallery table records GPU model, driver version, and TDR registry
values alongside every timing, so this class of investigation starts from data.

## What hdCodex got right and hdClaude keeps

The reviews were clear that the defects were implementation and contract
defects inside a sound architecture. Kept without change:

- Vulkan ray queries over `VK_KHR_ray_tracing_pipeline` for traversal.
- RAII image ownership with declared format/usage validated before allocation.
- A pre-device requirement provider so optional backends contribute instance and
  device requirements before device selection.
- RGBA32F reference precision kept separate from RGBA16F interactive formats.
- Progressive, interruptible rendering with accumulation reset on scene, camera,
  and output changes.
- Spectral transport with CIE XYZ sensor integration.
- Scene-linear EXR capture with a separate display transform for gallery JPEGs.

The one architectural departure is the megakernel, replaced by a wavefront
integrator — for the reason given in [architecture.md](architecture.md) §2, which
is a consequence of hdClaude's MaterialX commitment rather than a criticism of
hdCodex's choice.
