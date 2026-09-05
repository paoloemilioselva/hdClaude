#include "hdclaude/core/hash.h"

#include <algorithm>
#include <cstring>

namespace hdclaude {
namespace {

constexpr std::uint32_t kRoundConstants[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
    0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
    0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
    0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
    0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
    0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
    0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
    0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

inline std::uint32_t RotateRight(std::uint32_t value, std::uint32_t bits)
{
    return (value >> bits) | (value << (32u - bits));
}

inline std::uint32_t LoadBigEndian32(const std::uint8_t* bytes)
{
    return (static_cast<std::uint32_t>(bytes[0]) << 24) |
           (static_cast<std::uint32_t>(bytes[1]) << 16) |
           (static_cast<std::uint32_t>(bytes[2]) << 8) |
           static_cast<std::uint32_t>(bytes[3]);
}

inline void StoreBigEndian32(std::uint8_t* bytes, std::uint32_t value)
{
    bytes[0] = static_cast<std::uint8_t>(value >> 24);
    bytes[1] = static_cast<std::uint8_t>(value >> 16);
    bytes[2] = static_cast<std::uint8_t>(value >> 8);
    bytes[3] = static_cast<std::uint8_t>(value);
}

}  // namespace

Sha256Builder::Sha256Builder()
    : _state{0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
             0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u}
{
}

void Sha256Builder::Compress(const std::uint8_t* block)
{
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = LoadBigEndian32(block + i * 4);
    }
    for (int i = 16; i < 64; ++i) {
        const std::uint32_t s0 = RotateRight(w[i - 15], 7) ^
                                 RotateRight(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 = RotateRight(w[i - 2], 17) ^
                                 RotateRight(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    std::uint32_t a = _state[0], b = _state[1], c = _state[2], d = _state[3];
    std::uint32_t e = _state[4], f = _state[5], g = _state[6], h = _state[7];

    for (int i = 0; i < 64; ++i) {
        const std::uint32_t s1 =
            RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
        const std::uint32_t ch = (e & f) ^ (~e & g);
        const std::uint32_t temp1 = h + s1 + ch + kRoundConstants[i] + w[i];
        const std::uint32_t s0 =
            RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temp2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    _state[0] += a;
    _state[1] += b;
    _state[2] += c;
    _state[3] += d;
    _state[4] += e;
    _state[5] += f;
    _state[6] += g;
    _state[7] += h;
}

Sha256Builder& Sha256Builder::Update(const void* data, std::size_t size)
{
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    _length += size;

    if (_pending > 0) {
        const std::size_t take = std::min(size, _block.size() - _pending);
        std::memcpy(_block.data() + _pending, bytes, take);
        _pending += take;
        bytes += take;
        size -= take;
        if (_pending == _block.size()) {
            Compress(_block.data());
            _pending = 0;
        }
    }

    while (size >= _block.size()) {
        Compress(bytes);
        bytes += _block.size();
        size -= _block.size();
    }

    if (size > 0) {
        std::memcpy(_block.data() + _pending, bytes, size);
        _pending += size;
    }
    return *this;
}

Sha256Builder& Sha256Builder::Update(std::string_view text)
{
    return Update(text.data(), text.size());
}

Sha256Builder& Sha256Builder::Field(std::string_view text)
{
    const auto size = static_cast<std::uint64_t>(text.size());
    Update(&size, sizeof(size));
    return Update(text);
}

Sha256Digest Sha256Builder::Finish()
{
    const std::uint64_t bitLength = _length * 8;

    // Padding: 0x80, then zeros, then the 64-bit big-endian bit length.
    std::uint8_t padding[72] = {0x80};
    const std::size_t remainder = static_cast<std::size_t>(_length % 64);
    const std::size_t padLength = (remainder < 56) ? (56 - remainder)
                                                   : (120 - remainder);
    Update(padding, padLength);

    std::uint8_t lengthBytes[8];
    StoreBigEndian32(lengthBytes, static_cast<std::uint32_t>(bitLength >> 32));
    StoreBigEndian32(lengthBytes + 4, static_cast<std::uint32_t>(bitLength));
    Update(lengthBytes, sizeof(lengthBytes));

    Sha256Digest digest{};
    for (int i = 0; i < 8; ++i) {
        StoreBigEndian32(digest.data() + i * 4, _state[static_cast<std::size_t>(i)]);
    }
    _finished = true;
    return digest;
}

Sha256Digest Sha256(const void* data, std::size_t size)
{
    Sha256Builder builder;
    builder.Update(data, size);
    return builder.Finish();
}

Sha256Digest Sha256(std::string_view text)
{
    return Sha256(text.data(), text.size());
}

std::string ToHex(const Sha256Digest& digest)
{
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string text;
    text.reserve(digest.size() * 2);
    for (const std::uint8_t byte : digest) {
        text.push_back(kDigits[byte >> 4]);
        text.push_back(kDigits[byte & 0x0f]);
    }
    return text;
}

std::uint64_t Fnv1a64(const void* data, std::size_t size, std::uint64_t seed)
{
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::uint64_t hash = seed;
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

}  // namespace hdclaude
