#pragma once

// Publishes UsdCullVolume prims to Hydra.
//
// One adapter serves CubeCullVolume, SphereCullVolume and PlaneCullVolume:
// it is registered for the abstract CullVolume with includeDerivedPrimTypes
// (plugInfo.json), and the concrete type only decides the `shape` token.
// Every cull volume becomes one prim of the Hydra type `cullVolume`, carrying
// HdCullVolumeSchema beside the ordinary xform, visibility and purpose data
// sources. The `cullLink` collection is not handled here: UsdImaging's own
// CollectionAPI adapter publishes it at collections/cullLink.
//
// Scene index only. A UsdImagingDelegate (the pre-scene-index path) does not
// see cull volumes, which is why this derives from
// UsdImagingSceneIndexPrimAdapter.

#include "usdImagingCullVolume/api.h"

#include "pxr/pxr.h"
#include "pxr/usdImaging/usdImaging/sceneIndexPrimAdapter.h"

PXR_NAMESPACE_OPEN_SCOPE

class UsdImagingCullVolumeAdapter : public UsdImagingSceneIndexPrimAdapter {
  public:
    using BaseAdapter = UsdImagingSceneIndexPrimAdapter;

    USDIMAGINGCULLVOLUME_API
    UsdImagingCullVolumeAdapter();
    USDIMAGINGCULLVOLUME_API
    ~UsdImagingCullVolumeAdapter() override;

    USDIMAGINGCULLVOLUME_API
    TfTokenVector GetImagingSubprims(UsdPrim const& prim) override;

    USDIMAGINGCULLVOLUME_API
    TfToken GetImagingSubprimType(UsdPrim const& prim,
                                  TfToken const& subprim) override;

    USDIMAGINGCULLVOLUME_API
    HdContainerDataSourceHandle GetImagingSubprimData(
        UsdPrim const& prim, TfToken const& subprim,
        const UsdImagingDataSourceStageGlobals& stageGlobals) override;

    USDIMAGINGCULLVOLUME_API
    HdDataSourceLocatorSet InvalidateImagingSubprim(
        UsdPrim const& prim, TfToken const& subprim,
        TfTokenVector const& properties,
        UsdImagingPropertyInvalidationType invalidationType) override;
};

PXR_NAMESPACE_CLOSE_SCOPE
