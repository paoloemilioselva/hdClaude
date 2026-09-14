#include "hdclaude/gpu/acceleration_structure.h"

#include <iterator>
#include <unordered_set>

#include <cstring>
#include <utility>

#include "hdclaude/core/hash.h"

namespace hdclaude {
namespace {

/// Upload a host vector into a device-local buffer through a staging copy.
template <typename T>
VulkanBuffer UploadDeviceLocal(const VulkanContext& context,
                               VulkanAllocator& allocator,
                               const std::vector<T>& data,
                               VkBufferUsageFlags usage, const char* name)
{
    if (data.empty()) {
        return VulkanBuffer{};
    }

    const VkDeviceSize size = data.size() * sizeof(T);

    BufferDescription stagingDescription;
    stagingDescription.size = size;
    stagingDescription.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    stagingDescription.domain = BufferDomain::HostUpload;
    stagingDescription.debugName = std::string(name) + ".staging";
    VulkanBuffer staging(allocator, stagingDescription);
    staging.Write(data.data(), size);

    BufferDescription deviceDescription;
    deviceDescription.size = size;
    deviceDescription.usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    deviceDescription.domain = BufferDomain::DeviceLocal;
    deviceDescription.debugName = name;
    VulkanBuffer device(allocator, deviceDescription);

    context.SubmitImmediate([&](VkCommandBuffer command) {
        VkBufferCopy region{};
        region.size = size;
        vkCmdCopyBuffer(command, staging.Handle(), device.Handle(), 1, &region);
    });

    return device;
}

/// Usage bits a buffer needs to be read by an acceleration-structure build.
constexpr VkBufferUsageFlags kBuildInputUsage =
    VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

VkDeviceAddress StructureAddress(const VulkanContext& context,
                                 VkAccelerationStructureKHR structure)
{
    VkAccelerationStructureDeviceAddressInfoKHR info{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    info.accelerationStructure = structure;
    return vkGetAccelerationStructureDeviceAddressKHR(context.Device(), &info);
}

}  // namespace

// ---------------------------------------------------------------------------
// MeshPrototype / Scene
// ---------------------------------------------------------------------------

/// A scratch buffer whose device address meets the build's alignment.
///
/// `minAccelerationStructureScratchOffsetAlignment` is a build requirement, not
/// a buffer requirement, so an allocator satisfying the buffer's own alignment
/// can still hand back an address the build rejects. Over-allocating by one
/// alignment and rounding the address up is what makes the requirement hold for
/// any allocator.
///
/// Getting this wrong is not a wrong picture. The driver reads and writes
/// scratch memory through the misaligned address and the device is lost, which
/// is how it presented: an intermittent VK_ERROR_DEVICE_LOST on whichever scene
/// happened to be allocated badly, with nothing in the scene to blame.
struct AlignedScratch {
    VulkanBuffer buffer;
    VkDeviceAddress address = 0;
};

AlignedScratch MakeScratch(const VulkanContext& context, VulkanAllocator& allocator,
                           VkDeviceSize size, const std::string& name)
{
    const VkDeviceSize alignment =
        std::max<VkDeviceSize>(1, context.Capabilities().scratchAlignment);

    BufferDescription description;
    description.size = size + alignment;
    description.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    description.debugName = name;

    AlignedScratch scratch;
    scratch.buffer = VulkanBuffer(allocator, description);
    scratch.address =
        (scratch.buffer.DeviceAddress() + alignment - 1) & ~(alignment - 1);
    return scratch;
}

std::uint64_t MeshPrototype::Fingerprint() const
{
    // Hashed over the actual bytes rather than over counts or a name: two
    // prototypes with the same triangle count are not interchangeable, and a
    // renamed prototype with unchanged geometry is.
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    if (!positions.empty()) {
        hash = Fnv1a64(positions.data(), positions.size() * sizeof(float), hash);
    }
    if (!indices.empty()) {
        hash = Fnv1a64(indices.data(), indices.size() * sizeof(std::uint32_t), hash);
    }
    // Normals, texture coordinates and per-triangle materials are part of the
    // identity too, even though the acceleration structure is built from the
    // positions alone. The structure *owns* those buffers and hands their
    // addresses to the shading kernel, so a prototype that reused one built
    // from different shading data would be shaded with that data: two quads
    // with the same corners and different UVs would share one set of texture
    // coordinates, and a mesh whose UVs were added would keep having none.
    // That is not a subtle difference in the image and it took a texture with
    // four distinguishable quadrants to see it.
    if (!normals.empty()) {
        hash = Fnv1a64(normals.data(), normals.size() * sizeof(float), hash);
        const auto perCorner = static_cast<std::uint8_t>(normalsPerCorner);
        hash = Fnv1a64(&perCorner, sizeof(perCorner), hash);
    }
    if (!uvs.empty()) {
        hash = Fnv1a64(uvs.data(), uvs.size() * sizeof(float), hash);
        const auto perCorner = static_cast<std::uint8_t>(uvsPerCorner);
        hash = Fnv1a64(&perCorner, sizeof(perCorner), hash);
    }
    if (!segments.empty()) {
        hash = Fnv1a64(segments.data(), segments.size() * sizeof(float), hash);
    }
    if (!segmentMaterials.empty()) {
        hash = Fnv1a64(segmentMaterials.data(),
                       segmentMaterials.size() * sizeof(std::uint32_t), hash);
    }
    if (!triangleMaterials.empty()) {
        hash = Fnv1a64(triangleMaterials.data(),
                       triangleMaterials.size() * sizeof(std::uint32_t), hash);
    }
    // The opacity class changes the build flags, so a structure built for an
    // opaque mesh cannot be reused for a cut-out one.
    const auto opacityByte = static_cast<std::uint8_t>(opacity);
    hash = Fnv1a64(&opacityByte, sizeof(opacityByte), hash);
    return hash;
}

/// Overwrite a device-local buffer that already exists and is the right size.
///
/// The point of it is what it does *not* do: allocate. A refit re-uploads the
/// positions of a deforming mesh every frame, and allocating a second copy to
/// do it is how a groom that fits in device memory stops fitting.
template <typename T>
void OverwriteDeviceLocal(const VulkanContext& context,
                          VulkanAllocator& allocator, VulkanBuffer& destination,
                          const std::vector<T>& data, const char* name)
{
    if (data.empty() || !destination.Valid()) {
        return;
    }
    const VkDeviceSize size = data.size() * sizeof(T);

    BufferDescription stagingDescription;
    stagingDescription.size = size;
    stagingDescription.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    stagingDescription.domain = BufferDomain::HostUpload;
    stagingDescription.debugName = std::string(name) + ".staging";
    VulkanBuffer staging(allocator, stagingDescription);
    staging.Write(data.data(), size);

    context.SubmitImmediate([&](VkCommandBuffer command) {
        VkBufferCopy region{};
        region.size = size;
        vkCmdCopyBuffer(command, staging.Handle(), destination.Handle(), 1,
                        &region);
    });
}

/// One axis-aligned box per curve segment, big enough for the round cone.
///
/// The cone's surface never leaves the union of the two end spheres and the
/// truncated cone between them, so the box of the two centres grown by the
/// larger radius contains it. Tighter boxes are possible -- the exact bound of
/// a round cone is a little smaller off-axis -- and are not worth the
/// arithmetic here: a hair segment's radius is a fraction of its length, so the
/// slack is a fraction of an already small box.
std::vector<VkAabbPositionsKHR> CurveBounds(const MeshPrototype& prototype)
{
    std::vector<VkAabbPositionsKHR> boxes;
    boxes.reserve(prototype.SegmentCount());
    for (std::size_t i = 0; i < prototype.SegmentCount(); ++i) {
        const float* segment = prototype.segments.data() + i * 10;
        const float radius = std::max(std::max(segment[3], segment[8]), 0.0f);
        VkAabbPositionsKHR box{};
        box.minX = std::min(segment[0], segment[5]) - radius;
        box.minY = std::min(segment[1], segment[6]) - radius;
        box.minZ = std::min(segment[2], segment[7]) - radius;
        box.maxX = std::max(segment[0], segment[5]) + radius;
        box.maxY = std::max(segment[1], segment[6]) + radius;
        box.maxZ = std::max(segment[2], segment[7]) + radius;
        boxes.push_back(box);
    }
    return boxes;
}

std::uint64_t MeshPrototype::TopologyFingerprint() const
{
    std::uint64_t hash = 0x9e3779b97f4a7c15ULL;
    if (!indices.empty()) {
        hash = Fnv1a64(indices.data(), indices.size() * sizeof(std::uint32_t),
                       hash);
    }
    // The vertex count, because `maxVertex` is part of what the structure was
    // built with; and the sizes of the shading arrays, because a refit reuses
    // their buffers and cannot resize them.
    const std::uint64_t counts[6] = {
        static_cast<std::uint64_t>(positions.size()),
        static_cast<std::uint64_t>(normals.size()),
        static_cast<std::uint64_t>(uvs.size()),
        static_cast<std::uint64_t>(triangleMaterials.size()),
        static_cast<std::uint64_t>(segments.size()),
        static_cast<std::uint64_t>(segmentMaterials.size()),
    };
    hash = Fnv1a64(counts, sizeof(counts), hash);

    const std::uint8_t flags[3] = {
        static_cast<std::uint8_t>(normalsPerCorner),
        static_cast<std::uint8_t>(uvsPerCorner),
        static_cast<std::uint8_t>(opacity),
    };
    hash = Fnv1a64(flags, sizeof(flags), hash);
    return hash;
}

std::size_t Scene::TotalTriangles() const
{
    std::size_t total = 0;
    for (const MeshInstance& instance : instances) {
        if (instance.prototype < prototypes.size()) {
            const MeshPrototype& prototype = prototypes[instance.prototype];
            // A curve prototype has no triangles at all now that its segments
            // are intersected directly, so it contributes none. The figure is
            // what a scene costs as triangles, and counting segments in it
            // would make the two representations look comparable when the whole
            // point is that they are not.
            total += prototype.IsCurve() ? 0 : prototype.TriangleCount();
        }
    }
    return total;
}

/// Remove the one entry of `topology` that names `fingerprint`.
///
/// A multimap because several structures can share a topology; by value because
/// the one being taken is a particular structure, not any structure that
/// happens to have the same shape.
void EraseTopology(std::unordered_multimap<std::uint64_t, std::uint64_t>& index,
                   std::uint64_t topology, std::uint64_t fingerprint)
{
    auto range = index.equal_range(topology);
    for (auto it = range.first; it != range.second; ++it) {
        if (it->second == fingerprint) {
            index.erase(it);
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// BottomLevelStructure
// ---------------------------------------------------------------------------

BottomLevelStructure::BottomLevelStructure(const VulkanContext& context,
                                           VulkanAllocator& allocator,
                                           const MeshPrototype& prototype,
                                           bool allowUpdate)
    : _context(&context)
{
    context.RequireLive("BottomLevelStructure");

    const bool curve = prototype.IsCurve();
    if (!curve && (prototype.indices.empty() || prototype.positions.empty())) {
        throw VulkanError(VK_ERROR_INITIALIZATION_FAILED,
                          "Empty prototype: " + prototype.debugName);
    }

    _fingerprint = prototype.Fingerprint();
    _topology = prototype.TopologyFingerprint();
    _updatable = allowUpdate;
    _opacity = prototype.opacity;
    _curve = curve;
    // The count the structure is built over, whichever kind it is. A curve
    // prototype has no triangles; the name is kept because everything that
    // reads it wants "how many primitives does this structure hold".
    _triangleCount = static_cast<std::uint32_t>(prototype.PrimitiveCount());
    _vertexCount = static_cast<std::uint32_t>(prototype.VertexCount());

    const std::string name =
        prototype.debugName.empty() ? std::string("prototype") : prototype.debugName;

    if (curve) {
        // The segments themselves, read by the traversal kernel to intersect
        // the cone, and the boxes the structure is partitioned over. Two
        // buffers rather than one because the acceleration structure requires a
        // particular layout for the boxes and the kernel wants the geometry.
        _segments = UploadDeviceLocal(context, allocator, prototype.segments,
                                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                      (name + ".segments").c_str());
        _aabbs = UploadDeviceLocal(context, allocator,
                                   CurveBounds(prototype), kBuildInputUsage,
                                   (name + ".aabbs").c_str());
        if (!prototype.segmentMaterials.empty()) {
            _segmentMaterials = UploadDeviceLocal(
                context, allocator, prototype.segmentMaterials,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                (name + ".segmentMaterials").c_str());
        }
    } else {
        _positions = UploadDeviceLocal(context, allocator, prototype.positions,
                                       kBuildInputUsage,
                                       (name + ".positions").c_str());
        _indices = UploadDeviceLocal(context, allocator, prototype.indices,
                                     kBuildInputUsage,
                                     (name + ".indices").c_str());
    }
    if (!curve && !prototype.normals.empty()) {
        _normals = UploadDeviceLocal(context, allocator, prototype.normals,
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                     (name + ".normals").c_str());
    }
    if (!curve && !prototype.uvs.empty()) {
        _uvs = UploadDeviceLocal(context, allocator, prototype.uvs,
                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                 (name + ".uvs").c_str());
    }

    // --- Describe the geometry ----------------------------------------------
    VkAccelerationStructureGeometryKHR geometry{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    // Opaque geometry lets the driver skip any-hit evaluation entirely, which
    // is the single largest traversal win available and costs nothing when the
    // mesh genuinely has no cutouts. It does not make a procedural box opaque:
    // an AABB is always a *candidate*, and the kernel that intersects the cone
    // inside it is what decides whether there is a hit at all.
    geometry.flags = prototype.opacity == OpacityClass::Opaque
                         ? VK_GEOMETRY_OPAQUE_BIT_KHR
                         : VkGeometryFlagsKHR{0};

    if (curve) {
        geometry.geometryType = VK_GEOMETRY_TYPE_AABBS_KHR;
        VkAccelerationStructureGeometryAabbsDataKHR& boxes =
            geometry.geometry.aabbs;
        boxes.sType =
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR;
        boxes.data.deviceAddress = _aabbs.DeviceAddress();
        boxes.stride = sizeof(VkAabbPositionsKHR);
    } else {
        geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        VkAccelerationStructureGeometryTrianglesDataKHR& triangles =
            geometry.geometry.triangles;
        triangles.sType =
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        triangles.vertexData.deviceAddress = _positions.DeviceAddress();
        triangles.vertexStride = 3 * sizeof(float);
        triangles.maxVertex =
            static_cast<std::uint32_t>(prototype.VertexCount() - 1);
        triangles.indexType = VK_INDEX_TYPE_UINT32;
        triangles.indexData.deviceAddress = _indices.DeviceAddress();
        triangles.transformData.deviceAddress = 0;
    }

    VkAccelerationStructureBuildGeometryInfoKHR buildInfo{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    // Traversal speed over build speed: a prototype is built once and traversed
    // for the life of the scene, and reuse means even a deforming mesh rebuilds
    // rarely.
    buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    if (allowUpdate) {
        buildInfo.flags |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    }
    buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    buildInfo.geometryCount = 1;
    buildInfo.pGeometries = &geometry;

    VkAccelerationStructureBuildSizesInfoKHR sizes{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    vkGetAccelerationStructureBuildSizesKHR(
        context.Device(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
        &buildInfo, &_triangleCount, &sizes);

    // --- Allocate and build --------------------------------------------------
    BufferDescription storageDescription;
    storageDescription.size = sizes.accelerationStructureSize;
    storageDescription.usage =
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    storageDescription.debugName = name + ".blas";
    _storage = VulkanBuffer(allocator, storageDescription);

    VkAccelerationStructureCreateInfoKHR createInfo{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    createInfo.buffer = _storage.Handle();
    createInfo.size = sizes.accelerationStructureSize;
    createInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    context.Check(vkCreateAccelerationStructureKHR(context.Device(), &createInfo,
                                                   nullptr, &_structure),
                  "vkCreateAccelerationStructureKHR(" + name + ")");

    const AlignedScratch scratch =
        MakeScratch(context, allocator, sizes.buildScratchSize, name + ".scratch");

    buildInfo.dstAccelerationStructure = _structure;
    buildInfo.scratchData.deviceAddress = scratch.address;

    VkAccelerationStructureBuildRangeInfoKHR range{};
    range.primitiveCount = _triangleCount;
    const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;

    context.SubmitImmediate([&](VkCommandBuffer command) {
        vkCmdBuildAccelerationStructuresKHR(command, 1, &buildInfo, &ranges);
    });

    _address = StructureAddress(context, _structure);
}

void BottomLevelStructure::Reset()
{
    if (_context != nullptr && _structure != VK_NULL_HANDLE) {
        vkDestroyAccelerationStructureKHR(_context->Device(), _structure, nullptr);
    }
    _structure = VK_NULL_HANDLE;
    _address = 0;
    _storage.Reset();
    _positions.Reset();
    _indices.Reset();
    _normals.Reset();
    _uvs.Reset();
    _segments.Reset();
    _segmentMaterials.Reset();
    _aabbs.Reset();
    _curve = false;
    _context = nullptr;
    _triangleCount = 0;
    _vertexCount = 0;
    _fingerprint = 0;
    _topology = 0;
    _updatable = false;
}

bool BottomLevelStructure::Refit(VulkanAllocator& allocator,
                                 const MeshPrototype& prototype)
{
    if (!_updatable || _structure == VK_NULL_HANDLE || _context == nullptr) {
        return false;
    }
    if (prototype.TopologyFingerprint() != _topology) {
        return false;
    }
    // A curve prototype is never refitted. An update rewrites the vertex data
    // the structure was built over, and for curves that is the segment buffer
    // *and* the boxes the tree partitions -- neither of which this rewrites,
    // so a deforming groom would keep the shape it had on the frame its
    // structure was built and only its shading would follow. Rebuilding is
    // correct and says so; making the update handle boxes is the better answer
    // and is not what this is.
    if (_curve) {
        return false;
    }

    const std::string name =
        prototype.debugName.empty() ? std::string("prototype") : prototype.debugName;

    // The buffers keep their allocations and take new contents. The index
    // buffer is not touched: an update may not change it, and the topology
    // fingerprint above is what guarantees it has not.
    OverwriteDeviceLocal(*_context, allocator, _positions, prototype.positions,
                         (name + ".positions").c_str());
    OverwriteDeviceLocal(*_context, allocator, _normals, prototype.normals,
                         (name + ".normals").c_str());
    OverwriteDeviceLocal(*_context, allocator, _uvs, prototype.uvs,
                         (name + ".uvs").c_str());

    VkAccelerationStructureGeometryKHR geometry{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geometry.flags = _opacity == OpacityClass::Opaque ? VK_GEOMETRY_OPAQUE_BIT_KHR
                                                      : VkGeometryFlagsKHR{0};

    VkAccelerationStructureGeometryTrianglesDataKHR& triangles =
        geometry.geometry.triangles;
    triangles.sType =
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    triangles.vertexData.deviceAddress = _positions.DeviceAddress();
    triangles.vertexStride = 3 * sizeof(float);
    triangles.maxVertex = _vertexCount > 0 ? _vertexCount - 1 : 0;
    triangles.indexType = VK_INDEX_TYPE_UINT32;
    triangles.indexData.deviceAddress = _indices.DeviceAddress();
    triangles.transformData.deviceAddress = 0;

    VkAccelerationStructureBuildGeometryInfoKHR buildInfo{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    // The same flags the structure was built with, which an update requires.
    buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                      VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
    buildInfo.geometryCount = 1;
    buildInfo.pGeometries = &geometry;

    VkAccelerationStructureBuildSizesInfoKHR sizes{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    vkGetAccelerationStructureBuildSizesKHR(
        _context->Device(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
        &buildInfo, &_triangleCount, &sizes);

    const AlignedScratch scratch = MakeScratch(
        *_context, allocator, sizes.updateScratchSize, name + ".refit");

    // Updated in place: source and destination are the same structure, which is
    // what the update mode is for and what keeps a second copy from existing.
    buildInfo.srcAccelerationStructure = _structure;
    buildInfo.dstAccelerationStructure = _structure;
    buildInfo.scratchData.deviceAddress = scratch.address;

    VkAccelerationStructureBuildRangeInfoKHR range{};
    range.primitiveCount = _triangleCount;
    const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;

    _context->SubmitImmediate([&](VkCommandBuffer command) {
        vkCmdBuildAccelerationStructuresKHR(command, 1, &buildInfo, &ranges);
    });

    _fingerprint = prototype.Fingerprint();
    return true;
}

BottomLevelStructure::~BottomLevelStructure() { Reset(); }

BottomLevelStructure::BottomLevelStructure(BottomLevelStructure&& other) noexcept
    : _context(std::exchange(other._context, nullptr)),
      _structure(std::exchange(other._structure, VK_NULL_HANDLE)),
      _address(std::exchange(other._address, 0)),
      _storage(std::move(other._storage)),
      _positions(std::move(other._positions)),
      _indices(std::move(other._indices)),
      _normals(std::move(other._normals)),
      _uvs(std::move(other._uvs)),
      _segments(std::move(other._segments)),
      _segmentMaterials(std::move(other._segmentMaterials)),
      _aabbs(std::move(other._aabbs)),
      _curve(std::exchange(other._curve, false)),
      _triangleCount(std::exchange(other._triangleCount, 0)),
      _fingerprint(std::exchange(other._fingerprint, 0)),
      _topology(std::exchange(other._topology, 0)),
      _updatable(std::exchange(other._updatable, false)),
      _opacity(other._opacity),
      _vertexCount(std::exchange(other._vertexCount, 0))
{
}

BottomLevelStructure& BottomLevelStructure::operator=(
    BottomLevelStructure&& other) noexcept
{
    if (this != &other) {
        Reset();
        _context = std::exchange(other._context, nullptr);
        _structure = std::exchange(other._structure, VK_NULL_HANDLE);
        _address = std::exchange(other._address, 0);
        _storage = std::move(other._storage);
        _positions = std::move(other._positions);
        _indices = std::move(other._indices);
        _normals = std::move(other._normals);
        _uvs = std::move(other._uvs);
        _segments = std::move(other._segments);
        _segmentMaterials = std::move(other._segmentMaterials);
        _aabbs = std::move(other._aabbs);
        _curve = std::exchange(other._curve, false);
        _triangleCount = std::exchange(other._triangleCount, 0);
        _fingerprint = std::exchange(other._fingerprint, 0);
        _topology = std::exchange(other._topology, 0);
        _updatable = std::exchange(other._updatable, false);
        _opacity = other._opacity;
        _vertexCount = std::exchange(other._vertexCount, 0);
    }
    return *this;
}

// ---------------------------------------------------------------------------
// TopLevelStructure
// ---------------------------------------------------------------------------

void TopLevelStructure::Build(
    const VulkanContext& context, VulkanAllocator& allocator,
    const std::vector<VkAccelerationStructureInstanceKHR>& instances)
{
    context.RequireLive("TopLevelStructure::Build");

    // A scene with no geometry is still a scene, and is built rather than
    // skipped. Since the analytic lights became emitters a ray can hit, a stage
    // that authors only lights has something to render; and either way the
    // descriptor set needs a real handle, because a null acceleration structure
    // is not a legal descriptor unless `nullDescriptor` is enabled, which it is
    // not. Vulkan permits a top-level build of zero instances, so the buffer
    // carries one zeroed entry purely to have an address and the build is told
    // there are none.
    const std::vector<VkAccelerationStructureInstanceKHR> uploaded =
        instances.empty() ? std::vector<VkAccelerationStructureInstanceKHR>(1)
                          : instances;

    // Built into locals; the existing structure stays traversable until the
    // replacement is complete.
    VkAccelerationStructureKHR structure = VK_NULL_HANDLE;
    VulkanBuffer storage;
    VulkanBuffer instanceBuffer;

    instanceBuffer = UploadDeviceLocal(context, allocator, uploaded,
                                       kBuildInputUsage, "tlas.instances");

    VkAccelerationStructureGeometryKHR geometry{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geometry.geometry.instances.sType =
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    geometry.geometry.instances.arrayOfPointers = VK_FALSE;
    geometry.geometry.instances.data.deviceAddress = instanceBuffer.DeviceAddress();

    const auto instanceCount = static_cast<std::uint32_t>(instances.size());

    VkAccelerationStructureBuildGeometryInfoKHR buildInfo{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    buildInfo.geometryCount = 1;
    buildInfo.pGeometries = &geometry;

    VkAccelerationStructureBuildSizesInfoKHR sizes{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    vkGetAccelerationStructureBuildSizesKHR(
        context.Device(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
        &buildInfo, &instanceCount, &sizes);

    BufferDescription storageDescription;
    storageDescription.size = sizes.accelerationStructureSize;
    storageDescription.usage =
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    storageDescription.debugName = "tlas";
    storage = VulkanBuffer(allocator, storageDescription);

    VkAccelerationStructureCreateInfoKHR createInfo{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    createInfo.buffer = storage.Handle();
    createInfo.size = sizes.accelerationStructureSize;
    createInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    context.Check(vkCreateAccelerationStructureKHR(context.Device(), &createInfo,
                                                   nullptr, &structure),
                  "vkCreateAccelerationStructureKHR(tlas)");

    const AlignedScratch scratch =
        MakeScratch(context, allocator, sizes.buildScratchSize, "tlas.scratch");

    buildInfo.dstAccelerationStructure = structure;
    buildInfo.scratchData.deviceAddress = scratch.address;

    VkAccelerationStructureBuildRangeInfoKHR range{};
    range.primitiveCount = instanceCount;
    const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;

    context.SubmitImmediate([&](VkCommandBuffer command) {
        vkCmdBuildAccelerationStructuresKHR(command, 1, &buildInfo, &ranges);
    });

    // Publish.
    Reset();
    _context = &context;
    _structure = structure;
    _storage = std::move(storage);
    _instanceBuffer = std::move(instanceBuffer);
    _instanceCount = instanceCount;
    _address = StructureAddress(context, _structure);
}

void TopLevelStructure::Reset()
{
    if (_context != nullptr && _structure != VK_NULL_HANDLE) {
        vkDestroyAccelerationStructureKHR(_context->Device(), _structure, nullptr);
    }
    _structure = VK_NULL_HANDLE;
    _address = 0;
    _storage.Reset();
    _instanceBuffer.Reset();
    _instanceCount = 0;
    _context = nullptr;
}

TopLevelStructure::~TopLevelStructure() { Reset(); }

TopLevelStructure::TopLevelStructure(TopLevelStructure&& other) noexcept
    : _context(std::exchange(other._context, nullptr)),
      _structure(std::exchange(other._structure, VK_NULL_HANDLE)),
      _address(std::exchange(other._address, 0)),
      _storage(std::move(other._storage)),
      _instanceBuffer(std::move(other._instanceBuffer)),
      _instanceCount(std::exchange(other._instanceCount, 0))
{
}

TopLevelStructure& TopLevelStructure::operator=(TopLevelStructure&& other) noexcept
{
    if (this != &other) {
        Reset();
        _context = std::exchange(other._context, nullptr);
        _structure = std::exchange(other._structure, VK_NULL_HANDLE);
        _address = std::exchange(other._address, 0);
        _storage = std::move(other._storage);
        _instanceBuffer = std::move(other._instanceBuffer);
        _instanceCount = std::exchange(other._instanceCount, 0);
    }
    return *this;
}

// ---------------------------------------------------------------------------
// SceneAccelerator
// ---------------------------------------------------------------------------

SceneAccelerator::SceneAccelerator(const VulkanContext& context,
                                   VulkanAllocator& allocator)
    : _context(context), _allocator(allocator)
{
}

void SceneAccelerator::Update(const Scene& scene)
{
    _context.RequireLive("SceneAccelerator::Update");

    _lastBuilt = 0;
    _lastReused = 0;
    _lastRefit = 0;

    // Build any prototype we do not already hold, keyed by geometry rather than
    // by index: a publication that reorders prototypes reuses everything.
    _prototypeFingerprints.assign(scene.prototypes.size(), 0);

    // Everything this scene could possibly want, before anything is built.
    //
    // The structures that survive are the ones whose geometry is asked for
    // again, plus the ones whose *topology* is, because those are the
    // candidates a deforming prototype refits onto. Everything else is released
    // here rather than at the end.
    //
    // The order is the whole point. Building into a second map and freeing the
    // old one afterwards means both exist at once, and for a scene whose
    // geometry has wholly changed that is two complete copies of it on the
    // device: switching ALab's groom from swept tubes to implicit segments
    // asked for 14.2 GiB of the old and 4.6 GiB of the new together, which is
    // more than the card has, and the frame died rather than the switch merely
    // being expensive.
    {
        std::unordered_set<std::uint64_t> wantedGeometry;
        std::unordered_set<std::uint64_t> wantedTopology;
        for (const MeshPrototype& prototype : scene.prototypes) {
            if (prototype.indices.empty() && prototype.segments.empty()) {
                continue;
            }
            wantedGeometry.insert(prototype.Fingerprint());
            wantedTopology.insert(prototype.TopologyFingerprint());
        }
        for (auto it = _byFingerprint.begin(); it != _byFingerprint.end();) {
            const bool keep = wantedGeometry.count(it->first) != 0 ||
                              wantedTopology.count(it->second.Topology()) != 0;
            it = keep ? std::next(it) : _byFingerprint.erase(it);
        }
    }

    std::unordered_map<std::uint64_t, BottomLevelStructure> retained;

    // What is held, indexed by the part of a prototype an update may keep. A
    // deforming mesh arrives with a fingerprint nothing matches and a topology
    // that matches its own previous frame, and this is where the two meet.
    // Several structures can share a topology -- two characters wearing the
    // same coat -- so each is handed out once and removed as it goes.
    std::unordered_multimap<std::uint64_t, std::uint64_t> byTopology;
    for (const auto& entry : _byFingerprint) {
        byTopology.emplace(entry.second.Topology(), entry.first);
    }

    for (std::size_t i = 0; i < scene.prototypes.size(); ++i) {
        const MeshPrototype& prototype = scene.prototypes[i];
        // A curve prototype has no indices at all -- its segments are the
        // primitives -- so "has nothing to build" is a question about both.
        if (prototype.indices.empty() && prototype.segments.empty()) {
            continue;
        }
        const std::uint64_t fingerprint = prototype.Fingerprint();
        _prototypeFingerprints[i] = fingerprint;

        if (retained.count(fingerprint) != 0) {
            // Two prototypes with identical geometry share one structure.
            ++_lastReused;
            continue;
        }

        auto existing = _byFingerprint.find(fingerprint);
        if (existing != _byFingerprint.end()) {
            const std::uint64_t topology = existing->second.Topology();
            retained.emplace(fingerprint, std::move(existing->second));
            _byFingerprint.erase(existing);
            EraseTopology(byTopology, topology, fingerprint);
            ++_lastReused;
            continue;
        }

        // Nothing has this geometry. Something may still have its *topology*,
        // which is the same mesh at a different moment of an animation.
        const std::uint64_t topology = prototype.TopologyFingerprint();
        auto candidate = byTopology.find(topology);
        if (candidate != byTopology.end()) {
            const std::uint64_t previous = candidate->second;
            byTopology.erase(candidate);
            auto held = _byFingerprint.find(previous);
            if (held != _byFingerprint.end()) {
                BottomLevelStructure structure = std::move(held->second);
                _byFingerprint.erase(held);
                if (structure.Refit(_allocator, prototype)) {
                    retained.emplace(fingerprint, std::move(structure));
                    ++_lastRefit;
                    continue;
                }
                // Built before there was any evidence this geometry moves, so
                // it cannot be updated. Rebuilt now *asking* to be, which makes
                // the frame after this one a refit. The old structure is
                // released first so its memory is available to the new one --
                // holding both is what a deforming groom cannot afford.
                structure.Reset();
                retained.emplace(fingerprint,
                                 BottomLevelStructure(_context, _allocator,
                                                      prototype, true));
                ++_lastBuilt;
                continue;
            }
        }

        retained.emplace(fingerprint,
                         BottomLevelStructure(_context, _allocator, prototype));
        ++_lastBuilt;
    }

    // Anything left in the old map is no longer referenced and is released here.
    _byFingerprint = std::move(retained);

    // --- Instances ----------------------------------------------------------
    std::vector<VkAccelerationStructureInstanceKHR> instances;
    instances.reserve(scene.instances.size());

    for (const MeshInstance& instance : scene.instances) {
        if (instance.prototype >= _prototypeFingerprints.size()) {
            continue;
        }
        const std::uint64_t fingerprint = _prototypeFingerprints[instance.prototype];
        auto found = _byFingerprint.find(fingerprint);
        if (fingerprint == 0 || found == _byFingerprint.end()) {
            continue;
        }

        VkAccelerationStructureInstanceKHR entry{};
        std::memcpy(&entry.transform, instance.transform.m, sizeof(instance.transform.m));
        // The custom index is how a shading kernel recovers which instance it
        // hit, and through it the material and the geometry buffers.
        entry.instanceCustomIndex =
            static_cast<std::uint32_t>(&instance - scene.instances.data()) & 0xFFFFFF;
        entry.mask = instance.visible ? 0xFF : 0x00;
        entry.instanceShaderBindingTableRecordOffset = 0;
        entry.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        entry.accelerationStructureReference = found->second.DeviceAddress();
        instances.push_back(entry);
    }

    // DIAGNOSTIC: what the instances span, so two publications of the same
    // stage can be compared. A tree over instances scattered far wider than
    // the model traverses badly however good each prototype is.
    if (std::getenv("HDCLAUDE_TRACE") != nullptr) {
        float lo[3] = {1e30f, 1e30f, 1e30f};
        float hi[3] = {-1e30f, -1e30f, -1e30f};
        for (const auto& entry : instances) {
            for (int axis = 0; axis < 3; ++axis) {
                const float t = entry.transform.matrix[axis][3];
                lo[axis] = std::min(lo[axis], t);
                hi[axis] = std::max(hi[axis], t);
            }
        }
        std::fprintf(stderr,
                     "hdClaude tlas: %zu instances, translations "
                     "(%g %g %g)-(%g %g %g)\n",
                     instances.size(), lo[0], lo[1], lo[2], hi[0],
                     hi[1], hi[2]);
    }
    _tlas.Build(_context, _allocator, instances);
}

const BottomLevelStructure* SceneAccelerator::Blas(std::uint32_t prototype) const
{
    if (prototype >= _prototypeFingerprints.size()) {
        return nullptr;
    }
    auto found = _byFingerprint.find(_prototypeFingerprints[prototype]);
    return found == _byFingerprint.end() ? nullptr : &found->second;
}

}  // namespace hdclaude
