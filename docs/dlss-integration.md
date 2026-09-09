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

## 3. Guide buffers

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

## 4. Frame metadata

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

## 5. Reference mode is untouched

**Reference rendering never passes through a reconstruction backend.** With DLSS
present and absent, reference output must be bit-identical. This is a test, not
an intention: the phase 13 gate renders a gallery scene both ways and compares
hashes.

Gallery baselines are always reference renders. A reconstructed image is never
committed as a baseline.

## 6. Dependency handling

The DLSS SDK is proprietary and is **not committed**. `cmake/NvidiaDLSS.cmake`
fetches a pinned release of `NVIDIA/DLSS` with `FetchContent` into the
gitignored `_deps/` tree, exactly like every other dependency. If the fetch is
declined or unavailable, `HDCLAUDE_ENABLE_DLSS` turns off and the build
succeeds without it — DLSS is never a requirement of the default build.

Redistribution of the SDK's runtime binaries is governed by NVIDIA's licence.
hdClaude ships none of them; users obtain them through the fetch.

## 7. What DLSS is not allowed to do

Restating the non-goal because it is the one that erodes quietly:

DLSS must not be used to hide avoidable synchronisation stalls, broken MIS,
fireflies, NaNs, or biased transport. Reconstruction improves a correct image;
it does not repair an incorrect one. Any time reconstruction makes a bug less
visible, the bug is still the finding.

Practically: every reconstruction change is evaluated against the **converged
reference** by SSIM and by a temporal-stability metric, never by eye against the
previous reconstruction.
