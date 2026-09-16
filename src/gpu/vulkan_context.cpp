#include "hdclaude/gpu/vulkan_context.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>
#include <sstream>
#include <system_error>

namespace hdclaude {
namespace {

bool EnvironmentFlag(const char* name)
{
#if defined(_MSC_VER)
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) {
        return false;
    }
    const bool enabled = value[0] == '1';
    std::free(value);
    return enabled;
#else
    const char* value = std::getenv(name);
    return value != nullptr && value[0] == '1';
#endif
}

/// An environment variable's value, or empty. Mirrors EnvironmentFlag rather
/// than reaching for TfGetenv: this layer does not depend on pxr and a cache
/// path is not a reason to start.
std::string EnvironmentValue(const char* name)
{
#if defined(_MSC_VER)
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) {
        return {};
    }
    std::string text(value);
    std::free(value);
    return text;
#else
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
#endif
}

std::string ToLower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool HasExtension(const std::vector<VkExtensionProperties>& available,
                  const char* name)
{
    return std::any_of(available.begin(), available.end(),
                       [name](const VkExtensionProperties& properties) {
                           return std::strcmp(properties.extensionName, name) == 0;
                       });
}

std::vector<VkExtensionProperties> DeviceExtensionProperties(VkPhysicalDevice device)
{
    std::uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> properties(count);
    if (count > 0) {
        vkEnumerateDeviceExtensionProperties(device, nullptr, &count, properties.data());
    }
    return properties;
}

/// NVIDIA packs its driver version differently from the Vulkan convention, so
/// a generically decoded string would be wrong on exactly the vendor whose
/// driver version we most need to record in the gallery (lessons R11).
std::string FormatDriverVersion(std::uint32_t version, std::uint32_t vendorId)
{
    std::ostringstream out;
    if (vendorId == 0x10DE) {  // NVIDIA
        out << ((version >> 22) & 0x3ff) << '.' << ((version >> 14) & 0x0ff) << '.'
            << ((version >> 6) & 0x0ff) << '.' << (version & 0x003f);
    } else if (vendorId == 0x8086) {  // Intel on Windows
        out << (version >> 14) << '.' << (version & 0x3fff);
    } else {
        out << VK_API_VERSION_MAJOR(version) << '.' << VK_API_VERSION_MINOR(version)
            << '.' << VK_API_VERSION_PATCH(version);
    }
    return out.str();
}

}  // namespace

// ---------------------------------------------------------------------------

VulkanError::VulkanError(VkResult result, std::string context)
    : std::runtime_error(context + ": " + ToString(result)), _result(result)
{
}

const char* ToString(VkResult result)
{
    switch (result) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_TIMEOUT: return "VK_TIMEOUT";
        case VK_INCOMPLETE: return "VK_INCOMPLETE";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
        case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
        case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
        case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
        case VK_ERROR_UNKNOWN: return "VK_ERROR_UNKNOWN";
        case VK_ERROR_OUT_OF_POOL_MEMORY: return "VK_ERROR_OUT_OF_POOL_MEMORY";
        case VK_ERROR_INVALID_EXTERNAL_HANDLE: return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
        default: return "VkResult(unrecognized)";
    }
}

// ---------------------------------------------------------------------------

struct VulkanContext::Impl {
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    VkCommandPool immediatePool = VK_NULL_HANDLE;

    mutable std::mutex diagnosticMutex;
    std::string lastValidationError;
    std::string lastThirdPartyValidationError;
};

