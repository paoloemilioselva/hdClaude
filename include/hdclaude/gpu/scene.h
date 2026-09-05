// The renderer's scene snapshot.
//
// Plain data with no Vulkan and no OpenUSD in it. Hydra adapters publish one of
// these; the GPU backend consumes it. That boundary is what lets scene
// synchronisation be tested without a GPU and traversal be tested without a USD
// runtime (docs/architecture.md 3).
//
// Geometry is kept in *object space*, with instancing expressed as transforms.
// A prototype's acceleration structure is then independent of where it is
// placed, which is what makes it reusable across scene publications and across
// the many copies a PointInstancer produces.

#ifndef HDCLAUDE_GPU_SCENE_H
#define HDCLAUDE_GPU_SCENE_H

#include <cstdint>
#include <string>
#include <vector>

namespace hdclaude {

/// Row-major 3x4 object-to-world transform, matching the layout Vulkan's
/// instance structure expects, so no conversion happens at upload.
struct Transform3x4 {
    float m[12] = {1.0f, 0.0f, 0.0f, 0.0f,
                   0.0f, 1.0f, 0.0f, 0.0f,
                   0.0f, 0.0f, 1.0f, 0.0f};
};

/// How a mesh participates in traversal.
///
/// An opaque mesh lets the driver take its fast path and skip any-hit
/// evaluation entirely. This is part of a prototype's identity because changing
/// it changes the acceleration structure's build flags, so a mesh that becomes
/// cut-out cannot reuse an opaque structure.
enum class OpacityClass : std::uint8_t {
    Opaque,
    Cutout,
};

/// One object-space mesh prototype.
struct MeshPrototype {
    /// Interleaved xyz, three floats per vertex.
    std::vector<float> positions;
    /// Triangle indices, three per triangle.
    std::vector<std::uint32_t> indices;
    /// Interleaved xyz shading normals, one per vertex. May be empty.
    std::vector<float> normals;
    /// Interleaved uv, two per vertex. May be empty.
    std::vector<float> uvs;
    /// Per-triangle material index, one per triangle. May be empty, in which
    /// case every triangle uses the instance's material.
    std::vector<std::uint32_t> triangleMaterials;

    OpacityClass opacity = OpacityClass::Opaque;
    std::string debugName;

    std::size_t VertexCount() const { return positions.size() / 3; }
    std::size_t TriangleCount() const { return indices.size() / 3; }

    /// Identity for acceleration-structure reuse.
    ///
    /// Covers the vertex and index data and the opacity class -- everything a
    /// built structure depends on. Two prototypes with the same fingerprint
    /// share a BLAS; a prototype whose fingerprint is unchanged across a scene
    /// publication keeps the one it has.
    ///
    /// A fingerprint collision costs a wrong reuse, so this is a 64-bit hash of
    /// the actual bytes rather than of a summary.
    std::uint64_t Fingerprint() const;
};

/// One placement of a prototype.
struct MeshInstance {
    std::uint32_t prototype = 0;
    Transform3x4 transform;
    /// Material for triangles whose prototype does not name one.
    std::uint32_t material = 0;
    /// Visible to camera rays. Invisible instances still cast shadows unless
    /// excluded separately, matching UsdGeom's purpose semantics.
    bool visible = true;
};

/// An immutable published scene.
///
/// Published as a whole so the renderer never observes a half-updated scene.
/// The revision is what the frame description carries, so a single comparison
/// decides whether acceleration structures need rebuilding
/// (docs/architecture.md 4).
struct Scene {
    std::vector<MeshPrototype> prototypes;
    std::vector<MeshInstance> instances;
    std::uint64_t revision = 0;

    std::size_t TotalTriangles() const;
};

}  // namespace hdclaude

#endif  // HDCLAUDE_GPU_SCENE_H
