#pragma once

// Asking Hydra to sync the geometry again.
//
// Some of hdClaude's settings decide what an rprim *is* rather than how a
// frame of it is drawn: the subdivision level, the curve cross-section, and
// the refinement a camera earns. Changing one of those cannot take effect on
// the next frame, because the prototype was built during Sync -- the prims
// have to be synced again.
//
// The obvious way to ask is `HdChangeTracker::MarkRprimDirty`, and it does not
// work here. A render index driven by scene indices rather than by a scene
// *delegate* refuses that call outright -- "Calling method on HdChangeTracker
// that requires emulation" -- and that is the pipeline `usdrecord` and
// `usdview` have used since OpenUSD turned scene indices on by default. So the
// resync hdClaude already had for curve and subdivision settings has never
// fired in either of them; it worked only against the legacy emulation path,
// which is what the test harness happens to use. Nothing reported it, because
// a resync that does not happen looks exactly like a setting that has not been
// changed yet.
//
// The mechanism a scene-index pipeline does accept is a scene index of one's
// own saying its prims are dirty. This is that: a pass-through filter that
// changes nothing about the scene and exists only so hdClaude has somewhere to
// send the notice from.

#include "pxr/imaging/hd/filteringSceneIndex.h"
#include "pxr/imaging/hd/sceneIndexPlugin.h"
#include "pxr/pxr.h"

#include <cstddef>

PXR_NAMESPACE_OPEN_SCOPE

class HdRenderIndex;

TF_DECLARE_REF_PTRS(HdClaudeRetessellationSceneIndex);

/// A pass-through filter that can declare the geometry dirty.
class HdClaudeRetessellationSceneIndex final
    : public HdSingleInputFilteringSceneIndexBase {
  public:
    static HdClaudeRetessellationSceneIndexRefPtr New(
        const HdSceneIndexBaseRefPtr& inputScene)
    {
        return TfCreateRefPtr(new HdClaudeRetessellationSceneIndex(inputScene));
    }

    HdSceneIndexPrim GetPrim(const SdfPath& primPath) const override;
    SdfPathVector GetChildPrimPaths(const SdfPath& primPath) const override;

    /// Declare every mesh and curve prim entirely dirty, so Hydra syncs the
    /// geometry again. Returns how many prims were named.
    ///
    /// Entirely, rather than by the locators for points and topology: what has
    /// changed is not a value in the scene at all but hdClaude's reading of
    /// it, and there is no locator that says "the renderer would build this
    /// differently now". Naming the whole prim is the honest statement, and
    /// this happens when a setting changes rather than per frame.
    std::size_t DirtyGeometry();

  protected:
    explicit HdClaudeRetessellationSceneIndex(
        const HdSceneIndexBaseRefPtr& inputScene)
        : HdSingleInputFilteringSceneIndexBase(inputScene)
    {
    }

    void _PrimsAdded(const HdSceneIndexBase& sender,
                     const HdSceneIndexObserver::AddedPrimEntries& entries) override;
    void _PrimsRemoved(const HdSceneIndexBase& sender,
                       const HdSceneIndexObserver::RemovedPrimEntries& entries) override;
    void _PrimsDirtied(const HdSceneIndexBase& sender,
                       const HdSceneIndexObserver::DirtiedPrimEntries& entries) override;
};

/// The instance inserted into `index`'s chain, or null.
///
/// Found by walking in from the terminal scene index, because the plugin
/// registry constructs it and hands it to Hydra rather than to the renderer.
/// Null is an ordinary answer -- a host that drives the render index some other
/// way has no chain to walk -- and the caller falls back to the change tracker.
HdClaudeRetessellationSceneIndex* HdClaudeFindRetessellationSceneIndex(
    const HdRenderIndex* index);

class HdClaude_RetessellationSceneIndexPlugin final : public HdSceneIndexPlugin {
  public:
    HdClaude_RetessellationSceneIndexPlugin();

  protected:
    HdSceneIndexBaseRefPtr _AppendSceneIndex(
        const HdSceneIndexBaseRefPtr& inputScene,
        const HdContainerDataSourceHandle& inputArgs) override;
};

PXR_NAMESPACE_CLOSE_SCOPE
