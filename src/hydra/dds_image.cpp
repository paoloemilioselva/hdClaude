#include "dds_image.h"

#include "pxr/base/gf/half.h"

#include <bcdec.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

// The container, as Microsoft documents it ("DDS file layout", "DDS_HEADER",
// "DDS_PIXELFORMAT", "DDS_HEADER_DXT10"). Offsets are from the start of the
// header, which follows the four-byte magic number.
constexpr std::size_t kMagicSize = 4;
constexpr std::size_t kHeaderSize = 124;
constexpr std::size_t kDx10HeaderSize = 20;

constexpr std::size_t kHeaderFlags = 4;
constexpr std::size_t kHeaderHeight = 8;
constexpr std::size_t kHeaderWidth = 12;
constexpr std::size_t kHeaderDepth = 20;
constexpr std::size_t kPixelFormatFlags = 76;
constexpr std::size_t kPixelFormatFourCC = 80;
constexpr std::size_t kPixelFormatBits = 84;
constexpr std::size_t kPixelFormatMasks = 88;   // R, G, B, A in turn
constexpr std::size_t kHeaderCaps2 = 108;

constexpr std::uint32_t kFlagDepth = 0x800000;
constexpr std::uint32_t kCaps2Cubemap = 0x200;
constexpr std::uint32_t kCaps2Volume = 0x200000;

constexpr std::uint32_t kPixelAlphaPixels = 0x1;
constexpr std::uint32_t kPixelAlpha = 0x2;
constexpr std::uint32_t kPixelFourCC = 0x4;
constexpr std::uint32_t kPixelRgb = 0x40;
constexpr std::uint32_t kPixelYuv = 0x200;
constexpr std::uint32_t kPixelLuminance = 0x20000;
constexpr std::uint32_t kPixelBumpDuDv = 0x80000;

constexpr std::uint32_t kDimensionTexture2D = 3;
constexpr std::uint32_t kMiscTextureCube = 0x4;

constexpr std::uint32_t FourCC(char a, char b, char c, char d)
{
    return static_cast<std::uint32_t>(static_cast<unsigned char>(a)) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 8 |
           static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 16 |
           static_cast<std::uint32_t>(static_cast<unsigned char>(d)) << 24;
}

/// Little-endian reads, spelled out so they do not depend on the host's order
/// or on the alignment of a buffer the resolver handed us.
std::uint32_t ReadU32(const std::uint8_t* at)
{
    return static_cast<std::uint32_t>(at[0]) |
           static_cast<std::uint32_t>(at[1]) << 8 |
           static_cast<std::uint32_t>(at[2]) << 16 |
           static_cast<std::uint32_t>(at[3]) << 24;
}

std::uint16_t ReadU16(const std::uint8_t* at)
{
    return static_cast<std::uint16_t>(at[0] | at[1] << 8);
}

float HalfBitsToFloat(std::uint16_t bits)
{
    GfHalf half;
    half.setBits(bits);
    return static_cast<float>(half);
}

std::uint8_t UnitToByte(float unit)
{
    return static_cast<std::uint8_t>(std::clamp(unit, 0.0f, 1.0f) * 255.0f + 0.5f);
}

enum class Codec {
    Masked,       // bit-packed channels described by masks
    Components,   // whole 16- or 32-bit components, RGBA order
    Bc1, Bc2, Bc3, Bc4, Bc5, Bc6hUnsigned, Bc6hSigned, Bc7,
};

enum class Component { Unorm16, Half, Float };

struct Layout {
    Codec codec = Codec::Masked;

    // Codec::Masked
    std::uint32_t bitsPerPixel = 0;
    std::uint32_t masks[4] = {0, 0, 0, 0};
    bool luminance = false;

    // Codec::Components
    Component component = Component::Unorm16;
    std::uint32_t channels = 0;
};

Layout Masked(std::uint32_t bits, std::uint32_t r, std::uint32_t g,
              std::uint32_t b, std::uint32_t a)
{
    Layout layout;
    layout.codec = Codec::Masked;
    layout.bitsPerPixel = bits;
    layout.masks[0] = r;
    layout.masks[1] = g;
    layout.masks[2] = b;
    layout.masks[3] = a;
    return layout;
}

