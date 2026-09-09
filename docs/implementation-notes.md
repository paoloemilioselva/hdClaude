# Implementation notes

A running log of things discovered while building, as distinct from the design
documents, which state what the system *is*. Entries here are findings: an
assumption that turned out to be false, a constraint the API imposes, a bug
whose cause is worth remembering. Oldest first, so a later entry can correct an earlier one and the correction reads in order.

Each entry says what was expected, what is actually true, and what changed as a
result. An entry is added whenever a design document has to be corrected.

---

## 2026-09-05 — A VNDF density does not integrate to one, and that is correct

**Expected.** The acceptance test in `materialx-codegen.md` §8 said "combinator
mixture densities integrate to one over the sphere". The first implementation
asserted exactly that, and every GGX-based closure failed it:

```
conductor (roughness 0.8)   density 0.6246
conductor (roughness 0.4)   density 0.8602
dielectric                  density 0.9122
```

**Actually true.** The closures were right and the assertion was wrong. A
visible-normal sampler reflects the view direction about a sampled microfacet
normal, and on a rough surface that normal can be tilted far enough that the
result lands *below the horizon*, where the closure cannot scatter. Those
samples are discarded with weight zero and the estimator stays unbiased — but
the probability mass they carry is exactly what is missing from the density
integral.

The arithmetic is unambiguous:

| closure | density | discarded | sum |
|---|---:|---:|---:|
| conductor, roughness 0.8 | 0.6234 | 0.3749 | 0.9983 |
| conductor, roughness 0.4 | 0.8607 | 0.1376 | 0.9984 |
| dielectric | 0.9146 | 0.0836 | 0.9982 |
| `mix(conductor, diffuse)` | 0.9292 | 0.0692 | 0.9984 |
| `add(diffuse, conductor)` | 0.8990 | 0.0985 | 0.9975 |
| `layer(dielectric, diffuse)` | 0.9959 | 0.0019 | 0.9978 |

**Changed.** The invariant asserted is now

    integral of the reported density  +  P(sampler yields a zero-density direction)  =  1

which is what actually has to hold, and which a combinator reporting a selected
child's density instead of the mixture still fails. §8 of the codegen document
is corrected to state it that way.

Worth keeping: a bare density integral of 0.62 looks catastrophic and is
perfectly correct. The instinct to "fix" the closures at that point would have
introduced a real bug to satisfy a wrong test.

One residual limitation, recorded rather than hidden: the density integral is
estimated by uniform sampling of the sphere, which is a poor estimator for a
narrow lobe. At roughness 0.1 a GGX lobe covers about 1% of the sphere, so 1% of
samples carry the whole integral and the measured sum is 0.94 rather than 1.00.
Narrow lobes therefore get a looser tolerance, and the energy check — which
importance samples, and so does not suffer this — is what constrains them.

---

## 2026-09-05 — The overflow that produced a plausible number

Raising the validation sample count from 2^18 to 2^20 without lowering the
fixed-point scale wrapped a 32-bit accumulator. The result was not obviously
broken:

```
burley_diffuse   albedo 1.0056   ->   albedo 0.0056
layer            albedo 1.0001   ->   albedo 0.0001
```

1.0056 × 2^20 × 4096 = 4.319e9 against a 4.295e9 limit, and the wrapped
remainder divides back to exactly 0.0056. Two closures silently reported an
albedo three orders of magnitude too low, and the *test still passed*, because
an albedo below one is what the energy check is looking for.

The bound is `scale × samples × mean < 2^32`, and its three terms drift
independently as a test is tuned. So the host now checks the raw accumulators
against three quarters of the range and fails on saturation, rather than relying
on that arithmetic staying correct. The scale is documented with the bound
beside it.

A guard that detects the condition beats a comment asserting it cannot happen —
the comment was already there, and was already wrong.

---

## 2026-09-05 — Replacing the surface node changes who needs `closureData`

**Expected.** Overriding the `surface` node so it evaluates against the caller's
`closureData` would be self-contained.

**Actually true.** It was, for a hand-built graph whose surface node sits
directly in the material. It broke the moment a *named* surface shader was
tried, because `standard_surface` and `open_pbr_surface` are nodegraphs, and
MaterialX emits a nodegraph as its own GLSL function:

```
ERROR: standard_surface:2437: 'closureData' : undeclared identifier
```

`ClosureCompoundNode` decides whether to thread `closureData` into that
function's signature and call by asking
`ShaderGenerator::nodeNeedsClosureData(node)`, and
`HwShaderGenerator` answers yes for BSDF, EDF and VDF nodes only. That is
correct for the stock target: its surface node *constructs* its own
`ClosureData`, so a shader nodegraph never needs one passed in.

hdClaude's does not construct one, so shader and surface nodes must thread it
too.

**Changed.** `PathTracerShaderGenerator::nodeNeedsClosureData` also returns true
for `Classification::SHADER` and `Classification::SURFACE`. The override and
`PathTracerSurfaceNode` belong together: replacing the calling convention is
what creates the requirement.

The wider point: an extension point can have consequences several layers away
from where it is applied, and a test on a small hand-built graph will not find
them. Testing against the named surface shaders real assets use is what did.

---

## 2026-09-05 — Extending BSDF means extending VDF too

**Actually true.** MaterialX 1.39.3 registers `Type::VDF` as an *alias* of the
BSDF struct — the same type name, no definition of its own — but with a
**separate copy of the default-value literal**:

```cpp
registerTypeSyntax(Type::BSDF, AggregateTypeSyntax(this, "BSDF",
    "BSDF(vec3(0.0),vec3(1.0))", ..., "struct BSDF { ... };"));

registerTypeSyntax(Type::VDF, AggregateTypeSyntax(this, "BSDF",
    "BSDF(vec3(0.0),vec3(1.0))", EMPTY_STRING));   // no definition, own literal
```

Re-registering only `Type::BSDF` therefore leaves every VDF variable initialised
with a two-field literal for an eight-field struct:

```
ERROR: open_pbr_surface:2626: 'constructor' : Number of constructor parameters
does not match the number of structure fields
```

**Changed.** Both types are re-registered, from one shared default-value string
and one shared definition, so they cannot drift apart.

Worth noting *when* this surfaced. The hand-built closure material and
`standard_surface` both passed; only `open_pbr_surface` failed, because it is
the first of the three to use a VDF. A defect reachable by one material in three
is the argument for testing against the shaders assets actually use rather than
against a graph written to exercise what was just implemented.

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
retyped, and the generator is committed as
`tools/generate_pt_implementations.py` so the same guarantee survives the next
MaterialX version.

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

---

## 2026-09-06 — Replacing `emitPixelStage` also means owning its token substitutions

**Expected.** `PathTracerShaderGenerator::emitPixelStage` had to replace the
stock GLSL pixel stage in order to emit `hdclaude_material_shade` instead of
`main`. Everything else the stock stage did was believed to be rasteriser
scaffolding this target does not want.

**Actually true.** `GlslShaderGenerator::emitPixelStage` also sets the
`$fileTransformUv` token substitution, immediately before emitting node
function definitions. Every stock `mx_image_*.glsl` opens with
`#include "lib/$fileTransformUv"`, so without that assignment the include is
unresolvable and generation fails with:

```
Could not find include file: 'lib\$fileTransformUv'
```

The failure is invisible until a *textured* material is generated. The three
materials the closure and generation tests use are untextured, so the whole
suite passed while every textured material in a real asset failed to generate.
The first gallery scene surfaced it immediately: ten of its twenty materials.

**Changed.** `emitPixelStage` sets the substitution itself, before
`emitFunctionDefinitions`. The general lesson is recorded here because it will
recur: overriding a MaterialX emission hook inherits responsibility for the
side effects the base implementation performed, not only for the code it wrote.

---

## 2026-09-06 — A generated material that samples textures faults the device

**Expected.** A material hdClaude cannot fully support would render
incorrectly — wrong colour, missing detail — but would still render.

**Actually true.** MaterialX emits `uniform sampler2D` for an `<image>` node's
file input. hdClaude binds no texture descriptors, so the pipeline's descriptor
set has nothing at that binding. Sampling an unbound descriptor is undefined
behaviour, and on this driver it is not a wrong colour: it is
`VK_ERROR_DEVICE_LOST` on the first dispatch, which then poisons the sticky
device-lost latch and takes every subsequent frame with it.

**Changed.** `HdClaudeMaterialCompiler` inspects the generated shader's uniform
blocks for ports of type `FILENAME` and refuses such a material by name,
falling back to the mesh's displayColor. The check is against the *generated
shader* rather than the Hydra network, because what matters is what the emitted
code actually reads.

This is also why the refusal is distinguished from a compile failure: a
textured material is a known gap, not a defect, and raising `TF_RUNTIME_ERROR`
for every textured asset would bury the failures that are real.

---

## 2026-09-06 — Progressive rendering exhausted the descriptor pools

**Expected.** `PathTracer::Render` allocating a descriptor set per pipeline per
call was fine; the pools hold eight sets each.

**Actually true.** It was fine only because nothing had ever called `Render`
more than a few times. The Hydra render pass traces a slice of the sample
budget per execute, so a 64-sample image at four samples per frame is sixteen
calls, and the eighth failed with `VK_ERROR_OUT_OF_POOL_MEMORY`.

**Changed.** `ComputePipeline::ResetSets` returns a pool's sets, and `Render`
resets every pool before allocating. This is safe because every submit `Render`
makes is waited on before it returns, so no set can still be in flight.

Allocating once and rewriting descriptors only when a resource generation
changes would be better still, and is recorded in the roadmap rather than done
here: it needs the descriptor-generation tracking to be authoritative first.

---

## 2026-09-06 — Retrying a failed frame forever is worse than committing it

**Expected.** `docs/lessons-from-hdcodex.md` C3 says a failed revision must be
retried rather than committed, because hdCodex marked a failed frame converged
and left the viewport permanently stale.

**Actually true.** The opposite extreme is just as bad. When the device was
lost on the first frame, the render pass correctly declined to converge — and
`usdrecord`, which renders until convergence, never returned. The process sat
at 19 seconds of CPU over eleven minutes with a dead GPU, producing nothing and
printing nothing.

**Changed.** A failure is retried a bounded number of times
(`kMaxConsecutiveFailures`, currently three) and then reported as converged
with an error naming what happened. A transient fault still recovers; a
deterministic one stops. The counter resets on any successful frame.

The underlying rule is that *both* failure modes are silent to a user: one
shows a stale image forever, the other shows nothing forever. Neither is
acceptable, so the renderer has to say what went wrong and then let the host
proceed.

---

## 2026-09-06 — The first gallery render is black, and that is the scene

**Not a defect, but worth recording so it is not rediscovered.**

`gallery/shader_ball_gold.usda` renders pure black from its authored camera —
`luminance min 0.00000 max 0.00000` straight out of the tracer, not out of the
AOV. The same scene from `usdrecord`'s default framing camera renders correctly
lit, 100% of pixels non-black, mean luminance 0.21.

The asset is a sealed studio set: the camera is inside a closed box lit
entirely by `RectLight` prims. hdClaude does not consume UsdLux yet, and its
stand-in sun and sky are outside the box, so no ray reaches anything emissive
and every path returns zero. The renderer is behaving correctly on a scene it
cannot yet light.

The diagnostic that settled it is worth keeping: with `HDCLAUDE_TRACE=1` the
render pass reports the luminance range of what the tracer returned, before the
AOV or any display transform sees it. A black viewport has two very different
causes, and that one line separates them.

---

## 2026-09-06 — Lights are analytic, which is what makes MIS unnecessary for now

**Decision, recorded so the reasoning survives.**

hdClaude's UsdLux lights are not geometry: they are not in the acceleration
structure, and a scattered ray cannot hit one. Only next-event estimation finds
a light.

That is what lets the first implementation skip multiple importance sampling
entirely. There is no second strategy finding the same light, so there is
nothing to weight against. Emissive *geometry* is different -- a scattered ray
does hit it, and its emission is added on hit -- and the two mechanisms do not
overlap.

The cost is variance: a glossy reflection of a large rect light is found only
by the light sample, so it is noisier than it would be with BSDF sampling and
MIS. That is a quality issue at high roughness contrast rather than a bias, and
it is the right trade against implementing MIS before the estimator itself is
validated.

Light selection is uniform rather than power-weighted for the same reason: a
power distribution must be rebuilt whenever any light changes, and a stale one
is a far subtler defect than extra noise.

**What this means for the light table.** `castsShadows` is a flag on the light
rather than a separate list, so a light with `shadowEnable` off contributes
directly without a shadow ray. Rectangle and disk lights emit from one face;
the sampler rejects the back side rather than doubling the light.

---

## 2026-09-06 — UsdLux `normalize`, and why extents are read from the transform

Two details worth stating because both are easy to get subtly wrong and neither
announces itself.

`inputs:normalize` makes a light's total power independent of its size, so the
*radiance* it emits falls as its area grows. hdClaude divides by area at
publication rather than at sampling time, so the GPU never needs to know the
flag existed.

A light's width, height and radius are authored in its own space, and its
transform may scale it. Reading `inputs:width` alone would light a scaled
studio set as though it were unscaled. The extents are therefore built by
transforming the light's local axes and taking their world lengths, which is
also what makes the area used for the density agree with the area actually
sampled.

`inputs:enableColorTemperature` is deliberately not applied. Blackbody
conversion belongs with the spectral upsampling in phase 6; an RGB
approximation now would have to be unlearned, and hdClaude does not approximate
where it can wait. (Superseded on 2026-09-07, when the spectrum arrived and the
control was implemented as one -- see the entry of that date.)

---

## 2026-09-06 — A skinned mesh does not author its points

**Expected.** `sceneDelegate->Get(id, HdTokens->points)` returns a mesh's
points. A prim that returns none has no geometry and can be dropped.

**Actually true.** For a UsdSkel character, the deformed points are the *output
of an ExtComputation*, and the plain `Get` returns nothing. Dropping such a
prim is not "it has no geometry": it is discarding the character. In
`gallery/collectiveproject001.usda` this reduced the scene from its actual
content to 70 triangles of set dressing, and the deforming character was
absent rather than visibly wrong -- which is exactly the kind of failure that
survives a review.

**Changed.** `HdClaudeMesh::Sync` asks for
`GetExtComputationPrimvarDescriptors(id, HdInterpolationVertex)` first and
evaluates them through `HdExtComputationUtils::GetComputedPrimvarValues`,
falling back to the authored points only when no computation supplies them.
The same scene now publishes 6516 triangles, the character among them.

Declaring `HdPrimTypeTokens->extComputation` as a supported sprim was necessary
but not sufficient: without it the computation does not exist, and with it but
without reading its output the mesh still stands in its bind pose or vanishes.

---

## 2026-09-06 — The film was upside down, and glass could never leave itself

Two defects a user spotted immediately in the first images, both of which the
test suite was structurally unable to catch.

**The film.** `raygen` negated NDC y, putting row 0 at the top of the image.
Hydra's render buffers put row 0 at the *bottom* -- hdEmbree builds its NDC
without the negation, which is the reference worth trusting here. Every image
hdClaude handed a Hydra host was therefore vertically flipped.

No test caught it because every render test asserted on content that is
symmetric under a flip: a centred quad, a background corner, and a shadow
searched for by brightness rather than by position. The convention is now
stated in `PathTracer::Render`'s contract rather than left implicit, the
debug PPM writer flips on output because PPM is top-down, and the shadow test
scans the whole image instead of a quadrant so it no longer encodes an
assumption about which half is which.

**Glass.** `hdclaude_reconstruct` turned both normals to face the incoming ray
before handing them to the material. `mx_dielectric_bsdf` decides which side of
an interface it is on from `dot(N, V)` *before* it does its own forward-facing
flip -- its own comment says so -- so pre-flipping made `entering` true for
every hit. A ray inside the glass refracted as though entering again and could
never exit, which is why the glass shader ball rendered as a dull neutral
solid.

Every MaterialX closure calls `mx_forward_facing_normal` itself, so the fix is
to stop flipping: `SurfacePoint` now carries the true normals plus a separate
`frontGeometricNormal` for the decisions that genuinely need a viewer-facing
one -- ray offsets and which side a light is on.

The default bounce limit went from four to eight at the same time. Four is not
enough for an entry, an exit, and whatever lies behind the glass, and a
transmissive material that goes dark at the depth limit reads as a shading bug.

**The general lesson.** Both defects were in a *convention* rather than in a
computation, and both were invisible to tests that check magnitudes. A closure
that documents an ordering requirement -- read the side before you flip -- is
stating a precondition its caller can violate silently.

---

## 2026-09-06 — Textures, and why they are bindless

**The collision that forced the design.** MaterialX's stock Vulkan binding
context emits the public uniform block and every sampler into **set 0**, at
bindings it counts from zero. Set 0 is the path state. So every generated
material -- textured or not -- declared `layout(std140, binding=1) uniform
PublicUniforms_pixel` on top of `pathOrigin`, a storage buffer. It never
faulted only because that block is dead once SHADER_INTERFACE_REDUCED has baked
the values, and a textured material put a `sampler2D` on top of
`pathDirection`, which is the lost device recorded above.

**What replaced it.** `PathTracerShaderGenerator::emitUniforms` claims no
descriptors at all:

- a sampler uniform becomes `#define <name> hdclaude_textures[i]`, an index
  into one shared array, so the stock `mx_image_*.glsl` files work unchanged --
  `texture(name, uv)` still expands to a sampler expression;
- a value uniform becomes a plain global at its default, because nothing reads
  it after the values are baked and it needs no storage.

**Local indices, shared pool.** The generator numbers each material's samplers
from zero, so two materials both use index 0 for different images. Each
material's descriptor set is therefore written with its own image array, mapped
through `CompiledMaterial::textureSlots` into a scene-wide pool that holds each
distinct image once. Local numbering keeps the generator independent of the
scene; the pool keeps one image one upload.

**Where the array is declared.** In the generated material, not in
`path_state.glsl`. The shade kernel is *appended* to the material, so anything
`path_state.glsl` declares arrives hundreds of lines after the material body
that samples it. That produced `'hdclaude_textures' : undeclared identifier`
and is worth remembering as a general property of this pipeline: the material
comes first, and anything the material needs must be emitted by the generator.

**Resolving the path.** `hdMtlx` writes the *authored* asset path into the
document -- it calls `SdfAssetPath::GetAssetPath` -- so `../maps/uvgrid.exr`
stays relative and no resolver can anchor it afterwards. The resolved path
lives in the Hydra network, where USD anchored it against the authoring layer.
The two are joined by reproducing the name rather than pattern-matching it:
hdMtlx names a MaterialX node `HdMtlxCreateNameFromPath(hdNodePath)`, which is
the path's last element, and MaterialX names a sampler after its node and
input. Both halves are public API.

**Unused slots are written.** Every element of the array gets the magenta
placeholder rather than being left undefined. An undefined descriptor is
undefined behaviour the instant a shader indexes it, and on this driver that is
a lost device rather than a wrong pixel -- so an out-of-range index in
generated code should show up as obvious magenta.

---

## 2026-09-06 — GeomSubsets without splitting the mesh

A per-face material binding could be implemented by splitting a mesh into one
prototype per subset. hdClaude does not, because that turns a twenty-subset
asset into twenty acceleration-structure builds and twenty instances for one
piece of geometry.

Instead the binding becomes a per-triangle material index.
`HdMeshUtil::ComputeTriangleIndices` already records the coarse face each
triangle came from, so mapping faces to subsets and triangles to faces is
bookkeeping rather than geometry work.

Two placement decisions are worth stating.

The subset's *material path* is what the mesh publishes, not an index. A mesh
has no way to know which index a material will be assigned, and is routinely
synced before the material prim exists at all; only the scene store, at
snapshot time, can resolve one. A subset naming a material that is still
missing falls back to the mesh's own binding rather than vanishing.

The per-triangle indices live in their own buffer, *not* in the BLAS. A
material index changes whenever the material set changes, and folding it into
the acceleration structure would invalidate the fingerprint that lets a static
scene republish without rebuilding anything.

---

## 2026-09-06 — Completing UsdLux, and a double count the stand-in sun was hiding

**What arrived.** Cylinder lights, UsdLuxShapingAPI's cone and focus terms,
colour temperature, and a dome light's latitude-longitude environment map. With
rect, disk, sphere, distant and dome that is the whole of UsdLux's analytic set;
what remains is IES profiles, light filters, and geometry lights, each reported
by name when a scene uses one.

**Shaping multiplies radiance, not density.** A cone changes how much light
leaves in a direction; it does not change how the sampler chose that direction.
Folding the falloff into the solid-angle density instead would make a spot
light's estimator wrong exactly where the cone cuts off -- which is the part of
a spot light anyone looks at.

**Colour temperature tints without brightening.** The blackbody fit is
normalised to unit luminance before it multiplies the light's colour, so
raising the temperature changes hue and nothing else. A fit that is not
normalised makes the temperature control double as an exposure control, and the
two are then impossible to separate in an authored scene. (The promise survives
the RGB fit that made it: on 2026-09-07 the tint became a transported spectrum,
and the normalisation became an equality of luminous integrals.)

**The dome map is its own sampler, not an array element.** The environment
kernel has no generated material in front of it, and the texture array is
declared by the generator, so the environment kernel has no array to index. A
dedicated binding is simpler than making the array visible to every kernel.

**The double count.** The environment kernel added the stand-in sun's disc on
every bounce while the shade kernel also estimated that sun by next-event
estimation, so any path that scattered and then struck the disc counted it
twice. The disc is now added on the camera ray only. A path leaving a delta
closure gets neither, because next-event estimation skips a delta and the
environment test cannot tell that it did -- that is precisely the gap multiple
importance sampling closes, and it is why the sun stays documented as a
fallback rather than promoted to a light.

---

## 2026-09-06 — Exposure is a control, not a fudge

The gallery renders were blown out, and the tempting fix -- quietly scaling the
lighting -- would have made hdClaude disagree with every other renderer on what
an authored intensity means.

`usdrecord` already applies an sRGB transfer function by default, so the
brightness was not a missing encode: a 4K studio HDRI at unit intensity really
does clip. The answer is an exposure control, in stops, applied to the resolved
image after the film is read back. It defaults to zero, so the AOV carries the
radiance the renderer computed and a host with its own display transform is
unaffected; and because it is applied after resolve rather than during
accumulation, changing it costs no samples.

---

## 2026-09-06 — Subdivision refines on the CPU, and that is a choice

hdClaude refines uniformly through OpenSubdiv and traces the refined cage. The
alternatives -- evaluating limit patches at intersection time, or feature-
adaptive tessellation -- are a different project, and one that only starts to
pay for itself once displacement exists. What made the CPU route the right
first answer is that a ray tracer needs an explicit surface to build an
acceleration structure over either way.

Three details are easy to get wrong and each shows up as a plausible-looking
image rather than as an error.

**Authored normals and UVs belong to the control cage.** After refinement they
describe a mesh that no longer exists. Their array lengths happen to be checked
against the vertex count elsewhere in Sync, so a refined mesh would simply fail
that check and fall through -- silently, and only for meshes whose refined
vertex count differed. They are now not consulted at all when a mesh is
refined.

**The coarse adjacency does not describe the refined cage**, so smooth normals
come from the refined triangles directly. The accumulated face normals are left
unnormalised on purpose: the cross product's length is twice the triangle's
area, which weights each face by its size and stops a sliver from dominating a
vertex it barely touches.

**A GeomSubset is authored on the control cage.** Walking
`GetFaceParentFace` back up the refinement levels is what keeps a subset
selecting the right refined triangles; without it, a two-subset mesh at level
two assigns materials to faces that no longer correspond to anything the asset
authored. The subset path and the unsubdivided path now produce the same two
things -- indices and a coarse face per triangle -- so everything downstream is
written once.

The refinement level is read once, at delegate construction, because it changes
the geometry every mesh publishes rather than anything the render pass can vary
per frame.

---

## 2026-09-06 — The sort, and the barrier that was never checked

The per-material dispatch was correct before this change and wasteful: every
shading pipeline was dispatched over every active path and each invocation
discarded the paths that were not its own, so shading cost the number of
materials times the number of paths. The counting sort removes the discards,
and with it the last CPU-sized dispatch in the frame.

**Every dispatch after `raygen` is now indirect.** That was always the design
(§2 of [wavefront-integrator.md](wavefront-integrator.md)) and it is what the
sort forces: a material's group size exists only on the device, so either the
CPU reads it back -- a stall in the middle of every bounce, and the readback the
design forbids -- or a kernel writes the dispatch command. A kernel writes it.

**The misses keep their place on the active queue.** Grouping them would mean
a group the environment kernel would then have to find, and the environment
kernel already walks the active queue. So the groups partition the hits, not
the queue, and the two kernels read different things without either filtering
the other's paths.

**The sort is invisible in an image**, which is the difficulty in testing it:
dispatching every pipeline over every path produces exactly the same picture. It
is asserted instead against the counts the sort itself wrote -- two quads, one
sample, one bounce, and the two groups must sum to the number of shaded pixels
in the image. They do, 1169 + 1167 = 2336, and that is also the only time those
GPU-written counters are ever read by the host.

**Core validation had never checked the barriers.** Adding synchronisation
validation to the context reported, on the first run, a write-after-read hazard
that had been in every frame since compaction landed: the counter reset between
bounces is a `vkCmdFillBuffer`, the queue promotion before it a
`vkCmdCopyBuffer`, and the barrier between them named only the compute stage.
Transfer work was ordered against nothing. It happened to work -- the driver
serialises those two commands anyway -- which is exactly why a gate is worth more
than an inspection. The barrier now covers transfer and indirect reads as well
as compute, and the setting is enabled in `VulkanContext` rather than left to an
environment variable, because a check nobody remembers to turn on is not a gate.

---

## 2026-09-06 -- Two ways to hand a material geometry, both wrong

Measuring the sort found two defects that had nothing to do with the sort. Both
are the same shape: a value the shading code reads was never the value the
renderer meant to give it, and neither produced an error, a validation message,
or an obviously broken picture.

**A skipped assignment is undefined, not deferred.** The generated
`hdclaude_set_surface_hit` assigned world position, normal and tangent from its
arguments and emitted `vd.x = vd.x` for everything else, with a comment saying
the kernel would fill those directly. Nothing did. `vd` is a global, so a
material reading the object-space position -- which is every 3D procedural
pattern node, since that is the space they are authored in -- read uninitialised
memory, and so did every material reading a texture coordinate, which is every
image node. The setter now takes the object-space frame and the UV, and every
remaining member is assigned its type's zero rather than itself.

The symptom is worth recording because it is so unhelpful: a `fractal3d`-tinted
diffuse material rendered *black*. Undefined input made the pattern undefined,
the closure's response undefined, and the shadow-ray contribution failed
`dot(c, c) > 0` -- which is how a NaN is rejected. It also cost nothing to
evaluate, since a compiler may do as it likes with an undefined value, so the
first attempt at measuring per-material dispatch measured a shading graph the
GPU was entitled to skip entirely.

**A row-major transform read as columns is its own transpose.** An instance
transform is stored row-major 3x4 -- the layout Vulkan's acceleration-structure
instance wants, so the host writes it once for both purposes. GLSL indexes a
`mat3x4` by column, so `transform[0]` in the shader is the *first row* of what
the host wrote, and `mat3(transform[0].xyz, ...)` is therefore the transpose of
the intended matrix. The kernel built its normal matrix by transposing that --
arriving back at the inverse instead of the inverse transpose -- and transformed
tangents by the transpose as well.

Under translation and uniform scale the two agree, which is why every test
scene had passed. Under rotation they do not: a quad tilted by an instance
transform shaded differently from the same quad tilted on the host and
instanced with the identity. That is now a test, because it is the comparison
that makes the defect visible -- the surface is in the right *place* either way,
since positions come from the acceleration structure, and only the shading
differs.


---

## 2026-09-06 -- The first gallery pass, and what ten scenes said

The gallery script existed only as a reference in `render_gallery.bat`; the
PowerShell file it called had never been written, so nothing had ever rendered
the ten scenes in one pass. Writing it -- plus the display transform and the
image gate it drives -- turned the gallery from a contract into a measurement,
and the measurement is worth more than the images.

Seven scenes render, in 7.6 to 35.4 seconds each at 1024x1024 and 1024 samples.
Three do not, and each fails differently:

**KitchenSet loses the device.** `VK_ERROR_DEVICE_LOST` from
`vkWaitForFences(immediate)` inside `PathTracer::SetScene`, before a single ray
is traced. Nothing downstream of publication is implicated. What the failure
did *not* do is as interesting: the latch held, every later entry point
refused, the render pass gave up after three attempts instead of spinning, and
`usdrecord` exited non-zero rather than writing a black image and calling it
success -- the hdCodex failure this repository was started to avoid
([lessons](lessons-from-hdcodex.md) R9). It is the largest scene in the gallery
by prototype count, which is the first thing to look at.

**Collective Project 001 asks for a primvar nobody bound.**
`HdMtlxCreateMtlxDocumentFromHdNetwork` produces a `geompropvalue` node with no
`geomprop` input, and generation refuses it. hdClaude reports a material it
cannot compile rather than approximating one, and `usdrecord` treats that
report as fatal, so one material stops the scene. That policy is right and its
consequence is worth stating plainly: a single unsupported node costs the whole
image, so the set of nodes that generate has to be widened, not the policy
loosened.

**OpenPBR Playground finds the derivative problem again.** `mx_aastep` computes
its filter width with `dFdx`, which a compute stage rejects -- the same finding
as the `fwidth` one recorded above, in a different file, exactly as that note
predicted ("expect more, and expect them to be *stage* assumptions"). The right
answer is the same in shape and different in substance: a path tracer's
antialiasing comes from sampling the pixel, so the filter width there is zero,
not an error. That is a change to make deliberately rather than by enabling
`GL_KHR_compute_shader_derivatives`, which would compile and quietly measure
nothing.

The seven that render disagree with hdCodex in ways the images make obvious and
no test would have: Sponza is far too dark, both shader balls show the
renderer's magenta placeholder on an inner shell, their backdrop's printed
numbers are mirrored, the glass ball is opaque, and the height map's texture
does not vary the colour it is supposed to drive. Each of those is recorded
against its scene in [gallery.md](../gallery.md) rather than summarised into a
single "parity gap", because they are four different defects and will be fixed
by four different changes.

---

## 2026-09-06 -- Five defects behind one flat brown quad

The gallery's texture failures looked like one problem and were five, each
hiding the next. They are recorded together because the order they came out in
is the useful part: every fix made the following defect visible, and none of
them was findable from the code alone.

**1. A placeholder where a default belonged.** An image node with no `file` was
treated as a texture that failed to load and bound the magenta placeholder. But
an image node with no file is legal and common -- the StandardShaderBall
authors one and expects a stronger opinion to fill it in, which for the base
material never arrives -- and MaterialX defines such a node as returning its
`default`, which is zero for every `ND_image_*`. The pool now binds a
one-pixel black texture for an empty path and keeps the magenta for a file that
is named and cannot be read. The shader ball's inner shell stopped being
magenta and started being what the asset asks for.

**2. Refinement dropped the texture coordinates.** The subdivision work
correctly stopped consulting authored UVs on a refined mesh -- they describe
the control cage -- but nothing refined them, so a subdivided mesh reached the
GPU with none at all. The shading kernel then fell back to barycentrics, which
vary per triangle: across thousands of tiny triangles a texture is sampled at
effectively random coordinates and averages to a flat colour. That is exactly
what the height-map scene rendered. UVs are now refined through the same
`PrimvarRefiner` weights as the positions.

**3. `geompropvalue` is the other way to ask for UVs.** The generated setter
recognised `texcoord` and assigned everything else its type's zero. A material
reading `st` through a `geompropvalue` node -- which is what the height-map
material does, and what plenty of assets do -- got a zero coordinate and sampled
one texel for the whole surface.

**4. The names it matched were the wrong spelling.** Vertex-data variables reach
the setter either substituted (`i_geomprop_st`) or as the token MaterialX stores
them under (`$inGeomprop_st`), and those differ in case. Matching one spelling
silently dropped the other; the comparison is now case-insensitive, which is
what made the fix above actually take effect.

**5. Every texture was upside down.** `HioImage::Read` was asked for the file's
own row order, and the file's order is top row first, while the renderer's
sampler reads row 0 as v = 0 -- the bottom, which is where USD and MaterialX put
it. Nobody had noticed because a noise map, a roughness map, or a height map
looks equally plausible flipped. It took a backdrop with printed numbers on it
to see, and the numbers had been mirrored in every gallery render.

`TextureImage` now documents the convention it always needed, and a render test
samples a four-quadrant image and asserts which corner each colour lands in. It
is the assertion whose absence let this survive.

---

## 2026-09-06 -- The device loss was one misaligned address

Writing that four-quadrant test found something much larger. With the corners
finally correct, the test binary lost the Vulkan device -- and with
synchronisation validation on, the layer named the cause outright:

    vkCmdBuildAccelerationStructuresKHR(): pInfos[0].scratchData.deviceAddress
    must be aligned to minAccelerationStructureScratchOffsetAlignment (128)

`minAccelerationStructureScratchOffsetAlignment` is a requirement on the build,
not on the buffer. An allocator that satisfies the scratch buffer's own
alignment -- which is far weaker -- still hands back addresses that fail it, so
whether a build was legal came down to where the allocator happened to put the
buffer. The driver's response to a misaligned scratch address is to lose the
device.

That is the KitchenSet failure from the first gallery pass, and it explains its
shape: a scene that lost the device inside `SetScene`, before a ray was traced,
with nothing about the scene to blame. The alignment is now queried into
`VulkanCapabilities`, and every scratch allocation over-allocates by one
alignment and rounds its address up. **Pixar's KitchenSet renders**, in 124
seconds.

Two things made this findable. The test that changed the acceleration-structure
reuse pattern, and validation being on by default in the test binaries -- the
message that named the VUID took a minute to act on, and the same defect had
been an unexplained device loss for a whole gallery pass before that.

---

## 2026-09-06 -- Reuse is keyed on what the structure owns

The four-quadrant texture also came out wrong in a way the geometry explains: a
prototype's acceleration structure was being reused for a prototype with
different texture coordinates.

`MeshPrototype::Fingerprint` hashed positions, indices, and the opacity class --
everything the *build* depends on. But `BottomLevelStructure` owns the normal
and UV buffers too, and hands their addresses to the shading kernel through the
instance table. Two prototypes with the same corners and different UVs therefore
shared one set of coordinates, and a mesh that gained UVs kept having none.

The fingerprint now covers normals, texture coordinates, and per-triangle
materials as well. The rule it should have followed from the start: a
structure's identity is everything the structure *owns*, not everything the
build reads.

---

## 2026-09-06 -- Black glass was the reflection closure answering a refraction

The glass shader ball rendered opaque black. Raising the path length to 32
bounces changed nothing, which ruled out the obvious explanation and pointed at
the estimator rather than at depth.

The scatter step samples a direction and then evaluates the closure at it to get
the response and the density. It evaluated with `CLOSURE_TYPE_REFLECTION`
always. A refraction crosses the surface, so the reflection branch was being
asked about a direction below its own horizon: zero response, zero density, and
the path terminated as impossible. Every transmissive material was therefore
black, and the closure implementations -- which have a working transmission
branch, validated on the GPU -- were never reached from the integrator.

The kernel now picks the closure from which side the sampled direction left on:

    dot(L, N) * dot(V, N) > 0  ->  reflection, otherwise transmission

