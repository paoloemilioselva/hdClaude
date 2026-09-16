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
    /// Cap the longest edge of every texture, halving as many times as it
    /// takes; zero keeps every image at the size it was authored.
    ///
    /// A cap rather than a fixed number of halvings, because what a scene costs
    /// is the size its images *end at*: ALab ships 6,261 images totalling
    /// 49.6 GB, and halving each of them once still leaves 12 GB while a cap of
    /// 1024 leaves a figure that does not depend on how big the originals were.
    ///
    /// Returns true when the cap actually changed, and reloads every image
    /// already in the pool in place: the slot indices are baked into generated
    /// material code, so an image is re-decoded into the slot it already has
    /// rather than the pool being emptied and refilled.
    bool SetMaxEdge(std::uint32_t maxEdge);

    /// The current cap, zero for none.
    std::uint32_t MaxEdge() const;

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
    using Key = std::pair<std::string, HdClaudeTextureColorSpace>;

    mutable std::mutex _mutex;
    std::map<Key, std::uint32_t> _slots;
    /// What each slot was loaded from, so a changed cap can re-decode it into
    /// the same slot.
    std::vector<Key> _sources;
    std::vector<hdclaude::TextureImage> _images;
    std::vector<std::string> _failures;
    std::uint32_t _maxEdge = 0;
};

/// The longest edge a named texture quality keeps, or zero for the authored
/// image. Anything unrecognised is `high`, and the caller reports it.
///
/// Powers of two an octave apart, because each step is a halving and the
/// memory a step saves is a factor of four: 1024 keeps the detail a surface
/// filling the frame can show at the resolutions this renders at, and 256 is
/// the one for a stage that would not otherwise load.
std::uint32_t HdClaudeTextureEdgeCap(const std::string& quality);

/// Whether `quality` names one at all.
bool HdClaudeIsTextureQuality(const std::string& quality);

/// Decode one image. Exposed for testing; callers should use the pool.
///
/// `maxEdge` caps the longest edge by repeated halving, or keeps the authored
/// size when zero. The halving is a box filter, which preserves the image's
/// mean exactly -- so a dome light reduced this way still delivers the same
/// total power, spread over a coarser sky.
bool HdClaudeLoadTexture(const std::string& assetPath,
                         hdclaude::TextureImage* out, std::string* error,
                         HdClaudeTextureColorSpace colorSpace =
                             HdClaudeTextureColorSpace::Auto,
                         std::uint32_t maxEdge = 0);

PXR_NAMESPACE_CLOSE_SCOPE
