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

#include <cstddef>
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

/// How a curve primvar's values are laid out over a curve set, as Hydra's
/// interpolation tokens say. UsdGeomBasisCurves defines all four for curves,
/// and `Varying` is the one that does not follow the vertices: on a cubic
/// curve it is one value per segment end, interpolated linearly across each
/// segment, and its count follows the segment-count table rather than the
/// vertex count ("Primvar Interpolation" in UsdGeomBasisCurves).
enum class CurveInterpolation {
    Constant,
    Uniform,
    Varying,
    Vertex,
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
    /// Interleaved xyz, one per point, when the set authors normals, and empty
    /// when it does not. Unnormalised: an interpolated normal is a direction
    /// only once `RibbonCurves` has made it perpendicular to the curve.
    std::vector<float> normals;

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
/// Curves as segments, for a renderer that intersects them directly.
///
/// The alternative to `SweepCurves`, and the whole difference is what a ray is
/// asked to hit. A swept tube is `sides` facets a span, so a span costs about
/// five hundred bytes and approximates a circle; a segment is two positions and
/// two radii, thirty-two bytes, and *is* the circle. On ALab's stoat and Remi
/// that is 122 million triangles against 7.5 million segments.
///
/// Ten floats a segment: start xyz, start radius, start v, end xyz, end radius,
/// end v. The radius is half the width, because a width is a diameter -- the
/// one conversion in here, and the one worth saying out loud. The `v` is how
/// far along the whole strand that end is, which is the same parameter
/// `SweepCurves` writes as its texture coordinate, so a material reads the same
/// gradient whichever way the curve is drawn.
///
/// `widths` is read the same way `SweepCurves` reads it: empty, one for the
/// whole set, one per curve, or one per point, decided by how many there are.
/// `periodic` closes each curve back onto its first point.
std::vector<float> CurveSegments(const std::vector<int>& vertexCounts,
                                 const std::vector<float>& points,
                                 const std::vector<float>& widths,
                                 float fallbackWidth, bool periodic,
                                 std::string* reason);

CurvePolylines EvaluateCurves(const std::vector<int>& vertexCounts,
                              const std::vector<float>& points,
                              const std::vector<float>& widths,
                              CurveBasis basis, bool periodic,
                              int samplesPerSegment, std::string* reason);

/// The same, carrying the set's authored normals through the evaluation.
///
/// `normals` is interleaved xyz laid out as `normalsInterpolation` says, and
/// its count must be the one UsdGeomBasisCurves gives that interpolation:
/// one, one per curve, one per vertex, or the varying count (a nonperiodic
/// curve's segments plus one, a periodic curve's segments). Any other count is
/// refused by name. Vertex normals are evaluated through the basis exactly as
/// the positions are; varying ones are interpolated linearly across each
/// segment; constant and uniform ones are repeated. An empty `normals` is a
/// set with none, and leaves `normals` empty in the result.
CurvePolylines EvaluateCurves(const std::vector<int>& vertexCounts,
                              const std::vector<float>& points,
                              const std::vector<float>& widths,
                              const std::vector<float>& normals,
                              CurveInterpolation normalsInterpolation,
                              CurveBasis basis, bool periodic,
                              int samplesPerSegment, std::string* reason);

/// Curves as ribbons: what UsdGeomBasisCurves draws when a set authors
/// normals ("Tubes and Ribbons": curves with widths and normals are "imaged as
/// ribbons oriented in the direction of the interpolated normal vectors", and
/// their widths are the ribbon's width rather than a tube's diameter).
///
/// Each point of each polyline becomes two vertices, half the width either
/// side of the curve along `cross(N, T)` -- where T is the curve's tangent and
/// N its normal made perpendicular to it -- so the ribbon lies across the
/// curve and faces N. Consecutive points are joined by two triangles, the
/// strip Storm draws, and each vertex carries N as its shading normal and
/// (u across, v along) as its texture coordinate, u running 0 to 1 the way
/// `SweepCurves`' u runs around a tube.
///
/// A point whose orientation is undefined -- a normal that is zero or parallel
/// to the curve, or a curve with no direction there -- is given no width: the
/// ribbon narrows to nothing rather than taking an orientation the asset did
/// not author. How many there were is returned in `unoriented` so the caller
/// can report them against the prim.
///
/// `normals` is per point, as `EvaluateCurves` returns them; `widths` is read
/// as `SweepCurves` reads it. Returns an empty mesh, and sets `reason`, when
/// the input cannot be built.
CurveMesh RibbonCurves(const std::vector<int>& vertexCounts,
                       const std::vector<float>& points,
                       const std::vector<float>& widths,
                       const std::vector<float>& normals, float fallbackWidth,
                       bool periodic, std::size_t* unoriented,
                       std::string* reason);

}  // namespace hdclaude
