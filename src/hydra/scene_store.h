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
#include "pxr/imaging/hd/sceneIndex.h"
#include "pxr/usd/sdf/path.h"
#include "pxr/usd/sdf/pathExpression.h"

#include <cstdint>
#include <functional>
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

    /// The prim's displayColor, given to every one of its placements. What the
    /// shared material for unbound geometry shades with, and what any material
    /// reading a `displayColor` geomprop receives.
    float displayColor[3] = {0.5f, 0.5f, 0.5f};

    /// The rprim's `HdRprim::GetPrimId`, which the `primId` AOV reports and a
    /// picking host maps back to a path through the render index.
    int primId = -1;
};

/// Where the one material for unbound geometry is published.
///
/// Every mesh and curve set with no binding shares it, and its colour comes
/// from each placement's displayColor. A property path, so it can never be
/// mistaken for -- or collide with -- a material prim the stage contains.
inline SdfPath HdClaudeDisplayColorMaterialPath()
{
    static const SdfPath path =
        SdfPath("/__hdClaude").AppendProperty(TfToken("displayColorMaterial"));
    return path;
}

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

    /// The rprim's prim id. See `HdClaudeMeshEntry::primId`.
    int primId = -1;
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
    /// The compiled network's content key, or empty. Materials that share one
    /// share a material index in the snapshot, so the GPU builds one pipeline
    /// and runs one shading dispatch for all of them rather than one each.
    std::string shareKey;
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

/// A cull volume as published by its Hydra prim (docs/cull-volumes.md).
///
/// Two kinds of field, because they cost different amounts to change. The
/// volume's placement, shape and invert flag are uploaded on their own, so a
/// volume can be moved interactively without a scene publication. Whether it is
/// active, which rays it affects and which geometry it links decide every
/// placement's instance mask, and changing those publishes the scene.
struct HdClaudeCullVolumeEntry {
    /// World-to-unit transform, shape, axis and flags, as the kernels read it.
    /// `CullShape::None` when the volume is invalid as authored.
    hdclaude::CullVolume volume;
    /// Visible. Whether it is of a purpose a render draws is the render's to
    /// say (HdClaudeDrawSelection), so it is kept beside rather than folded in.
    bool visible = true;
    /// The authored purpose, empty for the default one.
    TfToken purpose;
    /// The `cullLink` collection's membership expression. An empty one
    /// matches nothing, as it does in USD: it is what a collection that
    /// includes nothing resolves to.
    SdfPathExpression cullLink;
    /// Whether the volume links every placement without asking `cullLink`:
    /// the expression is `//`, or no collection was published at all, which
    /// leaves the schema's own default of the whole stage.
    bool linksEverything = true;
    /// Why the volume is ignored, if it is. Reported by the prim when it
    /// changes.
    std::string report;
};

/// What one render pass draws of what the store holds.
///
/// Hydra syncs only the prims a pass's render tags and collection include, so
/// a prim a later toggle excludes -- a purpose turned off in a viewer's menu --
/// is simply never synced again, and its last publication stays in the store.
/// Drawing what the store holds would go on drawing it. The pass says instead
/// which rprims it includes, by id, and which cull-volume purposes; empty
/// functions include every rprim, and every volume but a guide or a proxy,
/// which is what hdClaude drew before a pass said.
struct HdClaudeDrawSelection {
    std::function<bool(const SdfPath& rprim)> includesRprim;
    std::function<bool(const TfToken& purpose)> includesPurpose;

    bool IncludesRprim(const SdfPath& id) const
    {
        return !includesRprim || includesRprim(id);
    }
    bool IncludesPurpose(const TfToken& purpose) const;
};

/// Thread-safe accumulator and snapshot source.
class HdClaudeSceneStore {
  public:
    // --- Publication, from prim Sync() ---------------------------------------

    void PublishMesh(const SdfPath& id, HdClaudeMeshEntry entry);
    void RemoveMesh(const SdfPath& id);

    /// Where an already published mesh is and how it is bound, leaving its
    /// prototype as it was. What a transform, visibility, instancing or
    /// binding edit changes, without regenerating geometry nothing touched.
    /// False, changing nothing, when no entry is held for `id`.
    bool UpdateMeshPlacement(const SdfPath& id,
                             std::vector<hdclaude::Transform3x4> transforms,
                             bool visible, SdfPath material,
                             std::vector<std::vector<TfToken>> instanceCategories,
                             int primId);

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

