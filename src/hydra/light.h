#pragma once

#include "pxr/imaging/hd/light.h"

PXR_NAMESPACE_OPEN_SCOPE

/// Publishes a UsdLux light as an analytic emitter.
///
/// hdClaude's lights are not geometry: they are sampled directly by next-event
/// estimation and are absent from the acceleration structure. That is what lets
/// the first implementation skip MIS entirely -- a scattered ray cannot hit a
/// light, so there is nothing to double count (docs/architecture.md 2).
///
/// A dome light is not an emitter here at all. It sets the scene's environment
/// radiance, which is what a ray that leaves the scene already returns, so
/// supporting it costs one colour rather than a second sampling path.
class HdClaudeLight final : public HdLight {
  public:
    HdClaudeLight(const TfToken& lightType, const SdfPath& id);
    ~HdClaudeLight() override;

    HdDirtyBits GetInitialDirtyBitsMask() const override;

    void Sync(HdSceneDelegate* sceneDelegate, HdRenderParam* renderParam,
              HdDirtyBits* dirtyBits) override;

    void Finalize(HdRenderParam* renderParam) override;

  private:
    TfToken _lightType;
};

PXR_NAMESPACE_CLOSE_SCOPE
