#include "hdclaude/core/spectrum.h"

#include "hdclaude/core/photometry.h"

#include <algorithm>
#include <cmath>

namespace hdclaude {
namespace {

/// Integration grid for every spectral integral here.
///
/// 5 nm across the visible range. The same step the illuminant table is
/// published at, so the illuminant is sampled at its own knots and contributes
/// no interpolation error of its own.
constexpr float kIntegrationStep = 5.0f;


/// Piecewise Gaussian with independent widths either side of the peak. The
/// building block of the Wyman/Sloan/Shirley colour matching fit.
inline float PiecewiseGaussian(float x, float peak, float widthLow, float widthHigh)
{
    const float t = (x - peak) * (x < peak ? 1.0f / widthLow : 1.0f / widthHigh);
    return std::exp(-0.5f * t * t);
}

// Radziszewski et al.'s visual-response-shaped wavelength density, in the
// parameterisation used by Wilkie et al. (2014). Sampling and density must
// stay consistent with each other; SpectrumTests asserts that numerically.
constexpr float kWavelengthCenter = 538.0f;
constexpr float kWavelengthScale = 0.0072f;          // nm^-1
constexpr float kWavelengthNorm = 0.0039398042f;     // normalises the density
constexpr float kWavelengthCdfA = 0.8569106254f;
constexpr float kWavelengthCdfB = 1.8275019724f;

}  // namespace

Vec3 CieXyzBar(float lambda)
{
    const float x = 1.056f * PiecewiseGaussian(lambda, 599.8f, 37.9f, 31.0f) +
                    0.362f * PiecewiseGaussian(lambda, 442.0f, 16.0f, 26.7f) -
                    0.065f * PiecewiseGaussian(lambda, 501.1f, 20.4f, 26.2f);
    const float y = 0.821f * PiecewiseGaussian(lambda, 568.8f, 46.9f, 40.5f) +
                    0.286f * PiecewiseGaussian(lambda, 530.9f, 16.3f, 31.1f);
    const float z = 1.217f * PiecewiseGaussian(lambda, 437.0f, 11.8f, 36.0f) +
                    0.681f * PiecewiseGaussian(lambda, 459.0f, 26.0f, 13.8f);
    return {x, y, z};
}

float CieYIntegral()
{
    // Computed once by Riemann sum at 1 nm over the visible range. Held as a
    // function rather than a literal so it tracks any change to CieXyzBar.
    static const float kIntegral = [] {
        float total = 0.0f;
        for (float lambda = kLambdaMin; lambda <= kLambdaMax; lambda += 1.0f) {
            total += CieXyzBar(lambda).y;
        }
        return total;
    }();
    return kIntegral;
}

Vec3 XyzToLinearSrgb(const Vec3& c)
{
    return {3.2404542f * c.x - 1.5371385f * c.y - 0.4985314f * c.z,
            -0.9692660f * c.x + 1.8760108f * c.y + 0.0415560f * c.z,
            0.0556434f * c.x - 0.2040259f * c.y + 1.0572252f * c.z};
}

Vec3 LinearSrgbToXyz(const Vec3& c)
{
    return {0.4124564f * c.x + 0.3575761f * c.y + 0.1804375f * c.z,
            0.2126729f * c.x + 0.7151522f * c.y + 0.0721750f * c.z,
            0.0193339f * c.x + 0.1191920f * c.y + 0.9503041f * c.z};
}

float SrgbToLinear(float encoded)
{
    return encoded <= 0.04045f ? encoded / 12.92f
                               : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
}

float LinearToSrgb(float linear)
{
    return linear <= 0.0031308f
               ? linear * 12.92f
               : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
}

float BlackbodyRadiance(float lambda, float kelvin)
{
    if (kelvin <= 0.0f) {
        return 0.0f;
    }
    constexpr double c = 299792458.0;         // m/s
    constexpr double h = 6.62606957e-34;      // J s
    constexpr double kb = 1.3806488e-23;      // J/K

    const double l = static_cast<double>(lambda) * 1e-9;  // nm -> m
    const double l5 = l * l * l * l * l;
    const double exponent = (h * c) / (l * kb * static_cast<double>(kelvin));
    // std::expm1 keeps precision for the long-wavelength tail where the
    // exponent is small and exp(x) - 1 would cancel catastrophically.
    return static_cast<float>((2.0 * h * c * c) / (l5 * std::expm1(exponent)));
}

float NormalizedBlackbody(float lambda, float kelvin)
{
    if (kelvin <= 0.0f) {
        return 0.0f;
    }
    // Wien's displacement law gives the peak wavelength directly, so the
    // normalisation needs no search.
    const float peakLambda = 2.8977721e-3f / kelvin * 1e9f;  // m -> nm
    const float peak = BlackbodyRadiance(peakLambda, kelvin);
    return peak > 0.0f ? BlackbodyRadiance(lambda, kelvin) / peak : 0.0f;
}

namespace {

/// Luminous integral of a spectrum against the illuminant grid.
template <typename Spectrum>
double LuminousIntegral(const Spectrum& spectrum)
{
    double total = 0.0;
    for (float lambda = kLambdaMin; lambda <= kLambdaMax;
         lambda += kIntegrationStep) {
        total += static_cast<double>(spectrum(lambda)) *
                 static_cast<double>(CieXyzBar(lambda).y);
    }
    return total * static_cast<double>(kIntegrationStep);
}

}  // namespace

float BlackbodyLuminousScale(float kelvin)
{
    if (!(kelvin > 0.0f)) {
        return 1.0f;
    }
    static const double reference =
        LuminousIntegral([](float lambda) { return IlluminantD65(lambda); });
    const double blackbody = LuminousIntegral(
        [kelvin](float lambda) { return NormalizedBlackbody(lambda, kelvin); });
    return blackbody > 0.0 ? static_cast<float>(reference / blackbody) : 1.0f;
}

Vec3 XyzToChromaticity(const Vec3& xyz)
{
    const float sum = xyz.x + xyz.y + xyz.z;
    if (!(sum > 0.0f)) {
        return {0.0f, 0.0f, 0.0f};
    }
    return {xyz.x / sum, xyz.y / sum, xyz.z / sum};
}

Vec3 BlackbodyXyz(float kelvin)
{
    Vec3 xyz;
    for (float lambda = kLambdaMin; lambda <= kLambdaMax;
         lambda += kIntegrationStep) {
        xyz += CieXyzBar(lambda) * NormalizedBlackbody(lambda, kelvin);
    }
    return xyz.y > 0.0f ? xyz * (1.0f / xyz.y) : Vec3{};
}

float DispersedIor(float ior, float abbe, float lambda)
{
    if (!(abbe > 0.0f)) {
        return ior;
    }

    // n = A + B / lambda^2, with B fixed by the Abbe number's definition and A
    // by the requirement that the curve pass through `ior` at the d-line.
    const double inverseF = 1.0 / (double(kFraunhoferF) * kFraunhoferF);
    const double inverseC = 1.0 / (double(kFraunhoferC) * kFraunhoferC);
    const double inverseD = 1.0 / (double(kFraunhoferD) * kFraunhoferD);

    const double b = (double(ior) - 1.0) / (double(abbe) * (inverseF - inverseC));
    const double a = double(ior) - b * inverseD;

    const double safe = std::max(1.0, double(lambda));
    return static_cast<float>(a + b / (safe * safe));
}

float VanDeHulstDiffuseAlbedo(float albedo, float g)
{
    const double a = std::min(std::max(double(albedo), 0.0), 1.0);
    const double anisotropy = std::min(std::max(double(g), -0.99), 0.99);

    const double denominator = 1.0 - a * anisotropy;
    if (!(denominator > 0.0)) {
        return 1.0f;
    }
    const double s = std::sqrt(std::max(0.0, (1.0 - a) / denominator));
    return static_cast<float>((1.0 - s) * (1.0 - 0.139 * s) / (1.0 + 1.17 * s));
}

float SubsurfaceSingleScatteringAlbedo(float reflectance, float g)
{
    const double c = std::min(std::max(double(reflectance), 0.0), 1.0);
    const double anisotropy = std::min(std::max(double(g), -0.99), 0.99);

    // OpenPBR's inversion, quoted. The square root's argument is a positive
    // quadratic in C over [0, 1], so it needs no guard of its own; `s` is
    // clamped because the fit overshoots by about a thousandth at each end and
    // an `s` of -0.00087 at C = 1 would return an albedo above one.
    const double s = std::min(
        std::max(4.09712 + 4.20863 * c -
                     std::sqrt(9.59217 + 41.6808 * c + 17.7126 * c * c),
                 0.0),
        1.0);

    const double sSquared = s * s;
    const double denominator = 1.0 - anisotropy * sSquared;
    if (!(denominator > 0.0)) {
        return 1.0f;
    }
    return static_cast<float>(
        std::min(std::max((1.0 - sSquared) / denominator, 0.0), 1.0));
}

float SampleVisibleWavelength(float u)
{
    return kWavelengthCenter -
           (1.0f / kWavelengthScale) *
               std::atanh(kWavelengthCdfA - kWavelengthCdfB * u);
}

float VisibleWavelengthPdf(float lambda)
{
    // The sampler's support is exactly [kLambdaMin, kLambdaMax]: the inverse
    // CDF maps u = 0 to 360 nm and u = 1 to 830 nm. Outside that range the
    // density is zero by construction, and the analytic form's tails must not
    // be returned -- a nonzero density for a wavelength the sampler can never
    // produce breaks the MIS weights that combine light and BSDF sampling.
    if (lambda < kLambdaMin || lambda > kLambdaMax) {
        return 0.0f;
    }
    const float c = std::cosh(kWavelengthScale * (lambda - kWavelengthCenter));
    return kWavelengthNorm / (c * c);
}

WavelengthSample SampleHeroWavelengths(float u)
{
    WavelengthSample sample;
    sample.lambda[0] = SampleVisibleWavelength(std::clamp(u, 0.0f, 1.0f));

    constexpr float kRange = kLambdaMax - kLambdaMin;
    constexpr float kStride = kRange / static_cast<float>(kSpectralLanes);

    // Every lane carries the *hero's* density, not its own.
    //
    // A rotated lane is not an independent draw: it is `lambda_0` shifted by a
    // fixed amount and wrapped, which is a bijection of the visible range onto
    // itself with unit Jacobian. The density of that random variable at the
    // value it took is therefore the density of the variable it was derived
    // from -- p(lambda_0) -- and not p(lambda_i), which is the density of a
    // different random variable that happens to share the value.
    //
    // Dividing by p(lambda_i) instead is biased, and biased in a way no image
    // would reveal: the estimator still converges, smoothly, to the wrong
    // spectrum. `TestHeroPacketIntegratesUnbiased` is what says which of the
    // two this is.
    const float density = VisibleWavelengthPdf(sample.lambda[0]);
    sample.pdf[0] = density;

    for (int i = 1; i < kSpectralLanes; ++i) {
        float lambda = sample.lambda[0] + static_cast<float>(i) * kStride;
        if (lambda > kLambdaMax) {
            lambda -= kRange;
        }
        sample.lambda[static_cast<std::size_t>(i)] = lambda;
        sample.pdf[static_cast<std::size_t>(i)] = density;
    }
    return sample;
}


// ---------------------------------------------------------------------------
// Illuminant D65
// ---------------------------------------------------------------------------

namespace {

/// CIE standard illuminant D65, relative spectral power at 5 nm from 300 nm.
///
/// The published table rather than a fit: D65 has structure -- the Fraunhofer
/// absorption lines it inherits from daylight -- that no smooth approximation
/// reproduces, and the round-trip gate is tight enough to see the difference.
constexpr float kD65Start = 300.0f;
constexpr float kD65Step = 5.0f;
constexpr float kD65[] = {
    0.0341f,   1.6643f,   3.2945f,   11.7652f,  20.2360f,  28.6447f,  37.0535f,
    38.5011f,  39.9488f,  42.4302f,  44.9117f,  45.7750f,  46.6383f,  49.3637f,
    52.0891f,  51.0323f,  49.9755f,  52.3118f,  54.6482f,  68.7015f,  82.7549f,
    87.1204f,  91.4860f,  92.4589f,  93.4318f,  90.0570f,  86.6823f,  95.7736f,
    104.8650f, 110.9360f, 117.0080f, 117.4100f, 117.8120f, 116.3360f, 114.8610f,
    115.3920f, 115.9230f, 112.3670f, 108.8110f, 109.0820f, 109.3540f, 108.5780f,
    107.8020f, 106.2960f, 104.7900f, 106.2390f, 107.6890f, 106.0470f, 104.4050f,
    104.2250f, 104.0460f, 102.0230f, 100.0000f, 98.1671f,  96.3342f,  96.0611f,
    95.7880f,  92.2368f,  88.6856f,  89.3459f,  90.0062f,  89.8026f,  89.5991f,
    88.6489f,  87.6987f,  85.4936f,  83.2886f,  83.4939f,  83.6992f,  81.8630f,
    80.0268f,  80.1207f,  80.2146f,  81.2462f,  82.2778f,  80.2810f,  78.2842f,
    74.0027f,  69.7213f,  70.6652f,  71.6091f,  72.9790f,  74.3490f,  67.9765f,
    61.6040f,  65.7448f,  69.8856f,  72.4863f,  75.0870f,  69.3398f,  63.5927f,
    55.0054f,  46.4182f,  56.6118f,  66.8054f,  65.0941f,  63.3828f,  63.8434f,
    64.3040f,  61.8779f,  59.4519f,  55.7054f,  51.9590f,  54.6998f,  57.4406f,
    58.8765f,  60.3125f,
};
constexpr int kD65Count = static_cast<int>(sizeof(kD65) / sizeof(kD65[0]));

}  // namespace

float IlluminantD65(float lambda)
{
    const float position = (lambda - kD65Start) / kD65Step;
    if (position <= 0.0f) {
        return kD65[0];
    }
    if (position >= static_cast<float>(kD65Count - 1)) {
        return kD65[kD65Count - 1];
    }
    const int index = static_cast<int>(position);
    const float t = position - static_cast<float>(index);
    return kD65[index] * (1.0f - t) + kD65[index + 1] * t;
}

namespace {

/// The wavelength coordinate the polynomial is written in.
///
/// Normalised to [0, 1] rather than nanometres so the three coefficients are of
/// comparable magnitude, which is what keeps the fit's normal equations well
/// conditioned.
inline float PolynomialArgument(float lambda)
{
    return (lambda - kLambdaMin) / (kLambdaMax - kLambdaMin);
}

/// Everything spectral here runs in double, and that is not an optimisation.
///
/// Near white the sigmoid is saturating: the difference between a reflectance
/// of 1 and one of 1 - 1e-8 is what separates a converged fit from one that is
/// visibly off, and in float that difference is below the noise floor of the
/// integral itself.
struct Vec3d {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

/// XYZ of a sigmoid reflectance under D65, before white adaptation.
///
/// Normalised by the illuminant's own luminous integral, so a reflectance of
/// one gives Y = 1 -- but not, in general, X and Z that agree with the colour
/// space's white point. See `WhiteAdaptation`.
Vec3d IntegrateSigmoidRaw(double c0, double c1, double c2)
{
    static const double normalisation = [] {
        double total = 0.0;
        for (float lambda = kLambdaMin; lambda <= kLambdaMax;
             lambda += kIntegrationStep) {
            total += static_cast<double>(IlluminantD65(lambda)) *
                     static_cast<double>(CieXyzBar(lambda).y);
        }
        return total * static_cast<double>(kIntegrationStep);
    }();

    Vec3d xyz;
    for (float lambda = kLambdaMin; lambda <= kLambdaMax;
         lambda += kIntegrationStep) {
        const double t = static_cast<double>(PolynomialArgument(lambda));
        const double p = (c0 * t + c1) * t + c2;
        const double reflectance = 0.5 * (1.0 + p / std::sqrt(1.0 + p * p));
        const double weight =
            reflectance * static_cast<double>(IlluminantD65(lambda));
        const Vec3 bar = CieXyzBar(lambda);
        xyz.x += static_cast<double>(bar.x) * weight;
        xyz.y += static_cast<double>(bar.y) * weight;
        xyz.z += static_cast<double>(bar.z) * weight;
    }
    const double scale = static_cast<double>(kIntegrationStep) / normalisation;
    return {xyz.x * scale, xyz.y * scale, xyz.z * scale};
}

/// The diagonal correction that pins a perfect reflector to the colour space's
/// white.
///
/// Both halves of this integral are approximations: the colour matching
/// functions are Wyman's multi-lobe fit, good to about a per cent of peak, and
/// the illuminant is a published table sampled at 5 nm. Their product's white
/// point therefore lands a fraction of a per cent away from the D65 the sRGB
/// matrix was derived against -- (0.9994, 1.0002, 0.99995) rather than
/// (1, 1, 1).
///
/// Left alone, that is not a rounding detail: it means a perfect white diffuse
/// surface under the scene's own illuminant does not render white, and no
/// amount of fitting can hide it, because a reflectance of one is the sigmoid's
/// boundary and there is nothing left to trade. Every other colour absorbs the
/// error into its fit and looks fine, which is exactly what makes it worth
/// pinning rather than tolerating.
///
/// So the integral is adapted to the white a perfect reflector must produce.
/// This is a von Kries adaptation done in XYZ, which for a correction this
/// small is indistinguishable from doing it in a cone space.
Vec3d WhiteAdaptation()
{
    static const Vec3d scale = [] {
        const Vec3d raw = IntegrateSigmoidRaw(0.0, 0.0, 1.0e12);
        const Vec3 reference = LinearSrgbToXyz(Vec3{1.0f, 1.0f, 1.0f});
        return Vec3d{static_cast<double>(reference.x) / raw.x,
                     static_cast<double>(reference.y) / raw.y,
                     static_cast<double>(reference.z) / raw.z};
    }();
    return scale;
}

/// XYZ of a sigmoid reflectance under D65, adapted. A reflectance of one gives
/// exactly the colour space's white.
Vec3d IntegrateSigmoidXyz(double c0, double c1, double c2)
{
    const Vec3d raw = IntegrateSigmoidRaw(c0, c1, c2);
    const Vec3d scale = WhiteAdaptation();
    return {raw.x * scale.x, raw.y * scale.y, raw.z * scale.z};
}

Vec3d LabDouble(const Vec3d& xyz)
{
    const Vec3 whiteF = LinearSrgbToXyz(Vec3{1.0f, 1.0f, 1.0f});
    const Vec3d white{static_cast<double>(whiteF.x),
                      static_cast<double>(whiteF.y),
                      static_cast<double>(whiteF.z)};
    const auto f = [](double ratio) {
        constexpr double delta = 6.0 / 29.0;
        return ratio > delta * delta * delta
                   ? std::cbrt(ratio)
                   : ratio / (3.0 * delta * delta) + 4.0 / 29.0;
    };
    const double fx = f(xyz.x / white.x);
    const double fy = f(xyz.y / white.y);
    const double fz = f(xyz.z / white.z);
    return {116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz)};
}

}  // namespace

Vec3 D65WhitePoint()
{
    // The white a perfect reflector produces, which the integral is adapted to
    // and which therefore is the colour space's own white by construction.
    return LinearSrgbToXyz(Vec3{1.0f, 1.0f, 1.0f});
}

// ---------------------------------------------------------------------------
// RGB to spectrum
// ---------------------------------------------------------------------------

float ReflectanceSigmoid(float x)
{
    // 0.5 * (1 + x / sqrt(1 + x^2)): smooth, strictly inside (0, 1), and
    // saturating slowly enough that a fit can reach a near-zero or near-one
    // reflectance without its coefficients running away.
    return 0.5f * (1.0f + x / std::sqrt(1.0f + x * x));
}

float EvaluateReflectance(const SigmoidCoefficients& coefficients, float lambda)
{
    const float t = PolynomialArgument(lambda);
    return ReflectanceSigmoid((coefficients.c0 * t + coefficients.c1) * t +
                              coefficients.c2);
}

Vec3 XyzToLab(const Vec3& xyz)
{
    const Vec3 white = D65WhitePoint();
    const auto f = [](float ratio) {
        constexpr float delta = 6.0f / 29.0f;
        return ratio > delta * delta * delta
                   ? std::cbrt(ratio)
                   : ratio / (3.0f * delta * delta) + 4.0f / 29.0f;
    };
    const float fx = f(xyz.x / white.x);
    const float fy = f(xyz.y / white.y);
    const float fz = f(xyz.z / white.z);
    return {116.0f * fy - 16.0f, 500.0f * (fx - fy), 200.0f * (fy - fz)};
}

namespace {

/// The chromaticity of a colour, and the magnitude divided out of it.
struct Decomposed {
    Vec3 chroma{0.0f, 0.0f, 0.0f};
    float scale = 0.0f;
    int face = 0;   // which channel was largest
};

Decomposed Decompose(const Vec3& linearSrgb)
{
    Decomposed out;
    const float channels[3] = {std::max(0.0f, linearSrgb.x),
                               std::max(0.0f, linearSrgb.y),
                               std::max(0.0f, linearSrgb.z)};
    out.face = 0;
    for (int k = 1; k < 3; ++k) {
        if (channels[k] > channels[out.face]) {
            out.face = k;
        }
    }
    out.scale = channels[out.face];
    if (!(out.scale > 0.0f)) {
        return out;
    }
    out.chroma = Vec3{channels[0] / out.scale, channels[1] / out.scale,
                      channels[2] / out.scale};
    return out;
}

/// Fit the three coefficients of a chromaticity, seeded from `seed`.
///
/// Split out from `FitSpectrum` because the table builder wants to seed each
/// fit from its neighbour, which is the whole reason the table is cheap to
/// build.
SigmoidCoefficients FitChroma(const Vec3& chroma, const SigmoidCoefficients& seed,
                              bool useSeed)
{
    const Vec3 targetXyz = LinearSrgbToXyz(chroma);
    const Vec3d targetLab = LabDouble({static_cast<double>(targetXyz.x),
                                       static_cast<double>(targetXyz.y),
                                       static_cast<double>(targetXyz.z)});

    double coefficients[3];
    if (useSeed) {
        coefficients[0] = seed.c0;
        coefficients[1] = seed.c1;
        coefficients[2] = seed.c2;
    } else {
        // Seed with the flat spectrum whose luminance is already the target's,
        // found by bisection on the constant term.
        //
        // This is not a nicety. A chromaticity always has a component at one,
        // and the sigmoid reaches one only in the limit, so Levenberg-Marquardt
        // starts against an asymptote: every step it tries improves by less
        // than the last, its damping escalates, and it gives up short.
        // Bisection has no such trouble, because it never needs a gradient.
        coefficients[0] = 0.0;
        coefficients[1] = 0.0;
        const double targetY = static_cast<double>(targetXyz.y);
        double low = -2.0e4;
        double high = 2.0e4;
        for (int step = 0; step < 200; ++step) {
            const double middle = 0.5 * (low + high);
            if (IntegrateSigmoidXyz(0.0, 0.0, middle).y < targetY) {
                low = middle;
            } else {
                high = middle;
            }
        }
        coefficients[2] = 0.5 * (low + high);
    }

    const auto residual = [&](const double c[3], double out[3]) {
        const Vec3d lab = LabDouble(IntegrateSigmoidXyz(c[0], c[1], c[2]));
        out[0] = lab.x - targetLab.x;
        out[1] = lab.y - targetLab.y;
        out[2] = lab.z - targetLab.z;
    };
    const auto squaredNorm = [](const double v[3]) {
        return v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    };

    double error[3];
    residual(coefficients, error);
    double damping = 1.0e-3;

    for (int iteration = 0; iteration < 600; ++iteration) {
        if (squaredNorm(error) < 1.0e-18) {
            break;
        }

        // Numerical Jacobian, with the step scaled to the coefficient it
        // perturbs: a saturated colour's fit runs to coefficients in the
        // hundreds, where a fixed absolute step measures nothing.
        double jacobian[3][3];
        for (int k = 0; k < 3; ++k) {
            const double step =
                1.0e-3 * std::max(1.0, std::fabs(coefficients[k]));
            double perturbed[3] = {coefficients[0], coefficients[1],
                                   coefficients[2]};
            perturbed[k] += step;
            double shifted[3];
            residual(perturbed, shifted);
            for (int row = 0; row < 3; ++row) {
                jacobian[row][k] = (shifted[row] - error[row]) / step;
            }
        }

        double ata[3][3] = {};
        double atr[3] = {};
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 3; ++column) {
                for (int k = 0; k < 3; ++k) {
                    ata[row][column] += jacobian[k][row] * jacobian[k][column];
                }
            }
            for (int k = 0; k < 3; ++k) {
                atr[row] += jacobian[k][row] * error[k];
            }
        }
        for (int k = 0; k < 3; ++k) {
            ata[k][k] *= (1.0 + damping);
        }

