# Implementation notes

A running log of things discovered while building, as distinct from the design
documents, which state what the system *is*. Entries here are findings: an
assumption that turned out to be false, a constraint the API imposes, a bug
whose cause is worth remembering. Newest first.

Each entry says what was expected, what is actually true, and what changed as a
result. An entry is added whenever a design document has to be corrected.

---

## 2026-09-05 — SER is unavailable from compute shaders

**Expected.** `docs/architecture.md` §2.1 stated that the `extend` kernel would
issue a `VK_NV_ray_tracing_invocation_reorder` hint on the hit material index,
recovering for a ray-query compute kernel the coherence benefit SER gives a
megakernel.

**Actually true.** SER's GLSL builtins (`hitObjectNV`, `reorderThreadNV`) are
registered by glslang only on `EShLangRayGen`, `EShLangClosestHit`, and
`EShLangMiss` — verified in
`glslang/MachineIndependent/Initialize.cpp`. There is no compute-stage
declaration. SER is a ray-tracing-pipeline feature; it cannot be used from a
ray-query compute shader.

Separately, the Vulkan validation layer rejects enabling
`VK_NV_ray_tracing_invocation_reorder` without also enabling
`VK_KHR_ray_tracing_pipeline`, which it requires:

```
vkCreateDevice(): pCreateInfo->ppEnabledExtensionNames[3] Missing extension
required to enable device extension VK_NV_ray_tracing_invocation_reorder:
 - VK_KHR_ray_tracing_pipeline
```

**Changed.** The extension is no longer enabled. The capability is still
*detected* and reported by the GPU probe, so the fact stays visible rather than
disappearing.

The cost is small, and the reason is structural rather than consoling: in a
wavefront renderer the per-material sort already delivers the execution
coherence SER exists to recover. SER's value is to megakernel and RT-pipeline
designs, which cannot sort because shading and traversal are fused. hdClaude
separated them, which is what makes the sort possible — and having paid for the
sort, SER would be buying the same thing twice.

This does mean one of the two arguments in `architecture.md` §2.1 for choosing
ray queries over an RT pipeline was wrong in the other direction: an RT
pipeline would have made SER available. The choice stands on the remaining
argument — the SBT buys nothing a wavefront design has not already bought —
but the record should not pretend the first argument was ever load-bearing.

---

## 2026-09-05 — Destroy on a lost device; skip only the wait

**Expected.** Rule 3 in `docs/architecture.md` §6 was implemented as "on device
loss, skip waits *and* per-object destruction", on the reasoning that calls
against a dead driver are what contaminated hdCodex's post-fault diagnostics.

**Actually true.** `vkDestroyDevice` requires that every child object has
already been destroyed, and destruction remains valid after device loss.
Skipping the command pool destroy produced an access violation inside
`vkDestroyDevice` on the first run of the device-loss test.

**Changed.** The rule is now stated precisely: **skip the wait, not the
destruction.** Waiting is what must be skipped — an unchecked `vkDeviceWaitIdle`
on a dead device returns `VK_ERROR_DEVICE_LOST`, is ignored, and every
diagnostic gathered afterwards is contaminated. Destroying is mandatory.

Worth noting that the test that found this is the one hdCodex never had.

---

## 2026-09-05 — A validation gate that cannot fail is not a gate

**Expected.** Lesson R8 was implemented as an atomic validation-error counter
checked at the end of the GPU test run.

**Actually true.** On a machine with no Khronos validation layer installed, the
context logged a warning, continued without validation, and the error-count
check passed against a counter nothing could ever increment. The run reported
"0 errors" and exited 0. That is R8 reproduced inside the fix for R8.

**Changed.** The GPU test now **fails** if validation was requested and the
layer is unavailable, naming the missing dependency. `docs/building.md` records
that the Vulkan SDK is optional for building and rendering but required to close
any phase.

Both this and the SER finding were surfaced by the same first validation-enabled
run. Neither would have been found by reading the code.

---

## 2026-09-05 — Resource destruction order versus the device

**Expected.** Nothing in particular; the GPU test created a `VulkanAllocator`
after the `VulkanContext` and explicitly reset the context at the end.

