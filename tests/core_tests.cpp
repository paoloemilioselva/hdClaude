#include "test_support.h"

#include "hdclaude/core/hash.h"
#include "hdclaude/core/shader_cache.h"
#include "hdclaude/core/curve_sweep.h"
#include "hdclaude/core/gaussian_splats.h"
#include "hdclaude/core/environment.h"
#include "hdclaude/core/display.h"
#include "hdclaude/core/image_metrics.h"
#include "hdclaude/core/quad_tessellation.h"
#include "hdclaude/core/sphere_mesh.h"
#include "hdclaude/core/spectrum.h"
#include "hdclaude/core/tessellation.h"

#include <algorithm>
#include <cstdint>
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

/// The Munsell half of phase 1's round-trip gate.
///
/// The gate names "a set of Munsell reflectances", and the set above is chosen
/// by eye. The 24 ColorChecker patches are the standard Munsell-specified set:
/// each is defined by a Munsell notation (dark skin is 3 YR 3.7/3.2, cyan is
/// 5 B 5/8, the neutrals run N 9.5/ to N 2/), and X-Rite publishes the sRGB
/// values of those notations under D65, which is the colour space this fit
/// works in. The values are the manufacturer's 8-bit sRGB, decoded through the
/// sRGB transfer function, and they include the most saturated reflectances in
/// the chart -- orange, yellow and cyan sit near the edge of sRGB -- which is
/// where a bounded spectral model is tested hardest.
void TestMunsellColorCheckerRoundTrips()
{
    struct Patch {
        const char* name;
        const char* munsell;
        int r, g, b;
    };
    const Patch patches[] = {
        {"dark skin", "3 YR 3.7/3.2", 0x73, 0x52, 0x44},
        {"light skin", "2.2 YR 6.47/4.1", 0xc2, 0x96, 0x82},
        {"blue sky", "4.3 PB 4.95/5.5", 0x62, 0x7a, 0x9d},
        {"foliage", "6.7 GY 4.2/4.1", 0x57, 0x6c, 0x43},
        {"blue flower", "9.7 PB 5.47/6.7", 0x85, 0x80, 0xb1},
        {"bluish green", "2.5 BG 7/6", 0x67, 0xbd, 0xaa},
        {"orange", "5 YR 6/11", 0xd6, 0x7e, 0x2c},
        {"purplish blue", "7.5 PB 4/10.7", 0x50, 0x5b, 0xa6},
        {"moderate red", "2.5 R 5/10", 0xc1, 0x5a, 0x63},
        {"purple", "5 P 3/7", 0x5e, 0x3c, 0x6c},
        {"yellow green", "5 GY 7.1/9.1", 0x9d, 0xbc, 0x40},
        {"orange yellow", "10 YR 7/10.5", 0xe0, 0xa3, 0x2e},
        {"blue", "7.5 PB 2.9/12.7", 0x38, 0x3d, 0x96},
        {"green", "0.25 G 5.4/9.6", 0x46, 0x94, 0x49},
        {"red", "5 R 4/12", 0xaf, 0x36, 0x3c},
        {"yellow", "5 Y 8/11.1", 0xe7, 0xc7, 0x1f},
        {"magenta", "2.5 RP 5/12", 0xbb, 0x56, 0x95},
        {"cyan", "5 B 5/8", 0x08, 0x85, 0xa1},
        {"white", "N 9.5/", 0xf3, 0xf3, 0xf3},
        {"neutral 8", "N 8/", 0xc8, 0xc8, 0xc8},
        {"neutral 6.5", "N 6.5/", 0xa0, 0xa0, 0xa0},
        {"neutral 5", "N 5/", 0x7a, 0x7a, 0x7a},
        {"neutral 3.5", "N 3.5/", 0x55, 0x55, 0x55},
        {"black", "N 2/", 0x34, 0x34, 0x34},
    };

    double worst = 0.0;
    const Patch* worstPatch = &patches[0];
    for (const Patch& patch : patches) {
        const Vec3 rgb{SrgbToLinear(float(patch.r) / 255.0f),
                       SrgbToLinear(float(patch.g) / 255.0f),
                       SrgbToLinear(float(patch.b) / 255.0f)};
        const SpectrumFit fit = FitSpectrum(rgb);
        const Vec3 back = IntegrateSpectrum(fit);

        const double difference =
            DeltaE2000(XyzToLab(LinearSrgbToXyz(rgb)),
                       XyzToLab(LinearSrgbToXyz(back)));
        if (difference > worst) {
            worst = difference;
            worstPatch = &patch;
        }

        for (float lambda = kLambdaMin; lambda <= kLambdaMax; lambda += 5.0f) {
            const float value = EvaluateSpectrum(fit, lambda);
            CHECK(value >= 0.0f && value <= 1.0f);
        }
    }

    std::printf("  Munsell ColorChecker round trip: worst dE2000 %.2e (%s, %s)\n",
                worst, worstPatch->name, worstPatch->munsell);
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

/// The packet must estimate a spectral integral without bias.
///
/// This is the estimator the film runs, written out: draw a packet, divide each
/// lane by its density, average. Integrating the CIE ybar this way has to
/// return its own integral, which is the normalisation constant the sensor is
/// built on.
///
/// It exists because the alternative -- dividing each lane by the base
/// density evaluated at that lane's own wavelength -- is wrong and produces an
/// image that converges perfectly well to the wrong spectrum. A rotated lane is
/// not an independent draw, and no amount of rendering would show which of the
/// two divisors was used.
void TestHeroPacketIntegratesUnbiased()
{
    constexpr int kSamples = 20000;
    double total = 0.0;
    for (int i = 0; i < kSamples; ++i) {
        // Stratified over the unit interval, offset off the ends where the
        // inverse CDF is at its steepest.
        const float u = (static_cast<float>(i) + 0.5f) /
                        static_cast<float>(kSamples);
        const WavelengthSample packet = SampleHeroWavelengths(u);

        double estimate = 0.0;
        for (int lane = 0; lane < kSpectralLanes; ++lane) {
            const auto index = static_cast<std::size_t>(lane);
            estimate += static_cast<double>(CieXyzBar(packet.lambda[index]).y) /
                        static_cast<double>(packet.pdf[index]);
        }
        total += estimate / static_cast<double>(kSpectralLanes);
    }
    const double estimated = total / static_cast<double>(kSamples);
    const double reference = static_cast<double>(CieYIntegral());

    std::printf("  hero packet integrates ybar to %.5f (reference %.5f)\n",
                estimated, reference);
    CHECK_NEAR(estimated, reference, reference * 2.0e-3);
}

/// The spectral blackbody has to land on the Planckian locus.
///
/// `enableColorTemperature` is the single most visible difference between a
/// spectral renderer and an RGB one, and it is only a difference if the
/// spectrum is right. Integrating Planck's law through the colour matching
/// functions and comparing the chromaticity against the published locus checks
/// the two together: a wrong constant in Planck and a wrong colour matching
/// integration would each move the answer, and neither is likely to move it
/// back onto the locus at five temperatures at once.
void TestBlackbodyLandsOnThePlanckianLocus()
{
    struct Point {
        float kelvin;
        float x;
        float y;
    };
    const Point locus[] = {
        {2000.0f, 0.5267f, 0.4133f},
        {3000.0f, 0.4369f, 0.4041f},
        {4000.0f, 0.3805f, 0.3768f},
        {6500.0f, 0.3135f, 0.3237f},
        {10000.0f, 0.2807f, 0.2884f},
    };

    for (const Point& point : locus) {
        const Vec3 chromaticity = XyzToChromaticity(BlackbodyXyz(point.kelvin));
        std::printf("  %.0f K -> x %.4f y %.4f (published %.4f %.4f)\n",
                    point.kelvin, chromaticity.x, chromaticity.y, point.x,
                    point.y);
        // Five thousandths, which is a few times the Wyman fit's own error and
        // a great deal less than the distance between these five points.
        CHECK_NEAR(chromaticity.x, point.x, 5.0e-3);
        CHECK_NEAR(chromaticity.y, point.y, 5.0e-3);
    }
}

/// A colour temperature tints; it must not brighten.
///
/// The scale equates the blackbody's luminous integral with D65's, so enabling
/// the control changes a light's hue and leaves its luminance where the author
/// put it. Matching *peaks* instead would make a warm light a dim one, because
/// a 2700 K blackbody peaks well outside the visible range.
void TestBlackbodyScaleKeepsLuminanceConstant()
{
    const float temperatures[] = {2000.0f, 2700.0f, 4000.0f, 6504.0f, 12000.0f};

    double reference = 0.0;
    for (float lambda = kLambdaMin; lambda <= kLambdaMax; lambda += 5.0f) {
        reference += static_cast<double>(IlluminantD65(lambda)) *
                     static_cast<double>(CieXyzBar(lambda).y);
    }

    for (const float kelvin : temperatures) {
        const float scale = BlackbodyLuminousScale(kelvin);
        double luminous = 0.0;
        for (float lambda = kLambdaMin; lambda <= kLambdaMax; lambda += 5.0f) {
            luminous += static_cast<double>(NormalizedBlackbody(lambda, kelvin)) *
                        static_cast<double>(scale) *
                        static_cast<double>(CieXyzBar(lambda).y);
        }
        CHECK_NEAR(luminous / reference, 1.0, 1.0e-4);
    }
}

/// The dispersion curve has to reproduce the two numbers that defined it.
///
/// An Abbe number is not a free parameter of a fit; it *is* a measurement, and
/// the two-term Cauchy through it is fully determined. So the curve must pass
/// through the quoted index at the yellow d-line, and the spread it produces
/// between the blue F-line and the red C-line must come back as
/// `(nd - 1) / V` exactly. Both are identities rather than approximations, which
/// is what makes this checkable to a part in ten thousand rather than to a
/// tolerance somebody chose.
///
/// The third case is the one that matters for a renderer: a *smaller* Abbe
/// number spreads light more. That reads backwards, it is the commonest way to
/// get dispersion inverted, and it is a sign error no image would obviously
/// betray -- a prism that fans red the wrong side of blue still looks like a
/// prism.
void TestDispersionReproducesItsAbbeNumber()
{
    struct Glass { float ior; float abbe; const char* name; };
    const Glass glasses[] = {
        {1.5168f, 64.17f, "BK7 crown"},
        {1.7847f, 25.72f, "SF11 dense flint"},
        {2.4168f, 55.30f, "diamond"},
    };

    for (const Glass& glass : glasses) {
        const float atD = DispersedIor(glass.ior, glass.abbe, kFraunhoferD);
        const float atF = DispersedIor(glass.ior, glass.abbe, kFraunhoferF);
        const float atC = DispersedIor(glass.ior, glass.abbe, kFraunhoferC);
        const double spread = double(atF) - double(atC);
        const double wanted = (double(glass.ior) - 1.0) / double(glass.abbe);

        std::printf("  %-18s n_d %.5f (quoted %.5f), n_F - n_C %.6f "
                    "(V implies %.6f)\n",
                    glass.name, atD, glass.ior, spread, wanted);

        CHECK_NEAR(atD, glass.ior, 1.0e-4);
        CHECK_NEAR(spread, wanted, 1.0e-4);
        // Blue is bent more than red by every ordinary transparent material.
        CHECK(atF > atC);
    }

    // A smaller Abbe number is a more dispersive glass, which is the direction
    // the name works against.
    const double flint =
        DispersedIor(1.6f, 25.0f, kFraunhoferF) - DispersedIor(1.6f, 25.0f, kFraunhoferC);
    const double crown =
        DispersedIor(1.6f, 64.0f, kFraunhoferF) - DispersedIor(1.6f, 64.0f, kFraunhoferC);
    CHECK(flint > crown * 2.0);

    // And zero means no dispersion at all, not a division by zero.
    CHECK_NEAR(DispersedIor(1.5f, 0.0f, 400.0f), 1.5, 1.0e-6);
    CHECK_NEAR(DispersedIor(1.5f, 0.0f, 700.0f), 1.5, 1.0e-6);
}

/// The subsurface albedo inversion returns the colour it was given.
///
/// A random walk is parameterised by how much of each *collision* survives, and
/// a subsurface material is authored by how much of the *light* comes back out.
/// Those are different numbers and they are far apart: van de Hulst's relation
/// says a medium whose collisions each survive with probability 0.6 returns
/// about a fifth of what enters it. OpenPBR states both directions in closed
/// form, so the inversion can be checked against the forward relation rather
/// than against remembered values -- an identity, to the accuracy of the fit
/// OpenPBR quotes, and not a tolerance anyone chose.
///
/// The ends are what a fit gets wrong, and both matter here. A perfectly white
/// subsurface material must be lossless or a furnace cannot gate it, and a
/// perfectly black one must not scatter at all.
void TestSubsurfaceAlbedoInversionRoundTrips()
{
    for (const float g : {-0.5f, 0.0f, 0.5f, 0.9f}) {
        double worst = 0.0;
        for (int i = 0; i <= 100; ++i) {
            const auto reflectance = static_cast<float>(i) / 100.0f;
            const float albedo = SubsurfaceSingleScatteringAlbedo(reflectance, g);
            const float back = VanDeHulstDiffuseAlbedo(albedo, g);
            worst = std::max(worst, std::abs(double(back) - double(reflectance)));
        }
        std::printf("  subsurface inversion, g = %+.1f: worst round trip %.2e\n",
                    g, worst);
        CHECK(worst < 2.0e-3);
    }

    // White is lossless and black does not scatter, at every anisotropy.
    for (const float g : {-0.5f, 0.0f, 0.5f, 0.9f}) {
        CHECK_NEAR(SubsurfaceSingleScatteringAlbedo(1.0f, g), 1.0, 1.0e-3);
        CHECK_NEAR(SubsurfaceSingleScatteringAlbedo(0.0f, g), 0.0, 1.0e-3);
    }

    // The inversion is the *opposite* of the identity, and by a wide margin:
    // this is the whole reason it exists. A material authored at 0.6 needs
    // collisions that survive 95 per cent of the time, and a walk given 0.6
    // directly would return about 0.19.
    const float albedo = SubsurfaceSingleScatteringAlbedo(0.6f, 0.0f);
    std::printf("  subsurface: colour 0.6 needs albedo %.4f; albedo 0.6 "
                "returns %.4f\n",
                albedo, VanDeHulstDiffuseAlbedo(0.6f, 0.0f));
    CHECK(albedo > 0.94f);
    CHECK(VanDeHulstDiffuseAlbedo(0.6f, 0.0f) < 0.22f);

    // Monotonic, which a fit evaluated outside its range need not be.
    float previous = -1.0f;
    for (int i = 0; i <= 50; ++i) {
        const float value =
            SubsurfaceSingleScatteringAlbedo(static_cast<float>(i) / 50.0f, 0.0f);
        CHECK(value >= previous);
        previous = value;
    }
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

// --- Curve sweep -----------------------------------------------------------

/// A straight two-point curve swept into a tube of a known radius.
///
/// The claim is geometric and checkable without a renderer: every vertex of the
/// tube lies exactly one radius from the curve's axis, and the normals point
/// straight out from it. That is what a swept circle *is*, so it is what the
/// tessellation must satisfy however many sides it has.
void TestCurveSweepIsARadiusFromItsAxis()
{
    const std::vector<int> counts{2};
    // Along +z, from the origin.
    const std::vector<float> points{0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 4.0f};
    const std::vector<float> widths{0.5f};

    std::string reason;
    const hdclaude::CurveMesh mesh = hdclaude::SweepCurves(
        counts, points, widths, 1.0f, 8, false, &reason);
    CHECK(mesh.Valid());
    CHECK(reason.empty());

    const std::size_t vertices = mesh.positions.size() / 3;
    // Two rings of nine vertices: eight sides plus the seam's second copy.
    CHECK_EQ(vertices, std::size_t(18));
    CHECK_EQ(mesh.normals.size(), mesh.positions.size());
    CHECK_EQ(mesh.uvs.size(), vertices * 2);

    double worstRadius = 0.0;
    double worstNormal = 0.0;
    for (std::size_t i = 0; i < vertices; ++i) {
        const double x = mesh.positions[i * 3 + 0];
        const double y = mesh.positions[i * 3 + 1];
        const double z = mesh.positions[i * 3 + 2];
        // The axis is the z line, so the distance to it is the xy radius.
        const double radius = std::sqrt(x * x + y * y);
        worstRadius = std::max(worstRadius, std::abs(radius - 0.25));
        // A ring sits on one of the two control points and nowhere between.
        CHECK(std::abs(z) < 1e-5 || std::abs(z - 4.0) < 1e-5);

        // The outward normal of a tube about the z axis is the radial
        // direction, and has no component along the axis.
        const double nx = mesh.normals[i * 3 + 0];
        const double ny = mesh.normals[i * 3 + 1];
        const double nz = mesh.normals[i * 3 + 2];
        worstNormal = std::max(worstNormal,
                               std::abs(nx * x + ny * y - radius));
        worstNormal = std::max(worstNormal, std::abs(nz));
    }
    CHECK(worstRadius < 1e-5);
    CHECK(worstNormal < 1e-5);
    std::printf("  curve sweep: %zu vertices, radius error %.2e, normal error %.2e\n",
                vertices, worstRadius, worstNormal);
}

/// Every triangle indexes a vertex that exists, and every vertex is used.
///
/// An index past the end is the failure that produces a device loss rather than
/// a wrong picture, so it is worth asserting rather than discovering.
void TestCurveSweepIndicesAreInRange()
{
    const std::vector<int> counts{4, 3};
    std::vector<float> points;
    for (int i = 0; i < 4; ++i) {
        points.insert(points.end(), {static_cast<float>(i), 0.0f, 0.0f});
    }
    for (int i = 0; i < 3; ++i) {
        points.insert(points.end(), {0.0f, static_cast<float>(i), 2.0f});
    }
    std::string reason;
    const hdclaude::CurveMesh mesh =
        hdclaude::SweepCurves(counts, points, {}, 0.1f, 5, false, &reason);
    CHECK(mesh.Valid());

    const auto vertices = static_cast<std::uint32_t>(mesh.positions.size() / 3);
    std::vector<bool> used(vertices, false);
    std::uint32_t highest = 0;
    for (const std::uint32_t index : mesh.indices) {
        CHECK(index < vertices);
        if (index < vertices) {
            used[index] = true;
            highest = std::max(highest, index);
        }
    }
    CHECK_EQ(mesh.indices.size() % 3, std::size_t(0));
    // Two strands of 4 and 3 rings: (4-1 + 3-1) segments, 5 sides, two
    // triangles a side.
    CHECK_EQ(mesh.indices.size(), std::size_t((3 + 2) * 5 * 2 * 3));
    CHECK_EQ(highest, vertices - 1);
    std::printf("  curve sweep: %zu triangles over %u vertices\n",
                mesh.indices.size() / 3, vertices);
}

/// Input the sweep cannot honour is refused by name rather than half drawn.
void TestCurveSweepRefusesWhatItCannotSweep()
{
    std::string reason;
    // Counts that do not add up to the points given.
    const hdclaude::CurveMesh mismatched = hdclaude::SweepCurves(
        {5}, {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f}, {}, 1.0f, 6, false, &reason);
    CHECK(!mismatched.Valid());
    CHECK(!reason.empty());

    reason.clear();
    // A single-vertex curve has no segment to sweep along.
    const hdclaude::CurveMesh degenerate = hdclaude::SweepCurves(
        {1, 1}, {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f}, {}, 1.0f, 6, false,
        &reason);
    CHECK(!degenerate.Valid());
    CHECK(!reason.empty());

    reason.clear();
    // Widths that match neither the curves nor the points.
    const hdclaude::CurveMesh widths = hdclaude::SweepCurves(
        {2}, {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f}, {1.0f, 2.0f, 3.0f}, 1.0f, 6,
        false, &reason);
    CHECK(!widths.Valid());
    CHECK(!reason.empty());
    std::printf("  curve sweep refuses bad input by name\n");
}

/// SSIM against the cases where its value is known without measuring it.
///
/// A metric that is only ever run on renders can be wrong in a direction that
/// makes every render look fine, and nothing would say so. These are the
/// positions the definition pins down: an image against itself, an image
/// against a scaled copy of itself, an image against noise, and an image
/// against a blurred version of itself -- which is the one that matters, since
/// it is the failure a reconstructor actually has.
void TestSsimMatchesItsDefinition()
{
    constexpr std::uint32_t kSize = 64;
    constexpr std::size_t kPixels = std::size_t(kSize) * kSize;

    // A structured image: a diagonal ramp with a bright square in it and a fine
    // checker over the whole of it, so there is a gradient, an edge and
    // high-frequency detail. A flat field would make every term of the metric
    // degenerate at once, and a smooth one would leave a blur nothing to
    // destroy.
    std::vector<float> image(kPixels * 4, 0.0f);
    for (std::uint32_t y = 0; y < kSize; ++y) {
        for (std::uint32_t x = 0; x < kSize; ++x) {
            const std::size_t index = (std::size_t(y) * kSize + x) * 4;
            const float ramp = float(x + y) / float(2 * kSize);
            const bool square = x > 20 && x < 44 && y > 20 && y < 44;
            const bool checker = ((x / 2) + (y / 2)) % 2 == 0;
            const float value =
                (square ? 0.9f : ramp) + (checker ? 0.05f : -0.05f);
            image[index + 0] = value;
            image[index + 1] = value;
            image[index + 2] = value;
            image[index + 3] = 1.0f;
        }
    }

    // Identical images are exactly one: every term of the product is its own
    // maximum, and no floating-point slack is involved because the numerator
    // and the denominator are literally the same expression.
    const double same =
        hdclaude::Ssim(image.data(), image.data(), kSize, kSize);
    CHECK(std::abs(same - 1.0) < 1e-12);

    // A small constant added everywhere leaves the structure exactly as it was
    // and moves the luminance term alone, so the result is below one and very
    // close to it.
    std::vector<float> brighter = image;
    for (std::size_t i = 0; i < kPixels; ++i) {
        for (int c = 0; c < 3; ++c) {
            brighter[i * 4 + std::size_t(c)] += 0.02f;
        }
    }
    const double offset =
        hdclaude::Ssim(image.data(), brighter.data(), kSize, kSize);
    CHECK(offset < 1.0);
    CHECK(offset > 0.9);

    // A blur keeps the luminance and destroys the detail, so it must score
    // below a shift that kept the detail and moved the luminance a little.
    // This is the ordering a reconstruction metric lives or dies on: if a
    // blurred image scored higher than a slightly shifted one, the metric would
    // reward exactly the failure it exists to catch. How much of each is
    // arbitrary; that blur is punished harder than a shift of comparable
    // magnitude is not.
    std::vector<float> blurred = image;
    for (std::uint32_t y = 1; y + 1 < kSize; ++y) {
        for (std::uint32_t x = 1; x + 1 < kSize; ++x) {
            for (int c = 0; c < 3; ++c) {
                float sum = 0.0f;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const std::size_t index =
                            ((std::size_t(y) + std::size_t(dy)) * kSize +
                             std::size_t(x) + std::size_t(dx)) *
                            4;
                        sum += image[index + std::size_t(c)];
                    }
                }
                blurred[(std::size_t(y) * kSize + x) * 4 + std::size_t(c)] =
                    sum / 9.0f;
            }
        }
    }
    const double blur =
        hdclaude::Ssim(image.data(), blurred.data(), kSize, kSize);
    CHECK(blur < offset);
    CHECK(blur > 0.0);

    // Uncorrelated noise of the same mean scores near nothing. Deterministic
    // noise, because a test that fails one run in fifty is not a test.
    std::vector<float> noise(kPixels * 4, 1.0f);
    std::uint32_t state = 0x12345678u;
    for (std::size_t i = 0; i < kPixels; ++i) {
        state = state * 1664525u + 1013904223u;
        const float value = float(state >> 8) / float(1u << 24);
        for (int c = 0; c < 3; ++c) {
            noise[i * 4 + std::size_t(c)] = value;
        }
    }
    const double random =
        hdclaude::Ssim(image.data(), noise.data(), kSize, kSize);
    CHECK(random < blur);
    CHECK(random < 0.3);

    std::printf("  ssim: identical %.6f, offset %.4f, blurred %.4f, noise %.4f\n",
                same, offset, blur, random);

    // An image too small for the window has no position where the window fits,
    // and says so rather than inventing edge pixels.
    CHECK_EQ(hdclaude::Ssim(image.data(), image.data(), 4, 4), 0.0);
}

