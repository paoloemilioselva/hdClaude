// Compute pipelines, descriptor sets, and dispatch.
//
// Every kernel in the renderer is a compute pipeline: ray generation,
// traversal, the sort, the per-material shading dispatches, shadow rays, and
// the film. This is the machinery they all share.
//
// The binding table is declared once per pipeline and drives both the layout
// and the pool sizes. hdCodex restated its pool sizes as literals that exactly
// matched the layout with zero headroom, so adding one binding failed
// allocation at startup with no compile-time signal
// (docs/lessons-from-hdcodex.md R10).

#ifndef HDCLAUDE_GPU_COMPUTE_PIPELINE_H
#define HDCLAUDE_GPU_COMPUTE_PIPELINE_H

#include <cstdint>
#include <string>
#include <vector>

#include <volk.h>

#include "hdclaude/gpu/vulkan_context.h"
#include "hdclaude/gpu/vulkan_resources.h"

namespace hdclaude {

/// One entry in a kernel's descriptor interface.
struct BindingDescription {
    std::uint32_t binding = 0;
    VkDescriptorType type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    /// Greater than one for a descriptor array; the renderer's bindless
    /// texture table is the only such binding.
    std::uint32_t count = 1;
    /// Partially bound, for the texture table where most slots are unused.
    bool partiallyBound = false;
    std::string debugName;
};

/// A compute pipeline and the descriptor machinery it needs.
///
/// Move-only, and constructed all-or-nothing: a failure at any stage releases
/// what was built rather than publishing a half-made pipeline. hdCodex
/// destroyed its pipeline before creating the replacement, so a MaterialX
/// compilation failure left a null pipeline that the render pass then bound and
/// dispatched (docs/lessons-from-hdcodex.md C4).
class ComputePipeline {
  public:
    ComputePipeline() = default;
    ComputePipeline(const VulkanContext& context,
                    const std::vector<std::uint32_t>& spirv,
                    const std::vector<BindingDescription>& bindings,
                    std::uint32_t pushConstantBytes = 0,
                    std::string debugName = {});
    ~ComputePipeline();

    ComputePipeline(ComputePipeline&&) noexcept;
    ComputePipeline& operator=(ComputePipeline&&) noexcept;
    ComputePipeline(const ComputePipeline&) = delete;
    ComputePipeline& operator=(const ComputePipeline&) = delete;

    bool Valid() const { return _pipeline != VK_NULL_HANDLE; }
    VkPipeline Handle() const { return _pipeline; }
    VkPipelineLayout Layout() const { return _layout; }
    VkDescriptorSetLayout SetLayout() const { return _setLayout; }
    ResourceGeneration Generation() const { return _generation; }

    /// Allocate a descriptor set for this pipeline. `frameSlots` sets are
    /// available; the wavefront scheduler takes one per frame slot so that
    /// alternating frames never mutate descriptor state an earlier submission
    /// may still be consuming.
    VkDescriptorSet AllocateSet();

    /// Return every set allocated from this pipeline's pool.
    ///
    /// The pool is small and fixed, so a renderer that allocates per frame
    /// exhausts it within a few frames. The caller must be certain no
    /// submission still references a set from this pool -- which for hdClaude
    /// means every submit has been waited on -- because resetting a pool
    /// invalidates its sets immediately.
    void ResetSets();

    /// Point a storage-buffer binding at a buffer.
    void WriteBuffer(VkDescriptorSet set, std::uint32_t binding,
                     const VulkanBuffer& buffer,
                     VkDescriptorType type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) const;

    /// Point a storage-image binding at an image.
    void WriteStorageImage(VkDescriptorSet set, std::uint32_t binding,
                           const VulkanImage& image,
                           VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL) const;

    /// Fill a sampled-image array binding, one write for the whole array.
    ///
    /// Every element is written, including the unused tail: a descriptor that
    /// is declared but never written is undefined behaviour the moment a
    /// shader indexes it, and an out-of-range index in generated code is
    /// exactly the sort of thing that should produce a wrong pixel rather than
    /// a lost device.
    void WriteSampledImageArray(VkDescriptorSet set, std::uint32_t binding,
                                const std::vector<VkDescriptorImageInfo>& images) const;

    /// Bind and dispatch. `groups` is the workgroup count, not the thread
    /// count -- the kernel's own `local_size` decides the rest.
    void Dispatch(VkCommandBuffer command, VkDescriptorSet set,
                  std::uint32_t groupsX, std::uint32_t groupsY = 1,
                  std::uint32_t groupsZ = 1, const void* pushConstants = nullptr,
                  std::uint32_t pushConstantBytes = 0) const;

    void Reset();

  private:
    const VulkanContext* _context = nullptr;
    VkShaderModule _module = VK_NULL_HANDLE;
    VkDescriptorSetLayout _setLayout = VK_NULL_HANDLE;
    VkPipelineLayout _layout = VK_NULL_HANDLE;
    VkPipeline _pipeline = VK_NULL_HANDLE;
    VkDescriptorPool _pool = VK_NULL_HANDLE;
    std::vector<BindingDescription> _bindings;
    std::uint32_t _pushConstantBytes = 0;
    ResourceGeneration _generation = 0;
    std::string _debugName;
};

/// Number of descriptor sets a pipeline's pool provides.
///
/// Two frame slots plus headroom. Derived from the binding table rather than
/// restated, so adding a binding cannot silently exhaust the pool.
inline constexpr std::uint32_t kDescriptorSetsPerPipeline = 8;

}  // namespace hdclaude

#endif  // HDCLAUDE_GPU_COMPUTE_PIPELINE_H
