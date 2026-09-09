#include "mesh.h"

#include "instancer.h"

#include "material_compiler.h"
#include "render_param.h"

#include <chrono>
#include "scene_store.h"
#include "subdivision.h"
#include "trace.h"

#include "pxr/base/gf/range3d.h"
#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/gf/vec2f.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/imaging/hd/extComputationUtils.h"
#include "pxr/imaging/hd/geomSubset.h"
#include "pxr/imaging/hd/meshUtil.h"
#include "pxr/imaging/hd/smoothNormals.h"
#include "pxr/imaging/hd/vertexAdjacency.h"
#include "pxr/imaging/hd/vtBufferSource.h"
#include "pxr/imaging/hdMtlx/hdMtlx.h"

#include <algorithm>
#include <map>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

/// Row-major 3x4, the layout Vulkan's instance structure expects.
hdclaude::Transform3x4 ToTransform(const GfMatrix4d& matrix)
{
    hdclaude::Transform3x4 out;
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            // GfMatrix4d is row-major with translation in row 3, so the basis
            // is transposed on the way out.
            out.m[row * 4 + column] = static_cast<float>(matrix[column][row]);
        }
        out.m[row * 4 + 3] = static_cast<float>(matrix[3][row]);
    }
    return out;
}

/// Flatten a VtValue of points into interleaved floats.
bool ExtractPoints(const VtValue& value, std::vector<float>& out)
{
    if (value.IsHolding<VtVec3fArray>()) {
        const VtVec3fArray& points = value.UncheckedGet<VtVec3fArray>();
        out.resize(points.size() * 3);
        for (std::size_t i = 0; i < points.size(); ++i) {
            out[i * 3 + 0] = points[i][0];
            out[i * 3 + 1] = points[i][1];
            out[i * 3 + 2] = points[i][2];
        }
        return true;
    }
    if (value.IsHolding<VtVec3dArray>()) {
        const VtVec3dArray& points = value.UncheckedGet<VtVec3dArray>();
        out.resize(points.size() * 3);
        for (std::size_t i = 0; i < points.size(); ++i) {
            out[i * 3 + 0] = static_cast<float>(points[i][0]);
            out[i * 3 + 1] = static_cast<float>(points[i][1]);
            out[i * 3 + 2] = static_cast<float>(points[i][2]);
        }
        return true;
    }
    return false;
}

/// Flatten a VtValue of vectors into a GfVec3f array, whatever its precision.
///
/// Normals reach Hydra as `normal3f`, `vector3f`, or either one's double
/// spelling, and a reader that accepts only the first quietly drops the rest.
bool ExtractVectors(const VtValue& value, VtVec3fArray& out)
{
    if (value.IsHolding<VtVec3fArray>()) {
        out = value.UncheckedGet<VtVec3fArray>();
        return true;
    }
    if (value.IsHolding<VtVec3dArray>()) {
        const VtVec3dArray& source = value.UncheckedGet<VtVec3dArray>();
        out.resize(source.size());
        for (std::size_t i = 0; i < source.size(); ++i) {
            out[i] = GfVec3f(source[i]);
        }
        return true;
    }
    return false;
}

/// Where a mesh's generated displayColor material is published.
///
/// A property path, so it can never be mistaken for -- or collide with -- a
/// material prim the stage actually contains.
SdfPath DisplayColorMaterialPath(const SdfPath& id)
{
    return id.AppendProperty(TfToken("hdClaudeDisplayColor"));
}

GfVec3f DisplayColor(HdSceneDelegate* delegate, const SdfPath& id)
{
    // Unbound geometry keeps the colour Hydra supplies rather than becoming a
    // uniform grey, so an unshaded asset still reads as itself.
    const VtValue value =
        delegate->Get(id, HdTokens->displayColor);
    if (value.IsHolding<VtVec3fArray>()) {
        const VtVec3fArray& colors = value.UncheckedGet<VtVec3fArray>();
        if (!colors.empty()) {
            return colors[0];
        }
    }
    return GfVec3f(0.5f, 0.5f, 0.5f);
}

}  // namespace

HdClaudeMesh::HdClaudeMesh(const SdfPath& id) : HdMesh(id) {}
HdClaudeMesh::~HdClaudeMesh() = default;

HdDirtyBits HdClaudeMesh::GetInitialDirtyBitsMask() const
{
    return HdChangeTracker::Clean | HdChangeTracker::DirtyPoints |
           HdChangeTracker::DirtyTopology | HdChangeTracker::DirtyTransform |
           HdChangeTracker::DirtyVisibility | HdChangeTracker::DirtyPrimvar |
           HdChangeTracker::DirtyNormals | HdChangeTracker::DirtyInstancer |
           HdChangeTracker::DirtyMaterialId | HdChangeTracker::DirtyDisplayStyle |
           HdChangeTracker::DirtySubdivTags;
}

