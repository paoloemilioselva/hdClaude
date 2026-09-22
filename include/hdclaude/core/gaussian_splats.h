#pragma once

// Gaussian splats: the arithmetic between what USD authors and what the
// traversal kernel reads.
//
// `UsdVolParticleField` authors a particle as a position, a quaternion, three
// scales and an opacity, and the kernel wants none of those: it wants the map
// that takes an object-space offset from the centre into the kernel's own unit
// space, where the falloff is exp(-0.5 |x|^2) whatever the particle's shape.
// Resolving that here means a quaternion is turned into a matrix once per
// particle per publication rather than once per candidate intersection, of
// which there are millions per frame.
//
// This file also owns the schema's validation rules -- positions define the
// count, a longer array is truncated, a shorter one is discarded entirely for
// its documented default -- because they are arithmetic on flat arrays and
// belong where they can be asserted without a stage.
//
// The decisions recorded in docs/gaussian-splats.md are implemented here, and
// the ones the specification does not make are named in the comments below
// rather than left for a reader to infer.
//
// No USD here, and no Vulkan.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hdclaude {

/// The spatial basis function a particle instantiates.
///
/// `UsdVol` declares each of these as an applied API schema with no attributes
/// of its own: the schema's presence *is* the kernel, and the three differ only
/// in the shape of their support and their falloff.
enum class SplatKernel : std::uint32_t {
    /// `ParticleFieldKernelGaussianEllipsoidAPI`. Solid: a unit-sigma Gaussian
    /// of the distance from the centre, scaled and rotated into an ellipsoid.
    GaussianEllipsoid = 0,
    /// `ParticleFieldKernelGaussianSurfletAPI`. The same falloff restricted to
    /// the local XY plane, and exactly zero off it.
    GaussianSurflet = 1,
    /// `ParticleFieldKernelConstantSurfletAPI`. A hard disk in the local XY
    /// plane: opacity 1 inside radius 1 and 0 outside, with no falloff at all.
    ConstantSurflet = 2,
};

/// How far the kernel's support reaches, in units of its own sigma.
///
/// Three for either Gaussian, which is the specification's own figure -- "the
/// 3-sigma point is 3.0 and 99.7% of the splat support is within a spherical
/// region of radius 3" -- rather than a truncation chosen here. One for the
/// constant surflet, whose support the specification bounds exactly.
float SplatSupportRadius(SplatKernel kernel);

/// Whether the kernel's support is flat, which is what a surflet is.
bool SplatKernelIsFlat(SplatKernel kernel);

/// One particle, as the traversal kernel reads it.
///
/// Thirteen floats, packed with no padding: every hdClaude storage buffer is
/// `scalar` layout, so a three-float member is three floats and a stride of
/// thirteen is exact. A curve segment packs ten the same way.
struct Splat {
    /// Object-space centre.
    float center[3] = {0.0f, 0.0f, 0.0f};
    /// The authored opacity, multiplying the kernel's falloff.
    ///
    /// Carried as authored. The specification says it "should be in the range
    /// [0, 1]" and a value outside that is reported rather than corrected,
    /// because an opacity that needs a sigmoid applied to it is an asset that
    /// needs converting, not a renderer that needs a clamp.
    float opacity = 1.0f;
    /// Rows of inv(S) transpose(R): takes an object-space offset from the
    /// centre into the kernel's unit space.
    ///
    /// Row `j` is column `j` of the rotation divided by scale `j`, which is
    /// what makes the construction cheap and what makes the inverse exact
    /// rather than solved for.
    float inverseTransform[9] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f,
                                 0.0f, 0.0f, 0.0f, 1.0f};
};

/// The authored per-particle arrays, in the order and units USD declares them.
///
/// Flat rather than typed, because this is the boundary the Hydra layer flattens
/// to: it has already resolved float against half and picked the attribute the
/// schema's `UsesFloat*` helpers name, so nothing below here knows that the half
/// flavour exists.
struct SplatCloudSource {
    /// Interleaved xyz, three per particle. Its length defines how many
    /// particles the field has.
    std::vector<float> positions;
    /// Four per particle, in (w, x, y, z) order -- real part first.
    ///
    /// Spelled out because `GfQuatf` stores its imaginary part first and its
    /// real part last, so a caller that reinterpreted the array would silently
    /// rotate every particle. The Hydra layer converts component by component.
    std::vector<float> orientations;
    /// Three per particle. Linear, as the schema requires: not the log-format
    /// sometimes found in a PLY.
    std::vector<float> scales;
    /// One per particle. Linear opacity, not a sigmoid activation.
    std::vector<float> opacities;
    /// Spherical-harmonics coefficients, particle-major:
    /// `3 * (degree+1)^2` floats for each particle, in l-major m-ascending
    /// order, multiplying the normalised real SH basis.
    std::vector<float> sphericalHarmonics;
    /// The degree every particle shares. The schema's fallback is 3.
    int sphericalHarmonicsDegree = 3;
    SplatKernel kernel = SplatKernel::GaussianEllipsoid;
};