/// Temporal instability against sequences whose flicker is known by
/// construction.
void TestTemporalInstabilityMeasuresFlicker()
{
    constexpr std::uint32_t kSize = 8;
    constexpr std::size_t kPixels = std::size_t(kSize) * kSize;

    const auto flat = [](float value) {
        std::vector<float> frame(kPixels * 4, 1.0f);
        for (std::size_t i = 0; i < kPixels; ++i) {
            for (int c = 0; c < 3; ++c) {
                frame[i * 4 + std::size_t(c)] = value;
            }
        }
        return frame;
    };

    // A sequence that does not change does not flicker.
    const std::vector<std::vector<float>> still = {flat(0.5f), flat(0.5f),
                                                   flat(0.5f)};
    CHECK_EQ(hdclaude::TemporalInstability(still, kSize, kSize), 0.0);

    // One that alternates between two values flickers by exactly the ratio of
    // the swing to the mean: 0.4 and 0.6 change by 0.2 about a mean of 0.5,
    // which is 0.4. A closed form, so an implementation that averaged the wrong
    // way or normalised by the wrong thing cannot pass it.
    const std::vector<std::vector<float>> alternating = {flat(0.4f), flat(0.6f),
                                                         flat(0.4f), flat(0.6f)};
    const double flicker =
        hdclaude::TemporalInstability(alternating, kSize, kSize);
    CHECK_NEAR(flicker, 0.4, 1e-6);

    // And a sequence of the same swing at twice the brightness flickers the
    // same amount, because the measure is relative: a reconstructor is not more
    // stable for having been given a darker image.
    const std::vector<std::vector<float>> brighter = {flat(0.8f), flat(1.2f),
                                                      flat(0.8f), flat(1.2f)};
    CHECK_NEAR(hdclaude::TemporalInstability(brighter, kSize, kSize), 0.4, 1e-6);

    // A black sequence does not flicker, rather than dividing by zero.
    const std::vector<std::vector<float>> black = {flat(0.0f), flat(0.0f)};
    CHECK_EQ(hdclaude::TemporalInstability(black, kSize, kSize), 0.0);

    std::printf("  temporal: still 0, alternating %.4f (expected 0.4)\n", flicker);
}

