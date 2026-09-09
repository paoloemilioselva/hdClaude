# NVIDIA DLSS integration

Status: design of record. Last revised 2026-09-05.

Real-time path tracing at production sample counts is not achievable by sampling
alone. hdClaude reaches interactive rates by rendering a correct low-sample
image and reconstructing it. This document specifies that boundary.

## 1. Scope

**In scope for the delegate:**

- DLSS Super Resolution (upscaling)
- DLAA (native-resolution anti-aliasing)
- DLSS Ray Reconstruction (denoising a 1-2 spp path-traced frame)

**Out of scope for the delegate:**

- Frame Generation, Multi Frame Generation, Reflex.

The reason is structural: those features act on presentation, and a Hydra
delegate does not own a swapchain. They belong to a separate viewer project that
owns presentation, and putting them here would require the delegate to acquire
responsibilities Hydra does not give it.

This is also why hdClaude integrates **NGX directly** (`nvsdk_ngx_vk`) rather
than through Streamline. Streamline's reason to exist is the presentation-layer
features we are not implementing; using it here would add an interposer and a
plugin loading path for no benefit.

## 2. The renderer-neutral boundary

NVIDIA types never appear in a renderer-neutral or Hydra-facing header.

```cpp
class ReconstructionBackend {
public:
    virtual ~ReconstructionBackend() = default;
    virtual const char* Name() const = 0;
    virtual ReconstructionSizing QuerySizing(uint32_t outputWidth,
                                             uint32_t outputHeight,
                                             ReconstructionQuality) const = 0;
    virtual bool Resize(VkCommandBuffer, const ReconstructionResolution&,
                        std::string* reason) = 0;
    virtual void Evaluate(VkCommandBuffer, const ReconstructionFrame&) = 0;
    virtual void ResetHistory() = 0;
};
```

Built in phase 12, and it differs from the sketch above it in three ways, each
forced by what DLSS actually is rather than chosen:

- **`Resize` takes a command buffer and can fail.** DLSS's feature creation is
  *recorded*, not immediate: it initialises device state and needs somewhere to
  put that work, and the buffer must be submitted and complete before `Evaluate`
  is recorded against the feature. It can also decline, so it returns a reason
  rather than nothing.
- **`QuerySizing` replaces `QuerySupport`.** Support is answered by
  `CreateNgxBackend` returning null with a reason, which is the same answer at
  the moment it matters. What a caller genuinely needs from a live backend is
  the render extent, because DLSS chooses that per quality mode and rendering at
  a size it did not ask for either wastes work or starves its model.
- **Vulkan types cross the boundary.** Images and command buffers are how any
  GPU reconstruction is expressed. The rule was always about NVIDIA types, and
  that still holds: `NVSDK_NGX_*` appears in exactly one translation unit.

The image contract is stated with the interface: `color`, `depth` and `motion`
in `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL` with `VK_IMAGE_USAGE_SAMPLED_BIT`,
and `output` in `VK_IMAGE_LAYOUT_GENERAL` with **both**
`VK_IMAGE_USAGE_STORAGE_BIT` and `VK_IMAGE_USAGE_TRANSFER_DST_BIT` -- the first
because NGX refuses a read-write resource without it, the second because DLSS
clears the output itself before writing to it.

Three implementations are planned, selected at runtime:

| Backend | Availability | Phase |
| --- | --- | --- |
| `NoneBackend` | always; passes the noisy frame through | with the film |
| `NativeBackend` | any Vulkan 1.3 device; SVGF-class spatiotemporal filter | 11 |
| `NgxBackend` | NVIDIA RTX with a supported driver | 12-14 |

`NativeBackend` is not a placeholder. A renderer that only reaches interactive
rates on one vendor's hardware is a worse renderer, and the native path is what
proves the guide buffers are correct independently of a closed-source model.

Backends contribute their Vulkan instance and device requirements through a
construction-time provider, queried **before** device selection, so an optional
backend cannot force a device choice on a system that will not use it.

## 3. Where a frame meets a backend

The integrator's film and guides are storage buffers, indexed by path, because
every kernel in a wavefront integrator indexes them that way. A backend reads
textures. `shaders/reconstruct_inputs.comp.glsl` is the one place those two
facts meet: it reads the film and the two guides out of their buffers, divides
the film by the sample count each pixel received, and writes the three images
DLSS is handed.

It could have been avoided by having `film` and `guides` write images directly.
That would have been the wrong trade. A reference render has to be bit-identical
whether or not a backend exists (§6 below), and the surest way to keep it so is
for the kernels that produce it never to learn that reconstruction exists. As it
stands, a reference frame pays nothing for reconstruction: not a dispatch, not a
barrier, not an image allocation, and not the compile of the packing kernel,
which happens the first time a frame asks to be reconstructed and never in a run
that does not.

