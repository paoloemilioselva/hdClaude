#pragma once

#include "api.h"

#include "pxr/imaging/hd/rendererPlugin.h"

PXR_NAMESPACE_OPEN_SCOPE

/// Entry point Hydra discovers through plugInfo.json.
class HDCLAUDE_API HdClaudeRendererPlugin final : public HdRendererPlugin {
  public:
    HdClaudeRendererPlugin() = default;
    ~HdClaudeRendererPlugin() override = default;

    HdRenderDelegate* CreateRenderDelegate() override;
    HdRenderDelegate* CreateRenderDelegate(
        const HdRenderSettingsMap& settingsMap) override;
    void DeleteRenderDelegate(HdRenderDelegate* renderDelegate) override;

    bool IsSupported(const HdRendererCreateArgs& createArgs,
                     std::string* reasonWhyNot = nullptr) const override;
};

PXR_NAMESPACE_CLOSE_SCOPE
