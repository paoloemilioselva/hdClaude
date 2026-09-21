// Tessellating one quad of a surface, at a rate that may differ on each side.
//
// Per-mesh refinement gives a whole mesh one level. That is the wrong answer
// for anything that spans a range of distances by itself -- a ground plane, a
// terrain, a floor -- where the near end wants every level it can get and the
// far end wants none.
//
// Per-face refinement gives each face its own rate, and immediately raises the
// problem that makes it hard: two faces refined differently leave T-junctions
// along the edge they share, and a T-junction is a crack. The usual answers
// snap the extra vertices onto the coarser edge, which closes the crack by
// moving the surface.
//
// hdClaude does not do that, and does not have to. Two things together make it
// unnecessary:
//
//   - the rate is a property of an **edge**, not of a face, so the two faces
//     that share an edge compute the same rate for it from the same two
//     endpoints -- there is nothing to disagree about; and
//   - the positions come from **evaluating the limit surface**, not from a
//     refined cage, so two faces sampling their shared edge at the same
//     parameters land on the same points because it is the same curve.
//
// What is left is combinatorial: given four edge rates, cover the unit square
// with triangles whose boundary samples are exactly those rates. That is what
// this does, and it is pure arithmetic on a parametric domain -- no positions,
// no Vulkan, no OpenSubdiv -- so the properties that matter can be asserted
// directly (docs/architecture.md 3).

#ifndef HDCLAUDE_CORE_QUAD_TESSELLATION_H
#define HDCLAUDE_CORE_QUAD_TESSELLATION_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace hdclaude {

/// Samples on the unit quad domain, and the triangles over them.
///
/// The first `BoundarySampleCount()` samples are the boundary, walked from the
/// corner (0, 0) along side 0 and round: a caller stitching this to a
/// neighbour needs to find them, and their order is the contract.
struct QuadTessellation {
    /// Interleaved uv in [0, 1]^2, two floats per sample.
    std::vector<float> uv;
    /// Triangle indices into the samples, three per triangle.
    std::vector<std::uint32_t> indices;
    /// How many leading entries of `uv` are on the boundary.
    std::size_t boundarySamples = 0;

    std::size_t SampleCount() const { return uv.size() / 2; }
    std::size_t TriangleCount() const { return indices.size() / 3; }
    std::size_t BoundarySampleCount() const { return boundarySamples; }
    bool Valid() const { return !indices.empty(); }
};

/// The most segments one side may be cut into.
///
/// Not a quality limit -- the face budget is what limits quality -- but a
/// bound on what one face can ask for before anything has a chance to refuse,
/// the same role `kHdClaudeMaxSubdivisionLevel` plays for a level.
inline constexpr int kMaxEdgeRate = 64;

/// How many segments an edge of `lengthInPixels` should be cut into so that
/// each is about `targetPixels` long.
///
/// Rounded up to a power of two. Not for tidiness: an edge is measured from
/// two positions that move continuously with the camera, and a rate that
/// changed at every distance would change the geometry at every distance.
/// Powers of two mean a rate changes at a handful of thresholds and holds
/// between them, which is also what makes a refinement level and an edge rate
/// the same statement about how fine something is.
///
/// Always at least one: an edge is a segment even when it covers no pixels at
/// all, because a face has to have a boundary.
int EdgeTessellationRate(float lengthInPixels, float targetPixels);

/// Cover the unit square with triangles whose sides are cut at the given
/// rates.
///
/// Side 0 runs from (0, 0) to (1, 0), side 1 from (1, 0) to (1, 1), side 2
/// from (1, 1) to (0, 1), and side 3 from (0, 1) to (0, 0) -- the corners in
/// the order a quad's vertices are given.
///
/// Every rate is clamped to [1, `kMaxEdgeRate`]. The interior is a regular
/// grid at the largest of the four rates, stitched to each side by a walk that
/// advances whichever of the two has the nearer next sample; the corners fall
/// out of that walk rather than being special-cased, because each side's walk
/// begins and ends on the same pair the neighbouring side's does.
QuadTessellation TessellateQuad(const int edgeRates[4]);

}  // namespace hdclaude

#endif  // HDCLAUDE_CORE_QUAD_TESSELLATION_H
