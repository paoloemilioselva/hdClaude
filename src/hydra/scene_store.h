#pragma once

// Where Hydra's per-prim Sync() results are accumulated into a renderer scene.
//
// Hydra syncs prims independently and in any order, so the renderer must never
// observe a half-updated scene. Prims publish here; the render pass takes a
// coherent snapshot when the revision changes (docs/architecture.md 4).
//
// This is the one place that knows how a Hydra prim path maps onto a renderer
// prototype and material index. Nothing below it knows about OpenUSD, and
// nothing above it knows about Vulkan.

#include "hdclaude/gpu/path_tracer.h"
#include "hdclaude/gpu/scene.h"

#include "pxr/usd/sdf/path.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

/// A mesh as published by its Hydra prim.
struct HdClaudeMeshEntry {
    hdclaude::MeshPrototype prototype;
    /// One entry per instance. A non-instanced mesh publishes exactly one.
    std::vector<hdclaude::Transform3x4> transforms;
    SdfPath material;
    bool visible = true;
};

/// A compiled material, keyed by the path of the Hydra material prim.
struct HdClaudeMaterialEntry {
    hdclaude::CompiledMaterial compiled;
    /// Why this material is not being used as authored, if it is not. Empty
    /// when the authored network compiled. Reported once rather than per
    /// frame, and surfaced through render stats.
    std::string fallbackReason;
};

/// Thread-safe accumulator and snapshot source.
class HdClaudeSceneStore {
  public:
    // --- Publication, from prim Sync() ---------------------------------------

    void PublishMesh(const SdfPath& id, HdClaudeMeshEntry entry);
    void RemoveMesh(const SdfPath& id);

    void PublishMaterial(const SdfPath& id, HdClaudeMaterialEntry entry);
    void RemoveMaterial(const SdfPath& id);

    /// True if a material prim of this path has been published.
    bool HasMaterial(const SdfPath& id) const;

    // --- Consumption, from the render pass ------------------------------------

    /// Bumped by every publication. The render pass compares it to decide
    /// whether to rebuild, so one comparison covers geometry and shading alike.
    std::uint64_t Revision() const;

    /// Build a coherent scene and the material table its indices refer to.
    ///
    /// Material index 0 is always the fallback, so a mesh with no usable
    /// material still renders as something rather than being dropped.
    hdclaude::Scene Snapshot(std::vector<hdclaude::CompiledMaterial>& materials) const;

    /// Materials that did not compile as authored, for render stats.
    std::vector<std::string> FallbackReports() const;

    /// The fallback material, published once by the delegate.
    void SetFallbackMaterial(hdclaude::CompiledMaterial material);
    bool HasFallbackMaterial() const;

  private:
    mutable std::mutex _mutex;
    std::map<SdfPath, HdClaudeMeshEntry> _meshes;
    std::map<SdfPath, HdClaudeMaterialEntry> _materials;
    hdclaude::CompiledMaterial _fallback;
    bool _hasFallback = false;
    std::uint64_t _revision = 1;
};

PXR_NAMESPACE_CLOSE_SCOPE
