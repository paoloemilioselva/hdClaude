#include "subdivision.h"

#include "trace.h"

#include "pxr/imaging/pxOsd/refinerFactory.h"
#include "pxr/imaging/pxOsd/tokens.h"

#include <opensubdiv/far/primvarRefiner.h>
#include <opensubdiv/far/topologyLevel.h>

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

PXR_NAMESPACE_CLOSE_SCOPE
