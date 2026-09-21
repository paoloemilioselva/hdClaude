#pragma once

// `UsdGeomSphere` to a mesh hdClaude can trace, converted here rather than by
// OpenUSD.
//
// `HdsiImplicitSurfaceSceneIndex` does this perfectly well and hdClaude used
// it, but two of its properties are fixed in a way that turned out to matter.
// Its density is `static constexpr size_t numRadial = 10` and the same again
// axially, so every sphere is ten by ten whatever it is for and whatever it is
// near. And the primvars it supplies are `points` alone, so a material that
// samples an image reads one texel at every vertex -- which is how a
// displacement map on a sphere came to produce a very slightly smaller sphere,
// with nothing anywhere saying why.
//
// So the sphere is hdClaude's, and the other five implicit types are still
// OpenUSD's: this index converts spheres and passes everything else through,
// and the plugin chains `HdsiImplicitSurfaceSceneIndex` behind it for the cube,
// cone, cylinder, capsule and plane.
//
// The cage is `GeomUtilSphereMeshGenerator`'s, point for point, so that at the
// default density nothing about the geometry changes. What is added is the
// density and the texture coordinates.

#include "pxr/imaging/hd/filteringSceneIndex.h"
#include "pxr/pxr.h"

PXR_NAMESPACE_OPEN_SCOPE

class HdRenderIndex;

TF_DECLARE_REF_PTRS(HdClaudeSphereSceneIndex);

class HdClaudeSphereSceneIndex final
    : public HdSingleInputFilteringSceneIndexBase {
  public:
    static HdClaudeSphereSceneIndexRefPtr New(
        const HdSceneIndexBaseRefPtr& inputScene)
    {
        return TfCreateRefPtr(new HdClaudeSphereSceneIndex(inputScene));
    }

    HdSceneIndexPrim GetPrim(const SdfPath& primPath) const override;
    SdfPathVector GetChildPrimPaths(const SdfPath& primPath) const override;

    /// How many divisions a converted sphere has, round and pole to pole.
    ///
    /// Clamped to what a sphere can be made of; `GetRadialDivisions` reports
    /// what was actually adopted, which is what a caller should believe rather
    /// than what it asked for.
    ///
    /// Returns true when this changed anything, having already declared every
    /// sphere it has dirty -- a density that changed is a different mesh, and
    /// Hydra has to be told so before it will ask for one.
    bool SetDivisions(int radial, int axial);
    int GetRadialDivisions() const { return _radial; }
    int GetAxialDivisions() const { return _axial; }

  protected:
    explicit HdClaudeSphereSceneIndex(const HdSceneIndexBaseRefPtr& inputScene);

    void _PrimsAdded(const HdSceneIndexBase& sender,
                     const HdSceneIndexObserver::AddedPrimEntries& entries) override;
    void _PrimsRemoved(const HdSceneIndexBase& sender,
                       const HdSceneIndexObserver::RemovedPrimEntries& entries) override;
    void _PrimsDirtied(const HdSceneIndexBase& sender,
                       const HdSceneIndexObserver::DirtiedPrimEntries& entries) override;

  private:
    int _radial;
    int _axial;
};

/// The instance in `index`'s chain, or null.
///
/// Found by walking in from the terminal, because the plugin registry
/// constructs it and hands it to Hydra rather than to the renderer. Null is an
/// ordinary answer for a host that drives the render index some other way.
HdClaudeSphereSceneIndex* HdClaudeFindSphereSceneIndex(
    const HdRenderIndex* index);

PXR_NAMESPACE_CLOSE_SCOPE
