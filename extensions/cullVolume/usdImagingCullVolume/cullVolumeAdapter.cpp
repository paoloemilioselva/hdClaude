#include "usdImagingCullVolume/cullVolumeAdapter.h"

#include "usdImagingCullVolume/cullVolumeSchema.h"

#include "pxr/base/tf/staticTokens.h"
#include "pxr/usd/usd/schemaRegistry.h"
#include "pxr/usdImaging/usdImaging/dataSourceAttribute.h"
#include "pxr/usdImaging/usdImaging/dataSourcePrim.h"

PXR_NAMESPACE_OPEN_SCOPE

// The USD names, spelled here because the schema is codeless: there is no
// UsdCullVolumeTokens to take them from.
TF_DEFINE_PRIVATE_TOKENS(
    _usdTokens,
    (CubeCullVolume)(SphereCullVolume)(PlaneCullVolume)(size)(radius)(axis)
    ((invert, "cullVolume:invert"))
    ((affectsPrimaryRays, "cullVolume:affectsPrimaryRays"))
    ((affectsSecondaryRays, "cullVolume:affectsSecondaryRays")));

TF_REGISTRY_FUNCTION(TfType)
{
    using Adapter = UsdImagingCullVolumeAdapter;
    TfType t = TfType::Define<Adapter, TfType::Bases<Adapter::BaseAdapter>>();
    t.SetFactory<UsdImagingPrimAdapterFactory<Adapter>>();
}

namespace {

/// The Hydra shape of a prim, from its schema type rather than its type name,
/// so a type derived from CubeCullVolume is still a cube. Empty for a cull
/// volume type this adapter does not know, which the renderer reports.
TfToken ShapeOf(const UsdPrim& prim)
{
    static const TfType cube =
        UsdSchemaRegistry::GetTypeFromSchemaTypeName(_usdTokens->CubeCullVolume);
    static const TfType sphere =
        UsdSchemaRegistry::GetTypeFromSchemaTypeName(_usdTokens->SphereCullVolume);
    static const TfType plane =
        UsdSchemaRegistry::GetTypeFromSchemaTypeName(_usdTokens->PlaneCullVolume);
    if (prim.IsA(cube)) {
        return HdCullVolumeSchemaTokens->cube;
    }
    if (prim.IsA(sphere)) {
        return HdCullVolumeSchemaTokens->sphere;
    }
    if (prim.IsA(plane)) {
        return HdCullVolumeSchemaTokens->plane;
    }
    return TfToken();
}

/// The `cullVolume` container: one data source per authored-or-fallback
/// attribute, and the shape. Values are passed through as authored -- a
/// negative size or an unknown axis is the renderer's to report, not this
/// adapter's to correct.
class _CullVolumeDataSource final : public HdContainerDataSource {
  public:
    HD_DECLARE_DATASOURCE(_CullVolumeDataSource);

    TfTokenVector GetNames() override
    {
        TfTokenVector names = {HdCullVolumeSchemaTokens->shape,
                               HdCullVolumeSchemaTokens->invert,
                               HdCullVolumeSchemaTokens->affectsPrimaryRays,
                               HdCullVolumeSchemaTokens->affectsSecondaryRays};
        if (_shape == HdCullVolumeSchemaTokens->cube) {
            names.push_back(HdCullVolumeSchemaTokens->size);
        } else if (_shape == HdCullVolumeSchemaTokens->sphere) {
            names.push_back(HdCullVolumeSchemaTokens->radius);
        } else if (_shape == HdCullVolumeSchemaTokens->plane) {
            names.push_back(HdCullVolumeSchemaTokens->axis);
        }
        return names;
    }

    HdDataSourceBaseHandle Get(const TfToken& name) override
    {
        if (name == HdCullVolumeSchemaTokens->shape) {
            return _shape.IsEmpty()
                       ? nullptr
                       : HdCullVolumeSchema::BuildShapeDataSource(_shape);
        }
        if (name == HdCullVolumeSchemaTokens->invert) {
            return _Attribute(_usdTokens->invert,
                              HdCullVolumeSchema::GetInvertLocator());
        }
        if (name == HdCullVolumeSchemaTokens->affectsPrimaryRays) {
            return _Attribute(_usdTokens->affectsPrimaryRays,
                              HdCullVolumeSchema::GetAffectsPrimaryRaysLocator());
        }
        if (name == HdCullVolumeSchemaTokens->affectsSecondaryRays) {
            return _Attribute(_usdTokens->affectsSecondaryRays,
                              HdCullVolumeSchema::GetAffectsSecondaryRaysLocator());
        }
        if (name == HdCullVolumeSchemaTokens->size &&
            _shape == HdCullVolumeSchemaTokens->cube) {
            return _Attribute(_usdTokens->size, HdCullVolumeSchema::GetSizeLocator());
        }
        if (name == HdCullVolumeSchemaTokens->radius &&
            _shape == HdCullVolumeSchemaTokens->sphere) {
            return _Attribute(_usdTokens->radius,
                              HdCullVolumeSchema::GetRadiusLocator());
        }
        if (name == HdCullVolumeSchemaTokens->axis &&
            _shape == HdCullVolumeSchemaTokens->plane) {
            return _Attribute(_usdTokens->axis, HdCullVolumeSchema::GetAxisLocator());
        }
        return nullptr;
    }

