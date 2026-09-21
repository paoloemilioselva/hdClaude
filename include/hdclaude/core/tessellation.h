// Choosing how finely a mesh is refined, from where the camera is.
//
// Uniform refinement spends the same detail everywhere: a mesh filling the
// frame and a mesh three pixels across are both refined to the same depth, so
// the level that makes the first one smooth makes the second one cost four
// times as much for nothing. That is what the level cap exists to contain, and
// containing it is not the same as fixing it.
//
// What this answers instead is, for one mesh, the level at which its *refined*
// edges are about as long as a chosen number of pixels -- so detail is bought
// where it can be seen and not where it cannot.
//
// Two things about the way it is asked matter.
//
// **The view is sampled, not followed.** A level is derived from a camera once
// and then held: published geometry is what acceleration structures are built
// over and what the accumulated film depends on, so re-deriving it whenever the
// camera moved would rebuild both on every viewport nudge and throw away the
// prototype reuse the fingerprints exist to provide. Following the camera is a
// render setting of its own, off by default, and a retessellate trigger asks
// for the recomputation explicitly.
//
// **Nothing off-screen is dropped.** A path tracer sees geometry the camera
// does not: in a reflection, through glass, as a shadow, and in every indirect
// bounce. Geometry outside the frustum is refined to a floor rather than to its
// control cage, so a mirror still reflects a rounded object.
//
// Plain arithmetic with no Vulkan and no OpenUSD in it, so the closed forms
// below are checked on any host (docs/architecture.md 3).

#ifndef HDCLAUDE_CORE_TESSELLATION_H
#define HDCLAUDE_CORE_TESSELLATION_H

#include <cstddef>
#include <cstdint>

