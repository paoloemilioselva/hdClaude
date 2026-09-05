#include "render_pass.h"

#include "camera.h"
#include "render_buffer.h"
#include "render_delegate.h"
#include "scene_store.h"
#include "trace.h"

#include "pxr/base/tf/diagnostic.h"
#include "pxr/imaging/hd/renderPassState.h"
#include "pxr/imaging/hd/tokens.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

TF_DEFINE_PRIVATE_TOKENS(_tokens,
                         (samplesPerPixel)
                         (maxBounces)
                         (samplesPerFrame)
                         (environmentIntensity)
                         (sunIntensity)
                         (exposure));

}  // namespace

bool HdClaudeRenderPass::Framing::operator==(const Framing& other) const
{
    return width == other.width && height == other.height &&
           sceneRevision == other.sceneRevision &&
           settingsVersion == other.settingsVersion &&
           std::memcmp(camera.cameraToWorld, other.camera.cameraToWorld,
                       sizeof(camera.cameraToWorld)) == 0 &&
           camera.tanHalfFov == other.camera.tanHalfFov &&
           camera.aspect == other.camera.aspect;
}

HdClaudeRenderPass::HdClaudeRenderPass(HdRenderIndex* index,
                                       const HdRprimCollection& collection,
                                       HdClaudeRenderDelegate* renderDelegate)
    : HdRenderPass(index, collection), _renderDelegate(renderDelegate)
{
}

HdClaudeRenderPass::~HdClaudeRenderPass() = default;

bool HdClaudeRenderPass::IsConverged() const { return _converged; }

