// GPU tests. These require a Vulkan 1.3 ray-query device.
//
// A machine without one is reported as skipped rather than failed, but a
// machine *with* one must pass: the point of these tests is that the resource
// rules in docs/architecture.md 6 are asserted against a real driver, not
// against a CPU-side shadow of what we believe the driver did (lessons R7).

#include "test_support.h"

#include "hdclaude/gpu/vulkan_context.h"
#include "hdclaude/gpu/vulkan_resources.h"

#include <cstdio>
#include <vector>

using namespace hdclaude;

namespace {

VulkanContextOptions TestOptions()
{
    VulkanContextOptions options;
    // Validation on for every GPU test run. R8: the count is a gate, checked at
    // the end of main, not a console message someone might read.
    options.enableValidation = true;
    return options;
}

// ---------------------------------------------------------------------------

void TestContextSelectsACapableDevice(const VulkanContext& context)
{
    const VulkanCapabilities& capabilities = context.Capabilities();

    std::printf("  device ......... %s\n", capabilities.deviceName.c_str());
    std::printf("  driver ......... %s\n", capabilities.driverVersion.c_str());
    std::printf("  device memory .. %llu MiB\n",
                static_cast<unsigned long long>(capabilities.deviceLocalMemory >> 20));
    std::printf("  subgroup ....... %u\n", capabilities.subgroupSize);
    std::printf("  ray query ...... %s\n", capabilities.rayQuery ? "yes" : "no");
    std::printf("  accel struct ... %s\n",
                capabilities.accelerationStructure ? "yes" : "no");
    std::printf("  SER (reorder) .. %s\n",
                capabilities.invocationReorder ? "yes" : "no");
    std::printf("  external memory  %s\n", capabilities.externalMemory ? "yes" : "no");
    std::printf("  validation ..... %s\n",
                context.ValidationEnabled() ? "enabled" : "UNAVAILABLE");

    // Ray query and acceleration structures are how the renderer traverses.
    // Device selection must not return a device lacking them.
    CHECK(capabilities.rayQuery);
    CHECK(capabilities.accelerationStructure);
    CHECK(capabilities.subgroupSize > 0);
    CHECK(capabilities.deviceLocalMemory > 0);
    CHECK(!capabilities.deviceName.empty());

    CHECK(context.Device() != VK_NULL_HANDLE);
    CHECK(context.Queue() != VK_NULL_HANDLE);
    CHECK(!context.IsDeviceLost());
}

void TestBufferGenerationsAreUniqueAndMonotonic(VulkanAllocator& allocator)
{
    // Rule 2: identity, not shape. Two buffers with identical descriptions must
    // still be distinguishable, or a descriptor that names one could be
    // considered current while pointing at the other.
    BufferDescription description;
    description.size = 4096;
    description.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    description.debugName = "test.identity";

    VulkanBuffer a(allocator, description);
    VulkanBuffer b(allocator, description);

    CHECK(a.Valid());
    CHECK(b.Valid());
    CHECK(a.Generation() != b.Generation());
    CHECK(b.Generation() > a.Generation());

    // Device-local buffers always get an address; the kernels reach queues and
    // path state through addresses rather than a descriptor per buffer.
    CHECK(a.DeviceAddress() != 0);

    const ResourceGeneration recycled = a.Generation();
    a.Reset();
    CHECK(!a.Valid());
    CHECK_EQ(a.Generation(), ResourceGeneration(0));

    // The freed buffer's memory may well be handed straight back. Its identity
    // must not be.
    VulkanBuffer c(allocator, description);
    CHECK(c.Generation() != recycled);
}

void TestDescriptorInvalidationFollowsIdentity(VulkanAllocator& allocator)
{
    // This is the hdCodex A1 scenario as a test. The old code invalidated a
    // descriptor set by comparing render extents; here the extents (and the
    // whole description) are identical while the buffer is replaced, and the
    // binding state must still report that a rewrite is needed.
    BufferDescription description;
    description.size = 65536;
    description.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    description.debugName = "test.invalidation";

    VulkanBuffer output(allocator, description);

    DescriptorBindingState binding;
    CHECK(binding.NeedsUpdate({output.Generation()}));

    binding.Record({output.Generation()});
    CHECK(!binding.NeedsUpdate({output.Generation()}));

    // Recreate at exactly the same size and usage, as an extent change that
    // happens to round to the same allocation would.
    output = VulkanBuffer(allocator, description);
    CHECK(binding.NeedsUpdate({output.Generation()}));

    binding.Record({output.Generation()});
    CHECK(!binding.NeedsUpdate({output.Generation()}));
    binding.Invalidate();
    CHECK(binding.NeedsUpdate({output.Generation()}));
}

void TestHostBufferRoundTrip(VulkanAllocator& allocator)
{
    BufferDescription description;
    description.size = 1024;
    description.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    description.domain = BufferDomain::HostUpload;
    description.debugName = "test.upload";

    VulkanBuffer upload(allocator, description);
    CHECK(upload.MappedData() != nullptr);

    std::vector<std::uint32_t> source(256);
    for (std::uint32_t i = 0; i < source.size(); ++i) {
        source[i] = i * 2654435761u;
    }
    upload.Write(source.data(), source.size() * sizeof(std::uint32_t));

    const auto* mapped = static_cast<const std::uint32_t*>(upload.MappedData());
    bool identical = true;
    for (std::size_t i = 0; i < source.size(); ++i) {
        identical = identical && mapped[i] == source[i];
    }
    CHECK(identical);
}

void TestDeviceLocalCopyIsObservedOnTheDevice(const VulkanContext& context,
                                              VulkanAllocator& allocator)
{
    // Round-trips real bytes through device-local memory, so this asserts what
    // the GPU actually holds rather than what a CPU shadow claims (lessons R7).
    constexpr VkDeviceSize kSize = 4096;
    std::vector<std::uint32_t> source(kSize / sizeof(std::uint32_t));
    for (std::uint32_t i = 0; i < source.size(); ++i) {
        source[i] = 0xA5A50000u + i;
    }

    BufferDescription uploadDescription;
    uploadDescription.size = kSize;
    uploadDescription.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    uploadDescription.domain = BufferDomain::HostUpload;
    uploadDescription.debugName = "test.roundtrip.upload";
    VulkanBuffer upload(allocator, uploadDescription);
    upload.Write(source.data(), kSize);

    BufferDescription deviceDescription;
    deviceDescription.size = kSize;
    deviceDescription.usage =
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    deviceDescription.domain = BufferDomain::DeviceLocal;
    deviceDescription.debugName = "test.roundtrip.device";
    VulkanBuffer device(allocator, deviceDescription);

    BufferDescription readbackDescription;
    readbackDescription.size = kSize;
    readbackDescription.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    readbackDescription.domain = BufferDomain::HostReadback;
    readbackDescription.debugName = "test.roundtrip.readback";
    VulkanBuffer readback(allocator, readbackDescription);

    context.SubmitImmediate([&](VkCommandBuffer command) {
        VkBufferCopy region{};
        region.size = kSize;
        vkCmdCopyBuffer(command, upload.Handle(), device.Handle(), 1, &region);

        VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(command, &dependency);

        vkCmdCopyBuffer(command, device.Handle(), readback.Handle(), 1, &region);
    });

    const auto* result = static_cast<const std::uint32_t*>(readback.MappedData());
    CHECK(result != nullptr);
    bool identical = result != nullptr;
    for (std::size_t i = 0; identical && i < source.size(); ++i) {
        identical = result[i] == source[i];
    }
    CHECK(identical);
}

void TestImageCreationChecksFormatSupport(VulkanAllocator& allocator)
{
    ImageDescription description;
    description.width = 64;
    description.height = 64;
    description.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    description.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    description.debugName = "test.noisyColor";

    CHECK(allocator.SupportsImageFormat(description.format, description.usage));

    VulkanImage image(allocator, description);
    CHECK(image.Valid());
    CHECK(image.View() != VK_NULL_HANDLE);
    CHECK(image.Generation() != 0);

    // An unsupported usage must be rejected at creation, naming the resource,
    // rather than surfacing as a validation message at first use.
    ImageDescription unsupported = description;
    unsupported.format = VK_FORMAT_R8G8B8_UNORM;  // rarely storage-capable
    unsupported.usage = VK_IMAGE_USAGE_STORAGE_BIT;
    if (!allocator.SupportsImageFormat(unsupported.format, unsupported.usage)) {
        bool threw = false;
        try {
            VulkanImage rejected(allocator, unsupported);
        } catch (const VulkanError& error) {
            threw = error.Result() == VK_ERROR_FORMAT_NOT_SUPPORTED;
        }
        CHECK(threw);
    }
}

void TestMoveSemanticsTransferOwnership(VulkanAllocator& allocator)
{
    BufferDescription description;
    description.size = 2048;
    description.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    description.debugName = "test.move";

    VulkanBuffer original(allocator, description);
    const VkBuffer handle = original.Handle();
    const ResourceGeneration generation = original.Generation();

    VulkanBuffer moved(std::move(original));
    CHECK(!original.Valid());
    CHECK_EQ(moved.Handle(), handle);
    CHECK_EQ(moved.Generation(), generation);

    // Move-assignment must release what the destination already owned, not leak
    // it. Validation would report the leak at device destruction if it did not.
    VulkanBuffer destination(allocator, description);
    destination = std::move(moved);
    CHECK_EQ(destination.Handle(), handle);
    CHECK(!moved.Valid());
}

void TestDeviceLossLatchRefusesWork()
{
    // hdCodex had no latch, so every call issued after a fault ran against a
    // dead device and every diagnostic gathered afterwards was contaminated
    // (lessons C3). A separate context is used because the latch is sticky.
    VulkanContext context(TestOptions());
    CHECK(!context.IsDeviceLost());

    bool liveAccepted = true;
    try {
        context.RequireLive("before");
    } catch (const VulkanError&) {
        liveAccepted = false;
    }
    CHECK(liveAccepted);

    context.SimulateDeviceLossForTesting();
    CHECK(context.IsDeviceLost());

    bool refused = false;
    try {
        context.RequireLive("after");
    } catch (const VulkanError& error) {
        refused = error.IsDeviceLost();
    }
    CHECK(refused);

    // Entry points that own GPU work must refuse rather than submit.
    bool submitRefused = false;
    try {
        context.SubmitImmediate([](VkCommandBuffer) {});
    } catch (const VulkanError& error) {
        submitRefused = error.IsDeviceLost();
    }
    CHECK(submitRefused);

    // WaitIdle must become a no-op rather than an unchecked call on a dead
    // device -- the specific hdCodex defect that hid the original fault.
    context.WaitIdle();
    CHECK(context.IsDeviceLost());

    // Destruction now runs the lost-device teardown path. If it issues waits or
    // per-object destroys against the latched device, this test crashes or
    // trips validation, which is the point.
}

}  // namespace