namespace hdclaude {

/// The camera a tessellation was derived against, and the frame it fills.
///
/// Carried rather than pointed at, because it is a *sample*: the view a level
/// was chosen from is held after the camera has moved on, and a reference to a
/// camera that keeps changing would silently make the choice follow it.
struct TessellationView {
    /// Column-major camera-to-world, as `RenderCamera` carries it. The
    /// translation in the last column is the eye.
    float cameraToWorld[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    /// Column-major world-to-clip, for the frustum test. Identity means the
    /// caller supplied none, and then nothing is ever off-screen -- which is
    /// the safe answer, not a free pass.
    float worldToClip[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    /// Tangent of half the *vertical* field of view, matching `RenderCamera`.
    float tanHalfFov = 0.414f;
    /// Height of the frame in pixels, which is what turns a world length into
    /// a pixel length together with the field of view.
    std::uint32_t pixelHeight = 0;
    /// Whether `worldToClip` was actually supplied.
    bool hasClip = false;
    /// Whether this view is usable at all. A tessellation asked against an
    /// invalid view falls back to the uniform level, rather than guessing.
    bool valid = false;
};

/// What the caller is willing to spend, and what it will not go below.
struct TessellationLimits {
    /// The most any mesh may be refined. The render setting's level, which in
    /// adaptive mode is a ceiling rather than the answer.
    int maxLevel = 2;
    /// The least a mesh in the frustum may be refined. Zero is the control
    /// cage.
    int minLevel = 0;
    /// The level a mesh entirely outside the frustum is held at.
    ///
    /// Not zero, and not the control cage. Geometry the camera cannot see
    /// still reaches the film through reflection, refraction, shadow and
    /// indirect light, and a mirror reflecting a faceted object is a defect
    /// that only appears in the scenes that have mirrors -- which in this
    /// gallery is most of them.
    int offScreenLevel = 1;
    /// How long a refined edge should be, in pixels. Smaller is finer.
    float targetEdgePixels = 4.0f;
    /// The most refined faces one mesh may be given.
    ///
    /// A mesh close enough to the camera can ask for a level whose face count
    /// grows past anything the machine has, and a budget that bites is the
    /// honest form of "there is a limit": it says which mesh it applied to and
    /// what it wanted, where a hard cap on the level silently gives every mesh
    /// the same answer whether or not it needed it.
    std::size_t maxRefinedFaces = 4u * 1024u * 1024u;
};

/// One mesh, as the chooser needs to see it.
struct TessellationRequest {
    /// World-space bounds of every placement of this mesh. A prototype shared
    /// by many instances is refined once, so the bounds are the union and the
    /// level is the one its *largest* appearance earns.
    float boundsMin[3] = {0.0f, 0.0f, 0.0f};
    float boundsMax[3] = {0.0f, 0.0f, 0.0f};
    /// Mean length of a control-cage edge, in world units, at the largest
    /// scale any placement gives it.
    ///
    /// The mean rather than the longest: one long edge across an otherwise
    /// fine cage would pull the whole mesh up a level it does not need, and
    /// the level is a property of the mesh as a whole.
    float coarseEdgeLength = 0.0f;
    /// How many faces the control cage has, for the budget.
    std::size_t coarseFaceCount = 0;
    /// How many faces each level multiplies the count by: four for
    /// Catmull-Clark and Loop, and for a bilinear quad; the caller says so
    /// rather than this guessing from a scheme it cannot see.
    int facesPerLevel = 4;
};

/// What was chosen, and why.
struct TessellationChoice {
    /// The level to refine to.
    int level = 0;
    /// The level the projected size asked for, before the limits applied.
    /// Reported so a budget or a ceiling that bit can say what it cost.
    int requested = 0;
    /// Whether the bounds lie entirely outside the frustum.
    bool offScreen = false;
    /// Whether `maxRefinedFaces` is what decided the answer.
    bool budgetClamped = false;
    /// Whether `maxLevel` is what decided the answer.
    bool ceilingClamped = false;
};

/// Whether the view a tessellation is derived from should be sampled again.
///
/// The whole of the rule, in one place and answerable without a camera or a
/// stage. A view is taken when there is none yet, when a setting that decides
/// refinement changed -- which includes a retessellate having been asked for,
/// since that is one of those settings -- and otherwise only when the caller
/// has turned following the camera on.
///
/// What is deliberately *not* here is the camera. A move is not a reason:
/// published geometry is what acceleration structures are built over and what
/// the accumulated film depends on, so re-deriving on every viewport nudge
/// would rebuild both and throw away the reuse that prototype fingerprints
/// exist to provide.
bool ShouldSampleView(bool adaptive, bool haveView, bool settingsChanged,
                      bool followCamera);

/// The world-space distance from the view's eye to the nearest point of a box.
///
/// Zero when the eye is inside it, which is the right answer: a mesh the camera
/// is standing in is as close as a mesh can be.
float DistanceToBounds(const TessellationView& view, const float boundsMin[3],
                       const float boundsMax[3]);

/// Whether a world-space box lies entirely outside the view's frustum.
///
/// The six clip planes are extracted from `worldToClip` and the box is tested
/// against each. This rejects a box that is outside *one* plane, which is
/// conservative in the direction that matters: a box it calls visible is
/// refined as though it were, and only a box no plane can exclude is called
/// off-screen. Always false when the view carries no projection.
bool BoundsAreOffScreen(const TessellationView& view, const float boundsMin[3],
                        const float boundsMax[3]);

/// How many pixels one world unit covers at `distance` from the eye.
///
/// `pixelHeight / (2 * distance * tanHalfFov)`, which is the pinhole
/// projection and nothing more. Zero for a view with no pixels or no field of
/// view.
float PixelsPerWorldUnit(const TessellationView& view, float distance);

/// The level at which `request`'s refined edges are about
/// `limits.targetEdgePixels` long, subject to the limits.
///
/// Falls back to `limits.maxLevel` -- the uniform answer -- for a view that is
/// not valid or a request that says nothing about its own size, so a caller
/// that cannot describe a mesh gets the behaviour it had before rather than
/// the control cage.
TessellationChoice ChooseTessellation(const TessellationView& view,
                                      const TessellationRequest& request,
                                      const TessellationLimits& limits);

/// How many faces `coarseFaceCount` becomes at `level`, saturating rather than
/// overflowing. `facesPerLevel` is four for every scheme hdClaude refines.
std::size_t RefinedFaceCount(std::size_t coarseFaceCount, int facesPerLevel,
                             int level);

}  // namespace hdclaude

#endif  // HDCLAUDE_CORE_TESSELLATION_H
