// The only translation unit that includes an NVIDIA header.
//
// Everything above this file sees `ReconstructionSupport` and a
// `VulkanRequirementProvider`, and compiles identically whether or not the SDK
// was found (docs/dlss-integration.md 2). The whole of the SDK-dependent code
// is inside one `#if HDCLAUDE_HAS_DLSS`, and the stubs below it are what the
// default build gets.

#include "hdclaude/gpu/reconstruction.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
// Explicitly, because WIN32_LEAN_AND_MEAN is defined for this build and the
// version API is one of the things that leaves out.
#include <winver.h>
#endif

#if defined(HDCLAUDE_HAS_DLSS)
#include <nvsdk_ngx_vk.h>
#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_helpers_vk.h>
#include <nvsdk_ngx_defs_dlssd.h>

#include <string>
#endif

namespace hdclaude {

namespace {

/// NVIDIA's PCI vendor ID. NGX runs on nothing else.
constexpr std::uint32_t kNvidiaVendorId = 0x10DE;

bool IsNvidia(VkPhysicalDevice device)
{
    if (device == VK_NULL_HANDLE) {
        return false;
    }
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(device, &properties);
    return properties.vendorID == kNvidiaVendorId;
}

}   // namespace

#if defined(HDCLAUDE_HAS_DLSS)

namespace {

/// This application, as NGX identifies it.
///
/// A project ID rather than the numeric application ID: the numeric form is
/// allocated by NVIDIA per title, and the ID-with-project form is what an
/// engine that has not been through that process is meant to use.
///
/// It has to be a UUID and not a name. NGX validates the string and answers
/// FAIL_InvalidParameter for anything else, which is indistinguishable from
/// every other way that call can be wrong -- so this is a fixed UUID for
/// hdClaude rather than the readable identifier it looks like it wants.
constexpr const char* kProjectId = "6f1cd0a3-9a1f-4c8b-91d5-2b7a0c4e5d38";
constexpr const char* kEngineVersion = "0.1";

/// Where the feature DLLs are.
///
/// NGX looks in the application folder first, and `nvngx_dlss.dll` is not
/// there: hdClaude ships none of NVIDIA's runtime binaries, so the only copy on
/// this machine is the one the CMake fetch put in the dependency tree. That
/// path is compiled in by cmake/NvidiaDLSS.cmake.
///
/// `HDCLAUDE_DLSS_RUNTIME_DIR` in the environment overrides it, which is how a
/// different DLSS runtime is put in front of the renderer: the model that runs
/// is the one in that DLL, and comparing two of them means pointing this at two
/// directories. Nothing else in hdClaude selects a version.
#if defined(HDCLAUDE_DLSS_RUNTIME_DIR)
const std::string& RuntimeDirectory()
{
    static const std::string path = [] {
        if (const char* override_ = std::getenv("HDCLAUDE_DLSS_RUNTIME_DIR")) {
            if (*override_ != 0) {
                return std::string(override_);
            }
        }
        return std::string(HDCLAUDE_DLSS_RUNTIME_DIR);
    }();
    return path;
}

const wchar_t* RuntimeSearchPath()
{
    static const std::wstring path = [] {
        const std::string& narrow = RuntimeDirectory();
        return std::wstring(narrow.begin(), narrow.end());
    }();
    return path.c_str();
}

/// The version of the DLSS runtime that will actually be loaded.
///
/// Read off the file rather than asked of NGX, because NGX has no call that
/// reports it -- there is no snippet-version parameter in the SDK headers. It
/// matters because the model is the DLL: "which DLSS" is a question about this
/// number, and a comparison that cannot name the two things being compared is
/// not a comparison.
std::string RuntimeVersion()
{
    static const std::string version = [] {
        const std::string file = RuntimeDirectory() + "/nvngx_dlss.dll";
#if defined(_WIN32)
        DWORD ignored = 0;
        const DWORD size = GetFileVersionInfoSizeA(file.c_str(), &ignored);
        if (size == 0) {
            return std::string("unknown");
        }
        std::vector<unsigned char> block(size);
        if (!GetFileVersionInfoA(file.c_str(), 0, size, block.data())) {
            return std::string("unknown");
        }
        VS_FIXEDFILEINFO* info = nullptr;
        UINT length = 0;
        if (!VerQueryValueA(block.data(), "\\",
                            reinterpret_cast<LPVOID*>(&info), &length) ||
            info == nullptr) {
            return std::string("unknown");
        }
        char text[64];
        std::snprintf(text, sizeof(text), "%u.%u.%u.%u",
                      unsigned(HIWORD(info->dwFileVersionMS)),
                      unsigned(LOWORD(info->dwFileVersionMS)),
                      unsigned(HIWORD(info->dwFileVersionLS)),
                      unsigned(LOWORD(info->dwFileVersionLS)));
        return std::string(text);
#else
        (void)file;
        return std::string("unknown");
#endif
    }();
    return version;
}
#endif

/// NGX result names, so a failure says which failure.
const char* ToString(NVSDK_NGX_Result result)
{
    switch (result) {
        case NVSDK_NGX_Result_Success: return "success";
        case NVSDK_NGX_Result_FAIL_FeatureNotSupported:
            return "the feature is not supported by this system, hardware or API";
        case NVSDK_NGX_Result_FAIL_PlatformError: return "a platform error";
        case NVSDK_NGX_Result_FAIL_FeatureAlreadyExists: return "already exists";
        case NVSDK_NGX_Result_FAIL_FeatureNotFound: return "the feature was not found";
        case NVSDK_NGX_Result_FAIL_InvalidParameter: return "an invalid parameter";
        case NVSDK_NGX_Result_FAIL_ScratchBufferTooSmall: return "the scratch buffer is too small";
        case NVSDK_NGX_Result_FAIL_NotInitialized: return "NGX is not initialised";
        case NVSDK_NGX_Result_FAIL_UnsupportedInputFormat: return "an unsupported input format";
        case NVSDK_NGX_Result_FAIL_RWFlagMissing: return "a read/write flag is missing";
        case NVSDK_NGX_Result_FAIL_MissingInput: return "a required input is missing";
        case NVSDK_NGX_Result_FAIL_UnableToInitializeFeature:
            return "the feature could not be initialised";
        case NVSDK_NGX_Result_FAIL_OutOfDate: return "the NGX runtime is out of date";
        case NVSDK_NGX_Result_FAIL_OutOfGPUMemory: return "out of GPU memory";
        case NVSDK_NGX_Result_FAIL_UnsupportedFormat: return "an unsupported format";
        case NVSDK_NGX_Result_FAIL_UnableToWriteToAppDataPath:
            return "the application data path is not writable";
        case NVSDK_NGX_Result_FAIL_UnsupportedParameter: return "an unsupported parameter";
        case NVSDK_NGX_Result_FAIL_Denied: return "denied";
        case NVSDK_NGX_Result_FAIL_NotImplemented: return "not implemented";
        default: break;
    }
    return "an unrecognised NGX result";
}

}   // namespace

bool NgxCompiledIn() { return true; }

NgxRequirementProvider::NgxRequirementProvider()
{
    unsigned int instanceCount = 0;
    const char** instanceExtensions = nullptr;
    unsigned int deviceCount = 0;
    const char** deviceExtensions = nullptr;

    const NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_RequiredExtensions(
        &instanceCount, &instanceExtensions, &deviceCount, &deviceExtensions);
    if (result != NVSDK_NGX_Result_Success) {
        _unavailable =
            std::string("NGX would not name its Vulkan extensions: ") +
            ToString(result);
        return;
    }

    // Copied rather than referenced. NGX returns pointers into its own static
    // storage and says nothing about how long they live; this provider outlives
    // the call and is read during device selection.
    for (unsigned int i = 0; i < instanceCount; ++i) {
        if (instanceExtensions && instanceExtensions[i]) {
            _instanceExtensions.emplace_back(instanceExtensions[i]);
        }
    }
    for (unsigned int i = 0; i < deviceCount; ++i) {
        if (deviceExtensions && deviceExtensions[i]) {
            _deviceExtensions.emplace_back(deviceExtensions[i]);
        }
    }
}

ReconstructionSupport QueryNgxSupport(const VulkanContext& context)
{
    ReconstructionSupport support;

    if (!IsNvidia(context.PhysicalDevice())) {
        support.reason = "the device is not an NVIDIA one";
        return support;
    }

    NVSDK_NGX_FeatureCommonInfo featureInfo{};
#if defined(HDCLAUDE_DLSS_RUNTIME_DIR)
    const wchar_t* searchPaths[] = {RuntimeSearchPath()};
    featureInfo.PathListInfo.Path = searchPaths;
    featureInfo.PathListInfo.Length = 1;
#endif

    // The proc addresses are handed over explicitly because hdClaude loads
    // Vulkan through volk: there is no system-wide `vkGetInstanceProcAddr` for
    // NGX to find on its own, and without these it initialises against nothing.
    NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_Init_with_ProjectID(
        kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, kEngineVersion, L".",
        context.Instance(), context.PhysicalDevice(), context.Device(),
        vkGetInstanceProcAddr, vkGetDeviceProcAddr, &featureInfo);
    if (result != NVSDK_NGX_Result_Success) {
        support.reason =
            std::string("NGX would not initialise: ") + ToString(result);
        return support;
    }

    NVSDK_NGX_Parameter* parameters = nullptr;
    result = NVSDK_NGX_VULKAN_GetCapabilityParameters(&parameters);
    if (result != NVSDK_NGX_Result_Success || parameters == nullptr) {
        support.reason =
            std::string("NGX would not hand over its capabilities: ") +
            ToString(result);
        NVSDK_NGX_VULKAN_Shutdown1(context.Device());
        return support;
    }

    support.available = true;

    int value = 0;
    if (parameters->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &value) ==
        NVSDK_NGX_Result_Success) {
        support.superResolution = value != 0;
    }
    if (parameters->Get(NVSDK_NGX_Parameter_SuperSamplingDenoising_Available,
                        &value) == NVSDK_NGX_Result_Success) {
        support.rayReconstruction = value != 0;
    }
    if (parameters->Get(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver,
                        &value) == NVSDK_NGX_Result_Success) {
        support.needsNewerDriver = value != 0;
    }
    unsigned int version = 0;
    if (parameters->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor,
                        &version) == NVSDK_NGX_Result_Success) {
        support.minDriverMajor = version;
    }
    if (parameters->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor,
                        &version) == NVSDK_NGX_Result_Success) {
        support.minDriverMinor = version;
    }

    NVSDK_NGX_VULKAN_DestroyParameters(parameters);
    NVSDK_NGX_VULKAN_Shutdown1(context.Device());
    return support;
}


