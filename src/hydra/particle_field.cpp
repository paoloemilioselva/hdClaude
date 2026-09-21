#include "particle_field.h"

#include "hdclaude/core/gaussian_splats.h"
#include "instancer.h"
#include "render_param.h"
#include "scene_store.h"
#include "trace.h"

#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/gf/quatd.h"
#include "pxr/base/gf/quatf.h"
#include "pxr/base/gf/quath.h"
#include "pxr/base/gf/vec3d.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/gf/vec3h.h"
#include "pxr/base/tf/staticTokens.h"
#include "pxr/base/vt/array.h"
#include "pxr/imaging/hd/changeTracker.h"
#include "pxr/imaging/hd/instancer.h"
#include "pxr/imaging/hd/renderIndex.h"
#include "pxr/imaging/hd/sceneDelegate.h"
#include "pxr/imaging/hd/tokens.h"

#include <string>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

// The attribute names UsdVolParticleField declares. Spelled here rather than
// taken from `UsdVolTokens` because a render delegate is below usdImaging and
// must not link the schema library to read a primvar by name -- and because the
// names are the interface: if a future schema revision renames one, this is the
// list that has to change.
TF_DEFINE_PRIVATE_TOKENS(
    _tokens,
    (positions)(positionsh)(orientations)(orientationsh)(scales)(scalesh)(
        opacities)(opacitiesh)(projectionModeHint)(sortingModeHint)
    ((harmonics, "radiance:sphericalHarmonicsCoefficients"))
    ((harmonicsh, "radiance:sphericalHarmonicsCoefficientsh"))
    ((harmonicsDegree, "radiance:sphericalHarmonicsDegree")));

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

/// Three floats per element, whichever precision the array arrived in.
///
/// The schema declares every per-particle attribute in a `float` and a `half`
/// flavour and says a consumer should prefer the float one; the preference is
/// applied by the caller, which asks for the float name first. Reading both
/// here is what makes that preference expressible rather than assumed -- and
/// `double` is accepted too, because `point3d` is a legal spelling of a
/// position and refusing it would be refusing the scene over a precision.
bool ReadVec3Array(const VtValue& value, std::vector<float>& out)
{
    const auto take = [&out](auto&& array) {
        out.resize(array.size() * 3);
        for (std::size_t i = 0; i < array.size(); ++i) {
            out[i * 3 + 0] = static_cast<float>(array[i][0]);
            out[i * 3 + 1] = static_cast<float>(array[i][1]);
            out[i * 3 + 2] = static_cast<float>(array[i][2]);
        }
    };
    if (value.IsHolding<VtVec3fArray>()) {
        take(value.UncheckedGet<VtVec3fArray>());
        return true;
    }
    if (value.IsHolding<VtVec3hArray>()) {
        take(value.UncheckedGet<VtVec3hArray>());
        return true;
    }
    if (value.IsHolding<VtVec3dArray>()) {
        take(value.UncheckedGet<VtVec3dArray>());
        return true;
    }
    return false;
}

/// Four floats per element, real part first.
///
/// The order is explicit because `GfQuat` stores its imaginary part first and
/// its real part last, and the core layer documents (w, x, y, z): a
/// reinterpreting copy would rotate every particle by something else.
bool ReadQuatArray(const VtValue& value, std::vector<float>& out)
{
    const auto take = [&out](auto&& array) {
        out.resize(array.size() * 4);
        for (std::size_t i = 0; i < array.size(); ++i) {
            const auto imaginary = array[i].GetImaginary();
            out[i * 4 + 0] = static_cast<float>(array[i].GetReal());
            out[i * 4 + 1] = static_cast<float>(imaginary[0]);
            out[i * 4 + 2] = static_cast<float>(imaginary[1]);
            out[i * 4 + 3] = static_cast<float>(imaginary[2]);
        }
    };
    if (value.IsHolding<VtQuatfArray>()) {
        take(value.UncheckedGet<VtQuatfArray>());
        return true;
    }
    if (value.IsHolding<VtQuathArray>()) {
        take(value.UncheckedGet<VtQuathArray>());
        return true;
    }
    if (value.IsHolding<VtQuatdArray>()) {
        take(value.UncheckedGet<VtQuatdArray>());
        return true;
    }
    return false;
}

