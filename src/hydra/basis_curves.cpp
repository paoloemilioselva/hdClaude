#include "basis_curves.h"

#include "hdclaude/core/curve_sweep.h"
#include "material_compiler.h"
#include "render_param.h"
#include "scene_store.h"
#include "trace.h"

#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/vt/array.h"
#include "pxr/imaging/hd/changeTracker.h"
#include "pxr/imaging/hd/instancer.h"
#include "pxr/imaging/hd/renderIndex.h"
#include "pxr/imaging/hd/sceneDelegate.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/hdMtlx/hdMtlx.h"

#include "instancer.h"

#include <string>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

hdclaude::Transform3x4 ToTransform(const GfMatrix4d& matrix)
{
    hdclaude::Transform3x4 out;
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            out.m[row * 4 + column] = static_cast<float>(matrix[column][row]);
        }
        out.m[row * 4 + 3] = static_cast<float>(matrix[3][row]);
    }
    return out;
}

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

/// Widths, whatever precision they arrive in. An absent or unreadable array is
/// not an error: the sweep falls back to a width the caller supplies.
void ExtractWidths(const VtValue& value, std::vector<float>& out)
{
    out.clear();
    if (value.IsHolding<VtFloatArray>()) {
        const VtFloatArray& widths = value.UncheckedGet<VtFloatArray>();
        out.assign(widths.begin(), widths.end());
    } else if (value.IsHolding<VtDoubleArray>()) {
        const VtDoubleArray& widths = value.UncheckedGet<VtDoubleArray>();
        out.reserve(widths.size());
        for (const double width : widths) {
            out.push_back(static_cast<float>(width));
        }
    }
}

SdfPath DisplayColorMaterialPath(const SdfPath& id)
{
    return id.AppendProperty(TfToken("hdClaudeDisplayColor"));
}

GfVec3f DisplayColor(HdSceneDelegate* delegate, const SdfPath& id)
{
    const VtValue value = delegate->Get(id, HdTokens->displayColor);
    if (value.IsHolding<VtVec3fArray>()) {
        const VtVec3fArray& colors = value.UncheckedGet<VtVec3fArray>();
        if (!colors.empty()) {
            return colors[0];
        }
    }
    return GfVec3f(0.5f, 0.5f, 0.5f);
}

}  // namespace

HdClaudeBasisCurves::HdClaudeBasisCurves(const SdfPath& id) : HdBasisCurves(id) {}

HdClaudeBasisCurves::~HdClaudeBasisCurves() = default;

HdDirtyBits HdClaudeBasisCurves::GetInitialDirtyBitsMask() const
{
    return HdChangeTracker::DirtyTopology | HdChangeTracker::DirtyPoints |
           HdChangeTracker::DirtyWidths | HdChangeTracker::DirtyPrimvar |
           HdChangeTracker::DirtyTransform | HdChangeTracker::DirtyVisibility |
           HdChangeTracker::DirtyMaterialId | HdChangeTracker::DirtyInstancer;
}

void HdClaudeBasisCurves::_InitRepr(const TfToken& reprToken,
                                    HdDirtyBits* dirtyBits)
{
    TF_UNUSED(reprToken);
    TF_UNUSED(dirtyBits);
}

HdDirtyBits HdClaudeBasisCurves::_PropagateDirtyBits(HdDirtyBits bits) const
{
    return bits;
}

void HdClaudeBasisCurves::Finalize(HdRenderParam* renderParam)
{
    if (auto* param = dynamic_cast<HdClaudeRenderParam*>(renderParam)) {
        if (param->SceneStore() != nullptr) {
            param->SceneStore()->RemoveMesh(GetId());
            param->SceneStore()->RemoveMaterial(
                DisplayColorMaterialPath(GetId()));
        }
    }
}

