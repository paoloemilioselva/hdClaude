// Hydra adapter classes that can be exercised without a render index, a stage,
// or a GPU: built from the adapter's own sources against OpenUSD, so a defect
// in what a host reads back is found here rather than as a black viewport.

#include "test_support.h"

#include "render_buffer.h"

#include "pxr/base/gf/half.h"
#include "pxr/base/gf/vec3i.h"

#include <cstring>
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
    return hdclaude_test::Summarize("hydra");
}
