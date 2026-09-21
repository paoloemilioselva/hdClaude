#include "sphere_scene_index.h"

#include "hdclaude/core/environment.h"
#include "hdclaude/core/sphere_mesh.h"

#include "pxr/base/gf/vec2f.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/tf/getenv.h"
#include "pxr/imaging/hd/meshSchema.h"
#include "pxr/imaging/hd/meshTopologySchema.h"
#include "pxr/imaging/hd/overlayContainerDataSource.h"
#include "pxr/imaging/hd/primvarSchema.h"
#include "pxr/imaging/hd/primvarsSchema.h"
#include "pxr/imaging/hd/renderIndex.h"
#include "pxr/imaging/hd/retainedDataSource.h"
#include "pxr/imaging/hd/sphereSchema.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/pxOsd/tokens.h"

#include <algorithm>
#include <iterator>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

/// The radius the prim authors, or one.
///
/// Read through the sphere schema's locator rather than from a cached value,
/// because a radius can be animated and this is asked once per sample time.
HdDoubleDataSourceHandle RadiusSource(
    const HdContainerDataSourceHandle& primDataSource)
{
    static const HdDataSourceLocator locator(HdSphereSchemaTokens->sphere,
                                             HdSphereSchemaTokens->radius);
    return HdDoubleDataSource::Cast(
        HdContainerDataSource::Get(primDataSource, locator));
}

double Radius(const HdContainerDataSourceHandle& primDataSource,
              const HdSampledDataSource::Time shutterOffset)
{
    if (const HdDoubleDataSourceHandle source = RadiusSource(primDataSource)) {
        return source->GetTypedValue(shutterOffset);
    }
    return 1.0;
}

/// The cage's points, at whatever radius the prim says at this moment.
class _PointsDataSource final : public HdVec3fArrayDataSource {
  public:
    HD_DECLARE_DATASOURCE(_PointsDataSource);

    VtValue GetValue(const Time shutterOffset) override
    {
        return VtValue(GetTypedValue(shutterOffset));
    }

    VtVec3fArray GetTypedValue(const Time shutterOffset) override
    {
        const hdclaude::SphereMesh mesh = hdclaude::GenerateSphereMesh(
            _radial, _axial, Radius(_primDataSource, shutterOffset));
        VtVec3fArray points(mesh.PointCount());
        for (std::size_t i = 0; i < mesh.PointCount(); ++i) {
            points[i] = GfVec3f(mesh.points[i * 3 + 0], mesh.points[i * 3 + 1],
                                mesh.points[i * 3 + 2]);
        }
        return points;
    }

    bool GetContributingSampleTimesForInterval(
        const Time startTime, const Time endTime,
        std::vector<Time>* const outSampleTimes) override
    {
        // The points move only because the radius does, so the radius decides
        // which samples contribute. A sphere with a fixed radius contributes
        // none and is not resampled.
        if (const HdDoubleDataSourceHandle source =
                RadiusSource(_primDataSource)) {
            return source->GetContributingSampleTimesForInterval(
                startTime, endTime, outSampleTimes);
        }
        return false;
    }

  private:
    _PointsDataSource(const HdContainerDataSourceHandle& primDataSource,
                      int radial, int axial)
        : _primDataSource(primDataSource), _radial(radial), _axial(axial)
    {
    }

    HdContainerDataSourceHandle _primDataSource;
    int _radial;
    int _axial;
};

HD_DECLARE_DATASOURCE_HANDLES(_PointsDataSource);

/// The topology and the texture coordinates, which depend on the density and
/// on nothing else -- not on the radius, and not on time.
struct _Shape {
    HdContainerDataSourceHandle mesh;
    HdDataSourceBaseHandle uvs;
};

