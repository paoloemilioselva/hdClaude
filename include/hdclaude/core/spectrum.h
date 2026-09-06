// Spectral primitives: wavelengths, colour matching, illuminants, and the
// hero-wavelength sampling used by the integrator.
//
// This header has no GPU and no OpenUSD dependency by design: the sensor and
// the colour pipeline are testable on any host, and the GPU shaders are
// written against the same definitions so a discrepancy is a test failure
// rather than a rendering artefact.
//
// See docs/spectral-rendering.md for the model and its justification.

#ifndef HDCLAUDE_CORE_SPECTRUM_H
#define HDCLAUDE_CORE_SPECTRUM_H

#include <array>
#include <cstdint>

namespace hdclaude {

/// Number of correlated hero wavelengths carried per path.
///
/// Four, not three: a vec4 is the natural GPU register and memory width, so
/// three lanes cost the same registers as four and waste one. See
/// docs/spectral-rendering.md 1.
inline constexpr int kSpectralLanes = 4;

/// Visible range used for sampling and integration, in nanometres.
inline constexpr float kLambdaMin = 360.0f;
inline constexpr float kLambdaMax = 830.0f;

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    friend Vec3 operator+(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
    friend Vec3 operator-(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    friend Vec3 operator*(const Vec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
    friend Vec3 operator*(float s, const Vec3& a) { return a * s; }
    Vec3& operator+=(const Vec3& b) { x += b.x; y += b.y; z += b.z; return *this; }
};

/// A hero-wavelength packet: the wavelengths and the density each was drawn
/// with. The densities are kept because they differ between lanes and are
/// needed for both the estimator and wavelength MIS.
struct WavelengthSample {
    std::array<float, kSpectralLanes> lambda{};
    std::array<float, kSpectralLanes> pdf{};
};

// ---------------------------------------------------------------------------
// Colour matching
// ---------------------------------------------------------------------------

/// CIE 1931 2-degree colour matching functions at a single wavelength.
///
/// Evaluated with the multi-lobe piecewise-Gaussian fit of Wyman, Sloan and
/// Shirley (2013) rather than an interpolated table. The fit's error is under
/// about 1% of peak, it is branch-light, and -- the reason that matters here --
/// the identical closed form is used by the GPU film kernel, so the CPU sensor
/// and the GPU sensor cannot drift apart.
Vec3 CieXyzBar(float lambda);

/// Integral of ybar over the visible range. The normalisation constant that
/// turns a spectral integral into photometrically consistent XYZ.
float CieYIntegral();

// ---------------------------------------------------------------------------
// Colour spaces
// ---------------------------------------------------------------------------

/// CIE XYZ (D65-adapted) to linear sRGB.
Vec3 XyzToLinearSrgb(const Vec3& xyz);

/// Linear sRGB to CIE XYZ (D65-adapted).
Vec3 LinearSrgbToXyz(const Vec3& rgb);

/// sRGB electro-optical transfer function and its inverse. Used only at the
/// asset-input and display-output boundaries, never in transport.
float SrgbToLinear(float encoded);
float LinearToSrgb(float linear);

// ---------------------------------------------------------------------------
// Illuminants and emitters
// ---------------------------------------------------------------------------

/// Planck's law: spectral radiance of a blackbody at `kelvin`, in W/(m^2 sr m).
///
/// This is the exact physical form, not an RGB approximation of a colour
/// temperature. UsdLux `enableColorTemperature` resolves to this, which is the
/// most visible single difference between hdClaude and an RGB renderer.
float BlackbodyRadiance(float lambda, float kelvin);

/// Blackbody radiance normalised to a peak of one, for use where a light's
/// authored intensity supplies the magnitude.
float NormalizedBlackbody(float lambda, float kelvin);

/// CIE illuminant E (equal energy).
inline float IlluminantE(float /*lambda*/) { return 1.0f; }

/// CIE standard illuminant D65, relative spectral power, normalised to 100 at
/// 560 nm.
///
/// The illuminant reflectance upsampling is defined against, because sRGB's
/// white point is D65: a reflectance fitted under one illuminant and integrated
/// under another does not return the colour it was fitted to, and the phase 1
/// exit gate is a round trip. Linearly interpolated from the CIE table at 5 nm,
/// clamped to its ends.
float IlluminantD65(float lambda);

/// The XYZ of a perfect white reflector under D65, normalised so Y = 1.
///
/// The white point every Lab conversion here is relative to, and the
/// normalisation that makes an upsampled reflectance of 1 integrate to white
/// rather than to whatever the illuminant's absolute power happens to be.
Vec3 D65WhitePoint();

// ---------------------------------------------------------------------------
// RGB to spectrum
// ---------------------------------------------------------------------------

/// Jakob-Hanika sigmoid coefficients for one reflectance spectrum.
///
/// `S(lambda) = sigmoid(c0 * t^2 + c1 * t + c2)` with `t` the wavelength
/// normalised to [0, 1] over the visible range. The sigmoid's range is (0, 1)
/// for any coefficients at all, so an upsampled reflectance cannot exceed unity
/// at any wavelength and no upsampled albedo can create energy -- which is the
/// property that makes this model, rather than a basis expansion, the right one
/// for reflectance (docs/spectral-rendering.md 2).
struct SigmoidCoefficients {
    float c0 = 0.0f;
    float c1 = 0.0f;
    float c2 = 0.0f;
};

/// The smooth, bounded sigmoid the model is built on.
float ReflectanceSigmoid(float x);

/// Evaluate an upsampled reflectance at one wavelength. Always in (0, 1).
float EvaluateReflectance(const SigmoidCoefficients& coefficients, float lambda);

/// Integrate an upsampled reflectance under D65 back to linear sRGB.
///
/// The inverse of `FitReflectance`, and the half of the round trip that says
/// whether the fit converged.
Vec3 IntegrateReflectance(const SigmoidCoefficients& coefficients);

/// Fit a reflectance spectrum whose D65 integral is `linearSrgb`.
///
/// Levenberg-Marquardt on the three coefficients, with the residual measured in
/// CIELab rather than in XYZ: the acceptance criterion is a colour difference,
/// and a least-squares fit in XYZ spends its accuracy where the eye does not
/// look. Colours outside [0, 1] are clamped -- a reflectance above one is not a
/// reflectance -- and the caller is expected to have divided out any magnitude
/// first (see `FitEmission`).
SigmoidCoefficients FitReflectance(const Vec3& linearSrgb);

/// An emission spectrum for an authored light colour.
///
/// Emission cannot use a bounded model directly: a light of RGB (5, 5, 5) must
/// integrate to five times white, which nothing in [0, 1] expresses. So the
/// chromaticity is upsampled as a reflectance and the magnitude is carried
/// alongside, which preserves the authored chromaticity exactly and integrates
/// to the authored luminance while staying non-negative everywhere.
struct EmissionSpectrum {
    SigmoidCoefficients chromaticity;
    /// Multiplies the bounded spectrum. One for a colour already in [0, 1].
    float scale = 1.0f;
};

EmissionSpectrum FitEmission(const Vec3& linearSrgb);

/// Evaluate an emission spectrum at one wavelength. Non-negative, unbounded.
float EvaluateEmission(const EmissionSpectrum& emission, float lambda);

// ---------------------------------------------------------------------------
// Colour difference
// ---------------------------------------------------------------------------

/// CIE L*a*b* of an XYZ colour, relative to the D65 white point.
Vec3 XyzToLab(const Vec3& xyz);

/// CIEDE2000 colour difference between two Lab colours.
///
/// The metric the upsampling round trip is judged by, because "within 1e-3" is
/// only meaningful in a space where a unit means something perceptual.
float DeltaE2000(const Vec3& lab1, const Vec3& lab2);

// ---------------------------------------------------------------------------
// Hero wavelength sampling
// ---------------------------------------------------------------------------

/// Sample one wavelength from the visual-response-shaped density of Radziszewski
/// et al., as used by Wilkie et al.'s hero wavelength scheme.
float SampleVisibleWavelength(float u);

/// Density of `SampleVisibleWavelength` at `lambda`.
///
/// Invariant, and a unit test: this must equal the derivative of the sampling
/// routine's inverse CDF everywhere in the visible range. Any inconsistency
/// between the two is a bias in every pixel.
float VisibleWavelengthPdf(float lambda);

/// Draw a correlated hero packet from a single uniform sample.
///
/// The first wavelength is importance sampled; the remaining lanes are rotated
/// by equal fractions of the visible range and wrapped, which stratifies the
/// spectrum from one random number.
WavelengthSample SampleHeroWavelengths(float u);

}  // namespace hdclaude

#endif  // HDCLAUDE_CORE_SPECTRUM_H
