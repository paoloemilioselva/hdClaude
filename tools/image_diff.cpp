// hdClaudeImageDiff -- the gallery's image gate.
//
// Compares a candidate render against its committed baseline and fails on
// three separate measures, because each catches something the others miss:
//
//   RMS            a small shift everywhere -- a changed estimator, a changed
//                  tone curve -- which no single pixel would flag.
//   worst pixel    a large shift somewhere -- one object shaded wrongly --
//                  which an average over a megapixel would hide.
//   failed pixels  how much of the image moved at all, which separates
//                  "sampling noise moved" from "the picture changed".
//
// It also fails an image that is uniformly black or has no finite pixels at
// all. hdCodex gated the gallery on the renderer's exit status, and a run that
// lost the device wrote a black image and exited zero, which was accepted
// (docs/lessons-from-hdcodex.md R9). An exit status is not evidence about an
// image; this is.
//
// Exit status: 0 pass, 1 fail, 2 usage. The measurements are printed either
// way, so a failure is reported with its numbers rather than as a verdict.

#include "pxr/base/gf/half.h"
#include "pxr/imaging/hio/image.h"
#include "pxr/imaging/hio/types.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

struct Image {
    int width = 0;
    int height = 0;
    /// RGBA, one float per channel, in whatever the file's own encoding is.
    std::vector<float> pixels;
};

/// Defaults for a converged 1024-sample gallery render.
///
/// A baseline is not noise-free, so a rerun of an unchanged renderer still
/// differs slightly; these are set above that and below anything a real change
/// produces. They are arguments rather than constants because the right values
/// depend on the sample count, and a scene rendered at fewer samples for a
/// quick check should not silently be held to the converged bar.
struct Thresholds {
    float rms = 0.010f;
    float worst = 0.30f;
    float failed = 0.04f;       // fraction of pixels over `perPixel`
    float perPixel = 0.02f;     // what counts as a failed pixel
};

int Usage()
{
    std::cerr << "Usage: hdClaudeImageDiff <baseline> <candidate> "
                 "[--rms <value>] [--worst <value>] "
                 "[--failed-fraction <value>] [--per-pixel <value>]\n"
                 "       hdClaudeImageDiff --scan <image>\n"
                 "       hdClaudeImageDiff --window <image> <u0> <u1> <v0> <v1>"
                 "   (mean of a region, in fractions of the image)\n"
                 "       hdClaudeImageDiff --energy <reference> <candidate>\n"
                 "       hdClaudeImageDiff --expectation <reference> <candidate> "
                 "[<block px> <z limit>]\n";
    return 2;
}

