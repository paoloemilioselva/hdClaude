#include "hdclaude/gpu/vulkan_context.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <sstream>

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

    if (isError || isWarning) {
        std::fprintf(stderr, "[vulkan %s] %s\n", isError ? "error" : "warning",
                     data->pMessage ? data->pMessage : "(no message)");
    }
    context->NoteValidationMessage(isError, isWarning,
                                   data->pMessage ? data->pMessage : "");
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

std::string VulkanContext::LastValidationError() const
{
    std::lock_guard<std::mutex> lock(_impl->diagnosticMutex);
    return _impl->lastValidationError;
}

void VulkanContext::ResetValidationCounters()
{
    _validationErrors.store(0, std::memory_order_relaxed);
    _validationWarnings.store(0, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(_impl->diagnosticMutex);
    _impl->lastValidationError.clear();
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

void VulkanContext::WaitIdle() const
{
    if (_device == VK_NULL_HANDLE || IsDeviceLost()) {
        return;
    }
    Check(vkDeviceWaitIdle(_device), "vkDeviceWaitIdle");
}

// ---------------------------------------------------------------------------

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
    if (_validationEnabled) {
        instanceLayers.push_back("VK_LAYER_KHRONOS_validation");
        instanceExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
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
    instanceInfo.enabledLayerCount = static_cast<std::uint32_t>(instanceLayers.size());
    instanceInfo.ppEnabledLayerNames = instanceLayers.data();
    instanceInfo.enabledExtensionCount =
        static_cast<std::uint32_t>(instanceExtensions.size());
    instanceInfo.ppEnabledExtensionNames = instanceExtensions.data();

    Check(vkCreateInstance(&instanceInfo, nullptr, &_instance), "vkCreateInstance");
    volkLoadInstanceOnly(_instance);

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
    CreateDevice(options);

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

        VkPhysicalDeviceSubgroupProperties subgroup{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
        VkPhysicalDeviceProperties2 properties2{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        properties2.pNext = &subgroup;
        vkGetPhysicalDeviceProperties2(candidate, &properties2);
        capabilities.subgroupSize = subgroup.subgroupSize;

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
            vkDeviceWaitIdle(_device);
        }
        if (_impl->immediatePool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(_device, _impl->immediatePool, nullptr);
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
