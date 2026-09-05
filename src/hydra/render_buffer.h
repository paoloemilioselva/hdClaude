#pragma once

#include "pxr/imaging/hd/renderBuffer.h"

#include <atomic>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

/// A CPU-backed AOV.
///
/// The renderer owns a private Vulkan device and no public OpenUSD API hands a
/// VkImage across devices, so the AOV is a host round trip. That decision, and
/// the external-memory interop that would remove it, are recorded in
/// docs/architecture.md 8.
class HdClaudeRenderBuffer final : public HdRenderBuffer {
  public:
    explicit HdClaudeRenderBuffer(const SdfPath& id);
    ~HdClaudeRenderBuffer() override;

    bool Allocate(const GfVec3i& dimensions, HdFormat format,
                  bool multiSampled) override;

    unsigned int GetWidth() const override { return _width; }
    unsigned int GetHeight() const override { return _height; }
    unsigned int GetDepth() const override { return 1; }
    HdFormat GetFormat() const override { return _format; }
    bool IsMultiSampled() const override { return false; }

    void* Map() override
    {
        ++_mappers;
        return _data.data();
    }
    void Unmap() override { --_mappers; }
    bool IsMapped() const override { return _mappers.load() != 0; }

    void Resolve() override {}
    bool IsConverged() const override { return _converged; }
    void SetConverged(bool converged) { _converged = converged; }

    /// Fill from the renderer's linear RGBA float output.
    void Write(const std::vector<float>& linearRgba);

    void Clear(const float* value);

  protected:
    void _Deallocate() override;

  private:
    unsigned int _width = 0;
    unsigned int _height = 0;
    HdFormat _format = HdFormatInvalid;
    std::vector<char> _data;
    std::atomic<int> _mappers{0};
    bool _converged = false;
};

PXR_NAMESPACE_CLOSE_SCOPE
