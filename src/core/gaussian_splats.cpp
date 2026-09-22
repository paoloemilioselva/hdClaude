#include "hdclaude/core/gaussian_splats.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace hdclaude {

namespace {

constexpr double kPi = 3.14159265358979323846;

/// The normalisation of a real spherical harmonic:
/// sqrt((2l+1)/(4 pi) * (l-m)!/(l+m)!).
///
/// Computed as a running quotient rather than from two factorials, because
/// (l+m)! overflows a double at degree 86 while this does not overflow at any
/// degree an asset could carry.
double HarmonicNormalisation(int l, int m)
{
    double value = (2.0 * l + 1.0) / (4.0 * kPi);
    for (int k = l - m + 1; k <= l + m; ++k) {
        value /= static_cast<double>(k);
    }
    return std::sqrt(value);
}

/// Multiply a 3x3 row-major matrix by a vector.
void Apply3x3(const float m[9], const double v[3], double out[3])
{
    for (int row = 0; row < 3; ++row) {
        out[row] = static_cast<double>(m[row * 3 + 0]) * v[0] +
                   static_cast<double>(m[row * 3 + 1]) * v[1] +
                   static_cast<double>(m[row * 3 + 2]) * v[2];
    }
}

/// Invert a 3x3 row-major matrix. Returns false when it is singular, which for
/// a splat means a scale of zero on some axis and is reported by the caller
/// rather than worked around.
bool Invert3x3(const float m[9], double out[9])
{
    const double a = m[0], b = m[1], c = m[2];
    const double d = m[3], e = m[4], f = m[5];
    const double g = m[6], h = m[7], i = m[8];

    const double determinant =
        a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (!(std::fabs(determinant) > 0.0) || !std::isfinite(determinant)) {
        return false;
    }
    const double inverse = 1.0 / determinant;
    out[0] = (e * i - f * h) * inverse;
    out[1] = (c * h - b * i) * inverse;
    out[2] = (b * f - c * e) * inverse;
    out[3] = (f * g - d * i) * inverse;
    out[4] = (a * i - c * g) * inverse;
    out[5] = (c * d - a * f) * inverse;
    out[6] = (d * h - e * g) * inverse;
    out[7] = (b * g - a * h) * inverse;
    out[8] = (a * e - b * d) * inverse;
    return true;
}

}  // namespace

float SplatSupportRadius(SplatKernel kernel)
{
    switch (kernel) {
        case SplatKernel::ConstantSurflet:
            return 1.0f;
        case SplatKernel::GaussianEllipsoid:
        case SplatKernel::GaussianSurflet:
            break;
    }
    return 3.0f;
}

bool SplatKernelIsFlat(SplatKernel kernel)
{
    return kernel == SplatKernel::GaussianSurflet ||
           kernel == SplatKernel::ConstantSurflet;
}

std::size_t SphericalHarmonicsCoefficientCount(int degree)
{
    if (degree < 0) {
        return 0;
    }
    const std::size_t order = static_cast<std::size_t>(degree) + 1;
    return order * order;
}

std::size_t SplatCloud::CoefficientsPerParticle() const
{
    return SphericalHarmonicsCoefficientCount(sphericalHarmonicsDegree);
}

float SphericalHarmonicsFallbackCoefficient()
{
    // 0.5 / Y(0,0), with Y(0,0) = 1/(2 sqrt(pi)).
    return static_cast<float>(0.5 / HarmonicNormalisation(0, 0));
}

