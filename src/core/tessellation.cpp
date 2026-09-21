#include "hdclaude/core/tessellation.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace hdclaude {

namespace {

/// The eye, from a column-major camera-to-world. GLSL's mat4 layout puts the
/// translation in elements 12, 13, 14, which is what `RenderCamera` carries.
void EyePosition(const TessellationView& view, float eye[3])
{
    eye[0] = view.cameraToWorld[12];
    eye[1] = view.cameraToWorld[13];
    eye[2] = view.cameraToWorld[14];
}

/// One row of a column-major 4x4, as a plane's four coefficients.
///
/// A clip plane is a sum and a difference of rows of the world-to-clip matrix:
/// a point is inside the left plane when `x_clip > -w_clip`, which is
/// `(row3 + row0) . p > 0`, and so on round the six. Written out rather than
/// looped because the signs are the whole content.
struct Plane {
    float a = 0.0f, b = 0.0f, c = 0.0f, d = 0.0f;
};

float Row(const float m[16], int row, int column)
{
    // Column-major: element (row, column) lives at column * 4 + row.
    return m[column * 4 + row];
}

Plane CombineRows(const float m[16], int row, float sign)
{
    Plane plane;
    plane.a = Row(m, 3, 0) + sign * Row(m, row, 0);
    plane.b = Row(m, 3, 1) + sign * Row(m, row, 1);
    plane.c = Row(m, 3, 2) + sign * Row(m, row, 2);
    plane.d = Row(m, 3, 3) + sign * Row(m, row, 3);
    return plane;
}

/// Whether every corner of the box is on the negative side of one plane.
///
/// Tested by the box's *support point* against the plane's normal, which is
/// the corner furthest along it: if even that corner is outside, all eight
/// are, and no cheaper test can say so.
bool BoxIsOutside(const Plane& plane, const float boundsMin[3],
                  const float boundsMax[3])
{
    const float x = plane.a >= 0.0f ? boundsMax[0] : boundsMin[0];
    const float y = plane.b >= 0.0f ? boundsMax[1] : boundsMin[1];
    const float z = plane.c >= 0.0f ? boundsMax[2] : boundsMin[2];
    return plane.a * x + plane.b * y + plane.c * z + plane.d < 0.0f;
}

}  // namespace

bool ShouldSampleView(bool adaptive, bool haveView, bool settingsChanged,
                      bool followCamera)
{
    if (!adaptive) {
        return false;
    }
    return !haveView || settingsChanged || followCamera;
}

float DistanceToBounds(const TessellationView& view, const float boundsMin[3],
                       const float boundsMax[3])
{
    float eye[3];
    EyePosition(view, eye);

    float squared = 0.0f;
    for (int axis = 0; axis < 3; ++axis) {
        const float low = std::min(boundsMin[axis], boundsMax[axis]);
        const float high = std::max(boundsMin[axis], boundsMax[axis]);
        // Zero on the axes the eye is already between, which is what makes an
        // eye inside the box answer zero rather than answering the distance to
        // a corner.
        const float outside =
            std::max(0.0f, std::max(low - eye[axis], eye[axis] - high));
        squared += outside * outside;
    }
    return std::sqrt(squared);
}

bool BoundsAreOffScreen(const TessellationView& view, const float boundsMin[3],
                        const float boundsMax[3])
{
    if (!view.hasClip) {
        // No projection was supplied, so nothing can be shown to be outside
        // it. Answering "visible" is the conservative direction: it refines
        // geometry that might not need it, where the other answer would
        // coarsen geometry that does.
        return false;
    }

    float low[3];
    float high[3];
    for (int axis = 0; axis < 3; ++axis) {
        low[axis] = std::min(boundsMin[axis], boundsMax[axis]);
        high[axis] = std::max(boundsMin[axis], boundsMax[axis]);
    }

    const Plane planes[6] = {
        CombineRows(view.worldToClip, 0, 1.0f),   // left:   w + x > 0
        CombineRows(view.worldToClip, 0, -1.0f),  // right:  w - x > 0
        CombineRows(view.worldToClip, 1, 1.0f),   // bottom: w + y > 0
        CombineRows(view.worldToClip, 1, -1.0f),  // top:    w - y > 0
        CombineRows(view.worldToClip, 2, 1.0f),   // near
        CombineRows(view.worldToClip, 2, -1.0f),  // far
    };
    for (const Plane& plane : planes) {
        if (BoxIsOutside(plane, low, high)) {
            return true;
        }
    }
    return false;
}

