#include "subdivision.h"

#include "trace.h"

#include "pxr/imaging/pxOsd/refinerFactory.h"
#include "pxr/imaging/pxOsd/tokens.h"

#include <opensubdiv/far/patchMap.h>
#include <opensubdiv/far/patchTable.h>
#include <opensubdiv/far/patchTableFactory.h>
#include <opensubdiv/far/primvarRefiner.h>
#include <opensubdiv/far/topologyLevel.h>

#include "hdclaude/core/quad_tessellation.h"

#include <array>
#include <map>
#include <memory>

#include <algorithm>
#include <cmath>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

/// A vertex OpenSubdiv can interpolate. Its Clear/AddWithWeight pair is the
/// whole interface Far::PrimvarRefiner asks for.
struct RefinableVertex {
    float x = 0.0f, y = 0.0f, z = 0.0f;

    void Clear() { x = y = z = 0.0f; }

    void AddWithWeight(const RefinableVertex& source, float weight)
    {
        x += weight * source.x;
        y += weight * source.y;
        z += weight * source.z;
    }
};

/// The same interface for a texture coordinate.
///
/// Refined as *vertex* data because that is how the control cage carries it:
/// hdClaude reads only vertex-interpolated `st`, so there is no seam here that
/// face-varying refinement would preserve and this cannot introduce one.
struct RefinableUv {
    float u = 0.0f, v = 0.0f;

    void Clear() { u = v = 0.0f; }

    void AddWithWeight(const RefinableUv& source, float weight)
    {
        u += weight * source.u;
        v += weight * source.v;
    }
};

}  // namespace

std::size_t HdClaudeCoarseFaceCount(const HdMeshTopology& topology)
{
    return static_cast<std::size_t>(topology.GetFaceVertexCounts().size());
}

float HdClaudeMeanEdgeLength(const HdMeshTopology& topology,
                             const std::vector<float>& points)
{
    const VtIntArray& counts = topology.GetFaceVertexCounts();
    const VtIntArray& indices = topology.GetFaceVertexIndices();
    const std::size_t vertexCount = points.size() / 3;
    if (vertexCount == 0) {
        return 0.0f;
    }

    double total = 0.0;
    std::size_t edges = 0;
    std::size_t cursor = 0;
    for (const int count : counts) {
        if (count < 2 || cursor + static_cast<std::size_t>(count) >
                             static_cast<std::size_t>(indices.size())) {
            cursor += static_cast<std::size_t>(std::max(count, 0));
            continue;
        }
        for (int corner = 0; corner < count; ++corner) {
            // Every edge of every face, closing the loop, so a quad
            // contributes four. An edge shared by two faces is counted twice,
            // which weights it as the two faces that use it -- and since this
            // is a mean, weighting by use is what a mesh with a few long
            // boundary edges wants.
            const int a = indices[cursor + corner];
            const int b = indices[cursor + (corner + 1) % count];
            if (a < 0 || b < 0 ||
                static_cast<std::size_t>(a) >= vertexCount ||
                static_cast<std::size_t>(b) >= vertexCount) {
                continue;
            }
            const double dx = points[a * 3 + 0] - points[b * 3 + 0];
            const double dy = points[a * 3 + 1] - points[b * 3 + 1];
            const double dz = points[a * 3 + 2] - points[b * 3 + 2];
            total += std::sqrt(dx * dx + dy * dy + dz * dz);
            ++edges;
        }
        cursor += static_cast<std::size_t>(count);
    }
    if (edges == 0) {
        return 0.0f;
    }
    return static_cast<float>(total / static_cast<double>(edges));
}

bool HdClaudeWantsSubdivision(const HdMeshTopology& topology)
{
    const TfToken& scheme = topology.GetScheme();
    return scheme == PxOsdOpenSubdivTokens->catmullClark ||
           scheme == PxOsdOpenSubdivTokens->loop ||
           scheme == PxOsdOpenSubdivTokens->bilinear;
}