    /// Publish a cull volume. Bumps `CullRevision` always, and `Revision` only
    /// when what changed decides instance masks (HdClaudeCullVolumeEntry).
    void PublishCullVolume(const SdfPath& id, HdClaudeCullVolumeEntry entry);
    void RemoveCullVolume(const SdfPath& id);

    /// The scene index cull-link expressions are evaluated against. Their
    /// predicates may ask about any prim, which only the scene index knows.
    void SetSceneIndex(const HdSceneIndexBaseRefPtr& sceneIndex);

    /// The `cullVolumes` render setting. Off, every volume is ignored, so a
    /// render can be compared with and without them without editing the stage.
    void SetCullVolumesEnabled(bool enabled);

    // --- Consumption, from the render pass ------------------------------------

    /// Bumped by every publication. The render pass compares it to decide
    /// whether to rebuild, so one comparison covers geometry and shading alike.
    std::uint64_t Revision() const;

    /// Bumped by every cull volume publication, including the ones that only
    /// moved a volume. The render pass uploads `CullVolumes()` when it changes.
    std::uint64_t CullRevision() const;

    /// The active cull volumes, in the order their bits are numbered in every
    /// snapshot's `cullLinks`: by path, the first kMaxCullVolumes of them.
    /// Active is visible and of a purpose `selection` includes, and it has to
    /// be the selection the snapshot was taken with, or the bits disagree.
    std::vector<hdclaude::CullVolume> CullVolumes(
        const HdClaudeDrawSelection& selection = {}) const;

    /// Build a coherent scene and the material table its indices refer to.
    ///
    /// Material index 0 is always the fallback, so a mesh with no usable
    /// material still renders as something rather than being dropped.
    /// Not const: taking a snapshot records it as the candidate for the next
    /// one's past, which is what lets an instance say where it was. A
    /// snapshot is a point in a sequence rather than a pure view of the store.
    hdclaude::Scene Snapshot(std::vector<hdclaude::CompiledMaterial>& materials,
                             const HdClaudeDrawSelection& selection = {});

    /// The last snapshot has been rendered: its placements become the past
    /// the next snapshot's motion is measured from.
    ///
    /// Separate from `Snapshot` because a snapshot can fail to publish, or
    /// publish and then fail to render, and neither was ever seen. Motion
    /// measured from it would be measured from a frame no host showed. Called
    /// after every completed frame; only the first after a snapshot does
    /// anything.
    void CommitHistory();

    /// Materials that did not compile as authored, for render stats.
    std::vector<std::string> FallbackReports() const;

    /// The fallback material, published once by the delegate.
    void SetFallbackMaterial(hdclaude::CompiledMaterial material);
    bool HasFallbackMaterial() const;

  private:
    mutable std::mutex _mutex;
    std::map<SdfPath, HdClaudeMeshEntry> _meshes;
    std::map<SdfPath, HdClaudeSplatEntry> _splats;
    /// Where each splat cloud's instances were on the last rendered snapshot,
    /// for the same reason `_previousTransforms` holds it for meshes.
    std::map<SdfPath, std::vector<hdclaude::Transform3x4>> _previousSplatTransforms;

    /// The last snapshot's placements, waiting for a frame of it to complete.
    std::map<SdfPath, std::vector<hdclaude::Transform3x4>> _pendingTransforms;
    std::map<SdfPath, std::vector<hdclaude::Transform3x4>> _pendingSplatTransforms;
    bool _historyPending = false;

    /// Where each mesh's instances were on the last snapshot that was
    /// rendered.
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
    std::map<SdfPath, HdClaudeCullVolumeEntry> _cullVolumes;
    HdSceneIndexBaseRefPtr _sceneIndex;
    bool _cullVolumesEnabled = true;
    std::uint64_t _cullRevision = 1;
    /// The volumes past kMaxCullVolumes the last snapshot ignored, so the
    /// report is made when that set changes rather than on every snapshot.
    std::string _reportedCullOverflow;
    /// What the last snapshot could not honour across lights, such as two dome
    /// lights linked differently. Reported beside the per-entry reports.
    std::vector<std::string> _snapshotReports;
    hdclaude::CompiledMaterial _fallback;
    bool _hasFallback = false;
    std::uint64_t _revision = 1;

    /// The active volumes' paths in bit order. Called with the lock held.
    std::vector<SdfPath> _ActiveCullVolumes(
        const HdClaudeDrawSelection& selection) const;
};

PXR_NAMESPACE_CLOSE_SCOPE
