# Implementation notes

A running log of things discovered while building, as distinct from the design
documents, which state what the system *is*. Entries here are findings: an
assumption that turned out to be false, a constraint the API imposes, a bug
whose cause is worth remembering. Newest first.

Each entry says what was expected, what is actually true, and what changed as a
result. An entry is added whenever a design document has to be corrected.

---

## 2026-09-05 — MaterialX 1.39.3 is the version of record, not 1.39.6

**Expected.** The design was written against a 1.39.6 checkout, on the
assumption that 1.39.x was 1.39.x.

**Actually true.** OpenUSD 26.03 ships **1.39.3**, and that is what hdClaude
links and generates with. The differences are not cosmetic:

| | 1.39.3 (ours) | 1.39.6 |
|---|---|---|
| Hardware generator headers | `MaterialXGenShader/` | `MaterialXGenHw/` |
| Surface node class | `SurfaceNodeGlsl` | `HwSurfaceNode` |
| `conductor_bsdf` signature | no `retroreflective` | has it |
| `conductor_bsdf` energy compensation | takes `vec3 F` | takes `FresnelData` |
| `mx_ggx_dir_albedo` | takes `F0`, `F90` | takes `FresnelData` |
| `mx_ggx_VNDF_reflection_PDF` | **absent** | present |
| `add_bsdf` throughput | `t1 + t2` | `max(t1 + t2 - 1, 0)` |
| `layer_bsdf` throughput | `t1 + t2` | `t1 * t2` |
| `multiply_bsdf` throughput | scaled by the weight | unscaled |
| `ClosureData` construction | inline, fixed 6 args | `makeClosureData` + a substitution token |
| `getBsdfInputName()` etc. | absent | present |

**Changed.** Every override was rewritten from the 1.39.3 sources, and the
version of record is stated in each file. Three consequences worth keeping:

- `mx_ggx_VNDF_reflection_PDF` is defined in
  `lib/mx_pt_sampling.glsl` under its upstream name, so the definition can be
  deleted outright when the OpenUSD distribution moves to a MaterialX that
  supplies it.
- `layer_bsdf`'s selection probability reads `top.throughput` — the *child's*
  value, which a leaf closure sets to `1 - directional_albedo` — so it is
  unaffected by the combinator's own throughput rule changing between versions.
- `ClosureData` is left **byte-compatible with upstream**. 1.39.3 constructs it
  inline with a fixed argument list, so adding fields would break every
  construction site hdClaude does not itself replace. The hero wavelengths and
  the stratified sample travel in per-invocation globals instead, which costs
  nothing in a compute shader.

The 1.39.6 checkout remains useful for reading intent, but the sources under
`<usd>/src/MaterialX-1.39.3` are the ones to consult.

---

## 2026-09-05 — A wrong nodedef name in an implementation fails silently

**Expected.** An implementation declaration naming a nodedef that does not exist
would be reported.

**Actually true.** It is ignored. MaterialX falls back through target
inheritance to the stock implementation, generation succeeds, and the only
symptom is a duplicate `struct ClosureData` at GLSL compile time — because the
stock closure file pulls in the stock closure header beside ours.

The trap is that nodedef names do not follow file names:

```
mx_multiply_bsdf_color3.glsl  ->  ND_multiply_bsdfC     (not ND_multiply_bsdf_color3)
mx_multiply_bsdf_float.glsl   ->  ND_multiply_bsdfF
```

hdClaude's first declarations used the file-derived names, so the multiply
overrides did nothing.

**Changed.** `mtlx/pbrlib/genglsl_pt/hdclaude_pbrlib_impl.mtlx` is **generated
from** the stock `pbrlib_genglsl_impl.mtlx`, taking nodedef, file, and function
verbatim and changing only the target. Names cannot diverge because they are not
retyped. The file also carries the authoritative list of the 15 closures still
to override, with their real nodedef names.

---

## 2026-09-05 — The surface override needs a declaration, or it silently is not used

**Expected.** Registering `PathTracerSurfaceNode` in the generator's constructor
would be enough for it to be used.

**Actually true.** The C++ registration is keyed by implementation *name*
(`IM_surface_genglsl_pt`), and that name only exists if an
`<implementation nodedef="ND_surface" target="genglsl_pt"/>` declares it. Without
the declaration MaterialX resolves `surface` through target inheritance to the
stock `SurfaceNodeGlsl`.

