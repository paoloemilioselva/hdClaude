// Buffers and images, with the resource rules from docs/architecture.md 6
// expressed as types rather than as conventions.
//
// Two of those rules shape this header:
//
//   Rule 1  Build, then publish, then rebind. A resource is constructed
//           completely or not at all; a failed construction leaves the previous
//           value untouched, because the new one is a separate object that is
//           moved into place only on success.
//   Rule 2  Invalidate by identity, not by shape. Every resource carries a
//           process-unique generation. A descriptor set records the generations
//           it was written against and is rewritten when any of them changes --
//           never when some *shape* that motivated the change happens to differ.
//
// hdCodex's A1 was a descriptor naming a freed buffer because the invalidation
// test compared render extents while the descriptor named a buffer. Generations
// make that class of bug unrepresentable.

#ifndef HDCLAUDE_GPU_VULKAN_RESOURCES_H
#define HDCLAUDE_GPU_VULKAN_RESOURCES_H

#include <cstdint>
#include <string>
#include <vector>

#include <volk.h>

#include "hdclaude/gpu/vulkan_context.h"

namespace hdclaude {

class VulkanAllocator;

/// A process-unique resource identity.
///
/// Monotonic and never reused, so "is this the same resource I bound?" is a
/// single integer comparison that cannot be fooled by an allocator handing back
/// an address a destroyed resource used to occupy.
using ResourceGeneration = std::uint64_t;

ResourceGeneration NextResourceGeneration();

// ---------------------------------------------------------------------------

enum class BufferDomain {
    /// GPU-only. The default for anything the shaders touch every frame.
    DeviceLocal,
    /// CPU-writable, GPU-readable. Staging uploads.
    HostUpload,
    /// GPU-writable, CPU-readable, persistently mapped. Readback.
    HostReadback,
};

struct BufferDescription {
    VkDeviceSize size = 0;
    VkBufferUsageFlags usage = 0;
    BufferDomain domain = BufferDomain::DeviceLocal;
    std::string debugName;
};

/// A device buffer and its allocation. Move-only: a buffer has exactly one
/// owner, so a double free is a compile error rather than a validation message.
class VulkanBuffer {
  public:
    VulkanBuffer() = default;
    VulkanBuffer(VulkanAllocator& allocator, const BufferDescription& description);
    ~VulkanBuffer();

    VulkanBuffer(VulkanBuffer&& other) noexcept;
    VulkanBuffer& operator=(VulkanBuffer&& other) noexcept;
    VulkanBuffer(const VulkanBuffer&) = delete;
    VulkanBuffer& operator=(const VulkanBuffer&) = delete;

    bool Valid() const { return _handle != VK_NULL_HANDLE; }
    VkBuffer Handle() const { return _handle; }
    VkDeviceSize Size() const { return _description.size; }
    const BufferDescription& Description() const { return _description; }

    /// Identity for descriptor invalidation. Changes whenever the underlying
    /// VkBuffer changes, and never otherwise.
    ResourceGeneration Generation() const { return _generation; }

    /// Device address, for buffer-device-address access from shaders. Zero if
    /// the buffer was not created with the address usage bit.
    VkDeviceAddress DeviceAddress() const { return _deviceAddress; }

    /// Persistently mapped pointer for host-visible domains; null otherwise.
    void* MappedData() const { return _mapped; }

    /// Copy into a host-visible buffer and flush if the memory is not coherent.
    void Write(const void* data, VkDeviceSize size, VkDeviceSize offset = 0);

    void Reset();

  private:
    VulkanAllocator* _allocator = nullptr;
    VkBuffer _handle = VK_NULL_HANDLE;
    void* _allocation = nullptr;   // VmaAllocation, kept opaque in the header
    void* _mapped = nullptr;
    VkDeviceAddress _deviceAddress = 0;
    BufferDescription _description;
    ResourceGeneration _generation = 0;
};

// ---------------------------------------------------------------------------

struct ImageDescription {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageUsageFlags usage = 0;
    std::uint32_t mipLevels = 1;
    std::string debugName;
};

/// A 2D image, its memory, and its view.
///
/// Deliberately does **not** track a current layout. With several command
/// buffers in flight, a CPU-side "current layout" describes what was last
/// *recorded*, not what the GPU will be in when a barrier executes -- accidental
/// correctness that stops holding the moment a second image or a second layout
/// appears (docs/architecture.md 6 rule 5, hdCodex N7). Layout is owned by the
/// frame slot that records the work; barriers take an explicit source layout.
class VulkanImage {
  public:
    VulkanImage() = default;
    VulkanImage(VulkanAllocator& allocator, const ImageDescription& description);
    ~VulkanImage();