**Actually true.** Validation reported three leaked `VkDeviceMemory` objects at
`vkDestroyDevice`: the allocator and its buffers outlived the device they were
allocated from.

**Changed.** The allocator is scoped so that it and everything it owns are
destroyed before the device. Recorded here because the same ordering hazard will
recur in the render delegate, where the context, the allocator, the scene, and
the frame slots all have to be torn down in a defined order, and where the
symptom will again be a leak report rather than a crash.

---

## 2026-09-05 — MaterialX gives us more than expected, and one thing less

**Expected.** hdClaude would have to implement GGX importance sampling and its
density to add path-tracing support to the MaterialX closures.

**Actually true.** `mx_ggx_importance_sample_VNDF` and
`mx_ggx_VNDF_reflection_PDF` already exist in
`pbrlib/genglsl/lib/mx_microfacet_specular.glsl`, upstream, because the
prefiltered-environment path needs them. They are used verbatim. A second GGX
implementation that disagreed with MaterialX's own would be a fidelity bug no
self-test could detect.

What MaterialX does *not* give us is a place to put a density during sampling —
see the entry below.

**Changed.** Nothing in the design; this made the design cheaper than budgeted.

---

## 2026-09-05 — Closure sampling cannot return a density

**Expected.** A `PT_SAMPLE` closure query would return a direction and its
density, and a separate `PT_PDF` query would supply densities for MIS.

**Actually true.** MaterialX generates a closure graph by evaluating children
*before* the combinator that consumes them: `mx_mix_bsdf` receives `BSDF fg` and
`BSDF bg` already evaluated. Under `PT_SAMPLE` those two densities belong to two
*different* directions — each child's own sample — and cannot be mixed. A `mix`
node has no way to compute the mixture density its MIS weight requires.

**Changed.** Sampling returns a direction only; `CLOSURE_TYPE_REFLECTION` and
`CLOSURE_TYPE_TRANSMISSION` write `BSDF.pdf` beside `BSDF.response`, and every
combinator mixes densities with the weights it already mixes responses with.
Full reasoning in `docs/materialx-codegen.md` §2.

This one is worth dwelling on because the wrong version is comfortable: it
compiles, it renders, it converges, and the image it converges to is wrong.
Reporting the selected child's density instead of the mixture breaks every MIS
weight in the renderer and is invisible to inspection. The acceptance test that
catches it — combinator mixture densities integrating to one — is listed
separately in `materialx-codegen.md` §8 for that reason.

---

## 2026-09-05 — The pbrlib override set is all-or-nothing

**Expected.** hdClaude could override MaterialX closures incrementally,
inheriting the not-yet-overridden ones from `genglsl`.

**Actually true.** MaterialX resolves a source file's `#include` relative to
that file's own directory. An upstream `pbrlib/genglsl/mx_dielectric_bsdf.glsl`
therefore resolves `lib/mx_closure_type.glsl` to *upstream's* copy. Generating
one upstream closure beside one hdClaude closure emits both copies and declares
`struct ClosureData` twice.

The set is exactly the pbrlib GLSL files that include `mx_closure_type.glsl`: 22
files in MaterialX 1.39. **No `stdlib` file includes it**, which is what keeps
the inheritance boundary clean — the entire pattern, math, colour, and
procedural library is inherited unchanged.

**Changed.** The 22 files are enumerated in
`mtlx/pbrlib/genglsl_pt/hdclaude_pbrlib_impl.mtlx` rather than left implicit,
with the completed ones marked. Generation cannot succeed until all 22 exist,
so this is a phase-4 completion criterion rather than a nice-to-have.

---

## 2026-09-05 — Deriving the Python requirement from the USD install

Not a correction, but worth recording as a technique.

`pxrConfig.cmake` in an OpenUSD distribution contains the absolute path of the
interpreter the distribution was built against:

```cmake
set(Python3_EXECUTABLE [[C:\...\Python312\python.exe]])
```

`setup_usd_env.bat` parses that, queries the recorded interpreter for its
version, and compares it against the active environment's Python. The check is
therefore derived rather than hard-coded, and stays correct when `USDROOT`
changes — a hard-coded "3.12" is right until the day someone points the scripts
at a different USD build.