void EvaluateSphericalHarmonics(const float* coefficients, int degree,
                                const float direction[3], float rgb[3])
{
    rgb[0] = 0.0f;
    rgb[1] = 0.0f;
    rgb[2] = 0.0f;
    if (coefficients == nullptr || degree < 0) {
        return;
    }

    double x = direction[0];
    double y = direction[1];
    double z = direction[2];
    const double length = std::sqrt(x * x + y * y + z * z);
    if (!(length > 0.0)) {
        // No direction at all. The DC term is the only one that does not depend
        // on one, and returning it is the honest answer rather than zero.
        const double basis = HarmonicNormalisation(0, 0);
        rgb[0] = static_cast<float>(basis * coefficients[0]);
        rgb[1] = static_cast<float>(basis * coefficients[1]);
        rgb[2] = static_cast<float>(basis * coefficients[2]);
        return;
    }
    x /= length;
    y /= length;
    z /= length;

    // sin(theta), and the azimuth's cosine and sine. At a pole the azimuth is
    // undefined and every term that would use it is multiplied by a power of
    // sin(theta), so any value does.
    const double sinTheta = std::sqrt(std::max(0.0, 1.0 - z * z));
    const double cosPhi = sinTheta > 0.0 ? x / sinTheta : 1.0;
    const double sinPhi = sinTheta > 0.0 ? y / sinTheta : 0.0;

    double accumulated[3] = {0.0, 0.0, 0.0};
    const auto accumulate = [&](int index, double basis) {
        const float* value = coefficients + 3 * static_cast<std::size_t>(index);
        accumulated[0] += basis * value[0];
        accumulated[1] += basis * value[1];
        accumulated[2] += basis * value[2];
    };

    // cos(m phi) and sin(m phi) by the angle-addition recurrence, so no
    // trigonometry runs per band.
    double cosMPhi = 1.0;
    double sinMPhi = 0.0;

    for (int m = 0; m <= degree; ++m) {
        if (m > 0) {
            const double nextCos = cosMPhi * cosPhi - sinMPhi * sinPhi;
            const double nextSin = sinMPhi * cosPhi + cosMPhi * sinPhi;
            cosMPhi = nextCos;
            sinMPhi = nextSin;
        }

        // P(m,m) = (-1)^m (2m-1)!! sin(theta)^m, the Condon-Shortley phase
        // included, which is what makes this agree with the published
        // closed forms for the first four bands.
        double pmm = 1.0;
        for (int i = 1; i <= m; ++i) {
            pmm *= -(2.0 * i - 1.0) * sinTheta;
        }

        double previous = 0.0;
        double beforePrevious = 0.0;
        for (int l = m; l <= degree; ++l) {
            double legendre;
            if (l == m) {
                legendre = pmm;
            } else if (l == m + 1) {
                legendre = z * (2.0 * m + 1.0) * previous;
            } else {
                legendre = (z * (2.0 * l - 1.0) * previous -
                            (l + m - 1.0) * beforePrevious) /
                           static_cast<double>(l - m);
            }
            beforePrevious = previous;
            previous = legendre;

            const double normalisation = HarmonicNormalisation(l, m);
            const int band = l * l + l;
            if (m == 0) {
                accumulate(band, normalisation * legendre);
            } else {
                const double scale = std::sqrt(2.0) * normalisation * legendre;
                accumulate(band + m, scale * cosMPhi);
                accumulate(band - m, scale * sinMPhi);
            }
        }
    }

    rgb[0] = static_cast<float>(accumulated[0]);
    rgb[1] = static_cast<float>(accumulated[1]);
    rgb[2] = static_cast<float>(accumulated[2]);
}

float SplatKernelResponse(const Splat& splat, SplatKernel kernel,
                          const float point[3])
{
    const double offset[3] = {
        static_cast<double>(point[0]) - static_cast<double>(splat.center[0]),
        static_cast<double>(point[1]) - static_cast<double>(splat.center[1]),
        static_cast<double>(point[2]) - static_cast<double>(splat.center[2])};
    double local[3];
    Apply3x3(splat.inverseTransform, offset, local);

    const double radius = SplatSupportRadius(kernel);
    if (SplatKernelIsFlat(kernel)) {
        // The specification puts a surflet's opacity on the local XY plane and
        // says it is zero off it. That is what this returns: the field itself,
        // not a thickened stand-in for it. A ray meets the plane exactly, which
        // is what SplatRayPeak does.
        if (local[2] != 0.0) {
            return 0.0f;
        }
        const double planar =
            std::sqrt(local[0] * local[0] + local[1] * local[1]);
        if (planar > radius) {
            return 0.0f;
        }
        if (kernel == SplatKernel::ConstantSurflet) {
            return 1.0f;
        }
        return static_cast<float>(std::exp(-0.5 * planar * planar));
    }

    const double squared =
        local[0] * local[0] + local[1] * local[1] + local[2] * local[2];
    if (squared > radius * radius) {
        return 0.0f;
    }
    return static_cast<float>(std::exp(-0.5 * squared));
}

