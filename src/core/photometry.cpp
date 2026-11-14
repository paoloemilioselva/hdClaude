#include "hdclaude/core/photometry.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace hdclaude {

namespace {

constexpr double kPi = 3.14159265358979323846;

struct V3 {
    double x = 0.0, y = 0.0, z = 0.0;
};

V3 Load(const double (&v)[3]) { return {v[0], v[1], v[2]}; }
V3 operator+(const V3& a, const V3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(const V3& a, const V3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(const V3& a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double Dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Cross(const V3& a, const V3& b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
double Length(const V3& a) { return std::sqrt(Dot(a, a)); }
V3 Normalized(const V3& a)
{
    const double length = Length(a);
    return length > 0.0 ? a * (1.0 / length) : V3{0.0, 0.0, 1.0};
}

/// Two unit vectors completing `n` to an orthonormal frame.
void Frame(const V3& n, V3* e1, V3* e2)
{
    const V3 helper = std::fabs(n.x) < 0.9 ? V3{1.0, 0.0, 0.0} : V3{0.0, 1.0, 0.0};
    *e1 = Normalized(Cross(helper, n));
    *e2 = Cross(n, *e1);
}

double Smoothstep(double edge0, double edge1, double x)
{
    const double t = std::clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

/// UsdLux's cone term at an angle `offAxis` (radians) from the light's axis.
double ConeFactor(const EmitterShaping& shaping, double offAxis)
{
    if (!(shaping.coneCosAngle > -1.0f)) {
        return 1.0;
    }
    const double cutoff = std::acos(std::clamp(static_cast<double>(shaping.coneCosAngle),
                                               -1.0, 1.0));
    if (offAxis > cutoff) {
        return 0.0;
    }
    const double softness = std::clamp(static_cast<double>(shaping.coneSoftness), 0.0, 1.0);
    const double smoothStart = cutoff * (1.0 - softness);
    if (!(cutoff > smoothStart)) {
        return 1.0;
    }
    return 1.0 - Smoothstep(smoothStart, cutoff, offAxis);
}

/// UsdLux's focus term for a cosine to the emitting surface's normal.
double FocusFactor(const EmitterShaping& shaping, double cosNormal)
{
    return shaping.focus > 0.0f
               ? std::pow(std::fabs(cosNormal), static_cast<double>(shaping.focus))
               : 1.0;
}

/// The off-axis angles where the cone term has a kink or a step, so that
/// quadrature can be split there and stay accurate on each smooth piece.
std::vector<double> Breakpoints(const EmitterShaping& shaping, double low, double high)
{
    std::vector<double> points = {low, high, 0.5 * kPi};
    if (shaping.coneCosAngle > -1.0f) {
        const double cutoff = std::acos(
            std::clamp(static_cast<double>(shaping.coneCosAngle), -1.0, 1.0));
        const double softness =
            std::clamp(static_cast<double>(shaping.coneSoftness), 0.0, 1.0);
        points.push_back(cutoff);
        points.push_back(cutoff * (1.0 - softness));
    }
    std::vector<double> inside;
    for (double p : points) {
        if (p >= low && p <= high) {
            inside.push_back(p);
        }
    }
    std::sort(inside.begin(), inside.end());
    inside.erase(std::unique(inside.begin(), inside.end()), inside.end());
    return inside;
}

/// Composite Simpson in the off-axis angle over [low, high], split at the
/// cone's breakpoints. Integrating in the angle rather than its cosine keeps
/// UsdLux's smoothstep, which is defined in the angle, smooth on every piece.
template <typename Function>
double IntegrateAngle(const EmitterShaping& shaping, double low, double high,
                      const Function& f, int intervals = 4096)
{
    const std::vector<double> points = Breakpoints(shaping, low, high);
    double total = 0.0;
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        const double a = points[i];
        const double b = points[i + 1];
        if (!(b > a)) {
            continue;
        }
        const double h = (b - a) / intervals;
        // The endpoints are evaluated just inside the piece, so a step that
        // sits exactly on one is taken from the side the piece is on.
        const double nudge = h * 1e-9;
        double sum = f(a + nudge) + f(b - nudge);
        for (int k = 1; k < intervals; ++k) {
            sum += f(a + k * h) * (k % 2 == 1 ? 4.0 : 2.0);
        }
        total += sum * h / 3.0;
    }
    return total;
}

/// Illuminance per unit luminance on the axis, by midpoint quadrature over the
/// surface. `surface(a, b, &normal)` maps the unit square onto the surface
/// uniformly in area and gives its outward normal there.
template <typename Surface>
double SurfaceIlluminance(const AreaEmitter& emitter, double area, double distance,
                          const Surface& surface)
{
    constexpr int kSide = 1024;
    const V3 centre = Load(emitter.position);
    const V3 axis = Normalized(Load(emitter.direction));
    const V3 receiver = centre + axis * distance;
    double total = 0.0;
    for (int i = 0; i < kSide; ++i) {
        const double a = (i + 0.5) / kSide;
        for (int j = 0; j < kSide; ++j) {
            const double b = (j + 0.5) / kSide;
            V3 normal;
            const V3 x = surface(a, b, &normal);
            const V3 toReceiver = receiver - x;
            const double r2 = Dot(toReceiver, toReceiver);
            if (!(r2 > 0.0)) {
                continue;
            }
            const V3 w = toReceiver * (1.0 / std::sqrt(r2));
            const double emitted = Dot(normal, w);
            const double received = Dot(axis, w);
            if (emitted <= 0.0 || received <= 0.0) {
                continue;
            }
            total += ShapingAttenuation(emitter.shaping, static_cast<float>(Dot(axis, w)),
                                        static_cast<float>(emitted)) *
                     emitted * received / r2;
        }
    }
    return total * area / (static_cast<double>(kSide) * kSide);
}

}  // namespace

float ShapingAttenuation(const EmitterShaping& shaping, float cosAxis, float cosNormal)
{
    if (!shaping.Shaped()) {
        return 1.0f;
    }
    const double offAxis = std::acos(std::clamp(static_cast<double>(cosAxis), -1.0, 1.0));
    return static_cast<float>(ConeFactor(shaping, offAxis) *
                              FocusFactor(shaping, cosNormal));
}

double EmitterArea(const AreaEmitter& emitter)
{
    switch (emitter.shape) {
    case EmitterShape::Rect:
        return 4.0 * Length(Load(emitter.uAxis)) * Length(Load(emitter.vAxis));
    case EmitterShape::Disk:
        return kPi * emitter.radius * emitter.radius;
    case EmitterShape::Sphere:
        return 4.0 * kPi * emitter.radius * emitter.radius;
    case EmitterShape::Cylinder:
        return 2.0 * kPi * emitter.radius * 2.0 * Length(Load(emitter.uAxis));
    }
    return 0.0;
}

double EmittedPowerPerLuminance(const AreaEmitter& emitter)
{
    const double area = EmitterArea(emitter);
    if (!emitter.shaping.Shaped()) {
        return kPi * area;
    }
    const EmitterShaping& shaping = emitter.shaping;
    const double focus = std::max(0.0, static_cast<double>(shaping.focus));
    switch (emitter.shape) {
    case EmitterShape::Rect:
    case EmitterShape::Disk:
        // The face's normal is the light's axis, so the focus cosine is the
        // off-axis one: area * 2 pi * integral of cone * cos^(focus+1) sin.
        return area * 2.0 * kPi *
               IntegrateAngle(shaping, 0.0, 0.5 * kPi, [&](double theta) {
                   const double c = std::cos(theta);
                   return ConeFactor(shaping, theta) * std::pow(c, focus + 1.0) *
                          std::sin(theta);
               });
    case EmitterShape::Sphere: {
        // Separable: whatever the direction, the points facing it integrate
        // max(0, n.w)^(focus+1) over the sphere to r^2 * 2 pi / (focus + 2),
        // and only the cone depends on the direction.
        const double r = emitter.radius;
        return r * r * 2.0 * kPi / (focus + 2.0) * 2.0 * kPi *
               IntegrateAngle(shaping, 0.0, kPi, [&](double theta) {
                   return ConeFactor(shaping, theta) * std::sin(theta);
               });
    }
    case EmitterShape::Cylinder: {
        // A direction at angle alpha to the cylinder's axis integrates
        // max(0, n.w)^(focus+1) over the lateral surface to
        // r l sin(alpha)^(focus+1) B, with B = 2 * integral over [0, pi/2] of
        // cos^(focus+1), a beta function (2 for focus 0: the silhouette
        // 2 r l sin(alpha)). The cone is about the light's -Z and the cylinder
        // runs along its X, so the rest is integrated over the sphere:
        // Simpson in the off-axis angle, the midpoint rule around it, which is
        // spectrally accurate for a smooth periodic integrand.
        const V3 axis = Normalized(Load(emitter.direction));
        const V3 along = Normalized(Load(emitter.uAxis));
        const double length = 2.0 * Length(Load(emitter.uAxis));
        const double beta = std::sqrt(kPi) * std::tgamma(0.5 * (focus + 2.0)) /
                            std::tgamma(0.5 * (focus + 3.0));
        V3 e1, e2;
        Frame(axis, &e1, &e2);
        const double a0 = Dot(axis, along);
        const double a1 = Dot(e1, along);
        const double a2 = Dot(e2, along);
        constexpr int kAround = 512;
        const auto ring = [&](double theta) {
            const double cone = ConeFactor(shaping, theta);
            if (cone <= 0.0) {
                return 0.0;
            }
            const double sinTheta = std::sin(theta);
            const double cosTheta = std::cos(theta);
            double sum = 0.0;
            for (int k = 0; k < kAround; ++k) {
                const double phi = 2.0 * kPi * (k + 0.5) / kAround;
                const double c =
                    cosTheta * a0 + sinTheta * (std::cos(phi) * a1 + std::sin(phi) * a2);
                const double sinAlpha = std::sqrt(std::max(0.0, 1.0 - c * c));
                sum += std::pow(sinAlpha, focus + 1.0);
            }
            return cone * sum * (2.0 * kPi / kAround) * sinTheta;
        };
        return emitter.radius * length * beta * IntegrateAngle(shaping, 0.0, kPi, ring, 1024);
    }
    }
    return 0.0;
}

double OnAxisIlluminancePerLuminance(const AreaEmitter& emitter, double distance)
{
    if (!(distance > 0.0)) {
        return 0.0;
    }
    const double area = EmitterArea(emitter);
    const bool shaped = emitter.shaping.Shaped();
    const V3 centre = Load(emitter.position);
    const V3 axis = Normalized(Load(emitter.direction));

    switch (emitter.shape) {
    case EmitterShape::Rect: {
        const V3 u = Load(emitter.uAxis);
        const V3 v = Load(emitter.vAxis);
        if (!shaped) {
            // A differential surface parallel to a rectangle, on the normal
            // through its centre: four times the configuration factor to a
            // parallel rectangle with a corner on the surface's normal
            // (Howell's catalogue of configuration factors), times pi.
            const double x = Length(u) / distance;
            const double y = Length(v) / distance;
            const double sx = std::sqrt(1.0 + x * x);
            const double sy = std::sqrt(1.0 + y * y);
            return 2.0 * (x / sx * std::atan(y / sx) + y / sy * std::atan(x / sy));
        }
        return SurfaceIlluminance(emitter, area, distance, [&](double a, double b, V3* n) {
            *n = axis;
            return centre + u * (2.0 * a - 1.0) + v * (2.0 * b - 1.0);
        });
    }
    case EmitterShape::Disk: {
        const double r = emitter.radius;
        if (!shaped) {
            return kPi * r * r / (distance * distance + r * r);
        }
        V3 e1, e2;
        Frame(axis, &e1, &e2);
        return SurfaceIlluminance(emitter, area, distance, [&](double a, double b, V3* n) {
            *n = axis;
            const double rho = r * std::sqrt(a);
            const double psi = 2.0 * kPi * b;
            return centre + e1 * (rho * std::cos(psi)) + e2 * (rho * std::sin(psi));
        });
    }
    case EmitterShape::Sphere: {
        const double r = emitter.radius;
        if (distance <= r) {
            return 0.0;
        }
        if (!shaped) {
            return kPi * (r / distance) * (r / distance);
        }
        return SurfaceIlluminance(emitter, area, distance, [&](double a, double b, V3* n) {
            const double z = 1.0 - 2.0 * a;
            const double s = std::sqrt(std::max(0.0, 1.0 - z * z));
            const double phi = 2.0 * kPi * b;
            *n = V3{s * std::cos(phi), s * std::sin(phi), z};
            return centre + *n * r;
        });
    }
    case EmitterShape::Cylinder: {
        const V3 u = Load(emitter.uAxis);
        const V3 along = Normalized(u);
        const double r = emitter.radius;
        const V3 offset = axis * distance;
        const V3 radial = offset - along * Dot(offset, along);
        if (Length(radial) <= r) {
            return 0.0;
        }
        V3 e1, e2;
        Frame(along, &e1, &e2);
        return SurfaceIlluminance(emitter, area, distance, [&](double a, double b, V3* n) {
            const double psi = 2.0 * kPi * b;
            *n = e1 * std::cos(psi) + e2 * std::sin(psi);
            return centre + u * (2.0 * a - 1.0) + *n * r;
        });
    }
    }
    return 0.0;
}

double DistantIlluminancePerLuminance(double angularRadius)
{
    const double theta = std::clamp(angularRadius, 0.0, 0.5 * kPi);
    const double s = std::sin(theta);
    return kPi * s * s;
}

}  // namespace hdclaude