Two things this says beyond the fix. The closure validation tests pass because
they exercise the closures directly, and the render tests passed because none
of their materials transmits -- a gap between two green suites is where this
lived. And the first gallery pass is what found it: the glass ball is in the
gallery precisely because transmission is hard, and a scene whose whole purpose
is one feature is worth more than the assertion it replaces.

---

## 2026-09-06 -- The playground: three refusals, three different answers

The OpenPBR playground failed on three unrelated things, and what is worth
recording is that each wanted a different kind of answer rather than the same
kind of leniency.

**A derivative that measures nothing is zero, not an error.** `mx_aastep`
computes its filter width with `dFdx`, and a compute stage rejects the builtin.
The standing rule -- screen-space derivatives are meaningless here, because
neighbouring lanes are unrelated paths -- was previously enforced by letting the
compile fail. That is right for `mx_subsurface_scattering_approx`, whose whole
body is a screen-space approximation of transport hdClaude intends to do
properly, and wrong for `mx_aastep`, whose answer in a path tracer is simply
zero: it antialiases by sampling the pixel. The generator now defines `dFdx`,
`dFdy` and `fwidth` as zero in the material preamble, and the subsurface
function stays *deleted*, so that one still fails loudly by name.

**An input the linked MaterialX does not have is dropped, not fatal.** The
playground authors `geometry_opacity` on `open_pbr_surface` as a colour, where
1.39.3 declares a float. MaterialX then cannot resolve the node at all, and one
input cost the whole material -- and, since hdClaude reports a material it
cannot compile rather than approximating it, the whole scene. Inputs whose name
or type the declaration does not have are now removed, with a warning naming
each. The policy of refusing to approximate a material is about *shading*, not
about tolerating a version difference in an asset.

**A `<UDIM>` token expands to tile 1001, and says so.** Eighty-five textures in
this scene are UDIM sets, and an unexpanded token opens nothing, so the first
render of the playground was magenta from edge to edge. Real tile selection --
one texture per tile, chosen by which unit square of UV space a sample lands in
-- is phase 7 work. Loading tile 1001 and shading every tile with it is wrong
for a multi-tile asset and right for the many that ship one tile, and the
comment says which it is. Failures fell from 85 to 21.

The 21 that remain are TIFFs, which this OpenUSD distribution's Hio has no
plugin for. That is an environment limitation and the magenta placeholder is
reporting it correctly -- which is the first time in this gallery that the
placeholder has meant what it says.

---

## 2026-09-06 -- A primvar reader, and the substring that looked like a match

The last blocked gallery scene needed two things, one expected and one that had
been quietly wrong since the UV work earlier the same day.

**`UsdPrimvarReader` is a `geompropvalue` behind an interface.** MaterialX
implements `ND_UsdPrimvarReader_*` as a nodegraph wrapping a `geompropvalue`
whose `geomprop` input is connected to the graph's `varname` interface. The
GLSL implementation reads that input's *value* to know which primvar to
declare, and an interface connection is not a value, so generation stops with
"No 'geomprop' parameter found on geompropvalue node 'primvar'". The node is
now rewritten in place -- category, nodedef, `varname` to `geomprop`,
`fallback` to `default` -- which leaves every connection into and out of it
untouched. Rewriting beats replacing precisely because of those connections.

**`geomprop_strand_u` contains `geomprop_st`.** The UV mapping added earlier
matched the variable name by substring, so a curve's float parameter
`strand_u` was recognised as the texture coordinate and assigned a `vec2`. The
material stopped compiling, which is the good outcome; the same test would have
silently assigned a `vec2` geomprop named `stuff` had one existed. The
comparison is now exact against the name after the `geomprop_` prefix, and it
requires a two-component port.

Worth keeping: the first defect was found by an asset and the second by the
first defect's fix. A substring test on a name looks equivalent to an exact one
right up until a name has a prefix in common with another, and the names here
come from assets rather than from this codebase.

All ten gallery scenes now render.

---

## 2026-09-06 -- Parity, and what closing the gap to hdCodex actually needed

Intel Sponza was the widest gap in the gallery: hdCodex renders a bright,
legible atrium and hdClaude rendered a near-black rectangle. Three separate
causes, and only the last is a matter of taste.

**A USD-native material is a MaterialX material.** Sponza's 137 materials are
`UsdPreviewSurface` networks, and hdClaude refused them -- "not a MaterialX
surface" -- and shaded `displayColor` instead. That refusal was reasoned from
the right rule and applied to the wrong case: the rule forbids hdClaude
*extracting* parameters from a named surface model, and MaterialX itself
declares `ND_UsdPreviewSurface_surfaceshader` and implements it as a nodegraph
of ordinary nodes. Refusing it left MaterialX's own translation unused. Sponza
went from 0 textures to 25 and from grey to shaded.

Two adaptations were needed to hand the network over.
`HdMtlxCreateMtlxDocumentFromHdNetwork` looks a node's type up as a MaterialX
nodedef name, which a USD-native network does not carry, so `UsdPreviewSurface`
and friends are renamed to their `ND_` equivalents first -- a rename, not a
translation, since MaterialX declares the same inputs under the same names. And
USD spells a wrap mode `repeat` where MaterialX's enum says `periodic`; a value
outside the enum stops generation for the whole material, which for a textured
asset is all of them.

**The environment was not a light.** hdClaude sampled its analytic lights by
next-event estimation and left the sky to be found by a scattered ray that
happened to escape. In an enclosed set almost none do, so an interior lit only
by sky was dark and full of fireflies -- it was gathering the sky through
chains of bounces instead of one shadow ray. The environment is now one more
emitter in the same uniform selection, sampled uniformly over the sphere, and
weighted against BSDF sampling by the balance heuristic. The density is uniform
rather than cosine-weighted for a specific reason: the environment kernel has
to recompute it for a scattered ray from a *direction alone*, with no surface
to hand, and an MIS weight that cannot be computed on both sides is not an MIS
weight.

The analytic lights are deliberately not MIS-weighted. They are absent from the
acceleration structure, nothing can hit them, and weighting them against a
strategy that cannot reach them would discard the half of their contribution
that has nothing to make it up.

**The stand-in sky was a tenth of what it needed to be.** Its documented
purpose is that a stage with no `UsdLux` prim renders as a lit room with a
lighting gap rather than as a silhouette that could equally be a shading bug.
At 0.05 it failed that on the first real lightless asset. It is now 0.30, which
lights an interior; a dome light replaces it entirely, so no scene that authors
its own lighting moves.

Sponza is still darker than hdCodex's baseline, and that is now the honest
answer rather than a defect: hdCodex shades every one of those materials as
flat grey `displayColor`, and hdClaude shades the brick, stone and fabric the
asset actually authored. Parity with a less correct image is not the goal.

---

## 2026-09-06 -- One shadow ray per bounce is an invariant, not a habit

Adding the environment as an emitter, I left the stand-in sun where it was: an
`if (lightCount == 0)` block that sampled it *in addition*. A lightless stage
therefore emitted two shadow rays per path per bounce, and both of the
structures underneath assume exactly one.

The shadow kernel adds an unoccluded contribution with
`pathRadiance[ray.path] += ray.contribution` and no atomic, which is safe only
because no two invocations name the same path. The shadow queue is sized at one
entry per path, and the overflow guard silently drops what does not fit. So the
second emitter did not brighten the image: it raced the first and threw away
whatever the queue could not hold.

The sun is now one option in the same uniform selection as the analytic lights
and the environment -- a delta emitter whose density is one, so the estimator
divides by the selection probability alone. The invariant is restored by
construction rather than by remembering it.

Worth keeping: the comment stating the invariant was two files away from the
code that broke it, and it was accurate the whole time. What would have caught
this is an assertion that `shadowCount` never exceeds the path count, which the
GPU can check and the host currently never reads.

---

## 2026-09-06 -- The missing TIFF decoder that was neither missing nor a decoder

Twenty-one of the OpenPBR playground's textures failed to open, all of them
`.tif`, and the conclusion wrote itself: this OpenUSD distribution has no TIFF
plugin, and hdClaude would need OpenImageIO. Both halves were wrong.

The distribution ships `hioOiio` -- the OpenImageIO plugin -- registered for
`tif`, `tiff`, `zfile` and `tx`, with `OpenImageIO.dll` and `tiff.dll` beside
it. Hio was reading those files perfectly well. The failures were hdClaude's,
in two places, and the file extension they had in common was a coincidence.

**A UDIM set does not necessarily start at 1001.** The `<UDIM>` expansion added
earlier assumed it did. The playground's tools are authored on tile 1003, so
every one of their textures resolved to a path with no file at it. The tile is
now found by asking the resolver, walking the 10x10 grid the UDIM convention
defines and taking the first that exists.

**Sixteen bits is not an exotic format.** `Widen` accepted unsigned byte, half
and float, and refused everything else with "unsupported component type" --
which included the `HioTypeUnsignedShort` a paint package writes a mask or a
height map as. Three textures failed on that, and the message was as unhelpful
as it sounds. Every component type Hio can produce is now converted through one
function, which is also the place to change when the texture pool carries more
than RGBA8.

The playground now loads all 100 of its textures.

The lesson is about the shape of the evidence, not about images. Twenty-one
failures that shared a file extension and *no other property* looked like one
cause, and the extension was the one thing that had nothing to do with it. What
would have separated them sooner is the error text: "no image plugin opened"
and "unsupported component type" are different failures, and they were being
read as one because they arrived in the same list.

---

## 2026-09-06 -- Face-varying UVs, and a "no result" that was the result

The StandardShaderBall's ground was mapped with its own barycentrics: the grid
ran diagonally, the printed numbers were rotated, and the region of the texture
on screen was not the one hdCodex shows. So were all five walls of its box.

The cause is one line of the asset:

    texCoord2f[] primvars:st = [(0,0), (1,-1), (1,0), (1,-1), (0,0), (0,-1)]
        (interpolation = "faceVarying")

Six coordinates on a four-vertex quad. hdClaude accepted `st` only when its
length equalled the vertex count, which is a test that rejects every
face-varying primvar there is -- and a UV seam is *authored* face-varying, so
this is not an edge case. The interpolation is now read from the prim's own
primvar descriptors rather than inferred from an array's length, and a
face-varying set is triangulated into one coordinate per triangle corner, which
the kernel indexes by primitive.

The part worth remembering is the second failure, which cost more than the
first. `HdMeshUtil::ComputeTriangulatedFaceVaryingPrimvar` returns a
three-valued result: `Error`, `Success`, and `Unchanged` -- "computation
succeeded but no result was produced, because it is the same as the input".
The ground is already two triangles, so triangulating its face-varying array is
a no-op and the return is `Unchanged` with the output VtValue left empty.
Testing for `Success` alone reads that as a failure, and an all-triangle mesh
ends up with no coordinates at all -- which is the exact state the fix was
meant to cure, arrived at by a different route.

A tri-state result where two of the three states mean success is worth reading
carefully. The name says so; the shape of the code did not.

---

## 2026-09-06 -- An rprim learns its instancer only if it asks

The OpenChessSet rendered sixteen named pieces and a single pawn sitting in the
middle of the board. The pawns are a `PointInstancer` with eight instances per
side, and hdClaude was drawing the prototype once, at its own transform.

Two things were wrong, one behind the other.

**`HdRprim::GetInstancerTransforms` is not the per-instance list.** It returns
one matrix per instancer in the parent chain -- each instancer's *own*
transform. The mesh adapter read it as the placements and its comment said so,
confidently. Computing instance transforms is the renderer's job:
`HdInstancer` holds the primvars and computes nothing, which is why every
render delegate ships an instancer of its own. hdClaude now has
`HdClaudeInstancer`, composing instancer transform, translate, rotate, scale
and per-instance matrix, and multiplying through nested instancers.

**And an rprim's instancer id starts empty.** `GetInstancerId()` is filled in
only when the rprim calls `_UpdateInstancer` during its own Sync, and the
instancer is created and synced only when something asks for it through
`HdInstancer::_SyncInstancerAndParents`. Without those two calls the id stays
empty, `CreateInstancer` is never called at all -- which is what the trace
showed, and what sent this investigation looking at plugin metadata and
supported prim types before looking at the rprim.

hdEmbree renders the same stage with all thirty-two pieces, which is what said
the pipeline was fine and the delegate was not. Comparing against another
delegate on the same stage is the cheapest instrument in the box.

---

## 2026-09-06 -- The dome was half a turn out

With the chess set's pieces finally all present, its background was still the
wrong part of the room: hdCodex shows a window and flowers, hdClaude showed
plaster medallions and a curtain. Same stage, same HDRI, same camera.

hdClaude wrapped the latitude-longitude lookup about -Z:

    u = atan(d.x, -d.z) / 2pi + 0.5

USD wraps it about +X, which is what `hdSt/shaders/domeLight.glslfx` samples
with and therefore the orientation an authored HDRI is framed against:

    u = (atan(d.z, d.x) + pi/2) / 2pi

The two differ by exactly half a turn, so every dome-lit scene was lit and
backed by the half of the environment behind the camera. It is a difference no
furnace test can see -- a constant environment is rotationally symmetric, and
hdClaude's own tests use one -- and it takes a *recognisable* environment to
notice. That is an argument for the gallery having a scene with a real HDRI in
it, which the chess set is.

Worth stating for the next convention like this: the right way to settle one is
to read the sampling code of the renderer that defines it. USD's dome light
orientation is not written down anywhere as prose; it is written down in the
shader that hdStorm uses.

---

## 2026-09-06 -- A fallback that stopped being one, and a seam that never survived

Two observations from looking at the images, both correct, both mine.

**The stand-in sky was lighting scenes that light themselves.** It only tinted
rays that escaped until the environment became an emitter sampled by next-event
estimation. From that commit onwards it was a fill light on every stage,
including the StandardShaderBall's five lights and Collective Project's three
-- a "default light" nobody authored. A stand-in is for a stage that supplies
no lighting; one that authors lights and no dome has no environment, and a ray
that leaves it sees nothing. The scene store now clears it in that case.

Worth naming the shape of this: making something an emitter changed what
"a default value" meant, and the default had been chosen when it could only
tint a background. A fallback's value is only safe while the thing it falls
back to cannot light anything.

**Face-varying UVs did not survive refinement.** Collective Project's character
looked untextured -- flat orange where hdCodex renders orange with yellow-green
limbs -- because its body is subdivided and authors `st` face-varying, and the
refinement carried only vertex-interpolated coordinates. Dropping them left the
mesh with none, so its colour map sampled one texel.

They are now refined through an OpenSubdiv face-varying *channel*, which is the
only way a seam survives: interpolating those coordinates as vertex data welds
the seam shut and smears the texture across it. The refined face's corners
index the channel rather than the vertices, and the triangle fan reads them the
same way it reads the vertex indices.

**And a third, found while reading the generated setter for the second:**
`bitangentWorld` was assigned `T`. The vertex-data mapping tested
`find("tangentworld")` before `find("bitangentworld")`, and the first is a
substring of the second, so the looser match won. Every anisotropic closure and
every normal map was working from a degenerate frame. This is the third
substring bug in a day -- `geomprop_strand_u` matched `geomprop_st`, and now
this -- which is enough to call it a pattern rather than an accident.

---

## 2026-09-06 -- The normals were fine; the frame they were read in was not

The chess set's pieces were faceted and its board was covered in a fine
herringbone; the OpenPBR Playground was a blizzard of fireflies on every
surface. Both look like a normals problem, and the obvious suspects were both
wrong: the chess set authors no normals at all and gets generated ones, and the
playground's authored normals belong to control cages that are refined away. The
generated normals were correct. What was wrong was the **tangent frame** they
were handed to the material in, and how the maps that perturb them were decoded.

**A tangent is a property of the parameterisation, not of the triangle.**
`hdclaude_reconstruct` built its tangent from the first edge of the triangle,
orthogonalised against the shading normal. That is a perfectly good unit vector
in the tangent plane, and it is the wrong one. A tangent-space normal map is
defined against the *texture's* axes -- its x perturbs the surface along
increasing `u`, its y along increasing `v` -- so a frame taken from an edge
applies every map at a rotation that changes from triangle to triangle, and the
two triangles of a quad disagree by roughly ninety degrees. That is what the
herringbone was: a normal map resolved into per-triangle noise. Every chess
material and 132 of the playground's nodes are `<normalmap>`.

The frame now comes from solving dP/du and dP/dv out of how position and
coordinate vary together across the triangle, with the tangent orthogonalised
against the shading normal -- not the geometric one, because the shading normal
is the third axis MaterialX's `normalmap` builds its frame from. A mesh with no
coordinates falls out of the same arithmetic: the corner defaults (0,0), (1,0)
and (0,1) *are* the barycentric parameterisation, so it needs no branch.

**The bitangent's sign is data, not a cross product.** The generated geometry
setter had been assigning `cross(N, T)`. A mirrored UV island -- how the second
half of a symmetric asset is normally laid out, both chess pieces included --
runs `v` the other way round, and a fixed cross product inverts every mapped
detail on exactly those islands. The kernel now reads the handedness off dP/dv
and passes the bitangent through the setter, so the generator no longer derives
a value it has no way to know.

**An 8-bit JPEG holding a normal map is not sRGB.** Textures were being marked
sRGB from `HioImage::IsColorSpaceSRGB`, which answers from the file format
alone: three 8-bit channels means sRGB to it. That is right for a colour map and
wrong for every data map shipped in the same container, and the chess set's
normal, roughness and metalness maps are all 8-bit JPEGs. Hardware-decoding a
normal map turns its flat (0.5, 0.5, 1) into roughly (0.21, 0.21, 1), so every
surface acquires a constant tilt and every bump is exaggerated -- which is where
the fireflies came from.

Only the material knows what an image means, and it says so three different ways
depending on who exported it: a `colorspace` attribute on the `file` input,
which is what a MaterialX document authors; a `sourceColorSpace` input, which is
what a `UsdUVTexture` authors; or nothing at all, in which case the node's
*type* is the answer -- an `image` returning `color3` is colour, one returning
`float` or `vector3` is data. The chess set relies on the third. All three are
now read, and the decision travels with the asset path to the texture pool,
which keys its cache on the pair: the same image can legitimately be sampled as
both.

Note what the document-level `colorspace="lin_rec709"` on `<materialx>` is *not*:
it is the working space, not the file's encoding. Reading it as the latter --
which `getActiveColorSpace` would, since it inherits -- calls every texture in
the document linear.

**And the authored normals hdClaude could not see.** Separately, and matching the
user's own guess: normals were read with a bare `Get(id, HdTokens->normals)` and
accepted only when the array happened to be one per vertex. USD spells authored
normals two ways -- the `normals` attribute and a `primvars:normals` primvar --
and Hydra presents both as a primvar named `normals`, so that part worked; but
asking `Get` says nothing about how the array is *indexed*, and a face-varying
or uniform set was silently dropped. Those are not an exotic variant. They are
how a hard edge is authored, since the two sides of a crease need different
normals at the same vertex, and 195 of the playground's meshes author exactly
that. They now go through the same triangulation the face-varying UVs do, and
the prototype carries a `normalsPerCorner` flag the kernel indexes by primitive.

**The general lesson**, and it is the same one the film flip and the pre-flipped
glass normals taught: all three defects were in a *convention* -- which axes a
frame is built from, which sign a bitangent has, what a byte in a texture means
-- and every one of them produced a plausible image. A wrong tangent is still
unit length and still orthogonal to the normal; an sRGB-decoded normal map still
normalises. Nothing here is catchable by a test that checks magnitudes, which is
why the two new render tests assert on *direction*: a material whose albedo is
its own tangent must come back green on a quad whose `u` runs along world +Y,
and one whose albedo is its own normal must show the two triangles of a quad
differing when their normals are authored per corner.

---

## 2026-09-06 -- An instanced rprim's transform is not its world transform

The Kitchen Set was missing its refrigerator, its stove, its table and its
chairs, while the props that were visible floated in the air near the window.
Reported by comparing against hdCodex's baseline, which has all of them.

Nothing was missing. The trace said 1462 prototypes and 1788 instances, and
walking the stage with `Usd.TraverseInstanceProxies` says the stage holds
exactly 1788 mesh instances. Every prim was published, with the right count, and
put in the wrong place.

`Kitchen_set_instanced.usd` is native USD instancing, so every rprim on this
stage lives inside a prototype -- the paths are all
`/UsdNiPropagatedPrototypes/.../UsdNiInstancer/UsdNiPrototype/Geom/...`. For
such an rprim there are *two* transforms and both are needed: the rprim's own,
which places the mesh within the prototype, and the instancer's, which places
the prototype in the world. The mesh adapter used the instancer's alone.

That collapses every mesh of a model onto its model's origin. 1460 of the
Kitchen Set's 1462 meshes carry a non-identity transform inside their
prototype, several with translations of a couple of hundred units, so the parts
of each model scattered around the room -- a cup where its table's root is, a
refrigerator's panels somewhere outside the frame or inside a wall. The failure
therefore reads as *missing geometry*, not as misplaced geometry, which is why
it survived a look at the image.

The composition is `mesh * instance`: USD composes a row vector's transforms
left to right, so the mesh goes into the prototype and the prototype into the
world. hdEmbree writes the same thing as `_transform * transforms[i]`, and
hdClaude's own instancer already composed in that direction -- a translation
primvar is applied as `step * transform` and a nested instancer as
`inner * outer`. The adapter was the one place that did not.

**Why nothing caught it.** The render tests build a `Scene` directly, so they
never reach the mesh adapter; there is no Hydra-linked test target at all. And
the gallery gate compares each render against its *committed baseline*, so it
catches a change and cannot catch a baseline that was wrong when it was
committed. This one was: the kitchen has looked like this since it was first
adopted. A gate built on self-comparison has that blind spot by construction,
and the only thing that closed it here was a second renderer's image of the
same stage.

That is an argument for the "Against hdCodex" table earning its place, and for
a renderer-owned scene whose correct appearance is checkable by construction --
a prototype whose meshes are deliberately offset from its root, instanced
several times, so that dropping either transform is unmistakable. Recorded in
docs/roadmap.md rather than built here.

---

## 2026-09-06 -- An HDRI is only a light above 1.0, and only if you can find it

Two changes that make no sense apart, and one measurement that only exists
because they landed together.

**Every texture was quantised to eight bits.** `TextureImage` held RGBA8 and the
loader clamped whatever it decoded into it. For an integer source that is a
precision loss and a fair trade -- the comment in the loader said as much. For a
*float* source it is not a trade at all. An HDRI's contribution as a light is
almost entirely the part of it above 1.0: the window, the lamp, the sun. Clamp
that and a dome light becomes a flat grey sky that casts no shadow worth the
name. The chess set has been lit by a clamped 4k HDRI since it was adopted.

Float and half sources are now kept in `R16G16B16A16_SFLOAT`, which is a
mandatory sampled format with linear filtering, so this needs no capability
query. Integer sources are unchanged: quantising them is still the trade it
always was, and the line between the two cases is the one that matters --
quantisation loses precision, clamping loses data.

**Which immediately made the image worse.** Uniform sphere sampling of a real
HDRI is a firefly generator by construction: the light is concentrated in a
fraction of a percent of the sphere and is hundreds of times brighter than
everything around it, so one shadow ray in a thousand finds it and carries a
thousand times the radiance it should. The first render with range restored was
brighter, correct in the mean, and covered in white specks.

So the environment now has a piecewise-constant distribution over its own
latitude-longitude parameterisation -- a marginal CDF over rows, a conditional
CDF per row, and the density those sample from. Each texel is weighted by
luminance times sin(theta), because a row near a pole covers far less solid
angle than one at the equator and luminance alone would spend most of the
samples on the two points where the map is most oversampled.

**The constraint that shaped it.** The density has to be evaluable *from a
direction alone*. The kernel that weighs a scattered ray against this strategy
has a miss and a direction and no surface in hand, so a density it cannot
recompute there is not a density multiple importance sampling can use. That is
why the density is stored rather than reconstructed from the CDFs, and why the
sampler's (u, v) -> direction map is written as the explicit inverse of the
lookup: the two are only ever right together, and a sampler that walks the map
in one parameterisation with a density evaluated in another agrees nowhere.

The distribution is built at most 1024x512, box-averaged down from whatever the
map is. Averaging rather than point-sampling is load-bearing: it keeps the
density nonzero wherever the map is, which is the property MIS needs from it. A
4k map's own resolution would cost 67 MB of CDF and density to resolve an edge
that sampling does not care about.

**What the tests had to be.** A furnace under a *uniform* dome is not the
trivial case it looks: a distribution over (u, v) describing a uniform sky is
proportional to sin(theta), so a missing Jacobian shows up there and not only in
a peaky map. It renders 0.818 against a closed form of 0.800.

But a uniform sky integrates to the same number under any rotation, so a second
test lights only `u < 0.5` -- the half-space x > 0 in the dome's frame -- and
asks a quad facing +Z what it receives. The answer is exactly half the furnace,
because the cosine weight is symmetric in x; a reconstruction rotated a quarter
turn would answer 0.8 or 0.0 instead. It renders 0.402 against 0.400.

**And a real lesson from getting that test wrong first.** It was written with a
bounce limit of one and rendered 0.11 instead of 0.40. Nothing was wrong with
the estimator: at one bounce the scattered ray is retired before it reaches the
environment kernel, so only the next-event half of the multiple-importance
estimate is ever added, and each contribution keeps just its MIS weight. Both
halves of an MIS estimate have to actually run. A bounce limit that stops one of
them does not make the image noisier -- it makes it *dark*, by a factor that
looks exactly like a missing light.

---

## 2026-09-07 -- RGB to spectrum, and the white that would not come back

The first piece of hero-wavelength transport, and the one everything else waits
on: an asset authors RGB, the integrator has to carry a spectrum, and the
conversion between them has to be exact enough that a spectral renderer does not
render every material a slightly different colour than an RGB one for no reason
a user could act on. That is the phase 1 exit gate -- upsample, integrate back,
and land within 1e-3 dE2000 -- and it is now met at 4.7e-5, worst case.

**Reflectance** uses the Jakob-Hanika model the design called for:
`S(lambda) = sigmoid(c0 t^2 + c1 t + c2)`. The reason it is a sigmoid of a
polynomial rather than a basis expansion is the one property that matters for a
reflectance: the sigmoid's range is (0, 1) for *any* coefficients at all, so no
fit, however badly conditioned, can produce a spectrum that reflects more light
than arrives. An upsampled albedo cannot create energy by construction rather
than by validation.

**Emission** cannot use it directly -- a light of RGB (5, 5, 5) integrates to
five times white, which nothing bounded expresses -- so the chromaticity is
fitted as a reflectance and the magnitude carried beside it. Chromaticity exact,
magnitude exact, non-negative everywhere, unbounded.

**The fit runs in double, and that is not an optimisation.** Levenberg-Marquardt
on three coefficients with a numerical Jacobian, with the residual measured in
CIELab because the acceptance criterion is a colour difference and a
least-squares fit in XYZ spends its accuracy where the eye does not look. In
float it stalled at 5e-2 dE on white: reaching a reflectance of 1 - 3e-6 needs a
coefficient around 300, where the derivative of the spectrum with respect to a
coefficient is about 1e-8, which is below the noise floor of the spectral
integral itself. The Jacobian was rounding error and the optimiser had no
gradient to follow. The routine runs once per colour and is cached, so precision
is free here in a way it never is in a kernel.

**And it is seeded by bisection**, because even in double, Levenberg-Marquardt
cannot climb an asymptote: a reflectance of exactly one needs an infinite
coefficient, every step improves by less than the last, the damping escalates
and it gives up. Bisection on the constant term against the target's luminance
has no such trouble -- it never needs a gradient -- and it leaves the optimiser
only the hue to solve, which is the part it is good at.

**The last five hundredths of a dE were not the fit at all.** With the seed in
place the fit returned `S = 1` exactly for white, and the round trip still came
back (0.9994, 1.0002, 0.99995). Both halves of the integral are approximations:
the colour matching functions are Wyman's multi-lobe fit, good to about a per
cent of peak, and the illuminant is a published table sampled at 5 nm. Their
product's white point lands a fraction of a per cent from the D65 the sRGB
matrix was derived against.

That is not a rounding detail to tolerate. It means **a perfect white diffuse
surface under the scene's own illuminant does not render white** -- and nothing
can hide it, because a reflectance of one is the model's boundary and there is
nothing left to trade. Every other colour absorbs the same error into its fit
and looks fine, which is precisely what makes it worth pinning: the one colour
that cannot compensate is the one a viewer would notice. The integral is now
adapted so a perfect reflector produces the colour space's white by
construction.

The general lesson is the same one the tangent frame taught, in a different
key: an error that every degree of freedom can absorb is invisible until you
reach a case with no degrees of freedom left. White was that case here. It is
worth keeping a boundary value in every test set for exactly this reason.

**What is not done.** This is the conversion, not the transport. Path throughput
and radiance are still `vec3`, `hdclaude_wavelengths` is still a constant the
kernels assign and nothing reads, the film still accumulates RGB, and colour
temperature is still applied as an RGB tint. Those are the next steps, and the
documents that describe hdClaude as transporting spectrally today still overstate
what it does.

---

## 2026-09-07 -- Transport becomes spectral, and the tests find out what that costs

A path now carries four wavelengths and four scalars along them. Throughput and
radiance are `vec4` lanes rather than RGB channels, the packet is drawn once at
ray generation and held for the path's life, and RGB survives in exactly two
places: where an asset authors one, and where the film hands an image back.

**Where the conversion happens, and where it does not.** The MaterialX graph
still computes in RGB -- its generated code is `vec3` throughout, and changing
that means changing the code generator's type system -- so the upsampling
happens at the *closure boundary*: the response a closure returns and the
radiance a light carries are upsampled to the lanes, not each texel inside the
graph. A material that multiplies two textures still multiplies them as RGB. The
transport between surfaces is spectral; the arithmetic within a material is not,
and `docs/spectral-rendering.md` 3 describes an end state this is a step toward
rather than the current one.

**Reflectance and emission upsample differently, and getting that wrong renders
a scene nobody authored.** A closure's response is a reflectance and becomes the
bare spectrum. A light's colour is an emission and becomes that spectrum *times
the illuminant its RGB was authored against* -- a white light emits D65, which
is what an RGB emitter means in a D65-referred pipeline. Multiply two
reflectances together instead and every lit surface is rendered under an
equal-energy sky, which is a colour cast with no author.

The film then divides by the same illuminant's luminous integral, which is what
closes the loop: a white surface under a white light resolves to white rather
than to whatever the illuminant's absolute power happens to be.

**The tables are sampled, not re-derived.** The colour matching functions and
D65 already exist on the host, where they are the definitions the upsampling fit
and its round-trip gate are written against. Giving the shader a second closed
form would be a second thing to keep in agreement with them, and a disagreement
would show up as every material being a slightly different colour than its own
unit test says. So they are sampled at 5 nm, uploaded, and interpolated -- along
with the chromaticity table, in the same buffer.

**What the tests had to learn.** Every furnace came back with a magenta cast on
the first run -- 0.836, 0.761, 0.839 against 0.800 -- which looked exactly like a
bad white point. It was not. It was chromatic noise: a pixel's colour now comes
from four wavelengths drawn at random, so a grey surface renders a slightly
different grey in every pixel and is neutral only in the mean. At sixteen times
the samples the same test read 0.807, 0.793, 0.780; averaged over a window
instead, at the original sample count, it reads 0.804, 0.801, 0.804.

That is a real and permanent cost of spectral rendering, not a defect: RGB
transport has *zero* chromatic noise on a grey surface, and there is no
arrangement of a spectral renderer that also has none. Worth knowing before
reading the gallery, where every image is now slightly noisier in colour at the
same sample count.

**And one test had to be rebuilt rather than loosened.** The per-material sort
test identified a background pixel by its colour being the environment constant
exactly. Under spectral transport a background pixel is a four-wavelength
*estimate* of the environment's spectrum, so it lands near the authored colour
and never on it, and every pixel looked shaded. The fix is not a tolerance: the
sampler is seeded from the pixel, the sample index and the bounce and from
nothing else, so a pixel that only ever saw the environment draws the same
packet whatever the scene contains. Rendering the same frame with the geometry
moved behind the camera gives a background that a background pixel matches
*bit for bit*, and a shaded pixel cannot. It still counts 1169 + 1167 = 2336,
the same numbers as before.

The general lesson is one this project keeps relearning from the other side: a
test that identifies something by an exact value is testing the estimator's
determinism as much as the thing it names, and changing the estimator breaks it.
The repair is usually to find what the two cases actually differ by, which here
was better than what it replaced.


---

## 2026-09-07 -- A colour temperature is a spectrum, and an RGB tint is a metamer of it

`enableColorTemperature` was the last input still resolved to a colour on the
host, and it is the one where doing so is least defensible. It is now carried to
the GPU as a temperature in kelvin and evaluated as Planck's law along the four
hero wavelengths.

**Why the tint was never equivalent.** Multiplying a light's colour by the
blackbody's *RGB* gives a light that matches the blackbody under the colour
matching functions and differs from it everywhere else. That is the definition of
a metamer, and the difference is not academic: the two spectra light a surface
whose reflectance varies across the spectrum -- every real surface -- to
different colours. A renderer that transports four lanes precisely so that
metamers stop being interchangeable should not manufacture one at the light.

**The illuminant is replaced, not multiplied.** An emitter's RGB is referred to
some illuminant; by default D65, because that is what an RGB colour means in an
sRGB pipeline. A light with a temperature is referred to its *blackbody*
instead. The tempting alternative -- keep D65 and multiply by the blackbody --
says a 2700 K lamp emits daylight through an amber filter, which is a different
spectrum from a 2700 K lamp and looks like one.

**Equated on luminous integrals, not on peaks.** UsdLux's control is a colour
control: enabling it must not change how much light is emitted. The blackbody is
peak-normalised for numerical reasons, so it needs a scale, and the scale has to
come from the two illuminants' *luminous* integrals -- the spectrum against
`ybar`. Matching peaks instead would make every warm light a dim one, because a
2700 K blackbody peaks around 1070 nm, far outside the visible range, so almost
none of a peak-normalised 2700 K spectrum lands where the eye is.

**What pins it.** Two tests, because the two claims can fail independently. On
the host, the spectrum is integrated through the colour matching functions and
its chromaticity compared against the published Planckian locus at five
temperatures, landing within 0.0025 in x and y -- which checks Planck's law and
the colour matching integration *together*, since a wrong constant in either
moves the answer and neither is likely to move it back onto the locus five times
over. On the GPU, a lit quad's red-to-blue ratio goes 0.998 neutral, 10.011 at
2700 K, 0.673 at 9000 K, while its luminance stays 0.2550 in all three. A
peak-normalised blackbody passes the first of those and fails the second badly,
which is exactly why the luminance is asserted alongside the hue.

**A test that was true and proved nothing.** The GPU test first drove the quad
with a unit-radiance distant light of 0.01 rad angular radius. That is an
irradiance of 3.1e-4, so the quad rendered at a luminance of 1e-4 -- correct
physics, and a useless place to assert that two luminances agree to within five
per cent. The light is now scaled by the reciprocal of its own solid angle,
which puts the neutral render at 0.2550 and makes the agreement mean something.
An assertion that passes is not the same as an assertion that could have failed.

