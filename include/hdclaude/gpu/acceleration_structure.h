// Ray-tracing acceleration structures.
//
// One bottom-level structure per distinct mesh prototype, and one top-level
// structure holding an instance per placement. Prototypes are object-space, so
// a BLAS is independent of where its mesh sits and survives both a scene
// publication that did not touch it and the many copies an instancer produces.
//
// Reuse is keyed on the prototype's *fingerprint*, not on its index or its
// name. A scene publication that reorders prototypes, or renames one, must not
// invalidate structures whose geometry is unchanged -- and a prototype whose
// geometry did change must not keep one, however stable its identity looks.

#ifndef HDCLAUDE_GPU_ACCELERATION_STRUCTURE_H
#define HDCLAUDE_GPU_ACCELERATION_STRUCTURE_H

#include <cstdint>
#include <unordered_map>
#include <vector>

#include <volk.h>

#include "hdclaude/gpu/scene.h"
#include "hdclaude/gpu/vulkan_context.h"
#include "hdclaude/gpu/vulkan_resources.h"

namespace hdclaude {

/// Geometry buffers plus the acceleration structure built over them.
///
/// The vertex and index buffers are owned here rather than separately, because
/// the structure references their device addresses: separating their lifetimes
/// would let one be freed while the other still points at it.
class BottomLevelStructure {
  public:
    BottomLevelStructure() = default;
    BottomLevelStructure(const VulkanContext& context, VulkanAllocator& allocator,
                         const MeshPrototype& prototype);
    ~BottomLevelStructure();

    BottomLevelStructure(BottomLevelStructure&&) noexcept;
    BottomLevelStructure& operator=(BottomLevelStructure&&) noexcept;
    BottomLevelStructure(const BottomLevelStructure&) = delete;
    BottomLevelStructure& operator=(const BottomLevelStructure&) = delete;

    bool Valid() const { return _structure != VK_NULL_HANDLE; }
    VkAccelerationStructureKHR Handle() const { return _structure; }
    VkDeviceAddress DeviceAddress() const { return _address; }
    std::uint64_t Fingerprint() const { return _fingerprint; }

    const VulkanBuffer& Positions() const { return _positions; }
    const VulkanBuffer& Indices() const { return _indices; }
    const VulkanBuffer& Normals() const { return _normals; }
    const VulkanBuffer& Uvs() const { return _uvs; }
    std::uint32_t TriangleCount() const { return _triangleCount; }

    void Reset();

  private:
    const VulkanContext* _context = nullptr;
    VkAccelerationStructureKHR _structure = VK_NULL_HANDLE;
    VkDeviceAddress _address = 0;
    VulkanBuffer _storage;
    VulkanBuffer _positions;
    VulkanBuffer _indices;
    VulkanBuffer _normals;
    VulkanBuffer _uvs;
    std::uint32_t _triangleCount = 0;
    std::uint64_t _fingerprint = 0;
};

/// The instance-level structure.
class TopLevelStructure {
  public:
    TopLevelStructure() = default;
    ~TopLevelStructure();

    TopLevelStructure(TopLevelStructure&&) noexcept;
    TopLevelStructure& operator=(TopLevelStructure&&) noexcept;
    TopLevelStructure(const TopLevelStructure&) = delete;
    TopLevelStructure& operator=(const TopLevelStructure&) = delete;

    bool Valid() const { return _structure != VK_NULL_HANDLE; }
    VkAccelerationStructureKHR Handle() const { return _structure; }
    VkDeviceAddress DeviceAddress() const { return _address; }
    std::uint32_t InstanceCount() const { return _instanceCount; }

    /// Build, replacing whatever was here.
    ///
    /// Built into locals and swapped in on success, so a failure leaves the
    /// previous structure intact and still traversable rather than leaving the
    /// renderer with nothing to trace against (docs/architecture.md 6 rule 1).
    void Build(const VulkanContext& context, VulkanAllocator& allocator,
               const std::vector<VkAccelerationStructureInstanceKHR>& instances);

    void Reset();

  private:
    const VulkanContext* _context = nullptr;
    VkAccelerationStructureKHR _structure = VK_NULL_HANDLE;
    VkDeviceAddress _address = 0;
    VulkanBuffer _storage;
    VulkanBuffer _instanceBuffer;
    std::uint32_t _instanceCount = 0;
};

/// Owns the structures for a scene and reuses what it can across publications.
class SceneAccelerator {
  public:
    SceneAccelerator(const VulkanContext& context, VulkanAllocator& allocator);

    /// Bring the structures up to date with `scene`.
    ///
    /// Prototypes whose fingerprint is unchanged keep their existing BLAS; the
    /// rest are built. The TLAS is rebuilt whenever instances change, which is
    /// cheap relative to a BLAS build.
    void Update(const Scene& scene);

    const TopLevelStructure& Tlas() const { return _tlas; }
    const BottomLevelStructure* Blas(std::uint32_t prototype) const;

    /// How many prototypes were rebuilt by the last Update, and how many were
    /// reused. Asserted by the tests: reuse that silently does not happen is a
    /// performance defect invisible in an image.
    std::uint32_t LastBuiltCount() const { return _lastBuilt; }
    std::uint32_t LastReusedCount() const { return _lastReused; }

  private:
    const VulkanContext& _context;
    VulkanAllocator& _allocator;
    TopLevelStructure _tlas;
    /// Keyed by fingerprint, so reuse follows geometry rather than position in
    /// the prototype list.
    std::unordered_map<std::uint64_t, BottomLevelStructure> _byFingerprint;
    std::vector<std::uint64_t> _prototypeFingerprints;
    std::uint32_t _lastBuilt = 0;
    std::uint32_t _lastReused = 0;
};

}  // namespace hdclaude

#endif  // HDCLAUDE_GPU_ACCELERATION_STRUCTURE_H