        double matrix[3][4] = {{ata[0][0], ata[0][1], ata[0][2], -atr[0]},
                               {ata[1][0], ata[1][1], ata[1][2], -atr[1]},
                               {ata[2][0], ata[2][1], ata[2][2], -atr[2]}};
        bool singular = false;
        for (int column = 0; column < 3 && !singular; ++column) {
            int pivot = column;
            for (int row = column + 1; row < 3; ++row) {
                if (std::fabs(matrix[row][column]) >
                    std::fabs(matrix[pivot][column])) {
                    pivot = row;
                }
            }
            if (std::fabs(matrix[pivot][column]) < 1.0e-300) {
                singular = true;
                break;
            }
            for (int k = 0; k < 4; ++k) {
                std::swap(matrix[column][k], matrix[pivot][k]);
            }
            for (int row = 0; row < 3; ++row) {
                if (row == column) continue;
                const double factor =
                    matrix[row][column] / matrix[column][column];
                for (int k = column; k < 4; ++k) {
                    matrix[row][k] -= factor * matrix[column][k];
                }
            }
        }
        if (singular) {
            break;
        }

        double candidate[3];
        for (int k = 0; k < 3; ++k) {
            candidate[k] = coefficients[k] + matrix[k][3] / matrix[k][k];
        }

