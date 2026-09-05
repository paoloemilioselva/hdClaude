#pragma once

// What a prim's Sync() needs in order to publish.
//
// Hydra hands this to every prim, so it is the one channel by which an adapter
// reaches the scene store and the material compiler without a global.

#include "scene_store.h"

#include "pxr/imaging/hd/renderDelegate.h"

PXR_NAMESPACE_OPEN_SCOPE

class HdClaudeMaterialCompiler;

class HdClaudeRenderParam final : public HdRenderParam {
  public:
    HdClaudeRenderParam(HdClaudeSceneStore* store,
                        HdClaudeMaterialCompiler* materialCompiler)
        : _store(store), _materialCompiler(materialCompiler)
    {
    }

    HdClaudeSceneStore* SceneStore() const { return _store; }
    HdClaudeMaterialCompiler* MaterialCompiler() const { return _materialCompiler; }

  private:
    HdClaudeSceneStore* _store;
    HdClaudeMaterialCompiler* _materialCompiler;
};

PXR_NAMESPACE_CLOSE_SCOPE
