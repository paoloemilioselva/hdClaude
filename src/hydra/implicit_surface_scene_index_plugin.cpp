#include "implicit_surface_scene_index_plugin.h"

#include "sphere_scene_index.h"

#include "pxr/imaging/hd/retainedDataSource.h"
#include "pxr/imaging/hd/sceneIndexPluginRegistry.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/hdsi/implicitSurfaceSceneIndex.h"

PXR_NAMESPACE_OPEN_SCOPE

TF_DEFINE_PRIVATE_TOKENS(
    _tokens,
    ((sceneIndexPluginName, "HdClaude_ImplicitSurfaceSceneIndexPlugin")));

/// Spelled exactly as the display name in resources/plugInfo.json, which is
/// what the registry matches on.
static const char* const kRendererDisplayName = "Claude GPU Path Tracer";

TF_REGISTRY_FUNCTION(TfType)
{
    HdSceneIndexPluginRegistry::Define<
        HdClaude_ImplicitSurfaceSceneIndexPlugin>();
}

TF_REGISTRY_FUNCTION(HdSceneIndexPlugin)
{
    // Phase 0, before the light linking scene index at phase 1: the meshes
    // this produces are geometry that linking then has to categorise, and a
    // sphere that is still a sphere when linking runs would be categorised as
    // a prim type hdClaude never sees. hdStorm and hdEmbree both place it at
    // phase 0 for the same reason.
    const HdSceneIndexPluginRegistry::InsertionPhase insertionPhase = 0;
    HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
        kRendererDisplayName, _tokens->sceneIndexPluginName,
        /* inputArgs = */ nullptr, insertionPhase,
        HdSceneIndexPluginRegistry::InsertionOrderAtStart);
}

HdClaude_ImplicitSurfaceSceneIndexPlugin::
    HdClaude_ImplicitSurfaceSceneIndexPlugin() = default;

HdSceneIndexBaseRefPtr
HdClaude_ImplicitSurfaceSceneIndexPlugin::_AppendSceneIndex(
    const HdSceneIndexBaseRefPtr& inputScene,
    const HdContainerDataSourceHandle& inputArgs)
{
    TF_UNUSED(inputArgs);

    // Five of the six implicit types to a mesh. The alternative the scene
    // index offers -- `axisToTransform`, which keeps the prim and only rotates
    // the spine of a cone, cylinder or capsule onto the axis it was authored
    // around -- is for a renderer that intersects quadrics analytically.
    // hdClaude traces triangles, so the mesh is what it needs.
    //
    // The sixth, the sphere, is hdClaude's own and is converted *before* this,
    // so no sphere prim reaches OpenUSD's index and it is not asked to handle
    // one. Two things about a sphere needed to be ours: its density, which is
    // a pair of `static constexpr` tens in OpenUSD and is a render setting
    // here, and its texture coordinates, which OpenUSD does not supply at all
    // -- so a material sampling an image on a sphere read one texel at every
    // vertex (sphere_scene_index.h).
    const HdDataSourceBaseHandle toMesh =
        HdRetainedTypedSampledDataSource<TfToken>::New(
            HdsiImplicitSurfaceSceneIndexTokens->toMesh);

    const HdContainerDataSourceHandle arguments =
        HdRetainedContainerDataSource::New(
            HdPrimTypeTokens->cube, toMesh,
            HdPrimTypeTokens->cone, toMesh,
            HdPrimTypeTokens->cylinder, toMesh,
            HdPrimTypeTokens->capsule, toMesh,
            HdPrimTypeTokens->plane, toMesh);

    return HdsiImplicitSurfaceSceneIndex::New(
        HdClaudeSphereSceneIndex::New(inputScene), arguments);
}

PXR_NAMESPACE_CLOSE_SCOPE
