// Reconstruction backend support, without NVIDIA in the interface.
//
// hdClaude reaches interactive rates by rendering a correct low-sample image
// and reconstructing it, and the boundary that happens across is specified in
// docs/dlss-integration.md. This header is the part of that boundary phase 12
// needs: whether a backend can run on this machine at all, and what it must be
// told before the Vulkan device exists.
//
// **No NVIDIA type appears here.** That is a project rule rather than taste
// (docs/dlss-integration.md 2): the delegate, the Hydra layer and every test
// must compile and behave identically whether or not the SDK was found, so the
// only thing that crosses this line is a plain description of what was
// discovered. The NGX headers are included by src/gpu/ngx_support.cpp and
// nowhere else.
//
// The `ReconstructionBackend` interface is here now that phase 10 has produced
// the inputs it takes. Vulkan types *do* cross this line -- images and command
// buffers are how any GPU reconstruction is expressed, and hiding them would
// buy nothing -- but NVIDIA types still do not.

#ifndef HDCLAUDE_GPU_RECONSTRUCTION_H
#define HDCLAUDE_GPU_RECONSTRUCTION_H

#include "hdclaude/gpu/vulkan_context.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace hdclaude {

/// What DLSS can do on this device, and -- when it cannot -- why.
///
/// `reason` is non-empty exactly when `available` is false, and is written for
/// somebody deciding what to change: a build without the SDK, a device from
/// another vendor, and a driver too old are three different answers and are
/// never collapsed into one.
struct ReconstructionSupport {
    /// NGX initialised and answered. False means nothing below is meaningful.
    bool available = false;

    /// DLSS Super Resolution and DLAA, which share one capability.
    bool superResolution = false;

    /// DLSS Ray Reconstruction, which is a separate model and a separate
    /// answer: a driver can offer one and not the other.
    bool rayReconstruction = false;

    /// The hardware could run it but this driver is too old. Distinct from
    /// plain unavailability, because it is the one case the user can fix.
    bool needsNewerDriver = false;
    unsigned int minDriverMajor = 0;
    unsigned int minDriverMinor = 0;

    /// Empty when `available`. Otherwise says what to change.
    std::string reason;
};

/// Whether this build has the DLSS SDK compiled into it at all.
///
/// Separate from `ReconstructionSupport::available`, and the distinction is the
/// point: a build without the SDK and a build with it on a device that cannot
/// run it are different facts, and a test that cannot tell them apart cannot
/// assert that the renderer is unchanged when DLSS is absent.
bool NgxCompiledIn();

/// NGX's Vulkan requirements, asked of the SDK before the instance exists.
///
/// NGX needs instance and device extensions of its own, and it will not
/// initialise without them. They therefore have to be requested during
/// bootstrap, which is why this is a `VulkanRequirementProvider` and not
/// something the backend asks for later.
///
/// Constructing one is cheap and safe on any machine: without the SDK, or when
/// the SDK declines to answer, the lists come back empty and `Unavailable()`
/// says why. Empty lists disable the backend and never fail context creation --
/// an optional backend must not be able to force a device choice on a system
/// that will not use it.
class NgxRequirementProvider final : public VulkanRequirementProvider {
  public:
    NgxRequirementProvider();

    std::vector<const char*> InstanceExtensions() const override;
    std::vector<const char*> DeviceExtensions(VkPhysicalDevice) const override;

    /// NGX runs on NVIDIA hardware only, which is decided from the candidate's
    /// vendor rather than by trying to initialise against it: this is asked
    /// during device *selection*, before there is a device to initialise with.
    bool SupportsDevice(VkPhysicalDevice) const override;

    /// Why the lists are empty, when they are. Empty when they are not.
    const std::string& Unavailable() const { return _unavailable; }

  private:
    /// Owned as strings and handed out as pointers, because NGX returns
    /// pointers into its own memory and this outlives the call that got them.
    std::vector<std::string> _instanceExtensions;
    std::vector<std::string> _deviceExtensions;
    mutable std::vector<const char*> _instanceView;
    mutable std::vector<const char*> _deviceView;
    std::string _unavailable;
};

/// Ask NGX what it can do on this context's device.
///
/// Initialises NGX, reads the capability parameters, and shuts it down again,
/// so this leaves nothing running and can be called before anything has decided
/// to use DLSS. It is the phase 12 gate's whole subject.
ReconstructionSupport QueryNgxSupport(const VulkanContext& context);

// ---------------------------------------------------------------------------

/// How hard a backend is asked to work, in the terms DLSS uses.
///
/// `NativeResolution` is DLAA: the render and output extents are equal and the
/// backend is anti-aliasing rather than upscaling. It is a separate value
/// rather than "upscaling by a factor of one" because DLSS treats it as its own
/// quality mode with its own model.
enum class ReconstructionQuality {
    NativeResolution,
    Quality,
    Balanced,
    Performance,
    UltraPerformance,
};

/// What a backend says it wants to be handed for a given output size.
///
/// The render extents are the backend's answer, not the caller's request: DLSS
/// chooses them per quality mode, and rendering at a size it did not ask for
/// either wastes work or gives its model less than it expects. `valid` is false
/// with a `reason` when it declines to answer.
struct ReconstructionSizing {
    std::uint32_t renderWidth = 0;
    std::uint32_t renderHeight = 0;
    /// The bounds a dynamic-resolution caller may move between without
    /// rebuilding. Equal to the optimal extents when the backend offers no
    /// range.
    std::uint32_t minWidth = 0;
    std::uint32_t minHeight = 0;
    std::uint32_t maxWidth = 0;
    std::uint32_t maxHeight = 0;
    bool valid = false;
    std::string reason;
};