_Shape BuildShape(int radial, int axial)
{
    _Shape shape;
    const hdclaude::SphereMesh generated =
        hdclaude::GenerateSphereMesh(radial, axial, 1.0);
    if (!generated.Valid()) {
        return shape;
    }

    VtIntArray counts(generated.faceVertexCounts.begin(),
                      generated.faceVertexCounts.end());
    VtIntArray indices(generated.faceVertexIndices.begin(),
                       generated.faceVertexIndices.end());

    shape.mesh =
        HdMeshSchema::Builder()
            .SetTopology(
                HdMeshTopologySchema::Builder()
                    .SetFaceVertexCounts(
                        HdRetainedTypedSampledDataSource<VtIntArray>::New(counts))
                    .SetFaceVertexIndices(
                        HdRetainedTypedSampledDataSource<VtIntArray>::New(indices))
                    .SetOrientation(
                        HdRetainedTypedSampledDataSource<TfToken>::New(
                            HdMeshTopologySchemaTokens->rightHanded))
                    .Build())
            // Catmull-Clark, as OpenUSD's generator declares it: the cage is a
            // control cage and the sphere it stands for is its limit surface,
            // which is what lets a refinement level decide how round it is.
            .SetSubdivisionScheme(
                HdRetainedTypedSampledDataSource<TfToken>::New(
                    PxOsdOpenSubdivTokens->catmullClark))
            .SetDoubleSided(HdRetainedTypedSampledDataSource<bool>::New(false))
            .Build();

    VtVec2fArray st(generated.faceVaryingUvs.size() / 2);
    for (std::size_t i = 0; i < st.size(); ++i) {
        st[i] = GfVec2f(generated.faceVaryingUvs[i * 2 + 0],
                        generated.faceVaryingUvs[i * 2 + 1]);
    }

    // Face-varying, which is the point of generating them at all: a sphere's
    // longitude wraps, so the vertex carrying u = 0 is the vertex that ought
    // to carry u = 1, and only a per-corner value can say both.
    shape.uvs =
        HdPrimvarSchema::Builder()
            .SetRole(HdPrimvarSchema::BuildRoleDataSource(
                HdPrimvarSchemaTokens->textureCoordinate))
            .SetInterpolation(HdPrimvarSchema::BuildInterpolationDataSource(
                HdPrimvarSchemaTokens->faceVarying))
            .SetPrimvarValue(
                HdRetainedTypedSampledDataSource<VtVec2fArray>::New(st))
            .Build();
    return shape;
}

/// What the converted prim looks like: a mesh, its points, its coordinates,
/// and no sphere.
HdContainerDataSourceHandle ConvertedSphere(
    const HdContainerDataSourceHandle& primDataSource, int radial, int axial)
{
    const _Shape shape = BuildShape(radial, axial);
    if (!shape.mesh) {
        return primDataSource;
    }

    const HdContainerDataSourceHandle primvars =
        HdRetainedContainerDataSource::New(
            HdPrimvarsSchemaTokens->points,
            HdPrimvarSchema::Builder()
                .SetRole(HdPrimvarSchema::BuildRoleDataSource(
                    HdPrimvarSchemaTokens->point))
                .SetInterpolation(HdPrimvarSchema::BuildInterpolationDataSource(
                    HdPrimvarSchemaTokens->vertex))
                .SetPrimvarValue(
                    _PointsDataSource::New(primDataSource, radial, axial))
                .Build(),
            // Named `st`, which is what hdClaude's mesh adapter looks for and
            // what a MaterialX `texcoord` node resolves to.
            TfToken("st"), shape.uvs);

    // The sphere schema is blocked rather than left in place: a prim that is
    // now a mesh and still answers as a sphere is two prims in one, and
    // anything downstream deciding which it is would be deciding by accident.
    static const HdDataSourceBaseHandle blocked = HdBlockDataSource::New();

    HdContainerDataSourceHandle overlay[] = {
        HdRetainedContainerDataSource::New(
            HdSphereSchemaTokens->sphere, blocked,
            HdMeshSchemaTokens->mesh, shape.mesh,
            HdPrimvarsSchemaTokens->primvars, primvars),
        primDataSource,
    };
    return HdOverlayContainerDataSource::New(std::size(overlay), overlay);
}

/// The divisions the environment asks for, clamped to what a sphere can be.
void ReadEnvironmentDivisions(int* radial, int* axial)
{
    *radial = std::max(TfGetenvInt("HDCLAUDE_SPHERE_RADIAL",
                                   hdclaude::kDefaultSphereRadial),
                       hdclaude::kMinSphereRadial);
    *axial = std::max(
        TfGetenvInt("HDCLAUDE_SPHERE_AXIAL", hdclaude::kDefaultSphereAxial),
        hdclaude::kMinSphereAxial);
}

}  // namespace

HdClaudeSphereSceneIndex::HdClaudeSphereSceneIndex(
    const HdSceneIndexBaseRefPtr& inputScene)
    : HdSingleInputFilteringSceneIndexBase(inputScene)
{
    ReadEnvironmentDivisions(&_radial, &_axial);
}

HdSceneIndexPrim HdClaudeSphereSceneIndex::GetPrim(const SdfPath& primPath) const
{
    const HdSceneIndexBaseRefPtr& input = _GetInputSceneIndex();
    if (!input) {
        return {};
    }
    HdSceneIndexPrim prim = input->GetPrim(primPath);
    if (prim.primType != HdPrimTypeTokens->sphere) {
        return prim;
    }
    prim.primType = HdPrimTypeTokens->mesh;
    prim.dataSource = ConvertedSphere(prim.dataSource, _radial, _axial);
    return prim;
}

