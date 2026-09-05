#include "render_delegate.h"

#include "camera.h"
#include "light.h"
#include "material.h"
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

PXR_NAMESPACE_OPEN_SCOPE

TF_DEFINE_PRIVATE_TOKENS(_tokens,
                         ((mtlxRenderContext, "mtlx"))
                         (samplesPerPixel)
                         (maxBounces)
                         (samplesPerFrame)
                         (environmentIntensity)
                         (sunIntensity));

namespace {

const TfTokenVector kSupportedRprimTypes = {
    HdPrimTypeTokens->mesh,
};

/// Light types hdClaude samples. A type absent from this list is never created
/// by Hydra, so an unsupported light is simply not in the scene rather than
/// present and ignored.
const TfTokenVector kSupportedLightTypes = {
    HdPrimTypeTokens->rectLight,  HdPrimTypeTokens->diskLight,
    HdPrimTypeTokens->sphereLight, HdPrimTypeTokens->distantLight,
    HdPrimTypeTokens->domeLight,
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

    _renderParam = std::make_unique<HdClaudeRenderParam>(
        _store.get(), _materialCompiler.get());
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
    // The stock instancer computes the transforms; HdClaudeMesh reads them
    // back through HdRprim::GetInstancerTransforms, which composes the chain.
    return new HdInstancer(delegate, id);
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
        {"Max bounces", _tokens->maxBounces,
         VtValue(TfGetenvInt("HDCLAUDE_MAX_BOUNCES", 4))},
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
    };
}

void HdClaudeRenderDelegate::RecordFrameTiming(double milliseconds,
                                               std::uint32_t samples)
{
    _lastFrameMilliseconds.store(milliseconds, std::memory_order_relaxed);
    _lastFrameSamples.store(samples, std::memory_order_relaxed);
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
    if (!_initializationError.empty()) {
        stats["error"] = VtValue(_initializationError);
    }

    // Materials that are not shaded as authored are named here, not only in
    // the console. A renderer that quietly substitutes a material and mentions
    // it once at startup is a renderer whose output cannot be trusted later.
    if (_store) {
        const std::vector<std::string> reports = _store->FallbackReports();
        if (!reports.empty()) {
            VtStringArray asArray(reports.begin(), reports.end());
            stats["materialFallbacks"] = VtValue(asArray);
        }
    }
    return stats;
}

PXR_NAMESPACE_CLOSE_SCOPE
