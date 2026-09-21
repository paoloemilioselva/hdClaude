#include "retessellation_scene_index_plugin.h"

#include "pxr/imaging/hd/renderIndex.h"
#include "pxr/imaging/hd/sceneIndexPluginRegistry.h"
#include "pxr/imaging/hd/tokens.h"

#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

TF_DEFINE_PRIVATE_TOKENS(
    _tokens,
    ((sceneIndexPluginName, "HdClaude_RetessellationSceneIndexPlugin")));

/// The renderer this is inserted for, spelled exactly as the display name in
/// resources/plugInfo.json: the registry matches on that string, and a
/// mismatch registers the scene index for a renderer nobody has.
static const char* const kRendererDisplayName = "Claude GPU Path Tracer";

TF_REGISTRY_FUNCTION(TfType)
{
    HdSceneIndexPluginRegistry::Define<HdClaude_RetessellationSceneIndexPlugin>();
}

TF_REGISTRY_FUNCTION(HdSceneIndexPlugin)
{
    // At the very end of the chain, which is the only place that matters for a
    // filter that changes nothing: a notice sent from here reaches the render
    // index without passing through anything that might filter it out.
    const HdSceneIndexPluginRegistry::InsertionPhase insertionPhase = 0;
    HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
        kRendererDisplayName, _tokens->sceneIndexPluginName,
        /* inputArgs = */ nullptr, insertionPhase,
        HdSceneIndexPluginRegistry::InsertionOrderAtEnd);
}

HdSceneIndexPrim HdClaudeRetessellationSceneIndex::GetPrim(
    const SdfPath& primPath) const
{
    if (const HdSceneIndexBaseRefPtr& input = _GetInputSceneIndex()) {
        return input->GetPrim(primPath);
    }
    return {};
}

SdfPathVector HdClaudeRetessellationSceneIndex::GetChildPrimPaths(
    const SdfPath& primPath) const
{
    if (const HdSceneIndexBaseRefPtr& input = _GetInputSceneIndex()) {
        return input->GetChildPrimPaths(primPath);
    }
    return {};
}

void HdClaudeRetessellationSceneIndex::_PrimsAdded(
    const HdSceneIndexBase&,
    const HdSceneIndexObserver::AddedPrimEntries& entries)
{
    _SendPrimsAdded(entries);
}

void HdClaudeRetessellationSceneIndex::_PrimsRemoved(
    const HdSceneIndexBase&,
    const HdSceneIndexObserver::RemovedPrimEntries& entries)
{
    _SendPrimsRemoved(entries);
}

void HdClaudeRetessellationSceneIndex::_PrimsDirtied(
    const HdSceneIndexBase&,
    const HdSceneIndexObserver::DirtiedPrimEntries& entries)
{
    _SendPrimsDirtied(entries);
}

std::size_t HdClaudeRetessellationSceneIndex::DirtyGeometry()
{
    HdSceneIndexObserver::DirtiedPrimEntries dirtied;

    // An explicit stack rather than recursion: a deep stage is not unusual and
    // the depth here is the scene's, not this code's to bound.
    std::vector<SdfPath> pending{SdfPath::AbsoluteRootPath()};
    while (!pending.empty()) {
        const SdfPath path = pending.back();
        pending.pop_back();

        const HdSceneIndexPrim prim = GetPrim(path);
        if (prim.primType == HdPrimTypeTokens->mesh ||
            prim.primType == HdPrimTypeTokens->basisCurves) {
            dirtied.emplace_back(path, HdDataSourceLocatorSet::UniversalSet());
        }
        for (const SdfPath& child : GetChildPrimPaths(path)) {
            pending.push_back(child);
        }
    }

    if (!dirtied.empty()) {
        _SendPrimsDirtied(dirtied);
    }
    return dirtied.size();
}

namespace {

/// Depth-first through the filtering chain, in from the terminal.
HdClaudeRetessellationSceneIndex* FindIn(const HdSceneIndexBaseRefPtr& scene)
{
    if (!scene) {
        return nullptr;
    }
    if (auto* found =
            dynamic_cast<HdClaudeRetessellationSceneIndex*>(get_pointer(scene))) {
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

HdClaudeRetessellationSceneIndex* HdClaudeFindRetessellationSceneIndex(
    const HdRenderIndex* index)
{
    if (index == nullptr) {
        return nullptr;
    }
    return FindIn(index->GetTerminalSceneIndex());
}

HdClaude_RetessellationSceneIndexPlugin::
    HdClaude_RetessellationSceneIndexPlugin() = default;

HdSceneIndexBaseRefPtr
HdClaude_RetessellationSceneIndexPlugin::_AppendSceneIndex(
    const HdSceneIndexBaseRefPtr& inputScene,
    const HdContainerDataSourceHandle& inputArgs)
{
    TF_UNUSED(inputArgs);
    return HdClaudeRetessellationSceneIndex::New(inputScene);
}

PXR_NAMESPACE_CLOSE_SCOPE