Layout Components(Component component, std::uint32_t channels)
{
    Layout layout;
    layout.codec = Codec::Components;
    layout.component = component;
    layout.channels = channels;
    return layout;
}

Layout Block(Codec codec)
{
    Layout layout;
    layout.codec = codec;
    return layout;
}

std::string FourCCName(std::uint32_t code)
{
    std::string name;
    for (int i = 0; i < 4; ++i) {
        const char c = static_cast<char>((code >> (8 * i)) & 0xff);
        if (c < 0x20 || c > 0x7e) {
            return std::to_string(code);   // a D3DFMT number, not letters
        }
        name += c;
    }
    return "'" + name + "'";
}

/// The layout a DX10 header's DXGI_FORMAT names.
bool FromDxgi(std::uint32_t format, Layout* layout, std::string* error)
{
    // The _SRGB and plain variants of a format decode alike. Which encoding
    // an eight-bit image is read in is decided after decoding, by the same
    // rule as the Hio path (HdClaudeDecodeDds), not by the file: tools write
    // colour maps as BC7_UNORM unless asked otherwise, and reading those as
    // linear would wash out every DDS albedo that a PNG would show correctly.
    switch (format) {
        case 2:  *layout = Components(Component::Float, 4); return true;
        case 6:  *layout = Components(Component::Float, 3); return true;
        case 10: *layout = Components(Component::Half, 4); return true;
        case 11: *layout = Components(Component::Unorm16, 4); return true;
        case 16: *layout = Components(Component::Float, 2); return true;
        case 34: *layout = Components(Component::Half, 2); return true;
        case 41: *layout = Components(Component::Float, 1); return true;
        case 54: *layout = Components(Component::Half, 1); return true;

        case 24:   // R10G10B10A2_UNORM
            *layout = Masked(32, 0x3ff, 0xffc00, 0x3ff00000, 0xc0000000);
            return true;
        case 27:   // R8G8B8A8_TYPELESS
        case 28:   // R8G8B8A8_UNORM
        case 29:   // R8G8B8A8_UNORM_SRGB
            *layout = Masked(32, 0xff, 0xff00, 0xff0000, 0xff000000);
            return true;
        case 35:   // R16G16_UNORM
            *layout = Masked(32, 0xffff, 0xffff0000, 0, 0);
            return true;
        case 48:   // R8G8_TYPELESS
        case 49:   // R8G8_UNORM
            *layout = Masked(16, 0xff, 0xff00, 0, 0);
            return true;
        case 53:   // R16_TYPELESS
        case 56:   // R16_UNORM
            *layout = Masked(16, 0xffff, 0, 0, 0);
            return true;
        case 60:   // R8_TYPELESS
        case 61:   // R8_UNORM
            *layout = Masked(8, 0xff, 0, 0, 0);
            return true;
        case 65:   // A8_UNORM
            *layout = Masked(8, 0, 0, 0, 0xff);
            return true;
        case 85:   // B5G6R5_UNORM
            *layout = Masked(16, 0xf800, 0x7e0, 0x1f, 0);
            return true;
        case 86:   // B5G5R5A1_UNORM
            *layout = Masked(16, 0x7c00, 0x3e0, 0x1f, 0x8000);
            return true;
        case 87:   // B8G8R8A8_UNORM
        case 90:   // B8G8R8A8_TYPELESS
        case 91:   // B8G8R8A8_UNORM_SRGB
            *layout = Masked(32, 0xff0000, 0xff00, 0xff, 0xff000000);
            return true;
        case 88:   // B8G8R8X8_UNORM
        case 92:   // B8G8R8X8_TYPELESS
        case 93:   // B8G8R8X8_UNORM_SRGB
            *layout = Masked(32, 0xff0000, 0xff00, 0xff, 0);
            return true;
        case 115:  // B4G4R4A4_UNORM
            *layout = Masked(16, 0xf00, 0xf0, 0xf, 0xf000);
            return true;

        case 70:
        case 71:
        case 72: *layout = Block(Codec::Bc1); return true;
        case 73:
        case 74:
        case 75: *layout = Block(Codec::Bc2); return true;
        case 76:
        case 77:
        case 78: *layout = Block(Codec::Bc3); return true;
        case 79:
        case 80: *layout = Block(Codec::Bc4); return true;
        case 82:
        case 83: *layout = Block(Codec::Bc5); return true;
        case 94:
        case 95: *layout = Block(Codec::Bc6hUnsigned); return true;
        case 96: *layout = Block(Codec::Bc6hSigned); return true;
        case 97:
        case 98:
        case 99: *layout = Block(Codec::Bc7); return true;

        // Signed-normalised data. Clamping it to [0, 1], as the Hio path does
        // for a signed source, would discard half its range, and remapping it
        // would be a guess at what the material expects; neither is honest.
        case 13: case 31: case 37: case 51: case 58: case 63: case 81: case 84:
            if (error) {
                *error = "DXGI format " + std::to_string(format) +
                         " is signed-normalised, which is not supported";
            }
            return false;

        default:
            if (error) {
                *error = "DXGI format " + std::to_string(format) +
                         " is not supported";
            }
            return false;
    }
}