        double candidateError[3];
        residual(candidate, candidateError);
        if (squaredNorm(candidateError) < squaredNorm(error)) {
            for (int k = 0; k < 3; ++k) {
                coefficients[k] = candidate[k];
                error[k] = candidateError[k];
            }
            damping = std::max(damping * 0.3, 1.0e-9);
        } else {
            damping *= 8.0;
            if (damping > 1.0e12) {
                break;
            }
        }
    }

    return {static_cast<float>(coefficients[0]),
            static_cast<float>(coefficients[1]),
            static_cast<float>(coefficients[2])};
}

}  // namespace

SpectrumFit FitSpectrum(const Vec3& linearSrgb)
{
    const Decomposed decomposed = Decompose(linearSrgb);
    SpectrumFit fit;
    fit.scale = decomposed.scale;
    if (!(decomposed.scale > 0.0f)) {
        // Black emits and reflects nothing; a fit would be meaningless and its
        // coefficients whatever the optimiser wandered into.
        fit.chroma = {0.0f, 0.0f, -1.0e4f};
        return fit;
    }
    fit.chroma = FitChroma(decomposed.chroma, {}, false);
    return fit;
}

float EvaluateSpectrum(const SpectrumFit& fit, float lambda)
{
    return fit.scale * EvaluateReflectance(fit.chroma, lambda);
}