/// The displacement estimator against shifts that are known exactly.
///
/// The image is a product of two sinusoids of different, coprime-ish periods,
/// which makes this a closed form rather than a resampling: the shifted image
/// is *evaluated* at the shifted coordinates instead of being interpolated from
/// the unshifted one, so nothing the estimator is asked to recover has been
/// smeared by the way the test built its input. The periods are long enough
/// that a shift of a whole pixel is unambiguous -- a fine checker would let a
/// one-pixel shift be mistaken for no shift at all, which is a property of the
/// picture and not of the estimator.
void TestShiftEstimatorRecoversAKnownDisplacement()
{
    constexpr std::uint32_t kSize = 96;

    const auto field = [](double x, double y) {
        return 0.5 + 0.35 * std::sin(2.0 * 3.14159265358979323846 * x / 23.0) *
                         std::cos(2.0 * 3.14159265358979323846 * y / 17.0);
    };

    const auto build = [&](double dx, double dy) {
        std::vector<float> image(std::size_t(kSize) * kSize * 4, 1.0f);
        for (std::uint32_t y = 0; y < kSize; ++y) {
            for (std::uint32_t x = 0; x < kSize; ++x) {
                const auto value = float(field(x + dx, y + dy));
                const std::size_t index = (std::size_t(y) * kSize + x) * 4;
                image[index + 0] = value;
                image[index + 1] = value;
                image[index + 2] = value;
            }
        }
        return image;
    };

    const std::vector<float> reference = build(0.0, 0.0);

    // An image against itself has not moved, and the estimator says so exactly
    // rather than to within something.
    const hdclaude::ImageShift still = hdclaude::EstimateShift(
        reference.data(), reference.data(), kSize, kSize);
    CHECK(still.valid);
    CHECK(still.Magnitude() < 1e-9);

    // The tolerance is not tuned: a twentieth of a pixel is an order of
    // magnitude below the half-pixel that would matter to anything asking this
    // question, and what is left at that scale is the bilinear resampling the
    // estimator does internally, not an error in the answer.
    constexpr double kTolerance = 0.05;

    struct Case {
        double dx;
        double dy;
    };
    const Case cases[] = {{0.37, -0.62}, {1.0, 0.0}, {0.0, -1.0}, {-0.5, 0.5}};
    for (const Case& one : cases) {
        const std::vector<float> moved = build(one.dx, one.dy);
        const hdclaude::ImageShift found = hdclaude::EstimateShift(
            reference.data(), moved.data(), kSize, kSize);
        CHECK(found.valid);
        CHECK_NEAR(found.dx, one.dx, kTolerance);
        CHECK_NEAR(found.dy, one.dy, kTolerance);
        std::printf("  shift (%+.2f %+.2f) recovered as (%+.4f %+.4f)\n",
                    one.dx, one.dy, found.dx, found.dy);
    }

    // A flat field has moved by an amount nothing can recover, and the
    // estimator abstains rather than reporting the zero that happens to fall
    // out of the arithmetic.
    const std::vector<float> flat(std::size_t(kSize) * kSize * 4, 0.5f);
    CHECK(!hdclaude::EstimateShift(flat.data(), flat.data(), kSize, kSize).valid);
}

/// The cubic bases against what each one is defined to do.
///
/// A basis matrix is four polynomials, and a transposed or misordered one still
/// produces a smooth curve near the control points -- which is exactly why this
/// cannot be checked by looking at a render. Each basis has a property that
/// pins it: where the curve starts, whether it touches its control points, and
/// that adjacent segments meet. Those are what is asserted.
void TestCubicCurveBasesMatchTheirDefinitions()
{
    // Six control points in a line, evenly spaced along x, so every closed form
    // below is a number rather than a shape.
    std::vector<float> points;
    for (int i = 0; i < 6; ++i) {
        points.push_back(float(i));
        points.push_back(0.0f);
        points.push_back(0.0f);
    }
    const std::vector<int> counts = {6};
    const std::vector<float> widths;

    // --- Segment counts, which are UsdGeomBasisCurves' rules ----------------
    //
    // Six vertices is three B-spline segments ((6-4)/1+1) and refuses to be a
    // whole number of Bezier ones ((6-4) % 3 != 0). Getting this wrong is how a
    // curve set loses its tail without saying so.
    {
        std::string reason;
        const hdclaude::CurvePolylines spline = hdclaude::EvaluateCurves(
            counts, points, widths, hdclaude::CurveBasis::BSpline, false, 1,
            &reason);
        CHECK(spline.Valid());
        CHECK_EQ(spline.vertexCounts.size(), std::size_t(1));
        // Three segments at one sample each, plus the final endpoint.
        CHECK_EQ(spline.vertexCounts[0], 4);

        std::string bezierReason;
        const hdclaude::CurvePolylines bezier = hdclaude::EvaluateCurves(
            counts, points, widths, hdclaude::CurveBasis::Bezier, false, 1,
            &bezierReason);
        CHECK(!bezier.Valid());
        CHECK(!bezierReason.empty());
        std::printf("  curve basis refuses 6 vertices of bezier: %s\n",
                    bezierReason.c_str());
    }

    // --- B-spline: interpolates nothing, and its start is a closed form -----
    //
    // A uniform cubic B-spline begins at (P0 + 4 P1 + P2) / 6, which for
    // collinear unit-spaced points is x = (0 + 4 + 2) / 6 = 1. If the weights
    // were reversed it would begin at (P1 + 4 P2 + P3) / 6 = 2, so this single
    // number separates the two orderings that a smooth-looking curve cannot.
    {
        std::string reason;
        const hdclaude::CurvePolylines spline = hdclaude::EvaluateCurves(
            counts, points, widths, hdclaude::CurveBasis::BSpline, false, 4,
            &reason);
        CHECK(spline.Valid());
        CHECK_NEAR(spline.points[0], 1.0, 1e-5);
        // And it ends at (P3 + 4 P4 + P5) / 6 = (3 + 16 + 5) / 6 = 4.
        const std::size_t last = spline.points.size() / 3 - 1;
        CHECK_NEAR(spline.points[last * 3 + 0], 4.0, 1e-5);
        // Collinear control points give a straight curve: nothing leaves the
        // axis, whatever the weights do along it.
        for (std::size_t i = 0; i < spline.points.size() / 3; ++i) {
            CHECK_NEAR(spline.points[i * 3 + 1], 0.0, 1e-6);
            CHECK_NEAR(spline.points[i * 3 + 2], 0.0, 1e-6);
        }
        std::printf("  bspline over 6 collinear points: starts %.4f, ends "
                    "%.4f (closed form 1 and 4)\n",
                    spline.points[0], spline.points[last * 3 + 0]);
    }

    // --- Catmull-Rom: passes through its interior control points ------------
    //
    // The property that distinguishes it from the B-spline, and the one an
    // artist relies on. It starts at P1 and ends at P4 for six vertices.
    {
        std::string reason;
        const hdclaude::CurvePolylines rom = hdclaude::EvaluateCurves(
            counts, points, widths, hdclaude::CurveBasis::CatmullRom, false, 3,
            &reason);
        CHECK(rom.Valid());
        CHECK_NEAR(rom.points[0], 1.0, 1e-5);
        const std::size_t last = rom.points.size() / 3 - 1;
        CHECK_NEAR(rom.points[last * 3 + 0], 4.0, 1e-5);
        // Every segment boundary lands on a control point: with three samples a
        // segment, index 3 is the joint between the first and second segments
        // and must be exactly P2.
        CHECK_NEAR(rom.points[3 * 3 + 0], 2.0, 1e-5);
    }

    // --- Bezier: interpolates its ends -------------------------------------
    {
        const std::vector<int> seven = {7};
        std::vector<float> control;
        for (int i = 0; i < 7; ++i) {
            control.push_back(float(i));
            control.push_back(0.0f);
            control.push_back(0.0f);
        }
        std::string reason;
        const hdclaude::CurvePolylines bezier = hdclaude::EvaluateCurves(
            seven, control, widths, hdclaude::CurveBasis::Bezier, false, 2,
            &reason);
        CHECK(bezier.Valid());
        // Seven vertices, vstep 3: two segments, so four spans and five points.
        CHECK_EQ(bezier.vertexCounts[0], 5);
        CHECK_NEAR(bezier.points[0], 0.0, 1e-5);
        const std::size_t last = bezier.points.size() / 3 - 1;
        CHECK_NEAR(bezier.points[last * 3 + 0], 6.0, 1e-5);
        // The joint between the two segments is the shared control point P3.
        CHECK_NEAR(bezier.points[2 * 3 + 0], 3.0, 1e-5);
    }

    // --- Adjacent segments meet ---------------------------------------------
    //
    // The check that would catch a stride error. A B-spline segment ends where
    // the next begins, so a polyline sampled from several segments has no jump
    // in it; a wrong vstep would step past a control point and leave one.
    {
        // Points on a circle, so the curve genuinely bends and a discontinuity
        // would show up as a length rather than cancelling along an axis.
        std::vector<float> ring;
        for (int i = 0; i < 8; ++i) {
            const double angle = 2.0 * 3.14159265358979323846 * i / 8.0;
            ring.push_back(float(std::cos(angle)));
            ring.push_back(float(std::sin(angle)));
            ring.push_back(0.0f);
        }
        std::string reason;
        const hdclaude::CurvePolylines spline = hdclaude::EvaluateCurves(
            {8}, ring, widths, hdclaude::CurveBasis::BSpline, false, 2,
            &reason);
        CHECK(spline.Valid());
        double longest = 0.0;
        const std::size_t n = spline.points.size() / 3;
        for (std::size_t i = 1; i < n; ++i) {
            const double dx = spline.points[i * 3 + 0] - spline.points[(i - 1) * 3 + 0];
            const double dy = spline.points[i * 3 + 1] - spline.points[(i - 1) * 3 + 1];
            const double dz = spline.points[i * 3 + 2] - spline.points[(i - 1) * 3 + 2];
            longest = std::max(longest, std::sqrt(dx * dx + dy * dy + dz * dz));
        }
        // The bound comes from the two answers, not from the measurement. Eight
        // control points on a unit circle are 2 sin(pi/8) = 0.765 apart, so a
        // missed joint leaves a span of about that. The curve itself lies
        // inside the control polygon at radius about 0.9 and each segment
        // sampled twice covers 22.5 degrees of it, a chord of about 0.35. A
        // half separates the two with room on both sides and sits at neither.
        std::printf("  bspline joints: longest span %.4f over a unit ring "
                    "(0.35 correct, 0.77 if a joint were missed)\n",
                    longest);
        CHECK(longest < 0.5);
    }

    // --- Widths ride the same basis -----------------------------------------
    //
    // They are `vertex` interpolation in every ALab curve set, so they are
    // evaluated with the positions rather than carried across. A constant or
    // per-curve width means the same before and after and is passed through.
    {
        std::vector<float> perPoint(6, 0.5f);
        std::string reason;
        const hdclaude::CurvePolylines spline = hdclaude::EvaluateCurves(
            counts, points, perPoint, hdclaude::CurveBasis::BSpline, false, 2,
            &reason);
        CHECK(spline.Valid());
        CHECK_EQ(spline.widths.size(), spline.points.size() / 3);
        // A constant width stays constant through any affine basis, because the
        // weights sum to one.
        for (const float width : spline.widths) {
            CHECK_NEAR(width, 0.5, 1e-6);
        }

        const std::vector<float> one = {0.25f};
        const hdclaude::CurvePolylines carried = hdclaude::EvaluateCurves(
            counts, points, one, hdclaude::CurveBasis::BSpline, false, 2,
            &reason);
        CHECK(carried.Valid());
        CHECK_EQ(carried.widths.size(), std::size_t(1));
        CHECK_EQ(carried.widths[0], 0.25f);
    }

    // --- Linear passes through untouched ------------------------------------
    {
        std::string reason;
        const hdclaude::CurvePolylines linear = hdclaude::EvaluateCurves(
            counts, points, widths, hdclaude::CurveBasis::Linear, false, 4,
            &reason);
        CHECK(linear.Valid());
        CHECK_EQ(linear.points.size(), points.size());
        CHECK_EQ(linear.vertexCounts[0], 6);
    }

    // --- A mismatch is reported, not absorbed -------------------------------
    {
        std::string reason;
        const hdclaude::CurvePolylines wrong = hdclaude::EvaluateCurves(
            {5}, points, widths, hdclaude::CurveBasis::BSpline, false, 1,
            &reason);
        CHECK(!wrong.Valid());
        CHECK(!reason.empty());
    }
}

