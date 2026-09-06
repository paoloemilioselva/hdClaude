// The display transform: scene-linear radiance to a display-encoded image.
//
// The renderer's AOV is scene-linear and unbounded, which is the only thing a
// host can composite or grade from. A JPEG is neither, so somewhere between the
// two a decision has to be made about what happens to values above one. That
// decision lives here rather than in the tool that writes the file, so the
// gallery baselines, any future viewer, and the tests all make the same one.
//
// No GPU and no OpenUSD dependency, like the rest of the core.

#ifndef HDCLAUDE_CORE_DISPLAY_H
#define HDCLAUDE_CORE_DISPLAY_H

#include "hdclaude/core/spectrum.h"

namespace hdclaude {

/// Scene-linear sRGB primaries to display-encoded sRGB, in [0, 1].
///
/// Three steps, in this order:
///
///  1. Exposure, in stops. Applied first, so it means what a photographer
///     means by it: a change of scene exposure, before any response curve.
///  2. Neutral highlight compression -- the Khronos PBR Neutral tone mapper.
///     Values below the compression threshold pass through *unchanged*, so a
///     diffuse surface renders at exactly its linear value and a comparison
///     against another renderer is still a comparison of the estimator. Only
///     the highlights roll off, and they desaturate as they do, because a
///     clipped highlight that keeps its hue reads as a coloured hole.
///  3. The sRGB transfer function, from `spectrum.h`. One definition of it.
///
/// Non-finite and negative inputs are sanitised to zero rather than propagated:
/// a NaN in a render is a bug to be found by a test, not something to encode
/// into a baseline image where it becomes a random byte.
Vec3 SceneLinearToDisplaySrgb(const Vec3& linear, float exposureStops = 0.0f);

}  // namespace hdclaude

#endif  // HDCLAUDE_CORE_DISPLAY_H