HdClaudeRefinedMesh HdClaudeSubdivide(const HdMeshTopology& topology,
                                      const std::vector<float>& points,
                                      int level,
                                      const std::vector<float>& uvs,
                                      const std::vector<float>& faceVaryingUvs)
{
    HdClaudeRefinedMesh result;

    const std::size_t coarseVertexCount = points.size() / 3;
    if (coarseVertexCount == 0 || level <= 0) {
        return result;
    }

    // A face-varying channel is declared to the refiner or it does not exist:
    // OpenSubdiv refines face-varying data against its *own* topology, which is
    // what preserves a seam. Hydra hands over the coordinates already
    // flattened -- one per face vertex, indices resolved -- so the channel's
    // topology is the identity.
    const std::size_t faceVaryingCount = faceVaryingUvs.size() / 2;
    std::vector<VtIntArray> faceVaryingTopologies;
    if (faceVaryingCount > 0) {
        VtIntArray identity(faceVaryingCount);
        for (std::size_t i = 0; i < faceVaryingCount; ++i) {
            identity[i] = static_cast<int>(i);
        }
        faceVaryingTopologies.push_back(std::move(identity));
    }

    PxOsdTopologyRefinerSharedPtr refiner;
    try {
        refiner = faceVaryingTopologies.empty()
                      ? PxOsdRefinerFactory::Create(topology.GetPxOsdMeshTopology())
                      : PxOsdRefinerFactory::Create(topology.GetPxOsdMeshTopology(),
                                                    faceVaryingTopologies);
    } catch (const std::exception& error) {
        HdClaudeTrace("subdivision: refiner construction failed (%s)",
                      error.what());
        return result;
    }
    if (!refiner) {
        return result;
    }

    // A refiner built from a topology whose vertex count disagrees with the
    // points array would read past the end of it. This happens for real: a
    // mesh whose points come from an ExtComputation can be synced while its
    // topology is still the previous frame's.
    if (static_cast<std::size_t>(refiner->GetLevel(0).GetNumVertices()) !=
        coarseVertexCount) {
        HdClaudeTrace(
            "subdivision: topology has %d vertices but %zu points were "
            "supplied; rendering the control cage",
            refiner->GetLevel(0).GetNumVertices(), coarseVertexCount);
        return result;
    }

    OpenSubdiv::Far::TopologyRefiner::UniformOptions options(level);
    // Refine every face to the same depth. Ordering the result by level rather
    // than interleaving it is what lets the last level be read as one
    // contiguous block below.
    options.fullTopologyInLastLevel = true;
    refiner->RefineUniform(options);

    // --- Points ---------------------------------------------------------------
    // One buffer holding every level, because PrimvarRefiner interpolates level
    // n from level n-1 and needs both alive at once.
    std::vector<RefinableVertex> vertices(
        static_cast<std::size_t>(refiner->GetNumVerticesTotal()));
    for (std::size_t i = 0; i < coarseVertexCount; ++i) {
        vertices[i].x = points[i * 3 + 0];
        vertices[i].y = points[i * 3 + 1];
        vertices[i].z = points[i * 3 + 2];
    }

    OpenSubdiv::Far::PrimvarRefiner primvarRefiner(*refiner);
    RefinableVertex* source = vertices.data();
    for (int current = 1; current <= level; ++current) {
        RefinableVertex* destination =
            source + refiner->GetLevel(current - 1).GetNumVertices();
        primvarRefiner.Interpolate(current, source, destination);
        source = destination;
    }

    // --- Texture coordinates --------------------------------------------------
    // Refined through the same weights as the positions, so the refined cage
    // carries the coordinates the control cage was authored with. Skipped when
    // the array does not describe this cage, which is the same guard the
    // positions get above.
    std::vector<RefinableUv> refinedUvs;
    RefinableUv* uvSource = nullptr;
    if (uvs.size() == coarseVertexCount * 2) {
        refinedUvs.resize(static_cast<std::size_t>(refiner->GetNumVerticesTotal()));
        for (std::size_t i = 0; i < coarseVertexCount; ++i) {
            refinedUvs[i].u = uvs[i * 2 + 0];
            refinedUvs[i].v = uvs[i * 2 + 1];
        }
        uvSource = refinedUvs.data();
        for (int current = 1; current <= level; ++current) {
            RefinableUv* destination =
                uvSource + refiner->GetLevel(current - 1).GetNumVertices();
            primvarRefiner.Interpolate(current, uvSource, destination);
            uvSource = destination;
        }
    }

    // --- Face-varying texture coordinates ------------------------------------
    std::vector<RefinableUv> refinedFaceVarying;
    RefinableUv* faceVaryingSource = nullptr;
    if (faceVaryingCount > 0 && refiner->GetNumFVarChannels() > 0 &&
        static_cast<std::size_t>(refiner->GetLevel(0).GetNumFVarValues(0)) ==
            faceVaryingCount) {
        refinedFaceVarying.resize(
            static_cast<std::size_t>(refiner->GetNumFVarValuesTotal(0)));
        for (std::size_t i = 0; i < faceVaryingCount; ++i) {
            refinedFaceVarying[i].u = faceVaryingUvs[i * 2 + 0];
            refinedFaceVarying[i].v = faceVaryingUvs[i * 2 + 1];
        }
        faceVaryingSource = refinedFaceVarying.data();
        for (int current = 1; current <= level; ++current) {
            RefinableUv* destination =
                faceVaryingSource + refiner->GetLevel(current - 1).GetNumFVarValues(0);
            primvarRefiner.InterpolateFaceVarying(current, faceVaryingSource,
                                                  destination, 0);
            faceVaryingSource = destination;
        }
    }

    const OpenSubdiv::Far::TopologyLevel& refined = refiner->GetLevel(level);
    const int refinedVertexCount = refined.GetNumVertices();

    result.positions.resize(static_cast<std::size_t>(refinedVertexCount) * 3);
    for (int i = 0; i < refinedVertexCount; ++i) {
        result.positions[i * 3 + 0] = source[i].x;
        result.positions[i * 3 + 1] = source[i].y;
        result.positions[i * 3 + 2] = source[i].z;
    }

    if (faceVaryingSource != nullptr) {
        // Filled per corner in the fan below, alongside the indices.
        result.uvsPerCorner = true;
        result.uvs.reserve(static_cast<std::size_t>(refined.GetNumFaces()) * 8);
    } else if (uvSource != nullptr) {
        result.uvs.resize(static_cast<std::size_t>(refinedVertexCount) * 2);
        for (int i = 0; i < refinedVertexCount; ++i) {
            result.uvs[i * 2 + 0] = uvSource[i].u;
            result.uvs[i * 2 + 1] = uvSource[i].v;
        }
    }

    // --- Faces ----------------------------------------------------------------
    // Catmull-Clark gives quads, Loop gives triangles, and bilinear preserves
    // the coarse valence, so the fan below has to handle any n rather than
    // assuming four.
    const int refinedFaceCount = refined.GetNumFaces();
    result.indices.reserve(static_cast<std::size_t>(refinedFaceCount) * 6);
    result.coarseFaces.reserve(static_cast<std::size_t>(refinedFaceCount) * 2);

    for (int face = 0; face < refinedFaceCount; ++face) {
        const OpenSubdiv::Far::ConstIndexArray corners =
            refined.GetFaceVertices(face);
        if (corners.size() < 3) {
            continue;
        }

        // Walk the parent chain back to the control cage, so a GeomSubset
        // authored on the coarse mesh still names the right refined triangles.
        int coarse = face;
        for (int current = level; current > 0; --current) {
            coarse = refiner->GetLevel(current).GetFaceParentFace(coarse);
            if (coarse < 0) {
                break;
            }
        }

        OpenSubdiv::Far::ConstIndexArray faceVaryingCorners;
        if (faceVaryingSource != nullptr) {
            faceVaryingCorners = refined.GetFaceFVarValues(face, 0);
        }

        for (int corner = 1; corner + 1 < corners.size(); ++corner) {
            result.indices.push_back(static_cast<std::uint32_t>(corners[0]));
            result.indices.push_back(static_cast<std::uint32_t>(corners[corner]));
            result.indices.push_back(
                static_cast<std::uint32_t>(corners[corner + 1]));
            result.coarseFaces.push_back(coarse);

            // The same fan, in face-varying values: a refined face's corners
            // index the channel, not the vertices, which is exactly the
            // distinction that keeps a seam a seam.
            if (faceVaryingSource != nullptr &&
                faceVaryingCorners.size() == corners.size()) {
                const int fan[3] = {faceVaryingCorners[0],
                                    faceVaryingCorners[corner],
                                    faceVaryingCorners[corner + 1]};
                for (const int value : fan) {
                    result.uvs.push_back(faceVaryingSource[value].u);
                    result.uvs.push_back(faceVaryingSource[value].v);
                }
            }
        }
    }

    return result;
}


