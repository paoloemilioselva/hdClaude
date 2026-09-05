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

    /// The scene revision actually uploaded to the path tracer. Advanced only
    /// after a successful upload, so a failed one is retried.
    std::uint64_t _uploadedRevision = 0;
    bool _hasUploaded = false;

    /// Consecutive failed executes. Bounds the retry so a deterministic
    /// failure cannot hold a render-until-converged host forever.
    static constexpr unsigned int kMaxConsecutiveFailures = 3;
    unsigned int _consecutiveFailures = 0;
};

PXR_NAMESPACE_CLOSE_SCOPE
