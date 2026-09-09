#include "hdclaude/core/curve_sweep.h"

#include <algorithm>
#include <cmath>

namespace hdclaude {
namespace {

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

Vec3 Add(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 Subtract(const Vec3& a, const Vec3& b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
Vec3 Scale(const Vec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
Vec3 Cross(const Vec3& a, const Vec3& b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
float Length(const Vec3& a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }

Vec3 At(const std::vector<float>& points, std::size_t index)
{
    return {points[index * 3 + 0], points[index * 3 + 1], points[index * 3 + 2]};
}

/// A unit vector not parallel to `direction`.
///
/// Built from the axis the direction leans on least, which is the one
/// construction that cannot degenerate: whichever component is smallest is the
/// axis the direction is furthest from being parallel to.
Vec3 Perpendicular(const Vec3& direction)
{
    const float ax = std::abs(direction.x);
    const float ay = std::abs(direction.y);
    const float az = std::abs(direction.z);
    Vec3 axis{0.0f, 0.0f, 1.0f};
    if (ax <= ay && ax <= az) {
        axis = {1.0f, 0.0f, 0.0f};
    } else if (ay <= az) {
        axis = {0.0f, 1.0f, 0.0f};
    }
    const Vec3 perpendicular = Cross(direction, axis);
    const float length = Length(perpendicular);
    if (length < 1e-12f) {
        return {1.0f, 0.0f, 0.0f};
    }
    return Scale(perpendicular, 1.0f / length);
}

}  // namespace

CurveMesh SweepCurves(const std::vector<int>& vertexCounts,
                      const std::vector<float>& points,
                      const std::vector<float>& widths, float fallbackWidth,
                      int sides, bool periodic, std::string* reason)
{
    CurveMesh mesh;
    const auto fail = [&](const char* text) {
        if (reason != nullptr) {
            *reason = text;
        }
        return mesh;
    };

    if (points.size() < 6 || points.size() % 3 != 0) {
        return fail("fewer than two points, or a points array that is not whole");
    }
    if (vertexCounts.empty()) {
        return fail("no curve vertex counts");
    }
    sides = std::max(3, sides);

    const std::size_t pointCount = points.size() / 3;
    std::size_t total = 0;
    for (const int count : vertexCounts) {
        if (count < 2) {
            return fail("a curve with fewer than two vertices");
        }
        total += static_cast<std::size_t>(count);
    }
    if (total != pointCount) {
        return fail("vertex counts that do not add up to the point count");
    }

    CurveWidths widthKind = CurveWidths::Constant;
    if (widths.size() == pointCount) {
        widthKind = CurveWidths::PerPoint;
    } else if (widths.size() == vertexCounts.size()) {
        widthKind = CurveWidths::PerCurve;
    } else if (widths.size() > 1) {
        return fail("a widths array matching neither the points nor the curves");
    }

    constexpr float kPi = 3.14159265358979323846f;
    const auto stride = static_cast<std::uint32_t>(sides + 1);

    std::size_t first = 0;
    for (std::size_t curve = 0; curve < vertexCounts.size(); ++curve) {
        const auto vertices = static_cast<std::size_t>(vertexCounts[curve]);
        const std::size_t rings = periodic ? vertices + 1 : vertices;
        const auto ringBase = static_cast<std::uint32_t>(mesh.positions.size() / 3);

        // The frame is carried along the curve rather than rebuilt at each
        // point. Rebuilding it from a fixed axis makes the tube spin wherever
        // the tangent passes near that axis, which shows as a twist in the
        // shading and a crease in the silhouette; carrying the previous frame
        // and re-orthogonalising it is parallel transport, and costs one cross
        // product a vertex.
        Vec3 reference;
        bool haveReference = false;

        for (std::size_t ring = 0; ring < rings; ++ring) {
            const std::size_t index = first + (ring % vertices);
            const Vec3 centre = At(points, index);

            Vec3 tangent{0.0f, 0.0f, 0.0f};
            const bool hasPrevious = periodic || ring > 0;
            const bool hasNext = periodic || ring + 1 < vertices;
            if (hasPrevious) {
                const std::size_t previous =
                    first + ((ring + vertices - 1) % vertices);
                tangent = Add(tangent, Subtract(centre, At(points, previous)));
            }
            if (hasNext) {
                const std::size_t next = first + ((ring + 1) % vertices);
                tangent = Add(tangent, Subtract(At(points, next), centre));
            }
            float length = Length(tangent);
            if (length < 1e-12f) {
                // Coincident neighbours leave no direction to sweep along. The
                // ring is still emitted, so the strand keeps its vertex count
                // and its indices stay valid; it is simply flat there.
                tangent = {0.0f, 0.0f, 1.0f};
                length = 1.0f;
            }
            tangent = Scale(tangent, 1.0f / length);

            if (!haveReference) {
                reference = Perpendicular(tangent);
                haveReference = true;
            }
            Vec3 bitangent = Cross(tangent, reference);
            float bitangentLength = Length(bitangent);
            if (bitangentLength < 1e-6f) {
                reference = Perpendicular(tangent);
                bitangent = Cross(tangent, reference);
                bitangentLength = Length(bitangent);
                if (bitangentLength < 1e-12f) {
                    bitangent = {0.0f, 1.0f, 0.0f};
                    bitangentLength = 1.0f;
                }
            }
            bitangent = Scale(bitangent, 1.0f / bitangentLength);
            const Vec3 across = Cross(bitangent, tangent);
            const float acrossLength = std::max(1e-12f, Length(across));
            reference = Scale(across, 1.0f / acrossLength);

            float width = fallbackWidth;
            if (widthKind == CurveWidths::PerPoint) {
                width = widths[index];
            } else if (widthKind == CurveWidths::PerCurve) {
                width = widths[curve];
            } else if (widths.size() == 1) {
                width = widths[0];
            }
            const float radius = std::max(0.0f, width) * 0.5f;

            const float v = rings > 1 ? static_cast<float>(ring) /
                                            static_cast<float>(rings - 1)
                                      : 0.0f;
            // The ring carries one extra vertex so the seam closes in *texture*
            // space: two copies at the same position with u of 0 and 1, or the
            // last face samples the whole map backwards.
            for (int side = 0; side <= sides; ++side) {
                const float u =
                    static_cast<float>(side) / static_cast<float>(sides);
                const float angle = u * 2.0f * kPi;
                const Vec3 normal =
                    Add(Scale(reference, std::cos(angle)),
                        Scale(bitangent, std::sin(angle)));
                const Vec3 position = Add(centre, Scale(normal, radius));
                mesh.positions.push_back(position.x);
                mesh.positions.push_back(position.y);
                mesh.positions.push_back(position.z);
                mesh.normals.push_back(normal.x);
                mesh.normals.push_back(normal.y);
                mesh.normals.push_back(normal.z);
                mesh.uvs.push_back(u);
                mesh.uvs.push_back(v);
            }
        }

        for (std::size_t ring = 0; ring + 1 < rings; ++ring) {
            const std::uint32_t a =
                ringBase + static_cast<std::uint32_t>(ring) * stride;
            const std::uint32_t b = a + stride;
            for (int side = 0; side < sides; ++side) {
                const auto s = static_cast<std::uint32_t>(side);
                mesh.indices.push_back(a + s);
                mesh.indices.push_back(b + s);
                mesh.indices.push_back(b + s + 1);
                mesh.indices.push_back(a + s);
                mesh.indices.push_back(b + s + 1);
                mesh.indices.push_back(a + s + 1);
            }
        }

        first += vertices;
    }

    if (mesh.indices.empty()) {
        return fail("no triangles, which means every curve was degenerate");
    }
    return mesh;
}

}  // namespace hdclaude