**What did not change, and the evidence for it.** No gallery scene enables the
control -- the chess board authors `enableColorTemperature = 0` -- so the
`kelvin = 0` path must be bit-for-bit what it was. Eight of the ten scenes
re-render to identical SHA-256 hashes; the two that do not, Collective Project
and the height map, also differ from each other between two consecutive runs of
this same build -- Collective Project at an RMS of 5.8e-5 then 7.0e-5 against
the committed baseline -- so what they show is the gallery's own run-to-run
nondeterminism and not a change in behaviour.


---

## 2026-09-07 -- Who is wrong, the scene or the toolchain

Intel Sponza renders with no normal, roughness or metalness maps. All 101 of
those inputs are dropped by `PruneUndeclaredInputs` because their types do not
match what MaterialX declares, and the obvious repair -- coerce the type and
carry on -- is the wrong one. This entry is what was found instead.

**The rule, and its limit.** hdClaude reports malformed input; it does not
repair it. A renderer that silently patches bad data hides the defect from the
person who can fix it, and makes its own image untrustworthy, because you can no
longer tell which pixels came from the asset and which from the workaround. The
limit of that rule is documentation: if a specification says two things are
interchangeable, then supporting the equivalence is correctness, not magic.

So the question was whether `color3` and `vector3` are documented as
interchangeable. They are not, and the specification says so in as many words --
MaterialX's Specification, on `<input>` elements: "Inputs may only be connected
to node/nodegraph outputs or nodedef interface inputs of the same type, though
it is permissible for a `string`-type output to be connected to a `filename`-type
input (but not the other way around)." One documented exception, and it is not
this one. Dropping the input with a warning is the spec-correct behaviour, and
it stays.

**Then the scene must be wrong. It mostly is not.** Sponza authors

```
normal3f inputs:normal.connect = </root/mtl/..._Normal.outputs:rgb>
float4   inputs:bias = (-1, -1, -1, 0)
float3   outputs:rgb
```

which is exactly what the UsdPreviewSurface and UsdUVTexture schemas require. Of
the 101 pruned inputs, 52 come from a scene that is authored correctly, and the
wrong type is manufactured downstream. Three separate upstream defects produce
them:

1. **`normal3f` is missing from hdMtlx's type table.** `_ConvertToMtlxType` in
   `pxr/imaging/hdMtlx/hdMtlx.cpp` maps `color3f` and `float3` but has no entry
   for `normal3f`, `vector3f` or `point3f`. An unmapped type returns the empty
   string, which reaches `setInputValue(name, value, "")`, and the input is
   typed **`string`**. That is the four materials that author
   `normal3f inputs:normal = (0, 0, 1)` as a literal.

2. **A connected input takes its type from the upstream output** (hdMtlx.cpp
   line 501), and MaterialX's own definitions disagree with each other about
   what that type is. In `libraries/bxdf/usd_preview_surface.mtlx`,
   `ND_UsdUVTexture` declares `<output name="rgb" type="color3"/>` while
   `ND_UsdPreviewSurface_surfaceshader` declares
   `<input name="normal" type="vector3"/>`. Connecting a texture's `rgb` to a
   surface's `normal` is what the USD specification prescribes for a normal map,
   and by MaterialX's own connection rule its own two nodedefs cannot express
   it. That is the 24.

3. **The same table maps USD `float4` to `vector4`** where that same nodedef
   declares `scale` and `bias` as `color4`. That is the 48.

**And 49 really are the scene.** Sponza connects `float inputs:roughness` and
`float inputs:metallic` to `outputs:rgb`, a three-component output. Three numbers
into a one-component input is a loss USD has no rule to resolve, and the fix is
one character in the asset: connect `outputs:r`. That is a genuine authoring bug,
and it is the kind this project now reports rather than guesses at.

**`tools/check_usd_materials.py`.** The distinction above is only cheap to draw
if something checks the scene *at the USD level*, reading authored types and
connections and nothing else. Then a finding is a statement about the asset that
holds for any renderer, and -- the useful half -- a mismatch that the script does
*not* report but which appears after translation is, by elimination, a bug in the
translation. Both halves of this entry came out of that one property.

**Three things the first version got wrong, all of which flattered it.** Worth
recording, because each is a way a checker can look clean and be useless.

It reported all 41 Open Chess Set materials as shading nothing. They bind
`outputs:mtlx:surface`, a render-context terminal, and `GetSurfaceOutput()`
returns the universal `outputs:surface`, which is indeed unconnected. Only the
last component of the name is the terminal.

It reported Pixar's Kitchen Set as authoring no materials at all. `TraverseAll`
does not descend into an instance -- the contents live in a prototype -- so the
walk found 453 prims, every one an Xform, and said nothing was wrong. A checker
that reports a clean bill of health for a scene it never looked at is worse than
no checker. It now walks the 114 prototypes too, once each rather than once per
instance. (Kitchen Set genuinely has no `UsdShade` materials -- 1462 meshes and
zero material prims -- because it is a `displayColor` asset. That is now reported
as a note rather than as silence.)

And it reported every `<UDIM>` path as a missing file. A tile pattern is not a
filename and is *supposed* not to resolve; what can be checked is whether any
tile exists on disk.

Between them these three accounted for 206 of the first run's 258 findings. The
52 that survive are real.

**The first thing it found was ours.** `gallery/subdivision_features.usda` named
`newzealand_height_map.png`, which lives under `gallery/textures/`. The file was
never found, so hdClaude drew the magenta placeholder it draws for an unreadable
texture -- and the committed baseline had encoded that placeholder since the
scene was adopted. The face-varying UV seam that sphere exists to test was a flat
colour, so the gate had been comparing one flat colour against the same flat
colour and passing. This is the second time a baseline has been wrong from the
day it was adopted, after the Kitchen Set's, and the second time the gate could
not catch it, because the gate's only reference is the baseline itself.


---

## 2026-09-07 -- A default sun has to know which way is up, and when not to shine

Two of the ten gallery scenes author no lights at all: Intel Sponza and Pixar's
Kitchen Set. Both rendered badly, and for two different reasons that a single
`lightCount == 0` test had run together.

**Which way is up.** The stand-in sun's direction was the constant
`{0.4, 0.7, 0.5}` -- about 48 degrees of elevation, and hardcoded Y-up. Kitchen
Set is Z-up. A wrongly assumed up axis does not dim a scene, it points the sun
*sideways*: the light ran horizontally through the room, along the floor rather
than down onto it.

A Hydra scene delegate is never told the stage's up axis. usdImaging passes the
world as authored and there is no `HdTokens` entry for it, so a renderer that
wants to put a default sun overhead has to be told. It is now a render setting,
`HDCLAUDE_UP_AXIS`, defaulting to Y, and the gallery script carries the axis per
scene alongside the camera it already carries for the same reason -- both are
facts about the stage that the delegate cannot recover.

The elevation is now 70 degrees. A high sun reaches the floor of a courtyard and
the back of an arcade; a low one rakes the near wall and leaves the rest of an
enclosed set to the sky. Sponza is the case that shows it -- its mean display
brightness goes from 0.005 to 0.018, and the image turns from a silhouette into
an arcade with lit columns and floor shadows.

**When not to shine.** Turning the sun up immediately exposed the other half of
the bug, in a scene that was never meant to be lit by it: the Open Chess Set
changed too, and the chess set has a dome light.

A dome light is not an entry in the light table. It supplies the *environment*,
and `Scene::hasDomeLight` exists precisely so the render pass does not put its
stand-in sky on top of one. But the stand-in *sun* was gated on
`frame.lightCount == 0u` alone, and a dome-lit stage has an empty light table.
So every dome-lit scene with no analytic lights had been rendering under its own
HDRI plus a second key light nobody authored -- the chess set for as long as it
has been in the gallery.

The gate is now `hdclaude_has_stand_in_sun()`, which asks for both, and the flag
is plumbed through the frame block rather than inferred: `hasDomeTexture` was
already there but means something narrower, since a dome with a constant colour
and no map lights a scene just as much as a textured one.

The lesson is the one about names. `lightCount` is an honest name for what it
counts -- entries in the light table -- and the test that used it was asking a
different question: *did this stage light itself at all*. The two agree for every
scene that has an analytic light and every scene that has nothing, which is eight
of the ten, and disagree exactly for the dome-only case.

**All three affected scenes moved toward hdCodex**, which is the check that
matters, since none of this was tuned against those baselines: Sponza's RMS
against hdCodex falls from 0.391 to 0.368, the chess set's from 0.060 to 0.049,
and Kitchen Set's from 0.074 to 0.056. The other seven scenes re-render
byte-identical, which is what the gating predicts -- the sun cannot reach a scene
that has lights.


---

## 2026-09-07 -- One missing call that read as three separate bugs

Reported as "an issue with potentially subdivision, uvs and textures". It was
one omission, in `HdClaudeMesh::Sync`: hdClaude never called
`sceneDelegate->GetSubdivTags(id)`. The string `SubdivTags` did not appear
anywhere in the source.

**Why that is not obviously a bug.** `GetMeshTopology` returns an
`HdMeshTopology`, and `HdMeshTopology` *has* subdivision tags -- it stores them
inside the `PxOsdMeshTopology` that `GetPxOsdMeshTopology` hands to the refiner
factory. So the code reads as though the tags come along with the topology. They
do not. The topology call returns the cage -- counts, indices, scheme, holes --
and the tags are a separate delegate call whose result has to be put back with
`SetSubdivTags`. Miss it and the tags are simply empty, and nothing complains.

**Why an empty tag set is worse than an error.** The refiner falls back to
OpenSubdiv's defaults, and OpenSubdiv's defaults are not USD's on any of the
three things that matter:

| | USD | OpenSubdiv default |
|---|---|---|
| `interpolateBoundary` | `edgeAndCorner` | none -- boundaries float |
| creases and corners | authored sharpness | absent |
| `faceVaryingLinearInterpolation` | `cornersPlus1` | fully linear |

Each produces a different-looking defect, which is why one bug arrived as three:

* An open boundary left to float shrinks inward at every refinement level. Two
  shells that meet along a closed seam in the cage pull apart from each other,
  and the gap has the sawtooth edge of the refined polygons. That is the crack
  across the Collective Project robot's head -- rendering the control cage at
  level 0 shows an unbroken shell, and level 2 splits it.
* Dropped creases round off every edge the model was built sharp on. The
  gallery's own `creasedCube` had been rendering as a smooth brown blob.
* Fully linear face-varying interpolation moves texture coordinates at every UV
  seam, which moves the texture -- the reported "uvs and textures" half.

**The fix is one line** plus `DirtySubdivTags` in the initial dirty-bits mask,
which was also absent, so the tags would not have been re-read when they
changed.

**The gallery could not have caught this, and did not.** Seven of the ten scenes
changed. The one that matters is `subdivision_features`, a renderer-owned scene
whose second shape exists precisely to prove that creases survive refinement:
its committed baseline was a rounded blob, and the gate had been comparing that
blob against itself and passing. Phase 3's exit gate claims subdivision
"preserves creases, corners, holes, orientation, face-varying seams" against a
committed baseline; the claim was false for as long as the baseline existed.

This is the third baseline in three days found to have been wrong from the day it
was adopted -- after the Kitchen Set's instance placement and the UV-seam
sphere's missing texture. The pattern is now clear enough to name: **a baseline
proves that nothing changed, and can prove nothing else.** Every gate whose only
reference is a previous render of the same renderer is a regression test wearing
a correctness test's clothes. The suite is deliberately USD-free -- the GPU
backend has no USD dependency and testing it through a stage would couple them --
which is a good rule that has left the entire Hydra adapter path with no
automated coverage at all. That gap, not this bug, is the thing worth fixing
next.

**Measured against hdCodex**, which is an independent renderer and so a check
this change was not tuned against: the playground falls from 0.262 to 0.257, the
subdivision matrix from 0.142 to 0.134, the chess set from 0.049 to 0.048. Three
shader balls move the other way by about 0.004. Correctness is preferred to
parity where they disagree -- USD says a creased edge is sharp, and hdCodex is
not ground truth.

**What this did not fix.** Pixar's Kitchen Set was expected to gain, since all
1462 of its meshes are `catmullClark`; it barely moved (RMS 0.0013), and its
table legs still taper to needles with one leg passing through the floor.
hdCodex renders those legs identically, from the same stage, so they are the
asset's own geometry and not a defect in either renderer. Worth recording
because the legs look exactly like the boundary-shrinkage artefact this entry is
about, and are not it.


---

## 2026-09-07 -- A light nothing can hit is not a light

Reported as a glass ball whose area lights do not fully reflect, with the right
question attached: is it that the lights are not rendered as geometry, or that
they are not sampled enough? It was the first, and no sample count would ever
have fixed it.

**Why sampling could not help.** Two decisions composed into a hole. The
analytic lights were absent from the acceleration structure -- deliberately, and
documented as what let the first implementation skip MIS. And next-event
estimation is skipped on a delta closure, which is correct: smooth glass has no
finite response at any single direction, so there is nothing to evaluate toward
a sampled point on a light. Each is defensible alone. Together they leave a
smooth dielectric with no way at all to see a light: not by sampling it, because
the closure is delta, and not by hitting it, because there is nothing to hit.
The ball was lit -- its rough parts take light through NEE perfectly well -- but
no light's *image* could appear in any reflection or refraction.

**Closed form, not triangles.** A rect light is a bounded plane and a sphere
light is a sphere. Tessellating them into a BLAS would add a discretisation of a
shape already known exactly, a build, and a question about how finely to divide
a light nobody is looking at. So `extend` intersects the lights analytically and
takes the nearest hit against the geometry distance, which is also what makes a
light *opaque*: it hides what is behind it, and does not shine through a wall.

The cost is a loop over the lights per ray. That is right for the handful a
scene authors and wrong for a thousand. When a thousand arrives they belong in
the structure, and the density and emission below do not change when that
happens -- only where the intersection comes from.

**The MIS is the part that can go wrong quietly.** The moment a ray can hit a
light, every non-delta surface reaches the same light twice: once by NEE, once by
scattering into it. Both estimates now take the balance heuristic's share. The
weights are written twice -- `hdclaude_sample_light` returns the density for a
sampled point, `hdclaude_light_hit_pdf` computes it for a point a ray reached --
and they have to agree exactly, including which faces emit: a rect emits from one
side, and a ray arriving at its back is not a hit, which is the same test the
sampler applies when it rejects a point facing away.

**So the test is a closed form, not a previous render.** Every baseline in this
project that has been wrong was wrong because it compared the renderer against
itself. A Lambertian surface of albedo `a` under a rectangular emitter of
radiance `L` directly above it leaves `a * L * F`, where `F` is the
point-to-rectangle configuration factor -- the standard corner formula, four
times over for a centred rectangle. At albedo 0.8, radiance 3, unit half-extents
and height 2 the closed form is 0.5747 and the render is 0.5716. It is the right
shape of assertion for an MIS weight because it is a *total*: counting a light
twice overshoots it, and weighting a strategy whose partner does not exist
undershoots it. Only a partition summing to one lands on it. A second test points
the camera at a light and reads back the radiance it was given, 0.4967 against
0.50.

**Distant lights had to come too, and that is not obvious.** A distant light is
at infinity, so no ray reaches it at a finite distance and it cannot be
intersected with the others. But once NEE weighs the analytic lights, a light
that is weighted down and has no second strategy to make up the difference simply
loses that energy. So a distant light's disc is added by the kernel that owns
rays which hit nothing, against the same cone density its sampler uses. Adding
hittability to some lights and not others is not a smaller change than doing all
of them; it is a wrong one.

**A scene of lights and no geometry is now a scene.** The test that reads a
light directly has no geometry in it at all, which built no top-level structure
and wrote `VK_NULL_HANDLE` into a descriptor -- illegal without the
`nullDescriptor` feature, and 18 validation errors. Vulkan permits a top-level
build of zero instances, so the instance buffer carries one zeroed entry to have
an address and the build is told there are none. The validation gate caught this
the first time the test ran, which is what it is for.

**Where this leaves parity.** Five gallery scenes changed, all of them scenes
with analytic lights, and three moved *away* from hdCodex: the gold ball from
0.048 to 0.061, the glass ball from 0.089 to 0.145, and Collective Project
unchanged at 0.118, against the playground improving from 0.257 to 0.246 and
bubblegum from 0.081 to 0.077. hdCodex's own image of the glass ball has no light
in it either -- it does not draw a light -- so the closer hdClaude gets to what
the scene describes, the further it reads from the reference. That is the
divergence the gallery already says it prefers, and it is now the largest one in
the table.


---

## 2026-09-07 -- How faint a glass highlight is supposed to be

Asked whether the glass ball should show the left-hand area light the way the
gold ball does, and whether its parameters match the borosilicate glass on
physicallybased.info. The short answer is that a much fainter reflection is
correct, and the reason is worth writing down because the eye is a poor judge of
it.

**Fresnel, at the two ends of the gallery.** Gold is a conductor and reflects
most of what hits it. A dielectric at normal incidence reflects
`((n-1)/(n+1))^2` -- at the shader ball's IOR of 1.54107 that is **4.5 per
cent**, about a twentieth of gold's. So the same light, in the same place,
leaves a highlight on glass roughly twenty times dimmer than on gold, and only
climbs toward gold's brightness at grazing angles, which is why the glass ball's
bright reflections sit on its rim and its silhouette rather than on the face
pointed at the camera. Nothing in the image contradicts that.

**Which is exactly why it is worth a test.** Four per cent against eight per
cent is invisible without a number to check against, and glass reflecting a
light at a twentieth of gold's strength looks, to the eye, much like glass not
reflecting it at all -- which is how the missing-light bug survived as long as it
did. A smooth `dielectric_bsdf` under a uniform environment of unit radiance
renders, at the pixel viewed head on, *precisely* its reflectance, because every
reflected direction returns the same radiance and no other term contributes. So
the closed form is directly measurable:

| IOR | rendered | ((n-1)/(n+1))^2 |
|---|---|---|
| 1.5 | 0.0402 | 0.0400 |
| 1.54107 | 0.0455 | 0.0453 |
| 2.0 | 0.1116 | 0.1111 |

**The parameters, against the reference.** The Standard Shader Ball's
`glass.mtlx` is an `standard_surface` with `base` 0, `transmission` 1,
`specular_roughness` 0.01625, `specular_IOR` **1.54107**, and
`transmission_color` **(0.942, 1.0, 0.9884)**. physicallybased.info gives 1.520
for both borosilicate and soda-lime glass, with a near-neutral transmission of
(0.988, 0.992, 0.985).

So they are close but not the same, in two ways worth naming. The IOR is 1.4 per
cent high, which moves normal-incidence reflectance from 4.26 to 4.53 per cent --
a difference no one will see. The transmission colour is the larger departure:
the asset's glass is noticeably green, with red at 0.942 where the reference has
0.988, and that tint compounds through the thickness of the ball. Neither is
wrong; the shader ball ships an artistic glass, not a spectrometer reading, and
its `transmission_dispersion` is 0 where real glass has an Abbe number the
reference does not publish either.

The asset is not edited over this. It is a third-party reference asset and its
values are its own; if a borosilicate match is ever wanted, the gallery
entrypoint sublayers the asset and can override the two inputs there, which is
where hdClaude puts corrections to assets it does not own.


---

## 2026-09-07 -- Where the glass highlight went

A better report than the last one: before the lights became hittable, the area
light was *just* visible on both the gold ball and the glass ball. Afterwards it
is crisp on the gold and appears gone on the glass, which also reads more
transparent. That is the shape of a regression, and it deserved more than "glass
is only four per cent reflective".

**Two closed forms say the estimator is right.** The first is Fresnel at normal
incidence, which the previous entry covers. The second is the one that matters
here, because it targets the exact configuration a near-mirror lobe creates:

A sharp lobe puts nearly all its density in a tiny cone, so at a direction
toward a light the closure's density dwarfs the light's, and the balance
heuristic gives next-event estimation almost nothing. Everything then rests on
the other strategy -- the scattered ray hitting the light. If that half were
missing the highlight would not merely get noisier, it would *vanish*, and every
furnace test would still pass, because a furnace has no light in it to lose.

So: a rect light wide enough to swallow the whole lobe, mirrored straight back
at the camera, must read `R(0) * L` whether the surface is a delta mirror -- which
takes the light entirely by hitting it -- or a narrow gloss, which splits it
between the two strategies. Rendered: delta 0.0800, gloss 0.0800, closed form
0.0800. The split conserves the total exactly.

**And the highlight is still on the ball.** Rendering the glass at two bounces
instead of eight is the diagnostic: two is not enough for a ray to enter the ball
and leave it, so transmission drops out and only the reflection survives. The
ball goes nearly black and the area light's diamond sits plainly on the upper
left of the dome, in the same place it sits on the gold ball. Nothing was lost.

**What actually changed is the background it sits against.** The glass transmits
about ninety-five per cent and reflects about four and a half. Making the lights
hittable therefore adds far more light *through* the ball than *off* it -- a lamp
seen through a window, which is the larger term by more than twenty to one. The
ball's mean display brightness rises from 0.677 to 0.713, and a four-per-cent
reflection that used to sit against a darker refracted backdrop now sits against
a brighter one. That is why it reads as washed out rather than absent, and why
the same change made the gold ball, which reflects nearly everything and
transmits nothing, look strictly better.

**The old highlight was noise, and that is the other half of the story.** Look at
the earlier images: the highlight on both balls is speckled. That is next-event
estimation on a near-mirror lobe -- a few samples landing where the closure's
response is enormous and the light's density small, which is unbiased and very
loud. Weighting it correctly hands the work to the strategy that does it quietly.
The gold ball shows what that is worth; the glass ball spends the same
improvement on a term that is a twentieth the size.


---

## 2026-09-07 -- Does a transmissive surface still reflect?

Sharper observation again: on the glass ball the reflections appear to be on the
*internal* faces pointed at the camera, and not on the outer shell. That is a
specific enough claim to have a specific test, and the obvious suspect is real:
every Fresnel assertion so far used `dielectric_bsdf` in its default
`scatter_mode` of "R", reflection only. The shader ball's glass transmits. A
closure that dropped its reflection lobe the moment transmission was enabled
would produce exactly the reported image and would pass every existing test.

**It does not.** The same rig -- a rect light wide enough to swallow the lobe,
mirrored back at the camera, nothing behind the quad but blackness so whatever
is transmitted leaves the scene -- now runs a third case with
`scatter_mode = "RT"`. The front face reflects **0.0808** against the closed
form's 0.0800, alongside 0.0800 for the reflection-only delta and 0.0800 for the
gloss. What a surface does with the light it does not reflect does not change
how much it reflects, and the renderer agrees.

**So why does the shell look empty?** Two reasons, and neither is a defect.

The outer face reflects about four and a half per cent of a bright white room
while transmitting about ninety-five per cent of *the same* bright white room.
The reflection and what lies behind it are nearly the same colour, so there is
almost no contrast to reveal one against the other. This is why glass in an
evenly lit room looks like a hole rather than a mirror, and it is why the light's
reflection is legible on the ball only when transmission is removed -- rendering
at two bounces, which is not enough to enter the ball and leave it again, drops
the shell to near black and puts the area light's diamond plainly on the dome.

The interior faces are bright for a reason the exterior cannot be: **total
internal reflection**. Beyond the critical angle -- 41.2 degrees at n = 1.5 --
a ray inside the glass reflects *entirely*, with no transmitted part at all. So
interior surfaces at glancing angles are perfect mirrors while the exterior at
the same angle is still only a few per cent. Real glass does this, and it is
where a paperweight's bright internal patterns come from.

**And some of it is the display, not the render.** The screenshots are usdview
at default exposure, where everything above 1.0 clips to white and the bright
transmitted light flattens whatever structure sits on top of it. The same frame
display-transformed at minus two stops shows the shell's gradients and
reflections perfectly well. Worth checking the exposure before reading a glass
render.


---

## 2026-09-07 -- Glass is unbiased and loud, and the reason is a missing strategy

The glass ball isolated -- inner sphere removed, one area light -- and rotated
frame by frame: the reflection hardens and softens unevenly, and there is less of
the light on the shell than one light in an otherwise empty scene ought to give.

**The estimator is not what is wrong.** The specular-under-a-light rig now runs
all four combinations of lobe and scatter mode, and the fourth is the shader
ball's glass exactly -- glossy *and* transmissive -- which is also the one place
the two densities could have disagreed. A closure picks a lobe before it picks a
direction; if next-event estimation were weighed against a reflection density
that had not been multiplied by the chance of choosing reflection -- about one in
twenty-two for glass at normal incidence -- while the scattered ray reported one
that had, the balance heuristic would be handed two different quantities and the
shortfall would land precisely on glossy transmissive surfaces. It does not:

| lobe | scatter mode | rendered |
|---|---|---|
| delta | R | 0.0800 |
| gloss 0.02 | R | 0.0800 |
| delta | RT | 0.0808 |
| gloss 0.02 | RT | 0.0810 |

against a closed form of 0.0800.

**What is wrong is that glass has only one strategy.** Next-event estimation
evaluates `CLOSURE_TYPE_REFLECTION` and nothing else. There is no transmission
estimate at all, and the guard that precedes it -- the light must lie on the
same side as the viewer -- makes that explicit. So a light reached *through* a
refracting surface is found by nothing except a scattered ray that happens to
point at it.

That leaves glass in a worse position than it looks. Its reflection is correctly
weighed down by MIS, because on a near-mirror lobe the closure's density dwarfs
the light's and next-event estimation genuinely has little to add there; and its
transmission has no next-event estimate to be weighed at all. Both terms
therefore rest on BSDF sampling, and the reflection branch is chosen on about one
sample in twenty-two. The estimate is unbiased -- the table above says so -- and
about twenty times louder than the same light on an opaque surface. At the sample
counts an interactive viewport reaches, that reads exactly as reported: a
highlight that firms up and softens as the object turns, and less light than a
single lamp seems to owe.

So the fix is not a correction, it is a missing feature: sample the transmission
closure toward a light as well, and weigh it by the same heuristic. It is
recorded in phase 6 as the largest remaining source of noise in the gallery
rather than folded in here, because it is a change to the estimator and wants its
own furnace.


---

## 2026-09-07 -- Transmission gets a next-event estimate, and it does not help the ball

Asked for a good-looking glass, so the missing strategy from the previous entry
is now implemented: next-event estimation picks the closure by which side the
light is on, exactly as a scattered direction already did. A light in front is a
reflection; a light behind is a transmission. The shadow ray is offset along the
side it leaves on, or a transmitted one starts on the wrong face and is occluded
by the surface it just passed through.

**It is correct.** The mirror image of the reflection assertion: a smooth
dielectric transmits `1 - R(0)` of what is directly behind it, so with a rect
light behind the quad and a black environment the pixel must read
`(1 - R(0)) * L` whether the surface is delta -- no next-event estimate, the
light found by hitting it -- or glossy, which now splits it. Rendered 1.9242 and
1.9240 against a closed form of 1.9200. A transmission response missing its
cosine, or weighed against the wrong density, moves that number; no reflection
test can, because none of them looks through anything.

**And it does nothing for the Standard Shader Ball.** Measured rather than
assumed: the glass at 64 samples against its own converged image scores an RMS of
0.107209 before the change and 0.107240 after. That is not an improvement, it is
the same number.

The reason is the same one that made the reflection highlight move strategies
earlier. That glass is near-specular -- 0.01625 roughness -- so on *both*
branches the closure's density dwarfs the light's and the balance heuristic
correctly hands next-event estimation almost nothing. Adding a second estimate
that is then weighted to nearly zero buys nothing. It pays where transmission is
*rough*: frosted glass, thick liquids, anything whose lobe is broad enough for a
light sample to compete.

So the gap was real and is now closed, and it is not what makes this asset quiet.
Worth recording plainly, because the tempting write-up -- implement the missing
strategy, show the closed form, declare the glass fixed -- would have been true in
every sentence and wrong in its conclusion. The remaining noise on a near-mirror
transmissive lobe is not a missing strategy; it is a lobe chosen on about one
sample in twenty-two whose light is small, and the honest fixes for that are more
samples, a broader lobe, or a bigger light.

**Cost.** The estimate now runs for lights on both sides, so a closure evaluation
happens where the old code returned early. Kitchen Set goes from 164 to 167
seconds and the playground from 108 to 113, about three to five per cent. Every
scene's image moved slightly because the random sequence shifted, while the means
did not -- Sponza 0.018298 to 0.018306, Kitchen Set 0.225991 to 0.225977.


---

## 2026-09-07 -- A honey that was glass, and the volume nobody read

A honey shader ball rendered identically to the clear one. The question asked was
the right one -- are we reading all the inputs? -- and the answer was yes,
completely: no input was pruned, and `transmission_depth`, `transmission_color`
and `transmission_scatter` all arrive in the generated shader.

**The colour was never on the surface to begin with.** OpenPBR replaces a
transmissive material's surface tint with white the moment `transmission_depth`
rises above zero, and hands the colour to the interior instead as an absorption
of `-log(colour) / depth`. Honey at depth 2 has therefore given up its only
surface colour by design. A renderer that ignores the volume does not render a
slightly-wrong honey; it renders clear glass.

**And the volume was published into globals nothing read.** `mx_anisotropic_vdf`
set `hdclaude_medium_absorption` and its neighbours; grepping the kernels and the
host for `hdclaude_medium` returned nothing. Worse, two comments asserted the
opposite -- the node's own header said the `shade` kernel applied Beer-Lambert
absorption and Henyey-Greenstein scattering, and the generated ABI block said
"read by the integrator". Both were false, and either would have stopped this
investigation early if believed. A comment that describes an intention in the
present tense is a trap.

**Where absorption has to be applied.** Not at the surface: there is no distance
to integrate over at a point, which is exactly why stock MaterialX's
`exp(-absorption)` -- absorption over one implied unit of distance -- is the
approximation a path tracer exists to avoid. So the coefficient rides on the path
and `extend` applies the attenuation, because `extend` is the kernel that knows
how far the ray actually went. Entering and leaving is decided by the side a
transmission crossed on: a geometric normal facing the incoming ray means the
path is going in.

**The transmittance is upsampled, not the coefficient.** `exp(-sigma d)` is
bounded in (0, 1] whatever the coefficient is, which is precisely the range the
reflectance fit is built for and guaranteed on; an absorption coefficient is
unbounded and has no such fit. It also keeps the one rule this renderer has about
colour, that RGB becomes spectral at the closure boundary and nowhere else.

**Two override bugs found on the way.** `mx_anisotropic_vdf` takes an `inout
BSDF` and wrote nothing to it, so a layer adding that value was adding whatever
the generated code had declared. And `mx_layer_vdf` added the base's response and
throughput to the top's, which for a volume meant adding 1.0 to the surface's
throughput -- stock MaterialX does that because *its* volume node folds the
medium into the throughput, and hdClaude's does not. The layer now passes the top
through unchanged, which is what "the surface is unchanged by what it encloses"
actually means. This visibly corrects the OpenPBR Playground, whose green jar was
washed out and is now green.

**Testing it needed the assertion to be a ratio.** The absolute value carries
whatever factor `open_pbr_surface` puts on transmission -- energy compensation
among them -- which came out about twelve per cent above `(1 - R(0))` and is not
something a medium test has any business predicting. Dividing two renders that
differ only in the interior cancels it exactly. What survives is Beer-Lambert
alone: 0.6402, 0.3596, 0.1599 against a closed form of 0.6400, 0.3600, 0.1600.

The first attempt built the material by wiring `anisotropic_vdf` under a `layer`
by hand and rendered black, for reasons in MaterialX's node resolution that were
not worth chasing. Building it as `open_pbr_surface` instead is both simpler and
better: it is the route a real asset takes, so the test exercises the path honey
exercises rather than one invented for it.

**What honey still is not.** `transmission_scatter` is authored at 0.9 and is
published and not transported. The medium absorbs and does not scatter, so the
ball is a clear amber rather than the cloudy material honey actually is. That is
a random walk with a phase function, not a multiply, and it is recorded as
remaining rather than approximated.


---

## 2026-09-07 -- A walk through a medium, and the closure that never enters one

Scattering now happens. `extend` samples a free flight, and if it lands short of
the boundary the path scatters there: a Henyey-Greenstein direction, and round
again. Honey goes from a clear amber to the cloudy material it actually is, mean
display brightness 0.402 to 0.635.

**Why the walk lives in the traversal kernel.** A scattering event is not a
shading event. It has no material, so the per-material sort has no bin for it,
and a path that scatters needs another *traversal* rather than another shade.
Walking to completion inside one invocation keeps the wavefront's shape -- a path
still leaves `extend` with exactly one surface hit -- at the cost of a loop
bounded by its own roulette instead of by the bounce count.

**Absorption is deterministic; only scattering is sampled.** The textbook form
samples a collision against the combined extinction and weights by the
single-scattering albedo. It is unbiased, and in a medium that only absorbs it
replaces a closed-form attenuation with a coin flip that kills the path -- right
in the mean, far noisier for nothing, and honey and coloured glass are exactly
that medium. Splitting them means a non-scattering medium takes the boundary
branch every time with no randomness at all.

**Scattering had to be made achromatic, and that is a real limitation.** A
chromatic scattering coefficient sampled against one control wavelength leaves
every other lane carrying `exp((control - sigma_lane) * flight)`. That grows with
the flight and compounds over a walk. It did not merely add variance: a strongly
forward-scattering slab rendered **NaN**. The honest fix is multiple importance
sampling across the four lanes' densities, which is a piece of work in itself;
taking the mean makes every lane share one density, so the scattering weight is
exactly one and only absorption carries colour. That is where a medium's colour
comes from in nearly every real material, but it is an approximation and is
recorded as one.

**The test that could not be posed here.** A closed form for a random walk in a
slab is the integral nobody can write down -- which is why it is sampled -- so the
assertion has to be a limit the walk reproduces. Forward-scattering at g near one
with unit albedo should be a no-op. It measured a fifth of the clear result, and
the walk is not at fault: a single quad bounds no volume, so entering the medium
puts the path in an *unbounded* one, where a beam diffuses over four mean free
paths and never re-collimates. The test was removed rather than loosened until it
passed. Asserting on a scattering medium needs a closed slab, and that is
recorded rather than faked.

**And subsurface is still not transported.** It is wired: a subsurface closure's
albedo and radius are converted to the scattering and absorption of a medium and
handed to the same walk, since a random walk beneath a surface and one inside a
volume are the same walk. The hand-off is simply never reached.
`mx_subsurface_bsdf` evaluates `CLOSURE_TYPE_REFLECTION` and samples a direction
in the hemisphere above the surface; it never produces a transmission, and the
medium is only entered on a transmission. So the bubblegum ball re-renders
byte-identically, which is the honest signal that nothing changed for it.

Making it work means giving the subsurface closure an entering direction, which
is a change to the closure rather than to the integrator. Worth stating plainly
because the machinery *looks* finished from the integrator's side, and a walk
that is never entered is indistinguishable from one that does not exist.


---

## 2026-09-07 -- The furnace that would not light, and what it caught

A random walk in a slab has no closed form -- that is why it is sampled -- so the
only exact assertion available about one is *conservation*. A closed object that
absorbs nothing, sitting in a uniform environment of unit radiance, must render
exactly one, whatever it does to the light inside it: every direction sees the
same radiance, so redirecting a path cannot change what it finds. That is the
volumetric white furnace, and it needed the one piece of geometry the test file
did not have, a second quad wound the other way, because one quad bounds no
volume and a path entering it is inside an unbounded medium for ever.

