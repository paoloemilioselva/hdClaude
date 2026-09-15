#pragma once

// Point instancing.
//
// A renderer has to compute instance transforms itself. `HdInstancer` holds
// the primvars and the parent chain and computes nothing; the one call that
// looks as though it does the work -- `HdRprim::GetInstancerTransforms` --
// returns one matrix per instancer in the chain, which is the *instancer's*
// own transform, not the transform of each instance. Reading it as the latter
// renders a point-instanced prototype exactly once, at the instancer's root:
// hdClaude drew 21 objects for the OpenChessSet's 32 pieces and a board, and
// the missing pieces were the ones the instancer would have placed.

#include "pxr/imaging/hd/instancer.h"
#include "pxr/base/tf/hashmap.h"
#include "pxr/base/vt/value.h"

#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

class HdClaudeInstancer : public HdInstancer {
  public:
    HdClaudeInstancer(HdSceneDelegate* delegate, const SdfPath& id);
    ~HdClaudeInstancer() override;

    void Sync(HdSceneDelegate* sceneDelegate, HdRenderParam* renderParam,
              HdDirtyBits* dirtyBits) override;

    /// One world-space transform per instance of `prototypeId`.
    ///
    /// Composed in the order USD defines: the instancer's own transform, then
    /// translate, rotate and scale from the instancer's primvars, then the
    /// per-instance `instanceTransform` matrix. Nested instancers multiply,
    /// so a prototype under two levels of instancing appears once per pair.
    VtMatrix4dArray ComputeInstanceTransforms(const SdfPath& prototypeId);

    /// The light-linking categories of each instance of `prototypeId`, in the
    /// same order `ComputeInstanceTransforms` returns them.
    ///
    /// An instance is in a category when Hydra says so of it -- through
    /// `GetInstanceCategories`, which is how a native instance is linked -- or
    /// of the instancer, which is how a point instancer is linked and applies
    /// to all its instances. Nested instancers take the union with each of
    /// their parent's instances, since a collection that includes an outer
    /// instance includes everything inside it.
    std::vector<std::vector<TfToken>> ComputeInstanceCategories(
        const SdfPath& prototypeId);

  private:
    void _SyncPrimvars(HdSceneDelegate* delegate, HdDirtyBits dirtyBits);

    /// The instancer's primvars, by name. Held as `VtValue` because the
    /// authored types vary -- rotations arrive as quaternions or as vec4s
    /// depending on the asset's age -- and the composition below is the only
    /// place that needs to know which.
    TfHashMap<TfToken, VtValue, TfToken::HashFunctor> _primvars;
};

/// The light-linking categories of each of an rprim's `instanceCount`
/// placements: the rprim's own, united with its instancer's for each instance
/// when it has one. Empty when no placement is in any category, which is what
/// the scene store takes to mean the rprim is linked to nothing.
std::vector<std::vector<TfToken>> HdClaudeRprimCategories(
    HdSceneDelegate* delegate, const SdfPath& id, const SdfPath& instancerId,
    std::size_t instanceCount);

PXR_NAMESPACE_CLOSE_SCOPE