namespace {

VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void* userData)
{
    auto* context = static_cast<VulkanContext*>(userData);
    if (context == nullptr || data == nullptr) {
        return VK_FALSE;
    }

    // Severity is a bitmask, not an ordinal. hdCodex's callback compared it
    // with >= and happened to work because of the bit values; testing the bit
    // is what actually expresses the intent (lessons R8 / hdCodex N13).
    const bool isError =
        (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0;
    const bool isWarning =
        (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0;

    // Whether the finding is entirely inside NVIDIA's NGX runtime.
    //
    // NGX records into the command buffer it is handed, names every resource
    // it allocates `nv.ngx.*`, and opens a debug label of the same prefix
    // around what it records. A message qualifies only when all three agree:
    // the innermost label is NGX's, and every object it names other than the
    // command buffer -- which is hdClaude's, lent to NGX -- is NGX's own. A
    // finding that touches one of hdClaude's images or buffers does not
    // qualify and fails the gate as any other does.
    //
    // Counted and printed in full rather than silenced: the gate cannot be made
    // to pass by a defect hdClaude cannot fix, and it must not stop saying the
    // defect is there. The first were two WRITE_AFTER_WRITE hazards inside Ray
    // Reconstruction's own Evaluate, on its first evaluation, DLSS 310.9.1.
    const auto startsWithNgx = [](const char* name) {
        return name != nullptr && std::strncmp(name, "nv.ngx.", 7) == 0;
    };
    bool thirdParty = data->cmdBufLabelCount > 0 &&
                      startsWithNgx(data->pCmdBufLabels[0].pLabelName);
    bool namesAnyResource = false;
    for (uint32_t i = 0; thirdParty && i < data->objectCount; ++i) {
        if (data->pObjects[i].objectType == VK_OBJECT_TYPE_COMMAND_BUFFER) {
            continue;
        }
        namesAnyResource = true;
        thirdParty = startsWithNgx(data->pObjects[i].pObjectName);
    }
    thirdParty = thirdParty && namesAnyResource;

    if (isError || isWarning) {
        std::fprintf(stderr, "[vulkan %s%s] %s\n", isError ? "error" : "warning",
                     thirdParty ? ", inside NGX" : "",
                     data->pMessage ? data->pMessage : "(no message)");
    }
    context->NoteValidationMessage(isError && !thirdParty, isWarning,
                                   data->pMessage ? data->pMessage : "");
    if (isError && thirdParty) {
        context->NoteThirdPartyValidationError(data->pMessage ? data->pMessage
                                                              : "");
    }
    return VK_FALSE;
}

}  // namespace

void VulkanContext::NoteValidationMessage(bool isError, bool isWarning,
                                          const char* message) const
{
    if (isError) {
        _validationErrors.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(_impl->diagnosticMutex);
        _impl->lastValidationError = message;
    } else if (isWarning) {
        _validationWarnings.fetch_add(1, std::memory_order_relaxed);
    }
}

void VulkanContext::NoteThirdPartyValidationError(const char* message) const
{
    _thirdPartyValidationErrors.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(_impl->diagnosticMutex);
    _impl->lastThirdPartyValidationError = message;
}

std::string VulkanContext::LastThirdPartyValidationError() const
{
    std::lock_guard<std::mutex> lock(_impl->diagnosticMutex);
    return _impl->lastThirdPartyValidationError;
}

std::string VulkanContext::LastValidationError() const
{
    std::lock_guard<std::mutex> lock(_impl->diagnosticMutex);
    return _impl->lastValidationError;
}

void VulkanContext::ResetValidationCounters()
{
    _validationErrors.store(0, std::memory_order_relaxed);
    _validationWarnings.store(0, std::memory_order_relaxed);
    _thirdPartyValidationErrors.store(0, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(_impl->diagnosticMutex);
    _impl->lastValidationError.clear();
    _impl->lastThirdPartyValidationError.clear();
}

void VulkanContext::Check(VkResult result, const char* context) const
{
    if (result == VK_SUCCESS) {
        return;
    }
    if (result == VK_ERROR_DEVICE_LOST) {
        // Latched before throwing, so that unwinding destructors already see
        // the lost state and skip their waits and destroys.
        _deviceLost.store(true, std::memory_order_release);
    }
    throw VulkanError(result, context);
}

void VulkanContext::RequireLive(const char* context) const
{
    if (IsDeviceLost()) {
        throw VulkanError(VK_ERROR_DEVICE_LOST, context);
    }
}

namespace {

std::atomic<std::uint64_t> g_deviceWaitsIssued{0};

}  // namespace

std::uint64_t VulkanContext::DeviceWaitsIssuedForTesting()
{
    return g_deviceWaitsIssued.load(std::memory_order_relaxed);
}

void VulkanContext::WaitIdle() const
{
    if (_device == VK_NULL_HANDLE || IsDeviceLost()) {
        return;
    }
    g_deviceWaitsIssued.fetch_add(1, std::memory_order_relaxed);
    Check(vkDeviceWaitIdle(_device), "vkDeviceWaitIdle");
}

// ---------------------------------------------------------------------------

VulkanSupport VulkanContext::Probe(const VulkanContextOptions& options)
{
    VulkanSupport support;
    if (volkInitialize() != VK_SUCCESS) {
        support.reason = "no Vulkan loader is present";
        return support;
    }

    VkApplicationInfo applicationInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    applicationInfo.pApplicationName = "hdClaude probe";
    applicationInfo.apiVersion = VK_API_VERSION_1_3;

    // No layers and no extensions: enumerating physical devices and reading
    // their extension properties needs neither, and a probe that enabled the
    // validation layers would make discovering the renderer slower than
    // rendering with it.
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &applicationInfo;

    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&instanceInfo, nullptr, &instance) != VK_SUCCESS) {
        support.reason = "no Vulkan instance could be created";
        return support;
    }
    volkLoadInstanceOnly(instance);

    std::uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
    std::vector<VkPhysicalDevice> devices(deviceCount);
    if (deviceCount > 0) {
        vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());
    }

    const std::string preferred = ToLower(options.preferredDeviceName);
    std::string rejected;
    for (VkPhysicalDevice candidate : devices) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(candidate, &properties);
        const auto extensions = DeviceExtensionProperties(candidate);
        const bool traces =
            HasExtension(extensions, VK_KHR_RAY_QUERY_EXTENSION_NAME) &&
            HasExtension(extensions,
                         VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);

        if (!traces) {
            if (!rejected.empty()) {
                rejected += ", ";
            }
            rejected += properties.deviceName;
            continue;
        }
        // The same preference the constructor applies, so the probe reports the
        // device the renderer would actually choose.
        if (!support.supported ||
            (!preferred.empty() &&
             ToLower(properties.deviceName).find(preferred) !=
                 std::string::npos)) {
            support.supported = true;
            support.deviceName = properties.deviceName;
        }
    }

    if (!support.supported) {
        support.reason =
            devices.empty()
                ? "no Vulkan physical device is present"
                : "no Vulkan device supports VK_KHR_ray_query with hardware "
                  "acceleration structures (found: " +
                      rejected + ")";
    }

    vkDestroyInstance(instance, nullptr);
    return support;
}

