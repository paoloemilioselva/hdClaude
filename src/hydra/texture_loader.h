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
#include <memory>
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

/// When a pool slot's image is decoded.
enum class HdClaudeTextureResidency {
    /// When the renderer uploads it, which then lets the decoded copy go: what
    /// a material's textures are, since nothing on the host reads them. The
    /// slot holds the image's description and how to decode it
    /// (TextureImage::decode), so a scene's images are never all decoded at
    /// once.
    Deferred,
    /// Now, and kept: for an image the host reads -- a dome's map, whose
    /// integral sets its brightness and whose texels build its sampling
    /// distribution.
    Decoded,
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
    ///
    /// Decoded now: this is how a light asks for its map. A slot asked for
    /// both ways is decoded.
    std::uint32_t Acquire(
        const std::string& assetPath,
        HdClaudeTextureColorSpace colorSpace = HdClaudeTextureColorSpace::Auto);

    /// One image to acquire: where it is, and how to read it.
    struct Request {
        std::string assetPath;
        HdClaudeTextureColorSpace colorSpace = HdClaudeTextureColorSpace::Auto;
        /// Whether this material can read the image's alpha
        /// (TextureImage::alphaRead). A slot's image is read for its alpha if
        /// any request for it is.
        bool readsAlpha = true;
    };

    /// What one AcquireAll decoded, as opposed to found already in the pool or
    /// deferred to the upload.
    struct AcquireReport {
        std::size_t decoded = 0;
        std::uint64_t bytes = 0;
    };

    /// The pool slot for each request, in order. A new image is deferred --
    /// described, not decoded -- unless `residency` asks for it decoded, when
    /// every such image is decoded in parallel.
    ///
    /// Slots are reserved under the pool's lock and the images decoded outside
    /// it, so the decode -- seconds for a 4096-square TIFF -- runs on every
    /// core rather than one image at a time. The pool must not be read until
    /// this returns: the reserved slots hold nothing until then.
    std::vector<std::uint32_t> AcquireAll(
        const std::vector<Request>& requests, AcquireReport* report = nullptr,
        HdClaudeTextureResidency residency = HdClaudeTextureResidency::Deferred);

    /// Every loaded image, indexed by the slot Acquire returned.
    ///
    /// Shared and immutable: a snapshot copies the pointers, and an image
    /// decoded again -- under a new cap -- is a new image in the same slot,
    /// so the renderer can tell a slot that changed from one that did not.
    const std::vector<std::shared_ptr<const hdclaude::TextureImage>>& Images()
        const
    {
        return _images;
    }

    /// Paths that could not be loaded, for render stats. A deferred image is
    /// added when the renderer tries to decode it, from whichever thread did.
    std::vector<std::string> Failures() const;

    void Clear();

  private:
    using Key = std::pair<std::string, HdClaudeTextureColorSpace>;

    mutable std::mutex _mutex;
    std::map<Key, std::uint32_t> _slots;
    /// What each slot was loaded from, so a changed cap can re-decode it into
    /// the same slot.
    std::vector<Key> _sources;
    std::vector<std::shared_ptr<const hdclaude::TextureImage>> _images;
    /// Whether each slot holds a decoded image rather than a deferred one.
    std::vector<bool> _decoded;
    /// Whether any request for each slot reads its alpha.
    std::vector<bool> _alphaRead;
  public:
    /// Failures, shared with every deferred image's decoder: those run on the
    /// renderer's threads, after AcquireAll has returned, and possibly after a
    /// snapshot holding them has outlived a Clear.
    struct FailureLog;

    HdClaudeTexturePool();
    ~HdClaudeTexturePool();

  private:
    std::shared_ptr<FailureLog> _failures;
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