bool SplatRayPeak(const Splat& splat, SplatKernel kernel,
                  const float origin[3], const float direction[3], float tMin,
                  float tMax, float* t, float* response)
{
    if (!(tMax >= tMin)) {
        return false;
    }
    const double offset[3] = {
        static_cast<double>(origin[0]) - static_cast<double>(splat.center[0]),
        static_cast<double>(origin[1]) - static_cast<double>(splat.center[1]),
        static_cast<double>(origin[2]) - static_cast<double>(splat.center[2])};
    const double along[3] = {static_cast<double>(direction[0]),
                             static_cast<double>(direction[1]),
                             static_cast<double>(direction[2])};

    double b[3];
    double a[3];
    Apply3x3(splat.inverseTransform, offset, b);
    Apply3x3(splat.inverseTransform, along, a);

    const double radius = SplatSupportRadius(kernel);

    if (SplatKernelIsFlat(kernel)) {
        // A flat kernel has no thickness, so there is one candidate distance
        // rather than a peak: where the ray crosses the plane the disk lies in.
        if (a[2] == 0.0) {
            return false;
        }
        const double hit = -b[2] / a[2];
        if (!(hit >= tMin) || !(hit <= tMax)) {
            return false;
        }
        const double u = b[0] + hit * a[0];
        const double v = b[1] + hit * a[1];
        const double planar = std::sqrt(u * u + v * v);
        if (planar > radius) {
            return false;
        }
        if (t != nullptr) {
            *t = static_cast<float>(hit);
        }
        if (response != nullptr) {
            *response = kernel == SplatKernel::ConstantSurflet
                            ? 1.0f
                            : static_cast<float>(std::exp(-0.5 * planar * planar));
        }
        return true;
    }

    const double aa = a[0] * a[0] + a[1] * a[1] + a[2] * a[2];
    const double ab = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    const double bb = b[0] * b[0] + b[1] * b[1] + b[2] * b[2];

    // Where |b + t a| is least. A ray that is degenerate in this metric -- a
    // zero direction, or one the transform annihilates -- has a constant
    // response, and the nearest end of the interval is as good as any.
    double peak = aa > 0.0 ? -ab / aa : static_cast<double>(tMin);
    peak = std::min(std::max(peak, static_cast<double>(tMin)),
                    static_cast<double>(tMax));

    const double squared = std::max(0.0, bb + peak * (2.0 * ab + peak * aa));
    if (squared > radius * radius) {
        return false;
    }
    if (t != nullptr) {
        *t = static_cast<float>(peak);
    }
    if (response != nullptr) {
        *response = static_cast<float>(std::exp(-0.5 * squared));
    }
    return true;
}

void SplatBounds(const Splat& splat, SplatKernel kernel, float minimum[3],
                 float maximum[3])
{
    // The support is { M x : |x| <= r } with M the inverse of what the splat
    // carries, so the half extent along an axis is r times the length of that
    // row of M. A flat kernel's support is a disk, so its third column takes no
    // part -- which is the only place the flatness reaches the bounds.
    double forward[9];
    if (!Invert3x3(splat.inverseTransform, forward)) {
        for (int axis = 0; axis < 3; ++axis) {
            minimum[axis] = splat.center[axis];
            maximum[axis] = splat.center[axis];
        }
        return;
    }
    const double radius = SplatSupportRadius(kernel);
    const int columns = SplatKernelIsFlat(kernel) ? 2 : 3;
    for (int row = 0; row < 3; ++row) {
        double squared = 0.0;
        for (int column = 0; column < columns; ++column) {
            const double element = forward[row * 3 + column];
            squared += element * element;
        }
        const double extent = radius * std::sqrt(squared);
        minimum[row] = static_cast<float>(splat.center[row] - extent);
        maximum[row] = static_cast<float>(splat.center[row] + extent);
    }
}