VulkanContext::VulkanContext(const VulkanContextOptions& options)
    : _impl(std::make_unique<Impl>())
{
    if (volkInitialize() != VK_SUCCESS) {
        throw VulkanError(VK_ERROR_INITIALIZATION_FAILED,
                          "No Vulkan loader was found on this system");
    }

    _validationEnabled = options.enableValidation ||
                         EnvironmentFlag("HDCLAUDE_ENABLE_VULKAN_VALIDATION");

    // --- Instance ------------------------------------------------------------
    std::uint32_t layerCount = 0;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
    std::vector<VkLayerProperties> layers(layerCount);
    if (layerCount > 0) {
        vkEnumerateInstanceLayerProperties(&layerCount, layers.data());
    }
    const bool validationAvailable =
        std::any_of(layers.begin(), layers.end(), [](const VkLayerProperties& l) {
            return std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0;
        });
    if (_validationEnabled && !validationAvailable) {
        std::fprintf(stderr,
                     "[hdClaude] Vulkan validation was requested but the "
                     "Khronos validation layer is not installed. Continuing "
                     "without it; validation-gated tests must not pass.\n");
        _validationEnabled = false;
    }

    std::vector<const char*> instanceLayers;
    std::vector<const char*> instanceExtensions;

    // Synchronisation validation, asked for through the layer's own settings
    // extension. Kept alive until vkCreateInstance because the chain points at
    // it.
    //
    // Core validation checks that each command is legal in isolation; it says
    // nothing about whether a buffer one kernel writes is visible to the next.
    // A wavefront integrator is almost entirely that question, so the gate
    // rule (docs/lessons-from-hdcodex.md R8) is only worth what this setting
    // adds -- it is what caught the counter reset that raced its own promotion
    // copy. The settings extension is used rather than the deprecated
    // VkValidationFeaturesEXT, which the layer now reports as a warning.
    const VkBool32 enableSyncValidation = VK_TRUE;
    VkLayerSettingEXT syncSetting{};
    syncSetting.pLayerName = "VK_LAYER_KHRONOS_validation";
    syncSetting.pSettingName = "validate_sync";
    syncSetting.type = VK_LAYER_SETTING_TYPE_BOOL32_EXT;
    syncSetting.valueCount = 1;
    syncSetting.pValues = &enableSyncValidation;

    VkLayerSettingsCreateInfoEXT layerSettings{
        VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT};
    layerSettings.settingCount = 1;
    layerSettings.pSettings = &syncSetting;
    bool layerSettingsAvailable = false;

    if (_validationEnabled) {
        instanceLayers.push_back("VK_LAYER_KHRONOS_validation");
        instanceExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

        // The settings extension is exposed by the validation layer itself, so
        // it has to be enumerated against that layer rather than against the
        // driver's list.
        std::uint32_t layerExtensionCount = 0;
        vkEnumerateInstanceExtensionProperties("VK_LAYER_KHRONOS_validation",
                                               &layerExtensionCount, nullptr);
        std::vector<VkExtensionProperties> layerExtensions(layerExtensionCount);
        if (layerExtensionCount > 0) {
            vkEnumerateInstanceExtensionProperties("VK_LAYER_KHRONOS_validation",
                                                   &layerExtensionCount,
                                                   layerExtensions.data());
        }
        layerSettingsAvailable =
            HasExtension(layerExtensions, VK_EXT_LAYER_SETTINGS_EXTENSION_NAME);
        _synchronisationValidationEnabled = layerSettingsAvailable;
        if (layerSettingsAvailable) {
            instanceExtensions.push_back(VK_EXT_LAYER_SETTINGS_EXTENSION_NAME);
        } else {
            std::fprintf(stderr,
                         "[hdClaude] The validation layer does not support "
                         "VK_EXT_layer_settings, so synchronisation validation "
                         "is off; hazards between kernels will not be "
                         "reported.\n");
        }
    }

    // Backend requirements are gathered before the instance exists, which is
    // the whole point of the provider: an optional backend cannot retrofit an
    // instance extension after creation.
    std::uint32_t availableCount = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &availableCount, nullptr);
    std::vector<VkExtensionProperties> availableInstance(availableCount);
    if (availableCount > 0) {
        vkEnumerateInstanceExtensionProperties(nullptr, &availableCount,
                                               availableInstance.data());
    }
    for (const VulkanRequirementProvider* provider : options.requirementProviders) {
        if (provider == nullptr) {
            continue;
        }
        for (const char* name : provider->InstanceExtensions()) {
            if (HasExtension(availableInstance, name) &&
                std::none_of(instanceExtensions.begin(), instanceExtensions.end(),
                             [name](const char* e) { return std::strcmp(e, name) == 0; })) {
                instanceExtensions.push_back(name);
            }
        }
    }

    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName = "hdClaude";
    application.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    application.pEngineName = "hdClaude";
    application.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    application.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &application;
    if (layerSettingsAvailable) {
        instanceInfo.pNext = &layerSettings;
    }
    instanceInfo.enabledLayerCount = static_cast<std::uint32_t>(instanceLayers.size());
    instanceInfo.ppEnabledLayerNames = instanceLayers.data();
    instanceInfo.enabledExtensionCount =
        static_cast<std::uint32_t>(instanceExtensions.size());
    instanceInfo.ppEnabledExtensionNames = instanceExtensions.data();

    // Timed under HDCLAUDE_TRACE, because a context is seconds of a user's wait
    // and the three steps cost wildly different amounts: on an RTX 5060 Ti the
    // instance is 50 ms, choosing the device 1 ms, and `vkCreateDevice` 3.8
    // seconds. Knowing which is which is what stopped the renderer paying for
    // four of them per render (docs/implementation-notes.md, 2026-09-16).
    const auto hdclaudeInstanceStart = std::chrono::steady_clock::now();
    Check(vkCreateInstance(&instanceInfo, nullptr, &_instance), "vkCreateInstance");
    volkLoadInstanceOnly(_instance);
    const auto hdclaudeInstanceDone = std::chrono::steady_clock::now();
    if (std::getenv("HDCLAUDE_TRACE") != nullptr) {
        std::fprintf(stderr, "[hdClaude] vkCreateInstance: %.0f ms\n",
                     std::chrono::duration<double, std::milli>(
                         hdclaudeInstanceDone - hdclaudeInstanceStart)
                         .count());
    }

    if (_validationEnabled) {
        VkDebugUtilsMessengerCreateInfoEXT messengerInfo{
            VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        messengerInfo.messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        messengerInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                    VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                    VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        messengerInfo.pfnUserCallback = DebugCallback;
        messengerInfo.pUserData = this;
        Check(vkCreateDebugUtilsMessengerEXT(_instance, &messengerInfo, nullptr,
                                             &_impl->messenger),
              "vkCreateDebugUtilsMessengerEXT");
    }

    SelectPhysicalDevice(options);
    const auto hdclaudeSelectDone = std::chrono::steady_clock::now();
    CreateDevice(options);
    if (std::getenv("HDCLAUDE_TRACE") != nullptr) {
        std::fprintf(stderr,
                     "[hdClaude] selectPhysicalDevice: %.0f ms, vkCreateDevice: "
                     "%.0f ms\n",
                     std::chrono::duration<double, std::milli>(
                         hdclaudeSelectDone - hdclaudeInstanceDone)
                         .count(),
                     std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - hdclaudeSelectDone)
                         .count());
    }

    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
                     VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = _queueFamily;
    Check(vkCreateCommandPool(_device, &poolInfo, nullptr, &_impl->immediatePool),
          "vkCreateCommandPool(immediate)");
}

