// Image metrics: how close two renders are, and how much a sequence flickers.
//
// These exist for one job the gallery's RMS gate cannot do. RMS says how far
// apart two images are pixel by pixel, which is the right question for "did
// this render change". It is the wrong question for "is this reconstruction of
// a noisy frame a good picture of the converged one": a reconstructor moves
// every pixel a little and is meant to, and RMS cannot tell that apart from a
// picture that fell apart. Structure can (docs/dlss-integration.md 8).
//
// No Vulkan and no OpenUSD, so they are testable on any host, and the closed
// forms below are checked in the core suite rather than only exercised through
// a render.

#ifndef HDCLAUDE_CORE_IMAGE_METRICS_H
#define HDCLAUDE_CORE_IMAGE_METRICS_H

#include <cstdint>
#include <vector>

namespace hdclaude {

/// Structural similarity between two linear images, on luminance.
///
/// Wang, Bovik, Sheikh and Simoncelli (2004), in the form that paper specifies:
/// an 11x11 circular Gaussian window of sigma 1.5, the three terms combined
/// with alpha = beta = gamma = 1, and the mean of the resulting map over every
/// position where the whole window fits. Positions where it does not are left
/// out rather than padded, because padding invents image content and SSIM is
/// sensitive to exactly the local statistics that the invention would change.
///
/// **The dynamic range is taken as 1.0**, which decides the stabilisers
/// C1 = (0.01 L)^2 and C2 = (0.03 L)^2. That is the paper's choice for
/// display-referred data and this renderer's output is linear HDR, so it is a
/// convention rather than a derivation: a highlight at 10.0 is measured with
/// stabilisers scaled for 1.0 and so is judged almost entirely on its structure
/// rather than on those floors. The metric is used comparatively -- two
/// candidates against one reference, measured identically -- which is what
/// makes the convention harmless. It is stated here so that nobody reads an
/// absolute SSIM out of this function and compares it against a published
/// number measured on 8-bit images.
///
/// Luminance is Rec.709 on linear RGB, which is the primaries the film resolves
/// to. Alpha is ignored.
///
/// `stride` is the number of floats per pixel; the first three are read.
/// Returns 1.0 for two identical images, and 0.0 when the images disagree in
/// size or are smaller than the window.
double Ssim(const float* a, const float* b, std::uint32_t width,
            std::uint32_t height, std::uint32_t stride = 4);

/// How much a sequence of frames flickers, as a fraction of its own brightness.
///
/// The mean over consecutive pairs, and over pixels, of the absolute change in
/// luminance, divided by the mean luminance of the sequence. Zero for a
/// sequence of identical frames; larger the more each frame disagrees with the
/// one before it.
///
/// This is the second half of what a reconstructor has to be judged on and the
/// half a still comparison cannot see: an image can be close to the converged
/// reference on every frame and still boil, because being close in each frame
/// says nothing about being close to *the previous frame*. A reconstructor that
/// is not more stable than the noisy sequence it was given has not reconstructed
/// anything.
///
/// Normalised by brightness so that two sequences of different exposure can be
/// compared; a sequence whose mean luminance is zero returns zero, because a
/// black sequence does not flicker.
double TemporalInstability(const std::vector<std::vector<float>>& frames,
                           std::uint32_t width, std::uint32_t height,
                           std::uint32_t stride = 4);

/// How far one image has moved relative to another, in pixels.
///
/// Returns the displacement `d` for which `moved(x, y)` is `reference(x + d)`,
/// estimated by the first-order relation between a difference of two images and
/// the gradient of one of them -- the Lucas-Kanade normal equations, solved
/// over the whole image and iterated on a bilinear warp so that a displacement
/// of about a pixel is still recovered rather than only a fraction of one.
///
/// This exists because the questions a reconstruction raises are questions
/// about *position*, and neither of the metrics above can answer one. SSIM over
/// a smooth image barely moves when the whole picture shifts by a pixel: a
/// sphere lit by one light has almost no detail for a shift to disturb, and the
/// measurement comes back flat whether the picture is where it belongs or a
/// pixel away from it. A displacement in pixels is the thing actually in
/// question, so it is the thing to measure.
///
/// Luminance again, and the same reasoning: a shift moves all three channels
/// together, so there is nothing to gain by solving three times.
///
/// `valid` is false when the images disagree in size, are too small, or carry
/// too little gradient to say anything -- a flat field has moved by an amount
/// no measurement can recover, and reporting zero would be a claim rather than
/// an abstention.
struct ImageShift {
    double dx = 0.0;
    double dy = 0.0;
    bool valid = false;

    /// The displacement's length, which is usually what a test asserts on.
    double Magnitude() const;
};

ImageShift EstimateShift(const float* reference, const float* moved,
                         std::uint32_t width, std::uint32_t height,
                         std::uint32_t stride = 4);

}  // namespace hdclaude

#endif  // HDCLAUDE_CORE_IMAGE_METRICS_H
