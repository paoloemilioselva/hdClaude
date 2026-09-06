#include "texture_loader.h"

#include "trace.h"

#include "pxr/imaging/hio/image.h"
#include "pxr/imaging/hio/types.h"
#include "pxr/usd/ar/resolver.h"

#include <algorithm>
#include <cmath>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

/// Widen whatever Hio decoded into the tightly packed RGBA8 the GPU expects.
///
/// Asking Hio to read straight into HioFormatUNorm8Vec4 would be simpler, but
/// a plugin is free to decline a conversion, and a silently unconverted read
/// is a buffer of the wrong stride rather than an error. Reading in the file's
/// own format and widening here is the version that cannot be quietly wrong.
bool Widen(const HioImageSharedPtr& image, hdclaude::TextureImage* out,
           std::string* error)
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

    size_t bytesPerComponent = 0;
    switch (type) {
        case HioTypeUnsignedByte: bytesPerComponent = 1; break;
        case HioTypeHalfFloat:    bytesPerComponent = 2; break;
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
    out->srgb = image->IsColorSpaceSRGB();
    out->rgba.assign(static_cast<size_t>(width) * height * 4, 0);

    const size_t pixels = static_cast<size_t>(width) * height;
    for (size_t i = 0; i < pixels; ++i) {
        for (size_t c = 0; c < 4; ++c) {
            std::uint8_t value = (c == 3) ? 255 : 0;
            if (c < channels) {
                const char* source =
                    raw.data() + (i * channels + c) * bytesPerComponent;
                switch (type) {
                    case HioTypeUnsignedByte:
                        value = static_cast<std::uint8_t>(
                            *reinterpret_cast<const unsigned char*>(source));
                        break;
                    case HioTypeHalfFloat: {
                        GfHalf half;
                        std::memcpy(&half, source, sizeof(GfHalf));
                        value = static_cast<std::uint8_t>(
                            std::clamp(static_cast<float>(half), 0.0f, 1.0f) *
                                255.0f + 0.5f);
                        break;
                    }
                    case HioTypeFloat: {
                        float f = 0.0f;
                        std::memcpy(&f, source, sizeof(float));
                        value = static_cast<std::uint8_t>(
                            std::clamp(f, 0.0f, 1.0f) * 255.0f + 0.5f);
                        break;
                    }
                    default:
                        break;
                }
            } else if (c < 3 && channels == 1) {
                // A single-channel image reads as grey rather than as red,
                // which is what a greyscale roughness or mask map means.
                const char* source = raw.data() + i * bytesPerComponent;
                if (type == HioTypeUnsignedByte) {
                    value = static_cast<std::uint8_t>(
                        *reinterpret_cast<const unsigned char*>(source));
                }
            }
            out->rgba[i * 4 + c] = value;
        }
    }
    return true;
}

}  // namespace

bool HdClaudeLoadTexture(const std::string& assetPath,
                         hdclaude::TextureImage* out, std::string* error)
{
    if (assetPath.empty() || out == nullptr) {
        if (error) *error = "no asset path";
        return false;
    }

    // A UDIM set, reduced to its first tile.
    //
    // `<UDIM>` is a token USD leaves in the path for the renderer to expand
    // into one texture per tile, selected by which unit square of UV space a
    // sample lands in. hdClaude has no tile selection yet, so it loads tile
    // 1001 and shades every tile with it. That is wrong for a multi-tile
    // asset and right for the many that ship one tile, and both are better
    // than the alternative: an unexpanded token opens nothing, and the OpenPBR
    // playground rendered as 85 magenta placeholders because of it.
    std::string path = assetPath;
    const std::string udim = "<UDIM>";
    const std::size_t token = path.find(udim);
    if (token != std::string::npos) {
        path.replace(token, udim.size(), "1001");
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
    return Widen(image, out, error);
}

std::uint32_t HdClaudeTexturePool::Acquire(const std::string& assetPath)
{
    std::lock_guard<std::mutex> lock(_mutex);

    const auto found = _slots.find(assetPath);
    if (found != _slots.end()) {
        return found->second;
    }

    const auto slot = static_cast<std::uint32_t>(_images.size());
    _slots[assetPath] = slot;

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

    if (HdClaudeLoadTexture(assetPath, &image, &error)) {
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
