// The wavefront path tracer.
//
// Owns the path state, the queues, the kernel pipelines, and the film, and
// drives a frame as a sequence of dispatches. Knows nothing about OpenUSD: it
// takes a Scene and a set of compiled material modules, both of which the
// Hydra layer produces.

#ifndef HDCLAUDE_GPU_PATH_TRACER_H
#define HDCLAUDE_GPU_PATH_TRACER_H

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "hdclaude/gpu/acceleration_structure.h"
#include "hdclaude/gpu/compute_pipeline.h"
#include "hdclaude/gpu/glsl_compiler.h"
#include "hdclaude/gpu/scene.h"
#include "hdclaude/gpu/vulkan_context.h"
#include "hdclaude/gpu/vulkan_resources.h"

namespace hdclaude {

/// Camera for one frame. A world matrix and a field of view, so the delegate
/// can hand over what Hydra gives it without inventing a convention.
struct RenderCamera {
    /// Column-major camera-to-world, matching GLSL's mat4.
    float cameraToWorld[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    float tanHalfFov = 0.414f;  // ~45 degrees vertical
    float aspect = 1.0f;
};

/// Lighting and sampling settings.
///
/// The environment and sun stand in for UsdLux, which arrives with the Hydra
/// delegate. Deliberately simple rather than approximate: nothing here has to be
/// unlearned when real lights land.
struct RenderSettings {
    std::uint32_t samplesPerPixel = 64;
    std::uint32_t maxBounces = 4;

    /// Index of the first sample this call traces.
    ///
    /// Progressive rendering is expressed as a sequence of calls that continue
    /// where the last left off, so an interactive host can spend a few
    /// milliseconds at a time instead of blocking for a whole image. The index
    /// seeds the sampler, so continuing at the right offset is what keeps the
    /// sequence decorrelated rather than re-tracing the same paths.
    std::uint32_t firstSample = 0;

    /// Clear the film before tracing. False continues an accumulation, which
    /// is only correct when the scene, the camera and the resolution are all
    /// unchanged since the previous call.
    bool resetAccumulation = true;

    float environmentColor[3] = {0.30f, 0.38f, 0.52f};
    /// Direction toward the stand-in sun, at 70 degrees of elevation for a
    /// Y-up stage. The Hydra render pass re-aims this about the stage's
    /// actual up axis, which it has to be told; this default is what a
    /// direct user of `PathTracer` gets.
    float sunDirection[3] = {0.24184476f, 0.93969262f, 0.24184476f};
    float sunAngularRadius = 0.02f;
    float sunRadiance[3] = {3.0f, 2.9f, 2.7f};
};

/// A material ready to shade with: the SPIR-V of its generated MaterialX
/// program joined to the shade kernel.
struct CompiledMaterial {
    std::vector<std::uint32_t> spirv;
    std::string debugName;

    /// Where this material's textures live in the scene's texture pool.
    ///
    /// The generator numbers a material's samplers from zero, so index i in
    /// the generated code means "this material's i-th texture". That local
    /// index is resolved against this table when the material's descriptor set
    /// is written, which is what lets two materials both use local index 0 for
    /// different images while one pool holds each distinct image once.
    std::vector<std::uint32_t> textureSlots;

