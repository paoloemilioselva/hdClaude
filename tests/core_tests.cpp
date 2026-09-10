#include "test_support.h"

#include "hdclaude/core/hash.h"
#include "hdclaude/core/shader_cache.h"
#include "hdclaude/core/curve_sweep.h"
#include "hdclaude/core/display.h"
#include "hdclaude/core/image_metrics.h"
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
    return hdclaude_test::Summarize("hdClaudeCoreTests");
}