It renders **1.4124**.

**And the walk is not what is wrong.** Running the same slab twice, once empty
and once filled with a conservative medium, splits the blame in a way the
combined figure cannot:

| slab | rendered | expected |
|---|---|---|
| surface only, nothing inside | 1.1505 | 1.0 |
| with a medium of unit albedo | 1.4124 | 1.0 |

An empty dielectric slab gains fifteen per cent. The medium only compounds it, by
crossing the surface more often. So `open_pbr_surface`'s transmission is not
energy conserving, and this is the third time that number has appeared: the
absorbing-medium test measured about twelve per cent above `(1 - R(0))` and was
rewritten as a ratio specifically so the excess would cancel, which was the right
way to test a medium and the wrong moment to stop asking why.

**The test is withheld rather than committed.** Two ways to have shipped it were
available and both are worse. Committing it failing breaks the suite for everyone
until the surface is fixed. Committing it at a tolerance wide enough to pass --
six per cent would not do it, but sixteen would -- records the defect as correct,
which is precisely the failure this project has now found in three separate
baselines. A gate that is loosened until it passes has stopped being a gate.

So the furnace waits for the fix it is diagnosing, and the fix is the next piece
of work: whatever `open_pbr_surface` does to transmitted energy, it adds fifteen
per cent per crossing, and every transmissive material in the gallery is
carrying it.


---

## 2026-09-07 -- Which of the two dielectrics is wrong

The furnace from the previous entry said a transmissive slab gains fifteen per
cent. It did not say *whose* fifteen per cent. Running it twice, over the same
geometry and the same environment, does:

| slab material | rendered | expected |
|---|---|---|
| bare `dielectric_bsdf`, scatter mode RT | 1.0001, 0.9987, 0.9979 | 1.0 |
| `open_pbr_surface`, transmission weight 1 | 1.1505, 1.1481, 1.1480 | 1.0 |

Both call the same closure. hdClaude's `mx_dielectric_bsdf` conserves energy to
within two parts in a thousand across a slab a path crosses twice, which is as
close as 512 samples can report. So the closure is right and the defect is in how
`open_pbr_surface` *composes* it.

**What OpenPBR does differently.** It does not ask the dielectric for reflection
and transmission together. It calls it twice -- once with scatter mode R and once
with T -- and then, at the end of the graph, does

    dielectric_substrate = mix(volume_transmission, opaque_base, transmission_weight)
    dielectric_base      = layer(dielectric_reflection, dielectric_substrate)

So a reflection-only lobe is layered over a transmission-only lobe.
`mx_layer_bsdf` evaluates that as `top.response + base.response * top.throughput`,
and with the transmission lobe already carrying its own `1 - F`, the *response*
is `R + T(1 - F)` -- about four per cent short at normal incidence, not fifteen
per cent over. **The evaluation loses; the render gains.** Whatever is wrong is
therefore in the layer's sampling and the mixture density it reports, not in the
arithmetic of its response. That is a narrow enough place to look, and it is
where the next attempt should start.

**What went in and what did not.** The bare-dielectric case is committed as a
gate, because it passes and because it is the assertion that will say the fix did
not break the closure while fixing the graph. The `open_pbr_surface` case is
written, measured, and left unasserted, with the number in a comment beside it.
Committing it failing would break the suite; committing it at a tolerance wide
enough to pass would record a fifteen per cent energy gain as correct, which is
the exact failure this project has now found in three separate committed
baselines. A gate loosened until it passes is not a gate.

Every transmissive material in the gallery is carrying this: glass, honey, the
playground's jars, Collective Project's canopy.


---

## 2026-09-07 -- Three layers nobody switched on, taking five per cent each

The furnace said a slab of `open_pbr_surface` gains fifteen per cent and a bare
`dielectric_bsdf` gains nothing. Both call the same closure, so the fault was in
the composition, and the direction of the error said where: `mx_layer_bsdf`
evaluates `top.response + base.response * top.throughput`, which for a
reflection lobe over a transmission lobe is `R + T(1 - F)` -- about four per cent
*short*. The evaluation loses while the render gains, so the error had to be in
the sampling and the density, not the arithmetic.

It was one clamp:

    float pTop = clamp(mean(1 - top.throughput), 0.05, 0.95);

The intent was defensible -- neither lobe should be selectable with zero
probability while still contributing to the response, which would be an infinite
weight -- and the floor was the expensive half of it. `open_pbr_surface` layers a
coat over a fuzz over its base *whether or not they are enabled*, and a layer of
zero weight has zero albedo. So three dead lobes claimed five per cent of the
mixture density each, and a density that does not describe the sampling is not a
density. Fifteen per cent, from three layers nobody switched on.

**Zero is safe, and for a reason worth stating.** The two conditions coincide: a
top layer with unit throughput has taken nothing from the ray, so its response is
zero and there is nothing left to weigh. `mix` at zero returns the base's density
exactly, and the selector never picks a lobe it was given no probability for. The
guard was protecting against a case that cannot arise.

**What it was worth.** The slab goes from 1.1505 to 1.0145. Against hdCodex --
an independent renderer, and not something this was tuned against -- the glass
ball falls from 0.145 to 0.113, the playground from 0.248 to 0.236, the chess set
from 0.049 to 0.045 and Collective Project from 0.118 to 0.116; the gold ball and
bubblegum drift the other way by about 0.007. Every mean in the gallery drops,
which is what removing invented energy looks like.

**And a residual that is not explained.** 1.4 per cent remains. It is gated at
three, which is loose against the closure's own two parts in a thousand and tight
enough that a return to fifteen cannot pass unnoticed, and the number is written
in the test beside the tolerance so that widening it further has to be somebody's
deliberate decision. Chasing the last per cent means auditing the other
combinators the same way -- `mx_mix_bsdf` and the thin-film mixes are the
untested ones -- and the furnace now exists to do it with.


---

## 2026-09-07 -- Subsurface: an attempt that was reverted, and what it found

Making the subsurface closure enter the surface was tried and taken back out. The
tree is unchanged; this is what the attempt learned, so the next one starts
further along.

**The diagnosis was right.** `mx_subsurface_bsdf` documents itself as providing
"the cosine-weighted distribution by which a path enters the surface", and then
samples `mx_pt_sample_cosine_hemisphere` about the *forward-facing* normal and
evaluates only `CLOSURE_TYPE_REFLECTION`. Both of those are the hemisphere
**above** the surface. The integrator enters a medium only when a scattered
direction crosses to the other side, so the parameters this closure publishes on
every evaluation are entered on none of them. It is a Lambertian reflector
wearing a random walk's name, and the bubblegum ball rendering byte-identically
after the medium machinery landed is what that looks like.

Sampling about `-N` and evaluating `CLOSURE_TYPE_TRANSMISSION` does route it into
the walk. Three things then went wrong, in increasing order of how much they
matter.

**The entry must be colourless, and proving it is not the same as guessing it.**
`color` is the medium's single-scattering albedo, published for the walk and
applied at every scattering event inside it. Applying it at the boundary as well
tints the entry and then tints it again on the way through -- the classic way a
random-walk BSSRDF ends up too dark to match its own albedo. Removing it is
right, and the ball then rendered *white*, which says the walk was contributing
no colour of its own and the entry had been carrying all of it.

**The hand-off is gated on a condition that is not the right one.** The
integrator took the subsurface parameters only when no volume was also published
(`hdclaude_medium_present < 0.5`). Dropping that gate gave a plausible pale, waxy
pink -- but it also moved the glass ball by an RMS of 0.188 and the honey ball by
0.107, neither of which has any subsurface at all, and it failed six render
checks. So the interaction between the volume node and the subsurface node is not
understood, and a change that improves one material by breaking two others is not
an improvement.

**And the radius mapping needs care.** Extinction is taken as `1 / radius`, and
the bubblegum material authors `subsurface_radius = (1, 0, 0.068)` -- a zero
component. Clamping that to `1e-4` gives an extinction of ten thousand and a mean
free path far below the walk's step budget, so green is either absorbed
immediately or the walk exhausts its cap. A zero radius means "no transport in
this channel", and the mapping has to say so rather than divide by an epsilon.

The next attempt should start from the third point and work back: get the
parameter mapping right first, then find out why glass and honey see a subsurface
publication at all, and only then change the closure's entry direction.


---

## 2026-09-07 -- One more fact about the 1.45 per cent

Reading `mx_dielectric_bsdf` for what it hands the layer above it turned up
something worth writing down before the next attempt.

`bsdf.throughput = 1.0 - dirAlbedoV * weight` is computed *before* the
scatter-mode branch dispatch, deliberately -- the comment says so, and the reason
given is sound: an early return that skipped it would leave a layer above
choosing on a stale value. But `dirAlbedoV` is the **reflection** directional
albedo, `mx_ggx_dir_albedo` times the GGX energy compensation. So a lobe asked
for transmission only still reports `1 - F` as its throughput, which is the
fraction of light a *reflector* would pass down, not anything about the
transmission it was actually asked for.

For `layer(R, T)` the layer reads only `top.throughput`, and the top is the
reflection lobe, so that value is the right one by luck. What it also does is
make `result.throughput = top.throughput + base.throughput` -- 1.39.3's additive
form -- come out near `2(1 - F)` rather than anything meaningful, and any layer
wrapped around *that* selects on it.

That is one of the two places the missing 1.45 per cent can be. The other is the
density a delta lobe reports: a smooth dielectric sets `isDelta` and the layer
mixes `top.pdf` and `base.pdf` as though both were solid-angle densities of the
same kind. Both are cheap to check with the furnace already in the suite, and
neither has been checked yet.


---

## 2026-09-07 -- The 1.45 per cent is two errors of opposite sign

Both halves are now measured, and neither can be fixed alone. The residual on
`layer(R, T)` is not a small error; it is a four per cent loss and a five per
cent gain that very nearly cancel.

**The gain, in the closure.** `mx_dielectric_bsdf` splits between reflection and
refraction with

    reflectProbability = transmissive ? clamp(luminance(F), 0.05, 0.95) : 1.0
    refractProbability = 1.0 - clamp(luminance(F), 0.05, 0.95)

and `transmissive` is `scatter_mode != 0`, which is true for **T-only** as well as
for RT. So a lobe asked for transmission alone still behaves as though a
reflection alternative existed: at normal incidence the clamp floors the split at
0.05, so it reflects one sample in twenty that the caller never asked for, and
divides its density by 0.95. That is the same clamp-off-zero mistake already
found in `mx_layer_bsdf`, one level down, and it is the third time this pattern
has appeared.

**The loss, in the layer.** `mx_layer_bsdf` evaluates
`top.response + base.response * top.throughput`. With the reflection lobe
reporting `throughput = 1 - F` and the transmission lobe's response *already*
carrying its own `1 - F`, the factor is applied twice: the layered response is
`F + (1 - F)^2`, about four per cent short of one at normal incidence, and a slab
is crossed twice.

**They cancel, and the proof is what happens when one is removed.** Restricting
the split to `scatter_mode == 2`, so a single-lobe request no longer pretends to
choose, takes the furnace from **1.0145 to 0.9206** -- from 1.5 per cent over to
8 per cent under, and 0.9206 is close to what `(F + (1 - F)^2)` squared predicts
for two crossings. The change is right on its own terms and makes the image
worse, so it was reverted rather than shipped.

**What the next attempt has to decide.** Whether the transmission lobe's response
should carry `1 - F` at all. MaterialX's layering convention is that a base sees
what the top transmits, which means the base should *not* know about the top's
Fresnel; if that is right, our transmission response is the deviation and the
layer is correct. If instead the response is right, then `layer` must not
re-apply `top.throughput` to a base that is the same closure's other half. One of
those two, and the furnace already in the suite will say which, because only one
of them lands on 1.0.

Worth noting for its own sake: a gate that reads 1.0145 was hiding a four per
cent error and a five per cent error. A tolerance of three per cent passes that
and would have passed it indefinitely. The furnace is doing its job precisely
because it is tight enough to have made this visible at all.


---

## 2026-09-07 -- Both halves, and why neither could go first

The furnace now reads 0.9953 on a layered slab, against 1.0145 before and 1.1505
before that. Both errors are fixed, and the reason they took three passes to find
is worth keeping.

**The gain: a single-lobe request that still pretended to choose.**
`mx_dielectric_bsdf` guarded its reflect/refract split on
`transmissive = scatter_mode != 0`, which is true for T-only as well as for RT.
A lobe asked for transmission alone therefore reflected one sample in twenty --
the clamp floors the split at 0.05 -- and divided its density by 0.95. The split
is a choice, and a closure asked for one lobe has nothing to choose between, so
it now applies only when `scatter_mode == 2`.

**The loss: Fresnel applied twice.** `mx_layer_bsdf` evaluates
`top.response + base.response * top.throughput`, and the reflection lobe reports
`throughput = 1 - F`. The transmission lobe's response carried its own `1 - F` as
well, so the layered response was `F + (1 - F)^2`. MaterialX's layering
convention settles which of the two is the deviation: a base sees what the top
transmits, so a base must *not* know the top's Fresnel -- the layer supplies it.
A transmission-only lobe exists only to be layered under a reflection one, which
is the only way `open_pbr_surface` uses it, so it stops carrying the factor. RT
keeps it, because there the closure weighs its own two lobes and nothing above
supplies anything.

**Why neither could be fixed first.** They are of opposite sign and nearly equal.
Removing the gain alone took the furnace from 1.0145 to **0.9206** -- a change
that is right on its own terms and makes every transmissive image in the gallery
worse. Any attempt to fix one and check the result would have been reverted as a
regression, which is exactly what happened on the first attempt. The two had to
be understood as a pair before either could move.

**What that says about tolerances.** The gate stood at three per cent while the
slab read 1.0145. That passes, and it would have passed indefinitely, and behind
it sat a four per cent error and a five per cent error. A tolerance wide enough
to accommodate a defect is wide enough to accommodate two defects whose sum is
smaller than either. The gate is now two per cent, three times the measured
error, and the two numbers this furnace has already caught -- 1.1505 and 1.0145
-- are written beside it so that widening it again has to be argued for.

Against hdCodex the glass ball falls from 0.113 to 0.112, the playground from
0.236 to 0.235 and the chess set from 0.0454 to 0.0453; Collective Project holds.
Small, and all in the same direction, which is what a correction of a fraction of
a per cent should look like.


---

## 2026-09-07 -- Bisecting the subsurface attempt

The earlier attempt changed two things at once and produced a confusing result:
six failing checks, and the glass and honey balls moving by RMS 0.188 and 0.107
despite having no subsurface at all. Applying the two halves separately says
which did what.

**The closure change alone is well behaved.** Making `mx_subsurface_bsdf` sample
about `-N` and evaluate `CLOSURE_TYPE_TRANSMISSION` -- so that a path entering a
subsurface material actually goes in -- leaves all 93 render checks passing and
every gallery scene byte-identical **except the Open Chess Set**, whose stone
pieces are the only geometry in the gallery that authors subsurface at all.
Sponza, glass and honey do not move.

So the glass and honey regression belonged to the *other* half: dropping the
`hdclaude_medium_present < 0.5` guard on the hand-off in `shade`. Something is
publishing a volume for those materials, and removing the guard let a subsurface
publication that should never have applied to them take it over. That is the
thing to understand next, and it is a question about what `standard_surface` and
`open_pbr_surface` publish for a material with no subsurface, not about the
closure.

**But the closure change is not right yet either.** It takes the chess set from
0.0453 to 0.0500 against hdCodex -- worse, and not obviously excusable the way
the hittable-lights divergence was, because hdCodex almost certainly renders
subsurface as diffuse and so did hdClaude until this change. Without the radius
mapping fixed first, entering the surface hands the path to a walk whose
extinction is `1 / radius` with an epsilon clamp, and the bubblegum material
authors a zero radius component. The order stated in the previous entry -- get
the mapping right, then the publication question, then the entry direction -- is
the right one, and this was an attempt to take the third step first.

Reverted. What it bought is the isolation: the closure change is safe and
narrow, and the regression that made the first attempt look catastrophic lives
entirely in the hand-off guard.


---

## 2026-09-07 -- Subsurface is blocked on chromatic media, not on its own mapping

The next step for subsurface was going to be the radius mapping: `extinction =
1 / max(radius, 1e-4)` turns the zero component the bubblegum material authors
into an extinction of ten thousand, which is not what a zero mean free path
means. That is true, and it is not the blocker.

**The colour of a subsurface material is its per-channel mean free path.** Red
travels far in skin and blue does not; that difference *is* the effect. In the
parameters it appears as a chromatic `radius`, and bubblegum authors
`(1, 0, 0.068)` -- a ratio of about 1 : 0 : 15 between the channels.

**The walk averages it away.** `extend` collapses the scattering coefficient to
its mean before sampling a free flight, because a chromatic coefficient sampled
against one control wavelength makes every other lane carry a weight that grows
with the flight and overflows -- it produced NaN, and taking the mean was the
containment. Absorption stays spectral, so *some* colour survives, but it is the
wrong mechanism: absorption removes light over a path length the walk has already
made achromatic, and it cannot reproduce a target diffuse albedo per channel the
way differing mean free paths do.

So fixing the mapping recovers information that is destroyed one step later. A
third attempt at subsurface transport would fail for a reason none of the first
two revealed, and it would fail *quietly* -- a plausible, desaturated result that
looks like a tuning problem rather than a structural one.

**The order is therefore:** multiple importance sampling across the four lanes'
densities for media, then the radius mapping, then the publication question --
why glass and honey see a subsurface publication at all -- and only then the
entry direction. The first of those is the one item that unblocks both honey's
chromatic scattering and subsurface, and it is the piece the medium work has been
deferring since the walk landed.

The subsurface furnace committed alongside this note is the gate for all of it,
and passes at 0.9995 on the Lambertian that subsurface currently is.


---

## 2026-09-07 -- Spectral media MIS, and a test that could not judge it

Balance-heuristic multiple importance sampling across the four lanes was
implemented for the medium walk and then reverted, and the reason it was reverted
is not the reason it first appeared to be. Recording both, because the second
attempt at this should not repeat the first's mistake in *reading* the result.

**What it does.** One lane is chosen uniformly to drive the free flight, and the
density is the average of all four lanes' densities rather than the sampled
lane's alone. That is the standard fix for the overflow described earlier: a lane
that is dense where the sampled lane is thin no longer carries
`exp((control - sigma_lane) * flight)` without bound, because dividing by the
mean caps any lane's weight at four however long the walk.

**It removed the overflow.** The chromatic slab that previously rendered NaN
rendered finite numbers, and every existing furnace and the absorbing-medium
closed form were unchanged to four decimals.

**But the test built to judge it was unsound, and the numbers looked like a
verdict.** A slab with `transmission_scatter` chromatic and `transmission_color`
white was assumed lossless -- absorption is `-log(1)/depth`, which is zero -- and
it read 1.03, 0.47, 0.49. That looks exactly like a spectral estimator losing the
lanes it did not sample from, and it was written up that way.

It is not. Reverting to the achromatic walk and re-running the same test gives
1.05, 0.44, 0.41: the *same* loss, from a code path where the walk multiplies
throughput by exactly one and cannot lose anything at all. Whatever removes that
energy is in the material or the surface, not the transport, and the colour in
the result cannot come from an averaged scattering coefficient either. The test
was measuring something else entirely.

So the honest position is that spectral MIS across the lanes is **unjudged**, not
refuted. It was reverted because a change that cannot be validated should not
ship, not because it was shown wrong. The next attempt needs a lossless medium it
can actually construct -- most likely `anisotropic_vdf` wired directly, where the
scattering and absorption coefficients are the authored inputs rather than
whatever OpenPBR derives from a colour and a depth -- and only then is the
question of per-step versus per-walk lane selection worth asking.

The unsound test is withdrawn rather than left in place with a comment. A test
whose premise has not been established measures nothing, and this one had already
produced one confident wrong conclusion.


---

## 2026-09-07 -- Dispersion cannot come through the closure

The question was whether dispersion should travel to the integrator through a
revived `BSDF::spectrum` channel or as a published parameter like the medium.
Neither: MaterialX 1.39.3 discards it before any closure can see it.

`open_pbr_surface` declares `transmission_dispersion_scale` and
`transmission_dispersion_abbe_number`, threads both through the generated
`NG_open_pbr_surface_surfaceshader` signature, and then **never reads either in
the body**. `ND_dielectric_bsdf` has no dispersion input to pass them to -- the
nodedef simply does not have one. So the graph accepts the parameters, carries
them as far as the function that would use them, and drops them.

That is not a hdClaude bug and there is nothing in the closure ABI to fix. A
per-lane response channel would have nothing to put in it.

**So dispersion has to come from the host.** hdClaude's material compiler reads
the authored network and can see `transmission_dispersion_abbe_number` on the
surface node directly, exactly as it reads any other input. The value becomes
per-material data alongside the compiled program, and the integrator applies it
at a transmission event: per-lane index from `DispersedIor`, refract on the hero
lane, and terminate the other three, because one path can only take one
direction.

This is the same division of labour the medium already uses -- the material
states a property, the integrator transports it -- and it needs no ABI change at
all, which makes it smaller than either option that was on the table. What it
does need is host plumbing that no other closure parameter uses: every other
material input reaches the GPU inside the generated program, and this one has to
travel beside it.

Recorded rather than started, because the shape of the change moved: it is now a
host-side feature with a shader consumer, not a closure feature. The
wavelength-to-index relation it will call is already in the core and tested
against BK7, SF11 and diamond.


---

## 2026-09-07 -- Dispersion, host-side, and what it costs

The previous entry established that dispersion cannot come through the closure
and has to arrive from the host. It now does.

**The route.** `AuthoredDispersion` in the MaterialX layer reads
`transmission_dispersion_scale` and `transmission_dispersion_abbe_number` off
the document -- the authored value where there is one, the nodedef's own default
where there is not, because a material that says nothing about dispersion still
*has* a scale, and it is zero. OpenPBR's scale "linearly scales the amount of
dispersion", and dispersion is the dispersive power `1 / V`, so the two numbers
resolve to one effective Abbe number `V / scale`. It lives beside the generator
rather than in the Hydra layer because it is a statement about a MaterialX
document, and because that is what makes it testable without OpenUSD. A
connected input is reported rather than sampled: an index of refraction taken
from the wrong texel is worse than no dispersion.

The number rides on `CompiledMaterial` and reaches the GPU as a push constant on
the shading dispatch. That dispatch is already per material, so a per-material
value costs nothing there -- which is why this needed no uniform block and no
ABI change. `mx_dielectric_bsdf` reads it from a global the shade kernel sets,
the same mechanism the medium uses in the opposite direction.

**The collapse, and the factor of four.** A dispersive interface sends every
wavelength somewhere else and a path is one direction, so the packet stops being
four correlated estimates the moment it meets one. The hero lane keeps going at
its own index; the other three are terminated.

Terminating alone renders a *quarter* of the right answer. The film averages the
four lanes -- it divides by `4 * p(lambda_hero)`, and every lane shares the
hero's density -- so three empty lanes make an average of a quarter of one
estimate. The survivor is scaled by the lane count to restore the single
wavelength estimator `CMF(lambda_0) * L_0 / p`, which is unbiased and four times
noisier. That is the true cost of dispersion and it is paid only by paths that
touch a dispersive material.

The compensation has to happen exactly once, and nothing else on the path
records that it already has: three zero lanes are what a terminated path looks
like too. So a path carries a flag. Without it, a ray entering a glass slab and
leaving it shades the same dispersive material twice and comes out four times
too bright.

Both failures are caught by a furnace rather than by an eye: a closed dispersive
slab that absorbs nothing, in a uniform environment, must still render one and
must still render neutral. It reads 0.9919, 0.9969, 1.0061.

**What the closed form says.** Dispersion's visible effect is a faint colour
fringe, which is precisely the kind of thing an image cannot be checked for, so
the magnitude is asserted against an integral. A smooth `dielectric_bsdf` in RT
mode, mirrored back at a rect light with nothing else in the scene, reflects
`R(lambda) = ((n(lambda) - 1) / (n(lambda) + 1))^2` and transmits everything
else into blackness, so the pixel *is* that spectrum's colour under the light.
At n = 1.5 and an Abbe number of 20 -- a dense flint, more dispersive than SF11
-- it renders 0.0759, 0.0811, 0.0876 against the closed form's 0.0782, 0.0816,
0.0874, and 1.154 blue over red against 1.118. Red is the loose channel because
sRGB red is a difference of large XYZ terms and so amplifies what noise a
collapsed packet leaves; it converges from 4.3 per cent low at 4096 samples to
2.9 per cent at 8192.

The ratio is asserted separately from the magnitude, because a dispersion
relation with its sign inverted still produces a tinted highlight and still
lands near a closed form computed with the same inverted sign. What it cannot do
is make blue the strongly reflected end.

**The limit, and it is a real one.** Dispersion is applied only to a
`dielectric_bsdf` whose `scatter_mode` transmits. That is what OpenPBR's input
says -- it is named `transmission_dispersion_*`, and it describes the medium the
light is refracted into -- and it is also all that can be said safely, because a
reflection-only dielectric is indistinguishable at runtime from a **coat**,
which is a different interface with its own index and no Abbe number at all.

The consequence is visible in the same measurement. MaterialX builds OpenPBR's
specular lobe as `layer(top: reflection-only dielectric, base: transmission-only
dielectric)`, so an `open_pbr_surface` glass refracts a full spectrum and
reflects a highlight that is only partly tinted: 1.037 blue over red where the
interface itself gives 1.154. Energy is still conserved -- the layer hands the
base `1 - F` and the base transmits it whatever its own index -- so this costs
colour in a highlight and nothing else. Applying the transmission medium's Abbe
number to every dielectric lobe would fix the highlight and disperse every coat
in the scene along with it, which is a worse trade and an invention rather than
an implementation.

**The gallery moved, slightly, and not because anything shades differently.**
`hdclaude_dispersed_ior(ior, 0, lambda)` returns its argument by an early return
and performs no arithmetic at all, so a non-dispersive material is handed a
bit-identical index. What changed is that `ior` is now a value the closure
assigns rather than a literal the compiler can fold through, and a different
schedule of the same arithmetic moves a Fresnel term in its last bits. In a path
tracer that reroutes a handful of paths, which is why two of the eleven scenes
shifted: OpenChessSet by an RMS of 0.00049 with 0.001 per cent of pixels over
the per-pixel limit, and Collective Project 001 by 6.1e-05 with none. Both means
are unchanged to six digits, which is what a rescheduled estimator looks like
and is not what a changed closure looks like. Avoiding it entirely would need a
specialisation constant per material pipeline, which is machinery bought for
byte-identity rather than for correctness.


---

## 2026-09-07 -- The medium's furnace, and the container it needs

Spectral MIS across the lanes is what the chromatic medium is blocked on, and
the chromatic medium is what subsurface is blocked on. The previous attempt at
it was reverted because it could not be validated, and the note on that reversal
named what the next attempt needs first: a lossless medium a test can actually
construct. This is that instrument. It found three things before a line of
transport was changed, which is the whole argument for building it first.

**The medium is wired by hand.** `MakeScatteringMedium` builds
`layer(R, layer_vdf(T, anisotropic_vdf))` -- the structure `open_pbr_surface`
generates for a transmissive material -- with the absorption and scattering
coefficients as authored inputs. Going through OpenPBR instead is what made the
reverted attempt unmeasurable: it derives its coefficients from a colour and a
depth, so the test could not state the medium it was testing, and the loss it
reported turned out not to be in the transport at all.

**A `layer` node has to be named, not categorised.** `layer` is two nodedefs
with the same category and the same output type: `ND_layer_bsdf`, whose base is
a BSDF, and `ND_layer_vdf`, whose base is a VDF. Adding one by category resolves
to the first, and connecting a VDF to a base declared BSDF produces a graph that
validates, generates, compiles, and silently layers the surface over a null
closure. A slab enclosing a vacuum read **0.8095** that way -- nineteen per cent
gone -- and the generated code was the only place that said why: it called
`mx_layer_bsdf` where the material meant `mx_layer_vdf`. `AddNodeOfDef`
instantiates a nodedef by name, and the render tests gained the
`HDCLAUDE_DUMP_SHADERS` facility the Hydra compiler already had, because a
material that compiles cleanly and renders wrongly leaves the generated source
as the only witness.

**Two parallel quads are not a container.** With the graph corrected, the slab
enclosing a vacuum read 0.9953 -- exactly the layered dielectric, so the wrapper
costs nothing -- and the same slab enclosing a *lossless* medium read **1.2494**,
rising to 1.2969 when the bounce limit went from 16 to 64 and to 1.3497 at four
times the scattering coefficient. A medium that absorbs nothing multiplies
throughput by exactly one, so none of that is the walk's arithmetic.

It is the geometry. The slab is two quads with open sides. A path that enters the
medium and then scatters sideways leaves through the open edge *still flagged as
being inside the medium*, and walks in an unbounded one until the step cap ends
it. Every furnace in the suite has used those two quads, and for a surface-only
material they are a perfectly good furnace, because a delta refraction never
leaves by the side.

**So the gate moved to a sphere, and on a sphere everything conserves.** A closed
sphere of the same material, same coefficients, reads 1.0001, 0.9963, 0.9960
isotropic; 1.0020 forward; 0.9923 dense; and 1.0023 chromatic at four-to-one
across the channels. The walk is right. The chromatic cases pass for a reason
worth stating plainly rather than for the reason they will pass later: the walk
collapses the scattering coefficient to its mean, and the mean of a lossless
medium is still lossless. They are in place so that they still have to hold when
the lanes carry their own coefficients.

**The sphere is also the first furnace that varies the angle.** Two flat parallel
quads present a delta refraction at one incidence per pixel and never a steep
one, so every furnace written until now was, without intending to be, a
near-normal measurement -- and an energy error that vanishes at normal incidence
had nowhere it could have been caught. A sphere presents every angle at once and
sends a large share of its interior paths past the critical angle, where they are
totally reflected and cross the boundary many more times. The bare
`dielectric_bsdf` reads 0.9980 and 0.9847 across the disc and `layer(R, T)` reads
0.9938 and 0.9833, so the closures hold up at angle. That was worth knowing
independently of the medium, and it is why the gate stays in the suite rather
than being deleted once the medium question is settled.

**Open question: why the unclosed medium *gains*.** The recorded expectation for
a medium that is never closed is that a path escaping it is not attenuated --
which for a medium that absorbs nothing is not an error at all. The measured
behaviour is a gain of a quarter that grows with the bounce limit and with the
scattering coefficient, and that is not explained. Every candidate checked
accounts for none of it: the walk multiplies by one when absorption is zero;
Russian roulette in the walk is unbiased and does not fire when throughput is
one; the environment kernel retires a path it pays out; the step cap loses rather
than gains; and the closures conserve at every angle on the sphere. It is
recorded here unexplained rather than given an explanation that sounds right,
because an explanation is not a measurement. It matters in practice for any asset
whose transmissive geometry is an open shell, which is common, and it should be
chased before a medium feature is built on top of it.


---

## 2026-09-07 -- The sphere was inside out, and what it was hiding

The entry above claims the volumetric walk conserves energy, on the evidence of
a sphere furnace reading 1.0001 isotropic and 1.0023 chromatic. **That claim is
withdrawn.** The sphere it was measured on was wound inside out, and the walk
gains forty per cent.

**The instrument's own defect.** `MakeSphere` emitted its triangles as
`(a, b, a+1)`, which for a UV sphere parameterised as
`(sin t cos p, cos t, sin t sin p)` gives a *inward* facing geometric normal.
The authored normals were the outward positions, so nothing looked wrong: every
closure reads the shading normal and shades a perfectly convincing glass ball.
But which side of an interface a transmission crosses is decided from the
**geometric** normal, and inverting it inverts that test. A path *leaving* the
sphere was recorded as *entering* the medium, and a path entering it was
recorded as leaving. Roughly half of all interior walks then ran outside the
sphere, in an unbounded medium with no boundary ahead of them -- 52 per cent at
the coefficients tested, measured by probing `hitGeometry` at the point the walk
ends.

The old walk handed each of those paths `exp(-sigma_a * 1e30)`, which for a
medium that absorbs nothing is `exp(-0)` and therefore one, and the path then
took the environment in full. In a furnace, a path that escapes early and
collects a uniform environment is indistinguishable from a path that did the
right thing. **So the furnace read one for the wrong reason, and a uniform
environment is structurally blind to this class of error.** That is worth
keeping in mind about every furnace here: it can only catch energy that is
created or destroyed, never light that is collected too early.

**What the corrected sphere shows.** With the winding fixed so paths are
genuinely inside the medium:

| interior | reads |
| --- | --- |
| vacuum, n = 1.5 | 1.0003 |
| isotropic, n = 1.5 | **1.4214** |
| isotropic, n = 1.2 | 0.9442 |
| forward g = 0.8, n = 1.5 | 1.5680 |
| dense, n = 1.5 | 1.3694 |
| isotropic, no reflection lobe on top | 0.6104 |

against the same closures with no medium at all, which read 1.0065 and 0.9900
across the disc for a bare `dielectric_bsdf` and 1.0003 and 0.9894 for
`layer(R, T)`. All of it reproduces on the walk exactly as committed, so none of
it belongs to the spectral work below.

The dependence is on the *interface*, not on the number of scattering events:
the dense medium scatters four times as often as the isotropic one and gains
less, while dropping the index from 1.5 to 1.2 turns a forty per cent gain into
a six per cent loss and taking the reflection lobe off the top turns it into a
thirty-nine per cent loss. What changes with the index is how much of the
interior directions lie beyond the critical angle.

That is a regime nothing here could previously reach. A solid glass sphere's
internal rays cannot exceed the critical angle -- refraction at the entry maps
the whole outside hemisphere into a cone of exactly that half-angle -- so a
sphere of glass never totally internally reflects, and two flat quads never
produce oblique internal directions at all. A scattering interior produces them
in every direction, and it is the first thing in this renderer that does.

So the defect is in how `layer(R, layer_vdf(T, vdf))` accounts for its lobes
when a path is inside and past the critical angle. That much is measurement. The
mechanism is not: the reading that fits is that the base lobe's sampler falls
back to `reflect(-V, H)` when `mx_pt_refract` fails, producing a direction the
*top* lobe then evaluates, so the response comes from one lobe and the density
from the probability of having chosen the other. It is written down as the thing
to check first and not as the answer.

**The gate is not committed.** A furnace committed at a tolerance that passes
1.42 would record the defect as correct, and the rule here is that the furnace
goes in with the fix. `MakeScatteringMedium`, the sphere and the vacuum control
are committed so that it can, and the vacuum control is asserted: the OpenPBR
structure enclosing nothing must read what the layered dielectric reads, which
puts any future failure inside the volume rather than around it.

**Spectral MIS across the lanes is written and not shipped.** The walk it
replaces samples one flight from a fixed control coefficient, which is why the
scattering coefficient had to be collapsed to its mean: any other lane then
carries `exp((control - sigma_lane) * flight)`, which compounds over a walk
until it overflows. The replacement chooses the proposing lane *at random* among
the four and divides by the balance heuristic's average of all four densities,
which makes every weight a ratio of one density to the mean of four and
therefore bounded by the lane count, however far apart the coefficients are. It
reduces exactly to the current walk when the medium is achromatic -- verified on
the GPU by probing the throughput after one weighted collision, which reads
exactly one -- so the achromatic walk becomes the chromatic one's special case
rather than a second path to keep in agreement.

