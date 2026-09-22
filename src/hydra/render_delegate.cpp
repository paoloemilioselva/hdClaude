#include "render_delegate.h"

#include "instancer.h"
#include "trace.h"

#include "camera.h"
#include "light.h"
#include "material.h"
#include "basis_curves.h"
#include "particle_field.h"
#include "mesh.h"
#include "render_buffer.h"
#include "render_pass.h"
#include "texture_loader.h"

#include "hdclaude/core/environment.h"
#include "hdclaude/core/sphere_mesh.h"

#include "pxr/base/plug/plugin.h"
#include "pxr/base/plug/registry.h"
#include "pxr/base/tf/diagnostic.h"
#include "pxr/base/tf/getenv.h"
#include "pxr/base/tf/staticTokens.h"
#include "pxr/imaging/hd/camera.h"
#include "pxr/imaging/hd/extComputation.h"
#include "pxr/imaging/hd/instancer.h"
#include "pxr/imaging/hd/resourceRegistry.h"
#include "pxr/imaging/hd/tokens.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

PXR_NAMESPACE_OPEN_SCOPE

TF_DEFINE_PRIVATE_TOKENS(_tokens,
                         ((mtlxRenderContext, "mtlx"))
                         (samplesPerPixel)
                         (maxBounces)
                         (samplesPerFrame)
                         (environmentIntensity)
                         (sunIntensity)
                         (upAxis)
                         (exposure)
                         (reconstruction)
                         (reconstructionPreset)
                         (reconstructionModel)
                         (reconstructionAutoExposure)
                         (lightGeometry)
                         (curveGeometry)
                         (splatTransport)
                         (device)
                         (shaderDirectory)
                         (statsReport)
                         (vulkanValidation)
                         (trace)
                         (profileKernels)
                         (poisonPathState)
                         (dumpShaders)
                         (frameLog)
                         (repeatRenders)
                         (republishAt)
                         (dlssRuntimeDirectory)
                         (curveSides)
                         (curveSegmentSamples)
                         (subdivisionLevel)
                         (adaptiveSubdivision)
                         (perFaceSubdivision)
                         (subdivisionEdgePixels)
                         (subdivisionOffScreenLevel)
                         (subdivisionFaceBudget)
                         (subdivisionFollowsCamera)
                         (retessellate)
                         (sphereRadial)
                         (sphereAxial)
                         (textureQuality)
                         (diffuseAlbedo)
                         (specularAlbedo)
                         (roughness));

namespace {

/// Which render setting stands for which environment variable.
///
/// One list rather than a convention, because the two names differ in spelling
/// and a convention that has to be remembered is a convention that drifts: an
/// audit found twelve variables with no setting beside them, every one of them
/// something worth flipping from a viewport rather than by restarting a process.
/// Adding a variable means adding a row here.
const std::vector<std::pair<TfToken, std::string>>& HdClaudeEnvironmentSettings()
{
    static const std::vector<std::pair<TfToken, std::string>> pairs = {
        {_tokens->samplesPerPixel, "HDCLAUDE_SAMPLES_PER_PIXEL"},
        {_tokens->samplesPerFrame, "HDCLAUDE_SAMPLES_PER_FRAME"},
        {_tokens->maxBounces, "HDCLAUDE_MAX_BOUNCES"},
        {_tokens->exposure, "HDCLAUDE_EXPOSURE"},
        {_tokens->environmentIntensity, "HDCLAUDE_ENVIRONMENT_INTENSITY"},
        {_tokens->sunIntensity, "HDCLAUDE_SUN_INTENSITY"},
        {_tokens->lightGeometry, "HDCLAUDE_LIGHT_GEOMETRY"},
        {_tokens->textureQuality, "HDCLAUDE_TEXTURE_QUALITY"},
        {_tokens->curveGeometry, "HDCLAUDE_CURVE_GEOMETRY"},
        {_tokens->curveSides, "HDCLAUDE_CURVE_SIDES"},
        {_tokens->curveSegmentSamples, "HDCLAUDE_CURVE_SEGMENT_SAMPLES"},
        {_tokens->subdivisionLevel, "HDCLAUDE_SUBDIVISION_LEVEL"},
        {_tokens->adaptiveSubdivision, "HDCLAUDE_ADAPTIVE_SUBDIVISION"},
        {_tokens->perFaceSubdivision, "HDCLAUDE_PER_FACE_SUBDIVISION"},
        {_tokens->subdivisionEdgePixels, "HDCLAUDE_SUBDIVISION_EDGE_PIXELS"},
        {_tokens->subdivisionOffScreenLevel,
         "HDCLAUDE_SUBDIVISION_OFFSCREEN_LEVEL"},
        {_tokens->subdivisionFaceBudget, "HDCLAUDE_SUBDIVISION_FACE_BUDGET"},
        {_tokens->subdivisionFollowsCamera,
         "HDCLAUDE_SUBDIVISION_FOLLOWS_CAMERA"},
        {_tokens->retessellate, "HDCLAUDE_RETESSELLATE"},
        {_tokens->sphereRadial, "HDCLAUDE_SPHERE_RADIAL"},
        {_tokens->sphereAxial, "HDCLAUDE_SPHERE_AXIAL"},
        {_tokens->splatTransport, "HDCLAUDE_SPLAT_TRANSPORT"},
        {_tokens->reconstruction, "HDCLAUDE_RECONSTRUCTION"},
        {_tokens->reconstructionPreset, "HDCLAUDE_RECONSTRUCTION_PRESET"},
        {_tokens->reconstructionModel, "HDCLAUDE_RECONSTRUCTION_MODEL"},
        {_tokens->reconstructionAutoExposure, "HDCLAUDE_DLSS_AUTO_EXPOSURE"},
        {_tokens->upAxis, "HDCLAUDE_UP_AXIS"},
        // The twelve the audit found.
        {_tokens->device, "HDCLAUDE_DEVICE"},
        {_tokens->shaderDirectory, "HDCLAUDE_SHADER_DIR"},
        {_tokens->statsReport, "HDCLAUDE_STATS_REPORT"},
        {_tokens->vulkanValidation, "HDCLAUDE_ENABLE_VULKAN_VALIDATION"},
        {_tokens->trace, "HDCLAUDE_TRACE"},
        {_tokens->profileKernels, "HDCLAUDE_PROFILE_KERNELS"},
        {_tokens->poisonPathState, "HDCLAUDE_POISON_PATH_STATE"},
        {_tokens->dumpShaders, "HDCLAUDE_DUMP_SHADERS"},
        {_tokens->frameLog, "HDCLAUDE_FRAME_LOG"},
        {_tokens->repeatRenders, "HDCLAUDE_REPEAT_RENDERS"},
        {_tokens->republishAt, "HDCLAUDE_REPUBLISH_AT"},
        {_tokens->dlssRuntimeDirectory, "HDCLAUDE_DLSS_RUNTIME_DIR"},
    };
    return pairs;
}

const TfTokenVector kSupportedRprimTypes = {
    HdPrimTypeTokens->mesh,
    // NurbsCurves arrive here too: UsdImaging's adapter reports them as
    // basisCurves with a linear basis, drawing the control cage rather than the
    // evaluated NURBS, so a stage's curves reach every Hydra renderer as
    // polylines.
    HdPrimTypeTokens->basisCurves,
    // UsdVolParticleField3DGaussianSplat, through UsdImaging's ParticleField
    // adapter, which is registered with `includeDerivedPrimTypes` and inserts
    // an rprim of this type. Nothing in the OpenUSD distribution renders one,
    // so what hdClaude draws comes from the schema (docs/gaussian-splats.md).
    HdPrimTypeTokens->particleField,
};

/// Light types hdClaude samples. A type absent from this list is never created
/// by Hydra, so an unsupported light is simply not in the scene rather than
/// present and ignored.
const TfTokenVector kSupportedLightTypes = {
    HdPrimTypeTokens->rectLight,     HdPrimTypeTokens->diskLight,
    HdPrimTypeTokens->sphereLight,   HdPrimTypeTokens->distantLight,
    HdPrimTypeTokens->cylinderLight, HdPrimTypeTokens->domeLight,
};

TfTokenVector MakeSupportedSprimTypes()
{
    TfTokenVector types = {
        HdPrimTypeTokens->camera,
        HdPrimTypeTokens->material,
        HdPrimTypeTokens->extComputation,
    };
    types.insert(types.end(), kSupportedLightTypes.begin(),
                 kSupportedLightTypes.end());
    return types;
}

const TfTokenVector kSupportedSprimTypes = MakeSupportedSprimTypes();

bool IsSupportedLightType(const TfToken& type)
{
    return std::find(kSupportedLightTypes.begin(), kSupportedLightTypes.end(),
                     type) != kSupportedLightTypes.end();
}

const TfTokenVector kSupportedBprimTypes = {
    HdPrimTypeTokens->renderBuffer,
};

/// Where the compute kernels live at run time.
///
/// An explicit environment override first, for development. Then the plugin
/// resource directory, so an installed hdClaude is self-contained. Then the
/// build-tree source directory, which is what a developer running from the
/// build gets without configuring anything.
std::filesystem::path ResolveShaderDirectory()
{
    const std::string overridePath =
        hdclaude::EnvironmentValue("HDCLAUDE_SHADER_DIR");
    if (!overridePath.empty()) {
        return std::filesystem::path(overridePath);
    }

    if (PlugPluginPtr plugin =
            PlugRegistry::GetInstance().GetPluginWithName("hdClaude")) {
        const std::filesystem::path resources =
            std::filesystem::path(plugin->GetResourcePath()) / "shaders";
        std::error_code code;
        if (std::filesystem::exists(resources / "shade.comp.glsl", code)) {
            return resources;
        }
    }

    return std::filesystem::path(HDCLAUDE_SHADER_DIR);
}

/// When this plugin's library was loaded.
///
/// A delegate's own timings account for a few seconds of a twenty-second render
/// of an empty stage, so the rest is spent either before it is constructed or
/// after it is destroyed. This is the earliest moment the plugin can observe,
/// and the difference between it and the constructor is everything Hydra, USD
/// and the loader did in between.
const std::chrono::steady_clock::time_point kLibraryLoaded =
    std::chrono::steady_clock::now();

}  // namespace

