#include "renderer_plugin.h"

#include "render_delegate.h"
#include "trace.h"

#include "hdclaude/gpu/vulkan_context.h"

#include "pxr/base/tf/getenv.h"
#include "pxr/imaging/hd/rendererPluginRegistry.h"

#include <chrono>

PXR_NAMESPACE_OPEN_SCOPE

TF_REGISTRY_FUNCTION(TfType)
{
    HdRendererPluginRegistry::Define<HdClaudeRendererPlugin>();
}

HdRenderDelegate* HdClaudeRendererPlugin::CreateRenderDelegate()
{
    return new HdClaudeRenderDelegate();
}

HdRenderDelegate* HdClaudeRendererPlugin::CreateRenderDelegate(
    const HdRenderSettingsMap& settingsMap)
{
    return new HdClaudeRenderDelegate(settingsMap);
}

void HdClaudeRendererPlugin::DeleteRenderDelegate(HdRenderDelegate* renderDelegate)
{
    delete renderDelegate;
}

bool HdClaudeRendererPlugin::IsSupported(
    const HdRendererCreateArgs& /*createArgs*/, std::string* reasonWhyNot) const
{
    // Answered by actually creating a context, because the question is not
    // "does a Vulkan device exist" but "can hdClaude trace on it". A plugin
    // that reports itself supported and then fails at first render is worse
    // than one that is honestly absent from the renderer menu.
    //
    // The context is built and dropped. Hydra calls this once per plugin at
    // discovery, so the cost is paid at startup, not per frame.
    // Answered once per process, because the answer cannot change within one.
    //
    // Hydra asks *three times* for a single `usdrecord` -- the plugin registry
    // resolves the renderer more than once -- and a context costs between four
    // and five seconds to create and destroy on an RTX 5060 Ti. Answering each
    // time made a render of a stage holding one camera and no geometry take 21
    // seconds, of which 14.4 were these probes and 3.8 the delegate's own
    // context. The device a probe would find is the same device every time, so
    // the second and third answers were already known.
    //
    // A function-local static is initialised once and is thread-safe by the
    // standard's own guarantee, which matters here because Hydra may resolve
    // plugins from more than one thread.
    static const hdclaude::VulkanSupport probe = [] {
        const auto start = std::chrono::steady_clock::now();
        hdclaude::VulkanContextOptions options;
        options.preferredDeviceName = TfGetenv("HDCLAUDE_DEVICE");
        hdclaude::VulkanSupport result;
        try {
            result = hdclaude::VulkanContext::Probe(options);
        } catch (const std::exception& error) {
            result.reason =
                std::string("no usable Vulkan device: ") + error.what();
        }
        HdClaudeTrace("support probe: %.0f ms, %s",
                      std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - start)
                          .count(),
                      result.supported ? result.deviceName.c_str()
                                       : result.reason.c_str());
        return result;
    }();

    if (!probe.supported && reasonWhyNot) {
        *reasonWhyNot = probe.reason;
    }
    return probe.supported;
}

PXR_NAMESPACE_CLOSE_SCOPE
