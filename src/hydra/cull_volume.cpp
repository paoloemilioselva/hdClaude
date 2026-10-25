#include "cull_volume.h"

#include "render_param.h"
#include "scene_store.h"
#include "trace.h"

#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/tf/diagnostic.h"
#include "pxr/base/tf/staticTokens.h"
#include "pxr/base/tf/stringUtils.h"
#include "pxr/imaging/hd/collectionsSchema.h"
#include "pxr/imaging/hd/purposeSchema.h"
#include "pxr/imaging/hd/renderIndex.h"
#include "pxr/imaging/hd/sceneDelegate.h"
#include "pxr/imaging/hd/sceneIndex.h"
#include "pxr/imaging/hd/tokens.h"

#include <cmath>
#include <string>

PXR_NAMESPACE_OPEN_SCOPE

// The names HdCullVolumeSchema publishes (extensions/cullVolume/
// usdImagingCullVolume/hdSchemaDefs.py), spelled here because hdClaude reads
// them without linking that library -- as it reads UsdVol's particle names.
// If the schema renames one, this is the list that has to change.
TF_DEFINE_PRIVATE_TOKENS(
    _tokens,
    (cullVolume)(shape)(size)(radius)(axis)(invert)(affectsPrimaryRays)(
        affectsSecondaryRays)(cube)(sphere)(plane)(X)(Y)(Z)(cullLink));

namespace {

template <typename T>
bool ReadValue(const HdContainerDataSourceHandle& container, const TfToken& name,
               T* value)
{
    if (!container) {
        return false;
    }
    auto source =
        HdTypedSampledDataSource<T>::Cast(container->Get(name));
    if (!source) {
        return false;
    }
    *value = source->GetTypedValue(0.0f);
    return true;
}

/// Row-major 3x4 rows of `matrix` read as a row-vector transform, which is how
/// GfMatrix4d applies to a point: p' = p * M.
void StoreRows(const GfMatrix4d& matrix, float rows[12])
{
    for (int column = 0; column < 3; ++column) {
        for (int row = 0; row < 4; ++row) {
            rows[column * 4 + row] = static_cast<float>(matrix[row][column]);
        }
    }
}

/// A volume that contains nothing: every point maps to (2, 2, 2), outside the
/// unit cube and the unit sphere, and every direction to zero, so no ray is
/// ever inside it. Unlike CullShape::None it still takes part, which matters
/// when it is inverted -- an inverted volume of size zero keeps nothing.
void StoreNowhere(float rows[12])
{
    for (int i = 0; i < 12; ++i) {
        rows[i] = 0.0f;
    }
    rows[3] = rows[7] = rows[11] = 2.0f;
}

}  // namespace

HdClaudeCullVolume::HdClaudeCullVolume(const SdfPath& id) : HdSprim(id) {}

HdClaudeCullVolume::~HdClaudeCullVolume() = default;

HdDirtyBits HdClaudeCullVolume::GetInitialDirtyBitsMask() const
{
    return HdChangeTracker::AllDirty;
}