Vec3 IntegrateSpectrum(const SpectrumFit& fit)
{
    const Vec3d xyz = IntegrateSigmoidXyz(static_cast<double>(fit.chroma.c0),
                                          static_cast<double>(fit.chroma.c1),
                                          static_cast<double>(fit.chroma.c2));
    const Vec3 rgb = XyzToLinearSrgb(Vec3{static_cast<float>(xyz.x),
                                          static_cast<float>(xyz.y),
                                          static_cast<float>(xyz.z)});
    return rgb * fit.scale;
}

// ---------------------------------------------------------------------------
// Tabulated spectra
// ---------------------------------------------------------------------------

std::string ValidateTabulatedSpectrum(const TabulatedSpectrum& spectrum,
                                      bool reflectance)
{
    const std::size_t count = spectrum.wavelengths.size();
    if (count != spectrum.values.size()) {
        return "has " + std::to_string(count) + " wavelengths but " +
               std::to_string(spectrum.values.size()) + " values";
    }
    if (count < 2) {
        return "has " + std::to_string(count) +
               " samples; a spectrum needs at least two";
    }
    for (std::size_t i = 0; i < count; ++i) {
        const float lambda = spectrum.wavelengths[i];
        const float value = spectrum.values[i];
        if (!std::isfinite(lambda) || !std::isfinite(value)) {
            return "sample " + std::to_string(i) + " is not finite";
        }
        if (i > 0 && !(lambda > spectrum.wavelengths[i - 1])) {
            return "wavelength " + std::to_string(i) + " (" +
                   std::to_string(lambda) +
                   " nm) does not increase on the one before it";
        }
        if (value < 0.0f) {
            return "value " + std::to_string(i) + " is negative (" +
                   std::to_string(value) + ")";
        }
        if (reflectance && value > 1.0f) {
            return "reflectance " + std::to_string(i) + " exceeds one (" +
                   std::to_string(value) +
                   "), which would reflect more light than arrives";
        }
    }
    return {};
}

