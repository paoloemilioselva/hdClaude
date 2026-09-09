// Vulkan instance, physical device, and logical device ownership.
//
// hdClaude creates a *private* Vulkan device rather than adopting the Hydra
// host's Hgi device. That decision and its consequence for AOV delivery are
// recorded in docs/architecture.md 8.
//
// Two rules from docs/architecture.md 6 are enforced here rather than left to
// callers:
//
//   Rule 3  Every Vulkan result is checked, vkDeviceWaitIdle included. A sticky
//           deviceLost latch is set by the checker, gates every entry point,
//           and is honoured by destructors so that a post-mortem teardown does
//           not issue calls against a dead device and contaminate diagnostics.
//   R8      Validation errors are counted, not merely printed. A run with
//           validation enabled can therefore fail rather than rely on someone
//           having read the console.

#ifndef HDCLAUDE_GPU_VULKAN_CONTEXT_H
#define HDCLAUDE_GPU_VULKAN_CONTEXT_H

#include <atomic>
#include <filesystem>
#include <functional>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <volk.h>

namespace hdclaude {

/// Thrown for any failed Vulkan call. Carries the result so callers can
/// distinguish recoverable conditions from device loss.
class VulkanError : public std::runtime_error {
  public:
    VulkanError(VkResult result, std::string context);

    VkResult Result() const { return _result; }
    bool IsDeviceLost() const
    {
        return _result == VK_ERROR_DEVICE_LOST ||
               _result == VK_ERROR_SURFACE_LOST_KHR;
    }

  private:
    VkResult _result;
};

/// Human-readable Vulkan result name, for diagnostics.
const char* ToString(VkResult result);

/// Requirements an optional backend contributes before the device exists.
///
/// Instance extensions must be known before instance creation, and device
/// extensions and features before *device selection* -- a backend cannot be
/// allowed to force a device choice on a system that will not use it. The
/// provider is consulted during bootstrap and then dropped; it is never
/// retained, so a backend cannot mutate requirements after the fact.
class VulkanRequirementProvider {
  public:
    virtual ~VulkanRequirementProvider() = default;

    /// Instance extensions this backend needs. Missing extensions disable the
    /// backend; they never fail context creation.
    virtual std::vector<const char*> InstanceExtensions() const { return {}; }

    /// Device extensions this backend needs, given a candidate physical device.
    virtual std::vector<const char*> DeviceExtensions(VkPhysicalDevice) const
    {
        return {};
    }

    /// Whether this backend can use the candidate device at all. A false answer
    /// does not reject the device; it only means the backend will be inactive.
    virtual bool SupportsDevice(VkPhysicalDevice) const { return true; }
};

struct VulkanContextOptions {
    /// Enable the Khronos validation layer. Also enabled by setting the
    /// environment variable HDCLAUDE_ENABLE_VULKAN_VALIDATION=1.
    bool enableValidation = false;

    /// Prefer this device by name substring, case-insensitive. Empty selects
    /// the highest-scoring capable device.
    std::string preferredDeviceName;

    /// Optional backend requirements, consulted during bootstrap only.
    std::vector<const VulkanRequirementProvider*> requirementProviders;
};

/// Capabilities discovered at device selection, for callers to branch on.
///
/// Every entry here is a *performance* path. Absence must never change what an
/// image looks like, only how long it takes -- see docs/architecture.md 2.1.
struct VulkanCapabilities {
    bool rayQuery = false;
    bool accelerationStructure = false;
    bool bufferDeviceAddress = false;
    bool descriptorIndexing = false;
    /// VK_NV_ray_tracing_invocation_reorder: Shader Execution Reordering.
    bool invocationReorder = false;
    /// VK_KHR_external_memory_win32, for the later Hgi interop phase.
    bool externalMemory = false;

    std::uint32_t subgroupSize = 0;
    /// Alignment every dynamic uniform buffer offset must be a multiple of.
    ///
    /// The frame block is uploaded once per sample into one buffer and bound
    /// with a per-sample offset, so the stride between copies is this rounded
    /// up. 256 is the conservative default the specification's minimum
    /// guarantees, used if the device is never asked.
    std::uint64_t uniformBufferOffsetAlignment = 256;

    /// Nanoseconds per timestamp tick, and how many bits of a timestamp are
    /// meaningful. Zero for the period means the device does not support
    /// timestamps at all, in which case the kernel profile is unavailable
    /// rather than wrong.
    float timestampPeriod = 0.0f;
    std::uint32_t timestampValidBits = 0;
    /// Alignment an acceleration-structure build's scratch address must meet.
    ///
    /// Recorded because nothing else enforces it: a scratch buffer's own
    /// alignment requirement is far weaker, so an address that violates this
    /// one is what an allocator hands back most of the time and a device loss
    /// is what the driver does about it.
    std::uint32_t scratchAlignment = 128;
    std::uint64_t deviceLocalMemory = 0;
    std::string deviceName;
    std::string driverVersion;
};

class VulkanContext {
  public:
    explicit VulkanContext(const VulkanContextOptions& options = {});
    ~VulkanContext();

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    VkInstance Instance() const { return _instance; }
    VkPhysicalDevice PhysicalDevice() const { return _physicalDevice; }
    VkDevice Device() const { return _device; }
    VkQueue Queue() const { return _queue; }

