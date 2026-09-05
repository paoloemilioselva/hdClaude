#include "subdivision.h"

#include "trace.h"

#include "pxr/imaging/pxOsd/refinerFactory.h"
#include "pxr/imaging/pxOsd/tokens.h"

#include <opensubdiv/far/primvarRefiner.h>
#include <opensubdiv/far/topologyLevel.h>

#include <algorithm>

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

}  // namespace

bool HdClaudeWantsSubdivision(const HdMeshTopology& topology)
{
    const TfToken& scheme = topology.GetScheme();
    return scheme == PxOsdOpenSubdivTokens->catmullClark ||
           scheme == PxOsdOpenSubdivTokens->loop ||
           scheme == PxOsdOpenSubdivTokens->bilinear;
}

HdClaudeRefinedMesh HdClaudeSubdivide(const HdMeshTopology& topology,
                                      const std::vector<float>& points,
                                      int level)
{
    HdClaudeRefinedMesh result;

    const std::size_t coarseVertexCount = points.size() / 3;
    if (coarseVertexCount == 0 || level <= 0) {
        return result;
    }

    PxOsdTopologyRefinerSharedPtr refiner;
    try {
        refiner = PxOsdRefinerFactory::Create(topology.GetPxOsdMeshTopology());
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

    const OpenSubdiv::Far::TopologyLevel& refined = refiner->GetLevel(level);
    const int refinedVertexCount = refined.GetNumVertices();

    result.positions.resize(static_cast<std::size_t>(refinedVertexCount) * 3);
    for (int i = 0; i < refinedVertexCount; ++i) {
        result.positions[i * 3 + 0] = source[i].x;
        result.positions[i * 3 + 1] = source[i].y;
        result.positions[i * 3 + 2] = source[i].z;
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

        for (int corner = 1; corner + 1 < corners.size(); ++corner) {
            result.indices.push_back(static_cast<std::uint32_t>(corners[0]));
            result.indices.push_back(static_cast<std::uint32_t>(corners[corner]));
            result.indices.push_back(
                static_cast<std::uint32_t>(corners[corner + 1]));
            result.coarseFaces.push_back(coarse);
        }
    }

    return result;
}

PXR_NAMESPACE_CLOSE_SCOPE
