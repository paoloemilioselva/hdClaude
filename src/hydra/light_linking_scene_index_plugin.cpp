#include "light_linking_scene_index_plugin.h"

#include "pxr/imaging/hd/sceneIndexPluginRegistry.h"
#include "pxr/imaging/hdsi/lightLinkingSceneIndex.h"

PXR_NAMESPACE_OPEN_SCOPE

TF_DEFINE_PRIVATE_TOKENS(
    _tokens,
    ((sceneIndexPluginName, "HdClaude_LightLinkingSceneIndexPlugin")));

/// Spelled exactly as the display name in resources/plugInfo.json, which is
/// what the registry matches on.
static const char* const kRendererDisplayName = "Claude GPU Path Tracer";

TF_REGISTRY_FUNCTION(TfType)
{
    HdSceneIndexPluginRegistry::Define<HdClaude_LightLinkingSceneIndexPlugin>();
}

TF_REGISTRY_FUNCTION(HdSceneIndexPlugin)
{
    // After the NURBS conversion at phase 0, so curves it produces are
    // geometry the linking scene index can categorise, and at the start of its
    // phase, as hdPrman places it.
    const HdSceneIndexPluginRegistry::InsertionPhase insertionPhase = 1;
    HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
        kRendererDisplayName, _tokens->sceneIndexPluginName,
        /* inputArgs = */ nullptr, insertionPhase,
        HdSceneIndexPluginRegistry::InsertionOrderAtStart);
}

HdClaude_LightLinkingSceneIndexPlugin::HdClaude_LightLinkingSceneIndexPlugin() =
    default;

HdSceneIndexBaseRefPtr HdClaude_LightLinkingSceneIndexPlugin::_AppendSceneIndex(
    const HdSceneIndexBaseRefPtr& inputScene,
    const HdContainerDataSourceHandle& inputArgs)
{
    TF_UNUSED(inputArgs);
    // No arguments: the scene index's own defaults name Hydra's light, light
    // filter and geometry prim types, which are the types hdClaude supports.
    return HdsiLightLinkingSceneIndex::New(inputScene, nullptr);
}

PXR_NAMESPACE_CLOSE_SCOPE
