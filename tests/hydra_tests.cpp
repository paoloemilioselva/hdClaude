// Hydra adapter classes that can be exercised without a render index, a stage,
// or a GPU: built from the adapter's own sources against OpenUSD, so a defect
// in what a host reads back is found here rather than as a black viewport.

#include "test_support.h"

#include "dds_image.h"
#include "instancer.h"
#include "render_buffer.h"

#include "pxr/base/gf/half.h"
#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/gf/quath.h"
#include "pxr/base/gf/rotation.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/gf/vec3i.h"
#include "pxr/imaging/hd/changeTracker.h"
#include "pxr/imaging/hd/sceneDelegate.h"
#include "pxr/imaging/hd/tokens.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <map>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

constexpr int kWidth = 3;
constexpr int kHeight = 2;
constexpr int kPixels = kWidth * kHeight;

/// A frame whose every pixel differs, with fractional values and values above
/// one, which a buffer that quantised or clamped would lose.
std::vector<float> TestFrame()
{
    std::vector<float> frame(kPixels * 4);
    for (int i = 0; i < kPixels; ++i) {
        frame[i * 4 + 0] = 0.25f + 0.5f * static_cast<float>(i);
        frame[i * 4 + 1] = 3.5f;
        frame[i * 4 + 2] = 0.125f;
        frame[i * 4 + 3] = 0.5f;
    }
    return frame;
}

/// Allocated in place: a render buffer is neither copyable nor movable.
void Allocate(HdClaudeRenderBuffer& buffer, HdFormat format)
{
    CHECK(buffer.Allocate(GfVec3i(kWidth, kHeight, 1), format, false));
}

float HalfAt(HdClaudeRenderBuffer& buffer, int index)
{
    GfHalf half;
    std::memcpy(&half, static_cast<const char*>(buffer.Map()) +
                           index * sizeof(GfHalf),
                sizeof(GfHalf));
    buffer.Unmap();
    return static_cast<float>(half);
}

bool AllZero(HdClaudeRenderBuffer& buffer)
{
    const auto* bytes = static_cast<const unsigned char*>(buffer.Map());
    const size_t size = kPixels * HdDataSizeOfFormat(buffer.GetFormat());
    bool zero = true;
    for (size_t i = 0; i < size; ++i) {
        zero = zero && bytes[i] == 0;
    }
    buffer.Unmap();
    return zero;
}

void TestColorWritesFloat32()
{
    HdClaudeRenderBuffer buffer(SdfPath("/aov"));
    Allocate(buffer, HdFormatFloat32Vec4);
    CHECK(buffer.HoldsNoFrame());
    const std::vector<float> frame = TestFrame();
    CHECK(buffer.Write(frame));
    CHECK(!buffer.HoldsNoFrame());
    const auto* values = static_cast<const float*>(buffer.Map());
    for (int i = 0; i < kPixels * 4; ++i) {
        CHECK_EQ(values[i], frame[i]);
    }
    buffer.Unmap();
}

void TestColorWritesFloat16()
{
    // The colour a host asking for half-float receives: the frame, linear and
    // unclamped, not the zeros of its allocation.
    HdClaudeRenderBuffer buffer(SdfPath("/aov"));
    Allocate(buffer, HdFormatFloat16Vec4);
    const std::vector<float> frame = TestFrame();
    CHECK(buffer.Write(frame));
    CHECK(!buffer.HoldsNoFrame());
    for (int i = 0; i < kPixels * 4; ++i) {
        // Half precision has eleven significant bits.
        CHECK_NEAR(HalfAt(buffer, i), frame[i], frame[i] * 1e-3);
    }
}

