#pragma once

#include "pxr/imaging/hd/material.h"

PXR_NAMESPACE_OPEN_SCOPE

class HdClaudeRenderParam;
struct HdClaudeMaterialEntry;

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

/// Compile every pending material something visible binds, and load the
/// textures they sample.
///
/// Called by the render pass before it reads the scene revision, so the
/// materials it compiles are part of the snapshot it is about to take rather
/// than a reason to take another one.
void HdClaudeCompileBoundMaterials(HdClaudeRenderParam* param);

PXR_NAMESPACE_CLOSE_SCOPE