// ---------------------------------------------------------------------------
// Per-face refinement
// ---------------------------------------------------------------------------

namespace {

/// A canonical name for a coarse edge, so that the two faces using it agree.
///
/// The lower vertex index first, always: a face walks its edges in its own
/// winding, and the two faces either side of an edge walk it in opposite
/// directions. Ordering the pair is what makes them the same edge.
struct CoarseEdgeKey {
    int low = 0;
    int high = 0;
    bool operator<(const CoarseEdgeKey& other) const
    {
        return low != other.low ? low < other.low : high < other.high;
    }
};

CoarseEdgeKey MakeEdgeKey(int a, int b)
{
    return a < b ? CoarseEdgeKey{a, b} : CoarseEdgeKey{b, a};
}

/// Edge indices for a topology, numbered by first use.
///
/// OpenSubdiv numbers edges too, but only after a refiner has been built from
/// a topology that may fail to build. This is asked before that and has to
/// answer for any topology at all.
class CoarseEdges {
  public:
    explicit CoarseEdges(const HdMeshTopology& topology)
    {
        const VtIntArray& counts = topology.GetFaceVertexCounts();
        const VtIntArray& indices = topology.GetFaceVertexIndices();
        std::size_t cursor = 0;
        for (const int count : counts) {
            if (count < 3 || cursor + static_cast<std::size_t>(count) >
                                 static_cast<std::size_t>(indices.size())) {
                cursor += static_cast<std::size_t>(std::max(count, 0));
                continue;
            }
            for (int corner = 0; corner < count; ++corner) {
                const int a = indices[cursor + corner];
                const int b = indices[cursor + (corner + 1) % count];
                const CoarseEdgeKey key = MakeEdgeKey(a, b);
                _index.emplace(key, static_cast<int>(_index.size()));
            }
            cursor += static_cast<std::size_t>(count);
        }
    }

