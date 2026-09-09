#include "hdclaude/gpu/vulkan_resources.h"

#include <atomic>
#include <cstring>
#include <utility>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <vk_mem_alloc.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace hdclaude {
namespace {

VmaAllocator ToVma(void* handle) { return static_cast<VmaAllocator>(handle); }
VmaAllocation ToVmaAllocation(void* handle)
{
    return static_cast<VmaAllocation>(handle);
}

}  // namespace

ResourceGeneration NextResourceGeneration()
{
    // Starts at one so that zero is always "no resource", and never reused, so
    // a comparison against a recorded generation cannot be satisfied by a
    // different resource that happened to reuse an address.
    static std::atomic<ResourceGeneration> counter{1};
    return counter.fetch_add(1, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// VulkanAllocator
// ---------------------------------------------------------------------------

VulkanAllocator::VulkanAllocator(const VulkanContext& context) : _context(context)
{
    // volk loads entry points dynamically, so VMA must be handed the function
    // pointers rather than left to link against a static loader.
    VmaVulkanFunctions functions{};
    functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo info{};
    info.physicalDevice = context.PhysicalDevice();
    info.device = context.Device();
    info.instance = context.Instance();
    info.vulkanApiVersion = VK_API_VERSION_1_3;
    info.pVulkanFunctions = &functions;
    info.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;

    VmaAllocator allocator = VK_NULL_HANDLE;
    context.Check(vmaCreateAllocator(&info, &allocator), "vmaCreateAllocator");
    _allocator = allocator;
}

VulkanAllocator::~VulkanAllocator()
{
    if (_allocator != nullptr) {
        vmaDestroyAllocator(ToVma(_allocator));
    }
}

std::uint64_t VulkanAllocator::DeviceLocalBytesUsed() const
{
    std::vector<VmaBudget> budgets(VK_MAX_MEMORY_HEAPS);
    vmaGetHeapBudgets(ToVma(_allocator), budgets.data());

    const auto& memory = _context.MemoryProperties();
    std::uint64_t used = 0;
    for (std::uint32_t i = 0; i < memory.memoryHeapCount; ++i) {
        if ((memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) {
            used += budgets[i].usage;
        }
    }
    return used;
}

std::uint64_t VulkanAllocator::DeviceLocalBytesAvailable() const
{
    std::vector<VmaBudget> budgets(VK_MAX_MEMORY_HEAPS);
    vmaGetHeapBudgets(ToVma(_allocator), budgets.data());

    const auto& memory = _context.MemoryProperties();
    std::uint64_t available = 0;
    for (std::uint32_t i = 0; i < memory.memoryHeapCount; ++i) {
        if ((memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) {
            const VkDeviceSize budget = budgets[i].budget;
            const VkDeviceSize usage = budgets[i].usage;
            available += budget > usage ? (budget - usage) : 0;
        }
    }
    return available;
}

bool VulkanAllocator::SupportsImageFormat(VkFormat format,
                                          VkImageUsageFlags usage) const
{
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(_context.PhysicalDevice(), format,
                                        &properties);
    const VkFormatFeatureFlags features = properties.optimalTilingFeatures;

    if ((usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0 &&
        (features & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) == 0) {
        return false;
    }
    if ((usage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0 &&
        (features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) == 0) {
        return false;
    }
    if ((usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0 &&
        (features & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) == 0) {
        return false;
    }
    if ((usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0 &&
        (features & VK_FORMAT_FEATURE_TRANSFER_DST_BIT) == 0) {
        return false;
    }
    if ((usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0 &&
        (features & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) == 0) {
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// VulkanBuffer
// ---------------------------------------------------------------------------

VulkanBuffer::VulkanBuffer(VulkanAllocator& allocator,
                           const BufferDescription& description)
    : _allocator(&allocator), _description(description)
{
    if (description.size == 0) {
        throw VulkanError(VK_ERROR_INITIALIZATION_FAILED,
                          "Zero-sized buffer: " + description.debugName);
    }

    VkBufferUsageFlags usage = description.usage;
    // Every device-local buffer gets a device address. The wavefront kernels
    // reach queues and path state through addresses rather than through a
    // descriptor per buffer, so requesting it uniformly avoids a class of
    // "forgot the usage bit" failures at pipeline creation.
    if (description.domain == BufferDomain::DeviceLocal) {
        usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }

    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = description.size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocationInfo{};
    switch (description.domain) {
        case BufferDomain::DeviceLocal:
            allocationInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
            break;
        case BufferDomain::HostUpload:
            allocationInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
            allocationInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                   VMA_ALLOCATION_CREATE_MAPPED_BIT;
            break;
        case BufferDomain::HostReadback:
            allocationInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
            allocationInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                                   VMA_ALLOCATION_CREATE_MAPPED_BIT;
            break;
    }

    VmaAllocation allocation = VK_NULL_HANDLE;
    VmaAllocationInfo resultInfo{};
    const VkResult result =
        vmaCreateBuffer(ToVma(allocator.Handle()), &bufferInfo, &allocationInfo,
                        &_handle, &allocation, &resultInfo);
    if (result != VK_SUCCESS) {
        // Leave the object empty rather than partially constructed. Rule 1: a
        // failed construction must not be publishable.
        _handle = VK_NULL_HANDLE;
        _allocator = nullptr;
        allocator.Context().Check(result,
                                  "vmaCreateBuffer(" + description.debugName + ")");
    }

    _allocation = allocation;
    _mapped = resultInfo.pMappedData;
    _generation = NextResourceGeneration();

    // Where the allocation actually landed.
    //
    // `VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE` prefers device memory and
    // silently accepts host memory when there is no room, which is the right
    // behaviour for a buffer that is written once and read rarely and the
    // wrong behaviour for geometry a ray traverses: reading it over PCIe
    // costs about two orders of magnitude and nothing says so. A renderer
    // that silently becomes a hundred times slower is exactly the kind of
    // quiet degradation this project reports rather than absorbs, so the
    // spill is counted and can be named.
    if (description.domain == BufferDomain::DeviceLocal) {
        VkMemoryPropertyFlags properties = 0;
        vmaGetAllocationMemoryProperties(ToVma(allocator.Handle()),
                                        allocation, &properties);
        if ((properties & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) == 0) {
            allocator.NoteDeviceLocalSpill(description.size);
        }
    }

    if ((usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0) {
        VkBufferDeviceAddressInfo addressInfo{
            VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        addressInfo.buffer = _handle;
        _deviceAddress =
            vkGetBufferDeviceAddress(allocator.Context().Device(), &addressInfo);
    }

    if (!description.debugName.empty() &&
        vkSetDebugUtilsObjectNameEXT != nullptr) {
        VkDebugUtilsObjectNameInfoEXT nameInfo{
            VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
        nameInfo.objectType = VK_OBJECT_TYPE_BUFFER;
        nameInfo.objectHandle = reinterpret_cast<std::uint64_t>(_handle);
        nameInfo.pObjectName = description.debugName.c_str();
        vkSetDebugUtilsObjectNameEXT(allocator.Context().Device(), &nameInfo);
    }
}

void VulkanBuffer::Reset()
{
    if (_handle != VK_NULL_HANDLE && _allocator != nullptr) {
        vmaDestroyBuffer(ToVma(_allocator->Handle()), _handle,
                         ToVmaAllocation(_allocation));
    }
    _handle = VK_NULL_HANDLE;
    _allocation = nullptr;
    _mapped = nullptr;
    _deviceAddress = 0;
    _allocator = nullptr;
    _generation = 0;
    _description = {};
}

VulkanBuffer::~VulkanBuffer() { Reset(); }

VulkanBuffer::VulkanBuffer(VulkanBuffer&& other) noexcept
    : _allocator(std::exchange(other._allocator, nullptr)),
      _handle(std::exchange(other._handle, VK_NULL_HANDLE)),
      _allocation(std::exchange(other._allocation, nullptr)),
      _mapped(std::exchange(other._mapped, nullptr)),
      _deviceAddress(std::exchange(other._deviceAddress, 0)),
      _description(std::move(other._description)),
      _generation(std::exchange(other._generation, 0))
{
}

VulkanBuffer& VulkanBuffer::operator=(VulkanBuffer&& other) noexcept
{
    if (this != &other) {
        Reset();
        _allocator = std::exchange(other._allocator, nullptr);
        _handle = std::exchange(other._handle, VK_NULL_HANDLE);
        _allocation = std::exchange(other._allocation, nullptr);
        _mapped = std::exchange(other._mapped, nullptr);
        _deviceAddress = std::exchange(other._deviceAddress, 0);
        _description = std::move(other._description);
        _generation = std::exchange(other._generation, 0);
    }
    return *this;
}

void VulkanBuffer::Write(const void* data, VkDeviceSize size, VkDeviceSize offset)
{
    if (_mapped == nullptr) {
        throw VulkanError(VK_ERROR_MEMORY_MAP_FAILED,
                          "Write to an unmapped buffer: " + _description.debugName);
    }
    if (offset + size > _description.size) {
        throw VulkanError(VK_ERROR_UNKNOWN,
                          "Write past the end of buffer: " + _description.debugName);
    }
    std::memcpy(static_cast<std::uint8_t*>(_mapped) + offset, data,
                static_cast<std::size_t>(size));
    // A no-op on coherent memory; VMA checks the property rather than assuming.
    vmaFlushAllocation(ToVma(_allocator->Handle()), ToVmaAllocation(_allocation),
                       offset, size);
}

// ---------------------------------------------------------------------------
// VulkanImage
// ---------------------------------------------------------------------------

VulkanImage::VulkanImage(VulkanAllocator& allocator,
                         const ImageDescription& description)
    : _allocator(&allocator), _description(description)
{
    if (description.width == 0 || description.height == 0) {
        throw VulkanError(VK_ERROR_INITIALIZATION_FAILED,
                          "Zero-sized image: " + description.debugName);
    }
    // Checked before allocation, so an unsupported format/usage pair is an
    // error naming the resource rather than a validation message at first use.
    if (!allocator.SupportsImageFormat(description.format, description.usage)) {
        throw VulkanError(VK_ERROR_FORMAT_NOT_SUPPORTED,
                          "Image format does not support the requested usage: " +
                              description.debugName);
    }

    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = description.format;
    imageInfo.extent = {description.width, description.height, 1};
    imageInfo.mipLevels = description.mipLevels;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = description.usage;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    VmaAllocation allocation = VK_NULL_HANDLE;
    const VkResult result =
        vmaCreateImage(ToVma(allocator.Handle()), &imageInfo, &allocationInfo,
                       &_handle, &allocation, nullptr);
    if (result != VK_SUCCESS) {
        _handle = VK_NULL_HANDLE;
        _allocator = nullptr;
        allocator.Context().Check(result,
                                  "vmaCreateImage(" + description.debugName + ")");
    }
    _allocation = allocation;

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = _handle;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = description.format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = description.mipLevels;
    viewInfo.subresourceRange.layerCount = 1;

    const VkResult viewResult = vkCreateImageView(allocator.Context().Device(),
                                                  &viewInfo, nullptr, &_view);
    if (viewResult != VK_SUCCESS) {
        // The image exists but the object as a whole does not. Release what was
        // built rather than publishing a half-constructed image (Rule 1).
        vmaDestroyImage(ToVma(allocator.Handle()), _handle, allocation);
        _handle = VK_NULL_HANDLE;
        _allocation = nullptr;
        _allocator = nullptr;
        allocator.Context().Check(viewResult,
                                  "vkCreateImageView(" + description.debugName + ")");
    }

    _generation = NextResourceGeneration();
}

void VulkanImage::Reset()
{
    if (_allocator != nullptr) {
        if (_view != VK_NULL_HANDLE) {
            vkDestroyImageView(_allocator->Context().Device(), _view, nullptr);
        }
        if (_handle != VK_NULL_HANDLE) {
            vmaDestroyImage(ToVma(_allocator->Handle()), _handle,
                            ToVmaAllocation(_allocation));
        }
    }
    _handle = VK_NULL_HANDLE;
    _view = VK_NULL_HANDLE;
    _allocation = nullptr;
    _allocator = nullptr;
    _generation = 0;
    _description = {};
}

VulkanImage::~VulkanImage() { Reset(); }

VulkanImage::VulkanImage(VulkanImage&& other) noexcept
    : _allocator(std::exchange(other._allocator, nullptr)),
      _handle(std::exchange(other._handle, VK_NULL_HANDLE)),
      _view(std::exchange(other._view, VK_NULL_HANDLE)),
      _allocation(std::exchange(other._allocation, nullptr)),
      _description(std::move(other._description)),
      _generation(std::exchange(other._generation, 0))
{
}

VulkanImage& VulkanImage::operator=(VulkanImage&& other) noexcept
{
    if (this != &other) {
        Reset();
        _allocator = std::exchange(other._allocator, nullptr);
        _handle = std::exchange(other._handle, VK_NULL_HANDLE);
        _view = std::exchange(other._view, VK_NULL_HANDLE);
        _allocation = std::exchange(other._allocation, nullptr);
        _description = std::move(other._description);
        _generation = std::exchange(other._generation, 0);
    }
    return *this;
}

void VulkanImage::RecordBarrier(VkCommandBuffer command, VkImageLayout oldLayout,
                                VkImageLayout newLayout,
                                VkPipelineStageFlags2 sourceStage,
                                VkPipelineStageFlags2 destinationStage,
                                VkAccessFlags2 sourceAccess,
                                VkAccessFlags2 destinationAccess) const
{
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = sourceStage;
    barrier.srcAccessMask = sourceAccess;
    barrier.dstStageMask = destinationStage;
    barrier.dstAccessMask = destinationAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = _handle;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = _description.mipLevels;
    barrier.subresourceRange.layerCount = 1;

    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;

    vkCmdPipelineBarrier2(command, &dependency);
}

}  // namespace hdclaude
