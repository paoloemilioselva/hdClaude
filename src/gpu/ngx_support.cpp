// The only translation unit that includes an NVIDIA header.
//
// Everything above this file sees `ReconstructionSupport` and a
// `VulkanRequirementProvider`, and compiles identically whether or not the SDK
// was found (docs/dlss-integration.md 2). The whole of the SDK-dependent code
// is inside one `#if HDCLAUDE_HAS_DLSS`, and the stubs below it are what the
// default build gets.

#include "hdclaude/gpu/reconstruction.h"

#include <cstdio>

#if defined(HDCLAUDE_HAS_DLSS)
#include <nvsdk_ngx_vk.h>
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
#if defined(HDCLAUDE_DLSS_RUNTIME_DIR)
const wchar_t* RuntimeSearchPath()
{
    static const std::wstring path = [] {
        const std::string narrow = HDCLAUDE_DLSS_RUNTIME_DIR;
        return std::wstring(narrow.begin(), narrow.end());
    }();
    return path.c_str();
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
