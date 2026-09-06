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
                 "[--failed-fraction <value>] [--per-pixel <value>]\n";
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

}  // namespace

int main(int argc, char** argv)
try {
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

    for (std::size_t pixel = 0; pixel < count; ++pixel) {
        float pixelWorst = 0.0f;
        for (int channel = 0; channel < 3; ++channel) {
            const float a = baseline.pixels[pixel * 4 + static_cast<std::size_t>(channel)];
            const float b = candidate.pixels[pixel * 4 + static_cast<std::size_t>(channel)];
            if (!std::isfinite(b)) {
                ++nonFinite;
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
                  << " non-finite samples in the candidate\n";
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