void TestColorUNorm8EncodesColourNotAlpha()
{
    HdClaudeRenderBuffer buffer(SdfPath("/aov"));
    Allocate(buffer, HdFormatUNorm8Vec4);
    std::vector<float> frame(kPixels * 4, 0.5f);
    CHECK(buffer.Write(frame));
    const auto* bytes = static_cast<const unsigned char*>(buffer.Map());
    // sRGB(0.5) = 0.7354, so 188 for colour; alpha is stored as it is.
    CHECK_EQ(static_cast<int>(bytes[0]), 188);
    CHECK_EQ(static_cast<int>(bytes[1]), 188);
    CHECK_EQ(static_cast<int>(bytes[2]), 188);
    CHECK_EQ(static_cast<int>(bytes[3]), 128);
    buffer.Unmap();
}

void TestColorRefusesAFormatItCannotFill()
{
    HdClaudeRenderBuffer buffer(SdfPath("/aov"));
    Allocate(buffer, HdFormatSNorm8Vec4);
    CHECK(!buffer.Write(TestFrame()));
    // Not written, and not reported as written: a later frame, or a later
    // binding, still sees a buffer that needs one.
    CHECK(buffer.HoldsNoFrame());
    CHECK(AllZero(buffer));
}

void TestClearFillsFloat16()
{
    HdClaudeRenderBuffer buffer(SdfPath("/aov"));
    Allocate(buffer, HdFormatFloat16Vec4);
    const float value[4] = {0.25f, 2.0f, 0.0f, 1.0f};
    buffer.Clear(value);
    CHECK(!buffer.HoldsNoFrame());
    for (int i = 0; i < kPixels; ++i) {
        for (int c = 0; c < 4; ++c) {
            CHECK_EQ(HalfAt(buffer, i * 4 + c), value[c]);
        }
    }
}

void TestClearOfAFormatItCannotFillHoldsNoFrame()
{
    HdClaudeRenderBuffer buffer(SdfPath("/aov"));
    Allocate(buffer, HdFormatSNorm8Vec4);
    const float value[4] = {0.5f, 0.5f, 0.5f, 1.0f};
    buffer.Clear(value);
    CHECK(buffer.HoldsNoFrame());
    CHECK(AllZero(buffer));
}

void TestReallocationDiscardsTheFrame()
{
    HdClaudeRenderBuffer buffer(SdfPath("/aov"));
    Allocate(buffer, HdFormatFloat32Vec4);
    CHECK(buffer.Write(TestFrame()));
    CHECK(buffer.Allocate(GfVec3i(kWidth, kHeight, 1), HdFormatFloat16Vec4,
                          false));
    CHECK(buffer.HoldsNoFrame());
    CHECK(AllZero(buffer));
    CHECK(buffer.Write(TestFrame()));
    CHECK_NEAR(HalfAt(buffer, 1), 3.5f, 1e-3);
}


// ---------------------------------------------------------------------------
// Instancer
// ---------------------------------------------------------------------------

/// A scene delegate holding one instancer's primvars and nothing else. No
/// render index: a primvar sync of an instancer with no parent never asks
/// for one.
class InstancerDelegate final : public HdSceneDelegate {
  public:
    InstancerDelegate() : HdSceneDelegate(nullptr, SdfPath("/delegate")) {}

    /// The authored primvars. An entry holding an empty value is a blocked
    /// primvar: still described, resolving to nothing.
    std::map<TfToken, VtValue> primvars;

    HdPrimvarDescriptorVector GetPrimvarDescriptors(
        const SdfPath& /*id*/, HdInterpolation interpolation) override
    {
        HdPrimvarDescriptorVector descriptors;
        if (interpolation == HdInterpolationInstance) {
            for (const auto& [name, value] : primvars) {
                descriptors.emplace_back(name, interpolation);
            }
        }
        return descriptors;
    }
    VtValue Get(const SdfPath& /*id*/, const TfToken& key) override
    {
        const auto found = primvars.find(key);
        return found == primvars.end() ? VtValue() : found->second;
    }
    GfMatrix4d GetInstancerTransform(const SdfPath& /*id*/) override
    {
        return GfMatrix4d(1.0);
    }
    VtIntArray GetInstanceIndices(const SdfPath& /*instancerId*/,
                                  const SdfPath& /*prototypeId*/) override
    {
        return VtIntArray{0, 1};
    }
};

