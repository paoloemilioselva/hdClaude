#pragma once

// Decoding an asset path into bytes the GPU layer can upload.
//
// This is the only place in hdClaude that reads an image file. It lives in the
// Hydra layer because OpenUSD already ships the image plugins and the asset
// resolver, and because the GPU layer must stay free of both (see
// docs/architecture.md 3).

#include "hdclaude/gpu/scene.h"

#include "pxr/pxr.h"

#include <map>
#include <mutex>
#include <string>
#include <utility>

PXR_NAMESPACE_OPEN_SCOPE

/// What a material says about a texture's encoding.
///
/// The file format alone cannot answer this. An 8-bit JPEG is sRGB-encoded if
/// it holds colour and linear if it holds a normal or a roughness, and the two
/// are indistinguishable in the file; only the material knows which it authored.
enum class HdClaudeTextureColorSpace {
    /// Nothing was declared, so the file's own encoding stands.
    Auto,
    /// Declared sRGB-encoded colour.
    Srgb,
    /// Declared linear data: a normal, roughness, metalness or mask map.
    Raw,
};

/// Loads textures once each and hands out their pool indices.
///
/// Materials name their textures by asset path, and several materials in a
/// scene routinely name the same one. Keying the pool on the resolved path
/// means an image is decoded and uploaded once however many materials sample
/// it, and it is what makes a material's local texture index resolvable to a
/// shared slot.
class HdClaudeTexturePool {
  public:
    /// The pool slot for an asset path, loading it if this is the first ask.
    ///
    /// A path that cannot be resolved or decoded still gets a slot, holding an
    /// invalid image. The renderer binds its placeholder there, so a missing
    /// texture is visibly wrong rather than silently black, and the material's
    /// other indices are unaffected.
    /// The same path can be asked for twice with different colour spaces --
    /// an asset may use one image as both colour and data -- so the pool is
    /// keyed on both and decodes each reading separately.
    std::uint32_t Acquire(
        const std::string& assetPath,
        HdClaudeTextureColorSpace colorSpace = HdClaudeTextureColorSpace::Auto);

    /// Every loaded image, indexed by the slot Acquire returned.
    const std::vector<hdclaude::TextureImage>& Images() const { return _images; }

    /// Paths that could not be loaded, for render stats.
    const std::vector<std::string>& Failures() const { return _failures; }

    void Clear();

  private:
    mutable std::mutex _mutex;
    std::map<std::pair<std::string, HdClaudeTextureColorSpace>, std::uint32_t>
        _slots;
    std::vector<hdclaude::TextureImage> _images;
    std::vector<std::string> _failures;
};

/// Decode one image. Exposed for testing; callers should use the pool.
bool HdClaudeLoadTexture(const std::string& assetPath,
                         hdclaude::TextureImage* out, std::string* error,
                         HdClaudeTextureColorSpace colorSpace =
                             HdClaudeTextureColorSpace::Auto);

PXR_NAMESPACE_CLOSE_SCOPE
