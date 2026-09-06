#pragma once

// Uniform subdivision of a mesh, through OpenSubdiv.
//
// hdClaude subdivides on the CPU and traces the refined cage. That is a
// deliberate choice rather than a limitation of the integrator: a ray tracer
// needs an explicit surface to build an acceleration structure over, and the
// alternatives -- evaluating limit patches at intersection time, or feature-
// adaptive tessellation with displacement -- are a different project that only
// pays off once displacement exists. The choice and its cost are recorded in
// docs/roadmap.md.

#include "pxr/imaging/hd/meshTopology.h"
#include "pxr/pxr.h"

#include <cstdint>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

/// A refined mesh: triangles, and where each came from.
struct HdClaudeRefinedMesh {
    /// Interleaved xyz, three floats per refined vertex.
    std::vector<float> positions;
    /// Interleaved uv; empty when the control cage had none.
    ///
    /// Two floats per refined vertex, or -- when the control cage's
    /// coordinates were face-varying -- two per refined triangle *corner*,
    /// which `uvsPerCorner` says.
    std::vector<float> uvs;
    bool uvsPerCorner = false;
    /// Triangle indices into `positions`.
    std::vector<std::uint32_t> indices;
    /// The *coarse* face each triangle descends from, so a GeomSubset authored
    /// on the control cage still selects the right refined triangles.
    std::vector<int> coarseFaces;

    bool Valid() const { return !indices.empty(); }
};

/// True if this topology asks to be subdivided at all.
///
/// A scheme of "none" is a polygon mesh that happens to carry subdivision
/// tags, and refining it would round off corners the asset intends to keep.
bool HdClaudeWantsSubdivision(const HdMeshTopology& topology);

/// Refine `topology` uniformly to `level` and triangulate the result.
///
/// `points` is the control cage, interleaved xyz. `uvs`, when not empty, is one
/// vertex-interpolated texture coordinate per control vertex and is refined
/// alongside the positions -- a refined mesh that dropped them would have no
/// texture coordinates at all, and the shading kernel would fall back to
/// barycentrics, which samples a texture at random across every tiny triangle
/// and averages to a flat colour.
///
/// Returns an invalid result if the topology cannot be refined, which the
/// caller should treat as "render the control cage" rather than as an error: a
/// mesh that fails to subdivide should still appear.
/// `faceVaryingUvs`, when not empty, is one coordinate per *face vertex* of the
/// control cage, refined through an OpenSubdiv face-varying channel. That is
/// the only way a UV seam survives refinement: interpolating those coordinates
/// as vertex data would weld the seam shut and smear the texture across it.
/// A mesh may supply one or the other, not both.
HdClaudeRefinedMesh HdClaudeSubdivide(const HdMeshTopology& topology,
                                      const std::vector<float>& points,
                                      int level,
                                      const std::vector<float>& uvs = {},
                                      const std::vector<float>& faceVaryingUvs = {});

PXR_NAMESPACE_CLOSE_SCOPE