float EvaluateTabulated(const TabulatedSpectrum& spectrum, float lambda)
{
    const std::vector<float>& x = spectrum.wavelengths;
    const std::vector<float>& y = spectrum.values;
    if (x.size() < 2 || x.size() != y.size() || lambda < x.front() ||
        lambda > x.back()) {
        return 0.0f;
    }
    // The first sample strictly above lambda; the segment ends there.
    const auto upper = std::upper_bound(x.begin(), x.end(), lambda);
    if (upper == x.end()) {
        return y.back();  // lambda is exactly the last sample
    }
    const std::size_t high = static_cast<std::size_t>(upper - x.begin());
    const std::size_t low = high - 1;
    const float t = (lambda - x[low]) / (x[high] - x[low]);
    return y[low] + (y[high] - y[low]) * t;
}

Vec3 TabulatedReflectanceToLinearSrgb(const TabulatedSpectrum& spectrum)
{
    // The integral IntegrateSigmoidRaw takes of a sigmoid, taken of the table
    // instead, with the same grid, the same normalisation and the same white
    // adaptation -- so the two are metamers under D65 by construction rather
    // than by agreeing on a convention in two places.
    double normalisation = 0.0;
    Vec3d xyz;
    for (float lambda = kLambdaMin; lambda <= kLambdaMax;
         lambda += kIntegrationStep) {
        const double illuminant = static_cast<double>(IlluminantD65(lambda));
        const Vec3 bar = CieXyzBar(lambda);
        normalisation += illuminant * static_cast<double>(bar.y);
        const double weight =
            static_cast<double>(EvaluateTabulated(spectrum, lambda)) * illuminant;
        xyz.x += static_cast<double>(bar.x) * weight;
        xyz.y += static_cast<double>(bar.y) * weight;
        xyz.z += static_cast<double>(bar.z) * weight;
    }
    if (!(normalisation > 0.0)) {
        return {};
    }
    const Vec3d scale = WhiteAdaptation();
    return XyzToLinearSrgb(
        Vec3{static_cast<float>(xyz.x / normalisation * scale.x),
             static_cast<float>(xyz.y / normalisation * scale.y),
             static_cast<float>(xyz.z / normalisation * scale.z)});
}

float TabulatedLuminousScale(const TabulatedSpectrum& spectrum)
{
    static const double reference =
        LuminousIntegral([](float lambda) { return IlluminantD65(lambda); });
    const double tabulated = LuminousIntegral(
        [&spectrum](float lambda) { return EvaluateTabulated(spectrum, lambda); });
    return tabulated > 0.0 ? static_cast<float>(reference / tabulated) : 0.0f;
}