HdClaudeRenderDelegate::HdClaudeRenderDelegate() : HdRenderDelegate()
{
    Initialize({});
}

HdClaudeRenderDelegate::HdClaudeRenderDelegate(
    const HdRenderSettingsMap& settingsMap)
    : HdRenderDelegate(settingsMap)
{
    Initialize(settingsMap);
}

HdClaudeRenderDelegate::~HdClaudeRenderDelegate()
{
    // The stats report, before anything is released.
    //
    // Written to a file rather than to stdout because the caller that wants it
    // is a script, and a number it has to find in a renderer's console output
    // is a number that breaks the first time anything else prints. Absent the
    // environment variable this costs nothing and says nothing, which is what
    // an ordinary render should get.
    if (const std::string path =
            hdclaude::EnvironmentValue("HDCLAUDE_STATS_REPORT");
        !path.empty()) {
        const std::uint64_t peak =
            _peakDeviceBytes.load(std::memory_order_relaxed);
        const std::uint64_t available =
            _allocator ? _allocator->DeviceLocalBytesAvailable() : 0;
        const std::uint64_t spilled =
            _allocator ? _allocator->DeviceLocalBytesSpilled() : 0;
        if (std::ofstream out{path}; out) {
            const auto& stages = _stageStats;
            out << "deviceBytesPeak " << peak << '\n'
                << "deviceBytesSpilled " << spilled << '\n'
                << "deviceBytesAvailable " << available << '\n'
                << "ingestMs "
                << stages.ingestMilliseconds.load(std::memory_order_relaxed)
                << '\n'
                << "publishMs "
                << stages.publishMilliseconds.load(std::memory_order_relaxed)
                << '\n'
                << "instances "
                << stages.instances.load(std::memory_order_relaxed) << '\n'
                << "triangles "
                << stages.triangles.load(std::memory_order_relaxed) << '\n'
                << "blasBuilt "
                << stages.blasBuilt.load(std::memory_order_relaxed)
                << '\n'
                << "blasRefit "
                << stages.blasRefit.load(std::memory_order_relaxed)
                << '\n'
                << "blasReused "
                << stages.blasReused.load(std::memory_order_relaxed)
                << '\n'
                << "cameraRays "
                << stages.cameraRays.load(std::memory_order_relaxed) << '\n'
                << "tracedRays "
                << stages.tracedRays.load(std::memory_order_relaxed) << '\n'
                << "shadowRays "
                << stages.shadowRays.load(std::memory_order_relaxed) << '\n'
                << "hitHash "
                << stages.hitHash.load(std::memory_order_relaxed) << '\n'
                << "rayHash "
                << stages.rayHash.load(std::memory_order_relaxed) << '\n'
                << "traceMs "
                << stages.traceMilliseconds.load(std::memory_order_relaxed)
                << '\n'
                << "kernelPrepareMs "
                << stages.kernelPrepareMs.load(std::memory_order_relaxed)
                << '\n'
                << "kernelExtendMs "
                << stages.kernelExtendMs.load(std::memory_order_relaxed)
                << '\n'
                << "kernelSortMs "
                << stages.kernelSortMs.load(std::memory_order_relaxed)
                << '\n'
                << "kernelEnvironmentMs "
                << stages.kernelEnvironmentMs.load(std::memory_order_relaxed)
                << '\n'
                << "kernelShadeMs "
                << stages.kernelShadeMs.load(std::memory_order_relaxed)
                << '\n'
                << "kernelShadowMs "
                << stages.kernelShadowMs.load(std::memory_order_relaxed)
                << '\n'
                << "kernelFilmMs "
                << stages.kernelFilmMs.load(std::memory_order_relaxed)
                << '\n'
                << "subdivideMs "
                << stages.subdivideMilliseconds.load(std::memory_order_relaxed)
                << '\n'
                << "meshesRefined "
                << stages.meshesRefined.load(std::memory_order_relaxed) << '\n'
                << "subdivideInputPoints "
                << stages.subdivideInputPoints.load(std::memory_order_relaxed)
                << '\n'
                << "subdivideOutputPoints "
                << stages.subdivideOutputPoints.load(std::memory_order_relaxed)
                << '\n'
                << "materialMs "
                << stages.materialMilliseconds.load(std::memory_order_relaxed)
                << '\n'
                << "materialsCompiled "
                << stages.materialsCompiled.load(std::memory_order_relaxed)
                << '\n'
                << "textureMs "
                << stages.textureMilliseconds.load(std::memory_order_relaxed)
                << '\n'
                << "texturesLoaded "
                << stages.texturesLoaded.load(std::memory_order_relaxed) << '\n'
                << "textureBytes "
                << stages.textureBytes.load(std::memory_order_relaxed) << '\n'
                << "startupVulkanMs "
                << stages.startupVulkanMs.load(std::memory_order_relaxed) << '\n'
                << "startupKernelsMs "
                << stages.startupKernelsMs.load(std::memory_order_relaxed)
                << '\n'
                << "startupMaterialXMs "
                << stages.startupMaterialXMs.load(std::memory_order_relaxed)
                << '\n'
                << "startupFallbackMs "
                << stages.startupFallbackMs.load(std::memory_order_relaxed)
                << '\n';
        } else {
            TF_WARN("hdClaude: could not write the stats report to '%s'",
                    path.c_str());
        }
    }

    // Reverse of construction. The path tracer holds buffers owned by the
    // allocator, which belongs to the device; releasing them out of order
    // leaks device memory that only surfaces as a validation message at
    // vkDestroyDevice (docs/implementation-notes.md).
    const auto teardownStart = std::chrono::steady_clock::now();
    _pathTracer.reset();
    _allocator.reset();
    _context.reset();
    // Timed because it is wall time a user waits through: a render of an empty
    // stage costs twenty seconds of which the delegate's own startup is under
    // five, and teardown is one of the two places the rest can be.
    HdClaudeTrace("teardown: %.0f ms releasing the device",
                  std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - teardownStart)
                      .count());
}