/// The extents a backend is being built for.
struct ReconstructionResolution {
    std::uint32_t renderWidth = 0;
    std::uint32_t renderHeight = 0;
    std::uint32_t outputWidth = 0;
    std::uint32_t outputHeight = 0;
    ReconstructionQuality quality = ReconstructionQuality::NativeResolution;
};

/// One image a backend reads or writes. Owned by the caller throughout.
struct ReconstructionTexture {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    bool Valid() const
    {
        return image != VK_NULL_HANDLE && view != VK_NULL_HANDLE &&
               width != 0 && height != 0;
    }
};

/// Everything a backend needs about one frame.
///
/// The conventions here are hdClaude's, stated once so a backend can translate
/// rather than guess:
///
///   * `motion` is in **pixels of the render image**, current-to-previous, in
///     the same axis directions as the image's own rows and columns. Adding it
///     to a pixel's coordinate gives that surface's coordinate on the previous
///     frame.
///   * `jitter` is the frame's sub-pixel offset **in render pixels**, measured
///     from the pixel centre, in the same axis directions. It is the offset
///     `raygen` actually used, not a sequence a backend is expected to
///     reproduce.
///   * `depth` is normalised device depth in [0, 1], near at 0.
///   * `color` is linear HDR, pre-exposure applied, never display-encoded.
///
/// Row 0 of every one of these is the same row of the same image, and that is
/// the whole of what the axis directions have to agree on: a temporal filter
/// reprojects within one coordinate system and never needs to know which way is
/// up. hdClaude's row 0 is the bottom of the frame, following Hydra.
///
/// Image layouts are the caller's to arrange, because the caller owns the
/// images and the command buffer and knows what wrote them last: `color`,
/// `depth` and `motion` must be in `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL`
/// and `output` in `VK_IMAGE_LAYOUT_GENERAL` when `Evaluate` is recorded.
///
/// Usage flags are the caller's too, and DLSS asks for more of `output` than
/// it looks like it needs: `VK_IMAGE_USAGE_STORAGE_BIT`, because a read-write
/// resource without it is refused outright, and
/// `VK_IMAGE_USAGE_TRANSFER_DST_BIT`, because it clears the image itself
/// before writing. The inputs need `VK_IMAGE_USAGE_SAMPLED_BIT`.
struct ReconstructionFrame {
    ReconstructionTexture color;
    ReconstructionTexture depth;
    ReconstructionTexture motion;
    ReconstructionTexture output;

    float jitterX = 0.0f;
    float jitterY = 0.0f;

    /// The exposure already folded into `color`, so a backend can divide it out
    /// of history that was scaled differently. One means none was applied.
    float preExposure = 1.0f;

    /// This frame shares no history with the last: a scene edit, a resize, a
    /// mode switch, or a camera cut. Distinct from `ResetHistory`, which says
    /// the same thing outside a frame.
    bool reset = false;
};

/// A reconstruction backend: takes a correct, noisy, low-sample frame and the
/// guides that describe it, and produces the output image.
///
/// Selected at runtime (docs/dlss-integration.md 2). Nothing above this
/// interface knows which one it holds.
class ReconstructionBackend {
  public:
    virtual ~ReconstructionBackend() = default;

    /// For diagnostics and stats. Stable, short, and never parsed.
    virtual const char* Name() const = 0;

    /// What to render at, for a given output size and quality.
    virtual ReconstructionSizing QuerySizing(std::uint32_t outputWidth,
                                             std::uint32_t outputHeight,
                                             ReconstructionQuality) const = 0;

    /// Build, or rebuild, for these extents.
    ///
    /// Takes a command buffer because DLSS's feature creation is *recorded*
    /// rather than immediate -- it initialises device state and needs somewhere
    /// to put that work. The buffer must be recording, and must be submitted
    /// and complete before `Evaluate` is recorded against the feature. This is
    /// a documented departure from the sketch in docs/dlss-integration.md 2,
    /// which had `Resize` take extents alone.
    ///
    /// Returns false with a reason on failure, and leaves any previous state
    /// destroyed rather than half-replaced: a backend that failed to resize
    /// evaluates nothing rather than evaluating at the old size.
    virtual bool Resize(VkCommandBuffer command, const ReconstructionResolution&,
                        std::string* reason) = 0;

    /// Record the reconstruction. Does nothing if `Resize` has not succeeded.
    virtual void Evaluate(VkCommandBuffer command, const ReconstructionFrame&) = 0;

    /// Discard accumulated history before the next frame.
    virtual void ResetHistory() = 0;
};

/// The DLSS backend, or nothing.
///
/// Returns null with a reason when this build has no SDK, when the device
/// cannot run DLSS, or when NGX declines to initialise -- all three of which
/// are ordinary answers rather than errors, because an optional backend that
/// threw would make the renderer's construction depend on the machine.
///
/// The returned backend holds NGX initialised for its own lifetime, which is
/// why this is separate from `QueryNgxSupport`: that one initialises, asks, and
/// shuts down again, and is safe to call before anything has decided to use
/// DLSS. Only one may exist at a time, because NGX is initialised per device
/// and not per object.
std::unique_ptr<ReconstructionBackend> CreateNgxBackend(const VulkanContext& context,
                                                        std::string* reason);

}   // namespace hdclaude

#endif   // HDCLAUDE_GPU_RECONSTRUCTION_H
