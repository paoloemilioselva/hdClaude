#include "render_buffer.h"

#include "pxr/base/gf/vec3i.h"
#include "pxr/base/tf/diagnostic.h"

#include <algorithm>
#include <cmath>
#include <cstring>

PXR_NAMESPACE_OPEN_SCOPE

HdClaudeRenderBuffer::HdClaudeRenderBuffer(const SdfPath& id)
    : HdRenderBuffer(id)
{
}

HdClaudeRenderBuffer::~HdClaudeRenderBuffer() = default;

bool HdClaudeRenderBuffer::Allocate(const GfVec3i& dimensions, HdFormat format,
                                    bool /*multiSampled*/)
{
    _Deallocate();

    if (dimensions[2] != 1) {
        TF_WARN("hdClaude: <%s> asked for a %d-deep render buffer; only 2D "
                "buffers are supported",
                GetId().GetText(), dimensions[2]);
        return false;
    }
    if (format == HdFormatInvalid) {
        return false;
    }

    _width = static_cast<unsigned int>(std::max(0, dimensions[0]));
    _height = static_cast<unsigned int>(std::max(0, dimensions[1]));
    _format = format;
    _data.assign(static_cast<std::size_t>(_width) * _height *
                     HdDataSizeOfFormat(format),
                 0);
    return true;
}

void HdClaudeRenderBuffer::_Deallocate()
{
    _width = 0;
    _height = 0;
    _format = HdFormatInvalid;
    _data.clear();
    _data.shrink_to_fit();
    _converged = false;
    _mappers.store(0);
}

void HdClaudeRenderBuffer::Clear(const float* value)
{
    if (_data.empty() || value == nullptr) {
        return;
    }
    const size_t componentCount = HdGetComponentCount(_format);
    const HdFormat component = HdGetComponentFormat(_format);
    const size_t pixelSize = HdDataSizeOfFormat(_format);
    const size_t pixels = static_cast<size_t>(_width) * _height;

    // One pixel is composed, then replicated. Composing per pixel would be the
    // same result at several times the cost on a large AOV.
    std::vector<char> pixel(pixelSize, 0);
    for (size_t c = 0; c < componentCount; ++c) {
        switch (component) {
            case HdFormatFloat32:
                std::memcpy(pixel.data() + c * sizeof(float), &value[c],
                            sizeof(float));
                break;
            case HdFormatInt32: {
                const int32_t asInt = static_cast<int32_t>(value[c]);
                std::memcpy(pixel.data() + c * sizeof(int32_t), &asInt,
                            sizeof(int32_t));
                break;
            }
            case HdFormatUNorm8: {
                const uint8_t asByte = static_cast<uint8_t>(
                    std::clamp(value[c], 0.0f, 1.0f) * 255.0f + 0.5f);
                pixel[c] = static_cast<char>(asByte);
                break;
            }
            default:
                // A format hdClaude does not compose is left at zero rather
                // than filled with reinterpreted bytes.
                return;
        }
    }
    for (size_t i = 0; i < pixels; ++i) {
        std::memcpy(_data.data() + i * pixelSize, pixel.data(), pixelSize);
    }
}

void HdClaudeRenderBuffer::WriteScalar(const std::vector<float>& values)
{
    const size_t pixels = static_cast<size_t>(_width) * _height;
    if (pixels == 0 || values.size() < pixels) {
        return;
    }
    const HdFormat component = HdGetComponentFormat(_format);
    const size_t pixelSize = HdDataSizeOfFormat(_format);

    for (size_t i = 0; i < pixels; ++i) {
        char* destination = _data.data() + i * pixelSize;
        const float value = values[i];
        switch (component) {
            case HdFormatFloat32:
                std::memcpy(destination, &value, sizeof(float));
                break;
            case HdFormatFloat16: {
                const GfHalf half(value);
                std::memcpy(destination, &half, sizeof(GfHalf));
                break;
            }
            case HdFormatUNorm8: {
                const auto quantised = static_cast<unsigned char>(
                    std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
                std::memcpy(destination, &quantised, sizeof(unsigned char));
                break;
            }
            default:
                // A format this does not know how to fill is left at its clear
                // value rather than filled with something plausible.
                return;
        }
    }
}

void HdClaudeRenderBuffer::Write(const std::vector<float>& linearRgba)
{
    const size_t pixels = static_cast<size_t>(_width) * _height;
    if (pixels == 0 || linearRgba.size() < pixels * 4) {
        return;
    }
    const size_t componentCount =
        std::min<size_t>(4, HdGetComponentCount(_format));
    const HdFormat component = HdGetComponentFormat(_format);
    const size_t pixelSize = HdDataSizeOfFormat(_format);

    for (size_t i = 0; i < pixels; ++i) {
        char* destination = _data.data() + i * pixelSize;
        for (size_t c = 0; c < componentCount; ++c) {
            const float value = linearRgba[i * 4 + c];
            switch (component) {
                case HdFormatFloat32:
                    std::memcpy(destination + c * sizeof(float), &value,
                                sizeof(float));
                    break;
                case HdFormatUNorm8: {
                    // The renderer's output is linear. A UNorm8 AOV is what an
                    // 8-bit viewport asks for, and clamping without encoding
                    // would hand it a dark image, so the sRGB transfer function
                    // is applied here -- the same one the EXR path leaves off
                    // because EXR is linear by definition.
                    const float clamped = std::clamp(value, 0.0f, 1.0f);
                    const float encoded =
                        clamped <= 0.0031308f
                            ? clamped * 12.92f
                            : 1.055f * std::pow(clamped, 1.0f / 2.4f) - 0.055f;
                    destination[c] = static_cast<char>(
                        static_cast<uint8_t>(encoded * 255.0f + 0.5f));
                    break;
                }
                case HdFormatInt32: {
                    const int32_t asInt = static_cast<int32_t>(value);
                    std::memcpy(destination + c * sizeof(int32_t), &asInt,
                                sizeof(int32_t));
                    break;
                }
                default:
                    return;
            }
        }
    }
}

PXR_NAMESPACE_CLOSE_SCOPE
