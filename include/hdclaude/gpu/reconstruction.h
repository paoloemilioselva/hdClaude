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
// What is deliberately *not* here yet is the `ReconstructionBackend` interface
// itself -- Resize, Evaluate, ResetHistory. Those describe work that phases 10
// and 11 have not produced anything for, and declaring them now would leave
// three unimplemented virtuals standing in for a phase that is not done. They
// land with the native backend in phase 11, and this header grows into them.

#ifndef HDCLAUDE_GPU_RECONSTRUCTION_H
#define HDCLAUDE_GPU_RECONSTRUCTION_H

#include "hdclaude/gpu/vulkan_context.h"

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

}   // namespace hdclaude

#endif   // HDCLAUDE_GPU_RECONSTRUCTION_H