HdDirtyBits HdClaudeMesh::_PropagateDirtyBits(HdDirtyBits bits) const
{
    return bits;
}

void HdClaudeMesh::_InitRepr(const TfToken& reprToken, HdDirtyBits*)
{
    // One representation: a path tracer has no notion of a wireframe or a
    // shaded-smooth variant, so every repr resolves to the same geometry.
    if (std::find_if(_reprs.begin(), _reprs.end(),
                     _ReprComparator(reprToken)) == _reprs.end()) {
        _reprs.emplace_back(reprToken, HdReprSharedPtr());
    }
}

void HdClaudeMesh::Finalize(HdRenderParam* renderParam)
{
    if (auto* param = static_cast<HdClaudeRenderParam*>(renderParam)) {
        param->SceneStore()->RemoveMesh(GetId());
        // The generated displayColor material, if this mesh made one, dies
        // with it. Leaving it behind would keep a compiled pipeline alive for
        // geometry that no longer exists.
        param->SceneStore()->RemoveMaterial(DisplayColorMaterialPath(GetId()));
    }
}

void HdClaudeMesh::Sync(HdSceneDelegate* sceneDelegate,
                        HdRenderParam* renderParam, HdDirtyBits* dirtyBits,
                        const TfToken& /*reprToken*/)
{
    auto* param = static_cast<HdClaudeRenderParam*>(renderParam);
    if (param == nullptr || sceneDelegate == nullptr) {
        return;
    }
    const SdfPath& id = GetId();

    // The instancer this mesh belongs to, and its parents, before anything
    // reads a transform.
    //
    // `HdRprim::GetInstancerId()` is not filled in by Hydra on its own -- an
    // rprim learns which instancer it belongs to only when it calls
    // `_UpdateInstancer`, and the instancer is created and synced only when
    // something asks for it. Skipping this leaves every instancer id empty,
    // `CreateInstancer` is never called, and a point-instanced prototype is
    // published once at its own transform: the OpenChessSet rendered one pawn
    // in the middle of the board instead of sixteen on it.
    _UpdateInstancer(sceneDelegate, dirtyBits);
    HdInstancer::_SyncInstancerAndParents(sceneDelegate->GetRenderIndex(),
                                          GetInstancerId());

    // Material binding first: the scene store resolves it to an index, and a
    // mesh published before its material simply uses the fallback until the
    // material's own Sync lands and bumps the revision.
    if (HdChangeTracker::IsDirty(*dirtyBits)) {
        SetMaterialId(sceneDelegate->GetMaterialId(id));
    }

    HdMeshTopology topology = GetMeshTopology(sceneDelegate);

    // Subdivision tags are a *separate* scene-delegate call, and everything
    // that makes a subdivision surface look like the model the artist built is
    // in them.
    //
    // `GetMeshTopology` returns the cage -- counts, indices, scheme, holes --
    // and nothing about how to refine it. Without
    // `GetSubdivTags` the refiner falls back to OpenSubdiv's own defaults, and
    // those disagree with USD's on every point that matters:
    //
    //   * `interpolateBoundary` defaults to `edgeAndCorner` in USD and to
    //     "none" in OpenSubdiv, so an open boundary is left to float inward
    //     instead of being pinned. Every border edge then shrinks away from
    //     wherever it used to meet its neighbour, which is what had been
    //     splitting the Collective Project robot's shell open along seams that
    //     are closed in the asset.
    //   * `creaseIndices`, `creaseSharpness` and their corner equivalents are
    //     simply absent, so a creased edge refines as a smooth one and the
    //     model rounds off where it was built sharp.
    //   * `faceVaryingLinearInterpolation` defaults to `cornersPlus1` in USD
    //     and to "all" -- fully linear -- in OpenSubdiv, which moves texture
    //     coordinates at every UV seam and so moves the texture.
    //
    // One missing call, and it reads as three unrelated bugs in geometry, in
    // UVs, and in texturing.
    topology.SetSubdivTags(sceneDelegate->GetSubdivTags(id));

    HdClaudeMeshEntry entry;
    entry.material = GetMaterialId();
    entry.visible = sceneDelegate->GetVisible(id);
    entry.prototype.debugName = id.GetString();

    // --- Points -------------------------------------------------------------
    //
    // A skinned mesh does not author its deformed points: UsdSkel arrives as an
    // ExtComputation whose output *is* the points, and `Get(id, points)` on
    // such a prim returns the rest pose or nothing at all. Reading the computed
    // primvars first is what makes a deforming character deform instead of
    // standing in its bind pose -- or, as happened here, vanishing entirely
    // because the prim published no points and was dropped.
    std::vector<float> points;
    bool havePoints = false;

    const HdExtComputationPrimvarDescriptorVector computedPrimvars =
        sceneDelegate->GetExtComputationPrimvarDescriptors(
            id, HdInterpolationVertex);
    if (!computedPrimvars.empty()) {
        const HdExtComputationUtils::ValueStore computed =
            HdExtComputationUtils::GetComputedPrimvarValues(computedPrimvars,
                                                            sceneDelegate);
        for (const HdExtComputationPrimvarDescriptor& descriptor :
             computedPrimvars) {
            if (descriptor.name != HdTokens->points) {
                continue;
            }
            const auto found = computed.find(descriptor.name);
            if (found != computed.end() &&
                ExtractPoints(found->second, points) && !points.empty()) {
                havePoints = true;
            }
            break;
        }
    }

    if (!havePoints) {
        havePoints =
            ExtractPoints(sceneDelegate->Get(id, HdTokens->points), points) &&
            !points.empty();
    }

    if (!havePoints) {
        param->SceneStore()->RemoveMesh(id);
        *dirtyBits = HdChangeTracker::Clean;
        return;
    }

    // --- Subdivision or triangulation ------------------------------------------
    //
    // A mesh whose scheme asks for subdivision is refined and the refined cage
    // is traced. Both routes end in the same two things -- triangle indices and
    // the coarse face behind each triangle -- so everything downstream, subsets
    // included, is written once.
    std::vector<std::uint32_t> indices;
    std::vector<int> coarseFaces;

    // The texture coordinates, and how they are interpolated.
    //
    // Asking the prim rather than guessing from the array's length: a quad with
    // four vertices and six face-varying coordinates is a real and common
    // shape -- the StandardShaderBall's ground and every wall of its box are
    // exactly that -- and a length test rejects it, which is how those
    // surfaces ended up shaded from barycentrics.
    VtVec2fArray authoredUvs;
    HdInterpolation uvInterpolation = HdInterpolationVertex;
    bool haveUvs = false;
    for (const HdInterpolation interpolation :
         {HdInterpolationVertex, HdInterpolationVarying,
          HdInterpolationFaceVarying}) {
        for (const HdPrimvarDescriptor& descriptor :
             GetPrimvarDescriptors(sceneDelegate, interpolation)) {
            if (descriptor.name != TfToken("st") &&
                descriptor.name != TfToken("uv")) {
                continue;
            }
            const VtValue value = sceneDelegate->Get(id, descriptor.name);
            if (!value.IsHolding<VtVec2fArray>()) {
                continue;
            }
            authoredUvs = value.UncheckedGet<VtVec2fArray>();
            uvInterpolation = interpolation;
            haveUvs = !authoredUvs.empty();
            break;
        }
        if (haveUvs) {
            break;
        }
    }

    // The control cage's coordinates, read before refinement because
    // refinement is what has to carry them: they are authored per control
    // vertex, and the refined cage has different vertices. Only the
    // vertex-interpolated ones refine; a face-varying set needs an OpenSubdiv
    // channel of its own, which is recorded as remaining work rather than
    // approximated by refining it as vertex data across its own seams.
    std::vector<float> coarseUvs;
    std::vector<float> coarseFaceVaryingUvs;
    if (haveUvs && uvInterpolation == HdInterpolationFaceVarying) {
        coarseFaceVaryingUvs.resize(authoredUvs.size() * 2);
        for (std::size_t i = 0; i < authoredUvs.size(); ++i) {
            coarseFaceVaryingUvs[i * 2 + 0] = authoredUvs[i][0];
            coarseFaceVaryingUvs[i * 2 + 1] = authoredUvs[i][1];
        }
    } else if (haveUvs && authoredUvs.size() == points.size() / 3) {
        coarseUvs.resize(authoredUvs.size() * 2);
        for (std::size_t i = 0; i < authoredUvs.size(); ++i) {
            coarseUvs[i * 2 + 0] = authoredUvs[i][0];
            coarseUvs[i * 2 + 1] = authoredUvs[i][1];
        }
    }

    const int subdivisionLevel = param->SubdivisionLevel();
    bool subdivided = false;
    if (subdivisionLevel > 0 && HdClaudeWantsSubdivision(topology)) {
        // Timed here rather than inside the refiner, because what a caller
        // wants to know is what refinement cost *this prim*, and the refiner is
        // a free function with no notion of which prim it is serving.
        const auto refineStart = std::chrono::steady_clock::now();
        const HdClaudeRefinedMesh refined = HdClaudeSubdivide(
            topology, points, subdivisionLevel, coarseUvs, coarseFaceVaryingUvs);
        const double refineMs =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - refineStart)
                .count();
        if (HdClaudeStageStats* stats = param->StageStats()) {
            HdClaudeAddMilliseconds(stats->subdivideMilliseconds, refineMs);
            stats->meshesRefined.fetch_add(1, std::memory_order_relaxed);
            stats->subdivideInputPoints.fetch_add(points.size(),
                                                  std::memory_order_relaxed);
            stats->subdivideOutputPoints.fetch_add(refined.positions.size(),
                                                   std::memory_order_relaxed);
        }
        // The limit surface of a Catmull-Clark cage lies inside the convex
        // hull of that cage, so a refined mesh can never be larger than the
        // mesh it came from. That is a theorem rather than a tolerance, which
        // makes it worth asserting: a refined point outside the input's bounds
        // is a defect in refinement, and a single stray vertex is enough to
        // inflate an acceleration structure's upper nodes and cost far more in
        // traversal than the geometry it belongs to.
        if (refined.Valid() && !points.empty() &&
            !refined.positions.empty()) {
            // Both are flat triples of floats, so they are walked as such.
            const auto bounds = [](const std::vector<float>& xyz) {
                GfRange3d range;
                for (std::size_t i = 0; i + 2 < xyz.size(); i += 3) {
                    range.UnionWith(
                        GfVec3d(xyz[i], xyz[i + 1], xyz[i + 2]));
                }
                return range;
            };
            const GfRange3d cage = bounds(points);
            const GfRange3d limit = bounds(refined.positions);
            const bool comparable = !cage.IsEmpty() && !limit.IsEmpty();
            // A hair of slack for the arithmetic, proportional to the cage
            // rather than absolute, so the check means the same thing on a
            // building and on a bolt.
            const double slack =
                1e-4 * std::max(1e-6, cage.GetSize().GetLength());
            GfRange3d grown = cage;
            grown.UnionWith(cage.GetMin() - GfVec3d(slack, slack, slack));
            grown.UnionWith(cage.GetMax() + GfVec3d(slack, slack, slack));
            if (comparable && !grown.Contains(limit)) {
                TF_WARN(
                    "hdClaude: %s refined outside its control cage: cage "
                    "(%g %g %g)-(%g %g %g), limit (%g %g %g)-(%g %g %g). "
                    "A Catmull-Clark limit surface cannot leave the hull of "
                    "its cage, so this is a refinement defect.",
                    id.GetText(), cage.GetMin()[0], cage.GetMin()[1],
                    cage.GetMin()[2], cage.GetMax()[0], cage.GetMax()[1],
                    cage.GetMax()[2], limit.GetMin()[0], limit.GetMin()[1],
                    limit.GetMin()[2], limit.GetMax()[0], limit.GetMax()[1],
                    limit.GetMax()[2]);
            }
        }
        if (refined.Valid()) {
            points = refined.positions;
            indices = refined.indices;
            coarseFaces = refined.coarseFaces;
            entry.prototype.uvs = refined.uvs;
            entry.prototype.uvsPerCorner = refined.uvsPerCorner;
            subdivided = true;
        }
        // An invalid result means "render the control cage": a mesh that
        // cannot be refined should still appear, and the reason is traced.
    }

    if (!subdivided) {
        // HdMeshUtil owns the face-varying and index bookkeeping, including the
        // coarse-face index each triangle came from.
        HdMeshUtil meshUtil(&topology, id);
        VtVec3iArray triangleIndices;
        VtIntArray primitiveParams;
        meshUtil.ComputeTriangleIndices(&triangleIndices, &primitiveParams);

        indices.reserve(triangleIndices.size() * 3);
        for (const GfVec3i& triangle : triangleIndices) {
            indices.push_back(static_cast<std::uint32_t>(triangle[0]));
            indices.push_back(static_cast<std::uint32_t>(triangle[1]));
            indices.push_back(static_cast<std::uint32_t>(triangle[2]));
        }
        coarseFaces.reserve(primitiveParams.size());
        for (const int faceParam : primitiveParams) {
            coarseFaces.push_back(
                HdMeshUtil::DecodeFaceIndexFromCoarseFaceParam(faceParam));
        }
    }

    if (indices.empty()) {
        param->SceneStore()->RemoveMesh(id);
        *dirtyBits = HdChangeTracker::Clean;
        return;
    }

    // --- GeomSubsets ----------------------------------------------------------
    // A per-face material binding becomes a per-triangle one, using the coarse
    // face index HdMeshUtil already recorded for each triangle. Doing it here
    // rather than splitting the mesh into one prototype per subset keeps a
    // single acceleration structure per mesh, which is what makes a
    // twenty-subset asset cost one build rather than twenty.
    const HdGeomSubsets& subsets = topology.GetGeomSubsets();
    if (!subsets.empty()) {
        entry.subsetMaterials.reserve(subsets.size());

        // Coarse face -> subset. A face named by no subset keeps the mesh's own
        // binding, and a face named by two takes the last, which is what USD's
        // own ordering implies.
        std::map<int, int> faceSubset;
        for (std::size_t i = 0; i < subsets.size(); ++i) {
            const HdGeomSubset& subset = subsets[i];
            entry.subsetMaterials.push_back(subset.materialId);
            if (subset.type != HdGeomSubset::TypeFaceSet) {
                continue;
            }
            for (const int face : subset.indices) {
                faceSubset[face] = static_cast<int>(i);
            }
        }

        entry.triangleSubsets.assign(coarseFaces.size(), -1);
        for (std::size_t t = 0; t < coarseFaces.size(); ++t) {
            const auto found = faceSubset.find(coarseFaces[t]);
            if (found != faceSubset.end()) {
                entry.triangleSubsets[t] = found->second;
            }
        }
    }

    entry.prototype.positions = std::move(points);
    entry.prototype.indices = std::move(indices);

    // --- Normals -------------------------------------------------------------
    // Authored normals win, in whatever interpolation they were authored in;
    // otherwise they are generated, because a mesh shaded from its triangle
    // planes reads as a modelling error rather than as a missing primvar.
    //
    // USD spells authored normals two ways -- the `normals` attribute
    // UsdGeomMesh declares, and a `primvars:normals` primvar -- and Hydra
    // presents both as a primvar named `normals`. Asking the *descriptors* for
    // it is therefore what finds either one, and it is also what says how the
    // array is indexed. Asking `Get` for a vertex-length array instead, as this
    // used to, silently dropped every face-varying and uniform set: those are
    // how a hard edge is authored, since the two sides of a crease need
    // different normals at the same vertex.
    //
    // Authored normals belong to the control cage. After refinement they
    // describe a mesh that no longer exists -- and UsdGeomMesh gives a
    // subdivision surface its normals from the limit surface regardless -- so
    // they are not consulted then.
    const std::size_t vertexCount = entry.prototype.VertexCount();

    VtVec3fArray authoredNormals;
    HdInterpolation normalInterpolation = HdInterpolationVertex;
    bool haveAuthoredNormals = false;
    if (!subdivided) {
        for (const HdInterpolation interpolation :
             {HdInterpolationVertex, HdInterpolationVarying,
              HdInterpolationFaceVarying, HdInterpolationUniform,
              HdInterpolationConstant}) {
            for (const HdPrimvarDescriptor& descriptor :
                 GetPrimvarDescriptors(sceneDelegate, interpolation)) {
                if (descriptor.name != HdTokens->normals) {
                    continue;
                }
                if (!ExtractVectors(sceneDelegate->Get(id, descriptor.name),
                                    authoredNormals) ||
                    authoredNormals.empty()) {
                    continue;
                }
                normalInterpolation = interpolation;
                haveAuthoredNormals = true;
                break;
            }
            if (haveAuthoredNormals) {
                break;
            }
        }
    }

    // A per-vertex array, copied straight across.
    auto assignPerVertex = [&](const VtVec3fArray& source) {
        if (source.size() != vertexCount) {
            return false;
        }
        entry.prototype.normals.resize(vertexCount * 3);
        for (std::size_t i = 0; i < vertexCount; ++i) {
            entry.prototype.normals[i * 3 + 0] = source[i][0];
            entry.prototype.normals[i * 3 + 1] = source[i][1];
            entry.prototype.normals[i * 3 + 2] = source[i][2];
        }
        return true;
    };

    // One normal per triangle corner, in the triangle order the indices were
    // written in, so the kernel indexes them by primitive.
    auto assignPerCorner = [&](const VtVec3fArray& corners) {
        if (corners.size() != entry.prototype.indices.size()) {
            return false;
        }
        entry.prototype.normals.resize(corners.size() * 3);
        for (std::size_t i = 0; i < corners.size(); ++i) {
            entry.prototype.normals[i * 3 + 0] = corners[i][0];
            entry.prototype.normals[i * 3 + 1] = corners[i][1];
            entry.prototype.normals[i * 3 + 2] = corners[i][2];
        }
        entry.prototype.normalsPerCorner = true;
        return true;
    };

    bool normalsAssigned = false;
    if (haveAuthoredNormals) {
        if (normalInterpolation == HdInterpolationVertex ||
            normalInterpolation == HdInterpolationVarying) {
            normalsAssigned = assignPerVertex(authoredNormals);
        } else if (normalInterpolation == HdInterpolationFaceVarying) {
            // The same triangulation the face-varying UVs get, and for the same
            // reason: HdMeshUtil owns the corner ordering, including the
            // winding flip a left-handed mesh needs, so walking the faces here
            // would be a second chance to disagree with the indices.
            HdMeshUtil meshUtil(&topology, id);
            VtValue triangulated;
            const HdMeshComputationResult computed =
                meshUtil.ComputeTriangulatedFaceVaryingPrimvar(
                    authoredNormals.cdata(),
                    static_cast<int>(authoredNormals.size()), HdTypeFloatVec3,
                    &triangulated);

            // `Unchanged` means the mesh was already triangles and the *input*
            // is the answer; reading it as a failure would drop the normals of
            // every triangulated asset.
            if (computed == HdMeshComputationResult::Success &&
                triangulated.IsHolding<VtVec3fArray>()) {
                normalsAssigned =
                    assignPerCorner(triangulated.UncheckedGet<VtVec3fArray>());
            } else if (computed == HdMeshComputationResult::Unchanged) {
                normalsAssigned = assignPerCorner(authoredNormals);
            }
        } else if (normalInterpolation == HdInterpolationUniform) {
            // One per coarse face, expanded to that face's triangles. Held per
            // corner rather than per vertex because a vertex is shared between
            // faces that disagree, which is the whole point of a uniform
            // normal.
            if (authoredNormals.size() ==
                    static_cast<std::size_t>(topology.GetNumFaces()) &&
                coarseFaces.size() == entry.prototype.TriangleCount()) {
                VtVec3fArray corners(entry.prototype.indices.size());
                for (std::size_t t = 0; t < coarseFaces.size(); ++t) {
                    const int face = coarseFaces[t];
                    if (face < 0 || static_cast<std::size_t>(face) >=
                                        authoredNormals.size()) {
                        continue;
                    }
                    corners[t * 3 + 0] = authoredNormals[face];
                    corners[t * 3 + 1] = authoredNormals[face];
                    corners[t * 3 + 2] = authoredNormals[face];
                }
                normalsAssigned = assignPerCorner(corners);
            }
        } else if (normalInterpolation == HdInterpolationConstant) {
            entry.prototype.normals.resize(vertexCount * 3);
            for (std::size_t i = 0; i < vertexCount; ++i) {
                entry.prototype.normals[i * 3 + 0] = authoredNormals[0][0];
                entry.prototype.normals[i * 3 + 1] = authoredNormals[0][1];
                entry.prototype.normals[i * 3 + 2] = authoredNormals[0][2];
            }
            normalsAssigned = true;
        }

        if (!normalsAssigned) {
            // A set that does not describe this mesh: the wrong length for the
            // interpolation it was declared with. Generating instead is better
            // than shading from the triangle planes, but the mismatch is worth
            // saying out loud rather than absorbing silently.
            HdClaudeTrace(
                "mesh <%s>: %zu authored normals do not match the mesh "
                "(%zu vertices, %zu triangles); generating instead",
                id.GetText(), authoredNormals.size(), vertexCount,
                entry.prototype.TriangleCount());
        }
    }

    if (!normalsAssigned && subdivided) {
        // The coarse adjacency does not describe the refined cage, so normals
        // come from the refined triangles: accumulate each triangle's normal
        // at its corners, then normalise.
        entry.prototype.normals.assign(vertexCount * 3, 0.0f);
        const std::vector<float>& p = entry.prototype.positions;
        for (std::size_t t = 0; t + 2 < entry.prototype.indices.size(); t += 3) {
            const std::uint32_t a = entry.prototype.indices[t];
            const std::uint32_t b = entry.prototype.indices[t + 1];
            const std::uint32_t c = entry.prototype.indices[t + 2];
            const GfVec3f pa(p[a * 3], p[a * 3 + 1], p[a * 3 + 2]);
            const GfVec3f pb(p[b * 3], p[b * 3 + 1], p[b * 3 + 2]);
            const GfVec3f pc(p[c * 3], p[c * 3 + 1], p[c * 3 + 2]);
            // Left unnormalised: the cross product's length is twice the
            // triangle's area, which weights each face by its size and keeps a
            // sliver from dominating a vertex it barely touches.
            const GfVec3f faceNormal = GfCross(pb - pa, pc - pa);
            for (const std::uint32_t corner : {a, b, c}) {
                entry.prototype.normals[corner * 3 + 0] += faceNormal[0];
                entry.prototype.normals[corner * 3 + 1] += faceNormal[1];
                entry.prototype.normals[corner * 3 + 2] += faceNormal[2];
            }
        }
        for (std::size_t i = 0; i < vertexCount; ++i) {
            GfVec3f n(entry.prototype.normals[i * 3],
                      entry.prototype.normals[i * 3 + 1],
                      entry.prototype.normals[i * 3 + 2]);
            const float length = n.GetLength();
            if (length > 1e-12f) {
                n /= length;
            }
            entry.prototype.normals[i * 3 + 0] = n[0];
            entry.prototype.normals[i * 3 + 1] = n[1];
            entry.prototype.normals[i * 3 + 2] = n[2];
        }
    } else if (!normalsAssigned) {
        Hd_VertexAdjacency adjacency;
        adjacency.BuildAdjacencyTable(&topology);

        VtVec3fArray points3f(vertexCount);
        for (std::size_t i = 0; i < vertexCount; ++i) {
            points3f[i] = GfVec3f(entry.prototype.positions[i * 3 + 0],
                                  entry.prototype.positions[i * 3 + 1],
                                  entry.prototype.positions[i * 3 + 2]);
        }
        const VtVec3fArray smooth =
            Hd_SmoothNormals::ComputeSmoothNormals(
                &adjacency, static_cast<int>(points3f.size()), points3f.cdata());
        if (smooth.size() == vertexCount) {
            entry.prototype.normals.resize(vertexCount * 3);
            for (std::size_t i = 0; i < vertexCount; ++i) {
                entry.prototype.normals[i * 3 + 0] = smooth[i][0];
                entry.prototype.normals[i * 3 + 1] = smooth[i][1];
                entry.prototype.normals[i * 3 + 2] = smooth[i][2];
            }
        }
    }

    // --- Texture coordinates --------------------------------------------------
    // Vertex-interpolated `st` only for now. Face-varying UVs need
    // ComputeTriangulatedFaceVaryingPrimvar and a vertex split, which is
    // recorded as remaining work rather than approximated here.
    //
    // A refined mesh already has its coordinates: they were read from the
    // control cage above and refined with the positions. Re-reading them here
    // would compare a coarse array against a refined vertex count, fail, and
    // leave the mesh with none -- which is what it used to do.
    if (!subdivided) {
        if (haveUvs && uvInterpolation == HdInterpolationFaceVarying) {
            // Triangulated into one coordinate per triangle corner, in the
            // same triangle order HdMeshUtil produced the indices in, so the
            // kernel can index them by primitive without a second mapping.
            HdMeshUtil meshUtil(&topology, id);
            VtValue triangulated;
            const HdMeshComputationResult computed =
                meshUtil.ComputeTriangulatedFaceVaryingPrimvar(
                    authoredUvs.cdata(), static_cast<int>(authoredUvs.size()),
                    HdTypeFloatVec2, &triangulated);

            // `Unchanged` means the triangulation was a no-op because the mesh
            // is already triangles, and the *input* is the answer. Reading it
            // as a failure leaves an all-triangle mesh with no coordinates at
            // all, which is what the StandardShaderBall's ground and walls are:
            // two triangles each, authored face-varying, and shaded from
            // barycentrics until this branch existed.
            const VtVec2fArray* corners = nullptr;
            if (computed == HdMeshComputationResult::Success &&
                triangulated.IsHolding<VtVec2fArray>()) {
                corners = &triangulated.UncheckedGet<VtVec2fArray>();
            } else if (computed == HdMeshComputationResult::Unchanged) {
                corners = &authoredUvs;
            }

            if (corners != nullptr &&
                corners->size() == entry.prototype.indices.size()) {
                entry.prototype.uvs.resize(corners->size() * 2);
                for (std::size_t i = 0; i < corners->size(); ++i) {
                    entry.prototype.uvs[i * 2 + 0] = (*corners)[i][0];
                    entry.prototype.uvs[i * 2 + 1] = (*corners)[i][1];
                }
                entry.prototype.uvsPerCorner = true;
            }
        } else {
            entry.prototype.uvs = std::move(coarseUvs);
            if (entry.prototype.uvs.size() != vertexCount * 2) {
                entry.prototype.uvs.clear();
            }
        }
    }

    // --- Instancing ------------------------------------------------------------
    // One prototype, many placements: the acceleration structure is built once
    // and instanced, which is the whole reason geometry stays object-space.
    //
    // The placements come from hdClaude's own instancer, which composes them
    // from the instancer's primvars. `HdRprim::GetInstancerTransforms` looks
    // like the call for this and is not: it returns one matrix per instancer in
    // the parent chain -- the instancer's own transform -- so reading it as the
    // per-instance list draws a point-instanced prototype exactly once, which
    // is how the OpenChessSet lost most of its pieces.
    //
    // An instanced rprim's own transform is *not* its world transform: it
    // places the mesh within the prototype, and the instancer places the
    // prototype. Both are needed. Dropping the first collapses every mesh of a
    // model onto that model's origin, which is a failure that reads as missing
    // geometry rather than as misplaced geometry -- an asset whose parts are
    // authored a couple of hundred units from its root ends up scattered
    // around the room or inside a wall. Pixar's Kitchen Set is 1462 meshes of
    // which 1460 are placed this way, so it lost its refrigerator, its stove,
    // its table and its chairs while still reporting every instance present.
    //
    // Local first, then the placement: USD composes a row vector's transforms
    // left to right, so `mesh * instance` is the mesh taken into the
    // prototype and the prototype into the world.
    const GfMatrix4d meshTransform = sceneDelegate->GetTransform(id);
    if (GetInstancerId().IsEmpty()) {
        entry.transforms.push_back(ToTransform(meshTransform));
    } else {
        VtMatrix4dArray transforms;
        HdInstancer* instancer =
            sceneDelegate->GetRenderIndex().GetInstancer(GetInstancerId());
        if (auto* claudeInstancer = dynamic_cast<HdClaudeInstancer*>(instancer)) {
            transforms = claudeInstancer->ComputeInstanceTransforms(id);
        }
        entry.transforms.reserve(transforms.size());
        for (const GfMatrix4d& matrix : transforms) {
            entry.transforms.push_back(ToTransform(meshTransform * matrix));
        }
        if (entry.transforms.empty()) {
            // An instancer with no instances draws nothing. Publishing the
            // prim at the origin instead would put a copy on screen that the
            // stage does not contain.
            param->SceneStore()->RemoveMesh(id);
            *dirtyBits = HdChangeTracker::Clean;
            return;
        }
    }

    entry.prototype.opacity = hdclaude::OpacityClass::Opaque;

    // --- Unbound geometry -------------------------------------------------------
    // A mesh with no material binding gets a diffuse material of its own
    // displayColor, published under a property path so it cannot collide with a
    // real material prim. It goes through the same MaterialX path as everything
    // else -- there is no second shading route to keep correct -- and it keeps
    // an unshaded asset looking like itself rather than uniformly grey.
    if (entry.material.IsEmpty() && param->MaterialCompiler() != nullptr) {
        const SdfPath generated = DisplayColorMaterialPath(id);
        const GfVec3f color = DisplayColor(sceneDelegate, id);
        if (!param->SceneStore()->HasMaterial(generated) || color != _lastDisplayColor) {
            HdClaudeMaterialEntry material;
            material.compiled = param->MaterialCompiler()->CompileDiffuse(
                color, "hdclaude_" + HdMtlxCreateNameFromPath(generated));
            if (material.compiled.spirv.empty()) {
                material.fallbackReason =
                    "the generated displayColor material did not compile";
            }
            param->SceneStore()->PublishMaterial(generated, std::move(material));
            _lastDisplayColor = color;
        }
        entry.material = generated;
    }

    HdClaudeTrace("mesh <%s>%s: %zu vertices, %zu triangles, %zu instances, "
                  "normals %s, uvs %s, %zu subsets",
                  id.GetText(), subdivided ? " [subdivided]" : "",
                  entry.prototype.VertexCount(),
                  entry.prototype.TriangleCount(), entry.transforms.size(),
                  entry.prototype.normals.empty() ? "no"
                  : !normalsAssigned                ? "generated"
                  : entry.prototype.normalsPerCorner
                      ? "authored (per corner)"
                      : "authored",
                  entry.prototype.uvs.empty()
                      ? (haveUvs ? (uvInterpolation == HdInterpolationFaceVarying
                                        ? "no (face-varying, dropped)"
                                        : "no (size mismatch)")
                                 : "no (none authored)")
                      : (entry.prototype.uvsPerCorner ? "yes (per corner)" : "yes"),
                  entry.subsetMaterials.size());
    param->SceneStore()->PublishMesh(id, std::move(entry));
    *dirtyBits = HdChangeTracker::Clean;
}

PXR_NAMESPACE_CLOSE_SCOPE