bool Read(const char* path, Image* result)
{
    const HioImageSharedPtr source =
        HioImage::OpenForReading(path, 0, 0, HioImage::Raw, false);
    if (!source || source->GetWidth() <= 0 || source->GetHeight() <= 0) {
        std::cerr << "Could not open image: " << path << '\n';
        return false;
    }
    result->width = source->GetWidth();
    result->height = source->GetHeight();

    const HioFormat format = source->GetFormat();
    const int channels = HioGetComponentCount(format);
    const std::size_t componentSize = HioGetDataSizeOfType(format);
    if (channels <= 0 || componentSize == 0 || HioIsCompressed(format)) {
        std::cerr << "Unsupported image format: " << path << '\n';
        return false;
    }

    const auto count = static_cast<std::size_t>(result->width) * result->height;
    std::vector<std::uint8_t> raw(count * static_cast<std::size_t>(channels) *
                                  componentSize);
    HioImage::StorageSpec storage;
    storage.width = result->width;
    storage.height = result->height;
    storage.depth = 1;
    storage.format = format;
    storage.flipped = false;
    storage.data = raw.data();
    if (!source->Read(storage)) {
        std::cerr << "Could not decode image: " << path << '\n';
        return false;
    }

    // Every encoding is widened to float in its own units: a byte image is
    // compared in display units and an EXR in linear ones. Comparing a JPEG
    // against a JPEG, which is what the gallery does, keeps both sides in the
    // same space by construction.
    const HioType type = HioGetHioType(format);
    const auto component = [type](const std::uint8_t* at) {
        switch (type) {
        case HioTypeUnsignedByte:
        case HioTypeUnsignedByteSRGB:
            return static_cast<float>(*at) / 255.0f;
        case HioTypeUnsignedShort: {
            std::uint16_t value = 0;
            std::memcpy(&value, at, sizeof(value));
            return static_cast<float>(value) / 65535.0f;
        }
        case HioTypeHalfFloat: {
            GfHalf value;
            std::memcpy(&value, at, sizeof(value));
            return static_cast<float>(value);
        }
        case HioTypeFloat: {
            float value = 0.0f;
            std::memcpy(&value, at, sizeof(value));
            return value;
        }
        default:
            return 0.0f;
        }
    };

    result->pixels.assign(count * 4, 1.0f);
    for (std::size_t pixel = 0; pixel < count; ++pixel) {
        const std::uint8_t* address =
            raw.data() + pixel * static_cast<std::size_t>(channels) * componentSize;
        for (int channel = 0; channel < std::min(channels, 4); ++channel) {
            result->pixels[pixel * 4 + static_cast<std::size_t>(channel)] =
                component(address + static_cast<std::size_t>(channel) * componentSize);
        }
    }
    return true;
}

/// Absolute properties of one render, with no baseline to compare against.
///
/// These must be measured on the *linear* image, which is the actual rendered
/// data. Everything downstream of it is a lossy view: the display transform
/// sanitises non-finite values and clamps negatives on the way through, so a
/// check placed after it cannot see either. The gallery gate compares display
/// JPEGs, which is the right thing for a visual baseline and blind to this, so
/// the scan runs separately and earlier, on the EXR.
/// The mean of one rectangle of an image, named in fractions of its size.
///
/// For a test that asks *where* something is rather than what shade it is: a
/// shape's patch against the patch beside it. Fractions rather than pixels
/// because the caller knows where a thing is in the frame and should not have
/// to know the resolution the frame was rendered at.
///
/// Each channel is reported, and the green one is first, because that is the
/// channel every other measurement in this project reads.
int Window(const char* path, double u0, double u1, double v0, double v1)
{
    Image image;
    if (!Read(path, &image)) {
        return 1;
    }
    if (!(u0 < u1) || !(v0 < v1)) {
        std::cerr << "window: expected u0 < u1 and v0 < v1\n";
        return 1;
    }

    const auto clampIndex = [](double fraction, int size) {
        const auto index = static_cast<int>(fraction * size);
        return std::min(std::max(index, 0), size - 1);
    };
    const int x0 = clampIndex(u0, image.width);
    const int x1 = clampIndex(u1, image.width);
    const int y0 = clampIndex(v0, image.height);
    const int y1 = clampIndex(v1, image.height);

    double sums[3] = {0.0, 0.0, 0.0};
    std::size_t counted = 0;
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const auto pixel =
                static_cast<std::size_t>(y) * image.width + static_cast<std::size_t>(x);
            bool finite = true;
            for (int channel = 0; channel < 3; ++channel) {
                finite = finite &&
                         std::isfinite(image.pixels[pixel * 4 +
                                                    static_cast<std::size_t>(channel)]);
            }
            if (!finite) {
                continue;
            }
            for (int channel = 0; channel < 3; ++channel) {
                sums[channel] +=
                    image.pixels[pixel * 4 + static_cast<std::size_t>(channel)];
            }
            ++counted;
        }
    }
    if (counted == 0) {
        std::cerr << "window: no finite pixels in the region\n";
        return 1;
    }

    std::cout << sums[1] / static_cast<double>(counted) << ' '
              << sums[0] / static_cast<double>(counted) << ' '
              << sums[2] / static_cast<double>(counted) << ' '
              << counted << " pixels\n";
    return 0;
}