void HdClaudeCullVolume::Sync(HdSceneDelegate* sceneDelegate,
                              HdRenderParam* renderParam, HdDirtyBits* dirtyBits)
{
    auto* param = static_cast<HdClaudeRenderParam*>(renderParam);
    HdClaudeSceneStore* store = param ? param->SceneStore() : nullptr;
    const SdfPath& id = GetId();
    if (!store || !sceneDelegate) {
        *dirtyBits = HdChangeTracker::Clean;
        return;
    }

    HdSceneIndexBaseRefPtr sceneIndex =
        sceneDelegate->GetRenderIndex().GetTerminalSceneIndex();
    HdContainerDataSourceHandle prim;
    if (sceneIndex) {
        prim = sceneIndex->GetPrim(id).dataSource;
    }
    HdContainerDataSourceHandle schema =
        prim ? HdContainerDataSource::Cast(prim->Get(_tokens->cullVolume))
             : nullptr;

    HdClaudeCullVolumeEntry entry;
    std::vector<std::string> problems;

    // --- Active ---------------------------------------------------------------
    //
    // Visible, and of a purpose the render draws. Which purposes those are is
    // the render pass's to say, from its render tags, as it says it for the
    // geometry (HdClaudeDrawSelection); a guide or proxy volume is active
    // exactly when guide or proxy gprims are drawn.
    if (HdTokenDataSourceHandle source =
            HdPurposeSchema::GetFromParent(prim).GetPurpose()) {
        entry.purpose = source->GetTypedValue(0.0f);
    }
    entry.visible = sceneDelegate->GetVisible(id);

    // --- Behaviour -------------------------------------------------------------
    bool affectsPrimary = true;
    bool affectsSecondary = true;
    bool invert = false;
    ReadValue(schema, _tokens->affectsPrimaryRays, &affectsPrimary);
    ReadValue(schema, _tokens->affectsSecondaryRays, &affectsSecondary);
    ReadValue(schema, _tokens->invert, &invert);
    entry.volume.flags = (affectsPrimary ? hdclaude::kCullAffectsPrimary : 0u) |
                         (affectsSecondary ? hdclaude::kCullAffectsSecondary : 0u) |
                         (invert ? hdclaude::kCullInvert : 0u);

    // --- Linking ----------------------------------------------------------------
    //
    // UsdImaging's CollectionAPI adapter publishes `cullLink` as a membership
    // expression: `//` for the schema's default, and an empty one -- which
    // matches nothing -- for a collection that includes nothing. Only a
    // collection that was not published at all falls back to the default.
    if (HdPathExpressionDataSourceHandle expression =
            HdCollectionsSchema::GetFromParent(prim)
                .GetCollection(_tokens->cullLink)
                .GetMembershipExpression()) {
        entry.cullLink = expression->GetTypedValue(0.0f);
        entry.linksEverything =
            entry.cullLink.GetText() == SdfPathExpression::Everything().GetText();
    }

    // --- Shape and placement ------------------------------------------------------
    TfToken shape;
    ReadValue(schema, _tokens->shape, &shape);
    const GfMatrix4d localToWorld = sceneDelegate->GetTransform(id);
    double determinant = 0.0;
    const GfMatrix4d worldToLocal = localToWorld.GetInverse(&determinant);
    const bool invertible = std::abs(determinant) > 1.0e-30;

    double extent = 1.0;
    if (shape == _tokens->cube) {
        double size = 2.0;
        ReadValue(schema, _tokens->size, &size);
        entry.volume.shape = hdclaude::CullShape::Cube;
        extent = 0.5 * size;
        if (size < 0.0) {
            problems.push_back(TfStringPrintf("size is %g, and a size cannot be "
                                              "negative",
                                              size));
        }
    } else if (shape == _tokens->sphere) {
        double radius = 1.0;
        ReadValue(schema, _tokens->radius, &radius);
        entry.volume.shape = hdclaude::CullShape::Sphere;
        extent = radius;
        if (radius < 0.0) {
            problems.push_back(TfStringPrintf("radius is %g, and a radius cannot "
                                              "be negative",
                                              radius));
        }
    } else if (shape == _tokens->plane) {
        TfToken axis = _tokens->Z;
        ReadValue(schema, _tokens->axis, &axis);
        entry.volume.shape = hdclaude::CullShape::Plane;
        if (axis == _tokens->X) {
            entry.volume.axis = 0;
        } else if (axis == _tokens->Y) {
            entry.volume.axis = 1;
        } else if (axis == _tokens->Z) {
            entry.volume.axis = 2;
        } else {
            problems.push_back(TfStringPrintf(
                "axis is \"%s\", and only X, Y and Z are axes", axis.GetText()));
        }
    } else {
        problems.push_back(TfStringPrintf(
            "its shape \"%s\" is not one hdClaude knows (cube, sphere, plane)",
            shape.GetText()));
    }
    if (!invertible) {
        problems.push_back("its transform is singular");
    }

    if (!problems.empty()) {
        // Reported and ignored, never corrected (README semantics rule 5).
        entry.volume.shape = hdclaude::CullShape::None;
        StoreNowhere(entry.volume.worldToUnit);
    } else if (extent == 0.0) {
        // A valid volume of no size: it contains nothing.
        StoreNowhere(entry.volume.worldToUnit);
    } else {
        GfMatrix4d toUnit = worldToLocal;
        if (entry.volume.shape != hdclaude::CullShape::Plane) {
            GfMatrix4d scale(1.0);
            scale.SetScale(1.0 / extent);
            toUnit = worldToLocal * scale;
        }
        StoreRows(toUnit, entry.volume.worldToUnit);
    }

    std::string report;
    if (!problems.empty()) {
        report = TfStringPrintf("hdClaude: cull volume <%s> is ignored: %s.",
                                id.GetText(),
                                TfStringJoin(problems, "; ").c_str());
    }
    if (report != _lastReport) {
        if (!report.empty()) {
            TF_WARN("%s", report.c_str());
        }
        _lastReport = report;
    }
    entry.report = report;

    store->SetSceneIndex(sceneIndex);
    store->PublishCullVolume(id, std::move(entry));
    HdClaudeTrace("cull volume %s published", id.GetText());

    *dirtyBits = HdChangeTracker::Clean;
}

void HdClaudeCullVolume::Finalize(HdRenderParam* renderParam)
{
    auto* param = static_cast<HdClaudeRenderParam*>(renderParam);
    if (param && param->SceneStore()) {
        param->SceneStore()->RemoveCullVolume(GetId());
    }
}

PXR_NAMESPACE_CLOSE_SCOPE