/// One float per element.
bool ReadFloatArray(const VtValue& value, std::vector<float>& out)
{
    const auto take = [&out](auto&& array) {
        out.assign(array.size(), 0.0f);
        for (std::size_t i = 0; i < array.size(); ++i) {
            out[i] = static_cast<float>(array[i]);
        }
    };
    if (value.IsHolding<VtFloatArray>()) {
        take(value.UncheckedGet<VtFloatArray>());
        return true;
    }
    if (value.IsHolding<VtHalfArray>()) {
        take(value.UncheckedGet<VtHalfArray>());
        return true;
    }
    if (value.IsHolding<VtDoubleArray>()) {
        take(value.UncheckedGet<VtDoubleArray>());
        return true;
    }
    return false;
}

const char* InterpolationName(HdInterpolation interpolation)
{
    switch (interpolation) {
        case HdInterpolationConstant:
            return "constant";
        case HdInterpolationUniform:
            return "uniform";
        case HdInterpolationVarying:
            return "varying";
        case HdInterpolationVertex:
            return "vertex";
        case HdInterpolationFaceVarying:
            return "faceVarying";
        case HdInterpolationInstance:
            return "instance";
        default:
            break;
    }
    return "unknown";
}

}  // namespace

HdClaudeParticleField::HdClaudeParticleField(const SdfPath& id) : HdRprim(id) {}

HdClaudeParticleField::~HdClaudeParticleField() = default;

HdDirtyBits HdClaudeParticleField::GetInitialDirtyBitsMask() const
{
    // The splat arrays are not `points` and not `topology`: Hydra has no schema
    // for them, so they arrive as primvars and change under DirtyPrimvar.
    // DirtyPoints is asked for as well because a scene index is free to invalidate
    // a position array under it, and asking for a bit that is never set costs
    // nothing while missing one that is costs a frame of stale geometry.
    return HdChangeTracker::DirtyPrimvar | HdChangeTracker::DirtyPoints |
           HdChangeTracker::DirtyTransform | HdChangeTracker::DirtyVisibility |
           HdChangeTracker::DirtyExtent | HdChangeTracker::DirtyMaterialId |
           HdChangeTracker::DirtyInstancer | HdChangeTracker::DirtyCategories;
}

TfTokenVector const& HdClaudeParticleField::GetBuiltinPrimvarNames() const
{
    // Both flavours of every per-particle attribute. A name declared here is
    // one Hydra will not also report as a user primvar, which keeps the data
    // out of whatever generic primvar handling a scene index applies.
    static const TfTokenVector names = {
        _tokens->positions,     _tokens->positionsh,
        _tokens->orientations,  _tokens->orientationsh,
        _tokens->scales,        _tokens->scalesh,
        _tokens->opacities,     _tokens->opacitiesh,
        _tokens->harmonics,     _tokens->harmonicsh,
        _tokens->harmonicsDegree};
    return names;
}

void HdClaudeParticleField::_InitRepr(const TfToken& reprToken,
                                      HdDirtyBits* dirtyBits)
{
    TF_UNUSED(reprToken);
    TF_UNUSED(dirtyBits);
}

HdDirtyBits HdClaudeParticleField::_PropagateDirtyBits(HdDirtyBits bits) const
{
    return bits;
}

void HdClaudeParticleField::Finalize(HdRenderParam* renderParam)
{
    if (auto* param = dynamic_cast<HdClaudeRenderParam*>(renderParam)) {
        if (param->SceneStore() != nullptr) {
            param->SceneStore()->RemoveSplats(GetId());
        }
    }
}