void HdClaudeRenderDelegate::Initialize(const HdRenderSettingsMap& settingsMap)
{
    _store = std::make_unique<HdClaudeSceneStore>();
    _texturePool = std::make_unique<HdClaudeTexturePool>();
    // The cap is set before any material syncs, not when the render pass first
    // reads the setting: a stage whose images do not fit in memory has to be
    // reduced as it loads, and by then the first of them is already decoded.
    _texturePool->SetMaxEdge(
        HdClaudeTextureEdgeCap(TfGetenv("HDCLAUDE_TEXTURE_QUALITY", "high")));
    _resourceRegistry = std::make_shared<HdResourceRegistry>();

    // Seed the defaults for anything the host did not set, so a
    // GetRenderSetting call never falls through to a hard-coded literal at the
    // point of use and disagree with what the settings UI is showing.
    for (const HdRenderSettingDescriptor& descriptor :
         GetRenderSettingDescriptors()) {
        if (settingsMap.find(descriptor.key) == settingsMap.end()) {
            _settingsMap[descriptor.key] = descriptor.defaultValue;
        }
    }

    // Every `HDCLAUDE_*` variable is also a render setting, and this is where
    // the two are reconciled for the layers that cannot ask Hydra anything.
    //
    // `src/gpu` and `src/core` read the environment directly, by design: they
    // know nothing about OpenUSD and must not. So the delegate resolves each
    // setting here -- host value first, environment second -- and pushes the
    // answer into the override those layers consult, before the Vulkan context
    // is built and before anything below caches a flag. A viewport session and
    // a batch render are then configured identically, which is the whole point
    // of the pairing.
    for (const auto& [setting, variable] : HdClaudeEnvironmentSettings()) {
        const VtValue value = _settingsMap.count(setting) != 0
                                  ? _settingsMap[setting]
                                  : VtValue();
        std::string text;
        if (value.IsHolding<std::string>()) {
            text = value.UncheckedGet<std::string>();
        } else if (value.IsHolding<bool>()) {
            text = value.UncheckedGet<bool>() ? "1" : "0";
        } else if (value.IsHolding<int>()) {
            text = std::to_string(value.UncheckedGet<int>());
        }
        hdclaude::SetEnvironmentOverride(variable, text);
    }

    try {
        hdclaude::VulkanContextOptions options;
        // Through `EnvironmentFlag`, which every layer shares, so that this
        // agrees with the trace about what a value means -- including the
        // trailing space cmd leaves on `set VAR=1 && program`.
        options.enableValidation =
            hdclaude::EnvironmentFlag("HDCLAUDE_ENABLE_VULKAN_VALIDATION");
        options.preferredDeviceName = hdclaude::EnvironmentValue("HDCLAUDE_DEVICE");

        // NGX will not initialise without instance and device extensions of its
        // own, and they can only be enabled while the instance and the device
        // are being created -- so an interactive frame that asks to be
        // reconstructed, much later, cannot arrange them. Installing the
        // provider here is what makes reconstruction possible at all; the
        // decision to *use* it stays with the render settings.
        //
        // Unconditional, and safe to be: without the SDK it names nothing, and
        // the context enables only extensions the chosen device actually has.
        // It is consulted during bootstrap alone, so a local outliving the
        // constructor is all its lifetime has to cover.
        const hdclaude::NgxRequirementProvider ngxProvider;
        options.requirementProviders.push_back(&ngxProvider);

        const auto vulkanStart = std::chrono::steady_clock::now();
        _context = std::make_unique<hdclaude::VulkanContext>(options);
        _allocator = std::make_unique<hdclaude::VulkanAllocator>(*_context);
        const auto kernelsStart = std::chrono::steady_clock::now();
        HdClaudeAddMilliseconds(
            _stageStats.startupVulkanMs,
            std::chrono::duration<double, std::milli>(kernelsStart - vulkanStart)
                .count());

        _pathTracer = std::make_unique<hdclaude::PathTracer>(
            *_context, *_allocator, ResolveShaderDirectory());
        HdClaudeAddMilliseconds(
            _stageStats.startupKernelsMs,
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - kernelsStart)
                .count());
    } catch (const std::exception& error) {
        // Reported, not thrown. Hydra creates a delegate while switching
        // renderers inside a running usdview; an exception here takes the
        // application down instead of showing an empty viewport and a reason.
        _initializationError =
            std::string("the Vulkan backend did not start: ") + error.what();
        TF_RUNTIME_ERROR("hdClaude: %s", _initializationError.c_str());
        _pathTracer.reset();
        _allocator.reset();
        _context.reset();
    }

    if (_pathTracer) {
        const auto materialXStart = std::chrono::steady_clock::now();
        _materialCompiler = std::make_unique<HdClaudeMaterialCompiler>(
            _pathTracer->ShadeKernelSource(),
            _pathTracer->DisplaceKernelSource());
        const auto fallbackStart = std::chrono::steady_clock::now();
        HdClaudeAddMilliseconds(
            _stageStats.startupMaterialXMs,
            std::chrono::duration<double, std::milli>(fallbackStart -
                                                      materialXStart)
                .count());

        // The fallback is compiled up front so that a material failure during
        // Sync has something to fall back *to*. Compiling it lazily on first
        // failure would put the compile at exactly the moment the renderer is
        // already in trouble.
        hdclaude::CompiledMaterial fallback = _materialCompiler->CompileDiffuse(
            GfVec3f(0.5f, 0.5f, 0.5f), "hdclaude_fallback");
        if (fallback.spirv.empty()) {
            _initializationError =
                "the fallback material did not compile; see the errors above";
        } else {
            _store->SetFallbackMaterial(std::move(fallback));
        }
        HdClaudeAddMilliseconds(
            _stageStats.startupFallbackMs,
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - fallbackStart)
                .count());
        HdClaudeTrace(
            "startup: %.0f ms before this delegate was constructed, then "
            "%.0f ms vulkan, %.0f ms kernels, %.0f ms materialx, "
            "%.0f ms fallback material",
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - kLibraryLoaded)
                    .count() -
                _stageStats.startupVulkanMs.load(std::memory_order_relaxed) -
                _stageStats.startupKernelsMs.load(std::memory_order_relaxed) -
                _stageStats.startupMaterialXMs.load(std::memory_order_relaxed) -
                _stageStats.startupFallbackMs.load(std::memory_order_relaxed),
            _stageStats.startupVulkanMs.load(std::memory_order_relaxed),
            _stageStats.startupKernelsMs.load(std::memory_order_relaxed),
            _stageStats.startupMaterialXMs.load(std::memory_order_relaxed),
            _stageStats.startupFallbackMs.load(std::memory_order_relaxed));
    }

    // Read once, at construction: a change of refinement level changes the
    // geometry every mesh publishes, so it is a delegate-wide decision rather
    // than something the render pass can vary per frame. Changing it means a
    // new delegate, which is what a host does when it applies a render setting
    // that alters the scene.
    HdClaudeTessellationSettings tessellation =
        HdClaudeReadTessellationEnvironment();
    // Six faces round a tube. Enough that a whisker reads as round at the size
    // curves are usually authored, cheap enough that a head of hair does not
    // pay for a smoothness nothing can see, and a setting because the right
    // answer depends on how close the camera gets.
    const int curveSides =
        std::clamp(TfGetenvInt("HDCLAUDE_CURVE_SIDES", 6), 3, 64);
    // How many straight spans a cubic curve segment becomes.
    //
    // One, and one is not the same as not evaluating: it puts both ends of
    // every segment on the actual curve, which for a B-spline is nowhere near
    // its control polygon. Hair is authored at several segments a strand and is
    // thinner than a pixel at any sane distance, so the spans are already
    // shorter than the geometry they approximate; raising this multiplies the
    // swept triangle count by exactly the same factor, and a head of fur is
    // measured in millions of segments before it starts.
    const int curveSegmentSamples =
        std::clamp(TfGetenvInt("HDCLAUDE_CURVE_SEGMENT_SAMPLES", 1), 1, 32);

    // Curves as segments the traversal kernel intersects, or as tubes of
    // triangles.
    //
    // Implicit by default. It is the exact shape rather than a faceted
    // approximation, about thirty-two bytes a segment against five hundred, and
    // on ALab's groom it is 4.6 GiB against 14.2 and 2.9 s of publish against
    // 23.1 -- on an asset whose swept form does not fit on the card, the
    // difference between a render and none.
    //
    // It was the default for a while with a crescent at every segment joint,
    // which beaded ALab's knitted yarn and patterned its fur. That was the
    // traversal kernel committing a farther generated hit over a nearer one,
    // and is fixed (roadmap open question 5). `swept` stays one setting away.
    const std::string curveGeometry =
        TfGetenv("HDCLAUDE_CURVE_GEOMETRY", "implicit");
    const bool implicitCurves = curveGeometry != "swept";

    _renderParam = std::make_unique<HdClaudeRenderParam>(
        _store.get(), _materialCompiler.get(), _texturePool.get(),
        tessellation, curveSides, curveSegmentSamples, implicitCurves,
        &_stageStats);
}