The weights, so it need not be reconstructed by guesswork. With `t` the
sampled flight, `sB` the distance to the boundary, and both coefficients per
lane:

```
proposer  ~ uniform over the four lanes
t         ~ Exp(sigma_s[proposer])
collision : w[j] = sigma_s[j] exp(-sigma_s[j] t)  / mean_k(sigma_s[k] exp(-sigma_s[k] t))
                   * exp(-sigma_a[j] t)
boundary  : w[j] = exp(-sigma_s[j] sB) / mean_k(exp(-sigma_s[k] sB))
                   * exp(-sigma_a[j] sB)
```

Absorption stays outside the ratio and therefore stays analytic, which is the
property the current walk was built around: sampling a collision and weighting
by the single-scattering albedo is the textbook form, but in a medium that only
absorbs it turns a closed-form attenuation into a coin flip that kills the path,
and honey and coloured glass are exactly that medium.

It is not committed because it cannot be gated. Its purpose is that a lossless
chromatic medium renders one and neutral, and no medium renders one at all until
the layering defect above is fixed. Shipping it would change the honey ball with
nothing able to say whether the change was an improvement, which is the same
mistake the reverted subsurface attempt made. It goes in behind the fix, with
the furnace.


---

## 2026-09-08 -- A dielectric seen from the inside

The layering defect the sphere furnace found is fixed, and it was not in the
layering. `mx_dielectric_bsdf` handed the *absolute* index to both sides of the
interface, so a path inside glass was shown the reflectance curve of a path
outside it.

Those two curves agree near normal incidence, which is why this survived every
test written until now. They diverge fast: at 35 degrees of internal incidence
the correct reflectance is 0.0863 and the outside curve gives 0.0431, a factor
of two; past the critical angle the correct value is exactly one and the outside
curve is still about 0.05, a factor of twenty. And a path could not previously
*get* to those angles. Refraction maps the whole outside hemisphere into a cone
of exactly the critical half-angle, so a solid glass sphere's internal rays
never exceed it and a flat slab never produces oblique internal directions at
all. A scattering interior was the first thing in this renderer that ever
totally internally reflected.

**Three things had to move together.**

*The Fresnel.* `mx_fresnel_dielectric` already returns exactly 1.0 when
`eta^2 + cos^2 - 1 < 0`, which is the critical angle written out; it was simply
never given an `eta` below one. Passing the relative index makes it right at
every internal angle. The normal-incidence reflectance does not move, since
`((n-1)/(n+1))^2` is the same for `n` and `1/n`.

*The directional albedo.* `layer` reads `1 - albedo` as the fraction of light
handed to the base, so the albedo has to agree with the Fresnel the response is
computed from. `mx_ggx_dir_albedo(NdotV, alpha, F0, 1.0)` interpolates F0 to F90
on a Schlick-shaped curve, which is the right family from outside and the wrong
one from inside -- it reaches one only at grazing. Below the critical angle it
understates the reflectance, the layer hands the surplus to the transmission
lobe, and the interface passes on more light than it received. On the inside the
same lobe energy is now weighted by the Fresnel actually in force:
`mx_ggx_dir_albedo(NdotV, alpha, 1.0, 1.0)`, the library's own `Ess`, times
`Fv`. For a smooth surface `Ess` is one and this is exactly the reflectance.

*The reflect/refract split.* Its `[0.05, 0.95]` clamp was documented as a guard
against a lobe contributing with zero probability. With a correct Fresnel the
two conditions coincide -- a dielectric's reflectance is never zero, and where
it is one the transmission lobe contributes nothing -- so the clamp can go. The
ceiling was the expensive half: past the critical angle the sampler reflects
every time whatever the split says, so reporting 0.95 divided each of those
samples by a density five per cent smaller than the one that produced it, on
every crossing.

**Asking which side you are on turned out to be the hard part**, and two wrong
answers were shipped and caught before this was right.

The first was the *shading* normal. `entering = dot(N, V) > 0` reads correctly
on a flat test surface and badly on a real one: an interpolated normal tilts
past the horizon near a silhouette, so `dot(N, V) < 0` happens all over the
outside of a perfectly opaque object. The first gallery render put a rim of
total internal reflection around every rounded thing in the subdivision scene.
The geometric normal is the only normal that answers the question, and closures
were not given it; they are now, through the ABI, with a fall back to the
shading normal when a caller supplies none.

That was not enough. The subdivision scene's meshes have *holes*, so their back
faces are genuinely visible, and a ray reaching the back of an opaque surface
arrives from behind while standing in the air. Gating on `scatter_mode` does not
separate those either: the reflection half of a split transmissive interface and
a coat over an opaque substrate are both reflection-only lobes, and the first is
inside glass while the second is not. What separates them is the path's own
history -- it is inside a medium because a transmission event put it there --
and only the integrator knows that. So the shade kernel publishes it, and a
closure reads the inside curve only when the path is inside something.

**What it is measured against.** The closure validation harness gained internal
incidences, which it reaches by taking `viewTheta` past 90 degrees, and the
reflectance is asserted against a closed form written independently of
MaterialX's: 0.0859 against 0.0860 at 35 degrees, 0.0403 against 0.0403 at 14,
0.9980 against 1.0 past the critical angle, and 1.0176 against 1.0 at grazing.
The old code would have read about half the first of those.

The sphere furnace with a lossless scattering interior falls from **1.4214 to
1.0110**, and the gate is now committed and asserted -- isotropic, forward,
dense, and chromatic at four to one across the channels, all within 1.2 per cent
of one and neutral. Two furnaces that already passed improved as well, which is
the corroboration that matters most: the layered dielectric slab from 0.9953 to
0.9978, and the same through `open_pbr_surface` with it.

**The gallery moved exactly where it should.** Sponza, the gold ball, Pixar's
kitchen, the New Zealand height map and the subdivision matrix are byte
identical. The bubblegum ball moves by an RMS of 9.2e-05 and Collective Project
by 0.0016, which is rerouted paths rather than changed shading. Everything with
transmission in it moves properly: the glass ball by 0.065, honey by 0.031, the
OpenPBR playground by 0.073, the chess set by 0.0035. The playground's dark
bottle stops clipping to white and reads as glass, and its bright-pixel count
falls slightly rather than rising, so the extra internal reflection did not cost
noise.


---

## 2026-09-08 -- Spectral MIS, measured and still not shipped

With the dielectric fixed, the chromatic gate finally had something to measure
against, so the spectral walk went in and the gate refused it. This time the
reason is measured rather than guessed, and the remedy is named.

**What was implemented.** Both coefficients per lane; the lane that proposes the
free flight chosen uniformly at random per step; and the density a sample is
divided by taken as the balance heuristic's average over all four lanes that
could have proposed it. Written as ratios of exponentials with the largest
factored out, so a dense medium over a long segment cannot underflow both halves
to zero.

**Two things it got right.** With achromatic coefficients every lane's density
is the same and every weight is exactly one: the sphere furnace read 1.0119
against the achromatic walk's 1.0110, which is the same number. And with a
*thin* chromatic medium -- coefficients 0.4, 0.2, 0.1 across a sphere of radius
one, so a path scatters about once -- it reads **1.0004, 0.9907, 0.9889** and is
converged. The per-event weights are right.

**What it got wrong.** A dense chromatic medium, 4 to 2 to 1, reads 1.0226,
1.0562, 1.0061, and eight times the samples does not tighten it: at 256 samples
the same case read 1.0336, 0.9902, 1.0104. The channels wander instead of
converging, which is a heavy tail rather than noise.

The thin case is what identifies it. Each step's weight is one lane's density
over the mean of four and so is bounded by the lane count -- but a walk
*multiplies* steps, and a product of terms each bounded by four is bounded by
four to the power of the step count. Bounding the step does not bound the path.
That is the same failure the fixed-control form had, arrived at more slowly: the
old version overflowed to NaN in tens of steps, this one merely refuses to
converge.

**The remedy, named rather than attempted.** The balance heuristic has to be
applied to the *whole walk's* density rather than to each step's. That means
choosing the proposing lane once per walk instead of once per step, accumulating
each lane's un-normalised path density along the walk -- in logarithms, which
removes the underflow question entirely -- and forming the weight once at the
end, where it is again a ratio of one density to the mean of four and is
therefore bounded by the lane count over the whole walk. This is what a spectral
path tracer carries as rescaled path probabilities, restricted here to a single
segment between surfaces.

It needs one structural change beyond the arithmetic: Russian roulette inside
the walk currently reads the throughput after every step, and there is no
per-step throughput to read once the weight is only formed at the end. Roulette
has to move to a quantity that is available per step -- the absorption term, or
the proposing lane's own survival -- or out of the walk entirely.

It also leaves compounding *across* surface crossings, since a walk is one
segment and a path in glass crosses many. That is a far smaller number than the
scattering events within a segment, and whether it matters is a question for the
same gate.

**So the gate did its job twice.** It refused an estimator that is unbiased and
whose mean is right -- the three channels of the dense case average 1.011,
exactly what the achromatic walk gives -- because being right in the mean is not
the same as being usable. The previous attempt at this was reverted for having
no test at all; this one is reverted by a test, with the per-event arithmetic
validated, the failure localised to compounding, and the fix specified.


---

## 2026-09-08 -- Three loose ends, one of them a new defect

Claims made earlier in this work, checked.

**The unclosed medium's gain is answered, and it was the dielectric.** The open
slab with a lossless medium in it read 1.2494 against 0.9953 with a vacuum, and
that was written up as an open question because nothing in the transport
accounted for it. With the relative-index fix in place the same slab reads
**1.0007** against 0.9978. The escape through the open edge was never the
problem: a path that leaves an unclosed medium sideways and collects a uniform
environment is contributing exactly what a furnace expects, so it was always
harmless there. The gain came from the same wrong Fresnel curve as everything
else, reaching the slab's own faces through the oblique internal directions only
a scattering interior produces. The open question is closed and the numbers that
recorded it are superseded by these.

That is worth noting as a pattern rather than only as a result. Two separate
symptoms -- a sphere gaining forty per cent and a slab gaining twenty-five --
were one defect, and the slab's version was written up as unexplained for a day
because the instrument that could localise it did not exist yet.

**The glass ball's darkening is the bounce limit, as claimed.** Rendered at 512
pixels and 256 samples with everything else held, the mean over the frame is
0.6859 at 8 bounces, 0.7364 at 16, 0.7508 at 32 and 0.7547 at 64. It converges,
and the converged value is ten per cent above what the gallery shows. Correct
total internal reflection traps light inside the ball for many more crossings,
and at the gallery's eight bounces a large share of those paths run out before
they can leave. The closure is not losing the energy -- the furnaces say so
directly, and the layered slab *improved* to 0.9978 with the same change -- the
path length is.

The gallery contract stays at eight bounces. It is a comparison standard rather
than a beauty pass, every scene has been measured at it, and changing it would
invalidate every baseline to make one asset prettier. Recorded here so nobody
re-derives it from the image.

**New: the renderer produces non-finite samples above eight bounces.** The same
series turned up 208, 115 and 78 non-finite samples in the 16, 32 and 64 bounce
renders of the glass ball, and none at 8. It is **not** the dielectric fix:
rebuilding the closure as it stood before that commit and rendering at 16
bounces gives 103 of them. It has presumably always been there, and the gallery
has never seen it because the contract renders at eight.

That matters more than it looks. `hdClaudeImageDiff` treats a non-finite sample
as a gate failure, so the gate is capable of catching this and has simply never
been pointed at a long enough path. Anything that raises the bounce count --
the interactive contract, and every temporal phase after it -- will walk into
it. The worst-pixel figure climbs with the same series, 2.13 at 16 bounces to
11.39 at 64, so whatever produces the non-finite values is likely the tail of
the same distribution rather than a separate fault.

It is recorded rather than chased here, with the reproduction written down: 512
pixels, 256 samples, `HDCLAUDE_MAX_BOUNCES` at 16 or above, the glass shader
ball, and `hdClaudeImageDiff` against the 8-bounce render to count them.


---

## 2026-09-08 -- Spectral MIS, per walk instead of per step

The estimator refused earlier today now ships, with one change: the lane that
proposes the free flights is chosen **once per walk** rather than once per step.

That is the whole difference, and the earlier measurement is what specified it.
Per-step selection bounds each step's weight by the lane count, but a walk
multiplies steps, so the path's weight is bounded only by the lane count raised
to the step count. Per-walk selection makes the balance heuristic apply to the
whole walk's density, and the weight is again one density over the mean of four
-- bounded by the lane count over the path.

**The density has a closed form, which is what makes this cheap.** For lane j,
the un-normalised probability of the sampled sequence is

```
q_j = sigma_s[j]^collisions * exp(-sigma_s[j] * distance)
```

because a product of exponentials is an exponential of a sum and every collision
contributes one factor of the coefficient. The walk carries two accumulators --
a collision count and a total distance -- and no per-step product at all. It is
evaluated in logarithms with the largest term factored out, so the ratio is
stable however far apart the coefficients are and however long the walk ran.

**Roulette had to move.** It divided the throughput after every step, and the
throughput is no longer updated until the walk ends. It now weighs the
absorption accumulated so far, which is the part of the weight that actually
decays, and its compensation is carried as a scalar and applied at the end. In a
medium that absorbs nothing this leaves the incoming throughput untouched and
roulette never fires, which is right: such a walk has lost nothing and should end
when it reaches a boundary rather than when it gives up.

**It converges, which is the claim the previous version failed.** On the
four-to-one chromatic sphere the channel spread is 3.2 per cent at 256 samples
and 1.4 at 2048, and on the forward-scattering one 3.7 falling to 0.8 -- about
the square root of the sample ratio, which is what honest variance does. The
per-step form sat near five per cent at both. The medium gate is asserted at
1024 samples for the media and 256 for the closure probes, and reads 1.0075,
1.0098, 1.0081 isotropic; 1.0035 forward; 1.0067 dense; 0.9991, 1.0037, 1.0085
chromatic; and 1.0007, 1.0038, 0.9996 chromatic forward.

**Three guards keep the change attributable**, and each is exact rather than a
tolerance.

A path in vacuum draws nothing, because every kernel shares one random stream
per path and a draw taken where it is not needed shifts what every later sampler
in the frame sees.

Whether a medium scatters is read from the *authored* coefficient rather than
the upsampled one. Upsampling a zero coefficient goes through a chroma table and
returns a hair under one before the logarithm, so the resolved `sigma_s` is a
millionth rather than nothing -- enough to make a clear glass look like a
scattering medium to a test on the resolved value.

And an achromatic medium draws nothing either. All four lanes then carry the
same coefficient, so the four techniques are one technique, the average equals
any one of them, and every weight is one whichever lane is named. Choosing lane
zero there is not a fixed control wavelength -- the thing this design exists to
avoid -- it is a choice among identical options, and it is gated on exact
equality because that is the condition under which the claim holds.

**What moved in the gallery, and what did not.** Seven scenes are byte
identical, including honey and the chess set, which the guards recovered: their
media scatter achromatically, so they now render the sequence they always did.
Intel Sponza moves by an RMS of 0.0037 -- its derived coefficient is not exactly
grey, so it does draw -- and the glass ball and the OpenPBR playground move by
0.031 and 0.057, which is where genuinely chromatic scattering lives.

Every per-channel mean is unchanged to four decimal places, in every scene. So
none of the visible movement is a change in what the estimator returns; it is the
same estimator drawing a different sequence. The chromatic transport is correct
and gated, and no asset in this gallery has a medium chromatic enough to show it
as a colour. That is worth saying plainly rather than implying the images
improved.


---

## 2026-09-08 -- The non-finite samples are older and wider than reported

Yesterday's entry said the renderer produces non-finite samples *above eight
bounces*. Both halves of that are wrong, and the correction matters more than
the original observation.

**They are there at eight bounces**, which is the gallery's own contract. The
earlier reading came from an interleaved console: `hdClaudeImageDiff` prints its
measurements on stdout and its non-finite report on stderr, and the report for
the eight-bounce image appeared underneath the sixteen-bounce heading. Comparing
the eight-bounce render against *itself* -- rms exactly zero -- still reports 208
of them. The count then *falls* with depth: 208 at eight, 115 at sixteen, 78 at
thirty-two, 77 at sixty-four.

**They are in every shader ball, not only the transmissive ones.** At 512 pixels
and 256 samples, the gold ball has 2450, honey has 1345, and glass has 208.
Gold is an opaque conductor with no transmission and no medium at all, so
nothing about this belongs to the volumetric work. Switching subdivision off
changes glass's count from 208 to 184, so it is not the refiner either.

**And the gallery gate cannot see any of it.** The gate compares the committed
JPEG against a *display-transformed* JPEG, and the display transform sanitises
non-finite values on the way through -- there is a core test asserting exactly
that. So `hdClaudeImageDiff`'s non-finite check, which is real and works, is
structurally unreachable for the gallery: by the time it sees an image, every
infinity has already been turned into a number. The roadmap's claim that the
gate "fails on ... a non-finite sample" is true of the tool and false of the
pipeline it sits in. That is why this has never fired.

**Where they are.** `hdClaudeImageDiff` now reports the first few positions
rather than only a count, which is what a defect needs and a tally cannot give.
Plotted over the gold ball, all 2448 of them sit on **concave interior surfaces
seen at or past edge-on**: the inside of the base ring, the inside of the small
tab, the underside of the dome's rim. Nowhere else in the frame.

**It is not the closures.** That was the obvious hypothesis and it is wrong. The
closure validation suite gained a grazing sweep -- conductor and dielectric, at
roughness 0.1, 0.4 and 0.8, at 1.4, 1.5, 1.55 radians, at exactly pi/2, and past
it at 1.6 and 2.0 -- and every one of the thirty-six probes reports **zero**
non-finite samples. A closure driven to the exact clamp does not produce one.

The sweep did find something else, small and real: a smooth dielectric seen
exactly edge-on from inside reads an albedo of 1.0219 where the closed form is
exactly one. Under total internal reflection the compensation term is exactly
`1 / Ess`, so a lobe whose true energy is `Ess` comes back as one only insofar
as `mx_ggx_dir_albedo` is right about `Ess` -- and at `NdotV` approaching zero it
is about two per cent out. That is an upstream fit's accuracy at the far end of
its domain rather than a transport defect, and the grazing sweep's energy bound
is set at 1.03 with that named, rather than at a round number chosen to pass.

**Nor is it reproducible from the parts.** A glass sphere with a rect light, a
constant environment and the stand-in sun, at 8, 16, 32 and 64 bounces and 256
samples, produces exactly zero non-finite pixels in all four. That test is
committed, because the reproduction it *fails* to achieve is itself information:
whatever is responsible is not glass, not a light, not the sun, and not path
length.

What is left is what the shader ball has and that scene does not: real geometry
with authored normals, several materials at once, textures, and concave surfaces
that face away from the camera. The overlay says concave-and-edge-on, and the
closure sweep says the closure is fine when driven there directly -- so the next
place to look is what the *integrator* hands a closure at such a point, and
whether some part of the geometric reconstruction degenerates there.


---

## 2026-09-08 -- The non-finite samples were mine, and the scan found the real one

The two entries above claim the renderer produces thousands of non-finite
samples. **It does not.** The renders that contained them were produced by the
wrong command, and every one of the gallery's own linear EXRs is free of them.

`render_claude.bat` forwards its arguments to `usdrecord`, and the gallery
script passed `--colorCorrectionMode disabled` while the probes written to chase
this did not. Without it, usdrecord applies an sRGB transfer function to a
scene-linear AOV on the way out -- and raising a *negative* linear sample to a
fractional power is a NaN. Every non-finite pixel counted in those entries was
made after the renderer had finished, by a transform that should never have run.
Rendering the same scenes with the flag gives zero.

That is embarrassing in a specific and useful way. The investigation was careful
about everything downstream of the measurement -- the closure sweep, the failed
reproduction, the overlay -- and never questioned the *command* that produced
the image being measured. Three separate guards were added inside the film
kernel to find where the value became non-finite, and all three were no-ops that
left the output bit-identical, which was the signal that the renderer was not
where the defect lived. That signal was there two builds before it was read.

The flag is now in `render_claude.bat` rather than in each caller, next to the
`--disableCameraLight` that is there for the same class of reason. The sRGB
transform belongs to the EXR-to-JPEG conversion, which is what
`hdClaudeDisplayTransform` does, and nowhere else. The gallery script no longer
passes it and every gallery image re-renders byte-identical.

**What survives from those entries**, and it is the part that matters: the
gallery gate compares *display-transformed JPEGs*, so it cannot see anything the
display transform normalises -- non-finite values, negatives, or any magnitude
above one. The EXR is the actual rendered data and nothing had ever looked at
it. Also standing: the closure grazing sweep, which found no non-finite value at
any angle and did find a real 1.0219 albedo at exactly edge-on from inside; and
the deep-path render test, which is a reproduction that legitimately finds
nothing.

**So `hdClaudeImageDiff` gained a `--scan` mode**: one image, no baseline, and
it reports the true range, the mean, the non-finite count and the negative
count. Pointed at the gallery's own linear renders it immediately says two
things the old gate could not.

*Negatives are everywhere and are not a defect.* Ten of the eleven scenes have
them -- 24637 in the subdivision matrix, 11362 in the gold ball, 5 in bubblegum.
A spectral renderer resolving to scene-linear sRGB produces a negative component
for any colour outside that gamut, because the XYZ-to-sRGB matrix has negative
coefficients and a saturated spectrum lands outside the primaries. That is the
gamut being honest. The scan reports them and does not fail on them, and the
display transform clamps them where clamping means something.

*And the OpenPBR Playground is genuinely broken.* Its linear render has a mean
of **1.17e19**, a range of **[-1.31e24, 2.18e25]**, and three non-finite samples
-- in a scene whose displayed mean is 0.38. A handful of pixels carry values
twenty-five orders of magnitude too large, the display transform clamps them to
white, and the gate has been passing the scene for as long as it has existed.
The first non-finite sample is at (484, 456).

That is not diagnosed. Two candidates were checked and rejected: the balance
heuristic *is* applied to every analytic light's next-event estimate, and the
stand-in sun -- which is a delta emitter and deliberately carries no MIS weight
-- is not present here at all, since `hdclaude_has_stand_in_sun` requires a stage
with no lights and no dome and the playground has both.

The scan is deliberately **not** wired into the gallery gate yet, because that
scene fails it. A gate committed while a scene fails it either blocks the
gallery or gets loosened until it passes, and the rule here is that a gate goes
in with its fix. It goes in when the playground's magnitudes are understood.


---

## 2026-09-08 -- Where the Playground's magnitudes are, and how old they are

Two questions answered before any hypothesis was allowed: where in the frame,
and since when.

**Where.** `--scan` gained a magnitude histogram and the position of the
brightest sample, because a range alone cannot separate three stray pixels from
a broken region. It is a region: **7970 samples over 1e2, 6709 over 1e4, 1534
over 1e8**, brightest at (534, 402). Plotted, the ones over 1e8 form a dense
cluster on **the dark green bottle** lying on the desk -- the scene's
transmissive object -- with a thin scatter of ordinary fireflies over the rest of
the frame.

**Since when.** Not from any of this week's work, which was the first thing worth
ruling out given how much of the transmissive path has moved. Rebuilding the
walk as it stood before the spectral MIS gives a range to 1.19e23 and 1546
samples over 1e8; rebuilding the closures as they stood before the relative-index
fix gives 8.33e22 and 1323. The defect is older than both.

It is worth recording that both fixes *reduced* it. Before the dielectric work
the scene had 13543 samples over 1e2 and 10630 negatives; it now has 7970 and
5998. Neither change was aimed at this and neither cured it.

So a transmissive OpenPBR material produces radiance twenty-five orders of
magnitude too large, in a gallery scene, at the gallery's own settings, and has
done for a long time. The display transform clamps it to white and the gate
compares the clamped image, which is why nothing has ever said so.

**What has been ruled out so far**, each by checking rather than by argument:
the balance heuristic is applied to every analytic light's next-event estimate;
the stand-in sun, which is a delta emitter carrying no MIS weight by design, is
not in this scene at all, since `hdclaude_has_stand_in_sun` requires a stage with
neither lights nor a dome; and it is not the spectral walk or the dielectric
closure as changed this week. `hdclaude_lane_extinction` clamps a negative
coefficient to zero before the exponential, so an authored `transmission_color`
above one -- which would otherwise make absorption amplify along a path -- cannot
be the mechanism either.

The next thing to establish is which term in the estimator is large, rather than
which object it lands on. The scene bisects easily: it has several lights and
many materials, and rendering it with the bottle's material substituted, or with
the lights removed one at a time, localises the term far faster than reading the
transport does. That is the approach that worked for the medium furnace and the
one that failed, twice, when it was skipped.


---

## 2026-09-08 -- The phase function pointed backwards, and the Playground's cause

Chasing the Playground's magnitudes turned up an unrelated defect on the way,
and localised the magnitudes themselves without yet explaining them.

**The Henyey-Greenstein sampler had its sign inverted.** The formula the
literature states -- and the one hdClaude carried verbatim -- gives the cosine
against `wo`, the direction pointing *back* along the ray, because a phase
function is conventionally written between two directions that both point away
from the vertex. `hdclaude_sample_phase` is handed the direction of *travel* and
returns another one, and built its basis around that, so every sample came out
reversed. A `g` of 0.8 scattered backwards; a `g` of 1, which OpenPBR defines as
fully forward, sent every ray exactly back the way it came.

Negating is the whole correction and it is a correction rather than a
convention: the Henyey-Greenstein density is symmetric under
`(g, cos) -> (-g, -cos)`, so a negated sample of the `wo` form is exactly a
sample of the propagation form with the same `g`.

It is gated by a test that fails on the old code. A slab of purely scattering
medium with a light behind it and blackness elsewhere reads 4.5005 forward
against 2.0637 backward; with the sign inverted it reads 2.0759 and 4.4874 --
the same two numbers, swapped. A furnace could never have caught this, since
both media are lossless and differ only in *where* the light goes, which is
exactly the kind of error the direction has to be asserted for separately.

No gallery image moves, because nothing in it authors an anisotropic medium that
reaches the walk. The Playground's bottle authors
`transmission_scatter_anisotropy` of 1 but no `transmission_scatter` at all, so
its medium never scatters and the phase function is never called for it.

**A second change, principled and inert.** A microfacet lobe is a delta whenever
*either* alpha is degenerate, not only when their average is.
`mx_average_alpha` is the geometric mean of the **clamped** pair, so a zero alpha
in one axis arrives as the epsilon and the mean lands well above it. OpenPBR's
own anisotropy graph produces exactly that -- `alpha_y = (1 - anisotropy) *
alpha_x`, so an anisotropy of one is a zero alpha by definition -- and such a
distribution is a delta in that axis, which next-event estimation cannot
evaluate. The three microfacet closures now test the unclamped minimum. It
changes no image and no measurement here, and is recorded as reasoning rather
than as a fix that was shown to do anything.

**And the Playground's cause is `coat_roughness_anisotropy = 1` on the bottle**,
which is proven by substitution rather than argued: setting it to zero takes the
scene from a mean of 1.17e19 and a range reaching 2.18e25 to a mean of 0.238 and
a range of [-3.99, 84.85], with the three non-finite samples gone and nothing at
all above 1e2. Nothing else about the scene or the renderer changed.

**Why is not yet known, and the obvious explanation is wrong.** The delta fix
above was written for exactly this and had *no effect*: the render is
bit-identical with and without it. The reason is that `coat_roughness` defaults
to zero and the bottle does not author it, so by the anisotropy graph both alphas
are zero whatever the anisotropy is -- and then the anisotropy cannot matter,
which the experiment flatly contradicts. So the model of that graph is wrong
somewhere, and the next step is to read what `open_pbr_surface` actually
generates for this material rather than what its nodegraph appears to say.
`HDCLAUDE_DUMP_SHADERS` exists for that, and `coat_roughness` feeds a second
consumer in the graph besides the anisotropy node, which is the first thing to
look at.

The scan stays unwired from the gallery gate until this is fixed.


---

## 2026-09-08 -- An alpha the estimator can actually evaluate

The Playground's magnitudes are fixed, and the cause is a floor that was set for
the wrong reason.

**The mechanism.** The bottle's coat authors `coat_roughness 0.33` with
`coat_roughness_anisotropy 1`, and OpenPBR's own mapping is
`alpha_y = (1 - anisotropy) * alpha_x`, so its alpha pair is exactly
`(0.154, 0)` -- rough in one axis and a delta in the other, by specification.
That value is legal and the asset is not at fault.

hdClaude clamped such an alpha to `M_FLOAT_EPS`, which is 1e-8. The GGX density
carries `(h.y / alpha_y)^2`, so at that width its value is set almost entirely by
the last bits of the half vector -- and the sampler and the evaluator reach that
vector by different routes, one through the VNDF and one through
`normalize(L + V)`. They disagree by orders of magnitude, `f / pdf` stops
cancelling, and the estimator returns whatever the rounding happened to be. That
is where 2.18e25 came from.

The floor is now 1e-4, and the number comes from float32 rather than from taste:
`h.y` carries an absolute error near 1e-7, so the ratio stays of order one for
any alpha above about 1e-6, and 1e-4 leaves two decades of margin. A lobe that
narrow -- alpha 1e-4 is a perceptual roughness of 0.01 -- is far below anything
this renderer's sampling resolves, and below it the ratio cannot be evaluated at
all. That is a representability limit, not an approximation of the physics, and
it is the honest reason to have a floor at all. Whether a lobe counts as a delta
is decided separately and from the *unclamped* input, so a mirror is still a
mirror.

**Confirmed two ways, which is why it is believed.** Substituting the asset's
anisotropy to zero and raising the floor are entirely unrelated interventions,
and they land on the same numbers: mean 0.2378 against 0.2376, range
[-3.99, 84.848] against [-3.99094, 84.848]. Neither is a tolerance being met;
they agree to the digits noise allows.

The Playground goes from a mean of 1.17e19 and a range reaching 2.18e25 to a
mean of 0.2376 and a range of 84.85, with its three non-finite samples gone.
Every gallery scene now scans clean: **no non-finite samples anywhere**. The
largest remaining value in the gallery is 506 on the chess board, which is a
specular highlight under a bright dome and has nothing over 1e4 behind it.

**So the scan is wired into the gate**, which is what it was waiting for. It
runs on the linear EXR before the display transform, because that is the actual
rendered data and the transform sanitises exactly what the scan is looking for.
The comparison against the committed JPEG stays where it is and stays a
comparison of display images, which is the right thing for a visual baseline;
the two checks answer different questions and now both are asked.

**What moved.** The Playground by an RMS of 0.053, which is the fix. Everything
else moves by 5e-4 or less, or not at all -- Sponza, the chess set, all four
shader balls -- which is paths rerouting on a changed alpha rather than shading
changing, and honey by 0.0046. The Kitchen Set, Collective Project, the
subdivision matrix and the height map are byte identical.

A note on the negatives the scan reports, since they are large and are not
failures: 24637 in the subdivision matrix, 11362 in the gold ball. A spectral
renderer resolving to scene-linear sRGB gives a negative component to any colour
outside that gamut, because the XYZ-to-sRGB matrix has negative coefficients and
a saturated spectrum lands outside the primaries. They are reported because they
are worth knowing about -- they are what becomes a NaN if anything ever raises
them to a fractional power -- and not gated on, because they are the gamut being
honest.

---

## 2026-09-08 -- Subsurface transports, and the glass eye nobody filled

Subsurface is transported. The three pieces the roadmap named -- the extinction
mapping, the publication question, and the entry direction -- all had to move
together, and a fourth turned up in the middle of them.

**The mapping, and both halves of it.** OpenPBR states the relation between what
an asset authors and what a random walk needs, and hdClaude had neither half.

The first half is the colour. MaterialX documents `subsurface_bsdf`'s `color` as
the diffuse reflectivity and OpenPBR as "the observed reflection color of the
subsurface scattering medium" -- the light that comes back *out*. A walk is
parameterised by the fraction of each *collision* that survives, and those are
not the same number or anywhere near it: van de Hulst's relation, which OpenPBR
quotes in closed form, says a medium whose collisions each survive with
probability 0.6 returns about 0.19 of what enters it. Handing `color` to the
walk directly is the mistake that stays invisible until something measures it,
because it makes every subsurface material darker in a way that reads as a
lighting problem. The inversion now lives in
`hdclaude::SubsurfaceSingleScatteringAlbedo`, and it is checked against the
forward relation it inverts rather than against remembered numbers: the round
trip is exact to 4.7e-5 over the whole range at four anisotropies.

The second half is the mean free path, `mu_t = 1/r`, with OpenPBR's own note
that "this may need to be regularized in the limit r -> 0". The bubblegum asset
authors `subsurface_radius (1, 0, 0.068)`, so that limit is not hypothetical.
The regularization is a floor on the *ratio* between channels rather than an
epsilon on the value, and the constant is not a matter of taste: a channel a
factor R denser than the least dense one reaches `hdclaude_lane_extinction` as a
transmittance of `exp(-R)`, and that function clamps at 1e-8, so R = 18.42 is
the largest ratio the spectral mapping can carry at all. Below it nothing
changes, because a medium that dense is in the diffusive regime, where van de
Hulst's reflectance does not depend on the mean free path. An epsilon on the
value would instead have tied the floor to the scene's unit of length.

A radius of zero in *every* channel is not regularized but solved. The light
leaves where it entered, so the closure is exactly a Lambertian of reflectance
`color`, which is the limit rather than a fallback.

**The publication question had a worse answer than expected.** Both
`standard_surface` and `open_pbr_surface` instantiate `subsurface_bsdf`
unconditionally, with `weight` hardwired to one, and gate it downstream with a
`mix` on the authored weight. So every one of those materials ran the closure
and published a medium. The guard in `shade` --
`subsurface_present && !medium_present` -- was meant to stop that, and
`standard_surface` **has no `anisotropic_vdf` anywhere in its graph**, so
`medium_present` was zero for all of them and the guard was true every time.

Every transmissive `standard_surface` in the gallery was therefore filled with
the default subsurface medium: mean free path one, single-scattering albedo one,
a dense lossless scattering interior nobody authored. The Collective Project's
robot has a glass eye face -- `transmission 1`, `transmission_depth 5`,
roughness 0.01 -- and it was rendering as frosted glass. It is a lens now, and
the concentric rings behind it resolve. That defect was found by fixing the
publication rather than by looking for it.

**So the medium travels on the BSDF struct.** A global cannot answer the
question the integrator asks, because the question is not "what did this
material publish" but "what interior does the lobe that carried the path
enclose", and only the combinator that chose that lobe knows. The medium is now
propagated by the same selection that propagates `sampledL` and `isDelta`,
through `mix`, `layer` and `add`, and read from the *sampling* pass -- the
evaluation pass selects nothing and would leave it saying whatever ran last.
`layer_vdf` is the one place it travels the other way: that node is how MaterialX
says a surface encloses an interior, so the interior is the base, and taking the
top wholesale dropped the only thing the base was there to contribute.

The gate for it is that an `open_pbr_surface` slab now authors a dense, strongly
coloured subsurface it never selects, and has to keep reading what the bare
`layer(R, T)` pair reads. A path entering the wrong interior would leave it
opaque and pink.

**The entry direction is a decision, and it is recorded as one.** An
index-matched interface does not deviate a ray, and van de Hulst's relation
assumes an index-matched boundary, so the physically literal entry is straight
through. It was implemented and measured, and it is not what ships. Two reasons.