/// The layout a legacy header's DDS_PIXELFORMAT describes.
bool FromLegacy(const std::uint8_t* header, Layout* layout, std::string* error)
{
    const std::uint32_t flags = ReadU32(header + kPixelFormatFlags);

    if (flags & kPixelFourCC) {
        const std::uint32_t code = ReadU32(header + kPixelFormatFourCC);
        switch (code) {
            case FourCC('D', 'X', 'T', '1'): *layout = Block(Codec::Bc1); return true;
            case FourCC('D', 'X', 'T', '3'): *layout = Block(Codec::Bc2); return true;
            case FourCC('D', 'X', 'T', '5'): *layout = Block(Codec::Bc3); return true;
            case FourCC('A', 'T', 'I', '1'):
            case FourCC('B', 'C', '4', 'U'):
                *layout = Block(Codec::Bc4);
                return true;
            case FourCC('A', 'T', 'I', '2'):
            case FourCC('B', 'C', '5', 'U'):
                *layout = Block(Codec::Bc5);
                return true;
            // D3DFMT values stored where a FourCC would be.
            case 36:  *layout = Components(Component::Unorm16, 4); return true;
            case 111: *layout = Components(Component::Half, 1); return true;
            case 112: *layout = Components(Component::Half, 2); return true;
            case 113: *layout = Components(Component::Half, 4); return true;
            case 114: *layout = Components(Component::Float, 1); return true;
            case 115: *layout = Components(Component::Float, 2); return true;
            case 116: *layout = Components(Component::Float, 4); return true;
            case FourCC('D', 'X', 'T', '2'):
            case FourCC('D', 'X', 'T', '4'):
                if (error) {
                    *error = "premultiplied-alpha " + FourCCName(code) +
                             " is not supported";
                }
                return false;
            case FourCC('B', 'C', '4', 'S'):
            case FourCC('B', 'C', '5', 'S'):
                if (error) {
                    *error = "signed-normalised " + FourCCName(code) +
                             " is not supported";
                }
                return false;
            default:
                if (error) {
                    *error = "FourCC " + FourCCName(code) + " is not supported";
                }
                return false;
        }
    }

    if (flags & (kPixelBumpDuDv | kPixelYuv)) {
        if (error) {
            *error = "signed (bump) and YUV pixel formats are not supported";
        }
        return false;
    }

    if (!(flags & (kPixelRgb | kPixelLuminance | kPixelAlpha))) {
        if (error) *error = "the pixel format describes no channels";
        return false;
    }

    const std::uint32_t bits = ReadU32(header + kPixelFormatBits);
    if (bits != 8 && bits != 16 && bits != 24 && bits != 32) {
        if (error) {
            *error = std::to_string(bits) + " bits a pixel is not supported";
        }
        return false;
    }
    std::uint32_t masks[4];
    for (int c = 0; c < 4; ++c) {
        masks[c] = ReadU32(header + kPixelFormatMasks + 4 * c);
    }
    // An alpha mask counts only when a flag says alpha is present; writers
    // leave stale masks behind in X8R8G8B8 and the like.
    if (!(flags & (kPixelAlphaPixels | kPixelAlpha))) {
        masks[3] = 0;
    }
    if (!(flags & (kPixelRgb | kPixelLuminance))) {
        masks[0] = masks[1] = masks[2] = 0;   // alpha only
    }
    *layout = Masked(bits, masks[0], masks[1], masks[2], masks[3]);
    layout->luminance = (flags & kPixelLuminance) != 0;
    return true;
}