void HdClaudeParticleField::Sync(HdSceneDelegate* sceneDelegate,
                                 HdRenderParam* renderParam,
                                 HdDirtyBits* dirtyBits,
                                 const TfToken& reprToken)
{
    TF_UNUSED(reprToken);
    auto* param = dynamic_cast<HdClaudeRenderParam*>(renderParam);
    if (param == nullptr || param->SceneStore() == nullptr) {
        *dirtyBits = HdChangeTracker::Clean;
        return;
    }
    const SdfPath& id = GetId();

    _UpdateInstancer(sceneDelegate, dirtyBits);
    HdInstancer::_SyncInstancerAndParents(sceneDelegate->GetRenderIndex(),
                                          GetInstancerId());

    if (HdChangeTracker::IsDirty(*dirtyBits)) {
        SetMaterialId(sceneDelegate->GetMaterialId(id));
    }

    // What the prim actually carries, once per prim.
    //
    // Hydra declares no schema for particle field data, so how the arrays reach
    // a delegate is a property of `UsdImagingParticleFieldAdapter` rather than
    // of a documented interface. This is how that gets answered by measurement
    // instead of by assumption -- and it stays useful afterwards, because an
    // asset whose attributes never arrive is otherwise silent.
    if (!_described) {
        _described = true;
        for (const HdInterpolation interpolation :
             {HdInterpolationConstant, HdInterpolationUniform,
              HdInterpolationVarying, HdInterpolationVertex,
              HdInterpolationFaceVarying, HdInterpolationInstance}) {
            for (const HdPrimvarDescriptor& descriptor :
                 GetPrimvarDescriptors(sceneDelegate, interpolation)) {
                HdClaudeTrace("particleField <%s>: primvar '%s' (%s, role '%s')",
                              id.GetText(), descriptor.name.GetText(),
                              InterpolationName(interpolation),
                              descriptor.role.GetText());
            }
        }
    }

    // Each datum, float flavour first, exactly as the schema's own
    // `UsesFloatPositions` and its siblings resolve it: the float attribute
    // wins where it is authored and non-empty, and the half one is read only
    // when it is not.
    const auto readVec3 = [&](const TfToken& wide, const TfToken& narrow,
                              std::vector<float>& out) {
        if (ReadVec3Array(sceneDelegate->Get(id, wide), out) && !out.empty()) {
            return;
        }
        ReadVec3Array(sceneDelegate->Get(id, narrow), out);
    };

    hdclaude::SplatCloudSource source;
    readVec3(_tokens->positions, _tokens->positionsh, source.positions);
    readVec3(_tokens->scales, _tokens->scalesh, source.scales);
    readVec3(_tokens->harmonics, _tokens->harmonicsh, source.sphericalHarmonics);

    if (!ReadQuatArray(sceneDelegate->Get(id, _tokens->orientations),
                       source.orientations) ||
        source.orientations.empty()) {
        ReadQuatArray(sceneDelegate->Get(id, _tokens->orientationsh),
                      source.orientations);
    }
    if (!ReadFloatArray(sceneDelegate->Get(id, _tokens->opacities),
                        source.opacities) ||
        source.opacities.empty()) {
        ReadFloatArray(sceneDelegate->Get(id, _tokens->opacitiesh),
                       source.opacities);
    }

    const VtValue degree = sceneDelegate->Get(id, _tokens->harmonicsDegree);
    if (degree.IsHolding<int>()) {
        source.sphericalHarmonicsDegree = degree.UncheckedGet<int>();
    }

    // The 3DGS prim applies `ParticleFieldKernelGaussianEllipsoidAPI`, and the
    // kernel is carried by which applied schema is present rather than by an
    // attribute. A delegate cannot read applied schemas -- Hydra does not
    // forward them -- so the ellipsoid is what a `particleField` is taken to
    // be. The two surflet kernels are declared by UsdVol and are not part of
    // this prim type; supporting them needs the kernel to reach Hydra, which is
    // the thing to raise upstream rather than guess at here.
    source.kernel = hdclaude::SplatKernel::GaussianEllipsoid;

    if (source.positions.empty()) {
        // Named and dropped. `ParticleFieldPositionBaseAPI` says a field with
        // no positions has no particles, so an empty one is legal -- but a prim
        // that authored positions hdClaude could not read is not, and the two
        // are indistinguishable from here, so both are said.
        TF_WARN(
            "hdClaude: particleField <%s> has no readable positions. The "
            "schema's `positions` (or `positionsh`) defines how many particles "
            "a field has, so it renders nothing.",
            id.GetText());
        param->SceneStore()->RemoveSplats(id);
        *dirtyBits = HdChangeTracker::Clean;
        return;
    }

    HdClaudeSplatEntry entry;
    entry.prototype.cloud = hdclaude::BuildSplatCloud(source);
    entry.prototype.debugName = id.GetString();
    entry.reports = entry.prototype.cloud.reports;
    entry.visible = sceneDelegate->GetVisible(id);

    if (!entry.prototype.cloud.Valid()) {
        TF_WARN(
            "hdClaude: particleField <%s> resolved to no particles from %zu "
            "authored positions.",
            id.GetText(), source.positions.size() / 3);
        param->SceneStore()->RemoveSplats(id);
        *dirtyBits = HdChangeTracker::Clean;
        return;
    }

    // A material bound to the prim. Reported rather than applied: the schema
    // carries radiance and no reflectance, so there is nothing for a surface
    // shader to shade, and a MaterialX `volumeshader` -- which *would* have
    // something to say about a splat cloud -- is a terminal hdClaude does not
    // compile yet (docs/gaussian-splats.md 4).
    if (!GetMaterialId().IsEmpty()) {
        entry.reports.push_back(
            "a material is bound at <" + GetMaterialId().GetString() +
            ">, and a Gaussian splat carries its own radiance as spherical "
            "harmonics with no reflectance to shade; the binding is not applied");
    }

    // The two rendering hints. Both are documented as tuning a renderer is free
    // to ignore, and both describe a projection-and-sort pipeline a ray tracer
    // does not have: the kernel is intersected where it is, and the nearest
    // accepted particle comes out of traversal rather than out of a sort. Said
    // once so an asset that set them knows they had no effect.
    const VtValue projection = sceneDelegate->Get(id, _tokens->projectionModeHint);
    if (projection.IsHolding<TfToken>()) {
        entry.reports.push_back(
            "projectionModeHint is '" +
            projection.UncheckedGet<TfToken>().GetString() +
            "', which hdClaude neither honours nor approximates: a path tracer "
            "intersects the 3D kernel rather than projecting it");
    }
    const VtValue sorting = sceneDelegate->Get(id, _tokens->sortingModeHint);
    if (sorting.IsHolding<TfToken>()) {
        entry.reports.push_back(
            "sortingModeHint is '" + sorting.UncheckedGet<TfToken>().GetString() +
            "', which hdClaude has nothing to apply it to: particles are not "
            "sorted, and the nearest one comes out of traversal");
    }

    const GfMatrix4d transform = sceneDelegate->GetTransform(id);
    if (GetInstancerId().IsEmpty()) {
        entry.transforms.push_back(ToTransform(transform));
    } else {
        VtMatrix4dArray transforms;
        HdInstancer* instancer =
            sceneDelegate->GetRenderIndex().GetInstancer(GetInstancerId());
        if (auto* claudeInstancer = dynamic_cast<HdClaudeInstancer*>(instancer)) {
            transforms = claudeInstancer->ComputeInstanceTransforms(id);
        }
        entry.transforms.reserve(transforms.size());
        for (const GfMatrix4d& matrix : transforms) {
            entry.transforms.push_back(ToTransform(transform * matrix));
        }
        if (entry.transforms.empty()) {
            param->SceneStore()->RemoveSplats(id);
            *dirtyBits = HdChangeTracker::Clean;
            return;
        }
    }

    entry.instanceCategories = HdClaudeRprimCategories(
        sceneDelegate, id, GetInstancerId(), entry.transforms.size());

    const hdclaude::SplatCloud& cloud = entry.prototype.cloud;
    HdClaudeTrace(
        "particleField <%s>: %zu particles, SH degree %d (%zu coefficients "
        "each), bounds (%.4f %.4f %.4f) to (%.4f %.4f %.4f), %zu instances",
        id.GetText(), cloud.Count(), cloud.sphericalHarmonicsDegree,
        cloud.CoefficientsPerParticle(), cloud.boundsMin[0], cloud.boundsMin[1],
        cloud.boundsMin[2], cloud.boundsMax[0], cloud.boundsMax[1],
        cloud.boundsMax[2], entry.transforms.size());

    param->SceneStore()->PublishSplats(id, std::move(entry));
    *dirtyBits = HdChangeTracker::Clean;
}

PXR_NAMESPACE_CLOSE_SCOPE
