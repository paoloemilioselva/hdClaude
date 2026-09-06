#include "mesh.h"

#include "instancer.h"

#include "material_compiler.h"
#include "render_param.h"
#include "scene_store.h"
#include "subdivision.h"
#include "trace.h"

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
           HdChangeTracker::DirtyMaterialId | HdChangeTracker::DirtyDisplayStyle;
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

    const HdMeshTopology topology = GetMeshTopology(sceneDelegate);

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
        const HdClaudeRefinedMesh refined = HdClaudeSubdivide(
            topology, points, subdivisionLevel, coarseUvs, coarseFaceVaryingUvs);
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
    // Authored normals if the mesh has vertex-interpolated ones; otherwise
    // smooth normals from the public adjacency API. Generating them rather than
    // falling back to flat shading matters for the shader balls, whose
    // silhouettes are the whole point of the asset.
    std::vector<float> normals;
    // Authored normals belong to the control cage. After refinement they
    // describe a mesh that no longer exists, so they are not consulted.
    const VtValue normalsValue =
        subdivided ? VtValue() : sceneDelegate->Get(id, HdTokens->normals);
    const std::size_t vertexCount = entry.prototype.VertexCount();
    if (ExtractPoints(normalsValue, normals) &&
        normals.size() == vertexCount * 3) {
        entry.prototype.normals = std::move(normals);
    } else if (subdivided) {
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
    } else {
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
    if (GetInstancerId().IsEmpty()) {
        entry.transforms.push_back(ToTransform(sceneDelegate->GetTransform(id)));
    } else {
        VtMatrix4dArray transforms;
        HdInstancer* instancer =
            sceneDelegate->GetRenderIndex().GetInstancer(GetInstancerId());
        if (auto* claudeInstancer = dynamic_cast<HdClaudeInstancer*>(instancer)) {
            transforms = claudeInstancer->ComputeInstanceTransforms(id);
        }
        entry.transforms.reserve(transforms.size());
        for (const GfMatrix4d& matrix : transforms) {
            entry.transforms.push_back(ToTransform(matrix));
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
                  entry.prototype.normals.empty() ? "no" : "yes",
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
