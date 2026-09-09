#pragma once

#include "api.h"
#include "material_compiler.h"
#include "render_param.h"
#include "scene_store.h"
#include "texture_loader.h"

#include "hdclaude/gpu/path_tracer.h"
#include "hdclaude/gpu/vulkan_context.h"
#include "stage_stats.h"

#include "hdclaude/gpu/vulkan_resources.h"

#include "pxr/imaging/hd/renderDelegate.h"

#include <atomic>
#include <memory>
#include <string>

PXR_NAMESPACE_OPEN_SCOPE

/// Owns the renderer for the lifetime of one Hydra render index.
///
/// The Vulkan device, the path tracer, the MaterialX compiler and the scene
/// store all live here. Construction is *fallible* and does not throw out of
/// Hydra: a delegate whose device failed to come up still answers every query,
/// creates every prim, and renders nothing, reporting why through render stats.
/// Throwing instead would take usdview down at renderer-switch time.
class HDCLAUDE_API HdClaudeRenderDelegate final : public HdRenderDelegate {
  public:
    HdClaudeRenderDelegate();
    explicit HdClaudeRenderDelegate(const HdRenderSettingsMap& settingsMap);
    ~HdClaudeRenderDelegate() override;

    HdClaudeRenderDelegate(const HdClaudeRenderDelegate&) = delete;
    HdClaudeRenderDelegate& operator=(const HdClaudeRenderDelegate&) = delete;

    const TfTokenVector& GetSupportedRprimTypes() const override;
    const TfTokenVector& GetSupportedSprimTypes() const override;
    const TfTokenVector& GetSupportedBprimTypes() const override;

    TfTokenVector GetShaderSourceTypes() const override;
    TfTokenVector GetMaterialRenderContexts() const override;
    TfToken GetMaterialBindingPurpose() const override;

    HdRenderParam* GetRenderParam() const override;
    HdResourceRegistrySharedPtr GetResourceRegistry() const override;

    HdRenderPassSharedPtr CreateRenderPass(
        HdRenderIndex* index, const HdRprimCollection& collection) override;

    HdInstancer* CreateInstancer(HdSceneDelegate* delegate,
                                 const SdfPath& id) override;
    void DestroyInstancer(HdInstancer* instancer) override;

    HdRprim* CreateRprim(const TfToken& typeId, const SdfPath& rprimId) override;
    void DestroyRprim(HdRprim* rprim) override;

    HdSprim* CreateSprim(const TfToken& typeId, const SdfPath& sprimId) override;
    HdSprim* CreateFallbackSprim(const TfToken& typeId) override;
    void DestroySprim(HdSprim* sprim) override;

    HdBprim* CreateBprim(const TfToken& typeId, const SdfPath& bprimId) override;
    HdBprim* CreateFallbackBprim(const TfToken& typeId) override;
    void DestroyBprim(HdBprim* bprim) override;

    void CommitResources(HdChangeTracker* tracker) override;

    HdAovDescriptor GetDefaultAovDescriptor(const TfToken& name) const override;
    HdRenderSettingDescriptorList GetRenderSettingDescriptors() const override;
    VtDictionary GetRenderStats() const override;

    // --- Used by the render pass ---------------------------------------------

    hdclaude::PathTracer* PathTracer() const { return _pathTracer.get(); }
    HdClaudeSceneStore* SceneStore() const { return _store.get(); }
    HdClaudeTexturePool* TexturePool() const { return _texturePool.get(); }

    /// Why the renderer is not usable, or empty if it is. The render pass shows
    /// this rather than rendering a black frame with no explanation.
    const std::string& InitializationError() const { return _initializationError; }

    /// Record what a frame cost, in time and in device memory.
    ///
    /// The memory is sampled here rather than at teardown because that is the
    /// only moment the renderer is holding everything a frame needs at once --
    /// path state, acceleration structures, textures and film. By the
    /// destructor the interesting part has already been released.
    void RecordFrameTiming(double milliseconds, std::uint32_t samples);

    /// Where the render pass records what ingestion and publication cost. The
    /// adapters reach the same object through the render param.
    HdClaudeStageStats& StageStats() { return _stageStats; }

    /// Device memory as the frame log needs it: the high-water mark, what
    /// the driver says is left, and how much of what was asked for as
    /// device-local was placed elsewhere.
    std::uint64_t PeakDeviceBytes() const
    {
        return _peakDeviceBytes.load(std::memory_order_relaxed);
    }
    std::uint64_t DeviceBytesAvailable() const;
    std::uint64_t DeviceBytesSpilled() const;

  private:
    void Initialize(const HdRenderSettingsMap& settingsMap);

    std::unique_ptr<HdClaudeSceneStore> _store;
    std::unique_ptr<HdClaudeTexturePool> _texturePool;
    std::unique_ptr<HdClaudeMaterialCompiler> _materialCompiler;
    std::unique_ptr<HdClaudeRenderParam> _renderParam;
    HdResourceRegistrySharedPtr _resourceRegistry;

    // Declaration order is destruction order reversed: the path tracer holds
    // buffers from the allocator, and the allocator must outlive them, so the
    // tracer is declared last and destroyed first. Getting this wrong leaked
    // device memory at vkDestroyDevice once already (docs/implementation-notes.md).
    std::unique_ptr<hdclaude::VulkanContext> _context;
    std::unique_ptr<hdclaude::VulkanAllocator> _allocator;

    /// The high-water mark of device-local memory, across every frame.
    ///
    /// A peak rather than a final reading, because a renderer that allocated
    /// and released is one that could fail on a smaller card, and a number
    /// taken after the release would not say so. In practice the two are close
    /// here -- path state and structures are allocated once and kept -- which
    /// is worth knowing rather than assuming.
    std::atomic<std::uint64_t> _peakDeviceBytes{0};

    /// What each stage of a render cost. Filled by the adapters through the
    /// render param, read by GetRenderStats and by the memory report.
    HdClaudeStageStats _stageStats;
    std::unique_ptr<hdclaude::PathTracer> _pathTracer;

    std::string _initializationError;
    std::atomic<double> _lastFrameMilliseconds{0.0};
    std::atomic<std::uint32_t> _lastFrameSamples{0};
};

PXR_NAMESPACE_CLOSE_SCOPE