// --- The DLSS backend ------------------------------------------------------

namespace {

/// hdClaude's quality values in NGX's terms.
NVSDK_NGX_PerfQuality_Value ToNgx(ReconstructionQuality quality)
{
    switch (quality) {
        case ReconstructionQuality::NativeResolution:
            return NVSDK_NGX_PerfQuality_Value_DLAA;
        case ReconstructionQuality::Quality:
            return NVSDK_NGX_PerfQuality_Value_MaxQuality;
        case ReconstructionQuality::Balanced:
            return NVSDK_NGX_PerfQuality_Value_Balanced;
        case ReconstructionQuality::Performance:
            return NVSDK_NGX_PerfQuality_Value_MaxPerf;
        case ReconstructionQuality::UltraPerformance:
            return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
    }
    return NVSDK_NGX_PerfQuality_Value_DLAA;
}

/// A preset as NGX's letter, and the parameter that carries it.
///
/// The hint is per quality mode -- there is a separate parameter for DLAA, for
/// Quality, and so on -- so the one that matches the mode being created is the
/// one set. Setting all of them would work and would also say that hdClaude did
/// not know which mode it was asking for.
NVSDK_NGX_DLSS_Hint_Render_Preset ToNgx(ReconstructionPreset preset)
{
    switch (preset) {
        case ReconstructionPreset::Default:
            return NVSDK_NGX_DLSS_Hint_Render_Preset_Default;
        case ReconstructionPreset::Stable:
            return NVSDK_NGX_DLSS_Hint_Render_Preset_F;
        case ReconstructionPreset::Transformer:
            return NVSDK_NGX_DLSS_Hint_Render_Preset_K;
        case ReconstructionPreset::TransformerAlternate:
            return NVSDK_NGX_DLSS_Hint_Render_Preset_J;
    }
    return NVSDK_NGX_DLSS_Hint_Render_Preset_Default;
}

const char* PresetParameter(ReconstructionQuality quality)
{
    switch (quality) {
        case ReconstructionQuality::NativeResolution:
            return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA;
        case ReconstructionQuality::Quality:
            return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality;
        case ReconstructionQuality::Balanced:
            return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced;
        case ReconstructionQuality::Performance:
            return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance;
        case ReconstructionQuality::UltraPerformance:
            return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance;
    }
    return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA;
}

/// An hdClaude texture as an NGX resource.
///
/// The subresource range is the whole of the image, because every image
/// hdClaude hands a backend is a single-level, single-layer colour image; a
/// backend that received a view of something else would be reading a resource
/// this renderer does not produce.
NVSDK_NGX_Resource_VK ToNgx(const ReconstructionTexture& texture, bool readWrite)
{
    VkImageSubresourceRange range{};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.baseMipLevel = 0;
    range.levelCount = 1;
    range.baseArrayLayer = 0;
    range.layerCount = 1;
    return NVSDK_NGX_Create_ImageView_Resource_VK(
        texture.view, texture.image, range, texture.format, texture.width,
        texture.height, readWrite);
}

/// The NGX backend.
///
/// Holds NGX initialised for its own lifetime and owns one DLSS feature. Not
/// copyable and not movable: the feature and the parameter block are device
/// state with an explicit destroy, and there is exactly one owner.
class NgxBackend final : public ReconstructionBackend {
  public:
    NgxBackend(const VulkanContext& context, NVSDK_NGX_Parameter* parameters)
        : _context(context), _parameters(parameters)
    {
    }