const SdfPath kInstancer("/instancer");
const SdfPath kPrototype("/instancer/prototype");

VtMatrix4dArray SyncedTransforms(HdClaudeInstancer& instancer,
                                 InstancerDelegate& delegate)
{
    HdDirtyBits bits = HdChangeTracker::DirtyPrimvar;
    instancer.Sync(&delegate, nullptr, &bits);
    return instancer.ComputeInstanceTransforms(kPrototype);
}

/// What an instancer created fresh with `delegate`'s primvars computes: the
/// transforms an edit that reaches the same state must agree with.
VtMatrix4dArray FreshTransforms(InstancerDelegate& delegate)
{
    HdClaudeInstancer fresh(&delegate, kInstancer);
    return SyncedTransforms(fresh, delegate);
}

bool SameTransforms(const VtMatrix4dArray& a, const VtMatrix4dArray& b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                if (std::fabs(a[i][r][c] - b[i][r][c]) > 1e-9) {
                    return false;
                }
            }
        }
    }
    return true;
}

void Author(InstancerDelegate& delegate)
{
    delegate.primvars[HdInstancerTokens->instanceTranslations] =
        VtValue(VtVec3fArray{GfVec3f(1, 2, 3), GfVec3f(-4, 0, 1)});
    delegate.primvars[HdInstancerTokens->instanceRotations] =
        VtValue(VtQuathArray{
            GfQuath(1, 0, 0, 0),
            GfQuath(GfRotation(GfVec3d(0, 1, 0), 60).GetQuat())});
    delegate.primvars[HdInstancerTokens->instanceScales] =
        VtValue(VtVec3fArray{GfVec3f(2, 2, 2), GfVec3f(0.5f, 1, 3)});
    GfMatrix4d sheared(1.0);
    sheared[1][0] = 0.25;
    delegate.primvars[HdInstancerTokens->instanceTransforms] =
        VtValue(VtMatrix4dArray{GfMatrix4d(1.0), sheared});
}

/// Each instance primvar removed, and separately blocked, after a frame was
/// computed with it: the edited instancer must agree with a new one built
/// from the final state, and must differ from what it computed before, or
/// the comparison saw nothing.
void TestInstancerForgetsRemovedPrimvars()
{
    for (const TfToken& name : {HdInstancerTokens->instanceScales,
                                HdInstancerTokens->instanceRotations,
                                HdInstancerTokens->instanceTransforms}) {
        for (const bool block : {false, true}) {
            InstancerDelegate delegate;
            Author(delegate);
            HdClaudeInstancer instancer(&delegate, kInstancer);
            const VtMatrix4dArray before = SyncedTransforms(instancer, delegate);

            if (block) {
                delegate.primvars[name] = VtValue();
            } else {
                delegate.primvars.erase(name);
            }
            const VtMatrix4dArray after = SyncedTransforms(instancer, delegate);
            const VtMatrix4dArray expected = FreshTransforms(delegate);
            hdclaude_test::Report(SameTransforms(after, expected),
                                  "edited instancer == fresh instancer",
                                  __FILE__, __LINE__,
                                  name.GetString() +
                                      (block ? " blocked" : " removed"));
            CHECK(!SameTransforms(before, expected));
        }
    }
}

/// Reauthored after removal, and left alone by a sync that dirtied nothing
/// of its primvars: the unchanged entries survive.
void TestInstancerKeepsWhatIsStillAuthored()
{
    InstancerDelegate delegate;
    Author(delegate);
    HdClaudeInstancer instancer(&delegate, kInstancer);
    const VtMatrix4dArray authored = SyncedTransforms(instancer, delegate);

    const VtValue scales = delegate.primvars[HdInstancerTokens->instanceScales];
    delegate.primvars.erase(HdInstancerTokens->instanceScales);
    SyncedTransforms(instancer, delegate);
    delegate.primvars[HdInstancerTokens->instanceScales] = scales;
    CHECK(SameTransforms(SyncedTransforms(instancer, delegate), authored));

    HdDirtyBits clean = HdChangeTracker::Clean;
    instancer.Sync(&delegate, nullptr, &clean);
    CHECK(SameTransforms(instancer.ComputeInstanceTransforms(kPrototype),
                         authored));
}


