#include "texture_loader.h"

#include "trace.h"

#include "pxr/imaging/hio/image.h"
#include "pxr/imaging/hio/types.h"
#include "pxr/usd/ar/resolver.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

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
    switch (colorSpace) {
        case HdClaudeTextureColorSpace::Srgb: out->srgb = true; break;
        case HdClaudeTextureColorSpace::Raw:  out->srgb = false; break;
        case HdClaudeTextureColorSpace::Auto:
            out->srgb = image->IsColorSpaceSRGB();
            break;
    }
    out->rgba.assign(static_cast<size_t>(width) * height * 4, 0);

    const size_t pixels = static_cast<size_t>(width) * height;
    for (size_t i = 0; i < pixels; ++i) {
        for (size_t c = 0; c < 4; ++c) {
            std::uint8_t value = (c == 3) ? 255 : 0;
            if (c < channels) {
                value = Quantise(raw.data() + (i * channels + c) * bytesPerComponent,
                                 type);
            } else if (c < 3 && channels == 1) {
                // A single-channel image reads as grey rather than as red,
                // which is what a greyscale roughness or mask map means.
                value = Quantise(raw.data() + i * bytesPerComponent, type);
            }
            out->rgba[i * 4 + c] = value;
        }
    }
    return true;
}

}  // namespace

bool HdClaudeLoadTexture(const std::string& assetPath,
                         hdclaude::TextureImage* out, std::string* error,
                         HdClaudeTextureColorSpace colorSpace)
{
    if (assetPath.empty() || out == nullptr) {
        if (error) *error = "no asset path";
        return false;
    }

    // A UDIM set, reduced to its first *existing* tile.
    //
    // `<UDIM>` is a token USD leaves in the path for the renderer to expand
    // into one texture per tile, selected by which unit square of UV space a
    // sample lands in. hdClaude has no tile selection yet, so it shades every
    // tile with one of them -- wrong for a multi-tile asset, right for the
    // many that ship one, and far better than the alternative: an unexpanded
    // token opens nothing at all.
    //
    // Which tile is found by asking, not assumed. 1001 is the first index of
    // the grid and not necessarily the first index an asset uses: the OpenPBR
    // playground's tools are authored on tile 1003, and hard-coding 1001 left
    // twenty-one of its textures unopened while looking exactly like a missing
    // TIFF decoder.
    std::string path = assetPath;
    const std::string udim = "<UDIM>";
    const std::size_t token = path.find(udim);
    if (token != std::string::npos) {
        // The 10x10 grid UDIM defines, in order.
        for (int tile = 1001; tile <= 1100; ++tile) {
            std::string candidate = path;
            candidate.replace(token, udim.size(), std::to_string(tile));
            if (ArGetResolver().Resolve(candidate)) {
                path = candidate;
                break;
            }
        }
        if (path.find(udim) != std::string::npos) {
            if (error) {
                *error = "no tile of the UDIM set '" + assetPath + "' exists";
            }
            return false;
        }
    }

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
    return Widen(image, out, error, colorSpace);
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
        image.rgba = {0, 0, 0, 255};
        image.srgb = false;
        image.debugName = "image node with no file";
        HdClaudeTrace("texture %u: unbound image node; reads its default", slot);
        _images.push_back(std::move(image));
        return slot;
    }

    if (HdClaudeLoadTexture(assetPath, &image, &error, colorSpace)) {
        HdClaudeTrace("texture %u: %s (%ux%u, %s)", slot, assetPath.c_str(),
                      image.width, image.height, image.srgb ? "sRGB" : "linear");
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
    _images.clear();
    _failures.clear();
}

PXR_NAMESPACE_CLOSE_SCOPE