const TfTokenVector& HdClaudeRenderDelegate::GetSupportedRprimTypes() const
{
    return kSupportedRprimTypes;
}

const TfTokenVector& HdClaudeRenderDelegate::GetSupportedSprimTypes() const
{
    return kSupportedSprimTypes;
}

const TfTokenVector& HdClaudeRenderDelegate::GetSupportedBprimTypes() const
{
    return kSupportedBprimTypes;
}

TfTokenVector HdClaudeRenderDelegate::GetShaderSourceTypes() const
{
    // "mtlx" only. hdClaude shades exclusively through MaterialX code
    // generation, and claiming "glslfx" would invite Hydra to hand over
    // networks this renderer has no way to execute.
    return {_tokens->mtlxRenderContext};
}

TfTokenVector HdClaudeRenderDelegate::GetMaterialRenderContexts() const
{
    return {_tokens->mtlxRenderContext};
}

TfToken HdClaudeRenderDelegate::GetMaterialBindingPurpose() const
{
    return HdTokens->full;
}

HdRenderParam* HdClaudeRenderDelegate::GetRenderParam() const
{
    return _renderParam.get();
}

HdResourceRegistrySharedPtr HdClaudeRenderDelegate::GetResourceRegistry() const
{
    return _resourceRegistry;
}

HdRenderPassSharedPtr HdClaudeRenderDelegate::CreateRenderPass(
    HdRenderIndex* index, const HdRprimCollection& collection)
{
    return std::make_shared<HdClaudeRenderPass>(index, collection, this);
}