// --- DDS ------------------------------------------------------------------
//
// Files built byte by byte, so each test states exactly which header field and
// which block it is about, and no image asset has to travel with the tests.

void PutU32(std::vector<std::uint8_t>& bytes, std::size_t at, std::uint32_t value)
{
    for (std::size_t i = 0; i < 4; ++i) {
        bytes[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
    }
}

void AppendU16(std::vector<std::uint8_t>& bytes, std::uint16_t value)
{
    bytes.push_back(static_cast<std::uint8_t>(value & 0xff));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8));
}

constexpr std::uint32_t kDdsFourCCFlag = 0x4;
constexpr std::uint32_t kDdsRgbFlag = 0x40;
constexpr std::uint32_t kDdsAlphaPixelsFlag = 0x1;
constexpr std::uint32_t kDdsLuminanceFlag = 0x20000;

constexpr std::uint32_t DdsFourCC(const char (&name)[5])
{
    return static_cast<std::uint32_t>(name[0]) |
           static_cast<std::uint32_t>(name[1]) << 8 |
           static_cast<std::uint32_t>(name[2]) << 16 |
           static_cast<std::uint32_t>(name[3]) << 24;
}

/// The magic number and a legacy header; the pixel data is appended after.
std::vector<std::uint8_t> LegacyDds(std::uint32_t width, std::uint32_t height,
                                    std::uint32_t pixelFlags,
                                    std::uint32_t fourCC = 0,
                                    std::uint32_t bits = 0,
                                    std::initializer_list<std::uint32_t> masks = {})
{
    std::vector<std::uint8_t> bytes(128, 0);
    PutU32(bytes, 0, DdsFourCC("DDS "));
    PutU32(bytes, 4, 124);
    PutU32(bytes, 8, 0x1007);   // CAPS | HEIGHT | WIDTH | PIXELFORMAT
    PutU32(bytes, 12, height);
    PutU32(bytes, 16, width);
    PutU32(bytes, 4 + 72, 32);
    PutU32(bytes, 4 + 76, pixelFlags);
    PutU32(bytes, 4 + 80, fourCC);
    PutU32(bytes, 4 + 84, bits);
    std::size_t at = 4 + 88;
    for (const std::uint32_t mask : masks) {
        PutU32(bytes, at, mask);
        at += 4;
    }
    PutU32(bytes, 4 + 104, 0x1000);   // DDSCAPS_TEXTURE
    return bytes;
}

/// A legacy header followed by a DX10 header naming `dxgiFormat`.
std::vector<std::uint8_t> Dx10Dds(std::uint32_t width, std::uint32_t height,
                                  std::uint32_t dxgiFormat,
                                  std::uint32_t miscFlags = 0,
                                  std::uint32_t arraySize = 1)
{
    std::vector<std::uint8_t> bytes =
        LegacyDds(width, height, kDdsFourCCFlag, DdsFourCC("DX10"));
    bytes.resize(148, 0);
    PutU32(bytes, 128, dxgiFormat);
    PutU32(bytes, 132, 3);   // D3D10_RESOURCE_DIMENSION_TEXTURE2D
    PutU32(bytes, 136, miscFlags);
    PutU32(bytes, 140, arraySize);
    return bytes;
}

/// A BC1 block of one flat colour: both endpoints `rgb565`, every index 0.
void AppendFlatBc1(std::vector<std::uint8_t>& bytes, std::uint16_t rgb565)
{
    AppendU16(bytes, rgb565);
    AppendU16(bytes, rgb565);
    for (int i = 0; i < 4; ++i) {
        bytes.push_back(0);
    }
}

