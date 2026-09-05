#pragma once

// What a prim's Sync() needs in order to publish.
//
// Hydra hands this to every prim, so it is the one channel by which an adapter
// reaches the scene store and the material compiler without a global.

#include "scene_store.h"
#include "texture_loader.h"

#include "pxr/imaging/hd/renderDelegate.h"

PXR_NAMESPACE_OPEN_SCOPE

class HdClaudeMaterialCompiler;

class HdClaudeRenderParam final : public HdRenderParam {
  public:
    HdClaudeRenderParam(HdClaudeSceneStore* store,
                        HdClaudeMaterialCompiler* materialCompiler,
                        HdClaudeTexturePool* texturePool)
        : _store(store),
          _materialCompiler(materialCompiler),
          _texturePool(texturePool)
    {
    }

    HdClaudeSceneStore* SceneStore() const { return _store; }
    HdClaudeMaterialCompiler* MaterialCompiler() const { return _materialCompiler; }
    HdClaudeTexturePool* TexturePool() const { return _texturePool; }

  private:
    HdClaudeSceneStore* _store;
    HdClaudeMaterialCompiler* _materialCompiler;
    HdClaudeTexturePool* _texturePool;
};

PXR_NAMESPACE_CLOSE_SCOPE
