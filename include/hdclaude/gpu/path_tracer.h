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

    float environmentColor[3] = {0.05f, 0.07f, 0.10f};
    float sunDirection[3] = {0.4f, 0.7f, 0.5f};
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

    /// Render `settings.samplesPerPixel` samples and return the resolved image
    /// as linear RGBA floats, row-major.
    ///
    /// Row 0 is the *bottom* of the image. That is Hydra's render-buffer
    /// convention, so the AOV write is a straight copy and nothing downstream
    /// has to remember to flip.
    std::vector<float> Render(std::uint32_t width, std::uint32_t height,
                              const RenderCamera& camera,
                              const RenderSettings& settings);

    /// The shade kernel source, joined to a generated material to produce a
    /// shading pipeline. Exposed so the Hydra layer can compile materials
    /// without duplicating knowledge of the ABI.
    const std::string& ShadeKernelSource() const { return _shadeKernelSource; }

  private:
    void EnsureResolution(std::uint32_t width, std::uint32_t height);
    void UploadTextures(const std::vector<TextureImage>& textures);
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
    VulkanBuffer _lightTable;
    std::uint32_t _lightCount = 0;

    // The scene's texture pool: each distinct image once, referred to by a
    // material's textureSlots.
    std::vector<VulkanImage> _texturePool;
    VulkanImage _placeholderTexture;
    VkSampler _sampler = VK_NULL_HANDLE;
    /// Per-material texture slots, parallel to _shade.
    std::vector<std::vector<std::uint32_t>> _materialTextureSlots;

    ComputePipeline _raygen;
    ComputePipeline _extend;
    ComputePipeline _environment;
    ComputePipeline _shadow;
    ComputePipeline _film;
    std::vector<ComputePipeline> _shade;

    // Path state, sized to the current resolution.
    std::uint32_t _width = 0;
    std::uint32_t _height = 0;
    VulkanBuffer _frameUniforms;
    VulkanBuffer _origin, _direction, _throughput, _radiance, _pixel, _rng;
    VulkanBuffer _hits, _counters, _activeQueue, _nextActiveQueue, _shadowRays;
    VulkanBuffer _accumulation;
    VulkanBuffer _readback;
};

}  // namespace hdclaude

#endif  // HDCLAUDE_GPU_PATH_TRACER_H