hdClaude generated a complete, compiling shader in that state. Its body was the
rasteriser's: a `u_viewPosition`-based view vector, an "Add environment
contribution" block, and — worst — a local
`ClosureData closureData = ClosureData(CLOSURE_TYPE_INDIRECT, ...)` **shadowing
the caller's parameter**. Every closure would have been evaluated with a closure
type the integrator never asked for.

**Changed.** The declaration is emitted by the generator script alongside the
closure ones. More importantly the test now asserts the override *positively*,
by a marker comment only hdClaude's node emits, and negatively against
`u_viewPosition`, `Add environment contribution`, and any locally constructed
`ClosureData`. The original assertions — absence of `u_lightData` and
`mx_environment_radiance` — all passed while the override was inert, because
the scene had no lights and our closures had already dropped the indirect
branch. Absence-only assertions were the wrong shape.

`createVariables` is overridden too, so the material's uniform interface no
longer declares `u_viewPosition` or the lighting uniforms. Those are part of the
ABI, and no kernel can meaningfully fill them.

---

## 2026-09-05 — Screen-space derivatives are meaningless in a wavefront kernel

**This is not a MaterialX defect.** Stating it plainly because the shape of the
finding invites the wrong conclusion. `genglsl` is a *rasterisation* target;
`fwidth` is well defined in a fragment shader, and the function below is a
reasonable screen-space approximation there. MaterialX already treats subsurface
as target-specific and says so — compare the two implementations of the same
node:

```osl
// genosl, an offline target: real transport
bsdf = subsurface_bssrdf(N, weight * albedo, radius, anisotropy);
```

```glsl
// genglsl, a rasteriser target: a screen-space approximation, openly labelled
vec3 sss = mx_subsurface_scattering_approx(N, L, P, color, radius);
...
// "For now, we render indirect subsurface as simple indirect diffuse."
```

The incompatibility is entirely hdClaude's doing: we reuse a fragment-shader
library from a compute stage it was never written for. That is a good trade —
inheriting the whole of stdlib is the point of the target — but it means
inherited assumptions have to be checked, and this is the first one that failed.
Expect more, and expect them to be *stage* assumptions rather than shading ones.

**Actually true.** `mx_microfacet_diffuse.glsl` contains

```glsl
float curvature = length(fwidth(N)) / length(fwidth(P));
```

inside `mx_subsurface_scattering_approx`, and a compute shader rejects `fwidth`
without `GL_KHR_compute_shader_derivatives`.

Enabling that extension would be the wrong fix. Neighbouring lanes in a `shade`
dispatch are unrelated paths that may be on opposite sides of the scene, not
adjacent pixels; `fwidth` there measures nothing. The extension would make the
code compile and silently produce nonsense.

**Changed.** hdClaude carries its own `lib/mx_microfacet_diffuse.glsl`: upstream's
file with that one function removed. Its only caller is `mx_subsurface_bsdf`,
which hdClaude will override with a real bounded spectral random walk — the
transport this function approximates. Until then any path reaching it fails to
compile with the function named, which is the correct loud failure.

The general rule this sets: texture footprints and curvature in this renderer
come from ray differentials, never from screen-space derivatives.

---

## 2026-09-05 — The validation layer is a dependency, so it is built like one

**Expected.** The Khronos validation layer would be a developer's local Vulkan
SDK install, discovered if present.

**Actually true.** That does not hold here, because validation is a *gate*: the
GPU tests fail without the layer, by design. A dependency that a test suite
cannot run without is not an optional local install — it is a dependency, and
leaving it out of the build meant a fresh clone could not run its own tests.

**Changed.** `Vulkan-ValidationLayers` is now built from source like everything
else: pinned to the same `vulkan-sdk-*` tag as Vulkan-Headers, volk, and
glslang, built into the gitignored dependency tree, never committed. Opt-in via
`HDCLAUDE_BUILD_VALIDATION_LAYERS` or the `dev-validation` preset, because the
first build is long — roughly four minutes on this workstation, incremental
afterwards.