    /// The pipeline cache every pipeline is created against, persisted between
    /// runs. May be VK_NULL_HANDLE, which is legal to pass and simply means the
    /// driver compiles from scratch.
    ///
    /// It exists for correctness as much as for speed. A pipeline the driver
    /// compiles during a run has a first execution that differs numerically
    /// from its later ones, so a cold compile costs a divergent first frame --
    /// demonstrated by emptying the driver's own cache, which makes an
    /// otherwise reproducible render diverge, and restoring it, which stops it.
    VkPipelineCache PipelineCache() const { return _pipelineCache; }
    std::uint32_t QueueFamily() const { return _queueFamily; }

    const VulkanCapabilities& Capabilities() const { return _capabilities; }
    const VkPhysicalDeviceMemoryProperties& MemoryProperties() const
    {
        return _memoryProperties;
    }

    // --- Failure state -------------------------------------------------------

    /// True once any checked call has returned VK_ERROR_DEVICE_LOST. Sticky:
    /// a lost device is never recovered in-process.
    bool IsDeviceLost() const
    {
        return _deviceLost.load(std::memory_order_acquire);
    }

    /// Check a Vulkan result, latching device loss and throwing on failure.
    /// Every Vulkan call in hdClaude goes through this, vkDeviceWaitIdle
    /// included -- an unchecked wait on a dead device silently succeeds and
    /// makes everything measured afterwards worthless.
    void Check(VkResult result, const char* context) const;

    /// Overload for contexts assembled at runtime, such as a resource name.
    void Check(VkResult result, const std::string& context) const
    {
        Check(result, context.c_str());
    }

    /// vkDeviceWaitIdle, checked, and a no-op once the device is lost.
    void WaitIdle() const;

    /// Throws if the device has been lost. Called at the top of every public
    /// entry point of every subsystem that owns GPU work.
    void RequireLive(const char* context) const;

    // --- Validation ----------------------------------------------------------

    bool ValidationEnabled() const { return _validationEnabled; }

    /// Validation messages seen at ERROR severity. A test with validation
    /// enabled must fail on a nonzero count; printing alone is not a gate.
    std::uint64_t ValidationErrorCount() const
    {
        return _validationErrors.load(std::memory_order_relaxed);
    }
    std::uint64_t ValidationWarningCount() const
    {
        return _validationWarnings.load(std::memory_order_relaxed);
    }
    void ResetValidationCounters();

    /// The most recent validation error text, for test diagnostics.
    std::string LastValidationError() const;

    // --- Immediate submission ------------------------------------------------

    /// Record and submit a one-shot command buffer, then wait. Used for uploads
    /// and acceleration-structure builds, never on the interactive path.
    void SubmitImmediate(const std::function<void(VkCommandBuffer)>& record) const;

    /// Called by the debug messenger. Public because the callback is a free
    /// function with C linkage; not part of the intended API.
    void NoteValidationMessage(bool isError, bool isWarning,
                               const char* message) const;

    /// Force the device-lost latch. Exists so tests can exercise the refusal
    /// and teardown paths without a real device fault -- an untested
    /// device-loss path is exactly what hdCodex shipped (lessons C3).
    void SimulateDeviceLossForTesting()
    {
        _deviceLost.store(true, std::memory_order_release);
    }

  private:
    void SelectPhysicalDevice(const VulkanContextOptions& options);
    void CreateDevice(const VulkanContextOptions& options);

    struct Impl;
    std::unique_ptr<Impl> _impl;

    VkInstance _instance = VK_NULL_HANDLE;
    VkPhysicalDevice _physicalDevice = VK_NULL_HANDLE;
    /// Where the persisted pipeline cache lives: HDCLAUDE_PIPELINE_CACHE if
    /// set, else a file in the system temporary directory.
    static std::filesystem::path PipelineCachePath();
    void CreatePipelineCache();
    void SavePipelineCache() const;

    VkDevice _device = VK_NULL_HANDLE;
    VkPipelineCache _pipelineCache = VK_NULL_HANDLE;
    std::filesystem::path _pipelineCachePath;
    VkQueue _queue = VK_NULL_HANDLE;
    std::uint32_t _queueFamily = 0;

    VulkanCapabilities _capabilities;
    VkPhysicalDeviceMemoryProperties _memoryProperties{};

    bool _validationEnabled = false;
    mutable std::atomic<bool> _deviceLost{false};
    mutable std::atomic<std::uint64_t> _validationErrors{0};
    mutable std::atomic<std::uint64_t> _validationWarnings{0};
};

}  // namespace hdclaude

#endif  // HDCLAUDE_GPU_VULKAN_CONTEXT_H