    int Get(int a, int b) const
    {
        const auto found = _index.find(MakeEdgeKey(a, b));
        return found == _index.end() ? -1 : found->second;
    }

  private:
    std::map<CoarseEdgeKey, int> _index;
};

/// One sample of one side, named so that both its faces name it alike.
struct SeamKey {
    HdClaudePtexCorner low;
    HdClaudePtexCorner high;
    int step = 0;
    int rate = 0;
    bool operator<(const SeamKey& other) const
    {
        if (!(low == other.low)) return low < other.low;
        if (!(high == other.high)) return high < other.high;
        if (step != other.step) return step < other.step;
        return rate < other.rate;
    }
};

/// A point OpenSubdiv can interpolate and this can read back.
struct RefinablePoint3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    void Clear() { x = y = z = 0.0f; }
    void AddWithWeight(const RefinablePoint3& source, float weight)
    {
        x += weight * source.x;
        y += weight * source.y;
        z += weight * source.z;
    }
};

struct RefinablePoint2 {
    float u = 0.0f, v = 0.0f;
    void Clear() { u = v = 0.0f; }
    void AddWithWeight(const RefinablePoint2& source, float weight)
    {
        u += weight * source.u;
        v += weight * source.v;
    }
};

}  // namespace

std::vector<HdClaudePtexFace> HdClaudePtexFaces(const HdMeshTopology& topology)
{
    std::vector<HdClaudePtexFace> faces;
    const VtIntArray& counts = topology.GetFaceVertexCounts();
    const VtIntArray& indices = topology.GetFaceVertexIndices();
    const CoarseEdges edges(topology);

    std::size_t cursor = 0;
    for (int face = 0; face < static_cast<int>(counts.size()); ++face) {
        const int count = counts[face];
        if (count < 3 || cursor + static_cast<std::size_t>(count) >
                             static_cast<std::size_t>(indices.size())) {
            cursor += static_cast<std::size_t>(std::max(count, 0));
            continue;
        }

        if (count == 4) {
            // A quad is its own ptex face: the whole face is one domain, and
            // its sides are its four coarse edges.
            HdClaudePtexFace entry;
            entry.coarseFace = face;
            for (int corner = 0; corner < 4; ++corner) {
                entry.corners[corner] = {HdClaudePtexCorner::Kind::Vertex,
                                         indices[cursor + corner]};
            }
            faces.push_back(entry);
        } else {
            // Anything else is split at its centre, one domain per corner.
            // Each is bounded by half of the edge before it, half of the edge
            // after it, and two interior sides meeting at the centre.
            for (int corner = 0; corner < count; ++corner) {
                const int here = indices[cursor + corner];
                const int next = indices[cursor + (corner + 1) % count];
                const int previous =
                    indices[cursor + (corner + count - 1) % count];

                HdClaudePtexFace entry;
                entry.coarseFace = face;
                entry.corners[0] = {HdClaudePtexCorner::Kind::Vertex, here};
                entry.corners[1] = {HdClaudePtexCorner::Kind::EdgeMidpoint,
                                    edges.Get(here, next)};
                entry.corners[2] = {HdClaudePtexCorner::Kind::FaceCentre, face};
                entry.corners[3] = {HdClaudePtexCorner::Kind::EdgeMidpoint,
                                    edges.Get(previous, here)};
                faces.push_back(entry);
            }
        }
        cursor += static_cast<std::size_t>(count);
    }
    return faces;
}

