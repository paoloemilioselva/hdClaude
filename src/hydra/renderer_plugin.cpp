#include "renderer_plugin.h"

#include "render_delegate.h"

#include "hdclaude/gpu/vulkan_context.h"

#include "pxr/imaging/hd/rendererPluginRegistry.h"

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
    try {
        hdclaude::VulkanContext context;
        if (!context.Capabilities().rayQuery ||
            !context.Capabilities().accelerationStructure) {
            if (reasonWhyNot) {
                *reasonWhyNot =
                    "the selected Vulkan device (" +
                    context.Capabilities().deviceName +
                    ") does not support VK_KHR_ray_query with hardware "
                    "acceleration structures";
            }
            return false;
        }
        return true;
    } catch (const std::exception& error) {
        if (reasonWhyNot) {
            *reasonWhyNot =
                std::string("no usable Vulkan device: ") + error.what();
        }
        return false;
    }
}

PXR_NAMESPACE_CLOSE_SCOPE
