#include "mesh.h"

#include "material_compiler.h"
#include "render_param.h"
#include "scene_store.h"
#include "trace.h"

#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/gf/vec2f.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/imaging/hd/meshUtil.h"
#include "pxr/imaging/hd/smoothNormals.h"
#include "pxr/imaging/hd/vertexAdjacency.h"
#include "pxr/imaging/hd/vtBufferSource.h"
#include "pxr/imaging/hdMtlx/hdMtlx.h"

#include <algorithm>

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
    std::vector<float> points;
    if (!ExtractPoints(sceneDelegate->Get(id, HdTokens->points), points) ||
        points.empty()) {
        param->SceneStore()->RemoveMesh(id);
        *dirtyBits = HdChangeTracker::Clean;
        return;
    }

    // --- Triangulation -------------------------------------------------------
    // HdMeshUtil owns the face-varying and index bookkeeping, including the
    // coarse-face index each triangle came from, which is what per-face
    // material subsets will need.
    HdMeshUtil meshUtil(&topology, id);
    VtVec3iArray triangleIndices;
    VtIntArray primitiveParams;
    meshUtil.ComputeTriangleIndices(&triangleIndices, &primitiveParams);

    if (triangleIndices.empty()) {
        param->SceneStore()->RemoveMesh(id);
        *dirtyBits = HdChangeTracker::Clean;
        return;
    }

    entry.prototype.positions = std::move(points);
    entry.prototype.indices.reserve(triangleIndices.size() * 3);
    for (const GfVec3i& triangle : triangleIndices) {
        entry.prototype.indices.push_back(static_cast<std::uint32_t>(triangle[0]));
        entry.prototype.indices.push_back(static_cast<std::uint32_t>(triangle[1]));
        entry.prototype.indices.push_back(static_cast<std::uint32_t>(triangle[2]));
    }

    // --- Normals -------------------------------------------------------------
    // Authored normals if the mesh has vertex-interpolated ones; otherwise
    // smooth normals from the public adjacency API. Generating them rather than
    // falling back to flat shading matters for the shader balls, whose
    // silhouettes are the whole point of the asset.
    std::vector<float> normals;
    const VtValue normalsValue = sceneDelegate->Get(id, HdTokens->normals);
    const std::size_t vertexCount = entry.prototype.VertexCount();
    if (ExtractPoints(normalsValue, normals) &&
        normals.size() == vertexCount * 3) {
        entry.prototype.normals = std::move(normals);
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
    for (const TfToken& name : {TfToken("st"), TfToken("uv")}) {
        const VtValue uvValue = sceneDelegate->Get(id, name);
        if (uvValue.IsHolding<VtVec2fArray>()) {
            const VtVec2fArray& uvs = uvValue.UncheckedGet<VtVec2fArray>();
            if (uvs.size() == vertexCount) {
                entry.prototype.uvs.resize(vertexCount * 2);
                for (std::size_t i = 0; i < uvs.size(); ++i) {
                    entry.prototype.uvs[i * 2 + 0] = uvs[i][0];
                    entry.prototype.uvs[i * 2 + 1] = uvs[i][1];
                }
                break;
            }
        }
    }

    // --- Instancing ------------------------------------------------------------
    // One prototype, many placements: the acceleration structure is built once
    // and instanced, which is the whole reason geometry stays object-space.
    //
    // HdRprim::GetInstancerTransforms already composes the instancer chain with
    // this prim's own transform, so a non-instanced mesh and an instanced one
    // differ only in how many matrices come back -- there is no second path to
    // keep correct.
    if (GetInstancerId().IsEmpty()) {
        entry.transforms.push_back(ToTransform(sceneDelegate->GetTransform(id)));
    } else {
        const VtMatrix4dArray transforms = GetInstancerTransforms(sceneDelegate);
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

    HdClaudeTrace("mesh <%s>: %zu vertices, %zu triangles, %zu instances, "
                  "normals %s, uvs %s",
                  id.GetText(), entry.prototype.VertexCount(),
                  entry.prototype.TriangleCount(), entry.transforms.size(),
                  entry.prototype.normals.empty() ? "no" : "yes",
                  entry.prototype.uvs.empty() ? "no" : "yes");
    param->SceneStore()->PublishMesh(id, std::move(entry));
    *dirtyBits = HdChangeTracker::Clean;
}

PXR_NAMESPACE_CLOSE_SCOPE