HdInstancer* HdClaudeRenderDelegate::CreateInstancer(HdSceneDelegate* delegate,
                                                     const SdfPath& id)
{
    // hdClaude's own, because the stock HdInstancer computes nothing: it holds
    // the primvars and the parent chain and leaves the composition to the
    // renderer. Returning the base class renders every point-instanced
    // prototype exactly once (src/hydra/instancer.h).
    return new HdClaudeInstancer(delegate, id);
}

void HdClaudeRenderDelegate::DestroyInstancer(HdInstancer* instancer)
{
    delete instancer;
}

HdRprim* HdClaudeRenderDelegate::CreateRprim(const TfToken& typeId,
                                             const SdfPath& rprimId)
{
    if (typeId == HdPrimTypeTokens->mesh) {
        return new HdClaudeMesh(rprimId);
    }
    if (typeId == HdPrimTypeTokens->basisCurves) {
        return new HdClaudeBasisCurves(rprimId);
    }
    if (typeId == HdPrimTypeTokens->particleField) {
        return new HdClaudeParticleField(rprimId);
    }
    TF_WARN("hdClaude: unsupported rprim type <%s>", typeId.GetText());
    return nullptr;
}

void HdClaudeRenderDelegate::DestroyRprim(HdRprim* rprim) { delete rprim; }

HdSprim* HdClaudeRenderDelegate::CreateSprim(const TfToken& typeId,
                                             const SdfPath& sprimId)
{
    if (typeId == HdPrimTypeTokens->camera) {
        return new HdClaudeCamera(sprimId);
    }
    if (typeId == HdPrimTypeTokens->material) {
        return new HdClaudeMaterial(sprimId);
    }
    if (typeId == HdPrimTypeTokens->extComputation) {
        // The stock prim is the whole implementation, and it is what makes
        // UsdSkel work: skinning arrives as an ExtComputation whose result is
        // the mesh points. Declining it would leave every deforming character
        // in its bind pose.
        return new HdExtComputation(sprimId);
    }
    if (IsSupportedLightType(typeId)) {
        return new HdClaudeLight(typeId, sprimId);
    }
    TF_WARN("hdClaude: unsupported sprim type <%s>", typeId.GetText());
    return nullptr;
}

HdSprim* HdClaudeRenderDelegate::CreateFallbackSprim(const TfToken& typeId)
{
    if (typeId == HdPrimTypeTokens->camera) {
        return new HdClaudeCamera(SdfPath::EmptyPath());
    }
    if (typeId == HdPrimTypeTokens->material) {
        return new HdClaudeMaterial(SdfPath::EmptyPath());
    }
    if (typeId == HdPrimTypeTokens->extComputation) {
        return new HdExtComputation(SdfPath::EmptyPath());
    }
    if (IsSupportedLightType(typeId)) {
        return new HdClaudeLight(typeId, SdfPath::EmptyPath());
    }
    return nullptr;
}

void HdClaudeRenderDelegate::DestroySprim(HdSprim* sprim) { delete sprim; }

HdBprim* HdClaudeRenderDelegate::CreateBprim(const TfToken& typeId,
                                             const SdfPath& bprimId)
{
    if (typeId == HdPrimTypeTokens->renderBuffer) {
        return new HdClaudeRenderBuffer(bprimId);
    }
    TF_WARN("hdClaude: unsupported bprim type <%s>", typeId.GetText());
    return nullptr;
}

HdBprim* HdClaudeRenderDelegate::CreateFallbackBprim(const TfToken& typeId)
{
    if (typeId == HdPrimTypeTokens->renderBuffer) {
        return new HdClaudeRenderBuffer(SdfPath::EmptyPath());
    }
    return nullptr;
}

void HdClaudeRenderDelegate::DestroyBprim(HdBprim* bprim) { delete bprim; }

void HdClaudeRenderDelegate::CommitResources(HdChangeTracker* /*tracker*/)
{
    // Nothing to do: prims publish into the scene store during Sync, and the
    // render pass takes a snapshot when the revision changes.
}

HdAovDescriptor HdClaudeRenderDelegate::GetDefaultAovDescriptor(
    const TfToken& name) const
{
    if (name == HdAovTokens->color) {
        // Float32 rather than UNorm8: the output is linear and unbounded, and
        // quantising it in the delegate would throw the range away before any
        // display transform gets to see it.
        return HdAovDescriptor(HdFormatFloat32Vec4, false,
                               VtValue(GfVec4f(0.0f)));
    }
    if (name == HdAovTokens->depth) {
        return HdAovDescriptor(HdFormatFloat32, false, VtValue(1.0f));
    }
    // The reconstruction guides, as a host can ask for them by name and look
    // at them (docs/dlss-integration.md 4). Clear values are NVIDIA's sky
    // defaults, so a pixel nothing was drawn into reads as sky reads.
    if (name == HdAovTokens->normal || name == _tokens->specularAlbedo) {
        return HdAovDescriptor(HdFormatFloat32Vec3, false,
                               VtValue(GfVec3f(0.0f)));
    }
    if (name == _tokens->diffuseAlbedo) {
        return HdAovDescriptor(HdFormatFloat32Vec3, false,
                               VtValue(GfVec3f(0.5f)));
    }
    if (name == _tokens->roughness) {
        return HdAovDescriptor(HdFormatFloat32, false, VtValue(0.0f));
    }
    if (name == HdAovTokens->primId || name == HdAovTokens->instanceId ||
        name == HdAovTokens->elementId) {
        return HdAovDescriptor(HdFormatInt32, false, VtValue(-1));
    }
    return HdAovDescriptor();
}