/// A splat cloud resolved for the renderer.
struct SplatCloud {
    std::vector<Splat> splats;
    /// `3 * (degree+1)^2` floats per entry of `splats`, particle-major.
    std::vector<float> sphericalHarmonics;
    int sphericalHarmonicsDegree = 0;
    SplatKernel kernel = SplatKernel::GaussianEllipsoid;

    /// Object-space bounds, the centres expanded by each particle's own kernel
    /// support. Degenerate when there are no particles.
    float boundsMin[3] = {0.0f, 0.0f, 0.0f};
    float boundsMax[3] = {0.0f, 0.0f, 0.0f};

    /// What was not honoured as authored, one sentence each, naming the
    /// attribute and what was done instead. Empty when everything was.
    std::vector<std::string> reports;

    std::size_t Count() const { return splats.size(); }
    bool Valid() const { return !splats.empty(); }
    /// How many coefficients each particle carries, from the degree.
    std::size_t CoefficientsPerParticle() const;
};

/// How many SH coefficients a degree implies: `(degree+1)^2`.
std::size_t SphericalHarmonicsCoefficientCount(int degree);

/// The coefficient that reproduces the schema's fallback radiance.
///
/// The schema says an absent or discarded coefficient array should behave as "a
/// SH coefficient corresponding to a DC signal of (0.5, 0.5, 0.5), with degree
/// 0". Since Y(0,0) is 1/(2 sqrt(pi)), that coefficient is sqrt(pi) -- and
/// deriving it rather than writing 1.7725 is what pins the basis normalisation
/// the rest of this file assumes.
float SphericalHarmonicsFallbackCoefficient();

/// Evaluate the radiance a particle emits in `direction`.
///
/// `coefficients` points at one particle's block, `3 * (degree+1)^2` floats.
/// `direction` need not be normalised. `rgb` receives the result, which is
/// **not** clamped: spherical harmonics ring, a truncated series can go
/// negative, and whether that is the asset's problem or the consumer's is a
/// decision for the caller rather than one to bury here.
void EvaluateSphericalHarmonics(const float* coefficients, int degree,
                                const float direction[3], float rgb[3]);

/// The kernel's falloff at an object-space point, without the opacity.
///
/// One for a point at the centre, exp(-0.5) at one sigma along any axis, and
/// exactly zero outside the support -- including off the plane of a surflet.
float SplatKernelResponse(const Splat& splat, SplatKernel kernel,
                          const float point[3]);

/// Where a ray comes closest to a particle's centre in its own metric, and how
/// strongly the kernel responds there.
///
/// This is the closed form the traversal kernel mirrors, and the decision
/// recorded in docs/gaussian-splats.md 2.2: the specification says where the
/// kernel is evaluated for a *projection*, and says nothing about a ray, so
/// hdClaude evaluates it at its maximum response along the ray. With
/// `A = inv(S) transpose(R) d` and `b = inv(S) transpose(R) (o - mu)`, the
/// response peaks at `t = -(A.b)/(A.A)`.
///
/// `tMin` and `tMax` bound the ray; the peak is clamped into them, so a
/// particle whose centre is behind the origin still reports the largest response
/// the ray actually reaches. Returns false when the ray misses the support
/// entirely, in which case `t` and `response` are untouched.
bool SplatRayPeak(const Splat& splat, SplatKernel kernel,
                  const float origin[3], const float direction[3], float tMin,
                  float tMax, float* t, float* response);

/// The axis-aligned box that holds a particle's whole support.
///
/// This is what the acceleration structure is partitioned over, and it is
/// derived from the kernel rather than chosen: the support is the image of a
/// ball of radius `SplatSupportRadius` under the inverse of what the splat
/// carries, so the half extent along an axis is that radius times the length of
/// the corresponding row of the forward transform. A flat kernel's support is a
/// disk, so its third column takes no part.
///
/// A singular transform -- a scale of zero on some axis -- yields the degenerate
/// box at the centre. `BuildSplatCloud` drops such a particle before it gets
/// here, so this is the answer for a caller that constructed one by hand.
void SplatBounds(const Splat& splat, SplatKernel kernel, float minimum[3],
                 float maximum[3]);

/// Resolve authored arrays into a cloud, applying the schema's own rules.
///
/// Per `ParticleFieldPositionBaseAPI`: the number of positions is the number of
/// particles; any other per-particle array is truncated if too long, and if too
/// short "the entire data set will be discarded" and the attribute's default
/// used. Both cases are reported, because an asset whose scales are one short is
/// an asset about to render at unit scale for no visible reason.
///
/// A particle whose scale is zero or negative on any axis has no support and no
/// invertible transform; it is dropped and counted in the reports rather than
/// nudged to something representable.
SplatCloud BuildSplatCloud(const SplatCloudSource& source);