    VulkanImage(VulkanImage&& other) noexcept;
    VulkanImage& operator=(VulkanImage&& other) noexcept;
    VulkanImage(const VulkanImage&) = delete;
    VulkanImage& operator=(const VulkanImage&) = delete;

    bool Valid() const { return _handle != VK_NULL_HANDLE; }
    VkImage Handle() const { return _handle; }
    VkImageView View() const { return _view; }
    const ImageDescription& Description() const { return _description; }
    ResourceGeneration Generation() const { return _generation; }

    /// Record a layout transition. The source layout is supplied by the caller
    /// -- the frame slot that recorded whatever put the image in that layout --
    /// rather than read from a shadow copy on this object.
    void RecordBarrier(VkCommandBuffer command, VkImageLayout oldLayout,
                       VkImageLayout newLayout, VkPipelineStageFlags2 sourceStage,
                       VkPipelineStageFlags2 destinationStage,
                       VkAccessFlags2 sourceAccess,
                       VkAccessFlags2 destinationAccess) const;

    void Reset();

  private:
    VulkanAllocator* _allocator = nullptr;
    VkImage _handle = VK_NULL_HANDLE;
    VkImageView _view = VK_NULL_HANDLE;
    void* _allocation = nullptr;
    ImageDescription _description;
    ResourceGeneration _generation = 0;
};

// ---------------------------------------------------------------------------

/// Owns the VMA allocator and reports its budget.
class VulkanAllocator {
  public:
    explicit VulkanAllocator(const VulkanContext& context);
    ~VulkanAllocator();

    VulkanAllocator(const VulkanAllocator&) = delete;
    VulkanAllocator& operator=(const VulkanAllocator&) = delete;

    const VulkanContext& Context() const { return _context; }
    void* Handle() const { return _allocator; }  // VmaAllocator

    /// Bytes currently allocated from device-local heaps. A wavefront path
    /// tracer's path state is large enough that this belongs in render stats
    /// rather than in a debugger.
    std::uint64_t DeviceLocalBytesUsed() const;
    std::uint64_t DeviceLocalBytesAvailable() const;

    /// Bytes asked for as device-local that were placed somewhere else.
    ///
    /// VMA prefers device memory and falls back to host memory rather than
    /// failing, which is right for a buffer written once and read rarely and
    /// wrong for geometry a ray traverses: the read then crosses PCIe and
    /// costs about two orders of magnitude, with nothing to say so. Counted
    /// so a renderer that has quietly become a hundred times slower can be
    /// asked why.
    std::uint64_t DeviceLocalBytesSpilled() const
    {
        return _spilledBytes.load(std::memory_order_relaxed);
    }
    void NoteDeviceLocalSpill(std::uint64_t bytes)
    {
        _spilledBytes.fetch_add(bytes, std::memory_order_relaxed);
    }

    /// Format support check against the physical device's optimal-tiling
    /// features. Called before allocation, so an unsupported usage is a clear
    /// error at creation rather than a validation message at first use.
    bool SupportsImageFormat(VkFormat format, VkImageUsageFlags usage) const;

  private:
    const VulkanContext& _context;
    void* _allocator = nullptr;  // VmaAllocator
    mutable std::atomic<std::uint64_t> _spilledBytes{0};
};

// ---------------------------------------------------------------------------

/// Records the resource identities a descriptor set was written against.
///
/// This is Rule 2 made concrete. A caller asks `NeedsUpdate` with the current
/// generations; if any differs, the set is rewritten. There is no path by which
/// a set can be considered current because its extents match.
class DescriptorBindingState {
  public:
    void Record(std::vector<ResourceGeneration> generations)
    {
        _generations = std::move(generations);
        _written = true;
    }

    bool NeedsUpdate(const std::vector<ResourceGeneration>& current) const
    {
        return !_written || _generations != current;
    }

    void Invalidate() { _written = false; }

  private:
    std::vector<ResourceGeneration> _generations;
    bool _written = false;
};

}  // namespace hdclaude

#endif  // HDCLAUDE_GPU_VULKAN_RESOURCES_H
