// Importance sampling an environment map.
//
// The environment is next-event estimation's largest emitter and, once its map
// keeps its real range, by far its most concentrated one: an HDRI's light comes
// almost entirely from a window or a sun that covers a fraction of a percent of
// the sphere and is hundreds of times brighter than everything around it.
// Sampling that uniformly is the textbook definition of a firefly generator --
// one shadow ray in a thousand finds the window and carries a thousand times
// the radiance it should.
//
// The answer is the standard one: a piecewise-constant distribution over the
// map's own latitude-longitude parameterisation, sampled by a marginal CDF over
// rows and a conditional CDF over each row.
//
// The one property this must have, and the reason the density is stored rather
// than recomputed, is that the *same* function has to be evaluable from a
// direction alone. The kernel that weighs a scattered ray against this strategy
// has a miss and a direction and no surface, so a density it cannot recompute
// there is not a density multiple importance sampling can use.

#ifndef HDCLAUDE_GPU_ENVIRONMENT_DISTRIBUTION_H
#define HDCLAUDE_GPU_ENVIRONMENT_DISTRIBUTION_H

#include "hdclaude/gpu/scene.h"

#include <cstdint>
#include <vector>

namespace hdclaude {

/// A piecewise-constant distribution over a latitude-longitude environment map.
///
/// One flat array so it uploads as a single buffer; the three blocks it holds
/// are found by the offsets below, which the frame constants carry to the
/// kernels.
struct EnvironmentDistribution {
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    /// `[marginal][conditional][density]`, in that order.
    ///
    /// - marginal: `height + 1` entries, a CDF over rows, ending at 1.
    /// - conditional: `height` CDFs of `width + 1` entries, each ending at 1.
    /// - density: `width * height` entries, the density in (u, v) measure,
    ///   integrating to 1 over the unit square.
    std::vector<float> data;

    std::size_t ConditionalOffset() const
    {
        return static_cast<std::size_t>(height) + 1;
    }
    std::size_t DensityOffset() const
    {
        return ConditionalOffset() +
               static_cast<std::size_t>(height) * (static_cast<std::size_t>(width) + 1);
    }
    std::size_t Size() const
    {
        return DensityOffset() +
               static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    }

    bool Valid() const
    {
        return width > 0 && height > 0 && data.size() == Size();
    }
};

/// The resolution the distribution is built at, at most.
///
/// A 4k HDRI's own resolution would cost 67 MB of CDF and density for no
/// measurable gain: the distribution's job is to put samples in the bright
/// region, not to resolve its edge, and the density only has to be nonzero
/// wherever the map is. Box-averaging down to this bound keeps that guarantee
/// -- an averaged block containing one bright texel is bright -- while bounding
/// what a 16k map can cost.
inline constexpr std::uint32_t kEnvironmentDistributionMaxWidth = 1024;
inline constexpr std::uint32_t kEnvironmentDistributionMaxHeight = 512;

/// Build the distribution for `map`, or an invalid one if it has no extent or
/// is uniformly black.
///
/// Each texel is weighted by its luminance times `sin(theta)`: a row near a
/// pole covers far less solid angle than one at the equator, and weighting by
/// luminance alone would spend most of the samples on the two points of the
/// sphere where the map is most oversampled.
EnvironmentDistribution BuildEnvironmentDistribution(const TextureImage& map);

}  // namespace hdclaude

#endif  // HDCLAUDE_GPU_ENVIRONMENT_DISTRIBUTION_H
