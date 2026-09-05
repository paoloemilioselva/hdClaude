#include "hdclaude/core/shader_cache.h"

#include "hdclaude/core/hash.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <system_error>

namespace hdclaude {
namespace {

// Bumped whenever the on-disk entry layout changes, so old entries are simply
// not found rather than mis-parsed.
constexpr std::uint32_t kEntryFormatVersion = 1;
constexpr char kEntryMagic[8] = {'H', 'D', 'C', 'L', 'S', 'P', 'V', '1'};

struct EntryHeader {
    char magic[8];
    std::uint32_t formatVersion;
    std::uint32_t wordCount;
};

/// Process-unique suffix for temporary files, so two threads or two processes
/// writing the same entry cannot collide on the temporary name.
std::string UniqueSuffix()
{
    static std::atomic<std::uint64_t> counter{0};
    return std::to_string(counter.fetch_add(1, std::memory_order_relaxed)) + "." +
           std::to_string(static_cast<std::uint64_t>(
               reinterpret_cast<std::uintptr_t>(&counter)));
}

}  // namespace

std::string ShaderCacheKey::Digest() const
{
    Sha256Builder builder;
    builder.Field(source)
        .Field(entryPoint)
        .Field(stage)
        .Field(generatorTarget)
        .Field(generatorVersion)
        .Field(compilerVersion)
        .Value(spirvVersion)
        .Value(vulkanVersion)
        .Value(abiVersion)
        .Value(static_cast<std::uint8_t>(optimize ? 1 : 0));
    return ToHex(builder.Finish());
}

ShaderCache::ShaderCache(std::filesystem::path directory)
    : _directory(std::move(directory))
{
    std::error_code error;
    std::filesystem::create_directories(_directory, error);
    // A cache directory that cannot be created is not fatal: Find will miss and
    // Store will fail, and the renderer compiles every time.
}

std::filesystem::path ShaderCache::PathFor(const ShaderCacheKey& key) const
{
    const std::string digest = key.Digest();
    // Two-character prefix directory keeps any single directory small enough
    // that lookups stay fast on filesystems that scan linearly.
    return _directory / digest.substr(0, 2) / (digest.substr(2) + ".spv");
}

std::optional<std::vector<std::uint32_t>> ShaderCache::Find(
    const ShaderCacheKey& key) const
{
    const std::filesystem::path path = PathFor(key);

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        ++_misses;
        return std::nullopt;
    }

    EntryHeader header{};
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!file || std::memcmp(header.magic, kEntryMagic, sizeof(kEntryMagic)) != 0 ||
        header.formatVersion != kEntryFormatVersion || header.wordCount == 0) {
        ++_misses;
        return std::nullopt;
    }

    std::vector<std::uint32_t> spirv(header.wordCount);
    file.read(reinterpret_cast<char*>(spirv.data()),
              static_cast<std::streamsize>(spirv.size() * sizeof(std::uint32_t)));
    if (!file) {
        ++_misses;
        return std::nullopt;
    }

    // A SPIR-V module always begins with the magic number. Checking it catches
    // a truncated or garbage entry that happened to have a plausible header.
    if (spirv.front() != 0x07230203u) {
        ++_misses;
        return std::nullopt;
    }

    ++_hits;
    return spirv;
}

bool ShaderCache::Store(const ShaderCacheKey& key,
                        const std::vector<std::uint32_t>& spirv)
{
    if (spirv.empty()) {
        return false;
    }

    const std::filesystem::path path = PathFor(key);
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        return false;
    }

    // Write to a unique temporary and rename. A reader therefore observes
    // either no entry or a complete one -- never the partially written file
    // that a direct write would expose to a concurrent process.
    const std::filesystem::path temporary =
        path.parent_path() / (path.filename().string() + "." + UniqueSuffix() + ".tmp");

    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) {
            return false;
        }

        EntryHeader header{};
        std::memcpy(header.magic, kEntryMagic, sizeof(kEntryMagic));
        header.formatVersion = kEntryFormatVersion;
        header.wordCount = static_cast<std::uint32_t>(spirv.size());

        file.write(reinterpret_cast<const char*>(&header), sizeof(header));
        file.write(reinterpret_cast<const char*>(spirv.data()),
                   static_cast<std::streamsize>(spirv.size() * sizeof(std::uint32_t)));
        if (!file) {
            file.close();
            std::filesystem::remove(temporary, error);
            return false;
        }
    }

    std::filesystem::rename(temporary, path, error);
    if (error) {
        // Another process may have won the race and created the entry first,
        // which is a success from this caller's point of view.
        std::error_code removeError;
        std::filesystem::remove(temporary, removeError);
        return std::filesystem::exists(path);
    }
    return true;
}

}  // namespace hdclaude