int Scan(const char* path)
{
    Image image;
    if (!Read(path, &image)) {
        return 1;
    }

    const auto count = static_cast<std::size_t>(image.width) * image.height;
    std::size_t nonFinite = 0;
    std::size_t negative = 0;
    double sum = 0.0;
    float smallest = 0.0f;
    float largest = 0.0f;
    std::size_t brightest = 0;
    std::size_t overHundred = 0;
    std::size_t overTenThousand = 0;
    std::size_t overHundredMillion = 0;
    std::vector<std::size_t> firstNonFinite;
    std::vector<std::size_t> firstNegative;

    for (std::size_t pixel = 0; pixel < count; ++pixel) {
        for (int channel = 0; channel < 3; ++channel) {
            const float value =
                image.pixels[pixel * 4 + static_cast<std::size_t>(channel)];
            if (!std::isfinite(value)) {
                ++nonFinite;
                if (firstNonFinite.size() < 6 &&
                    (firstNonFinite.empty() || firstNonFinite.back() != pixel)) {
                    firstNonFinite.push_back(pixel);
                }
                continue;
            }
            // Counted and reported, but *not* a failure. A spectral renderer
            // resolving to scene-linear sRGB produces negative components for
            // any colour outside that gamut, because the XYZ-to-sRGB matrix has
            // negative coefficients and a saturated spectrum lands outside the
            // primaries. That is the gamut being honest, not the transport
            // being wrong, and the display transform clamps it at the point
            // where clamping is meaningful.
            //
            // Worth reporting all the same: it is what turns into a NaN the
            // moment anything raises it to a fractional power, which is exactly
            // what happens if a render is written through a transfer function
            // instead of staying linear.
            if (value < 0.0f) {
                ++negative;
                if (firstNegative.size() < 6 &&
                    (firstNegative.empty() || firstNegative.back() != pixel)) {
                    firstNegative.push_back(pixel);
                }
            }
            sum += value;
            smallest = std::min(smallest, value);
            if (value > largest) {
                largest = value;
                brightest = pixel;
            }
            // How the magnitudes are distributed, not only how far they reach.
            // Three pixels at 1e25 and three thousand at 1e3 are different
            // defects, and the range alone cannot tell them apart.
            if (value > 1.0e2f) ++overHundred;
            if (value > 1.0e4f) ++overTenThousand;
            if (value > 1.0e8f) ++overHundredMillion;
        }
    }

    const double mean = sum / static_cast<double>(count * 3);
    std::cout << "  scan: mean " << mean << ", range [" << smallest << ", "
              << largest << "], " << nonFinite << " non-finite, " << negative
              << " negative\n";
    if (overHundred > 0) {
        std::cout << "        " << overHundred << " over 1e2, "
                  << overTenThousand << " over 1e4, " << overHundredMillion
                  << " over 1e8; brightest at ("
                  << brightest % static_cast<std::size_t>(image.width) << ", "
                  << brightest / static_cast<std::size_t>(image.width) << ")\n";
    }

    bool pass = true;
    const auto report = [](const char* what,
                           const std::vector<std::size_t>& pixels, int width) {
        std::cerr << "  " << what << " at";
        for (const std::size_t pixel : pixels) {
            std::cerr << " (" << pixel % static_cast<std::size_t>(width) << ", "
                      << pixel / static_cast<std::size_t>(width) << ")";
        }
        std::cerr << '\n';
    };
    if (nonFinite > 0) {
        report("non-finite", firstNonFinite, image.width);
        pass = false;
    }
    if (negative > 0) {
        report("negative", firstNegative, image.width);
    }
    if (mean <= 1.0e-6) {
        std::cerr << "  image is uniformly black\n";
        pass = false;
    }
    return pass ? 0 : 1;
}

