# hdClaude architecture

Status: design of record. Last revised 2026-09-05.

`hdClaude` is an out-of-tree OpenUSD Hydra render delegate whose renderer is a
**spectral, wavefront GPU path tracer** with **shading executed from
MaterialX-generated code**. This document states the design and, where it
departs from the obvious choice, the reason.

Related documents:

- [Roadmap and phase tracker](roadmap.md) — what is built, what is next, exit gates.
- [MaterialX code generation](materialx-codegen.md) — the `genglsl_pt` target.
- [Spectral rendering](spectral-rendering.md) — wavelengths, upsampling, sensor.
- [Wavefront integrator](wavefront-integrator.md) — queues, kernels, scheduling.
- [DLSS integration](dlss-integration.md) — guides, resolution, history.
- [Lessons from hdCodex](lessons-from-hdcodex.md) — defects inherited as rules.
- [Debugging a render](debugging-a-render.md) — how a reported image is turned into a measurement.
- [Implementation notes](implementation-notes.md) — findings that corrected this design.

## 1. The three commitments

Everything below is downstream of three commitments that are not negotiable
inside this project.

### 1.1 MaterialX shaders are compiled and executed, never reinterpreted

The renderer contains **no** parameter extractor for OpenPBR, Standard Surface,
UsdPreviewSurface, or any other named surface model, and **no** hand-written
"closure ABI" that a MaterialX graph is lowered into. A bound material is
compiled by MaterialX's own shader generator into GLSL, compiled to SPIR-V, and
*executed*. Pattern graphs — `image`, `noise3d`, `ramplr`, `tiledimage`,
arbitrary user nodegraphs, arbitrary math — work because MaterialX generates
them, not because hdClaude recognises them.

The one thing stock MaterialX GLSL generation does not emit is BSDF importance
sampling. hdClaude supplies that as a **derived MaterialX target**,
`genglsl_pt`, declared with `<targetdef name="genglsl_pt" inherit="genglsl"/>`
and a set of implementation overrides for the `pbrlib` closure nodes only. This
is the mechanism the MaterialX documentation prescribes for a new render target
— the same one `essl` and `genmsl` use to specialise `genglsl`. See
[materialx-codegen.md](materialx-codegen.md).

Consequence: a material either compiles and is rendered with full fidelity, or
it fails to compile and is reported. There is no third "approximated" state.

**`UsdPreviewSurface` is not an exception to this, and hdClaude renders it.**
MaterialX declares `ND_UsdPreviewSurface_surfaceshader` and implements it as
`IMP_UsdPreviewSurface_surfaceshader`, a nodegraph of ordinary MaterialX nodes;
generating it yields calls to `mx_generalized_schlick_bsdf`,
`mx_oren_nayar_diffuse_bsdf` and `mx_dielectric_bsdf`, which are hdClaude's own
`genglsl_pt` overrides. Nothing in hdClaude knows what `diffuseColor` or
`roughness` mean. What the Hydra layer adds is a **rename**:
`HdMtlxCreateMtlxDocumentFromHdNetwork` looks a node's type up as a MaterialX
nodedef name, and a USD-authored network carries USD's own names
(`UsdPreviewSurface`, `UsdUVTexture`, `UsdPrimvarReader_float2`), so those are
mapped to the `ND_` names MaterialX declares for them -- same inputs, same
names, same values -- plus one enum whose spelling differs (`repeat` against
`periodic`). Refusing that translation was not fidelity; it left MaterialX's
own translation unused and cost every material in a USD-authored asset.

A terminal MaterialX does *not* declare is still reported and shaded with
`displayColor`, which remains the only non-MaterialX path and is a fallback
rather than a shading model.

### 1.2 Spectral is the transport representation

Path throughput is carried on **four correlated hero wavelengths** in a `vec4`.
Authored RGB reflectances are upsampled to smooth, bounded, energy-conserving
spectra (Jakob-Hanika sigmoid polynomials); emission uses a separate unbounded
upsampling; IOR is wavelength-dependent where the material says so. The virtual
sensor integrates CIE XYZ and converts to linear sRGB once, at the film. RGB is
an asset-input and display-output format only.

Four lanes rather than three: a `vec4` is the natural GPU register width, hero
wavelength MIS is materially better with four samples, and the interactive
preview must not drop to a *different* estimator (an hdCodex defect — see
[lessons](lessons-from-hdcodex.md), N9).

### 1.3 Only public OpenUSD APIs

No private OpenUSD headers, no `hdEmbree` implementation symbols, no vendored
USD source. This is what makes an out-of-tree build against a stock
distribution possible.

## 2. Why a wavefront path tracer

hdCodex used a single megakernel compute shader with `VK_KHR_ray_query`
traversal and an interpreted material ABI. That is the right *first* renderer.
It is the wrong renderer for commitment 1.1, for one structural reason:

> A megakernel must contain the union of every material's code. Once materials
> are real compiled MaterialX programs of unbounded size, that union does not
> fit in a register budget, and occupancy collapses for every pixel in the
> frame — including the ones shaded by a trivial material.

