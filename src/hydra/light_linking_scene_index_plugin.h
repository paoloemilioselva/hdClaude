#pragma once

// Light linking, resolved the way OpenUSD resolves it for every renderer.
//
// UsdLux links a light to geometry through two collections on the light,
// `lightLink` and `shadowLink`. A render delegate does not read collections:
// it reads *categories*. Each light's `lightLink` and `shadowLink` parameters
// name one, and `HdSceneDelegate::GetCategories` lists the ones an rprim is in
// (with `GetInstanceCategories` doing the same per instance).
//
// Through the legacy UsdImagingDelegate those answers come from UsdImaging's own
// collection cache. Through a scene index -- which is how usdview and usdrecord
// run in OpenUSD 26.03 -- nothing computes them unless a scene index does, and
// OpenUSD ships that scene index as `HdsiLightLinkingSceneIndex` without
// inserting it for any renderer. Without it, every light reports no link and
// every rprim no category, so an asset's linking is silently gone: ALab's sun
// and sky were blocked by the 3.5 km skydome their shadow links exclude. This
// plugin inserts it for hdClaude, exactly as hdPrman inserts it for RenderMan.

#include "pxr/imaging/hd/sceneIndexPlugin.h"
#include "pxr/pxr.h"

PXR_NAMESPACE_OPEN_SCOPE

class HdClaude_LightLinkingSceneIndexPlugin final : public HdSceneIndexPlugin {
  public:
    HdClaude_LightLinkingSceneIndexPlugin();

  protected:
    HdSceneIndexBaseRefPtr _AppendSceneIndex(
        const HdSceneIndexBaseRefPtr& inputScene,
        const HdContainerDataSourceHandle& inputArgs) override;
};

PXR_NAMESPACE_CLOSE_SCOPE