/// One channel of a masked layout.
struct MaskChannel {
    std::uint32_t mask = 0;
    std::uint32_t shift = 0;
    std::uint32_t max = 0;
    std::uint32_t width = 0;

    explicit MaskChannel(std::uint32_t m) : mask(m)
    {
        if (mask == 0) {
            return;
        }
        while (((mask >> shift) & 1u) == 0) {
            ++shift;
        }
        max = mask >> shift;
        for (std::uint32_t v = max; v != 0; v >>= 1) {
            width += v & 1u;
        }
    }

    bool Present() const { return mask != 0; }

    std::uint8_t Read(std::uint32_t pixel) const
    {
        const std::uint64_t value = (pixel & mask) >> shift;
        return static_cast<std::uint8_t>((value * 255u + max / 2u) / max);
    }
};

/// Whether a format holds eight-bit colour, which Hio would call sRGB if the
/// same pixels came from a PNG.
bool IsEightBitColour(const Layout& layout)
{
    switch (layout.codec) {
        case Codec::Bc1:
        case Codec::Bc2:
        case Codec::Bc3:
        case Codec::Bc7:
            return true;
        case Codec::Masked: {
            if (layout.luminance) {
                return false;
            }
            for (int c = 0; c < 3; ++c) {
                const MaskChannel channel(layout.masks[c]);
                if (!channel.Present() || channel.width > 8) {
                    return false;
                }
            }
            return true;
        }
        default:
            return false;
    }
}

bool IsFloat(const Layout& layout)
{
    return layout.codec == Codec::Bc6hUnsigned ||
           layout.codec == Codec::Bc6hSigned ||
           (layout.codec == Codec::Components &&
            layout.component != Component::Unorm16);
}

std::size_t BlockBytes(Codec codec)
{
    return codec == Codec::Bc1 || codec == Codec::Bc4 ? 8u : 16u;
}

/// Bytes the top-level image occupies.
std::uint64_t TopLevelBytes(const Layout& layout, std::uint64_t width,
                            std::uint64_t height)
{
    switch (layout.codec) {
        case Codec::Masked:
            return (width * layout.bitsPerPixel + 7u) / 8u * height;
        case Codec::Components:
            return width * height * layout.channels *
                   (layout.component == Component::Float ? 4u : 2u);
        default:
            return ((width + 3u) / 4u) * ((height + 3u) / 4u) *
                   BlockBytes(layout.codec);
    }
}

/// The decoded image, top row first, in whichever of the two widths it needs.
struct Decoded {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> bytes;   // RGBA8, when the source is not float
    std::vector<float> floats;         // RGBA32F, when it is
};

