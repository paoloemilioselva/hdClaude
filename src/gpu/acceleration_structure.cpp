#include "hdclaude/gpu/acceleration_structure.h"

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
    }
    if (!uvs.empty()) {
        hash = Fnv1a64(uvs.data(), uvs.size() * sizeof(float), hash);
        const auto perCorner = static_cast<std::uint8_t>(uvsPerCorner);
        hash = Fnv1a64(&perCorner, sizeof(perCorner), hash);
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

std::size_t Scene::TotalTriangles() const
{
    std::size_t total = 0;
    for (const MeshInstance& instance : instances) {
        if (instance.prototype < prototypes.size()) {
            total += prototypes[instance.prototype].TriangleCount();
        }
    }
    return total;
}

// ---------------------------------------------------------------------------
// BottomLevelStructure
// ---------------------------------------------------------------------------

BottomLevelStructure::BottomLevelStructure(const VulkanContext& context,
                                           VulkanAllocator& allocator,
                                           const MeshPrototype& prototype)
    : _context(&context)
{
    context.RequireLive("BottomLevelStructure");

    if (prototype.indices.empty() || prototype.positions.empty()) {
        throw VulkanError(VK_ERROR_INITIALIZATION_FAILED,
                          "Empty prototype: " + prototype.debugName);
    }

    _fingerprint = prototype.Fingerprint();
    _triangleCount = static_cast<std::uint32_t>(prototype.TriangleCount());

    const std::string name =
        prototype.debugName.empty() ? std::string("prototype") : prototype.debugName;

    _positions = UploadDeviceLocal(context, allocator, prototype.positions,
                                   kBuildInputUsage, (name + ".positions").c_str());
    _indices = UploadDeviceLocal(context, allocator, prototype.indices,
                                 kBuildInputUsage, (name + ".indices").c_str());
    if (!prototype.normals.empty()) {
        _normals = UploadDeviceLocal(context, allocator, prototype.normals,
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                     (name + ".normals").c_str());
    }
    if (!prototype.uvs.empty()) {
        _uvs = UploadDeviceLocal(context, allocator, prototype.uvs,
                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                 (name + ".uvs").c_str());
    }

    // --- Describe the geometry ----------------------------------------------
    VkAccelerationStructureGeometryKHR geometry{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    // Opaque geometry lets the driver skip any-hit evaluation entirely, which
    // is the single largest traversal win available and costs nothing when the
    // mesh genuinely has no cutouts.
    geometry.flags = prototype.opacity == OpacityClass::Opaque
                         ? VK_GEOMETRY_OPAQUE_BIT_KHR
                         : VkGeometryFlagsKHR{0};

    VkAccelerationStructureGeometryTrianglesDataKHR& triangles =
        geometry.geometry.triangles;
    triangles.sType =
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    triangles.vertexData.deviceAddress = _positions.DeviceAddress();
    triangles.vertexStride = 3 * sizeof(float);
    triangles.maxVertex = static_cast<std::uint32_t>(prototype.VertexCount() - 1);
    triangles.indexType = VK_INDEX_TYPE_UINT32;
    triangles.indexData.deviceAddress = _indices.DeviceAddress();
    triangles.transformData.deviceAddress = 0;

    VkAccelerationStructureBuildGeometryInfoKHR buildInfo{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    // Traversal speed over build speed: a prototype is built once and traversed
    // for the life of the scene, and reuse means even a deforming mesh rebuilds
    // rarely.
    buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
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
    _context = nullptr;
    _triangleCount = 0;
    _fingerprint = 0;
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
      _triangleCount(std::exchange(other._triangleCount, 0)),
      _fingerprint(std::exchange(other._fingerprint, 0))
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
        _triangleCount = std::exchange(other._triangleCount, 0);
        _fingerprint = std::exchange(other._fingerprint, 0);
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

    if (instances.empty()) {
        Reset();
        return;
    }

    // Built into locals; the existing structure stays traversable until the
    // replacement is complete.
    VkAccelerationStructureKHR structure = VK_NULL_HANDLE;
    VulkanBuffer storage;
    VulkanBuffer instanceBuffer;

    instanceBuffer = UploadDeviceLocal(context, allocator, instances,
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

    // Build any prototype we do not already hold, keyed by geometry rather than
    // by index: a publication that reorders prototypes reuses everything.
    _prototypeFingerprints.assign(scene.prototypes.size(), 0);
    std::unordered_map<std::uint64_t, BottomLevelStructure> retained;

    for (std::size_t i = 0; i < scene.prototypes.size(); ++i) {
        const MeshPrototype& prototype = scene.prototypes[i];
        if (prototype.indices.empty()) {
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
            retained.emplace(fingerprint, std::move(existing->second));
            _byFingerprint.erase(existing);
            ++_lastReused;
        } else {
            retained.emplace(fingerprint, BottomLevelStructure(_context, _allocator,
                                                               prototype));
            ++_lastBuilt;
        }
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