A straight-through entry is a delta, and a delta has no solid-angle density to
report. Every surface model puts this node inside a `mix`, and a combinator must
report the mixture's density; handing it a one where its sibling reports a real
density is not a mixture of anything. The alternative is a narrow lobe of some
invented width, which is the kind of constant this renderer does not have.

And the cosine entry is the arrangement in which the authored colour is the
colour seen from every direction. Van de Hulst's relation has no angular
argument -- it relates an albedo to the fraction of entering light that returns,
which is what OpenPBR's own energy constraint uses it as -- and a walk entered
along the incident direction returns that fraction only when averaged over the
hemisphere. Measured on a sphere of a 0.6 material in a unit furnace, the
undeviated entry reads **0.5577** and the cosine entry **0.6173**; at 0.2 they
read 0.1684 and 0.2093. Both conserve energy exactly, at 1.0021 and 1.0018, so
this is about where the light goes rather than how much of it there is. What the
cosine boundary costs is the angular structure of subsurface reflection, which
for a medium diffusive enough to look like subsurface scattering is slight, and
the two to four per cent it sits above the authored value.

**The fourth thing, which the colour gate found.** With the mapping and the
publication both right, a magenta material authored (0.6, 0.2, 0.4) rendered
0.5178, 0.1971, 0.4158 -- a seventh short in red with the channels either side
of it moving the other way. Not an energy error; every furnace still read one.

Van de Hulst's relation is steeply nonlinear where subsurface materials live,
and it was being applied to three numbers *before* they became a spectrum. A
nonlinearity applied before a projection does not commute with it, so the walk
was returning a spectrum whose projection was some colour other than the
authored one. The reflectance is now published unconverted and inverted per
wavelength, which makes the walk's reflectance at every wavelength exactly the
authored spectrum, whose projection is exactly the authored colour. The same
material now reads 0.6139, 0.2052, 0.4072 -- matching the achromatic probes at
0.6173 and 0.2093 to within the noise, which is what says the remaining offset
belongs to the boundary and the fit rather than to colour.

Carrying it required the medium to travel as an extinction and a *bounded*
colour rather than as the two coefficients a closure authors. Two coefficients
resolved to wavelengths separately do not keep the ratio between them, and that
ratio is the whole colour of a subsurface material. What the colour means
travels with it, because the two closures that publish an interior describe it
differently: `anisotropic_vdf` by a coefficient pair, `subsurface_bsdf` by the
light that comes back out.

**One conflation had to be undone on the way.** The path state marked a
transmission as being inside a medium with a hardcoded one, so "which side of
the interface am I on" and "is there an interior to transport through" were the
same bit. They are different questions, and a clear dielectric answers them
differently: it encloses nothing and still has an inside, which is the side a
closure reads its relative index from. Separating them is what kept the layered
dielectric furnaces at the numbers they were committed with; conflating them the
other way took `layer(R, T)` from 0.9978 to 0.9955 by quietly undoing the
relative-index fix.

**And the reference distance for a coefficient's spectrum was wrong for
anything dense.** `hdclaude_lane_extinction` upsampled the transmittance over
one *scene unit*, so a coefficient of 30 arrived as a transmittance of 1e-13 and
the clamp turned it into 18.4. Every medium denser than about 18 per unit mapped
to the same one, and a subsurface material, whose mean free paths are
millimetres, is nothing but such media. It is now referred to the medium's own
mean free path, which puts the densest channel at `exp(-1)` and every other
between that and one -- the best-conditioned band the fit has, and at any scene
scale rather than at one privileged unit of length.

**What the gates say.** A closed sphere of unit-albedo subsurface reads 1.0018,
0.9984, 0.9983, which is conservation at every angle at once; the slab with the
bubblegum asset's own zero-radius component reads 1.0109, 1.0157, 1.0139. The
colour gate reads 0.6173 for 0.6, 0.2093 for 0.2, and 0.6139, 0.2052, 0.4072 for
(0.6, 0.2, 0.4). Its three-hundredths tolerance discriminates between everything
it has to: the identity mistake takes 0.6 to 0.19 and the undeviated entry to
0.5577.

The sphere is the body for conservation because it is closed and presents every
angle; the slab is the body for the zero-radius case because the regularized
channel has a mean free path of 0.054, which crosses 0.2 of slab in about
fourteen collisions and a sphere of radius one in some thirteen hundred, against
a walk capped at 256. Its open sides cost nothing at that density.

**What moved in the gallery**, and every one of the five is explicable:

- **Collective Project 001**, RMS 0.0328. The glass eye stops being frosted.
  This is the unauthored medium above, and it is a defect fixed rather than a
  change of look.
- **BubbleGum**, RMS 0.0727 over 57 per cent of its pixels. Subsurface is
  transported for the first time. Red has a mean free path of 0.0325 against a
  green of zero and a blue of 0.0022, so red travels through the thin parts and
  the other two do not, which is why the ball deepens in colour and the thin
  handle lights from within.
- **OpenChessSet**, RMS 0.0148. The stone pieces carry subsurface and now bleed
  like marble.
- **OpenPBR Playground**, RMS 0.0191. The green jar and the purple toy on the
  shelf transport instead of reading as opaque diffuse.
- **Honey**, RMS 0.0351. Its albedo is (1.0, 0.552, 0.229) -- red scatters
  losslessly and blue is absorbed -- and that ratio now survives being resolved
  to wavelengths, so the amber is deeper and less milky.

Sponza, the gold and glass shader balls, the Kitchen Set, the subdivision matrix
and the height map are byte identical, which is the attribution: nothing without
an interior moved. Every scene still scans clean, with no non-finite samples
anywhere.

**What is still not done.** Absorption is not carried on shadow rays. There is
no next-event estimation at a scattering vertex, so a subsurface material is lit
only by what its walk runs into on the way out. A path inside a *clear* interior
that reaches no boundary is still killed as an unclosed medium, which was right
when every transmission was a medium and is now merely conservative -- that is a
separate finding, left alone here so this change stays attributable. And the
walk's 256-collision cap is reached rather than avoided by a material whose mean
free path is small against the object, which biases the exit point and not the
energy.

---

## 2026-09-08 -- The plinth had a label, and one connection was missing

The StandardShaderBall's base and its internal sphere rendered as flat untextured
grey. They should carry lettering -- "Material Preview - 4 cm Grid" -- and the
asset's own `thumbnails/standard_shader_ball_scene.png` shows it.

**Nothing was falling back.** Both prims are bound, resolved off the composed
stage rather than read out of the variant-laden layers:

    .../shader_ball/neutral_objects/base  -> /materials/neutral
    .../shader_ball/neutral_objects/core  -> /materials/neutral

and no input was pruned, no texture failed to load, and no warning was raised on
any of the four shader-ball renders. hdClaude shaded exactly what was wired.

**What was wired.** `neutral`'s `outputs:mtlx:surface` points at a *NodeGraph*
rather than at a shader, and that nodegraph holds two surface shaders. Which one
its `outputs:out` reaches is chosen by a `material_model` variantSet on the
enclosing `materials` Scope -- `standard_surface` or `OpenPBRSurface` -- which
the asset defaults to `OpenPBRSurface`.

The two variants of the same material are not equivalent. Under
`OpenPBRSurface` the whole reachable network is

    emission_color     <- mtlxconvert1 <- mtlximage2   (ND_image_vector3, no file)
    emission_luminance <- mtlxextract1 <- mtlximage2
    specular_weight     = 0.0
    specular_roughness  = 0.0

with no `base_color` connection at all, so `mtlximage1` -- which holds
`@../maps/neutral.ACEScg.exr@` -- is never reached. Under `standard_surface`,
`mtlxstandard_surface1` has `inputs:base_color.connect -> mtlximage1` and the
map arrives.

**Checked rather than argued.** Flipping `material_model` to `standard_surface`
on the unmodified asset, with no override of any kind, reaches `mtlximage1` and
the EXR; everything else about the network is the same, down to the emission
wiring and the zero specular. That is what says the OpenPBR variant is short
exactly one connection rather than the material being intended flat, and it is
also what says the override below reproduces the asset's own intent instead of
inventing a look.

The sibling materials say it from the other side: `sss_bars` and `uvgrid` wire
`base_color -> mtlximage1` on *both* of their surface shaders, so both of their
variants are complete. Only `neutral`'s OpenPBR one is not. And a third
representation, `outputs:surface -> usdpreview/usdpreviewsurface1`, reads the map
into `diffuseColor`, so the `mtlx` context under the asset's default selection is
the only one that loses the lettering.

**Fixed by override, not by editing, and not by flipping the variant.** The four
`gallery/shader_ball_*.usda` entrypoints already sublayer the asset, so each now
authors the one missing connection as a local `over` on
`materials/neutral/mtlx/open_pbr_surface1`. The vendored asset is untouched and
removing the four overrides reproduces it as published.

Selecting `standard_surface` would also produce the lettering and is the wrong
lever. That variantSet sits on the `materials` Scope, so it would move `neutral`,
`sss_bars` and `uvgrid` to a different surface model while the example material,
the box and the walls stayed on OpenPBR -- there are three separate
`material_model` variantSets in this asset and all three default to
`OpenPBRSurface`. These scenes exist to render the asset in the configuration it
publishes. The override keeps that configuration and repairs the connection;
flipping the variant would route around the defect and hide it.

Raised with the asset's authors; the finding is written up in gallery.md beside
the images so it travels with them.

**What it changes.** All four shader balls, by an RMS of 0.17 to 0.20 over a
quarter to two fifths of their pixels, which is a base and an inner sphere going
from white to the map's neutral grey and gaining their lettering. The glass ball
is the one that gains the most: the refracted image through the shell was a blank
grey and is now the legible plinth and the "SUB-SURFACE BAR" text, which is
most of what a glass test image is for. The bubblegum ball stops being lit by a
white plinth bouncing pink light back into it.

**What was checked and left alone.** `mtlximage2` still has no `inputs:file` and
is still wired to both emission inputs. hdClaude reads such a node's default
rather than the magenta placeholder -- a node with no file is an ordinary
authored value and not a broken asset reference, which is the decision of
2026-09-06 -- so it contributes nothing and warns about nothing. That is the
asset's business to finish, and it is recorded rather than patched.

The map is named `.ACEScg.exr` and the asset declares no `colorSpace` on the
attribute, so it is read as scene-linear like every other EXR in this gallery,
including `ground.ACEScg.exr` and `sss_bars.ACEScg.exr` which were already in
use. Honouring ACEScg primaries would need colour management this renderer does
not have, and this change neither introduces that gap nor widens it.

---

## 2026-09-08 -- UDIM, and the tile everything was being shaded with

`<UDIM>` is a token USD leaves in a texture path for the renderer to expand into
one image per tile, chosen by which unit square of UV space a sample lands in:
tile `1001 + floor(u) + 10 * floor(v)`. hdClaude expanded it to the set's first
*existing* tile and shaded every tile with that one image.

For the many assets that ship a single tile that is right, and it stayed right
for the whole gallery, which is why it survived. ALab's `electronics_turntable01`
is the case it is wrong for, and it is wrong on a scale worth writing down:

    mainBody_M_geo        [1001]      2 of 22 meshes are on tile 1001
    tunrtableTop_M_geo    [1002]
    screwsBody_M_geo      [1003]      ... and 19 are not
    interiorMech_M_geo    [1004]
    turntableIcon_M_geo   [1005]
    rubberFeet_M_geo      [1006]
    disc_M_geo            [1007]
    discPin_M_geo         [1002, 1013]

    UV samples: 1001 46.5%, 1003 33.1%, 1007 9.8%, 1002 6.7%, rest 4%
    -> 53.5 per cent of the asset was shaded with tile 1001's image

Its wooden body looked right and everything above it -- platter, tonearm,
controls, disc -- was smeared with pieces of the body's wood-and-label map.

**Selection has to be per sample.** `discPin_M_geo` straddles tiles 1002 and 1013
by itself, so choosing a tile per mesh, or per material, would still be wrong on
that one prim. The tile comes from the texture coordinate of the sample being
shaded and from nothing else.

**Which meant a filename could no longer be a sampler.** MaterialX's GLSL syntax
makes `filename` a `sampler2D`, and hdClaude's generator turned each one into an
index in a shared array with `#define <name> hdclaude_textures[i]`, which let the
stock `mx_image_*` bodies work unchanged -- `texture(name, uv)` still expanded to
a sampler expression. A sampler carries one image and cannot express a choice
among many, so `filename` is now a small struct instead:

    struct HdclaudeTexture {
        int slot;        // first index into hdclaude_textures
        int tileOffset;  // into hdclaude_udim_tiles
        int tileCount;   // 1 when the image is not a UDIM set
    };

A set takes one array slot per tile, contiguously and in ascending tile order,
and the material declares a flat `hdclaude_udim_tiles` table that the handle
indexes into. One table for the whole material rather than one per image,
because a handle can carry an offset and cannot carry an array.

It is registered as a **scalar** type syntax and not an aggregate one, which is
not a detail. An aggregate type is one MaterialX will *construct*: it emitted
`HdclaudeTexture(orientation_file)` where the stock sampler type emits
`orientation_file`, and a one-argument constructor for a three-field struct does
not compile. A filename is passed along, never built.

**The six `image` nodes are overridden**, in a new `mtlx/stdlib/genglsl_pt`
beside the pbrlib set, for the body alone. Unlike the pbrlib overrides this set
is not all-or-nothing -- no stdlib GLSL file includes a header hdClaude replaces
-- but it is complete anyway, because a `HdclaudeTexture` handed to a `sampler2D`
parameter does not compile, which is the right way to find out.

`HwImageNode` had to be registered under the genglsl_pt implementation names,
and that is not a formality either. It *adds* the `uv_scale` and `uv_offset`
inputs that the `image` nodedef does not declare and the GLSL body takes, so the
plain source-code node MaterialX falls back to emits an eleven-argument call
against a thirteen-parameter function. The symptom is a compile error naming
neither.

**The tiles are asked for, not assumed.** The material compiler resolves the set
before generation -- the tile count decides how many slots the set takes, so it
has to be known first -- by asking the asset resolver for each of the hundred
grid positions. A set is not required to be contiguous or to start at 1001: the
turntable jumps from 1007 to 1013, and the OpenPBR playground's tools start at
1003, which is what made hard-coding 1001 look like a missing TIFF decoder back
in June.

**A sample on a tile the set does not ship reads the node's `default`.** Not the
nearest tile, not the first one. There is no image there, and substituting one is
the same class of mistake as the behaviour being replaced.

**The loader now refuses an unexpanded token** rather than collapsing it. Tile
selection lives in the compiler and the shader, so a `<UDIM>` reaching the loader
means the expansion did not happen, and guessing would hide it. A set of one tile
is substituted too, so no legitimate path leaves a token behind.

**The gate is a quad that spans six tiles and a set that ships three.** Tiles
1001, 1002 and 1012 are solid red, green and blue; 1003, 1011 and 1013 are
absent. It reads

    udim 1001 0.58 -0.00 0.00, 1002 -0.04 0.62 0.01, 1012 -0.00 -0.00 0.71
    udim absent tiles: 1003 0.000, 1011 0.000, 1013 0.000 (expected 0)

Under the old behaviour all six squares would have been red. The set is
deliberately neither contiguous nor a rectangle, because a lookup that assumed
`first + offset` would pass on a contiguous set and fail on ALab's.

**What moved.** The OpenPBR Playground by an RMS of 0.0322 over 32 per cent of
its pixels, which is 85 UDIM textures each finding the tile it belongs to: the
rolled mat on the shelf goes from grey to teal, the toy beside it gains its
colour, the jar gains its label, the books on the shelf separate. The chess set
moved by an RMS of 0.00034 with a worst pixel of 0.34 over eight pixels in a
million, and Collective Project 001 by 0.00022 -- neither has a UDIM set, and
both are a firefly landing differently because the generated code changed shape.
Everything else is byte identical.

ALab is not in the gallery: this asset is one of many in a layout that will be
added later, and it was rendered here to find the defect rather than to record
it.

---

## 2026-09-08 -- The DLSS build had never been run, and did not work

`HDCLAUDE_ENABLE_DLSS` has been OFF since it was written on 2026-09-05, and
`compile.bat dev-dlss` had never been invoked. The first invocation found two
defects in `cmake/NvidiaDLSS.cmake`, neither of which could have been found any
other way, and the second of which the first was hiding.

**The SDK was fetched and then declared unobtainable.** The clone succeeds --
`HEAD is now at a8ed84e DLSS Super Resolution and Ray Reconstruction SDK
310.3.0` -- and the very next message is

    CMake Warning: hdClaude: the NVIDIA DLSS SDK could not be obtained.

`FetchContent_Populate` sets `dlss_SOURCE_DIR` in its *calling* scope, and the
call was wrapped in a `block()`, which is a new variable scope. So the variable
was empty everywhere below it and the check asked whether
`/include/nvsdk_ngx_vk.h` existed, which it never does. The comment called the
wrapper "an error guard so an unavailable or declined SDK turns the feature off
rather than stopping the configure"; `block()` scopes variables and catches
nothing, so it was not a guard, and the only thing it did was break the check.

The downgrade path is the right behaviour -- DLSS must never be a requirement of
the default build -- which is exactly why it was able to hide this. A feature
that turns itself off quietly cannot tell you it turned itself off for the wrong
reason.

**And the library path named a directory the SDK has never had.** With the
variable fixed, the link would have gone to
`lib/Windows_x86_64/x86_64/nvsdk_ngx_s.lib`. What the SDK ships is

    lib/Windows_x86_64/  dev  khr  rel  uwp  vs2010  vs2012  vs2013  x64

so the modern MSVC import library is in `x64`, and the `vsNNNN` directories stop
at vs2013. The `_dbg` variants are built against the debug CRT, so the choice now
follows the configuration rather than being fixed at the release one.

A missing library is now a **fatal error** rather than a downgrade. The two cases
are different and were being treated the same: an SDK that cannot be obtained is
a fact about the machine and turning the feature off is right, while an SDK that
is present with headers and no library is a fact about this file being wrong
about its layout, and quietly turning the feature off there is how the first
defect survived four days.

`HDCLAUDE_DLSS_RUNTIME_DIR` now points at `lib/Windows_x86_64/rel`, where
`nvngx_dlss.dll` and `nvngx_dlssd.dll` live. Nothing is copied out of the fetched
tree: redistribution of those is governed by NVIDIA's licence and hdClaude ships
none of them. `dev` holds the variants that write an overlay and a log, which is
what to point NGX at when a feature refuses to initialise.

`compile.bat dev-dlss` now reports `NVIDIA DLSS ........ ON` and completes. That
is the configure and the fetch proven, and no more than that: nothing links
`hdClaudeNgx` yet, so the link itself is still unexercised. The support query of
phase 12 is what will exercise it.

---

## 2026-09-08 -- NGX answers, and three more things had to be right first

The DLSS support query works, and this machine says:

    DLSS SDK ....... compiled in
    NGX extensions . 1 instance, 3 device
    DLSS ........... available
                     super resolution yes, ray reconstruction yes
    validation ..... 0 errors, 0 warnings

That is phase 12's subject: NGX initialises against hdClaude's Vulkan context,
hands over its capability parameters, and is shut down again, leaving nothing
running. Getting there took three corrections, and each was invisible until the
step before it was right.

**The library was the static-CRT one.** `cmake/NvidiaDLSS.cmake` linked
`nvsdk_ngx_s.lib`, and the first thing that actually linked NGX got

    nvsdk_ngx_s.lib(...) : error LNK2038: mismatch detected for 'RuntimeLibrary':
    value 'MT_StaticRelease' doesn't match value 'MD_DynamicRelease'

`_s` and `_d` are a CRT choice, not a debug one, and the naming does not say so.
Read out of the libraries rather than guessed: `nvsdk_ngx_d.lib` carries MSVCRT
default-lib directives and `nvsdk_ngx_s.lib` carries LIBCMT, so `_d` is the
dynamic-CRT build. hdClaude and OpenUSD are both `/MD`. This is the third defect
in that file and the second that could only surface once something linked.

**NGX needs its extensions enabled, which is what the provider is for.**
`VulkanRequirementProvider` was already designed and in `vulkan_context.h` --
"instance extensions must be known before instance creation, and device
extensions and features before *device selection*" -- and had no implementations.
`NVSDK_NGX_VULKAN_RequiredExtensions` names one instance extension and four
device ones, and without them `NVSDK_NGX_VULKAN_Init_with_ProjectID` answers
`FAIL_InvalidParameter`. So the test installs the provider before constructing
the context, which is the arrangement the design specifies and the first thing
to use it.

**One of the four cannot be enabled.** NGX asks for
`VK_EXT_buffer_device_address`, which Vulkan 1.2 promoted to
`VkPhysicalDeviceVulkan12Features::bufferDeviceAddress`. hdClaude enables the
core feature because ray query and the acceleration structures need it, and the
specification forbids both at once. The validation layer says it outright:

    vkCreateDevice(): pNext chain includes VkPhysicalDeviceVulkan12Features with
    bufferDeviceAddress set to VK_TRUE and ppEnabledExtensionNames contains
    VK_EXT_buffer_device_address

The provider drops it. That is not dropping a requirement: the capability NGX
wants is present either way, and NGX names the extension because its list was
written against a Vulkan that had no core form. Worth noting that the validation
gate found this on the first run -- the same gate that exists because hdCodex
shipped a frame whose counter reset raced the copy that read it.

**And the project ID has to be a UUID.** `NVSDK_NGX_VULKAN_Init_with_ProjectID`
with `NVSDK_NGX_ENGINE_TYPE_CUSTOM` validates the string, and answers
`FAIL_InvalidParameter` for a name -- indistinguishable from every other way that
call can be wrong, which is why it survived the extension fix. It is a fixed
UUID for hdClaude now, with a comment saying why it is not the readable
identifier it looks like it should be.

**What the boundary looks like.** `include/hdclaude/gpu/reconstruction.h` names
no NVIDIA type: a `ReconstructionSupport` that says what is possible and why not,
and an `NgxRequirementProvider`. `src/gpu/ngx_support.cpp` is the only
translation unit that includes an NGX header, its SDK-dependent half is inside
one `#if HDCLAUDE_HAS_DLSS`, and the stubs below it are what the default build
compiles. `hdClaudeNgx` is linked PRIVATE to `hdClaudeGpu`, so nothing above that
library can see the SDK even by accident.

What is deliberately absent is the `ReconstructionBackend` interface itself --
Resize, Evaluate, ResetHistory. Those describe work phases 10 and 11 have not
produced anything for, and declaring them now would leave three unimplemented
virtuals standing in for a phase that is not done. They land with the native
backend.

**The gate is in two halves and neither asserts DLSS is available.** A test that
demanded it would fail on every machine without an NVIDIA card, which is exactly
the coupling the renderer-neutral boundary exists to prevent. What it asserts is
that the answer is self-consistent -- available implies no reason given,
unavailable implies one -- that a build with no SDK contributes no extensions and
claims no support, and that installing the provider on any device leaves
validation clean. The availability itself is printed, because on this machine the
answer is the finding.

Both configurations pass all seven suites: `dev` reports
`NVIDIA DLSS ........ OFF` and the query answers "this build has no NVIDIA DLSS
SDK", `dev-dlss` reports ON and answers with the capabilities above. A
`dev-dlss` test preset now exists, which it did not; the configure preset had
one and there was no way to run the suite against it.

---

## 2026-09-08 -- One entry point, and a frame that knows what it is of

Phase 9's first step: `BeginFrame(FrameDescription)` / `EndFrame(FrameHandle)`
replaces `Render(width, height, camera, settings)` as the entry point. Nothing
overlaps yet -- the trace still happens inside `BeginFrame` and `EndFrame` hands
back what it produced -- so the phase gate stays unmet. What lands is the shape
the rest of the phase needs, and two properties that are worth having on their
own.

**The invalidation decision is made once.** `FrameDescription` carries
everything that could invalidate anything -- extents, camera, settings, mode and
scene revision -- and `InvalidateFor` is the only thing that reads it. The
design has said so since 2026-09-05 and the reason is a lesson rather than a
preference: hdCodex's `SetScene()` / `SetShadingMode()` /
implicit-resize-inside-`Trace()` triad each had to guess what the others implied,
each guessed wrong differently, and that triad is the direct cause of four
shipped defects.

The caller's `resetAccumulation` is now an *input* rather than the answer. It
says what the caller believes; the renderer decides what it knows -- a changed
resolution, a changed mode, a changed scene revision -- and a caller that forgets
one of them gets a correct frame anyway. That asymmetry is the whole point, and
it is what the gate tests: every case asks for `resetAccumulation = false` and is
right to be overruled.

**A frame carries what it is a frame of.** `FrameResult` holds the extents it was
rendered at, its own index, and the sample range it added. Nothing re-derives
those from the renderer's current extents, which is exactly what hdCodex did and
why a host that resized between submitting a frame and receiving it wrote an
image of one size into a buffer of another with nothing to say so -- findings A2,
A1, N1 and N8.

The render pass now checks the returned extents against the buffer it is about to
write and drops the frame if they disagree. That check cannot fire today, because
nothing overlaps and the frame is finished before the line runs. It is there now
so that it is already right when something does, which is the opposite of how the
four findings above happened.

`EndFrame` on a handle whose frame has already been taken returns an invalid
result rather than the previous frame's. Handing back a stale image is the
failure this shape exists to prevent, so it must not be reachable by asking
twice either -- and the gate asks twice.

**`Render()` stays**, implemented as `EndFrame(BeginFrame(...))`, because every
test and the whole gallery use it and none of them is interested in frame
identity. It names no scene revision and no mode, so the invalidation reduces to
the resize it always did, which is what keeps it exactly the function it was. It
cannot overlap anything by construction, so it will stay a convenience rather
than becoming the interactive path.

**Every gallery scene re-renders byte identical**, all eleven at RMS 0, and the
suite passes at 218 checks. For a change that moves an entry point that is the
property worth having: nothing about what the renderer computes moved, so
anything that does move later is attributable to the step that moved it.

What is left of phase 9 is the part the gate is about. Every submit still goes
through `SubmitImmediate` and is waited on before the call returns, so there is
one frame in flight and no overlap to measure. Per-slot resources come next, then
the submit moves into `BeginFrame` and the wait into `EndFrame`, and only then
can GPU timestamps show frame N+1's trace overlapping frame N's readback across a
queue -- which is the claim hdCodex could not make.

---

## 2026-09-08 -- The renderer does not always reproduce its own output

Phase 9's second step was going to be batching a whole sample into one command
buffer. It is not, and the reason is worth more than the batching would have
been: **this renderer is intermittently nondeterministic, and the gallery gate
has been absorbing it.**

**What the frame looks like today.** Every kernel dispatch goes through
`SubmitImmediate`, which allocates a command buffer and a fence, submits, and
waits for the device before returning. A frame is one submit per bounce plus one
for the film -- at the gallery's 32 samples and 8 bounces, **288 full device
drains per `Render` call**, none of which the host needs, because nothing between
two bounces is read by it.

One thing stood in the way: `bounce` was a field of the frame uniform, and the
uniform is written by the *host*, so every dispatch in a batched buffer would
have read whichever value was written last. It has only four readers -- one in
`raygen` (which is `sampleIndex`, not `bounce`), one in `environment`, two in
`shade` -- so it moved to a push constant on those two kernels. That change is
in, and on its own it is worth having: the value belongs where it varies.

Batching then took the render suite from **243 s to 107 s** with all seven suites
passing, synchronisation validation clean, and Intel Sponza byte identical. The
Open Chess Set moved by an RMS of 0.0018, which looked like a missing barrier the
submit boundaries had been hiding.

**It was not.** Bisecting said so first -- with the push constants kept and the
per-bounce submit restored, the chess set was byte identical, so the difference
belonged to the batching. Then the same build, unchanged, rendered a full gallery
in which four scenes moved by between 3.7e-5 and 9.4e-4: Sponza, the chess set,
Collective Project 001, and the **New Zealand height map**, which is one textured
quad. That last one is what gave it away. Nothing about a push constant for
`bounce` can move a single quad by a ten-thousandth, and nothing about batching
can either, because that run was not batched.

So the height map was rendered twice more, with the same binary and against its
committed baseline:

    rms 0 (limit 0.01), worst 0 (limit 0.3), failed 0% (limit 4)
    rms 0 (limit 0.01), worst 0 (limit 0.3), failed 0% (limit 4)

Identical, twice, to the baseline it had differed from twenty minutes earlier.
Two back-to-back renders of the same scene also compare bit identical to each
other. The renderer usually reproduces itself and sometimes does not.

**What that costs.** Every "byte identical" in this record is weaker than it
reads. The gate's RMS limit is 0.01 and this noise is two orders below it, so it
has never failed and never will; what it does instead is make a genuine change of
that size indistinguishable from nothing having happened. The chess set's 0.0018
under batching is exactly in that band, which is why it cannot be attributed
either way.

**And it blocks phase 9.** The remaining steps -- per-slot resources, then real
overlap -- are precisely the changes whose correctness argument is "the image did
not move". A renderer that cannot reproduce its own output cannot support that
argument, so determinism has to come first. Batching is reverted rather than
shipped on an unverifiable claim.

**Where to look.** The film takes no atomics by invariant: paths map one-to-one
onto pixels, so exactly one invocation writes each entry. What is not
deterministic is the *order* paths take in the compacted queue --
`atomicAdd(counters.nextActiveCount, 1u)` -- and therefore their order through
the material sort. Each path still writes its own pixel, so order alone should
not change a result, which is why this needs an instrument rather than another
argument: a test that renders the same scene twice in one process and compares
the linear films, run enough times to catch something that is usually invisible.
That is the next task, ahead of anything else in phase 9.

What ships from this session is the push-constant move and the finding. The
batching is written up here so the next attempt starts from what was measured --
a 2.3x reduction in the render suite, sync validation clean -- rather than from
scratch.

---

## 2026-09-08 -- The determinism instrument, and what it ruled out

The nondeterminism found earlier now has an instrument, a reproduction, and four
eliminated causes. It does not yet have a cause.

**The gate that ships.** `tests/render_tests.cpp` renders one scene
progressively -- four chunks of eight samples, accumulating, which is what the
gallery does -- and compares the resolved films **exactly**, six times over. Not
"near": two runs of the same arithmetic on the same inputs have no tolerance to
be within, and a tolerance here would rebuild the blindness the gate exists to
remove. It compares films rather than display images for the same reason the
non-finite scan does: a display transform normalises exactly what the check is
looking for.

A second phase republishes the scene between renders, which rebuilds the
acceleration structures. A GPU builder is under no obligation to produce the same
tree twice, and a different tree should still give the same closest hit -- the
nearest intersection along a ray is unique whatever order a traversal finds it
in. This asks whether that holds in practice, and it is the discriminator the
first phase cannot be.

Both read zero:

    determinism: 0 of 6 repeats differ from the first; worst 0.000e+00
    determinism across a scene rebuild: 0 of 6 differ; worst 0.000e+00

**The reproduction is across processes, and it is solid.** Three separate
`usdrecord` runs of the Open Chess Set at gallery settings produce three
different linear films:

    run A against run B:  rms 0.00131, worst 1.218, 0.0056% of pixels
    run A against run C:  rms 0.00054, worst 0.314, 0.0075% of pixels

The worst pixels are firefly-scale, which is what a path taking a different route
looks like, and the affected fraction is small. This is the thing the gallery
gate cannot see: its RMS limit is 0.01 and its comparison is of display images.

**Four things are ruled out.**

*The scene store's ordering.* `_meshes`, `_materials` and `_lights` are
`std::map<SdfPath, ...>`, so a snapshot's order is fixed by path and identical in
every process. This mattered most for lights: next-event estimation selects an
emitter by index, so a varying light order would change which light a given
random draw picks, at exactly this magnitude. It does not vary.

*The generated MaterialX.* Two processes were made to dump every generated
program for the chess set. All sixteen are byte identical, so neither MaterialX's
own container iteration nor hdClaude's generation contributes.

*Repetition within a process.* Six repeats, exact comparison, zero differences.

*Rebuilding the acceleration structure within a process.* Six rebuilds, zero
differences.

**And one contrast narrows it further.** The New Zealand height map -- one quad,
one texture, subdivided at level 6 -- was among the scenes that moved in the
gallery run that started this. Rendered three times in three processes it is
**identical every time**, and it matched its committed baseline on two separate
occasions after differing from it once. So on that scene the effect is rare;
on the chess set it happens between every pair of runs.

What separates them is what to look at next: the chess set has a `PointInstancer`,
a dome light with an HDRI and therefore an environment CDF, thirty-two instances
and many materials; the height map has none of those. The environment
distribution is the most interesting of them, because it is built per process
from a texture and sampled by every miss.

**Why this is worth the trouble.** Every "byte identical" in this record is an
argument about a change, and this is the noise floor those arguments sit on. It
is two orders below the gallery gate's limit, so nothing has ever failed on it
and nothing will; what it does instead is make a real change of that size
indistinguishable from no change at all. Phase 9's remaining steps are exactly
such changes, which is why this comes first.

---

## 2026-09-08 -- The seam UDIM selection introduced

Choosing a tile per sample left a defect behind, and it was one the change itself
created: hdClaude binds a single sampler for every texture and it addresses
`REPEAT`.

That is right for an ordinary map and wrong for a tile. A tile is a whole image,
and a bilinear fetch just inside its right edge reaches past that edge and comes
back with the *left* edge of the same tile -- so a one-texel band of the wrong
thing runs down every boundary where two tiles meet. Before tile selection
existed the question could not arise, because every tile sampled the same image
at whatever coordinate it was given.

The fix is to hold the coordinate half a texel inside the tile, which is what
`CLAMP_TO_EDGE` would do, taken from the texture's own size rather than needing a
second sampler bound over the same images:

    vec2 texels = vec2(textureSize(hdclaude_textures[slot], 0));
    uv = clamp(uv, 0.5 / texels, 1.0 - 0.5 / texels);

It costs a `textureSize` on the UDIM path and nothing anywhere else.

**What it does not do is filter across tiles.** A tile's neighbour's texels are
not in it, so a set whose tiles are meant to be continuous still shows a
texel-scale discontinuity at the join. Removing that needs border texels
replicated from the neighbour at load time, which is a different piece of work
and is recorded rather than guessed at.

**The gate took three attempts, and the first two are the interesting part.**

The first asserted a three-pixel window near the seam against the tile's middle,
and failed with the fix in place. It was measuring the wrong thing: the quad puts
about twenty pixels across a tile, so a window wide enough to average noise is
also wide enough to reach into the *next* tile -- an effect that has nothing to
do with wrapping and would fail whether the seam were fixed or not.

The second used single pixels and compared the seam against the tile's middle. It
discriminated, but by 0.89 to a threshold of 0.90. A gate whose failing and
passing cases are a hundredth apart is a coin toss, and the reason was
illumination: the quad is lit, so two points two thirds of a tile apart differ by
more than the artefact does.

The third compares two points *one texel apart* -- 0.875, the last texel's
centre, against 0.96, past it -- which a clamp resolves to the same texel and a
wrap does not, and between which the falloff is negligible. It reads 0.546
against 0.528 with the fix and 0.352 against 0.525 without: a ratio of 1.03
against 0.67, either side of a 0.85 threshold.

