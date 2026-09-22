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
    /// Build. `allowUpdate` asks for a structure that can later be refitted.
    ///
    /// Not the default, because it is not free: a structure built to be
    /// updatable is a little larger and traverses a little slower, and most
    /// prototypes in most scenes never move. The accelerator asks for it the
    /// *second* time it sees a topology, which is the first moment there is
    /// evidence that this geometry deforms.
    BottomLevelStructure(const VulkanContext& context, VulkanAllocator& allocator,
                         const MeshPrototype& prototype, bool allowUpdate = false);
    /// Build over a Gaussian splat cloud.
    ///
    /// Boxes again, as a curve set is, and for the same reason: the kernel is
    /// intersected in the traversal shader, so what the structure partitions is
    /// each particle's support rather than any surface. The particles and their
    /// radiance are held here beside the structure because it hands their
    /// addresses to the kernels, exactly as it does a mesh's normals.
    BottomLevelStructure(const VulkanContext& context, VulkanAllocator& allocator,
                         const SplatPrototype& prototype);
    ~BottomLevelStructure();

    BottomLevelStructure(BottomLevelStructure&&) noexcept;
    BottomLevelStructure& operator=(BottomLevelStructure&&) noexcept;
    BottomLevelStructure(const BottomLevelStructure&) = delete;
    BottomLevelStructure& operator=(const BottomLevelStructure&) = delete;

    bool Valid() const { return _structure != VK_NULL_HANDLE; }
    VkAccelerationStructureKHR Handle() const { return _structure; }
    VkDeviceAddress DeviceAddress() const { return _address; }
    std::uint64_t Fingerprint() const { return _fingerprint; }
    std::uint64_t Topology() const { return _topology; }
    /// Whether this structure was built so that `Refit` can work on it.
    bool Updatable() const { return _updatable; }

    /// Move the vertices without rebuilding.
    ///
    /// A Vulkan acceleration-structure update keeps the tree it already has and
    /// moves its bounds to follow the new positions, which is what makes it
    /// cheap: the topology is unchanged by definition, so nothing has to be
    /// partitioned again. The vertex, normal and UV buffers are overwritten in
    /// place rather than reallocated -- a second copy of a deforming groom is
    /// exactly the allocation that pushes a frame past the device's memory and
    /// into host-visible spill.
    ///
    /// Returns false, having changed nothing, when this structure was not built
    /// updatable or the prototype is not the same topology. A caller that gets
    /// false rebuilds.
    ///
    /// The cost is BVH quality: refitting only moves bounds, so a mesh that
    /// deforms far from the shape it was built around traverses more slowly
    /// each time. hdClaude refits for as long as the topology holds and does not
    /// yet rebuild on a quality measure; what that measure should be is not
    /// something to invent without one.
    bool Refit(VulkanAllocator& allocator, const MeshPrototype& prototype);

    const VulkanBuffer& Positions() const { return _positions; }
    const VulkanBuffer& Indices() const { return _indices; }
    const VulkanBuffer& Normals() const { return _normals; }
    const VulkanBuffer& Uvs() const { return _uvs; }
    /// Curve segments, eight floats each. Empty for a triangle prototype.
    const VulkanBuffer& Segments() const { return _segments; }
    const VulkanBuffer& SegmentMaterials() const { return _segmentMaterials; }
    /// Particles, thirteen floats each. Empty for anything but a splat cloud.
    const VulkanBuffer& Splats() const { return _splats; }
    /// Spherical-harmonics coefficients, particle-major.
    const VulkanBuffer& Harmonics() const { return _harmonics; }
    bool IsCurve() const { return _curve; }
    bool IsSplat() const { return _splat; }
    /// The degree every particle of this cloud shares.
    std::uint32_t HarmonicsDegree() const { return _harmonicsDegree; }
    /// Which spatial basis function the particles instantiate.
    SplatKernel Kernel() const { return _kernel; }
    /// How many primitives the structure holds: triangles, or segments.
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
    VulkanBuffer _segments;
    VulkanBuffer _segmentMaterials;
    /// The boxes the structure is partitioned over. Held because the structure
    /// references them and a refit rewrites them.
    VulkanBuffer _aabbs;
    VulkanBuffer _splats;
    VulkanBuffer _harmonics;
    bool _curve = false;
    bool _splat = false;
    std::uint32_t _harmonicsDegree = 0;
    SplatKernel _kernel = SplatKernel::GaussianEllipsoid;
    std::uint32_t _triangleCount = 0;
    std::uint32_t _vertexCount = 0;
    std::uint64_t _fingerprint = 0;
    std::uint64_t _topology = 0;
    bool _updatable = false;
    OpacityClass _opacity = OpacityClass::Opaque;
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
    /// The structure built over splat prototype `prototype`, or null.
    ///
    /// A separate lookup rather than a shared one, because the two prototype
    /// lists are numbered independently: splat prototype 0 and mesh prototype 0
    /// both exist and are different bodies.
    const BottomLevelStructure* SplatBlas(std::uint32_t prototype) const;

    /// How many prototypes were rebuilt by the last Update, and how many were
    /// reused. Asserted by the tests: reuse that silently does not happen is a
    /// performance defect invisible in an image.
    std::uint32_t LastBuiltCount() const { return _lastBuilt; }
    std::uint32_t LastReusedCount() const { return _lastReused; }
    /// How many kept their tree and moved its bounds to follow new vertices.
    std::uint32_t LastRefitCount() const { return _lastRefit; }

  private:
    const VulkanContext& _context;
    VulkanAllocator& _allocator;
    TopLevelStructure _tlas;
    /// Keyed by fingerprint, so reuse follows geometry rather than position in
    /// the prototype list.
    std::unordered_map<std::uint64_t, BottomLevelStructure> _byFingerprint;
    std::vector<std::uint64_t> _prototypeFingerprints;
    std::unordered_map<std::uint64_t, BottomLevelStructure> _bySplatFingerprint;
    std::vector<std::uint64_t> _splatPrototypeFingerprints;
    std::uint32_t _lastBuilt = 0;
    std::uint32_t _lastReused = 0;
    std::uint32_t _lastRefit = 0;
};

}  // namespace hdclaude

#endif  // HDCLAUDE_GPU_ACCELERATION_STRUCTURE_H
