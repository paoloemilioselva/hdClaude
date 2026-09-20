// The surface at each vertex of a mesh prototype.
//
// Shading asks what the surface is doing at a *hit*, and `shade.comp.glsl`
// answers from the triangle that was hit: three corners, three texture
// coordinates, and the derivatives solved from how the two vary together
// across that one triangle. Displacement asks the same question at a *vertex*,
// and a vertex belongs to every triangle around it.
//
// That difference is not cosmetic. A vertex displaced by one answer per
// incident triangle would be displaced to several different places at once,
// and the mesh would come apart along every edge. So the frame is accumulated
// over the triangles that share the vertex and the vertex is displaced once,
// which is what keeps a displaced surface watertight.
//
// Computed on the host, like the subdivision it follows (docs/architecture.md
// 7): it is geometry, not shading, and the commitment that displacement is
// evaluated by the generated MaterialX program on the GPU is about the
// *material*, not about the frame the material is handed.

#ifndef HDCLAUDE_GPU_VERTEX_FRAMES_H
#define HDCLAUDE_GPU_VERTEX_FRAMES_H

#include <cstddef>
#include <vector>

#include "hdclaude/gpu/scene.h"

namespace hdclaude {

/// One entry per vertex: the surface, and how it moves.
///
/// The derivatives are left *unnormalised* and are not orthogonalised against
/// the normal. That is deliberate: `shade.comp.glsl` takes its tangent frame
/// from dP/du and dP/dv by orthogonalising against the shading normal and
/// reading the handedness off dP/dv, and a displacement pass that normalised
/// early would hand the material a frame built a different way from the one
/// the same material sees when it is shaded. The two have to agree, so the
/// arithmetic is done in one place -- the kernel -- from the same inputs.
struct VertexFrames {
    /// Interleaved xyz shading normals, three floats per vertex.
    std::vector<float> normals;
    /// Interleaved xyz dP/du, three floats per vertex.
    std::vector<float> dpdu;
    /// Interleaved xyz dP/dv, three floats per vertex.
    std::vector<float> dpdv;
    /// Interleaved uv, two floats per vertex. Empty when the prototype carries
    /// no texture coordinates, in which case nothing can be said about one
    /// vertex's coordinate and the pass reads zero.
    std::vector<float> uvs;

    std::size_t VertexCount() const { return normals.size() / 3; }
    bool Empty() const { return normals.empty(); }
};

/// The frames for `prototype`, in object space, one per vertex.
///
/// Empty for a curve prototype: a curve has no vertices in this sense and no
/// surface parameterisation, and displacing one is a separate question from
/// displacing a mesh.
///
/// Three things are worth knowing about what this returns.
///
/// **The normal is per vertex even when the prototype's is not.** A
/// face-varying normal array carries one normal per triangle corner, which is
/// how a hard edge is authored, and the two sides of a crease disagree by
/// construction. A vertex has one position and can only be displaced to one
/// place, so the corners' normals are summed there. A crease displaced this
/// way moves along the average of its two faces, which is the only direction
/// that keeps both faces attached to it.
///
/// **A vertex on a UV seam keeps the first coordinate authored for it**, in
/// index order, rather than an average. The two sides of a seam are typically
/// at opposite ends of the map -- u = 0 against u = 1 -- so their mean is a
/// coordinate that appears nowhere on the surface and would read a texel from
/// the middle of the image. The first is arbitrary between two authored
/// answers; the mean is not one of the authored answers at all.
///
/// **Each triangle's contribution is weighted by twice its area**, which is
/// the length of the cross product that already has to be computed, so a
/// sliver does not pull a vertex it barely touches.
VertexFrames ComputeVertexFrames(const MeshPrototype& prototype);

}  // namespace hdclaude

#endif  // HDCLAUDE_GPU_VERTEX_FRAMES_H
