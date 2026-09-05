#include "hdclaude/core/spectrum.h"

#include <algorithm>
#include <cmath>

namespace hdclaude {
namespace {

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
    sample.pdf[0] = VisibleWavelengthPdf(sample.lambda[0]);

    constexpr float kRange = kLambdaMax - kLambdaMin;
    constexpr float kStride = kRange / static_cast<float>(kSpectralLanes);

    for (int i = 1; i < kSpectralLanes; ++i) {
        float lambda = sample.lambda[0] + static_cast<float>(i) * kStride;
        if (lambda > kLambdaMax) {
            lambda -= kRange;
        }
        sample.lambda[static_cast<std::size_t>(i)] = lambda;
        sample.pdf[static_cast<std::size_t>(i)] = VisibleWavelengthPdf(lambda);
    }
    return sample;
}

}  // namespace hdclaude
