// A sphere as a Catmull-Clark control cage, with texture coordinates.
//
// `UsdGeomSphere` is not geometry a ray tracer can trace, so something has to
// turn it into one. OpenUSD ships `GeomUtilSphereMeshGenerator` and hdClaude
// used it through `HdsiImplicitSurfaceSceneIndex`, which is a perfectly good
// sphere and has two properties that turned out to matter:
//
//   - its density is `static constexpr size_t numRadial = 10` and the same
//     again for the axial divisions, so a sphere is always ten by ten whatever
//     it is for; and
//   - the primvars it supplies are `points` and nothing else, so a material
//     that samples an image reads one texel for every vertex -- which is how a
//     displacement map on a sphere came to produce a slightly smaller sphere.
//
// This is hdClaude's own, and it differs from OpenUSD's in exactly those two
// respects. The point order, the face order and the winding are deliberately
// *identical*, so that at the default density the cage is the same cage and
// the only new thing is the coordinates.
//
// The coordinates are **face-varying**, and that is not a detail. A sphere's
// longitude wraps, so the vertex where u would be 1 is the same vertex where u
// is 0; a vertex-interpolated coordinate cannot say both, and the seam column
// would run backwards across the whole map instead of wrapping. Face-varying
// values belong to a face's corner rather than to the vertex, which is exactly
// the distinction a seam needs -- and it is the same channel a UV seam on any
// authored mesh refines through.
//
// Plain arithmetic with no Vulkan and no OpenUSD in it, so a sphere can be
// checked against closed forms on any host (docs/architecture.md 3).

#ifndef HDCLAUDE_CORE_SPHERE_MESH_H
#define HDCLAUDE_CORE_SPHERE_MESH_H

#include <cstddef>
#include <vector>

namespace hdclaude {

/// The fewest divisions a sphere can have and still be a sphere. Matching
/// `GeomUtilSphereMeshGenerator::minNumRadial` and `minNumAxial`, because a
/// caller that asks for less should get the same refusal either way.
inline constexpr int kMinSphereRadial = 3;
inline constexpr int kMinSphereAxial = 2;

/// What OpenUSD's generator uses, and therefore what hdClaude defaults to: at
/// this density the cage below is the one hdClaude has always traced.
inline constexpr int kDefaultSphereRadial = 10;
inline constexpr int kDefaultSphereAxial = 10;

/// A control cage: quads around the middle, a triangle fan at each pole.
struct SphereMesh {
    /// Interleaved xyz. The bottom pole is first and the top pole is last,
    /// with `numAxial - 1` rings of `numRadial` points between them.
    std::vector<float> points;
    /// Vertices per face: three for a pole fan's triangles, four for the
    /// quads between them.
    std::vector<int> faceVertexCounts;
    /// Indices into `points`, in the order the counts describe.
    std::vector<int> faceVertexIndices;
    /// Interleaved uv, one per *face vertex* rather than per point, in the
    /// same order as `faceVertexIndices`. See the note above on the seam.
    std::vector<float> faceVaryingUvs;

    std::size_t PointCount() const { return points.size() / 3; }
    std::size_t FaceCount() const { return faceVertexCounts.size(); }
    bool Valid() const { return !faceVertexCounts.empty(); }
};

/// How many points a sphere of this density has: `numRadial * (numAxial - 1)`
/// on the rings, plus the two poles. Zero below the minimum divisions.
std::size_t SphereMeshPointCount(int numRadial, int numAxial);

/// How many faces: one triangle per radial division at each pole, and
/// `numAxial - 2` strips of `numRadial` quads between them.
std::size_t SphereMeshFaceCount(int numRadial, int numAxial);

/// The cage for a sphere of `radius`, centred on the origin, with its poles on
/// Z -- which is where `UsdGeomSphere` puts them.
///
/// Returns an invalid mesh below the minimum divisions rather than a degenerate
/// one, so a caller that clamps and a caller that refuses both have something
/// to key on.
SphereMesh GenerateSphereMesh(int numRadial, int numAxial, double radius);

}  // namespace hdclaude

#endif  // HDCLAUDE_CORE_SPHERE_MESH_H
