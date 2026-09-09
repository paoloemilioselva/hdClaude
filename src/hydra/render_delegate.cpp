#include "render_delegate.h"

#include "instancer.h"
#include "trace.h"

#include "camera.h"
#include "light.h"
#include "material.h"
#include "basis_curves.h"
#include "mesh.h"
#include "render_buffer.h"
#include "render_pass.h"

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
                         (exposure));

namespace {

const TfTokenVector kSupportedRprimTypes = {
    HdPrimTypeTokens->mesh,
    // NurbsCurves arrive here too: UsdImaging's adapter reports them as
    // basisCurves with a linear basis, drawing the control cage rather than the
    // evaluated NURBS, so a stage's curves reach every Hydra renderer as
    // polylines.
    HdPrimTypeTokens->basisCurves,
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
    const std::string overridePath = TfGetenv("HDCLAUDE_SHADER_DIR");
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
    if (const std::string path = TfGetenv("HDCLAUDE_STATS_REPORT");
        !path.empty()) {
        const std::uint64_t peak =
            _peakDeviceBytes.load(std::memory_order_relaxed);
        const std::uint64_t available =
            _allocator ? _allocator->DeviceLocalBytesAvailable() : 0;
        if (std::ofstream out{path}; out) {
            const auto& stages = _stageStats;
            out << "deviceBytesPeak " << peak << '\n'
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
                << stages.textureBytes.load(std::memory_order_relaxed) << '\n';
        } else {
            TF_WARN("hdClaude: could not write the stats report to '%s'",
                    path.c_str());
        }
    }

    // Reverse of construction. The path tracer holds buffers owned by the
    // allocator, which belongs to the device; releasing them out of order
    // leaks device memory that only surfaces as a validation message at
    // vkDestroyDevice (docs/implementation-notes.md).
    _pathTracer.reset();
    _allocator.reset();
    _context.reset();
}

void HdClaudeRenderDelegate::Initialize(const HdRenderSettingsMap& settingsMap)
{
    _store = std::make_unique<HdClaudeSceneStore>();
    _texturePool = std::make_unique<HdClaudeTexturePool>();
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

    try {
        hdclaude::VulkanContextOptions options;
        options.enableValidation =
            TfGetenvBool("HDCLAUDE_ENABLE_VULKAN_VALIDATION", false);
        options.preferredDeviceName = TfGetenv("HDCLAUDE_DEVICE");

        _context = std::make_unique<hdclaude::VulkanContext>(options);
        _allocator = std::make_unique<hdclaude::VulkanAllocator>(*_context);
        _pathTracer = std::make_unique<hdclaude::PathTracer>(
            *_context, *_allocator, ResolveShaderDirectory());
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
        _materialCompiler = std::make_unique<HdClaudeMaterialCompiler>(
            _pathTracer->ShadeKernelSource());

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
    }

    // Read once, at construction: a change of refinement level changes the
    // geometry every mesh publishes, so it is a delegate-wide decision rather
    // than something the render pass can vary per frame. Changing it means a
    // new delegate, which is what a host does when it applies a render setting
    // that alters the scene.
    const int subdivisionLevel =
        std::clamp(TfGetenvInt("HDCLAUDE_SUBDIVISION_LEVEL", 2), 0, 6);
    // Six faces round a tube. Enough that a whisker reads as round at the size
    // curves are usually authored, cheap enough that a head of hair does not
    // pay for a smoothness nothing can see, and a setting because the right
    // answer depends on how close the camera gets.
    const int curveSides =
        std::clamp(TfGetenvInt("HDCLAUDE_CURVE_SIDES", 6), 3, 64);

    _renderParam = std::make_unique<HdClaudeRenderParam>(
        _store.get(), _materialCompiler.get(), _texturePool.get(),
        subdivisionLevel, curveSides, &_stageStats);
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
    };
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