void HdClaudePtexCornerPosition(const HdMeshTopology& topology,
                                const std::vector<float>& points,
                                const HdClaudePtexCorner& corner,
                                float position[3])
{
    position[0] = position[1] = position[2] = 0.0f;
    const std::size_t vertexCount = points.size() / 3;
    const auto read = [&points, vertexCount](int vertex, float out[3]) {
        if (vertex >= 0 && static_cast<std::size_t>(vertex) < vertexCount) {
            out[0] = points[vertex * 3 + 0];
            out[1] = points[vertex * 3 + 1];
            out[2] = points[vertex * 3 + 2];
        }
    };

    if (corner.kind == HdClaudePtexCorner::Kind::Vertex) {
        read(corner.index, position);
        return;
    }

    const VtIntArray& counts = topology.GetFaceVertexCounts();
    const VtIntArray& indices = topology.GetFaceVertexIndices();

    if (corner.kind == HdClaudePtexCorner::Kind::FaceCentre) {
        std::size_t cursor = 0;
        for (int face = 0; face < static_cast<int>(counts.size()); ++face) {
            const int count = counts[face];
            if (face == corner.index && count > 0 &&
                cursor + static_cast<std::size_t>(count) <=
                    static_cast<std::size_t>(indices.size())) {
                for (int i = 0; i < count; ++i) {
                    float point[3] = {0.0f, 0.0f, 0.0f};
                    read(indices[cursor + i], point);
                    for (int axis = 0; axis < 3; ++axis) {
                        position[axis] += point[axis];
                    }
                }
                for (int axis = 0; axis < 3; ++axis) {
                    position[axis] /= static_cast<float>(count);
                }
                return;
            }
            cursor += static_cast<std::size_t>(std::max(count, 0));
        }
        return;
    }

    // An edge midpoint. Found by walking to the edge with that index, which is
    // the same numbering `HdClaudePtexFaces` used.
    const CoarseEdges edges(topology);
    std::size_t cursor = 0;
    for (const int count : counts) {
        if (count < 3 || cursor + static_cast<std::size_t>(count) >
                             static_cast<std::size_t>(indices.size())) {
            cursor += static_cast<std::size_t>(std::max(count, 0));
            continue;
        }
        for (int i = 0; i < count; ++i) {
            const int a = indices[cursor + i];
            const int b = indices[cursor + (i + 1) % count];
            if (edges.Get(a, b) == corner.index) {
                float first[3] = {0.0f, 0.0f, 0.0f};
                float second[3] = {0.0f, 0.0f, 0.0f};
                // The lower index first, so the average is the same sum in the
                // same order whichever face asked.
                read(std::min(a, b), first);
                read(std::max(a, b), second);
                for (int axis = 0; axis < 3; ++axis) {
                    position[axis] = 0.5f * (first[axis] + second[axis]);
                }
                return;
            }
        }
        cursor += static_cast<std::size_t>(count);
    }
}