/// Where a candidate's light went, against a reference of the same frame.
///
/// A diagnostic, not a gate, and it answers a question the gate cannot: a
/// reconstruction that keeps a third of an image's light could be losing it in
/// the highlights it clamps or across the dim frame around them, and those are
/// different defects. So the reference's pixels are banded by their own
/// luminance, a decade a band, and each band reports its share of the
/// reference's light and how much of that the candidate kept at the same
/// pixels. Rec.709 luminance of the linear values, and signed, so a gamut's
/// negative components count as they are.
int Energy(const char* referencePath, const char* candidatePath)
{
    Image reference;
    Image candidate;
    if (!Read(referencePath, &reference) || !Read(candidatePath, &candidate)) {
        return 1;
    }
    if (reference.width != candidate.width ||
        reference.height != candidate.height) {
        std::cerr << "Resolution differs\n";
        return 1;
    }
    const auto count =
        static_cast<std::size_t>(reference.width) * reference.height;
    const auto luminance = [](const Image& image, std::size_t pixel) {
        const float* p = &image.pixels[pixel * 4];
        return 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
    };

    constexpr int kBands = 6;
    const char* names[kBands] = {"below 0.001", "0.001 to 0.01", "0.01 to 0.1",
                                 "0.1 to 1",    "1 to 10",       "10 and above"};
    double referenceBand[kBands] = {};
    double candidateBand[kBands] = {};
    std::size_t pixelsBand[kBands] = {};
    double referenceTotal = 0.0;
    double candidateTotal = 0.0;
    for (std::size_t pixel = 0; pixel < count; ++pixel) {
        const double r = luminance(reference, pixel);
        const double c = luminance(candidate, pixel);
        if (!std::isfinite(r) || !std::isfinite(c)) {
            continue;
        }
        int band = 0;
        for (double edge = 0.001; band < kBands - 1 && r >= edge; edge *= 10.0) {
            ++band;
        }
        referenceBand[band] += r;
        candidateBand[band] += c;
        ++pixelsBand[band];
        referenceTotal += r;
        candidateTotal += c;
    }

    std::cout << "  energy: candidate keeps "
              << 100.0 * candidateTotal / std::max(referenceTotal, 1.0e-30)
              << "% of the reference's light\n";
    for (int band = 0; band < kBands; ++band) {
        if (pixelsBand[band] == 0) {
            continue;
        }
        std::cout << "    reference " << names[band] << ": " << pixelsBand[band]
                  << " pixels, "
                  << 100.0 * referenceBand[band] / std::max(referenceTotal, 1.0e-30)
                  << "% of its light, kept "
                  << 100.0 * candidateBand[band] /
                         std::max(std::abs(referenceBand[band]), 1.0e-30)
                  << "%\n";
    }
    return 0;
}

