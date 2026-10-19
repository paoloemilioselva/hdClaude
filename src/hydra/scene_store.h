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

#include "pxr/base/tf/token.h"
#include "pxr/imaging/hd/material.h"
#include "pxr/usd/sdf/path.h"

#include <cstdint>
#include <map>
#include <set>
#include <mutex>
#include <string>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

/// A mesh as published by its Hydra prim.
struct HdClaudeMeshEntry {
    hdclaude::MeshPrototype prototype;
    /// One entry per instance. A non-instanced mesh publishes exactly one.
    std::vector<hdclaude::Transform3x4> transforms;
    SdfPath material;
    bool visible = true;

    /// Material bound to each GeomSubset, in subset order.
    ///
    /// Kept as paths rather than indices because the store assigns indices at
    /// snapshot time: a mesh has no way to know what index a material will get,
    /// and may well be synced before that material exists at all.
    std::vector<SdfPath> subsetMaterials;
    /// Which entry of `subsetMaterials` owns each triangle, or -1 for the
    /// mesh's own binding. Empty when the mesh has no subsets.
    std::vector<int> triangleSubsets;

    /// The light-linking categories each entry of `transforms` belongs to, in
    /// the same order. Empty when the mesh is in no category at all, which is
    /// every mesh of a stage that links nothing.
    std::vector<std::vector<TfToken>> instanceCategories;
};

/// A Gaussian splat cloud as published by its Hydra prim.
///
/// No material path, deliberately. A splat carries its own radiance as
/// spherical harmonics and there is no reflectance anywhere in the schema to
/// shade it with, so a material bound to a `ParticleField` is reported rather
/// than applied (docs/gaussian-splats.md 4).
struct HdClaudeSplatEntry {
    hdclaude::SplatPrototype prototype;
    /// One entry per instance. A non-instanced cloud publishes exactly one.
    std::vector<hdclaude::Transform3x4> transforms;
    bool visible = true;

    /// The light-linking categories each entry of `transforms` belongs to.
    std::vector<std::vector<TfToken>> instanceCategories;

    /// What about this cloud is not honoured as authored, if anything: the
    /// schema's own validation rules, plus the rasterizer hints a ray tracer
    /// has nothing to do with. Reported once rather than per frame.
    std::vector<std::string> reports;
};

/// A material, keyed by the path of the Hydra material prim.
///
/// Published *pending* by Sync and compiled only once something that will be
/// rendered binds it. Hydra syncs every material in the stage whether or not a
/// prim uses it, and a stage with material variants carries every variant's
/// materials; compiling them all, and decoding every texture they name, is
/// what a material Sync cost before this was lazy.
struct HdClaudeMaterialEntry {
    hdclaude::CompiledMaterial compiled;
    /// Why this material is not being used as authored, if it is not. Empty
    /// when the authored network compiled. Reported once rather than per
    /// frame, and surfaced through render stats.
    std::string fallbackReason;

    /// The authored network, held while the material is pending and dropped
    /// once it is compiled.
    HdMaterialNetworkMap network;
    /// True from Sync until the material is compiled.
    bool pending = false;
    /// Which publication this is, so a compilation finished after the
    /// material was published again is not mistaken for the current one.
    std::uint64_t generation = 0;
};

/// A pending material something visible binds, ready to be compiled.
struct HdClaudePendingMaterial {
    SdfPath id;
    HdMaterialNetworkMap network;
    std::uint64_t generation = 0;
};