The wavefront architecture solves exactly this. Path state lives in device
memory; the frame is executed as a sequence of kernels over *queues*; and
material shading is dispatched **once per distinct material**, so each dispatch
contains only that one MaterialX program.

```text
                    +--------------------------------------+
                    |  per bounce (indirect dispatches)     |
   ray generation   |                                       |
   ---------------> |  extend  -->  sort  -->  shade[m]  ---+--> escaped -> env
   (camera rays,    |  (trace)     (by mat)   (N dispatch)  |    absorbed -> film
    spectral,       |                              |        |
    jittered)       |                              v        |
                    |                       shadow queue    |
                    |                       (batched trace) |
                    +---------------------------------------+
                                       |
                                       v
                          film -> reference accumulation (RGBA32F)
                               -> interactive frame + guides -> DLSS
```

Kernel set, all compute, all launched with `vkCmdDispatchIndirect` from
GPU-written counters so the CPU never reads a queue length:

| Kernel | Role |
| --- | --- |
| `raygen` | camera rays, spectral wavelength sampling, jitter, path state init |
| `extend` | closest-hit traversal for the active path queue |
| `sort` | counting sort of hit records into per-material queues |
| `shade_<material>` | one specialised dispatch per material; MaterialX program |
| `shadow` | any-hit traversal of the shadow-ray queue, film-resolves NEE |
| `film` | resolve, accumulate, write guides |

### 2.1 Traversal: ray query in compute, not an RT pipeline

Traversal uses `VK_KHR_ray_query` inside the `extend`/`shadow` compute kernels
rather than `VK_KHR_ray_tracing_pipeline`. In a wavefront design the RT
pipeline's shader-binding-table dispatch buys nothing — we have already
separated shading from traversal, which is the whole point of an SBT — while it
costs portability and a second shader-compilation path.

**Shader Execution Reordering is not used, and cannot be.** An earlier version
of this document said the `extend` kernel would issue a
`VK_NV_ray_tracing_invocation_reorder` hint on the hit material index. That is
not possible: SER's builtins are declared only on the ray-generation,
closest-hit, and miss stages, never on compute, and the extension additionally
requires `VK_KHR_ray_tracing_pipeline` to be enabled alongside it. The
capability is detected and reported, but the extension is not enabled. See
[implementation-notes.md](implementation-notes.md).

This costs little, for a structural reason: **the per-material sort already
delivers the execution coherence SER exists to recover.** SER's value is to
megakernel and RT-pipeline designs, which cannot sort because shading and
traversal are fused. Having separated them and paid for the sort, hdClaude would
be buying the same coherence twice.

### 2.2 Why sorting is not optional here

Sorting is what makes 1.1 affordable. Without it, `shade` would have to branch
over every material per lane. With it, `shade_<material>` is a dispatch whose
every invocation runs the same MaterialX program with the same register
footprint. The cost is one counting pass and one scatter over the hit buffer
per bounce; the benefit is that adding a 400-node procedural nodegraph to one
object does not slow down any other object.

## 3. Layering

```text
src/core      no Vulkan, no OpenUSD. Hashing, shader cache, spectral tables,
              display transform, scene value types. Unit-testable on any host.

src/gpu       Vulkan 1.3 device, memory, acceleration structures, the wavefront
              scheduler and its kernels, the film, the DLSS backend boundary.
              Knows nothing about USD.

src/materialx MaterialX document construction, the genglsl_pt shader generator,
              GLSL->SPIR-V compilation, and the material program cache.
              Knows nothing about Vulkan resource layout beyond the ABI header.

src/hydra     pxr::HdClaude* adapters. Sync() produces immutable scene snapshots.
              Knows nothing about Vulkan.
```

Dependencies point strictly downward. `src/hydra` never includes a Vulkan
header; `src/gpu` never includes a `pxr` header. The GPU boundary is injected,
so scene synchronisation and cache behaviour are testable with no GPU and no
USD runtime present.

Namespaces: Hydra-discoverable classes are `pxr::HdClaude*` with an
`HDCLAUDE_API` export macro; everything else is `hdclaude::`.

## 4. Frame lifecycle — one entry point

The renderer exposes a **single frame-scoped entry point**, not a set of
independent setters:

```cpp
FrameHandle BeginFrame(const FrameDescription&);   // camera, extents, mode,
                                                   // scene revision, sample range
void        EndFrame(FrameHandle);
```

`FrameDescription` carries everything that could invalidate anything, so the
invalidation decision is made **once, in one place**. hdCodex's `SetScene()` /
`SetShadingMode()` / implicit-resize-inside-`Trace()` triad each had to guess
what the others implied, and each guessed wrong in a different way; that triad
is the direct cause of four separate shipped defects. See
[lessons](lessons-from-hdcodex.md), D3.

Every frame carries an identity. A result returned to Hydra is tagged with the
frame that produced it and the extents it was rendered at — never re-derived
from whatever the current extents happen to be.

## 5. Two accumulation contracts, never mixed