    ~NgxBackend() override
    {
        ReleaseFeature();
        if (_parameters != nullptr) {
            NVSDK_NGX_VULKAN_DestroyParameters(_parameters);
        }
        NVSDK_NGX_VULKAN_Shutdown1(_context.Device());
    }

    NgxBackend(const NgxBackend&) = delete;
    NgxBackend& operator=(const NgxBackend&) = delete;

    /// The name carries the runtime's version, because the version *is* the
    /// model: which DLSS is running is a question about which `nvngx_dlss.dll`
    /// was loaded, and a comparison between two of them has to be able to say
    /// which one produced a picture. Built once and held, since `Name` returns
    /// a pointer and callers keep it.
    const char* Name() const override
    {
        static const std::string name = "NVIDIA DLSS " + RuntimeVersion();
        return name.c_str();
    }

    ReconstructionSizing QuerySizing(std::uint32_t outputWidth,
                                     std::uint32_t outputHeight,
                                     ReconstructionQuality quality) const override
    {
        ReconstructionSizing sizing;
        if (outputWidth == 0 || outputHeight == 0) {
            sizing.reason = "an output extent of zero";
            return sizing;
        }
        float sharpness = 0.0f;
        const NVSDK_NGX_Result result = NGX_DLSS_GET_OPTIMAL_SETTINGS(
            _parameters, outputWidth, outputHeight, ToNgx(quality),
            &sizing.renderWidth, &sizing.renderHeight, &sizing.maxWidth,
            &sizing.maxHeight, &sizing.minWidth, &sizing.minHeight, &sharpness);
        if (result != NVSDK_NGX_Result_Success) {
            sizing.reason =
                std::string("DLSS would not name its render extents: ") +
                ToString(result);
            return sizing;
        }
        // Zero is how DLSS says "not at this output size", which is a real
        // answer -- the model has a smallest input it will work from -- and is
        // reported rather than passed on as extents nothing can render at.
        if (sizing.renderWidth == 0 || sizing.renderHeight == 0) {
            sizing.reason = "DLSS does not support this output extent";
            return sizing;
        }
        sizing.valid = true;
        return sizing;
    }

