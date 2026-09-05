// Content hashing for cache keys and resource fingerprints.
//
// Two distinct jobs, deliberately kept as two functions:
//
//   Sha256  - cache keys that persist to disk and must not collide across
//             machines, compiler versions, or runs.
//   Fnv1a64 - in-memory fingerprints for "has this prototype changed?"
//             decisions, where speed matters and a rebuild on collision is
//             merely slow rather than wrong.

#ifndef HDCLAUDE_CORE_HASH_H
#define HDCLAUDE_CORE_HASH_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

namespace hdclaude {

using Sha256Digest = std::array<std::uint8_t, 32>;

/// SHA-256 of an arbitrary byte range.
Sha256Digest Sha256(const void* data, std::size_t size);

/// SHA-256 of a string.
Sha256Digest Sha256(std::string_view text);

/// Lowercase hexadecimal rendering of a digest, 64 characters.
std::string ToHex(const Sha256Digest& digest);

/// Incremental SHA-256, for keys assembled from many pieces without
/// concatenating them into one buffer first.
class Sha256Builder {
  public:
    Sha256Builder();

    Sha256Builder& Update(const void* data, std::size_t size);
    Sha256Builder& Update(std::string_view text);

    /// Append a length-prefixed field. Prefixing prevents the ambiguity where
    /// ("ab", "c") and ("a", "bc") would otherwise produce the same digest --
    /// a real hazard for cache keys built from several free-form strings.
    Sha256Builder& Field(std::string_view text);

    /// Append a trivially copyable value in a byte-order-stable form.
    template <typename T>
    Sha256Builder& Value(const T& value)
    {
        static_assert(std::is_trivially_copyable_v<T>,
                      "Sha256Builder::Value requires a trivially copyable type");
        return Update(&value, sizeof(T));
    }

    /// Finalize. The builder must not be used afterwards.
    Sha256Digest Finish();

  private:
    std::array<std::uint32_t, 8> _state{};
    std::array<std::uint8_t, 64> _block{};
    std::uint64_t _length = 0;
    std::size_t _pending = 0;
    bool _finished = false;

    void Compress(const std::uint8_t* block);
};

/// FNV-1a 64-bit, for in-memory fingerprints.
std::uint64_t Fnv1a64(const void* data, std::size_t size,
                      std::uint64_t seed = 0xcbf29ce484222325ULL);

/// Combine a hash into an accumulator. Not a cryptographic construction.
inline std::uint64_t HashCombine(std::uint64_t accumulator, std::uint64_t value)
{
    accumulator ^= value + 0x9e3779b97f4a7c15ULL + (accumulator << 6) +
                   (accumulator >> 2);
    return accumulator;
}

}  // namespace hdclaude

#endif  // HDCLAUDE_CORE_HASH_H