**Extents.** The render extent is the backend's answer, not the caller's
request. `FrameDescription::width` and `height` are the size the *image* comes
back at; `QuerySizing` decides what is traced to produce it, and
`FrameResult::renderWidth` and `renderHeight` report it. Everything upstream of
the backend -- the trace, the film, the guides, the jitter, the motion vectors --
is in render pixels, and only the image that comes out is in output pixels. So a
caller reading the depth AOV of an upscaled frame must read it at the render
extent, which is why that extent travels on the result rather than being left to
be inferred from the image.

**Failure is a smaller frame, never a black one.** A backend that cannot be
created, cannot be built for these extents, or declines to evaluate leaves the
frame exactly as it was traced, with `reconstructed` false and
`PathTracer::ReconstructionUnavailable()` saying why. The renderer's behaviour
must not depend on the machine any more than it has to, and an optional backend
that turned its own absence into a failed frame would be the opposite of that.

**The provider is a construction-time obligation, and a real one.** NGX will not
initialise without instance and device extensions of its own, and those can only
be enabled while the instance and the device are being created -- long before
anything decides to reconstruct anything. A context built without
`NgxRequirementProvider` reports that the *device* cannot run DLSS, which is
untrue and unfixable from where it is read. Both the Hydra delegate and the
render tests install it unconditionally; with no SDK it names nothing, and the
context enables only extensions the chosen device actually has.

**Formats.** Colour `R16G16B16A16_SFLOAT`, depth `R32_SFLOAT`, motion
`R16G16_SFLOAT`, all written as storage images by the packing kernel and read as
sampled images by the backend. Vulkan requires storage support of the first and
of neither of the others, so support is asked of the device and a device that
says no is reported rather than left to fail at the first dispatch.

## 4. Guide buffers

Ray Reconstruction needs more than colour. Every guide below is produced by the
`film` and `shade` kernels from values the **MaterialX closures themselves**
report — `guideAlbedo` and `guideRoughness` on the `BSDF` struct (see
[materialx-codegen.md](materialx-codegen.md) §2). No guide is derived from a
surface-model name; that is a stated non-goal, and it is also the only way
guides can work for an arbitrary authored nodegraph.

| Guide | Format | Contract |
| --- | --- | --- |
| Noisy colour | `R16G16B16A16_SFLOAT` | linear RGB, HDR, current frame only, pre-exposure applied |
| Depth | `R32_SFLOAT` | non-linear device depth matching the supplied projection |
| Motion | `R16G16_SFLOAT` | pixel-space, current-to-previous, jitter handled per SDK contract |
| Normal + roughness | `R16G16B16A16_SFLOAT` | world-space normal, linear perceptual roughness |
| Diffuse albedo | `R16G16B16A16_SFLOAT` | demodulation albedo of the diffuse lobes |
| Specular albedo | `R16G16B16A16_SFLOAT` | demodulation albedo of the specular lobes |

Guides describe the **primary visible surface**. For a path that starts on a
perfect mirror, the first non-delta surface is the one that carries the guide —
otherwise a mirror reconstructs as a flat colour. This is a real subtlety and is
handled explicitly in `shade`, not left to chance.

The colour handed to a backend is linear RGB, never CIE XYZ and never
display-encoded. The spectral-to-XYZ-to-linear-sRGB conversion happens in `film`
before the backend sees anything.

## 5. Frame metadata

```cpp
struct FrameMetadata {
    Matrix4  currentWorldToView, currentViewToClip;
    Matrix4  previousWorldToView, previousViewToClip;
    Vec2     jitterPixels, previousJitterPixels;
    Vec2     motionVectorScale;
    float    preExposure;
    uint64_t frameIndex;
    bool     cameraCut;      // teleport: history is invalid
    bool     historyReset;   // scene or settings changed
};
```

`cameraCut` and `historyReset` are distinct. A camera cut invalidates temporal
reprojection; a history reset invalidates the accumulated history itself. Scene
edits, material recompiles, resolution changes, and mode switches all raise
`historyReset`.

The jitter sequence must be the one the SDK expects — a Halton sequence of the
length DLSS specifies for the active quality mode — not the renderer's
preferred sequence. In DLSS modes, `raygen` takes the jitter from
`FrameMetadata` rather than generating its own.

What is shipped is the radical inverse in bases 2 and 3, indexed by the frame,
offset to `[-0.5, 0.5]` and measured from the pixel centre in render pixels;
`raygen` displaces the camera ray by it and `FrameResult::jitter` reports the
offset that was used rather than a sequence a backend is expected to reproduce.
A caller may state the offset instead of taking that sequence, by setting
`RenderSettings::fixedJitter`, and it is then used exactly as given — which is
what a host already driving a temporal pattern of its own needs, and what makes
the measurement below possible. A reference render ignores a stated offset and
says so once: an average of hundreds of independently jittered samples has no
single offset it could report.

**It is handed to DLSS negated**, and that is the one translation on this
boundary that is not a unit conversion. hdClaude's jitter says where the sample
*landed*, measured from the pixel centre. DLSS asks for something else that is
also called a jitter: "the jitter applied to the projection matrix", by the
recipe `ProjectionMatrix.M[2][0] += ProjectionJitter.X` (DLSS Programming Guide
3.7.2, 3.7.3). Offsetting a projection by `+d` moves the rendered content `+d`
across the screen, so the sample a pixel takes moves `-d`. The two numbers are
the same displacement seen from opposite ends.