SdfPathVector HdClaudeSphereSceneIndex::GetChildPrimPaths(
    const SdfPath& primPath) const
{
    if (const HdSceneIndexBaseRefPtr& input = _GetInputSceneIndex()) {
        return input->GetChildPrimPaths(primPath);
    }
    return {};
}

void HdClaudeSphereSceneIndex::_PrimsAdded(
    const HdSceneIndexBase&,
    const HdSceneIndexObserver::AddedPrimEntries& entries)
{
    HdSceneIndexObserver::AddedPrimEntries converted;
    converted.reserve(entries.size());
    for (const HdSceneIndexObserver::AddedPrimEntry& entry : entries) {
        converted.emplace_back(entry.primPath,
                               entry.primType == HdPrimTypeTokens->sphere
                                   ? HdPrimTypeTokens->mesh
                                   : entry.primType);
    }
    _SendPrimsAdded(converted);
}

void HdClaudeSphereSceneIndex::_PrimsRemoved(
    const HdSceneIndexBase&,
    const HdSceneIndexObserver::RemovedPrimEntries& entries)
{
    _SendPrimsRemoved(entries);
}

void HdClaudeSphereSceneIndex::_PrimsDirtied(
    const HdSceneIndexBase&,
    const HdSceneIndexObserver::DirtiedPrimEntries& entries)
{
    HdSceneIndexObserver::DirtiedPrimEntries converted(entries);
    for (const HdSceneIndexObserver::DirtiedPrimEntry& entry : entries) {
        // A radius that changed is a `sphere` locator downstream of here and a
        // *points* locator from here on, because that is what the radius has
        // become. Without this the prim would be dirtied for a schema it no
        // longer has and its mesh would keep the size it was born with.
        static const HdDataSourceLocator radius(HdSphereSchemaTokens->sphere,
                                                HdSphereSchemaTokens->radius);
        if (entry.dirtyLocators.Intersects(radius)) {
            static const HdDataSourceLocator points(
                HdPrimvarsSchemaTokens->primvars,
                HdPrimvarsSchemaTokens->points);
            converted.emplace_back(entry.primPath,
                                   HdDataSourceLocatorSet{points});
        }
    }
    _SendPrimsDirtied(converted);
}

bool HdClaudeSphereSceneIndex::SetDivisions(int radial, int axial)
{
    const int wantRadial = std::max(radial, hdclaude::kMinSphereRadial);
    const int wantAxial = std::max(axial, hdclaude::kMinSphereAxial);
    if (wantRadial == _radial && wantAxial == _axial) {
        return false;
    }
    _radial = wantRadial;
    _axial = wantAxial;

    // Every sphere this index has converted is now a different mesh. Nothing
    // in the scene changed, so nothing downstream will ask again unless it is
    // told -- and a density that silently does not take effect looks exactly
    // like a setting that was ignored.
    HdSceneIndexObserver::DirtiedPrimEntries dirtied;
    std::vector<SdfPath> pending{SdfPath::AbsoluteRootPath()};
    while (!pending.empty()) {
        const SdfPath path = pending.back();
        pending.pop_back();
        if (const HdSceneIndexBaseRefPtr& input = _GetInputSceneIndex()) {
            if (input->GetPrim(path).primType == HdPrimTypeTokens->sphere) {
                dirtied.emplace_back(path,
                                     HdDataSourceLocatorSet::UniversalSet());
            }
        }
        for (const SdfPath& child : GetChildPrimPaths(path)) {
            pending.push_back(child);
        }
    }
    if (!dirtied.empty()) {
        _SendPrimsDirtied(dirtied);
    }
    return true;
}

namespace {

HdClaudeSphereSceneIndex* FindIn(const HdSceneIndexBaseRefPtr& scene)
{
    if (!scene) {
        return nullptr;
    }
    if (auto* found =
            dynamic_cast<HdClaudeSphereSceneIndex*>(get_pointer(scene))) {
        return found;
    }
    if (auto filtering = TfDynamic_cast<HdFilteringSceneIndexBaseRefPtr>(scene)) {
        for (const HdSceneIndexBaseRefPtr& input : filtering->GetInputScenes()) {
            if (auto* found = FindIn(input)) {
                return found;
            }
        }
    }
    return nullptr;
}

}  // namespace

HdClaudeSphereSceneIndex* HdClaudeFindSphereSceneIndex(
    const HdRenderIndex* index)
{
    if (index == nullptr) {
        return nullptr;
    }
    return FindIn(index->GetTerminalSceneIndex());
}

PXR_NAMESPACE_CLOSE_SCOPE