    /// The Abbe number of this material's transmission, or zero for none.
    ///
    /// The one material property that travels beside the program instead of
    /// inside it. MaterialX 1.39.3's `open_pbr_surface` declares
    /// `transmission_dispersion_abbe_number` and
    /// `transmission_dispersion_scale`, threads both into the generated
    /// function's signature, and reads neither; `ND_dielectric_bsdf` has no
    /// dispersion input to receive them. So the graph accepts dispersion,
    /// carries it as far as the function that would use it, and drops it, and
    /// no closure can be handed it through the ABI. The material compiler reads
    /// it from the authored network and the integrator applies it
    /// (docs/implementation-notes.md, 2026-09-07).
    ///
    /// Already scaled: OpenPBR's `transmission_dispersion_scale` multiplies the
    /// dispersive power `1 / V`, so a scale of zero -- the default, and what
    /// every material that does not ask for dispersion has -- leaves this zero.
    float dispersionAbbe = 0.0f;
};

/// Resolves `#include` directives in the kernel sources.
///
/// MaterialX resolves its own includes during generation, so the compiler
/// itself needs no includer; hdClaude's own kernels do, and this is the one
/// place that knows where they live.
std::string ResolveKernelIncludes(const std::filesystem::path& directory,
                                  const std::string& source);

/// Read a kernel source with its includes resolved.
std::string LoadKernel(const std::filesystem::path& directory,
                       const std::string& name);

/// Which accumulation contract a frame is rendered under.
///
/// The two never share storage and never share history (docs/architecture.md 5).
/// Interactive reduces sample count and path length; it does not change the
/// estimator, because a preview that is a different estimator produces
/// chromatic bias a temporal reconstructor will lock in.
enum class RenderMode {
    /// Persistent average, owned by the renderer. What the gallery renders.
    Reference,
    /// Per-frame noisy image plus guides, history owned by a reconstruction
    /// backend. Nothing produces one yet; the mode exists so the frame that
    /// carries it can be told apart from a reference frame, and so the
    /// invalidation below can treat a switch between them as the reset it is.
    Interactive,
};

/// Everything that could invalidate anything, in one place.
///
/// This is the single frame-scoped entry point's argument, and the reason it
/// exists is a lesson rather than a preference: hdCodex's `SetScene()` /
/// `SetShadingMode()` / implicit-resize-inside-`Trace()` triad each had to guess
/// what the others implied, each guessed wrong differently, and that triad is
/// the direct cause of four shipped defects (docs/lessons-from-hdcodex.md D3).
/// Carrying the lot in one struct is what lets the invalidation decision be
/// made once.
struct FrameDescription {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    RenderCamera camera;
    RenderSettings settings;
    RenderMode mode = RenderMode::Reference;

    /// The scene revision this frame is rendered against.
    ///
    /// The renderer does not fetch the scene from this -- `SetScene` publishes
    /// it -- but a frame that names a different revision than the last one
    /// cannot continue its accumulation, and saying so here is what stops the
    /// caller having to remember.
    std::uint64_t sceneRevision = 0;
};

/// A frame in progress. Returned by BeginFrame and consumed by EndFrame.
///
/// Opaque on purpose: it carries the identity the result will be tagged with,
/// and nothing a caller should read directly.
struct FrameHandle {
    std::uint64_t index = 0;
    bool valid = false;
};

/// A finished frame, and what it is a frame *of*.
///
/// The extents travel with the image rather than being re-derived from whatever
/// the renderer's current extents happen to be. That is the whole point: a host
/// that resized between submitting a frame and receiving it would otherwise
/// write an image of one size into a buffer of another, which is how hdCodex
/// produced findings A2, A1, N1 and N8.
struct FrameResult {
    std::uint64_t index = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    /// The sample range this frame added, for a caller tracking convergence.
    std::uint32_t firstSample = 0;
    std::uint32_t sampleCount = 0;

    /// True when the description's accumulation was restarted, whether the
    /// caller asked for it or the renderer decided it. A caller that tracks its
    /// own sample count needs to know which happened.
    bool accumulationReset = false;

    /// Linear RGBA, row-major, row 0 at the *bottom* -- Hydra's render-buffer
    /// convention, so the AOV write is a straight copy.
    std::vector<float> image;

    bool Valid() const { return width != 0 && height != 0 && !image.empty(); }
};

class PathTracer {
  public:
    PathTracer(const VulkanContext& context, VulkanAllocator& allocator,
               std::filesystem::path shaderDirectory);
    ~PathTracer();

    PathTracer(const PathTracer&) = delete;
    PathTracer& operator=(const PathTracer&) = delete;

    /// Publish a scene and the materials its instances reference.
    ///
    /// `materials` is indexed by the `material` field of a MeshInstance, so
    /// there is one compiled pipeline per distinct material and the index in
    /// the scene is the index here.
    void SetScene(const Scene& scene, const std::vector<CompiledMaterial>& materials);

    /// Begin a frame. The one entry point; nothing else invalidates anything.
    ///
    /// Every decision this frame implies -- whether the resolution changed,
    /// whether the accumulation can continue, whether the mode switched -- is
    /// taken here, from the description alone, and taken once.
    ///
    /// The trace itself still happens inside this call and `EndFrame` only
    /// hands back what it produced. That is phase 9's first step and not its
    /// last: the split exists so that moving the submit here and the wait there
    /// changes the renderer and not its callers. Until that lands there is no
    /// overlap, and the phase 9 gate stays unmet.
    FrameHandle BeginFrame(const FrameDescription& description);

    /// Finish a frame and take its result, tagged with the extents it was
    /// rendered at.
    FrameResult EndFrame(FrameHandle handle);