    bool Resize(VkCommandBuffer command, const ReconstructionResolution& resolution,
                std::string* reason) override
    {
        // Destroyed first, and unconditionally. A failed rebuild must leave
        // nothing to evaluate rather than the previous feature at the previous
        // size, which would reconstruct one extent's frames into another's.
        ReleaseFeature();

        if (command == VK_NULL_HANDLE || resolution.renderWidth == 0 ||
            resolution.renderHeight == 0 || resolution.outputWidth == 0 ||
            resolution.outputHeight == 0) {
            if (reason != nullptr) {
                *reason = "a zero extent, or no command buffer to record into";
            }
            return false;
        }

        // The preset, set before the feature is created because that is when
        // DLSS reads it. `Default` is passed through rather than skipped: the
        // parameters outlive one build, so leaving a previous preset in place
        // would make "default" mean "whatever was asked for last time".
        _parameters->Set(PresetParameter(resolution.quality),
                         static_cast<unsigned int>(ToNgx(resolution.preset)));

        NVSDK_NGX_DLSS_Create_Params create{};
        create.Feature.InWidth = resolution.renderWidth;
        create.Feature.InHeight = resolution.renderHeight;
        create.Feature.InTargetWidth = resolution.outputWidth;
        create.Feature.InTargetHeight = resolution.outputHeight;
        create.Feature.InPerfQualityValue = ToNgx(resolution.quality);
        // Four flags, and each is a statement about hdClaude rather than a
        // preference:
        //
        //   IsHDR         the film hands over linear radiance with no display
        //                 transform, which is what this flag means.
        //   MVLowRes      motion is written by the guide kernel at the render
        //                 extent, not the output one.
        //   AutoExposure  there is no exposure texture to give it. DLSS
        //                 estimates its own rather than assuming one, which is
        //                 the documented answer for a renderer that does not
        //                 have one to hand.
        //
        // DepthInverted is *not* set: the guide kernel writes near at zero.
        // Sharpening is not set because the SDK has deprecated it.
        create.InFeatureCreateFlags =
            NVSDK_NGX_DLSS_Feature_Flags_IsHDR |
            NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
        // AutoExposure only when the caller has no exposure to give. The guide
        // calls the exposure parameter "the preferred method" and auto-exposure
        // a thing for "some situations" (3.9, 3.10), and on a path-traced frame
        // -- a dark scene with a handful of very bright pixels in it -- the
        // estimate DLSS makes for itself is far enough out to cost 42% of the
        // image's light.
        if (resolution.exposure == ReconstructionExposure::Automatic) {
            create.InFeatureCreateFlags |=
                NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
        }

        const NVSDK_NGX_Result result = NGX_VULKAN_CREATE_DLSS_EXT1(
            _context.Device(), command, 1, 1, &_feature, _parameters, &create);
        if (result != NVSDK_NGX_Result_Success || _feature == nullptr) {
            _feature = nullptr;
            if (reason != nullptr) {
                *reason = std::string("DLSS would not build: ") + ToString(result);
            }
            return false;
        }
        _resolution = resolution;
        // A feature that has just been built has no history, so the first
        // frame through it is a reset whether or not the caller says so.
        _reset = true;
        return true;
    }

