#pragma once

#include "pxr/imaging/hd/renderBuffer.h"

#include <atomic>
#include <cstdint>
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
    ///
    /// Returns false, writing nothing, for a format it cannot fill -- the
    /// buffer then still holds no frame -- so the caller can say so.
    bool Write(const std::vector<float>& linearRgba);

    /// One value per pixel, for a single-channel AOV such as depth.
    ///
    /// Separate from `Write` rather than a stride on it, because the two differ
    /// in what they are given and not merely in how they walk it: expanding a
    /// depth buffer to RGBA to reuse the other path would allocate four times
    /// the data to throw three quarters of it away.
    ///
    /// Returns false, writing nothing, for a format it cannot fill, so the
    /// caller can say so. Depth-stencil (Float32UInt8) takes the value as its
    /// depth and a zero stencil.
    bool WriteScalar(const std::vector<float>& values);

    /// A per-pixel vector of data -- a normal, an albedo -- `stride` floats a
    /// pixel starting `offset` into each, into however many channels the
    /// buffer has.
    ///
    /// No transfer function, unlike `Write`: these are measurements handed to
    /// something that computes with them, and an sRGB curve applied to an
    /// albedo is a wrong albedo. A format that cannot hold a signed value
    /// exactly is left at its clear value rather than filled with a quantised
    /// one.
    void WriteData(const std::vector<float>& values, std::size_t stride,
                   std::size_t offset);

    /// Per-pixel integers -- an id -- `stride` a pixel starting `offset` into
    /// each, into an Int32 buffer's first channel. Any other format is left at
    /// its clear value: an id converted to a float is a different id past 2^24.
    void WriteIds(const std::vector<std::int32_t>& values, std::size_t stride,
                  std::size_t offset);

    void Clear(const float* value);

    /// Whether nothing has been written since the buffer was allocated.
    ///
    /// A host can bind a new buffer, or reallocate one, after the frame it
    /// shows has finished. That buffer still has to receive the frame, or
    /// it reads as its allocation: zeros, which in a depth AOV is the near
    /// plane -- a surface at the eye.
    bool HoldsNoFrame() const { return !_written; }

  protected:
    void _Deallocate() override;

  private:
    unsigned int _width = 0;
    unsigned int _height = 0;
    HdFormat _format = HdFormatInvalid;
    std::vector<char> _data;
    std::atomic<int> _mappers{0};
    bool _converged = false;
    bool _written = false;
};

PXR_NAMESPACE_CLOSE_SCOPE