| | Reference | Interactive |
| --- | --- | --- |
| Storage | RGBA32F persistent average | per-frame noisy RGBA16F + guides |
| Owner of history | the renderer | the reconstruction backend |
| Buffer | its own, never resized by interaction | its own, per frame slot |
| Estimator | 4 spectral lanes, full bounces | **4 spectral lanes**, fewer bounces/spp |

The interactive preview reduces *sample count and path length*. It does not
change the estimator: the same wavelengths, the same MaterialX programs, the
same MIS. A preview that is a different estimator produces chromatic bias that a
temporal reconstructor will happily lock in.

The two contracts never share a buffer. Sharing one was the root cause of four
hdCodex findings at once — stale descriptors, forced device stalls, allocation
churn, and a submission ring that could not overlap. See
[lessons](lessons-from-hdcodex.md), D2/N3/N5.

## 6. Resource rules

These are project rules, not suggestions, and each exists because its violation
shipped in the reference project.

1. **Build, then publish, then rebind.** Never destroy a resource before its
   replacement exists. Construct into locals, swap on success, update
   descriptors last. Applies to buffers, images, pipelines, and pipeline
   layouts alike.
2. **Invalidate by identity, not by shape.** A descriptor that names a buffer is
   invalidated when that buffer's *handle* changes, not when the extents that
   motivated the change happen to differ. Every GPU resource carries a
   generation counter; descriptor sets cache the generation they were written
   against.
3. **Every Vulkan call is checked.** `vkDeviceWaitIdle` included. A sticky
   `deviceLost` latch is set by the checker and tested at the top of every
   public entry point; the destructor honours it and skips waits.
4. **Failure is reported as failure.** `IsConverged()` stays false on error,
   `TF_RUNTIME_ERROR` is raised (not `TF_WARN`, which `usdrecord` ignores), and
   the failure is surfaced through `HdRenderDelegate::GetRenderStats()`. A
   failed scene revision is retried, not committed.
5. **Image layout is per-frame-slot state.** With multiple command buffers in
   flight, a single CPU-side "current layout" shadow describes what was last
   *recorded*, not what the GPU will be in. Transitions take an explicit source
   layout owned by the slot.

## 7. Acceleration structures and geometry

Object-space prototypes; one BLAS per distinct published prototype,
fingerprinted by vertex/index signature and opacity class; native TLAS instances
for regular meshes, `PointInstancer` expansion, and nested Hydra instancers.
Unchanged prototypes retain their BLAS across publications. Transform-only
changes update the TLAS in place. Stable-topology deformation (UsdSkel via Hydra
`extComputation`) updates its BLAS in place; topology or opacity-class changes
rebuild only that BLAS.

Subdivision uses public `HdMeshTopology`/`PxOsd`/OpenSubdiv to produce cached
uniform Catmull-Clark, Loop, or bilinear refinement, preserving creases,
corners, holes, orientation, face-varying seams, and material subsets.

How deep each mesh is refined can be chosen from the camera rather than fixed
for the stage: `Adaptive subdivision` gives a mesh the level at which its
refined edges are about a chosen number of pixels long. The view it is derived
from is *sampled* — taken once and held — because published geometry is what
acceleration structures are built over and what the accumulated film depends
on, so following the camera would rebuild both on every nudge; a setting turns
following on and `Retessellate` asks for a fresh sample. Geometry outside the
frustum is refined to a floor rather than culled, because a path tracer sees
what the camera does not. What limits the cost is a budget on refined faces,
which applies whether or not the level is adaptive.

MaterialX displacement is evaluated on the refined mesh **by the same generated
MaterialX program** used for shading — a `displacementshader` output compiled
through the same `genglsl_pt` path and run as a GPU compute pass, not a CPU
reimplementation. This is the same commitment as 1.1 applied to geometry.

## 8. AOV delivery to Hydra

This decision is made explicitly because two later phases depend on it.

**Answer for the delegate: CPU readback is the Hydra adapter.** `VulkanContext`
owns a private `VkInstance`/`VkDevice`; the Hydra host owns a different one, and
no public OpenUSD API hands a `VkImage` across devices. The readback is
double-buffered with one frame of latency and overlaps its host copy with the
*next* frame's execution — with per-slot storage, so the overlap is real GPU
overlap and not merely recording overlap.

**Later phase:** `VK_KHR_external_memory_win32` export surfaced through
`HdRenderBuffer::GetResource()`, so an Hgi host can consume the image directly.
This is a phase of its own with its own gate, not a footnote.

Frame Generation and Reflex are explicitly **out of scope for a delegate** —
they require owning presentation, and belong to a separate viewer project.

## 9. Non-goals

- No NVIDIA-only requirement in the default build. DLSS is optional and
  discovered at runtime; the renderer-native reconstruction fallback must work
  on any Vulkan 1.3 ray-query device.
- No lowering of MaterialX semantics to fit a denoiser, and no guide buffer
  derived from a surface-model *name*.
- No display-encoded or tone-mapped values fed to a reconstruction backend.
- No DLSS used to hide synchronisation stalls, broken MIS, fireflies, NaNs, or
  biased transport. Reconstruction improves a correct image; it does not repair
  an incorrect one.
- No "approximate" material path. See 1.1.
