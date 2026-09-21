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

/// How many faces the control cage has.
///
/// The count a refinement budget is reckoned against: every scheme hdClaude
/// refines multiplies it by four a level, so this and the level are the whole
/// of what a refined mesh will cost in faces.
std::size_t HdClaudeCoarseFaceCount(const HdMeshTopology& topology);

/// The mean length of a control-cage edge, in the mesh's own space.
///
/// The mean rather than the longest: a refinement level is a property of the
/// mesh as a whole, and one long edge across an otherwise fine cage would pull
/// every face of it up a level that only that edge needed. Zero for a topology
/// with no edges, which the caller should read as "this mesh cannot say how
/// big it is" rather than as "this mesh is small".
float HdClaudeMeanEdgeLength(const HdMeshTopology& topology,
                             const std::vector<float>& points);

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

/// One corner of a ptex face, named so that two faces sharing it agree.
///
/// A quad's ptex face is the face itself and its corners are coarse vertices.
/// Any other face is split into one ptex face per corner, whose corners are
/// then a coarse vertex, two coarse edge midpoints, and the face's centre.
/// Naming them by *which* vertex, edge or face they come from -- rather than
/// by a position -- is what makes the two faces either side of a shared side
/// arrive at the same answer for it, exactly rather than nearly.
struct HdClaudePtexCorner {
    enum class Kind : std::uint8_t { Vertex, EdgeMidpoint, FaceCentre };
    Kind kind = Kind::Vertex;
    int index = 0;

    bool operator<(const HdClaudePtexCorner& other) const
    {
        return kind != other.kind ? kind < other.kind : index < other.index;
    }
    bool operator==(const HdClaudePtexCorner& other) const
    {
        return kind == other.kind && index == other.index;
    }
};

/// One quad of the limit surface's parameterisation, and where it came from.
struct HdClaudePtexFace {
    /// The coarse face this is part of, for carrying GeomSubsets through.
    int coarseFace = 0;
    /// Its four corners, in the order the domain's corners are: (0,0), (1,0),
    /// (1,1), (0,1).
    HdClaudePtexCorner corners[4];
};

/// The ptex faces of a topology, in the order OpenSubdiv numbers them.
///
/// A quad contributes one and anything else contributes one per corner, which
/// is what `Far::PtexIndices` counts and what `Far::PatchMap::FindPatch`
/// indexes by.
std::vector<HdClaudePtexFace> HdClaudePtexFaces(const HdMeshTopology& topology);

/// The object-space position of a ptex corner, for measuring a side.
///
/// The *cage's* position rather than the limit's, and deliberately: this
/// decides a tessellation rate, which is a choice rather than a measurement,
/// and it has to come out identical for the two faces that share a side. A
/// limit position would be equal mathematically and not bit for bit, and a
/// rate that disagreed in its last place at a threshold would open the seam it
/// exists to close.
void HdClaudePtexCornerPosition(const HdMeshTopology& topology,
                                const std::vector<float>& points,
                                const HdClaudePtexCorner& corner,
                                float position[3]);

/// Per-face refinement: the limit surface, sampled at a rate per side.
///
/// `edgeRates` is four rates per ptex face, in the order `HdClaudePtexFaces`
/// returns them and the order a domain's sides run. Positions come from
/// evaluating the limit surface rather than from a refined cage, which is what
/// lets neighbouring faces be tessellated differently without a crack: they
/// sample the same curve at the same parameters along the side they share.
///
/// `isolationLevel` is how far OpenSubdiv isolates irregular features before
/// it caps them; it bounds the patch table's size and has nothing to do with
/// how finely the result is tessellated.
///
/// Returns an invalid mesh when the topology cannot be refined or the rates do
/// not describe it, which the caller should treat as "refine this uniformly
/// instead" rather than as an error.
///
/// `worstSeamGap`, when given, receives the largest distance between two faces'
/// answers for a point they share. It is the number the whole design rests on
/// and it is measured rather than assumed: every sample on a side is recorded
/// against that side's identity and the step along it, and a second face
/// arriving at the same identity and step has to agree. Zero means the two
/// faces produced bit-identical positions; anything above the scale of float
/// rounding is a crack.
HdClaudeRefinedMesh HdClaudeSubdivideAdaptive(
    const HdMeshTopology& topology, const std::vector<float>& points,
    const std::vector<int>& edgeRates, int isolationLevel,
    const std::vector<float>& uvs = {},
    const std::vector<float>& faceVaryingUvs = {},
    float* worstSeamGap = nullptr);

PXR_NAMESPACE_CLOSE_SCOPE