/// Whether two renders have the same expected image, region by region.
///
/// The gate above assumes both images drew the same random numbers for the
/// same estimator, so that an unchanged renderer differs by little per pixel.
/// Two *different* unbiased estimators of the same image -- next-event
/// estimation alone against a balance-heuristic combination, say -- agree in
/// expectation and in nothing else: their noise is independent, and a
/// per-pixel limit fails them or has to be loosened until it means nothing.
///
/// So each image is cut into square blocks and each block's mean luminance is
/// compared against the noise the two images themselves show there: the
/// difference of the means over the combined standard error, estimated from
/// the spread of each block's pixels. That spread includes the block's real
/// structure as well as its noise, which can only make a difference *harder*
/// to call significant -- so a pass needs a negative control beside it, and a
/// failure is a real one. Blocks are independent, so the limit is a z-score set
/// far past what a few hundred blocks produce by chance.
int Expectation(const char* referencePath, const char* candidatePath,
                int block, double zLimit)
{
    Image reference;
    Image candidate;
    if (!Read(referencePath, &reference) || !Read(candidatePath, &candidate)) {
        return 1;
    }
    if (reference.width != candidate.width ||
        reference.height != candidate.height) {
        std::cerr << "Resolution differs\n";
        return 1;
    }
    const auto luminance = [](const Image& image, int x, int y) {
        const float* p =
            &image.pixels[(static_cast<std::size_t>(y) * image.width + x) * 4];
        return 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
    };

    double worstZ = 0.0;
    double worstReference = 0.0;
    double worstCandidate = 0.0;
    int worstX = 0;
    int worstY = 0;
    double referenceTotal = 0.0;
    double candidateTotal = 0.0;
    int blocks = 0;
    for (int by = 0; by + block <= reference.height; by += block) {
        for (int bx = 0; bx + block <= reference.width; bx += block) {
            double sumR = 0.0, sumC = 0.0, squareR = 0.0, squareC = 0.0;
            int n = 0;
            for (int y = by; y < by + block; ++y) {
                for (int x = bx; x < bx + block; ++x) {
                    const double r = luminance(reference, x, y);
                    const double c = luminance(candidate, x, y);
                    if (!std::isfinite(r) || !std::isfinite(c)) {
                        std::cerr << "  non-finite pixel at (" << x << ", " << y
                                  << ")\n";
                        return 1;
                    }
                    sumR += r;
                    sumC += c;
                    squareR += r * r;
                    squareC += c * c;
                    ++n;
                }
            }
            const double meanR = sumR / n;
            const double meanC = sumC / n;
            const double varianceR = std::max(0.0, squareR / n - meanR * meanR);
            const double varianceC = std::max(0.0, squareC / n - meanC * meanC);
            const double standardError =
                std::sqrt((varianceR + varianceC) / (n - 1));
            const double difference = std::abs(meanC - meanR);
            // A block with no spread in either image -- empty background --
            // is compared exactly: any difference there is not noise.
            const double z = standardError > 0.0 ? difference / standardError
                              : difference > 1.0e-9 ? 1.0e30
                                                     : 0.0;
            if (z > worstZ) {
                worstZ = z;
                worstReference = meanR;
                worstCandidate = meanC;
                worstX = bx;
                worstY = by;
            }
            referenceTotal += sumR;
            candidateTotal += sumC;
            ++blocks;
        }
    }

    std::cout << "  expectation: " << blocks << " blocks of " << block << " px, "
              << "candidate mean " << candidateTotal / (blocks * block * block)
              << " against " << referenceTotal / (blocks * block * block)
              << ", worst z " << worstZ << " (limit " << zLimit << ") at ("
              << worstX << ", " << worstY << "): " << worstCandidate
              << " against " << worstReference << '\n';
    if (candidateTotal <= 1.0e-6 * blocks * block * block) {
        std::cerr << "  candidate is uniformly black\n";
        return 1;
    }
    if (worstZ > zLimit) {
        std::cerr << "  a region's mean differs by more than its noise allows\n";
        return 1;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv)
try {
    if (argc == 3 && std::string(argv[1]) == "--scan") {
        return Scan(argv[2]);
    }
    if (argc == 7 && std::string(argv[1]) == "--window") {
        return Window(argv[2], std::stod(argv[3]), std::stod(argv[4]),
                      std::stod(argv[5]), std::stod(argv[6]));
    }
    if (argc == 4 && std::string(argv[1]) == "--energy") {
        return Energy(argv[2], argv[3]);
    }
    if ((argc == 4 || argc == 6) && std::string(argv[1]) == "--expectation") {
        return Expectation(argv[2], argv[3], argc == 6 ? std::stoi(argv[4]) : 16,
                           argc == 6 ? std::stod(argv[5]) : 6.0);
    }
    if (argc < 3) {
        return Usage();
    }
    Thresholds thresholds;
    for (int argument = 3; argument < argc; argument += 2) {
        if (argument + 1 >= argc) {
            return Usage();
        }
        const std::string name = argv[argument];
        const float value = std::stof(argv[argument + 1]);
        if (name == "--rms") {
            thresholds.rms = value;
        } else if (name == "--worst") {
            thresholds.worst = value;
        } else if (name == "--failed-fraction") {
            thresholds.failed = value;
        } else if (name == "--per-pixel") {
            thresholds.perPixel = value;
        } else {
            return Usage();
        }
    }

    Image baseline;
    Image candidate;
    if (!Read(argv[1], &baseline) || !Read(argv[2], &candidate)) {
        return 1;
    }
    if (baseline.width != candidate.width ||
        baseline.height != candidate.height) {
        std::cerr << "Resolution differs: baseline " << baseline.width << "x"
                  << baseline.height << ", candidate " << candidate.width << "x"
                  << candidate.height << '\n';
        return 1;
    }

    const auto count =
        static_cast<std::size_t>(candidate.width) * candidate.height;
    double squared = 0.0;
    float worst = 0.0f;
    std::size_t failedPixels = 0;
    double candidateSum = 0.0;
    std::size_t nonFinite = 0;
    // Where the first few are, not only how many. A non-finite sample is a
    // defect to be found rather than a number to be reported, and its position
    // says which object produced it -- the one thing a count cannot.
    constexpr std::size_t kReportedPositions = 8;
    std::vector<std::size_t> nonFinitePixels;

    for (std::size_t pixel = 0; pixel < count; ++pixel) {
        float pixelWorst = 0.0f;
        for (int channel = 0; channel < 3; ++channel) {
            const float a = baseline.pixels[pixel * 4 + static_cast<std::size_t>(channel)];
            const float b = candidate.pixels[pixel * 4 + static_cast<std::size_t>(channel)];
            if (!std::isfinite(b)) {
                ++nonFinite;
                if (nonFinitePixels.size() < kReportedPositions &&
                    (nonFinitePixels.empty() || nonFinitePixels.back() != pixel)) {
                    nonFinitePixels.push_back(pixel);
                }
                continue;
            }
            candidateSum += b;
            const float difference = std::abs(a - b);
            squared += static_cast<double>(difference) * difference;
            pixelWorst = std::max(pixelWorst, difference);
        }
        worst = std::max(worst, pixelWorst);
        if (pixelWorst > thresholds.perPixel) {
            ++failedPixels;
        }
    }

    const double rms = std::sqrt(squared / static_cast<double>(count * 3));
    const double failedFraction =
        static_cast<double>(failedPixels) / static_cast<double>(count);
    const double mean = candidateSum / static_cast<double>(count * 3);

    std::cout << "  rms " << rms << " (limit " << thresholds.rms << "), worst "
              << worst << " (limit " << thresholds.worst << "), failed "
              << failedFraction * 100.0 << "% (limit "
              << thresholds.failed * 100.0 << "), mean " << mean << '\n';

    bool pass = true;
    if (nonFinite > 0) {
        std::cerr << "  " << nonFinite
                  << " non-finite samples in the candidate, first at";
        for (const std::size_t pixel : nonFinitePixels) {
            std::cerr << " (" << pixel % candidate.width << ", "
                      << pixel / candidate.width << ")";
        }
        std::cerr << '\n';
        pass = false;
    }
    // A render that produced nothing is a failure whatever the difference says,
    // and it says little when the baseline is dark too.
    if (mean <= 1.0e-6) {
        std::cerr << "  candidate is uniformly black\n";
        pass = false;
    }
    if (rms > thresholds.rms) {
        std::cerr << "  rms exceeds its limit\n";
        pass = false;
    }
    if (worst > thresholds.worst) {
        std::cerr << "  worst pixel exceeds its limit\n";
        pass = false;
    }
    if (failedFraction > thresholds.failed) {
        std::cerr << "  too many pixels differ\n";
        pass = false;
    }
    return pass ? 0 : 1;
} catch (const std::exception& error) {
    std::cerr << "Image diff failed: " << error.what() << '\n';
    return 1;
}
