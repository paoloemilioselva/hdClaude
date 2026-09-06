#include "instancer.h"

#include "trace.h"

#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/gf/quatd.h"
#include "pxr/base/gf/quath.h"
#include "pxr/base/gf/rotation.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/gf/vec4f.h"
#include "pxr/imaging/hd/sceneDelegate.h"
#include "pxr/imaging/hd/tokens.h"

PXR_NAMESPACE_OPEN_SCOPE

HdClaudeInstancer::HdClaudeInstancer(HdSceneDelegate* delegate, const SdfPath& id)
    : HdInstancer(delegate, id)
{
}

HdClaudeInstancer::~HdClaudeInstancer() = default;

void HdClaudeInstancer::Sync(HdSceneDelegate* sceneDelegate,
                             HdRenderParam* /*renderParam*/,
                             HdDirtyBits* dirtyBits)
{
    // The parent chain first: a nested instancer's transforms are composed
    // from its parent's, so the parent has to be synced before this one is
    // asked for anything.
    _UpdateInstancer(sceneDelegate, dirtyBits);

    if (HdChangeTracker::IsAnyPrimvarDirty(*dirtyBits, GetId())) {
        _SyncPrimvars(sceneDelegate, *dirtyBits);
    }
}

void HdClaudeInstancer::_SyncPrimvars(HdSceneDelegate* delegate,
                                      HdDirtyBits dirtyBits)
{
    const SdfPath& id = GetId();
    for (const HdPrimvarDescriptor& descriptor :
         delegate->GetPrimvarDescriptors(id, HdInterpolationInstance)) {
        if (!HdChangeTracker::IsPrimvarDirty(dirtyBits, id, descriptor.name)) {
            continue;
        }
        const VtValue value = delegate->Get(id, descriptor.name);
        if (!value.IsEmpty()) {
            _primvars[descriptor.name] = value;
        }
    }
}

namespace {

/// A rotation primvar, whichever of the two authored forms it arrives in.
///
/// USD authors instancer orientations as quaternions; older assets and some
/// exporters carry a `vec4`. Both mean the same rotation, and refusing one of
/// them silently drops every instance's orientation while leaving its position
/// correct -- which reads as an asset with badly modelled pieces rather than as
/// a renderer that ignored a primvar.
bool RotationAt(const VtValue& value, std::size_t index, GfQuatd* out)
{
    if (value.IsHolding<VtQuathArray>()) {
        const VtQuathArray& rotations = value.UncheckedGet<VtQuathArray>();
        if (index >= rotations.size()) return false;
        const GfQuath& q = rotations[index];
        *out = GfQuatd(q.GetReal(), GfVec3d(q.GetImaginary()));
        return true;
    }
    if (value.IsHolding<VtQuatfArray>()) {
        const VtQuatfArray& rotations = value.UncheckedGet<VtQuatfArray>();
        if (index >= rotations.size()) return false;
        const GfQuatf& q = rotations[index];
        *out = GfQuatd(q.GetReal(), GfVec3d(q.GetImaginary()));
        return true;
    }
    if (value.IsHolding<VtQuatdArray>()) {
        const VtQuatdArray& rotations = value.UncheckedGet<VtQuatdArray>();
        if (index >= rotations.size()) return false;
        *out = rotations[index];
        return true;
    }
    if (value.IsHolding<VtVec4fArray>()) {
        // (real, imaginary) in USD's authored order.
        const VtVec4fArray& rotations = value.UncheckedGet<VtVec4fArray>();
        if (index >= rotations.size()) return false;
        const GfVec4f& r = rotations[index];
        *out = GfQuatd(r[0], GfVec3d(r[1], r[2], r[3]));
        return true;
    }
    return false;
}

}  // namespace

VtMatrix4dArray HdClaudeInstancer::ComputeInstanceTransforms(
    const SdfPath& prototypeId)
{
    HdSceneDelegate* delegate = GetDelegate();
    const SdfPath& id = GetId();

    const GfMatrix4d instancerTransform = delegate->GetInstancerTransform(id);
    const VtIntArray indices = delegate->GetInstanceIndices(id, prototypeId);

    VtMatrix4dArray transforms;
    transforms.reserve(indices.size());

    for (const int index : indices) {
        const auto at = static_cast<std::size_t>(index);
        GfMatrix4d transform = instancerTransform;

        const auto translate = _primvars.find(HdInstancerTokens->instanceTranslations);
        if (translate != _primvars.end() &&
            translate->second.IsHolding<VtVec3fArray>()) {
            const VtVec3fArray& values =
                translate->second.UncheckedGet<VtVec3fArray>();
            if (at < values.size()) {
                GfMatrix4d step(1.0);
                step.SetTranslate(GfVec3d(values[at]));
                transform = step * transform;
            }
        }

        const auto rotate = _primvars.find(HdInstancerTokens->instanceRotations);
        if (rotate != _primvars.end()) {
            GfQuatd rotation;
            if (RotationAt(rotate->second, at, &rotation)) {
                GfMatrix4d step(1.0);
                step.SetRotate(rotation);
                transform = step * transform;
            }
        }

        const auto scale = _primvars.find(HdInstancerTokens->instanceScales);
        if (scale != _primvars.end() && scale->second.IsHolding<VtVec3fArray>()) {
            const VtVec3fArray& values = scale->second.UncheckedGet<VtVec3fArray>();
            if (at < values.size()) {
                GfMatrix4d step(1.0);
                step.SetScale(GfVec3d(values[at]));
                transform = step * transform;
            }
        }

        const auto instanced = _primvars.find(HdInstancerTokens->instanceTransforms);
        if (instanced != _primvars.end() &&
            instanced->second.IsHolding<VtMatrix4dArray>()) {
            const VtMatrix4dArray& values =
                instanced->second.UncheckedGet<VtMatrix4dArray>();
            if (at < values.size()) {
                transform = values[at] * transform;
            }
        }

        transforms.push_back(transform);
    }

    // Nested instancing multiplies: this instancer's own placements, each
    // repeated once per placement of the instancer itself.
    if (GetParentId().IsEmpty()) {
        return transforms;
    }

    HdInstancer* parent =
        delegate->GetRenderIndex().GetInstancer(GetParentId());
    auto* claudeParent = dynamic_cast<HdClaudeInstancer*>(parent);
    if (claudeParent == nullptr) {
        return transforms;
    }

    const VtMatrix4dArray parentTransforms =
        claudeParent->ComputeInstanceTransforms(id);

    VtMatrix4dArray composed;
    composed.reserve(parentTransforms.size() * transforms.size());
    for (const GfMatrix4d& outer : parentTransforms) {
        for (const GfMatrix4d& inner : transforms) {
            composed.push_back(inner * outer);
        }
    }
    return composed;
}

PXR_NAMESPACE_CLOSE_SCOPE
