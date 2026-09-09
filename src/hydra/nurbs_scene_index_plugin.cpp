#include "nurbs_scene_index_plugin.h"

#include "pxr/imaging/hd/sceneIndexPluginRegistry.h"
#include "pxr/imaging/hdsi/nurbsApproximatingSceneIndex.h"

PXR_NAMESPACE_OPEN_SCOPE

TF_DEFINE_PRIVATE_TOKENS(
    _tokens,
    ((sceneIndexPluginName, "HdClaude_NurbsApproximatingSceneIndexPlugin")));

/// The renderer this is inserted for, spelled exactly as the display name in
/// resources/plugInfo.json: the registry matches on that string, and a
/// mismatch registers the scene index for a renderer nobody has.
static const char* const kRendererDisplayName = "Claude GPU Path Tracer";

TF_REGISTRY_FUNCTION(TfType)
{
    HdSceneIndexPluginRegistry::Define<
        HdClaude_NurbsApproximatingSceneIndexPlugin>();
}

TF_REGISTRY_FUNCTION(HdSceneIndexPlugin)
{
    // At the start, before anything downstream has had to understand a prim
    // type this renderer does not support.
    const HdSceneIndexPluginRegistry::InsertionPhase insertionPhase = 0;
    HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
        kRendererDisplayName, _tokens->sceneIndexPluginName,
        /* inputArgs = */ nullptr, insertionPhase,
        HdSceneIndexPluginRegistry::InsertionOrderAtStart);
}

HdClaude_NurbsApproximatingSceneIndexPlugin::
    HdClaude_NurbsApproximatingSceneIndexPlugin() = default;

HdSceneIndexBaseRefPtr
HdClaude_NurbsApproximatingSceneIndexPlugin::_AppendSceneIndex(
    const HdSceneIndexBaseRefPtr& inputScene,
    const HdContainerDataSourceHandle& inputArgs)
{
    TF_UNUSED(inputArgs);
    return HdsiNurbsApproximatingSceneIndex::New(inputScene);
}

PXR_NAMESPACE_CLOSE_SCOPE