void HdClaudeRenderPass::_Execute(
    const HdRenderPassStateSharedPtr& renderPassState,
    const TfTokenVector& /*renderTags*/)
{
    if (!_renderDelegate || !renderPassState) {
        return;
    }

    // Every AOV starts this execute unconverged. Whatever happens below either
    // finishes the image and says so, or leaves it unconverged so the host
    // calls back.
    HdClaudeRenderBuffer* colorBuffer = nullptr;
    const HdRenderPassAovBinding* colorBinding = nullptr;
    const HdRenderPassAovBindingVector& bindings =
        renderPassState->GetAovBindings();
    for (const HdRenderPassAovBinding& binding : bindings) {
        auto* buffer = dynamic_cast<HdClaudeRenderBuffer*>(binding.renderBuffer);
        if (!buffer) {
            continue;
        }
        buffer->SetConverged(false);
        if (binding.aovName == HdAovTokens->color) {
            colorBuffer = buffer;
            colorBinding = &binding;
        }
    }

    // How a failure ends.
    //
    // Retrying is right for a transient fault and wrong for a deterministic
    // one: a host that renders until convergence will spin forever on a
    // failure that cannot succeed. So a failure is retried a bounded number of
    // times and then reported as converged -- the image is wrong either way,
    // and an application that never returns is the worse of the two. The
    // counter resets on any successful frame.
    const auto noteFailure = [&]() -> bool {
        ++_consecutiveFailures;
        if (_consecutiveFailures < kMaxConsecutiveFailures) {
            return false;
        }
        TF_RUNTIME_ERROR(
            "hdClaude: giving up after %u consecutive failures; the frame is "
            "reported complete so the host does not spin. See the errors above.",
            _consecutiveFailures);
        return true;
    };

    const auto markConverged = [&](bool converged) {
        _converged = converged;
        for (const HdRenderPassAovBinding& binding : bindings) {
            if (auto* buffer =
                    dynamic_cast<HdClaudeRenderBuffer*>(binding.renderBuffer)) {
                buffer->SetConverged(converged);
            }
        }
    };

    HdClaudeTrace("execute: %zu aov bindings", bindings.size());

    hdclaude::PathTracer* tracer = _renderDelegate->PathTracer();
    HdClaudeSceneStore* store = _renderDelegate->SceneStore();
    if (!tracer || !store) {
        // The backend never came up. The reason was reported once at
        // construction and is in render stats; repeating it per frame would
        // bury it. Nothing will change on a retry, so this is converged.
        if (colorBuffer && colorBinding) {
            colorBuffer->Clear(
                colorBinding->clearValue.IsHolding<GfVec4f>()
                    ? colorBinding->clearValue.UncheckedGet<GfVec4f>().data()
                    : nullptr);
        }
        markConverged(true);
        return;
    }

    if (!colorBuffer) {
        // No colour AOV to write. Not an error -- some passes bind only depth,
        // which hdClaude does not produce yet.
        markConverged(true);
        return;
    }

    const unsigned int width = colorBuffer->GetWidth();
    const unsigned int height = colorBuffer->GetHeight();
    if (width == 0 || height == 0) {
        markConverged(true);
        return;
    }

    // --- Settings ------------------------------------------------------------
    const unsigned int settingsVersion =
        _renderDelegate->GetRenderSettingsVersion();
    const std::uint32_t targetSamples = static_cast<std::uint32_t>(
        std::clamp(_renderDelegate->GetRenderSetting<int>(
                       _tokens->samplesPerPixel, 64),
                   1, 65536));
    const std::uint32_t maxBounces = static_cast<std::uint32_t>(std::clamp(
        _renderDelegate->GetRenderSetting<int>(_tokens->maxBounces, 4), 1, 64));
    const std::uint32_t samplesPerFrame = static_cast<std::uint32_t>(std::clamp(
        _renderDelegate->GetRenderSetting<int>(_tokens->samplesPerFrame, 4), 1,
        4096));

    // --- Framing --------------------------------------------------------------
    const HdClaudeCameraResult cameraResult = HdClaudeMakeRenderCamera(
        renderPassState->GetWorldToViewMatrix(),
        renderPassState->GetProjectionMatrix());
    if (cameraResult.orthographic) {
        // Said once per framing change rather than once per frame.
        static bool warned = false;
        if (!warned) {
            warned = true;
            TF_WARN(
                "hdClaude: orthographic cameras are not implemented; rendering "
                "with the equivalent perspective view");
        }
    }

    Framing framing;
    framing.camera = cameraResult.camera;
    framing.width = width;
    framing.height = height;
    framing.sceneRevision = store->Revision();
    framing.settingsVersion = settingsVersion;

    const bool restart = !_hasFraming || framing != _framing;
    if (restart) {
        _framing = framing;
        _hasFraming = true;
        _samplesCompleted = 0;
    }
    _targetSamples = targetSamples;

    // --- Scene upload ----------------------------------------------------------
    if (!_hasUploaded || _uploadedRevision != framing.sceneRevision) {
        std::vector<hdclaude::CompiledMaterial> materials;
        hdclaude::Scene scene = store->Snapshot(materials);

        // The texture pool is the delegate's, not the store's: it is filled
        // during material Sync and is shared by every material that names the
        // same image, so it is copied into the snapshot here rather than
        // duplicated per material.
        if (HdClaudeTexturePool* pool = _renderDelegate->TexturePool()) {
            scene.textures = pool->Images();
        }
        HdClaudeTrace("snapshot: %zu prototypes, %zu instances, %zu triangles, "
                      "%zu materials, %zu lights, %zu textures, "
                      "environment %.3f %.3f %.3f%s",
                      scene.prototypes.size(), scene.instances.size(),
                      scene.TotalTriangles(), materials.size(),
                      scene.lights.size(), scene.textures.size(),
                      scene.environmentColor[0], scene.environmentColor[1],
                      scene.environmentColor[2],
                      scene.hasDomeLight ? " (dome light)" : "");
        std::copy(std::begin(scene.environmentColor),
                  std::end(scene.environmentColor), std::begin(_environmentColor));
        try {
            tracer->SetScene(scene, materials);
            HdClaudeTrace("scene published");
            _uploadedRevision = framing.sceneRevision;
            _hasUploaded = true;
        } catch (const std::exception& error) {
            // Not committed: _uploadedRevision keeps its old value, so the next
            // execute retries this revision. A failure that advanced the
            // revision would leave the viewport permanently stale with no way
            // back short of reloading the stage.
            HdClaudeTrace("scene publication failed: %s", error.what());
            TF_RUNTIME_ERROR("hdClaude: could not publish the scene: %s",
                             error.what());
            markConverged(noteFailure());
            return;
        }
    }

    if (_samplesCompleted >= _targetSamples) {
        markConverged(true);
        return;
    }

    // --- Trace -----------------------------------------------------------------
    hdclaude::RenderSettings settings;
    settings.maxBounces = maxBounces;

    // The environment the scene published: a dome light's radiance, or the
    // stand-in sky when the stage has none.
    std::copy(std::begin(_environmentColor), std::end(_environmentColor),
              std::begin(settings.environmentColor));

    // Scale the sky and the stand-in sun. Both default to 1, so this changes
    // nothing until a user asks it to.
    const float environmentIntensity = std::max(
        0.0f, _renderDelegate->GetRenderSetting<float>(
                  _tokens->environmentIntensity, 1.0f));
    const float sunIntensity = std::max(
        0.0f,
        _renderDelegate->GetRenderSetting<float>(_tokens->sunIntensity, 1.0f));
    for (int i = 0; i < 3; ++i) {
        settings.environmentColor[i] *= environmentIntensity;
        settings.sunRadiance[i] *= sunIntensity;
    }
    settings.firstSample = _samplesCompleted;
    settings.samplesPerPixel =
        std::min(samplesPerFrame, _targetSamples - _samplesCompleted);
    // A restart clears the film; a continuation adds to it. The two are decided
    // by the same comparison that decided whether to reset the sample count, so
    // they cannot disagree.
    settings.resetAccumulation = (_samplesCompleted == 0);

    HdClaudeTrace("tracing %ux%u, samples %u..%u of %u, %u bounces", width,
                  height, settings.firstSample,
                  settings.firstSample + settings.samplesPerPixel,
                  _targetSamples, settings.maxBounces);

    const auto start = std::chrono::steady_clock::now();
    std::vector<float> image;
    try {
        image = tracer->Render(width, height, framing.camera, settings);
    } catch (const std::exception& error) {
        HdClaudeTrace("path trace failed: %s", error.what());
        TF_RUNTIME_ERROR("hdClaude: the path trace failed: %s", error.what());
        // The sample count is not advanced, so a transient failure is retried
        // rather than baked into a committed frame. noteFailure decides when
        // retrying has stopped being useful.
        markConverged(noteFailure());
        return;
    }
    const double milliseconds =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start)
            .count();

    if (HdClaudeTraceEnabled()) {
        // What the tracer actually produced, before the AOV and any display
        // transform touch it. A black viewport has two very different causes --
        // the renderer returned nothing, or something downstream ate it -- and
        // this is the line that tells them apart.
        float lowest = std::numeric_limits<float>::max();
        float highest = -std::numeric_limits<float>::max();
        double total = 0.0;
        std::size_t lit = 0;
        const std::size_t pixels = static_cast<std::size_t>(width) * height;
        for (std::size_t i = 0; i < pixels; ++i) {
            const float luminance = 0.2126f * image[i * 4 + 0] +
                                    0.7152f * image[i * 4 + 1] +
                                    0.0722f * image[i * 4 + 2];
            lowest = std::min(lowest, luminance);
            highest = std::max(highest, luminance);
            total += luminance;
            if (luminance > 1e-6f) {
                ++lit;
            }
        }
        HdClaudeTrace(
            "traced in %.1f ms; luminance min %.5f max %.5f mean %.5f, "
            "%.1f%% of pixels non-black",
            milliseconds, lowest, highest,
            pixels ? total / static_cast<double>(pixels) : 0.0,
            pixels ? 100.0 * static_cast<double>(lit) / static_cast<double>(pixels)
                   : 0.0);
    } else {
        HdClaudeTrace("traced in %.1f ms", milliseconds);
    }
    _consecutiveFailures = 0;
    _samplesCompleted += settings.samplesPerPixel;

    // Exposure last, on the resolved image, so the accumulated film keeps the
    // radiance the renderer computed and changing exposure costs no samples.
    const float exposure =
        _renderDelegate->GetRenderSetting<float>(_tokens->exposure, 0.0f);
    if (exposure != 0.0f) {
        const float scale = std::pow(2.0f, exposure);
        for (std::size_t i = 0; i < image.size(); i += 4) {
            image[i + 0] *= scale;
            image[i + 1] *= scale;
            image[i + 2] *= scale;
        }
    }

    colorBuffer->Write(image);
    _renderDelegate->RecordFrameTiming(milliseconds, _samplesCompleted);

    markConverged(_samplesCompleted >= _targetSamples);
}

PXR_NAMESPACE_CLOSE_SCOPE
