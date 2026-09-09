#include "camera.h"

#include <cmath>

PXR_NAMESPACE_OPEN_SCOPE

HdClaudeCamera::HdClaudeCamera(const SdfPath& id) : HdCamera(id) {}
HdClaudeCamera::~HdClaudeCamera() = default;

HdClaudeCameraResult HdClaudeMakeRenderCamera(const GfMatrix4d& worldToView,
                                              const GfMatrix4d& projection)
{
    HdClaudeCameraResult result;

    const GfMatrix4d viewToWorld = worldToView.GetInverse();
    // GfMatrix4d is row-major and GLSL's mat4 is column-major, so the transpose
    // is the conversion, not a handedness fix. USD's camera already looks down
    // -Z, which is what raygen assumes.
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            result.camera.cameraToWorld[column * 4 + row] =
                static_cast<float>(viewToWorld[column][row]);
        }
    }

    // World to clip, in the same transposition and for the same reason. USD
    // composes row-vector transforms left to right, so a world point reaches
    // clip space as p * worldToView * projection.
    const GfMatrix4d worldToClip = worldToView * projection;
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            result.camera.worldToClip[column * 4 + row] =
                static_cast<float>(worldToClip[column][row]);
        }
    }

    // P[3][3] is 0 for a perspective projection and 1 for an orthographic one.
    result.orthographic = std::abs(projection[3][3] - 1.0) < 1e-6;

    const double focalY = projection[1][1];
    const double focalX = projection[0][0];
    if (!result.orthographic && std::abs(focalY) > 1e-9) {
        result.camera.tanHalfFov = static_cast<float>(1.0 / std::abs(focalY));
        if (std::abs(focalX) > 1e-9) {
            result.camera.aspect =
                static_cast<float>(std::abs(focalY) / std::abs(focalX));
        }
    }
    return result;
}

PXR_NAMESPACE_CLOSE_SCOPE