HdRenderSettingDescriptorList
HdClaudeRenderDelegate::GetRenderSettingDescriptors() const
{
    // Environment variables supply the defaults so the gallery scripts can set
    // them without a host that exposes a settings UI; render_claude.bat
    // documents the same names. A host that sets a value explicitly always
    // wins, because Initialize only fills in keys the host left unset.
    return {
        {"Samples per pixel", _tokens->samplesPerPixel,
         VtValue(TfGetenvInt("HDCLAUDE_SAMPLES_PER_PIXEL", 64))},
        // Eight, not four. A glass ball needs an entry, an exit, and whatever
        // it refracts through behind it; at four bounces transmissive
        // materials go dark and read as a shading bug rather than as a depth
        // limit. render_claude.bat documents the same default.
        {"Max bounces", _tokens->maxBounces,
         VtValue(TfGetenvInt("HDCLAUDE_MAX_BOUNCES", 8))},
        {"Samples per frame", _tokens->samplesPerFrame,
         VtValue(TfGetenvInt("HDCLAUDE_SAMPLES_PER_FRAME", 4))},

        // The stand-in sun and sky, until UsdLux lights are consumed. They are
        // real render settings rather than hidden constants because without
        // them an enclosed set -- which is what every studio-lit gallery scene
        // is -- renders black with no way for a user to tell whether the
        // geometry or the lighting is at fault.
        {"Environment intensity", _tokens->environmentIntensity,
         VtValue(float(TfGetenvDouble("HDCLAUDE_ENVIRONMENT_INTENSITY", 1.0)))},
        {"Sun intensity", _tokens->sunIntensity,
         VtValue(float(TfGetenvDouble("HDCLAUDE_SUN_INTENSITY", 1.0)))},

        // Which way is up, for aiming the stand-in sun.
        //
        // A Hydra scene delegate is not told the stage's up axis: usdImaging
        // passes the world as authored and there is no `HdTokens` for it. So a
        // renderer that wants to put its default sun overhead has to be told,
        // and the alternative to a setting is a hardcoded axis that is simply
        // wrong for half of USD -- hdClaude's own gallery has a Z-up scene, and
        // a Y-up sun in it shines sideways along the floor.
        {"Up axis", _tokens->upAxis,
         VtValue(std::string(TfGetenv("HDCLAUDE_UP_AXIS", "Y")))},

        // Exposure, in stops, applied to the resolved image. Zero by default,
        // so the AOV carries the radiance the renderer computed and nothing is
        // baked in; a host with its own display transform is unaffected. It
        // exists because a physically bright environment -- a studio HDRI, say
        // -- is genuinely blown out at unit exposure, and the alternative to a
        // control is quietly scaling the lighting instead.
        {"Exposure", _tokens->exposure,
         VtValue(float(TfGetenvDouble("HDCLAUDE_EXPOSURE", 0.0)))},

        // What the geometry *is*, rather than how a frame of it is traced.
        //
        // These four rebuild rprims when they change rather than taking effect
        // on the next frame, which is why the render pass resyncs the stage
        // when it sees one of them move. They are settings and not only
        // environment variables because the interesting thing to do with them
        // is flip one in a viewport and look: swept against implicit on a real
        // groom is a comparison no still frame makes for you.
        //
        // Accepted: swept, implicit. Implicit is the default: it is exact and
        // far lighter.
        {"Curve geometry", _tokens->curveGeometry,
         VtValue(std::string(TfGetenv("HDCLAUDE_CURVE_GEOMETRY", "implicit")))},
        {"Curve sides", _tokens->curveSides,
         VtValue(TfGetenvInt("HDCLAUDE_CURVE_SIDES", 6))},
        {"Curve segment samples", _tokens->curveSegmentSamples,
         VtValue(TfGetenvInt("HDCLAUDE_CURVE_SEGMENT_SAMPLES", 1))},
        // The refinement depth. A ceiling rather than the answer once
        // `Adaptive subdivision` is on.
        {"Subdivision level", _tokens->subdivisionLevel,
         VtValue(TfGetenvInt("HDCLAUDE_SUBDIVISION_LEVEL", 2))},

        // Whether each mesh gets the level its projected size earns.
        //
        // Off by default, because turning it on changes the geometry of every
        // scene whose meshes are at more than one distance, and a default that
        // quietly makes every committed image different is the wrong default.
        // What it buys is spending refinement where it can be seen: a mesh
        // three pixels across and a mesh filling the frame are refined the
        // same amount without it.
        {"Adaptive subdivision", _tokens->adaptiveSubdivision,
         VtValue(hdclaude::EnvironmentFlag("HDCLAUDE_ADAPTIVE_SUBDIVISION", false))},

        // Whether each face, rather than each mesh, gets its own rate.
        //
        // A mesh is one level's worth of detail everywhere, which is the wrong
        // answer for anything that spans a range of distances by itself: a
        // ground plane, a terrain, a floor. This gives each side of each face
        // the rate its own depth earns. It implies `Adaptive subdivision`,
        // since the rates come from the sampled view, and it changes where the
        // positions come from: the limit surface rather than a refined cage,
        // which is what lets neighbouring faces be tessellated differently
        // without a crack between them.
        {"Per-face subdivision", _tokens->perFaceSubdivision,
         VtValue(hdclaude::EnvironmentFlag("HDCLAUDE_PER_FACE_SUBDIVISION",
                                           false))},

        // How long a refined edge should be on screen, in pixels. Smaller is
        // finer, and each halving is one more level.
        {"Subdivision edge pixels", _tokens->subdivisionEdgePixels,
         VtValue(static_cast<float>(
             TfGetenvDouble("HDCLAUDE_SUBDIVISION_EDGE_PIXELS", 4.0)))},

        // What a mesh entirely outside the frustum is held at.
        //
        // One rather than zero, and that is the whole difference between a
        // reduction and a cull: a path tracer sees geometry the camera does
        // not -- in a mirror, through glass, as a shadow, and in every
        // indirect bounce -- so dropping off-screen meshes to their control
        // cage is a defect that appears only in the scenes that have mirrors.
        {"Off-screen subdivision level", _tokens->subdivisionOffScreenLevel,
         VtValue(TfGetenvInt("HDCLAUDE_SUBDIVISION_OFFSCREEN_LEVEL", 1))},

        // The most refined faces one mesh may be given, adaptive or not.
        //
        // A cap on the *level* gives every mesh the same answer whether or not
        // it needed it; a cap on the faces says which mesh it applied to and
        // what that mesh wanted, which is what makes it actionable.
        {"Subdivision face budget", _tokens->subdivisionFaceBudget,
         VtValue(TfGetenvInt("HDCLAUDE_SUBDIVISION_FACE_BUDGET",
                             4 * 1024 * 1024))},

        // Whether a camera move re-derives the levels.
        //
        // Off, deliberately. Published geometry is what acceleration
        // structures are built over and what the accumulated film depends on,
        // so following the camera would rebuild both on every viewport nudge
        // and throw away the prototype reuse the fingerprints exist for. The
        // view is sampled once and held; `Retessellate` asks for another
        // sample explicitly.
        {"Subdivision follows camera", _tokens->subdivisionFollowsCamera,
         VtValue(hdclaude::EnvironmentFlag("HDCLAUDE_SUBDIVISION_FOLLOWS_CAMERA",
                                         false))},

        // Bump this to derive the levels again against the camera as it is
        // now. Any change is the request; the value itself means nothing.
        {"Retessellate", _tokens->retessellate,
         VtValue(TfGetenvInt("HDCLAUDE_RETESSELLATE", 0))},

        // How many divisions a `UsdGeomSphere` becomes, round and pole to
        // pole.
        //
        // Ten and ten, which is what OpenUSD's implicit-surface scene index
        // uses as a pair of `static constexpr` values -- so the default cage
        // is the one hdClaude has always traced, point for point, and the only
        // new thing on a sphere is its texture coordinates. Raise them for a
        // sphere that fills the frame or carries a displacement: a control
        // cage of ten by ten is smooth at its limit but coarse as a surface to
        // displace.
        {"Sphere radial divisions", _tokens->sphereRadial,
         VtValue(TfGetenvInt("HDCLAUDE_SPHERE_RADIAL",
                             hdclaude::kDefaultSphereRadial))},
        {"Sphere axial divisions", _tokens->sphereAxial,
         VtValue(TfGetenvInt("HDCLAUDE_SPHERE_AXIAL",
                             hdclaude::kDefaultSphereAxial))},

        // How much of each texture is kept, as a cap on its longest edge.
        //
        // The scene's images are routinely the largest thing a stage holds:
        // ALab's 6,261 of them come to 49.6 GB, which is more than this
        // machine has, and no amount of geometry in that stage approaches it.
        // A cap on the longest edge is what makes the total independent of how
        // large the originals happened to be.
        //
        // High is the authored image and is the default, because a setting
        // that quietly changed what an asset says is what the renderer is for
        // would be the wrong default: every reduction is a different picture,
        // and the one a scene authored is the one worth defaulting to.
        //
        // Accepted: high (authored), medium (1024), low (256).
        {"Texture quality", _tokens->textureQuality,
         VtValue(std::string(TfGetenv("HDCLAUDE_TEXTURE_QUALITY", "high")))},

        // Whether a light's own shape is rendered.
        //
        // Off, because a `UsdLux` light usually stands in for a fixture the
        // asset also models, and the bare rectangle beside the lamp it
        // represents is a shape nothing in the scene meant to show. Off
        // overrides whatever the asset authored per light rather than combining
        // with it, so a scene cannot put geometry into a render that asked for
        // none; on hands the choice back to each light.
        // How a Gaussian splat cloud is transported, and this is not a quality
        // dial: the two readings of the schema are different pictures.
        //
        // `coverage` estimates the alpha compositing `UsdVolParticleField`
        // defines, which is the appearance the asset was trained for and needs
        // no constant the schema does not supply. `volume` reads the kernel as a
        // density and lets a path travel through the cloud, which is what makes
        // splats participate in transport -- and needs a length scale the schema
        // does not give, so it looks different (docs/gaussian-splats.md 7).
        //
        // Accepted: coverage, volume. Coverage is the default, because it is the
        // format's own answer to the question.
        // Diagnostics and bootstrap. None of these changes an image except by
        // changing which device draws it, and every one of them is a thing
        // worth flipping in a viewport rather than by restarting a process with
        // a different environment -- which is the only reason they were ever
        // environment variables alone.
        {"Device", _tokens->device,
         VtValue(std::string(TfGetenv("HDCLAUDE_DEVICE", "")))},
        {"Shader directory", _tokens->shaderDirectory,
         VtValue(std::string(TfGetenv("HDCLAUDE_SHADER_DIR", "")))},
        {"Stats report", _tokens->statsReport,
         VtValue(std::string(TfGetenv("HDCLAUDE_STATS_REPORT", "")))},
        {"Vulkan validation", _tokens->vulkanValidation,
         VtValue(hdclaude::EnvironmentFlag("HDCLAUDE_ENABLE_VULKAN_VALIDATION"))},
        {"Trace", _tokens->trace,
         VtValue(hdclaude::EnvironmentFlag("HDCLAUDE_TRACE"))},
        {"Profile kernels", _tokens->profileKernels,
         VtValue(hdclaude::EnvironmentFlag("HDCLAUDE_PROFILE_KERNELS"))},
        {"Poison path state", _tokens->poisonPathState,
         VtValue(hdclaude::EnvironmentFlag("HDCLAUDE_POISON_PATH_STATE"))},
        {"Dump shaders", _tokens->dumpShaders,
         VtValue(std::string(TfGetenv("HDCLAUDE_DUMP_SHADERS", "")))},
        {"Frame log", _tokens->frameLog,
         VtValue(std::string(TfGetenv("HDCLAUDE_FRAME_LOG", "")))},
        {"Repeat renders", _tokens->repeatRenders,
         VtValue(TfGetenvInt("HDCLAUDE_REPEAT_RENDERS", 0))},
        {"Republish at", _tokens->republishAt,
         VtValue(TfGetenvInt("HDCLAUDE_REPUBLISH_AT", 0))},
        {"DLSS runtime directory", _tokens->dlssRuntimeDirectory,
         VtValue(std::string(TfGetenv("HDCLAUDE_DLSS_RUNTIME_DIR", "")))},
        {"Splat transport", _tokens->splatTransport,
         VtValue(std::string(TfGetenv("HDCLAUDE_SPLAT_TRANSPORT", "coverage")))},
        {"Light geometry", _tokens->lightGeometry,
         VtValue(TfGetenvBool("HDCLAUDE_LIGHT_GEOMETRY", false))},

        // Reconstruction, and which model reconstructs.
        //
        // Strings rather than an enum because Hydra render settings carry
        // `VtValue` and a host renders a string as an editable field, which is
        // the only control usdview offers for something that is not a number or
        // a checkbox. The same names work as environment variables, so a batch
        // render and a viewport session are configured identically.
        //
        // "off" is a reference render: the progressive accumulation this
        // renderer has always done, converging to the truth. Everything else
        // switches the pass to interactive frames handed to a reconstruction
        // backend, which is a *different estimator* rather than a faster one
        // (docs/dlss-integration.md 6) -- so this setting changes what the
        // image is, not merely how long it takes.
        //
        // Accepted: off, dlaa, quality, balanced, performance, ultraperformance.
        {"Reconstruction", _tokens->reconstruction,
         VtValue(std::string(TfGetenv("HDCLAUDE_RECONSTRUCTION", "off")))},

        // Accepted: default, stable, transformer, transformer-alt. DLSS reads
        // the preset when its feature is built, so changing this rebuilds.
        {"Reconstruction preset", _tokens->reconstructionPreset,
         VtValue(std::string(TfGetenv("HDCLAUDE_RECONSTRUCTION_PRESET",
                                      "default")))},

        // Which reconstruction runs. Accepted: super-resolution,
        // ray-reconstruction. Super Resolution by default, which is what this
        // setting's absence always meant; Ray Reconstruction denoises as it
        // upscales and is the model built for a path-traced frame, and reads
        // the reconstruction guides to do it. Changing it rebuilds.
        {"Reconstruction model", _tokens->reconstructionModel,
         VtValue(std::string(TfGetenv("HDCLAUDE_RECONSTRUCTION_MODEL",
                                      "super-resolution")))},

        // Let DLSS estimate the frame's exposure rather than being told it.
        // Off, because being told is what its own guide asks for, and because
        // on a path-traced frame the estimate costs a measurable fraction of
        // the image's light; on, to reproduce that comparison.
        {"Reconstruction auto-exposure", _tokens->reconstructionAutoExposure,
         VtValue(TfGetenvBool("HDCLAUDE_DLSS_AUTO_EXPOSURE", false))},
    };
}

