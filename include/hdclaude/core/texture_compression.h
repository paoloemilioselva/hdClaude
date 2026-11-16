// Block compression of textures: BC4, BC5, BC6H and BC7 encoders.
//
// A texture compressed this way occupies a quarter to an eighth of the device
// memory it takes uncompressed, and is decoded by the sampler in hardware, so
// nothing in a shader changes. It is lossy: every 4x4 block is reduced to two
// endpoints and an index per texel, and what the sampler returns is the
// nearest of the levels between them.
//
// Each encoder writes one mode of its format, chosen as the single-subset mode
// with the most index precision:
//
//   BC4   one channel, 8 bytes a block      two 8-bit endpoints, 3-bit indices
//   BC5   two channels, 16 bytes a block    two BC4 blocks
//   BC6H  RGB half, 16 bytes a block        mode 11: 10-bit endpoints, 4-bit
//                                           indices; unsigned values only
//   BC7   RGBA 8-bit, 16 bytes a block      mode 6: 7-bit endpoints with a
//                                           p-bit each, 4-bit indices
//
// Endpoints come from the block's principal axis and are then refitted to the
// indices by least squares; indices are chosen against the exact palette the
// format decodes to. Multi-subset modes, which handle a block holding two
// unrelated colours better, are not written. Pure CPU code: no device, no
// codec, so the core tests decode its blocks with an independent decoder.

#ifndef HDCLAUDE_CORE_TEXTURE_COMPRESSION_H
#define HDCLAUDE_CORE_TEXTURE_COMPRESSION_H

#include <cstdint>
#include <vector>

namespace hdclaude {

/// One 4x4 block of one channel, row by row.
void EncodeBc4Block(const std::uint8_t values[16], std::uint8_t block[8]);

/// One 4x4 block of two channels: `values` is 16 (r, g) pairs, row by row.
void EncodeBc5Block(const std::uint8_t values[32], std::uint8_t block[16]);

/// One 4x4 block of RGBA bytes, row by row. The bytes are encoded as they are,
/// so an sRGB-encoded block is compressed in its encoding, as BC7_SRGB
/// expects.
void EncodeBc7Block(const std::uint8_t rgba[64], std::uint8_t block[16]);

/// One 4x4 block of RGB half floats (their bit patterns), row by row. Every
/// value must be finite and not negative: BC6H_UFLOAT has no sign, and the
/// caller decides what to do with an image that does.
void EncodeBc6hBlock(const std::uint16_t rgb[48], std::uint8_t block[16]);

/// A whole image, in blocks, row of blocks after row of blocks, in the same row
/// order as `texels`. An edge that is not a multiple of four repeats its last
/// row or column into the block, which the sampler never reads.
///
/// `texels` is tightly packed with `channels` components a texel: one byte
/// each for BC4 (1), BC5 (2) and BC7 (4), one half each for BC6H (3).
enum class BlockEncoding : std::uint8_t { Bc4, Bc5, Bc6h, Bc7 };
std::vector<std::uint8_t> EncodeBlocks(BlockEncoding encoding, const void* texels,
                                       std::uint32_t width, std::uint32_t height);

/// Bytes one block of `encoding` occupies.
inline constexpr std::uint32_t BlockBytes(BlockEncoding encoding)
{
    return encoding == BlockEncoding::Bc4 ? 8u : 16u;
}

}  // namespace hdclaude

#endif  // HDCLAUDE_CORE_TEXTURE_COMPRESSION_H