float PixelsPerWorldUnit(const TessellationView& view, float distance)
{
    if (view.pixelHeight == 0 || !(view.tanHalfFov > 0.0f) ||
        !(distance > 0.0f)) {
        return 0.0f;
    }
    // The frame spans 2 * distance * tanHalfFov world units vertically at this
    // distance, and pixelHeight pixels.
    return static_cast<float>(view.pixelHeight) /
           (2.0f * distance * view.tanHalfFov);
}

std::size_t RefinedFaceCount(std::size_t coarseFaceCount, int facesPerLevel,
                             int level)
{
    if (coarseFaceCount == 0 || level <= 0) {
        return coarseFaceCount;
    }
    const std::size_t multiplier =
        static_cast<std::size_t>(std::max(facesPerLevel, 1));
    std::size_t count = coarseFaceCount;
    for (int i = 0; i < level; ++i) {
        // Saturating, because the question the caller asks is "does this fit
        // in the budget", and an answer that wrapped round would say yes.
        if (count > std::numeric_limits<std::size_t>::max() / multiplier) {
            return std::numeric_limits<std::size_t>::max();
        }
        count *= multiplier;
    }
    return count;
}

TessellationChoice ChooseTessellation(const TessellationView& view,
                                      const TessellationRequest& request,
                                      const TessellationLimits& limits)
{
    TessellationChoice choice;
    const int ceiling = std::max(limits.maxLevel, 0);
    const int floorLevel = std::clamp(limits.minLevel, 0, ceiling);

    // Nothing to go on: no view, or a mesh that cannot say how big its edges
    // are. The uniform level is what this replaced, so it is what a caller who
    // cannot describe its mesh gets back -- an unrefined mesh would be a
    // silent downgrade where a question was really unanswerable.
    const bool describable = view.valid && request.coarseEdgeLength > 0.0f &&
                             view.tanHalfFov > 0.0f && view.pixelHeight != 0;

    choice.offScreen =
        describable &&
        BoundsAreOffScreen(view, request.boundsMin, request.boundsMax);

    if (!describable) {
        choice.level = ceiling;
        choice.requested = ceiling;
    } else if (choice.offScreen) {
        choice.requested = std::clamp(limits.offScreenLevel, 0, ceiling);
        choice.level = choice.requested;
    } else {
        const float distance =
            DistanceToBounds(view, request.boundsMin, request.boundsMax);
        const float pixels = PixelsPerWorldUnit(view, distance);

        int wanted = ceiling;
        if (pixels > 0.0f) {
            // A coarse edge is `coarseEdgeLength` long and covers
            // `edgePixels` pixels; each level halves it. The level that brings
            // it to the target is log2 of the ratio, rounded up so the answer
            // is at least as fine as asked for rather than at most.
            const float edgePixels = request.coarseEdgeLength * pixels;
            const float target = std::max(limits.targetEdgePixels, 0.01f);
            if (edgePixels <= target) {
                wanted = 0;
            } else {
                wanted = static_cast<int>(
                    std::ceil(std::log2(edgePixels / target) - 1.0e-4f));
            }
        } else {
            // The eye is inside the bounds, so no distance separates them and
            // the projection says nothing. That is the closest a mesh can be,
            // so it gets the ceiling rather than the floor.
            wanted = ceiling;
        }

        choice.requested = std::max(wanted, 0);
        choice.level = std::clamp(choice.requested, floorLevel, ceiling);
        choice.ceilingClamped = choice.requested > ceiling;
    }

    // The budget last, because it overrides every other reason to be fine --
    // including the ceiling, which is a preference, where this is the memory
    // the machine has.
    while (choice.level > 0 &&
           RefinedFaceCount(request.coarseFaceCount, request.facesPerLevel,
                            choice.level) > limits.maxRefinedFaces) {
        --choice.level;
        choice.budgetClamped = true;
    }

    return choice;
}

}  // namespace hdclaude
