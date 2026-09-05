// Content-addressed SPIR-V cache.
//
// MaterialX generates GLSL at runtime for every bound material, so shader
// compilation is on the critical path of the first frame rather than a
// build-time step. This cache makes the second run of a scene fast and, more
// importantly, makes it *correct*: the key covers every input that could change
// the produced SPIR-V, so a stale entry cannot be served after a toolchain,
// generator, or ABI change.

#ifndef HDCLAUDE_CORE_SHADER_CACHE_H
#define HDCLAUDE_CORE_SHADER_CACHE_H

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace hdclaude {

/// Every input that can change generated SPIR-V.
///
/// Adding a field here is how a semantic change is made to invalidate the
/// cache. Omitting one is how a stale entry gets served -- so the rule is that
/// anything the generator or compiler reads belongs in this struct.
struct ShaderCacheKey {
    std::string source;             ///< the generated GLSL, verbatim
    std::string entryPoint;
    std::string stage;              ///< "compute", "vertex", ...
    std::string generatorTarget;    ///< MaterialX target, e.g. "genglsl_pt"
    std::string generatorVersion;   ///< MaterialX library version
    std::string compilerVersion;    ///< glslang version
    std::uint32_t spirvVersion = 0; ///< encoded SPIR-V target version
    std::uint32_t vulkanVersion = 0;///< encoded Vulkan target version
    std::uint32_t abiVersion = 0;   ///< hdClaude material ABI revision
    bool optimize = false;

    /// Hexadecimal SHA-256 over all fields, length-prefixed so that two
    /// different field splits cannot produce the same key.
    std::string Digest() const;
};

/// A disk-backed SPIR-V cache. Safe to use from multiple processes: entries are
/// written to a unique temporary file and atomically renamed into place, so a
/// concurrent reader sees either no entry or a complete one, never a partial.
class ShaderCache {
  public:
    explicit ShaderCache(std::filesystem::path directory);

    /// Look up compiled SPIR-V. Returns nothing on a miss, on a corrupt entry,
    /// or on any I/O error -- a cache failure must degrade to recompilation
    /// and never to a rendering failure.
    std::optional<std::vector<std::uint32_t>> Find(const ShaderCacheKey& key) const;

    /// Store compiled SPIR-V. Returns false if the entry could not be written;
    /// the caller continues regardless, for the same reason.
    bool Store(const ShaderCacheKey& key, const std::vector<std::uint32_t>& spirv);

    const std::filesystem::path& Directory() const { return _directory; }

    /// Counters for tests and render statistics.
    std::uint64_t HitCount() const { return _hits; }
    std::uint64_t MissCount() const { return _misses; }

  private:
    std::filesystem::path PathFor(const ShaderCacheKey& key) const;

    std::filesystem::path _directory;
    mutable std::uint64_t _hits = 0;
    mutable std::uint64_t _misses = 0;
};

}  // namespace hdclaude

#endif  // HDCLAUDE_CORE_SHADER_CACHE_H