void VulkanContext::SelectPhysicalDevice(const VulkanContextOptions& options)
{
    std::uint32_t deviceCount = 0;
    Check(vkEnumeratePhysicalDevices(_instance, &deviceCount, nullptr),
          "vkEnumeratePhysicalDevices");
    if (deviceCount == 0) {
        throw VulkanError(VK_ERROR_INITIALIZATION_FAILED,
                          "No Vulkan physical device is present");
    }
    std::vector<VkPhysicalDevice> devices(deviceCount);
    Check(vkEnumeratePhysicalDevices(_instance, &deviceCount, devices.data()),
          "vkEnumeratePhysicalDevices");

    const std::string preferred = ToLower(options.preferredDeviceName);

    VkPhysicalDevice best = VK_NULL_HANDLE;
    std::uint64_t bestScore = 0;
    VulkanCapabilities bestCapabilities;
    std::string rejectionSummary;

    for (VkPhysicalDevice candidate : devices) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(candidate, &properties);

        const auto extensions = DeviceExtensionProperties(candidate);

        VulkanCapabilities capabilities;
        capabilities.deviceName = properties.deviceName;
        capabilities.driverVersion =
            FormatDriverVersion(properties.driverVersion, properties.vendorID);
        capabilities.rayQuery = HasExtension(extensions, VK_KHR_RAY_QUERY_EXTENSION_NAME);
        capabilities.accelerationStructure =
            HasExtension(extensions, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
        capabilities.bufferDeviceAddress =
            HasExtension(extensions, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME) ||
            properties.apiVersion >= VK_API_VERSION_1_2;
        capabilities.descriptorIndexing =
            HasExtension(extensions, VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME) ||
            properties.apiVersion >= VK_API_VERSION_1_2;
        capabilities.invocationReorder =
            HasExtension(extensions, "VK_NV_ray_tracing_invocation_reorder");
        capabilities.externalMemory =
            HasExtension(extensions, "VK_KHR_external_memory_win32");

        // Ray query and acceleration structures are hard requirements: they are
        // how the renderer traverses. Everything else in VulkanCapabilities is
        // a performance path.
        if (!capabilities.rayQuery || !capabilities.accelerationStructure) {
            rejectionSummary += std::string("  ") + properties.deviceName +
                                ": no ray query / acceleration structure support\n";
            continue;
        }
        if (properties.apiVersion < VK_API_VERSION_1_3) {
            rejectionSummary += std::string("  ") + properties.deviceName +
                                ": Vulkan 1.3 required\n";
            continue;
        }

        bool providersAccept = true;
        for (const VulkanRequirementProvider* provider : options.requirementProviders) {
            if (provider != nullptr && !provider->SupportsDevice(candidate)) {
                providersAccept = false;
            }
        }
        (void)providersAccept;  // Providers cannot veto; see the class comment.

        VkPhysicalDeviceMemoryProperties memory{};
        vkGetPhysicalDeviceMemoryProperties(candidate, &memory);
        std::uint64_t deviceLocal = 0;
        for (std::uint32_t i = 0; i < memory.memoryHeapCount; ++i) {
            if ((memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) {
                deviceLocal = std::max<std::uint64_t>(deviceLocal, memory.memoryHeaps[i].size);
            }
        }
        capabilities.deviceLocalMemory = deviceLocal;

        VkPhysicalDeviceAccelerationStructurePropertiesKHR accelerationProperties{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
        VkPhysicalDeviceSubgroupProperties subgroup{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
        subgroup.pNext = &accelerationProperties;
        VkPhysicalDeviceProperties2 properties2{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        properties2.pNext = &subgroup;
        vkGetPhysicalDeviceProperties2(candidate, &properties2);
        capabilities.subgroupSize = subgroup.subgroupSize;
        capabilities.timestampPeriod = properties.limits.timestampPeriod;

        if (properties.limits.minUniformBufferOffsetAlignment > 0) {
            capabilities.uniformBufferOffsetAlignment =
                properties.limits.minUniformBufferOffsetAlignment;
        }
        if (accelerationProperties.minAccelerationStructureScratchOffsetAlignment > 0) {
            capabilities.scratchAlignment =
                accelerationProperties.minAccelerationStructureScratchOffsetAlignment;
        }

        // Discrete first, then device-local memory. An explicit name preference
        // dominates both, so a workstation with an integrated GPU alongside the
        // discrete one can be steered without code changes.
        std::uint64_t score = deviceLocal >> 20;
        if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            score += 1ull << 40;
        }
        if (!preferred.empty() &&
            ToLower(properties.deviceName).find(preferred) != std::string::npos) {
            score += 1ull << 50;
        }

        if (score > bestScore) {
            bestScore = score;
            best = candidate;
            bestCapabilities = capabilities;
        }
    }

    if (best == VK_NULL_HANDLE) {
        throw VulkanError(
            VK_ERROR_FEATURE_NOT_PRESENT,
            "No Vulkan device supports the required ray-query feature set.\n" +
                rejectionSummary);
    }

    _physicalDevice = best;
    _capabilities = bestCapabilities;
    vkGetPhysicalDeviceMemoryProperties(_physicalDevice, &_memoryProperties);
}

void VulkanContext::CreateDevice(const VulkanContextOptions& options)
{
    std::uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(_physicalDevice, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(_physicalDevice, &familyCount,
                                             families.data());

    bool found = false;
    for (std::uint32_t i = 0; i < familyCount; ++i) {
        const VkQueueFlags required = VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
        if ((families[i].queueFlags & required) == required) {
            _queueFamily = i;
            // Timestamps are a property of the *queue family*, not only of
            // the device: a family may report zero valid bits and write
            // nothing useful. Recorded with the family that was chosen, so
            // the profile can decline rather than report noise.
            _capabilities.timestampValidBits = families[i].timestampValidBits;
            found = true;
            break;
        }
    }
    if (!found) {
        throw VulkanError(VK_ERROR_FEATURE_NOT_PRESENT,
                          "No queue family supports compute and transfer");
    }

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = _queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;

    std::vector<const char*> deviceExtensions{
        VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
        VK_KHR_RAY_QUERY_EXTENSION_NAME,
        VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
    };
    // VK_NV_ray_tracing_invocation_reorder is deliberately NOT enabled.
    //
    // Its GLSL builtins (hitObjectNV, reorderThreadNV) are registered only on
    // the ray-generation, closest-hit, and miss stages -- not on compute. SER
    // is therefore unusable from hdClaude's ray-query compute kernels, and
    // enabling the extension would additionally drag in
    // VK_KHR_ray_tracing_pipeline as a required dependency, which validation
    // rejects if omitted.
    //
    // This costs little: in a wavefront renderer the per-material sort already
    // provides the execution coherence SER exists to recover, and SER's real
    // value is to megakernel and RT-pipeline designs that cannot sort. The
    // capability is still detected and reported so the decision stays visible.
    // See docs/architecture.md 2.1.
    if (_capabilities.externalMemory) {
        deviceExtensions.push_back("VK_KHR_external_memory_win32");
    }

    const auto available = DeviceExtensionProperties(_physicalDevice);
    for (const VulkanRequirementProvider* provider : options.requirementProviders) {
        if (provider == nullptr) {
            continue;
        }
        for (const char* name : provider->DeviceExtensions(_physicalDevice)) {
            if (HasExtension(available, name) &&
                std::none_of(deviceExtensions.begin(), deviceExtensions.end(),
                             [name](const char* e) { return std::strcmp(e, name) == 0; })) {
                deviceExtensions.push_back(name);
            }
        }
    }

    VkPhysicalDeviceAccelerationStructureFeaturesKHR accelerationFeatures{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    accelerationFeatures.accelerationStructure = VK_TRUE;

    VkPhysicalDeviceRayQueryFeaturesKHR rayQueryFeatures{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    rayQueryFeatures.rayQuery = VK_TRUE;
    rayQueryFeatures.pNext = &accelerationFeatures;

    VkPhysicalDeviceVulkan12Features features12{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    features12.bufferDeviceAddress = VK_TRUE;
    features12.descriptorIndexing = VK_TRUE;
    features12.runtimeDescriptorArray = VK_TRUE;
    features12.descriptorBindingPartiallyBound = VK_TRUE;
    features12.descriptorBindingVariableDescriptorCount = VK_TRUE;
    features12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    features12.shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;
    features12.scalarBlockLayout = VK_TRUE;
    features12.hostQueryReset = VK_TRUE;
    features12.pNext = &rayQueryFeatures;

    VkPhysicalDeviceVulkan13Features features13{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    features13.synchronization2 = VK_TRUE;
    features13.maintenance4 = VK_TRUE;
    features13.computeFullSubgroups = VK_TRUE;
    features13.subgroupSizeControl = VK_TRUE;
    features13.pNext = &features12;

    VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features2.features.shaderInt64 = VK_TRUE;
    features2.features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    features2.pNext = &features13;

    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount =
        static_cast<std::uint32_t>(deviceExtensions.size());
    deviceInfo.ppEnabledExtensionNames = deviceExtensions.data();
    deviceInfo.pNext = &features2;

    Check(vkCreateDevice(_physicalDevice, &deviceInfo, nullptr, &_device),
          "vkCreateDevice");
    volkLoadDevice(_device);
    vkGetDeviceQueue(_device, _queueFamily, 0, &_queue);

    CreatePipelineCache();
}

std::filesystem::path VulkanContext::PipelineCachePath()
{
    const std::string override = EnvironmentValue("HDCLAUDE_PIPELINE_CACHE");
    if (!override.empty()) {
        return std::filesystem::path(override);
    }
    std::error_code error;
    std::filesystem::path directory = std::filesystem::temp_directory_path(error);
    if (error) {
        return {};
    }
    return directory / "hdClaude.pipeline.cache";
}

void VulkanContext::CreatePipelineCache()
{
    // A pipeline cache of our own, persisted between runs.
    //
    // Without one, every pipeline is compiled by the driver during the run
    // unless the driver's *implicit* disk cache happens to hold it -- and that
    // cache is not ours, is keyed on things we do not control, and can be
    // cleared by anything on the machine. It is also not a performance question
    // only: a pipeline compiled during a run produces a first execution that
    // differs numerically from its later ones, so a cold compile costs a
    // divergent first frame. Emptying the driver's cache and rendering the same
    // scene is enough to show it, and putting the cache back is enough to make
    // it stop.
    //
    // The data is passed to the driver exactly as it was read. A cache blob
    // from another device, driver or vendor is *required* to be rejected by the
    // implementation after it checks the header it wrote, so a stale or foreign
    // file costs a recompile rather than anything worse -- which is why no
    // validation is attempted here beyond reading the bytes.
    _pipelineCachePath = PipelineCachePath();
    std::vector<char> initial;
    if (!_pipelineCachePath.empty()) {
        std::ifstream file(_pipelineCachePath, std::ios::binary);
        if (file) {
            initial.assign(std::istreambuf_iterator<char>(file),
                           std::istreambuf_iterator<char>());
        }
    }

    VkPipelineCacheCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    info.initialDataSize = initial.size();
    info.pInitialData = initial.empty() ? nullptr : initial.data();
    // Not checked: a cache is an optimisation and a renderer that cannot create
    // one still renders. Failing the device over it would trade a working image
    // for a faster start.
    if (vkCreatePipelineCache(_device, &info, nullptr, &_pipelineCache) !=
        VK_SUCCESS) {
        _pipelineCache = VK_NULL_HANDLE;
    }
}

void VulkanContext::SavePipelineCache() const
{
    if (_pipelineCache == VK_NULL_HANDLE || _pipelineCachePath.empty() ||
        IsDeviceLost()) {
        return;
    }
    std::size_t size = 0;
    if (vkGetPipelineCacheData(_device, _pipelineCache, &size, nullptr) !=
            VK_SUCCESS ||
        size == 0) {
        return;
    }
    std::vector<char> data(size);
    if (vkGetPipelineCacheData(_device, _pipelineCache, &size, data.data()) !=
        VK_SUCCESS) {
        return;
    }
    // Written to a temporary and renamed, so a run interrupted mid-write leaves
    // the previous cache rather than a truncated one the driver must reject.
    std::error_code error;
    const std::filesystem::path temporary =
        _pipelineCachePath.string() + ".partial";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) {
            return;
        }
        file.write(data.data(), static_cast<std::streamsize>(size));
        if (!file) {
            return;
        }
    }
    std::filesystem::rename(temporary, _pipelineCachePath, error);
}

VulkanContext::~VulkanContext()
{
    // Rule 3, stated precisely: on a lost device we skip the *wait*, not the
    // destruction.
    //
    // Waiting is what must be skipped -- an unchecked vkDeviceWaitIdle on a
    // dead device returns VK_ERROR_DEVICE_LOST, is ignored, and every
    // diagnostic gathered afterwards is contaminated (lessons C3, hdCodex N4).
    //
    // Destroying is not optional. vkDestroyDevice requires that every child
    // object has already been destroyed, and destruction remains valid after
    // device loss. Skipping the command pool here to "avoid touching a dead
    // device" is an access violation in vkDestroyDevice, which is exactly what
    // the first run of the device-loss test produced.
    const bool lost = IsDeviceLost();

    if (_device != VK_NULL_HANDLE) {
        if (!lost) {
            g_deviceWaitsIssued.fetch_add(1, std::memory_order_relaxed);
            vkDeviceWaitIdle(_device);
        }
        if (_impl->immediatePool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(_device, _impl->immediatePool, nullptr);
        }
        if (_pipelineCache != VK_NULL_HANDLE) {
            SavePipelineCache();
            vkDestroyPipelineCache(_device, _pipelineCache, nullptr);
        }
        vkDestroyDevice(_device, nullptr);
    }
    if (_impl->messenger != VK_NULL_HANDLE) {
        vkDestroyDebugUtilsMessengerEXT(_instance, _impl->messenger, nullptr);
    }
    if (_instance != VK_NULL_HANDLE) {
        vkDestroyInstance(_instance, nullptr);
    }
}

void VulkanContext::SubmitImmediate(
    const std::function<void(VkCommandBuffer)>& record) const
{
    RequireLive("SubmitImmediate");

    VkCommandBufferAllocateInfo allocateInfo{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocateInfo.commandPool = _impl->immediatePool;
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount = 1;

    VkCommandBuffer command = VK_NULL_HANDLE;
    Check(vkAllocateCommandBuffers(_device, &allocateInfo, &command),
          "vkAllocateCommandBuffers(immediate)");

    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};

    try {
        Check(vkCreateFence(_device, &fenceInfo, nullptr, &fence),
              "vkCreateFence(immediate)");

        VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        Check(vkBeginCommandBuffer(command, &beginInfo), "vkBeginCommandBuffer");
        record(command);
        Check(vkEndCommandBuffer(command), "vkEndCommandBuffer");

        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        Check(vkQueueSubmit(_queue, 1, &submit, fence), "vkQueueSubmit(immediate)");
        Check(vkWaitForFences(_device, 1, &fence, VK_TRUE, UINT64_MAX),
              "vkWaitForFences(immediate)");
    } catch (...) {
        if (fence != VK_NULL_HANDLE && !IsDeviceLost()) {
            vkDestroyFence(_device, fence, nullptr);
        }
        if (!IsDeviceLost()) {
            vkFreeCommandBuffers(_device, _impl->immediatePool, 1, &command);
        }
        throw;
    }

    vkDestroyFence(_device, fence, nullptr);
    vkFreeCommandBuffers(_device, _impl->immediatePool, 1, &command);
}

}  // namespace hdclaude