    /// Render `settings.samplesPerPixel` samples and return the resolved image
    /// as linear RGBA floats, row-major.
    ///
    /// Row 0 is the *bottom* of the image. That is Hydra's render-buffer
    /// convention, so the AOV write is a straight copy and nothing downstream
    /// has to remember to flip.
    ///
    /// A convenience over BeginFrame/EndFrame for a caller with no interest in
    /// frame identity, which is every test and the gallery. It cannot overlap
    /// anything by construction, so it will stay a convenience rather than
    /// becoming the interactive path.
    std::vector<float> Render(std::uint32_t width, std::uint32_t height,
                              const RenderCamera& camera,
                              const RenderSettings& settings);

    /// The shade kernel source, joined to a generated material to produce a
    /// shading pipeline. Exposed so the Hydra layer can compile materials
    /// without duplicating knowledge of the ABI.
    const std::string& ShadeKernelSource() const { return _shadeKernelSource; }

    /// How many paths each material's dispatch covered in the last bounce of
    /// the last completed `Render`, read back from the device.
    ///
    /// This is the only way to observe the sort: the counts are written by the
    /// GPU and consumed by an indirect dispatch without ever crossing to the
    /// host during a frame, so a test that wants to assert the split has to ask
    /// afterwards. It is a diagnostic, not part of rendering -- calling it
    /// during a frame would be exactly the readback the design forbids.
    std::vector<std::uint32_t> MaterialCounts() const;

    /// Rays traced since construction, counted on the device.
    ///
    /// One per active path per bounce, and one per shadow ray shading asked
    /// for. Accumulated by the kernel that already reads those counts to size
    /// its dispatches, and read back once per call after every dispatch it
    /// describes has finished -- never during a frame, which is the stall this
    /// design exists to avoid.
    std::uint64_t TracedRays() const { return _tracedRayCount; }
    std::uint64_t ShadowRays() const { return _shadowRayCount; }

    /// A hash over every hit this tracer has resolved.
    ///
    /// Two runs that agree here resolved the same geometry for the same rays,
    /// whatever order they got to it in. It exists to answer one question that
    /// nothing else can: when two renders of one scene differ, is it what the
    /// rays *hit* or what was done with the hits.
    std::uint64_t HitHash() const { return _hitHash; }

    /// The same over the rays themselves, which is the other half of that
    /// question: two processes can trace the same *number* of rays without
    /// tracing the same rays. Read together, an agreeing ray hash and a
    /// disagreeing hit hash put the cause in the acceleration structure, and a
    /// disagreeing ray hash puts it upstream of traversal.
    std::uint64_t RayHash() const { return _rayHash; }

  private:
    /// The invalidation decision, made once and in one place.
    ///
    /// Returns whether the accumulation has to restart. A caller may also ask
    /// for a restart through `RenderSettings::resetAccumulation`; this decides
    /// the cases the caller cannot be relied on to notice.
    bool InvalidateFor(const FrameDescription& description);

    /// The trace itself: the body the old Render() was, unchanged.
    ///
    /// Private because it takes no frame identity and makes no invalidation
    /// decision -- both were lifted out of it, which is the whole of this step.
    std::vector<float> Trace(std::uint32_t width, std::uint32_t height,
                             const RenderCamera& camera,
                             const RenderSettings& settings);

    void EnsureResolution(std::uint32_t width, std::uint32_t height);
    void UploadTextures(const std::vector<TextureImage>& textures);
    /// Sample the colour matching functions and the illuminant, fit the
    /// chromaticity table, and upload all of it. Once, at construction: none of
    /// it depends on the scene.
    void BuildSpectralTables();

    VulkanImage UploadTexture(const TextureImage& texture);

    /// The image array a material's descriptor set should be written with.
    /// `material` indexes the compiled materials; a negative index means a
    /// kernel that never samples, which gets placeholders throughout.
    std::vector<VkDescriptorImageInfo> TextureBindingsFor(int material) const;
    void WriteDescriptors(VkDescriptorSet set, const ComputePipeline& pipeline,
                          int material = -1);

    const VulkanContext& _context;
    VulkanAllocator& _allocator;
    std::filesystem::path _shaderDirectory;
    GlslCompiler _compiler;
    std::string _shadeKernelSource;