std::uint64_t HdClaudeRenderDelegate::DeviceBytesAvailable() const
{
    return _allocator ? _allocator->DeviceLocalBytesAvailable() : 0;
}

std::uint64_t HdClaudeRenderDelegate::DeviceBytesSpilled() const
{
    return _allocator ? _allocator->DeviceLocalBytesSpilled() : 0;
}

void HdClaudeRenderDelegate::RecordFrameTiming(double milliseconds,
                                               std::uint32_t samples)
{
    _lastFrameMilliseconds.store(milliseconds, std::memory_order_relaxed);
    _lastFrameSamples.store(samples, std::memory_order_relaxed);

    // What the frame cost in device memory, kept as a high-water mark.
    //
    // The figure is the *heap* usage the driver reports for this process, not
    // the sum of what this allocator asked for: a caller sizing a machine cares
    // what the card is holding, which includes the driver's own overhead and
    // any padding an allocation was rounded up to.
    if (_allocator) {
        const std::uint64_t used = _allocator->DeviceLocalBytesUsed();
        std::uint64_t peak = _peakDeviceBytes.load(std::memory_order_relaxed);
        while (used > peak &&
               !_peakDeviceBytes.compare_exchange_weak(
                   peak, used, std::memory_order_relaxed)) {
        }
    }
}