`ExternalProject` rather than `FetchContent`, for two reasons. The layers
configure their own dependency set (SPIRV-Headers, SPIRV-Tools) through
`UPDATE_DEPS`, so using their known-good revisions avoids pinning a second set
that could disagree with theirs. And they set enough CMake globals that
`add_subdirectory` would leak configuration into hdClaude's own targets.

Discovery order is now: an explicit `HDCLAUDE_VALIDATION_LAYER_DIR`, the
source-built tree, a copy-only SDK under `_deps`, then a system `VULKAN_SDK`.
Whatever is found is passed to the GPU test as `VK_LAYER_PATH` by CTest, so the
tests need no manual environment. When nothing is found the configure summary
says `Validation layer ... NOT FOUND (GPU tests will fail)`, rather than letting
it surface later as a puzzling test result.

Verified both directions: `ctest` passes with the layer auto-discovered and no
manual environment, and the test binary run without `VK_LAYER_PATH` refuses and
names the fix.

---

## 2026-09-05 — Batch files silently lost their CRLF endings

Editing the `.bat` files with tools that write LF left them with Unix line
endings. `cmd.exe` requires CRLF in batch files: labels and `GOTO` break, and
the failure is a cascade of "'...' is not recognized as an internal or external
command" for fragments of the file's own text, which does not obviously point at
line endings.

`.gitattributes` already declares `*.bat text eol=crlf`, so a fresh clone is
correct and only the local working tree was affected — which is the worse case,
because the problem is invisible in review while breaking the machine it was
edited on. All `.bat` files are normalised, and this is worth re-checking after
any bulk edit of them.

---

## 2026-09-05 — The `surface` node builds ClosureData, and builds it for a rasteriser

**Expected.** Overriding the `pbrlib` closure nodes plus a generator subclass
would be enough to emit a path-tracing shader.

**Actually true.** `ClosureData` is not constructed by the closures. It is
constructed by the **`surface` node**, in
`source/MaterialXGenHw/Nodes/HwSurfaceNode.cpp`, and that construction encodes a
rasteriser's whole structure:

```cpp
// HwSurfaceNode.cpp
:181  makeClosureData(CLOSURE_TYPE_INDIRECT,     L, V, N, P, occlusion)
:211  makeClosureData(CLOSURE_TYPE_EMISSION,     L, V, N, P, occlusion)
:237  makeClosureData(CLOSURE_TYPE_TRANSMISSION, L, V, N, P, occlusion)
:302  makeClosureData(CLOSURE_TYPE_REFLECTION,   L, V, N, P, occlusion)
```

The `REFLECTION` construction sits inside a loop over `u_lightData`, and the
`INDIRECT` one calls the prefiltered-environment path. Neither is meaningful to
a path tracer, which supplies its own direction and its own light sample from
the integrator.

**Changed.** hdClaude adds a C++ node implementation,
`PathTracerSurfaceNode`, registered by the generator and declared as
`<implementation nodedef="ND_surface" target="genglsl_pt"/>`. It emits a body
that calls the BSDF and EDF with the **caller-supplied** `closureData` — no
light loop, no environment lookup.

This is a small amount of code but it was not in the plan, and it is the piece
that actually turns a rasterisation shader graph into a path-tracing one. The
closure overrides supply sampling; this supplies the *calling convention*.

Note also that the stock `surface` node has no GLSL file at all — its
declaration is bodiless (`<implementation name="IM_surface_genglsl"
nodedef="ND_surface" target="genglsl" />`) because the implementation is
entirely C++. hdClaude's is the same shape.

---

## 2026-09-05 — Buffer device addresses need an explicit int64 extension

A `uint64_t` holding a buffer device address requires
`GL_EXT_shader_explicit_arithmetic_types_int64`, which is easy to omit because
`GL_EXT_buffer_reference` does not imply it. The diagnostic is unhelpful:

```
ERROR: test.targetFeatures:14: '' :  syntax error, unexpected IDENTIFIER
```

It points at the declaration, not at the missing extension. Since every
wavefront kernel reaches its queues and path state through device addresses,
this would have been rediscovered per kernel. It is asserted once in
`tests/glsl_compiler_tests.cpp`, in a shader that also exercises scalar block
layout and non-uniform descriptor indexing, so a misconfigured target
environment fails there rather than inside the first real kernel.

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