    std::unique_ptr<SceneAccelerator> _accelerator;
    VulkanBuffer _instanceTable;
    std::uint32_t _instanceCount = 0;
    VulkanBuffer _triangleMaterials;
    VulkanBuffer _lightTable;
    std::uint32_t _lightCount = 0;

    // The scene's texture pool: each distinct image once, referred to by a
    // material's textureSlots.
    std::vector<VulkanImage> _texturePool;
    VulkanImage _placeholderTexture;
    VulkanImage _domeTexture;
    float _domeWorldToLight[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    float _domeLightToWorld[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    /// Whether the published scene supplied its own environment. The
    /// stand-in sun is withheld when it did.
    bool _hasDomeLight = false;
    float _domeColorTemperature = 0.0f;
    float _domeTemperatureScale = 1.0f;

    /// The dome map's sampling distribution, uploaded as one buffer. Always a
    /// real buffer -- a descriptor set cannot point at nothing -- and the
    /// kernels read `hasEnvironmentDistribution` rather than its size.
    /// The hero packet each path carries.
    VulkanBuffer _wavelengths;

    /// The colour matching functions, the illuminant, and the chromaticity
    /// table, in one buffer. Built once at construction: none of it depends on
    /// the scene, and the table costs the better part of a second to fit.
    VulkanBuffer _spectralTables;
    std::uint32_t _spectralSamples = 0;
    std::uint32_t _chromaTableOffset = 0;
    std::uint32_t _chromaTableSize = 0;
    float _spectralNormalisation = 1.0f;

    VulkanBuffer _environmentDistribution;
    std::uint32_t _environmentWidth = 0;
    std::uint32_t _environmentHeight = 0;
    std::uint32_t _environmentConditional = 0;
    std::uint32_t _environmentDensity = 0;
    VkSampler _sampler = VK_NULL_HANDLE;
    /// Per-material texture slots, parallel to _shade.
    std::vector<std::vector<std::uint32_t>> _materialTextureSlots;
    /// Per-material Abbe number, parallel to _shade. Pushed with the material
    /// id at each shading dispatch.
    std::vector<float> _materialDispersion;

    ComputePipeline _raygen;
    ComputePipeline _extend;
    ComputePipeline _prepareDispatch;
    ComputePipeline _materialSort;
    ComputePipeline _environment;
    ComputePipeline _shadow;
    ComputePipeline _film;
    std::vector<ComputePipeline> _shade;

    // Path state, sized to the current resolution.
    VulkanBuffer _rayReadback;
    std::uint64_t _tracedRayCount = 0;
    std::uint64_t _shadowRayCount = 0;
    std::uint64_t _hitHash = 0;
    std::uint64_t _rayHash = 0;

    std::uint32_t _width = 0;
    std::uint32_t _height = 0;

    // --- Frame identity and the last description accepted -------------------
    //
    // Held so the next frame can be compared against it. Nothing else reads
    // them: a frame's own extents travel in its FrameResult, because
    // re-deriving them from here is precisely the mistake this records exist to
    // make impossible.
    std::uint64_t _frameIndex = 0;
    bool _hasPreviousFrame = false;
    RenderMode _previousMode = RenderMode::Reference;
    std::uint64_t _previousSceneRevision = 0;

    /// Filled by BeginFrame, taken by EndFrame. One frame is in flight at a
    /// time in this step, which is why this is a single slot rather than a ring.
    FrameResult _pendingFrame;
    VulkanBuffer _frameUniforms;
    VulkanBuffer _origin, _direction, _throughput, _radiance, _pixel, _rng;
    VulkanBuffer _scatterPdf;
    /// Absorption coefficient of the medium each path is currently inside,
    /// or zero in vacuum. Written when a transmission event crosses into a
    /// surface whose closure published one.
    VulkanBuffer _medium;
    /// Whether each path has already been collapsed onto its hero wavelength
    /// by a dispersive surface, so the collapse compensates exactly once.
    VulkanBuffer _heroOnly;
    VulkanBuffer _hits, _counters, _activeQueue, _nextActiveQueue, _shadowRays;
    VulkanBuffer _accumulation;
    VulkanBuffer _readback;

    // The material sort. The queue is sized by the resolution; the table and
    // the indirect commands are sized by the number of materials, so they are
    // built when a scene is published rather than when the resolution changes.
    VulkanBuffer _materialQueue;
    VulkanBuffer _materialTable;
    VulkanBuffer _dispatchArgs;
};

}  // namespace hdclaude

#endif  // HDCLAUDE_GPU_PATH_TRACER_H
