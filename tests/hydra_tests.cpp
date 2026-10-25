// Hydra adapter classes that can be exercised without a render index, a stage,
// or a GPU: built from the adapter's own sources against OpenUSD, so a defect
// in what a host reads back is found here rather than as a black viewport.

#include "test_support.h"

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
#include <cstring>
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
    return hdclaude_test::Summarize("hydra");
}
