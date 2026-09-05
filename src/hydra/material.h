#pragma once

#include "pxr/imaging/hd/material.h"

PXR_NAMESPACE_OPEN_SCOPE

/// Compiles a Hydra material network into a shading pipeline.
///
/// The compilation itself lives in HdClaudeMaterialCompiler; this is the Hydra
/// prim that owns the lifetime and reacts to dirtiness.
class HdClaudeMaterial final : public HdMaterial {
  public:
    explicit HdClaudeMaterial(const SdfPath& id);
    ~HdClaudeMaterial() override;

    HdDirtyBits GetInitialDirtyBitsMask() const override;

    void Sync(HdSceneDelegate* sceneDelegate, HdRenderParam* renderParam,
              HdDirtyBits* dirtyBits) override;

    void Finalize(HdRenderParam* renderParam) override;
};

PXR_NAMESPACE_CLOSE_SCOPE
