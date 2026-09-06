#pragma once

#include "pxr/imaging/hd/light.h"

PXR_NAMESPACE_OPEN_SCOPE

/// Publishes a UsdLux light as an analytic emitter.
///
/// hdClaude's lights are not geometry, but they are hittable: they stay out of
/// the acceleration structure and are intersected in closed form instead, which
/// is what a rect or a sphere already is. A scattered ray can therefore reach
/// one, so the same light arrives by next-event estimation and by BSDF sampling
/// and the two are weighed by the balance heuristic (docs/architecture.md 2).
/// That is what puts a light in a mirror and a highlight in a glass ball, which
/// no amount of sampling could do while nothing could hit them.
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
