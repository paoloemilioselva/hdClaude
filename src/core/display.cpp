#include "hdclaude/core/display.h"

#include <algorithm>
#include <cmath>

namespace hdclaude {
namespace {

/// Where highlight compression begins. Below this the transform is the
/// identity, which is the property that makes the baselines comparable to
/// another renderer's linear output.
constexpr float kCompressionStart = 0.8f;

/// How much a compressed highlight desaturates on its way to white.
constexpr float kDesaturation = 0.15f;

float Sanitise(float value)
{
    return std::isfinite(value) ? std::max(value, 0.0f) : 0.0f;
}

/// The Khronos PBR Neutral tone mapper.
///
/// Chosen over a filmic curve because the gallery is a comparison instrument,
/// not a look: it leaves the diffuse range alone, so a difference between two
/// baselines is a difference in the render rather than in the response curve.
/// The dark-end offset is part of the published transform; it makes the curve
/// meet zero without the toe that would lift black.
Vec3 CompressHighlights(Vec3 colour)
{
    const float darkest = std::min({colour.x, colour.y, colour.z});
    const float offset =
        darkest < 0.08f ? darkest - 6.25f * darkest * darkest : 0.04f;
    colour.x -= offset;
    colour.y -= offset;
    colour.z -= offset;

    const float peak = std::max({colour.x, colour.y, colour.z});
    if (peak < kCompressionStart) {
        return colour;
    }

    // A hyperbola that reaches one only in the limit, so no finite radiance
    // ever clips to a flat white.
    const float distance = 1.0f - kCompressionStart;
    const float compressed =
        1.0f - distance * distance / (peak + distance - kCompressionStart);
    const float scale = compressed / peak;

    // The brighter the highlight, the closer it moves to neutral. Without this
    // an over-range saturated light keeps its hue at full chroma and reads as a
    // coloured hole rather than as something too bright to see.
    const float toWhite = 1.0f - 1.0f / (kDesaturation * (peak - compressed) + 1.0f);

    colour.x = std::lerp(colour.x * scale, compressed, toWhite);
    colour.y = std::lerp(colour.y * scale, compressed, toWhite);
    colour.z = std::lerp(colour.z * scale, compressed, toWhite);
    return colour;
}

}  // namespace

Vec3 SceneLinearToDisplaySrgb(const Vec3& linear, float exposureStops)
{
    const float stops = std::isfinite(exposureStops)
                            ? std::clamp(exposureStops, -20.0f, 20.0f)
                            : 0.0f;
    const float exposure = std::exp2(stops);

    Vec3 colour{Sanitise(linear.x) * exposure, Sanitise(linear.y) * exposure,
                Sanitise(linear.z) * exposure};
    colour = CompressHighlights(colour);

    return {LinearToSrgb(std::clamp(colour.x, 0.0f, 1.0f)),
            LinearToSrgb(std::clamp(colour.y, 0.0f, 1.0f)),
            LinearToSrgb(std::clamp(colour.z, 0.0f, 1.0f))};
}

}  // namespace hdclaude
