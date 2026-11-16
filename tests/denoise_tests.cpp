// The denoiser, against images whose converged answer is known.
//
// No GPU of hdClaude's and no OpenUSD: a flat image of two radiances, one far
// above one, with multiplicative noise on it, and the albedo and normal a
// renderer would give it. Denoised, each half must come back to its own
// radiance -- an HDR value of fifty is not one -- with far less noise than it
// went in with, and nothing negative or non-finite.

#include "test_support.h"

#include "hdclaude/denoise/denoiser.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

namespace {

struct Half {
    double mean = 0.0;
    double deviation = 0.0;
};

/// The mean and standard deviation of the red channel over a column range.
Half Measure(const std::vector<float>& image, std::uint32_t width,
             std::uint32_t height, std::uint32_t x0, std::uint32_t x1)
{
    double sum = 0.0;
    double squares = 0.0;
    std::size_t count = 0;
    // A margin, so the edge between the halves -- which a denoiser is right to
    // keep sharp and wrong to measure as noise -- is not in either.
    for (std::uint32_t y = 4; y + 4 < height; ++y) {
        for (std::uint32_t x = x0 + 4; x + 4 < x1; ++x) {
            const double value = image[(static_cast<std::size_t>(y) * width + x) * 4];
            sum += value;
            squares += value * value;
            ++count;
        }
    }
    Half half;
    half.mean = sum / double(count);
    half.deviation = std::sqrt(std::max(0.0, squares / double(count) - half.mean * half.mean));
    return half;
}

void TestDenoiseKeepsRadianceAndRemovesNoise(hdclaude::DenoiserDevice device,
                                             const char* name)
{
    hdclaude::Denoiser denoiser(HDCLAUDE_OIDN_RUNTIME_DIRECTORY, device);
    if (!denoiser.Available()) {
        // A GPU device is a property of the machine; the CPU device is not.
        std::printf("  %s: unavailable (%s)\n", name, denoiser.Unavailable().c_str());
        CHECK(device != hdclaude::DenoiserDevice::Cpu);
        return;
    }
    std::printf("  %s: %s\n", name, denoiser.Description().c_str());

    const std::uint32_t width = 128;
    const std::uint32_t height = 64;
    const float dim = 0.5f;
    const float bright = 50.0f;
    std::mt19937 random(7);
    std::uniform_real_distribution<float> noise(0.4f, 1.6f);
    std::vector<float> color(width * height * 4);
    std::vector<float> albedo(width * height * 3, 0.8f);
    std::vector<float> normal(width * height * 3, 0.0f);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * width + x;
            const float value = (x < width / 2 ? dim : bright) * noise(random);
            color[i * 4 + 0] = value;
            color[i * 4 + 1] = value;
            color[i * 4 + 2] = value;
            color[i * 4 + 3] = 1.0f;
            normal[i * 3 + 2] = 1.0f;
        }
    }

    hdclaude::DenoiseInputs inputs;
    inputs.width = width;
    inputs.height = height;
    inputs.color = color.data();
    inputs.albedo = albedo.data();
    inputs.normal = normal.data();
    std::vector<float> output;
    std::string error;
    const bool ok =
        denoiser.Denoise(inputs, hdclaude::DenoiserQuality::High, &output, &error);
    if (!ok) {
        std::printf("  %s: %s\n", name, error.c_str());
    }
    CHECK(ok);
    CHECK_EQ(output.size(), color.size());
    if (!ok || output.size() != color.size()) {
        return;
    }

    int bad = 0;
    for (const float value : output) {
        bad += std::isfinite(value) && value >= 0.0f ? 0 : 1;
    }
    CHECK_EQ(bad, 0);

    const Half dimIn = Measure(color, width, height, 0, width / 2);
    const Half brightIn = Measure(color, width, height, width / 2, width);
    const Half dimOut = Measure(output, width, height, 0, width / 2);
    const Half brightOut = Measure(output, width, height, width / 2, width);
    std::printf("  %s: dim %.4f +- %.4f -> %.4f +- %.4f, bright %.3f +- %.3f -> "
                "%.3f +- %.3f\n",
                name, dimIn.mean, dimIn.deviation, dimOut.mean, dimOut.deviation,
                brightIn.mean, brightIn.deviation, brightOut.mean,
                brightOut.deviation);
    // Each half's radiance, the bright one included: an HDR image read as LDR
    // comes back at one.
    CHECK(std::abs(dimOut.mean - dim) < 0.05 * dim);
    CHECK(std::abs(brightOut.mean - bright) < 0.05 * bright);
    // And a fraction of the noise.
    CHECK(dimOut.deviation < dimIn.deviation / 4.0);
    CHECK(brightOut.deviation < brightIn.deviation / 4.0);
    // The alpha is the image's, untouched.
    CHECK_EQ(output[3], 1.0f);
}

void TestUnavailableLibrarySaysWhy()
{
    // A directory with no OIDN in it, and an override that names one: the
    // override is reported, not silently skipped.
#if defined(_WIN32)
    _putenv_s("HDCLAUDE_OIDN_DIR", "Z:/no/such/directory");
#else
    setenv("HDCLAUDE_OIDN_DIR", "/no/such/directory", 1);
#endif
    hdclaude::Denoiser denoiser("", hdclaude::DenoiserDevice::Cpu);
#if defined(_WIN32)
    _putenv_s("HDCLAUDE_OIDN_DIR", "");
#else
    unsetenv("HDCLAUDE_OIDN_DIR");
#endif
    // Unless the process already holds OIDN, which takes precedence and is
    // then what is used.
    if (denoiser.Available()) {
        std::printf("  override: OIDN already loaded, override not consulted\n");
        return;
    }
    std::printf("  override: %s\n", denoiser.Unavailable().c_str());
    CHECK(denoiser.Unavailable().find("HDCLAUDE_OIDN_DIR") != std::string::npos);
}

}  // namespace

int main()
{
    CHECK(hdclaude::DenoiserCompiledIn());
    // First, before anything has loaded OIDN into this process.
    TestUnavailableLibrarySaysWhy();
    TestDenoiseKeepsRadianceAndRemovesNoise(hdclaude::DenoiserDevice::Cpu, "cpu");
    TestDenoiseKeepsRadianceAndRemovesNoise(hdclaude::DenoiserDevice::Default,
                                            "default");
    return hdclaude_test::Summarize("hdClaudeDenoiseTests");
}
