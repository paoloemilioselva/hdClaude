#include "render_pass.h"

#include "camera.h"
#include "render_buffer.h"
#include "render_delegate.h"
#include "render_param.h"
#include "scene_store.h"
#include "trace.h"

#include "pxr/base/tf/diagnostic.h"
#include "pxr/base/tf/getenv.h"
#include "pxr/imaging/hd/changeTracker.h"
#include "pxr/imaging/hd/renderIndex.h"
#include "pxr/imaging/hd/renderPassState.h"
#include "pxr/imaging/hd/tokens.h"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <cstdio>
#include <chrono>
#include <cmath>
#include <cctype>
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
                         (upAxis)
                         (exposure)
                         (reconstruction)
                         (reconstructionPreset)
                         (reconstructionAutoExposure)
                         (lightGeometry)
                         (curveGeometry)
                         (curveSides)
                         (curveSegmentSamples)
                         (subdivisionLevel));

/// The reconstruction setting, parsed.
///
/// A name nobody recognises is reported and refused rather than guessed at: a
/// user who typed "perf" and got a reference render with no explanation would
/// have no way to tell that from DLSS being unavailable, and the whole purpose
/// of the setting is to make those two states distinguishable.
struct ReconstructionChoice {
    bool on = false;
    hdclaude::ReconstructionQuality quality =
        hdclaude::ReconstructionQuality::NativeResolution;
    bool recognised = true;
};

