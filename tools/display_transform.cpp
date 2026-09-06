// hdClaudeDisplayTransform -- scene-linear EXR to a display-encoded image.
//
// The renderer writes scene-linear radiance; a gallery baseline is a JPEG. This
// is the one place that crosses between them, so the decision about what
// happens above one is made once, in hdclaude::SceneLinearToDisplaySrgb, rather
// than by whatever the image library happens to do when it truncates a float.
//
// Reading and writing go through Hio, which is already a dependency of the
// delegate, so the gallery pipeline adds no image library of its own.

#include "hdclaude/core/display.h"

#include "pxr/imaging/hio/image.h"
#include "pxr/imaging/hio/types.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

int Usage()
{
    std::cerr << "Usage: hdClaudeDisplayTransform <linear-input> "
                 "<display-output> [--exposure <stops>]\n";
    return 2;
}

std::uint8_t ToByte(float unit)
{
    return static_cast<std::uint8_t>(
        std::lround(std::clamp(unit, 0.0f, 1.0f) * 255.0f));
}

}  // namespace

int main(int argc, char** argv)
try {
    if (argc != 3 && argc != 5) {
        return Usage();
    }
    float exposure = 0.0f;
    if (argc == 5) {
        if (std::string(argv[3]) != "--exposure") {
            return Usage();
        }
        exposure = std::stof(argv[4]);
    }

    // Raw, unflipped: the renderer's row 0 is the bottom of the image and the
    // EXR was written that way, so asking Hio to reorient here would flip the
    // baseline relative to every other view of the same render.
    const HioImageSharedPtr input =
        HioImage::OpenForReading(argv[1], 0, 0, HioImage::Raw, false);
    if (!input || input->GetWidth() <= 0 || input->GetHeight() <= 0) {
        std::cerr << "Could not open linear input: " << argv[1] << '\n';
        return 1;
    }

    const int width = input->GetWidth();
    const int height = input->GetHeight();
    const auto pixels =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height);

    std::vector<float> linear(pixels * 4);
    HioImage::StorageSpec source;
    source.width = width;
    source.height = height;
    source.depth = 1;
    source.format = HioFormatFloat32Vec4;
    source.flipped = false;
    source.data = linear.data();
    if (!input->Read(source)) {
        std::cerr << "Could not decode linear input: " << argv[1] << '\n';
        return 1;
    }

    std::vector<std::uint8_t> display(pixels * 4);
    for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
        const hdclaude::Vec3 encoded = hdclaude::SceneLinearToDisplaySrgb(
            {linear[pixel * 4], linear[pixel * 4 + 1], linear[pixel * 4 + 2]},
            exposure);
        display[pixel * 4 + 0] = ToByte(encoded.x);
        display[pixel * 4 + 1] = ToByte(encoded.y);
        display[pixel * 4 + 2] = ToByte(encoded.z);

        const float alpha = linear[pixel * 4 + 3];
        display[pixel * 4 + 3] = ToByte(std::isfinite(alpha) ? alpha : 1.0f);
    }

    const HioImageSharedPtr output = HioImage::OpenForWriting(argv[2]);
    if (!output) {
        std::cerr << "Could not open display output: " << argv[2] << '\n';
        return 1;
    }
    HioImage::StorageSpec destination;
    destination.width = width;
    destination.height = height;
    destination.depth = 1;
    destination.format = HioFormatUNorm8Vec4;
    destination.flipped = false;
    destination.data = display.data();
    if (!output->Write(destination)) {
        std::cerr << "Could not write display output: " << argv[2] << '\n';
        return 1;
    }
    return 0;
} catch (const std::exception& error) {
    std::cerr << "Display transform failed: " << error.what() << '\n';
    return 1;
}
