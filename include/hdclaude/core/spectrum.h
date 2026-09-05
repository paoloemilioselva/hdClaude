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

/// CIE illuminant E (equal energy). The reference illuminant under which
/// reflectance upsampling is defined; see docs/spectral-rendering.md 2.
inline float IlluminantE(float /*lambda*/) { return 1.0f; }

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