SplatCloud BuildSplatCloud(const SplatCloudSource& source)
{
    SplatCloud cloud;
    cloud.kernel = source.kernel;

    const std::size_t count = source.positions.size() / 3;
    if (count == 0) {
        if (!source.positions.empty()) {
            cloud.reports.push_back(
                "positions holds " + std::to_string(source.positions.size()) +
                " floats, which is not three per particle; the field is empty");
        }
        return cloud;
    }
    if (source.positions.size() % 3 != 0) {
        cloud.reports.push_back(
            "positions holds " + std::to_string(source.positions.size()) +
            " floats, which is not a whole number of particles; the last "
            "partial position is ignored");
    }

    // Each per-particle array is resolved by the rule
    // ParticleFieldPositionBaseAPI states: too long is truncated, too short is
    // discarded entirely for the attribute's documented default. Neither case
    // is silent.
    const auto resolve = [&](const std::vector<float>& authored,
                             std::size_t stride, const char* name) -> bool {
        if (authored.empty()) {
            return false;
        }
        const std::size_t supplied = authored.size() / stride;
        if (supplied < count) {
            cloud.reports.push_back(
                std::string(name) + " supplies " + std::to_string(supplied) +
                " values for " + std::to_string(count) +
                " particles; the schema discards a short array, so its default "
                "is used for every particle");
            return false;
        }
        if (supplied > count) {
            cloud.reports.push_back(
                std::string(name) + " supplies " + std::to_string(supplied) +
                " values for " + std::to_string(count) +
                " particles; the surplus is truncated");
        }
        return true;
    };

    const bool hasOrientations =
        resolve(source.orientations, 4, "orientations");
    const bool hasScales = resolve(source.scales, 3, "scales");
    const bool hasOpacities = resolve(source.opacities, 1, "opacities");

    int degree = source.sphericalHarmonicsDegree;
    if (degree < 0) {
        cloud.reports.push_back(
            "radiance:sphericalHarmonicsDegree is " + std::to_string(degree) +
            ", which is not a degree; the schema's fallback radiance is used");
        degree = 0;
    }
    const std::size_t coefficients = SphericalHarmonicsCoefficientCount(degree);
    const bool hasHarmonics =
        degree == source.sphericalHarmonicsDegree &&
        resolve(source.sphericalHarmonics, coefficients * 3,
                "radiance:sphericalHarmonicsCoefficients");

    if (hasHarmonics) {
        cloud.sphericalHarmonicsDegree = degree;
        cloud.sphericalHarmonics.resize(count * coefficients * 3);
        std::copy(source.sphericalHarmonics.begin(),
                  source.sphericalHarmonics.begin() +
                      static_cast<std::ptrdiff_t>(cloud.sphericalHarmonics.size()),
                  cloud.sphericalHarmonics.begin());
    } else {
        // The schema's own fallback: a DC signal of (0.5, 0.5, 0.5) at degree 0.
        cloud.sphericalHarmonicsDegree = 0;
        cloud.sphericalHarmonics.assign(count * 3,
                                        SphericalHarmonicsFallbackCoefficient());
    }

    cloud.splats.reserve(count);
    std::vector<float> keptHarmonics;
    const std::size_t stride = cloud.CoefficientsPerParticle() * 3;
    keptHarmonics.reserve(cloud.sphericalHarmonics.size());

    std::size_t degenerate = 0;
    float lowestOpacity = std::numeric_limits<float>::infinity();
    float highestOpacity = -std::numeric_limits<float>::infinity();
    bool opacityOutOfRange = false;
    bool boundsStarted = false;

    for (std::size_t index = 0; index < count; ++index) {
        double scale[3] = {1.0, 1.0, 1.0};
        if (hasScales) {
            for (int axis = 0; axis < 3; ++axis) {
                scale[axis] = source.scales[index * 3 + axis];
            }
        }
        if (!(scale[0] > 0.0) || !(scale[1] > 0.0) || !(scale[2] > 0.0)) {
            ++degenerate;
            continue;
        }

        double w = 1.0, x = 0.0, y = 0.0, z = 0.0;
        if (hasOrientations) {
            w = source.orientations[index * 4 + 0];
            x = source.orientations[index * 4 + 1];
            y = source.orientations[index * 4 + 2];
            z = source.orientations[index * 4 + 3];
            const double norm = std::sqrt(w * w + x * x + y * y + z * z);
            if (norm > 0.0) {
                w /= norm;
                x /= norm;
                y /= norm;
                z /= norm;
            } else {
                w = 1.0;
                x = y = z = 0.0;
            }
        }

        // The rotation, column by column, because a column is what a row of the
        // inverse needs: row j of inv(S) transpose(R) is column j of R over
        // scale j.
        const double rotation[3][3] = {
            {1.0 - 2.0 * (y * y + z * z), 2.0 * (x * y - w * z),
             2.0 * (x * z + w * y)},
            {2.0 * (x * y + w * z), 1.0 - 2.0 * (x * x + z * z),
             2.0 * (y * z - w * x)},
            {2.0 * (x * z - w * y), 2.0 * (y * z + w * x),
             1.0 - 2.0 * (x * x + y * y)}};

        Splat splat;
        for (int axis = 0; axis < 3; ++axis) {
            splat.center[axis] = source.positions[index * 3 + axis];
        }
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 3; ++column) {
                splat.inverseTransform[row * 3 + column] =
                    static_cast<float>(rotation[column][row] / scale[row]);
            }
        }
        splat.opacity = hasOpacities ? source.opacities[index] : 1.0f;
        lowestOpacity = std::min(lowestOpacity, splat.opacity);
        highestOpacity = std::max(highestOpacity, splat.opacity);
        if (splat.opacity < 0.0f || splat.opacity > 1.0f) {
            opacityOutOfRange = true;
        }

        float minimum[3];
        float maximum[3];
        SplatBounds(splat, cloud.kernel, minimum, maximum);
        for (int axis = 0; axis < 3; ++axis) {
            if (!boundsStarted) {
                cloud.boundsMin[axis] = minimum[axis];
                cloud.boundsMax[axis] = maximum[axis];
            } else {
                cloud.boundsMin[axis] =
                    std::min(cloud.boundsMin[axis], minimum[axis]);
                cloud.boundsMax[axis] =
                    std::max(cloud.boundsMax[axis], maximum[axis]);
            }
        }
        boundsStarted = true;

        if (stride > 0) {
            const std::size_t base = index * stride;
            keptHarmonics.insert(
                keptHarmonics.end(),
                cloud.sphericalHarmonics.begin() +
                    static_cast<std::ptrdiff_t>(base),
                cloud.sphericalHarmonics.begin() +
                    static_cast<std::ptrdiff_t>(base + stride));
        }
        cloud.splats.push_back(splat);
    }

    cloud.sphericalHarmonics = std::move(keptHarmonics);

    if (degenerate > 0) {
        cloud.reports.push_back(
            std::to_string(degenerate) + " of " + std::to_string(count) +
            " particles have a scale of zero or less on some axis, which has no "
            "support and no invertible kernel; they are dropped");
    }
    if (opacityOutOfRange) {
        cloud.reports.push_back(
            "opacities range over [" + std::to_string(lowestOpacity) + ", " +
            std::to_string(highestOpacity) +
            "], outside the [0, 1] the schema specifies; they are used as "
            "authored, and a PLY's sigmoid-activated values need converting in "
            "the scene rather than here");
    }

    // A negative DC radiance, which is the one radiance error worth testing for
    // by itself.
    //
    // The DC coefficient is proportional to the mean radiance over the sphere,
    // so a negative one is not the ringing a truncated series legitimately
    // produces at some directions -- it is a mean emission below zero, which no
    // emitter has. It has exactly one common cause: the reference 3D Gaussian
    // splatting implementation computes colour as `0.5 + Y(0,0) f_dc`, while USD
    // specifies `colour = Y(0,0) c`, so a converter that wrote `f_dc` straight
    // through leaves every coefficient short by `0.5 / Y(0,0)` -- which is
    // `sqrt(pi)`, the same number the schema's own fallback implies.
    //
    // Reported, not corrected. Adding the offset here would render a plausible
    // picture from data that says something else, and would do it to assets that
    // are authored correctly too.
    if (!cloud.splats.empty() && !cloud.sphericalHarmonics.empty()) {
        const double y00 = HarmonicNormalisation(0, 0);
        std::size_t negative = 0;
        double lowest = 0.0;
        for (std::size_t particle = 0; particle < cloud.splats.size();
             ++particle) {
            const float* dc =
                cloud.sphericalHarmonics.data() + particle * stride;
            for (int channel = 0; channel < 3; ++channel) {
                const double radiance = dc[channel] * y00;
                if (radiance < 0.0) {
                    ++negative;
                    lowest = std::min(lowest, radiance);
                }
            }
        }
        if (negative > 0) {
            const std::size_t channels = cloud.splats.size() * 3;
            const double percent =
                100.0 * static_cast<double>(negative) / static_cast<double>(channels);
            cloud.reports.push_back(
                std::to_string(negative) + " of " + std::to_string(channels) +
                " DC radiance channels are negative (" +
                std::to_string(percent) + "%, lowest " +
                std::to_string(lowest) +
                "). The DC coefficient is the mean radiance over the sphere and "
                "cannot be negative. The usual cause is the reference 3D "
                "Gaussian splatting convention, colour = 0.5 + Y(0,0) f_dc, "
                "written through unchanged: USD specifies colour = Y(0,0) c, so "
                "the DC coefficients are short by sqrt(pi) = 1.7724539 per "
                "channel. Fix it in the scene; hdClaude renders the radiance as "
                "authored");
        }
    }

    return cloud;
}