/// A light as published by its Hydra prim.
///
/// A dome light is not an emitter in the light table: it supplies the radiance
/// a ray sees on leaving the scene, which the environment kernel already
/// returns. Keeping both cases in one entry means the store has a single map
/// keyed by prim path, so removal and revision bumping do not have to know
/// which kind a path was.
struct HdClaudeLightEntry {
    hdclaude::Light light;
    bool isDome = false;
    float environmentColor[3] = {0.0f, 0.0f, 0.0f};
    /// Pool slot of the dome's latitude-longitude map, or -1.
    int domeTexture = -1;
    /// World-to-light for the dome, column-major.
    float domeColorTemperature = 0.0f;
    float domeTemperatureScale = 1.0f;
    float domeWorldToLight[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    float domeLightToWorld[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    /// What about this light is not honoured as authored, if anything.
    std::string report;

    /// The Hydra categories of the light's `collection:lightLink` and
    /// `collection:shadowLink`. Empty for a collection that includes everything,
    /// which is how Hydra reports one.
    TfToken lightLink;
    TfToken shadowLink;
};

/// Thread-safe accumulator and snapshot source.
class HdClaudeSceneStore {
  public:
    // --- Publication, from prim Sync() ---------------------------------------

    void PublishMesh(const SdfPath& id, HdClaudeMeshEntry entry);
    void RemoveMesh(const SdfPath& id);

    void PublishSplats(const SdfPath& id, HdClaudeSplatEntry entry);
    void RemoveSplats(const SdfPath& id);

    void PublishMaterial(const SdfPath& id, HdClaudeMaterialEntry entry);
    void RemoveMaterial(const SdfPath& id);

    /// True if a material prim of this path has been published.
    bool HasMaterial(const SdfPath& id) const;

    /// Pending materials that a visible mesh, curve or GeomSubset binds.
    ///
    /// The bound set is only known here, after every rprim has published: Hydra
    /// syncs sprims before rprims, so a material at Sync cannot tell whether
    /// anything will use it. Networks are copied out so the compile runs
    /// without the store's lock -- a compile is long, and publication from
    /// other prims must not wait on it.
    std::vector<HdClaudePendingMaterial> BoundPendingMaterials() const;

    /// Replace a pending material with its compiled form, unless it was
    /// published again or removed since `generation` was handed out.
    void CompletePendingMaterial(const SdfPath& id, std::uint64_t generation,
                                 HdClaudeMaterialEntry compiled);

    void PublishLight(const SdfPath& id, HdClaudeLightEntry entry);
    void RemoveLight(const SdfPath& id);

    // --- Consumption, from the render pass ------------------------------------

    /// Bumped by every publication. The render pass compares it to decide
    /// whether to rebuild, so one comparison covers geometry and shading alike.
    std::uint64_t Revision() const;

    /// Build a coherent scene and the material table its indices refer to.
    ///
    /// Material index 0 is always the fallback, so a mesh with no usable
    /// material still renders as something rather than being dropped.
    /// Not const: taking a snapshot records it as the next one's past, which
    /// is what lets an instance say where it was. A snapshot is a point in a
    /// sequence rather than a pure view of the store.
    hdclaude::Scene Snapshot(std::vector<hdclaude::CompiledMaterial>& materials);

    /// Materials that did not compile as authored, for render stats.
    std::vector<std::string> FallbackReports() const;

    /// The fallback material, published once by the delegate.
    void SetFallbackMaterial(hdclaude::CompiledMaterial material);
    bool HasFallbackMaterial() const;

  private:
    mutable std::mutex _mutex;
    std::map<SdfPath, HdClaudeMeshEntry> _meshes;
    std::map<SdfPath, HdClaudeSplatEntry> _splats;
    /// Where each splat cloud's instances were on the previous snapshot, for
    /// the same reason `_previousTransforms` holds it for meshes.
    std::map<SdfPath, std::vector<hdclaude::Transform3x4>> _previousSplatTransforms;

    /// Where each mesh's instances were on the previous snapshot.
    ///
    /// Kept here rather than on the entry because it is a property of the
    /// *sequence of snapshots* rather than of a mesh: an rprim republishes
    /// whenever anything about it changes and has no idea which publication is
    /// which, while the store sees them in order and is the only thing that
    /// does. A mesh absent from this map has no history and is reported as
    /// having none, which the renderer turns into no motion.
    std::map<SdfPath, std::vector<hdclaude::Transform3x4>> _previousTransforms;

    /// Meshes already reported for sampling an image with no texture
    /// coordinates. Said once per mesh rather than once per publication: a
    /// stage that republishes every frame would otherwise bury the message it
    /// is trying to deliver.
    std::set<SdfPath> _reportedMissingUvs;
    std::map<SdfPath, HdClaudeMaterialEntry> _materials;
    std::uint64_t _materialGeneration = 0;
    std::map<SdfPath, HdClaudeLightEntry> _lights;
    /// What the last snapshot could not honour across lights, such as two dome
    /// lights linked differently. Reported beside the per-entry reports.
    std::vector<std::string> _snapshotReports;
    hdclaude::CompiledMaterial _fallback;
    bool _hasFallback = false;
    std::uint64_t _revision = 1;
};

PXR_NAMESPACE_CLOSE_SCOPE