/// Every layer reads a flag the same way, and a stray space does not silence
/// one of them.
///
/// hdClaude read `HDCLAUDE_TRACE` three ways at once: `TfGetenvBool` in the
/// Hydra layer, the first character in the Vulkan context, and "is it set at
/// all" in the acceleration structure -- so `HDCLAUDE_TRACE=0` switched two of
/// the three on, and `"1 "` switched one of them off. That is not hypothetical
/// whitespace: `set HDCLAUDE_TRACE=1 && program` in cmd assigns everything up
/// to the `&&`, the space included, which produced a render that traced its
/// Vulkan stages and none of its lights.
void TestEnvironmentFlagReadsOneWay()
{
    const auto set = [](const char* value) {
#if defined(_MSC_VER)
        _putenv_s("HDCLAUDE_TEST_FLAG", value);
#else
        setenv("HDCLAUDE_TEST_FLAG", value, 1);
#endif
    };

    for (const char* yes : {"1", "true", "TRUE", "yes", "on", " 1", "1 ",
                            "\t1\n", "  true  "}) {
        set(yes);
        CHECK(hdclaude::EnvironmentFlag("HDCLAUDE_TEST_FLAG"));
    }
    for (const char* no : {"0", "false", "FALSE", "no", "off", " 0 ", "0\t"}) {
        set(no);
        CHECK(!hdclaude::EnvironmentFlag("HDCLAUDE_TEST_FLAG"));
    }

    // Unset and unrecognised both take the caller's default rather than a
    // guess. Guessing is how "0" came to mean "trace".
    set("");
    CHECK(!hdclaude::EnvironmentFlag("HDCLAUDE_TEST_FLAG"));
    CHECK(hdclaude::EnvironmentFlag("HDCLAUDE_TEST_FLAG", true));
    set("banana");
    CHECK(!hdclaude::EnvironmentFlag("HDCLAUDE_TEST_FLAG"));
    CHECK(hdclaude::EnvironmentFlag("HDCLAUDE_TEST_FLAG", true));
    CHECK(!hdclaude::EnvironmentFlag("HDCLAUDE_TEST_UNSET_FLAG"));
    CHECK(hdclaude::EnvironmentFlag("HDCLAUDE_TEST_UNSET_FLAG", true));

    // And the value comes back trimmed, since a path or a device name typed
    // beside an `&&` carries the same space.
    set("  RTX 5060 Ti  ");
    CHECK_EQ(hdclaude::EnvironmentValue("HDCLAUDE_TEST_FLAG"),
             std::string("RTX 5060 Ti"));
    set("");
}

}  // namespace

/// The signed area of a tessellation, and whether any of it is degenerate.
///
/// The area is the instrument. A domain covered exactly once has area one; a
/// gap makes it less, an overlap makes it more, and a triangle wound the other
/// way subtracts where it should add. One number catches all three, which no
/// amount of counting triangles does.
double TessellationArea(const hdclaude::QuadTessellation& mesh, bool* degenerate)
{
    double total = 0.0;
    *degenerate = false;
    for (std::size_t t = 0; t < mesh.TriangleCount(); ++t) {
        const std::uint32_t ia = mesh.indices[t * 3 + 0];
        const std::uint32_t ib = mesh.indices[t * 3 + 1];
        const std::uint32_t ic = mesh.indices[t * 3 + 2];
        const double ax = mesh.uv[ia * 2 + 0], ay = mesh.uv[ia * 2 + 1];
        const double bx = mesh.uv[ib * 2 + 0], by = mesh.uv[ib * 2 + 1];
        const double cx = mesh.uv[ic * 2 + 0], cy = mesh.uv[ic * 2 + 1];
        const double area =
            0.5 * ((bx - ax) * (cy - ay) - (cx - ax) * (by - ay));
        if (std::fabs(area) < 1e-12) {
            *degenerate = true;
        }
        total += area;
    }
    return total;
}

/// An edge's rate is a power of two chosen from how many pixels it covers.
void TestEdgeRateIsAPowerOfTwo()
{
    // Shorter than the target is one segment, not none: a face has to have a
    // boundary.
    CHECK_EQ(hdclaude::EdgeTessellationRate(0.0f, 4.0f), 1);
    CHECK_EQ(hdclaude::EdgeTessellationRate(4.0f, 4.0f), 1);
    // Exactly eight times the target is eight segments, not sixteen: the
    // rounding is the same one a refinement level gets.
    CHECK_EQ(hdclaude::EdgeTessellationRate(32.0f, 4.0f), 8);
    // And anything above a power of two goes to the next one.
    CHECK_EQ(hdclaude::EdgeTessellationRate(33.0f, 4.0f), 16);
    CHECK_EQ(hdclaude::EdgeTessellationRate(5.0f, 4.0f), 2);
    // Bounded, so one face cannot ask for more than anything has a chance to
    // refuse.
    CHECK_EQ(hdclaude::EdgeTessellationRate(1.0e9f, 1.0f), hdclaude::kMaxEdgeRate);

    // A level and a rate are one statement in two units, and the conversion is
    // named because mixing them up is quiet. An off-screen floor given as a
    // level and used as a rate held geometry one level coarser than asked,
    // which is indistinguishable from geometry that is far away.
    CHECK_EQ(hdclaude::EdgeRateForLevel(0), 1);
    CHECK_EQ(hdclaude::EdgeRateForLevel(1), 2);
    CHECK_EQ(hdclaude::EdgeRateForLevel(3), 8);
    CHECK_EQ(hdclaude::EdgeRateForLevel(6), 64);
    CHECK_EQ(hdclaude::EdgeRateForLevel(30), hdclaude::kMaxEdgeRate);
    CHECK_EQ(hdclaude::EdgeRateForLevel(-1), 1);

    // And the two agree about what a level means: an edge covering 2^L times
    // the target is exactly level L's worth of halvings.
    for (int level = 0; level <= 6; ++level) {
        const float length = 4.0f * static_cast<float>(1 << level);
        CHECK_EQ(hdclaude::EdgeTessellationRate(length, 4.0f),
                 hdclaude::EdgeRateForLevel(level));
    }
}

/// A quad tessellated at four independent rates covers its domain exactly
/// once, and its boundary is cut exactly where the rates say.
///
/// Those two properties are the whole contract. The first is what makes the
/// face itself sound; the second is what makes it agree with its neighbours,
/// because a neighbour computes the same rate for the shared edge from the
/// same two endpoints and so samples it at the same parameters.
void TestQuadTessellationCoversItsDomain()
{
    using hdclaude::QuadTessellation;
    using hdclaude::TessellateQuad;

    // The degenerate case first: every side one segment is two triangles over
    // four corners and nothing else.
    {
        const int rates[4] = {1, 1, 1, 1};
        const QuadTessellation mesh = TessellateQuad(rates);
        CHECK(mesh.Valid());
        CHECK_EQ(mesh.SampleCount(), std::size_t(4));
        CHECK_EQ(mesh.TriangleCount(), std::size_t(2));
        bool degenerate = false;
        CHECK_NEAR(TessellationArea(mesh, &degenerate), 1.0, 1e-6);
        CHECK(!degenerate);
    }

    // Every combination of rates up to eight, which is 4096 quads and covers
    // every ordering of finer and coarser sides there is.
    const int choices[] = {1, 2, 4, 8, 3, 5};
    std::size_t checked = 0;
    double worstArea = 0.0;
    bool anyDegenerate = false;
    bool anyOutOfRange = false;
    bool anyBadIndex = false;
    for (const int a : choices) {
        for (const int b : choices) {
            for (const int c : choices) {
                for (const int d : choices) {
                    const int rates[4] = {a, b, c, d};
                    const QuadTessellation mesh = TessellateQuad(rates);
                    if (!mesh.Valid()) {
                        anyBadIndex = true;
                        continue;
                    }
                    bool degenerate = false;
                    worstArea = std::max(
                        worstArea,
                        std::fabs(TessellationArea(mesh, &degenerate) - 1.0));
                    anyDegenerate = anyDegenerate || degenerate;

                    for (const float value : mesh.uv) {
                        if (!(value >= 0.0f && value <= 1.0f)) {
                            anyOutOfRange = true;
                        }
                    }
                    for (const std::uint32_t index : mesh.indices) {
                        if (index >= mesh.SampleCount()) {
                            anyBadIndex = true;
                        }
                    }

                    // The boundary, sample by sample, is where the rates put
                    // it. This is the half of the contract a neighbour relies
                    // on, so it is asserted against the arithmetic rather than
                    // against a count.
                    const float corners[4][2] = {
                        {0, 0}, {1, 0}, {1, 1}, {0, 1}};
                    std::size_t at = 0;
                    for (int side = 0; side < 4; ++side) {
                        for (int step = 0; step < rates[side]; ++step, ++at) {
                            const float t = static_cast<float>(step) /
                                            static_cast<float>(rates[side]);
                            const float* from = corners[side];
                            const float* to = corners[(side + 1) % 4];
                            const float u = from[0] + (to[0] - from[0]) * t;
                            const float v = from[1] + (to[1] - from[1]) * t;
                            if (std::fabs(mesh.uv[at * 2 + 0] - u) > 1e-6f ||
                                std::fabs(mesh.uv[at * 2 + 1] - v) > 1e-6f) {
                                anyOutOfRange = true;
                            }
                        }
                    }
                    if (at != mesh.BoundarySampleCount()) {
                        anyBadIndex = true;
                    }
                    ++checked;
                }
            }
        }
    }
    std::printf("  quad tessellation: %zu rate combinations, worst area error "
                "%.3e\n",
                checked, worstArea);
    CHECK_EQ(checked, std::size_t(1296));
    // Exactly once: not 0.999 and not 1.001, because a gap and an overlap are
    // both defects and neither has a tolerance worth granting.
    CHECK(worstArea < 1e-6);
    CHECK(!anyDegenerate);
    CHECK(!anyOutOfRange);
    CHECK(!anyBadIndex);
}

/// Two faces sharing an edge cut it identically, which is what closes the
/// seam between them.
///
/// Asserted the way it actually happens: one quad has the shared edge as its
/// side 1 and the other has it as its side 3 reversed, and the parameters they
/// sample have to be the same set. A tessellator that derived its boundary
/// from anything but the edge's own rate would fail this whatever else it got
/// right.
void TestQuadTessellationAgreesWithItsNeighbour()
{
    // The shared edge's rate is 8; everything else about the two faces
    // differs, which is the point.
    const int left[4] = {2, 8, 4, 1};
    const int right[4] = {16, 2, 1, 8};

    const hdclaude::QuadTessellation a = hdclaude::TessellateQuad(left);
    const hdclaude::QuadTessellation b = hdclaude::TessellateQuad(right);
    CHECK(a.Valid() && b.Valid());
    if (!a.Valid() || !b.Valid()) {
        return;
    }

    // Side 1 of the left quad: u = 1, v climbing.
    std::vector<float> mine;
    std::size_t at = static_cast<std::size_t>(left[0]);
    for (int step = 0; step < left[1]; ++step, ++at) {
        CHECK(std::fabs(a.uv[at * 2 + 0] - 1.0f) < 1e-6f);
        mine.push_back(a.uv[at * 2 + 1]);
    }
    mine.push_back(1.0f);  // the corner the next side owns

    // Side 3 of the right quad: u = 0, v falling.
    std::vector<float> theirs;
    at = static_cast<std::size_t>(right[0] + right[1] + right[2]);
    for (int step = 0; step < right[3]; ++step, ++at) {
        CHECK(std::fabs(b.uv[at * 2 + 0]) < 1e-6f);
        theirs.push_back(b.uv[at * 2 + 1]);
    }
    theirs.push_back(0.0f);
    std::sort(theirs.begin(), theirs.end());

    CHECK_EQ(mine.size(), theirs.size());
    float worst = 0.0f;
    for (std::size_t i = 0; i < mine.size() && i < theirs.size(); ++i) {
        worst = std::max(worst, std::fabs(mine[i] - theirs[i]));
    }
    std::printf("  quad seam: %zu samples each, worst disagreement %.3e\n",
                mine.size(), worst);
    CHECK(worst < 1e-6f);
}