    void Evaluate(VkCommandBuffer command, const ReconstructionFrame& frame) override
    {
        if (_feature == nullptr || command == VK_NULL_HANDLE) {
            return;
        }
        if (!frame.color.Valid() || !frame.depth.Valid() ||
            !frame.motion.Valid() || !frame.output.Valid()) {
            std::fprintf(stderr,
                         "hdClaude: DLSS was handed an incomplete frame and "
                         "reconstructed nothing\n");
            return;
        }

        NVSDK_NGX_Resource_VK color = ToNgx(frame.color, false);
        NVSDK_NGX_Resource_VK depth = ToNgx(frame.depth, false);
        NVSDK_NGX_Resource_VK motion = ToNgx(frame.motion, false);
        NVSDK_NGX_Resource_VK output = ToNgx(frame.output, true);

        NVSDK_NGX_VK_DLSS_Eval_Params eval{};
        eval.Feature.pInColor = &color;
        eval.Feature.pInOutput = &output;
        eval.pInDepth = &depth;
        eval.pInMotionVectors = &motion;
        // Negated, and this is the one translation on this boundary that is not
        // a unit conversion.
        //
        // hdClaude's jitter is where the sample *landed*, measured from the
        // pixel centre (the contract on ReconstructionFrame). DLSS asks for
        // something else that is also called a jitter: "the jitter applied to
        // the projection matrix", by the recipe
        // `ProjectionMatrix.M[2][0] += ProjectionJitter.X` (DLSS Programming
        // Guide 3.7.2, 3.7.3). Offsetting a projection by +d moves the rendered
        // content +d across the screen, so the sample a pixel takes moves -d.
        // The two numbers are the same displacement seen from opposite ends,
        // and they differ in sign.
        //
        // The axes need no flip on top of that. The guide asks for the
        // co-ordinate system the motion vectors are in, and both hdClaude's
        // motion vectors and the images it hands over are in the row order of
        // those images (reconstruct_inputs.comp.glsl) -- DLSS never learns
        // which way is up, only that everything it is given agrees.
        eval.InJitterOffsetX = -frame.jitterX;
        eval.InJitterOffsetY = -frame.jitterY;
        // The frame's exposure, when the caller measured one. DLSS samples only
        // the first channel of this image, and the feature was built without
        // the auto-exposure flag precisely so that it reads this instead.
        NVSDK_NGX_Resource_VK exposure{};
        if (frame.exposure.Valid()) {
            exposure = ToNgx(frame.exposure, false);
            eval.pInExposureTexture = &exposure;
        }
        eval.InRenderSubrectDimensions.Width = frame.color.width;
        eval.InRenderSubrectDimensions.Height = frame.color.height;
        eval.InReset = (frame.reset || _reset) ? 1 : 0;
        // Motion is already in render pixels, which is the space DLSS measures
        // in, so it is handed over unscaled. The scale exists for renderers
        // that write normalised motion and would otherwise have to rewrite the
        // buffer.
        eval.InMVScaleX = 1.0f;
        eval.InMVScaleY = 1.0f;
        eval.InPreExposure = frame.preExposure;

        const NVSDK_NGX_Result result =
            NGX_VULKAN_EVALUATE_DLSS_EXT(command, _feature, _parameters, &eval);
        if (result != NVSDK_NGX_Result_Success) {
            std::fprintf(stderr, "hdClaude: DLSS would not evaluate: %s\n",
                         ToString(result));
            return;
        }
        _reset = false;
    }

