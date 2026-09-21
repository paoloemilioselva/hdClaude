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

    return cloud;
}

}  // namespace hdclaude
