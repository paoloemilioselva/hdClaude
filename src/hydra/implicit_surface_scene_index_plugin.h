#pragma once

// Spheres, cubes, cones, cylinders, capsules and planes, turned into meshes.
//
// `UsdGeomSphere` and its siblings reach a render delegate as their own prim
// types -- `sphere`, `cube`, `cone`, `cylinder`, `capsule`, `plane` -- and not
// as meshes. A renderer either intersects them analytically or asks for the
// mesh; hdClaude traces triangles, so it asks.
//
// OpenUSD ships `HdsiImplicitSurfaceSceneIndex` to do the conversion and, as
// with light linking, inserts it for nobody: hdStorm and hdEmbree each insert
// it for themselves. Without it a stage whose only geometry is a
// `UsdGeomSphere` arrives as "0 prototypes, 0 instances" and renders an empty
// frame -- no warning, because nothing was dropped; the prim simply was never
// of a type hdClaude creates.
//
// The scene index can also be configured to keep the implicit type and only
// fix up the transform for the cone, cylinder and capsule spine axis, which is
// what a renderer with analytic quadrics wants. hdClaude takes the mesh for
// every type, since a triangulated quadric is what its acceleration structure
// holds either way.

#include "pxr/imaging/hd/sceneIndexPlugin.h"
#include "pxr/pxr.h"

PXR_NAMESPACE_OPEN_SCOPE

class HdClaude_ImplicitSurfaceSceneIndexPlugin final : public HdSceneIndexPlugin {
  public:
    HdClaude_ImplicitSurfaceSceneIndexPlugin();

  protected:
    HdSceneIndexBaseRefPtr _AppendSceneIndex(
        const HdSceneIndexBaseRefPtr& inputScene,
        const HdContainerDataSourceHandle& inputArgs) override;
};

PXR_NAMESPACE_CLOSE_SCOPE
