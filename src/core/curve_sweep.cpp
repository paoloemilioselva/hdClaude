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

// --- Cubic bases -----------------------------------------------------------

namespace {

/// The stride from one segment's first control point to the next's.
///
/// UsdGeomBasisCurves' table: Bezier 3, bspline 1, catmullRom 1. It is what
/// makes a Bezier curve of seven vertices two segments where a B-spline of
/// seven is four.
int VertexStep(CurveBasis basis)
{
    return basis == CurveBasis::Bezier ? 3 : 1;
}

/// The four basis weights at `t` in [0, 1], in control-point order.
///
/// Each set sums to one at every t, which is what makes the result an affine
/// combination of the control points and so independent of where the origin is.
void BasisWeights(CurveBasis basis, double t, double weight[4])
{
    const double t2 = t * t;
    const double t3 = t2 * t;
    switch (basis) {
        case CurveBasis::BSpline:
            // The uniform cubic B-spline. It interpolates none of its control
            // points -- it starts at (P0 + 4 P1 + P2) / 6 -- and that is the
            // basis's defining behaviour rather than an error to correct: hair
            // authored as a B-spline is authored expecting it.
            weight[0] = (-t3 + 3.0 * t2 - 3.0 * t + 1.0) / 6.0;
            weight[1] = (3.0 * t3 - 6.0 * t2 + 4.0) / 6.0;
            weight[2] = (-3.0 * t3 + 3.0 * t2 + 3.0 * t + 1.0) / 6.0;
            weight[3] = t3 / 6.0;
            return;
        case CurveBasis::CatmullRom:
            // Passes through P1 at t = 0 and P2 at t = 1, which is what makes a
            // Catmull-Rom curve go through its control points and a B-spline
            // not.
            weight[0] = 0.5 * (-t3 + 2.0 * t2 - t);
            weight[1] = 0.5 * (3.0 * t3 - 5.0 * t2 + 2.0);
            weight[2] = 0.5 * (-3.0 * t3 + 4.0 * t2 + t);
            weight[3] = 0.5 * (t3 - t2);
            return;
        case CurveBasis::Bezier: {
            const double u = 1.0 - t;
            weight[0] = u * u * u;
            weight[1] = 3.0 * t * u * u;
            weight[2] = 3.0 * t2 * u;
            weight[3] = t3;
            return;
        }
        case CurveBasis::Linear:
            break;
    }
    weight[0] = 0.0;
    weight[1] = 1.0 - t;
    weight[2] = t;
    weight[3] = 0.0;
}

const char* BasisName(CurveBasis basis)
{
    switch (basis) {
        case CurveBasis::Linear: return "linear";
        case CurveBasis::BSpline: return "bspline";
        case CurveBasis::CatmullRom: return "catmullRom";
        case CurveBasis::Bezier: return "bezier";
    }
    return "unknown";
}

}  // namespace

CurvePolylines EvaluateCurves(const std::vector<int>& vertexCounts,
                              const std::vector<float>& points,
                              const std::vector<float>& widths,
                              CurveBasis basis, bool periodic,
                              int samplesPerSegment, std::string* reason)
{
    const auto fail = [&](const std::string& what) {
        if (reason != nullptr) {
            *reason = what;
        }
        return CurvePolylines{};
    };

    if (vertexCounts.empty()) {
        return fail("no curves");
    }
    if (points.size() % 3 != 0) {
        return fail("point array is not a whole number of xyz triples");
    }
    const std::size_t pointCount = points.size() / 3;

    std::size_t declared = 0;
    for (const int count : vertexCounts) {
        if (count < 0) {
            return fail("a curve has a negative vertex count");
        }
        declared += static_cast<std::size_t>(count);
    }
    if (declared != pointCount) {
        return fail("vertex counts describe " + std::to_string(declared) +
                    " points but " + std::to_string(pointCount) + " were given");
    }

    // A linear set is already a polyline. Returned rather than refused so a
    // caller can hand every set through without first asking which basis it is.
    if (basis == CurveBasis::Linear) {
        CurvePolylines through;
        through.vertexCounts = vertexCounts;
        through.points = points;
        through.widths = widths;
        return through;
    }

    // Per-point widths are the only layout that has to be evaluated: a constant
    // or a per-curve width means the same thing before and after.
    const bool widthsPerPoint = widths.size() == pointCount;

    const int step = VertexStep(basis);
    const int samples = samplesPerSegment > 1 ? samplesPerSegment : 1;

    CurvePolylines result;
    result.vertexCounts.reserve(vertexCounts.size());

    std::size_t base = 0;
    for (std::size_t curve = 0; curve < vertexCounts.size(); ++curve) {
        const int count = vertexCounts[curve];

        // UsdGeomBasisCurves' validity rules, applied as written. A count that
        // does not describe whole segments is reported rather than truncated.
        int segments = 0;
        if (periodic) {
            if (count < 4 || count % step != 0) {
                return fail(std::string("curve ") + std::to_string(curve) +
                            " has " + std::to_string(count) +
                            " vertices, which is not a whole number of " +
                            BasisName(basis) + " segments for a periodic curve");
            }
            segments = count / step;
        } else {
            if (count < 4 || (count - 4) % step != 0) {
                return fail(std::string("curve ") + std::to_string(curve) +
                            " has " + std::to_string(count) +
                            " vertices, which is not a whole number of " +
                            BasisName(basis) + " segments");
            }
            segments = (count - 4) / step + 1;
        }

        // A nonperiodic curve carries the last sample of its last segment; a
        // periodic one does not, because that sample is its first point again
        // and `SweepCurves` closes the ring itself.
        const int emitted =
            periodic ? segments * samples : segments * samples + 1;
        result.vertexCounts.push_back(emitted);

        for (int segment = 0; segment < segments; ++segment) {
            const int last =
                (!periodic && segment + 1 == segments) ? samples : samples - 1;
            for (int sample = 0; sample <= last; ++sample) {
                const double t =
                    static_cast<double>(sample) / static_cast<double>(samples);
                double weight[4];
                BasisWeights(basis, t, weight);

                double x = 0.0;
                double y = 0.0;
                double z = 0.0;
                double width = 0.0;
                for (int control = 0; control < 4; ++control) {
                    std::size_t index =
                        static_cast<std::size_t>(segment * step + control);
                    // Only a periodic curve wraps; a nonperiodic one cannot
                    // reach past its last vertex, which the segment count above
                    // already guarantees.
                    if (periodic) {
                        index %= static_cast<std::size_t>(count);
                    }
                    const std::size_t at = base + index;
                    x += weight[control] * points[at * 3 + 0];
                    y += weight[control] * points[at * 3 + 1];
                    z += weight[control] * points[at * 3 + 2];
                    if (widthsPerPoint) {
                        width += weight[control] * widths[at];
                    }
                }

                result.points.push_back(static_cast<float>(x));
                result.points.push_back(static_cast<float>(y));
                result.points.push_back(static_cast<float>(z));
                if (widthsPerPoint) {
                    // A width is a distance and cannot be negative. A
                    // Catmull-Rom or B-spline combination can undershoot below
                    // zero where authored widths change sharply, and a negative
                    // radius sweeps the tube inside out.
                    result.widths.push_back(
                        static_cast<float>(width > 0.0 ? width : 0.0));
                }
            }
        }
        base += static_cast<std::size_t>(count);
    }

    if (!widthsPerPoint) {
        result.widths = widths;
    }
    return result;
}

}  // namespace hdclaude
