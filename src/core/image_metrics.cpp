#include "hdclaude/core/image_metrics.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace hdclaude {
namespace {

/// Rec.709 luminance on linear RGB, which is the space the film resolves to.
inline double Luminance(const float* pixel)
{
    return 0.2126 * pixel[0] + 0.7152 * pixel[1] + 0.0722 * pixel[2];
}

constexpr int kWindow = 11;
constexpr double kSigma = 1.5;

/// The separable half of an 11x11 circular Gaussian, normalised so that the
/// outer product sums to one. Built once per call rather than tabulated,
/// because it costs eleven exponentials and a table would be one more thing to
/// keep honest.
std::vector<double> GaussianRow()
{
    std::vector<double> weights(kWindow);
    const int centre = kWindow / 2;
    double sum = 0.0;
    for (int i = 0; i < kWindow; ++i) {
        const double offset = i - centre;
        weights[static_cast<std::size_t>(i)] =
            std::exp(-(offset * offset) / (2.0 * kSigma * kSigma));
        sum += weights[static_cast<std::size_t>(i)];
    }
    for (double& weight : weights) {
        weight /= sum;
    }
    return weights;
}

}  // namespace

double Ssim(const float* a, const float* b, std::uint32_t width,
            std::uint32_t height, std::uint32_t stride)
{
    if (a == nullptr || b == nullptr || stride < 3) {
        return 0.0;
    }
    if (width < static_cast<std::uint32_t>(kWindow) ||
        height < static_cast<std::uint32_t>(kWindow)) {
        return 0.0;
    }

    // Luminance once, rather than three multiplies inside the window loop.
    const std::size_t pixels = static_cast<std::size_t>(width) * height;
    std::vector<double> x(pixels);
    std::vector<double> y(pixels);
    for (std::size_t i = 0; i < pixels; ++i) {
        x[i] = Luminance(a + i * stride);
        y[i] = Luminance(b + i * stride);
    }

    const std::vector<double> row = GaussianRow();
    constexpr double kRange = 1.0;
    const double c1 = (0.01 * kRange) * (0.01 * kRange);
    const double c2 = (0.03 * kRange) * (0.03 * kRange);

    const int centre = kWindow / 2;
    double total = 0.0;
    std::size_t counted = 0;

    for (std::uint32_t py = static_cast<std::uint32_t>(centre);
         py + static_cast<std::uint32_t>(centre) < height; ++py) {
        for (std::uint32_t px = static_cast<std::uint32_t>(centre);
             px + static_cast<std::uint32_t>(centre) < width; ++px) {
            double meanX = 0.0;
            double meanY = 0.0;
            double meanXX = 0.0;
            double meanYY = 0.0;
            double meanXY = 0.0;

            for (int wy = 0; wy < kWindow; ++wy) {
                const double weightY = row[static_cast<std::size_t>(wy)];
                const std::size_t base =
                    (static_cast<std::size_t>(py) + static_cast<std::size_t>(wy) -
                     static_cast<std::size_t>(centre)) *
                    width;
                for (int wx = 0; wx < kWindow; ++wx) {
                    const double weight =
                        weightY * row[static_cast<std::size_t>(wx)];
                    const std::size_t index =
                        base + static_cast<std::size_t>(px) +
                        static_cast<std::size_t>(wx) -
                        static_cast<std::size_t>(centre);
                    const double xv = x[index];
                    const double yv = y[index];
                    meanX += weight * xv;
                    meanY += weight * yv;
                    meanXX += weight * xv * xv;
                    meanYY += weight * yv * yv;
                    meanXY += weight * xv * yv;
                }
            }

            // The weighted covariance, taken about the weighted means. Negative
            // variances are impossible in exact arithmetic and reachable in
            // floating point when a window is constant, so they are clamped --
            // the clamp changes a value that is already zero to the last bit.
            const double varX = std::max(0.0, meanXX - meanX * meanX);
            const double varY = std::max(0.0, meanYY - meanY * meanY);
            const double covXY = meanXY - meanX * meanY;

            const double numerator =
                (2.0 * meanX * meanY + c1) * (2.0 * covXY + c2);
            const double denominator =
                (meanX * meanX + meanY * meanY + c1) * (varX + varY + c2);
            total += denominator > 0.0 ? numerator / denominator : 1.0;
            ++counted;
        }
    }

    return counted > 0 ? total / static_cast<double>(counted) : 0.0;
}

double TemporalInstability(const std::vector<std::vector<float>>& frames,
                           std::uint32_t width, std::uint32_t height,
                           std::uint32_t stride)
{
    if (frames.size() < 2 || stride < 3) {
        return 0.0;
    }
    const std::size_t pixels = static_cast<std::size_t>(width) * height;
    const std::size_t expected = pixels * stride;
    for (const std::vector<float>& frame : frames) {
        if (frame.size() != expected) {
            return 0.0;
        }
    }

    double change = 0.0;
    double brightness = 0.0;
    for (std::size_t f = 0; f < frames.size(); ++f) {
        for (std::size_t i = 0; i < pixels; ++i) {
            const double current = Luminance(frames[f].data() + i * stride);
            brightness += current;
            if (f > 0) {
                const double previous =
                    Luminance(frames[f - 1].data() + i * stride);
                change += std::abs(current - previous);
            }
        }
    }

    const double meanBrightness =
        brightness / static_cast<double>(pixels * frames.size());
    if (!(meanBrightness > 0.0)) {
        return 0.0;
    }
    const double meanChange =
        change / static_cast<double>(pixels * (frames.size() - 1));
    return meanChange / meanBrightness;
}