    void ResetHistory() override { _reset = true; }

  private:
    void ReleaseFeature()
    {
        if (_feature != nullptr) {
            NVSDK_NGX_VULKAN_ReleaseFeature(_feature);
            _feature = nullptr;
        }
    }

    const VulkanContext& _context;
    NVSDK_NGX_Parameter* _parameters = nullptr;
    NVSDK_NGX_Handle* _feature = nullptr;
    ReconstructionResolution _resolution;
    bool _reset = true;
};

}   // namespace

std::unique_ptr<ReconstructionBackend> CreateNgxBackend(const VulkanContext& context,
                                                        std::string* reason)
{
    const auto decline = [reason](std::string why) {
        if (reason != nullptr) {
            *reason = std::move(why);
        }
        return std::unique_ptr<ReconstructionBackend>();
    };

    if (!IsNvidia(context.PhysicalDevice())) {
        return decline("the device is not an NVIDIA one");
    }

    NVSDK_NGX_FeatureCommonInfo featureInfo{};
#if defined(HDCLAUDE_DLSS_RUNTIME_DIR)
    const wchar_t* searchPaths[] = {RuntimeSearchPath()};
    featureInfo.PathListInfo.Path = searchPaths;
    featureInfo.PathListInfo.Length = 1;
#endif

    NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_Init_with_ProjectID(
        kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, kEngineVersion, L".",
        context.Instance(), context.PhysicalDevice(), context.Device(),
        vkGetInstanceProcAddr, vkGetDeviceProcAddr, &featureInfo);
    if (result != NVSDK_NGX_Result_Success) {
        return decline(std::string("NGX would not initialise: ") + ToString(result));
    }

    // The *capability* parameters, not freshly allocated ones. The optimal
    // settings call reads a callback that only this block carries, and asking
    // for it on an allocated block is a documented way to get FAIL_OutOfDate
    // from a driver that is perfectly current.
    NVSDK_NGX_Parameter* parameters = nullptr;
    result = NVSDK_NGX_VULKAN_GetCapabilityParameters(&parameters);
    if (result != NVSDK_NGX_Result_Success || parameters == nullptr) {
        NVSDK_NGX_VULKAN_Shutdown1(context.Device());
        return decline(std::string("NGX would not hand over its capabilities: ") +
                       ToString(result));
    }

    int available = 0;
    if (parameters->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &available) !=
            NVSDK_NGX_Result_Success ||
        available == 0) {
        NVSDK_NGX_VULKAN_DestroyParameters(parameters);
        NVSDK_NGX_VULKAN_Shutdown1(context.Device());
        return decline("DLSS Super Resolution is not available on this device");
    }

    return std::make_unique<NgxBackend>(context, parameters);
}

