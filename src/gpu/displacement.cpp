#include "hdclaude/gpu/displacement.h"

#include <cstring>

namespace hdclaude {

namespace {

/// What the kernel's push constant block holds. Mirrors `DisplaceParams` in
/// shaders/displace.comp.glsl; the two are changed together.
struct DisplaceParams {
    VkDeviceAddress positions = 0;
    VkDeviceAddress normals = 0;
    VkDeviceAddress dpdu = 0;
    VkDeviceAddress dpdv = 0;
    VkDeviceAddress uvs = 0;
    VkDeviceAddress displaced = 0;
    std::uint32_t vertexCount = 0;
    std::uint32_t space = 0;
};

constexpr std::uint32_t kWorkgroupSize = 64;

/// A buffer the shader reads by address and the host fills directly.
///
/// Host-visible rather than staged through a copy. This runs once per
/// prototype at scene publication and never inside a frame, so the cost of
/// reading host memory over the bus is paid once against the two extra
/// allocations and the barrier a staging copy would need.
VulkanBuffer UploadBuffer(VulkanAllocator& allocator, const void* data,
                          VkDeviceSize bytes, const char* name)
{
    BufferDescription description;
    description.size = bytes;
    description.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    description.domain = BufferDomain::HostUpload;
    description.debugName = name;
    VulkanBuffer buffer(allocator, description);
    if (buffer.Valid() && data != nullptr) {
        buffer.Write(data, bytes);
    }
    return buffer;
}

}  // namespace

std::vector<BindingDescription> DisplacementBindings()
{
    BindingDescription textures;
    textures.binding = 16;
    textures.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    textures.count = kTextureCapacity;
    textures.partiallyBound = true;
    textures.debugName = "hdclaude_textures";
    return {textures};
}

DisplacementPass::DisplacementPass(const VulkanContext& context,
                                   const std::vector<std::uint32_t>& spirv,
                                   DisplacementSpace space,
                                   std::string debugName)
    : _context(&context),
      _pipeline(context, spirv, DisplacementBindings(), sizeof(DisplaceParams),
                debugName),
      _space(space),
      _debugName(std::move(debugName))
{
    if (_pipeline.Valid()) {
        _set = _pipeline.AllocateSet();
    }
}

void DisplacementPass::SetTextures(
    const std::vector<VkDescriptorImageInfo>& textures)
{
    if (_set == VK_NULL_HANDLE) {
        return;
    }
    _pipeline.WriteSampledImageArray(_set, 16, textures);
}

std::vector<float> DisplacementPass::Displace(VulkanAllocator& allocator,
                                              const MeshPrototype& prototype,
                                              const VertexFrames& frames) const
{
    const std::size_t vertexCount = prototype.VertexCount();
    if (!Valid() || _context == nullptr || prototype.IsCurve() ||
        vertexCount == 0 || frames.VertexCount() != vertexCount) {
        return {};
    }

    // The kernel reads a vec3 per vertex under `scalar` layout, which is
    // twelve tightly packed bytes, so the prototype's interleaved array is
    // already the buffer's contents.
    const VkDeviceSize vectorBytes =
        static_cast<VkDeviceSize>(vertexCount) * 3 * sizeof(float);
    const VkDeviceSize uvBytes =
        static_cast<VkDeviceSize>(vertexCount) * 2 * sizeof(float);

    VulkanBuffer positions = UploadBuffer(allocator, prototype.positions.data(),
                                          vectorBytes, "displace.positions");
    VulkanBuffer normals =
        UploadBuffer(allocator, frames.normals.data(), vectorBytes,
                     "displace.normals");
    VulkanBuffer dpdu =
        UploadBuffer(allocator, frames.dpdu.data(), vectorBytes, "displace.dpdu");
    VulkanBuffer dpdv =
        UploadBuffer(allocator, frames.dpdv.data(), vectorBytes, "displace.dpdv");
    VulkanBuffer uvs;
    if (frames.uvs.size() == vertexCount * 2) {
        uvs = UploadBuffer(allocator, frames.uvs.data(), uvBytes, "displace.uvs");
    }

    BufferDescription resultDescription;
    resultDescription.size = vectorBytes;
    resultDescription.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                              VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    resultDescription.domain = BufferDomain::HostReadback;
    resultDescription.debugName = "displace.result";
    VulkanBuffer result(allocator, resultDescription);

    if (!positions.Valid() || !normals.Valid() || !dpdu.Valid() ||
        !dpdv.Valid() || !result.Valid()) {
        return {};
    }

    DisplaceParams params;
    params.positions = positions.DeviceAddress();
    params.normals = normals.DeviceAddress();
    params.dpdu = dpdu.DeviceAddress();
    params.dpdv = dpdv.DeviceAddress();
    params.uvs = uvs.Valid() ? uvs.DeviceAddress() : 0;
    params.displaced = result.DeviceAddress();
    params.vertexCount = static_cast<std::uint32_t>(vertexCount);
    params.space = static_cast<std::uint32_t>(_space);

    const std::uint32_t groups =
        (params.vertexCount + kWorkgroupSize - 1) / kWorkgroupSize;

    _context->SubmitImmediate([&](VkCommandBuffer command) {
        _pipeline.Dispatch(command, _set, groups, 1, 1, &params,
                           sizeof(params));
    });

    // SubmitImmediate waits, so the result is complete and the readback is
    // host-coherent by the time this returns.
    std::vector<float> displaced(vertexCount * 3, 0.0f);
    std::memcpy(displaced.data(), result.MappedData(),
                static_cast<std::size_t>(vectorBytes));
    return displaced;
}

}  // namespace hdclaude
