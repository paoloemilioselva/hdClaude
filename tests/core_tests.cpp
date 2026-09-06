#include "test_support.h"

#include "hdclaude/core/hash.h"
#include "hdclaude/core/shader_cache.h"
#include "hdclaude/core/display.h"
#include "hdclaude/core/spectrum.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

using namespace hdclaude;

namespace {

// ---------------------------------------------------------------------------
// Hashing
// ---------------------------------------------------------------------------

void TestSha256KnownVectors()
{
    // FIPS 180-4 examples. A hand-written SHA-256 that is subtly wrong would
    // still produce stable-looking cache keys, so it is checked against
    // published vectors rather than against itself.
    CHECK_EQ(ToHex(Sha256("")),
             std::string("e3b0c44298fc1c149afbf4c8996fb924"
                         "27ae41e4649b934ca495991b7852b855"));
    CHECK_EQ(ToHex(Sha256("abc")),
             std::string("ba7816bf8f01cfea414140de5dae2223"
                         "b00361a396177a9cb410ff61f20015ad"));
    CHECK_EQ(ToHex(Sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
             std::string("248d6a61d20638b8e5c026930c3e6039"
                         "a33ce45964ff2167f6ecedd419db06c1"));

    // A message spanning several blocks, exercising the incremental path.
    const std::string million(1000, 'a');
    Sha256Builder builder;
    for (int i = 0; i < 1000; ++i) {
        builder.Update(million);
    }
    CHECK_EQ(ToHex(builder.Finish()),
             std::string("cdc76e5c9914fb9281a1c7e284d73e67"
                         "f1809a48a497200e046d39ccc7112cd0"));
}

void TestSha256FieldsAreUnambiguous()
{
    // Length-prefixed fields must distinguish different splits of the same
    // concatenation. Without this, two different cache keys collide and the
    // wrong SPIR-V is served -- silently.
    Sha256Builder a;
    a.Field("ab").Field("c");
    Sha256Builder b;
    b.Field("a").Field("bc");
    CHECK(ToHex(a.Finish()) != ToHex(b.Finish()));
}

// ---------------------------------------------------------------------------
// Shader cache
// ---------------------------------------------------------------------------

ShaderCacheKey MakeKey()
{
    ShaderCacheKey key;
    key.source = "void main() {}";
    key.entryPoint = "main";
    key.stage = "compute";
    key.generatorTarget = "genglsl_pt";
    key.generatorVersion = "1.39.3";
    key.compilerVersion = "glslang-1.4.357.0";
    key.spirvVersion = 0x00010600u;
    key.vulkanVersion = 0x00403000u;
    key.abiVersion = 1;
    key.optimize = false;
    return key;
}

std::vector<std::uint32_t> MakeSpirv()
{
    // Magic number plus a plausible header. Find() validates the magic.
    return {0x07230203u, 0x00010600u, 0x00000000u, 0x00000010u, 0x00000000u};
}

void TestShaderCacheRoundTrip()
{
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / "hdclaude-cache-test";
    std::error_code error;
    std::filesystem::remove_all(directory, error);

    ShaderCache cache(directory);
    const ShaderCacheKey key = MakeKey();

    CHECK(!cache.Find(key).has_value());
    CHECK_EQ(cache.MissCount(), std::uint64_t(1));

    CHECK(cache.Store(key, MakeSpirv()));

    const auto found = cache.Find(key);
    CHECK(found.has_value());
    if (found) {
        CHECK(*found == MakeSpirv());
    }
    CHECK_EQ(cache.HitCount(), std::uint64_t(1));

    std::filesystem::remove_all(directory, error);
}

void TestShaderCacheMissesOnEveryKeyComponent()
{
    // The exit gate in docs/roadmap.md phase 1: the cache must miss on a change
    // to *every* component of its key. A component that does not participate is
    // how a stale shader survives a toolchain or ABI change.
    const ShaderCacheKey base = MakeKey();
    const std::string baseDigest = base.Digest();

    auto differs = [&](ShaderCacheKey modified) {
        return modified.Digest() != baseDigest;
    };

    {   auto k = base; k.source += " ";                 CHECK(differs(k)); }
    {   auto k = base; k.entryPoint = "other";          CHECK(differs(k)); }
    {   auto k = base; k.stage = "vertex";              CHECK(differs(k)); }
    {   auto k = base; k.generatorTarget = "genglsl";   CHECK(differs(k)); }
    {   auto k = base; k.generatorVersion = "1.39.6";   CHECK(differs(k)); }
    {   auto k = base; k.compilerVersion = "other";     CHECK(differs(k)); }
    {   auto k = base; k.spirvVersion += 1;             CHECK(differs(k)); }
    {   auto k = base; k.vulkanVersion += 1;            CHECK(differs(k)); }
    {   auto k = base; k.abiVersion += 1;               CHECK(differs(k)); }
    {   auto k = base; k.optimize = !k.optimize;        CHECK(differs(k)); }
}

// ---------------------------------------------------------------------------
// Spectral
// ---------------------------------------------------------------------------

void TestWavelengthSamplingIsConsistent()
{
    // The single most important spectral invariant: the density must be the
    // derivative of the sampler's inverse CDF. If these disagree, every pixel
    // is biased, and no image inspection will reveal it.
    constexpr int kSamples = 512;
    for (int i = 1; i < kSamples; ++i) {
        const float u = static_cast<float>(i) / static_cast<float>(kSamples);
        const float h = 1.0e-4f;
        const float lambda = SampleVisibleWavelength(u);
        const float dLambdaDu =
            (SampleVisibleWavelength(u + h) - SampleVisibleWavelength(u - h)) /
            (2.0f * h);
        // p(lambda) = du/dlambda = 1 / (dlambda/du)
        const float numericPdf = 1.0f / dLambdaDu;
        CHECK_NEAR(VisibleWavelengthPdf(lambda), numericPdf, 2.0e-5);
    }
}

void TestWavelengthDensityIntegratesToOne()
{
    // Integrated well outside the visible range on purpose: the density must be
    // exactly zero where the sampler cannot land, so the tails of the analytic
    // form contribute nothing and the total is one.
    double total = 0.0;
    constexpr double kStep = 0.05;
    for (double lambda = 200.0; lambda <= 1000.0; lambda += kStep) {
        total += VisibleWavelengthPdf(static_cast<float>(lambda)) * kStep;
    }
    CHECK_NEAR(total, 1.0, 1.0e-3);
}

void TestWavelengthSupportMatchesVisibleRange()
{
    // The sampler's support and the range the hero packet wraps within must be
    // the same interval. If they differ, a rotated lane can land where the
    // density is zero and the estimator divides by zero.
    CHECK_NEAR(SampleVisibleWavelength(0.0f), kLambdaMin, 1.0e-3);
    CHECK_NEAR(SampleVisibleWavelength(1.0f), kLambdaMax, 1.0e-3);
    CHECK_EQ(VisibleWavelengthPdf(kLambdaMin - 0.5f), 0.0f);
    CHECK_EQ(VisibleWavelengthPdf(kLambdaMax + 0.5f), 0.0f);
}

void TestHeroPacketStratifies()
{
    for (int i = 1; i < 32; ++i) {
        const float u = static_cast<float>(i) / 32.0f;
        const WavelengthSample sample = SampleHeroWavelengths(u);
        for (int lane = 0; lane < kSpectralLanes; ++lane) {
            const float lambda = sample.lambda[static_cast<std::size_t>(lane)];
            CHECK(lambda >= kLambdaMin - 1.0f);
            CHECK(lambda <= kLambdaMax + 1.0f);
            CHECK(sample.pdf[static_cast<std::size_t>(lane)] > 0.0f);
        }
        // Lanes must be distinct, or the packet carries redundant information
        // and the estimator's variance reduction does not happen.
        for (int a = 0; a < kSpectralLanes; ++a) {
            for (int b = a + 1; b < kSpectralLanes; ++b) {
                CHECK(std::fabs(sample.lambda[static_cast<std::size_t>(a)] -
                                sample.lambda[static_cast<std::size_t>(b)]) > 1.0f);
            }
        }
    }
}

void TestColorSpaceRoundTrip()
{
    const Vec3 colors[] = {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
                           {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f, 1.0f},
                           {0.18f, 0.42f, 0.73f}};
    for (const Vec3& rgb : colors) {
        const Vec3 back = XyzToLinearSrgb(LinearSrgbToXyz(rgb));
        CHECK_NEAR(back.x, rgb.x, 1.0e-4);
        CHECK_NEAR(back.y, rgb.y, 1.0e-4);
        CHECK_NEAR(back.z, rgb.z, 1.0e-4);
    }
}

void TestSrgbTransferRoundTrip()
{
    for (int i = 0; i <= 100; ++i) {
        const float encoded = static_cast<float>(i) / 100.0f;
        CHECK_NEAR(LinearToSrgb(SrgbToLinear(encoded)), encoded, 1.0e-5);
    }
}

void TestWhiteSpectrumIsNeutral()
{
    // A flat unit spectrum integrated against the colour matching functions
    // must produce an achromatic result under illuminant E. This catches an
    // error in the CMF fit or in the normalisation that a round trip through
    // the colour matrices alone would not.
    Vec3 xyz{};
    for (float lambda = kLambdaMin; lambda <= kLambdaMax; lambda += 1.0f) {
        xyz += CieXyzBar(lambda);
    }
    const float scale = 1.0f / CieYIntegral();
    xyz = xyz * scale;

    CHECK_NEAR(xyz.y, 1.0, 1.0e-5);
    // Illuminant E's chromaticity is x = y = 1/3, so X and Z both equal Y.
    CHECK_NEAR(xyz.x, 1.0, 0.02);
    CHECK_NEAR(xyz.z, 1.0, 0.02);
}

/// The phase 1 exit gate: upsampling followed by CIE integration returns the
/// colour it started from.
///
/// This is the claim the whole spectral pipeline rests on. Every RGB an asset
/// authors becomes a spectrum, is transported as one, and is integrated back
/// through the colour matching functions; if that round trip does not return
/// the original, then a renderer that transports spectrally renders every
/// material a slightly different colour than an RGB renderer would, for no
/// reason a user could act on.
///
/// Measured in CIEDE2000 because "within a thousandth" only means something in
/// a space where a unit is perceptual. The set spans the primaries and
/// secondaries -- which sit on the gamut boundary and are where a bounded
/// model has the least freedom -- a grey ramp, and a spread of muted
/// reflectances of the kind a real asset is painted in.
void TestReflectanceUpsamplingRoundTrips()
{
    struct Sample {
        const char* name;
        Vec3 rgb;
    };
    const Sample samples[] = {
        {"red", {1.0f, 0.0f, 0.0f}},
        {"green", {0.0f, 1.0f, 0.0f}},
        {"blue", {0.0f, 0.0f, 1.0f}},
        {"cyan", {0.0f, 1.0f, 1.0f}},
        {"magenta", {1.0f, 0.0f, 1.0f}},
        {"yellow", {1.0f, 1.0f, 0.0f}},
        {"white", {1.0f, 1.0f, 1.0f}},
        {"grey 0.5", {0.5f, 0.5f, 0.5f}},
        {"grey 0.18", {0.18f, 0.18f, 0.18f}},
        {"skin", {0.55f, 0.38f, 0.31f}},
        {"foliage", {0.16f, 0.29f, 0.12f}},
        {"sky", {0.22f, 0.35f, 0.62f}},
        {"clay", {0.48f, 0.24f, 0.14f}},
        {"olive", {0.34f, 0.32f, 0.10f}},
        {"plum", {0.29f, 0.14f, 0.30f}},
        {"teal", {0.10f, 0.36f, 0.34f}},
    };

    double worst = 0.0;
    const char* worstName = "";
    for (const Sample& sample : samples) {
        const SpectrumFit fit = FitSpectrum(sample.rgb);
        const Vec3 back = IntegrateSpectrum(fit);

        const Vec3 before = XyzToLab(LinearSrgbToXyz(sample.rgb));
        const Vec3 after = XyzToLab(LinearSrgbToXyz(back));
        const double difference = DeltaE2000(before, after);
        if (difference > worst) {
            worst = difference;
            worstName = sample.name;
        }

        // A reflectance model that can create energy is not one: whatever the
        // fit converged to, the spectrum it describes must stay inside [0, 1]
        // at every wavelength, which the sigmoid guarantees structurally and
        // this checks has not been undone.
        for (float lambda = kLambdaMin; lambda <= kLambdaMax; lambda += 5.0f) {
            const float value = EvaluateSpectrum(fit, lambda);
            CHECK(value >= 0.0f && value <= 1.0f);
        }
    }

    std::printf("  reflectance round trip: worst dE2000 %.2e (%s)\n", worst,
                worstName);
    CHECK(worst < 1.0e-3);
}

/// Emission is unbounded and must stay exact in both chromaticity and
/// magnitude.
///
/// A light of RGB (5, 5, 5) integrates to five times white; nothing in [0, 1]
/// says that, which is why emission carries its magnitude beside a bounded
/// spectrum rather than being fitted as one.
void TestEmissionUpsamplingPreservesColourAndMagnitude()
{
    const Vec3 colours[] = {{5.0f, 5.0f, 5.0f},
                            {1.0f, 0.6f, 0.2f},
                            {12.0f, 3.0f, 0.5f},
                            {0.0f, 0.0f, 0.0f}};

    for (const Vec3& colour : colours) {
        const SpectrumFit emission = FitSpectrum(colour);

        for (float lambda = kLambdaMin; lambda <= kLambdaMax; lambda += 5.0f) {
            CHECK(EvaluateSpectrum(emission, lambda) >= 0.0f);
        }

        const Vec3 back = IntegrateSpectrum(emission);
        CHECK_NEAR(back.x, colour.x, 1.0e-3 + 1.0e-3 * colour.x);
        CHECK_NEAR(back.y, colour.y, 1.0e-3 + 1.0e-3 * colour.y);
        CHECK_NEAR(back.z, colour.z, 1.0e-3 + 1.0e-3 * colour.z);
    }
}

/// The metric itself, against the published CIEDE2000 test data.
///
/// A round trip judged by a wrong difference formula is a round trip that
/// passes for the wrong reason, so the formula is checked against Sharma,
/// Wu and Dalal's worked pairs -- including the ones chosen to exercise the
/// hue-rotation term that a naive implementation drops.
void TestDeltaE2000MatchesPublishedPairs()
{
    struct Pair {
        Vec3 a;
        Vec3 b;
        double expected;
    };
    const Pair pairs[] = {
        {{50.0000f, 2.6772f, -79.7751f}, {50.0000f, 0.0000f, -82.7485f}, 2.0425},
        {{50.0000f, 3.1571f, -77.2803f}, {50.0000f, 0.0000f, -82.7485f}, 2.8615},
        {{50.0000f, 2.8361f, -74.0200f}, {50.0000f, 0.0000f, -82.7485f}, 3.4412},
        {{50.0000f, -1.3802f, -84.2814f}, {50.0000f, 0.0000f, -82.7485f}, 1.0000},
        {{50.0000f, 0.0000f, 0.0000f}, {50.0000f, -1.0000f, 2.0000f}, 2.3669},
        {{50.0000f, -1.0000f, 2.0000f}, {50.0000f, 0.0000f, 0.0000f}, 2.3669},
        {{50.0000f, 2.5000f, 0.0000f}, {50.0000f, 0.0000f, -2.5000f}, 4.3065},
        {{60.2574f, -34.0099f, 36.2677f}, {60.4626f, -34.1751f, 39.4387f}, 1.2644},
        {{22.7233f, 20.0904f, -46.6940f}, {23.0331f, 14.9730f, -42.5619f}, 2.0373},
        {{2.0776f, 0.0795f, -1.1350f}, {0.9033f, -0.0636f, -0.5514f}, 0.9082},
    };

    for (const Pair& pair : pairs) {
        CHECK_NEAR(DeltaE2000(pair.a, pair.b), pair.expected, 1.0e-3);
    }
}

/// The tabulated fit must agree with the fit it stands in for.
///
/// A shading point's colour is not known until it is shaded, so the GPU reads
/// coefficients from a table rather than fitting. A table that disagrees with
/// the fitter is a renderer whose materials are a slightly different colour
/// than its own unit tests say they are -- and nothing downstream would show
/// it, because both answers are smooth, bounded and plausible.
///
/// Bilinear interpolation between two fits is not itself a fit, so the bar here
/// is a tenth of a dE2000 rather than the fitter's own 1e-3: still forty times
/// under a just-noticeable difference, and it is the interpolation being
/// measured, not the model.
void TestChromaTableMatchesTheFit()
{
    const ChromaTable table = BuildChromaTable();
    CHECK(table.Valid());
    if (!table.Valid()) {
        return;
    }

    const Vec3 samples[] = {
        {1.0f, 0.0f, 0.0f},   {0.0f, 1.0f, 0.0f},   {0.0f, 0.0f, 1.0f},
        {1.0f, 1.0f, 1.0f},   {0.5f, 0.5f, 0.5f},   {0.55f, 0.38f, 0.31f},
        {0.16f, 0.29f, 0.12f}, {0.22f, 0.35f, 0.62f}, {0.48f, 0.24f, 0.14f},
        {0.9f, 0.87f, 0.4f},  {0.05f, 0.42f, 0.37f}, {0.73f, 0.19f, 0.66f},
    };

    double worst = 0.0;
    for (const Vec3& rgb : samples) {
        SpectrumFit tabulated;
        tabulated.chroma = LookUpChroma(table, rgb);
        tabulated.scale = std::max({rgb.x, rgb.y, rgb.z});

        const Vec3 back = IntegrateSpectrum(tabulated);
        const double difference =
            DeltaE2000(XyzToLab(LinearSrgbToXyz(rgb)),
                       XyzToLab(LinearSrgbToXyz(back)));
        worst = std::max(worst, difference);
    }

    std::printf("  chroma table: worst dE2000 %.2e\n", worst);
    CHECK(worst < 0.1);
}

void TestBlackbodyPeakMatchesWien()
{
    // Wien's displacement law is an independent check on Planck's law: the
    // peak of the sampled spectrum must land where the law says it does.
    for (const float kelvin : {2000.0f, 3200.0f, 5600.0f, 6500.0f}) {
        const float expectedPeak = 2.8977721e-3f / kelvin * 1e9f;

        float bestLambda = 0.0f;
        float bestValue = 0.0f;
        for (float lambda = 100.0f; lambda <= 3000.0f; lambda += 0.5f) {
            const float value = BlackbodyRadiance(lambda, kelvin);
            if (value > bestValue) {
                bestValue = value;
                bestLambda = lambda;
            }
        }
        CHECK_NEAR(bestLambda, expectedPeak, 1.0);
        CHECK_NEAR(NormalizedBlackbody(expectedPeak, kelvin), 1.0, 1.0e-5);
    }
}

void TestDisplayTransformLeavesTheDiffuseRangeAlone()
{
    // The gallery is a comparison instrument, so the transform must not be a
    // look: below the compression threshold it is the sRGB transfer function
    // of the published offset and nothing else. A curve that quietly shaped
    // the diffuse range would make every baseline comparison a comparison of
    // curves.
    for (float linear = 0.1f; linear < 0.7f; linear += 0.05f) {
        const Vec3 display = SceneLinearToDisplaySrgb({linear, linear, linear});
        CHECK_NEAR(display.x, LinearToSrgb(linear - 0.04f), 1.0e-5);
        CHECK_NEAR(display.y, display.x, 1.0e-6);
        CHECK_NEAR(display.z, display.x, 1.0e-6);
    }
}

void TestDisplayTransformCompressesRatherThanClips()
{
    // Over-range values must stay ordered and stay inside the display range.
    // Clipping would collapse them to one value, which is what makes a blown
    // highlight unreadable in a baseline: two very different radiances would
    // encode identically and the diff would report no change.
    float previous = -1.0f;
    for (float linear = 1.0f; linear < 200.0f; linear *= 1.5f) {
        const Vec3 display = SceneLinearToDisplaySrgb({linear, linear, linear});
        CHECK(display.x > previous);
        CHECK(display.x <= 1.0f);
        previous = display.x;
    }
    CHECK(previous > 0.9f);
}

void TestDisplayTransformSanitisesAndExposes()
{
    // A NaN encodes as black rather than as a random byte, and exposure is a
    // scene-referred control: +1 stop is the same image at twice the radiance,
    // not a shifted curve.
    const Vec3 nan = SceneLinearToDisplaySrgb(
        {std::numeric_limits<float>::quiet_NaN(), -1.0f,
         std::numeric_limits<float>::infinity()});
    CHECK_EQ(nan.x, 0.0f);
    CHECK_EQ(nan.y, 0.0f);
    CHECK_EQ(nan.z, 0.0f);

    const Vec3 exposed = SceneLinearToDisplaySrgb({0.1f, 0.1f, 0.1f}, 1.0f);
    const Vec3 doubled = SceneLinearToDisplaySrgb({0.2f, 0.2f, 0.2f}, 0.0f);
    CHECK_NEAR(exposed.x, doubled.x, 1.0e-6);
}

}  // namespace

int main()
{
    TestSha256KnownVectors();
    TestSha256FieldsAreUnambiguous();
    TestShaderCacheRoundTrip();
    TestShaderCacheMissesOnEveryKeyComponent();
    TestWavelengthSamplingIsConsistent();
    TestWavelengthDensityIntegratesToOne();
    TestWavelengthSupportMatchesVisibleRange();
    TestHeroPacketStratifies();
    TestColorSpaceRoundTrip();
    TestSrgbTransferRoundTrip();
    TestWhiteSpectrumIsNeutral();
    TestBlackbodyPeakMatchesWien();
    TestDeltaE2000MatchesPublishedPairs();
    TestReflectanceUpsamplingRoundTrips();
    TestEmissionUpsamplingPreservesColourAndMagnitude();
    TestChromaTableMatchesTheFit();
    TestDisplayTransformLeavesTheDiffuseRangeAlone();
    TestDisplayTransformCompressesRatherThanClips();
    TestDisplayTransformSanitisesAndExposes();
    return hdclaude_test::Summarize("hdClaudeCoreTests");
}