bool DecodeDds(const std::vector<std::uint8_t>& bytes, hdclaude::TextureImage* image,
               HdClaudeTextureColorSpace colorSpace = HdClaudeTextureColorSpace::Auto,
               std::string* error = nullptr)
{
    std::string ignored;
    return HdClaudeDecodeDds(bytes.data(), bytes.size(), colorSpace, image,
                             error ? error : &ignored);
}

/// RGBA8 at (x, y), y counted from the bottom row as TextureImage stores it.
const std::uint8_t* Texel8(const hdclaude::TextureImage& image, std::uint32_t x,
                           std::uint32_t y)
{
    return &image.texels[(static_cast<std::size_t>(y) * image.width + x) * 4];
}

float TexelHalf(const hdclaude::TextureImage& image, std::uint32_t x,
                std::uint32_t y, std::uint32_t c)
{
    GfHalf half;
    std::memcpy(&half,
                &image.texels[((static_cast<std::size_t>(y) * image.width + x) * 4 + c) * 2],
                sizeof(half));
    return static_cast<float>(half);
}

/// Legacy A8R8G8B8: the masks, not a fixed order, decide which byte is red,
/// and the file's top row ends up as the image's last.
void TestDdsReadsMaskedPixelsBottomUp()
{
    std::vector<std::uint8_t> bytes =
        LegacyDds(2, 2, kDdsRgbFlag | kDdsAlphaPixelsFlag, 0, 32,
                  {0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000});
    // Stored B, G, R, A in memory. Top row: red, green. Bottom row: blue, grey.
    for (const std::uint8_t byte : {0, 0, 255, 255,   0, 255, 0, 128,
                                    255, 0, 0, 255,   64, 64, 64, 0}) {
        bytes.push_back(byte);
    }

    hdclaude::TextureImage image;
    CHECK(HdClaudeIsDds(bytes.data(), bytes.size()));
    CHECK(DecodeDds(bytes, &image));
    CHECK(image.Valid());
    CHECK_EQ(image.width, 2u);
    CHECK_EQ(image.height, 2u);
    // Row 0 is the bottom of the image: blue then grey.
    CHECK_EQ(Texel8(image, 0, 0)[2], 255);
    CHECK_EQ(Texel8(image, 0, 0)[0], 0);
    CHECK_EQ(Texel8(image, 1, 0)[0], 64);
    CHECK_EQ(Texel8(image, 1, 0)[3], 0);
    // Row 1 is the top: red then half-transparent green.
    CHECK_EQ(Texel8(image, 0, 1)[0], 255);
    CHECK_EQ(Texel8(image, 0, 1)[1], 0);
    CHECK_EQ(Texel8(image, 1, 1)[1], 255);
    CHECK_EQ(Texel8(image, 1, 1)[3], 128);
    // Legacy eight-bit colour carries no encoding of its own and reads as
    // sRGB, as the same pixels from a PNG would; a material can overrule it.
    CHECK(image.format == hdclaude::TexelFormat::Rgba8Srgb);
    CHECK(DecodeDds(bytes, &image, HdClaudeTextureColorSpace::Raw));
    CHECK(image.format == hdclaude::TexelFormat::Rgba8Unorm);
}

/// One channel is grey, and an alpha mask without the flag that says alpha is
/// present is a leftover and ignored.
void TestDdsReadsLuminanceAsGrey()
{
    std::vector<std::uint8_t> bytes =
        LegacyDds(1, 1, kDdsLuminanceFlag, 0, 8, {0xff, 0, 0, 0xff});
    bytes.push_back(200);

    hdclaude::TextureImage image;
    CHECK(DecodeDds(bytes, &image));
    CHECK_EQ(Texel8(image, 0, 0)[0], 200);
    CHECK_EQ(Texel8(image, 0, 0)[1], 200);
    CHECK_EQ(Texel8(image, 0, 0)[2], 200);
    CHECK_EQ(Texel8(image, 0, 0)[3], 255);
    CHECK(image.format == hdclaude::TexelFormat::Rgba8Unorm);
}