// ---------------------------------------------------------------------------
// The volumetric reading
// ---------------------------------------------------------------------------

namespace {

/// The three scale factors a particle was built from, recovered exactly.
///
/// Column `j` of the forward transform is `scale[j]` times column `j` of a
/// rotation, and a rotation's columns are unit, so a column's length *is* the
/// scale. Nothing is estimated and no decomposition is solved for.
bool SplatScales(const Splat& splat, double scale[3])
{
    double forward[9];
    if (!Invert3x3(splat.inverseTransform, forward)) {
        return false;
    }
    for (int column = 0; column < 3; ++column) {
        const double x = forward[0 * 3 + column];
        const double y = forward[1 * 3 + column];
        const double z = forward[2 * 3 + column];
        scale[column] = std::sqrt(x * x + y * y + z * z);
    }
    return scale[0] > 0.0 && scale[1] > 0.0 && scale[2] > 0.0;
}

}  // namespace

float SplatExtinction(const Splat& splat, float maximumOpticalDepth)
{
    double scale[3];
    if (!SplatScales(splat, scale)) {
        return 0.0f;
    }
    // The radius of the sphere of the same volume, which is what makes this
    // exact for an isotropic particle and symmetric about the authored value
    // for any other.
    const double mean = std::cbrt(scale[0] * scale[1] * scale[2]);
    if (!(mean > 0.0)) {
        return 0.0f;
    }

    const double opacity = std::min(std::max(double(splat.opacity), 0.0), 1.0);
    // -ln(1 - o), bounded where it diverges. An opacity of one really is an
    // infinite optical depth; the bound is on the representation, not on the
    // data, and at the default of 20 it leaves a transmittance of 2e-9.
    double depth = opacity >= 1.0 ? double(maximumOpticalDepth)
                                  : -std::log(1.0 - opacity);
    depth = std::min(depth, double(maximumOpticalDepth));

    // A ray through the centre of an isotropic particle integrates
    // exp(-0.5 (t/s)^2) over the whole line, which is s sqrt(2 pi).
    constexpr double kRootTwoPi = 2.5066282746310002;
    return static_cast<float>(depth / (kRootTwoPi * mean));
}

