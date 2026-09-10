#pragma once

// What a prim's Sync() needs in order to publish.
//
// Hydra hands this to every prim, so it is the one channel by which an adapter
// reaches the scene store and the material compiler without a global.

#include "scene_store.h"
#include "texture_loader.h"

#include "stage_stats.h"

#include "pxr/imaging/hd/renderDelegate.h"

PXR_NAMESPACE_OPEN_SCOPE

class HdClaudeMaterialCompiler;

class HdClaudeRenderParam final : public HdRenderParam {
  public:
    HdClaudeRenderParam(HdClaudeSceneStore* store,
                        HdClaudeMaterialCompiler* materialCompiler,
                        HdClaudeTexturePool* texturePool,
                        int subdivisionLevel, int curveSides,
                        int curveSegmentSamples, bool implicitCurves,
                        HdClaudeStageStats* stageStats)
        : _store(store),
          _materialCompiler(materialCompiler),
          _texturePool(texturePool),
          _subdivisionLevel(subdivisionLevel),
          _curveSides(curveSides),
          _curveSegmentSamples(curveSegmentSamples),
          _implicitCurves(implicitCurves),
          _stageStats(stageStats)
    {
    }

    HdClaudeSceneStore* SceneStore() const { return _store; }
    HdClaudeMaterialCompiler* MaterialCompiler() const { return _materialCompiler; }
    HdClaudeTexturePool* TexturePool() const { return _texturePool; }

    /// Where a stage records what it cost. Never null in the delegate's own
    /// param; a caller must still check, because a test may construct one.
    HdClaudeStageStats* StageStats() const { return _stageStats; }

    /// Uniform refinement depth for meshes whose scheme asks for it. Zero
    /// renders the control cage.
    ///
    /// Read at Sync rather than at render time because refinement changes the
    /// geometry itself: the acceleration structure is built from the refined
    /// cage, so the level is part of what a prim publishes.
    int SubdivisionLevel() const { return _subdivisionLevel; }

    /// How many faces a swept curve's cross-section has.
    ///
    /// The same kind of number as the subdivision level and read at the same
    /// time, for the same reason: it changes the geometry a prim publishes,
    /// not how that geometry is drawn.
    int CurveSides() const { return _curveSides; }

    /// How many straight spans each cubic curve segment becomes.
    int CurveSegmentSamples() const { return _curveSegmentSamples; }

    /// Whether curves are intersected as segments rather than swept to tubes.
    bool ImplicitCurves() const { return _implicitCurves; }

    /// The settings that decide what geometry an rprim *is*, rather than how a
    /// frame of it is traced.
    ///
    /// Changing one of these cannot take effect on the next frame the way an
    /// exposure or a sample count can: the prototype was already built one way
    /// during Sync, so the rprims that depend on it have to be resynced. The
    /// render pass owns that -- it is the only part of the delegate holding a
    /// render index -- and these setters exist for it to call. Returns true
    /// when something actually moved, because marking every rprim dirty for a
    /// setting that did not change would resync the stage once a frame.
    bool SetGeometrySettings(bool implicitCurves, int curveSides,
                             int curveSegmentSamples, int subdivisionLevel)
    {
        const bool changed = implicitCurves != _implicitCurves ||
                             curveSides != _curveSides ||
                             curveSegmentSamples != _curveSegmentSamples ||
                             subdivisionLevel != _subdivisionLevel;
        _implicitCurves = implicitCurves;
        _curveSides = curveSides;
        _curveSegmentSamples = curveSegmentSamples;
        _subdivisionLevel = subdivisionLevel;
        return changed;
    }

  private:
    HdClaudeSceneStore* _store;
    HdClaudeMaterialCompiler* _materialCompiler;
    HdClaudeTexturePool* _texturePool;
    HdClaudeStageStats* _stageStats = nullptr;
    int _subdivisionLevel = 0;
    int _curveSides = 6;
    int _curveSegmentSamples = 1;
    bool _implicitCurves = true;
};

PXR_NAMESPACE_CLOSE_SCOPE