#else   // HDCLAUDE_HAS_DLSS

bool NgxCompiledIn() { return false; }

NgxRequirementProvider::NgxRequirementProvider()
    : _unavailable("this build has no NVIDIA DLSS SDK")
{
}

ReconstructionSupport QueryNgxSupport(const VulkanContext&)
{
    ReconstructionSupport support;
    support.reason = "this build has no NVIDIA DLSS SDK";
    return support;
}

std::unique_ptr<ReconstructionBackend> CreateNgxBackend(const VulkanContext&,
                                                        std::string* reason)
{
    if (reason != nullptr) {
        *reason = "this build has no NVIDIA DLSS SDK";
    }
    return {};
}

#endif   // HDCLAUDE_HAS_DLSS

// --- Shared by both builds --------------------------------------------------
//
// These read only what the constructor stored, so they behave the same whether
// the lists were filled by NGX or left empty because there was no SDK. That is
// the whole reason the provider is safe to install unconditionally.

std::vector<const char*> NgxRequirementProvider::InstanceExtensions() const
{
    _instanceView.clear();
    _instanceView.reserve(_instanceExtensions.size());
    for (const std::string& name : _instanceExtensions) {
        _instanceView.push_back(name.c_str());
    }
    return _instanceView;
}

std::vector<const char*> NgxRequirementProvider::DeviceExtensions(
    VkPhysicalDevice device) const
{
    // Asked per candidate device, and answered for NVIDIA ones only. A device
    // from another vendor is not rejected; the backend is simply inactive on
    // it, and asking it for NGX's extensions would fail selection for a feature
    // it was never going to run.
    if (!IsNvidia(device)) {
        return {};
    }
    _deviceView.clear();
    _deviceView.reserve(_deviceExtensions.size());
    for (const std::string& name : _deviceExtensions) {
        // Not the extension a core feature has replaced.
        //
        // NGX asks for VK_EXT_buffer_device_address, which Vulkan 1.2 promoted
        // to `VkPhysicalDeviceVulkan12Features::bufferDeviceAddress`; hdClaude
        // enables the core feature because ray query and the acceleration
        // structures need it, and the specification forbids enabling both. The
        // validation layer says so outright -- "pNext chain includes
        // VkPhysicalDeviceVulkan12Features with bufferDeviceAddress set to
        // VK_TRUE and ppEnabledExtensionNames contains
        // VK_EXT_buffer_device_address" -- and refusing here is not dropping a
        // requirement, because the capability NGX wants is present either way.
        //
        // NGX names it because its list was written against a Vulkan that had
        // no core form. Anything else it asks for is passed through untouched,
        // and the context enables only what the device actually has.
        if (name == "VK_EXT_buffer_device_address") {
            continue;
        }
        _deviceView.push_back(name.c_str());
    }
    return _deviceView;
}

bool NgxRequirementProvider::SupportsDevice(VkPhysicalDevice device) const
{
    return _unavailable.empty() && IsNvidia(device);
}

}   // namespace hdclaude