std::size_t SplatMajorantGrid::CellCount() const
{
    return static_cast<std::size_t>(resolution[0]) *
           static_cast<std::size_t>(resolution[1]) *
           static_cast<std::size_t>(resolution[2]);
}

int SplatMajorantGrid::CellAt(const float point[3]) const
{
    int coordinate[3];
    for (int axis = 0; axis < 3; ++axis) {
        const float offset = point[axis] - origin[axis];
        const int index = static_cast<int>(std::floor(offset / cellSize[axis]));
        if (index < 0 || index >= resolution[axis]) {
            return -1;
        }
        coordinate[axis] = index;
    }
    return (coordinate[2] * resolution[1] + coordinate[1]) * resolution[0] +
           coordinate[0];
}

float SplatMajorantGrid::MajorantAt(const float point[3]) const
{
    const int cell = CellAt(point);
    if (cell < 0 || static_cast<std::size_t>(cell) >= majorant.size()) {
        return 0.0f;
    }
    return majorant[static_cast<std::size_t>(cell)];
}

SplatMajorantGrid BuildSplatMajorantGrid(const SplatCloud& cloud,
                                         std::size_t targetCells,
                                         float maximumOpticalDepth)
{
    SplatMajorantGrid grid;
    if (cloud.splats.empty()) {
        grid.offsets.assign(2, 0);
        grid.majorant.assign(1, 0.0f);
        return grid;
    }

    double extent[3];
    for (int axis = 0; axis < 3; ++axis) {
        grid.origin[axis] = cloud.boundsMin[axis];
        extent[axis] = double(cloud.boundsMax[axis]) - double(cloud.boundsMin[axis]);
        // A cloud that is flat on an axis -- one particle, or a plane of them --
        // still needs a width to divide by.
        if (!(extent[axis] > 0.0)) {
            extent[axis] = 1.0e-4;
        }
    }

    // About one cell per particle unless the caller says otherwise. See the
    // header: a cell that holds eight particles bounds eight times the density
    // any point in it has, and every one of those is a rejected collision.
    if (targetCells == 0) {
        targetCells = std::min<std::size_t>(
            std::max<std::size_t>(cloud.splats.size(), 4096),
            std::size_t(1) << 21);
    }

    // Roughly cubical cells, about `targetCells` of them. A cell much longer on
    // one axis bounds badly along it and wastes steps across it.
    const double volume = extent[0] * extent[1] * extent[2];
    const double side =
        std::cbrt(volume / static_cast<double>(std::max<std::size_t>(1, targetCells)));
    for (int axis = 0; axis < 3; ++axis) {
        const int count = static_cast<int>(std::ceil(extent[axis] / std::max(side, 1.0e-6)));
        grid.resolution[axis] = std::min(std::max(count, 1), 1024);
        grid.cellSize[axis] =
            static_cast<float>(extent[axis] / grid.resolution[axis]);
    }

    const std::size_t cells = grid.CellCount();
    grid.majorant.assign(cells, 0.0f);

    // Two passes over the particles: count what each cell holds, then fill.
    // Counting first is what keeps this one allocation rather than one per cell.
    std::vector<std::uint32_t> counts(cells, 0);
    const float radius = SplatSupportRadius(cloud.kernel);

    struct Reach {
        int lo[3];
        int hi[3];
        double centre[3];
        double density;
        /// The particle's unit principal axes, and the scale along each.
        ///
        /// Both, because the bound below is per axis. Dividing a world distance
        /// by the *largest* scale is also a valid bound and is what this did
        /// first; on a real capture, whose scales run from 0.0009 to 0.067, it
        /// let one flat particle claim its full peak in seventeen thousand cells
        /// at once. The majorant was then thousands of times the density
        /// anywhere in those cells, which is correct, useless, and -- through a
        /// trial cap -- how a cloud came to render as black cubes.
        double axis[3][3];
        double scale[3];
    };
    std::vector<Reach> reach(cloud.splats.size());

    for (std::size_t i = 0; i < cloud.splats.size(); ++i) {
        const Splat& splat = cloud.splats[i];
        Reach& entry = reach[i];

        float minimum[3];
        float maximum[3];
        SplatBounds(splat, cloud.kernel, minimum, maximum);

        double forward[9];
        const bool invertible = Invert3x3(splat.inverseTransform, forward);
        for (int column = 0; column < 3; ++column) {
            double length = 0.0;
            for (int row = 0; row < 3; ++row) {
                const double element = invertible ? forward[row * 3 + column] : 0.0;
                entry.axis[column][row] = element;
                length += element * element;
            }
            length = std::sqrt(length);
            entry.scale[column] = length;
            // A column of the forward transform is its scale times a *unit*
            // column of the rotation, so dividing by the length recovers the
            // axis exactly.
            for (int row = 0; row < 3; ++row) {
                entry.axis[column][row] =
                    length > 0.0 ? entry.axis[column][row] / length : 0.0;
            }
        }
        entry.density = SplatExtinction(splat, maximumOpticalDepth);
        for (int axis = 0; axis < 3; ++axis) {
            entry.centre[axis] = splat.center[axis];
            const double low = (double(minimum[axis]) - grid.origin[axis]) /
                               grid.cellSize[axis];
            const double high = (double(maximum[axis]) - grid.origin[axis]) /
                                grid.cellSize[axis];
            entry.lo[axis] = std::min(std::max(int(std::floor(low)), 0),
                                      grid.resolution[axis] - 1);
            entry.hi[axis] = std::min(std::max(int(std::floor(high)), 0),
                                      grid.resolution[axis] - 1);
        }
        for (int z = entry.lo[2]; z <= entry.hi[2]; ++z) {
            for (int y = entry.lo[1]; y <= entry.hi[1]; ++y) {
                for (int x = entry.lo[0]; x <= entry.hi[0]; ++x) {
                    const std::size_t cell = static_cast<std::size_t>(
                        (z * grid.resolution[1] + y) * grid.resolution[0] + x);
                    ++counts[cell];
                }
            }
        }
    }

    grid.offsets.assign(cells + 1, 0);
    for (std::size_t cell = 0; cell < cells; ++cell) {
        grid.offsets[cell + 1] = grid.offsets[cell] + counts[cell];
    }
    grid.indices.assign(grid.offsets[cells], 0);

    std::vector<std::uint32_t> cursor(grid.offsets.begin(),
                                      grid.offsets.begin() +
                                          static_cast<std::ptrdiff_t>(cells));

    for (std::size_t i = 0; i < cloud.splats.size(); ++i) {
        const Reach& entry = reach[i];
        if (!(entry.density > 0.0) || !(entry.scale[0] > 0.0) ||
            !(entry.scale[1] > 0.0) || !(entry.scale[2] > 0.0)) {
            continue;
        }
        for (int z = entry.lo[2]; z <= entry.hi[2]; ++z) {
            for (int y = entry.lo[1]; y <= entry.hi[1]; ++y) {
                for (int x = entry.lo[0]; x <= entry.hi[0]; ++x) {
                    const std::size_t cell = static_cast<std::size_t>(
                        (z * grid.resolution[1] + y) * grid.resolution[0] + x);
                    grid.indices[cursor[cell]++] =
                        static_cast<std::uint32_t>(i);

                    // The bound, per principal axis.
                    //
                    // The Mahalanobis distance is the sum over the particle's
                    // three axes of the squared offset along each, divided by
                    // that axis's scale. Each term can be minimised over the
                    // cell independently, and the sum of those minima is a
                    // lower bound on the distance -- and therefore an upper
                    // bound on the response, which is what a majorant must be.
                    // Doing it per axis rather than with one worst-case scale
                    // is what stops a long thin particle claiming its peak
                    // everywhere its longest axis can reach.
                    //
                    // The minimum of |(p - c) . u| over an axis-aligned box is
                    // the box's support function along u: the centre offset,
                    // less what the half extents can give back along that
                    // direction, floored at zero for a box the centre is inside.
                    double offset[3];
                    double half[3];
                    for (int axis = 0; axis < 3; ++axis) {
                        const int coordinate = axis == 0 ? x : (axis == 1 ? y : z);
                        const double low = grid.origin[axis] +
                                           double(coordinate) * grid.cellSize[axis];
                        half[axis] = 0.5 * grid.cellSize[axis];
                        offset[axis] = low + half[axis] - entry.centre[axis];
                    }
                    double squared = 0.0;
                    for (int principal = 0; principal < 3; ++principal) {
                        const double* direction = entry.axis[principal];
                        double along = 0.0;
                        double reach = 0.0;
                        for (int axis = 0; axis < 3; ++axis) {
                            along += offset[axis] * direction[axis];
                            reach += half[axis] * std::fabs(direction[axis]);
                        }
                        const double nearest =
                            std::max(0.0, std::fabs(along) - reach);
                        const double scaled = nearest / entry.scale[principal];
                        squared += scaled * scaled;
                    }
                    const double response =
                        squared >= radius * radius
                            ? 0.0
                            : std::exp(-0.5 * squared);
                    grid.majorant[cell] +=
                        static_cast<float>(entry.density * response);
                }
            }
        }
    }

    return grid;
}

float EvaluateSplatDensity(const SplatCloud& cloud,
                           const SplatMajorantGrid& grid, const float point[3],
                           float maximumOpticalDepth)
{
    const int cell = grid.CellAt(point);
    if (cell < 0 || static_cast<std::size_t>(cell) + 1 >= grid.offsets.size()) {
        return 0.0f;
    }
    double total = 0.0;
    for (std::uint32_t at = grid.offsets[static_cast<std::size_t>(cell)];
         at < grid.offsets[static_cast<std::size_t>(cell) + 1]; ++at) {
        const std::uint32_t index = grid.indices[at];
        if (index >= cloud.splats.size()) {
            continue;
        }
        const Splat& splat = cloud.splats[index];
        const double response =
            SplatKernelResponse(splat, cloud.kernel, point);
        if (response > 0.0) {
            total += double(SplatExtinction(splat, maximumOpticalDepth)) * response;
        }
    }
    return static_cast<float>(total);
}

}  // namespace hdclaude