void HdClaudeBasisCurves::Sync(HdSceneDelegate* sceneDelegate,
                               HdRenderParam* renderParam,
                               HdDirtyBits* dirtyBits, const TfToken& reprToken)
{
    TF_UNUSED(reprToken);
    auto* param = dynamic_cast<HdClaudeRenderParam*>(renderParam);
    if (param == nullptr || param->SceneStore() == nullptr) {
        *dirtyBits = HdChangeTracker::Clean;
        return;
    }
    const SdfPath& id = GetId();

    // The instancer this curve set belongs to, before any transform is read.
    // Exactly as for a mesh: an rprim learns its instancer only by asking, and
    // skipping this draws a point-instanced prototype once at its own
    // transform.
    _UpdateInstancer(sceneDelegate, dirtyBits);
    HdInstancer::_SyncInstancerAndParents(sceneDelegate->GetRenderIndex(),
                                          GetInstancerId());

    if (HdChangeTracker::IsDirty(*dirtyBits)) {
        SetMaterialId(sceneDelegate->GetMaterialId(id));
    }

    const HdBasisCurvesTopology topology = GetBasisCurvesTopology(sceneDelegate);

    std::vector<float> points;
    if (!ExtractPoints(sceneDelegate->Get(id, HdTokens->points), points)) {
        TF_WARN("hdClaude: curves <%s> have no readable points", id.GetText());
        param->SceneStore()->RemoveMesh(id);
        *dirtyBits = HdChangeTracker::Clean;
        return;
    }

    std::vector<float> widths;
    ExtractWidths(sceneDelegate->Get(id, HdTokens->widths), widths);

    // The fallback is the same figure UsdGeomCurves documents as its default
    // width, so a curve set that authors none is drawn at the size the
    // schema says it has rather than at one chosen here.
    constexpr float kDefaultWidth = 1.0f;

    // Only the linear basis is swept. UsdImaging's NURBS adapter reports
    // linear/linear/nonperiodic and draws the control cage rather than the
    // evaluated curve, so a NurbsCurves prim reaches every Hydra renderer as a
    // polyline; a cubic basis needs its basis matrices evaluated to a polyline
    // first, which is separate work and is refused by name rather than swept as
    // though it were linear.
    if (topology.GetCurveType() != HdTokens->linear) {
        TF_WARN("hdClaude: curves <%s> use a cubic basis, which is not "
                "evaluated yet; they are not drawn",
                id.GetText());
        param->SceneStore()->RemoveMesh(id);
        *dirtyBits = HdChangeTracker::Clean;
        return;
    }

    const VtIntArray counts = topology.GetCurveVertexCounts();
    const std::vector<int> vertexCounts(counts.begin(), counts.end());
    const bool periodic = topology.GetCurveWrap() == HdTokens->periodic;

    std::string reason;
    const hdclaude::CurveMesh swept =
        hdclaude::SweepCurves(vertexCounts, points, widths, kDefaultWidth,
                              param->CurveSides(), periodic, &reason);
    if (!swept.Valid()) {
        // Reported by name and dropped, rather than drawn as something else. A
        // curve hdClaude cannot sweep is a curve the scene should hear about.
        TF_WARN("hdClaude: curves <%s> were not swept: %s", id.GetText(),
                reason.c_str());
        param->SceneStore()->RemoveMesh(id);
        *dirtyBits = HdChangeTracker::Clean;
        return;
    }

    HdClaudeMeshEntry entry;
    entry.prototype.debugName = id.GetString();
    entry.prototype.positions = swept.positions;
    entry.prototype.indices = swept.indices;
    entry.prototype.normals = swept.normals;
    entry.prototype.normalsPerCorner = false;
    entry.prototype.uvs = swept.uvs;
    entry.prototype.uvsPerCorner = false;
    entry.prototype.opacity = hdclaude::OpacityClass::Opaque;
    entry.visible = sceneDelegate->GetVisible(id);
    entry.material = GetMaterialId();

    // Unbound curves get a diffuse material of their own displayColor, through
    // the same MaterialX path everything else uses.
    if (entry.material.IsEmpty() && param->MaterialCompiler() != nullptr) {
        const SdfPath generated = DisplayColorMaterialPath(id);
        const GfVec3f color = DisplayColor(sceneDelegate, id);
        if (!param->SceneStore()->HasMaterial(generated) ||
            color != _lastDisplayColor) {
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

    const GfMatrix4d curveTransform = sceneDelegate->GetTransform(id);
    if (GetInstancerId().IsEmpty()) {
        entry.transforms.push_back(ToTransform(curveTransform));
    } else {
        VtMatrix4dArray transforms;
        HdInstancer* instancer =
            sceneDelegate->GetRenderIndex().GetInstancer(GetInstancerId());
        if (auto* claudeInstancer = dynamic_cast<HdClaudeInstancer*>(instancer)) {
            transforms = claudeInstancer->ComputeInstanceTransforms(id);
        }
        entry.transforms.reserve(transforms.size());
        for (const GfMatrix4d& matrix : transforms) {
            entry.transforms.push_back(ToTransform(curveTransform * matrix));
        }
        if (entry.transforms.empty()) {
            param->SceneStore()->RemoveMesh(id);
            *dirtyBits = HdChangeTracker::Clean;
            return;
        }
    }

    HdClaudeTrace("curves <%s>: %zu strands -> %zu vertices, %zu triangles, "
                  "%zu instances",
                  id.GetText(), vertexCounts.size(),
                  entry.prototype.VertexCount(),
                  entry.prototype.TriangleCount(), entry.transforms.size());
    param->SceneStore()->PublishMesh(id, std::move(entry));
    *dirtyBits = HdChangeTracker::Clean;
}

PXR_NAMESPACE_CLOSE_SCOPE
