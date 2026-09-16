#include "texture_loader.h"

#include "trace.h"

#include "pxr/base/gf/half.h"
#include "pxr/imaging/hio/image.h"
#include "pxr/imaging/hio/types.h"
#include "pxr/usd/ar/resolver.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

/// Widen whatever Hio decoded into the tightly packed RGBA8 the GPU expects.
///
/// Asking Hio to read straight into HioFormatUNorm8Vec4 would be simpler, but
/// a plugin is free to decline a conversion, and a silently unconverted read
/// is a buffer of the wrong stride rather than an error. Reading in the file's
/// own format and widening here is the version that cannot be quietly wrong.
/// One decoded component, as the eight-bit value the GPU texture holds.
///
/// The renderer's textures are RGBA8, so everything wider is quantised here
/// rather than at upload; a 16-bit source is a finer *source*, not a finer
/// texture, until the pool carries more than one format.
std::uint8_t Quantise(const char* source, HioType type)
{
    const auto fromUnit = [](float unit) {
        return static_cast<std::uint8_t>(std::clamp(unit, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    switch (type) {
        case HioTypeUnsignedByte:
        case HioTypeUnsignedByteSRGB:
            return static_cast<std::uint8_t>(
                *reinterpret_cast<const unsigned char*>(source));
        case HioTypeSignedByte: {
            std::int8_t value = 0;
            std::memcpy(&value, source, sizeof(value));
            return fromUnit(static_cast<float>(value) / 127.0f);
        }
        case HioTypeUnsignedShort: {
            std::uint16_t value = 0;
            std::memcpy(&value, source, sizeof(value));
            return fromUnit(static_cast<float>(value) / 65535.0f);
        }
        case HioTypeSignedShort: {
            std::int16_t value = 0;
            std::memcpy(&value, source, sizeof(value));
            return fromUnit(static_cast<float>(value) / 32767.0f);
        }
        case HioTypeUnsignedInt: {
            std::uint32_t value = 0;
            std::memcpy(&value, source, sizeof(value));
            return fromUnit(static_cast<float>(value) / 4294967295.0f);
        }
        case HioTypeInt: {
            std::int32_t value = 0;
            std::memcpy(&value, source, sizeof(value));
            return fromUnit(static_cast<float>(value) / 2147483647.0f);
        }
        case HioTypeHalfFloat: {
            GfHalf value;
            std::memcpy(&value, source, sizeof(value));
            return fromUnit(static_cast<float>(value));
        }
        case HioTypeFloat: {
            float value = 0.0f;
            std::memcpy(&value, source, sizeof(value));
            return fromUnit(value);
        }
        default:
            return 0;
    }
}

/// One decoded component, unclamped.
///
/// Only the float and half sources need this: an integer source has a defined
/// maximum and quantising it costs precision, while a float source has no
/// maximum and quantising it costs everything above 1.0.
float FloatComponent(const char* source, HioType type)
{
    switch (type) {
        case HioTypeHalfFloat: {
            GfHalf value;
            std::memcpy(&value, source, sizeof(value));
            return static_cast<float>(value);
        }
        case HioTypeFloat: {
            float value = 0.0f;
            std::memcpy(&value, source, sizeof(value));
            return value;
        }
        default:
            return 0.0f;
    }
}

/// True when a decoded component carries values the eight-bit path would clamp.
bool IsFloatType(HioType type)
{
    return type == HioTypeHalfFloat || type == HioTypeFloat;
}

void WriteHalf(std::uint8_t* destination, float value)
{
    const GfHalf half(value);
    std::memcpy(destination, &half, sizeof(half));
}

bool Widen(const HioImageSharedPtr& image, hdclaude::TextureImage* out,
           std::string* error, HdClaudeTextureColorSpace colorSpace)
{
    const int width = image->GetWidth();
    const int height = image->GetHeight();
    if (width <= 0 || height <= 0) {
        if (error) *error = "the image has no extent";
        return false;
    }

    const HioFormat format = image->GetFormat();
    const size_t channels = HioGetComponentCount(format);
    const HioType type = HioGetHioType(format);
    if (channels == 0 || channels > 4) {
        if (error) *error = "unsupported channel count";
        return false;
    }

    // The component types a decoder actually produces. Sixteen-bit is not
    // exotic -- it is what a mask or a height map authored in a paint package
    // saves as, and three of the OpenPBR playground's TIFFs are exactly that --
    // and refusing it looked from the outside like a missing TIFF decoder.
    size_t bytesPerComponent = 0;
    switch (type) {
        case HioTypeUnsignedByte:
        case HioTypeUnsignedByteSRGB:
        case HioTypeSignedByte:   bytesPerComponent = 1; break;
        case HioTypeUnsignedShort:
        case HioTypeSignedShort:
        case HioTypeHalfFloat:    bytesPerComponent = 2; break;
        case HioTypeUnsignedInt:
        case HioTypeInt:
        case HioTypeFloat:        bytesPerComponent = 4; break;
        default:
            if (error) {
                *error = "unsupported component type";
            }
            return false;
    }

    std::vector<char> raw(static_cast<size_t>(width) * height * channels *
                          bytesPerComponent);

    HioImage::StorageSpec storage;
    storage.width = width;
    storage.height = height;
    storage.depth = 1;
    storage.format = format;
    // Read bottom-up. `flipped` asks Hio to reverse the file's row order, and
    // the file's order is top row first; the renderer's convention -- stated on
    // TextureImage and matched by the sampler, whose v = 0 is the first row of
    // the uploaded image -- is that row 0 is v = 0, the *bottom* of the image,
    // because that is where USD and MaterialX put it.
    //
    // Passing false here uploaded every texture upside down. It is not a
    // striking failure: a noise or a gradient map looks equally plausible
    // either way, and it took a backdrop with printed numbers on it to make the
    // flip visible at all.
    storage.flipped = true;
    storage.data = raw.data();

    if (!image->Read(storage)) {
        if (error) *error = "the image plugin could not read it";
        return false;
    }

    out->width = static_cast<std::uint32_t>(width);
    out->height = static_cast<std::uint32_t>(height);
    // What the file says it holds, overruled by what the material says it
    // means. Hio answers from the format alone -- an 8-bit three-channel image
    // is sRGB to it -- and that is right for a colour map and wrong for every
    // data map shipped in the same container. The chess set's normal, roughness
    // and metalness maps are all 8-bit JPEGs whose MaterialX nodes are
    // `vector3` and `float` with no colorspace: reading them as sRGB tilted
    // every surface and exaggerated every bump.
    // A float source keeps its range. An HDRI's whole contribution as a light
    // is the part of it above 1.0 -- the window, the lamp, the sun -- and the
    // eight-bit path clamps exactly that away, leaving a dome light that lights
    // nothing brightly and casts no shadow worth the name. Colour space does
    // not enter into it: a float image is linear by definition, so the sRGB
    // request only ever chooses between the two eight-bit forms.
    const bool keepFloat = IsFloatType(type);
    if (keepFloat) {
        out->format = hdclaude::TexelFormat::Rgba16Sfloat;
    } else {
        switch (colorSpace) {
            case HdClaudeTextureColorSpace::Srgb:
                out->format = hdclaude::TexelFormat::Rgba8Srgb;
                break;
            case HdClaudeTextureColorSpace::Raw:
                out->format = hdclaude::TexelFormat::Rgba8Unorm;
                break;
            case HdClaudeTextureColorSpace::Auto:
                out->format = image->IsColorSpaceSRGB()
                                  ? hdclaude::TexelFormat::Rgba8Srgb
                                  : hdclaude::TexelFormat::Rgba8Unorm;
                break;
        }
    }

    const size_t texel = hdclaude::TexelSize(out->format);
    out->texels.assign(static_cast<size_t>(width) * height * texel, 0);

    const size_t pixels = static_cast<size_t>(width) * height;
    for (size_t i = 0; i < pixels; ++i) {
        for (size_t c = 0; c < 4; ++c) {
            // The component this channel reads, or none: alpha defaults to
            // opaque, and a single-channel image reads as grey rather than as
            // red, which is what a greyscale roughness or mask map means.
            const char* source = nullptr;
            if (c < channels) {
                source = raw.data() + (i * channels + c) * bytesPerComponent;
            } else if (c < 3 && channels == 1) {
                source = raw.data() + i * bytesPerComponent;
            }

            if (keepFloat) {
                WriteHalf(out->texels.data() + (i * 4 + c) * 2,
                          source ? FloatComponent(source, type)
                                 : (c == 3 ? 1.0f : 0.0f));
            } else {
                out->texels[i * 4 + c] =
                    source ? Quantise(source, type)
                           : static_cast<std::uint8_t>(c == 3 ? 255 : 0);
            }
        }
    }
    return true;
}

/// sRGB's transfer function and its inverse, as IEC 61966-2-1 defines them.
///
/// A texture stored as `Rgba8Srgb` is *encoded*, and the sampler decodes it on
/// every fetch. Averaging four encoded values is therefore not averaging the
/// four colours: sRGB is concave, so the encoded mean is brighter than the
/// mean's encoding, and a reduced texture drifts lighter everywhere it has
/// contrast. Each reduction step decodes, averages, and re-encodes.
float SrgbToLinear(float encoded)
{
    return encoded <= 0.04045f ? encoded / 12.92f
                               : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
}

float LinearToSrgb(float linear)
{
    return linear <= 0.0031308f
               ? linear * 12.92f
               : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
}

/// Halve an image in both axes, averaging each two-by-two block.
///
/// A box filter, which is exactly the right filter for a power-of-two
/// reduction: every source texel lands in exactly one block, so the image's
/// mean is preserved to the precision of its storage. An odd dimension rounds
/// up and its last block is half empty, which weights that edge more heavily
/// than the rest -- the alternative is dropping the line entirely.
void Halve(hdclaude::TextureImage* image)
{
    // Rounded up, so an odd dimension keeps its last row or column as a block
    // of its own rather than dropping it. Truncating would lose a line at every
    // step and take the image's mean with it.
    const std::uint32_t width = std::max(1u, (image->width + 1u) / 2u);
    const std::uint32_t height = std::max(1u, (image->height + 1u) / 2u);
    const bool srgb = image->format == hdclaude::TexelFormat::Rgba8Srgb;
    const bool half = image->format == hdclaude::TexelFormat::Rgba16Sfloat;
    const std::size_t texel = hdclaude::TexelSize(image->format);

    std::vector<std::uint8_t> reduced(static_cast<std::size_t>(width) * height *
                                      texel);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            for (std::uint32_t c = 0; c < 4; ++c) {
                float sum = 0.0f;
                int taken = 0;
                for (std::uint32_t dy = 0; dy < 2; ++dy) {
                    for (std::uint32_t dx = 0; dx < 2; ++dx) {
                        const std::uint32_t sx = x * 2u + dx;
                        const std::uint32_t sy = y * 2u + dy;
                        if (sx >= image->width || sy >= image->height) {
                            continue;
                        }
                        const std::size_t at =
                            (static_cast<std::size_t>(sy) * image->width + sx) *
                                texel + c * (half ? 2u : 1u);
                        float value = 0.0f;
                        if (half) {
                            GfHalf stored;
                            std::memcpy(&stored, &image->texels[at],
                                        sizeof(stored));
                            value = static_cast<float>(stored);
                        } else {
                            value = static_cast<float>(image->texels[at]) / 255.0f;
                            // Alpha is not sRGB-encoded even in an sRGB image.
                            if (srgb && c < 3) {
                                value = SrgbToLinear(value);
                            }
                        }
                        sum += value;
                        ++taken;
                    }
                }
                float mean = taken > 0 ? sum / static_cast<float>(taken) : 0.0f;
                const std::size_t to =
                    (static_cast<std::size_t>(y) * width + x) * texel +
                    c * (half ? 2u : 1u);
                if (half) {
                    WriteHalf(reduced.data() + to, mean);
                } else {
                    if (srgb && c < 3) {
                        mean = LinearToSrgb(mean);
                    }
                    reduced[to] = static_cast<std::uint8_t>(
                        std::clamp(mean, 0.0f, 1.0f) * 255.0f + 0.5f);
                }
            }
        }
    }

    image->width = width;
    image->height = height;
    image->texels = std::move(reduced);
}

/// Halve until the longest edge is within `maxEdge`. Reports how many times.
int Reduce(hdclaude::TextureImage* image, std::uint32_t maxEdge)
{
    int steps = 0;
    while (maxEdge > 0 && std::max(image->width, image->height) > maxEdge &&
           (image->width > 1 || image->height > 1)) {
        Halve(image);
        ++steps;
    }
    return steps;
}

}  // namespace

