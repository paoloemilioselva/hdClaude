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
    /// The colour the generated displayColor material was last compiled for.
    /// Compared before recompiling, so an animated mesh does not rebuild a
    /// pipeline every frame for a colour that has not changed.
    GfVec3f _lastDisplayColor{-1.0f, -1.0f, -1.0f};
};

PXR_NAMESPACE_CLOSE_SCOPE