double TabulatedRadiantPerLuminous(const TabulatedSpectrum& spectrum)
{
    const std::vector<float>& l = spectrum.wavelengths;
    const std::vector<float>& v = spectrum.values;
    if (l.size() < 2 || v.size() != l.size()) {
        return 0.0;
    }
    double radiant = 0.0;
    for (std::size_t i = 0; i + 1 < l.size(); ++i) {
        radiant += 0.5 * (static_cast<double>(v[i]) + v[i + 1]) *
                   (static_cast<double>(l[i + 1]) - l[i]);
    }
    // Each interval in steps of at most 1 nm, at their midpoints, so a narrow
    // table is integrated over exactly its own range -- and only where the
    // film integrates, since a luminance is what the film measures.
    double luminous = 0.0;
    for (std::size_t i = 0; i + 1 < l.size(); ++i) {
        const double a = std::max(static_cast<double>(l[i]), static_cast<double>(kLambdaMin));
        const double b = std::min(static_cast<double>(l[i + 1]), static_cast<double>(kLambdaMax));
        if (!(b > a)) {
            continue;
        }
        const int steps = std::max(1, static_cast<int>(std::ceil(b - a)));
        const double h = (b - a) / steps;
        for (int k = 0; k < steps; ++k) {
            const double lambda = a + h * (k + 0.5);
            const double t = (lambda - l[i]) / (static_cast<double>(l[i + 1]) - l[i]);
            const double value = v[i] + (static_cast<double>(v[i + 1]) - v[i]) * t;
            luminous += value * CieXyzBar(static_cast<float>(lambda)).y * h;
        }
    }
    if (!(luminous > 0.0)) {
        return 0.0;
    }
    return radiant / (kMaximumLuminousEfficacy * luminous);
}

// ---------------------------------------------------------------------------
// Illuminants as the draft UsdLux physical-lighting proposal defines them
// ---------------------------------------------------------------------------

namespace {

/// The CIE daylight basis S0, S1, S2 (CIE 015), 300 to 830 nm at 5 nm: the
/// CIE's 10 nm values with the 5 nm entries linearly interpolated, which is
/// what CIE 015 prescribes for 5 nm work. Taken from the colour-science
/// dataset `DATA_BASIS_FUNCTIONS_CIE_ILLUMINANT_D_SERIES`, which cites
/// Wyszecki and Stiles (2000).
constexpr float kDaylightStart = 300.0f;
constexpr float kDaylightStep = 5.0f;
constexpr float kDaylightS0[] = {
    0.04f, 3.02f, 6.0f, 17.8f, 29.6f, 42.45f, 55.3f, 56.3f,
    57.3f, 59.55f, 61.8f, 61.65f, 61.5f, 65.15f, 68.8f, 66.1f,
    63.4f, 64.6f, 65.8f, 80.3f, 94.8f, 99.8f, 104.8f, 105.35f,
    105.9f, 101.35f, 96.8f, 105.35f, 113.9f, 119.75f, 125.6f, 125.55f,
    125.5f, 123.4f, 121.3f, 121.3f, 121.3f, 117.4f, 113.5f, 113.3f,
    113.1f, 111.95f, 110.8f, 108.65f, 106.5f, 107.65f, 108.8f, 107.05f,
    105.3f, 104.85f, 104.4f, 102.2f, 100.0f, 98.0f, 96.0f, 95.55f,
    95.1f, 92.1f, 89.1f, 89.8f, 90.5f, 90.4f, 90.3f, 89.35f,
    88.4f, 86.2f, 84.0f, 84.55f, 85.1f, 83.5f, 81.9f, 82.25f,
    82.6f, 83.75f, 84.9f, 83.1f, 81.3f, 76.6f, 71.9f, 73.1f,
    74.3f, 75.35f, 76.4f, 69.85f, 63.3f, 67.5f, 71.7f, 74.35f,
    77.0f, 71.1f, 65.2f, 56.45f, 47.7f, 58.15f, 68.6f, 66.8f,
    65.0f, 65.5f, 66.0f, 63.5f, 61.0f, 57.15f, 53.3f, 56.1f,
    58.9f, 60.4f, 61.9f,
};
constexpr float kDaylightS1[] = {
    0.02f, 2.26f, 4.5f, 13.45f, 22.4f, 32.2f, 42.0f, 41.3f,
    40.6f, 41.1f, 41.6f, 39.8f, 38.0f, 40.2f, 42.4f, 40.45f,
    38.5f, 36.75f, 35.0f, 39.2f, 43.4f, 44.85f, 46.3f, 45.1f,
    43.9f, 40.5f, 37.1f, 36.9f, 36.7f, 36.3f, 35.9f, 34.25f,
    32.6f, 30.25f, 27.9f, 26.1f, 24.3f, 22.2f, 20.1f, 18.15f,
    16.2f, 14.7f, 13.2f, 10.9f, 8.6f, 7.35f, 6.1f, 5.15f,
    4.2f, 3.05f, 1.9f, 0.95f, 0.0f, -0.8f, -1.6f, -2.55f,
    -3.5f, -3.5f, -3.5f, -4.65f, -5.8f, -6.5f, -7.2f, -7.9f,
    -8.6f, -9.05f, -9.5f, -10.2f, -10.9f, -10.8f, -10.7f, -11.35f,
    -12.0f, -13.0f, -14.0f, -13.8f, -13.6f, -12.8f, -12.0f, -12.65f,
    -13.3f, -13.1f, -12.9f, -11.75f, -10.6f, -11.1f, -11.6f, -11.9f,
    -12.2f, -11.2f, -10.2f, -9.0f, -7.8f, -9.5f, -11.2f, -10.8f,
    -10.4f, -10.5f, -10.6f, -10.15f, -9.7f, -9.0f, -8.3f, -8.8f,
    -9.3f, -9.55f, -9.8f,
};
constexpr float kDaylightS2[] = {
    0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 6.25f, 8.5f, 8.15f,
    7.8f, 7.25f, 6.7f, 6.0f, 5.3f, 5.7f, 6.1f, 4.55f,
    3.0f, 2.1f, 1.2f, 0.05f, -1.1f, -0.8f, -0.5f, -0.6f,
    -0.7f, -0.95f, -1.2f, -1.9f, -2.6f, -2.75f, -2.9f, -2.85f,
    -2.8f, -2.7f, -2.6f, -2.6f, -2.6f, -2.2f, -1.8f, -1.65f,
    -1.5f, -1.4f, -1.3f, -1.25f, -1.2f, -1.1f, -1.0f, -0.75f,
    -0.5f, -0.4f, -0.3f, -0.15f, 0.0f, 0.1f, 0.2f, 0.35f,
    0.5f, 1.3f, 2.1f, 2.65f, 3.2f, 3.65f, 4.1f, 4.4f,
    4.7f, 4.9f, 5.1f, 5.9f, 6.7f, 7.0f, 7.3f, 7.95f,
    8.6f, 9.2f, 9.8f, 10.0f, 10.2f, 9.25f, 8.3f, 8.95f,
    9.6f, 9.05f, 8.5f, 7.75f, 7.0f, 7.3f, 7.6f, 7.8f,
    8.0f, 7.35f, 6.7f, 5.95f, 5.2f, 6.3f, 7.4f, 7.1f,
    6.8f, 6.9f, 7.0f, 6.7f, 6.4f, 5.95f, 5.5f, 5.8f,
    6.1f, 6.3f, 6.5f,
};
constexpr int kDaylightCount = static_cast<int>(sizeof(kDaylightS0) / sizeof(float));
static_assert(sizeof(kDaylightS1) == sizeof(kDaylightS0) &&
                  sizeof(kDaylightS2) == sizeof(kDaylightS0),
              "the daylight basis functions share one grid");

float DaylightBasis(const float* table, float lambda)
{
    const float position = (lambda - kDaylightStart) / kDaylightStep;
    if (position < 0.0f || position > static_cast<float>(kDaylightCount - 1)) {
        return 0.0f;
    }
    const int index = std::min(static_cast<int>(position), kDaylightCount - 2);
    const float t = position - static_cast<float>(index);
    return table[index] * (1.0f - t) + table[index + 1] * t;
}

TabulatedSpectrum Tabulate(float (*sample)(float, float), float parameter)
{
    TabulatedSpectrum spectrum;
    for (float lambda = kIlluminantFirst; lambda <= kIlluminantLast + 0.5f;
         lambda += kIlluminantStep) {
        spectrum.wavelengths.push_back(lambda);
        spectrum.values.push_back(sample(lambda, parameter));
    }
    return spectrum;
}

}  // namespace

