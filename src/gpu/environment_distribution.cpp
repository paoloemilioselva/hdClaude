#include "hdclaude/gpu/environment_distribution.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace hdclaude {

namespace {

constexpr float kPi = 3.14159265358979323846f;

/// IEEE 754 half to float.
///
/// Written out rather than taken from Imath: this library includes no `pxr`
/// header by design (docs/architecture.md 3), and the conversion is short
/// enough that a dependency would cost more than it saves.
float FromHalf(std::uint16_t bits)
{
    const std::uint32_t sign = static_cast<std::uint32_t>(bits & 0x8000u) << 16;
    std::uint32_t exponent = (bits >> 10) & 0x1fu;
    std::uint32_t mantissa = bits & 0x3ffu;

    std::uint32_t result = 0;
    if (exponent == 0) {
        if (mantissa != 0) {
            // Subnormal: normalise it by shifting the mantissa up until the
            // implicit bit appears, paying for each shift out of the exponent.
            exponent = 1;
            while ((mantissa & 0x400u) == 0) {
                mantissa <<= 1;
                --exponent;
            }
            mantissa &= 0x3ffu;
            result = sign | ((exponent + 112u) << 23) | (mantissa << 13);
        } else {
            result = sign;   // +/- zero
        }
    } else if (exponent == 0x1fu) {
        result = sign | 0x7f800000u | (mantissa << 13);   // inf or NaN
    } else {
        result = sign | ((exponent + 112u) << 23) | (mantissa << 13);
    }

    float value = 0.0f;
    std::memcpy(&value, &result, sizeof(value));
    return value;
}

/// The sRGB electro-optical transfer function, per channel.
float FromSrgb(float encoded)
{
    return encoded <= 0.04045f ? encoded / 12.92f
                               : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
}

/// Relative luminance of one texel of `map`, in the same linear values the
/// sampler will return.
///
/// The sRGB decode matters: an sRGB-encoded map is decoded by the hardware
/// sampler, so a distribution built from the raw bytes would be a distribution
/// over different numbers than the ones being integrated.
float Luminance(const TextureImage& map, std::uint32_t x, std::uint32_t y)
{
    const std::size_t index =
        (static_cast<std::size_t>(y) * map.width + x) * 4;
    float rgb[3] = {0.0f, 0.0f, 0.0f};

    if (map.format == TexelFormat::Rgba16Sfloat) {
        for (int c = 0; c < 3; ++c) {
            std::uint16_t bits = 0;
            std::memcpy(&bits, map.texels.data() + (index + c) * 2, sizeof(bits));
            rgb[c] = FromHalf(bits);
        }
    } else {
        for (int c = 0; c < 3; ++c) {
            const float unit = map.texels[index + c] * (1.0f / 255.0f);
            rgb[c] = map.format == TexelFormat::Rgba8Srgb ? FromSrgb(unit) : unit;
        }
    }

    const float luminance =
        0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2];
    // A map holding a NaN or a negative -- both of which real EXRs do -- must
    // not poison the CDF, which would then fail to be monotonic and make the
    // binary search return anything at all.
    return std::isfinite(luminance) ? std::max(0.0f, luminance) : 0.0f;
}

}  // namespace

EnvironmentDistribution BuildEnvironmentDistribution(const TextureImage& map)
{
    EnvironmentDistribution distribution;
    if (!map.Valid()) {
        return distribution;
    }

    const std::uint32_t width =
        std::min(map.width, kEnvironmentDistributionMaxWidth);
    const std::uint32_t height =
        std::min(map.height, kEnvironmentDistributionMaxHeight);

    // Box-average each destination texel over the source block it covers, so a
    // bright texel raises its whole block rather than being missed by a point
    // sample. That is what keeps the density nonzero wherever the map is, which
    // is the property multiple importance sampling needs from it.
    std::vector<float> weights(static_cast<std::size_t>(width) * height, 0.0f);
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::uint32_t y0 = y * map.height / height;
        const std::uint32_t y1 = std::max(y0 + 1, (y + 1) * map.height / height);

        // Row v maps to polar angle (1 - v) * pi, so its solid angle goes as
        // sin of that. Evaluated at the row's centre.
        const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(height);
        const float sinTheta = std::sin((1.0f - v) * kPi);

        for (std::uint32_t x = 0; x < width; ++x) {
            const std::uint32_t x0 = x * map.width / width;
            const std::uint32_t x1 = std::max(x0 + 1, (x + 1) * map.width / width);

            float total = 0.0f;
            std::uint32_t count = 0;
            for (std::uint32_t sy = y0; sy < y1 && sy < map.height; ++sy) {
                for (std::uint32_t sx = x0; sx < x1 && sx < map.width; ++sx) {
                    total += Luminance(map, sx, sy);
                    ++count;
                }
            }
            weights[static_cast<std::size_t>(y) * width + x] =
                count > 0 ? (total / static_cast<float>(count)) * sinTheta : 0.0f;
        }
    }

    // --- CDFs ---------------------------------------------------------------
    distribution.width = width;
    distribution.height = height;
    distribution.data.assign(distribution.Size(), 0.0f);

    const std::size_t conditional = distribution.ConditionalOffset();
    const std::size_t density = distribution.DensityOffset();

    std::vector<float> rowIntegral(height, 0.0f);
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::size_t base = conditional +
                                 static_cast<std::size_t>(y) * (width + 1);
        float running = 0.0f;
        for (std::uint32_t x = 0; x < width; ++x) {
            running += weights[static_cast<std::size_t>(y) * width + x];
            distribution.data[base + x + 1] = running;
        }
        rowIntegral[y] = running;
        if (running > 0.0f) {
            for (std::uint32_t x = 1; x <= width; ++x) {
                distribution.data[base + x] /= running;
            }
        } else {
            // An entirely black row still needs a monotonic CDF, or the search
            // that walks it is undefined. It is never selected: its marginal
            // interval has zero width.
            for (std::uint32_t x = 1; x <= width; ++x) {
                distribution.data[base + x] =
                    static_cast<float>(x) / static_cast<float>(width);
            }
        }
    }

    float total = 0.0f;
    for (std::uint32_t y = 0; y < height; ++y) {
        total += rowIntegral[y];
        distribution.data[y + 1] = total;
    }
    if (!(total > 0.0f)) {
        // A uniformly black map is not an emitter, and a distribution over it
        // has no meaning. Reported as invalid so the caller keeps sampling the
        // sphere uniformly rather than dividing by zero.
        return EnvironmentDistribution();
    }
    for (std::uint32_t y = 1; y <= height; ++y) {
        distribution.data[y] /= total;
    }

    // The density in (u, v) measure. `weights` sums to `total` over
    // `width * height` cells each of area `1 / (width * height)`, so the
    // integral of `weights` over the unit square is `total / (width * height)`
    // and dividing by that is what makes the density integrate to one.
    const float scale = static_cast<float>(width) * static_cast<float>(height) / total;
    for (std::size_t i = 0; i < weights.size(); ++i) {
        distribution.data[density + i] = weights[i] * scale;
    }

    return distribution;
}

}  // namespace hdclaude
