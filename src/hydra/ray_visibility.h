// Per-ray-kind visibility of an rprim, from RenderMan's and Karma's primvars.
// See hdclaude/core/ray_visibility.h for what they are and how they combine.
#ifndef HDCLAUDE_HYDRA_RAY_VISIBILITY_H
#define HDCLAUDE_HYDRA_RAY_VISIBILITY_H

#include "pxr/pxr.h"
#include "pxr/usd/sdf/path.h"

#include <cstdint>

PXR_NAMESPACE_OPEN_SCOPE

class HdSceneDelegate;

/// The kinds of ray (`hdclaude::kRay*`) that meet rprim `id`.
///
/// Read from its constant primvars, which carry what an ancestor authored as
/// well as its own (UsdImaging inherits constant primvars down namespace):
/// `ri:attributes:visibility:camera`, `:indirect` and `:transmission`, and
/// `karma:object:rendervisibility`. A kind is hidden if either convention
/// hides it. A value that cannot be read, or a pattern that is not honoured as
/// written, is warned about once per prim and message.
std::uint8_t HdClaudeRayVisibility(HdSceneDelegate* delegate, const SdfPath& id);

PXR_NAMESPACE_CLOSE_SCOPE

#endif  // HDCLAUDE_HYDRA_RAY_VISIBILITY_H
