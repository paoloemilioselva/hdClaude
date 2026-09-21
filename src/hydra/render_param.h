#pragma once

// What a prim's Sync() needs in order to publish.
//
// Hydra hands this to every prim, so it is the one channel by which an adapter
// reaches the scene store and the material compiler without a global.

#include "scene_store.h"
#include "texture_loader.h"

#include "stage_stats.h"

#include "hdclaude/core/environment.h"
#include "hdclaude/core/tessellation.h"

#include <algorithm>
#include <cstring>

#include "pxr/base/tf/getenv.h"
#include "pxr/imaging/hd/renderDelegate.h"

PXR_NAMESPACE_OPEN_SCOPE

class HdClaudeMaterialCompiler;

/// How finely meshes are refined, and what decides it.
///
/// One struct rather than four loose numbers because these are compared as a
/// group: any change to them means every mesh has to be refined again, and a
/// comparison that forgot one would leave the stage refined by the setting it
/// no longer has.
struct HdClaudeTessellationSettings {
    /// Whether the level is chosen per mesh from its projected size.
    ///
    /// Off by default. Turning it on changes the geometry of every scene with
    /// meshes at more than one distance, and that is a decision for whoever is
    /// rendering rather than a default that quietly makes every committed
    /// image different.
    bool adaptive = false;

    /// The refinement depth. In adaptive mode this is the *ceiling*: no mesh
    /// is refined further, however close it is.
    int level = 2;

    /// What a mesh entirely outside the frustum is held at, in adaptive mode.
    ///
    /// Not zero. A path tracer sees geometry the camera does not -- in a
    /// mirror, through glass, as a shadow, in every indirect bounce -- so this
    /// is a reduction rather than a cull.
    int offScreenLevel = 1;

    /// How long a refined edge should be, in pixels.
    float targetEdgePixels = 4.0f;

    /// The most refined faces one mesh may be given, whether or not the level
    /// is adaptive. The honest form of "there is a limit": it names the mesh
    /// it applied to and what that mesh wanted.
    std::size_t maxRefinedFaces = 4u * 1024u * 1024u;

    /// Whether a camera move re-derives the levels.
    ///
    /// Off by default, and deliberately. Published geometry is what
    /// acceleration structures are built over and what the accumulated film
    /// depends on, so re-deriving on every viewport nudge would rebuild both
    /// and throw away the prototype reuse the fingerprints exist to provide.
    /// The view is *sampled* once and then held.
    bool followCamera = false;

    /// A counter the caller bumps to ask for the levels to be derived again
    /// against the camera as it is now. Any change is the request; the value
    /// itself means nothing.
    int retessellate = 0;

    bool operator==(const HdClaudeTessellationSettings& other) const
    {
        return adaptive == other.adaptive && level == other.level &&
               offScreenLevel == other.offScreenLevel &&
               targetEdgePixels == other.targetEdgePixels &&
               maxRefinedFaces == other.maxRefinedFaces &&
               followCamera == other.followCamera &&
               retessellate == other.retessellate;
    }
    bool operator!=(const HdClaudeTessellationSettings& other) const
    {
        return !(*this == other);
    }
};

/// The deepest refinement any setting may ask for.
///
/// Ten rather than the six this used to be. Six was a cap on the *level*, and
/// a cap on the level is the wrong instrument: it gives every mesh the same
/// answer whether or not it needed one, so a mesh that wanted more could not
/// have it and a mesh that wanted less was refined anyway. What limits the
/// cost now is `maxRefinedFaces`, which is about the machine and can say which
/// mesh it applied to. This is left in place so that a typo in a setting
/// cannot ask for 4^30 faces before the budget gets a chance to refuse.
inline constexpr int kHdClaudeMaxSubdivisionLevel = 10;

/// The bounds every reader of these settings applies.
///
/// In one place because the delegate reads them from the environment when it
/// is built and the render pass reads them from the render settings every
/// frame; two readers that clamped differently would disagree about what the
/// settings are and resync the whole stage once a frame over it.
inline HdClaudeTessellationSettings HdClaudeClampTessellation(
    HdClaudeTessellationSettings settings)
{
    settings.level =
        std::clamp(settings.level, 0, kHdClaudeMaxSubdivisionLevel);
    settings.offScreenLevel =
        std::clamp(settings.offScreenLevel, 0, settings.level);
    settings.targetEdgePixels = std::clamp(settings.targetEdgePixels, 0.25f,
                                           4096.0f);
    settings.maxRefinedFaces =
        std::max<std::size_t>(settings.maxRefinedFaces, 1);
    return settings;
}

