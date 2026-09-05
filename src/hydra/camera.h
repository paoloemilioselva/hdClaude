#pragma once

#include "hdclaude/gpu/path_tracer.h"

#include "pxr/base/gf/matrix4d.h"
#include "pxr/imaging/hd/camera.h"

PXR_NAMESPACE_OPEN_SCOPE

/// The stock HdCamera is sufficient; this exists so the delegate has a type to
/// create and so camera prims participate in Sync like everything else.
class HdClaudeCamera final : public HdCamera {
  public:
    explicit HdClaudeCamera(const SdfPath& id);
    ~HdClaudeCamera() override;
};

/// Build a render camera from the matrices a render pass is given.
///
/// The pass state -- not the HdCamera prim -- is the authority here. usdview's
/// free camera is delivered as matrices with no prim behind it, and even when a
/// prim exists the pass state carries the framing and conform policy already
/// applied. Reading the prim instead would render a different view from the one
/// the viewport is showing.
///
/// A standard perspective projection recovers exactly, with no ambiguity about
/// which aperture was authored:
///
///     tan(fovY/2) = 1 / P[1][1]        aspect = P[1][1] / P[0][0]
///
/// An orthographic projection has P[3][3] == 1 and is *not* representable by a
/// pinhole; the caller is told rather than silently given a perspective view of
/// the scene.
struct HdClaudeCameraResult {
    hdclaude::RenderCamera camera;
    bool orthographic = false;
};

HdClaudeCameraResult HdClaudeMakeRenderCamera(const GfMatrix4d& worldToView,
                                              const GfMatrix4d& projection);

PXR_NAMESPACE_CLOSE_SCOPE