std::string Lowered(const std::string& text)
{
    std::string out = text;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

ReconstructionChoice ParseReconstruction(const std::string& value)
{
    using Q = hdclaude::ReconstructionQuality;
    const std::string name = Lowered(value);
    ReconstructionChoice choice;
    if (name.empty() || name == "off" || name == "none" || name == "0") {
        return choice;
    }
    choice.on = true;
    if (name == "dlaa" || name == "native") {
        choice.quality = Q::NativeResolution;
    } else if (name == "quality") {
        choice.quality = Q::Quality;
    } else if (name == "balanced") {
        choice.quality = Q::Balanced;
    } else if (name == "performance" || name == "perf") {
        choice.quality = Q::Performance;
    } else if (name == "ultraperformance" || name == "ultra-performance" ||
               name == "ultraperf") {
        choice.quality = Q::UltraPerformance;
    } else {
        choice.on = false;
        choice.recognised = false;
    }
    return choice;
}

bool ParsePreset(const std::string& value, hdclaude::ReconstructionPreset* out)
{
    using P = hdclaude::ReconstructionPreset;
    const std::string name = Lowered(value);
    if (name.empty() || name == "default") {
        *out = P::Default;
    } else if (name == "stable" || name == "f") {
        *out = P::Stable;
    } else if (name == "transformer" || name == "k") {
        *out = P::Transformer;
    } else if (name == "transformer-alt" || name == "transformeralt" ||
               name == "j") {
        *out = P::TransformerAlternate;
    } else {
        return false;
    }
    return true;
}

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
    HdClaudeRenderBuffer* depthBuffer = nullptr;
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
        } else if (binding.aovName == HdAovTokens->depth) {
            depthBuffer = buffer;
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

    // --- Geometry settings ----------------------------------------------------
    //
    // These decide what an rprim *is*, so a change to one cannot take effect on
    // the next frame the way a sample count can: the prototypes were built the
    // old way during Sync and have to be built again. This is the only part of
    // the delegate holding a render index, so this is where that happens.
    //
    // Every rprim is marked rather than only the curves. Subdivision belongs to
    // meshes and curve geometry to curves, and separating them would mean
    // asking the index what type each prim is to save work in a case that
    // happens when somebody moves a slider.
    {
        const std::string curveGeometry =
            _renderDelegate->GetRenderSetting<std::string>(
                _tokens->curveGeometry, std::string("implicit"));
        // Anything but `swept` is the default, implicit, and says so once. The
        // fallback has to agree with the delegate's: a pass that read an
        // unset setting as swept would rebuild every curve on its first frame,
        // holding both forms at once on exactly the asset the default exists
        // for.
        if (curveGeometry != "swept" && curveGeometry != "implicit" &&
            _reportedCurveGeometry != curveGeometry) {
            _reportedCurveGeometry = curveGeometry;
            TF_WARN(
                "hdClaude: \"%s\" is not a curve geometry, so curves render "
                "implicit. Use one of: implicit, swept.",
                curveGeometry.c_str());
        }
        const int curveSides = std::clamp(
            _renderDelegate->GetRenderSetting<int>(_tokens->curveSides, 6), 3,
            64);
        const int curveSegmentSamples = std::clamp(
            _renderDelegate->GetRenderSetting<int>(_tokens->curveSegmentSamples,
                                                   1),
            1, 32);
        const int subdivision = std::clamp(
            _renderDelegate->GetRenderSetting<int>(_tokens->subdivisionLevel, 2),
            0, 6);

        auto* param = static_cast<HdClaudeRenderParam*>(
            _renderDelegate->GetRenderParam());
        if (param != nullptr &&
            param->SetGeometrySettings(curveGeometry != "swept", curveSides,
                                       curveSegmentSamples, subdivision) &&
            GetRenderIndex() != nullptr) {
            HdChangeTracker& tracker = GetRenderIndex()->GetChangeTracker();
            for (const SdfPath& rprim : GetRenderIndex()->GetRprimIds()) {
                tracker.MarkRprimDirty(rprim, HdChangeTracker::DirtyTopology |
                                                  HdChangeTracker::DirtyPoints |
                                                  HdChangeTracker::DirtyWidths);
            }
            HdClaudeTrace("geometry settings changed; resyncing %zu rprims",
                          GetRenderIndex()->GetRprimIds().size());
        }
    }

    // --- Reconstruction -------------------------------------------------------
    //
    // Off is the reference render this delegate has always done. On switches
    // the whole pass to interactive frames: each one is its own estimate at
    // `samplesPerFrame` samples, decorrelated from the last, and the averaging
    // that a reference render does in the film is done instead by the backend's
    // temporal history. The sample budget is spent the same way and means
    // something different, so `Samples per pixel` still bounds the sequence and
    // the last reconstructed frame is what a converged host is left looking at.
    const std::string reconstructionName =
        _renderDelegate->GetRenderSetting<std::string>(_tokens->reconstruction,
                                                       std::string("off"));
    const ReconstructionChoice reconstruction =
        ParseReconstruction(reconstructionName);
    if (!reconstruction.recognised && _reportedReconstruction != reconstructionName) {
        _reportedReconstruction = reconstructionName;
        TF_WARN(
            "hdClaude: \"%s\" is not a reconstruction mode, so this renders "
            "without reconstruction. Use one of: off, dlaa, quality, balanced, "
            "performance, ultraperformance.",
            reconstructionName.c_str());
    }

    const std::string presetName = _renderDelegate->GetRenderSetting<std::string>(
        _tokens->reconstructionPreset, std::string("default"));
    hdclaude::ReconstructionPreset preset =
        hdclaude::ReconstructionPreset::Default;
    if (!ParsePreset(presetName, &preset) && _reportedPreset != presetName) {
        _reportedPreset = presetName;
        TF_WARN(
            "hdClaude: \"%s\" is not a reconstruction preset, so the backend "
            "chooses its own. Use one of: default, stable (DLSS preset F), "
            "transformer (K), transformer-alt (J).",
            presetName.c_str());
    }

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
    //
    // DIAGNOSTIC: republish once, at a chosen frame.
    //
    // An interactive session that republishes and then keeps rendering
    // reaches a state tens of times faster than a batch render ever does.
    // Reproducing that offline is the only way to find out what the fast
    // state *is*, so this forces the republish a viewer would have caused.
    const int republishAt = TfGetenvInt("HDCLAUDE_REPUBLISH_AT", 0);
    if (republishAt > 0 &&
        _frameLogIndex == static_cast<std::uint64_t>(republishAt)) {
        _hasUploaded = false;
    }
    if (!_hasUploaded || _uploadedRevision != framing.sceneRevision) {
        // Ingestion and publication are timed separately because they fail and
        // scale for different reasons: the first is a traversal on the host and
        // the second is an upload and an acceleration structure build. A single
        // figure covering both would say a heavy scene is slow to load without
        // saying which half.
        const auto ingestStart = std::chrono::steady_clock::now();
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
        HdClaudeStageStats& stages = _renderDelegate->StageStats();
        HdClaudeAddMilliseconds(
            stages.ingestMilliseconds,
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - ingestStart)
                .count());
        stages.instances.store(scene.instances.size(),
                               std::memory_order_relaxed);
        stages.triangles.store(scene.TotalTriangles(),
                               std::memory_order_relaxed);

        const auto publishStart = std::chrono::steady_clock::now();
        try {
            tracer->SetScene(scene, materials);
            HdClaudeAddMilliseconds(
                stages.publishMilliseconds,
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - publishStart)
                    .count());
            stages.blasBuilt.store(tracer->BlasBuilt(),
                                   std::memory_order_relaxed);
            stages.blasReused.store(tracer->BlasReused(),
                                    std::memory_order_relaxed);
            stages.blasRefit.store(tracer->BlasRefit(),
                                   std::memory_order_relaxed);
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

    // Aim the stand-in sun, which only lights a stage that has no lights of
    // its own -- `hdclaude_emitter_count` includes it exactly when the light
    // count is zero.
    //
    // 70 degrees of elevation, which is a high sun: it reaches the floor of a
    // courtyard and the back of an arcade, where a low one rakes across the
    // near wall and leaves the rest of an enclosed set to the sky alone. The
    // azimuth is 45 degrees off each horizontal axis so the light is oblique to
    // an axis-aligned building rather than square to one face of it, which is
    // what keeps a box reading as a box.
    //
    // Elevation is measured about the stage's up axis, and getting that wrong
    // does not dim the scene, it points the sun *sideways*: Pixar's Kitchen Set
    // is Z-up, and the Y-up direction this used to hardcode ran horizontally
    // through it.
    {
        const std::string upAxis = _renderDelegate->GetRenderSetting<std::string>(
            _tokens->upAxis, std::string("Y"));
        constexpr float kSinElevation = 0.93969262f;  // sin(70 degrees)
        constexpr float kHorizontal = 0.24184476f;    // cos(70) * cos(45)
        const bool zUp = (upAxis == "Z" || upAxis == "z");
        settings.sunDirection[0] = kHorizontal;
        settings.sunDirection[1] = zUp ? kHorizontal : kSinElevation;
        settings.sunDirection[2] = zUp ? kSinElevation : kHorizontal;
    }

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
    settings.reconstruct = reconstruction.on;
    settings.reconstructionQuality = reconstruction.quality;
    settings.reconstructionPreset = preset;
    settings.lightGeometry = _renderDelegate->GetRenderSetting<bool>(
        _tokens->lightGeometry, false);
    settings.reconstructionAutoExposure =
        _renderDelegate->GetRenderSetting<bool>(
            _tokens->reconstructionAutoExposure, false);

    HdClaudeTrace("tracing %ux%u, samples %u..%u of %u, %u bounces", width,
                  height, settings.firstSample,
                  settings.firstSample + settings.samplesPerPixel,
                  _targetSamples, settings.maxBounces);

    // The frame-scoped entry point, not a bare Render.
    //
    // Everything that could invalidate anything travels in one struct and the
    // decision is taken once, inside the renderer -- including the ones this
    // pass already tracks, so the two cannot disagree about what a resize or a
    // scene revision implies (docs/architecture.md 4).
    hdclaude::FrameDescription description;
    description.width = width;
    description.height = height;
    description.camera = framing.camera;
    description.settings = settings;
    description.sceneRevision = framing.sceneRevision;
    // Reconstruction is honoured in interactive mode alone, by contract, so
    // asking for it is what selects the mode. Nothing else in this pass chooses
    // between the two.
    description.mode = reconstruction.on ? hdclaude::RenderMode::Interactive
                                         : hdclaude::RenderMode::Reference;

    // Camera rays are the pixels times the samples and need no counter to
    // know. What they go on to spawn -- a ray per surviving bounce and a shadow
    // ray per shading event -- is written on the device and is not counted;
    // that needs an accumulator in the counters buffer and one readback after
    // the frame, which is recorded rather than estimated.
    _renderDelegate->StageStats().cameraRays.fetch_add(
        static_cast<std::uint64_t>(width) * height * settings.samplesPerPixel,
        std::memory_order_relaxed);

    const auto start = std::chrono::steady_clock::now();
    hdclaude::FrameResult frame;
    try {
        frame = tracer->EndFrame(tracer->BeginFrame(description));
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

    // What the frame is a frame *of*, taken from the frame and not from
    // whatever the current extents happen to be.
    //
    // This is the check whose absence produced hdCodex A2, A1, N1 and N8: a
    // host that resized between the submit and the result would otherwise have
    // an image of one size written into a buffer of another, and nothing would
    // say so. It cannot fire today, because nothing overlaps yet and the frame
    // is finished before this line runs. It is here now so that it is already
    // right when something does.
    if (!frame.Valid() || frame.width != width || frame.height != height) {
        HdClaudeTrace("frame %llu was rendered at %ux%u but the buffer is now "
                      "%ux%u; dropping it",
                      static_cast<unsigned long long>(frame.index),
                      frame.width, frame.height, width, height);
        markConverged(noteFailure());
        return;
    }
    // Non-const: the exposure control below scales it in place, on the
    // resolved image, so the accumulated film keeps the radiance the renderer
    // computed.
    std::vector<float>& image = frame.image;

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

    // What actually reconstructed the frame, or why nothing did.
    //
    // Reported rather than left to be inferred from the picture, because the
    // two failures look alike: a mode nobody recognised and a backend that
    // could not run both leave an unreconstructed frame on screen, and at one
    // sample a frame both look like the renderer is broken. Said once per
    // distinct answer.
    if (reconstruction.on) {
        if (frame.reconstructed) {
            char described[320];
            std::snprintf(described, sizeof(described),
                          "%s, %s, %ux%u -> %ux%u, preset \"%s\", exposure "
                          "%.4g (0 means the backend measures its own)",
                          frame.reconstructionBackend.c_str(),
                          reconstructionName.c_str(), frame.renderWidth,
                          frame.renderHeight, frame.width, frame.height,
                          presetName.c_str(),
                          settings.reconstructionAutoExposure
                              ? 0.0
                              : double(tracer->LastReconstructionExposure()));
            if (_reportedBackend != described) {
                _reportedBackend = described;
                TF_STATUS("hdClaude: reconstructing with %s", described);
            }
        } else {
            const std::string reason = tracer->ReconstructionUnavailable();
            if (_reportedUnavailable != reason) {
                _reportedUnavailable = reason;
                TF_WARN(
                    "hdClaude: \"%s\" was asked for and no backend "
                    "reconstructed the frame: %s. The image is the interactive "
                    "estimate as traced, %u sample%s a frame, which is noisy by "
                    "construction rather than converged.",
                    reconstructionName.c_str(),
                    reason.empty() ? "no reason given" : reason.c_str(),
                    settings.samplesPerPixel,
                    settings.samplesPerPixel == 1 ? "" : "s");
            }
        }
    }

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
    // Depth only when a host asked for it, and untouched on the way: it is a
    // geometric measurement rather than a picture, so no exposure and no
    // transfer function apply to it.
    //
    // And only when it describes the same grid as the colour AOV. An upscaling
    // reconstruction traces smaller than it outputs, so the depth guide is at
    // `renderWidth` x `renderHeight` while the buffer is at the output extent;
    // writing one into the other reads a correct buffer at the wrong stride.
    // The guide is real and correct, it is simply not an AOV at this size, so
    // it is withheld rather than stretched into place.
    if (depthBuffer != nullptr && !frame.depth.empty()) {
        if (frame.renderWidth == width && frame.renderHeight == height) {
            depthBuffer->WriteScalar(frame.depth);
        } else {
            static bool warnedDepthExtent = false;
            if (!warnedDepthExtent) {
                warnedDepthExtent = true;
                TF_WARN(
                    "hdClaude: the depth AOV is not written while "
                    "reconstruction upscales -- the frame was traced at %ux%u "
                    "and the buffer is %ux%u. Use the DLAA mode, which traces "
                    "at the output extent.",
                    frame.renderWidth, frame.renderHeight, width, height);
            }
        }
    }
    // Taken from the tracer rather than accumulated here: it counts them on
    // the device and reads them back once, and a total kept on this side would
    // be a second answer to the same question.
    {
        HdClaudeStageStats& stages = _renderDelegate->StageStats();
        stages.tracedRays.store(tracer->TracedRays(), std::memory_order_relaxed);
        stages.shadowRays.store(tracer->ShadowRays(), std::memory_order_relaxed);
        stages.hitHash.store(tracer->HitHash(), std::memory_order_relaxed);
        stages.rayHash.store(tracer->RayHash(), std::memory_order_relaxed);
        const auto& kernels = tracer->LastKernelProfile();
        if (kernels.valid) {
            stages.kernelPrepareMs.store(kernels.prepareMs,
                                         std::memory_order_relaxed);
            stages.kernelExtendMs.store(kernels.extendMs,
                                        std::memory_order_relaxed);
            stages.kernelSortMs.store(kernels.sortMs, std::memory_order_relaxed);
            stages.kernelEnvironmentMs.store(kernels.environmentMs,
                                             std::memory_order_relaxed);
            stages.kernelShadeMs.store(kernels.shadeMs,
                                       std::memory_order_relaxed);
            stages.kernelShadowMs.store(kernels.shadowMs,
                                        std::memory_order_relaxed);
            stages.kernelFilmMs.store(kernels.filmMs, std::memory_order_relaxed);
        }
    }

    HdClaudeAddMilliseconds(_renderDelegate->StageStats().traceMilliseconds,
                            milliseconds);
    _renderDelegate->RecordFrameTiming(milliseconds, _samplesCompleted);

    // A line per traced frame, appended, for a session nobody can attach a
    // script to.
    //
    // The teardown report is cumulative and so cannot show a renderer getting
    // *faster as it runs*, which is the thing worth catching in an
    // interactive session: what matters there is the shape of the sequence,
    // not its total. Each line carries this frame's own trace time and the
    // rays and hashes it added, so a reader can see where a cost settles and
    // whether the answers settle with it.
    //
    // Opened and closed per line rather than held: an interactive session ends
    // when someone closes a window, and a buffered stream loses the last and
    // most interesting frames when it does.
    if (const std::string path = TfGetenv("HDCLAUDE_FRAME_LOG");
        !path.empty()) {
        const std::uint64_t traced = tracer->TracedRays();
        const std::uint64_t peak = _renderDelegate->PeakDeviceBytes();
        const std::uint64_t available =
            _renderDelegate->DeviceBytesAvailable();
        const std::uint64_t spilled =
            _renderDelegate->DeviceBytesSpilled();
        const auto& profile = tracer->LastKernelProfile();
        const double gpuSampleMs =
            profile.valid ? profile.prepareMs + profile.extendMs +
                                profile.sortMs + profile.environmentMs +
                                profile.shadeMs + profile.shadowMs +
                                profile.filmMs
                          : 0.0;
        HdClaudeStageStats& logStages = _renderDelegate->StageStats();
        const double subdivideNow =
            logStages.subdivideMilliseconds.load(std::memory_order_relaxed);
        const double publishNow =
            logStages.publishMilliseconds.load(std::memory_order_relaxed);
        const std::uint64_t shadow = tracer->ShadowRays();
        if (std::ofstream out{path, std::ios::app}; out) {
            if (_frameLogIndex == 0) {
                out << "# frame samples traceMs rays shadowRays hitHash "
                    << "rayHash width height deviceMiB availableMiB "
                    << "spilledMiB gpuSampleMs subdivideMs publishMs\n";
            }
            out << ++_frameLogIndex << ' ' << settings.samplesPerPixel
                << ' ' << std::fixed << std::setprecision(2)
                << milliseconds << std::defaultfloat << ' '
                << (traced - _frameLogTracedRays) << ' '
                << (shadow - _frameLogShadowRays) << ' '
                << tracer->HitHash() << ' ' << tracer->RayHash() << ' '
                << width << ' ' << height << ' '
                // The memory picture, per frame.
                //
                // A device-local allocation can be *evicted* to system
                // memory by the operating system when the card is
                // oversubscribed, and nothing in Vulkan reports that: the
                // allocation is still device-local as far as the API is
                // concerned, and the traversal reading it simply crosses
                // PCIe instead. The budget is the only thing visible from
                // here that moves when it happens, so it is on every line.
                << (peak >> 20) << ' ' << (available >> 20) << ' '
                << (spilled >> 20) << ' '
                // The GPU's own time for one sample, and the host work
                // that happened alongside this frame.
                //
                // `traceMs` is wall time on the render thread, so it counts
                // every stall as well as every dispatch. A frame whose GPU
                // time is small and whose wall time is not was waiting for
                // something, and the two columns beside it say what: Hydra
                // subdividing and publishing on the worker threads is host
                // work that competes with a render loop built out of
                // submit-and-wait.
                << std::fixed << std::setprecision(3) << gpuSampleMs
                << std::defaultfloat << ' ' << (subdivideNow - _frameLogSubdivideMs)
                << ' ' << (publishNow - _frameLogPublishMs) << '\n';
        }
        _frameLogTracedRays = traced;
        _frameLogShadowRays = shadow;
        _frameLogSubdivideMs = subdivideNow;
        _frameLogPublishMs = publishNow;
    }

    // The repeat diagnostic, decided here because this is where the image is
    // declared finished. It cannot live on the early return above: a host
    // stops calling Execute the moment IsConverged answers true, so a hook
    // that waits to be called again after convergence is never reached.
    //
    // What it buys is the only honest test of in-process reproducibility at
    // the sample count where it matters. Rendering a static stage at several
    // time codes does not test it -- nothing changes, so the accumulation is
    // correctly not reset and the later frames re-emit the first film
    // milliseconds apart -- and every other route to a second render also
    // rebuilds the acceleration structure, recompiles the shaders, or starts
    // another process.
    const bool finished = _samplesCompleted >= _targetSamples;
    if (finished) {
        if (!_repeatsStarted) {
            _repeatsStarted = true;
            const int repeats = TfGetenvInt("HDCLAUDE_REPEAT_RENDERS", 0);
            _repeatsAsked = repeats > 0;
            _repeatsRemaining =
                _repeatsAsked ? static_cast<std::uint32_t>(repeats) : 0u;
        }
        if (_repeatsAsked) {
            // Every render is reported, the last one included: reporting
            // only those with a repeat still to come would leave the final
            // render out of the comparison this exists for. To stderr rather
            // than through Tf, because a status message is silent unless
            // something installs a delegate to print it.
            // The trace time this render spent, taken as a delta so each
            // repeat reports its own rather than the running total. A second
            // render being faster than the first, with the same scene and
            // the same structure, is the question this answers.
            const double traceNow =
                _renderDelegate->StageStats().traceMilliseconds.load(
                    std::memory_order_relaxed);
            const double traceThis = traceNow - _repeatTraceMs;
            _repeatTraceMs = traceNow;
            std::fprintf(
                stderr,
                "hdClaude repeat: traceMs %.1f tracedRays %llu hitHash "
                "%llu rayHash %llu\n",
                traceThis,
                static_cast<unsigned long long>(tracer->TracedRays()),
                static_cast<unsigned long long>(tracer->HitHash()),
                static_cast<unsigned long long>(tracer->RayHash()));
            if (_repeatsRemaining > 0) {
                --_repeatsRemaining;
                _samplesCompleted = 0;
                tracer->ResetCounters();
                // Camera rays are counted on this side rather than by the
                // tracer, so resetting the tracer alone leaves them
                // summing across the repeats: three renders wrote a
                // cameraRays of exactly three times the truth into a
                // committed .stats. A diagnostic that quietly corrupts the
                // file it is measured beside is worse than no diagnostic.
                _renderDelegate->StageStats().cameraRays.store(
                    0, std::memory_order_relaxed);
                markConverged(false);
                return;
            }
        }
    }

    markConverged(finished);
}

PXR_NAMESPACE_CLOSE_SCOPE