The axes need no flip on top of that. The guide asks for the co-ordinate system
the motion vectors are in (3.7.3, point 4), and both hdClaude's motion vectors
and the images it hands over are in the row order of those images — DLSS never
learns which way is up, only that everything it is given agrees.

### How the sign was settled

It had been an open question, and the reason it survived is worth keeping: a
reversed sign does not break the image. Over a Halton sequence the errors are
symmetric about the pixel centre, so the damage is a softening, and nothing
distinguishes it from the softening a reconstructor legitimately produces.

Hold the offset still and it stops being a blur and becomes a **displacement**,
of twice the offset — DLSS resolves its history at the pixel centre by shifting
it by the jitter it was told, and a reversed sign shifts it the wrong way by
exactly as much as the samples were already displaced. At half a pixel on each
axis that is a whole pixel on each axis.

SSIM cannot see it. On the test scene — a sphere and a backdrop lit by one rect
light — SSIM against the converged reference reads 0.6675 aligned and 0.6655 a
whole pixel out, two parts in a thousand, and at one sample a frame the reversal
moves the DLAA figure by 0.004. A metric that answers the same either way is not
an instrument. What answers is measuring the displacement itself:
`hdclaude::EstimateShift` solves the Lucas-Kanade normal equations on a bilinear
warp and returns how far one image has moved relative to another, in pixels,
checked in the core suite against shifts known in closed form (recovered to
about 0.002 px).

Measured on an RTX 5060 Ti, 256x256, eight frames of 128 samples each at a held
offset of (0.5, 0.5):

| Sign handed to NGX | Displacement from the converged render |
|---|---|
| Negated, as shipped | **0.1302 px** |
| Passed through, as it was | 1.3934 px |

1.3934 is close to the 1.4142 a whole pixel on each axis would be, and short of
it because DLSS's resolve is not a pure translation. The gate asserts under
0.25 px, which no tolerance could be tuned to let the reversed sign through.

## 6. Reference mode is untouched

**Reference rendering never passes through a reconstruction backend.** With DLSS
present and absent, reference output must be bit-identical. This is a test, not
an intention, and it is checked two ways.

Within one process, `hdClaudeRenderTests` renders a reference frame before
anything has created a backend, creates one by asking for an interactive frame,
and renders the same reference frame again — comparing the two films byte for
byte with `memcmp`, because "untouched" has no epsilon. That is the stronger
half: the second render happens with NGX initialised and the backend alive,
rather than merely compiled in.

Across the two builds, the same test hashes the film and prints it. `dev` and
`dev-dlss` both answer `8e46df46f3ff2e79` on an RTX 5060 Ti, which is the claim
about *presence and absence of the SDK* that no single process can make.

Gallery baselines are always reference renders. A reconstructed image is never
committed as a baseline.

## 7. Dependency handling

The DLSS SDK is proprietary and is **not committed**. `cmake/NvidiaDLSS.cmake`
fetches a pinned release of `NVIDIA/DLSS` with `FetchContent` into the
gitignored `_deps/` tree, exactly like every other dependency. If the fetch is
declined or unavailable, `HDCLAUDE_ENABLE_DLSS` turns off and the build
succeeds without it — DLSS is never a requirement of the default build.

Redistribution of the SDK's runtime binaries is governed by NVIDIA's licence.
hdClaude ships none of them; users obtain them through the fetch.

## 8. What DLSS is not allowed to do

Restating the non-goal because it is the one that erodes quietly:

DLSS must not be used to hide avoidable synchronisation stalls, broken MIS,
fireflies, NaNs, or biased transport. Reconstruction improves a correct image;
it does not repair an incorrect one. Any time reconstruction makes a bug less
visible, the bug is still the finding.

Practically: every reconstruction change is evaluated against the **converged
reference** by SSIM and by a temporal-stability metric, never by eye against the
previous reconstruction. Both live in `hdclaude::` core with no Vulkan and no
OpenUSD, so their closed forms are checked on any host rather than only ever
exercised through a render, and both are stated as comparisons rather than
thresholds — the reconstructed frame must be a better picture of the converged
reference than the frame it was made from, and the reconstructed sequence must
be steadier than the sequence it was made from. Neither claim can be tuned
without making the renderer worse. A third, `EstimateShift`, answers where a
picture sits rather than how good it is; §5 says why that turned out to be a
question SSIM could not be asked.

Measured on an RTX 5060 Ti at 256x256, DLAA at native resolution over a
twenty-four frame sequence of one sample each: SSIM against the converged
reference 0.3755, against 0.0154 for the unreconstructed frame it was made
from; temporal instability 0.0252 against 0.7265. The SSIM is low in absolute
terms and is meant to be — a single path-traced sample is not the aliased
raster frame DLSS was trained on, which is what Ray Reconstruction is for
(phase 14).
