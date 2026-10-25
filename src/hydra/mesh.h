#pragma once

#include "pxr/base/gf/vec3f.h"
#include "pxr/imaging/hd/mesh.h"

PXR_NAMESPACE_OPEN_SCOPE

/// Publishes a Hydra mesh as a renderer prototype plus its instance transforms.
///
/// Geometry is kept object-space and instancing is expressed as transforms, so
/// one acceleration structure serves every copy (docs/architecture.md 7).
class HdClaudeMesh final : public HdMesh {
  public:
    explicit HdClaudeMesh(const SdfPath& id);
    ~HdClaudeMesh() override;

    HdDirtyBits GetInitialDirtyBitsMask() const override;

    void Sync(HdSceneDelegate* sceneDelegate, HdRenderParam* renderParam,
              HdDirtyBits* dirtyBits, const TfToken& reprToken) override;

    void Finalize(HdRenderParam* renderParam) override;

  protected:
    void _InitRepr(const TfToken& reprToken, HdDirtyBits* dirtyBits) override;
    HdDirtyBits _PropagateDirtyBits(HdDirtyBits bits) const override;

  private:
    /// Republish only the placements when nothing else is dirty. True when it
    /// did, and the full sync is skipped.
    bool _SyncPlacementOnly(HdSceneDelegate* sceneDelegate,
                            HdRenderParam* renderParam, HdDirtyBits dirtyBits);

    /// Whether the last full sync refined this mesh to a level its placements
    /// decided -- adaptive or per-face refinement of a subdivision surface --
    /// so that moving it changes its geometry and a placement-only sync
    /// would keep a refinement chosen for where it was.
    bool _refinementFollowsPlacement = true;
};

PXR_NAMESPACE_CLOSE_SCOPE