/// The sphere cage is a sphere, and it is laid out the way OpenUSD lays one
/// out.
///
/// The layout is asserted rather than merely produced because it is a
/// compatibility claim: at the default density this has to be the cage
/// hdClaude has always traced, so that adding texture coordinates changes the
/// coordinates and nothing else.
void TestSphereMeshIsASphere()
{
    using hdclaude::GenerateSphereMesh;
    using hdclaude::SphereMesh;

    // OpenUSD's `_SphereToMesh` is ten by ten: nine rings of ten points plus
    // two poles, and ten quads in each of eight strips plus a ten-triangle fan
    // at each pole.
    CHECK_EQ(hdclaude::SphereMeshPointCount(10, 10), std::size_t(92));
    CHECK_EQ(hdclaude::SphereMeshFaceCount(10, 10), std::size_t(100));

    const float radius = 2.5f;
    const SphereMesh mesh = GenerateSphereMesh(10, 10, radius);
    CHECK(mesh.Valid());
    CHECK_EQ(mesh.PointCount(), std::size_t(92));
    CHECK_EQ(mesh.FaceCount(), std::size_t(100));

    // Every point is on the sphere. This is the one assertion that cannot be
    // satisfied by a plausible-looking blob.
    float worst = 0.0f;
    for (std::size_t i = 0; i < mesh.PointCount(); ++i) {
        const float x = mesh.points[i * 3 + 0];
        const float y = mesh.points[i * 3 + 1];
        const float z = mesh.points[i * 3 + 2];
        worst = std::max(worst,
                         std::fabs(std::sqrt(x * x + y * y + z * z) - radius));
    }
    std::printf("  sphere cage: 92 points, worst radius error %.3e\n", worst);
    CHECK(worst < 1.0e-5f);

    // The poles are first and last, on Z, which is where UsdGeomSphere puts
    // them and what the face indices below assume.
    CHECK(std::fabs(mesh.points[0]) < 1e-6f);
    CHECK(std::fabs(mesh.points[1]) < 1e-6f);
    CHECK(std::fabs(mesh.points[2] + radius) < 1e-5f);
    const std::size_t top = mesh.PointCount() - 1;
    CHECK(std::fabs(mesh.points[top * 3 + 0]) < 1e-6f);
    CHECK(std::fabs(mesh.points[top * 3 + 1]) < 1e-6f);
    CHECK(std::fabs(mesh.points[top * 3 + 2] - radius) < 1e-5f);

    // Twenty triangles at the poles and eighty quads between them, and every
    // index names a point that exists.
    std::size_t triangles = 0;
    std::size_t quads = 0;
    std::size_t corners = 0;
    for (const int count : mesh.faceVertexCounts) {
        if (count == 3) ++triangles;
        if (count == 4) ++quads;
        corners += static_cast<std::size_t>(count);
    }
    CHECK_EQ(triangles, std::size_t(20));
    CHECK_EQ(quads, std::size_t(80));
    CHECK_EQ(mesh.faceVertexIndices.size(), corners);
    CHECK_EQ(mesh.faceVaryingUvs.size(), corners * 2);
    bool inRange = true;
    for (const int index : mesh.faceVertexIndices) {
        inRange = inRange && index >= 0 &&
                  static_cast<std::size_t>(index) < mesh.PointCount();
    }
    CHECK(inRange);

    // A sphere with the fewest divisions it can have is two fans back to back
    // and no quads at all, which is the case an off-by-one in the strip loop
    // turns into a crash or an empty mesh.
    const SphereMesh smallest = GenerateSphereMesh(3, 2, 1.0);
    CHECK(smallest.Valid());
    CHECK_EQ(smallest.FaceCount(), std::size_t(6));
    CHECK_EQ(smallest.PointCount(), std::size_t(5));

    // Below the minimum it refuses rather than producing something degenerate.
    CHECK(!GenerateSphereMesh(2, 2, 1.0).Valid());
    CHECK(!GenerateSphereMesh(3, 1, 1.0).Valid());
}

/// The seam closes, which is the whole reason the coordinates are per corner.
///
/// A sphere's longitude wraps, so the vertex carrying u = 0 is the same vertex
/// that ought to carry u = 1. A vertex-interpolated coordinate can only say
/// one of those, and the last column of faces would then run the texture
/// backwards across the entire map.
void TestSphereMeshCoordinatesCloseTheSeam()
{
    const hdclaude::SphereMesh mesh = hdclaude::GenerateSphereMesh(10, 10, 1.0);
    CHECK(mesh.Valid());
    if (!mesh.Valid()) {
        return;
    }

    float lowU = 2.0f;
    float highU = -1.0f;
    float lowV = 2.0f;
    float highV = -1.0f;
    for (std::size_t i = 0; i * 2 + 1 < mesh.faceVaryingUvs.size(); ++i) {
        lowU = std::min(lowU, mesh.faceVaryingUvs[i * 2 + 0]);
        highU = std::max(highU, mesh.faceVaryingUvs[i * 2 + 0]);
        lowV = std::min(lowV, mesh.faceVaryingUvs[i * 2 + 1]);
        highV = std::max(highV, mesh.faceVaryingUvs[i * 2 + 1]);
    }
    std::printf("  sphere uvs: u %.3f..%.3f, v %.3f..%.3f\n", lowU, highU, lowV,
                highV);
    // The map is covered once, corner to corner: the poles reach v = 0 and
    // v = 1, and the seam reaches u = 1.
    CHECK(std::fabs(lowU) < 1e-6f);
    CHECK(std::fabs(highU - 1.0f) < 1e-6f);
    CHECK(std::fabs(lowV) < 1e-6f);
    CHECK(std::fabs(highV - 1.0f) < 1e-6f);

    // And the seam is closed rather than merely reaching one: some corner
    // refers to a *first-column* vertex while carrying u = 1. That pair --
    // one vertex, two coordinates -- is the thing vertex interpolation cannot
    // express, so finding it is finding the seam.
    bool wrapped = false;
    std::size_t corner = 0;
    for (const int count : mesh.faceVertexCounts) {
        for (int i = 0; i < count; ++i, ++corner) {
            const int index = mesh.faceVertexIndices[corner];
            const float u = mesh.faceVaryingUvs[corner * 2 + 0];
            // The first column of every ring: point 1 starts ring one, and the
            // columns repeat every ten points from there.
            const bool firstColumn = index > 0 &&
                                     index < static_cast<int>(
                                                 mesh.PointCount() - 1) &&
                                     ((index - 1) % 10) == 0;
            if (firstColumn && std::fabs(u - 1.0f) < 1e-6f) {
                wrapped = true;
            }
        }
    }
    CHECK(wrapped);
}

/// A camera move is not a reason to refine again.
///
/// The rule Paolo asked for, and the one thing about this feature that is a
/// policy rather than a projection: published geometry is what acceleration
/// structures are built over and what the accumulated film depends on, so a
/// level that followed the camera would rebuild both on every viewport nudge.
/// The view is sampled when there is none, when a refinement setting changes
/// -- a retessellate being one of those settings -- and otherwise only when
/// following was asked for.
void TestTessellationViewIsSampledNotFollowed()
{
    // Nothing at all happens when the level is not adaptive, however much the
    // camera moves and whatever the settings do.
    CHECK(!hdclaude::ShouldSampleView(/*adaptive=*/false, /*haveView=*/false,
                                      /*settingsChanged=*/true,
                                      /*followCamera=*/true));

    // The first frame: there is nothing to derive from yet.
    CHECK(hdclaude::ShouldSampleView(true, false, false, false));

    // A refinement setting changed -- the level, the target, the off-screen
    // floor, or a retessellate, which is a setting like the others.
    CHECK(hdclaude::ShouldSampleView(true, true, true, false));

    // And the case the whole design turns on: a view already exists, nothing
    // was asked for, and following is off. The camera may have moved anywhere;
    // the answer is no.
    CHECK(!hdclaude::ShouldSampleView(true, true, false, false));

    // Unless following was turned on, which is a setting of its own.
    CHECK(hdclaude::ShouldSampleView(true, true, false, true));
}

/// A perspective world-to-clip for a camera at the origin looking down -Z,
/// column-major as GLSL reads it. The OpenGL form, which is what a USD host
/// hands over.
void MakeWorldToClip(float near, float far, float f, float aspect, float m[16])
{
    for (int i = 0; i < 16; ++i) {
        m[i] = 0.0f;
    }
    m[0] = f / aspect;                       // column 0
    m[5] = f;                                // column 1
    m[10] = (far + near) / (near - far);     // column 2
    m[11] = -1.0f;
    m[14] = 2.0f * far * near / (near - far);  // column 3
}

hdclaude::TessellationView ViewAt(float eyeZ, std::uint32_t pixelHeight,
                                  float tanHalfFov)
{
    hdclaude::TessellationView view;
    view.cameraToWorld[12] = 0.0f;
    view.cameraToWorld[13] = 0.0f;
    view.cameraToWorld[14] = eyeZ;
    view.tanHalfFov = tanHalfFov;
    view.pixelHeight = pixelHeight;
    view.valid = true;
    return view;
}

/// The projection a tessellation level is chosen from is a pinhole, and can be
/// read off by hand.
void TestTessellationProjectionIsThePinhole()
{
    const hdclaude::TessellationView view = ViewAt(0.0f, 1000, 0.5f);

    // The frame spans 2 * d * tanHalfFov world units vertically, so at ten
    // units it is ten units tall and a thousand pixels: a hundred pixels to
    // the unit.
    CHECK_NEAR(hdclaude::PixelsPerWorldUnit(view, 10.0f), 100.0, 1e-4);
    CHECK_NEAR(hdclaude::PixelsPerWorldUnit(view, 100.0f), 10.0, 1e-4);
    // No distance, no projection -- and no crash.
    CHECK_EQ(hdclaude::PixelsPerWorldUnit(view, 0.0f), 0.0f);

    const float low[3] = {-1.0f, -1.0f, -1.0f};
    const float high[3] = {1.0f, 1.0f, 1.0f};
    // Eye at +11 on Z, the box reaching +1: ten units between them.
    CHECK_NEAR(hdclaude::DistanceToBounds(ViewAt(11.0f, 1000, 0.5f), low, high),
               10.0, 1e-5);
    // An eye inside the box is as close as an eye can be, which is zero and
    // not the distance to a corner.
    CHECK_EQ(hdclaude::DistanceToBounds(ViewAt(0.0f, 1000, 0.5f), low, high),
             0.0f);
}