std::uint32_t HdClaudeTextureEdgeCap(const std::string& quality)
{
    std::string name;
    std::transform(quality.begin(), quality.end(), std::back_inserter(name),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (name == "medium" || name == "med") {
        return 1024;
    }
    if (name == "low") {
        return 256;
    }
    return 0;
}

bool HdClaudeIsTextureQuality(const std::string& quality)
{
    std::string name;
    std::transform(quality.begin(), quality.end(), std::back_inserter(name),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return name.empty() || name == "high" || name == "medium" || name == "med" ||
           name == "low";
}

bool HdClaudeLoadTexture(const std::string& assetPath,
                         hdclaude::TextureImage* out, std::string* error,
                         HdClaudeTextureColorSpace colorSpace,
                         std::uint32_t maxEdge)
{
    if (assetPath.empty() || out == nullptr) {
        if (error) *error = "no asset path";
        return false;
    }

    // An unexpanded UDIM token is a defect, not something to guess at.
    //
    // `<UDIM>` is a token USD leaves in the path for the renderer to expand
    // into one texture per tile, selected by which unit square of UV space a
    // sample lands in. That selection lives in the material compiler and the
    // generated shader now (docs/implementation-notes.md, 2026-09-08), which
    // resolve the set's tiles before generation and hand this loader one
    // concrete path per tile. So a token reaching here means the expansion did
    // not happen, and the honest answer is to say so.
    //
    // It used to collapse the set to its first existing tile and shade every
    // tile with it -- right for the many assets that ship one, and wrong for
    // every asset that does not. ALab's `electronics_turntable01` has nineteen
    // of its twenty-two meshes on a tile other than 1001, so better than half
    // its surface carried the wrong image, and nothing said a word.
    if (assetPath.find("<UDIM>") != std::string::npos) {
        if (error) {
            *error = "'" + assetPath +
                     "' still carries an unexpanded <UDIM> token; its tiles "
                     "should have been resolved before the image was loaded";
        }
        return false;
    }

    std::string path = assetPath;

    // Resolve through Ar so a path relative to a layer, or inside a package,
    // is found the same way every other USD consumer finds it.
    std::string resolved = path;
    if (ArResolvedPath resolvedPath = ArGetResolver().Resolve(path)) {
        resolved = resolvedPath.GetPathString();
    }

    HioImageSharedPtr image = HioImage::OpenForReading(resolved);
    if (!image) {
        if (error) {
            *error = "no image plugin opened '" + resolved + "'";
        }
        return false;
    }

    out->debugName = assetPath;
    if (!Widen(image, out, error, colorSpace)) {
        return false;
    }

    // Reduced after decoding rather than by asking the image plugin for a
    // smaller read. A plugin is free to decline a resize, and Hio reports that
    // by handing back a buffer of the size it felt like -- the same class of
    // silent mismatch this file already refuses to rely on for format
    // conversion. The cost is that one image is decoded at its authored size
    // before it is reduced, so the *peak* is one full image; what the cap
    // controls is what the scene holds, which is what runs a machine out of
    // memory.
    const std::uint32_t authoredWidth = out->width;
    const std::uint32_t authoredHeight = out->height;
    if (Reduce(out, maxEdge) > 0) {
        HdClaudeTrace("texture %s: %ux%u reduced to %ux%u (cap %u)",
                      assetPath.c_str(), authoredWidth, authoredHeight,
                      out->width, out->height, maxEdge);
    }
    return true;
}

std::uint32_t HdClaudeTexturePool::Acquire(const std::string& assetPath,
                                           HdClaudeTextureColorSpace colorSpace)
{
    std::lock_guard<std::mutex> lock(_mutex);

    const auto key = std::make_pair(assetPath, colorSpace);
    const auto found = _slots.find(key);
    if (found != _slots.end()) {
        return found->second;
    }

    const auto slot = static_cast<std::uint32_t>(_images.size());
    _slots[key] = slot;
    _sources.push_back(key);

    hdclaude::TextureImage image;
    std::string error;
    if (assetPath.empty()) {
        // An image node with no file. MaterialX defines such a node as
        // returning its `default` input, whose declared value for every
        // `ND_image_*` is zero, so a one-pixel black texture is what the
        // generated code has to sample to produce it -- the stock
        // implementation samples unconditionally and has no other way to say
        // "no image".
        //
        // This is deliberately *not* the failure placeholder. A node with no
        // file is an authoring choice that assets make routinely; a file that
        // is named and cannot be read is a broken asset. Rendering the first
        // as magenta puts a glaring wrong colour on a surface that should be
        // unaffected, which is exactly what the StandardShaderBall's inner
        // shell did.
        image.width = 1;
        image.height = 1;
        image.texels = {0, 0, 0, 255};
        image.format = hdclaude::TexelFormat::Rgba8Unorm;
        image.debugName = "image node with no file";
        HdClaudeTrace("texture %u: unbound image node; reads its default", slot);
        _images.push_back(std::move(image));
        return slot;
    }

    if (HdClaudeLoadTexture(assetPath, &image, &error, colorSpace, _maxEdge)) {
        const char* encoding = "linear";
        if (image.format == hdclaude::TexelFormat::Rgba8Srgb) {
            encoding = "sRGB";
        } else if (image.format == hdclaude::TexelFormat::Rgba16Sfloat) {
            encoding = "linear half";
        }
        HdClaudeTrace("texture %u: %s (%ux%u, %s)", slot, assetPath.c_str(),
                      image.width, image.height, encoding);
    } else {
        // The slot still exists and still holds an invalid image, so the
        // renderer binds its placeholder and the material's other indices are
        // undisturbed. A missing texture should be visible, not silent.
        image = hdclaude::TextureImage();
        image.debugName = assetPath;
        _failures.push_back(assetPath + ": " + error);
        HdClaudeTrace("texture %u: FAILED %s (%s)", slot, assetPath.c_str(),
                      error.c_str());
    }
    _images.push_back(std::move(image));
    return slot;
}

void HdClaudeTexturePool::Clear()
{
    std::lock_guard<std::mutex> lock(_mutex);
    _slots.clear();
    _sources.clear();
    _images.clear();
    _failures.clear();
}

std::uint32_t HdClaudeTexturePool::MaxEdge() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _maxEdge;
}

bool HdClaudeTexturePool::SetMaxEdge(std::uint32_t maxEdge)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (maxEdge == _maxEdge) {
        return false;
    }
    _maxEdge = maxEdge;

    // Every image already held is re-decoded from its own source, into the slot
    // it already occupies. Re-decoding rather than halving what is in hand,
    // because halving a reduced image again is not the same as reducing the
    // original that far -- and because raising the cap has to get the detail
    // back, which nothing in the pool still holds.
    _failures.clear();
    for (std::size_t slot = 0; slot < _images.size(); ++slot) {
        const std::string& path = _sources[slot].first;
        if (path.empty()) {
            continue;   // an image node with no file; its one texel stands
        }
        hdclaude::TextureImage image;
        std::string error;
        if (HdClaudeLoadTexture(path, &image, &error, _sources[slot].second,
                                _maxEdge)) {
            _images[slot] = std::move(image);
        } else {
            _images[slot] = hdclaude::TextureImage();
            _images[slot].debugName = path;
            _failures.push_back(path + ": " + error);
        }
    }
    HdClaudeTrace("texture quality changed: %zu images reloaded with a cap of "
                  "%u", _images.size(), _maxEdge);
    return true;
}

PXR_NAMESPACE_CLOSE_SCOPE