TabulatedSpectrum WhiteIlluminant()
{
    return Tabulate([](float lambda, float) { return IlluminantD65(lambda); }, 0.0f);
}

TabulatedSpectrum BlackbodyIlluminant(float kelvin)
{
    if (!(kelvin > 0.0f)) {
        return {};
    }
    return Tabulate([](float lambda, float k) { return NormalizedBlackbody(lambda, k); },
                    kelvin);
}

bool IlluminantDDefined(float kelvin)
{
    return kelvin >= 4000.0f && kelvin <= 25000.0f;
}

float IlluminantD(float lambda, float kelvin)
{
    if (!IlluminantDDefined(kelvin)) {
        return 0.0f;
    }
    // CIE 015: the daylight locus, in two pieces, then the weights of the
    // basis. Unrounded M1 and M2.
    const double t = static_cast<double>(kelvin);
    const double x =
        kelvin <= 7000.0f
            ? -4.6070e9 / (t * t * t) + 2.9678e6 / (t * t) + 0.09911e3 / t + 0.244063
            : -2.0064e9 / (t * t * t) + 1.9018e6 / (t * t) + 0.24748e3 / t + 0.237040;
    const double y = -3.000 * x * x + 2.870 * x - 0.275;
    const double m = 0.0241 + 0.2562 * x - 0.7341 * y;
    const double m1 = (-1.3515 - 1.7703 * x + 5.9114 * y) / m;
    const double m2 = (0.0300 - 31.4424 * x + 30.0717 * y) / m;
    return static_cast<float>(DaylightBasis(kDaylightS0, lambda) +
                              m1 * DaylightBasis(kDaylightS1, lambda) +
                              m2 * DaylightBasis(kDaylightS2, lambda));
}

TabulatedSpectrum DaylightIlluminant(float kelvin)
{
    if (!IlluminantDDefined(kelvin)) {
        return {};
    }
    return Tabulate([](float lambda, float k) { return IlluminantD(lambda, k); }, kelvin);
}

TabulatedSpectrum ResampleTo5nm(const TabulatedSpectrum& spectrum, int* negatives)
{
    if (negatives) {
        *negatives = 0;
    }
    const std::vector<float>& x = spectrum.wavelengths;
    const std::vector<float>& y = spectrum.values;
    const std::size_t n = x.size();
    if (n < 2 || n != y.size()) {
        return {};
    }

    // The Lagrange polynomial through samples [first, first + count).
    const auto lagrange = [&](std::size_t first, std::size_t count, double at) {
        double total = 0.0;
        for (std::size_t i = first; i < first + count; ++i) {
            double term = static_cast<double>(y[i]);
            for (std::size_t j = first; j < first + count; ++j) {
                if (j != i) {
                    term *= (at - static_cast<double>(x[j])) /
                            (static_cast<double>(x[i]) - static_cast<double>(x[j]));
                }
            }
            total += term;
        }
        return total;
    };

    const auto evaluate = [&](double at) {
        // The interval [x[k], x[k + 1]] holding `at`.
        std::size_t k = static_cast<std::size_t>(
            std::upper_bound(x.begin(), x.end(), static_cast<float>(at)) - x.begin());
        k = k == 0 ? 0 : std::min(k - 1, n - 2);
        if (static_cast<double>(x[k]) == at) {
            return static_cast<double>(y[k]);   // a sample is its own value
        }
        if (n == 2) {
            return lagrange(0, 2, at);
        }
        if (n == 3) {
            return lagrange(0, 3, at);
        }
        if (k == 0) {
            return lagrange(0, 3, at);          // first interval: quadratic
        }
        if (k == n - 2) {
            return lagrange(n - 3, 3, at);      // last interval: quadratic
        }
        return lagrange(k - 1, 4, at);          // interior: cubic
    };

    TabulatedSpectrum result;
    const auto push = [&](float lambda, double value) {
        if (value < 0.0) {
            if (negatives) {
                ++*negatives;
            }
            value = 0.0;
        }
        result.wavelengths.push_back(lambda);
        result.values.push_back(static_cast<float>(value));
    };

    push(x.front(), static_cast<double>(y.front()));
    const float firstGrid = std::floor(x.front() / 5.0f) * 5.0f + 5.0f;
    for (float lambda = firstGrid; lambda < x.back(); lambda += 5.0f) {
        if (lambda > x.front()) {
            push(lambda, evaluate(static_cast<double>(lambda)));
        }
    }
    push(x.back(), static_cast<double>(y.back()));
    return result;
}

// ---------------------------------------------------------------------------
// The chromaticity table
// ---------------------------------------------------------------------------

namespace {

/// The chromaticity a table cell stands for.
///
/// Face `f` is the channel pinned to one; the two ratios fill the others in
/// their natural order, so the cell at (1, 1) on every face is white and the
/// faces agree there.
Vec3 CellChroma(int face, float a, float b)
{
    switch (face) {
        case 0:  return Vec3{1.0f, a, b};
        case 1:  return Vec3{a, 1.0f, b};
        default: return Vec3{a, b, 1.0f};
    }
}

}  // namespace