/// The level is the one that brings a refined edge to the target pixel length,
/// and ten times the distance is three levels coarser.
void TestTessellationLevelFollowsProjectedSize()
{
    hdclaude::TessellationLimits limits;
    limits.maxLevel = 10;
    limits.minLevel = 0;
    limits.targetEdgePixels = 4.0f;
    limits.maxRefinedFaces = 1u << 30;

    hdclaude::TessellationRequest request;
    request.boundsMin[0] = request.boundsMin[1] = request.boundsMin[2] = -1.0f;
    request.boundsMax[0] = request.boundsMax[1] = request.boundsMax[2] = 1.0f;
    request.coarseEdgeLength = 1.0f;
    request.coarseFaceCount = 1;
    request.facesPerLevel = 4;

    // Ten units away at a hundred pixels to the unit: a one-unit edge covers a
    // hundred pixels, and reaching four needs a factor of 25, which is between
    // sixteen and thirty-two -- five halvings.
    const hdclaude::TessellationChoice near =
        hdclaude::ChooseTessellation(ViewAt(11.0f, 1000, 0.5f), request, limits);
    std::printf("  tessellation: 10 units -> level %d\n", near.level);
    CHECK_EQ(near.level, 5);
    CHECK(!near.offScreen);
    CHECK(!near.budgetClamped);

    // Ten times further is a tenth the pixels, so the ratio falls from 25 to
    // 2.5: two halvings instead of five.
    const hdclaude::TessellationChoice far =
        hdclaude::ChooseTessellation(ViewAt(101.0f, 1000, 0.5f), request, limits);
    std::printf("  tessellation: 100 units -> level %d\n", far.level);
    CHECK_EQ(far.level, 2);

    // Far enough that a whole coarse edge is already inside the target, and
    // the answer is the control cage.
    const hdclaude::TessellationChoice tiny =
        hdclaude::ChooseTessellation(ViewAt(1001.0f, 1000, 0.5f), request, limits);
    CHECK_EQ(tiny.level, 0);

    // A ratio that is exactly a power of two answers exactly that power, not
    // one more: at 32 units an edge covers 31.25 px... so ask for it directly
    // by moving the target instead, which is the same arithmetic without a
    // distance that has to come out round.
    hdclaude::TessellationLimits exact = limits;
    exact.targetEdgePixels = 100.0f / 8.0f;  // ratio exactly 8 at ten units
    const hdclaude::TessellationChoice power =
        hdclaude::ChooseTessellation(ViewAt(11.0f, 1000, 0.5f), request, exact);
    CHECK_EQ(power.level, 3);

    // The floor holds a mesh in the frustum above the control cage when the
    // caller asks it to, and the ceiling is reported rather than silently
    // becoming the answer.
    hdclaude::TessellationLimits floored = limits;
    floored.minLevel = 2;
    CHECK_EQ(hdclaude::ChooseTessellation(ViewAt(1001.0f, 1000, 0.5f), request,
                                          floored)
                 .level,
             2);

    hdclaude::TessellationLimits capped = limits;
    capped.maxLevel = 3;
    const hdclaude::TessellationChoice clamped =
        hdclaude::ChooseTessellation(ViewAt(11.0f, 1000, 0.5f), request, capped);
    CHECK_EQ(clamped.level, 3);
    CHECK_EQ(clamped.requested, 5);
    CHECK(clamped.ceilingClamped);
}

/// Geometry outside the frustum is held at a floor, never dropped to its cage.
///
/// A path tracer sees what the camera does not -- in a mirror, through glass,
/// as a shadow, in every indirect bounce -- so the saving off-screen is a
/// reduction and not a cull.
void TestTessellationHoldsOffScreenGeometryAtAFloor()
{
    hdclaude::TessellationView view = ViewAt(0.0f, 1000, 0.5f);
    MakeWorldToClip(0.1f, 1000.0f, 2.0f, 1.0f, view.worldToClip);
    view.hasClip = true;

    hdclaude::TessellationLimits limits;
    limits.maxLevel = 10;
    limits.offScreenLevel = 1;
    limits.targetEdgePixels = 4.0f;
    limits.maxRefinedFaces = 1u << 30;

    hdclaude::TessellationRequest request;
    request.coarseEdgeLength = 1.0f;
    request.coarseFaceCount = 1;
    request.facesPerLevel = 4;

    // In front of the camera, which looks down -Z.
    const float inFrontMin[3] = {-1.0f, -1.0f, -11.0f};
    const float inFrontMax[3] = {1.0f, 1.0f, -9.0f};
    std::copy(inFrontMin, inFrontMin + 3, request.boundsMin);
    std::copy(inFrontMax, inFrontMax + 3, request.boundsMax);
    CHECK(!hdclaude::BoundsAreOffScreen(view, request.boundsMin,
                                        request.boundsMax));
    const hdclaude::TessellationChoice visible =
        hdclaude::ChooseTessellation(view, request, limits);
    CHECK(!visible.offScreen);
    CHECK(visible.level > limits.offScreenLevel);

    // Behind the camera: outside the near plane, and every bit as able to
    // appear in a reflection.
    const float behindMin[3] = {-1.0f, -1.0f, 9.0f};
    const float behindMax[3] = {1.0f, 1.0f, 11.0f};
    std::copy(behindMin, behindMin + 3, request.boundsMin);
    std::copy(behindMax, behindMax + 3, request.boundsMax);
    CHECK(hdclaude::BoundsAreOffScreen(view, request.boundsMin,
                                       request.boundsMax));
    const hdclaude::TessellationChoice behind =
        hdclaude::ChooseTessellation(view, request, limits);
    CHECK(behind.offScreen);
    CHECK_EQ(behind.level, 1);

    // Off to the side, past the right plane.
    const float asideMin[3] = {999.0f, -1.0f, -11.0f};
    const float asideMax[3] = {1001.0f, 1.0f, -9.0f};
    std::copy(asideMin, asideMin + 3, request.boundsMin);
    std::copy(asideMax, asideMax + 3, request.boundsMax);
    CHECK(hdclaude::BoundsAreOffScreen(view, request.boundsMin,
                                       request.boundsMax));

    // A view with no projection cannot show anything to be outside it, and
    // says so rather than guessing.
    hdclaude::TessellationView unprojected = ViewAt(0.0f, 1000, 0.5f);
    CHECK(!hdclaude::BoundsAreOffScreen(unprojected, request.boundsMin,
                                        request.boundsMax));
}

/// The face budget is what "there is a limit" honestly looks like.
void TestTessellationBudgetOverridesTheCeiling()
{
    CHECK_EQ(hdclaude::RefinedFaceCount(1000, 4, 0), std::size_t(1000));
    CHECK_EQ(hdclaude::RefinedFaceCount(1000, 4, 3), std::size_t(64000));
    // Saturating rather than wrapping: a count that wrapped would fit any
    // budget, which is the one answer that must never come back.
    CHECK_EQ(hdclaude::RefinedFaceCount(std::size_t(1) << 60, 4, 8),
             std::numeric_limits<std::size_t>::max());

    hdclaude::TessellationLimits limits;
    limits.maxLevel = 10;
    limits.targetEdgePixels = 4.0f;
    limits.maxRefinedFaces = 100000;

    hdclaude::TessellationRequest request;
    request.boundsMin[0] = request.boundsMin[1] = request.boundsMin[2] = -1.0f;
    request.boundsMax[0] = request.boundsMax[1] = request.boundsMax[2] = 1.0f;
    request.coarseEdgeLength = 1.0f;
    request.coarseFaceCount = 1000;
    request.facesPerLevel = 4;

    // The projection asks for five, which would be 1,024,000 faces. Three fits
    // in the budget at 64,000 and four does not at 256,000.
    const hdclaude::TessellationChoice choice =
        hdclaude::ChooseTessellation(ViewAt(11.0f, 1000, 0.5f), request, limits);
    std::printf("  tessellation: wanted level %d, budget gave %d\n",
                choice.requested, choice.level);
    CHECK_EQ(choice.requested, 5);
    CHECK_EQ(choice.level, 3);
    CHECK(choice.budgetClamped);
}

/// A view that says nothing gives back the uniform answer, not the cage.
void TestTessellationWithoutAViewIsUniform()
{
    hdclaude::TessellationLimits limits;
    limits.maxLevel = 4;

    hdclaude::TessellationRequest request;
    request.coarseEdgeLength = 1.0f;
    request.coarseFaceCount = 1;

    hdclaude::TessellationView none;  // valid defaults to false
    const hdclaude::TessellationChoice choice =
        hdclaude::ChooseTessellation(none, request, limits);
    CHECK_EQ(choice.level, 4);
    CHECK_EQ(choice.requested, 4);

    // A mesh that cannot say how long its edges are is in the same position:
    // the level it would have had, rather than a downgrade nobody asked for.
    hdclaude::TessellationRequest sizeless;
    sizeless.coarseFaceCount = 1;
    CHECK_EQ(hdclaude::ChooseTessellation(ViewAt(11.0f, 1000, 0.5f), sizeless,
                                          limits)
                 .level,
             4);
}

// ---------------------------------------------------------------------------
// Gaussian splats
// ---------------------------------------------------------------------------

/// Rotate a vector by a quaternion the long way, as q v q*.
///
/// Written out rather than taken from a matrix on purpose: the implementation
/// under test builds a matrix from the same quaternion, and a test that used
/// that matrix would assert only that the code agrees with itself.
void RotateByQuaternion(const float q[4], const double v[3], double out[3])
{
    const double w = q[0], x = q[1], y = q[2], z = q[3];
    // t = q * (0, v)
    const double tw = -(x * v[0] + y * v[1] + z * v[2]);
    const double tx = w * v[0] + y * v[2] - z * v[1];
    const double ty = w * v[1] + z * v[0] - x * v[2];
    const double tz = w * v[2] + x * v[1] - y * v[0];
    // out = t * conjugate(q), imaginary part only, over the squared norm --
    // which is what rotation by a quaternion means when the quaternion has not
    // been normalised, and the splat builder normalises its own.
    const double squaredNorm = w * w + x * x + y * y + z * z;
    const double inverse = squaredNorm > 0.0 ? 1.0 / squaredNorm : 1.0;
    out[0] = (tw * -x + tx * w + ty * -z - tz * -y) * inverse;
    out[1] = (tw * -y + ty * w + tz * -x - tx * -z) * inverse;
    out[2] = (tw * -z + tz * w + tx * -y - ty * -x) * inverse;
}

Splat OneSplat(const float center[3], const float quaternion[4],
               const float scale[3], float opacity,
               SplatKernel kernel = SplatKernel::GaussianEllipsoid)
{
    SplatCloudSource source;
    source.kernel = kernel;
    source.positions = {center[0], center[1], center[2]};
    source.orientations = {quaternion[0], quaternion[1], quaternion[2],
                           quaternion[3]};
    source.scales = {scale[0], scale[1], scale[2]};
    source.opacities = {opacity};
    source.sphericalHarmonicsDegree = 0;
    source.sphericalHarmonics = {1.0f, 1.0f, 1.0f};
    const SplatCloud cloud = BuildSplatCloud(source);
    return cloud.Valid() ? cloud.splats[0] : Splat{};
}

/// One basis function, by asking for a radiance whose only coefficient is it.
double Harmonic(int index, int degree, const double direction[3])
{
    std::vector<float> coefficients(
        SphericalHarmonicsCoefficientCount(degree) * 3, 0.0f);
    coefficients[static_cast<std::size_t>(index) * 3] = 1.0f;
    const float unit[3] = {static_cast<float>(direction[0]),
                           static_cast<float>(direction[1]),
                           static_cast<float>(direction[2])};
    float rgb[3];
    EvaluateSphericalHarmonics(coefficients.data(), degree, unit, rgb);
    return rgb[0];
}

void TestSphericalHarmonicsAreOrthonormal()
{
    // The instrument the rest of the radiance work rests on. A basis that is
    // merely close to orthonormal produces a radiance that is merely close to
    // the one the asset authored, in a way no picture reveals: the error is a
    // smooth tint that varies with view direction.
    //
    // Integrated on a product rule in (cos theta, phi), which is the measure
    // the sphere's area element already is, so no Jacobian is involved.
    const int degree = 3;
    const int functions =
        static_cast<int>(SphericalHarmonicsCoefficientCount(degree));
    const int zSteps = 300;
    const int phiSteps = 600;
    const double dz = 2.0 / zSteps;
    const double dphi = 2.0 * 3.14159265358979323846 / phiSteps;

    std::vector<double> integral(
        static_cast<std::size_t>(functions) * functions, 0.0);
    std::vector<double> value(static_cast<std::size_t>(functions), 0.0);

    for (int iz = 0; iz < zSteps; ++iz) {
        const double z = -1.0 + (iz + 0.5) * dz;
        const double sinTheta = std::sqrt(std::max(0.0, 1.0 - z * z));
        for (int ip = 0; ip < phiSteps; ++ip) {
            const double phi = (ip + 0.5) * dphi;
            const double direction[3] = {sinTheta * std::cos(phi),
                                         sinTheta * std::sin(phi), z};
            for (int i = 0; i < functions; ++i) {
                value[static_cast<std::size_t>(i)] =
                    Harmonic(i, degree, direction);
            }
            for (int i = 0; i < functions; ++i) {
                for (int j = 0; j < functions; ++j) {
                    integral[static_cast<std::size_t>(i) * functions + j] +=
                        value[static_cast<std::size_t>(i)] *
                        value[static_cast<std::size_t>(j)] * dz * dphi;
                }
            }
        }
    }

    for (int i = 0; i < functions; ++i) {
        for (int j = 0; j < functions; ++j) {
            const double expected = i == j ? 1.0 : 0.0;
            CHECK_NEAR(integral[static_cast<std::size_t>(i) * functions + j],
                       expected, 3.0e-3);
        }
    }
}