  private:
    _CullVolumeDataSource(const SdfPath& sceneIndexPath, const UsdPrim& prim,
                          const UsdImagingDataSourceStageGlobals& stageGlobals)
        : _sceneIndexPath(sceneIndexPath),
          _prim(prim),
          _stageGlobals(stageGlobals),
          _shape(ShapeOf(prim))
    {
    }

    /// The attribute's value over time. The locator is what UsdImaging marks
    /// dirty every frame when the attribute is animated, which is what lets a
    /// renderer follow an animated cull volume without a resync.
    HdDataSourceBaseHandle _Attribute(const TfToken& name,
                                      const HdDataSourceLocator& locator) const
    {
        const UsdAttribute attribute = _prim.GetAttribute(name);
        if (!attribute) {
            return nullptr;
        }
        return UsdImagingDataSourceAttributeNew(attribute, _stageGlobals,
                                                _sceneIndexPath, locator);
    }

    const SdfPath _sceneIndexPath;
    const UsdPrim _prim;
    const UsdImagingDataSourceStageGlobals& _stageGlobals;
    const TfToken _shape;
};

/// The prim: everything UsdImagingDataSourcePrim publishes for an Xformable
/// (xform, visibility, purpose, and the rest), plus `cullVolume`.
class _CullVolumePrimDataSource final : public UsdImagingDataSourcePrim {
  public:
    HD_DECLARE_DATASOURCE(_CullVolumePrimDataSource);

    TfTokenVector GetNames() override
    {
        TfTokenVector names = UsdImagingDataSourcePrim::GetNames();
        names.push_back(HdCullVolumeSchema::GetSchemaToken());
        return names;
    }

    HdDataSourceBaseHandle Get(const TfToken& name) override
    {
        if (name == HdCullVolumeSchema::GetSchemaToken()) {
            return _CullVolumeDataSource::New(_GetSceneIndexPath(), _GetUsdPrim(),
                                              _GetStageGlobals());
        }
        return UsdImagingDataSourcePrim::Get(name);
    }

  private:
    _CullVolumePrimDataSource(const SdfPath& sceneIndexPath, const UsdPrim& prim,
                              const UsdImagingDataSourceStageGlobals& stageGlobals)
        : UsdImagingDataSourcePrim(sceneIndexPath, prim, stageGlobals)
    {
    }
};

}  // namespace

UsdImagingCullVolumeAdapter::UsdImagingCullVolumeAdapter() = default;
UsdImagingCullVolumeAdapter::~UsdImagingCullVolumeAdapter() = default;

TfTokenVector UsdImagingCullVolumeAdapter::GetImagingSubprims(UsdPrim const& prim)
{
    TF_UNUSED(prim);
    return {TfToken()};
}

TfToken UsdImagingCullVolumeAdapter::GetImagingSubprimType(UsdPrim const& prim,
                                                           TfToken const& subprim)
{
    TF_UNUSED(prim);
    // The Hydra prim type is spelled as the schema token: one type for every
    // shape, which a renderer lists among its supported sprim types.
    return subprim.IsEmpty() ? HdCullVolumeSchema::GetSchemaToken() : TfToken();
}

HdContainerDataSourceHandle UsdImagingCullVolumeAdapter::GetImagingSubprimData(
    UsdPrim const& prim, TfToken const& subprim,
    const UsdImagingDataSourceStageGlobals& stageGlobals)
{
    if (!subprim.IsEmpty()) {
        return nullptr;
    }
    return _CullVolumePrimDataSource::New(prim.GetPath(), prim, stageGlobals);
}

HdDataSourceLocatorSet UsdImagingCullVolumeAdapter::InvalidateImagingSubprim(
    UsdPrim const& prim, TfToken const& subprim, TfTokenVector const& properties,
    UsdImagingPropertyInvalidationType invalidationType)
{
    if (!subprim.IsEmpty()) {
        return HdDataSourceLocatorSet();
    }

    HdDataSourceLocatorSet result;
    for (const TfToken& property : properties) {
        if (property == _usdTokens->size) {
            result.insert(HdCullVolumeSchema::GetSizeLocator());
        } else if (property == _usdTokens->radius) {
            result.insert(HdCullVolumeSchema::GetRadiusLocator());
        } else if (property == _usdTokens->axis) {
            result.insert(HdCullVolumeSchema::GetAxisLocator());
        } else if (property == _usdTokens->invert) {
            result.insert(HdCullVolumeSchema::GetInvertLocator());
        } else if (property == _usdTokens->affectsPrimaryRays) {
            result.insert(HdCullVolumeSchema::GetAffectsPrimaryRaysLocator());
        } else if (property == _usdTokens->affectsSecondaryRays) {
            result.insert(HdCullVolumeSchema::GetAffectsSecondaryRaysLocator());
        }
    }
    result.insert(UsdImagingDataSourcePrim::Invalidate(prim, subprim, properties,
                                                       invalidationType));
    return result;
}

PXR_NAMESPACE_CLOSE_SCOPE