HdClaudeRefinedMesh HdClaudeSubdivideAdaptive(
    const HdMeshTopology& topology, const std::vector<float>& points,
    const std::vector<int>& edgeRates, int isolationLevel,
    const std::vector<float>& uvs, const std::vector<float>& faceVaryingUvs,
    float* worstSeamGap)
{
    HdClaudeRefinedMesh result;
    if (worstSeamGap != nullptr) {
        *worstSeamGap = 0.0f;
    }

    const std::size_t coarseVertexCount = points.size() / 3;
    const std::vector<HdClaudePtexFace> ptexFaces = HdClaudePtexFaces(topology);
    if (coarseVertexCount == 0 || ptexFaces.empty() ||
        edgeRates.size() != ptexFaces.size() * 4) {
        return result;
    }

    // --- The texture-coordinate channel --------------------------------------
    //
    // One face-varying channel either way. A face-varying set is already one
    // value per face vertex; a vertex-interpolated set becomes a channel whose
    // topology *is* the vertex topology, which refines identically -- so the
    // two arrive at the same place and only one path has to be right.
    const VtIntArray& faceVertexIndices = topology.GetFaceVertexIndices();
    std::vector<float> channelValues;
    VtIntArray channelIndices;
    if (faceVaryingUvs.size() >= 2 &&
        faceVaryingUvs.size() / 2 ==
            static_cast<std::size_t>(faceVertexIndices.size())) {
        channelValues = faceVaryingUvs;
        channelIndices.resize(faceVertexIndices.size());
        for (std::size_t i = 0; i < channelIndices.size(); ++i) {
            channelIndices[i] = static_cast<int>(i);
        }
    } else if (uvs.size() == coarseVertexCount * 2) {
        channelValues = uvs;
        channelIndices = faceVertexIndices;
    }

    std::vector<VtIntArray> channelTopologies;
    if (!channelValues.empty()) {
        channelTopologies.push_back(channelIndices);
    }

    PxOsdTopologyRefinerSharedPtr refiner;
    try {
        refiner = channelTopologies.empty()
                      ? PxOsdRefinerFactory::Create(topology.GetPxOsdMeshTopology())
                      : PxOsdRefinerFactory::Create(topology.GetPxOsdMeshTopology(),
                                                    channelTopologies);
    } catch (const std::exception& error) {
        HdClaudeTrace("adaptive subdivision: refiner failed (%s)", error.what());
        return result;
    }
    if (!refiner ||
        static_cast<std::size_t>(refiner->GetLevel(0).GetNumVertices()) !=
            coarseVertexCount) {
        return result;
    }

    using namespace OpenSubdiv;

    const int isolation = std::clamp(isolationLevel, 1, 6);
    Far::TopologyRefiner::AdaptiveOptions adaptiveOptions(isolation);
    adaptiveOptions.considerFVarChannels = !channelTopologies.empty();
    adaptiveOptions.useInfSharpPatch = true;
    refiner->RefineAdaptive(adaptiveOptions);

    Far::PatchTableFactory::Options patchOptions(isolation);
    // Gregory end caps, and this is the one option here that is not a
    // preference. Everything per-face refinement does rests on two faces
    // evaluating the side they share to the same points, and around an
    // extraordinary vertex -- which every pole of a sphere and every corner of
    // a cube has -- the patches are caps rather than the real limit surface.
    // A B-spline cap is cheaper and is only *approximately* continuous with
    // the regular patches beside it, which is a gap: exactly the crack this
    // was built to avoid, in exactly the places a subdivision surface is
    // interesting. Gregory caps are the basis OpenSubdiv provides for being
    // watertight across that boundary, and they are what this needs.
    patchOptions.SetEndCapType(
        Far::PatchTableFactory::Options::ENDCAP_GREGORY_BASIS);
    patchOptions.useInfSharpPatch = true;
    patchOptions.generateFVarTables = !channelTopologies.empty();
    const int channel = 0;
    if (patchOptions.generateFVarTables) {
        patchOptions.numFVarChannels = 1;
        patchOptions.fvarChannelIndices = &channel;
        patchOptions.SetFVarPatchPrecision<float>();
    }

    const std::unique_ptr<Far::PatchTable> patchTable(
        Far::PatchTableFactory::Create(*refiner, patchOptions));
    if (!patchTable) {
        return result;
    }
    const Far::PatchMap patchMap(*patchTable);

    // --- Control points ------------------------------------------------------
    //
    // Every level the adaptive refinement produced, then the local points the
    // end caps need. A patch's control vertices index into this whole buffer,
    // so it is one array rather than a per-level one.
    std::vector<RefinablePoint3> vertices(
        static_cast<std::size_t>(refiner->GetNumVerticesTotal()) +
        static_cast<std::size_t>(patchTable->GetNumLocalPoints()));
    for (std::size_t i = 0; i < coarseVertexCount; ++i) {
        vertices[i].x = points[i * 3 + 0];
        vertices[i].y = points[i * 3 + 1];
        vertices[i].z = points[i * 3 + 2];
    }

    Far::PrimvarRefiner primvarRefiner(*refiner);
    {
        RefinablePoint3* source = vertices.data();
        for (int level = 1; level <= refiner->GetMaxLevel(); ++level) {
            RefinablePoint3* destination =
                source + refiner->GetLevel(level - 1).GetNumVertices();
            primvarRefiner.Interpolate(level, source, destination);
            source = destination;
        }
    }
    if (patchTable->GetNumLocalPoints() > 0) {
        patchTable->ComputeLocalPointValues(
            vertices.data(),
            vertices.data() + refiner->GetNumVerticesTotal());
    }

    std::vector<RefinablePoint2> channelPoints;
    const bool haveChannel =
        !channelValues.empty() && refiner->GetNumFVarChannels() > 0;
    if (haveChannel) {
        channelPoints.resize(
            static_cast<std::size_t>(refiner->GetNumFVarValuesTotal(channel)) +
            static_cast<std::size_t>(
                patchTable->GetNumLocalPointsFaceVarying(channel)));
        const std::size_t sourceCount = channelValues.size() / 2;
        for (std::size_t i = 0;
             i < sourceCount && i < channelPoints.size(); ++i) {
            channelPoints[i].u = channelValues[i * 2 + 0];
            channelPoints[i].v = channelValues[i * 2 + 1];
        }
        RefinablePoint2* source = channelPoints.data();
        for (int level = 1; level <= refiner->GetMaxLevel(); ++level) {
            RefinablePoint2* destination =
                source + refiner->GetLevel(level - 1).GetNumFVarValues(channel);
            primvarRefiner.InterpolateFaceVarying(level, source, destination,
                                                  channel);
            source = destination;
        }
        if (patchTable->GetNumLocalPointsFaceVarying(channel) > 0) {
            patchTable->ComputeLocalPointValuesFaceVarying(
                channelPoints.data(),
                channelPoints.data() +
                    refiner->GetNumFVarValuesTotal(channel),
                channel);
        }
        result.uvsPerCorner = true;
    }

    // --- Tessellate and evaluate ---------------------------------------------
    std::map<SeamKey, std::array<float, 3>> seam;
    for (std::size_t face = 0; face < ptexFaces.size(); ++face) {
        int rates[4];
        for (int side = 0; side < 4; ++side) {
            rates[side] = edgeRates[face * 4 + static_cast<std::size_t>(side)];
        }
        const hdclaude::QuadTessellation domain =
            hdclaude::TessellateQuad(rates);
        if (!domain.Valid()) {
            continue;
        }

        const auto base = static_cast<std::uint32_t>(result.positions.size() / 3);
        const std::size_t firstSample = result.positions.size() / 3;
        std::vector<float> sampleUvs;
        if (haveChannel) {
            sampleUvs.resize(domain.SampleCount() * 2, 0.0f);
        }

        for (std::size_t sample = 0; sample < domain.SampleCount(); ++sample) {
            const float u = domain.uv[sample * 2 + 0];
            const float v = domain.uv[sample * 2 + 1];

            const Far::PatchTable::PatchHandle* handle =
                patchMap.FindPatch(static_cast<int>(face), u, v);
            if (handle == nullptr) {
                result.positions.insert(result.positions.end(), {0.0f, 0.0f, 0.0f});
                continue;
            }

            // Twenty, which is a Gregory patch's control point count and the
            // largest any patch here has: a regular B-spline patch has sixteen.
            float weights[20];
            float du[20];
            float dv[20];
            patchTable->EvaluateBasis(*handle, u, v, weights, du, dv);
            const Far::ConstIndexArray controls =
                patchTable->GetPatchVertices(*handle);

            RefinablePoint3 position;
            for (int i = 0; i < controls.size(); ++i) {
                position.AddWithWeight(vertices[controls[i]], weights[i]);
            }
            result.positions.push_back(position.x);
            result.positions.push_back(position.y);
            result.positions.push_back(position.z);

            if (haveChannel) {
                float channelWeights[20];
                float channelDu[20];
                float channelDv[20];
                patchTable->EvaluateBasisFaceVarying(
                    *handle, u, v, channelWeights, channelDu, channelDv,
                    nullptr, nullptr, nullptr, channel);
                const Far::ConstIndexArray channelControls =
                    patchTable->GetPatchFVarValues(*handle, channel);
                RefinablePoint2 coordinate;
                for (int i = 0; i < channelControls.size(); ++i) {
                    coordinate.AddWithWeight(channelPoints[channelControls[i]],
                                             channelWeights[i]);
                }
                sampleUvs[sample * 2 + 0] = coordinate.u;
                sampleUvs[sample * 2 + 1] = coordinate.v;
            }
        }

        // --- What the seam cost ----------------------------------------------
        //
        // Every boundary sample is filed under the side it is on and how far
        // along that side it is, both named the same way by whichever face is
        // asking. The second face to file a given entry has to agree with the
        // first, and how much it does not is the gap.
        if (worstSeamGap != nullptr) {
            std::size_t at = 0;
            for (int side = 0; side < 4; ++side) {
                const HdClaudePtexCorner from = ptexFaces[face].corners[side];
                const HdClaudePtexCorner to =
                    ptexFaces[face].corners[(side + 1) % 4];
                const bool forward = from < to;
                const int rate = rates[side];
                for (int step = 0; step < rate; ++step, ++at) {
                    // Measured from the lower-named corner, so the two faces
                    // that walk this side in opposite directions still file
                    // each sample under the same step.
                    const int canonical = forward ? step : rate - step;
                    const SeamKey key{forward ? from : to, forward ? to : from,
                                      canonical, rate};
                    const std::size_t sample = firstSample + at;
                    const float* position = &result.positions[sample * 3];
                    const auto existing = seam.find(key);
                    if (existing == seam.end()) {
                        seam.emplace(key, std::array<float, 3>{position[0],
                                                               position[1],
                                                               position[2]});
                        continue;
                    }
                    const float dx = position[0] - existing->second[0];
                    const float dy = position[1] - existing->second[1];
                    const float dz = position[2] - existing->second[2];
                    *worstSeamGap = std::max(
                        *worstSeamGap,
                        std::sqrt(dx * dx + dy * dy + dz * dz));
                }
            }
        }

        for (std::size_t t = 0; t < domain.TriangleCount(); ++t) {
            for (int corner = 0; corner < 3; ++corner) {
                const std::uint32_t local = domain.indices[t * 3 + corner];
                result.indices.push_back(base + local);
                if (haveChannel) {
                    result.uvs.push_back(sampleUvs[local * 2 + 0]);
                    result.uvs.push_back(sampleUvs[local * 2 + 1]);
                }
            }
            result.coarseFaces.push_back(ptexFaces[face].coarseFace);
        }
    }

    return result;
}

PXR_NAMESPACE_CLOSE_SCOPE
