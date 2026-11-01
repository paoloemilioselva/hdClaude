#pragma once

// Decoding a DirectDraw Surface (.dds) into a texture the GPU layer can upload.
//
// Every other image format reaches hdClaude through Hio, but no Hio plugin in
// the OpenUSD distributions hdClaude builds against reads DDS: the built-in
// reader covers PNG, JPEG, BMP, TGA and HDR, and hioOiio registers only TIFF
// and TX. So the container is parsed here and its blocks decompressed with
// bcdec (cmake/Dependencies.cmake).
//
// Separate from texture_loader.cpp so it can be tested on bytes built in
// memory, with no file, resolver or image plugin involved.

#include "texture_loader.h"

#include "hdclaude/gpu/scene.h"

#include "pxr/pxr.h"

#include <cstddef>
#include <string>

PXR_NAMESPACE_OPEN_SCOPE

/// Whether `bytes` start with the DDS magic number.
bool HdClaudeIsDds(const void* bytes, std::size_t size);

/// Decode the top-level image of a DDS file held in memory.
///
/// What is read:
///   - BC1, BC2, BC3, BC4, BC5 and BC7 (DXT1, DXT3, DXT5, ATI1, ATI2), into
///     eight bits a channel;
///   - BC6H, signed and unsigned, into half floats -- it is an HDR format and
///     clamping it would throw away what it is for;
///   - uncompressed images described by channel masks (RGBA8, BGRA8, BGRX8,
///     RGB565, RGB5A1, RGBA4, R10G10B10A2, R8, R8G8, L8, A8L8, A8, R16, R16G16);
///   - uncompressed 16-bit unsigned-normalised, half and float images of one,
///     two or four channels, and float of three.
///
/// What is refused, with a reason in `error`: cube maps, volumes and texture
/// arrays (a texture node samples one 2D image); signed-normalised formats;
/// premultiplied DXT2 and DXT4; YUV and anything else not listed above.
///
/// Only the first mip level is read. The pool reduces images itself, with a
/// filter that respects sRGB, and a file's own mips are not guaranteed to.
///
/// Encoding follows the same rule as the Hio path: `colorSpace` decides when
/// the material declared one, and under `Auto` eight-bit colour (BC1, BC2,
/// BC3, BC7, RGBA8 and the like) is sRGB and everything else linear -- as Hio
/// would read the same pixels from a PNG. A DX10 `_SRGB` suffix is not
/// consulted: tools write colour maps as `BC7_UNORM` unless asked otherwise,
/// so taking `UNORM` at its word would wash out most DDS albedo maps.
///
/// Rows come out bottom-up, as TextureImage requires; DDS stores them top-down.
bool HdClaudeDecodeDds(const void* bytes, std::size_t size,
                       HdClaudeTextureColorSpace colorSpace,
                       hdclaude::TextureImage* out, std::string* error);

PXR_NAMESPACE_CLOSE_SCOPE
