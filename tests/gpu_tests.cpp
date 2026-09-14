// GPU tests. These require a Vulkan 1.3 ray-query device.
//
// A machine without one is reported as skipped rather than failed, but a
// machine *with* one must pass: the point of these tests is that the resource
// rules in docs/architecture.md 6 are asserted against a real driver, not
// against a CPU-side shadow of what we believe the driver did (lessons R7).

#include "test_support.h"

#include "hdclaude/gpu/reconstruction.h"
#include "hdclaude/gpu/vulkan_context.h"
#include "hdclaude/gpu/vulkan_resources.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
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

    // Destruction runs the lost-device teardown path, and the gate is that it
    // issues no wait. Counted rather than trusted to crash: a simulated loss
    // leaves a healthy device underneath, so a wait issued anyway would
    // succeed and nothing would crash at all.
    const std::uint64_t waitsBefore = VulkanContext::DeviceWaitsIssuedForTesting();
    {
        VulkanContext lost(TestOptions());
        lost.SimulateDeviceLossForTesting();
    }
    CHECK_EQ(VulkanContext::DeviceWaitsIssuedForTesting(), waitsBefore);

    // Destruction of `context` below still runs that path a second time, with
    // every refusal above having happened first. If it issues per-object
    // destroys incorrectly this test crashes or trips validation.
}

void TestLiveTeardownWaitsForTheDevice()
{
    // The control. Without it the count above would pass just as well if the
    // counter were never incremented at all.
    std::uint64_t waitsBefore = 0;
    {
        VulkanContext context(TestOptions());
        waitsBefore = VulkanContext::DeviceWaitsIssuedForTesting();
    }
    CHECK_EQ(VulkanContext::DeviceWaitsIssuedForTesting(), waitsBefore + 1);
}

}  // namespace

/// What NGX says about this machine, and that asking cost the renderer nothing.
///
/// Two claims, and the second is the one that matters for every build that will
/// never run DLSS. The support query has to give a *correct* answer -- not a
/// hopeful one -- and it has to be safe to ask on any device, including one from
/// another vendor and a build with no SDK at all.
///
/// This cannot assert that DLSS is available: whether it is depends on the
/// machine, and a test that demanded it would fail on every non-NVIDIA
/// developer's box, which is exactly the coupling the renderer-neutral boundary
/// exists to prevent. What it asserts instead is that the answer is
/// *self-consistent* -- available implies a reason of none and unavailable
/// implies a reason given -- and it prints the answer, because on this machine
/// the answer is the finding.
void TestReconstructionSupportIsAnsweredHonestly(const VulkanContext& context)
{
    const bool compiledIn = NgxCompiledIn();
    std::printf("  DLSS SDK ....... %s\n",
                compiledIn ? "compiled in" : "not in this build");

    // Constructing the provider must be safe with or without the SDK, because
    // the context installs it during bootstrap before anything knows whether
    // DLSS will be used.
    const NgxRequirementProvider provider;
    if (!provider.Unavailable().empty()) {
        std::printf("  NGX extensions . unavailable: %s\n",
                    provider.Unavailable().c_str());
    } else {
        const std::vector<const char*> instance = provider.InstanceExtensions();
        const std::vector<const char*> device =
            provider.DeviceExtensions(context.PhysicalDevice());
        std::printf("  NGX extensions . %zu instance, %zu device\n",
                    instance.size(), device.size());
        for (const char* name : instance) {
            std::printf("                   instance: %s\n", name);
        }
        for (const char* name : device) {
            std::printf("                   device:   %s\n", name);
        }
    }

    // A provider with nothing to contribute must contribute nothing, whatever
    // the device. An optional backend that named an extension on a machine it
    // cannot run on would be forcing a device choice for a feature that will
    // never be used, which rule R-none-of-this exists to prevent
    // (docs/dlss-integration.md 2).
    if (!compiledIn) {
        CHECK(provider.InstanceExtensions().empty());
        CHECK(provider.DeviceExtensions(context.PhysicalDevice()).empty());
        CHECK(!provider.SupportsDevice(context.PhysicalDevice()));
    }

    const ReconstructionSupport support = QueryNgxSupport(context);
    std::printf("  DLSS ........... %s\n",
                support.available ? "available" : support.reason.c_str());
    if (support.available) {
        std::printf("                   super resolution %s, "
                    "ray reconstruction %s\n",
                    support.superResolution ? "yes" : "no",
                    support.rayReconstruction ? "yes" : "no");
        if (support.needsNewerDriver) {
            std::printf("                   needs driver %u.%u or newer\n",
                        support.minDriverMajor, support.minDriverMinor);
        }
    }

    // Self-consistency, which is the part that can fail on any machine.
    if (support.available) {
        CHECK(support.reason.empty());
    } else {
        CHECK(!support.reason.empty());
        CHECK(!support.superResolution);
        CHECK(!support.rayReconstruction);
    }
    // Without the SDK there is nothing to be available.
    if (!compiledIn) {
        CHECK(!support.available);
    }
}