void TestSphericalHarmonicsMatchTheirClosedForms()
{
    // The published real spherical harmonics, Condon-Shortley phase included,
    // as every 3D Gaussian splatting implementation writes them. Checked
    // against the closed forms rather than against a golden output, so a change
    // of convention here is a failure rather than a new baseline.
    const double c0 = 0.28209479177387814;
    const double c1 = 0.4886025119029199;
    const double c2[5] = {1.0925484305920792, -1.0925484305920792,
                          0.31539156525252005, -1.0925484305920792,
                          0.5462742152960396};
    const double c3[7] = {-0.5900435899266435, 2.890611442640554,
                          -0.4570457994644658, 0.3731763325901154,
                          -0.4570457994644658, 1.445305721320277,
                          -0.5900435899266435};

    const double directions[4][3] = {
        {0.0, 0.0, 1.0},
        {0.6, 0.0, 0.8},
        {-0.36, 0.48, 0.8},
        {0.4242640687, -0.5656854249, -0.7071067812}};

    // The tolerance is float precision, because that is what the API returns:
    // the basis is evaluated in double and the radiance handed back in float.
    for (const auto& d : directions) {
        const double x = d[0], y = d[1], z = d[2];
        const double xx = x * x, yy = y * y, zz = z * z;
        const double xy = x * y, yz = y * z, xz = x * z;

        CHECK_NEAR(Harmonic(0, 3, d), c0, 2.0e-6);

        CHECK_NEAR(Harmonic(1, 3, d), -c1 * y, 2.0e-6);
        CHECK_NEAR(Harmonic(2, 3, d), c1 * z, 2.0e-6);
        CHECK_NEAR(Harmonic(3, 3, d), -c1 * x, 2.0e-6);

        CHECK_NEAR(Harmonic(4, 3, d), c2[0] * xy, 2.0e-6);
        CHECK_NEAR(Harmonic(5, 3, d), c2[1] * yz, 2.0e-6);
        CHECK_NEAR(Harmonic(6, 3, d), c2[2] * (2.0 * zz - xx - yy), 2.0e-6);
        CHECK_NEAR(Harmonic(7, 3, d), c2[3] * xz, 2.0e-6);
        CHECK_NEAR(Harmonic(8, 3, d), c2[4] * (xx - yy), 2.0e-6);

        CHECK_NEAR(Harmonic(9, 3, d), c3[0] * y * (3.0 * xx - yy), 2.0e-6);
        CHECK_NEAR(Harmonic(10, 3, d), c3[1] * xy * z, 2.0e-6);
        CHECK_NEAR(Harmonic(11, 3, d), c3[2] * y * (4.0 * zz - xx - yy), 2.0e-6);
        CHECK_NEAR(Harmonic(12, 3, d),
                   c3[3] * z * (2.0 * zz - 3.0 * xx - 3.0 * yy), 2.0e-6);
        CHECK_NEAR(Harmonic(13, 3, d), c3[4] * x * (4.0 * zz - xx - yy), 2.0e-6);
        CHECK_NEAR(Harmonic(14, 3, d), c3[5] * z * (xx - yy), 2.0e-6);
        CHECK_NEAR(Harmonic(15, 3, d), c3[6] * x * (xx - 3.0 * yy), 2.0e-6);
    }
}

void TestSphericalHarmonicsFallbackIsTheSchemasDcSignal()
{
    // UsdVolParticleFieldSphericalHarmonicsAttributeAPI says a discarded or
    // absent coefficient array should behave as "a SH coefficient corresponding
    // to a DC signal of (0.5, 0.5, 0.5), with degree 0". That pins the basis
    // normalisation, so it is asserted rather than assumed.
    const float dc = SphericalHarmonicsFallbackCoefficient();
    CHECK_NEAR(dc, std::sqrt(3.14159265358979323846), 1.0e-6);

    const float coefficients[3] = {dc, dc, dc};
    const float directions[5][3] = {{0.0f, 0.0f, 1.0f},
                                    {0.0f, 0.0f, -1.0f},
                                    {1.0f, 0.0f, 0.0f},
                                    {0.0f, -1.0f, 0.0f},
                                    {0.577f, 0.577f, 0.577f}};
    for (const auto& direction : directions) {
        float rgb[3];
        EvaluateSphericalHarmonics(coefficients, 0, direction, rgb);
        CHECK_NEAR(rgb[0], 0.5, 1.0e-6);
        CHECK_NEAR(rgb[1], 0.5, 1.0e-6);
        CHECK_NEAR(rgb[2], 0.5, 1.0e-6);
    }
}

void TestSplatKernelIsUnitSigmaOnItsOwnAxes()
{
    // The specification's kernel: standard deviation 1 in the kernel's own
    // space, so one scale along a principal axis is one sigma whatever the
    // scale's magnitude, and the 3-sigma point is where the support ends.
    const float center[3] = {2.0f, -1.0f, 0.5f};
    // 40 degrees about a slanted axis, so no principal axis is a world axis.
    const double angle = 40.0 * 3.14159265358979323846 / 180.0;
    const double axis[3] = {0.4082482905, 0.8164965809, 0.4082482905};
    const float quaternion[4] = {
        static_cast<float>(std::cos(angle * 0.5)),
        static_cast<float>(axis[0] * std::sin(angle * 0.5)),
        static_cast<float>(axis[1] * std::sin(angle * 0.5)),
        static_cast<float>(axis[2] * std::sin(angle * 0.5))};
    const float scale[3] = {0.4f, 0.05f, 1.7f};
    const Splat splat = OneSplat(center, quaternion, scale, 1.0f);

    CHECK_NEAR(
        SplatKernelResponse(splat, SplatKernel::GaussianEllipsoid, center), 1.0,
        1.0e-6);

    for (int principal = 0; principal < 3; ++principal) {
        double unit[3] = {0.0, 0.0, 0.0};
        unit[principal] = 1.0;
        double rotated[3];
        RotateByQuaternion(quaternion, unit, rotated);

        for (const double sigmas : {1.0, -1.0, 2.0, 3.0}) {
            const float point[3] = {
                static_cast<float>(center[0] +
                                   sigmas * scale[principal] * rotated[0]),
                static_cast<float>(center[1] +
                                   sigmas * scale[principal] * rotated[1]),
                static_cast<float>(center[2] +
                                   sigmas * scale[principal] * rotated[2])};
            CHECK_NEAR(SplatKernelResponse(
                           splat, SplatKernel::GaussianEllipsoid, point),
                       std::exp(-0.5 * sigmas * sigmas), 1.0e-5);
        }

        // Just past the support the field is exactly zero, at the radius the
        // specification names rather than at one chosen here.
        const double beyond =
            SplatSupportRadius(SplatKernel::GaussianEllipsoid) + 1.0e-3;
        const float outside[3] = {
            static_cast<float>(center[0] +
                               beyond * scale[principal] * rotated[0]),
            static_cast<float>(center[1] +
                               beyond * scale[principal] * rotated[1]),
            static_cast<float>(center[2] +
                               beyond * scale[principal] * rotated[2])};
        CHECK_EQ(
            SplatKernelResponse(splat, SplatKernel::GaussianEllipsoid, outside),
            0.0f);
    }
}

void TestSplatKernelWithEqualScalesIsIsotropic()
{
    // A structural check that needs no formula: whatever the rotation, three
    // equal scales must make the response a function of distance alone. A
    // transposed rotation or a row-for-column slip fails this and passes the
    // axis test.
    const float center[3] = {0.0f, 0.0f, 0.0f};
    const float quaternion[4] = {0.5f, 0.5f, -0.5f, 0.5f};
    const float scale[3] = {0.3f, 0.3f, 0.3f};
    const Splat splat = OneSplat(center, quaternion, scale, 1.0f);

    const double directions[4][3] = {
        {1.0, 0.0, 0.0},
        {0.0, 1.0, 0.0},
        {0.0, 0.0, 1.0},
        {0.5773502692, 0.5773502692, 0.5773502692}};
    for (const auto& direction : directions) {
        for (const double distance : {0.15, 0.3, 0.6}) {
            const float point[3] = {
                static_cast<float>(distance * direction[0]),
                static_cast<float>(distance * direction[1]),
                static_cast<float>(distance * direction[2])};
            const double sigmas = distance / 0.3;
            CHECK_NEAR(SplatKernelResponse(
                           splat, SplatKernel::GaussianEllipsoid, point),
                       std::exp(-0.5 * sigmas * sigmas), 1.0e-5);
        }
    }
}

void TestSplatRayPeakMatchesASearchAlongTheRay()
{
    // The closed form the traversal kernel mirrors, against a dense search. An
    // analytic peak that is subtly the wrong root produces an image that is
    // merely a little dim, which is why this is asserted rather than looked at.
    std::uint32_t state = 0x9e3779b9u;
    const auto next = [&state]() {
        state = state * 1664525u + 1013904223u;
        return static_cast<double>(state >> 8) / static_cast<double>(1u << 24);
    };

    int tested = 0;
    for (int trial = 0; trial < 24; ++trial) {
        const float center[3] = {static_cast<float>(next() * 4.0 - 2.0),
                                 static_cast<float>(next() * 4.0 - 2.0),
                                 static_cast<float>(next() * 4.0 - 2.0)};
        double raw[4] = {next() * 2.0 - 1.0, next() * 2.0 - 1.0,
                         next() * 2.0 - 1.0, next() * 2.0 - 1.0};
        const double norm = std::sqrt(raw[0] * raw[0] + raw[1] * raw[1] +
                                      raw[2] * raw[2] + raw[3] * raw[3]);
        if (!(norm > 0.0)) {
            continue;
        }
        const float quaternion[4] = {static_cast<float>(raw[0] / norm),
                                     static_cast<float>(raw[1] / norm),
                                     static_cast<float>(raw[2] / norm),
                                     static_cast<float>(raw[3] / norm)};
        const float scale[3] = {static_cast<float>(0.05 + next() * 0.9),
                                static_cast<float>(0.05 + next() * 0.9),
                                static_cast<float>(0.05 + next() * 0.9)};
        const Splat splat = OneSplat(center, quaternion, scale, 1.0f);

        const float origin[3] = {static_cast<float>(next() * 12.0 - 6.0),
                                 static_cast<float>(next() * 12.0 - 6.0),
                                 static_cast<float>(next() * 12.0 - 6.0)};
        double toward[3] = {center[0] - origin[0] + (next() * 2.0 - 1.0) * 0.4,
                            center[1] - origin[1] + (next() * 2.0 - 1.0) * 0.4,
                            center[2] - origin[2] + (next() * 2.0 - 1.0) * 0.4};
        const double length = std::sqrt(toward[0] * toward[0] +
                                        toward[1] * toward[1] +
                                        toward[2] * toward[2]);
        if (!(length > 0.0)) {
            continue;
        }
        const float direction[3] = {static_cast<float>(toward[0] / length),
                                    static_cast<float>(toward[1] / length),
                                    static_cast<float>(toward[2] / length)};

        const float tMin = 0.0f;
        const float tMax = 40.0f;
        const int steps = 200000;
        double bestT = 0.0;
        double bestResponse = 0.0;
        for (int step = 0; step <= steps; ++step) {
            const double t =
                tMin + (tMax - tMin) * (static_cast<double>(step) / steps);
            const float point[3] = {
                static_cast<float>(origin[0] + t * direction[0]),
                static_cast<float>(origin[1] + t * direction[1]),
                static_cast<float>(origin[2] + t * direction[2])};
            const double response = SplatKernelResponse(
                splat, SplatKernel::GaussianEllipsoid, point);
            if (response > bestResponse) {
                bestResponse = response;
                bestT = t;
            }
        }

        float peakT = 0.0f;
        float peakResponse = 0.0f;
        const bool hit =
            SplatRayPeak(splat, SplatKernel::GaussianEllipsoid, origin,
                         direction, tMin, tMax, &peakT, &peakResponse);
        if (bestResponse <= 0.0) {
            // The search found nothing inside the support. The closed form may
            // still report a peak just inside its boundary, so all that is
            // asserted is that it does not claim a response the search missed.
            CHECK(!hit || peakResponse < 1.0e-3f);
            continue;
        }
        CHECK(hit);
        if (!hit) {
            continue;
        }
        ++tested;
        CHECK_NEAR(peakResponse, bestResponse, 1.0e-4);
        CHECK_NEAR(peakT, bestT, 2.0 * (tMax - tMin) / steps + 1.0e-3);
    }
    // The trial set has to actually exercise the thing.
    CHECK(tested >= 12);
}

