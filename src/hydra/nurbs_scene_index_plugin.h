#pragma once

// Curves that arrive as NURBS, converted to the linear cage every Hydra
// renderer actually receives.
//
// UsdImaging's NURBS adapter has two faces. Through the legacy path it reports
// a `basisCurves` prim with a linear basis -- its own comment says it is
// "drawing the cage for NURBS curves" -- but through a *scene index* it reports
// the prim's type as `nurbsCurves` and hands over the NURBS data untouched. A
// renderer that supports only `basisCurves` therefore sees curves under one
// path and nothing at all under the other, which is what happened here: the
// stoat's whiskers were absent, no warning was raised, and nothing had gone
// wrong except that Hydra never asked.
//
// OpenUSD ships the conversion as `HdsiNurbsApproximatingSceneIndex`. This
// plugin inserts it for hdClaude, so the renderer sees the same cage the legacy
// path would have produced and needs no NURBS code of its own.

#include "pxr/imaging/hd/sceneIndexPlugin.h"
#include "pxr/pxr.h"

PXR_NAMESPACE_OPEN_SCOPE

class HdClaude_NurbsApproximatingSceneIndexPlugin final
    : public HdSceneIndexPlugin {
  public:
    HdClaude_NurbsApproximatingSceneIndexPlugin();

  protected:
    HdSceneIndexBaseRefPtr _AppendSceneIndex(
        const HdSceneIndexBaseRefPtr& inputScene,
        const HdContainerDataSourceHandle& inputArgs) override;
};

PXR_NAMESPACE_CLOSE_SCOPE