double ImageShift::Magnitude() const
{
    return std::sqrt(dx * dx + dy * dy);
}

ImageShift EstimateShift(const float* reference, const float* moved,
                         std::uint32_t width, std::uint32_t height,
                         std::uint32_t stride)
{
    ImageShift shift;
    if (reference == nullptr || moved == nullptr || stride < 3) {
        return shift;
    }
    // A margin is skipped on every side, so there has to be an interior left.
    constexpr int kMargin = 3;
    if (width < 4 * kMargin || height < 4 * kMargin) {
        return shift;
    }

    const std::size_t pixels = static_cast<std::size_t>(width) * height;
    std::vector<double> a(pixels);
    std::vector<double> b(pixels);
    for (std::size_t i = 0; i < pixels; ++i) {
        a[i] = Luminance(reference + i * stride);
        b[i] = Luminance(moved + i * stride);
    }

    const auto at = [&](const std::vector<double>& image, double x, double y) {
        // Bilinear, on coordinates the caller has already kept inside the
        // image; the floor and the clamp agree there and the clamp only guards
        // the last row and column against a fractional part of exactly zero
        // reading one sample past the end.
        const double fx = std::floor(x);
        const double fy = std::floor(y);
        const auto x0 = static_cast<int>(fx);
        const auto y0 = static_cast<int>(fy);
        const int x1 = std::min(x0 + 1, static_cast<int>(width) - 1);
        const int y1 = std::min(y0 + 1, static_cast<int>(height) - 1);
        const double tx = x - fx;
        const double ty = y - fy;
        const auto sample = [&](int px, int py) {
            return image[static_cast<std::size_t>(py) * width +
                         static_cast<std::size_t>(px)];
        };
        const double top = sample(x0, y0) * (1.0 - tx) + sample(x1, y0) * tx;
        const double bottom = sample(x0, y1) * (1.0 - tx) + sample(x1, y1) * tx;
        return top * (1.0 - ty) + bottom * ty;
    };

    // Newton on the warp: at each step the reference is resampled at the
    // displacement found so far, and the linear system is solved for what is
    // left. Eight steps is far more than the two or three a sub-pixel
    // displacement needs, and it costs nothing at the sizes this runs on.
    double dx = 0.0;
    double dy = 0.0;
    double conditioning = 0.0;
    for (int iteration = 0; iteration < 8; ++iteration) {
        double gxx = 0.0;
        double gyy = 0.0;
        double gxy = 0.0;
        double gxd = 0.0;
        double gyd = 0.0;
        double energy = 0.0;

        for (std::uint32_t y = kMargin; y + kMargin < height; ++y) {
            for (std::uint32_t x = kMargin; x + kMargin < width; ++x) {
                const double sx = x + dx;
                const double sy = y + dy;
                // A sample that the displacement has taken outside the image
                // has no counterpart to be compared against, and padding one in
                // would be inventing the very thing being measured.
                if (sx < 1.0 || sy < 1.0 || sx > width - 2.0 ||
                    sy > height - 2.0) {
                    continue;
                }
                const double gx =
                    0.5 * (at(a, sx + 1.0, sy) - at(a, sx - 1.0, sy));
                const double gy =
                    0.5 * (at(a, sx, sy + 1.0) - at(a, sx, sy - 1.0));
                const double difference =
                    b[static_cast<std::size_t>(y) * width + x] - at(a, sx, sy);

                gxx += gx * gx;
                gyy += gy * gy;
                gxy += gx * gy;
                gxd += gx * difference;
                gyd += gy * difference;
                energy += gx * gx + gy * gy;
            }
        }

        const double determinant = gxx * gyy - gxy * gxy;
        // A determinant of zero is the aperture problem: an image with gradient
        // along one direction only, or none at all, does not determine a
        // displacement and no amount of arithmetic will make it.
        if (!(std::abs(determinant) > 1e-12) || !(energy > 1e-9)) {
            return shift;
        }
        conditioning = energy;

        // The sign: `difference` is how much brighter the moved image is than
        // the reference read at the current displacement, and moving the read
        // point *along* the gradient raises what is read there, so a positive
        // projection asks for a positive step.
        const double stepX = (gyy * gxd - gxy * gyd) / determinant;
        const double stepY = (gxx * gyd - gxy * gxd) / determinant;
        dx += stepX;
        dy += stepY;

        if (std::abs(stepX) < 1e-6 && std::abs(stepY) < 1e-6) {
            break;
        }
        // A displacement larger than the image is a divergence, not an answer.
        if (!(std::abs(dx) < width * 0.5) || !(std::abs(dy) < height * 0.5)) {
            return shift;
        }
    }

    shift.dx = dx;
    shift.dy = dy;
    shift.valid = conditioning > 1e-9;
    return shift;
}

}  // namespace hdclaude