// ---------------------------------------------------------------------------
// The volumetric reading
// ---------------------------------------------------------------------------
//
// Everything above treats a particle's opacity as coverage: a probability that
// it stops a ray, which is exactly the schema's own alpha-compositing model
// estimated without sorting. What follows is the other reading, in which the
// kernel is a *density* and a path travels through the cloud rather than being
// stopped by one particle of it. That is what makes a splat cloud a participant
// in transport -- attenuating over distance, scattering if a material says it
// does, and interacting with fog and glass on equal terms -- and it is the
// reading a renderer needs in order to path trace splats fully rather than to
// reproduce a rasterizer's picture of them.
//
// It costs the one thing the schema does not supply. Opacity is dimensionless
// and extinction is a reciprocal length, and no density field reproduces alpha
// compositing for all ray directions (docs/gaussian-splats.md 2.1). The
// conversion below is therefore a stated choice with a stated domain of
// exactness rather than a derivation, and it is the only place in this file
// where that is true.

/// The extinction coefficient at a particle's centre, per unit length.
///
/// Chosen so that a ray through the centre of an **isotropic** particle
/// accumulates an optical depth of exactly `-ln(1 - opacity)`, and therefore
/// arrives at exactly the authored alpha: the two readings agree exactly for
/// such a particle, whichever direction the ray came from. For an anisotropic
/// one they cannot agree in every direction at once, and this matches the
/// geometric mean of its three axes -- the radius of the sphere of the same
/// volume -- so the disagreement is bounded by the particle's axis ratio and
/// falls either side of the authored value rather than always one way.
///
/// An opacity of one is an infinite optical depth, which is the right answer
/// and not a representable one. It is bounded at `maximumOpticalDepth`, whose
/// default of 20 leaves a transmittance of 2e-9 -- opaque to any measurement an
/// image can make. That is a numerical bound on a quantity that is genuinely
/// infinite, not an approximation of the authored data.
float SplatExtinction(const Splat& splat, float maximumOpticalDepth = 20.0f);

/// A macrocell grid over a splat cloud, holding an upper bound on the density
/// in each cell and the particles that reach it.
///
/// This is what makes unbiased transport through the cloud possible at bounded
/// cost. Delta tracking samples a collision from a *majorant* and then rejects
/// in proportion to the true density, and it is unbiased only if the majorant
/// really does bound the density everywhere in the cell -- a majorant that is
/// too small produces an image that is wrong by an amount nothing in the
/// picture reveals. So the bound here is conservative by construction and
/// asserted by sampling, rather than estimated.
struct SplatMajorantGrid {
    float origin[3] = {0.0f, 0.0f, 0.0f};
    /// Width of one cell on each axis. Never zero.
    float cellSize[3] = {1.0f, 1.0f, 1.0f};
    int resolution[3] = {1, 1, 1};

    /// Upper bound on the summed density anywhere in each cell.
    std::vector<float> majorant;
    /// Start of each cell's run in `indices`, with one extra entry at the end.
    std::vector<std::uint32_t> offsets;
    /// Particles whose support reaches each cell, cell-major.
    std::vector<std::uint32_t> indices;

    std::size_t CellCount() const;
    /// The cell a point falls in, or -1 when it is outside the grid.
    int CellAt(const float point[3]) const;
    /// The bound for a point, which is zero outside the grid because no
    /// particle's support reaches there.
    float MajorantAt(const float point[3]) const;
};

/// Build the grid, aiming for about `targetCells` cells over the cloud.
///
/// The count is a target rather than a rule: the grid is sized so its cells are
/// roughly cubical, because a cell far longer on one axis than another bounds
/// badly in the long direction and costs a delta-tracking step in the short one.
///
/// Zero, the default, derives the target from the cloud: about one cell per
/// particle. That is not a tuning preference, it is what keeps the bound useful.
/// A cell's majorant sums the peak of every particle reaching it, so a cell
/// holding eight particles bounds eight times the density any single point in it
/// actually has, and delta tracking then rejects seven collisions out of eight.
/// A 262,144-particle capture at a fixed 32,768 cells held eight particles a
/// cell and a majorant of 3,200 per unit length -- a mean free path of 0.0003
/// across a cloud one unit wide, which is 3,200 trials to cross.
SplatMajorantGrid BuildSplatMajorantGrid(const SplatCloud& cloud,
                                         std::size_t targetCells = 0,
                                         float maximumOpticalDepth = 20.0f);

/// The summed extinction at a point, from the particles the grid says reach it.
///
/// Equal to the sum over every particle in the cloud, because the grid holds
/// every particle whose support reaches the cell -- which the tests assert
/// against a brute-force sum rather than assuming.
float EvaluateSplatDensity(const SplatCloud& cloud,
                           const SplatMajorantGrid& grid, const float point[3],
                           float maximumOpticalDepth = 20.0f);

}  // namespace hdclaude