/// Under Auto a DX10 header's UNORM is not taken as "linear": tools write
/// colour maps as BC7_UNORM by default, so eight-bit colour is sRGB whether the
/// suffix is _SRGB or not, exactly as for a PNG, and the material decides.
void TestDdsEightBitColourIsSrgbUnlessDeclared()
{
    for (const std::uint32_t format : {28u, 29u, 87u}) {   // RGBA8, _SRGB, BGRA8
        std::vector<std::uint8_t> bytes = Dx10Dds(1, 1, format);
        for (const std::uint8_t byte : {10, 20, 30, 40}) {
            bytes.push_back(byte);
        }
        hdclaude::TextureImage image;
        CHECK(DecodeDds(bytes, &image));
        CHECK_EQ(Texel8(image, 0, 0)[0], format == 87 ? 30 : 10);
        CHECK_EQ(Texel8(image, 0, 0)[3], 40);
        CHECK(image.format == hdclaude::TexelFormat::Rgba8Srgb);
        CHECK(DecodeDds(bytes, &image, HdClaudeTextureColorSpace::Raw));
        CHECK(image.format == hdclaude::TexelFormat::Rgba8Unorm);
    }
}

/// BC1 blocks in a size that is not a multiple of four: the file holds whole
/// blocks, the image only the pixels inside its edge, and the top block row
/// becomes the top of the image.
void TestDdsDecodesBc1Blocks()
{
    // Six by six: two blocks across, two down. Top blocks red, bottom blue.
    std::vector<std::uint8_t> bytes =
        LegacyDds(6, 6, kDdsFourCCFlag, DdsFourCC("DXT1"));
    AppendFlatBc1(bytes, 0xf800);
    AppendFlatBc1(bytes, 0xf800);
    AppendFlatBc1(bytes, 0x001f);
    AppendFlatBc1(bytes, 0x001f);

    hdclaude::TextureImage image;
    CHECK(DecodeDds(bytes, &image));
    CHECK(image.Valid());
    CHECK_EQ(image.width, 6u);
    CHECK_EQ(image.height, 6u);
    // The bottom block row spans file rows 4-5, which are image rows 0-1.
    for (std::uint32_t y = 0; y < 6; ++y) {
        const bool top = y >= 2;
        for (std::uint32_t x = 0; x < 6; ++x) {
            const std::uint8_t* texel = Texel8(image, x, y);
            CHECK_EQ(texel[0], top ? 255 : 0);
            CHECK_EQ(texel[2], top ? 0 : 255);
            CHECK_EQ(texel[3], 255);
        }
    }
    CHECK(image.format == hdclaude::TexelFormat::Rgba8Srgb);
}

/// BC4 is one channel of data: grey, and linear even under Auto.
void TestDdsDecodesBc4AsLinearGrey()
{
    std::vector<std::uint8_t> bytes =
        LegacyDds(4, 4, kDdsFourCCFlag, DdsFourCC("ATI1"));
    bytes.push_back(200);   // red0
    bytes.push_back(0);     // red1
    for (int i = 0; i < 6; ++i) {
        bytes.push_back(0); // every index selects red0
    }

    hdclaude::TextureImage image;
    CHECK(DecodeDds(bytes, &image));
    CHECK_EQ(Texel8(image, 3, 3)[0], 200);
    CHECK_EQ(Texel8(image, 3, 3)[1], 200);
    CHECK_EQ(Texel8(image, 3, 3)[2], 200);
    CHECK(image.format == hdclaude::TexelFormat::Rgba8Unorm);
}