ChromaTable BuildChromaTable()
{
    ChromaTable table;
    table.size = kChromaTableSize;
    table.coefficients.assign(static_cast<std::size_t>(kChromaTableFaces) *
                                  table.size * table.size * 3,
                              0.0f);

    for (int face = 0; face < kChromaTableFaces; ++face) {
        SigmoidCoefficients seed;
        bool haveSeed = false;
        for (int j = 0; j < table.size; ++j) {
            // Restart each row from the row above rather than from the end of
            // the previous one, which is a discontinuity in the parameter.
            SigmoidCoefficients rowSeed = seed;
            bool haveRowSeed = haveSeed;
            for (int i = 0; i < table.size; ++i) {
                const float a =
                    static_cast<float>(i) / static_cast<float>(table.size - 1);
                const float b =
                    static_cast<float>(j) / static_cast<float>(table.size - 1);
                const SigmoidCoefficients fitted =
                    FitChroma(CellChroma(face, a, b), rowSeed, haveRowSeed);

                const std::size_t index =
                    ((static_cast<std::size_t>(face) * table.size + j) *
                         table.size +
                     i) *
                    3;
                table.coefficients[index + 0] = fitted.c0;
                table.coefficients[index + 1] = fitted.c1;
                table.coefficients[index + 2] = fitted.c2;

                rowSeed = fitted;
                haveRowSeed = true;
                if (i == 0) {
                    seed = fitted;
                    haveSeed = true;
                }
            }
        }
    }
    return table;
}

SigmoidCoefficients LookUpChroma(const ChromaTable& table, const Vec3& linearSrgb)
{
    if (!table.Valid()) {
        return {};
    }
    const Decomposed decomposed = Decompose(linearSrgb);
    if (!(decomposed.scale > 0.0f)) {
        return {0.0f, 0.0f, -1.0e4f};
    }

    float a = 0.0f;
    float b = 0.0f;
    switch (decomposed.face) {
        case 0:  a = decomposed.chroma.y; b = decomposed.chroma.z; break;
        case 1:  a = decomposed.chroma.x; b = decomposed.chroma.z; break;
        default: a = decomposed.chroma.x; b = decomposed.chroma.y; break;
    }

    const float last = static_cast<float>(table.size - 1);
    const float fi = std::clamp(a, 0.0f, 1.0f) * last;
    const float fj = std::clamp(b, 0.0f, 1.0f) * last;
    const int i0 = std::min(static_cast<int>(fi), table.size - 1);
    const int j0 = std::min(static_cast<int>(fj), table.size - 1);
    const int i1 = std::min(i0 + 1, table.size - 1);
    const int j1 = std::min(j0 + 1, table.size - 1);
    const float ti = fi - static_cast<float>(i0);
    const float tj = fj - static_cast<float>(j0);

    const auto at = [&](int i, int j, int component) {
        const std::size_t index =
            ((static_cast<std::size_t>(decomposed.face) * table.size + j) *
                 table.size +
             i) *
            3;
        return table.coefficients[index + static_cast<std::size_t>(component)];
    };

    SigmoidCoefficients out;
    float* destination = &out.c0;
    for (int component = 0; component < 3; ++component) {
        const float top = at(i0, j0, component) * (1.0f - ti) +
                          at(i1, j0, component) * ti;
        const float bottom = at(i0, j1, component) * (1.0f - ti) +
                             at(i1, j1, component) * ti;
        destination[component] = top * (1.0f - tj) + bottom * tj;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Colour difference
// ---------------------------------------------------------------------------

float DeltaE2000(const Vec3& lab1, const Vec3& lab2)
{
    constexpr float kPi = 3.14159265358979323846f;
    const auto radians = [](float degrees) { return degrees * kPi / 180.0f; };
    const auto degrees = [](float r) { return r * 180.0f / kPi; };

    const float c1 = std::sqrt(lab1.y * lab1.y + lab1.z * lab1.z);
    const float c2 = std::sqrt(lab2.y * lab2.y + lab2.z * lab2.z);
    const float meanC = 0.5f * (c1 + c2);
    const float meanC7 = std::pow(meanC, 7.0f);
    const float g = 0.5f * (1.0f - std::sqrt(meanC7 / (meanC7 + std::pow(25.0f, 7.0f))));

    const float a1 = (1.0f + g) * lab1.y;
    const float a2 = (1.0f + g) * lab2.y;
    const float cp1 = std::sqrt(a1 * a1 + lab1.z * lab1.z);
    const float cp2 = std::sqrt(a2 * a2 + lab2.z * lab2.z);

    const auto hue = [&](float a, float b) {
        if (a == 0.0f && b == 0.0f) return 0.0f;
        float h = degrees(std::atan2(b, a));
        return h < 0.0f ? h + 360.0f : h;
    };
    const float h1 = hue(a1, lab1.z);
    const float h2 = hue(a2, lab2.z);

    const float deltaL = lab2.x - lab1.x;
    const float deltaC = cp2 - cp1;

    float deltah = 0.0f;
    if (cp1 * cp2 != 0.0f) {
        deltah = h2 - h1;
        if (deltah > 180.0f) deltah -= 360.0f;
        else if (deltah < -180.0f) deltah += 360.0f;
    }
    const float deltaH = 2.0f * std::sqrt(cp1 * cp2) * std::sin(radians(deltah * 0.5f));

    const float meanL = 0.5f * (lab1.x + lab2.x);
    const float meanCp = 0.5f * (cp1 + cp2);

    float meanH = h1 + h2;
    if (cp1 * cp2 != 0.0f) {
        if (std::fabs(h1 - h2) > 180.0f) {
            meanH += (h1 + h2 < 360.0f) ? 360.0f : -360.0f;
        }
        meanH *= 0.5f;
    }

    const float t = 1.0f - 0.17f * std::cos(radians(meanH - 30.0f)) +
                    0.24f * std::cos(radians(2.0f * meanH)) +
                    0.32f * std::cos(radians(3.0f * meanH + 6.0f)) -
                    0.20f * std::cos(radians(4.0f * meanH - 63.0f));

    const float deltaTheta =
        30.0f * std::exp(-((meanH - 275.0f) / 25.0f) * ((meanH - 275.0f) / 25.0f));
    const float meanCp7 = std::pow(meanCp, 7.0f);
    const float rc = 2.0f * std::sqrt(meanCp7 / (meanCp7 + std::pow(25.0f, 7.0f)));
    const float sl = 1.0f + (0.015f * (meanL - 50.0f) * (meanL - 50.0f)) /
                                std::sqrt(20.0f + (meanL - 50.0f) * (meanL - 50.0f));
    const float sc = 1.0f + 0.045f * meanCp;
    const float sh = 1.0f + 0.015f * meanCp * t;
    const float rt = -std::sin(radians(2.0f * deltaTheta)) * rc;

    const float termL = deltaL / sl;
    const float termC = deltaC / sc;
    const float termH = deltaH / sh;
    return std::sqrt(termL * termL + termC * termC + termH * termH +
                     rt * termC * termH);
}

}  // namespace hdclaude