The tiles are four texels across, deliberately coarse. The band a wrap corrupts
is one texel wide, so at this frame's resolution an eight-texel tile puts the
whole artefact inside two pixels and a finer one hides it completely.

And it was run with the fix reverted, twice, because a gate nobody has seen fail
is not a gate -- which is the rule that exists here because hdCodex's validation
gate passed vacuously for want of a layer.

**And one clue for the determinism hunt, from the gallery run that followed.**
The New Zealand height map moved against its committed baseline by an RMS of
**3.72045e-05** -- the same figure, to every digit it prints, that it produced in
the run which started that investigation, under a different build. Two different
binaries do not agree to six digits by chance.

So the effect is not a spread of random outcomes. It looks bimodal: the renderer
produces one of two results for that scene, and the other one differs from the
baseline by exactly that. A race with two stable outcomes -- an ordering that
resolves one way or the other -- fits, where accumulated floating-point noise
does not.

That is worth more than another elimination, because it says what to look for: a
binary choice made once per process, not a drift. The gallery baselines from this
run were therefore not adopted. The seam fix does change generated code and could
legitimately move a scene by this much, and while the two cannot be told apart
there is nothing to be gained by writing either of them down.

---

## 2026-09-09 -- What a render costs, stage by stage

The gallery recorded a wall time per scene, which says a scene is slow and
nothing about where. It now records what each stage cost, and the figures come
from the renderer rather than from something measuring it from outside: only the
renderer knows what it is holding, and only it knows the moment a frame holds all
of it at once.

The Open Chess Set, and what four and a half of its twenty-five seconds are:

    ingest:   342 ms snapshot, 978 ms publish (49 instances, 23,023,360 triangles)
    subdiv:   4472 ms over 21 meshes, 1,197,990 -> 19,206,066 points
    shading:  2013 ms over 22 materials, 1445 ms over 42 textures (672.0 MiB)
    rays:     640,679,936 from the camera
    memory:   1.7 GiB on the device at the peak, 10.9 GiB free

Nothing said before that refinement was the largest single cost of that scene, or
that level 2 turns 1.2 million control points into 19.2 million.

**And one scene is 150 MiB from falling over.** The OpenPBR Playground:

    ingest:   4013 ms snapshot, 7911 ms publish (331 instances, 49,028,799 triangles)
    subdiv:   25147 ms over 339 meshes, 4,558,935 -> 77,636,880 points
    shading:  6759 ms over 54 materials, 25421 ms over 154 textures (8.3 GiB)
    memory:   12.4 GiB on the device at the peak, 149.9 MiB free

That is on a card with sixteen gigabytes. The scene reaches 12.4 GiB and leaves
149.9 MiB of device-local memory unused, so it does not fit on anything smaller
and is one texture away from not fitting here. It has been the most expensive
scene in the gallery since it was added and the reason was assumed to be its
sample count; it is 8.3 GiB of texture and 77.6 million refined points. That is
worth knowing before the ALab layout scenes arrive, and it is exactly the kind of
fact a wall time cannot carry.

**Where each figure comes from.** `HdClaudeStageStats` is a struct of atomics
owned by the delegate and reached by the adapters through the render param.
Atomics because Hydra syncs prims in parallel: refinement and material
compilation both happen on worker threads, and a plain `+=` from several of them
is a race that makes a diagnostic quietly wrong, which is worse than not having
one. Times accumulate across threads, so a stage's figure is the *total work*
done in it rather than the wall time it occupied -- a refinement total larger
than the frame's wall time is threads working at once, not an error, and the
header says so where somebody reading the number will find it.

Textures are timed around `Acquire` and counted only when the pool actually grew.
The pool shares an image between every material that names it, so a second ask
costs a lookup, and counting it as a load would say a scene decoded far more than
it did.

Ingestion and publication are separate because they fail and scale for different
reasons: the first is a traversal on the host, the second an upload and an
acceleration structure build. Sponza spends 2.8 seconds in the second with no
refinement at all; the Kitchen Set spends 3.3 with 1,788 instances.

**Device memory is a high-water mark**, sampled after each frame rather than at
teardown, because that is the only moment the renderer holds path state,
structures, textures and film together. It is the heap usage the driver reports
for the process rather than the sum of what this allocator asked for: somebody
sizing a machine cares what the card is holding, which includes the driver's own
overhead.

**Camera rays are exact and the rest were absent.** Pixels times samples needs no
counter. What those rays go on to spawn was written on the device and not
reported, because getting it honestly needed an accumulator in the counters
buffer and one readback after the frame. It was named rather than estimated -- a
plausible multiplier would be a guess wearing a number's clothes -- and it landed
the same day; see the entry below.

**Two routes out.** Everything appears in `GetRenderStats()`, which is where a
Hydra host asks. The gallery script needs the same numbers out of a subprocess,
so the delegate also writes them to the file named by `HDCLAUDE_STATS_REPORT` --
a file rather than stdout, because a number a script has to find in a renderer's
console output breaks the first time anything else prints. Without the variable
it writes nothing and costs nothing.

The script reads whatever keys it finds rather than a fixed list, so a stage
added on the renderer's side reaches `build/gallery-timings.json` without the
script being taught about it. The table in gallery.md gains one column, device
memory, because a table with eleven is not a table; the rest live in the JSON,
since the interesting question about a scene is usually not the one a table was
built to answer.

---

## 2026-09-09 -- The stats are versioned beside the scene

The stage figures were being written to a scratch file and thrown away with the
build tree. They are now a committed artefact per scene, `gallery/<key>.stats`,
named for the `.usda` it describes so the two travel together and a diff says
what changed about a *scene* rather than what changed about a table.

The file is in two groups, because they answer different questions and a reader
should not have to count lines to tell which they are looking at.

`[scene]` is what the scene *is*: instances, triangles, control points in and
refined points out, materials, textures and their bytes, camera rays, and the
device memory peak. These change only when the asset changes or when the
renderer's handling of it does, which makes a diff here worth reading.

`[cost]` is what it cost on one machine on one day: the device, the date, the
wall time, and the per-stage times. These move a little every run and a diff
here usually means nothing.

Written without a byte-order mark, which `Set-Content -Encoding utf8` on Windows
PowerShell adds: a committed text file that begins with three invisible bytes is
a file every other tool has to be told about.

**And the settings line stopped lying.** Both this file and the timing table
described every render as `1024x1024`, built from the width twice, when these
scenes are framed by their own cameras and most are not square -- the height map
is 1024 by 434. The width is the only figure the script sets, so it is the only
one either of them claims now. It is a small thing and it is the sort a reader
takes on trust.

The environment variable is `HDCLAUDE_STATS_REPORT`, renamed from
`HDCLAUDE_MEMORY_REPORT` when it stopped being only about memory.

---

## 2026-09-09 -- The rays are counted, and they explain the timings

The stats recorded camera rays, which are the pixels times the samples and need
no counting, and said nothing about what those rays go on to spawn. They are
counted now, on the device, and they turn out to be the number that explains the
gallery's timing table.

    scene                      camera        traced   per camera      shadow
    Intel Sponza          603,979,776 1,811,192,973        3.00  845,070,836
    OpenChessSet          640,679,936   993,842,904        1.55  192,307,466
    ShaderBall Gold     1,073,741,824 3,601,513,782        3.35 2,225,695,355
    ShaderBall Glass    1,073,741,824 4,469,181,327        4.16 2,662,072,418
    ShaderBall Honey    1,073,741,824 4,375,783,543        4.08 2,543,350,891
    ShaderBall BubbleGum 1,073,741,824 4,125,784,676       3.84 2,592,877,252
    Pixar's KitchenSet    640,679,936 1,770,415,731        2.76  754,840,623
    Collective Project    715,128,832 2,183,311,940        3.05  938,675,395
    OpenPBR Playground    805,306,368 2,750,706,918        3.42  578,152,125
    Subdivision Matrix  1,073,741,824 1,227,764,153        1.14  113,255,138
    New Zealand Height    455,081,984   620,923,611        1.36   41,452,854

The glass ball traces **4.16 rays per camera ray** and the subdivision matrix
1.14. Both render 1024 samples at eight bounces; the settings are identical and
the work differs by a factor of four. That is a property of the scene -- how far
a path gets before it is absorbed, terminated or leaves -- and it cannot be
predicted from the bounce limit, which is why it was worth counting rather than
deriving.

It also confirms what the closure notes have been saying about glass. The three
transmissive or subsurface balls are the top three: refraction and total internal
reflection keep paths alive, and the glass ball's own note records its mean
converging upward from 0.6859 at eight bounces to 0.7547 at sixty-four. The
4.16 is the same fact from the other side.

Shadow rays are a separate question and behave differently: the gold ball asks
for 2.07 per camera ray and the Playground 0.72, on a scene with far more
geometry. Next-event estimation fires where a lobe is rough enough for it to be
worth anything, so a scene of mirrors and glass asks less often than a scene of
diffuse surfaces under the same lights.

**How they are counted.** `prepare_dispatch` already reads `activeCount` and
`shadowCount` once per bounce to size the indirect dispatches, so it adds them
into two accumulators in the same buffer while it has them in hand -- one
invocation, no atomics needed for correctness, and nothing that was not already
being read. The accumulators live past byte 16 of the counters buffer because
the inter-bounce reset fills bytes 4 to 16 and would otherwise clear them every
bounce.

They are read back **once per call**, in the same submit that copies the film,
after every dispatch they describe has finished. That is the only point a ray
count can be taken without putting a stall in the middle of the frame producing
it, and it is the same allowance `MaterialCounts` already has: a diagnostic
after the fact, never a readback during a frame.

The counts are cleared at the start of each call rather than by `raygen`, which
runs once per *sample* and would have reset them thirty-two times a frame.

Every gallery image re-renders byte identical except the four the known
cross-process nondeterminism moved, which were restored rather than adopted.

---

## 2026-09-09 -- Four more causes eliminated, and a claim withdrawn

The cross-process nondeterminism is characterised much more sharply and still has
no cause. What follows is what it is not, and one thing said earlier that does
not survive the extra data.

**Withdrawn: it is not bimodal.** The previous entry read a great deal into the
height map producing exactly 3.72045e-05 on two occasions under different
binaries, and concluded the effect was a binary choice made once per process
rather than a drift. More runs do not support that. Three renders of the chess
set at gallery settings differ from each other by 1.06e-5, 2.08e-4 and 5.30e-4 --
a spread of magnitudes, not two stable outcomes. Two runs landing on the same
figure was a coincidence given weight it could not carry, and "what to look for"
was named on the strength of it. The correction matters more than the original
claim did.

**It is driven by sample count, not by resolution.**

    1024 x 1024, 64 spp    (67M camera rays)   identical across three processes
    1024 x 1024, 1024 spp  (1.07B camera rays) different in every pair

Full resolution at a low sample count is clean, so the number of paths in flight
is not what matters. Somewhere between 67 million and 1.07 billion camera rays
the effect appears, which puts it around one ray in a hundred million: rare,
per-ray, and invisible below the gallery's own settings. That is why every
in-process gate passes -- they run at 128 x 128.

**It is not the progressive machinery.** The same 1024 samples in one call and in
thirty-two calls are both irreproducible, at rms 2.08e-4 and 5.30e-4. Descriptor
pool resets, accumulation continuation and the per-call state are all ruled out
together.

**It is not uninitialised path state.** Every path buffer -- origin, direction,
throughput, radiance, wavelengths, pixel, rng, scatter density, medium, hero
flag, hits, both queues, the shadow queue and the sorted queue -- is now fillable
with a known pattern before a frame by `HDCLAUDE_POISON_PATH_STATE`. A read of a
slot before it is written would be stable within a process and arbitrary across
them, which is exactly this effect's shape, so making that content identical
everywhere should have made the renderer reproducible. It does not: poisoned,
three processes still differ, at 1.06e-5 and 5.30e-4. The diagnostic is kept
because it is the instrument that answered this and will be wanted again.

**It is not the sort's scatter.** Both writes there -- the per-material count and
the scatter cursor -- are `atomicAdd`. A path claimed twice would be shaded twice
and its radiance added twice, which is a firefly at low frequency and would have
fitted; it is not what is happening.

**One earlier bisect was worthless and is retracted.** Rendering at one bounce
against eight looked like it separated traversal from continuation, and both read
zero. Both were run at 512 x 512 and 64 samples, which is below the threshold
above: *everything* is clean there. It measured nothing.

**What is left.** The acceleration structure build is the last obvious candidate:
it happens once per process, a GPU builder is under no obligation to produce the
same tree twice, and while a different tree should still give the same closest
hit, "should" is what this whole investigation keeps disproving. The in-process
rebuild test found nothing, but it runs on a synthetic scene of two prototypes
with no coincident surfaces.

The instrument that would settle it is an **id AOV** -- instance and primitive per
pixel. If two processes agree on what every ray hit and disagree on the image,
the difference is in shading; if they disagree on the hits, it is traversal or
the structure. That AOV is already on phase 7's remaining list for other reasons,
and it is now the cheapest way to answer this.

---

## 2026-09-09 -- The rays are identical and the hits are not

The cross-process nondeterminism now has a location. It is not in shading.

The instrument is a **hit hash**: a running `atomicAdd` over every hit the call
resolves, seeded on the instance, the primitive and the path that found it. It
lives in the counters buffer alongside the ray counts, is cleared once per call
by the same fill that clears them, is read back with them in one copy, and is
folded into a 64-bit value on the host so that the order the calls arrive in is
part of the answer. It reaches the report file and the `.stats` group exactly,
as text -- a hash does not survive a `double`, which would round away everything
below its low eleven bits and let two runs that disagreed print the same figure.

A per-pixel id AOV was the plan and would have been the wrong instrument: at one
sample it tests about a million primary hits against an effect that shows up
around one ray in a hundred million, so it would have read clean and proved
nothing. A hash over *every* hit in the render tests all of them.

The first run of it, on the Open Chess Set at gallery settings across three
processes, said something the image comparisons could not:

    tracedRays   993843050    993842838    993843257
    shadowRays   192307739    192307658    192307711

**The three processes do not agree on how many rays they traced.** That is not
noise on a fixed set of hits: paths are surviving to different depths. The count
is trustworthy -- `prepare_dispatch` runs one invocation with `local_size_x = 1`,
so the accumulation is not a race -- but it does not localise anything on its
own, because a single hit resolved differently at bounce zero changes every
bounce after it.

So the same test at **one bounce**, where the rays are identical across processes
by construction: they come from the camera and a fixed sampler, and raygen
assigns path ids as `activeQueue.values[index] = index`, so ray *n* is the same
ray with the same identity in every process. Three runs:

    cameraRays   640679936    640679936    640679936
    tracedRays   640679936    640679936    640679936
    hitHash      10263936795610863036
                 17012422686764329764
                 17564936163537684517

Identical rays, to the unit. Different geometry. The hash is keyed on
`record.x` and `record.y` -- the instance and the primitive -- and deliberately
not on `record.z/w`, which carry the barycentrics, so it cannot move on
floating-point drift in *where* on a triangle a ray landed. It moved because a
ray was told it hit a different triangle.

**This exonerates shading.** Every difference in the eight-bounce images is
downstream of a first-bounce hit that already differed.

Three candidates die with it. There are no subgroup operations and no shared
memory anywhere in the kernels, so nothing computes a result that depends on
which lanes happened to be resident. The ray query initialises with
`gl_RayFlagsOpaqueEXT`, which forces every geometry opaque regardless of the
per-geometry `VK_GEOMETRY_OPAQUE_BIT_KHR`, so there are no candidate
intersections and no shader-side commit policy whose order could matter. And the
sort's scatter was already ruled out; it now cannot be reached before the
divergence has happened.

What remains is genuinely two things, not one, and the ray counts cannot tell
them apart:

* the **acceleration structure**, built once per process by the driver under no
  obligation to produce the same tree twice, breaking a tie between adjacent or
  coincident triangles differently; or
* the **rays themselves**, if the driver's compilation of raygen differs between
  processes and a last-ulp difference in a direction changes a grazing hit.

The second is not far-fetched: identical ray *counts* say nothing about identical
ray *values*. The next instrument separates them by hashing the origin and
direction bits of every primary ray. If the rays are bit-identical and the hits
still differ, it is the structure; if the rays differ, it is upstream of
traversal entirely and the structure is innocent.

---

## 2026-09-09 -- The rays are bit-identical, and the fault is intermittent

The companion instrument is a **ray hash**: the same `atomicAdd` construction
over `floatBitsToUint` of every origin and direction the call traces, in the
counters word that had been a pad. It answers the half the counts could not.
Identical ray *counts* say nothing about identical ray *values*, so a
last-ulp difference in a direction -- from the driver compiling raygen
differently between processes -- would have produced different hits with every
count matching, and would have exonerated the acceleration structure.

It does not. Across processes the ray hash is **equal**, to the bit:

    cameraRays   640679936            640679936            640679936
    rayHash      3050392357624304916  (all three)
    hitHash      10263936795610863036 (all three)

Which is the second surprise. In the batch before the ray hash existed, the same
one-bounce configuration gave three processes one canonical hit hash and two
deviants, with images differing by an RMS of 2.2e-5. Since the ray hash was
added, **six consecutive processes across two batches agree exactly** -- rms 0,
worst 0 -- and on the same value that batch's *first* process produced. The
canonical answer has not moved; the deviations have stopped.

So the fault is **intermittent, and perturbable by the instrument measuring
it**. A global `atomicAdd` from every one of 640 million invocations is a heavy
serialisation point, and adding a second one plainly changed something that was
not the arithmetic: the hashes prove the arithmetic is identical.

That is not the signature of a wrong code path. A wrong code path does not stop
being wrong because a counter was added beside it. It is the signature of a
timing- or hardware-sensitive fault, and two facts sharpen that reading. The
device is a GeForce RTX 5060 Ti, which has **no ECC**, so a memory soft error is
neither corrected nor reported. And the effect's rate -- around one ray in a
hundred million, invisible below a billion camera rays -- is the rate at which
such things are usually noticed.

The specifications are worth stating here, because they decide whether a
difference of this kind is a defect at all. Vulkan's acceleration structure
chapter says **nothing** about a build being reproducible from identical
inputs; it is silent, not permissive. Ray traversal is explicitly not
deterministic at the margin: *"If t = tmax, the candidate **may** be set as the
current closest hit or dropped"*, the intersection test *"is performed in an
implementation specific manner, and **may** be performed with floating-point
operations"* with *"inaccuracies … expected"*, and not double-hitting or missing
at a shared edge is a **should**, not a **must**. So two processes disagreeing
about a grazing triangle is *permitted*; two processes disagreeing about a
triangle nowhere near an edge is not explained by any of it.

What has not been tested, in the whole of this investigation, is the thing that
separates those readings: **two full renders inside one process** at the sample
count where the effect appears. Every in-process check so far ran at 128 x 128,
where nothing differs at all. Two renders in one process share an acceleration
structure, a shader compilation and a device context, so a difference between
them cannot be any of the three, and would place the fault outside the
renderer's control entirely. The seed is `hdclaude_seed(index, sampleIndex, 0)`
and carries no time code, so a static stage rendered at four frames must produce
four identical films; that is the test, and it is now the one that matters.

---

## 2026-09-09 -- A vacuous test, retracted, and the effect stops reproducing

**Retracted: the in-process test at gallery settings measured nothing.** The
plan was sound -- four renders of a static stage in one process, sharing an
acceleration structure, a shader compilation and a device context, so a
difference between them could not be any of the three. `usdrecord --frames 1:4`
duly reported four time codes and four identical films at rms 0, and that was
read as four billion rays with no deviation and as evidence against a transient
hardware fault.

The four files were written **fifty-four milliseconds apart**. A thousand-sample
render of that scene takes minutes. Frames two, three and four were never
rendered: nothing about the stage, the camera or the settings changed, so
`InvalidateFor` correctly declined to reset the accumulation and the pass
re-emitted the film it already had. The test measured the invalidation logic
working exactly as designed, and the reported counters -- `tracedRays`
993843050, identical to a single render -- said so plainly enough to have been
noticed before the conclusion was drawn.

Nothing has yet been established about in-process behaviour at the sample count
where the effect appears, and the transient reading is not ruled out. Making
that test real needs the render forced to repeat, which is a diagnostic hook
beside `HDCLAUDE_POISON_PATH_STATE` rather than a scene trick: any change to the
stage that would invalidate the accumulation also changes the image.

**And the effect has stopped reproducing.** The eight-bounce cross-process test
that this morning gave three different films with three different ray counts now
gives three identical ones -- rms 0, `tracedRays` 993843050 in all three, and
both hashes equal. Counting the one-bounce batches, **nine consecutive processes
are clean** since the ray hash was added, having been dirty in every batch
before it.

Two readings fit and watching cannot separate them. Either the instrument masks
what it measures -- a global `atomicAdd` from every ray is a serialisation point
and could easily move whatever timing this depends on -- or the fault is simply
intermittent and this is a quiet window. The A/B that decides it is removing the
per-ray atomic while keeping the arithmetic that feeds it, and running the same
three processes again: if they go dirty, the instrument was masking; if they
stay clean, the phenomenon is intermittent at the scale of hours and every
earlier "eliminated cause" was tested against something that may not have been
present at the time.

That second possibility is the uncomfortable one, and it is worth saying
plainly: a long list of causes has been eliminated by running a test once and
seeing it come out clean. If the effect can be absent for nine consecutive
processes, a single clean run never eliminated anything.

---

## 2026-09-09 -- The A/B says the instrument was innocent, and a real repeat test

The A/B settles the first of the two readings and rules it out. With the per-ray
`atomicAdd` removed and the arithmetic that feeds it kept, three more processes
render the Open Chess Set **identically** -- rms 0, `tracedRays` 993843050 and
the same hit hash in all three. The instrument was not masking anything.

So the second reading stands: **the effect is intermittent at the scale of
hours.** It reproduced in every batch this morning, across two binaries, and has
now been absent for twelve consecutive processes across two more. Nothing in the
code distinguishes the quiet period from the noisy one; the A/B was run
specifically to find such a thing and found none.

That is the uncomfortable conclusion, and it is worth stating without softening:
**a long list of causes was eliminated by running a test once and seeing it come
out clean.** The scene store's ordering, the generated MaterialX, the sort's
scatter, the progressive machinery, uninitialised path state -- each was struck
off on a single clean run. If the effect can be absent for twelve processes in a
row, a single clean run never eliminated anything. Those causes are not
reinstated, but they are no longer *closed*, and the poison test in particular
proved only that poisoning did not make the renderer irreproducible on the day
it was run.

What can be trusted from here is what a **committed** measurement catches. Both
hashes now reach the `.stats` file that each gallery scene carries beside its
baseline, so the next occurrence shows up as a diff on a tracked file rather
than as a bespoke hunt started from a suspicion. That is the durable outcome of
the day: the committed `chess_board.stats` reads `tracedRays 993842904`, which is
a *fourth* distinct value and was written by a run that had the fault, and
nothing at the time noticed.

**A real in-process repeat test now exists.** `HDCLAUDE_REPEAT_RENDERS=n`
renders the finished image n more times inside one process, reporting each
render's ray counts and both hashes to stderr. The hook sits where convergence
is declared, which is the only place it can: the natural-looking home is the
early return that fires when a call finds the image already complete, and a host
stops calling `Execute` the moment `IsConverged` answers true, so that hook is
never reached. The first attempt put it there and printed nothing.

It is the only honest way to ask the question. Every other route to a second
render of the same image also rebuilds the acceleration structure, recompiles
the shaders, or starts another process -- which is precisely what the test is
trying to hold fixed.

At gallery settings the Open Chess Set rendered four times in one process gives
four identical results:

    tracedRays 993843050  shadowRays 192307739
    hitHash 6942637114454757797  rayHash 10775195811620185525   (four times)

Each of those is a real render -- the counters are reset between them and
re-accumulate to the same figure, which is what distinguishes this from the
retracted test. What it does **not** do is discriminate, because it was run
inside the quiet window where the cross-process comparison is also clean. Its
value is entirely prospective: when the fault returns, this is the test that
separates a per-process cause from a transient one, and it can be run in a
minute instead of built from scratch under suspicion.

---

## 2026-09-09 -- Two ways a hash stopped being a hash

Arming the standing watch found two places where the value being written was
not the value being measured. Both are the same mistake in different languages,
and both would have made the watch useless in exactly the case it exists for.

**The stats dictionary rounded it.** `GetRenderStats()` returns a
`VtDictionary`, and every entry in it was `VtValue(double(...))`, which is right
for the counts -- a triangle count or a ray count is far below the 2^53 a double
carries exactly -- and wrong for a hash. `6942637114454757797` comes back from a
double as `6942637114454757376`: everything below the low eleven bits is gone,
so two runs that genuinely disagreed could report the same figure. Both hashes
now go through as text. A count is a quantity and a hash is an identity, and the
only thing anyone does with an identity is compare it.

**Then the gallery script rounded it again.** `render_gallery.ps1` read the
renderer's report with `$stages[$key] = [double]$parts[1]`, one cast covering
every field, so the exact integer written to the report file became a double on
the way into the committed `.stats`. This one was caught by reading the file the
first gallery run had just written and noticing its hash ended `...757376` where
the renderer had printed `...757797` -- the same eleven bits, lost a second
time, three layers further out.

The parser now keeps the text as it came and each use site says what it wants:
`[uint64]` for a count or a hash, `[double]` where a fraction is meant. That is
the arrangement that should have been there anyway, since the dictionary is
described in its own comment as keeping "every key the renderer wrote, kept as
it came".

**And the stats are now compared, not merely written.** The scene group -- what
a scene *is*, as opposed to what it cost -- is checked against the committed
file before being replaced, and any key that moved is reported with its old and
new values. Reported rather than thrown: the image gate already fails a render
that moved, and a deliberate change that legitimately alters ray counts should
not have to fight the suite to land.

It exists because the counts were already carrying a defect nobody saw. The
committed `chess_board.stats` recorded a `tracedRays` of 993842904 -- a value
produced by a run that had the intermittent nondeterminism, a fourth distinct
figure alongside the three the hunt turned up -- and it sat there unremarked,
because writing a number down is not the same as reading it.

---

## 2026-09-09 -- A reliable reproducer, found by the watch on its first outing

**Retracted: the fault is not "intermittent at the scale of hours".** That
reading was drawn from twelve consecutive clean processes, and every one of them
rendered the **Open Chess Set**. It was the only scene the hunt ever used. The
conclusion generalised from one scene to the renderer, and the generalisation is
wrong.

Arming the committed `.stats` with both hashes found the reproducer the whole
investigation had been missing. Pixar's Kitchen Set gives a **different hit hash
every single time**:

    18048153105721151987
     9348002138563568471
    10304544561685832220
    13107609297471498173

Four consecutive renders, four values. And in every one of them `tracedRays`,
`shadowRays` and `rayHash` do not move at all. **The rays are bit-identical and
the hits are not** -- the same signature the one-bounce experiment produced on
the chess set before that scene went quiet, now on a second scene and on demand
rather than by luck.

The image gate never sees it. All four renders compare to the committed baseline
at rms 0, because that comparison is against the **JPEG**, and a radiance
difference below 8-bit quantisation vanishes into it. That is worth stating
plainly: an image gate reading exactly zero is not evidence that a render
reproduced. The hashes are, and they are measured on the linear film's causes
rather than on a display-transformed copy of its effects.

So the watch justified itself immediately. It was built to make a recurrence
show up as a diff on a tracked file instead of a bespoke hunt, and the first
time it ran it turned a phenomenon that had to be stalked into one that can be
asked for.

Why this scene and not the other is not yet answered, but the shape of the
guess is obvious: a kitchen is full of coincident and near-coincident surfaces
-- a cabinet door against its frame, a worktop against a splashback, objects
resting exactly on shelves -- and Vulkan is explicit that a candidate at
`t = tmax` *"may be set as the current closest hit or dropped"* and that
watertightness at a shared edge is a **should**. Chess pieces on a board have
far fewer such ties.

With a reproducer, the experiment that has been out of reach all along is
finally possible: render the Kitchen Set several times **in one process**. Same
acceleration structure, same shader compilation, same device context. If the hit
hash differs there too, the structure is exonerated and the cause is transient;
if it is stable within a process and differs between them, it is the build.

The answer came back immediately. Three renders of the Kitchen Set **inside one
process**:

    tracedRays 1770415731  shadowRays 754840623
    hitHash 16515537762802266486  rayHash 7474697996910621400   (three times)

Identical, every field. Across processes the same scene gives a different hit
hash every time. **Stable within a process, different between them.**

That settles it. A transient fault -- a memory soft error on a consumer card
with no ECC, which was a live hypothesis -- would strike within a process as
readily as between two, and it does not. What differs between two processes and
not within one is the state built once at startup, and of that the acceleration
structure is the only part that decides which triangle a ray hits. The rays are
bit-identical; the structure they are traced against is not the same structure.

Vulkan permits this. Its acceleration-structure chapter says nothing at all
about a build being reproducible from identical inputs -- silent, rather than
permissive -- and its traversal chapter is explicit at the margin: a candidate
at `t = tmax` *"may be set as the current closest hit or dropped"*, the
intersection test *"is performed in an implementation specific manner, and
**may** be performed with floating-point operations"* with *"inaccuracies …
expected"*, and not double-hitting or missing at a shared edge is a **should**.
Two differently-built trees are therefore entitled to disagree about a tie, and
a kitchen is full of ties in a way a chessboard is not.

So this is not a defect to fix in hdClaude's own code, and the honest response
is to stop treating cross-process bit-equality as a property the renderer can
have. What it *can* have, and now demonstrably does, is **in-process**
reproducibility -- which is the property phase 9's remaining steps actually need,
since each rests on the argument that a change did not move the image, and that
argument is made by rendering twice in one process rather than twice in two.

**One defect in the diagnostic, found by using it.** The repeat run wrote a
`cameraRays` of 1922039808 into the committed stats -- exactly three times the
truth. Camera rays are counted by the render pass rather than by the tracer, so
`PathTracer::ResetCounters` did not touch them and they summed across the
repeats. A diagnostic that quietly corrupts the file it is measured beside is
worse than no diagnostic, and the pass now clears that counter with the others.

**Which scenes actually reproduce, measured rather than assumed.** Regenerating
the gallery with the hashes in place answers a question nobody had asked
directly. Nine of the eleven scenes give byte-identical images and unchanged
counts. Two do not:

* **Pixar's Kitchen Set** -- a different hit hash on every render, with the rays
  and every count identical, and an image that still compares at rms 0 because
  the difference is below what an 8-bit JPEG can hold.
* **The OpenPBR Playground** -- the same, except that here it *does* reach the
  picture: rms 1.19e-4 against its committed baseline, well inside the gate's
  1e-2 limit but not zero.

The Playground's baseline is deliberately **not** replaced. A committed baseline
should change when someone means it to, and this difference has a cause that is
neither the asset nor the renderer's code; adopting the new render would only
swap one arbitrary member of a family of near-identical images for another and
lose the fact that they differ. Its `.stats` is updated, which is where the
evidence belongs.

The consequence to expect: those two scenes will report a moved hit hash on
every gallery run, forever, until the cause changes. That is signal and not
noise -- they genuinely do not reproduce across processes -- and it is the
reason the comparison reports rather than throws.

---

## 2026-09-09 -- The batching lands, and the timer that was never there

**The batching is re-landed and this time verified.** One command buffer per
sample holding every bounce and the film, in place of one per bounce plus one
for the film -- 288 submits per call at the gallery's 32 samples and 8 bounces,
down to 32. Every `SubmitImmediate` allocates a command buffer and a fence,
submits, and *waits for the device to go idle*, so the GPU drained 288 times a
frame for nothing: no host reads anything between two bounces. The barriers were
always there; a submit boundary was simply a free full barrier obtained by
stalling, and the explicit ones now carry the weight.

Measured back to back on the same machine, the render suite goes from
**237.84 s to 100.33 s** -- 2.37x, and close enough to the 243/107 the earlier
attempt reported to call it confirmed.

It was reverted the first time for want of a way to verify it, and that is what
has changed. Four things say it is sound:

* the whole suite passes, including the render tests with synchronisation
  validation enabled, which is the gate for exactly this class of change;
* the batched build reproduces the **unbatched** build's hashes bit for bit --
  the chess set, the glass ball and Intel Sponza all return the committed
  `hitHash` and `rayHash` that yesterday's unbatched binary produced. A hazard
  could not do that;
* three renders of Sponza inside one batched process are identical in every
  field;
* the images are unchanged at rms 0.

One render of Sponza did move, at rms 1.5e-3, with its ray counts and both
hashes changed -- and this is precisely where the first attempt died, unable to
tell a real regression from noise. Four further renders returned the committed
values exactly. It was the acceleration-structure nondeterminism, which Sponza
exhibits occasionally where the Kitchen Set exhibits it always. Its baseline is
not adopted.

**And the timing had a hole in it the size of the render.** Paolo noticed wall
times that the split timings could not account for and asked whether the machine
was doing something else. It was not. The stats carried `ingestMs`, `publishMs`,
`subdivideMs`, `materialMs` and `textureMs` -- every one of them a cost of
*preparing* a scene -- and **nothing at all for tracing it**. The render pass
computed the duration of every trace and handed it to `RecordFrameTiming`, which
kept the last one for the delegate's own use and never wrote it anywhere a
reader could see. `traceMs` now accumulates it across the progressive calls.

That still leaves a gap, and the gap turns out to be the interesting part. The
chess set at gallery settings: 22.1 s of wall time, of which **8.2 s is
tracing**. At one sample per pixel, where the trace collapses to 28 ms, the same
scene still takes **11.85 s** -- so the remainder is not the render, and it is
not the machine being busy either. It is `usdrecord` and Python starting, USD's
plugin discovery, opening the stage, Hydra populating its scene index before the
delegate's own ingest timer starts, then the EXR write and the device teardown.
None of it is visible from inside the delegate, which is why no stage could
account for it.

Measured across the whole gallery it is emphatically **not** a fixed startup
tax, which one scene alone made it look like:

    scene                     wall     trace   outside
    newzealand_heightmap      6.57       1.7      4.90
    subdivision_features      8.85       3.1      5.68
    shader_ball_bubblegum    59.61      51.0      8.25
    shader_ball_gold         24.36      15.7      8.32
    chess_board              22.12       8.2     12.66
    intel_sponza             48.55      14.0     31.05
    openpbr_playground       87.47      33.5     41.40
    pixar_kitchen           157.80      93.3     61.14

The four shader balls share one stage and sit at a flat 8.2-8.4 s however long
they trace, which is the per-process floor for that asset. Sponza spends 31 s
outside the renderer and the Kitchen Set 61 s. It scales with the scene, so what
it measures is **stage loading and scene-index population** -- work done before
the delegate is asked for anything -- and on the two largest scenes it is a
third to a half of the wall time.

So `outsideSeconds` is recorded explicitly, against trace plus ingest plus
publish -- the three that are wall-clock stages of the render thread. The other
three are summed across Hydra's workers and happen *inside* ingest and publish;
subtracting them as well would count the same work twice.

The consequence for reading the gallery's table: **wall time is a poor measure
of renderer cost.** Three quarters of the New Zealand height map's six seconds
are not rendering, and neither is two fifths of the Kitchen Set's two and a
half minutes; comparing the two on wall time compares two quantities that are
each mostly something else. `traceMs` is the column that means what the table
was always taken to mean, and `outsideSeconds` is the one that says how much of
the rest is USD rather than hdClaude.

---

