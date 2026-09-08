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
#include <vector>

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

/// A hero-wavelength packet: the wavelengths, and the density to divide each
/// lane's contribution by.
///
/// The densities are equal, and that is the point rather than an oversight. A
/// rotated lane is `lambda_0` shifted and wrapped -- a bijection of the visible
/// range onto itself with unit Jacobian -- so the density of that variable at
/// the value it took is p(lambda_0), whatever the value is. The array is kept
/// per lane because wavelength MIS, when dispersion lands, will need them to
/// differ.
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

/// What to multiply `NormalizedBlackbody` by so it carries the same luminous
/// power as D65.
///
/// `enableColorTemperature` tints a light; it does not brighten it. That is a
/// statement about *luminance*, so the two illuminants have to be equated on
/// their luminous integrals rather than on their peaks -- a 2700 K blackbody
/// peaks far outside the visible range, and matching peaks would make a warm
/// light a dim one.
float BlackbodyLuminousScale(float kelvin);

/// CIE xy chromaticity of a spectrum's XYZ.
Vec3 XyzToChromaticity(const Vec3& xyz);

/// XYZ of a blackbody at `kelvin`, normalised so Y = 1.
///
/// Exposed for testing: the chromaticity it implies can be checked against the
/// published Planckian locus, which is what says the spectral form and the
/// colour matching integration are both right rather than wrong together.
Vec3 BlackbodyXyz(float kelvin);

/// Index of refraction at `lambda`, from a nominal index and an Abbe number.
///
/// The Abbe number is how optical glass is actually catalogued: `V = (nd - 1) /
/// (nF - nC)`, the ratio of refractivity at the yellow d-line to the spread
/// between the blue F-line and the red C-line. A low V means a glass that
/// spreads light strongly, so a *smaller* Abbe number is more dispersive, which
/// is the opposite of what the name suggests to anyone meeting it for the first
/// time.
///
/// Two-term Cauchy, `n = A + B / lambda^2`, whose two coefficients are exactly
/// determined by the two numbers a material authors. That is why this form and
/// not a Sellmeier fit: Sellmeier is more accurate over a wider band and needs
/// six coefficients no asset supplies.
///
/// `abbe` at or below zero means no dispersion, and the nominal index is
/// returned unchanged.
float DispersedIor(float ior, float abbe, float lambda);

/// Semi-infinite diffuse reflectance of a scattering medium, from van de Hulst.
///
/// The closed form OpenPBR's subsurface parameterisation is written against: a
/// medium of single-scattering albedo `albedo` and Henyey-Greenstein anisotropy
/// `g`, filling a half-space with an index-matched boundary, reflects
///
///     C = (1 - s)(1 - 0.139 s) / (1 + 1.17 s),   s = sqrt((1 - a) / (1 - a g))
///
/// of the light that enters it. It is the *observed* colour of a subsurface
/// material, which is what both `subsurface_bsdf`'s `color` and OpenPBR's
/// `subsurface_color` are documented to be, and it does not depend on the mean
/// free path at all -- only on how much of each collision survives.
///
/// Exposed so the inversion below can be checked against it rather than against
/// a table of remembered numbers.
float VanDeHulstDiffuseAlbedo(float albedo, float g);

/// The inverse: the single-scattering albedo a random walk must be given so that
/// it reflects `reflectance` out of a semi-infinite half-space.
///
/// OpenPBR states this inversion in closed form, and it is quoted here as
/// written:
///
///     a = (1 - s^2) / (1 - g s^2)
///     s = 4.09712 + 4.20863 C - sqrt(9.59217 + 41.6808 C + 17.7126 C^2)
///
/// This is why an authored subsurface colour is not handed to the walk as its
/// scattering albedo directly. A walk whose collisions each survive with
/// probability C loses light at every one of them, so a medium given C = 0.5
/// returns far less than half of what enters it; the inversion is what makes
/// the material render the colour it was authored with. The relation assumes an
/// index-matched boundary, which is the boundary `subsurface_bsdf` describes --
/// the node has no index of refraction to describe any other.
float SubsurfaceSingleScatteringAlbedo(float reflectance, float g);

/// The three Fraunhofer lines the Abbe number is defined against, in nanometres.
constexpr float kFraunhoferF = 486.13f;   // blue
constexpr float kFraunhoferD = 587.56f;   // yellow, where `ior` is quoted
constexpr float kFraunhoferC = 656.27f;   // red

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

