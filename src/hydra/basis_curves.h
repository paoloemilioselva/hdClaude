#pragma once

#include "pxr/base/gf/vec3f.h"
#include "pxr/imaging/hd/basisCurves.h"

PXR_NAMESPACE_OPEN_SCOPE

/// Publishes Hydra curves as a swept tube, through the same path a mesh takes.
///
/// Curves reach the renderer as triangles rather than as a primitive of their
/// own, which is a deliberate choice recorded in src/hydra/curves.h: it reuses
/// the acceleration structure, the shading and the descriptor set layout
/// exactly as they are, and the alternative -- procedural geometry with the
/// sweep intersected analytically -- changes the traversal kernel that
/// everything else is measured against.
class HdClaudeBasisCurves final : public HdBasisCurves {
  public:
    explicit HdClaudeBasisCurves(const SdfPath& id);
    ~HdClaudeBasisCurves() override;

    HdDirtyBits GetInitialDirtyBitsMask() const override;

    void Sync(HdSceneDelegate* sceneDelegate, HdRenderParam* renderParam,
              HdDirtyBits* dirtyBits, const TfToken& reprToken) override;

    void Finalize(HdRenderParam* renderParam) override;

  protected:
    void _InitRepr(const TfToken& reprToken, HdDirtyBits* dirtyBits) override;
    HdDirtyBits _PropagateDirtyBits(HdDirtyBits bits) const override;

  private:
    /// The colour the generated displayColor material was last compiled for,
    /// so an animated curve set does not rebuild a pipeline for a colour that
    /// has not changed.
    GfVec3f _lastDisplayColor{-1.0f, -1.0f, -1.0f};
};

PXR_NAMESPACE_CLOSE_SCOPE