## 2026-09-09 -- Per-slot resources, and the reset that was in the way

Phase 9's next step: everything a frame writes now belongs to a **frame slot**,
and there are two of them. A `FrameSlot` holds the path state, the queues, the
counters, the sort's table and indirect commands, the frame uniform and both
readbacks. The film is deliberately *not* in it -- progressive accumulation is
the one thing consecutive frames are meant to share, and giving each slot its own
would not protect the accumulation but break it.

Two is the number that does the work. It lets the next frame be recorded and
submitted while this one is still being read back; a third would cost another
full set of path state, the renderer's largest allocation by far, to overlap
something nothing is waiting on.

**The descriptor sets are the half that mattered**, and they were the actual
obstacle. They used to be allocated inside `Trace` from a pool reset at the top
of it, every single call. Resetting a descriptor pool whose sets an earlier
frame's command buffers still reference is undefined behaviour, so that
arrangement is correct *only* because every submit waits for the device to go
idle -- which is exactly the property the next step removes. No amount of
per-frame buffering would have helped while the pool was still being reset under
whatever was in flight.

So sets are allocated once per slot and rewritten only when what they name has
actually moved. A `_resourceGeneration` counter is bumped by the two things that
invalidate them -- a resolution change replacing every buffer, and a published
scene replacing the shading pipelines -- and a slot whose sets are behind
rewrites them before it is used. The pool reset survives, but only inside that
branch, where it is safe for a reason that can be stated: the resource change
that bumped the generation happened *between* frames, not during one.

It is meant to be behaviour-neutral and it is. The whole suite passes; the chess
set, the glass ball, the subdivision matrix and the height map all render at
rms 0 against their committed baselines with **both hashes unmoved**, which is
a stronger statement than the images alone -- a gallery render makes 32
progressive calls and therefore alternates slots 32 times, so every one of those
frames used a different slot from the one before it and produced the identical
answer.

It is also, unexpectedly, faster. The render suite goes from 100.33 s to
**77.27 s**, because allocating seven-plus descriptor sets and writing
twenty-odd bindings into each of them, on every trace, was never free. Against
the 237.84 s the suite cost before the batching, the two steps together are
3.1x.

---

## 2026-09-09 -- Two things tried on the way to the wait, and both reverted

With per-slot resources in, the remaining step is to move the submit into
`BeginFrame` and the wait into `EndFrame`. Two attempts at the ground work were
made and neither survives; both are recorded because each rules something out.

**One command buffer for the whole frame is slower, not faster.** The batching
that halved the render suite went from one buffer per bounce to one per sample,
so the obvious next move was one per *frame*: thirty-two submits down to one.
It is correct -- the suite passes and the images are unchanged at rms 0 with the
hashes unmoved -- and it costs a great deal of time:

    chess set      22.5 s -> 31.6 s
    glass ball     27.6 s -> 41.8 s
    subdiv matrix   8.8 s -> 12.4 s

The reason is not mysterious in hindsight. A frame at gallery settings is 32
samples of 8 bounces of ten-odd dispatches with a barrier between each, so a
whole-frame buffer holds some two and a half thousand commands, and **the device
does nothing at all while the host records them**. Per-sample buffers hand the
GPU a quarter-million-path workload every few milliseconds instead. The earlier
step won because it removed *waits*; this one lost because it added latency
before the first one.

The lesson generalises past this change: fewer submits is not the goal, and it
was never the mechanism. The mechanism was removing the device stall that
`SubmitImmediate` performs after each submit, and that is achieved by not
waiting, not by merging buffers.

**And the sample index does not want to be a push constant.** Whole-frame
recording needs one, because `sampleIndex` was a field of a host-written uniform
and the host cannot rewrite it while recorded dispatches still read it -- exactly
the argument that moved `bounce` out. So it moved the same way, raygen being the
only kernel that reads it.

It works, and it makes the **first render of every process** disagree with every
later one. Four trials of three renders in one process, and every trial has the
same shape:

    render 1   993843147 / 993842489 / 993843165 / ...    all different
    render 2   \  identical to each other, every time
    render 3   /

That is not the acceleration structure, which is fixed for the life of a process
and gives the *same* answer to all three. It is not uninitialised path state
either: `HDCLAUDE_POISON_PATH_STATE` does not change it. Reverting the change
restores six identical canonical renders across two processes. The cause is
unexplained, the change had no remaining benefit once whole-frame recording was
abandoned, and shipping an unexplained first-frame regression to buy nothing is
not a trade worth making.

Worth keeping from it: the instrument found this in one run. A regression that
appears only in the first image of a process, two orders of magnitude below the
gate's RMS limit, is exactly what would have shipped invisibly a week ago.

**What the split actually needs.** The obstacle is real and still there: one
uniform buffer written by the host between samples cannot be in flight twice.
The answer is not a push constant but a **dynamic uniform offset** -- one buffer
holding an aligned copy per sample, bound with a per-sample offset, so nothing
is ever rewritten while it is being read and no kernel signature changes at all.
That is the next thing to build.

---

## 2026-09-09 -- The dynamic offset works, and something in the first trace does not

The design named for the split is right and it is implemented: binding 0 becomes
`VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC`, the frame block is allocated with
one copy per sample at the device's `minUniformBufferOffsetAlignment` stride,
every dispatch of sample *n* binds at offset *n* x stride, and the host fills all
of them before recording. No kernel signature changes. It works: the whole suite
passes, and the images are unchanged.

It also makes the **first render of every process** disagree with every later
one -- the same signature the push constant produced, and the reason that is
worth stating is that it means **the push constant was never the culprit**. Two
implementations with nothing in common in the data path do the same thing.

The baseline is not in doubt. The committed build gives **eighteen renders across
six processes, every one identical**, first renders included. The changed builds
gave a divergent first render **eight times out of eight**, across three
independent attempts: the push constant, the dynamic offset writing all copies
up front, and the dynamic offset writing each copy immediately before its own
submit.

That third one was a bisect and it eliminates the obvious explanation. Writing
ahead of time is not the problem: with the write moved back to exactly where the
committed build does it, byte for byte the same host behaviour, the first render
still diverges. Also eliminated: uninitialised path state (`HDCLAUDE_POISON_PATH_STATE`
does not change it), descriptor ordering (the buffer grows and bumps the
generation *before* the sets are written, so nothing dangles), and the
acceleration structure -- which cannot be it at all, because the structure is
built once per process and answers all three renders of a repeat identically,
which is exactly what the committed build shows.

What the two attempts do share is that each **changes a pipeline layout**:
raygen gaining a push constant range, or binding 0 changing descriptor type. The
hypothesis that fits every observation is a driver-side one -- the first
execution of a freshly compiled pipeline differing numerically from later ones,
with the layout change moving the compilation onto a different path. It is
consistent with the effect surviving every change to the data path and with its
being confined to the first trace. It is a hypothesis and not a finding: nothing
here has tested it.

**Not shipped.** A renderer whose first image differs for reasons nobody can
state is exactly what this phase spent a week making visible, and shipping one
to buy an optimisation would be the same trade refused twice already today.

**Shipped: the plumbing**, which is independent and inert. `Dispatch` and
`DispatchIndirect` take a dynamic offset and bind exactly as many offsets as the
layout has dynamic descriptors -- none, at present, so every call is unchanged.
`WriteBuffer` takes a range, because a dynamic binding needs the size of one
element rather than the whole buffer, or every offset but zero runs past its end.
`VulkanCapabilities` records `uniformBufferOffsetAlignment`. Turning it on is
then a three-line change: the binding type, the range, and the per-sample
allocation.

**And a latent bug, found by it.** `ComputePipeline`'s move constructor and move
assignment are written out by hand, member by member, and the new `_dynamicBindings`
was not among them. Every pipeline is built as `_raygen = build(...)`, so the
count was computed correctly and then moved away, and the bind supplied no offset
for a layout that required one -- which the validation layer caught immediately
and stated exactly. The member is carried now, and the header says why anything
added beside it must be too. That hazard is a property of hand-written move
operations, not of this change: the next member added there will be silently
dropped the same way.

---

## 2026-09-09 -- Phase 10 begins with a hole in phase 9's decision

The temporal foundation's first requirement is the one signal a reconstruction
backend cannot work without: when to throw its history away. Building it found
that the decision it belongs in was incomplete.

`InvalidateFor` compared the extents, the mode and the scene revision, and
**not the camera**. The function exists to make a caller's `resetAccumulation` an
input rather than the answer -- "a caller that forgets a resize, a mode switch or
a scene revision still gets a correct frame" -- and a caller that moved the
camera and forgot was the one case that claim did not actually cover. An
accumulated film is an average of one integral; a camera that has moved makes it
an average of two, and every sample after the move is being added to the wrong
picture.

The comparison is exact, deliberately. There is no tolerance at which a camera
has moved "not enough to matter": a hundredth of a pixel of parallax is a
different integral. A camera that has not moved produces bit-identical numbers
from the same source data, so equality is the honest test and an epsilon would be
a threshold nothing asked for. The risk in that -- that the delegate recomputes
its matrix each `Sync` with some instability and every call then resets -- is
real and is checked rather than assumed: the chess set and the glass ball still
accumulate across 32 progressive calls and render at rms 0 with both hashes
unmoved, so the camera arriving from Hydra is stable to the bit.

**History reset is a strict subset of accumulation reset, and the camera is what
separates them.** A resize, a mode switch or a changed scene leaves a
reconstructor nothing to carry forward. A camera that moved leaves it everything
to carry forward -- that is precisely what motion vectors are for, and discarding
history on camera motion would throw away the one case temporal reconstruction
exists to handle. So `FrameResult` now carries both, and the gate asserts the
difference: the moved-camera frame resets the accumulation and not the history,
while the resized, revised and switched frames reset both.

The gate also asserts that a camera which then stays put may continue, so it
cannot pass by resetting everything -- the failure mode that would make the whole
check vacuous.

---

## 2026-09-09 -- Any change to the kernels' layout makes the renderer irreproducible

The depth AOV was built end to end and it is not what this entry is about.

It follows the specification rather than a convention of its own: hdEmbree
transforms the hit by the view and projection matrices it was handed and writes
`(ndc.z + 1) / 2`, so `RenderCamera` now carries the host's `worldToClip` --
composed in `HdClaudeMakeRenderCamera` from the same two matrices the pass
already had -- and the guide kernel does the same arithmetic. Which
normalisation is *the* one is a question about the host's projection, not about
this renderer, and rebuilding a projection here from a field of view would be
inventing an answer to it. The kernel runs once per sample, immediately after
the first bounce's `extend`, which is the only point where the primary hit
exists; the bounce after it overwrites both the hit record and the origin that
`extend` moved onto the hit. A miss writes 1.0, which is the clear value Hydra
gives a depth AOV.

Then the guard flagged in the phase 10 plan fired.

**Adding the binding makes the renderer disagree with itself inside one
process.** Three renders of the chess set in a single process, twice over: once
`[canonical, divergent, canonical]`, once `[divergent, canonical, canonical]`.
The committed build has never done this -- 24 renders across 8 processes, every
one identical, including a control taken in the same hour specifically so the
comparison could not be blamed on the scene drifting.

The obvious suspect was the extra dispatch, and it is not that. With the binding
still declared and the dispatch removed by a `false &&`, the divergence remains.
**It is the layout change alone.**

That makes four independent changes, all of them layout changes, all perturbing:

    raygen gains a push constant range          diverges (4 trials, first render)
    binding 0 becomes UNIFORM_BUFFER_DYNAMIC    diverges (2 trials, first render)
    a binding is added, kernel dispatched       diverges (2 of 6 renders)
    a binding is added, kernel not dispatched   diverges (1 of 6 renders)
    ------------------------------------------------------------------------
    layout unchanged                            30 renders, 0 divergent

And it is *in-process* nondeterminism, which is the part that matters. The
acceleration structure cannot produce it: it is built once and every render of a
repeat traverses the same tree, which is exactly why the committed build's
repeats agree. Something that varies between two renders in one process is a
race or a warm-up, not a structure.

The shader-cache hypothesis -- a new layout being a new cache key, so the driver
compiles in-process rather than loading a compiled pipeline -- fits the shape of
it, and the one test available was inconclusive: `__GL_SHADER_DISK_CACHE=0` did
not make the committed build diverge, but nothing confirms that variable is
honoured on this platform, and the cache directory itself cannot be cleared from
here.

**This is now the critical path, not a side quest.** It has stopped two
different pieces of work today, and it will stop every future one: the id AOVs,
normals, albedo, motion vectors and the reconstruction backend all add bindings,
and adding a binding is the thing that breaks reproducibility. The next task is
not another feature but understanding this, and it is at last a well-posed
question with a clean control and four data points.

The depth AOV is reverted rather than shipped, for the reason that has decided
every one of these today and one more besides: the committed hashes are the
instrument that found this, and shipping something that makes them unstable
would spend the instrument to buy the feature.

---

## 2026-09-09 -- The cause, found by taking the driver's cache away

The first-trace divergence is the **driver's shader disk cache**, and the
experiment that shows it is symmetric:

    cache present     6 renders clean   (and 30 before it)
    cache moved away  4 trials, every one divergent on the first render
    cache restored    6 renders clean again

Nothing else changed between those runs. Same binary, same scene, same settings.
With `%LOCALAPPDATA%\NVIDIA\GLCache` emptied, the **committed** build -- the one
that had been reproducible thirty times over -- diverges on the first render of
every process, and putting the cache back makes it reproducible again.

The mechanism follows: a pipeline the driver has not cached is compiled during
the run, and its **first execution differs numerically from its later ones**.
Once the compiled form is on disk it is loaded rather than built, and every
render agrees from the first dispatch.

That explains every observation this took to reach, and retires several dead
ends:

* every one of the four layout changes perturbed the renderer because a changed
  layout is a **different cache key** -- a push constant range, a binding type,
  an added binding, cached or not cached, the arithmetic never mattered;
* it was always the *first* render in a process, because the second runs on a
  pipeline the first one finished compiling;
* poisoning path state, moving the uniform write, and reordering descriptor
  writes all failed to change it, because none of them is where it lives;
* `__GL_SHADER_DISK_CACHE=0` did nothing, which was read as weak evidence
  against the cache and was really just evidence that the variable is not
  honoured here.

**hdClaude has no pipeline cache of its own.** `vkCreateComputePipelines` is
called with `VK_NULL_HANDLE`, so the only thing standing between this renderer
and a cold compile on every layout change is the driver's implicit cache, which
is not ours, not portable, and -- as this experiment demonstrates -- can be
cleared by anything on the machine.

Two consequences worth stating plainly.

**A baseline rendered immediately after a shader change may be a cold-cache
render.** Every gallery regeneration today followed a build, and the ones that
followed a *shader* build had a cold cache for the kernels that changed. That is
very likely what several of the day's "intermittent" divergences were, and it is
a reason to be suspicious of any baseline adopted in the same minute as a shader
edit.

**It is not the whole story, and the other half stands.** The Kitchen Set
differs on *every* render across processes while three renders inside one
process agree exactly -- the opposite signature, per-process rather than
first-render, and the acceleration structure remains its explanation. There are
two phenomena here and this experiment separates them cleanly for the first
time: one is a compile, the other is a tree.

---

## 2026-09-09 -- A pipeline cache of our own, which does not fix it

`vkCreateComputePipelines` was called with `VK_NULL_HANDLE` for its cache, so
hdClaude had none and every pipeline was left to the driver's implicit one. It
has one now: created with the device, seeded from a file, written back at
teardown through a temporary and a rename so an interrupted run leaves the
previous cache rather than a truncated one. The path is
`HDCLAUDE_PIPELINE_CACHE` or a file in the system temporary directory. The blob
is passed to the driver exactly as read, because an implementation is required
to reject a cache from another device or driver after checking the header it
wrote itself -- a stale file costs a recompile and nothing worse, which is why
nothing here validates it. Failing to create one is not fatal: a cache is an
optimisation and a renderer that cannot make one still renders.

It is worth having and it is **not** the fix, which the experiment says
directly. With the driver's `GLCache` moved aside and hdClaude's own cache warm
-- 2.3 MB, written by the run before and loaded at startup -- the first render
of a process still diverges, in both trials. Restore the driver's cache and
everything is clean again.

So whatever the driver keeps in `GLCache` is not what a `VkPipelineCache`
covers. The plausible reading is that the Vulkan-level cache holds an
intermediate form and the final machine code is produced later, on first use,
and cached separately -- but that is a guess about a closed implementation and
is written here as one. What is measured is the A/B, three times over now, and
that our cache does not change it.

The cache is shipped anyway. Passing null was an omission rather than a
decision, it is standard practice, and it is behaviour-neutral where it counts:
with the driver's cache in place the whole suite passes, the chess set renders
at rms 0, and all three renders of a repeat give the canonical hashes.

**The mitigation that might actually work** is a warm-up: a throwaway render
before the first real one, absorbing the cold first execution of every pipeline.
It is worth trying rather than assuming, because the divergence survives an
entire 32-call render -- the first *image* differs, not merely the first
dispatch -- so a token dispatch may not be enough. That is now a cheap
experiment rather than a speculation: the fault reproduces on demand by moving
one directory.

---

## 2026-09-09 -- What the cache does and does not explain, and two failed fixes

The diagnosis holds and sharpens. Three further experiments, all of them
negative, and they matter because each removes a fix that would otherwise have
looked obvious.

**A pipeline cache of hdClaude's own does not substitute for the driver's.**
With `GLCache` moved aside and our own cache warm at 2.3 MB, written by the
previous run and loaded at startup, the first render still diverges. Whatever
the driver keeps in `GLCache` is not what a `VkPipelineCache` holds. The cache
is shipped anyway -- passing `VK_NULL_HANDLE` was an omission, not a decision --
but it is not the fix and is not described as one.

**It is not the cache's capacity.** `GLCache` stood at 975 MB, close enough to
NVIDIA's default limit to suggest that new entries simply could not be written.
Emptying it entirely and rendering three times over says otherwise: with all the
room in the world the divergence is unchanged, and the directory grows to four
files and stops. The driver is not caching these pipelines during these runs at
all.

Which leaves an asymmetry worth stating, because it is the useful part. A
**cached** layout gives a clean first render -- that is why the committed build
is reproducible, and why the gallery, which renders every scene in a fresh
process, has been trustworthy. An **uncached** layout diverges on the first
render of every process and does not stop: five consecutive trials of the depth
AOV build, and three more on an empty cache. Yet the current layout is cached,
so the driver evidently does write these entries eventually, under conditions
that no experiment here reproduced. What those conditions are is the open
question.

**A warm-up sample does not absorb it.** The reasoning was sound -- if the first
execution of a pipeline differs, make that execution one whose result is thrown
away -- and the implementation is cheap, because a throwaway sample recorded
before the film clear costs nothing but its own time: the clear that had to
happen anyway wipes it, and the counters are zeroed at the real sample zero.
Three trials say it does not work. The cold period is longer than one sample's
dispatches, and a warm-up long enough to cover it would cost about as much as
the frame it protects. Reverted, because a mitigation that does not mitigate is
just a code path.

One detail may explain the shortfall and is worth checking before anyone tries
this again: every queue-sized dispatch in the integrator is *indirect*, and an
indirect dispatch of zero workgroups executes no invocations at all. A material
that no path reached during the warm-up sample is therefore still cold
afterwards, so one sample warms only the pipelines it happened to feed.

**The depth AOV stays unshipped and the reason is now precise rather than
mysterious.** It is not that the AOV is wrong -- it follows hdEmbree, it renders,
the suite passes. It is that its binding changes the layout, an uncached layout
diverges on every process's first render, and the gallery renders every scene in
exactly one process. Shipping it would make every committed hash in the gallery
unreliable, which is to say it would destroy the instrument that found all of
this. The kernel and its wiring are described in the entries above in enough
detail to rebuild in an afternoon; what is worth more than the code is knowing
why it could not go in.

---

## 2026-09-09 -- The render purpose, and a profile that measured the wrong scene

`usdrecord` renders the `default` purpose unless told otherwise, and a
production asset puts its real geometry under `purpose="render"` while keeping a
cheap stand-in under `purpose="proxy"`. The default is therefore not a subset of
the right answer; it is the wrong half of the stage.

ALab's `lab_structure01` shows the size of the mistake. Thirty-six of its meshes
and its single `PointInstancer` are render purpose; two meshes are proxy.
Rendered as `usdrecord` defaults:

    instances    25      triangles    304,698      wall  47.5 s

and with `--purposes render`:

    instances  2,074     triangles 87,749,024      wall 170.4 s

The first render succeeded, took forty-seven seconds, wrote an image, and was of
almost nothing. That is the failure worth guarding against: nothing errors,
nothing warns, and the picture looks like a picture. Every number taken from it
-- the ray throughput, the memory peak, the split timings -- described a scene
nobody asked about, and was wrong by two orders of magnitude.

So `render_claude.bat` now passes `--purposes render` for every caller, beside
the two flags that were already always-on for the same class of reason, and the
gallery's per-scene `Purposes` field is gone because nothing needs to remember
it any more. Checked rather than assumed: of the eleven gallery stages only
`collectiveproject001` has a render/proxy split at all and it already requested
render, so the change moves no image, and the chess set and Collective Project
both re-render at rms 0.

This belongs with hdClaude's standing refusal to substitute something plausible
for what a scene authored. Rendering the proxy while the stage asks for render
geometry is that same error wearing a different hat -- except that here the
input is perfectly valid and the renderer is choosing the wrong half of it.

---

## 2026-09-09 -- Profiling ALab's lab_structure01, and what it is not

The question was whether the acceleration structure needs work. On this evidence
it does not, and the instrument that says so is two counters that already
existed and were never reported.

`SceneAccelerator` has kept `_lastBuilt` and `_lastReused` since prototypes were
first deduplicated by geometry fingerprint, and nothing ever read them. They are
in the stats now, because the ratio is the difference between instancing and the
appearance of it: a stage of two thousand instances over a handful of distinct
prototypes should build a handful of structures, and building two thousand would
mean the deduplication is not seeing what it should. The chess set reports 8
built and 13 reused -- twenty-one prototypes collapsing to eight structures.

**The scene is slow in tracing, and it is not the structure.** At the settings
that matter, `lab_structure01` spends 133 s of 170 s wall inside the trace, at
**1.12 Mrays/s** against 398 for a simple gallery scene. Subdivision is what
moves it, and the shape of that is the interesting part:

    subdiv 0    5,483,056 triangles    trace   9.2 s   16.2 Mrays/s
    subdiv 1   21,937,256 triangles    trace 134.6 s    1.11 Mrays/s
    subdiv 2   87,749,024 triangles    trace 133.3 s    1.12 Mrays/s

Four times the triangles costs **fourteen and a half times** the trace, and then
four times more costs nothing at all. That is a cliff followed by saturation, not
a scaling curve, and a bounding-volume hierarchy does not behave that way.

Four candidates are eliminated by measurement rather than argument. It is not
**deduplication**: 335 structures built and 4 reused at *both* levels, identical.
It is not **memory**: the peak moves from 7.31 to 9.11 GiB with between 3.4 and
5.2 GiB still free, so nothing is spilling. It is not **more work**: the ray
counts are 149.8 M and 149.5 M, the same rays costing more each. And it is not
**shading setup**: both levels compile 93 materials, load 902 textures and emit
character-for-character identical warnings.

What is left is the cost of a ray, which rose by an order of magnitude when the
geometry was refined and then stopped caring how much geometry there was. That
is the signature of a per-ray bottleneck rather than a per-triangle one, and the
honest answer is that **`traceMs` cannot distinguish traversal from shading**,
because it covers extend, sort, shade, shadow and film together. Splitting it
with GPU timestamps per kernel is the next instrument, and until it exists any
claim about which half is slow is a guess.

One other figure deserves attention on its own account: **902 textures weighing
6.84 GiB**, loaded in about 14 s. That is a fixed cost paid before a single ray
is traced, it dominates a low-sample render outright, and for a full ALab layout
rather than one entity it will not fit.

**And the curves.** `stoat01` carries 33 `NurbsCurves` totalling 260 control
points at a constant width of 0.014 -- whiskers, not a coat. Only `mesh` is in
`kSupportedRprimTypes`, so Hydra never creates them and they are absent rather
than wrong. Supporting them is a real piece of phase 3 work (a curve primitive,
its acceleration-structure geometry type, and a shading frame for a swept
curve), but this asset sets the bar low enough to be a reasonable first case.

---

## 2026-09-09 -- The kernel profile, and traversal collapsing on refined geometry

`traceMs` covers extend, sort, shade, shadow, environment and film together, so
it could say a scene was slow and never say in what. `HDCLAUDE_PROFILE_KERNELS`
splits it: a query pool timestamps each kernel group of **one** sample, twelve
spans a bounce plus two for the film, and the totals reach the stats. One sample
rather than all of them, because a sample is representative and timestamping
every dispatch of a thousand-sample render would change what it measures. The
device is asked first -- a timestamp period of zero or a queue family with no
valid bits means the profile declines rather than reports noise.

On ALab's `lab_structure01`, at 512 px and eight samples, per sample in
milliseconds:

                subdiv 0   subdiv 1   subdiv 2
    prepare         0.04       0.04       0.04
    extend          0.25      46.27      51.22
    sort            0.15       1.95       2.07
    environment     0.31       1.65       1.65
    shade           4.84      24.08      29.92
    shadow          0.15       1.08       1.07
    film            0.05       0.28       0.45

**It is `extend`** -- the ray query itself. Traversal goes from about four per
cent of the trace to sixty per cent, and against the full-size render that is
roughly **377 Mrays/s falling to 1.8**. Four times the triangles costs a hundred
and eighty-six times the traversal, and four times more after that costs eleven
per cent.

The ray counts rule out the obvious reading: 2,336,376 traced at level 0 against
2,332,181 at level 1 and 2,331,249 at level 2. The same rays, in the same
numbers, taking two hundred times longer each.

Six explanations are eliminated by measurement.

* **Not deduplication.** 335 structures built and 4 reused at every level.
* **Not triangle count.** Level 2 has four times level 1's geometry and costs
  eleven per cent more.
* **Not memory.** The peak moves 7.31 to 9.11 GiB with 3.4 GiB still free.
* **Not opacity.** Every prototype is classified `Opaque`, so no build takes the
  any-hit path.
* **Not broken refinement.** A Catmull-Clark limit surface lies inside the
  convex hull of its cage -- a theorem, not a tolerance -- and a check on that
  invariant now runs on every refined mesh and reports not one violation on this
  asset. The check is kept: a single stray vertex inflates an acceleration
  structure's upper nodes and costs far more in traversal than the geometry it
  belongs to, and that is worth catching by assertion rather than by a
  fortnight's confusion.
* **Not scrambled connectivity.** Scrambled indices would draw a different
  picture; the mean across levels is 0.269, 0.270.

And it is not refinement in general. The chess set refines to 23 M triangles --
about level 1's count here -- and traces six times faster. Something about *this*
asset's refined geometry is pathological for the hierarchy, and black-box
measurement has run out of road: the next instrument is a GPU profiler that can
report traversal steps and hierarchy quality, or an isolation that renders one
prototype at a time until the bad one names itself.

What can be said without it: the slowness is traversal, it is not the amount of
geometry, and it appears the moment refinement runs.

---

## 2026-09-09 -- Curves, as swept tubes through the mesh path

Only `mesh` was in `kSupportedRprimTypes`, so a stage's curves were never
created by Hydra at all -- absent rather than wrong, which is the better of the
two failures but still a hole. `basisCurves` is supported now, swept into a tube
and published through exactly the path a mesh takes.

**The shape of the choice.** A ray tracer needs an explicit surface to build an
acceleration structure over. The exact alternative -- procedural AABB geometry
with the swept cone intersected in the traversal kernel -- is what a hair
renderer eventually wants, and it costs two things this project currently cannot
spend cheaply: a candidate-intersection path in `extend`, which is the kernel
every other measurement depends on and the one just found to be collapsing on
ALab's geometry, and a new binding, which is the change that reliably perturbs
the first render of a process. Sweeping a tube costs neither. It reuses the
acceleration structure, the shading, and the descriptor set layout exactly as
they are, so **this feature adds no shader change and no binding at all**.

The approximation is honest and bounded: a tube of *n* faces approximates a
circular sweep as a subdivided mesh approximates a limit surface, and converges
the same way. `HDCLAUDE_CURVE_SIDES` sets it, defaulting to six, for the same
reason the subdivision level is a setting.

**What the asset actually needs, checked rather than assumed.** ALab's
`stoat01` carries 33 `NurbsCurves` of 260 control points between them, at a
constant width of 0.014 -- whiskers, not a coat. And `UsdImagingNurbsCurvesAdapter`
reports them to Hydra as `basisCurves` with a **linear** basis: its own comment
says it is "drawing the cage for NURBS curves", so the control polygon is what
every Hydra renderer receives, not the evaluated NURBS. hdClaude therefore draws
what every other Hydra renderer draws. A cubic basis is refused by name rather
than swept as though it were linear, because evaluating basis matrices to a
polyline is separate work and silently treating a cubic curve as a polyline
would make it visibly wrong in a way nothing reported.

**The sweep lives in the core**, with no USD and no Vulkan in it, which is what
makes it testable on any host. Three tests state the claim geometrically rather
than by eye:

* every vertex of a tube swept along the z axis lies exactly one radius from
  that axis and its normal points straight out of it -- that is what a swept
  circle *is*, so it must hold whatever the side count. Worst error 4.3e-09.
* every index is in range, every vertex is used, and the triangle count is the
  one the segment and side counts imply. An index past the end is the failure
  that costs a device rather than a picture.
* input that cannot be honoured -- counts that do not add up, a single-vertex
  curve, widths matching neither points nor curves -- is refused with a reason,
  not half drawn.

A frame is carried along each curve by parallel transport rather than rebuilt
from a fixed axis, because rebuilding makes the tube spin wherever the tangent
passes near that axis, and that shows as a twist in the shading and a crease in
the silhouette. Each ring carries one extra vertex so the texture seam closes:
two copies at the same position with u of 0 and 1, or the last face samples the
whole map backwards.

Not yet verified end to end: the plugin could not be installed while another
render held it, so the stoat has not been rendered with this build. The code
compiles and the core tests pass; the picture is still owed.

---

## 2026-09-09 -- The curves were there, and Hydra never asked for them

Declaring `basisCurves` supported was not enough, and the reason is worth
recording because nothing about it fails loudly.

`UsdImagingNurbsCurvesAdapter` has two faces. Through the **legacy** path it
reports a `basisCurves` prim with a linear basis -- its own comment says it is
"drawing the cage for NURBS curves" -- and a renderer that supports
`basisCurves` gets the control polygon. Through a **scene index**, the same
adapter's `GetImagingSubprimType` returns `HdPrimTypeTokens->nurbsCurves` and
hands the NURBS data over untouched. `usdrecord` takes the second path.

So the whiskers arrived as `nurbsCurves`, Hydra gated their creation on
`IsRprimTypeSupported`, hdClaude said no, and **nothing was reported at all** --
not even the "unsupported rprim type" warning, because the delegate was never
asked. The geometry was simply absent, and every earlier statement that ALab's
curves "are absent rather than wrong" was true for a reason nobody had checked.

Declaring `nurbsCurves` supported instead is a dead end, and it was worth ten
minutes to prove rather than assume: the prims are then created -- confirmed by
name, all thirty-three -- and `GetBasisCurvesTopology` returns nothing, because
`HdSceneIndexAdapterSceneDelegate` has no `nurbsCurves` handling and `HdTokens`
has no `curveVertexCounts`. There is an `HdNurbsCurvesSchema` and no
`HdNurbsCurves` rprim to read it through the legacy API.

OpenUSD ships the conversion: `HdsiNurbsApproximatingSceneIndex`. hdClaude now
registers a scene index plugin that inserts it, so a NURBS prim becomes the
linear cage before the renderer sees it -- the same cage the legacy path would
have produced, which means hdClaude draws what every other Hydra renderer draws
and needs no NURBS code of its own.

**One line made the difference and it is not in any C++ file.** The plugin's
`plugInfo.json` entry needs `"loadWithRenderer": "Claude GPU Path Tracer"`.
Without it the type is declared, the registry knows it exists, and it is never
loaded: `_LoadPluginsForRenderer` preloads only what a plugInfo has tagged for
that renderer, and a scene index plugin registers itself for a renderer *from
inside the library*, so a library that is never loaded never registers. The
symptom is a correct-looking plugin that does nothing whatsoever.

With it, the stoat renders its whiskers: 288 instances against 255, each strand
56 vertices and 84 triangles -- seven segments, six sides, two triangles a side.
Against the same render without them the image moves by an RMS of 0.0085 over
0.53 per cent of pixels, which is what thirty-three hairs at 256 pixels should
be. The suite passes and the gallery is unmoved.

---

## 2026-09-09 -- Confirmed: a render gets faster the more times you run it

Paolo loaded ALab's `lab_structure01` in a viewer, found it slow, hid `/root`,
unhid it, and found the same scene faster than it had been on load. His reading
was that something in hdClaude's own code was involved. Reproduced here, and it
is confirmed as an effect while being cleared of that particular cause.

**It is not stale scene data.** Publishing the same stage twice in one process
produces byte-identical snapshots -- 339 prototypes, 2074 instances, 21937256
triangles, 902 textures both times -- and a top-level structure spanning exactly
the same translations, `(-539.863 -81.4423 -50.1653)` to `(321.799 230.282
412.85)`. Republishing does not make the trace faster either: 39.5 ms against
45.4 ms for the extend kernel, which is noise in the wrong direction. Whatever
the viewer's second look benefits from, it is not a better snapshot.

**It is the render repeating.** Six renders of the identical scene in one
process, with the same acceleration structure and the same rays:

    lab_structure01   7733  7052  4366  4211  4291  4200 ms
    chess_board        200   173   172   171   172   173 ms

The first render is **1.84x slower** than the settled state on the ALab asset,
and 1.16x on the chess set. The effect is real, it is large, and it is exactly
what hiding and unhiding a root would expose: the second look is warm.

**And the timing and the divergence are the same phenomenon.** The hashes settle
on the very render the timing does. Renders three through six all report
`hitHash 1997098710`; renders one and two report different values and different
ray counts. The renders that are slow are precisely the renders that give
different answers.

That ties this to the shader-cache finding earlier today, and the two together
now say one thing rather than two: a pipeline the driver has not finished
optimising is **both slower and numerically different**, and once it settles the
renderer is fast and reproducible at the same moment. It also explains why
restoring the driver's cache fixed the divergence -- a cached pipeline starts
optimised -- and why a warm-up of a single sample did not: the cold period here
lasts two entire renders, not one dispatch.

**A correction follows.** Every kernel profile in the previous entry was taken
on the first sample of the first render, which is the coldest measurement
available. Re-measured warm, the numbers move by about a tenth: extend is
0.188 ms at subdivision 0 and **42.5 ms** at subdivision 1, against 0.249 and
46.3 cold. The 186x collapse survives warming, so the two effects are separate
and the traversal finding stands on its own -- but the figures in that entry are
the cold ones and should be read as such.

Two diagnostics are kept from this: the repeat report now carries each render's
own trace time as a delta rather than the running total, which is what made the
warm-up visible at all, and the top-level structure reports the span of its
instance translations under `HDCLAUDE_TRACE`, which is what ruled the snapshot
out. The forced republish that proved the snapshots identical is removed, having
answered its question.