void DecodeMasked(const Layout& layout, const std::uint8_t* data, Decoded* out)
{
    const MaskChannel r(layout.masks[0]);
    const MaskChannel g(layout.masks[1]);
    const MaskChannel b(layout.masks[2]);
    const MaskChannel a(layout.masks[3]);
    // One colour channel reads as grey, as a single-channel image does on the
    // Hio path: that is what a greyscale roughness or mask map means.
    const bool grey = r.Present() &&
                      (layout.luminance || (!g.Present() && !b.Present()));

    const std::size_t pixelBytes = layout.bitsPerPixel / 8u;
    const std::size_t rowBytes = (static_cast<std::size_t>(out->width) *
                                      layout.bitsPerPixel + 7u) / 8u;
    out->bytes.resize(static_cast<std::size_t>(out->width) * out->height * 4u);
    for (std::uint32_t y = 0; y < out->height; ++y) {
        const std::uint8_t* row = data + y * rowBytes;
        for (std::uint32_t x = 0; x < out->width; ++x) {
            std::uint32_t pixel = 0;
            for (std::size_t i = 0; i < pixelBytes; ++i) {
                pixel |= static_cast<std::uint32_t>(row[x * pixelBytes + i]) << (8u * i);
            }
            std::uint8_t* to = &out->bytes[(static_cast<std::size_t>(y) * out->width + x) * 4u];
            if (grey) {
                to[0] = to[1] = to[2] = r.Read(pixel);
            } else {
                to[0] = r.Present() ? r.Read(pixel) : 0;
                to[1] = g.Present() ? g.Read(pixel) : 0;
                to[2] = b.Present() ? b.Read(pixel) : 0;
            }
            to[3] = a.Present() ? a.Read(pixel) : 255;
        }
    }
}

void DecodeComponents(const Layout& layout, const std::uint8_t* data, Decoded* out)
{
    const std::size_t size = layout.component == Component::Float ? 4u : 2u;
    const std::size_t pixels = static_cast<std::size_t>(out->width) * out->height;
    const bool keepFloat = layout.component != Component::Unorm16;
    if (keepFloat) {
        out->floats.resize(pixels * 4u);
    } else {
        out->bytes.resize(pixels * 4u);
    }

    for (std::size_t i = 0; i < pixels; ++i) {
        float value[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        for (std::uint32_t c = 0; c < layout.channels; ++c) {
            const std::uint8_t* at = data + (i * layout.channels + c) * size;
            switch (layout.component) {
                case Component::Unorm16:
                    value[c] = static_cast<float>(ReadU16(at)) / 65535.0f;
                    break;
                case Component::Half:
                    value[c] = HalfBitsToFloat(ReadU16(at));
                    break;
                case Component::Float: {
                    const std::uint32_t bits = ReadU32(at);
                    std::memcpy(&value[c], &bits, sizeof(float));
                    break;
                }
            }
        }
        if (layout.channels == 1) {
            value[1] = value[2] = value[0];
        }
        for (std::size_t c = 0; c < 4; ++c) {
            if (keepFloat) {
                out->floats[i * 4u + c] = value[c];
            } else {
                out->bytes[i * 4u + c] = UnitToByte(value[c]);
            }
        }
    }
}

void DecodeBlocks(const Layout& layout, const std::uint8_t* data, Decoded* out)
{
    const std::uint32_t blocksWide = (out->width + 3u) / 4u;
    const std::uint32_t blocksHigh = (out->height + 3u) / 4u;
    const std::size_t blockBytes = BlockBytes(layout.codec);
    const bool keepFloat = IsFloat(layout);
    const std::size_t pixels = static_cast<std::size_t>(out->width) * out->height;
    if (keepFloat) {
        out->floats.resize(pixels * 4u);
    } else {
        out->bytes.resize(pixels * 4u);
    }

    for (std::uint32_t by = 0; by < blocksHigh; ++by) {
        for (std::uint32_t bx = 0; bx < blocksWide; ++bx) {
            const std::uint8_t* block =
                data + (static_cast<std::size_t>(by) * blocksWide + bx) * blockBytes;

            // A whole 4x4 block, RGBA, then copied into the image clipped to
            // its edge: a dimension that is not a multiple of four still has
            // whole blocks in the file.
            std::uint8_t rgba[16 * 4] = {};
            float rgbaFloat[16 * 4] = {};
            switch (layout.codec) {
                case Codec::Bc1: bcdec_bc1(block, rgba, 16); break;
                case Codec::Bc2: bcdec_bc2(block, rgba, 16); break;
                case Codec::Bc3: bcdec_bc3(block, rgba, 16); break;
                case Codec::Bc7: bcdec_bc7(block, rgba, 16); break;
                case Codec::Bc4: {
                    std::uint8_t red[16];
                    bcdec_bc4(block, red, 4);
                    for (int i = 0; i < 16; ++i) {
                        rgba[i * 4 + 0] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = red[i];
                        rgba[i * 4 + 3] = 255;
                    }
                    break;
                }
                case Codec::Bc5: {
                    std::uint8_t redGreen[16 * 2];
                    bcdec_bc5(block, redGreen, 8);
                    for (int i = 0; i < 16; ++i) {
                        rgba[i * 4 + 0] = redGreen[i * 2 + 0];
                        rgba[i * 4 + 1] = redGreen[i * 2 + 1];
                        rgba[i * 4 + 2] = 0;
                        rgba[i * 4 + 3] = 255;
                    }
                    break;
                }
                case Codec::Bc6hUnsigned:
                case Codec::Bc6hSigned: {
                    // Halves, three a texel; the pitch counts halves, not bytes.
                    std::uint16_t rgb[16 * 3];
                    bcdec_bc6h_half(block, rgb, 12,
                                    layout.codec == Codec::Bc6hSigned ? 1 : 0);
                    for (int i = 0; i < 16; ++i) {
                        for (int c = 0; c < 3; ++c) {
                            rgbaFloat[i * 4 + c] = HalfBitsToFloat(rgb[i * 3 + c]);
                        }
                        rgbaFloat[i * 4 + 3] = 1.0f;
                    }
                    break;
                }
                default:
                    break;
            }

            for (std::uint32_t py = 0; py < 4; ++py) {
                const std::uint32_t y = by * 4u + py;
                if (y >= out->height) {
                    break;
                }
                for (std::uint32_t px = 0; px < 4; ++px) {
                    const std::uint32_t x = bx * 4u + px;
                    if (x >= out->width) {
                        break;
                    }
                    const std::size_t to = (static_cast<std::size_t>(y) * out->width + x) * 4u;
                    const std::size_t from = (py * 4u + px) * 4u;
                    if (keepFloat) {
                        std::copy_n(rgbaFloat + from, 4, out->floats.begin() + to);
                    } else {
                        std::copy_n(rgba + from, 4, out->bytes.begin() + to);
                    }
                }
            }
        }
    }
}

}  // namespace