/// Float formats keep what is above one: half RGBA from a DX10 header, and a
/// one-channel float from a legacy D3DFMT code.
void TestDdsKeepsFloatRange()
{
    std::vector<std::uint8_t> half = Dx10Dds(1, 1, 10);   // R16G16B16A16_FLOAT
    for (const float value : {4.0f, 0.5f, 0.0f, 1.0f}) {
        AppendU16(half, GfHalf(value).bits());
    }
    hdclaude::TextureImage image;
    CHECK(DecodeDds(half, &image, HdClaudeTextureColorSpace::Srgb));
    CHECK(image.format == hdclaude::TexelFormat::Rgba16Sfloat);
    CHECK_NEAR(TexelHalf(image, 0, 0, 0), 4.0, 1e-3);
    CHECK_NEAR(TexelHalf(image, 0, 0, 1), 0.5, 1e-3);

    std::vector<std::uint8_t> single = LegacyDds(1, 1, kDdsFourCCFlag, 114);
    const float value = 12.5f;
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    single.resize(single.size() + 4);
    PutU32(single, single.size() - 4, bits);
    CHECK(DecodeDds(single, &image));
    CHECK(image.format == hdclaude::TexelFormat::Rgba16Sfloat);
    CHECK_NEAR(TexelHalf(image, 0, 0, 0), 12.5, 1e-2);
    CHECK_NEAR(TexelHalf(image, 0, 0, 2), 12.5, 1e-2);
    CHECK_NEAR(TexelHalf(image, 0, 0, 3), 1.0, 1e-3);
}

/// What is refused is refused with a reason, never decoded as something else.
void TestDdsRefusesWhatItCannotRead()
{
    const auto refused = [](const std::vector<std::uint8_t>& bytes,
                            const char* expect) {
        hdclaude::TextureImage image;
        std::string error;
        const bool ok = DecodeDds(bytes, &image, HdClaudeTextureColorSpace::Auto,
                                  &error);
        hdclaude_test::Report(!ok && error.find(expect) != std::string::npos,
                              "refused with a reason", __FILE__, __LINE__,
                              std::string("expected '") + expect + "', got '" +
                                  error + "'");
    };

    std::vector<std::uint8_t> cube =
        LegacyDds(4, 4, kDdsFourCCFlag, DdsFourCC("DXT1"));
    PutU32(cube, 4 + 108, 0x200 | 0xfc00);   // CUBEMAP and all six faces
    cube.resize(cube.size() + 8 * 6, 0);
    refused(cube, "cube");

    refused(Dx10Dds(4, 4, 71, 0x4), "cube");
    refused(Dx10Dds(4, 4, 71, 0, 3), "array");
    refused(LegacyDds(4, 4, kDdsFourCCFlag, DdsFourCC("DXT2")), "premultiplied");
    refused(Dx10Dds(4, 4, 84), "signed");

    // A header promising more than the file holds.
    std::vector<std::uint8_t> truncated =
        LegacyDds(8, 8, kDdsFourCCFlag, DdsFourCC("DXT1"));
    AppendFlatBc1(truncated, 0xffff);
    refused(truncated, "shorter");

    const std::vector<std::uint8_t> notDds(200, 0);
    CHECK(!HdClaudeIsDds(notDds.data(), notDds.size()));
    refused(notDds, "not a DDS");
}

}  // namespace

int main()
{
    TestColorWritesFloat32();
    TestColorWritesFloat16();
    TestColorUNorm8EncodesColourNotAlpha();
    TestColorRefusesAFormatItCannotFill();
    TestClearFillsFloat16();
    TestClearOfAFormatItCannotFillHoldsNoFrame();
    TestReallocationDiscardsTheFrame();
    TestInstancerForgetsRemovedPrimvars();
    TestInstancerKeepsWhatIsStillAuthored();
    TestDdsReadsMaskedPixelsBottomUp();
    TestDdsReadsLuminanceAsGrey();
    TestDdsEightBitColourIsSrgbUnlessDeclared();
    TestDdsDecodesBc1Blocks();
    TestDdsDecodesBc4AsLinearGrey();
    TestDdsKeepsFloatRange();
    TestDdsRefusesWhatItCannotRead();
    return hdclaude_test::Summarize("hydra");
}
