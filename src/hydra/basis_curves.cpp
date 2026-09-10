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

    // Which interpolation the vertices are authored for.
    //
    // UsdImaging's NURBS adapter reports linear/linear/nonperiodic and draws the
    // control cage rather than the evaluated curve, so a NurbsCurves prim
    // reaches every Hydra renderer as a polyline and needs no evaluation. A
    // cubic set does: its control points are not on the curve, and a B-spline's
    // are not even close to it -- a strand of hair starts a sixth of the way
    // into its own control polygon.
    hdclaude::CurveBasis basis = hdclaude::CurveBasis::Linear;
    if (topology.GetCurveType() != HdTokens->linear) {
        const TfToken& authored = topology.GetCurveBasis();
        if (authored == HdTokens->bSpline) {
            basis = hdclaude::CurveBasis::BSpline;
        } else if (authored == HdTokens->catmullRom) {
            basis = hdclaude::CurveBasis::CatmullRom;
        } else if (authored == HdTokens->bezier) {
            basis = hdclaude::CurveBasis::Bezier;
        } else {
            // Named rather than guessed at. A basis this renderer does not know
            // is not one of the three UsdGeomBasisCurves defines, and sweeping
            // it as though it were would put a curve in the picture that the
            // asset did not author.
            TF_WARN("hdClaude: curves <%s> use basis '%s', which hdClaude does "
                    "not evaluate; they are not drawn",
                    id.GetText(), authored.GetText());
            param->SceneStore()->RemoveMesh(id);
            *dirtyBits = HdChangeTracker::Clean;
            return;
        }
    }

    const VtIntArray counts = topology.GetCurveVertexCounts();
    const std::vector<int> vertexCounts(counts.begin(), counts.end());
    const TfToken& wrap = topology.GetCurveWrap();
    const bool periodic = wrap == HdTokens->periodic;

    // `pinned` is a third wrap mode, and it is not periodic with a different
    // name: it repeats phantom control points at each end so the curve reaches
    // its first and last vertex, which changes the segment count and the
    // indices every segment reads. Refused by name until it is implemented,
    // rather than drawn as a nonperiodic curve that would be short at both ends.
    if (basis != hdclaude::CurveBasis::Linear && wrap == HdTokens->pinned) {
        TF_WARN("hdClaude: curves <%s> are pinned, which hdClaude does not "
                "evaluate yet; they are not drawn",
                id.GetText());
        param->SceneStore()->RemoveMesh(id);
        *dirtyBits = HdChangeTracker::Clean;
        return;
    }

    std::string reason;

    // The cubic bases evaluated to polylines, which is what the sweep takes.
    // A linear set passes through untouched, so this is unconditional.
    const hdclaude::CurvePolylines evaluated = hdclaude::EvaluateCurves(
        vertexCounts, points, widths, basis, periodic,
        param->CurveSegmentSamples(), &reason);
    if (!evaluated.Valid()) {
        TF_WARN("hdClaude: curves <%s> were not evaluated: %s", id.GetText(),
                reason.c_str());
        param->SceneStore()->RemoveMesh(id);
        *dirtyBits = HdChangeTracker::Clean;
        return;
    }

    HdClaudeMeshEntry entry;
    entry.prototype.debugName = id.GetString();

    if (param->ImplicitCurves()) {
        // The segments themselves, intersected in the traversal kernel: no
        // triangles, no `sides`, and no facets. A strand is a cone with a
        // sphere at each end and the renderer hits that shape rather than an
        // approximation of it.
        entry.prototype.segments = hdclaude::CurveSegments(
            evaluated.vertexCounts, evaluated.points, evaluated.widths,
            kDefaultWidth, periodic, &reason);
        if (entry.prototype.segments.empty()) {
            TF_WARN("hdClaude: curves <%s> produced no segments: %s",
                    id.GetText(), reason.c_str());
            param->SceneStore()->RemoveMesh(id);
            *dirtyBits = HdChangeTracker::Clean;
            return;
        }
    } else {
        const hdclaude::CurveMesh swept = hdclaude::SweepCurves(
            evaluated.vertexCounts, evaluated.points, evaluated.widths,
            kDefaultWidth, param->CurveSides(), periodic, &reason);
        if (!swept.Valid()) {
            // Reported by name and dropped, rather than drawn as something
            // else. A curve hdClaude cannot sweep is a curve the scene should
            // hear about.
            TF_WARN("hdClaude: curves <%s> were not swept: %s", id.GetText(),
                    reason.c_str());
            param->SceneStore()->RemoveMesh(id);
            *dirtyBits = HdChangeTracker::Clean;
            return;
        }
        entry.prototype.positions = swept.positions;
        entry.prototype.indices = swept.indices;
        entry.prototype.normals = swept.normals;
        entry.prototype.normalsPerCorner = false;
        entry.prototype.uvs = swept.uvs;
        entry.prototype.uvsPerCorner = false;
    }
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