bool HdClaudeIsDds(const void* bytes, std::size_t size)
{
    return bytes != nullptr && size >= kMagicSize &&
           ReadU32(static_cast<const std::uint8_t*>(bytes)) ==
               FourCC('D', 'D', 'S', ' ');
}

bool HdClaudeDecodeDds(const void* bytes, std::size_t size,
                       HdClaudeTextureColorSpace colorSpace,
                       hdclaude::TextureImage* out, std::string* error)
{
    if (!HdClaudeIsDds(bytes, size)) {
        if (error) *error = "not a DDS file";
        return false;
    }
    if (size < kMagicSize + kHeaderSize) {
        if (error) *error = "the DDS header is truncated";
        return false;
    }
    const auto* file = static_cast<const std::uint8_t*>(bytes);
    const std::uint8_t* header = file + kMagicSize;
    if (ReadU32(header) != kHeaderSize) {
        if (error) *error = "the DDS header has the wrong size";
        return false;
    }

    const std::uint32_t flags = ReadU32(header + kHeaderFlags);
    const std::uint32_t height = ReadU32(header + kHeaderHeight);
    const std::uint32_t width = ReadU32(header + kHeaderWidth);
    const std::uint32_t depth = ReadU32(header + kHeaderDepth);
    const std::uint32_t caps2 = ReadU32(header + kHeaderCaps2);
    if (width == 0 || height == 0) {
        if (error) *error = "the image has no extent";
        return false;
    }
    if (caps2 & kCaps2Cubemap) {
        if (error) *error = "cube maps are not supported; a texture reads one 2D image";
        return false;
    }
    if ((caps2 & kCaps2Volume) || ((flags & kFlagDepth) && depth > 1)) {
        if (error) *error = "volume textures are not supported";
        return false;
    }

    Layout layout;
    std::size_t offset = kMagicSize + kHeaderSize;
    const bool dx10 = (ReadU32(header + kPixelFormatFlags) & kPixelFourCC) &&
                      ReadU32(header + kPixelFormatFourCC) == FourCC('D', 'X', '1', '0');
    if (dx10) {
        if (size < offset + kDx10HeaderSize) {
            if (error) *error = "the DX10 header is truncated";
            return false;
        }
        const std::uint8_t* extension = file + offset;
        const std::uint32_t dimension = ReadU32(extension + 4);
        const std::uint32_t misc = ReadU32(extension + 8);
        const std::uint32_t arraySize = ReadU32(extension + 12);
        if (dimension != kDimensionTexture2D) {
            if (error) *error = "only 2D textures are supported";
            return false;
        }
        if (misc & kMiscTextureCube) {
            if (error) *error = "cube maps are not supported; a texture reads one 2D image";
            return false;
        }
        if (arraySize != 1) {
            if (error) *error = "texture arrays are not supported";
            return false;
        }
        if (!FromDxgi(ReadU32(extension), &layout, error)) {
            return false;
        }
        offset += kDx10HeaderSize;
    } else if (!FromLegacy(header, &layout, error)) {
        return false;
    }

    // Checked before anything is allocated, so a header claiming a vast image
    // cannot make us allocate one: every supported layout takes at least half
    // a byte a pixel, which bounds the decoded size by the file's.
    const std::uint64_t needed = TopLevelBytes(layout, width, height);
    if (needed > size - offset) {
        if (error) {
            *error = "the file is shorter than its " + std::to_string(width) +
                     "x" + std::to_string(height) + " image";
        }
        return false;
    }

    Decoded decoded;
    decoded.width = width;
    decoded.height = height;
    const std::uint8_t* data = file + offset;
    switch (layout.codec) {
        case Codec::Masked:     DecodeMasked(layout, data, &decoded); break;
        case Codec::Components: DecodeComponents(layout, data, &decoded); break;
        default:                DecodeBlocks(layout, data, &decoded); break;
    }

    // The encoding, by the same rule as the Hio path: what the material
    // declared, else sRGB for eight-bit colour and linear for everything else.
    const bool keepFloat = IsFloat(layout);
    if (keepFloat) {
        out->format = hdclaude::TexelFormat::Rgba16Sfloat;
    } else {
        bool srgb = false;
        switch (colorSpace) {
            case HdClaudeTextureColorSpace::Srgb: srgb = true; break;
            case HdClaudeTextureColorSpace::Raw:  srgb = false; break;
            case HdClaudeTextureColorSpace::Auto:
                srgb = IsEightBitColour(layout);
                break;
        }
        out->format = srgb ? hdclaude::TexelFormat::Rgba8Srgb
                           : hdclaude::TexelFormat::Rgba8Unorm;
    }

    // Bottom row first: DDS stores the top row first, and TextureImage's row 0
    // is v = 0, the bottom of the image.
    out->width = width;
    out->height = height;
    const std::size_t texel = hdclaude::TexelSize(out->format);
    const std::size_t rowTexels = static_cast<std::size_t>(width) * 4u;
    out->texels.assign(static_cast<std::size_t>(width) * height * texel, 0);
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::size_t from = static_cast<std::size_t>(y) * rowTexels;
        const std::size_t to = static_cast<std::size_t>(height - 1u - y) * rowTexels;
        if (keepFloat) {
            for (std::size_t i = 0; i < rowTexels; ++i) {
                const GfHalf half(decoded.floats[from + i]);
                std::memcpy(out->texels.data() + (to + i) * 2u, &half, sizeof(half));
            }
        } else {
            std::copy_n(decoded.bytes.begin() + from, rowTexels,
                        out->texels.begin() + to);
        }
    }
    return true;
}

PXR_NAMESPACE_CLOSE_SCOPE
