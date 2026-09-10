#pragma once

// Curves as swept tubes.
//
// A ray tracer needs an explicit surface to build an acceleration structure
// over. The alternative -- procedural AABB geometry with the swept cone
// intersected in the traversal kernel -- is exact, and is what a hair renderer
// eventually wants, but it means teaching `extend` to handle candidate
// intersections and adding a binding for the curve data. Sweeping a tube reuses
// the triangle path whole: the same acceleration structure, the same shading,
// and the same descriptor set layout.
//
// The cost of that is honest and bounded. A tube of `sides` faces approximates
// a circular sweep exactly as a subdivided mesh approximates a limit surface,
// and converges the same way, which is why the count is a setting for the same
// reason the subdivision level is one.
//
// No USD here, and no Vulkan: this is arithmetic on flat arrays, so it is
// testable without a device or a stage.

#include <cstdint>
#include <string>
#include <vector>

namespace hdclaude {

/// A tessellated curve set.
struct CurveMesh {
    /// Interleaved xyz, three floats per vertex.
    std::vector<float> positions;
    /// Interleaved xyz, one per vertex, pointing out of the tube.
    std::vector<float> normals;
    /// Interleaved uv, two per vertex: u around the tube, v along the curve.
    std::vector<float> uvs;
    /// Triangle indices into `positions`.
    std::vector<std::uint32_t> indices;

    bool Valid() const { return !indices.empty(); }
};

/// How a curve set's widths are laid out, decided by how many there are.
enum class CurveWidths {
    /// One width for the whole set, or none at all.
    Constant,
    /// One per curve.
    PerCurve,
    /// One per point.
    PerPoint,
};

/// Sweep a tube along every curve.
///
/// `vertexCounts` is the number of points in each curve and must add up to the
/// number of points. `points` is interleaved xyz. `widths` may be empty, hold
/// one value, one per curve, or one per point; the size decides which, because
/// that is what the interpolation means. `fallbackWidth` is used when `widths`
/// is empty.
///
/// `periodic` closes each curve back onto its first point. `sides` is how many
/// faces the cross-section has and is clamped to at least three.
///
/// Returns an empty mesh, and sets `reason`, when the input cannot be swept --
/// so a caller can report the failure against the prim it came from rather than
/// draw nothing and stay silent.
CurveMesh SweepCurves(const std::vector<int>& vertexCounts,
                      const std::vector<float>& points,
                      const std::vector<float>& widths, float fallbackWidth,
                      int sides, bool periodic, std::string* reason);

// --- Cubic bases -----------------------------------------------------------

/// The interpolation a curve set's vertices are authored for.
///
/// `Linear` needs no evaluation: its control points *are* the polyline. The
/// three cubic bases are the ones UsdGeomBasisCurves names, and each carries a
/// `vstep` -- the stride from one segment's first control point to the next's --
/// which is what decides how many segments a vertex count describes.
enum class CurveBasis {
    Linear,
    BSpline,
    CatmullRom,
    Bezier,
};

/// A curve set reduced to polylines, ready for `SweepCurves`.
struct CurvePolylines {
    std::vector<int> vertexCounts;
    /// Interleaved xyz.
    std::vector<float> points;
    /// Per point when the input had per-point widths, otherwise carried through
    /// unchanged: a constant or per-curve width means the same thing before and
    /// after evaluation, and only a per-vertex one has to be evaluated with the
    /// positions.
    std::vector<float> widths;

    bool Valid() const { return !vertexCounts.empty(); }
};

/// Evaluate a cubic curve set into polylines.
///
/// The segment rules are UsdGeomBasisCurves', not this renderer's: a segment is
/// four consecutive control points, the stride between segments is the basis's
/// `vstep` (3 for Bezier, 1 for the other two), and a nonperiodic curve of `n`
/// vertices therefore has `(n - 4) / vstep + 1` segments while a periodic one
/// has `n / vstep`. A vertex count that does not satisfy the basis's divisibility
/// rule is refused by name rather than rounded down to one that does, because a
/// curve set that does not describe whole segments is an authoring error and
/// silently dropping its tail would hide it.
///
/// `samplesPerSegment` is how many straight spans each cubic segment becomes,
/// clamped to at least one. One is not a degenerate choice: it evaluates the
/// basis at both ends of every segment, which already differs from the control
/// cage -- a B-spline passes through none of its control points -- and for hair
/// authored at a few segments per strand it is often enough. It is a setting for
/// the same reason `sides` is.
///
/// A `Linear` basis is returned unchanged, so a caller may hand every curve set
/// through this without asking which it has.
///
/// Returns an empty result, and sets `reason`, when the input cannot be
/// evaluated.
CurvePolylines EvaluateCurves(const std::vector<int>& vertexCounts,
                              const std::vector<float>& points,
                              const std::vector<float>& widths,
                              CurveBasis basis, bool periodic,
                              int samplesPerSegment, std::string* reason);

}  // namespace hdclaude