VtDictionary HdClaudeRenderDelegate::GetRenderStats() const
{
    VtDictionary stats;
    stats["rendererName"] = VtValue(std::string("hdClaude"));
    stats["msPerFrame"] =
        VtValue(_lastFrameMilliseconds.load(std::memory_order_relaxed));
    stats["samplesCompleted"] =
        VtValue(int(_lastFrameSamples.load(std::memory_order_relaxed)));

    if (_context) {
        stats["device"] = VtValue(_context->Capabilities().deviceName);
        stats["deviceLost"] = VtValue(_context->IsDeviceLost());
    }
    stats["deviceBytesPeak"] =
        VtValue(double(_peakDeviceBytes.load(std::memory_order_relaxed)));

    // The stages, so a slow scene says which part of it is slow. Doubles
    // throughout: VtValue carries them to any host, and a byte count large
    // enough to lose precision in one is larger than a card holds.
    stats["ingestMs"] =
        VtValue(_stageStats.ingestMilliseconds.load(std::memory_order_relaxed));
    stats["publishMs"] =
        VtValue(_stageStats.publishMilliseconds.load(std::memory_order_relaxed));
    stats["instances"] =
        VtValue(double(_stageStats.instances.load(std::memory_order_relaxed)));
    stats["triangles"] =
        VtValue(double(_stageStats.triangles.load(std::memory_order_relaxed)));
    stats["blasBuilt"] =
        VtValue(double(_stageStats.blasBuilt.load(std::memory_order_relaxed)));
    stats["blasReused"] =
        VtValue(double(_stageStats.blasReused.load(std::memory_order_relaxed)));
    stats["blasRefit"] =
        VtValue(double(_stageStats.blasRefit.load(std::memory_order_relaxed)));
    stats["cameraRays"] =
        VtValue(double(_stageStats.cameraRays.load(std::memory_order_relaxed)));
    stats["tracedRays"] =
        VtValue(double(_stageStats.tracedRays.load(std::memory_order_relaxed)));
    stats["shadowRays"] =
        VtValue(double(_stageStats.shadowRays.load(std::memory_order_relaxed)));
    // As text, because a 64-bit hash does not survive a double: everything
    // below the low eleven bits would be rounded away, and two runs that
    // disagreed could report the same figure. The other entries here are
    // counts, which a double carries exactly at these magnitudes; this one is
    // an identity, and the only useful thing to do with it is compare it.
    {
        std::ostringstream hash;
        hash << _stageStats.hitHash.load(std::memory_order_relaxed);
        stats["hitHash"] = VtValue(hash.str());
    }
    {
        std::ostringstream hash;
        hash << _stageStats.rayHash.load(std::memory_order_relaxed);
        stats["rayHash"] = VtValue(hash.str());
    }
    stats["traceMs"] =
        VtValue(_stageStats.traceMilliseconds.load(std::memory_order_relaxed));
    stats["subdivideMs"] =
        VtValue(_stageStats.subdivideMilliseconds.load(std::memory_order_relaxed));
    stats["meshesRefined"] =
        VtValue(double(_stageStats.meshesRefined.load(std::memory_order_relaxed)));
    stats["subdivideInputPoints"] = VtValue(
        double(_stageStats.subdivideInputPoints.load(std::memory_order_relaxed)));
    stats["subdivideOutputPoints"] = VtValue(
        double(_stageStats.subdivideOutputPoints.load(std::memory_order_relaxed)));
    stats["materialMs"] =
        VtValue(_stageStats.materialMilliseconds.load(std::memory_order_relaxed));
    stats["materialsCompiled"] = VtValue(
        double(_stageStats.materialsCompiled.load(std::memory_order_relaxed)));
    stats["textureMs"] =
        VtValue(_stageStats.textureMilliseconds.load(std::memory_order_relaxed));
    stats["texturesLoaded"] =
        VtValue(double(_stageStats.texturesLoaded.load(std::memory_order_relaxed)));
    stats["textureBytes"] =
        VtValue(double(_stageStats.textureBytes.load(std::memory_order_relaxed)));
    if (_allocator) {
        stats["deviceBytesUsed"] =
            VtValue(double(_allocator->DeviceLocalBytesUsed()));
        stats["deviceBytesAvailable"] =
            VtValue(double(_allocator->DeviceLocalBytesAvailable()));
    }
    if (!_initializationError.empty()) {
        stats["error"] = VtValue(_initializationError);
    }

    // Materials that are not shaded as authored are named here, not only in
    // the console. A renderer that quietly substitutes a material and mentions
    // it once at startup is a renderer whose output cannot be trusted later.
    if (_store) {
        std::vector<std::string> reports = _store->FallbackReports();
        if (_texturePool) {
            const std::vector<std::string>& failures = _texturePool->Failures();
            reports.insert(reports.end(), failures.begin(), failures.end());
        }
        if (!reports.empty()) {
            VtStringArray asArray(reports.begin(), reports.end());
            stats["materialFallbacks"] = VtValue(asArray);
        }
    }
    return stats;
}

PXR_NAMESPACE_CLOSE_SCOPE