void TestSplatBoundsHoldTheSupportAndAreTight()
{
    // The boxes the acceleration structure is partitioned over. Too small clips
    // the falloff; too large costs traversal on every ray that misses.
    const float center[3] = {-0.75f, 1.25f, 3.0f};
    const float quaternion[4] = {0.8f, 0.2f, -0.4f, 0.39799497f};
    const float scale[3] = {0.5f, 0.12f, 0.9f};
    const Splat splat = OneSplat(center, quaternion, scale, 1.0f);

    float minimum[3];
    float maximum[3];
    SplatBounds(splat, SplatKernel::GaussianEllipsoid, minimum, maximum);

    const double radius = SplatSupportRadius(SplatKernel::GaussianEllipsoid);
    double reached[3] = {0.0, 0.0, 0.0};
    const int samples = 40000;
    const double golden = 3.14159265358979323846 * (3.0 - std::sqrt(5.0));
    for (int i = 0; i < samples; ++i) {
        // A Fibonacci sphere, so the support is covered evenly rather than
        // sampled thickly at the poles.
        const double z = 1.0 - 2.0 * (i + 0.5) / samples;
        const double r = std::sqrt(std::max(0.0, 1.0 - z * z));
        const double phi = golden * i;
        const double unit[3] = {r * std::cos(phi), r * std::sin(phi), z};

        // A point on the surface of the support: out to the support radius in
        // the kernel's own space, scaled, then rotated.
        const double scaled[3] = {unit[0] * radius * scale[0],
                                  unit[1] * radius * scale[1],
                                  unit[2] * radius * scale[2]};
        double rotated[3];
        RotateByQuaternion(quaternion, scaled, rotated);
        for (int axis = 0; axis < 3; ++axis) {
            const double coordinate = center[axis] + rotated[axis];
            CHECK(coordinate >= minimum[axis] - 1.0e-4);
            CHECK(coordinate <= maximum[axis] + 1.0e-4);
            reached[axis] = std::max(reached[axis], std::fabs(rotated[axis]));
        }
    }

    // Tightness is asserted at the exact extreme rather than at the best of a
    // finite sample set. The support is the image of a ball of radius r under
    // the forward transform M, so the largest coordinate along an axis is
    // r |row(M)| and it is reached at x = r row(M)/|row(M)|. Sampling gets
    // within a covering radius of that and no closer, which is a property of
    // the sample set and not of the bounds.
    double forward[3][3];
    for (int column = 0; column < 3; ++column) {
        double unit[3] = {0.0, 0.0, 0.0};
        unit[column] = scale[column];
        double rotated[3];
        RotateByQuaternion(quaternion, unit, rotated);
        for (int row = 0; row < 3; ++row) {
            forward[row][column] = rotated[row];
        }
    }
    for (int axis = 0; axis < 3; ++axis) {
        const double half = 0.5 * (maximum[axis] - minimum[axis]);
        const double* row = forward[axis];
        const double length =
            std::sqrt(row[0] * row[0] + row[1] * row[1] + row[2] * row[2]);
        CHECK_NEAR(radius * length, half, 1.0e-5 * half + 1.0e-6);
        CHECK_NEAR(0.5 * (maximum[axis] + minimum[axis]), center[axis], 1.0e-5);

        // And the sampled maximum must have got close to it, which is what
        // says the two calculations are describing the same body.
        CHECK(reached[axis] > 0.99 * half);
    }
}

void TestSplatBuildAppliesTheSchemasLengthRules()
{
    // ParticleFieldPositionBaseAPI: positions define the count, a longer array
    // is truncated, and a shorter one is discarded entirely for the attribute's
    // default. Padding a short array would be the silent repair the project does
    // not do, so both outcomes are asserted along with the report.
    SplatCloudSource source;
    source.positions = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 2.0f, 0.0f, 0.0f};
    // One scale too many: truncated.
    source.scales = {0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f,
                     0.5f, 0.5f, 0.5f, 9.0f, 9.0f, 9.0f};
    // One opacity short: discarded, so every particle is fully opaque.
    source.opacities = {0.25f, 0.25f};
    source.sphericalHarmonicsDegree = 0;
    source.sphericalHarmonics = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
                                 1.0f, 1.0f, 1.0f, 1.0f};

    const SplatCloud cloud = BuildSplatCloud(source);
    CHECK_EQ(cloud.Count(), std::size_t(3));
    for (const Splat& splat : cloud.splats) {
        CHECK_EQ(splat.opacity, 1.0f);
        // A scale of 0.5 puts one sigma at 0.5, so the inverse is 2.
        CHECK_NEAR(splat.inverseTransform[0], 2.0, 1.0e-6);
    }
    bool saidTruncated = false;
    bool saidDiscarded = false;
    for (const std::string& report : cloud.reports) {
        if (report.find("scales") != std::string::npos &&
            report.find("truncated") != std::string::npos) {
            saidTruncated = true;
        }
        if (report.find("opacities") != std::string::npos &&
            report.find("discards") != std::string::npos) {
            saidDiscarded = true;
        }
    }
    CHECK(saidTruncated);
    CHECK(saidDiscarded);
}

void TestSplatBuildFallsBackToTheSchemasRadiance()
{
    // No coefficients at all: degree 0, and the DC signal the schema names.
    SplatCloudSource source;
    source.positions = {0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f};
    source.sphericalHarmonicsDegree = 3;

    const SplatCloud cloud = BuildSplatCloud(source);
    CHECK_EQ(cloud.Count(), std::size_t(2));
    CHECK_EQ(cloud.sphericalHarmonicsDegree, 0);
    CHECK_EQ(cloud.CoefficientsPerParticle(), std::size_t(1));
    CHECK_EQ(cloud.sphericalHarmonics.size(), std::size_t(6));
    const float direction[3] = {0.0f, 1.0f, 0.0f};
    for (std::size_t particle = 0; particle < cloud.Count(); ++particle) {
        float rgb[3];
        EvaluateSphericalHarmonics(
            cloud.sphericalHarmonics.data() + particle * 3, 0, direction, rgb);
        CHECK_NEAR(rgb[0], 0.5, 1.0e-6);
        CHECK_NEAR(rgb[2], 0.5, 1.0e-6);
    }
}

void TestSplatBuildKeepsHarmonicsWithTheirParticles()
{
    // A dropped particle must take its coefficients with it, or every particle
    // after it wears the radiance of its neighbour -- which looks like noise in
    // the asset rather than like a bug here.
    SplatCloudSource source;
    source.positions = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 2.0f, 0.0f, 0.0f};
    source.scales = {0.5f, 0.5f, 0.5f, 0.0f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f};
    source.sphericalHarmonicsDegree = 0;
    source.sphericalHarmonics = {1.0f, 0.0f, 0.0f,   // particle 0: red
                                 0.0f, 1.0f, 0.0f,   // particle 1: dropped
                                 0.0f, 0.0f, 1.0f};  // particle 2: blue
    const SplatCloud cloud = BuildSplatCloud(source);
    CHECK_EQ(cloud.Count(), std::size_t(2));
    CHECK_EQ(cloud.sphericalHarmonics.size(), std::size_t(6));
    CHECK_EQ(cloud.sphericalHarmonics[0], 1.0f);
    CHECK_EQ(cloud.sphericalHarmonics[1], 0.0f);
    CHECK_EQ(cloud.sphericalHarmonics[5], 1.0f);
    CHECK_EQ(cloud.splats[1].center[0], 2.0f);

    bool saidDropped = false;
    for (const std::string& report : cloud.reports) {
        if (report.find("dropped") != std::string::npos) {
            saidDropped = true;
        }
    }
    CHECK(saidDropped);
}

void TestSurfletSupportIsADisk()
{
    // The two surflet kernels are flat: opacity on the local XY plane and
    // exactly zero off it. A ray meets the plane, which is the one place the
    // flatness changes the intersector rather than the bounds.
    const float center[3] = {0.0f, 0.0f, 0.0f};
    const float identity[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    const float scale[3] = {1.0f, 1.0f, 1.0f};

    const Splat gaussian =
        OneSplat(center, identity, scale, 1.0f, SplatKernel::GaussianSurflet);
    const float onPlane[3] = {1.0f, 0.0f, 0.0f};
    const float offPlane[3] = {1.0f, 0.0f, 0.25f};
    CHECK_NEAR(
        SplatKernelResponse(gaussian, SplatKernel::GaussianSurflet, onPlane),
        std::exp(-0.5), 1.0e-6);
    CHECK_EQ(
        SplatKernelResponse(gaussian, SplatKernel::GaussianSurflet, offPlane),
        0.0f);

    // A ray down -Z crosses the plane at the disk, whatever its own start.
    const float origin[3] = {0.5f, 0.0f, 4.0f};
    const float direction[3] = {0.0f, 0.0f, -1.0f};
    float t = 0.0f;
    float response = 0.0f;
    CHECK(SplatRayPeak(gaussian, SplatKernel::GaussianSurflet, origin, direction,
                       0.0f, 100.0f, &t, &response));
    CHECK_NEAR(t, 4.0, 1.0e-5);
    CHECK_NEAR(response, std::exp(-0.5 * 0.25), 1.0e-6);

    // A ray in the plane never meets it.
    const float grazing[3] = {1.0f, 0.0f, 0.0f};
    CHECK(!SplatRayPeak(gaussian, SplatKernel::GaussianSurflet, origin, grazing,
                        0.0f, 100.0f, &t, &response));

    // The constant surflet is a hard disk of radius one, and its bounds are
    // flat along the axis it has no thickness in.
    const Splat constant =
        OneSplat(center, identity, scale, 1.0f, SplatKernel::ConstantSurflet);
    const float inside[3] = {0.9f, 0.0f, 0.0f};
    const float outside[3] = {1.1f, 0.0f, 0.0f};
    CHECK_EQ(SplatKernelResponse(constant, SplatKernel::ConstantSurflet, inside),
             1.0f);
    CHECK_EQ(
        SplatKernelResponse(constant, SplatKernel::ConstantSurflet, outside),
        0.0f);
    float minimum[3];
    float maximum[3];
    SplatBounds(constant, SplatKernel::ConstantSurflet, minimum, maximum);
    CHECK_NEAR(maximum[0] - minimum[0], 2.0, 1.0e-5);
    CHECK_NEAR(maximum[2] - minimum[2], 0.0, 1.0e-5);
}

int main()
{
    TestEnvironmentFlagReadsOneWay();
    TestEdgeRateIsAPowerOfTwo();
    TestQuadTessellationCoversItsDomain();
    TestQuadTessellationAgreesWithItsNeighbour();
    TestSphereMeshIsASphere();
    TestSphereMeshCoordinatesCloseTheSeam();
    TestTessellationViewIsSampledNotFollowed();
    TestTessellationProjectionIsThePinhole();
    TestTessellationLevelFollowsProjectedSize();
    TestTessellationHoldsOffScreenGeometryAtAFloor();
    TestTessellationBudgetOverridesTheCeiling();
    TestTessellationWithoutAViewIsUniform();
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
    TestMunsellColorCheckerRoundTrips();
    TestEmissionUpsamplingPreservesColourAndMagnitude();
    TestChromaTableMatchesTheFit();
    TestHeroPacketIntegratesUnbiased();
    TestBlackbodyLandsOnThePlanckianLocus();
    TestDispersionReproducesItsAbbeNumber();
    TestSubsurfaceAlbedoInversionRoundTrips();
    TestBlackbodyScaleKeepsLuminanceConstant();
    TestDisplayTransformLeavesTheDiffuseRangeAlone();
    TestDisplayTransformCompressesRatherThanClips();
    TestDisplayTransformSanitisesAndExposes();
    TestCurveSweepIsARadiusFromItsAxis();
    TestCurveSweepIndicesAreInRange();
    TestCurveSweepRefusesWhatItCannotSweep();
    TestCubicCurveBasesMatchTheirDefinitions();
    TestSsimMatchesItsDefinition();
    TestTemporalInstabilityMeasuresFlicker();
    TestShiftEstimatorRecoversAKnownDisplacement();
    TestSphericalHarmonicsAreOrthonormal();
    TestSphericalHarmonicsMatchTheirClosedForms();
    TestSphericalHarmonicsFallbackIsTheSchemasDcSignal();
    TestSplatKernelIsUnitSigmaOnItsOwnAxes();
    TestSplatKernelWithEqualScalesIsIsotropic();
    TestSplatRayPeakMatchesASearchAlongTheRay();
    TestSplatBoundsHoldTheSupportAndAreTight();
    TestSplatBuildAppliesTheSchemasLengthRules();
    TestSplatBuildFallsBackToTheSchemasRadiance();
    TestSplatBuildKeepsHarmonicsWithTheirParticles();
    TestSurfletSupportIsADisk();
    return hdclaude_test::Summarize("hdClaudeCoreTests");
}
