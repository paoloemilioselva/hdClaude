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
                        HdClaudeTexturePool* texturePool,
                        int subdivisionLevel)
        : _store(store),
          _materialCompiler(materialCompiler),
          _texturePool(texturePool),
          _subdivisionLevel(subdivisionLevel)
    {
    }

    HdClaudeSceneStore* SceneStore() const { return _store; }
    HdClaudeMaterialCompiler* MaterialCompiler() const { return _materialCompiler; }
    HdClaudeTexturePool* TexturePool() const { return _texturePool; }

    /// Uniform refinement depth for meshes whose scheme asks for it. Zero
    /// renders the control cage.
    ///
    /// Read at Sync rather than at render time because refinement changes the
    /// geometry itself: the acceleration structure is built from the refined
    /// cage, so the level is part of what a prim publishes.
    int SubdivisionLevel() const { return _subdivisionLevel; }

  private:
    HdClaudeSceneStore* _store;
    HdClaudeMaterialCompiler* _materialCompiler;
    HdClaudeTexturePool* _texturePool;
    int _subdivisionLevel = 0;
};

PXR_NAMESPACE_CLOSE_SCOPE
