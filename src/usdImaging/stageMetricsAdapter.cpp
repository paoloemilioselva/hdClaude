// The stage's metres per unit and up axis, published on every UsdLux light.
//
// A light authored by what it delivers -- lumens per square metre of emitter,
// lux at a distance in metres, lux on a surface facing up -- needs to know how
// long the stage's unit is and which way is up (docs/usd-proposals.md). Hydra
// tells a render delegate neither: `HdSceneGlobalsSchema` carries the camera,
// the render settings and pass, the time codes and the current frame, and
// nothing of the stage's metrics. A UsdImaging adapter, though, is handed the
// `UsdPrim` it images, and through it the stage.
//
// So this is a *keyless* API schema adapter -- registered with an empty
// `apiSchemaName`, which UsdImaging runs for every prim -- that contributes
// two entries to a light's `light` container:
//
//   hdClaude:stageMetersPerUnit   double, UsdGeomGetStageMetersPerUnit
//   hdClaude:stageUpAxis          token,  UsdGeomGetStageUpAxis
//
// The stage scene index overlays an adapter's container on the prim adapter's
// and the other API schema adapters', recursively, so these sit beside the
// light's own parameters and reach `HdLight::Sync` through
// `GetLightParamValue` like any of them. Every other prim gets nothing, and
// costs one applied-schema check.
//
// Its own small module rather than part of hdClaude's, because UsdImaging
// loads an adapter's library in every session that images a stage, Storm's
// included, and that should not load a Vulkan path tracer.

#include "pxr/base/tf/staticTokens.h"
#include "pxr/base/tf/type.h"
#include "pxr/imaging/hd/lightSchema.h"
#include "pxr/imaging/hd/retainedDataSource.h"
#include "pxr/usd/usdGeom/metrics.h"
#include "pxr/usd/usdLux/lightAPI.h"
#include "pxr/usdImaging/usdImaging/apiSchemaAdapter.h"

PXR_NAMESPACE_OPEN_SCOPE

TF_DEFINE_PRIVATE_TOKENS(
    _tokens,
    ((stageMetersPerUnit, "hdClaude:stageMetersPerUnit"))
    ((stageUpAxis, "hdClaude:stageUpAxis")));

class HdClaudeStageMetricsAdapter : public UsdImagingAPISchemaAdapter {
  public:
    using BaseAdapter = UsdImagingAPISchemaAdapter;

    HdContainerDataSourceHandle GetImagingSubprimData(
        UsdPrim const& prim, TfToken const& subprim,
        TfToken const& appliedInstanceName,
        const UsdImagingDataSourceStageGlobals& /*stageGlobals*/) override
    {
        if (!subprim.IsEmpty() || !appliedInstanceName.IsEmpty() ||
            !prim.HasAPI<UsdLuxLightAPI>()) {
            return nullptr;
        }
        const UsdStageWeakPtr stage = prim.GetStage();
        if (!stage) {
            return nullptr;
        }
        // Read once per light when it is imaged. A change to the stage's
        // metadata later in a session is not seen until the light is imaged
        // again; UsdImaging does not invalidate prims on layer metadata.
        return HdRetainedContainerDataSource::New(
            HdLightSchemaTokens->light,
            HdRetainedContainerDataSource::New(
                _tokens->stageMetersPerUnit,
                HdRetainedTypedSampledDataSource<double>::New(
                    UsdGeomGetStageMetersPerUnit(stage)),
                _tokens->stageUpAxis,
                HdRetainedTypedSampledDataSource<TfToken>::New(
                    UsdGeomGetStageUpAxis(stage))));
    }
};

TF_REGISTRY_FUNCTION(TfType)
{
    using Adapter = HdClaudeStageMetricsAdapter;
    TfType t = TfType::Define<Adapter, TfType::Bases<Adapter::BaseAdapter>>();
    t.SetFactory<UsdImagingAPISchemaAdapterFactory<Adapter>>();
}

PXR_NAMESPACE_CLOSE_SCOPE
