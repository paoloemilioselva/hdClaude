#pragma once

#include "api.h"

#include "hdclaude/gpu/path_tracer.h"

#include "pxr/imaging/hd/renderPass.h"

#include <cstdint>

PXR_NAMESPACE_OPEN_SCOPE

class HdClaudeRenderDelegate;

/// Drives the path tracer for one viewport.
///
/// Progressive: each _Execute traces a slice of the sample budget and hands the
/// partial image over, so an interactive host stays responsive instead of
/// blocking for a whole image. Convergence is reported honestly -- a pass that
/// failed is not converged, and it does not commit the revision it failed on,
/// so the next execute retries rather than leaving a black viewport that
/// nothing will ever repaint (docs/lessons-from-hdcodex.md C3).
class HDCLAUDE_API HdClaudeRenderPass final : public HdRenderPass {
  public:
    HdClaudeRenderPass(HdRenderIndex* index,
                       const HdRprimCollection& collection,
                       HdClaudeRenderDelegate* renderDelegate);
    ~HdClaudeRenderPass() override;

    bool IsConverged() const override;

  protected:
    void _Execute(const HdRenderPassStateSharedPtr& renderPassState,
                  const TfTokenVector& renderTags) override;

  private:
    /// State that, when it changes, invalidates the accumulated film.
    struct Framing {
        hdclaude::RenderCamera camera;
        unsigned int width = 0;
        unsigned int height = 0;
        std::uint64_t sceneRevision = 0;
        unsigned int settingsVersion = 0;

        bool operator==(const Framing& other) const;
        bool operator!=(const Framing& other) const { return !(*this == other); }
    };

    HdClaudeRenderDelegate* _renderDelegate;

    Framing _framing;
    bool _hasFraming = false;
    std::uint32_t _samplesCompleted = 0;
    std::uint32_t _targetSamples = 0;
    bool _converged = false;

    /// Diagnostic: how many more times to render the finished image over
    /// again, and what the finished ones hashed to. Set from
    /// HDCLAUDE_REPEAT_RENDERS; zero, and none of this runs.
    ///
    /// It exists because in-process reproducibility could not otherwise be
    /// tested at the sample count where it matters. A static stage rendered at
    /// several time codes does not test it: nothing has changed, so the
    /// accumulation is correctly not reset and the later frames re-emit the
    /// film the first one produced, fifty milliseconds apart. Only an explicit
    /// instruction to render it again renders it again.
    std::uint32_t _repeatsRemaining = 0;
    bool _repeatsStarted = false;
    bool _repeatsAsked = false;
    /// Trace milliseconds at the last repeat boundary, so each repeat
    /// reports its own time rather than the running total.
    double _repeatTraceMs = 0.0;

    /// Per-frame log state; see HDCLAUDE_FRAME_LOG. The ray counts are
    /// running totals on the tracer, so a frame reports the difference.
    std::uint64_t _frameLogIndex = 0;
    std::uint64_t _frameLogTracedRays = 0;
    std::uint64_t _frameLogShadowRays = 0;

    /// The scene revision actually uploaded to the path tracer. Advanced only
    /// after a successful upload, so a failed one is retried.
    std::uint64_t _uploadedRevision = 0;
    bool _hasUploaded = false;

    /// The environment radiance the published scene carries, remembered
    /// because the snapshot is only taken when the revision changes while the
    /// render settings are read every frame.
    float _environmentColor[3] = {0.05f, 0.07f, 0.10f};

    /// Consecutive failed executes. Bounds the retry so a deterministic
    /// failure cannot hold a render-until-converged host forever.
    static constexpr unsigned int kMaxConsecutiveFailures = 3;
    unsigned int _consecutiveFailures = 0;
};

PXR_NAMESPACE_CLOSE_SCOPE