int main()
{
    std::printf("hdClaudeGpuTests\n");

    std::unique_ptr<VulkanContext> context;
    try {
        context = std::make_unique<VulkanContext>(TestOptions());
    } catch (const VulkanError& error) {
        std::printf("SKIP: no usable Vulkan ray-query device (%s)\n", error.what());
        return 0;
    }

    // R8 in a new disguise: if validation was requested but the layer is
    // absent, the error-count gate below passes vacuously and the run *looks*
    // clean. Refusing here is the difference between a gate and a decoration.
    if (!context->ValidationEnabled()) {
        std::fprintf(stderr,
                     "FAIL: Vulkan validation was requested but the Khronos "
                     "validation layer is not available, so the validation "
                     "gate below would pass without checking anything.\n"
                     "      Install the Vulkan SDK, or set HDCLAUDE_VULKAN_SDK "
                     "to one. See docs/building.md.\n");
        return 1;
    }

    TestContextSelectsACapableDevice(*context);

    // Scoped so the allocator and every resource it owns are destroyed before
    // the device is. Validation reports the reverse order as leaked device
    // memory at vkDestroyDevice, which is how this ordering bug was caught.
    {
        VulkanAllocator allocator(*context);
        std::printf(
            "  device bytes ... %llu used, %llu available\n",
            static_cast<unsigned long long>(allocator.DeviceLocalBytesUsed()),
            static_cast<unsigned long long>(allocator.DeviceLocalBytesAvailable()));

        TestBufferGenerationsAreUniqueAndMonotonic(allocator);
        TestDescriptorInvalidationFollowsIdentity(allocator);
        TestHostBufferRoundTrip(allocator);
        TestDeviceLocalCopyIsObservedOnTheDevice(*context, allocator);
        TestImageCreationChecksFormatSupport(allocator);
        TestMoveSemanticsTransferOwnership(allocator);
    }

    // R8: validation errors fail the run. Checked before the device-loss test,
    // which deliberately tears a context down in the lost state.
    const std::uint64_t errors = context->ValidationErrorCount();
    if (errors != 0) {
        std::fprintf(stderr,
                     "FAIL: %llu Vulkan validation error(s). Last: %s\n",
                     static_cast<unsigned long long>(errors),
                     context->LastValidationError().c_str());
    }
    CHECK_EQ(errors, std::uint64_t(0));
    if (context->ValidationEnabled()) {
        std::printf("  validation ..... %llu errors, %llu warnings\n",
                    static_cast<unsigned long long>(errors),
                    static_cast<unsigned long long>(context->ValidationWarningCount()));
    }

    context.reset();

    TestDeviceLossLatchRefusesWork();

    return hdclaude_test::Summarize("hdClaudeGpuTests");
}