// ---------------------------------------------------------------------------

/// Half-precision, because that is the format DLSS documents for colour and
/// motion and the test has to write and read real bytes rather than assume.
/// Round-to-nearest is unnecessary here -- the values are exact in half -- so
/// this is the plain bit rearrangement, and it handles zero and normals only,
/// which is all this test produces.
std::uint16_t ToHalf(float value)
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const std::uint32_t sign = (bits >> 16) & 0x8000u;
    const std::int32_t exponent = static_cast<std::int32_t>((bits >> 23) & 0xFFu) - 127;
    const std::uint32_t mantissa = bits & 0x7FFFFFu;
    if (exponent < -14) {
        return static_cast<std::uint16_t>(sign);
    }
    if (exponent > 15) {
        return static_cast<std::uint16_t>(sign | 0x7C00u);
    }
    return static_cast<std::uint16_t>(
        sign | (static_cast<std::uint32_t>(exponent + 15) << 10) | (mantissa >> 13));
}

float FromHalf(std::uint16_t half)
{
    const std::uint32_t sign = static_cast<std::uint32_t>(half & 0x8000u) << 16;
    const std::uint32_t exponent = (half >> 10) & 0x1Fu;
    const std::uint32_t mantissa = half & 0x3FFu;
    std::uint32_t bits = 0;
    if (exponent == 0) {
        bits = sign;  // zero, and subnormals rounded to it
    } else if (exponent == 31) {
        bits = sign | 0x7F800000u | (mantissa << 13);
    } else {
        bits = sign | ((exponent + 112u) << 23) | (mantissa << 13);
    }
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

/// Upload host bytes into an image and leave it in `layout`.
void UploadImage(const VulkanContext& context, VulkanAllocator& allocator,
                 VulkanImage& image, const void* data, VkDeviceSize size,
                 VkImageLayout layout)
{
    BufferDescription staging;
    staging.size = size;
    staging.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    staging.domain = BufferDomain::HostUpload;
    staging.debugName = "test.dlss.staging";
    VulkanBuffer upload(allocator, staging);
    upload.Write(data, size);

    context.SubmitImmediate([&](VkCommandBuffer command) {
        image.RecordBarrier(command, VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                            VK_PIPELINE_STAGE_2_COPY_BIT, 0,
                            VK_ACCESS_2_TRANSFER_WRITE_BIT);

        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent.width = image.Description().width;
        region.imageExtent.height = image.Description().height;
        region.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(command, upload.Handle(), image.Handle(),
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        image.RecordBarrier(command, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, layout,
                            VK_PIPELINE_STAGE_2_COPY_BIT,
                            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                            VK_ACCESS_2_TRANSFER_WRITE_BIT,
                            VK_ACCESS_2_MEMORY_READ_BIT);
    });
}

/// DLSS reconstructs a frame, and the frame it produces is the one it was
/// given.
///
/// This is the first time anything actually *runs* DLSS rather than asking
/// whether it could. It cannot assert image quality -- the model is closed and
/// its output is not a closed form -- so it asserts the things that are true of
/// any correct upscale and false of every way this plumbing can be wrong: the
/// output is the target size, it is finite everywhere, and the bright half of
/// the input is the bright half of the output. A backend that received the
/// images in the wrong layouts, or wrote nothing, or upscaled a buffer of
/// zeros, fails all three.
///
/// Skipped, loudly, on a machine or a build without DLSS. That is not a
/// weakening: `TestReconstructionSupportIsAnsweredHonestly` already gates the
/// answer, and this one gates the evaluation when there is one to gate.
void TestDlssReconstructsTheFrameItIsGiven(const VulkanContext& context,
                                           VulkanAllocator& allocator)
{
    std::string reason;
    std::unique_ptr<ReconstructionBackend> backend =
        CreateNgxBackend(context, &reason);
    if (!backend) {
        std::printf("  DLSS evaluate .. skipped: %s\n", reason.c_str());
        return;
    }
    std::printf("  backend ........ %s\n", backend->Name());

    constexpr std::uint32_t kOutputWidth = 512;
    constexpr std::uint32_t kOutputHeight = 512;

    // DLAA first, because its answer is one the specification fixes rather
    // than one the model chooses: anti-aliasing at native resolution renders
    // at the output extent, and a backend that answered otherwise would have
    // the caller rendering at a size DLAA does not mean.
    const ReconstructionSizing native = backend->QuerySizing(
        kOutputWidth, kOutputHeight, ReconstructionQuality::NativeResolution);
    CHECK(native.valid);
    CHECK_EQ(native.renderWidth, kOutputWidth);
    CHECK_EQ(native.renderHeight, kOutputHeight);

    const ReconstructionSizing sizing = backend->QuerySizing(
        kOutputWidth, kOutputHeight, ReconstructionQuality::Performance);
    CHECK(sizing.valid);
    CHECK(sizing.renderWidth > 0 && sizing.renderWidth < kOutputWidth);
    CHECK(sizing.renderHeight > 0 && sizing.renderHeight < kOutputHeight);
    std::printf("  DLSS sizing .... %ux%u -> %ux%u (performance)\n",
                sizing.renderWidth, sizing.renderHeight, kOutputWidth,
                kOutputHeight);

    const std::uint32_t renderWidth = sizing.renderWidth;
    const std::uint32_t renderHeight = sizing.renderHeight;
    const std::size_t renderPixels =
        static_cast<std::size_t>(renderWidth) * renderHeight;
    const std::size_t outputPixels =
        static_cast<std::size_t>(kOutputWidth) * kOutputHeight;

    const auto makeImage = [&](std::uint32_t width, std::uint32_t height,
                               VkFormat format, VkImageUsageFlags usage,
                               const char* name) {
        ImageDescription description;
        description.width = width;
        description.height = height;
        description.format = format;
        description.usage = usage;
        description.debugName = name;
        return VulkanImage(allocator, description);
    };

    constexpr VkImageUsageFlags kInputUsage =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VulkanImage color = makeImage(renderWidth, renderHeight,
                                  VK_FORMAT_R16G16B16A16_SFLOAT, kInputUsage,
                                  "test.dlss.color");
    VulkanImage depth = makeImage(renderWidth, renderHeight, VK_FORMAT_R32_SFLOAT,
                                  kInputUsage, "test.dlss.depth");
    VulkanImage motion = makeImage(renderWidth, renderHeight,
                                   VK_FORMAT_R16G16_SFLOAT, kInputUsage,
                                   "test.dlss.motion");
    // Storage, because NGX refuses a read-write resource whose image was not
    // created with it -- FAIL_RWFlagMissing, which is the one failure this
    // test would otherwise report as a black image. Transfer-destination too,
    // because DLSS clears the output itself with vkCmdClearColorImage, which
    // validation reported the first time this ran.
    VulkanImage output = makeImage(kOutputWidth, kOutputHeight,
                                   VK_FORMAT_R16G16B16A16_SFLOAT,
                                   VK_IMAGE_USAGE_STORAGE_BIT |
                                       VK_IMAGE_USAGE_SAMPLED_BIT |
                                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                       VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                   "test.dlss.output");

    // A frame with a left half at zero and a right half bright. Anything that
    // upscales it keeps that; anything that drops it does not.
    constexpr float kBright = 4.0f;
    std::vector<std::uint16_t> colorBytes(renderPixels * 4, 0);
    for (std::uint32_t y = 0; y < renderHeight; ++y) {
        for (std::uint32_t x = 0; x < renderWidth; ++x) {
            const std::size_t index = (static_cast<std::size_t>(y) * renderWidth + x) * 4;
            const float value = x >= renderWidth / 2 ? kBright : 0.0f;
            colorBytes[index + 0] = ToHalf(value);
            colorBytes[index + 1] = ToHalf(value);
            colorBytes[index + 2] = ToHalf(value);
            colorBytes[index + 3] = ToHalf(1.0f);
        }
    }
    // A flat surface halfway down the depth range, and nothing moving. The
    // guides have to be present and consistent even when they say nothing
    // happened: DLSS reads them whether or not they carry information.
    const std::vector<float> depthBytes(renderPixels, 0.5f);
    const std::vector<std::uint16_t> motionBytes(renderPixels * 2, 0);

    UploadImage(context, allocator, color, colorBytes.data(),
                colorBytes.size() * sizeof(std::uint16_t),
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    UploadImage(context, allocator, depth, depthBytes.data(),
                depthBytes.size() * sizeof(float),
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    UploadImage(context, allocator, motion, motionBytes.data(),
                motionBytes.size() * sizeof(std::uint16_t),
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    context.SubmitImmediate([&](VkCommandBuffer command) {
        output.RecordBarrier(command, VK_IMAGE_LAYOUT_UNDEFINED,
                             VK_IMAGE_LAYOUT_GENERAL,
                             VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0,
                             VK_ACCESS_2_MEMORY_WRITE_BIT);
    });

    ReconstructionResolution resolution;
    resolution.renderWidth = renderWidth;
    resolution.renderHeight = renderHeight;
    resolution.outputWidth = kOutputWidth;
    resolution.outputHeight = kOutputHeight;
    resolution.quality = ReconstructionQuality::Performance;

    // Building is recorded and must complete before evaluating is recorded,
    // which is why these are two submissions and not one.
    bool built = false;
    std::string buildReason;
    context.SubmitImmediate([&](VkCommandBuffer command) {
        built = backend->Resize(command, resolution, &buildReason);
    });
    if (!built) {
        std::printf("  DLSS build ..... %s\n", buildReason.c_str());
    }
    CHECK(built);
    if (!built) {
        return;
    }

    ReconstructionFrame frame;
    const auto describe = [](const VulkanImage& image) {
        ReconstructionTexture texture;
        texture.image = image.Handle();
        texture.view = image.View();
        texture.format = image.Description().format;
        texture.width = image.Description().width;
        texture.height = image.Description().height;
        return texture;
    };
    frame.color = describe(color);
    frame.depth = describe(depth);
    frame.motion = describe(motion);
    frame.output = describe(output);
    frame.reset = true;

    context.SubmitImmediate([&](VkCommandBuffer command) {
        backend->Evaluate(command, frame);
    });

    BufferDescription readbackDescription;
    readbackDescription.size = outputPixels * 4 * sizeof(std::uint16_t);
    readbackDescription.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    readbackDescription.domain = BufferDomain::HostReadback;
    readbackDescription.debugName = "test.dlss.readback";
    VulkanBuffer readback(allocator, readbackDescription);

    context.SubmitImmediate([&](VkCommandBuffer command) {
        output.RecordBarrier(command, VK_IMAGE_LAYOUT_GENERAL,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_2_COPY_BIT,
                             VK_ACCESS_2_MEMORY_WRITE_BIT,
                             VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent.width = kOutputWidth;
        region.imageExtent.height = kOutputHeight;
        region.imageExtent.depth = 1;
        vkCmdCopyImageToBuffer(command, output.Handle(),
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               readback.Handle(), 1, &region);
    });

    const auto* pixels = static_cast<const std::uint16_t*>(readback.MappedData());
    CHECK(pixels != nullptr);
    if (pixels == nullptr) {
        return;
    }

    double left = 0.0;
    double right = 0.0;
    std::size_t leftCount = 0;
    std::size_t rightCount = 0;
    bool finite = true;
    // The middle eighth is skipped on both sides. An upscaler is entitled to
    // blur across the edge, and asserting about pixels on it would be
    // asserting about the model's filter width rather than about the plumbing.
    const std::uint32_t margin = kOutputWidth / 8;
    for (std::uint32_t y = 0; y < kOutputHeight; ++y) {
        for (std::uint32_t x = 0; x < kOutputWidth; ++x) {
            const std::size_t index =
                (static_cast<std::size_t>(y) * kOutputWidth + x) * 4;
            const float value = FromHalf(pixels[index]);
            if (!std::isfinite(value)) {
                finite = false;
            }
            if (x + margin < kOutputWidth / 2) {
                left += value;
                ++leftCount;
            } else if (x > kOutputWidth / 2 + margin) {
                right += value;
                ++rightCount;
            }
        }
    }
    left /= double(leftCount);
    right /= double(rightCount);
    std::printf("  DLSS output .... left %.4f, right %.4f\n", left, right);

    CHECK(finite);
    CHECK(right > 0.5 * kBright);
    CHECK(left < 0.1 * kBright);

    // Rebuilding must be safe: an interactive caller resizes, and a backend
    // that leaked its feature or evaluated against a released one would fail
    // here or in validation rather than in a session weeks later.
    resolution.quality = ReconstructionQuality::NativeResolution;
    resolution.renderWidth = kOutputWidth;
    resolution.renderHeight = kOutputHeight;
    bool rebuilt = false;
    context.SubmitImmediate([&](VkCommandBuffer command) {
        rebuilt = backend->Resize(command, resolution, &buildReason);
    });
    CHECK(rebuilt);
    backend->ResetHistory();
}

int main()
{
    std::printf("hdClaudeGpuTests\n");

    // NGX is installed as a requirement provider *before* the context exists,
    // which is the whole reason the provider is a construction-time thing: NGX
    // names instance and device extensions it will not initialise without, and
    // they have to be enabled when the instance and device are created. The
    // provider is safe to install unconditionally -- with no SDK it names
    // nothing, and the context only enables extensions a device actually has,
    // so a machine that will never run DLSS is unaffected.
    const NgxRequirementProvider ngxProvider;

    std::unique_ptr<VulkanContext> context;
    try {
        VulkanContextOptions options = TestOptions();
        options.requirementProviders.push_back(&ngxProvider);
        context = std::make_unique<VulkanContext>(options);
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
                     "      Build it from source with 'compile.bat "
                     "dev-validation', or configure with\n"
                     "      -DHDCLAUDE_BUILD_VALIDATION_LAYERS=ON. "
                     "See docs/building.md.\n");
        return 1;
    }

    // Core validation alone is not the gate. Synchronisation validation is what
    // reports a buffer one kernel writes that the next cannot yet see, and it
    // can be off with the layer present, so a clean count would say nothing.
    if (!context->SynchronisationValidationEnabled()) {
        std::fprintf(stderr,
                     "FAIL: the validation layer is running without "
                     "synchronisation validation (it lacks "
                     "VK_EXT_layer_settings), so the validation gate would "
                     "pass without checking kernel hazards; see "
                     "docs/building.md\n");
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
        // Inside the allocator's scope, so the images this creates are
        // destroyed before the device is -- the ordering the comment
        // opening this block was written about.
        TestDlssReconstructsTheFrameItIsGiven(*context, allocator);
    }

    TestReconstructionSupportIsAnsweredHonestly(*context);

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
    TestLiveTeardownWaitsForTheDevice();

    return hdclaude_test::Summarize("hdClaudeGpuTests");
}