/// An upsampled spectrum: a bounded shape and the magnitude it is scaled by.
///
/// One type for reflectance and for emission, because splitting a colour into
/// chromaticity and magnitude is what both of them need and doing it twice is
/// how the two drift apart.
///
/// Emission cannot use a bounded model on its own -- a light of RGB (5, 5, 5)
/// must integrate to five times white, which nothing in [0, 1] expresses -- so
/// the magnitude has to be carried separately whatever else is true. Once it
/// is, a reflectance gets the same decomposition for free, and gains two
/// properties from it: the round trip becomes exact by construction rather than
/// by convergence, since the scale cancels; and a grey upsamples to a *flat*
/// spectrum rather than to whatever a three-parameter fit converged on, which
/// is what a grey physically is.
///
/// The trade is that every colour's spectrum is a scaled saturated one, where a
/// direct three-parameter fit would find a flatter spectrum for a desaturated
/// colour. Both are metameric under D65 and both are smooth and bounded; they
/// differ only under a narrowband illuminant, which nothing here yet is.
struct SpectrumFit {
    /// The shape, whose D65 integral is the colour divided by `scale`. Bounded
    /// in (0, 1) at every wavelength by the sigmoid, so a reflectance built
    /// from it cannot create energy.
    SigmoidCoefficients chroma;
    /// The largest component of the authored colour. At most one for a
    /// reflectance; unbounded for emission.
    float scale = 1.0f;
};

/// Fit the spectrum whose D65 integral is `linearSrgb`.
///
/// The magnitude is divided out first, then Levenberg-Marquardt fits the three
/// coefficients of the remaining chromaticity, with the residual measured in
/// CIELab rather than in XYZ: the acceptance criterion is a colour difference,
/// and a least-squares fit in XYZ spends its accuracy where the eye does not
/// look.
SpectrumFit FitSpectrum(const Vec3& linearSrgb);

/// Evaluate an upsampled spectrum at one wavelength. Never negative.
float EvaluateSpectrum(const SpectrumFit& fit, float lambda);

/// Integrate an upsampled spectrum under D65 back to linear sRGB.
///
/// The other half of the round trip, and the one that says whether the fit
/// converged.
Vec3 IntegrateSpectrum(const SpectrumFit& fit);

// ---------------------------------------------------------------------------
// The chromaticity table
// ---------------------------------------------------------------------------

/// Resolution of one axis of the chromaticity table.
inline constexpr int kChromaTableSize = 32;

/// Faces of the table: one per channel that can be the largest.
inline constexpr int kChromaTableFaces = 3;

/// Sigmoid coefficients for every chromaticity, ready for the GPU.
///
/// A shading point's colour is not known until it is shaded -- it comes out of
/// a texture and a whole MaterialX graph -- so the fit cannot run per hit; it
/// is an iterative optimisation. Tabulating it is the documented answer
/// (docs/spectral-rendering.md 2), and the decomposition above is what makes
/// the table small: only the *chromaticity* needs tabulating, and a
/// chromaticity has one component pinned to one, so two free axes remain.
///
/// Indexed by which channel is largest and by the other two divided by it, so
/// every entry is a colour on the surface of the unit cube. `size * size`
/// entries per face, each three coefficients, row-major in the second ratio.
struct ChromaTable {
    int size = kChromaTableSize;
    /// `faces * size * size * 3` floats.
    std::vector<float> coefficients;

    bool Valid() const
    {
        return size > 1 &&
               coefficients.size() == static_cast<std::size_t>(kChromaTableFaces) *
                                          size * size * 3;
    }
};

/// Build the table.
///
/// Each fit is seeded from the one before it in scan order, which is what keeps
/// this to a fraction of a second: neighbouring chromaticities have
/// neighbouring coefficients, so a seeded fit converges in a handful of
/// iterations where a cold one takes dozens.
ChromaTable BuildChromaTable();

/// Look a chromaticity up in the table, bilinearly, exactly as the GPU does.
///
/// Exposed so the two can be tested against each other: a table the shader
/// reads differently from the host is a table that describes nothing.
SigmoidCoefficients LookUpChroma(const ChromaTable& table, const Vec3& linearSrgb);

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
