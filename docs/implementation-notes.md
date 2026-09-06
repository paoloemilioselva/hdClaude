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
where it can wait.

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
two are then impossible to separate in an authored scene.

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