/// The settings as the environment states them, which is also what the render
/// settings default to.
inline HdClaudeTessellationSettings HdClaudeReadTessellationEnvironment()
{
    HdClaudeTessellationSettings settings;
    settings.adaptive =
        hdclaude::EnvironmentFlag("HDCLAUDE_ADAPTIVE_SUBDIVISION", false);
    settings.level = TfGetenvInt("HDCLAUDE_SUBDIVISION_LEVEL", 2);
    settings.offScreenLevel =
        TfGetenvInt("HDCLAUDE_SUBDIVISION_OFFSCREEN_LEVEL", 1);
    settings.targetEdgePixels = static_cast<float>(
        TfGetenvDouble("HDCLAUDE_SUBDIVISION_EDGE_PIXELS", 4.0));
    settings.maxRefinedFaces = static_cast<std::size_t>(std::max(
        TfGetenvInt("HDCLAUDE_SUBDIVISION_FACE_BUDGET", 4 * 1024 * 1024), 1));
    settings.followCamera = hdclaude::EnvironmentFlag(
        "HDCLAUDE_SUBDIVISION_FOLLOWS_CAMERA", false);
    settings.retessellate = TfGetenvInt("HDCLAUDE_RETESSELLATE", 0);
    return HdClaudeClampTessellation(settings);
}

class HdClaudeRenderParam final : public HdRenderParam {
  public:
    HdClaudeRenderParam(HdClaudeSceneStore* store,
                        HdClaudeMaterialCompiler* materialCompiler,
                        HdClaudeTexturePool* texturePool,
                        const HdClaudeTessellationSettings& tessellation,
                        int curveSides,
                        int curveSegmentSamples, bool implicitCurves,
                        HdClaudeStageStats* stageStats)
        : _store(store),
          _materialCompiler(materialCompiler),
          _texturePool(texturePool),
          _tessellation(tessellation),
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
    int SubdivisionLevel() const { return _tessellation.level; }

    /// How finely to refine, and what decides it.
    const HdClaudeTessellationSettings& Tessellation() const
    {
        return _tessellation;
    }

    /// The camera the levels are derived against.
    ///
    /// A *sample*, not a live view: it is taken when there is nothing yet to
    /// derive from, when a refinement setting changes, when a retessellate is
    /// asked for, and otherwise only if `followCamera` is on. A mesh reads it
    /// during Sync, where no camera otherwise exists.
    const hdclaude::TessellationView& TessellationCamera() const
    {
        return _view;
    }

    /// Replace the sampled view. Returns true when it actually moved, so the
    /// caller can resync exactly when there is a reason to.
    bool SetTessellationCamera(const hdclaude::TessellationView& view)
    {
        const bool changed =
            view.valid != _view.valid || view.hasClip != _view.hasClip ||
            view.tanHalfFov != _view.tanHalfFov ||
            view.pixelHeight != _view.pixelHeight ||
            std::memcmp(view.cameraToWorld, _view.cameraToWorld,
                        sizeof(view.cameraToWorld)) != 0 ||
            std::memcmp(view.worldToClip, _view.worldToClip,
                        sizeof(view.worldToClip)) != 0;
        _view = view;
        return changed;
    }

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
                             int curveSegmentSamples,
                             const HdClaudeTessellationSettings& tessellation)
    {
        const bool changed = implicitCurves != _implicitCurves ||
                             curveSides != _curveSides ||
                             curveSegmentSamples != _curveSegmentSamples ||
                             tessellation != _tessellation;
        _implicitCurves = implicitCurves;
        _curveSides = curveSides;
        _curveSegmentSamples = curveSegmentSamples;
        _tessellation = tessellation;
        return changed;
    }

  private:
    HdClaudeSceneStore* _store;
    HdClaudeMaterialCompiler* _materialCompiler;
    HdClaudeTexturePool* _texturePool;
    HdClaudeStageStats* _stageStats = nullptr;
    HdClaudeTessellationSettings _tessellation;
    hdclaude::TessellationView _view;
    int _curveSides = 6;
    int _curveSegmentSamples = 1;
    bool _implicitCurves = true;
};

PXR_NAMESPACE_CLOSE_SCOPE
